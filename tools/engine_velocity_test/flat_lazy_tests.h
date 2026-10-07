#pragma once
// engine_velocity_test: the flat draw bracket's lazy form (src\d3d11\engine_velocity.h, "the flat draw bracket,
// lazy form"; the policy in src\d3d11\flat_substitution.h), on WARP, through the same functions the flat runtime calls.
//
// The flat runtime put the game's state back after every substituted producer draw and bound EDVR's again for the
// next: eleven context calls a draw at least. Now EDVR's state stays bound across consecutive producer draws and the
// game's goes back once, before anything that could see it. What could go wrong is the game drawing, dispatching,
// clearing, copying or presenting on EDVR's pixel shader, blend state and MRT6, or a restore putting back a stale
// render-target set over a rebind the game made. So the rig holds the CONTEXT ITSELF to the game's state, read back
// with Get calls after every hooked call that is not a producer draw, and to EDVR's after every producer's begin:
//
//   - a mixed sequence: runs of producer draws, a game setter of the pixel shader, of the blend state, of the render
//     targets, and every call the policy names (another draw, a dispatch, a clear, a copy, a resolve, a command list,
//     the Present), ClearState and a resize; every game call sees exactly the game's state, every producer draw EDVR's
//   - a game Get* between a producer draw and the next hooked call sees EDVR's state: that is the one exposure the lazy
//     form has (no Get is hooked, and it is VR's too); the rig pins it so it cannot become a surprise, and pins that
//     the very next hooked call restores
//   - the overlay guard's private t3 never outlives its draw, and a lazily kept underlay draw before it does not decline
//     the guard: the overlay counters come out as they do when every draw restores after itself
//   - a run of consecutive producer draws costs the context at least 70% fewer calls than the restore-after-every-draw
//     form, counted by a recorder on the context's own vtable, not by the wrapper's own counters
//   - with the lazy form off (a diagnostic capture is armed) every producer draw restores after itself
//   - each restore, dropped in turn from a copy of the policy (before another draw, a dispatch, a clear, a copy, a
//     resolve, a command list, the Present), is noticed by the same checks
//   - the census line says how many draws, runs and flushes there were
//
// The KEPT ANSWERS (src\d3d11\flat_query_cut.h): a run of substituted draws no longer asks the context for the game's blend
// state, the game's render targets or the runtime's acceptance of MRT6 each time, and the coverage classification no
// longer asks it for the depth view or the shaders. What can go wrong is the game changing that state through a path
// nothing hooks, which no shadow or kept answer can see, and the sampled check exists for exactly that:
//   - a blend state and a render-target set changed by a REAL call with no shadow update, on a checking frame, are
//     found (counted, one log line, the state asks the context from then on) and the context still ends up in the game's
//     state; off a checking frame the stale answer is used, which is what one frame in 64 bounds
//   - the depth view and the shaders the coverage classification asks about, through hooked setters (the shadow follows,
//     ClearState included) and through an unhooked one (found on a checking frame)
//   - the calls saved, counted on the context's own vtable: many short runs cost fewer with the answers kept than with
//     every state asking the context
//   - the frame's end lets go of what was kept: no view of the game's stays alive on our account
//
// The runtime's wiring -- that each hook calls the policy with the right event -- is a source scan in flat_temporal_test.

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../../src/d3d11/engine_velocity.h"
#include "../../src/d3d11/flat_query_cut.h"
#include "../../src/d3d11/flat_query_reads.h"
#include "../../src/d3d11/flat_substitution.h"
#include "lifecycle_tests.h"

namespace flat_lazy_tests {
using Microsoft::WRL::ComPtr;
using lifecycle_tests::Game;
using lifecycle_tests::Harness;
using edvr::BindSlot;
using edvr::FlatSubstAction;
using edvr::FlatSubstEvent;

struct ApiProbe {
    const void* ownerContext = nullptr;
    std::thread::id ownerThread;
    bool enabled = false;
    unsigned verifierCalls = 0;
    std::array<uint64_t, 256> sites{};
    std::array<uint64_t, 5> classes{};
    unsigned badNotes = 0;
    void reset(const void* context, bool sampling) {
        ownerContext = context;
        ownerThread = std::this_thread::get_id();
        enabled = sampling;
        verifierCalls = badNotes = 0;
        sites.fill(0);
        classes.fill(0);
    }
};
extern ApiProbe g_apiProbe;
void apiProbeThreadRejection(const Harness& h);

// --- The recorder: every call the code under test makes on the context, counted on its own vtable -----------------
// The wrapper counts its own calls (engineVelocityNoteStateCalls); a count that undercounts is exactly what a rig that
// reads the same counter cannot see. So the calls of interest are counted where D3D receives them. The context's
// vtable is a heap object of its own and the runtime re-points some slots in it between calls: an entry patched for
// ClearRenderTargetView (slot 50) was the runtime's again by the first call, and no thunk there was ever reached, so
// that method is not recorded. Every slot below is checked, by calling it, to stay ours.
enum RecSlot : unsigned {
    kOMSetRT, kOMSetRTUav, kOMSetBlend, kPSSetShader, kVSSetShader, kPSSetSrv,
    kOMGetRT, kOMGetRTUav, kOMGetBlend, kPSGetShader, kVSGetShader, kPSGetSrv,
    kVSGetCb, kOMGetDepth, kCopyResource, kRecCount
};
// ID3D11DeviceContext vtable indices (vscreen.cpp's table, the SDK's declaration order). Checked below by calling each.
constexpr size_t kVtIndex[kRecCount] = {33, 34, 35, 9, 11, 8, 89, 90, 91, 74, 76, 73, 72, 92, 47};
constexpr const char* kRecName[kRecCount] = {
    "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews", "OMSetBlendState", "PSSetShader", "VSSetShader",
    "PSSetShaderResources", "OMGetRenderTargets", "OMGetRenderTargetsAndUnorderedAccessViews", "OMGetBlendState",
    "PSGetShader", "VSGetShader", "PSGetShaderResources", "VSGetConstantBuffers", "OMGetDepthStencilState", "CopyResource"};

struct Recorder {
    ID3D11DeviceContext* ctx = nullptr;
    bool on = false;
    bool installed = false;
    void** vtable = nullptr;
    void* orig[kRecCount] = {};
    unsigned long long calls[kRecCount] = {};
    unsigned long long total() const {
        unsigned long long n = 0;
        for (unsigned long long c : calls) n += c;
        return n;
    }
    void reset() { std::memset(calls, 0, sizeof(calls)); }
    // "OMSetRenderTargets 1, PSSetShader 1": what was called, for the line that reports a count.
    std::string describe() const {
        std::string s;
        for (unsigned i = 0; i < kRecCount; ++i)
            if (calls[i]) s += (s.empty() ? "" : ", ") + std::string(kRecName[i]) + " " + std::to_string(calls[i]);
        return s.empty() ? "none" : s;
    }
};
inline Recorder g_rec;
inline bool g_rejectNextMrt6 = false;

// Model a runtime refusing one MRT6 set, while the real WARP binding/readback
// and rollback still execute. The recorder continues to count the actual call.
inline void STDMETHODCALLTYPE recTargets(ID3D11DeviceContext* self, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth) {
    if (g_rec.on && self == g_rec.ctx) ++g_rec.calls[kOMSetRT];
    ID3D11RenderTargetView* refused[8]{};
    if (g_rec.on && g_rejectNextMrt6 && count == 8 && targets && targets[6]) {
        for (unsigned i = 0; i < 8; ++i) refused[i] = targets[i];
        refused[6] = nullptr;
        targets = refused;
        g_rejectNextMrt6 = false;
    }
    reinterpret_cast<void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
        ID3D11RenderTargetView* const*, ID3D11DepthStencilView*)>(g_rec.orig[kOMSetRT])(self, count, targets, depth);
}

template <unsigned S, class... A>
void STDMETHODCALLTYPE recThunk(ID3D11DeviceContext* self, A... a) {
    if (g_rec.on && self == g_rec.ctx) ++g_rec.calls[S];
    reinterpret_cast<void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(g_rec.orig[S])(self, a...);
}

inline void recPatch(unsigned i, void* to) {
    DWORD old = 0;
    VirtualProtect(&g_rec.vtable[kVtIndex[i]], sizeof(void*), PAGE_READWRITE, &old);
    g_rec.vtable[kVtIndex[i]] = to;
    VirtualProtect(&g_rec.vtable[kVtIndex[i]], sizeof(void*), old, &old);
}
inline void recInstall(ID3D11DeviceContext* ctx) {
    static void* const thunks[kRecCount] = {
        reinterpret_cast<void*>(&recTargets),
        reinterpret_cast<void*>(&recThunk<kOMSetRTUav, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT,
                                          ID3D11UnorderedAccessView* const*, const UINT*>),
        reinterpret_cast<void*>(&recThunk<kOMSetBlend, ID3D11BlendState*, const FLOAT*, UINT>),
        reinterpret_cast<void*>(&recThunk<kPSSetShader, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT>),
        reinterpret_cast<void*>(&recThunk<kVSSetShader, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT>),
        reinterpret_cast<void*>(&recThunk<kPSSetSrv, UINT, UINT, ID3D11ShaderResourceView* const*>),
        reinterpret_cast<void*>(&recThunk<kOMGetRT, UINT, ID3D11RenderTargetView**, ID3D11DepthStencilView**>),
        reinterpret_cast<void*>(&recThunk<kOMGetRTUav, UINT, ID3D11RenderTargetView**, ID3D11DepthStencilView**, UINT, UINT,
                                          ID3D11UnorderedAccessView**>),
        reinterpret_cast<void*>(&recThunk<kOMGetBlend, ID3D11BlendState**, FLOAT*, UINT*>),
        reinterpret_cast<void*>(&recThunk<kPSGetShader, ID3D11PixelShader**, ID3D11ClassInstance**, UINT*>),
        reinterpret_cast<void*>(&recThunk<kVSGetShader, ID3D11VertexShader**, ID3D11ClassInstance**, UINT*>),
        reinterpret_cast<void*>(&recThunk<kPSGetSrv, UINT, UINT, ID3D11ShaderResourceView**>),
        reinterpret_cast<void*>(&recThunk<kVSGetCb, UINT, UINT, ID3D11Buffer**>),
        reinterpret_cast<void*>(&recThunk<kOMGetDepth, ID3D11DepthStencilState**, UINT*>),
        reinterpret_cast<void*>(&recThunk<kCopyResource, ID3D11Resource*, ID3D11Resource*>),
    };
    g_rec.ctx = ctx;
    g_rec.vtable = *reinterpret_cast<void***>(ctx);
    g_rec.reset();
    for (unsigned i = 0; i < kRecCount; ++i) {
        g_rec.orig[i] = g_rec.vtable[kVtIndex[i]];
        recPatch(i, thunks[i]);
    }
    g_rec.installed = true;
}
inline void recRemove() {
    if (!g_rec.installed) return;
    for (unsigned i = 0; i < kRecCount; ++i) recPatch(i, g_rec.orig[i]);
    g_rec.installed = false;
    g_rec.on = false;
}
// The recorder counts only while one of these lives: the rig's own setup and readbacks are not the code under test.
struct RecOn {
    bool was;
    RecOn() : was(g_rec.on) { g_rec.on = true; }
    ~RecOn() { g_rec.on = was; }
};
struct RecOff {
    bool was;
    RecOff() : was(g_rec.on) { g_rec.on = false; }
    ~RecOff() { g_rec.on = was; }
};

