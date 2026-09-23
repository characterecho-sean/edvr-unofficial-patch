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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
               // surfaces of this ratio (ui_depth's classifier: it is UI) made
               // at two internal resolutions more than 1% apart (it scales),
               // kept in ui_quality_ratios.txt beside the logs
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

// ------------------------------------------------------------- learning --
//
// Two questions, and both are needed: IS IT UI -- ui_depth's classifier saw
// the GUI renderer's draws land in the surface -- and DOES IT SCALE -- the
// same ratio at a second internal resolution more than 1% away. A GUI
// surface of fixed pixel size (the census's 666x998: one size at two
// resolutions 7.5% apart) answers yes to the first and no to the second,
// and enlarging it in every later session would be wrong (review P2-1). So
// a GUI-drawn surface is first PENDING, with the basis it was made against;
// the same ratio at a basis more than 1% different PROMOTES it to the
// table; the same pixel size at such a basis marks the size FIXED, for good.

struct UiQualityPending {
    uint32_t rw = 0, rh = 0;  // its ratio against the basis it was made at
    uint32_t w = 0, h = 0;    // its pixel size
    uint32_t bw = 0, bh = 0;  // that basis, the internal render size
};
struct UiQualitySize {
    uint32_t w = 0, h = 0;
};

// Two bases are different resolutions when either axis differs by more than
// 1% (and a pixel, for the rounding).
inline bool uiQualityBasisDiffers(uint32_t aw, uint32_t ah, uint32_t bw, uint32_t bh) {
    return !uiQualityRatioNear(aw, bw, aw / 100 + 1) || !uiQualityRatioNear(ah, bh, ah / 100 + 1);
}

enum class UiQualityLearn : uint8_t {
    kIgnored,     // not a surface's shape, or its ratio is already on the table
    kFixed,       // a size already known not to scale
    kPending,     // recorded: waiting for the same ratio at another resolution
    kSameBasis,   // already pending at this resolution: no new evidence
    kPromoted,    // the same ratio at a second resolution: on the table now
    kNotScaling,  // the same pixel size at a second resolution: fixed, for good
    kFull,        // no room to record it
};

struct UiQualityLearning {
    static constexpr uint32_t kMaxRatios = 32, kMaxPending = 16, kMaxFixed = 16;
    UiQualityRatio table[kMaxRatios];
    uint32_t n = 0;
    UiQualityPending pending[kMaxPending];
    uint32_t np = 0;
    UiQualitySize fixed[kMaxFixed];
    uint32_t nf = 0;

