// fix.ui_quality -- the arithmetic of the engine's own panel sizing
// (docs/ui-sizing-owner-2026-09-23.md), pure and header-only: no device, no
// Config, no Log. The DLL (ui_surfaces.cpp) and its build-gate rig
// (tools/ui_quality_test) both include this one file.
//
// THE CONFIRMATION INSTRUMENT (the doc's section 8). Every Scaleform
// render-to-texture panel is sized in FUN_144570500 (init) or FUN_144571180
// (a view change) as stage x s, s = c/1920 with c = W_ui x k and
// k = tan(0.782)/tan(vFOV/2), and created through one chain of calls. A
// create's return-address chain -- the game's own frames, innermost first --
// says whether it is one: `rtt` when it holds both 0x4551C7D and 0x45517F6,
// tagged `init` (0x4570902) or `change` (0x456DA45) and `colour` (0x28148A9)
// or `depth` (0x2814686); `glyph-cache` when it holds Scaleform's raster
// cache's texture maker's returns, 0x30FC1A (init) or 0x312450 (re-create);
// `other` otherwise. Every RVA is EliteDangerous64.exe build 332841's.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

constexpr uint32_t kUiChainFrames = 12;  // game frames kept, innermost first

struct UiChain {
    uint32_t rva[kUiChainFrames] = {};
    uint32_t n = 0;
};

// The game module's frames among a captured stack (innermost first), as RVAs,
// up to kUiChainFrames. EDVR's own frames and the runtime's are skipped.
inline uint32_t uiChainFromFrames(const uintptr_t* frames, uint32_t count, uintptr_t base,
                                  uintptr_t size, UiChain* out) {
    if (!out) return 0;
    *out = UiChain{};
    if (!frames || !base || !size) return 0;
    for (uint32_t i = 0; i < count && out->n < kUiChainFrames; ++i) {
        const uintptr_t a = frames[i];
        if (a < base || a - base >= size) continue;
        out->rva[out->n++] = static_cast<uint32_t>(a - base);
    }
    return out->n;
}

inline bool uiChainHas(const UiChain& c, uint32_t rva) {
    for (uint32_t i = 0; i < c.n; ++i)
        if (c.rva[i] == rva) return true;
    return false;
}

// The RVAs the verdict reads (section 8), in the order the line names them.
constexpr uint32_t kUiRvaRttMid = 0x4551C7D, kUiRvaRttOuter = 0x45517F6;
constexpr uint32_t kUiRvaInit = 0x4570902, kUiRvaChange = 0x456DA45;
constexpr uint32_t kUiRvaColour = 0x28148A9, kUiRvaDepth = 0x2814686;
constexpr uint32_t kUiRvaGlyphInit = 0x30FC1A, kUiRvaGlyphRecreate = 0x312450;
constexpr uint32_t kUiChainKeys[] = {kUiRvaRttMid, kUiRvaRttOuter, kUiRvaInit, kUiRvaChange,
                                     kUiRvaColour, kUiRvaDepth, kUiRvaGlyphInit,
                                     kUiRvaGlyphRecreate};
constexpr uint32_t kUiChainKeyCount = sizeof(kUiChainKeys) / sizeof(kUiChainKeys[0]);

enum class UiChainKind : uint8_t { kOther = 0, kRtt, kGlyphCache };

struct UiChainVerdict {
    UiChainKind kind = UiChainKind::kOther;
    bool init = false, change = false, colour = false, depth = false;
    uint32_t hits = 0;  // bit i: kUiChainKeys[i] is in the chain
};

inline UiChainVerdict uiChainVerdict(const UiChain& c) {
    UiChainVerdict v;
    for (uint32_t i = 0; i < kUiChainKeyCount; ++i)
        if (uiChainHas(c, kUiChainKeys[i])) v.hits |= 1u << i;
    const bool rtt = (v.hits & 3u) == 3u;
    const bool glyph = (v.hits & (3u << 6)) != 0;
    v.kind = rtt ? UiChainKind::kRtt : glyph ? UiChainKind::kGlyphCache : UiChainKind::kOther;
    v.init = (v.hits & (1u << 2)) != 0;
    v.change = (v.hits & (1u << 3)) != 0;
    v.colour = (v.hits & (1u << 4)) != 0;
    v.depth = (v.hits & (1u << 5)) != 0;
    return v;
}

