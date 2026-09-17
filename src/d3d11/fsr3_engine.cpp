#include "fsr3_engine.h"

#include <cstring>
#include <vector>

#include <windows.h>
#include <d3d11.h>

#include "../common/config.h"
#include "../common/log.h"
#include "perf_monitor.h"   // the context's creation is an event with a duration
#include "gpu_timing.h"
#include "gpu_adapter_name.h"  // adapterName -- shared with dlaa.cpp

// Bare `/D EDVR_HAVE_FSR3` (no value, matching how a future build.bat block
// might be typed beside NGX's `/DEDVR_HAVE_NGX=1`, build.bat:300) would make
// `#if EDVR_HAVE_FSR3` below a syntax error rather than a clean 0. Pin it.
#ifndef EDVR_HAVE_FSR3
#define EDVR_HAVE_FSR3 0
#endif

#if EDVR_HAVE_FSR3
// AMD's FidelityFX Super Resolution 3.1, the community Direct3D 11 port
// (metarutaiga, hardened by OptiScaler; MIT; design doc section 2), fetched
// into third_party\ffx-dx11\ (or %LOCALAPPDATA%\EDVR\ffx-dx11\) by
// tools\fetch_ffx_dx11.py and linked only when build.bat's own
// EDVR_HAVE_FSR3 block found it. Upscaler-only entry points: this header
// alone (it pulls in ffx_interface.h/ffx_types.h/ffx_error.h itself), not
// the combined ffxFsr3Context wrapper.
#include <dxgi1_4.h>  // IDXGIAdapter3::QueryVideoMemoryInfo, the create's VRAM line
#include <FidelityFX/host/ffx_fsr3upscaler.h>

// backends\dx11\ffx_dx11.h is deliberately NOT included. Its declaration of
// ffxGetResourceDX11_Fsr31 is extern "C" with a `const ID3D11Resource*`
// first parameter; the port's own ffx_dx11.cpp DEFINES it with a non-const
// `ID3D11Resource*` outside any extern "C" block (confirmed by reading
// ffx_dx11.cpp directly). The header's declared symbol is therefore never
// defined, and calling it as the header declares it fails to link
// (LNK2019). Worked around here: the six functions whose header
// declaration and .cpp definition agree are re-declared verbatim (still
// extern "C", still matching ffx_dx11.h byte for byte), and the seventh --
// ffxGetResourceDX11_Fsr31 -- is declared separately to match its REAL
// (non-const, ordinary C++-linkage, free-function) definition instead.
// Declaring both forms in one translation unit would itself be a linkage
// conflict, which is exactly why ffx_dx11.h cannot simply be included
// alongside this.
extern "C" {
FFX_API size_t ffxGetScratchMemorySizeDX11(size_t maxContexts);
FFX_API FfxDevice ffxGetDeviceDX11_Fsr31(ID3D11Device* device);
FFX_API FfxErrorCode ffxGetInterfaceDX11(FfxInterface* backendInterface, FfxDevice device,
                                         void* scratchBuffer, size_t scratchBufferSize,
                                         uint32_t maxContexts);
FFX_API FfxCommandList ffxGetCommandListDX11(ID3D11DeviceContext* deviceContext);
FFX_API FfxSurfaceFormat ffxGetSurfaceFormatDX11(DXGI_FORMAT format);
FFX_API FfxResourceDescription GetFfxResourceDescriptionDX11(ID3D11Resource* pResource);
}  // extern "C"

// Not extern "C": see the comment above. Matches ffx_dx11.cpp's real
// signature (dx11Resource non-const, no default argument needed here since
// every call site below passes all four arguments explicitly).
FfxResource ffxGetResourceDX11_Fsr31(ID3D11Resource* dx11Resource,
                                     FfxResourceDescription ffxResDescription,
                                     wchar_t const* ffxResName, FfxResourceStates state);
#endif  // EDVR_HAVE_FSR3

