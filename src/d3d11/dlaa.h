// DLAA at the door -- NVIDIA's trained temporal anti-aliasing (DLSS with
// no upscaling), fed by the plumbing this branch built for its own pass.
//
// The own pass proved the inputs (docs/anti-aliasing.md): the jitter goes
// through the projection edit, the head's pose delta and the scene's
// depth register the cockpit (three docked flights, 2026-09-03), and the
// reprojection of every pixel is a motion vector by another name. DLSS
// consumes exactly that set -- colour, depth, per-pixel motion, the
// jitter offset -- and replaces the hand-rolled history clip with a
// trained one, which is where text goes from "registered" to "steady".
// The player asked for it after the twelfth build; the rig's GPU is an
// RTX 5090 (measured, Win32_VideoController).
//
// This file is the NGX glue and nothing else. It compiles with the calls
// only when the build has NVIDIA's SDK (EDVR_HAVE_NGX, set by build.bat
// when EDVR_NGX_SDK names the SDK's directory); without it, every entry
// answers "not built in" and the temporal pass runs its own history as
// before. The runtime DLL (nvngx_dlss.dll) must sit beside the game's
// executable, where NGX looks for it; its absence is also a plain answer.
#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

// The upscaler feature slots (dlaa.cpp's g_feature, fsr3_engine.cpp's g_ctx): each slot keeps its own feature, its own size
// key and its own history, so two callers never share an accumulation. Slots 0 and 1 are the eyes' (slot 0 is also the flat
// profile's); slot 2 is the VR world route's (vr_world_route.h, kVrWorldFeatureSlot). The eyes have every role (the full
// frame, the fovea's centre crop, the steady periphery); the world has the full frame only, so the foveated entry points
// refuse slot 2. fsr3_engine.h takes this constant from here. Pure, like the mode ladder below: tools\dlaa_mode_test pins it.
constexpr uint32_t kUpscalerSlots = 3;
constexpr uint32_t kUpscalerEyeSlots = 2;
inline bool upscalerSlotHasFullFrame(int slot) {
    return slot >= 0 && static_cast<uint32_t>(slot) < kUpscalerSlots;
}
inline bool upscalerSlotHasFoveatedRoles(int slot) {
    return slot >= 0 && static_cast<uint32_t>(slot) < kUpscalerEyeSlots;
}
// How a slot is named in a log line. The eyes keep the words every log has always carried ("eye 0"), so the lines slots 0 and
// 1 print are the lines they printed before the third slot existed.
inline const char* upscalerSlotLabel(int slot) {
    switch (slot) {
        case 0:  return "eye 0";
        case 1:  return "eye 1";
        case 2:  return "the VR world (upscaler slot 2)";
        default: return "an unknown upscaler slot";
    }
}

// The DLSS quality-mode ladder's pure decision, factored out of the NGX
// glue below so a rig with no SDK and no device (tools/dlaa_mode_test)
// can drive it directly: four modes, largest render fraction first,
// matching the order NVIDIA's own enum has always been walked in.
// docs/anti-aliasing.md, "DLSS mode selection hardened (2026-09-23)".
enum class DlssMode : int { Quality = 0, Balanced = 1, Performance = 2, UltraPerformance = 3 };
constexpr int kDlssModeCount = 4;
constexpr const char* kDlssModeNames[kDlssModeCount] = {"quality", "balanced", "performance",
                                                         "ultra performance"};

// One mode's answer to NGX_DLSS_GET_OPTIMAL_SETTINGS for a given output
// size: `ok` false means the query itself failed and the rest of the
// struct is unset. min/max are the INPUT render size this mode accepts,
// inclusive at both ends; opt is the runtime's own recommended render
// size for this mode at this output; sharpness is its suggestion, kept
// only for parity with the DLAA-equal-size query (nothing downstream
// reads it, same as before this hardening).
struct DlssModeRange {
    bool     ok = false;
    unsigned optW = 0, optH = 0;
    unsigned minW = 0, minH = 0;
    unsigned maxW = 0, maxH = 0;
    float    sharpness = 0.0f;
};

