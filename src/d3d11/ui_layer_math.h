// fix.ui_quality's arithmetic -- the UI layer (docs/ui-layer-2026-09-23.md,
// Design A of docs/crisp-ui-handoff.md) -- pure and header-only: no device,
// no Config, no Log. The DLL (src/d3d11/ui_layer.cpp) and its build-gate rig
// (tools/ui_layer_test) both include this one file, so the rig cannot test a
// copy that has drifted from the code that flies.
//
// WHAT THE LAYER IS. A classified UI draw is rasterised, with the game's own
// shaders and state, into an EDVR-owned per-eye RGBA target at the size of
// the frame that leaves EDVR's door (the upscaler's output -- the
// unit-quality size under DLSS) times the key's target, instead of into the
// game's eye target at the scene's render size. The layer is composited over
// the finished eye after the upscale and RCAS. Four pieces of arithmetic:
//
//   * the SIZE of the layer, and its memory;
//   * the MAP from a pixel of the game's eye target to a pixel of the layer
//     -- the viewport and scissor rects of a redirected draw pass through
//     it -- plus the JITTER CANCEL, so the UI is rasterised unjittered;
//   * the BLEND conversion that makes the layer a premultiplied colour plus
//     a TRANSMITTANCE (what of the frame still shows through), so one
//     composite reproduces what the game's own blends painted, in order;
//   * the COMPOSITE's filter: an exact box (area) filter of the layer over
//     each output pixel's footprint -- a copy at 1.0, a supersampled
//     downsample at 1.25.
//
// WHY TRANSMITTANCE AND NOT COVERAGE. The design (A3) kept coverage in alpha,
// which cannot express an OPAQUE draw: D3D11's blend is src*f1 + dst*f2 with
// no constant term, so no factor pair turns a shader's alpha into a stored 1.
// Transmittance can: cleared to 1, an opaque draw multiplies it by ZERO
// (ZERO, ZERO), an over by (1 - a) (ZERO, INV_SRC_ALPHA), light that covers
// nothing by one (ZERO, ONE). The composite is then out = L.rgb + F.rgb * L.a,
// exact for any sequence of the accepted shapes by the associativity of
// premultiplied over.
//
// Conventions are temporal_math.h's: a jitter of (jx, jy) render pixels
// means the rendered content sits jx pixels RIGHT and jy pixels DOWN of where
// the unjittered projection would put it; texture rows count downward.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {

// ---------------------------------------------------------------- the key --

// "off" | "1.0" | "1.25" -> 0 (off) | 1.0 | 1.25. Exact text, the way
// fix.hud_quality and fix.settlement_detail read their choices: "1.00" is
// not "1.0". Anything else is off, and *recognized says so for the log.
inline float uiQualityParse(const char* text, bool* recognized) {
    if (recognized) *recognized = true;
    if (!text) {
        if (recognized) *recognized = false;
        return 0.0f;
    }
    if (std::strcmp(text, "off") == 0) return 0.0f;
    if (std::strcmp(text, "1.0") == 0) return 1.0f;
    if (std::strcmp(text, "1.25") == 0) return 1.25f;
    if (recognized) *recognized = false;
    return 0.0f;
}

// --------------------------------------------------------- size and memory --

struct UiLayerSize {
    uint32_t w = 0, h = 0;
};

// round-half-up(v * target); 0 for a dead input.
inline uint32_t uiLayerDim(uint32_t v, float target) {
    if (v == 0 || !(target > 0.0f) || !std::isfinite(target)) return 0;
    const double d = static_cast<double>(v) * static_cast<double>(target) + 0.5;
    const uint32_t r = static_cast<uint32_t>(d);
    return r ? r : 1u;
}