// --- What the game has bound, and what the context actually holds -------------------------------------------------
struct GameState {
    ID3D11PixelShader* ps = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* t3 = nullptr;
};
struct Actual {
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RenderTargetView> rtv[8];
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> t3;
};
inline Actual readActual(ID3D11DeviceContext* ctx) {
    RecOff off;
    Actual a;
    ctx->PSGetShader(&a.ps, nullptr, nullptr);
    ctx->VSGetShader(&a.vs, nullptr, nullptr);
    float f[4] = {};
    UINT m = 0;
    ctx->OMGetBlendState(&a.blend, f, &m);
    ID3D11RenderTargetView* rt[8] = {};
    ID3D11DepthStencilView* d = nullptr;
    ctx->OMGetRenderTargets(8, rt, &d);
    for (unsigned i = 0; i < 8; ++i) a.rtv[i].Attach(rt[i]);
    a.dsv.Attach(d);
    ctx->PSGetShaderResources(3, 1, &a.t3);
    return a;
}
// Exactly the game's state?
inline bool holdsGame(const Actual& a, const GameState& g, std::string* why) {
    if (a.ps.Get() != g.ps) { *why = "the pixel shader is not the game's"; return false; }
    if (a.vs.Get() != g.vs) { *why = "the vertex shader is not the game's"; return false; }
    if (a.blend.Get() != g.blend) { *why = "the blend state is not the game's"; return false; }
    for (unsigned i = 0; i < 8; ++i)
        if (a.rtv[i].Get() != g.rtv[i]) { *why = "render target " + std::to_string(i) + " is not the game's"; return false; }
    if (a.dsv.Get() != g.dsv) { *why = "the depth view is not the game's"; return false; }
    if (a.t3.Get() != g.t3) { *why = "the pixel shader resource at t3 is not the game's"; return false; }
    return true;
}
// EDVR's substitution, for a producer draw: its own pixel shader, MRT6 bound, a derived blend state, and the game's
// other targets kept. The guard's private t3 is its own case (guard).
inline bool holdsEdvr(const Actual& a, const GameState& g, bool guard, std::string* why) {
    if (!a.ps || a.ps.Get() == g.ps) { *why = "the pixel shader is not EDVR's patched one"; return false; }
    if (!a.rtv[6]) { *why = "MRT6 is not bound"; return false; }
    for (unsigned i = 0; i < 8; ++i)
        if (i != 6 && a.rtv[i].Get() != g.rtv[i]) { *why = "render target " + std::to_string(i) + " is not the game's"; return false; }
    if (a.dsv.Get() != g.dsv) { *why = "the depth view is not the game's"; return false; }
    if (!a.blend || a.blend.Get() == g.blend) { *why = "the blend state is not the derived one"; return false; }
    if (guard ? (!a.t3 || a.t3.Get() == g.t3) : a.t3.Get() != g.t3) {
        *why = guard ? "the guard's private t3 is not bound" : "t3 is not the game's";
        return false;
    }
    return true;
}

inline edvr::EngineVelocityFlushCause causeOf(FlatSubstEvent e) {
    switch (e) {
    case FlatSubstEvent::kDispatch: return edvr::EngineVelocityFlushCause::kDispatch;
    case FlatSubstEvent::kClear: return edvr::EngineVelocityFlushCause::kClear;
    case FlatSubstEvent::kCopy: return edvr::EngineVelocityFlushCause::kCopy;
    case FlatSubstEvent::kResolve: return edvr::EngineVelocityFlushCause::kResolve;
    case FlatSubstEvent::kKeepTargets: return edvr::EngineVelocityFlushCause::kKeepTargets;
    case FlatSubstEvent::kExecuteCommandList: return edvr::EngineVelocityFlushCause::kCommandList;
    case FlatSubstEvent::kPresent: return edvr::EngineVelocityFlushCause::kPresent;
    default: return edvr::EngineVelocityFlushCause::kOtherDraw;
    }
}

// --- The game and the flat runtime, emulated ----------------------------------------------------------------------
// The game's calls do what their hooks do (the real call, then the binding shadow) and update what the game believes
// it has bound; the runtime's side is the policy under test plus the engine functions the flat scope calls.
template <class Faults = edvr::FlatSubstNoFaults>
class Emu {
public:
    Emu(const Harness& h, Game& g) : h_(h), g_(g), ctx_(g.ctx) {}
    GameState exp;
    bool ok = true;
    std::string why;
    unsigned producers = 0;
    bool overlayReady = false;
    ComPtr<ID3D11VertexShader> overlayVs;
    ComPtr<ID3D11PixelShader> overlayPs;
    ComPtr<ID3D11DepthStencilState> overlayDepth;
    ComPtr<ID3D11ShaderResourceView> sentinel;
    ComPtr<ID3D11Texture2D> sentinelTexture;
    ComPtr<ID3D11BlendState> additive;

    void fail(const std::string& m) {
        if (ok) why = m;
        ok = false;
    }

    // ---- the game's calls -----------------------------------------------------------------------------
    void gameSetPs(ID3D11PixelShader* p, uint64_t hash) { g_.setPs(p, hash); exp.ps = p; }
    void gameSetVs(ID3D11VertexShader* p, uint64_t hash) { g_.setVs(p, hash); exp.vs = p; }
    void gameSetBlend(ID3D11BlendState* b) { g_.setBlend(b); exp.blend = b; }
    void gameSetT3(ID3D11ShaderResourceView* v) {
        ID3D11ShaderResourceView* one = v;
        ctx_->PSSetShaderResources(3, 1, &one);
        g_.shadow(BindSlot::PsSrv3, v);
        exp.t3 = v;
    }
    // The game binds its render targets (the same four as the pass starts with, or the first `count` of them).
    void gameBindTargets(unsigned count = 4) {
        ID3D11RenderTargetView* r[4] = {g_.sourceRtv[0].Get(), g_.sourceRtv[1].Get(), g_.sourceRtv[2].Get(), g_.sourceRtv[3].Get()};
        ctx_->OMSetRenderTargets(count, r, g_.sourceDsv.Get());
        g_.shadow(BindSlot::Rtv0, r[0]);
        g_.shadow(BindSlot::Dsv0, g_.sourceDsv.Get());
        for (unsigned i = 0; i < 8; ++i) exp.rtv[i] = i < count ? r[i] : nullptr;
        exp.dsv = g_.sourceDsv.Get();
    }
    // The game changes state through a path nothing hooks: the real call and no shadow update, so no shortcut can see it.
    void gameSetBlendUnhooked(ID3D11BlendState* b) {
        const float f[4] = {};
        ctx_->OMSetBlendState(b, f, ~0u);
        exp.blend = b;
    }
    void gameBindTargetsUnhooked(unsigned count) {
        ID3D11RenderTargetView* r[4] = {g_.sourceRtv[0].Get(), g_.sourceRtv[1].Get(), g_.sourceRtv[2].Get(), g_.sourceRtv[3].Get()};
        ctx_->OMSetRenderTargets(count, r, g_.sourceDsv.Get());
        for (unsigned i = 0; i < 8; ++i) exp.rtv[i] = i < count ? r[i] : nullptr;
        exp.dsv = g_.sourceDsv.Get();
    }
    // The binding shadow's generations move with nothing rebound (its once-a-frame bump, a set that kept the targets):
    // the wrapper reads the context's render targets again, and finds its own MRT6 among them.
    void bumpTargetGenerations() {
        g_.shadow(BindSlot::Rtv0, g_.sourceRtv[0].Get());
        g_.shadow(BindSlot::Dsv0, g_.sourceDsv.Get());
    }
    // The on-foot frame as the game starts it: the naming, the pass's bindings. Nothing drawn.
    void startFrame() {
        g_.beginFrame();
        g_.writeScene(g_.sceneA.Get(), g_.rows[0]);
        edvr::engineVelocityNoteSource(g_.sourceDepth.Get(), g_.sceneA.Get());
        g_.sourcePass(0);
        exp = GameState{};
        exp.ps = g_.ps.Get();
        exp.vs = g_.vs.Get();
        exp.blend = nullptr;
        exp.dsv = g_.sourceDsv.Get();
        for (unsigned i = 0; i < 4; ++i) exp.rtv[i] = g_.sourceRtv[i].Get();
        if (sentinel) gameSetT3(sentinel.Get());
    }
    // The first frame the source is named in has no scene-constant rows the watch has seen, so its draw is declined, as it
    // always was (the S1 case says so): a frame to get past that before anything is measured.
    void warmUp() {
        startFrame();
        bool had6 = false;
        const bool substituted = edvr::engineVelocityFlatBeginDraw(ctx_, &had6);
        ctx_->DrawInstanced(4, 1, 0, 0);
        if (substituted) edvr::engineVelocityFlatEndDraw(ctx_);
        present();
    }
    // ClearState, and a fresh pass after it (the game binds everything again).
    void restartPass() {
        g_.sourcePass(0);
        g_.setPool(g_.viewA.Get());   // ClearState took the pool view and the scene constants too
        g_.setScene(g_.sceneA.Get());
        exp = GameState{};
        exp.vs = g_.vs.Get();
        exp.dsv = g_.sourceDsv.Get();
        for (unsigned i = 0; i < 4; ++i) exp.rtv[i] = g_.sourceRtv[i].Get();
        gameSetPs(g_.ps.Get(), lifecycle_tests::kPsHash);
        gameSetBlend(nullptr);
        if (sentinel) gameSetT3(sentinel.Get());
    }

