#include "dlaa.h"
#include "dlss_floor.h"  // dlssModeRanges, defined below beside dlaaAvailable
#include "hdr_backend_flags.h"  // the HDR route's creation flags, pure (section 81)

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include "../common/log.h"
#include "../common/runtime_profile.h"  // the flat HDR route's fixed exposure (section 106)
#include "flat_hdr_crumbs.h"   // the flat HDR route's crash-safe breadcrumbs around the feature's steps
#include "perf_monitor.h"   // the feature's creation is an event with a duration
#include "gpu_timing.h"
#include "gpu_adapter_name.h"  // adapterName -- shared with fsr3_engine.cpp

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
bool     g_maskNoted = false; // the bias mask's arrival, said once
uint32_t g_timeCount = 0;
double   g_timeSum = 0.0;
double   g_timeMax = 0.0;

// Per-role, per-eye price, alongside the pooled figures above (which stay
// as they are for their existing callers): which of the three independent
// NGX features -- the full frame, the fovea's centre crop, or the steady
// periphery -- paid for a given evaluation, so the totals line can show a
// real per-eye number instead of a pooled average mislabeled as one.
// Declared unconditionally, like the pooled figures, so the accessors
// below compile with no DLSS SDK in the build too (they just never see a
// count).
enum class DlaaRole { Full = 0, Centre = 1, Periphery = 2 };
constexpr int kDlaaRoles = 3;
struct DlaaRoleStats {
    uint32_t count = 0;
    double   sum = 0.0;
    double   maxMs = 0.0;
};
// Indexed by upscaler slot (dlaa.h, kUpscalerSlots): the eyes' two and the VR world route's third. Only the full-frame role
// ever counts on the world's slot; the centre and periphery roles are the eyes' (dlaaEvaluateFovea, dlaaEvaluatePeriphery
// refuse slot 2), so their third column stays empty.
DlaaRoleStats g_roleStats[kDlaaRoles][kUpscalerSlots];

// Whether a role has this slot at all: the full frame on every slot, the other two on the eyes' only.
bool roleHasSlot(DlaaRole role, int slot) {
    return role == DlaaRole::Full ? upscalerSlotHasFullFrame(slot) : upscalerSlotHasFoveatedRoles(slot);
}

// The measured price for one role and eye, for the totals line. False when
// that role/eye combination has not evaluated yet.
bool roleTotals(DlaaRole role, int eye, uint32_t* evaluations, double* avgMs, double* maxMs) {
    if (!roleHasSlot(role, eye)) return false;
    const DlaaRoleStats& rs = g_roleStats[static_cast<int>(role)][eye];
    if (rs.count == 0) return false;
    if (evaluations) *evaluations = rs.count;
    if (avgMs) *avgMs = rs.sum / static_cast<double>(rs.count);
    if (maxMs) *maxMs = rs.maxMs;
    return true;
}

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

// Stage 0 price report (docs/foveated-dlss-design-2026-09-14.md): the
// environment stamp's one new fact, off adapterName (gpu_adapter_name.h,
// shared with fsr3_engine.cpp -- there is no reason to ask the adapter
// twice in a session, whichever engine asks first). Driver version and the
// DLSS runtime's own version/hash are not read here; the OpenXR startup
// lines already name the runtime and headset, so those are not repeated
// either.

struct EyeFeature {
    NVSDK_NGX_Handle* handle = nullptr;
    uint32_t          w = 0, h = 0;
    uint32_t          outW = 0, outH = 0;
    uint64_t          presetGen = 0;   // the preset generation the feature was created under
    // The flat HDR route's input is HDR with automatic exposure (section 81). The flags are creation-time, so
    // the feature key carries the bit: a route flip remakes the feature and its history starts again.
    bool              hdr = false;
};

// The flag set is hdr_backend_flags.h's, pure so a rig can pin it; every constant is checked against the SDK's own
// enum here, so a drift in either fails this compile and not a flight.
static_assert(kDlssFlagIsHdr == static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_IsHDR), "IsHDR bit");
static_assert(kDlssFlagMvLowRes == static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes), "MVLowRes bit");
static_assert(kDlssFlagMvJittered == static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVJittered), "MVJittered bit");
static_assert(kDlssFlagDepthInverted == static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted), "DepthInverted bit");
static_assert(kDlssFlagAutoExposure == static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure), "AutoExposure bit");
static_assert(flatDlssCreateFlags(false, true) == flatDlssCreateFlags(false, false), "the LDR set has no profile");
static_assert(flatDlssCreateFlags(false, true) == static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                                                   NVSDK_NGX_DLSS_Feature_Flags_DepthInverted),
              "the LDR flag set is what it always was");