// The layer: the frame the door hands on, times the target. D3D11's largest
// 2D texture is 16384 a side; a layer that would exceed it is refused (0x0),
// never clamped -- a clamped layer would put the UI at the wrong size.
inline UiLayerSize uiLayerSize(uint32_t outW, uint32_t outH, float target) {
    UiLayerSize s;
    s.w = uiLayerDim(outW, target);
    s.h = uiLayerDim(outH, target);
    if (s.w == 0 || s.h == 0 || s.w > 16384u || s.h > 16384u) return UiLayerSize{};
    return s;
}

// Bytes of one 8-bit RGBA surface of this size: the layer, and the
// composite's own output (the door's size, the same 4 bytes a pixel for the
// 8-bit families the composite accepts). Megabytes are decimal, the unit
// the design's budget was written in (74 MB at 4340x4284).
inline uint64_t uiLayerBytes(uint32_t w, uint32_t h, uint32_t bytesPerPixel = 4u) {
    return static_cast<uint64_t>(w) * h * bytesPerPixel;
}
inline double uiLayerMB(uint64_t bytes) { return static_cast<double>(bytes) / 1.0e6; }

// ------------------------------------------------------------- the map --

// XL = ax * X + bx, YL = ay * Y + by: a pixel of the game's eye target (X, Y)
// to a pixel of the layer. The eye's image in the game's target is the
// rectangle (x0, y0, w, h) -- the whole texture for a per-eye target -- and
// it covers the frustum the layer covers, so the map is a scale and an
// offset. The tangent form of the design (crisp-ui-handoff.md A3) is kept
// below for the day the frustum the game was told differs from the one the
// layer covers; the rig pins the two equal when they agree. On the native
// runtime the door's crop is applied at the COMPOSITE (the frame's bounds
// name the layer's rectangle), so the draw-time map is always this one.
struct UiLayerMap {
    float ax = 1.0f, bx = 0.0f, ay = 1.0f, by = 0.0f;
};

inline UiLayerMap uiLayerMapFromRegion(float x0, float y0, float w, float h,
                                       uint32_t layerW, uint32_t layerH) {
    UiLayerMap m;
    if (!(w > 0.0f) || !(h > 0.0f) || layerW == 0 || layerH == 0) return m;
    m.ax = static_cast<float>(layerW) / w;
    m.ay = static_cast<float>(layerH) / h;
    m.bx = -m.ax * x0;
    m.by = -m.ay * y0;
    return m;
}

// The tangent form. told = the frustum the game rendered with, truth = the
// frustum the layer covers, both {l, r, t, b} in OpenVR's raw convention,
// where row 0 looks along b (temporal_math.h). The game's region (x0, y0,
// w, h) spans `told`.
inline UiLayerMap uiLayerMapFromTangents(const float told[4], const float truth[4], float x0,
                                         float y0, float w, float h, uint32_t layerW,
                                         uint32_t layerH) {
    UiLayerMap m;
    const float tw = truth[1] - truth[0], th = truth[2] - truth[3];
    if (!(w > 0.0f) || !(h > 0.0f) || tw == 0.0f || th == 0.0f || layerW == 0 || layerH == 0) {
        return m;
    }
    const float Lw = static_cast<float>(layerW), Lh = static_cast<float>(layerH);
    m.ax = (Lw / w) * (told[1] - told[0]) / tw;
    m.bx = Lw * (told[0] - truth[0]) / tw - m.ax * x0;
    // Rows run from b (row 0) toward t, in both the game's region and the
    // layer: row R of the game's region looks along told b + R/h (t' - b').
    m.ay = (Lh / h) * (told[2] - told[3]) / th;
    m.by = Lh * (told[3] - truth[3]) / th - m.ay * y0;
    return m;
}

struct UiViewport {
    float x = 0, y = 0, w = 0, h = 0, minZ = 0, maxZ = 1;
};

