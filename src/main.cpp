// perfect_dark - ReXGlue Recompiled Project

#include "generated/default/perfect_dark_init.h"

#include "perfect_dark_app.h"

#include <rex/chrono/clock.h>
#include <rex/diag_timing.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#include <dbghelp.h>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <thread>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

// Developer F-key diagnostics (see PerfectDarkInputTracker::OnDraw). Players
// never need these; set pd_dev_hotkeys = true in perfect_dark.toml to enable.
REXCVAR_DEFINE_BOOL(pd_dev_hotkeys, false, "PerfectDark",
                    "Enable developer diagnostic hotkeys (F1, F2, F5, F6, F8, F10-F12)");
REXCVAR_DEFINE_BOOL(pd_trace_stick_look, false, "PerfectDark",
                    "Test: log the CamSpy's turn and height every drawn frame while it is steered");
REXCVAR_DEFINE_BOOL(pd_test_camspy_mouse, false, "PerfectDark",
                    "Test: treat the test pipe's right stick Y as mouse movement on the CamSpy (height kicks)");
REXCVAR_DEFINE_BOOL(pd_keyboard_prompts, true, "PerfectDark",
                    "With no gamepad connected, show the bound key (\"[E]\") where the game's text has a "
                    "button icon");
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
// The launcher's "Auto-Aim on" extra (HANDOVER 4az). The port starts new and default
// control settings with Auto-Aim off (PdHookAutoAimOff*); this keeps the Xbox's on.
// Settings a player already has are not changed: Player Setup > Control sets those.
REXCVAR_DEFINE_BOOL(pd_default_auto_aim, false, "PerfectDark",
                    "New and default control settings start with Auto-Aim on");

// Settings > Video > Render Scale (PerfectDarkApp::OnPreSetup, HANDOVER 4ai).
REXCVAR_DEFINE_STRING(pd_render_scale, "1", "PerfectDark",
                      "Internal rendering sharpness: 1, 2 or 3 times the Xbox's 1280 x 720; "
                      "applied when the game starts (an old \"auto\" is 1)")
    .allowed({"1", "2", "3", "auto"});

// Settings > Video > Aspect Ratio (PerfectDarkApp::ApplyAspectRatioIndex, HANDOVER 4ay).
// The game still has the N64's screen-ratio switch (optionsGetScreenRatio,
// sub_821DC2E0): every view's aspect is its width over its height times 0.75,
// and times 4/3 again when the ratio is 16:9, which the Xbox version always
// returns. For 4:3 the hook answers "normal", so the 3D view gets the N64's
// 4:3 field of view, and the SDK presenter shows the 16:9 frame squeezed to a
// 4:3 box with bars at the sides (present_aspect_ratio). Both apply at once.
REXCVAR_DEFINE_STRING(pd_aspect_ratio, "16:9", "PerfectDark",
                      "Picture shape: 16:9 (the Xbox version) or 4:3 (the N64's field of view, "
                      "with bars at the sides)")
    .allowed({"16:9", "4:3"});

// The game's text lookup (sub_8226C9F0), as it returns: button icons in the
// string become the bound keys while no gamepad is connected (PerfectDarkApp::
// KeyboardTextFor).
uint32_t PdTextForDisplay(uint32_t text) {
  return PerfectDarkApp::KeyboardTextFor(text);
}

bool PdAspect43() {
  return REXCVAR_GET(pd_aspect_ratio) == "4:3";
}

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

// Camera Bob (Settings > Display Interface, pd_view_bob). The head update sub_820F0150 (the
// N64's bheadUpdate) builds the eye's offset (r1+80), look (r1+112) and up (r1+96)
// vectors from Joanna's walk animation and idle breathing, then hands them on at
// 0x820F068C (smoothed into player+968/+980/+992). Head Roll only decides whether
// the camera turns by the look and up vectors (sub_820F26D0), so with it off the eye
// still bobs. Off, the eye stays at the resting height (player+1052, its value
// standing still) looking straight ahead with no roll; the offset's x and z stay, as
// they are what moves Joanna (the walk code sub_820EF018 reads player+968/+976).
REXCVAR_DEFINE_BOOL(pd_view_bob, true, "PerfectDark",
                    "Camera bob while walking (off = the camera stays level and steady)");

