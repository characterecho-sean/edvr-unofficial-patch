// Build gate for fix.ui_quality (docs/ui-layer-2026-09-23.md), both halves
// of the one key, from the headers the DLL compiles:
//
//   * the surfaces (src/d3d11/ui_quality_math.h, absorbed from
//     fix.hud_quality's rig): the factor, its floor and cap; the rounding;
//     the internal render resolution from the runtime's recommendation and
//     HMD Quality (3070 x 0.65 = 1995 and the rest); the census's five
//     ratios against their own evidence; the candidate shape; the learned
//     ratios' file parser.
//   * the layer's arithmetic (src/d3d11/ui_layer_math.h): the key; the
//     layer's size and memory at 1.0 and 1.25; the viewport and scissor map;
//     the jitter cancel, both from pixels and from the tangent shift the
//     projection was given (temporal_math.h's own function); the tangent
//     form of the map against the region form; the blend conversion table,
//     the multiply included, and a CPU model proving layer-then-composite
//     equals the game's own blends in order, over a thousand random
//     sequences; the depth-stencil classification; the composite's
//     footprint weights; the door's arming and G1's "late"; the classifier
//     gate's every refusal, in order.
//   * the GPU half on WARP, with the production HLSL (ui_layer_shaders.h)
//     and the production seed (ui_deferred_depth.h): a jittered quad
//     rasterised through the redirected viewport lands on exactly the pixels
//     of the unjittered reference at 1.0 and 1.25, at all eight Halton
//     phases -- and does not without the cancel, so the check has teeth;
//     draws blended into the layer with the converted states and composited
//     equal the same draws blended straight into the frame, a multiply
//     among them; a stencil-tested draw against the layer's seeded copy of
//     the game's stencil lands where the same draw lands in the frame, at
//     1.0 under jitter and at 1.25; the write-back leaves the game's stencil
//     as the original draw did; a coloured quad composited at 1.25 equals
//     the box-filter model; the debug view; a cropped layer rectangle.
//
// Exit codes: 0 pass, 1 a check failed, 2 usage. --dry-run touches nothing.
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/common/temporal_math.h"
#include "../../src/d3d11/ui_deferred_depth.h"
#include "../../src/d3d11/ui_layer_math.h"
#include "../../src/d3d11/ui_layer_shaders.h"
#include "../../src/d3d11/ui_quality_math.h"

using Microsoft::WRL::ComPtr;
using namespace edvr;

