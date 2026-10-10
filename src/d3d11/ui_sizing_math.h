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
#include <cstdlib>
#include <cstring>

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

// ------------------------------------------- Supersampling and the size budget --
//
// 2026-10-07. A Frontier VR launch at Elite's Supersampling 2.0 (left there by a flat session; the graphics
// options are one file for every install) died within seconds: the panel patch asked D3D11 for a 19200x10800
// render-to-texture panel and Elite aborts on any refused create. The mechanism, from the game's own code
// (docs/ui-layer-2026-09-23.md, "2026-10-07: Supersampling and the panel budget"):
//   * the UI screen record carries two sizes, +0x40 (the base: the VR manager's trunc(recommended x HMD
//     Quality), the display, or a 2D-mode override) and +0x30 = trunc(+0x40 x scale), the scale being the
//     SSAAMultiplier entry of the .fxcfg (FUN_14284CB70 and FUN_14288E3A0 write both; FUN_142842A70 reads
//     c = max(+0x30, +0x40) x k). The scene's own views are sized from +0x30 too.
//   * so the game's c is linear in Supersampling, per axis: at Supersampling S > 1 every panel the game makes
//     is S times wider than W_ui x k says, and EDVR's f (made from W_ui x k) must carry the same S or the
//     patch stacks its own density on the game's (S x 1/f instead of 1/f).
//   * and no f, however small, may take any panel past D3D11's 16384 (D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION).
//     The widest panel the formula can ask for is S x B / f, B the base c over the states the record takes
//     (a stage the width of the 1920 one: the menu's 16:9 surface). The refused create is 19200 = 2.0 x B /
//     0.4 with B = 3840, which is the one number every state of the record that a flight has seen agrees on;
//     B is that, or larger where the display, the 2D screen's forced width or the scene say so.
constexpr double kUiPanelTextureLimit = 16384.0;  // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
constexpr double kUiPanelBudget = 14336.0;        // 7/8 of it: a stage 14% wider than the 1920 one still fits
constexpr double kUiPanelObservedBase = 3840.0;   // B from the refused create (19200 x 0.4 / 2.0)

// The factor's inputs, as EDVR has them.
struct UiPanelInputs {
    uint32_t renderW = 0;     // W_ui: trunc(what the game is told x HMD Quality)
    float fovTangent = 0.0f;  // 2 tan(vFOV/2) of the frustum the game is told
    uint32_t outputW = 0;     // W_out: the runtime's untrimmed recommendation
    float trueTangent = 0.0f; // 2 tan(vFOV/2) of the true display frustum
    float target = 0.0f;      // T: fix.ui_quality's 100 or 125, as 1.0 or 1.25
    float supersampling = 0.0f;  // the .fxcfg's SSAAMultiplier; 0 while unknown (then there is no factor)
    uint32_t displayW = 0;    // the game window's width (DisplaySettings.xml); 0 unknown (the observed base stands)
    uint32_t screenW = 0;     // the 2D screen's forced width (fix.vscreen_res_width, applied); 0 none
};

enum class UiPanelClamp : uint8_t { kNone = 0, kCap, kFloor, kBudget };
enum class UiPanelBase : uint8_t { kObserved = 0, kScene, kDisplay, kScreen };

// Everything one factor is made of, for the line and the tests.
struct UiPanelPlan {
    double f = 1.0;           // the factor the floats get
    double formula = 1.0;     // (W_ui x k) / (W_out x k_out) / T: the factor before this change, unclamped
    double beforeF = 1.0;     // ...clamped to [1/4, 1]: what the patch wrote before the Supersampling term
    double ss = 1.0;          // max(Supersampling, 1)
    double lineF = 1.0;       // formula x ss clamped to [1/4, 1], without the budget: the layer/render ratio the
                              // orbit lines' width needs (the budget thins panels, it does not thin the layer)
    double base = 0.0;        // B, px
    UiPanelBase baseFrom = UiPanelBase::kObserved;
    double largest = 0.0;     // the widest panel the formula could ask for at f: ss x B / f, px
    double largestBefore = 0.0;  // ...at the factor without the Supersampling term (what crashed): ss x B / beforeF
    bool ssActs = false;      // the Supersampling term changed the factor
    bool budgetActs = false;  // the budget changed the factor
    UiPanelClamp clamp = UiPanelClamp::kNone;
};

