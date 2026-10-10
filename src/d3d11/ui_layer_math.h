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
#include <cstdio>
#include <cstring>

#include "holo_families.h"  // the crisp take's eight hologram VS hashes: kHoloGeneric's match list
#include "supercruise_lines.h"  // the orbit lines' and the supercruise bars' shader pairs
#include "holo_material.h"  // the cockpit holo panels' second vertex shader (Disable GUI effects)

namespace edvr {

// ---------------------------------------------------------------- the key --

// "off" | "100" | "125" -> 0 (off) | 1.0 | 1.25: the target HMD Quality, as
// a percentage of HMD Quality 1.0 in the file (a plain number, like every
// numeric key) and "100%" / "125%" in the F8 menu. Exact text, the way the
// other choice keys read: "100.0" is not "100". The first
// spellings -- "1.0", "1" and "1.25" -- are read for one release as 100 and
// 125, with *alias set to the new spelling for the log's one-time note.
// Anything else is off, and *recognized says so for the log. One reader for
// both halves (uiLayerConfigure hands the target to ui_panel_scale).
inline float uiQualityParse(const char* text, bool* recognized, const char** alias = nullptr) {
    if (recognized) *recognized = true;
    if (alias) *alias = nullptr;
    if (!text) {
        if (recognized) *recognized = false;
        return 0.0f;
    }
    if (std::strcmp(text, "off") == 0) return 0.0f;
    if (std::strcmp(text, "100") == 0) return 1.0f;
    if (std::strcmp(text, "125") == 0) return 1.25f;
    if (std::strcmp(text, "1.0") == 0 || std::strcmp(text, "1") == 0) {
        if (alias) *alias = "100";
        return 1.0f;
    }
    if (std::strcmp(text, "1.25") == 0) {
        if (alias) *alias = "125";
        return 1.25f;
    }
    if (recognized) *recognized = false;
    return 0.0f;
}

// The target as the menu and the log say it: "100%", "125%", else "off".
inline const char* uiQualityLabel(float target) {
    if (target == 1.0f) return "100%";
    if (target == 1.25f) return "125%";
    return "off";
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
// ADDITIVE (ONE, ONE) and scaled additive (SRC_ALPHA, ONE) convert exactly
// and cover NOTHING: transmittance is untouched (T' = T), the layer's colour
// gain is exactly the light the game's blend would have added. That is the
// hologram families' shape -- they are additive glows drawn with depth off
// (holo_families.h, ui_depth.cpp's generic hologram coverage) -- so their
// crisp take adds no coverage of its own; what differs from stock is WHERE
// the light lands (the layer, tonemapped once more), never how much.
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

// A stencil-only game write cannot change an unmodified depth-only seed.
// Keep the legacy whole-seed refresh for every other writer. In particular,
// any private depth-writing interaction in this frame retains it across all
// eyes/layers: its fine-grid depth or another cache's raw source may differ
// from a fresh resampling of the game buffer. The UI-quality rig proves this
// guard with actual differing depth/colour, not just predicate arithmetic.
inline bool uiLayerSeedWriterInvalidates(const UiDsEffect& writer, uint8_t seededMask,
                                        bool seededDepth, bool privateDepthWriteThisFrame) {
    return writer.writes() && !(writer.stencilWrite && !writer.depthWrite &&
        seededDepth && !seededMask && !privateDepthWriteThisFrame);
}

struct UiLayerPrivateDepthGuard {
    uint64_t sequence = 0;
    void note(uint64_t current, bool rawDepthWritePotential) {
        if (rawDepthWritePotential && current > sequence) sequence = current;
    }
    void noteReplay(uint64_t captured, uint64_t lastRedirect, bool rawDepthWritePotential) {
        // A late replay protects the current caches without advancing any
        // rendering cache's freshness sequence.
        note(captured > lastRedirect ? captured : lastRedirect, rawDepthWritePotential);
    }
    // An earlier/out-of-order cache sequence is conservative too; only a
    // monotonically newer frame can resume preservation.
    bool active(uint64_t current) const { return !current || current <= sequence; }
    void reset() { sequence = 0; }
};

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
    kHolo,       // cockpit holo panels (vs 81216C77F90DEDD6; with Elite's Disable
                 // GUI effects on, vs 1989E6D3B405FDE0: uiVsIsHoloPanel)
    kFlightHud,  // flight HUD (vs B7790CBFC6554097)
    kSprite,     // target-time sprite (vs E508648660A352B2)
    kHoloGeneric,  // the crisp take's eight (holo_families.h kHoloFamiliesTake:
                   // the radar's icon core, its two stalks, and the five
                   // contact markers -- the canopy is not one, and neither
                   // are the three the phase-3 review refused: the target
                   // sphere, the corona family, the world-marker reticle),
                   // Phase 3 of the crisp-HUD design. ONE family for eight
                   // hashes: the 30 s table prints one row, and the per-draw
                   // first-seen lines name the VS hash (ui_layer.cpp's
                   // noteFamily keys on (family, decision, vs, ps)).
    kOrbitLines,       // the supercruise orbit lines and ring lines (vs C7FA0C0F5DD49180 with
                       // its pixel shader 6EEF165A350DA30F: supercruise_lines.h): SCENE geometry,
                       // not interface, taken into the HDR layer so they are drawn at the layer's
                       // density instead of the render's and composited after the upscale. Its
                       // OWN family, not kHoloGeneric: that list also feeds ui_depth's hologram
                       // pass, which must not see a line.
    kSupercruiseBars,  // the supercruise bars (vs A47A3315FFF5E2E4 with ps 869FFF43E875906E): a
                       // line list, taken into the HDR layer through a private geometry shader
                       // that gives each 1 px segment a smooth strip (supercruise_bars.h)
    kSpaceDust,        // the supercruise space dust (vs 9BFC7FD232328391 with ps DBF1725726018F52):
                       // 300 additive ribbon quads, taken into the HDR layer as they are -- the
                       // vertex shader builds the ribbon's width in world units, so nothing about
                       // the draw changes but where it lands. Not a scene LINE (no width to give
                       // it): it is exempt from the density rule and is taken whatever the layer's
                       // size (uiLayerFamilyIsSceneLines does not name it)
    kAfterUi,    // not the interface at all: an owner draw that WRITES an eye
                 // target the UI was already taken from this frame
                 // (uiLayerNoteOther's 'W' case), taken into the same layer
                 // after the UI so it stays over it. Never reached through
                 // uiLayerFamilyOf/uiLayerFamilyFor -- uiLayerNoteOther
                 // assigns it, with the eye already known from the taken
                 // target, not derived by family.
    kMapCanvas,  // flat only (flat_ui_layer_math.h flatUiMapFamilyOf, while GuiFocus 6, 7 or 8 -- the Galaxy Map,
                 // the System Map, the Orrery -- is open): the map's GUI canvas composite (vs 12382D2EA45E9632 with
                 // ps 855C469156AB997F), drawn into the HDR scene and taken into the flat UI layer. No VR classifier
                 // names it: uiLayerFamilyFor's HDR branch never returns it.
    kMapSprite,  // flat only, the same gate: a map's icon sprite (vs C31238331D8AC3D4 with ps BD0FEB3276C8B2D7, or
                 // with ps 539A4858CE3A3477 on the System Map)
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
        case UiLayerFamily::kHoloGeneric: return "hologram";
        case UiLayerFamily::kOrbitLines: return "orbit lines";
        case UiLayerFamily::kSupercruiseBars: return "supercruise bars";
        case UiLayerFamily::kSpaceDust: return "space dust";
        case UiLayerFamily::kAfterUi: return "after the UI";
        case UiLayerFamily::kMapCanvas: return "map canvas";
        case UiLayerFamily::kMapSprite: return "map sprite";
        default: return "none";
    }
}

