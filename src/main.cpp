// perfect_dark - ReXGlue Recompiled Project

#include "generated/default/perfect_dark_init.h"

#include "perfect_dark_app.h"

#include <rex/chrono/clock.h>
#include <rex/diag_timing.h>

#include <dbghelp.h>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

// Developer F-key diagnostics (see PerfectDarkInputTracker::OnDraw). Players
// never need these; set pd_dev_hotkeys = true in perfect_dark.toml to enable.
REXCVAR_DEFINE_BOOL(pd_dev_hotkeys, false, "PerfectDark",
                    "Enable developer diagnostic hotkeys (F1, F2, F5, F6, F8, F10-F12)");
REXCVAR_DEFINE_BOOL(pd_frame_stats, false, "PerfectDark",
                    "Log presented frame rate and slow frames every five seconds (developer "
                    "aid for performance work)");

// Setup extras (Install-PerfectDark.ps1). Each is read by patched game code:
// cheatIsUnlocked in generated/default/perfect_dark_recomp.7.cpp, the
// multiplayer feature check, and PerfectDarkApp::TickStoryComplete.
REXCVAR_DEFINE_BOOL(pd_unlock_all_cheats, false, "PerfectDark",
                    "Every cheat is available in the Cheats menu (each still has to be switched "
                    "on)");
REXCVAR_DEFINE_BOOL(pd_story_complete, false, "PerfectDark",
                    "Mark every solo mission complete on every difficulty, unlocking the "
                    "special missions and Perfect Dark difficulty (missions keep any real best "
                    "time)");
REXCVAR_DEFINE_BOOL(pd_unlock_all_multiplayer, false, "PerfectDark",
                    "Unlock every Combat Simulator arena, weapon, character and option");

// Settings > Video > Render Scale (PerfectDarkApp::OnPreSetup, HANDOVER 4ai).
REXCVAR_DEFINE_STRING(pd_render_scale, "1", "PerfectDark",
                      "Internal rendering sharpness: 1, 2 or 3 times the Xbox's 1280 x 720; "
                      "applied when the game starts (an old \"auto\" is 1)")
    .allowed({"1", "2", "3", "auto"});

// Which one-time updates of an older release's perfect_dark.toml this file
// has had (PerfectDarkApp::MigrateSettings, HANDOVER 4ak).
REXCVAR_DEFINE_INT32(pd_settings_revision, 0, "PerfectDark",
                     "Which one-time updates of older settings this file has had (set by the "
                     "game; leave it alone)");

REXCVAR_DEFINE_DOUBLE(pd_dark_room_visibility, 0.37, "PerfectDark",
                      "How much of a room stays visible with its lights switched off: 0 is "
                      "fully black (original), 0.37 dark but visible")
    .range(0.0, 0.6);
REXCVAR_DEFINE_DOUBLE(pd_dark_room_object_visibility, 0.03, "PerfectDark",
                      "How much of their colour objects, characters and your gun keep in a room "
                      "with its lights switched off: 0.03 leaves them almost black, as the user "
                      "chose, 1 leaves them fully lit")
    .range(0.0, 0.6);

// Dark rooms, kept as two settings because they are two different things in
// the game. Objects, characters and weapons are darkened by the shade alpha
// (PdShadeAlpha, perfect_dark_recomp.13.cpp), capped at 1 - object visibility.
// Walls and floors are lit by lights_set_for_room (sub_821519C8) from the
// room's brightness, 0-255, with lit rooms far above this floor: raised to at
// least room visibility x 255.
float PdDarkRoomVisibility() {
  return float(REXCVAR_GET(pd_dark_room_visibility));
}

float PdDarkRoomObjectVisibility() {
  return float(REXCVAR_GET(pd_dark_room_object_visibility));
}

