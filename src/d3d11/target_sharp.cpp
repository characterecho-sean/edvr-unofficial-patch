#include "target_sharp.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../common/config.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "exposure_fix.h"   // lookupShaderHash
#include "fsr_hlsl_gen.h"   // AMD's ffx_a.h and ffx_fsr1.h, as string chunks
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

// Shared by both resamplers: the bindings, a Catmull-Rom fetch, and the
// game's own colour maths.
//
// The colour maths is ps 63ABD86359B57D01 transcribed from its disassembly:
//
//   mul r0.xyz, r0.xyzx, r0.xyzx        <- the square
//   mul r0.xyz, r0.xyzx, cb2[0].xxxx
//   mul r0.xyz, r0.xyzx, cb1[84].zzzz
//   mul r0.xyz, r0.xyzx, cb1[61].yyyy
//   mul r1.xyz, r0.yyyy, cb1[86].xyzx
//   mad r0.xyw, r0.xxxx, cb1[85].xyxz, r1.xyxz
//   mad r0.xyz, r0.zzzz, cb1[87].xyzx, r0.xywx
//   mul o0.xyz, r0.xyzx, cb1[90].yyyy
//
// The two mads are one 3x3 matrix whose COLUMNS are cb1[85], cb1[86] and
// cb1[87] -- written out as three dot products rather than left as the
// register shuffle, because a shuffle that is subtly wrong looks like a
// colour shift and nothing else. The emitted tail was diffed against the
// game's before this ever shipped.
const char kPsCommon[] =
    "cbuffer CB1 : register(b1) { float4 g_cb1[91]; };\n"
    "cbuffer CB2 : register(b2) { float4 g_cb2[1]; };\n"
    "Texture2D<float4> g_tex : register(t0);\n"
    "SamplerState g_smp : register(s0);\n"
    "\n"
    "// Catmull-Rom, a = -0.5, stated so it can be checked against a\n"
    "// reference rather than recognised:\n"
    "//   w0 = -0.5t +       t^2 - 0.5t^3\n"
    "//   w1 =  1    - 2.5 * t^2 + 1.5t^3\n"
    "//   w2 =  0.5t + 2.0 * t^2 - 1.5t^3\n"
    "//   w3 =       - 0.5 * t^2 + 0.5t^3\n"
    "float4 fetchCubic(float2 uv)\n"
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
    "float4 shade(float3 rgb, float a)\n"
    "{\n"
    "    float3 c = rgb * rgb;\n"
    "    c *= g_cb2[0].x;\n"
    "    c *= g_cb1[84].z;\n"
    "    c *= g_cb1[61].y;\n"
    "    float3 o;\n"
    "    o.x = c.x * g_cb1[85].x + c.y * g_cb1[86].x + c.z * g_cb1[87].x;\n"
    "    o.y = c.x * g_cb1[85].y + c.y * g_cb1[86].y + c.z * g_cb1[87].y;\n"
    "    o.z = c.x * g_cb1[85].z + c.y * g_cb1[86].z + c.z * g_cb1[87].z;\n"
    "    return float4(o * g_cb1[90].y, a);\n"
    "}\n";

// The fallback, for a rig where EASU will not compile. A cubic is a smaller
// win than an edge-adaptive kernel on line art and a much larger one than
// the single bilinear tap the game does.
const char kPsCubicMain[] =
    "float4 main(float2 uv : TEXCOORD0) : SV_TARGET\n"
    "{\n"
    "    float4 s = fetchCubic(uv);\n"
    "    // A cubic kernel overshoots. rgb is SQUARED in shade(), so a\n"
    "    // negative channel would come back as a bright one; alpha gates a\n"
    "    // discard.\n"
    "    s.rgb = max(s.rgb, 0.0);\n"
    "    s.a = saturate(s.a);\n"
    "    if (s.a - 0.00001 < 0.0) discard;\n"
    "    return shade(s.rgb, s.a);\n"
    "}\n";

