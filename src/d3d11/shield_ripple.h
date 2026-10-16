// The shield-strike refraction ripple, drawn after the upscale (docs/dlss-blur-elements-2026-10-14.md, "Design, 2026-10-16: the shield refraction after the
// upscale" and its "Built" entry). Pure facts and rules, shared by the layer (ui_layer.cpp), the draw hook (vscreen.cpp) and the rig (tools/shield_ripple_test).
//
//   THE DRAW. vs 820E5C131B99361D / ps 6EAA86EFE135B2D4 (exact pair), one draw an eye-frame (2880 indices, one instance, one bubble: the last wins), into the
//   lit HDR eye target, blend SRC_ALPHA / INV_SRC_ALPHA over, depth GEQUAL with no write, stencil off. The pixel shader sums n = (0,0,1) plus, for each of
//   cb2[7].w hits, the t1 normal (x2-1, faded by saturate(0.5 * x1.x), weighted by saturate(dot(normalize(hitpos), v1))), and samples t0 -- the pre-DLSS eye HDR
//   copy -- at uv_jit + normalize(n).xy * cb2[0].x, where uv_jit = (v0.x / v0.z * 0.5 + 0.5, 1 - (v0.y / v0.z * 0.5 + 0.5)) and v0 is the JITTERED clip position
//   (x, y, w). It writes rgb * cb1[90].z * cb1[90].y with alpha 1: an overwrite. No tint, fresnel or chromatic term; d = normalize(n).xy * cb2[0].x is a UV
//   fraction, so it holds at any resolution.
//
//   THE ROUTE. The draw is taken out of the scene and re-issued unchanged through the HDR layer route (viewport remap, jitter cancel, seeded depth GEQUAL) into
//   a cleared per-eye R32G32_FLOAT layer D with blend disabled and t0 swapped for a UV ramp (texel i holds (i + 0.5) / W), so D records the game's own
//   uv_jit + d. At the door, after RCAS and before the weapons/HUD composite, the picture is copied and rewritten over D's coverage:
//   out = copy.Sample(D - jitter_uv), jitter_uv = (jx / W, jy / H) in the render's pixels. T(F)(x + d) = T(F(x + d)) to within about 1/255 (the tonemap is one tap
//   of its HDR source, then exposure, LUT and saturation).
//
//   THE CHECKS (R1). The route is exact only if the pixel shader's gain cb1[90].z * cb1[90].y is 1 and t0's sampler (s0) is linear: on the first draw the layer
//   reads both, and stands the route down to stock (the draw stays in the scene) naming the reason if either is not.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {

constexpr uint64_t kUiVsShieldRefract = 0x820E5C131B99361Dull;
constexpr uint64_t kUiPsShieldRefract = 0x6EAA86EFE135B2D4ull;
inline bool shieldRefractPair(uint64_t vs, uint64_t ps) { return vs == kUiVsShieldRefract && ps == kUiPsShieldRefract; }
// vscreen.cpp reads the pixel shader's hash for this vertex shader (the pair is the key).
inline bool shieldRefractVs(uint64_t vs) { return vs == kUiVsShieldRefract; }

// R1. The gain the pixel shader multiplies t0's sample by (cb1[90].z * cb1[90].y) must be 1 within 1e-6: with the ramp at t0, D holds uv_jit + d only then.
inline bool shieldRippleGainOk(float z, float y) { return std::fabs(static_cast<double>(z) * static_cast<double>(y) - 1.0) <= 1e-6; }
// R1. t0's sampler must filter linearly in min and mag (D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT 0x14, D3D11_FILTER_MIN_MAG_MIP_LINEAR 0x15): the ramp then reads
// back its own coordinate. A point filter reads the nearest texel's centre; an anisotropic or comparison filter is not what the shader was measured with.
inline bool shieldRippleFilterOk(unsigned d3dFilter) { return d3dFilter == 0x14u || d3dFilter == 0x15u; }

// D's cleared value, and the test for "the bubble drew here". A legitimate uv_jit + d is within a few tenths of [0, 1]; the clear is far outside.
constexpr float kShieldRippleNoCover = -10000.0f;
inline bool shieldRippleCovered(float dx) { return dx > -1000.0f; }

// D is released after this many frames with no 820E draw taken.
constexpr uint32_t kShieldRippleIdleFrames = 300;

// The jitter the draw's v0 carries, in the uv of the render's size: the game's content sits (jx, jy) render pixels right and down of its unjittered place.
inline void shieldRippleJitterUv(float jx, float jy, float renderW, float renderH, float out[2]) {
    out[0] = renderW > 0.0f ? jx / renderW : 0.0f;
    out[1] = renderH > 0.0f ? jy / renderH : 0.0f;
}
// Where the door reads the picture: D minus the jitter (D = uv_jit + d = uv_unjittered + jitter_uv + d).
inline void shieldRippleSampleUv(const float d[2], const float jitterUv[2], float out[2]) {
    out[0] = d[0] - jitterUv[0];
    out[1] = d[1] - jitterUv[1];
}

}  // namespace edvr
