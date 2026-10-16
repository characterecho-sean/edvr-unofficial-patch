// The shield ripple under fix.sharp_weapons, in the build's gate (docs/dlss-blur-elements-2026-10-14.md, "Built, 2026-10-16: the shield refraction after the upscale").
//
//   THE RULE (pure). The exact pair 820E/6EAA is the family kShieldRipple on the HDR branch of the production uiLayerFamilyFor and nothing else is; the ripple's own half (live iff
//   fix.sharp_weapons, a temporal mode, not stood down, not flat); the first-draw checks (gain 1 within 1e-6, a linear s0); D's coverage test; the jitter arithmetic.
//   THE GAME'S SHADER (WARP). The real ps_6EAA (fixtures\ps_6EAA86EFE135B2D4.dxbc, from edvr_logs\shaders) drawn on the UV ramp writes the game's own uv_jit + d:
//   compared with the analytic value for a 2016-wide ramp (the size the flights run) within 1e-6.
//   THE ROUTE (WARP). The whole chain with the production pieces: the production remap and jitter cancel (ui_layer_math.h) for the draw into D, depth GEQUAL
//   against a seeded depth, blend off, the real pixel shader on the ramp; the production door pass (the build's bytecode, the production parameter block) over a
//   copy of the picture. Against the stock path (the same pixel shader reading the HDR scene, then the tonemap) the rippled picture agrees within 2/255 over the
//   bubble, and is untouched outside it and under the occluder. Mutants, each caught by the same comparator that passes the real thing: the jitter not subtracted, the
//   ramp's y flipped, d scaled by the layer/render ratio, no depth seed, an in-place read without the copy, the ripple after the weapons/HUD composite, and 820E
//   skipped with D not admitted.
//   THE WIRING (source scans, each with a control). The ripple before the composite at the door, t0 swapped and put back, D cleared far outside [0, 1], the first-draw
//   checks, the idle release, the vscreen hook, the eye-dump watch, the ini text.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include "../../src/d3d11/shield_ripple.h"
#include "../../src/d3d11/shield_ripple_shaders.h"
#include "../../src/d3d11/ui_layer_math.h"
#include "temporal_shader_bytecode.h"

using namespace edvr;
using Microsoft::WRL::ComPtr;

static unsigned g_checks = 0;
static void check(bool ok, const char* why) {
    ++g_checks;
    if (!ok) throw std::runtime_error(why);
}
static void hr(HRESULT r, const char* what) { check(SUCCEEDED(r), what); }
static std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ============================================================================================ the rule
static void ruleTests() {
    check(shieldRefractPair(kUiVsShieldRefract, kUiPsShieldRefract), "the pair is the pair");
    for (int bit = 0; bit < 64; bit += 7) {
        check(!shieldRefractPair(kUiVsShieldRefract ^ (1ull << bit), kUiPsShieldRefract), "a one-bit change of the vertex shader is not the pair");
        check(!shieldRefractPair(kUiVsShieldRefract, kUiPsShieldRefract ^ (1ull << bit)), "a one-bit change of the pixel shader is not the pair");
    }
    UiFamilyFacts f;
    f.targetKind = 1;
    f.vs = kUiVsShieldRefract;
    f.ps = kUiPsShieldRefract;
    check(uiLayerFamilyFor(f) == UiLayerFamily::kShieldRipple, "the pair is the shield-ripple family on the HDR target");
    f.ps = 0;
    check(uiLayerFamilyFor(f) != UiLayerFamily::kShieldRipple, "the vertex shader alone is not");
    f.ps = kUiPsShieldRefract;
    f.targetKind = 2;
    check(uiLayerFamilyFor(f) != UiLayerFamily::kShieldRipple, "never on the post-tonemap target");
    check(uiLayerFamilyTakesHdr(UiLayerFamily::kShieldRipple) && !uiLayerFamilyIsSceneLines(UiLayerFamily::kShieldRipple), "taken by the HDR route, exempt from the density rule");
    check(std::strcmp(uiLayerFamilyName(UiLayerFamily::kShieldRipple), "shield ripple") == 0, "the family's name is the log's");
    check(shieldRefractVs(kUiVsShieldRefract) && !shieldRefractVs(0x1234ull), "the draw hook reads the pixel shader for the vertex shader");

    // The family gate: the shield ripple follows its own half (fix.sharp_weapons's, with its own stand-down), whatever fix.ui_quality says; the others ignore it.
    for (int q = 0; q < 2; ++q)
        for (int s = 0; s < 2; ++s)
            for (int r = 0; r < 2; ++r) {
                check(uiLayerFamilyEnabled(UiLayerFamily::kShieldRipple, q != 0, s != 0, r != 0) == (r != 0), "the shield ripple follows its own half alone");
                check(uiLayerFamilyEnabled(UiLayerFamily::kWeaponsFire, q != 0, s != 0, r != 0) == (s != 0), "the weapons fire still follows fix.sharp_weapons alone");
                check(uiLayerFamilyEnabled(UiLayerFamily::kFlightHud, q != 0, s != 0, r != 0) == (q != 0), "the interface still follows fix.ui_quality alone");
            }
    check(uiLayerRippleLiveFor(true, true, true, false) && !uiLayerRippleLiveFor(false, true, true, false) && !uiLayerRippleLiveFor(true, false, true, false) &&
              !uiLayerRippleLiveFor(true, true, true, true),
          "the ripple is live for fix.sharp_weapons on, a temporal mode, not stood down, in either profile");
    check(uiLayerMachineryLive(false, true) && !uiLayerMachineryLive(false, false), "the machinery runs for the ripple as it does for the weapons (their union is passed)");
    check(static_cast<int>(UiLayerFamily::kShieldRipple) + 1 == static_cast<int>(UiLayerFamily::kCount), "the family is last in its enum");
    UiLayerDrawFacts d;
    d.family = UiLayerFamily::kShieldRipple;
    d.eyeTarget = true;
    d.crispHdr = true;
    d.eye = 0;
    d.armed = true;
    d.blend = UiBlendShape::kOver;
    d.familyEnabled = false;
    check(uiLayerDecide(d) == UiLayerDecision::kFamilyOff, "a ripple that is off (fix.sharp_weapons off, or stood down) is refused for its key");
    d.familyEnabled = true;
    check(uiLayerDecide(d) == UiLayerDecision::kRedirect, "the 820E facts (blend over, depth tested, no write) are redirected");

    // R1.
    check(shieldRippleGainOk(256.0f, 1.0f / 256.0f), "the measured gain (256 * 1/256) passes");
    check(shieldRippleGainOk(1.0f, 1.0f) && !shieldRippleGainOk(1.0f, 1.001f) && !shieldRippleGainOk(0.0f, 1.0f) && !shieldRippleGainOk(2.0f, 1.0f),
          "a gain other than 1 stands the route down");
    check(shieldRippleGainOk(1.0f, 1.0f + 9e-7f) && !shieldRippleGainOk(1.0f, 1.0f + 3e-6f), "the bound is 1e-6");
    check(shieldRippleFilterOk(D3D11_FILTER_MIN_MAG_MIP_LINEAR) && shieldRippleFilterOk(D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT) &&
              !shieldRippleFilterOk(D3D11_FILTER_MIN_MAG_MIP_POINT) && !shieldRippleFilterOk(D3D11_FILTER_ANISOTROPIC) && !shieldRippleFilterOk(D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR),
          "only a linear min and mag filter passes");
    // D's coverage and the arithmetic.
    check(shieldRippleCovered(0.5f) && shieldRippleCovered(-0.2f) && shieldRippleCovered(1.3f) && !shieldRippleCovered(kShieldRippleNoCover), "coverage: the clear is far outside [0, 1]");
    float j[2];
    shieldRippleJitterUv(0.5f, -0.25f, 2016.0f, 2016.0f, j);
    float o[2];
    const float d2[2] = {0.6f, 0.4f};
    shieldRippleSampleUv(d2, j, o);
    check(std::fabs(j[0] - 0.5f / 2016.0f) < 1e-9f && std::fabs(j[1] + 0.25f / 2016.0f) < 1e-9f && std::fabs(o[0] - (0.6f - j[0])) < 1e-9f && std::fabs(o[1] - (0.4f - j[1])) < 1e-9f,
          "the jitter in uv and the read position D minus jitter");
    check(kShieldRippleIdleFrames == 300, "D is released after 300 idle frames");
}

