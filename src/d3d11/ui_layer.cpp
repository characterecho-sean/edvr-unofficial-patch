// fix.ui_quality -- the UI layer half. ui_layer.h says what it is and why;
// ui_layer_math.h holds its arithmetic and ui_layer_shaders.h its composite,
// both shared with tools/ui_quality_test; ui_surfaces.* is the other half
// (every offscreen UI surface at the target's size); docs/ui-layer-2026-09-23.md
// is the design as built.
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
#include "ui_deferred_depth.h"  // the Seeder: the game's depth-stencil at the layer's size

#include "binding_shadow.h"
#include "device_hook.h"   // deviceHookHmdQuality, for the configure line
#include "foveation.h"     // whether a shading-rate image is bound for the eye
#include "gpu_timing.h"
#include "graphics_runtime.h"
#include "shader_swap.h"
#include "ui_deferred.h"   // uiDeferredRouteCapturedThisDraw: the replay's own draws
#include "ui_depth.h"      // uiDepthEyeOfTarget: the eye, by the pass's own table
#include "ui_surfaces.h"   // the surfaces half: the target, and its summary
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
#include <memory>
#include <string>

namespace edvr {

namespace detail {
bool g_uiLayerLive = false;
bool g_uiLayerWatching = false;
bool g_uiLayerRedirecting = false;
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
bool g_jitterAsShipped = true;  // advanced.temporal_aa_jitter_sign/lag at their defaults
bool g_stoodDown = false;
std::string g_keyText = "?";
bool g_keyNoted = false;

void refreshLive() {
    detail::g_uiLayerLive = g_target > 0.0f && g_temporal && g_jitterAsShipped && !g_stoodDown;
}

void standDown(const char* why) {
    if (g_stoodDown) return;
    g_stoodDown = true;
    refreshLive();
    Log::get().note(
        "ui quality: the layer stands down for the rest of the session -- %s. The UI goes into "
        "the game's frame as before (and gets the UI depth and reactive mask again); the "
        "surfaces stay as they are. Turning fix.ui_quality off and on re-arms it.",
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
    uint32_t basisW = 0, basisH = 0;  // the door size the layer was made for
    uint64_t seq = 0;            // the frame whose draws it holds
    uint32_t draws = 0;          // redirected into it for that frame
    uint64_t compositedSeq = 0;  // the last frame the door composited (or tried)
    const void* target = nullptr;  // the game's target those draws left (identity)

    // The per-channel transmittance a multiply leaves (made at the first
    // multiply, cleared to 1 at the first multiply of each frame).
    Ptr<ID3D11Texture2D> mTex;
    Ptr<ID3D11RenderTargetView> mRtv;
    Ptr<ID3D11ShaderResourceView> mSrv;
    uint32_t mW = 0, mH = 0;
    uint64_t mSeq = 0;

    // The layer's depth-stencil target, for UI that TESTS depth or stencil:
    // the game's own, resampled to the layer's size with the jitter
    // cancelled, at the first such draw of the frame (and again whenever the
    // game writes its own buffer, or a later draw reads bits not seeded).
    Ptr<ID3D11Texture2D> dsTex;
    Ptr<ID3D11DepthStencilView> dsv;
    uint32_t dsW = 0, dsH = 0;
    DXGI_FORMAT dsViewFmt = DXGI_FORMAT_UNKNOWN;
    uint64_t dsSeq = 0;          // seeded for this frame (0: not, or stale)
    const void* dsSource = nullptr;  // the game's depth texture it was seeded from
    uint8_t dsSeededMask = 0;    // stencil bits seeded
    bool dsSeededDepth = false;
    // The copy of the game's depth-stencil the seed reads.
    Ptr<ID3D11Texture2D> dsCopy;
    Ptr<ID3D11ShaderResourceView> dsCopyDepth, dsCopyStencil;
    uint32_t dsCopyW = 0, dsCopyH = 0;
    DXGI_FORMAT dsCopyFmt = DXGI_FORMAT_UNKNOWN;

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
    bool saved = false;    // the game's state is held below: restore it on any exit
    bool counted = false;  // this decision's draw is counted (a fallback re-issue is not)
    int eye = -1;
    UiLayerFamily family = UiLayerFamily::kNone;
    uint64_t seq = 0;
    const void* targetRes = nullptr;
    uint32_t targetW = 0, targetH = 0;
    float jx = 0.0f, jy = 0.0f;  // this frame's jitter, in the target's pixels
    UiBlendShape shape = UiBlendShape::kRefused;  // as decided
    UiDsEffect ds;               // what it does with the game's depth-stencil
    uint8_t stencilRead = 0;     // its stencil read mask, for the seed
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
    // The write-back's own save (the game's targets, while its depth-only
    // re-issue runs with no colour target bound).
    ID3D11RenderTargetView* wbRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* wbDsv = nullptr;
    bool wbActive = false;
};
Draw g_draw;
uint64_t g_lastRedirectSeq = 0;
bool g_familyEngaged[static_cast<size_t>(UiLayerFamily::kCount)] = {};
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

// The seed's machinery: ui_deferred_depth.h's Seeder (rig-tested in
// tools/ui_deferred_test/depth_test.cpp) on a deferred context, executed on
// the immediate one with its state restored.
std::unique_ptr<edvr_deferred_depth::Seeder> g_seeder;
Ptr<ID3D11DeviceContext> g_deferred;
bool g_seederTried = false;

// --------------------------------------------------------------- counters

struct Window {
    uint64_t frames = 0;
    uint64_t decided[static_cast<size_t>(UiLayerFamily::kCount)]
                    [static_cast<size_t>(UiLayerDecision::kCount)] = {};
    uint64_t redirected = 0, refusedAtIssue = 0;
    uint64_t viewportRemaps = 0, scissorRemaps = 0, jitterCancels = 0, clears = 0;
    uint64_t multiplies = 0, writeBacks = 0, dsTested = 0, seeds = 0, seedFailures = 0;
    uint64_t lostLayers = 0, doors = 0, treated = 0, composites = 0, overGameImage = 0;
    uint64_t compositeRefused = 0, afterWrites = 0, afterReads = 0, debugComposites = 0;
    uint64_t eyeMatched = 0, eyeSwapped = 0, eyeUntold = 0, seedStale = 0;
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
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
        default: return "another format";
    }
}

bool structuralDecision(UiLayerDecision d) {
    // The transient ones -- not armed yet, late this frame -- are counted on
    // the totals line and never get a first-seen line of their own.
    return d != UiLayerDecision::kNotArmed && d != UiLayerDecision::kLate &&
           d != UiLayerDecision::kNotUi;
}

// Once per (family, outcome, shader pair): what it is, where it draws, what
// was decided -- with the blend or depth-stencil state that decided it, so a
// refusal names the numbers rather than a category.
void noteFamily(UiLayerFamily f, UiLayerDecision d, const char* detail) {
    if (!structuralDecision(d)) return;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs), ps = bindingShaderHash(BindSlot::Ps);
    for (uint32_t i = 0; i < g_familySeenCount; ++i) {
        const FamilySeen& s = g_familySeen[i];
        if (s.family == uint8_t(f) && s.decision == uint8_t(d) && s.vs == vs && s.ps == ps) return;
    }
    if (g_familySeenCount >= kMaxFamilyLines) return;
    g_familySeen[g_familySeenCount++] = {uint8_t(f), uint8_t(d), vs, ps};
    Log::get().note("ui quality: layer: %s (vs %016llX ps %016llX) into a %ux%u %s target: %s%s%s.",
                    uiLayerFamilyName(f), static_cast<unsigned long long>(vs),
                    static_cast<unsigned long long>(ps), g_tc.info.a, g_tc.info.b,
                    viewName(g_tc.view), uiLayerDecisionName(d), detail && *detail ? " -- " : "",
                    detail ? detail : "");
}

// The census's own notation for a blend (bl=enable src,dst,op/srcA,dstA,opA
// and the write mask) and a depth-stencil state, for those lines.
void describeBlend(const UiBlendRt& b, char* out, size_t n) {
    _snprintf_s(out, n, _TRUNCATE, "bl=%u%u,%u,%u/%u,%u,%u bm=%X", b.enable ? 1u : 0u, b.src, b.dst,
                b.op, b.srcA, b.dstA, b.opA, b.mask);
}
void describeDs(const UiDsState& s, UINT ref, DXGI_FORMAT viewFmt, char* out, size_t n) {
    _snprintf_s(out, n, _TRUNCATE,
                "depth %u func %u write %u; stencil %u ref %u read %02X write %02X front "
                "func %u ops %u,%u,%u back func %u ops %u,%u,%u; %s%s%s",
                s.depthEnable ? 1u : 0u, s.depthFunc, s.depthWriteAll ? 1u : 0u,
                s.stencilEnable ? 1u : 0u, ref, s.readMask, s.writeMask, s.front.func, s.front.fail,
                s.front.depthFail, s.front.pass, s.back.func, s.back.fail, s.back.depthFail,
                s.back.pass, viewName(viewFmt), s.readOnlyDepth ? " read-only depth" : "",
                s.readOnlyStencil ? " read-only stencil" : "");
}

// ------------------------------------------------------------ the layer

bool makeTarget(ID3D11Device* dev, uint32_t w, uint32_t h, Ptr<ID3D11Texture2D>* tex,
                Ptr<ID3D11RenderTargetView>* rtv, Ptr<ID3D11ShaderResourceView>* srv) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, tex->ReleaseAndGetAddressOf())) ||
        FAILED(dev->CreateRenderTargetView(tex->Get(), nullptr, rtv->ReleaseAndGetAddressOf())) ||
        FAILED(dev->CreateShaderResourceView(tex->Get(), nullptr, srv->ReleaseAndGetAddressOf()))) {
        tex->Reset();
        rtv->Reset();
        srv->Reset();
        return false;
    }
    return true;
}