uint32_t PdRoomBrightnessFloor(uint32_t brightness) {
  const uint32_t value = brightness & 0xFF;
  const uint32_t floor = uint32_t(REXCVAR_GET(pd_dark_room_visibility) * 255.0 + 0.5);
  return value < floor ? floor : value;
}

REXCVAR_DEFINE_BOOL(pd_menu_mouse, true, "PerfectDark",
                    "Use the mouse in menus: point at a row to highlight it, left button "
                    "selects, right button goes back");

REXCVAR_DEFINE_INT32(pd_mp_update_rate, 30, "PerfectDark",
                     "Most player updates a second each PC sends in Combat Simulator. The game "
                     "shipped 15 (Co-Operative uses 30); higher is smoother")
    .range(15, 30);

// Combat Simulator's player update cap (sub_822848F8 sets the network rate
// range: lockstep modes 30..30, Combat Simulator 5..15). The network tick
// runs 30 times a second, so 30 is the most that can go out.
int64_t PdMpUpdateRate() {
  return REXCVAR_GET(pd_mp_update_rate);
}

// The game's adaptive send rate (sub_822C8E58, per connection) starts a match
// at about 16 updates a second and climbs 10% every five seconds, takes 20%
// off whenever it thinks the link is congested, and falls to its minimum of
// 5 when more than 8% of packets go missing - a minute and a half back to 30.
// Measured on two copies: the joiner skipped 220 send slots while moving in
// one short match. With the floor on, the minimum is the cap, as the game
// already does for its lockstep modes, so the other players always get the
// full rate. At a few kilobytes a second that costs nothing on any modern link.
REXCVAR_DEFINE_BOOL(pd_mp_rate_floor, true, "PerfectDark",
                    "Keep Combat Simulator sending pd_mp_update_rate updates a second, "
                    "instead of letting the game throttle to as few as 5 after packet loss "
                    "and climb back over a minute");

int64_t PdMpUpdateRateMin() {
  return REXCVAR_GET(pd_mp_rate_floor) ? REXCVAR_GET(pd_mp_update_rate) : 5;
}

// How far behind their newest update the other players' facing and aim are
// drawn. sub_822BA128 interpolates them from the last 15 updates at "now -
// half the round trip - this" (the game shipped 200; generated/default/
// perfect_dark_recomp.16.cpp). Updates come about every 33 ms, so anything
// above that still interpolates smoothly; below it the facing holds and then
// jumps with each update. Their body position is not affected: it is
// predicted ahead, not delayed. Read every frame, so it can change mid-match.
// 50 was the best of 200/100/50/0 in a real match over Radmin (2026-10-04):
// turning shows ~150 ms sooner and still interpolates smoothly. A link with
// more jitter than that (internet play) may look smoother at 100 (F9 panel).
REXCVAR_DEFINE_INT32(pd_mp_facing_delay_ms, 50, "PerfectDark",
                     "How many milliseconds behind their newest update other players' "
                     "facing and aim are drawn, on top of half the ping. The game "
                     "shipped 200; lower shows their turning sooner, and below about 35 "
                     "it turns in steps. 100 can look smoother on a jittery internet link")
    .range(0, 200);

int64_t PdMpFacingDelayMs() {
  return REXCVAR_GET(pd_mp_facing_delay_ms);
}

REXCVAR_DEFINE_DOUBLE(pd_shine, 1.15, "PerfectDark",
                      "Shine on reflective surfaces (suit, guns, signs, ships): 1.0 is the "
                      "original, 1.15 a little more")
    .range(1.0, 3.0);

bool PdExtraAllCheats() {
  return REXCVAR_GET(pd_unlock_all_cheats);
}

bool PdExtraAllMultiplayer() {
  return REXCVAR_GET(pd_unlock_all_multiplayer);
}

