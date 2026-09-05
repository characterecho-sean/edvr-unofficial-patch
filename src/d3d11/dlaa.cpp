#include "dlaa.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

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
// The fovea features, kept apart from the full-frame ones: created with
// output sub-rectangles enabled and their own history, so switching the
// fovea on or off never disturbs the full-frame path's accumulation.
EyeFeature g_fovea[2];
// The steady periphery's features (docs/performance.md feature 6): DLAA on
// a reduced copy of the frame, a third slot with its own history, so the
// fovea, the periphery and the full frame never share an accumulation.
EyeFeature g_periph[2];

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
    for (EyeFeature& f : g_fovea) {
        if (f.handle) {
            NVSDK_NGX_D3D11_ReleaseFeature(f.handle);
            f.handle = nullptr;
        }
        f.w = f.h = 0;
    }
    for (EyeFeature& f : g_periph) {
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

#ifdef EDVR_HAVE_NGX
namespace {
// The general crop evaluation, which both the fovea and the steady periphery
// are: NVIDIA runs on an INPUT crop (icx,icy,icw,ich) of the render-size
// textures and writes an OUTPUT crop (ocx,ocy,ocw,och) of the native output,
// through a feature `f` of its own (`what` names it in the log). The feature
// is created at the input CROP size -> the output CROP size, not the full
// frame -- the difference the crop probe (dlaaCropProbe) validated and the
// production path first got wrong (0xBAD00005 in the field, 2026-09-05).
// InRenderSubrectDimensions equals the created InWidth/InHeight; the input
// sub-rect bases locate the crop in the full-frame render textures, and the
// output base writes the (possibly upscaled) crop into the native output.
// Equal crops are DLAA (1:1); a smaller input is DLSS upscaling just the
// crop. Keyed on all four sizes, so a live width or render-size change
// rebuilds and resets.
bool evaluateCrop(EyeFeature& f, const char* what, int eye, ID3D11DeviceContext* ctx,
                  ID3D11Texture2D* colour, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                  ID3D11Texture2D* output, uint32_t inW, uint32_t inH, uint32_t outW,
                  uint32_t outH, uint32_t icx, uint32_t icy, uint32_t icw, uint32_t ich,
                  uint32_t ocx, uint32_t ocy, uint32_t ocw, uint32_t och, float jx, float jy,
                  bool reset, float frameMs, const char** reason) {
    if (!g_available || !g_params || !ctx || !colour || !depth || !motion || !output ||
        !inW || !inH || !icw || !ich || !ocw || !och) {
        if (reason) *reason = g_available ? "a missing input" : g_reason;
        return false;
    }
    if (icx + icw > inW || icy + ich > inH || ocx + ocw > outW || ocy + och > outH) {
        snprintf(g_reasonBuf, sizeof(g_reasonBuf), "the %s crop falls outside the frame", what);
        g_reason = g_reasonBuf;
        if (reason) *reason = g_reason;
        return false;
    }
    pollTimingRing(ctx);
    bool didCreate = false;
    if (!f.handle || f.w != icw || f.h != ich || f.outW != ocw || f.outH != och) {
        if (f.handle) {
            NVSDK_NGX_D3D11_ReleaseFeature(f.handle);
            f.handle = nullptr;
        }
        // Equal crops are DLAA. A smaller input picks the quality mode by the
        // FRAME's upscale ratio, not the crop's -- the frame ratio is the same
        // for both eyes and stable, so the eyes never land on different DLSS
        // networks across a 0.5/0.58/0.667 threshold the way the per-eye crop
        // ratio does when rounding straddles it (the review of 2026-09-05,
        // F1). The +eps keeps exactly 0.5 on MaxPerf rather than
        // UltraPerformance (a 1/2-scale input is not the 1/3-scale mode).
        NVSDK_NGX_PerfQuality_Value q = NVSDK_NGX_PerfQuality_Value_DLAA;
        if (inW != outW || inH != outH) {
            const float ratio = static_cast<float>(inW) / static_cast<float>(outW) + 0.002f;
            q = ratio >= 0.667f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
              : ratio >= 0.58f  ? NVSDK_NGX_PerfQuality_Value_Balanced
              : ratio >= 0.5f   ? NVSDK_NGX_PerfQuality_Value_MaxPerf
              :                   NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        }
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = icw;
        cp.Feature.InHeight = ich;
        cp.Feature.InTargetWidth = ocw;
        cp.Feature.InTargetHeight = och;
        cp.Feature.InPerfQualityValue = q;
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        // NVIDIA writes at the output sub-rectangle's base rather than the origin.
        cp.InEnableOutputSubrects = true;
        const NVSDK_NGX_Result cr = NGX_D3D11_CREATE_DLSS_EXT(ctx, &f.handle, g_params, &cp);
        if (NVSDK_NGX_FAILED(cr) || !f.handle) {
            f.handle = nullptr;
            snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                     "the %s feature would not be created at %ux%u->%ux%u: %s (0x%08X)", what,
                     icw, ich, ocw, och, ngxResultName(cr), static_cast<unsigned>(cr));
            g_reason = g_reasonBuf;
            if (reason) *reason = g_reason;
            return false;
        }
        f.w = icw;
        f.h = ich;
        f.outW = ocw;
        f.outH = och;
        didCreate = true;
        Log::get().note(
            "temporal aa %s: NVIDIA's feature is created for eye %d, crop %ux%u in -> "
            "%ux%u out (%s, output sub-rectangles; input based at %u,%u in the %ux%u render, "
            "output at %u,%u in the %ux%u frame); its history starts here.",
            what, eye, icw, ich, ocw, och, (icw == ocw && ich == och) ? "DLAA" : qualityName(q),
            icx, icy, inW, inH, ocx, ocy, outW, outH);
    }

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = colour;
    ep.Feature.pInOutput = output;
    ep.Feature.InSharpness = 0.0f;
    ep.pInDepth = depth;
    ep.pInMotionVectors = motion;
    ep.InJitterOffsetX = jx;
    ep.InJitterOffsetY = jy;
    ep.InRenderSubrectDimensions.Width = icw;
    ep.InRenderSubrectDimensions.Height = ich;
    // A freshly created feature has no history, so it must reset whatever the
    // caller thought -- a live fovea-width or render-size change recreates the
    // feature without the caller's history flag knowing.
    ep.InReset = (reset || didCreate) ? 1 : 0;
    ep.InMVScaleX = 1.0f;
    ep.InMVScaleY = 1.0f;
    // The crop's top-left in each render-size input, and where the (possibly
    // upscaled) crop lands in the native output.
    ep.InColorSubrectBase.X = icx;
    ep.InColorSubrectBase.Y = icy;
    ep.InDepthSubrectBase.X = icx;
    ep.InDepthSubrectBase.Y = icy;
    ep.InMVSubrectBase.X = icx;
    ep.InMVSubrectBase.Y = icy;
    ep.InOutputSubrectBase.X = ocx;
    ep.InOutputSubrectBase.Y = ocy;
    ep.InPreExposure = 1.0f;
    ep.InExposureScale = 1.0f;
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
        snprintf(g_reasonBuf, sizeof(g_reasonBuf), "the %s evaluation failed: %s (0x%08X)", what,
                 ngxResultName(er), static_cast<unsigned>(er));
        g_reason = g_reasonBuf;
        if (reason) *reason = g_reason;
        return false;
    }
    ++g_evaluations;
    if (reset || didCreate) ++g_resets;   // a create forces a reset too (the review, F7)
    return true;
}
}  // namespace
#endif  // EDVR_HAVE_NGX