namespace {

unsigned g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("FAIL: %s\n", what);
    }
}
bool near1(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

// ------------------------------------------------------------ arithmetic

void testKey() {
    bool ok = false;
    check(uiQualityParse("off", &ok) == 0.0f && ok, "off is off");
    check(uiQualityParse("1.0", &ok) == 1.0f && ok, "1.0 is 1.0");
    check(uiQualityParse("1.25", &ok) == 1.25f && ok, "1.25 is 1.25");
    check(uiQualityParse("1.00", &ok) == 0.0f && !ok, "1.00 is not 1.0 (exact text)");
    check(uiQualityParse("on", &ok) == 0.0f && !ok, "on is refused as off");
    check(uiQualityParse(nullptr, &ok) == 0.0f && !ok, "no text is off");
}

void testSize() {
    // Sean's rig 2026-09-23: the eye renders 1995x1970 at HMD Quality 0.65,
    // DLSS returns 3070x3032.
    UiLayerSize s = uiLayerSize(3070, 3032, 1.0f);
    check(s.w == 3070 && s.h == 3032, "1.0: the layer is the size the upscaler returns");
    s = uiLayerSize(3070, 3032, 1.25f);
    check(s.w == 3838 && s.h == 3790, "1.25: 3070x3032 x 1.25 = 3838x3790 (3837.5 rounds up)");
    check(uiLayerBytes(3070, 3032) == 37232960ull, "3070x3032 RGBA8 = 37,232,960 bytes");
    check(uiLayerBytes(3838, 3790) == 58184080ull, "3838x3790 RGBA8 = 58,184,080 bytes");
    check(uiLayerBytes(4340, 4284) == 74370240ull && near1(uiLayerMB(uiLayerBytes(4340, 4284)), 74.37, 0.01),
          "4340x4284: the handoff's 74 MB per eye");
    s = uiLayerSize(13108, 1000, 1.25f);
    check(s.w == 0 && s.h == 0, "a layer past D3D11's 16384 is refused, never clamped");
    s = uiLayerSize(0, 3032, 1.0f);
    check(s.w == 0 && s.h == 0, "no door size, no layer");
    s = uiLayerSize(3070, 3032, 0.0f);
    check(s.w == 0 && s.h == 0, "off, no layer");
}

void testMap() {
    // 1.0: the game's 1995x1970 target onto the 3070x3032 layer.
    UiLayerMap m = uiLayerMapFromRegion(0, 0, 1995, 1970, 3070, 3032);
    UiViewport full;
    full.w = 1995;
    full.h = 1970;
    UiViewport o = uiLayerMapViewport(m, full, 0, 0);
    check(near1(o.x, 0) && near1(o.y, 0) && near1(o.w, 3070, 1e-2) && near1(o.h, 3032, 1e-2),
          "1.0: the game's full viewport is the whole layer");
    UiViewport sub;
    sub.x = 100;
    sub.y = 200;
    sub.w = 400;
    sub.h = 300;
    sub.minZ = 0.25f;
    sub.maxZ = 0.5f;
    o = uiLayerMapViewport(m, sub, 0, 0);
    check(near1(o.x, 100.0 * 3070 / 1995, 1e-2) && near1(o.y, 200.0 * 3032 / 1970, 1e-2) &&
              near1(o.w, 400.0 * 3070 / 1995, 1e-2) && near1(o.h, 300.0 * 3032 / 1970, 1e-2) &&
              o.minZ == 0.25f && o.maxZ == 0.5f,
          "a sub-viewport scales about the target's origin; depth range unchanged");
    // 1.25.
    m = uiLayerMapFromRegion(0, 0, 1995, 1970, 3838, 3790);
    o = uiLayerMapViewport(m, full, 0, 0);
    check(near1(o.w, 3838, 1e-2) && near1(o.h, 3790, 1e-2), "1.25: the full viewport is the whole 1.25 layer");

    // The jitter cancel: a point the unjittered projection puts at game pixel
    // X0 is rendered at X0 + jx; through the mapped viewport plus the cancel
    // it lands at ax * X0 -- where the unjittered draw would have, at every
    // Halton phase the pass uses.
    for (uint32_t n = 0; n < kTemporalJitterCount; ++n) {
        float jx = 0, jy = 0;
        temporalJitter(n, &jx, &jy);
        float cx = 0, cy = 0;
        uiLayerJitterCancel(jx, jy, m, &cx, &cy);
        const UiViewport v = uiLayerMapViewport(m, full, cx, cy);
        const double X0 = 777.25, Y0 = 1234.5;
        // The viewport transform of the jittered NDC point.
        const double ndcX = (X0 + jx) / 1995.0 * 2.0 - 1.0, ndcY = 1.0 - (Y0 + jy) / 1970.0 * 2.0;
        const double xl = v.x + (ndcX + 1.0) * 0.5 * v.w, yl = v.y + (1.0 - ndcY) * 0.5 * v.h;
        check(near1(xl, m.ax * X0, 2e-3) && near1(yl, m.ay * Y0, 2e-3),
              "the cancel puts a jittered point where the unjittered projection would");
    }
    // From the tangent shift the projection was given (native_temporal's
    // inverse of temporalJitterToTangents).
    const float tan[4] = {-1.2f, 1.05f, -1.1f, 1.15f};
    for (uint32_t n = 0; n < kTemporalJitterCount; ++n) {
        float jx = 0, jy = 0, dx = 0, dy = 0, bx = 0, by = 0;
        temporalJitter(n, &jx, &jy);
        temporalJitterToTangents(jx, jy, tan, 1995, 1970, &dx, &dy);
        uiLayerJitterFromTangentShift(dx, dy, tan, 1995, 1970, &bx, &by);
        check(near1(bx, jx, 1e-4) && near1(by, jy, 1e-4), "the tangent shift gives back the pixel jitter");
    }
    // The tangent form equals the region form when told == truth.
    const float told[4] = {-1.0f, 1.0f, 1.2f, -0.9f};
    const UiLayerMap a = uiLayerMapFromTangents(told, told, 0, 0, 1995, 1970, 3070, 3032);
    const UiLayerMap b = uiLayerMapFromRegion(0, 0, 1995, 1970, 3070, 3032);
    check(near1(a.ax, b.ax) && near1(a.bx, b.bx, 1e-3) && near1(a.ay, b.ay) && near1(a.by, b.by, 1e-3),
          "the tangent map reduces to the region map without a guard lie");
    // A guard lie: the game told 10% wider on each side than the layer covers.
    const float truth[4] = {-1.0f, 1.0f, 1.2f, -0.9f};
    const float wide[4] = {-1.2f, 1.2f, 1.2f, -0.9f};
    const UiLayerMap g = uiLayerMapFromTangents(wide, truth, 0, 0, 2400, 1970, 2000, 3032);
    // Game column 200 looks along -1.2 + 200/2400 * 2.4 = -1.0 = truth l: layer x 0.
    check(near1(g.ax * 200 + g.bx, 0.0, 1e-2), "under a guard lie the true left edge lands on layer x 0");
    check(near1(g.ax * 1200 + g.bx, 1000.0, 1e-2), "and the centre on the layer's centre");
}

void testScissor() {
    UiLayerMap m = uiLayerMapFromRegion(0, 0, 1995, 1970, 3070, 3032);
    UiRect full;
    full.r = 1995;
    full.b = 1970;
    UiRect o = uiLayerMapScissor(m, full, 0, 0, 3070, 3032);
    check(o.l == 0 && o.t == 0 && o.r == 3070 && o.b == 3032, "1.0: the full scissor is the whole layer, exactly");
    m = uiLayerMapFromRegion(0, 0, 1995, 1970, 3838, 3790);
    o = uiLayerMapScissor(m, full, 0, 0, 3838, 3790);
    check(o.l == 0 && o.t == 0 && o.r == 3838 && o.b == 3790, "1.25: the full scissor is the whole layer, exactly");
    UiRect sub;
    sub.l = 10;
    sub.t = 20;
    sub.r = 30;
    sub.b = 41;
    o = uiLayerMapScissor(m, sub, 0, 0, 3838, 3790);
    // 10*1.9238 = 19.24 -> 19; 30*1.9238 = 57.71 -> 58; 20*1.9239 = 38.48 -> 38;
    // 41*1.9239 = 78.88 -> 79: outward.
    check(o.l == 19 && o.r == 58 && o.t == 38 && o.b == 79, "a clipping scissor rounds outward");
    o = uiLayerMapScissor(m, sub, -0.4f, 0.6f, 3838, 3790);
    check(o.l == 18 && o.r == 58 && o.t == 39 && o.b == 80, "the scissor moves with the jitter cancel");
    UiRect off;
    off.l = -50;
    off.t = -50;
    off.r = 5000;
    off.b = 5000;
    o = uiLayerMapScissor(m, off, 0, 0, 3838, 3790);
    check(o.l == 0 && o.t == 0 && o.r == 3838 && o.b == 3790, "a scissor past the target clamps to the layer");
}

// A colour factor's alpha twin: D3D11 refuses a *_COLOR factor in the alpha
// equation (CreateBlendState fails, and a null state draws unblended).
uint8_t alphaOf(uint8_t f) {
    using namespace uiblend;
    return f == kSrcColor ? kSrcAlpha : f == kInvSrcColor ? kInvSrcAlpha : f == kDestColor ? kDestAlpha
         : f == kInvDestColor ? kInvDestAlpha : f;
}

UiBlendRt blend(bool on, uint8_t s, uint8_t d, uint8_t mask = uiblend::kWriteAll, uint8_t op = uiblend::kOpAdd) {
    UiBlendRt b;
    b.enable = on;
    b.src = s;
    b.dst = d;
    b.op = op;
    b.srcA = s == uiblend::kSrcAlpha ? uiblend::kOne : alphaOf(s);  // the game's alpha half is irrelevant
    b.dstA = alphaOf(d);
    b.mask = mask;
    return b;
}

void testBlend() {
    using namespace uiblend;
    UiBlendRt c;
    check(uiLayerConvertBlend(blend(false, kOne, kZero), &c) && c.enable && c.src == kOne && c.dst == kZero &&
              c.srcA == kZero && c.dstA == kZero && c.mask == kWriteAll,
          "opaque: colour ONE/ZERO, transmittance to zero");
    check(uiLayerConvertBlend(blend(true, kSrcAlpha, kInvSrcAlpha), &c) && c.src == kSrcAlpha &&
              c.dst == kInvSrcAlpha && c.srcA == kZero && c.dstA == kInvSrcAlpha,
          "over: colour kept, transmittance times (1 - a)");
    check(uiLayerConvertBlend(blend(true, kOne, kInvSrcAlpha, kWriteRgb), &c) && c.src == kOne &&
              c.dst == kInvSrcAlpha && c.srcA == kZero && c.dstA == kInvSrcAlpha && c.mask == kWriteAll,
          "premultiplied over (the menu panel's, RGB-only mask): alpha written anyway");
    check(uiLayerConvertBlend(blend(true, kOne, kOne), &c) && c.srcA == kZero && c.dstA == kOne,
          "additive: transmittance untouched");
    check(uiLayerConvertBlend(blend(true, kSrcAlpha, kOne), &c) && c.srcA == kZero && c.dstA == kOne,
          "scaled additive: transmittance untouched");
    check(!uiLayerConvertBlend(blend(true, kOne, kOne, kWriteAll, 2), &c), "subtract is refused");
    // The loading screen's gamma pass (ps 8ADB2A81A45E8A4B): the frame
    // tinted by the source colour. Both factor orders are a multiply; the
    // layer keeps the equation and leaves the transmittance alone, and the
    // second draw scales the per-channel transmittance by the same colour.
    check(uiLayerBlendShape(blend(true, kZero, kSrcColor)) == UiBlendShape::kMultiply &&
              uiLayerBlendShape(blend(true, kDestColor, kZero)) == UiBlendShape::kMultiply,
          "ZERO,SRC_COLOR and DEST_COLOR,ZERO are a multiply");
    check(uiLayerConvertBlend(blend(true, kDestColor, kZero), &c) && c.src == kDestColor &&
              c.dst == kZero && c.mask == kWriteRgb,
          "a multiply into the layer: the game's colour factors, alpha untouched");
    check(uiLayerMultiplyBlend(blend(true, kZero, kSrcColor), &c) && c.src == kZero &&
              c.dst == kSrcColor && c.srcA == kZero && c.dstA == kOne && c.mask == kWriteRgb,
          "the multiply's second draw scales the transmittance, alpha untouched");
    check(!uiLayerMultiplyBlend(blend(true, kSrcAlpha, kInvSrcAlpha), &c),
          "only a multiply has a second draw");
    check(uiLayerConvertBlend(blend(true, kZero, kSrcColor, 0x3), &c) && c.mask == 0x3,
          "a multiply under a partial colour mask is carried, per channel");
    check(!uiLayerConvertBlend(blend(true, kBlendFactor, kInvBlendFactor), &c), "a blend factor is refused");
    check(!uiLayerConvertBlend(blend(true, kSrc1Color, kInvSrc1Color), &c), "dual source is refused");
    check(!uiLayerConvertBlend(blend(true, kOne, kInvSrcAlpha, kWriteAlpha), &c), "an alpha-only write is refused");
    // Transmittance is one number for three channels: a covering draw that
    // leaves a colour channel out cannot be carried, light that adds can.
    check(!uiLayerConvertBlend(blend(false, kOne, kZero, 0x3), &c), "an opaque draw without blue is refused");
    check(!uiLayerConvertBlend(blend(true, kSrcAlpha, kInvSrcAlpha, 0x5), &c), "an over without green is refused");
    check(uiLayerConvertBlend(blend(true, kOne, kOne, 0x1), &c) && c.mask == (0x1 | kWriteAlpha),
          "additive light into red alone is carried, its mask kept");
    check(!uiLayerConvertBlend(blend(true, kInvDestAlpha, kOne), &c), "a destination-alpha factor is refused");
    const D3D11_BLEND_DESC d = uiLayerBlendDesc(c = UiBlendRt{});
    check(!d.AlphaToCoverageEnable && !d.IndependentBlendEnable, "the layer's state: no alpha-to-coverage");

    // The CPU model: any sequence of accepted draws blended straight into a
    // frame equals the same draws blended into a cleared layer and
    // composited.
    uint32_t seed = 12345u;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f;
    };
    const UiBlendRt shapes[7] = {blend(false, kOne, kZero), blend(true, kSrcAlpha, kInvSrcAlpha),
                                 blend(true, kOne, kInvSrcAlpha), blend(true, kOne, kOne),
                                 blend(true, kSrcAlpha, kOne), blend(true, kZero, kSrcColor),
                                 blend(true, kDestColor, kZero)};
    // Colours at most 0.7 and light added at most 0.04 a draw, six draws at
    // most: no step ever saturates, so the two paths must agree exactly (a
    // saturated additive step is the one place 8-bit hardware and this
    // algebra part, in both the game's frame and the layer alike).
    double worst = 0.0;
    int multiplies = 0;
    for (int trial = 0; trial < 1000; ++trial) {
        UiPx frame{rnd() * 0.7f, rnd() * 0.7f, rnd() * 0.7f, rnd()};
        UiPx direct = frame;
        UiPx layer{0, 0, 0, 1};
        UiPx mult{1, 1, 1, 1};
        const int n = 1 + static_cast<int>(rnd() * 5.99f);
        for (int k = 0; k < n; ++k) {
            const UiBlendRt& g = shapes[static_cast<int>(rnd() * 6.99f)];
            UiPx s{rnd() * 0.7f, rnd() * 0.7f, rnd() * 0.7f, rnd()};
            if (g.src == kOne && g.dst == kInvSrcAlpha) {  // a premultiplied source
                s.r *= s.a;
                s.g *= s.a;
                s.b *= s.a;
            }
            if (g.dst == kOne) {  // light that covers nothing: small
                s.r *= 0.04f / 0.7f;
                s.g *= 0.04f / 0.7f;
                s.b *= 0.04f / 0.7f;
            }
            direct = uiBlendApply(g, s, direct);
            UiBlendRt conv;
            if (!uiLayerConvertBlend(g, &conv)) {
                check(false, "every accepted shape converts");
                continue;
            }
            layer = uiBlendApply(conv, s, layer);
            UiBlendRt second;
            if (uiLayerMultiplyBlend(g, &second)) {
                mult = uiBlendApply(second, s, mult);
                ++multiplies;
            }
        }
        const UiPx out = uiLayerCompositePx(layer, mult, frame);
        worst = (std::max)(worst, static_cast<double>(std::fabs(out.r - direct.r)));
        worst = (std::max)(worst, static_cast<double>(std::fabs(out.g - direct.g)));
        worst = (std::max)(worst, static_cast<double>(std::fabs(out.b - direct.b)));
    }
    check(multiplies > 100, "the sequences include multiplies");
    check(worst < 1e-5,
          "layer-then-composite equals the game's own blends in order, multiplies among them "
          "(1000 sequences)");
}