// CPU use (HANDOVER 4ah). Three game threads waited by spinning: on the Xbox
// 360 they shared a core with other hardware threads, on a PC each burned a
// whole core (measured: the main thread, a 10 ms timer and a worker, ~100%
// each). Called from the game code in generated/default:
//  - the GPU-completion poll (perfect_dark_recomp.19.cpp sub_82329E08), which
//    all six of the game's GPU waits loop on: yield for the first 0.5 ms of a
//    wait, then sleep in 0.5 ms high-resolution steps;
//  - Sleep(0) in the timer thread's wait for its next 10 ms tick and at the
//    end of each pass of the worker it drives (perfect_dark_recomp.13.cpp
//    sub_8226D308 and sub_8226D520): a 0.5 ms sleep instead.
REXCVAR_DEFINE_BOOL(pd_sleep_in_waits, true, "PerfectDark",
                    "Let the game's busy-waiting threads (GPU waits, a 10 ms timer, a worker) "
                    "sleep in 0.5 ms steps instead of spinning a CPU core each");

namespace {
void SleepHalfMillisecond() {
  struct Timer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
                                           0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                           TIMER_ALL_ACCESS);
    ~Timer() {
      if (handle) CloseHandle(handle);
    }
  };
  thread_local Timer timer;
  LARGE_INTEGER due;
  due.QuadPart = -5000;  // 0.5 ms, relative, in 100 ns units
  if (timer.handle && SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(timer.handle, INFINITE);
  } else {
    SwitchToThread();
  }
}
}  // namespace

// Crash report. A crash in the PC side of the game used to leave the log
// stopping mid-line with no reason given. Now an unhandled exception writes a
// [crash] line - the exception, and the module and offset it happened at - and
// a small dump (stacks and modules, no game memory) beside the logs, for a
// player to send in with the log. abort() (the runtime's own fatal errors, an
// uncaught C++ exception) gets a line too. The newest five dumps are kept.
namespace {
std::wstring CrashLogsFolder() {
  wchar_t exe[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe, n);
  return dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"logs";
}

void KeepNewestDumps(const std::wstring& dir, size_t keep) {
  std::vector<std::filesystem::path> dumps;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const auto name = entry.path().filename().wstring();
    if (name.rfind(L"crash-", 0) == 0 && entry.path().extension() == L".dmp") {
      dumps.push_back(entry.path());
    }
  }
  std::sort(dumps.begin(), dumps.end());  // names carry the date and time
  for (size_t i = 0; i + keep < dumps.size(); ++i) std::filesystem::remove(dumps[i], ec);
}

LONG WINAPI PdCrashFilter(EXCEPTION_POINTERS* info) {
  static std::atomic<bool> once{false};
  if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
  const uintptr_t address = record ? uintptr_t(record->ExceptionAddress) : 0;
  HMODULE module = nullptr;
  wchar_t module_path[MAX_PATH] = L"?";
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(address), &module);
  if (module) GetModuleFileNameW(module, module_path, MAX_PATH);
  std::wstring module_name(module_path);
  module_name = module_name.substr(module_name.find_last_of(L"\\/") + 1);
  REXLOG_CRITICAL("[crash] exception 0x{:08X} at {}+0x{:X} (thread {}){}",
                  record ? uint32_t(record->ExceptionCode) : 0u,
                  std::string(module_name.begin(), module_name.end()),
                  module ? address - uintptr_t(module) : address, GetCurrentThreadId(),
                  record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                          record->NumberParameters >= 2
                      ? fmt::format(", {} address 0x{:X}",
                                    record->ExceptionInformation[0] ? "writing" : "reading",
                                    record->ExceptionInformation[1])
                      : std::string());
  const std::wstring dir = CrashLogsFolder();
  SYSTEMTIME now;
  GetLocalTime(&now);
  wchar_t name[64];
  swprintf(name, 64, L"\\crash-%04u%02u%02u-%02u%02u%02u.dmp", now.wYear, now.wMonth, now.wDay,
           now.wHour, now.wMinute, now.wSecond);
  const std::wstring dump_path = dir + name;
  HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), file,
        MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
        info ? &exception : nullptr, nullptr, nullptr);
    CloseHandle(file);
    const std::wstring file_name(name + 1);
    REXLOG_CRITICAL("[crash] {}: logs\\{}", ok ? "crash dump written" : "crash dump failed",
                    std::string(file_name.begin(), file_name.end()));
    KeepNewestDumps(dir, 5);
  }
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}