inline const char* uiPanelBaseName(UiPanelBase b) {
    return b == UiPanelBase::kScene     ? "the scene's own width"
           : b == UiPanelBase::kDisplay ? "the display's width"
           : b == UiPanelBase::kScreen  ? "the 2D screen's forced width"
                                        : "the width a refused create measured";
}

inline double uiPanelClampF(double v) { return v < 0.25 ? 0.25 : v > 1.0 ? 1.0 : v; }

// The solve, from the two numbers the rest of the plan is made of: the formula (W_ui x k) / (W_out x k_out) / T
// and the base B, and the Supersampling. Pure and separate so the game's own setter hook (ui_panel_scale.cpp) can
// make the same factor from what the render thread last published, without the render thread's other inputs.
inline void uiPanelSolve(double formula, double base, UiPanelBase baseFrom, float supersampling, UiPanelPlan* out) {
    UiPanelPlan p;
    p.formula = formula;
    p.ss = supersampling > 1.0f ? static_cast<double>(supersampling) : 1.0;
    const double withSs = p.ss > 1.0 ? formula * p.ss : formula;
    p.base = base;
    p.baseFrom = baseFrom;
    const double floorF = p.ss * base / kUiPanelBudget;
    const double withBudget = withSs < floorF ? floorF : withSs;
    p.beforeF = uiPanelClampF(formula);
    p.lineF = uiPanelClampF(withSs);
    p.f = uiPanelClampF(withBudget);
    p.ssActs = p.lineF != p.beforeF;
    p.budgetActs = p.f != p.lineF;
    if (p.budgetActs)
        p.clamp = UiPanelClamp::kBudget;
    else if (withSs < 0.25)
        p.clamp = UiPanelClamp::kCap;
    else if (withSs > 1.0)
        p.clamp = UiPanelClamp::kFloor;
    p.largest = p.ss * p.base / p.f;
    p.largestBefore = p.ss * p.base / p.beforeF;
    if (out) *out = p;
}

// f = clamp( max( formula x ss, ss x B / budget ), [1/4, 1] ). At Supersampling <= 1, ss is exactly 1 and the
// budget is not binding (B <= 5734 at f 0.4), f is today's expression bit for bit. False when an input is missing.
inline bool uiPanelPlanFor(const UiPanelInputs& in, UiPanelPlan* out) {
    if (out) *out = UiPanelPlan{};
    if (!in.renderW || !in.outputW || !(in.target > 0.0f)) return false;
    if (!(in.supersampling > 0.0f) || !std::isfinite(in.supersampling)) return false;
    const double k = uiSizingK(in.fovTangent), kOut = uiSizingK(in.trueTangent);
    if (!(k > 0.0) || !(kOut > 0.0)) return false;
    const double formula = (static_cast<double>(in.renderW) * k) / (static_cast<double>(in.outputW) * kOut) /
                           static_cast<double>(in.target);
    double base = kUiPanelObservedBase;
    UiPanelBase baseFrom = UiPanelBase::kObserved;
    const double scene = static_cast<double>(in.renderW) * k;
    if (scene > base) {
        base = scene;
        baseFrom = UiPanelBase::kScene;
    }
    if (static_cast<double>(in.displayW) > base) {
        base = static_cast<double>(in.displayW);
        baseFrom = UiPanelBase::kDisplay;
    }
    if (static_cast<double>(in.screenW) > base) {
        base = static_cast<double>(in.screenW);
        baseFrom = UiPanelBase::kScreen;
    }
    uiPanelSolve(formula, base, baseFrom, in.supersampling, out);
    return true;
}

