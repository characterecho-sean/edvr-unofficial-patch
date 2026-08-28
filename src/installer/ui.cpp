#include "ui.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <vector>

namespace edvr::installer::ui {
namespace {

ULONG_PTR g_gdiplusToken = 0;
Theme     g_theme;
Fonts     g_fonts;
bool      g_themeRead = false;

// Per-control state for the owner-drawn ones. A handful of buttons and
// checkboxes, so a vector scan is cheaper than a map and easier to read.
struct Owned {
    HWND        hwnd = nullptr;
    ButtonStyle style = ButtonStyle::Secondary;
    bool        isCheckbox = false;
    bool        checked = false;
    bool        hot = false;
    bool        pressed = false;
};
std::vector<Owned> g_owned;

Owned* find(HWND hwnd) {
    for (Owned& o : g_owned) {
        if (o.hwnd == hwnd) return &o;
    }
    return nullptr;
}

COLORREF mix(COLORREF a, COLORREF b, int percentB) {
    const int r = (GetRValue(a) * (100 - percentB) + GetRValue(b) * percentB) / 100;
    const int g = (GetGValue(a) * (100 - percentB) + GetGValue(b) * percentB) / 100;
    const int bl = (GetBValue(a) * (100 - percentB) + GetBValue(b) * percentB) / 100;
    return RGB(r, g, bl);
}

Gdiplus::Color gdip(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

// A rounded rectangle as a GDI+ path. GDI's own RoundRect is not antialiased,
// and at the radii used here that reads as a jagged edge rather than a corner.
Gdiplus::GraphicsPath* roundedPath(const RECT& rect, int radius) {
    Gdiplus::GraphicsPath* path = new Gdiplus::GraphicsPath();
    const int x = rect.left, y = rect.top;
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    int r = radius;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) {
        path->AddRectangle(Gdiplus::Rect(x, y, w, h));
        return path;
    }
    const int d = r * 2;
    path->AddArc(x, y, d, d, 180.0f, 90.0f);
    path->AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path->AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path->AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
    return path;
}

bool systemPrefersDark() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1, size = sizeof(value), type = 0;
    const LSTATUS st = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                        reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return st == ERROR_SUCCESS && type == REG_DWORD && value == 0;
}

// The system accent, so the primary button belongs to the machine it is running
// on rather than to a palette chosen here. Falls back to a blue that works in
// both themes.
COLORREF systemAccent(bool dark) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, KEY_READ,
                      &key) == ERROR_SUCCESS) {
        DWORD value = 0, size = sizeof(value), type = 0;
        const LSTATUS st = RegQueryValueExW(key, L"AccentColor", nullptr, &type,
                                            reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(key);
        if (st == ERROR_SUCCESS && type == REG_DWORD) {
            // Stored ABGR.
            const COLORREF accent = RGB(value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF);
            // Some accents are too dark or too light to carry white text on a
            // button; nudge rather than refuse.
            const int luma = (GetRValue(accent) * 299 + GetGValue(accent) * 587 +
                              GetBValue(accent) * 114) / 1000;
            if (luma > 150) return mix(accent, RGB(0, 0, 0), 25);
            if (luma < 40) return mix(accent, RGB(255, 255, 255), 20);
            return accent;
        }
    }
    return dark ? RGB(76, 148, 255) : RGB(0, 95, 184);
}

bool fontExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    EnumFontFamiliesExW(
        dc, &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) -> int {
            *reinterpret_cast<bool*>(param) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

HFONT makeFont(const wchar_t* face, int pointsTimesTen, int weight, UINT dpi) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pointsTimesTen, static_cast<int>(dpi), 720);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, face);
    return CreateFontIndirectW(&lf);
}