void PdAbortHandler(int) {
  REXLOG_CRITICAL("[crash] the game stopped itself (abort); the lines above say why");
  rex::FlushLogging();
}
}  // namespace

void PdInstallCrashReport() {
  SetUnhandledExceptionFilter(PdCrashFilter);
  std::signal(SIGABRT, PdAbortHandler);
}

void PdGpuWaitBackoff() {
  if (!REXCVAR_GET(pd_sleep_in_waits)) return;
  thread_local int64_t wait_start = 0;
  thread_local int64_t last_poll = 0;
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  // Polls within one wait come microseconds apart; a gap over 2 ms means a new wait.
  if (now.QuadPart - last_poll > freq / 500) wait_start = now.QuadPart;
  if (now.QuadPart - wait_start < freq / 2000) {
    SwitchToThread();
  } else {
    SleepHalfMillisecond();
  }
  QueryPerformanceCounter(&now);
  last_poll = now.QuadPart;
}

// Game clock (HANDOVER 4aj). The port's osGetCount (perfect_dark_recomp.12.cpp
// sub_8224FB60, called once at the start of every frame) turns the time since
// the previous frame into N64 counter units, but never less than one frame
// (781250 units) - so the frame-wait loop that reads it can never spin forever.
// On the Xbox each frame starts on the vblank, so that floor never mattered. On a
// PC the main loop sometimes resumes a little early; each early start was still
// counted as a whole frame, the game clock ran ~2% fast (measured: 47.7 MHz where
// the N64 count is 46.875) and about once a second a frame advanced two steps
// (g_Vars diffframe60 = 2). Now an early call still advances one frame, but the
// difference is owed and taken off the next call. The debt is capped at one frame,
// so a game that really runs faster than the vblank (the network clock sync) is
// counted exactly as before. Network matches take their time from the network
// clock, not from here.
REXCVAR_DEFINE_BOOL(pd_smooth_game_clock, true, "PerfectDark",
                    "Keep the game's frame clock from running fast when a frame starts a "
                    "little early (removes a double step about once a second)");

bool PdSmoothGameClock() {
  return REXCVAR_GET(pd_smooth_game_clock);
}

uint32_t PdOsCountStep(uint32_t real_step) {
  constexpr int64_t kFrame = 781250;      // one 60 Hz frame of the N64 count
  constexpr int64_t kMaxStep = 3125000;   // the game's own cap: four frames
  static int64_t debt = 0;                // counted ahead of real time
  int64_t step = int64_t(real_step) - debt;
  if (step < kFrame) {
    debt = std::min<int64_t>(kFrame - step, kFrame);
    step = kFrame;
  } else {
    debt = 0;
  }
  return uint32_t(std::min(step, kMaxStep));
}

// The game's timer object (sub_822B7700 seconds, sub_822B7750 milliseconds,
// sub_822B7790 microseconds; perfect_dark_recomp.16.cpp, HANDOVER 4ap) turns
// timebase ticks into time with the Xbox 360's rate built in: ticks * 1250 /
// 62347, 49.8776 ticks a microsecond. The guest timebase here runs at 50 MHz,
// so that time ran 0.245% fast. It is the network clock a match runs on
// (sub_82287538 via sub_822F6930), so in every network match the game stepped
// 0.25% fast with a double step every 6-7 s on every PC (measured in a
// two-PC match, 2026-10-25). Converting with the real guest tick rate makes it
// exact; nothing else in the game has the constant. Every PC in a match needs
// the same setting.
REXCVAR_DEFINE_BOOL(pd_exact_timer, true, "PerfectDark",
                    "Convert the game's own timer (the network match clock) with the real "
                    "guest tick rate instead of the Xbox 360's built-in 49.875 MHz");