// ============================================================================================ the wiring
static std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    check(at != std::string::npos, "a control's text is in the source (the control is stale otherwise)");
    return text.replace(at, from.size(), to);
}
static bool has(const std::string& t, const std::string& s) { return t.find(s) != std::string::npos; }
static bool before(const std::string& t, const std::string& a, const std::string& b) {
    const size_t i = t.find(a), j = t.find(b);
    return i != std::string::npos && j != std::string::npos && i < j;
}
static bool wiredLayer(const std::string& t) {
    return
        // the door: the ripple, then the composite
        before(t, "if (g_rippleLive) guarded(\"shieldRipple.door\", [&] { rippleDoor(e, eye, sequence, frame, region, layerUv, ownedFrame, resolved, &rewritten); });",
               "if (!e.srv || !e.rtv || e.compositedSeq == sequence) return handOn();") &&
        // the draw: D cleared far outside [0, 1], blend off, t0 swapped for the ramp and put back
        has(t, "const FLOAT clearD[4] = {kShieldRippleNoCover, kShieldRippleNoCover, 0.0f, 0.0f};") &&
        has(t, "if (converted && rp) layerBlend = g_rip.blend.Get();") && has(t, "ID3D11ShaderResourceView* ramp = g_rip.rampSrv.Get();\n        vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &ramp);") &&
        has(t, "if (g_draw.rippleSwapped) {") && has(t, "g_draw.rippleSwapped = false;") &&
        // the ripple is its own half of the layer, with the jitter subtracted from D at the door
        has(t, "g_rippleLive = uiLayerRippleLiveFor(g_sharpWeapons, g_temporal, g_jitterAsShipped, g_stoodDown || g_rippleStoodDown);") &&
        has(t, "shieldRippleJitterUv(e.dJx, e.dJy, static_cast<float>(e.dRenderW), static_cast<float>(e.dRenderH), p.jitterUv);") &&
        has(t, "if (family == UiLayerFamily::kShieldRipple && f.familyEnabled) f.familyEnabled = rippleR1Allows(ctx);") &&
        // the first-draw checks
        has(t, "shieldRippleGainOk(row[2], row[1])") && has(t, "shieldRippleFilterOk(g_rip.filter)") && has(t, "rippleStandDown(why);") &&
        // the idle release
        has(t, "if (g_rip.frame - e.dLastFrame > kShieldRippleIdleFrames) {") &&
        // no key of its own: the ripple follows fix.sharp_weapons, and a change of that key re-arms a stood-down ripple and asks the first-draw checks again
        !has(t, "advanced.shield_ripple") && has(t, "if (g_rippleNoted && sharp != g_rippleNotedOn) {\n        g_rippleStoodDown = false;\n        g_rip.check = 0;");
}
static bool wiredHook(const std::string& v) {
    return has(v, "(weaponsFireVs(f.vs) || shieldRefractVs(f.vs))") && !has(v, "shieldProbe") && !has(v, "advanced.shield_ripple");
}
static void wiringTests() {
    const std::string layer = readFile("src/d3d11/ui_layer.cpp"), vscreen = readFile("src/d3d11/vscreen.cpp"), snap = readFile("src/d3d11/eye_draw_snapshot.h"), ini = readFile("edvr.ini");
    check(layer.size() > 100000 && vscreen.size() > 100000 && snap.size() > 1000 && ini.size() > 10000, "the sources are readable from the repo root");
    check(wiredLayer(layer), "ui_layer.cpp: the ripple before the composite, t0 swapped and restored, D cleared far outside, the checks, the idle release, the key");
    check(!wiredLayer(replaced(layer, "    if (g_rippleLive) guarded(\"shieldRipple.door\", [&] { rippleDoor(e, eye, sequence, frame, region, layerUv, ownedFrame, resolved, &rewritten); });\n", "")),
          "control: no door call trips the pin");
    check(!wiredLayer(replaced(replaced(layer, "    if (g_rippleLive) guarded(\"shieldRipple.door\", [&] { rippleDoor(e, eye, sequence, frame, region, layerUv, ownedFrame, resolved, &rewritten); });\n", ""), "    if (!e.srv || !e.rtv || e.compositedSeq == sequence) return handOn();", "    if (!e.srv || !e.rtv || e.compositedSeq == sequence) return handOn();\n    if (g_rippleLive) guarded(\"shieldRipple.door\", [&] { rippleDoor(e, eye, sequence, frame, region, layerUv, ownedFrame, resolved, &rewritten); });")),
          "control: the ripple after the composite's early-out and so (on a UI frame) after nothing it must precede trips the pin");
    check(!wiredLayer(replaced(layer, "kShieldRippleNoCover, kShieldRippleNoCover, 0.0f, 0.0f}", "0.0f, 0.0f, 0.0f, 0.0f}")), "control: D cleared to a value inside [0, 1] trips the pin");
    check(!wiredLayer(replaced(layer, "if (converted && rp) layerBlend = g_rip.blend.Get();", "")), "control: the game's blend into D trips the pin");
    check(!wiredLayer(replaced(layer, "g_draw.rippleSwapped = false;", "")), "control: a ramp that is never put back trips the pin");
    check(!wiredLayer(replaced(layer, "shieldRippleJitterUv(e.dJx, e.dJy, static_cast<float>(e.dRenderW), static_cast<float>(e.dRenderH), p.jitterUv);", "p.jitterUv[0] = p.jitterUv[1] = 0.0f;")),
          "control: a jitter that is not subtracted trips the pin");
    check(!wiredLayer(replaced(layer, "if (family == UiLayerFamily::kShieldRipple && f.familyEnabled) f.familyEnabled = rippleR1Allows(ctx);", "")), "control: no first-draw checks trip the pin");
    check(!wiredLayer(replaced(layer, "shieldRippleGainOk(row[2], row[1])", "true")), "control: a gain that is never checked trips the pin");
    check(!wiredLayer(replaced(layer, "if (g_rip.frame - e.dLastFrame > kShieldRippleIdleFrames) {", "if (false) {")), "control: a D that is never released trips the pin");
    check(wiredHook(vscreen), "vscreen.cpp: the pixel shader is read for the 820E vertex shader, and no instrument hook is left");
    check(!wiredHook(vscreen + "\nshieldProbeOfferDraw(self, kind, count, instances, args);"), "control: a surviving instrument hook trips the pin");
    check(!wiredHook(replaced(vscreen, "(weaponsFireVs(f.vs) || shieldRefractVs(f.vs))", "weaponsFireVs(f.vs)")), "control: no pixel shader read for 820E trips the pin");
    check(has(snap, "case 0x820E5C131B99361Dull: // the shield's refracting bubble"), "eye_draw_snapshot.h: the next eye dump watches 820E (VS b0/b1/b2)");
    check(!has(replaced(snap, "case 0x820E5C131B99361Dull: // the shield's refracting bubble\n", ""), "case 0x820E5C131B99361Dull"), "control: an unwatched 820E trips the pin");
    // edvr.ini: no key of its own (sharp_weapons_test holds that fix.sharp_weapons says it covers the ripple).
    check(!has(ini, "shield_ripple"), "edvr.ini: the temporary key is gone");
}

