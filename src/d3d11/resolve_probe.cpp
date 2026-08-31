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

// The shader modes replace what the pass COMPUTES; the state modes leave
// the game's own shader alone and change what is allowed to SURVIVE it.
//
// The second group exists because the first answered its question. With
// `white` the resolve emits a constant -- reading no texture, no G-buffer,
// nothing -- and the affected eye's body is still black. Nothing about the
// shader's inputs can do that. The output is being rejected after it runs.
//
// It is rejected per-PIXEL, not per-draw: only the body goes dark, and the
// same draw lights the rest of the frame. That leaves the depth test and
// the stencil test, and depth is already known to be written in that eye --
// the disc occludes the star field behind it. Stencil is the one nothing
// here has ever recorded.
enum class Mode : uint32_t { kOff, kWhite, kInputs, kNoStencil, kNoDepth,
                             kNoBoth };

bool isShaderMode(Mode m) { return m == Mode::kWhite || m == Mode::kInputs; }
bool isStateMode(Mode m) {
    return m == Mode::kNoStencil || m == Mode::kNoDepth || m == Mode::kNoBoth;
}

// TWO independent halves, because the test that is still missing needs
// both at once.
//
// `white` (a constant-emitting shader, the game's own state) left the body
// black, and `noboth` (the game's own shader, both per-pixel tests off) left
// it black too. Each of those rules out one thing while the OTHER is still
// in play: the first still ran under the game's tests, and the second still
// ran the game's own shader. The combination -- a shader that cannot output
// black, with nothing able to reject it per pixel -- has never been run, and
// it is the one that separates "the write never lands" from "the write lands
// and something later paints over it".
//
// Spelled `white+noboth` in the ini; either half alone still works.
Mode                g_shaderMode = Mode::kOff;
Mode                g_stateMode = Mode::kOff;
ID3D11PixelShader*  g_probePs = nullptr;
bool                g_tried = false;
ID3D11PixelShader*  g_savedPs = nullptr;
bool                g_psEngaged = false;
// The derived depth-stencil state, built ONCE from the game's own at the
// first matched draw and reused. Derived rather than authored: this must
// change one field and leave every other comparison, mask and pass-op as
// the game set it, or a "the body appears" result would be worthless --
// it would prove only that some other depth-stencil configuration draws
// differently, which is not in doubt.
ID3D11DepthStencilState* g_derived = nullptr;
bool                     g_derivedTried = false;
ID3D11DepthStencilState* g_savedDs = nullptr;
UINT                     g_savedRef = 0;
bool                     g_dsEngaged = false;
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

void releaseState() {
    if (g_derived) {
        g_derived->Release();
        g_derived = nullptr;
    }
    g_derivedTried = false;
}

}  // namespace