void testFootprint() {
    uint32_t first = 0;
    float w[kUiLayerMaxTaps] = {};
    int n = uiLayerFootprint(7.0, 8.0, 100, &first, w);
    check(n == 1 && first == 7 && near1(w[0], 1.0), "1:1 on the grid is a copy");
    n = uiLayerFootprint(0.0, 1.25, 100, &first, w);
    check(n == 2 && first == 0 && near1(w[0], 0.8) && near1(w[1], 0.2), "1.25: pixel 0 is 0.8 + 0.2");
    n = uiLayerFootprint(1.25, 2.5, 100, &first, w);
    check(n == 2 && first == 1 && near1(w[0], 0.6) && near1(w[1], 0.4), "1.25: pixel 1 is 0.6 + 0.4");
    n = uiLayerFootprint(3.75, 5.0, 100, &first, w);
    check(n == 2 && first == 3 && near1(w[0], 0.2) && near1(w[1], 0.8), "1.25: pixel 3 is 0.2 + 0.8");
    n = uiLayerFootprint(98.9, 100.4, 100, &first, w);
    check(n == 2 && first == 98 && near1(w[0] + w[1], 1.0), "the edge clamps and renormalises");
}

void testGate() {
    UiLayerDrawFacts f;
    f.family = UiLayerFamily::kScreen;
    f.verdictForwards = true;
    f.eyeTarget = true;
    f.ldrView = true;
    f.eye = 0;
    f.armed = true;
    f.blend = UiBlendShape::kPremulOver;
    check(uiLayerDecide(f) == UiLayerDecision::kRedirect, "a post-tonemap composite, armed, is redirected");
    auto with = [&](void (*edit)(UiLayerDrawFacts&)) {
        UiLayerDrawFacts g = f;
        edit(g);
        return uiLayerDecide(g);
    };
    check(with([](UiLayerDrawFacts& g) { g.family = UiLayerFamily::kNone; }) == UiLayerDecision::kNotUi, "no family");
    check(with([](UiLayerDrawFacts& g) { g.verdictForwards = false; }) == UiLayerDecision::kVerdict, "swallowed");
    check(with([](UiLayerDrawFacts& g) { g.eyeTarget = false; }) == UiLayerDecision::kNotEyeTarget, "not an eye");
    check(with([](UiLayerDrawFacts& g) { g.ldrView = false; }) == UiLayerDecision::kHdrTarget, "HDR target");
    check(with([](UiLayerDrawFacts& g) { g.vrs = true; }) == UiLayerDecision::kVrs, "variable-rate shading");
    check(with([](UiLayerDrawFacts& g) { g.eye = -1; }) == UiLayerDecision::kNoEye, "no eye");
    check(with([](UiLayerDrawFacts& g) { g.targetMatchesEye = false; }) == UiLayerDecision::kTargetSize,
          "a target that is not the submitted eye's size");
    check(with([](UiLayerDrawFacts& g) { g.late = true; }) == UiLayerDecision::kLate, "late (G1)");
    check(with([](UiLayerDrawFacts& g) { g.armed = false; }) == UiLayerDecision::kNotArmed, "not armed");
    check(with([](UiLayerDrawFacts& g) { g.replayOwns = true; }) == UiLayerDecision::kReplayOwns,
          "the deferred replay's own capture");
    check(with([](UiLayerDrawFacts& g) { g.mrt = true; }) == UiLayerDecision::kMrt, "two targets");
    check(with([](UiLayerDrawFacts& g) { g.ds.stencilTest = true; }) == UiLayerDecision::kRedirect,
          "a stencil test the layer can seed is taken");
    check(with([](UiLayerDrawFacts& g) {
              g.ds.depthTest = true;
              g.dsReproducible = false;
          }) == UiLayerDecision::kDepthStencilTest,
          "a depth test the layer cannot reproduce is left");
    check(with([](UiLayerDrawFacts& g) { g.ds.stencilWrite = true; }) == UiLayerDecision::kRedirect,
          "the menu panel's stencil write is taken (the write-back keeps it)");
    check(with([](UiLayerDrawFacts& g) { g.ds.stencilWrite = true; }) == UiLayerDecision::kRedirect &&
              with([](UiLayerDrawFacts& g) {
                  g.ds.stencilWrite = true;
                  g.dsReproducible = false;  // only a TEST needs the seed
              }) == UiLayerDecision::kRedirect,
          "a write alone needs no seed");
    check(with([](UiLayerDrawFacts& g) {
              g.ds.stencilWrite = true;
              g.substituted = true;
          }) == UiLayerDecision::kSubstitutedWrite,
          "a write through the curved screen's own geometry is left");
    check(with([](UiLayerDrawFacts& g) { g.substituted = true; }) == UiLayerDecision::kRedirect,
          "the curved screen's plain composite is taken");
    check(with([](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kMultiply; }) == UiLayerDecision::kRedirect,
          "the loading screen's multiply is taken");
    check(with([](UiLayerDrawFacts& g) {
              g.blend = UiBlendShape::kMultiply;
              g.substituted = true;
          }) == UiLayerDecision::kBlendRefused,
          "a multiply through a substitution is left (its second draw cannot be repeated)");
    check(with([](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kRefused; }) == UiLayerDecision::kBlendRefused, "blend");

    // The on-foot gate: on foot the 2D screen IS the world (flight 09:38:
    // the layer took it and the temporal pass got a black eye). The journal's
    // pair (known, on foot), read once a frame, drives the fact.
    {
        auto screenWith = [&](bool gateHolds, UiLayerFamily fam = UiLayerFamily::kScreen) {
            UiLayerDrawFacts g = f;
            g.family = fam;
            g.onFoot = gateHolds;
            return uiLayerDecide(g);
        };
        UiOnFootGate gate;
        check(gate.state == -1, "the gate starts unread");
        check(uiLayerOnFootStep(gate, true, true, 1000) &&
                  screenWith(gate.state == 1) == UiLayerDecision::kOnFootWorld,
              "on foot: the 2D screen is not redirected");
        check(std::strcmp(uiLayerDecisionName(UiLayerDecision::kOnFootWorld),
                          "on foot: the screen shows the world; the temporal pass keeps it") == 0,
              "...and the left-in-the-frame line gives the reason");
        check(screenWith(true, UiLayerFamily::kPanel) == UiLayerDecision::kRedirect &&
                  screenWith(true, UiLayerFamily::kLoader) == UiLayerDecision::kRedirect,
              "on foot, the menus and the loading screen are still taken");
        check(with([](UiLayerDrawFacts& g) {
                  g.onFoot = true;
                  g.armed = false;
                  g.late = true;
              }) == UiLayerDecision::kOnFootWorld,
              "on foot is the reason whatever else holds");
        check(uiLayerOnFootStep(gate, false, false, 1500) && uiLayerOnFootStep(gate, false, false, 3900),
              "a short unknown (a mid-write read of Status.json) holds the gate");
        check(!uiLayerOnFootStep(gate, false, false, 4001) && gate.state == 0 &&
                  screenWith(gate.state == 1) == UiLayerDecision::kRedirect,
              "unknown for 3 s after the last on-foot reading releases it");
        UiOnFootGate aboard;
        check(!uiLayerOnFootStep(aboard, true, false, 1000) &&
                  screenWith(aboard.state == 1) == UiLayerDecision::kRedirect,
              "aboard: the 2D screen is redirected");
        check(uiLayerOnFootStep(aboard, true, true, 2000) && !uiLayerOnFootStep(aboard, true, false, 2100),
              "embarking releases the gate at once, without the hold");
        UiOnFootGate unknown;
        check(!uiLayerOnFootStep(unknown, false, false, 1000) && !uiLayerOnFootStep(unknown, false, true, 2000) &&
                  unknown.state == 0 && screenWith(unknown.state == 1) == UiLayerDecision::kRedirect,
              "unknown journal (a menu, the watcher off): the 2D screen is redirected, as before the gate");
    }
    check(with([](UiLayerDrawFacts& g) { g.layerReady = false; }) == UiLayerDecision::kLayerFailed, "no layer");
    // The first failing test is the one reported: the cockpit's HDR draw
    // reads as HDR even while the layer is not armed yet.
    check(with([](UiLayerDrawFacts& g) {
              g.family = UiLayerFamily::kHolo;
              g.ldrView = false;
              g.armed = false;
          }) == UiLayerDecision::kHdrTarget,
          "the HDR families read as HDR, whatever else");

    UiLayerDoorState d;
    d.doorSeq = 5;
    d.treatedSeq = 5;
    d.fullW = 3070;
    d.fullH = 3032;
    check(uiLayerArmed(d, 6) && !uiLayerLateFor(d, 6), "armed behind last frame's door and pass");
    check(!uiLayerArmed(d, 7), "a frame without a door disarms");
    check(uiLayerLateFor(d, 5) && !uiLayerArmed(d, 5), "a draw after its eye's door this frame is late");
    d.treatedSeq = 4;
    check(!uiLayerArmed(d, 6), "a door fed by raw pixels (the pass declined) does not arm");
    d.treatedSeq = 5;
    d.fullW = 0;
    check(!uiLayerArmed(d, 6), "no size, not armed");

    float uv[4];
    const uint32_t whole3070[4] = {0, 0, 3070, 3032};
    uiLayerUvFromRegion(whole3070, 3070, 3032, uv);
    check(uv[0] == 0 && uv[1] == 0 && uv[2] == 1 && uv[3] == 1, "the whole eye is the whole layer");
    // A guard crop: the region the door rounded, not the raw fraction, so at
    // 1.0 every output pixel lands on exactly one layer texel.
    const uint32_t crop[4] = {307, 0, 2763, 3032};
    uiLayerUvFromRegion(crop, 3070, 3032, uv);
    check(near1(uv[0] * 3070.0, 307.0, 1e-3) && near1(uv[2] * 3070.0, 2763.0, 1e-3),
          "a cropped region names its whole-pixel rectangle of the layer");
    {
        // ...and through the composite's own arithmetic that is one texel a
        // pixel at 1.0: footprint of output pixel i = [307 + i, 308 + i).
        const double span = (static_cast<double>(uv[2]) - uv[0]) * 3070.0 / (2763 - 307);
        uint32_t first = 0;
        float w[kUiLayerMaxTaps] = {};
        const double x0 = uv[0] * 3070.0 + 100 * span;
        const int n = uiLayerFootprint(x0, x0 + span, 3070, &first, w);
        // (float uv leaves a neighbour a few millionths of weight: invisible)
        check(n >= 1 && first == 407 && w[0] > 0.9999f, "a cropped eye at 1.0 composites texel for texel");
    }
    const float whole[4] = {0, 0, 1, 1}, half[4] = {0, 0, 0.5f, 1}, cropped[4] = {0.1f, 0, 0.9f, 1};
    check(uiLayerRegionMatches(3070, 3032, whole, 3838, 3790), "a per-eye frame matches its layer");
    check(!uiLayerRegionMatches(3070, 3032, half, 3838, 3790), "half of a double-wide texture does not");
    check(uiLayerRegionMatches(2456, 3032, cropped, 3838, 3790), "a guard crop still matches");
}