// The families the crisp take draws into the eye's HDR layer when they are drawn into the lit HDR scene target (the
// cockpit's holo panels, the flight HUD, the target sprite, the eight holograms, and the three supercruise draws: the
// orbit lines, the bars and the space dust). The ONE list: ui_layer.cpp's decision and the rigs' routing model both ask it.
// The two map families (kMapCanvas, kMapSprite) are in it too: the flat layer asks for them only while a map is open
// (flat_ui_layer_math.h), and no VR family rule names them, so the VR take never reaches them.
inline bool uiLayerFamilyTakesHdr(UiLayerFamily f) {
    return f == UiLayerFamily::kHolo || f == UiLayerFamily::kFlightHud || f == UiLayerFamily::kSprite ||
           f == UiLayerFamily::kHoloGeneric || f == UiLayerFamily::kOrbitLines ||
           f == UiLayerFamily::kSupercruiseBars || f == UiLayerFamily::kSpaceDust ||
           f == UiLayerFamily::kMapCanvas || f == UiLayerFamily::kMapSprite;
}

// The two families that are scene geometry rather than interface: the layer takes them for DENSITY alone (their lines
// are drawn at the layer's resolution instead of the render's), so a layer that is not wider than the render has
// nothing to give them and they stay in the scene (UiLayerDecision::kNoDensityGain).
inline bool uiLayerFamilyIsSceneLines(UiLayerFamily f) {
    return f == UiLayerFamily::kOrbitLines || f == UiLayerFamily::kSupercruiseBars;
}

// Whether the layer is wider than the render it replaces the lines of: the layer's size against the eye target's, both
// axes (a layer that is wider in one only is not a density gain the strip's width arithmetic is made for).
inline bool uiLayerWiderThanRender(uint32_t layerW, uint32_t layerH, uint32_t renderW, uint32_t renderH) {
    return renderW != 0 && renderH != 0 && layerW > renderW && layerH > renderH;
}

// After the UI: whether a later owner draw that WRITES an eye target the UI
// was already taken from this frame (uiLayerNoteOther's 'W' case) should be
// ATTEMPTED as a take into the same layer, after the UI. The read case (its
// 'R' case) never reaches this at all -- a draw that only reads the target
// is left exactly as before, uncounted here. kAttempt still goes through
// uiLayerDecide as UiLayerFamily::kAfterUi, which may itself refuse (family
// census: kMrt, kBlendRefused, kDepthStencilTest, kSubstitutedWrite,
// kVerdict, a begin-time blend or seed failure...) -- counted separately
// from this gate.
enum class UiAfterWriteDecision : uint8_t {
    kAttempt = 0,  // not an eye-sized input: try the take
    kPostPass,     // samples an eye-sized texture at a bound PS SRV slot (the
                   // frame or a copy of it; an overlay's own art is smaller):
                   // left, so a post pass is never captured whole into the layer
};

inline UiAfterWriteDecision uiLayerAfterWriteDecide(bool eyeSizedInput) {
    return eyeSizedInput ? UiAfterWriteDecision::kPostPass : UiAfterWriteDecision::kAttempt;
}