// The render preset -- NVIDIA's model -- set by dlaaSetPreset from the
// config and applied to the shared parameter block before each feature is
// created, per ROLE: g_preset for the full frame and the periphery (every
// mode), g_presetFovea for the fovea crop when it upscales (Balanced,
// Performance, Ultra Performance). 0 = the driver's own choice per mode (K
// for DLAA, Quality and Balanced, M for Performance, L for Ultra
// Performance). A change bumps the generation, so live features are
// recreated. The desk (2026-09-05): M is far behind K at rest on fine detail
// (a rest error 2.7x K's); under motion L softens least while K reconstructs
// most at rest; and the cost probe priced the models at the Crystal Super's
// sizes -- on the full frame K, L, M and J cost the same within 7% (the
// price is the output's), under Performance L costs 1.8x K, and under DLAA L
// costs FIVE times K. So: K everywhere but the fovea's upscaling crop, which
// gets L (its faster convergence from fresh content is what a crop needs,
// and a crop is small enough that L's price does not matter); never L or M
// under DLAA.
unsigned g_preset = 11;        // the full frame's and the periphery's, every mode
unsigned g_presetFovea = 12;   // the fovea crop's, when it upscales
uint64_t g_presetGen = 1;
// The flat HDR route's exposure input (section 106): NVIDIA's "1x1 texture containing the final exposure scale",
// R32_FLOAT 1.0, made on the first such evaluation. H is pre-tonemap radiance the game's own tone pass exposes later.
ID3D11Texture2D* g_exposureOne = nullptr;

const char* presetName(unsigned p) {
    switch (p) {
        case 0:  return "the driver's default for the mode";
        case 5:  return "E";
        case 6:  return "F";
        case 10: return "J";
        case 11: return "K";
        case 12: return "L";
        case 13: return "M";
        default: return "?";
    }
}

// L and M are upscaling models: under DLAA L cost five times K on the desk.
unsigned dlaaSafe(unsigned p) { return (p == 12 || p == 13) ? 11u : p; }

// The hints for a feature about to be created: `upscaleP` for the upscaling
// modes, g_preset for DLAA and the Quality modes.
void applyPresetHints(unsigned upscaleP) {
    if (!g_params) return;
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, dlaaSafe(g_preset));
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, dlaaSafe(g_preset));
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, dlaaSafe(g_preset));
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, upscaleP);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, upscaleP);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, upscaleP);
}
void applyPresetHints() { applyPresetHints(g_preset); }

// The preset a feature of this quality and role runs under, for the log.
unsigned presetFor(NVSDK_NGX_PerfQuality_Value q, bool fovea = false) {
    if (q == NVSDK_NGX_PerfQuality_Value_DLAA || q == NVSDK_NGX_PerfQuality_Value_MaxQuality ||
        q == NVSDK_NGX_PerfQuality_Value_UltraQuality) {
        return dlaaSafe(g_preset);
    }
    return fovea ? g_presetFovea : g_preset;
}

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
// The full-frame features, one per upscaler slot (dlaa.h, kUpscalerSlots): the eyes' two, which dlaaWarm makes on the loading
// screen, and the VR world route's third (slot 2), made lazily by the first evaluation on it and released with the others.
EyeFeature g_feature[kUpscalerSlots];
// The fovea features, kept apart from the full-frame ones: created with
// output sub-rectangles enabled and their own history, so switching the
// fovea on or off never disturbs the full-frame path's accumulation. The eyes'
// only (kUpscalerEyeSlots): the VR world's slot has no fovea.
EyeFeature g_fovea[kUpscalerEyeSlots];
// The steady periphery's features (docs/performance.md feature 6): DLAA on
// a reduced copy of the frame, a third slot with its own history, so the
// fovea, the periphery and the full frame never share an accumulation. The
// eyes' only, as the fovea's are.
EyeFeature g_periph[kUpscalerEyeSlots];

