// visual.desktop_window (docs\desktop-window.md): the pure rules. What the setting's words mean, the shape of the picture, where it goes in the swap chain's
// buffer, the window that shows it, the formats it may be drawn in, and the two log lines tools\edvr_log.py --health reads. No device and no window here;
// tools\desktop_window_test runs every function.
//
// THE PICTURE. One eye is drawn as the eye's bounds sub-rect, in pixels, so its shape is |du| x texW over |dv| x texH. Both eyes side by side is the sum of the two
// widths over the larger height. The picture is fitted, never stretched: when the window has the picture's shape it fills the client area exactly, and when it has
// not (the window is not resized yet, the game fought the move, exclusive fullscreen) the picture is undistorted with black bars.
//
// WHICH PART OF THE BUFFER THE CLIENT SHOWS. Scaling STRETCH (and every legacy blit-model chain, which always stretches) maps the whole buffer onto the client, so the
// picture is fitted to the client's shape and then stretched over the buffer with the buffer's own factors. Scaling NONE shows the top-left client-sized region of the
// buffer, and the picture is fitted into that. ASPECT_RATIO_STRETCH keeps the buffer's own shape on screen, so the picture is fitted into the buffer.
#pragma once

#include <dxgiformat.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

enum class DesktopWindowMode : uint8_t { Game = 0, LeftEye = 1, RightEye = 2, BothEyes = 3 };

namespace dwdetail {
inline bool sameWord(const char* text, const char* word) {
    for (;; ++text, ++word) {
        const char a = *text >= 'A' && *text <= 'Z' ? static_cast<char>(*text + 32) : *text;
        if (a != *word) return false;
        if (!a) return true;
    }
}
}  // namespace dwdetail

// The key's words. The empty string is the default (game, known); any other word that is not one of the four is game and unknown, so the caller can say so once.
// The flat profile never reaches this: its getString answers "off" for a key it does not allow, which is an unknown word and so game.
inline DesktopWindowMode desktopWindowParse(const char* text, bool* known = nullptr) {
    if (known) *known = true;
    if (!text || !*text || dwdetail::sameWord(text, "game")) return DesktopWindowMode::Game;
    if (dwdetail::sameWord(text, "left_eye")) return DesktopWindowMode::LeftEye;
    if (dwdetail::sameWord(text, "right_eye")) return DesktopWindowMode::RightEye;
    if (dwdetail::sameWord(text, "both_eyes")) return DesktopWindowMode::BothEyes;
    if (known) *known = false;
    return DesktopWindowMode::Game;
}

inline const char* desktopWindowModeName(DesktopWindowMode mode) {
    switch (mode) {
        case DesktopWindowMode::LeftEye: return "left_eye";
        case DesktopWindowMode::RightEye: return "right_eye";
        case DesktopWindowMode::BothEyes: return "both_eyes";
        default: return "game";
    }
}

// The eyes a mode draws: bit 0 the left, bit 1 the right.
inline unsigned desktopWindowEyeMask(DesktopWindowMode mode) {
    switch (mode) {
        case DesktopWindowMode::LeftEye: return 1u;
        case DesktopWindowMode::RightEye: return 2u;
        case DesktopWindowMode::BothEyes: return 3u;
        default: return 0u;
    }
}

constexpr uint64_t kDesktopWindowStaleMs = 500;   // an eye image older than this is no longer being handed over

struct DwSize { double w = 0, h = 0; };
struct DwRect { double x = 0, y = 0, w = 0, h = 0; };
struct DwPixelRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool ok() const { return w > 0 && h > 0; }
};

// The eye's size in pixels inside its bounds. bounds is {uMin, vMin, uMax, vMax}; a flipped pair has the same size.
inline DwSize desktopWindowEyeSize(uint32_t textureWidth, uint32_t textureHeight, const float bounds[4]) {
    DwSize s;
    s.w = std::fabs(static_cast<double>(bounds[2]) - static_cast<double>(bounds[0])) * textureWidth;
    s.h = std::fabs(static_cast<double>(bounds[3]) - static_cast<double>(bounds[1])) * textureHeight;
    return s;
}