    // ---- the runtime's side ---------------------------------------------------------------------------
    // flatRuntimeSubstitution: the policy decides, the engine acts.
    // `eventCalls`: the context calls the decision made (a flush is a dozen; forgetting is none).
    unsigned long long eventCalls = 0;
    void substitution(FlatSubstEvent e) {
        eventCalls = 0;
        if (!ok || !edvr::engineVelocityFlatPending()) return;
        const unsigned long long before = g_rec.total();
        switch (edvr::flatSubstAction<Faults>(e)) {
        case FlatSubstAction::kFlush: {
            RecOn on;
            edvr::engineVelocityFlatFlush(ctx_, causeOf(e));
            break;
        }
        case FlatSubstAction::kAbandon: {
            RecOn on;
            edvr::engineVelocityFlatAbandon();
            break;
        }
        default: break;
        }
        eventCalls = g_rec.total() - before;
    }
    void expectGame(const char* what) {
        if (!ok) return;
        std::string reason;
        if (!holdsGame(readActual(ctx_), exp, &reason)) fail(std::string(what) + ": " + reason);
    }
    // A hooked call that is not a substituted producer draw: the hook calls the runtime, then the real call runs.
    void action(FlatSubstEvent e, const char* what) {
        substitution(e);
        expectGame(what);
    }
    // The flat scope's producer branch, the real draw, and the scope's end.
    bool producer(const char* what, bool guard = false) {
        if (!ok) return false;
        bool had6 = true, substituted = false;
        {
            RecOn on;
            substituted = edvr::engineVelocityFlatBeginDraw(ctx_, &had6);
        }
        if (!substituted) {
            fail(std::string(what) + ": the engine declined a producer draw");
            return false;
        }
        ++producers;
        if (had6) fail(std::string(what) + ": the game's slot 6 was reported occupied");
        std::string reason;
        if (!holdsEdvr(readActual(ctx_), exp, guard, &reason)) fail(std::string(what) + ": " + reason);
        ctx_->DrawInstanced(4, 1, 0, 0);
        {
            RecOn on;
            edvr::engineVelocityFlatEndDraw(ctx_);
        }
        return true;
    }
    // A candidate producer draw the slow half declines AFTER it bound MRT6 (a blend state it cannot derive): the game's
    // state is fully back before the draw, whatever the visit bound on the way, and nothing is left pending.
    void declinedProducer(const char* what) {
        if (!ok) return;
        bool had6 = false, substituted = true;
        {
            RecOn on;
            substituted = edvr::engineVelocityFlatBeginDraw(ctx_, &had6);
        }
        if (substituted) { fail(std::string(what) + ": the engine substituted a draw it had to decline"); return; }
        expectGame(what);
        if (ok && edvr::engineVelocityFlatPending()) fail(std::string(what) + ": a declined draw left the bracket pending");
        if (ok) ctx_->DrawInstanced(4, 1, 0, 0);
    }
    void otherDraw(const char* what) {
        action(FlatSubstEvent::kOtherDraw, what);
        if (ok) ctx_->DrawInstanced(4, 1, 0, 0);
    }
    void dispatch() {
        action(FlatSubstEvent::kDispatch, "a dispatch");
        if (ok) ctx_->Dispatch(1, 1, 1);
    }
    void clear() {
        action(FlatSubstEvent::kClear, "a clear");
        const float black[4] = {};
        if (ok) ctx_->ClearRenderTargetView(g_.sourceRtv[0].Get(), black);
    }
    void copy() {
        action(FlatSubstEvent::kCopy, "a copy");
        if (ok) ctx_->CopyResource(g_.sourceColour[1].Get(), g_.sourceColour[0].Get());
    }
    void resolve() { action(FlatSubstEvent::kResolve, "a resolve"); }
    // OMSetRenderTargetsAndUnorderedAccessViews that keeps the render targets and sets UAVs beside them: the game's own
    // targets have to be what the UAV slots follow, so the hook asks the runtime first.
    void keepTargetsSet() { action(FlatSubstEvent::kKeepTargets, "a set that keeps the render targets and sets UAVs"); }
    void commandList() { action(FlatSubstEvent::kExecuteCommandList, "a command list"); }
    void present() {
        action(FlatSubstEvent::kPresent, "the present");
        edvr::engineVelocityFlatFrameEnd();   // the runtime's BeforePresent: the flush, then what the frame kept goes
        if (ok) g_.endFrame();
    }
    // The game's Get of state EDVR's substitution still holds: not a hooked call, so it reads what is bound.
    void gameGetPixelShader(const char* what, bool expectEdvrs) {
        ComPtr<ID3D11PixelShader> seen;
        {
            RecOff off;
            ctx_->PSGetShader(&seen, nullptr, nullptr);
        }
        if (expectEdvrs && (!seen || seen.Get() == exp.ps)) fail(std::string(what) + ": the unhooked Get did not read EDVR's shader");
        if (!expectEdvrs && seen.Get() != exp.ps) fail(std::string(what) + ": the game's Get did not read its own shader");
    }
    void clearState() {
        if (!ok) return;
        // As the hook does it: the shadow forgets everything, the runtime is told, then the real ClearState.
        for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
        substitution(FlatSubstEvent::kClearState);
        // The context lost its state and may be gone: the runtime forgets, and touches nothing.
        if (eventCalls != 0) fail("ClearState: EDVR made " + std::to_string(eventCalls) + " context calls to put state back");
        ctx_->ClearState();
        exp = GameState{};
        expectGame("after ClearState");
        if (edvr::engineVelocityFlatPending()) fail("ClearState left the bracket pending");
    }
    void resize() {
        if (!ok) return;
        substitution(FlatSubstEvent::kResize);
        if (eventCalls != 0) fail("a resize: EDVR made " + std::to_string(eventCalls) + " context calls to put state back on a context that may be gone");
        if (edvr::engineVelocityFlatPending()) fail("a resize left the bracket pending");
    }

private:
    const Harness& h_;
    Game& g_;
    ID3D11DeviceContext* ctx_;
};

// The blend state the game sets between draws.
inline ComPtr<ID3D11BlendState> makeAdditive(const Harness& h) {
    D3D11_BLEND_DESC add = edvr::engineVelocityDefaultBlend();
    add.RenderTarget[0].BlendEnable = TRUE;
    add.RenderTarget[0].SrcBlend = add.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    add.RenderTarget[0].SrcBlendAlpha = add.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    ComPtr<ID3D11BlendState> state;
    h.check(SUCCEEDED(h.device->CreateBlendState(&add, &state)), "flat lazy: the game's blend state");
    return state;
}

// Leaves the engine and the context as the next case finds them, whichever way a case returns.
struct SceneCleanup {
    ID3D11DeviceContext* ctx;
    explicit SceneCleanup(ID3D11DeviceContext* c) : ctx(c) {}
    ~SceneCleanup() {
        edvr::engineVelocityFlatAbandon();
        edvr::engineVelocityShutdown();
        ctx->ClearState();
        edvr::engineVelocityFlatLazy(false);
        edvr::flatQueryCut().reset();
    }
};