bool dlssEvaluateFovea(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                       ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                       ID3D11Texture2D* output, uint32_t inW, uint32_t inH,
                       uint32_t outW, uint32_t outH, uint32_t icx, uint32_t icy,
                       uint32_t icw, uint32_t ich, uint32_t ocx, uint32_t ocy,
                       uint32_t ocw, uint32_t och, float jx, float jy, bool reset,
                       float frameMs, const char** reason) {
#ifndef EDVR_HAVE_NGX
    (void)ctx; (void)eye; (void)colour; (void)depth; (void)motion; (void)output;
    (void)inW; (void)inH; (void)outW; (void)outH; (void)icx; (void)icy; (void)icw;
    (void)ich; (void)ocx; (void)ocy; (void)ocw; (void)och;
    (void)jx; (void)jy; (void)reset; (void)frameMs;
    if (reason) *reason = "this build has no DLSS SDK in it";
    return false;
#else
    if (eye < 0 || eye > 1) {
        if (reason) *reason = "a missing input";
        return false;
    }
    return evaluateCrop(g_fovea[eye], "fovea", eye, ctx, colour, depth, motion, output, inW, inH,
                        outW, outH, icx, icy, icw, ich, ocx, ocy, ocw, och, jx, jy, reset, frameMs,
                        reason);
#endif
}

// The 1:1 crop (DLAA): the output crop equals the input crop, in the same
// full-frame textures. A thin wrapper over the general path.
bool dlaaEvaluateFovea(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                       ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                       ID3D11Texture2D* output, uint32_t w, uint32_t h,
                       uint32_t cropX, uint32_t cropY, uint32_t cropW, uint32_t cropH,
                       float jx, float jy, bool reset, float frameMs,
                       const char** reason) {
    return dlssEvaluateFovea(ctx, eye, colour, depth, motion, output, w, h, w, h, cropX, cropY,
                             cropW, cropH, cropX, cropY, cropW, cropH, jx, jy, reset, frameMs,
                             reason);
}

