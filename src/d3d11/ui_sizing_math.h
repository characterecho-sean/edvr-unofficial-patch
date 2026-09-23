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

// ------------------------------------------------ the engine-side sizing --
//
// THE PATCH (the trace's section 3). The panel formula's two divisions,
// inlined in the init FUN_144570500 and the view-change recompute
// FUN_144571180, are `divss xmm6, [rip+disp32]` against the game's 1080.0f
// and 1920.0f. Pointing the four disp32s at EDVR's own two floats, 1080 x f
// and 1920 x f, makes the game itself create, lay out, viewport and depth-
// partner every render-to-texture panel at s / f: an operand swap, no length
// or control-flow change. With
//     f = (W_ui x k) / (W_out x k_out) / T,
// W_ui x k the game's own c (the render width and k of the frustum it is
// told), W_out x k_out the runtime's untrimmed output width and the true
// frustum's k, and T the key's target, the panel comes out at
// stage x W_out x k_out x T / 1920 -- its untrimmed size at HMD Quality = T
// whatever the trim or HMD Quality. Clamped to [1/4, 1]: no panel above four
// times its game size, and none smaller than the game makes it.

// The shape, twice in .text: comiss xmm0,[16/9]; jb +10; divss xmm6,[1080];
// jmp +11; movaps xmm6,xmm1; divss xmm6,[1920]. -1: a disp32 byte.
constexpr int kUiPanelShape[30] = {0x0F, 0x2F, 0x05, -1,   -1,   -1,   -1,   0x72, 0x0A, 0xF3,
                                   0x0F, 0x5E, 0x35, -1,   -1,   -1,   -1,   0xEB, 0x0B, 0x0F,
                                   0x28, 0xF1, 0xF3, 0x0F, 0x5E, 0x35, -1,   -1,   -1,   -1};
constexpr uint32_t kUiPanelShapeBytes = 30;
// Offsets in the shape: each disp32 and the end of its instruction (RIP).
constexpr uint32_t kUiPanelCmpDisp = 3, kUiPanelCmpNext = 7;
constexpr uint32_t kUiPanel1080Disp = 13, kUiPanel1080Next = 17;
constexpr uint32_t kUiPanel1920Disp = 26, kUiPanel1920Next = 30;

// Build 332841 (EliteDangerous64.exe, PE stamp 1788384820, image 104894464).
constexpr uint32_t kUiPanelStamp = 1788384820u, kUiPanelImageSize = 104894464u;
constexpr uint32_t kUiPanelSiteRva[2] = {0x45706FA, 0x4571205};  // init, view change
constexpr uint8_t kUiPanelSiteBytes[2][30] = {
    {0x0F, 0x2F, 0x05, 0x83, 0x26, 0xD8, 0x00, 0x72, 0x0A, 0xF3, 0x0F, 0x5E, 0x35, 0x55, 0xDF,
     0x86, 0x00, 0xEB, 0x0B, 0x0F, 0x28, 0xF1, 0xF3, 0x0F, 0x5E, 0x35, 0x4C, 0xDF, 0x86, 0x00},
    {0x0F, 0x2F, 0x05, 0x78, 0x1B, 0xD8, 0x00, 0x72, 0x0A, 0xF3, 0x0F, 0x5E, 0x35, 0x4A, 0xD4,
     0x86, 0x00, 0xEB, 0x0B, 0x0F, 0x28, 0xF1, 0xF3, 0x0F, 0x5E, 0x35, 0x41, 0xD4, 0x86, 0x00}};
constexpr uint32_t kUiPanelAspectRva = 0x52F2D84, kUiPanel1080Rva = 0x4DDE660,
                   kUiPanel1920Rva = 0x4DDE664;
constexpr uint32_t kUiPanelFuncRva[2] = {0x4570500, 0x4571180};
constexpr uint8_t kUiPanelProlog0[11] = {0x40, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00};
constexpr uint8_t kUiPanelProlog1[13] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x4C,
                                         0x8B, 0x81, 0xF8, 0x00, 0x00, 0x00};

inline bool uiPanelShapeAt(const uint8_t* p) {
    if (!p) return false;
    for (uint32_t i = 0; i < kUiPanelShapeBytes; ++i)
        if (kUiPanelShape[i] >= 0 && p[i] != static_cast<uint8_t>(kUiPanelShape[i])) return false;
    return true;
}