// The shape (width over height) of what the mode draws, from the eyes it needs. False when a needed eye has no size.
inline bool desktopWindowAspect(DesktopWindowMode mode, const DwSize eyes[2], double* aspect) {
    double w = 0, h = 0;
    switch (mode) {
        case DesktopWindowMode::LeftEye: w = eyes[0].w; h = eyes[0].h; break;
        case DesktopWindowMode::RightEye: w = eyes[1].w; h = eyes[1].h; break;
        case DesktopWindowMode::BothEyes:
            if (!(eyes[0].w > 0) || !(eyes[0].h > 0) || !(eyes[1].w > 0) || !(eyes[1].h > 0)) return false;
            w = eyes[0].w + eyes[1].w;
            h = eyes[0].h > eyes[1].h ? eyes[0].h : eyes[1].h;
            break;
        default: return false;
    }
    if (!(w > 0) || !(h > 0) || !std::isfinite(w) || !std::isfinite(h)) return false;
    if (aspect) *aspect = w / h;
    return true;
}

// The largest rectangle of the given shape (width over height) inside a region, centred in it, in the region's own coordinates.
inline DwRect desktopWindowFit(double regionWidth, double regionHeight, double aspect) {
    DwRect r;
    if (!(regionWidth > 0) || !(regionHeight > 0) || !(aspect > 0)) return r;
    if (regionWidth / regionHeight > aspect) { r.h = regionHeight; r.w = regionHeight * aspect; }
    else { r.w = regionWidth; r.h = regionWidth / aspect; }
    r.x = (regionWidth - r.w) * 0.5;
    r.y = (regionHeight - r.h) * 0.5;
    return r;
}

enum class DwScaling : uint8_t { Stretch = 0, None = 1, AspectRatio = 2 };

// Where the picture goes in the swap chain's buffer, in buffer pixels (see the top of the file). An empty rectangle: nothing is drawn.
inline DwRect desktopWindowShown(DwScaling scaling, double bufferWidth, double bufferHeight, double clientWidth, double clientHeight, double aspect) {
    DwRect none;
    if (!(bufferWidth > 0) || !(bufferHeight > 0) || !(clientWidth > 0) || !(clientHeight > 0) || !(aspect > 0)) return none;
    switch (scaling) {
        case DwScaling::None: {
            const double w = clientWidth < bufferWidth ? clientWidth : bufferWidth;
            const double h = clientHeight < bufferHeight ? clientHeight : bufferHeight;
            return desktopWindowFit(w, h, aspect);
        }
        case DwScaling::AspectRatio:
            return desktopWindowFit(bufferWidth, bufferHeight, aspect);
        default: {
            const DwRect f = desktopWindowFit(clientWidth, clientHeight, aspect);
            const double kx = bufferWidth / clientWidth, ky = bufferHeight / clientHeight;
            DwRect r;
            r.x = f.x * kx; r.y = f.y * ky; r.w = f.w * kx; r.h = f.h * ky;
            return r;
        }
    }
}

// A rectangle on whole pixels, inside the buffer. The edges are rounded, not the size, so two rectangles that meet still meet.
inline DwPixelRect desktopWindowSnap(const DwRect& r, int bufferWidth, int bufferHeight) {
    DwPixelRect p;
    if (!(r.w > 0) || !(r.h > 0)) return p;
    auto edge = [](double v, int hi) { const int e = static_cast<int>(std::floor(v + 0.5)); return e < 0 ? 0 : e > hi ? hi : e; };
    const int x0 = edge(r.x, bufferWidth), y0 = edge(r.y, bufferHeight);
    const int x1 = edge(r.x + r.w, bufferWidth), y1 = edge(r.y + r.h, bufferHeight);
    p.x = x0; p.y = y0; p.w = x1 - x0; p.h = y1 - y0;
    return p;
}

