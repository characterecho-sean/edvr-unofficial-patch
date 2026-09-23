// fix.ui_quality's arithmetic -- the UI layer (docs/ui-layer-2026-09-23.md,
// Design A of docs/crisp-ui-handoff.md) -- pure and header-only: no device,
// no Config, no Log. The DLL (src/d3d11/ui_layer.cpp) and its build-gate rig
// (tools/ui_quality_test) both include this one file, so the rig cannot test
// a copy that has drifted from the code that flies. The key's panel half has
// its own arithmetic, ui_sizing_math.h.
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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {

// ---------------------------------------------------------------- the key --

// "off" | "1.0" | "1.25" -> 0 (off) | 1.0 | 1.25. Exact text, the way
// fix.settlement_detail reads its choices: "1.00" is not "1.0". Anything
// else is off, and *recognized says so for the log. One reader for both
// halves (uiLayerConfigure hands the target to ui_panel_scale).
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
    kOpaque,          // blending off (or ONE, ZERO): covers what it draws
    kOver,            // SRC_ALPHA, INV_SRC_ALPHA: the classic over
    kPremulOver,      // ONE, INV_SRC_ALPHA: premultiplied over
    kAdditive,        // ONE, ONE: light added, covers nothing
    kScaledAdditive,  // SRC_ALPHA, ONE: light added, covers nothing
    kMultiply,        // ZERO, SRC_COLOR or DEST_COLOR, ZERO: the frame tinted
                      // by the source colour (the loading screen's gamma
                      // variant, ps 8ADB2A81A45E8A4B, census 2026-09-08)
    kRefused,         // anything else: the draw stays in the game's frame
};

inline const char* uiBlendShapeName(UiBlendShape s) {
    switch (s) {
        case UiBlendShape::kOpaque: return "opaque";
        case UiBlendShape::kOver: return "over";
        case UiBlendShape::kPremulOver: return "premultiplied over";
        case UiBlendShape::kAdditive: return "additive";
        case UiBlendShape::kScaledAdditive: return "scaled additive";
        case UiBlendShape::kMultiply: return "multiply";
        default: return "refused";
    }
}