// rc-since-rc2 review F4: the after-UI retry preserves the original
// decision's two exclusions before attempting a take. The original family
// path never takes an excluded shader (ui_depth's list, vscreen.cpp's
// uiLayerFamilyOf) and never takes the world-screen composite while the
// screen shows the world (uiLayerDecide's kWorldScreen); the retry's
// kAfterUi family alone saw neither.
inline bool uiLayerAfterWritePreserved(bool excluded, bool worldScreenHeld, bool panelSized) {
    if (excluded) return false;
    if (worldScreenHeld && panelSized) return false;
    return true;
}

// THE AFTER-UI IDENTITY (2026-09-30, docs/ui-layer-2026-09-23.md "2026-09-30:
// the station menu's frosted base under the HUD").
//
// The after-UI rule needs to know which game target the layer's UI came from,
// one resource identity per eye (Eye::target): a draw that WRITES it is taken
// after the UI, a small draw that only READS it is a post pass and stays in
// the frame. In a frame where the crisp-HUD tonemap re-issue opened the eye's
// layer, that identity is the tonemap's output, A. Elite runs a post pass
// between the tonemap and the interface (vs 20F383BBAC05C031, n=4: it reads A
// and writes B), and every interface draw lands in B. Keyed to A the rule
// never fired ("after the UI the game drew 0 times", eye check "could not be
// told", flights 055723 and 060935), so the frosted base drawn under the
// panels (vs C4B4B334B26E81A9) stayed in the game's frame, UNDER the layer
// that now held the HUD the base covers in stock.
//
// The identity follows the eye through that pass instead: a reader of the
// identity that draws into ANOTHER eye-sized 8-bit target carries the
// identity there. Exactly once per eye-frame, and only while the layer holds
// nothing but the re-issue's HUD (chainOpen: set by the re-issue, cleared by
// the follow and by the first taken interface draw) -- after the UI has
// started, a pass over the eye is a post pass of the UI and never moves the
// identity, as before. The reader itself stays in the game's frame.
//
// Three pure pieces, so the rig (tools/ui_quality_test, the recorded station
// tails) and uiLayerNoteOther call the same code. The order they are called
// in is uiLayerNoteOther's; the rig scans it for exactly that order.

// The write case: which eye's identity a draw's own render target is (-1:
// neither). eyeTarget is uiLayerTargetKind() != 0; a null identity (nothing
// taken from that eye this frame) matches nothing.
inline int uiLayerAfterWriteEye(bool eyeTarget, const void* target, const void* taken0,
                                const void* taken1) {
    if (!eyeTarget || !target) return -1;
    if (target == taken0) return 0;
    if (target == taken1) return 1;
    return -1;
}

// The read case: which eye's identity a resource a small draw samples at PS
// SRV 0 or 1 is (-1: neither).
inline int uiLayerAfterReadEye(const void* sampled, const void* taken0, const void* taken1) {
    if (!sampled) return -1;
    if (sampled == taken0) return 0;
    if (sampled == taken1) return 1;
    return -1;
}

// Whether the reader of an eye's identity carries it into its own target.
// identity is that eye's, otherIdentity the other eye's (null when nothing is
// taken from it); readerTargetIsEye8bit is uiLayerTargetKind() == 2 and
// readerTarget its resource. A reader without an eye-sized 8-bit target, a
// reader that draws into a taken target, and any reader once the chain is
// closed never carry it.
inline bool uiLayerFollowReader(bool chainOpen, const void* identity, const void* otherIdentity,
                                bool readerTargetIsEye8bit, const void* readerTarget) {
    if (!chainOpen || !identity || !readerTargetIsEye8bit || !readerTarget) return false;
    return readerTarget != identity && readerTarget != otherIdentity;
}

// THE FAMILY RULE, pure: vscreen.cpp's uiLayerFamilyOf gathers these facts
// for an owner draw into an eye target, in this order and only as far as the
// rule reads them, and this decides. The shader hashes are the ones the
// census and every flight since have named.
constexpr uint64_t kUiVsPanel = 0xA888D51024D9798Eull;   // menu / modal panel composite
constexpr uint64_t kUiVsLoader = 0x4EF6DDB075A927FAull;  // loading screen composite
constexpr uint64_t kUiVsHolo = 0x81216C77F90DEDD6ull;
// The same cockpit panels with Elite's "Disable GUI effects" on: another vertex shader (and pixel
// shader, holo_material.h) with the stock one's draw state and a smaller output signature. Without
// this the HDR take never named them -- it keys the cockpit families by vertex shader alone -- and
// they stayed in the scene, upscaled with it (docs/ui-layer-2026-09-23.md, "2026-10-01: Disable GUI
// effects", user 5 on rc.5: soft panels, ghosting text; the pair in 3 of 3 field logs with the
// setting on and 0 of 11 with it off).
constexpr uint64_t kUiVsHoloGuiFxOff = kHoloGuiFxOffVs;
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
// 2026-09-27: the tinted and cheap variants join (ui_depth.cpp:105-115
// documents all three: nine disassembly lines differ, none in the
// sampling; the IN-FLIGHT menu -- the escape menu, station services -- and
// the holo effect over the panels draw through them). The first crisp-HUD
// flight showed the cockpit's menu composites were never recognized with
// the two original PSes alone (30k+ draws, "no learned surface, pixel
// shader not known"), so in-flight menus never took the layer at all.
constexpr uint64_t kUiPanelPs[] = {0x9107E72CB016CC02ull, 0x219323C8C025AD94ull,
                                   0x015EF9349EC097E8ull, 0xF2F872B191F656D5ull};