void resolveProbeConfigure(Config& cfg) {
    const std::string spec = cfg.getString("advanced.resolve_probe", "");
    if (spec.length() < sizeof(g_spec) && spec == g_spec) return;
    if (spec.length() >= sizeof(g_spec)) return;
    memcpy(g_spec, spec.c_str(), spec.length() + 1);

    // Words joined by '+', in any order: at most one shader word and one
    // state word. A whole spec is refused rather than partly applied, so a
    // typo cannot quietly run half the test somebody thinks they asked for.
    Mode wantShader = Mode::kOff;
    Mode wantState = Mode::kOff;
    bool ok = true;
    if (!(spec.empty() || spec == "off" || spec == "0")) {
        size_t at = 0;
        while (at <= spec.size()) {
            const size_t plus = spec.find('+', at);
            const std::string w =
                spec.substr(at, plus == std::string::npos ? std::string::npos
                                                          : plus - at);
            if (w == "white" || w == "inputs") {
                if (wantShader != Mode::kOff) ok = false;
                wantShader = (w == "white") ? Mode::kWhite : Mode::kInputs;
            } else if (w == "nostencil" || w == "nodepth" || w == "noboth") {
                if (wantState != Mode::kOff) ok = false;
                wantState = (w == "nostencil") ? Mode::kNoStencil
                          : (w == "nodepth")   ? Mode::kNoDepth
                                               : Mode::kNoBoth;
            } else {
                ok = false;
            }
            if (plus == std::string::npos) break;
            at = plus + 1;
        }
    }
    if (!ok) {
        Log::get().note(
            "resolve probe: \"%s\" is not understood. One shader word "
            "(white, inputs) and/or one state word (nostencil, nodepth, "
            "noboth), joined with '+' -- \"white+noboth\" runs both. off is "
            "off. Refused; the game draws its own.",
            spec.c_str());
        wantShader = Mode::kOff;
        wantState = Mode::kOff;
    }
    if (wantShader == g_shaderMode && wantState == g_stateMode) return;
    g_shaderMode = wantShader;
    g_stateMode = wantState;
    // The compiled shader is per-mode (the variants differ by #define), so
    // a mode change drops it and the next matched draw builds the new one.
    // The derived state goes with it for the same reason.
    releaseShader();
    releaseState();
    g_engagedNoted = false;
    switch (g_shaderMode) {
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
        default:
            break;
    }
    switch (g_stateMode) {
        case Mode::kNoStencil:
        case Mode::kNoDepth:
        case Mode::kNoBoth:
            Log::get().note(
                "resolve probe ARMED (%s): the resolve draws with its "
                "depth-stencil state copied from the game's and %s disabled "
                "-- every other comparison, mask and pass-op left exactly as "
                "the game set it, so a body that appears can only be the test "
                "that was turned off. Restored after each draw.",
                g_stateMode == Mode::kNoStencil ? "nostencil"
                    : g_stateMode == Mode::kNoDepth ? "nodepth" : "noboth",
                g_stateMode == Mode::kNoStencil ? "the STENCIL test"
                    : g_stateMode == Mode::kNoDepth ? "the DEPTH test"
                                                    : "both tests");
            break;
        default:
            break;
    }
    if (g_shaderMode == Mode::kOff && g_stateMode == Mode::kOff) {
        Log::get().note("resolve probe: off, the game's own resolve.");
    } else if (g_shaderMode != Mode::kOff && g_stateMode != Mode::kOff) {
        Log::get().note(
            "resolve probe: BOTH halves are armed. The replacement shader "
            "cannot output black and the per-pixel tests cannot reject it, "
            "so a body-shaped dark region now means the write lands and "
            "something LATER paints over it -- which is a different fault "
            "from the one this has been chasing, and points at the draws "
            "after the resolve rather than at the resolve itself.");
    }
}

bool resolveProbeWantsDraws() {
    return g_shaderMode != Mode::kOff || g_stateMode != Mode::kOff;
}