// ============================================================================================ WARP
static const char* kQuadVs = R"HLSL(
cbuffer J : register(b0) { float4 rect; float2 jit; float z; float pad; };
struct V { float3 t0 : TEXCOORD0; float3 t1 : TEXCOORD1; float3 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float4 pos : SV_Position; };
V main(uint id : SV_VertexID) {
    float2 q[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1)};
    float2 g = lerp(rect.xy, rect.zw, q[id]);
    float2 c = g + jit;
    V o;
    o.pos = float4(c, z, 1);
    o.t0 = float3(c, 1);
    o.t1 = float3(0.3 * c.x, 0.2 * c.y, 1.0);
    o.t2 = float3(0.5 * c, 1.2);
    o.t3 = float4(1, 0, 0, 1);
    return o;
}
)HLSL";
static const char* kTriVs = R"HLSL(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p = id == 0 ? float2(-1, -1) : (id == 1 ? float2(-1, 3) : float2(3, -1));
    return float4(p, 0, 1);
}
)HLSL";
static const char* kTonePs = R"HLSL(
Texture2D<float4> H : register(t0);
float4 main(float4 p : SV_Position) : SV_Target {
    float3 c = H.Load(int3(int2(p.xy), 0)).rgb;
    return float4(pow(c / (1.0 + c), 1.0 / 2.2), 1);
}
)HLSL";
static const char* kFillPs = R"HLSL(
struct V { float3 t0 : TEXCOORD0; float3 t1 : TEXCOORD1; float3 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float4 pos : SV_Position; };
float4 main(V v) : SV_Target { return float4(0.2, 0.9, 0.3, 1); }
)HLSL";
// An in-place variant of the door pass (the mutant "an in-place read without the copy"): it reads the picture it is writing.
static const char* kInPlaceCs = R"HLSL(
cbuffer P : register(b0) { int4 region; float4 layerUv; float2 jitterUv; uint2 outSize; };
Texture2D<float2> D : register(t1);
RWTexture2D<unorm float4> Frame : register(u0);
[numthreads(32, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    float2 local = (float2(id.xy) + 0.5) / float2(outSize);
    float2 uvEye = layerUv.xy + local * (layerUv.zw - layerUv.xy);
    uint dw, dh; D.GetDimensions(dw, dh);
    float2 d = D.Load(int3(int2(min(uvEye * float2(dw, dh), float2(dw - 1, dh - 1))), 0));
    if (d.x < -1000.0) return;
    float2 q = (d - jitterUv) * float2(outSize) - 0.5;
    int2 i0 = int2(floor(q));
    float2 f = frac(q);
    int2 hi = int2(outSize) - 1;
    float4 a = Frame[clamp(i0, int2(0, 0), hi)], b = Frame[clamp(i0 + int2(1, 0), int2(0, 0), hi)];
    float4 c = Frame[clamp(i0 + int2(0, 1), int2(0, 0), hi)], e = Frame[clamp(i0 + int2(1, 1), int2(0, 0), hi)];
    Frame[id.xy] = lerp(lerp(a, b, f.x), lerp(c, e, f.x), f.y);
}
)HLSL";

struct Rgb { float r = 0, g = 0, b = 0; };

// The scene: smooth, with enough slope for a half-pixel error to show in the 8-bit picture.
static Rgb scene(float u, float v) {
    const float s = std::sin(6.2831853f * 3.0f * u) * std::cos(6.2831853f * 2.0f * v);
    return {1.1f + 0.55f * s, 0.9f + 0.45f * std::sin(6.2831853f * 2.0f * u + 1.3f * v), 0.7f + 0.4f * std::cos(6.2831853f * 3.0f * v + u)};
}
static float tone(float c) { return std::pow(c / (1.0f + c), 1.0f / 2.2f); }
static uint8_t q8(float v) { return static_cast<uint8_t>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f)); }

static uint16_t toHalf(float v) { return DirectX::PackedVector::XMConvertFloatToHalf(v); }

struct Hit {  // the constants of the real pixel shader
    float cb2[9][4];
};
static Hit makeHit() {
    Hit h{};
    h.cb2[0][0] = 0.04f; h.cb2[0][1] = 1.5f; h.cb2[0][2] = 0.7f; h.cb2[0][3] = 1.0f;
    h.cb2[1][0] = 1.2f;  h.cb2[1][1] = 0.8f; h.cb2[1][2] = 0.6f; h.cb2[1][3] = 1.1f;
    const float pos[3][3] = {{0.5f, 0.2f, 1.0f}, {-0.4f, 0.3f, 0.9f}, {0.1f, -0.6f, 1.1f}};
    for (int i = 0; i < 3; ++i) {
        h.cb2[3 + i][0] = pos[i][0]; h.cb2[3 + i][1] = pos[i][1]; h.cb2[3 + i][2] = pos[i][2];
    }
    const int32_t n = 3;
    std::memcpy(&h.cb2[7][3], &n, 4);
    h.cb2[8][0] = 3.0f; h.cb2[8][1] = 1.0f;
    return h;
}
static const float kNormal[3] = {0.7f, 0.4f, 0.9f};   // the constant t1 texel (all taps read it)