bool ensureLayer(ID3D11Device* dev, Eye& e, uint32_t w, uint32_t h, int eye) {
    if (e.tex && e.w == w && e.h == h) return true;
    const bool resized = e.tex != nullptr;
    e.w = e.h = 0;
    e.seq = 0;
    e.draws = 0;
    if (!dev || !w || !h || !makeTarget(dev, w, h, &e.tex, &e.rtv, &e.srv)) return false;
    e.w = w;
    e.h = h;
    Log::get().note("ui quality: layer: %s eye's layer %s at %ux%u (R8G8B8A8_UNORM, %.1f MB).",
                    eye == 0 ? "left" : "right", resized ? "re-created" : "created", w, h,
                    uiLayerMB(uiLayerBytes(w, h)));
    return true;
}

bool ensureLayerFor(ID3D11DeviceContext* ctx, int eye) {
    Eye& e = g_eye[eye];
    const UiLayerSize s = uiLayerSize(e.door.fullW, e.door.fullH, g_target);
    if (!s.w || !s.h) return false;
    if (e.tex && e.w == s.w && e.h == s.h) {
        e.basisW = e.door.fullW;
        e.basisH = e.door.fullH;
        return true;
    }
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!ensureLayer(dev.Get(), e, s.w, s.h, eye)) return false;
    e.basisW = e.door.fullW;
    e.basisH = e.door.fullH;
    return true;
}

// The per-channel transmittance, at the layer's size, made at the first
// multiply the eye sees.
bool ensureMult(ID3D11DeviceContext* ctx, Eye& e, int eye) {
    if (e.mTex && e.mW == e.w && e.mH == e.h) return true;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    e.mW = e.mH = 0;
    e.mSeq = 0;
    if (!dev || !e.w || !makeTarget(dev.Get(), e.w, e.h, &e.mTex, &e.mRtv, &e.mSrv)) return false;
    e.mW = e.w;
    e.mH = e.h;
    Log::get().note("ui quality: layer: %s eye's multiply transmittance created at %ux%u (%.1f MB) "
                    "-- a draw that tints the frame by its colour went into the layer.",
                    eye == 0 ? "left" : "right", e.w, e.h, uiLayerMB(uiLayerBytes(e.w, e.h)));
    return true;
}

// Every layer, transmittance, depth target and composite output released
// (the door state kept): the key went off, the pass went away, or the layer
// stood down. About 380 MB at 1.25 on a 4340x4284 eye is not left resident
// for a feature that is off.
bool releaseLayers() {
    bool any = false;
    for (Eye& e : g_eye) {
        any = any || e.tex || e.mTex || e.dsTex || e.dsCopy || e.out || e.copy || e.frameSrv;
        e.tex.Reset();
        e.rtv.Reset();
        e.srv.Reset();
        e.w = e.h = e.basisW = e.basisH = 0;
        e.seq = 0;
        e.draws = 0;
        e.mTex.Reset();
        e.mRtv.Reset();
        e.mSrv.Reset();
        e.mW = e.mH = 0;
        e.mSeq = 0;
        e.dsTex.Reset();
        e.dsv.Reset();
        e.dsW = e.dsH = 0;
        e.dsViewFmt = DXGI_FORMAT_UNKNOWN;
        e.dsSeq = 0;
        e.dsSource = nullptr;
        e.dsCopy.Reset();
        e.dsCopyDepth.Reset();
        e.dsCopyStencil.Reset();
        e.dsCopyW = e.dsCopyH = 0;
        e.dsCopyFmt = DXGI_FORMAT_UNKNOWN;
        e.out.Reset();
        e.outUav.Reset();
        e.outW = e.outH = 0;
        e.outFmt = DXGI_FORMAT_UNKNOWN;
        e.frameSrv.Reset();
        e.frameRes = nullptr;
        e.frameView = DXGI_FORMAT_UNKNOWN;
        e.copy.Reset();
        e.copySrv.Reset();
        e.copyW = e.copyH = 0;
        e.copyFmt = DXGI_FORMAT_UNKNOWN;
    }
    return any;
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
        rt = uiLayerBlendRtFrom(d.RenderTarget[0]);
        if (rtOut) *rtOut = rt;
        if (d.AlphaToCoverageEnable) return UiBlendShape::kRefused;
        Ptr<ID3D11BlendState1> bs1;
        if (SUCCEEDED(bs->QueryInterface(__uuidof(ID3D11BlendState1),
                                         reinterpret_cast<void**>(bs1.GetAddressOf()))) &&
            bs1) {
            D3D11_BLEND_DESC1 d1{};
            bs1->GetDesc1(&d1);
            if (d1.RenderTarget[0].LogicOpEnable) return UiBlendShape::kRefused;
        }
    } else if (rtOut) {
        *rtOut = rt;
    }
    return uiLayerBlendShape(rt);
}