// One mixed scene: every event the policy names, in an order that puts each after a run of lazily kept producer draws.
template <class Faults>
bool mixedScene(const Harness& h, std::string* why) {
    lifecycle_fake::g_hookLive = true;
    edvr::g_clockForTest = &lifecycle_fake::fakeClock;
    Game g(h);
    g.setup();
    SceneCleanup cleanup(g.ctx);
    for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
    edvr::engineVelocityConfigure(true);
    edvr::engineVelocityFlatLazy(true);
    g.makeSource(40, 24);
    Emu<Faults> e(h, g);
    e.additive = makeAdditive(h);
    ComPtr<ID3D11BlendState> logicBlend;   // a blend state the derived one cannot express: a D3D11.1 logic op
    {
        ComPtr<ID3D11Device1> dev1;
        D3D11_BLEND_DESC1 lb{};
        lb.RenderTarget[0].LogicOpEnable = TRUE;
        lb.RenderTarget[0].LogicOp = D3D11_LOGIC_OP_COPY;
        lb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState1> lb1;
        h.check(SUCCEEDED(g.dev->QueryInterface(IID_PPV_ARGS(&dev1))) && SUCCEEDED(dev1->CreateBlendState1(&lb, &lb1)) &&
                    SUCCEEDED(lb1.As(&logicBlend)),
                "flat lazy: a blend state with a logic op (WARP takes it)");
    }
    {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = sd.Height = sd.MipLevels = sd.ArraySize = sd.SampleDesc.Count = 1;
        sd.Format = DXGI_FORMAT_R32G32_FLOAT;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        h.check(SUCCEEDED(g.dev->CreateTexture2D(&sd, nullptr, &e.sentinelTexture)) &&
                    SUCCEEDED(g.dev->CreateShaderResourceView(e.sentinelTexture.Get(), nullptr, &e.sentinel)),
                "flat lazy: the game's t3");
    }

    // Frame 1: a run, the game changes its pixel shader between draws, then its blend state, then draws elsewhere.
    e.warmUp();
    e.startFrame();
    e.producer("the first producer draw of a frame");
    e.producer("a second producer draw, nothing changed between");
    e.producer("a third");
    e.gameGetPixelShader("a game Get between producer draws", true);
    e.gameSetPs(g.ps2.Get(), lifecycle_tests::kPsHash2);
    e.producer("the game set another pixel shader: EDVR patches that one");
    e.gameSetPs(g.ps.Get(), lifecycle_tests::kPsHash);
    e.producer("and set the first back");
    e.gameSetBlend(e.additive.Get());
    e.producer("the game set its blend state: derived again");
    e.otherDraw("a draw that is not a producer");
    e.gameGetPixelShader("a game Get after a hooked call put the game's state back", false);
    e.producer("a new run after another draw");
    e.producer("and its second");
    e.gameSetBlend(nullptr);
    e.dispatch();
    e.producer("a run after a dispatch");
    e.clear();
    e.producer("a run after a clear");
    e.producer("its second");
    e.copy();
    e.producer("a run after a copy");
    e.resolve();
    e.producer("a run after a resolve");
    e.commandList();
    e.producer("a run after a command list");
    // The game rebinds its render targets between two producer draws (a new pass): MRT6 goes with the old set, and
    // the next restore must put back the NEW set, not the one saved before.
    e.gameBindTargets();
    e.producer("the game rebound its render targets");
    e.producer("and a second draw of that pass");
    e.keepTargetsSet();
    e.producer("a run after a set that kept the targets");
    // A rebind to a DIFFERENT set, then a draw that is not a producer: the restore must not put the old set back over
    // the game's new one (the generation says the game has rebound since).
    e.gameBindTargets(2);
    e.otherDraw("a draw after the game rebound a smaller set: the game's set stays");
    e.producer("a producer draw with the smaller set");
    e.producer("and a second");
    // The generations move with nothing rebound while MRT6 is bound: the read the wrapper makes has its own MRT6 in it,
    // which is not the game's, and comes off with the next restore, whether the draw is substituted or not.
    e.bumpTargetGenerations();
    e.producer("the generations moved under a bound MRT6: still substituted");
    e.otherDraw("and the next draw that is not a producer finds the game's set without MRT6");
    e.producer("a run again");
    e.bumpTargetGenerations();
    e.gameSetPs(g.unkeyed.Get(), lifecycle_tests::kUnkeyedPs);
    e.declinedProducer("the generations moved and the pixel shader is not one EDVR patches: declined, with MRT6 still on");
    e.gameSetPs(g.ps.Get(), lifecycle_tests::kPsHash);
    e.producer("a run again, with a shader it patches");
    e.bumpTargetGenerations();
    e.gameSetBlend(logicBlend.Get());
    e.declinedProducer("the generations moved and the blend cannot be derived: declined with MRT6 still on");
    e.gameSetBlend(nullptr);
    e.gameBindTargets();
    e.present();
    if (!e.ok) { if (why) *why = e.why; return false; }

    // A draw the slow half declines AFTER it bound MRT6: the game's blend state carries a logic op, which the derived one
    // cannot express. The game's own render targets must be back before that draw (the visit put MRT6 on over a set the
    // wrapper had read), and the next draw, with a blend it can derive, is substituted again.
    e.startFrame();
    e.producer("a producer draw before the refused blend");
    e.gameBindTargets();   // a new pass: MRT6 went with the old set, and the next draw binds it again
    e.gameSetBlend(logicBlend.Get());
    e.declinedProducer("a draw whose blend cannot be derived, after MRT6 went on");
    e.gameSetBlend(nullptr);
    e.producer("the next draw, with a blend it can derive");
    e.gameSetBlend(logicBlend.Get());   // the run above is still kept bound: the game's new blend state arrives over it
    e.declinedProducer("a draw with a logic-op blend, from a run that was kept bound");
    e.present();
    if (!e.ok) { if (why) *why = e.why; return false; }

    // Frame 2: the Present with nothing kept is nothing to do; ClearState and a resize with a run pending forget it.
    e.startFrame();
    e.present();
    e.startFrame();
    e.producer("a run before ClearState");
    e.producer("its second");
    e.clearState();
    e.restartPass();
    e.producer("a run after ClearState re-binds");
    e.present();
    e.startFrame();
    e.producer("a run before a resize");
    e.resize();
    if (!e.ok) { if (why) *why = e.why; return false; }
    // (the resize left the context as it was: the game's own next call flushes nothing and finds EDVR's state; the
    // runtime would have torn the device down. Put the game's state back for the cleanup below.)
    e.clearState();
    e.restartPass();
    e.present();

    // The overlay guard. Its shaders are the flight's exact pair, the depth state writes no depth.
    {
        const auto vb = g.compile(std::string(shader_tests::kVsCommon) + shader_tests::kVsB, "vs_5_0");
        const auto pb = g.compile(shader_tests::kPsB, "ps_5_0");
        constexpr uint64_t overlayVsHash = 0xBBE58E40FE88EC80ull, overlayPsHash = 0xDB3E8D20CF53FBC0ull;
        h.check(SUCCEEDED(g.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &e.overlayVs)) &&
                    SUCCEEDED(g.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &e.overlayPs)),
                "flat lazy: the overlay pair's original shaders");
        edvr::engineVelocityRememberVs(e.overlayVs.Get(), overlayVsHash, vb->GetBufferPointer(), vb->GetBufferSize(), false);
        edvr::engineVelocityRememberPs(e.overlayPs.Get(), overlayPsHash, pb->GetBufferPointer(), pb->GetBufferSize(), false);
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        h.check(SUCCEEDED(g.dev->CreateDepthStencilState(&dd, &e.overlayDepth)), "flat lazy: the overlay depth state");
        auto underlay = [&](const char* what) {
            e.gameSetVs(g.vs.Get(), lifecycle_tests::kVsHash);
            e.gameSetPs(g.ps.Get(), lifecycle_tests::kPsHash);
            g.ctx->OMSetDepthStencilState(g.depthState.Get(), 0);
            e.producer(what);
        };
        auto overlay = [&](const char* what) {
            g.ctx->VSSetShader(e.overlayVs.Get(), nullptr, 0);
            g.shadow(BindSlot::Vs, e.overlayVs.Get(), overlayVsHash);
            e.exp.vs = e.overlayVs.Get();
            e.gameSetPs(e.overlayPs.Get(), overlayPsHash);
            g.ctx->OMSetDepthStencilState(e.overlayDepth.Get(), 5);
            e.producer(what, true);
            e.expectGame((std::string("after ") + what + ": the game's t3 is back").c_str());
        };
        e.startFrame();
        underlay("an underlay producer");
        overlay("an overlay draw after a lazily kept underlay draw");
        overlay("a second overlay draw of the group");
        underlay("an underlay draw after the overlay draws: t3 is the game's");
        overlay("an overlay draw of a new group");
        e.present();
    }
    if (!e.ok) { if (why) *why = e.why; return false; }
    return true;
}

// Producer draws in runs, in the restore-after-every-draw form (what the flat scope did before the lazy bracket) and in the
// lazy form, with the kept answers (flat_query_cut.h) answering or every state asking the context: the calls the context
// received. `runs` runs of `draws` consecutive draws, a draw that is not a producer between two runs. `gameSetsPs`: the
// game binds its own pixel shader before every draw, as it does between materials, so the lazy form still patches each one.
enum class Form { PerDraw, Lazy, LazyAsking };
inline std::string g_lastRunDescription;   // what the last countRuns' calls were, by method
inline unsigned long long countRuns(const Harness& h, Form form, unsigned runs, unsigned draws, bool gameSetsPs) {
    Game g(h);
    g.setup();
    SceneCleanup cleanup(g.ctx);
    for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
    edvr::engineVelocityConfigure(true);
    const bool lazy = form != Form::PerDraw;
    edvr::engineVelocityFlatLazy(lazy);
    edvr::flatQueryCut().reset();
    if (form == Form::LazyAsking) edvr::flatQueryCut().fallBackAll();
    g.makeSource(40, 24);
    Emu<> e(h, g);
    e.warmUp();
    e.startFrame();
    e.producer("the frame's first draw, uncounted");   // the eye-frame's own preparation is the same in every form
    // The old form kept nothing between draws: what the bracket held after that first draw is not part of it.
    if (form == Form::PerDraw) edvr::engineVelocityFlatAbandon();
    g_rec.reset();
    for (unsigned run = 0; run < runs; ++run) {
        if (run) e.otherDraw("a draw between two runs");
        for (unsigned i = 0; i < draws; ++i) {
            if (gameSetsPs) e.gameSetPs(i % 2 ? g.ps2.Get() : g.ps.Get(), i % 2 ? lifecycle_tests::kPsHash2 : lifecycle_tests::kPsHash);
            if (lazy) {
                RecOn on;
                bool had6 = false;
                edvr::engineVelocityFlatBeginDraw(g.ctx, &had6);
                {
                    RecOff off;
                    g.ctx->DrawInstanced(4, 1, 0, 0);
                }
                edvr::engineVelocityFlatEndDraw(g.ctx);
            } else {
                // What FlatRuntimeDrawScope did per producer draw: read the eight targets, BeforeDraw, the draw, the
                // wrapper's restore, and the eight targets set back.
                RecOn on;
                ID3D11RenderTargetView* rt[8] = {};
                ID3D11DepthStencilView* d = nullptr;
                g.ctx->OMGetRenderTargets(8, rt, &d);
                edvr::engineVelocityBeforeDraw(g.ctx, false);
                {
                    RecOff off;
                    g.ctx->DrawInstanced(4, 1, 0, 0);
                }
                edvr::engineVelocityAfterFlatDraw(g.ctx);
                g.ctx->OMSetRenderTargets(8, rt, d);
                for (auto* v : rt) if (v) v->Release();
                if (d) d->Release();
            }
        }
    }
    e.present();   // the lazy form's restore, at the frame's end (counted where the policy issues it)
    g_lastRunDescription = g_rec.describe();
    return g_rec.total();
}
inline unsigned long long countRun(const Harness& h, bool lazy, unsigned draws, bool gameSetsPs) {
    return countRuns(h, lazy ? Form::Lazy : Form::PerDraw, 1, draws, gameSetsPs);
}

// The overlay counters of the guard sequence, eager and lazy: the same, or the lazy form declined something.
struct OverlayCounts {
    unsigned long long copies = ~0ull, guarded = ~0ull, declined = ~0ull;
};
inline OverlayCounts overlaySequence(const Harness& h, bool lazy) {
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    Game g(h);
    g.setup();
    SceneCleanup cleanup(g.ctx);
    for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
    edvr::engineVelocityConfigure(true);
    edvr::engineVelocityFlatLazy(lazy);
    g.makeSource(40, 24);
    Emu<> e(h, g);
    {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = sd.Height = sd.MipLevels = sd.ArraySize = sd.SampleDesc.Count = 1;
        sd.Format = DXGI_FORMAT_R32G32_FLOAT;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        h.check(SUCCEEDED(g.dev->CreateTexture2D(&sd, nullptr, &e.sentinelTexture)) &&
                    SUCCEEDED(g.dev->CreateShaderResourceView(e.sentinelTexture.Get(), nullptr, &e.sentinel)),
                "flat lazy: the game's t3");
    }
    const auto vb = g.compile(std::string(shader_tests::kVsCommon) + shader_tests::kVsB, "vs_5_0");
    const auto pb = g.compile(shader_tests::kPsB, "ps_5_0");
    constexpr uint64_t overlayVsHash = 0xBBE58E40FE88EC80ull, overlayPsHash = 0xDB3E8D20CF53FBC0ull;
    h.check(SUCCEEDED(g.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &e.overlayVs)) &&
                SUCCEEDED(g.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &e.overlayPs)),
            "flat lazy: the overlay pair's original shaders");
    edvr::engineVelocityRememberVs(e.overlayVs.Get(), overlayVsHash, vb->GetBufferPointer(), vb->GetBufferSize(), false);
    edvr::engineVelocityRememberPs(e.overlayPs.Get(), overlayPsHash, pb->GetBufferPointer(), pb->GetBufferSize(), false);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    h.check(SUCCEEDED(g.dev->CreateDepthStencilState(&dd, &e.overlayDepth)), "flat lazy: the overlay depth state");
    auto underlay = [&] {
        e.gameSetVs(g.vs.Get(), lifecycle_tests::kVsHash);
        e.gameSetPs(g.ps.Get(), lifecycle_tests::kPsHash);
        g.ctx->OMSetDepthStencilState(g.depthState.Get(), 0);
        e.producer("overlay sequence: an underlay draw");
    };
    auto overlay = [&] {
        g.ctx->VSSetShader(e.overlayVs.Get(), nullptr, 0);
        g.shadow(BindSlot::Vs, e.overlayVs.Get(), overlayVsHash);
        e.exp.vs = e.overlayVs.Get();
        e.gameSetPs(e.overlayPs.Get(), overlayPsHash);
        g.ctx->OMSetDepthStencilState(e.overlayDepth.Get(), 5);
        e.producer("overlay sequence: an overlay draw", true);
    };
    e.warmUp();
    const size_t mark = lifecycle_fake::g_log.size();
    e.startFrame();
    underlay(); overlay(); overlay(); underlay(); overlay();
    e.present();
    e.startFrame();
    underlay(); overlay();
    e.present();
    g.endFrame(true);
    const std::string line = lifecycle_tests::lastLine("engine motion: flat overlay guard:", mark);
    OverlayCounts c;
    c.copies = lifecycle_tests::number(line, "copies ");
    c.guarded = lifecycle_tests::number(line, "guarded draws ");
    c.declined = lifecycle_tests::number(line, "declined state ") + lifecycle_tests::number(line, "resource ") +
                 lifecycle_tests::number(line, "shader ");
    if (!e.ok) std::printf("  flat lazy: the overlay sequence failed at: %s\n", e.why.c_str());
    h.check(e.ok, "flat lazy: the overlay sequence keeps the game's state");
    return c;
}

