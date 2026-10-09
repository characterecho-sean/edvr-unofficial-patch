// Build gate for fix.ui_quality (docs/ui-layer-2026-09-23.md), both halves
// of the one key, from the headers the DLL compiles:
//
//   * the panels (src/d3d11/ui_quality_math.h and ui_sizing_math.h): the
//     internal render resolution from the runtime's recommendation and HMD
//     Quality (3070 x 0.65 = 1995 and the rest), the vertical field of view
//     as the panel formula reads it, the interface surface's shape; the
//     sizing chains' formatter and verdicts; the engine-side panel factor
//     across the flights' states and the build-332841 bytes it patches.
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
//     and the production seed (ui_layer_seed.h): a jittered quad
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
//   * the after-UI take (uiLayerNoteOther's write case, ui_layer_math.h's
//     uiLayerAfterWriteDecide): a plain overlay drawn after the UI is
//     attempted and, routed the same way into the layer, composites over
//     the UI, matching the game's own order; an overlay sampling an
//     eye-sized input, or one the take path refuses (kMrt and kVerdict,
//     the same rules as any family), is left under the UI instead.
//   * the after-UI identity (ui_after_ui_test.h; 2026-09-30, the station menu's
//     frosted base): the identity's pure pieces (a write into it, a read of it,
//     the follow through the game's post pass) as a truth table; the RECORDED
//     post-tonemap tails of two field censuses (station services, and the
//     game's menu over it) routed through the real family rule, decide, gate
//     and follow -- no interface draw left under the layer, and exactly the
//     frosted bases left under it when routed as before the follow; the same
//     draw structures composited stock against layered on the CPU blend model
//     (equal with the follow, not without it); the known limit pinned (an
//     HDR-phase draw the layer does not take, after a taken HUD draw, still
//     sits under it); and a scan of ui_layer.cpp for the order the routing
//     model assumes (the follow before the once-per-pair note).
//   * the crisp-HUD parity model (docs/cockpit-hud-layer-design-2026-09-27.md,
//     design point 5): stock T(F(1-a) + L) against the layer's T(F)(1-a) +
//     T(L) on a synthetic frame through two stand-in tonemaps -- exact where
//     the HUD is opaque or the pixel uncovered, within the G-F budget over a
//     dark background, the translucent-over-bright cost nonzero and under
//     its measured 113-step ceiling, and failing with the take removed.
//
//   * the surface strip's wiring in vscreen.cpp (ui_intro_curve_wiring_test.h; docs/intro-video.md, job 3): source scans of the intro movie's
//     and the splash's strip, each with controls that edit a copy and must trip the pin; --wiring runs the source scans alone, which is how
//     tools\vr_world_route_test\mutants.py proves them on edited copies of the real file.
//
// Exit codes: 0 pass, 1 a check failed, 2 usage. --dry-run touches nothing.
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "../../src/common/system_d3d11.h"
#include "../../src/common/temporal_math.h"
#include "../../src/d3d11/orbital_width.h"  // the orbit lines' width decision, for the panel factor's Supersampling term
#include "../../src/d3d11/ui_layer_seed.h"
#include "../../src/d3d11/ui_layer_seed_census.h"
#include "../../src/d3d11/ui_layer_math.h"
#include "../../src/d3d11/ui_layer_shaders.h"
#include "../../src/d3d11/ui_quality_math.h"
#include "../../src/d3d11/ui_sizing_math.h"

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
    // off | 100 | 125: percent of HMD Quality 1.0, the target 1.0 / 1.25 inside.
    bool ok = false;
    const char* alias = "unset";
    check(uiQualityParse("off", &ok, &alias) == 0.0f && ok && !alias, "off is off");
    check(uiQualityParse("100", &ok, &alias) == 1.0f && ok && !alias, "100 is a target of 1.0");
    check(uiQualityParse("125", &ok, &alias) == 1.25f && ok && !alias, "125 is a target of 1.25");
    // The first spellings, read for one release, each naming its new one.
    check(uiQualityParse("1.0", &ok, &alias) == 1.0f && ok && alias && std::strcmp(alias, "100") == 0,
          "1.0 is read as 100, and says so");
    check(uiQualityParse("1", &ok, &alias) == 1.0f && ok && alias && std::strcmp(alias, "100") == 0,
          "1 is read as 100, and says so");
    check(uiQualityParse("1.25", &ok, &alias) == 1.25f && ok && alias && std::strcmp(alias, "125") == 0,
          "1.25 is read as 125, and says so");
    check(uiQualityParse("125", &ok) == 1.25f && ok, "the alias out-parameter is optional");
    // Anything else is off, and refused out loud.
    const char* garbage[] = {"100%", "125%", "100.0", "1.00", "Off", "on", "150", "0", "", " 125"};
    bool refused = true;
    for (const char* g : garbage) {
        alias = "unset";
        refused = refused && uiQualityParse(g, &ok, &alias) == 0.0f && !ok && !alias;
    }
    check(refused, "100%, 125%, 100.0, 1.00, Off, on, 150, 0, empty and a leading space are refused as off");
    check(uiQualityParse(nullptr, &ok) == 0.0f && !ok, "no text is off");
    // The log's and the menu's spelling.
    check(std::strcmp(uiQualityLabel(1.0f), "100%") == 0 && std::strcmp(uiQualityLabel(1.25f), "125%") == 0 &&
              std::strcmp(uiQualityLabel(0.0f), "off") == 0,
          "the label: 100%, 125%, off");
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
    check(with([](UiLayerDrawFacts& g) { g.eye = -1; }) == UiLayerDecision::kNoEye, "no eye");
    check(with([](UiLayerDrawFacts& g) { g.targetMatchesEye = false; }) == UiLayerDecision::kTargetSize,
          "a target that is not the submitted eye's size");
    check(with([](UiLayerDrawFacts& g) { g.late = true; }) == UiLayerDecision::kLate, "late (G1)");
    check(with([](UiLayerDrawFacts& g) { g.armed = false; }) == UiLayerDecision::kNotArmed, "not armed");
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

    // The crisp-HUD (HDR) take of the three cockpit HUD families (Phase 2:
    // the holo panels, the flight HUD and the target sprite): every admitted
    // blend shape must have its composition semantics end to end. A multiply
    // has no transmittance route into the HDR layer (the HDR take skips
    // ensureMult; the coverage pass transfers only scalar alpha), so the
    // gate refuses it BEFORE the redirect (review R5), per family; the
    // measured blends -- premultiplied-over and opaque -- must still be
    // taken, with each family's MEASURED depth-stencil state (the Phase 0
    // census, flights 1-3; the classification of those states is in
    // testDepthStencil).
    auto hdrWith = [&](UiLayerFamily fam, void (*edit)(UiLayerDrawFacts&)) {
        UiLayerDrawFacts g = f;
        g.family = fam;
        g.ldrView = false;  // the lit pre-tonemap HDR target
        g.crispHdr = true;
        edit(g);
        return uiLayerDecide(g);
    };
    check(hdrWith(UiLayerFamily::kHolo, [](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kPremulOver; }) ==
              UiLayerDecision::kRedirect,
          "the holo panels' premultiplied-over is taken into the HDR layer");
    check(hdrWith(UiLayerFamily::kHolo, [](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kOpaque; }) ==
              UiLayerDecision::kRedirect,
          "an opaque holo draw is taken into the HDR layer");
    check(hdrWith(UiLayerFamily::kFlightHud,
                  [](UiLayerDrawFacts& g) {
                      g.blend = UiBlendShape::kPremulOver;
                      g.ds.depthTest = true;  // the measured GEQUAL against the scene pair, no write
                  }) == UiLayerDecision::kRedirect,
          "the flight HUD's premultiplied-over, testing depth, is taken into the HDR layer");
    check(hdrWith(UiLayerFamily::kSprite,
                  [](UiLayerDrawFacts& g) {
                      g.blend = UiBlendShape::kPremulOver;
                      // The measured sprite: depth OFF (no test, no write),
                      // stencil tested (read 0x01) and written (0x05).
                      g.ds.stencilTest = true;
                      g.ds.stencilWrite = true;
                  }) == UiLayerDecision::kRedirect,
          "the target sprite, testing and writing stencil, is taken (the write-back keeps the "
          "game's buffer)");
    check(hdrWith(UiLayerFamily::kHolo, [](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kMultiply; }) ==
              UiLayerDecision::kBlendRefused,
          "a multiply into the HDR target is refused: the HDR half has no transmittance route");
    check(hdrWith(UiLayerFamily::kFlightHud,
                  [](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kMultiply; }) ==
              UiLayerDecision::kBlendRefused,
          "...refused for the flight HUD too (R5, per family)");
    check(hdrWith(UiLayerFamily::kSprite, [](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kMultiply; }) ==
              UiLayerDecision::kBlendRefused,
          "...and for the target sprite (R5, per family)");
    // The crisp publication deadline (review crisp-hud-phase3-2026-09-28, the
    // missing ship/target mesh holograms): content after the eye's tonemap
    // re-issue can never publish -- the layer clears next frame. Refuse to
    // stock; the fact is crisp-scoped, so an LDR draw with it set is unaffected.
    check(hdrWith(UiLayerFamily::kHolo,
                  [](UiLayerDrawFacts& g) {
                      g.blend = UiBlendShape::kPremulOver;
                      g.lateTone = true;
                  }) == UiLayerDecision::kToneLate,
          "a crisp draw after the eye's tonemap re-issue refuses to stock: it cannot publish");
    check(with([](UiLayerDrawFacts& g) { g.lateTone = true; }) == UiLayerDecision::kRedirect,
          "the tone-late fact is crisp-scoped: an LDR draw with it set is still taken");
    // Phase 3: the crisp take's eight hologram families, the one kHoloGeneric
    // family. Their
    // states are NOT flight-measured; the documented shape is an additive
    // glow with depth off (ui_depth.cpp's generic hologram coverage), and the
    // refusal net owns anything unconvertible, with the reason named.
    check(hdrWith(UiLayerFamily::kHoloGeneric, [](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kAdditive; }) ==
              UiLayerDecision::kRedirect,
          "a hologram's additive glow (ONE, ONE, depth off) is taken into the HDR layer");
    check(hdrWith(UiLayerFamily::kHoloGeneric,
                  [](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kScaledAdditive; }) ==
              UiLayerDecision::kRedirect,
          "...a scaled-additive one (SRC_ALPHA, ONE) too");
    check(hdrWith(UiLayerFamily::kHoloGeneric, [](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kMultiply; }) ==
              UiLayerDecision::kBlendRefused,
          "...and the R5 multiply refusal stands for the holograms");
    check(hdrWith(UiLayerFamily::kHoloGeneric,
                  [](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kRefused; }) ==
              UiLayerDecision::kBlendRefused,
          "...and an unconvertible hologram blend refuses to stock, named");
    check(hdrWith(UiLayerFamily::kHolo,
                  [](UiLayerDrawFacts& g) {
                      g.blend = UiBlendShape::kMultiply;
                      g.substituted = true;
                  }) == UiLayerDecision::kBlendRefused,
          "...through a substitution too");
    check(hdrWith(UiLayerFamily::kHolo,
                  [](UiLayerDrawFacts& g) {
                      g.blend = UiBlendShape::kMultiply;
                      g.layerReady = false;
                  }) == UiLayerDecision::kBlendRefused,
          "...and the refusal is the blend's, ahead of the layer's readiness");
    check(with([](UiLayerDrawFacts& g) {
              g.family = UiLayerFamily::kHolo;
              g.ldrView = false;
              g.blend = UiBlendShape::kMultiply;
          }) == UiLayerDecision::kHdrTarget,
          "with the crisp take off a holo multiply is still plain HDR, left stock");
    check(with([](UiLayerDrawFacts& g) {
              g.family = UiLayerFamily::kFlightHud;
              g.ldrView = false;
          }) == UiLayerDecision::kHdrTarget &&
              with([](UiLayerDrawFacts& g) {
                  g.family = UiLayerFamily::kSprite;
                  g.ldrView = false;
              }) == UiLayerDecision::kHdrTarget &&
              with([](UiLayerDrawFacts& g) {
                  g.family = UiLayerFamily::kHoloGeneric;
                  g.ldrView = false;
              }) == UiLayerDecision::kHdrTarget,
          "with the crisp take off the flight HUD, the sprite and the holograms stay plain HDR, "
          "left stock");

    // The on-foot gate: on foot the 2D screen IS the world (flight 09:38:
    // the layer took it and the temporal pass got a black eye). The journal's
    // pair (known, on foot), read once a frame, drives the fact.
    {
        auto screenWith = [&](bool gateHolds, UiLayerFamily fam = UiLayerFamily::kScreen) {
            UiLayerDrawFacts g = f;
            g.family = fam;
            g.worldScreen = gateHolds;
            return uiLayerDecide(g);
        };
        UiOnFootGate gate;
        check(gate.state == -1, "the gate starts unread");
        check(uiLayerOnFootStep(gate, true, true, 1000) &&
                  screenWith(gate.state == 1) == UiLayerDecision::kWorldScreen,
              "on foot: the 2D screen is not redirected");
        check(std::strcmp(uiLayerDecisionName(UiLayerDecision::kWorldScreen),
                          "the screen shows the world (on foot, or a 3D map); the temporal pass "
                          "keeps it") == 0,
              "...and the left-in-the-frame line gives the reason");
        check(screenWith(true, UiLayerFamily::kPanel) == UiLayerDecision::kRedirect &&
                  screenWith(true, UiLayerFamily::kLoader) == UiLayerDecision::kRedirect,
              "on foot, the menus and the loading screen are still taken");
        check(with([](UiLayerDrawFacts& g) {
                  g.worldScreen = true;
                  g.armed = false;
                  g.late = true;
              }) == UiLayerDecision::kWorldScreen,
              "the world screen is the reason whatever else holds");
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

        // The screen's own identity (review P1): the depth probe's count of
        // draws into the screen-sized depth target, needing no journal. The
        // held fact is (journal) OR (depth), as ui_layer.cpp combines them.
        auto heldBy = [](const UiOnFootGate& j, const UiWorldScreenGate& w) {
            return j.state == 1 || w.busy;
        };
        UiOnFootGate off;  // the journal watcher off: every reading unknown
        UiWorldScreenGate world;
        bool held = false;
        for (int i = 0; i < 2; ++i) {
            const bool j = uiLayerOnFootStep(off, false, false, 1000 + i * 11);
            held = j || uiLayerWorldScreenStep(world, true, 5505);  // flight 09:38's first count
        }
        check(held && heldBy(off, world) && off.state == 0 &&
                  screenWith(heldBy(off, world)) == UiLayerDecision::kWorldScreen,
              "journal off, the screen's depth busy (5505 draws a frame, on foot): left in the eye");
        UiOnFootGate menuJournal;
        UiWorldScreenGate menu;
        bool menuHeld = false;
        for (int i = 0; i < 200; ++i) {
            menuHeld = uiLayerOnFootStep(menuJournal, false, false, 1000 + i * 11) ||
                       uiLayerWorldScreenStep(menu, true, i % 2 ? 4 : 3);  // the UI's own 3 or 4
        }
        check(!menuHeld && screenWith(heldBy(menuJournal, menu)) == UiLayerDecision::kRedirect,
              "journal off, the screen's depth quiet (3 or 4 draws, a menu): taken, the menu stays sharp");
        UiWorldScreenGate unknownCount;
        check(!uiLayerWorldScreenStep(unknownCount, false, 9000) &&
                  !uiLayerWorldScreenStep(unknownCount, false, 9000) && unknownCount.draws == 0,
              "a count the probe did not take is no count (unknown journal, no count: taken)");
        UiWorldScreenGate h;
        check(!uiLayerWorldScreenStep(h, true, 15612) && !uiLayerWorldScreenStep(h, true, 4) &&
                  !uiLayerWorldScreenStep(h, true, 15612),
              "hysteresis: one busy transition frame, then a quiet one, does not hold it");
        check(uiLayerWorldScreenStep(h, true, 187) && h.busy,
              "...two frames running over 64 do (the census's on-foot minimum, 187)");
        bool stillHeld = true;
        for (uint32_t i = 0; i + 1 < kUiWorldLeaveFrames; ++i) stillHeld = stillHeld && uiLayerWorldScreenStep(h, true, 3);
        check(stillHeld, "...89 quiet frames running do not let go (a loading hitch on foot)");
        check(uiLayerWorldScreenStep(h, true, 40) && uiLayerWorldScreenStep(h, true, 3),
              "...a frame between the thresholds (40) restarts the quiet run");
        bool released = false;
        for (uint32_t i = 0; i < kUiWorldLeaveFrames && !released; ++i) released = !uiLayerWorldScreenStep(h, true, 3);
        check(released && !h.busy, "...a second running under 32 lets go");
        check(!uiLayerWorldScreenStep(h, true, 64) && !uiLayerWorldScreenStep(h, true, 64),
              "exactly the threshold (64) is not over it");
        UiOnFootGate journalOnFoot;
        UiWorldScreenGate quietOnFoot;
        check((uiLayerOnFootStep(journalOnFoot, true, true, 1000) ||
               uiLayerWorldScreenStep(quietOnFoot, true, 3)) &&
                  heldBy(journalOnFoot, quietOnFoot),
              "the journal on foot holds it whatever the depth says (either signal holds)");
        check(std::strcmp(uiGuiFocusName(6), "galaxy map") == 0 &&
                  std::strcmp(uiGuiFocusName(5), "station services") == 0 &&
                  std::strcmp(uiGuiFocusName(7), "system map") == 0 && uiGuiFocusName(12) == nullptr,
              "GuiFocus names for the gate's lines");
    }

    // The route's price (the review's "Findings and costs"): per-eye-frame
    // sums from samples read in the order they were issued, the eyes
    // interleaved (eye 1's UI runs before eye 0's composite).
    {
        UiRouteSum s0, s1;
        double out = -1.0;
        check(!uiRouteAdd(s0, 10, 0.20, true, &out) && !uiRouteAdd(s0, 10, 0.05, true, &out) &&
                  !uiRouteAdd(s1, 10, 0.30, true, &out) && !uiRouteAdd(s0, 10, 0.07, true, &out),
              "an eye's samples of one frame add up while the other eye keeps its own sum");
        check(uiRouteAdd(s0, 11, 0.10, true, &out) && std::fabs(out - 0.32) < 1e-9,
              "a later frame's sample closes the sum: 0.20 + 0.05 + the composite's 0.07");
        check(uiRouteLate(s0, 10) && !uiRouteAdd(s0, 10, 5.0, true, &out) && s0.seq == 11 &&
                  std::fabs(s0.ms - 0.10) < 1e-9,
              "a sample for a frame already closed is late and changes nothing");
        out = -1.0;
        check(!uiRouteAdd(s0, 11, 0.0, false, &out) && !uiRouteAdd(s0, 12, 0.10, true, &out) &&
                  out == -1.0,
              "a sample that did not measure drops its eye-frame rather than report it short");
        uiRouteLost(s0, 13);
        check(uiRouteAdd(s0, 13, 0.20, true, &out) && std::fabs(out - 0.10) < 1e-9 && s0.bad,
              "a timer never had for a later frame spoils that frame when it opens");
        out = -1.0;
        check(!uiRouteAdd(s0, 14, 0.30, true, &out) && out == -1.0, "...and it is dropped");
        check(!uiRouteClose(s0, 14, &out) && s0.open,
              "a sum stays open while a timer of its frame may still be read");
        check(uiRouteClose(s0, 15, &out) && std::fabs(out - 0.30) < 1e-9 && !s0.open,
              "...and closes once none can");
        uiRouteLost(s1, 10);
        check(!uiRouteClose(s1, 11, &out) && !s1.open, "a timer never had in the open frame spoils it");
        float v[5] = {5.0f, 1.0f, 4.0f, 2.0f, 3.0f};
        check(std::fabs(uiLayerPercentile(v, 5, 0.5) - 3.0) < 1e-6 &&
                  std::fabs(uiLayerPercentile(v, 5, 0.95) - 4.8) < 1e-6,
              "median 3, p95 4.8 of 1..5 (linear between order statistics)");
        float one[1] = {0.25f};
        check(std::fabs(uiLayerPercentile(one, 1, 0.95) - 0.25) < 1e-6 &&
                  uiLayerPercentile(nullptr, 0, 0.5) == 0.0,
              "one sample is its own percentile; none reads 0");
        float even[4] = {9.0f, 1.0f, 1.0f, 5.0f};
        const double evenP95 = uiLayerPercentile(even, 4, 0.95);
        check(std::fabs(evenP95 - 8.4) < 1e-9 && uiLayerSortedPercentile(even, 4, 0.5) == 3.0,
              "one sorted snapshot preserves even-count median and duplicate order statistics");
        float fullWindow[8192];
        for (uint32_t i = 0; i < 8192; ++i) fullWindow[i] = static_cast<float>((8191 - i) / 4);
        const double fullP95 = uiLayerPercentile(fullWindow, 8192, 0.95);
        check(fullP95 == 1945.0 && uiLayerSortedPercentile(fullWindow, 8192, 0.5) == 1023.5 &&
                  uiLayerSortedPercentile(nullptr, 0, 0.5) == 0.0 &&
                  uiLayerSortedPercentile(one, 1, 0.5) == 0.25,
              "full 8192-sample route window reuses its sort with unchanged interpolation");
        check(std::strcmp(uiRouteStageName(UiRouteStage::kSeed), "depth-stencil seed") == 0 &&
                  std::strcmp(uiRouteStageName(UiRouteStage::kComposite), "composite") == 0,
              "the price line's stage names");
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
// The confirmation instrument (ui_sizing_math.h; docs/ui-sizing-owner-2026-
// 09-23.md section 8): a create's chain -- the game's frames, innermost
// first -- read into a verdict, on synthetic chains.
void testChains() {
    const uintptr_t base = 0x140000000ull, size = 104894464ull;  // build 332841's image
    auto chainOf = [&](std::initializer_list<uint32_t> rvas, bool withForeign) {
        uintptr_t frames[40] = {};
        uint32_t n = 0;
        if (withForeign) {
            frames[n++] = 0x7FFA12340000ull;  // EDVR's own hook frame
            frames[n++] = 0x7FFA56780000ull;  // the D3D runtime
        }
        for (uint32_t r : rvas) frames[n++] = base + r;
        UiChain c;
        uiChainFromFrames(frames, n, base, size, &c);
        return c;
    };
    // The chain section 8 expects, innermost first.
    const UiChain initColour = chainOf({0x51AF6B, 0x50EC69, 0x5185B3, 0x28148A9, 0x2833883, 0x4551C7D,
                                        0x45517F6, 0x4570902, 0x456D910, 0x10},
                                       true);
    check(initColour.n == 10 && initColour.rva[0] == 0x51AF6B && initColour.rva[5] == 0x4551C7D,
          "a chain keeps the game's frames, innermost first, and skips EDVR's and the runtime's");
    UiChainVerdict v = uiChainVerdict(initColour);
    check(v.kind == UiChainKind::kRtt && v.init && !v.change && v.colour && !v.depth &&
              std::strcmp(uiChainVerdictShort(v), "rtt init colour") == 0,
          "0x4551C7D and 0x45517F6, through 0x4570902 and 0x28148A9: rtt init colour");
    char text[320];
    uiChainVerdictText(v, text, sizeof(text));
    check(std::strcmp(text, "rtt init colour (0x4551C7D yes, 0x45517F6 yes, 0x4570902 yes, 0x456DA45 no, "
                            "0x28148A9 yes, 0x2814686 no, 0x30FC1A no, 0x312450 no)") == 0,
          "...and the verdict names every key's hit and miss");
    char rvas[160];
    uiChainFormat(initColour, rvas, sizeof(rvas));
    check(std::strcmp(rvas, "0x51AF6B/0x50EC69/0x5185B3/0x28148A9/0x2833883/0x4551C7D/0x45517F6/"
                            "0x4570902/0x456D910/0x10") == 0,
          "the chain formatted innermost first");
    const UiChain changeDepth = chainOf({0x51AF6B, 0x50EC69, 0x5185B3, 0x2814686, 0x28338B0, 0x4551C7D,
                                         0x45517F6, 0x456DA45},
                                        false);
    v = uiChainVerdict(changeDepth);
    check(v.kind == UiChainKind::kRtt && v.change && v.depth &&
              std::strcmp(uiChainVerdictShort(v), "rtt change depth") == 0,
          "through 0x456DA45 and 0x2814686: rtt change depth");
    v = uiChainVerdict(chainOf({0x51AF6B, 0x4551C7D, 0x4570902}, false));
    check(v.kind == UiChainKind::kOther && (v.hits & 1u) && !(v.hits & 2u),
          "0x4551C7D without 0x45517F6 is other (a miss, and it says which)");
    check(uiChainVerdict(chainOf({0x30C0AA, 0x30FC1A, 0x30F9E0}, true)).kind == UiChainKind::kGlyphCache &&
              uiChainVerdict(chainOf({0x312450}, false)).kind == UiChainKind::kGlyphCache,
          "the raster cache's texture maker, init 0x30FC1A or re-create 0x312450: glyph-cache");
    const UiChain none = chainOf({0x1000, 0x2000}, true);
    v = uiChainVerdict(none);
    uiChainVerdictText(v, text, sizeof(text));
    check(v.kind == UiChainKind::kOther && v.hits == 0 &&
              std::strcmp(text, "other (0x4551C7D no, 0x45517F6 no, 0x4570902 no, 0x456DA45 no, 0x28148A9 no, "
                                "0x2814686 no, 0x30FC1A no, 0x312450 no)") == 0,
          "neither: other, every key a miss");
    const UiChain deep = chainOf({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0x4551C7D, 0x45517F6}, false);
    check(deep.n == 12 && deep.rva[11] == 12 && uiChainVerdict(deep).kind == UiChainKind::kOther,
          "twelve game frames kept: keys past the twelfth are not seen");
    UiChain empty;
    uiChainFormat(empty, rvas, sizeof(rvas));
    check(rvas[0] == '\0' && uiChainVerdict(empty).kind == UiChainKind::kOther, "an empty chain");
    // k and the implied stage: the menu's 16:9 surface reads 1920x1080 on
    // every frustum it was seen on (the doc's section 6).
    const double kUntrimmed = uiSizingK(uiQualityFovTangent(1.2648f, 1.2648f));
    const double kTrimmed = uiSizingK(uiQualityFovTangent(1.1779f, 1.1779f));
    check(std::fabs(kUntrimmed - 0.7853) < 5e-4 && std::fabs(kTrimmed - 0.8433) < 5e-4,
          "k = tan(0.782)/tan(vFOV/2): 0.7853 untrimmed, 0.8433 trimmed 7/5/2");
    check(std::fabs(uiImpliedStage(1566, 1995, kUntrimmed) / 1920.0 - 1.0) < 0.003 &&
              std::fabs(uiImpliedStage(880, 1995, kUntrimmed) / 1080.0 - 1.0) < 0.003 &&
              std::fabs(uiImpliedStage(1254, 1597, kUntrimmed) / 1920.0 - 1.0) < 0.003 &&
              std::fabs(uiImpliedStage(1346, 1597, kTrimmed) / 1920.0 - 1.0) < 0.003,
          "the menu's 1566x880, 1254x705 (old k) and 1346x757 (new k) imply the same 1920x1080 stage");
    check(uiImpliedStage(100, 0, kUntrimmed) == 0.0 && uiSizingK(0.0f) == 0.0, "no W or no FOV: no stage");
}

// The engine-side panel sizing (ui_sizing_math.h, ui_panel_scale.h): the
// factor across the flights' states, the cap and the floor, and the four
// operand sites' bytes -- the embedded build-332841 patterns against the
// shape, a synthetic scan, the rel32 arithmetic, and, when the game is
// installed at the usual place and is that build, the executable itself
// (read-only; any other build is skipped, as the patch stands down on it).
double panelFactor(uint32_t askW, float hmd, float up, float down, uint32_t outW, float trueUp, float trueDown,
                   float target, UiPanelClamp* clamp = nullptr) {
    UiPanelInputs in;
    in.renderW = uiQualityInternalDim(askW, hmd);
    in.fovTangent = uiQualityFovTangent(up, down);
    in.outputW = outW;
    in.trueTangent = uiQualityFovTangent(trueUp, trueDown);
    in.target = target;
    in.supersampling = 1.0f;  // these flights ran at Supersampling 1.0; panelbudget:: (ui_panel_budget_test.h) holds the rest
    double f = 0.0;
    return uiPanelFactor(in, &f, clamp) ? f : -1.0;
}

struct PeFile {
    std::vector<uint8_t> data;
    uint32_t stamp = 0, imageSize = 0;
    struct Section {
        char name[9] = {};
        uint32_t va = 0, vsize = 0, raw = 0, rawSize = 0;
    };
    std::vector<Section> sections;
    const uint8_t* at(uint32_t rva, uint32_t n) const {
        for (const Section& s : sections) {
            if (rva >= s.va && rva - s.va + n <= (s.rawSize > s.vsize ? s.rawSize : s.vsize) &&
                s.raw + (rva - s.va) + n <= data.size())
                return data.data() + s.raw + (rva - s.va);
        }
        return nullptr;
    }
};

bool readPe(const char* path, PeFile* pe) {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") != 0 || !file) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0x400) {
        std::fclose(file);
        return false;
    }
    pe->data.resize(static_cast<size_t>(size));
    const size_t got = std::fread(pe->data.data(), 1, pe->data.size(), file);
    std::fclose(file);
    if (got != pe->data.size()) return false;
    const uint8_t* d = pe->data.data();
    uint32_t lfanew = 0;
    std::memcpy(&lfanew, d + 0x3C, 4);
    if (lfanew + 0x108 > pe->data.size() || std::memcmp(d + lfanew, "PE\0\0", 4) != 0) return false;
    const uint32_t coff = lfanew + 4, opt = coff + 20;
    uint16_t sections = 0, optSize = 0;
    std::memcpy(&sections, d + coff + 2, 2);
    std::memcpy(&pe->stamp, d + coff + 4, 4);
    std::memcpy(&optSize, d + coff + 16, 2);
    std::memcpy(&pe->imageSize, d + opt + 56, 4);
    for (uint16_t i = 0; i < sections; ++i) {
        const uint8_t* s = d + opt + optSize + 40u * i;
        if (static_cast<size_t>(s - d) + 40 > pe->data.size()) return false;
        PeFile::Section sec;
        std::memcpy(sec.name, s, 8);
        std::memcpy(&sec.vsize, s + 8, 4);
        std::memcpy(&sec.va, s + 12, 4);
        std::memcpy(&sec.rawSize, s + 16, 4);
        std::memcpy(&sec.raw, s + 20, 4);
        pe->sections.push_back(sec);
    }
    return true;
}