namespace edvr {
namespace {

// Declared unconditionally, like dlaa.cpp's own pooled totals (dlaa.cpp:32-
// 41): the accessors below compile with no AMD SDK in the build too, and
// just never see a count, so fsr3Totals answers "nothing has run" honestly
// rather than needing its own stub half.
bool        g_tried = false;
bool        g_available = false;
const char* g_reason = "not asked yet";

uint32_t g_evaluations = 0;
uint32_t g_resets = 0;        // evaluations that restarted AMD's history
uint32_t g_timeCount = 0;
double   g_timeSum = 0.0;
double   g_timeMax = 0.0;

char g_reasonBuf[256];

#if EDVR_HAVE_FSR3

// Both axes, jitter and motion vectors: the identity, and MEASURED to be
// the identity by tools\fsr3_engine_test's registration table (test (d),
// design doc 3.5 item 3), which runs all sixteen combinations of jitter
// sign {as_is, flip_x, flip_y, flip_both} x motion-vector sign {as_is,
// flip_x, flip_y, flip_both} over a translating scene whose motion is known
// exactly at every pixel. All four identities won, and nothing else came
// within twice the scene's own at-rest floor. It agrees with the port's own
// source: its reprojection samples the history at uv + mv (ffx_fsr3-
// upscaler_prepare_inputs.h:32, ffx_fsr3upscaler_reproject.h:58), so the
// vector points current -> previous, which is what dlaa.h's dlMv carries
// and what NVIDIA is given; and its upsample takes the un-jittered position
// as (pixel + 0.5) - Jitter() (ffx_fsr3upscaler_upsample.h:307), so the
// content sits at +jitter, which is dlaa.h's convention too. The scale of 1
// is right because the port divides motionVectorScale by the render size
// itself (ffx_fsr3upscaler.cpp:990-991), so a vector in RENDER PIXELS
// scaled by 1 is exactly what its shaders want.
constexpr float kFsrJitterScaleX = 1.0f;
constexpr float kFsrJitterScaleY = 1.0f;
constexpr float kFsrMotionVectorScaleX = 1.0f;
constexpr float kFsrMotionVectorScaleY = 1.0f;

// A documented fallback for cameraFovAngleVertical when the caller has not
// computed real tangents yet (fovY == 0: temporal_pass.cpp passes 0.0f
// outside the AMD branch, and could in principle be asked before the first
// eye's tangents are known). 90 degrees is not measured from Elite; it is
// only ever used for the frame or two before a real value is available.
constexpr float kFsrDefaultFovYRadians = 1.5707963f;  // 90 degrees

// Documented default near/far planes (design doc, Track A's facts): used
// only when nearZ/farZ are the "no depth yet" sentinel (temporal_pass.h:
// 0 = unknown). These are the planes the game has been seen to use.
constexpr float kFsrDefaultNearZ = 0.025f;
constexpr float kFsrDefaultFarZ = 50000.0f;

ID3D11Device* g_device = nullptr;
FfxInterface  g_backend{};
// Scratch for two contexts (ffxGetScratchMemorySizeDX11(2)), owned for the
// session once fsr3Available succeeds. The backend interface captures a
// pointer into this buffer at ffxGetInterfaceDX11 time, so it must not move
// or be freed while any context could still be live -- it is only ever
// grown once (in fsr3Available) and released in fsr3Shutdown, after
// fsr3ReleaseFeatures has destroyed every context.
std::vector<uint8_t> g_scratch;

struct EyeCtx {
    FfxFsr3UpscalerContext ctx{};
    bool     valid = false;
    uint32_t w = 0, h = 0, outW = 0, outH = 0;
    // FSR 3.1 moved three of the upscaler's own working surfaces out of the
    // context and into the dispatch description, so that a host running
    // frame interpolation can share them with it: dilatedDepth,
    // dilatedMotionVectors and reconstructedPrevNearestDepth are allocated
    // by the CALLER to the port's own recipe (ffxFsr3UpscalerGetShared-
    // ResourceDescriptions, asked below rather than hard-coded). They are
    // not optional. A null one registers as the port's NULL resource
    // (RegisterResourceDX11 maps a null pointer to internal index 0), and
    // then the dilate pass writes its motion vectors nowhere and the
    // reprojection reads zero back: the upscaler still accumulates, it just
    // never sees any motion at all. That is exactly what
    // tools\fsr3_engine_test's (d0) probe measured before these existed --
    // a uniform +40 px motion vector produced byte-identical output to a
    // zero one. EDVR does not run frame interpolation, so these are private
    // scratch, one set per eye, keyed and destroyed with the context.
    ID3D11Texture2D* dilatedDepth = nullptr;
    ID3D11Texture2D* dilatedMv = nullptr;
    ID3D11Texture2D* prevNearestDepth = nullptr;
};
// One per eye, keyed on (w, h, outW, outH) exactly as dlaa.cpp's
// ensureFeature keys NGX (design doc 3.2): recreate on a key change,
// destroying the old context first.
EyeCtx g_ctx[2];

// The GPU-price ring, dlaa.cpp's own discipline (QuerySlot/pollTimingRing/
// acquireQuerySlot there): never awaited. FSR has one role (unlike DLAA's
// full/centre/periphery split), so the slot only needs to remember the eye.
struct QuerySlot {
    GpuTimer timer;
    bool     inUse = false;
    int      eye = 0;
};
constexpr int kQueryRing = 8;
QuerySlot g_qring[kQueryRing];

void releaseQuerySlot(QuerySlot& q) {
    q.timer.reset();
    q.inUse = false;
}

void pollTimingRing(ID3D11DeviceContext* ctx) {
    if (!gpuTimingOwns(ctx)) return;
    for (QuerySlot& q : g_qring) {
        if (!q.inUse) continue;
        double ms = 0.0;
        const GpuTimerPoll result = q.timer.poll(ctx, ms);
        if (result == GpuTimerPoll::Pending) continue;
        q.inUse = false;
        if (result != GpuTimerPoll::Ready) continue;
        ++g_timeCount;
        g_timeSum += ms;
        if (ms > g_timeMax) g_timeMax = ms;
    }
}

int acquireQuerySlot(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!ctx || !dev) return -1;
    if (!gpuTimingAccepts(ctx) && !gpuTimingBind(dev, ctx)) return -1;
    for (int i = 0; i < kQueryRing; ++i) {
        QuerySlot& q = g_qring[i];
        if (q.inUse) continue;
        if (q.timer.begin(dev, ctx)) { q.inUse = true; return i; }
        return -1;  // Shared clock pressure cannot be fixed by trying another free slot.
    }
    return -1;
}

const char* ffxErrorName(FfxErrorCode e) {
    switch (e) {
        case FFX_OK:                           return "success";
        case FFX_ERROR_INVALID_POINTER:        return "an invalid pointer";
        case FFX_ERROR_INVALID_ALIGNMENT:      return "an invalid alignment";
        case FFX_ERROR_INVALID_SIZE:           return "an invalid size";
        case FFX_ERROR_INVALID_PATH:           return "an invalid path";
        case FFX_ERROR_EOF:                    return "an unexpected end of file";
        case FFX_ERROR_MALFORMED_DATA:         return "malformed data";
        case FFX_ERROR_OUT_OF_MEMORY:          return "out of memory";
        case FFX_ERROR_INCOMPLETE_INTERFACE:   return "an incomplete backend interface";
        case FFX_ERROR_INVALID_ENUM:           return "an invalid enum value";
        case FFX_ERROR_INVALID_ARGUMENT:       return "an invalid argument";
        case FFX_ERROR_OUT_OF_RANGE:           return "a value out of range";
        case FFX_ERROR_NULL_DEVICE:            return "a null device";
        case FFX_ERROR_BACKEND_API_ERROR:      return "a backend API error";
        case FFX_ERROR_INSUFFICIENT_MEMORY:    return "insufficient memory";
        default:                               return "an unrecognised FFX error";
    }
}

// fpMessage's target: relayed into the gfx log verbatim, capped so a
// misbehaving build cannot fill the log with per-frame DEBUG_CHECKING
// noise. g_msgCount is also read by fsr3TestMessageCount (below, test-only)
// so tools\fsr3_engine_test can prove a dispatch stayed silent without
// scraping the log file.
uint32_t g_msgCount = 0;
constexpr uint32_t kFsrMsgCap = 20;

void FsrMessage(FfxMsgType type, const wchar_t* message) {
    (void)type;
    ++g_msgCount;
    if (g_msgCount > kFsrMsgCap) {
        if (g_msgCount == kFsrMsgCap + 1) {
            Log::get().note("fsr3: further FSR messages suppressed.");
        }
        return;
    }
    char narrow[256];
    WideCharToMultiByte(CP_UTF8, 0, message ? message : L"", -1, narrow, sizeof(narrow), nullptr,
                        nullptr);
    Log::get().note("fsr3: %s", narrow);
}

// The adapter's current LOCAL video memory usage, for the create's VRAM
// delta line: IDXGIAdapter3::QueryVideoMemoryInfo, which WARP (a software
// rasteriser with no real budget) is not guaranteed to answer. A refusal is
// reported as such, not treated as an error (design doc 3.4, 3.5 item g).
bool queryVideoMemoryUsage(ID3D11Device* dev, uint64_t* bytes) {
    if (!dev || !bytes) return false;
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) ||
        !dxgiDev) {
        return false;
    }
    IDXGIAdapter* adapter = nullptr;
    const HRESULT ga = dxgiDev->GetAdapter(&adapter);
    dxgiDev->Release();
    if (FAILED(ga) || !adapter) return false;
    IDXGIAdapter3* adapter3 = nullptr;
    const HRESULT qi =
        adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3));
    adapter->Release();
    if (FAILED(qi) || !adapter3) return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    const HRESULT qv = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    adapter3->Release();
    if (FAILED(qv)) return false;
    *bytes = info.CurrentUsage;
    return true;
}