// f, clamped to [1/4, 1] and raised by the budget; false when an input is missing.
inline bool uiPanelFactor(const UiPanelInputs& in, double* f, UiPanelClamp* clamp = nullptr) {
    UiPanelPlan p;
    const bool ok = uiPanelPlanFor(in, &p);
    if (clamp) *clamp = p.clamp;
    if (ok && f) *f = p.f;
    return ok;
}

// ------------------------------------------------------------ the flat profile's factor (2026-10-09) --
//
// In the flat profile the panel formula's (c, d) are the UI screen record's width and height with k = 1
// (FUN_142842A70 asks the VR manager for k only in the VR modes): the scene's render size R, Supersampling already in
// it. Measured on the 2026-10-09 09:36 Epic flight (771bb99a, D 3840x2160 throughout): the same rtt-init panel was
// created 1920x960 while R was 3840x2160 and 960x480 once R was 1920x1080 -- the panels follow R, not the display D.
// The game divides by 1080 when c/d is not below 16/9 (the comiss/jb at both sites), by 1920 when it is; the ratio is
// taken on that axis. The patch makes s' = s / f, so for panels at D x T:
//     f = (R / D) / T                (on the axis the game divides; R and D must have one aspect)
// clamped and budgeted by the same uiPanelSolve with no Supersampling term (it is in R already). The widest panel the
// formula can ask for is the 1920 stage's: c at f = 1 on the width axis, 1920 x d / 1080 on the height axis.
struct UiFlatPanelInputs {
    uint32_t renderW = 0, renderH = 0;  // R: the scene the game renders (the flat runtime's own measurement)
    uint32_t outputW = 0, outputH = 0;  // D: the swap chain's back buffer
    float target = 0.0f;                // T: 1.0 or 1.25
};
enum class UiFlatPanelRefuse : uint8_t { kNone = 0, kUnknown, kAspect };
inline const char* uiFlatPanelRefuseName(UiFlatPanelRefuse r) {
    return r == UiFlatPanelRefuse::kUnknown ? "the render or display size is not known yet"
           : r == UiFlatPanelRefuse::kAspect ? "the render size is not the display's shape (a non-scene frame, or a resolution unlike the screen's)"
                                             : "none";
}
// The axis the game divides on: height (1080) unless R is narrower than 16:9. Exact integer compare, as the game's
// float compare at 16:9 itself is not below.
inline bool uiFlatPanelHeightAxis(uint32_t renderW, uint32_t renderH) {
    return static_cast<uint64_t>(renderW) * 9u >= static_cast<uint64_t>(renderH) * 16u;
}
inline UiFlatPanelRefuse uiFlatPanelPlanFor(const UiFlatPanelInputs& in, UiPanelPlan* out) {
    if (out) *out = UiPanelPlan{};
    if (!in.renderW || !in.renderH || !in.outputW || !in.outputH || !(in.target > 0.0f)) return UiFlatPanelRefuse::kUnknown;
    // One shape: R x D's height against R's height x D's width, within 1% (the game's rounding of R is a pixel or two).
    const double rd = static_cast<double>(in.renderW) * in.outputH, dr = static_cast<double>(in.renderH) * in.outputW;
    if (std::fabs(rd - dr) > 0.01 * (rd > dr ? rd : dr)) return UiFlatPanelRefuse::kAspect;
    const bool height = uiFlatPanelHeightAxis(in.renderW, in.renderH);
    const double ratio = height ? static_cast<double>(in.renderH) / in.outputH : static_cast<double>(in.renderW) / in.outputW;
    const double formula = ratio / static_cast<double>(in.target);
    const double base = height ? 1920.0 * static_cast<double>(in.renderH) / 1080.0 : static_cast<double>(in.renderW);
    uiPanelSolve(formula, base, UiPanelBase::kScene, 1.0f, out);
    return UiFlatPanelRefuse::kNone;
}
// The game's Supersampling setter moved from ssFrom to ssTo before the render size has followed (the setter thunk):
// R scales with it, so the formula and the base do. ssFrom is the live value the plan was made beside.
inline bool uiFlatPanelMove(double formula, double base, float ssFrom, float ssTo, UiPanelPlan* out) {
    if (!(formula > 0.0) || !(base > 0.0) || !(ssFrom > 0.0f) || !(ssTo > 0.0f)) return false;
    const double r = static_cast<double>(ssTo) / static_cast<double>(ssFrom);
    uiPanelSolve(formula * r, base * r, UiPanelBase::kScene, 1.0f, out);
    return true;
}
// The flat frame boundary's decision about the plan it just made (ui_panel_scale.cpp's flatFrameBoundary; the 0.19.0 release review, finding 3).
//
// A plan is written only once the inputs have held it for kUiPanelSettleFrames boundaries (the two runs of one view change read one factor). The game's Supersampling
// setter is the exception: it writes the NEW factor (uiFlatPanelMove) before the game reconfigures, and the scene's size follows later. Until it does, the plan the
// boundary makes is still the OLD size's, which, settled long ago, would have been written straight back over the setter's factor -- and the panels the reconfigure makes in
// between would be sized for the render the game is leaving -- and the new size's plan, once it arrived, would then wait the whole settle again for the right one. So a
// factor the setter wrote is HELD (nothing is written, nothing is published) until the scene's size differs from the size it had when the setter ran; the new size's plan is
// then accepted at once, without settling through the old value, and written if it is not the factor already there. A size that never follows (the setter moved a value that
// leaves the render as it was) ends the hold after kUiPanelTransitionFrames boundaries, and the plan from the size there is stands.
constexpr uint32_t kUiPanelSettleFrames = 10;      // the inputs steady this long before a write
constexpr uint32_t kUiPanelTransitionFrames = 120; // the longest a setter's factor is held for the scene's size to follow
class UiFlatPanelSettle {
public:
    enum class Act : uint8_t {
        kWait,    // the plan has not held long enough: nothing written, nothing published
        kHold,    // a setter's factor stands until the scene's size follows: nothing written, nothing published
        kKeep,    // settled and published; the factor already there is the plan's (within 0.1%)
        kWrite    // settled and published; write the plan
    };
    struct Step {
        Act act = Act::kWait;
        bool publish = false;     // the plan is settled: publish it for the setter thunk (the believable-live-value rule is the caller's)
        bool arrived = false;     // this boundary saw the scene's size follow a setter: the plan was accepted without settling
        bool timedOut = false;    // this boundary gave up waiting for it
        uint32_t waited = 0;      // boundaries a held factor waited (on arrived or timedOut)
    };
    // `planF` this boundary's plan; `live` a factor has been written; `floatsF` the factor in the floats now (the setter may have moved it); `epoch` the number of
    // factor moves the setter has made and `atW`/`atH` the scene's size when the last one ran; `nowW`/`nowH` the scene's size now.
    Step step(double planF, bool live, double floatsF, uint32_t epoch, uint32_t atW, uint32_t atH, uint32_t nowW, uint32_t nowH) {
        Step r;
        if (epoch != seenEpoch_) {   // the setter moved the factor since the last boundary (again, if one was held already: the wait starts over)
            seenEpoch_ = epoch;
            holding_ = true;
            atW_ = atW;
            atH_ = atH;
            waited_ = 0;
        }
        if (holding_) {
            if (nowW != atW_ || nowH != atH_) {
                holding_ = false;
                r.arrived = true;
                r.waited = waited_;
            } else if (++waited_ <= kUiPanelTransitionFrames) {
                r.act = Act::kHold;
                return r;
            } else {
                holding_ = false;
                r.timedOut = true;
                r.waited = waited_ - 1;
            }
        }
        if (r.arrived) {
            pending_ = planF;
            settle_ = kUiPanelSettleFrames;
        } else {
            if (std::fabs(planF - pending_) > 1e-6) {
                pending_ = planF;
                settle_ = 0;
                r.act = Act::kWait;
                return r;
            }
            if (++settle_ < kUiPanelSettleFrames) {
                r.act = Act::kWait;
                return r;
            }
        }
        r.publish = true;
        r.act = (live && std::fabs(planF / floatsF - 1.0) <= 0.001) ? Act::kKeep : Act::kWrite;
        return r;
    }
    // A frame with no plan (a loading screen, a 512x512 preview): the settle starts over, the held factor stays.
    void unsettle() { settle_ = 0; }
    // The key off, or the anti-aliasing: the game's own sizes, and nothing held or half-settled.
    void forget() {
        pending_ = -1.0;
        settle_ = 0;
        holding_ = false;
    }
    bool holding() const { return holding_; }

private:
    double pending_ = -1.0;
    uint32_t settle_ = 0;
    uint32_t seenEpoch_ = 0;
    bool holding_ = false;
    uint32_t atW_ = 0, atH_ = 0, waited_ = 0;
};

