// Perfect Dark XBLA PC Recomp - launcher entry point.
//
//   Perfect Dark.exe                  start the game; shows setup first when the
//                                     game data has not been imported (or a
//                                     needed file is missing)
//   Perfect Dark.exe --setup          open the setup window
//
// For automated tests (no window):
//   --setup --unattended [--package <file or folder>] [--extras fullscreen,shortcut,cheats,story,multiplayer]
//           [--shortcut-folder <dir>]
//   Exit code 0 imported, 1 failed, 2 several packages found (pass --package), 3 none found.
//
// Any other arguments are passed on to the game.
//
// A GUI-subsystem program, so starting it never flashes a console.
#include "launcher.h"

#include <objbase.h>
#include <shellapi.h>

#define PD_WIDEN2(x) L##x
#define PD_WIDEN(x) PD_WIDEN2(x)
#define PD_STRINGIZE2(x) #x
#define PD_STRINGIZE(x) PD_STRINGIZE2(x)

namespace pd {
const wchar_t* const kVersion = PD_WIDEN(PD_STRINGIZE(PD_LAUNCHER_VERSION));
}

namespace {

using namespace pd;

int RunUnattended(const Paths& p, const SetupOptions& options, const std::wstring& extras) {
  Log(L"Unattended setup");
  std::vector<Source> found;
  std::wstring why;
  if (!options.package.empty()) {
    found = SourcesIn(options.package, &why);
  } else {
    found = DetectSources(p);
    why = L"no package found";
  }
  if (found.size() != 1) {
    Log(L"Unattended: " + (found.empty() ? why : std::to_wstring(found.size()) + L" candidates, pass --package"));
    return found.empty() ? 3 : 2;
  }
  std::atomic<bool> cancel{false};
  const fs::path saves_from = found[0].kind == SourceKind::PreviousInstall ? found[0].path : FindPreviousInstall(p);
  ImportResult result = Import(p, found[0], saves_from, cancel, nullptr);
  if (!result.ok) return 1;
  Extras want;
  want.fullscreen = extras.find(L"fullscreen") != std::wstring::npos;
  want.shortcut = extras.find(L"shortcut") != std::wstring::npos;
  want.cheats = extras.find(L"cheats") != std::wstring::npos;
  want.story = extras.find(L"story") != std::wstring::npos;
  want.multiplayer = extras.find(L"multiplayer") != std::wstring::npos;
  // As on a first setup: unticked extras change nothing; never replace a shortcut that is not ours.
  const std::wstring note = ApplyExtras(p, want, Extras{}, options.shortcut_folder, nullptr);
  if (!note.empty()) Log(L"Unattended: " + note);
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool setup = false, unattended = false;
  std::wstring extras;
  SetupOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--setup") {
      setup = true;
    } else if (arg == L"--unattended") {
      unattended = true;
    } else if (arg == L"--package" && i + 1 < argc) {
      options.package = argv[++i];
    } else if (arg == L"--extras" && i + 1 < argc) {
      extras = argv[++i];
    } else if (arg == L"--shortcut-folder" && i + 1 < argc) {
      options.shortcut_folder = argv[++i];
    } else {
      options.game_args += (options.game_args.empty() ? L"" : L" ") + Quote(arg);
    }
  }
  if (argv) LocalFree(argv);

  const Paths p = MakePaths();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  // The normal case: everything is in place, so start the game at once. A
  // copy left over from an import that was cut off while swapping folders is
  // put back, or cleared away, first (a cheap check: one folder lookup).
  std::wstring missing;
  RecoverInterruptedSwap(p);
  const bool installed = IsInstalled(p, &missing);
  if (installed && !setup && options.package.empty()) {
    if (GetFileAttributesW(p.config.c_str()) == INVALID_FILE_ATTRIBUTES) {
      LogTo(p.logs);
      EnsureSettings(p);
    }
    RepairShortcut(p, options.shortcut_folder);
    std::wstring error;
    int code = 0;
    if (!LaunchGame(p, options.game_args, &error)) {
      LogTo(p.logs);
      Log(L"FAILED to start the game: " + error);
      MessageBoxW(nullptr, error.c_str(), L"Perfect Dark", MB_OK | MB_ICONERROR);
      code = 1;
    }
    CoUninitialize();
    return code;
  }

  // Setup. One window per folder: a second launch brings the first to the front.
  std::wstring mutex_name = L"Local\\PerfectDarkSetup-";
  for (wchar_t c : p.root.wstring()) mutex_name += (c == L'\\' || c == L':') ? L'_' : static_cast<wchar_t>(towlower(c));
  if (mutex_name.size() > 250) mutex_name.resize(250);
  HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(L"PerfectDarkSetup", nullptr)) {
      ShowWindow(other, SW_RESTORE);
      SetForegroundWindow(other);
    }
    CloseHandle(mutex);
    CoUninitialize();
    return 0;
  }

  LogTo(p.logs);
  options.forced = installed;
  options.reason = installed ? L"--setup" : L"missing " + missing;
  const int code = unattended ? RunUnattended(p, options, extras) : RunSetupWindow(instance, p, options);
  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  CoUninitialize();
  return code;
}