constexpr uint64_t kUiLoaderPs[] = {0x85565E9261812E2Full, 0x8ADB2A81A45E8A4Bull};

inline bool uiKnownPs(const uint64_t* list, size_t n, uint64_t ps) {
    for (size_t i = 0; i < n; ++i)
        if (list[i] == ps) return true;
    return false;
}

// The cockpit holo panels' vertex shaders: the stock one's, and the one the game draws them with
// while Disable GUI effects is on. The ONE place that says so: both of uiLayerFamilyFor's
// branches ask it, so a panel the HDR take names is named on the post-tonemap target too.
constexpr bool uiVsIsHoloPanel(uint64_t vs) { return vs == kUiVsHolo || vs == kUiVsHoloGuiFxOff; }

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
        // The crisp take's eight hologram families join the three named
        // families (Phase 3): one family, matched by VS hash against
        // holo_families.h's take-only list (kHoloFamiliesTake). The canopy
        // is deliberately NOT on that list -- it sits in front of the whole
        // sky, and covering it would smear the stars behind it, the depth
        // pass's own reasoning -- and neither are the two the phase-3
        // review refused. Two exact target-sphere pixel shaders are now
        // considered separately: their original screen-depth address must be
        // repaired and all dependencies prepared before redirecting. Unknown
        // sphere variants and the shared world/cockpit corona remain stock.
        // World-marker brackets remain in the original scene. The cockpit holo
        // panels are two vertex shaders (uiVsIsHoloPanel): the stock one and the
        // one the game switches in with Disable GUI effects on. An unnamed pair
        // gets no decision and no refusal line (2026-10-01: soft, ghosting panels
        // for a user with the setting on), which is why ui_scene_composites.h
        // counts the composites left in the scene. The three supercruise draws (the
        // orbit lines, the bars and the space dust) are named by their shader PAIR,
        // here on the HDR target alone: the vertex shaders of the bars and of the dust
        // are also seen with another pixel shader (the flat recipes), and a draw of
        // theirs into the post-tonemap target is nothing this names.
        out = uiVsIsHoloPanel(f.vs)    ? UiLayerFamily::kHolo
              : f.vs == kUiVsFlightHud ? UiLayerFamily::kFlightHud
              : f.vs == kUiVsSprite    ? UiLayerFamily::kSprite
              : (f.vs == kUiVsOrbitLines && f.ps == kUiPsOrbitLines) ? UiLayerFamily::kOrbitLines
              : (f.vs == kUiVsSupercruiseBars && f.ps == kUiPsSupercruiseBars) ? UiLayerFamily::kSupercruiseBars
              : (f.vs == kUiVsSpaceDust && f.ps == kUiPsSpaceDust) ? UiLayerFamily::kSpaceDust
              : (uiHoloGenericHash(f.vs) ||
                 (f.vs == kHoloTargetSphere &&
                  (f.ps == 0xEA02FAC2BD6C643Cull || f.ps == 0xE95634B0F61D218Full)))
                  ? UiLayerFamily::kHoloGeneric : UiLayerFamily::kNone;
        if (out != UiLayerFamily::kNone) w = UiFamilyWhy::kDirect;
    } else if (f.excluded) {
        w = UiFamilyWhy::kExcluded;
    } else if (f.panelSized) {
        w = UiFamilyWhy::kScreen;
        out = UiLayerFamily::kScreen;
    } else if (f.learnedSurface) {
        w = UiFamilyWhy::kLearnedSurface;
        out = f.vs == kUiVsPanel       ? UiLayerFamily::kPanel
              : f.vs == kUiVsLoader    ? UiLayerFamily::kLoader
              : uiVsIsHoloPanel(f.vs)  ? UiLayerFamily::kHolo
              : f.vs == kUiVsSprite    ? UiLayerFamily::kSprite
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
// bit 0: journalOnFootKnown && journalOnFoot, the on-foot frame pacing's
// source). It LAGS: the game rewrites the file about
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
    kNoEye,          // the eye could not be told
    kTargetSize,     // the target is not the size of the region the game
                     // submits for that eye (the map is target-to-layer)
    kLate,           // its eye's composite already ran this frame (gate G1)
    kToneLate,       // crisp: its eye's tonemap re-issue already ran this frame --
                     // content taken now can never publish (the layer clears next
                     // frame); the draw stays in the game's frame, as stock
    kNotArmed,       // no door frame for this eye last frame (first frames,
                     // key just on, pass not running)
    kNoDensityGain,  // an orbit-line or supercruise-bar draw (scene geometry the layer takes
                     // for density alone) while the layer is not wider than the render: the
                     // layer has nothing to give it and it stays in the scene
    kMrt,            // more than one render target bound, or PS UAVs
    kDepthStencilTest,  // tests depth or stencil against a depth target the
                        // layer cannot reproduce at its size (the format, an
                        // array, MSAA, a read-only view, another size)
    kSubstitutedWrite,  // writes depth or stencil, but through a substitution
                        // (the loader panel, the curved screen) whose own
                        // geometry the write-back cannot re-issue
    kBlendRefused,   // a blend with no premultiplied or multiplicative form
    kFamilyNotReady, // the family's own private pass is not available (the supercruise bars'
                     // geometry shader, its constants or its rasterizer state could not be
                     // made, or the game has a geometry shader of its own bound): the draw
                     // stays in the scene, and the module's line says which
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
        case UiLayerDecision::kNoEye: return "eye unknown";
        case UiLayerDecision::kTargetSize:
            return "its target is not the size of the eye the game submits";
        case UiLayerDecision::kLate: return "arrived after its eye's composite (gate G1)";
        case UiLayerDecision::kToneLate:
            return "arrived after its eye's tonemap re-issue (cannot publish this frame)";
        case UiLayerDecision::kNotArmed: return "layer not armed";
        case UiLayerDecision::kNoDensityGain:
            return "the layer is not wider than the render (nothing to gain; left in the scene)";
        case UiLayerDecision::kMrt: return "more than one render target, or pixel-shader UAVs";
        case UiLayerDecision::kDepthStencilTest:
            return "tests depth or stencil against a target the layer cannot reproduce";
        case UiLayerDecision::kSubstitutedWrite:
            return "writes depth or stencil through a substituted geometry";
        case UiLayerDecision::kBlendRefused:
            return "blend with no premultiplied or multiplicative form";
        case UiLayerDecision::kFamilyNotReady:
            return "the family's own private pass is not available (left in the scene)";
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
    // The VR world route owns the world for this frame (vr_world_route.h: it resolved the world
    // once and the eye shift is off), so the screen composite is the layer's to RE-ISSUE instead
    // of the temporal pass's to treat: the world-screen refusal does not apply, and every later
    // test applies to the screen draw as to any opaque, no-depth eye draw. Always false on a
    // frame the route does not own (design doc section 82; ui_layer.cpp sets it
    // for the 2D screen family alone, from vrWorldRouteLayerMayTake()).
    bool worldRoute = false;
    bool eyeTarget = false;       // an eye-sized 2D colour target
    bool ldrView = false;         // ... viewed as 8-bit UNORM (post-tonemap)
    int eye = -1;                 // 0 left, 1 right, -1 unknown
    bool targetMatchesEye = true; // the target is the submitted region's size
    bool late = false;            // its eye's door already ran this frame
    bool lateTone = false;        // crisp: its eye's tonemap re-issue already ran this frame
                                  // (content taken now can never publish -- the game's own
                                  // tonemap ordering varies frame to frame, and the layer's
                                  // clear next frame discards it: the missing ship/target
                                  // mesh holograms, review crisp-hud-phase3-2026-09-28)
    bool armed = false;           // the door and the pass ran for it last frame
    bool mrt = false;            // a second render target, or PS UAVs, bound
    UiDsEffect ds;                // what it does with the bound depth target
    bool dsReproducible = true;   // ... and whether the layer can seed its own
    bool substituted = false;     // drawn by a substitution's own geometry
    UiBlendShape blend = UiBlendShape::kRefused;
    bool layerReady = true;       // the eye's layer exists at the wanted size
    // The orbit lines and the supercruise bars (uiLayerFamilyIsSceneLines) are scene geometry the layer takes for DENSITY: a
    // layer that is not wider than the render (uiLayerWiderThanRender) has nothing to give them. True for every other
    // family, and by default, so a family that is not a scene line is decided exactly as it always was.
    bool layerWider = true;
    // The family's own private pass exists (the supercruise bars' geometry shader and its state: supercruise_bars.h). True by
    // default and for every family but that one.
    bool familyReady = true;
    // The HDR HUD take is armed (with fix.ui_quality) and this is one of the
    // cockpit HUD families (the holo panels, the flight HUD, the target
    // sprite, and the crisp take's eight hologram families as kHoloGeneric --
    // and the three supercruise draws, the orbit lines, the bars and the space
    // dust: uiLayerFamilyTakesHdr says which)
    // drawn into the
    // lit HDR (pre-tonemap) eye target: the draw goes to the HDR layer, and
    // the tonemap re-issue brings it back over the finished eye. Every other
    // test (eye known, armed, not late, no MRT/UAV, the seeded depth-stencil,
    // the blend) applies exactly as for the LDR take.
    bool crispHdr = false;
};