// THE TWO CRITICAL SECTIONS (the follow-up review of dbbbcf03, finding 2). The boundary (render thread) reads the setter's epoch and the factor in the floats, decides, and
// writes; the setter thunk (a game thread) writes its factor and then publishes a new epoch. Taken one load and one store at a time, a setter that landed between the
// boundary's epoch read and its write was overwritten (epoch 0 read, setter 0.400 and epoch 1, factor read as 0.400, the settled old plan 0.800 written back, then a hold on
// the overwritten value), and so was a boundary that read the factor between the setter's write and its epoch. Each is therefore ONE operation under one lock, the same lock
// that serialises the floats' writes: the boundary from its epoch read to the end of its write, the setter from its read of the published plan to its epoch. They are
// templates over an Env so that the production glue (ui_panel_scale.cpp's PanelEnv) and the rig's model run THESE statements; the rig injects a setter or a boundary at each
// UiFlatSeam (a no-op in production) and a thread that arrives while the lock is held waits for it, as the real one does.
//
// What runs under the lock, and only this: the settle's arithmetic (UiFlatPanelSettle::step), uiFlatPanelMove / uiPanelSolve, loads and stores of atomics, the one atomic load of
// the scene's size, and the floats' write (two VirtualProtect calls and two float stores, which were under this lock before). Not under it: reading the game's context for
// the live Supersampling (done before the lock and handed in), the log, the counters, any call into the game's code, any wait on another lock.
enum class UiFlatSeam : uint8_t {
    kBeforeLock = 0,   // the boundary, before it takes the lock
    kEpochRead,        // the boundary, holding the lock, after it read the epoch and the size
    kDecided,          // ...after the settle decided
    kPublished,        // ...after it published the plan, before it writes
    kWritten,          // ...after it wrote
    kSetterWritten     // the setter, holding the lock, after it wrote its factor and before it published the epoch
};

