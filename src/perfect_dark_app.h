// perfect_dark - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once


#include <windows.h>
#include <xinput.h>

#include <dbghelp.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <random>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <imgui.h>

// xinput.h #defines XINPUT_DEVTYPE_GAMEPAD/KEYBOARD, which collide with the
// enum of the same names in rex/input/input.h. Drop the macros before
// pulling in any rex input header.
#undef XINPUT_DEVTYPE_GAMEPAD
#undef XINPUT_DEVTYPE_KEYBOARD
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/cvar.h>
#include <rex/diag_timing.h>
#include <rex/graphics/graphics_system.h>
#include <rex/kernel/xam/netplay.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window_listener.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <vector>

// Developer diagnostics: the F1/F2/F5/F6/F8/F10-F12 hotkeys, the menu memory
// probe and the look-timing log. Off by default so a player pressing F-keys
// cannot trigger memory scans or image dumps. Defined in main.cpp.
REXCVAR_DECLARE(bool, pd_dev_hotkeys);
// A crash writes a [crash] line and a small dump into logs\ (main.cpp).
void PdInstallCrashReport();
REXCVAR_DECLARE(bool, pd_story_complete);
REXCVAR_DECLARE(bool, pd_frame_stats);
REXCVAR_DECLARE(bool, pd_menu_mouse);
REXCVAR_DECLARE(int32_t, pd_mp_facing_delay_ms);
REXCVAR_DECLARE(double, pd_shine);
#include <rex/logging/macros.h>
#include <rex/rex_app.h>

#include "version.h"

#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/window_sdl.h>

class PerfectDarkApp;

// TEMP_MENU_SCAN: a self-contained, in-process guest-memory write-watch,
// triggered by F8 (arm) / F6 (disarm + report) - see PerfectDarkInputTracker.
// The problem this solves: the game's menu backdrop and UI chrome (spinning
// loading icon, row-highlight glow) animate constantly, so a before/after
// memory *diff* is swamped by irrelevant noise no matter how the window is
// narrowed (tried: full range, narrowed to the UI heap, paused mid-mission -
// see HANDOVER.md). A write-watch sidesteps this entirely: instead of asking
// "what changed", it asks "what code writes to *this exact address*",
// which is unaffected by how much unrelated memory is also changing.
//
// Implementation: this is a software watchpoint built the same way
// debuggers build one without hardware breakpoint registers - mark the
// containing page PAGE_READONLY (reads still succeed silently; only writes
// fault), catch the resulting SEH access violation via a vectored exception
// handler, resolve the faulting instruction's address to a symbol with
// DbgHelp (dbghelp.lib is already linked; ReXGlue names recompiled guest
// functions "sub_<guest address>", so the resolved symbol *is* the guest
// PowerPC function responsible - no manual disassembly needed), then
// single-step the one faulting instruction (page temporarily restored
// read-write, trap flag set) and re-arm PAGE_READONLY on the subsequent
// single-step trap. Repeats for every write for as long as it's armed.
namespace pd_watch {

struct WatchState {
  std::mutex mutex;
  std::unordered_map<uintptr_t, DWORD> old_protect_by_page;
  std::unordered_map<DWORD, uintptr_t> single_step_page_by_thread;
  std::vector<uint32_t> watched_guest_addrs;
  uint8_t* virtual_membase = nullptr;
  bool veh_installed = false;
  PVOID veh_handle = nullptr;
  bool sym_initialized = false;
  int hits_logged = 0;
};

inline WatchState& State() {
  static WatchState s;
  return s;
}

inline LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* info) {
  auto* rec = info->ExceptionRecord;
  auto& st = State();
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    uintptr_t fault_addr = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
    bool is_write = rec->ExceptionInformation[0] == 1;
    uintptr_t page = fault_addr & ~static_cast<uintptr_t>(0xFFF);

    std::lock_guard<std::mutex> lock(st.mutex);
    auto it = st.old_protect_by_page.find(page);
    if (it == st.old_protect_by_page.end()) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    if (is_write) {
      uint32_t guest_addr =
          static_cast<uint32_t>(fault_addr - reinterpret_cast<uintptr_t>(st.virtual_membase));
      bool exact = std::find(st.watched_guest_addrs.begin(), st.watched_guest_addrs.end(),
                             guest_addr) != st.watched_guest_addrs.end();
      DWORD64 rip = info->ContextRecord->Rip;
      char sym_buf[sizeof(SYMBOL_INFO) + 256] = {};
      PSYMBOL_INFO sym = reinterpret_cast<PSYMBOL_INFO>(sym_buf);
      sym->SizeOfStruct = sizeof(SYMBOL_INFO);
      sym->MaxNameLen = 256;
      DWORD64 disp = 0;
      IMAGEHLP_LINE64 line = {};
      line.SizeOfStruct = sizeof(line);
      DWORD line_disp = 0;
      bool have_line = SymGetLineFromAddr64(GetCurrentProcess(), rip, &line_disp, &line);
      if (SymFromAddr(GetCurrentProcess(), rip, &disp, sym)) {
        if (have_line) {
          REXLOG_INFO(
              "[menu_watch] {}write to guest 0x{:08X} from {}+0x{:X} at {}:{} (host rip=0x{:X})",
              exact ? "*** EXACT MATCH *** " : "", guest_addr, sym->Name,
              static_cast<unsigned long long>(disp), line.FileName, line.LineNumber,
              static_cast<unsigned long long>(rip));
        } else {
          REXLOG_INFO("[menu_watch] {}write to guest 0x{:08X} from {}+0x{:X} (host rip=0x{:X})",
                      exact ? "*** EXACT MATCH *** " : "", guest_addr, sym->Name,
                      static_cast<unsigned long long>(disp), static_cast<unsigned long long>(rip));
        }
      } else {
        REXLOG_INFO("[menu_watch] {}write to guest 0x{:08X} from host rip=0x{:X} (no symbol)",
                    exact ? "*** EXACT MATCH *** " : "", guest_addr,
                    static_cast<unsigned long long>(rip));
      }
      st.hits_logged++;
    }

    DWORD old_prot = 0;
    VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READWRITE, &old_prot);
    info->ContextRecord->EFlags |= 0x100;  // Trap flag: single-step the next instruction.
    st.single_step_page_by_thread[GetCurrentThreadId()] = page;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP) {
    std::lock_guard<std::mutex> lock(st.mutex);
    auto it = st.single_step_page_by_thread.find(GetCurrentThreadId());
    if (it == st.single_step_page_by_thread.end()) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t page = it->second;
    st.single_step_page_by_thread.erase(it);
    DWORD old_prot = 0;
    VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READONLY, &old_prot);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

inline void ArmMemoryWriteWatch(rex::memory::Memory* mem, const std::vector<uint32_t>& guest_addrs) {
  auto& st = State();
  std::lock_guard<std::mutex> lock(st.mutex);
  st.virtual_membase = mem->virtual_membase();
  st.watched_guest_addrs = guest_addrs;
  st.hits_logged = 0;

  if (!st.sym_initialized) {
    SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    st.sym_initialized = true;
  }
  if (!st.veh_installed) {
    st.veh_handle = AddVectoredExceptionHandler(1, VectoredHandler);
    st.veh_installed = true;
  }

  std::unordered_map<uintptr_t, bool> pages_to_protect;
  for (uint32_t addr : guest_addrs) {
    uint8_t* host = mem->TranslateVirtual<uint8_t*>(addr);
    uintptr_t page = reinterpret_cast<uintptr_t>(host) & ~static_cast<uintptr_t>(0xFFF);
    pages_to_protect[page] = true;
  }
  int newly_watched = 0;
  for (const auto& kv : pages_to_protect) {
    uintptr_t page = kv.first;
    if (st.old_protect_by_page.count(page)) continue;
    DWORD old_prot = 0;
    if (VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READONLY, &old_prot)) {
      st.old_protect_by_page[page] = old_prot;
      newly_watched++;
    }
  }
  REXLOG_INFO("[menu_watch] Watching {} page(s) covering {} addresses ({} newly armed).",
              pages_to_protect.size(), guest_addrs.size(), newly_watched);
}

// Watches a raw guest address *range* rather than a discrete address list -
// used to watch the game's static image (.data/.bss, where real persistent
// state like "current screen split value" almost certainly lives) rather
// than the transient per-frame UI rendering scratch heap, which turned out
// to be the wrong target (see the "packed color/attribute word" finding
// documented in HANDOVER.md - that heap is reused for whatever the current
// draw call needs, unrelated to any specific menu item). Safe to mark the
// whole image PAGE_READONLY: ReXGlue statically recompiles guest PowerPC
// into host x64 ahead of time, so guest memory (code included) is only ever
// *read* by REX_LOAD operations, never CPU-fetched as instructions - the
// actual executable code lives entirely in this host process's own code
// section, a completely separate address range.
inline void ArmMemoryWriteWatchRange(rex::memory::Memory* mem, uint32_t start, uint32_t end) {
  auto& st = State();
  std::lock_guard<std::mutex> lock(st.mutex);
  st.virtual_membase = mem->virtual_membase();
  st.watched_guest_addrs.clear();
  st.hits_logged = 0;

  if (!st.sym_initialized) {
    SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    st.sym_initialized = true;
  }
  if (!st.veh_installed) {
    st.veh_handle = AddVectoredExceptionHandler(1, VectoredHandler);
    st.veh_installed = true;
  }

  int newly_watched = 0;
  int total_pages = 0;
  for (uint32_t addr = start & ~0xFFFu; addr < end; addr += 0x1000) {
    uint8_t* host = mem->TranslateVirtual<uint8_t*>(addr);
    uintptr_t page = reinterpret_cast<uintptr_t>(host) & ~static_cast<uintptr_t>(0xFFF);
    total_pages++;
    if (st.old_protect_by_page.count(page)) continue;
    DWORD old_prot = 0;
    if (VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READONLY, &old_prot)) {
      st.old_protect_by_page[page] = old_prot;
      newly_watched++;
    }
  }
  REXLOG_INFO("[menu_watch] Watching range 0x{:08X}-0x{:08X}: {} page(s) ({} newly armed).", start,
              end, total_pages, newly_watched);
}

inline void DisarmMemoryWriteWatch() {
  auto& st = State();
  std::lock_guard<std::mutex> lock(st.mutex);
  size_t page_count = st.old_protect_by_page.size();
  for (const auto& kv : st.old_protect_by_page) {
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(kv.first), 4096, kv.second, &unused);
  }
  st.old_protect_by_page.clear();
  st.single_step_page_by_thread.clear();
  REXLOG_INFO("[menu_watch] Disarmed ({} page(s) unprotected). {} write hit(s) were logged above.",
              page_count, st.hits_logged);
}

}  // namespace pd_watch

// Drawing palette for the PC settings panel, matching the recompiled game's
// own "Display Options" screen (dark navy card, cyan headings/accent line,
// red/cyan toggle squares) rather than inventing a new look.
namespace pd_ui {

constexpr ImU32 kCardTop = IM_COL32(0x10, 0x18, 0x26, 255);
constexpr ImU32 kCardBottom = IM_COL32(0x1C, 0x2E, 0x40, 255);
constexpr ImU32 kCardBorder = IM_COL32(0x3F, 0xD0, 0xE8, 255);
constexpr ImU32 kAccentCyan = IM_COL32(0x3F, 0xD0, 0xE8, 255);
constexpr ImU32 kRowIdle = IM_COL32(0x14, 0x1E, 0x2E, 190);
constexpr ImU32 kRowIdleBorder = IM_COL32(0x2E, 0x6F, 0x8A, 160);
constexpr ImU32 kRowSelectedTop = IM_COL32(0x1E, 0x6E, 0x8A, 255);
constexpr ImU32 kRowSelectedBottom = IM_COL32(0x10, 0x40, 0x50, 255);
constexpr ImU32 kRowSelectedBorder = IM_COL32(0x3F, 0xD0, 0xE8, 255);
constexpr ImU32 kTextNormal = IM_COL32(0xE6, 0xEA, 0xF7, 255);
constexpr ImU32 kTextDim = IM_COL32(0x8E, 0x9A, 0xB0, 255);
constexpr ImU32 kTextSelected = IM_COL32(0xFF, 0xFF, 0xFF, 255);
constexpr ImU32 kChevron = IM_COL32(0x3F, 0xD0, 0xE8, 255);
constexpr ImU32 kChevronDim = IM_COL32(0x50, 0x60, 0x74, 255);

inline void DrawChevron(ImDrawList* draw_list, ImVec2 center, float size, bool pointing_right,
                        ImU32 color) {
  ImVec2 a, b, c;
  if (pointing_right) {
    a = ImVec2(center.x - size * 0.5f, center.y - size);
    b = ImVec2(center.x - size * 0.5f, center.y + size);
    c = ImVec2(center.x + size * 0.6f, center.y);
  } else {
    a = ImVec2(center.x + size * 0.5f, center.y - size);
    b = ImVec2(center.x + size * 0.5f, center.y + size);
    c = ImVec2(center.x - size * 0.6f, center.y);
  }
  draw_list->AddTriangleFilled(a, b, c, color);
}

}  // namespace pd_ui

// Typing a player name. The guest menus have no text entry of their own (the
// console asked the dashboard for its keyboard), so Settings > Online >
// Player Name opens this instead: a small box in the same style as the F9
// panel that takes typed characters straight from ImGui's input queue.
//
// While it is open mnk_mode is off, the same trick key rebinding uses, so the
// menu underneath does not also act on what is being typed.
class PdNameEntryDialog : public rex::ui::ImGuiDialog {
 public:
  // kind 0: player name. kind 1: Direct Connect host address.
  PdNameEntryDialog(rex::ui::ImGuiDrawer* imgui_drawer, PerfectDarkApp* app, std::string initial,
                    int kind = 0)
      : rex::ui::ImGuiDialog(imgui_drawer), app_(app), text_(std::move(initial)), kind_(kind) {}

  void OnDraw(ImGuiIO& io) override;

 protected:
  void OnClose() override;

 private:
  static constexpr size_t kMaxName = 15;     // the lobby's name column
  static constexpr size_t kMaxAddress = 63;  // an IPv4 address or a hostname

  PerfectDarkApp* app_;
  std::string text_;
  int kind_ = 0;
  bool has_opened_shown_ = false;
  bool saved_ = false;
  bool input_primed_ = false;
  bool key_was_down_[3] = {};  // Enter, Escape, Backspace
  int64_t repeat_at_ms_ = 0;
  int64_t opened_at_ms_ = 0;
};

// Native-feeling PC display settings panel. There is no in-game menu item
// that opens this - Perfect Dark's own "Display Options" screen (Help &
// Options > Settings > Display) only exposes HUD toggles (crosshair,
// ammo/gun-function display, subtitles, mission timer); the Xbox 360 never
// had a per-game resolution setting since output resolution was a dashboard-
// level concern, not something any title's own menu controlled. Opened with
// F9 instead (see PerfectDarkInputTracker), styled to match that same
// Display Options screen's dark navy/cyan look rather than looking like an
// unrelated debug overlay.
class PerfectDarkSettingsDialog : public rex::ui::ImGuiDialog {
 public:
  explicit PerfectDarkSettingsDialog(rex::ui::ImGuiDrawer* imgui_drawer, PerfectDarkApp* app)
      : rex::ui::ImGuiDialog(imgui_drawer), app_(app) {}

  void OnDraw(ImGuiIO& io) override;

 protected:
  void OnClose() override;

 private:
  enum Row : int {
    kRowFullscreen = 0,
    kRowResolution,
    kRowShowFps,
    kRowFacingDelay,
    kRowDone,
    kRowCount,
  };

  struct InputEdges {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool confirm = false;
    bool back = false;
  };

  InputEdges PollInput();
  void MoveSelection(int delta);
  void CycleRow(int row, int delta);
  void ActivateRow(int row);
  void DrawRow(ImDrawList* draw_list, ImVec2 pos, ImVec2 size, int row, bool selected,
              const char* label, const char* value, bool has_value);

  PerfectDarkApp* app_;
  bool has_opened_shown_ = false;
  int selected_row_ = kRowFullscreen;

  bool key_was_down_[6] = {};
  WORD pad_buttons_prev_ = 0;
  bool pad_was_connected_ = false;
  // The first PollInput() call only seeds key_was_down_/pad_buttons_prev_ and
  // reports no edges - the key that opened this dialog (F9) is often still
  // physically held on that first polled frame, and without priming it would
  // read as a fresh edge here too, immediately cycling the first row.
  bool input_primed_ = false;
};

// Join Online Match heard more than one hosted match: a modal list, styled
// like the settings panel, to choose whose match to join (or cancel and stay
// in this PC's own lobby). The modal popup keeps the game from seeing the
// keys; the choice is acted on only once the confirm/back key is released,
// so the game never sees it held when the list closes.
class PerfectDarkMatchPicker : public rex::ui::ImGuiDialog {
 public:
  PerfectDarkMatchPicker(rex::ui::ImGuiDrawer* imgui_drawer, PerfectDarkApp* app);

  void OnDraw(ImGuiIO& io) override;

 protected:
  void OnClose() override;

 private:
  enum Key : int { kUp, kDown, kConfirm, kBack, kKeyCount };
  void PollInput(bool edges[kKeyCount], bool* any_held);

  PerfectDarkApp* app_;
  std::vector<rex::kernel::xam::netplay::FoundMatch> matches_;
  int selected_ = 0;
  bool opened_ = false;
  bool primed_ = false;
  bool was_down_[kKeyCount] = {};
  // -1 none yet; otherwise the row to act on once every key is released.
  int chosen_ = -1;
};

// Persistent, invisible background dialog whose only purpose is a per-frame
// hook: ImGuiDrawer::Draw() only calls OnDraw() for dialogs that exist in its
// list (it early-returns entirely when the list is empty), so without this
// there would be no way to poll for the F9 press that opens the settings
// panel above. Draws nothing, never closes itself. Same technique as
// hexic-hd-recomp's HexicScoreTracker.
class PerfectDarkInputTracker : public rex::ui::ImGuiDialog {
 public:
  explicit PerfectDarkInputTracker(rex::ui::ImGuiDrawer* imgui_drawer, PerfectDarkApp* app)
      : rex::ui::ImGuiDialog(imgui_drawer), app_(app) {}

  void OnDraw(ImGuiIO& io) override;

  // Draws nothing of its own, so it never asks for extra repaints: the window
  // repaints when the game shows a frame (60 a second) instead of at the
  // monitor's full rate all the time (SDK ImGuiDialog::WantsContinuousRedraw).
  bool WantsContinuousRedraw() const override { return false; }

 private:
  PerfectDarkApp* app_;
  bool f9_was_down_ = false;
  // TEMP_MENU_SCAN: F10 triggers a one-shot guest-memory scan for known
  // Video Options menu strings, to locate the real menu-item table for the
  // native-menu injection work. Remove once that table's address is known.
  bool f10_was_down_ = false;
  // TEMP_MENU_SCAN: F11 toggles a before/after guest-memory snapshot+diff,
  // to find the exact byte backing "2-Player Screen Split" by pressing F11,
  // toggling the option in-game, then pressing F11 again.
  bool f11_was_down_ = false;
  // TEMP_MENU_SCAN: F12 searches for pointers to the shared UI string
  // table's guest address, to find a fixed .data global the recompiled
  // source can be grepped for statically.
  bool f12_was_down_ = false;
  // TEMP_MENU_SCAN: F8 arms / F6 disarms the write-watch (see pd_watch).
  bool f8_was_down_ = false;
  bool f6_was_down_ = false;
  // TEMP_MENU_SCAN: F7 arms a write-watch on the static image instead.
  bool f7_was_down_ = false;
  // TEMP_MENU_MOUSE: F3 dumps the live menu dialog state.
  bool f3_was_down_ = false;
  // TEMP_MENU_SCAN: F5 triggers the sub_82343458 register trace.
  bool f5_was_down_ = false;
  // TEMP_LIGHT_DIAG: Insert arms the pixel-constant draw capture.
  bool ins_was_down_ = false;
  // TEMP_MENU_SCAN: F4 triggers the second (item-base) trace point.
  bool f4_was_down_ = false;
  // TEMP_MENU_SCAN: F1 arms a write-watch on a narrow range around the
  // sub_82343458 r3/r11 candidates (both proved byte-identical between
  // Horizontal/Vertical - watching for the actual write is the fallback).
  bool f1_was_down_ = false;
};

class PerfectDarkApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    auto app = std::unique_ptr<PerfectDarkApp>(new PerfectDarkApp(ctx, "perfect_dark",
        PPCImageConfig));
    current_instance_ = app.get();
    return app;
  }

  // Keep the game fully portable: game data, saves and cache all live beside
  // the executable rather than under the user's profile, so the installed
  // folder can be moved or backed up as a single unit.
  //
  // Release layout: the game program sits in <release>\resources\runtime and
  // its data beside that folder - resources\assets (imported by "Perfect
  // Dark.exe" in the release root from the player's own XBLA package) and
  // resources\userdata, which also holds perfect_dark.toml. Any other place
  // (a dev build, or an older release's Game folder) keeps assets, userdata
  // and perfect_dark.toml beside the exe, as before.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::vector<wchar_t> exe_path(32768);
    const DWORD exe_length = GetModuleFileNameW(nullptr, exe_path.data(), static_cast<DWORD>(exe_path.size()));
    const auto app_directory = std::filesystem::path(std::wstring(exe_path.data(), exe_length)).parent_path();

    const bool release_layout = _wcsicmp(app_directory.filename().c_str(), L"runtime") == 0;
    const auto data_root = release_layout ? app_directory.parent_path() : app_directory;

    const auto assets_path = data_root / "assets";
    if (!std::filesystem::is_directory(assets_path)) {
      // Preempt the SDK's generic "--game_data_root does not exist" message
      // (written for a CLI flag the player never typed) with one that tells
      // them what to actually do.
      if (release_layout) {
        MessageBoxW(nullptr,
                    L"Perfect Dark's game data has not been imported yet.\n\n"
                    L"Open \"Perfect Dark.exe\" in the main Perfect Dark folder "
                    L"and select your Xbox 360 Perfect Dark package.",
                    L"Perfect Dark", MB_OK | MB_ICONERROR);
      } else {
        MessageBoxW(nullptr,
                    L"Perfect Dark's game data was not found.\n\n"
                    L"The \"assets\" folder with the game data must be beside "
                    L"the program.",
                    L"Perfect Dark", MB_OK | MB_ICONERROR);
      }
      ExitProcess(1);
    }

    paths.game_data_root = assets_path;

    const auto user_data_path = data_root / "userdata";
    paths.user_data_root = user_data_path;
    paths.cache_root = user_data_path / "cache";
    if (release_layout) paths.config_path = user_data_path / "perfect_dark.toml";
  }

  std::string GetWindowTitle() const override { return "Perfect Dark"; }

  // window() is a protected ReXApp accessor; the settings dialog/tracker
  // aren't part of this class hierarchy so they need a public way to check
  // focus before trusting GetAsyncKeyState/XInput, which both read
  // system-wide input state rather than state scoped to this window.
  bool HasWindowFocus() const { return window() && window()->HasFocus(); }

  // The guest's own "Gamer profile not online" screen (Connect to Xbox LIVE /
  // Continue playing offline) gates on the local profile's signin state; the
  // player is never going to sign in to Xbox LIVE through this runtime, so
  // there's nothing to gain by making them dismiss that prompt on every
  // launch. Marking the profile signed-in-locally (state 2 - present, but not
  // connected to Live) satisfies the guest's check the same way manually
  // picking "Continue playing offline" would, without showing the dialog at
  // all. kernel_state()/user_profile() already exist by this point (XEX image
  // is loaded before OnPostSetup runs; see ReXApp::Setup), unlike
  // OnCreateDialogs, which runs too early for this.
  void OnPostSetup() override {
    REXLOG_INFO("Perfect Dark PC v{}", PD_VERSION);
    LogSystemInfo();
    MigrateSettings();
    auto* kernel_state = REX_KERNEL_STATE();
    auto* profile = kernel_state ? kernel_state->user_profile() : nullptr;
    if (profile) {
      profile->set_signin_state(2);
    }
    // The game's speed follows vblanks, so keep them at 60 Hz whatever the
    // Vsync setting (SDK fixed_guest_vblank; see HANDOVER 4a). Vsync off then
    // only lets frames tear instead of running the game two to four times too
    // fast.
    rex::cvar::SetFlagByName("fixed_guest_vblank", "true");
    // Mouse and keyboard are always on here (there is no option for them), so
    // they must not depend on a line in perfect_dark.toml: the SDK's own
    // default is false, and anything that saves the config while a screen has
    // mnk_mode switched off - key rebinding, the Online page - would otherwise
    // leave the next run with no keyboard.
    rex::cvar::SetFlagByName("mnk_mode", "true");
    // Internet play: the built-in online server (tools/relay/pd_relay.py).
    // Empty until one is deployed; a net_relay_server line in the toml or on
    // the command line wins.
    if (kOnlineServer[0] && rex::cvar::GetFlagByName("net_relay_server").empty()) {
      rex::cvar::SetFlagByName("net_relay_server", kOnlineServer);
    }
  }

  // Shader template fix-ups, applied right after default.xex is loaded and
  // before the game compiles its HLSL templates (both live in read-only data,
  // so each write flips the page writable first - a direct write faults).
  //
  // Deliberately NOT applied: xenia-canary's "Fix texture noise on NVIDIA
  // GPUs" patch (0x30 at 0x82014B7B). That byte is the register digit in the
  // vertex template's "float4 FogNearFar:register(c1)"; the patch turns it
  // into c0, which aliases FogNearFar onto UVScaleOffset. The engine writes
  // FogNearFar (w = -1 means "shade with pixel constant c4") to c1, so with the
  // patch the shade flag never reached the shader: props, bodies and the gun
  // stayed fully lit when a room's lights went off (HANDOVER 4i). The noise
  // that patch hid is fixed properly by the FL threshold patch below.
  void OnPostLoadXexImage() override {
    if (auto* mem = runtime() ? runtime()->memory() : nullptr) {
      const uint8_t* fog_register = mem->TranslateVirtual<uint8_t*>(0x82014B7B);
      REXLOG_INFO("[shaderfix] FogNearFar register digit is '{}' (c1 expected)",
                  fog_register ? static_cast<char>(*fog_register) : '?');

      // Shade-mode comparison in the runtime pixel-shader template.
      //
      // Characters and objects are darkened by blending toward a per-object
      // shade colour (the N64 fog-colour register, FogCol / c4 here). The
      // vertex shader flags "use the shade alpha" by writing exactly -1 to the
      // FL varying, and the generated pixel shader tests it with == -1:
      //     float foglevel = In.FL;
      //     if( In.FL == -1 ) foglevel = FogCol.a;
      // FL is interpolated, and NVIDIA's interpolation hands some pixels
      // -0.9999... instead of -1 (Triang3l, xenia game-compatibility #316), so
      // the test fails on those pixels. They then blend by ~-1 instead of the
      // shade alpha, giving roughly 2*colour - FogCol. That single comparison
      // produced objects and guards staying lit in dark rooms, the "noise" on
      // models, and the IR scanner failing to highlight hidden objects (it uses
      // the same FogCol path). Distance-fog values are never negative, so a
      // threshold of -0.5 cannot misfire. Same length, so nothing shifts.
      static const char kFind[] = "if( In.FL == -1 )";
      static const char kRepl[] = "if( In.FL < -0.5)";
      static_assert(sizeof(kFind) == sizeof(kRepl), "shader patch must not change length");
      uint8_t* fl = mem->TranslateVirtual<uint8_t*>(0x8201864E);
      if (fl && memcmp(fl, kFind, sizeof(kFind) - 1) == 0) {
        DWORD fl_old = 0;
        if (VirtualProtect(fl, sizeof(kFind) - 1, PAGE_READWRITE, &fl_old)) {
          memcpy(fl, kRepl, sizeof(kRepl) - 1);
          DWORD unused = 0;
          VirtualProtect(fl, sizeof(kFind) - 1, fl_old, &unused);
          REXLOG_INFO("[shaderfix] FL shade-mode comparison patched: == -1  ->  < -0.5");
        } else {
          REXLOG_INFO("[shaderfix] FL patch: could not unprotect page");
        }
      } else {
        // Different XEX build/title update, or already patched - leave it.
        REXLOG_INFO("[shaderfix] FL patch skipped: expected text not found at 0x8201864E");
      }

      PatchShine(mem);
    }
  }

  // Shine on reflective surfaces (Joanna's suit, guns, signs, ships). The
  // runtime pixel-shader template has two terms, both gated per material by
  // reflParams.x (c38), so surfaces without reflectivity never change:
  //   specular glints  reflLight += 1.0f * lightColour[l] * pow(...,60.0f);
  //   reflection       stage.rgb = stage.rgb*(1-reflParams.x) + reflCube*reflParams.x;
  // pd_shine scales both: the glint strength, and the reflection mix (the
  // same blend written as lerp, with the amount clamped to 1). The lines are
  // rewritten in place before the game compiles its shaders; each new line is
  // no longer than the original and is padded with spaces.
  static void PatchShine(rex::memory::Memory* mem) {
    double k = REXCVAR_GET(pd_shine);
    if (!(k >= 1.0)) k = 1.0;
    if (k > 3.0) k = 3.0;
    auto patch_line = [&](uint32_t addr, const std::string& find, std::string repl) {
      uint8_t* at = mem->TranslateVirtual<uint8_t*>(addr);
      if (!at || memcmp(at, find.data(), find.size()) != 0 || repl.size() > find.size()) {
        return false;
      }
      repl.insert(repl.size() - 1, find.size() - repl.size(), ' ');  // pad before the newline
      DWORD old = 0;
      if (!VirtualProtect(at, find.size(), PAGE_READWRITE, &old)) return false;
      memcpy(at, repl.data(), repl.size());
      DWORD unused = 0;
      VirtualProtect(at, find.size(), old, &unused);
      return true;
    };
    if (k == 1.0) {
      REXLOG_INFO("[shaderfix] shine left as the original (pd_shine = 1)");
      return;
    }
    const std::string num = fmt::format("{:.2f}", k);
    const bool glint = patch_line(
        0x820187D8, "     reflLight += 1.0f * lightColour[l] * pow(saturate(dp),60.0f);\n",
        "     reflLight += " + num + " * lightColour[l] * pow(saturate(dp),60.0f);\n");
    const bool reflection = patch_line(
        0x820186D0,
        "  stage%d.rgb = stage%d.rgb * ( 1 - reflParams.x ) + reflCube * reflParams.x;\n",
        "  stage%d.rgb = lerp(stage%d.rgb, reflCube, saturate(reflParams.x*" + num + "));\n");
    REXLOG_INFO("[shaderfix] shine x{}: glints {}, reflections {}", num,
                glint ? "patched" : "NOT patched", reflection ? "patched" : "NOT patched");
  }

  // Register the always-on input tracker that opens the PC settings panel on
  // F9 (see PerfectDarkInputTracker/PerfectDarkSettingsDialog).
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    rex::input::mnk::MnkInputDriver::SetLookDeltaSink(&PerfectDarkApp::OnMouseLookDelta);
    // Highest z-order, so key rebinding sees every wheel notch first; it never
    // marks an event handled, so the rest of the chain still gets them.
    if (window()) window()->AddInputListener(&rebind_wheel_listener_, 0x7FFF);
    imgui_drawer_ = drawer;
    imgui_drawer_->AddDialog(new PerfectDarkInputTracker(imgui_drawer_, this));
  }

  void OnSettingsDialogClosed() { settings_dialog_open_ = false; }

  void OpenSettingsDialog() {
    if (imgui_drawer_ && !settings_dialog_open_) {
      settings_dialog_open_ = true;
      imgui_drawer_->AddDialog(new PerfectDarkSettingsDialog(imgui_drawer_, this));
    }
  }

  // Settings > Online > Player Name. Asked for from the guest thread, so the
  // dialog is created on the next UI frame (TickNameEntry).
  static void RequestNameEntry() { name_entry_wanted_ = true; }
  static void RequestAddressEntry() { address_entry_wanted_ = true; }

  static void TickNameEntry() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->imgui_drawer_) return;
    const bool address = address_entry_wanted_.exchange(false);
    const bool name = name_entry_wanted_.exchange(false);
    if ((!address && !name) || name_entry_open_) return;
    name_entry_open_ = true;
    // The name is typed, so the game must not see the keys: mnk_mode off for
    // the duration, exactly as key rebinding does it.
    name_entry_prev_mnk_ = rex::cvar::GetFlagByName("mnk_mode");
    rex::cvar::SetFlagByName("mnk_mode", "false");
    app->imgui_drawer_->AddDialog(address ? new PdNameEntryDialog(app->imgui_drawer_, app,
                                                                   last_direct_address_, 1)
                                          : new PdNameEntryDialog(app->imgui_drawer_, app,
                                                                   OnlineName()));
  }

  static void OnNameEntryClosed(bool saved, const std::string& text, int kind) {
    // mnk_mode goes back first: SaveConfig writes only the cvars that differ
    // from their default, and mnk_mode's default is false - saving while it is
    // switched off for typing would drop the line and leave the next run with
    // no keyboard or mouse. (Key rebinding restores it before saving too.)
    if (!name_entry_prev_mnk_.empty()) {
      rex::cvar::SetFlagByName("mnk_mode", name_entry_prev_mnk_);
      name_entry_prev_mnk_.clear();
    }
    if (saved && kind == 1) {
      // Direct Connect: ask that one host; the Multiplayer rows join it once
      // it answers (see PdMultiplayerRowImpl).
      namespace netplay = rex::kernel::xam::netplay;
      last_direct_address_ = text;
      netplay::SetOnlineMode(false);
      netplay::SetEnabled(true);
      netplay::BrowseDirect(text);
      netplay::ShowNotice("Connecting to " + text + "...");
      direct_join_pending_ = true;
    } else if (saved) {
      const std::string old_name = OnlineName();
      rex::cvar::SetFlagByName("net_player_name", text);
      SavePcSettings();
      REXLOG_INFO("[online] player name is now \"{}\"", text);
      ReplaceGuestPlayerName(old_name, OnlineName());
      // The game sends names only when a player joins, so the others keep the
      // old one until this player leaves the lobby and comes back.
      namespace netplay = rex::kernel::xam::netplay;
      if (old_name != OnlineName() &&
          (netplay::IsHostingSession() || netplay::IsJoinedSession())) {
        netplay::ShowNotice(netplay::IsHostingSession()
                                ? "Name changed - players already here see it after they rejoin"
                                : "Name changed - other players see it once you rejoin");
      }
    }
    name_entry_open_ = false;
  }

  // The game asks for the profile name (XUserGetName) only at certain moments -
  // opening Combat Simulator, starting a match - and keeps its own copies: the
  // player table at 0x82641950 (216 bytes a player, the name at +75, 24 bytes)
  // and the lobby's. So a new Player Name used to show only after leaving
  // Combat Simulator and coming back (user, 2026-10-27). The copies of the old
  // name in the game's data are rewritten at once instead. Names are at most 15
  // characters, and every field that holds one has room for 16.
  static constexpr uint32_t kNameScanStart = 0x82630000;
  static constexpr uint32_t kNameScanEnd = 0x82678000;
  static void ReplaceGuestPlayerName(const std::string& old_name, const std::string& new_name) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    if (!mem || old_name.empty() || old_name == new_name || old_name.size() > 15) return;
    const std::string name = new_name.substr(0, 15);
    uint8_t* data = mem->TranslateVirtual<uint8_t*>(kNameScanStart);
    const size_t size = kNameScanEnd - kNameScanStart;
    const size_t n = old_name.size();
    int replaced = 0;
    for (size_t i = 1; i + 16 <= size; ++i) {
      if (data[i] != uint8_t(old_name[0]) || data[i - 1] != 0 || data[i + n] != 0 ||
          std::memcmp(data + i, old_name.data(), n) != 0) {
        continue;
      }
      std::memset(data + i, 0, std::max(n, name.size()) + 1);
      std::memcpy(data + i, name.data(), name.size());
      REXLOG_INFO("[online] player name updated in the game at 0x{:08X}",
                  kNameScanStart + uint32_t(i));
      ++replaced;
      i += 15;
    }
    if (!replaced) REXLOG_INFO("[online] no copy of the old player name in the game's data");
  }

  // TEMP_MENU_SCAN: diagnostic guest-memory scan for the native "Video
  // Options" menu-item table, triggered by F10 (see PerfectDarkInputTracker).
  // No symbols exist for this XBLA binary, so the table's address is found
  // empirically: search committed guest memory for the exact menu strings
  // (confirmed byte-for-byte identical to the N64 decompilation's text
  // tables, e.g. "2-Player Screen Split"), then search again for 32-bit
  // big-endian pointers to any string found - those pointer locations are
  // candidate menuitem struct fields. Remove this whole block (and the F10
  // wiring in PerfectDarkInputTracker::OnDraw) once the table is found.
  static void DebugScanForMenuStrings() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[menu_scan] runtime/memory not ready");
      return;
    }
    auto* mem = app->runtime()->memory();

    struct Needle {
      std::string label;
      std::vector<uint8_t> bytes;
    };
    std::vector<Needle> needles;
    auto add_ascii = [&](const char* text) {
      std::vector<uint8_t> b(text, text + strlen(text));
      needles.push_back({std::string(text) + " (ascii)", std::move(b)});
    };
    auto add_utf16be = [&](const char* text) {
      std::vector<uint8_t> b;
      for (const char* p = text; *p; ++p) {
        b.push_back(0);
        b.push_back(static_cast<uint8_t>(*p));
      }
      needles.push_back({std::string(text) + " (utf16be)", std::move(b)});
    };
    auto add_utf16le = [&](const char* text) {
      std::vector<uint8_t> b;
      for (const char* p = text; *p; ++p) {
        b.push_back(static_cast<uint8_t>(*p));
        b.push_back(0);
      }
      needles.push_back({std::string(text) + " (utf16le)", std::move(b)});
    };

    static const char* kStrings[] = {
        "2-Player Screen Split", "Screen Split", "Video Options", "Hi-Res", "Screen Size",
    };
    for (const char* s : kStrings) {
      add_ascii(s);
      add_utf16be(s);
      add_utf16le(s);
    }

    REXLOG_INFO("[menu_scan] Starting scan for {} needles...", needles.size());

    std::unordered_map<uint32_t, std::string> hit_map;
    const uint32_t kScanStart = 0x00010000;
    const uint32_t kScanEnd = 0xC0000000u;
    int regions_scanned = 0;

    auto for_each_committed_region = [&](auto&& visit) {
      uint32_t addr = kScanStart;
      while (addr < kScanEnd) {
        auto* heap = mem->LookupHeap(addr);
        if (!heap) {
          addr += 0x10000;
          continue;
        }
        rex::memory::HeapAllocationInfo info{};
        if (!heap->QueryRegionInfo(addr, &info) || info.region_size == 0) {
          addr += 0x10000;
          continue;
        }
        bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
        bool readable = (info.protect & rex::memory::kMemoryProtectRead) != 0;
        if (committed && readable) {
          visit(addr, info.region_size);
        }
        addr += info.region_size;
      }
    };

    // Phase 1: find the raw string data.
    for_each_committed_region([&](uint32_t region_addr, uint32_t region_size) {
      if (hit_map.size() >= 200) return;
      regions_scanned++;
      uint8_t* host = mem->TranslateVirtual<uint8_t*>(region_addr);
      for (const auto& needle : needles) {
        if (needle.bytes.size() > region_size) continue;
        size_t limit = region_size - needle.bytes.size();
        for (size_t off = 0; off <= limit; ++off) {
          if (memcmp(host + off, needle.bytes.data(), needle.bytes.size()) == 0) {
            uint32_t hit_addr = region_addr + static_cast<uint32_t>(off);
            hit_map[hit_addr] = needle.label;
            REXLOG_INFO("[menu_scan] STRING HIT: {} at 0x{:08X}", needle.label, hit_addr);
          }
        }
      }
    });
    REXLOG_INFO("[menu_scan] Scanned {} committed regions, found {} string hits.", regions_scanned,
                hit_map.size());

    if (hit_map.empty()) {
      REXLOG_INFO(
          "[menu_scan] No string hits - menu text may use a different encoding or be "
          "compressed until first shown.");
      return;
    }

    // Phase 2: find 32-bit big-endian pointers to any string hit.
    REXLOG_INFO("[menu_scan] Phase 2: searching for pointers to string hits...");
    std::vector<uint32_t> ptr_hit_addrs;
    for_each_committed_region([&](uint32_t region_addr, uint32_t region_size) {
      if (ptr_hit_addrs.size() >= 200 || region_size < 4) return;
      uint8_t* host = mem->TranslateVirtual<uint8_t*>(region_addr);
      for (size_t off = 0; off + 4 <= region_size; off += 4) {
        uint32_t value = (uint32_t(host[off]) << 24) | (uint32_t(host[off + 1]) << 16) |
                         (uint32_t(host[off + 2]) << 8) | uint32_t(host[off + 3]);
        auto it = hit_map.find(value);
        if (it != hit_map.end()) {
          uint32_t ptr_addr = region_addr + static_cast<uint32_t>(off);
          REXLOG_INFO("[menu_scan] POINTER HIT: 0x{:08X} -> \"{}\" string at 0x{:08X}", ptr_addr,
                      it->second, value);
          ptr_hit_addrs.push_back(ptr_addr);
        }
      }
    });
    REXLOG_INFO("[menu_scan] Done. {} pointer hits logged.", ptr_hit_addrs.size());

    // TEMP_MENU_SCAN: cache for the F8/F6 write-watch feature below, so it
    // can watch "whatever F10 last found" without needing hardcoded
    // addresses (which shift between runs since this is heap-allocated).
    last_scan_string_addrs_.clear();
    for (const auto& kv : hit_map) last_scan_string_addrs_.push_back(kv.first);
    last_scan_ptr_addrs_ = ptr_hit_addrs;

    // Phase 3: hex-dump the surrounding memory around each cluster of >=2
    // adjacent (stride-4) pointer hits - this is almost certainly the real
    // menuitem/label table, and the dump lets us read out its exact struct
    // layout (type/flags/handler fields) by eye.
    std::sort(ptr_hit_addrs.begin(), ptr_hit_addrs.end());
    size_t i = 0;
    while (i < ptr_hit_addrs.size()) {
      size_t j = i;
      while (j + 1 < ptr_hit_addrs.size() && ptr_hit_addrs[j + 1] - ptr_hit_addrs[j] == 4) {
        ++j;
      }
      if (j > i) {
        uint32_t dump_start = ptr_hit_addrs[i] >= 48 ? ptr_hit_addrs[i] - 48 : 0;
        uint32_t dump_end = ptr_hit_addrs[j] + 160;
        REXLOG_INFO("[menu_scan] Cluster of {} adjacent pointers at 0x{:08X}..0x{:08X}, dumping 0x{:08X}..0x{:08X}:",
                    j - i + 1, ptr_hit_addrs[i], ptr_hit_addrs[j], dump_start, dump_end);
        for (uint32_t a = dump_start; a < dump_end; a += 16) {
          uint8_t* row = mem->TranslateVirtual<uint8_t*>(a);
          std::string hex;
          std::string words;
          for (int k = 0; k < 16; k += 4) {
            uint32_t w = (uint32_t(row[k]) << 24) | (uint32_t(row[k + 1]) << 16) |
                         (uint32_t(row[k + 2]) << 8) | uint32_t(row[k + 3]);
            words += fmt::format("{:08X} ", w);
          }
          for (int k = 0; k < 16; ++k) {
            hex += (row[k] >= 0x20 && row[k] < 0x7f) ? static_cast<char>(row[k]) : '.';
          }
          REXLOG_INFO("[menu_scan]   0x{:08X}: {} | {}", a, words, hex);
        }
      }
      i = j + 1;
    }

    // Phase 4: the table itself is a heap allocation whose address varies
    // run to run, so it can't be grepped for directly in the recompiled
    // source. But IF some fixed .data/.bss global in the static image
    // (0x82000000-0x826E0000) holds a pointer to it, that global's address
    // IS stable and CAN be grepped for. Search freshly-discovered table
    // slot addresses (not hardcoded ones) for exactly that.
    REXLOG_INFO("[menu_scan] Phase 4: searching for pointers to the table slots themselves...");
    const uint32_t kImageStart = 0x82000000;
    const uint32_t kImageEnd = 0x826E0000;
    std::unordered_map<uint32_t, int> slot_targets;
    for (uint32_t a : ptr_hit_addrs) slot_targets[a] = 0;
    int slot_ptr_hits = 0;
    int slot_ptr_image_hits = 0;
    for_each_committed_region([&](uint32_t region_addr, uint32_t region_size) {
      if (region_size < 4) return;
      uint8_t* host = mem->TranslateVirtual<uint8_t*>(region_addr);
      for (size_t off = 0; off + 4 <= region_size; off += 4) {
        uint32_t value = (uint32_t(host[off]) << 24) | (uint32_t(host[off + 1]) << 16) |
                         (uint32_t(host[off + 2]) << 8) | uint32_t(host[off + 3]);
        if (slot_targets.find(value) == slot_targets.end()) continue;
        uint32_t ptr_addr = region_addr + static_cast<uint32_t>(off);
        bool in_image = ptr_addr >= kImageStart && ptr_addr < kImageEnd;
        REXLOG_INFO("[menu_scan] {} SLOT-POINTER at 0x{:08X} -> table slot 0x{:08X}",
                    in_image ? "IMAGE" : "heap", ptr_addr, value);
        slot_ptr_hits++;
        if (in_image) slot_ptr_image_hits++;
      }
    });
    REXLOG_INFO("[menu_scan] Phase 4 done. {} slot-pointer hits, {} in the static image range.",
                slot_ptr_hits, slot_ptr_image_hits);
  }

  // TEMP_MENU_SCAN: shared region-walker, factored out of
  // DebugScanForMenuStrings so DebugDumpStringTable's reverse-pointer search
  // can reuse it without duplicating the heap-info plumbing.
  template <typename Visit>
  static void ForEachCommittedRegion(rex::memory::Memory* mem, Visit&& visit) {
    const uint32_t kScanStart = 0x00010000;
    const uint32_t kScanEnd = 0xC0000000u;
    uint32_t addr = kScanStart;
    while (addr < kScanEnd) {
      auto* heap = mem->LookupHeap(addr);
      if (!heap) {
        addr += 0x10000;
        continue;
      }
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(addr, &info) || info.region_size == 0) {
        addr += 0x10000;
        continue;
      }
      bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
      bool readable = (info.protect & rex::memory::kMemoryProtectRead) != 0;
      if (committed && readable) {
        visit(addr, info.region_size);
      }
      addr += info.region_size;
    }
  }

  // TEMP_MENU_SCAN: manual big-endian 32-bit read, matching the pattern
  // DebugScanForMenuStrings already uses - REX_LOAD_U32 isn't usable here
  // since it expands to something relying on a `base` variable that only
  // exists inside generated DEFINE_REX_FUNC bodies, not this app-side code.
  static uint32_t ReadGuestU32(rex::memory::Memory* mem, uint32_t addr) {
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(addr);
    if (!p) return 0;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
           uint32_t(p[3]);
  }

  // TEMP_MENU_SCAN: does this guest address look like a pointer into a
  // printable string in the shared UI string blob found by
  // DebugScanForMenuStrings? Used to find the extent of the pointer table by
  // walking outward from a known-good slot. Note: entries are terminated by
  // '\n' (0x0A), not '\0' - confirmed empirically (byte 13 after "Video
  // Options" is 0x0A) - this is a newline-separated text blob, not a set of
  // C strings.
  static bool LooksLikeAsciiStringPointer(rex::memory::Memory* mem, uint32_t ptr) {
    if (ptr < 0x00010000 || ptr >= 0xC0000000u) return false;
    if (!mem->LookupHeap(ptr)) return false;
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(ptr);
    if (!p) return false;
    int len = 0;
    for (; len < 256; ++len) {
      uint8_t c = p[len];
      if (c == 0 || c == '\n') break;
      if (c < 0x20 || c >= 0x7f) return false;
    }
    return len >= 1 && len < 256;
  }

  // TEMP_MENU_SCAN: F1 walks outward from the last F10 scan's pointer hits
  // to find the full extent of the shared UI string table, then dumps every
  // entry (index, address, decoded text) - including entries far from any
  // string we explicitly searched for, i.e. potentially unused/hidden menu
  // items or other screens' labels sitting in the same pooled table.
  // ---------------------------------------------------------------- menus --
  //
  // Mouse in menus (pd_menu_mouse). The game's menus are built for a pad: the
  // highlight moves a row at a time and A selects. Everything needed to point
  // at a row instead is in the menu's own state, which lives at fixed
  // addresses in this build:
  //
  //   0x82639B44  how many dialogs are open (0 = no menu on screen)
  //   0x82639B48  the dialog on top, the one being driven
  //   0x82639B4C  the rows it laid out, four bytes each: height (s16), which
  //               item the row shows (u8), block index (s8)
  //   0x82639CDC  how many rows are in that table
  //
  // A dialog carries the menu it was opened from at +0x00 (its item list is at
  // +0x08 of that, twenty bytes per item), the item the highlight is on at
  // +0x08, and its box at +0x14: x, y, width, height, then the width and
  // height of its contents. The numbers are in the menu's own 640 x 240 space,
  // which is stretched over the window.
  //
  // Rows are laid out for every open dialog in one table, innermost last, so
  // the rows of the dialog on top are the last run - it starts where the row's
  // item index last goes back to 0.
  struct MenuRow {
    uint32_t item;        // guest address of the menu item this row shows
    uint32_t type;        // MENUITEMTYPE_*
    float left, right;    // the column it sits in, in the menu's own space
    float top, bottom;    // where the game draws it, scrolling included
    int32_t block;        // the item's own data (list position etc.), -1 for none
    bool focusable;       // the game's highlight can sit on it
    bool blocked = false; // a kind of row it can sit on, but the game skips this one right now
  };
  struct MenuLayout {
    uint32_t dialog;
    float x, y, width, height;
    bool dimmed;          // a dropdown is open over the dialog
    bool tabs;            // the page is one of several, with chevrons either side
    std::vector<MenuRow> rows;
  };

  // g_Vars (0x82648BF8) + 1768 is the stage being played; the title screen is
  // a stage of its own.
  static constexpr uint32_t kStageNum = 0x826492E0;
  static constexpr uint32_t kStageTitle = 90;
  // The game's own "this player is in control" flag (the decompilation's
  // g_PlayersWithControl[0]): cleared while a menu, briefing or cutscene has
  // the screen, set while the player is playing. Found by watching .bss across
  // front end and play.
  static constexpr uint32_t kPlayerHasControl = 0x82656740;

  // The menu system's state, laid out as in the N64 game (struct menu in the
  // decompilation) with this build's larger tables. Every screen of the front
  // end, the pause menus and the Combat Simulator lobby go through it:
  //   +0x4F4 depth (s16), +0x4F8 the dialog on top,
  //   +0x4FC rows: 100 x {height s16, item index u8, data block s8}, then rowend,
  //   +0x690 columns: 18 x 10 bytes {width s16, height s16, -, -, rowstart u16,
  //          row count u8, -}, then colend,
  //   +0x748 blocks: the per-item data (list scroll and selection) rows point into.
  // A dialog names its own columns (+0x04 first column, +0x05 how many), so
  // the rows that belong to the page on screen are never guessed at.
  static constexpr uint32_t kMenuBase = 0x82639650;
  // The open layers, one per level of menu (indexed by depth), 0x48 bytes
  // each: the dialogs that are pages of the same layer, then how many there
  // are and which one is showing. Pages of one layer are what the chevrons
  // either side of a dialog move between. Found by following the one other
  // place the open dialog is pointed at from; the N64's own layout does not
  // survive into this build (its layers sit where this build has zeroes).
  static constexpr uint32_t kMenuDepth = kMenuBase + 0x4F4;
  static constexpr uint32_t kMenuCurDialog = kMenuBase + 0x4F8;
  static constexpr uint32_t kMenuRows = kMenuBase + 0x4FC;
  static constexpr uint32_t kMenuRowEnd = kMenuBase + 0x68C;
  static constexpr uint32_t kMenuCols = kMenuBase + 0x690;
  static constexpr uint32_t kMenuColEnd = kMenuBase + 0x744;
  static constexpr uint32_t kMenuBlocks = kMenuBase + 0x748;
  static constexpr int kMenuRowSlots = 100;
  static constexpr int kMenuColSlots = 18;
  static constexpr int kMenuBlockSlots = 80;
  static constexpr uint32_t kMenuLayerBase = 0x826395EC;  // layer `depth` is + depth * 0x48
  static constexpr uint32_t kMenuLayerSize = 0x48;
  static constexpr uint32_t kMenuLayerCount = 0x44;  // u8 how many pages, then u8 which
  static constexpr int kMenuLayerSlots = 17;
  static uint32_t kMenuLayerAt(int32_t depth) {
    return kMenuLayerBase + uint32_t(depth) * kMenuLayerSize;
  }
  static constexpr uint32_t kMenuColSize = 10;
  // A dialog's rows start under its title strip (LINEHEIGHT + 1).
  static constexpr float kMenuTitleStrip = 12.0f;
  // The menus are drawn in a 640-wide space, and down the screen in the N64's
  // 220 lines: the whole picture, top to bottom, with no offset.
  static constexpr float kMenuSpaceWidth = 640.0f;
  static constexpr float kMenuSpaceHeight = 220.0f;
  // A dialog box is believed only if it fits this (some sit a little lower).
  static constexpr float kMenuSaneHeight = 240.0f;

  static uint8_t ReadGuestU8(rex::memory::Memory* mem, uint32_t addr) {
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(addr);
    return p ? p[0] : 0;
  }
  static int16_t ReadGuestS16(rex::memory::Memory* mem, uint32_t addr) {
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(addr);
    return p ? int16_t((uint16_t(p[0]) << 8) | uint16_t(p[1])) : 0;
  }
  static void WriteGuestS16(rex::memory::Memory* mem, uint32_t addr, int16_t value) {
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(addr);
    if (!p) return;
    p[0] = uint8_t(uint16_t(value) >> 8);
    p[1] = uint8_t(uint16_t(value) & 0xFF);
  }

  // Is this a dialog on screen? A dialog points at the menu definition it was
  // opened from and carries a box that fits on screen.
  static bool MenuDialogLooksLive(rex::memory::Memory* mem, uint32_t dialog) {
    if (dialog < 0x82000000 || dialog >= 0x82800000) return false;
    // The definition and its item list can live anywhere in guest memory:
    // pages built at run time - Settings and the controls pages by the game,
    // Achievements, the server browser and the Online page by this port -
    // keep theirs on the heap rather than in the game's image.
    const uint32_t def = ReadGuestU32(mem, dialog + 0x00);
    if (def < 0x10000 || def >= 0xA0000000) return false;
    const uint32_t items = ReadGuestU32(mem, def + 0x08);
    if (items < 0x10000 || items >= 0xA0000000) return false;
    const int32_t x = int32_t(ReadGuestU32(mem, dialog + 0x14));
    const int32_t y = int32_t(ReadGuestU32(mem, dialog + 0x18));
    const int32_t w = int32_t(ReadGuestU32(mem, dialog + 0x1C));
    const int32_t h = int32_t(ReadGuestU32(mem, dialog + 0x20));
    if (x < 0 || y < 0 || w <= 8 || h <= 8) return false;
    return x <= int32_t(kMenuSpaceWidth) && y <= int32_t(kMenuSaneHeight) &&
           w <= int32_t(kMenuSpaceWidth) && h <= int32_t(kMenuSaneHeight);
  }

  // The dialog on top, or 0 when no menu is open. The depth and the row count
  // are checked with it - a dialog that has been closed still looks like a
  // dialog in memory, and acting on one of those would take the mouse away
  // from the game with no menu on screen.
  static uint32_t MenuFindDialog(rex::memory::Memory* mem) {
    const int32_t depth = int16_t(ReadGuestU32(mem, kMenuDepth) >> 16);
    if (depth <= 0 || depth > 16) return 0;
    const int32_t rowend = int32_t(ReadGuestU32(mem, kMenuRowEnd));
    if (rowend < 0 || rowend > kMenuRowSlots) return 0;
    const uint32_t dialog = ReadGuestU32(mem, kMenuCurDialog);
    return MenuDialogLooksLive(mem, dialog) ? dialog : 0;
  }

  // Rows the game itself will not leave the highlight on: greyed out (the
  // Combat Simulator lobby's Ready once pressed) or hidden. Its tick moves the
  // highlight off such a row on the next frame and plays the focus sound as it
  // does, so writing the highlight there with the pointer made the sound go
  // off over and over as the mouse moved across the row. The game answers
  // "would I skip this row?" with sub_821C1F38, which calls the row's own
  // handler, so it can only be asked on the game thread: a hook in the dialog
  // tick (sub_821C7198, via main.cpp) asks about every row and records the
  // answers here with MenuSkipNote.
  static inline std::mutex menu_skip_mutex_;
  static inline std::unordered_map<uint32_t, bool> menu_skip_;
  static inline std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> menu_skip_at_;

  static bool MenuItemSkipped(uint32_t item) {
    std::lock_guard<std::mutex> lock(menu_skip_mutex_);
    auto it = menu_skip_.find(item);
    return it != menu_skip_.end() && it->second;
  }

  // Game thread, before each dialog tick: is it time to ask about this
  // dialog's rows again? (At once for a dialog not seen yet, then every 50 ms.)
  static bool MenuSkipScanWanted(uint32_t dialog) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(menu_skip_mutex_);
    auto it = menu_skip_at_.find(dialog);
    if (it != menu_skip_at_.end() && now - it->second < std::chrono::milliseconds(50)) return false;
    menu_skip_at_[dialog] = now;
    return true;
  }
  static void MenuSkipNote(uint32_t item, bool skipped) {
    std::lock_guard<std::mutex> lock(menu_skip_mutex_);
    menu_skip_[item] = skipped;
  }

  // Which kinds of row the game's own highlight will sit on. A menu is mostly
  // made of things that are not choices - the title, separators, the
  // description panel at the bottom of the Combat Simulator lobby - and
  // putting the highlight on one of those leaves nothing highlighted at all.
  // The numbering is the N64 game's (MENUITEMTYPE_*), which this port kept,
  // plus three of its own before the end marker.
  static bool MenuItemTakesFocus(uint32_t type) {
    switch (type) {
      case 0x00:
      case 0x01:  // label (a title, a line of text)
      case 0x06:  // objectives
      case 0x0B:  // separator
      case 0x0E:  // ranking
      case 0x12:  // model
      case 0x15:  // meter
      case 0x17:  // marquee
      case 0x1A:  // this port's own additions: panels and the end marker
      case 0x1B:
      case 0x1C:
      case 0x1D:
        return false;
      default:
        return true;
    }
  }

  // Does clicking this kind of row do something in its own right? These beat
  // the chevrons at the sides of a page; a panel of text or a picture does
  // not, so a click on the blue either side of it turns the page.
  static bool MenuItemActsOnClick(uint32_t type) {
    switch (type) {
      case 0x03:
      case 0x04:  // selectable
      case 0x08:  // slider
      case 0x09:  // checkbox
      case 0x0A:
      case 0x0C:  // dropdown
      case 0x0D:  // keyboard
      case 0x11:  // carousel
      case 0x19:  // controller
        return true;
      default:
        return false;
    }
  }

  // Items that keep a selection of their own inside them: a list, the player
  // stats list, and a dropdown while it is open.
  static bool MenuItemIsList(uint32_t type) { return type == 0x02 || type == 0x0F; }
  static constexpr uint32_t kMenuItemDropdown = 0x0C;
  static constexpr uint32_t kMenuItemSlider = 0x08;
  static constexpr uint32_t kMenuItemCarousel = 0x11;

  // Rows for a screen with no columns laid out: the choices in the menu,
  // evenly spaced down the box under its title strip.
  static void MenuRowsFromItems(rex::memory::Memory* mem, uint32_t items, MenuLayout* out) {
    constexpr int kMaxItems = 40;
    std::vector<uint32_t> choices;
    for (int i = 0; i < kMaxItems; ++i) {
      const uint32_t item = items + uint32_t(i) * 20;
      const uint32_t type = ReadGuestU32(mem, item) >> 24;
      if (type == 0x1D || type == 0x00) break;
      if (MenuItemTakesFocus(type)) choices.push_back(item);
    }
    if (choices.empty()) return;
    const float top = out->y + kMenuTitleStrip;
    const float height = (out->height - kMenuTitleStrip) / float(choices.size());
    for (size_t i = 0; i < choices.size(); ++i) {
      out->rows.push_back({choices[i], ReadGuestU32(mem, choices[i]) >> 24, out->x,
                           out->x + out->width, top + height * float(i),
                           top + height * float(i + 1), -1, true, false});
    }
  }

  // Fills in the dialog on top and every row it shows, placed exactly where
  // the game draws them: the same walk as its renderer (dialog_render in the
  // decompilation) - column by column from the dialog's left edge, the last
  // column stretched to the box, rows stacked under the title strip and moved
  // by the dialog's scroll, rows scrolled out of the box left out. Returns
  // false when no menu is open; every field is checked before it is used.
  static bool MenuMouseLayout(rex::memory::Memory* mem, MenuLayout* out) {
    if (!mem) return false;
    const uint32_t dialog = MenuFindDialog(mem);
    if (!dialog) return false;
    const uint32_t def = ReadGuestU32(mem, dialog + 0x00);
    const uint32_t items = ReadGuestU32(mem, def + 0x08);

    const int32_t x = int32_t(ReadGuestU32(mem, dialog + 0x14));
    const int32_t y = int32_t(ReadGuestU32(mem, dialog + 0x18));
    const int32_t w = int32_t(ReadGuestU32(mem, dialog + 0x1C));
    const int32_t h = int32_t(ReadGuestU32(mem, dialog + 0x20));
    out->dialog = dialog;
    out->x = float(x);
    out->y = float(y);
    out->width = float(w);
    out->height = float(h);
    out->dimmed = ReadGuestU32(mem, dialog + 0x0C) != 0;
    out->rows.clear();

    // Pages that are one of a set (How To Play, the briefing screens) show a
    // chevron either side, and the pad moves between them with left and right.
    const int32_t depth = int16_t(ReadGuestU32(mem, kMenuDepth) >> 16);
    const uint32_t layer = kMenuLayerAt(depth);
    const int siblings = int8_t(ReadGuestU8(mem, layer + kMenuLayerCount));
    const int cursibling = int8_t(ReadGuestU8(mem, layer + kMenuLayerCount + 1));
    out->tabs = siblings >= 2 && siblings <= kMenuLayerSlots && cursibling >= 0 &&
                cursibling < siblings &&
                ReadGuestU32(mem, layer + uint32_t(cursibling) * 4) == dialog;

    const int colstart = ReadGuestU8(mem, dialog + 0x04);
    const int numcols = ReadGuestU8(mem, dialog + 0x05);
    const int32_t colend = int32_t(ReadGuestU32(mem, kMenuColEnd));
    const int32_t rowend = int32_t(ReadGuestU32(mem, kMenuRowEnd));
    if (numcols <= 0 || colend <= 0 || colend > kMenuColSlots || colstart + numcols > colend ||
        rowend <= 0) {
      MenuRowsFromItems(mem, items, out);
      return true;
    }

    const int32_t scroll = int32_t(ReadGuestU32(mem, dialog + 0x64));
    const float view_top = float(y) + kMenuTitleStrip;
    const float view_bottom = float(y + h);
    float curx = float(x);
    int32_t sumwidth = 0;
    for (int c = 0; c < numcols; ++c) {
      const uint32_t col = kMenuCols + uint32_t(colstart + c) * kMenuColSize;
      const int32_t colwidth = ReadGuestS16(mem, col + 0);
      const int rowstart = uint16_t(ReadGuestS16(mem, col + 6));
      const int numrows = ReadGuestU8(mem, col + 8);
      int32_t width = colwidth;
      sumwidth += colwidth;
      if (c == numcols - 1 && sumwidth < w - 2) width += (w - 2) - sumwidth;

      float cury = view_top + float(scroll);
      for (int r = 0; r < numrows; ++r) {
        const int index = rowstart + r;
        if (index < 0 || index >= rowend || index >= kMenuRowSlots) break;
        const uint32_t word = ReadGuestU32(mem, kMenuRows + uint32_t(index) * 4);
        const float row_h = float(int16_t(word >> 16));
        const uint32_t item = items + ((word >> 8) & 0xFF) * 20;
        const int32_t block = int8_t(word & 0xFF);
        const bool on_screen = row_h > 0.0f && cury + row_h >= view_top && cury <= view_bottom;
        if (on_screen) {
          const uint32_t type = ReadGuestU32(mem, item) >> 24;
          const bool kind_ok = MenuItemTakesFocus(type);
          const bool blocked = kind_ok && MenuItemSkipped(item);
          out->rows.push_back({item, type, curx, curx + float(width), cury, cury + row_h,
                               block >= 0 && block < kMenuBlockSlots ? block : -1,
                               kind_ok && !blocked, blocked});
        }
        cury += row_h;
      }
      curx += float(colwidth);
    }
    return true;  // a menu is open; it may have no rows the pointer can use
  }

  // The chevrons sit just outside the box, halfway down it (menu_render_dialog
  // draws them at the left and right edges). Their hit areas are a good deal
  // bigger than the marks themselves, and reach over the page title printed
  // sideways beside them, which reads as part of the same control.
  static int MenuTabArrowAt(const MenuLayout& layout, float px, float py) {
    if (!layout.tabs) return 0;
    // Well beyond the marks themselves: the strip they sit in, the sideways
    // page title beside them, and a good way into the panel, since that empty
    // blue margin either side of the text is where a hand naturally goes.
    // Rows that do something when clicked still win (MenuItemActsOnClick).
    constexpr float kOut = 50.0f;
    constexpr float kIn = 70.0f;
    constexpr float kTall = 60.0f;
    const float middle = layout.y + layout.height * 0.5f;
    if (py < middle - kTall || py > middle + kTall) return 0;
    if (px >= layout.x - kOut && px < layout.x + kIn) return -1;
    if (px > layout.x + layout.width - kIn && px <= layout.x + layout.width + kOut) return 1;
    return 0;
  }

  // The chevron itself and a margin round it: here the chevron wins even over a
  // row that acts on a click. Rows reach the box's edges, so a click on the
  // inner half of a chevron used to select the row beside it instead (the
  // pause menu's Leaderboards, user 2026-10-27). The rows' text starts about
  // 55 units in, well clear of this.
  static bool MenuTabArrowCoreAt(const MenuLayout& layout, float px, float py) {
    constexpr float kCoreIn = 32.0f;
    if (MenuTabArrowAt(layout, px, py) == 0) return false;
    return px < layout.x + kCoreIn || px > layout.x + layout.width - kCoreIn;
  }

  // The focusable row under a point, or -1. Inside a column, a point in the
  // gap between two rows (a separator, a label) goes to the nearer one, so the
  // highlight does not flicker off as the pointer crosses a thin line.
  // A carousel - the body and head on the Combat Simulator's Character page,
  // and anything else that steps through choices - draws an arrow at each end
  // of its row (menuitem_carousel_render), right on the box's border. Clicks
  // used to count only inside the row and within 30 units of its end, so a
  // click on an arrow, or just past one, often did nothing (user, 2026-10-25).
  // Each arrow now takes the outer quarter of its row, a margin outside the
  // box, and a little above and below the row. Returns -1 (left), 1 (right) or
  // 0, and the carousel row in *row_index.
  static constexpr float kCarouselEndShare = 0.25f;  // of the row's width
  static constexpr float kCarouselEndMin = 30.0f;
  static constexpr float kCarouselOutside = 24.0f;   // beyond the box edge
  static constexpr float kCarouselReach = 6.0f;      // above and below the row
  static int CarouselArrowAt(const MenuLayout& layout, float px, float py, int* row_index) {
    int best = -1;
    int best_side = 0;
    float best_distance = kCarouselReach + 0.001f;
    for (size_t i = 0; i < layout.rows.size(); ++i) {
      const MenuRow& row = layout.rows[i];
      if (!row.focusable || row.type != kMenuItemCarousel) continue;
      const float ends = std::max(kCarouselEndMin, (row.right - row.left) * kCarouselEndShare);
      int side = 0;
      if (px >= row.left - kCarouselOutside && px <= row.left + ends) {
        side = -1;
      } else if (px >= row.right - ends && px <= row.right + kCarouselOutside) {
        side = 1;
      }
      if (side == 0) continue;
      const float distance = py < row.top ? row.top - py : (py > row.bottom ? py - row.bottom : 0.0f);
      if (distance < best_distance) {
        best_distance = distance;
        best = int(i);
        best_side = side;
      }
    }
    if (row_index) *row_index = best;
    return best >= 0 ? best_side : 0;
  }

  static int MenuRowAt(const MenuLayout& layout, float px, float py) {
    if (px < layout.x || px > layout.x + layout.width || py < layout.y ||
        py > layout.y + layout.height) {
      return -1;
    }
    constexpr float kGapReach = 6.0f;
    int best = -1;
    float best_distance = kGapReach;
    for (size_t i = 0; i < layout.rows.size(); ++i) {
      const MenuRow& row = layout.rows[i];
      if (row.blocked && px >= row.left && px < row.right && py >= row.top && py < row.bottom) {
        return -1;  // pointing at a row the game skips: highlight nothing, not its neighbour
      }
      if (!row.focusable || px < row.left || px >= row.right) continue;
      if (py >= row.top && py < row.bottom) return int(i);
      const float distance = py < row.top ? row.top - py : py - row.bottom;
      if (distance < best_distance) {
        best_distance = distance;
        best = int(i);
      }
    }
    return best;
  }

  // A list's own state, in the data block its row points at (the N64's
  // menuitemdata_list): how far it has scrolled, the entry selected, where it
  // is scrolling to, and how tall its window is.
  struct MenuListState {
    uint32_t at;
    int16_t curoffsety, index, targetoffsety, viewheight;
  };
  static bool MenuReadList(rex::memory::Memory* mem, const MenuRow& row, MenuListState* out) {
    if (row.block < 0) return false;
    out->at = kMenuBlocks + uint32_t(row.block) * 4;
    out->curoffsety = ReadGuestS16(mem, out->at + 0);
    out->index = ReadGuestS16(mem, out->at + 2);
    out->targetoffsety = ReadGuestS16(mem, out->at + 4);
    out->viewheight = ReadGuestS16(mem, out->at + 6);
    return out->index >= 0 && out->viewheight > 0 && out->viewheight <= 480;
  }

  // Does this list stay put, so that pointing at an entry means anything?
  // Every list keeps its selection in the middle of its window and scrolls the
  // entries past it, so on all but one kind the entry under the pointer moves
  // the moment anything is selected. The exception is a dropdown whose options
  // all fit: the game holds its scroll at the top end, which its target offset
  // gives away - it stays at the halfway mark whichever option is picked. (A
  // plain list is never still: its target is simply where its selection is,
  // which can equal that mark by chance.)
  static bool MenuListStandsStill(const MenuListState& list, int line, bool dropdown) {
    if (!dropdown) return false;
    const int halfheight = (list.viewheight / 2) / line * line;
    return list.targetoffsety == halfheight;
  }

  // Which entry of a list is under the pointer, or -1. Entries are drawn one
  // line apart, the list scrolled so that entry `i` sits at
  //   top + halfheight - curoffsety + i * line
  // (menuitem_list_render). An open dropdown is the same list drawn over the
  // dialog, starting a line under its row or pulled up to fit the box
  // (menuitem_dropdown_overlay). A line is 12 units in this version (11 on
  // the N64) unless the list says otherwise, which it shows by how far down
  // its selection sits.
  static int MenuListEntryAt(rex::memory::Memory* mem, const MenuLayout& layout,
                             const MenuRow& row, bool dropdown, float py) {
    MenuListState list;
    if (!MenuReadList(mem, row, &list)) return -1;
    int line = 12;
    if (!dropdown && list.index > 0 && list.targetoffsety > 0 &&
        list.targetoffsety % list.index == 0) {
      const int guess = list.targetoffsety / list.index;
      if (guess >= 8 && guess <= 40) line = guess;
    }
    float top = row.top;
    if (dropdown) {
      top = row.top + 1.0f;
      if (top + float(list.viewheight) > layout.y + layout.height + 2.0f) {
        top = layout.y + layout.height - float(list.viewheight) + 2.0f;
      }
    }
    if (py < top || py >= top + float(list.viewheight)) return -1;
    const int halfheight = (list.viewheight / 2) / line * line;
    if (!MenuListStandsStill(list, line, dropdown)) return -1;
    // A plain list is placed by its selection (its target is exactly where the
    // selection sits, group headings included); a dropdown's target is held
    // inside its ends, so it is placed from the top.
    float entry0 = top + float(halfheight) - float(list.curoffsety);
    int base = 0;
    if (!dropdown) {
      entry0 += float(list.targetoffsety);
      base = list.index;
    }
    const int entry = base + int(std::floor((py - entry0) / float(line)));
    if (entry < 0) return -1;
    if (dropdown && list.viewheight % line == 0 && entry >= list.viewheight / line &&
        list.curoffsety == list.targetoffsety) {
      // The whole list fits its window, so this is past the last entry.
      return -1;
    }
    return entry;
  }

  // Menu space to window pixels, for drawing the pointer: the menus span the
  // picture, 640 across and 220 down.
  static float MenuPixelsY(const ImGuiIO& io, float menu_y) {
    return io.DisplaySize.y * menu_y / kMenuSpaceHeight;
  }

  static ImVec2 MenuCursorPixels(const ImGuiIO& io) {
    return ImVec2(menu_cursor_x_ * io.DisplaySize.x / kMenuSpaceWidth,
                  MenuPixelsY(io, menu_cursor_y_));
  }

  // The menus are driven by the mouse but the game draws no pointer, so this
  // draws one: the usual arrow, outlined so it shows against both the bright
  // and the dark parts of a menu.
  static void DrawMenuCursor() {
    ImGuiIO& io = ImGui::GetIO();
    // Keep the pointer's speed the same across and down: a menu unit is a
    // different number of pixels each way, depending on the window's shape.
    if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
      menu_dy_scale_.store((io.DisplaySize.x / kMenuSpaceWidth) /
                           (io.DisplaySize.y / kMenuSpaceHeight));
    }
    if (!menu_mouse_active_) return;
    const ImVec2 at = MenuCursorPixels(io);
    const float scale = io.DisplaySize.y / 720.0f;
    const float w = 11.0f * scale;
    const float h = 18.0f * scale;
    ImDrawList* draw = ImGui::GetForegroundDrawList();

    const ImVec2 tip(at.x, at.y);
    const ImVec2 left(at.x, at.y + h);
    const ImVec2 right(at.x + w, at.y + h * 0.72f);
    draw->AddTriangleFilled(tip, left, right, IM_COL32(255, 255, 255, 235));
    draw->AddTriangle(tip, left, right, IM_COL32(0, 0, 0, 200), 1.5f * scale);

    // TEMP_MENU_MOUSE: with developer hotkeys on, outline the box and every
    // row the pointer can use, the one under it in green, to check the maths
    // against what the game drew.
    if (REXCVAR_GET(pd_dev_hotkeys) && menu_layout_.dialog) {
      const float sx = io.DisplaySize.x / kMenuSpaceWidth;
      draw->AddRect(ImVec2(menu_layout_.x * sx, MenuPixelsY(io, menu_layout_.y)),
                    ImVec2((menu_layout_.x + menu_layout_.width) * sx,
                           MenuPixelsY(io, menu_layout_.y + menu_layout_.height)),
                    IM_COL32(255, 80, 80, 200), 0.0f, 0, 2.0f);
      if (menu_layout_.tabs) {
        const float middle = menu_layout_.y + menu_layout_.height * 0.5f;
        for (int side = -1; side <= 1; side += 2) {
          const float near_x = side < 0 ? menu_layout_.x - 26.0f : menu_layout_.x + menu_layout_.width;
          draw->AddRect(ImVec2(near_x * sx, MenuPixelsY(io, middle - 30.0f)),
                        ImVec2((near_x + 26.0f) * sx, MenuPixelsY(io, middle + 30.0f)),
                        IM_COL32(120, 180, 255, 160), 0.0f, 0, 1.5f);
        }
      }
      for (size_t i = 0; i < menu_layout_.rows.size(); ++i) {
        const MenuRow& row = menu_layout_.rows[i];
        if (!row.focusable) continue;
        if (row.type == kMenuItemCarousel) {
          const float ends = std::max(kCarouselEndMin, (row.right - row.left) * kCarouselEndShare);
          for (int side = 0; side < 2; ++side) {
            const float x0 = side ? row.right - ends : row.left - kCarouselOutside;
            const float x1 = side ? row.right + kCarouselOutside : row.left + ends;
            draw->AddRect(ImVec2(x0 * sx, MenuPixelsY(io, row.top - kCarouselReach)),
                          ImVec2(x1 * sx, MenuPixelsY(io, row.bottom + kCarouselReach)),
                          IM_COL32(120, 180, 255, 160), 0.0f, 0, 1.5f);
          }
        }
        const bool hot = int(i) == menu_hover_row_;
        draw->AddRect(ImVec2(row.left * sx + 2.0f, MenuPixelsY(io, row.top) + 1.0f),
                      ImVec2(row.right * sx - 2.0f, MenuPixelsY(io, row.bottom) - 1.0f),
                      hot ? IM_COL32(80, 255, 80, 220) : IM_COL32(255, 220, 80, 120), 0.0f, 0,
                      hot ? 2.0f : 1.0f);
      }
    }
  }

  // Runs once per drawn frame. One set of rules for every menu that goes
  // through the game's menu system:
  //   pointer over a row      -> that row is highlighted (only when the mouse
  //                              moves, so keys and pads keep working)
  //   pointer over a list     -> the entry under it is selected
  //   left click              -> select what is under the pointer (A)
  //   right click             -> back (B); Esc does the same (input driver)
  //   wheel                   -> selection up / down, a step per notch
  // Does nothing at all while no menu is open, so playing is untouched.
  static void TickMenuMouse() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    namespace mnk = rex::input::mnk;

    if (!REXCVAR_GET(pd_menu_mouse)) {
      if (menu_mouse_active_) {
        menu_mouse_active_ = false;
        mnk::MnkInputDriver::SetMenuMouseMode(false);
      }
      return;
    }

    MenuLayout layout = {0, 0.0f, 0.0f, 0.0f, 0.0f, false, false, {}};
    const bool open = MenuMouseLayout(mem, &layout);
    const auto now = std::chrono::steady_clock::now();
    if (open) {
      menu_open_at_ = now;
      menu_ever_open_ = true;
      institute_menu_pending_ = false;
    }
    if (!open) {
      // Menus swap dialogs as you move through them, and for a frame or two
      // there may be nothing to find. Hold on to the mouse briefly so a click
      // in that gap still goes to the menu rather than to the game.
      if (menu_mouse_active_ && now - menu_open_at_ < std::chrono::milliseconds(250)) return;
      if (menu_mouse_active_) {
        menu_mouse_active_ = false;
        mnk::MnkInputDriver::SetMenuMouseMode(false);
      }
      // "Press START To Play" has no menu of its own, and reaching for a key
      // to get past it with a mouse in hand is a nuisance, so a left click is
      // START there, and during the attract film the title plays behind it.
      //
      // Telling that film from someone actually playing takes a little care:
      // it runs a real stage with a real player, exactly like a mission. What
      // separates them is how they were reached - a mission is always started
      // from a menu, the film is not. So the film is "still showing" until a
      // menu opens, and only then does a click stop being START (it would
      // pause the game on every shot otherwise).
      const uint32_t stage = ReadGuestU32(mem, kStageNum);
      if (stage == kStageTitle) menu_attract_ = true;
      const bool front_end = stage == kStageTitle || menu_attract_;
      const bool focused = app->HasWindowFocus();
      const bool lmb = focused && front_end && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
      const bool any_key = TitleAnyKeyPressed(focused && front_end);
      if ((lmb && !menu_lmb_was_down_) || any_key) {
        title_start_pressed_ = true;  // the version label goes (DrawTitleVersion)
        mnk::MnkInputDriver::InjectButtons(0x0010, 90);   // X_INPUT_GAMEPAD_START
        mnk::MnkInputDriver::InjectPadKeystroke(0x5814);  // pad START
      }
      menu_lmb_was_down_ = lmb;
      menu_rmb_was_down_ = false;
      return;
    }

    const uint32_t focus = ReadGuestU32(mem, layout.dialog + 0x08);
    menu_attract_ = false;
    if (!menu_mouse_active_) {
      menu_mouse_active_ = true;
      mnk::MnkInputDriver::SetMenuMouseMode(true);
      // Start the pointer on the row the pad left the highlight on, so it does
      // not jump somewhere else the moment a menu opens.
      menu_cursor_x_ = layout.x + layout.width * 0.5f;
      menu_cursor_y_ = layout.y + layout.height * 0.5f;
      for (const MenuRow& row : layout.rows) {
        if (row.item == focus && row.focusable) {
          menu_cursor_x_ = (row.left + row.right) * 0.5f;
          menu_cursor_y_ = (row.top + row.bottom) * 0.5f;
          break;
        }
      }
      menu_wheel_.store(0);
      menu_list_steps_ = 0;
      menu_slider_drag_ = false;
      menu_slider_paying_ = false;
      menu_slider_close_ = false;
      menu_slider_owed_ = 0.0f;
      // Buttons already down (the click that opened this menu) are not clicks.
      menu_lmb_was_down_ = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
      menu_rmb_was_down_ = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    }

    // Motion collected by OnMouseLookDelta while a menu is open. Whether the
    // mouse moved at all matters: the highlight follows the pointer only when
    // the pointer moves, so pressing W or S (or a pad) moves it and it stays
    // there. Writing the pointer's row every frame would undo every key press
    // the moment it happened.
    bool pointer_moved = false;
    float moved_dx = 0.0f;
    {
      std::lock_guard<std::mutex> lock(net_look_mutex_);
      pointer_moved = menu_cursor_dx_ != 0.0f || menu_cursor_dy_ != 0.0f;
      moved_dx = menu_cursor_dx_;
      menu_cursor_x_ += menu_cursor_dx_;
      menu_cursor_y_ += menu_cursor_dy_;
      menu_cursor_dx_ = 0.0f;
      menu_cursor_dy_ = 0.0f;
    }
    // The pointer goes anywhere on screen, like a pointer should.
    menu_cursor_x_ = std::clamp(menu_cursor_x_, 0.0f, kMenuSpaceWidth - 1.0f);
    menu_cursor_y_ = std::clamp(menu_cursor_y_, 0.0f, kMenuSpaceHeight - 1.0f);

    // The row the highlight is on, and whether it keeps a list of its own that
    // is taking the input: a list page's list, or a dropdown that is open.
    int focus_row = -1;
    for (size_t i = 0; i < layout.rows.size(); ++i) {
      if (layout.rows[i].item == focus) focus_row = int(i);
    }
    const uint32_t focus_type = focus_row >= 0 ? ReadGuestU32(mem, focus) >> 24 : 0;
    const bool dropdown_open = layout.dimmed && focus_type == kMenuItemDropdown;
    const bool slider_active = layout.dimmed && focus_type == kMenuItemSlider;
    const bool focus_is_list = MenuItemIsList(focus_type) || dropdown_open;

    const int hovered =
        (dropdown_open || slider_active) ? -1 : MenuRowAt(layout, menu_cursor_x_, menu_cursor_y_);
    const uint32_t hovered_type = hovered >= 0 ? layout.rows[size_t(hovered)].type : 0;

    // ---- pointing ---------------------------------------------------------
    // Highlight the row under the pointer; over a list (or an open dropdown)
    // select the entry under it too. A list centres its selection as it
    // scrolls, which moves the entries under a pointer that has not moved, so
    // a new entry is only taken once the list has finished moving.
    int list_row = -1;
    if (dropdown_open) {
      list_row = focus_row;
    } else if (hovered >= 0 && MenuItemIsList(hovered_type)) {
      list_row = hovered;
    } else if (hovered < 0 && focus_is_list) {
      // Pages built around one list (the weapons and people pages of How To
      // Play) fill the rest of the box with a picture and a description, and
      // there is nothing else to point at. The list follows the mouse there
      // wherever the pointer happens to be.
      list_row = focus_row;
    }
    // Achievements and Keyboard & Mouse are long columns of rows that scroll
    // whenever the highlight reaches their top or bottom row, so a pointer
    // resting there kept them scrolling and the rows between were hard to pick.
    // There only the wheel, keys and pad move the highlight (user, 2026-10-27);
    // a click still acts on the row it is on.
    const uint32_t page_def = ReadGuestU32(mem, layout.dialog);
    const bool wheel_only_page = page_def != 0 && (page_def == achievements_dialog_ ||
                                                   page_def == pc_controls_dialog_);
    if (pointer_moved) {
      if (hovered >= 0 && !dropdown_open && !wheel_only_page) {
        const uint32_t item = layout.rows[size_t(hovered)].item;
        if (focus != item) WriteGuestU32(mem, layout.dialog + 0x08, item);
      }
      MenuListState list;
      if (list_row >= 0 && MenuReadList(mem, layout.rows[size_t(list_row)], &list)) {
        const int entry =
            list.curoffsety == list.targetoffsety
                ? MenuListEntryAt(mem, layout, layout.rows[size_t(list_row)], dropdown_open,
                                  menu_cursor_y_)
                : -1;
        if (entry >= 0 && entry != list.index) {
          // A list that stands still: take the entry the pointer is on. The
          // list's own tick keeps it in range and confirms it on A.
          WriteGuestS16(mem, list.at + 2, int16_t(entry));
        }
        // A list that scrolls keeps its selection in the middle of its window,
        // so there is nothing under the pointer to point at and moving the
        // mouse leaves it alone. The wheel steps it instead (below), which is
        // what the weapons and people pages of How To Play use.
      }
    }

    // Steps waiting to go into a list, one at a time: they are given to the
    // game as pad presses so it scrolls, plays its sound and tells the page
    // which entry is now picked (the model and description on the weapons
    // page come from that).
    if (menu_list_steps_ != 0 && now >= menu_list_step_at_) {
      const bool down = menu_list_steps_ > 0;
      menu_list_steps_ += down ? -1 : 1;
      // Far enough apart that the game sees each one as its own press: it
      // reads the pad once a frame, so the gap between holds has to last
      // several frames. Closer together and it reads a queue of steps as one
      // press being held, which it answers with its own slow key repeat.
      menu_list_step_at_ = now + std::chrono::milliseconds(120);
      mnk::MnkInputDriver::InjectStick(0, int16_t(down ? -32000 : 32000), 50);
    }

    // ---- dragging a slider -------------------------------------------------
    // A slider only moves while it is the thing taking the input - press A on
    // it and the dialog dims, exactly as the pad does - and its value lives
    // inside the game's own handler, out of reach. So a click opens it and
    // holding the button turns sideways movement of the mouse into the stick,
    // which is what moves the marker; letting go closes it again. The stick is
    // pushed as far as the mouse is moving, so a nudge is a nudge and a sweep
    // is a sweep.
    // The game reads the stick as a speed, not a place, and that speed has a
    // ceiling: a quick sweep of the hand asks for more travel than the slider
    // can make while the hand is moving, so it used to fall short - the same
    // sweep moved it half as far as a slow one. What the mouse asked for is
    // therefore kept as a debt in menu_slider_owed_ and paid off over the
    // frames that follow, as fast as the game will take it, so a movement
    // always ends up worth the same whatever speed it was made at.
    constexpr float kFullTilt = 3.9f;      // menu units of mouse per frame, flat out
    constexpr float kDeadzone = 7000.0f;   // under this the game ignores the stick
    if (menu_slider_drag_ && slider_active && moved_dx != 0.0f) {
      menu_slider_owed_ += moved_dx;
      menu_slider_moved_ = true;
    }
    if (slider_active && (menu_slider_drag_ || menu_slider_paying_)) {
      const float seconds =
          std::chrono::duration<float>(now - menu_slider_paid_at_).count();
      menu_slider_paid_at_ = now;
      const float frames = std::clamp(seconds, 0.005f, 0.05f) * 60.0f;
      const float most = kFullTilt * frames;
      const float owed = std::fabs(menu_slider_owed_);
      const float pay = std::min(owed, most);
      if (pay > 0.05f) {
        const float push = kDeadzone + (32000.0f - kDeadzone) * (pay / most);
        const int hold = int(frames * 16.6f) + 20;
        mnk::MnkInputDriver::InjectStick(
            int16_t(menu_slider_owed_ < 0.0f ? -push : push), 0, hold);
        menu_slider_owed_ -= menu_slider_owed_ < 0.0f ? -pay : pay;
      } else {
        menu_slider_owed_ = 0.0f;
      }
    }

    // ---- buttons ----------------------------------------------------------
    // Both ways: the pad state for the menus that read it, and a keystroke for
    // the screens that read those instead.
    const bool focused = app->HasWindowFocus();
    const bool lmb_now = focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rmb_now = focused && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (lmb_now && !menu_lmb_was_down_) {
      // A click selects what is under the pointer. Over a row, make sure that
      // row is the one highlighted first (the mouse may not have moved since
      // a key moved the highlight). Pages the pointer cannot see into - one
      // big panel with no rows, an open dropdown - take the click as it is.
      bool press = true;
      bool has_rows = false;
      for (const MenuRow& row : layout.rows) has_rows = has_rows || row.focusable;
      const bool row_wins = hovered >= 0 && MenuItemActsOnClick(hovered_type) &&
                            !MenuTabArrowCoreAt(layout, menu_cursor_x_, menu_cursor_y_);
      const int arrow = (dropdown_open || slider_active || row_wins)
                            ? 0
                            : MenuTabArrowAt(layout, menu_cursor_x_, menu_cursor_y_);
      if (slider_active || (hovered >= 0 && hovered_type == kMenuItemSlider)) {
        // A slider takes the button for as long as it is held: pressing opens
        // it if it was shut, dragging moves it, and letting go settles it.
        // Pressing and letting go without moving is a plain click, which
        // opens a shut slider and shuts an open one.
        menu_slider_drag_ = true;
        menu_slider_moved_ = false;
        menu_slider_was_open_ = slider_active;
        menu_slider_owed_ = 0.0f;
        menu_slider_paying_ = false;
        menu_slider_paid_at_ = now;
      }
      // A carousel arrow (CarouselArrowAt) is the pad's left or right on its
      // row. Its area reaches past the box, so outside the box a page chevron
      // there wins.
      int carousel = 0;
      if (!dropdown_open && !slider_active && (hovered < 0 || hovered_type == kMenuItemCarousel)) {
        int carousel_row = -1;
        carousel = CarouselArrowAt(layout, menu_cursor_x_, menu_cursor_y_, &carousel_row);
        const bool outside_box = menu_cursor_x_ < layout.x || menu_cursor_x_ > layout.x + layout.width;
        if (carousel != 0 && outside_box &&
            MenuTabArrowAt(layout, menu_cursor_x_, menu_cursor_y_) != 0) {
          carousel = 0;
        }
        if (carousel != 0) {
          WriteGuestU32(mem, layout.dialog + 0x08, layout.rows[size_t(carousel_row)].item);
        }
      }
      if (carousel != 0) {
        mnk::MnkInputDriver::InjectStick(int16_t(carousel > 0 ? 32000 : -32000), 0, 90);
        press = false;
      } else if (arrow != 0) {
        // A chevron: the page either side, as left and right do on the pad.
        mnk::MnkInputDriver::InjectStick(int16_t(arrow > 0 ? 32000 : -32000), 0, 90);
        press = false;
      } else if (slider_active) {
        press = false;  // settled when the button is let go
      } else if (!dropdown_open && has_rows) {
        if (hovered >= 0) {
          WriteGuestU32(mem, layout.dialog + 0x08, layout.rows[size_t(hovered)].item);
        } else {
          press = false;  // a click on nothing does nothing
        }
      }
      if (press) {
        mnk::MnkInputDriver::InjectButtons(0x1000, 20);   // X_INPUT_GAMEPAD_A
        mnk::MnkInputDriver::InjectPadKeystroke(0x5800);  // pad A
      }
    }
    if (rmb_now && !menu_rmb_was_down_) {
      if (slider_active) {
        // Back out of a slider rather than out of the page it is on: the game
        // leaves a slider the same way it entered, with A.
        menu_slider_drag_ = false;
        mnk::MnkInputDriver::InjectButtons(0x1000, 20);
        mnk::MnkInputDriver::InjectPadKeystroke(0x5800);
      } else {
        mnk::MnkInputDriver::InjectButtons(0x2000, 20);   // X_INPUT_GAMEPAD_B
        mnk::MnkInputDriver::InjectPadKeystroke(0x5801);  // pad B
      }
    }
    if (!lmb_now && menu_lmb_was_down_ && menu_slider_drag_) {
      // Letting go after dragging leaves the slider where it was put. A click
      // that moved nothing leaves it open instead, so it can be nudged with
      // the wheel or dragged from there; clicking it again closes it.
      menu_slider_drag_ = false;
      menu_slider_close_ = menu_slider_moved_ || menu_slider_was_open_;
      // Whatever the last of the movement still owes is paid off first, or
      // letting go of a quick sweep would cut it short.
      menu_slider_paying_ = std::fabs(menu_slider_owed_) > 0.05f;
      menu_slider_paying_until_ = now + std::chrono::milliseconds(500);
    }
    if (menu_slider_paying_ &&
        (std::fabs(menu_slider_owed_) <= 0.05f || now > menu_slider_paying_until_ ||
         !slider_active)) {
      menu_slider_paying_ = false;
      menu_slider_owed_ = 0.0f;
    }
    if (menu_slider_close_ && !menu_slider_paying_) {
      menu_slider_close_ = false;
      mnk::MnkInputDriver::InjectButtons(0x1000, 20);   // X_INPUT_GAMEPAD_A
      mnk::MnkInputDriver::InjectPadKeystroke(0x5800);  // pad A
    }
    menu_lmb_was_down_ = lmb_now;
    menu_rmb_was_down_ = rmb_now;

    // ---- wheel ------------------------------------------------------------
    // A notch moves the selection one step. In a list (or an open dropdown)
    // that is the list's own step, done the way a pad does it so the list
    // scrolls and plays its sound; on a page of rows it is the next row that
    // can take the highlight. Pages with neither get the pad's step too.
    const int wheel = menu_wheel_.exchange(0);
    if (wheel != 0 && slider_active) {
      // A notch is one step of an open slider: up to the right, down to the
      // left, which is the pad's own step.
      mnk::MnkInputDriver::InjectStick(int16_t(wheel > 0 ? 12000 : -12000), 0, 50);
    } else if (wheel != 0) {
      bool has_rows = false;
      for (const MenuRow& row : layout.rows) has_rows = has_rows || row.focusable;
      if (focus_is_list || !has_rows) {
        menu_list_steps_ = std::clamp(menu_list_steps_ - wheel, -4, 4);
      } else {
        int at = focus_row;
        if (at < 0 || !layout.rows[size_t(at)].focusable) {
          at = hovered >= 0 ? hovered : -1;
        }
        const int step = wheel > 0 ? -1 : 1;
        int to = at;
        bool past_edge = false;
        for (int n = 0; n < std::abs(wheel); ++n) {
          int next = to < 0 ? (step > 0 ? 0 : int(layout.rows.size()) - 1) : to + step;
          while (next >= 0 && next < int(layout.rows.size()) &&
                 !layout.rows[size_t(next)].focusable) {
            next += step;
          }
          if (next < 0 || next >= int(layout.rows.size())) {
            past_edge = true;
            break;
          }
          to = next;
        }
        if (to >= 0 && to != at) {
          WriteGuestU32(mem, layout.dialog + 0x08, layout.rows[size_t(to)].item);
        } else if (past_edge && to == at) {
          // Nothing further on screen: a page that scrolls has more rows out
          // of sight, so let the game take the step - it scrolls to show it
          // (and wraps round, as it does for the pad).
          menu_list_steps_ = std::clamp(menu_list_steps_ - wheel, -4, 4);
        }
      }
    }

    menu_hover_row_ = hovered;
    menu_layout_ = layout;
  }

  // TEMP_MENU_MOUSE: dump the menu system's own state for one player's menu.
  // The layout is the N64 game's (see the decompilation): a menu holds ten
  // dialogs of 0x70 bytes, then numdialogs (+0x460), depth (+0x4F4), a pointer
  // to the open dialog (+0x4F8), the rows it laid out (+0x4FC, 4 bytes each:
  // height, item index, block index) up to rowend (+0x65C), and the columns
  // (+0x660, 8 bytes each: width, height, unk, rowstart, numrows) up to colend
  // (+0x6D8). A dialog carries its box at +0x14 (x, y, width, height) and the
  // item under the cursor at +0x08.
  static void DumpMenuState(uint32_t base) {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();

    const int32_t numdialogs = int16_t(ReadGuestU32(mem, base + 0x460) >> 16);
    const int32_t depth = int16_t(ReadGuestU32(mem, base + 0x4F4) >> 16);
    const uint32_t curdialog = ReadGuestU32(mem, base + 0x4F8);
    const int32_t rowend = int32_t(ReadGuestU32(mem, base + 0x65C));
    const int32_t colend = int32_t(ReadGuestU32(mem, base + 0x6D8));
    REXLOG_INFO("[menudump] 0x{:08X}: {} dialog(s), depth {}, open 0x{:08X}, {} row(s), {} col(s)",
                base, numdialogs, depth, curdialog, rowend, colend);
    if (curdialog < 0x82000000 || curdialog >= 0x82800000) return;

    REXLOG_INFO("[menudump]   dialog: def 0x{:08X}, focus 0x{:08X}, box x {} y {} w {} h {}, "
                "content {}x{}, scroll {}",
                ReadGuestU32(mem, curdialog + 0x00), ReadGuestU32(mem, curdialog + 0x08),
                int32_t(ReadGuestU32(mem, curdialog + 0x14)),
                int32_t(ReadGuestU32(mem, curdialog + 0x18)),
                int32_t(ReadGuestU32(mem, curdialog + 0x1C)),
                int32_t(ReadGuestU32(mem, curdialog + 0x20)),
                int32_t(ReadGuestU32(mem, curdialog + 0x24)),
                int32_t(ReadGuestU32(mem, curdialog + 0x28)),
                int32_t(ReadGuestU32(mem, curdialog + 0x64)));

    std::string rows;
    for (int i = 0; i < rowend && i < 24; ++i) {
      const uint32_t w = ReadGuestU32(mem, base + 0x4FC + uint32_t(i) * 4);
      rows += fmt::format(" [{}] h{} item{}", i, int16_t(w >> 16), (w >> 8) & 0xFF);
    }
    REXLOG_INFO("[menudump]   rows:{}", rows);

    std::string cols;
    for (int i = 0; i < colend && i < 8; ++i) {
      const uint32_t a = base + 0x660 + uint32_t(i) * 8;
      const uint32_t w0 = ReadGuestU32(mem, a);
      const uint32_t w1 = ReadGuestU32(mem, a + 4);
      cols += fmt::format(" [{}] w{} h{} rowstart{} numrows{}", i, int16_t(w0 >> 16),
                          int16_t(w0 & 0xFFFF), (w1 >> 8) & 0xFFFF, w1 & 0xFF);
    }
    REXLOG_INFO("[menudump]   cols:{}", cols);
  }

  // TEMP_MENU_MOUSE: raw words, eight to a line, so the shape of the menu
  // block can be read out of the log.
  static void DumpMenuRaw(uint32_t from, uint32_t to) {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    for (uint32_t a = from; a < to; a += 32) {
      std::string line;
      for (int i = 0; i < 8; ++i) {
        line += fmt::format(" {:08X}", ReadGuestU32(mem, a + uint32_t(i) * 4));
      }
      REXLOG_INFO("[menuraw] {:08X}:{}", a, line);
    }
  }

  // TEMP_TID_SCAN: the text bank is a run of strings in load order, so a
  // string's id can be counted off a known one ("Show Crouch" = 0x8D47).
  // TEMP_TID_SCAN: find the slots that point at two known strings; the gap
  // between them is the gap between their text ids, so the unknown one can be
  // read off the known "Show Crouch" = 0x8D47.
  // TEMP_TID_SCAN: every place the prompt's text id (0x8C66) is stored, with
  // the words around it, to find the table the footer buttons come from.
  static void DebugFindMenuState() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    MenuLayout layout = {0, 0.0f, 0.0f, 0.0f, 0.0f, false, false, {}};
    if (!MenuMouseLayout(mem, &layout)) {
      REXLOG_INFO("[menumouse] no menu open (stage {}, depth {}, dialog 0x{:08X}, rows {})",
                  ReadGuestU32(mem, kStageNum),
                  int16_t(ReadGuestU32(mem, kMenuDepth) >> 16), ReadGuestU32(mem, kMenuCurDialog),
                  int32_t(ReadGuestU32(mem, kMenuRowEnd)));
      return;
    }
    const uint32_t focus = ReadGuestU32(mem, layout.dialog + 0x08);
    REXLOG_INFO("[menumouse] pointer {:.0f},{:.0f}, stage {}", menu_cursor_x_, menu_cursor_y_,
                ReadGuestU32(mem, kStageNum));
    {  // which page this is, for looking it up in the game's own tables
      const uint32_t def = ReadGuestU32(mem, layout.dialog);
      REXLOG_INFO("[menumouse] def 0x{:08X} title {:04X} items 0x{:08X} flags {:08X}", def,
                  ReadGuestU32(mem, def + 4), ReadGuestU32(mem, def + 8),
                  ReadGuestU32(mem, def + 16));
    }
    REXLOG_INFO("[menumouse] dialog 0x{:08X} box {} {} {}x{}, {} row(s), focus 0x{:08X}{}{}",
                layout.dialog, layout.x, layout.y, layout.width, layout.height,
                layout.rows.size(), focus, layout.tabs ? ", tabs" : "",
                layout.dimmed ? ", dimmed" : "");
    for (const MenuRow& row : layout.rows) {
      std::string list_info;
      MenuListState list;
      if (MenuReadList(mem, row, &list)) {
        list_info = fmt::format(" list cur {} index {} target {} view {}", list.curoffsety,
                                list.index, list.targetoffsety, list.viewheight);
      }
      REXLOG_INFO("[menumouse]   x {:.0f}..{:.0f} y {:.0f}..{:.0f} item 0x{:08X} type {:02X} "
                  "block {}{}{}{}", row.left, row.right, row.top, row.bottom, row.item,
                  ReadGuestU32(mem, row.item) >> 24, row.block, row.focusable ? "" : " (no focus)",
                  list_info, row.item == focus ? "  <- focus" : "");
    }
  }

  static void DebugDumpStringTable() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[str_table] runtime/memory not ready");
      return;
    }
    if (last_scan_ptr_addrs_.empty()) {
      REXLOG_INFO("[str_table] No cached pointer hits - press F10 first, then F1.");
      return;
    }
    auto* mem = app->runtime()->memory();
    uint32_t anchor = *std::min_element(last_scan_ptr_addrs_.begin(), last_scan_ptr_addrs_.end());
    anchor -= (anchor % 4);

    uint32_t table_start = anchor;
    while (LooksLikeAsciiStringPointer(mem, ReadGuestU32(mem, table_start - 4))) {
      table_start -= 4;
    }
    uint32_t table_end = anchor;
    while (LooksLikeAsciiStringPointer(mem, ReadGuestU32(mem, table_end))) {
      table_end += 4;
    }

    int count = static_cast<int>((table_end - table_start) / 4);
    REXLOG_INFO(
        "[str_table] Table spans 0x{:08X}..0x{:08X} ({} entries), anchored from the last F10 "
        "scan's pointer hits:",
        table_start, table_end, count);
    for (uint32_t addr = table_start, idx = 0; addr < table_end; addr += 4, ++idx) {
      uint32_t str_ptr = ReadGuestU32(mem, addr);
      char buf[256];
      uint8_t* p = mem->TranslateVirtual<uint8_t*>(str_ptr);
      size_t n = 0;
      for (; n < sizeof(buf) - 1 && p[n] != 0 && p[n] != '\n'; ++n) buf[n] = static_cast<char>(p[n]);
      buf[n] = 0;
      REXLOG_INFO("[str_table]   [{}] slot=0x{:08X} -> 0x{:08X} \"{}\"", idx, addr, str_ptr, buf);
    }
    REXLOG_INFO("[str_table] Done - {} entries.", count);

    // TEMP_MENU_SCAN: now the real target - find whatever per-item struct(s)
    // reference these table entries, so their handler field (see the N64
    // struct menuitem's trailing function pointer) can be located. A menu
    // item could plausibly store either (a) a copy of the string pointer
    // itself, or (b) a pointer to the table slot (&table[index]) which it
    // dereferences each frame. Search all of memory for both, excluding the
    // table's own memory (every slot trivially "points to" its own string -
    // that's not an external reference, just the table being the table).
    std::unordered_map<uint32_t, uint32_t> strptr_to_slot;   // string addr -> slot addr
    std::unordered_map<uint32_t, uint32_t> slotaddr_to_slot; // slot addr -> slot addr (self)
    for (uint32_t addr = table_start; addr < table_end; addr += 4) {
      uint32_t str_ptr = ReadGuestU32(mem, addr);
      strptr_to_slot[str_ptr] = addr;
      slotaddr_to_slot[addr] = addr;
    }

    const uint32_t kImageStart = 0x82000000;
    const uint32_t kImageEnd = 0x826E0000;
    int strptr_hits = 0, slotptr_hits = 0;
    ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
      // Skip the table's own backing region entirely - see note above.
      bool overlaps_table = region_addr < table_end && region_addr + region_size > table_start;
      uint8_t* host = mem->TranslateVirtual<uint8_t*>(region_addr);
      for (size_t off = 0; off + 4 <= region_size; off += 4) {
        uint32_t addr = region_addr + static_cast<uint32_t>(off);
        if (overlaps_table && addr >= table_start && addr < table_end) continue;
        uint32_t value = (uint32_t(host[off]) << 24) | (uint32_t(host[off + 1]) << 16) |
                         (uint32_t(host[off + 2]) << 8) | uint32_t(host[off + 3]);
        bool in_image = addr >= kImageStart && addr < kImageEnd;
        if (auto it = strptr_to_slot.find(value); it != strptr_to_slot.end()) {
          uint32_t idx = (it->second - table_start) / 4;
          REXLOG_INFO(
              "[str_table] {} STRPTR-REF at 0x{:08X} -> table[{}] (slot 0x{:08X})",
              in_image ? "IMAGE" : "heap ", addr, idx, it->second);
          strptr_hits++;
        }
        if (auto it = slotaddr_to_slot.find(value); it != slotaddr_to_slot.end()) {
          uint32_t idx = (it->second - table_start) / 4;
          REXLOG_INFO(
              "[str_table] {} SLOTPTR-REF at 0x{:08X} -> table[{}] (slot 0x{:08X})",
              in_image ? "IMAGE" : "heap ", addr, idx, it->second);
          slotptr_hits++;
        }
      }
    });
    REXLOG_INFO("[str_table] Reverse-pointer search done: {} string-pointer refs, {} slot-pointer refs.",
                strptr_hits, slotptr_hits);
  }

  // TEMP_MENU_SCAN: two-stage F11 snapshot/diff to find the exact guest byte
  // backing "2-Player Screen Split" (or any other live toggle). Usage: (1)
  // F11 on the Video Options screen -> captures a baseline. (2) Toggle the
  // option in-game, then F11 again -> diffs against the baseline. The
  // game's animated menu backdrop touches huge swaths of memory every
  // frame (skinned animation, physics, timers), so a raw byte diff is
  // useless (millions of hits) - instead this filters for the one pattern
  // an enum-like state byte actually produces: old value and new value
  // both small (<=3) and different from each other. That collapses the
  // noise to a short, actionable list. Once the real address is found, the
  // recompiled source can be grepped for its hex value to find the actual
  // getter/setter/handler guest functions.
  // TEMP_MENU_SCAN: F2 arms a write-watch on the two exact stack slots the
  // string-table reverse-pointer search (F1) found repeatedly refreshed
  // while rendering the "2-Player Screen Split" row: 0x7018EBDC (copy of
  // the row-label pointer, table[125]) and 0x7018EBE4 (copy of the current
  // *value* label pointer - table[132]="Horizontal" or table[133]="Vertical"
  // depending on state). These are confirmed transient per-frame scratch,
  // not the real persistent state, but watching them narrowly (unlike the
  // whole-page/whole-image attempts that flooded or crashed) should reveal
  // *which function* writes the current value there each frame - then a few
  // lines before that store, in the generated source, should show where it
  // reads the value index/pointer from, which is the actual target. Empirically
  // stable across the last several relaunches, but if it ever comes up empty,
  // re-run F10+F1 first to get fresh addresses for the current process.
  static void ArmWriteWatchOnScreenSplitValueSlot() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[menu_watch] runtime/memory not ready");
      return;
    }
    std::vector<uint32_t> targets = {0x7018EBDC, 0x7018EBE4};
    pd_watch::ArmMemoryWriteWatch(app->runtime()->memory(), targets);
    REXLOG_INFO(
        "[menu_watch] Armed on the 2-Player Screen Split value-label stack slots. Toggle the "
        "option in-game, then F6.");
  }

  // TEMP_MENU_SCAN: F8 arms a write-watch (see pd_watch namespace above) on
  // whatever addresses the last F10 scan found - the string-table pointer
  // slots and the raw string bytes. Toggle the option in-game afterward,
  // then press F6 to see which guest function(s) wrote there and disarm.
  static void ArmWriteWatchOnLastScan() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[menu_watch] runtime/memory not ready");
      return;
    }
    if (last_scan_ptr_addrs_.empty() && last_scan_string_addrs_.empty()) {
      REXLOG_INFO(
          "[menu_watch] No cached addresses - press F10 first while on the Video Options "
          "screen, then F8.");
      return;
    }
    std::vector<uint32_t> targets;
    targets.insert(targets.end(), last_scan_ptr_addrs_.begin(), last_scan_ptr_addrs_.end());
    targets.insert(targets.end(), last_scan_string_addrs_.begin(), last_scan_string_addrs_.end());
    pd_watch::ArmMemoryWriteWatch(app->runtime()->memory(), targets);
    REXLOG_INFO(
        "[menu_watch] Armed on {} addresses ({} pointer slots + {} string bytes). Toggle the "
        "option in-game, then press F6.",
        targets.size(), last_scan_ptr_addrs_.size(), last_scan_string_addrs_.size());
  }

  static void DisarmWriteWatchAndReport() { pd_watch::DisarmMemoryWriteWatch(); }

  // TEMP_MENU_SCAN: F7 arms a write-watch on the game's static image
  // (0x82000000-0x826E0000: code+rodata+data+bss) instead of the transient
  // UI rendering scratch heap that F8 watches. Real persistent state (the
  // actual current screen-split value) is expected to live here. Toggle
  // the option in-game, then F6 to see results (same disarm as F8's).
  static void ArmWriteWatchOnImage() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[menu_watch] runtime/memory not ready");
      return;
    }
    pd_watch::ArmMemoryWriteWatchRange(app->runtime()->memory(), 0x82000000, 0x826E0000);
    REXLOG_INFO("[menu_watch] Armed on the static image. Toggle the option in-game, then F6.");
  }

  // TEMP_MENU_SCAN: F1 arms a write-watch on a narrow range (one guest page)
  // covering both the sub_82343458 r3 (0x4005F1B0) and computed-r11
  // (0x4005F604) candidates - both were proved byte-identical between
  // Horizontal/Vertical via direct dumps (F5/F2), so neither is the real
  // state; this catches whatever address the toggle actually writes to,
  // if it's nearby. Toggle the option in-game, then F6 to see results.
  static void ArmWriteWatchOnItemStruct() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[menu_watch] runtime/memory not ready");
      return;
    }
    pd_watch::ArmMemoryWriteWatchRange(app->runtime()->memory(), 0x4005F000, 0x40060000);
    REXLOG_INFO(
        "[menu_watch] Armed on 0x4005F000-0x40060000 (item struct page). Toggle the option "
        "in-game, then F6.");
  }

  static void DebugSnapshotOrDiffMemory() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[menu_diff] runtime/memory not ready");
      return;
    }
    auto* mem = app->runtime()->memory();

    static int stage = 0;
    static std::vector<std::pair<uint32_t, std::vector<uint8_t>>> snapshot;

    // Narrowed to the UI/menu heap arena where the string table (see
    // DebugScanForMenuStrings) actually lives - the 0x400C4xxx-0x401CBxxx
    // range we've observed it in across runs. Restricting to this ~32MB
    // window instead of the full address space cuts out essentially all
    // gameplay/animation noise (character skinning, physics, audio), which
    // lives in an entirely different heap region and was the reason the
    // full-range diff produced hundreds of thousands of irrelevant hits.
    const uint32_t kScanStart = 0x40000000;
    const uint32_t kScanEnd = 0x42000000u;
    auto for_each_committed_region = [&](auto&& visit) {
      uint32_t addr = kScanStart;
      while (addr < kScanEnd) {
        auto* heap = mem->LookupHeap(addr);
        if (!heap) {
          addr += 0x10000;
          continue;
        }
        rex::memory::HeapAllocationInfo info{};
        if (!heap->QueryRegionInfo(addr, &info) || info.region_size == 0) {
          addr += 0x10000;
          continue;
        }
        bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
        bool readable = (info.protect & rex::memory::kMemoryProtectRead) != 0;
        if (committed && readable) {
          visit(addr, info.region_size);
        }
        addr += info.region_size;
      }
    };

    if (stage == 0) {
      snapshot.clear();
      for_each_committed_region([&](uint32_t addr, uint32_t size) {
        uint8_t* host = mem->TranslateVirtual<uint8_t*>(addr);
        snapshot.emplace_back(addr, std::vector<uint8_t>(host, host + size));
      });
      stage = 1;
      REXLOG_INFO(
          "[menu_diff] Baseline captured ({} regions). Now TOGGLE the option in-game, then "
          "press F11 again.",
          snapshot.size());
    } else {
      std::vector<std::tuple<uint32_t, uint8_t, uint8_t>> candidates;
      for (const auto& region : snapshot) {
        uint32_t addr = region.first;
        const auto& bytes = region.second;
        auto* heap = mem->LookupHeap(addr);
        if (!heap) continue;
        rex::memory::HeapAllocationInfo info{};
        if (!heap->QueryRegionInfo(addr, &info)) continue;
        bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
        bool readable = (info.protect & rex::memory::kMemoryProtectRead) != 0;
        if (!committed || !readable) continue;
        size_t n = std::min<size_t>(bytes.size(), info.region_size);
        uint8_t* host = mem->TranslateVirtual<uint8_t*>(addr);
        for (size_t i = 0; i < n; ++i) {
          if (host[i] != bytes[i]) {
            candidates.emplace_back(addr + static_cast<uint32_t>(i), bytes[i], host[i]);
          }
        }
      }
      REXLOG_INFO(
          "[menu_diff] {} total byte diffs in the narrowed 0x40000000-0x42000000 range:",
          candidates.size());
      int logged = 0;
      for (const auto& [a, old_v, new_v] : candidates) {
        if (logged >= 300) {
          REXLOG_INFO("[menu_diff]   ... {} more not shown.", candidates.size() - logged);
          break;
        }
        REXLOG_INFO("[menu_diff]   DIFF at 0x{:08X}: {} -> {}", a, old_v, new_v);
        logged++;
      }
      snapshot.clear();
      snapshot.shrink_to_fit();
      stage = 0;
    }
  }

  // TEMP_MENU_SCAN: F12 repurposed - search all committed guest memory for
  // the literal filename "XStr" / "XStr_Original" / ".cmp" (the likely
  // on-disk resource behind the heap-resident UI string table found by
  // DebugScanForMenuStrings - see HANDOVER.md). If the guest code opens this
  // file by name (rather than a precomputed file ID/hash), the filename
  // bytes should be sitting in memory near whatever function loads and
  // decompresses it, and a hex dump around the hit may reveal that
  // function's calling context (return address on the stack, or a
  // neighboring file-table struct).
  // TEMP_MENU_SCAN: resolution table shared with the F9 panel (kRowResolution)
  // so the native menu row and the overlay stay in lockstep. Scale rises with
  // resolution, which is what makes the internal render target sharpen as you
  // go up - applied live, no restart.
  static void ApplyPcResolutionIndex(int32_t index) {
    static const int32_t kW[] = {1280, 1920, 2560, 3840};
    static const int32_t kH[] = {720, 1080, 1440, 2160};
    if (index < 0 || index > 3) return;

    // Deliberately does NOT drive resolution_scale off the resolution.
    //
    // Changing resolution_scale forces GraphicsSystem::ReinitializeContext(),
    // a full GPU teardown (the scale is baked into the texture cache at
    // construction, so there is no live path for it). Live-rendered content
    // survives that fine, but the menu backdrop that is already on screen goes
    // black until the menu is re-entered - which is exactly when someone is
    // changing resolution. Keeping the scale out of this path means the four
    // resolutions switch instantly with no artifact at all.
    //
    // resolution_scale remains a separate setting, read once at startup from
    // perfect_dark.toml, so the internal render resolution is whatever the user
    // picked and never changes underneath a running frame.
    const std::string scale = rex::cvar::GetFlagByName("resolution_scale");
    SetPcResolution(kW[index], kH[index], scale.empty() ? 1 : std::atoi(scale.c_str()));
  }

  // TEMP_MENU_SCAN: guest addresses of the strings we write into writable
  // scratch. All are "text\n\0" to match the game's own format, and each slot
  // is 16 bytes so nothing here may exceed 15 characters.
  static constexpr uint32_t kResOptionStrings = 0x82675000;  // 4 x 16
  static constexpr uint32_t kStrWindowed = 0x82675040;
  static constexpr uint32_t kStrFullscreen = 0x82675050;
  static constexpr uint32_t kStrOff = 0x82675060;
  static constexpr uint32_t kStrOn = 0x82675070;
  static constexpr uint32_t kLblResolution = 0x82675080;
  static constexpr uint32_t kLblGameDisplay = 0x82675090;
  static constexpr uint32_t kLblVsync = 0x826750A0;
  static constexpr uint32_t kLblShowFps = 0x826750B0;
  // 32-byte slots from here on - these labels exceed 15 characters.
  static constexpr uint32_t kLblGamepadControls = 0x826750C0;
  static constexpr uint32_t kLblPcControls = 0x826750E0;
  static constexpr uint32_t kLblMouseSens = 0x82675100;
  static constexpr uint32_t kLblInvertLook = 0x82675120;
  static constexpr uint32_t kSensOptionStrings = 0x82675140;  // 5 x 16
  // Render Scale row: its label, the three values (1x 2x 3x) as shown for the
  // scale this run renders at, and the same marked "(requires restart)" for
  // the others (32-byte slots, after "Press ANY KEY To Play" at 0x826752D0).
  static constexpr uint32_t kLblRenderScale = 0x82675200;
  static constexpr uint32_t kRenderScaleStrings = 0x82675210;         // 3 x 16
  static constexpr uint32_t kRenderScaleRestartStrings = 0x826752F0;  // 3 x 32
  // Upscaling row (HANDOVER 4an): its label and three values.
  static constexpr uint32_t kLblUpscaling = 0x82675290;
  static constexpr uint32_t kUpscalingStrings = 0x826752A0;  // 3 x 16
  // Keyboard binding rows: label and value strings, 32-byte slots so a long
  // action name or a two-key bind ("2,WheelUp") fits.
  static constexpr uint32_t kBindRowStride = 32;
  static constexpr int kBindRowSlots = 24;
  // Guest heap blocks, allocated once by the menu patch before any row exists.
  static inline std::atomic<uint32_t> bind_label_strings_{0};
  static inline std::atomic<uint32_t> bind_value_strings_{0};
  static uint32_t BindLabelStrings() { return bind_label_strings_.load(); }
  static uint32_t BindValueStrings() { return bind_value_strings_.load(); }

  // TEMP_MENU_SCAN: a menuitem's param2 is either a text ID or a function that
  // returns the label as a char* - the stock "2-Player Screen Split" row uses
  // the latter (0x821AE370 is `li r3,0x56DA; b <lang_get>`). Using thunks lets
  // a row carry an arbitrary label without touching the game's string table.
  static void PdLabelResolution(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kLblResolution; }
  static void PdLabelGameDisplay(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kLblGameDisplay; }
  static void PdLabelVsync(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kLblVsync; }
  static void PdLabelShowFps(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kLblShowFps; }
  static void PdLabelRenderScale(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kLblRenderScale; }
  static void PdLabelUpscaling(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kLblUpscaling; }
  static void PdLabelGamepadControls(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = kLblGamepadControls;
  }
  static void PdLabelPcControls(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kLblPcControls; }
  static void PdLabelMouseSens(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kLblMouseSens; }
  static void PdLabelInvertLook(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kLblInvertLook; }

  // TEMP_MENU_SCAN: shared body for the two-state (Off/On style) rows. Same op
  // protocol as PdResolutionMenuHandler - see the comment there.
  static void PdTwoStateHandler(PPCContext& ctx, uint32_t off_str, uint32_t on_str,
                                bool (*get)(), void (*set)(bool)) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    uint32_t ret = 0;
    switch (op) {
      case 1:
        if (mem) WriteGuestU32(mem, data, 2);
        break;
      case 3: {
        uint32_t i = mem ? ReadGuestU32(mem, data) : 0;
        ret = (i == 1) ? on_str : off_str;
        break;
      }
      case 6: {
        uint32_t i = mem ? ReadGuestU32(mem, data) : 0;
        set(i == 1);
        break;
      }
      case 7:
        if (mem) WriteGuestU32(mem, data, get() ? 1u : 0u);
        break;
      default:
        break;
    }
    ctx.r3.u64 = ret;
  }

  // TEMP_MENU_SCAN: PC Controls rows. mnk_sensitivity is a double cvar in the
  // SDK's MnK driver; we expose five presets rather than a free slider because
  // the native menu only does discrete option lists.
  static const double* SensValues() {
    static const double kV[5] = {0.5, 1.0, 1.5, 2.0, 3.0};
    return kV;
  }

  static int32_t GetMouseSensIndex() {
    std::string v = rex::cvar::GetFlagByName("mnk_sensitivity");
    double d = v.empty() ? 1.0 : atof(v.c_str());
    const double* vals = SensValues();
    int32_t best = 1;
    double best_err = 1e9;
    for (int32_t i = 0; i < 5; ++i) {
      double err = d > vals[i] ? d - vals[i] : vals[i] - d;
      if (err < best_err) {
        best_err = err;
        best = i;
      }
    }
    return best;
  }

  // Combat Simulator > Match Type lists Local Multiplayer, Xbox LIVE Private
  // Match and Xbox LIVE Quick Match (text ids 0x8CC2, 0x8CC1, 0x8CC0; indices
  // 0, 1, 2). Quick Match needs Microsoft's matchmaking service, so its slot is
  // reused: TickLobbyLabels renames the entries Host / Find Online Match and
  // this wrapper around the row's handler (sub_8227E9A0) gives Find its
  // behaviour - it starts a search and opens the server browser dialog (see
  // "Server browser" below); the Match Type itself stays as it was. The
  // handler's ops: 1 count, 3 label for the index at [r5] (char* in r3),
  // 6 set the index at [r5], 7 current index. Host and Find need online play
  // (net_private_match); without it a banner says how to turn it on, instead
  // of the game's "Connection to Xbox LIVE has been lost".
  static constexpr uint32_t kMatchTypeItem = 0x825FA244;
  static constexpr uint32_t kMatchTypeHandler = 0x8227E9A0;
  static constexpr uint32_t kMatchTypeLabelTid = 0x8CBF;
  static constexpr uint32_t kMatchTypeHost = 1;
  static constexpr uint32_t kMatchTypeJoin = 2;
  // "Joined Online Match" - index 1's label while this PC is a guest.
  static constexpr uint32_t kStrJoinedMatch = 0x82675480;

  // ---- Server browser (Match Type > Find Online Match) ---------------------
  // A native dialog like PC Controls, titled with the Quick Match tid (0x8CC0,
  // renamed "Find Online Match"). One spinner row per match found - its label
  // the host's match, its value mode, arena, players and ping - then a
  // Refresh row. Pressing a row opens the game's own dropdown: option 0 is
  // the details (what the row shows), option 1 "Join this match" /
  // "Search again". Empty match rows answer op 24 (the N64 menu's
  // MENUOP_CHECKHIDDEN) so only real matches show; row 0 says "Searching..."
  // or "No online matches found" when there are none. The dialog is pushed
  // with sub_821C2838 (menuPushDialog, what a row with the opens-a-dialog
  // flag 0x04 calls) one frame after Find is picked, once the dropdown list
  // has closed. Its items, strings and a scratch word live in a guest heap
  // block (SystemHeapAlloc), clear of the item-array scratch.
  static constexpr int kBrowserRows = 8;             // match rows; row 8 is Refresh
  static constexpr uint32_t kBrowserStride = 160;    // one line of row text
  static inline uint32_t browser_dialog_ = 0;
  static inline uint32_t browser_strings_ = 0;
  static inline uint32_t browser_word_ = 0;
  static inline std::atomic<bool> browser_open_pending_{false};

  static void WriteMenuText(rex::memory::Memory* mem, uint32_t addr, const std::string& text,
                            size_t cap) {
    std::string s = text.substr(0, cap - 2);
    s += "\n";
    WriteGuestBytes(mem, addr, s.c_str(), s.size() + 1);
  }

  // What a match row shows beside its name: players and ping, short enough
  // not to run into the name.
  static std::string BrowserSummary(const rex::kernel::xam::netplay::BrowserMatch& m) {
    std::string text;
    if (m.players > 0) {
      text = std::to_string(m.players) +
             (m.max_players > 0 ? "/" + std::to_string(m.max_players) : "") + "  ";
    }
    return text + std::to_string(m.ping_ms) + " ms";
  }

  // The dropdown's middle line: mode and arena (and whether it has started).
  static std::string BrowserDetails(const rex::kernel::xam::netplay::BrowserMatch& m) {
    std::string text = m.mode.empty() ? std::string("Match") : m.mode;
    if (!m.arena.empty()) text += " on " + m.arena;
    if (m.in_progress) text += ", playing";
    return text;
  }

  // Formats a row's single line into its guest buffer and returns the address.
  // A match row reads "Alpha's match   Combat on Skedar   2/8  12 ms".
  static uint32_t BrowserString(int row) {
    namespace netplay = rex::kernel::xam::netplay;
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    if (!mem || !browser_strings_) return 0;
    const uint32_t addr = browser_strings_ + uint32_t(row) * kBrowserStride;
    const auto matches = netplay::BrowseResults();
    const bool searching = netplay::BrowseSearching();
    std::string text;
    if (row == kBrowserRows) {
      text = searching ? "Refresh   (searching...)"
                       : "Refresh   (" + std::to_string(matches.size()) +
                             (matches.size() == 1 ? " match)" : " matches)");
    } else if (row < int(matches.size())) {
      const auto& m = matches[size_t(row)];
      text = m.name + "'s match   " + BrowserDetails(m) + "   " + BrowserSummary(m);
    } else if (row == 0) {
      text = searching ? "Searching your network..."
                       : "No games found - Refresh, or use Direct Connect";
    }
    WriteMenuText(mem, addr, text, kBrowserStride);
    return addr;
  }

  // Joins browser result `row`: the invite first (so the lobby opened next
  // stays quiet), then the online lobby via Match Type's Private Match slot,
  // exactly as the old Join did, and the game takes the invite from there.
  static void BrowserJoin(PPCContext& ctx, uint8_t* base, int row) {
    namespace netplay = rex::kernel::xam::netplay;
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    if (!mem || !browser_word_ || !netplay::BrowseJoin(size_t(row), 0)) return;
    WriteGuestU32(mem, browser_word_, kMatchTypeHost);
    ctx.r3.u64 = 6;
    ctx.r4.u64 = kMatchTypeItem;
    ctx.r5.u64 = browser_word_;
    sub_8227E9A0(ctx, base);
  }

  // Rows are plain selectable items (type 0x04): one press joins a match or
  // searches again. Op 6 is the press; op 24 asks whether the row is hidden.
  static void PdBrowserRowImpl(PPCContext& ctx, uint8_t* base, int row) {
    namespace netplay = rex::kernel::xam::netplay;
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    if (!mem) {
      ctx.r3.u64 = 0;
      return;
    }
    const bool refresh = row == kBrowserRows;
    const bool match = !refresh && row < int(netplay::BrowseResults().size());
    if (op == 6) {
      if (refresh) {
        netplay::BrowseRefresh();
      } else if (match) {
        BrowserJoin(ctx, base, row);
      }
      ctx.r3.u64 = 0;
      return;
    }
    if (op == 24) {  // hidden: unused match rows (row 0 carries the status instead)
      ctx.r3.u64 = (!match && !refresh && row > 0) ? 1 : 0;
      return;
    }
    ctx.r3.u64 = 0;
  }

  template <int N>
  static void PdBrowserRowLabel(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = BrowserString(N);
  }
  template <int N>
  static void PdBrowserRowHandler(PPCContext& ctx, uint8_t* base) {
    PdBrowserRowImpl(ctx, base, N);
  }

  // ---- Achievements list (main menu > Achievements) ------------------------
  // The stock item calls XamShowAchievementsUI, the Xbox dashboard's own
  // viewer, which this port has no equivalent of - so the item did nothing.
  // It now opens a native dialog listing the title's achievements from the
  // runtime's store (which already records unlocks): the row is the name,
  // its value "Unlocked 25G" or "Locked 25G", and its dropdown adds the
  // description. F7 still opens the SDK's own overlay.
  static constexpr int kAchievementRows = 24;
  static constexpr uint32_t kAchievementStride = 160;  // name 48, status 32, description 80
  static inline uint32_t achievements_dialog_ = 0;
  static inline uint32_t pc_controls_dialog_ = 0;  // Settings > Keyboard & Mouse
  // Player Setup's rebuilt rows (0 until they are built).
  static inline uint32_t setup_items_ = 0;
  static inline uint32_t achievements_strings_ = 0;

  // Re-read twice a second, so an unlock during play shows up without asking
  // the store on every menu frame.
  static const std::vector<rex::system::AchievementInfo>& AchievementList() {
    static std::vector<rex::system::AchievementInfo> cache;
    static uint64_t refreshed_ms = 0;
    const uint64_t now = NowMs();
    if (cache.empty() || now - refreshed_ms > 500) {
      auto* kernel_state = REX_KERNEL_STATE();
      if (kernel_state) cache = kernel_state->achievements().ListAchievements();
      refreshed_ms = now;
    }
    return cache;
  }

  // which: 0 name, 1 status (option 0), 2 description (option 1).
  static uint32_t AchievementString(int row, int which) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    if (!mem || !achievements_strings_) return 0;
    static constexpr uint32_t kOffset[3] = {0, 48, 80};
    static constexpr size_t kCap[3] = {48, 32, 80};
    const uint32_t addr =
        achievements_strings_ + uint32_t(row) * kAchievementStride + kOffset[which];
    const auto& list = AchievementList();
    auto* kernel_state = REX_KERNEL_STATE();
    std::string text;
    if (row < int(list.size())) {
      const auto& a = list[size_t(row)];
      const bool unlocked = kernel_state && kernel_state->achievements().IsUnlocked(a.id);
      if (which == 0) text = a.label;
      if (which == 1) {
        text = (unlocked ? "Unlocked  " : "Locked  ") + std::to_string(a.gamerscore) + "G";
      }
      if (which == 2) {
        text = (unlocked || a.unachieved_description.empty()) ? a.description
                                                              : a.unachieved_description;
      }
    } else if (row == 0 && which == 0) {
      text = "No achievements loaded";
    }
    WriteMenuText(mem, addr, text, kCap[which]);
    return addr;
  }

  static void PdAchievementRowImpl(PPCContext& ctx, int row) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    if (!mem) {
      ctx.r3.u64 = 0;
      return;
    }
    const bool real = row < int(AchievementList().size());
    switch (op) {
      case 1:  // status, then description
        if (data) WriteGuestU32(mem, data, real ? 2 : 1);
        ctx.r3.u64 = 0;
        return;
      case 3: {
        const uint32_t index = data ? ReadGuestU32(mem, data) : 0;
        ctx.r3.u64 = AchievementString(row, index == 1 ? 2 : 1);
        return;
      }
      case 7:
        if (data) WriteGuestU32(mem, data, 0);
        ctx.r3.u64 = 0;
        return;
      case 24:  // hidden: rows past the last achievement
        ctx.r3.u64 = (!real && row > 0) ? 1 : 0;
        return;
      default:
        ctx.r3.u64 = 0;
        return;
    }
  }

  template <int N>
  static void PdAchievementRowLabel(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = AchievementString(N, 0);
  }
  template <int N>
  static void PdAchievementRowHandler(PPCContext& ctx, uint8_t*) {
    PdAchievementRowImpl(ctx, N);
  }
  template <std::size_t... I>
  static void FillAchievementThunks(std::index_sequence<I...>, PPCFunc** labels,
                                    PPCFunc** handlers) {
    ((labels[I] = &PdAchievementRowLabel<int(I)>,
      handlers[I] = &PdAchievementRowHandler<int(I)>),
     ...);
  }

  // ---- Your Address (the host's lobby, and Settings > Online) ---------------
  // Where a host's game can be reached, so they can tell friends. Each of this
  // PC's addresses is named by its network (LAN, Wi-Fi, Hamachi, ZeroTier...,
  // netplay::LocalAddresses). Closed, the row shows the first; its dropdown
  // lists them all, and picking one copies it to the clipboard.
  //
  // In the Combat Simulator lobby the row reuses item 0x15 of the lobby's item
  // array: one of six rows that only ever showed for Xbox LIVE Quick Match,
  // which the port never enters, so nothing is inserted and the game's item
  // IDs stay where they were. It is hidden except while hosting.
  static constexpr uint32_t kHostAddressItem = 0x825FA280;      // lobby row, id 0x15
  static constexpr uint32_t kHostAddressItemWord = 0x0C150000;  // what must be there
  static constexpr int kAddressSlots = 8;
  static constexpr uint32_t kAddressStride = 48;
  static inline uint32_t address_strings_ = 0;  // label (32), then kAddressSlots x 48

  static std::vector<rex::kernel::xam::netplay::LocalAddress> ShareableAddresses() {
    auto list = rex::kernel::xam::netplay::LocalAddresses();
    if (list.size() > size_t(kAddressSlots)) list.resize(kAddressSlots);
    return list;
  }

  static uint32_t AddressLabelString() {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    if (!mem || !address_strings_) return 0;
    WriteMenuText(mem, address_strings_, "Your Address", 32);
    return address_strings_;
  }

  // "LAN  192.168.1.20" for entry `index`.
  static uint32_t AddressEntryString(uint32_t index) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    if (!mem || !address_strings_) return 0;
    const auto list = ShareableAddresses();
    const uint32_t slot = std::min<uint32_t>(index, kAddressSlots - 1);
    const uint32_t addr = address_strings_ + 32 + slot * kAddressStride;
    std::string text = index < list.size() ? list[index].label + "  " + list[index].address
                                           : std::string("No network found");
    WriteMenuText(mem, addr, text, kAddressStride);
    return addr;
  }

  static void CopyToClipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
      if (void* dest = GlobalLock(block)) {
        memcpy(dest, text.c_str(), text.size() + 1);
        GlobalUnlock(block);
        if (!SetClipboardData(CF_TEXT, block)) GlobalFree(block);
      } else {
        GlobalFree(block);
      }
    }
    CloseClipboard();
  }

  // Answers the address ops for a row; false for ops the caller handles.
  static bool AddressRowOp(rex::memory::Memory* mem, PPCContext& ctx, uint32_t op, uint32_t data) {
    switch (op) {
      case 1:
        if (data) WriteGuestU32(mem, data, std::max<uint32_t>(1, uint32_t(ShareableAddresses().size())));
        ctx.r3.u64 = 0;
        return true;
      case 3:
        ctx.r3.u64 = AddressEntryString(data ? ReadGuestU32(mem, data) : 0);
        return true;
      case 6: {
        const uint32_t index = data ? ReadGuestU32(mem, data) : 0;
        const auto list = ShareableAddresses();
        if (index < list.size()) {
          CopyToClipboard(list[index].address);
          rex::kernel::xam::netplay::ShowNotice("Copied " + list[index].address +
                                                " - friends choose Join Match > Direct Connect");
        }
        ctx.r3.u64 = 0;
        return true;
      }
      case 7:
        if (data) WriteGuestU32(mem, data, 0);
        ctx.r3.u64 = 0;
        return true;
      default:
        return false;
    }
  }

  static void PdHostAddressLabel(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = AddressLabelString();
  }

  // The lobby's Your Address row is switched off (2026-09-17): players get the
  // host's address from their VPN (Radmin VPN) instead. Set true to show it
  // to the host again.
  static constexpr bool kShowHostAddressRow = false;

  static void PdHostAddressHandler(PPCContext& ctx, uint8_t*) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    if (!mem) {
      ctx.r3.u64 = 0;
      return;
    }
    if (op == 24) {  // hidden: players share addresses from Radmin VPN or similar
      ctx.r3.u64 = (kShowHostAddressRow && rex::kernel::xam::netplay::IsHostingSession()) ? 0 : 1;
      return;
    }
    if (!AddressRowOp(mem, ctx, op, data)) ctx.r3.u64 = 0;
  }

  // ---- Multiplayer (Combat Simulator > Match Type > Join Match) -------------
  // LAN and DIRECT CONNECT: two plain selectable rows (item type 0x04, the
  // game's own "press to do it" row), so one press opens the screen. The game
  // calls a row handler with op 6 when pressed, and with the hidden/disabled
  // checks (ops 24, 15) every frame, which is where pending screens open. The
  // network is only switched on by a pick, so opening this screen does not
  // touch it. ONLINE (the relay server) is hidden until one is deployed.
  //   LAN             the server browser, searching this PC's own networks -
  //                   a home network, or a virtual LAN the players run
  //                   themselves (Hamachi, ZeroTier, Radmin...).
  //   DIRECT CONNECT  type a host address; that host alone is asked, and its
  //                   match is joined through the browser's own join path.
  // Screens open one frame after the pick, once the dropdown has closed - the
  // same way Match Type opens this screen.
  static constexpr int kMultiplayerRows = 2;
  static constexpr uint32_t kMultiplayerStride = 96;  // label 32, value 64
  static inline uint32_t multiplayer_dialog_ = 0;
  static inline uint32_t multiplayer_strings_ = 0;
  static inline std::atomic<bool> multiplayer_open_pending_{false};
  static inline std::atomic<bool> address_entry_wanted_{false};
  static inline std::atomic<bool> direct_join_pending_{false};
  static inline std::string last_direct_address_;

  static uint32_t MultiplayerString(int row, int which) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    if (!mem || !multiplayer_strings_) return 0;
    static constexpr const char* kLabels[kMultiplayerRows] = {"LAN", "Direct Connect"};
    static constexpr const char* kValues[kMultiplayerRows] = {"Browse local games",
                                                              "Enter host address..."};
    const uint32_t addr = multiplayer_strings_ + uint32_t(row) * kMultiplayerStride +
                          (which == 0 ? 0 : 32);
    WriteMenuText(mem, addr, which == 0 ? kLabels[row] : kValues[row], which == 0 ? 32 : 64);
    return addr;
  }

  // Opens whichever screen a pick asked for on an earlier frame.
  static void PushPendingDialog(PPCContext& ctx, uint8_t* base) {
    uint32_t dialog = 0;
    if (multiplayer_dialog_ && multiplayer_open_pending_.exchange(false)) {
      dialog = multiplayer_dialog_;
    } else if (browser_dialog_ && browser_open_pending_.exchange(false)) {
      dialog = browser_dialog_;
    }
    if (!dialog) return;
    const uint64_t r3 = ctx.r3.u64;
    const uint64_t r4 = ctx.r4.u64;
    const uint64_t r5 = ctx.r5.u64;
    ctx.r3.u64 = dialog;
    sub_821C2838(ctx, base);  // menuPushDialog
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
  }

  static void PdMultiplayerRowImpl(PPCContext& ctx, uint8_t* base, int row) {
    namespace netplay = rex::kernel::xam::netplay;
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    if (!mem) {
      ctx.r3.u64 = 0;
      return;
    }
    if (op != 6) {
      PushPendingDialog(ctx, base);
      // Direct Connect: once the typed host has answered (or not), join it
      // exactly as picking it in the browser would.
      if (row == 0 && direct_join_pending_ && !netplay::BrowseSearching()) {
        direct_join_pending_ = false;
        if (!netplay::BrowseResults().empty()) {
          const uint64_t r3 = ctx.r3.u64;
          const uint64_t r4 = ctx.r4.u64;
          const uint64_t r5 = ctx.r5.u64;
          BrowserJoin(ctx, base, 0);
          ctx.r3.u64 = r3;
          ctx.r4.u64 = r4;
          ctx.r5.u64 = r5;
        } else {
          netplay::ShowNotice("No game answered at " + last_direct_address_ +
                              " - check the address, and that the host has UDP 1000, 1001 and 3076 open");
        }
      }
    }
    (void)data;
    if (op == 6) {
      if (row == 0) {
        netplay::SetOnlineMode(false);
        netplay::SetEnabled(true);
        netplay::BrowseRefresh();
        browser_open_pending_ = browser_dialog_ != 0;
      } else {
        RequestAddressEntry();
      }
    }
    ctx.r3.u64 = 0;
  }

  template <int N>
  static void PdMultiplayerRowLabel(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = MultiplayerString(N, 0);
  }
  template <int N>
  static void PdMultiplayerRowHandler(PPCContext& ctx, uint8_t* base) {
    PdMultiplayerRowImpl(ctx, base, N);
  }

  // ---- Online page (Help & Options > Settings > Online) --------------------
  // Player name, whether online play is on, and whether the online server can
  // be reached. Four spinner rows, built the same way as the achievements
  // list: our own strings in a guest heap block, label and handler thunks.
  //
  // Only the name needs typing, which no guest menu can do; that option hands
  // over to PdNameEntryDialog.
  // Internet play: the built-in online server (tools/relay/pd_relay.py).
  // Empty until one is deployed; a net_relay_server line in the toml or on the
  // command line wins.
  static constexpr const char* kOnlineServer = "";
  static constexpr uint32_t kOnlineTid = 0x8CB8;  // "Online", the row and title
  static constexpr int kOnlineRows = 4;
  static constexpr uint32_t kOnlineStride = 128;  // label 32, then two 48-byte values
  static inline uint32_t online_dialog_ = 0;
  static inline uint32_t match_type_scratch_ = 0;  // one guest word, see PdMatchTypeHandler
  static inline uint32_t online_strings_ = 0;
  static inline std::atomic<bool> name_entry_wanted_{false};
  static inline bool name_entry_open_ = false;
  static inline std::string name_entry_prev_mnk_;
  static inline std::atomic<bool> online_warmed_{false};

  // A name to start with, picked once per install. Without this the game would
  // fall back to the Windows account name, which is often someone's real name
  // and is shown to everyone in the lobby, on the leaderboards and online.
  // Every one of these fits the 15 character name column.
  static constexpr const char* kStarterNames[] = {
      "Agent", "Big Sim", "Little Sim", "Dim Sim", "Sim bad", "Agent Bob", "Agent Dave",
      "Agent Smith", "Big Boss", "Little Boss", "Big Man", "Little Man", "Some Guy", "The Dude",
      "The Lad", "Night Gardener", "Big Money", "Big Lad", "Old Mate", "Good Mate", "The Big L",
      "The Big D", "Link", "Big J", "Big Kev", "Big Al", "Tiny Tim", "Barry", "Gary", "Dave",
      "Clive", "Bruce", "Frank", "Egg Boy", "Bob", "Ninja", "Pirate", "Cowboy", "Wizard", "Dax",
      "Robot", "Alien", "Spy", "Betty", "Master Pain", "Sniper", "Camper", "The Bear", "Wombat",
      "Possum", "Meatball", "Blade", "Noodle", "Big Bean", "Kyle", "Jeremy", "Doug",
      "Square Nut", "Slappy Dave", "Chosen One", "Big Cheese", "Big Egg", "Captain Toast",
      "Fancy Pants", "Door Guy", "Donut", "Big Slapper", "Slap Man", "Not Boris", "Natalya",
      "Trevelyan", "Disco", "Bunker Boy", "Bottles", "Klungo", "Big Boo", "Neo", "Slippy",
      "Drumstick", "Steve E", "Dr Doak", "Secret Agent", "James Ponder", "Jo Mama", "Mr Dark",
      "Perfect Dave", "Perfect Bob", "Perfect Gary", "Cannon Fodder", "Wizpig", "LAN Man"};

  // Picks a name and keeps it. Runs once, and only when no name is set, so a
  // player who has chosen one (or typed one in Player Name) keeps it, and the
  // name stays the same from then on rather than changing every launch.
  static void TickStarterName() {
    static bool done = false;
    if (done) return;
    PerfectDarkApp* app = current_instance_;
    if (!app || app->config_path().empty()) return;  // too early to save yet
    done = true;
    if (!rex::cvar::GetFlagByName("net_player_name").empty()) return;
    std::mt19937 rng{std::random_device{}()};
    const size_t count = sizeof(kStarterNames) / sizeof(kStarterNames[0]);
    const char* name = kStarterNames[std::uniform_int_distribution<size_t>(0, count - 1)(rng)];
    rex::cvar::SetFlagByName("net_player_name", name);
    SavePcSettings();
    REXLOG_INFO("[online] no player name set; this install is \"{}\"", name);
  }

  // The name other players see: net_player_name, else the Windows profile
  // name, exactly as netplay decides it.
  static std::string OnlineName() {
    namespace netplay = rex::kernel::xam::netplay;
    std::string profile_name;
    auto* kernel_state = REX_KERNEL_STATE();
    if (kernel_state) {
      const auto& profile = kernel_state->user_profile();
      if (profile) profile_name = profile->name();
    }
    return netplay::ConfiguredPlayerName(profile_name);
  }

  static bool OnlinePlayOn() { return rex::cvar::GetFlagByName("net_private_match") == "true"; }

  // Registering with the online server waits for its answer, which would stall
  // the menu, so the page kicks it off once on a thread of its own and the row
  // says "Connected" as soon as it has.
  static void WarmUpOnlineServer() {
    if (online_warmed_.exchange(true)) return;
    std::thread([] { rex::kernel::xam::netplay::StartOnline(); }).detach();
  }

  // which: 0 label, 1 first option, 2 second option.
  static uint32_t OnlineString(int row, int which) {
    namespace netplay = rex::kernel::xam::netplay;
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    if (!mem || !online_strings_) return 0;
    static constexpr uint32_t kOffset[3] = {0, 32, 80};
    static constexpr size_t kCap[3] = {32, 48, 48};
    const uint32_t addr = online_strings_ + uint32_t(row) * kOnlineStride + kOffset[which];
    const netplay::OnlineStatus status = netplay::Status();
    std::string text;
    switch (row) {
      case 0:
        if (which == 0) text = "Player Name";
        if (which == 1) text = OnlineName();
        if (which == 2) text = "Change name...";
        break;
      case 1:
        if (which == 0) text = "Online Play";
        if (which == 1) text = "Off";
        if (which == 2) text = "On";
        break;
      case 2:
        if (which == 0) text = "Online Server";
        if (which == 1) {
          if (!status.enabled) {
            text = "Starts when you play online";
          } else if (!status.server_configured) {
            text = "None - your own network";
          } else if (status.connected) {
            text = "Connected";
          } else {
            WarmUpOnlineServer();
            text = "Connecting...";
          }
        }
        break;
      case 3:
        if (which == 0) text = "Your Address";
        if (which == 1) {
          text = status.address.empty() ? std::string("Found when you host") : status.address;
        }
        break;
      default:
        break;
    }
    WriteMenuText(mem, addr, text, kCap[which]);
    return addr;
  }

  static void PdOnlineRowImpl(PPCContext& ctx, int row) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    if (!mem) {
      ctx.r3.u64 = 0;
      return;
    }
    // Your Address: every shareable address, picked to copy (see above).
    if (row == 3 && op != 24 && AddressRowOp(mem, ctx, op, data)) {
      return;
    }
    // Name: the name itself, then "Change name...". Online Play: Off / On.
    // The server row is read-only, so it has one option, its value.
    const uint32_t count = (row == 0 || row == 1) ? 2 : 1;
    switch (op) {
      case 1:
        if (data) WriteGuestU32(mem, data, count);
        ctx.r3.u64 = 0;
        return;
      case 3: {
        const uint32_t index = data ? ReadGuestU32(mem, data) : 0;
        ctx.r3.u64 = OnlineString(row, index == 0 ? 1 : 2);
        return;
      }
      case 6: {  // an option was picked
        const uint32_t index = data ? ReadGuestU32(mem, data) : 0;
        if (row == 0 && index == 1) {
          RequestNameEntry();
        } else if (row == 1) {
          // Chosen deliberately, so this one is remembered: the game will be
          // online from the start next time (and offline from the start if it
          // is switched off).
          const bool on = index != 0;
          rex::kernel::xam::netplay::SetEnabled(on);
          SavePcSettings();
          REXLOG_INFO("[online] online play switched {}", on ? "on" : "off");
        }
        ctx.r3.u64 = 0;
        return;
      }
      case 7:  // the option the row shows when it is closed
        if (data) WriteGuestU32(mem, data, (row == 1 && OnlinePlayOn()) ? 1 : 0);
        ctx.r3.u64 = 0;
        return;
      default:
        ctx.r3.u64 = 0;
        return;
    }
  }

  template <int N>
  static void PdOnlineRowLabel(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = OnlineString(N, 0);
  }
  template <int N>
  static void PdOnlineRowHandler(PPCContext& ctx, uint8_t*) {
    PdOnlineRowImpl(ctx, N);
  }
  template <std::size_t... I>
  static void FillOnlineThunks(std::index_sequence<I...>, PPCFunc** labels, PPCFunc** handlers) {
    ((labels[I] = &PdOnlineRowLabel<int(I)>, handlers[I] = &PdOnlineRowHandler<int(I)>), ...);
  }

  static void PdMatchTypeHandler(PPCContext& ctx, uint8_t* base) {
    namespace netplay = rex::kernel::xam::netplay;
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    // Join Match was picked on an earlier frame: open the Multiplayer screen
    // now that the dropdown list has closed.
    if (op != 6) {
      PushPendingDialog(ctx, base);
    }
    // The list reads Join Match, Host Match, Local Multiplayer - the reverse
    // of the game's own order (0 Local, 1 Host, 2 Join) - so option indexes
    // are mirrored on the way in (ops 3, 6) and out (op 7). Everything below
    // and the game's handler work with the game's own indexes.
    const bool mirrored = mem && data && (op == 3 || op == 6);
    const uint32_t shown = mirrored ? ReadGuestU32(mem, data) : 0;
    if (mirrored) WriteGuestU32(mem, data, 2 - std::min<uint32_t>(shown, 2));
    // The menu may keep using its option word after the call, so it gets its
    // own value back.
    auto restore = [&] {
      if (mirrored) WriteGuestU32(mem, data, shown);
    };
    if (mem && data && (op == 3 || op == 6)) {
      const uint32_t index = ReadGuestU32(mem, data);
      if (op == 3 && index == kMatchTypeHost && netplay::IsJoinedSession()) {
        ctx.r3.u64 = kStrJoinedMatch;
        restore();
        return;
      }
      if (op == 6 && (index == kMatchTypeHost || index == kMatchTypeJoin)) {
        // Nothing has touched the network so far: the game's sockets are on
        // 127.0.0.1 and Windows has had no reason to ask about the firewall.
        // Picking an online match is the moment the player asks for online
        // play, so it starts here - the sockets bind for real on their next
        // use (netplay::SetEnabled -> NotifyNetplayChanged). Not saved to the
        // config: a run that never comes here never goes online. The switch
        // in Settings > Online is the one that is remembered.
        if (index == kMatchTypeJoin) {
          // Join Match: the lobby stays as it is; the Multiplayer screen opens
          // (ONLINE / LAN / DIRECT CONNECT), and its choice starts the network.
          multiplayer_open_pending_ = multiplayer_dialog_ != 0;
          ctx.r3.u64 = 0;
          restore();
          return;
        }
        // Host Match: hosted on this PC's own networks (LAN, and Direct
        // Connect for anyone who can reach it).
        netplay::SetOnlineMode(false);
        if (!netplay::Enabled()) {
          netplay::SetEnabled(true);
          netplay::ShowNotice("Hosting - Windows may ask to allow network access");
        }
      }
    }
    sub_8227E9A0(ctx, base);
    restore();
    if (op == 7 && mem && data) {
      WriteGuestU32(mem, data, 2 - std::min<uint32_t>(ReadGuestU32(mem, data), 2));
    }
  }

  static void PdMouseSensHandler(PPCContext& ctx, uint8_t*) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    uint32_t ret = 0;
    switch (op) {
      case 1:
        if (mem) WriteGuestU32(mem, data, 5);
        break;
      case 3: {
        uint32_t i = mem ? ReadGuestU32(mem, data) : 0;
        if (i > 4) i = 0;
        ret = kSensOptionStrings + i * 16;
        break;
      }
      case 6: {
        uint32_t i = mem ? ReadGuestU32(mem, data) : 0;
        if (i > 4) i = 0;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", SensValues()[i]);
        if (rex::cvar::SetFlagByName("mnk_sensitivity", buf)) {
          SavePcSettings();
        }
        break;
      }
      case 7:
        if (mem) WriteGuestU32(mem, data, static_cast<uint32_t>(GetMouseSensIndex()));
        break;
      default:
        break;
    }
    ctx.r3.u64 = ret;
  }

  static bool GetPcInvertLook() {
    std::string value = rex::cvar::GetFlagByName("mnk_invert_y");
    return value == "true" || value == "1";
  }

  static void SetPcInvertLook(bool enabled) {
    if (rex::cvar::SetFlagByName("mnk_invert_y", enabled ? "true" : "false")) {
      SavePcSettings();
    }
  }

  static void PdInvertLookMenuHandler(PPCContext& ctx, uint8_t*) {
    PdTwoStateHandler(ctx, kStrOff, kStrOn, &GetPcInvertLook, &SetPcInvertLook);
  }

  static void PdGameDisplayMenuHandler(PPCContext& ctx, uint8_t*) {
    PdTwoStateHandler(ctx, kStrWindowed, kStrFullscreen, &GetPcFullscreen, &SetPcFullscreen);
  }

  static void PdVsyncMenuHandler(PPCContext& ctx, uint8_t*) {
    PdTwoStateHandler(ctx, kStrOff, kStrOn, &GetPcVsync, &SetPcVsync);
  }

  static void PdShowFpsMenuHandler(PPCContext& ctx, uint8_t*) {
    PdTwoStateHandler(ctx, kStrOff, kStrOn, &GetPcShowFps, &SetPcShowFps);
  }

  // TEMP_MENU_SCAN: the keyboard bindings shown on the PC Controls page.
  // Perfect Dark's Classic scheme only exposes these controls, so this is the
  // complete set that can be bound - melee, grenades, night vision and weapon
  // select 1-4 are Quick Menu actions, not pad buttons, and cannot appear here.
  struct BindRow {
    const char* label;
    const char* cvar;
    const char* fallback;  // the shipped default, restored by "Reset to default"
  };
  static const BindRow* BindRows() {
    static const BindRow kRows[] = {
        {"Fire", "keybind_right_trigger", "LMB"},
        {"Aim", "keybind_left_trigger", "RMB"},
        {"Reload", "keybind_x", "R"},
        {"Use / Activate", "keybind_a", "E"},
        {"Next Weapon", "keybind_y", "WheelUp"},
        {"Previous Weapon", "keybind_b", "WheelDown"},
        {"Crouch", "keybind_lstick_press", "Control"},
        {"Zoom", "keybind_rstick_press", "MMB"},
        {"Weapon Menu", "keybind_left_shoulder", "Q"},
        {"Secondary Fire", "keybind_right_shoulder", "F"},
        {"Objectives", "keybind_back", "Tab"},
        {"Pause", "keybind_start", "Escape"},
        {"Move Forward", "keybind_lstick_up", "W"},
        {"Move Back", "keybind_lstick_down", "S"},
        {"Strafe Left", "keybind_lstick_left", "A"},
        {"Strafe Right", "keybind_lstick_right", "D"},
    };
    return kRows;
  }
  // The right-stick keys (Look Up/Down/Left/Right) are not listed: the mouse
  // does the looking, they ship unbound, and a row that always reads "-" only
  // makes the page longer. The binds themselves still exist for anyone who
  // wants them in perfect_dark.toml.
  static constexpr int kBindRowCount = 16;

  // Each binding row is a dropdown. Entry 0 is the current key - it is what the
  // row shows, because op 7 always reports it as selected - and the rest are
  // commands. Their text lives in scratch at kBindOptionStrings.
  static constexpr uint32_t kBindOptionStrings = 0x82675400;  // 3 x 32
  static constexpr int kBindOptionCount = 4;
  static constexpr const char* kBindOptionText[3] = {"Change key\n", "Add a second key\n",
                                                     "Reset to default\n"};

  static std::vector<std::string> SplitKeys(const std::string& value) {
    std::vector<std::string> keys;
    size_t start = 0;
    while (start <= value.size()) {
      const size_t comma = value.find(',', start);
      std::string key =
          value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      while (!key.empty() && key.front() == ' ') key.erase(0, 1);
      while (!key.empty() && key.back() == ' ') key.pop_back();
      if (!key.empty()) keys.push_back(key);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    return keys;
  }

  static std::string JoinKeys(const std::vector<std::string>& keys) {
    std::string value;
    for (const auto& key : keys) {
      if (!value.empty()) value += ",";
      value += key;
    }
    return value;
  }

  // The config names ("LMB", "WheelUp", "Control") read poorly in a menu.
  static std::string KeyDisplayName(const std::string& key) {
    static const std::pair<const char*, const char*> kNames[] = {
        {"LMB", "Left Mouse"},   {"RMB", "Right Mouse"},  {"MMB", "Middle Mouse"},
        {"Mouse4", "Mouse 4"},   {"Mouse5", "Mouse 5"},
        {"WheelUp", "Wheel Up"}, {"WheelDown", "Wheel Down"}, {"Control", "Ctrl"},
        {"Escape", "Esc"},       {"Return", "Enter"},     {"PageUp", "Page Up"},
        {"PageDown", "Page Down"}, {"CapsLock", "Caps Lock"}, {"LBracket", "["},
        {"RBracket", "]"},       {"Semicolon", ";"},      {"Quote", "'"},
        {"Comma", ","},          {"Period", "."},         {"Slash", "/"},
        {"Backslash", "\\"},     {"Backtick", "`"},       {"Minus", "-"},
        {"Plus", "="},
    };
    for (const auto& [raw, nice] : kNames) {
      if (key == raw) return nice;
    }
    return key;
  }

  static std::string FormatBinding(const std::string& value) {
    const auto keys = SplitKeys(value);
    if (keys.empty()) return "-";
    std::string text;
    for (const auto& key : keys) {
      if (!text.empty()) text += " / ";
      text += KeyDisplayName(key);
    }
    return text;
  }

  // Shared body: publishes the current key for a row as its value text. The
  // value is read from the cvar every time the menu asks, so it always reflects
  // what is actually bound rather than a snapshot taken at install time.
  static void PdBindRowImpl(PPCContext& ctx, int row) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    uint32_t ret = 0;
    switch (op) {
      case 1:
        if (mem) WriteGuestU32(mem, data, kBindOptionCount);
        break;
      case 3: {
        const uint32_t index = mem ? ReadGuestU32(mem, data) : 0;
        if (index >= 1 && index < uint32_t(kBindOptionCount)) {
          ret = kBindOptionStrings + (index - 1) * kBindRowStride;
          break;
        }
        const uint32_t addr = BindValueStrings() + uint32_t(row) * kBindRowStride;
        if (mem) {
          std::string value;
          if (capture_row_.load() == row) {
            value = "Press a key...";
          } else if (notice_row_.load() == row && NowMs() < notice_until_ms_.load()) {
            value = std::string("Taken by ") + BindRows()[notice_other_.load()].label;
          } else {
            value = FormatBinding(rex::cvar::GetFlagByName(BindRows()[row].cvar));
          }
          if (value.size() > kBindRowStride - 3) value.resize(kBindRowStride - 3);
          value += "\n";
          WriteGuestBytes(mem, addr, value.c_str(), value.size() + 1);
        }
        ret = addr;
        break;
      }
      case 6: {
        const uint32_t index = mem ? ReadGuestU32(mem, data) : 0;
        if (index == 1 || index == 2) {
          BeginKeyCapture(row, index == 2);
        } else if (index == 3) {
          SetRowKeys(row, SplitKeys(BindRows()[row].fallback));
          SavePcSettings();
        }
        break;
      }
      case 7:
        if (mem) WriteGuestU32(mem, data, 0);
        break;
      default:
        break;
    }
    ctx.r3.u64 = ret;
  }

  // --- key rebinding -------------------------------------------------------
  // "Change key" / "Add a second key" arm a capture from the guest thread. The
  // UI thread (TickKeyCapture, every frame) waits until every key is up - the key
  // that confirmed the dropdown is still down - then takes the next key, mouse
  // button or wheel notch and writes it. The game must not also treat that press
  // as menu input, so mnk_mode is switched off for the duration (the MnK driver
  // reads it on every poll) and restored once everything is released again.
  enum CapturePhase : int { kCaptureIdle = 0, kCaptureWaitRelease, kCaptureListen, kCaptureFinish };
  static inline std::atomic<int> capture_row_{-1};
  static inline std::atomic<int> capture_phase_{kCaptureIdle};
  static inline std::atomic<bool> capture_append_{false};
  static inline std::atomic<int> capture_wheel_{0};
  static inline std::atomic<int64_t> capture_deadline_ms_{0};
  static inline std::string capture_prev_mnk_;
  // A refused "Add a second key" shows "Taken by <action>" on the row briefly.
  static inline std::atomic<int> notice_row_{-1};
  static inline std::atomic<int> notice_other_{0};
  static inline std::atomic<int64_t> notice_until_ms_{0};

  static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  static void BeginKeyCapture(int row, bool append) {
    if (capture_phase_.load() != kCaptureIdle) return;
    capture_prev_mnk_ = rex::cvar::GetFlagByName("mnk_mode");
    rex::cvar::SetFlagByName("mnk_mode", "false");
    capture_append_ = append;
    capture_wheel_ = 0;
    capture_deadline_ms_ = NowMs() + 10000;
    capture_row_ = row;
    capture_phase_ = kCaptureWaitRelease;  // published last: the UI thread keys off it
    REXLOG_INFO("[rebind] {}: waiting for a key ({})", BindRows()[row].label,
                append ? "add second" : "replace");
  }

  // Gives `row` exactly `keys` and takes each of them off every other action. An
  // action left with nothing receives this row's previous keys, so a clash is a
  // swap and no action ends up unbound.
  static void SetRowKeys(int row, std::vector<std::string> keys) {
    const BindRow* rows = BindRows();
    const auto previous = SplitKeys(rex::cvar::GetFlagByName(rows[row].cvar));
    std::vector<std::string> spare;
    for (const auto& key : previous) {
      if (std::find(keys.begin(), keys.end(), key) == keys.end()) spare.push_back(key);
    }
    for (int j = 0; j < kBindRowCount; ++j) {
      if (j == row) continue;
      auto others = SplitKeys(rex::cvar::GetFlagByName(rows[j].cvar));
      const size_t before = others.size();
      others.erase(std::remove_if(others.begin(), others.end(),
                                  [&](const std::string& key) {
                                    return std::find(keys.begin(), keys.end(), key) != keys.end();
                                  }),
                   others.end());
      if (others.size() == before) continue;
      if (others.empty()) {
        if (spare.empty()) continue;  // nothing to hand over: leave the key shared
        others = spare;
        spare.clear();
      }
      rex::cvar::SetFlagByName(rows[j].cvar, JoinKeys(others));
      REXLOG_INFO("[rebind] {} -> {}", rows[j].label, JoinKeys(others));
    }
    rex::cvar::SetFlagByName(rows[row].cvar, JoinKeys(keys));
    REXLOG_INFO("[rebind] {} -> {}", rows[row].label, JoinKeys(keys));
  }

  static bool AnyInputDown() {
    for (int vk = 1; vk < 255; ++vk) {
      if (GetAsyncKeyState(vk) & 0x8000) return true;
    }
    return false;
  }

  // Esc cancels, and F4 / F7 / F9 open runtime panels, so none of them can be
  // captured. The generic Shift/Ctrl/Alt codes are used rather than left/right.
  static bool IsCaptureExcluded(int vk) {
    switch (vk) {
      case VK_ESCAPE:
      case VK_F4:
      case VK_F7:
      case VK_F9:
      case VK_LWIN:
      case VK_RWIN:
      case VK_APPS:
        return true;
      default:
        return vk >= VK_LSHIFT && vk <= VK_RMENU;
    }
  }

  static std::string CaptureName(int vk) {
    if (vk == VK_RETURN) return "Return";  // also listed as NumpadEnter
    return rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(vk));
  }

  static void BeginCaptureFinish(const char* outcome) {
    const int row = capture_row_.load();
    if (row >= 0) REXLOG_INFO("[rebind] {}: {}", BindRows()[row].label, outcome);
    capture_deadline_ms_ = NowMs() + 3000;
    capture_phase_ = kCaptureFinish;
  }

  static void TickKeyCapture(bool focused) {
    const int phase = capture_phase_.load();
    if (phase == kCaptureIdle) return;
    const int row = capture_row_.load();
    const bool expired = NowMs() > capture_deadline_ms_.load();
    if (phase == kCaptureWaitRelease) {
      if (!AnyInputDown()) {
        capture_wheel_ = 0;
        capture_phase_ = kCaptureListen;
      } else if (expired) {
        BeginCaptureFinish("timed out");
      }
    } else if (phase == kCaptureListen) {
      if (expired) {
        BeginCaptureFinish("timed out, nothing changed");
        return;
      }
      if (!focused) return;
      if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        BeginCaptureFinish("cancelled");
        return;
      }
      std::string key;
      const int wheel = capture_wheel_.exchange(0);
      if (wheel > 0) key = "WheelUp";
      if (wheel < 0) key = "WheelDown";
      for (int vk = 1; key.empty() && vk < 255; ++vk) {
        if (IsCaptureExcluded(vk) || !(GetAsyncKeyState(vk) & 0x8000)) continue;
        key = CaptureName(vk);
      }
      if (key.empty()) return;
      std::vector<std::string> keys;
      if (capture_append_) {
        // Adding a key that is another action's only key would leave that
        // action unbound (or, if kept, make one key do two things), so refuse
        // and say which action has it. "Change key" swaps instead.
        for (int j = 0; j < kBindRowCount; ++j) {
          if (j == row) continue;
          const auto others = SplitKeys(rex::cvar::GetFlagByName(BindRows()[j].cvar));
          if (others.size() == 1 && others[0] == key) {
            notice_other_ = j;
            notice_until_ms_ = NowMs() + 4000;
            notice_row_ = row;
            BeginCaptureFinish("refused, key is the only one on another action");
            return;
          }
        }
        keys = SplitKeys(rex::cvar::GetFlagByName(BindRows()[row].cvar));
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
          if (keys.size() >= 2) keys.resize(1);
          keys.push_back(key);
        }
      } else {
        keys = {key};
      }
      SetRowKeys(row, keys);
      BeginCaptureFinish("bound");
    } else if (phase == kCaptureFinish) {
      // Hand input back only once the captured key is up again, so it cannot
      // also land on the menu.
      if (!AnyInputDown() || expired) {
        rex::cvar::SetFlagByName("mnk_mode", capture_prev_mnk_);
        SavePcSettings();
        capture_row_ = -1;
        capture_phase_ = kCaptureIdle;
      }
    }
  }

  // Wheel notches are not visible to GetAsyncKeyState, so they come in through
  // a window input listener. It never marks events handled.
  class RebindWheelListener : public rex::ui::WindowInputListener {
   public:
    void OnMouseWheel(rex::ui::MouseEvent& e) override {
      const int detents = e.scroll_y() / int(rex::ui::MouseEvent::kScrollPerDetent);
      // A menu that is one long list scrolls with the wheel, which suits it far
      // better than pushing the pointer at it (TickMenuMouse).
      if (detents != 0 && menu_mouse_active_) menu_wheel_ += detents;
      if (capture_phase_.load() != kCaptureListen) return;
      if (detents != 0) capture_wheel_ = detents > 0 ? 1 : -1;
    }
  };
  static inline RebindWheelListener rebind_wheel_listener_;

  // Distinct thunk per row so each menuitem gets its own guest address; the
  // template gives us one function instance per row without the boilerplate.
  template <int N>
  static void PdBindRowLabel(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = BindLabelStrings() + uint32_t(N) * kBindRowStride;
  }
  template <int N>
  static void PdBindRowHandler(PPCContext& ctx, uint8_t*) {
    PdBindRowImpl(ctx, N);
  }

  // TEMP_MENU_SCAN: native C++ implementation of a guest `menuitem` handler,
  // registered at a guest address via FunctionDispatcher::AllocateThunk so the
  // game's own menu code can call straight into it.
  //
  // Protocol reverse-engineered from sub_821AACC8 (the stock "2-Player Screen
  // Split" spinner handler), which dispatches on r3 with r5 pointing at a
  // scratch word:
  //   op 1 -> write the number of options into data[0] (NOT a return value)
  //   op 3 -> data[0] holds the option index; return a char* to its label in r3
  //   op 6 -> data[0] holds the newly chosen index; apply it
  //   op 7 -> write the currently selected index into data[0]
  // Everything else returns 0. The menu also sends ops 2/12/14/24/25/26 that
  // sub_821AACC8 ignores (its dispatch only covers r3 in 1..7), so we do too.
  //
  // Pressing A on the row opens the game's own dropdown list of the option
  // labels; picking one sends op 6.
  static void PdResolutionMenuHandler(PPCContext& ctx, uint8_t* /*base*/) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    uint32_t ret = 0;

    switch (op) {
      case 1:
        // GET_OPTION_COUNT. sub_821AACC8 (the stock spinner handler) writes the
        // count to *r5 and returns 0 in r3 - it is NOT a return value. Getting
        // this wrong makes the menu read a count of 0 and swallow every press.
        if (mem) WriteGuestU32(mem, data, 4);
        break;
      case 3: {
        uint32_t i = mem ? ReadGuestU32(mem, data) : 0;
        if (i > 3) i = 0;
        ret = kResOptionStrings + i * 16;
        break;
      }
      case 6: {
        uint32_t i = mem ? ReadGuestU32(mem, data) : 0;
        if (i > 3) i = 0;
        ApplyPcResolutionIndex(static_cast<int32_t>(i));
        break;
      }
      case 7:
        if (mem) {
          WriteGuestU32(mem, data, static_cast<uint32_t>(GetPcResolutionIndex()));
        }
        break;
      default:
        break;
    }
    ctx.r3.u64 = ret;
  }

  // TEMP_MENU_SCAN: overwrite a string in the game's table in place. Only works
  // when the replacement is no longer than the original. Used for menu rows
  // whose label must be a real text ID (see the note on type-0x04 items).
  static int ReplaceGuestString(rex::memory::Memory* mem, const char* find, const char* repl) {
    const size_t flen = strlen(find);
    const size_t rlen = strlen(repl);
    if (rlen > flen) return 0;
    int patched = 0;
    ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
      uint8_t* host = mem->TranslateVirtual<uint8_t*>(region_addr);
      if (!host || region_size < flen + 1) return;
      for (size_t off = 0; off + flen + 1 <= region_size; ++off) {
        if (memcmp(host + off, find, flen) == 0) {
          memcpy(host + off, repl, rlen + 1);  // includes the NUL
          ++patched;
        }
      }
    });
    return patched;
  }

  // Replace a phrase *inside* a string in the game's table, keeping whatever
  // follows it. ReplaceGuestString writes its NUL at the end of the
  // replacement, so it throws away the rest of the sentence: renaming
  // "Xbox LIVE Quick Match" also chopped the help line that mentions it in
  // passing. This one shuffles the tail down instead, so only the phrase
  // changes. As before the replacement must be no longer than the phrase.
  //
  // A phrase can also occur in data that is not text, so a match only counts
  // when it sits in a plausible string: no control codes other than newlines
  // and tabs, and a NUL within kMaxGuestString.
  //
  // With whole_string a phrase only counts when it is the entire string, for
  // words that also occur inside other entries ("Friends" is a leaderboard
  // filter and half of "Invite Friends").
  static constexpr size_t kMaxGuestString = 4096;

  // One phrase to swap, and how many copies were found.
  struct GuestSwap {
    const char* from;
    const char* to;
    bool whole_string = false;
    int copies = 0;
  };

  // Rewrite a phrase in place, shifting the tail of its string down. Returns
  // false when it is not a plausible string (no control codes other than
  // newlines and tabs, and a NUL within kMaxGuestString) - the same bytes can
  // occur in data that is not text. The help pages are Windows-1252, so bytes
  // above 0x7e count as text.
  static bool ApplyGuestSwap(uint8_t* host, size_t region_size, size_t off, const GuestSwap& swap,
                             size_t flen, size_t rlen) {
    if (swap.whole_string && (off == 0 || host[off - 1] != 0 || host[off + flen] != 0)) {
      return false;
    }
    size_t end = off + flen;
    const size_t limit = std::min<size_t>(region_size, off + kMaxGuestString);
    while (end < limit && host[end] != 0) {
      const uint8_t c = host[end];
      if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') return false;
      ++end;
    }
    if (end >= limit || host[end] != 0) return false;
    const size_t tail = end - (off + flen);
    memcpy(host + off, swap.to, rlen);
    memmove(host + off + rlen, host + off + flen, tail);
    memset(host + off + rlen + tail, 0, flen - rlen + 1);
    return true;
  }

  // Apply every swap in one walk of guest memory. Walking it costs about half
  // a second, so doing that once per phrase is what it used to do - with two
  // dozen phrases that is ten seconds of a blocked UI thread, which ate the
  // intro. Candidates are bucketed by first byte, so most offsets cost one
  // lookup.
  //
  // At a given offset the swaps are tried in table order, which is what keeps
  // "Xbox LIVE Quick Match: Competitive..." ahead of "Xbox LIVE Quick Match":
  // a sentence that quotes a menu name starts at or before the name itself.
  static void ReplaceGuestStrings(rex::memory::Memory* mem, GuestSwap* swaps, size_t count) {
    std::vector<uint8_t> buckets[256];
    std::vector<size_t> lengths(count);
    for (size_t i = 0; i < count; ++i) {
      lengths[i] = strlen(swaps[i].from);
      if (lengths[i] == 0 || strlen(swaps[i].to) > lengths[i]) continue;
      buckets[uint8_t(swaps[i].from[0])].push_back(uint8_t(i));
    }
    ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
      uint8_t* host = mem->TranslateVirtual<uint8_t*>(region_addr);
      if (!host || region_size < 2) return;
      for (size_t off = 0; off + 1 < region_size; ++off) {
        const std::vector<uint8_t>& candidates = buckets[host[off]];
        if (candidates.empty()) continue;
        for (const uint8_t index : candidates) {
          GuestSwap& swap = swaps[index];
          const size_t flen = lengths[index];
          if (off + flen + 1 > region_size) continue;
          if (memcmp(host + off, swap.from, flen) != 0) continue;
          const size_t rlen = strlen(swap.to);
          if (!ApplyGuestSwap(host, region_size, off, swap, flen, rlen)) continue;
          ++swap.copies;
          off += rlen;  // carry on after the replacement
          break;
        }
      }
    });
  }

  // A single phrase, for callers with only one.
  static int ReplaceGuestSubstring(rex::memory::Memory* mem, const char* find, const char* repl,
                                   bool whole_string = false) {
    GuestSwap swap{find, repl, whole_string, 0};
    ReplaceGuestStrings(mem, &swap, 1);
    return swap.copies;
  }

  // TEMP_LOOK_SCAN: read a big-endian guest float.
  static float ReadGuestF32(rex::memory::Memory* mem, uint32_t addr) {
    uint32_t bits = ReadGuestU32(mem, addr);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
  }

  // TEMP_LOOK_SCAN: locate Perfect Dark's player look angles.
  //
  // From the N64 decompilation (src/include/types.h) the player struct holds:
  //   +0x144 f32 vv_theta      horizontal look, degrees
  //   +0x14c f32 vv_costheta
  //   +0x150 f32 vv_sintheta
  //   +0x154 f32 vv_verta      vertical look, degrees
  // costheta/sintheta are cos/sin of theta, which is a self-verifying signature:
  // we can find the struct by searching for that invariant instead of trusting
  // the XBLA build to use the same offsets as the N64 one.
  static void DebugScanForLookAngles() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();

    int hits = 0;
    look_candidates_.clear();
    look_candidate_theta_.clear();
    ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
      if (region_size < 0x20) return;
      for (uint32_t off = 0; off + 0x18 <= region_size; off += 4) {
        uint32_t a = region_addr + off;
        float theta = ReadGuestF32(mem, a);
        // Every test below is written as a POSITIVE condition: NaN compares
        // false against everything, so a "reject if out of range" test lets all
        // the garbage through - which is exactly what the first pass did.
        if (!(std::isfinite(theta) && theta >= -0.001f && theta <= 360.001f)) continue;
        float c = ReadGuestF32(mem, a + 8);
        float sn = ReadGuestF32(mem, a + 12);
        if (!(std::isfinite(c) && std::isfinite(sn))) continue;
        // A real cos/sin pair sits on the unit circle; this alone discards the
        // degenerate all-zero runs that fill most of memory.
        const double mag = double(c) * double(c) + double(sn) * double(sn);
        if (!(mag > 0.999 && mag < 1.001)) continue;
        const double rad = double(theta) * 3.14159265358979323846 / 180.0;
        const double dc = cos(rad) - double(c);
        const double ds = sin(rad) - double(sn);
        if (!(dc * dc + ds * ds < 1e-4)) continue;
        float verta = ReadGuestF32(mem, a + 0x10);
        float verta360 = ReadGuestF32(mem, a + 0x14);
        (void)verta;
        (void)verta360;
        look_candidates_.push_back(a);
        look_candidate_theta_.push_back(theta);
        ++hits;
      }
    });
    REXLOG_INFO("[look] captured {} candidate(s). Now TURN, then press F6 to filter.", hits);
  }

  // TEMP_LOOK_SCAN: keep only candidates whose theta actually moved since the
  // capture and that still satisfy the cos/sin invariant. Repeat after each
  // turn and the list collapses to the real player angle.
  static void DebugFilterLookAngles() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    if (look_candidates_.empty()) {
      REXLOG_INFO("[look] no candidates - press F8 first");
      return;
    }

    std::vector<uint32_t> keep;
    std::vector<float> keep_theta;
    for (size_t i = 0; i < look_candidates_.size(); ++i) {
      uint32_t a = look_candidates_[i];
      float theta = ReadGuestF32(mem, a);
      float c = ReadGuestF32(mem, a + 8);
      float sn = ReadGuestF32(mem, a + 12);
      if (!(std::isfinite(theta) && std::isfinite(c) && std::isfinite(sn))) continue;
      const double mag = double(c) * double(c) + double(sn) * double(sn);
      if (!(mag > 0.999 && mag < 1.001)) continue;
      const double rad = double(theta) * 3.14159265358979323846 / 180.0;
      const double dc = cos(rad) - double(c);
      const double ds = sin(rad) - double(sn);
      if (!(dc * dc + ds * ds < 1e-4)) continue;
      float moved = theta - look_candidate_theta_[i];
      if (moved < 0) moved = -moved;
      if (!(moved > 1.0f)) continue;
      keep.push_back(a);
      keep_theta.push_back(theta);
    }

    look_candidates_.swap(keep);
    look_candidate_theta_.swap(keep_theta);
    REXLOG_INFO("[look] {} candidate(s) survived the turn", look_candidates_.size());
    if (look_candidates_.size() == 1) {
      DebugFindLookPointer(look_candidates_[0]);
    }
    for (size_t i = 0; i < look_candidates_.size() && i < 12; ++i) {
      uint32_t a = look_candidates_[i];
      REXLOG_INFO("[look]   theta@0x{:08X} = {:.3f} deg  verta={:.3f}  (player base ~0x{:08X})", a,
                  ReadGuestF32(mem, a), ReadGuestF32(mem, a + 0x10), a - 0x144);
    }
  }

  static void WriteGuestF32(rex::memory::Memory* mem, uint32_t addr, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    WriteGuestU32(mem, addr, bits);
  }

  // TEMP_LOOK_SCAN: does writing the angle actually move the view, or does the
  // game recompute it from its own accumulator every frame? This is the test
  // that decides whether direct injection is viable at all.
  // TEMP_LOOK_SCAN: find every angle-like float near the player that currently
  // matches the view angle. Perfect Dark keeps more than one: the view angle we
  // drive, the character's body/turn angle, and interpolation copies used for
  // rendering. Turning only the view leaves the body pointing the old way, which
  // is felt as the character strafing while walking.
  static void DebugDumpNearbyAngles() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    uint32_t theta_addr = GetLivePlayerThetaAddr(mem);
    if (!theta_addr) {
      REXLOG_INFO("[angles] no live player");
      return;
    }
    const uint32_t base = theta_addr - kThetaOffset;
    const float theta = ReadGuestF32(mem, theta_addr);

    auto near_angle = [](float v, float target) {
      float d = v - target;
      while (d > 180.0f) d -= 360.0f;
      while (d < -180.0f) d += 360.0f;
      return d < 0 ? -d : d;
    };

    REXLOG_INFO("[angles] player base 0x{:08X}, vv_theta = {:.3f}", base, theta);
    int shown = 0;
    for (uint32_t off = 0; off < 0x2400 && shown < 40; off += 4) {
      uint32_t a = base + off;
      float v = ReadGuestF32(mem, a);
      if (!std::isfinite(v)) continue;
      if (v < -400.0f || v > 400.0f) continue;
      float d = near_angle(v, theta);
      if (d > 6.0f) continue;
      REXLOG_INFO("[angles]   +0x{:04X} = {:8.3f}  (delta {:.3f})", off, v, d);
      ++shown;
    }
    REXLOG_INFO("[angles] {} field(s) tracking the view angle", shown);
  }

  static void DebugNudgeLook() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    if (look_candidates_.size() != 1) {
      REXLOG_INFO("[look] need exactly one candidate (have {}) - run F8, turn, F6, turn, F6",
                  look_candidates_.size());
      return;
    }
    uint32_t a = look_candidates_[0];
    float theta = ReadGuestF32(mem, a);
    float verta = ReadGuestF32(mem, a + 0x10);


    float new_theta = theta + 45.0f;
    while (new_theta >= 360.0f) new_theta -= 360.0f;
    ApplyLookAngles(mem, a, new_theta, verta);

    REXLOG_INFO("[look] nudged theta {:.3f} -> {:.3f} (verta {:.3f}) at 0x{:08X}", theta, new_theta,
                verta, a);
  }

  // Writes theta plus the derived fields the game keeps alongside it.
  //
  // The angle does not live alone: Perfect Dark keeps an angular VELOCITY beside
  // it and integrates that every tick. Relative to vv_theta the layout is
  //   +0x00 vv_theta   +0x04 speedtheta
  //   +0x08 vv_costheta +0x0C vv_sintheta
  //   +0x10 vv_verta   +0x14 vv_verta360   +0x18 speedverta
  // Writing the angle while leaving the speeds running means the game keeps
  // turning under us between our writes, which is felt as jitter - worst while
  // turning, exactly as reported. Zero them so our value is the only input.
  static void ApplyLookAngles(rex::memory::Memory* mem, uint32_t theta_addr, float theta,
                              float verta) {
    const double rad = double(theta) * 3.14159265358979323846 / 180.0;
    WriteGuestF32(mem, theta_addr, theta);
    WriteGuestF32(mem, theta_addr + 8, float(cos(rad)));
    WriteGuestF32(mem, theta_addr + 12, float(sin(rad)));
    WriteGuestF32(mem, theta_addr + 0x10, verta);
    float verta360 = verta < 0.0f ? verta + 360.0f : verta;
    WriteGuestF32(mem, theta_addr + 0x14, verta360);

    WriteGuestF32(mem, theta_addr + 4, 0.0f);     // speedtheta
    WriteGuestF32(mem, theta_addr + 0x18, 0.0f);  // speedverta

    // The player also keeps a previous-frame copy of the view angle that the
    // renderer interpolates from (found by dumping every angle-like float near
    // the player that tracked vv_theta - only this one did). Leaving it behind
    // gives the interpolator a gap to smear across on every injection, felt as
    // judder while turning. Keep it in step so there is nothing to smear.
    WriteGuestF32(mem, kPrevThetaOffset - kThetaOffset + theta_addr, theta);
  }

  // TEMP_LOOK_SCAN: the player struct lives on the heap, so its address changes
  // per level. Find a STABLE pointer to it instead: scan for words holding an
  // address that lands inside the struct, and keep the ones that live in the
  // static image (0x82000000..0x826E0000) - those are globals the game itself
  // dereferences, so we can follow the same pointer every frame.
  static void DebugFindLookPointer(uint32_t theta_addr) {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();

    const uint32_t lo = theta_addr > 0x800 ? theta_addr - 0x800 : 0;
    const uint32_t hi = theta_addr + 0x40;
    int shown = 0, total = 0;

    ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
      // Only the static image holds pointers that survive a level change.
      if (region_addr < 0x82000000 || region_addr >= 0x826E0000) return;
      for (uint32_t off = 0; off + 4 <= region_size; off += 4) {
        uint32_t a = region_addr + off;
        uint32_t v = ReadGuestU32(mem, a);
        if (v < lo || v > hi) continue;
        ++total;
        if (shown < 20) {
          REXLOG_INFO("[look] ptr@0x{:08X} -> 0x{:08X}  (theta is +0x{:X} from it)", a, v,
                      theta_addr - v);
          ++shown;
        }
      }
    });
    REXLOG_INFO("[look] {} static pointer(s) into the player struct", total);
  }

  // ---------------------------------------------------------------------
  // Mouse look
  //
  // The stock MnK driver folds mouse motion into the right thumbstick, so it
  // inherits Perfect Dark's stick deadzone, acceleration curve and turn-rate
  // cap - no sensitivity setting makes that feel like a PC shooter. Instead we
  // take the raw deltas and drive the game's own aim angles.
  //
  // Located by scanning for the invariant cos(theta)/sin(theta) stored beside
  // the angle, then reverse-searching the static image for pointers into that
  // struct. Three globals point at the live player; the layout is:
  //     player_base + 0x14C  f32 vv_theta      horizontal, degrees
  //     player_base + 0x154  f32 vv_costheta
  //     player_base + 0x158  f32 vv_sintheta
  //     player_base + 0x15C  f32 vv_verta      vertical, degrees
  //     player_base + 0x160  f32 vv_verta360
  // Note this is NOT the N64 decomp's 0x144 - the XBLA build shifted it by 8,
  // which is why the offsets are derived by search rather than assumed.
  static constexpr uint32_t kPlayerPtrGlobal = 0x82648C5C;
  static constexpr uint32_t kThetaOffset = 0x14C;
  // Previous-frame copy of the view angle, used for render interpolation.
  static constexpr uint32_t kPrevThetaOffset = 0x1A24;

  // Degrees of rotation per mouse count at sensitivity 1.0.
  static constexpr float kMouseLookDegreesPerCount = 0.08f;
  static constexpr float kMaxPitchDegrees = 85.0f;

  // Returns the guest address of vv_theta for the live player, or 0 when there
  // is no plausible player right now (menus, loading, cutscenes).
  static uint32_t GetLivePlayerThetaAddr(rex::memory::Memory* mem) {
    uint32_t base = ReadGuestU32(mem, kPlayerPtrGlobal);
    if (base < 0x40000000 || base >= 0x50000000) return 0;
    uint32_t a = base + kThetaOffset;

    // Only write if it really looks like the angle triple - this is what stops
    // us corrupting memory when the pointer is stale between levels.
    float theta = ReadGuestF32(mem, a);
    float c = ReadGuestF32(mem, a + 8);
    float sn = ReadGuestF32(mem, a + 12);
    if (!(std::isfinite(theta) && std::isfinite(c) && std::isfinite(sn))) return 0;
    if (!(theta >= -0.5f && theta <= 360.5f)) return 0;
    const double mag = double(c) * double(c) + double(sn) * double(sn);
    if (!(mag > 0.99 && mag < 1.01)) return 0;
    return a;
  }

  // Invoked by the MnK driver from inside GetState, on the guest thread, once
  // per game frame.
  static void OnMouseLookDelta(int32_t dx, int32_t dy) {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    if (dx == 0 && dy == 0) return;

    // While a menu is open the mouse moves the pointer over it, not the aim.
    // The motion is banked here and used by TickMenuMouse on the draw thread.
    if (menu_mouse_active_) {
      std::string sens_str = rex::cvar::GetFlagByName("mnk_sensitivity");
      float sens = sens_str.empty() ? 1.0f : float(atof(sens_str.c_str()));
      if (!(sens > 0.0f)) sens = 1.0f;
      // A pointer wants roughly a screen's width for a comfortable sweep,
      // whatever the aiming sensitivity is set to.
      const float step = 0.35f * sens;
      std::lock_guard<std::mutex> lock(net_look_mutex_);
      menu_cursor_dx_ += float(dx) * step;
      menu_cursor_dy_ += float(dy) * step * menu_dy_scale_.load();
      return;
    }

    auto* mem = app->runtime()->memory();
    // Nothing to aim before the game has shown its first menu. Everything up
    // to that point is front end - the title screen, the attract film, and the
    // Carrington Institute loading behind the main menu - and all of it is a
    // real stage with a real player standing in it, so the mouse used to turn
    // Joanna's head about on the way in. (The stage number cannot tell them
    // apart: the menus stand in the Institute, which is also somewhere you
    // play.) Once a menu has been up, the ordinary rules hold: a menu takes
    // the mouse as a pointer, and play takes it as aim.
    if (!menu_ever_open_) return;
    // The same each time the Carrington Institute loads - coming back from a
    // mission with Return to Main Menu plays its walk-in again before the menu
    // appears (user, 2026-10-27). Until a menu has opened there, no aim.
    // Choosing Carrington Institute to walk around opens the menu first.
    // (NoteGameStage tracks the loads, every game frame.)
    if (institute_menu_pending_) return;
    if (ReadGuestU32(mem, kStageNum) == kStageTitle) return;
    // The game also takes control away for its own menus, briefings and
    // cutscenes; there is nothing to aim then either.
    if (ReadGuestU8(mem, kPlayerHasControl) == 0) return;
    uint32_t a = GetLivePlayerThetaAddr(mem);
    if (!a) return;

    std::string sens_str = rex::cvar::GetFlagByName("mnk_sensitivity");
    float sens = sens_str.empty() ? 1.0f : float(atof(sens_str.c_str()));
    if (!(sens > 0.0f)) sens = 1.0f;
    const float step = kMouseLookDegreesPerCount * sens;

    // In a networked match the turn is sent with this PC's input and applied
    // by every PC (NetLookSend). A direct write here would change only this
    // PC's game, so never do one while a session exists - also in the moments
    // around the match where the game is not exchanging input yet (that
    // movement is dropped, not saved up).
    {
      namespace netplay = rex::kernel::xam::netplay;
      const bool exchanging = NetInputActive(mem);
      if (exchanging || netplay::IsHostingSession() || netplay::IsJoinedSession()) {
        const float y_sign_net = GetPcInvertLook() ? 1.0f : -1.0f;
        std::lock_guard<std::mutex> lock(net_look_mutex_);
        if (exchanging) {
          net_look_theta_ += float(dx) * step;
          net_look_verta_ += y_sign_net * float(dy) * step;
        } else {
          local_look_theta_ += float(dx) * step;
          local_look_verta_ += y_sign_net * float(dy) * step;
        }
        return;
      }
    }

    float theta = ReadGuestF32(mem, a);
    float verta = ReadGuestF32(mem, a + 0x10);

    // TEMP_LOOK_SCAN: measure instead of guessing.
    //  - rate: how often we are actually invoked. Should track the frame rate;
    //    if it is erratic or a multiple/fraction of it, the cadence is wrong.
    //  - drift: whether the GAME changed the angle since our last write. If it
    //    does, something else is still driving aim and fighting us.
    {
      static uint64_t n = 0;
      static uint64_t drifted = 0;
      static double drift_sum = 0.0;
      static float last_written = 0.0f;
      static bool have_last = false;
      static auto window_start = std::chrono::steady_clock::now();

      if (have_last) {
        float d = theta - last_written;
        while (d > 180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        if (d < 0) d = -d;
        if (d > 0.001f) {
          ++drifted;
          drift_sum += d;
        }
      }
      ++n;
      if ((n % 120) == 0) {
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - window_start).count();
        window_start = now;
        if (REXCVAR_GET(pd_dev_hotkeys)) {
          REXLOG_INFO(
              "[looktime] {:.1f} injections/sec | game moved the angle on {}/120 of them, "
              "avg {:.4f} deg",
              secs > 0 ? 120.0 / secs : 0.0, drifted, drifted ? drift_sum / double(drifted) : 0.0);
        }
        drifted = 0;
        drift_sum = 0.0;
      }
      last_written = theta;
      have_last = true;
      look_last_written_theta_ = &last_written;
    }

    // Observed in game: increasing vv_theta turns RIGHT on screen, so mouse-right
    // (dx > 0) must add. The opposite guess made her look left when the mouse
    // went right.
    theta += float(dx) * step;
    while (theta < 0.0f) theta += 360.0f;
    while (theta >= 360.0f) theta -= 360.0f;

    // Observed in game: increasing vv_verta looks UP, and screen dy is positive
    // downward, so mouse-down must subtract to look down.
    const float y_sign = GetPcInvertLook() ? 1.0f : -1.0f;
    verta += y_sign * float(dy) * step;
    if (verta > kMaxPitchDegrees) verta = kMaxPitchDegrees;
    if (verta < -kMaxPitchDegrees) verta = -kMaxPitchDegrees;

    ApplyLookAngles(mem, a, theta, verta);
    if (look_last_written_theta_) *look_last_written_theta_ = theta;
  }

  static inline float* look_last_written_theta_ = nullptr;

  // ---------------------------------------------------------------------------
  // Mouse look in networked matches (Co-Operative, Counter-Operative).
  //
  // Online co-op is lockstep: each PC sends its controller input for a frame,
  // both PCs feed every player's input into the simulation on the same frame,
  // and a divergence ends the match with "Synchronisation error detected".
  // Direct mouse look writes the view angle on the PC that moved the mouse
  // only, so it desynchronises immediately.
  //
  // Instead the angle change travels with the input the game already sends
  // (sub_82294F00 queues this PC's 12-byte XINPUT_GAMEPAD for a frame) and is
  // applied on every PC by the game's own per-player input step:
  //  - send:    buttons 0x0400 (unused by XInput and ignored by the game) marks
  //             a mouse frame; the high bytes of thumb_rx / thumb_ry carry
  //             the turn and pitch as a signed 8-bit code (only the stick high
  //             bytes cross the network - a 16-bit value arrived rounded, so
  //             the two PCs turned by different amounts). The code is
  //             companded (NetLookDegrees): fine steps for small movements,
  //             about 17 degrees for the largest, at about 30 input frames a
  //             second. Whatever one code cannot express carries over to the
  //             next frame, so no movement is lost.
  //  - receive: after sub_8225D9F8 turns a frame's input into a pad record,
  //             the code is kept per pad and the right stick is centred in
  //             the record (never in the input buffer, which the game may
  //             still send or compare).
  //  - apply:   sub_82187900 (right stick X) is read by the player input step
  //             (sub_820F2988) with the current player set; the kept steps are
  //             added to that player's view angles there, once.
  // Every PC runs the same code on the same data at the same point, so the
  // angles stay identical. Offline play keeps direct look as before.
  static constexpr uint32_t kNetInputActive = 0x826646D1;  // netgame +6241
  static constexpr uint32_t kPadRecords = 0x826540E8;      // 4 x 68 bytes
  static constexpr uint32_t kPadRecordSize = 68;
  // g_Vars (0x82648BF8): players[] at +100, currentplayer at +1188 (set by
  // setCurrentPlayerNum, sub_821F5400); +1192 is currentplayerstats.
  static constexpr uint32_t kCurrentPlayerPtr = 0x8264909C;
  static constexpr uint16_t kNetLookFlag = 0x0400;
  static constexpr float kNetLookUnit = 0.02f;
  static constexpr float kNetLookCurve = 0.0009f;

  // Degrees for a code in -127..127. The receiver uses this exact function.
  static float NetLookDegrees(int code) {
    const float m = float(code < 0 ? -code : code);
    const float deg = kNetLookUnit * m + kNetLookCurve * m * m;
    return code < 0 ? -deg : deg;
  }

  // Lockstep input is in use when the input poll (sub_8225E910) takes the
  // network path - its own test, a flag at [0x826646D4] + 1436 - and that path
  // is feeding received input (netgame +6241). The netgame byte alone stays
  // set in Combat Simulator, which polls the controllers locally.
  static bool NetInputActive(rex::memory::Memory* mem) {
    const auto* p = mem->TranslateVirtual<const uint8_t*>(kNetInputActive);
    if (!p || *p == 0) return false;
    const uint32_t mode = ReadGuestU32(mem, 0x826646D4);
    if (mode < 0x40000000 || mode >= 0xA0000000) return false;
    const auto* flag = mem->TranslateVirtual<const uint8_t*>(mode + 1436);
    return flag && *flag != 0;
  }

  // sin/cos computed the same way on every CPU (the C runtime picks different
  // code for different processors, and the angles must match bit for bit).
  static void NetLookSinCos(double deg, double* s, double* c) {
    constexpr double kPi = 3.14159265358979323846;
    double x = deg * kPi / 180.0;
    while (x > kPi) x -= 2.0 * kPi;
    while (x < -kPi) x += 2.0 * kPi;
    double term_s = x, term_c = 1.0, sum_s = x, sum_c = 1.0;
    const double x2 = x * x;
    for (int n = 1; n < 20; ++n) {
      term_s *= -x2 / double((2 * n) * (2 * n + 1));
      term_c *= -x2 / double((2 * n - 1) * (2 * n));
      sum_s += term_s;
      sum_c += term_c;
    }
    *s = sum_s;
    *c = sum_c;
  }

 public:
  // input: guest address of the 12-byte gamepad about to be queued for
  // `frame`. Guest thread.
  static void NetLookSend(uint32_t input, uint32_t frame) {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    if (rex::cvar::GetFlagByName("mnk_direct_look") != "true" || rex::cvar::GetFlagByName("mnk_mode") != "true") return;
    auto* mem = app->runtime()->memory();
    auto* pad = mem->TranslateVirtual<uint8_t*>(input);
    if (!pad) return;
    std::lock_guard<std::mutex> lock(net_look_mutex_);
    if (!net_look_have_frame_ || frame != net_look_frame_) {
      // A new frame: take up to one frame's worth of the banked movement.
      auto take = [](float* banked) {
        int best = 0;
        float best_err = *banked < 0.0f ? -*banked : *banked;
        for (int q = -127; q <= 127; ++q) {
          float err = *banked - NetLookDegrees(q);
          if (err < 0.0f) err = -err;
          if (err < best_err) {
            best_err = err;
            best = q;
          }
        }
        *banked -= NetLookDegrees(best);
        return int8_t(best);
      };
      net_look_q_theta_ = take(&net_look_theta_);
      net_look_q_verta_ = take(&net_look_verta_);
      net_look_frame_ = frame;
      net_look_have_frame_ = true;
    }
    // The same frame queued again gets the same values, not new movement.
    // Without mouse movement the pad goes out untouched, so a gamepad's right
    // stick and the right-stick keys keep working.
    if (net_look_q_theta_ == 0 && net_look_q_verta_ == 0) return;
    uint16_t buttons = uint16_t(pad[0] << 8 | pad[1]);
    buttons |= kNetLookFlag;
    pad[0] = uint8_t(buttons >> 8);
    pad[1] = uint8_t(buttons);
    pad[8] = uint8_t(net_look_q_theta_);
    pad[9] = 0;
    pad[10] = uint8_t(net_look_q_verta_);
    pad[11] = 0;
  }

  // record: guest address of the pad record being filled; input: that frame's
  // 12-byte gamepad (identical on every PC).
  static void NetLookReceive(uint32_t record, uint32_t input) {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    if (record < kPadRecords || record >= kPadRecords + 4 * kPadRecordSize) return;
    const uint32_t index = (record - kPadRecords) / kPadRecordSize;
    auto* mem = app->runtime()->memory();
    NetLookPending& pending = net_look_pending_[index];
    // While the game waits for the other PC it has no input buffer (0).
    if (input < 0x40000000) {
      pending.valid = false;
      return;
    }
    const auto* pad = mem->TranslateVirtual<const uint8_t*>(input);
    auto* rec = mem->TranslateVirtual<uint8_t*>(record);
    if (!pad || !rec) return;
    uint16_t buttons = uint16_t(pad[0] << 8 | pad[1]);
    if (!(buttons & kNetLookFlag)) {
      pending.valid = false;
      return;
    }
    // An input frame drives two game ticks, and this runs before each; the
    // turn belongs to the input frame, so it is applied once (NetLookApply).
    pending.frame = ReadGuestU32(mem, input + 12);
    pending.theta = int8_t(pad[8]);
    pending.verta = int8_t(pad[10]);
    pending.valid = pending.theta != 0 || pending.verta != 0;
    if (pending.valid) {
      static int logged = 0;
      if (logged < 40) {
        ++logged;
        REXLOG_INFO("[netlook] pad {} frame {} turn code {} pitch code {}", index, pending.frame,
                    pending.theta, pending.verta);
      }
    }
    // Centre the right stick in the record: held-direction bits (+8: X
    // 0x00100000 / 0x00200000, Y 0x4 / 0x8), scaled bytes (+24 X, +25 Y) and
    // the dead-zoned values (+28 X, +30 Y).
    uint32_t held = uint32_t(rec[8]) << 24 | uint32_t(rec[9]) << 16 | uint32_t(rec[10]) << 8 | rec[11];
    held &= ~uint32_t(0x00300000 | 0x0000000C);
    rec[8] = uint8_t(held >> 24);
    rec[9] = uint8_t(held >> 16);
    rec[10] = uint8_t(held >> 8);
    rec[11] = uint8_t(held);
    rec[24] = rec[25] = 0;
    rec[28] = rec[29] = rec[30] = rec[31] = 0;
  }

  // input: the local pad's 12-byte gamepad the game is about to send (pad
  // record + 44). The game sends the controller slot assigned to the local
  // player; when nothing is plugged into that slot, send the first connected
  // one instead, so keyboard and mouse (always slot 0) or a lone gamepad in
  // another slot drive the local player in every networked mode.
  static uint32_t NetLocalInput(uint32_t input) {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return input;
    auto* mem = app->runtime()->memory();
    const uint32_t record = input - 44;
    if (record < kPadRecords || record >= kPadRecords + 4 * kPadRecordSize) return input;
    const uint32_t slot = (record - kPadRecords) / kPadRecordSize;
    auto connected = [&](uint32_t i) {
      const auto* p = mem->TranslateVirtual<const uint8_t*>(kPadRecords + i * kPadRecordSize + 1);
      return p && *p != 0;
    };
    uint32_t use = slot;
    if (!connected(slot)) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (connected(i)) {
          use = i;
          break;
        }
      }
    }
    static uint32_t logged_slot = ~0u, logged_use = ~0u;
    if (slot != logged_slot || use != logged_use) {
      logged_slot = slot;
      logged_use = use;
      REXLOG_INFO("[netinput] local player's controller slot {} ({}), sending slot {}", slot,
                  connected(slot) ? "connected" : "empty", use);
    }
    return kPadRecords + use * kPadRecordSize + 44;
  }

  // pad: controller number whose right stick the player input step reads.
  static void NetLookApply(uint32_t pad_index) {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    if (pad_index >= 4) return;
    auto* mem = app->runtime()->memory();

    float turn = 0.0f, pitch = 0.0f;
    if (NetInputActive(mem)) {
      // Lockstep: an input frame's turn is spread over the game ticks that use
      // it - half on the first, the rest on the next - so the view moves every
      // tick as in single player. Every PC does the same arithmetic in the
      // same ticks.
      NetLookPending& pending = net_look_pending_[pad_index];
      NetLookSpread& spread = net_look_spread_[pad_index];
      if (pending.valid && pending.frame != spread.frame) {
        spread.frame = pending.frame;
        spread.theta += NetLookDegrees(pending.theta);
        spread.verta += NetLookDegrees(pending.verta);
        spread.ticks = 0;
      }
      pending.valid = false;
      if (spread.theta == 0.0f && spread.verta == 0.0f) return;
      if (spread.ticks++ == 0) {
        turn = spread.theta * 0.5f;
        pitch = spread.verta * 0.5f;
      } else {
        turn = spread.theta;
        pitch = spread.verta;
      }
      spread.theta -= turn;
      spread.verta -= pitch;
    } else {
      // Not lockstep (Combat Simulator): each PC runs its own player, so this
      // PC's mouse turns the player whose input step reads the keyboard and
      // mouse controller slot.
      if (pad_index != uint32_t(atoi(rex::cvar::GetFlagByName("mnk_user_index").c_str()))) return;
      std::lock_guard<std::mutex> lock(net_look_mutex_);
      turn = local_look_theta_;
      pitch = local_look_verta_;
      local_look_theta_ = 0.0f;
      local_look_verta_ = 0.0f;
      if (turn == 0.0f && pitch == 0.0f) return;
    }

    uint32_t player = ReadGuestU32(mem, kCurrentPlayerPtr);
    if (player < 0x40000000 || player >= 0x50000000) return;
    const uint32_t a = player + kThetaOffset;
    float theta = ReadGuestF32(mem, a);
    float verta = ReadGuestF32(mem, a + 0x10);
    if (!(std::isfinite(theta) && std::isfinite(verta))) return;
    theta += turn;
    while (theta < 0.0f) theta += 360.0f;
    while (theta >= 360.0f) theta -= 360.0f;
    verta += pitch;
    if (verta > kMaxPitchDegrees) verta = kMaxPitchDegrees;
    if (verta < -kMaxPitchDegrees) verta = -kMaxPitchDegrees;
    double sn = 0.0, c = 1.0;
    NetLookSinCos(double(theta), &sn, &c);
    WriteGuestF32(mem, a, theta);
    WriteGuestF32(mem, a + 8, float(c));
    WriteGuestF32(mem, a + 12, float(sn));
    WriteGuestF32(mem, a + 0x10, verta);
    WriteGuestF32(mem, a + 0x14, verta < 0.0f ? verta + 360.0f : verta);
    WriteGuestF32(mem, a + 4, 0.0f);     // speedtheta
    WriteGuestF32(mem, a + 0x18, 0.0f);  // speedverta
    WriteGuestF32(mem, kPrevThetaOffset - kThetaOffset + a, theta);
  }

 private:
  struct NetLookPending {
    bool valid;
    int8_t theta;
    int8_t verta;
    uint32_t frame;
  };
  struct NetLookSpread {
    uint32_t frame;
    float theta;
    float verta;
    int ticks;
  };
  static inline NetLookSpread net_look_spread_[4] = {
      {~0u, 0.0f, 0.0f, 0}, {~0u, 0.0f, 0.0f, 0}, {~0u, 0.0f, 0.0f, 0}, {~0u, 0.0f, 0.0f, 0}};
  static inline float local_look_theta_ = 0.0f;
  static inline float local_look_verta_ = 0.0f;
  static inline std::mutex net_look_mutex_;
  static inline float net_look_theta_ = 0.0f;
  static inline float net_look_verta_ = 0.0f;
  static inline int8_t net_look_q_theta_ = 0;
  static inline int8_t net_look_q_verta_ = 0;
  static inline uint32_t net_look_frame_ = 0;
  static inline bool net_look_have_frame_ = false;
  static inline NetLookPending net_look_pending_[4];
  // Mouse in menus: the pointer lives in the menu's own 640 x 240 space.
  static inline bool menu_mouse_active_ = false;
  static inline float menu_cursor_x_ = 0.0f;
  static inline float menu_cursor_y_ = 0.0f;
  static inline float menu_cursor_dx_ = 0.0f;
  static inline float menu_cursor_dy_ = 0.0f;
  static inline bool menu_lmb_was_down_ = false;
  static inline bool menu_rmb_was_down_ = false;
  static inline int menu_hover_row_ = -1;
  // A menu of the game's own has been on screen at least once since launch.
  static inline bool menu_ever_open_ = false;
 public:
  // Every game frame (PdNoteGameFrame): a load of the Carrington Institute
  // holds the aim until a menu opens there. A load is a new stage number, or
  // the level's frame counter (g_Vars+0x0C, which counts up every frame and
  // starts again at each load - found by sampling g_Vars over the title loop)
  // going back: Return to Main Menu from inside the Institute reloads stage 38
  // without the number changing (user, 2026-10-27).
  static void NoteGameStage(uint32_t stage, uint32_t level_frames) {
    static uint32_t last_stage = 0;
    static uint32_t last_frames = 0;
    const bool reloaded = stage != last_stage || level_frames < last_frames;
    last_stage = stage;
    last_frames = level_frames;
    if (reloaded && stage == kStageInstitute) institute_menu_pending_ = true;
  }
 private:
  // The Carrington Institute (the menus' stage) has loaded and no menu has
  // opened in it yet: its walk-in is playing (OnMouseLookDelta).
  static inline std::atomic<bool> institute_menu_pending_{false};
  static inline bool title_start_pressed_ = false;  // START given on the title or film
  static constexpr uint32_t kStageInstitute = 38;
  // The attract film is still showing (no menu has been opened since the title).
  static inline bool menu_attract_ = false;
  static inline std::chrono::steady_clock::time_point menu_open_at_ =
      std::chrono::steady_clock::now() - std::chrono::seconds(10);
  static inline std::atomic<int> menu_wheel_{0};
  // Steps waiting to be handed to a list (from the wheel).
  static inline int menu_list_steps_ = 0;
  // The left button is down on a slider that is open, so the mouse moves it,
  // and whether this drag has moved it at all.
  static inline bool menu_slider_drag_ = false;
  static inline bool menu_slider_moved_ = false;
  static inline bool menu_slider_was_open_ = false;
  // Movement the mouse has asked for that the slider has not made yet, and
  // the paying off of it once the button is let go.
  static inline float menu_slider_owed_ = 0.0f;
  static inline bool menu_slider_paying_ = false;
  static inline bool menu_slider_close_ = false;
  static inline std::chrono::steady_clock::time_point menu_slider_paid_at_ =
      std::chrono::steady_clock::now();
  static inline std::chrono::steady_clock::time_point menu_slider_paying_until_ =
      std::chrono::steady_clock::now();
  static inline std::chrono::steady_clock::time_point menu_list_step_at_ =
      std::chrono::steady_clock::now();
  // Menu units down per unit across, for an even pointer speed (DrawMenuCursor).
  static inline std::atomic<float> menu_dy_scale_{0.61f};
  static inline MenuLayout menu_layout_ = {0, 0.0f, 0.0f, 0.0f, 0.0f, false, false, {}};

 public:
  // TEMP_LIGHT_DIAG: dark-room / IR-scanner investigation.
  //
  // Hypothesis: when the game darkens a room (or the IR scanner reveals an
  // object) it rewrites data the GPU emulation has already uploaded, and some
  // of it is never re-uploaded - so those draws keep their old, lit look.
  // These keys force a refresh at two different layers. Whichever one makes
  // stuck-lit objects snap to the correct look names the stale layer.
  static rex::graphics::GraphicsSystem* ConcreteGraphics() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->graphics_system()) return nullptr;
    // Safe: rex::graphics::GraphicsSystem is the only IGraphicsSystem here.
    return static_cast<rex::graphics::GraphicsSystem*>(app->runtime()->graphics_system());
  }

  // F5: mark ALL guest memory as needing re-upload to the GPU. Fixes it if the
  // GPU's copy of guest RAM (vertex colours, textures) has gone stale.
  static void DebugInvalidateGpuMemory() {
    if (auto* g = ConcreteGraphics()) {
      g->InvalidateGpuMemory();
      REXLOG_INFO("[lightdiag] F5 InvalidateGpuMemory: all guest memory marked for re-upload");
    }
  }

  // F6: drop the texture cache and vertex-buffer residency. Fixes it if the
  // stale part is cached derived state rather than the raw memory copy.
  static void DebugClearGpuCaches() {
    if (auto* g = ConcreteGraphics()) {
      g->ClearCaches();
      REXLOG_INFO("[lightdiag] F6 ClearCaches: texture cache + vertex residency dropped");
    }
  }

  static inline std::vector<uint32_t> look_candidates_;
  static inline std::vector<float> look_candidate_theta_;

  static void WriteGuestBytes(rex::memory::Memory* mem, uint32_t addr, const char* s,
                              size_t len) {
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(addr);
    if (!p) return;
    memcpy(p, s, len);
  }

  static void WriteGuestU32(rex::memory::Memory* mem, uint32_t addr, uint32_t value) {
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(addr);
    if (!p) return;
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
  }

  // TEMP_MENU_SCAN: proof that the native menu is editable.
  //
  // The guest's menu system is the N64 one, compiled as static data in the
  // image (found by scanning for 20-byte records whose last field is a code
  // pointer, then cross-validating that a 24-byte `menudialogdef` points back
  // at the array start):
  //
  //   struct menuitem     { u8 type; u32 flags; u32 param2; u32 param3; ptr handler; }  // 20 bytes
  //   struct menudialogdef{ u8 type; u32 title_textid; menuitem* items; ... }           // 24 bytes
  //
  // Item types decoded empirically against what Display Options renders on
  // screen: 0x09 = checkbox/toggle, 0x0C = spinner (label + cycling value),
  // 0x1D = end-of-array terminator. param2 is the label's 16-bit text ID
  // (bank<<9 | index, exactly as N64 lang.c describes).
  //
  // There are three "Video Options" dialogdefs (title tid 0x56D6), one per
  // context. For each, this copies its item array into scratch, appends one
  // extra spinner item, terminates it, and repoints the dialogdef at the copy.
  // Nothing is overwritten in place - the originals are left intact.
  // TEMP_MENU_SCAN: probe what's actually writable before trying to patch.
  // The first attempt crashed with no log output, which points at the write
  // itself faulting - the menu arrays are plausibly in read-only .rodata.
  static void ProbeMenuMemory() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[menu_probe] runtime/memory not ready");
      return;
    }
    auto* mem = app->runtime()->memory();
    const uint32_t kAddrs[] = {
        0x825DA188,  // dialogdef (Video Options #1)
        0x825DA1AC,  // dialogdef #2 items pointer field
        0x825DA160,  // item array
        0x825BF000,  // thunk-reserve scratch candidate
        0x82674607,  // large zero run in data
        0x8266AD08,  // another large zero run
    };
    for (uint32_t a : kAddrs) {
      auto* heap = mem->LookupHeap(a);
      if (!heap) {
        REXLOG_INFO("[menu_probe] 0x{:08X}: no heap", a);
        continue;
      }
      rex::memory::HeapAllocationInfo info{};
      bool ok = heap->QueryRegionInfo(a, &info);
      uint32_t before = ReadGuestU32(mem, a);
      bool wrote = false;
      if (ok) {
        __try {
          WriteGuestU32(mem, a, before);  // write back the same value
          wrote = (ReadGuestU32(mem, a) == before);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          wrote = false;
        }
      }
      REXLOG_INFO(
          "[menu_probe] 0x{:08X}: query={} state=0x{:X} protect=0x{:X} value=0x{:08X} writable={}",
          a, ok, ok ? info.state : 0u, ok ? info.protect : 0u, before, wrote);
    }
  }

  // True once the game's text bank holds `find`. The Settings rows reuse two of
  // its strings, so the menu patch has to wait until it is loaded. memchr keeps
  // the sweep cheap, and it stops at the first hit.
  static bool GuestStringPresent(rex::memory::Memory* mem, const char* find) {
    const size_t flen = strlen(find);
    bool found = false;
    ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
      if (found || region_size < flen) return;
      uint8_t* host = mem->TranslateVirtual<uint8_t*>(region_addr);
      if (!host) return;
      const uint8_t* p = host;
      const uint8_t* last = host + region_size - flen;
      while (p <= last) {
        p = static_cast<const uint8_t*>(memchr(p, find[0], size_t(last - p) + 1));
        if (!p) break;
        if (memcmp(p, find, flen) == 0) {
          found = true;
          return;
        }
        ++p;
      }
    });
    return found;
  }

  // Installs the native menu rows (Video Options and PC Controls) without the
  // player pressing F11. Polled from the UI thread about once a second until the
  // text bank is loaded, then patched exactly once.
  static void TickAutoMenuPatch() {
    static int frames = 0;
    static int checks = 0;
    static bool done = false;
    if (done || ++frames % 60 != 0) return;
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    ++checks;
    if (!GuestStringPresent(app->runtime()->memory(), "Controller Pak Menu\n")) {
      if (checks >= 180) {
        REXLOG_INFO("[menu_patch] text bank never appeared; PC menu rows not installed");
        done = true;
        menu_patched_ = true;
      }
      return;
    }
    done = true;
    PatchPlayerSetupMenus();
    PatchGamerCardPrompt();
    menu_patched_ = true;
    REXLOG_INFO("[menu_patch] text bank found after {} check(s); installing PC menu rows",
                checks);
    PatchVideoOptionsMenu();
  }

  // Xbox LIVE wording in the XBLA text bank. No Xbox LIVE service is involved,
  // so Match Type's entries become Local Multiplayer / Host Online Match /
  // Find Online Match (the Quick Match slot is reused for the server browser -
  // see PdMatchTypeHandler), with the descriptions, the How To Play pages and
  // the leaderboard wording to match. The lobby's "Invite Friends" button
  // keeps its name: netplay::OnInviteFriends tells the player how friends
  // join. Every replacement is shorter than the original, so it is written in
  // place. Polled like TickAutoMenuPatch until the text bank is loaded.
  //
  // Order matters: a phrase is rewritten wherever it appears, so the long
  // sentences that quote a menu name come before the menu name itself.
  //
  // Not every phrase is in memory at once: the long How To Play pages only
  // appear once that screen has been opened. Phrases that are not found yet
  // are retried every five seconds until they are.
  static constexpr int kLabelRetryFrames = 300;
  static constexpr int kLabelRetries = 240;  // twenty minutes
  static void TickLobbyLabels() {
    static int frames = 0;
    static int checks = 0;
    static bool started = false;
    static bool done = false;
    if (done) return;
    ++frames;
    if (started ? (frames % kLabelRetryFrames != 0) : (frames % 60 != 0)) return;
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    if (!started) {
      if (!GuestStringPresent(mem, "Xbox LIVE Private Match")) {
        if (++checks >= 180) {
          REXLOG_INFO("[lobby] XBLA text bank never appeared; lobby labels left as they are");
          done = true;
        }
        return;
      }
      started = true;
    }
    static GuestSwap kLabels[] = {
        // How To Play. These quote the Match Type entries, so they have to be
        // rewritten before the entries themselves are renamed.
        {"Xbox LIVE Private Match: Social matches with friends over Xbox LIVE.",
         "Host Match: Host a match others can join over LAN or Direct Connect."},
        {"Xbox LIVE Quick Match: Competitive skill-based matchmaking over Xbox LIVE.",
         "Join Match: Join a game online, on your LAN, or by Direct Connect."},
        {"You can drop straight into an Xbox LIVE match by choosing Xbox LIVE Quick Match",
         "You can join a match someone else is hosting by choosing Join Match"},
        {"in Local Multiplayer and Xbox LIVE Private Matches by choosing",
         "in Local Multiplayer and online matches by choosing"},
        {"Perfect Dark supports up to 8 players over Xbox LIVE.",
         "Perfect Dark supports up to 8 players online."},
        {"Co-Op mode supports Local multiplayer via Split Screen as well as multiplayer over "
         "Xbox LIVE.",
         "Co-Op mode supports split-screen multiplayer as well as online play."},
        {"Counter-Operative mode supports Local multiplayer via Split Screen as well as "
         "multiplayer over Xbox LIVE.",
         "Counter-Operative mode supports split-screen multiplayer as well as online play."},
        // Combat Simulator's Player Setup: the page now holds every setting
        // that is drawn on screen, so it is named for that.
        {"Player Options", "HUD Options"},
        // Match Type and its descriptions.
        {"Xbox LIVE Private Match", "Host Match"},
        {"Xbox LIVE Quick Match", "Join Match"},
        {"Social matches with friends over Xbox LIVE", "Host a match others can join"},
        {"Competitive skill-based matchmaking over Xbox LIVE",
         "LAN or Direct Connect"},
        {"Split-screen with up to four players on one Xbox 360 console",
         "Split-screen with up to four players on one PC"},
        // Co-operative / Counter-operative use the same online play.
        {"Split-screen with up to two players on one Xbox 360 console",
         "Split-screen with up to two players on one PC"},
        {"Co-operate with another player over Xbox LIVE", "Co-operate with another player online"},
        {"Compete against another player over Xbox LIVE", "Compete against another player online"},
        {"Join an Xbox LIVE game", "Join an online game"},
        // Nobody can be signed out of a service the port does not use, but the
        // game still has these to hand.
        {"multiplayer games on Xbox LIVE.", "multiplayer games online."},
        {"%s is not signed in with an online gamer profile,", "%s has no online profile,"},
        // Leaderboards rank only the player on this PC, so the Xbox LIVE
        // column header and the friends wording mean nothing here.
        {"Gamertag", "Player"},
        {"Your Friends Are Not Yet Ranked On This Leaderboard", "No Scores On This Board Yet"},
        {"Your gamer profile is currently offline. To send your score to the online leaderboard, "
         "press the Xbox Guide button and connect to Xbox LIVE.",
         "Your scores are kept on this PC and are never sent anywhere."},
        {"Gamer profile offline", "Local leaderboards"},
        // The board's three filters all show the same local scores, so the
        // friends one is named for where they are kept instead. Whole-string:
        // "Friends" is also the end of the lobby's "Invite Friends" button.
        {"Friends", "This PC", true},
        // No Xbox LIVE party to invite; netplay::OnInviteFriends answers this
        // button too, with the same "here is how friends join" message.
        {"Invite Xbox LIVE Party", "Invite Friends"},
        // The gamer card is answered locally now (XamShowGamerCardUIForXUID),
        // so the button says what it actually shows.
        {"View Gamer Profile", "View Player Info"},
        {"View Gamer Card", "Player Info"},
    };
    constexpr size_t kLabelCount = sizeof(kLabels) / sizeof(kLabels[0]);
    static int retries = 0;
    const int64_t started_ms = NowMs();
    ReplaceGuestStrings(mem, kLabels, kLabelCount);
    int remaining = 0;
    for (size_t i = 0; i < kLabelCount; ++i) {
      if (kLabels[i].copies == 0) ++remaining;
    }
    REXLOG_INFO("[lobby] pass {}: {} of {} phrases rewritten in {} ms", retries + 1,
                int(kLabelCount) - remaining, int(kLabelCount), NowMs() - started_ms);
    if (remaining == 0) {
      done = true;
    } else if (++retries >= kLabelRetries) {
      done = true;
      for (size_t i = 0; i < kLabelCount; ++i) {
        if (kLabels[i].copies == 0) REXLOG_INFO("[lobby] never found: \"{}\"", kLabels[i].from);
      }
    }
  }

  // Setup extra "100% story completion" (pd_story_complete): every solo
  // mission counts as completed on every difficulty. The game file keeps best
  // times as u16 seconds, 0 meaning not completed, in besttimes[21 stages][3
  // difficulties] at 0x826420C0 (g_GameFile 0x826420A0 + 0x20, as read by
  // cheatIsUnlocked, sub_8219D1E0). A mission with no time gets 59:59; real
  // times are left alone. Re-applied every two seconds because loading a save
  // replaces the game file; the game's own saves then keep it.
  static constexpr uint32_t kBestTimes = 0x826420C0;
  static constexpr int kSoloStages = 21;
  static constexpr uint16_t kUntimedBest = 59 * 60 + 59;
  static void TickStoryComplete() {
    static int frames = 0;
    if (!REXCVAR_GET(pd_story_complete) || ++frames % 120 != 0) return;
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    uint8_t* times = app->runtime()->memory()->TranslateVirtual<uint8_t*>(kBestTimes);
    if (!times) return;
    int filled = 0;
    for (int i = 0; i < kSoloStages * 3; ++i) {
      uint8_t* t = times + i * 2;
      if (t[0] == 0 && t[1] == 0) {
        t[0] = uint8_t(kUntimedBest >> 8);
        t[1] = uint8_t(kUntimedBest & 0xFF);
        ++filled;
      }
    }
    if (filled) {
      REXLOG_INFO("[extras] story completion: filled {} mission best time(s)", filled);
    }
  }

  // Host side of the server browser: the mode and arena this PC's match shows
  // in other players' lists, read once a second from the game's multiplayer
  // setup (g_MpSetup at 0x82641898 in the N64 layout: scenario at +16,
  // stagenum at +17 - the arena option code writes +17) and named through
  // the arena table (0x825DD560, 17 x 6 bytes: stage, feature, pad, name tid)
  // and the scenario names (tids 0x50F6.., Combat to Capture the Case).
  static constexpr uint32_t kMpSetup = 0x82641898;
  static constexpr uint32_t kArenaTable = 0x825DD560;
  static constexpr int kArenaCount = 17;
  static constexpr uint32_t kScenarioTid = 0x50F6;

  // A string from the game's text table (sub_821886E8's lookup): bank
  // id >> 9, index id & 0x1FF, slot = bank start (u32s at 0x825F76A8) +
  // index; banks past 70 share bank 70's start + 512. 0 if not loaded.
  static uint32_t GuestTextAddress(rex::memory::Memory* mem, uint32_t tid) {
    constexpr uint32_t kBankStarts = 0x825F76A8;
    constexpr uint32_t kTablePtr = 0x825F77D0;
    const uint32_t bank = tid >> 9;
    const uint32_t index = tid & 0x1FF;
    if (bank == 0) return 0;
    const uint32_t slot = bank <= 70 ? ReadGuestU32(mem, kBankStarts + (bank - 1) * 4) + index
                                     : ReadGuestU32(mem, kBankStarts + 69 * 4) + 512 + index;
    const uint32_t table = ReadGuestU32(mem, kTablePtr);
    const uint32_t ptr = table ? ReadGuestU32(mem, table + slot * 4) : 0;
    return (ptr < 0x40000000 || ptr >= 0xA0000000) ? 0 : ptr;
  }

  // Replaces a text-table string in place (the new text no longer than the
  // old), found by its text id. ReplaceGuestString walks all of guest memory,
  // about 0.45 s on the UI thread each time; five of them at start-up froze
  // the window on the boot screens for over two seconds (HANDOVER 4aq). Falls
  // back to the walk if the string is not where its id says.
  static int ReplaceTextById(rex::memory::Memory* mem, uint32_t tid, const char* find,
                             const char* repl) {
    const uint32_t ptr = GuestTextAddress(mem, tid);
    char* p = ptr ? mem->TranslateVirtual<char*>(ptr) : nullptr;
    const size_t flen = std::strlen(find);
    const size_t rlen = std::strlen(repl);
    if (p && rlen <= flen && std::memcmp(p, find, flen) == 0) {
      std::memcpy(p, repl, rlen + 1);  // includes the NUL
      return 1;
    }
    return ReplaceGuestString(mem, find, repl);
  }

  static std::string GuestText(rex::memory::Memory* mem, uint32_t tid) {
    const uint32_t ptr = GuestTextAddress(mem, tid);
    if (!ptr) return {};
    const auto* p = mem->TranslateVirtual<const uint8_t*>(ptr);
    std::string text;
    for (int i = 0; p && i < 40 && p[i] && p[i] != '\n'; ++i) text += char(p[i]);
    return text;
  }

  static void TickMatchDetails() {
    namespace netplay = rex::kernel::xam::netplay;
    static int frames = 0;
    if (++frames % 60 != 0 || !netplay::Enabled()) return;
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint8_t* setup = mem ? mem->TranslateVirtual<const uint8_t*>(kMpSetup) : nullptr;
    if (!setup) return;
    const int scenario = setup[16];
    const int stage = setup[17];
    const std::string mode = scenario < 6 ? GuestText(mem, kScenarioTid + uint32_t(scenario)) : "";
    std::string arena;
    for (int i = 0; i < kArenaCount; ++i) {
      const uint32_t record = kArenaTable + uint32_t(i) * 6;
      if (int(ReadGuestU32(mem, record) >> 16) == stage) {
        arena = GuestText(mem, ReadGuestU32(mem, record + 4) >> 16);
        break;
      }
    }
    // While the game reloads its setup (menus, loading) both read back empty;
    // keep showing the last good details rather than a blank match.
    if (mode.empty() && arena.empty()) return;
    static std::string last;
    if (mode + "|" + arena != last) {
      last = mode + "|" + arena;
      REXLOG_INFO("[browser] this PC's match shows as \"{}\" on \"{}\" (scenario {}, stage {:02X})",
                  mode, arena, scenario, stage);
    }
    netplay::SetMatchDetails(arena, mode);
  }

  // Developer aid (pd_frame_stats, or the bug-report switch diagnostics): every five seconds, logs how many frames
  // the window presented, the average rate, the slowest frame and how many
  // took longer than 20 ms (a missed 60 Hz frame). Measured between calls of
  // the UI draw hook, which runs once per presented frame.
  static void TickFrameStats() {
    if (!REXCVAR_GET(pd_frame_stats) && !rex::diag::TimingLogOn()) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point window_start = clock::now();
    static clock::time_point last = window_start;
    static int frames = 0;
    static int slow = 0;
    static double worst_ms = 0.0;
    const clock::time_point now = clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    ++frames;
    if (ms > 20.0) ++slow;
    if (ms > worst_ms) worst_ms = ms;
    const double window = std::chrono::duration<double>(now - window_start).count();
    if (window >= 5.0) {
      // The game's own frames (its speed follows these) next to the frames
      // the window presented (the monitor's rate with vsync).
      static uint64_t guest_start = 0;
      PerfectDarkApp* app = current_instance_;
      auto* gs = (app && app->runtime()) ? app->runtime()->graphics_system() : nullptr;
      const uint64_t guest_now = gs ? gs->guest_frame_count() : 0;
      const double guest_fps = guest_start ? double(guest_now - guest_start) / window : 0.0;
      guest_start = guest_now;
      REXLOG_INFO("[frame_stats] game {:.1f} fps; window {} frames in {:.1f} s: {:.1f} fps, "
                  "slowest {:.1f} ms, {} over 20 ms",
                  guest_fps, frames, window, frames / window, worst_ms, slow);
      window_start = now;
      frames = 0;
      slow = 0;
      worst_ms = 0.0;
    }
  }

  // Opens the match list once a Join search has heard several hosts.
  void TickMatchPicker() {
    if (!imgui_drawer_ || match_picker_open_ ||
        !rex::kernel::xam::netplay::PendingMatchChoice(nullptr)) {
      return;
    }
    match_picker_open_ = true;
    imgui_drawer_->AddDialog(new PerfectDarkMatchPicker(imgui_drawer_, this));
  }
  void OnMatchPickerClosed() { match_picker_open_ = false; }

  // Shows the latest Private Match search message ("Looking for a Private
  // Match...", "No Private Match found...") at the top of the game window for
  // a few seconds, in the menus' navy and cyan.
  // ---- "Press ANY KEY To Play" ---------------------------------------------
  // On the title and its attract film, any key is START (as a left click
  // already was), and without a controller the prompt says so (user,
  // 2026-10-25). Keys bound to START or A are left out - they reach the game as
  // those buttons already - and so are Alt, Shift, Ctrl, the Windows and menu
  // keys, Print Screen and the reserved F4/F7/F9, so switching windows or
  // taking a screenshot does not start the game. A key already held when the
  // title appears (Esc backing out to it) does not count until it is let go.
  static bool TitleAnyKeyPressed(bool active) {
    static bool was_down[256] = {};
    static std::chrono::steady_clock::time_point last_call{};
    const auto now = std::chrono::steady_clock::now();
    const bool fresh = now - last_call > std::chrono::milliseconds(250);
    last_call = now;
    if (!active) return false;
    bool skip[256] = {};
    for (const char* bind : {"keybind_start", "keybind_a"}) {
      for (const std::string& key : SplitKeys(rex::cvar::GetFlagByName(bind))) {
        const uint32_t vk = uint32_t(rex::ui::ParseVirtualKey(key));
        if (vk > 0 && vk < 256) skip[vk] = true;
      }
    }
    for (int vk : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU,
                   VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN, VK_APPS, VK_SNAPSHOT, VK_F4, VK_F7,
                   VK_F9}) {
      skip[vk] = true;
    }
    bool pressed = false;
    for (int vk = 0x08; vk < 0xFF; ++vk) {  // keyboard keys; mouse buttons are below 0x08
      const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
      if (down && !was_down[vk] && !skip[vk] && !fresh) pressed = true;
      was_down[vk] = down;
    }
    return pressed;
  }

  // The prompt itself comes from the game's text table (see GuestText): the
  // table entry for "Press START To Play" is pointed at our own string while no
  // gamepad is connected, and back at the game's when one is. The new text is
  // longer, so it cannot be written over the old one in place.
  static constexpr uint32_t kStrPressAnyKey = 0x826752D0;  // 32 bytes, scratch strings
  static inline uint32_t title_prompt_entry_ = 0;     // guest address of the table entry
  static inline uint32_t title_prompt_original_ = 0;  // the game's pointer
  static inline int title_prompt_shown_ = -1;         // -1 unknown, 0 START, 1 ANY KEY

  // The table entry whose text is exactly `text` (ending in NUL or a newline;
  // the XBLA strings, this one among them, have no newline), or 0.
  static uint32_t FindTextTableEntry(rex::memory::Memory* mem, const char* text) {
    constexpr uint32_t kBankStarts = 0x825F76A8;
    constexpr uint32_t kTablePtr = 0x825F77D0;
    const uint32_t table = ReadGuestU32(mem, kTablePtr);
    if (table < 0x40000000 || table >= 0xA0000000) return 0;
    uint32_t slots = 0;
    for (uint32_t bank = 0; bank < 70; ++bank) {
      slots = std::max(slots, ReadGuestU32(mem, kBankStarts + bank * 4) + 1024);
    }
    slots = std::min<uint32_t>(slots, 0x20000);
    const size_t length = std::strlen(text);
    for (uint32_t slot = 0; slot < slots; ++slot) {
      const uint32_t ptr = ReadGuestU32(mem, table + slot * 4);
      if (ptr < 0x40000000 || ptr >= 0xA0000000) continue;
      const auto* p = mem->TranslateVirtual<const char*>(ptr);
      if (p && std::memcmp(p, text, length) == 0 && (p[length] == 0 || p[length] == '\n')) {
        return table + slot * 4;
      }
    }
    return 0;
  }

  static void TickTitlePrompt() {
    if (!menu_patched_) return;
    static std::chrono::steady_clock::time_point next{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(1);
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    // The table is refilled when the game loads its text again, so check the
    // entry still holds one of the two pointers, and look it up again if not.
    if (title_prompt_entry_) {
      const uint32_t now_ptr = ReadGuestU32(mem, title_prompt_entry_);
      if (now_ptr != title_prompt_original_ && now_ptr != kStrPressAnyKey) {
        title_prompt_entry_ = 0;
        title_prompt_shown_ = -1;
      }
    }
    if (!title_prompt_entry_) {
      title_prompt_entry_ = FindTextTableEntry(mem, "Press START To Play");
      if (!title_prompt_entry_) return;  // the title's text is not loaded yet
      title_prompt_original_ = ReadGuestU32(mem, title_prompt_entry_);
      static const char kText[] = "Press ANY KEY To Play";
      WriteGuestBytes(mem, kStrPressAnyKey, kText, sizeof(kText));  // with its NUL
      title_prompt_shown_ = -1;
    }
    const int want = GamepadConnected() ? 0 : 1;
    if (want == title_prompt_shown_) return;
    WriteGuestU32(mem, title_prompt_entry_, want ? kStrPressAnyKey : title_prompt_original_);
    title_prompt_shown_ = want;
    REXLOG_INFO("[menu_patch] title prompt: Press {} To Play", want ? "ANY KEY" : "START");
  }

  // ---- Button prompts without a controller --------------------------------
  // The footer prompts - "(A) Select", "(B) Cancel" and the rest - only mean
  // something with a controller in hand. They come from one table in the
  // game's data, 28 five-word entries whose last two words are the button
  // icon and the text id (see PatchGamerCardPrompt). While no gamepad is
  // connected, every entry with a button icon is blanked and the footer draws
  // nothing; plugging one in puts the game's own values back. Entries without
  // an icon (plain text) are left alone, and the two player-info prompts,
  // already taken off at start-up, stay off.
  static constexpr uint32_t kPromptIcons = 0x82603AEC;  // icon word, first entry
  static constexpr int kPromptCount = 28;
  static inline uint32_t prompt_icon_[kPromptCount] = {};
  static inline uint32_t prompt_text_[kPromptCount] = {};
  static inline int prompt_state_ = -1;  // -1 unread, 0 shown, 1 hidden, 2 not recognised
  static inline bool menu_patched_ = false;

  // Any real gamepad: SDL sees every kind; XInput is asked as well in case
  // SDL's gamepad support did not start.
  static bool GamepadConnected() {
    if (rex::input::sdl::ConnectedGamepadCount() > 0) return true;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
      XINPUT_STATE state{};
      if (XInputGetState(i, &state) == ERROR_SUCCESS) return true;
    }
    return false;
  }

  static void TickControllerPrompts() {
    // After the start-up patches, so the snapshot has the player-info
    // prompts already gone and putting the table back cannot return them.
    if (!menu_patched_ || prompt_state_ == 2) return;
    static std::chrono::steady_clock::time_point next{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(1);
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    if (prompt_state_ == -1) {
      for (int i = 0; i < kPromptCount; ++i) {
        const uint32_t at = kPromptIcons + uint32_t(i) * 20;
        const uint32_t icon = ReadGuestU32(mem, at);
        const uint32_t text = ReadGuestU32(mem, at + 4);
        if (!(icon == 0 || (icon >= 0x0DB0 && icon <= 0x0DD0)) ||
            !(text == 0 || (text >= 0x5000 && text < 0x9000))) {
          REXLOG_WARN("[menu_patch] button prompt table not recognised (entry {}: "
                      "{:08X} {:08X}); prompts left as they are",
                      i, icon, text);
          prompt_state_ = 2;
          return;
        }
        prompt_icon_[i] = icon;
        prompt_text_[i] = text;
      }
      prompt_state_ = 0;  // as the game has them
    }
    const int want = GamepadConnected() ? 0 : 1;
    if (want == prompt_state_) return;
    int changed = 0;
    for (int i = 0; i < kPromptCount; ++i) {
      if (!prompt_icon_[i]) continue;  // plain text, or already taken off
      const uint32_t at = kPromptIcons + uint32_t(i) * 20;
      WriteGuestU32(mem, at, want ? 0 : prompt_icon_[i]);
      WriteGuestU32(mem, at + 4, want ? 0 : prompt_text_[i]);
      ++changed;
    }
    prompt_state_ = want;
    REXLOG_INFO("[menu_patch] {}: {} button prompts {}",
                want ? "no controller" : "controller connected", changed,
                want ? "hidden" : "shown");
  }

  // The version, small and faint in the bottom-left corner while the intro
  // film plays: the city and the ship with "Press START To Play". The title
  // sequence is two stages that take turns - stage 90 holds the boot notices,
  // the studio logos and the Perfect Dark logo, then the film runs as a real
  // stage (48) until the player presses START, and after a minute it goes back
  // round through the logos. So the label shows on the film only: the front
  // end as TickMenuMouse tracks it (menu_attract_), on any stage but the title
  // stage, with no menu open. The film fades in from black over about four
  // seconds, so the label waits a second and then fades in with it rather than
  // sitting over a black screen.
  // Pressing START on the film loads the Carrington Institute (its opening
  // scene, then the menus), and the front end counts as the film until a menu
  // opens - so the label stayed up over the Institute (user, 2026-10-27). The
  // film is therefore the stage entered straight from the title stage, and
  // only that stage shows the label.
  static void DrawTitleVersion() {
    static bool in_film = false;
    static std::chrono::steady_clock::time_point film_start;
    static uint32_t last_stage = 0;
    static uint32_t film_stage = 0;
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    const uint32_t stage = ReadGuestU32(app->runtime()->memory(), kStageNum);
    if (stage != last_stage) {
      film_stage = last_stage == kStageTitle && menu_attract_ ? stage : 0;
      last_stage = stage;
      if (stage == kStageTitle) title_start_pressed_ = false;
    }
    // A key pressed during the logos (the title stage) skips the film and loads
    // the Institute straight from the title stage, which then looked like the
    // film (user, 2026-10-27): once START has been given, no label until the
    // title sequence comes round again, and never over the Institute.
    const bool film = menu_attract_ && !menu_mouse_active_ && stage != 0 &&
                      stage != kStageTitle && stage != kStageInstitute &&
                      stage == film_stage && !title_start_pressed_;
    if (!film) {
      in_film = false;
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!in_film) {
      in_film = true;
      film_start = now;
    }
    const float seconds = std::chrono::duration<float>(now - film_start).count();
    const float fade = std::clamp((seconds - 1.0f) / 2.0f, 0.0f, 1.0f);
    if (fade <= 0.0f) return;
    ImGuiIO& io = ImGui::GetIO();
    const float scale = io.DisplaySize.y / 720.0f;
    const float size = 15.0f * scale;
    const ImVec2 pos(16.0f * scale, io.DisplaySize.y - size - 14.0f * scale);
    ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), size, pos,
                                            IM_COL32(255, 255, 255, static_cast<int>(90.0f * fade)),
                                            "v" PD_VERSION);
  }

  // Players change their settings in the game's own menus, and no F-key
  // opens anything for them. enable_rexglue_menus (SDK, off by default)
  // turns the developer overlays back on, the F9 panel with them.
  static bool DeveloperMenusEnabled() {
    return rex::cvar::GetFlagByName("enable_rexglue_menus") == "true";
  }

  static void DrawPrivateMatchNotice() {
    static std::string text;
    static std::chrono::steady_clock::time_point shown_at;
    std::string fresh;
    if (rex::kernel::xam::netplay::TakeNotice(&fresh)) {
      text = fresh;
      shown_at = std::chrono::steady_clock::now();
    }
    if (text.empty()) return;
    constexpr float kShowSeconds = 4.0f;
    constexpr float kFadeSeconds = 0.6f;
    const float age =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - shown_at).count();
    if (age > kShowSeconds + kFadeSeconds) {
      text.clear();
      return;
    }
    const float alpha = age <= kShowSeconds ? 1.0f : 1.0f - (age - kShowSeconds) / kFadeSeconds;
    ImGuiIO& io = ImGui::GetIO();
    const float scale = io.DisplaySize.y / 720.0f;
    ImFont* font = ImGui::GetFont();
    const float size = 22.0f * scale;
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    const ImVec2 pad(18.0f * scale, 10.0f * scale);
    const ImVec2 min((io.DisplaySize.x - extent.x) * 0.5f - pad.x, 40.0f * scale);
    const ImVec2 max(min.x + extent.x + pad.x * 2.0f, min.y + extent.y + pad.y * 2.0f);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(min, max, IM_COL32(8, 16, 48, int(225 * alpha)), 6.0f * scale);
    draw->AddRect(min, max, IM_COL32(63, 208, 232, int(255 * alpha)), 6.0f * scale, 0,
                  1.5f * scale);
    draw->AddText(font, size, ImVec2(min.x + pad.x, min.y + pad.y),
                  IM_COL32(230, 245, 255, int(255 * alpha)), text.c_str());
  }

  // ---- Combat Simulator player pages --------------------------------------
  // Player Setup's three pages were split by where the game put them rather
  // than by what they do: Control carried the aiming settings *and* seven
  // things that are only drawn on the screen, while Player Options held four
  // more of the same kind. So the drawn ones all move to Player Options,
  // renamed HUD Options, and Control keeps the controls. The rows themselves
  // are the game's own - each one is copied whole, so it keeps its handler,
  // its setting bit and its text - only the lists they sit in are rebuilt.
  //
  // A Player Name row is added at the top of Player Setup, because that is
  // where someone looks to change who they are, and the Combat Simulator's
  // player list right beside it shows that name. It is the same row as
  // Settings > Online's, so there is one name everywhere.
  static constexpr uint32_t kSetupDef = 0x825DEAA4;      // Player Setup
  static constexpr uint32_t kSetupItems = 0x825DEA08;    // Character, Control, ...
  static constexpr int kSetupRowCount = 4;
  static constexpr uint32_t kControlDef = 0x825DDAD0;    // Control
  static constexpr uint32_t kControlItems = 0x825DD968;
  static constexpr uint32_t kHudDef = 0x825DD94C;        // Player Options -> HUD Options
  static constexpr uint32_t kHudItems = 0x825DD8E8;
  static constexpr int kHudOldRowCount = 4;              // radar and the highlights
  static constexpr uint32_t kSetupHudRowTid = 0x5022;    // "Player Options" -> "HUD Options"
  // Control's rows, in the order the game lists them.
  static constexpr int kCtrlKeep = 8;    // Control Style .. Y axis sensitivity
  static constexpr int kCtrlSeparator = 8;
  static constexpr int kCtrlHudFirst = 9;   // Sight on Screen
  static constexpr int kCtrlHudCount = 7;   // .. Show Crouch
  static constexpr int kCtrlBack = 16;

  static void CopyMenuItem(rex::memory::Memory* mem, uint32_t from, uint32_t to) {
    for (int w = 0; w < 5; ++w) WriteGuestU32(mem, to + uint32_t(w) * 4,
                                              ReadGuestU32(mem, from + uint32_t(w) * 4));
  }
  static void WriteMenuEnd(rex::memory::Memory* mem, uint32_t at) {
    WriteGuestU32(mem, at, 0x1D000000);
    for (int w = 1; w < 5; ++w) WriteGuestU32(mem, at + uint32_t(w) * 4, 0);
  }

  // ---- the Xbox "gamer card" prompt ----------------------------------------
  // Pages that list players - the Combat Simulator's Player Ranking tab and
  // the leaderboards - offered a Y button, "View Gamer Profile" on the console
  // and relabelled "View Player Info" here. There is no gamer card to open on
  // a PC, so the prompt is taken off the screen.
  //
  // The prompts come from a table of five word entries in the game's own data,
  // the last two words being the button's icon and its text; entries with no
  // icon already exist (icon 0), so clearing both words leaves the footer with
  // nothing to draw. Only the prompt goes: pressing Y still runs whatever the
  // game ran before.
  static constexpr uint32_t kGamerCardTid = 0x8C66;        // "View Gamer Profile"
  static constexpr uint32_t kPromptEntries[2] = {0x82603C04, 0x82603CCC};  // icon word
  static void PatchGamerCardPrompt() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    int cleared = 0;
    for (uint32_t icon_at : kPromptEntries) {
      if (ReadGuestU32(mem, icon_at + 4) != kGamerCardTid) continue;  // not that prompt
      WriteGuestU32(mem, icon_at, 0);      // no icon
      WriteGuestU32(mem, icon_at + 4, 0);  // no text
      ++cleared;
    }
    // The lobby's X prompt, "Invite Friends": the Invite Friends window still opens
    // on X (pad) or R (keyboard), only the footer no longer advertises it. Its text
    // ids are 0x8C55 and 0x8C65 (two strings, three entries in the table).
    int invites = 0;
    for (int i = 0; i < kPromptCount; ++i) {
      const uint32_t at = kPromptIcons + uint32_t(i) * 20;
      const uint32_t text = ReadGuestU32(mem, at + 4);
      if (text != 0x8C55 && text != 0x8C65) continue;
      WriteGuestU32(mem, at, 0);
      WriteGuestU32(mem, at + 4, 0);
      ++invites;
    }
    REXLOG_INFO("[menu_patch] invite friends prompt: {} entries cleared", invites);
    REXLOG_INFO("[menu_patch] player info prompt: {} of {} entries cleared", cleared,
                sizeof(kPromptEntries) / sizeof(kPromptEntries[0]));
  }

  static void PatchPlayerSetupMenus() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    if (setup_items_) return;
    auto* mem = app->runtime()->memory();
    auto* fd = app->runtime()->function_dispatcher();
    if (!fd) return;

    // Only rebuild pages that still look like the ones this was written for.
    const uint32_t ctrl_first = ReadGuestU32(mem, kControlItems) >> 24;
    const uint32_t hud_first = ReadGuestU32(mem, kHudItems) >> 24;
    const uint32_t setup_first = ReadGuestU32(mem, kSetupItems) >> 24;
    if (ctrl_first != 0x0C || hud_first != 0x09 || setup_first != 0x04 ||
        ReadGuestU32(mem, kControlItems + uint32_t(kCtrlBack) * 20 + 8) != 0x8C3F) {
      REXLOG_INFO("[menu_patch] player setup pages are not as expected; left alone");
      return;
    }

    const uint32_t heap = mem->SystemHeapAlloc(1024);
    if (!heap) return;
    const uint32_t control_items = heap;                 // 10 rows + end
    const uint32_t hud_items = control_items + 11 * 20;  // 12 rows + end
    const uint32_t setup_items = hud_items + 13 * 20;    // 5 rows + end

    // Control: the controls, then Back.
    uint32_t out = control_items;
    for (int i = 0; i < kCtrlKeep; ++i, out += 20) {
      CopyMenuItem(mem, kControlItems + uint32_t(i) * 20, out);
    }
    CopyMenuItem(mem, kControlItems + uint32_t(kCtrlSeparator) * 20, out);
    out += 20;
    CopyMenuItem(mem, kControlItems + uint32_t(kCtrlBack) * 20, out);
    WriteMenuEnd(mem, out + 20);

    // HUD Options: what is drawn around your gun, then what is drawn about
    // everything else (radar and the highlights, which were here already).
    out = hud_items;
    for (int i = 0; i < kCtrlHudCount; ++i, out += 20) {
      CopyMenuItem(mem, kControlItems + uint32_t(kCtrlHudFirst + i) * 20, out);
    }
    CopyMenuItem(mem, kControlItems + uint32_t(kCtrlSeparator) * 20, out);
    out += 20;
    for (int i = 0; i < kHudOldRowCount; ++i, out += 20) {
      CopyMenuItem(mem, kHudItems + uint32_t(i) * 20, out);
    }
    WriteMenuEnd(mem, out);

    // Player Setup: Player Name, then the pages as they were.
    const uint32_t name_label = fd->AllocateThunk(&PdOnlineRowLabel<0>, 0);
    const uint32_t name_handler = fd->AllocateThunk(&PdOnlineRowHandler<0>, 0);
    out = setup_items;
    if (name_label && name_handler) {
      WriteGuestU32(mem, out + 0, 0x0C000000);  // dropdown: the name, "Change name..."
      WriteGuestU32(mem, out + 4, 0);
      WriteGuestU32(mem, out + 8, name_label);
      WriteGuestU32(mem, out + 12, 0);
      WriteGuestU32(mem, out + 16, name_handler);
      out += 20;
    }
    for (int i = 0; i < kSetupRowCount; ++i, out += 20) {
      CopyMenuItem(mem, kSetupItems + uint32_t(i) * 20, out);
    }
    WriteMenuEnd(mem, out);

    // The page keeps its own name: the title now uses the same text as the row
    // that opens it, which TickLobbyLabels renames to "HUD Options".
    WriteGuestU32(mem, kHudDef + 4, kSetupHudRowTid);
    WriteGuestU32(mem, kControlDef + 8, control_items);
    WriteGuestU32(mem, kHudDef + 8, hud_items);
    WriteGuestU32(mem, kSetupDef + 8, setup_items);
    setup_items_ = setup_items;
    REXLOG_INFO("[menu_patch] player setup rebuilt: control 0x{:08X}, HUD 0x{:08X}, "
                "setup 0x{:08X}{}",
                control_items, hud_items, setup_items,
                (name_label && name_handler) ? "" : " - no Player Name row (thunks failed)");
  }

  static void PatchVideoOptionsMenu() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[menu_patch] runtime/memory not ready");
      return;
    }
    REXLOG_INFO("[menu_patch] starting");
    if (REXCVAR_GET(pd_dev_hotkeys)) ProbeMenuMemory();
    auto* mem = app->runtime()->memory();

    static bool already_installed = false;
    if (already_installed) {
      REXLOG_INFO("[menu_patch] already installed, ignoring");
      return;
    }

    // --- 1. all strings, written into writable scratch ---------------------
    // The handlers hand these back as char* for op 3, and the label thunks
    // return the label ones, so no string-table surgery is needed at all.
    // Game strings are "text\n\0", so match that exactly.
    {
      struct StrDef { uint32_t addr; const char* text; };
      const StrDef kStrings[] = {
          {kResOptionStrings + 0 * 16, "1280 x 720\n"},
          {kResOptionStrings + 1 * 16, "1920 x 1080\n"},
          {kResOptionStrings + 2 * 16, "2560 x 1440\n"},
          {kResOptionStrings + 3 * 16, "3840 x 2160\n"},
          {kStrWindowed, "Windowed\n"},
          {kStrFullscreen, "Fullscreen\n"},
          {kStrOff, "Off\n"},
          {kStrOn, "On\n"},
          {kLblResolution, "Resolution\n"},
          {kLblGameDisplay, "Game Display\n"},
          {kLblVsync, "Vsync\n"},
          {kLblShowFps, "Show FPS\n"},
          {kLblRenderScale, "Render Scale\n"},
          {kRenderScaleStrings + 0 * 16, "1x\n"},
          {kRenderScaleStrings + 1 * 16, "2x\n"},
          {kRenderScaleStrings + 2 * 16, "3x\n"},
          {kRenderScaleRestartStrings + 0 * 32, "1x (requires restart)\n"},
          {kRenderScaleRestartStrings + 1 * 32, "2x (requires restart)\n"},
          {kRenderScaleRestartStrings + 2 * 32, "3x (requires restart)\n"},
          {kLblUpscaling, "Upscaling\n"},
          {kUpscalingStrings + 0 * 16, "Off\n"},
          {kUpscalingStrings + 1 * 16, "Sharpen\n"},
          {kUpscalingStrings + 2 * 16, "FSR\n"},
          {kLblGamepadControls, "Gamepad\n"},  // box is fixed-width; longer clips
          {kLblPcControls, "PC Controls\n"},
          {kLblMouseSens, "Mouse Sensitivity\n"},
          {kLblInvertLook, "Invert Look\n"},
          {kSensOptionStrings + 0 * 16, "0.5\n"},
          {kSensOptionStrings + 1 * 16, "1.0\n"},
          {kSensOptionStrings + 2 * 16, "1.5\n"},
          {kSensOptionStrings + 3 * 16, "2.0\n"},
          {kSensOptionStrings + 4 * 16, "3.0\n"},
      };
      for (const StrDef& d : kStrings) {
        WriteGuestBytes(mem, d.addr, d.text, strlen(d.text) + 1);
      }
      if (!bind_label_strings_.load()) {
        bind_label_strings_ = mem->SystemHeapAlloc(kBindRowSlots * kBindRowStride);
        bind_value_strings_ = mem->SystemHeapAlloc(kBindRowSlots * kBindRowStride);
      }
      for (int i = 0; i < kBindRowCount; ++i) {
        std::string label = BindRows()[i].label;
        label += "\n";
        WriteGuestBytes(mem, BindLabelStrings() + uint32_t(i) * kBindRowStride, label.c_str(),
                        label.size() + 1);
      }
      for (int i = 0; i < kBindOptionCount - 1; ++i) {
        WriteGuestBytes(mem, kBindOptionStrings + uint32_t(i) * kBindRowStride,
                        kBindOptionText[i], strlen(kBindOptionText[i]) + 1);
      }
    }

    // --- 2. register the native label + value handlers at guest addresses ---
    auto* fd = app->runtime()->function_dispatcher();
    if (!fd) {
      REXLOG_INFO("[menu_patch] no function dispatcher");
      return;
    }

    // Your Address in the lobby (see "Your Address" above).
    if (!address_strings_) {
      address_strings_ = mem->SystemHeapAlloc(32 + kAddressSlots * kAddressStride);
    }
    if (address_strings_ && ReadGuestU32(mem, kHostAddressItem) == kHostAddressItemWord) {
      const uint32_t label = fd->AllocateThunk(&PdHostAddressLabel, 0);
      const uint32_t handler = fd->AllocateThunk(&PdHostAddressHandler, 0);
      if (label && handler) {
        WriteGuestU32(mem, kHostAddressItem + 8, label);
        WriteGuestU32(mem, kHostAddressItem + 16, handler);
        REXLOG_INFO("[menu_patch] lobby: Your Address row installed at 0x{:08X}", kHostAddressItem);
      }
    } else {
      REXLOG_INFO("[menu_patch] lobby row 0x{:08X} is not the expected Quick Match row; "
                  "Your Address left out", kHostAddressItem);
    }

    // Match Type: Local / Host / Join (see PdMatchTypeHandler).
    if (ReadGuestU32(mem, kMatchTypeItem + 16) == kMatchTypeHandler &&
        (ReadGuestU32(mem, kMatchTypeItem + 8) & 0xFFFF) == kMatchTypeLabelTid) {
      const uint32_t match_type = fd->AllocateThunk(&PdMatchTypeHandler, 0);
      if (match_type) {
        static const char kJoined[] = "Joined Match";
        WriteGuestBytes(mem, kStrJoinedMatch, kJoined, sizeof(kJoined));
        WriteGuestU32(mem, kMatchTypeItem + 16, match_type);
        REXLOG_INFO("[menu_patch] Match Type: Host / Join Online Match (handler 0x{:08X})",
                    match_type);
      }
    } else {
      REXLOG_INFO("[menu_patch] Match Type row not found at 0x{:08X}; Quick Match left in",
                  kMatchTypeItem);
    }

    // Online page (see "Online page" above): four rows in a guest heap block.
    // The Settings lists get a row pointing at it further down.
    if (!online_dialog_) {
      const uint32_t heap = mem->SystemHeapAlloc(2048);
      if (heap) {
        online_strings_ = heap;
        const uint32_t items = heap + uint32_t(kOnlineRows) * kOnlineStride;
        match_type_scratch_ = heap + 2048 - 8;
        const uint32_t dlg = items + uint32_t(kOnlineRows + 1) * 20;
        PPCFunc* labels[kOnlineRows] = {};
        PPCFunc* handlers[kOnlineRows] = {};
        FillOnlineThunks(std::make_index_sequence<kOnlineRows>{}, labels, handlers);
        // Online Server (row 2) is not shown: there is no server yet, so it
        // only ever said "None - your own network". Its code stays for when
        // one is deployed; rows keep their numbers, so nothing else changes.
        static constexpr int kShownRows[] = {0, 1, 3};
        constexpr int kShown = int(sizeof(kShownRows) / sizeof(kShownRows[0]));
        bool ok = true;
        for (int i = 0; i < kShown && ok; ++i) {
          const int row = kShownRows[i];
          const uint32_t lbl = fd->AllocateThunk(labels[row], 0);
          const uint32_t hnd = fd->AllocateThunk(handlers[row], 0);
          ok = lbl && hnd;
          const uint32_t ia = items + uint32_t(i) * 20;
          WriteGuestU32(mem, ia + 0, 0x0C000000);
          WriteGuestU32(mem, ia + 4, 0);
          WriteGuestU32(mem, ia + 8, lbl);
          WriteGuestU32(mem, ia + 12, 0);
          WriteGuestU32(mem, ia + 16, hnd);
        }
        const uint32_t term = items + uint32_t(kShown) * 20;
        WriteGuestU32(mem, term + 0, 0x1D000000);
        for (int w = 1; w < 5; ++w) WriteGuestU32(mem, term + w * 4, 0);
        WriteGuestU32(mem, dlg + 0, 0x01000000);
        WriteGuestU32(mem, dlg + 4, kOnlineTid);
        WriteGuestU32(mem, dlg + 8, items);
        for (int w = 3; w < 6; ++w) WriteGuestU32(mem, dlg + w * 4, 0);
        online_dialog_ = ok ? dlg : 0;
        REXLOG_INFO("[menu_patch] online dialog at 0x{:08X}{}", dlg,
                    ok ? "" : " - FAILED to allocate its thunks");
      }
    }

    // Multiplayer screen (see "Multiplayer" above): three rows in a guest heap
    // block. Match Type's Join Match opens it.
    if (!multiplayer_dialog_) {
      const uint32_t heap = mem->SystemHeapAlloc(1024);
      if (heap) {
        multiplayer_strings_ = heap;
        const uint32_t items = heap + uint32_t(kMultiplayerRows) * kMultiplayerStride;
        const uint32_t dlg = items + uint32_t(kMultiplayerRows + 1) * 20;
        PPCFunc* const labels[kMultiplayerRows] = {
            &PdMultiplayerRowLabel<0>, &PdMultiplayerRowLabel<1>};
        PPCFunc* const handlers[kMultiplayerRows] = {
            &PdMultiplayerRowHandler<0>, &PdMultiplayerRowHandler<1>};
        bool ok = true;
        for (int i = 0; i < kMultiplayerRows && ok; ++i) {
          const uint32_t lbl = fd->AllocateThunk(labels[i], 0);
          const uint32_t hnd = fd->AllocateThunk(handlers[i], 0);
          ok = lbl && hnd;
          const uint32_t ia = items + uint32_t(i) * 20;
          WriteGuestU32(mem, ia + 0, 0x04000000);  // selectable
          WriteGuestU32(mem, ia + 4, 0);
          WriteGuestU32(mem, ia + 8, lbl);
          WriteGuestU32(mem, ia + 12, 0);
          WriteGuestU32(mem, ia + 16, hnd);
        }
        const uint32_t term = items + uint32_t(kMultiplayerRows) * 20;
        WriteGuestU32(mem, term + 0, 0x1D000000);
        for (int w = 1; w < 5; ++w) WriteGuestU32(mem, term + w * 4, 0);
        WriteGuestU32(mem, dlg + 0, 0x01000000);
        WriteGuestU32(mem, dlg + 4, 0x8CD1);  // "Multiplayer"
        WriteGuestU32(mem, dlg + 8, items);
        for (int w = 3; w < 6; ++w) WriteGuestU32(mem, dlg + w * 4, 0);
        multiplayer_dialog_ = ok ? dlg : 0;
        REXLOG_INFO("[menu_patch] multiplayer dialog at 0x{:08X}{}", dlg,
                    ok ? "" : " - FAILED to allocate its thunks");
      }
    }

    // Achievements list (see "Achievements list" above): one row per
    // achievement in a guest heap block, then the menu items that used to
    // call the Xbox dashboard are pointed at it.
    if (!achievements_dialog_) {
      const uint32_t heap = mem->SystemHeapAlloc(8192);
      if (heap) {
        achievements_strings_ = heap;
        const uint32_t items = heap + uint32_t(kAchievementRows) * kAchievementStride;
        const uint32_t dlg = items + uint32_t(kAchievementRows + 1) * 20;
        PPCFunc* labels[kAchievementRows] = {};
        PPCFunc* handlers[kAchievementRows] = {};
        FillAchievementThunks(std::make_index_sequence<kAchievementRows>{}, labels, handlers);
        bool ok = true;
        for (int i = 0; i < kAchievementRows && ok; ++i) {
          const uint32_t lbl = fd->AllocateThunk(labels[i], 0);
          const uint32_t hnd = fd->AllocateThunk(handlers[i], 0);
          ok = lbl && hnd;
          const uint32_t ia = items + uint32_t(i) * 20;
          WriteGuestU32(mem, ia + 0, 0x0C000000);
          WriteGuestU32(mem, ia + 4, 0);
          WriteGuestU32(mem, ia + 8, lbl);
          WriteGuestU32(mem, ia + 12, 0);
          WriteGuestU32(mem, ia + 16, hnd);
        }
        const uint32_t term = items + uint32_t(kAchievementRows) * 20;
        WriteGuestU32(mem, term + 0, 0x1D000000);
        for (int w = 1; w < 5; ++w) WriteGuestU32(mem, term + w * 4, 0);
        WriteGuestU32(mem, dlg + 0, 0x01000000);
        WriteGuestU32(mem, dlg + 4, 0x8C15);  // "Achievements"
        WriteGuestU32(mem, dlg + 8, items);
        for (int w = 3; w < 6; ++w) WriteGuestU32(mem, dlg + w * 4, 0);
        achievements_dialog_ = ok ? dlg : 0;
        REXLOG_INFO("[menu_patch] achievements dialog at 0x{:08X} ({} achievements){}", dlg,
                    AchievementList().size(), ok ? "" : " - FAILED to allocate its thunks");
      }
      if (achievements_dialog_) {
        // The main menu's Achievements row (tid 0x8C15) and the Leaderboards
        // screen's own entry (0x8CDF) are selectable rows whose handler calls
        // XamShowAchievementsUI; give them the opens-a-dialog flag and our
        // dialog instead.
        constexpr uint32_t kSelectable = 0x04;
        int patched = 0;
        ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
          for (uint32_t a = region_addr; a + 20 <= region_addr + region_size; a += 4) {
            const uint32_t tid = ReadGuestU32(mem, a + 8);
            if (tid != 0x8C15 && tid != 0x8CDF) continue;
            if ((ReadGuestU32(mem, a) >> 24) != kSelectable) continue;
            const uint32_t flags = ReadGuestU32(mem, a + 4);
            const uint32_t handler = ReadGuestU32(mem, a + 16);
            if ((flags & 0xFF000000) || handler < 0x82000000 || handler >= 0x826E0000) continue;
            WriteGuestU32(mem, a + 4, flags | 0x4);  // opens a dialog
            WriteGuestU32(mem, a + 16, achievements_dialog_);
            REXLOG_INFO("[menu_patch]   Achievements item 0x{:08X} (tid {:04X}) -> the list "
                        "(was 0x{:08X}, flags {:08X})",
                        a, tid, handler, flags);
            ++patched;
          }
        });
        REXLOG_INFO("[menu_patch] Achievements: {} menu item(s) repointed", patched);
      }
    }

    // Server browser dialog (see "Server browser" above): 8 match rows, a
    // Refresh row, a terminator and the dialogdef, in a guest heap block.
    if (!browser_dialog_) {
      const uint32_t heap = mem->SystemHeapAlloc(4096);
      if (heap) {
        browser_strings_ = heap;                  // 9 rows x 160 bytes
        browser_word_ = heap + 1536;
        const uint32_t items = heap + 1600;       // 10 items x 20 bytes
        const uint32_t dlg = heap + 1824;         // 24 bytes
        PPCFunc* const labels[kBrowserRows + 1] = {
            &PdBrowserRowLabel<0>, &PdBrowserRowLabel<1>, &PdBrowserRowLabel<2>,
            &PdBrowserRowLabel<3>, &PdBrowserRowLabel<4>, &PdBrowserRowLabel<5>,
            &PdBrowserRowLabel<6>, &PdBrowserRowLabel<7>, &PdBrowserRowLabel<8>,
        };
        PPCFunc* const handlers[kBrowserRows + 1] = {
            &PdBrowserRowHandler<0>, &PdBrowserRowHandler<1>, &PdBrowserRowHandler<2>,
            &PdBrowserRowHandler<3>, &PdBrowserRowHandler<4>, &PdBrowserRowHandler<5>,
            &PdBrowserRowHandler<6>, &PdBrowserRowHandler<7>, &PdBrowserRowHandler<8>,
        };
        bool ok = true;
        for (int i = 0; i <= kBrowserRows && ok; ++i) {
          const uint32_t lbl = fd->AllocateThunk(labels[i], 0);
          const uint32_t hnd = fd->AllocateThunk(handlers[i], 0);
          ok = lbl && hnd;
          const uint32_t ia = items + uint32_t(i) * 20;
          WriteGuestU32(mem, ia + 0, 0x04000000);  // selectable: one press
          WriteGuestU32(mem, ia + 4, 0);
          WriteGuestU32(mem, ia + 8, lbl);
          WriteGuestU32(mem, ia + 12, 0);
          WriteGuestU32(mem, ia + 16, hnd);
        }
        const uint32_t term = items + uint32_t(kBrowserRows + 1) * 20;
        WriteGuestU32(mem, term + 0, 0x1D000000);
        for (int w = 1; w < 5; ++w) WriteGuestU32(mem, term + w * 4, 0);
        WriteGuestU32(mem, dlg + 0, 0x01000000);
        WriteGuestU32(mem, dlg + 4, 0x8CC0);  // "Find Online Match" (renamed Quick Match)
        WriteGuestU32(mem, dlg + 8, items);
        for (int w = 3; w < 6; ++w) WriteGuestU32(mem, dlg + w * 4, 0);
        if (ok) browser_dialog_ = dlg;
        REXLOG_INFO("[menu_patch] server browser dialog at 0x{:08X}{}", dlg,
                    ok ? "" : " - FAILED to allocate its thunks");
      }
    }

    struct RowDef {
      const char* name;
      PPCFunc* label_fn;
      PPCFunc* handler_fn;
      uint32_t label_addr;   // filled in below
      uint32_t handler_addr;
    };
    RowDef rows[] = {
        {"Resolution", &PdLabelResolution, &PdResolutionMenuHandler, 0, 0},
        {"Game Display", &PdLabelGameDisplay, &PdGameDisplayMenuHandler, 0, 0},
        {"Vsync", &PdLabelVsync, &PdVsyncMenuHandler, 0, 0},
        {"Show FPS", &PdLabelShowFps, &PdShowFpsMenuHandler, 0, 0},
        {"Render Scale", &PdLabelRenderScale, &PdRenderScaleMenuHandler, 0, 0},
        {"Upscaling", &PdLabelUpscaling, &PdUpscalingMenuHandler, 0, 0},
    };
    for (RowDef& r : rows) {
      r.label_addr = fd->AllocateThunk(r.label_fn, 0);
      r.handler_addr = fd->AllocateThunk(r.handler_fn, 0);
      if (!r.label_addr || !r.handler_addr) {
        REXLOG_INFO("[menu_patch] FAILED to allocate thunks for \"{}\"", r.name);
        return;
      }
      REXLOG_INFO("[menu_patch] row \"{}\": label 0x{:08X} handler 0x{:08X}", r.name,
                  r.label_addr, r.handler_addr);
    }

    // The three Video Options dialogdefs (items pointer lives at +8).
    const uint32_t kDialogs[] = {0x825DA188, 0x825DA1A4, 0x825DA1FC};
    const uint32_t kTypeSpinner = 0x0C;
    const uint32_t kTypeEnd = 0x1D;
    already_installed = true;

    // Scratch: a large zero-filled run in the data section. NOT the
    // thunk-reserve region at 0x825BB934 - ProbeMenuMemory showed that maps
    // read-only (protect=0x1) and writing there faults, which is what killed
    // the first attempt. This region probes writable (protect=0x3).
    static uint32_t scratch = 0x82674620;

    for (uint32_t dlg : kDialogs) {
      uint32_t items = ReadGuestU32(mem, dlg + 8);
      if (items < 0x82000000 || items >= 0x826E0000) {
        REXLOG_INFO("[menu_patch] dlg 0x{:08X}: items ptr 0x{:08X} out of range, skipping", dlg,
                    items);
        continue;
      }

      // Count real items (stop at terminator).
      int count = 0;
      while (count < 32) {
        uint32_t type = ReadGuestU32(mem, items + count * 20) >> 24;
        if (type == kTypeEnd || type == 0) break;
        ++count;
      }

      uint32_t dest = scratch;
      for (int i = 0; i < count; ++i) {
        for (int w = 0; w < 5; ++w) {
          WriteGuestU32(mem, dest + i * 20 + w * 4,
                        ReadGuestU32(mem, items + i * 20 + w * 4));
        }
      }

      // Appended rows.
      int added = 0;
      for (const RowDef& r : rows) {
        uint32_t ni = dest + (count + added) * 20;
        WriteGuestU32(mem, ni + 0, kTypeSpinner << 24);
        WriteGuestU32(mem, ni + 4, 0);
        WriteGuestU32(mem, ni + 8, r.label_addr);
        WriteGuestU32(mem, ni + 12, 0);
        WriteGuestU32(mem, ni + 16, r.handler_addr);
        ++added;
      }

      // Terminator.
      uint32_t te = dest + (count + added) * 20;
      WriteGuestU32(mem, te + 0, kTypeEnd << 24);
      for (int w = 1; w < 5; ++w) WriteGuestU32(mem, te + w * 4, 0);

      WriteGuestU32(mem, dlg + 8, dest);
      REXLOG_INFO("[menu_patch] dlg 0x{:08X}: {} item(s) at 0x{:08X} -> {} item(s) at 0x{:08X}",
                  dlg, count, items, count + added, dest);
      scratch = te + 20 + 16;  // bump, with padding
    }

    // --- 4. Settings menu: relabel "Control" and add a "PC Controls" page ----
    // Settings items are type 0x04 (submenu):
    //   type | flags | param2 = label | param3 = index | target dialogdef
    // A dialogdef is 6 words:
    //   type(0x01) | title | items | handler | flags | nextsibling
    // Several Settings dialogdefs share one item array, so relocate each unique
    // array once and repoint every dialogdef that referenced it.
    {
      const uint32_t kControlTid = 0x56B7;
      // A type-0x04 (submenu) item resolves param2 as a TEXT ID - unlike the
      // type-0x0C spinners, it will not call a function pointer. Feeding it a
      // thunk address makes the game resolve some unrelated string ("Game ...")
      // and drop rows whose lookup fails, which is why the label thunks did not
      // work here. So repurpose two N64-era strings that the XBLA build never
      // shows; both are longer than the replacement, so they overwrite in place.
      const uint32_t kGamepadTid = 0x566B;   // was "Controller Pak Menu"
      const uint32_t kPcCtrlTid = 0x5670;    // was "Controller Pak 1"
      const uint32_t kGraphicsTid = 0x5671;  // was "Controller Pak 2"
      const uint32_t kLayoutTid = 0x5672;    // was "Controller Pak 3"
      const uint32_t kInterfaceTid = 0x566D;  // was "Choose Controller Pak to Edit:"
      REXLOG_INFO("[menu_patch] label strings replaced: {} {} {} {} {}",
                  ReplaceTextById(mem, kGamepadTid, "Controller Pak Menu\n", "Gamepad Controls\n"),
                  ReplaceTextById(mem, kPcCtrlTid, "Controller Pak 1\n", "Keyboard & Mouse\n"),
                  ReplaceTextById(mem, kGraphicsTid, "Controller Pak 2\n", "Graphics\n"),
                  ReplaceTextById(mem, kLayoutTid, "Controller Pak 3\n", "Button Layout\n"),
                  ReplaceTextById(mem, kInterfaceTid, "Choose Controller Pak to Edit:\n",
                                  "Display Interface\n"));
      const uint32_t kTypeSubmenu = 0x04;

      uint32_t lbl_gamepad = fd->AllocateThunk(&PdLabelGamepadControls, 0);
      uint32_t lbl_pc = fd->AllocateThunk(&PdLabelPcControls, 0);
      uint32_t lbl_sens = fd->AllocateThunk(&PdLabelMouseSens, 0);
      uint32_t lbl_invert = fd->AllocateThunk(&PdLabelInvertLook, 0);
      uint32_t h_sens = fd->AllocateThunk(&PdMouseSensHandler, 0);
      uint32_t h_invert = fd->AllocateThunk(&PdInvertLookMenuHandler, 0);
      (void)lbl_gamepad;
      (void)lbl_pc;
      if (!lbl_sens || !lbl_invert || !h_sens || !h_invert) {
        REXLOG_INFO("[menu_patch] FAILED to allocate PC Controls thunks");
        return;
      }

      // Build the PC Controls item array: two spinner rows + terminator.
      uint32_t pc_items = scratch;
      struct SpinRow { uint32_t label; uint32_t handler; };
      std::vector<SpinRow> pc_rows = {{lbl_sens, h_sens}, {lbl_invert, h_invert}};

      // One row per binding, so the keyboard layout is visible in the game's own
      // menu rather than only in the config file.
      PPCFunc* const kBindLabelFns[kBindRowCount] = {
          &PdBindRowLabel<0>,  &PdBindRowLabel<1>, &PdBindRowLabel<2>,  &PdBindRowLabel<3>,
          &PdBindRowLabel<4>,  &PdBindRowLabel<5>, &PdBindRowLabel<6>,  &PdBindRowLabel<7>,
          &PdBindRowLabel<8>,  &PdBindRowLabel<9>, &PdBindRowLabel<10>, &PdBindRowLabel<11>,
          &PdBindRowLabel<12>, &PdBindRowLabel<13>, &PdBindRowLabel<14>, &PdBindRowLabel<15>,
      };
      PPCFunc* const kBindHandlerFns[kBindRowCount] = {
          &PdBindRowHandler<0>,  &PdBindRowHandler<1>,  &PdBindRowHandler<2>,
          &PdBindRowHandler<3>,  &PdBindRowHandler<4>,  &PdBindRowHandler<5>,
          &PdBindRowHandler<6>,  &PdBindRowHandler<7>,  &PdBindRowHandler<8>,
          &PdBindRowHandler<9>,  &PdBindRowHandler<10>, &PdBindRowHandler<11>,
          &PdBindRowHandler<12>, &PdBindRowHandler<13>, &PdBindRowHandler<14>,
          &PdBindRowHandler<15>,
      };
      for (int i = 0; i < kBindRowCount; ++i) {
        uint32_t lbl = fd->AllocateThunk(kBindLabelFns[i], 0);
        uint32_t hnd = fd->AllocateThunk(kBindHandlerFns[i], 0);
        if (!lbl || !hnd) {
          REXLOG_INFO("[menu_patch] FAILED to allocate thunks for binding row {}", i);
          return;
        }
        pc_rows.push_back({lbl, hnd});
      }
      REXLOG_INFO("[menu_patch] PC Controls: {} row(s)", (int)pc_rows.size());

      int n = 0;
      for (const SpinRow& r : pc_rows) {
        uint32_t ia = pc_items + n * 20;
        WriteGuestU32(mem, ia + 0, kTypeSpinner << 24);
        WriteGuestU32(mem, ia + 4, 0);
        WriteGuestU32(mem, ia + 8, r.label);
        WriteGuestU32(mem, ia + 12, 0);
        WriteGuestU32(mem, ia + 16, r.handler);
        ++n;
      }
      uint32_t pc_term = pc_items + n * 20;
      WriteGuestU32(mem, pc_term + 0, kTypeEnd << 24);
      for (int w = 1; w < 5; ++w) WriteGuestU32(mem, pc_term + w * 4, 0);
      scratch = pc_term + 20 + 16;

      // Build the PC Controls dialogdef.
      uint32_t pc_dlg = scratch;
      WriteGuestU32(mem, pc_dlg + 0, 0x01000000);
      WriteGuestU32(mem, pc_dlg + 4, 0x5670);  // title: the repurposed tid
      WriteGuestU32(mem, pc_dlg + 8, pc_items);
      WriteGuestU32(mem, pc_dlg + 12, 0);
      WriteGuestU32(mem, pc_dlg + 16, 0);
      WriteGuestU32(mem, pc_dlg + 20, 0);
      pc_controls_dialog_ = pc_dlg;
      scratch = pc_dlg + 24 + 16;
      REXLOG_INFO("[menu_patch] PC Controls dialog at 0x{:08X} (items 0x{:08X})", pc_dlg,
                  pc_items);

      // Find every Settings dialogdef: one whose item array holds a type-0x04
      // item labelled with the Control tid.
      std::map<uint32_t, uint32_t> relocated;  // old items -> new items
      int dialogs_patched = 0;
      ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
        for (uint32_t a = region_addr; a + 24 <= region_addr + region_size; a += 4) {
          if (ReadGuestU32(mem, a) != 0x01000000) continue;
          uint32_t items = ReadGuestU32(mem, a + 8);
          if (items < 0x82000000 || items >= 0x826E0000 - 20) continue;

          int count = 0;
          bool has_control = false;
          while (count < 24) {
            uint32_t w0 = ReadGuestU32(mem, items + count * 20);
            uint32_t type = w0 >> 24;
            if (type == kTypeEnd) break;
            if (type == 0) { count = -1; break; }
            if (type == kTypeSubmenu &&
                ReadGuestU32(mem, items + count * 20 + 8) == kControlTid) {
              has_control = true;
            }
            ++count;
          }
          if (count <= 0 || !has_control) continue;

          auto it = relocated.find(items);
          if (it == relocated.end()) {
            // Locate the Control row so PC Controls can go directly beneath it.
            // Appending at the end does not work: these arrays carry trailing
            // entries (a type-0x0B and a function-pointer type-0x04) after the
            // visible rows, and anything past them never renders.
            int ctrl_idx = -1;
            for (int i = 0; i < count; ++i) {
              uint32_t w0 = ReadGuestU32(mem, items + i * 20);
              if ((w0 >> 24) == kTypeSubmenu &&
                  ReadGuestU32(mem, items + i * 20 + 8) == kControlTid) {
                ctrl_idx = i;
                break;
              }
            }
            if (ctrl_idx < 0) continue;

            const uint32_t ctrl_flags = ReadGuestU32(mem, items + ctrl_idx * 20 + 4);
            const uint32_t ctrl_param3 = ReadGuestU32(mem, items + ctrl_idx * 20 + 12);

            // Does this array number its rows sequentially? If so the inserted
            // row has to be numbered too, and everything after it shifted.
            bool sequential = true;
            uint32_t base = ReadGuestU32(mem, items + 12);
            for (int i = 0; i < count; ++i) {
              if (ReadGuestU32(mem, items + i * 20 + 12) != base + uint32_t(i)) {
                sequential = false;
                break;
              }
            }

            // Only one Settings list (0x825DABB0) carries Cheats; the ones the
            // main menu and pause menu open leave it out, so the Cheats menu
            // could not be reached. Lists without it get a Cheats row beneath
            // Display, opening the game's own Cheats dialog.
            constexpr uint32_t kDisplayTid = 0x56B8;
            constexpr uint32_t kCheatsTid = 0x56B9;
            constexpr uint32_t kCheatsDialog = 0x825D69B4;
            // ... and an Online row under it, for the player name and whether
            // online play is on (see "Online page").
            const bool add_online = online_dialog_ != 0;

            // Settings reads: Audio, Graphics, Display Interface, Keyboard &
            // Mouse, Gamepad Controls, Online, Cheats. Rows are recognised by
            // the screen they open (some lists label them through functions,
            // not text ids). Anything else keeps its place after them, and a
            // list's trailing entries (a type-0x0B and a function row, which
            // must stay last) are copied behind everything.
            constexpr uint32_t kAudioDialogs[2] = {0x825DA09C, 0x825DA11C};
            constexpr uint32_t kVideoDialogs[2] = {0x825DA188, 0x825DA1A4};
            std::vector<std::array<uint32_t, 5>> rows(count);
            for (int i = 0; i < count; ++i) {
              for (int w = 0; w < 5; ++w) rows[i][w] = ReadGuestU32(mem, items + i * 20 + w * 4);
            }
            int visible = 0;
            while (visible < count && (rows[visible][0] >> 24) == kTypeSubmenu) ++visible;
            enum { kAudio, kVideo, kDisplay, kPc, kControl, kOnline, kCheats, kSlots };
            std::array<int, kSlots> slot;
            slot.fill(-1);
            std::vector<int> others;
            for (int i = 0; i < visible; ++i) {
              const uint32_t label = rows[i][2];
              const uint32_t target = rows[i][4];
              if (target == kAudioDialogs[0] || target == kAudioDialogs[1]) {
                slot[kAudio] = i;
              } else if (target == kVideoDialogs[0] || target == kVideoDialogs[1]) {
                slot[kVideo] = i;
              } else if (label == kDisplayTid) {
                slot[kDisplay] = i;
              } else if (i == ctrl_idx) {
                slot[kControl] = i;
              } else if (target == kCheatsDialog) {
                slot[kCheats] = i;
              } else {
                others.push_back(i);
              }
            }
            std::vector<std::array<uint32_t, 5>> built;
            const uint32_t ref_flags = rows[ctrl_idx][1];
            for (int k = 0; k < kSlots; ++k) {
              if (k == kPc) {
                built.push_back({kTypeSubmenu << 24, ctrl_flags, kPcCtrlTid, ctrl_param3, pc_dlg});
                continue;
              }
              if (k == kOnline) {
                if (add_online) {
                  built.push_back({kTypeSubmenu << 24, ref_flags, kOnlineTid, ctrl_param3,
                                   online_dialog_});
                }
                continue;
              }
              if (k == kCheats && slot[kCheats] < 0) {
                built.push_back({kTypeSubmenu << 24, ref_flags, kCheatsTid, ctrl_param3, kCheatsDialog});
                continue;
              }
              if (slot[k] < 0) continue;
              auto row = rows[slot[k]];
              if (k == kVideo) row[2] = kGraphicsTid;
              if (k == kDisplay) row[2] = kInterfaceTid;
              if (k == kControl) row[2] = kGamepadTid;
              // Some lists name Cheats through a function that returns nothing
              // until a save has unlocked a cheat, hiding the row; the text id
              // always shows it.
              if (k == kCheats) row[2] = kCheatsTid;
              built.push_back(row);
            }
            for (int i : others) built.push_back(rows[i]);
            for (int i = visible; i < count; ++i) built.push_back(rows[i]);

            const int out = int(built.size());
            uint32_t dest = mem->SystemHeapAlloc(uint32_t(out + 1) * 20);
            if (!dest) continue;
            for (int i = 0; i < out; ++i) {
              for (int w = 0; w < 5; ++w) WriteGuestU32(mem, dest + i * 20 + w * 4, built[i][w]);
            }
            if (sequential) {
              for (int i = 0; i < out; ++i) {
                WriteGuestU32(mem, dest + i * 20 + 12, base + uint32_t(i));
              }
            }
            uint32_t te2 = dest + out * 20;
            WriteGuestU32(mem, te2 + 0, kTypeEnd << 24);
            for (int w = 1; w < 5; ++w) WriteGuestU32(mem, te2 + w * 4, 0);
            relocated[items] = dest;
            it = relocated.find(items);

            REXLOG_INFO("[menu_patch]   array 0x{:08X} -> 0x{:08X} ({} -> {} rows, ctrl@{}, seq={})",
                        items, dest, count, out, ctrl_idx, sequential);
            for (int i = 0; i <= out; ++i) {
              REXLOG_INFO("[menu_patch]     [{}] {:08X} {:08X} {:08X} {:08X} {:08X}", i,
                          ReadGuestU32(mem, dest + i * 20 + 0),
                          ReadGuestU32(mem, dest + i * 20 + 4),
                          ReadGuestU32(mem, dest + i * 20 + 8),
                          ReadGuestU32(mem, dest + i * 20 + 12),
                          ReadGuestU32(mem, dest + i * 20 + 16));
            }
          }
          WriteGuestU32(mem, a + 8, it->second);

          ++dialogs_patched;
        }
      });
      if (scratch >= kResOptionStrings) {
        REXLOG_INFO("[menu_patch] WARNING scratch 0x{:08X} has reached the string block "
                    "at 0x{:08X} - labels will be corrupted", scratch, kResOptionStrings);
      }
      REXLOG_INFO("[menu_patch] Settings: {} dialog(s) repointed, {} unique item array(s)",
                  dialogs_patched, (int)relocated.size());
    }
    RemoveDeadConsoleRows();
    ArrangeControlsMenus();
    REXLOG_INFO("[menu_patch] done - now open Settings > Video");
  }

  // Help & Options drops Controls (the button layout picture) and reads How
  // To Play, Settings, Awards, Credits. The picture moves to Settings >
  // Gamepad Controls: its main-menu version already opens it from Control
  // Style, and every other version gets a Button Layout row above its last
  // row.
  static constexpr uint32_t kButtonLayoutDialog = 0x825D9FC4;
  static constexpr uint32_t kAwardsDialog = 0x825F91E0;
  static constexpr uint32_t kCreditsHandler = 0x82275C70;
  static constexpr uint32_t kGamepadOptionsTitle = 0x56C0;

  static void ArrangeControlsMenus() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    constexpr uint32_t kTypeEnd = 0x1D;
    constexpr uint32_t kLayoutTid = 0x5672;  // "Button Layout" (see the Settings patch)
    int help = 0, layout = 0;
    std::map<uint32_t, uint32_t> relocated;
    ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
      for (uint32_t a = region_addr; a + 24 <= region_addr + region_size; a += 4) {
        if (ReadGuestU32(mem, a) != 0x01000000) continue;
        const uint32_t items = ReadGuestU32(mem, a + 8);
        if (items < 0x82000000 || items >= 0x826E0000 - 20) continue;
        std::vector<std::array<uint32_t, 5>> rows;
        bool bad = false;
        while (rows.size() < 24) {
          std::array<uint32_t, 5> r;
          for (int w = 0; w < 5; ++w) r[w] = ReadGuestU32(mem, items + uint32_t(rows.size()) * 20 + w * 4);
          if ((r[0] >> 24) == kTypeEnd) break;
          if ((r[0] >> 24) == 0) { bad = true; break; }
          rows.push_back(r);
        }
        if (bad || rows.empty()) continue;
        auto find = [&](auto pred) {
          for (size_t i = 0; i < rows.size(); ++i) if (pred(rows[i])) return int(i);
          return -1;
        };
        const int controls = find([](auto& r) { return r[4] == kButtonLayoutDialog; });
        const int awards = find([](auto& r) { return r[4] == kAwardsDialog; });
        const int credits = find([](auto& r) { return r[4] == kCreditsHandler; });

        // Help & Options (in place: it only shrinks).
        if (controls >= 0 && awards >= 0 && !relocated.count(items)) {
          relocated[items] = items;
          std::vector<std::array<uint32_t, 5>> built;
          for (size_t i = 0; i < rows.size(); ++i) {
            if (int(i) == controls || int(i) == awards) continue;
            if (int(i) == credits) built.push_back(rows[awards]);
            built.push_back(rows[i]);
          }
          if (credits < 0) built.push_back(rows[awards]);
          for (size_t i = 0; i < built.size(); ++i)
            for (int w = 0; w < 5; ++w) WriteGuestU32(mem, items + uint32_t(i) * 20 + w * 4, built[i][w]);
          const uint32_t term = items + uint32_t(built.size()) * 20;
          WriteGuestU32(mem, term, kTypeEnd << 24);
          for (int w = 1; w < 5; ++w) WriteGuestU32(mem, term + w * 4, 0);
          ++help;
          REXLOG_INFO("[menu_patch] Help & Options array 0x{:08X}: Controls removed, Awards "
                      "before Credits ({} rows)", items, built.size());
          continue;
        }

        // Gamepad option screens without a way to the picture.
        if (ReadGuestU32(mem, a + 4) != kGamepadOptionsTitle || controls >= 0) continue;
        auto it = relocated.find(items);
        if (it == relocated.end()) {
          std::vector<std::array<uint32_t, 5>> built(rows.begin(), rows.end() - 1);
          built.push_back({0x04000000, 0x00000004, kLayoutTid, 0, kButtonLayoutDialog});
          built.push_back(rows.back());
          const uint32_t dest = mem->SystemHeapAlloc(uint32_t(built.size() + 1) * 20);
          if (!dest) continue;
          for (size_t i = 0; i < built.size(); ++i)
            for (int w = 0; w < 5; ++w) WriteGuestU32(mem, dest + uint32_t(i) * 20 + w * 4, built[i][w]);
          const uint32_t term = dest + uint32_t(built.size()) * 20;
          WriteGuestU32(mem, term, kTypeEnd << 24);
          for (int w = 1; w < 5; ++w) WriteGuestU32(mem, term + w * 4, 0);
          it = relocated.emplace(items, dest).first;
          REXLOG_INFO("[menu_patch] Gamepad options array 0x{:08X} -> 0x{:08X}: Button Layout added",
                      items, dest);
        }
        WriteGuestU32(mem, a + 8, it->second);
        ++layout;
      }
    });
    REXLOG_INFO("[menu_patch] Help & Options: {} list(s) arranged; Button Layout in {} gamepad "
                "screen(s)", help, layout);
  }

  // Help & Options > Reinstall Extras reinstalls the Xbox 360 dashboard theme,
  // gamer pictures and avatar awards. None of that exists here: the page holds
  // one row ("Reinstall Avatar Award 1") that does nothing and drops straight
  // back. The row is taken out of every menu that carries it.
  //
  // The row is found by the dialog it opens (+16), not by its label: unlike
  // the Settings rows, these carry a label *thunk* at +8 (0x82276920), so
  // there is no text id to match. The dialog is located by its title tid.
  //
  // Removing shrinks the array, so unlike the Settings patch this rewrites in
  // place: the rows after it move up and a new terminator goes at the end.
  // Rows whose param (+12) numbers them sequentially are renumbered.
  static void RemoveDeadConsoleRows() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) return;
    auto* mem = app->runtime()->memory();
    constexpr uint32_t kReinstallTid = 0x8CA8;
    constexpr uint32_t kTypeEnd = 0x1D;

    // The Reinstall Extras dialogdef: {0x01000000, title tid, items, 0, 0, 0}.
    uint32_t reinstall_dialog = 0;
    ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
      for (uint32_t a = region_addr; a + 24 <= region_addr + region_size; a += 4) {
        if (ReadGuestU32(mem, a) == 0x01000000 && ReadGuestU32(mem, a + 4) == kReinstallTid) {
          reinstall_dialog = a;
        }
      }
    });
    if (!reinstall_dialog) {
      REXLOG_INFO("[menu_patch] Reinstall Extras dialog not found; menus left as they are");
      return;
    }
    REXLOG_INFO("[menu_patch] Reinstall Extras dialog at 0x{:08X}", reinstall_dialog);
    int removed = 0;
    std::map<uint32_t, bool> seen;  // one pass per item array, however many dialogs share it
    ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
      for (uint32_t a = region_addr; a + 24 <= region_addr + region_size; a += 4) {
        if (ReadGuestU32(mem, a) != 0x01000000) continue;
        const uint32_t items = ReadGuestU32(mem, a + 8);
        if (items < 0x82000000 || items >= 0x826E0000 - 20) continue;
        if (seen.count(items)) continue;

        int count = 0, row = -1;
        while (count < 24) {
          const uint32_t type = ReadGuestU32(mem, items + count * 20) >> 24;
          if (type == kTypeEnd) break;
          if (type == 0) { count = -1; break; }
          if (ReadGuestU32(mem, items + count * 20 + 16) == reinstall_dialog) row = count;
          ++count;
        }
        if (count <= 0 || row < 0) continue;
        seen[items] = true;

        bool sequential = true;
        const uint32_t base = ReadGuestU32(mem, items + 12);
        for (int i = 0; i < count; ++i) {
          if (ReadGuestU32(mem, items + i * 20 + 12) != base + uint32_t(i)) {
            sequential = false;
            break;
          }
        }
        for (int i = row; i + 1 < count; ++i) {
          for (int w = 0; w < 5; ++w) {
            WriteGuestU32(mem, items + i * 20 + w * 4,
                          ReadGuestU32(mem, items + (i + 1) * 20 + w * 4));
          }
        }
        const int out = count - 1;
        if (sequential) {
          for (int i = 0; i < out; ++i) WriteGuestU32(mem, items + i * 20 + 12, base + uint32_t(i));
        }
        const uint32_t term = items + out * 20;
        WriteGuestU32(mem, term + 0, kTypeEnd << 24);
        for (int w = 1; w < 5; ++w) WriteGuestU32(mem, term + w * 4, 0);
        ++removed;
        REXLOG_INFO("[menu_patch] Reinstall Extras removed from array 0x{:08X} (row {} of {})",
                    items, row, count);
      }
    });
    REXLOG_INFO("[menu_patch] Reinstall Extras: {} menu(s) cleaned", removed);
  }

  // TEMP_MENU_SCAN: dump the whole guest static image (code+rodata+data+bss)
  // to a file so it can be analysed offline in Python without a rebuild cycle
  // per hypothesis. The thing we're hunting - a static array of N64-style
  // `struct menuitem` (whose last field is a handler function pointer) -
  // should show up as a run of valid code pointers at a constant stride.
  static void DebugDumpStaticImage() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[image_dump] runtime/memory not ready");
      return;
    }
    auto* mem = app->runtime()->memory();
    const uint32_t kImageStart = 0x82000000;
    const uint32_t kImageEnd = 0x826E0000;
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(kImageStart);
    if (!p) {
      REXLOG_INFO("[image_dump] could not translate image base");
      return;
    }
    FILE* f = fopen("guest_image.bin", "wb");
    if (!f) {
      REXLOG_INFO("[image_dump] could not open guest_image.bin");
      return;
    }
    size_t len = kImageEnd - kImageStart;
    fwrite(p, 1, len, f);
    fclose(f);
    REXLOG_INFO("[image_dump] wrote guest_image.bin ({} bytes, base 0x{:08X})", len, kImageStart);
  }

  static void DebugScanForFilenameStrings() {
    PerfectDarkApp* app = current_instance_;
    if (!app || !app->runtime() || !app->runtime()->memory()) {
      REXLOG_INFO("[filename_scan] runtime/memory not ready");
      return;
    }
    auto* mem = app->runtime()->memory();

    static const char* kNeedles[] = {
        "XStr_Original.cmp", "XStr_Original", "XStr", ".cmp", "Original.cmp",
    };

    int hits = 0;
    ForEachCommittedRegion(mem, [&](uint32_t region_addr, uint32_t region_size) {
      uint8_t* host = mem->TranslateVirtual<uint8_t*>(region_addr);
      if (!host) return;
      for (const char* needle : kNeedles) {
        size_t needle_len = strlen(needle);
        if (needle_len > region_size) continue;
        size_t limit = region_size - needle_len;
        for (size_t off = 0; off <= limit; ++off) {
          if (memcmp(host + off, needle, needle_len) == 0) {
            uint32_t hit_addr = region_addr + static_cast<uint32_t>(off);
            REXLOG_INFO("[filename_scan] HIT \"{}\" at 0x{:08X}", needle, hit_addr);
            uint32_t dump_start = hit_addr >= 32 ? hit_addr - 32 : 0;
            for (uint32_t a = dump_start; a < hit_addr + 32; a += 16) {
              uint8_t* row = mem->TranslateVirtual<uint8_t*>(a);
              std::string words, ascii;
              for (int k = 0; k < 16; k += 4) {
                uint32_t w = (uint32_t(row[k]) << 24) | (uint32_t(row[k + 1]) << 16) |
                             (uint32_t(row[k + 2]) << 8) | uint32_t(row[k + 3]);
                words += fmt::format("{:08X} ", w);
              }
              for (int k = 0; k < 16; ++k) {
                ascii += (row[k] >= 0x20 && row[k] < 0x7f) ? static_cast<char>(row[k]) : '.';
              }
              REXLOG_INFO("[filename_scan]   0x{:08X}: {} | {}", a, words, ascii);
            }
            hits++;
          }
        }
      }
    });
    REXLOG_INFO("[filename_scan] Done. {} hits.", hits);

    // TEMP_MENU_SCAN: the actual load call site found via static analysis
    // (perfect_dark_recomp.13.cpp:63030-63067, inside the function containing
    // loc_8226C704) passes r3=0x82652968 to sub_82254838 right alongside
    // r4=0x8201955C ("XStr_Original.cmp"). Dump it directly to see what kind
    // of string/value it is - likely a resource-category name paired with
    // the filename.
    for (uint32_t addr : {0x82652968u}) {
      uint8_t* p = mem->TranslateVirtual<uint8_t*>(addr);
      if (!p) {
        REXLOG_INFO("[filename_scan] 0x{:08X}: unreadable", addr);
        continue;
      }
      std::string ascii;
      for (int k = 0; k < 64; ++k) {
        ascii += (p[k] >= 0x20 && p[k] < 0x7f) ? static_cast<char>(p[k]) : '.';
      }
      REXLOG_INFO("[filename_scan] 0x{:08X} (64 bytes): {}", addr, ascii);
    }
  }

  static void SavePcSettings() {
    PerfectDarkApp* app = current_instance_;
    if (!app || app->config_path().empty()) {
      return;
    }
    // SaveConfig writes every cvar that differs from its default, which would
    // include the built-in online server address applied at startup. That is
    // this build's address, not a choice the player made: writing it into
    // perfect_dark.toml would freeze it there and a later build's server would
    // never be used. It is taken back out for the duration of the save.
    const std::string relay = rex::cvar::GetFlagByName("net_relay_server");
    const bool built_in = !relay.empty() && relay == kOnlineServer;
    if (built_in) rex::cvar::SetFlagByName("net_relay_server", "");
    // The same for resolution_scale: this run set it from Render Scale, and
    // only what the file itself held (a hand-edited value, or nothing) goes back.
    const std::string running_scale = rex::cvar::GetFlagByName("resolution_scale");
    if (render_scale_set_) {
      rex::cvar::SetFlagByName("resolution_scale", config_resolution_scale_.empty()
                                                       ? std::string("1")
                                                       : config_resolution_scale_);
    }
    rex::cvar::SaveConfig(app->config_path());
    if (render_scale_set_) rex::cvar::SetFlagByName("resolution_scale", running_scale);
    if (built_in) rex::cvar::SetFlagByName("net_relay_server", relay);
  }

  // ---- Render Scale (Settings > Video; HANDOVER 4ai) ------------------------
  // The internal rendering sharpness, as a multiple of the Xbox's 1280 x 720:
  // 1x, 2x or 3x (pd_render_scale, default 1x). It sizes the emulated GPU's
  // memory when the GPU starts, so it is applied in OnPreSetup and a change
  // needs a restart: from the front end the game restarts itself at once;
  // during a mission or a networked match it waits for the next start. The
  // scales other than the one running read "(requires restart)".
  //
  // There was an Auto choice (by display height and GPU, 4ai); a tester found
  // it confusing next to Resolution, so the user had it removed (2026-10-25,
  // HANDOVER 4aq). An old "auto" in perfect_dark.toml now means 1x.
  static inline std::string config_resolution_scale_;  // the line in the file, if any
  static inline bool render_scale_set_ = false;        // this run chose resolution_scale
  static inline int running_render_scale_ = 0;         // what this run renders at
  static inline int pending_render_scale_index_ = -1;  // chosen, applies next start

  struct GpuForScale {
    bool known = false;
    bool integrated = false;
    uint64_t video_memory_mb = 0;
    std::string name;
    std::string driver;  // as the vendor writes it (NVIDIA: 566.36), or empty
  };

  // The first hardware adapter (the one the renderer uses), read without
  // running anything on the GPU; DXGI and D3D12 are loaded at run time and
  // any failure reports "unknown" (treated as a dedicated GPU with memory).
  static const GpuForScale& DetectGpuForScale() {
    static const GpuForScale gpu = ReadPrimaryGpu();
    return gpu;
  }

  static GpuForScale ReadPrimaryGpu() {
    GpuForScale out;
    HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
    HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
    if (dxgi && d3d12) {
      using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
      auto create_factory1 =
          reinterpret_cast<CreateFactory1Fn>(::GetProcAddress(dxgi, "CreateDXGIFactory1"));
      auto create_device =
          reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(::GetProcAddress(d3d12, "D3D12CreateDevice"));
      IDXGIFactory1* factory = nullptr;
      if (create_factory1 && create_device &&
          SUCCEEDED(create_factory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
          DXGI_ADAPTER_DESC1 desc;
          const bool hardware =
              SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
          if (hardware) {
            out.known = true;
            out.video_memory_mb = uint64_t(desc.DedicatedVideoMemory) / (1024 * 1024);
            char name[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr,
                                nullptr);
            out.name = name;
            // The user-mode driver version, a.b.c.d. NVIDIA's own number is the
            // last five digits of c and d together: 32.0.15.6636 is 566.36.
            LARGE_INTEGER umd{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
              const unsigned a = HIWORD(umd.HighPart), b = LOWORD(umd.HighPart);
              const unsigned c = HIWORD(umd.LowPart), d = LOWORD(umd.LowPart);
              out.driver = fmt::format("{}.{}.{}.{}", a, b, c, d);
              if (desc.VendorId == 0x10DE) {
                const unsigned nv = (c % 10) * 10000 + d;
                out.driver += fmt::format(" ({}.{:02})", nv / 100, nv % 100);
              }
            }
            ID3D12Device* device = nullptr;
            if (SUCCEEDED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                        reinterpret_cast<void**>(&device)))) {
              D3D12_FEATURE_DATA_ARCHITECTURE arch = {};
              if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch,
                                                        sizeof(arch)))) {
                out.integrated = arch.UMA != FALSE;
              }
              device->Release();
            }
            adapter->Release();
            break;
          }
          adapter->Release();
        }
        factory->Release();
      }
    }
    if (d3d12) ::FreeLibrary(d3d12);
    if (dxgi) ::FreeLibrary(dxgi);
    return out;
  }

  // The display the game opens on: the SDK's monitor setting (0 default,
  // 1 primary, 2.. by enumeration order) or the primary.
  static HMONITOR GameMonitor() {
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const int option = std::atoi(rex::cvar::GetFlagByName("monitor").c_str());
    if (option > 1) {
      std::vector<HMONITOR> all;
      EnumDisplayMonitors(
          nullptr, nullptr,
          [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            reinterpret_cast<std::vector<HMONITOR>*>(p)->push_back(m);
            return TRUE;
          },
          reinterpret_cast<LPARAM>(&all));
      if (option <= int(all.size())) monitor = all[size_t(option - 1)];
    }
    return monitor;
  }

  // The chosen scale, 1 to 3 (an old "auto" is 1).
  static int RenderScaleChoice() {
    const std::string choice = rex::cvar::GetFlagByName("pd_render_scale");
    if (choice == "2") return 2;
    if (choice == "3") return 3;
    return 1;
  }

  // The resolution_scale line as written in perfect_dark.toml, if there is one
  // (the cvar itself cannot tell a written value from its default).
  static std::string ReadConfigResolutionScale(const std::filesystem::path& config) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, config.wstring().c_str(), L"rb") != 0 || !f) return {};
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    size_t pos = 0;
    while ((pos = text.find("resolution_scale", pos)) != std::string::npos) {
      const bool line_start = pos == 0 || text[pos - 1] == '\n';
      size_t i = pos + 16;
      pos = i;
      if (!line_start) continue;
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
      if (i >= text.size() || text[i] != '=') continue;
      ++i;
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
      std::string value;
      while (i < text.size() && text[i] >= '0' && text[i] <= '9') value += text[i++];
      return value;
    }
    return {};
  }

  // ---- Log header (HANDOVER 4ak) --------------------------------------------
  // What a problem report needs to know about the PC, written once at start:
  // Windows, CPU, memory, GPU and driver, every active display's mode with its
  // exact refresh rate (a "60 Hz" screen is often 59.94 or 60.1), and the
  // settings that change timing.
  static std::string Utf8(const wchar_t* text) {
    char out[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out, sizeof(out) - 1, nullptr, nullptr);
    return out;
  }

  static std::string WindowsText() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    auto get_version =
        ntdll ? reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"))
              : nullptr;
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (!get_version || get_version(&v) != 0) return "unknown";
    DWORD ubr = 0, size = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                 RRF_RT_REG_DWORD, nullptr, &ubr, &size);
    // Windows 11 still reports itself as 10.0; its builds start at 22000.
    const bool eleven = v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000;
    return fmt::format("{} {}.{}.{}.{}", eleven ? "Windows 11" : "Windows", v.dwMajorVersion,
                       v.dwMinorVersion, v.dwBuildNumber, ubr);
  }

  static std::string CpuText() {
    int regs[4] = {};
    char brand[49] = {};
    __cpuid(regs, int(0x80000000));
    if (unsigned(regs[0]) >= 0x80000004u) {
      for (int i = 0; i < 3; ++i) {
        __cpuid(regs, int(0x80000002 + i));
        std::memcpy(brand + i * 16, regs, 16);
      }
    }
    std::string name = brand;
    name.erase(0, name.find_first_not_of(' '));
    while (!name.empty() && name.back() == ' ') name.pop_back();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    // Installed memory (64 GB), not what Windows has left after reserving some (63).
    ULONGLONG installed_kb = 0;
    if (!GetPhysicallyInstalledSystemMemory(&installed_kb)) {
      MEMORYSTATUSEX mem{};
      mem.dwLength = sizeof(mem);
      GlobalMemoryStatusEx(&mem);
      installed_kb = mem.ullTotalPhys / 1024;
    }
    return fmt::format("{}, {} threads, {:.0f} GB memory", name.empty() ? "unknown" : name,
                       si.dwNumberOfProcessors, double(installed_kb) / (1024.0 * 1024));
  }

  // One line per active display: its name, the monitor's own name, the
  // desktop mode and the exact refresh rate (QueryDisplayConfig's fraction).
  static std::vector<std::string> DisplayLines() {
    std::vector<std::string> out;
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) !=
        ERROR_SUCCESS) {
      return out;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                           modes.data(), nullptr) != ERROR_SUCCESS) {
      return out;
    }
    paths.resize(path_count);
    MONITORINFOEXW game{};
    game.cbSize = sizeof(game);
    if (!GetMonitorInfoW(GameMonitor(), &game)) game.szDevice[0] = 0;
    for (const auto& path : paths) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
      source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      source.header.size = sizeof(source);
      source.header.adapterId = path.sourceInfo.adapterId;
      source.header.id = path.sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) source.viewGdiDeviceName[0] = 0;
      DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
      target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      target.header.size = sizeof(target);
      target.header.adapterId = path.targetInfo.adapterId;
      target.header.id = path.targetInfo.id;
      if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
        target.monitorFriendlyDeviceName[0] = 0;
      }
      uint32_t width = 0, height = 0;
      const UINT32 index = path.sourceInfo.modeInfoIdx;
      if (index < modes.size() && modes[index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        width = modes[index].sourceMode.width;
        height = modes[index].sourceMode.height;
      }
      const auto& rate = path.targetInfo.refreshRate;
      const double hz = rate.Denominator ? double(rate.Numerator) / double(rate.Denominator) : 0.0;
      const bool is_game = game.szDevice[0] && wcscmp(game.szDevice, source.viewGdiDeviceName) == 0;
      const std::string monitor = Utf8(target.monitorFriendlyDeviceName);
      out.push_back(fmt::format("display {}{}: {} x {} at {:.3f} Hz{}",
                                Utf8(source.viewGdiDeviceName),
                                monitor.empty() ? std::string() : " (" + monitor + ")", width,
                                height, hz, is_game ? ", the game's display" : ""));
    }
    return out;
  }

  static void LogSystemInfo() {
    REXLOG_INFO("[system] {}", WindowsText());
    REXLOG_INFO("[system] CPU: {}", CpuText());
    const GpuForScale& gpu = DetectGpuForScale();
    if (gpu.known) {
      REXLOG_INFO("[system] GPU: {} ({}, {} MB), driver {}", gpu.name,
                  gpu.integrated ? "integrated" : "dedicated", gpu.video_memory_mb,
                  gpu.driver.empty() ? "unknown" : gpu.driver);
    }
    for (const auto& line : DisplayLines()) REXLOG_INFO("[system] {}", line);
    auto flag = [](const char* name) { return rex::cvar::GetFlagByName(name); };
    REXLOG_INFO("[system] settings: gpu {}, fullscreen {}, vsync {}, render scale {}x, "
                "upscaling {}, smooth game clock {}",
                flag("gpu"), flag("fullscreen"), flag("vsync"), running_render_scale_,
                flag("present_effect"), flag("pd_smooth_game_clock"));
    REXLOG_INFO("[system] diagnostics: {}", flag("diagnostics") == "true"
                                                ? "on (detailed log)"
                                                : "off (set diagnostics = true in "
                                                  "resources\\userdata\\perfect_dark.toml "
                                                  "for a detailed log)");
  }

  // ---- Settings from older releases (HANDOVER 4ak) ---------------------------
  // Setup writes perfect_dark.toml only when there is none, so after an update a
  // player's file still holds what the older release shipped, even where the new
  // one ships something else. Each step here runs once (pd_settings_revision)
  // and changes a value only if it is still exactly the old shipped one, so a
  // value the player chose is left alone. A fresh file says it is current.
  static void MigrateSettings() {
    constexpr int kRevision = 1;
    const int had = std::atoi(rex::cvar::GetFlagByName("pd_settings_revision").c_str());
    if (had >= kRevision) return;
    std::string changed;
    if (had < 1) {
      // v0.9.9.2 shipped 0.34; 0.37 since 2026-10-25 (the user found dark
      // rooms very slightly too dark).
      const double v = std::atof(rex::cvar::GetFlagByName("pd_dark_room_visibility").c_str());
      if (std::fabs(v - 0.34) < 1e-6) {
        rex::cvar::SetFlagByName("pd_dark_room_visibility", "0.37");
        changed += " pd_dark_room_visibility 0.34 -> 0.37";
      }
    }
    rex::cvar::SetFlagByName("pd_settings_revision", std::to_string(kRevision));
    SavePcSettings();
    REXLOG_INFO("[settings] perfect_dark.toml updated from revision {} to {}:{}", had, kRevision,
                changed.empty() ? " nothing to change" : changed);
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    (void)config;
    PdInstallCrashReport();
    config_resolution_scale_ = ReadConfigResolutionScale(config_path());
    running_render_scale_ = RenderScaleChoice();
    rex::cvar::SetFlagByName("resolution_scale", std::to_string(running_render_scale_));
    render_scale_set_ = true;
    REXLOG_INFO("[render_scale] {}x", running_render_scale_);
  }

  // Front end (title, attract film, the Institute's menus) with no networked
  // match: restarting loses nothing there.
  static bool RenderScaleRestartIsSafe() {
    namespace netplay = rex::kernel::xam::netplay;
    if (netplay::IsHostingSession() || netplay::IsJoinedSession()) return false;
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    if (!mem) return false;
    const uint32_t stage = ReadGuestU32(mem, kStageNum);
    return stage == kStageTitle || stage == 38 || menu_attract_;
  }

  // Starts a fresh copy of the game with the same command line and ends this
  // one at once (a normal shutdown can stall and leave the old window up).
  [[noreturn]] static void RestartGame() {
    wchar_t exe[32768] = {};
    GetModuleFileNameW(nullptr, exe, 32768);
    std::wstring command = GetCommandLineW();
    const std::filesystem::path folder = std::filesystem::path(exe).parent_path();
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       folder.c_str(), &startup, &process)) {
      AllowSetForegroundWindow(process.dwProcessId);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      REXLOG_INFO("[render_scale] restarting to apply the new Render Scale");
    } else {
      REXLOG_ERROR("[render_scale] could not restart (error {}); applies next start",
                   GetLastError());
    }
    TerminateProcess(GetCurrentProcess(), 0);
    for (;;) {
    }
  }

  // index 0-2 = 1x-3x.
  static void ApplyRenderScaleIndex(int index) {
    if (index < 0 || index > 2) return;
    static const char* kChoice[] = {"1", "2", "3"};
    rex::cvar::SetFlagByName("pd_render_scale", kChoice[index]);
    SavePcSettings();
    const int wanted = index + 1;
    if (wanted == running_render_scale_) {
      pending_render_scale_index_ = -1;
      return;
    }
    if (RenderScaleRestartIsSafe()) {
      RestartGame();
    }
    pending_render_scale_index_ = index;
    rex::kernel::xam::netplay::ShowNotice(
        "Render Scale changes the next time the game starts.");
  }

  // ---- Upscaling (Settings > Video; HANDOVER 4an) ----------------------------
  // How the rendered picture is scaled to the window: Off (bilinear, the
  // original look), Sharpen (AMD CAS: contrast-adaptive sharpening, and its
  // own resampling when the sizes differ) or FSR (AMD FSR 1: edge-adaptive
  // upscaling plus sharpening). FSR is the one that matters with Render Scale
  // 1x on a large screen; with the picture already at the window's size it
  // only sharpens. present_effect in the SDK; applied at once.
  static int UpscalingIndex() {
    const std::string effect = rex::cvar::GetFlagByName("present_effect");
    if (effect == "cas") return 1;
    if (effect == "fsr") return 2;
    return 0;
  }

  static void ApplyUpscalingIndex(int index) {
    static const char* const kEffects[] = {"bilinear", "cas", "fsr"};
    if (index < 0 || index > 2 || index == UpscalingIndex()) return;
    rex::cvar::SetFlagByName("present_effect", kEffects[index]);
    SavePcSettings();
    REXLOG_INFO("[upscaling] present_effect = {}", kEffects[index]);
    PerfectDarkApp* app = current_instance_;
    if (!app) return;
    app->app_context().CallInUIThreadDeferred([]() {
      auto* graphics = ConcreteGraphics();
      if (graphics && graphics->presenter()) {
        graphics->presenter()->ReloadGuestOutputPaintConfigFromCVarsFromUIThread();
      }
    });
  }

  // Same op protocol as PdResolutionMenuHandler.
  static void PdUpscalingMenuHandler(PPCContext& ctx, uint8_t* /*base*/) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    uint32_t ret = 0;
    switch (op) {
      case 1:
        if (mem) WriteGuestU32(mem, data, 3);
        break;
      case 3: {
        uint32_t i = mem ? ReadGuestU32(mem, data) : 0;
        if (i > 2) i = 0;
        ret = kUpscalingStrings + i * 16;
        break;
      }
      case 6: {
        uint32_t i = mem ? ReadGuestU32(mem, data) : 0;
        if (i > 2) i = 0;
        ApplyUpscalingIndex(int(i));
        break;
      }
      case 7:
        if (mem) WriteGuestU32(mem, data, uint32_t(UpscalingIndex()));
        break;
      default:
        break;
    }
    ctx.r3.u64 = ret;
  }

  // Same op protocol as PdResolutionMenuHandler.
  static void PdRenderScaleMenuHandler(PPCContext& ctx, uint8_t* /*base*/) {
    PerfectDarkApp* app = current_instance_;
    auto* mem = (app && app->runtime()) ? app->runtime()->memory() : nullptr;
    const uint32_t op = ctx.r3.u32;
    const uint32_t data = ctx.r5.u32;
    uint32_t ret = 0;
    switch (op) {
      case 1:
        if (mem) WriteGuestU32(mem, data, 3);
        break;
      case 3: {
        uint32_t i = mem ? ReadGuestU32(mem, data) : 0;
        if (i > 2) i = 0;
        ret = int(i) + 1 == running_render_scale_ ? kRenderScaleStrings + i * 16
                                                  : kRenderScaleRestartStrings + i * 32;
        break;
      }
      case 6: {
        uint32_t i = mem ? ReadGuestU32(mem, data) : 0;
        if (i > 2) i = 0;
        ApplyRenderScaleIndex(int(i));
        break;
      }
      case 7:
        if (mem) {
          const int shown = pending_render_scale_index_ >= 0 ? pending_render_scale_index_
                                                             : RenderScaleChoice() - 1;
          WriteGuestU32(mem, data, uint32_t(shown));
        }
        break;
      default:
        break;
    }
    ctx.r3.u64 = ret;
  }

  static void SetPcResolution(int32_t width, int32_t height, int32_t scale) {
    PerfectDarkApp* app = current_instance_;
    if (!app) {
      return;
    }

    // rex::cvar::SetFlagByName returns *success*, not *value changed* - it is
    // true even when the new value equals the old one. Comparing first matters
    // for resolution_scale: ReinitializeContext() is a full GPU teardown
    // (ShutdownContext + SetupContext), which discards every cached GPU-side
    // surface. Firing it when the scale did not actually change costs a
    // needless teardown and blacks out the menu's 3D backdrop, which the game
    // renders once and never regenerates.
    const std::string scale_str = std::to_string(scale);
    const bool scale_differs = rex::cvar::GetFlagByName("resolution_scale") != scale_str;

    bool changed = false;
    changed |= rex::cvar::SetFlagByName("video_mode_width", std::to_string(width));
    changed |= rex::cvar::SetFlagByName("video_mode_height", std::to_string(height));
    bool scale_changed = rex::cvar::SetFlagByName("resolution_scale", scale_str) && scale_differs;
    changed |= scale_changed;

    if (changed) {
      SavePcSettings();
    }

    // Rebuild scale-dependent graphics resources immediately.
    if (scale_changed && app->runtime() && app->runtime()->graphics_system()) {
      app->runtime()->graphics_system()->ReinitializeContext();
    }

    // Apply the output/window resolution immediately.
    app->app_context().CallInUIThreadDeferred([app, width, height]() {
      if (app->window()) {
        auto* sdl_window = static_cast<rex::ui::WindowSDL*>(app->window());
        sdl_window->SetLogicalSize(width, height);
      }
    });
  }

  static int32_t GetPcResolutionIndex() {
    std::string width = rex::cvar::GetFlagByName("video_mode_width");
    std::string height = rex::cvar::GetFlagByName("video_mode_height");

    if (width == "1280" && height == "720") return 0;
    if (width == "1920" && height == "1080") return 1;
    if (width == "2560" && height == "1440") return 2;
    if (width == "3840" && height == "2160") return 3;
    return 1;
  }

  static bool GetPcVsync() {
    std::string value = rex::cvar::GetFlagByName("vsync");
    return value == "true" || value == "1";
  }

  static void SetPcVsync(bool enabled) {
    // command_processor.cpp reads REXCVAR_GET(vsync) every loop iteration, so
    // this takes effect on the next frame - no context reinit needed.
    if (rex::cvar::SetFlagByName("vsync", enabled ? "true" : "false")) {
      SavePcSettings();
    }
  }

  static bool GetPcShowFps() {
    std::string value = rex::cvar::GetFlagByName("show_fps");
    return value == "true" || value == "1";
  }

  static void SetPcShowFps(bool enabled) {
    if (rex::cvar::SetFlagByName("show_fps", enabled ? "true" : "false")) {
      SavePcSettings();
    }
  }

  // F9 > Online Aim Delay (pd_mp_facing_delay_ms, see src/main.cpp).
  static int GetPcFacingDelay() { return REXCVAR_GET(pd_mp_facing_delay_ms); }

  static void SetPcFacingDelay(int ms) {
    if (rex::cvar::SetFlagByName("pd_mp_facing_delay_ms", std::to_string(ms))) {
      SavePcSettings();
    }
  }

  static bool GetPcFullscreen() {
    std::string value = rex::cvar::GetFlagByName("fullscreen");
    return value == "true" || value == "1";
  }

  static void SetPcFullscreen(bool fullscreen) {
    PerfectDarkApp* app = current_instance_;
    if (!app) {
      return;
    }

    if (rex::cvar::SetFlagByName("fullscreen", fullscreen ? "true" : "false")) {
      SavePcSettings();
    }

    app->app_context().CallInUIThreadDeferred([app, fullscreen]() {
      if (app->window()) {
        app->window()->SetFullscreen(fullscreen);
      }
    });
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}

 private:
  inline static PerfectDarkApp* current_instance_ = nullptr;
  rex::ui::ImGuiDrawer* imgui_drawer_ = nullptr;
  bool settings_dialog_open_ = false;
  bool match_picker_open_ = false;

  // TEMP_MENU_SCAN: addresses found by the last F10 scan, reused by the
  // F8/F6 write-watch feature so it doesn't need hardcoded addresses.
  inline static std::vector<uint32_t> last_scan_string_addrs_;
  inline static std::vector<uint32_t> last_scan_ptr_addrs_;
};

inline void PerfectDarkInputTracker::OnDraw(ImGuiIO& /*io*/) {
  // F9 opens the PC settings panel - only with enable_rexglue_menus set, as
  // players change these settings in the game's own menus. A dedicated key
  // nothing else in the game binds. Edge-triggered off a raw GetAsyncKeyState poll, since the
  // settings dialog and this tracker aren't part of the guest input pipeline
  // (which is suspended while a modal ImGui popup is open anyway).
  PerfectDarkApp::TickAutoMenuPatch();
  PerfectDarkApp::TickKeyCapture(app_ && app_->HasWindowFocus());
  PerfectDarkApp::TickStarterName();
  PerfectDarkApp::TickLobbyLabels();
  PerfectDarkApp::TickNameEntry();
  PerfectDarkApp::TickStoryComplete();
  PerfectDarkApp::TickFrameStats();
  PerfectDarkApp::TickMenuMouse();
  PerfectDarkApp::DrawMenuCursor();
  PerfectDarkApp::TickMatchDetails();
  PerfectDarkApp::DrawPrivateMatchNotice();
  PerfectDarkApp::TickControllerPrompts();
  PerfectDarkApp::TickTitlePrompt();
  PerfectDarkApp::DrawTitleVersion();
  if (app_) app_->TickMatchPicker();

  if (app_ && app_->HasWindowFocus()) {
    bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9_down && !f9_was_down_ && PerfectDarkApp::DeveloperMenusEnabled()) {
      app_->OpenSettingsDialog();
    }
    f9_was_down_ = f9_down;

    // Everything below is a developer diagnostic (see pd_dev_hotkeys).
    if (!REXCVAR_GET(pd_dev_hotkeys)) return;

    // TEMP_MENU_MOUSE
    bool f3_down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    if (f3_down && !f3_was_down_) {
      PerfectDarkApp::DebugFindMenuState();
    }
    f3_was_down_ = f3_down;

    // TEMP_MENU_SCAN
    bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10_down && !f10_was_down_) {
      PerfectDarkApp::DebugScanForMenuStrings();
    }
    f10_was_down_ = f10_down;

    // TEMP_MENU_SCAN
    // TEMP_MENU_SCAN: F11 repurposed from the (obsolete) snapshot/diff tool to
    // the native menu patch - see PatchVideoOptionsMenu.
    bool f11_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f11_down && !f11_was_down_) {
      PerfectDarkApp::PatchVideoOptionsMenu();
    }
    f11_was_down_ = f11_down;

    // TEMP_MENU_SCAN: F12 repurposed again - the sub_82344130 field trace
    // (g_trace_82344130_once) turned out to be a decoy (see HANDOVER.md);
    // now triggers DebugScanForFilenameStrings instead.
    bool f12_down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12_down && !f12_was_down_) {
      PerfectDarkApp::DebugDumpStaticImage();
    }
    f12_was_down_ = f12_down;

    // TEMP_LOOK_SCAN: F8 repurposed from the obsolete write-watch arm.
    bool f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8_down && !f8_was_down_) {
      PerfectDarkApp::DebugScanForLookAngles();
    }
    f8_was_down_ = f8_down;

    // TEMP_LIGHT_DIAG: F6 drops GPU caches (see DebugClearGpuCaches).
    bool f6_down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (f6_down && !f6_was_down_) {
      PerfectDarkApp::DebugClearGpuCaches();
    }
    f6_was_down_ = f6_down;

    // TEMP_LIGHT_DIAG: F5 forces a full guest-memory re-upload.
    bool f5_down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (f5_down && !f5_was_down_) {
      PerfectDarkApp::DebugInvalidateGpuMemory();
    }
    f5_was_down_ = f5_down;

    // TEMP_LIGHT_DIAG: Insert summarises pixel-shader constant c4 (the
    // per-object shade colour) over the next 4000 draws; see gpu_draw_capture
    // in the D3D12 command processor. Take one capture with the room lit and
    // one with it dark, from the same spot.
    bool ins_down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (ins_down && !ins_was_down_) {
      REXLOG_INFO("[draw_capture] armed for 4000 draws: {}",
                  rex::cvar::SetFlagByName("gpu_draw_capture", "4000"));
    }
    ins_was_down_ = ins_down;

    // TEMP_MENU_SCAN: F7 is reserved by the SDK achievements overlay - do not
    // bind anything here, it opens that panel and hides the guest.



    // TEMP_MENU_SCAN: F2 repurposed from the (decoy-chain) trace2 trigger to
    // ArmWriteWatchOnScreenSplitValueSlot - not F4, since the SDK itself
    // binds F4 to "toggle settings overlay" (rex_app.cpp, bind_settings),
    // which collided with this hotkey originally.
    bool f4_down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (f4_down && !f4_was_down_) {
      PerfectDarkApp::ArmWriteWatchOnScreenSplitValueSlot();
    }
    f4_was_down_ = f4_down;

    // TEMP_MENU_SCAN: F1 repurposed from ArmWriteWatchOnItemStruct (that
    // struct turned out to be an unrelated shader-parameter table, not menu
    // data) to DebugDumpStringTable instead. Press F10 first, then F1.
    bool f1_down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (f1_down && !f1_was_down_) {
      PerfectDarkApp::DebugDumpStringTable();
    }
    f1_was_down_ = f1_down;
  }
}

inline PerfectDarkSettingsDialog::InputEdges PerfectDarkSettingsDialog::PollInput() {
  InputEdges edges;

  if (!app_ || !app_->HasWindowFocus()) {
    return edges;
  }

  int vks[6] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE};
  bool now_down[6];
  for (int i = 0; i < 6; ++i) {
    now_down[i] = (GetAsyncKeyState(vks[i]) & 0x8000) != 0;
  }

  XINPUT_STATE state{};
  bool pad_connected = XInputGetState(0, &state) == ERROR_SUCCESS;

  if (!input_primed_) {
    for (int i = 0; i < 6; ++i) {
      key_was_down_[i] = now_down[i];
    }
    if (pad_connected) {
      pad_was_connected_ = true;
      pad_buttons_prev_ = state.Gamepad.wButtons;
    }
    input_primed_ = true;
    return edges;
  }

  edges.up = now_down[0] && !key_was_down_[0];
  edges.down = now_down[1] && !key_was_down_[1];
  edges.left = now_down[2] && !key_was_down_[2];
  edges.right = now_down[3] && !key_was_down_[3];
  edges.confirm = now_down[4] && !key_was_down_[4];
  edges.back = now_down[5] && !key_was_down_[5];
  for (int i = 0; i < 6; ++i) {
    key_was_down_[i] = now_down[i];
  }

  if (pad_connected) {
    pad_was_connected_ = true;
    WORD buttons = state.Gamepad.wButtons;
    WORD pressed = buttons & ~pad_buttons_prev_;
    if (pressed & XINPUT_GAMEPAD_DPAD_UP) edges.up = true;
    if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) edges.down = true;
    if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) edges.left = true;
    if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) edges.right = true;
    if (pressed & XINPUT_GAMEPAD_A) edges.confirm = true;
    if (pressed & XINPUT_GAMEPAD_B) edges.back = true;
    pad_buttons_prev_ = buttons;
  } else if (pad_was_connected_) {
    pad_buttons_prev_ = 0;
  }

  return edges;
}

inline void PerfectDarkSettingsDialog::MoveSelection(int delta) {
  selected_row_ = (selected_row_ + delta + kRowCount) % kRowCount;
}

inline void PerfectDarkSettingsDialog::CycleRow(int row, int delta) {
  switch (row) {
    case kRowFullscreen:
      PerfectDarkApp::SetPcFullscreen(!PerfectDarkApp::GetPcFullscreen());
      break;
    case kRowResolution: {
      static const int32_t kW[] = {1280, 1920, 2560, 3840};
      static const int32_t kH[] = {720, 1080, 1440, 2160};
      static const int32_t kScale[] = {1, 2, 2, 3};
      int index = PerfectDarkApp::GetPcResolutionIndex();
      index = (index + delta + 4) % 4;
      PerfectDarkApp::SetPcResolution(kW[index], kH[index], kScale[index]);
      break;
    }
    case kRowShowFps:
      PerfectDarkApp::SetPcShowFps(!PerfectDarkApp::GetPcShowFps());
      break;
    case kRowFacingDelay: {
      // 200 (the game's own) down to 0; a value set by hand starts from the
      // nearest step.
      static const int kSteps[] = {200, 100, 50, 0};
      const int now = PerfectDarkApp::GetPcFacingDelay();
      auto gap = [now](int step) { return step > now ? step - now : now - step; };
      int index = 0;
      for (int i = 1; i < 4; ++i) {
        if (gap(kSteps[i]) < gap(kSteps[index])) index = i;
      }
      index = (index + delta + 4) % 4;
      PerfectDarkApp::SetPcFacingDelay(kSteps[index]);
      break;
    }
    default:
      break;
  }
}

inline void PerfectDarkSettingsDialog::ActivateRow(int row) {
  // Only Done responds to A/Enter; every other row's value only changes via
  // Left/Right, so a confirm press can't accidentally change a setting.
  if (row == kRowDone) {
    ImGui::CloseCurrentPopup();
    Close();
  }
}

inline void PerfectDarkSettingsDialog::DrawRow(ImDrawList* draw_list, ImVec2 pos, ImVec2 size,
                                               int row, bool selected, const char* label,
                                               const char* value, bool has_value) {
  using namespace pd_ui;

  ImVec2 row_max(pos.x + size.x, pos.y + size.y);
  if (selected) {
    draw_list->AddRectFilledMultiColor(pos, row_max, kRowSelectedTop, kRowSelectedTop,
                                       kRowSelectedBottom, kRowSelectedBottom);
    draw_list->AddRect(pos, row_max, kRowSelectedBorder, 6.0f, 0, 2.0f);
  } else {
    draw_list->AddRectFilled(pos, row_max, kRowIdle, 6.0f);
    draw_list->AddRect(pos, row_max, kRowIdleBorder, 6.0f, 0, 1.5f);
  }

  float label_size = ImGui::GetFontSize();
  ImVec2 label_pos(pos.x + 20.0f, pos.y + (size.y - label_size) * 0.5f);
  draw_list->AddText(nullptr, label_size, label_pos, selected ? kTextSelected : kTextNormal,
                     label);

  if (has_value) {
    float chevron_inset = 34.0f;
    ImU32 chevron_color = selected ? kChevron : kChevronDim;
    DrawChevron(draw_list, ImVec2(row_max.x - size.x * 0.42f, pos.y + size.y * 0.5f), 7.0f, false,
               chevron_color);
    DrawChevron(draw_list, ImVec2(row_max.x - chevron_inset, pos.y + size.y * 0.5f), 7.0f, true,
               chevron_color);

    ImVec2 value_text_size = ImGui::CalcTextSize(value);
    float center_x = row_max.x - (size.x * 0.42f + chevron_inset) * 0.5f;
    ImVec2 value_pos(center_x - value_text_size.x * 0.5f, pos.y + (size.y - label_size) * 0.5f);
    draw_list->AddText(nullptr, label_size, value_pos, selected ? kTextSelected : kTextDim, value);
  }
}

inline void PerfectDarkSettingsDialog::OnDraw(ImGuiIO& io) {
  if (!has_opened_shown_) {
    ImGui::OpenPopup("##PerfectDarkPcSettings");
    has_opened_shown_ = true;
  }

  using namespace pd_ui;

  InputEdges input = PollInput();
  if (input.up) MoveSelection(-1);
  if (input.down) MoveSelection(1);
  if (input.left) CycleRow(selected_row_, -1);
  if (input.right) CycleRow(selected_row_, 1);
  if (input.confirm) ActivateRow(selected_row_);

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 viewport_center =
      viewport ? ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f)
               : ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(viewport_center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  constexpr float kWindowWidth = 460.0f;
  ImGui::SetNextWindowSize(ImVec2(kWindowWidth, 0), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));

  bool still_open =
      ImGui::BeginPopupModal("##PerfectDarkPcSettings", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  if (still_open) {
    if (input.back) {
      ImGui::CloseCurrentPopup();
      Close();
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 window_pos = ImGui::GetWindowPos();

    constexpr int kRows = kRowCount;
    constexpr float kRowHeight = 42.0f;
    constexpr float kRowSpacing = 8.0f;
    constexpr float kSideMargin = 22.0f;
    constexpr float kTopContent = 66.0f;
    constexpr float kBottomMargin = 20.0f;
    float content_height =
        kTopContent + kRows * kRowHeight + (kRows - 1) * kRowSpacing + kBottomMargin;
    ImVec2 window_size(kWindowWidth, content_height);
    ImVec2 window_end(window_pos.x + window_size.x, window_pos.y + window_size.y);

    ImGui::Dummy(window_size);

    draw_list->PushClipRect(window_pos, window_end, true);
    ImVec2 card_min(window_pos.x + 8, window_pos.y + 6);
    ImVec2 card_max(window_end.x - 8, window_end.y - 6);
    draw_list->AddRectFilledMultiColor(card_min, card_max, kCardTop, kCardTop, kCardBottom,
                                       kCardBottom);
    draw_list->AddRect(card_min, card_max, kCardBorder, 12.0f, 0, 2.0f);
    draw_list->PopClipRect();

    float heading_size = ImGui::GetFontSize() * 1.3f;
    ImVec2 heading_pos(window_pos.x + kSideMargin, window_pos.y + 18.0f);
    draw_list->AddText(nullptr, heading_size, heading_pos, kTextSelected, "DISPLAY OPTIONS");

    float accent_y = window_pos.y + 18.0f + heading_size + 6.0f;
    draw_list->AddLine(ImVec2(window_pos.x + kSideMargin, accent_y),
                       ImVec2(window_end.x - kSideMargin, accent_y), kAccentCyan, 2.0f);

    static const char* kRowLabels[kRowCount] = {
        "Display Mode",
        "Resolution",
        "Show FPS",
        "Online Aim Delay",
        "Done",
    };
    static const char* kResolutionValues[4] = {
        "1280 x 720",
        "1920 x 1080",
        "2560 x 1440",
        "3840 x 2160",
    };

    float row_y = window_pos.y + kTopContent;
    for (int row = 0; row < kRows; ++row) {
      ImVec2 row_pos(window_pos.x + kSideMargin, row_y);
      ImVec2 row_size(window_size.x - kSideMargin * 2.0f, kRowHeight);
      bool selected = (row == selected_row_);

      const char* value = nullptr;
      bool has_value = true;
      switch (row) {
        case kRowFullscreen:
          value = PerfectDarkApp::GetPcFullscreen() ? "Fullscreen" : "Windowed";
          break;
        case kRowResolution:
          value = kResolutionValues[PerfectDarkApp::GetPcResolutionIndex()];
          break;
        case kRowShowFps:
          value = PerfectDarkApp::GetPcShowFps() ? "On" : "Off";
          break;
        case kRowFacingDelay: {
          static char delay_text[24];
          const int ms = PerfectDarkApp::GetPcFacingDelay();
          std::snprintf(delay_text, sizeof(delay_text),
                        ms == 200  ? "%d ms (original)"
                        : ms == 50 ? "%d ms (default)"
                                   : "%d ms",
                        ms);
          value = delay_text;
          break;
        }
        default:
          has_value = false;
          value = "";
          break;
      }

      bool is_action_row = (row == kRowDone);
      if (is_action_row) {
        ImVec2 row_max(row_pos.x + row_size.x, row_pos.y + row_size.y);
        ImU32 top = selected ? kRowSelectedTop : IM_COL32(0x2E, 0x6F, 0xD8, 255);
        ImU32 bottom = selected ? kRowSelectedBottom : IM_COL32(0x1E, 0x49, 0x9A, 255);
        draw_list->AddRectFilledMultiColor(row_pos, row_max, top, top, bottom, bottom);
        draw_list->AddRect(row_pos, row_max, selected ? kRowSelectedBorder : kCardBorder, 6.0f, 0,
                           selected ? 2.0f : 1.5f);
        float font_size = ImGui::GetFontSize();
        ImVec2 text_size = ImGui::CalcTextSize(kRowLabels[row]);
        ImVec2 text_pos(row_pos.x + (row_size.x - text_size.x) * 0.5f,
                        row_pos.y + (row_size.y - font_size) * 0.5f);
        draw_list->AddText(nullptr, font_size, text_pos, kTextSelected, kRowLabels[row]);
      } else {
        DrawRow(draw_list, row_pos, row_size, row, selected, kRowLabels[row], value, has_value);
      }

      // Mouse fallback: Done is a single full-width activation button; every
      // other row only changes value via the drawn left/right chevrons
      // (matching Left/Right being the only way to change a value), and
      // clicking anywhere else on the row just selects it.
      ImGui::PushID(row);
      if (is_action_row) {
        ImGui::SetCursorScreenPos(row_pos);
        if (ImGui::InvisibleButton("row", row_size)) {
          ActivateRow(row);
        }
      } else if (has_value) {
        float row_right = row_pos.x + row_size.x;
        float chevron_left_x = row_right - row_size.x * 0.42f;
        float chevron_right_x = row_right - 34.0f;
        constexpr float kChevronHitHalfWidth = 20.0f;
        float band1_end = chevron_left_x - kChevronHitHalfWidth;
        float band2_end = chevron_left_x + kChevronHitHalfWidth;
        float band3_end = chevron_right_x - kChevronHitHalfWidth;
        float band4_end = chevron_right_x + kChevronHitHalfWidth;

        auto place_band = [&](const char* id, float x0, float x1b) {
          if (x1b <= x0) return false;
          ImGui::SetCursorScreenPos(ImVec2(x0, row_pos.y));
          return ImGui::InvisibleButton(id, ImVec2(x1b - x0, row_size.y));
        };

        if (place_band("b1", row_pos.x, band1_end)) selected_row_ = row;
        if (place_band("b2", band1_end, band2_end)) {
          selected_row_ = row;
          CycleRow(row, -1);
        }
        if (place_band("b3", band2_end, band3_end)) selected_row_ = row;
        if (place_band("b4", band3_end, band4_end)) {
          selected_row_ = row;
          CycleRow(row, 1);
        }
        if (place_band("b5", band4_end, row_right)) selected_row_ = row;
      } else {
        ImGui::SetCursorScreenPos(row_pos);
        if (ImGui::InvisibleButton("row", row_size)) {
          selected_row_ = row;
        }
      }
      ImGui::PopID();

      row_y += kRowHeight + kRowSpacing;
    }

    ImGui::EndPopup();
  }

  ImGui::PopStyleColor(1);
  ImGui::PopStyleVar(3);

  if (!still_open) {
    Close();
  }
}

inline void PdNameEntryDialog::OnDraw(ImGuiIO& io) {
  using namespace pd_ui;
  if (!has_opened_shown_) {
    ImGui::OpenPopup("##PdPlayerName");
    has_opened_shown_ = true;
  }

  // Typed characters come from ImGui's own queue (the window feeds it through
  // ImGuiDrawer::OnKeyChar); Enter, Escape and Backspace are read as edges the
  // same way the F9 panel reads its keys.
  const bool focused = app_ && app_->HasWindowFocus();
  bool now_down[3] = {};
  if (focused) {
    const int vks[3] = {VK_RETURN, VK_ESCAPE, VK_BACK};
    for (int i = 0; i < 3; ++i) now_down[i] = (GetAsyncKeyState(vks[i]) & 0x8000) != 0;
  }
  bool confirm = false, cancel = false, erase = false;
  if (!input_primed_) {
    for (int i = 0; i < 3; ++i) key_was_down_[i] = now_down[i];
    input_primed_ = true;
  } else {
    confirm = now_down[0] && !key_was_down_[0];
    cancel = now_down[1] && !key_was_down_[1];
    erase = now_down[2] && !key_was_down_[2];
    // Held backspace repeats, like any text box.
    const int64_t now_ms = PerfectDarkApp::NowMs();
    if (now_down[2] && !erase) {
      if (!repeat_at_ms_) repeat_at_ms_ = now_ms + 400;
      if (now_ms >= repeat_at_ms_) {
        erase = true;
        repeat_at_ms_ = now_ms + 45;
      }
    } else if (!now_down[2]) {
      repeat_at_ms_ = 0;
    }
    for (int i = 0; i < 3; ++i) key_was_down_[i] = now_down[i];
  }
  // The E that picked "Enter host address..." / "Change name..." arrives as a
  // typed character a frame or two later; ignore characters briefly after
  // opening so it doesn't land in the box.
  const int64_t now_ms = PerfectDarkApp::NowMs();
  if (!opened_at_ms_) opened_at_ms_ = now_ms;
  if (focused && now_ms - opened_at_ms_ >= 250) {
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
      const ImWchar c = io.InputQueueCharacters[i];
      if (c >= 0x20 && c < 0x7f && text_.size() < (kind_ == 1 ? kMaxAddress : kMaxName)) {
        text_.push_back(static_cast<char>(c));
      }
    }
  }
  if (erase && !text_.empty()) text_.pop_back();

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  const ImVec2 center = viewport ? ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                          viewport->WorkPos.y + viewport->WorkSize.y * 0.5f)
                                 : ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  constexpr float kWindowWidth = 460.0f;
  ImGui::SetNextWindowSize(ImVec2(kWindowWidth, 0), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));

  const bool still_open =
      ImGui::BeginPopupModal("##PdPlayerName", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  if (still_open) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 window_pos = ImGui::GetWindowPos();
    constexpr float kSideMargin = 22.0f;
    const ImVec2 window_size(kWindowWidth, 158.0f);
    const ImVec2 window_end(window_pos.x + window_size.x, window_pos.y + window_size.y);
    ImGui::Dummy(window_size);

    draw_list->PushClipRect(window_pos, window_end, true);
    const ImVec2 card_min(window_pos.x + 8, window_pos.y + 6);
    const ImVec2 card_max(window_end.x - 8, window_end.y - 6);
    draw_list->AddRectFilledMultiColor(card_min, card_max, kCardTop, kCardTop, kCardBottom,
                                       kCardBottom);
    draw_list->AddRect(card_min, card_max, kCardBorder, 12.0f, 0, 2.0f);
    draw_list->PopClipRect();

    const float heading_size = ImGui::GetFontSize() * 1.3f;
    draw_list->AddText(nullptr, heading_size, ImVec2(window_pos.x + kSideMargin, window_pos.y + 18),
                       kTextSelected, kind_ == 1 ? "DIRECT CONNECT - HOST ADDRESS" : "PLAYER NAME");
    const float accent_y = window_pos.y + 18.0f + heading_size + 6.0f;
    draw_list->AddLine(ImVec2(window_pos.x + kSideMargin, accent_y),
                       ImVec2(window_end.x - kSideMargin, accent_y), kAccentCyan, 2.0f);

    // The box, with a caret that blinks so an empty name still looks editable.
    const ImVec2 box_min(window_pos.x + kSideMargin, accent_y + 18.0f);
    const ImVec2 box_max(window_end.x - kSideMargin, box_min.y + 46.0f);
    draw_list->AddRectFilledMultiColor(box_min, box_max, kRowSelectedTop, kRowSelectedTop,
                                       kRowSelectedBottom, kRowSelectedBottom);
    draw_list->AddRect(box_min, box_max, kRowSelectedBorder, 6.0f, 0, 2.0f);
    const float text_size = ImGui::GetFontSize() * 1.15f;
    const ImVec2 text_pos(box_min.x + 14.0f, box_min.y + 12.0f);
    draw_list->AddText(nullptr, text_size, text_pos, kTextSelected, text_.c_str());
    const float text_width =
        ImGui::GetFont()->CalcTextSizeA(text_size, FLT_MAX, 0.0f, text_.c_str()).x;
    if ((PerfectDarkApp::NowMs() / 500) % 2 == 0) {
      const ImVec2 caret(text_pos.x + text_width + 2.0f, text_pos.y);
      draw_list->AddLine(caret, ImVec2(caret.x, caret.y + text_size), kAccentCyan, 2.0f);
    }

    draw_list->AddText(nullptr, ImGui::GetFontSize() * 0.95f,
                       ImVec2(window_pos.x + kSideMargin, box_max.y + 14.0f), kTextDim,
                       kind_ == 1 ? "Enter connects    Esc cancels" : "Enter saves    Esc cancels");

    if (confirm || cancel) {
      saved_ = confirm && !text_.empty();
      ImGui::CloseCurrentPopup();
      Close();
    }
    ImGui::EndPopup();
  } else {
    Close();
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}

inline void PdNameEntryDialog::OnClose() {
  PerfectDarkApp::OnNameEntryClosed(saved_, text_, kind_);
}

inline void PerfectDarkSettingsDialog::OnClose() {
  if (app_) {
    app_->OnSettingsDialogClosed();
  }
}

inline PerfectDarkMatchPicker::PerfectDarkMatchPicker(rex::ui::ImGuiDrawer* imgui_drawer,
                                                      PerfectDarkApp* app)
    : rex::ui::ImGuiDialog(imgui_drawer), app_(app) {
  rex::kernel::xam::netplay::PendingMatchChoice(&matches_);
}

// Arrow keys plus the default menu keys (W/S move, E select, 1 back), and a
// controller's D-pad, A and B. The first poll only records what is held.
inline void PerfectDarkMatchPicker::PollInput(bool edges[kKeyCount], bool* any_held) {
  static const int kKeys[kKeyCount][3] = {
      {VK_UP, 'W', 0},
      {VK_DOWN, 'S', 0},
      {VK_RETURN, 'E', VK_SPACE},
      {VK_ESCAPE, VK_BACK, '1'},
  };
  static const WORD kPad[kKeyCount] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
                                       XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B};
  const bool focused = app_ && app_->HasWindowFocus();
  XINPUT_STATE pad{};
  const bool pad_ok = focused && XInputGetState(0, &pad) == ERROR_SUCCESS;
  *any_held = false;
  for (int k = 0; k < kKeyCount; ++k) {
    bool down = false;
    for (int vk : kKeys[k]) {
      if (focused && vk && (GetAsyncKeyState(vk) & 0x8000)) down = true;
    }
    if (pad_ok && (pad.Gamepad.wButtons & kPad[k])) down = true;
    edges[k] = primed_ && down && !was_down_[k];
    was_down_[k] = down;
    *any_held = *any_held || down;
  }
  primed_ = true;
}

inline void PerfectDarkMatchPicker::OnDraw(ImGuiIO& io) {
  using namespace pd_ui;
  if (!opened_) {
    ImGui::OpenPopup("##PerfectDarkMatchPicker");
    opened_ = true;
  }

  const int rows = int(matches_.size()) + 1;  // the matches, then Cancel
  bool edges[kKeyCount] = {};
  bool any_held = false;
  PollInput(edges, &any_held);
  if (chosen_ < 0) {
    if (edges[kUp]) selected_ = (selected_ + rows - 1) % rows;
    if (edges[kDown]) selected_ = (selected_ + 1) % rows;
    if (edges[kConfirm]) chosen_ = selected_;
    if (edges[kBack]) chosen_ = rows - 1;
  }
  const bool finish = chosen_ >= 0 && !any_held;

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 viewport_center =
      viewport ? ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f)
               : ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(viewport_center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  // Sized to the window like the notice banner, so the text reads at the
  // same size as the game's own menus.
  const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
  ImFont* font = ImGui::GetFont();
  const float font_size = 20.0f * scale;
  const float heading_size = 26.0f * scale;
  auto text_width = [&](const char* text) {
    return font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text).x;
  };
  const float window_width = 600.0f * scale;
  ImGui::SetNextWindowSize(ImVec2(window_width, 0), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));

  const bool still_open =
      ImGui::BeginPopupModal("##PerfectDarkMatchPicker", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  if (still_open) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 window_pos = ImGui::GetWindowPos();

    const float kRowHeight = 50.0f * scale;
    const float kRowSpacing = 10.0f * scale;
    const float kSideMargin = 26.0f * scale;
    const float heading_y = window_pos.y + 20.0f * scale;
    const float accent_y = heading_y + heading_size + 8.0f * scale;
    const float subtitle_y = accent_y + 12.0f * scale;
    const float rows_top = subtitle_y + font_size + 22.0f * scale;
    const float kBottomMargin = 24.0f * scale;
    const float content_height = (rows_top - window_pos.y) + rows * kRowHeight +
                                 (rows - 1) * kRowSpacing + kBottomMargin;
    ImVec2 window_size(window_width, content_height);
    ImVec2 window_end(window_pos.x + window_size.x, window_pos.y + window_size.y);
    ImGui::Dummy(window_size);

    draw_list->PushClipRect(window_pos, window_end, true);
    ImVec2 card_min(window_pos.x + 8 * scale, window_pos.y + 6 * scale);
    ImVec2 card_max(window_end.x - 8 * scale, window_end.y - 6 * scale);
    draw_list->AddRectFilledMultiColor(card_min, card_max, kCardTop, kCardTop, kCardBottom,
                                       kCardBottom);
    draw_list->AddRect(card_min, card_max, kCardBorder, 12.0f * scale, 0, 2.0f * scale);
    draw_list->PopClipRect();

    draw_list->AddText(font, heading_size, ImVec2(window_pos.x + kSideMargin, heading_y),
                       kTextSelected, "JOIN AN ONLINE MATCH");
    draw_list->AddLine(ImVec2(window_pos.x + kSideMargin, accent_y),
                       ImVec2(window_end.x - kSideMargin, accent_y), kAccentCyan, 2.0f * scale);
    draw_list->AddText(font, font_size, ImVec2(window_pos.x + kSideMargin, subtitle_y), kTextDim,
                       "More than one friend is hosting. Whose match?");

    float row_y = rows_top;
    for (int row = 0; row < rows; ++row) {
      ImVec2 row_pos(window_pos.x + kSideMargin, row_y);
      ImVec2 row_size(window_size.x - kSideMargin * 2.0f, kRowHeight);
      ImVec2 row_max(row_pos.x + row_size.x, row_pos.y + row_size.y);
      const bool selected = row == selected_;
      if (selected) {
        draw_list->AddRectFilledMultiColor(row_pos, row_max, kRowSelectedTop, kRowSelectedTop,
                                           kRowSelectedBottom, kRowSelectedBottom);
        draw_list->AddRect(row_pos, row_max, kRowSelectedBorder, 6.0f * scale, 0, 2.0f * scale);
      } else {
        draw_list->AddRectFilled(row_pos, row_max, kRowIdle, 6.0f * scale);
        draw_list->AddRect(row_pos, row_max, kRowIdleBorder, 6.0f * scale, 0, 1.5f * scale);
      }
      const float text_y = row_pos.y + (row_size.y - font_size) * 0.5f;
      const float inset = 22.0f * scale;
      if (row == rows - 1) {
        const char* label = "Cancel - stay in my lobby";
        draw_list->AddText(font, font_size,
                           ImVec2(row_pos.x + (row_size.x - text_width(label)) * 0.5f, text_y),
                           selected ? kTextSelected : kTextNormal, label);
      } else {
        const auto& match = matches_[size_t(row)];
        const std::string label = match.host_name + "'s match";
        draw_list->AddText(font, font_size, ImVec2(row_pos.x + inset, text_y),
                           selected ? kTextSelected : kTextNormal, label.c_str());
        const char* address = match.address.c_str();
        draw_list->AddText(font, font_size,
                           ImVec2(row_max.x - inset - text_width(address), text_y),
                           selected ? kTextSelected : kTextDim, address);
      }
      ImGui::PushID(row);
      ImGui::SetCursorScreenPos(row_pos);
      if (ImGui::InvisibleButton("row", row_size) && chosen_ < 0) {
        selected_ = row;
        chosen_ = row;
      }
      ImGui::PopID();
      row_y += kRowHeight + kRowSpacing;
    }
    if (finish) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::PopStyleColor(1);
  ImGui::PopStyleVar(3);

  if (finish || !still_open) {
    // Cancel's row index is one past the list, which ChooseMatch treats as
    // cancelling.
    rex::kernel::xam::netplay::ChooseMatch(finish ? size_t(chosen_) : SIZE_MAX);
    Close();
  }
}

inline void PerfectDarkMatchPicker::OnClose() {
  if (app_) {
    app_->OnMatchPickerClosed();
  }
}
