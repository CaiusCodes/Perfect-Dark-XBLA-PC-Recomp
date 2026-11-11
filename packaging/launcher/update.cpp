// Perfect Dark XBLA PC Recomp - the update window (Perfect Dark.exe --update).
//
// Opened by the game's Help & Options > Check for Updates once the player
// chose to update (the game closes itself first). It asks GitHub for the
// latest release (src/pd_update.h), downloads its ZIP into resources\.update,
// checks the SHA-256 GitHub gives for it, unpacks it and replaces the program
// files: Perfect Dark.exe, README.txt, licenses\ and resources\runtime\ (not
// runtime\logs). resources\assets and resources\userdata (game data, saves,
// settings, shader caches) are never touched, nor is a saves folder chosen in
// setup or resources\userdata-location.txt, which names it.
//
// The replacement is two steps per file, so a failure can be undone: every new
// file is first written beside its target as <name>.update-new; then each
// target becomes <name>.update-old and the new file takes its name. Any error
// in the second step renames everything back. The old files are deleted
// afterwards; one still in use (this launcher's own exe) is deleted at the next
// start, where an update cut off half-way is also put back
// (RecoverInterruptedUpdate).
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus

#include "launcher.h"

#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <memory>
#include <mutex>
#include <thread>

#include "../../src/pd_update.h"
#include "resource.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
// stb_image (public domain / MIT, licenses\LICENSE-stb.txt): only its zlib
// inflate is used, for the release ZIP.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma clang diagnostic pop

namespace pd {
namespace {

const wchar_t kNewSuffix[] = L".update-new";
const wchar_t kOldSuffix[] = L".update-old";

// ---------------------------------------------------------------- ZIP --
uint32_t Crc32(const unsigned char* data, size_t size) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint16_t Le16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const unsigned char* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16) | uint32_t(p[3]) << 24; }

bool ReadFileAll(const fs::path& path, std::vector<unsigned char>& out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (1ll << 31);
  if (ok) {
    out.resize(size_t(size.QuadPart));
    DWORD got = 0;
    ok = ReadFile(file, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
  }
  CloseHandle(file);
  return ok;
}

bool WriteFileAll(const fs::path& path, const unsigned char* data, size_t size) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, data, DWORD(size), &written, nullptr) && written == size;
  CloseHandle(file);
  return ok;
}

// Unpacks a ZIP (stored or deflated entries) under `dest`. Names that would
// leave `dest` are refused.
bool ExtractZip(const fs::path& zip, const fs::path& dest, std::wstring* error) {
  std::vector<unsigned char> data;
  if (!ReadFileAll(zip, data) || data.size() < 22) return *error = L"the download could not be read", false;
  size_t eocd = std::string::npos;
  for (size_t i = data.size() - 22; i + 1 > 0 && data.size() - i <= 65557; --i) {
    if (Le32(&data[i]) == 0x06054B50) { eocd = i; break; }
    if (i == 0) break;
  }
  if (eocd == std::string::npos) return *error = L"the download is not a ZIP", false;
  const uint16_t count = Le16(&data[eocd + 10]);
  size_t at = Le32(&data[eocd + 16]);
  for (uint16_t n = 0; n < count; ++n) {
    if (at + 46 > data.size() || Le32(&data[at]) != 0x02014B50) return *error = L"the ZIP is damaged", false;
    const uint16_t method = Le16(&data[at + 10]);
    const uint32_t crc = Le32(&data[at + 16]);
    const uint32_t packed = Le32(&data[at + 20]), unpacked = Le32(&data[at + 24]);
    const uint16_t name_len = Le16(&data[at + 28]), extra_len = Le16(&data[at + 30]), note_len = Le16(&data[at + 32]);
    const uint32_t local = Le32(&data[at + 42]);
    if (at + 46 + name_len > data.size()) return *error = L"the ZIP is damaged", false;
    std::string name(reinterpret_cast<const char*>(&data[at + 46]), name_len);
    at += 46 + name_len + extra_len + note_len;
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos || ("/" + name + "/").find("/../") != std::string::npos)
      return *error = L"the ZIP holds an unsafe name", false;
    const fs::path out = dest / fs::path(update::Widen(name)).make_preferred();
    if (name.back() == '/') {
      std::error_code ec;
      fs::create_directories(out, ec);
      continue;
    }
    if (local + 30 > data.size() || Le32(&data[local]) != 0x04034B50) return *error = L"the ZIP is damaged", false;
    const size_t start = local + 30 + Le16(&data[local + 26]) + Le16(&data[local + 28]);
    if (start + packed > data.size()) return *error = L"the ZIP is damaged", false;
    std::vector<unsigned char> file(unpacked);
    if (method == 0) {
      if (packed != unpacked) return *error = L"the ZIP is damaged", false;
      std::copy(data.begin() + start, data.begin() + start + packed, file.begin());
    } else if (method == 8) {
      const int got = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(file.data()), int(unpacked),
                                                       reinterpret_cast<const char*>(&data[start]), int(packed));
      if (got != int(unpacked)) return *error = L"the ZIP is damaged", false;
    } else {
      return *error = L"the ZIP uses an unknown compression", false;
    }
    if (Crc32(file.data(), file.size()) != crc) return *error = L"the ZIP is damaged (checksum)", false;
    if (!WriteFileAll(out, file.data(), file.size())) return *error = L"could not unpack " + out.filename().wstring(), false;
  }
  return true;
}

