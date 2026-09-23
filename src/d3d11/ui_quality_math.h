// fix.ui_quality's surfaces half, the pure arithmetic: the factor a surface
// grows by, the game's internal render resolution derived from the runtime's
// recommendation and HMD Quality, and the table of interface-surface ratios
// a create is matched against. Header-only, no device, no Config, no Log:
// ui_surfaces.cpp and tools/ui_quality_test compile exactly this.
// (Absorbed 2026-09-23 from fix.hud_quality's hud_quality_math.*, which the
// one key replaced.)
//
// WHY RATIOS. Elite draws the cockpit's panels -- vector, text, icons -- into
// offscreen interface surfaces whose size is a fixed fraction of its
// internal render resolution (fss_res.h's census: 908x1361 at 4340x4284 and
// 1363x2042 at 6510x6426, the same fraction to four figures), so HMD
// Quality 0.65 rasterises them at 0.65 of what 1.0 would. A create whose
// size is one of those fractions of the internal resolution is made at the
// size it would have at the key's target instead; fss_res.cpp's viewport
// and scissor backstops make the game's draws fill it.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "../openxr/game_render_size.h"  // gameFacingDimension: what the game is told

namespace edvr {

// factor = target / HMD Quality, capped at 4 (sixteen times the pixels is
// already far past any panel's need). False -- nothing to do -- when the key
// is off, HMD Quality is unknown, or the factor does not exceed 1 by more
// than 1% (HMD Quality already at or above the target: the game's own
// surface is already that size or bigger).
inline bool uiQualityFactor(float target, float hmd, float* factorOut) {
    if (!(target > 0.0f) || !(hmd > 0.0f) || !std::isfinite(hmd)) return false;
    float factor = target / hmd;
    if (!(factor > 1.01f)) return false;
    if (factor > 4.0f) factor = 4.0f;
    if (factorOut) *factorOut = factor;
    return true;
}

// round(v * factor): a grown texture's width or height.
inline uint32_t uiQualityRoundDim(uint32_t v, float factor) {
    return static_cast<uint32_t>(static_cast<float>(v) * factor + 0.5f);
}

// ------------------------------------------------ the internal resolution --

// One axis of the game's internal render size: the runtime's game-facing
// recommendation (even, game_render_size.h) times HMD Quality, which Elite
// truncates to whole pixels. 3070 x 0.65 = 1995; 3032 x 0.65 = 1970;
// 3070 x 0.5 = 1535. 0 when either input is unknown.
inline uint32_t uiQualityInternalDim(uint32_t recommended, float hmd) {
    if (!recommended || recommended > 16384u || !(hmd > 0.0f) || !std::isfinite(hmd)) return 0;
    const float v = static_cast<float>(recommended) * hmd;
    if (!(v >= 1.0f) || v > 16384.0f) return 0;
    return static_cast<uint32_t>(v);
}

// The recommendation the game is given before the runtime's first frame
// says otherwise: the larger eye's swapchain size on each axis (the runtime
// answers GetRecommendedRenderTargetSize with the max over eyes), made even
// the way the runtime makes every game-facing size.
inline void uiQualityRecommendedFromEyes(const uint32_t w[2], const uint32_t h[2],
                                         uint32_t* outW, uint32_t* outH) {
    const uint32_t mw = w[0] > w[1] ? w[0] : w[1];
    const uint32_t mh = h[0] > h[1] ? h[0] : h[1];
    if (outW) *outW = openxr::gameFacingDimension(mw);
    if (outH) *outH = openxr::gameFacingDimension(mh);
}

// ---------------------------------------------------------- the ratios --

// A surface's size over the internal size, in ten-thousandths (four
// significant figures); 0 with nothing to divide by.
inline uint32_t uiQualityRatioX10000(uint32_t dim, uint32_t internalDim) {
    if (!internalDim) return 0;
    return static_cast<uint32_t>(static_cast<double>(dim) * 10000.0 /
                                     static_cast<double>(internalDim) +
                                 0.5);
}

inline bool uiQualityRatioNear(uint32_t a, uint32_t b, uint32_t tolerance) {
    return (a > b ? a - b : b - a) <= tolerance;
}

// Two roundings of the same fraction at two resolutions differ by a few
// parts in 10000 (the census's own spread is 6 on the worst axis); 10 takes
// that without joining two different panels (the nearest two seeds differ
// by 5 on one axis and 503 on the other).
constexpr uint32_t kUiQualityRatioTolerance = 10;

// The shape an interface surface has: smaller than the internal size on
// both axes (the eye targets and the 3840x2160 2D screen are not), not a
// power of two on either (atlases, icon caches and shadow maps are), and not
// a sliver (a 256x1 lookup strip, a 1x1 target).
inline bool uiQualityCandidateShape(uint32_t w, uint32_t h, uint32_t internalW,
                                    uint32_t internalH) {
    if (w < 16 || h < 16 || !internalW || !internalH) return false;
    if (w >= internalW || h >= internalH) return false;
    if ((w & (w - 1)) == 0 || (h & (h - 1)) == 0) return false;
    return true;
}

enum class UiQualityOrigin : uint8_t {
    kNone = 0,
    kCensus,   // compiled in: the census below
    kLearned,  // this rig: the GUI renderer's own draws were seen landing in
               // a surface of this ratio (ui_depth's classifier), saved to
               // ui_quality_ratios.txt beside the logs
};

struct UiQualityRatio {
    uint32_t w = 0, h = 0;  // ten-thousandths of the internal width, height
    UiQualityOrigin origin = UiQualityOrigin::kNone;
};

// THE CENSUS (2026-09-23): every census frame in the 35 flight logs since
// 2026-08 that carries GUI draws (vs 666EF0C4C616F67E vector,
// 1012E00B3CB44469 text, A3E5D3FCBC1165F8 icon) into an offscreen target,
// grouped by ratio to that frame's eye viewport. These five were seen at two
// or more eye widths more than 1% apart, agreeing within the tolerance, all
// DXGI format 27 and all three families drawn into each: panels that scale
// with the internal resolution, drawn by the GUI renderer. Surfaces that
// held one pixel size across a resolution change, powers of two, eye-sized
// and larger-than-eye targets are not here.
struct UiQualitySeed {
    uint32_t w, h;
    const char* evidence;
};
constexpr UiQualitySeed kUiQualitySeeds[] = {
    {2092, 3175, "539x807 at 2576x2544, 589x883 at 2818x2784, 1135x1701 at 5424x5356 (6 logs)"},
    {4187, 2649, "1078x674 at 2576x2544, 2271x1419 at 5424x5356 (5 logs)"},
    {4498, 3726, "1267x1036 at 2818x2784, 2440x1996 at 5424x5356 (5 logs)"},
    {6280, 1367, "1769x380 at 2818x2784, 3407x732 at 5424x5356 (5 logs)"},
    {6783, 1362, "1354x290 at 1996x2121, 1400x300 at 2064x2208 (5 logs)"},
};
constexpr uint32_t kUiQualitySeedCount = sizeof(kUiQualitySeeds) / sizeof(kUiQualitySeeds[0]);

// The entry whose ratio is within the tolerance on both axes, or -1.
inline int uiQualityRatioFind(const UiQualityRatio* table, uint32_t n, uint32_t rw, uint32_t rh,
                              uint32_t tolerance = kUiQualityRatioTolerance) {
    if (!table) return -1;
    for (uint32_t i = 0; i < n; ++i) {
        if (uiQualityRatioNear(rw, table[i].w, tolerance) &&
            uiQualityRatioNear(rh, table[i].h, tolerance)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The census into a table, first: returns the count written (at most max).
inline uint32_t uiQualitySeedTable(UiQualityRatio* table, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kUiQualitySeedCount && n < max; ++i) {
        table[n].w = kUiQualitySeeds[i].w;
        table[n].h = kUiQualitySeeds[i].h;
        table[n].origin = UiQualityOrigin::kCensus;
        ++n;
    }
    return n;
}

// Appends a learned ratio unless one within the tolerance is already there
// (census or learned). True only when it was added.
inline bool uiQualityLearn(UiQualityRatio* table, uint32_t* n, uint32_t max, uint32_t rw,
                           uint32_t rh) {
    if (!table || !n || !rw || !rh || rw >= 10000 || rh >= 10000) return false;
    if (uiQualityRatioFind(table, *n, rw, rh) >= 0) return false;
    if (*n >= max) return false;
    table[*n].w = rw;
    table[*n].h = rh;
    table[*n].origin = UiQualityOrigin::kLearned;
    ++*n;
    return true;
}

// ui_quality_ratios.txt: one learned ratio a line, "W H" in ten-thousandths;
// '#' starts a comment line; anything malformed is skipped a line at a time,
// never trusted. Appends to the table through uiQualityLearn (so a line that
// repeats the census or another line adds nothing); returns lines taken.
inline uint32_t uiQualityParseRatios(const char* text, UiQualityRatio* table, uint32_t* n,
                                     uint32_t max) {
    if (!text || !table || !n) return 0;
    uint32_t taken = 0;
    const char* p = text;
    while (*p) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        const char* lineEnd = p;
        while (*lineEnd && *lineEnd != '\n') ++lineEnd;
        if (*p != '#') {
            char* end = nullptr;
            const unsigned long w = std::strtoul(p, &end, 10);
            const char* q = end;
            const unsigned long h = (end && end != p) ? std::strtoul(q, &end, 10) : 0;
            const bool twoNumbers = end && end != q && w > 0 && h > 0 && w < 10000 && h < 10000;
            // Nothing but spaces may follow the second number.
            const char* rest = end;
            while (twoNumbers && rest < lineEnd && (*rest == ' ' || *rest == '\t' || *rest == '\r'))
                ++rest;
            if (twoNumbers && rest == lineEnd &&
                uiQualityLearn(table, n, max, static_cast<uint32_t>(w), static_cast<uint32_t>(h))) {
                ++taken;
            }
        }
        p = lineEnd;
    }
    return taken;
}

}  // namespace edvr