// EASU, and RCAS over it. Everything the filtering itself does is AMD's;
// what is here is the callbacks their header asks the calling shader to
// provide, plus the constants.
//
// WHY THE CONSTANTS ARE BUILT PER PIXEL. FsrEasuCon bakes an output-pixel to
// source-pixel map into con0, which assumes the two grids are related by a
// fixed scale -- true for a fullscreen upscale, false here: this quad is 3D
// cockpit geometry, so its UV is perspective-interpolated and not affine in
// screen space. Zeroing con0.xy drops the integer output position out of
// FsrEasuF's first line and con0.zw carries the source position directly,
// which is the same arithmetic it would have done. con1..con3 are
// FsrEasuCon's own terms with inputSize = the surface, term for term.
//
// RCAS reads a five-tap neighbourhood of the UPSCALED image, which does not
// exist as a texture here -- so FsrRcasLoadF evaluates EASU again at the
// neighbouring output pixel, reached by stepping the UV along its own screen
// derivatives. Five EASU evaluations for one output pixel, on one small quad
// per eye.
const char kPsEasuMain[] =
    "static float2 g_size, g_rcp, g_uv, g_dx, g_dy;\n"
    "\n"
    "AF4 FsrEasuRF(AF2 p) { return g_tex.GatherRed(g_smp, p); }\n"
    "AF4 FsrEasuGF(AF2 p) { return g_tex.GatherGreen(g_smp, p); }\n"
    "AF4 FsrEasuBF(AF2 p) { return g_tex.GatherBlue(g_smp, p); }\n"
    "\n"
    "float3 easuAt(float2 uv)\n"
    "{\n"
    "    float2 pp = uv * g_size - 0.5;\n"
    "    AU4 con0 = AU4(0, 0, asuint(pp.x), asuint(pp.y));\n"
    "    AU4 con1 = AU4(asuint(g_rcp.x), asuint(g_rcp.y),\n"
    "                   asuint(g_rcp.x), asuint(-g_rcp.y));\n"
    "    AU4 con2 = AU4(asuint(-g_rcp.x), asuint(2.0 * g_rcp.y),\n"
    "                   asuint(g_rcp.x), asuint(2.0 * g_rcp.y));\n"
    "    AU4 con3 = AU4(0, asuint(4.0 * g_rcp.y), 0, 0);\n"
    "    AF3 c;\n"
    "    FsrEasuF(c, AU2(0, 0), con0, con1, con2, con3);\n"
    "    return c;\n"
    "}\n"
    "\n"
    "AF4 FsrRcasLoadF(ASU2 p)\n"
    "{\n"
    "    return AF4(easuAt(g_uv + p.x * g_dx + p.y * g_dy), 1.0);\n"
    "}\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_TARGET\n"
    "{\n"
    "    g_tex.GetDimensions(g_size.x, g_size.y);\n"
    "    g_rcp = 1.0 / g_size;\n"
    "    g_uv = uv;\n"
    "    // One output pixel's step in UV, taken BEFORE the discard so the\n"
    "    // derivative is read while the quad is still whole.\n"
    "    g_dx = ddx(uv);\n"
    "    g_dy = ddy(uv);\n"
    "\n"
    "    // Alpha is a coverage mask and EASU has no alpha path, so it keeps\n"
    "    // the cubic. It gates the same discard the game's shader gates.\n"
    "    float a = saturate(fetchCubic(uv).a);\n"
    "    if (a - 0.00001 < 0.0) discard;\n"
    "\n"
    "    AF3 c;\n"
    "#if EDVR_RCAS\n"
    "    FsrRcasF(c.r, c.g, c.b, AU2(0, 0),\n"
    "             AU4(asuint(float(EDVR_RCAS_SHARP)), 0, 0, 0));\n"
    "#else\n"
    "    c = easuAt(uv);\n"
    "#endif\n"
    "    return shade(max(c, 0.0), a);\n"
    "}\n";

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

std::string joinChunks(const char* const* chunks) {
    std::string out;
    for (const char* const* c = chunks; *c; ++c) out += *c;
    return out;
}

enum class Mode { kOff, kCubic, kEasu };

bool     g_sharp = false;
bool     g_failed = false;
uint64_t g_vsHash = kVsHash;
float    g_sharpen = 0.25f;   // RCAS stops; negative = RCAS off
Mode     g_running = Mode::kOff;