// ------------------------------------------------------------- install --
fs::path WithSuffix(const fs::path& path, const wchar_t* suffix) { return fs::path(path.wstring() + suffix); }

bool EndsWith(const std::wstring& s, const wchar_t* tail) {
  const size_t n = wcslen(tail);
  return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

// The program files of an unpacked release (relative paths): everything but
// resources\ other than resources\runtime, and never runtime\logs.
std::vector<fs::path> ProgramFiles(const fs::path& release) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(release, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const fs::path rel = fs::relative(it->path(), release, ec);
    auto part = rel.begin();
    if (part == rel.end()) continue;
    if (_wcsicmp(part->c_str(), L"resources") == 0) {
      ++part;
      if (part == rel.end() || _wcsicmp(part->c_str(), L"runtime") != 0) continue;
      ++part;
      if (part != rel.end() && _wcsicmp(part->c_str(), L"logs") == 0) continue;
    }
    files.push_back(rel);
  }
  return files;
}

bool Install(const Paths& p, const fs::path& release, std::wstring* error) {
  const std::vector<fs::path> files = ProgramFiles(release);
  if (files.empty()) return *error = L"the release is empty", false;
  // 1. Every new file beside its target.
  std::vector<fs::path> staged;
  auto unstage = [&] {
    std::error_code ec;
    for (const fs::path& f : staged) fs::remove(f, ec);
  };
  for (const fs::path& rel : files) {
    const fs::path target = p.root / rel;
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    const fs::path fresh = WithSuffix(target, kNewSuffix);
    if (!CopyFileW((release / rel).c_str(), fresh.c_str(), FALSE)) {
      unstage();
      return *error = L"could not write " + rel.wstring() + L" (is the folder read-only?)", false;
    }
    staged.push_back(fresh);
  }
  // 2. Swap them in; undo everything on a failure.
  struct Done { fs::path target; bool had_old; };
  std::vector<Done> done;
  for (const fs::path& rel : files) {
    const fs::path target = p.root / rel, old = WithSuffix(target, kOldSuffix), fresh = WithSuffix(target, kNewSuffix);
    const bool had_old = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
    bool ok = !had_old || MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (ok && !MoveFileExW(fresh.c_str(), target.c_str(), 0)) {
      if (had_old) MoveFileExW(old.c_str(), target.c_str(), 0);
      ok = false;
    }
    if (!ok) {
      for (auto it = done.rbegin(); it != done.rend(); ++it) {
        DeleteFileW(it->target.c_str());
        if (it->had_old) MoveFileExW(WithSuffix(it->target, kOldSuffix).c_str(), it->target.c_str(), 0);
      }
      unstage();
      return *error = L"could not replace " + rel.wstring() + L" (is a file in use?); nothing was changed", false;
    }
    done.push_back({target, had_old});
  }
  // 3. The old files go (this exe's own copy stays until the next start).
  for (const Done& d : done)
    if (d.had_old) DeleteFileW(WithSuffix(d.target, kOldSuffix).c_str());
  return true;
}

}  // namespace

