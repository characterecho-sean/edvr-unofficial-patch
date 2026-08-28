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
//
// EVERY recorded line carries q=N, one shared per-frame ordinal across draws,
// copies, resolves, updates and dispatches. The per-kind # indexes count
// within their own kind, so nothing about them says whether a copy landed
// between two draws -- and "did anything write the body texture between the
// two eye composites" is precisely the FSS question. Log line order happens
// to preserve it today; q makes it explicit, greppable, and safe against a
// second thread ever contributing lines.
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

// The automatic arm, for builds too brief to catch by hand (2026-08-25, the
// FSS ring split). The FSS body "tiles in" over the first frames after a
// zoom, and those frames are the only ones the bug exists in: a keypress
// census armed by reaction lands twenty frames late at 90Hz and captures the
// settled state, which is how every prior capture measured an ordering the
// build phase was never proven to share. vscreen calls this when a draw
// first lands in a target of the size named by advanced.census_auto after a
// quiet spell; the census that follows records offscreen draws REGARDLESS of
// advanced.census_offscreen, because a capture aimed at an offscreen build
// that did not record offscreen draws is the field session wasted.
//
// Refused silently while a census is armed or running -- the caller fires on
// a per-draw path and must not be able to spam the log.
void drawCensusAutoRequest();

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
//
// Also carries kind 'U' for UpdateSubresource -- the CPU-upload path a
// streamed tile build classically arrives through -- with src null (the
// source is CPU memory, which has no token) and the box as the destination
// region.
void drawCensusCopy(char kind, void* dst, uint32_t dstSub, uint32_t dstX,
                    uint32_t dstY, void* src, uint32_t srcSub, bool hasBox,
                    uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
                    bool foreignCtx = false);

// One draw recorded entirely by DIRECT reads off the calling context -- the
// form for draws the owner-context binding shadow cannot describe: draws on
// deferred/foreign contexts (t=f) and the GPU-driven indirect draws (kind
// 'Y' DrawInstancedIndirect / 'Z' DrawIndexedInstancedIndirect, n=0 i=0,
// args= naming the argument buffer). Round seventeen: the ring draws read
// per-eye surfaces nothing recorded ever wrote, and these were the two draw
// classes no census line had ever carried.
void drawCensusDrawDirect(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                          uint32_t instances, bool foreignCtx,
                          void* indirectArgs, uint32_t indirectOff);

// One CopyStructureCount, logged as "DCS": the call that moves a GPU-side
// element count into an argument buffer -- the write that arms every
// indirect consumer, and the only writer class that could feed the
// round-sixteen argument buffers nothing else touched.
void drawCensusStructCount(void* dst, uint32_t dstOff, void* srcView,
                           bool foreignCtx);

// One ResolveSubresource, recorded while a census is running. Logged as a
// "DCC" line with kind 'V' and the resolve format on the end.
//
// WHY (2026-08-25, the FSS ring split). The settled-state captures showed
// the body drawn into one target (@79 in the recorded session) while both
// eye composites sampled another (@38), and nothing the census recorded
// connects the two: no copy between them appeared, and ResolveSubresource --
// the one call that moves an MSAA render into a sampleable texture -- was
// not hooked at all. If that transfer runs BETWEEN the two eye composites,
// the second eye samples a newer body than the first every frame of the
// build, which is exactly the reported split. This line is how that either
// becomes a measurement or dies.
void drawCensusResolve(void* dst, uint32_t dstSub, void* src, uint32_t srcSub,
                       uint32_t fmt);

// One compute Dispatch, recorded while a census is running. Logged as "DCX"
// with the bound compute shader's content hash and what UAV slots 0-3
// resolve to -- the destinations a compute writer would write.
//
// The hook itself is the exposure fix's, which has always owned slot 41;
// this is only the recording, so a census can finally see the third way a
// texture changes without a draw. Costs one armed-check per dispatch when no
// census runs.
// foreignCtx marks a deferred/foreign context (recorded at build time, not
// playback); indirectArgs non-null marks DispatchIndirect, with n= unset and
// the argument buffer named instead.
void drawCensusDispatch(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y,
                        uint32_t z, bool foreignCtx, void* indirectArgs,
                        uint32_t indirectOff);

// The constant-buffer watch (2026-08-25, the FSS ring split, round two).
//
// The build-phase captures closed the texture channel: both eye composites
// sample the same four resources and nothing writes any of them between the
// two reads, sixty frames measured. What the census could NOT see is the one
// input that is per-eye by construction: the CONTENTS of the constant
// buffers the composite binds. Both eyes bind the same 208-byte object
// (c=@70), which therefore must be rewritten between the passes -- and a
// scan-reveal progress value stepping per WRITE rather than per FRAME would
// show eye B a more-revealed body every frame of the build. That is the
// last game-side channel, and this instrument reads it.
//
// advanced.census_cb_watch names a vertex-shader hash (the census's vh=).
// While a census runs, every eye draw running that shader dumps a DCW line:
// the current CPU-side shadow of its VS b0 and PS b0 contents -- refreshed
// from the Map/Unmap tee, so the dump is exactly the bytes the GPU will
// read for that draw -- with the draw's q, so eye A's dump and eye B's dump
// pair off within the frame. Two dumps per eye per frame; the offline diff
// names the stepping field, or proves the constants identical and closes
// the game side entirely.

// Map/Unmap tee, called from vscreen's hooks while a census is armed. Note
// the mapped pointer at Map; at Unmap -- BEFORE the real Unmap, while the
// memory is still the game's live write -- the census copies the watched
// buffer's bytes into its shadow.
void drawCensusCbNoteMap(void* resource, void* data);
void drawCensusCbNoteUnmap(void* resource);

// UpdateSubresource form of the same tee: the whole write is in hand.
void drawCensusCbNoteUpdate(void* resource, const void* data, uint32_t bytes);

// The readback tick, from vScreenFrameBoundary with the owner context.
// A watched constant buffer the write tee never saw is copied on the GPU
// instead (see draw_census.cpp, kMaxCbReads); this is where the copy is
// mapped and logged, a few frames after it was queued. Free when nothing
// is pending, which is almost always.
void drawCensusTick(ID3D11DeviceContext* ctx);

// The frame edge, from vScreenFrameBoundary: starts a pending census, advances
// a running one, finishes a spent one. frameNo is vscreen's frame counter,
// logged so a census can be lined up against the rest of the log.
void drawCensusFrameBoundary(uint32_t frameNo);

}  // namespace edvr