// The steady periphery (docs/performance.md feature 6): DLAA over the WHOLE of
// a w x h frame -- a reduced copy of the render, or the render itself when the
// game rendered small -- through the third feature slot, so its history is
// its own. The composite upscales what comes back around the fovea.
bool dlaaEvaluatePeriphery(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                           ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                           ID3D11Texture2D* output, uint32_t w, uint32_t h, float jx, float jy,
                           bool reset, float frameMs, const char** reason) {
#ifndef EDVR_HAVE_NGX
    (void)ctx; (void)eye; (void)colour; (void)depth; (void)motion; (void)output;
    (void)w; (void)h; (void)jx; (void)jy; (void)reset; (void)frameMs;
    if (reason) *reason = "this build has no DLSS SDK in it";
    return false;
#else
    if (eye < 0 || eye > 1) {
        if (reason) *reason = "a missing input";
        return false;
    }
    return evaluateCrop(g_periph[eye], "periphery", eye, ctx, colour, depth, motion, output, w, h,
                        w, h, 0, 0, w, h, 0, 0, w, h, jx, jy, reset, frameMs, reason);
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

// ---------------------------------------------------------------------------
// The moving-crop probe (docs/performance.md, feature 6 and Phase 0 item 16).
//
// Feature 6 runs NVIDIA's model on a crop around the gaze point, and the
// crop moves with the eyes. NVIDIA keeps its history in output space and
// reprojects it by the motion vectors it is given, so a crop whose base
// moved by (dx, dy) since last frame shows every pixel's content (dx, dy)
// away from where the history holds it -- a uniform pan, which the pass
// can fold into the vectors. Whether the runtime treats it as one, resets,
// or smears, is decided here at the desk: a synthetic scene of fine detail
// rendered with the pass's own kind of jitter, a crop that sits still, a
// crop that moves a step a frame with the step in the vectors (both
// signs), the same with the vectors at zero, and a saccade; the crop's
// output measured against the box-filtered truth each time.
//
// Grey scene, so the truth is one number per pixel: a soft two-axis
// grating with one-pixel bright lines every sixteen -- the lines are what
// a single jittered frame gets wrong and an accumulated history gets right.
// ---------------------------------------------------------------------------
namespace {

#ifdef EDVR_HAVE_NGX

constexpr uint32_t kPW = 1280, kPH = 960;   // the frame
constexpr uint32_t kPCW = 512, kPCH = 384;  // the crop
constexpr uint32_t kPFrames = 24;
constexpr uint32_t kPBorder = 24;           // the metric's interior margin
constexpr int      kPStep = 4;              // the moving crop's step, px/frame

// The scene must be one a single jittered frame gets WRONG, or the
// history has nothing to show. A first draft drew one-pixel lines on
// pixel boundaries, which every point sample within half a pixel of the
// centre lands inside -- no aliasing at all, and the first frame beat the
// converged one. So: half-pixel lines that straddle no pixel edge (a
// point sample hits them on half the jitters and misses on the rest,
// where the box truth is half), and a grating near Nyquist, which point
// samples alias and the box filter attenuates.
float probeScene(double x, double y) {
    double v = 0.45 + 0.20 * sin(x * (6.283185307 / 23.0)) * cos(y * (6.283185307 / 17.0)) +
               0.12 * sin(x * (6.283185307 / 2.5)) * sin(y * (6.283185307 / 3.5));
    const double fx = x - 16.0 * floor(x / 16.0);
    const double fy = y - 16.0 * floor(y / 16.0);
    if ((fx >= 0.25 && fx < 0.75) || (fy >= 0.25 && fy < 0.75)) v += 0.45;
    if (v > 1.0) v = 1.0;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v);
}

double probeHalton(uint32_t i, uint32_t base) {
    double f = 1.0, r = 0.0;
    while (i > 0) {
        f /= base;
        r += f * (i % base);
        i /= base;
    }
    return r;
}

uint16_t probeHalf(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((u >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = u & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);           // flush tiny to zero
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u); // overflow to inf
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

struct ProbeReport {
    char*    buf;
    uint32_t cap;
    uint32_t len = 0;
    void line(const char* fmt, ...) {
        if (!buf || len + 2 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(buf + len, cap - len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        len += static_cast<uint32_t>(n);
        if (len + 2 < cap) {
            buf[len++] = '\n';
            buf[len] = 0;
        } else {
            len = cap - 1;
            buf[len] = 0;
        }
    }
};

struct ProbeRig {
    ID3D11Device*         dev = nullptr;
    ID3D11DeviceContext*  ctx = nullptr;
    ID3D11Texture2D*      colour = nullptr;
    ID3D11Texture2D*      depth = nullptr;
    ID3D11Texture2D*      motion = nullptr;
    ID3D11Texture2D*      output = nullptr;
    ID3D11Texture2D*      staging = nullptr;
    std::vector<uint8_t>  frames[kPFrames];   // grey, jittered
    float                 jit[kPFrames][2] = {};
    std::vector<float>    truth;
    std::vector<uint8_t>  rgba;
    std::vector<uint16_t> mv;

    ~ProbeRig() {
        if (colour) colour->Release();
        if (depth) depth->Release();
        if (motion) motion->Release();
        if (output) output->Release();
        if (staging) staging->Release();
    }

    bool make2D(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, const void* init,
                UINT pitch, ID3D11Texture2D** out) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = init;
        sd.SysMemPitch = pitch;
        return SUCCEEDED(dev->CreateTexture2D(&td, init ? &sd : nullptr, out)) && *out;
    }

    bool build(ProbeReport& rep) {
        // The scene, box-filtered (4x4) for the truth; then the frames the
        // pass would render: point samples at the pixel centre plus the
        // frame's jitter, Halton (2,3) centred, the sequence the pass uses.
        truth.resize(kPW * kPH);
        for (uint32_t y = 0; y < kPH; ++y) {
            for (uint32_t x = 0; x < kPW; ++x) {
                double s = 0.0;
                for (int sy = 0; sy < 4; ++sy) {
                    for (int sx = 0; sx < 4; ++sx) {
                        s += probeScene(x + (sx + 0.5) / 4.0, y + (sy + 0.5) / 4.0);
                    }
                }
                truth[y * kPW + x] = static_cast<float>(s / 16.0);
            }
        }
        for (uint32_t k = 0; k < kPFrames; ++k) {
            jit[k][0] = static_cast<float>(probeHalton(k + 1, 2) - 0.5);
            jit[k][1] = static_cast<float>(probeHalton(k + 1, 3) - 0.5);
            frames[k].resize(kPW * kPH);
            for (uint32_t y = 0; y < kPH; ++y) {
                for (uint32_t x = 0; x < kPW; ++x) {
                    const float v = probeScene(x + 0.5 + jit[k][0], y + 0.5 + jit[k][1]);
                    frames[k][y * kPW + x] = static_cast<uint8_t>(v * 255.0f + 0.5f);
                }
            }
        }
        rgba.resize(kPW * kPH * 4);
        mv.resize(kPW * kPH * 2);

        std::vector<float> depthInit(kPW * kPH, 0.5f);   // reversed-Z, mid-scene
        if (!make2D(kPW, kPH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, nullptr, 0,
                    &colour) ||
            !make2D(kPW, kPH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depthInit.data(),
                    kPW * 4, &depth) ||
            !make2D(kPW, kPH, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, nullptr, 0,
                    &motion) ||
            !make2D(kPW, kPH, DXGI_FORMAT_R8G8B8A8_UNORM,
                    D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0,
                    &output)) {
            rep.line("crop probe: could not create the frame textures");
            return false;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kPCW;
        td.Height = kPCH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging)) || !staging) {
            rep.line("crop probe: could not create the readback texture");
            return false;
        }
        return true;
    }

    void uploadFrame(uint32_t k) {
        const uint8_t* g = frames[k].data();
        for (uint32_t i = 0; i < kPW * kPH; ++i) {
            rgba[i * 4 + 0] = g[i];
            rgba[i * 4 + 1] = g[i];
            rgba[i * 4 + 2] = g[i];
            rgba[i * 4 + 3] = 255;
        }
        ctx->UpdateSubresource(colour, 0, nullptr, rgba.data(), kPW * 4, 0);
    }

    void uploadMotion(float mx, float my) {
        const uint16_t hx = probeHalf(mx), hy = probeHalf(my);
        for (uint32_t i = 0; i < kPW * kPH; ++i) {
            mv[i * 2 + 0] = hx;
            mv[i * 2 + 1] = hy;
        }
        ctx->UpdateSubresource(motion, 0, nullptr, mv.data(), kPW * 4, 0);
    }

    NVSDK_NGX_Handle* makeFeature() {
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = kPCW;
        cp.Feature.InHeight = kPCH;
        cp.Feature.InTargetWidth = kPCW;
        cp.Feature.InTargetHeight = kPCH;
        cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        cp.InEnableOutputSubrects = true;
        NVSDK_NGX_Handle* h = nullptr;
        const NVSDK_NGX_Result cr = NGX_D3D11_CREATE_DLSS_EXT(ctx, &h, g_params, &cp);
        if (NVSDK_NGX_FAILED(cr)) return nullptr;
        return h;
    }

    bool eval(NVSDK_NGX_Handle* h, uint32_t bx, uint32_t by, float jx, float jy, bool reset,
              NVSDK_NGX_Result* err) {
        NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
        ep.Feature.pInColor = colour;
        ep.Feature.pInOutput = output;
        ep.pInDepth = depth;
        ep.pInMotionVectors = motion;
        ep.InJitterOffsetX = jx;
        ep.InJitterOffsetY = jy;
        ep.InRenderSubrectDimensions.Width = kPCW;
        ep.InRenderSubrectDimensions.Height = kPCH;
        ep.InReset = reset ? 1 : 0;
        ep.InMVScaleX = 1.0f;
        ep.InMVScaleY = 1.0f;
        ep.InColorSubrectBase.X = bx;
        ep.InColorSubrectBase.Y = by;
        ep.InDepthSubrectBase.X = bx;
        ep.InDepthSubrectBase.Y = by;
        ep.InMVSubrectBase.X = bx;
        ep.InMVSubrectBase.Y = by;
        ep.InOutputSubrectBase.X = bx;
        ep.InOutputSubrectBase.Y = by;
        ep.InPreExposure = 1.0f;
        ep.InExposureScale = 1.0f;
        ep.InFrameTimeDeltaInMsec = 11.1f;
        const NVSDK_NGX_Result er = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, h, g_params, &ep);
        if (err) *err = er;
        return !NVSDK_NGX_FAILED(er);
    }

    // Mean absolute error of the crop's interior against the truth, in
    // 0..255 units. -1 when the readback fails.
    double measure(uint32_t bx, uint32_t by) {
        D3D11_BOX box{bx, by, 0, bx + kPCW, by + kPCH, 1};
        ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, output, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return -1.0;
        double sum = 0.0;
        uint32_t n = 0;
        for (uint32_t y = kPBorder; y < kPCH - kPBorder; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
            for (uint32_t x = kPBorder; x < kPCW - kPBorder; ++x) {
                const double got = row[x * 4] / 255.0;
                const double want = truth[(by + y) * kPW + (bx + x)];
                sum += fabs(got - want);
                ++n;
            }
        }
        ctx->Unmap(staging, 0);
        return n ? 255.0 * sum / n : -1.0;
    }

    // One condition: 24 frames from a fresh history. The crop's base
    // follows `moving` (kPStep a frame in x) and `saccade` (a jump at
    // frame 12); the vectors carry mvSign times the base's shift, in the
    // pass's own convention (previous minus current, pixels); the jitter
    // handed to NVIDIA is jSign times the offset the frame was sampled at
    // (+1 the sample offset, -1 the content's shift on screen). Errors
    // after the first frame, right after the jump (saccade only) and at
    // the end.
    bool run(bool moving, bool saccade, int mvSign, float jSign, double* eFirst,
             double* eJump, double* eEnd, ProbeReport& rep, const char* name) {
        NVSDK_NGX_Handle* h = makeFeature();
        if (!h) {
            rep.line("crop probe: %s -- the crop feature would not be created (output "
                     "sub-rectangles enabled)", name);
            return false;
        }
        uint32_t bx = moving ? 320u : 384u, by = 288u;
        uint32_t prevX = bx, prevY = by;
        *eFirst = *eJump = *eEnd = -1.0;
        float lastMx = 0.0f, lastMy = 0.0f;
        bool motionSet = false;
        bool ok = true;
        for (uint32_t k = 0; k < kPFrames && ok; ++k) {
            if (moving) bx = 320u + static_cast<uint32_t>(kPStep) * k;
            if (saccade && k == 12) {
                bx += 200u;
                by += 100u;
            }
            const float dx = static_cast<float>(static_cast<int>(bx) - static_cast<int>(prevX));
            const float dy = static_cast<float>(static_cast<int>(by) - static_cast<int>(prevY));
            const float mx = mvSign * dx, my = mvSign * dy;
            if (!motionSet || mx != lastMx || my != lastMy) {
                uploadMotion(mx, my);
                lastMx = mx;
                lastMy = my;
                motionSet = true;
            }
            uploadFrame(k);
            NVSDK_NGX_Result er = NVSDK_NGX_Result_Success;
            ok = eval(h, bx, by, jSign * jit[k][0], jSign * jit[k][1], k == 0, &er);
            if (!ok) {
                rep.line("crop probe: %s -- frame %u refused: %s (0x%08X)", name, k,
                         ngxResultName(er), static_cast<unsigned>(er));
                break;
            }
            if (k == 0) *eFirst = measure(bx, by);
            if (saccade && k == 12) *eJump = measure(bx, by);
            prevX = bx;
            prevY = by;
        }
        if (ok) *eEnd = measure(bx, by);
        NVSDK_NGX_D3D11_ReleaseFeature(h);
        return ok;
    }
};