template <class Env>
struct UiLockGuard {
    explicit UiLockGuard(Env& e) : env(e) { env.lock(); }
    ~UiLockGuard() { env.unlock(); }
    UiLockGuard(const UiLockGuard&) = delete;
    UiLockGuard& operator=(const UiLockGuard&) = delete;
    Env& env;
};

// The plan the render thread last published for the setter thunk (the believable live Supersampling it was made beside, and the scene size).
struct UiPublishedPlan {
    bool ready = false, flat = false;
    double formula = 0.0, base = 0.0;
    float ss = 0.0f;
    uint64_t dims = 0;
};

// The boundary, after it has a plan for the scene it sees (nowW x nowH): the epoch, the decision, the publish and the write, as one operation. `*wrote` is true when the
// floats were written. Env: lock(), unlock(), seam(UiFlatSeam), setterEpoch(), setterAt(), isLive(), factorNow(), publish(plan, w, h), writeFactors(f, lineF, ss) -> bool.
template <class Env>
UiFlatPanelSettle::Step uiFlatPanelBoundarySection(UiFlatPanelSettle& settle, Env& env, const UiPanelPlan& plan, uint32_t nowW, uint32_t nowH, bool* wrote) {
    *wrote = false;
    env.seam(UiFlatSeam::kBeforeLock);
    UiLockGuard<Env> guard(env);
    const uint32_t epoch = env.setterEpoch();
    const uint64_t at = env.setterAt();
    env.seam(UiFlatSeam::kEpochRead);
    const UiFlatPanelSettle::Step s = settle.step(plan.f, env.isLive(), env.factorNow(), epoch, static_cast<uint32_t>(at >> 32),
                                                  static_cast<uint32_t>(at & 0xFFFFFFFFu), nowW, nowH);
    env.seam(UiFlatSeam::kDecided);
    if (s.act == UiFlatPanelSettle::Act::kHold || s.act == UiFlatPanelSettle::Act::kWait) return s;
    if (s.publish) env.publish(plan, nowW, nowH);
    env.seam(UiFlatSeam::kPublished);
    if (s.act == UiFlatPanelSettle::Act::kWrite) *wrote = env.writeFactors(plan.f, plan.lineF, 1.0);
    env.seam(UiFlatSeam::kWritten);
    return s;
}