void testPanelScale() {
    // The factor: f = (W_ui x k) / (W_out x k_out) / T, the panel s / f.
    const float pimax = 1.2648f, trim = 1.1779f;
    double f = panelFactor(3070, 0.65f, pimax, pimax, 3070, pimax, pimax, 1.0f);
    check(std::fabs(f - 1995.0 / 3070.0) < 1e-6,
          "untrimmed at 0.65 -> 1.0: f = 1995/3070, the panels x1.539 (their HMD Quality 1.0 size)");
    f = panelFactor(3070, 0.65f, pimax, pimax, 3070, pimax, pimax, 1.25f);
    check(std::fabs(1.0 / f - 1.25 * 3070.0 / 1995.0) < 1e-4, "...-> 1.25: x1.924");
    UiPanelClamp clamp = UiPanelClamp::kNone;
    f = panelFactor(2458, 0.65f, trim, trim, 3070, pimax, pimax, 1.25f, &clamp);
    const double cTrim = 1597.0 * uiSizingK(uiQualityFovTangent(trim, trim));
    const double cOut = 3070.0 * uiSizingK(uiQualityFovTangent(pimax, pimax));
    check(std::fabs(f - cTrim / cOut / 1.25) < 1e-6 && std::fabs(1.0 / f - 2.2375) < 0.002 &&
              clamp == UiPanelClamp::kNone,
          "trimmed 7/5/2 at 0.65 -> 1.25: f = (1597 x 0.8433) / (3070 x 0.7853) / 1.25, x2.24");
    float d1080 = 0.0f, d1920 = 0.0f;
    uiPanelDivisors(f, &d1080, &d1920);
    const uint32_t menuW = uiPanelSize(1920, cTrim, d1920), menuH = uiPanelSize(1080, cTrim, d1920);
    check(std::abs(static_cast<int>(menuW) - static_cast<int>(1.25 * cOut)) <= 2 &&
              std::abs(static_cast<int>(menuH) - static_cast<int>(1.25 * cOut * 1080.0 / 1920.0)) <= 2,
          "...the trimmed menu's 16:9 screen comes out 3013x1695: its untrimmed size at 1.25, not 1346x757");
    check(uiPanelSize(1920, cTrim, 1920.0f) == 1346,
          "(the game's own divisor makes the 1346 the flight logged)");
    f = panelFactor(2458, 0.65f, trim, trim, 3070, pimax, pimax, 1.0f);
    check(std::fabs(1.0 / f - 1.790) < 0.003, "trimmed at 0.65 -> 1.0: x1.79");
    // Quest 3 shapes (up 0.9657, down 1.4281): untrimmed, and trimmed 2/2/7.
    f = panelFactor(3072, 0.65f, 0.9657f, 1.4281f, 3072, 0.9657f, 1.4281f, 1.25f);
    check(std::fabs(f - 1996.0 / 3072.0 / 1.25) < 1e-6, "Quest 3 untrimmed at 0.65 -> 1.25: f = 1996/3072/1.25");
    f = panelFactor(2662, 0.65f, 0.7536f, 1.1106f, 3072, 0.9657f, 1.4281f, 1.25f);
    const double cQ = 1730.0 * uiSizingK(uiQualityFovTangent(0.7536f, 1.1106f));
    const double cQout = 3072.0 * uiSizingK(uiQualityFovTangent(0.9657f, 1.4281f));
    check(std::fabs(f - cQ / cQout / 1.25) < 1e-6,
          "Quest 3 trimmed 2/2/7: k from the narrower frustum it is told, k_out from the headset's");
    // The cap and the floor.
    f = panelFactor(3070, 0.2f, pimax, pimax, 3070, pimax, pimax, 1.25f, &clamp);
    check(std::fabs(f - kUiPanelObservedBase / kUiPanelBudget) < 1e-9 && clamp == UiPanelClamp::kBudget,
          "HMD Quality 0.2 -> 1.25 would be x6.25: the 4x cap's 0.25 would ask for a 15360 px panel, over the size budget, so f is raised to x3.73");
    f = panelFactor(3070, 0.2f, pimax, pimax, 3070, pimax, pimax, 1.0f, &clamp);
    check(std::fabs(f - kUiPanelObservedBase / kUiPanelBudget) < 1e-9 && clamp == UiPanelClamp::kBudget,
          "...and at 100: the budget's floor (x3.73) is above the 4x cap's, so it is the budget that holds, never the 4x cap alone");
    f = panelFactor(3070, 1.5f, pimax, pimax, 3070, pimax, pimax, 1.25f, &clamp);
    check(f == 1.0 && clamp == UiPanelClamp::kFloor, "HMD Quality 1.5 above 1.25: 1, never smaller than the game's");
    f = panelFactor(2458, 1.25f, trim, trim, 3070, pimax, pimax, 1.25f, &clamp);
    check(f < 0.87 && clamp == UiPanelClamp::kNone,
          "at 1.25 with the trim on the panels still grow back to their untrimmed size");
    check(panelFactor(3070, 0.0f, pimax, pimax, 3070, pimax, pimax, 1.25f) < 0.0 &&
              panelFactor(3070, 0.65f, pimax, pimax, 0, pimax, pimax, 1.25f) < 0.0 &&
              panelFactor(3070, 0.65f, pimax, pimax, 3070, 0.0f, 0.0f, 1.25f) < 0.0 &&
              panelFactor(3070, 0.65f, pimax, pimax, 3070, pimax, pimax, 0.0f) < 0.0,
          "an unknown input: no factor (the floats stay as they are)");
    uiPanelDivisors(1.0, &d1080, &d1920);
    check(d1080 == 1080.0f && d1920 == 1920.0f, "f = 1 is the game's own 1080 and 1920, bit for bit");

    // The bytes. The embedded patterns are the shape, and read the constants.
    for (int i = 0; i < 2; ++i) {
        const UiPanelTargets t = uiPanelTargets(kUiPanelSiteBytes[i], kUiPanelSiteRva[i]);
        check(uiPanelShapeAt(kUiPanelSiteBytes[i]) && t.aspect == kUiPanelAspectRva && t.d1080 == kUiPanel1080Rva &&
                  t.d1920 == kUiPanel1920Rva,
              i == 0 ? "the init site's bytes are the shape, reading 16/9, 1080 and 1920"
                     : "the view-change site's bytes are the shape, reading 16/9, 1080 and 1920");
    }
    check(kUiPanelSiteRva[0] + kUiPanel1080Disp == 0x4570707 && kUiPanelSiteRva[0] + kUiPanel1920Disp == 0x4570714 &&
              kUiPanelSiteRva[1] + kUiPanel1080Disp == 0x4571212 && kUiPanelSiteRva[1] + kUiPanel1920Disp == 0x457121F,
          "the four operands are at 0x4570707, 0x4570714, 0x4571212, 0x457121F (the trace's section 3)");
    std::vector<uint8_t> text(4096, 0x90);
    std::memcpy(text.data() + 100, kUiPanelSiteBytes[0], 30);
    std::memcpy(text.data() + 2000, kUiPanelSiteBytes[1], 30);
    std::vector<uint8_t> decoy(kUiPanelSiteBytes[0], kUiPanelSiteBytes[0] + 30);
    decoy[20] = 0x29;  // movaps xmm6,xmm1 -> another register: not the shape
    std::memcpy(text.data() + 3000, decoy.data(), 30);
    uint32_t found[4] = {};
    check(uiPanelScan(text.data(), text.size(), 0x1000, found, 4) == 2 && found[0] == 0x1000 + 100 &&
              found[1] == 0x1000 + 2000,
          "the scan finds the two shapes and not the decoy one byte off");
    int32_t disp = 0;
    check(uiPanelDisp(0x14457070Bull, 0x14457070Bull + 0x86DF55ull, &disp) && disp == 0x86DF55 &&
              uiPanelDisp(0x140000000ull, 0x13FFF0000ull, &disp) && disp == -0x10000 &&
              !uiPanelDisp(0x140000000ull, 0x240000000ull, &disp),
          "rel32 to a page near the image, and refused out of range");
    check(uiPanelDisp(0x14457070Bull, 0x144DDE660ull, &disp) && disp == uiReadDisp(kUiPanelSiteBytes[0] + 13),
          "the game's own 1080 operand is exactly that arithmetic");

    // The executable, when it is here and is build 332841.
    static const char* kExe =
        "C:\\Users\\seanm\\AppData\\Local\\Frontier_Developments\\Products\\elite-dangerous-odyssey-64\\"
        "EliteDangerous64.exe";
    PeFile pe;
    if (!readPe(kExe, &pe)) {
        std::printf("  (the game executable is not at the usual place: the embedded patterns stand)\n");
        return;
    }
    if (pe.stamp != kUiPanelStamp || pe.imageSize != kUiPanelImageSize) {
        std::printf("  (the installed game is not build 332841 -- stamp %u: the patch stands down on it)\n",
                    pe.stamp);
        return;
    }
    bool sitesOk = true;
    for (int i = 0; i < 2; ++i) {
        const uint8_t* p = pe.at(kUiPanelSiteRva[i], 30);
        sitesOk = sitesOk && p && std::memcmp(p, kUiPanelSiteBytes[i], 30) == 0;
    }
    const uint8_t* p0 = pe.at(kUiPanelFuncRva[0], sizeof(kUiPanelProlog0));
    const uint8_t* p1 = pe.at(kUiPanelFuncRva[1], sizeof(kUiPanelProlog1));
    check(sitesOk && p0 && p1 && std::memcmp(p0, kUiPanelProlog0, sizeof(kUiPanelProlog0)) == 0 &&
              std::memcmp(p1, kUiPanelProlog1, sizeof(kUiPanelProlog1)) == 0,
          "build 332841 on disk: both sites' 30 bytes and both prologues are the embedded ones");
    float aspect = 0.0f, c1080 = 0.0f, c1920 = 0.0f;
    const uint8_t* a = pe.at(kUiPanelAspectRva, 4);
    const uint8_t* b = pe.at(kUiPanel1080Rva, 4);
    const uint8_t* c = pe.at(kUiPanel1920Rva, 4);
    if (a) std::memcpy(&aspect, a, 4);
    if (b) std::memcpy(&c1080, b, 4);
    if (c) std::memcpy(&c1920, c, 4);
    check(aspect == 16.0f / 9.0f && c1080 == 1080.0f && c1920 == 1920.0f,
          "...the constants they read are 16/9, 1080 and 1920");
    uint32_t count = 0, at[4] = {};
    for (const PeFile::Section& s : pe.sections) {
        if (std::strncmp(s.name, ".text", 5) != 0) continue;
        const uint8_t* t = pe.at(s.va, s.rawSize < s.vsize ? s.rawSize : s.vsize);
        if (t) count = uiPanelScan(t, s.rawSize < s.vsize ? s.rawSize : s.vsize, s.va, at, 4);
    }
    check(count == 2 && at[0] == kUiPanelSiteRva[0] && at[1] == kUiPanelSiteRva[1],
          "...and the shape occurs exactly twice in .text, at the two sites");
}