#endif  // EDVR_HAVE_NGX

}  // namespace

int dlaaCropProbe(ID3D11Device* dev, ID3D11DeviceContext* ctx, char* report,
                  uint32_t reportBytes) {
    ProbeReport rep{report, reportBytes};
    if (report && reportBytes) report[0] = 0;
#ifndef EDVR_HAVE_NGX
    (void)dev; (void)ctx;
    rep.line("crop probe: this build has no DLSS SDK in it");
    return 0;
#else
    const char* why = nullptr;
    if (!dev || !ctx || !dlaaAvailable(dev, &why)) {
        rep.line("crop probe: DLAA is not available here (%s)", why ? why : "no device");
        return 0;
    }
    ProbeRig rig;
    rig.dev = dev;
    rig.ctx = ctx;
    if (!rig.build(rep)) return 0;
    rep.line("crop probe: a %ux%u frame, a %ux%u crop (%.0f%% of the pixels), %u frames per "
             "condition from a fresh history, Halton (2,3) jitter; error = mean |output - "
             "truth| over the crop's interior, 0..255",
             kPW, kPH, kPCW, kPCH, 100.0 * kPCW * kPCH / (kPW * kPH), kPFrames);

    double f1, j1, e1;   // still crop, jitter +
    double f2, j2, e2;   // still crop, jitter -
    if (!rig.run(false, false, 0, +1.0f, &f1, &j1, &e1, rep, "still crop, jitter +")) return 0;
    if (!rig.run(false, false, 0, -1.0f, &f2, &j2, &e2, rep, "still crop, jitter -")) return 0;
    const float jSign = (e2 < e1) ? -1.0f : +1.0f;
    // A frame sampled at x + j shows its content moved by -j, so handing
    // over -j hands over the content's shift on screen -- the convention
    // the conventions rig found NVIDIA expects and the pass ships ("as
    // passed" there is content right by jx). +j is the sample offset.
    rep.line("  still crop, jitter handed over as the sample offset:   first frame %.2f, "
             "after %u frames %.2f", f1, kPFrames, e1);
    rep.line("  still crop, jitter handed over as the content's shift: first frame %.2f, "
             "after %u frames %.2f   <- %s", f2, kPFrames, e2,
             jSign < 0 ? "WINS; the content's shift is NVIDIA's convention, and the pass's own"
                       : "loses; the SAMPLE OFFSET won, which contradicts the pass's convention");
    const double eSteady = jSign > 0 ? e1 : e2;
    const double eFirst = jSign > 0 ? f1 : f2;

    double mf, mj, mPlus, mMinus, mZero;
    if (!rig.run(true, false, +1, jSign, &mf, &mj, &mPlus, rep, "moving crop, vectors +")) return 0;
    if (!rig.run(true, false, -1, jSign, &mf, &mj, &mMinus, rep, "moving crop, vectors -")) return 0;
    if (!rig.run(true, false, 0, jSign, &mf, &mj, &mZero, rep, "moving crop, vectors 0")) return 0;
    rep.line("  moving crop, %d px/frame, vectors carry +shift: after %u frames %.2f", kPStep,
             kPFrames, mPlus);
    rep.line("  moving crop, %d px/frame, vectors carry -shift: after %u frames %.2f", kPStep,
             kPFrames, mMinus);
    rep.line("  moving crop, %d px/frame, vectors zero:         after %u frames %.2f", kPStep,
             kPFrames, mZero);
    const int mvSign = (mMinus < mPlus) ? -1 : +1;
    const double eMove = (mvSign < 0) ? mMinus : mPlus;

    double sf, sj, se;
    if (!rig.run(false, true, mvSign, jSign, &sf, &sj, &se, rep, "saccade")) return 0;
    rep.line("  saccade of 200x100 px at frame 12, vectors carry it (%s): right after %.2f, "
             "12 frames later %.2f", mvSign < 0 ? "-shift" : "+shift", sj, se);

    // Where the moved crop's error sits between a fresh history (first
    // frame) and a converged still one: near 0 is a pan, near 1 a reset
    // per move, beyond it a smear.
    const double span = eFirst - eSteady;
    if (span <= 0.5) {
        rep.line("  verdict: NOT MEASURABLE -- the still crop's history did not beat its own "
                 "first frame (%.2f vs %.2f), so the scene gave the history nothing to fix "
                 "and a moved crop cannot be placed against it. The moved crop's error was "
                 "%.2f with the shift in the vectors, %.2f without.",
                 eSteady, eFirst, eMove, mZero);
        return 4;
    }
    const double r = (eMove - eSteady) / span;
    int verdict;
    const char* word;
    if (r < 0.35) {
        verdict = 1;
        word = "PAN -- a moved crop with the shift in the vectors converges like a still one";
    } else if (r < 1.5) {
        verdict = 2;
        word = "RESET-LIKE -- a moved crop converges like a fresh history each move";
    } else {
        verdict = 3;
        word = "SMEAR -- a moved crop is worse than a fresh history";
    }
    rep.line("  verdict: %s (r = %.2f: 0 = still crop's %.2f, 1 = first frame's %.2f; the "
             "vectors' sign that wins is %s, the pass's own convention is previous minus "
             "current)",
             word, r, eSteady, eFirst, mvSign < 0 ? "-shift" : "+shift");
    return verdict;
#endif
}

// ---------------------------------------------------------------------------
// The motion probe (2026-09-05). Sean, flying the fovea with NVIDIA on both
// sides of the seam: "a blur effect in the center of my vision as I move my
// head; it takes a second, or once I stop, for it to become crisp". Full-frame
// DLAA never drew that complaint. Either the crop's feature behaves worse
// under motion than the full frame's (a sub-rectangle defect), or DLAA
// softens under motion everywhere and the full frame hid it by doing it
// uniformly while a periphery that softens less now exposes it. Reading the
// two evaluation paths cannot separate the two -- their parameters are
// identical apart from the sub-rectangles -- so the desk decides: the crop
// probe's synthetic scene PANNING at a steady speed for eighteen frames, then
// standing still for eighteen, evaluated three ways on identical inputs (the
// full frame, the fovea crop at a fixed base, and a half-size frame reduced
// the way the steady periphery is), the error in the crop's interior measured
// against the box-filtered truth frame by frame.
// ---------------------------------------------------------------------------
namespace {

#ifdef EDVR_HAVE_NGX

constexpr uint32_t kMW = 1280, kMH = 960;      // the frame
constexpr uint32_t kMCX = 384, kMCY = 288;     // the crop's base
constexpr uint32_t kMCW = 512, kMCH = 384;     // the crop
constexpr uint32_t kMBorder = 24;              // the metric's interior margin
constexpr uint32_t kMFrames = 36;
constexpr uint32_t kMMoving = 18;              // frames 1..18 pan, 19..35 stand still
constexpr int      kMV = 6;                    // the pan, px/frame at full size (even: 3 at half)
constexpr uint32_t kMExtra = kMV * kMMoving;   // the scene's extra width for the pan

struct MotionRig {
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Texture2D *colour = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr,
                    *staging = nullptr;
    ID3D11Texture2D *colourH = nullptr, *depthH = nullptr, *motionH = nullptr, *outputH = nullptr,
                    *stagingH = nullptr;
    std::vector<uint8_t>  framesF[kMFrames];   // grey, jittered, panned
    std::vector<uint8_t>  framesH[kMFrames];   // the same, boxed 2x2 (the steady periphery's input)
    float                 jit[kMFrames][2] = {};
    std::vector<float>    truthF;              // box-filtered scene, (kMW + kMExtra) x kMH
    std::vector<float>    truthH;              // ...and its 2x2 mean, half size
    std::vector<uint8_t>  rgba;
    std::vector<uint16_t> mv;