// What a draw does with its depth-stencil target, from the D3D11 state the
// DLL reads (ui_layer_shaders.h's translation, ui_layer_math.h's effect).
void testDepthStencil() {
    D3D11_DEPTH_STENCIL_DESC d{};
    // The menu panel, its escape-menu variant and the loader (census
    // 2026-09-23): depth off, stencil ALWAYS / REPLACE, ref 4, write 0x04.
    d.DepthEnable = FALSE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_LESS;
    d.StencilEnable = TRUE;
    d.StencilReadMask = 0xFF;
    d.StencilWriteMask = 0x04;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                   D3D11_COMPARISON_ALWAYS};
    d.BackFace = d.FrontFace;
    UiDsEffect e = uiLayerDsEffect(uiLayerDsStateFrom(&d, 0), true);
    check(e.writes() && e.stencilWrite && !e.tests(), "the menu panel writes stencil and tests nothing");
    e = uiLayerDsEffect(uiLayerDsStateFrom(&d, D3D11_DSV_READ_ONLY_STENCIL), true);
    check(!e.writes() && !e.tests(), "through a read-only stencil view it writes nothing");
    e = uiLayerDsEffect(uiLayerDsStateFrom(&d, 0), false);
    check(!e.writes() && !e.tests(), "with no depth target bound it does nothing");
    d.StencilWriteMask = 0;
    check(!uiLayerDsEffect(uiLayerDsStateFrom(&d, 0), true).writes(), "a zero write mask writes nothing");
    d.StencilWriteMask = 0x04;
    d.FrontFace.StencilFunc = d.BackFace.StencilFunc = D3D11_COMPARISON_NOT_EQUAL;
    e = uiLayerDsEffect(uiLayerDsStateFrom(&d, 0), true);
    check(e.tests() && e.stencilTest && e.writes(), "NOT_EQUAL tests (and still writes)");
    d.StencilReadMask = 0;
    check(uiLayerDsEffect(uiLayerDsStateFrom(&d, 0), true).stencilTest,
          "a test with a zero read mask is still a test (NOT_EQUAL of 0 and 0 always fails)");
    check(uiLayerDsEffect(uiLayerDsStateFrom(nullptr, 0), true).depthTest &&
              uiLayerDsEffect(uiLayerDsStateFrom(nullptr, 0), true).depthWrite,
          "no state bound: D3D11's default depth test and write");
    D3D11_DEPTH_STENCIL_DESC a{};
    a.DepthEnable = TRUE;
    a.DepthFunc = D3D11_COMPARISON_ALWAYS;
    a.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    e = uiLayerDsEffect(uiLayerDsStateFrom(&a, 0), true);
    check(!e.tests() && !e.writes(), "depth ALWAYS without a write is neither");
}

// ---------------------------------------------------------- the surfaces

bool matchAt(uint32_t w, uint32_t h, uint32_t iw, uint32_t ih, const UiQualityRatio* t, uint32_t n,
             int want) {
    return uiQualityCandidateShape(w, h, iw, ih) &&
           uiQualityRatioFind(t, n, uiQualityRatioX10000(w, iw), uiQualityRatioX10000(h, ih)) == want;
}

void testSurfaces() {
    float f = 0.0f;
    check(uiQualityFactor(1.0f, 0.65f, &f) && near1(f, 1.0 / 0.65, 1e-5), "1.0 at HMD Quality 0.65: x1.5385");
    check(uiQualityFactor(1.25f, 0.65f, &f) && near1(f, 1.25 / 0.65, 1e-5), "1.25 at 0.65: x1.9231");
    check(uiQualityFactor(1.25f, 1.0f, &f) && near1(f, 1.25, 1e-6), "1.25 at 1.0: x1.25");
    check(!uiQualityFactor(1.0f, 1.0f, &f), "1.0 at 1.0 is a no-op");
    check(!uiQualityFactor(1.0f, 1.25f, &f), "HMD Quality above the target is a no-op");
    check(uiQualityFactor(1.0f, 0.98f, &f) && !uiQualityFactor(1.0f, 0.995f, &f), "the 1% floor's edge");
    check(uiQualityFactor(1.0f, 0.2f, &f) && f == 4.0f, "the 4x cap");
    check(!uiQualityFactor(1.0f, 0.0f, &f) && !uiQualityFactor(0.0f, 0.65f, &f) &&
              !uiQualityFactor(1.0f, -1.0f, &f),
          "an unknown HMD Quality or an off key does nothing");
    check(uiQualityRoundDim(908, 1.0f / 0.7f) == 1297 && uiQualityRoundDim(1361, 1.0f / 0.7f) == 1944,
          "908x1361 at 1/0.7 is 1297x1944");
    check(uiQualityRoundDim(908, 2.0f) == 1816 && uiQualityRoundDim(1361, 3.0f) == 4083,
          "a whole factor is exact");

    // The internal resolution, known before anything is submitted.
    check(uiQualityInternalDim(3070, 0.65f) == 1995 && uiQualityInternalDim(3032, 0.65f) == 1970,
          "3070x3032 at HMD Quality 0.65 renders 1995x1970");
    check(uiQualityInternalDim(3070, 0.5f) == 1535 && uiQualityInternalDim(3032, 0.5f) == 1516,
          "3070x3032 at 0.5 renders 1535x1516 (the 2026-09-23 flight's menu)");
    check(uiQualityInternalDim(2458, 0.65f) == 1597 && uiQualityInternalDim(2824, 0.65f) == 1835,
          "2458x2824 at 0.65 renders 1597x1835 (the same flight, FOV-trimmed)");
    check(uiQualityInternalDim(0, 0.65f) == 0 && uiQualityInternalDim(3070, 0.0f) == 0,
          "an unknown input is an unknown size");
    uint32_t rw = 0, rh = 0;
    const uint32_t ew[2] = {3069, 3070}, eh[2] = {3031, 3032};
    uiQualityRecommendedFromEyes(ew, eh, &rw, &rh);
    check(rw == 3070 && rh == 3032, "the recommendation: the larger eye, made even");
    const uint32_t ow[2] = {3071, 3000}, oh[2] = {2999, 2999};
    uiQualityRecommendedFromEyes(ow, oh, &rw, &rh);
    check(rw == 3072 && rh == 3000, "an odd size rounds up to even, as the runtime tells the game");

    // The census, against its own evidence.
    UiQualityRatio t[32];
    uint32_t n = uiQualitySeedTable(t, 32);
    check(n == kUiQualitySeedCount && n == 5, "five census ratios");
    check(matchAt(539, 807, 2576, 2544, t, n, 0) && matchAt(589, 883, 2818, 2784, t, n, 0) &&
              matchAt(1135, 1701, 5424, 5356, t, n, 0),
          "census 1 at 2576, 2818 and 5424 wide");
    check(matchAt(908, 1361, 4340, 4284, t, n, 0) && matchAt(1363, 2042, 6510, 6426, t, n, 0),
          "census 1 at fss_res.h's own two sessions");
    check(matchAt(1078, 674, 2576, 2544, t, n, 1) && matchAt(2271, 1419, 5424, 5356, t, n, 1), "census 2");
    check(matchAt(1267, 1036, 2818, 2784, t, n, 2) && matchAt(2440, 1996, 5424, 5356, t, n, 2), "census 3");
    check(matchAt(1769, 380, 2818, 2784, t, n, 3) && matchAt(3407, 732, 5424, 5356, t, n, 3), "census 4");
    check(matchAt(1354, 290, 1996, 2121, t, n, 4) && matchAt(1400, 300, 2064, 2208, t, n, 4), "census 5");
    // What the game would make at Sean's 1995x1970: census 1 at 0.2092 x 0.3175.
    check(matchAt(417, 625, 1995, 1970, t, n, 0), "census 1 at 1995x1970 (417x625)");
    check(!matchAt(544, 807, 2576, 2544, t, n, 0) && uiQualityRatioFind(t, n, uiQualityRatioX10000(544, 2576),
                                                                        uiQualityRatioX10000(807, 2544)) < 0,
          "one percent wider matches nothing");
    check(!uiQualityCandidateShape(512, 512, 1995, 1970) && !uiQualityCandidateShape(256, 1, 1995, 1970) &&
              !uiQualityCandidateShape(1995, 1970, 1995, 1970) &&
              !uiQualityCandidateShape(3840, 2160, 1995, 1970) && !uiQualityCandidateShape(10, 12, 1995, 1970) &&
              uiQualityCandidateShape(417, 625, 1995, 1970),
          "the candidate shape: smaller than the eye, no power of two, no sliver");

    // The learned-ratio file.
    const char* file = "# fix.ui_quality\n2092 3175\n9999 1\nfoo\n4000 2000 7\n12 34\r\n12 40\n";
    const uint32_t taken = uiQualityParseRatios(file, t, &n, 32);
    check(taken == 2 && n == 7 && t[5].w == 9999 && t[5].h == 1 && t[6].w == 12 && t[6].h == 34 &&
              t[6].origin == UiQualityOrigin::kLearned,
          "the file: comments, a census repeat, junk, a third number and a near-repeat add nothing");
    check(!uiQualityLearn(t, &n, 32, 2095, 3170) && uiQualityLearn(t, &n, 32, 5000, 5000) && n == 8 &&
              !uiQualityLearn(t, &n, 32, 10000, 5),
          "learning: a ratio within the tolerance of one on file is not added");
}

