// fix.ui_quality -- the UI layer. ui_layer.h says what it is and why;
// ui_layer_math.h holds its arithmetic and ui_layer_shaders.h its composite,
// both shared with tools/ui_layer_test; docs/ui-layer-2026-09-23.md is the
// design as built.
//
// THREADS. Everything here runs on the game's render thread: the draws, the
// door (native_temporal.cpp and native_sharpen.cpp run inside the game's
// Submit, which the runtime serves synchronously while the game waits), and
// the frame boundary (Present). The one other thread that matters -- the XR
// owner, which may open a frame -- is only read, under native_temporal's own
// lock, through nativeTemporalDrawJitter.
#include "ui_layer.h"

#include "ui_layer_math.h"
#include "ui_layer_shaders.h"

#include "binding_shadow.h"
#include "device_hook.h"   // deviceHookHmdQuality, for the configure line
#include "foveation.h"     // whether a shading-rate image is bound for the eye
#include "gpu_timing.h"
#include "graphics_runtime.h"
#include "shader_swap.h"
#include "ui_depth.h"      // uiDepthEyeOfTarget: the eye, by the pass's own table
#include "vscreen.h"       // the raw OM/RS entry points, vScreenIsEyeSized

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/temporal_mode.h"

#include <windows.h>

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {

namespace detail {
bool g_uiLayerLive = false;
bool g_uiLayerWatching = false;
}  // namespace detail

namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr uint64_t kTotalsMs = 30000;
constexpr uint32_t kWatchPerFrame = 64;
constexpr uint32_t kMaxFamilyLines = 48;
constexpr uint32_t kMaxAfterLines = 16;

// ------------------------------------------------------------ configuration

float g_target = 0.0f;     // 0 off, 1.0, 1.25
bool g_temporal = false;   // a temporal mode is on (the layer's door exists)
bool g_debugView = false;  // advanced.temporal_aa_debug = ui_layer
bool g_stoodDown = false;
std::string g_keyText = "?";
bool g_keyNoted = false;

void refreshLive() {
    detail::g_uiLayerLive = g_target > 0.0f && g_temporal && !g_stoodDown;
}

void standDown(const char* why) {
    if (g_stoodDown) return;
    g_stoodDown = true;
    refreshLive();
    Log::get().note(
        "ui layer: standing down for the rest of the session -- %s. The UI goes "
        "into the game's frame as before (and gets the UI depth and reactive "
        "mask again); turning fix.ui_quality off and on re-arms it.",
        why ? why : "a refusal");
}

// ------------------------------------------------------------------ per eye

struct Eye {
    // The layer: R8G8B8A8_UNORM, cleared to (0, 0, 0, 1) at the first draw
    // of each frame -- premultiplied colour and transmittance.
    Ptr<ID3D11Texture2D> tex;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
    uint32_t w = 0, h = 0;
    uint64_t seq = 0;            // the frame whose draws it holds
    uint32_t draws = 0;          // redirected into it for that frame
    uint64_t compositedSeq = 0;  // the last frame the door composited (or tried)
    const void* target = nullptr;  // the game's target those draws left (identity)

    // The door.
    UiLayerDoorState door;
    const void* temporalOut = nullptr;  // the pass's output (identity), this frame
    uint64_t temporalOutSeq = 0;
    bool doorFromPass = false;          // this frame's door input was the pass's

    // The eye check: where the target the UI left was copied to this frame,
    // and what the game submitted for this eye (identities).
    const void* copiedTo = nullptr;
    uint64_t copiedSeq = 0;
    const void* submitted = nullptr;
    uint64_t submittedSeq = 0;

    // The composite's output: the frame's region, the frame's format.
    Ptr<ID3D11Texture2D> out;
    Ptr<ID3D11UnorderedAccessView> outUav;
    uint32_t outW = 0, outH = 0;
    DXGI_FORMAT outFmt = DXGI_FORMAT_UNKNOWN;
    // The view over the frame, cached on exactly that resource.
    void* frameRes = nullptr;
    Ptr<ID3D11ShaderResourceView> frameSrv;
    DXGI_FORMAT frameView = DXGI_FORMAT_UNKNOWN;
    // The copy-through, for a frame that refuses a shader view.
    Ptr<ID3D11Texture2D> copy;
    Ptr<ID3D11ShaderResourceView> copySrv;
    uint32_t copyW = 0, copyH = 0;
    DXGI_FORMAT copyFmt = DXGI_FORMAT_UNKNOWN;
};
Eye g_eye[2];

void releaseEye(Eye& e) {
    const UiLayerDoorState door = e.door;
    e = Eye{};
    e.door = door;
}

// ------------------------------------------------------------ the draw path

// The target Rtv0 names, once per binding generation.
struct TargetCache {
    uint32_t gen = 0;
    int kind = 0;
    ResourceInfo info;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
};
TargetCache g_tc;

// One decided draw at a time: forwardWithVerdict decides, then brackets each
// issue with Begin/End, on the render thread, before the next draw arrives.
struct Draw {
    bool decided = false, active = false;
    bool saved = false;  // the game's state is held below: restore it on any exit
    int eye = -1;
    UiLayerFamily family = UiLayerFamily::kNone;
    uint64_t seq = 0;
    const void* targetRes = nullptr;
    uint32_t targetW = 0, targetH = 0;
    float jx = 0.0f, jy = 0.0f;  // this frame's jitter, in the target's pixels
    // Saved at Begin, put back at End.
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT vpCount = 0;
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT scCount = 0;
    bool scissorSet = false;
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
};
Draw g_draw;
thread_local bool t_redirecting = false;
uint64_t g_lastRedirectSeq = 0;
uint32_t g_watchBudget = kWatchPerFrame;

FaultBudget g_drawBudget("uiLayer.draw", 4);
FaultBudget g_compositeBudget("uiLayer.composite", 4);

// Converted blend states, keyed by the converted description (a handful).
struct BlendEntry {
    D3D11_BLEND_DESC desc{};
    Ptr<ID3D11BlendState> state;
};
BlendEntry g_blends[16];
uint32_t g_blendCount = 0;

// --------------------------------------------------------------- counters

struct Window {
    uint64_t frames = 0;
    uint64_t decided[static_cast<size_t>(UiLayerFamily::kCount)]
                    [static_cast<size_t>(UiLayerDecision::kCount)] = {};
    uint64_t redirected = 0, refusedAtIssue = 0;
    uint64_t viewportRemaps = 0, scissorRemaps = 0, jitterCancels = 0, clears = 0;
    uint64_t lostLayers = 0, doors = 0, treated = 0, composites = 0, overGameImage = 0;
    uint64_t compositeRefused = 0, afterWrites = 0, afterReads = 0, debugComposites = 0;
    uint64_t eyeMatched = 0, eyeSwapped = 0, eyeUntold = 0;
    uint32_t timed = 0;
    double timeSum = 0.0, timeMax = 0.0;
};
Window g_win;
uint64_t g_winStartMs = 0;
uint64_t g_sessionRedirected = 0;

// First-seen lines, deduplicated.
struct FamilySeen {
    uint8_t family, decision;
    uint64_t vs, ps;
};
FamilySeen g_familySeen[kMaxFamilyLines];
uint32_t g_familySeenCount = 0;
struct AfterSeen {
    char kind;
    uint64_t vs, ps;
};
AfterSeen g_afterSeen[kMaxAfterLines];
uint32_t g_afterSeenCount = 0;
bool g_engageNoted = false, g_compositeNoted = false, g_timingNoted = false;

const char* viewName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
        default: return "another format";
    }
}