// --- The kept answers, and what samples them ---------------------------------------------------------------------------------
struct CutOutcome {
    bool held = false;   // the context was the game's after every game call and EDVR's after every producer begin
    std::string why;
    edvr::FlatQueryCounts counts;
    std::string lines;   // what the log said, one per line
};

// One scene: a run of producer draws (the blend state, the render targets and the acceptance of MRT6 are read and
// kept), a draw that is not one, then the game changes `what` through a path nothing hooks, and another run and another
// draw follow. `checking`: the frame is one of the checked ones (one in 64).
enum class Unhooked { Blend, Targets };
inline CutOutcome unhookedScene(const Harness& h, Unhooked what, bool checking) {
    CutOutcome out;
    lifecycle_fake::g_hookLive = true;
    edvr::g_clockForTest = &lifecycle_fake::fakeClock;
    Game g(h);
    g.setup();
    SceneCleanup cleanup(g.ctx);
    for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
    edvr::engineVelocityConfigure(true);
    edvr::engineVelocityFlatLazy(true);
    edvr::flatQueryCut().reset();
    g.makeSource(40, 24);
    Emu<> e(h, g);
    e.additive = makeAdditive(h);
    const size_t mark = lifecycle_fake::g_log.size();
    e.warmUp();
    e.startFrame();
    e.producer("a run: what the bracket reads is read and kept");
    e.producer("its second draw");
    e.otherDraw("a draw that is not a producer: the game's state goes back, what was kept stays");
    if (checking) edvr::flatQueryCut().beginFrame(0);
    if (what == Unhooked::Blend) e.gameSetBlendUnhooked(e.additive.Get());
    else e.gameBindTargetsUnhooked(2);
    e.producer("the next run, under a change no setter hook saw");
    e.otherDraw("and what comes back is the game's own");
    if (what == Unhooked::Blend) e.gameSetBlendUnhooked(nullptr);
    else e.gameBindTargetsUnhooked(4);
    e.producer("another run, after whatever the check decided");
    e.otherDraw("and again the game's own");
    e.present();
    out.held = e.ok;
    out.why = e.why;
    out.counts = edvr::flatQueryCut().take();
    for (size_t i = mark; i < lifecycle_fake::g_log.size(); ++i)
        if (lifecycle_fake::g_log[i].find("flat query shortcut") != std::string::npos) out.lines += lifecycle_fake::g_log[i] + "\n";
    return out;
}

// The frame's end lets go of what the bracket kept: the game's render-target views and its blend state are not held past
// the frame, and ClearState lets go at once. Counted on the COM reference counts of a view and a blend state the game
// bound.
inline bool releaseScene(const Harness& h, std::string* why) {
    Game g(h);
    g.setup();
    SceneCleanup cleanup(g.ctx);
    for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
    edvr::engineVelocityConfigure(true);
    edvr::engineVelocityFlatLazy(true);
    edvr::flatQueryCut().reset();
    g.makeSource(40, 24);
    Emu<> e(h, g);
    e.additive = makeAdditive(h);
    ID3D11RenderTargetView* view = g.sourceRtv[0].Get();
    ID3D11BlendState* blend = e.additive.Get();
    auto refs = [&] { view->AddRef(); return view->Release(); };         // the view's COM count as it stands
    auto blendRefs = [&] { blend->AddRef(); return blend->Release(); };  // and the blend state's
    e.warmUp();
    e.startFrame();
    e.gameSetBlend(blend);
    e.producer("a first run: the derived blend state is made, and stays in its cache");
    e.otherDraw("a draw that is not a producer");
    e.present();
    e.clearState();
    const ULONG unbound = refs(), blendUnbound = blendRefs();   // nothing bound, nothing kept (the cache's own reference stays)
    e.startFrame();
    e.gameSetBlend(blend);
    e.present();
    const ULONG quiet = refs(), blendQuiet = blendRefs();       // a frame ended with nothing kept: what the game and the context hold
    e.startFrame();
    e.gameSetBlend(blend);
    e.producer("a run");
    e.otherDraw("a draw that is not a producer");
    const ULONG during = refs(), blendDuring = blendRefs();
    e.present();
    const ULONG after = refs(), blendAfter = blendRefs();
    e.startFrame();
    e.gameSetBlend(blend);
    e.producer("a run before ClearState");
    const ULONG held = refs(), blendHeld = blendRefs();
    e.clearState();
    const ULONG cleared = refs(), blendCleared = blendRefs();    if (!e.ok) { *why = e.why; return false; }
    if (during <= quiet || blendDuring <= blendQuiet) { *why = "the bracket held no reference to the game's view or blend state mid-frame (the scene proves nothing)"; return false; }
    if (after != quiet) { *why = "the Present left the game's render-target view held"; return false; }
    if (blendAfter != blendQuiet) { *why = "the Present left the game's blend state held"; return false; }
    if (held <= unbound || cleared != unbound) { *why = "ClearState left the game's render-target view held"; return false; }
    if (blendHeld <= blendUnbound || blendCleared != blendUnbound) { *why = "ClearState left the game's blend state held"; return false; }
    return true;
}
// The coverage classification's two questions (flat_query_reads.h), through the same functions the runtime calls, against
// the context itself: the binding shadow follows hooked setters (ClearState included) and does not follow a real call
// nothing hooked, which one frame in 64 finds.
struct CoverageOutcome {
    bool ok = true;
    std::string why;
};
inline CoverageOutcome coverageScene(const Harness& h) {
    CoverageOutcome out;
    auto fail = [&](const std::string& m) { if (out.ok) out.why = m; out.ok = false; };
    Game g(h);
    g.setup();
    SceneCleanup cleanup(g.ctx);
    for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
    g.makeSource(40, 24);
    edvr::FlatQueryCut& cut = edvr::flatQueryCut();
    cut.reset();
    std::vector<std::string> said;
    auto log = [&](const char* line) { said.push_back(line); };
    ComPtr<ID3D11Resource> depthRes;
    g.sourceDsv->GetResource(&depthRes);
    const void* depthIdentity = depthRes.Get();
    // A second depth view of the same shape, for the change nothing hooks.
    ComPtr<ID3D11Texture2D> otherDepth;
    ComPtr<ID3D11DepthStencilView> otherDsv;
    {
        D3D11_TEXTURE2D_DESC td{};
        g.sourceDepth->GetDesc(&td);
        h.check(SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &otherDepth)) &&
                    SUCCEEDED(g.dev->CreateDepthStencilView(otherDepth.Get(), nullptr, &otherDsv)),
                "flat lazy: a second depth view");
    }
    ComPtr<ID3D11Resource> otherRes;
    otherDsv->GetResource(&otherRes);
    ID3D11RenderTargetView* r4[4] = {g.sourceRtv[0].Get(), g.sourceRtv[1].Get(), g.sourceRtv[2].Get(), g.sourceRtv[3].Get()};
    auto bindHooked = [&](ID3D11DepthStencilView* dsv) {   // a hooked OMSetRenderTargets: the real call, then the shadow
        g.ctx->OMSetRenderTargets(4, r4, dsv);
        g.shadow(BindSlot::Rtv0, r4[0]);
        g.shadow(BindSlot::Dsv0, dsv);
    };
    auto depthNow = [&](const void* shadow, unsigned long long* reads) {
        ComPtr<ID3D11Resource> hold;
        RecOn on;
        g_rec.reset();
        const void* got = edvr::flatQueryDepth(cut, g.ctx, shadow, hold, log);
        if (reads) *reads = g_rec.calls[kOMGetRT];
        return got;
    };

    // The depth view: answered from the shadow, with no read of the context.
    bindHooked(g.sourceDsv.Get());
    unsigned long long reads = 9;
    cut.beginFrame(1);
    if (depthNow(depthIdentity, &reads) != depthIdentity || reads != 0) fail("coverage depth: an ordinary frame answered from the shadow with a read of the context");
    // On a checking frame the context is asked as well and agrees.
    cut.beginFrame(0);
    if (depthNow(depthIdentity, &reads) != depthIdentity || reads != 1) fail("coverage depth: a checking frame did not ask the context once");
    if (!said.empty() || cut.fellBack(edvr::FlatQuery::CoverageDepth)) fail("coverage depth: an agreeing check was reported wrong");
    // A hooked rebind: the shadow follows, and so the check still agrees.
    bindHooked(otherDsv.Get());
    cut.beginFrame(64);
    if (depthNow(otherRes.Get(), &reads) != otherRes.Get() || !said.empty()) fail("coverage depth: a hooked rebind was reported wrong");
    // ClearState, as the hook does it (the shadow forgets everything, then the real call): nothing is bound in either.
    for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
    g.ctx->ClearState();
    cut.beginFrame(128);
    if (depthNow(nullptr, &reads) != nullptr || !said.empty()) fail("coverage depth: after ClearState the shadow and the context disagreed");
    // A change nothing hooked: a real rebind with no shadow update. An ordinary frame cannot see it; a checking frame does.
    bindHooked(g.sourceDsv.Get());
    g.ctx->OMSetRenderTargets(4, r4, otherDsv.Get());
    cut.beginFrame(129);
    if (depthNow(depthIdentity, &reads) != depthIdentity) fail("coverage depth: an ordinary frame's shortcut was not the shadow's");
    cut.beginFrame(192);
    if (depthNow(depthIdentity, &reads) != otherRes.Get()) fail("coverage depth: a checking frame did not use the context's answer for a change nothing hooked");
    if (said.size() != 1 || said[0].find("coverage depth view") == std::string::npos || !cut.fellBack(edvr::FlatQuery::CoverageDepth))
        fail("coverage depth: the change nothing hooked was not reported once, by name, and fallen back on");
    // Fallen back: every frame asks, the answer is the context's, and nothing more is said.
    cut.beginFrame(193);
    if (depthNow(depthIdentity, &reads) != otherRes.Get() || reads != 1 || said.size() != 1) fail("coverage depth: a fallen-back state did not just ask the context");
    g.ctx->ClearState();
    cut.reset();
    said.clear();

    // The shaders.
    std::map<const void*, uint64_t> hashes = {{g.vs.Get(), lifecycle_tests::kVsHash}, {g.ps.Get(), lifecycle_tests::kPsHash},
                                              {g.ps2.Get(), lifecycle_tests::kPsHash2}};
    auto hashOf = [&](void* p) -> uint64_t { auto it = hashes.find(p); return it == hashes.end() ? 0 : it->second; };
    unsigned flushes = 0;
    auto flush = [&] { ++flushes; };
    auto shadersNow = [&](uint64_t vs, uint64_t ps, unsigned long long* readsOut) {
        RecOn on;
        g_rec.reset();
        const bool match = edvr::flatQueryShaders(cut, g.ctx, vs, ps, hashOf, flush, log);
        if (readsOut) *readsOut = g_rec.calls[kVSGetShader] + g_rec.calls[kPSGetShader];
        return match;
    };
    g.setVs();
    g.setPs(g.ps.Get(), lifecycle_tests::kPsHash);
    cut.beginFrame(1);
    if (!shadersNow(lifecycle_tests::kVsHash, lifecycle_tests::kPsHash, &reads) || reads != 0 || flushes != 0)
        fail("coverage shaders: an ordinary frame did not answer from the shadow with no read and no flush");
    cut.beginFrame(0);
    if (!shadersNow(lifecycle_tests::kVsHash, lifecycle_tests::kPsHash, &reads) || reads != 2 || flushes != 1)
        fail("coverage shaders: a checking frame did not flush, ask the context for both shaders and agree");
    if (!said.empty()) fail("coverage shaders: an agreeing check was reported wrong");
    g.setPs(g.ps2.Get(), lifecycle_tests::kPsHash2);   // hooked: the shadow follows
    cut.beginFrame(64);
    if (!shadersNow(lifecycle_tests::kVsHash, lifecycle_tests::kPsHash2, &reads) || !said.empty()) fail("coverage shaders: a hooked rebind was reported wrong");
    // ClearState: the shadow forgets the hashes and so does the context (no shader bound): nothing to match, on either side.
    for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
    g.ctx->ClearState();
    cut.beginFrame(128);
    if (!shadersNow(0, 0, &reads) || !said.empty()) fail("coverage shaders: after ClearState the shadow and the context disagreed");
    // A change nothing hooked: a real pixel-shader set with no shadow update.
    g.setVs();
    g.setPs(g.ps.Get(), lifecycle_tests::kPsHash);
    g.ctx->PSSetShader(g.ps2.Get(), nullptr, 0);
    cut.beginFrame(129);
    if (!shadersNow(lifecycle_tests::kVsHash, lifecycle_tests::kPsHash, &reads)) fail("coverage shaders: an ordinary frame's shortcut was not the shadow's");
    cut.beginFrame(192);
    if (shadersNow(lifecycle_tests::kVsHash, lifecycle_tests::kPsHash, &reads)) fail("coverage shaders: a checking frame did not use the context's answer for a change nothing hooked");
    if (said.size() != 1 || said[0].find("coverage shader identity") == std::string::npos || !cut.fellBack(edvr::FlatQuery::ShaderIdentity))
        fail("coverage shaders: the change nothing hooked was not reported once, by name, and fallen back on");
    // Fallen back: the shadow corrected by a hooked set, and every frame asks and matches.
    g.setPs(g.ps2.Get(), lifecycle_tests::kPsHash2);
    cut.beginFrame(193);
    if (!shadersNow(lifecycle_tests::kVsHash, lifecycle_tests::kPsHash2, &reads) || reads != 2 || said.size() != 1) fail("coverage shaders: a fallen-back state did not just ask the context");
    return out;
}

