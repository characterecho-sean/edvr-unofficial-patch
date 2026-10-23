// visual.star_glow's flat mechanism: flat_star_glow.h says what and why; the rules are flat_star_glow_math.h's.
#include "flat_star_glow.h"

#include "flat_compute_readback.h"  // FlatComputeInternalScope
#include "flat_star_glow_math.h"
#include "ui_layer.h"               // UiGlowStar
#include "vscreen.h"                // the raw OM entry points: the shadow keeps describing the game's bindings

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "../common/star_glow.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>

namespace edvr {

namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr UINT kTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
constexpr size_t kCloneSlots = 16;

std::atomic<int> g_percent{100};           // visual.star_glow, snapped; read by every watched draw
bool g_configRead = false;
uint32_t g_configGeneration = 0;
bool g_unrecognisedNoted = false;
bool g_wasOn = false;
int g_windowPercent = 100;
ULONGLONG g_windowStart = 0;
uint64_t g_frameNo = 0;
uint64_t g_layerFailedFrame = ~0ull;      // the frame S could not be made in

FlatStarFrame g_frame;
FlatStarWindow g_win;
bool g_cleared = false;                    // S was cleared this frame (its first replay)
bool g_feedNoted = false;                  // the once-only first feed line
FaultBudget g_budget("starGlow.replay", 4);

// S: the star draws alone, an R11G11B10_FLOAT layer at the scene target's size.
struct Layer {
    Ptr<ID3D11Texture2D> tex;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
    uint32_t w = 0, h = 0;
};
Layer g_layer;

// The game's depth-stencil states, each with a clone that has its depth and stencil writes off. A state is immutable, so the clone is made once;
// the source is held so its address cannot be reused by another state while the entry stands.
struct CloneSlot {
    Ptr<ID3D11DepthStencilState> source, clone;
    bool used = false, trivial = false;   // trivial: the source writes nothing, the draw keeps it as it is
};
CloneSlot g_clones[kCloneSlots];
size_t g_cloneNext = 0;
Ptr<ID3D11Device> g_device;

// What the bracket saved and puts back.
struct Saved {
    Ptr<ID3D11RenderTargetView> rtvs[kTargets];
    Ptr<ID3D11DepthStencilView> dsv;
    Ptr<ID3D11DepthStencilState> ds;
    UINT stencilRef = 0;
    bool active = false;
};
Saved g_saved;

void releaseObjects() {
    g_layer = Layer{};
    for (CloneSlot& c : g_clones) c = CloneSlot{};
    g_cloneNext = 0;
}

void releaseSaved() {
    for (auto& r : g_saved.rtvs) r.Reset();
    g_saved.dsv.Reset();
    g_saved.ds.Reset();
    g_saved.stencilRef = 0;
    g_saved.active = false;
}

bool ensureLayer(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h) {
    if (g_layer.tex && g_layer.w == w && g_layer.h == h) return true;
    g_layer = Layer{};
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !w || !h) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Layer made;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &made.tex)) || !made.tex || FAILED(dev->CreateRenderTargetView(made.tex.Get(), nullptr, &made.rtv)) ||
        !made.rtv || FAILED(dev->CreateShaderResourceView(made.tex.Get(), nullptr, &made.srv)) || !made.srv)
        return false;
    made.w = w;
    made.h = h;
    g_layer = made;
    return true;
}

// The state to draw a replay with: the game's own, or its clone with the writes off. False: the clone could not be made.
bool replayDepthState(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* game, ID3D11DepthStencilState** out) {
    for (const CloneSlot& c : g_clones) {
        if (c.used && c.source.Get() == game) {
            *out = c.trivial ? game : c.clone.Get();
            return true;
        }
    }
    D3D11_DEPTH_STENCIL_DESC desc{};
    if (game) {
        game->GetDesc(&desc);
    } else {   // the API's default state
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D11_COMPARISON_LESS;
        desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    }
    CloneSlot& slot = g_clones[g_cloneNext];
    g_cloneNext = (g_cloneNext + 1) % kCloneSlots;
    slot = CloneSlot{};
    slot.source = game;
    const bool writesDepth = desc.DepthEnable && desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
    const bool writesStencil = desc.StencilEnable && desc.StencilWriteMask != 0;
    if (!writesDepth && !writesStencil) {
        slot.trivial = true;
        slot.used = true;
        *out = game;
        return true;
    }
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.StencilWriteMask = 0;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || FAILED(dev->CreateDepthStencilState(&desc, &slot.clone)) || !slot.clone) {
        slot = CloneSlot{};
        return false;
    }
    slot.used = true;
    *out = slot.clone.Get();
    return true;
}

