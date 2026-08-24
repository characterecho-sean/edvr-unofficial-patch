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

// Is the census also recording draws that land OUTSIDE the eye textures?
// Read once when a census starts, never on the draw path.
//
// WHY THIS EXISTS (2026-08-24, the FSS ring split)
//
// A census of an FSS view came back with 220 draws a frame, and skipping
// every one of the two largest families -- 68 draws an eye each, 136 a frame,
// millions over a session -- removed nothing visible. The bodies the player
// is looking at are not drawn into the eye textures at all: they are built in
// an offscreen target and composited in, and the eye-sized gate this module
// has always had made them invisible to the one instrument that could name
// them. The gate was right for issue 69074, which was about an overlay
// stamped into the eyes; it is wrong for anything rendered before the
// composite.
//
// OFF BY DEFAULT, and deliberately. A full scene is thousands of draws a
// frame where an FSS view is hundreds, and three frames of that would spend
// the line cap on the way past. Turned on only when the question is "where
// is this drawn", which is exactly when the extra volume is the answer.
bool drawCensusWantsOffscreen();

// One draw that reached a target which is NOT an eye texture, recorded only
// while a census is running AND advanced.census_offscreen is set. Same
// bindings, same shader identity, same line shape as the eye form -- the
// target is on the line as r=, so the interned table names its size, which
// is the whole point of recording these.
//
// Logged as "DCO" rather than "DC" so tools/diff_draw_census.py keeps seeing
// exactly the draw population it was written against.
void drawCensusOffDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances);

// One CopyResource ('R') or CopySubresourceRegion ('S'), recorded while a
// census is running. Logged as "DCC".
//
// WHY COPIES ARE ON A CENSUS AT ALL (2026-08-24, the FSS ring split)
//
// Every draw that reached the eye textures during an FSS view turned out to
// be INVISIBLE: skipping both 68-draw quad families removed nothing, skipping
// the one mesh removed nothing, and binding flat magenta over eleven thousand
// matched draws changed not one pixel. The picture the player sees is not
// drawn into the eye textures -- it is copied there from the offscreen
// targets the bodies are tiled into. A census that records only draws
// therefore cannot see the image at all, which is how three probes in a row
// came back empty while the instrument insisted it was working.
//
// dstX/dstY and the source box are the point for a TILED blit: which region
// moved is what separates "the whole image was copied once" from "tiles are
// landing one at a time", and if the two eyes' copies differ in count or
// region then the split has been measured rather than inferred.
//
// box values are meaningless when hasBox is false (CopyResource copies the
// whole resource) and must not be read.
void drawCensusCopy(char kind, void* dst, uint32_t dstSub, uint32_t dstX,
                    uint32_t dstY, void* src, uint32_t srcSub, bool hasBox,
                    uint32_t left, uint32_t top, uint32_t right, uint32_t bottom);

// The frame edge, from vScreenFrameBoundary: starts a pending census, advances
// a running one, finishes a spent one. frameNo is vscreen's frame counter,
// logged so a census can be lined up against the rest of the log.
void drawCensusFrameBoundary(uint32_t frameNo);

}  // namespace edvr