bool structuralDecision(UiLayerDecision d) {
    // The transient ones -- not armed yet, late this frame -- are counted on
    // the totals line and never get a first-seen line of their own.
    return d != UiLayerDecision::kNotArmed && d != UiLayerDecision::kLate &&
           d != UiLayerDecision::kNotUi;
}

void noteFamily(UiLayerFamily f, UiLayerDecision d) {
    if (!structuralDecision(d)) return;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs), ps = bindingShaderHash(BindSlot::Ps);
    for (uint32_t i = 0; i < g_familySeenCount; ++i) {
        const FamilySeen& s = g_familySeen[i];
        if (s.family == uint8_t(f) && s.decision == uint8_t(d) && s.vs == vs && s.ps == ps) return;
    }
    if (g_familySeenCount >= kMaxFamilyLines) return;
    g_familySeen[g_familySeenCount++] = {uint8_t(f), uint8_t(d), vs, ps};
    Log::get().note("ui layer: %s (vs %016llX ps %016llX) into a %ux%u %s target: %s.",
                    uiLayerFamilyName(f), static_cast<unsigned long long>(vs),
                    static_cast<unsigned long long>(ps), g_tc.info.a, g_tc.info.b,
                    viewName(g_tc.view), uiLayerDecisionName(d));
}

// ------------------------------------------------------------ the layer

bool ensureLayer(ID3D11Device* dev, Eye& e, uint32_t w, uint32_t h, int eye) {
    if (e.tex && e.w == w && e.h == h) return true;
    const bool resized = e.tex != nullptr;
    e.tex.Reset();
    e.rtv.Reset();
    e.srv.Reset();
    e.w = e.h = 0;
    e.seq = 0;
    e.draws = 0;
    if (!dev || !w || !h) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &e.tex)) ||
        FAILED(dev->CreateRenderTargetView(e.tex.Get(), nullptr, &e.rtv)) ||
        FAILED(dev->CreateShaderResourceView(e.tex.Get(), nullptr, &e.srv))) {
        e.tex.Reset();
        e.rtv.Reset();
        e.srv.Reset();
        return false;
    }
    e.w = w;
    e.h = h;
    Log::get().note("ui layer: %s eye's layer %s at %ux%u (R8G8B8A8_UNORM, %.1f MB).",
                    eye == 0 ? "left" : "right", resized ? "re-created" : "created", w, h,
                    uiLayerMB(uiLayerBytes(w, h)));
    return true;
}

bool ensureLayerFor(ID3D11DeviceContext* ctx, int eye) {
    Eye& e = g_eye[eye];
    const UiLayerSize s = uiLayerSize(e.door.fullW, e.door.fullH, g_target);
    if (!s.w || !s.h) return false;
    if (e.tex && e.w == s.w && e.h == s.h) return true;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    return ensureLayer(dev.Get(), e, s.w, s.h, eye);
}

ID3D11BlendState* cachedBlend(ID3D11DeviceContext* ctx, const UiBlendRt& conv) {
    const D3D11_BLEND_DESC want = uiLayerBlendDesc(conv);
    for (uint32_t i = 0; i < g_blendCount; ++i) {
        if (std::memcmp(&g_blends[i].desc, &want, sizeof(want)) == 0) return g_blends[i].state.Get();
    }
    if (g_blendCount >= 16) return nullptr;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    Ptr<ID3D11BlendState> state;
    if (!dev || FAILED(dev->CreateBlendState(&want, &state)) || !state) return nullptr;
    g_blends[g_blendCount].desc = want;
    g_blends[g_blendCount].state = state;
    return g_blends[g_blendCount++].state.Get();
}

// The shape of a bound blend state, alpha-to-coverage and logic ops refused.
UiBlendShape shapeOf(ID3D11BlendState* bs, UiBlendRt* rtOut) {
    UiBlendRt rt;  // null state: D3D11's default -- blending off, all written
    if (bs) {
        D3D11_BLEND_DESC d{};
        bs->GetDesc(&d);
        if (d.AlphaToCoverageEnable) return UiBlendShape::kRefused;
        Ptr<ID3D11BlendState1> bs1;
        if (SUCCEEDED(bs->QueryInterface(__uuidof(ID3D11BlendState1),
                                         reinterpret_cast<void**>(bs1.GetAddressOf()))) &&
            bs1) {
            D3D11_BLEND_DESC1 d1{};
            bs1->GetDesc1(&d1);
            if (d1.RenderTarget[0].LogicOpEnable) return UiBlendShape::kRefused;
        }
        rt = uiLayerBlendRtFrom(d.RenderTarget[0]);
    }
    if (rtOut) *rtOut = rt;
    return uiLayerBlendShape(rt);
}

// Depth or stencil the layer cannot honour: a bound depth target that the
// draw tests or writes (the layer has none), or stencil tested or written.
bool depthStencilEffect(ID3D11DeviceContext* ctx, bool dsvBound) {
    if (!dsvBound) return false;  // nothing bound: both tests pass already
    Ptr<ID3D11DepthStencilState> dss;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&dss, &ref);
    D3D11_DEPTH_STENCIL_DESC d{};
    if (dss) {
        dss->GetDesc(&d);
    } else {  // D3D11's default state: depth on, LESS, writes
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_LESS;
        d.StencilEnable = FALSE;
    }
    const bool depth = d.DepthEnable && (d.DepthFunc != D3D11_COMPARISON_ALWAYS ||
                                         d.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO);
    auto faceActs = [&](const D3D11_DEPTH_STENCILOP_DESC& f) {
        const bool tests = f.StencilFunc != D3D11_COMPARISON_ALWAYS && d.StencilReadMask != 0;
        const bool writes = d.StencilWriteMask != 0 &&
                            (f.StencilPassOp != D3D11_STENCIL_OP_KEEP ||
                             f.StencilFailOp != D3D11_STENCIL_OP_KEEP ||
                             f.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP);
        return tests || writes;
    };
    const bool stencil = d.StencilEnable && (faceActs(d.FrontFace) || faceActs(d.BackFace));
    return depth || stencil;
}

