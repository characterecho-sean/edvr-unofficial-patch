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
// Successful CreatePS hook only: three exact originals (the two hologram
// sphere programs and the frosted base, ui_holo_remap.h), no disk/HLSL input.
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
extern bool g_uiLayerWorldReissue;
extern bool g_uiLayerMapsOn;
extern uint64_t g_uiLayerGateFrame;
extern uint64_t g_uiLayerNamedAt;
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

// The composite census (ui_scene_composites.h; vscreen.cpp forwardWithVerdict, owner draws
// while live): one draw into an eye target that samples a learned interface surface, settled
// after both takes -- taken into the layer, or left in the game's frame, in which case it is
// named by its shaders and the family the rule gave it (UiLayerFamily as an int; 0 none).
// Counted per window and reported every 30 s, zeros included: the composites no family names
// reach no decision and no refusal line, which is how user 5's cockpit panels (Disable GUI
// effects on) stayed in the scene unseen.
void uiLayerNoteCompositeTaken();
void uiLayerNoteCompositeLeft(uint64_t vs, uint64_t ps, int family);

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

// ---- the VR on-foot world route (vr_world_route.h; docs/design-flat-temporal-aa-2026-09-23.md, section 82) ----
//
// On a frame the route owns the world (it resolved the world once, at the tone, and the eye shift is off), the 2D
// screen's composite is not TAKEN by the layer: the game's own draw lands in its eye image as it always did, which
// the game's post pass copies on and which a refused re-issue leaves whole for the eye route. The layer draws it a
// SECOND time, after the game's draw, into the eye's layer -- the game's own shader, opaque, from the route's mipped
// copy of the resolved screen through a trilinear sampler like the game's -- and the door then runs layer-only for
// that eye (native_temporal.cpp, native_sharpen.cpp): the layer's opaque screen over a black frame IS the eye.
// With fix.panel_curvature above 0 the game's draw is the curve substitution's strip (panel_curve.h) and the second draw is that
// strip too: vscreen.cpp issues panelCurveReissue between Begin and End, the same helper that bound the strip for the game's draw,
// so the layer's bend and placement are the game's (the plan accepts a substituted draw as it does a flat one).
// With experimental.temporal_aa_on_foot_world off none of this ever happens (the route never owns a frame): the
// decision, the draws, the jitter and the door are what they were.
//
// The on-foot world-screen gate the layer computes at its frame boundary (the journal's on-foot reading or the
// screen's own busy depth: the 2D screen shows the world). The gate only runs while the layer is live, so the
// answer is stale otherwise -- read uiLayerLiveForWorldRoute beside it. Render thread.
bool uiLayerWorldScreenHeld();
// The layer is live and has not stood down: exactly the condition under which the gate above is computed
// (fix.ui_quality on, a temporal mode on, the jitter switches as shipped, not stood down). A route must not run
// with the layer off. uiLayerNotLiveReason is the one line saying why not, nullptr when it is live.
bool uiLayerLiveForWorldRoute();
const char* uiLayerNotLiveReason();

// One load, for vscreen.cpp's draw path: the 2D screen composite just decided is the route's to RE-ISSUE. Set by
// uiLayerDecide (which then returns false: the draw is not taken), cleared by the re-issue, by
// uiLayerWorldReissueAbandon and at the frame boundary. While it is set the tail of the game's draw skips the
// per-eye screen-motion reissues the layer will make unnecessary (the recognition still runs).
inline bool uiLayerWorldReissuePending() { return detail::g_uiLayerWorldReissue; }
// Right after the game's own issue of that draw: bind the eye's layer (cleared at the first draw of the frame as
// ever), the viewport and scissor through the map with the jitter cancelled, the opaque blend conversion, no depth
// target, the mipped screen at PS slot 0 and a trilinear sampler at PS slot 0 -- every other binding stays the
// game's. False with the game's state untouched on any refusal (counted by reason; the first eight distinct
// reasons are logged once each), and the eye route then serves the eye. The caller issues the game's draw
// once more between Begin and End, exactly as the crisp tonemap re-issue does. All of it runs under
// VrWorldInternalScope, so vscreen's hooks step aside.
bool uiLayerWorldReissueBegin(ID3D11DeviceContext* ctx);
// Puts every binding Begin changed back and, when the re-issue landed, tells the route which eye it took
// (vrWorldRouteNoteEyeTaken). Safe without a Begin. landed false (the curved screen's strip draw faulted after Begin: panel_curve.h
// panelCurveReissue returned false): the bindings go back, the eye is NOT taken, and the refusal is counted as a fault -- the eye
// route serves the eye. Every other caller issues a draw that cannot fail this way and passes nothing.
void uiLayerWorldReissueEnd(ID3D11DeviceContext* ctx, bool landed = true);
// The decided draw went no further (the game's draw was swallowed or never issued): forget the pending re-issue.
void uiLayerWorldReissueAbandon();
// The door's preflight, from treat() before it commits an eye to layer-only: would the composite certainly run
// over `frame` (the black frame the door is about to hand on) for this eye and sequence? 0 when it would; else a
// ui_layer_math.h UiWorldDoorGap as an int, and the door leaves the eye to the eye route in the same call.
int uiLayerWorldDoorGap(uint64_t sequence, uint32_t eye, ID3D11Texture2D* frame);
// The route's counters for the current 30 s window (the same numbers the gates and the route's lines print), so a
// rig or the route's own census can read them: the 2D screen draws asked of the decision and what became of them
// (screenDecided indexes ui_layer_math.h's UiLayerDecision; a draw the route's mode let through is NOT in
// [kRedirect] -- it is `reissued` once the re-issue ran), the draws the game left in an eye image after its screen
// was re-issued (lost while the route owns that eye), and the route's own refusals (refused indexes UiWorldRefuse).
struct UiLayerWorldStats {
    uint64_t screenAsked = 0;
    uint64_t screenDecided[24] = {};   // 24 wide: the orbit lines' and the bars' two decisions (2026-10-07) took the enum past 16
    uint64_t reissued = 0;
    uint64_t lostDraws = 0;
    uint64_t refused[16] = {};
};
UiLayerWorldStats uiLayerWorldStats();