// The render-ratio fallback shared by every selection site that cannot
// trust a range query -- ensureFeature when NGX will not name ranges at
// all, or named an incomplete ladder, and evaluateCrop's per-frame crop
// mode, which is never range-queried. One helper, one set of thresholds:
// before the 2026-09-23 hardening these were two separate copies (0.66
// with no epsilon in one, 0.667 with +0.002 in the other) that could pick
// different modes for the same ratio. The epsilon keeps an exact half on
// Performance rather than Ultra Performance (a 1/2-scale input is not the
// 1/3-scale mode; the evaluateCrop review of 2026-09-05, F1).
inline DlssMode dlssModeByRatio(unsigned w, unsigned outW) {
    if (!outW) return DlssMode::UltraPerformance;
    const float ratio = static_cast<float>(w) / static_cast<float>(outW) + 0.002f;
    return ratio >= 0.667f ? DlssMode::Quality
         : ratio >= 0.58f  ? DlssMode::Balanced
         : ratio >= 0.5f   ? DlssMode::Performance
         :                   DlssMode::UltraPerformance;
}

// Chooses a mode from four already-queried ranges (ensureFeature now
// queries all four every time -- see the 2026-09-23 entry in
// docs/anti-aliasing.md: a `break` on the first failed query used to hide
// every mode below it on the ladder, and cost a flight where ultra
// performance should have held a 1229x1412 input against a 3070x3032
// output but was, on the evidence available, never even tried). In order:
//   1. Among modes whose [min,max] holds w x h (>= min, <= max on both
//      axes -- exactly-at-minimum stays in that mode), the one whose own
//      optimal render size is nearest w wins.
//   2. Otherwise, if every query succeeded, the ladder is a complete and
//      honest "no": returns false (a real refusal).
//   3. Otherwise -- no mode held it AND at least one query failed -- the
//      ladder is incomplete, not a real refusal: the nearest mode by
//      ratio is picked and NGX's own create call decides (its failure
//      path already logs the NGX result).
// On a pick, *fromRange says whether it came from rule 1 (*range is that
// mode's own queried data) or rule 3 (a ratio guess: *range is zeroed,
// since nothing about it is actually known).
inline bool dlssChooseMode(const DlssModeRange modes[kDlssModeCount], unsigned w, unsigned h,
                           unsigned outW, DlssMode* chosen, bool* fromRange,
                           DlssModeRange* range) {
    int best = -1;
    unsigned bestDiff = ~0u;
    bool anyFailed = false;
    for (int k = 0; k < kDlssModeCount; ++k) {
        const DlssModeRange& m = modes[k];
        if (!m.ok) { anyFailed = true; continue; }
        const bool inRange = w >= m.minW && h >= m.minH && w <= m.maxW && h <= m.maxH;
        const unsigned diff = m.optW > w ? m.optW - w : w - m.optW;
        if (inRange && diff < bestDiff) { best = k; bestDiff = diff; }
    }
    if (best >= 0) {
        if (chosen) *chosen = static_cast<DlssMode>(best);
        if (fromRange) *fromRange = true;
        if (range) *range = modes[best];
        return true;
    }
    if (!anyFailed) return false;
    if (chosen) *chosen = dlssModeByRatio(w, outW);
    if (fromRange) *fromRange = false;
    if (range) *range = DlssModeRange{};
    return true;
}

// Is DLAA usable on this device? Initialises NGX on the first ask (once
// per session, whatever the answer) and says why not when it is not:
// the reason is a static string for the log. Cheap after the first call.
bool dlaaAvailable(ID3D11Device* dev, const char** reason);

// The loading-screen warm-up (temporal_pass.cpp, warmTrainedOnce): initialises
// NGX on ctx's device and makes the two full-frame features exactly as
// dlaaEvaluate's next call would -- the same slots, the same key (w x h in
// and out, DLAA, the current preset generation) -- so that call finds them
// made and reuses them, or recreates on a size or preset mismatch as it
// always did. `features = false` initialises only (the fovea path makes its
// own crop features). initMs is the initialisation's duration (about zero
// when NGX was already asked), createMs[eye] each create's (zero when the
// feature already stood). False, with its reason, on the first refusal;
// the state it leaves behind is the state the first evaluation would have
// left, so nothing here needs undoing. Render thread only, like the rest.
bool dlaaWarm(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, bool features,
              double* initMs, double createMs[2], const char** reason);