// The jitter cancel, in layer pixels. The game's content sits (jx, jy)
// render pixels right and down of its unjittered place; through the map
// that is (jx * ax, jy * ay) layer pixels, so the redirected viewport moves
// by the negative. Exact whether or not the game's viewport spans its whole
// target, because the jitter is a property of the projection.
inline void uiLayerJitterCancel(float jx, float jy, const UiLayerMap& m, float* cancelX,
                                float* cancelY) {
    if (cancelX) *cancelX = -jx * m.ax;
    if (cancelY) *cancelY = -jy * m.ay;
}

// The pixel jitter from the tangent shift the projection was actually given
// (temporalJitterToTangents: dx = -jx * (r - l) / w, dy = +jy * (b - t) / h,
// over a render region w x h) -- native_temporal.cpp's own inverse.
inline void uiLayerJitterFromTangentShift(float dx, float dy, const float tan[4], float w,
                                          float h, float* jx, float* jy) {
    const float rl = tan[1] - tan[0], bt = tan[3] - tan[2];
    if (jx) *jx = (rl != 0.0f) ? -dx * w / rl : 0.0f;
    if (jy) *jy = (bt != 0.0f) ? dy * h / bt : 0.0f;
}

// A game viewport through the map, plus the cancel. Depth range unchanged.
inline UiViewport uiLayerMapViewport(const UiLayerMap& m, const UiViewport& v, float cancelX,
                                     float cancelY) {
    UiViewport o = v;
    o.x = m.ax * v.x + m.bx + cancelX;
    o.y = m.ay * v.y + m.by + cancelY;
    o.w = m.ax * v.w;
    o.h = m.ay * v.h;
    return o;
}

struct UiRect {
    int32_t l = 0, t = 0, r = 0, b = 0;
};

// Outward rounding with a thousandth of a pixel of slack, which keeps an
// exact edge exact through float error (1995 * (3070/1995.f) is not always
// 3070.0 in float).
inline int64_t uiLayerFloorEdge(double v) { return static_cast<int64_t>(std::floor(v + 1e-3)); }
inline int64_t uiLayerCeilEdge(double v) { return static_cast<int64_t>(std::ceil(v - 1e-3)); }

// A scissor rect through the same map, rounded OUTWARD (a clip must never
// lose a pixel the game's clip kept) and clamped to the layer. The jitter
// cancel moves the scissor with the content it clips. A rect that spans the
// game's whole target maps to the whole layer exactly (the rig pins it).
inline UiRect uiLayerMapScissor(const UiLayerMap& m, const UiRect& r, float cancelX,
                                float cancelY, uint32_t layerW, uint32_t layerH) {
    const double l = static_cast<double>(m.ax) * r.l + m.bx + cancelX;
    const double t = static_cast<double>(m.ay) * r.t + m.by + cancelY;
    const double rr = static_cast<double>(m.ax) * r.r + m.bx + cancelX;
    const double bb = static_cast<double>(m.ay) * r.b + m.by + cancelY;
    int64_t L = uiLayerFloorEdge(l), T = uiLayerFloorEdge(t), R = uiLayerCeilEdge(rr),
            B = uiLayerCeilEdge(bb);
    const int64_t W = layerW, H = layerH;
    L = L < 0 ? 0 : (L > W ? W : L);
    T = T < 0 ? 0 : (T > H ? H : T);
    R = R < 0 ? 0 : (R > W ? W : R);
    B = B < 0 ? 0 : (B > H ? H : B);
    UiRect o;
    o.l = static_cast<int32_t>(L);
    o.t = static_cast<int32_t>(T);
    o.r = static_cast<int32_t>(R < L ? L : R);
    o.b = static_cast<int32_t>(B < T ? T : B);
    return o;
}

// --------------------------------------------------------- the blends --