void RecoverInterruptedUpdate(const Paths& p) {
  std::error_code ec;
  const fs::path work = p.resources / L".update";
  std::vector<fs::path> leftovers;
  auto scan = [&](const fs::path& folder, bool recursive) {
    std::error_code e;
    if (recursive) {
      for (fs::recursive_directory_iterator it(folder, e), end; it != end; it.increment(e)) {
        if (e) break;
        const std::wstring name = it->path().wstring();
        if (EndsWith(name, kOldSuffix) || EndsWith(name, kNewSuffix)) leftovers.push_back(it->path());
      }
    } else {
      for (fs::directory_iterator it(folder, e), end; it != end; it.increment(e)) {
        if (e) break;
        const std::wstring name = it->path().wstring();
        if (EndsWith(name, kOldSuffix) || EndsWith(name, kNewSuffix)) leftovers.push_back(it->path());
      }
    }
  };
  scan(p.root, false);
  scan(p.root / L"licenses", true);
  scan(p.runtime, true);
  const bool work_left = GetFileAttributesW(work.c_str()) != INVALID_FILE_ATTRIBUTES;
  if (leftovers.empty() && !work_left) return;
  LogTo(p.logs);
  for (const fs::path& f : leftovers) {
    const std::wstring name = f.wstring();
    if (EndsWith(name, kNewSuffix)) {
      DeleteFileW(f.c_str());
      continue;
    }
    const fs::path base = name.substr(0, name.size() - wcslen(kOldSuffix));
    if (GetFileAttributesW(base.c_str()) == INVALID_FILE_ATTRIBUTES) {
      if (MoveFileExW(f.c_str(), base.c_str(), 0)) Log(L"Update: put back " + base.filename().wstring() + L" (an update was cut off).");
    } else {
      DeleteFileW(f.c_str());
    }
  }
  if (work_left) fs::remove_all(work, ec);
}