// One eye, one frame (`eye` is the upscaler feature slot, kUpscalerSlots above:
// 0 and 1 the eyes', 2 the VR world route's, each with its own feature and
// history; dlaaWarm makes the eyes' two only, the world's is made lazily on its
// first evaluation): colour (R8G8B8A8_UNORM, w x h, a shader view
// possible), depth (R32_FLOAT, the game's reversed-Z values copied), the
// motion vectors (R16G16_FLOAT, pixels, current -> previous), into the
// output (R8G8B8A8_UNORM, outW x outH, an unordered-access view
// possible). Equal sizes are DLAA; an output larger than the input is
// DLSS proper, the quality mode being the one whose own render size (as
// the runtime names it for this output) is nearest the input, among the
// modes whose range holds it; the size ratio decides only when the
// runtime will not say. The jitter is this frame's offset in input
// pixels; reset breaks the history and must be true ONLY when it is
// broken (an eye's first frame, a size change, a withhold) -- the review
// of 2026-09-04 found it raised every frame; frameMs is the time since
// this eye's previous evaluation, zero when unknown. A feature per eye is
// created on first use and rebuilt on a size change. False on any
// refusal, with its reason.
// `reactive` may be null; when given it is a w x h mask (R8_UNORM) whose
// value tells the runtime how far to favour THIS frame's colour over the
// history at that pixel -- NVIDIA's bias-current-colour input. It is for
// content that changes without moving, which no motion vector can
// describe: a HUD readout counting down registers perfectly and blends
// with the digit before it (measured 2026-09-08, the flip side of the
// interface depth, then keyed fix.ui_depth). Zero everywhere is the same as
// not passing one. The runtime takes ONE such mask, so when the temporal
// pass's mover mask (tier 1 of docs/per-object-motion.md) is on as well, the
// pass folds the interface's into it before calling here and hands the union.
//
// `hdr` (the flat HDR route, docs/design-flat-temporal-aa-2026-09-23.md section 81): the colour is HDR scene
// radiance (R11G11B10F or fp16), the output R16G16B16A16_FLOAT, and the feature is created with IsHDR and
// AutoExposure beside the LDR flags (hdr_backend_flags.h). The flags are creation-time, so the feature key carries
// the bit: a flip remakes the feature and its history starts again. False is every other caller, unchanged.
bool dlaaEvaluate(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                  ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                  ID3D11Texture2D* output, ID3D11Texture2D* reactive,
                  uint32_t w, uint32_t h,
                  uint32_t outW, uint32_t outH, float jx, float jy, bool reset,
                  float frameMs, const char** reason, bool hdr = false);

// One eye, one frame, but NVIDIA runs on a CROP of the frame -- the fovea,
// docs/performance.md feature 6. The colour, depth and motion are the same
// full-frame textures dlaaEvaluate is fed; the crop names the sub-rectangle
// (in render pixels, cropX/cropY the top-left, cropW/cropH the size) that
// NVIDIA reads and writes, through the runtime's input and output
// sub-rectangles. DLAA only (1:1), so the output crop is the input crop; a
// per-eye feature is created with output sub-rectangles enabled and rebuilt
// on a size change (`eye` is 0 or 1: the VR world's slot 2 has no fovea and
// is refused). The crop must lie inside the frame. `output` is written
// only in the crop region; the periphery is the caller's to fill and blend.
// The moving-crop probe (2026-09-04) proved a crop pans cleanly through
// NVIDIA's history; a fixed centre is the trivial case of that. False on any
// refusal, with its reason. Counts into the same totals as dlaaEvaluate.
bool dlaaEvaluateFovea(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                       ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                       ID3D11Texture2D* output, uint32_t w, uint32_t h,
                       uint32_t cropX, uint32_t cropY, uint32_t cropW, uint32_t cropH,
                       float jx, float jy, bool reset, float frameMs,
                       const char** reason);