inline UiLayerDecision uiLayerDecide(const UiLayerDrawFacts& f) {
    if (f.family == UiLayerFamily::kNone) return UiLayerDecision::kNotUi;
    if (!f.verdictForwards) return UiLayerDecision::kVerdict;
    // Before every other test, so the reason is the same whatever else holds
    // (armed or not, late or not): the screen that shows the world stays --
    // unless the VR world route owns the frame, which re-issues it into the
    // layer (and then every test below applies to it as to any eye draw).
    if (f.worldScreen && f.family == UiLayerFamily::kScreen && !f.worldRoute) return UiLayerDecision::kWorldScreen;
    if (!f.eyeTarget) return UiLayerDecision::kNotEyeTarget;
    // The lit HDR target, before exposure and the tonemap: refused as stock,
    // unless the HDR HUD take owns this family (the cockpit HUD families: the
    // holo panels, the flight HUD, the target sprite, the holograms).
    if (!f.ldrView && !f.crispHdr) return UiLayerDecision::kHdrTarget;
    if (f.eye < 0 || f.eye > 1) return UiLayerDecision::kNoEye;
    if (!f.targetMatchesEye) return UiLayerDecision::kTargetSize;
    if (f.late) return UiLayerDecision::kLate;
    if (f.crispHdr && f.lateTone) return UiLayerDecision::kToneLate;
    if (!f.armed) return UiLayerDecision::kNotArmed;
    // After the arming (the layer's size is known from the door) and before any state is read: a scene line the layer cannot
    // draw any denser than the scene does is not taken.
    if (!f.layerWider && uiLayerFamilyIsSceneLines(f.family)) return UiLayerDecision::kNoDensityGain;
    if (f.mrt) return UiLayerDecision::kMrt;
    if (f.ds.tests() && !f.dsReproducible) return UiLayerDecision::kDepthStencilTest;
    if (f.ds.writes() && f.substituted) return UiLayerDecision::kSubstitutedWrite;
    if (f.blend == UiBlendShape::kRefused) return UiLayerDecision::kBlendRefused;
    if (f.blend == UiBlendShape::kMultiply && f.substituted) return UiLayerDecision::kBlendRefused;
    // The HDR half has no transmittance route for a multiply: the HDR take
    // skips ensureMult (the transmittance target is the LDR layer's), and
    // the coverage pass transfers only scalar HDR alpha, so a multiply
    // redirected into the HDR layer would lose its destination modulation
    // (review R5). Refuse BEFORE the redirect, kHoloGeneric included. The
    // three named families measured premultiplied-over; the holograms are
    // documented additive glows (depth off, mirrored blends -- ui_depth.cpp's
    // generic hologram coverage). Their states are NOT flight-measured: an
    // unconvertible blend or depth-stencil state is what the refusal net is
    // for, and it names the state that refused.
    if (f.crispHdr && f.blend == UiBlendShape::kMultiply) return UiLayerDecision::kBlendRefused;
    // Before the layer's own readiness: a draw the family's private pass cannot serve must not be the reason the HDR layer is
    // made (ui_layer.cpp asks the family first, then the layer).
    if (!f.familyReady && f.family == UiLayerFamily::kSupercruiseBars) return UiLayerDecision::kFamilyNotReady;
    if (!f.layerReady) return UiLayerDecision::kLayerFailed;
    return UiLayerDecision::kRedirect;
}