void releaseSaved() {
    for (auto*& r : g_draw.rtv) {
        if (r) r->Release();
        r = nullptr;
    }
    if (g_draw.dsv) g_draw.dsv->Release();
    g_draw.dsv = nullptr;
    if (g_draw.blend) g_draw.blend->Release();
    g_draw.blend = nullptr;
    g_draw.saved = false;
}

UINT boundCount(ID3D11RenderTargetView* const* rtvs) {
    UINT n = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (rtvs[i]) n = i + 1;
    }
    return n;
}

void restore(ID3D11DeviceContext* ctx) {
    vScreenSetRenderTargetsRaw(ctx, boundCount(g_draw.rtv), g_draw.rtv, g_draw.dsv);
    vScreenRSSetViewportsRaw(ctx, g_draw.vpCount, g_draw.vp);
    if (g_draw.scissorSet) ctx->RSSetScissorRects(g_draw.scCount, g_draw.sc);
    ctx->OMSetBlendState(g_draw.blend, g_draw.factor, g_draw.sampleMask);
}

bool beginInner(ID3D11DeviceContext* ctx) {
    Eye& e = g_eye[g_draw.eye];
    if (!e.rtv) return false;
    // The blend at the moment of issue: a verdict's own Begin runs between
    // the decision and here.
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    ctx->OMGetBlendState(&bs, factor, &sampleMask);
    UiBlendRt game, conv;
    ID3D11BlendState* layerBlend = nullptr;
    if (shapeOf(bs, &game) == UiBlendShape::kRefused || !uiLayerConvertBlend(game, &conv) ||
        !(layerBlend = cachedBlend(ctx, conv))) {
        if (bs) bs->Release();
        ++g_win.refusedAtIssue;
        return false;
    }
    // A new frame for this eye's layer: clear it, and count a layer the door
    // never composited (the frame took a path without a door, or a withhold).
    if (e.seq != g_draw.seq) {
        if (e.seq && e.draws && e.compositedSeq != e.seq) ++g_win.lostLayers;
        vScreenClearRenderTargetViewRaw(ctx, e.rtv.Get(), kUiLayerClear);
        e.seq = g_draw.seq;
        e.draws = 0;
        e.target = g_draw.targetRes;
        ++g_win.clears;
    }
    // Save.
    g_draw.blend = bs;
    std::memcpy(g_draw.factor, factor, sizeof(factor));
    g_draw.sampleMask = sampleMask;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_draw.rtv, &g_draw.dsv);
    g_draw.vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&g_draw.vpCount, g_draw.vp);
    g_draw.scissorSet = false;
    g_draw.scCount = 0;
    {
        Ptr<ID3D11RasterizerState> rs;
        ctx->RSGetState(&rs);
        D3D11_RASTERIZER_DESC rd{};
        if (rs) rs->GetDesc(&rd);
        if (rs && rd.ScissorEnable) {
            g_draw.scCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            ctx->RSGetScissorRects(&g_draw.scCount, g_draw.sc);
            g_draw.scissorSet = g_draw.scCount > 0;
        }
    }
    // Everything the game had is held: from here on any exit, a fault's
    // included, puts it back (uiLayerBegin).
    g_draw.saved = true;
    // The map (the game's eye target onto the layer) and the jitter cancel.
    const UiLayerMap m = uiLayerMapFromRegion(0.0f, 0.0f, static_cast<float>(g_draw.targetW),
                                              static_cast<float>(g_draw.targetH), e.w, e.h);
    float cx = 0.0f, cy = 0.0f;
    uiLayerJitterCancel(g_draw.jx, g_draw.jy, m, &cx, &cy);
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    for (UINT i = 0; i < g_draw.vpCount; ++i) {
        const D3D11_VIEWPORT& g = g_draw.vp[i];
        UiViewport v;
        v.x = g.TopLeftX;
        v.y = g.TopLeftY;
        v.w = g.Width;
        v.h = g.Height;
        v.minZ = g.MinDepth;
        v.maxZ = g.MaxDepth;
        const UiViewport o = uiLayerMapViewport(m, v, cx, cy);
        vp[i] = {o.x, o.y, o.w, o.h, o.minZ, o.maxZ};
    }
    vScreenRSSetViewportsRaw(ctx, g_draw.vpCount, vp);
    if (g_draw.scissorSet) {
        D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        for (UINT i = 0; i < g_draw.scCount; ++i) {
            UiRect r;
            r.l = g_draw.sc[i].left;
            r.t = g_draw.sc[i].top;
            r.r = g_draw.sc[i].right;
            r.b = g_draw.sc[i].bottom;
            const UiRect o = uiLayerMapScissor(m, r, cx, cy, e.w, e.h);
            sc[i] = {o.l, o.t, o.r, o.b};
        }
        ctx->RSSetScissorRects(g_draw.scCount, sc);
    }
    ctx->OMSetBlendState(layerBlend, factor, sampleMask);
    ID3D11RenderTargetView* layer = e.rtv.Get();
    vScreenSetRenderTargetsRaw(ctx, 1, &layer, nullptr);
    g_draw.active = true;

    ++e.draws;
    ++g_win.redirected;
    ++g_sessionRedirected;
    g_win.viewportRemaps += g_draw.vpCount;
    if (g_draw.scissorSet) g_win.scissorRemaps += g_draw.scCount;
    if (g_draw.jx != 0.0f || g_draw.jy != 0.0f) ++g_win.jitterCancels;
    g_lastRedirectSeq = g_draw.seq;
    detail::g_uiLayerWatching = true;
    if (!g_engageNoted) {
        g_engageNoted = true;
        Log::get().note(
            "ui layer: engaged -- the %s went into the %s eye's layer (%ux%u) from a %ux%u "
            "%s target: viewport scale %.4f x %.4f, jitter cancel (%.3f, %.3f) layer "
            "pixels, blend %s with transmittance in alpha.",
            uiLayerFamilyName(g_draw.family), g_draw.eye == 0 ? "left" : "right", e.w, e.h,
            g_draw.targetW, g_draw.targetH, viewName(g_tc.view), static_cast<double>(m.ax),
            static_cast<double>(m.ay), static_cast<double>(cx), static_cast<double>(cy),
            uiBlendShapeName(uiLayerBlendShape(game)));
    }
    return true;
}

