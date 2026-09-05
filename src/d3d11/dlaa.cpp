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

// For tools/smoke: the moving-crop probe (dlaa.h). The verdict comes back
// and the report is written for the harness to print.
extern "C" __declspec(dllexport) int edvrDlaaCropProbe(void* device, void* context, char* report,
                                                       unsigned reportBytes) {
    return edvr::dlaaCropProbe(static_cast<ID3D11Device*>(device),
                               static_cast<ID3D11DeviceContext*>(context), report, reportBytes);
}
