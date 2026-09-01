// One named draw, re-issued with a different stencil REFERENCE and nothing
// else changed.
//
// WHERE THIS COMES FROM (2026-08-31, the black planet in the right eye)
//
// A planetary body renders as a featureless black disc in one eye and
// correctly in the other, in the DSS and FSS only, on a stock game. Two
// weeks of skipping draws one at a time found nothing: every draw in the
// failing eye's lighting pass has now been dropped without the body
// appearing.
//
// What finally separated the eyes was not a skip but a COUNT. Grouping a
// census by render target splits a frame into its two eye passes, and the
// second eye's pass is periodically missing exactly four draws --
// 41E245D488BFE83E, D95905C18B7FAD93, D1281DF454A153AD and
// 5E417E9DF2E7F9E6, always all four or none, never once absent from the
// first eye across eighteen recorded passes. Skipping those four by hand in
// the working eye changes nothing visible, so they are not the body: they
// are a marker of some upstream decision that also goes wrong.
//
// Diffing the SAME eye's pass between a frame that has them and a frame that
// does not leaves exactly one other difference. 9BFC7FD232328391 -- 1800
// vertices, writing all four channels, drawn immediately after where the
// four belong -- runs with
//
//     stencil reference 8   in the frame the body survives
//     stencil reference 16  in the frame the body is black
//
// Every other draw in the pass is identical in state. The reference is a
// CPU-set value, so this is the game choosing a different number, not a race
// on the GPU.
//
// The mechanism that fits every result, including the negatives: at 8 that
// draw's stencil test excludes the body's region and the body survives; at
// 16 it does not, and the draw paints its own content over the body, leaving
// the silhouette the geometry pass laid down in both eyes. Skipping the four
// by hand does not alter the reference, which is exactly why it changed
// nothing.
//
// WHY A REFERENCE OVERRIDE AND NOT ANOTHER SKIP
//
// Skips answer "does this draw put the pixels there". Fifteen have now said
// no. This asks a different question -- "is this NUMBER the difference" --
// and it is the only state in the pass that varies between a good frame and
// a bad one. The state OBJECT is the game's own, re-bound unchanged; only
// the reference passed alongside it differs, so a body that appears can be
// attributed to that number and nothing else.
//
//   vs:HASH:REF   every eye draw running that vertex shader is issued with
//                 stencil reference REF. The game's own state object, its
//                 comparisons, masks and pass-ops are untouched, and both
//                 are put back after the draw.
//
// Both directions are worth running. Forcing 8 should make the body appear
// in the failing eye; forcing 16 should make it vanish in the working one,
// which is the confirmation the first result on its own cannot supply.
//
// Off by default and the only shipped state.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void stencilProbeConfigure(Config& cfg);

// One bool for the draw path's early-out set.
bool stencilProbeWantsDraws();

// Called for every eye draw while armed: true when this draw runs the named
// VERTEX shader. Matched on the vertex shader because that is the key the
// census prints as vh= and every field instruction has been written in.
bool stencilProbeOnEyeDraw(ID3D11DeviceContext* ctx);

// Around the matched draw: re-bind the game's own depth-stencil state with
// our reference, and put the game's reference back. End must run even when
// Begin declined, and does.
void stencilProbeBegin(ID3D11DeviceContext* ctx);
void stencilProbeEnd(ID3D11DeviceContext* ctx);

void stencilProbeShutdown();

}  // namespace edvr