// Hover and press, which owner-drawn buttons do not get for free.
LRESULT CALLBACK ownedProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                           UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    Owned* owned = find(hwnd);
    switch (message) {
        case WM_MOUSEMOVE: {
            if (owned && !owned->hot) {
                owned->hot = true;
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (owned) {
                owned->hot = false;
                owned->pressed = false;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        }
        case WM_LBUTTONDOWN:
            if (owned) {
                owned->pressed = true;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        case WM_LBUTTONUP:
            if (owned) {
                owned->pressed = false;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ownedProc, 1);
            for (size_t i = 0; i < g_owned.size(); ++i) {
                if (g_owned[i].hwnd == hwnd) {
                    g_owned.erase(g_owned.begin() + i);
                    break;
                }
            }
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

}  // namespace

void startup() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
    refreshTheme();
}

void shutdown() {
    releaseFonts();
    if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
}

int dp(int value96, UINT dpi) { return MulDiv(value96, static_cast<int>(dpi), 96); }

void refreshTheme() {
    const bool dark = systemPrefersDark();
    Theme t;
    t.dark = dark;
    if (dark) {
        t.windowBg = RGB(32, 32, 32);
        t.cardBg = RGB(43, 43, 43);
        t.cardBorder = RGB(58, 58, 58);
        t.text = RGB(240, 240, 240);
        t.subtext = RGB(160, 160, 160);
        t.control = RGB(56, 56, 56);
        t.controlHover = RGB(68, 68, 68);
        t.controlBorder = RGB(78, 78, 78);
        t.good = RGB(108, 203, 130);
        t.warn = RGB(240, 190, 95);
        t.bad = RGB(240, 120, 110);
        t.muted = RGB(130, 130, 130);
    } else {
        t.windowBg = RGB(243, 243, 243);
        t.cardBg = RGB(255, 255, 255);
        t.cardBorder = RGB(226, 226, 226);
        t.text = RGB(26, 26, 26);
        t.subtext = RGB(102, 102, 102);
        t.control = RGB(251, 251, 251);
        t.controlHover = RGB(244, 244, 244);
        t.controlBorder = RGB(214, 214, 214);
        t.good = RGB(16, 124, 65);
        t.warn = RGB(157, 93, 0);
        t.bad = RGB(186, 43, 34);
        t.muted = RGB(140, 140, 140);
    }
    t.accent = systemAccent(dark);
    t.accentHover = mix(t.accent, dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 12);
    t.accentText = RGB(255, 255, 255);
    g_theme = t;
    g_themeRead = true;
}

const Theme& theme() {
    if (!g_themeRead) refreshTheme();
    return g_theme;
}

void buildFonts(UINT dpi) {
    releaseFonts();
    // Segoe UI Variable is the Windows 11 face; Segoe UI is the Windows 10 one.
    // Asking for a face that is not installed gets a silent substitution, which
    // is how a program ends up rendering in something nobody chose.
    const wchar_t* display = fontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display"
                                                                     : L"Segoe UI";
    const wchar_t* body = fontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text"
                                                                : L"Segoe UI";
    g_fonts.title = makeFont(display, 180, FW_SEMIBOLD, dpi);
    g_fonts.heading = makeFont(body, 110, FW_SEMIBOLD, dpi);
    g_fonts.body = makeFont(body, 100, FW_NORMAL, dpi);
    g_fonts.bodyBold = makeFont(body, 100, FW_SEMIBOLD, dpi);
    g_fonts.caption = makeFont(body, 90, FW_NORMAL, dpi);
}

void releaseFonts() {
    HFONT* all[] = {&g_fonts.title, &g_fonts.heading, &g_fonts.body, &g_fonts.bodyBold,
                    &g_fonts.caption};
    for (HFONT* f : all) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

const Fonts& fonts() { return g_fonts; }

void applyWindowChrome(HWND window) {
    // Both of these are Windows 10 20H1 / Windows 11 attributes. On anything
    // older DwmSetWindowAttribute simply fails, which is the desired outcome:
    // the window keeps the system's own chrome.
    const BOOL dark = theme().dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const DWORD round = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
}

void fillRounded(HDC dc, RECT rect, int radius, COLORREF fill) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath* path = roundedPath(rect, radius);
    Gdiplus::SolidBrush brush(gdip(fill));
    graphics.FillPath(&brush, path);
    delete path;
}

void strokeRounded(HDC dc, RECT rect, int radius, COLORREF stroke, int inset) {
    RECT r = rect;
    r.left += inset;
    r.top += inset;
    r.right -= inset + 1;
    r.bottom -= inset + 1;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath* path = roundedPath(r, radius);
    Gdiplus::Pen pen(gdip(stroke), 1.0f);
    graphics.DrawPath(&pen, path);
    delete path;
}

void fillCircle(HDC dc, int centreX, int centreY, int radius, COLORREF fill) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(gdip(fill));
    graphics.FillEllipse(&brush, centreX - radius, centreY - radius, radius * 2, radius * 2);
}

void fillRect(HDC dc, RECT rect, COLORREF fill) {
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void drawText(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF colour,
              UINT format) {
    HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    const int previousMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF previousColour = SetTextColor(dc, colour);
    DrawTextW(dc, text.c_str(), -1, &rect, format);
    SetTextColor(dc, previousColour);
    SetBkMode(dc, previousMode);
    SelectObject(dc, previous);
}

int textWidth(HDC dc, const std::wstring& text, HFONT font) {
    HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, previous);
    return size.cx;
}

void paintCard(HDC dc, RECT rect, UINT dpi) {
    const Theme& t = theme();
    fillRounded(dc, rect, dp(8, dpi), t.cardBg);
    strokeRounded(dc, rect, dp(8, dpi), t.cardBorder);
}

HWND makeButton(HWND parent, const wchar_t* text, int id, ButtonStyle style, HFONT font) {
    HWND button = CreateWindowExW(0, L"BUTTON", text,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                  parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  nullptr, nullptr);
    if (!button) return nullptr;
    if (font) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    Owned owned;
    owned.hwnd = button;
    owned.style = style;
    g_owned.push_back(owned);
    SetWindowSubclass(button, ownedProc, 1, 0);
    return button;
}

void setButtonStyle(HWND button, ButtonStyle style) {
    if (Owned* owned = find(button)) {
        owned->style = style;
        InvalidateRect(button, nullptr, TRUE);
    }
}

HWND makeCheckbox(HWND parent, const wchar_t* text, int id, bool checked, HFONT font) {
    HWND box = CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                               parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                               nullptr);
    if (!box) return nullptr;
    if (font) SendMessageW(box, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    Owned owned;
    owned.hwnd = box;
    owned.isCheckbox = true;
    owned.checked = checked;
    g_owned.push_back(owned);
    SetWindowSubclass(box, ownedProc, 1, 0);
    return box;
}

bool checkboxChecked(HWND box) {
    const Owned* owned = find(box);
    return owned && owned->checked;
}

void setCheckboxChecked(HWND box, bool checked) {
    if (Owned* owned = find(box)) {
        owned->checked = checked;
        InvalidateRect(box, nullptr, TRUE);
    }
}

void toggleCheckbox(HWND box) {
    if (Owned* owned = find(box)) {
        owned->checked = !owned->checked;
        InvalidateRect(box, nullptr, TRUE);
    }
}

void drawButton(const DRAWITEMSTRUCT* item, UINT dpi) {
    const Owned* owned = find(item->hwndItem);
    if (!owned) return;
    const Theme& t = theme();
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool focused = (item->itemState & ODS_FOCUS) != 0;

    wchar_t label[256]{};
    GetWindowTextW(item->hwndItem, label, 256);

    RECT rect = item->rcItem;
    const int radius = dp(6, dpi);
    HDC dc = item->hDC;

    // The parent's card or window colour shows through the rounded corners, so
    // the button has to know which surface it is sitting on.
    const bool onWindow = owned->style == ButtonStyle::Tab ||
                          owned->style == ButtonStyle::TabActive ||
                          owned->style == ButtonStyle::Link;
    fillRect(dc, rect, onWindow ? t.windowBg : t.cardBg);

    COLORREF face = t.control;
    COLORREF border = t.controlBorder;
    COLORREF ink = t.text;

    switch (owned->style) {
        case ButtonStyle::Primary:
            face = owned->hot ? t.accentHover : t.accent;
            border = face;
            ink = t.accentText;
            break;
        case ButtonStyle::Secondary:
            face = owned->hot ? t.controlHover : t.control;
            break;
        case ButtonStyle::TabActive:
            face = t.cardBg;
            border = t.cardBorder;
            ink = t.text;
            break;
        case ButtonStyle::Tab:
            face = owned->hot ? mix(t.windowBg, t.cardBg, 60) : t.windowBg;
            border = face;
            ink = t.subtext;
            break;
        case ButtonStyle::Link:
            face = t.windowBg;
            border = face;
            ink = owned->hot ? t.text : t.accent;
            break;
    }
    if (disabled) {
        face = mix(face, t.windowBg, 55);
        border = mix(border, t.windowBg, 55);
        ink = t.muted;
    }
    if (owned->pressed && !disabled) face = mix(face, RGB(0, 0, 0), t.dark ? 0 : 8);

    if (owned->style != ButtonStyle::Link) {
        fillRounded(dc, rect, radius, face);
        if (border != face) strokeRounded(dc, rect, radius, border);
    }
    if (focused && !disabled) strokeRounded(dc, rect, radius, t.accent, dp(2, dpi));

    RECT textRect = rect;
    drawText(dc, label, textRect, owned->style == ButtonStyle::TabActive ? fonts().bodyBold
                                                                        : fonts().body,
             ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void drawCheckbox(const DRAWITEMSTRUCT* item, UINT dpi) {
    Owned* owned = find(item->hwndItem);
    if (!owned) return;
    const Theme& t = theme();
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    HDC dc = item->hDC;

    wchar_t label[512]{};
    GetWindowTextW(item->hwndItem, label, 512);

    fillRect(dc, item->rcItem, t.cardBg);

    const int side = dp(18, dpi);
    RECT box{item->rcItem.left, 0, 0, 0};
    box.top = item->rcItem.top + ((item->rcItem.bottom - item->rcItem.top) - side) / 2;
    box.right = box.left + side;
    box.bottom = box.top + side;

    const COLORREF face = owned->checked ? (disabled ? mix(t.accent, t.cardBg, 55) : t.accent)
                                         : (owned->hot ? t.controlHover : t.cardBg);
    fillRounded(dc, box, dp(4, dpi), face);
    strokeRounded(dc, box, dp(4, dpi), owned->checked ? face : t.controlBorder);

    if (owned->checked) {
        // A tick, drawn rather than typed: Segoe MDL2 is not on every machine
        // and a font fallback here would put a box glyph in a checkbox.
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(gdip(t.accentText), static_cast<Gdiplus::REAL>(dp(2, dpi)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        const float x = static_cast<float>(box.left), y = static_cast<float>(box.top);
        const float s = static_cast<float>(side);
        Gdiplus::PointF points[3] = {
            {x + s * 0.26f, y + s * 0.52f}, {x + s * 0.43f, y + s * 0.70f},
            {x + s * 0.75f, y + s * 0.31f}};
        graphics.DrawLines(&pen, points, 3);
    }

    RECT textRect = item->rcItem;
    textRect.left = box.right + dp(10, dpi);
    drawText(dc, label, textRect, fonts().body, disabled ? t.muted : t.text,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if ((item->itemState & ODS_FOCUS) && !disabled) {
        RECT focus = item->rcItem;
        strokeRounded(dc, focus, dp(4, dpi), t.accent);
    }
}

bool drawOwnerDrawn(const DRAWITEMSTRUCT* item, UINT dpi) {
    const Owned* owned = find(item->hwndItem);
    if (!owned) return false;
    if (owned->isCheckbox)
        drawCheckbox(item, dpi);
    else
        drawButton(item, dpi);
    return true;
}

RECT slimThumb(const RECT& pane, int contentPx, int pagePx, int posPx, UINT dpi) {
    RECT none{0, 0, 0, 0};
    if (pagePx <= 0 || contentPx <= pagePx) return none;
    const int trackTop = pane.top + dp(3, dpi);
    const int trackBottom = pane.bottom - dp(3, dpi);
    const int track = trackBottom - trackTop;
    if (track <= dp(24, dpi)) return none;

    int thumb = static_cast<int>(static_cast<long long>(track) * pagePx / contentPx);
    const int minThumb = dp(24, dpi);
    if (thumb < minThumb) thumb = minThumb;
    if (thumb > track) thumb = track;

    const int range = contentPx - pagePx;
    int pos = posPx < 0 ? 0 : (posPx > range ? range : posPx);
    const int offset =
        range > 0 ? static_cast<int>(static_cast<long long>(track - thumb) * pos / range) : 0;

    RECT r;
    r.right = pane.right - dp(3, dpi);
    r.left = r.right - dp(5, dpi);
    r.top = trackTop + offset;
    r.bottom = r.top + thumb;
    return r;
}

int slimPosFromThumbTop(const RECT& pane, int contentPx, int pagePx, int thumbTopY, UINT dpi) {
    const RECT ref = slimThumb(pane, contentPx, pagePx, 0, dpi);
    if (ref.right <= ref.left) return 0;
    const int trackTop = pane.top + dp(3, dpi);
    const int track = (pane.bottom - dp(3, dpi)) - trackTop;
    const int thumb = ref.bottom - ref.top;
    const int slack = track - thumb;
    if (slack <= 0) return 0;
    int offset = thumbTopY - trackTop;
    if (offset < 0) offset = 0;
    if (offset > slack) offset = slack;
    return static_cast<int>(static_cast<long long>(contentPx - pagePx) * offset / slack);
}

void drawSlimScrollbar(HDC dc, const RECT& pane, int contentPx, int pagePx, int posPx, UINT dpi,
                       bool active) {
    const RECT thumb = slimThumb(pane, contentPx, pagePx, posPx, dpi);
    if (thumb.right <= thumb.left) return;
    const Theme& t = theme();
    fillRounded(dc, thumb, dp(2, dpi), active ? t.subtext : t.controlBorder);
}

int textHeightPx(HFONT font) {
    HDC dc = GetDC(nullptr);
    HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    ReleaseDC(nullptr, dc);
    return metrics.tmHeight > 0 ? metrics.tmHeight : 16;
}

}  // namespace edvr::installer::ui