// Is the bound blend ONE / ONE, ADD in slot 0? (The jet's add: the bloom blur pair also draws with the blend off all frame.)
bool blendIsOneOne(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11BlendState> bs;
    FLOAT factor[4] = {};
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, factor, &mask);
    if (!bs) return false;
    D3D11_BLEND_DESC bd{};
    bs->GetDesc(&bd);
    const D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
    return rt.BlendEnable && rt.SrcBlend == D3D11_BLEND_ONE && rt.DestBlend == D3D11_BLEND_ONE && rt.BlendOp == D3D11_BLEND_OP_ADD;
}

bool beginReplay(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, FlatStarRefusal& why) {
    why = FlatStarRefusal::kLayerFail;
    if (g_layer.tex && (g_layer.w != w || g_layer.h != h)) {
        if (g_cleared) {   // S already holds this frame's light at another size: one layer a frame
            why = FlatStarRefusal::kSizeMismatch;
            return false;
        }
        g_layer = Layer{};
    }
    if (g_layerFailedFrame == g_frameNo) return false;   // it failed this frame already: not asked again until the next
    if (!ensureLayer(ctx, w, h)) {
        g_layerFailedFrame = g_frameNo;
        return false;
    }
    why = FlatStarRefusal::kStateFault;
    ID3D11RenderTargetView* raw[kTargets] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(kTargets, raw, &dsv);
    for (UINT i = 0; i < kTargets; ++i) g_saved.rtvs[i].Attach(raw[i]);
    g_saved.dsv.Attach(dsv);
    UINT ref = 0;
    ID3D11DepthStencilState* ds = nullptr;
    ctx->OMGetDepthStencilState(&ds, &ref);
    g_saved.ds.Attach(ds);
    g_saved.stencilRef = ref;
    if (!g_saved.rtvs[0]) return false;
    ID3D11DepthStencilState* replay = nullptr;
    if (!replayDepthState(ctx, g_saved.ds.Get(), &replay)) {
        why = FlatStarRefusal::kDepthCloneFail;
        return false;
    }
    if (!g_cleared) {   // the frame's first replay
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        vScreenClearRenderTargetViewRaw(ctx, g_layer.rtv.Get(), zero);
        g_cleared = true;
        ++g_win.layerBuilt;
    }
    ID3D11RenderTargetView* s = g_layer.rtv.Get();
    // The saved state is live from the first call that changes the game's bindings, not from the end of this function: a fault inside either call
    // below leaves S bound, and the failure path in flatStarGlowReplayBegin puts the game's targets and depth state back only while this is set.
    g_saved.active = true;
    vScreenSetRenderTargetsRaw(ctx, 1, &s, g_saved.dsv.Get());
    vScreenOMSetDepthStencilStateRaw(ctx, replay, ref);
    why = FlatStarRefusal::kNone;
    return true;
}

// Puts the saved targets and depth state back. Guarded on its own, not through the fault budget: a bracket that just faulted has charged the budget, and
// a spent budget would skip the one call that takes S off the game's bindings. A fault here charges the budget like any other in the bracket.
bool restoreSaved(ID3D11DeviceContext* ctx) {
    const bool ran = guarded("starGlow.restore", [&] {
        ID3D11RenderTargetView* raw[kTargets] = {};
        UINT n = 0;
        for (UINT i = 0; i < kTargets; ++i) {
            raw[i] = g_saved.rtvs[i].Get();
            if (raw[i]) n = i + 1;
        }
        vScreenSetRenderTargetsRaw(ctx, n, raw, g_saved.dsv.Get());
        vScreenOMSetDepthStencilStateRaw(ctx, g_saved.ds.Get(), g_saved.stencilRef);
    });
    if (!ran) g_budget.charge();
    return ran;
}