    ~MotionRig() {
        ID3D11Texture2D* all[10] = {colour, depth, motion, output, staging,
                                    colourH, depthH, motionH, outputH, stagingH};
        for (auto* t : all) if (t) t->Release();
    }

    static uint32_t shiftAt(uint32_t k) { return static_cast<uint32_t>(kMV) * (k < kMMoving ? k : kMMoving); }

    bool make2D(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, const void* init, UINT pitch,
                ID3D11Texture2D** out) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = init;
        sd.SysMemPitch = pitch;
        return SUCCEEDED(dev->CreateTexture2D(&td, init ? &sd : nullptr, out)) && *out;
    }

    bool makeStaging(uint32_t w, uint32_t h, ID3D11Texture2D** out) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, out)) && *out;
    }

    bool build(ProbeReport& rep) {
        // The scene's truth over the width the pan sweeps, box-filtered 4x4,
        // and its half-size mean. Frame k shows scene column x + shift(k) at
        // screen column x, so one static truth serves every frame.
        const uint32_t tw = kMW + kMExtra;
        truthF.resize(static_cast<size_t>(tw) * kMH);
        for (uint32_t y = 0; y < kMH; ++y) {
            for (uint32_t x = 0; x < tw; ++x) {
                double s = 0.0;
                for (int sy = 0; sy < 4; ++sy) {
                    for (int sx = 0; sx < 4; ++sx) {
                        s += probeScene(x + (sx + 0.5) / 4.0, y + (sy + 0.5) / 4.0);
                    }
                }
                truthF[static_cast<size_t>(y) * tw + x] = static_cast<float>(s / 16.0);
            }
        }
        const uint32_t twH = tw / 2, hH = kMH / 2;
        truthH.resize(static_cast<size_t>(twH) * hH);
        for (uint32_t y = 0; y < hH; ++y) {
            for (uint32_t x = 0; x < twH; ++x) {
                const float* r0 = &truthF[static_cast<size_t>(2 * y) * tw + 2 * x];
                const float* r1 = &truthF[static_cast<size_t>(2 * y + 1) * tw + 2 * x];
                truthH[static_cast<size_t>(y) * twH + x] = 0.25f * (r0[0] + r0[1] + r1[0] + r1[1]);
            }
        }
        // The frames: point samples at the pixel centre plus the jitter
        // (Halton (2,3) centred, the pass's sequence), panned; and each boxed
        // 2x2 as the steady periphery's reduction would.
        for (uint32_t k = 0; k < kMFrames; ++k) {
            jit[k][0] = static_cast<float>(probeHalton(k + 1, 2) - 0.5);
            jit[k][1] = static_cast<float>(probeHalton(k + 1, 3) - 0.5);
            const double shift = shiftAt(k);
            framesF[k].resize(static_cast<size_t>(kMW) * kMH);
            for (uint32_t y = 0; y < kMH; ++y) {
                for (uint32_t x = 0; x < kMW; ++x) {
                    const float v = probeScene(x + 0.5 + jit[k][0] + shift, y + 0.5 + jit[k][1]);
                    framesF[k][static_cast<size_t>(y) * kMW + x] = static_cast<uint8_t>(v * 255.0f + 0.5f);
                }
            }
            framesH[k].resize(static_cast<size_t>(kMW / 2) * (kMH / 2));
            for (uint32_t y = 0; y < kMH / 2; ++y) {
                for (uint32_t x = 0; x < kMW / 2; ++x) {
                    const uint8_t* r0 = &framesF[k][static_cast<size_t>(2 * y) * kMW + 2 * x];
                    const uint8_t* r1 = &framesF[k][static_cast<size_t>(2 * y + 1) * kMW + 2 * x];
                    const uint32_t s = r0[0] + r0[1] + r1[0] + r1[1];
                    framesH[k][static_cast<size_t>(y) * (kMW / 2) + x] = static_cast<uint8_t>((s + 2) / 4);
                }
            }
        }
        rgba.resize(static_cast<size_t>(kMW) * kMH * 4);
        mv.resize(static_cast<size_t>(kMW) * kMH * 2);

        std::vector<float> depthInit(static_cast<size_t>(kMW) * kMH, 0.5f);   // reversed-Z, mid-scene
        const UINT rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (!make2D(kMW, kMH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, nullptr, 0, &colour) ||
            !make2D(kMW, kMH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depthInit.data(), kMW * 4, &depth) ||
            !make2D(kMW, kMH, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, nullptr, 0, &motion) ||
            !make2D(kMW, kMH, DXGI_FORMAT_R8G8B8A8_UNORM, rw, nullptr, 0, &output) ||
            !make2D(kMW / 2, kMH / 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, nullptr, 0, &colourH) ||
            !make2D(kMW / 2, kMH / 2, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depthInit.data(), (kMW / 2) * 4, &depthH) ||
            !make2D(kMW / 2, kMH / 2, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, nullptr, 0, &motionH) ||
            !make2D(kMW / 2, kMH / 2, DXGI_FORMAT_R8G8B8A8_UNORM, rw, nullptr, 0, &outputH)) {
            rep.line("motion probe: could not create the frame textures");
            return false;
        }
        if (!makeStaging(kMCW, kMCH, &staging) || !makeStaging(kMCW / 2, kMCH / 2, &stagingH)) {
            rep.line("motion probe: could not create the readback textures");
            return false;
        }
        return true;
    }

    void uploadFrame(bool half, uint32_t k) {
        const uint32_t w = half ? kMW / 2 : kMW, h = half ? kMH / 2 : kMH;
        const uint8_t* g = (half ? framesH[k] : framesF[k]).data();
        for (uint32_t i = 0; i < w * h; ++i) {
            rgba[i * 4 + 0] = g[i];
            rgba[i * 4 + 1] = g[i];
            rgba[i * 4 + 2] = g[i];
            rgba[i * 4 + 3] = 255;
        }
        ctx->UpdateSubresource(half ? colourH : colour, 0, nullptr, rgba.data(), w * 4, 0);
    }

    void uploadMotion(bool half, float mx, float my) {
        const uint32_t w = half ? kMW / 2 : kMW, h = half ? kMH / 2 : kMH;
        const uint16_t hx = probeHalf(mx), hy = probeHalf(my);
        for (uint32_t i = 0; i < w * h; ++i) {
            mv[i * 2 + 0] = hx;
            mv[i * 2 + 1] = hy;
        }
        ctx->UpdateSubresource(half ? motionH : motion, 0, nullptr, mv.data(), w * 4, 0);
    }

    // mode 0: the full frame (no sub-rectangles); 1: the fovea crop (a
    // feature of the crop's size, output sub-rectangles, a fixed base); 2:
    // the half-size frame (the steady periphery's DLAA); 3: DLSS Performance
    // from the half-size frame to the full size (the dlss fovea's mode). The
    // preset is the render-preset hint for the mode's quality, set on the
    // shared parameter block before the create (0 = the driver's default);
    // the caller restores the defaults when its sweep is done.
    NVSDK_NGX_Handle* makeFeature(int mode, unsigned preset) {
        const bool perf = mode == 3;
        NVSDK_NGX_DLSS_Create_Params cp{};
        const uint32_t w = mode == 1 ? kMCW : (mode == 2 || perf) ? kMW / 2 : kMW;
        const uint32_t h = mode == 1 ? kMCH : (mode == 2 || perf) ? kMH / 2 : kMH;
        cp.Feature.InWidth = w;
        cp.Feature.InHeight = h;
        cp.Feature.InTargetWidth = perf ? kMW : w;
        cp.Feature.InTargetHeight = perf ? kMH : h;
        cp.Feature.InPerfQualityValue =
            perf ? NVSDK_NGX_PerfQuality_Value_MaxPerf : NVSDK_NGX_PerfQuality_Value_DLAA;
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        cp.InEnableOutputSubrects = mode == 1;
        g_params->Set(perf ? NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance
                           : NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                      preset);
        NVSDK_NGX_Handle* hnd = nullptr;
        const NVSDK_NGX_Result cr = NGX_D3D11_CREATE_DLSS_EXT(ctx, &hnd, g_params, &cp);
        if (NVSDK_NGX_FAILED(cr)) return nullptr;
        return hnd;
    }

    bool eval(int mode, NVSDK_NGX_Handle* h, float jx, float jy, bool reset, NVSDK_NGX_Result* err) {
        const bool halfIn = mode == 2 || mode == 3;   // the half-size inputs
        NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
        ep.Feature.pInColor = halfIn ? colourH : colour;
        ep.Feature.pInOutput = mode == 2 ? outputH : output;   // Performance answers at the full size
        ep.pInDepth = halfIn ? depthH : depth;
        ep.pInMotionVectors = halfIn ? motionH : motion;
        ep.InJitterOffsetX = jx;
        ep.InJitterOffsetY = jy;
        ep.InRenderSubrectDimensions.Width = mode == 1 ? kMCW : halfIn ? kMW / 2 : kMW;
        ep.InRenderSubrectDimensions.Height = mode == 1 ? kMCH : halfIn ? kMH / 2 : kMH;
        ep.InReset = reset ? 1 : 0;
        ep.InMVScaleX = 1.0f;
        ep.InMVScaleY = 1.0f;
        if (mode == 1) {
            ep.InColorSubrectBase.X = kMCX;
            ep.InColorSubrectBase.Y = kMCY;
            ep.InDepthSubrectBase.X = kMCX;
            ep.InDepthSubrectBase.Y = kMCY;
            ep.InMVSubrectBase.X = kMCX;
            ep.InMVSubrectBase.Y = kMCY;
            ep.InOutputSubrectBase.X = kMCX;
            ep.InOutputSubrectBase.Y = kMCY;
        }
        ep.InPreExposure = 1.0f;
        ep.InExposureScale = 1.0f;
        ep.InFrameTimeDeltaInMsec = 11.1f;
        const NVSDK_NGX_Result er = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, h, g_params, &ep);
        if (err) *err = er;
        return !NVSDK_NGX_FAILED(er);
    }

    // Mean absolute error over the crop's interior (at half size, the same
    // region at half the coordinates) against frame k's truth, 0..255.
    double measure(int mode, uint32_t k) {
        const bool half = mode == 2;
        const uint32_t d = half ? 2u : 1u;
        const uint32_t bx = kMCX / d, by = kMCY / d, cw = kMCW / d, ch = kMCH / d, bd = kMBorder / d;
        const uint32_t tw = (kMW + kMExtra) / d;
        const uint32_t shift = shiftAt(k) / d;
        const std::vector<float>& truth = half ? truthH : truthF;
        ID3D11Texture2D* stg = half ? stagingH : staging;
        D3D11_BOX box{bx, by, 0, bx + cw, by + ch, 1};
        ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, half ? outputH : output, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) return -1.0;
        double sum = 0.0;
        uint32_t n = 0;
        for (uint32_t y = bd; y < ch - bd; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
            for (uint32_t x = bd; x < cw - bd; ++x) {
                const double got = row[x * 4] / 255.0;
                const double want = truth[static_cast<size_t>(by + y) * tw + (bx + x + shift)];
                sum += fabs(got - want);
                ++n;
            }
        }
        ctx->Unmap(stg, 0);
        return n ? 255.0 * sum / n : -1.0;
    }

    // One series: 36 frames from a fresh history, the pan in frames 1..18
    // and the vectors carrying mvSign times its per-frame shift (previous
    // minus current is the pass's convention: content that moved left by v
    // was at x + v last frame), the jitter handed over with jSign; the error
    // after every frame.
    bool run(int mode, int mvSign, float jSign, double err[kMFrames], ProbeReport& rep,
             const char* name, unsigned preset = 0) {
        NVSDK_NGX_Handle* h = makeFeature(mode, preset);
        if (!h) {
            rep.line("motion probe: %s -- the feature would not be created", name);
            return false;
        }
        const bool half = mode == 2 || mode == 3;   // half-size inputs: half the pan, half the jitter
        const float v = half ? kMV * 0.5f : static_cast<float>(kMV);
        const float js = half ? 0.5f * jSign : jSign;
        float lastMx = 1e9f;
        bool ok = true;
        for (uint32_t k = 0; k < kMFrames && ok; ++k) {
            const bool moving = k >= 1 && k <= kMMoving;
            const float mx = moving ? mvSign * v : 0.0f;
            if (mx != lastMx) {
                uploadMotion(half, mx, 0.0f);
                lastMx = mx;
            }
            uploadFrame(half, k);
            NVSDK_NGX_Result er = NVSDK_NGX_Result_Success;
            ok = eval(mode, h, js * jit[k][0], js * jit[k][1], k == 0, &er);
            if (!ok) {
                rep.line("motion probe: %s -- frame %u refused: %s (0x%08X)", name, k,
                         ngxResultName(er), static_cast<unsigned>(er));
                break;
            }
            err[k] = measure(mode, k);
        }
        NVSDK_NGX_D3D11_ReleaseFeature(h);
        return ok;
    }
};