// The disassembled shader, in float (ps_6EAA86EFE135B2D4, instructions 0-52): d = normalize(n).xy * cb2[0].x for the interpolated inputs.
static void analyticD(const Hit& h, float cx, float cy, float d[2]) {
    const float v1[3] = {0.3f * cx, 0.2f * cy, 1.0f};
    const float v2[3] = {0.5f * cx, 0.5f * cy, 1.2f};
    const float v3[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float* c2 = &h.cb2[0][0];
    auto cb = [&](int row, int k) { return c2[row * 4 + k]; };
    const float x1[5][2] = {{cb(0, 3), cb(0, 2)}, {cb(1, 1), cb(1, 0)}, {cb(1, 3), cb(1, 2)}, {cb(2, 1), cb(2, 0)}, {cb(2, 3), cb(2, 2)}};
    float B[3] = {v1[1] * v3[2] - v1[2] * v3[1], v1[2] * v3[0] - v1[0] * v3[2], v1[0] * v3[1] - v1[1] * v3[0]};
    if (!(0.0f < v3[3])) for (float& b : B) b = -b;
    const float r0w = cb(8, 0) - cb(8, 1);
    float n[3] = {0, 0, 1};
    for (int i = 0; i < 3; ++i) {
        const float a = x1[i][0];
        const float b = x1[i][1] * r0w + cb(8, 1);
        const float hp[3] = {cb(3 + i, 0), cb(3 + i, 1), cb(3 + i, 2)};
        const float r4[3] = {v2[0] - hp[0], v2[1] - hp[1], v2[2] - hp[2]};
        const float r2[3] = {r4[0] / b, r4[1] / b, r4[2] / b};
        const float dx = r2[0] * v3[0] + r2[1] * v3[1] + r2[2] * v3[2];
        const float dy = r2[0] * B[0] + r2[1] * B[1] + r2[2] * B[2];
        (void)dx; (void)dy;
        const float len = 1.0f / std::sqrt(hp[0] * hp[0] + hp[1] * hp[1] + hp[2] * hp[2]);
        const float nh[3] = {hp[0] * len, hp[1] * len, hp[2] * len};
        const float wgt = std::min(std::max(nh[0] * v1[0] + nh[1] * v1[1] + nh[2] * v1[2], 0.0f), 1.0f);
        const float fade = std::min(std::max(a * 0.5f, 0.0f), 1.0f);
        for (int k = 0; k < 3; ++k) {
            const float t = kNormal[k] * 2.0f - 1.0f;
            n[k] += (t + fade * -t) * wgt;
        }
    }
    const float il = 1.0f / std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    d[0] = n[0] * il * cb(0, 0);
    d[1] = n[1] * il * cb(0, 0);
}

struct Fx {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> info;
    ComPtr<ID3D11VertexShader> quadVs, triVs;
    ComPtr<ID3D11PixelShader> realPs, tonePs, fillPs;
    ComPtr<ID3D11ComputeShader> doorCs, inPlaceCs;
    ComPtr<ID3D11SamplerState> lin;
    ComPtr<ID3D11RasterizerState> rsNone;
    ComPtr<ID3D11BlendState> blendOff;
    ComPtr<ID3D11DepthStencilState> dsGeq, dsWrite;
    ComPtr<ID3D11ShaderResourceView> normalSrv;
    ComPtr<ID3D11Buffer> cb1, cb2;
    bool debugLayer = false;
    ComPtr<ID3D11SamplerState> pointS;
    ID3D11SamplerState* samplerOverride = nullptr;   // t0's sampler for the next bubble draw (null: linear)

    ComPtr<ID3DBlob> compile(const char* text, const char* profile) {
        ComPtr<ID3DBlob> code, err;
        const HRESULT r = D3DCompile(text, std::strlen(text), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &err);
        if (FAILED(r) && err) std::fprintf(stderr, "%s\n", static_cast<const char*>(err->GetBufferPointer()));
        hr(r, "the rig's shader compiles");
        return code;
    }
    ComPtr<ID3D11Texture2D> tex(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, const void* data, UINT pitch) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA sd{data, pitch, 0};
        ComPtr<ID3D11Texture2D> t;
        hr(dev->CreateTexture2D(&d, data ? &sd : nullptr, &t), "a rig texture");
        return t;
    }
    ComPtr<ID3D11ShaderResourceView> srv(ID3D11Texture2D* t, DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        d.Format = fmt == DXGI_FORMAT_UNKNOWN ? td.Format : fmt; d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> s;
        hr(dev->CreateShaderResourceView(t, &d, &s), "a rig SRV");
        return s;
    }
    ComPtr<ID3D11RenderTargetView> rtv(ID3D11Texture2D* t) {
        ComPtr<ID3D11RenderTargetView> r;
        hr(dev->CreateRenderTargetView(t, nullptr, &r), "a rig RTV");
        return r;
    }
    ComPtr<ID3D11Buffer> constants(const void* data, UINT bytes) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (bytes + 15u) & ~15u; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        std::vector<uint8_t> pad(bd.ByteWidth, 0);
        std::memcpy(pad.data(), data, bytes);
        D3D11_SUBRESOURCE_DATA sd{pad.data(), 0, 0};
        ComPtr<ID3D11Buffer> b;
        hr(dev->CreateBuffer(&bd, &sd, &b), "a rig constant buffer");
        return b;
    }
    explicit Fx(bool hardware) {
        D3D_FEATURE_LEVEL level;
        const D3D_DRIVER_TYPE type = hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
        UINT flags = hardware ? 0 : D3D11_CREATE_DEVICE_DEBUG;
        HRESULT r = D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        if (FAILED(r)) {
            flags = 0;
            r = D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        }
        hr(r, "a WARP device");
        debugLayer = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
        if (debugLayer) dev.As(&info);
        auto qv = compile(kQuadVs, "vs_5_0"), tv = compile(kTriVs, "vs_5_0"), tp = compile(kTonePs, "ps_5_0"), fp = compile(kFillPs, "ps_5_0"), ip = compile(kInPlaceCs, "cs_5_0");
        hr(dev->CreateVertexShader(qv->GetBufferPointer(), qv->GetBufferSize(), nullptr, &quadVs), "quad vs");
        hr(dev->CreateVertexShader(tv->GetBufferPointer(), tv->GetBufferSize(), nullptr, &triVs), "tri vs");
        hr(dev->CreatePixelShader(tp->GetBufferPointer(), tp->GetBufferSize(), nullptr, &tonePs), "tone ps");
        hr(dev->CreatePixelShader(fp->GetBufferPointer(), fp->GetBufferSize(), nullptr, &fillPs), "fill ps");
        hr(dev->CreateComputeShader(ip->GetBufferPointer(), ip->GetBufferSize(), nullptr, &inPlaceCs), "in-place cs");
        // the build's bytes
        hr(dev->CreateComputeShader(kShieldRippleDoorBytecode, sizeof(kShieldRippleDoorBytecode), nullptr, &doorCs), "the build's door pass");
        // the game's pixel shader, from the fixture
        const std::string bytes = readFile("tools/shield_ripple_test/fixtures/ps_6EAA86EFE135B2D4.dxbc");
        check(bytes.size() == 2004, "the pixel shader fixture is the 2004 bytes the flight dumped");
        hr(dev->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &realPs), "the game's ps_6EAA from the fixture");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr(dev->CreateSamplerState(&sd, &lin), "sampler");
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        hr(dev->CreateSamplerState(&sd, &pointS), "point sampler");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        hr(dev->CreateRasterizerState(&rd, &rsNone), "raster");
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = FALSE; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        hr(dev->CreateBlendState(&bd, &blendOff), "blend");
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE; dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        hr(dev->CreateDepthStencilState(&dd, &dsGeq), "depth GEQUAL");
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        hr(dev->CreateDepthStencilState(&dd, &dsWrite), "depth write");
        // t1: a constant normal texel; cb1: the frame constants with row 90 = (0, 1/256, 256, 0); cb2: the hits.
        const uint16_t t1[4] = {toHalf(kNormal[0]), toHalf(kNormal[1]), toHalf(kNormal[2]), toHalf(1.0f)};
        // half precision rounds the texel: the analytic value must use what the GPU reads
        ComPtr<ID3D11Texture2D> nt = tex(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, std::array<float, 4>{kNormal[0], kNormal[1], kNormal[2], 1.0f}.data(), 16);
        (void)t1;
        normalSrv = srv(nt.Get());
        std::vector<float> c1(91 * 4, 0.0f);
        c1[90 * 4 + 1] = 1.0f / 256.0f;
        c1[90 * 4 + 2] = 256.0f;
        cb1 = constants(c1.data(), 91 * 16);
        const Hit h = makeHit();
        cb2 = constants(h.cb2, sizeof(h.cb2));
    }
    ComPtr<ID3D11Buffer> quadConstants(float x0, float y0, float x1, float y1, float jx, float jy, float z) {
        const float v[8] = {x0, y0, x1, y1, jx, jy, z, 0.0f};
        return constants(v, sizeof(v));
    }
    // The game's draw of a rect, with the real pixel shader: t0 as given, into `target`, depth as given.
    void drawBubble(ID3D11RenderTargetView* target, ID3D11DepthStencilView* dsv, ID3D11ShaderResourceView* t0, const D3D11_VIEWPORT& vp, ID3D11Buffer* quad) {
        ID3D11RenderTargetView* rt = target;
        ctx->OMSetRenderTargets(1, &rt, dsv);
        ctx->OMSetBlendState(blendOff.Get(), nullptr, 0xFFFFFFFFu);
        ctx->OMSetDepthStencilState(dsv ? dsGeq.Get() : nullptr, 0);
        ctx->RSSetState(rsNone.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(quadVs.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &quad);
        ctx->PSSetShader(realPs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {t0, normalSrv.Get()};
        ctx->PSSetShaderResources(0, 2, srvs);
        ID3D11SamplerState* s = samplerOverride ? samplerOverride : lin.Get();
        ctx->PSSetSamplers(0, 1, &s);
        ID3D11Buffer* cbs[3] = {nullptr, cb1.Get(), cb2.Get()};
        ctx->PSSetConstantBuffers(0, 3, cbs);
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(6, 0);
        ID3D11ShaderResourceView* none[2] = {};
        ctx->PSSetShaderResources(0, 2, none);
    }
    void fullscreen(ID3D11PixelShader* ps, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* in, uint32_t w, uint32_t h) {
        ID3D11RenderTargetView* rt = target;
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        ctx->OMSetBlendState(blendOff.Get(), nullptr, 0xFFFFFFFFu);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->RSSetState(rsNone.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(triVs.Get(), nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &in);
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
    }
    template <class T>
    std::vector<T> read(ID3D11Texture2D* t, uint32_t perPixel) {
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> stage;
        hr(dev->CreateTexture2D(&d, nullptr, &stage), "staging");
        ctx->CopyResource(stage.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "map");
        std::vector<T> out(static_cast<size_t>(d.Width) * d.Height * perPixel);
        for (uint32_t y = 0; y < d.Height; ++y) std::memcpy(&out[static_cast<size_t>(y) * d.Width * perPixel], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, d.Width * perPixel * sizeof(T));
        ctx->Unmap(stage.Get(), 0);
        return out;
    }
};

// The UV ramp: texel (i, j) holds ((i + 0.5) / W, (j + 0.5) / H); `flipY` is the mutant.
static ComPtr<ID3D11Texture2D> makeRamp(Fx& f, uint32_t w, uint32_t h, bool flipY) {
    std::vector<float> t(static_cast<size_t>(w) * h * 2);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float* p = &t[(static_cast<size_t>(y) * w + x) * 2];
            p[0] = (x + 0.5f) / w;
            p[1] = flipY ? 1.0f - (y + 0.5f) / h : (y + 0.5f) / h;
        }
    return f.tex(w, h, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, t.data(), w * 8);
}

// ---- the game's shader on the ramp against the analytic uv + d
static void shaderTests(Fx& f, bool hardware) {
    const uint32_t R = 2016, L = 64;
    const float jx = 0.37f, jy = -0.21f;
    auto ramp = makeRamp(f, R, R, false);
    auto rampSrv = f.srv(ramp.Get());
    auto dTex = f.tex(L, L, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    auto dRtv = f.rtv(dTex.Get());
    const float clear[4] = {kShieldRippleNoCover, kShieldRippleNoCover, 0, 0};
    f.ctx->ClearRenderTargetView(dRtv.Get(), clear);
    const float jn[2] = {2.0f * jx / R, -2.0f * jy / R};
    auto quad = f.quadConstants(-0.6f, -0.5f, 0.7f, 0.6f, jn[0], jn[1], 0.5f);
    // the production remap and jitter cancel (game viewport = the render target, onto the layer)
    const UiLayerMap m = uiLayerMapFromRegion(0, 0, static_cast<float>(R), static_cast<float>(R), L, L);
    float cx = 0, cy = 0;
    uiLayerJitterCancel(jx, jy, m, &cx, &cy);
    UiViewport gv;
    gv.w = static_cast<float>(R);
    gv.h = static_cast<float>(R);
    const UiViewport o = uiLayerMapViewport(m, gv, cx, cy);
    const D3D11_VIEWPORT vp{o.x, o.y, o.w, o.h, o.minZ, o.maxZ};
    f.drawBubble(dRtv.Get(), nullptr, rampSrv.Get(), vp, quad.Get());
    const std::vector<float> D = f.read<float>(dTex.Get(), 2);
    // The same draw with t0 POINT sampled: the shader's own arithmetic, free of the texture unit's filter precision. The result is the centre of the texel that
    // holds uv_jit + d, so it must equal the analytic position's nearest texel centre except where that position sits within 1e-3 of a texel edge.
    f.ctx->ClearRenderTargetView(dRtv.Get(), clear);
    f.samplerOverride = f.pointS.Get();
    f.drawBubble(dRtv.Get(), nullptr, rampSrv.Get(), vp, quad.Get());
    f.samplerOverride = nullptr;
    const std::vector<float> Dp = f.read<float>(dTex.Get(), 2);
    const Hit h = makeHit();
    double arith = 0.0;
    unsigned arithN = 0;
    for (uint32_t y = 0; y < L; ++y)
        for (uint32_t x = 0; x < L; ++x) {
            const float gx = (x + 0.5f) / L * 2.0f - 1.0f, gy = 1.0f - (y + 0.5f) / L * 2.0f;
            const float edge = 1.0f / L;
            if (gx < -0.6f + edge || gx > 0.7f - edge || gy < -0.5f + edge || gy > 0.6f - edge) continue;
            const float cxn = gx + jn[0], cyn = gy + jn[1];
            float d[2];
            analyticD(h, cxn, cyn, d);
            const double ux = (cxn * 0.5 + 0.5 + d[0]) * R, uy = (1.0 - (cyn * 0.5 + 0.5) + d[1]) * R;
            const double fx = ux - std::floor(ux), fy = uy - std::floor(uy);
            if (fx < 1e-3 || fx > 1.0 - 1e-3 || fy < 1e-3 || fy > 1.0 - 1e-3) continue;
            const double ex = (std::floor(ux) + 0.5) / R, ey = (std::floor(uy) + 0.5) / R;
            arith = std::max({arith, std::fabs(Dp[(y * L + x) * 2] - ex), std::fabs(Dp[(y * L + x) * 2 + 1] - ey)});
            ++arithN;
        }
    std::printf("the game's ps_6EAA, t0 point-sampled on a %ux%u ramp: %u pixels, max |D - nearest texel centre of (uv_jit + d)| = %.3g (a texel is %.3g)\n", R, R, arithN, arith,
                1.0 / R);
    check(arithN > 800 && arith <= 1e-6, "the real pixel shader computes uv_jit + d to within 1e-3 of a texel (its arithmetic, free of the filter)");
    double worst = 0.0;
    unsigned covered = 0, bad = 0;
    for (uint32_t y = 0; y < L; ++y)
        for (uint32_t x = 0; x < L; ++x) {
            const float gx = (x + 0.5f) / L * 2.0f - 1.0f, gy = 1.0f - (y + 0.5f) / L * 2.0f;
            const float cxn = gx + jn[0], cyn = gy + jn[1];
            const bool inside = gx > -0.6f && gx < 0.7f && gy > -0.5f && gy < 0.6f;
            const float dx = D[(y * L + x) * 2], dy = D[(y * L + x) * 2 + 1];
            if (!inside) {
                if (shieldRippleCovered(dx)) ++bad;
                continue;
            }
            // pixels whose centre is within half a pixel of the quad's edge may fall either way
            const float edge = 1.0f / L;
            if (gx < -0.6f + edge || gx > 0.7f - edge || gy < -0.5f + edge || gy > 0.6f - edge) continue;
            ++covered;
            if (!shieldRippleCovered(dx)) { ++bad; continue; }
            float d[2];
            analyticD(h, cxn, cyn, d);
            const float ex = cxn * 0.5f + 0.5f + d[0], ey = 1.0f - (cyn * 0.5f + 0.5f) + d[1];
            worst = std::max({worst, static_cast<double>(std::fabs(dx - ex)), static_cast<double>(std::fabs(dy - ey))});
        }
    // The ramp alone: with no hits the shader's d is exactly zero, so D must be uv_jit (this isolates the ramp's read-back from the shader's arithmetic).
    double rampOnly = 0.0;
    {
        Hit none = makeHit();
        const int32_t zero = 0;
        std::memcpy(&none.cb2[7][3], &zero, 4);
        ComPtr<ID3D11Buffer> saved = f.cb2;
        f.cb2 = f.constants(none.cb2, sizeof(none.cb2));
        f.ctx->ClearRenderTargetView(dRtv.Get(), clear);
        f.drawBubble(dRtv.Get(), nullptr, rampSrv.Get(), vp, quad.Get());
        f.cb2 = saved;
        const std::vector<float> D0 = f.read<float>(dTex.Get(), 2);
        for (uint32_t y = 0; y < L; ++y)
            for (uint32_t x = 0; x < L; ++x) {
                const float gx = (x + 0.5f) / L * 2.0f - 1.0f, gy = 1.0f - (y + 0.5f) / L * 2.0f;
                const float edge = 1.0f / L;
                if (gx < -0.6f + edge || gx > 0.7f - edge || gy < -0.5f + edge || gy > 0.6f - edge) continue;
                const float cxn = gx + jn[0], cyn = gy + jn[1];
                rampOnly = std::max({rampOnly, static_cast<double>(std::fabs(D0[(y * L + x) * 2] - (cxn * 0.5f + 0.5f))),
                                     static_cast<double>(std::fabs(D0[(y * L + x) * 2 + 1] - (1.0f - (cyn * 0.5f + 0.5f))))});
            }
    }
    std::printf("the ramp alone (no hits, d = 0): max |D - uv_jit| = %.3g\n", rampOnly);
    const double rampOnlyCopy = rampOnly;
    (void)rampOnlyCopy;
    // t0 linearly filtered, as the game binds it: the ramp reads back its own coordinate to within the texture unit's fractional-weight precision. Hardware keeps
    // 8 fractional bits (an error of at most a texel / 512 = 1e-6 at 2016); WARP's is coarser, so there the bound is a tenth of a texel.
    const double bound = hardware ? 1e-6 : 0.1 / R;
    std::printf("the game's ps_6EAA on a %ux%u ramp, linear: %u interior pixels, max |D - (uv_jit + d)| = %.3g (bound on this adapter %.3g), %u uncovered or stray\n", R, R,
                covered, worst, bound, bad);
    check(covered > 1000 && bad == 0, "D covers the bubble's interior and nothing outside it");
    check(worst <= bound, hardware ? "on hardware the real pixel shader on the ramp matches the analytic uv + d within 1e-6"
                                   : "on WARP the real pixel shader on the ramp matches the analytic uv + d within a tenth of a texel");
    check(rampOnly <= bound, "the ramp alone reads back within the same bound");
}

// ---- the whole route against the stock path
struct Route {
    uint32_t R = 64, L = 96;
    float jx = 0.5f, jy = -0.5f;
    bool noJitterSubtract = false, flipRamp = false, scaleD = false, noSeed = false, inPlace = false, afterComposite = false, noD = false;
    bool notOwned = false;       // the flat profile's picture: no UAV, not EDVR's to rewrite; the pass rewrites EDVR's copy of it
    bool noBaseCopy = false;     // (mutant, with notOwned) the copy is not started as the whole picture
    bool rewriteOriginal = false;// (mutant, with notOwned) the game's own picture is rewritten instead of the copy (here: a UAV-capable stand-in)
};
struct Result {
    int worstBubble = 0;    // max |out - expected| over the picture, 8-bit
    int worstOverlay = 0;
    unsigned bubblePixels = 0;
    int wx = -1, wy = -1, wc = 0, got = 0, want = 0, wkind = 0;   // where the worst bubble error was: pixel, channel, values, 1 bubble 2 elsewhere
    unsigned over1 = 0, over2 = 0, countBubble = 0;               // bubble samples more than 1 and more than 2 off
    unsigned origChanged = 0;                                     // (notOwned) bytes of the original picture the pass changed
};

static Result runRoute(Fx& f, const Route& r) {
    const uint32_t R = r.R, L = r.L;
    // the jittered render of the scene (the pre-DLSS HDR copy t0 holds): the point at pixel centre p is the scene at p/R - jitter
    std::vector<uint16_t> hdr(static_cast<size_t>(R) * R * 4);
    for (uint32_t y = 0; y < R; ++y)
        for (uint32_t x = 0; x < R; ++x) {
            const Rgb c = scene((x + 0.5f) / R - r.jx / R, (y + 0.5f) / R - r.jy / R);
            uint16_t* p = &hdr[(static_cast<size_t>(y) * R + x) * 4];
            p[0] = toHalf(c.r); p[1] = toHalf(c.g); p[2] = toHalf(c.b); p[3] = toHalf(1.0f);
        }
    auto hdrTex = f.tex(R, R, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, hdr.data(), R * 8);
    auto hdrSrv = f.srv(hdrTex.Get());
    // the picture the door receives: the scene tonemapped at the layer's resolution (an ideal reconstruction), in 8 bits
    std::vector<uint8_t> frame8(static_cast<size_t>(L) * L * 4);
    for (uint32_t y = 0; y < L; ++y)
        for (uint32_t x = 0; x < L; ++x) {
            const Rgb c = scene((x + 0.5f) / L, (y + 0.5f) / L);
            uint8_t* p = &frame8[(static_cast<size_t>(y) * L + x) * 4];
            p[0] = q8(tone(c.r)); p[1] = q8(tone(c.g)); p[2] = q8(tone(c.b)); p[3] = 255;
        }
    auto frame = f.tex(L, L, DXGI_FORMAT_R8G8B8A8_UNORM,
                       (r.notOwned && !r.rewriteOriginal) ? D3D11_BIND_SHADER_RESOURCE : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE),
                       frame8.data(), L * 4);
    // the texture the pass writes and the weapons/HUD composite lands on: the frame itself (EDVR's own), or EDVR's copy of the game's picture
    ComPtr<ID3D11Texture2D> rewrittenTex;
    if (r.notOwned && !r.rewriteOriginal) rewrittenTex = f.tex(L, L, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    ID3D11Texture2D* destTex = rewrittenTex ? rewrittenTex.Get() : frame.Get();
    // the layer route's draw into D
    auto ramp = makeRamp(f, R, R, r.flipRamp);
    auto rampSrv = f.srv(ramp.Get());
    auto dTex = f.tex(L, L, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    auto dRtv = f.rtv(dTex.Get());
    const float clear[4] = {kShieldRippleNoCover, kShieldRippleNoCover, 0, 0};
    f.ctx->ClearRenderTargetView(dRtv.Get(), clear);
    const float jn[2] = {2.0f * r.jx / R, -2.0f * r.jy / R};
    const float bx0 = -0.55f, by0 = -0.45f, bx1 = 0.6f, by1 = 0.55f;      // the bubble
    const float ox0 = 0.05f, oy0 = -0.25f, ox1 = 0.45f, oy1 = 0.2f;       // an occluder in front of part of it
    auto bubbleQuad = f.quadConstants(bx0, by0, bx1, by1, jn[0], jn[1], 0.5f);
    auto occQuad = f.quadConstants(ox0, oy0, ox1, oy1, jn[0], jn[1], 0.9f);
    // the seeded depth: the game's depth at the layer's size (the occluder at 0.9, the rest at 0)
    auto depthTex = f.tex(L, L, DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL, nullptr, 0);
    ComPtr<ID3D11DepthStencilView> dsv;
    hr(f.dev->CreateDepthStencilView(depthTex.Get(), nullptr, &dsv), "dsv");
    f.ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    const UiLayerMap m = uiLayerMapFromRegion(0, 0, static_cast<float>(R), static_cast<float>(R), L, L);
    float cx = 0, cy = 0;
    uiLayerJitterCancel(r.jx, r.jy, m, &cx, &cy);
    UiViewport gv;
    gv.w = static_cast<float>(R);
    gv.h = static_cast<float>(R);
    const UiViewport o = uiLayerMapViewport(m, gv, cx, cy);
    const D3D11_VIEWPORT vp{o.x, o.y, o.w, o.h, o.minZ, o.maxZ};
    if (!r.noSeed) {
        // the seed: the scene's depth drawn into the layer's copy (here the occluder only; the game's other geometry is farther than the bubble)
        auto colour = f.tex(L, L, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, nullptr, 0);
        auto crt = f.rtv(colour.Get());
        ID3D11RenderTargetView* rt = crt.Get();
        f.ctx->OMSetRenderTargets(1, &rt, dsv.Get());
        f.ctx->OMSetDepthStencilState(f.dsWrite.Get(), 0);
        f.ctx->OMSetBlendState(f.blendOff.Get(), nullptr, 0xFFFFFFFFu);
        f.ctx->RSSetState(f.rsNone.Get());
        f.ctx->IASetInputLayout(nullptr);
        f.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        f.ctx->VSSetShader(f.quadVs.Get(), nullptr, 0);
        ID3D11Buffer* q = occQuad.Get();
        f.ctx->VSSetConstantBuffers(0, 1, &q);
        f.ctx->PSSetShader(f.fillPs.Get(), nullptr, 0);
        f.ctx->RSSetViewports(1, &vp);
        f.ctx->Draw(6, 0);
    }
    if (!r.noD) f.drawBubble(dRtv.Get(), dsv.Get(), rampSrv.Get(), vp, bubbleQuad.Get());

    // the stock reference: the same pixel shader on the HDR scene, at the layer's resolution through the same remap, then the tonemap
    auto refHdr = f.tex(L, L, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    auto refRtv = f.rtv(refHdr.Get());
    const float zero[4] = {};
    f.ctx->ClearRenderTargetView(refRtv.Get(), zero);
    f.drawBubble(refRtv.Get(), nullptr, hdrSrv.Get(), vp, bubbleQuad.Get());
    auto refTex = f.tex(L, L, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, nullptr, 0);
    auto refTone = f.rtv(refTex.Get());
    f.fullscreen(f.tonePs.Get(), refTone.Get(), f.srv(refHdr.Get()).Get(), L, L);
    const std::vector<uint8_t> ref8 = f.read<uint8_t>(refTex.Get(), 4);

    // D, possibly mutated: d scaled by the layer/render ratio
    if (r.scaleD) {
        std::vector<float> D = f.read<float>(dTex.Get(), 2);
        for (uint32_t y = 0; y < L; ++y)
            for (uint32_t x = 0; x < L; ++x) {
                float* p = &D[(static_cast<size_t>(y) * L + x) * 2];
                if (!shieldRippleCovered(p[0])) continue;
                const float gx = (x + 0.5f) / L * 2.0f - 1.0f, gy = 1.0f - (y + 0.5f) / L * 2.0f;
                const float u = (gx + jn[0]) * 0.5f + 0.5f, v = 1.0f - ((gy + jn[1]) * 0.5f + 0.5f);
                const float s = static_cast<float>(L) / R;
                p[0] = u + (p[0] - u) * s;
                p[1] = v + (p[1] - v) * s;
            }
        f.ctx->UpdateSubresource(dTex.Get(), 0, nullptr, D.data(), L * 8, 0);
    }

    // the weapons/HUD composite, stood for by a rectangle of solid colour written into the frame
    const D3D11_BOX overlayBox{static_cast<UINT>(L * 0.30f), static_cast<UINT>(L * 0.30f), 0, static_cast<UINT>(L * 0.70f), static_cast<UINT>(L * 0.55f), 1};
    std::vector<uint8_t> overlay(static_cast<size_t>(overlayBox.right - overlayBox.left) * (overlayBox.bottom - overlayBox.top) * 4);
    for (size_t i = 0; i < overlay.size(); i += 4) { overlay[i] = 255; overlay[i + 1] = 40; overlay[i + 2] = 200; overlay[i + 3] = 255; }
    const UINT overlayPitch = (overlayBox.right - overlayBox.left) * 4;
    auto paintOverlay = [&] { f.ctx->UpdateSubresource(destTex, 0, &overlayBox, overlay.data(), overlayPitch, 0); };
    if (r.afterComposite) paintOverlay();

    // the door: the production pass over a copy of the picture
    ShieldRippleParams p{};
    p.region[2] = static_cast<int32_t>(L);
    p.region[3] = static_cast<int32_t>(L);
    p.layerUv[2] = p.layerUv[3] = 1.0f;
    if (!r.noJitterSubtract) shieldRippleJitterUv(r.jx, r.jy, static_cast<float>(R), static_cast<float>(R), p.jitterUv);
    p.outSize[0] = p.outSize[1] = L;
    auto cbParams = f.constants(&p, sizeof(p));
    auto copy = f.tex(L, L, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    if (!r.inPlace) f.ctx->CopyResource(copy.Get(), frame.Get());
    ComPtr<ID3D11UnorderedAccessView> uav;
    hr(f.dev->CreateUnorderedAccessView(destTex, nullptr, &uav), "uav");
    if (rewrittenTex && !r.noBaseCopy) f.ctx->CopyResource(rewrittenTex.Get(), frame.Get());   // the copy starts as the whole picture
    auto copySrv = f.srv(copy.Get());
    auto dSrv = f.srv(dTex.Get());
    ID3D11ShaderResourceView* srvs[2] = {r.inPlace ? nullptr : copySrv.Get(), dSrv.Get()};
    f.ctx->CSSetShader(r.inPlace ? f.inPlaceCs.Get() : f.doorCs.Get(), nullptr, 0);
    ID3D11Buffer* cbp = cbParams.Get();
    f.ctx->CSSetConstantBuffers(0, 1, &cbp);
    ID3D11SamplerState* smp = f.lin.Get();
    f.ctx->CSSetSamplers(0, 1, &smp);
    f.ctx->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* u = uav.Get();
    f.ctx->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
    f.ctx->Dispatch((L + 31) / 32, (L + 7) / 8, 1);
    ID3D11ShaderResourceView* nullSrv[2] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    f.ctx->CSSetShaderResources(0, 2, nullSrv);
    f.ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    // the game's own picture, as the pass left it (before the composite lands anywhere)
    unsigned origChanged = 0;
    if (r.notOwned) {
        const std::vector<uint8_t> after = f.read<uint8_t>(frame.Get(), 4);
        for (size_t i = 0; i < after.size(); ++i)
            if (after[i] != frame8[i]) ++origChanged;
    }
    if (!r.afterComposite) paintOverlay();
    const std::vector<uint8_t> out8 = f.read<uint8_t>(destTex, 4);

    // expected: the stock picture over the bubble that the occluder does not hide, the unrippled picture elsewhere, the overlay on top
    Result res;
    res.origChanged = origChanged;
    for (uint32_t y = 0; y < L; ++y)
        for (uint32_t x = 0; x < L; ++x) {
            const float gx = (x + 0.5f) / L * 2.0f - 1.0f, gy = 1.0f - (y + 0.5f) / L * 2.0f;
            const float edge = 1.5f / L;    // a pixel or so of the quad's edges is rasterisation's call
            const bool inBubble = gx > bx0 + edge && gx < bx1 - edge && gy > by0 + edge && gy < by1 - edge;
            const bool nearBubbleEdge = !inBubble && gx > bx0 - edge && gx < bx1 + edge && gy > by0 - edge && gy < by1 + edge;
            const bool inOccWide = gx > ox0 - edge && gx < ox1 + edge && gy > oy0 - edge && gy < oy1 + edge;     // the occluder and a pixel or so around it
            const bool inOccCore = gx > ox0 + edge && gx < ox1 - edge && gy > oy0 + edge && gy < oy1 - edge;     // wholly behind it

            const bool inOverlay = x >= overlayBox.left && x < overlayBox.right && y >= overlayBox.top && y < overlayBox.bottom;
            if (nearBubbleEdge || (inOccWide && !inOccCore)) continue;
            for (int c = 0; c < 3; ++c) {
                const size_t i = (static_cast<size_t>(y) * L + x) * 4 + c;
                int want;
                bool inEdge = false;
                if (inOverlay) want = overlay[((y - overlayBox.top) * (overlayBox.right - overlayBox.left) + (x - overlayBox.left)) * 4 + c];
                else if (inBubble && !inOccWide) want = ref8[i];
                else want = frame8[i];
                if (inEdge) continue;
                const int e = std::abs(static_cast<int>(out8[i]) - want);
                if (inOverlay) res.worstOverlay = std::max(res.worstOverlay, e);
                else {
                    if (inBubble && !inOccWide) { ++res.countBubble; if (e > 1) ++res.over1; if (e > 2) ++res.over2; }
                    if (e > res.worstBubble) { res.wx = static_cast<int>(x); res.wy = static_cast<int>(y); res.wc = c; res.got = out8[i]; res.want = want; res.wkind = (inBubble && !inOccWide) ? 1 : 2; }
                    res.worstBubble = std::max(res.worstBubble, e);
                }
            }
        }
    for (uint32_t y = 0; y < L; ++y)
        for (uint32_t x = 0; x < L; ++x) {
            const float gx = (x + 0.5f) / L * 2.0f - 1.0f, gy = 1.0f - (y + 0.5f) / L * 2.0f;
            if (gx > bx0 + 0.05f && gx < bx1 - 0.05f && gy > by0 + 0.05f && gy < by1 - 0.05f) ++res.bubblePixels;
        }
    return res;
}

static void routeTests(Fx& f) {
    const Route good;
    const Result g = runRoute(f, good);
    std::printf("route (render 64, layer 96, jitter %.2f,%.2f px): %u bubble pixels, max |rippled - stock| %d/255 over the bubble and the rest, %d/255 on the overlay\n",
                static_cast<double>(good.jx), static_cast<double>(good.jy), g.bubblePixels, g.worstBubble, g.worstOverlay);
    std::printf("  worst at (%d,%d) channel %d: got %d, expected %d (%s); bubble samples off by more than 1: %u, by more than 2: %u of %u\n", g.wx, g.wy, g.wc, g.got, g.want,
                g.wkind == 1 ? "inside the bubble" : "outside it", g.over1, g.over2, g.countBubble);
    check(g.bubblePixels > 1500, "the bubble covers a good part of the picture");
    check(g.worstBubble <= 2, "after the upscale agrees with the stock path within 2/255 over the bubble, and leaves the rest and the occluded part untouched");
    check(g.worstOverlay == 0, "the weapons/HUD composite lands on top of the rippled picture untouched");

    // The flat profile's picture: not EDVR's to rewrite, so the pass rewrites EDVR's copy of it and hands that on; the game's own picture is left as it was.
    {
        Route nr;
        nr.notOwned = true;
        const Result n = runRoute(f, nr);
        std::printf("route, a picture that is not EDVR's own (the flat profile's): max |rippled - stock| %d/255 over the bubble and the rest, %d/255 on the overlay, %u bytes of the "
                    "original changed\n", n.worstBubble, n.worstOverlay, n.origChanged);
        check(n.worstBubble <= 2 && n.worstOverlay == 0, "a picture that is not EDVR's own: EDVR's rewritten copy agrees with the stock path as the in-place route does");
        check(n.origChanged == 0, "...and the game's own picture is not touched");
        Route nb = nr;
        nb.noBaseCopy = true;
        const Result nbr = runRoute(f, nb);
        std::printf("mutant %-44s max error %3d (bubble and rest) %3d (overlay)\n", "the copy not started as the whole picture", nbr.worstBubble, nbr.worstOverlay);
        check(nbr.worstBubble > 3, "...a copy that does not start as the whole picture is caught");
        Route ro = nr;
        ro.rewriteOriginal = true;
        const Result ror = runRoute(f, ro);
        std::printf("mutant %-44s %u bytes of the original changed\n", "the game's own picture rewritten", ror.origChanged);
        check(ror.origChanged > 100, "...rewriting the game's own picture instead of the copy is caught");
    }

    struct Mut { const char* name; Route r; };
    std::vector<Mut> muts;
    { Route r; r.noJitterSubtract = true; muts.push_back({"the jitter not subtracted", r}); }
    { Route r; r.flipRamp = true; muts.push_back({"the ramp's y flipped", r}); }
    { Route r; r.scaleD = true; muts.push_back({"d scaled by the layer/render ratio", r}); }
    { Route r; r.noSeed = true; muts.push_back({"no depth seed", r}); }
    { Route r; r.inPlace = true; muts.push_back({"an in-place read without the copy", r}); }
    { Route r; r.afterComposite = true; muts.push_back({"the ripple after the weapons/HUD composite", r}); }
    { Route r; r.noD = true; muts.push_back({"820E skipped with D not admitted", r}); }
    for (const Mut& m : muts) {
        const Result x = runRoute(f, m.r);
        std::printf("mutant %-44s max error %3d (bubble and rest) %3d (overlay)\n", m.name, x.worstBubble, x.worstOverlay);
        check(std::max(x.worstBubble, x.worstOverlay) > 3, (std::string("mutant caught: ") + m.name).c_str());
    }
    // the same comparator passes the real thing at another jitter, another ratio (a different headset's render size)
    Route other;
    other.R = 80;
    other.L = 96;
    other.jx = -0.3f;
    other.jy = 0.45f;
    const Result o = runRoute(f, other);
    std::printf("route (render 80, layer 96, jitter %.2f,%.2f): max error %d, overlay %d\n", static_cast<double>(other.jx), static_cast<double>(other.jy), o.worstBubble, o.worstOverlay);
    check(o.worstBubble <= 2 && o.worstOverlay == 0, "and at another render size and jitter");
}

static void infoCheck(Fx& f) {
    if (!f.info) {
        std::puts("shield_ripple_test: no debug layer on this machine (the passes ran without it)");
        return;
    }
    const UINT64 count = f.info->GetNumStoredMessages();
    unsigned bad = 0;
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(f.info->GetMessage(i, nullptr, &size)) || !size) continue;
        std::vector<uint8_t> raw(size);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(raw.data());
        if (FAILED(f.info->GetMessage(i, msg, &size))) continue;
        if (msg->Severity == D3D11_MESSAGE_SEVERITY_ERROR || msg->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
            ++bad;
            std::fprintf(stderr, "debug layer: %s\n", msg->pDescription);
        }
    }
    check(bad == 0, "the debug layer reports no error or corruption");
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("shield_ripple_test: dry-run (no device, no files)");
        return 0;
    }
    const bool hardware = argc > 1 && std::strcmp(argv[1], "--hardware-self-test") == 0;
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0 && !hardware) {
        std::fprintf(stderr, "usage: --self-test | --dry-run | --hardware-self-test (the machine's own adapter; not a gate)\n");
        return 2;
    }
    try {
        ruleTests();
        wiringTests();
        Fx f(hardware);
        shaderTests(f, hardware);
        routeTests(f);
        infoCheck(f);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "shield_ripple_test: FAIL: %s (after %u checks)\n", e.what(), g_checks);
        return 1;
    }
    std::printf("shield_ripple_test: %u checks passed.\n", g_checks);
    return 0;
}