uint64_t PdTimerMicroseconds(uint64_t ticks) {
  const uint64_t frequency = rex::chrono::Clock::guest_tick_frequency();
  if (!REXCVAR_GET(pd_exact_timer) || frequency == 0) {
    return ticks * 1250 / 62347;  // the game's own conversion
  }
  return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}

// The Xbox LIVE Arcade logo (DataFiles\XBLA\ArcadeWide.ptc / ArcadeNormal.ptc) is
// a loading screen: sub_82251360 (perfect_dark_recomp.12.cpp) fades it in, keeps
// drawing it until the game has loaded (byte 0x82652141), and sub_822511D8 fades
// it out. Skipping its three draws leaves that loop as it is - frames are still
// presented and loading is not touched - with nothing on screen (user, 2026-10-25;
// HANDOVER 4aq).
REXCVAR_DEFINE_BOOL(pd_skip_arcade_logo, true, "PerfectDark",
                    "Show a black screen instead of the Xbox LIVE Arcade logo while the game "
                    "loads");

bool PdSkipArcadeLogo() {
  return REXCVAR_GET(pd_skip_arcade_logo);
}

// The blue "Xbox 360 Product Identification" screen is the N64 game's legal
// screen (titleTickLegal = sub_8224A3E8, perfect_dark_recomp.12.cpp): it moves
// on once the title timer passes 180 ticks of 60 Hz and the game has finished
// loading: about 3.2 s with the fade, which the user chose to keep (2026-10-25;
// HANDOVER 4aq). The setting is there to shorten it.
REXCVAR_DEFINE_INT32(pd_legal_screen_ticks, 180, "PerfectDark",
                     "How long the Product Identification screen at start-up stays up, in "
                     "60ths of a second (the game's own is 180)")
    .range(0, 600);

int32_t PdLegalScreenTicks() {
  return REXCVAR_GET(pd_legal_screen_ticks);
}

// Multiplayer music (HANDOVER 4ar). Every PC picks the Combat Simulator track
// itself: mpChooseTrack (sub_821CE2D0, perfect_dark_recomp.8.cpp) rolls random()
// over its own soundtrack settings and never the track it just played, at the
// start of a match and whenever a track runs out (the music tick, sub_82240598
// in .12.cpp). The game does not send it, so in a network match each player
// heard something different (user, 2026-10-27). Now the host's pick rules:
//  - the host publishes the track slot it chose (netplay::SetHostValue; joined
//    PCs ask for it twice a second);
//  - a joined PC that already has the host's new pick plays it instead of its
//    own; if its own pick came first, the next frame after the host's arrives
//    runs the track out (elapsed = length) and the game's own track change
//    picks again, now the host's;
//  - near the end of a track a joined PC holds the track for up to 3 s
//    while it waits for the host's next pick, instead of playing a track of its own.
// Both PCs need this build; a host without it never answers, and nothing changes.
REXCVAR_DEFINE_BOOL(pd_mp_music_sync, true, "PerfectDark",
                    "In a network match, play the host's multiplayer music track on every PC "
                    "instead of each PC picking its own");

