#include "dlaa.h"

#include <cstdio>
#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/log.h"

#ifdef EDVR_HAVE_NGX
// NVIDIA's SDK, as shipped: nvsdk_ngx.h declares the D3D11 entry points,
// nvsdk_ngx_helpers.h the DLSS create/evaluate wrappers and their
// parameter structs. Built only when build.bat found the SDK.
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4245 4324 4505)
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#pragma warning(pop)
#endif

namespace edvr {
namespace {

bool        g_tried = false;
bool        g_available = false;
const char* g_reason = "not asked yet";

uint32_t g_evaluations = 0;
uint32_t g_resets = 0;        // evaluations that restarted NVIDIA's history
uint32_t g_timeCount = 0;
double   g_timeSum = 0.0;
double   g_timeMax = 0.0;

#ifdef EDVR_HAVE_NGX

ID3D11Device*       g_device = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;
// The capability block, kept: the optimal-settings query only answers on
// THIS block (the SDK's helper looks its callback up here and returns
// FAIL_OutOfDate on a block from AllocateParameters -- which is what the
// first two flights did, printing 0x0 as if the runtime had answered;
// the review of 2026-09-04, F2).
NVSDK_NGX_Parameter* g_caps = nullptr;
bool                 g_optimalFailNoted = false;

struct EyeFeature {
    NVSDK_NGX_Handle* handle = nullptr;
    uint32_t          w = 0, h = 0;
    uint32_t          outW = 0, outH = 0;
};

const char* qualityName(NVSDK_NGX_PerfQuality_Value q) {
    switch (q) {
        case NVSDK_NGX_PerfQuality_Value_DLAA:             return "DLAA";
        case NVSDK_NGX_PerfQuality_Value_UltraQuality:     return "ultra quality";
        case NVSDK_NGX_PerfQuality_Value_MaxQuality:       return "quality";
        case NVSDK_NGX_PerfQuality_Value_Balanced:         return "balanced";
        case NVSDK_NGX_PerfQuality_Value_MaxPerf:          return "performance";
        case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return "ultra performance";
        default:                                           return "?";
    }
}
EyeFeature g_feature[2];

// The GPU-price ring, the resolve's discipline: never awaited.
struct QuerySlot {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
    bool         inUse = false;
};
constexpr int kQueryRing = 8;
QuerySlot g_qring[kQueryRing];

void releaseQuerySlot(QuerySlot& q) {
    if (q.disjoint) { q.disjoint->Release(); q.disjoint = nullptr; }
    if (q.begin) { q.begin->Release(); q.begin = nullptr; }
    if (q.end) { q.end->Release(); q.end = nullptr; }
    q.inUse = false;
}

void pollTimingRing(ID3D11DeviceContext* ctx) {
    for (QuerySlot& q : g_qring) {
        if (!q.inUse) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(q.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        UINT64 t0 = 0, t1 = 0;
        const HRESULT hr0 = ctx->GetData(q.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT hr1 = ctx->GetData(q.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        q.inUse = false;
        if (dj.Disjoint || hr0 != S_OK || hr1 != S_OK || dj.Frequency == 0) continue;
        const double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
        ++g_timeCount;
        g_timeSum += ms;
        if (ms > g_timeMax) g_timeMax = ms;
    }
}

int acquireQuerySlot(ID3D11Device* dev) {
    for (int i = 0; i < kQueryRing; ++i) {
        QuerySlot& q = g_qring[i];
        if (q.inUse) continue;
        if (!q.disjoint) {
            D3D11_QUERY_DESC qdd{};
            qdd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            D3D11_QUERY_DESC qdt{};
            qdt.Query = D3D11_QUERY_TIMESTAMP;
            const bool made = SUCCEEDED(dev->CreateQuery(&qdd, &q.disjoint)) &&
                              SUCCEEDED(dev->CreateQuery(&qdt, &q.begin)) &&
                              SUCCEEDED(dev->CreateQuery(&qdt, &q.end));
            if (!made) {
                releaseQuerySlot(q);
                continue;
            }
        }
        return i;
    }
    return -1;
}

const char* ngxResultName(NVSDK_NGX_Result r) {
    switch (r) {
        case NVSDK_NGX_Result_Success:                    return "success";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported:   return "the feature is not supported on this GPU";
        case NVSDK_NGX_Result_FAIL_PlatformError:         return "a platform error";
        case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:  return "the feature already exists";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound:       return "the feature was not found";
        case NVSDK_NGX_Result_FAIL_InvalidParameter:      return "an invalid parameter";
        case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "the scratch buffer is too small";
        case NVSDK_NGX_Result_FAIL_NotInitialized:        return "NGX is not initialised";
        case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "an unsupported input format";
        case NVSDK_NGX_Result_FAIL_RWFlagMissing:         return "a read/write flag is missing on a resource";
        case NVSDK_NGX_Result_FAIL_MissingInput:          return "a required input is missing";
        case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "the feature could not be initialised";
        case NVSDK_NGX_Result_FAIL_OutOfDate:             return "the driver is out of date";
        case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:        return "out of GPU memory";
        case NVSDK_NGX_Result_FAIL_UnsupportedFormat:     return "an unsupported format";
        case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "the application data path is not writable";
        case NVSDK_NGX_Result_FAIL_UnsupportedParameter:  return "an unsupported parameter";
        case NVSDK_NGX_Result_FAIL_Denied:                return "denied (the runtime refused this application)";
        default:                                          return "an NGX error";
    }
}

char g_reasonBuf[256];

void releaseFeatures() {
    for (EyeFeature& f : g_feature) {
        if (f.handle) {
            NVSDK_NGX_D3D11_ReleaseFeature(f.handle);
            f.handle = nullptr;
        }
        f.w = f.h = 0;
    }
}

#endif  // EDVR_HAVE_NGX

}  // namespace

bool dlaaAvailable(ID3D11Device* dev, const char** reason) {
#ifndef EDVR_HAVE_NGX
    (void)dev;
    g_reason = "this build has no DLSS SDK in it (build with EDVR_NGX_SDK set)";
    if (reason) *reason = g_reason;
    return false;
#else
    if (!g_tried) {
        g_tried = true;
        g_available = false;
        if (!dev) {
            g_reason = "no device";
        } else {
            // A project id NGX accepts is a GUID, hex only: the first try had
            // letters in it and was refused as an invalid parameter (the
            // harness, 2026-09-03).
            // The game's own directory is where NGX looks for nvngx_dlss.dll
            // and where its logs may go; EDVR's logs live beside it too.
            wchar_t dir[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, dir, MAX_PATH);
            wchar_t* slash = wcsrchr(dir, L'\\');
            if (slash) *slash = 0;
            const NVSDK_NGX_Result init = NVSDK_NGX_D3D11_Init_with_ProjectID(
                "6f2c7c6e-3d5a-4b91-8e0d-2a9f4c1b7e33", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                "0.13", dir, dev);
            if (NVSDK_NGX_FAILED(init)) {
                snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                         "NGX would not initialise: %s (0x%08X); is nvngx_dlss.dll "
                         "beside the game's executable, and the driver current?",
                         ngxResultName(init), static_cast<unsigned>(init));
                g_reason = g_reasonBuf;
            } else {
                g_device = dev;
                NVSDK_NGX_Parameter* caps = nullptr;
                const NVSDK_NGX_Result cr = NVSDK_NGX_D3D11_GetCapabilityParameters(&caps);
                int available = 0;
                if (NVSDK_NGX_FAILED(cr) || !caps ||
                    NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) ||
                    !available) {
                    int needUpdate = 0;
                    if (caps) caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needUpdate);
                    snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                             "the runtime says DLSS is not available on this GPU%s",
                             needUpdate ? " (it wants a newer driver)" : "");
                    g_reason = g_reasonBuf;
                } else if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&g_params)) ||
                           !g_params) {
                    g_reason = "NGX would not allocate its parameters";
                } else {
                    g_caps = caps;
                    g_available = true;
                    g_reason = "available";
                }
            }
        }
    }
    if (reason) *reason = g_reason;
    return g_available;