// The GPU-price ring, the resolve's discipline: never awaited.
struct QuerySlot {
    GpuTimer     timer;
    bool         inUse = false;
    DlaaRole     role = DlaaRole::Full;   // which feature this sample prices
    int          eye = 0;                 // the upscaler slot: 0 left, 1 right, 2 the VR world route's (full frame only)
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
        if (roleHasSlot(q.role, q.eye)) {
            DlaaRoleStats& rs = g_roleStats[static_cast<int>(q.role)][q.eye];
            ++rs.count;
            rs.sum += ms;
            if (ms > rs.maxMs) rs.maxMs = ms;
        }
    }
}

int acquireQuerySlot(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!ctx || !dev) return -1;
    if (!gpuTimingAccepts(ctx) && !gpuTimingBind(dev, ctx)) return -1;
    for (int i = 0; i < kQueryRing; ++i) {
        QuerySlot& q = g_qring[i];
        if (q.inUse) continue;
        if (q.timer.begin(dev, ctx)) { q.inUse = true; return i; }
        return -1; // Shared clock pressure cannot be fixed by trying another free slot.
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

// DlssMode/DlssModeRange/dlssModeByRatio/dlssChooseMode live in dlaa.h,
// SDK-free, so tools/dlaa_mode_test can drive the selection with no NGX
// and no device. kNgxLadder is the one place that maps a DlssMode index
// to NVIDIA's own enum, in the ladder's long-standing order (largest
// render fraction first).
constexpr NVSDK_NGX_PerfQuality_Value kNgxLadder[kDlssModeCount] = {
    NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced,
    NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance};

// The output size this file last printed the mode table for; (0,0)
// initially, so the first ladder walk always logs. Not per-eye -- the
// query depends only on the output size, so both eyes share one line,
// and a preset-only change (same output) does not repeat it.
uint32_t g_modesLoggedW = 0, g_modesLoggedH = 0;

// Point 1 of the 2026-09-23 hardening: whatever the selection below does
// with these four answers, name them all, once per output size, so a
// refusal is always explained after the fact rather than needing a second
// flight with better logging. The 2026-09-23 flight refused a 1229x1412
// input against a 3070x3032 output having printed only that one refusal's
// own (empty) range; this line would have shown whether ultra
// performance's query had failed or its range genuinely stopped short.
void logDlssModesOnce(uint32_t outW, uint32_t outH, const DlssModeRange modes[kDlssModeCount],
                      const unsigned errCodes[kDlssModeCount]) {
    if (outW == g_modesLoggedW && outH == g_modesLoggedH) return;
    g_modesLoggedW = outW;
    g_modesLoggedH = outH;
    char line[320];
    size_t used = 0;
    for (int k = 0; k < kDlssModeCount && used < sizeof(line); ++k) {
        const DlssModeRange& m = modes[k];
        const int n = m.ok
            ? snprintf(line + used, sizeof(line) - used, "%s%s %ux%u (%ux%u..%ux%u)",
                      k ? ", " : "", kDlssModeNames[k], m.optW, m.optH, m.minW, m.minH, m.maxW,
                      m.maxH)
            : snprintf(line + used, sizeof(line) - used, "%s%s query failed (0x%08X)",
                      k ? ", " : "", kDlssModeNames[k], errCodes[k]);
        if (n > 0) used += static_cast<size_t>(n);
    }
    Log::get().note("dlss: modes for %ux%u: %s", outW, outH, line);
}

// The full-frame feature for one upscaler slot (an eye's, or the VR world route's third): made when it is missing or its key
// (the sizes, the preset generation, the route's bit) has moved, left alone otherwise. Each slot keeps its own key, its own
// handle and so its own history; the preset generation is shared, so dlaaSetPreset remakes every slot's feature. The
// ONE block dlaaEvaluate and dlaaWarm share, so what the warm-up makes on
// the loading screen is exactly what the first evaluation would have made,
// and that evaluation finds it and skips the create (or recreates on a
// mismatch, as it always did). createMs is the create's own duration,
// zero when nothing was made.
bool ensureFeature(ID3D11DeviceContext* ctx, int eye, uint32_t w, uint32_t h,
                   uint32_t outW, uint32_t outH, const char** reason, double* createMs,
                   bool hdr = false) {
    if (createMs) *createMs = 0.0;
    if (!upscalerSlotHasFullFrame(eye)) {
        if (reason) *reason = "an upscaler slot out of range";
        return false;
    }
    EyeFeature& f = g_feature[eye];
    if (!f.handle || f.w != w || f.h != h || f.outW != outW || f.outH != outH ||
        f.presetGen != g_presetGen || f.hdr != hdr) {
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
        // The flat HDR route's crumbs (flat_hdr_crumbs.h): the runtime's own questions about render sizes, then the
        // feature's creation and (in dlaaEvaluate) its evaluation, each bracketed where only the route's bit writes.
        HdrCrumbSpan query(hdr, "backend-query", "in=%ux%u out=%ux%u", w, h, outW, outH);
        if (outW == w && outH == h) {
            optErr = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_caps, w, h, quality, &optW, &optH, &maxW,
                                                   &maxH, &minW, &minH, &sharpness);
            optKnown = !NVSDK_NGX_FAILED(optErr);
        } else {
            DlssModeRange modes[kDlssModeCount];
            unsigned errCodes[kDlssModeCount] = {};
            for (int k = 0; k < kDlssModeCount; ++k) {
                unsigned oW2 = 0, oH2 = 0, mxW = 0, mxH = 0, mnW = 0, mnH = 0;
                float sh = 0.0f;
                // Never breaks on a failed query (the 2026-09-23 hardening):
                // one bad query used to end the walk here and hide every
                // mode below it on the ladder -- see docs/anti-aliasing.md.
                optErr = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_caps, outW, outH, kNgxLadder[k], &oW2,
                                                       &oH2, &mxW, &mxH, &mnW, &mnH, &sh);
                errCodes[k] = static_cast<unsigned>(optErr);
                modes[k].ok = !NVSDK_NGX_FAILED(optErr);
                if (modes[k].ok) {
                    optKnown = true;
                    modes[k].optW = oW2; modes[k].optH = oH2;
                    modes[k].minW = mnW; modes[k].minH = mnH;
                    modes[k].maxW = mxW; modes[k].maxH = mxH;
                    modes[k].sharpness = sh;
                }
            }
            // Logged once per output size, whatever the pick below does
            // with it, so a refusal is always explained after the fact.
            logDlssModesOnce(outW, outH, modes, errCodes);

            DlssMode picked = DlssMode::Quality;
            bool fromRange = false;
            DlssModeRange chosenRange;
            if (dlssChooseMode(modes, w, h, outW, &picked, &fromRange, &chosenRange)) {
                quality = kNgxLadder[static_cast<int>(picked)];
                if (fromRange) {
                    optW = chosenRange.optW; optH = chosenRange.optH;
                    minW = chosenRange.minW; minH = chosenRange.minH;
                    maxW = chosenRange.maxW; maxH = chosenRange.maxH;
                    sharpness = chosenRange.sharpness;
                }
            } else {
                // Reached only when all four queries answered and none
                // holds the input (build point 2's third case) -- the four
                // ranges go straight into the reason so the log explains
                // the refusal without a second flight for better logging.
                char ranges[224];
                size_t used = 0;
                for (int k = 0; k < kDlssModeCount && used < sizeof(ranges); ++k) {
                    const DlssModeRange& m = modes[k];
                    const int n = snprintf(ranges + used, sizeof(ranges) - used,
                                           "%s%s %ux%u..%ux%u", k ? ", " : "", kDlssModeNames[k],
                                           m.minW, m.minH, m.maxW, m.maxH);
                    if (n > 0) used += static_cast<size_t>(n);
                }
                snprintf(g_reasonBuf, sizeof(g_reasonBuf),
                         "a %ux%u frame sits outside every DLSS mode's render range for a "
                         "%ux%u output (%s)",
                         w, h, outW, outH, ranges);
                g_reason = g_reasonBuf;
                if (reason) *reason = g_reason;
                return false;
            }
        }
        query.result("ok=%u ngx=0x%08X", optKnown ? 1u : 0u, static_cast<unsigned>(optErr));
        query.close();
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
        // LDR colour (HDR on the HDR route: fixed exposure in flat, automatic in the VR world route; hdr_backend_flags.h);
        // motion vectors at the render size, unjittered (the pass computes them on the unjittered
        // grid); reversed-Z depth.
        cp.InFeatureCreateFlags = static_cast<int>(flatDlssCreateFlags(hdr, runtimeFlatProfile()));
        cp.InEnableOutputSubrects = false;
        applyPresetHints();
        // An event with a duration for the monitor's drop attribution: the
        // feature's creation is the mod's own heaviest one-off on the render
        // thread, and it recurs at every size change.
        HdrCrumbSpan create(hdr, "backend-create", "ngx feature in=%ux%u out=%ux%u quality=%d flags=0x%X", w, h, outW, outH,
                            static_cast<int>(quality), static_cast<unsigned>(cp.InFeatureCreateFlags));
        const int64_t createT0 = qpcNow();
        const NVSDK_NGX_Result cr =
            NGX_D3D11_CREATE_DLSS_EXT(ctx, &f.handle, g_params, &cp);
        const double ms = qpcFrequency() > 0
                              ? static_cast<double>(qpcNow() - createT0) * 1000.0 /
                                    static_cast<double>(qpcFrequency())
                              : 0.0;
        create.result("ngx=0x%08X handle=%u", static_cast<unsigned>(cr), f.handle ? 1u : 0u);
        create.close();   // after the duration above is taken: the crumb's own write is not part of the creation's time
        perfMonitorNoteEvent(kEvNgx, ms);
        if (createMs) *createMs = ms;
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
        f.presetGen = g_presetGen;
        f.hdr = hdr;
        if (hdr)
            Log::get().note("dlss: the feature for %s was created for the flat HDR route: HDR input, exposure %s; the "
                            "history starts here.", upscalerSlotLabel(eye),
                            flatDlssFixedExposure(hdr, runtimeFlatProfile())
                                ? "fixed at 1.0 (IsHDR with MVLowRes | DepthInverted, no AutoExposure; a 1x1 exposure texture)"
                                : "automatic (IsHDR | AutoExposure with MVLowRes | DepthInverted)");
        // Build point 5, 2026-09-23: a create success resets the shared
        // reason, so a caller that reads it later (dlaaAvailable's *reason,
        // which just echoes g_reason once NGX has initialised) is not shown
        // a stale refusal from a size this eye no longer has. Without this,
        // a ladder-walk refusal at one output size could outlive its own
        // cause and still be quoted after a later size change fixed it.
        g_reason = "available";
        if (outW == w && outH == h) {
            Log::get().note(
                "dlaa: the feature is created for %s at %ux%u, DLAA, preset %s (the "
                "runtime's optimal render size for this output %ux%u, which DLAA ignores); "
                "the history starts here (made in %.0f ms).",
                upscalerSlotLabel(eye), w, h, presetName(presetFor(quality)), optW, optH, ms);
        } else {
            Log::get().note(
                "dlss: the feature is created for %s, %ux%u in and %ux%u out (%.0f%% "
                "per axis), the %s mode, preset %s, whose own render size is %ux%u and whose "
                "range the runtime names as %ux%u..%ux%u; the history starts here (made in %.0f ms).",
                upscalerSlotLabel(eye), w, h, outW, outH,
                100.0 * static_cast<double>(w) / static_cast<double>(outW),
                qualityName(quality), presetName(presetFor(quality)), optW, optH, minW, minH, maxW, maxH, ms);
        }
    }
    return true;
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
        // Stamped wherever the first ask runs -- the loading-screen warm-up
        // (temporal_pass.cpp, warmTrainedOnce) or the first treat -- so the
        // log prices NGX's own initialisation apart from the feature creates.
        const int64_t t0 = qpcNow();
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
        const double ms = qpcFrequency() > 0
                              ? static_cast<double>(qpcNow() - t0) * 1000.0 /
                                    static_cast<double>(qpcFrequency())
                              : 0.0;
        // Stage 0 price report: the environment stamp, once, whatever the
        // answer above -- a refusal is still evidence, and is diagnosed by
        // the GPU it was refused on. The runtime name, headset and refresh
        // rate are already on record, but in the OpenXR module's own
        // edvr_openxr_*.log, not this one; not repeated here. The driver
        // version and the DLSS runtime's own version are not read anywhere.
        Log::get().note(
            "dlaa: first asked for on %s. Driver version and the DLSS "
            "runtime's own version are not read by this build; the OpenXR "
            "runtime name, headset and refresh rate are in that module's "
            "own log, not this one.",
            adapterName(dev));
        if (g_available) {
            Log::get().note("dlaa: NGX initialised in %.0f ms on device %p.", ms, (void*)dev);
        } else {
            Log::get().note("dlaa: NGX refused after %.0f ms: %s", ms, g_reason);
        }
    }
    if (reason) *reason = g_reason;
    return g_available;
