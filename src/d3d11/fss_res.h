// Render targets created larger than the game asked for: the Full System
// Scanner's body layer (experimental.fss_res).
//
// (Two other matchers shared this mechanism until 2026-09-23 -- a developer
// instrument that grew surfaces named by size, and fix.ui_quality's ratio
// match on the interface surfaces. Both are retired: fix.ui_quality sizes
// the interface panels in the game's own panel formula, ui_panel_scale.h.)
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
#pragma once

#include <cstdint>

struct D3D11_TEXTURE2D_DESC;

namespace edvr {

class Config;

// Reads experimental.fss_res. Called at install and on the ini reload path,
// so it is live -- but only for textures created AFTERWARDS: the next zoom.
void fssResConfigure(Config& cfg);

// One bool for the CreateTexture2D hot gate: experimental.fss_res.
bool fssResWantsCreates();

// The match: if *d is the half-eye body-layer shape, double its Width and
// Height in place and return true; the caller creates with the modified desc
// and reports the texture back through fssResNoteCreated. *scaleOut (may be
// null) is the factor applied. false leaves *d and *scaleOut untouched.
bool fssResMaybeInflate(D3D11_TEXTURE2D_DESC* d, bool hasInitialData, float* scaleOut);

// Track a texture created inflated: the size the game ASKED for (the size
// its viewports will arrive in), the size it actually got and the factor
// between them. The caller has all of this in hand already and passes it
// rather than this module stashing it between the two calls: CreateTexture2D
// runs on the game's streaming threads, and a pending value stashed between
// calls would be a race that mis-attributes one create's result to another's.
void fssResNoteCreated(void* texture, uint32_t origW, uint32_t origH,
                       uint32_t newW, uint32_t newH, float scale);

// Identity test for the eye-classification exclusion and the viewport
// hooks. Compares pointers only; a stale entry for a texture the game
// released costs a compare and nothing else.
bool fssResIsInflated(void* resource);

// The original (pre-inflation) size recorded for an inflated texture. False
// when the resource is not one of ours.
bool fssResOrigSize(void* resource, uint32_t* w, uint32_t* h);

// The factor that texture grew by, and 1 for anything untracked. The
// viewport paths multiply by this.
float fssResScaleOf(void* resource);

// Anything tracked at all? The viewport paths gate on this so a session
// that never opens the FSS never pays a resolve.
//
// Inline: asked on the viewport paths, and the build has no /GL to fold a
// cross-TU getter for one scalar load.
namespace detail {
extern uint32_t g_fssResCount;
}  // namespace detail
inline bool fssResActive() { return detail::g_fssResCount != 0; }

// The viewport paths' receipts: scaled at RSSetViewports, or caught late by
// the draw-time backstop. Capped log lines; the counts land in the note.
void fssResNoteViewportScaled(bool late);

// A copy or resolve the game issued with a tracked (inflated) texture as
// either side. This does NOT rescale the copy -- no such copy has ever been
// observed landing in one of these surfaces (the FSS body layer's own
// census found none), and guessing at box math for a shape nobody has seen
// would be a fix built on an untested hypothesis. It counts and logs
// instead, capped, so a flight says whether this ever actually happens and,
// if so, exactly what shape it is.
void fssResNoteCopyMaybeMismatched(void* dst, void* src);

}  // namespace edvr
