#include "target_sharp.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdlib>
#include <string>

#include "../common/config.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "exposure_fix.h"   // lookupShaderHash
#include "shader_swap.h"
#include "vscreen.h"        // vScreenIsEyeSized

namespace edvr {
namespace {

// The composite's shape, from the census A/B that named it.
constexpr char     kKind = 'X';
constexpr uint32_t kIndices = 6;
constexpr uint32_t kInstances = 1;

// vs E508648660A352B2, measured on game build 332753. Pinnable from the ini
// because a game update can recompile a shader without changing what it
// does, and the alternative -- matching on shape alone -- would catch every
// other 6-index quad that samples one non-eye-sized texture.
constexpr uint64_t kVsHash = 0xE508648660A352B2ull;

// The replacement. Everything after the fetch is ps 63ABD86359B57D01
// transcribed from its own disassembly:
//
//   sample r0, v0.xy, t0, s0
//   add r1.x, r0.w, l(-0.000010) ; lt r1.x, r1.x, l(0) ; discard_nz r1.x
//   mul r0.xyz, r0.xyzx, r0.xyzx        <- the square
//   mov o0.w, r0.w                      <- alpha is the SAMPLED alpha
//   mul r0.xyz, r0.xyzx, cb2[0].xxxx
//   mul r0.xyz, r0.xyzx, cb1[84].zzzz
//   mul r0.xyz, r0.xyzx, cb1[61].yyyy
//   mul r1.xyz, r0.yyyy, cb1[86].xyzx
//   mad r0.xyw, r0.xxxx, cb1[85].xyxz, r1.xyxz
//   mad r0.xyz, r0.zzzz, cb1[87].xyzx, r0.xywx
//   mul o0.xyz, r0.xyzx, cb1[90].yyyy
//
// The two mads are one 3x3 matrix whose COLUMNS are cb1[85], cb1[86] and
// cb1[87] -- written out below as three dot products rather than left as
// the register shuffle, because a shuffle that is subtly wrong looks like a
// colour shift and nothing else.
const char kPsHlsl[] =
    "cbuffer CB1 : register(b1) { float4 g_cb1[91]; };\n"
    "cbuffer CB2 : register(b2) { float4 g_cb2[1]; };\n"
    "Texture2D<float4> g_tex : register(t0);\n"
    "\n"
    "// Catmull-Rom, a = -0.5, stated so it can be checked against a\n"
    "// reference rather than recognised:\n"
    "//   w0 = -0.5t +       t^2 - 0.5t^3\n"
    "//   w1 =  1    - 2.5 * t^2 + 1.5t^3\n"
    "//   w2 =  0.5t + 2.0 * t^2 - 1.5t^3\n"
    "//   w3 =       - 0.5 * t^2 + 0.5t^3\n"
    "float4 fetch(float2 uv)\n"
    "{\n"
    "    float2 size;\n"
    "    g_tex.GetDimensions(size.x, size.y);\n"
    "    int2 lim = int2(size) - 1;\n"
    "    float2 pos = uv * size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 t = pos - base;\n"
    "    float2 t2 = t * t;\n"
    "    float2 t3 = t2 * t;\n"
    "    float2 w0 = -0.5 * t + t2 - 0.5 * t3;\n"
    "    float2 w1 = 1.0 - 2.5 * t2 + 1.5 * t3;\n"
    "    float2 w2 = 0.5 * t + 2.0 * t2 - 1.5 * t3;\n"
    "    float2 w3 = -0.5 * t2 + 0.5 * t3;\n"
    "    float wx[4] = { w0.x, w1.x, w2.x, w3.x };\n"
    "    float wy[4] = { w0.y, w1.y, w2.y, w3.y };\n"
    "    float4 acc = 0.0;\n"
    "    [unroll] for (int j = 0; j < 4; ++j) {\n"
    "        [unroll] for (int i = 0; i < 4; ++i) {\n"
    "            int2 p = clamp(int2(base) + int2(i - 1, j - 1),\n"
    "                           int2(0, 0), lim);\n"
    "            acc += g_tex.Load(int3(p, 0)) * (wx[i] * wy[j]);\n"
    "        }\n"
    "    }\n"
    "    return acc;\n"
    "}\n"
    "\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_TARGET\n"
    "{\n"
    "    float4 s = fetch(uv);\n"
    "    // A cubic kernel overshoots. rgb is SQUARED below, so a negative\n"
    "    // channel would come back as a bright one; alpha gates a discard.\n"
    "    s.rgb = max(s.rgb, 0.0);\n"
    "    s.a = saturate(s.a);\n"
    "\n"
    "    if (s.a - 0.00001 < 0.0) discard;\n"
    "\n"
    "    float3 c = s.rgb * s.rgb;\n"
    "    c *= g_cb2[0].x;\n"
    "    c *= g_cb1[84].z;\n"
    "    c *= g_cb1[61].y;\n"
    "\n"
    "    float3 o;\n"
    "    o.x = c.x * g_cb1[85].x + c.y * g_cb1[86].x + c.z * g_cb1[87].x;\n"
    "    o.y = c.x * g_cb1[85].y + c.y * g_cb1[86].y + c.z * g_cb1[87].y;\n"
    "    o.z = c.x * g_cb1[85].z + c.y * g_cb1[86].z + c.z * g_cb1[87].z;\n"
    "\n"
    "    return float4(o * g_cb1[90].y, s.a);\n"
    "}\n";

bool     g_sharp = false;
bool     g_failed = false;
uint64_t g_vsHash = kVsHash;

ID3D11PixelShader* g_ps = nullptr;

bool               g_engaged = false;
ID3D11PixelShader* g_displaced = nullptr;
uint64_t           g_applied = 0;

ID3D11PixelShader* replacement(ID3D11DeviceContext* ctx) {
    if (g_ps || g_failed) return g_ps;
    g_ps = shaderSwapCompilePs(ctx, kPsHlsl, sizeof(kPsHlsl) - 1, "main",
                               "target_indicator_ps", nullptr,
                               "target indicator");
    if (!g_ps) {
        // shaderSwapCompilePs has already said why. One stand-down for the
        // session: a compile that failed once fails every draw, and a line
        // per draw would be the log.
        g_failed = true;
        Log::get().note("target indicator: the replacement shader could not "
                        "be built, so the indicator is drawn exactly as the "
                        "game draws it for the rest of this session.");
    }
    return g_ps;
}

}  // namespace

void targetSharpConfigure(Config& cfg) {
    const bool was = g_sharp;
    const std::string m = cfg.getString("experimental.target_indicator",
                                        "stock");
    if (m == "stock") {
        g_sharp = false;
    } else if (m == "sharp") {
        g_sharp = true;
    } else {
        g_sharp = false;
        Log::get().note("target_indicator \"%s\" is not stock or sharp; "
                        "running stock.", m.c_str());
    }

    // The pin, for a build where the shader was recompiled. Empty keeps the
    // measured hash; a value that will not parse is refused out loud rather
    // than silently matching nothing.
    const std::string pin = cfg.getString("advanced.target_indicator_vs", "");
    if (pin.empty()) {
        g_vsHash = kVsHash;
    } else {
        char* end = nullptr;
        const uint64_t h = _strtoui64(pin.c_str(), &end, 16);
        if (end && *end == '\0' && h != 0) {
            g_vsHash = h;
        } else {
            g_vsHash = kVsHash;
            Log::get().note("target_indicator_vs \"%s\" is not a hex shader "
                            "hash; the measured one is used instead.",
                            pin.c_str());
        }
    }

    if (was != g_sharp) {
        Log::get().note(
            "target indicator: %s. The selected target's direction indicator "
            "is composited into the eye by one quad through a single bilinear "
            "sample; sharp replaces that pixel shader with the same colour "
            "maths over a Catmull-Rom reconstruction, so the magnification "
            "has cleaner edges. It cannot add detail. Watching for vs "
            "%016llX.",
            g_sharp ? "sharp" : "stock",
            static_cast<unsigned long long>(g_vsHash));
    }
}

bool targetSharpWantsDraws() { return g_sharp && !g_failed; }

bool targetSharpOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                          uint32_t instances) {
    if (!targetSharpWantsDraws()) return false;
    if (kind != kKind || count != kIndices || instances != kInstances) {
        return false;
    }
    // Slot 0 is the interface surface: a Texture2D that is NOT eye-sized.
    // Eye-sized by vScreen's answer rather than an equality, the lesson
    // holo_fix paid for on a rig with a render scale.
    ResourceInfo surf;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &surf) ||
        !surf.isTexture2D || vScreenIsEyeSized(surf.a, surf.b)) {
        return false;
    }
    // Slots 1-3 unbound. The composite reads one texture and nothing else,
    // which is most of what separates it from every other textured quad.
    if (bindingGet(BindSlot::PsSrv1) || bindingGet(BindSlot::PsSrv2) ||
        bindingGet(BindSlot::PsSrv3)) {
        return false;
    }
    // The clincher, and last because it costs a call: nothing hooks
    // VSSetShader, so the shader is read off the context the way the census
    // reads it.
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;
    const uint64_t h = lookupShaderHash(vs);
    vs->Release();
    return h == g_vsHash;
}

void targetSharpBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    ID3D11PixelShader* ps = replacement(ctx);
    if (!ps) return;   // stock behaviour, which the log explained once

    // The game's own pixel shader, off the context: binding_shadow does not
    // carry shaders, and PSSetShader is not hooked.
    ctx->PSGetShader(&g_displaced, nullptr, nullptr);
    ctx->PSSetShader(ps, nullptr, 0);
    g_engaged = true;

    if (++g_applied == 1) {
        Log::get().note("target indicator: sharp engaged -- the composite is "
                        "reconstructed with a Catmull-Rom kernel for exactly "
                        "this draw, and the game's own shader is restored "
                        "after every one.");
    }
}

void targetSharpEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    ID3D11PixelShader* orig = g_displaced;
    g_displaced = nullptr;
    // Null restores an unbind, which is also the truth.
    ctx->PSSetShader(orig, nullptr, 0);
    if (orig) orig->Release();
}

void targetSharpShutdown() {
    if (g_ps) {
        g_ps->Release();
        g_ps = nullptr;
    }
    if (g_displaced) {
        g_displaced->Release();
        g_displaced = nullptr;
    }
}

}  // namespace edvr