#endif
}

// dlss_floor.h: the four ranges for an output, asked BEFORE that output is
// (the door's served floor, native_temporal.cpp). The same query and ladder
// ensureFeature walks, answered and logged the same way; nothing is chosen
// here, and ensureFeature's own walk and selection are untouched.
bool dlssModeRanges(ID3D11Device* dev, uint32_t outW, uint32_t outH,
                    DlssModeRange modes[kDlssModeCount], bool quiet) {
    for (int k = 0; k < kDlssModeCount; ++k) modes[k] = DlssModeRange{};
#ifndef EDVR_HAVE_NGX
    (void)dev;
    (void)outW;
    (void)outH;
    return false;
#else
    if (!outW || !outH || !dlaaAvailable(dev, nullptr) || !g_caps) return false;
    unsigned errCodes[kDlssModeCount] = {};
    bool any = false;
    for (int k = 0; k < kDlssModeCount; ++k) {
        unsigned oW = 0, oH = 0, mxW = 0, mxH = 0, mnW = 0, mnH = 0;
        float sh = 0.0f;
        const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
            g_caps, outW, outH, kNgxLadder[k], &oW, &oH, &mxW, &mxH, &mnW, &mnH, &sh);
        errCodes[k] = static_cast<unsigned>(r);
        modes[k].ok = !NVSDK_NGX_FAILED(r);
        if (!modes[k].ok) continue;
        any = true;
        modes[k].optW = oW; modes[k].optH = oH;
        modes[k].minW = mnW; modes[k].minH = mnH;
        modes[k].maxW = mxW; modes[k].maxH = mxH;
        modes[k].sharpness = sh;
    }
    if (!quiet) logDlssModesOnce(outW, outH, modes, errCodes);
    return any;
