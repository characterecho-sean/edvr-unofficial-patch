// fix.ui_quality -- the UI layer (docs/ui-layer-2026-09-23.md; Design A of
// docs/crisp-ui-handoff.md, phase 1).
//
// The game composites its menus, the 2D screen and the loading screen into
// each eye at the scene's render size (HMD Quality x the headset's size),
// before EDVR's upscale -- so at HMD Quality 0.65 a menu sampled from a
// 3840x2160 panel is rasterised into a 1995x1970 eye and then upscaled, and
// the adaptive UI evidence tells DLSS not to accumulate it. This takes those
// draws out of the eye: each is rasterised by the game's own shaders into
// an EDVR-owned per-eye layer at the size of the frame the door hands on
// (the upscaler's output, the unit-quality size under DLSS) times the key's
// target, unjittered, and the layer is composited over the finished eye
// after the upscale and RCAS, before EDVR's own menu. The UI therefore
// never reaches the upscaler's input: it cannot swim or ghost, and the UI
// depth re-issue and the reactive mask are skipped for the draws it takes.
//
// WHAT IT TAKES (phase 1): the post-tonemap composites -- the 2D screen's
// composite (recognised the way the panel distance and the curved screen
// recognise it, srv0IsPanelSized), and every eye draw that samples an
// interface surface ui_depth has learned (the menu / modal panel family,
// the loading screen's composite, the rest), into an 8-bit UNORM eye
// target, with no depth or stencil test or write, and a blend with a
// premultiplied form. WHAT IT LEAVES: the cockpit's holo panels, flight HUD
// and target sprite, which the game draws into the lit HDR target before
// exposure and the tonemap -- the deferred UI replay (ui_deferred.cpp)
// already re-draws those after the upscale at the output size under an
// external engine, and the layer, composited after the tonemap, cannot take
// them without transcribing it. Every family it leaves is named in the log
// with the reason.
//
// THE ORDER IT CHANGES, and the only one: anything the game drew into an
// eye AFTER a redirected draw now lands UNDER it. The totals line counts
// those draws (and draws that read the eye's target after the UI) and names
// each shader pair once.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

class Config;

namespace detail {
extern bool g_uiLayerLive;
extern bool g_uiLayerWatching;
extern bool g_uiLayerRedirecting;
}  // namespace detail

// The draw path's one gate: fix.ui_quality is on, a temporal mode is on, and
// the layer has not stood down. One load.
inline bool uiLayerLive() { return detail::g_uiLayerLive; }

// Reads fix.ui_quality, fix.temporal_aa and advanced.temporal_aa_debug
// (value ui_layer). Live: an "off" composites the frame in flight and
// redirects nothing from the next draw.
void uiLayerConfigure(Config& cfg);

// Which kind of target Rtv0 is, cached per binding generation: 0 not an
// eye-sized 2D target, 1 an eye target whose view is not 8-bit UNORM (the
// lit HDR target, the G-buffer), 2 an eye target viewed as 8-bit UNORM (the
// post-tonemap target the UI composites draw into).
int uiLayerTargetKind();

// For an owner-context draw whose family vscreen.cpp recognised
// (ui_layer_math.h's UiLayerFamily, as an int): true when the draw goes to
// the layer, and the caller must then bracket EVERY issue of this draw --
// the game's own, the curved screen's and the loader panel's substitutions
// -- with uiLayerBegin/uiLayerEnd. verdictForwards: the draw's verdict
// forwards it as the game's (with or without its own state wrap), rather
// than swallowing it or re-issuing it.
bool uiLayerDecide(ID3D11DeviceContext* ctx, int family, bool verdictForwards);

// Around one issue of a decided draw: bind the eye's layer as the only
// render target (no depth), the viewports and scissors through the map with
// the jitter cancelled, the blend converted (ui_layer_math.h). Begin returns
// false -- and End is then a no-op -- if the draw's state at the moment of
// issue refuses (a blend changed by a verdict's own Begin), in which case the
// draw goes to the game's frame as always. Every state change goes through
// the raw entry points, so the binding shadow keeps describing the game's.
bool uiLayerBegin(ID3D11DeviceContext* ctx);
void uiLayerEnd(ID3D11DeviceContext* ctx);

