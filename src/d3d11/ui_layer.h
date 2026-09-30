// fix.ui_quality -- the UI layer (docs/ui-layer-2026-09-23.md; Design A of
// docs/crisp-ui-handoff.md, phase 1). The key's other half, every
// render-to-texture panel at the target's size, is ui_panel_scale.h.
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
// recognise it, srv0IsPanelSized) except while that screen IS the world --
// on foot, or a 3D map -- where it stays in the picture for the temporal
// pass (the world-screen gate, ui_layer_math.h: the journal's on-foot
// reading OR the screen's own busy depth), and every eye draw that samples an
// interface surface ui_depth has learned (the menu / modal panel family,
// the loading screen's composite, the rest), into an 8-bit UNORM eye
// target, with a blend that has a premultiplied or multiplicative form.
// Depth and stencil: a draw that WRITES them (the menu panel marks its
// footprint in stencil for the draws after it) draws its colour into the
// layer and is issued once more with no colour target, so the write lands
// in the game's own buffer exactly as before; a draw that TESTS them is
// drawn against the layer's own depth-stencil target, the game's resampled
// to the layer's size with the jitter cancelled. A multiply (the loading
// screen's gamma pass) scales the layer and a per-channel transmittance the
// composite applies to the frame. ALSO TAKEN, into a per-eye HDR layer at
// the same size (the crisp-HUD half of fix.ui_quality,
// docs/cockpit-hud-layer-design-2026-09-27.md): the cockpit's HDR HUD
// families -- the holo panels, the flight HUD, the target sprite, and the
// crisp take's eight hologram families (the radar's icon core, its two
// stalks, and the five contact markers; NOT the glass canopy, the target
// sphere, the corona family or the world-marker reticle -- the canopy sits
// in front of the whole sky and the other three are refused by the phase-3
// review, reviews/crisp-hud-phase3-review-2026-09-28.md) -- which the game
// draws
// into the lit HDR target before exposure and the tonemap.
// They are tonemapped into the 8-bit layer by the game's own tonemap draw,
// re-issued once per eye with the layer as its HDR source (the
// tonemap_admit.h admission), so the exposure, LUT and bloom are the game's
// own and the door's composite is untouched. Every family it leaves is
// named in the log with the reason.
//
// THE ORDER IT CHANGES, and the only one: a draw after a redirected draw
// that WRITES the same eye target is taken into the layer too, after the
// UI, so it stays where the game drew it -- over the UI. Two things still
// land under it as before: a post pass (it samples an eye-sized input) and
// a write the take path refuses at issue. A draw that only READS the
// target is never taken; it no longer sees the UI in what it reads. The
// totals line counts every case and names each shader pair once.
#pragma once

#include <cstdint>
#include <cstddef>

struct ID3D11DeviceContext;
struct ID3D11PixelShader;

struct ID3D11Texture2D;

