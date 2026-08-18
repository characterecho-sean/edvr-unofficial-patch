// The cull guard's submit-side copy.
//
// The guard's first submit mechanism narrowed the VRTextureBounds_t handed
// to the runtime -- free, and correct by the OpenVR contract. The field
// refuted it in one flight (2026-08-18): OpenComposite over VDXR displayed
// the full wide-rendered image against the true-FOV mapping regardless of
// the bounds, which a player experiences as the whole world distorting with
// every head turn. The layer below EDVR is somebody else's code on both of
// this project's rigs, and its bounds handling is not ours to fix.
//
// So the default mechanism trusts nothing below Submit: copy the true-FOV
// pixel region into an EDVR-owned texture of exactly that size and submit
// THAT, full-bounds -- an ordinary submission indistinguishable from a game
// whose render resolution changed, which both field paths demonstrably
// handle. Costs one GPU region-copy per eye per frame while the guard is
// live.
#pragma once

#include <cstdint>

#include "openvr_min.h"

namespace edvr {

// Copies the crop region of srcHandle (a D3D11 texture the game submitted)
// into an EDVR-owned per-eye texture and returns that texture's handle.
// The region is the composition of the game's own bounds (null = whole
// texture) with the guard's crop fractions {left, top, right, bottom};
// *outBounds receives full-span bounds carrying only the original
// orientation (a game that submitted flipped v stays flipped).
//
// snapW/snapH, when non-zero, are the EXACT output size the crop must land
// on -- the canonical submission size the session established before the
// guard went live. The fraction box is nudged (centred, clamped) to that
// size; a box more than 64 pixels away from it means the source is not the
// adopted-size render target this stage promised, and the copy refuses.
// Zero/zero crops the fraction box as computed.
//
// Returns null on any failure -- wrong texture kind, MSAA or array source,
// snap mismatch, create or copy failure, fault budget spent -- and the
// caller must stand the guard down rather than submit a frame whose
// projection and image disagree.
void* guardCropCopy(uint32_t eye, void* srcHandle,
                    const vr::VRTextureBounds_t* srcBounds,
                    const float fractions[4], uint32_t snapW, uint32_t snapH,
                    vr::VRTextureBounds_t* outBounds);

uint32_t guardCropCopies();

void guardCropShutdown();

}  // namespace edvr
