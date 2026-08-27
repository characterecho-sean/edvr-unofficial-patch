#include "fss_panel.h"

#include <cstdio>    // _snprintf_s: the macro value for the compile

#include <windows.h>

#include <d3d11.h>

#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "exposure_fix.h"   // lookupShaderHash
#include "fss_panel_vs.h"
#include "shader_swap.h"

namespace edvr {
namespace {

// The pair, as dumped and transcribed. A game update that rebuilds them
// changes these hashes and the fix goes inert with a note, exactly like a
// pinned exposure shader.
constexpr uint64_t kColorHash   = 0xA888D51024D9798Eull;
constexpr uint64_t kPrepassHash = 0xB018D143700AB803ull;

bool  g_enabled = false;
float g_factor = 1.0f;

// The compiled pair, and the factor they were compiled at. A change
// releases and recompiles at the next matched draw; a failed compile
// stands the whole fix down for the session (both or neither -- one
// shader alone desynchronises the depth prepass).
ID3D11VertexShader* g_colorVs = nullptr;
ID3D11VertexShader* g_prepassVs = nullptr;
float g_compiledFactor = 0.0f;
bool  g_compileTried = false;
bool  g_compileFailedNoted = false;

// Which of the pair the current draw runs, decided by fssPanelOnEyeDraw
// and consumed by begin in the same call stack.
uint64_t g_matchedHash = 0;

bool                g_engaged = false;
ID3D11VertexShader* g_savedVs = nullptr;

uint64_t g_applied = 0;
bool     g_engagedNoted = false;

FaultBudget g_budget("fssPanel", 8);

void releaseShaders() {
    if (g_colorVs) {
        g_colorVs->Release();
        g_colorVs = nullptr;
    }
    if (g_prepassVs) {
        g_prepassVs->Release();
        g_prepassVs = nullptr;
    }
    g_compileTried = false;
}

// Compile both replacements with the current factor baked in. Null pair
// on any failure; the note is said once per session.
void ensureCompiled(ID3D11DeviceContext* ctx) {
    if (g_compileTried && g_compiledFactor == g_factor) return;
    releaseShaders();
    g_compileTried = true;
    g_compiledFactor = g_factor;

    char dist[32];
    _snprintf_s(dist, sizeof(dist), _TRUNCATE, "%.6f", g_factor);
    const SwapMacro macros[] = {{"EDVR_FSS_DIST", dist}, {nullptr, nullptr}};

    // One source buffer per shader: the shared preamble plus its body, the
    // same bytes the desk compile checked.
    std::string src = std::string(kFssPanelCommon) + kFssPanelColorVS;
    g_colorVs = shaderSwapCompileVs(ctx, src.c_str(), src.size(), "main",
                                    "fss_panel_color", macros, "fss panel");
    src = std::string(kFssPanelCommon) + kFssPanelPrepassVS;
    g_prepassVs = shaderSwapCompileVs(ctx, src.c_str(), src.size(), "main",
                                      "fss_panel_prepass", macros, "fss panel");
    if (!g_colorVs || !g_prepassVs) {
        releaseShaders();
        g_compileTried = true;   // do not retry every draw; config change resets
        if (!g_compileFailedNoted) {
            g_compileFailedNoted = true;
            Log::get().note(
                "fss panel: the replacement pair did not compile (the shader "
                "swap's own lines above say why), so the scanner's screen "
                "draws at the game's own distance this session.");
        }
    }
}

}  // namespace

void fssPanelConfigure(Config& cfg) {
    const bool  was = g_enabled;
    const float wasFactor = g_factor;

    // Stock by default. Inheriting panel_distance when the key is ABSENT
    // was flown by accident (2026-08-27: the [fix]->[experimental] move
    // left live inis' stock pins unread) and moved the whole scanner UI
    // for anyone with a customised on-foot distance -- the off-centre-arc
    // behaviour that kept this key pinned all campaign. Inherit is now
    // opt-in: an explicit 0.
    float f = cfg.getFloat("experimental.fss_panel_distance", 1.0f);
    if (f == 0.0f) {
        // Inherit the on-foot panel's distance: one setting, two screens.
        f = cfg.getFloat("fix.panel_distance", 1.0f);
    }
    if (f < 0.2f) f = 0.2f;
    if (f > 3.0f) f = 3.0f;
    g_factor = f;
    // Within a percent of stock there is nothing worth swapping a shader
    // pair for.
    g_enabled = f < 0.99f || f > 1.01f;

    if (g_enabled != was || (g_enabled && g_factor != wasFactor)) {
        Log::get().note(
            g_enabled
                ? "fss panel: the scanner's screen sits at %.2f of its stock "
                  "distance (%s). Angular size is unchanged -- the screen "
                  "moves, it does not grow."
                : "fss panel: stock distance; the scanner's screen is the "
                  "game's own.",
            g_factor,
            cfg.getFloat("experimental.fss_panel_distance", 0.0f) == 0.0f
                ? "inherited from panel_distance"
                : "fss_panel_distance");
    }
    // A factor change invalidates the compiled pair; the next matched draw
    // recompiles. Done here rather than comparing floats per draw.
    if (g_factor != wasFactor) g_compileTried = false;
}

bool fssPanelWantsDraws() { return g_enabled; }

bool fssPanelOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances) {
    if (!g_enabled || kind != 'X' || count != 6 || instances != 1 || !ctx) {
        return false;
    }
    // The hash read costs a VSGetShader on the few 6-index instanced quads
    // a frame -- the holo fix's cost class, and the census-skip vs:HASH
    // pattern's mechanics.
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        h = lookupShaderHash(vs);
        if (vs) vs->Release();
    });
    if (h != kColorHash && h != kPrepassHash) return false;
    g_matchedHash = h;
    return true;
}

void fssPanelBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (!ctx || !g_matchedHash) return;
    guardedBudget(g_budget, [&] {
        ensureCompiled(ctx);
        ID3D11VertexShader* ours =
            g_matchedHash == kColorHash ? g_colorVs : g_prepassVs;
        if (!ours) return;   // compile failed: the game draws stock

        ctx->VSGetShader(&g_savedVs, nullptr, nullptr);
        ctx->VSSetShader(ours, nullptr, 0);
        g_engaged = true;
        ++g_applied;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "fss panel: engaged -- the scanner's screen is drawn through "
                "the replacement pair at %.2f of stock distance. The game's "
                "shaders are restored after every draw.",
                g_compiledFactor);
        }
    });
}

void fssPanelEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    g_matchedHash = 0;
    ctx->VSSetShader(g_savedVs, nullptr, 0);
    if (g_savedVs) {
        g_savedVs->Release();
        g_savedVs = nullptr;
    }
}

void fssPanelShutdown() { releaseShaders(); }

}  // namespace edvr