double motionMean(const double* e, uint32_t a, uint32_t b) {
    double s = 0.0;
    uint32_t n = 0;
    for (uint32_t k = a; k <= b && k < kMFrames; ++k) {
        if (e[k] >= 0.0) {
            s += e[k];
            ++n;
        }
    }
    return n ? s / n : -1.0;
}

#endif  // EDVR_HAVE_NGX

}  // namespace

int dlaaMotionProbe(ID3D11Device* dev, ID3D11DeviceContext* ctx, char* report,
                    uint32_t reportBytes) {
    ProbeReport rep{report, reportBytes};
    if (report && reportBytes) report[0] = 0;
#ifndef EDVR_HAVE_NGX
    (void)dev; (void)ctx;
    rep.line("motion probe: this build has no DLSS SDK in it");
    return 0;
#else
    const char* why = nullptr;
    if (!dev || !ctx || !dlaaAvailable(dev, &why)) {
        rep.line("motion probe: DLAA is not available here (%s)", why ? why : "no device");
        return 0;
    }
    MotionRig rig;
    rig.dev = dev;
    rig.ctx = ctx;
    if (!rig.build(rep)) return 0;
    rep.line("motion probe: a %ux%u frame panning %d px/frame for frames 1..%u, still after; "
             "error = mean |output - truth| over the %ux%u crop's interior at (%u,%u), 0..255; "
             "the half-size series is measured against its own half-size truth",
             kMW, kMH, kMV, kMMoving, kMCW, kMCH, kMCX, kMCY);

    // The signs, decided by the full frame itself: the vectors' sign that
    // converges better under the pan, then the jitter's if the rest error
    // does not beat the first frame (the crop probe found the content's
    // shift, -j, to be NVIDIA's convention for these frames).
    double fPlus[kMFrames], fMinus[kMFrames];
    float jSign = -1.0f;
    if (!rig.run(0, +1, jSign, fPlus, rep, "full frame, vectors +")) return 0;
    if (!rig.run(0, -1, jSign, fMinus, rep, "full frame, vectors -")) return 0;
    int mvSign = motionMean(fMinus, 8, kMMoving - 1) < motionMean(fPlus, 8, kMMoving - 1) ? -1 : +1;
    double* full = mvSign < 0 ? fMinus : fPlus;
    if (motionMean(full, 30, 35) >= full[0] * 0.9) {
        // Try the other jitter sign before giving up on the scene.
        double gPlus[kMFrames], gMinus[kMFrames];
        if (!rig.run(0, +1, +1.0f, gPlus, rep, "full frame, vectors +, jitter +")) return 0;
        if (!rig.run(0, -1, +1.0f, gMinus, rep, "full frame, vectors -, jitter +")) return 0;
        const int mv2 = motionMean(gMinus, 8, kMMoving - 1) < motionMean(gPlus, 8, kMMoving - 1) ? -1 : +1;
        double* full2 = mv2 < 0 ? gMinus : gPlus;
        if (motionMean(full2, 30, 35) < motionMean(full, 30, 35)) {
            jSign = +1.0f;
            mvSign = mv2;
            memcpy(fPlus, gPlus, sizeof(fPlus));
            memcpy(fMinus, gMinus, sizeof(fMinus));
            full = mvSign < 0 ? fMinus : fPlus;
        }
    }
    rep.line("  signs: vectors carry %s the pan (previous minus current is the pass's convention), "
             "jitter handed over as %s",
             mvSign > 0 ? "+" : "-", jSign < 0 ? "the content's shift (-j)" : "the sample offset (+j)");

    double crop[kMFrames], half[kMFrames];
    if (!rig.run(1, mvSign, jSign, crop, rep, "fovea crop")) return 0;
    if (!rig.run(2, mvSign, jSign, half, rep, "half-size frame")) return 0;

    const uint32_t cols[12] = {0, 2, 5, 8, 11, 14, 17, 18, 19, 21, 25, 35};
    char line[512];
    int n = snprintf(line, sizeof(line), "  frame:            ");
    for (uint32_t c : cols) n += snprintf(line + n, sizeof(line) - n, "%6u", c);
    rep.line("%s", line);
    const char* names[3] = {"  full-frame DLAA:  ", "  fovea crop DLAA:  ", "  half-size DLAA:   "};
    const double* series[3] = {full, crop, half};
    for (int s = 0; s < 3; ++s) {
        n = snprintf(line, sizeof(line), "%s", names[s]);
        for (uint32_t c : cols) n += snprintf(line + n, sizeof(line) - n, "%6.2f", series[s][c]);
        rep.line("%s", line);
    }
    rep.line("  (frames 1..%u pan; %u is the first still frame)", kMMoving, kMMoving + 1);

    const double fullMove = motionMean(full, 8, kMMoving - 1), fullEarly = motionMean(full, 19, 21),
                 fullRest = motionMean(full, 28, 35);
    const double cropMove = motionMean(crop, 8, kMMoving - 1), cropEarly = motionMean(crop, 19, 21),
                 cropRest = motionMean(crop, 28, 35);
    const double halfMove = motionMean(half, 8, kMMoving - 1), halfRest = motionMean(half, 28, 35);
    rep.line("  steady pan (frames 8-17) / first frames still (19-21) / at rest (28-35): full %.2f / %.2f / %.2f, "
             "crop %.2f / %.2f / %.2f, half %.2f / - / %.2f",
             fullMove, fullEarly, fullRest, cropMove, cropEarly, cropRest, halfMove, halfRest);
    if (fullRest > 0.0 && cropRest > 0.0 && halfRest > 0.0) {
        rep.line("  softening under the pan (pan error / rest error): full %.2fx, crop %.2fx, half-size %.2fx",
                 fullMove / fullRest, cropMove / cropRest, halfMove / halfRest);
    }

    // The presets. The softening under motion is the model's, and NVIDIA
    // ships several: the render-preset hint picks one per quality mode. DLAA
    // defaults to K and Performance (the dlss fovea's mode) to M, so in the
    // dlss configuration the fovea and the periphery already run different
    // networks. Each preset under the same pan, for DLAA on the full frame
    // and for Performance from the half-size frame measured against the
    // full-size truth. A refused preset is reported and skipped; the shared
    // block's hints go back to the defaults after, so production is untouched.
    struct PresetCase { unsigned id; const char* name; };
    const PresetCase dlaaPresets[5] = {{0, "default (K)"}, {10, "J"}, {11, "K"},
                                       {5, "E (CNN, deprecated)"}, {6, "F (CNN, deprecated)"}};
    const PresetCase perfPresets[5] = {{0, "default (M)"}, {11, "K"}, {10, "J"}, {13, "M"}, {12, "L"}};
    rep.line("  presets, full-frame DLAA -- pan 8-17 / first still 19-21 / rest 28-35 / softening / "
             "first frame:");
    for (const PresetCase& pc : dlaaPresets) {
        double e[kMFrames];
        for (double& v : e) v = -1.0;
        if (rig.run(0, mvSign, jSign, e, rep, pc.name, pc.id)) {
            const double pm = motionMean(e, 8, kMMoving - 1), pe = motionMean(e, 19, 21), pr = motionMean(e, 28, 35);
            rep.line("    %-22s %.2f / %.2f / %.2f / %.2fx / %.2f", pc.name, pm, pe, pr,
                     pr > 0.0 ? pm / pr : 0.0, e[0]);
        }
    }
    rep.line("  presets, DLSS Performance 2x from the half-size frame (the dlss fovea's mode), against "
             "the full-size truth:");
    for (const PresetCase& pc : perfPresets) {
        double e[kMFrames];
        for (double& v : e) v = -1.0;
        if (rig.run(3, mvSign, jSign, e, rep, pc.name, pc.id)) {
            const double pm = motionMean(e, 8, kMMoving - 1), pe = motionMean(e, 19, 21), pr = motionMean(e, 28, 35);
            rep.line("    %-22s %.2f / %.2f / %.2f / %.2fx / %.2f", pc.name, pm, pe, pr,
                     pr > 0.0 ? pm / pr : 0.0, e[0]);
        }
    }
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, 0u);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, 0u);

    if (fullRest < 0.0 || fullRest >= full[0] * 0.9) {
        rep.line("  verdict: NOT MEASURABLE -- the full frame's rest error (%.2f) did not beat its "
                 "first frame (%.2f)", fullRest, full[0]);
        return 4;
    }
    const double rMove = cropMove / fullMove, rEarly = cropEarly / fullEarly, rRest = cropRest / fullRest;
    int verdict;
    const char* word;
    if (rMove > 1.2) {
        verdict = 2;
        word = "THE CROP IS SOFTER UNDER MOTION than the full frame on the same input -- a sub-rectangle defect";
    } else if (rEarly > 1.2) {
        verdict = 3;
        word = "THE CROP RECOVERS SLOWER after the pan stops than the full frame -- a sub-rectangle defect";
    } else {
        verdict = 1;
        word = "THE CROP MATCHES THE FULL FRAME under motion and after it; any softening seen is the "
               "model's own, which a full frame does everywhere at once and a fixed periphery makes "
               "visible by comparison";
    }
    rep.line("  verdict: %s (crop/full: pan %.2f, first still frames %.2f, rest %.2f)", word, rMove,
             rEarly, rRest);
    return verdict;