// The setter thunk's move to the Supersampling `newSs`: the factor from the published plan, written if it is not there, and (flat) the count that tells the boundary there is
// a factor to hold, as one operation. True when it moved the factor. Env: lock(), unlock(), seam(UiFlatSeam), published() -> UiPublishedPlan, factorNow(), lineFactorNow(),
// writeFactors(f, lineF, ss) -> bool, noteMove(flat, publishedDims).
template <class Env>
bool uiPanelSetterSection(Env& env, float newSs) {
    UiLockGuard<Env> guard(env);
    const UiPublishedPlan pub = env.published();
    if (!pub.ready || !(pub.formula > 0.0) || !(pub.base > 0.0)) return false;
    UiPanelPlan p;
    if (pub.flat) {
        // Flat: R carries the Supersampling, so the plan scales by its move.
        if (!uiFlatPanelMove(pub.formula, pub.base, pub.ss, newSs, &p)) return false;
    } else {
        uiPanelSolve(pub.formula, pub.base, UiPanelBase::kObserved, newSs, &p);
    }
    if (std::fabs(p.f - env.factorNow()) <= 1e-9 && std::fabs(p.lineF - env.lineFactorNow()) <= 1e-9) return false;
    if (!env.writeFactors(p.f, p.lineF, p.ss)) return false;
    env.seam(UiFlatSeam::kSetterWritten);
    env.noteMove(pub.flat, pub.dims);
    return true;
}

// The panel the game makes from a stage on the height axis: trunc(stage x (R_h / 1080 x f)), its own arithmetic.
inline uint32_t uiFlatPanelSizeHeightAxis(uint32_t stage, uint32_t renderH, float divisor1080) {
    if (!(divisor1080 > 0.0f)) return 0;
    const float s = static_cast<float>(renderH) / divisor1080;
    return static_cast<uint32_t>(static_cast<float>(stage) * s);
}