// The shape of the colour equation. Refused: a subtract/min/max op,
// destination-alpha factors (the layer's alpha is not the frame's), dual-
// source, a blend factor, and a write mask with no colour in it -- or, for
// a shape that COVERS (opaque, over), a mask that leaves any colour channel
// out: transmittance is one number for all three, so a channel the game kept
// would come out of the composite covered anyway. Light that covers nothing
// and a multiply (per-channel by construction) are exact under any colour
// mask. Alpha-to-coverage and logic ops are refused by the caller, which
// reads them from the state object.
inline UiBlendShape uiLayerBlendShape(const UiBlendRt& b) {
    if ((b.mask & uiblend::kWriteRgb) == 0) return UiBlendShape::kRefused;
    const bool allColour = (b.mask & uiblend::kWriteRgb) == uiblend::kWriteRgb;
    if (!b.enable) return allColour ? UiBlendShape::kOpaque : UiBlendShape::kRefused;
    if (b.op != uiblend::kOpAdd) return UiBlendShape::kRefused;
    using namespace uiblend;
    if (b.src == kOne && b.dst == kZero)
        return allColour ? UiBlendShape::kOpaque : UiBlendShape::kRefused;
    if (b.src == kSrcAlpha && b.dst == kInvSrcAlpha)
        return allColour ? UiBlendShape::kOver : UiBlendShape::kRefused;
    if (b.src == kOne && b.dst == kInvSrcAlpha)
        return allColour ? UiBlendShape::kPremulOver : UiBlendShape::kRefused;
    if (b.src == kOne && b.dst == kOne) return UiBlendShape::kAdditive;
    if (b.src == kSrcAlpha && b.dst == kOne) return UiBlendShape::kScaledAdditive;
    if ((b.src == kZero && b.dst == kSrcColor) || (b.src == kDestColor && b.dst == kZero))
        return UiBlendShape::kMultiply;
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
//   | ZERO, SRC_COLOR (multiply) | as the game  | not written  T' = T     |
//
// A multiply also scales a second, per-channel transmittance M (its own
// RGBA8 target, cleared to 1), drawn by the same draw once more with
// uiLayerMultiplyBlend: the composite is then out = L.rgb + F.rgb * T * M.rgb,
// exact for any order of the accepted shapes (over: L' = c + L(1-a),
// T' = T(1-a); additive: L' = L + c; multiply: L' = L s, M' = M s).
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
        case UiBlendShape::kMultiply:
            o.src = in.src;
            o.dst = in.dst;
            o.dstA = uiblend::kOne;
            o.mask = static_cast<uint8_t>(in.mask & uiblend::kWriteRgb);  // T untouched
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

// The second draw of a multiply, into the per-channel transmittance M: the
// game's own colour factors and colour mask, alpha not written.
inline bool uiLayerMultiplyBlend(const UiBlendRt& in, UiBlendRt* out) {
    if (uiLayerBlendShape(in) != UiBlendShape::kMultiply || !out) return false;
    UiBlendRt o;
    o.enable = true;
    o.op = o.opA = uiblend::kOpAdd;
    o.src = in.src;
    o.dst = in.dst;
    o.srcA = uiblend::kZero;
    o.dstA = uiblend::kOne;
    o.mask = static_cast<uint8_t>(in.mask & uiblend::kWriteRgb);
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
// One factor for channel c (0..2 colour, 3 alpha): the colour factors read
// the same channel of the source or the destination, as the hardware does.
inline float uiBlendFactor(uint8_t f, const UiPx& s, const UiPx& d, int c) {
    using namespace uiblend;
    const float sc = c == 0 ? s.r : c == 1 ? s.g : c == 2 ? s.b : s.a;
    const float dc = c == 0 ? d.r : c == 1 ? d.g : c == 2 ? d.b : d.a;
    switch (f) {
        case kZero: return 0.0f;
        case kOne: return 1.0f;
        case kSrcAlpha: return s.a;
        case kInvSrcAlpha: return 1.0f - s.a;
        case kDestAlpha: return d.a;
        case kInvDestAlpha: return 1.0f - d.a;
        case kSrcColor: return sc;
        case kInvSrcColor: return 1.0f - sc;
        case kDestColor: return dc;
        case kInvDestColor: return 1.0f - dc;
        default: return 0.0f;
    }
}
inline UiPx uiBlendApply(const UiBlendRt& b, const UiPx& s, const UiPx& d) {
    UiPx o = d;
    const float sv[4] = {s.r, s.g, s.b, s.a}, dv[4] = {d.r, d.g, d.b, d.a};
    float ov[4] = {d.r, d.g, d.b, d.a};
    for (int c = 0; c < 4; ++c) {
        if (!(b.mask & (1 << c))) continue;
        if (!b.enable) {
            ov[c] = uiSat(sv[c]);
            continue;
        }
        const uint8_t fsrc = c < 3 ? b.src : b.srcA, fdst = c < 3 ? b.dst : b.dstA;
        ov[c] = uiSat(sv[c] * uiBlendFactor(fsrc, s, d, c) + dv[c] * uiBlendFactor(fdst, s, d, c));
    }
    o.r = ov[0];
    o.g = ov[1];
    o.b = ov[2];
    o.a = ov[3];
    return o;
}

// The composite, per pixel: out = L.rgb + F.rgb * L.a * M.rgb (L.a = the
// scalar transmittance, M = the per-channel one a multiply leaves; 1 where
// none drew), in the space the game's own composite blended in -- the
// stored, encoded values through a UNORM view -- with the frame's alpha
// kept. The layer is cleared to (0, 0, 0, 1) and M to (1, 1, 1, 1): nothing
// drawn, everything of the frame shows.
inline UiPx uiLayerCompositePx(const UiPx& layer, const UiPx& mult, const UiPx& frame) {
    UiPx o;
    o.r = uiSat(layer.r + frame.r * layer.a * mult.r);
    o.g = uiSat(layer.g + frame.g * layer.a * mult.g);
    o.b = uiSat(layer.b + frame.b * layer.a * mult.b);
    o.a = frame.a;
    return o;
}
inline UiPx uiLayerCompositePx(const UiPx& layer, const UiPx& frame) {
    UiPx one;
    one.r = one.g = one.b = one.a = 1.0f;
    return uiLayerCompositePx(layer, one, frame);
}

// ------------------------------------------------ depth and stencil --

// A draw's depth-stencil state, in D3D11_DEPTH_STENCIL_DESC's numbers
// (comparison 8 = ALWAYS, stencil op 1 = KEEP), plus the view's read-only
// flags, for the pure classification below.
namespace uids {
constexpr uint8_t kAlways = 8, kKeep = 1;
}
struct UiDsFace {
    uint8_t fail = uids::kKeep, depthFail = uids::kKeep, pass = uids::kKeep, func = uids::kAlways;
};
struct UiDsState {
    bool depthEnable = false;
    uint8_t depthFunc = uids::kAlways;
    bool depthWriteAll = false;
    bool stencilEnable = false;
    uint8_t readMask = 0xFF, writeMask = 0xFF;
    UiDsFace front, back;
    bool readOnlyDepth = false, readOnlyStencil = false;  // the view's flags
    bool stencilPlane = true;  // the view's format has stencil (D32_FLOAT has none)
};

// What a draw does with the depth-stencil target bound with it: whether its
// colour output depends on it (a TEST), and whether it changes it (a
// WRITE). Neither with no target bound: D3D11 passes both tests then.
struct UiDsEffect {
    bool depthTest = false, stencilTest = false, depthWrite = false, stencilWrite = false;
    bool tests() const { return depthTest || stencilTest; }
    bool writes() const { return depthWrite || stencilWrite; }
};
inline UiDsEffect uiLayerDsEffect(const UiDsState& s, bool dsvBound) {
    UiDsEffect e;
    if (!dsvBound) return e;
    e.depthTest = s.depthEnable && s.depthFunc != uids::kAlways;
    e.depthWrite = s.depthEnable && s.depthWriteAll && !s.readOnlyDepth;
    // Any function but ALWAYS is a test, whatever the read mask: with a mask
    // of 0 the outcome is constant, but NOT_EQUAL, LESS, GREATER and NEVER
    // are a constant FAIL, and a layer with no depth target would pass them.
    auto faceTests = [&](const UiDsFace& f) { return f.func != uids::kAlways; };
    auto faceWrites = [&](const UiDsFace& f) {
        return f.pass != uids::kKeep || f.fail != uids::kKeep || f.depthFail != uids::kKeep;
    };
    // A view with no stencil plane: D3D11 passes the stencil test and drops
    // the write, and so does the layer's copy in the same format -- there is
    // nothing to test against or seed (review P3-6: it re-seeded every draw).
    e.stencilTest = s.stencilEnable && s.stencilPlane && (faceTests(s.front) || faceTests(s.back));
    e.stencilWrite = s.stencilEnable && s.stencilPlane && s.writeMask != 0 && !s.readOnlyStencil &&
                     (faceWrites(s.front) || faceWrites(s.back));
    return e;
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

// THE FAMILY RULE, pure: vscreen.cpp's uiLayerFamilyOf gathers these facts
// for an owner draw into an eye target, in this order and only as far as the
// rule reads them, and this decides. The shader hashes are the ones the
// census and every flight since have named.
constexpr uint64_t kUiVsPanel = 0xA888D51024D9798Eull;   // menu / modal panel composite
constexpr uint64_t kUiVsLoader = 0x4EF6DDB075A927FAull;  // loading screen composite
constexpr uint64_t kUiVsHolo = 0x81216C77F90DEDD6ull;
constexpr uint64_t kUiVsFlightHud = 0xB7790CBFC6554097ull;
constexpr uint64_t kUiVsSprite = 0xE508648660A352B2ull;
constexpr uint64_t kUiVsGuiVector = 0x666EF0C4C616F67Eull, kUiVsGuiText = 0x1012E00B3CB44469ull,
                   kUiVsGuiIcons = 0xA3E5D3FCBC1165F8ull;
// The two composite families' own pixel shaders, as the layer's and ui_depth's
// family lines named them on 2026-09-23: the menu panel (and its variant),
// the loading screen (and its gamma pass). A draw of one of these pairs is
// that family by its shaders alone -- recognition that does not wait for
// ui_depth to learn the surface it samples. The 13:23 flight lost the menu
// panel for the rest of the session when the FOV trim, adopted at the main
// menu, had the game re-create its interface surfaces: the redirects stopped
// within a third of a second of the render-size change and never resumed,
// though ui_depth learned one of the new surfaces a second later.
constexpr uint64_t kUiPanelPs[] = {0x9107E72CB016CC02ull, 0x219323C8C025AD94ull};
constexpr uint64_t kUiLoaderPs[] = {0x85565E9261812E2Full, 0x8ADB2A81A45E8A4Bull};

inline bool uiKnownPs(const uint64_t* list, size_t n, uint64_t ps) {
    for (size_t i = 0; i < n; ++i)
        if (list[i] == ps) return true;
    return false;
}

struct UiFamilyFacts {
    int targetKind = 0;           // uiLayerTargetKind: 0 no eye target, 1 not 8-bit UNORM, 2 post-tonemap
    uint64_t vs = 0, ps = 0;      // the bound shaders' hashes (ps asked only where the rule needs it)
    bool excluded = false;        // ui_depth's exclude list
    bool panelSized = false;      // srv0IsPanelSized: the 2D screen's composite
    bool learnedSurface = false;  // a learned interface surface in PS slots 0..3
};

// How the rule reached its answer, for the family census (ui_layer.cpp).
enum class UiFamilyWhy : uint8_t {
    kNotEyeTarget = 0,  // not an eye target (the family rule never ran) -- vscreen's gate
    kNotPostTonemap,    // an eye target, but not the 8-bit post-tonemap one
    kExcluded,          // on ui_depth's exclude list
    kScreen,            // the 2D screen, by its panel-sized SRV
    kLearnedSurface,    // by a learned interface surface
    kShaderPair,        // by its shader pair alone (no learned surface)
    kDirect,            // a GUI or flight-HUD shader straight into the eye
    kNoSurface,         // a composite shader with no learned surface and an unknown pair
    kOther,             // any other draw
    kCount
};

inline const char* uiFamilyWhyName(UiFamilyWhy w) {
    switch (w) {
        case UiFamilyWhy::kNotEyeTarget: return "not an eye target";
        case UiFamilyWhy::kNotPostTonemap: return "not the post-tonemap target";
        case UiFamilyWhy::kExcluded: return "excluded";
        case UiFamilyWhy::kScreen: return "the 2D screen's SRV";
        case UiFamilyWhy::kLearnedSurface: return "a learned surface";
        case UiFamilyWhy::kShaderPair: return "its shader pair alone";
        case UiFamilyWhy::kDirect: return "a direct shader";
        case UiFamilyWhy::kNoSurface: return "no learned surface, pixel shader not known";
        default: return "other";
    }
}

inline UiLayerFamily uiLayerFamilyFor(const UiFamilyFacts& f, UiFamilyWhy* why = nullptr) {
    UiFamilyWhy w = UiFamilyWhy::kOther;
    UiLayerFamily out = UiLayerFamily::kNone;
    if (f.targetKind == 0) {
        w = UiFamilyWhy::kNotEyeTarget;
    } else if (f.targetKind == 1) {
        w = UiFamilyWhy::kNotPostTonemap;
        out = f.vs == kUiVsHolo        ? UiLayerFamily::kHolo
              : f.vs == kUiVsFlightHud ? UiLayerFamily::kFlightHud
              : f.vs == kUiVsSprite    ? UiLayerFamily::kSprite
                                       : UiLayerFamily::kNone;
        if (out != UiLayerFamily::kNone) w = UiFamilyWhy::kDirect;
    } else if (f.excluded) {
        w = UiFamilyWhy::kExcluded;
    } else if (f.panelSized) {
        w = UiFamilyWhy::kScreen;
        out = UiLayerFamily::kScreen;
    } else if (f.learnedSurface) {
        w = UiFamilyWhy::kLearnedSurface;
        out = f.vs == kUiVsPanel    ? UiLayerFamily::kPanel
              : f.vs == kUiVsLoader ? UiLayerFamily::kLoader
              : f.vs == kUiVsHolo   ? UiLayerFamily::kHolo
              : f.vs == kUiVsSprite ? UiLayerFamily::kSprite
                                    : UiLayerFamily::kSurface;
    } else if (f.vs == kUiVsPanel && uiKnownPs(kUiPanelPs, sizeof(kUiPanelPs) / sizeof(kUiPanelPs[0]), f.ps)) {
        w = UiFamilyWhy::kShaderPair;
        out = UiLayerFamily::kPanel;
    } else if (f.vs == kUiVsLoader &&
               uiKnownPs(kUiLoaderPs, sizeof(kUiLoaderPs) / sizeof(kUiLoaderPs[0]), f.ps)) {
        w = UiFamilyWhy::kShaderPair;
        out = UiLayerFamily::kLoader;
    } else if (f.vs == kUiVsGuiVector || f.vs == kUiVsGuiText || f.vs == kUiVsGuiIcons) {
        w = UiFamilyWhy::kDirect;
        out = UiLayerFamily::kGuiDirect;
    } else if (f.vs == kUiVsFlightHud) {
        w = UiFamilyWhy::kDirect;
        out = UiLayerFamily::kFlightHud;
    } else if (f.vs == kUiVsPanel || f.vs == kUiVsLoader) {
        w = UiFamilyWhy::kNoSurface;
    }
    if (why) *why = w;
    return out;
}

// THE ON-FOOT GATE. On foot, Odyssey renders the world ONCE, flat, into the
// 2D screen's 3840x2160 target and shows it as a panel in each eye through
// the screen's composite (flight 2026-09-23 09:38: vs 5C36AF051B98B9F1 ps
// CFE84157BC76E921, one draw an eye; 5.5k-15.6k draws a frame into the
// screen's own depth target, none into an eye). There the composite IS the
// game world: the layer taking it handed the temporal pass a black eye (the
// luma probe read game=0.000 on every sample for two minutes) and put the
// world on screen with no temporal pass at all -- the distant hills
// shimmered. So while the commander is on foot the 2D screen stays in the
// game's frame, the helmet HUD (drawn into the same texture) with it;
// everything else the layer takes, it still takes.
//
// The reading is the journal watcher's, of the game's Status.json (Flags2
// bit 0: journalOnFootKnown && journalOnFoot, the LOD governor's and the
// on-foot frame pacing's source). It LAGS: the game rewrites the file about
// once a second and the watcher reads it twice a second (the 09:38 flight:
// the screen went on foot at 09:41:04.442, the journal said so at
// 09:41:05.003) -- the transitions it lags are behind a loading screen.
// Unknown (a menu, the watcher off) does not hold the gate: the screen is
// taken, as before the gate. Held through a SHORT unknown, though: a read
// that caught the file mid-write finds no Flags2 and reads as unknown, and
// the direction that breaks the picture is taking the world, so on foot
// holds until the journal says aboard, or has said nothing for kHoldMs.
struct UiOnFootGate {
    int8_t state = -1;          // -1 never read; 0 the screen is taken; 1 on foot: left
    uint64_t lastOnFootMs = 0;  // the last reading that said on foot
};
constexpr uint64_t kUiOnFootHoldMs = 3000;

// One reading of the journal's pair at nowMs; true while the gate holds.
inline bool uiLayerOnFootStep(UiOnFootGate& g, bool known, bool onFoot, uint64_t nowMs) {
    if (known && onFoot) {
        g.lastOnFootMs = nowMs;
        g.state = 1;
    } else if (known) {
        g.state = 0;  // aboard: released at once
    } else if (!(g.state == 1 && nowMs - g.lastOnFootMs < kUiOnFootHoldMs)) {
        g.state = 0;  // unknown, and not a blip inside an on-foot stretch
    }
    return g.state == 1;
}

// THE SCREEN'S OWN IDENTITY (the UI architecture review's P1, 2026-09-23).
// The journal is optional -- d3d11.journal_watch, and its folder must be
// found -- and a second late: with it off the gate above never holds and
// the on-foot world goes back into the layer. The world on the 2D screen is
// told by its content too: a depth target of the 2D screen's own size takes
// the world's draws. The depth probe's census over every September log of
// both installs: 175 to 17,799 draws a frame in 206 samples across 57
// on-foot sessions (the six of 2026-09-23: 381 to 15,612), while no census
// outside an on-foot session shows a screen-sized depth target with even
// 10 -- the UI's own take 3 or 4 a frame. No log caught station services
// or a map on the screen (Status.json's GuiFocus is logged beside the
// count now, so the next flight names them). A map, or any
// other 3D scene drawn through the 2D screen, is moving content for the
// same reason as the world, and is held the same way; only a static screen
// -- the menus, station services' panels -- is quiet, and goes to the layer.
//
// Hysteresis both ways: the screen becomes the world after
// kUiWorldEnterFrames frames running over kUiWorldEnterDraws, and stops
// being it after kUiWorldLeaveFrames frames running under
// kUiWorldLeaveDraws (a frame between the two restarts that run): one busy
// transition frame does not blur a menu, one quiet frame on foot does not
// hand the temporal pass a black eye. Either signal holds the screen in the
// picture (the journal's or this one); neither: the layer takes it, so an
// unknown journal with a quiet screen -- the main menu -- stays sharp.
constexpr uint32_t kUiWorldEnterDraws = 64;   // 2.7x under the on-foot minimum, 16x the UI's
constexpr uint32_t kUiWorldEnterFrames = 2;
constexpr uint32_t kUiWorldLeaveDraws = 32;
constexpr uint32_t kUiWorldLeaveFrames = 90;  // a second at 90 Hz

struct UiWorldScreenGate {
    bool busy = false;
    uint32_t run = 0;    // frames running toward the other state
    uint32_t draws = 0;  // the count the last step judged by
};

// One frame's count: the most draws a depth target of the 2D screen's size
// took (known: the depth probe watches and the screen's size is known; an
// unknown count is none). True while the screen is the world.
inline bool uiLayerWorldScreenStep(UiWorldScreenGate& g, bool known, uint32_t draws) {
    g.draws = known ? draws : 0;
    if (!g.busy) {
        if (g.draws > kUiWorldEnterDraws) {
            if (++g.run >= kUiWorldEnterFrames) {
                g.busy = true;
                g.run = 0;
            }
        } else {
            g.run = 0;
        }
    } else if (g.draws < kUiWorldLeaveDraws) {
        if (++g.run >= kUiWorldLeaveFrames) {
            g.busy = false;
            g.run = 0;
        }
    } else {
        g.run = 0;
    }
    return g.busy;
}

// Status.json's GuiFocus, named for the gate's lines (Frontier's journal
// manual); nullptr past the known values.
inline const char* uiGuiFocusName(uint32_t focus) {
    static const char* const kNames[] = {"no focus",     "right panel",   "left panel",
                                         "comms panel",  "role panel",    "station services",
                                         "galaxy map",   "system map",    "orrery",
                                         "FSS",          "SAA",           "codex"};
    return focus < sizeof(kNames) / sizeof(kNames[0]) ? kNames[focus] : nullptr;
}

// Why a UI draw was left in the game's frame (or kRedirect). The order is
// the order of the tests in uiLayerDecide, so the reason reported is the
// first that failed.
enum class UiLayerDecision : uint8_t {
    kRedirect = 0,
    kNotUi,          // no family
    kVerdict,        // another fix swallows or re-issues the draw
    kWorldScreen,    // the 2D screen while it shows the world: on foot (Odyssey
                     // renders the world once, flat, into the screen's target)
                     // or a 3D map; taking it would hand the temporal pass a
                     // black eye (the on-foot gate and the screen's own
                     // identity, above)
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
    kMrt,            // more than one render target bound, or PS UAVs
    kDepthStencilTest,  // tests depth or stencil against a depth target the
                        // layer cannot reproduce at its size (the format, an
                        // array, MSAA, a read-only view, another size)
    kSubstitutedWrite,  // writes depth or stencil, but through a substitution
                        // (the loader panel, the curved screen) whose own
                        // geometry the write-back cannot re-issue
    kBlendRefused,   // a blend with no premultiplied or multiplicative form
    kLayerFailed,    // the layer (or its depth target) could not be created
    kCount
};

inline const char* uiLayerDecisionName(UiLayerDecision d) {
    switch (d) {
        case UiLayerDecision::kRedirect: return "redirected into the layer";
        case UiLayerDecision::kNotUi: return "not UI";
        case UiLayerDecision::kVerdict: return "another fix swallows or re-issues it";
        case UiLayerDecision::kWorldScreen:
            return "the screen shows the world (on foot, or a 3D map); the temporal pass keeps it";
        case UiLayerDecision::kNotEyeTarget: return "not drawn into an eye target";
        case UiLayerDecision::kHdrTarget:
            return "drawn into the HDR target before the tonemap (left in the picture)";
        case UiLayerDecision::kVrs: return "variable-rate shading bound for the eye";
        case UiLayerDecision::kNoEye: return "eye unknown";
        case UiLayerDecision::kTargetSize:
            return "its target is not the size of the eye the game submits";
        case UiLayerDecision::kLate: return "arrived after its eye's composite (gate G1)";
        case UiLayerDecision::kNotArmed: return "layer not armed";
        case UiLayerDecision::kMrt: return "more than one render target, or pixel-shader UAVs";
        case UiLayerDecision::kDepthStencilTest:
            return "tests depth or stencil against a target the layer cannot reproduce";
        case UiLayerDecision::kSubstitutedWrite:
            return "writes depth or stencil through a substituted geometry";
        case UiLayerDecision::kBlendRefused:
            return "blend with no premultiplied or multiplicative form";
        case UiLayerDecision::kLayerFailed: return "layer creation failed";
        default: return "?";
    }
}

// The facts the draw path gathers; the decision is a pure function of them.
struct UiLayerDrawFacts {
    UiLayerFamily family = UiLayerFamily::kNone;
    bool verdictForwards = true;  // the draw is forwarded as-is by its verdict
    bool worldScreen = false;     // the 2D screen shows the world: the journal says on
                                  // foot, or the screen's own depth is busy
    bool eyeTarget = false;       // an eye-sized 2D colour target
    bool ldrView = false;         // ... viewed as 8-bit UNORM (post-tonemap)
    bool vrs = false;             // variable-rate shading bound
    int eye = -1;                 // 0 left, 1 right, -1 unknown
    bool targetMatchesEye = true; // the target is the submitted region's size
    bool late = false;            // its eye's door already ran this frame
    bool armed = false;           // the door and the pass ran for it last frame
    bool mrt = false;            // a second render target, or PS UAVs, bound
    UiDsEffect ds;                // what it does with the bound depth target
    bool dsReproducible = true;   // ... and whether the layer can seed its own
    bool substituted = false;     // drawn by a substitution's own geometry
    UiBlendShape blend = UiBlendShape::kRefused;
    bool layerReady = true;       // the eye's layer exists at the wanted size
};

inline UiLayerDecision uiLayerDecide(const UiLayerDrawFacts& f) {
    if (f.family == UiLayerFamily::kNone) return UiLayerDecision::kNotUi;
    if (!f.verdictForwards) return UiLayerDecision::kVerdict;
    // Before every other test, so the reason is the same whatever else holds
    // (armed or not, late or not): the screen that shows the world stays.
    if (f.worldScreen && f.family == UiLayerFamily::kScreen) return UiLayerDecision::kWorldScreen;
    if (!f.eyeTarget) return UiLayerDecision::kNotEyeTarget;
    if (!f.ldrView) return UiLayerDecision::kHdrTarget;
    if (f.vrs) return UiLayerDecision::kVrs;
    if (f.eye < 0 || f.eye > 1) return UiLayerDecision::kNoEye;
    if (!f.targetMatchesEye) return UiLayerDecision::kTargetSize;
    if (f.late) return UiLayerDecision::kLate;
    if (!f.armed) return UiLayerDecision::kNotArmed;
    if (f.mrt) return UiLayerDecision::kMrt;
    if (f.ds.tests() && !f.dsReproducible) return UiLayerDecision::kDepthStencilTest;
    if (f.ds.writes() && f.substituted) return UiLayerDecision::kSubstitutedWrite;
    if (f.blend == UiBlendShape::kRefused) return UiLayerDecision::kBlendRefused;
    if (f.blend == UiBlendShape::kMultiply && f.substituted) return UiLayerDecision::kBlendRefused;
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

// ------------------------------------------------------- the route's price --
//
// The layer's whole route, not only its composite (the UI architecture
// review, "Findings and costs"): every GPU interval the layer ADDS to a
// frame is timed -- the layer's clear, the depth-stencil seed (a copy of the
// game's depth plus the Seeder's passes), the multiply's transmittance (its
// clear and the draw's second issue), a depth-writing draw's colourless
// re-issue, and the composite -- with GpuTimer (never a Flush or a wait),
// and summed per eye-frame, the unit a frame's budget is spent in: a stage
// can run more than once in an eye's frame (a write-back per depth-writing
// draw). The UI draws themselves, rasterised into the layer instead of the
// eye, are the game's own work and are not timed.
enum class UiRouteStage : uint8_t { kClear = 0, kSeed, kMultiply, kWriteBack, kComposite, kCount };

inline const char* uiRouteStageName(UiRouteStage s) {
    switch (s) {
        case UiRouteStage::kClear: return "clear";
        case UiRouteStage::kSeed: return "depth-stencil seed";
        case UiRouteStage::kMultiply: return "multiply";
        case UiRouteStage::kWriteBack: return "write-back";
        case UiRouteStage::kComposite: return "composite";
        default: return "?";
    }
}

// One eye's open sum. Samples arrive in submission order (the timers are
// polled oldest first, and the GPU finishes them in order), but the eyes
// interleave -- eye 0's composite runs after eye 1's UI -- so each eye (and
// each stage) keeps its own: a sample of a later frame closes the open sum.
// A sample that did not measure (a disjoint interval, an expiry) or one
// never taken (no free timer) spoils its eye-frame, which is dropped rather
// than reported short; one that arrives for a frame already closed is LATE
// (the caller counts it) and changes nothing.
struct UiRouteSum {
    uint64_t seq = 0;        // the eye-frame being summed
    uint64_t lostSeq = 0;    // the newest eye-frame a sample of which was never taken
    uint64_t closedSeq = 0;  // the newest eye-frame closed
    double ms = 0.0;
    bool open = false, bad = false;
};

// A sample of frame `seq` was never taken (no free timer).
inline void uiRouteLost(UiRouteSum& s, uint64_t seq) {
    if (s.open && s.seq == seq) {
        s.bad = true;
    } else if (seq > s.lostSeq) {
        s.lostSeq = seq;
    }
}

// A sample for a frame this sum has already closed, or passed.
inline bool uiRouteLate(const UiRouteSum& s, uint64_t seq) {
    return seq <= s.closedSeq || (s.open && seq < s.seq);
}

// A sample of frame `seq`: `valid` with `ms`, or one that did not measure.
// True when it closed an earlier eye-frame's good sum, into *closedMs.
inline bool uiRouteAdd(UiRouteSum& s, uint64_t seq, double ms, bool valid, double* closedMs) {
    if (uiRouteLate(s, seq)) return false;
    bool closed = false;
    if (s.open && seq != s.seq) {
        s.closedSeq = s.seq;
        if (!s.bad && closedMs) {
            *closedMs = s.ms;
            closed = true;
        }
        s.open = false;
    }
    if (!s.open) {
        s.open = true;
        s.seq = seq;
        s.ms = 0.0;
        s.bad = seq == s.lostSeq;
    }
    if (valid && ms >= 0.0) {
        s.ms += ms;
    } else {
        s.bad = true;
    }
    return closed;
}

// Closes the open sum when no sample of its frame can still arrive (every
// timer of that frame or earlier is read). True when that closed a good sum.
inline bool uiRouteClose(UiRouteSum& s, uint64_t oldestPendingSeq, double* closedMs) {
    if (!s.open || s.seq >= oldestPendingSeq) return false;
    s.open = false;
    s.closedSeq = s.seq;
    if (s.bad || !closedMs) return false;
    *closedMs = s.ms;
    return true;
}

// v[0..n) sorted in place, one percentile read off by linear interpolation
// between the two bracketing order statistics (temporal_pass.cpp's
// windowPercentile, the definition numpy uses).
inline double uiLayerPercentile(float* v, uint32_t n, double frac) {
    if (!v || !n) return 0.0;
    std::sort(v, v + n);
    const double pos = frac * static_cast<double>(n - 1);
    uint32_t lo = static_cast<uint32_t>(pos);
    if (lo > n - 1) lo = n - 1;
    const uint32_t hi = lo + 1 < n ? lo + 1 : lo;
    const double t = pos - static_cast<double>(lo);
    return static_cast<double>(v[lo]) * (1.0 - t) + static_cast<double>(v[hi]) * t;
}

}  // namespace edvr