// The general form: NVIDIA runs on an INPUT crop (icx,icy,icw,ich) of the
// render-size textures and writes an OUTPUT crop (ocx,ocy,ocw,och) of the
// native output. Equal crops are DLAA (1:1, dlaaEvaluateFovea above); a
// smaller input crop is DLSS upscaling just the fovea (docs/performance.md
// feature 6, the half-render variant). The feature is created at the input
// crop -> output crop and rebuilt when either changes. False on refusal.
bool dlssEvaluateFovea(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                       ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                       ID3D11Texture2D* output, uint32_t inW, uint32_t inH,
                       uint32_t outW, uint32_t outH, uint32_t icx, uint32_t icy,
                       uint32_t icw, uint32_t ich, uint32_t ocx, uint32_t ocy,
                       uint32_t ocw, uint32_t och, float jx, float jy, bool reset,
                       float frameMs, const char** reason);

// The steady periphery (docs/performance.md feature 6): DLAA over the WHOLE
// of a w x h frame -- a copy of the render reduced to the periphery's scale,
// or the render itself when the game rendered small -- through a third
// per-eye feature with its own history, so the fovea, the periphery and the
// full frame never share an accumulation. The composite upscales what comes
// back around the fovea. The inputs are the same kinds as dlaaEvaluate's, at
// w x h; `output` (R8G8B8A8_UNORM, w x h, an unordered-access view possible)
// is written whole. `eye` is 0 or 1: slot 2, the VR world's, has no periphery
// and is refused. False on any refusal, with its reason.
bool dlaaEvaluatePeriphery(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                           ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                           ID3D11Texture2D* output, uint32_t w, uint32_t h, float jx, float jy,
                           bool reset, float frameMs, const char** reason);

// The model NVIDIA runs, as its render-preset number (nvsdk_ngx_defs.h:
// 11 = K, 10 = J, 12 = L, 13 = M, 0 = the driver's own choice per quality
// mode): `preset` for the full frame and the periphery in every mode,
// `foveaPreset` for the fovea crop when it upscales. Read from the config by
// the temporal pass; a change recreates every live feature on its next
// evaluation, so it is live. Under DLAA the upscaling models L and M are
// never applied (L cost five times K on the desk). The desk (2026-09-05):
// Performance mode's default M is at 2.7x K's error at rest on fine detail;
// under motion L softens least and converges fastest from fresh content,
// which is what a crop needs; on the full frame the models cost the same.
void dlaaSetPreset(unsigned preset, unsigned foveaPreset);
// Temporary A/B (advanced.flat_dlss_exposure): true = the flat HDR route's feature is made without AutoExposure and
// evaluated with a 1x1 exposure texture of 1.0; false = auto (the shipped flags). A change remakes that feature.
void dlaaSetHdrExposureFixed(bool fixed);

// The measured price, for the totals line: evaluations, the mean
// milliseconds by timestamp query, and how many evaluations carried the
// reset. False when nothing has run.
bool dlaaTotals(uint32_t* evaluations, double* avgMs, double* maxMs,
                uint32_t* resets);

// The same price, split by which of the three independent NGX features
// paid it (the full frame, the fovea's centre crop, the steady periphery)
// and by eye, so a display can show a real per-eye figure instead of the
// pooled average above mislabeled as one. False when that role/eye has
// not evaluated yet. eye: 0 left, 1 right; the full-frame role alone also
// answers for slot 2, the VR world route's (the centre and periphery roles
// are the eyes' only, and refuse it).
bool dlaaFullTotals(int eye, uint32_t* evaluations, double* avgMs, double* maxMs);
bool dlaaCentreTotals(int eye, uint32_t* evaluations, double* avgMs, double* maxMs);
bool dlaaPeripheryTotals(int eye, uint32_t* evaluations, double* avgMs, double* maxMs);

void dlaaShutdown();

}  // namespace edvr