// ------------------------------------------------------------ the composite

ID3D11ComputeShader* g_cs = nullptr;
bool g_csTried = false;
ID3D11Buffer* g_cb = nullptr;
bool g_fmtChecked[2] = {}, g_fmtOk[2] = {};

struct QuerySlot {
    GpuTimer timer;
    bool inUse = false;
};
constexpr int kQueryRing = 8;
QuerySlot g_qring[kQueryRing];

void pollTiming(ID3D11DeviceContext* ctx) {
    if (!gpuTimingOwns(ctx)) return;
    for (QuerySlot& q : g_qring) {
        if (!q.inUse) continue;
        double ms = 0.0;
        const GpuTimerPoll r = q.timer.poll(ctx, ms);
        if (r == GpuTimerPoll::Pending) continue;
        q.inUse = false;
        if (r != GpuTimerPoll::Ready) continue;
        ++g_win.timed;
        g_win.timeSum += ms;
        if (ms > g_win.timeMax) g_win.timeMax = ms;
    }
}

int acquireSlot(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!dev || !ctx) return -1;
    if (!gpuTimingAccepts(ctx) && !gpuTimingBind(dev, ctx)) return -1;
    for (int i = 0; i < kQueryRing; ++i) {
        QuerySlot& q = g_qring[i];
        if (q.inUse) continue;
        if (q.timer.begin(dev, ctx)) {
            q.inUse = true;
            return i;
        }
        return -1;
    }
    return -1;
}

void compileOnce(ID3D11DeviceContext* ctx) {
    if (g_cs || g_csTried || !ctx) return;
    g_csTried = true;
    g_cs = shaderSwapCompileCs(ctx, kUiLayerCompositeHlsl, sizeof(kUiLayerCompositeHlsl) - 1,
                               "main", "ui_layer_composite_cs", nullptr, "ui layer");
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT texFmt, DXGI_FORMAT viewFmt,
             UINT bind, Ptr<ID3D11Texture2D>* tex, Ptr<ID3D11ShaderResourceView>* srv,
             Ptr<ID3D11UnorderedAccessView>* uav) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = texFmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex->ReleaseAndGetAddressOf()))) return false;
    if (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = viewFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(tex->Get(), &sd, srv->ReleaseAndGetAddressOf())))
            return false;
    }
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = viewFmt;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateUnorderedAccessView(tex->Get(), &ud, uav->ReleaseAndGetAddressOf())))
            return false;
    }
    return true;
}