// The flat profile's factor (ui_sizing_math.h's uiFlatPanelPlanFor, 2026-10-09): f = (R / D) / T on the axis the game
// divides, the panels at D x T whatever the Supersampling. The 09:36 Epic flight's numbers: D 3840x2160, an rtt-init panel
// 1920x960 at R 3840x2160 and 960x480 at R 1920x1080 -- the same 960x480 stage at s = R_h / 1080.
void testFlatPanelScale() {
    auto plan = [](uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, float t, UiPanelPlan* p) {
        UiFlatPanelInputs in;
        in.renderW = rw;
        in.renderH = rh;
        in.outputW = dw;
        in.outputH = dh;
        in.target = t;
        return uiFlatPanelPlanFor(in, p);
    };
    UiPanelPlan p;
    // The flight's state: 0.5 Supersampling on a 4K screen.
    check(plan(1920, 1080, 3840, 2160, 1.0f, &p) == UiFlatPanelRefuse::kNone && std::fabs(p.f - 0.5) < 1e-12 &&
              p.clamp == UiPanelClamp::kNone && !p.budgetActs && p.ss == 1.0,
          "flat R 1920x1080 on D 3840x2160 at 100: f = 0.5 (the panels x2), no Supersampling term, no clamp");
    float d1080 = 0.0f, d1920 = 0.0f;
    uiPanelDivisors(p.f, &d1080, &d1920);
    check(d1080 == 540.0f && d1920 == 960.0f, "...the operands read 540 and 960");
    check(uiFlatPanelSizeHeightAxis(960, 1080, 1080.0f) == 960 && uiFlatPanelSizeHeightAxis(480, 1080, 1080.0f) == 480,
          "the flight's stage 960x480 at R 1920x1080 and the game's own divisor is the logged 960x480");
    check(uiFlatPanelSizeHeightAxis(960, 2160, 1080.0f) == 1920 && uiFlatPanelSizeHeightAxis(480, 2160, 1080.0f) == 960,
          "...and at R 3840x2160 the logged 1920x960: the panels follow R, which is what the factor divides out");
    check(uiFlatPanelSizeHeightAxis(960, 1080, d1080) == 1920 && uiFlatPanelSizeHeightAxis(480, 1080, d1080) == 960,
          "...so at R 1920x1080 with f 0.5 the panel is the display's 1920x960");
    check(std::fabs(p.largest - 1920.0 / 0.5) < 1e-9, "the widest panel the formula can ask for is the 1920 stage at D: 3840 px");
    check(plan(1920, 1080, 3840, 2160, 1.25f, &p) == UiFlatPanelRefuse::kNone && std::fabs(p.f - 0.4) < 1e-12 &&
              p.clamp == UiPanelClamp::kNone && std::fabs(p.largest - 4800.0) < 1e-9,
          "...at 125: f = 0.4 (x2.5), the widest 4800 px");
    uiPanelDivisors(p.f, &d1080, &d1920);
    check(uiFlatPanelSizeHeightAxis(960, 1080, d1080) == 2400 && uiFlatPanelSizeHeightAxis(480, 1080, d1080) == 1200,
          "...the 960x480 stage comes out 2400x1200: the display's size x 1.25");
    // Supersampling 1 and above: at or above D x T the game's own panels stand.
    check(plan(3840, 2160, 3840, 2160, 1.0f, &p) == UiFlatPanelRefuse::kNone && p.f == 1.0,
          "R = D at 100: f = 1, the game's own operands");
    check(plan(3840, 2160, 3840, 2160, 1.25f, &p) == UiFlatPanelRefuse::kNone && std::fabs(p.f - 0.8) < 1e-12,
          "R = D at 125: f = 0.8 (x1.25)");
    check(plan(5760, 3240, 3840, 2160, 1.25f, &p) == UiFlatPanelRefuse::kNone && p.f == 1.0 && p.clamp == UiPanelClamp::kFloor,
          "Supersampling 1.5 at 125: the game's panels are already 1.5x D, f = 1 (the floor): never smaller than the game makes them");
    // Other shapes: the axis the game divides on.
    check(uiFlatPanelHeightAxis(1920, 1080) && uiFlatPanelHeightAxis(3440, 1440) && !uiFlatPanelHeightAxis(2560, 1600),
          "the height (1080) axis at 16:9 and wider, the width (1920) axis below 16:9");
    check(plan(1280, 800, 2560, 1600, 1.0f, &p) == UiFlatPanelRefuse::kNone && std::fabs(p.f - 0.5) < 1e-12 &&
              std::fabs(p.base - 1280.0) < 1e-9,
          "16:10 at half: f 0.5 on the width axis, the base the render width");
    check(plan(1720, 720, 3440, 1440, 1.0f, &p) == UiFlatPanelRefuse::kNone && std::fabs(p.f - 0.5) < 1e-12 &&
              std::fabs(p.base - 1280.0) < 1e-9,
          "21:9 at half: f 0.5 on the height axis, the base the 1920 stage at R's height (1280 px)");
    check(plan(1919, 1080, 3840, 2160, 1.0f, &p) == UiFlatPanelRefuse::kNone,
          "a pixel of rounding in R is the same shape");
    // The cap and the budget: an 8K screen at a quarter render.
    check(plan(1920, 1080, 7680, 4320, 1.25f, &p) == UiFlatPanelRefuse::kNone && p.f == 0.25 && p.clamp == UiPanelClamp::kCap &&
              p.largest <= kUiPanelBudget,
          "R a quarter of an 8K D at 125 would be x5: capped at x4, the widest 7680 px, inside the budget");
    // Refusals: no factor, the floats hold.
    check(plan(512, 512, 3840, 2160, 1.0f, &p) == UiFlatPanelRefuse::kAspect,
          "the flight's 512x512 frames (a render unlike the screen's shape) make no factor");
    check(plan(0, 0, 3840, 2160, 1.0f, &p) == UiFlatPanelRefuse::kUnknown &&
              plan(1920, 1080, 0, 0, 1.0f, &p) == UiFlatPanelRefuse::kUnknown &&
              plan(1920, 1080, 3840, 2160, 0.0f, &p) == UiFlatPanelRefuse::kUnknown,
          "an unknown size or the key off: no factor");
    // The setter thunk's move: R follows the Supersampling, so the plan scales with it.
    UiPanelPlan base;
    plan(1920, 1080, 3840, 2160, 1.0f, &base);
    check(uiFlatPanelMove(base.formula, base.base, 0.5f, 1.0f, &p) && p.f == 1.0,
          "the thunk: Supersampling 0.5 -> 1.0 moves f 0.5 -> 1 before R follows");
    check(uiFlatPanelMove(base.formula, base.base, 0.5f, 0.75f, &p) && std::fabs(p.f - 0.75) < 1e-12,
          "...0.5 -> 0.75: f 0.75, R's own 2880x1620 over D");
    UiPanelPlan direct;
    plan(2880, 1620, 3840, 2160, 1.0f, &direct);
    check(std::fabs(direct.f - p.f) < 1e-12, "...the same f the frame boundary makes once R is 2880x1620 (so it writes nothing more)");
    check(!uiFlatPanelMove(base.formula, base.base, 0.0f, 1.0f, &p), "...and no move without the value the plan was made at");
}

