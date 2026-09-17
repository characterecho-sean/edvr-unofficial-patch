#include "fsr3_engine.h"

#include <cstring>

#include <windows.h>
#include <d3d11.h>

#include "../common/config.h"
#include "../common/log.h"

// Bare `/D EDVR_HAVE_FSR3` (no value, matching how a future build.bat block
// might be typed beside NGX's `/DEDVR_HAVE_NGX=1`, build.bat:300) would make
// `#if EDVR_HAVE_FSR3` below a syntax error rather than a clean 0. Pin it.
#ifndef EDVR_HAVE_FSR3
#define EDVR_HAVE_FSR3 0
#endif

#if EDVR_HAVE_FSR3
// AMD's FidelityFX Super Resolution 3.1, the community Direct3D 11 port
// (metarutaiga, hardened by OptiScaler; MIT; design doc section 2), fetched
// into third_party\ffx-dx11\ by tools\fetch_ffx_dx11.py and linked only
// when build.bat's own EDVR_HAVE_FSR3 block (not yet written -- Track C)
// found it. TODO(Track C): the port's own headers, roughly
//   #include "ffx_fsr3upscaler.h"
//   #include "dx11/ffx_dx11.h"
// plus whatever ffxGetInterfaceDX11 needs.
#endif

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

#if EDVR_HAVE_FSR3
// TODO(Track C): g_ctx[2], one FfxFsr3UpscalerContext per eye, keyed on
// (w, h, outW, outH) exactly as dlaa.cpp's ensureFeature keys NGX (design
// doc 3.2) -- recreate on a key change, reset the pass's continuity on
// create. A scratch buffer sized for two contexts via
// ffxGetScratchMemorySizeDX11(2), and ffxGetInterfaceDX11 once per session
// on the game's device.
#endif

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
    // TODO(Track C): mirror dlaaAvailable (dlaa.cpp:441-525) -- feature
    // level 11_0 or better, typed UAV loads per
    // D3D11_FEATURE_D3D11_OPTIONS2 (design doc 3.2: cards without them,
    // believed NVIDIA before Maxwell, must get a clean refusal here, not a
    // crash inside the port), ffxGetInterfaceDX11 once per session, timed
    // and logged the way dlaa: first asked for... is.
    (void)dev;
    if (!g_tried) {
        g_tried = true;
        g_available = false;
        g_reason = "AMD's upscaler is not yet implemented in this build (Track C)";
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
    // TODO(Track C): mirror dlaaWarm (dlaa.cpp:527-561) -- fsr3Available,
    // then make the (w,h)->(outW,outH) context so the first treated frame
    // finds it made.
    (void)ctx; (void)w; (void)h; (void)outW; (void)outH;
    if (createMs) *createMs = 0.0;
    if (why) *why = "AMD's upscaler is not yet implemented in this build (Track C)";
    return false;
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
    // TODO(Track C): the input mapping is the design doc's table, 3.3 --
    // color/depth/motionVectors/jitterOffset/reactive/reset/frameTimeDelta
    // as dlaaEvaluate's own inputs, plus cameraNear/cameraFar (nearZ/farZ,
    // which of the reversed pair FSR calls near is read off the port's own
    // header comment at build time), cameraFovAngleVertical = fovY,
    // viewSpaceToMetersFactor = 1 (Elite's units are metres),
    // enableSharpening = false, no HIGH_DYNAMIC_RANGE, no
    // DISPLAY_RESOLUTION, no JITTER_CANCELLATION, no AUTO_EXPOSURE.
    // ffxFsr3UpscalerContextDispatch into `out`. Time it with the same
    // GpuTimer machinery dlaa.cpp's evaluateCrop uses, folding into
    // g_evaluations/g_resets/g_timeSum/g_timeCount/g_timeMax below so
    // fsr3Totals reports honestly once this runs for real.
    (void)ctx; (void)eye; (void)colour; (void)depth; (void)mv; (void)reactive; (void)out;
    (void)w; (void)h; (void)outW; (void)outH; (void)jx; (void)jy; (void)reset; (void)frameMs;
    (void)nearZ; (void)farZ; (void)fovY;
    if (why) *why = "AMD's upscaler is not yet implemented in this build (Track C)";
    return false;
#endif
}

void fsr3ReleaseFeatures() {
#if EDVR_HAVE_FSR3
    // TODO(Track C): destroy g_ctx[2] (a size or engine change mid-session:
    // the trim and HMD Quality can both change the output size live, and a
    // switch away from fsr should not leave AMD's contexts allocated for a
    // session that no longer uses them).
#endif
}

void fsr3Shutdown() {
#if EDVR_HAVE_FSR3
    fsr3ReleaseFeatures();
    // TODO(Track C): ffxGetInterfaceDX11's teardown, if any is needed.
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
    // TODO(Track C): the port's own version, once linked; "fsr 3.1.2" per
    // the design doc's pinned FidelityFX SDK tag until the port names itself.
    return "fsr 3.1.2";
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

}  // namespace edvr