// The 5 s line and the line for the setting returning to 100, each built into a buffer of its own right above the call that prints it.
void logWindow(int percent, const FlatStarWindow& w) {
    char line[1024];
    flatStarWindowLine(line, sizeof(line), percent, w);
    Log::get().note("%s", line);
}
void logOff(int fromPercent) {
    char line[256];
    flatStarOffLine(line, sizeof(line), fromPercent);
    Log::get().note("%s", line);
}

}  // namespace

bool flatStarGlowOn() { return g_percent.load(std::memory_order_relaxed) > 100; }

void flatStarGlowFrame(uint64_t nextFrame, bool watched) {
    if (!runtimeFlatProfile()) return;
    Config& cfg = Config::get();
    // Live: the settings' generation is read BEFORE the values (config.h), so a swap that lands between costs one more read, never a stale one.
    const uint32_t generation = cfg.generation();
    if (!g_configRead || generation != g_configGeneration) {
        g_configRead = true;
        g_configGeneration = generation;
        bool recognised = true;
        const int percent = starGlowPercent(cfg, &recognised);
        if (!recognised && !g_unrecognisedNoted) {
            g_unrecognisedNoted = true;
            Log::get().note("flat star glow: visual.star_glow = \"%s\" is not one of 100, 125, 150, 175, 200; it is read as 100 (the game as it ships)",
                            cfg.getString("visual.star_glow", "100").c_str());
        }
        g_percent.store(percent, std::memory_order_relaxed);
    }
    const int percent = g_percent.load(std::memory_order_relaxed);
    const bool on = percent > 100;
    // The frame that ended, into the window; the frame that starts, clean.
    if (g_frame.anyStar) ++g_win.starFrames;
    flatStarBeginFrame(g_frame);
    g_cleared = false;
    g_frameNo = nextFrame;
    const ULONGLONG now = GetTickCount64();
    if (on) {
        if (!g_wasOn || percent != g_windowPercent) {
            if (g_wasOn) logWindow(g_windowPercent, g_win);   // the setting changed while on: the old window's counts belong to the old setting
            g_wasOn = true;
            g_windowPercent = percent;
            g_windowStart = now;
            g_win = FlatStarWindow{};
            Log::get().note("flat star glow: on at setting=%d (k=%.2f, m=%.2f) from frame %llu; the frame that just ended was %s by the flat runtime's draw "
                            "scope; a line every 5 s follows (setting=%d with every count zero means no star pass reached the scope), and one line when the "
                            "setting returns to 100",
                            percent, flatStarGlowK(percent), flatStarGlowM(percent), static_cast<unsigned long long>(nextFrame),
                            watched ? "watched" : "NOT watched (a stood-down frame, or the flat temporal anti-aliasing is off)", percent);
        } else if (now - g_windowStart >= 5000) {
            logWindow(percent, g_win);
            g_win = FlatStarWindow{};
            g_windowStart = now;
        }
    } else if (g_wasOn) {
        g_wasOn = false;
        logOff(g_windowPercent);
        g_win = FlatStarWindow{};
        g_windowPercent = 100;
        releaseSaved();     // nothing is held at 100
        releaseObjects();
    }
}

bool flatStarGlowObserve(ID3D11DeviceContext* ctx, const FlatStarDraw& draw, bool* touched, uint8_t* roleOut) {
    if (touched) *touched = false;
    if (roleOut) *roleOut = 0;
    const FlatStarRole role = flatStarRoleOf(draw.vs, draw.ps, draw.hCandidate);
    if (role == FlatStarRole::kNone) {
        flatStarNoteOther(g_frame, draw.hCandidate);
        return false;
    }
    if (touched) *touched = true;
    FlatStarFacts facts;
    facts.hCandidate = draw.hCandidate;
    facts.triggered = draw.triggered;
    // Only the jet's add reads the blend, and only once the cheaper gates would let it through.
    if (flatStarRoleNeedsBlend(role) && draw.hCandidate && !draw.triggered && g_frame.pre && g_frame.jetSource) {
        FlatComputeInternalScope internal;
        guarded("starGlow.blend", [&] { facts.blendOneOne = ctx && blendIsOneOne(ctx); });
    }
    FlatStarVerdict v = flatStarDecide(g_frame, role, facts);
    if (v.replay && !g_budget.shouldRun()) {   // the bracket faulted too often: the replay stays off for the session
        v.replay = false;
        v.refusal = FlatStarRefusal::kStateFault;
    }
    flatStarCountVerdict(g_win, v);
    if (v.replay && roleOut) *roleOut = static_cast<uint8_t>(role);
    return v.replay;
}

