// The "may want" byte: one byte per vertex shader, computed once when the shader is bound (hookedVSSetShader) and stored beside its
// content hash in the binding shadow (bindingVsMayWant), so a draw reads it with one load. A set bit means the subscriber may want the
// draw and runs exactly as before; a clear bit means the subscriber's own pure predicate rejects this vertex shader with every pixel
// shader, so the call is skipped. Hash 0 (a shader EDVR never hashed) is "may want" for every subscriber.
//
// The byte holds shader-hash terms only. Every runtime term (live and failed flags, sourceFrame == frame, native on foot, scope-mark
// arming, stand-downs, target kind) is still tested per draw after a set bit, so the byte cannot go stale and needs no invalidation.
// Not in it, on purpose: engine velocity (its miss path restores the game's shaders), the temporal rigid draw (already an inline hash
// test), UI depth, uiLayerNoteOther, star glow, target sharp and the marker tee (per-draw resets, counters and the draw serial).
#pragma once

#include <cstdint>

#include "motion_vs.h"
#include "ui_layer_math.h"

namespace edvr {

enum VsMayWantBit : uint8_t {
    kVsMayScreenUi = 1,         // screenMotionUiDraw
    kVsMayScreenRecognize = 2,  // screenMotionDraw (through screenMotionRecognize)
    kVsMayWeaponMotion = 4,     // weaponMotionWants, offscreen draws
    kVsMayUiFamily = 8,         // uiLayerFamilyOf on the HDR target (kind 1) only
};
constexpr uint8_t kVsMayWantAll = 0x0F;

inline uint8_t vsMayWantByte(uint64_t vs) {
    if (!vs) return kVsMayWantAll;
    uint8_t m = 0;
    if (screenMotionUiPairVs(vs)) m |= kVsMayScreenUi;
    if (vs == kScreenMotionRecognizePair.vs) m |= kVsMayScreenRecognize;
    if (weaponMotionFamilyHash(vs)) m |= kVsMayWeaponMotion;
    if (uiLayerFamilyHdrVsMayWant(vs)) m |= kVsMayUiFamily;
    return m;
}

// True when the subscriber behind `bit` would reject this draw on the vertex shader alone.
inline bool vsMaySkip(uint8_t mayWant, uint8_t bit) { return (mayWant & bit) == 0; }

}  // namespace edvr