ID3D11PixelShader* g_ps = nullptr;
float              g_psSharpen = 0.0f;
bool               g_psHadRcas = false;

bool               g_engaged = false;
ID3D11PixelShader* g_displaced = nullptr;
uint64_t           g_applied = 0;

ID3D11PixelShader* replacement(ID3D11DeviceContext* ctx) {
    const bool wantRcas = g_sharpen >= 0.0f;
    if (g_ps && g_psHadRcas == wantRcas && g_psSharpen == g_sharpen) {
        return g_ps;
    }
    if (g_failed) return nullptr;
    if (g_ps) {
        g_ps->Release();
        g_ps = nullptr;
    }

    // AMD's own unit is STOPS of sharpness reduction, so 0 is the sharpest
    // and the linear value the shader wants is exp2(-stops).
    char sharpBuf[32] = "1.0";
    if (wantRcas) {
        _snprintf_s(sharpBuf, sizeof(sharpBuf), _TRUNCATE, "%.8f",
                    static_cast<double>(powf(2.0f, -g_sharpen)));
    }
    const SwapMacro macros[] = {{"EDVR_RCAS", wantRcas ? "1" : "0"},
                                {"EDVR_RCAS_SHARP", sharpBuf},
                                {nullptr, nullptr}};

    const std::string easu = std::string(kGpuPrologue) +
                             joinChunks(kFfxAChunks) +
                             "#define FSR_EASU_F 1\n" +
                             "#define FSR_RCAS_F 1\n" +
                             joinChunks(kFfxFsr1Chunks) + kPsCommon +
                             kPsEasuMain;
    g_ps = shaderSwapCompilePs(ctx, easu.c_str(), easu.size(), "main",
                               "target_indicator_easu", macros,
                               "target indicator");
    g_running = g_ps ? Mode::kEasu : Mode::kOff;
    if (!g_ps) {
        Log::get().note("target indicator: EASU would not compile; falling "
                        "back to the bicubic, a smaller win than FSR on line "
                        "art and a much larger one than the bilinear tap the "
                        "game does.");
        const std::string cubic = std::string(kPsCommon) + kPsCubicMain;
        g_ps = shaderSwapCompilePs(ctx, cubic.c_str(), cubic.size(), "main",
                                   "target_indicator_cubic", nullptr,
                                   "target indicator");
        g_running = g_ps ? Mode::kCubic : Mode::kOff;
    }
    if (!g_ps) {
        // shaderSwapCompilePs has already said why. One stand-down for the
        // session: a compile that failed once fails every draw, and a line
        // per draw would be the log.
        g_failed = true;
        Log::get().note("target indicator: no resampler would compile, so the "
                        "indicator is drawn exactly as the game draws it for "
                        "the rest of this session.");
        return nullptr;
    }
    g_psHadRcas = wantRcas;
    g_psSharpen = g_sharpen;
    Log::get().note(
        "target indicator: running %s%s.",
        g_running == Mode::kEasu ? "EASU (AMD's own, vendored)"
                                 : "the bicubic (Catmull-Rom)",
        (g_running == Mode::kEasu && wantRcas) ? ", then RCAS-sharpened" : "");
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

    // AMD's unit: stops of sharpness reduction, 0 the sharpest. "off" runs
    // EASU with no sharpening pass at all.
    const std::string s = cfg.getString("advanced.target_indicator_sharpen",
                                        "0.25");
    if (s == "off") {
        g_sharpen = -1.0f;
    } else {
        g_sharpen = static_cast<float>(atof(s.c_str()));
        if (g_sharpen < 0.0f) g_sharpen = 0.0f;
        if (g_sharpen > 2.0f) g_sharpen = 2.0f;
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
            "maths over an edge-adaptive reconstruction (AMD's EASU), "
            "sharpening %s. It cannot add detail. Watching for vs %016llX.",
            g_sharp ? "sharp" : "stock",
            g_sharpen >= 0.0f ? "on" : "off",
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
                        "reconstructed for exactly this draw, and the game's "
                        "own shader is restored after every one.");
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