// The pixel rectangles of the eyes the mode draws, inside the shown rectangle. Returns how many were written (out[i], eye[i]). One eye fills it. Both eyes: the left
// takes the width its pixels hold out of the sum, the right the rest, each centred vertically at the scale of the sum; equal eyes are two halves.
inline unsigned desktopWindowEyeRects(DesktopWindowMode mode, const DwPixelRect& shown, const DwSize eyes[2], DwPixelRect out[2], unsigned eye[2]) {
    if (!shown.ok()) return 0;
    if (mode == DesktopWindowMode::LeftEye || mode == DesktopWindowMode::RightEye) {
        out[0] = shown; eye[0] = mode == DesktopWindowMode::LeftEye ? 0u : 1u;
        return 1;
    }
    if (mode != DesktopWindowMode::BothEyes || !(eyes[0].w > 0) || !(eyes[1].w > 0)) return 0;
    const double scale = static_cast<double>(shown.w) / (eyes[0].w + eyes[1].w);
    int leftW = static_cast<int>(std::floor(eyes[0].w * scale + 0.5));
    if (leftW < 1) leftW = 1;
    if (leftW > shown.w - 1) leftW = shown.w - 1;
    if (leftW < 1) return 0;
    const int rightW = shown.w - leftW;
    for (unsigned i = 0; i < 2; ++i) {
        int h = static_cast<int>(std::floor(eyes[i].h * scale + 0.5));
        if (h < 1) h = 1;
        if (h > shown.h) h = shown.h;
        out[i].x = shown.x + (i == 0 ? 0 : leftW);
        out[i].w = i == 0 ? leftW : rightW;
        out[i].h = h;
        out[i].y = shown.y + (shown.h - h) / 2;
        eye[i] = i;
    }
    return 2;
}

// The window. The largest client area of the given shape whose outer rectangle (the client plus the frame, frameDx x frameDy, which AdjustWindowRectEx reports)
// fits inside the bound, centred in the bound. ok is false when the bound cannot hold a client of at least one pixel.
struct DwWindowPlan {
    bool ok = false;
    int left = 0, top = 0, right = 0, bottom = 0;
    int clientWidth = 0, clientHeight = 0;
};
inline DwWindowPlan desktopWindowPlan(double aspect, int boundLeft, int boundTop, int boundRight, int boundBottom, int frameDx, int frameDy) {
    DwWindowPlan p;
    const int boundW = boundRight - boundLeft, boundH = boundBottom - boundTop;
    const int availW = boundW - frameDx, availH = boundH - frameDy;
    if (!(aspect > 0) || availW < 1 || availH < 1) return p;
    double ch = availH, cw = ch * aspect;
    if (cw > availW) { cw = availW; ch = cw / aspect; }
    int iw = static_cast<int>(std::floor(cw + 0.5)), ih = static_cast<int>(std::floor(ch + 0.5));
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    if (iw > availW) iw = availW;
    if (ih > availH) ih = availH;
    const int outerW = iw + frameDx, outerH = ih + frameDy;
    p.left = boundLeft + (boundW - outerW) / 2;
    p.top = boundTop + (boundH - outerH) / 2;
    p.right = p.left + outerW;
    p.bottom = p.top + outerH;
    p.clientWidth = iw;
    p.clientHeight = ih;
    p.ok = true;
    return p;
}

// Two window rectangles {left, top, right, bottom} are the same place when no edge differs by more than tolerance pixels.
inline bool desktopWindowRectsNear(const int a[4], const int b[4], int tolerance) {
    for (int i = 0; i < 4; ++i) {
        const int d = a[i] - b[i];
        if ((d < 0 ? -d : d) > tolerance) return false;
    }
    return true;
}