// --- The faults: the policy with one restore dropped ---------------------------------------------------------------
struct FaultKeepOnOtherDraw : edvr::FlatSubstNoFaults { static constexpr bool keepOnOtherDraw = true; };
struct FaultKeepOnDispatch : edvr::FlatSubstNoFaults { static constexpr bool keepOnDispatch = true; };
struct FaultKeepOnClear : edvr::FlatSubstNoFaults { static constexpr bool keepOnClear = true; };
struct FaultKeepOnCopy : edvr::FlatSubstNoFaults { static constexpr bool keepOnCopy = true; };
struct FaultKeepOnResolve : edvr::FlatSubstNoFaults { static constexpr bool keepOnResolve = true; };
struct FaultKeepOnKeepTargets : edvr::FlatSubstNoFaults { static constexpr bool keepOnKeepTargets = true; };
struct FaultKeepOnCommandList : edvr::FlatSubstNoFaults { static constexpr bool keepOnExecuteCommandList = true; };
struct FaultKeepOnPresent : edvr::FlatSubstNoFaults { static constexpr bool keepOnPresent = true; };
struct FaultFlushOnResize : edvr::FlatSubstNoFaults { static constexpr bool flushOnResize = true; };

struct ApiRunResult {
    Recorder recorder;
    ApiProbe probe;
};

enum class ApiScenario { Eager, Declined, Memo, Rejected };

inline ApiRunResult runApiTransaction(const Harness& h, bool sampling, bool matchingOwner,
    ApiScenario scenario = ApiScenario::Eager) {
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    edvr::flatQueryCut().reset();
    Game g(h);
    g.setup();
    SceneCleanup cleanup(g.ctx);
    for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
    edvr::engineVelocityConfigure(true);
    edvr::engineVelocityFlatLazy(false);
    g.makeSource(40, 24);
    Emu<> e(h, g);
    e.warmUp();
    e.startFrame();

    if (scenario == ApiScenario::Declined || scenario == ApiScenario::Memo) {
        edvr::engineVelocityFlatLazy(scenario == ApiScenario::Declined);
        h.check(e.producer("API fixture setup producer"), "engine velocity API sampling: prior producer establishes held state");
        edvr::flatQueryCut().beginFrame(1); // an ordinary frame: held answers, not the periodic checking frame
        if (scenario == ApiScenario::Declined)
            e.gameSetPs(g.unkeyed.Get(), lifecycle_tests::kUnkeyedPs);
    }

    g_apiProbe.reset(matchingOwner ? static_cast<const void*>(g.ctx) : static_cast<const void*>(h.device), sampling);
    edvr::plugin_cost::detail::g_apiSampleHint = sampling;
    g_rec.reset();
    g_rejectNextMrt6 = scenario == ApiScenario::Rejected;
    if (scenario == ApiScenario::Declined || scenario == ApiScenario::Rejected)
        e.declinedProducer("sampled declined target/blend API transaction");
    else
        e.producer("sampled target/blend API transaction");
    h.check(!g_rejectNextMrt6, "engine velocity API sampling: rejection fixture reaches the actual MRT6 set");
    g_rejectNextMrt6 = false;
    edvr::plugin_cost::detail::g_apiSampleHint = false;
    g_apiProbe.enabled = false;
    h.check(e.ok, "engine velocity API sampling: fixture preserves the actual producer decision and game state");

    ApiRunResult out;
    out.recorder = g_rec;
    out.probe = g_apiProbe;
    e.present();
    g.ctx->ClearState();
    return out;
}

template <class ApiPolicy>
inline ApiRunResult runVrApiTransaction(const Harness& h) {
    edvr::g_runtimeProfile = edvr::RuntimeProfile::LegacyVr;
    Game g(h);
    g.setup();
    SceneCleanup cleanup(g.ctx);
    for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
    edvr::engineVelocityConfigure(true);
    g.ordinaryFrame();
    g.beginFrame();
    g.writeScene(g.sceneA.Get(), g.rows[0]);
    g.pass(0, 0); // prepare the real eye pass without entering a draw policy
    g_apiProbe.reset(g.ctx, true);
    edvr::plugin_cost::detail::g_apiSampleHint = true;
    g_rec.reset();
    {
        RecOn on;
        edvr::engineVelocityBeforeDrawWithApi<ApiPolicy>(g.ctx, true);
    }
    h.check(edvr::engineVelocityDrawSubstituted(), "engine velocity API sampling: VR fixture reaches the substituted eye draw");
    g.ctx->DrawInstanced(4, 1, 0, 0);
    g.setPs(g.unkeyed.Get(), lifecycle_tests::kUnkeyedPs);
    {
        RecOn on;
        edvr::engineVelocityBeforeDrawWithApi<ApiPolicy>(g.ctx, true);
    }
    h.check(!edvr::engineVelocityDrawSubstituted(), "engine velocity API sampling: VR unkeyed draw takes the typed restore path");
    edvr::plugin_cost::detail::g_apiSampleHint = false;
    g_apiProbe.enabled = false;
    ApiRunResult out{g_rec, g_apiProbe};
    g.endFrame();
    g.ctx->ClearState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    return out;
}

