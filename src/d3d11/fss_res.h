// Render targets created larger than the game asked for: the Full System
// Scanner's body layer, and any surface named by size.
//
// Two matchers share one mechanism here -- the tracking, the viewport
// scaling and the eye-test exclusion are the same three moves whichever
// rule fired. The FSS rule is described first because it is the one that
// was measured into existence; the size-named rule is a developer
// instrument added 2026-09-02 and is described under THE SECOND MATCHER.
// (If that instrument ever graduates to a shipped fix, this module wants
// renaming: it is no longer only about the scanner.)
//
// WHY THIS EXISTS (docs/fss-scanner.md, 2026-08-25)
//
// The FSS renders the zoomed body -- planet and rings -- once per frame,
// MONO, into one offscreen target of exactly HALF the eye size per axis,
// then pastes that image into each eye through per-eye transforms. Measured
// end to end across three instrumented flights: the halving is why the body
// reads as a pixelated sprite, and the per-eye RESAMPLE of a half-res image
// is the last mechanism standing for the ring split -- a thin, low-contrast,
// edge-on ring aliases differently under two different sample grids, so one
// eye shows segments the other misses while the ring streams in. Every other
// channel was measured symmetric: the content (round two), the constants
// (round three), and the delivery (the snapshot A/B, null).
//
// WHAT IT DOES
//
// Three small moves, all identity-tracked, all off unless
// experimental.fss_res = 1:
//
//   1. CreateTexture2D (device_hook's hook, this module's match): a
//      render-target or depth texture asked for at exactly eye/2 x eye/2 is
//      created at DOUBLE the requested size instead. The FSS body target
//      and its depth buffer are the only textures that shape ever drawn --
//      measured by the auto-census's own trigger, which needed 15,654
//      frames of flight, menus and supercruise to see a single draw land
//      in one.
//   2. RSSetViewports (vscreen's hook, this module's answer): a viewport of
//      exactly the ORIGINAL half size, set while an inflated texture is the
//      bound target, is scaled x2 -- so the game's draws fill what was
//      actually allocated. A draw-time backstop catches a viewport set
//      before the target was bound.
//   3. targetIsEyeSized (vscreen): the inflated textures are now exactly
//      eye-sized, and are excluded from the eye test BY IDENTITY -- the
//      same collision the on-foot panel documents, solved the same way.
//
// The composite needs no help: it samples the body through normalized UVs,
// so a fuller texture simply arrives sharper. Round two measured the body
// target taking only draws -- no copies, no compute, no resolves land in it
// -- which is what makes inflation this small.
//
// REFUSAL: any doubt (initial data, mips, MSAA, arrays, no eye size
// published yet, a failed create) falls through to the stock size, and the
// game renders exactly as without EDVR.
//
// THE SECOND MATCHER: advanced.surface_inflate (2026-09-02)
//
// Elite builds the cockpit's holographic panels in offscreen INTERFACE
// SURFACES -- vector geometry (vs 666EF0C4C616F67E, whose pixel shader has
// no sample instruction at all) and text from a 2048x2048 glyph atlas,
// rasterised into a texture that the cockpit's own meshes then sample onto
// the panel surfaces. Read out of a cockpit census: an interface surface is
// an odd, non-power-of-two render target with a depth partner of the same
// size, and the cockpit's mesh draws carry it in a sampler slot.
//
// Those surfaces are a fixed fraction of the game's INTERNAL render
// resolution, not of the submitted eye texture. Measured across two
// sessions on one rig: a panel came out 908x1361 with the scene rendering
// at 4340x4284, and 1363x2042 with the scene at 6510x6426 -- the same
// fraction to four significant figures, three surfaces agreeing. So the
// game already has this knob, and it is its own supersampling setting:
// turning it up re-rasterises the panels sharper, and charges 2.25x the
// pixels for the WHOLE SCENE to do it.
//
// This matcher separates the two. A size named in advanced.surface_inflate
// is created N times larger and its viewport scaled to match, so the game
// rasterises that one panel bigger while the scene is left alone. The
// content is vector and a large glyph atlas, so what comes back is real
// detail and not a resample -- which is why this is here rather than in the
// FSR path. docs/intro-video.md is the opposite case and says so: a fixed
// 1920x1080 decode, nothing to re-render, resampling the only option.
//
// The strongest evidence that this is safe to do is that the game already
// does it to itself: the same GUI renderer produces correct panels at both
// 1.0x and 1.5x, so it is not carrying a hardcoded surface size. What EDVR
// changes is which textures get the larger one.
//
// A DEVELOPER INSTRUMENT, and named by size on purpose: which surface is
// which is not knowable from outside a session. Take a cockpit census with
// advanced.census_offscreen = 1, read the odd sizes that have a depth
// partner, and name one. Off by default, and free when off.
#pragma once

#include <cstdint>

struct D3D11_TEXTURE2D_DESC;

namespace edvr {

class Config;

// Reads experimental.fss_res and advanced.surface_inflate. Called at install
// and on the ini reload path, so both are live -- but only for textures
// created AFTERWARDS. For the FSS that is the next zoom; an interface
// surface is built when its panel is, so a named size takes effect on the
// next trip through the main menu, not mid-flight.
void fssResConfigure(Config& cfg);

// One bool for the CreateTexture2D hot gate.
bool fssResWantsCreates();

// The match: if *d is the half-eye body-layer shape, or a size named by
// advanced.surface_inflate, multiply its Width and Height in place and
// return true; the caller creates with the modified desc and reports the
// texture back through fssResNoteCreated. false leaves *d untouched.
bool fssResMaybeInflate(D3D11_TEXTURE2D_DESC* d, bool hasInitialData);

// Track a texture created inflated, with the size the game ASKED for -- the
// size its viewports will arrive in -- and the factor it grew by. The caller
// has both descs in hand and passes the factor rather than this module
// stashing one between the two calls: CreateTexture2D runs on the game's
// streaming threads, and a pending-factor global would be a race that
// mis-scales a viewport rather than one that crashes.
void fssResNoteCreated(void* texture, uint32_t origW, uint32_t origH,
                       uint32_t scale);

// Identity test for the eye-classification exclusion and the viewport
// hooks. Compares pointers only; a stale entry for a texture the game
// released costs a compare and nothing else.
bool fssResIsInflated(void* resource);

// The original (pre-inflation) size recorded for an inflated texture. False
// when the resource is not one of ours.
bool fssResOrigSize(void* resource, uint32_t* w, uint32_t* h);

// The factor that texture grew by, and 1 for anything untracked. The
// viewport paths multiply by this rather than by a constant 2: the FSS rule
// always doubles, but a named surface may ask for more.
uint32_t fssResScaleOf(void* resource);

// Anything tracked at all? The viewport paths gate on this so a session
// that never opens the FSS never pays a resolve.
bool fssResActive();

// The viewport paths' receipts: scaled at RSSetViewports, or caught late by
// the draw-time backstop. Capped log lines; the counts land in the note.
void fssResNoteViewportScaled(bool late);

}  // namespace edvr
