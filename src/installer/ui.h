// The look: a small drawing layer so the installer does not look like 2003.
//
// Plain Win32 controls with the message font are what a Windows program looks
// like when nobody decided how it should look. The constraint that rules out
// the usual answers -- WinUI, WPF, a WebView -- is the one that made this a
// single executable in the first place: no runtime to install, nothing to
// download, nothing that can be missing on somebody's machine. So the modern
// parts are drawn here, with GDI+ for antialiased shapes and GDI for text,
// both of which ship with Windows.
//
// What that buys: real typography (Segoe UI Variable where it exists), cards
// on a tinted background, an accent-filled primary button that reacts to the
// pointer, status dots that carry meaning by colour, rounded window corners,
// and light or dark following the system setting. What it costs: buttons and
// checkboxes are owner-drawn and their hover state is tracked by hand, which
// is what most of this file is.
#pragma once

#include <windows.h>

#include <string>

namespace edvr::installer::ui {

struct Theme {
    bool     dark = false;
    COLORREF windowBg = 0;
    COLORREF cardBg = 0;
    COLORREF cardBorder = 0;
    COLORREF text = 0;
    COLORREF subtext = 0;
    COLORREF accent = 0;
    COLORREF accentHover = 0;
    COLORREF accentText = 0;
    COLORREF control = 0;       // secondary button face
    COLORREF controlHover = 0;
    COLORREF controlBorder = 0;
    COLORREF good = 0;          // installed, safe
    COLORREF warn = 0;          // worth knowing
    COLORREF bad = 0;           // broken
    COLORREF muted = 0;         // absent, not applicable
};

struct Fonts {
    HFONT title = nullptr;     // the one big line
    HFONT heading = nullptr;   // card headings
    HFONT body = nullptr;
    HFONT bodyBold = nullptr;
    // Not called "small": windows.h defines `small` as a macro for char, and a
    // field of that name turns into a syntax error two headers away.
    HFONT caption = nullptr;
};

// Follows the system light/dark setting; call refreshTheme() on
// WM_SETTINGCHANGE.
const Theme& theme();
void         refreshTheme();

const Fonts& fonts();
void         buildFonts(UINT dpi);   // rebuilt on DPI change
void         releaseFonts();

// GDI+ has to be alive for any of the shape drawing below.
void startup();
void shutdown();

int dp(int value96, UINT dpi);

// Window chrome that only exists on Windows 10/11 and is a no-op elsewhere:
// a dark title bar to match a dark window, and rounded corners.
void applyWindowChrome(HWND window);

// Shapes. Antialiased, which is the whole reason GDI+ is here: an aliased
// rounded rectangle looks worse than a square one.
void fillRounded(HDC dc, RECT rect, int radius, COLORREF fill);
void strokeRounded(HDC dc, RECT rect, int radius, COLORREF stroke, int inset = 0);
void fillCircle(HDC dc, int centreX, int centreY, int radius, COLORREF fill);
void fillRect(HDC dc, RECT rect, COLORREF fill);

// The slim overlay scrollbar every scrolling pane shares: no track chrome,
// one rounded thumb hugging the pane's right edge, in the card language.
// Geometry, painting and the drag inverse live together so a hit test can
// never drift from what was drawn. All heights are device pixels; an empty
// rect comes back when nothing scrolls.
RECT slimThumb(const RECT& pane, int contentPx, int pagePx, int posPx, UINT dpi);
int  slimPosFromThumbTop(const RECT& pane, int contentPx, int pagePx, int thumbTopY, UINT dpi);
void drawSlimScrollbar(HDC dc, const RECT& pane, int contentPx, int pagePx, int posPx, UINT dpi,
                       bool active);

// A font's line height in device pixels, for sizing single-line EDITs to
// their text: an EDIT draws text at the top of its client area, so vertical
// centring means sizing the control to the text and centring the control.
int textHeightPx(HFONT font);

// One combo look for every real COMBOBOX in the app (the install picker,
// the settings window's resolution dropdown): field and list rows in the
// house font and colours. The OS keeps the frame and the drop arrow.
void drawComboItem(const DRAWITEMSTRUCT* item, UINT dpi);

// Text, with the DPI-correct font and no background fill.
void drawText(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF colour,
              UINT format);
int  textWidth(HDC dc, const std::wstring& text, HFONT font);

// A card: the surface everything sits on.
void paintCard(HDC dc, RECT rect, UINT dpi);

enum class ButtonStyle { Primary, Secondary, Tab, TabActive, Link };

// Owner-drawn button. `style` is remembered per control, and the pointer is
// tracked so it can light up under the mouse.
HWND makeButton(HWND parent, const wchar_t* text, int id, ButtonStyle style, HFONT font);
void setButtonStyle(HWND button, ButtonStyle style);
void drawButton(const DRAWITEMSTRUCT* item, UINT dpi);

// Owner-drawn checkbox. BS_OWNERDRAW does not keep the checked state for us,
// so it is kept here and read back with checkboxChecked().
HWND makeCheckbox(HWND parent, const wchar_t* text, int id, bool checked, HFONT font);
bool checkboxChecked(HWND box);
void setCheckboxChecked(HWND box, bool checked);
void toggleCheckbox(HWND box);
void drawCheckbox(const DRAWITEMSTRUCT* item, UINT dpi);

// True when the control is one of ours, so WM_DRAWITEM can dispatch.
bool drawOwnerDrawn(const DRAWITEMSTRUCT* item, UINT dpi);

}  // namespace edvr::installer::ui