// D3D11_BLEND / D3D11_BLEND_OP values, numerically (ui_layer_shaders.h
// static_asserts them against d3d11.h), so this header needs no device
// headers.
namespace uiblend {
constexpr uint8_t kZero = 1, kOne = 2, kSrcColor = 3, kInvSrcColor = 4, kSrcAlpha = 5,
                  kInvSrcAlpha = 6, kDestAlpha = 7, kInvDestAlpha = 8, kDestColor = 9,
                  kInvDestColor = 10, kSrcAlphaSat = 11, kBlendFactor = 14,
                  kInvBlendFactor = 15, kSrc1Color = 16, kInvSrc1Color = 17, kSrc1Alpha = 18,
                  kInvSrc1Alpha = 19;
constexpr uint8_t kOpAdd = 1;
constexpr uint8_t kWriteRgb = 0x7, kWriteAlpha = 0x8, kWriteAll = 0xF;
}  // namespace uiblend

// One render target's blend: the fields of D3D11_RENDER_TARGET_BLEND_DESC.
struct UiBlendRt {
    bool enable = false;
    uint8_t src = uiblend::kOne, dst = uiblend::kZero, op = uiblend::kOpAdd;
    uint8_t srcA = uiblend::kOne, dstA = uiblend::kZero, opA = uiblend::kOpAdd;
    uint8_t mask = uiblend::kWriteAll;
};

// What a draw's colour blend does to the frame beneath it.
enum class UiBlendShape : uint8_t {
    kOpaque,          // blending off: covers what it draws
    kOver,            // SRC_ALPHA, INV_SRC_ALPHA: the classic over
    kPremulOver,      // ONE, INV_SRC_ALPHA: premultiplied over
    kAdditive,        // ONE, ONE: light added, covers nothing
    kScaledAdditive,  // SRC_ALPHA, ONE: light added, covers nothing
    kRefused,         // anything else: the draw stays in the game's frame
};

inline const char* uiBlendShapeName(UiBlendShape s) {
    switch (s) {
        case UiBlendShape::kOpaque: return "opaque";
        case UiBlendShape::kOver: return "over";
        case UiBlendShape::kPremulOver: return "premultiplied over";
        case UiBlendShape::kAdditive: return "additive";
        case UiBlendShape::kScaledAdditive: return "scaled additive";
        default: return "refused";
    }
}

// The shape of the colour equation. Refused: a subtract/min/max op,
// destination-colour or destination-alpha factors (the layer's alpha is not
// the frame's), dual-source, a blend factor, and a write mask with no colour
// in it -- or, for a shape that COVERS (opaque, over), a mask that leaves
// any colour channel out: transmittance is one number for all three, so a
// channel the game kept would come out of the composite covered anyway.
// Light that covers nothing is exact under any colour mask. Alpha-to-
// coverage and logic ops are refused by the caller, which reads them from
// the state object.
inline UiBlendShape uiLayerBlendShape(const UiBlendRt& b) {
    if ((b.mask & uiblend::kWriteRgb) == 0) return UiBlendShape::kRefused;
    const bool allColour = (b.mask & uiblend::kWriteRgb) == uiblend::kWriteRgb;
    if (!b.enable) return allColour ? UiBlendShape::kOpaque : UiBlendShape::kRefused;
    if (b.op != uiblend::kOpAdd) return UiBlendShape::kRefused;
    using namespace uiblend;
    if (b.src == kSrcAlpha && b.dst == kInvSrcAlpha)
        return allColour ? UiBlendShape::kOver : UiBlendShape::kRefused;
    if (b.src == kOne && b.dst == kInvSrcAlpha)
        return allColour ? UiBlendShape::kPremulOver : UiBlendShape::kRefused;
    if (b.src == kOne && b.dst == kOne) return UiBlendShape::kAdditive;
    if (b.src == kSrcAlpha && b.dst == kOne) return UiBlendShape::kScaledAdditive;
    return UiBlendShape::kRefused;
}