void PdViewBobHold(uint8_t* base, uint32_t sp) {
  if (REXCVAR_GET(pd_view_bob)) return;
  auto load = [base](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return __builtin_bswap32(v);
  };
  auto store = [base](uint32_t a, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    v = __builtin_bswap32(v);
    std::memcpy(base + a, &v, 4);
  };
  const uint32_t player = load(0x82648BF8u + 1188);  // g_Vars.currentplayer
  if (!player) return;
  const uint32_t rest = load(player + 1052);
  float height;
  std::memcpy(&height, &rest, 4);
  store(sp + 84, height);
  store(sp + 112, 0.0f);
  store(sp + 116, 0.0f);
  store(sp + 120, 1.0f);
  store(sp + 96, 0.0f);
  store(sp + 100, 1.0f);
  store(sp + 104, 0.0f);
}

// Lighting capture for bug reports (perfect_dark_app.h, F9 or Insert): off by default.
REXCVAR_DEFINE_BOOL(pd_light_capture, false, "PerfectDark",
                    "F9 or Insert logs one frame's draws and lighting constants (for lighting bug reports)");
REXCVAR_DEFINE_STRING(pd_light_watch, "", "PerfectDark",
                      "With pd_light_capture: F9 watches writes to this hex byte pattern in the game's memory, "
                      "F9 again lists the code that made them (lighting investigation)");

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

// Controller stick dead zones (issue #23): the game ignores each stick axis
// up to 10000 of 32767, about 30%, which makes precise aiming hard. These
// replace that for controllers, in percent of the stick's travel; 30 is the
// game's own and changes nothing. Keyboard and mouse are not affected.
// Settings > Gamepad Controls (perfect_dark_app.h, "Stick dead zones").
REXCVAR_DEFINE_INT32(pd_deadzone_right, 30, "PerfectDark",
                     "Controller right stick dead zone, percent of its travel (0-30). "
                     "30 is the game's own; lower reacts to smaller movements")
    .range(0, 30);
REXCVAR_DEFINE_INT32(pd_deadzone_left, 30, "PerfectDark",
                     "Controller left stick dead zone, percent of its travel (0-30). "
                     "30 is the game's own; lower reacts to smaller movements")
    .range(0, 30);

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

bool PdDefaultAutoAim() {
  return REXCVAR_GET(pd_default_auto_aim);
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
  // A guest thread: its registers and the return addresses up its stack (each frame's
  // back chain at 0(r1), the caller's saved LR 8 bytes below the next frame), read only
  // from committed guest pages.
  if (rex::runtime::ThreadState* ts = rex::runtime::ThreadState::Get(); ts && ts->context()) {
    const PPCContext& c = *ts->context();
    uint8_t* mem = rex::system::kernel_state() && rex::system::kernel_state()->memory()
                       ? rex::system::kernel_state()->memory()->virtual_membase()
                       : nullptr;
    const auto load = [mem](uint32_t address, uint32_t& value) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!mem || !address || !VirtualQuery(mem + address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
      std::memcpy(&value, mem + address, 4);
      value = __builtin_bswap32(value);
      return true;
    };
    std::string callers;
    uint32_t frame = c.r1.u32, back = 0, lr = 0;
    for (int i = 0; i < 8 && load(frame, back) && back > frame; ++i, frame = back) {
      if (load(back - 8, lr)) callers += fmt::format(" {:08X}", lr);
    }
    REXLOG_CRITICAL("[crash] guest lr {:08X} ctr {:08X} r1 {:08X} r3 {:08X} r4 {:08X} r5 {:08X} r11 {:08X} r30 {:08X} "
                    "r31 {:08X}; callers{}",
                    uint32_t(c.lr), c.ctr.u32, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r11.u32, c.r30.u32, c.r31.u32,
                    callers);
  }
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

// Hybrid CPUs (HANDOVER 4ba). Intel 12th gen and later mix fast performance cores
// with slow efficiency cores, and Windows 10 does not tell them apart: the game's
// threads, which sleep between frames, woke on efficiency cores and missed vblanks
// (an i9-12900K on Windows 10 ran ~46 fps with the GPU 45% busy). The process is
// kept on the cores of the highest efficiency class; on a CPU with one kind of
// core this does nothing.
REXCVAR_DEFINE_BOOL(prefer_performance_cores, true, "PerfectDark",
                    "On a CPU with performance and efficiency cores, keep the game on the "
                    "performance cores");

void PdPreferPerformanceCores() {
  ULONG size = 0;
  GetSystemCpuSetInformation(nullptr, 0, &size, GetCurrentProcess(), 0);
  std::vector<uint8_t> buffer(size);
  if (size == 0 ||
      !GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                                  size, &size, GetCurrentProcess(), 0)) {
    REXLOG_INFO("[system] CPU sets not available: cores left to Windows");
    return;
  }
  std::vector<std::pair<ULONG, BYTE>> sets;  // id, efficiency class
  BYTE top = 0, bottom = 255;
  for (ULONG offset = 0; offset < size;) {
    const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
    if (entry->Size == 0) break;
    if (entry->Type == CpuSetInformation) {
      const BYTE efficiency = entry->CpuSet.EfficiencyClass;
      sets.emplace_back(entry->CpuSet.Id, efficiency);
      top = std::max(top, efficiency);
      bottom = std::min(bottom, efficiency);
    }
    offset += entry->Size;
  }
  if (sets.empty() || top == bottom) {
    REXLOG_INFO("[system] CPU cores: one kind ({} logical processors), no core preference",
                sets.size());
    return;
  }
  std::vector<ULONG> fast;
  for (const auto& [id, efficiency] : sets) {
    if (efficiency == top) fast.push_back(id);
  }
  if (!REXCVAR_GET(prefer_performance_cores)) {
    REXLOG_INFO("[system] hybrid CPU: {} of {} logical processors are performance cores; "
                "prefer_performance_cores is off, so all are used",
                fast.size(), sets.size());
    return;
  }
  if (SetProcessDefaultCpuSets(GetCurrentProcess(), fast.data(), ULONG(fast.size()))) {
    REXLOG_INFO("[system] hybrid CPU: game kept on the {} performance-core logical processors "
                "of {}",
                fast.size(), sets.size());
  } else {
    REXLOG_WARN("[system] hybrid CPU: keeping the game on the performance cores failed ({})",
                GetLastError());
  }
}