ID3D11Texture2D* compositeInner(Eye& e, uint32_t eye, ID3D11Texture2D* frame,
                                const uint32_t region[4], const float* bounds, const char** why) {
    D3D11_TEXTURE2D_DESC fd{};
    frame->GetDesc(&fd);
    const DXGI_FORMAT view = uiLayerFrameView(fd.Format);
    if (view == DXGI_FORMAT_UNKNOWN) {
        *why = "the frame at the door is not an 8-bit UNORM family (the composite blends "
               "in the space the game's UI composites did, and refuses to guess another)";
        return nullptr;
    }
    if (fd.SampleDesc.Count != 1 || fd.ArraySize != 1 || fd.MipLevels != 1) {
        *why = "the frame at the door is multisampled, an array or mipped";
        return nullptr;
    }
    if (region[2] <= region[0] || region[3] <= region[1] || region[2] > fd.Width ||
        region[3] > fd.Height) {
        *why = "the frame's region is empty or off the texture";
        return nullptr;
    }
    const uint32_t rw = region[2] - region[0], rh = region[3] - region[1];
    float uv[4];
    uiLayerUvFromBounds(bounds, uv);
    if (!uiLayerRegionMatches(rw, rh, uv, e.w, e.h)) {
        *why = "the frame's region does not describe the layer's eye (a half of a "
               "double-wide texture, or a size the layer was not made for)";
        return nullptr;
    }
    Ptr<ID3D11Device> dev;
    frame->GetDevice(&dev);
    Ptr<ID3D11DeviceContext> ctx;
    if (dev) dev->GetImmediateContext(&ctx);
    if (!dev || !ctx) {
        *why = "no device";
        return nullptr;
    }
    pollTiming(ctx.Get());
    compileOnce(ctx.Get());
    if (!g_cs) {
        *why = "the composite shader did not compile";
        return nullptr;
    }
    const int fi = view == DXGI_FORMAT_R8G8B8A8_UNORM ? 0 : 1;
    if (!g_fmtChecked[fi]) {
        g_fmtChecked[fi] = true;
        UINT support = 0;
        g_fmtOk[fi] = SUCCEEDED(dev->CheckFormatSupport(view, &support)) &&
                      (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    }
    if (!g_fmtOk[fi]) {
        *why = "no typed unordered-access store for the frame's format on this GPU";
        return nullptr;
    }
    if (!g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(UiLayerCompositeParams);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb))) {
            g_cb = nullptr;
            *why = "the parameter buffer could not be created";
            return nullptr;
        }
    }
    // The frame's view: over it directly when it allows one, else its region
    // copied out first (the sharpen's rule, for the same reason).
    ID3D11ShaderResourceView* frameSrv = nullptr;
    bool viaCopy = false;
    if (fd.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        if (e.frameRes != static_cast<void*>(frame) || !e.frameSrv || e.frameView != view) {
            e.frameSrv.Reset();
            e.frameRes = nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = view;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            if (SUCCEEDED(dev->CreateShaderResourceView(frame, &sd, &e.frameSrv))) {
                e.frameRes = frame;
                e.frameView = view;
            }
        }
        frameSrv = e.frameSrv.Get();
    }
    if (!frameSrv) {
        viaCopy = true;
        if (!e.copy || e.copyW != rw || e.copyH != rh || e.copyFmt != fd.Format) {
            e.copyW = e.copyH = 0;
            if (!makeTex(dev.Get(), rw, rh, fd.Format, view, D3D11_BIND_SHADER_RESOURCE, &e.copy,
                         &e.copySrv, nullptr)) {
                e.copy.Reset();
                e.copySrv.Reset();
                *why = "the frame refuses a shader view and could not be copied";
                return nullptr;
            }
            e.copyW = rw;
            e.copyH = rh;
            e.copyFmt = fd.Format;
        }
        frameSrv = e.copySrv.Get();
    }
    if (!e.out || e.outW != rw || e.outH != rh || e.outFmt != fd.Format) {
        e.outW = e.outH = 0;
        e.out.Reset();
        e.outUav.Reset();
        if (!makeTex(dev.Get(), rw, rh, fd.Format, view,
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &e.out, nullptr,
                     &e.outUav)) {
            e.out.Reset();
            e.outUav.Reset();
            *why = "the composite's output texture could not be created";
            return nullptr;
        }
        e.outW = rw;
        e.outH = rh;
        e.outFmt = fd.Format;
        Log::get().note("ui layer: %s eye's composite output %ux%u (%s, %.1f MB).",
                        eye == 0 ? "left" : "right", rw, rh, viewName(view),
                        uiLayerMB(uiLayerBytes(rw, rh)));
    }

    UiLayerCompositeParams p{};
    if (viaCopy) {
        p.region[0] = p.region[1] = 0;
        p.region[2] = static_cast<int32_t>(rw);
        p.region[3] = static_cast<int32_t>(rh);
    } else {
        for (int i = 0; i < 4; ++i) p.region[i] = static_cast<int32_t>(region[i]);
    }
    std::memcpy(p.uv, uv, sizeof(uv));
    p.layerSize[0] = static_cast<float>(e.w);
    p.layerSize[1] = static_cast<float>(e.h);
    p.outSize[0] = rw;
    p.outSize[1] = rh;
    p.mode = g_debugView ? 1u : 0u;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) {
        *why = "the parameter buffer could not be written";
        return nullptr;
    }
    std::memcpy(m.pData, &p, sizeof(p));
    ctx->Unmap(g_cb, 0);

    // The compute stage is saved and put back around the dispatch (the
    // sharpen's discipline: other passes own slots here).
    ID3D11ComputeShader* savedCs = nullptr;
    ID3D11ShaderResourceView* savedSrv[2] = {};
    ID3D11UnorderedAccessView* savedUav = nullptr;
    ID3D11Buffer* savedCb = nullptr;
    ctx->CSGetShader(&savedCs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 2, savedSrv);
    ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
    ctx->CSGetConstantBuffers(0, 1, &savedCb);

    const int qs = acquireSlot(dev.Get(), ctx.Get());
    if (viaCopy) {
        D3D11_BOX box{region[0], region[1], 0, region[2], region[3], 1};
        ctx->CopySubresourceRegion(e.copy.Get(), 0, 0, 0, 0, frame, 0, &box);
    }
    ID3D11ShaderResourceView* nullSrv[2] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 2, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ID3D11ShaderResourceView* srvs[2] = {frameSrv, e.srv.Get()};
    ctx->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uav = e.outUav.Get();
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
    if (qs >= 0) g_qring[qs].timer.end(ctx.Get());

    ctx->CSSetShaderResources(0, 2, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(savedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, savedSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &savedCb);
    if (savedCs) savedCs->Release();
    for (auto* s : savedSrv)
        if (s) s->Release();
    if (savedUav) savedUav->Release();
    if (savedCb) savedCb->Release();

    ++g_win.composites;
    if (!e.doorFromPass) ++g_win.overGameImage;
    if (g_debugView) ++g_win.debugComposites;
    if (!g_compositeNoted) {
        g_compositeNoted = true;
        Log::get().note(
            "ui layer: first composite -- the %s eye's %ux%u layer over a %ux%u %s frame "
            "(layer rectangle u %.3f..%.3f, v %.3f..%.3f), after the upscale and RCAS, before "
            "EDVR's menu%s. The one order that differs from the game's own: whatever the game "
            "drew into an eye AFTER a redirected draw is under the UI now (counted on the "
            "totals line as 'after the UI').",
            eye == 0 ? "left" : "right", e.w, e.h, rw, rh, viewName(view),
            static_cast<double>(uv[0]), static_cast<double>(uv[2]), static_cast<double>(uv[1]),
            static_cast<double>(uv[3]),
            g_debugView ? " -- the ui_layer debug view is on: the layer over black, a blue "
                          "wash where it covers"
                        : "");
    }
    ID3D11Texture2D* out = e.out.Get();
    out->AddRef();
    return out;
}

// ------------------------------------------------------------ the totals

void appendf(std::string& s, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) s.append(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n)
                                                                  : sizeof(buf) - 1);
}

void logTotals(double seconds) {
    const double frames = g_win.frames ? static_cast<double>(g_win.frames) : 1.0;
    std::string taken, left;
    for (size_t f = 1; f < static_cast<size_t>(UiLayerFamily::kCount); ++f) {
        const uint64_t n = g_win.decided[f][static_cast<size_t>(UiLayerDecision::kRedirect)];
        if (n) {
            appendf(taken, "%s%s %.2f", taken.empty() ? "" : ", ",
                    uiLayerFamilyName(static_cast<UiLayerFamily>(f)),
                    static_cast<double>(n) / frames);
        }
        for (size_t d = 2; d < static_cast<size_t>(UiLayerDecision::kCount); ++d) {
            const uint64_t k = g_win.decided[f][d];
            if (!k) continue;
            appendf(left, "%s%s %.2f a frame (%s)", left.empty() ? "" : "; ",
                    uiLayerFamilyName(static_cast<UiLayerFamily>(f)),
                    static_cast<double>(k) / frames,
                    uiLayerDecisionName(static_cast<UiLayerDecision>(d)));
        }
    }
    const Eye& l = g_eye[0];
    Log::get().note(
        "ui layer totals (%.0f s, %llu frames): fix.ui_quality = %s; layer %ux%u per eye "
        "(%.1f MB each) + composite output %ux%u (%.1f MB each); %.2f draws a frame "
        "redirected (%s); per frame %.2f viewport remaps, %.2f scissor remaps, %.2f jitter "
        "cancels; composite %.3f ms per eye average, %.3f max, %u timed of %llu composites "
        "(%llu over the game's own image, %llu in the ui_layer debug view).",
        seconds, static_cast<unsigned long long>(g_win.frames), g_keyText.c_str(), l.w, l.h,
        uiLayerMB(uiLayerBytes(l.w, l.h)), l.outW, l.outH,
        uiLayerMB(uiLayerBytes(l.outW, l.outH)), static_cast<double>(g_win.redirected) / frames,
        taken.empty() ? "none" : taken.c_str(), static_cast<double>(g_win.viewportRemaps) / frames,
        static_cast<double>(g_win.scissorRemaps) / frames,
        static_cast<double>(g_win.jitterCancels) / frames,
        g_win.timed ? g_win.timeSum / g_win.timed : 0.0, g_win.timeMax, g_win.timed,
        static_cast<unsigned long long>(g_win.composites),
        static_cast<unsigned long long>(g_win.overGameImage),
        static_cast<unsigned long long>(g_win.debugComposites));
    Log::get().note("ui layer: left in the game's frame: %s.",
                    left.empty() ? "nothing classified" : left.c_str());
    Log::get().note(
        "ui layer gates: G1 -- %llu UI draws arrived after their eye's composite had run (left "
        "in the game's frame, counted above as 'after its eye's composite'); %llu layers never "
        "reached the door; the door ran %llu times, the pass treated %llu eyes; %llu composites "
        "refused; %llu redirected draws refused at issue by a changed blend; after the UI the "
        "game drew %llu times into, and %llu times read, an eye target the UI was taken from "
        "(those now land under it); eye check against the game's Submit: %llu matched, %llu "
        "SWAPPED, %llu could not be told; %llu redirected this session%s.",
        static_cast<unsigned long long>(
            [] {
                uint64_t n = 0;
                for (size_t f = 1; f < static_cast<size_t>(UiLayerFamily::kCount); ++f)
                    n += g_win.decided[f][static_cast<size_t>(UiLayerDecision::kLate)];
                return n;
            }()),
        static_cast<unsigned long long>(g_win.lostLayers),
        static_cast<unsigned long long>(g_win.doors), static_cast<unsigned long long>(g_win.treated),
        static_cast<unsigned long long>(g_win.compositeRefused),
        static_cast<unsigned long long>(g_win.refusedAtIssue),
        static_cast<unsigned long long>(g_win.afterWrites),
        static_cast<unsigned long long>(g_win.afterReads),
        static_cast<unsigned long long>(g_win.eyeMatched),
        static_cast<unsigned long long>(g_win.eyeSwapped),
        static_cast<unsigned long long>(g_win.eyeUntold),
        static_cast<unsigned long long>(g_sessionRedirected),
        g_stoodDown ? " -- STOOD DOWN (the line above says why)" : "");
}

}  // namespace