// The typed-UAV-load formats AMD's port keeps its internal history and
// dilated-motion targets in, read off ffx_fsr3upscaler.cpp's own internal
// resource table (fsr3upscalerCreateResources): R8_UNORM (masks/luma
// history), R16_FLOAT (luma/exposure history), R16G16_FLOAT (dilated
// motion vectors), R16G16B16A16_FLOAT (colour history). All four are
// OPTIONAL typed-UAV-load formats on D3D11 (unlike the mandatory R32 set),
// gated by D3D11_FEATURE_D3D11_OPTIONS2::TypedUAVLoadAdditionalFormats plus
// a per-format CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2) query.
struct FormatCheck { DXGI_FORMAT format; const char* name; };
constexpr FormatCheck kFsrUavFormats[] = {
    {DXGI_FORMAT_R8_UNORM, "R8_UNORM"},
    {DXGI_FORMAT_R16_FLOAT, "R16_FLOAT"},
    {DXGI_FORMAT_R16G16_FLOAT, "R16G16_FLOAT"},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT"},
};

// One of the three caller-owned working surfaces (EyeCtx above), made to
// the description AMD's port itself hands back rather than to numbers
// copied out of it: a port update that changes a format or a size is then
// a clean refusal with a reason, not a silently wrong surface.
bool makeSharedSurface(const FfxCreateResourceDescription& want, ID3D11Texture2D** out) {
    *out = nullptr;
    if (!g_device || want.resourceDescription.type != FFX_RESOURCE_TYPE_TEXTURE2D) return false;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    switch (want.resourceDescription.format) {
        case FFX_SURFACE_FORMAT_R32_FLOAT:    fmt = DXGI_FORMAT_R32_FLOAT; break;
        case FFX_SURFACE_FORMAT_R16G16_FLOAT: fmt = DXGI_FORMAT_R16G16_FLOAT; break;
        case FFX_SURFACE_FORMAT_R32_UINT:     fmt = DXGI_FORMAT_R32_UINT; break;
        default: return false;  // a format this port has never asked for before
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = want.resourceDescription.width;
    td.Height = want.resourceDescription.height;
    td.MipLevels = want.resourceDescription.mipCount ? want.resourceDescription.mipCount : 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    // SHADER_RESOURCE is not optional even for the one AMD's description
    // asks only a UAV for: RegisterResourceDX11 creates a shader-resource
    // view for EVERY registered texture unconditionally, and its TIF helper
    // turns a failed CreateShaderResourceView into a bare `throw 1`.
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (!td.Width || !td.Height) return false;
    return SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, out)) && *out != nullptr;
}

