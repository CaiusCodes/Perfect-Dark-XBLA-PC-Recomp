// Where the game keeps its data. In a release the game program lives in
// <release>\resources\runtime and its data beside that folder: resources\assets
// (imported by "Perfect Dark.exe" in the release root) and the saves folder,
// which also holds perfect_dark.toml. Dev builds keep everything beside the exe.
//
// The saves folder is resources\userdata unless the player chose another one in
// setup ("Keep saves and settings in another folder"). Then
// resources\userdata-location.txt holds that folder (one line, UTF-8); the
// launcher writes it, and an update never touches it (only the program files
// are replaced).
#pragma once

#include <windows.h>

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace pd_paths {

inline std::filesystem::path ExeFolder() {
  static wchar_t exe_path[32768];
  const DWORD length = GetModuleFileNameW(nullptr, exe_path, 32768);
  return std::filesystem::path(std::wstring(exe_path, length)).parent_path();
}

// True in a release (the exe in resources\runtime).
inline bool InRuntimeFolder() {
  std::wstring name = ExeFolder().filename().wstring();
  for (wchar_t& c : name) c = static_cast<wchar_t>(towlower(c));
  return name == L"runtime";
}

// resources\ in a release, the exe's folder otherwise.
inline std::filesystem::path DataRoot() {
  const auto exe = ExeFolder();
  return InRuntimeFolder() ? exe.parent_path() : exe;
}

// The folder chosen in setup; empty when saves are in resources\userdata.
inline std::filesystem::path CustomUserDataRoot() {
  if (!InRuntimeFolder()) return {};
  std::ifstream in(DataRoot() / "userdata-location.txt", std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line)) return {};
  if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // BOM
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  if (line.empty()) return {};
  return std::filesystem::path(std::u8string(line.begin(), line.end()));
}

// Saves, settings (perfect_dark.toml), screenshots and caches.
inline std::filesystem::path UserDataRoot() {
  const auto custom = CustomUserDataRoot();
  return custom.empty() ? DataRoot() / "userdata" : custom;
}

}  // namespace pd_paths