bool resolveProbeOnEyeDraw(ID3D11DeviceContext* ctx) {
    if (!resolveProbeWantsDraws() || !ctx) return false;
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
    if (!resolveProbeWantsDraws() || !ctx) return;
    guardedBudget(g_budget, [&] {
        if (isShaderMode(g_shaderMode)) {
            if (!g_probePs && !g_tried) {
                g_tried = true;
                const SwapMacro white[] = {{"PROBE_WHITE", "1"},
                                           {nullptr, nullptr}};
                g_probePs = shaderSwapCompilePs(
                    ctx, kProbePsHlsl, sizeof(kProbePsHlsl) - 1, "main",
                    "resolve_probe_ps",
                    g_shaderMode == Mode::kWhite ? white : nullptr,
                    "resolve probe");
            }
            if (!g_probePs) return;   // shader_swap said why; draw stock
            ctx->PSGetShader(&g_savedPs, nullptr, nullptr);
            ctx->PSSetShader(g_probePs, nullptr, 0);
            g_psEngaged = true;
        }
        if (isStateMode(g_stateMode)) {
            // The game's own state is read first and DERIVED from, every
            // time this is armed fresh, because authoring one from nothing
            // would change comparisons and masks nobody asked about and
            // make a positive result unattributable.
            ctx->OMGetDepthStencilState(&g_savedDs, &g_savedRef);
            if (!g_derived && !g_derivedTried) {
                g_derivedTried = true;
                D3D11_DEPTH_STENCIL_DESC d{};
                if (g_savedDs) {
                    g_savedDs->GetDesc(&d);
                } else {
                    // No state bound is the D3D11 default: depth on, LESS,
                    // write all, stencil off. Spelled out rather than left
                    // zeroed, which would silently mean depth OFF.
                    d.DepthEnable = TRUE;
                    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
                    d.DepthFunc = D3D11_COMPARISON_LESS;
                    d.StencilEnable = FALSE;
                }
                // The game's own values are captured BEFORE ours are applied.
                //
                // The first version of this logged the desc after modifying
                // it, under the heading "the game's resolve state reads" --
                // so the line reported OUR state wearing the game's name, and
                // the two runs disagreed with each other in a way that took
                // reading the source to untangle. A log line that lies about
                // whose values it is printing is worse than no line.
                const bool wasDepth = d.DepthEnable != FALSE;
                const bool wasStencil = d.StencilEnable != FALSE;
                const unsigned wasFunc = static_cast<unsigned>(d.DepthFunc);
                const bool wasWriteAll =
                    d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
                if (g_stateMode == Mode::kNoStencil ||
                    g_stateMode == Mode::kNoBoth) {
                    d.StencilEnable = FALSE;
                }
                if (g_stateMode == Mode::kNoDepth ||
                    g_stateMode == Mode::kNoBoth) {
                    d.DepthEnable = FALSE;
                }
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (dev) {
                    dev->CreateDepthStencilState(&d, &g_derived);
                    dev->Release();
                }
                Log::get().note(
                    "resolve probe: THE GAME'S state is depth=%s func=%u "
                    "write=%s stencil=%s ref=%u; OURS is depth=%s "
                    "stencil=%s.",
                    wasDepth ? "on" : "off", wasFunc,
                    wasWriteAll ? "all" : "zero",
                    wasStencil ? "on" : "off", g_savedRef,
                    d.DepthEnable ? "on" : "off",
                    d.StencilEnable ? "on" : "off");
            }
            if (!g_derived) {
                if (g_savedDs) {
                    g_savedDs->Release();
                    g_savedDs = nullptr;
                }
                return;   // draw stock
            }
            ctx->OMSetDepthStencilState(g_derived, g_savedRef);
            g_dsEngaged = true;
        }
        if (!g_psEngaged && !g_dsEngaged) return;
        ++g_applied;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "resolve probe: engaged -- the lighting resolve is drawing "
                "with %s, restored after every draw. Counting silently from "
                "here.",
                g_psEngaged ? "the replacement shader"
                            : "the modified depth-stencil state");
        }
    });
}

void resolveProbeEnd(ID3D11DeviceContext* ctx) {
    if ((!g_psEngaged && !g_dsEngaged) || !ctx) return;
    const bool ps = g_psEngaged, ds = g_dsEngaged;
    g_psEngaged = false;
    g_dsEngaged = false;
    guardedBudget(g_budget, [&] {
        // Restore even where the saved pointer is null: null IS a state the
        // game can have been in, and leaving ours bound would apply it to
        // every later draw in the frame.
        if (ps) {
            ctx->PSSetShader(g_savedPs, nullptr, 0);
            if (g_savedPs) {
                g_savedPs->Release();
                g_savedPs = nullptr;
            }
        }
        if (ds) {
            ctx->OMSetDepthStencilState(g_savedDs, g_savedRef);
            if (g_savedDs) {
                g_savedDs->Release();
                g_savedDs = nullptr;
            }
        }
    });
}

void resolveProbeShutdown() {
    releaseShader();
    releaseState();
    if (g_savedPs) {
        g_savedPs->Release();
        g_savedPs = nullptr;
    }
    if (g_savedDs) {
        g_savedDs->Release();
        g_savedDs = nullptr;
    }
}

}  // namespace edvr