// The layer's blend for a draw: the game's colour equation and colour write
// mask kept (an opaque draw becomes the equivalent ONE, ZERO), the alpha
// equation replaced so the layer's alpha is TRANSMITTANCE, and alpha always
// written:
//
//   | game colour blend          | layer colour | layer alpha (src, dst)  |
//   |----------------------------|--------------|-------------------------|
//   | off (opaque)               | ONE, ZERO    | ZERO, ZERO   T' = 0     |
//   | SRC_ALPHA, INV_SRC_ALPHA   | as the game  | ZERO, INV_SRC_ALPHA     |
//   | ONE, INV_SRC_ALPHA         | as the game  | ZERO, INV_SRC_ALPHA     |
//   | ONE, ONE                   | as the game  | ZERO, ONE    T' = T     |
//   | SRC_ALPHA, ONE             | as the game  | ZERO, ONE    T' = T     |
//
// False for a refused shape: the draw is not redirected.
inline bool uiLayerConvertBlend(const UiBlendRt& in, UiBlendRt* out) {
    const UiBlendShape s = uiLayerBlendShape(in);
    if (s == UiBlendShape::kRefused || !out) return false;
    UiBlendRt o;
    o.enable = true;
    o.op = o.opA = uiblend::kOpAdd;
    o.mask = static_cast<uint8_t>((in.mask & uiblend::kWriteRgb) | uiblend::kWriteAlpha);
    o.srcA = uiblend::kZero;
    switch (s) {
        case UiBlendShape::kOpaque:
            o.src = uiblend::kOne;
            o.dst = uiblend::kZero;
            o.dstA = uiblend::kZero;
            break;
        case UiBlendShape::kOver:
        case UiBlendShape::kPremulOver:
            o.src = in.src;
            o.dst = in.dst;
            o.dstA = uiblend::kInvSrcAlpha;
            break;
        default:  // the additive shapes
            o.src = in.src;
            o.dst = in.dst;
            o.dstA = uiblend::kOne;
            break;
    }
    *out = o;
    return true;
}