// The depth-stencil state bound now, as UiDsState, with its reference.
UiDsState dsStateOf(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT* refOut,
                    DXGI_FORMAT* viewFmtOut) {
    Ptr<ID3D11DepthStencilState> dss;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&dss, &ref);
    D3D11_DEPTH_STENCIL_DESC d{};
    if (dss) dss->GetDesc(&d);
    UINT flags = 0;
    DXGI_FORMAT viewFmt = DXGI_FORMAT_UNKNOWN;
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        dsv->GetDesc(&vd);
        flags = vd.Flags;
        viewFmt = vd.Format;
    }
    if (refOut) *refOut = ref;
    if (viewFmtOut) *viewFmtOut = viewFmt;
    return uiLayerDsStateFrom(dss ? &d : nullptr, flags);
}

// Typeless family and the two shader views of a depth-stencil view format;
// false for one the Seeder does not read (D16).
bool dsFormats(DXGI_FORMAT view, DXGI_FORMAT* typeless, DXGI_FORMAT* depthSrv,
               DXGI_FORMAT* stencilSrv) {
    switch (view) {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            *typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
            *depthSrv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            *stencilSrv = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
            return true;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            *typeless = DXGI_FORMAT_R24G8_TYPELESS;
            *depthSrv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            *stencilSrv = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            return true;
        case DXGI_FORMAT_D32_FLOAT:
            *typeless = DXGI_FORMAT_R32_TYPELESS;
            *depthSrv = DXGI_FORMAT_R32_FLOAT;
            *stencilSrv = DXGI_FORMAT_UNKNOWN;
            return true;
        default:
            return false;
    }
}

bool ensureSeeder(ID3D11Device* dev) {
    if (g_seeder && g_deferred) return true;
    if (g_seederTried || !dev) return false;
    g_seederTried = true;
    try {
        auto seeder = std::make_unique<edvr_deferred_depth::Seeder>();
        seeder->init(dev);
        Ptr<ID3D11DeviceContext> deferred;
        if (FAILED(dev->CreateDeferredContext(0, &deferred)) || !deferred) {
            Log::get().note("ui quality: layer: no deferred context for the depth-stencil seed; "
                            "depth- or stencil-tested UI stays in the picture.");
            return false;
        }
        g_seeder = std::move(seeder);
        g_deferred = deferred;
        Log::get().note(
            "ui quality: layer: the depth-stencil seed is ready (stencil written %s) -- UI that "
            "tests the game's depth or stencil is drawn against the game's own buffer resampled "
            "to the layer's size.",
            g_seeder->usesSpecifiedStencilRef() ? "in one pass" : "one pass per bit");
        return true;
    } catch (const std::exception& ex) {
        Log::get().note("ui quality: layer: the depth-stencil seed could not be built (%s); "
                        "depth- or stencil-tested UI stays in the picture.",
                        ex.what());
        return false;
    }
}

// Can the layer reproduce this draw's tests against its own seeded copy?
bool dsReproducible(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, uint32_t targetW,
                    uint32_t targetH, char* why, size_t whyN) {
    if (!dsv) return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    dsv->GetDesc(&vd);
    DXGI_FORMAT tf, df, sf;
    if (vd.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice != 0) {
        _snprintf_s(why, whyN, _TRUNCATE, "the depth target is not a single 2D level");
        return false;
    }
    if (vd.Flags != 0) {
        _snprintf_s(why, whyN, _TRUNCATE, "the depth target is bound read-only");
        return false;
    }
    if (!dsFormats(vd.Format, &tf, &df, &sf)) {
        _snprintf_s(why, whyN, _TRUNCATE, "the depth format %s is not one the seed reads",
                    viewName(vd.Format));
        return false;
    }
    Ptr<ID3D11Resource> res;
    dsv->GetResource(&res);
    Ptr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    if (td.ArraySize != 1 || td.SampleDesc.Count != 1 || td.Width != targetW ||
        td.Height != targetH) {
        _snprintf_s(why, whyN, _TRUNCATE, "the depth target is %ux%u x%u samples, not the %ux%u eye",
                    td.Width, td.Height, td.SampleDesc.Count, targetW, targetH);
        return false;
    }
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!ensureSeeder(dev.Get())) {
        _snprintf_s(why, whyN, _TRUNCATE, "the depth-stencil seed is unavailable");
        return false;
    }
    return true;
}

// The layer's depth-stencil target, seeded from the game's own at the
// layer's size with this frame's jitter cancelled: the Seeder maps a layer
// pixel p to the game pixel floor(p * game / layer + jitter), which is the
// redirected viewport's own map inverted.
bool seedLayerDepth(ID3D11DeviceContext* ctx, Eye& e, int eye, ID3D11DepthStencilView* gameDsv,
                    uint8_t stencilMask, bool needDepth) {
    if (!gameDsv || !g_seeder || !g_deferred) return false;
    Ptr<ID3D11Resource> res;
    gameDsv->GetResource(&res);
    Ptr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    gameDsv->GetDesc(&vd);
    DXGI_FORMAT tf, df, sf;
    if (!dsFormats(vd.Format, &tf, &df, &sf)) return false;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    // The copy the seed reads: the game's texture as it stands, so a view of
    // it is never bound while the game's own depth view is.
    if (!e.dsCopy || e.dsCopyW != td.Width || e.dsCopyH != td.Height || e.dsCopyFmt != tf) {
        e.dsCopy.Reset();
        e.dsCopyDepth.Reset();
        e.dsCopyStencil.Reset();
        e.dsCopyW = e.dsCopyH = 0;
        D3D11_TEXTURE2D_DESC cd{};
        cd.Width = td.Width;
        cd.Height = td.Height;
        cd.MipLevels = cd.ArraySize = 1;
        cd.Format = tf;
        cd.SampleDesc.Count = 1;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = df;
        if (FAILED(dev->CreateTexture2D(&cd, nullptr, &e.dsCopy)) ||
            FAILED(dev->CreateShaderResourceView(e.dsCopy.Get(), &sd, &e.dsCopyDepth))) {
            e.dsCopy.Reset();
            e.dsCopyDepth.Reset();
            return false;
        }
        if (sf != DXGI_FORMAT_UNKNOWN) {
            sd.Format = sf;
            if (FAILED(dev->CreateShaderResourceView(e.dsCopy.Get(), &sd, &e.dsCopyStencil))) {
                e.dsCopy.Reset();
                e.dsCopyDepth.Reset();
                return false;
            }
        }
        e.dsCopyW = td.Width;
        e.dsCopyH = td.Height;
        e.dsCopyFmt = tf;
    }
    // The layer's own target, at the layer's size, in the game's format.
    if (!e.dsTex || e.dsW != e.w || e.dsH != e.h || e.dsViewFmt != vd.Format) {
        e.dsTex.Reset();
        e.dsv.Reset();
        e.dsW = e.dsH = 0;
        e.dsSeq = 0;
        D3D11_TEXTURE2D_DESC ld{};
        ld.Width = e.w;
        ld.Height = e.h;
        ld.MipLevels = ld.ArraySize = 1;
        ld.Format = tf;
        ld.SampleDesc.Count = 1;
        ld.Usage = D3D11_USAGE_DEFAULT;
        ld.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC lv{};
        lv.Format = vd.Format;
        lv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateTexture2D(&ld, nullptr, &e.dsTex)) ||
            FAILED(dev->CreateDepthStencilView(e.dsTex.Get(), &lv, &e.dsv))) {
            e.dsTex.Reset();
            e.dsv.Reset();
            return false;
        }
        e.dsW = e.w;
        e.dsH = e.h;
        e.dsViewFmt = vd.Format;
        const uint32_t bpp = vd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? 8u : 4u;
        Log::get().note("ui quality: layer: %s eye's depth-stencil target created at %ux%u (%s, "
                        "%.1f MB; with the copy of the game's it reads, %.1f MB) -- a UI draw "
                        "tests the game's depth or stencil.",
                        eye == 0 ? "left" : "right", e.w, e.h, viewName(vd.Format),
                        uiLayerMB(uiLayerBytes(e.w, e.h, bpp)),
                        uiLayerMB(uiLayerBytes(e.w, e.h, bpp) +
                                  uiLayerBytes(td.Width, td.Height, bpp)));
    }
    vScreenCopyResourceRaw(ctx, e.dsCopy.Get(), tex.Get());
    bool recorded = false;
    try {
        g_seeder->seed(g_deferred.Get(), e.dsCopyDepth.Get(), e.dsCopyStencil.Get(), e.dsv.Get(),
                       td.Width, td.Height, e.w, e.h, g_draw.jx, g_draw.jy, stencilMask, needDepth);
        recorded = true;
    } catch (const std::exception&) {
        recorded = false;
    }
    Ptr<ID3D11CommandList> list;
    const HRESULT fin = g_deferred->FinishCommandList(FALSE, &list);
    if (!recorded || FAILED(fin) || !list) return false;
    vScreenExecuteCommandListRaw(ctx, list.Get(), 1);
    // What the seed wrote: with SV_StencilRef, one pass copies the depth and
    // all eight stencil bits whenever it draws at all; without it, the depth
    // when asked and one pass per asked bit (the rest cleared to 0).
    const bool hasStencil = e.dsCopyStencil != nullptr;
    const bool full = hasStencil && g_seeder->usesSpecifiedStencilRef() && (needDepth || stencilMask);
    e.dsSeq = g_draw.seq;
    e.dsSource = tex.Get();
    e.dsSeededMask = full ? 0xFF : (hasStencil ? stencilMask : 0);
    e.dsSeededDepth = needDepth || full;
    ++g_win.seeds;
    return true;
}