// The family rule (ui_layer_math.h's uiLayerFamilyFor, which vscreen.cpp's
// uiLayerFamilyOf feeds): the 13:23 flight lost the menu panel when the FOV
// trim, adopted at the main menu, had the game re-create its interface
// surfaces -- a learned surface is no longer what the panel samples, and the
// panel stays the menu panel by its shader pair.
void testFamilyRule() {
    UiFamilyFacts f;
    f.targetKind = 2;
    f.vs = kUiVsPanel;
    f.ps = 0x9107E72CB016CC02ull;
    UiFamilyWhy why = UiFamilyWhy::kOther;
    f.learnedSurface = true;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kPanel && why == UiFamilyWhy::kLearnedSurface,
          "the menu panel over a learned surface: the menu panel");
    f.learnedSurface = false;  // the surface the game re-created, not learned yet
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kPanel && why == UiFamilyWhy::kShaderPair,
          "...over a re-created surface nothing has learned: still the menu panel, by its shader pair");
    f.ps = 0x219323C8C025AD94ull;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kPanel, "...its variant pixel shader too");
    f.ps = 0x015EF9349EC097E8ull;  // the tinted in-flight variant (ui_depth.cpp:105-115)
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kPanel && why == UiFamilyWhy::kShaderPair,
          "the tinted in-flight variant is the menu panel too (the 2026-09-27 flight: never recognized, never taken)");
    f.ps = 0xF2F872B191F656D5ull;  // and the cheap one
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kPanel && why == UiFamilyWhy::kShaderPair,
          "the cheap variant too");
    f.ps = 0xDEADBEEFCAFEF00Dull;  // a pixel shader that is no variant of the pair
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNoSurface,
          "the same vertex shader with another pixel shader and no learned surface: not UI, and said why");
    f.vs = kUiVsLoader;
    f.ps = 0x85565E9261812E2Full;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kLoader && why == UiFamilyWhy::kShaderPair,
          "the loading screen by its pair; its gamma pass too");
    f.ps = 0x8ADB2A81A45E8A4Bull;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kLoader, "...the gamma pass (a multiply)");
    f.targetKind = 0;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotEyeTarget,
          "not an eye target: none, whatever the shaders");
    f.targetKind = 1;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotPostTonemap,
          "the lit HDR target: not the composite's");
    f.vs = kUiVsHolo;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kHolo, "...where the holo panels are named");
    // The same panels with Elite's Disable GUI effects on are drawn with another vertex shader
    // (vs 1989E6D3B405FDE0 ps EAB8A1C95A13FFBE, holo_material.h; docs/ui-layer-2026-09-23.md, 2026-10-01):
    // named exactly as the stock pair is, and a hash one bit away names nothing.
    f.vs = kUiVsHoloGuiFxOff;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kHolo && why == UiFamilyWhy::kDirect,
          "...and the panels with Disable GUI effects on (vs 1989E6D3B405FDE0), a direct shader like the stock pair's");
    f.vs = kUiVsHoloGuiFxOff ^ 1ull;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotPostTonemap,
          "...a vertex shader one bit from that names nothing");
    f.vs = kUiVsFlightHud;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kFlightHud && why == UiFamilyWhy::kDirect,
          "...the flight HUD is named, a direct shader");
    f.vs = kUiVsSprite;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kSprite, "...and the target sprite");
    // Phase 3, corrected by the phase-3 review (R1/R2): the crisp take
    // admits the eight proven-safe radar/icon families
    // (holo_families.h's kHoloFamiliesTake); each names the one kHoloGeneric
    // family on the lit HDR target. The depth pass's two refused families
    // name nothing here and stay stock: the target sphere (its PSes
    // integer-Load scene depth at SV_Position pixel coordinates, which the
    // layer's larger viewport breaks -- R1), the corona family (one shader
    // pair paints both the radar glow and the real sun's corona -- R2).
    // World-marker brackets and canopy stay outside the crisp take.
    const uint64_t kTakeVs[] = {kHoloIconCore,   kHoloIconStalkA, kHoloIconStalkB,
                                kHoloContactA,   kHoloContactB,   kHoloContactC,
                                kHoloContactD,   kHoloContactE};
    for (uint64_t h : kTakeVs) {
        f.vs = h;
        char what[128];
        std::snprintf(what, sizeof(what), "vs %016llX names the hologram family on the HDR target",
                      static_cast<unsigned long long>(h));
        check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kHoloGeneric && why == UiFamilyWhy::kDirect, what);
    }
    f.vs = kHoloTargetSphere;
    for (uint64_t ps : {0xEA02FAC2BD6C643Cull, 0xE95634B0F61D218Full}) {
        f.ps = ps;
        check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kHoloGeneric && why == UiFamilyWhy::kDirect,
              "exact model composite pair is considered for depth-address remap");
    }
    f.ps = 0;
    const uint64_t kRefusedVs[] = {kHoloTargetSphere, kHoloCoronaFamily};
    for (uint64_t h : kRefusedVs) {
        f.vs = h;
        char what[128];
        std::snprintf(what, sizeof(what),
                      "vs %016llX names no crisp family: the take refuses it, it stays stock",
                      static_cast<unsigned long long>(h));
        check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotPostTonemap,
              what);
    }
    f.vs = kHoloCanopy;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotPostTonemap,
          "the canopy is refused: not one of the take's eight, it sits in front of the whole sky");
    f.vs = kHoloWorldMarkerReticle;
    f.ps = 0x2D037A047171BF3Bull;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotPostTonemap,
          "world-space bracket VS/PS pair stays outside the crisp HUD layer");
    f.ps ^= 1;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kNone, "an unknown reticle PS stays stock");
    f.ps = 0;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kNone, "a missing reticle PS stays stock");
    f.ps = 0x2D037A047171BF3Bull;
    f.targetKind = 0;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kNone, "the reticle pair on a non-eye stays stock");
    f.targetKind = 2;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kNone, "world-space brackets stay stock on the post-tone eye too");
    f.vs = kUiVsPanel;
    f.ps = 0x9107E72CB016CC02ull;
    f.excluded = true;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kExcluded,
          "the exclude list wins over the pair");
    f.excluded = false;
    f.panelSized = true;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kScreen, "the 2D screen's SRV names the 2D screen first");
    UiFamilyFacts g;
    g.targetKind = 2;
    g.vs = kUiVsGuiText;
    check(uiLayerFamilyFor(g) == UiLayerFamily::kGuiDirect, "a GUI draw straight into the eye");
    g.vs = 0x1234;
    g.learnedSurface = true;
    check(uiLayerFamilyFor(g) == UiLayerFamily::kSurface, "any other learned-surface composite");
    g.learnedSurface = false;
    check(uiLayerFamilyFor(g, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kOther,
          "anything else: none");
    // A holo panel over a learned surface on the post-tonemap eye is a holo panel, whichever of the two
    // vertex shaders the setting Disable GUI effects gave it.
    g.learnedSurface = true;
    for (uint64_t vs : {kUiVsHolo, kUiVsHoloGuiFxOff}) {
        g.vs = vs;
        check(uiLayerFamilyFor(g, &why) == UiLayerFamily::kHolo && why == UiFamilyWhy::kLearnedSurface,
              "a holo panel over a learned surface on the post-tonemap eye, either vertex shader");
    }
}

