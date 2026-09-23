// fix.ui_quality's surfaces half, the pure arithmetic: the factor a surface
// grows by, the game's internal render resolution derived from the runtime's
// recommendation and HMD Quality, the rule an interface panel's size follows,
// the table of panels a create is matched against, learning and the pair
// memo. Header-only, no device, no Config, no Log: ui_surfaces.cpp and
// tools/ui_quality_test compile exactly this.
//
// THE RULE (fitted 2026-09-23 from every September flight log: 33 distinct
// render states in 126 log-states, three headset shapes, FOV trims, the cull
// guard's widened frusta, HMD Quality 0.5 to 1.27). Elite sizes a cockpit
// interface panel -- both
// axes, one scale -- as a constant times
//
//     U = W / (2 tan(vFOV / 2)),   vFOV = atan(tanUp) + atan(tanDown),
//
// W the render width in pixels and vFOV the game's vertical field of view
// (the frustum's up and down half-angles added, so an asymmetric frustum
// counts as the symmetric one of the same angle). Every panel made in the
// state it was first seen fits within 0.28%, the median 0.04%. The first
// build's rule -- width over the render width, height over the render
// height -- holds only while the frustum stays the Pimax's untrimmed one: on
// Sean's FOV-trimmed eye (1597x1835, vFOV 99.3 degrees) the panels are 7.9%
// off it, on a Quest 3 8.1%, under the cull guard's symmetric frusta up to
// 90% -- which is why the 2026-09-23 11:49 flight matched nothing.
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
// surface is already that size or bigger). At a fixed frustum U is linear in
// W, so the panel grows by exactly this to the size it has at the target.
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

// --------------------------------------------------------------- the rule --

// 2 tan(vFOV / 2) for a frustum's up and down tangents (their magnitudes:
// the sign conventions differ between OpenVR and OpenXR, the angle does
// not). Pimax Crystal Super untrimmed (1.2648, 1.2648): 2.5296; trimmed 2
// degrees top and bottom (1.1779, 1.1779): 2.3558; Quest 3 (0.9657, 1.4281):
// 2.3417 (not the tangents' sum, 2.3938). 0 when either is not usable.
inline float uiQualityFovTangent(float up, float down) {
    up = std::fabs(up);
    down = std::fabs(down);
    if (!(up > 0.01f) || !(down > 0.01f) || !(up < 100.0f) || !(down < 100.0f)) return 0.0f;
    return 2.0f * std::tan((std::atan(up) + std::atan(down)) * 0.5f);
}

// The render state a create is judged in: the render size and the vertical
// field of view as 2 tan(vFOV / 2).
struct UiQualityBasis {
    uint32_t W = 0, H = 0;
    float T = 0.0f;
    bool valid() const { return W && H && T > 0.0f; }
    double unit() const { return valid() ? static_cast<double>(W) / T : 0.0; }  // U
};

// A panel dimension over U, in ten-thousandths; 0 with no basis.
inline uint32_t uiQualityPanelX10000(uint32_t dim, const UiQualityBasis& b) {
    const double u = b.unit();
    if (!(u > 0.0)) return 0;
    return static_cast<uint32_t>(static_cast<double>(dim) * 10000.0 / u + 0.5);
}

// Within 0.5% of each other (and a unit for the rounding). The rule's worst
// residual over the September logs is 0.28%; the nearest two panels (2a and
// 2b below, the same aspect) are 3.2% apart.
inline bool uiQualityRatioNear(uint32_t a, uint32_t b) {
    const uint32_t d = a > b ? a - b : b - a;
    return static_cast<uint64_t>(d) * 1000u <= static_cast<uint64_t>(b) * 5u + 1000u;
}

// Two bases are different resolutions when their units differ by more than
// 1% -- an HMD Quality change, a FOV trim (at HMD Quality 0.65 Sean's trim
// takes U from 788.7 to 677.9), another headset.
inline bool uiQualityBasisDiffers(const UiQualityBasis& a, const UiQualityBasis& b) {
    const double ua = a.unit(), ub = b.unit();
    if (!(ua > 0.0) || !(ub > 0.0)) return false;
    return std::fabs(ua - ub) > 0.01 * ua;
}

// The shape an interface surface has: smaller than the render size on both
// axes (the eye targets and the 3840x2160 2D screen are not), not a power of
// two on either (atlases, icon caches and shadow maps are), and not a sliver
// (a 256x1 lookup strip, a 1x1 target).
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
               // surfaces of this ratio (ui_depth's classifier: it is UI) at
               // two units more than 1% apart (it scales), kept in
               // ui_quality_panels.txt beside the logs
};

