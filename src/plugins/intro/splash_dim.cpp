#include "temporal_shader_bytecode.h"
#include "../../d3d11/splash_dim.h"

#include <windows.h>

#include <d3d11.h>

#include <string>

#include "../../common/config.h"
#include "../../common/runtime_profile.h"
#include "../../common/intro_mode.h"
#include "../../common/frame_flag.h"
#include "../../common/guard.h"
#include "../../common/log.h"
#include "../../d3d11/binding_shadow.h"
#include "../../d3d11/loader_panel.h"
#include "../../d3d11/shader_swap.h"
#include "../../d3d11/intro_cost_sites.h"

namespace edvr {
namespace {

// The scrim's measured alpha: 0x66 of 255, exactly 0.4, constant across
// every flight of the loader-panel work. The dim is the scrim's own tint,
// applied to the screen instead of the world.


// The composite's RTV must be the eye texture: the backdrop verdict also
// wraps the OFFSCREEN half of the still's path, and dimming both halves
// would dim twice.
constexpr uint32_t kEyeTolerance = 8;

FaultBudget g_budget("splashDim", 4);

bool g_on = false;

ID3D11PixelShader* g_ps = nullptr;
ID3D11BlendState*  g_blend = nullptr;
bool g_psTried = false;
bool g_blendTried = false;

// Saved around one re-issue.
ID3D11PixelShader*   g_savedPs = nullptr;
ID3D11ClassInstance* g_savedInst[16] = {};
UINT                 g_savedInstCount = 0;
ID3D11BlendState*    g_savedBlend = nullptr;
FLOAT                g_savedFactor[4] = {};
UINT                 g_savedMask = 0;
bool                 g_engaged = false;
bool                 g_costSample = false;

void failOnce(const char* why) {
    static bool noted = false;
    if (noted) return;
    noted = true;
    Log::get().note("splash dim: %s. The splash stays undimmed.", why);
}

bool ensureBuilt(ID3D11DeviceContext* ctx, bool costSample) {
    if (!g_ps && !g_psTried) {
        g_psTried = true;
        g_ps = shaderSwapCreatePs(ctx, kSplashDimBytecode, sizeof(kSplashDimBytecode), "splash_dim_ps", "splash dim");
        if (!g_ps) failOnce("the dark shader would not compile");
    }
    if (g_ps && !g_blend && !g_blendTried) {
        g_blendTried = true;
        ID3D11Device* dev = nullptr;
        if (costSample) {
            intro_cost::note(intro_cost::Site::SplashBlendGetDevice,
                             plugin_cost::ApiClass::ReadQuery);
        }
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask =
                D3D11_COLOR_WRITE_ENABLE_ALL;
            if (costSample) {
                intro_cost::note(intro_cost::Site::SplashCreateBlendState,
                                 plugin_cost::ApiClass::Work);
            }
            if (FAILED(dev->CreateBlendState(&bd, &g_blend))) {
                g_blend = nullptr;
                failOnce("the blend state could not be created");
            }
            dev->Release();
        }
    }
    return g_ps && g_blend;
}

}  // namespace

void splashDimConfigure(Config& cfg) {
    const bool was = g_on;
    g_on = loadingDimParse(runtimeVrProfile() ?
        cfg.getString("fix.loading_dim", "screen") : "stock").splashDim;
    if (was != g_on) {
        Log::get().note(
            "splash dim: %s. While the loader's dialogs are up (and their "
            "full-view scrim is being withheld), the splash screen itself "
            "is %s (docs/loading-panel-handoff.md).",
            g_on ? "ON" : "off",
            g_on ? "dimmed by the scrim's own tint -- the screen steps "
                   "back, the world does not"
                 : "left at full brightness");
    }
}

bool splashDimBegin(ID3D11DeviceContext* ctx) {
    if (!g_on || !ctx || g_engaged) return false;
    if (!loaderPanelDimWanted()) return false;
    const bool costSample = edvrPluginCostApiSampleContext(ctx) != 0;

    // Only the EYE-side composite: the backdrop verdict also wraps the
    // offscreen half of the still's path.
    ResourceInfo info;
    uint32_t ew = 0, eh = 0;
    if (!bindingResolve(bindingGet(BindSlot::Rtv0), &info) ||
        !info.isTexture2D || !eyeTextureSize(&ew, &eh) ||
        info.a + kEyeTolerance < ew || info.a > ew + kEyeTolerance ||
        info.b + kEyeTolerance < eh || info.b > eh + kEyeTolerance) {
        return false;
    }

    bool armed = false;
    guardedBudget(g_budget, [&] {
        if (!ensureBuilt(ctx, costSample)) return;
        g_savedInstCount = 16;
        if (costSample) {
            intro_cost::note(intro_cost::Site::SplashPsGetShader,
                             plugin_cost::ApiClass::ReadQuery);
        }
        ctx->PSGetShader(&g_savedPs, g_savedInst, &g_savedInstCount);
        if (costSample) {
            intro_cost::note(intro_cost::Site::SplashOmGetBlendState,
                             plugin_cost::ApiClass::ReadQuery);
        }
        ctx->OMGetBlendState(&g_savedBlend, g_savedFactor, &g_savedMask);
        if (costSample) {
            intro_cost::note(intro_cost::Site::SplashPsSetShaderApply,
                             plugin_cost::ApiClass::State);
        }
        ctx->PSSetShader(g_ps, nullptr, 0);
        const FLOAT factor[4] = {0, 0, 0, 0};
        if (costSample) {
            intro_cost::note(intro_cost::Site::SplashOmSetBlendStateApply,
                             plugin_cost::ApiClass::State);
        }
        ctx->OMSetBlendState(g_blend, factor, 0xFFFFFFFFu);
        g_engaged = true;
        g_costSample = costSample;
        armed = true;
    });
    return armed;
}

void splashDimEnd(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_engaged) return;
    const bool costSample = g_costSample &&
                           edvrPluginCostApiSampleContext(ctx) != 0;
    g_engaged = false;
    guardedBudget(g_budget, [&] {
        if (costSample) {
            intro_cost::note(intro_cost::Site::SplashPsSetShaderRestore,
                             plugin_cost::ApiClass::State);
        }
        ctx->PSSetShader(g_savedPs, g_savedInstCount ? g_savedInst : nullptr,
                         g_savedInstCount);
        if (costSample) {
            intro_cost::note(intro_cost::Site::SplashOmSetBlendStateRestore,
                             plugin_cost::ApiClass::State);
        }
        ctx->OMSetBlendState(g_savedBlend, g_savedFactor, g_savedMask);
    });
    g_costSample = false;
    if (g_savedPs) { g_savedPs->Release(); g_savedPs = nullptr; }
    for (UINT i = 0; i < g_savedInstCount; ++i) {
        if (g_savedInst[i]) { g_savedInst[i]->Release(); g_savedInst[i] = nullptr; }
    }
    g_savedInstCount = 0;
    if (g_savedBlend) { g_savedBlend->Release(); g_savedBlend = nullptr; }
}

void splashDimShutdown() {
    if (g_ps) { g_ps->Release(); g_ps = nullptr; }
    if (g_blend) { g_blend->Release(); g_blend = nullptr; }
    g_psTried = false;
    g_blendTried = false;
    g_engaged = false;
    g_costSample = false;
}

}  // namespace edvr