// The orbit lines and the supercruise bars in the HDR layer (2026-10-07; docs/ui-layer-2026-09-23.md, "2026-10-07: orbit lines,
// supercruise bars and space dust in the layer"; supercruise_lines.h): scene geometry the layer takes for DENSITY. Their two families are
// named by a shader PAIR on the lit HDR target alone, are the crisp take's (uiLayerFamilyTakesHdr, the one list), are declined
// when the layer is not wider than the render (kNoDensityGain), and are otherwise decided exactly as every other HDR family.
void testSupercruiseLines() {
    UiFamilyFacts f;
    f.targetKind = 1;  // the lit HDR eye target
    UiFamilyWhy why = UiFamilyWhy::kOther;
    check(kUiVsOrbitLines == 0xC7FA0C0F5DD49180ull && kUiPsOrbitLines == 0x6EEF165A350DA30Full &&
              kUiVsSupercruiseBars == 0xA47A3315FFF5E2E4ull && kUiPsSupercruiseBars == 0x869FFF43E875906Eull,
          "SC1a: the pairs are the disassembled ones (vs C7FA0C0F.. / ps 6EEF165A.., vs A47A3315.. / ps 869FFF43..)");
    f.vs = kUiVsOrbitLines;
    f.ps = kUiPsOrbitLines;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kOrbitLines && why == UiFamilyWhy::kDirect,
          "SC1b: the orbit lines' pair on the HDR target names the orbit-lines family, a direct shader");
    f.vs = kUiVsSupercruiseBars;
    f.ps = kUiPsSupercruiseBars;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kSupercruiseBars && why == UiFamilyWhy::kDirect,
          "SC1c: the bars' pair on the HDR target names the supercruise-bars family");
    // The PAIR is the key: the bars' vertex shader is seen with another pixel shader (the flat recipes), a missing pixel shader
    // (ui_layer.cpp reads it for these two vertex shaders alone) and the other family's pixel shader name nothing.
    bool nothing = true;
    const uint64_t kFlatBarsPs = 0xC67370DE72E4422Cull;  // flat_projection_recipes.h: the vertex shader's other pair
    for (uint64_t ps : {kFlatBarsPs, 0ull, kUiPsOrbitLines, kUiPsSupercruiseBars ^ 1ull, kUiPsSupercruiseBars << 1}) {
        f.vs = kUiVsSupercruiseBars;
        f.ps = ps;
        why = UiFamilyWhy::kOther;
        nothing = nothing && uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotPostTonemap;
    }
    for (uint64_t ps : {0ull, kUiPsSupercruiseBars, kUiPsOrbitLines ^ 1ull, kFlatBarsPs}) {
        f.vs = kUiVsOrbitLines;
        f.ps = ps;
        why = UiFamilyWhy::kOther;
        nothing = nothing && uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotPostTonemap;
    }
    check(nothing, "SC1d: (A47A, another pixel shader) and (C7FA, another pixel shader) stay kNone, whatever the pixel shader says");
    // A vertex shader one bit away, and the pair on any other target, name nothing.
    f.vs = kUiVsOrbitLines ^ 1ull;
    f.ps = kUiPsOrbitLines;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kNone, "SC1e: a vertex shader one bit from the orbit lines' names nothing");
    f.vs = kUiVsSupercruiseBars ^ 0x100ull;
    f.ps = kUiPsSupercruiseBars;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kNone, "SC1f: ...nor one bit from the bars'");
    bool offHdr = true;
    for (int kind : {0, 2}) {
        for (const auto& pair : {std::pair<uint64_t, uint64_t>{kUiVsOrbitLines, kUiPsOrbitLines},
                                 std::pair<uint64_t, uint64_t>{kUiVsSupercruiseBars, kUiPsSupercruiseBars}}) {
            UiFamilyFacts g;
            g.targetKind = kind;
            g.vs = pair.first;
            g.ps = pair.second;
            offHdr = offHdr && uiLayerFamilyFor(g) == UiLayerFamily::kNone;
            // Over a learned surface or the 2D screen's SRV the post-tonemap branch names an interface composite, never the lines.
            g.learnedSurface = true;
            offHdr = offHdr && uiLayerFamilyFor(g) != UiLayerFamily::kOrbitLines && uiLayerFamilyFor(g) != UiLayerFamily::kSupercruiseBars;
            g.learnedSurface = false;
            g.panelSized = true;
            offHdr = offHdr && uiLayerFamilyFor(g) != UiLayerFamily::kOrbitLines && uiLayerFamilyFor(g) != UiLayerFamily::kSupercruiseBars;
        }
    }
    check(offHdr, "SC1g: on a draw that is not into an eye target, or into the post-tonemap one, neither pair is a family (HDR branch only)");

    // The names the 30 s line prints (taken: "<name> <rate>"), each its own.
    check(std::strcmp(uiLayerFamilyName(UiLayerFamily::kOrbitLines), "orbit lines") == 0 &&
              std::strcmp(uiLayerFamilyName(UiLayerFamily::kSupercruiseBars), "supercruise bars") == 0,
          "SC2a: the families are named \"orbit lines\" and \"supercruise bars\" in the 30 s line");
    bool distinctNames = true;
    for (uint32_t a = 1; a < static_cast<uint32_t>(UiLayerFamily::kCount); ++a)
        for (uint32_t b = a + 1; b < static_cast<uint32_t>(UiLayerFamily::kCount); ++b)
            distinctNames = distinctNames && std::strcmp(uiLayerFamilyName(static_cast<UiLayerFamily>(a)),
                                                        uiLayerFamilyName(static_cast<UiLayerFamily>(b))) != 0;
    check(distinctNames, "SC2b: every family has a name of its own (the census rows cannot merge)");
    check(std::strstr(uiLayerFamilyName(UiLayerFamily::kSupercruiseBars), "dust") == nullptr &&
              std::strstr(uiLayerFamilyName(UiLayerFamily::kOrbitLines), "dust") == nullptr,
          "SC2c: neither name calls the line draws space dust (the maintainer's naming: the A47A line list is the bars)");

    // The one list of families the crisp take draws into the HDR layer, and the two that are scene lines.
    bool takesHdr = true, sceneLines = true;
    for (uint32_t i = 0; i < static_cast<uint32_t>(UiLayerFamily::kCount); ++i) {
        const UiLayerFamily fam = static_cast<UiLayerFamily>(i);
        const bool expectHdr = fam == UiLayerFamily::kHolo || fam == UiLayerFamily::kFlightHud || fam == UiLayerFamily::kSprite ||
                               fam == UiLayerFamily::kHoloGeneric || fam == UiLayerFamily::kOrbitLines ||
                               fam == UiLayerFamily::kSupercruiseBars || fam == UiLayerFamily::kSpaceDust;
        takesHdr = takesHdr && uiLayerFamilyTakesHdr(fam) == expectHdr;
        sceneLines = sceneLines && uiLayerFamilyIsSceneLines(fam) == (fam == UiLayerFamily::kOrbitLines || fam == UiLayerFamily::kSupercruiseBars);
    }
    check(takesHdr, "SC3a: uiLayerFamilyTakesHdr is exactly the holo panels, flight HUD, sprite, holograms, orbit lines, bars and space dust");
    check(sceneLines, "SC3b: uiLayerFamilyIsSceneLines is exactly the orbit lines and the bars (the space dust has no width to give: it is exempt from the density rule)");
    check(uiLayerWiderThanRender(5040, 4873, 2016, 1949) && uiLayerWiderThanRender(4032, 3898, 2016, 1949) &&
              !uiLayerWiderThanRender(2016, 1949, 2016, 1949) && !uiLayerWiderThanRender(4032, 1949, 4032, 1949) &&
              !uiLayerWiderThanRender(5040, 1949, 2016, 1949) && !uiLayerWiderThanRender(2016, 4873, 2016, 1949) &&
              !uiLayerWiderThanRender(1000, 1000, 2016, 1949) && !uiLayerWiderThanRender(5040, 4873, 0, 1949) &&
              !uiLayerWiderThanRender(5040, 4873, 2016, 0),
          "SC3c: the layer is wider than the render only when both axes are (125% over a 2016 wide render, and 100% over it, are; "
          "DLAA at 100%, one axis, a smaller layer and a dead render size are not)");

    // The decision: the lines' measured states into the production classification, then the ladder.
    UiDsState orbitDs;  // depth GEQUAL, no write; stencil EQUAL, ref 1, read 0x81, write 0 (census: ds=17wZ st=11)
    orbitDs.depthEnable = true;
    orbitDs.depthFunc = 7;
    orbitDs.depthWriteAll = false;
    orbitDs.stencilEnable = true;
    orbitDs.readMask = 0x81;
    orbitDs.writeMask = 0x00;
    orbitDs.front.func = 3;
    orbitDs.back.func = 3;
    const UiDsEffect orbitEffect = uiLayerDsEffect(orbitDs, true);
    check(orbitEffect.depthTest && orbitEffect.stencilTest && !orbitEffect.depthWrite && !orbitEffect.stencilWrite &&
              orbitEffect.tests() && !orbitEffect.writes(),
          "SC4a: the orbit lines' state TESTS depth and stencil and writes neither (so the seed serves it and no write-back runs)");
    UiDsState barsDs;  // depth and stencil off (census: ds=02wA st=04)
    barsDs.depthEnable = false;
    barsDs.depthFunc = 2;
    barsDs.depthWriteAll = true;
    barsDs.stencilEnable = false;
    const UiDsEffect barsEffect = uiLayerDsEffect(barsDs, true);
    check(!barsEffect.tests() && !barsEffect.writes(), "SC4b: the bars' state tests and writes nothing: nothing to seed");
    UiBlendRt over;  // SRC_ALPHA, INV_SRC_ALPHA, add; alpha the same; all channels (census: bl=15,6,1/5,6,1 bm=F)
    over.enable = true;
    over.src = uiblend::kSrcAlpha;
    over.dst = uiblend::kInvSrcAlpha;
    over.op = uiblend::kOpAdd;
    over.srcA = uiblend::kSrcAlpha;
    over.dstA = uiblend::kInvSrcAlpha;
    over.opA = uiblend::kOpAdd;
    over.mask = 0xF;
    check(uiLayerBlendShape(over) == UiBlendShape::kOver, "SC4c: both lines' blend is the over shape the layer converts");

    auto lines = [&](UiLayerFamily fam, const UiDsEffect& ds, void (*edit)(UiLayerDrawFacts&)) {
        UiLayerDrawFacts g;
        g.family = fam;
        g.verdictForwards = true;
        g.eyeTarget = true;
        g.ldrView = false;   // the lit HDR target
        g.crispHdr = true;   // the crisp take is armed and the family is one of its (ui_layer.cpp: uiLayerFamilyTakesHdr)
        g.eye = 0;
        g.armed = true;
        g.ds = ds;
        g.blend = UiBlendShape::kOver;
        edit(g);
        return uiLayerDecide(g);
    };
    auto none = [](UiLayerDrawFacts&) {};
    check(lines(UiLayerFamily::kOrbitLines, orbitEffect, none) == UiLayerDecision::kRedirect,
          "SC5a: an armed orbit-lines draw into the HDR target, testing the seeded depth-stencil, over-blended, is redirected");
    check(lines(UiLayerFamily::kSupercruiseBars, barsEffect, none) == UiLayerDecision::kRedirect,
          "SC5b: an armed bars draw (no depth, no stencil) is redirected");
    check(lines(UiLayerFamily::kOrbitLines, orbitEffect, [](UiLayerDrawFacts& g) { g.late = true; }) == UiLayerDecision::kLate &&
              lines(UiLayerFamily::kSupercruiseBars, barsEffect, [](UiLayerDrawFacts& g) { g.late = true; }) == UiLayerDecision::kLate,
          "SC5c: after its eye's composite either family is kLate (gate G1)");
    check(lines(UiLayerFamily::kOrbitLines, orbitEffect, [](UiLayerDrawFacts& g) { g.dsReproducible = false; }) ==
              UiLayerDecision::kDepthStencilTest,
          "SC5d: orbit lines over a depth target the layer cannot reproduce are kDepthStencilTest (the read-only view, MSAA...)");
    check(lines(UiLayerFamily::kSupercruiseBars, barsEffect, [](UiLayerDrawFacts& g) { g.dsReproducible = false; }) ==
              UiLayerDecision::kRedirect,
          "SC5e: ...which the bars never need: they test nothing");
    check(lines(UiLayerFamily::kOrbitLines, orbitEffect, [](UiLayerDrawFacts& g) { g.crispHdr = false; }) == UiLayerDecision::kHdrTarget &&
              lines(UiLayerFamily::kSupercruiseBars, barsEffect, [](UiLayerDrawFacts& g) { g.crispHdr = false; }) == UiLayerDecision::kHdrTarget,
          "SC5f: with the crisp take off (or stood down) both stay plain HDR, left in the scene");
    check(lines(UiLayerFamily::kOrbitLines, orbitEffect, [](UiLayerDrawFacts& g) { g.lateTone = true; }) == UiLayerDecision::kToneLate &&
              lines(UiLayerFamily::kOrbitLines, orbitEffect, [](UiLayerDrawFacts& g) { g.armed = false; }) == UiLayerDecision::kNotArmed &&
              lines(UiLayerFamily::kOrbitLines, orbitEffect, [](UiLayerDrawFacts& g) { g.mrt = true; }) == UiLayerDecision::kMrt &&
              lines(UiLayerFamily::kOrbitLines, orbitEffect, [](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kMultiply; }) == UiLayerDecision::kBlendRefused &&
              lines(UiLayerFamily::kOrbitLines, orbitEffect, [](UiLayerDrawFacts& g) { g.layerReady = false; }) == UiLayerDecision::kLayerFailed,
          "SC5g: the ladder's other tests reach them as they reach every HDR family (tone-late, not armed, MRT, a multiply, no layer)");

    // THE DENSITY DECLINE.
    auto narrow = [](UiLayerDrawFacts& g) { g.layerWider = false; };
    check(lines(UiLayerFamily::kOrbitLines, orbitEffect, narrow) == UiLayerDecision::kNoDensityGain &&
              lines(UiLayerFamily::kSupercruiseBars, barsEffect, narrow) == UiLayerDecision::kNoDensityGain,
          "SC6a: a layer that is not wider than the render declines both: kNoDensityGain");
    check(lines(UiLayerFamily::kHolo, orbitEffect, narrow) == UiLayerDecision::kRedirect &&
              lines(UiLayerFamily::kFlightHud, orbitEffect, narrow) == UiLayerDecision::kRedirect &&
              lines(UiLayerFamily::kHoloGeneric, barsEffect, narrow) == UiLayerDecision::kRedirect &&
              lines(UiLayerFamily::kSprite, barsEffect, narrow) == UiLayerDecision::kRedirect,
          "SC6b: the fact is asked of the two scene-line families alone: the cockpit HUD families are decided as they always were");
    check(lines(UiLayerFamily::kOrbitLines, orbitEffect,
                [](UiLayerDrawFacts& g) {
                    g.layerWider = false;
                    g.late = true;
                }) == UiLayerDecision::kLate &&
              lines(UiLayerFamily::kOrbitLines, orbitEffect,
                    [](UiLayerDrawFacts& g) {
                        g.layerWider = false;
                        g.armed = false;
                    }) == UiLayerDecision::kNotArmed,
          "SC6c: the density decline comes after the arming (the layer's size is the door's): late and not-armed are the reasons first");
    check(lines(UiLayerFamily::kOrbitLines, orbitEffect,
                [](UiLayerDrawFacts& g) {
                    g.layerWider = false;
                    g.mrt = true;
                    g.dsReproducible = false;
                    g.layerReady = false;
                }) == UiLayerDecision::kNoDensityGain,
          "SC6d: ...and before every state test: the reason is the density's, whatever the state would have said");

    // THE FAMILY'S OWN READINESS (the bars' private geometry shader): before the layer's readiness, the bars alone.
    auto notReady = [](UiLayerDrawFacts& g) { g.familyReady = false; };
    check(lines(UiLayerFamily::kSupercruiseBars, barsEffect, notReady) == UiLayerDecision::kFamilyNotReady &&
              lines(UiLayerFamily::kSupercruiseBars, barsEffect,
                    [](UiLayerDrawFacts& g) {
                        g.familyReady = false;
                        g.layerReady = false;
                    }) == UiLayerDecision::kFamilyNotReady,
          "SC7a: a bars draw whose private pass is missing is kFamilyNotReady, ahead of the layer's own readiness (no HDR layer is made for it)");
    check(lines(UiLayerFamily::kOrbitLines, orbitEffect, notReady) == UiLayerDecision::kRedirect &&
              lines(UiLayerFamily::kHolo, orbitEffect, notReady) == UiLayerDecision::kRedirect,
          "SC7b: the orbit lines need no private pass, and no other family is asked");
    check(lines(UiLayerFamily::kSupercruiseBars, barsEffect,
                [](UiLayerDrawFacts& g) {
                    g.familyReady = false;
                    g.late = true;
                }) == UiLayerDecision::kLate,
          "SC7c: every earlier reason still comes first");
    bool texts = true;
    for (UiLayerDecision d : {UiLayerDecision::kNoDensityGain, UiLayerDecision::kFamilyNotReady}) {
        const std::string name = uiLayerDecisionName(d), key = uiLayerDecisionKey(d);
        texts = texts && !name.empty() && name != "?" && !key.empty() && key != "?";
        for (uint32_t other = 0; other < static_cast<uint32_t>(UiLayerDecision::kCount); ++other) {
            if (other == static_cast<uint32_t>(d)) continue;
            texts = texts && name != uiLayerDecisionName(static_cast<UiLayerDecision>(other)) &&
                    key != uiLayerDecisionKey(static_cast<UiLayerDecision>(other));
        }
    }
    check(texts, "SC7d: the two new decisions have a reason and a short key of their own (the 30 s lines and the first-seen lines name them)");
    // The key-off contract: with the two new facts at their defaults the decision of every family is the decision as it was.
    UiLayerDrawFacts defaults;
    check(defaults.layerWider && defaults.familyReady, "SC7e: the new facts default to true, so a draw that does not set them is decided as before");

    // ---------------------------------------------------------------------------------------------------------------------------------
    // THE SPACE DUST (vs 9BFC7FD232328391 / ps DBF1725726018F52): 300 additive ribbon quads into the lit HDR target, depth GEQUAL with no
    // write, stencil ALWAYS with REPLACE under mask 0x40 (census 2026-10-07 05:48, frame 1 rows #145 and #194: ds=17wZ st=18 so=r40/w40/f8/3,3,3
    // bl=12,2,1/2,2,1 bm=F). The family is the pair, on the HDR target alone; the decision is every other HDR family's; it is exempt from the
    // density rule (it has no width to give) and needs no private pass.
    UiFamilyFacts d;
    d.targetKind = 1;
    d.vs = kUiVsSpaceDust;
    d.ps = kUiPsSpaceDust;
    UiFamilyWhy dwhy = UiFamilyWhy::kOther;
    check(kUiVsSpaceDust == 0x9BFC7FD232328391ull && kUiPsSpaceDust == 0xDBF1725726018F52ull, "SD1a: the pair is the disassembled one (vs 9BFC7FD232328391 / ps DBF1725726018F52)");
    check(uiLayerFamilyFor(d, &dwhy) == UiLayerFamily::kSpaceDust && dwhy == UiFamilyWhy::kDirect,
          "SD1b: the census tuple on the HDR target names the space-dust family, a direct shader");
    // The PAIR is the key: the flat route's other pixel shader for the same vertex shader (CB7AF179, flat_projection_recipes.h ~305), none, a bit away,
    // and the other two families' pixel shaders name nothing; neither does the pair on any other target.
    const uint64_t kFlatDustPs = 0xCB7AF179DF4E6A60ull;  // flat_projection_recipes.h: this vertex shader's flat-route pair
    bool dustNothing = true;
    for (uint64_t ps : {kFlatDustPs, 0ull, kUiPsSpaceDust ^ 1ull, kUiPsSpaceDust >> 4, kUiPsOrbitLines, kUiPsSupercruiseBars}) {
        d.ps = ps;
        dwhy = UiFamilyWhy::kOther;
        dustNothing = dustNothing && uiLayerFamilyFor(d, &dwhy) == UiLayerFamily::kNone && dwhy == UiFamilyWhy::kNotPostTonemap;
    }
    d.ps = kUiPsSpaceDust;
    for (uint64_t vs : {kUiVsSpaceDust ^ 1ull, kUiVsSpaceDust ^ 0x100ull, kUiVsSupercruiseBars, kUiVsOrbitLines}) {
        d.vs = vs;
        dustNothing = dustNothing && uiLayerFamilyFor(d) != UiLayerFamily::kSpaceDust;
    }
    d.vs = kUiVsSpaceDust;
    for (int kind : {0, 2}) {
        d.targetKind = kind;
        dustNothing = dustNothing && uiLayerFamilyFor(d) != UiLayerFamily::kSpaceDust;
        d.panelSized = true;
        dustNothing = dustNothing && uiLayerFamilyFor(d) != UiLayerFamily::kSpaceDust;
        d.panelSized = false;
    }
    check(dustNothing, "SD1c: (9BFC7FD2, another pixel shader), a vertex shader a bit away, the other families' shaders and any target but the lit HDR one name no space dust");
    // And the other two families keep their own names with the dust's shaders around.
    d.targetKind = 1;
    d.vs = kUiVsOrbitLines;
    d.ps = kUiPsOrbitLines;
    check(uiLayerFamilyFor(d) == UiLayerFamily::kOrbitLines, "SD1d: the orbit lines are still the orbit lines");
    d.vs = kUiVsSupercruiseBars;
    d.ps = kUiPsSupercruiseBars;
    check(uiLayerFamilyFor(d) == UiLayerFamily::kSupercruiseBars, "SD1e: ...and the bars the bars");
    check(std::strcmp(uiLayerFamilyName(UiLayerFamily::kSpaceDust), "space dust") == 0, "SD1f: the family is named \"space dust\" in the 30 s line");

    UiDsState dustDs;  // depth GEQUAL, no write; stencil ALWAYS with REPLACE on every outcome under mask 0x40, reference 8
    dustDs.depthEnable = true;
    dustDs.depthFunc = 7;
    dustDs.depthWriteAll = false;
    dustDs.stencilEnable = true;
    dustDs.readMask = 0x40;
    dustDs.writeMask = 0x40;
    dustDs.front.func = uids::kAlways;
    dustDs.front.fail = dustDs.front.depthFail = dustDs.front.pass = 3;  // D3D11_STENCIL_OP_REPLACE
    dustDs.back = dustDs.front;
    const UiDsEffect dustEffect = uiLayerDsEffect(dustDs, true);
    check(dustEffect.depthTest && !dustEffect.stencilTest && !dustEffect.depthWrite && dustEffect.stencilWrite && dustEffect.tests() && dustEffect.writes(),
          "SD2a: the dust's state TESTS depth and WRITES stencil (the test seeds the layer's depth; the write is kept by the colourless re-issue into the game's buffer)");
    UiBlendRt additive;  // ONE, ONE, add, all channels (census: bl=12,2,1/2,2,1 bm=F)
    additive.enable = true;
    additive.src = uiblend::kOne;
    additive.dst = uiblend::kOne;
    additive.op = uiblend::kOpAdd;
    additive.srcA = uiblend::kOne;
    additive.dstA = uiblend::kOne;
    additive.opA = uiblend::kOpAdd;
    additive.mask = 0xF;
    check(uiLayerBlendShape(additive) == UiBlendShape::kAdditive, "SD2b: the dust's blend is the additive shape, converted exactly (ui_layer_math.h uiLayerConvertBlend: colour as the game's, transmittance untouched)");
    UiBlendRt converted;
    check(uiLayerConvertBlend(additive, &converted) && converted.src == uiblend::kOne && converted.dst == uiblend::kOne && converted.dstA == uiblend::kOne &&
              converted.srcA == uiblend::kZero && (converted.mask & uiblend::kWriteRgb) == uiblend::kWriteRgb,
          "SD2c: ...ONE, ONE into the layer's colour, the layer's alpha (the transmittance) kept as it is");

    auto dust = [&](void (*edit)(UiLayerDrawFacts&)) {
        UiLayerDrawFacts g;
        g.family = UiLayerFamily::kSpaceDust;
        g.verdictForwards = true;
        g.eyeTarget = true;
        g.ldrView = false;
        g.crispHdr = true;
        g.eye = 0;
        g.armed = true;
        g.ds = dustEffect;
        g.blend = UiBlendShape::kAdditive;
        edit(g);
        return uiLayerDecide(g);
    };
    check(dust(none) == UiLayerDecision::kRedirect, "SD3a: the census tuple (armed, HDR, additive, depth tested, stencil written) is kRedirect");
    // One-token mutants of that row, each refused.
    check(dust([](UiLayerDrawFacts& g) {
              UiBlendRt minBlend;
              minBlend.enable = true;
              minBlend.src = uiblend::kOne;
              minBlend.dst = uiblend::kOne;
              minBlend.op = 4;  // D3D11_BLEND_OP_MIN
              minBlend.srcA = uiblend::kOne;
              minBlend.dstA = uiblend::kOne;
              minBlend.opA = uiblend::kOpAdd;
              minBlend.mask = 0xF;
              g.blend = uiLayerBlendShape(minBlend);
          }) == UiLayerDecision::kBlendRefused,
          "SD3b: the blend operation MIN instead of ADD is kBlendRefused");
    check(dust([](UiLayerDrawFacts& g) { g.blend = UiBlendShape::kMultiply; }) == UiLayerDecision::kBlendRefused, "SD3c: a multiply into the HDR layer has no transmittance route: kBlendRefused");
    check(dust([](UiLayerDrawFacts& g) { g.dsReproducible = false; }) == UiLayerDecision::kDepthStencilTest, "SD3d: an unreproducible depth state (read-only view, MSAA, another size) is kDepthStencilTest");
    check(dust([](UiLayerDrawFacts& g) { g.lateTone = true; }) == UiLayerDecision::kToneLate, "SD3e: a draw after its eye's tonemap re-issue is kToneLate");
    check(dust([](UiLayerDrawFacts& g) { g.late = true; }) == UiLayerDecision::kLate && dust([](UiLayerDrawFacts& g) { g.armed = false; }) == UiLayerDecision::kNotArmed &&
              dust([](UiLayerDrawFacts& g) { g.crispHdr = false; }) == UiLayerDecision::kHdrTarget && dust([](UiLayerDrawFacts& g) { g.eye = -1; }) == UiLayerDecision::kNoEye &&
              dust([](UiLayerDrawFacts& g) { g.mrt = true; }) == UiLayerDecision::kMrt && dust([](UiLayerDrawFacts& g) { g.layerReady = false; }) == UiLayerDecision::kLayerFailed,
          "SD3f: late, not armed, the crisp take off, no eye, a second target and no layer are each the ladder's own reason");
    check(dust([](UiLayerDrawFacts& g) { g.substituted = true; }) == UiLayerDecision::kSubstitutedWrite,
          "SD3g: a stencil write through a substituted geometry would be refused (the colourless re-issue cannot repeat it); the dust is never substituted");
    // The density rule and the private pass do not apply to it: it is taken at any layer size.
    check(dust([](UiLayerDrawFacts& g) { g.layerWider = false; }) == UiLayerDecision::kRedirect && dust([](UiLayerDrawFacts& g) { g.familyReady = false; }) == UiLayerDecision::kRedirect,
          "SD3h: the space dust is exempt from the density rule and the bars' private pass: taken whatever the layer's size");
    // The mutant of the exemption: were it a scene line, a narrow layer would decline it.
    check(lines(UiLayerFamily::kSupercruiseBars, dustEffect, narrow) == UiLayerDecision::kNoDensityGain,
          "SD3i: (control) a family that IS a scene line is declined by the same fact, so SD3h proves the exemption");
}

