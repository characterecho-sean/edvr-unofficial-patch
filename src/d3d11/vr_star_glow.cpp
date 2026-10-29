// visual.star_glow in the headset: vr_star_glow.h says what and why; the rules are vr_star_glow_math.h's.
#include "vr_star_glow.h"

#include "binding_shadow.h"
#include "menu.h"               // menuStarGlowBloom: the Bloom hint's reader
#include "ui_layer.h"           // UiGlowStar, UiHeadsetGlow, uiLayerHeadsetGlowFeed
#include "vr_hud_glow_math.h"   // the HUD's bloom: the mode, the window, the lines
#include "vr_star_glow_math.h"
#include "vr_world_route.h"     // VrWorldInternalScope: the helper passes' own D3D calls step past vscreen's hooks
#include "vscreen.h"            // the raw OM entry points, vScreenIsEyeSized

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "../common/star_glow.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>

namespace edvr {

namespace detail {
std::atomic<bool> g_vrStarGlowWanted{false};
std::atomic<bool> g_vrStarGlowPending{false};
}  // namespace detail

namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr UINT kTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
constexpr size_t kCloneSlots = 16;
constexpr ULONGLONG kWindowMs = 10000;

// ---- the setting and the mode ----
bool g_configRead = false;
uint32_t g_configGeneration = 0;
int g_percent = 100;                  // visual.star_glow as the reader gave it: 100, 125, 150, 175 or 200
bool g_unrecognisedNoted = false;
VrStarMode g_mode = VrStarMode::kOff;
int g_windowPercent = 100;            // the setting the window being counted belongs to (valid while ON)
int g_shownPercent = 100;             // the last setting seen while not off: the line for the setting returning to 100 says what it was
ULONGLONG g_windowStart = 0;
uint64_t g_frame = 1;                 // the frame in progress: the pre-pass table's stamps are compared with it
uint64_t g_jetSourceFrame = 0;        // the frame the jet's source was last drawn in (any target: it draws into one of its own)
FaultBudget g_budget("vrStarGlow", 4);

// ---- one H ----
// S: the star passes of this H alone, an R11G11B10_FLOAT layer at H's size.
struct Layer {
    Ptr<ID3D11Texture2D> tex;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
    uint32_t w = 0, h = 0;
};
struct Slot {
    Layer layer;
    VrStarH run;                      // this frame's run, light and first level
    bool cleared = false;             // S was cleared this frame (its first replay)
};
Slot g_slots[kVrStarSlots];
VrStarSlotTable g_table;              // H resource identity -> slot
VrStarPreTable g_pre;                 // depth views a pre-pass was drawn with
VrStarWindow g_win;

// Rtv0's classification, per binding generation: is it an eye-sized R11G11B10_FLOAT texture (an H once a depth buffer is bound), and which slot holds it.
struct Target {
    uint32_t gen = 0;                 // 0: stale (generations start at 1)
    bool hFormat = false;
    void* res = nullptr;
    uint32_t w = 0, h = 0;
    int slot = -1;
};
Target g_target;

// The game's depth-stencil states, each with a clone that has its depth and stencil writes off (a state is immutable: the clone is made once; the source
// is held so its address cannot be reused by another state while the entry stands).
struct CloneSlot {
    Ptr<ID3D11DepthStencilState> source, clone;
    bool used = false, trivial = false;   // trivial: the source writes nothing, the replay keeps it as it is
};
CloneSlot g_clones[kCloneSlots];
size_t g_cloneNext = 0;
ID3D11Device* g_device = nullptr;     // identity only: the device the layers and clones were made on

// What the replay's bracket saved and puts back.
struct Saved {
    Ptr<ID3D11RenderTargetView> rtvs[kTargets];
    Ptr<ID3D11DepthStencilView> dsv;
    Ptr<ID3D11DepthStencilState> ds;
    UINT stencilRef = 0;
    bool active = false;
};
Saved g_saved;

// The game's view of H at t0, held while the first level reads H'.
struct FirstLevelBind {
    Ptr<ID3D11ShaderResourceView> game;
    bool active = false;
};
FirstLevelBind g_t0;

// The once-only lines.
bool g_feedNoted = false, g_layerFailNoted = false, g_feedFailNoted = false, g_cloneFailNoted = false, g_slotsFullNoted = false;

// ---- the HUD's bloom (vr_hud_glow_math.h) ----
// Its own mode and window: the star glow standing down never frees what the HUD's feed holds (there is nothing: H' is ui_layer.cpp's), and the HUD's mode follows the
// HDR HUD take and Elite's Bloom, not the setting.
VrHudGlowMode g_hudMode = VrHudGlowMode::kOff;
VrHudGlowWindow g_hudWin;
ULONGLONG g_hudWindowStart = 0;
uint64_t g_hudEyeFramesBase[2] = {};   // uiLayerHudEyeFrames at the window's start
bool g_hudFedNoted = false, g_hudFailNoted = false;
uint64_t g_boundaries = 0;             // frame boundaries this module has seen in the VR profile: what the HUD's lines say "frame" by

void refreshPending() {
    bool any = false;
    for (size_t i = 0; i < kVrStarSlots; ++i)
        if (g_table.key[i] && g_slots[i].run.lit && !g_slots[i].run.done) any = true;
    detail::g_vrStarGlowPending.store(any, std::memory_order_relaxed);
}

void releaseSaved() {
    for (auto& r : g_saved.rtvs) r.Reset();
    g_saved.dsv.Reset();
    g_saved.ds.Reset();
    g_saved.stencilRef = 0;
    g_saved.active = false;
}

void releaseSlot(size_t i) {
    g_slots[i] = Slot{};
    g_table.release(static_cast<int>(i));
    g_win.slot[i] = VrStarSlotWindow{};
    g_target.gen = 0;                 // the cached slot index may have been this one's
}

void releaseAll() {
    for (size_t i = 0; i < kVrStarSlots; ++i) releaseSlot(i);
    for (CloneSlot& c : g_clones) c = CloneSlot{};
    g_cloneNext = 0;
    g_pre = VrStarPreTable{};
    g_jetSourceFrame = 0;
    g_target = Target{};
    g_device = nullptr;
    g_t0.game.Reset();
    g_t0.active = false;
    releaseSaved();
    detail::g_vrStarGlowPending.store(false, std::memory_order_relaxed);
}

// A device other than the one the layers were made on: nothing made against the old one is usable.
void noteDevice(ID3D11Device* dev) {
    if (g_device == dev) return;
    if (g_device) {
        for (size_t i = 0; i < kVrStarSlots; ++i) g_slots[i].layer = Layer{};
        for (CloneSlot& c : g_clones) c = CloneSlot{};
        g_cloneNext = 0;
    }
    g_device = dev;
}

bool ensureLayer(ID3D11DeviceContext* ctx, Slot& s, uint32_t w, uint32_t h) {
    if (s.layer.tex && s.layer.w == w && s.layer.h == h) return true;
    s.layer = Layer{};
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !w || !h) return false;
    noteDevice(dev.Get());
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
    s.layer = made;
    return true;
}