// True between a successful uiLayerBegin and its End (owner context only):
// the passes that ride the game's own draw -- the screen's motion and UI
// mask, the mesh motion (screen_motion.cpp, vscreen.cpp) -- stand aside for
// a draw the layer has taken: its pixels are no longer in the pass's input,
// and the bound target and viewport are the layer's. One load, inline: it
// is asked on every owner draw those passes see, key off or on.
inline bool uiLayerRedirecting() { return detail::g_uiLayerRedirecting; }

// After the first redirected draw of a frame: one load. When true, every
// other owner draw is shown to uiLayerNoteOther, which counts draws that
// write an eye target the UI was taken from, or (a full-screen pass: count
// <= 6 vertices) read one -- the one order the layer changes (they now land
// under the UI, or no longer see it).
inline bool uiLayerWatching() { return detail::g_uiLayerWatching; }
void uiLayerNoteOther(ID3D11DeviceContext* ctx, uint32_t count);

// The eye check, the one authority on which eye is which: the game's
// Submit names it. A copy out of a target the UI was taken from this frame
// (the copy hooks, while watching) and the texture the game submitted for
// each eye (native_temporal.cpp) together say whether the layer's eye --
// ui_depth's order rule, first target = left -- matched, was swapped, or
// could not be told. Identities only, never dereferenced.
void uiLayerNoteCopy(const void* destination, const void* source);
void uiLayerNoteSubmitted(uint64_t sequence, uint32_t eye, const void* submitted);

// The door, from the native runtime's submit chain (native_temporal.cpp and
// native_sharpen.cpp, both on the thread the game submits from):
//
// the temporal pass treated this eye this frame and handed on `output`
// (identity only, never dereferenced);
void uiLayerNoteTemporal(uint64_t sequence, uint32_t eye, const void* output);
// the door step ran for this eye: `source` is the frame arriving at it (the
// pass's output or the game's own image). When it is the pass's output, and
// a composite over it can run (its format, the GPU's typed stores, the
// shader), this arms the next frame and publishes the size the layer takes;
// otherwise the layer is not armed, and says why once.
void uiLayerDoorSeen(uint64_t sequence, uint32_t eye, ID3D11Texture2D* source);
// the composite, LAST: the layer over `frame`'s `region` (x0, y0, x1, y1 in
// frame pixels), whose rectangle of the layer is `layerUv` (u0, v0, u1, v1:
// the door's input region over its source's size, uiLayerUvFromRegion, so
// a cropped eye lands texel for texel). Returns an AddRef'd EDVR-owned
// texture of the region's size in the frame's format -- forward it with
// full bounds -- or null: nothing was redirected into this eye this frame,
// or the composite refused (said once; a refusal with UI in the layer
// stands the layer down).
ID3D11Texture2D* uiLayerComposite(uint64_t sequence, uint32_t eye, ID3D11Texture2D* frame,
                                  const uint32_t region[4], const float layerUv[4]);

// Once per frame, from vScreenFrameBoundary: the shader's warm compile, the
// 30-second totals, the per-frame watch reset.
void uiLayerFrameBoundary(ID3D11DeviceContext* ctx);

void uiLayerShutdown();

// Defined in native_temporal.cpp, read by the layer at DRAW time: the frame
// the game is drawing (the sequence the runtime's beginFrame opened) and the
// jitter that frame's projection carries for `eye`, in render pixels over
// the region it was computed for (w x h), before the pass's own sign and lag
// switches -- where the game put the pixels, not how the pass reads them.
// (0, 0) while the pass is not jittering. False before the first frame, or
// with no native temporal channel.
bool nativeTemporalDrawJitter(uint32_t eye, uint64_t* sequence, float* jx, float* jy,
                              uint32_t* w, uint32_t* h);

}  // namespace edvr