namespace {
constexpr uint32_t kGVarsMpRunning = 0x82648BF8 + 1336;  // g_Vars normmplayerisrunning
constexpr uint32_t kMpMusicOn = 0x82642078;     // the music tick's MP track switch
constexpr uint32_t kTrackElapsed = 0x82644C38;  // 60ths of a second into the track
constexpr uint32_t kTrackLength = 0x825EA178;   // its length, 60ths
constexpr uint32_t kTrackSlot = 0x82641904;     // slot of the track playing (the last pick)
constexpr uint32_t kTrackTable = 0x825DD138;    // 6 bytes a slot: u16 music id << 9 | seconds
constexpr uint32_t kTrackSlots = 48;            // the soundtrack bitmask holds 48
constexpr auto kHostPickFresh = std::chrono::seconds(10);
constexpr auto kMostHold = std::chrono::seconds(3);

uint32_t mp_music_used_seq = 0;  // the host pick this PC last played
std::chrono::steady_clock::time_point mp_music_hold_since;

uint32_t GuestU32(const uint8_t* base, uint32_t address) {
  return _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + address));
}
void SetGuestU32(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(base + address) = _byteswap_ulong(value);
}

// A new pick of the host's, set within the last few seconds.
bool NewHostPick(rex::kernel::xam::netplay::HostValue* host) {
  if (!rex::kernel::xam::netplay::LatestHostValue(host)) return false;
  return host->seq != mp_music_used_seq && host->value < kTrackSlots &&
         std::chrono::steady_clock::now() - host->set_at < kHostPickFresh;
}
}  // namespace

void PdMpChooseTrack(PPCContext& ctx, uint8_t* base) {
  namespace netplay = rex::kernel::xam::netplay;
  sub_821CE2D0(ctx, base);
  if (!REXCVAR_GET(pd_mp_music_sync)) return;
  const uint32_t own = GuestU32(base, kTrackSlot);
  if (netplay::IsHostingSession()) {
    netplay::SetHostValue(own);
    REXLOG_INFO("[mp_music] host: track slot {} (music {})", own, ctx.r3.u32);
    return;
  }
  if (!netplay::IsJoinedSession()) return;
  netplay::HostValue host;
  if (!NewHostPick(&host)) {
    REXLOG_INFO("[mp_music] own pick, slot {}, until the host's arrives", own);
    return;
  }
  mp_music_used_seq = host.seq;
  const uint32_t slot = host.value;
  const uint32_t entry = uint32_t(_byteswap_ushort(
      *reinterpret_cast<const uint16_t*>(base + kTrackTable + slot * 6)));
  ctx.r3.u64 = entry >> 9;
  SetGuestU32(base, kTrackLength, (entry & 0x1FF) * 60);
  SetGuestU32(base, kTrackSlot, slot);
  REXLOG_INFO("[mp_music] host's pick #{}: track slot {} (music {}; own pick was {})", host.seq,
              slot, entry >> 9, own);
}

// Each frame on a joined PC (from PdNoteGameFrame).
void PdMpMusicFrame(uint8_t* base) {
  namespace netplay = rex::kernel::xam::netplay;
  if (!REXCVAR_GET(pd_mp_music_sync) || !netplay::IsJoinedSession()) return;
  netplay::HostValue host;
  const bool new_pick = NewHostPick(&host);  // also keeps the polling going in the lobby
  if (!GuestU32(base, kGVarsMpRunning) || !GuestU32(base, kMpMusicOn)) return;
  const uint32_t length = GuestU32(base, kTrackLength);
  const uint32_t elapsed = GuestU32(base, kTrackElapsed);
  const auto now = std::chrono::steady_clock::now();
  if (new_pick) {
    if (GuestU32(base, kTrackSlot) == host.value) {
      mp_music_used_seq = host.seq;  // already playing it
    } else if (int32_t(elapsed) <= int32_t(length)) {
      SetGuestU32(base, kTrackElapsed, length);  // the next music tick changes track
    }
    mp_music_hold_since = {};
    return;
  }
  // Close to the end with no new pick from the host yet: wait for it.
  if (length > 8 && int32_t(elapsed) >= int32_t(length) - 8 && int32_t(elapsed) <= int32_t(length)) {
    if (mp_music_hold_since == std::chrono::steady_clock::time_point{}) mp_music_hold_since = now;
    if (now - mp_music_hold_since < kMostHold) {
      SetGuestU32(base, kTrackElapsed, length - 8);
    }
  } else if (int32_t(elapsed) < int32_t(length) - 8) {
    mp_music_hold_since = {};
  }
}