// fix.ui_quality's after-UI take (uiLayerNoteOther, vscreen.cpp): a draw
// after the UI that WRITES an eye target the UI was taken from is taken
// into the same layer too, after the UI, so it stays over it -- unless it
// samples an eye-sized input (uiLayerAfterWriteDecide's own gate: case b of
// the pixel test below) or the take itself refuses (governed by the exact
// same uiLayerDecide rules as any other family: case d). The read case
// (case c) never reaches either of these -- it is uiLayerNoteOther's
// unchanged 'R' branch, identified before a family is ever assigned, so
// there is nothing of its own to unit-test here.
void testAfterUi() {
    check(std::strcmp(uiLayerFamilyName(UiLayerFamily::kAfterUi), "after the UI") == 0,
          "the after-UI family's census name");
    check(uiLayerAfterWriteDecide(false) == UiAfterWriteDecision::kAttempt,
          "a write with no eye-sized input is attempted (case a's precondition)");
    check(uiLayerAfterWriteDecide(true) == UiAfterWriteDecision::kPostPass,
          "a write that samples an eye-sized input is left as a post pass, not attempted (case b)");
    UiLayerDrawFacts f;
    f.family = UiLayerFamily::kAfterUi;
    f.verdictForwards = true;
    f.eyeTarget = true;
    f.ldrView = true;
    f.eye = 1;
    f.armed = true;
    f.blend = UiBlendShape::kPremulOver;
    check(uiLayerDecide(f) == UiLayerDecision::kRedirect,
          "an after-UI write, armed and blended normally, is redirected exactly like any family");
    UiLayerDrawFacts refused = f;
    refused.mrt = true;
    check(uiLayerDecide(refused) == UiLayerDecision::kMrt,
          "an after-UI write into a second bound target is refused like any family (case d)");
    UiLayerDrawFacts notForwarded = f;
    notForwarded.verdictForwards = false;
    check(uiLayerDecide(notForwarded) == UiLayerDecision::kVerdict,
          "an after-UI write another fix swallows or re-issues is refused, not taken (case d)");
    // rc-since-rc2 review F4: the retry preserves the original decision's
    // exclusions. Original: a world-screen composite while the screen shows
    // the world is never taken (kWorldScreen); the family rule's kExcluded
    // never reaches a family. The retry must decline both the same way.
    {
        UiLayerDrawFacts w;
        w.family = UiLayerFamily::kScreen;
        w.worldScreen = true;
        check(uiLayerDecide(w) == UiLayerDecision::kWorldScreen,
              "the original decision holds the world screen out of the layer");
        check(!uiLayerAfterWritePreserved(true, false, false),
              "an excluded shader is never taken after the UI (F4)");
        check(!uiLayerAfterWritePreserved(false, true, true),
              "the held world-screen composite is never taken after the UI (F4)");
        check(uiLayerAfterWritePreserved(false, true, false),
              "an ordinary write while the world shows still attempts (F4)");
        check(uiLayerAfterWritePreserved(false, false, true),
              "a panel-sized write with nothing held still attempts (F4)");
    }
}

// ------------------------------------------------------------ the crisp HUD

// The crisp-HUD parity model (docs/cockpit-hud-layer-design-2026-09-27.md,
// "The design" point 5, Phase 1's rig sentence; gate G-F's numbers). Stock
// Elite composites a HUD draw into the HDR frame and tonemaps:
//   stock = T(F*(1 - a) + L)
// with the draw's premultiplied radiance L and coverage a over the
// background F. The HDR layer tonemaps separately and composites after:
//   layer = T(F)*(1 - a) + T(L)
// The two agree where the HUD is opaque or the background dark; they differ
// where translucent glass crosses a bright background -- the accepted cost,
// Decisions 1's lost bloom halo.
//
// The model is a synthetic 96x64 frame: a dark floor (~0.004 a channel, the
// G-E dark scene's 0.003-0.012 luma), a warm gaussian sun peaking at 16 with
// the gradient between, and a HUD element with an opaque core, an additive
// glow over the dark field and a translucent glass band across the sun
// (coverage 0.15..0.85, dim above the sun's centre line, lit below), plus an
// opaque element on the disc itself. Two stand-in tonemaps:
//   soft:  T(x) = x / (1 + x)            (Reinhard)
//   harsh: T(x) = log1p(x) / log1p(16)   (a log shoulder, white at 16)
// -- the point is the family of compressive curves, not Elite's exact one.
// The metric is the gate's own (hud_parity.py): per pixel the max over RGB
// of |stock - layer|, in 8-bit steps (x255).
//
// What it proves:
//   * opaque (a == 1) and uncovered (a == 0, L == 0) pixels agree with stock
//     to the LAST BIT, both curves -- the math identity, no epsilon;
//   * HUD over a dark background (at most 0.0075 a channel) agrees within
//     2.0 steps, the budget G-F chose (hud_parity.py's --budget default; the
//     flight measured p50/p99 = 0.00 steps over ~440k HUD pixels). The bound
//     is by construction: a compressive T keeps the error under T(F) <= F a
//     channel, so the dark field's own ceiling is 1.9 steps, and the model
//     measures 0.63 (soft) and 0.13 (harsh);
//   * translucent glass over a bright background (0 < a < 1, background luma
//     > 1 -- the gate's regime and its Rec.709 luma) differs from stock
//     NONZERO (the model's worst is 107 steps soft, 69 harsh) and under
//     113.0 steps, the worst restricted p99 the five flight-2 ledgers
//     measured (40-113 steps, 5-40 pixels a frame, ledger 123104 the top).
//     The rig documents the accepted cost; a model change that makes the
//     regime worse than measured screams;
//   * with the take removed the rig FAILS. The take is what moves the HUD
//     from the HDR image into the layer; removed, the layer is empty, its
//     composite the bare background to the last bit, and parity degenerates
//     to stock -- it cannot see a removed take. So the block carries a
//     presence assertion: the layer composite must differ from T(F) by 8
//     steps somewhere the HUD is (the model's strongest contribution is
//     169-201 steps). Its teeth are proven by construction, not by toggling
//     code: the same assertion run on the take-removed model fails.
//
// What it does NOT prove: T here is a stand-in. Elite's real tonemap (the
// exposure scalar and colour LUT, the EDHM 3D-LUT variants among them) is
// deliberately not transcribed -- the design rules that out and re-issues
// the game's own draw; the measured in-sim tail lives in the design doc's
// G-F entries. And the flight's 113 is a restricted p99 over real frames
// while the rig's is a max over a synthetic scene, so the ceiling here is a
// bound on the model, not the in-sim budget.