// The state to replay a pass with: the game's own, or its clone with the writes off. False: the clone could not be made.
bool depthStateFor(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* game, ID3D11DepthStencilState** out) {
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

// Is the bound blend ONE / ONE, ADD in slot 0? (The jet's add: the bloom blur pair also draws with the blend off all frame, but never into an H.)
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

// Puts the saved targets and depth state back. Guarded on its own, not through the fault budget: a bracket that just faulted has charged the budget, and
// a spent budget would skip the one call that takes S off the game's bindings. A fault here charges the budget like any other in the bracket.
bool restoreSaved(ID3D11DeviceContext* ctx) {
    const bool ran = guarded("vrStarGlow.restore", [&] {
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

// Saves the game's targets and depth state and binds S as the one colour target with the game's depth view and the replay's depth state. The saved state is
// live from the first call that changes the game's bindings: a fault inside either call leaves S bound, and replayBegin puts the game's back while it is set.
VrStarRefusal beginReplay(ID3D11DeviceContext* ctx, size_t si, uint32_t w, uint32_t h) {
    Slot& s = g_slots[si];
    if (!ensureLayer(ctx, s, w, h)) return VrStarRefusal::kLayerFail;
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
    if (!g_saved.rtvs[0]) return VrStarRefusal::kStateFault;
    ID3D11DepthStencilState* replay = nullptr;
    if (!depthStateFor(ctx, g_saved.ds.Get(), &replay)) return VrStarRefusal::kDepthCloneFail;
    if (!s.cleared) {   // this H's first replay of the frame
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        vScreenClearRenderTargetViewRaw(ctx, s.layer.rtv.Get(), zero);
        s.cleared = true;
        ++g_win.slot[si].layers;
    }
    ID3D11RenderTargetView* target = s.layer.rtv.Get();
    g_saved.active = true;
    vScreenSetRenderTargetsRaw(ctx, 1, &target, g_saved.dsv.Get());
    vScreenOMSetDepthStencilStateRaw(ctx, replay, ref);
    return VrStarRefusal::kNone;
}

// beginReplay under the helper scope (outside the guard: an SEH fault does not unwind a destructor inside it) and the fault budget. False: nothing is left
// bound, the reason is in *why.
bool replayBegin(ID3D11DeviceContext* ctx, size_t si, uint32_t w, uint32_t h, VrStarRefusal* why) {
    VrWorldInternalScope internal;
    VrStarRefusal r = VrStarRefusal::kStateFault;
    const bool ran = guardedBudget(g_budget, [&] { r = beginReplay(ctx, si, w, h); });
    if (!ran) r = VrStarRefusal::kStateFault;
    if (!ran || r != VrStarRefusal::kNone) {
        // A fault after the first rebinding left S bound and no replayEnd will follow (the caller issues only when this returns true): put the game's
        // bindings back here. A refusal before any rebinding never set active, so the game's state was not touched and nothing is put back.
        if (g_saved.active) restoreSaved(ctx);
        releaseSaved();
        *why = r;
        return false;
    }
    return true;
}

// The target Rtv0 is bound to, classified once per binding generation: a resolve only when the binding changed.
Target& currentTarget() {
    const uint32_t gen = bindingGeneration(BindSlot::Rtv0);
    if (g_target.gen == gen) return g_target;
    g_target = Target{};
    g_target.gen = gen;
    void* rtv = bindingGet(BindSlot::Rtv0);
    ResourceInfo info;
    if (rtv && bindingResolve(rtv, &info) && info.isTexture2D && info.fmt == static_cast<uint32_t>(DXGI_FORMAT_R11G11B10_FLOAT) &&
        vScreenIsEyeSized(info.a, info.b)) {
        g_target.hFormat = true;
        g_target.res = info.resource;
        g_target.w = info.a;
        g_target.h = info.b;
        g_target.slot = g_table.find(info.resource);
    }
    return g_target;
}

void noteOnce(bool& said, const char* what, size_t slot, uint32_t w, uint32_t h) {
    if (said) return;
    said = true;
    Log::get().note("vr star glow: %s (H slot %u, %ux%u); said once", what, static_cast<unsigned>(slot + 1), w, h);
}

// One draw, classified (the caller holds the budget). True: it is a star pass and its replay is bound.
bool classify(ID3D11DeviceContext* ctx, bool plain) {
    Target& t = currentTarget();
    void* const dsv = bindingGet(BindSlot::Dsv0);
    const uint64_t ps = bindingShaderHash(BindSlot::Ps);
    if (!t.hFormat || !dsv) {
        // Not a draw of an eye H. The two passes that are evidence of a star frame draw elsewhere: the pre-pass (no pixel shader) into the G-buffer, the
        // jet's source into a depth-less target of its own. A pixel-shader hash is already loaded; everything else is one compare.
        if (ps == 0) {
            if (dsv && bindingShaderHash(BindSlot::Vs) == kFlatStarPreVs) {
                g_pre.note(dsv, g_frame);
                ++g_win.prePasses;
            }
        } else if (ps == kFlatStarJetSourcePs && !dsv && bindingShaderHash(BindSlot::Vs) == kFlatStarJetSourceVs) {
            g_jetSourceFrame = g_frame;
            ++g_win.jetSources;
        }
        return false;
    }
    ++g_win.hDraws;
    const FlatStarRole role = flatStarRoleOf(bindingShaderHash(BindSlot::Vs), ps, true);
    if (!flatStarRoleReplays(role)) {
        // A draw of an H that is no pass to replay: the pre-pass or the jet's source drawn into an H (evidence, the run goes on), or any other draw, which
        // ends this H's run (the ring that follows it is not a star's).
        if (role == FlatStarRole::kPre) {
            g_pre.note(dsv, g_frame);
            ++g_win.prePasses;
        } else if (role == FlatStarRole::kJetSource) {
            g_jetSourceFrame = g_frame;
            ++g_win.jetSources;
        } else if (t.slot >= 0) {
            flatStarNoteOther(g_slots[t.slot].run.f, true);
        }
        return false;
    }
    // A star pass of this H: its slot (made at the first one), then flat's gates for this H alone.
    int si = t.slot;
    if (si < 0) {
        bool fresh = false;
        si = g_table.claim(t.res, g_frame, &fresh);
        if (si < 0) {
            ++g_win.slotsFull;
            if (!g_slotsFullNoted) {
                g_slotsFullNoted = true;
                Log::get().note("vr star glow: a star pass of an H beyond the %u slots (%ux%u) found every slot in use; it is not replayed (slots-full in the window's head); said once",
                                static_cast<unsigned>(kVrStarSlots), t.w, t.h);
            }
            return false;
        }
        if (fresh) {
            g_slots[si] = Slot{};
            g_win.slot[si] = VrStarSlotWindow{};
            refreshPending();
        }
        t.slot = si;
    } else {
        g_table.claim(t.res, g_frame, nullptr);   // used this frame
    }
    Slot& s = g_slots[si];
    VrStarSlotWindow& w = g_win.slot[si];
    w.w = t.w;
    w.h = t.h;
    VrStarFacts facts;
    facts.preSeen = g_pre.seen(dsv, g_frame);
    facts.jetSource = g_jetSourceFrame == g_frame;
    // Only the jet's add reads the blend, and only once the cheaper gates would let it through.
    if (flatStarRoleNeedsBlend(role) && !s.run.done && (s.run.f.pre || facts.preSeen) && (s.run.f.jetSource || facts.jetSource))
        guarded("vrStarGlow.blend", [&] { facts.blendOneOne = blendIsOneOne(ctx); });
    const FlatStarVerdict v = vrStarDecide(s.run, role, facts);
    const int ri = vrStarRoleIndex(role);
    if (ri >= 0) ++w.seen[ri];
    if (!v.replay) {
        vrStarCountRefusal(w, vrStarRefusalOf(v.refusal));
        return false;
    }
    if (!plain) {
        vrStarCountRefusal(w, VrStarRefusal::kNotPlain);
        return false;
    }
    VrStarRefusal why = VrStarRefusal::kStateFault;
    if (!replayBegin(ctx, static_cast<size_t>(si), t.w, t.h, &why)) {
        vrStarCountRefusal(w, why);
        if (why == VrStarRefusal::kLayerFail) noteOnce(g_layerFailNoted, "the star layer could not be created", static_cast<size_t>(si), t.w, t.h);
        else if (why == VrStarRefusal::kDepthCloneFail) noteOnce(g_cloneFailNoted, "the bound depth-stencil state could not be cloned without its writes", static_cast<size_t>(si), t.w, t.h);
        return false;
    }
    if (ri >= 0) ++w.replayed[ri];
    s.run.f.anyReplay = true;
    if (!s.run.lit) {
        s.run.lit = true;
        refreshPending();
    }
    return true;
}

// The first level reads an H. Two inputs may join it, each on its own evidence: the star light, when the table holds the H with light this frame, and the HUD, when an
// eye's HDR HUD layer was taken from this very H (ui_layer.cpp uiHeadsetHudPick). One H' is built for both (uiLayerHeadsetGlowFeed) and bound at t0 for this one draw.
void bindFirstLevel(ID3D11DeviceContext* ctx) {
    void* const srv = bindingGet(BindSlot::PsSrv0);
    ResourceInfo info;
    // Who is asking: the star glow's light is waiting in some H, and/or an eye's HUD is. A first level nobody is waiting for is looked at by neither.
    const bool starAsked = g_mode == VrStarMode::kOn && detail::g_vrStarGlowPending.load(std::memory_order_relaxed);
    const bool hudAsked = g_hudMode == VrHudGlowMode::kOn && uiLayerHudGlowPending();
    if (!srv || !bindingResolve(srv, &info) || !info.isTexture2D) {
        if (starAsked) ++g_win.firstLevelUnknownH;
        if (hudAsked) vrHudCount(g_hudWin, UiHeadsetHud::kNoLayer, -1, false, 0);
        return;
    }
    int si = -1;
    UiGlowStar star;
    UiGlowStar* starIn = nullptr;
    if (starAsked) {
        si = g_table.find(info.resource);
        const bool known = si >= 0;
        switch (vrStarFirstLevel(known, known && g_slots[si].run.lit, known && g_slots[si].run.done)) {
            case VrFirstLevel::kUnknownH: ++g_win.firstLevelUnknownH; break;
            case VrFirstLevel::kHoldsNoLight: ++g_win.firstLevelNoLight; break;
            case VrFirstLevel::kRepeat: ++g_win.firstLevelRepeat; break;
            case VrFirstLevel::kFeed: {
                Slot& s = g_slots[si];
                ++g_win.slot[si].firstLevels;
                s.run.done = true;   // this H's first level has come, fed or not: its later star passes are after-trigger
                refreshPending();
                star.srv = s.layer.srv.Get();
                star.m = static_cast<float>(flatStarGlowM(g_percent));
                star.w = s.layer.w;
                star.h = s.layer.h;
                starIn = &star;
                break;
            }
        }
    }
    if (!starIn && !hudAsked) return;
    UiHeadsetGlow glow;
    ID3D11ShaderResourceView* fed = uiLayerHeadsetGlowFeed(ctx, 0, info.resource, starIn, &glow);
    Ptr<ID3D11ShaderResourceView> game;
    ctx->PSGetShaderResources(0, 1, &game);
    const bool canBind = fed && game;   // the game's own view is what goes back after the draw: without it H' is not bound
    const UiHeadsetHud pick = static_cast<UiHeadsetHud>(glow.hud);
    if (hudAsked || pick == UiHeadsetHud::kFeed) {
        const bool hudFed = glow.hudIncluded && canBind;
        vrHudCount(g_hudWin, pick, glow.eye, hudFed, glow.hudIncluded && !game ? static_cast<int>(UiGlowFeed::kBinding) : glow.verdict);
        if (pick == UiHeadsetHud::kFeed && !hudFed && !g_hudFailNoted) {
            g_hudFailNoted = true;
            char line[640];
            vrHudGlowFailLine(line, sizeof(line), glow.eye, glow.verdict, game != nullptr);
            Log::get().note("%s", line);
        }
    }
    if (starIn) {
        Slot& s = g_slots[si];
        VrStarSlotWindow& w = g_win.slot[si];
        if (!canBind || !star.applied) {
            vrStarCountRefusal(w, VrStarRefusal::kFeedFail);
            if (!g_feedFailNoted) {
                g_feedFailNoted = true;
                Log::get().note("vr star glow: H' could not be built for the first level (H slot %u, %ux%u; the feed's verdict %d, size mismatch %d, pass unavailable %d, "
                                "game's view of t0 %s); the draw reads H as the game made it; said once",
                                static_cast<unsigned>(si + 1), s.layer.w, s.layer.h, glow.verdict, star.sizeMismatch ? 1 : 0, star.unavailable ? 1 : 0,
                                game ? "held" : "missing");
            }
        }
    }
    if (!canBind) return;
    ID3D11ShaderResourceView* bound = fed;
    vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &bound);
    g_t0.game = game;
    g_t0.active = true;
    if (glow.hudIncluded && !g_hudFedNoted) {
        g_hudFedNoted = true;
        char line[640];
        vrHudGlowFedLine(line, sizeof(line), glow.eye, info.a, info.b, g_boundaries);
        Log::get().note("%s", line);
    }
    if (!starIn || !star.applied) return;
    Slot& s = g_slots[si];
    VrStarSlotWindow& w = g_win.slot[si];
    ++w.fed;
    if (!g_feedNoted) {
        g_feedNoted = true;
        Log::get().note("vr star glow: the star light joins the game's bloom -- the first-level draw (ps %016llX) reads H' = H + %.2f x S at t0 from frame %llu on "
                        "(H slot %u, %ux%u; S: the star passes replayed as the game issued them, one layer per eye's H); the exposure reduction, the composite and "
                        "every other reader keep H.",
                        static_cast<unsigned long long>(kVrStarFirstLevelPs), static_cast<double>(star.m), static_cast<unsigned long long>(g_frame),
                        static_cast<unsigned>(si + 1), s.layer.w, s.layer.h);
    }
}

// The game's view of H goes back at t0.
void endFirstLevel(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* game = g_t0.game.Get();
    g_t0.active = false;
    const bool ran = guarded("vrStarGlow.restoreT0", [&] { vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &game); });
    if (!ran) g_budget.charge();
    g_t0.game.Reset();
}

// ---- the frame ----
size_t slotsInUse() {
    size_t n = 0;
    for (size_t i = 0; i < kVrStarSlots; ++i)
        if (g_table.key[i]) ++n;
    return n;
}

// The 10 s lines: the head, then one line for each H the table holds.
void logWindow(int percent) {
    char line[1400];
    vrStarWindowLine(line, sizeof(line), percent, slotsInUse(), g_win);
    Log::get().note("%s", line);
    for (size_t i = 0; i < kVrStarSlots; ++i) {
        if (!g_win.slot[i].w) continue;
        vrStarSlotLine(line, sizeof(line), i, g_win.slot[i]);
        Log::get().note("%s", line);
    }
}

// The frame that ended, into the window; the frame that starts, clean.
void foldFrame() {
    for (size_t i = 0; i < kVrStarSlots; ++i) {
        if (!g_table.key[i]) continue;
        Slot& s = g_slots[i];
        VrStarSlotWindow& w = g_win.slot[i];
        if (s.run.f.anyStar) ++w.starFrames;
        if (s.run.lit && !s.run.done) vrStarCountRefusal(w, VrStarRefusal::kNoFirstLevel);
        vrStarHBeginFrame(s.run);
        s.cleared = false;
        if (g_table.idle(static_cast<int>(i), g_frame)) releaseSlot(i);
    }
    ++g_frame;
    refreshPending();
}

// A new window: empty, but every H the table holds keeps its size, so its line is printed from the first window on.
void resetWindow(ULONGLONG now) {
    VrStarWindow next;
    for (size_t i = 0; i < kVrStarSlots; ++i) {
        if (!g_table.key[i]) continue;
        next.slot[i].w = g_win.slot[i].w;
        next.slot[i].h = g_win.slot[i].h;
    }
    g_win = next;
    g_windowStart = now;
}

// Stands everything down (the mode left ON, or the profile left VR): at a frame boundary the game's bindings are certainly its own, every object is released,
// and the hooks go back to their one load.
void standDown() {
    detail::g_vrStarGlowWanted.store(g_hudMode == VrHudGlowMode::kOn, std::memory_order_relaxed);   // the HUD's bloom keeps the hooks if it is on
    releaseAll();
}

// ---- the HUD's bloom: the frame ----
// The 10 s line: the window's counts and the eye-frames the layer took in it.
void hudLogWindow() {
    for (int e = 0; e < 2; ++e) g_hudWin.eyeFrames[e] = uiLayerHudEyeFrames(e) - g_hudEyeFramesBase[e];
    char line[900];
    vrHudGlowWindowLine(line, sizeof(line), g_hudWin);
    Log::get().note("%s", line);
}

void hudResetWindow(ULONGLONG now) {
    g_hudWin = VrHudGlowWindow{};
    for (int e = 0; e < 2; ++e) g_hudEyeFramesBase[e] = uiLayerHudEyeFrames(e);
    g_hudWindowStart = now;
}

// Stands the HUD's bloom down (the profile left VR, or the fault budget is spent): the hooks keep only what the star glow still wants.
void hudStandDown() {
    g_hudMode = VrHudGlowMode::kOff;
    g_hudWin = VrHudGlowWindow{};
    detail::g_vrStarGlowWanted.store(g_mode == VrStarMode::kOn, std::memory_order_relaxed);
}

// Once a frame, before the star glow's own boundary: reads the mode (the HDR HUD take armed, Elite's Bloom), arms or disarms the hooks, prints the window.
void hudFrameBoundary() {
    ++g_boundaries;
    const VrHudGlowMode mode = vrHudGlowMode(uiLayerCrispOn(), menuStarGlowBloom());
    const ULONGLONG now = GetTickCount64();
    char line[640];
    if (mode != g_hudMode) {
        const VrHudGlowMode before = g_hudMode;
        if (before == VrHudGlowMode::kOn) hudLogWindow();   // the counts of the state that just ended
        g_hudMode = mode;
        hudResetWindow(now);
        if (mode == VrHudGlowMode::kOn) {
            g_hudFedNoted = g_hudFailNoted = false;
            vrHudGlowOnLine(line, sizeof(line), g_boundaries);
            Log::get().note("%s", line);
        } else if (mode == VrHudGlowMode::kPaused) {
            vrHudGlowPausedLine(line, sizeof(line));
            Log::get().note("%s", line);
        } else if (before != VrHudGlowMode::kOff) {
            vrHudGlowOffLine(line, sizeof(line));
            Log::get().note("%s", line);
        }
    } else if (mode == VrHudGlowMode::kOn && now - g_hudWindowStart >= kWindowMs) {
        hudLogWindow();
        hudResetWindow(now);
    }
    detail::g_vrStarGlowWanted.store(g_mode == VrStarMode::kOn || g_hudMode == VrHudGlowMode::kOn, std::memory_order_relaxed);
}

}  // namespace

void vrStarGlowFirstLevel(ID3D11DeviceContext* ctx) {
    if (!ctx || !vrStarIsFirstLevel(bindingShaderHash(BindSlot::Ps))) return;   // the common case while some H is lit: one load, one compare
    if (g_t0.active) return;
    VrWorldInternalScope internal;
    guardedBudget(g_budget, [&] { bindFirstLevel(ctx); });
}

bool vrStarGlowAfterDraw(ID3D11DeviceContext* ctx, bool plain) {
    if (!ctx) return false;
    if (g_t0.active) endFirstLevel(ctx);   // only the first-level draw read H'; the game's view is back before anything else looks
    if (g_mode != VrStarMode::kOn) return false;   // the HUD's bloom alone is armed: no star pass to classify, no replay
    ++g_win.draws;
    bool replay = false;
    if (!guardedBudget(g_budget, [&] { replay = classify(ctx, plain); })) {
        // A fault inside the classification: if it left S bound (the bracket's own guard puts it back; this is the net under it), the game's state is restored.
        if (g_saved.active) restoreSaved(ctx);
        releaseSaved();
        return false;
    }
    return replay;
}

void vrStarGlowReplayEnd(ID3D11DeviceContext* ctx) {
    if (!g_saved.active) return;
    restoreSaved(ctx);
    releaseSaved();
}

void vrStarGlowFrameBoundary() {
    // The profile can switch live: a headset session that became a flat one stands down at its next boundary, and a flat one never starts.
    if (runtimeFlatProfile()) {
        hudStandDown();   // the flat profile feeds its own HUD bloom (flat_ui_layer.h); nothing here runs
        if (g_mode != VrStarMode::kOff) {
            standDown();
            g_mode = VrStarMode::kOff;
        }
        return;
    }
    // A spent fault budget (FEATURE-DISABLED in the log) turns the glow off for the session: the hooks go back to their one load.
    if (!g_budget.shouldRun()) {
        hudStandDown();
        if (g_mode != VrStarMode::kOff) {
            standDown();
            g_mode = VrStarMode::kOff;
        }
        return;
    }
    hudFrameBoundary();
    if (g_mode == VrStarMode::kOn) foldFrame();
    Config& cfg = Config::get();
    // Live: the settings' generation is read BEFORE the values (config.h), so a swap that lands between costs one more read, never a stale one.
    const uint32_t generation = cfg.generation();
    if (!g_configRead || generation != g_configGeneration) {
        g_configRead = true;
        g_configGeneration = generation;
        bool recognised = true;
        g_percent = starGlowPercent(cfg, &recognised);
        if (!recognised && !g_unrecognisedNoted) {
            g_unrecognisedNoted = true;
            Log::get().note("vr star glow: visual.star_glow = \"%s\" is not one of 100, 125, 150, 175, 200; it is read as 100 (the game as it ships)",
                            cfg.getString("visual.star_glow", "100").c_str());
        }
    }
    const VrStarMode mode = vrStarMode(g_percent, g_percent > 100 ? menuStarGlowBloom() : sgb::Bloom::Unknown);
    const ULONGLONG now = GetTickCount64();
    char line[512];
    if (mode != g_mode) {
        const VrStarMode before = g_mode;
        if (before == VrStarMode::kOn) logWindow(g_windowPercent);   // the counts of the state that just ended
        standDown();   // nothing carries over, whichever way the mode moved
        g_mode = mode;
        if (mode == VrStarMode::kOn) {
            g_win = VrStarWindow{};
            g_windowPercent = g_percent;
            g_windowStart = now;
            g_feedNoted = g_layerFailNoted = g_feedFailNoted = g_cloneFailNoted = g_slotsFullNoted = false;
            detail::g_vrStarGlowWanted.store(true, std::memory_order_relaxed);
            Log::get().note("vr star glow: on at setting=%d (k=%.2f, m=%.2f) from frame %llu; Elite's Bloom is not known to be off; the draw hooks now watch the owner "
                            "context's draws, and a window line plus one line for each eye's H follows every 10 s (draws=0: no draw reached the hook; h-draws=0: none wrote "
                            "an eye H; an H line with replayed all zero: no star pass of it got through a gate); one line when the setting returns to 100 or Bloom is "
                            "known off",
                            g_percent, flatStarGlowK(g_percent), flatStarGlowM(g_percent), static_cast<unsigned long long>(g_frame));
        } else if (mode == VrStarMode::kPaused) {
            vrStarPausedLine(line, sizeof(line), g_percent);
            Log::get().note("%s", line);
        } else if (before != VrStarMode::kOff) {
            vrStarOffLine(line, sizeof(line), g_shownPercent);
            Log::get().note("%s", line);
        }
        if (mode != VrStarMode::kOff) g_shownPercent = g_percent;
        return;
    }
    if (mode != VrStarMode::kOn) {
        if (mode == VrStarMode::kPaused) g_shownPercent = g_percent;
        return;
    }
    g_shownPercent = g_percent;
    if (g_percent != g_windowPercent) {
        // The setting moved while on (live): the old setting's window is printed, the new one starts. The slots and layers stay.
        logWindow(g_windowPercent);
        resetWindow(now);
        g_windowPercent = g_percent;
    } else if (now - g_windowStart >= kWindowMs) {
        logWindow(g_percent);
        resetWindow(now);
    }
}

}  // namespace edvr