void releaseEyeSurfaces(EyeCtx& e) {
    if (e.dilatedDepth) { e.dilatedDepth->Release(); e.dilatedDepth = nullptr; }
    if (e.dilatedMv) { e.dilatedMv->Release(); e.dilatedMv = nullptr; }
    if (e.prevNearestDepth) { e.prevNearestDepth->Release(); e.prevNearestDepth = nullptr; }
}

// The per-eye context: made when missing or its key (the sizes) has moved,
// left alone otherwise. The ONE block fsr3Evaluate and fsr3Warm share, so
// what the warm-up makes on the loading screen is exactly what the first
// evaluation would have made (mirrors dlaa.cpp's ensureFeature).
bool ensureContext(unsigned eye, uint32_t w, uint32_t h, uint32_t outW, uint32_t outH,
                   const char** why, double* createMs) {
    if (createMs) *createMs = 0.0;
    if (eye > 1) {
        if (why) *why = "eye must be 0 or 1";
        return false;
    }
    EyeCtx& e = g_ctx[eye];
    if (e.valid && e.w == w && e.h == h && e.outW == outW && e.outH == outH) return true;
    if (e.valid) {
        ffxFsr3UpscalerContextDestroy(&e.ctx);
        releaseEyeSurfaces(e);
        e = EyeCtx{};
    }

    // advanced.temporal_aa_diagnostics is a context-creation-time flag (the
    // debug checks it enables run only inside ContextDispatch, but the flag
    // itself is baked in at create), so it is read here rather than through
    // Fsr3Settings/fsr3ReadConfig, which cover the two live per-DISPATCH
    // keys (advanced.temporal_aa_fsr_reactive, advanced.temporal_aa_fsr_debug).
    // NOT folded into the context key above: a live toggle only takes effect
    // on the next size-driven recreation, the same scope dlaa.cpp's own
    // preset generation gets an explicit bump for and this does not.
    const bool diagnostics = Config::get().getBool("advanced.temporal_aa_diagnostics", false);

    FfxFsr3UpscalerContextDescription desc{};
    desc.flags = FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
    if (diagnostics) desc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    desc.maxRenderSize = FfxDimensions2D{w, h};
    desc.maxUpscaleSize = FfxDimensions2D{outW, outH};
    desc.fpMessage = &FsrMessage;
    desc.backendInterface = g_backend;

    uint64_t vramBefore = 0;
    const bool haveVramBefore = queryVideoMemoryUsage(g_device, &vramBefore);

    const int64_t createT0 = qpcNow();
    // AMD's DX11 backend does not always fail through FfxErrorCode: its TIF
    // helper (ffx_dx11.cpp) answers a failed D3D11 call mid-create with a
    // bare `throw 1`, an untyped C++ exception with no place in this
    // otherwise all-FfxErrorCode C API. fsr3_engine.h promises false + why
    // on any refusal, never a crash, so that throw is caught here and
    // folded into the same reason path a real FfxErrorCode takes (found by
    // isolating a UAV-only output texture's failed CreateShaderResourceView
    // during Track C's own WARP rig; ffxFsr3UpscalerContextCreate's own
    // resource creation -- CreateBackendContextDX11 -- carries the same
    // TIF-guarded calls, so this is not only a dispatch-time risk).
    FfxErrorCode cr = FFX_ERROR_BACKEND_API_ERROR;
    bool threw = false;
    try {
        cr = ffxFsr3UpscalerContextCreate(&e.ctx, &desc);
    } catch (...) {
        threw = true;
    }
    const double ms = qpcFrequency() > 0
                          ? static_cast<double>(qpcNow() - createT0) * 1000.0 /
                                static_cast<double>(qpcFrequency())
                          : 0.0;
    perfMonitorNoteEvent(kEvFsr, ms);
    if (createMs) *createMs = ms;
    if (threw) {
        snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                 "AMD's context would not be created for eye %u at %ux%u -> %ux%u: a D3D11 call "
                 "inside AMD's port failed and its backend raised a C++ exception instead of an "
                 "FfxErrorCode",
                 eye, w, h, outW, outH);
        g_reason = g_reasonBuf;
        if (why) *why = g_reason;
        return false;
    }
    if (cr != FFX_OK) {
        snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                 "AMD's context would not be created for eye %u at %ux%u -> %ux%u: %s (0x%08X)",
                 eye, w, h, outW, outH, ffxErrorName(cr), static_cast<unsigned>(cr));
        g_reason = g_reasonBuf;
        if (why) *why = g_reason;
        return false;
    }
    // The three caller-owned working surfaces (EyeCtx). Made here, with the
    // context, so a dispatch never has to: a null one is not an error the
    // port reports, it is a silent loss of every motion vector.
    FfxFsr3UpscalerSharedResourceDescriptions shared{};
    const FfxErrorCode sr = ffxFsr3UpscalerGetSharedResourceDescriptions(&e.ctx, &shared);
    if (sr != FFX_OK || !makeSharedSurface(shared.dilatedDepth, &e.dilatedDepth) ||
        !makeSharedSurface(shared.dilatedMotionVectors, &e.dilatedMv) ||
        !makeSharedSurface(shared.reconstructedPrevNearestDepth, &e.prevNearestDepth)) {
        releaseEyeSurfaces(e);
        ffxFsr3UpscalerContextDestroy(&e.ctx);
        e = EyeCtx{};
        snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                 "AMD's three caller-owned working surfaces would not be made for eye %u at "
                 "%ux%u (dilated depth, dilated motion vectors, reconstructed previous nearest "
                 "depth): %s",
                 eye, w, h,
                 sr != FFX_OK ? "the port would not describe them" : "this device would not "
                                                                     "create one of them");
        g_reason = g_reasonBuf;
        if (why) *why = g_reason;
        return false;
    }

    e.valid = true;
    e.w = w; e.h = h; e.outW = outW; e.outH = outH;

    uint64_t vramAfter = 0;
    const bool haveVramAfter = queryVideoMemoryUsage(g_device, &vramAfter);
    char vramNote[48];
    if (haveVramBefore && haveVramAfter) {
        const double deltaMb = (static_cast<double>(vramAfter) - static_cast<double>(vramBefore)) /
                               (1024.0 * 1024.0);
        snprintf(vramNote, sizeof(vramNote), "%+.1f MB", deltaMb);
    } else {
        snprintf(vramNote, sizeof(vramNote), "not reported by this device");
    }
    Log::get().note(
        "fsr3: the context is created for eye %u at %ux%u -> %ux%u%s; VRAM %s; the history "
        "starts here (made in %.0f ms).",
        eye, w, h, outW, outH, diagnostics ? ", AMD's own debug checking on" : "", vramNote, ms);
    return true;
}

