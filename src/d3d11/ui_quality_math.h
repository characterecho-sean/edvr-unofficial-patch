// fix.ui_quality's render-state arithmetic: the game's internal render width
// derived from the runtime's recommendation and HMD Quality, the vertical
// field of view as the panel formula reads it, and the shape an interface
// panel's surface has. Header-only, no device, no Config, no Log:
// ui_panel_scale.cpp, ui_surfaces.cpp and tools/ui_quality_test compile
// exactly this.
//
// THE RULE (fitted 2026-09-23 from every September flight log, then derived
// from the game's own code: docs/ui-sizing-owner-2026-09-23.md). Elite sizes
// every render-to-texture interface panel -- both axes, one scale -- as its
// stage times W x k / 1920, k = tan(0.782) / tan(vFOV/2): a constant times
//
//     U = W / (2 tan(vFOV / 2)),   vFOV = atan(tanUp) + atan(tanDown),
//
// W the render width in pixels and vFOV the game's vertical field of view
// (the frustum's up and down half-angles added, so an asymmetric frustum
// counts as the symmetric one of the same angle). ui_panel_scale.h sizes the
// panels in that formula itself (ui_sizing_math.h).
#pragma once

#include <cmath>
#include <cstdint>

#include "../openxr/game_render_size.h"  // gameFacingDimension: what the game is told

namespace edvr {

// ------------------------------------------------ the internal resolution --

// One axis of the game's internal render size: the runtime's game-facing
// recommendation (even, game_render_size.h) times HMD Quality, which Elite
// truncates to whole pixels. 3070 x 0.65 = 1995; 3032 x 0.65 = 1970;
// 3070 x 0.5 = 1535. 0 when either input is unknown.
inline uint32_t uiQualityInternalDim(uint32_t recommended, float hmd) {
    if (!recommended || recommended > 16384u || !(hmd > 0.0f) || !std::isfinite(hmd)) return 0;
    const float v = static_cast<float>(recommended) * hmd;
    if (!(v >= 1.0f) || v > 16384.0f) return 0;
    return static_cast<uint32_t>(v);
}

// The recommendation the game is given before the runtime's first frame
// says otherwise: the larger eye's swapchain size on each axis (the runtime
// answers GetRecommendedRenderTargetSize with the max over eyes), made even
// the way the runtime makes every game-facing size.
inline void uiQualityRecommendedFromEyes(const uint32_t w[2], const uint32_t h[2],
                                         uint32_t* outW, uint32_t* outH) {
    const uint32_t mw = w[0] > w[1] ? w[0] : w[1];
    const uint32_t mh = h[0] > h[1] ? h[0] : h[1];
    if (outW) *outW = openxr::gameFacingDimension(mw);
    if (outH) *outH = openxr::gameFacingDimension(mh);
}

// --------------------------------------------------------------- the rule --

// 2 tan(vFOV / 2) for a frustum's up and down tangents (their magnitudes:
// the sign conventions differ between OpenVR and OpenXR, the angle does
// not). Pimax Crystal Super untrimmed (1.2648, 1.2648): 2.5296; trimmed 2
// degrees top and bottom (1.1779, 1.1779): 2.3558; Quest 3 (0.9657, 1.4281):
// 2.3417 (not the tangents' sum, 2.3938). 0 when either is not usable.
inline float uiQualityFovTangent(float up, float down) {
    up = std::fabs(up);
    down = std::fabs(down);
    if (!(up > 0.01f) || !(down > 0.01f) || !(up < 100.0f) || !(down < 100.0f)) return 0.0f;
    return 2.0f * std::tan((std::atan(up) + std::atan(down)) * 0.5f);
}

// The shape an interface surface has: smaller than the render size on both
// axes (the eye targets and the 3840x2160 2D screen are not), not a power of
// two on either (atlases, icon caches and shadow maps are), and not a sliver
// (a 256x1 lookup strip, a 1x1 target). The sizing chain instrument's gate
// (ui_surfaces.cpp).
inline bool uiQualityCandidateShape(uint32_t w, uint32_t h, uint32_t internalW,
                                    uint32_t internalH) {
    if (w < 16 || h < 16 || !internalW || !internalH) return false;
    if (w >= internalW || h >= internalH) return false;
    if ((w & (w - 1)) == 0 || (h & (h - 1)) == 0) return false;
    return true;
}

}  // namespace edvr