namespace {
// The game thread's time in the GPU-completion poll, for its frame split in the
// stutter log (PdNoteGameFrame). Counted per thread: six waits share the poll.
thread_local int64_t t_gpu_poll_ticks = 0;
thread_local uint64_t t_gpu_poll_cycles = 0;
thread_local uint32_t t_gpu_poll_waits = 0;
}  // namespace

void PdGpuWaitBackoff() {
  if (!REXCVAR_GET(pd_sleep_in_waits)) return;
  thread_local int64_t wait_start = 0;
  thread_local int64_t last_poll = 0;
  thread_local uint64_t last_poll_cycles = 0;
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const int64_t start = now.QuadPart;
  const bool timing = rex::diag::TimingLogOn();
  const uint64_t cycles = timing ? rex::diag::ThreadCycles() : 0;
  // Polls within one wait come microseconds apart; a gap over 2 ms means a new wait.
  if (now.QuadPart - last_poll > freq / 500) {
    wait_start = now.QuadPart;
    ++t_gpu_poll_waits;
  } else if (timing && last_poll && last_poll_cycles) {
    // The guest's own poll code between two calls is part of the wait too.
    t_gpu_poll_ticks += now.QuadPart - last_poll;
    t_gpu_poll_cycles += cycles - last_poll_cycles;
  }
  if (now.QuadPart - wait_start < freq / 2000) {
    SwitchToThread();
  } else {
    SleepHalfMillisecond();
  }
  QueryPerformanceCounter(&now);
  last_poll = now.QuadPart;
  last_poll_cycles = 0;
  if (timing) {
    last_poll_cycles = rex::diag::ThreadCycles();
    t_gpu_poll_ticks += now.QuadPart - start;
    t_gpu_poll_cycles += last_poll_cycles - cycles;
  }
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
//  - a joined PC plays the host's pick from the last 10 s instead of its own.
//    At a match's start (both PCs pick as the stage starts, the host's arriving
//    a few hundred ms later) it waits up to 1.5 s for it;
//  - with track changes during the match (the music tick's switch, kMpMusicOn,
//    set by match setup sub_821D28A0; off in the network matches tested, which
//    keep one track all match), a new host pick that arrives after a joined
//    PC's own runs its track out (elapsed = length) so the game picks again,
//    and near the end of a track a joined PC holds it for up to 3 s while it
//    waits for the host's next pick (HANDOVER 4ar, 4bj).
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

constexpr uint32_t kGVarsLevelFrames = 0x82648BF8 + 0x0C;  // g_Vars lvframenum
// At a match's start the game picks while both PCs start the stage together; the
// host's pick reaches a joined PC within a few hundred ms (it asks twice a second).
constexpr auto kStartWait = std::chrono::milliseconds(1500);
constexpr uint32_t kStartFrames = 600;  // "the start": the first 10 s of the stage

uint32_t mp_music_used_seq = 0;  // the host pick this PC last played
std::chrono::steady_clock::time_point mp_music_hold_since;

uint32_t GuestU32(const uint8_t* base, uint32_t address) {
  return _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + address));
}
void SetGuestU32(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(base + address) = _byteswap_ulong(value);
}

