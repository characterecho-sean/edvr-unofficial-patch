// fix.ui_quality -- the interface surfaces' instruments and HMD Quality
// (docs/ui-layer-2026-09-23.md). The key's panels are sized by the game's
// own panel formula (ui_panel_scale.h); its layer, the per-eye layer the
// post-tonemap UI is drawn into, is ui_layer.h; one key, one configure
// (uiLayerConfigure hands the target on), one family of log lines.
//
// (Until 2026-09-23 this module also matched the game's interface surfaces
// at CreateTexture2D by their ratio to the render size and made them bigger
// through fss_res's inflation mechanism, learning ratios into a file beside
// the logs. The engine-side panel patch replaced it, flown OK on f05c84bf
// (flight 162703); when the patch stands down on a new game build, the
// panels stay at the game's size until it is re-keyed, like every other
// build-keyed patch here.)
//
// THREADS. uiSurfacesNoteChain and uiSurfacesNoteAtlas run inside the game's
// CreateTexture2D, on its streaming threads -- and on the render thread. So
// nothing on that path takes a lock anything holds around a create: the
// native channel's values are lock-free snapshots, HMD Quality a cache (read
// on configure and on a thread-pool thread, never in a create), and EDVR's
// own creates are not offered at all (device_hook.cpp). The module's own
// state is under one SRW lock that never spans a create.
#pragma once

#include <cstddef>
#include <cstdint>

struct D3D11_TEXTURE2D_DESC;
struct ID3D11Texture2D;

namespace edvr {

// From uiLayerConfigure: the key's target (0 off, 1.0, 1.25). The
// instruments run while it is on.
void uiSurfacesSetTarget(float target);

// Once a frame from uiLayerFrameBoundary: the instruments' frame count, and
// every five seconds while the key is on HMD Quality's refresh, queued to a
// pool thread.
void uiSurfacesFrameBoundary();

// THE CONFIRMATION INSTRUMENT (docs/ui-sizing-owner-2026-09-23.md section 8;
// ui_sizing_math.h), the record a new game build is re-keyed against. Every
// render or depth target the game creates in an interface panel's shape --
// single mip, no MSAA, no initial data, smaller than the render size on both
// axes and a power of two on neither -- gets its game return-address chain
// once per distinct size and kind (colour or depth), logged with the frame,
// the render width W, tangents, vFOV and k, the implied stage and a verdict
// (rtt / glyph-cache / other, every key RVA's hit or miss). One load a create
// while the key is off.
bool uiSurfacesWantsChain(const D3D11_TEXTURE2D_DESC& d, bool initialData);
void uiSurfacesNoteChain(const D3D11_TEXTURE2D_DESC& d);

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
// behind what the game is told (review P3-1). False before the first
// beginFrame, after an invalidate, or with no native temporal channel.
bool nativeTemporalRecommended(uint32_t* w, uint32_t* h);
// ...and the same frame's vertical frustum: eye 0's up and down tangents
// (magnitudes), the frustum the game is told. Lock-free.
bool nativeTemporalVerticalTangents(float* up, float* down);
// ...and what the game is told now, the host's ask (max over eyes): during an
// adoption it leads the recommendation above, and the game re-creates its
// surfaces for it before a frame of it arrives (review P3-1, the 2026-09-23
// 13:23 menu flight). False when the host does not say. Lock-free.
bool nativeTemporalAsked(uint32_t* w, uint32_t* h);
// ...and the TRUE display frustum's vertical tangents (eye 0, magnitudes):
// the headset's own, before a cull guard or a trim -- the engine-side panel
// sizing's untrimmed k (ui_panel_scale.h). Lock-free.
bool nativeTemporalTrueVerticalTangents(float* up, float* down);

// HMD Quality as last read (the newest .fxcfg's HMDRenderTargetMultiplier,
// cached, never read on the frame path); 0 while unknown. Lock-free.
float uiSurfacesHmdQuality();

}  // namespace edvr
