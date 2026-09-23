// fix.ui_quality -- the surfaces half (docs/ui-layer-2026-09-23.md). The
// key's other half, the per-eye layer the post-tonemap UI is drawn into, is
// ui_layer.h; one key, one configure (uiLayerConfigure hands the target on),
// one family of log lines.
//
// Every offscreen interface panel the game makes -- recognised by the rule
// its size follows, a constant times U = W / 2 tan(vFOV/2) (ui_quality_math.h:
// the census's five panels, plus any this rig's GUI draws have been seen
// landing in at two U) -- is created at the size it would have at HMD
// Quality = the key's target, through fss_res's inflation mechanism (the
// tracked ring, the viewport and scissor backstops, the eye-test
// exclusion). The render width W is known from the first frame: the
// runtime's game-facing recommendation times HMD Quality, truncated as
// Elite truncates it, and cross-checked against the size the game actually
// submits; the vertical field of view is the frame's own frustum (native
// temporal's beginFrame).
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
struct ID3D11Texture2D;

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

// THE CONFIRMATION INSTRUMENT (docs/ui-sizing-owner-2026-09-23.md section 8;
// ui_sizing_math.h). Inside uiSurfacesMatch, every create of a surface's
// shape gets its game return-address chain once per distinct size and kind
// (colour or depth), logged with the frame, EDVR's W, tangents, vFOV and k,
// the implied stage and a verdict (rtt / glyph-cache / other, every key
// RVA's hit or miss).
//
// ...and the glyph atlas (section 8.3): an A8_UNORM texture 1024 or more a
// side the game creates (hookedCreateTexture2D asks, once created) gets its
// chain and verdict logged, and its writes counted from the context hooks --
// UpdateSubresource, a writing Map, a copy into it -- every 30 s: Scaleform's
// raster cache is written as glyphs arrive, a static font texture never.
bool uiSurfacesWantsAtlas(const D3D11_TEXTURE2D_DESC& d);
void uiSurfacesNoteAtlas(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d, bool initialData);
void uiAtlasNoteWriteSlow(const void* res, int how);  // how: 0 update, 1 map, 2 copy
namespace detail {
extern bool g_uiAtlasWatching;  // set once the first atlas is registered
}
// One load a call on the context hooks until an atlas is watched.
inline void uiAtlasNoteWrite(const void* res, int how) {
    if (detail::g_uiAtlasWatching) uiAtlasNoteWriteSlow(res, how);
}
// The atlas's 30-second line (from ui_layer's totals), when one is watched.
void uiSurfacesLogAtlas();

// Defined in native_temporal.cpp, lock-free: the size, max over eyes, the
// runtime's beginFrame says the frame being drawn was rendered for
// (fix.openxr_resolution, the FOV trim and the cull guard included) --
// during a cull-guard or FOV-trim adoption the previous ask, one rebuild
// behind what the game is told (review P3-1, open). False before the first
// beginFrame, after an invalidate, or with no native temporal channel.
bool nativeTemporalRecommended(uint32_t* w, uint32_t* h);
// ...and the same frame's vertical frustum: eye 0's up and down tangents
// (magnitudes), which the panel rule needs (ui_quality_math.h). Lock-free.
bool nativeTemporalVerticalTangents(float* up, float* down);
// ...and what the game is told now, the host's ask (max over eyes): during an
// adoption it leads the recommendation above, and the game re-creates its
// surfaces for it before a frame of it arrives -- so a create is judged
// against it first (review P3-1, the 2026-09-23 13:23 menu flight). False
// when the host does not say. Lock-free.
bool nativeTemporalAsked(uint32_t* w, uint32_t* h);

}  // namespace edvr