    // The census, and nothing learned.
    void reset() {
        n = uiQualitySeedTable(table, kMaxRatios);
        np = nf = 0;
    }
    uint32_t learnedCount() const {
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; ++i) k += table[i].origin == UiQualityOrigin::kLearned ? 1u : 0u;
        return k;
    }
    bool isFixed(uint32_t w, uint32_t h) const {
        for (uint32_t i = 0; i < nf; ++i)
            if (fixed[i].w == w && fixed[i].h == h) return true;
        return false;
    }
    bool addFixed(uint32_t w, uint32_t h) {
        if (!w || !h || isFixed(w, h) || nf >= kMaxFixed) return false;
        fixed[nf++] = {w, h};
        return true;
    }
    void dropPending(uint32_t i) {
        if (i < np) pending[i] = pending[--np];
    }
    bool addPending(const UiQualityPending& p) {
        if (np >= kMaxPending || !p.rw || !p.rh || !p.w || !p.h || !p.bw || !p.bh) return false;
        pending[np++] = p;
        return true;
    }

    // One sighting of a GUI-drawn surface of w x h made against bw x bh.
    // *outW / *outH: the ratio promoted, on kPromoted.
    UiQualityLearn observe(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh,
                           uint32_t* outW = nullptr, uint32_t* outH = nullptr) {
        if (!uiQualityCandidateShape(w, h, bw, bh)) return UiQualityLearn::kIgnored;
        if (isFixed(w, h)) return UiQualityLearn::kFixed;
        const uint32_t rw = uiQualityRatioX10000(w, bw), rh = uiQualityRatioX10000(h, bh);
        if (uiQualityRatioFind(table, n, rw, rh) >= 0) return UiQualityLearn::kIgnored;
        // The same pixel size at another resolution: it does not scale.
        for (uint32_t i = 0; i < np; ++i) {
            if (pending[i].w != w || pending[i].h != h ||
                !uiQualityBasisDiffers(pending[i].bw, pending[i].bh, bw, bh)) {
                continue;
            }
            addFixed(w, h);
            for (uint32_t k = np; k-- > 0;)
                if (pending[k].w == w && pending[k].h == h) dropPending(k);
            return UiQualityLearn::kNotScaling;
        }
        // The same ratio: at another resolution it scales; at this one it
        // says nothing new.
        for (uint32_t i = 0; i < np; ++i) {
            const UiQualityPending p = pending[i];
            if (!uiQualityRatioNear(p.rw, rw, kUiQualityRatioTolerance) ||
                !uiQualityRatioNear(p.rh, rh, kUiQualityRatioTolerance)) {
                continue;
            }
            if (!uiQualityBasisDiffers(p.bw, p.bh, bw, bh)) return UiQualityLearn::kSameBasis;
            const uint32_t lw = (p.rw + rw + 1) / 2, lh = (p.rh + rh + 1) / 2;
            dropPending(i);
            if (!uiQualityLearn(table, &n, kMaxRatios, lw, lh)) return UiQualityLearn::kFull;
            if (outW) *outW = lw;
            if (outH) *outH = lh;
            return UiQualityLearn::kPromoted;
        }
        return addPending({rw, rh, w, h, bw, bh}) ? UiQualityLearn::kPending : UiQualityLearn::kFull;
    }
};

// ui_quality_ratios.txt, one fact a line ('#' starts a comment):
//   learned RW RH                -- a ratio on the table
//   pending RW RH W H BW BH      -- a GUI surface waiting for a second resolution
//   fixed W H                    -- a GUI surface size that does not scale
// Ratios in ten-thousandths. Anything else -- malformed, out of range, the
// first build's bare "W H" lines, which were learned without the scaling
// test -- is skipped a line at a time, never trusted. Appends to `l` (after
// reset(): the census first); returns the lines taken.
inline uint32_t uiQualityParseLearning(const char* text, UiQualityLearning& l) {
    if (!text) return 0;
    uint32_t taken = 0;
    const char* p = text;
    while (*p) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        const char* lineEnd = p;
        while (*lineEnd && *lineEnd != '\n') ++lineEnd;
        if (*p != '#') {
            char word[12] = {};
            size_t k = 0;
            while (p + k < lineEnd && k < sizeof(word) - 1 && p[k] >= 'a' && p[k] <= 'z') {
                word[k] = p[k];
                ++k;
            }
            unsigned long v[6] = {};
            int count = 0;
            const char* q = p + k;
            bool clean = k > 0 && (q < lineEnd && (*q == ' ' || *q == '\t'));
            while (clean && count < 6) {
                while (q < lineEnd && (*q == ' ' || *q == '\t')) ++q;
                if (q >= lineEnd || *q == '\r') break;
                char* end = nullptr;
                v[count] = std::strtoul(q, &end, 10);
                if (end == q || end > lineEnd) {
                    clean = false;
                    break;
                }
                ++count;
                q = end;
            }
            while (clean && q < lineEnd && (*q == ' ' || *q == '\t' || *q == '\r')) ++q;
            clean = clean && q == lineEnd;
            auto ratio = [](unsigned long x) { return x > 0 && x < 10000; };
            auto pixels = [](unsigned long x) { return x > 0 && x <= 16384; };
            bool ok = false;
            if (clean && std::strcmp(word, "learned") == 0 && count == 2 && ratio(v[0]) && ratio(v[1])) {
                ok = uiQualityLearn(l.table, &l.n, UiQualityLearning::kMaxRatios,
                                    static_cast<uint32_t>(v[0]), static_cast<uint32_t>(v[1]));
            } else if (clean && std::strcmp(word, "pending") == 0 && count == 6 && ratio(v[0]) &&
                       ratio(v[1]) && pixels(v[2]) && pixels(v[3]) && pixels(v[4]) && pixels(v[5])) {
                ok = l.addPending({static_cast<uint32_t>(v[0]), static_cast<uint32_t>(v[1]),
                                   static_cast<uint32_t>(v[2]), static_cast<uint32_t>(v[3]),
                                   static_cast<uint32_t>(v[4]), static_cast<uint32_t>(v[5])});
            } else if (clean && std::strcmp(word, "fixed") == 0 && count == 2 && pixels(v[0]) &&
                       pixels(v[1])) {
                ok = l.addFixed(static_cast<uint32_t>(v[0]), static_cast<uint32_t>(v[1]));
            }
            if (ok) ++taken;
        }
        p = lineEnd;
    }
    return taken;
}