struct UiQualityRatio {
    uint32_t w = 0, h = 0;  // ten-thousandths of U
    UiQualityOrigin origin = UiQualityOrigin::kNone;
};

// THE CENSUS (2026-09-23): the medians of every first sighting of the five
// cockpit panels in the September logs, each in the render state it was
// made in (the depth probe names their depth partners; the classifier the
// GUI drawing into them). 2a and 2b share an aspect and are 3.2% apart; the
// first build's fifth seed (1354x290 on the Quest) is panel 4.
struct UiQualitySeed {
    uint32_t w, h;
    const char* evidence;
};
constexpr UiQualitySeed kUiQualitySeeds[] = {
    {5291, 7929, "panel 1: 539x807 @2576x2544 Pimax, 358x537 @1597x1835 trimmed, 451x676 @1996x2121 Quest 3, 666x998 @2307x1652 (130 sightings)"},
    {10576, 6608, "panel 2a: 1078x674 @2576x2544 Pimax, 717x448 @1597x1835 trimmed"},
    {10919, 6822, "panel 2b: 1112x695 @2576x2544 Pimax, 740x462 @1597x1835 trimmed, 931x581 @1996x2121 Quest 3 (52)"},
    {11371, 9303, "panel 3: 1158x947 @2576x2544 Pimax, 771x630 @1597x1835 trimmed, 969x793 @1996x2121 Quest 3 (131)"},
    {15885, 3408, "panel 4: 1617x347 @2576x2544 Pimax, 1076x231 @1597x1835 trimmed, 1354x290 @1996x2121 Quest 3 (130)"},
};
constexpr uint32_t kUiQualitySeedCount = sizeof(kUiQualitySeeds) / sizeof(kUiQualitySeeds[0]);