// The resource a depth-stencil view is over, as an identity.
const void* dsvResource(ID3D11DepthStencilView* dsv) {
    if (!dsv) return nullptr;
    Ptr<ID3D11Resource> res;
    dsv->GetResource(&res);
    return res.Get();
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

// One issue of a decided draw into the layer (which = 0) or into the
// multiply transmittance (which = 1).
bool beginInner(ID3D11DeviceContext* ctx, int which) {
    Eye& e = g_eye[g_draw.eye];
    ID3D11RenderTargetView* target = which ? e.mRtv.Get() : e.rtv.Get();
    if (!target) return false;
    // The blend at the moment of issue: a verdict's own Begin runs between
    // the decision and here, and the shape must still be the decided one.
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    ctx->OMGetBlendState(&bs, factor, &sampleMask);
    UiBlendRt game, conv;
    ID3D11BlendState* layerBlend = nullptr;
    const UiBlendShape shape = shapeOf(bs, &game);
    const bool converted = shape == g_draw.shape &&
                           (which ? uiLayerMultiplyBlend(game, &conv) : uiLayerConvertBlend(game, &conv));
    if (!converted || !(layerBlend = cachedBlend(ctx, conv))) {
        if (bs) bs->Release();
        ++g_win.refusedAtIssue;
        return false;
    }
    // A new frame for this eye's layer: clear it, and count a layer the door
    // never composited (the frame took a path without a door, or a withhold).
    if (which == 0 && e.seq != g_draw.seq) {
        if (e.seq && e.draws && e.compositedSeq != e.seq) ++g_win.lostLayers;
        vScreenClearRenderTargetViewRaw(ctx, e.rtv.Get(), kUiLayerClear);
        e.seq = g_draw.seq;
        e.draws = 0;
        e.target = g_draw.targetRes;
        ++g_win.clears;
    }
    if (which == 1 && e.mSeq != g_draw.seq) {
        vScreenClearRenderTargetViewRaw(ctx, e.mRtv.Get(), kUiLayerMultClear);
        e.mSeq = g_draw.seq;
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
    // The layer's depth-stencil target: for a draw that tests, seeded from
    // the game's own first (or again, when stale or short of the bits this
    // draw reads); for a draw that only writes, bound once seeded this frame
    // so later tests in the layer see its write.
    ID3D11DepthStencilView* layerDsv = nullptr;
    const void* gameDs = dsvResource(g_draw.dsv);
    const bool seededNow = e.dsv && e.dsSeq == g_draw.seq && e.dsSource && e.dsSource == gameDs;
    const bool needsDs = g_draw.ds.tests() || (g_draw.ds.writes() && seededNow);
    if (needsDs) {
        const bool stale = !seededNow;
        const uint8_t wantMask = g_draw.ds.stencilTest ? g_draw.stencilRead : 0;
        const bool shortBits = (wantMask & ~e.dsSeededMask) != 0;
        const bool shortDepth = g_draw.ds.depthTest && !e.dsSeededDepth;
        if (g_draw.ds.tests() && (stale || shortBits || shortDepth)) {
            const uint8_t mask =
                static_cast<uint8_t>(wantMask | (stale ? 0 : e.dsSeededMask));
            const bool depth = g_draw.ds.depthTest || (!stale && e.dsSeededDepth);
            if (!seedLayerDepth(ctx, e, g_draw.eye, g_draw.dsv, mask, depth)) {
                ++g_win.seedFailures;
                releaseSaved();
                ++g_win.refusedAtIssue;
                return false;
            }
        }
        layerDsv = e.dsv.Get();
        if (g_draw.ds.tests() && !g_draw.counted) ++g_win.dsTested;
    }
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
    vScreenSetRenderTargetsRaw(ctx, 1, &target, layerDsv);
    g_draw.active = true;

    // Counted once per draw: the curved screen's fall-through re-issues the
    // same draw after a failed substitution, and a multiply's second draw is
    // the same draw too.
    if (!g_draw.counted) {
        g_draw.counted = true;
        ++e.draws;
        ++g_win.redirected;
        ++g_sessionRedirected;
        g_win.viewportRemaps += g_draw.vpCount;
        if (g_draw.scissorSet) g_win.scissorRemaps += g_draw.scCount;
        if (g_draw.jx != 0.0f || g_draw.jy != 0.0f) ++g_win.jitterCancels;
    }
    if (which == 1) ++g_win.multiplies;
    g_lastRedirectSeq = g_draw.seq;
    detail::g_uiLayerWatching = true;
    // Once per family: where its first draw landed and the cancel it took,
    // so a family placed in fixed clip space rather than through the eye's
    // projection (which the cancel would jitter) can be told on the flight.
    bool& familyEngaged = g_familyEngaged[static_cast<size_t>(g_draw.family)];
    if (!familyEngaged && g_draw.vpCount && which == 0) {
        familyEngaged = true;
        const D3D11_VIEWPORT& gv = g_draw.vp[0];
        Log::get().note(
            "ui quality: layer: first %s draw in the %s eye's layer -- the game's viewport "
            "(%.1f, %.1f) %.1fx%.1f became (%.3f, %.3f) %.1fx%.1f, a jitter cancel of (%.3f, "
            "%.3f) layer pixels.",
            uiLayerFamilyName(g_draw.family), g_draw.eye == 0 ? "left" : "right",
            static_cast<double>(gv.TopLeftX), static_cast<double>(gv.TopLeftY),
            static_cast<double>(gv.Width), static_cast<double>(gv.Height),
            static_cast<double>(vp[0].TopLeftX), static_cast<double>(vp[0].TopLeftY),
            static_cast<double>(vp[0].Width), static_cast<double>(vp[0].Height),
            static_cast<double>(cx), static_cast<double>(cy));
    }
    if (!g_engageNoted && which == 0) {
        g_engageNoted = true;
        Log::get().note(
            "ui quality: layer: engaged -- the %s went into the %s eye's layer (%ux%u) from a "
            "%ux%u %s target: viewport scale %.4f x %.4f, jitter cancel (%.3f, %.3f) layer "
            "pixels, blend %s with transmittance in alpha.",
            uiLayerFamilyName(g_draw.family), g_draw.eye == 0 ? "left" : "right", e.w, e.h,
            g_draw.targetW, g_draw.targetH, viewName(g_tc.view), static_cast<double>(m.ax),
            static_cast<double>(m.ay), static_cast<double>(cx), static_cast<double>(cy),
            uiBlendShapeName(shape));
    }
    return true;
}

bool beginGuarded(ID3D11DeviceContext* ctx, int which) {
    if (!g_draw.decided || g_draw.active || !ctx) return false;
    bool ok = false;
    const bool ran = guardedBudget(g_drawBudget, [&] { ok = beginInner(ctx, which); });
    if (!ran) {
        // Whatever was changed before the fault, put the game's state back.
        if (g_draw.saved) guarded("uiLayer.restore", [&] { restore(ctx); });
        releaseSaved();
        g_draw.active = false;
        detail::g_uiLayerRedirecting = false;
        standDown("a fault while binding the layer for a draw");
        return false;
    }
    detail::g_uiLayerRedirecting = ok;
    return ok;
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
                               "main", "ui_layer_composite_cs", nullptr, "ui quality: layer");
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
                                const uint32_t region[4], const float layerUv[4], bool useMult,
                                const char** why) {
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
    float uv[4] = {layerUv[0], layerUv[1], layerUv[2], layerUv[3]};
    if (!uiLayerRegionMatches(rw, rh, uv, e.w, e.h)) {
        // The frame this layer was sized for (the door's previous size) has
        // changed aspect this frame -- the per-eye width, an FOV trim: the
        // layer still covers the same frustum and the shader scales each
        // axis on its own, so one frame lands stretched and the next layer
        // is made for the new size. Anything else is a frame that does not
        // hold the layer's eye (half of a double-wide texture): refused.
        const bool resized = e.basisW != e.door.fullW || e.basisH != e.door.fullH;
        if (!resized) {
            *why = "the frame's region does not describe the layer's eye (a half of a "
                   "double-wide texture?)";
            return nullptr;
        }
        static bool stretchNoted = false;
        if (!stretchNoted) {
            stretchNoted = true;
            Log::get().note("ui quality: layer: the door's frame changed shape under a layer made "
                            "for the old one; composited stretched for that frame, the next layer "
                            "is made for the new size.");
        }
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
        Log::get().note("ui quality: layer: %s eye's composite output %ux%u (%s, %.1f MB).",
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
    p.useMult = useMult ? 1u : 0u;
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
    ID3D11ShaderResourceView* savedSrv[3] = {};
    ID3D11UnorderedAccessView* savedUav = nullptr;
    ID3D11Buffer* savedCb = nullptr;
    ctx->CSGetShader(&savedCs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 3, savedSrv);
    ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
    ctx->CSGetConstantBuffers(0, 1, &savedCb);

    const int qs = acquireSlot(dev.Get(), ctx.Get());
    if (viaCopy) {
        D3D11_BOX box{region[0], region[1], 0, region[2], region[3], 1};
        ctx->CopySubresourceRegion(e.copy.Get(), 0, 0, 0, 0, frame, 0, &box);
    }
    ID3D11ShaderResourceView* nullSrv[3] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 3, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ID3D11ShaderResourceView* srvs[3] = {frameSrv, e.srv.Get(), useMult ? e.mSrv.Get() : nullptr};
    ctx->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uav = e.outUav.Get();
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
    if (qs >= 0) g_qring[qs].timer.end(ctx.Get());

    ctx->CSSetShaderResources(0, 3, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(savedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 3, savedSrv);
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
            "ui quality: layer: first composite -- the %s eye's %ux%u layer over a %ux%u %s frame "
            "(layer rectangle u %.3f..%.3f, v %.3f..%.3f), after the upscale and RCAS, before "
            "EDVR's menu%s. The one order that differs from the game's own: whatever the game "
            "drew into an eye AFTER a redirected draw is under the UI now (counted on the gates "
            "line as 'after the UI').",
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

// Can a composite run over the frames this door hands on? The format, the
// GPU's typed stores for it and the shader, each refused once with a line
// and cached, so the layer is never armed behind a composite that would
// refuse with UI already in it.
bool doorCanComposite(ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    const DXGI_FORMAT view = uiLayerFrameView(d.Format);
    static bool formatNoted = false, uavNoted = false, shaderNoted = false;
    if (view == DXGI_FORMAT_UNKNOWN || d.SampleDesc.Count != 1 || d.ArraySize != 1 ||
        d.MipLevels != 1) {
        if (!formatNoted) {
            formatNoted = true;
            Log::get().note("ui quality: layer: the pass hands on a %ux%u %s (DXGI_FORMAT %d, %u "
                            "samples, %u mips) -- not an 8-bit UNORM eye; the layer is not armed "
                            "(the composite blends only in the space the game's UI composites "
                            "did).",
                            d.Width, d.Height, viewName(d.Format), static_cast<int>(d.Format),
                            d.SampleDesc.Count, d.MipLevels);
        }
        return false;
    }
    Ptr<ID3D11Device> dev;
    source->GetDevice(&dev);
    Ptr<ID3D11DeviceContext> ctx;
    if (dev) dev->GetImmediateContext(&ctx);
    if (!dev || !ctx) return false;
    const int fi = view == DXGI_FORMAT_R8G8B8A8_UNORM ? 0 : 1;
    if (!g_fmtChecked[fi]) {
        g_fmtChecked[fi] = true;
        UINT support = 0;
        g_fmtOk[fi] = SUCCEEDED(dev->CheckFormatSupport(view, &support)) &&
                      (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    }
    if (!g_fmtOk[fi]) {
        if (!uavNoted) {
            uavNoted = true;
            Log::get().note("ui quality: layer: this GPU has no typed unordered-access store for "
                            "%s; the layer is not armed.",
                            viewName(view));
        }
        return false;
    }
    compileOnce(ctx.Get());
    if (!g_cs) {
        if (!shaderNoted) {
            shaderNoted = true;
            Log::get().note("ui quality: layer: the composite shader did not compile (the line "
                            "above says why); the layer is not armed.");
        }
        return false;
    }
    return true;
}

// ------------------------------------------------------------ the totals

void appendf(std::string& s, const char* fmt, ...) {
    char buf[320];
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
    char surfaces[640];
    uiSurfacesSummary(surfaces, sizeof(surfaces));
    const Eye& l = g_eye[0];
    Log::get().note(
        "ui quality: %s -- surfaces: %s; layer: %.0f s, %llu frames, %ux%u per eye (%.1f MB "
        "each) + composite output %ux%u (%.1f MB each)%s; %.2f draws a frame redirected (%s), "
        "%.2f multiplies, %.2f depth/stencil write-backs, %.2f tested against a seeded copy "
        "(%llu seeds, %llu failed, %llu stale); per frame %.2f viewport remaps, %.2f scissor "
        "remaps, %.2f jitter cancels; composite %.3f ms per eye average, %.3f max, %u timed of "
        "%llu composites (%llu over the game's own image, %llu in the ui_layer debug view).",
        g_keyText.c_str(), surfaces, seconds, static_cast<unsigned long long>(g_win.frames),
        l.w, l.h, uiLayerMB(uiLayerBytes(l.w, l.h)), l.outW, l.outH,
        uiLayerMB(uiLayerBytes(l.outW, l.outH)),
        l.mTex ? " + multiply transmittance" : "",
        static_cast<double>(g_win.redirected) / frames, taken.empty() ? "none" : taken.c_str(),
        static_cast<double>(g_win.multiplies) / frames,
        static_cast<double>(g_win.writeBacks) / frames,
        static_cast<double>(g_win.dsTested) / frames,
        static_cast<unsigned long long>(g_win.seeds),
        static_cast<unsigned long long>(g_win.seedFailures),
        static_cast<unsigned long long>(g_win.seedStale),
        static_cast<double>(g_win.viewportRemaps) / frames,
        static_cast<double>(g_win.scissorRemaps) / frames,
        static_cast<double>(g_win.jitterCancels) / frames,
        g_win.timed ? g_win.timeSum / g_win.timed : 0.0, g_win.timeMax, g_win.timed,
        static_cast<unsigned long long>(g_win.composites),
        static_cast<unsigned long long>(g_win.overGameImage),
        static_cast<unsigned long long>(g_win.debugComposites));
    Log::get().note("ui quality: left in the game's frame: %s.",
                    left.empty() ? "nothing classified" : left.c_str());
    uint64_t late = 0;
    for (size_t f = 1; f < static_cast<size_t>(UiLayerFamily::kCount); ++f)
        late += g_win.decided[f][static_cast<size_t>(UiLayerDecision::kLate)];
    Log::get().note(
        "ui quality: gates: G1 -- %llu UI draws arrived after their eye's composite had run "
        "(left in the game's frame); %llu layers never reached the door; the door ran %llu "
        "times, the pass treated %llu eyes; %llu composites refused; %llu redirected draws "
        "refused at issue (a changed blend, or a seed that failed); after the UI the game drew "
        "%llu times into, and %llu times read, an eye target the UI was taken from (those now "
        "land under it, or miss it); eye check against the game's Submit: %llu matched, %llu "
        "SWAPPED, %llu could not be told; %llu redirected this session%s.",
        static_cast<unsigned long long>(late), static_cast<unsigned long long>(g_win.lostLayers),
        static_cast<unsigned long long>(g_win.doors), static_cast<unsigned long long>(g_win.treated),
        static_cast<unsigned long long>(g_win.compositeRefused),
        static_cast<unsigned long long>(g_win.refusedAtIssue),
        static_cast<unsigned long long>(g_win.afterWrites),
        static_cast<unsigned long long>(g_win.afterReads),
        static_cast<unsigned long long>(g_win.eyeMatched),
        static_cast<unsigned long long>(g_win.eyeSwapped),
        static_cast<unsigned long long>(g_win.eyeUntold),
        static_cast<unsigned long long>(g_sessionRedirected),
        g_stoodDown ? " -- the layer STOOD DOWN (the line above says why)" : "");
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
    // The cancel follows the shipped jitter convention (as_is, no lag); the
    // two switches that re-read it are diagnostics of the pass's reading,
    // and while either is set the game's pixels may sit where the cancel
    // does not expect them -- the layer waits rather than guess.
    const bool jitterAsShipped =
        _stricmp(cfg.getString("advanced.temporal_aa_jitter_sign", "as_is").c_str(), "as_is") == 0 &&
        !(cfg.getFloat("advanced.temporal_aa_jitter_lag", 0.0f) >= 0.5f);
    const bool changed = !g_keyNoted || text != g_keyText || target != g_target ||
                         temporal != g_temporal || debugView != g_debugView ||
                         jitterAsShipped != g_jitterAsShipped;
    // A live change of the key re-arms a stood-down layer: the player asked.
    if (g_keyNoted && (text != g_keyText || target != g_target)) g_stoodDown = false;
    g_keyText = text;
    g_target = target;
    g_temporal = temporal;
    g_debugView = debugView;
    g_jitterAsShipped = jitterAsShipped;
    refreshLive();
    // The surfaces half follows the same key: one setting, both mechanisms.
    uiSurfacesSetTarget(target, text.c_str());
    if (!changed) return;
    g_keyNoted = true;
    if (!recognized) {
        Log::get().note("ui quality: fix.ui_quality = '%s' is not off, 1.0 or 1.25 -- off.",
                        text.c_str());
        return;
    }
    if (target <= 0.0f) {
        Log::get().note("ui quality: off -- the game's UI surfaces and composites are drawn as "
                        "they always were.");
        return;
    }
    float hmd = 0.0f;
    const bool hmdKnown = deviceHookHmdQuality(&hmd) && hmd > 0.0f;
    char hmdText[64] = "unknown";
    if (hmdKnown) std::snprintf(hmdText, sizeof(hmdText), "%.2f", static_cast<double>(hmd));
    Log::get().note(
        "ui quality: %s (HMD Quality %s) -- surfaces: every offscreen UI surface whose size is a "
        "known fraction of the internal render resolution is created at the size it would have "
        "at HMD Quality %s; layer: %s",
        text.c_str(), hmdText, text.c_str(),
        !temporal ? "waits -- fix.temporal_aa is off, and the layer is composited at that "
                    "pass's door."
        : !jitterAsShipped
            ? "waits -- advanced.temporal_aa_jitter_sign or _lag is set, and the layer cancels "
              "the jitter by the shipped convention only."
            : "the game's post-tonemap UI (the 2D screen, the menus, the loading screen) is drawn "
              "by its own shaders into a per-eye layer at that size times the door's output, "
              "unjittered, and composited after the upscale and RCAS, before EDVR's menu; the "
              "cockpit's holo panels, flight HUD and target sprite are drawn before the tonemap "
              "and stay in the picture (advanced.ui_replay says whether the deferred UI replay "
              "redraws them). Draws the layer takes get no UI depth and no reactive mask.");
    if (debugView && temporal) {
        Log::get().note("ui quality: advanced.temporal_aa_debug = ui_layer -- the layer is shown "
                        "over black.");
    }
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

bool uiLayerDecide(ID3D11DeviceContext* ctx, int familyInt, bool verdictForwards,
                   bool substituted) {
    g_draw.decided = false;
    g_draw.counted = false;
    g_draw.ds = UiDsEffect{};
    g_draw.stencilRead = 0;
    g_draw.shape = UiBlendShape::kRefused;
    if (!ctx || familyInt <= 0 || familyInt >= static_cast<int>(UiLayerFamily::kCount)) return false;
    const UiLayerFamily family = static_cast<UiLayerFamily>(familyInt);
    UiLayerDrawFacts f;
    f.family = family;
    f.verdictForwards = verdictForwards;
    f.substituted = substituted;
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
            // The map sends the whole target onto the layer: only right when
            // the target IS the region the game submits for that eye.
            f.targetMatchesEye = !sw || !sh || (sw == g_tc.info.a && sh == g_tc.info.b);
            f.late = uiLayerLateFor(g_eye[f.eye].door, seq);
            f.armed = uiLayerArmed(g_eye[f.eye].door, seq);
        }
    }
    // The flag is only ever set, and consumed, while the replay may act.
    f.replayOwns = uiDeferredMayAct() && uiDeferredRouteCapturedThisDraw();
    // The cheap facts first; the state reads only when none of them refused.
    f.blend = UiBlendShape::kOpaque;
    UiLayerDecision d = uiLayerDecide(f);
    char detail[320] = "";
    UiDsState dsState;
    if (d == UiLayerDecision::kRedirect) {
        // A second render target, or pixel-shader UAVs (which the layer's
        // raw OMSetRenderTargets would not carry across), refuse the draw.
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs,
                                                       &dsv, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
        bool anyUav = false;
        for (auto* u : uavs) {
            if (u) {
                anyUav = true;
                u->Release();
            }
        }
        f.mrt = boundCount(rtvs) > 1 || anyUav;
        UINT ref = 0;
        DXGI_FORMAT dsFmt = DXGI_FORMAT_UNKNOWN;
        dsState = dsStateOf(ctx, dsv, &ref, &dsFmt);
        f.ds = uiLayerDsEffect(dsState, dsv != nullptr);
        char dsWhy[160] = "";
        if (f.ds.tests()) {
            f.dsReproducible = dsReproducible(ctx, dsv, g_tc.info.a, g_tc.info.b, dsWhy, sizeof(dsWhy));
        }
        ID3D11BlendState* bs = nullptr;
        FLOAT factor[4];
        UINT mask = 0;
        ctx->OMGetBlendState(&bs, factor, &mask);
        UiBlendRt game;
        f.blend = shapeOf(bs, &game);
        if (bs) bs->Release();
        // A multiply is drawn twice (the layer, then its transmittance): a
        // depth or stencil WRITE would then land twice in the layer's copy.
        const bool multiplyWrites = f.blend == UiBlendShape::kMultiply && f.ds.writes();
        if (multiplyWrites) f.blend = UiBlendShape::kRefused;
        d = uiLayerDecide(f);
        if (d == UiLayerDecision::kRedirect) {
            f.layerReady = ensureLayerFor(ctx, f.eye) &&
                           (f.blend != UiBlendShape::kMultiply || ensureMult(ctx, g_eye[f.eye], f.eye));
            d = uiLayerDecide(f);
        }
        // What decided it, in the census's own numbers, for the first line.
        char bl[64], ds[256];
        describeBlend(game, bl, sizeof(bl));
        describeDs(dsState, ref, dsFmt, ds, sizeof(ds));
        if (d == UiLayerDecision::kBlendRefused) {
            _snprintf_s(detail, _TRUNCATE, "%s%s", bl,
                        multiplyWrites ? " (a multiply that writes depth or stencil)" : "");
        } else if (d == UiLayerDecision::kDepthStencilTest ||
                   d == UiLayerDecision::kSubstitutedWrite) {
            _snprintf_s(detail, _TRUNCATE, "%s%s%s", ds, *dsWhy ? "; " : "", dsWhy);
        } else if (d == UiLayerDecision::kRedirect) {
            _snprintf_s(detail, _TRUNCATE, "%s%s%s%s", uiBlendShapeName(f.blend),
                        f.ds.tests() ? "; tests against the layer's seeded copy of the game's "
                                       "depth-stencil"
                                     : "",
                        f.ds.writes() ? "; its depth/stencil write kept in the game's buffer by a "
                                        "colourless re-issue"
                                      : "",
                        (f.ds.tests() || f.ds.writes()) ? (std::string(" (") + ds + ")").c_str()
                                                        : "");
        }
        for (auto* r : rtvs)
            if (r) r->Release();
        if (dsv) dsv->Release();
    }
    ++g_win.decided[static_cast<size_t>(family)][static_cast<size_t>(d)];
    noteFamily(family, d, detail);
    if (d != UiLayerDecision::kRedirect) return false;
    g_draw.decided = true;
    g_draw.eye = f.eye;
    g_draw.family = family;
    g_draw.seq = seq;
    g_draw.targetRes = g_tc.info.resource;
    g_draw.targetW = g_tc.info.a;
    g_draw.targetH = g_tc.info.b;
    g_draw.shape = f.blend;
    g_draw.ds = f.ds;
    g_draw.stencilRead = dsState.readMask;
    // The pass computed the jitter against the eye's submitted region, which
    // is the target (targetMatchesEye above).
    g_draw.jx = jx;
    g_draw.jy = jy;
    return true;
}

bool uiLayerBegin(ID3D11DeviceContext* ctx) { return beginGuarded(ctx, 0); }

void uiLayerEnd(ID3D11DeviceContext* ctx) {
    if (!g_draw.active) return;
    if (!guarded("uiLayer.end", [&] { restore(ctx); })) {
        standDown("a fault while putting the game's state back after a draw");
    }
    releaseSaved();
    g_draw.active = false;
    detail::g_uiLayerRedirecting = false;
}

bool uiLayerMultiplyBegin(ID3D11DeviceContext* ctx) {
    if (!g_draw.decided || g_draw.shape != UiBlendShape::kMultiply) return false;
    return beginGuarded(ctx, 1);
}

bool uiLayerWriteBackBegin(ID3D11DeviceContext* ctx) {
    if (!g_draw.decided || !g_draw.counted || !g_draw.ds.writes() || g_draw.active || !ctx)
        return false;
    bool ok = false;
    guarded("uiLayer.writeBack", [&] {
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_draw.wbRtv, &g_draw.wbDsv);
        if (!g_draw.wbDsv) return;
        // The game's own depth target and state, no colour target: the draw's
        // depth or stencil write lands in the game's buffer exactly as it
        // always did, and its colour lands nowhere (it is in the layer).
        vScreenSetRenderTargetsRaw(ctx, 0, nullptr, g_draw.wbDsv);
        ok = true;
    });
    if (!ok) {
        for (auto*& r : g_draw.wbRtv) {
            if (r) r->Release();
            r = nullptr;
        }
        if (g_draw.wbDsv) g_draw.wbDsv->Release();
        g_draw.wbDsv = nullptr;
        return false;
    }
    g_draw.wbActive = true;
    ++g_win.writeBacks;
    return true;
}

void uiLayerWriteBackEnd(ID3D11DeviceContext* ctx) {
    if (!g_draw.wbActive) return;
    guarded("uiLayer.writeBackEnd", [&] {
        vScreenSetRenderTargetsRaw(ctx, boundCount(g_draw.wbRtv), g_draw.wbRtv, g_draw.wbDsv);
    });
    for (auto*& r : g_draw.wbRtv) {
        if (r) r->Release();
        r = nullptr;
    }
    if (g_draw.wbDsv) g_draw.wbDsv->Release();
    g_draw.wbDsv = nullptr;
    g_draw.wbActive = false;
}

void uiLayerNoteOther(ID3D11DeviceContext* ctx, uint32_t count) {
    if (!ctx) return;
    const void* taken[2] = {nullptr, nullptr};
    for (int e = 0; e < 2; ++e) {
        if (g_eye[e].seq == g_lastRedirectSeq && g_eye[e].draws) taken[e] = g_eye[e].target;
    }
    // A game draw that writes the depth target a seed copied, after the seed:
    // the layer's copy is stale, and the next tested draw seeds again.
    for (Eye& e : g_eye) {
        if (e.dsSeq != g_lastRedirectSeq || !e.dsSource) continue;
        void* dsvView = bindingGet(BindSlot::Dsv0);
        ResourceInfo info;
        if (!dsvView || !bindingResolve(dsvView, &info) || info.resource != e.dsSource) continue;
        ID3D11DepthStencilView* dsv = static_cast<ID3D11DepthStencilView*>(dsvView);
        if (uiLayerDsEffect(dsStateOf(ctx, dsv, nullptr, nullptr), true).writes()) {
            e.dsSeq = 0;
            ++g_win.seedStale;
        }
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
        "ui quality: layer: after the UI, the game %s an eye target the UI was taken from: vs "
        "%016llX ps %016llX -- with the layer, that %s.",
        kind == 'W' ? "drew into" : "read", static_cast<unsigned long long>(vs),
        static_cast<unsigned long long>(ps),
        kind == 'W' ? "lands under the UI now instead of over it"
                    : "no longer sees the UI in what it reads (a post pass over the eye: the UI "
                      "misses it)");
}

void uiLayerNoteDepthClear(void* dsv) {
    if (!dsv) return;
    bool any = false;
    for (const Eye& e : g_eye) any = any || (e.dsSeq && e.dsSource);
    if (!any) return;
    ResourceInfo info;
    if (!bindingResolve(dsv, &info) || !info.resource) return;
    for (Eye& e : g_eye) {
        if (e.dsSeq && e.dsSource == info.resource) {
            e.dsSeq = 0;
            ++g_win.seedStale;
        }
    }
}

void uiLayerNoteTemporal(uint64_t sequence, uint32_t eye, const void* output) {
    if (eye > 1) return;
    Eye& e = g_eye[eye];
    e.door.treatedSeq = sequence;
    e.temporalOut = output;
    e.temporalOutSeq = sequence;
    ++g_win.treated;
}

void uiLayerDoorSeen(uint64_t sequence, uint32_t eye, ID3D11Texture2D* source) {
    if (eye > 1 || !source) return;
    Eye& e = g_eye[eye];
    e.door.doorSeq = sequence;
    e.doorFromPass = e.temporalOutSeq == sequence && e.temporalOut == source;
    ++g_win.doors;
    if (e.doorFromPass && detail::g_uiLayerLive) {
        // Whether a composite can run over this frame at all is settled
        // here, before anything is redirected, not at the composite with a
        // frame's UI already in the layer: an unarmed layer loses nothing.
        if (!doorCanComposite(source)) {
            e.door.fullW = e.door.fullH = 0;
            return;
        }
        D3D11_TEXTURE2D_DESC d{};
        source->GetDesc(&d);
        if (d.Width != e.door.fullW || d.Height != e.door.fullH) {
            if (g_target > 0.0f) {
                const UiLayerSize s = uiLayerSize(d.Width, d.Height, g_target);
                Log::get().note(
                    "ui quality: layer: the %s eye's door hands on %ux%u; its layer is %ux%u at %s "
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
                                  const uint32_t region[4], const float layerUv[4]) {
    if (eye > 1 || !frame || !region || !layerUv) return nullptr;
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
            // The game's Submit says this UI belongs to the other eye: every
            // frame from here would invert the disparity of every menu. Stand
            // down at once -- the UI goes back into the game's frame, in the
            // right eyes -- and say how to flip the order rule.
            ++g_win.eyeSwapped;
            Log::get().note(
                "ui quality: layer: the %s eye's layer holds UI from the target the game "
                "submitted for the %s eye -- the eye rule (first target of a frame = left) is "
                "backwards on this rig. advanced.ui_depth_eyes = swapped flips it (and the UI "
                "depth's with it); then turn fix.ui_quality off and on.",
                eye == 0 ? "left" : "right", eye == 0 ? "right" : "left");
            standDown("the eye check found the layer's eyes swapped");
            return nullptr;
        } else {
            ++g_win.eyeUntold;
        }
    }
    if (graphicsRuntimeDisabled()) return nullptr;
    const bool useMult = hasUi && e.mSrv && e.mSeq == sequence;
    ID3D11Texture2D* result = nullptr;
    const char* why = nullptr;
    const bool ran = guardedBudget(g_compositeBudget, [&] {
        result = compositeInner(e, eye, frame, region, layerUv, useMult, &why);
    });
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
                Log::get().note("ui quality: layer: the debug view's empty frame was not "
                                "composited -- %s.",
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
    if (ctx && detail::g_uiLayerLive) compileOnce(ctx);
    // Not live -- off, no pass, the jitter switches set, stood down: this
    // frame's doors have run, so nothing still needs the layers. Let the
    // memory go; the next live frame makes them again.
    if (!detail::g_uiLayerLive && releaseLayers()) {
        Log::get().note("ui quality: layer: not live -- both eyes' layers and composite outputs "
                        "released.");
    }
    uiSurfacesFrameBoundary();
    const uint64_t now = GetTickCount64();
    if (!g_winStartMs) g_winStartMs = now;
    if (now - g_winStartMs < kTotalsMs) return;
    const bool anything = g_win.redirected || g_win.composites || g_win.compositeRefused;
    if (g_target > 0.0f || anything) {
        logTotals(static_cast<double>(now - g_winStartMs) / 1000.0);
        if (!g_timingNoted && g_win.timed) {
            g_timingNoted = true;
            Log::get().note("ui quality: layer: the composite measured %.3f ms per eye on average "
                            "over its first %u timed composites (RCAS, the step before it, "
                            "measured 0.23 ms an eye at 5792x5356).",
                            g_win.timeSum / g_win.timed, g_win.timed);
        }
    }
    g_win = Window{};
    g_winStartMs = now;
}

void uiLayerShutdown() {
    if (g_draw.active) releaseSaved();
    g_draw = Draw{};
    releaseLayers();
    for (QuerySlot& q : g_qring) {
        q.timer.reset();
        q.inUse = false;
    }
    for (uint32_t i = 0; i < g_blendCount; ++i) g_blends[i] = BlendEntry{};
    g_blendCount = 0;
    g_seeder.reset();
    g_deferred.Reset();
    g_seederTried = false;
    if (g_cb) {
        g_cb->Release();
        g_cb = nullptr;
    }
    if (g_cs) {
        g_cs->Release();
        g_cs = nullptr;
    }
    if (g_sessionRedirected) {
        Log::get().note("ui quality: layer: %llu draws redirected this session.",
                        static_cast<unsigned long long>(g_sessionRedirected));
    }
}

}  // namespace edvr
