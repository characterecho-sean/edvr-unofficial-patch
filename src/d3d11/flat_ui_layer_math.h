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
    kAfterTone,       // drawn after this frame's tone pass: it could never be tonemapped back (2026-10-09 13:23 flight)
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
    case FlatUiRefuse::kAfterTone: return "after-tone";
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

// The take's certainty (2026-10-09). A HUD draw of frame N is taken only when (a) frame N-1's tone pass was seen
// reading THAT draw's target -- or a plain copy of it -- and (b) frame N has not yet copied that target or tonemapped it. Per target
// (13:23 flight, ea04a8a5: the first ask of a frame was a hologram whose target the tone never read, and one target
// stood for all of them), and never after the tone (a draw taken then can never be tonemapped back: the frame's HUD is
// lost and the path backs off). A target the tone does not read, or reads only through a copy the adapter did not see,
// stays stock and is counted as tone-unproven; one drawn after the tone is counted as after-tone.
struct FlatUiToneProof {
    static constexpr uint32_t kTargets = 4;
    uint64_t frame = 0;                      // the frame the lists below are about
    const void* hud[kTargets] = {};          // this frame's HUD-family HDR targets, in order of first ask
    const void* alias[kTargets] = {};        // ...a plain copy of each this frame (the copy pass's output), or null
    bool read[kTargets] = {};                // ...this frame's tone read it (or its copy)
    uint32_t count = 0;
    const void* consumed[8] = {};            // this frame: what a copy or a tone pass already read (a HUD draw into one
    uint32_t consumedCount = 0;              // of these could never reach the tone again)
    uint64_t provenFrame = 0;                // the frame the proven list is from
    const void* proven[kTargets] = {};       // the targets that frame's tone read
    uint32_t provenCount = 0;
};
inline void flatUiToneProofFrame(FlatUiToneProof& p, uint64_t frame) {
    if (p.frame == frame) return;
    if (p.frame) {  // the frame that ended: what its tone read is the next frame's proof
        p.provenCount = 0;
        for (uint32_t i = 0; i < p.count; ++i)
            if (p.read[i]) p.proven[p.provenCount++] = p.hud[i];
        p.provenFrame = p.frame;
    }
    p.frame = frame;
    p.count = 0;
    p.consumedCount = 0;
    for (uint32_t i = 0; i < FlatUiToneProof::kTargets; ++i) {
        p.hud[i] = p.alias[i] = nullptr;
        p.read[i] = false;
    }
}
inline void flatUiToneConsume(FlatUiToneProof& p, const void* r) {
    if (!r) return;
    for (uint32_t i = 0; i < p.consumedCount; ++i)
        if (p.consumed[i] == r) return;
    if (p.consumedCount < 8) p.consumed[p.consumedCount++] = r;
}
inline void flatUiToneProofHud(FlatUiToneProof& p, uint64_t frame, const void* target) {
    flatUiToneProofFrame(p, frame);
    if (!target) return;
    for (uint32_t i = 0; i < p.count; ++i)
        if (p.hud[i] == target) return;
    if (p.count < FlatUiToneProof::kTargets) p.hud[p.count++] = target;
}
// A plain copy (the game's copy pixel shader) reading one of this frame's HUD targets names its output as that target's alias.
inline void flatUiToneProofCopy(FlatUiToneProof& p, uint64_t frame, const void* source, const void* output) {
    flatUiToneProofFrame(p, frame);
    if (!source || !output) return;
    flatUiToneConsume(p, source);
    for (uint32_t i = 0; i < p.count; ++i)
        if (p.hud[i] == source) p.alias[i] = output;
}
// The tone pass reads `input` at its HDR slot: every HUD target it is (or is the copy of) is proven for the next frame.
// Returns that target's alias when the input is one (the admission accepts it), or the target itself, or null when the
// input is none of them.
inline const void* flatUiToneProofTone(FlatUiToneProof& p, uint64_t frame, const void* input) {
    flatUiToneProofFrame(p, frame);
    flatUiToneConsume(p, input);
    const void* matched = nullptr;
    for (uint32_t i = 0; i < p.count && input; ++i) {
        if (input == p.hud[i] || input == p.alias[i]) {
            p.read[i] = true;
            flatUiToneConsume(p, p.hud[i]);
            if (!matched) matched = input;
        }
    }
    return matched;
}
inline bool flatUiToneProven(const FlatUiToneProof& p, uint64_t frame, const void* target) {
    if (frame <= 1 || p.provenFrame + 1 != frame || !target) return false;
    for (uint32_t i = 0; i < p.provenCount; ++i)
        if (p.proven[i] == target) return true;
    return false;
}
// Has this frame already read 	arget -- a copy taken of it, or the tone pass reading it or its copy? A draw into it now
// could never be tonemapped back (it is counted as after-tone and left in the frame).
inline bool flatUiToneConsumed(const FlatUiToneProof& p, uint64_t frame, const void* target) {
    if (p.frame != frame || !target) return false;
    for (uint32_t i = 0; i < p.consumedCount; ++i)
        if (p.consumed[i] == target) return true;
    return false;
}
// The door arms the next frame when EDVR resolved this one and the picture the game's output copy reads is the
// display's own size: the layer is then made at D x target. A frame the resolve refused, or a resolve handed on at
// another size (an evaluation size below the display's), arms nothing, and the next frame's HUD stays in the game's frame.
inline bool flatUiLayerDoorArms(bool treated, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH) {
    return treated && outW && outH && inW == outW && inH == outH;
}

}  // namespace edvr