namespace edvr {
// Successful CreatePS hook only: two exact originals, no disk/HLSL input.
// The render-owner cache prepares their restricted DXBC remap before a take.
void uiLayerRememberHoloPs(ID3D11PixelShader* shader, uint64_t hash,
                           const void* bytes, size_t count, bool linked);

class Config;

namespace detail {
extern bool g_uiLayerLive;
extern bool g_uiLayerWatching;
extern bool g_uiSeedDiagnostics;
extern bool g_uiLayerRedirecting;
extern bool g_uiLayerIssueBlocked;
extern bool g_uiLayerCrispOn;
extern bool g_uiLayerCrispPending;
}  // namespace detail

// The draw path's one gate: fix.ui_quality is on, a temporal mode is on, and
// the layer has not stood down. One load.
inline bool uiLayerLive() { return detail::g_uiLayerLive; }
// A persistent driver restoration fault leaves stock shader state untrusted.
// Owner-context issues stay suppressed, Begin's fallback included, for the rest
// of the frame it happened in: uiLayerFrameBoundary then puts back whichever of
// the hologram PS and b13 is still EDVR's and lifts this (or lifts it anyway
// after eight failed boundaries). The layer itself stays stood down for the
// session either way.
inline bool uiLayerIssueBlocked() { return detail::g_uiLayerIssueBlocked; }

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
// than swallowing it or re-issuing it. substituted: the draw will be issued
// through the curved screen's own geometry, which the second issues below
// cannot repeat (so a multiply or a depth/stencil write through it stays in
// the frame). knownEye: -1 asks uiDepthEyeOfTarget, as every real UI family
// does; uiLayerNoteOther passes the eye it already knows instead (the
// target IS the one that eye's UI was taken from this frame), rather than
// re-deriving an answer that must agree with it by construction.
bool uiLayerDecide(ID3D11DeviceContext* ctx, int family, bool verdictForwards, bool substituted,
                   int knownEye = -1);

// The family census (vscreen.cpp, owner draws while live): one draw of the
// menu panel's or the loading screen's composite vertex shader, the family
// the rule gave it (UiLayerFamily as an int; 0 none) and how
// (ui_layer_math.h's UiFamilyWhy as an int), with its pixel shader where the
// rule asked for it. Counted per window and reported every 30 s -- the
// draws the rule turned away are otherwise invisible, since a draw with no
// family never reaches uiLayerDecide.
void uiLayerNoteFamilyProbe(uint64_t vs, uint64_t ps, int family, int why);

// Around one issue of a decided draw: bind the eye's layer as the only
// render target (with the layer's own seeded depth-stencil target when the
// draw tests depth or stencil), the viewports and scissors through the map
// with the jitter cancelled, the blend converted (ui_layer_math.h). Begin
// returns false -- and End is then a no-op -- if the draw's state at the
// moment of issue refuses (a blend changed by a verdict's own Begin, a seed
// that failed), in which case the draw goes to the game's frame as always.
// Exception: uiLayerIssueBlocked suppresses an unsafe issue after both guarded
// original-shader restoration attempts fault, until the frame boundary settles
// it; this is not a stock fallback.
// Every state change goes through the raw entry points, so the binding
// shadow keeps describing the game's.
bool uiLayerBegin(ID3D11DeviceContext* ctx);
void uiLayerEnd(ID3D11DeviceContext* ctx);

// After a decided draw's issue, its second issues -- each only when it
// returns true, each closed by the End beside it:
//
// a multiply's second draw, into the per-channel transmittance, bracketed
// like the first (the draw re-issued as it was; closed by uiLayerEnd);
bool uiLayerMultiplyBegin(ID3D11DeviceContext* ctx);
// a depth or stencil write, kept in the game's own buffer: the game's depth
// target and state with NO colour target bound, the game's viewports and
// scissors (the draw re-issued as it was; closed by uiLayerWriteBackEnd).
bool uiLayerWriteBackBegin(ID3D11DeviceContext* ctx);
void uiLayerWriteBackEnd(ID3D11DeviceContext* ctx);

// the crisp-HUD half of fix.ui_quality (Phases 1-3 of docs/cockpit-hud-layer-design-2026-09-27.md): the
// cockpit's HDR HUD families (the holo panels, the flight HUD, the target
// sprite, the holograms) are taken into a per-eye HDR layer by the ordinary take
// path above (g_draw.hdr), and reach the eye at the game's own tonemap draw,
// re-issued once per eye per frame with the HDR layer as its HDR source, into
// the 8-bit layer -- which the door's composite then shows unchanged.
//
// uiLayerCrispOn: the draw path's one gate, beside uiLayerLive -- the key is
// on, a temporal mode is on, the jitter switches are as shipped, and the HDR
// path has not stood down. One load.
inline bool uiLayerCrispOn() { return detail::g_uiLayerCrispOn; }
// From vscreen's eye-draw branch (owner context, uiLayerCrispOn()): is this
// draw the game's tonemap, admitted for the re-issue (tonemap_admit.h's
// structural admission, shared with the HUD layer census)? True only when the
// eye's HDR layer holds this frame's HUD draws and every ordering guard
// passes; then the caller skips its after-UI read check for the draw and
// brackets the draw's own issue with uiLayerCrispToneBegin/End.
bool uiLayerCrispNoteEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances,
                             uint32_t startInstance);