#endif  // EDVR_HAVE_FSR3

}  // namespace

bool fsr3Available(ID3D11Device* dev, const char** why) {
#if !EDVR_HAVE_FSR3
    (void)dev;
    // Read here too (not only from fsr3Evaluate's stub, which is never
    // reached without the SDK) so the config contract's static scan of
    // src\ -- and a build with no SDK at runtime -- both see the two
    // advanced keys read, per the design doc's Phase 0 (section 3.1).
    (void)fsr3ReadConfig();
    g_tried = true;
    g_available = false;
    g_reason = "this build was made without AMD's upscaler (EDVR_HAVE_FSR3)";
    if (why) *why = g_reason;
    return false;
#else
    if (!g_tried) {
        // Stamped wherever the first ask runs (the loading-screen warm-up,
        // temporal_pass.cpp's warmTrainedOnce, or the first treat) so the
        // log prices AMD's own initialisation apart from context creates,
        // mirroring dlaaAvailable (dlaa.cpp:441-525).
        const int64_t t0 = qpcNow();
        g_tried = true;
        g_available = false;
        if (!dev) {
            g_reason = "no device";
        } else if (dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1) {
            // 11_1, not 11_0: the port's LUMA_PYRAMID pass declares
            // "requires additional functionality: 64 UAV slots", which is
            // 11_1's compute-shader UAV-slot extension (base D3D11 caps a
            // compute shader at 8). An 11_0 device takes every other pass
            // and then fails context creation with
            // FFX_ERROR_BACKEND_API_ERROR on that one -- measured on a WARP
            // device asked for each level in turn (tools\fsr3_engine_test's
            // own createWarpDevice carries the same note). Refusing here
            // makes that a sentence in the log instead. Elite's own device
            // negotiates 12_0 (flight log 2026-09-16 20:26), so no rig in
            // the field is affected; this is for older hardware.
            g_reason = "the device is below feature level 11_1, which AMD's port's luma-pyramid "
                       "pass needs for its 64 compute UAV slots";
        } else {
            D3D11_FEATURE_DATA_D3D11_OPTIONS2 opts2{};
            const HRESULT optsHr =
                dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &opts2, sizeof(opts2));
            if (FAILED(optsHr) || !opts2.TypedUAVLoadAdditionalFormats) {
                g_reason =
                    "the device does not support typed UAV loads of additional formats "
                    "(D3D11_FEATURE_D3D11_OPTIONS2), which AMD's internal history targets need";
            } else {
                const char* missing = nullptr;
                for (const FormatCheck& fc : kFsrUavFormats) {
                    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs2{};
                    fs2.InFormat = fc.format;
                    if (FAILED(dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs2,
                                                        sizeof(fs2))) ||
                        !(fs2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) {
                        missing = fc.name;
                        break;
                    }
                }
                if (missing) {
                    snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                             "the device cannot typed-UAV-load %s, one of the formats AMD's "
                             "port keeps its history in",
                             missing);
                    g_reason = g_reasonBuf;
                } else {
                    const size_t scratchSize = ffxGetScratchMemorySizeDX11(2);
                    g_scratch.assign(scratchSize, uint8_t{0});
                    const FfxDevice ffxDev = ffxGetDeviceDX11_Fsr31(dev);
                    const FfxErrorCode ir = ffxGetInterfaceDX11(&g_backend, ffxDev, g_scratch.data(),
                                                                g_scratch.size(), 2);
                    if (ir != FFX_OK) {
                        snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                                 "the DX11 backend interface would not initialise: %s (0x%08X)",
                                 ffxErrorName(ir), static_cast<unsigned>(ir));
                        g_reason = g_reasonBuf;
                        g_scratch.clear();
                        g_scratch.shrink_to_fit();
                    } else {
                        g_device = dev;
                        g_available = true;
                        g_reason = "available";
                    }
                }
            }
        }
        const double ms = qpcFrequency() > 0
                              ? static_cast<double>(qpcNow() - t0) * 1000.0 /
                                    static_cast<double>(qpcFrequency())
                              : 0.0;
        Log::get().note("fsr3: first asked for on %s, AMD's port version %s.", adapterName(dev),
                        fsr3VersionLabel());
        if (g_available) {
            Log::get().note("fsr3: the DX11 backend initialised in %.0f ms on device %p.", ms,
                            (void*)dev);
        } else {
            Log::get().note("fsr3: refused after %.0f ms: %s", ms, g_reason);
        }
    }
    if (why) *why = g_reason;
    return g_available;