// ------------------------------------------------------------ the GPU

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11Buffer> quadCb, compCb;
    ComPtr<ID3D11RasterizerState> noCull;
};

const char kQuadHlsl[] = R"HLSL(
cbuffer Q : register(b0) { float4 rect; float4 colour; float4 jitter; };
float4 vsMain(uint id : SV_VertexID) : SV_Position {
    float2 c = float2((id & 1) ? rect.z : rect.x, (id & 2) ? rect.w : rect.y);
    return float4(c + jitter.xy, 0, 1);
}
float4 psMain() : SV_Target { return colour; }
)HLSL";

struct QuadCb {
    float rect[4];
    float colour[4];
    float jitter[4];
};

bool compile(const char* src, size_t len, const char* entry, const char* profile, ComPtr<ID3DBlob>* out) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(src, len, "ui_quality_test", nullptr, nullptr, entry, profile,
                                  D3DCOMPILE_ENABLE_STRICTNESS, 0, out->GetAddressOf(), &err);
    if (FAILED(hr) && err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
    return SUCCEEDED(hr);
}

bool setup(Gpu& g, bool hardware) {
    D3D_FEATURE_LEVEL fl{};
    if (FAILED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr,
                                 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx))) {
        return false;
    }
    ComPtr<ID3DBlob> v, p, c;
    if (!compile(kQuadHlsl, sizeof(kQuadHlsl) - 1, "vsMain", "vs_5_0", &v) ||
        !compile(kQuadHlsl, sizeof(kQuadHlsl) - 1, "psMain", "ps_5_0", &p) ||
        !compile(kUiLayerCompositeHlsl, sizeof(kUiLayerCompositeHlsl) - 1, "main", "cs_5_0", &c)) {
        return false;
    }
    if (FAILED(g.dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &g.vs)) ||
        FAILED(g.dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &g.ps)) ||
        FAILED(g.dev->CreateComputeShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &g.cs))) {
        return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(QuadCb);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.quadCb))) return false;
    bd.ByteWidth = sizeof(UiLayerCompositeParams);
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.compCb))) return false;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(g.dev->CreateRasterizerState(&rd, &g.noCull))) return false;
    return true;
}

struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    uint32_t w = 0, h = 0;
};

Tex makeTex(Gpu& g, uint32_t w, uint32_t h, bool uav) {
    Tex t;
    t.w = w;
    t.h = h;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET);
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &t.tex))) return t;
    g.dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
    if (uav) {
        g.dev->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav);
    } else {
        g.dev->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv);
    }
    return t;
}

std::vector<uint8_t> readBack(Gpu& g, const Tex& t) {
    D3D11_TEXTURE2D_DESC d{};
    t.tex->GetDesc(&d);
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    std::vector<uint8_t> out(static_cast<size_t>(t.w) * t.h * 4);
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &st))) return out;
    g.ctx->CopyResource(st.Get(), t.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return out;
    for (uint32_t y = 0; y < t.h; ++y) {
        std::memcpy(&out[static_cast<size_t>(y) * t.w * 4], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch,
                    static_cast<size_t>(t.w) * 4);
    }
    g.ctx->Unmap(st.Get(), 0);
    return out;
}

void quad(Gpu& g, const Tex& target, const float rect[4], const float colour[4], float jx, float jy,
          const D3D11_VIEWPORT& vp, ID3D11BlendState* bs) {
    QuadCb q{};
    std::memcpy(q.rect, rect, sizeof(q.rect));
    std::memcpy(q.colour, colour, sizeof(q.colour));
    q.jitter[0] = jx;
    q.jitter[1] = jy;
    g.ctx->UpdateSubresource(g.quadCb.Get(), 0, nullptr, &q, 0, 0);
    ID3D11RenderTargetView* rtv = target.rtv.Get();
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g.ctx->OMSetBlendState(bs, nullptr, 0xFFFFFFFFu);
    g.ctx->RSSetState(g.noCull.Get());
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, g.quadCb.GetAddressOf());
    g.ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    g.ctx->PSSetConstantBuffers(0, 1, g.quadCb.GetAddressOf());
    g.ctx->Draw(4, 0);
}