inline void apiTransactionTests(const Harness& h) {
    const ApiRunResult sampled = runApiTransaction(h, true, true);
    const ApiRunResult disabled = runApiTransaction(h, false, true);
    const ApiRunResult foreign = runApiTransaction(h, true, false);

    h.check(sampled.probe.sites[132] == 1 && sampled.probe.sites[135] == 1,
            "engine velocity API sampling: eager draw restores its MRT6 and derived blend exactly once");

    h.check(sampled.probe.badNotes == 0 && sampled.probe.sites[126] > 0,
            "engine velocity API sampling: current owner context records the target snapshot site");
    h.check(sampled.probe.sites[128] == sampled.recorder.calls[kOMGetRTUav] &&
                sampled.probe.sites[126] + sampled.probe.sites[127] + sampled.probe.sites[130] ==
                    sampled.recorder.calls[kOMGetRT],
            "engine velocity API sampling: each target query note matches the context vtable recorder");
    h.check(sampled.probe.sites[129] + sampled.probe.sites[131] + sampled.probe.sites[132] ==
                sampled.recorder.calls[kOMSetRT] &&
                sampled.probe.sites[133] == sampled.recorder.calls[kOMGetBlend] &&
                sampled.probe.sites[134] + sampled.probe.sites[135] == sampled.recorder.calls[kOMSetBlend],
            "engine velocity API sampling: MRT6/blend apply and draw-scope restore notes match actual calls");
    h.check(sampled.probe.classes[static_cast<unsigned>(edvr::plugin_cost::ApiClass::ReadQuery)] ==
                sampled.probe.sites[126] + sampled.probe.sites[127] + sampled.probe.sites[128] +
                    sampled.probe.sites[130] + sampled.probe.sites[133] &&
                sampled.probe.classes[static_cast<unsigned>(edvr::plugin_cost::ApiClass::State)] ==
                    sampled.probe.sites[129] + sampled.probe.sites[131] + sampled.probe.sites[132] +
                    sampled.probe.sites[134] + sampled.probe.sites[135],
            "engine velocity API sampling: ReadQuery and State classes stay independent and exact");
    h.check(disabled.probe.verifierCalls == 0 && disabled.probe.badNotes == 0 &&
                std::all_of(disabled.probe.sites.begin(), disabled.probe.sites.end(), [](uint64_t n) { return n == 0; }),
            "engine velocity API sampling: hint-off path skips verifier and erases API notes");
    h.check(foreign.probe.verifierCalls > 0 && foreign.probe.badNotes == 0 &&
                std::all_of(foreign.probe.sites.begin(), foreign.probe.sites.end(), [](uint64_t n) { return n == 0; }),
            "engine velocity API sampling: mismatched owner context rejects notes while the transaction runs");
    bool sameCalls = true;
    for (unsigned i = 0; i < kRecCount; ++i)
        sameCalls = sameCalls && sampled.recorder.calls[i] == disabled.recorder.calls[i] &&
                    sampled.recorder.calls[i] == foreign.recorder.calls[i];
    h.check(sameCalls, "engine velocity API sampling: sampled, disabled, and foreign cases execute identical context calls");
    for (ApiScenario scenario : {ApiScenario::Declined, ApiScenario::Memo, ApiScenario::Rejected}) {
        const ApiRunResult actual = runApiTransaction(h, true, true, scenario);
        const ApiRunResult noApi = runApiTransaction(h, false, true, scenario);
        const bool exact = actual.probe.badNotes == 0 &&
            actual.probe.sites[126] + actual.probe.sites[127] + actual.probe.sites[130] == actual.recorder.calls[kOMGetRT] &&
            actual.probe.sites[128] == actual.recorder.calls[kOMGetRTUav] &&
            actual.probe.sites[129] + actual.probe.sites[131] + actual.probe.sites[132] == actual.recorder.calls[kOMSetRT] &&
            actual.probe.sites[133] == actual.recorder.calls[kOMGetBlend] &&
            actual.probe.sites[134] + actual.probe.sites[135] == actual.recorder.calls[kOMSetBlend];
        h.check(exact, "engine velocity API sampling: declined/memo/rejected notes match actual target/blend calls");
        bool unchanged = noApi.probe.verifierCalls == 0 && noApi.probe.badNotes == 0 &&
            std::all_of(noApi.probe.sites.begin(), noApi.probe.sites.end(), [](uint64_t n) { return n == 0; });
        for (unsigned i = 0; i < kRecCount; ++i)
            unchanged = unchanged && actual.recorder.calls[i] == noApi.recorder.calls[i];
        h.check(unchanged, "engine velocity API sampling: declined/memo/rejected NoApi work is unchanged and unannotated");
        if (scenario == ApiScenario::Declined)
            h.check(actual.probe.sites[132] == 1 && actual.probe.sites[135] == 1,
                    "engine velocity API sampling: declined draw restores held target and blend");
        if (scenario == ApiScenario::Memo)
            h.check(actual.probe.sites[126] == 0 && actual.probe.sites[130] == 0 && actual.probe.sites[133] == 0 &&
                        actual.probe.sites[129] == 1 && actual.probe.sites[132] == 1,
                    "engine velocity API sampling: memo shortcuts suppress query notes while apply/restore remain exact");
        if (scenario == ApiScenario::Rejected)
            h.check(actual.probe.sites[129] == 1 && actual.probe.sites[130] == 1 && actual.probe.sites[131] == 1,
                    "engine velocity API sampling: rejected readback records apply/query/rollback exactly once");
        std::printf("  engine velocity API scenario %u: sites126-135", static_cast<unsigned>(scenario));
        for (unsigned site = 126; site <= 135; ++site) std::printf(" %llu", static_cast<unsigned long long>(actual.probe.sites[site]));
        std::printf("; %s\n", actual.recorder.describe().c_str());
    }
    const ApiRunResult vr = runVrApiTransaction<edvr::plugin_cost::SampledApi<>>(h);
    const ApiRunResult vrNoApi = runVrApiTransaction<edvr::plugin_cost::NoApi>(h);
    h.check(vr.probe.badNotes == 0 && vr.probe.sites[127] > 0 && vr.probe.sites[135] > 0 &&
                vr.probe.sites[126] == 0 &&
                vr.probe.sites[127] + vr.probe.sites[130] == vr.recorder.calls[kOMGetRT] &&
                vr.probe.sites[128] == vr.recorder.calls[kOMGetRTUav] &&
                vr.probe.sites[129] + vr.probe.sites[131] + vr.probe.sites[132] == vr.recorder.calls[kOMSetRT] &&
                vr.probe.sites[133] == vr.recorder.calls[kOMGetBlend] &&
                vr.probe.sites[134] + vr.probe.sites[135] == vr.recorder.calls[kOMSetBlend],
            "engine velocity API sampling: VR target/current-read and blend restore notes match actual calls");
    bool vrUnchanged = vrNoApi.probe.verifierCalls == 0 && vrNoApi.probe.badNotes == 0 &&
        std::all_of(vrNoApi.probe.sites.begin(), vrNoApi.probe.sites.end(), [](uint64_t n) { return n == 0; });
    for (unsigned i = 0; i < kRecCount; ++i)
        vrUnchanged = vrUnchanged && vr.recorder.calls[i] == vrNoApi.recorder.calls[i];
    h.check(vrUnchanged, "engine velocity API sampling: typed VR NoApi ignores a positive hint and preserves context calls");
    apiProbeThreadRejection(h);
}

