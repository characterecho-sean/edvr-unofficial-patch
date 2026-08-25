// The Full System Scanner's body layer at full eye resolution.
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
// Three small moves, all identity-tracked, all off unless fix.fss_res = 1:
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

// Reads fix.fss_res. Called at install and on the ini reload path, so the
// flag is live: flipping it changes the NEXT FSS zoom (textures are created
// per zoom), no restart.
void fssResConfigure(Config& cfg);

// One bool for the CreateTexture2D hot gate.
bool fssResWantsCreates();

// The match: if *d is the half-eye body-layer shape, double its Width and
// Height in place and return true; the caller creates with the modified
// desc and reports the texture back through fssResNoteCreated. false leaves
// *d untouched.
bool fssResMaybeInflate(D3D11_TEXTURE2D_DESC* d, bool hasInitialData);

// Track a texture created inflated, with the size the game ASKED for --
// the size its viewports will arrive in.
void fssResNoteCreated(void* texture, uint32_t origW, uint32_t origH);

// Identity test for the eye-classification exclusion and the viewport
// hooks. Compares pointers only; a stale entry for a texture the game
// released costs a compare and nothing else.
bool fssResIsInflated(void* resource);

// The original (half) size recorded for an inflated texture. False when the
// resource is not one of ours.
bool fssResOrigSize(void* resource, uint32_t* w, uint32_t* h);

// Anything tracked at all? The viewport paths gate on this so a session
// that never opens the FSS never pays a resolve.
bool fssResActive();

// The viewport paths' receipts: scaled at RSSetViewports, or caught late by
// the draw-time backstop. Capped log lines; the counts land in the note.
void fssResNoteViewportScaled(bool late);

}  // namespace edvr