void composite(Gpu& g, const Tex& frame, const uint32_t region[4], const float uv[4], const Tex& layer,
               const Tex& out, uint32_t mode, const Tex* mult = nullptr) {
    UiLayerCompositeParams p{};
    for (int i = 0; i < 4; ++i) p.region[i] = static_cast<int32_t>(region[i]);
    std::memcpy(p.uv, uv, sizeof(p.uv));
    p.layerSize[0] = static_cast<float>(layer.w);
    p.layerSize[1] = static_cast<float>(layer.h);
    p.outSize[0] = region[2] - region[0];
    p.outSize[1] = region[3] - region[1];
    p.mode = mode;
    p.useMult = mult ? 1u : 0u;
    g.ctx->UpdateSubresource(g.compCb.Get(), 0, nullptr, &p, 0, 0);
    ID3D11RenderTargetView* none = nullptr;
    g.ctx->OMSetRenderTargets(1, &none, nullptr);
    g.ctx->CSSetShader(g.cs.Get(), nullptr, 0);
    g.ctx->CSSetConstantBuffers(0, 1, g.compCb.GetAddressOf());
    ID3D11ShaderResourceView* srvs[3] = {frame.srv.Get(), layer.srv.Get(), mult ? mult->srv.Get() : nullptr};
    g.ctx->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uav = out.uav.Get();
    g.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    g.ctx->Dispatch((p.outSize[0] + 7) / 8, (p.outSize[1] + 7) / 8, 1);
    ID3D11ShaderResourceView* nulls[3] = {};
    g.ctx->CSSetShaderResources(0, 3, nulls);
    ID3D11UnorderedAccessView* nullUav = nullptr;
    g.ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

ComPtr<ID3D11BlendState> state(Gpu& g, const UiBlendRt& b) {
    const D3D11_BLEND_DESC d = uiLayerBlendDesc(b);
    ComPtr<ID3D11BlendState> s;
    g.dev->CreateBlendState(&d, &s);
    return s;
}

// A jittered quad through the redirected viewport lands exactly where the
// unjittered one does, for every Halton phase; without the cancel it does
// not (the check has teeth).
void testRedirect(Gpu& g, float target) {
    const uint32_t W = 40, H = 30;
    const UiLayerSize ls = uiLayerSize(W, H, target);
    Tex ref = makeTex(g, ls.w, ls.h, false), lay = makeTex(g, ls.w, ls.h, false);
    const UiLayerMap m = uiLayerMapFromRegion(0, 0, static_cast<float>(W), static_cast<float>(H), ls.w, ls.h);
    // Edges 0.3 of a layer pixel past a boundary: 0.2 from the nearest pixel
    // centre, so float error cannot move one across a centre, while any
    // uncancelled jitter of a quarter pixel or more does.
    // Inside the layer's outermost row and column: a sub-pixel viewport
    // origin clips its own far edge to whole pixels (measured on WARP and on
    // a hardware adapter alike: a viewport ending at 29.83 draws no row 29),
    // so a negative cancel can cost the layer its last row or column -- the
    // eye's edge, under the lens mask. docs/ui-layer-2026-09-23.md records it.
    const float left = 10.3f, right = 31.3f, top = 7.3f, bottom = 24.3f;  // layer pixels
    const float rect[4] = {-1.0f + 2.0f * left / ls.w, 1.0f - 2.0f * bottom / ls.h,
                           -1.0f + 2.0f * right / ls.w, 1.0f - 2.0f * top / ls.h};
    const float white[4] = {1, 1, 1, 1};
    const float clear[4] = {0, 0, 0, 1};
    UiBlendRt conv;
    uiLayerConvertBlend(UiBlendRt{}, &conv);  // the opaque draw's conversion
    ComPtr<ID3D11BlendState> bs = state(g, conv);
    const D3D11_VIEWPORT refVp{0, 0, static_cast<float>(ls.w), static_cast<float>(ls.h), 0, 1};
    g.ctx->ClearRenderTargetView(ref.rtv.Get(), clear);
    quad(g, ref, rect, white, 0, 0, refVp, bs.Get());
    const std::vector<uint8_t> want = readBack(g, ref);
    bool allSame = true, anyDiffers = false;
    for (uint32_t n = 0; n < kTemporalJitterCount; ++n) {
        float jx = 0, jy = 0;
        temporalJitter(n, &jx, &jy);
        // The game's projection: content jx pixels right, jy pixels down.
        const float ndcX = 2.0f * jx / W, ndcY = -2.0f * jy / H;
        UiViewport gv;
        gv.w = static_cast<float>(W);
        gv.h = static_cast<float>(H);
        float cx = 0, cy = 0;
        uiLayerJitterCancel(jx, jy, m, &cx, &cy);
        const UiViewport v = uiLayerMapViewport(m, gv, cx, cy);
        const D3D11_VIEWPORT vp{v.x, v.y, v.w, v.h, v.minZ, v.maxZ};
        g.ctx->ClearRenderTargetView(lay.rtv.Get(), clear);
        quad(g, lay, rect, white, ndcX, ndcY, vp, bs.Get());
        {
            const std::vector<uint8_t> got = readBack(g, lay);
            if (got != want) {
                for (size_t i = 0; i < got.size(); i += 4) {
                    if (got[i] != want[i]) {
                        std::printf("  phase %u (jx %.4f jy %.4f, viewport %.4f,%.4f): first differing "
                                    "pixel (%zu,%zu) got %u want %u\n",
                                    n, jx, jy, vp.TopLeftX, vp.TopLeftY, (i / 4) % ls.w, (i / 4) / ls.w,
                                    got[i], want[i]);
                        break;
                    }
                }
            }
            allSame = allSame && got == want;
        }
        const UiViewport u = uiLayerMapViewport(m, gv, 0, 0);
        const D3D11_VIEWPORT uvp{u.x, u.y, u.w, u.h, 0, 1};
        g.ctx->ClearRenderTargetView(lay.rtv.Get(), clear);
        quad(g, lay, rect, white, ndcX, ndcY, uvp, bs.Get());
        anyDiffers = anyDiffers || readBack(g, lay) != want;
    }
    check(allSame, target > 1.1f ? "1.25: every Halton phase rasterises onto the unjittered pixels"
                                 : "1.0: every Halton phase rasterises onto the unjittered pixels");
    check(anyDiffers, target > 1.1f ? "1.25: without the cancel some phase lands elsewhere"
                                    : "1.0: without the cancel some phase lands elsewhere");
}

uint8_t q8(float v) { return static_cast<uint8_t>(std::lround(uiSat(v) * 255.0f)); }

// Draws blended into the layer and composited equal the same draws blended
// straight into the frame, within the 8-bit rounding of the stores.
void testComposite(Gpu& g) {
    using namespace uiblend;
    const uint32_t W = 24, H = 16;
    Tex frame = makeTex(g, W, H, false), direct = makeTex(g, W, H, false), layer = makeTex(g, W, H, false);
    Tex mult = makeTex(g, W, H, false);
    Tex out = makeTex(g, W, H, true);
    const float base[4] = {0.2f, 0.5f, 0.7f, 0.6f};
    g.ctx->ClearRenderTargetView(frame.rtv.Get(), base);
    g.ctx->ClearRenderTargetView(direct.rtv.Get(), base);
    g.ctx->ClearRenderTargetView(layer.rtv.Get(), kUiLayerClear);
    g.ctx->ClearRenderTargetView(mult.rtv.Get(), kUiLayerMultClear);
    struct Step {
        UiBlendRt game;
        float rect[4];
        float colour[4];
    };
    // The loading screen's order: a panel, the gamma multiply over part of
    // the frame and part of the panel, then more UI over it.
    const Step steps[] = {
        {blend(true, kOne, kInvSrcAlpha, kWriteRgb), {-1, -1, 0.5f, 1}, {0.3f * 0.6f, 0.1f * 0.6f, 0.9f * 0.6f, 0.6f}},
        {blend(true, kZero, kSrcColor), {-0.25f, -1, 1, 0.75f}, {0.5f, 0.8f, 0.3f, 1.0f}},
        {blend(true, kSrcAlpha, kInvSrcAlpha), {-0.5f, -0.5f, 1, 0.5f}, {1.0f, 0.8f, 0.1f, 0.35f}},
        {blend(true, kDestColor, kZero), {-1, 0, 0.25f, 1}, {0.9f, 0.6f, 0.7f, 1.0f}},
        {blend(true, kOne, kOne), {0, -1, 1, 1}, {0.05f, 0.1f, 0.02f, 0.0f}},
        {blend(false, kOne, kZero), {0.5f, 0.5f, 1, 1}, {0.9f, 0.1f, 0.4f, 0.2f}},
    };
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
    for (const Step& s : steps) {
        D3D11_BLEND_DESC gd = uiLayerBlendDesc(s.game);
        ComPtr<ID3D11BlendState> gs;
        g.dev->CreateBlendState(&gd, &gs);
        quad(g, direct, s.rect, s.colour, 0, 0, vp, gs.Get());
        UiBlendRt conv, second;
        check(uiLayerConvertBlend(s.game, &conv), "the step's blend converts");
        quad(g, layer, s.rect, s.colour, 0, 0, vp, state(g, conv).Get());
        if (uiLayerMultiplyBlend(s.game, &second)) quad(g, mult, s.rect, s.colour, 0, 0, vp, state(g, second).Get());
    }
    const uint32_t region[4] = {0, 0, W, H};
    const float uvFull[4] = {0, 0, 1, 1};
    composite(g, frame, region, uvFull, layer, out, 0, &mult);
    const std::vector<uint8_t> got = readBack(g, out), want = readBack(g, direct), f = readBack(g, frame);
    int worst = 0;
    size_t worstAt = 0;
    bool alphaKept = true;
    for (size_t i = 0; i < got.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs(int(got[i + c]) - int(want[i + c]));
            if (d > worst) {
                worst = d;
                worstAt = i + c;
            }
        }
        alphaKept = alphaKept && got[i + 3] == f[i + 3];
    }
    if (worst > 3) {
        const std::vector<uint8_t> L = readBack(g, layer), M = readBack(g, mult);
        const size_t px = worstAt / 4;
        std::printf("  worst %d at pixel (%zu,%zu) channel %zu: got %u want %u; L %u T %u M %u F %u\n", worst,
                    px % W, px / W, worstAt % 4, got[worstAt], want[worstAt], L[worstAt], L[px * 4 + 3],
                    M[worstAt], f[worstAt]);
    }
    check(worst <= 3,
          "over, premultiplied over, two multiplies, additive and opaque: layer + transmittance + "
          "composite = the game's blends (8-bit)");
    check(alphaKept, "the composite keeps the frame's alpha");
    // Without the transmittance the multiplies' tint on the frame is lost:
    // the check above has teeth.
    composite(g, frame, region, uvFull, layer, out, 0);
    const std::vector<uint8_t> noMult = readBack(g, out);
    int lost = 0;
    for (size_t i = 0; i < noMult.size(); i += 4)
        for (int c = 0; c < 3; ++c) lost = (std::max)(lost, std::abs(int(noMult[i + c]) - int(want[i + c])));
    check(lost > 10, "without the multiply's transmittance the frame is not tinted");
    g.ctx->ClearRenderTargetView(layer.rtv.Get(), kUiLayerClear);

    // A single coloured quad over a known frame: the exact expected pixels.
    g.ctx->ClearRenderTargetView(layer.rtv.Get(), kUiLayerClear);
    const float rect[4] = {-0.5f, -0.5f, 0.5f, 0.5f};  // pixels 6..17 x 4..11
    const float c[4] = {0.8f * 0.5f, 0.2f * 0.5f, 0.4f * 0.5f, 0.5f};
    UiBlendRt conv;
    uiLayerConvertBlend(blend(true, kOne, kInvSrcAlpha), &conv);
    quad(g, layer, rect, c, 0, 0, vp, state(g, conv).Get());
    composite(g, frame, region, uvFull, layer, out, 0);
    const std::vector<uint8_t> one = readBack(g, out), Lb = readBack(g, layer);
    const size_t inside = (static_cast<size_t>(8) * W + 12) * 4, outside = (static_cast<size_t>(1) * W + 1) * 4;
    // What the layer and the frame actually store (the hardware's own
    // rounding of 0.7 and 0.5), then the composite: L + F * T.
    check(Lb[inside] == q8(c[0]) && std::abs(int(Lb[inside + 3]) - 128) <= 1 && Lb[outside + 3] == 255,
          "the layer holds the premultiplied colour and the transmittance (1 - a); 1 where nothing drew");
    const float T = Lb[inside + 3] / 255.0f;
    bool colourOk = true;
    for (int ch = 0; ch < 3; ++ch) {
        const uint8_t e = q8(Lb[inside + ch] / 255.0f + f[inside + ch] / 255.0f * T);
        colourOk = colourOk && std::abs(int(one[inside + ch]) - int(e)) <= 1;
    }
    check(colourOk, "a coloured quad composited over a known frame is the expected colour");
    check(one[outside] == f[outside] && one[outside + 1] == f[outside + 1] && one[outside + 2] == f[outside + 2],
          "where the layer holds nothing the frame is untouched");

    // The debug view: the layer over black, a blue wash where it covers.
    composite(g, frame, region, uvFull, layer, out, 1);
    const std::vector<uint8_t> dbg = readBack(g, out);
    check(dbg[outside] == 0 && dbg[outside + 1] == 0 && dbg[outside + 2] == 0, "debug view: uncovered is black");
    check(std::abs(int(dbg[inside]) - int(Lb[inside])) <= 1 &&
              std::abs(int(dbg[inside + 2]) - q8(Lb[inside + 2] / 255.0f + 0.25f * (1.0f - T))) <= 1,
          "debug view: covered shows the layer with the coverage wash");

    // A cropped rectangle: the frame's region is the layer's middle half.
    Tex half = makeTex(g, W / 2, H, true), frameHalf = makeTex(g, W / 2, H, false);
    g.ctx->ClearRenderTargetView(frameHalf.rtv.Get(), base);
    const uint32_t hr[4] = {0, 0, W / 2, H};
    const float uvMid[4] = {0.25f, 0, 0.75f, 1};
    composite(g, frameHalf, hr, uvMid, layer, half, 0);
    const std::vector<uint8_t> mid = readBack(g, half);
    // Output (6, 8) is layer (12, 8): inside the quad.
    const size_t at = (static_cast<size_t>(8) * (W / 2) + 6) * 4;
    check(std::abs(int(mid[at]) - int(one[inside])) <= 1, "a cropped rectangle samples the layer's matching pixels");
}

