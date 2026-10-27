// Perfect Dark XBLA PC Recomp - the setup window.
//
// The look of the earlier Setup, painted natively: a near-black field, a header
// band with the game's menu-blue fade and scanlines, cyan step tabs and cards,
// and a touch of red for the marker and for errors. One page: the package line
// (Browse Package or drag and drop), the import progress, the optional extras
// and a prominent Play button. Everything is painted with GDI+ in 96-DPI layout
// units and scaled for the monitor's DPI, so the window stays sharp at any
// Windows scaling.
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus

#include "launcher.h"

#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>
#include <shobjidl.h>

#include <memory>
#include <thread>

#include "resource.h"

namespace pd {
namespace {

using namespace Gdiplus;

// ------------------------------------------------------------- layout --
constexpr float kW = 640, kH = 668;
constexpr float kUiScale = 1.1f;  // drawn 10% larger than the 96-DPI layout
constexpr float kRowsY = 236, kRowPitch = 30;   // candidate rows when several packages are found
constexpr int kMaxRows = 4;
constexpr float kExtrasCapsY = 390, kExtrasCardY = 410, kExtraFirst = 422, kExtraPitch = 34;
constexpr int kExtraCount = 5;

enum Control { kNone = -1, kBrowse, kLeft, kPlay, kExtra0, kExtra1, kExtra2, kExtra3, kExtra4, kRow0 };

enum class Mode { Idle, Choosing, Importing, Ready };

constexpr UINT WM_APP_START = WM_APP + 1;
constexpr UINT WM_APP_PROGRESS = WM_APP + 2;
constexpr UINT WM_APP_DONE = WM_APP + 3;

const wchar_t* const kClassName = L"PerfectDarkSetup";
const wchar_t kDot[] = L"·";
const wchar_t kEllipsis[] = L"…";

Color Hex(uint32_t rgb, BYTE alpha = 255) {
  return Color(alpha, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// Carrington Institute palette: near-black field, the game's menu cyan, and a
// touch of Joanna red.
namespace tone {
const uint32_t Background = 0x07090D, Band = 0x0F1D2C, CardTop = 0x0F1826, CardBottom = 0x15263A, Cyan = 0x3FD0E8,
               CyanDeep = 0x1E6E8A, CyanPress = 0x2AA9BE, CyanHover = 0x72E2F2, Red = 0xE0474F, Text = 0xE6EAF7,
               Soft = 0xAFB6D6, Muted = 0x7F88A8, Field = 0x0A0F18, Ok = 0x5BE3A8, Ink = 0x061018;
}

struct ProgressMsg {
  int percent;
  std::wstring phase, detail;
};

struct RectL {
  float x, y, w, h;
  bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct ExtraDef {
  const wchar_t* title;
  const wchar_t* detail;
};
const ExtraDef kExtraDefs[kExtraCount] = {
    {L"Start in fullscreen", L"You can switch at any time in Settings > Graphics."},
    {L"Create desktop shortcut", L"Adds a Perfect Dark icon to your desktop that opens this folder's game."},
    {L"All cheats unlocked", L"Every cheat is available in the Cheats menu. You still switch each one on."},
    {L"100% story completion", L"Marks every mission complete in your save. Stays after unticking."},
    {L"Unlock all multiplayer content", L"Every Combat Simulator weapon, character, scenario and option."},
};

class SetupWindow {
 public:
  SetupWindow(const Paths& p, const SetupOptions& o) : p_(p), options_(o) {}
  int Run(HINSTANCE instance);

 private:
  static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

  void CreateFonts();
  void ApplyScale(UINT dpi, const RECT* suggested);
  void Paint(HDC dc);
  void PaintCard(Graphics& g, float x, float y, float w, float h);
  void PaintTabs(Graphics& g);
  void PaintButton(Graphics& g, Control id, const wchar_t* text, bool enabled, bool primary, Font* font);
  void PaintText(Graphics& g, const std::wstring& text, Font* font, uint32_t color, RectF rect,
                 StringAlignment align = StringAlignmentNear, bool wrap = false);

  RectL ControlRect(Control id) const;
  bool Enabled(Control id) const;
  Control HitTest(float x, float y) const;
  void Click(Control id);

  void Start();
  void Browse();
  void SelectPath(const fs::path& path);
  void StartImport(Source source);  // by value: it may come from candidates_, which is cleared
  void OnProgress(ProgressMsg* msg);
  void OnDone(ImportResult* result);
  void Play();
  void RequestClose();
  void Invalidate() { InvalidateRect(hwnd_, nullptr, FALSE); }

  const Paths& p_;
  SetupOptions options_;
  HWND hwnd_ = nullptr;
  HICON icon_ = nullptr;
  float scale_ = 1;
  Mode mode_ = Mode::Idle;
  bool installed_ = false;
  bool imported_now_ = false;
  bool close_after_import_ = false;
  std::vector<Source> candidates_;
  std::wstring heading_, hint_, path_text_, status_, phase_, stats_, detail_;
  uint32_t status_color_ = tone::Muted, phase_color_ = tone::Cyan;
  bool path_set_ = false;
  int progress_ = 0;
  bool show_bar_ = false;
  Extras want_, was_;
  Control hover_ = kNone, pressed_ = kNone;
  bool tracking_ = false;
  std::atomic<bool> cancel_{false};
  std::thread worker_;
  std::unique_ptr<Font> brand_, tag_, title_, caps_, body_, small_, foot_, button_, play_, row_title_, row_detail_,
      tab_;
};

// ------------------------------------------------------------ helpers --
void RoundedPath(GraphicsPath& path, float x, float y, float w, float h, float r) {
  const float d = std::max(1.0f, r * 2);
  path.AddArc(x, y, d, d, 180, 90);
  path.AddArc(x + w - d, y, d, d, 270, 90);
  path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  path.AddArc(x, y + h - d, d, d, 90, 90);
  path.CloseFigure();
}

// Fonts are sized in 96-DPI pixels (points * 96 / 72); the paint transform scales them.
std::unique_ptr<Font> MakeFont(const wchar_t* family, float points, int style) {
  return std::make_unique<Font>(family, points * 96.0f / 72.0f, style, UnitPixel);
}

void SetupWindow::CreateFonts() {
  brand_ = MakeFont(L"Segoe UI Black", 24, FontStyleRegular);
  tag_ = MakeFont(L"Segoe UI Semibold", 8.5f, FontStyleRegular);
  title_ = MakeFont(L"Segoe UI Semibold", 14, FontStyleRegular);
  caps_ = MakeFont(L"Segoe UI Semibold", 8.25f, FontStyleRegular);
  body_ = MakeFont(L"Segoe UI", 10, FontStyleRegular);
  small_ = MakeFont(L"Segoe UI", 9, FontStyleRegular);
  foot_ = MakeFont(L"Segoe UI", 8, FontStyleRegular);
  button_ = MakeFont(L"Segoe UI Semibold", 10, FontStyleRegular);
  play_ = MakeFont(L"Segoe UI Black", 15, FontStyleRegular);
  row_title_ = MakeFont(L"Segoe UI Semibold", 10, FontStyleRegular);
  row_detail_ = MakeFont(L"Segoe UI", 8.5f, FontStyleRegular);
  tab_ = MakeFont(L"Segoe UI Semibold", 8.5f, FontStyleRegular);
}

// --------------------------------------------------------------- scale --
// The layout scale for a monitor: its DPI times the enlargement, reduced when
// the window would not fit the monitor's work area (small laptop screens).
void SetupWindow::ApplyScale(UINT dpi, const RECT* suggested) {
  HMONITOR monitor = suggested ? MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST)
                               : (hwnd_ ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST)
                                        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
  MONITORINFO info = {sizeof(info)};
  GetMonitorInfoW(monitor, &info);
  const RECT work = info.rcWork;
  const float base = dpi / 96.0f;
  const float fit = std::min((work.right - work.left) * 0.95f / kW, (work.bottom - work.top) * 0.95f / kH);
  scale_ = std::max(0.5f, std::min(base * kUiScale, fit));
  const int w = static_cast<int>(kW * scale_ + 0.5f), h = static_cast<int>(kH * scale_ + 0.5f);
  int x, y;
  if (suggested) {
    x = suggested->left;
    y = suggested->top;
  } else {
    x = work.left + ((work.right - work.left) - w) / 2;
    y = work.top + ((work.bottom - work.top) - h) / 2;
  }
  SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  const int r = static_cast<int>(16 * scale_);
  SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2), TRUE);
  Invalidate();
}

// ------------------------------------------------------------ controls --
RectL SetupWindow::ControlRect(Control id) const {
  switch (id) {
    case kBrowse: return {456, 236, 124, 34};
    case kLeft: return {330, 610, 104, 38};
    case kPlay: return {446, 602, 154, 52};
    case kExtra0:
    case kExtra1:
    case kExtra2:
    case kExtra3:
    case kExtra4: return {52, kExtraFirst + (id - kExtra0) * kExtraPitch - 3, 548, kExtraPitch};
    default: {
      const int row = id - kRow0;
      return {58, kRowsY + row * kRowPitch, 392, kRowPitch - 2};
    }
  }
}

bool SetupWindow::Enabled(Control id) const {
  switch (id) {
    case kBrowse: return mode_ != Mode::Importing && !imported_now_;
    case kLeft: return !(mode_ == Mode::Importing && cancel_.load());
    case kPlay: return mode_ == Mode::Ready && installed_;
    case kExtra0:
    case kExtra1:
    case kExtra2:
    case kExtra3:
    case kExtra4: return true;
    default: return mode_ == Mode::Choosing && id - kRow0 < static_cast<int>(candidates_.size());
  }
}

Control SetupWindow::HitTest(float x, float y) const {
  std::vector<Control> ids = {kBrowse, kLeft, kPlay, kExtra0, kExtra1, kExtra2, kExtra3, kExtra4};
  if (mode_ == Mode::Choosing) {
    for (int i = 0; i < static_cast<int>(candidates_.size()) && i < kMaxRows; ++i)
      ids.push_back(static_cast<Control>(kRow0 + i));
  }
  for (Control id : ids) {
    if (ControlRect(id).Contains(x, y) && Enabled(id)) return id;
  }
  return kNone;
}

// --------------------------------------------------------------- paint --
void SetupWindow::PaintText(Graphics& g, const std::wstring& text, Font* font, uint32_t color, RectF rect,
                            StringAlignment align, bool wrap) {
  if (text.empty()) return;
  StringFormat format;
  format.SetAlignment(align);
  format.SetLineAlignment(StringAlignmentNear);
  format.SetTrimming(wrap ? StringTrimmingWord : StringTrimmingEllipsisPath);
  if (!wrap) format.SetFormatFlags(StringFormatFlagsNoWrap);
  SolidBrush brush(Hex(color));
  g.DrawString(text.c_str(), -1, font, rect, &format, &brush);
}

void SetupWindow::PaintCard(Graphics& g, float x, float y, float w, float h) {
  GraphicsPath card;
  RoundedPath(card, x, y, w, h, 14);
  LinearGradientBrush fill(RectF(x, y, w, h), Hex(tone::CardTop), Hex(tone::CardBottom), 90.0f);
  g.FillPath(&fill, &card);
  Pen edge(Hex(tone::Cyan, 150), 1.2f);
  g.DrawPath(&edge, &card);
}

// Step tabs under the header: done steps get a tick and a full cyan underline;
// the current one is white with the red marker, and Import's underline fills
// with the real progress; later steps stay dim.
void SetupWindow::PaintTabs(Graphics& g) {
  const int stage = mode_ == Mode::Ready ? 2 : mode_ == Mode::Importing ? 1 : 0;
  const wchar_t* names[3] = {L"PACKAGE", L"IMPORT", L"PLAY"};
  constexpr float kTabW = 560.0f / 3.0f;
  StringFormat center;
  center.SetAlignment(StringAlignmentCenter);
  SolidBrush track(Hex(tone::Cyan, 40));
  SolidBrush cyan(Hex(tone::Cyan));
  SolidBrush red(Hex(tone::Red));
  for (int i = 0; i < 3; ++i) {
    const float tx = 40 + i * kTabW;
    g.FillRectangle(&track, tx, 126.0f, kTabW - 1, 2.0f);
    float fill = 0;
    uint32_t color = tone::Muted;
    std::wstring label = names[i];
    if (i < stage) {
      color = tone::Cyan;
      fill = 1;
    } else if (i == stage) {
      color = tone::Text;
      if (i == 1 && mode_ == Mode::Importing) {
        label += L"  " + std::wstring(kDot) + L"  " + std::to_wstring(progress_) + L"%";
        fill = progress_ / 100.0f;
      }
      g.FillRectangle(&red, tx, 125.0f, 24.0f, 4.0f);
    }
    if (fill > 0) g.FillRectangle(&cyan, tx, 126.0f, (kTabW - 1) * fill, 2.0f);
    SolidBrush ink(Hex(color));
    g.DrawString(label.c_str(), -1, tab_.get(), RectF(tx, 100, kTabW - 1, 20), &center, &ink);
    if (i < stage) {
      // A done step gets a tick before its name (drawn: no glyph dependency).
      RectF bounds;
      g.MeasureString(label.c_str(), -1, tab_.get(), RectF(tx, 100, kTabW - 1, 20), &center, &bounds);
      const float cx = bounds.X - 11, cy = 111;
      Pen tick(Hex(tone::Cyan), 1.8f);
      tick.SetStartCap(LineCapRound);
      tick.SetEndCap(LineCapRound);
      tick.SetLineJoin(LineJoinRound);
      const PointF points[3] = {PointF(cx - 4, cy), PointF(cx - 1, cy + 3.5f), PointF(cx + 4.5f, cy - 3.5f)};
      g.DrawLines(&tick, points, 3);
    }
  }
}

// Secondary buttons are dark cards with a pale label; Play is the one solid
// cyan control, so it reads as the thing to press.
void SetupWindow::PaintButton(Graphics& g, Control id, const wchar_t* text, bool enabled, bool primary, Font* font) {
  const RectL r = ControlRect(id);
  uint32_t back, fore, border;
  if (!enabled) {
    back = 0x131E2A, fore = 0x4F6072, border = 0x1E2C3A;
  } else if (primary) {
    back = pressed_ == id ? tone::CyanPress : hover_ == id ? tone::CyanHover : tone::Cyan, fore = tone::Ink,
    border = hover_ == id || pressed_ == id ? tone::CyanHover : tone::Cyan;
  } else {
    back = pressed_ == id ? 0x0A121C : hover_ == id ? 0x15253A : 0x0D1724, fore = tone::Soft, border = 0x34465E;
  }
  if (primary && enabled) {
    // A soft glow around the main action.
    for (int i = 3; i >= 1; --i) {
      GraphicsPath glow;
      RoundedPath(glow, r.x - i * 2, r.y - i * 2, r.w + i * 4, r.h + i * 4, 8.0f + i * 2);
      Pen pen(Hex(tone::Cyan, static_cast<BYTE>(16 + (4 - i) * 6)), 2.0f);
      g.DrawPath(&pen, &glow);
    }
  }
  GraphicsPath shape;
  RoundedPath(shape, r.x, r.y, r.w, r.h, primary ? 8.0f : 6.0f);
  if (primary && enabled) {
    LinearGradientBrush fill(RectF(r.x, r.y, r.w, r.h), Hex(back), Hex(pressed_ == id ? back : tone::CyanPress), 90.0f);
    g.FillPath(&fill, &shape);
  } else {
    SolidBrush fill(Hex(back));
    g.FillPath(&fill, &shape);
  }
  Pen pen(Hex(border), primary && enabled ? 1.6f : 1.2f);
  g.DrawPath(&pen, &shape);
  StringFormat center;
  center.SetAlignment(StringAlignmentCenter);
  center.SetLineAlignment(StringAlignmentCenter);
  center.SetFormatFlags(StringFormatFlagsNoWrap);
  SolidBrush brush(Hex(fore));
  g.DrawString(text, -1, font, RectF(r.x, r.y + 1, r.w, r.h), &center, &brush);
}

void SetupWindow::Paint(HDC dc) {
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

    // The field, then the header band fading from the game's deep menu blue into it.
    SolidBrush field(Hex(tone::Background));
    g.FillRectangle(&field, RectF(-1, -1, kW + 2, kH + 2));
    LinearGradientBrush band(RectF(0, 0, kW, 136), Hex(tone::Band), Hex(tone::Background), 90.0f);
    g.FillRectangle(&band, RectF(0, 0, kW, 136));
    // Scanlines in the top corner, echoing the game's own menu frames.
    Pen scan(Hex(tone::Cyan, 30), 1);
    for (int i = 0; i < 13; ++i) {
      const float x = 404.0f + i * 18.0f;
      g.DrawLine(&scan, x, 0.0f, x + 72.0f, 96.0f);
    }

    PaintText(g, L"PERFECT DARK", brand_.get(), tone::Text, RectF(34, 18, 440, 52));
    PaintText(g, std::wstring(L"XBLA PC PORT  ") + kDot + L"  SETUP  " + kDot + L"  V" + kVersion, tag_.get(),
              tone::Cyan, RectF(40, 68, 440, 20));
    PaintTabs(g);
    const wchar_t* title = mode_ == Mode::Ready      ? L"Ready to play"
                           : mode_ == Mode::Importing ? L"Importing your game"
                                                      : L"Set up your game";
    PaintText(g, title, title_.get(), tone::Text, RectF(38, 138, 400, 32));

    // Package card.
    const bool choosing = mode_ == Mode::Choosing;
    PaintCard(g, 40, 176, 560, choosing ? 188.0f : 140.0f);
    PaintText(g, L"GAME PACKAGE", caps_.get(), tone::Cyan, RectF(60, 190, 300, 18));
    PaintText(g, heading_, body_.get(), tone::Soft, RectF(60, 208, 520, 22));
    if (choosing) {
      for (int i = 0; i < static_cast<int>(candidates_.size()) && i < kMaxRows; ++i) {
        const RectL r = ControlRect(static_cast<Control>(kRow0 + i));
        if (hover_ == kRow0 + i) {
          SolidBrush hover(Hex(tone::Cyan, 34));
          g.FillRectangle(&hover, r.x, r.y, r.w, r.h);
        }
        Pen ring(Hex(tone::Cyan, 170), 1.4f);
        g.DrawEllipse(&ring, r.x + 6, r.y + 6, 14.0f, 14.0f);
        const Source& s = candidates_[i];
        std::wstring text = (s.kind == SourceKind::PreviousInstall ? L"Previous install  " : L"") + s.path.wstring();
        PaintText(g, text, small_.get(), tone::Text, RectF(r.x + 28, r.y + 4, r.w - 100, 22));
        PaintText(g, FormatMB(s.bytes), small_.get(), tone::Muted, RectF(r.x + r.w - 72, r.y + 4, 68, 22),
                  StringAlignmentFar);
      }
    } else {
      GraphicsPath fieldShape;
      RoundedPath(fieldShape, 60, 236, 384, 34, 3);
      SolidBrush fieldFill(Hex(tone::Field));
      g.FillPath(&fieldFill, &fieldShape);
      Pen fieldEdge(Hex(tone::Cyan, 90), 1);
      g.DrawPath(&fieldEdge, &fieldShape);
      PaintText(g, path_text_, small_.get(), path_set_ ? tone::Text : tone::Muted, RectF(68, 244, 368, 24));
      PaintText(g, status_, small_.get(), status_color_, RectF(60, 278, 524, 34), StringAlignmentNear, true);
    }
    const wchar_t* browse = imported_now_ ? L"Imported" : L"Browse Package";
    PaintButton(g, kBrowse, browse, Enabled(kBrowse), false, button_.get());

    // Progress: phase on the left, live counts on the right, a cyan bar.
    if (!choosing) {
      PaintText(g, phase_, caps_.get(), phase_color_, RectF(40, 326, 560, 20));
      PaintText(g, stats_, small_.get(), tone::Muted, RectF(300, 326, 300, 20), StringAlignmentFar);
      if (show_bar_) {
        GraphicsPath track;
        RoundedPath(track, 40, 348, 560, 8, 4);
        SolidBrush trackFill(Hex(tone::Field));
        g.FillPath(&trackFill, &track);
        Pen trackEdge(Hex(tone::Cyan, 70), 1);
        g.DrawPath(&trackEdge, &track);
        const float width = 560.0f * progress_ / 100.0f;
        if (width > 2) {
          GraphicsPath bar;
          RoundedPath(bar, 40, 348, width, 8, std::min(4.0f, width / 2));
          LinearGradientBrush barFill(RectF(40, 348, 560, 8), Hex(tone::CyanDeep), Hex(tone::Cyan), 0.0f);
          g.FillPath(&barFill, &bar);
        }
      }
      PaintText(g, detail_, small_.get(), tone::Muted, RectF(40, 362, 560, 20));
    }

    // Extras: a painted checkbox, a title and a one-line explanation each.
    PaintText(g, std::wstring(L"OPTIONAL EXTRAS  ") + kDot + L"  APPLIED WHEN YOU PRESS PLAY", caps_.get(),
              tone::Cyan, RectF(40, kExtrasCapsY, 560, 18));
    PaintCard(g, 40, kExtrasCardY, 560, 12 + kExtraCount * kExtraPitch);
    const bool on[kExtraCount] = {want_.fullscreen, want_.shortcut, want_.cheats, want_.story, want_.multiplayer};
    for (int i = 0; i < kExtraCount; ++i) {
      const float y = kExtraFirst + i * kExtraPitch;
      if (hover_ == kExtra0 + i) {
        const RectL r = ControlRect(static_cast<Control>(kExtra0 + i));
        GraphicsPath hoverShape;
        RoundedPath(hoverShape, r.x, r.y, r.w, r.h, 6);
        SolidBrush hover(Hex(tone::Cyan, 22));
        g.FillPath(&hover, &hoverShape);
      }
      GraphicsPath box;
      RoundedPath(box, 62, y + 3, 18, 18, 3);
      if (on[i]) {
        SolidBrush fill(Hex(tone::Cyan));
        g.FillPath(&fill, &box);
        Pen tick(Hex(tone::Ink), 2.2f);
        tick.SetStartCap(LineCapRound);
        tick.SetEndCap(LineCapRound);
        tick.SetLineJoin(LineJoinRound);
        const PointF points[3] = {PointF(66, y + 12.5f), PointF(70, y + 16.5f), PointF(77, y + 7.5f)};
        g.DrawLines(&tick, points, 3);
      } else {
        SolidBrush fill(Hex(tone::Field));
        g.FillPath(&fill, &box);
        Pen edge(Hex(tone::Cyan, 170), 1.2f);
        g.DrawPath(&edge, &box);
      }
      PaintText(g, kExtraDefs[i].title, row_title_.get(), tone::Text, RectF(92, y - 1, 490, 20));
      PaintText(g, kExtraDefs[i].detail, row_detail_.get(), tone::Muted, RectF(92, y + 17, 500, 16));
    }

    // Footer and the two buttons.
    PaintText(g, std::wstring(L"Xbox LIVE Arcade  ") + kDot + L"  Title ID 584109C2", foot_.get(), tone::Muted,
              RectF(40, 625, 270, 16));
    const wchar_t* left = mode_ == Mode::Importing ? (cancel_.load() ? L"Cancelling" : L"Cancel") : L"Close";
    PaintButton(g, kLeft, left, Enabled(kLeft), false, button_.get());
    PaintButton(g, kPlay, L"PLAY", Enabled(kPlay), true, play_.get());

    // Window outline.
    GraphicsPath outline;
    RoundedPath(outline, 0.5f, 0.5f, kW - 1, kH - 1, 16);
    Pen outlinePen(Hex(tone::Cyan, 110), 1);
    g.DrawPath(&outlinePen, &outline);
  }
  BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
  SelectObject(memory, old);
  DeleteObject(bitmap);
  DeleteDC(memory);
}

// ---------------------------------------------------------------- flow --
void SetupWindow::Start() {
  std::wstring missing;
  installed_ = IsInstalled(p_, &missing);
  // The extras start unticked on a first setup, where an unticked box changes
  // nothing. Reopened with --setup they show what is set, and unticking one
  // switches it off.
  was_ = installed_ ? CurrentExtras(p_, options_.shortcut_folder) : Extras{};
  want_ = was_;
  if (!options_.package.empty()) {
    SelectPath(options_.package);
    return;
  }
  if (installed_) {
    mode_ = Mode::Ready;
    heading_ = L"Perfect Dark is set up in this folder";
    path_text_ = L"resources\\assets";
    path_set_ = true;
    status_ = L"✓  Game data imported. Saves and settings are in resources\\userdata.";
    status_color_ = tone::Ok;
    phase_ = L"READY TO PLAY";
    phase_color_ = tone::Ok;
    detail_ = L"Press Play, change your extras, or browse for your package to import it again.";
    Invalidate();
    return;
  }
  // The game's own program files come with the release, not with the package:
  // when one is gone (or the launcher was started from inside the ZIP) importing
  // cannot help, so say so at once instead of looking for a package.
  if (missing.rfind(L"resources\\runtime\\", 0) == 0) {
    heading_ = L"Perfect Dark's program files are missing";
    path_text_ = missing;
    path_set_ = true;
    status_ = L"Extract the whole ZIP, then open Perfect Dark.exe again. Your game data and saves are not affected.";
    status_color_ = tone::Red;
    phase_ = L"INCOMPLETE DOWNLOAD";
    phase_color_ = tone::Red;
    detail_ = L"Details: resources\\runtime\\logs\\setup.log";
    Log(L"Not looking for a package: " + missing + L" is missing.");
    Invalidate();
    return;
  }
  heading_ = L"Select your Xbox 360 Perfect Dark package";
  path_text_ = L"No package selected";
  path_set_ = false;
  status_ = std::wstring(L"Looking beside Perfect Dark.exe") + kEllipsis;
  status_color_ = tone::Muted;
  Invalidate();
  UpdateWindow(hwnd_);
  std::vector<Source> found = DetectSources(p_);
  Log(L"Detected " + std::to_wstring(found.size()) + L" package(s) or previous install(s).");
  if (found.size() == 1) {
    StartImport(found[0]);
  } else if (found.size() > 1) {
    candidates_ = found;
    if (candidates_.size() > kMaxRows) candidates_.resize(kMaxRows);
    mode_ = Mode::Choosing;
    heading_ = L"More than one was found. Choose the one to import:";
    Invalidate();
  } else {
    status_ = L"No package was found beside Perfect Dark.exe or in a \"Game Package\" folder. "
              L"Drop the package file or its folder onto this window, or browse for it.";
    status_color_ = tone::Muted;
    Invalidate();
  }
}

void SetupWindow::Browse() {
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
  DWORD flags = 0;
  dialog->GetOptions(&flags);
  dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_DONTADDTORECENT);
  dialog->SetTitle(L"Select your Perfect Dark Xbox 360 package");
  COMDLG_FILTERSPEC filter = {L"Xbox 360 package (any file)", L"*.*"};
  dialog->SetFileTypes(1, &filter);
  IShellItem* folder = nullptr;
  if (SUCCEEDED(SHCreateItemFromParsingName(p_.root.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
    dialog->SetFolder(folder);
    folder->Release();
  }
  if (SUCCEEDED(dialog->Show(hwnd_))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        SelectPath(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
}

void SetupWindow::SelectPath(const fs::path& path) {
  if (mode_ == Mode::Importing || imported_now_) return;
  path_text_ = path.wstring();
  path_set_ = true;
  phase_.clear();
  stats_.clear();
  detail_.clear();
  show_bar_ = false;
  SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  std::wstring why;
  std::vector<Source> found = SourcesIn(path, &why);
  if (found.empty()) {
    Log(L"Rejected " + path.wstring() + L": " + why);
    if (mode_ == Mode::Choosing) mode_ = Mode::Idle;
    if (installed_) mode_ = Mode::Ready;
    heading_ = installed_ ? L"Perfect Dark is set up in this folder" : L"Select your Xbox 360 Perfect Dark package";
    status_ = why;
    status_color_ = tone::Red;
    Invalidate();
    return;
  }
  if (found.size() == 1) {
    StartImport(found[0]);
    return;
  }
  candidates_ = found;
  if (candidates_.size() > kMaxRows) candidates_.resize(kMaxRows);
  mode_ = Mode::Choosing;
  heading_ = L"That folder holds more than one. Choose the one to import:";
  Invalidate();
}

void SetupWindow::StartImport(Source source) {
  if (worker_.joinable()) worker_.join();
  mode_ = Mode::Importing;
  candidates_.clear();
  path_text_ = Describe(source);
  path_set_ = true;
  heading_ = L"Your package is only read, never changed. Nothing leaves this folder.";
  status_ = L"✓  Perfect Dark  " + std::wstring(kDot) + L"  " +
            (source.kind == SourceKind::PreviousInstall ? L"previous install" : L"Xbox LIVE Arcade package") +
            L"  " + kDot + L"  " + FormatMB(source.bytes);
  status_color_ = tone::Ok;
  phase_ = L"PREPARING";
  phase_color_ = tone::Cyan;
  stats_.clear();
  detail_.clear();
  progress_ = 0;
  show_bar_ = true;
  cancel_ = false;
  Invalidate();
  const fs::path saves_from = source.kind == SourceKind::PreviousInstall ? source.path : FindPreviousInstall(p_);
  HWND hwnd = hwnd_;
  worker_ = std::thread([this, hwnd, source, saves_from]() {
    ImportResult* result = new ImportResult(Import(p_, source, saves_from, cancel_, [hwnd](int percent, const std::wstring& phase, const std::wstring& detail) {
      PostMessageW(hwnd, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(new ProgressMsg{percent, phase, detail}));
    }));
    PostMessageW(hwnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(result));
  });
}

void SetupWindow::OnProgress(ProgressMsg* msg) {
  if (mode_ == Mode::Importing) {
    progress_ = std::clamp(msg->percent, 0, 100);
    phase_ = msg->phase;
    stats_ = msg->detail;
    Invalidate();
  }
  delete msg;
}

void SetupWindow::OnDone(ImportResult* result) {
  if (worker_.joinable()) worker_.join();
  stats_.clear();
  if (result->ok) {
    mode_ = Mode::Ready;
    installed_ = true;
    imported_now_ = true;
    heading_ = L"Your package is no longer needed. Perfect Dark.exe now starts the game.";
    progress_ = 100;
    phase_ = L"READY TO PLAY";
    phase_color_ = tone::Ok;
    detail_ = result->migrated.empty() ? L"Game data imported into resources\\assets." : result->migrated;
    status_ = L"✓  Imported and verified. Tick any extras you want, then press Play.";
    status_color_ = tone::Ok;
    if (GetForegroundWindow() != hwnd_) {
      FLASHWINFO flash = {sizeof(flash), hwnd_, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
      FlashWindowEx(&flash);
    }
  } else {
    mode_ = installed_ ? Mode::Ready : Mode::Idle;
    show_bar_ = false;
    phase_ = result->cancelled ? L"IMPORT CANCELLED" : L"IMPORT DID NOT COMPLETE";
    phase_color_ = result->cancelled ? tone::Muted : tone::Red;
    status_ = result->message;
    status_color_ = result->cancelled ? tone::Muted : tone::Red;
    detail_ = result->cancelled ? L"" : L"Your package was not changed. Details: resources\\runtime\\logs\\setup.log";
    heading_ = installed_ ? L"Perfect Dark is set up in this folder" : L"Select your Xbox 360 Perfect Dark package";
  }
  delete result;
  cancel_ = false;
  Invalidate();
  if (close_after_import_) DestroyWindow(hwnd_);
}

void SetupWindow::Play() {
  const std::wstring note = ApplyExtras(p_, want_, was_, options_.shortcut_folder, [this](const fs::path& link) {
    const std::wstring text = L"There is already a Perfect Dark shortcut on your desktop that opens a different copy:\n\n" +
                              link.wstring() + L"\n\nReplace it with one for this folder?";
    return MessageBoxW(hwnd_, text.c_str(), L"Perfect Dark", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
  });
  was_ = want_;
  if (!note.empty()) MessageBoxW(hwnd_, note.c_str(), L"Perfect Dark", MB_OK | MB_ICONINFORMATION);
  std::wstring error;
  if (!LaunchGame(p_, options_.game_args, &error)) {
    Log(L"FAILED to start the game: " + error);
    MessageBoxW(hwnd_, error.c_str(), L"Perfect Dark", MB_OK | MB_ICONERROR);
    return;
  }
  Log(L"Play: game started.");
  DestroyWindow(hwnd_);
}

void SetupWindow::RequestClose() {
  if (mode_ == Mode::Importing) {
    // Closing stops the import first; the window goes once it has cleaned up.
    close_after_import_ = true;
    cancel_ = true;
    Invalidate();
    return;
  }
  DestroyWindow(hwnd_);
}

void SetupWindow::Click(Control id) {
  switch (id) {
    case kBrowse: Browse(); break;
    case kLeft:
      if (mode_ == Mode::Importing) {
        cancel_ = true;
        Invalidate();
      } else {
        RequestClose();
      }
      break;
    case kPlay: Play(); break;
    case kExtra0: want_.fullscreen = !want_.fullscreen; break;
    case kExtra1: want_.shortcut = !want_.shortcut; break;
    case kExtra2: want_.cheats = !want_.cheats; break;
    case kExtra3: want_.story = !want_.story; break;
    case kExtra4: want_.multiplayer = !want_.multiplayer; break;
    default:
      if (id >= kRow0 && id - kRow0 < static_cast<int>(candidates_.size())) StartImport(candidates_[id - kRow0]);
      break;
  }
  Invalidate();
}

// ------------------------------------------------------------- messages --
LRESULT CALLBACK SetupWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  SetupWindow* self = reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    self = static_cast<SetupWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SetupWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      Paint(dc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: ApplyScale(HIWORD(wp), reinterpret_cast<RECT*>(lp)); return 0;
    case WM_APP_START: Start(); return 0;
    case WM_APP_PROGRESS: OnProgress(reinterpret_cast<ProgressMsg*>(lp)); return 0;
    case WM_APP_DONE: OnDone(reinterpret_cast<ImportResult*>(lp)); return 0;
    case WM_MOUSEMOVE: {
      if (!tracking_) {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
        tracking_ = TrackMouseEvent(&track) != FALSE;
      }
      const Control hit = HitTest(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
      if (hit != hover_) {
        hover_ = hit;
        Invalidate();
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_ = false;
      if (hover_ != kNone) {
        hover_ = kNone;
        Invalidate();
      }
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
        // Borderless window: drag it from anywhere that is not a control.
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
      if (wp == VK_ESCAPE) RequestClose();
      if (wp == VK_RETURN && Enabled(kPlay)) Play();
      return 0;
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wp);
      wchar_t path[32768] = {};
      if (DragQueryFileW(drop, 0, path, 32768) > 0) SelectPath(path);
      DragFinish(drop);
      return 0;
    }
    case WM_CLOSE: RequestClose(); return 0;
    case WM_DESTROY:
      if (worker_.joinable()) {
        cancel_ = true;
        worker_.join();
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

int SetupWindow::Run(HINSTANCE instance) {
  ULONG_PTR token = 0;
  GdiplusStartupInput input;
  if (GdiplusStartup(&token, &input, nullptr) != Ok) return 1;
  CreateFonts();
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
  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_ACCEPTFILES, kClassName, L"Perfect Dark Setup",
                          WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, 0, 0, 100, 100, nullptr, nullptr, instance, this);
  if (!hwnd_) return 1;
  // Explorer runs unelevated like us, but allow drops from it either way.
  ChangeWindowMessageFilterEx(hwnd_, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
  ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
  ChangeWindowMessageFilterEx(hwnd_, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);
  ApplyScale(GetDpiForWindow(hwnd_), nullptr);
  heading_ = L"Select your Xbox 360 Perfect Dark package";
  ShowWindow(hwnd_, SW_SHOWNORMAL);
  SetForegroundWindow(hwnd_);
  UpdateWindow(hwnd_);
  PostMessageW(hwnd_, WM_APP_START, 0, 0);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  const int code = installed_ ? 0 : 1;
  // Fonts are GDI+ objects: release them before GDI+ shuts down.
  brand_.reset(), tag_.reset(), title_.reset(), caps_.reset(), body_.reset(), small_.reset(), foot_.reset();
  button_.reset(), play_.reset(), row_title_.reset(), row_detail_.reset(), tab_.reset();
  GdiplusShutdown(token);
  return code;
}

}  // namespace

int RunSetupWindow(HINSTANCE instance, const Paths& p, const SetupOptions& options) {
  Log(L"Setup opened" + (options.reason.empty() ? std::wstring() : L" (" + options.reason + L")"));
  SetupWindow window(p, options);
  return window.Run(instance);
}

}  // namespace pd