inline void run(const Harness& h) {
    edvr::g_clockForTest = &lifecycle_fake::fakeClock;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    ID3D11DeviceContext* ctx = h.context;

    // The recorder's vtable slots, each checked by calling the method it names: a miscounted slot would count another
    // method's calls, and every call count below would mean nothing.
    recInstall(ctx);
    {
        Game g(h);
        g.setup();
        g.makeSource(40, 24);
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11Buffer> cb;
        ComPtr<ID3D11DepthStencilState> ds;
        ID3D11RenderTargetView* rt[1] = {g.sourceRtv[0].Get()};
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11RenderTargetView* got[1] = {};
        ID3D11DepthStencilView* gotDsv = nullptr;
        ID3D11UnorderedAccessView* gotUav[1] = {};
        float f[4] = {};
        UINT m = 0, ref = 0;
        struct Probe { RecSlot slot; std::function<void()> call; };
        std::vector<Probe> probes = {
            {kOMSetRT, [&] { ctx->OMSetRenderTargets(1, rt, nullptr); }},
            {kOMSetRTUav, [&] { ctx->OMSetRenderTargetsAndUnorderedAccessViews(0xFFFFFFFFu, nullptr, nullptr, 0, 0xFFFFFFFFu, nullptr, nullptr); }},
            {kOMSetBlend, [&] { ctx->OMSetBlendState(nullptr, f, ~0u); }},
            {kPSSetShader, [&] { ctx->PSSetShader(nullptr, nullptr, 0); }},
            {kVSSetShader, [&] { ctx->VSSetShader(nullptr, nullptr, 0); }},
            {kPSSetSrv, [&] { ctx->PSSetShaderResources(3, 1, &nullSrv); }},
            {kOMGetRT, [&] { ctx->OMGetRenderTargets(1, got, &gotDsv); if (got[0]) got[0]->Release(); if (gotDsv) { gotDsv->Release(); gotDsv = nullptr; } }},
            {kOMGetRTUav, [&] { ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, gotUav); if (gotUav[0]) gotUav[0]->Release(); }},
            {kOMGetBlend, [&] { blend.Reset(); ctx->OMGetBlendState(&blend, f, &m); }},
            {kPSGetShader, [&] { ps.Reset(); ctx->PSGetShader(&ps, nullptr, nullptr); }},
            {kVSGetShader, [&] { vs.Reset(); ctx->VSGetShader(&vs, nullptr, nullptr); }},
            {kPSGetSrv, [&] { srv.Reset(); ctx->PSGetShaderResources(3, 1, &srv); }},
            {kVSGetCb, [&] { cb.Reset(); ctx->VSGetConstantBuffers(1, 1, &cb); }},
            {kOMGetDepth, [&] { ds.Reset(); ctx->OMGetDepthStencilState(&ds, &ref); }},
            {kCopyResource, [&] { ctx->CopyResource(g.sourceColour[1].Get(), g.sourceColour[0].Get()); }},
            
        };
        bool slotsOk = true;
        for (const Probe& p : probes) {
            g_rec.reset();
            g_rec.on = true;
            p.call();
            g_rec.on = false;
            bool only = g_rec.calls[p.slot] == 1;
            for (unsigned i = 0; i < kRecCount; ++i) if (i != p.slot && g_rec.calls[i] != 0) only = false;
            if (!only) {
                slotsOk = false;
                std::printf("  flat lazy: vtable slot %zu is not %s (recorded %llu calls of it)", kVtIndex[p.slot], kRecName[p.slot],
                            g_rec.calls[p.slot]);
                for (unsigned i = 0; i < kRecCount; ++i)
                    if (i != p.slot && g_rec.calls[i]) std::printf("; %llu of %s (slot %zu)", g_rec.calls[i], kRecName[i], kVtIndex[i]);
                std::printf("; vtable entry %p, original %p\n", g_rec.vtable[kVtIndex[p.slot]], g_rec.orig[p.slot]);
            }
        }
        h.check(slotsOk, "flat lazy: every recorded vtable slot is the method it is named for");
        ctx->ClearState();
    }

    // The mixed scene, with the policy as it ships, and each restore dropped in turn.
    std::string why;
    const bool shipped = mixedScene<edvr::FlatSubstNoFaults>(h, &why);
    if (!shipped) {
        std::printf("  flat lazy: the mixed scene failed at: %s\n", why.c_str());
        const size_t n = lifecycle_fake::g_log.size();
        for (size_t i = n > 8 ? n - 8 : 0; i < n; ++i) std::printf("    log| %.300s\n", lifecycle_fake::g_log[i].c_str());
    }
    h.check(shipped, "flat lazy: the mixed scene holds: every game call sees the game's state, every producer draw EDVR's");
    struct Fault { bool (*scene)(const Harness&, std::string*); const char* what; };
    const Fault faults[] = {
        {&mixedScene<FaultKeepOnOtherDraw>, "a draw that is not a producer keeps EDVR's state"},
        {&mixedScene<FaultKeepOnDispatch>, "a dispatch keeps it"},
        {&mixedScene<FaultKeepOnClear>, "a clear keeps it"},
        {&mixedScene<FaultKeepOnCopy>, "a copy keeps it"},
        {&mixedScene<FaultKeepOnResolve>, "a resolve keeps it"},
        {&mixedScene<FaultKeepOnKeepTargets>, "a set that keeps the render targets keeps it"},
        {&mixedScene<FaultKeepOnCommandList>, "a command list keeps it"},
        {&mixedScene<FaultKeepOnPresent>, "the Present keeps it"},
        {&mixedScene<FaultFlushOnResize>, "a resize puts state back through a context that may be gone"},
    };
    for (const Fault& f : faults) {
        std::string reason;
        const bool held = f.scene(h, &reason);
        h.check(!held, (std::string("flat lazy: the rig does not notice: ") + f.what).c_str());
    }

    // The saving, counted where D3D receives the calls.
    unsigned long long saved[2] = {};
    for (int variant = 0; variant < 2; ++variant) {
        const bool gameSetsPs = variant == 1;
        const unsigned long long oldCalls = countRun(h, false, 40, gameSetsPs);
        const std::string oldWas = g_lastRunDescription;
        const unsigned long long lazyCalls = countRun(h, true, 40, gameSetsPs);
        std::printf("  flat lazy: 40 consecutive producer draws%s: %llu context calls in the restore-after-every-draw form, %llu in the lazy form (%.0f%% fewer)\n"
                    "    restore-after-every-draw: %s\n    lazy: %s\n",
                    gameSetsPs ? ", the game setting its own pixel shader before each" : "", oldCalls, lazyCalls,
                    oldCalls ? 100.0 * (1.0 - double(lazyCalls) / double(oldCalls)) : 0.0, oldWas.c_str(), g_lastRunDescription.c_str());
        h.check(oldCalls > 0 && lazyCalls * 10 <= oldCalls * 3,
                gameSetsPs ? "flat lazy: a run of producer draws with a shader change between each costs 70% fewer context calls"
                           : "flat lazy: a run of consecutive producer draws costs 70% fewer context calls");
        saved[variant] = oldCalls - lazyCalls;
    }
    h.check(saved[0] > 0 && saved[1] > 0, "flat lazy: and both runs saved calls");
    h.check(countRun(h, false, 40, false) == 440,
            "flat lazy: the restore-after-every-draw form is eleven context calls a draw: the figure the census measured (13,549 over 1,130)");

    // The kept answers, on the case they are for: many short runs (a draw that is not a producer between two of them),
    // where every run pays for reading the game's state again. Three forms: the old per-draw bracket, the lazy bracket with
    // every state asking the context, and the lazy bracket answering from what it kept.
    {
        const unsigned long long perDraw = countRuns(h, Form::PerDraw, 40, 1, false);
        const std::string perDrawWas = g_lastRunDescription;
        const unsigned long long asking = countRuns(h, Form::LazyAsking, 40, 1, false);
        const std::string askingWas = g_lastRunDescription;
        const unsigned long long kept = countRuns(h, Form::Lazy, 40, 1, false);
        std::printf("  flat lazy: 40 runs of one producer draw each: %llu context calls restoring after every draw, %llu lazy with every state "
                    "asking the context, %llu lazy answering from what it kept (%.0f%% fewer than asking)\n"
                    "    per draw: %s\n    lazy, asking: %s\n    lazy, kept: %s\n",
                    perDraw, asking, kept, asking ? 100.0 * (1.0 - double(kept) / double(asking)) : 0.0, perDrawWas.c_str(),
                    askingWas.c_str(), g_lastRunDescription.c_str());
        h.check(kept > 0 && kept * 100 <= asking * 75,
                "flat lazy: a run answering from what it kept costs at least 25% fewer calls than one asking the context for the same state");
        h.check(kept < perDraw, "flat lazy: and short runs are still cheaper than restoring after every draw");
    }

    // A change nothing hooked, on a checking frame and off one, for the blend state and for the render targets.
    for (int what = 0; what < 2; ++what) {
        const Unhooked kind = what == 0 ? Unhooked::Blend : Unhooked::Targets;
        const edvr::FlatQuery query = what == 0 ? edvr::FlatQuery::GameBlend : edvr::FlatQuery::GameTargets;
        const char* name = what == 0 ? "blend state" : "render targets";
        const CutOutcome checked = unhookedScene(h, kind, true);
        if (!checked.held) std::printf("  flat lazy: the %s change on a checking frame failed at: %s\n", name, checked.why.c_str());
        h.check(checked.held, (std::string("flat lazy: the context is the game's again after a ") + name + " change nothing hooked, on a checking frame").c_str());
        h.check(checked.counts.mismatched[static_cast<unsigned>(query)] == 1 && (checked.counts.fellBack & (1u << static_cast<unsigned>(query))) != 0,
                (std::string("flat lazy: the check counts that ") + name + " change once, and the state asks the context from then on").c_str());
        h.check(checked.lines.find(edvr::flatQueryName(query)) != std::string::npos &&
                    checked.lines.find('\n') == checked.lines.rfind('\n'),
                (std::string("flat lazy: and says so once in the log, naming the ") + name).c_str());
        const CutOutcome off = unhookedScene(h, kind, false);
        h.check(!off.held && off.counts.mismatched[static_cast<unsigned>(query)] == 0,
                (std::string("flat lazy: off a checking frame the ") + name + " change nothing hooked is NOT seen: the stale answer is used (what one frame in 64 bounds)").c_str());
    }
    {
        std::string releaseWhy;
        const bool released = releaseScene(h, &releaseWhy);
        if (!released) std::printf("  flat lazy: the release scene failed: %s\n", releaseWhy.c_str());
        h.check(released, "flat lazy: the Present and ClearState let go of the game's views the bracket kept");
    }
    {
        const CoverageOutcome coverage = coverageScene(h);
        if (!coverage.ok) std::printf("  flat lazy: the coverage questions failed at: %s\n", coverage.why.c_str());
        h.check(coverage.ok, "flat lazy: the coverage classification's depth and shader questions answer from the shadow, follow hooked setters and "
                             "ClearState, and a change nothing hooked is found on a checking frame");
    }

    // The overlay guard: the counters agree with the eager form's.
    {
        const OverlayCounts eager = overlaySequence(h, false);
        const OverlayCounts lazy = overlaySequence(h, true);
        h.check(eager.guarded == 4 && eager.declined == 0, "flat lazy: the overlay sequence, every draw restoring after itself, guards four draws and declines none");
        h.check(lazy.copies == eager.copies && lazy.guarded == eager.guarded && lazy.declined == eager.declined,
                "flat lazy: and with the lazy form the overlay counters are the same: no group was declined for a state left bound");
    }

    // The lazy form off (a diagnostic capture armed): every producer draw restores after itself.
    {
        Game g(h);
        g.setup();
        for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
        edvr::engineVelocityConfigure(true);
        edvr::engineVelocityFlatLazy(false);
        g.makeSource(40, 24);
        Emu<> e(h, g);
        e.warmUp();
        e.startFrame();
        // A diagnostic capture arms in the middle of a run: the runtime turns the lazy form off and, in the draw scope's
        // constructor, puts the game's state back first; every producer draw after that restores after itself.
        edvr::engineVelocityFlatLazy(true);
        e.producer("a lazy producer draw, before the capture arms");
        e.producer("and another");
        edvr::engineVelocityFlatLazy(false);
        e.substitution(FlatSubstEvent::kOtherDraw);
        e.expectGame("a capture armed mid-run: the game's state is back before the next draw");
        for (int i = 0; i < 3; ++i) {
            e.producer("not lazy: a producer draw");
            e.expectGame("not lazy: after a producer draw the game's state is back");
        }
        h.check(!edvr::engineVelocityFlatPending(), "flat lazy: not lazy, nothing is left pending");
        if (!e.ok) std::printf("  flat lazy: not lazy, failed at: %s\n", e.why.c_str());
        h.check(e.ok, "flat lazy: with the lazy form off every producer draw restores after itself");
        e.present();
        edvr::engineVelocityShutdown();
        g.ctx->ClearState();
    }

    apiTransactionTests(h);

    // The census line.
    {
        Game g(h);
        g.setup();
        for (auto& s : lifecycle_fake::g_slots) { s.ptr = nullptr; s.hash = 0; ++s.gen; }
        const size_t mark = lifecycle_fake::g_log.size();
        edvr::engineVelocityConfigure(true);
        edvr::engineVelocityFlatLazy(true);
        g.makeSource(40, 24);
        Emu<> e(h, g);
        e.warmUp();
        e.startFrame();
        for (int i = 0; i < 5; ++i) e.producer("census: a producer draw");
        e.otherDraw("census: another draw");
        for (int i = 0; i < 3; ++i) e.producer("census: a producer draw");
        e.keepTargetsSet();
        for (int i = 0; i < 2; ++i) e.producer("census: a producer draw");
        e.present();
        g.endFrame(true);
        const std::string line = lifecycle_tests::lastLine("engine motion: flat draw bracket over", mark);
        h.check(!line.empty(), "flat lazy: the 30 s summary has a flat draw bracket line");
        if (line.empty() || lifecycle_tests::number(line, "put back ") != 3) std::printf("  flat lazy: the census line is: %s\n", line.c_str());
        h.check(lifecycle_tests::number(line, " s: ") == 10 && lifecycle_tests::number(line, "draws in ") == 3 &&
                    lifecycle_tests::number(line, "runs (") == 7,
                "flat lazy: it counts 10 substituted draws in 3 runs, 7 of them found EDVR's state still bound");
        h.check(lifecycle_tests::number(line, "put back ") == 3 && lifecycle_tests::number(line, "another draw ") == 1 &&
                    lifecycle_tests::number(line, "a set keeping the targets ") == 1 && lifecycle_tests::number(line, "the present ") == 1,
                "flat lazy: and that the game's state went back three times: another draw, a targets-keeping set, the present");
        edvr::engineVelocityShutdown();
        g.ctx->ClearState();
    }
    recRemove();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::LegacyVr;
    std::printf("  flat lazy: the bracket's lazy form on WARP -- mixed scene, every restore dropped in turn, the saving counted on the "
                "context's vtable, the overlay guard, the census line: every case as specified\n");
}

}  // namespace flat_lazy_tests