#endif
}

bool dlaaEvaluate(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                  ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                  ID3D11Texture2D* output, uint32_t w, uint32_t h,
                  uint32_t outW, uint32_t outH, float jx, float jy, bool reset,
                  float frameMs, const char** reason) {
#ifndef EDVR_HAVE_NGX
    (void)ctx; (void)eye; (void)colour; (void)depth; (void)motion; (void)output;
    (void)w; (void)h; (void)outW; (void)outH; (void)jx; (void)jy; (void)reset;
    (void)frameMs;
    if (reason) *reason = "this build has no DLSS SDK in it";
    return false;
#else
    if (!g_available || !g_params || !g_caps || !ctx || !colour || !depth || !motion || !output ||
        eye < 0 || eye > 1 || !w || !h) {
        if (reason) *reason = g_available ? "a missing input" : g_reason;
        return false;
    }
    pollTimingRing(ctx);
    if (!outW || !outH) {
        outW = w;
        outH = h;
    }
    EyeFeature& f = g_feature[eye];
    if (!f.handle || f.w != w || f.h != h || f.outW != outW || f.outH != outH) {
        if (f.handle) {
            NVSDK_NGX_D3D11_ReleaseFeature(f.handle);
            f.handle = nullptr;
        }
        // Equal sizes are DLAA. A larger output is DLSS proper: the mode
        // is the one whose own render size, as the runtime names it for
        // this output, is nearest the input among the modes whose range
        // holds it -- the input is whatever Elite's HMD Quality produced,
        // not what a mode would ask for. The size ratio decides only when
        // the runtime will not say (it did not, for two flights: the query
        // was made on the wrong parameter block; the review's F2).
        NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_DLAA;
        unsigned optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
        float sharpness = 0.0f;
        bool optKnown = false;
        NVSDK_NGX_Result optErr = NVSDK_NGX_Result_Success;
        if (outW == w && outH == h) {
            optErr = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_caps, w, h, quality, &optW, &optH, &maxW,
                                                   &maxH, &minW, &minH, &sharpness);
            optKnown = !NVSDK_NGX_FAILED(optErr);
        } else {
            const NVSDK_NGX_PerfQuality_Value ladder[4] = {
                NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced,
                NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance};
            int best = -1;
            unsigned bestDiff = ~0u;
            for (int k = 0; k < 4; ++k) {
                unsigned oW2 = 0, oH2 = 0, mxW = 0, mxH = 0, mnW = 0, mnH = 0;
                float sh = 0.0f;
                optErr = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_caps, outW, outH, ladder[k], &oW2, &oH2,
                                                       &mxW, &mxH, &mnW, &mnH, &sh);
                if (NVSDK_NGX_FAILED(optErr)) break;
                optKnown = true;
                const bool inRange = w >= mnW && h >= mnH && w <= mxW && h <= mxH;
                const unsigned diff = oW2 > w ? oW2 - w : w - oW2;
                if (inRange && diff < bestDiff) {
                    best = k;
                    bestDiff = diff;
                    optW = oW2; optH = oH2; minW = mnW; minH = mnH; maxW = mxW; maxH = mxH;
                    sharpness = sh;
                }
            }
            if (best >= 0) {
                quality = ladder[best];
            } else if (optKnown) {
                snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                         "a %ux%u frame sits outside every DLSS mode's render range for a "
                         "%ux%u output",
                         w, h, outW, outH);
                g_reason = g_reasonBuf;
                if (reason) *reason = g_reason;
                return false;
            } else {
                const float ratio = static_cast<float>(w) / static_cast<float>(outW);
                quality = ratio >= 0.66f ? ladder[0] : ratio >= 0.58f ? ladder[1]
                        : ratio >= 0.5f ? ladder[2] : ladder[3];
            }
        }
        if (!optKnown && !g_optimalFailNoted) {
            g_optimalFailNoted = true;
            Log::get().note(
                "dlss: the runtime would not name its render sizes (%s, 0x%08X); the mode "
                "is chosen by the size ratio alone.",
                ngxResultName(optErr), static_cast<unsigned>(optErr));
        }
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = w;
        cp.Feature.InHeight = h;
        cp.Feature.InTargetWidth = outW;
        cp.Feature.InTargetHeight = outH;
        cp.Feature.InPerfQualityValue = quality;
        // LDR colour; motion vectors at the render size, unjittered (the
        // pass computes them on the unjittered grid); reversed-Z depth.
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        cp.InEnableOutputSubrects = false;
        const NVSDK_NGX_Result cr =
            NGX_D3D11_CREATE_DLSS_EXT(ctx, &f.handle, g_params, &cp);
        if (NVSDK_NGX_FAILED(cr) || !f.handle) {
            f.handle = nullptr;
            snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                     "the DLAA feature would not be created at %ux%u: %s (0x%08X)", w, h,
                     ngxResultName(cr), static_cast<unsigned>(cr));
            g_reason = g_reasonBuf;
            if (reason) *reason = g_reason;
            return false;
        }
        f.w = w;
        f.h = h;
        f.outW = outW;
        f.outH = outH;
        if (outW == w && outH == h) {
            Log::get().note(
                "dlaa: the feature is created for eye %d at %ux%u, DLAA (the runtime's "
                "optimal render size for this output %ux%u, which DLAA ignores); the "
                "history starts here.",
                eye, w, h, optW, optH);
        } else {
            Log::get().note(
                "dlss: the feature is created for eye %d, %ux%u in and %ux%u out (%.0f%% "
                "per axis), the %s mode, whose own render size is %ux%u and whose range "
                "the runtime names as %ux%u..%ux%u; the history starts here.",
                eye, w, h, outW, outH,
                100.0 * static_cast<double>(w) / static_cast<double>(outW),
                qualityName(quality), optW, optH, minW, minH, maxW, maxH);
        }
    }

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = colour;
    ep.Feature.pInOutput = output;
    ep.Feature.InSharpness = 0.0f;
    ep.pInDepth = depth;
    ep.pInMotionVectors = motion;
    ep.InJitterOffsetX = jx;
    ep.InJitterOffsetY = jy;
    ep.InRenderSubrectDimensions.Width = w;
    ep.InRenderSubrectDimensions.Height = h;
    ep.InReset = reset ? 1 : 0;
    ep.InMVScaleX = 1.0f;
    ep.InMVScaleY = 1.0f;
    ep.InPreExposure = 1.0f;
    ep.InExposureScale = 1.0f;
    // The frame delta the SDK asks for ("helps in determining the amount
    // to denoise or anti-alias based on the speed of the object"); zero
    // when unknown, which the runtime treats as unstated.
    ep.InFrameTimeDeltaInMsec = frameMs;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    const int qs = dev ? acquireQuerySlot(dev) : -1;
    if (dev) dev->Release();
    if (qs >= 0) {
        ctx->Begin(g_qring[qs].disjoint);
        ctx->End(g_qring[qs].begin);
    }
    const NVSDK_NGX_Result er = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, f.handle, g_params, &ep);
    if (qs >= 0) {
        ctx->End(g_qring[qs].end);
        ctx->End(g_qring[qs].disjoint);
        g_qring[qs].inUse = true;
    }
    if (NVSDK_NGX_FAILED(er)) {
        snprintf(g_reasonBuf, sizeof(g_reasonBuf), "the evaluation failed: %s (0x%08X)",
                 ngxResultName(er), static_cast<unsigned>(er));
        g_reason = g_reasonBuf;
        if (reason) *reason = g_reason;
        return false;
    }
    ++g_evaluations;
    if (reset) ++g_resets;
    return true;
