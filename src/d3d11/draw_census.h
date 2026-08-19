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
void drawCensusEyeDraw(char kind, uint32_t count, uint32_t instances,
                       uint32_t eyeDrawIndex);

// The frame edge, from vScreenFrameBoundary: starts a pending census, advances
// a running one, finishes a spent one. frameNo is vscreen's frame counter,
// logged so a census can be lined up against the rest of the log.
void drawCensusFrameBoundary(uint32_t frameNo);

}  // namespace edvr