// The file's text for `l`: the learned ratios, the pending sightings and the
// fixed sizes (the census is compiled in and not written).
inline std::string uiQualityFormatLearning(const UiQualityLearning& l) {
    std::string s =
        "# fix.ui_quality: interface surfaces on this rig, in ten-thousandths of the internal\n"
        "# render size. learned: GUI-drawn and seen to scale; pending: GUI-drawn, waiting for\n"
        "# a second resolution; fixed: GUI-drawn at one size across resolutions. Delete to forget.\n";
    char line[96];
    for (uint32_t i = 0; i < l.n; ++i) {
        if (l.table[i].origin != UiQualityOrigin::kLearned) continue;
        std::snprintf(line, sizeof(line), "learned %u %u\n", l.table[i].w, l.table[i].h);
        s += line;
    }
    for (uint32_t i = 0; i < l.np; ++i) {
        const UiQualityPending& p = l.pending[i];
        std::snprintf(line, sizeof(line), "pending %u %u %u %u %u %u\n", p.rw, p.rh, p.w, p.h, p.bw,
                      p.bh);
        s += line;
    }
    for (uint32_t i = 0; i < l.nf; ++i) {
        std::snprintf(line, sizeof(line), "fixed %u %u\n", l.fixed[i].w, l.fixed[i].h);
        s += line;
    }
    return s;
}

// ------------------------------------------------------------ the pair --
//
// A colour target and its depth partner are two creates of one size, back
// to back. If the basis moved between them -- the submitted size arriving,
// HMD Quality re-read -- one could be enlarged and the other not, and D3D11
// refuses to bind a render target and a depth target of different sizes
// (review P3-9). So each decision on a surface-shaped create is remembered
// by the size the game asked for, and a create of that size within
// kUiQualityMemoMs gets the same decision.
struct UiQualityDecision {
    uint32_t ow = 0, oh = 0;  // the size the game asked for
    uint32_t nw = 0, nh = 0;  // what it was made at (ow x oh: left alone)
    float factor = 1.0f;
    char family = 0;
    UiQualityOrigin origin = UiQualityOrigin::kNone;
    uint64_t ms = 0;          // when it was decided (0: an empty slot)
};
constexpr uint64_t kUiQualityMemoMs = 2000;
struct UiQualityMemo {
    UiQualityDecision slot[8];
    uint32_t next = 0;
    const UiQualityDecision* find(uint32_t ow, uint32_t oh, uint64_t nowMs) const {
        for (const UiQualityDecision& d : slot) {
            if (d.ms && d.ow == ow && d.oh == oh && nowMs >= d.ms && nowMs - d.ms <= kUiQualityMemoMs)
                return &d;
        }
        return nullptr;
    }
    void put(const UiQualityDecision& d) {
        slot[next] = d;
        next = (next + 1) % 8;
    }
    void clear() {
        for (UiQualityDecision& d : slot) d = UiQualityDecision{};
        next = 0;
    }
};

}  // namespace edvr
