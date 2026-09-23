// fix.ui_quality -- the surfaces half (docs/ui-layer-2026-09-23.md). The
// key's other half, the per-eye layer the post-tonemap UI is drawn into, is
// ui_layer.h; one key, one configure (uiLayerConfigure hands the target on),
// one family of log lines.
//
// Every offscreen interface surface whose size is a known fraction of the
// game's internal render resolution (ui_quality_math.h: the census's five,
// plus any this rig's GUI draws have been seen landing in) is created at the
// size it would have at HMD Quality = the key's target, through fss_res's
// inflation mechanism (the tracked ring, the viewport and scissor
// backstops, the eye-test exclusion). The internal resolution is known from
// the first frame: the runtime's game-facing recommendation times HMD
// Quality, truncated as Elite truncates it, and cross-checked against the
// size the game actually submits.
//
// THREADS. uiSurfacesMatch and uiSurfacesNoteCreated run inside the game's
// CreateTexture2D, on its streaming threads -- and on the render thread,
// possibly INSIDE native temporal's treat(), which holds that channel's
// mutex while the pass creates its targets (review P1-1). So nothing on
// that path takes a lock anything holds around a create: the recommendation
// is a lock-free snapshot, HMD Quality a cache (read on configure and on a
// thread-pool thread, never in a create), the eye size a shared-memory
// word, and EDVR's own creates are not offered at all (device_hook.cpp).
// The module's own state is under one SRW lock that never spans a create.
#pragma once

#include <cstddef>
#include <cstdint>

struct D3D11_TEXTURE2D_DESC;

namespace edvr {

// From uiLayerConfigure: the key's target (0 off, 1.0, 1.25) and its text.
void uiSurfacesSetTarget(float target, const char* text);

// One load for fss_res's CreateTexture2D gate: the key is on.
bool uiSurfacesWantCreates();

// Inside CreateTexture2D, for a desc fss_res has already found to be a
// single-mip, non-MSAA render or depth target with no initial data, from a
// caller outside EDVR's own module: when its size is a table ratio of the
// internal resolution, grow *d in place by the factor and return true
// (*factorOut the exact factor, *familyOut 'V'/'T'/'I' when the classifier
// already knows a surface of that size, else 0). A create of a size decided
// in the last two seconds gets that decision again (a colour target and its
// depth partner stay one size, review P3-9).
bool uiSurfacesMatch(D3D11_TEXTURE2D_DESC* d, float* factorOut, char* familyOut);

// The receipts, from fss_res's tracked paths: a grown texture was created;
// a viewport or scissor was rescaled into one; a copy touched one.
void uiSurfacesNoteCreated(uint32_t origW, uint32_t origH, uint32_t newW, uint32_t newH,
                           char family);
void uiSurfacesNoteViewport();
void uiSurfacesNoteScissor();
void uiSurfacesNoteCopy();

// Once a frame from uiLayerFrameBoundary, acting every five seconds: HMD
// Quality's refresh queued to a pool thread, the cross-check of the derived
// internal size against the submitted one, and the learning (a GUI-drawn
// surface that scales across two resolutions; ui_quality_math.h).
void uiSurfacesFrameBoundary();

// The surfaces' part of the 30-second "ui quality:" totals line.
void uiSurfacesSummary(char* out, size_t n);

// Defined in native_temporal.cpp, lock-free: the size, max over eyes, the
// runtime's beginFrame says the frame being drawn was rendered for
// (fix.openxr_resolution, the FOV trim and the cull guard included) --
// during a cull-guard or FOV-trim adoption the previous ask, one rebuild
// behind what the game is told (review P3-1, open). False before the first
// beginFrame, after an invalidate, or with no native temporal channel.
bool nativeTemporalRecommended(uint32_t* w, uint32_t* h);

}  // namespace edvr