inline int32_t uiReadDisp(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
}

// A shape's three RIP-relative targets, as RVAs, from its bytes at `siteRva`.
struct UiPanelTargets {
    uint32_t aspect = 0, d1080 = 0, d1920 = 0;
};
inline UiPanelTargets uiPanelTargets(const uint8_t* p, uint32_t siteRva) {
    UiPanelTargets t;
    t.aspect = siteRva + kUiPanelCmpNext + static_cast<uint32_t>(uiReadDisp(p + kUiPanelCmpDisp));
    t.d1080 = siteRva + kUiPanel1080Next + static_cast<uint32_t>(uiReadDisp(p + kUiPanel1080Disp));
    t.d1920 = siteRva + kUiPanel1920Next + static_cast<uint32_t>(uiReadDisp(p + kUiPanel1920Disp));
    return t;
}

// The shape's starts in a buffer (a .text image at `bufRva`), up to `cap`;
// returns how many there are in all.
inline uint32_t uiPanelScan(const uint8_t* buf, size_t n, uint32_t bufRva, uint32_t* out, uint32_t cap) {
    uint32_t count = 0;
    if (!buf || n < kUiPanelShapeBytes) return 0;
    for (size_t i = 0; i + kUiPanelShapeBytes <= n; ++i) {
        if (buf[i] != 0x0F || buf[i + 7] != 0x72 || !uiPanelShapeAt(buf + i)) continue;
        if (out && count < cap) out[count] = bufRva + static_cast<uint32_t>(i);
        ++count;
    }
    return count;
}

// The disp32 that points an instruction ending at `next` at `target`
// (absolute addresses); false when out of rel32 range.
inline bool uiPanelDisp(uint64_t next, uint64_t target, int32_t* disp) {
    const int64_t d = static_cast<int64_t>(target) - static_cast<int64_t>(next);
    if (d < INT32_MIN || d > INT32_MAX) return false;
    if (disp) *disp = static_cast<int32_t>(d);
    return true;
}

// The factor's inputs, as EDVR has them.
struct UiPanelInputs {
    uint32_t renderW = 0;     // W_ui: trunc(what the game is told x HMD Quality)
    float fovTangent = 0.0f;  // 2 tan(vFOV/2) of the frustum the game is told
    uint32_t outputW = 0;     // W_out: the runtime's untrimmed recommendation
    float trueTangent = 0.0f; // 2 tan(vFOV/2) of the true display frustum
    float target = 0.0f;      // T: fix.ui_quality's 1.0 or 1.25
};

enum class UiPanelClamp : uint8_t { kNone = 0, kCap, kFloor };

// f, clamped to [1/4, 1]; false when an input is missing.
inline bool uiPanelFactor(const UiPanelInputs& in, double* f, UiPanelClamp* clamp = nullptr) {
    if (clamp) *clamp = UiPanelClamp::kNone;
    if (!in.renderW || !in.outputW || !(in.target > 0.0f)) return false;
    const double k = uiSizingK(in.fovTangent), kOut = uiSizingK(in.trueTangent);
    if (!(k > 0.0) || !(kOut > 0.0)) return false;
    double v = (static_cast<double>(in.renderW) * k) / (static_cast<double>(in.outputW) * kOut) /
               static_cast<double>(in.target);
    if (v < 0.25) {
        v = 0.25;
        if (clamp) *clamp = UiPanelClamp::kCap;
    } else if (v > 1.0) {
        v = 1.0;
        if (clamp) *clamp = UiPanelClamp::kFloor;
    }
    if (f) *f = v;
    return true;
}

// The two floats the patched operands read: 1080 x f and 1920 x f.
inline void uiPanelDivisors(double f, float* d1080, float* d1920) {
    if (d1080) *d1080 = static_cast<float>(1080.0 * f);
    if (d1920) *d1920 = static_cast<float>(1920.0 * f);
}

// The panel a stage becomes: trunc(stage x c / divisor), the game's own
// arithmetic (c = W_ui x k; the VR branch divides by the 1920 float).
inline uint32_t uiPanelSize(uint32_t stage, double c, float divisor1920) {
    if (!(divisor1920 > 0.0f)) return 0;
    const float s = static_cast<float>(c) / divisor1920;
    return static_cast<uint32_t>(static_cast<float>(stage) * s);
}

}  // namespace edvr