#endif
}

bool dlaaWarm(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, bool features,
              double* initMs, double createMs[2], const char** reason) {
#ifndef EDVR_HAVE_NGX
    (void)ctx; (void)w; (void)h; (void)features;
    if (initMs) *initMs = 0.0;
    if (createMs) createMs[0] = createMs[1] = 0.0;
    if (reason) *reason = "this build has no DLSS SDK in it";
    return false;
#else
    if (initMs) *initMs = 0.0;
    if (createMs) createMs[0] = createMs[1] = 0.0;
    if (!ctx || !w || !h) {
        if (reason) *reason = "a missing input";
        return false;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    const int64_t t0 = qpcNow();
    const bool ok = dlaaAvailable(dev, reason);   // ~0 ms when already asked
    if (initMs) {
        *initMs = qpcFrequency() > 0
                      ? static_cast<double>(qpcNow() - t0) * 1000.0 /
                            static_cast<double>(qpcFrequency())
                      : 0.0;
    }
    // NVIDIA's ceiling (dlss_floor.h): a size it names no usable range for has no 1:1 feature either -- the 2026-10-09 flight's
    // 8268x3948 failed its create -- so the warm-up stands aside there, quietly. The first upscaled frame cuts its output to the
    // ceiling and makes its own feature; the loading frames at this size stand aside too (temporal_pass.cpp).
    if (ok && features && dev) {
        DlssModeRange modes[kDlssModeCount];
        if (!dlssModeRanges(dev, w, h, modes) || !dlssRangesAnswered(modes)) features = false;
    }
    if (dev) dev->Release();
    if (!ok) return false;
    if (!features) return true;
    for (int eye = 0; eye < 2; ++eye) {
        if (!ensureFeature(ctx, eye, w, h, w, h, reason, createMs ? &createMs[eye] : nullptr)) {
            return false;
        }
    }
    return true;
#endif
}

bool dlaaEvaluate(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                  ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                  ID3D11Texture2D* output, ID3D11Texture2D* reactive,
                  uint32_t w, uint32_t h,
                  uint32_t outW, uint32_t outH, float jx, float jy, bool reset,
                  float frameMs, const char** reason, bool hdr) {
#ifndef EDVR_HAVE_NGX
    (void)ctx; (void)eye; (void)colour; (void)depth; (void)motion; (void)output;
    (void)reactive;
    (void)w; (void)h; (void)outW; (void)outH; (void)jx; (void)jy; (void)reset;
    (void)frameMs; (void)hdr;
    if (reason) *reason = "this build has no DLSS SDK in it";
    return false;
#else
    if (!upscalerSlotHasFullFrame(eye)) {
        if (reason) *reason = "an upscaler slot out of range";
        return false;
    }
    if (!g_available || !g_params || !g_caps || !ctx || !colour || !depth || !motion || !output ||
        !w || !h) {
        if (reason) *reason = g_available ? "a missing input" : g_reason;
        return false;
    }
    pollTimingRing(ctx);
    if (!outW || !outH) {
        outW = w;
        outH = h;
    }
    // The feature: found made (by the warm-up or a previous frame) or made
    // here, through the one block the warm-up shares (ensureFeature).
    if (!ensureFeature(ctx, eye, w, h, outW, outH, reason, nullptr, hdr)) return false;
    EyeFeature& f = g_feature[eye];

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = colour;
    ep.Feature.pInOutput = output;
    ep.Feature.InSharpness = 0.0f;
    ep.pInDepth = depth;
    ep.pInMotionVectors = motion;
    // The bias-current-colour mask, when a caller has one. Null is the
    // shipped state and the same as a mask of zeroes.
    ep.pInBiasCurrentColorMask = reactive;
    if (reactive && !g_maskNoted) {
        g_maskNoted = true;
        Log::get().note("dlaa: a bias-current-colour mask is being handed to NVIDIA. "
                        "Only preset F supports this input; modern presets use EDVR's separate UI resolve.");
    }
    ep.InJitterOffsetX = jx;
    ep.InJitterOffsetY = jy;
    ep.InRenderSubrectDimensions.Width = w;
    ep.InRenderSubrectDimensions.Height = h;
    ep.InReset = reset ? 1 : 0;
    ep.InMVScaleX = 1.0f;
    ep.InMVScaleY = 1.0f;
    ep.InPreExposure = 1.0f;
    ep.InExposureScale = 1.0f;
    if (flatDlssFixedExposure(hdr, runtimeFlatProfile())) {
        if (!g_exposureOne) {
            ID3D11Device* exposureDev = nullptr;
            ctx->GetDevice(&exposureDev);
            if (exposureDev) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_FLOAT;
                td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                const float one = 1.0f;
                D3D11_SUBRESOURCE_DATA init{&one, sizeof(one), 0};
                const HRESULT hrExposure = exposureDev->CreateTexture2D(&td, &init, &g_exposureOne);
                if (FAILED(hrExposure))
                    Log::get().note("dlss: the fixed exposure texture (1x1 R32_FLOAT = 1.0) was refused (hr=0x%08X); "
                                    "the evaluation runs without it", static_cast<unsigned>(hrExposure));
                exposureDev->Release();
            }
        }
        ep.pInExposureTexture = g_exposureOne;
    }
    // The frame delta the SDK asks for ("helps in determining the amount
    // to denoise or anti-alias based on the speed of the object"); zero
    // when unknown, which the runtime treats as unstated.
    ep.InFrameTimeDeltaInMsec = frameMs;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    const int qs = dev ? acquireQuerySlot(dev, ctx) : -1;
    if (dev) dev->Release();
    if (qs >= 0) { g_qring[qs].role = DlaaRole::Full; g_qring[qs].eye = eye; }
    HdrCrumbSpan evaluate(hdr, "backend-evaluate", "ngx in=%ux%u out=%ux%u reset=%u", w, h, outW, outH, reset ? 1u : 0u);
    const NVSDK_NGX_Result er = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, f.handle, g_params, &ep);
    evaluate.result("ngx=0x%08X", static_cast<unsigned>(er));
    evaluate.close();
    if (qs >= 0) g_qring[qs].timer.end(ctx); // Poll consumes failed End samples too.
    if (NVSDK_NGX_FAILED(er)) {
        ID3D11Device* failedDevice = nullptr;
        ctx->GetDevice(&failedDevice);
        if (failedDevice) {
            Log::get().note("dlaa: NVIDIA evaluation failed (0x%08X), "
                            "GetDeviceRemovedReason=0x%08X on device %p.",
                            static_cast<unsigned>(er),
                            static_cast<unsigned>(failedDevice->GetDeviceRemovedReason()),
                            (void*)failedDevice);
            failedDevice->Release();
        }
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
// frame -- the difference the crop probe (since removed) validated and the
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
    // Hoisted out of the create-block below: needed again at the eval call
    // site, well past where that block ends, to tag the timing sample.
    const bool isFovea = what[0] == 'f';
    pollTimingRing(ctx);
    bool didCreate = false;
    if (!f.handle || f.w != icw || f.h != ich || f.outW != ocw || f.outH != och ||
        f.presetGen != g_presetGen) {
        if (f.handle) {
            NVSDK_NGX_D3D11_ReleaseFeature(f.handle);
            f.handle = nullptr;
        }
        // Equal crops are DLAA. A smaller input picks the quality mode by the
        // FRAME's upscale ratio, not the crop's -- the frame ratio is the same
        // for both eyes and stable, so the eyes never land on different DLSS
        // networks across a 0.5/0.58/0.667 threshold the way the per-eye crop
        // ratio does when rounding straddles it (the review of 2026-09-05,
        // F1). dlssModeByRatio (dlaa.h) is the one ratio rule in the file now
        // (build point 4, 2026-09-23) -- ensureFeature's own range-unknown
        // fallback calls the same helper, so a given ratio can no longer pick
        // different modes depending which call site saw it.
        NVSDK_NGX_PerfQuality_Value q = NVSDK_NGX_PerfQuality_Value_DLAA;
        if (inW != outW || inH != outH) {
            q = kNgxLadder[static_cast<int>(dlssModeByRatio(inW, outW))];
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
        applyPresetHints(isFovea ? g_presetFovea : g_preset);
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
        f.presetGen = g_presetGen;
        didCreate = true;
        // Same reset as ensureFeature's (build point 5): this crop
        // feature's own create succeeded, so the shared reason must not
        // go on quoting an older refusal.
        g_reason = "available";
        Log::get().note(
            "temporal aa %s: NVIDIA's feature is created for eye %d, crop %ux%u in -> "
            "%ux%u out (%s, preset %s, output sub-rectangles; input based at %u,%u in the "
            "%ux%u render, output at %u,%u in the %ux%u frame); its history starts here.",
            what, eye, icw, ich, ocw, och, (icw == ocw && ich == och) ? "DLAA" : qualityName(q),
            presetName(presetFor(q, isFovea)), icx, icy, inW, inH, ocx, ocy, outW, outH);
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
    const int qs = dev ? acquireQuerySlot(dev, ctx) : -1;
    if (dev) dev->Release();
    if (qs >= 0) {
        g_qring[qs].role = isFovea ? DlaaRole::Centre : DlaaRole::Periphery;
        g_qring[qs].eye = eye;
    }
    const NVSDK_NGX_Result er = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, f.handle, g_params, &ep);
    if (qs >= 0) g_qring[qs].timer.end(ctx); // Poll consumes failed End samples too.
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
    if (!upscalerSlotHasFoveatedRoles(eye)) {
        // The VR world's slot (2) is a real slot with no fovea; anything else is no slot at all.
        if (reason) *reason = upscalerSlotHasFullFrame(eye) ? "this upscaler slot has no fovea" : "a missing input";
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
    if (!upscalerSlotHasFoveatedRoles(eye)) {
        if (reason) *reason = upscalerSlotHasFullFrame(eye) ? "this upscaler slot has no periphery" : "a missing input";
        return false;
    }
    return evaluateCrop(g_periph[eye], "periphery", eye, ctx, colour, depth, motion, output, w, h,
                        w, h, 0, 0, w, h, 0, 0, w, h, jx, jy, reset, frameMs, reason);
#endif
}

void dlaaSetPreset(unsigned preset, unsigned foveaPreset) {
#ifdef EDVR_HAVE_NGX
    if (preset != g_preset || foveaPreset != g_presetFovea) {
        g_preset = preset;
        g_presetFovea = foveaPreset;
        ++g_presetGen;   // every live feature is recreated on its next evaluation
    }
#else
    (void)preset;
    (void)foveaPreset;
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

// The measured price for one role, both eyes visible to the caller by
// asking twice: the full frame's own NGX feature (every mode), the fovea's
// centre crop, and the steady periphery, each with its own history and so
// its own price. False when that role/eye has not evaluated yet -- the
// pooled dlaaTotals above stays as the fallback for callers that only want
// one number.
bool dlaaFullTotals(int eye, uint32_t* evaluations, double* avgMs, double* maxMs) {
    return roleTotals(DlaaRole::Full, eye, evaluations, avgMs, maxMs);
}
bool dlaaCentreTotals(int eye, uint32_t* evaluations, double* avgMs, double* maxMs) {
    return roleTotals(DlaaRole::Centre, eye, evaluations, avgMs, maxMs);
}
bool dlaaPeripheryTotals(int eye, uint32_t* evaluations, double* avgMs, double* maxMs) {
    return roleTotals(DlaaRole::Periphery, eye, evaluations, avgMs, maxMs);
}

void dlaaShutdown() {
#ifdef EDVR_HAVE_NGX
    releaseFeatures();
    if (g_exposureOne) { g_exposureOne->Release(); g_exposureOne = nullptr; }
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
