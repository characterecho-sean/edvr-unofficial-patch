// The eye heal: the left eye's black arrival tiles filled from the right
// eye's image, per pixel, stereo untouched.
//
// THE MEASURED DEFECT (docs/fss-scanner.md, rounds 33-34): for ~10 frames
// of the Full System Scanner's zoom arrival, the LEFT (primary) submitted
// image carries hard-black not-yet-resolved tiles the right does not --
// 16 vs 2 on bright content -- because the engine applies its reveal gate
// to the primary view only. The body sits at optical infinity, where the
// two eyes' images of it differ by a PURE HORIZONTAL SHIFT computable
// from the published eye tangents: dx = W * (outer - inner) / (outer +
// inner). So a black left pixel whose shifted right counterpart is lit
// is a gated tile, and the right's pixel is the correct content for the
// left too. Anything genuinely dark (space, shadow) is dark in both and
// heals nothing: the pass is self-limiting.
//
// The compute runs in d3d11.dll (which owns the compile path) and is
// invoked from openvr_api.dll at Submit(left) through one exported
// function -- the two halves are one process, so the texture pointer
// crosses directly. Null return means "submit the original", every
// failure logged once.
#pragma once

#include <cstdint>

extern "C" {
// leftTex/rightTex: ID3D11Texture2D* of the two submitted eyes (right is
// last frame's -- one frame stale, at infinity where that is nothing).
// outerMag/innerMag: the eye frustum tangent magnitudes from the bridge.
// Returns our healed ID3D11Texture2D* to submit for the left eye, or
// null to submit the original.
__declspec(dllexport) void* edvrFssHealLeft(void* leftTex, void* rightTex,
                                            float outerMag, float innerMag);
}