// "0x51AF6B/0x50EC69/..." (at most 12 x 10 characters).
inline void uiChainFormat(const UiChain& c, char* out, size_t n) {
    if (!out || !n) return;
    out[0] = '\0';
    size_t used = 0;
    for (uint32_t i = 0; i < c.n && used + 1 < n; ++i) {
        const int m = std::snprintf(out + used, n - used, "%s0x%X", i ? "/" : "", c.rva[i]);
        if (m < 0) break;
        used += static_cast<size_t>(m);
    }
    if (used >= n) out[n - 1] = '\0';
}

// The verdict's kind and tags alone: "rtt init colour", "glyph-cache", "other".
inline const char* uiChainVerdictShort(const UiChainVerdict& v) {
    if (v.kind == UiChainKind::kGlyphCache) return "glyph-cache";
    if (v.kind != UiChainKind::kRtt) return "other";
    static const char* const kRtt[3][3] = {
        {"rtt", "rtt colour", "rtt depth"},
        {"rtt init", "rtt init colour", "rtt init depth"},
        {"rtt change", "rtt change colour", "rtt change depth"}};
    const int when = v.init ? 1 : v.change ? 2 : 0;
    const int kind = v.colour ? 1 : v.depth ? 2 : 0;
    return kRtt[when][kind];
}

// "rtt init colour" / "glyph-cache" / "other", then every key's hit or miss:
// "(0x4551C7D yes, 0x45517F6 yes, 0x4570902 yes, 0x456DA45 no, ...)".
inline void uiChainVerdictText(const UiChainVerdict& v, char* out, size_t n) {
    if (!out || !n) return;
    const char* kind = v.kind == UiChainKind::kRtt          ? "rtt"
                       : v.kind == UiChainKind::kGlyphCache ? "glyph-cache"
                                                            : "other";
    int used = std::snprintf(out, n, "%s%s%s%s%s (", kind,
                             v.kind == UiChainKind::kRtt && v.init ? " init" : "",
                             v.kind == UiChainKind::kRtt && v.change ? " change" : "",
                             v.kind == UiChainKind::kRtt && v.colour ? " colour" : "",
                             v.kind == UiChainKind::kRtt && v.depth ? " depth" : "");
    for (uint32_t i = 0; i < kUiChainKeyCount && used > 0 && static_cast<size_t>(used) < n; ++i) {
        const int m = std::snprintf(out + used, n - static_cast<size_t>(used), "%s0x%X %s",
                                    i ? ", " : "", kUiChainKeys[i],
                                    (v.hits >> i) & 1u ? "yes" : "no");
        if (m < 0) break;
        used += m;
    }
    if (used > 0 && static_cast<size_t>(used) + 1 < n) {
        out[used] = ')';
        out[used + 1] = '\0';
    } else {
        out[n - 1] = '\0';
    }
}

// The panel formula's k for a frustum of 2 tan(vFOV/2) = T (ui_quality_math.h's
// uiQualityFovTangent): tan(0.782) / tan(vFOV/2). 0 when T is unknown.
inline double uiSizingK(float fovTangent) {
    if (!(fovTangent > 0.0f)) return 0.0;
    return 2.0 * std::tan(0.782) / static_cast<double>(fovTangent);
}

// The movie stage a panel dimension implies: dim x 1920 / (W x k) -- the
// panel formula s = W x k / 1920 inverted (the VR branch, c/d < 16/9). 0
// when an input is missing. A 1920x1080 stage is the menu's 16:9 screen.
inline double uiImpliedStage(uint32_t dim, uint32_t renderW, double k) {
    if (!renderW || !(k > 0.0)) return 0.0;
    return static_cast<double>(dim) * 1920.0 / (static_cast<double>(renderW) * k);
}

}  // namespace edvr