// ------------------------------------------------------ the VR world route --
//
// What the layer does for the VR on-foot world route (docs/design-flat-temporal-aa-2026-09-23.md, section 82;
// vr_world_route.h). On a frame the route owns, the game's 2D screen composite -- one sample of the screen
// texture into each eye image -- is NOT taken: it lands in its eye image exactly as it always did (the game's own
// post pass copies that image on, and a refused re-issue must leave the eye whole for the eye route to serve). The
// layer draws it a SECOND time, after the game's draw, into the eye's layer from the route's mipped copy of the
// resolved screen; the door then runs layer-only for that eye (native_temporal.cpp) and the layer's opaque screen
// over a black frame is the eye. All of it pure here, so the rig drives the code the DLL runs; with the key off
// nothing below is ever true (the route never owns a frame) and every function answers as it did before it existed.

// The route's mode for a draw: the 2D screen's composite, while the screen shows the world, on a frame the route
// owns. A screen that is not the world (a menu) is taken as it always was, whatever the route says.
inline bool uiLayerWorldRouteMode(UiLayerFamily family, bool worldScreen, bool worldRoute) {
    return family == UiLayerFamily::kScreen && worldScreen && worldRoute;
}

// Which tally a decided draw goes to. A draw in the route's mode that passed every test is not "redirected into
// the layer" (the layer took nothing from the game's frame): it is counted as a re-issue, by the caller, when the
// re-issue ran. Every other decision is counted as it always was -- kWorldScreen included, on the frames the route
// does not own -- and a route-mode draw the tests refused is counted as that refusal.
enum class UiWorldCount : uint8_t { kDecided, kReissue };
inline UiWorldCount uiLayerWorldCount(UiLayerDecision d, bool routeMode) {
    return routeMode && d == UiLayerDecision::kRedirect ? UiWorldCount::kReissue : UiWorldCount::kDecided;
}

// Why the route's re-issue did not happen, beyond the decision's own tests (those are UiLayerDecision values).
enum class UiWorldRefuse : uint8_t {
    kNone = 0,
    // (There is no "curved" reason: a curved screen is re-issued through the strip the game's draw is substituted with,
    // panel_curve.h panelCurveReissue, so the plan accepts it as it does a flat one.)
    kDepthState,     // the screen draw tests or writes depth or stencil (the re-issue binds no depth target)
    kNotOpaque,      // the screen draw blends: the layer-only eye is the layer over black, and only an opaque draw is
                     // the eye that way
    kNoSource,       // the draw's PS slot 0 is not a 2D texture
    kMipsNull,       // the mipped screen was not available (vr_world_mips.h says why, once)
    kNoSampler,      // the draw binds no sampler at PS slot 0
    kSamplerNull,    // the trilinear sampler like the game's could not be made
    kStateChanged,   // the draw's bindings were not the decided draw's at the moment of the re-issue
    kBeginRefused,   // the layer refused the issue (a blend that changed since the decision, or a fault)
    kFault,          // a fault while binding the re-issue
    kCount
};
inline const char* uiWorldRefuseName(UiWorldRefuse r) {
    switch (r) {
        case UiWorldRefuse::kDepthState: return "the screen draw tests or writes depth or stencil (the re-issue binds none)";
        case UiWorldRefuse::kNotOpaque:
            return "the screen draw blends (only an opaque draw is the whole eye over a black frame)";
        case UiWorldRefuse::kNoSource: return "the draw's texture at PS slot 0 is not a 2D texture";
        case UiWorldRefuse::kMipsNull: return "the mipped screen was not available";
        case UiWorldRefuse::kNoSampler: return "the draw binds no sampler at PS slot 0";
        case UiWorldRefuse::kSamplerNull: return "the trilinear sampler could not be made";
        case UiWorldRefuse::kStateChanged: return "the draw's bindings changed between its decision and the re-issue";
        case UiWorldRefuse::kBeginRefused: return "the layer refused the issue (a changed blend, or a fault)";
        case UiWorldRefuse::kFault: return "a fault while binding the re-issue";
        default: return "?";
    }
}