double toneSoft(double x) { return x / (1.0 + x); }
double toneHarsh(double x) { return std::log1p(x) / std::log1p(16.0); }

void testHudParity() {
    // The stand-ins the exactness and the dark bound rest on.
    bool shape = toneSoft(0.0) == 0.0 && toneHarsh(0.0) == 0.0;
    for (int i = 1; i <= 64 && shape; ++i) {
        const double x = i * 0.25;  // 0.25 .. 16, the frame's HDR span
        shape = toneSoft(x) < x && toneHarsh(x) < x && toneSoft(x) > toneSoft(x - 0.25) &&
                toneHarsh(x) > toneHarsh(x - 0.25);
    }
    check(shape, "parity model: both stand-in tonemaps fix T(0) = 0 and are monotone and compressive");

    const int W = 96, H = 64;
    std::vector<double> F(W * H * 3, 0.0), L(W * H * 3, 0.0), cov(W * H, 0.0);
    auto at = [&](int x, int y) { return y * W + x; };
    // The dark floor and a warm gaussian sun (sigma 6); its falloff is the gradient.
    const double floorC[3] = {0.004, 0.003, 0.006};
    const double sunC[3] = {16.0, 14.0, 11.0};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double dx = x - 72.0, dy = y - 24.0;
            const double g = std::exp(-(dx * dx + dy * dy) / 72.0);
            for (int c = 0; c < 3; ++c)
                F[at(x, y) * 3 + c] = floorC[c] + sunC[c] * g;
        }
    auto set = [&](int x, int y, double a, double r, double g, double b) {
        const int p = at(x, y) * 3;
        cov[at(x, y)] = a;
        L[p + 0] = r;
        L[p + 1] = g;
        L[p + 2] = b;
    };
    // An opaque core over the dark field.
    for (int y = 40; y < 56; ++y)
        for (int x = 8; x < 28; ++x) set(x, y, 1.0, 0.35, 0.50, 0.40);
    // An additive glow over the dark field, a warm ramp down to a whisper.
    for (int y = 40; y < 56; ++y)
        for (int x = 30; x < 48; ++x) {
            const double s = 0.05 + 0.45 * (x - 30) / 17.0;
            set(x, y, 0.0, s, s * 0.8, s * 0.6);
        }
    // Translucent glass across the sun: coverage 0.15..0.85 across the band,
    // dim above the sun's centre line, lit below it.
    for (int y = 8; y < 40; ++y)
        for (int x = 56; x < 88; ++x) {
            const double a = 0.15 + 0.7 * (x - 56) / 31.0;
            if (y < 24)
                set(x, y, a, a * 0.08, a * 0.08, a * 0.09);
            else
                set(x, y, a, a * 0.50, a * 0.55, a * 0.60);
        }
    // An opaque element on the disc itself: exactness is F-independent.
    for (int y = 16; y < 22; ++y)
        for (int x = 64; x < 70; ++x) set(x, y, 1.0, 0.45, 0.35, 0.30);

    // The tolerances, all in 8-bit steps: the budget G-F chose (the flight's
    // p50/p99 measured 0.00); the measured ceiling, flight 2's worst
    // restricted p99; the regime's cost must be plainly there; the HUD must
    // move its own composite by this somewhere.
    const double kBudget = 2.0, kCeiling = 113.0, kNonzero = 1.0, kPresent = 8.0;
    const double kDarkChan = 0.0075;  // over a darker field the error is under T(F) <= F a channel
    const double lumaW[3] = {0.2126, 0.7152, 0.0722};  // the gate's luma (hud_parity.py)
    double (*const curves[2])(double) = {toneSoft, toneHarsh};
    int nOpaque = 0, nUncovered = 0, nAdditive = 0, nDarkHud = 0, nRegime = 0;
    double exactWorst[2] = {0.0, 0.0}, darkWorst[2] = {0.0, 0.0}, regimeWorst[2] = {0.0, 0.0};
    double present[2] = {0.0, 0.0}, removed[2] = {0.0, 0.0};
    for (int k = 0; k < 2; ++k) {
        double (*const T)(double) = curves[k];
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const int p = at(x, y) * 3;
                const double a = cov[at(x, y)];
                const bool hud = a > 0.0 || L[p] > 0.0 || L[p + 1] > 0.0 || L[p + 2] > 0.0;
                double err = 0.0, own = 0.0, gone = 0.0;
                for (int c = 0; c < 3; ++c) {
                    const double stock = T(F[p + c] * (1.0 - a) + L[p + c]);
                    const double layer = T(F[p + c]) * (1.0 - a) + T(L[p + c]);
                    err = (std::max)(err, std::fabs(stock - layer));
                    own = (std::max)(own, std::fabs(layer - T(F[p + c])));
                    // The take removed: an empty layer over the same frame.
                    const double bare = T(F[p + c]) * (1.0 - 0.0) + T(0.0);
                    gone = (std::max)(gone, std::fabs(bare - T(F[p + c])));
                }
                err *= 255.0;
                own *= 255.0;
                gone *= 255.0;
                if (a == 1.0 || !hud) {
                    exactWorst[k] = (std::max)(exactWorst[k], err);
                    if (k == 0) {
                        if (a == 1.0)
                            ++nOpaque;
                        else
                            ++nUncovered;
                    }
                }
                if (!hud)
                    continue;
                if (k == 0 && a == 0.0)
                    ++nAdditive;
                present[k] = (std::max)(present[k], own);
                removed[k] = (std::max)(removed[k], gone);
                const double fMax = (std::max)(F[p], (std::max)(F[p + 1], F[p + 2]));
                if (fMax <= kDarkChan) {
                    darkWorst[k] = (std::max)(darkWorst[k], err);
                    if (k == 0)
                        ++nDarkHud;
                }
                const double lu = lumaW[0] * F[p] + lumaW[1] * F[p + 1] + lumaW[2] * F[p + 2];
                if (0.0 < a && a < 1.0 && lu > 1.0) {
                    regimeWorst[k] = (std::max)(regimeWorst[k], err);
                    if (k == 0)
                        ++nRegime;
                }
            }
    }
    check(nOpaque >= 300 && nUncovered >= 4000 && nAdditive >= 200 && nDarkHud >= 500 && nRegime >= 400,
          "parity model: the scene holds every regime -- opaque, uncovered, additive over dark, "
          "translucent over bright");
    check(exactWorst[0] == 0.0 && exactWorst[1] == 0.0,
          "parity model: opaque and uncovered pixels match stock to the last bit, both curves");
    check(darkWorst[0] <= kBudget && darkWorst[1] <= kBudget,
          "parity model: HUD over a dark background matches stock within the G-F budget of 2 steps, "
          "both curves");
    check(regimeWorst[0] > kNonzero && regimeWorst[1] > kNonzero,
          "parity model: translucent glass over a bright background differs from stock, plainly "
          "nonzero (the accepted halo cost), both curves");
    check(regimeWorst[0] <= kCeiling && regimeWorst[1] <= kCeiling,
          "parity model: that difference stays under the measured 113-step ceiling, both curves");
    check(present[0] >= kPresent && present[1] >= kPresent,
          "parity model: the HUD is present in the layer composite where it should be");
    check(removed[0] == 0.0 && removed[1] == 0.0,
          "parity model: fail with the take removed -- an empty layer composites to the bare "
          "background to the last bit, so the presence assertion fails and the rig screams");
}

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
    // The three crisp-HUD families' MEASURED states (Phase 0 census, flights
    // 1-3; build/hud_census_f3.txt's "ga <family> state" lines), classified:
    // the holo panels and the flight HUD test GEQUAL against the scene pair
    // and write nothing; the target sprite is depth-off by construction and
    // writes STENCIL (its footprint, ref 5, mask 0x05), so its take needs the
    // seeded copy's bit 0x01 AND the colourless write-back into the game's
    // own buffer.
    D3D11_DEPTH_STENCIL_DESC hud{};
    hud.DepthEnable = TRUE;
    hud.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    hud.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    hud.StencilEnable = FALSE;
    e = uiLayerDsEffect(uiLayerDsStateFrom(&hud, 0), true);
    check(e.depthTest && !e.depthWrite && !e.stencilTest && !e.stencilWrite,
          "the flight HUD (and the holo panels): tests GEQUAL, writes nothing -- seeded depth, no "
          "write-back");
    D3D11_DEPTH_STENCIL_DESC spr{};
    spr.DepthEnable = FALSE;  // the census's "depth 0 func 2 write 1": the
    spr.DepthFunc = D3D11_COMPARISON_LESS;  // write mask is a no-op with depth off
    spr.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    spr.StencilEnable = TRUE;
    spr.StencilReadMask = 0x01;
    spr.StencilWriteMask = 0x05;
    spr.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                     D3D11_COMPARISON_EQUAL};
    spr.BackFace = spr.FrontFace;
    e = uiLayerDsEffect(uiLayerDsStateFrom(&spr, 0), true);
    check(!e.depthTest && !e.depthWrite && e.stencilTest && e.stencilWrite,
          "the target sprite: no depth test or write (depth is off), tests and writes stencil -- "
          "seeded bit 0x01, and the write-back keeps the game's buffer");
    // A stencil test against a view with no stencil plane (D32_FLOAT): D3D11
    // passes it and drops the write; there is nothing to seed, so nothing is
    // (review P3-6: it re-seeded on every draw).
    D3D11_DEPTH_STENCIL_DESC t{};
    t.DepthEnable = FALSE;
    t.StencilEnable = TRUE;
    t.StencilReadMask = t.StencilWriteMask = 0x04;
    t.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                   D3D11_COMPARISON_EQUAL};
    t.BackFace = t.FrontFace;
    UiDsState st = uiLayerDsStateFrom(&t, 0);
    check(uiLayerDsEffect(st, true).stencilTest && uiLayerDsEffect(st, true).stencilWrite,
          "with a stencil plane, the test and the write count");
    st.stencilPlane = false;
    check(!uiLayerDsEffect(st, true).tests() && !uiLayerDsEffect(st, true).writes(),
          "without one, neither does: no seed, every draw");
}

// ------------------------------------------------------- the render state

// The panel formula's inputs as ui_panel_scale.cpp and the sizing chains
// read them (ui_quality_math.h), and the chain instrument's shape gate.
void testRenderState() {
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

    // The rule: a panel is a constant times U = W / 2 tan(vFOV/2).
    check(near1(uiQualityFovTangent(1.2648f, 1.2648f), 2.5296, 1e-3) &&
              near1(uiQualityFovTangent(1.1779f, -1.1779f), 2.3558, 1e-3) &&
              near1(uiQualityFovTangent(0.9657f, 1.4281f), 2.3417, 1e-3) &&
              near1(uiQualityFovTangent(1.4281f, 0.9657f), 2.3417, 1e-3),
          "2 tan(vFOV/2): Pimax 2.5296, trimmed 2.3558, Quest 3 2.3417 (the angle, not the tangents' sum "
          "2.3938), either sign, either order");
    check(uiQualityFovTangent(0.0f, 1.0f) == 0.0f && uiQualityFovTangent(1.0f, NAN) == 0.0f,
          "no usable tangent, no rule");
    check(!uiQualityCandidateShape(512, 512, 1995, 1970) && !uiQualityCandidateShape(256, 1, 1995, 1970) &&
              !uiQualityCandidateShape(1995, 1970, 1995, 1970) &&
              !uiQualityCandidateShape(3840, 2160, 1995, 1970) && !uiQualityCandidateShape(10, 12, 1995, 1970) &&
              uiQualityCandidateShape(417, 625, 1995, 1970),
          "the candidate shape: smaller than the eye, no power of two, no sliver");
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
    // Windows' own d3d11 through common/system_d3d11.h, never an import: EDVR's proxy sits beside this exe.
    const auto createDevice = edvr::systemD3D11CreateDevice();
    if (!createDevice ||
        FAILED(createDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr,
                            0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx))) {
        return false;
    }
    check(edvr::reportSystemD3D11Only("ui_quality_test"),
          "the rig runs on System32's d3d11.dll and on no other d3d11.dll");
    ComPtr<ID3DBlob> v, p;
    if (!compile(kQuadHlsl, sizeof(kQuadHlsl) - 1, "vsMain", "vs_5_0", &v) ||
        !compile(kQuadHlsl, sizeof(kQuadHlsl) - 1, "psMain", "ps_5_0", &p)) {
        return false;
    }
    if (FAILED(g.dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &g.vs)) ||
        FAILED(g.dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &g.ps)) ||
        FAILED(g.dev->CreateComputeShader(kUiLayerCompositeBytecode, sizeof(kUiLayerCompositeBytecode), nullptr, &g.cs))) {
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

// fix.ui_quality's after-UI take, at the pixel level: the game's own order
// (a small overlay quad drawn AFTER a big UI quad, into the same target) is
// what the composite must reproduce once the UI has moved to the layer. The
// overlay is routed by the SAME call uiLayerNoteOther makes --
// uiLayerAfterWriteDecide, ui_layer_math.h -- so a regression that stops it
// returning kAttempt for a plain write fails this, not just a hand-picked
// "before/after" pair.
// The layer's own storage (premultiplied colour + transmittance, 8-bit UNORM)
// rounds at a different point than one hardware blend pass straight into
// `direct`, exactly as testComposite's own worst<=3 tolerance already
// accounts for -- so "the same order" is judged by a small per-channel
// tolerance, not byte equality, and "the wrong order" by a difference far
// past it (these colours are chosen far enough apart that landing under
// instead of over the UI misses by tens of levels, not a rounding LSB).
int worstDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    int worst = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) worst = (std::max)(worst, std::abs(int(a[i]) - int(b[i])));
    return worst;
}