// The game window's size from DisplaySettings.xml's text: <ScreenWidth> and <ScreenHeight>. False unless both
// are there and sane (a window of 320 to 16384 a side).
inline bool uiDisplaySizeFromXml(const char* text, size_t n, uint32_t* w, uint32_t* h) {
    if (!text || !n) return false;
    auto number = [&](const char* tag, uint32_t* v) {
        const size_t len = std::strlen(tag);
        for (size_t i = 0; i + len < n; ++i) {
            if (std::memcmp(text + i, tag, len) != 0) continue;
            char buf[16] = {};
            size_t used = 0;
            for (size_t j = i + len; j < n && used + 1 < sizeof(buf) && text[j] >= '0' && text[j] <= '9'; ++j)
                buf[used++] = text[j];
            if (!used) return false;
            const long value = std::strtol(buf, nullptr, 10);
            if (value < 320 || value > 16384) return false;
            *v = static_cast<uint32_t>(value);
            return true;
        }
        return false;
    };
    uint32_t ww = 0, hh = 0;
    if (!number("<ScreenWidth>", &ww) || !number("<ScreenHeight>", &hh)) return false;
    if (w) *w = ww;
    if (h) *h = hh;
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

// ------------------------------------------------- the game's LIVE Supersampling --
//
// 2026-10-08. 6b1ce794 read the Supersampling from the .fxcfg every 5 s, so a change in the game's graphics menu
// could reach the panels before the factor knew. The value the game acts on is the scale entry of its render
// context, ctx+0x3564 (docs/ui-layer-2026-09-23.md, "2026-10-07: Supersampling and the panel budget"), clamped
// to [ctx+0x3568, ctx+0x356C] by FUN_1428767D0, the setter, which is virtual slot +0x90 of the context's
// interface class (vtable 0x52E9020; slot RVA 0x52E90B0). The interface object holds the context at this+0x18;
// the setter's sibling at +0x98 (0x28414E0) reads ctx+0x359C, the scale the last configure used. EDVR replaces
// both slots with thunks that forward untouched and remember the context (and, at the setter, move the
// factor before the original runs: the game's reconfigure follows the setter, never precedes it).
constexpr uint32_t kUiSsSetterSlotRva = 0x52E90B0, kUiSsGetterSlotRva = 0x52E90B8;
constexpr uint32_t kUiSsSetterRva = 0x28767D0, kUiSsGetterRva = 0x28414E0;
constexpr uint32_t kUiSsCtxFromThis = 0x18;
constexpr uint32_t kUiSsOffCur = 0x3564, kUiSsOffMin = 0x3568, kUiSsOffMax = 0x356C;
constexpr uint8_t kUiSsSetterBytes[38] = {0x48, 0x8B, 0x41, 0x18, 0xF3, 0x0F, 0x10, 0x80, 0x68, 0x35, 0x00, 0x00, 0x0F,
                                          0x2F, 0xC1, 0x77, 0x0C, 0xF3, 0x0F, 0x10, 0x80, 0x6C, 0x35, 0x00, 0x00, 0xF3,
                                          0x0F, 0x5D, 0xC1, 0xF3, 0x0F, 0x11, 0x80, 0x64, 0x35, 0x00, 0x00, 0xC3};
constexpr uint8_t kUiSsGetterBytes[13] = {0x48, 0x8B, 0x41, 0x18, 0xF3, 0x0F, 0x10, 0x80, 0x9C, 0x35, 0x00, 0x00, 0xC3};
constexpr float kUiSsCeiling = 8.0f;  // the .fxcfg reader's own ceiling (device_hook.cpp eliteHmdMultiplier)

enum class UiSsRead : uint8_t { kNone = 0, kFault, kOk };  // no render context known yet / a read faulted / read
enum class UiSsSource : uint8_t { kNone = 0, kLive, kFxcfg };
enum class UiSsWhy : uint8_t { kNone = 0, kNotCaptured, kUnreadable, kNotFinite, kOutOfRange };

inline const char* uiSsSourceName(UiSsSource s) {
    return s == UiSsSource::kLive ? "live" : s == UiSsSource::kFxcfg ? ".fxcfg" : "none";
}
inline const char* uiSsWhyName(UiSsWhy w) {
    return w == UiSsWhy::kNotCaptured   ? "the game has not called its Supersampling setter or getter yet, so its render context is not known"
           : w == UiSsWhy::kUnreadable  ? "the render context could not be read"
           : w == UiSsWhy::kNotFinite   ? "the live value is not a number"
           : w == UiSsWhy::kOutOfRange  ? "the live value is outside the game's own range"
                                        : "";
}

// The live entry is believable: all three finite, the range the game clamps to sane and ordered, and the value
// inside it (the setter makes it so; anything else is the wrong memory).
inline bool uiLiveSupersamplingValid(float cur, float lo, float hi, UiSsWhy* why) {
    if (why) *why = UiSsWhy::kNone;
    if (!std::isfinite(cur) || !std::isfinite(lo) || !std::isfinite(hi)) {
        if (why) *why = UiSsWhy::kNotFinite;
        return false;
    }
    if (!(lo > 0.0f) || !(hi >= lo) || hi > kUiSsCeiling || !(cur > 0.0f) || cur < lo - 1e-4f || cur > hi + 1e-4f) {
        if (why) *why = UiSsWhy::kOutOfRange;
        return false;
    }
    return true;
}

struct UiSsPick {
    float ss = 0.0f;                          // the Supersampling the factor is made from; 0 when neither source has one
    UiSsSource source = UiSsSource::kNone;
    UiSsWhy why = UiSsWhy::kNone;             // when source is the .fxcfg: why the live value was not used
};

// The choice: the live value when it was read and is believable, the .fxcfg's when not (and why), nothing when
// neither is there. `fxcfg` is 0 while unknown.
inline UiSsPick uiPickSupersampling(UiSsRead read, float cur, float lo, float hi, float fxcfg) {
    UiSsPick p;
    if (read == UiSsRead::kOk) {
        UiSsWhy why = UiSsWhy::kNone;
        if (uiLiveSupersamplingValid(cur, lo, hi, &why)) {
            p.ss = cur;
            p.source = UiSsSource::kLive;
            return p;
        }
        p.why = why;
    } else {
        p.why = read == UiSsRead::kFault ? UiSsWhy::kUnreadable : UiSsWhy::kNotCaptured;
    }
    if (fxcfg > 0.0f && std::isfinite(fxcfg)) {
        p.ss = fxcfg;
        p.source = UiSsSource::kFxcfg;
    }
    return p;
}

// The Supersampling the panel factor sees changed (the effective one, max(S, 1)): write now, past the settle.
inline bool uiPanelSsMoved(double lastEff, double nowEff) { return std::fabs(nowEff - lastEff) > 1e-6; }

// ------------------------------------------------------------------- the net --
//
// Last resort at the panel's CreateTexture2D: whatever the factor, a render or depth target over D3D11's limit
// on either axis is created shrunk to fit, aspect kept, so the create cannot be refused (a refused create is
// fatal in Elite). The longer side becomes the limit exactly; the other is scaled the same and floored, never
// under 1. Requests at or under the limit are not touched (false, outputs equal the inputs).
constexpr uint32_t kUiPanelNetLimit = 16384;  // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION

inline bool uiPanelNetShrink(uint32_t w, uint32_t h, uint32_t* outW, uint32_t* outH) {
    uint32_t nw = w, nh = h;
    const bool over = w > kUiPanelNetLimit || h > kUiPanelNetLimit;
    if (over) {
        if (w >= h) {
            nw = kUiPanelNetLimit;
            nh = static_cast<uint32_t>(static_cast<uint64_t>(h) * kUiPanelNetLimit / w);
        } else {
            nh = kUiPanelNetLimit;
            nw = static_cast<uint32_t>(static_cast<uint64_t>(w) * kUiPanelNetLimit / h);
        }
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
    }
    if (outW) *outW = nw;
    if (outH) *outH = nh;
    return over;
}

}  // namespace edvr