// The entry whose ratio is within 0.5% on both axes, or -1.
inline int uiQualityRatioFind(const UiQualityRatio* table, uint32_t n, uint32_t rw, uint32_t rh) {
    if (!table) return -1;
    for (uint32_t i = 0; i < n; ++i) {
        if (uiQualityRatioNear(rw, table[i].w) && uiQualityRatioNear(rh, table[i].h))
            return static_cast<int>(i);
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

inline bool uiQualityRatioPlausible(uint32_t r) { return r > 0 && r < 1000000u; }

// Appends a learned ratio unless one within the tolerance is already there
// (census or learned). True only when it was added.
inline bool uiQualityLearn(UiQualityRatio* table, uint32_t* n, uint32_t max, uint32_t rw,
                           uint32_t rh) {
    if (!table || !n || !uiQualityRatioPlausible(rw) || !uiQualityRatioPlausible(rh)) return false;
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
// same ratio at a second unit more than 1% away. A GUI surface of fixed
// pixel size answers yes to the first and no to the second, and enlarging it
// in every later session would be wrong (review P2-1). So a GUI-drawn
// surface is first PENDING, with the basis it was made against; the same
// ratio at a unit more than 1% different PROMOTES it to the table; the same
// pixel size at such a unit marks the size FIXED, for good.

struct UiQualityPending {
    uint32_t rw = 0, rh = 0;  // its ratio against the basis it was made at
    uint32_t w = 0, h = 0;    // its pixel size
    UiQualityBasis b;         // that basis
};
struct UiQualitySize {
    uint32_t w = 0, h = 0;
};

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
        if (np >= kMaxPending || !uiQualityRatioPlausible(p.rw) || !uiQualityRatioPlausible(p.rh) ||
            !p.w || !p.h || !p.b.valid()) {
            return false;
        }
        pending[np++] = p;
        return true;
    }

    // One sighting of a GUI-drawn surface of w x h made in basis b.
    // *outW / *outH: the ratio promoted, on kPromoted.
    UiQualityLearn observe(uint32_t w, uint32_t h, const UiQualityBasis& b, uint32_t* outW = nullptr,
                           uint32_t* outH = nullptr) {
        if (!b.valid() || !uiQualityCandidateShape(w, h, b.W, b.H)) return UiQualityLearn::kIgnored;
        if (isFixed(w, h)) return UiQualityLearn::kFixed;
        const uint32_t rw = uiQualityPanelX10000(w, b), rh = uiQualityPanelX10000(h, b);
        if (uiQualityRatioFind(table, n, rw, rh) >= 0) return UiQualityLearn::kIgnored;
        // The same pixel size at another resolution: it does not scale.
        for (uint32_t i = 0; i < np; ++i) {
            if (pending[i].w != w || pending[i].h != h || !uiQualityBasisDiffers(pending[i].b, b)) continue;
            addFixed(w, h);
            for (uint32_t k = np; k-- > 0;)
                if (pending[k].w == w && pending[k].h == h) dropPending(k);
            return UiQualityLearn::kNotScaling;
        }
        // The same ratio: at another resolution it scales; at this one it
        // says nothing new.
        for (uint32_t i = 0; i < np; ++i) {
            const UiQualityPending p = pending[i];
            if (!uiQualityRatioNear(rw, p.rw) || !uiQualityRatioNear(rh, p.rh)) continue;
            if (!uiQualityBasisDiffers(p.b, b)) return UiQualityLearn::kSameBasis;
            const uint32_t lw = (p.rw + rw + 1) / 2, lh = (p.rh + rh + 1) / 2;
            dropPending(i);
            if (!uiQualityLearn(table, &n, kMaxRatios, lw, lh)) return UiQualityLearn::kFull;
            if (outW) *outW = lw;
            if (outH) *outH = lh;
            return UiQualityLearn::kPromoted;
        }
        UiQualityPending p;
        p.rw = rw;
        p.rh = rh;
        p.w = w;
        p.h = h;
        p.b = b;
        return addPending(p) ? UiQualityLearn::kPending : UiQualityLearn::kFull;
    }
};

// ui_quality_panels.txt, one fact a line ('#' starts a comment):
//   learned RW RH                     -- a panel ratio on the table
//   pending RW RH W H BW BH BT        -- a GUI surface waiting for a second
//                                        resolution (BT: 2 tan(vFOV/2) x 10000)
//   fixed W H                         -- a GUI surface size that does not scale
// Ratios in ten-thousandths of U. Anything else -- malformed, out of range --
// is skipped a line at a time, never trusted. (The first builds' file,
// ui_quality_ratios.txt, held ratios to the render size, not to U: not read.)
// Appends to `l` (after reset(): the census first); returns the lines taken.
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
            unsigned long v[8] = {};
            int count = 0;
            const char* q = p + k;
            bool clean = k > 0 && (q < lineEnd && (*q == ' ' || *q == '\t'));
            while (clean && count < 8) {
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
            auto ratio = [](unsigned long x) { return uiQualityRatioPlausible(static_cast<uint32_t>(x)) && x < 1000000ul; };
            auto pixels = [](unsigned long x) { return x > 0 && x <= 16384; };
            bool ok = false;
            if (clean && std::strcmp(word, "learned") == 0 && count == 2 && ratio(v[0]) && ratio(v[1])) {
                ok = uiQualityLearn(l.table, &l.n, UiQualityLearning::kMaxRatios,
                                    static_cast<uint32_t>(v[0]), static_cast<uint32_t>(v[1]));
            } else if (clean && std::strcmp(word, "pending") == 0 && count == 7 && ratio(v[0]) &&
                       ratio(v[1]) && pixels(v[2]) && pixels(v[3]) && pixels(v[4]) && pixels(v[5]) &&
                       v[6] > 100 && v[6] < 1000000ul) {
                UiQualityPending e;
                e.rw = static_cast<uint32_t>(v[0]);
                e.rh = static_cast<uint32_t>(v[1]);
                e.w = static_cast<uint32_t>(v[2]);
                e.h = static_cast<uint32_t>(v[3]);
                e.b.W = static_cast<uint32_t>(v[4]);
                e.b.H = static_cast<uint32_t>(v[5]);
                e.b.T = static_cast<float>(v[6]) / 10000.0f;
                ok = l.addPending(e);
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
        "# fix.ui_quality: interface panels on this rig, in ten-thousandths of U = W / (2 tan(vFOV/2)).\n"
        "# learned: GUI-drawn and seen to scale; pending: GUI-drawn, waiting for a second resolution\n"
        "# (W H BW BH BT: its size, the render size and 2 tan(vFOV/2) x 10000 it was made at);\n"
        "# fixed: GUI-drawn at one size across resolutions. Delete to forget.\n";
    char line[112];
    for (uint32_t i = 0; i < l.n; ++i) {
        if (l.table[i].origin != UiQualityOrigin::kLearned) continue;
        std::snprintf(line, sizeof(line), "learned %u %u\n", l.table[i].w, l.table[i].h);
        s += line;
    }
    for (uint32_t i = 0; i < l.np; ++i) {
        const UiQualityPending& p = l.pending[i];
        std::snprintf(line, sizeof(line), "pending %u %u %u %u %u %u %u\n", p.rw, p.rh, p.w, p.h, p.b.W,
                      p.b.H, static_cast<uint32_t>(p.b.T * 10000.0f + 0.5f));
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