#endif
}

// The fovea path's desk check (the review of 2026-09-05, F1/F12): a solid
// colour run through dlaaEvaluateFovea must come back as ITSELF inside the
// crop (1:1, not upscaled or refused) and leave the output OUTSIDE the crop
// untouched. This is the test that would have caught the feature-created-at-
// full-size bug before a flight. Returns 1 pass, 0 fail (*why names it), -1
// skip (no runtime). Its own textures, released before it returns.
#ifdef EDVR_HAVE_NGX
// One fovea case: a solid colour at the input crop, evaluated to the output
// crop of a magenta-filled output; the output crop must come back the colour
// (1:1 or upscaled, a solid stays a solid) and the rest untouched.
static bool foveaCase(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT inW, UINT inH,
                      UINT outW, UINT outH, UINT icx, UINT icy, UINT icw, UINT ich, UINT ocx,
                      UINT ocy, UINT ocw, UINT och, bool periphery, const char** why) {
    auto mk = [&](UINT tw, UINT th, DXGI_FORMAT fmt, UINT bind, const void* init, UINT pitch,
                  ID3D11Texture2D** out) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = init; sd.SysMemPitch = pitch;
        return SUCCEEDED(dev->CreateTexture2D(&td, init ? &sd : nullptr, out)) && *out;
    };
    std::vector<uint8_t> colA(static_cast<size_t>(inW) * inH * 4);
    std::vector<uint8_t> outB(static_cast<size_t>(outW) * outH * 4);
    for (size_t i = 0; i < static_cast<size_t>(inW) * inH; ++i) {
        colA[i * 4 + 0] = 90; colA[i * 4 + 1] = 160; colA[i * 4 + 2] = 200; colA[i * 4 + 3] = 255;
    }
    for (size_t i = 0; i < static_cast<size_t>(outW) * outH; ++i) {
        outB[i * 4 + 0] = 255; outB[i * 4 + 1] = 0; outB[i * 4 + 2] = 255; outB[i * 4 + 3] = 255;
    }
    std::vector<float> depth(static_cast<size_t>(inW) * inH, 0.5f);
    std::vector<uint16_t> motion(static_cast<size_t>(inW) * inH * 2, 0);
    ID3D11Texture2D *col = nullptr, *dep = nullptr, *mot = nullptr, *out = nullptr, *stg = nullptr;
    bool ok = mk(inW, inH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, colA.data(), inW * 4, &col) &&
              mk(inW, inH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth.data(), inW * 4, &dep) &&
              mk(inW, inH, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, motion.data(), inW * 4, &mot) &&
              mk(outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM,
                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, outB.data(), outW * 4, &out);
    if (ok) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = outW; td.Height = outH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &stg)) && stg;
    }
    if (!ok && why) *why = "the test textures could not be created";
    const char* wf = "";
    for (int k = 0; ok && k < 2; ++k) {
        const bool ran = periphery
            ? dlaaEvaluatePeriphery(ctx, 0, col, dep, mot, out, inW, inH, 0.0f, 0.0f, k == 0, 0.0f, &wf)
            : dlssEvaluateFovea(ctx, 0, col, dep, mot, out, inW, inH, outW, outH, icx, icy, icw, ich,
                                ocx, ocy, ocw, och, 0.0f, 0.0f, k == 0, 0.0f, &wf);
        if (!ran) {
            if (why) *why = wf;   // an NGX rejection (0xBAD00005) lands here
            ok = false;
        }
    }
    uint8_t centre[4] = {}, outside[4] = {};
    if (ok) {
        ctx->CopyResource(stg, out);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
            auto rd = [&](UINT x, UINT y, uint8_t* o) {
                const uint8_t* r = static_cast<const uint8_t*>(m.pData) +
                                   static_cast<size_t>(y) * m.RowPitch + static_cast<size_t>(x) * 4;
                o[0] = r[0]; o[1] = r[1]; o[2] = r[2]; o[3] = r[3];
            };
            rd(ocx + ocw / 2, ocy + och / 2, centre);
            rd(5, 5, outside);
            ctx->Unmap(stg, 0);
        } else {
            if (why) *why = "the output could not be read back";
            ok = false;
        }
    }
    if (ok) {
        auto approx = [](uint8_t v, int t) { const int d = static_cast<int>(v) - t; return d > -8 && d < 8; };
        if (!(approx(centre[0], 90) && approx(centre[1], 160) && approx(centre[2], 200))) {
            if (why) *why = "the output crop is not the source colour -- NVIDIA refused the crop "
                            "or wrote it in the wrong place";
            ok = false;
        } else if (periphery) {
            // The periphery is the whole frame: its corner must be the colour too.
            if (!(approx(outside[0], 90) && approx(outside[1], 160) && approx(outside[2], 200))) {
                if (why) *why = "the periphery's corner is not the source colour -- the whole-frame "
                                "DLAA did not cover the frame";
                ok = false;
            }
        } else if (!(approx(outside[0], 255) && approx(outside[1], 0) && approx(outside[2], 255))) {
            if (why) *why = "the output OUTSIDE the crop was overwritten -- the crop is not "
                            "confined to its sub-rectangle";
            ok = false;
        }
    }
    if (stg) stg->Release();
    if (out) out->Release();
    if (mot) mot->Release();
    if (dep) dep->Release();
    if (col) col->Release();
    return ok;
}
#endif