// The same reasons as short keys, for the 30 s line (the long texts above are the first-eight lines': Log's line
// holds 1200 characters, and ten long reasons beside fourteen decisions would not fit).
inline const char* uiWorldRefuseKey(UiWorldRefuse r) {
    switch (r) {
        case UiWorldRefuse::kDepthState: return "depth-state";
        case UiWorldRefuse::kNotOpaque: return "blending-draw";
        case UiWorldRefuse::kNoSource: return "no-source-texture";
        case UiWorldRefuse::kMipsNull: return "no-mipped-screen";
        case UiWorldRefuse::kNoSampler: return "no-sampler";
        case UiWorldRefuse::kSamplerNull: return "sampler-not-made";
        case UiWorldRefuse::kStateChanged: return "bindings-changed";
        case UiWorldRefuse::kBeginRefused: return "layer-refused-issue";
        case UiWorldRefuse::kFault: return "fault";
        default: return "?";
    }
}
inline const char* uiLayerDecisionKey(UiLayerDecision d) {
    switch (d) {
        case UiLayerDecision::kRedirect: return "redirect";
        case UiLayerDecision::kNotUi: return "not-ui";
        case UiLayerDecision::kVerdict: return "verdict";
        case UiLayerDecision::kWorldScreen: return "world-screen";
        case UiLayerDecision::kNotEyeTarget: return "not-eye-target";
        case UiLayerDecision::kHdrTarget: return "hdr-target";
        case UiLayerDecision::kNoEye: return "no-eye";
        case UiLayerDecision::kTargetSize: return "target-size";
        case UiLayerDecision::kLate: return "late";
        case UiLayerDecision::kToneLate: return "tone-late";
        case UiLayerDecision::kNotArmed: return "not-armed";
        case UiLayerDecision::kNoDensityGain: return "no-density-gain";
        case UiLayerDecision::kMrt: return "mrt";
        case UiLayerDecision::kDepthStencilTest: return "depth-stencil-test";
        case UiLayerDecision::kSubstitutedWrite: return "substituted-write";
        case UiLayerDecision::kBlendRefused: return "blend-refused";
        case UiLayerDecision::kFamilyNotReady: return "family-not-ready";
        case UiLayerDecision::kLayerFailed: return "layer-failed";
        default: return "?";
    }
}

// One id space for the log's dedupe: the route's own reasons are their enum value, a refusal by the decision's
// tests is kUiWorldDecisionBase + the decision.
constexpr uint16_t kUiWorldDecisionBase = 32;
inline uint16_t uiWorldReasonId(UiWorldRefuse r) { return static_cast<uint16_t>(r); }
inline uint16_t uiWorldReasonId(UiLayerDecision d) {
    return static_cast<uint16_t>(kUiWorldDecisionBase + static_cast<uint16_t>(d));
}
inline const char* uiWorldReasonName(uint16_t id) {
    if (id >= kUiWorldDecisionBase) return uiLayerDecisionName(static_cast<UiLayerDecision>(id - kUiWorldDecisionBase));
    return uiWorldRefuseName(static_cast<UiWorldRefuse>(id));
}

// "vr world route: layer did not take the screen draw for eye N: <reason>" -- the first kUiWorldReasonLines
// DISTINCT reasons of a session, once each (the counters say how often; a line per frame would flood the log).
constexpr uint32_t kUiWorldReasonLines = 8;
struct UiWorldReasonLog {
    uint16_t seen[kUiWorldReasonLines] = {};
    uint32_t n = 0;
    // True the first time `id` is offered while fewer than kUiWorldReasonLines distinct reasons have been logged.
    bool first(uint16_t id) {
        for (uint32_t i = 0; i < n; ++i) {
            if (seen[i] == id) return false;
        }
        if (n >= kUiWorldReasonLines) return false;
        seen[n++] = id;
        return true;
    }
};
inline int uiWorldFormatRefusal(char* out, size_t size, int eye, uint16_t id) {
    if (eye < 0 || eye > 1) {
        return std::snprintf(out, size, "vr world route: layer did not take the screen draw for an unknown eye: %s",
                             uiWorldReasonName(id));
    }
    return std::snprintf(out, size, "vr world route: layer did not take the screen draw for eye %d: %s", eye,
                         uiWorldReasonName(id));
}

// Is the layer live for the route? The exact condition under which the layer's own gate (the world-screen
// reading) runs today: fix.ui_quality on, a temporal mode on, the jitter switches as shipped, and the layer not
// stood down -- ui_layer.cpp's refreshLive() evaluates it through this, and the route must not run without it. The
// reason is the one line the route's log gives for a layer that is not; null when the layer is live.
inline bool uiLayerLiveFor(float target, bool temporal, bool jitterAsShipped, bool stoodDown) {
    return target > 0.0f && temporal && jitterAsShipped && !stoodDown;
}
inline const char* uiLayerNotLiveReasonFor(float target, bool temporal, bool jitterAsShipped, bool stoodDown) {
    if (!(target > 0.0f)) return "fix.ui_quality is off";
    if (!temporal) return "no temporal mode is on (fix.temporal_aa is off)";
    if (!jitterAsShipped) return "the eye jitter is not as shipped";
    if (stoodDown) return "the layer stood down for the session";
    return nullptr;
}