// ---- the window is an ordinary, movable, resizable one (round 2) ----
// While the feature is active the window is framed and sizable whatever the game had (a borderless popup, or Elite's own windowed style): a caption, a system menu and a
// thick frame, and no minimize box (a minimized window could stop Elite rendering) and no maximize box. The bits are the Win32 values, written out so this header needs no
// windows.h: WS_POPUP, WS_MAXIMIZEBOX, WS_MINIMIZEBOX, WS_MAXIMIZE, WS_MINIMIZE, WS_CAPTION, WS_SYSMENU, WS_THICKFRAME. WS_VISIBLE, WS_CLIPSIBLINGS and WS_CLIPCHILDREN stay
// as the game had them. 0x94020000 (the borderless popup of the first flight) becomes 0x14CC0000.
constexpr uint32_t kDwWsPopup = 0x80000000u, kDwWsMaximizeBox = 0x00010000u, kDwWsMinimizeBox = 0x00020000u, kDwWsMaximize = 0x01000000u, kDwWsMinimize = 0x20000000u,
                   kDwWsCaption = 0x00C00000u, kDwWsSysMenu = 0x00080000u, kDwWsThickFrame = 0x00040000u;
inline uint32_t desktopWindowFramedStyle(uint32_t style) {
    return (style & ~(kDwWsPopup | kDwWsMaximizeBox | kDwWsMinimizeBox | kDwWsMaximize | kDwWsMinimize)) | kDwWsCaption | kDwWsSysMenu | kDwWsThickFrame;
}
// Is a window's style the one EDVR put on it? Only the bits EDVR manages are compared: a state bit (visible, maximized) or a clip bit moving is not the game re-applying its mode.
inline bool desktopWindowStyleIsOurs(uint32_t style, uint32_t ours) {
    constexpr uint32_t managed = kDwWsPopup | kDwWsMaximizeBox | kDwWsMinimizeBox | kDwWsCaption | kDwWsSysMenu | kDwWsThickFrame;
    return (style & managed) == (ours & managed);
}

// The client's shape is the picture's when neither axis is off by more than the tolerance (pixels) against what the other axis implies.
inline bool desktopWindowAspectOff(int clientW, int clientH, double aspect, int tolerance) {
    if (!(aspect > 0) || clientW < 1 || clientH < 1) return false;
    const double wantW = clientH * aspect, wantH = clientW / aspect;
    return std::fabs(clientW - wantW) > tolerance || std::fabs(clientH - wantH) > tolerance;
}

struct DwArea {
    int left = 0, top = 0, right = 0, bottom = 0;
};
// A window rectangle {left, top, right, bottom} lies wholly inside an area (a work area).
inline bool desktopWindowRectInside(const int r[4], const DwArea& a) { return r[0] >= a.left && r[1] >= a.top && r[2] <= a.right && r[3] <= a.bottom; }
// A window of this outer size cannot fit the area, whatever its position.
inline bool desktopWindowTooLarge(int outerW, int outerH, const DwArea& a) { return outerW > a.right - a.left || outerH > a.bottom - a.top; }

constexpr int kDesktopWindowMinClientHeight = 240;