// One load for the ordinary draw between admission and issue: an admission is
// outstanding, so forwardWithVerdict must run the re-issue bracket after the
// game's own issue.
inline bool uiLayerCrispPending() { return detail::g_uiLayerCrispPending; }
// Around the re-issue of the admitted tonemap draw (the game's own draw,
// re-issued by the caller between them): Begin binds the 8-bit layer, the
// disabled RGB-only blend, the full-layer viewport and the HDR layer at the
// admitted HDR slot -- everything else stays the game's, still bound from its
// draw -- and returns false with the game's state untouched on any decline
// (each counted, each named once a session). End puts the game's state back
// through the raw entry points, runs the coverage pass (the HDR layer's
// transmittance into the 8-bit layer's alpha), and marks the layer's frame.
bool uiLayerCrispToneBegin(ID3D11DeviceContext* ctx);
void uiLayerCrispToneEnd(ID3D11DeviceContext* ctx);

// True between a successful uiLayerBegin and its End (owner context only):
// the passes that ride the game's own draw -- the screen's motion and UI
// mask, the mesh motion (screen_motion.cpp, vscreen.cpp) -- stand aside for
// a draw the layer has taken: its pixels are no longer in the pass's input,
// and the bound target and viewport are the layer's. One load, inline: it
// is asked on every owner draw those passes see, key off or on.
inline bool uiLayerRedirecting() { return detail::g_uiLayerRedirecting; }

// After the first redirected draw of a frame: one load. When true, every
// other owner draw is shown to uiLayerNoteOther, which decides whether a
// draw that WRITES an eye target the UI was taken from should be taken into
// the layer too, after the UI (so it stays over it) -- unless it is a post
// pass (an eye-sized input) or the take path refuses it at issue, both left
// as before -- and returns true exactly when it was, so the caller can
// bracket it with uiLayerBegin/uiLayerEnd like any other decided draw. A
// draw that only READS the target (a full-screen pass: count <= 6 vertices)
// is left alone and never taken; it no longer sees the UI in what it reads.
// verdictForwards and substituted are the same facts uiLayerDecide takes for
// a real UI family (vscreen.cpp's forwardWithVerdict already has them to
// hand). Every case is counted and each shader pair named once.
inline bool uiLayerWatching() { return detail::g_uiLayerWatching; }
inline bool uiLayerSeedDiagnostics() { return detail::g_uiSeedDiagnostics; }
bool uiLayerNoteOther(ID3D11DeviceContext* ctx, uint32_t count, bool verdictForwards, bool substituted,
                      bool excluded, bool panelSized, uint32_t instances, uint32_t verdict, char drawKind);
// Completes CPU-only observation after the forwarding/substitution decision.
void uiLayerSeedDrawOutcome(bool forwarded, bool substituted, bool redirected, bool outcomeKnown);
// A game clear of a depth-stencil view, while watching: when it clears the
// buffer a layer's depth-stencil target was seeded from this frame, the
// layer's copy is stale and the next tested draw seeds it again. (A game
// draw that writes it is caught by uiLayerNoteOther the same way.)
void uiLayerNoteDepthClear(void* dsv, uint32_t flags, float depth, uint8_t stencil);

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

// Once per frame, from vScreenFrameBoundary: first, the settle of a fence a
// failed hologram restore raised (uiLayerIssueBlocked above); then the
// shader's warm compile, the 30-second totals, the per-frame watch reset.
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
