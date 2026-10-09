// fix.ui_quality's flat half -- the pure rules of the mono adapter (flat_ui_layer.h), header-only so
// tools/ui_quality_test pins them: which families the flat layer asks for, what jitter a draw's camera carries, and
// when the door arms. No device, no Config, no Log.
#pragma once

#include "ui_layer_math.h"

#include <cmath>
#include <cstdint>

namespace edvr {

// The colour target's class (from the retired flat UI census, 2026-10-09). Scene-sized means the render size R or the
// output size D; a size that is neither is an offscreen target. Float formats are the HDR scene.
enum class FlatUiTarget : uint8_t { kHdr = 0, kLdr, kBackBuffer, kOffscreen, kOther, kCount };
inline bool flatUiIs8bit(uint32_t f) { return f == 28 || f == 29 || f == 87 || f == 91 || f == 88 || f == 93; }
inline bool flatUiIsFloat(uint32_t f) { return f == 26 || f == 10 || f == 11; }
inline FlatUiTarget flatUiTargetClass(bool isBackBuffer, uint32_t w, uint32_t h, uint32_t format, uint32_t dW, uint32_t dH,
                                      uint32_t rW, uint32_t rH) {
    if (isBackBuffer) return FlatUiTarget::kBackBuffer;
    const bool sceneSized = (w == dW && h == dH) || (rW && w == rW && h == rH);
    if (!sceneSized) return FlatUiTarget::kOffscreen;
    if (flatUiIsFloat(format)) return FlatUiTarget::kHdr;
    if (flatUiIs8bit(format)) return FlatUiTarget::kLdr;
    return FlatUiTarget::kOther;
}

// The shared family rule, asked both ways (an HDR eye target, then the post-tonemap one) with no learned surface:
// ui_layer_math.h's shader-pair recognitions, not a second hash table.
inline UiLayerFamily flatUiFamilyOf(uint64_t vs, uint64_t ps) {
    UiFamilyFacts f;
    f.vs = vs;
    f.ps = ps;
    f.targetKind = 1;
    UiLayerFamily out = uiLayerFamilyFor(f);
    if (out != UiLayerFamily::kNone) return out;
    f.targetKind = 2;
    return uiLayerFamilyFor(f);
}

// The families the flat layer asks the shared decision for: the cockpit HUD families the 2026-10-09 09:36 census found
// drawn into the scene's HDR target before the resolve, jittered (the holo panels, the flight HUD, the target sprite,
// the holograms). The scene lines the VR layer also takes for density (the orbit lines, the supercruise bars, the
// space dust) are not asked: they are scene geometry, upscaled with the world in flat as they always were.
inline bool flatUiLayerTakesFamily(UiLayerFamily f) {
    return f == UiLayerFamily::kHolo || f == UiLayerFamily::kFlightHud || f == UiLayerFamily::kSprite ||
           f == UiLayerFamily::kHoloGeneric;
}

// The adapter's own refusals, before the shared decision is asked. Each leaves the draw in the game's frame as stock.
enum class FlatUiRefuse : uint8_t {
    kOtherWork = 0,   // the flat scope already does something with this draw (a capture, an overlay, a substitution)
    kNotHdrTarget,    // not drawn into the scene's HDR target (the only target the take reproduces)
    kNotUpstream,     // the camera injector does not own the jitter (the legacy per-draw binding would differ per draw)
    kNoRows,          // no camera rows for the draw: its jitter cannot be read
    kOtherShift,      // its camera rows carry a shift that is neither the frame's phase nor zero
    kToneUnproven,    // the frame before did not show the game's tone pass reading the HUD's HDR target (or a plain copy
                      // of it): a take could not be certain to come back, so none is made (2026-10-09 11:32 flight)
    kCount
};
inline const char* flatUiRefuseName(FlatUiRefuse r) {
    switch (r) {
    case FlatUiRefuse::kOtherWork: return "other-work";
    case FlatUiRefuse::kNotHdrTarget: return "not-hdr-target";
    case FlatUiRefuse::kNotUpstream: return "not-upstream";
    case FlatUiRefuse::kNoRows: return "no-camera-rows";
    case FlatUiRefuse::kOtherShift: return "other-shift";
    case FlatUiRefuse::kToneUnproven: return "tone-unproven";
    default: return "?";
    }
}

// The jitter a draw's camera rows carry. The rows' measured shift (flat_camera_phase.h, flatCameraMeasureRowShift: NDC,
// +x right, +y up) is turned into render pixels the way flatProjectionJitter makes it (x = ndcX * w / 2, y = -ndcY * h / 2:
// right and down, where the game put the pixels) and matched to the frame's phase or to zero within `tolerance` pixels.
// Anything else (an off-centre camera of its own, a phase from another frame) is not guessed at.
enum class FlatUiJitter : uint8_t { kPhase = 0, kZero, kOther, kNoRows };
struct FlatUiJitterRead {
    FlatUiJitter kind = FlatUiJitter::kNoRows;
    float jx = 0.0f, jy = 0.0f;  // the jitter to cancel, render pixels (the phase, or 0)
    double mx = 0.0, my = 0.0;   // what the rows measured, render pixels
};
constexpr double kFlatUiJitterTolerance = 0.01;  // render pixels
inline FlatUiJitterRead flatUiLayerJitterOf(bool measured, double ndcX, double ndcY, uint32_t w, uint32_t h, float phaseX,
                                            float phaseY) {
    FlatUiJitterRead r;
    if (!measured || !w || !h || !std::isfinite(ndcX) || !std::isfinite(ndcY)) return r;
    r.mx = ndcX * static_cast<double>(w) * 0.5;
    r.my = -ndcY * static_cast<double>(h) * 0.5;
    const bool phase = std::fabs(r.mx - phaseX) <= kFlatUiJitterTolerance && std::fabs(r.my - phaseY) <= kFlatUiJitterTolerance;
    const bool zero = std::fabs(r.mx) <= kFlatUiJitterTolerance && std::fabs(r.my) <= kFlatUiJitterTolerance;
    if (phase) {
        r.kind = FlatUiJitter::kPhase;
        r.jx = phaseX;
        r.jy = phaseY;
    } else if (zero) {
        r.kind = FlatUiJitter::kZero;
    } else {
        r.kind = FlatUiJitter::kOther;
    }
    return r;
}

// The door arms the next frame when EDVR resolved this one and the picture the game's output copy reads is the
// display's own size: the layer is then made at D x target. A frame the resolve refused, or a resolve handed on at
// another size (an evaluation size below the display's), arms nothing, and the next frame's HUD stays in the game's frame.
// The take's certainty (2026-10-09): a HUD draw of frame N is taken only when frame N-1's tone pass was seen reading
// the target the HUD families drew into (or a plain copy of it). The relation, not the identity: a renderer may
// alternate its HDR targets between frames, and the admission at the tone checks frame N's own identity again.
struct FlatUiToneProof {
    uint64_t frame = 0;          // the frame the proof is about (whose HUD asks and tone were seen)
    const void* hudTarget = nullptr;  // this frame's HUD-family HDR target, from the first ask
    const void* alias = nullptr;      // a plain copy of it this frame (the copy pass's output), or null
    uint64_t provenFrame = 0;    // the last frame whose tone read hudTarget or alias
};
inline void flatUiToneProofFrame(FlatUiToneProof& p, uint64_t frame) {
    if (p.frame == frame) return;
    p.frame = frame;
    p.hudTarget = nullptr;
    p.alias = nullptr;
}
inline void flatUiToneProofHud(FlatUiToneProof& p, uint64_t frame, const void* target) {
    flatUiToneProofFrame(p, frame);
    if (!p.hudTarget) p.hudTarget = target;
}
// A plain copy (the game's copy pixel shader) reading the HUD's target this frame names its output as the alias.
inline void flatUiToneProofCopy(FlatUiToneProof& p, uint64_t frame, const void* source, const void* output) {
    flatUiToneProofFrame(p, frame);
    if (source && output && source == p.hudTarget) p.alias = output;
}
// The tone pass reads `input` at its HDR slot: proven when that is the HUD's target or its copy. True when proven.
inline bool flatUiToneProofTone(FlatUiToneProof& p, uint64_t frame, const void* input) {
    flatUiToneProofFrame(p, frame);
    const bool reads = input && (input == p.hudTarget || input == p.alias);
    if (reads) p.provenFrame = frame;
    return reads;
}
inline bool flatUiToneProven(const FlatUiToneProof& p, uint64_t frame) {
    return frame > 1 && p.provenFrame + 1 == frame;
}

inline bool flatUiLayerDoorArms(bool treated, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH) {
    return treated && outW && outH && inW == outW && inH == outH;
}

}  // namespace edvr