// The user owns the window's position and size; EDVR keeps only the shape. When the client is off the picture's shape (or the window is larger than the work area) this
// gives the window to ask for: keep the dimension the user changed more, relative to the last settled client size (the width when there is none), set the other from the
// shape, keep the window's top-left, then clamp: shrink to the work area if it is larger, hold a minimum client height, and slide the window inside the work area. The frame
// (the window's size less its client's) is measured by the caller.
struct DwCorrection {
    bool ok = false, keptHeight = false;
    int left = 0, top = 0, right = 0, bottom = 0;
    int clientWidth = 0, clientHeight = 0;
};
inline DwCorrection desktopWindowCorrect(double aspect, int winLeft, int winTop, int clientW, int clientH, int settledW, int settledH, int frameDx, int frameDy,
                                         const DwArea& work, int minClientH = kDesktopWindowMinClientHeight) {
    DwCorrection c;
    if (!(aspect > 0) || clientW < 1 || clientH < 1) return c;
    bool keepHeight = false;
    if (settledW > 0 && settledH > 0) {
        const double relW = std::fabs(static_cast<double>(clientW - settledW)) / settledW, relH = std::fabs(static_cast<double>(clientH - settledH)) / settledH;
        keepHeight = relH > relW;
    }
    auto rnd = [](double v) { return static_cast<int>(std::floor(v + 0.5)); };
    int cw = keepHeight ? rnd(clientH * aspect) : clientW, ch = keepHeight ? clientH : rnd(clientW / aspect);
    const int availW = (work.right - work.left) - frameDx, availH = (work.bottom - work.top) - frameDy;
    if (availW >= 1 && availH >= 1 && (cw > availW || ch > availH)) {
        if (availW / aspect <= availH) { cw = availW; ch = rnd(cw / aspect); }
        else { ch = availH; cw = rnd(ch * aspect); }
        if (cw > availW) cw = availW;
        if (ch > availH) ch = availH;
    }
    if (ch < minClientH) { ch = minClientH; cw = rnd(ch * aspect); }
    if (cw < 1) cw = 1;
    const int outerW = cw + frameDx, outerH = ch + frameDy;
    int left = winLeft, top = winTop;
    if (left + outerW > work.right) left = work.right - outerW;
    if (left < work.left) left = work.left;
    if (top + outerH > work.bottom) top = work.bottom - outerH;
    if (top < work.top) top = work.top;
    c.ok = true;
    c.keptHeight = keepHeight;
    c.left = left; c.top = top; c.right = left + outerW; c.bottom = top + outerH;
    c.clientWidth = cw; c.clientHeight = ch;
    return c;
}

// ---- the cursor (round 3) ----
// While the desktop window is active the cursor must be free to leave Elite's window. The executable's ClipCursor, SetCursorPos, GetCursorPos and ShowCursor are watched
// (desktop_window_cursor.cpp); only ClipCursor with a rectangle is changed, and only while the window is active: it is released instead. SetCursorPos is the mouse-look's
// re-centre if Elite steers by the cursor's movement, so it is counted and never touched.
enum class DwClipVerdict : uint8_t { Pass, PassCounted, Release };
inline DwClipVerdict desktopWindowClipVerdict(bool active, bool hasRect) {
    if (!active) return DwClipVerdict::Pass;
    return hasRect ? DwClipVerdict::Release : DwClipVerdict::PassCounted;
}
// A SetCursorPos target within two pixels of the client's centre (screen coordinates): the mouse-look's re-centre.
inline bool desktopWindowAtCentre(int x, int y, int centreX, int centreY, int tolerance = 2) {
    const int dx = x - centreX, dy = y - centreY;
    return (dx < 0 ? -dx : dx) <= tolerance && (dy < 0 ? -dy : dy) <= tolerance;
}
// DirectInput's cooperative level words: DISCL_EXCLUSIVE 1, NONEXCLUSIVE 2, FOREGROUND 4, BACKGROUND 8, NOWINKEY 0x10.
inline int desktopWindowCooperativeWords(char* out, size_t size, uint32_t flags) {
    const bool exclusive = (flags & 1u) != 0, shared = (flags & 2u) != 0, foreground = (flags & 4u) != 0, background = (flags & 8u) != 0;
    const char* level = exclusive && !shared ? "exclusive" : shared && !exclusive ? "nonexclusive" : exclusive ? "exclusive and nonexclusive" : "neither exclusive nor nonexclusive";
    const char* where = foreground && !background ? "foreground" : background && !foreground ? "background" : foreground ? "foreground and background" : "neither foreground nor background";
    return std::snprintf(out, size, "%s, %s%s", level, where, (flags & 0x10u) ? ", no windows key" : "");
}
// The cursor line, every 10 s while active, after the census line (the strings are the caller's: "WxH at x,y" or "none", and "show, display count N" or "none").
inline int desktopWindowCursorLine(char* out, size_t size, unsigned released, const char* lastRect, unsigned passed, unsigned setPos, unsigned setPosCentre, unsigned getPos,
                                   unsigned showCalls, const char* lastShow) {
    return std::snprintf(out, size, "desktop window: cursor (10 s): clip released %u (last rect %s), clip passed %u, setpos %u (at the client centre %u), getpos %u, showcursor %u (last %s)",
                         released, lastRect, passed, setPos, setPosCentre, getPos, showCalls, lastShow);
}

