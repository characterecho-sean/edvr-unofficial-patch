// metal_fx_engine.cpp -- see metal_fx_engine.h for what this is and why it
// exists. All of it is the DXMT extension interface: a QueryInterface, one
// capability query, and one call. There is no Metal here.
#include "metal_fx_engine.h"

#include <d3d11.h>

#include <cstring>

#include "../common/log.h"

namespace edvr {
namespace {

// The extension pointer, per device context. Queried once and cached; the
// answer cannot change while the process lives, because it is a property of
// the driver behind the context and the device is not recreated under us. A
// different context (a recreated device) drops the old reference here, from
// the owner thread. A refusal is cached too -- it is a property of the
// driver, not of the frame -- and `have` is what says the cached reference is
// usable, never the pointer alone.
struct Cached {
    ID3D11DeviceContext *context = nullptr;
    void *ext = nullptr;          // mfx::ContextExt1*, an AddRef'd reference
    bool queried = false;
    bool have = false;            // ext is a usable ContextExt1, not just a reference
    const char *reason = "";
};

// Renderer state: one per owner thread, released by flatMonoResolveReset or at
// the end of a session from the owner thread. Never a static destructor --
// releasing a COM reference under the DLL loader lock is how this file's kind
// of code crashes a process that is already on its way out (the same rule the
// resolver's own State, flat_mono_resolve.cpp:100, states).
Cached &cached() { return *new Cached; }

// Said once a session, so a refusal is visible without a flight's log being a
// wall of the same line.
bool g_saidInterface = false, g_saidSupported = false;
constexpr uint32_t kLogCap = 4;
uint32_t g_unsupportedLogged = 0;

void sayUnavailable(const char *what, const char *reason) {
    if (g_unsupportedLogged < kLogCap && reason && *reason) {
        ++g_unsupportedLogged;
        Log::get().note("MFX: %s: %s", what, reason);
    }
}

// Said once a session, so the dispatch proof below is logged once.
bool g_saidGeneration = false;

// The one-time query. Returns the cached extension or null, and sets *why on
// every path that did not get one. A failed QI is the load-bearing refusal:
// it is how a non-DXMT D3D11 (WARP, NVIDIA, AMD, the game's own) says no, and
// it is a normal outcome here, not an error.
mfx::ContextExt1 *extension(ID3D11DeviceContext *ctx, const char **why) {
    if (why) *why = nullptr;
    if (!ctx) {
        if (why) *why = "flat-mfx-no-context";
        sayUnavailable("no immediate context", *why);
        return nullptr;
    }
    Cached &c = cached();
    if (c.queried && c.context == ctx) {
        if (!c.have && why) *why = c.reason;
        return c.have ? static_cast<mfx::ContextExt1 *>(c.ext) : nullptr;
    }
    // A different context (a device recreated, or a second one): drop the old
    // references from the owner thread and ask again.
    if (c.ext) {
        static_cast<mfx::ContextExt1 *>(c.ext)->Release();
        c.ext = nullptr;
    }
    c = Cached{};
    c.context = ctx;
    c.queried = true;

    mfx::ContextExt *base = nullptr;
    // REFIID is `const IID&` and IID is GUID, so the constant binds directly:
    // no cast, and nothing to get wrong about the cast.
    if (FAILED(ctx->QueryInterface(mfx::kIidContextExt, reinterpret_cast<void **>(&base))) || !base) {
        c.reason = "flat-mfx-no-dxmt-context-ext";
        if (why) *why = c.reason;
        if (!g_saidInterface) {
            g_saidInterface = true;
            Log::get().note("MFX: DXMT temporal interface unavailable (%s); fix.temporal_aa = mfx is inert on this "
                            "device. MetalFX is reached through DXMT's own interface, not a vendor extension.",
                            c.reason);
        }
        sayUnavailable("no DXMT temporal interface", c.reason);
        return nullptr;
    }
    mfx::ContextExt1 *ext1 = nullptr;
    if (FAILED(base->QueryInterface(mfx::kIidContextExt1, reinterpret_cast<void **>(&ext1))) || !ext1) {
        base->Release();
        c.reason = "flat-mfx-no-dxmt-context-ext1";
        if (why) *why = c.reason;
        if (!g_saidInterface) {
            g_saidInterface = true;
            Log::get().note("MFX: DXMT temporal interface available, the feature-query interface is not (%s)",
                            c.reason);
        }
        sayUnavailable("no DXMT feature-query interface", c.reason);
        return nullptr;
    }
    base->Release();
    c.ext = ext1;
    if (!g_saidInterface) {
        g_saidInterface = true;
        Log::get().note("MFX: DXMT temporal interface available (IMTLD3D11ContextExt1)");
    }

    // The one real capability question. DXMT answers it from the Metal device
    // ([MTLFXTemporalScalerDescriptor supportsDevice:] behind
    // MTL_FEATURE_METALFX_TEMPORAL_SCALER), not from a device name or a
    // registry key, so this is a real answer on this GPU.
    BOOL supported = FALSE;
    const HRESULT hr = ext1->CheckFeatureSupport(mfx::Feature::MetalFxTemporalScaler, &supported,
                                                 sizeof(supported));
    if (FAILED(hr)) {
        c.reason = "flat-mfx-check-feature-support-failed";
        if (why) *why = c.reason;
        sayUnavailable("DXMT's feature query failed", c.reason);
        return nullptr;
    }
    if (!supported) {
        c.have = false;
        c.reason = "flat-mfx-unsupported";
        if (why) *why = c.reason;
        if (!g_saidSupported) {
            g_saidSupported = true;
            Log::get().note("MFX: DXMT temporal interface available, MetalFX temporal scaler unsupported");
        }
        sayUnavailable("MetalFX temporal scaler unsupported", c.reason);
        return nullptr;
    }
    c.have = true;
    c.reason = "";
    if (!g_saidSupported) {
        g_saidSupported = true;
        Log::get().note("MFX: DXMT temporal interface available, MetalFX temporal scaler supported");
    }
    return ext1;
}

} // namespace

bool mfxAvailable(ID3D11DeviceContext *ctx, const char **why) {
    if (why) *why = nullptr;
    return extension(ctx, why) != nullptr;
}

bool mfxEvaluate(ID3D11DeviceContext *ctx, ID3D11Texture2D *colour, ID3D11Texture2D *depth,
                 ID3D11Texture2D *mv, ID3D11Texture2D *out, uint32_t w, uint32_t h, uint32_t outW,
                 uint32_t outH, float jx, float jy, bool reset, const char **why, bool autoExposure) {
    if (why) *why = nullptr;
    // Every one of these is refused before the interface is asked, so a
    // malformed frame can never become a partially filled descriptor crossing
    // into DXMT.
    if (!colour || !depth || !mv || !out) {
        if (why) *why = "flat-mfx-missing-resource";
        sayUnavailable("a required resource was null", *why);
        return false;
    }
    if (!w || !h || !outW || !outH) {
        if (why) *why = "flat-mfx-invalid-extent";
        return false;
    }
    // The capability gate runs BEFORE the descriptor is built, so a refused
    // request never becomes a descriptor crossing into DXMT.
    mfx::ContextExt1 *ext = extension(ctx, why);
    if (!ext) return false;

    mfx::TemporalUpscaleDesc desc{};
    // The whole render-size texture is the input content. 0 would mean "full
    // width" to DXMT, which is the same region, but naming it keeps the
    // descriptor honest about the route and leaves the "0" path untested here.
    desc.InputContentWidth = w;
    desc.InputContentHeight = h;
    // The frontend's own reset, not a MetalFX-specific one: the same value
    // fsr3Evaluate and dlaaEvaluate are handed on this frame.
    desc.InReset = reset ? TRUE : FALSE;
    // EDVR's depth is reversed-Z, d = near/z, saturated to [0,1] by the prep
    // kernel. Zero is the near plane, so MetalFX must be told zero is far.
    desc.DepthReversed = TRUE;
    // EDVR's motion vectors are at the render size, as MetalFX expects by
    // default. dxmt's own MV-downscale kernel (MTLFXMVScaleContext) would
    // instead run and rescale, which is the wrong path for these vectors.
    desc.MotionVectorInDisplayRes = FALSE;
    desc.Color = colour;
    desc.Depth = depth;
    desc.MotionVector = mv;
    desc.Output = out;
    // Render pixels, current -> previous, +Y down, both raster phases already
    // removed by the prep kernel (flat_mono_shader_source.h:194). MetalFX
    // converts to pixels by multiplying by these, so 1 is the identity and any
    // other value would rescale vectors that are already in the units it wants.
    desc.MotionVectorScaleX = 1.0f;
    desc.MotionVectorScaleY = 1.0f;
    // EDVR holds no exposure texture (as on the FSR and NGX HDR routes), so
    // the scaler computes exposure itself and ignores an exposure texture
    // entirely -- which is why the field is left null rather than bound to a
    // constant. Not to be confused with the HD route's radiance scale; nothing
    // here is a tonemap.
    desc.PreExposure = 1.0f;
    desc.ExposureTexture = nullptr;
    desc.AutoExposure = autoExposure ? TRUE : FALSE;
    desc.JitterOffsetX = mfxJitterOffsetX(jx);
    desc.JitterOffsetY = mfxJitterOffsetY(jy);

    // DXMT closes the current pass, records the call in this chunk's encoder
    // list between EDVR's prep dispatch and its finish dispatch, and encodes
    // MetalFX into the same MTLCommandBuffer with its own hazard and
    // residency tracking (its TemporalUpscale, dxmt_context.cpp:5302 and
    // :590). Ordering is its job; EDVR has nothing to do but be in order
    // itself, which it is. No command buffer, no commit, no wait, no fence,
    // no copy: nothing below the call touches the device.
    //
    // Said once a session, next to the dispatch and not at initialisation,
    // so it proves a scaler call actually went out this run rather than
    // only that the capability check passed.
    if (!g_saidGeneration) {
        g_saidGeneration = true;
        Log::get().note("MFX: dispatching TemporalUpscale, MetalFX generation = 3");
    }
    ext->TemporalUpscale(&desc);
    return true;
}

} // namespace edvr