void testAfterUiOrder(Gpu& g) {
    using namespace uiblend;
    const uint32_t W = 16, H = 12;
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
    const float bg[4] = {0.1f, 0.15f, 0.2f, 1.0f};
    // "The UI": a big translucent quad, always taken (unaffected by this fix).
    const UiBlendRt uiGameBlend = blend(true, kSrcAlpha, kInvSrcAlpha);
    const float uiRect[4] = {-1, -1, 1, 1};
    const float uiColour[4] = {0.8f, 0.15f, 0.15f, 0.9f};
    // "The overlay": a smaller translucent quad the stock game draws AFTER
    // the UI, centred so it overlaps it -- the field report's holo effect
    // over a UI surface, modelled generically.
    const UiBlendRt overlayGameBlend = blend(true, kSrcAlpha, kInvSrcAlpha);
    const float overlayRect[4] = {-0.4f, -0.4f, 0.4f, 0.4f};
    const float overlayColour[4] = {0.1f, 0.85f, 0.15f, 0.55f};

    UiBlendRt uiConv, overlayConv;
    check(uiLayerConvertBlend(uiGameBlend, &uiConv), "the UI quad's blend converts");
    check(uiLayerConvertBlend(overlayGameBlend, &overlayConv), "the overlay quad's blend converts");
    ComPtr<ID3D11BlendState> uiGameBs = state(g, uiGameBlend), overlayGameBs = state(g, overlayGameBlend);
    ComPtr<ID3D11BlendState> uiLayerBs = state(g, uiConv), overlayLayerBs = state(g, overlayConv);

    // The stock reference: both quads drawn directly into one target, in the
    // game's own order -- the overlay ends up over the UI.
    Tex direct = makeTex(g, W, H, false);
    g.ctx->ClearRenderTargetView(direct.rtv.Get(), bg);
    quad(g, direct, uiRect, uiColour, 0, 0, vp, uiGameBs.Get());
    quad(g, direct, overlayRect, overlayColour, 0, 0, vp, overlayGameBs.Get());
    const std::vector<uint8_t> want = readBack(g, direct);

    const uint32_t region[4] = {0, 0, W, H};
    const float uvFull[4] = {0, 0, 1, 1};

    // Case (a): a plain overlay -- no eye-sized input, nothing refuses it.
    // Taken into the layer, after the UI: the composite must match "want".
    {
        Tex frame = makeTex(g, W, H, false), layer = makeTex(g, W, H, false), out = makeTex(g, W, H, true);
        g.ctx->ClearRenderTargetView(frame.rtv.Get(), bg);
        g.ctx->ClearRenderTargetView(layer.rtv.Get(), kUiLayerClear);
        quad(g, layer, uiRect, uiColour, 0, 0, vp, uiLayerBs.Get());
        const bool taken = uiLayerAfterWriteDecide(/*eyeSizedInput=*/false) == UiAfterWriteDecision::kAttempt;
        check(taken, "case (a): a plain overlay write is attempted");
        // The take path's OWN issue is uiLayerBegin/End around the game's
        // draw, at the layer's target -- modelled here the same way every
        // other decided draw in this file is (quad into `layer`, converted
        // blend), since that machinery is proved elsewhere (testRedirect,
        // testComposite). What this proves is which target it lands in.
        if (taken) quad(g, layer, overlayRect, overlayColour, 0, 0, vp, overlayLayerBs.Get());
        composite(g, frame, region, uvFull, layer, out, 0);
        const std::vector<uint8_t> got = readBack(g, out);
        // THE assertion that fails if the fix is reverted (uiLayerAfterWriteDecide
        // always kPostPass, or nothing ever routes the overlay into `layer`):
        // without the take, this falls to the frame branch below instead, and
        // matches "want" only by the same coincidence case (b) is about to rule out.
        check(worstDiff(got, want) <= 3,
              "case (a): a plain overlay after the UI is taken into the layer and composites over it");
    }
    // Case (b): the overlay samples an eye-sized input (a post pass over the
    // frame, or a copy of it) -- left in the frame, under the UI.
    {
        Tex frame = makeTex(g, W, H, false), layer = makeTex(g, W, H, false), out = makeTex(g, W, H, true);
        g.ctx->ClearRenderTargetView(frame.rtv.Get(), bg);
        g.ctx->ClearRenderTargetView(layer.rtv.Get(), kUiLayerClear);
        quad(g, layer, uiRect, uiColour, 0, 0, vp, uiLayerBs.Get());
        const bool taken = uiLayerAfterWriteDecide(/*eyeSizedInput=*/true) == UiAfterWriteDecision::kAttempt;
        check(!taken, "case (b): an overlay sampling an eye-sized input is left as a post pass");
        quad(g, frame, overlayRect, overlayColour, 0, 0, vp, overlayGameBs.Get());
        composite(g, frame, region, uvFull, layer, out, 0);
        const std::vector<uint8_t> got = readBack(g, out);
        check(worstDiff(got, want) > 3,
              "case (b): a post pass stays under the UI -- the composite reproduces the bug, proving "
              "the comparison above has teeth");
    }
    // Case (d): the take path itself refuses a plain write at decide time
    // (here: a second render target bound, uiLayerDecide's kMrt -- the same
    // rule as testAfterUi's pure check). Left in the frame, exactly as (b).
    {
        Tex frame = makeTex(g, W, H, false), layer = makeTex(g, W, H, false), out = makeTex(g, W, H, true);
        g.ctx->ClearRenderTargetView(frame.rtv.Get(), bg);
        g.ctx->ClearRenderTargetView(layer.rtv.Get(), kUiLayerClear);
        quad(g, layer, uiRect, uiColour, 0, 0, vp, uiLayerBs.Get());
        UiLayerDrawFacts f;
        f.family = UiLayerFamily::kAfterUi;
        f.verdictForwards = true;
        f.eyeTarget = true;
        f.ldrView = true;
        f.eye = 0;
        f.armed = true;
        f.blend = UiBlendShape::kPremulOver;
        f.mrt = true;
        const bool taken = uiLayerAfterWriteDecide(/*eyeSizedInput=*/false) == UiAfterWriteDecision::kAttempt &&
                           uiLayerDecide(f) == UiLayerDecision::kRedirect;
        check(!taken, "case (d): a write the take path refuses (a second bound target) is not taken");
        quad(g, frame, overlayRect, overlayColour, 0, 0, vp, overlayGameBs.Get());
        composite(g, frame, region, uvFull, layer, out, 0);
        const std::vector<uint8_t> got = readBack(g, out);
        check(worstDiff(got, want) > 3, "case (d): a refused take stays under the UI, same as a post pass");
    }
    // Case (c), the read exclusion, draws nothing and so has no pixel of its
    // own to check here; it is uiLayerNoteOther's unchanged 'R' branch
    // (testAfterUi's comment says why it has no unit test either).
}

// A render-size change with the layer engaged (the 13:23 flight: the FOV
// trim adopted at the main menu took the door from 3070x3032 to 2458x2824 and
// the game's eye from 1995x1970 to 1597x1835). The old layer no longer
// describes the new frame (the stretched frame); the layer re-made for the
// new door is armed for the first frame after the change, the family's next
// draw is redirected there, and its composite over the new frame is exact.
void testSizeChange(Gpu& g) {
    using namespace uiblend;
    const uint32_t oldW = 40, oldH = 30;  // the door before
    const uint32_t newW = 32, newH = 36;  // after: another shape
    const uint32_t rW = 16, rH = 18;      // the game's eye after, upscaled 2x by the door
    const float uvFull[4] = {0, 0, 1, 1};
    const UiLayerSize before = uiLayerSize(oldW, oldH, 1.0f), after = uiLayerSize(newW, newH, 1.0f);
    check(!uiLayerRegionMatches(newW, newH, uvFull, before.w, before.h) &&
              uiLayerRegionMatches(newW, newH, uvFull, after.w, after.h),
          "a size change: the old layer does not describe the new frame (the stretched frame), the "
          "layer re-made for the new door does");
    // The first frame after the change: the door ran for the new size at
    // sequence 10, so a draw of sequence 11 is armed, into the game's new
    // eye target, which is the region it submits.
    UiLayerDoorState door;
    door.doorSeq = 10;
    door.treatedSeq = 10;
    door.fullW = newW;
    door.fullH = newH;
    UiLayerDrawFacts f;
    f.family = UiLayerFamily::kPanel;
    f.eyeTarget = f.ldrView = true;
    f.eye = 0;
    f.targetMatchesEye = true;
    f.late = uiLayerLateFor(door, 11);
    f.armed = uiLayerArmed(door, 11);
    f.blend = UiBlendShape::kOver;
    check(uiLayerDecide(f) == UiLayerDecision::kRedirect,
          "the menu panel's first draw after the change is redirected (armed by the door's new size)");

    Tex layer = makeTex(g, after.w, after.h, false), frame = makeTex(g, newW, newH, false);
    Tex direct = makeTex(g, newW, newH, false), out = makeTex(g, newW, newH, true);
    const float base[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    g.ctx->ClearRenderTargetView(frame.rtv.Get(), base);
    g.ctx->ClearRenderTargetView(direct.rtv.Get(), base);
    g.ctx->ClearRenderTargetView(layer.rtv.Get(), kUiLayerClear);
    // Edges 0.3 of a layer pixel past a boundary (testRedirect's reason).
    const float left = 5.3f, right = 21.3f, top = 7.3f, bottom = 27.3f;  // new-frame pixels
    const float rect[4] = {-1.0f + 2.0f * left / newW, 1.0f - 2.0f * bottom / newH,
                           -1.0f + 2.0f * right / newW, 1.0f - 2.0f * top / newH};
    const float colour[4] = {0.9f, 0.3f, 0.1f, 1.0f};
    UiBlendRt opaque, conv;
    uiLayerConvertBlend(opaque, &conv);
    D3D11_BLEND_DESC od = uiLayerBlendDesc(opaque);
    ComPtr<ID3D11BlendState> os;
    g.dev->CreateBlendState(&od, &os);
    const D3D11_VIEWPORT full{0, 0, static_cast<float>(newW), static_cast<float>(newH), 0, 1};
    quad(g, direct, rect, colour, 0, 0, full, os.Get());
    // The game draws the panel into its 16x18 eye with this frame's jitter;
    // the redirect maps its viewport onto the new layer and cancels it.
    float jx = 0, jy = 0;
    temporalJitter(3, &jx, &jy);
    const UiLayerMap m = uiLayerMapFromRegion(0, 0, static_cast<float>(rW), static_cast<float>(rH), after.w, after.h);
    float cx = 0, cy = 0;
    uiLayerJitterCancel(jx, jy, m, &cx, &cy);
    UiViewport gv;
    gv.w = static_cast<float>(rW);
    gv.h = static_cast<float>(rH);
    const UiViewport v = uiLayerMapViewport(m, gv, cx, cy);
    const D3D11_VIEWPORT vp{v.x, v.y, v.w, v.h, v.minZ, v.maxZ};
    quad(g, layer, rect, colour, 2.0f * jx / rW, -2.0f * jy / rH, vp, state(g, conv).Get());
    const uint32_t region[4] = {0, 0, newW, newH};
    composite(g, frame, region, uvFull, layer, out, 0);
    const std::vector<uint8_t> got = readBack(g, out), want = readBack(g, direct);
    int worst = 0;
    for (size_t i = 0; i < got.size(); i += 4)
        for (int c = 0; c < 3; ++c) worst = (std::max)(worst, std::abs(int(got[i + c]) - int(want[i + c])));
    check(worst <= 1, "...and composited over the new frame it is the game's own draw, pixel for pixel "
                      "(the new size's map and jitter cancel)");
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
// from the game's (ui_layer_seed.h's Seeder, the DLL's own) with the
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
    edvr_layer_seed::Seeder seeder;
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

#include "ui_seed_census_test.h"
#include "ui_seed_freshness_test.h"
#include "ui_after_ui_test.h"
#include "ui_world_route_test.h"
#include "ui_world_route_wiring_test.h"
#include "ui_intro_curve_wiring_test.h"
#include "ui_panel_budget_test.h"

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("ui_quality_test: dry-run (no device, no files)");
        return 0;
    }
    // --wiring: the source scans of src\d3d11 alone, with their controls (no device). tools\vr_world_route_test\mutants.py runs this on edited
    // copies of vscreen.cpp in a temp tree (the rig reads its sources from the working directory), so every pin is seen to fail on the real file.
    if (argc == 2 && std::strcmp(argv[1], "--wiring") == 0) {
        afterui::testWiring();
        worldroute::testWiring();
        introcurve::testWiring();
        std::printf("ui_quality_test --wiring: %u checks, %u failures\n", g_checks, g_fails);
        return g_fails ? 1 : 0;
    }
    // --hardware: the same checks on the default hardware adapter instead of
    // WARP, by hand (the gate runs --self-test; a build machine may have no GPU).
    const bool hardware = argc == 2 && std::strcmp(argv[1], "--hardware") == 0;
    if (argc != 2 || (std::strcmp(argv[1], "--self-test") != 0 && !hardware)) {
        std::puts("usage: ui_quality_test --self-test | --hardware | --wiring | --dry-run");
        return 2;
    }
    testRenderState();
    testSeedCensusCpu();
    testSeedWriterRule();
    testKey();
    testSize();
    testMap();
    testScissor();
    testBlend();
    testDepthStencil();
    testFootprint();
    testGate();
    testFamilyRule();
    testSupercruiseLines();
    testAfterUi();
    afterui::testIdentity();
    afterui::testRecordedTails();
    afterui::testStationPixels();
    afterui::testGuiFxOffCockpit();
    afterui::testWiring();
    worldroute::testAll();
    worldroute::testWiring();
    introcurve::testWiring();
    testHudParity();
    testChains();
    testPanelScale();
    testFlatPanelScale();
    panelbudget::testAll();
    panelbudget::testWiring();
    Gpu g;
    if (!setup(g, hardware)) {
        check(false, "a device and the production composite shader");
    } else {
        testRedirect(g, 1.0f);
        testRedirect(g, 1.25f);
        testComposite(g);
        testAfterUiOrder(g);
        testDownsample(g);
        testSeededStencil(g);
        testWriteBack(g);
        testSeedCensusGpu(g);
        testPrivateDepthRefreshProof(g);
        testSeedFreshnessGpu(g);
        testSizeChange(g);
    }
    std::printf("ui_quality_test: %u checks, %u failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