// The formats of one draw. The shader view reads the bytes the eye holds as they are: a typeless source gets the UNORM view of its family, a typed one its own
// format; a typed _SRGB source is decoded by the view and encoded again by an _SRGB target on an 8-bit buffer. The target is the buffer's own bit depth.
// Anything else is a refusal with its reason (said once; nothing is skipped and nothing is drawn).
struct DwFormats {
    bool ok = false;
    bool srgb = false;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT target = DXGI_FORMAT_UNKNOWN;
    char reason[96] = {};
};
inline DwFormats desktopWindowFormats(DXGI_FORMAT source, DXGI_FORMAT buffer) {
    DwFormats f;
    switch (source) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: f.view = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: f.view = source; f.srgb = true; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: f.view = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: f.view = source; f.srgb = true; break;
        default:
            std::snprintf(f.reason, sizeof f.reason, "the eye image's format (DXGI %d) is not an 8-bit RGBA or BGRA one", static_cast<int>(source));
            return f;
    }
    switch (buffer) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            f.target = f.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            f.target = f.srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
            if (f.srgb) {
                std::snprintf(f.reason, sizeof f.reason, "an sRGB eye image into a 10-bit back buffer would not keep its bytes");
                return f;
            }
            f.target = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        default:
            std::snprintf(f.reason, sizeof f.reason, "the swap chain's buffer format (DXGI %d) is not an 8-bit RGBA or BGRA one or R10G10B10A2", static_cast<int>(buffer));
            return f;
    }
    f.ok = true;
    return f;
}

// The two lines the graphics half writes that tools\edvr_log.py --health reads (its self-test renders its fixtures from these literals). The first is said when the
// setting is first read and at each change, the second every 10 s while the window is being drawn.
inline int desktopWindowSettingLine(char* out, size_t size, const char* value, const char* was) {
    if (was) return std::snprintf(out, size, "desktop window: setting %s (was %s)", value, was);
    return std::snprintf(out, size, "desktop window: setting %s", value);
}
// Said when an activation ends: why, the session's totals so far, and what became of the window ("restored to ...", "left alone (...)", "no reshape was made").
inline int desktopWindowStoppedLine(char* out, size_t size, const char* reason, unsigned long long drawn, unsigned long long seen, const char* window) {
    return std::snprintf(out, size, "desktop window: stopped (%s); drawn=%llu of %llu Presents; window %s", reason, drawn, seen, window);
}
inline int desktopWindowStatsLine(char* out, size_t size, unsigned long long drawn, unsigned long long seen, unsigned long long skipped,
                                  unsigned long long left, unsigned long long right, unsigned long long stale, int clientW, int clientH,
                                  unsigned chainW, unsigned chainH, unsigned moves, unsigned resizes) {
    return std::snprintf(out, size,
                         "desktop window: 10 s: drawn=%llu seen=%llu skipped_draws=%llu eyes_L=%llu eyes_R=%llu stale=%llu client=%dx%d swap_chain=%ux%u moves=%u resizes=%u",
                         drawn, seen, skipped, left, right, stale, clientW, clientH, chainW, chainH, moves, resizes);
}

}  // namespace edvr