int dlaaFoveaSelfTest(ID3D11Device* dev, ID3D11DeviceContext* ctx, const char** why) {
#ifndef EDVR_HAVE_NGX
    (void)dev; (void)ctx;
    if (why) *why = "this build has no DLSS SDK in it";
    return -1;
#else
    const char* w0 = "";
    if (!dev || !ctx || !dlaaAvailable(dev, &w0)) {
        if (why) *why = w0[0] ? w0 : "no device";
        return -1;
    }
    // The 1:1 crop (DLAA): input crop == output crop, same-size textures.
    if (!foveaCase(dev, ctx, 512, 384, 512, 384, 128, 64, 256, 256, 128, 64, 256, 256, false, why)) {
        return 0;
    }
    // The upscale crop (DLSS, the half-render variant): a 256x256 input crop of
    // a small render becomes a 512x512 output crop of a 2x native frame (ratio
    // 0.5 -> MaxPerf).
    if (!foveaCase(dev, ctx, 512, 384, 1024, 768, 128, 64, 256, 256, 256, 128, 512, 512, false, why)) {
        return 0;
    }
    // A non-half ratio (0.6 -> Balanced), so a second DLSS mode's create is
    // exercised too (the review of 2026-09-05, F1/F3): 300x300 input crop of a
    // 640x480 render, 500x500 output crop of a 1067x800 frame.
    if (!foveaCase(dev, ctx, 640, 480, 1068, 800, 160, 90, 300, 300, 268, 150, 500, 500, false, why)) {
        return 0;
    }
    // The steady periphery's slot: whole-frame DLAA on a reduced copy, at a
    // mid size and at the smallest the smoke harness's 400x304 source reduces
    // to (NVIDIA must accept a 200x152 DLAA feature, or the desk pipeline case
    // would pass vacuously).
    if (!foveaCase(dev, ctx, 512, 384, 512, 384, 0, 0, 512, 384, 0, 0, 512, 384, true, why)) {
        return 0;
    }
    if (!foveaCase(dev, ctx, 200, 152, 200, 152, 0, 0, 200, 152, 0, 0, 200, 152, true, why)) {
        return 0;
    }
    return 1;
#endif
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

// For tools/smoke: the motion probe (dlaa.h): the crop against the full
// frame and a half-size frame under a steady pan, frame by frame.
extern "C" __declspec(dllexport) int edvrDlaaMotionProbe(void* device, void* context, char* report,
                                                         unsigned reportBytes) {
    return edvr::dlaaMotionProbe(static_cast<ID3D11Device*>(device),
                                 static_cast<ID3D11DeviceContext*>(context), report, reportBytes);
}

// For tools/smoke: the moving-crop probe (dlaa.h). The verdict comes back
// and the report is written for the harness to print.
extern "C" __declspec(dllexport) int edvrDlaaCropProbe(void* device, void* context, char* report,
                                                       unsigned reportBytes) {
    return edvr::dlaaCropProbe(static_cast<ID3D11Device*>(device),
                               static_cast<ID3D11DeviceContext*>(context), report, reportBytes);
}

// For tools/smoke: the fovea path's 1:1 crop check (the review of 2026-09-05).
// 1 pass, 0 fail (*why names it), -1 skip (no runtime).
extern "C" __declspec(dllexport) int edvrDlaaFoveaCheck(void* device, void* context,
                                                        const char** why) {
    return edvr::dlaaFoveaSelfTest(static_cast<ID3D11Device*>(device),
                                   static_cast<ID3D11DeviceContext*>(context), why);
}