// The CPU model of the output-merger blend for the factors the accepted
// shapes use, for the rig: the game's draws applied to a frame directly
// against the same draws applied to a layer and composited.
struct UiPx {
    float r = 0, g = 0, b = 0, a = 0;
};
inline float uiSat(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float uiBlendFactor(uint8_t f, const UiPx& s, const UiPx& d) {
    using namespace uiblend;
    switch (f) {
        case kZero: return 0.0f;
        case kOne: return 1.0f;
        case kSrcAlpha: return s.a;
        case kInvSrcAlpha: return 1.0f - s.a;
        case kDestAlpha: return d.a;
        case kInvDestAlpha: return 1.0f - d.a;
        default: return 0.0f;  // colour factors are refused shapes
    }
}
inline UiPx uiBlendApply(const UiBlendRt& b, const UiPx& s, const UiPx& d) {
    UiPx o = d;
    if (!b.enable) {
        if (b.mask & 1) o.r = uiSat(s.r);
        if (b.mask & 2) o.g = uiSat(s.g);
        if (b.mask & 4) o.b = uiSat(s.b);
        if (b.mask & 8) o.a = uiSat(s.a);
        return o;
    }
    const float fs = uiBlendFactor(b.src, s, d), fd = uiBlendFactor(b.dst, s, d);
    const float fsa = uiBlendFactor(b.srcA, s, d), fda = uiBlendFactor(b.dstA, s, d);
    if (b.mask & 1) o.r = uiSat(s.r * fs + d.r * fd);
    if (b.mask & 2) o.g = uiSat(s.g * fs + d.g * fd);
    if (b.mask & 4) o.b = uiSat(s.b * fs + d.b * fd);
    if (b.mask & 8) o.a = uiSat(s.a * fsa + d.a * fda);
    return o;
}

// The composite, per pixel: out = L.rgb + F.rgb * L.a (L.a = transmittance),
// in the space the game's own composite blended in -- the stored, encoded
// values through a UNORM view -- with the frame's alpha kept. The layer is
// cleared to (0, 0, 0, 1): nothing drawn, everything of the frame shows.
inline UiPx uiLayerCompositePx(const UiPx& layer, const UiPx& frame) {
    UiPx o;
    o.r = uiSat(layer.r + frame.r * layer.a);
    o.g = uiSat(layer.g + frame.g * layer.a);
    o.b = uiSat(layer.b + frame.b * layer.a);
    o.a = frame.a;
    return o;
}

// ------------------------------------------------------ the composite filter --

// The layer texels a footprint [x0, x1) covers along one axis, with their
// area weights normalised to 1. Output pixel i of a region whose layer
// rectangle starts at layer coordinate o with s layer texels a pixel covers
// [o + i*s, o + (i+1)*s): one texel with weight 1 at s = 1 on the grid (a
// copy), up to three at s = 1.25. The HLSL in ui_layer_shaders.h is this
// loop, transcribed; the rig compares the two on WARP.
constexpr int kUiLayerMaxTaps = 4;  // s <= 2 needs at most 3; one spare
inline int uiLayerFootprint(double x0, double x1, uint32_t layerDim, uint32_t* first,
                            float weights[kUiLayerMaxTaps]) {
    if (!(x1 > x0) || layerDim == 0 || !first) return 0;
    int64_t k0 = static_cast<int64_t>(std::floor(x0));
    int64_t k1 = static_cast<int64_t>(std::ceil(x1)) - 1;
    if (k0 < 0) k0 = 0;
    if (k1 > static_cast<int64_t>(layerDim) - 1) k1 = static_cast<int64_t>(layerDim) - 1;
    int n = 0;
    double total = 0.0;
    for (int64_t k = k0; k <= k1 && n < kUiLayerMaxTaps; ++k) {
        const double a = (x0 > static_cast<double>(k)) ? x0 : static_cast<double>(k);
        const double b = (x1 < static_cast<double>(k + 1)) ? x1 : static_cast<double>(k + 1);
        const double w = b - a;
        if (w <= 1e-9) continue;
        if (n == 0) *first = static_cast<uint32_t>(k);
        weights[n++] = static_cast<float>(w);
        total += w;
    }
    for (int j = 0; j < n; ++j) weights[j] = static_cast<float>(weights[j] / total);
    return n;
}

// ------------------------------------------------------ the classifier gate --

// Which piece of the interface an eye draw is: recognised in vscreen.cpp
// (the 2D screen's composite, the way the panel distance and the curved
// screen already recognise it) and in ui_depth.cpp (a composite of a learned
// interface surface, named by its vertex shader), decided here.
enum class UiLayerFamily : uint8_t {
    kNone = 0,
    kScreen,     // the 2D screen's composite (main menu, station services,
                 // maps, the on-foot view and its helmet HUD, cinema): samples
                 // the forced-size panel at PS slot 0
    kPanel,      // a menu / modal panel composite of a learned surface
                 // (vs A888D51024D9798E)
    kLoader,     // the loading screen's composite (vs 4EF6DDB075A927FA)
    kSurface,    // any other eye draw sampling a learned interface surface
    kGuiDirect,  // a GUI-family draw (vector/text/icon) straight into an eye
    kHolo,       // cockpit holo panels (vs 81216C77F90DEDD6)
    kFlightHud,  // flight HUD (vs B7790CBFC6554097)
    kSprite,     // target-time sprite (vs E508648660A352B2)
    kCount
};

inline const char* uiLayerFamilyName(UiLayerFamily f) {
    switch (f) {
        case UiLayerFamily::kScreen: return "2D screen";
        case UiLayerFamily::kPanel: return "menu panel";
        case UiLayerFamily::kLoader: return "loading screen";
        case UiLayerFamily::kSurface: return "interface composite";
        case UiLayerFamily::kGuiDirect: return "GUI draw into the eye";
        case UiLayerFamily::kHolo: return "cockpit holo panels";
        case UiLayerFamily::kFlightHud: return "flight HUD";
        case UiLayerFamily::kSprite: return "target sprite";
        default: return "none";
    }
}

// Why a UI draw was left in the game's frame (or kRedirect). The order is
// the order of the tests in uiLayerDecide, so the reason reported is the
// first that failed.
enum class UiLayerDecision : uint8_t {
    kRedirect = 0,
    kNotUi,          // no family
    kVerdict,        // another fix swallows or re-issues the draw
    kNotEyeTarget,   // the colour target is not an eye-sized 2D target
    kHdrTarget,      // drawn into the lit HDR target BEFORE exposure and the
                     // tonemap: the layer is composited after both, so taking
                     // it would lose the game's exposure, tonemap and bloom
    kVrs,            // variable-rate shading is bound for the eye (foveation)
    kNoEye,          // the eye could not be told
    kTargetSize,     // the target is not the size of the region the game
                     // submits for that eye (the map is target-to-layer)
    kLate,           // its eye's composite already ran this frame (gate G1)
    kNotArmed,       // no door frame for this eye last frame (first frames,
                     // key just on, pass not running)
    kMrt,            // more than one render target bound
    kDepthStencil,   // tests or writes depth, or stencil, against a bound
                     // depth target: the layer has none, and dropping a write
                     // another draw reads would change the game's picture
    kBlendRefused,   // a blend with no premultiplied form
    kLayerFailed,    // the layer could not be created
    kCount
};

inline const char* uiLayerDecisionName(UiLayerDecision d) {
    switch (d) {
        case UiLayerDecision::kRedirect: return "redirected into the layer";
        case UiLayerDecision::kNotUi: return "not UI";
        case UiLayerDecision::kVerdict: return "another fix swallows or re-issues it";
        case UiLayerDecision::kNotEyeTarget: return "not drawn into an eye target";
        case UiLayerDecision::kHdrTarget:
            return "drawn into the HDR target before the tonemap (left in the picture; under an "
                   "external engine the deferred UI replay re-draws it after the upscale)";
        case UiLayerDecision::kVrs: return "variable-rate shading bound for the eye";
        case UiLayerDecision::kNoEye: return "eye unknown";
        case UiLayerDecision::kTargetSize:
            return "its target is not the size of the eye the game submits";
        case UiLayerDecision::kLate: return "arrived after its eye's composite (gate G1)";
        case UiLayerDecision::kNotArmed: return "layer not armed";
        case UiLayerDecision::kMrt: return "more than one render target, or pixel-shader UAVs";
        case UiLayerDecision::kDepthStencil: return "depth- or stencil-tested or -writing";
        case UiLayerDecision::kBlendRefused: return "blend with no premultiplied form";
        case UiLayerDecision::kLayerFailed: return "layer creation failed";
        default: return "?";
    }
}

// The facts the draw path gathers; the decision is a pure function of them.
struct UiLayerDrawFacts {
    UiLayerFamily family = UiLayerFamily::kNone;
    bool verdictForwards = true;  // the draw is forwarded as-is by its verdict
    bool eyeTarget = false;       // an eye-sized 2D colour target
    bool ldrView = false;         // ... viewed as 8-bit UNORM (post-tonemap)
    bool vrs = false;             // variable-rate shading bound
    int eye = -1;                 // 0 left, 1 right, -1 unknown
    bool targetMatchesEye = true; // the target is the submitted region's size
    bool late = false;            // its eye's door already ran this frame
    bool armed = false;           // the door and the pass ran for it last frame
    bool mrt = false;             // a second render target, or PS UAVs, bound
    bool depthStencil = false;    // tests/writes depth or stencil vs a bound DSV
    UiBlendShape blend = UiBlendShape::kRefused;
    bool layerReady = true;       // the eye's layer exists at the wanted size
};

inline UiLayerDecision uiLayerDecide(const UiLayerDrawFacts& f) {
    if (f.family == UiLayerFamily::kNone) return UiLayerDecision::kNotUi;
    if (!f.verdictForwards) return UiLayerDecision::kVerdict;
    if (!f.eyeTarget) return UiLayerDecision::kNotEyeTarget;
    if (!f.ldrView) return UiLayerDecision::kHdrTarget;
    if (f.vrs) return UiLayerDecision::kVrs;
    if (f.eye < 0 || f.eye > 1) return UiLayerDecision::kNoEye;
    if (!f.targetMatchesEye) return UiLayerDecision::kTargetSize;
    if (f.late) return UiLayerDecision::kLate;
    if (!f.armed) return UiLayerDecision::kNotArmed;
    if (f.mrt) return UiLayerDecision::kMrt;
    if (f.depthStencil) return UiLayerDecision::kDepthStencil;
    if (f.blend == UiBlendShape::kRefused) return UiLayerDecision::kBlendRefused;
    if (!f.layerReady) return UiLayerDecision::kLayerFailed;
    return UiLayerDecision::kRedirect;
}

// Armed for eye e at frame `sequence`: the door ran for that eye in the
// frame before, fed by the temporal pass's own output, and it published a
// size. A draw whose eye's door already ran THIS frame is late (G1).
struct UiLayerDoorState {
    uint64_t doorSeq = 0;       // last sequence the door step ran for this eye
    uint64_t treatedSeq = 0;    // last sequence the temporal pass treated it
    uint32_t fullW = 0, fullH = 0;  // the frame the door hands on, uncropped
};
inline bool uiLayerArmed(const UiLayerDoorState& d, uint64_t sequence) {
    return sequence > 1 && d.doorSeq + 1 == sequence && d.treatedSeq + 1 == sequence &&
           d.fullW && d.fullH;
}
inline bool uiLayerLateFor(const UiLayerDoorState& d, uint64_t sequence) {
    return sequence != 0 && d.doorSeq == sequence;
}

// The composite's rectangle of the layer: the door's input region -- the
// whole-pixel rectangle supersampleRegionFromBounds made of the Submit
// bounds, already unflipped (the frame and the layer store the eye in the
// same orientation) -- over its source's size. From the ROUNDED region and
// not the raw bounds: a cull-guard crop is an arbitrary fraction, and the
// half-pixel between the two would shift the UI and straddle every texel.
inline void uiLayerUvFromRegion(const uint32_t region[4], uint32_t sourceW, uint32_t sourceH,
                                float uv[4]) {
    if (!sourceW || !sourceH) {
        uv[0] = uv[1] = 0.0f;
        uv[2] = uv[3] = 1.0f;
        return;
    }
    uv[0] = static_cast<float>(static_cast<double>(region[0]) / sourceW);
    uv[1] = static_cast<float>(static_cast<double>(region[1]) / sourceH);
    uv[2] = static_cast<float>(static_cast<double>(region[2]) / sourceW);
    uv[3] = static_cast<float>(static_cast<double>(region[3]) / sourceH);
}

// Does a frame region, with the layer rectangle it claims, describe the
// same eye the layer does? The frame's implied uncropped shape must match
// the layer's aspect within 5% -- a double-wide texture's half claims a
// rectangle of the whole, twice as wide, and is refused rather than
// composited squashed.
inline bool uiLayerRegionMatches(uint32_t regionW, uint32_t regionH, const float uv[4],
                                 uint32_t layerW, uint32_t layerH) {
    const double du = static_cast<double>(uv[2]) - uv[0], dv = static_cast<double>(uv[3]) - uv[1];
    if (!(du > 1e-4) || !(dv > 1e-4) || !regionW || !regionH || !layerW || !layerH) return false;
    const double impliedAspect = (regionW / du) / (regionH / dv);
    const double layerAspect = static_cast<double>(layerW) / layerH;
    return std::fabs(impliedAspect / layerAspect - 1.0) < 0.05;
}

}  // namespace edvr