// At 1.25 the composite is the box filter of the layer over each pixel.
void testDownsample(Gpu& g) {
    using namespace uiblend;
    const uint32_t W = 16, H = 12;
    const UiLayerSize ls = uiLayerSize(W, H, 1.25f);  // 20x15
    Tex frame = makeTex(g, W, H, false), layer = makeTex(g, ls.w, ls.h, false), out = makeTex(g, W, H, true);
    const float base[4] = {0.1f, 0.3f, 0.6f, 1.0f};
    g.ctx->ClearRenderTargetView(frame.rtv.Get(), base);
    g.ctx->ClearRenderTargetView(layer.rtv.Get(), kUiLayerClear);
    const float rect[4] = {-1.0f + 2.0f * 5 / ls.w, -1.0f + 2.0f * 3 / ls.h, -1.0f + 2.0f * 13 / ls.w,
                           -1.0f + 2.0f * 11 / ls.h};
    const float c[4] = {0.9f, 0.6f, 0.3f, 1.0f};
    UiBlendRt conv;
    uiLayerConvertBlend(blend(false, kOne, kZero), &conv);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(ls.w), static_cast<float>(ls.h), 0, 1};
    quad(g, layer, rect, c, 0, 0, vp, state(g, conv).Get());
    const uint32_t region[4] = {0, 0, W, H};
    const float uv[4] = {0, 0, 1, 1};
    composite(g, frame, region, uv, layer, out, 0);
    const std::vector<uint8_t> got = readBack(g, out), L = readBack(g, layer), F = readBack(g, frame);
    int worst = 0;
    const double s = 1.25;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            uint32_t fx = 0, fy = 0;
            float wx[kUiLayerMaxTaps], wy[kUiLayerMaxTaps];
            const int nx = uiLayerFootprint(x * s, (x + 1) * s, ls.w, &fx, wx);
            const int ny = uiLayerFootprint(y * s, (y + 1) * s, ls.h, &fy, wy);
            double acc[4] = {};
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const size_t k = (static_cast<size_t>(fy + j) * ls.w + fx + i) * 4;
                    for (int ch = 0; ch < 4; ++ch) acc[ch] += L[k + ch] / 255.0 * wx[i] * wy[j];
                }
            }
            const size_t o = (static_cast<size_t>(y) * W + x) * 4;
            for (int ch = 0; ch < 3; ++ch) {
                const double e = acc[ch] + F[o + ch] / 255.0 * acc[3];
                worst = (std::max)(worst, std::abs(int(got[o + ch]) - int(std::lround((std::min)(e, 1.0) * 255.0))));
            }
        }
    }
    check(worst <= 1, "1.25: the composite is the box filter of the layer over each pixel's footprint");
    // The quad's interior is covered on all sides: fully opaque, the quad's colour.
    const size_t in = (static_cast<size_t>(5) * W + 7) * 4;
    check(std::abs(int(got[in]) - q8(0.9f)) <= 1, "1.25: a pixel inside the quad is the quad's colour");
}

// ------------------------------------------------ depth and stencil on the GPU

struct Ds {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> depth, stencil;
    uint32_t w = 0, h = 0;
};

// The game's depth-stencil shape (D24S8 here; the flight's is D32S8 -- the
// seed reads both through the same two views).
Ds makeDs(Gpu& g, uint32_t w, uint32_t h, bool views) {
    Ds d;
    d.w = w;
    d.h = h;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | (views ? D3D11_BIND_SHADER_RESOURCE : 0);
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &d.tex))) return d;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g.dev->CreateDepthStencilView(d.tex.Get(), &dv, &d.dsv);
    if (views) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        g.dev->CreateShaderResourceView(d.tex.Get(), &sd, &d.depth);
        sd.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        g.dev->CreateShaderResourceView(d.tex.Get(), &sd, &d.stencil);
    }
    return d;
}

// The menu panel's stencil write (ALWAYS / REPLACE, write 0x04, depth off),
// and a draw testing it (EQUAL under read mask 0x04).
ComPtr<ID3D11DepthStencilState> stencilState(Gpu& g, bool writer) {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = FALSE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    d.StencilEnable = TRUE;
    d.StencilReadMask = writer ? 0xFF : 0x04;
    d.StencilWriteMask = writer ? 0x04 : 0x00;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                   writer ? D3D11_STENCIL_OP_REPLACE : D3D11_STENCIL_OP_KEEP,
                   writer ? D3D11_COMPARISON_ALWAYS : D3D11_COMPARISON_EQUAL};
    d.BackFace = d.FrontFace;
    ComPtr<ID3D11DepthStencilState> s;
    const HRESULT hr = g.dev->CreateDepthStencilState(&d, &s);
    if (FAILED(hr)) std::printf("  CreateDepthStencilState(%s) failed: 0x%08lX\n", writer ? "writer" : "tester", hr);
    return s;
}

void quadDs(Gpu& g, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, ID3D11DepthStencilState* dss,
            const float rect[4], const float colour[4], float ndcX, float ndcY, const D3D11_VIEWPORT& vp,
            ID3D11BlendState* bs) {
    QuadCb q{};
    std::memcpy(q.rect, rect, sizeof(q.rect));
    std::memcpy(q.colour, colour, sizeof(q.colour));
    q.jitter[0] = ndcX;
    q.jitter[1] = ndcY;
    g.ctx->UpdateSubresource(g.quadCb.Get(), 0, nullptr, &q, 0, 0);
    g.ctx->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
    g.ctx->OMSetDepthStencilState(dss, 4);
    g.ctx->OMSetBlendState(bs, nullptr, 0xFFFFFFFFu);
    g.ctx->RSSetState(g.noCull.Get());
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, g.quadCb.GetAddressOf());
    g.ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    g.ctx->PSSetConstantBuffers(0, 1, g.quadCb.GetAddressOf());
    g.ctx->Draw(4, 0);
    g.ctx->OMSetDepthStencilState(nullptr, 0);
}

std::vector<uint8_t> readBackDs(Gpu& g, const Ds& d) {
    D3D11_TEXTURE2D_DESC td{};
    d.tex->GetDesc(&td);
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    std::vector<uint8_t> out(static_cast<size_t>(d.w) * d.h * 4);
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &st))) return out;
    g.ctx->CopyResource(st.Get(), d.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return out;
    for (uint32_t y = 0; y < d.h; ++y)
        std::memcpy(&out[static_cast<size_t>(y) * d.w * 4], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch,
                    static_cast<size_t>(d.w) * 4);
    g.ctx->Unmap(st.Get(), 0);
    return out;
}

