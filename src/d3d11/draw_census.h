// A hotkey-armed census of the draws that land in the eye textures.
//
// WHY THIS EXISTS
//
// Frontier issue 69074: the RemLok helmet's edge overlay -- the faint lines
// that appear when the canopy is gone -- is drawn into the WRONG eyes, so the
// lines sit along the nose instead of at the temples. Any fix (skipping the
// draws, or handing them a corrected transform the way the panel distance fix
// does) first needs those draws IDENTIFIED, and nothing about them is knowable
// outside a live session: not their shader, not what they sample, not where in
// the frame they land.
//
// So this is the measuring instrument, not the fix. Press the census key with
// the effect absent, press it again with it present -- each press logs a few
// whole frames of every draw that reaches the eye textures, with what each one
// read and wrote -- and tools/diff_draw_census.py names the draws only the
// second census contains. General-purpose on purpose: anything a module
// toggle or game state can flip on and off can be isolated the same way.
//
// It records only when armed. Unarmed, the entire module is one bool read per
// eye draw.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// Is a census pending or capturing? Read on the draw path; it is what keeps
// beginPanelOverride counting eye draws while a census runs even when every
// other subscriber of that count is off.
bool drawCensusArmed();

// The hotkey. Arms a capture of the next few WHOLE frames -- recording starts
// at the coming frame boundary, so a census never contains a partial frame.
// A press while armed is refused with a note rather than restarting the
// capture.
void drawCensusRequest();

// One draw that reached an eye texture, called between the bindings being
// read and the draw being forwarded. kind: 'D' Draw, 'I' DrawIndexed,
// 'N' DrawInstanced, 'X' DrawIndexedInstanced. count is the vertex or index
// count, instances is 1 for the non-instanced kinds. eyeDrawIndex is
// vscreen's running count for this frame, so a line can be placed within the
// frame it came from.
//
// ctx is the immediate context the draw is about to run on -- vscreen has
// already established that it is ours -- and it is here for the input
// assembler.
//
// WHY THE CONTEXT AND NOT THE SHADOW
//
// Everything else on a census line comes from binding_shadow, because
// something in the tree hooks the call that sets it. Nothing hooks
// IASetVertexBuffers, IASetPrimitiveTopology or VSSetShader, and hooking them
// to answer a question asked for three frames a session would put two more
// patched vtable entries on the hot path -- two more slots for the D3D runtime
// to re-point out from under us, which is the failure that cost the 0.7.x
// line a 64-round war. IAGet*/VSGetShader read the same state with no patch
// at all, and cost nothing when no census is running, which is almost always.
//
// What they buy: the curved-screen work (docs/screen-curvature.md) turns on
// whether the panel composite draws a real vertex buffer in a local space --
// substitutable, cheap -- or synthesises its corners in the shader, which
// would mean reverse-engineering the transform. The stride and topology
// settle it. The vertex shader's identity settles a second question the same
// session: whether HMD Cinema Mode's two composite draws share a shader, and
// so whether recognising by shader would catch the eye that today's
// panel-sized-SRV test misses.
void drawCensusEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances, uint32_t eyeDrawIndex);

// The frame edge, from vScreenFrameBoundary: starts a pending census, advances
// a running one, finishes a spent one. frameNo is vscreen's frame counter,
// logged so a census can be lined up against the rest of the log.
void drawCensusFrameBoundary(uint32_t frameNo);

}  // namespace edvr
