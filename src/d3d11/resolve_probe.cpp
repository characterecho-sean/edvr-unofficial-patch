#include "resolve_probe.h"

#include <windows.h>

#include <cstring>
#include <string>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "exposure_fix.h"   // lookupShaderHash: the shared shader registry
#include "shader_swap.h"

namespace edvr {
namespace {

// The deferred resolve's PIXEL shader, measured 2026-08-30 from a field
// shader dump (advanced.glare_shader_dump) and confirmed by disassembly.
//
// A CONTENT hash, so it survives the game recreating its shaders and only
// changes when the code does. Matching on the pixel shader rather than the
// vertex one is deliberate: the vertex shader is a bare full-screen quad
// that other passes could plausibly share, while this pixel shader is the
// lighting resolve and nothing else.
constexpr uint64_t kResolvePs = 0x7CECABDE34FFBE9EULL;

// The replacement. Its signature matches the shader it stands in for --
// TEXCOORD0/TEXCOORD1 in, two render targets out -- because a mismatch
// there is undefined behaviour rather than a wrong picture.
//
// The pixel address is the original's, instruction for instruction: uv
// scaled by cb2[1].xy, floored, clamped to the last texel. Reproducing it
// rather than sampling with a sampler matters -- the real shader uses
// ld_indexable, and a bilinear tap at a different address would answer a
// slightly different question than the one being asked.
//
// Desk-compiled by tools/compile_variants.py before it ships, both
// variants. The game is never the compiler's first audience.
constexpr char kProbePsHlsl[] = R"HLSL(
Texture2D<float4> gt0 : register(t0);
Texture2D<float4> gt1 : register(t1);
Texture2D<float4> gt2 : register(t2);
Texture2D<float4> gt3 : register(t3);

cbuffer Resolve : register(b2) { float4 c[48]; };

struct VSOut {
    float2 uv  : TEXCOORD0;
    float3 ray : TEXCOORD1;
};

struct PSOut {
    float3 col : SV_TARGET0;
    float  lum : SV_TARGET1;
};

PSOut main(VSOut i) {
    PSOut o;
    o.lum = 0.0;
#ifdef PROBE_WHITE
    // Constant everywhere. The resolve covers the whole screen, so anything
    // that is NOT this colour afterwards is a region where the write was
    // rejected downstream of the pixel shader.
    o.col = float3(1.0, 1.0, 1.0);
#else
    int2 px = (int2)floor(i.uv * c[1].xy);
    px = min(px, (int2)c[1].xy - 1);
    px = max(px, int2(0, 0));
    float4 g1 = gt1.Load(int3(px, 0));
    float4 g2 = gt2.Load(int3(px, 0));
    float4 g3 = gt3.Load(int3(px, 0));
    // red = the flag byte (bit 128 bypasses the multiplier below)
    // green = t2.w, the multiplier that blacks the surface when zero
    // blue = the shadow mask
    o.col = float3(g1.w, g2.w, g3.x);
#endif
    return o;
}
)HLSL";

enum class Mode : uint32_t { kOff, kWhite, kInputs };

Mode                g_mode = Mode::kOff;
ID3D11PixelShader*  g_probePs = nullptr;
bool                g_tried = false;
ID3D11PixelShader*  g_savedPs = nullptr;
bool                g_engaged = false;
bool                g_engagedNoted = false;
uint64_t            g_applied = 0;
char                g_spec[24] = {};

FaultBudget g_budget("resolveProbe", 8);

void releaseShader() {
    if (g_probePs) {
        g_probePs->Release();
        g_probePs = nullptr;
    }
    g_tried = false;
}

}  // namespace

void resolveProbeConfigure(Config& cfg) {
    const std::string spec = cfg.getString("advanced.resolve_probe", "");
    if (spec.length() < sizeof(g_spec) && spec == g_spec) return;
    if (spec.length() >= sizeof(g_spec)) return;
    memcpy(g_spec, spec.c_str(), spec.length() + 1);

    Mode want = Mode::kOff;
    if (spec.empty() || spec == "off" || spec == "0") {
        want = Mode::kOff;
    } else if (spec == "white") {
        want = Mode::kWhite;
    } else if (spec == "inputs") {
        want = Mode::kInputs;
    } else {
        Log::get().note(
            "resolve probe: \"%s\" is not off, white or inputs; refused.",
            spec.c_str());
        want = Mode::kOff;
    }
    if (want == g_mode) return;
    g_mode = want;
    // The compiled shader is per-mode (the variants differ by #define), so
    // a mode change drops it and the next matched draw builds the new one.
    releaseShader();
    g_engagedNoted = false;
    switch (g_mode) {
        case Mode::kWhite:
            Log::get().note(
                "resolve probe ARMED (white): the deferred lighting resolve "
                "draws through a replacement that emits a constant. The pass "
                "covers the whole screen, so the view should go white -- any "
                "region that does NOT is where the pixel shader's output is "
                "being rejected AFTER it runs (stencil, blend mask or "
                "predication). A body-shaped hole in one eye names that "
                "class; a white screen in both eyes clears it.");
            break;
        case Mode::kInputs:
            Log::get().note(
                "resolve probe ARMED (inputs): the resolve emits its own "
                "inputs instead of lighting -- red is the flag byte, GREEN "
                "is t2.w, blue is the shadow mask. t2.w multiplies both the "
                "diffuse and specular terms on this rig's constants, so a "
                "body that shows green in one eye and not the other is the "
                "answer. Neither eye showing green over the body means the "
                "multiplier is not the mechanism.");
            break;
        case Mode::kOff:
            Log::get().note("resolve probe: off, the game's own resolve.");
            break;
    }
}

bool resolveProbeWantsDraws() { return g_mode != Mode::kOff; }

bool resolveProbeOnEyeDraw(ID3D11DeviceContext* ctx) {
    if (g_mode == Mode::kOff || !ctx) return false;
    bool match = false;
    guardedBudget(g_budget, [&] {
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (ps) {
            match = lookupShaderHash(ps) == kResolvePs;
            ps->Release();
        }
    });
    return match;
}

void resolveProbeBegin(ID3D11DeviceContext* ctx) {
    if (g_mode == Mode::kOff || !ctx) return;
    guardedBudget(g_budget, [&] {
        if (!g_probePs && !g_tried) {
            g_tried = true;
            const SwapMacro white[] = {{"PROBE_WHITE", "1"}, {nullptr, nullptr}};
            g_probePs = shaderSwapCompilePs(
                ctx, kProbePsHlsl, sizeof(kProbePsHlsl) - 1, "main",
                "resolve_probe_ps",
                g_mode == Mode::kWhite ? white : nullptr, "resolve probe");
        }
        if (!g_probePs) return;   // shader_swap said why; draw stock
        ctx->PSGetShader(&g_savedPs, nullptr, nullptr);
        ctx->PSSetShader(g_probePs, nullptr, 0);
        g_engaged = true;
        ++g_applied;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "resolve probe: engaged -- the lighting resolve is drawing "
                "through the replacement, restored after every draw. "
                "Counting silently from here.");
        }
    });
}

void resolveProbeEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    guardedBudget(g_budget, [&] {
        // Restore even if the saved pointer is null: null IS a state the
        // game can have been in, and leaving ours bound would paint every
        // later draw with it.
        ctx->PSSetShader(g_savedPs, nullptr, 0);
        if (g_savedPs) {
            g_savedPs->Release();
            g_savedPs = nullptr;
        }
    });
}

void resolveProbeShutdown() {
    releaseShader();
    if (g_savedPs) {
        g_savedPs->Release();
        g_savedPs = nullptr;
    }
}

}  // namespace edvr