// The host's pick, set within the last few seconds (used before or not).
bool RecentHostPick(rex::kernel::xam::netplay::HostValue* host) {
  if (!rex::kernel::xam::netplay::LatestHostValue(host)) return false;
  return host->value < kTrackSlots && std::chrono::steady_clock::now() - host->set_at < kHostPickFresh;
}

// A recent pick of the host's that this PC has not played yet.
bool NewHostPick(rex::kernel::xam::netplay::HostValue* host) {
  return RecentHostPick(host) && host->seq != mp_music_used_seq;
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
  bool found = RecentHostPick(&host);
  if (!found && GuestU32(base, kGVarsLevelFrames) < kStartFrames) {
    // The match is starting: the host is picking now too; its pick is on the way.
    const auto until = std::chrono::steady_clock::now() + kStartWait;
    while (!found && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      found = RecentHostPick(&host);
    }
  }
  if (!found) {
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
      _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + 0x82648BF8 + 0x0C)),  // level frames
      *(base + 0x82656740),  // the player has control
      _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + 0x82648BF8 + 1228)));  // tick mode
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
  // The game thread's own split over the window (issue #1): how much of each
  // frame it spent in the GPU-completion poll, and how much it was on a CPU.
  static uint64_t window_tsc = 0, window_cycles = 0;
  if (!window_start) {
    window_start = now.QuadPart;
    window_tsc = rex::diag::TscNow();
    window_cycles = rex::diag::ThreadCycles();
    t_gpu_poll_ticks = 0;
    t_gpu_poll_cycles = 0;
    t_gpu_poll_waits = 0;
  }
  if (now.QuadPart - window_start >= 10 * freq) {
    const double seconds = double(now.QuadPart - window_start) / double(freq);
    REXLOG_INFO("[timing] game steps: {} frames, game time {:.2f}% of real time, {} double "
                "steps (2+ steps in a frame under 20 ms), {} long frames with 2+ steps, most {} "
                "in one frame{}",
                frames, double(steps) / (seconds * 60.0) * 100.0, doubles, long_frames, most,
                network_frames ? fmt::format(", network clock for {} frames", network_frames)
                               : std::string());
    const uint64_t tsc = rex::diag::TscNow();
    const uint64_t cycles = rex::diag::ThreadCycles();
    if (frames && tsc > window_tsc) {
      const double wall_ms = seconds * 1000.0;
      const double tsc_per_ms = double(tsc - window_tsc) / wall_ms;
      const double n = double(frames);
      const double poll_ms = double(t_gpu_poll_ticks) * 1000.0 / double(freq);
      const double cpu_ms = double(cycles - window_cycles) / tsc_per_ms;
      const double poll_cpu_ms = double(t_gpu_poll_cycles) / tsc_per_ms;
      REXLOG_INFO("[timing] game thread per frame: {:.2f} ms = waiting for the GPU {:.2f} ms (on a "
                  "CPU {:.2f} ms, {:.1f} waits a frame), the rest {:.2f} ms (on a CPU {:.2f} ms)",
                  wall_ms / n, poll_ms / n, poll_cpu_ms / n, double(t_gpu_poll_waits) / n,
                  (wall_ms - poll_ms) / n, (cpu_ms - poll_cpu_ms) / n);
    }
    window_tsc = tsc;
    window_cycles = cycles;
    t_gpu_poll_ticks = 0;
    t_gpu_poll_cycles = 0;
    t_gpu_poll_waits = 0;
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

// A network match's frame wait (sub_8214D6C8, the loop at 0x8214D8A8 that reads the
// match clock until the next frame is due), each time it is about to go round again.
// Under Xenos the game thread has spent the frame waiting for the GPU before it gets
// here, so the loop seldom turns; with gpu_native nothing else waits and it spun
// through most of every frame (a native host: 16.6 ms on a CPU per 16.7 ms frame,
// 2026-11-03). Yield for the first 0.5 ms of a wait, then sleep in 0.5 ms steps.
void PdNetFrameWaitBackoff() {
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
  if (now.QuadPart - last_poll > freq / 500) wait_start = now.QuadPart;  // a new wait
  if (now.QuadPart - wait_start < freq / 2000) {
    SwitchToThread();
  } else {
    SleepHalfMillisecond();
  }
  QueryPerformanceCounter(&now);
  last_poll = now.QuadPart;
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