// --------------------------------------------------------------- the API

void uiLayerConfigure(Config& cfg) {
    const std::string text = cfg.getString("fix.ui_quality", "off");
    bool recognized = true;
    const float target = uiQualityParse(text.c_str(), &recognized);
    const bool temporal = temporalModeEnabled(cfg.getString("fix.temporal_aa", "off"));
    const bool debugView = _stricmp(cfg.getString("advanced.temporal_aa_debug", "off").c_str(),
                                    "ui_layer") == 0;
    const bool changed = !g_keyNoted || text != g_keyText || target != g_target ||
                         temporal != g_temporal || debugView != g_debugView;
    // A live change of the key re-arms a stood-down layer: the player asked.
    if (g_keyNoted && (text != g_keyText || target != g_target)) g_stoodDown = false;
    g_keyText = text;
    g_target = target;
    g_temporal = temporal;
    g_debugView = debugView;
    refreshLive();
    if (!changed) return;
    g_keyNoted = true;
    if (!recognized) {
        Log::get().note("ui layer: fix.ui_quality = '%s' is not off, 1.0 or 1.25 -- off.",
                        text.c_str());
        return;
    }
    if (target <= 0.0f) {
        Log::get().note("ui layer: off -- the game's UI is drawn into its frame, as it always "
                        "was.");
        return;
    }
    float hmd = 0.0f;
    const bool hmdKnown = deviceHookHmdQuality(&hmd) && hmd > 0.0f;
    char hmdText[64] = "unknown";
    if (hmdKnown) std::snprintf(hmdText, sizeof(hmdText), "%.2f", static_cast<double>(hmd));
    if (!temporal) {
        Log::get().note(
            "ui layer: fix.ui_quality = %s, but fix.temporal_aa is off, so it waits: the layer "
            "is composited at the temporal pass's door, and without the pass there is no "
            "upscale for the UI to escape.",
            text.c_str());
        return;
    }
    Log::get().note(
        "ui layer: fix.ui_quality = %s (HMD Quality %s) -- the game's post-tonemap UI (the 2D "
        "screen's composite, and every eye draw of a learned interface surface: the menus, the "
        "loading screen) is drawn by its own shaders into a per-eye layer at %s times the size "
        "the door hands on, unjittered, and composited after the upscale and RCAS, before "
        "EDVR's menu. The cockpit's holo panels, flight HUD and target sprite are drawn into the "
        "HDR target before the tonemap and stay with the deferred UI replay. Draws the layer "
        "takes get no UI depth and no reactive mask.%s",
        text.c_str(), hmdText, text.c_str(),
        debugView ? " advanced.temporal_aa_debug = ui_layer: the layer is shown over black." : "");
}

int uiLayerTargetKind() {
    const uint32_t gen = bindingGeneration(BindSlot::Rtv0);
    if (gen == g_tc.gen) return g_tc.kind;
    g_tc = TargetCache{};
    g_tc.gen = gen;
    void* rtv = bindingGet(BindSlot::Rtv0);
    ResourceInfo info;
    if (!rtv || !bindingResolve(rtv, &info) || !info.isTexture2D ||
        !vScreenIsEyeSized(info.a, info.b)) {
        return 0;
    }
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    if (!guarded("uiLayer.rtvDesc",
                 [&] { static_cast<ID3D11RenderTargetView*>(rtv)->GetDesc(&d); })) {
        return 0;
    }
    g_tc.info = info;
    g_tc.view = d.Format;
    g_tc.kind = (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && d.Texture2D.MipSlice == 0 &&
                 uiLayerLdrView(d.Format))
                    ? 2
                    : 1;
    return g_tc.kind;
}