// ============================================================= window ==
namespace {

using namespace Gdiplus;

constexpr float kW = 640, kH = 340;
constexpr float kUiScale = 1.1f;
enum Control { kNone = -1, kLeft, kPlay };
enum class Stage { Waiting, Checking, Downloading, Verifying, Unpacking, Installing, Updated, UpToDate, Failed, Cancelled };
constexpr UINT WM_APP_STATE = WM_APP + 1;
const wchar_t* const kClassName = L"PerfectDarkUpdate";
const wchar_t kDot[] = L"·";

Color Hex(uint32_t rgb, BYTE alpha = 255) { return Color(alpha, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }
namespace tone {
const uint32_t Background = 0x07090D, Band = 0x0F1D2C, CardTop = 0x0F1826, CardBottom = 0x15263A, Cyan = 0x3FD0E8,
               CyanDeep = 0x1E6E8A, CyanPress = 0x2AA9BE, CyanHover = 0x72E2F2, Red = 0xE0474F, Text = 0xE6EAF7,
               Soft = 0xAFB6D6, Muted = 0x7F88A8, Field = 0x0A0F18, Ok = 0x5BE3A8, Ink = 0x061018;
}

void RoundedPath(GraphicsPath& path, float x, float y, float w, float h, float r) {
  const float d = std::max(1.0f, r * 2);
  path.AddArc(x, y, d, d, 180, 90);
  path.AddArc(x + w - d, y, d, d, 270, 90);
  path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  path.AddArc(x, y + h - d, d, d, 90, 90);
  path.CloseFigure();
}

std::unique_ptr<Font> MakeFont(const wchar_t* family, float points, int style) {
  return std::make_unique<Font>(family, points * 96.0f / 72.0f, style, UnitPixel);
}

std::wstring Megabytes(uint64_t bytes) {
  wchar_t text[32];
  swprintf(text, 32, L"%.1f", bytes / 1048576.0);
  return text;
}

class UpdateWindow {
 public:
  explicit UpdateWindow(const Paths& p) : p_(p) {}
  int Run(HINSTANCE instance);

 private:
  struct Shared {  // written by the worker, read by the window
    std::mutex lock;
    Stage stage = Stage::Waiting;
    std::wstring version, detail, error;
    uint64_t done = 0, total = 0;
  };
  static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
  void ApplyScale(UINT dpi);
  void Paint(HDC dc);
  void PaintText(Graphics& g, const std::wstring& text, Font* font, uint32_t color, RectF rect,
                 StringAlignment align = StringAlignmentNear, bool wrap = false);
  void PaintButton(Graphics& g, Control id, const wchar_t* text, bool enabled, bool primary, Font* font);
  RectF ControlRect(Control id) const;
  bool Enabled(Control id) const;
  Control HitTest(float x, float y) const;
  void Click(Control id);
  void Work();
  void Set(Stage stage, const std::wstring& detail = L"");
  Stage CurrentStage();
  void Invalidate() { InvalidateRect(hwnd_, nullptr, FALSE); }

  const Paths& p_;
  HWND hwnd_ = nullptr;
  HICON icon_ = nullptr;
  float scale_ = 1;
  Shared shared_;
  std::atomic<bool> cancel_{false};
  std::thread worker_;
  Control hover_ = kNone, pressed_ = kNone;
  bool tracking_ = false;
  std::unique_ptr<Font> brand_, tag_, title_, caps_, small_, button_, play_;
};

void UpdateWindow::Set(Stage stage, const std::wstring& detail) {
  {
    std::lock_guard<std::mutex> guard(shared_.lock);
    shared_.stage = stage;
    shared_.detail = detail;
  }
  PostMessageW(hwnd_, WM_APP_STATE, 0, 0);
}

Stage UpdateWindow::CurrentStage() {
  std::lock_guard<std::mutex> guard(shared_.lock);
  return shared_.stage;
}

void UpdateWindow::Work() {
  auto fail = [this](const std::wstring& why) {
    Log(L"Update FAILED: " + why);
    {
      std::lock_guard<std::mutex> guard(shared_.lock);
      shared_.error = why;
    }
    Set(cancel_ ? Stage::Cancelled : Stage::Failed);
  };
  // The game closes itself before opening this; give it a moment.
  for (int i = 0; i < 100 && GameRunning(p_); ++i) {
    if (cancel_) return Set(Stage::Cancelled);
    Sleep(200);
  }
  if (GameRunning(p_)) return fail(L"Perfect Dark is still running from this folder. Close it, then try again.");
  Set(Stage::Checking);
  const std::wstring agent = std::wstring(L"PerfectDark-XBLA-PC-Recomp-Launcher/") + kVersion;
  update::Release release;
  std::string error;
  if (!update::LatestRelease(release, agent, error)) return fail(L"Could not check for updates: " + update::Widen(error) + L".");
  {
    std::lock_guard<std::mutex> guard(shared_.lock);
    shared_.version = update::Widen(release.version);
    shared_.total = release.zip_size;
  }
  Log(L"Update: latest release v" + update::Widen(release.version) + L", this copy v" + kVersion);
  if (update::CompareVersions(release.version, update::Narrow(kVersion)) <= 0) return Set(Stage::UpToDate);
  if (cancel_) return Set(Stage::Cancelled);

  const fs::path work = p_.resources / L".update";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  const fs::path zip = work / update::Widen(release.zip_name);
  Set(Stage::Downloading);
  HANDLE file = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return fail(L"Could not save the download in resources\\.update (is the folder read-only?).");
  DWORD last_post = 0;
  const bool got = update::HttpGet(update::Widen(release.zip_url), agent, nullptr, file,
                                   [this, &last_post](uint64_t done, uint64_t total) {
                                     {
                                       std::lock_guard<std::mutex> guard(shared_.lock);
                                       shared_.done = done;
                                       if (total) shared_.total = total;
                                     }
                                     if (GetTickCount() - last_post > 100) {
                                       last_post = GetTickCount();
                                       PostMessageW(hwnd_, WM_APP_STATE, 0, 0);
                                     }
                                     return !cancel_.load();
                                   },
                                   error);
  CloseHandle(file);
  if (!got) {
    fs::remove_all(work, ec);
    return fail(L"The download did not finish: " + update::Widen(error) + L". Nothing was changed.");
  }
  Set(Stage::Verifying);
  const std::string sha = update::FileSha256(zip.wstring());
  if (sha != release.sha256) {
    fs::remove_all(work, ec);
    Log(L"Update: SHA-256 " + update::Widen(sha) + L", expected " + update::Widen(release.sha256));
    return fail(L"The download did not match the release's checksum, so it was not used. Nothing was changed.");
  }
  Log(L"Update: downloaded " + zip.filename().wstring() + L", SHA-256 verified.");
  if (cancel_) {
    fs::remove_all(work, ec);
    return Set(Stage::Cancelled);
  }
  Set(Stage::Unpacking);
  std::wstring why;
  const fs::path unpacked = work / L"new";
  if (!ExtractZip(zip, unpacked, &why)) {
    fs::remove_all(work, ec);
    return fail(L"The download could not be unpacked: " + why + L". Nothing was changed.");
  }
  const fs::path release_root = unpacked / L"Perfect Dark";
  if (GetFileAttributesW((release_root / L"Perfect Dark.exe").c_str()) == INVALID_FILE_ATTRIBUTES ||
      GetFileAttributesW((release_root / L"resources" / L"runtime" / L"Perfect Dark.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
    fs::remove_all(work, ec);
    return fail(L"The download is not a Perfect Dark release. Nothing was changed.");
  }
  Set(Stage::Installing);
  if (!Install(p_, release_root, &why)) {
    fs::remove_all(work, ec);
    return fail(L"The update could not be installed: " + why + L".");
  }
  fs::remove_all(work, ec);
  Log(L"Update: installed v" + update::Widen(release.version) + L".");
  Set(Stage::Updated);
}

void UpdateWindow::ApplyScale(UINT dpi) {
  HMONITOR monitor = hwnd_ ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST)
                           : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info = {sizeof(info)};
  GetMonitorInfoW(monitor, &info);
  const RECT work = info.rcWork;
  const float fit = std::min((work.right - work.left) * 0.95f / kW, (work.bottom - work.top) * 0.95f / kH);
  scale_ = std::max(0.5f, std::min(dpi / 96.0f * kUiScale, fit));
  const int w = int(kW * scale_ + 0.5f), h = int(kH * scale_ + 0.5f);
  SetWindowPos(hwnd_, nullptr, work.left + ((work.right - work.left) - w) / 2, work.top + ((work.bottom - work.top) - h) / 2,
               w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  const int r = int(16 * scale_);
  SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2), TRUE);
  Invalidate();
}

RectF UpdateWindow::ControlRect(Control id) const {
  return id == kLeft ? RectF(330, 282, 104, 38) : RectF(446, 274, 154, 52);
}

bool UpdateWindow::Enabled(Control id) const {
  const Stage s = const_cast<UpdateWindow*>(this)->CurrentStage();
  const bool busy = s == Stage::Verifying || s == Stage::Unpacking || s == Stage::Installing;
  if (id == kLeft) return !busy && !(cancel_.load() && s <= Stage::Downloading);
  std::wstring missing;
  return (s == Stage::Updated || s == Stage::UpToDate || s == Stage::Failed || s == Stage::Cancelled) &&
         IsInstalled(p_, &missing);
}

Control UpdateWindow::HitTest(float x, float y) const {
  for (Control id : {kLeft, kPlay}) {
    const RectF r = ControlRect(id);
    if (x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height && Enabled(id)) return id;
  }
  return kNone;
}

void UpdateWindow::Click(Control id) {
  const Stage s = CurrentStage();
  if (id == kLeft) {
    if (s <= Stage::Downloading) {
      cancel_ = true;  // the worker stops and reports Cancelled
      Invalidate();
    } else {
      DestroyWindow(hwnd_);
    }
  } else if (id == kPlay) {
    std::wstring error;
    if (!LaunchGame(p_, L"", &error)) {
      MessageBoxW(hwnd_, error.c_str(), L"Perfect Dark", MB_OK | MB_ICONERROR);
      return;
    }
    Log(L"Update window: game started.");
    DestroyWindow(hwnd_);
  }
}

void UpdateWindow::PaintText(Graphics& g, const std::wstring& text, Font* font, uint32_t color, RectF rect,
                             StringAlignment align, bool wrap) {
  if (text.empty()) return;
  StringFormat format;
  format.SetAlignment(align);
  format.SetTrimming(wrap ? StringTrimmingWord : StringTrimmingEllipsisCharacter);
  if (!wrap) format.SetFormatFlags(StringFormatFlagsNoWrap);
  SolidBrush brush(Hex(color));
  g.DrawString(text.c_str(), -1, font, rect, &format, &brush);
}

void UpdateWindow::PaintButton(Graphics& g, Control id, const wchar_t* text, bool enabled, bool primary, Font* font) {
  const RectF r = ControlRect(id);
  uint32_t back, fore, border;
  if (!enabled) {
    back = 0x131E2A, fore = 0x4F6072, border = 0x1E2C3A;
  } else if (primary) {
    back = pressed_ == id ? tone::CyanPress : hover_ == id ? tone::CyanHover : tone::Cyan, fore = tone::Ink;
    border = hover_ == id || pressed_ == id ? tone::CyanHover : tone::Cyan;
  } else {
    back = pressed_ == id ? 0x0A121C : hover_ == id ? 0x15253A : 0x0D1724, fore = tone::Soft, border = 0x34465E;
  }
  GraphicsPath shape;
  RoundedPath(shape, r.X, r.Y, r.Width, r.Height, primary ? 8.0f : 6.0f);
  SolidBrush fill(Hex(back));
  g.FillPath(&fill, &shape);
  Pen pen(Hex(border), primary && enabled ? 1.6f : 1.2f);
  g.DrawPath(&pen, &shape);
  StringFormat center;
  center.SetAlignment(StringAlignmentCenter);
  center.SetLineAlignment(StringAlignmentCenter);
  SolidBrush brush(Hex(fore));
  g.DrawString(text, -1, font, RectF(r.X, r.Y + 1, r.Width, r.Height), &center, &brush);
}

void UpdateWindow::Paint(HDC dc) {
  Shared s;
  {
    std::lock_guard<std::mutex> guard(shared_.lock);
    s.stage = shared_.stage, s.version = shared_.version, s.detail = shared_.detail, s.error = shared_.error;
    s.done = shared_.done, s.total = shared_.total;
  }
  RECT client;
  GetClientRect(hwnd_, &client);
  HDC memory = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
  HGDIOBJ old = SelectObject(memory, bitmap);
  {
    Graphics g(memory);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.ScaleTransform(scale_, scale_);
    SolidBrush field(Hex(tone::Background));
    g.FillRectangle(&field, RectF(-1, -1, kW + 2, kH + 2));
    LinearGradientBrush band(RectF(0, 0, kW, 110), Hex(tone::Band), Hex(tone::Background), 90.0f);
    g.FillRectangle(&band, RectF(0, 0, kW, 110));
    Pen scan(Hex(tone::Cyan, 30), 1);
    for (int i = 0; i < 13; ++i) g.DrawLine(&scan, 404.0f + i * 18.0f, 0.0f, 476.0f + i * 18.0f, 96.0f);
    PaintText(g, L"PERFECT DARK", brand_.get(), tone::Text, RectF(34, 18, 440, 52));
    PaintText(g, std::wstring(L"XBLA PC PORT  ") + kDot + L"  UPDATE  " + kDot + L"  V" + kVersion, tag_.get(),
              tone::Cyan, RectF(40, 68, 440, 20));

    const std::wstring v = s.version.empty() ? L"" : L"v" + s.version;
    std::wstring title, phase, stats, detail;
    uint32_t phase_color = tone::Cyan;
    float progress = -1;
    switch (s.stage) {
      case Stage::Waiting: title = L"Updating Perfect Dark"; phase = L"WAITING FOR THE GAME TO CLOSE"; break;
      case Stage::Checking: title = L"Updating Perfect Dark"; phase = L"CHECKING GITHUB FOR THE LATEST RELEASE"; break;
      case Stage::Downloading:
        title = L"Updating to " + v;
        phase = L"DOWNLOADING";
        stats = Megabytes(s.done) + (s.total ? L" / " + Megabytes(s.total) : L"") + L" MB";
        progress = s.total ? float(double(s.done) / double(s.total)) : 0.0f;
        break;
      case Stage::Verifying: title = L"Updating to " + v; phase = L"CHECKING THE DOWNLOAD"; progress = 1; break;
      case Stage::Unpacking: title = L"Updating to " + v; phase = L"UNPACKING"; progress = 1; break;
      case Stage::Installing: title = L"Updating to " + v; phase = L"INSTALLING"; progress = 1; break;
      case Stage::Updated:
        title = L"Updated to " + v;
        phase = L"READY TO PLAY";
        phase_color = tone::Ok;
        detail = L"Press Play to start the new version.";
        break;
      case Stage::UpToDate:
        title = L"Perfect Dark is up to date";
        phase = L"NO NEWER VERSION";
        phase_color = tone::Ok;
        detail = L"You have the latest version (v" + std::wstring(kVersion) + L").";
        break;
      case Stage::Failed:
        title = L"The update did not complete";
        phase = L"UPDATE FAILED";
        phase_color = tone::Red;
        detail = s.error + L" Details: resources\\runtime\\logs\\setup.log";
        break;
      case Stage::Cancelled:
        title = L"Update cancelled";
        phase = L"NOTHING WAS CHANGED";
        phase_color = tone::Muted;
        break;
    }
    PaintText(g, title, title_.get(), tone::Text, RectF(38, 104, 560, 32));
    GraphicsPath card;
    RoundedPath(card, 40, 146, 560, 112, 14);
    LinearGradientBrush fill(RectF(40, 146, 560, 112), Hex(tone::CardTop), Hex(tone::CardBottom), 90.0f);
    g.FillPath(&fill, &card);
    Pen edge(Hex(tone::Cyan, 150), 1.2f);
    g.DrawPath(&edge, &card);
    PaintText(g, phase, caps_.get(), phase_color, RectF(60, 162, 360, 18));
    PaintText(g, stats, small_.get(), tone::Muted, RectF(380, 160, 200, 18), StringAlignmentFar);
    if (progress >= 0) {
      GraphicsPath track;
      RoundedPath(track, 60, 186, 520, 8, 4);
      SolidBrush track_fill(Hex(tone::Field));
      g.FillPath(&track_fill, &track);
      const float width = 520.0f * std::min(1.0f, progress);
      if (width > 2) {
        GraphicsPath bar;
        RoundedPath(bar, 60, 186, width, 8, std::min(4.0f, width / 2));
        LinearGradientBrush bar_fill(RectF(60, 186, 520, 8), Hex(tone::CyanDeep), Hex(tone::Cyan), 0.0f);
        g.FillPath(&bar_fill, &bar);
      }
    }
    if (detail.empty())
      detail = L"Only the program files change. Your saves, settings and game data are kept.";
    PaintText(g, detail, small_.get(), s.stage == Stage::Failed ? tone::Soft : tone::Muted, RectF(60, 204, 520, 48),
              StringAlignmentNear, true);

    const bool cancelable = s.stage <= Stage::Downloading;
    PaintButton(g, kLeft, cancelable ? (cancel_ ? L"Cancelling" : L"Cancel") : L"Close", Enabled(kLeft), false,
                button_.get());
    PaintButton(g, kPlay, L"PLAY", Enabled(kPlay), true, play_.get());
    GraphicsPath outline;
    RoundedPath(outline, 0.5f, 0.5f, kW - 1, kH - 1, 16);
    Pen outline_pen(Hex(tone::Cyan, 110), 1);
    g.DrawPath(&outline_pen, &outline);
  }
  BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
  SelectObject(memory, old);
  DeleteObject(bitmap);
  DeleteDC(memory);
}

LRESULT CALLBACK UpdateWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  UpdateWindow* self = reinterpret_cast<UpdateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    self = static_cast<UpdateWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT UpdateWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      Paint(dc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: ApplyScale(HIWORD(wp)); return 0;
    case WM_APP_STATE: {
      const Stage s = CurrentStage();
      if ((s == Stage::Updated || s == Stage::Failed) && GetForegroundWindow() != hwnd_) {
        FLASHWINFO flash = {sizeof(flash), hwnd_, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&flash);
      }
      Invalidate();
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!tracking_) {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
        tracking_ = TrackMouseEvent(&track) != FALSE;
      }
      const Control hit = HitTest(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
      if (hit != hover_) hover_ = hit, Invalidate();
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_ = false;
      if (hover_ != kNone) hover_ = kNone, Invalidate();
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, hover_ != kNone ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_LBUTTONDOWN: {
      const Control hit = HitTest(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
      if (hit == kNone) {
        ReleaseCapture();
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
      }
      pressed_ = hit;
      SetCapture(hwnd_);
      Invalidate();
      return 0;
    }
    case WM_LBUTTONUP: {
      const Control was = pressed_;
      pressed_ = kNone;
      ReleaseCapture();
      const Control hit = HitTest(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
      if (was != kNone && was == hit) Click(hit);
      Invalidate();
      return 0;
    }
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE && Enabled(kLeft)) Click(kLeft);
      if (wp == VK_RETURN && Enabled(kPlay)) Click(kPlay);
      return 0;
    case WM_CLOSE:
      if (Enabled(kLeft)) {
        if (CurrentStage() <= Stage::Downloading) cancel_ = true;
        else DestroyWindow(hwnd_);
      }
      return 0;
    case WM_DESTROY:
      cancel_ = true;
      if (worker_.joinable()) worker_.join();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

int UpdateWindow::Run(HINSTANCE instance) {
  ULONG_PTR token = 0;
  GdiplusStartupInput input;
  if (GdiplusStartup(&token, &input, nullptr) != Ok) return 1;
  brand_ = MakeFont(L"Segoe UI Black", 24, FontStyleRegular);
  tag_ = MakeFont(L"Segoe UI Semibold", 8.5f, FontStyleRegular);
  title_ = MakeFont(L"Segoe UI Semibold", 14, FontStyleRegular);
  caps_ = MakeFont(L"Segoe UI Semibold", 8.25f, FontStyleRegular);
  small_ = MakeFont(L"Segoe UI", 9, FontStyleRegular);
  button_ = MakeFont(L"Segoe UI Semibold", 10, FontStyleRegular);
  play_ = MakeFont(L"Segoe UI Black", 15, FontStyleRegular);
  icon_ = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = Proc;
  wc.hInstance = instance;
  wc.hIcon = icon_;
  wc.hIconSm = icon_;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);
  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"Perfect Dark Update", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                          0, 0, 100, 100, nullptr, nullptr, instance, this);
  if (!hwnd_) return 1;
  ApplyScale(GetDpiForWindow(hwnd_));
  ShowWindow(hwnd_, SW_SHOWNORMAL);
  SetForegroundWindow(hwnd_);
  ::UpdateWindow(hwnd_);
  worker_ = std::thread([this] { Work(); });
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  const int code = CurrentStage() == Stage::Updated || CurrentStage() == Stage::UpToDate ? 0 : 1;
  brand_.reset(), tag_.reset(), title_.reset(), caps_.reset(), small_.reset(), button_.reset(), play_.reset();
  GdiplusShutdown(token);
  return code;
}

}  // namespace

int RunUpdateWindow(HINSTANCE instance, const Paths& p) {
  Log(std::wstring(L"Update opened (this copy v") + kVersion + L")");
  UpdateWindow window(p);
  return window.Run(instance);
}

}  // namespace pd