// Can the door go layer-only for this eye: is the composite that has to produce the eye certain to run? Decided in
// treat() BEFORE the door commits to a black frame, so a composite that could not run leaves the eye to the eye
// route (the game's own image through the pass, in the same call) instead of handing the headset a black eye.
enum class UiWorldDoorGap : uint8_t {
    kNone = 0,
    kNotLive,          // the layer is off or stood down
    kNoLayer,          // no layer was made for the eye
    kNoContent,        // the layer holds no draw of this frame (the re-issue did not land)
    kAlreadyDone,      // the composite already ran for this sequence
    kRuntimeDisabled,  // the graphics runtime is shutting down
    kCannotComposite,  // the frame's format, the GPU's typed stores or the shader refuse a composite
    kAspect,           // the frame's shape is not the layer's (the door's size is changing)
    kCount
};
struct UiWorldDoorFacts {
    bool live = false;
    bool layerMade = false;
    bool holdsContent = false;
    bool alreadyComposited = false;
    bool runtimeDisabled = false;
    bool canComposite = false;
    bool aspectMatches = false;
};
inline UiWorldDoorGap uiWorldDoorGap(const UiWorldDoorFacts& f) {
    if (!f.live) return UiWorldDoorGap::kNotLive;
    if (!f.layerMade) return UiWorldDoorGap::kNoLayer;
    if (!f.holdsContent) return UiWorldDoorGap::kNoContent;
    if (f.alreadyComposited) return UiWorldDoorGap::kAlreadyDone;
    if (f.runtimeDisabled) return UiWorldDoorGap::kRuntimeDisabled;
    if (!f.canComposite) return UiWorldDoorGap::kCannotComposite;
    if (!f.aspectMatches) return UiWorldDoorGap::kAspect;
    return UiWorldDoorGap::kNone;
}
inline const char* uiWorldDoorGapName(UiWorldDoorGap g) {
    switch (g) {
        case UiWorldDoorGap::kNotLive: return "the UI layer is not live";
        case UiWorldDoorGap::kNoLayer: return "the eye has no layer";
        case UiWorldDoorGap::kNoContent: return "the layer holds nothing of this frame (the screen re-issue did not land)";
        case UiWorldDoorGap::kAlreadyDone: return "the layer was already composited for this frame";
        case UiWorldDoorGap::kRuntimeDisabled: return "the graphics runtime is shutting down";
        case UiWorldDoorGap::kCannotComposite:
            return "the composite cannot run over the door's frame (its format, the GPU's typed stores or the shader)";
        case UiWorldDoorGap::kAspect: return "the door's frame is not the shape of the layer (its size is changing)";
        default: return "none";
    }
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
// not the raw bounds: submitted bounds can be an arbitrary fraction, and the
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
// eye, are the game's own work. The primary HDR HUD draw has a separate,
// diagnostic-only moved-rendering stage; it is excluded from machinery cost.
enum class UiRouteStage : uint8_t {
    kClear = 0,
    kSeed,
    kMultiply,
    kWriteBack,
    kComposite,
    // the crisp-HUD half's of fix.ui_quality HDR HUD layer: its per-frame clear, its depth-stencil
    // seed, the tonemap re-issue over the 8-bit layer, and the coverage pass
    // that writes the HDR layer's transmittance into the 8-bit layer's alpha.
    kHdrClear,
    kHdrSeed,
    kHdrTonemap,
    kHdrCoverage,
    kHdrMovedDraw,
    kCount
};

inline const char* uiRouteStageName(UiRouteStage s) {
    switch (s) {
        case UiRouteStage::kClear: return "clear";
        case UiRouteStage::kSeed: return "depth-stencil seed";
        case UiRouteStage::kMultiply: return "multiply";
        case UiRouteStage::kWriteBack: return "write-back";
        case UiRouteStage::kComposite: return "composite";
        case UiRouteStage::kHdrClear: return "HDR HUD clear";
        case UiRouteStage::kHdrSeed: return "HDR HUD depth-stencil seed";
        case UiRouteStage::kHdrTonemap: return "HUD tonemap re-issue";
        case UiRouteStage::kHdrCoverage: return "HUD coverage";
        case UiRouteStage::kHdrMovedDraw: return "HDR HUD moved draw";
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

// Reads an already-sorted v[0..n), one percentile by linear interpolation
// between the two bracketing order statistics (temporal_pass.cpp's
// windowPercentile, the definition numpy uses).
inline double uiLayerSortedPercentile(const float* v, uint32_t n, double frac) {
    if (!v || !n) return 0.0;
    const double pos = frac * static_cast<double>(n - 1);
    uint32_t lo = static_cast<uint32_t>(pos);
    if (lo > n - 1) lo = n - 1;
    const uint32_t hi = lo + 1 < n ? lo + 1 : lo;
    const double t = pos - static_cast<double>(lo);
    return static_cast<double>(v[lo]) * (1.0 - t) + static_cast<double>(v[hi]) * t;
}

inline double uiLayerPercentile(float* v, uint32_t n, double frac) {
    if (!v || !n) return 0.0;
    std::sort(v, v + n);
    return uiLayerSortedPercentile(v, n, frac);
}

}  // namespace edvr