bool uiLayerDecide(ID3D11DeviceContext* ctx, int familyInt, bool verdictForwards) {
    g_draw.decided = false;
    if (!ctx || familyInt <= 0 || familyInt >= static_cast<int>(UiLayerFamily::kCount)) return false;
    const UiLayerFamily family = static_cast<UiLayerFamily>(familyInt);
    UiLayerDrawFacts f;
    f.family = family;
    f.verdictForwards = verdictForwards;
    const int kind = uiLayerTargetKind();
    f.eyeTarget = kind != 0;
    f.ldrView = kind == 2;
    // A shading-rate image bound for the eye (or possibly bound) would shade
    // the layer -- a different size -- through the eye's tiles.
    f.vrs = detail::g_foveationBound != nullptr || detail::g_foveationBoundUnknown;
    uint64_t seq = 0;
    float jx = 0.0f, jy = 0.0f;
    uint32_t sw = 0, sh = 0;
    if (f.eyeTarget && f.ldrView) {
        f.eye = uiDepthEyeOfTarget(g_tc.info.resource, g_tc.info.a, g_tc.info.b, g_tc.info.fmt);
        if (f.eye >= 0 &&
            nativeTemporalDrawJitter(static_cast<uint32_t>(f.eye), &seq, &jx, &jy, &sw, &sh)) {
            f.late = uiLayerLateFor(g_eye[f.eye].door, seq);
            f.armed = uiLayerArmed(g_eye[f.eye].door, seq);
        }
    }
    // The cheap facts first; the state reads only when none of them refused.
    f.blend = UiBlendShape::kOpaque;
    UiLayerDecision d = uiLayerDecide(f);
    if (d == UiLayerDecision::kRedirect) {
        ID3D11RenderTargetView* rtvs[2] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(2, rtvs, &dsv);
        f.mrt = rtvs[1] != nullptr;
        f.depthStencil = depthStencilEffect(ctx, dsv != nullptr);
        for (auto* r : rtvs)
            if (r) r->Release();
        if (dsv) dsv->Release();
        ID3D11BlendState* bs = nullptr;
        FLOAT factor[4];
        UINT mask = 0;
        ctx->OMGetBlendState(&bs, factor, &mask);
        f.blend = shapeOf(bs, nullptr);
        if (bs) bs->Release();
        if (uiLayerDecide(f) == UiLayerDecision::kRedirect) f.layerReady = ensureLayerFor(ctx, f.eye);
        d = uiLayerDecide(f);
    }
    ++g_win.decided[static_cast<size_t>(family)][static_cast<size_t>(d)];
    noteFamily(family, d);
    if (d != UiLayerDecision::kRedirect) return false;
    g_draw.decided = true;
    g_draw.eye = f.eye;
    g_draw.family = family;
    g_draw.seq = seq;
    g_draw.targetRes = g_tc.info.resource;
    g_draw.targetW = g_tc.info.a;
    g_draw.targetH = g_tc.info.b;
    // The pass computed the jitter against the eye's submitted region; the
    // target is that size on every rig measured, and is scaled if not.
    g_draw.jx = sw ? jx * static_cast<float>(g_tc.info.a) / static_cast<float>(sw) : jx;
    g_draw.jy = sh ? jy * static_cast<float>(g_tc.info.b) / static_cast<float>(sh) : jy;
    return true;
}

bool uiLayerBegin(ID3D11DeviceContext* ctx) {
    if (!g_draw.decided || g_draw.active || !ctx) return false;
    bool ok = false;
    const bool ran = guardedBudget(g_drawBudget, [&] { ok = beginInner(ctx); });
    if (!ran) {
        // Whatever was changed before the fault, put the game's state back.
        if (g_draw.saved) guarded("uiLayer.restore", [&] { restore(ctx); });
        releaseSaved();
        g_draw.active = false;
        standDown("a fault while binding the layer for a draw");
        return false;
    }
    t_redirecting = ok;
    return ok;
}

void uiLayerEnd(ID3D11DeviceContext* ctx) {
    if (!g_draw.active) return;
    if (!guarded("uiLayer.end", [&] { restore(ctx); })) {
        standDown("a fault while putting the game's state back after a draw");
    }
    releaseSaved();
    g_draw.active = false;
    t_redirecting = false;
}

bool uiLayerRedirecting() { return t_redirecting; }

void uiLayerNoteOther(ID3D11DeviceContext* ctx, uint32_t count) {
    if (!ctx) return;
    const void* taken[2] = {nullptr, nullptr};
    for (int e = 0; e < 2; ++e) {
        if (g_eye[e].seq == g_lastRedirectSeq && g_eye[e].draws) taken[e] = g_eye[e].target;
    }
    if (!taken[0] && !taken[1]) return;
    char kind = 0;
    // A write: the draw's target is one the UI left this frame -- a compare
    // on the target cache, every draw. A read: a pass over the eye samples it
    // at t0/t1 -- full-screen passes are a handful of vertices, so only those
    // are resolved, and at most kWatchPerFrame a frame.
    if (uiLayerTargetKind() != 0 &&
        (g_tc.info.resource == taken[0] || g_tc.info.resource == taken[1])) {
        kind = 'W';
        ++g_win.afterWrites;
    } else if (count <= 6 && g_watchBudget) {
        --g_watchBudget;
        static const BindSlot kSlots[2] = {BindSlot::PsSrv0, BindSlot::PsSrv1};
        for (BindSlot slot : kSlots) {
            void* v = bindingGet(slot);
            ResourceInfo info;
            if (v && bindingResolve(v, &info) && info.resource &&
                (info.resource == taken[0] || info.resource == taken[1])) {
                kind = 'R';
                ++g_win.afterReads;
                break;
            }
        }
    }
    if (!kind) return;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs), ps = bindingShaderHash(BindSlot::Ps);
    for (uint32_t i = 0; i < g_afterSeenCount; ++i) {
        if (g_afterSeen[i].kind == kind && g_afterSeen[i].vs == vs && g_afterSeen[i].ps == ps) return;
    }
    if (g_afterSeenCount >= kMaxAfterLines) return;
    g_afterSeen[g_afterSeenCount++] = {kind, vs, ps};
    Log::get().note(
        "ui layer: after the UI, the game %s an eye target the UI was taken from: vs %016llX "
        "ps %016llX -- with the layer, that %s.",
        kind == 'W' ? "drew into" : "read", static_cast<unsigned long long>(vs),
        static_cast<unsigned long long>(ps),
        kind == 'W' ? "lands under the UI now instead of over it"
                    : "no longer sees the UI in what it reads (a post pass over the eye: the UI "
                      "misses it)");
}

void uiLayerNoteTemporal(uint64_t sequence, uint32_t eye, const void* output) {
    if (eye > 1) return;
    Eye& e = g_eye[eye];
    e.door.treatedSeq = sequence;
    e.temporalOut = output;
    e.temporalOutSeq = sequence;
    ++g_win.treated;
}