bool flatStarGlowReplayBegin(ID3D11DeviceContext* ctx, uint8_t role, uint32_t width, uint32_t height) {
    if (!ctx || g_saved.active) return false;
    FlatComputeInternalScope internal;
    FlatStarRefusal why = FlatStarRefusal::kStateFault;
    bool ok = false;
    const bool ran = guardedBudget(g_budget, [&] { ok = beginReplay(ctx, width, height, why); });
    if (!ran) why = FlatStarRefusal::kStateFault;
    if (!ran || !ok) {
        flatStarCountRefusal(g_win, why);
        // A fault after the first rebinding left S bound and no ReplayEnd will follow (uiReissue calls it only when Begin returns true): put the game's
        // bindings back here. A refusal before any rebinding (ok false) never set active, so the game's state was not touched and nothing is put back.
        if (g_saved.active) restoreSaved(ctx);
        releaseSaved();
        return false;
    }
    flatStarCountReplayed(g_win, static_cast<FlatStarRole>(role));
    g_frame.anyReplay = true;
    return true;
}

void flatStarGlowReplayEnd(ID3D11DeviceContext* ctx) {
    if (!g_saved.active) return;
    FlatComputeInternalScope internal;
    if (!restoreSaved(ctx)) flatStarCountRefusal(g_win, FlatStarRefusal::kStateFault);
    releaseSaved();
}

bool flatStarGlowLive() {
    return flatStarGlowOn() && g_frame.anyReplay && g_layer.srv;
}

bool flatStarGlowFeedInput(UiGlowStar* star) {
    if (!star || !flatStarGlowLive()) return false;
    *star = UiGlowStar{};
    star->srv = g_layer.srv.Get();
    star->m = static_cast<float>(flatStarGlowM(g_percent.load(std::memory_order_relaxed)));
    star->w = g_layer.w;
    star->h = g_layer.h;
    return true;
}

void flatStarGlowNoteFeed(const UiGlowStar& star, bool bound, uint64_t frame, uint32_t slot, uint64_t vs, uint64_t ps, uint32_t hdrW, uint32_t hdrH,
                          uint32_t targetW, uint32_t targetH) {
    ++g_win.asked;
    if (star.applied && bound) {
        ++g_win.fed;
        if (!g_feedNoted) {
            g_feedNoted = true;
            Log::get().note("flat star glow: the star light joins the game's bloom -- the first-level draw vs %016llX ps %016llX (target %ux%u of the "
                            "scene's %ux%u) reads H' = H%s + %.2f x S at t%u from frame %llu on (S: the star passes replayed as the game issued them, "
                            "%ux%u); the chain, its tier and the tonemap are the game's, and the tonemap still reads H.",
                            static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps), targetW, targetH, hdrW, hdrH,
                            star.hudIncluded ? " + the HUD layer" : "", static_cast<double>(star.m), slot, static_cast<unsigned long long>(frame), star.w,
                            star.h);
        }
        return;
    }
    if (star.sizeMismatch) flatStarCountRefusal(g_win, FlatStarRefusal::kSizeMismatch);
    else flatStarCountRefusal(g_win, FlatStarRefusal::kLayerFail);
}

void flatStarGlowNoteNoBloom() { flatStarCountRefusal(g_win, FlatStarRefusal::kBloomOff); }

void flatStarGlowRelease() {
    releaseSaved();
    releaseObjects();
}

void flatStarGlowNoteDevice(ID3D11Device* device) {
    if (!device || device == g_device.Get()) return;
    if (g_device) {
        releaseSaved();
        releaseObjects();
    }
    g_device = device;
}

}  // namespace edvr