// ---- the on-foot maps gate (experimental.on_foot_maps_sharp; ui_maps_math.h; docs/design-world-camera-motion-2026-09-30.md) ----
//
// With the key on, the layer's world-screen gate is decided by the world camera alone: the 2D screen is the world only while
// a draw that reads the world camera names its source (screen_motion.cpp), 2 frames in a row to hold, 3 to release. A map or a
// menu on foot names nothing, so the layer takes its composite -- sharp, after the upscaler -- and an eye that holds nothing
// else skips the upscaler (the layer-only door below). With the key off none of this is ever true or ever called.
//
// The key is on, the layer and screen motion are live, so the naming decides the gate (latched at the frame boundary, so every
// draw of a frame sees one answer). One load.
inline bool uiLayerMapsOn() { return detail::g_uiLayerMapsOn; }
// screen_motion.cpp, once, at the draw that names the screen's source for the frame in flight (the world camera's terrain or
// scene draw, or the pool-family fallback): the gate judges the frame that ends at the next boundary by it. Attributed to the
// layer's own frame count, so it is right whichever boundary runs first. Two loads and a store.
inline void uiLayerNoteScreenNamed() { detail::g_uiLayerNamedAt = detail::g_uiLayerGateFrame; }
// Did a draw name the screen's source in the frame that has just ended? Valid at any boundary that runs AFTER the layer's (the route's, the
// census's, screen motion's): the layer's own boundary has counted the frame by then, whether or not the layer is live. Independent of every
// key -- screen_motion.cpp tells the layer whenever it names a source -- so the VR camera census reads the naming flips with it.
inline bool uiLayerLastFrameNamed() { return detail::g_uiLayerGateFrame != 0 && detail::g_uiLayerNamedAt + 1 == detail::g_uiLayerGateFrame; }
// vscreen.cpp, at a 2D screen composite the layer took while the maps gate is on: screen motion's recognition of that composite
// matched (it is what keeps naming the world's source for the next frames; a take swallows the per-eye call that used to run it).
void uiLayerMapsNoteRecognised();
// The door runs layer-only for this eye in this sequence: the layer holds the eye's WHOLE picture. Either the VR world route's
// re-issued world (vrWorldRouteDoorLayerOnly) or, with the maps gate on, a 2D screen the layer TOOK in this sequence while
// nothing else was drawn into an eye-sized target (ui_maps_math.h uiMapsDoor). Asked by the temporal door and again by the
// sharpen door for the same eye and sequence, with the same answer.
bool uiLayerDoorLayerOnly(uint32_t eye, uint64_t sequence);

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
// One exception to "left alone": in a frame the crisp re-issue opened, the
// eye's target is the tonemap's output, and Elite's post pass reads it and
// draws into another eye-sized 8-bit target where every interface draw lands;
// that read carries the eye's target there (uiLayerFollowReader, once an
// eye-frame, before any UI draw is taken), so the interface draws are writes
// into it. The pass itself is still left in the frame.
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

#if defined(EDVR_VSCREEN_PREDICATE_TEST)
struct UiLayerPredicateTestTemporalInput final {
    bool active = false;
    uint64_t sequence = 0;
    uint32_t eye = 0;
    float jx = 0.0f, jy = 0.0f;
    uint32_t width = 0, height = 0;
};
void uiLayerPredicateTestSetTemporalInput(
    const UiLayerPredicateTestTemporalInput& input) noexcept;
UiLayerPredicateTestTemporalInput uiLayerPredicateTestGetTemporalInput() noexcept;
#endif

}  // namespace edvr