// Game steps in the stutter log (vblank_timing_log, HANDOVER 4al). Called at the
// start of every frame, where g_Vars diffframe60 still holds the step count of
// the frame that just ended. A frame that took under 20 ms but stepped the game
// twice or more is a double step: the game jumps ahead on screen, which is what
// the 4aj clock fault looked like. Every 10 s one line also gives game time
// against real time (100% = the game runs at its own speed).
void PdNoteGameFrame(uint8_t* base) {
  PdMpMusicFrame(base);
  PerfectDarkApp::NoteGameStage(
      _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + 0x826492E0)),   // stage
      _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + 0x82648BF8 + 0x0C)));  // level frames
  if (!rex::diag::TimingLogOn()) return;
  constexpr uint32_t kGVars = 0x82648BF8;
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  static int64_t window_start = 0, last_frame = 0;
  static uint64_t frames = 0, steps = 0, doubles = 0, long_frames = 0, network_frames = 0;
  static uint32_t most = 0;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const uint32_t step = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + kGVars));
  const bool network_clock = base[kGVars + 0x731] != 0;
  if (last_frame && now.QuadPart - last_frame < freq) {
    const double ms = double(now.QuadPart - last_frame) * 1000.0 / double(freq);
    ++frames;
    steps += step;
    most = std::max(most, step);
    if (step >= 2) ++(ms < 20.0 ? doubles : long_frames);
    if (network_clock) ++network_frames;
  }
  last_frame = now.QuadPart;
  if (!window_start) window_start = now.QuadPart;
  if (now.QuadPart - window_start >= 10 * freq) {
    const double seconds = double(now.QuadPart - window_start) / double(freq);
    REXLOG_INFO("[timing] game steps: {} frames, game time {:.2f}% of real time, {} double "
                "steps (2+ steps in a frame under 20 ms), {} long frames with 2+ steps, most {} "
                "in one frame{}",
                frames, double(steps) / (seconds * 60.0) * 100.0, doubles, long_frames, most,
                network_frames ? fmt::format(", network clock for {} frames", network_frames)
                               : std::string());
    window_start = now.QuadPart;
    frames = steps = doubles = long_frames = network_frames = 0;
    most = 0;
  }
}

bool PdIdleSleep() {
  if (!REXCVAR_GET(pd_sleep_in_waits)) return false;
  SleepHalfMillisecond();
  return true;
}

// Menu rows the game skips (greyed out or hidden): called from the dialog tick in
// generated/default (perfect_dark_recomp.8.cpp sub_821C7198). See
// PerfectDarkApp::MenuItemSkipped.
bool PdMenuSkipScanWanted(uint32_t dialog) {
  return PerfectDarkApp::MenuSkipScanWanted(dialog);
}

void PdMenuSkipNote(uint32_t item, uint32_t skipped) {
  PerfectDarkApp::MenuSkipNote(item, skipped != 0);
}

// Mouse look in networked matches: called from the game's input code in
// generated/default (perfect_dark_recomp.15.cpp sub_82294F00, .13.cpp
// sub_8225D9F8, .6.cpp sub_82187900). See PerfectDarkApp::NetLookSend.
void PdNetLookSend(uint32_t input, uint32_t frame) {
  PerfectDarkApp::NetLookSend(input, frame);
}

void PdNetLookReceive(uint32_t record, uint32_t input) {
  PerfectDarkApp::NetLookReceive(record, input);
}

void PdNetLookApply(uint32_t pad) {
  PerfectDarkApp::NetLookApply(pad);
}

uint32_t PdNetLocalInput(uint32_t input) {
  return PerfectDarkApp::NetLocalInput(input);
}

REX_DEFINE_APP(perfect_dark, PerfectDarkApp::Create)