#endif
}

bool fsr3Warm(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, uint32_t outW,
              uint32_t outH, double* createMs, const char** why) {
#if !EDVR_HAVE_FSR3
    (void)ctx; (void)w; (void)h; (void)outW; (void)outH;
    if (createMs) *createMs = 0.0;
    // Quiet: dlaaWarm's own stub (dlaa.cpp:527-534) does not log either,
    // and warmTrainedOnce (temporal_pass.cpp) calls this at most once a
    // session (g_warmState's terminal states), so there is nothing to
    // de-duplicate here.
    if (why) *why = "this build was made without AMD's upscaler (EDVR_HAVE_FSR3)";
    return false;
#else
    if (createMs) *createMs = 0.0;
    if (!ctx || !w || !h || !outW || !outH) {
        if (why) *why = "a missing input";
        return false;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    const bool ok = fsr3Available(dev, why);  // ~0 ms when already asked
    if (dev) dev->Release();
    if (!ok) return false;
    double ms0 = 0.0, ms1 = 0.0;
    if (!ensureContext(0, w, h, outW, outH, why, &ms0)) return false;
    if (!ensureContext(1, w, h, outW, outH, why, &ms1)) return false;
    if (createMs) *createMs = ms0 + ms1;
    return true;
#endif
}

bool fsr3Evaluate(ID3D11DeviceContext* ctx, unsigned eye, ID3D11Texture2D* colour,
                  ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* reactive,
                  ID3D11Texture2D* out, uint32_t w, uint32_t h, uint32_t outW,
                  uint32_t outH, float jx, float jy, bool reset, float frameMs,
                  float nearZ, float farZ, float fovY, const char** why) {
#if !EDVR_HAVE_FSR3
    (void)ctx; (void)eye; (void)colour; (void)depth; (void)mv; (void)reactive; (void)out;
    (void)w; (void)h; (void)outW; (void)outH; (void)jx; (void)jy; (void)reset; (void)frameMs;
    (void)nearZ; (void)farZ; (void)fovY;
    if (why) *why = "this build was made without AMD's upscaler (EDVR_HAVE_FSR3)";
    return false;
#else
    if (!g_available || !ctx || !colour || !depth || !mv || !out || eye > 1 || !w || !h) {
        if (why) *why = g_available ? "a missing input" : g_reason;
        return false;
    }
    pollTimingRing(ctx);
    if (!outW || !outH) {
        outW = w;
        outH = h;
    }
    // The context: found made (by the warm-up or a previous frame) or made
    // here, through the one block the warm-up shares (ensureContext).
    if (!ensureContext(eye, w, h, outW, outH, why, nullptr)) return false;
    EyeCtx& e = g_ctx[eye];

    // FSR's cameraNear/cameraFar, under DEPTH_INVERTED: the port's own
    // debug check (ffx_fsr3upscaler.cpp's fsr3upscalerDebugCheckDispatch,
    // gated on ENABLE_DEBUG_CHECKING) warns when cameraNear < cameraFar, so
    // the LARGER of the pair is handed in as cameraNear and the smaller as
    // cameraFar. The actual view-space transform this feeds
    // (setupDeviceDepthToViewSpaceDepthParams) takes FFX_MINIMUM/
    // FFX_MAXIMUM of the pair regardless of which argument carried which
    // value, and its own comment says so ("make sure it has no impact if
    // near and far plane values are swapped in dispatch params") -- so the
    // ordering below only silences DEBUG_CHECKING; it changes nothing the
    // upscaler actually computes. nearZ/farZ are 0 when no depth has been
    // probed yet (temporal_pass.h); the documented defaults stand in.
    float n = nearZ, f = farZ;
    if (n <= 0.0f || f <= 0.0f) {
        n = kFsrDefaultNearZ;
        f = kFsrDefaultFarZ;
    }
    const float camNear = n > f ? n : f;
    const float camFar = n > f ? f : n;

    const Fsr3Settings settings = fsr3ReadConfig();

    FfxFsr3UpscalerDispatchDescription dd{};
    dd.commandList = ffxGetCommandListDX11(ctx);
    const FfxResourceDescription colourDesc = GetFfxResourceDescriptionDX11(colour);
    const FfxResourceDescription depthDesc = GetFfxResourceDescriptionDX11(depth);
    const FfxResourceDescription mvDesc = GetFfxResourceDescriptionDX11(mv);
    const FfxResourceDescription outDesc = GetFfxResourceDescriptionDX11(out);
    dd.color = ffxGetResourceDX11_Fsr31(colour, colourDesc, L"EDVR_FSR_Color",
                                        FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.depth = ffxGetResourceDX11_Fsr31(depth, depthDesc, L"EDVR_FSR_Depth",
                                        FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.motionVectors = ffxGetResourceDX11_Fsr31(mv, mvDesc, L"EDVR_FSR_Mv",
                                                FFX_RESOURCE_STATE_COMPUTE_READ);
    if (reactive) {
        const FfxResourceDescription reactiveDesc = GetFfxResourceDescriptionDX11(reactive);
        dd.reactive = ffxGetResourceDX11_Fsr31(reactive, reactiveDesc, L"EDVR_FSR_Reactive",
                                               FFX_RESOURCE_STATE_COMPUTE_READ);
    }
    // The three caller-owned working surfaces FSR 3.1 dispatches through
    // (EyeCtx, ensureContext): the dilate pass writes the dilated motion
    // vectors and depth into the first two and the reprojection reads them
    // straight back, so leaving them null costs every motion vector
    // silently -- FFX_OK, an accumulating history, and no motion in it.
    const FfxResourceDescription dilDepthDesc = GetFfxResourceDescriptionDX11(e.dilatedDepth);
    const FfxResourceDescription dilMvDesc = GetFfxResourceDescriptionDX11(e.dilatedMv);
    const FfxResourceDescription prevDepthDesc = GetFfxResourceDescriptionDX11(e.prevNearestDepth);
    dd.dilatedDepth = ffxGetResourceDX11_Fsr31(e.dilatedDepth, dilDepthDesc, L"EDVR_FSR_DilatedDepth",
                                               FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.dilatedMotionVectors = ffxGetResourceDX11_Fsr31(
        e.dilatedMv, dilMvDesc, L"EDVR_FSR_DilatedMv", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.reconstructedPrevNearestDepth =
        ffxGetResourceDX11_Fsr31(e.prevNearestDepth, prevDepthDesc, L"EDVR_FSR_PrevNearestDepth",
                                 FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    // exposure and transparencyAndComposition stay zero-initialised (no
    // resource): design doc 3.3 -- preExposure is the constant 1 instead,
    // and EDVR has no transparency/composition mask.
    dd.output = ffxGetResourceDX11_Fsr31(out, outDesc, L"EDVR_FSR_Output",
                                         FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.jitterOffset = FfxFloatCoords2D{jx * kFsrJitterScaleX, jy * kFsrJitterScaleY};
    dd.motionVectorScale = FfxFloatCoords2D{kFsrMotionVectorScaleX, kFsrMotionVectorScaleY};
    dd.renderSize = FfxDimensions2D{w, h};
    dd.upscaleSize = FfxDimensions2D{outW, outH};
    dd.enableSharpening = false;
    dd.sharpness = 0.0f;
    dd.frameTimeDelta = frameMs < 0.1f ? 0.1f : frameMs > 100.0f ? 100.0f : frameMs;
    dd.preExposure = 1.0f;
    dd.reset = reset;
    dd.cameraNear = camNear;
    dd.cameraFar = camFar;
    dd.cameraFovAngleVertical = fovY > 0.0f ? fovY : kFsrDefaultFovYRadians;
    dd.viewSpaceToMetersFactor = 1.0f;
    dd.flags = settings.debug ? FFX_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW : 0u;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    const int qs = dev ? acquireQuerySlot(dev, ctx) : -1;
    if (dev) dev->Release();
    if (qs >= 0) g_qring[qs].eye = static_cast<int>(eye);
    // Same TIF/throw risk as ensureContext's create call above (ExecuteGpuJobsDX11
    // and RegisterResourceDX11 both carry TIF-guarded D3D11 calls a real dispatch
    // can reach), folded into the same why-string contract rather than crashing.
    FfxErrorCode dr = FFX_ERROR_BACKEND_API_ERROR;
    bool threw = false;
    try {
        dr = ffxFsr3UpscalerContextDispatch(&e.ctx, &dd);
    } catch (...) {
        threw = true;
    }
    if (qs >= 0) g_qring[qs].timer.end(ctx);  // Poll consumes failed End samples too.
    if (threw) {
        snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                 "the dispatch failed: a D3D11 call inside AMD's port failed and its backend "
                 "raised a C++ exception instead of an FfxErrorCode");
        g_reason = g_reasonBuf;
        if (why) *why = g_reason;
        return false;
    }
    if (dr != FFX_OK) {
        snprintf(g_reasonBuf, sizeof(g_reasonBuf), "the dispatch failed: %s (0x%08X)",
                 ffxErrorName(dr), static_cast<unsigned>(dr));
        g_reason = g_reasonBuf;
        if (why) *why = g_reason;
        return false;
    }
    ++g_evaluations;
    if (reset) ++g_resets;
    return true;
#endif
}

void fsr3ReleaseFeatures() {
#if EDVR_HAVE_FSR3
    for (EyeCtx& e : g_ctx) {
        if (e.valid) {
            ffxFsr3UpscalerContextDestroy(&e.ctx);
            releaseEyeSurfaces(e);
            e = EyeCtx{};
        }
    }
#endif
}

void fsr3Shutdown() {
#if EDVR_HAVE_FSR3
    fsr3ReleaseFeatures();
    for (QuerySlot& q : g_qring) releaseQuerySlot(q);
    g_scratch.clear();
    g_scratch.shrink_to_fit();
    g_backend = FfxInterface{};
    g_device = nullptr;
#endif
    if (g_evaluations > 0) {
        Log::get().note("fsr3: %u eye-frames evaluated this session (%u of them started "
                        "AMD's history afresh), %.2f ms each on average (max %.2f).",
                        g_evaluations, g_resets,
                        g_timeCount ? g_timeSum / static_cast<double>(g_timeCount) : 0.0,
                        g_timeMax);
    }
    g_tried = false;
    g_available = false;
}

const char* fsr3VersionLabel() {
#if EDVR_HAVE_FSR3
    static char label[32] = {};
    if (!label[0]) {
        snprintf(label, sizeof(label), "fsr %d.%d.%d", FFX_FSR3UPSCALER_VERSION_MAJOR,
                 FFX_FSR3UPSCALER_VERSION_MINOR, FFX_FSR3UPSCALER_VERSION_PATCH);
    }
    return label;
#else
    return "fsr";
#endif
}

bool fsr3Totals(uint32_t* evaluations, double* avgMs, double* maxMs, uint32_t* resets) {
    if (g_evaluations == 0) return false;
    if (evaluations) *evaluations = g_evaluations;
    if (resets) *resets = g_resets;
    if (avgMs) *avgMs = g_timeCount ? g_timeSum / static_cast<double>(g_timeCount) : 0.0;
    if (maxMs) *maxMs = g_timeMax;
    return true;
}

Fsr3Settings fsr3ReadConfig() {
    Fsr3Settings s;
    auto& cfg = Config::get();
    s.reactive = cfg.getBool("advanced.temporal_aa_fsr_reactive", false);
    s.debug = cfg.getBool("advanced.temporal_aa_fsr_debug", false);
    return s;
}

#if EDVR_HAVE_FSR3
// Test-only, NOT part of fsr3_engine.h's contract: how many fpMessage lines
// this session has relayed to the log (FsrMessage, above), so
// tools\fsr3_engine_test can prove a dispatch under DEBUG_CHECKING stayed
// silent without scraping the log file. The rig forward-declares this
// itself; it is deliberately absent from the header everything else
// includes.
uint32_t fsr3TestMessageCount() { return g_msgCount; }
#endif

}  // namespace edvr