#endif
}

bool dlaaTotals(uint32_t* evaluations, double* avgMs, double* maxMs,
                uint32_t* resets) {
    if (g_evaluations == 0) return false;
    if (evaluations) *evaluations = g_evaluations;
    if (resets) *resets = g_resets;
    if (avgMs) *avgMs = g_timeCount ? g_timeSum / static_cast<double>(g_timeCount) : 0.0;
    if (maxMs) *maxMs = g_timeMax;
    return true;
}

void dlaaShutdown() {
#ifdef EDVR_HAVE_NGX
    releaseFeatures();
    for (QuerySlot& q : g_qring) releaseQuerySlot(q);
    if (g_params) {
        NVSDK_NGX_D3D11_DestroyParameters(g_params);
        g_params = nullptr;
    }
    if (g_caps) {
        NVSDK_NGX_D3D11_DestroyParameters(g_caps);
        g_caps = nullptr;
    }
    if (g_device) {
        NVSDK_NGX_D3D11_Shutdown1(g_device);
        g_device = nullptr;
    }
#endif
    if (g_evaluations > 0) {
        Log::get().note("dlaa: %u eye-frames evaluated this session (%u of them started "
                        "NVIDIA's history afresh), %.2f ms each on average (max %.2f).",
                        g_evaluations, g_resets,
                        g_timeCount ? g_timeSum / static_cast<double>(g_timeCount) : 0.0,
                        g_timeMax);
    }
    g_tried = false;
    g_available = false;
}

}  // namespace edvr

// For tools/smoke: is DLAA usable on this device, and if not, why.
extern "C" __declspec(dllexport) int edvrDlaaAvailable(void* device, const char** reason) {
    return edvr::dlaaAvailable(static_cast<ID3D11Device*>(device), reason) ? 1 : 0;
}

// For tools/smoke: the evaluations so far and how many carried the reset --
// the desk check that NVIDIA's history is not restarted every frame. 0 when
// nothing has run.
extern "C" __declspec(dllexport) int edvrDlaaCounts(unsigned* evaluations, unsigned* resets) {
    uint32_t e = 0, r = 0;
    double a = 0.0, m = 0.0;
    if (!edvr::dlaaTotals(&e, &a, &m, &r)) return 0;
    if (evaluations) *evaluations = e;
    if (resets) *resets = r;
    return 1;
}