void uiLayerDoorSeen(uint64_t sequence, uint32_t eye, ID3D11Texture2D* source,
                     const float* bounds) {
    (void)bounds;
    if (eye > 1 || !source) return;
    Eye& e = g_eye[eye];
    e.door.doorSeq = sequence;
    e.doorFromPass = e.temporalOutSeq == sequence && e.temporalOut == source;
    ++g_win.doors;
    if (e.doorFromPass) {
        D3D11_TEXTURE2D_DESC d{};
        source->GetDesc(&d);
        if (d.Width != e.door.fullW || d.Height != e.door.fullH) {
            if (g_target > 0.0f) {
                const UiLayerSize s = uiLayerSize(d.Width, d.Height, g_target);
                Log::get().note(
                    "ui layer: the %s eye's door hands on %ux%u; its layer is %ux%u at %s "
                    "(%.1f MB).",
                    eye == 0 ? "left" : "right", d.Width, d.Height, s.w, s.h, g_keyText.c_str(),
                    uiLayerMB(uiLayerBytes(s.w, s.h)));
            }
            e.door.fullW = d.Width;
            e.door.fullH = d.Height;
        }
    }
}

void uiLayerNoteCopy(const void* destination, const void* source) {
    if (!destination || !source) return;
    for (Eye& e : g_eye) {
        if (e.draws && e.seq == g_lastRedirectSeq && e.target == source) {
            e.copiedTo = destination;
            e.copiedSeq = e.seq;
        }
    }
}

void uiLayerNoteSubmitted(uint64_t sequence, uint32_t eye, const void* submitted) {
    if (eye > 1) return;
    g_eye[eye].submitted = submitted;
    g_eye[eye].submittedSeq = sequence;
}

ID3D11Texture2D* uiLayerComposite(uint64_t sequence, uint32_t eye, ID3D11Texture2D* frame,
                                  const uint32_t region[4], const float* bounds) {
    if (eye > 1 || !frame || !region) return nullptr;
    Eye& e = g_eye[eye];
    if (!e.srv || !e.rtv || e.compositedSeq == sequence) return nullptr;
    const bool hasUi = e.seq == sequence && e.draws;
    // The ui_layer debug view shows the layer on every frame the key is
    // live, an empty one included -- black, so a frame whose UI the layer
    // did not take reads as missing, never as the ordinary picture.
    const bool debugEmpty = !hasUi && g_debugView && detail::g_uiLayerLive;
    if (!hasUi && !debugEmpty) return nullptr;
    e.compositedSeq = sequence;
    if (debugEmpty) {
        Ptr<ID3D11Device> dev;
        frame->GetDevice(&dev);
        Ptr<ID3D11DeviceContext> ctx;
        if (dev) dev->GetImmediateContext(&ctx);
        if (!ctx) return nullptr;
        vScreenClearRenderTargetViewRaw(ctx.Get(), e.rtv.Get(), kUiLayerClear);
        e.seq = sequence;
        e.draws = 0;
        e.target = nullptr;
    } else {
        // The eye check: did the UI this layer holds leave the target the
        // game submitted (or copied into what it submitted) for THIS eye?
        const void* mine = e.submittedSeq == sequence ? e.submitted : nullptr;
        const void* theirs = g_eye[1 - eye].submitted;  // this frame's or last: stable textures
        const void* copied = e.copiedSeq == sequence ? e.copiedTo : nullptr;
        if (!mine || mine == theirs) {
            ++g_win.eyeUntold;
        } else if (e.target == mine || copied == mine) {
            ++g_win.eyeMatched;
        } else if (theirs && (e.target == theirs || copied == theirs)) {
            ++g_win.eyeSwapped;
            static bool swappedNoted = false;
            if (!swappedNoted) {
                swappedNoted = true;
                Log::get().note(
                    "ui layer: the %s eye's layer holds UI from the target the game submitted for "
                    "the %s eye -- the eye rule (first target of a frame = left) is backwards on "
                    "this rig. advanced.ui_depth_eyes = swapped flips it (and the UI depth's with it).",
                    eye == 0 ? "left" : "right", eye == 0 ? "right" : "left");
            }
        } else {
            ++g_win.eyeUntold;
        }
    }
    if (graphicsRuntimeDisabled()) return nullptr;
    ID3D11Texture2D* result = nullptr;
    const char* why = nullptr;
    const bool ran =
        guardedBudget(g_compositeBudget, [&] { result = compositeInner(e, eye, frame, region, bounds, &why); });
    if (!result) {
        ++g_win.compositeRefused;
        // The UI of this frame is in the layer and will not reach the eye:
        // once, then the draws go back into the game's frame. (An empty
        // debug frame lost nothing, and only says why.)
        if (hasUi || !ran) {
            standDown(!ran ? "a fault in the composite" : (why ? why : "the composite refused"));
        } else {
            static bool debugRefusalNoted = false;
            if (!debugRefusalNoted) {
                debugRefusalNoted = true;
                Log::get().note("ui layer: the debug view's empty frame was not composited -- %s.",
                                why ? why : "a refusal");
            }
        }
    }
    return result;
}

void uiLayerFrameBoundary(ID3D11DeviceContext* ctx) {
    detail::g_uiLayerWatching = false;
    g_watchBudget = kWatchPerFrame;
    ++g_win.frames;
    // The warm compile, the sharpen's reason: not a first-use D3DCompile at
    // the door.
    if (ctx && g_target > 0.0f && g_temporal && !g_stoodDown) compileOnce(ctx);
    const uint64_t now = GetTickCount64();
    if (!g_winStartMs) g_winStartMs = now;
    if (now - g_winStartMs < kTotalsMs) return;
    const bool anything = g_win.redirected || g_win.composites || g_win.compositeRefused;
    if (g_target > 0.0f || anything) {
        logTotals(static_cast<double>(now - g_winStartMs) / 1000.0);
        if (!g_timingNoted && g_win.timed) {
            g_timingNoted = true;
            Log::get().note("ui layer: the composite measured %.3f ms per eye on average over its "
                            "first %u timed composites (RCAS, the step before it, measured 0.23 ms "
                            "an eye at 5792x5356).",
                            g_win.timeSum / g_win.timed, g_win.timed);
        }
    }
    g_win = Window{};
    g_winStartMs = now;
}

void uiLayerShutdown() {
    if (g_draw.active) releaseSaved();
    g_draw = Draw{};
    for (Eye& e : g_eye) releaseEye(e);
    for (QuerySlot& q : g_qring) {
        q.timer.reset();
        q.inUse = false;
    }
    for (uint32_t i = 0; i < g_blendCount; ++i) g_blends[i] = BlendEntry{};
    g_blendCount = 0;
    if (g_cb) {
        g_cb->Release();
        g_cb = nullptr;
    }
    if (g_cs) {
        g_cs->Release();
        g_cs = nullptr;
    }
    if (g_sessionRedirected) {
        Log::get().note("ui layer: %llu draws redirected this session.",
                        static_cast<unsigned long long>(g_sessionRedirected));
    }
}

}  // namespace edvr