// The menu panel writes its footprint into stencil; a later draw tests it.
// The layer takes the tester: the layer's depth-stencil target is seeded
// from the game's (ui_deferred_depth.h's Seeder, the DLL's own) with the
// frame's jitter cancelled, the tester is drawn into the layer against it
// through the redirected viewport, and the composite must equal the tester
// drawn into the frame against an UNJITTERED footprint -- exactly, at 1.0
// under three jitters, and at 1.25. (The footprint's edges sit on pixel
// boundaries -- at 1.25, on boundaries of both grids, every fourth game
// pixel: a nearest-sample reprojection is exact there for any jitter under
// half a pixel. Anywhere else it can be half a LAYER pixel off at the edge
// -- the one error the seed makes, at a mask's rim; the last case measures
// that it stays inside the rim's own row of pixels.)
void testSeededStencil(Gpu& g) {
    edvr_deferred_depth::Seeder seeder;
    try {
        seeder.init(g.dev.Get());
    } catch (const std::exception& e) {
        std::printf("  seeder: %s\n", e.what());
        check(false, "the depth-stencil seed builds");
        return;
    }
    const uint32_t W = 24, H = 16;
    const float rectB[4] = {-1.2f, -1.2f, 1.2f, 1.2f};
    // 0.8 is 204 exactly: a colour on a .5 step stores as either neighbour
    // (WARP rounds 229.5 up, the desk's NVIDIA adapter down).
    const float colourB[4] = {0.8f, 0.2f, 0.4f, 1.0f};
    const float base[4] = {0.2f, 0.5f, 0.7f, 1.0f};
    const float white[4] = {1, 1, 1, 1};
    ComPtr<ID3D11DepthStencilState> writer = stencilState(g, true), tester = stencilState(g, false);
    UiBlendRt conv;
    uiLayerConvertBlend(UiBlendRt{}, &conv);  // B is opaque
    ComPtr<ID3D11BlendState> layerBlend = state(g, conv);
    const D3D11_VIEWPORT gameVp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};

    Tex frame = makeTex(g, W, H, false);
    g.ctx->ClearRenderTargetView(frame.rtv.Get(), base);

    struct Case {
        float target, jx, jy;
        uint32_t x0, x1;  // A's footprint, game pixels [x0, x1) x [4, 12)
        bool rim;         // edges off the 1.25 grid: only the rim may differ
        const char* what;
    };
    const Case cases[] = {
        {1.0f, 0.0f, 0.0f, 8, 20, false, "1.0, no jitter"},
        {1.0f, 0.375f, -0.25f, 8, 20, false, "1.0, jitter (0.375, -0.25)"},
        {1.0f, -0.4375f, 0.3125f, 10, 20, false, "1.0, jitter (-0.4375, 0.3125)"},
        {1.25f, 0.0f, 0.0f, 8, 20, false, "1.25"},
        {1.25f, 0.0f, 0.0f, 10, 20, true, "1.25, an edge off the grid"},
    };
    for (const Case& c : cases) {
        const float rectA[4] = {-1.0f + 2.0f * c.x0 / W, 1.0f - 2.0f * 12 / H, -1.0f + 2.0f * c.x1 / W,
                                1.0f - 2.0f * 4 / H};
        // The reference: A's footprint written unjittered, B tested into the frame.
        Tex direct = makeTex(g, W, H, false);
        Ds refDs = makeDs(g, W, H, false);
        g.ctx->ClearRenderTargetView(direct.rtv.Get(), base);
        g.ctx->ClearDepthStencilView(refDs.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        quadDs(g, nullptr, refDs.dsv.Get(), writer.Get(), rectA, white, 0, 0, gameVp, nullptr);
        quadDs(g, direct.rtv.Get(), refDs.dsv.Get(), tester.Get(), rectB, colourB, 0, 0, gameVp, nullptr);
        const std::vector<uint8_t> want = readBack(g, direct);
        {
            const std::vector<uint8_t> st = readBackDs(g, refDs);
            int fours = 0, bs = 0;
            for (size_t i = 3; i < st.size(); i += 4) fours += st[i] == 4 ? 1 : 0;
            for (size_t i = 0; i < want.size(); i += 4) bs += want[i] == q8(colourB[0]) ? 1 : 0;
            if (bs != static_cast<int>((c.x1 - c.x0) * 8))
                std::printf("  reference (%s): %d stencil 4s, %d pixels of B\n", c.what, fours, bs);
        }
        // The game: A written into its depth-stencil with the frame's jitter.
        Ds game = makeDs(g, W, H, true);
        g.ctx->ClearDepthStencilView(game.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        quadDs(g, nullptr, game.dsv.Get(), writer.Get(), rectA, white, 2.0f * c.jx / W, -2.0f * c.jy / H, gameVp,
               nullptr);
        // The layer: seeded, then B through the redirected viewport.
        const UiLayerSize ls = uiLayerSize(W, H, c.target);
        Tex layer = makeTex(g, ls.w, ls.h, false), out = makeTex(g, W, H, true);
        Ds layerDs = makeDs(g, ls.w, ls.h, false);
        g.ctx->ClearRenderTargetView(layer.rtv.Get(), kUiLayerClear);
        try {
            seeder.seed(g.ctx.Get(), game.depth.Get(), game.stencil.Get(), layerDs.dsv.Get(), W, H, ls.w, ls.h,
                        c.jx, c.jy, 0x04, false);
        } catch (const std::exception& e) {
            std::printf("  seed: %s\n", e.what());
            check(false, "the seed runs");
            continue;
        }
        const UiLayerMap m = uiLayerMapFromRegion(0, 0, static_cast<float>(W), static_cast<float>(H), ls.w, ls.h);
        float cx = 0, cy = 0;
        uiLayerJitterCancel(c.jx, c.jy, m, &cx, &cy);
        UiViewport gv;
        gv.w = static_cast<float>(W);
        gv.h = static_cast<float>(H);
        const UiViewport v = uiLayerMapViewport(m, gv, cx, cy);
        const D3D11_VIEWPORT vp{v.x, v.y, v.w, v.h, v.minZ, v.maxZ};
        quadDs(g, layer.rtv.Get(), layerDs.dsv.Get(), tester.Get(), rectB, colourB, 2.0f * c.jx / W,
               -2.0f * c.jy / H, vp, layerBlend.Get());
        const uint32_t region[4] = {0, 0, W, H};
        const float uv[4] = {0, 0, 1, 1};
        composite(g, frame, region, uv, layer, out, 0);
        const std::vector<uint8_t> got = readBack(g, out);
        int worst = 0, bPixels = 0, offRim = 0;
        for (size_t i = 0; i < got.size(); i += 4) {
            int d = 0;
            for (int ch = 0; ch < 3; ++ch) d = (std::max)(d, std::abs(int(got[i + ch]) - int(want[i + ch])));
            const uint32_t x = static_cast<uint32_t>((i / 4) % W), y = static_cast<uint32_t>((i / 4) / W);
            // The rim: the pixel rows and columns either side of each edge.
            const bool nearX = x + 1 >= c.x0 && x <= c.x0 || x + 1 >= c.x1 && x <= c.x1;
            const bool nearY = y + 1 >= 4 && y <= 4 || y + 1 >= 12 && y <= 12;
            if (c.rim && (nearX || nearY)) continue;
            if (d > 1) ++offRim;
            worst = (std::max)(worst, d);
            bPixels += got[i] == q8(colourB[0]) ? 1 : 0;
        }
        const int wantB = c.rim ? -1 : static_cast<int>((c.x1 - c.x0) * 8);
        char what[160];
        std::snprintf(what, sizeof(what),
                      c.rim ? "a stencil-tested draw against the layer's seeded copy differs only at the rim (%s)"
                            : "a stencil-tested draw against the layer's seeded copy lands where the game's does (%s)",
                      c.what);
        if (worst > 1 || (wantB >= 0 && bPixels != wantB)) {
            std::printf("  %s: worst %d, %d pixels of B (seed %s)\n", c.what, worst, bPixels,
                        seeder.usesSpecifiedStencilRef() ? "one pass" : "per bit");
            for (uint32_t y = 0; y < H; ++y) {
                std::printf("   ");
                for (uint32_t x = 0; x < W; ++x) {
                    const size_t i = (static_cast<size_t>(y) * W + x) * 4;
                    std::printf("%c", got[i] == want[i] ? (got[i] == q8(colourB[0]) ? 'B' : '.')
                                                        : (got[i] == q8(colourB[0]) ? '+' : '-'));
                }
                std::printf("\n");
            }
        }
        check(worst <= 1 && offRim == 0 && (wantB < 0 || bPixels == wantB), what);
    }
}

// The write-back: the stencil writer issued once more with NO colour target
// leaves the game's depth-stencil exactly as the original draw did.
void testWriteBack(Gpu& g) {
    const uint32_t W = 24, H = 16;
    const float rectA[4] = {-0.6f, -0.3f, 0.45f, 0.8f};
    const float white[4] = {1, 1, 1, 1};
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
    ComPtr<ID3D11DepthStencilState> writer = stencilState(g, true);
    Tex frame = makeTex(g, W, H, false);
    Ds original = makeDs(g, W, H, false), back = makeDs(g, W, H, false);
    g.ctx->ClearDepthStencilView(original.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.5f, 1);
    g.ctx->ClearDepthStencilView(back.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.5f, 1);
    quadDs(g, frame.rtv.Get(), original.dsv.Get(), writer.Get(), rectA, white, 0, 0, vp, nullptr);
    quadDs(g, nullptr, back.dsv.Get(), writer.Get(), rectA, white, 0, 0, vp, nullptr);
    const std::vector<uint8_t> a = readBackDs(g, original), b = readBackDs(g, back);
    size_t written = 0;
    for (size_t i = 3; i < a.size(); i += 4) written += a[i] == 5 ? 1 : 0;  // 1 | 4
    check(a == b && written > 0, "the write-back leaves the game's stencil as the original draw did");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("ui_quality_test: dry-run (no device, no files)");
        return 0;
    }
    // --hardware: the same checks on the default hardware adapter instead of
    // WARP, by hand (the gate runs --self-test; a build machine may have no GPU).
    const bool hardware = argc == 2 && std::strcmp(argv[1], "--hardware") == 0;
    if (argc != 2 || (std::strcmp(argv[1], "--self-test") != 0 && !hardware)) {
        std::puts("usage: ui_quality_test --self-test | --hardware | --dry-run");
        return 2;
    }
    testSurfaces();
    testKey();
    testSize();
    testMap();
    testScissor();
    testBlend();
    testDepthStencil();
    testFootprint();
    testGate();
    Gpu g;
    if (!setup(g, hardware)) {
        check(false, "a device and the production composite shader");
    } else {
        testRedirect(g, 1.0f);
        testRedirect(g, 1.25f);
        testComposite(g);
        testDownsample(g);
        testSeededStencil(g);
        testWriteBack(g);
    }
    std::printf("ui_quality_test: %u checks, %u failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
