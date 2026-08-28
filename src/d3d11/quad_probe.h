// Reading the rectangles a batched UI draw actually paints -- every draw of
// that shape in one frame, not just the first.
//
// WHY THIS EXISTS
//
// Elite draws its solid UI rectangles through one textureless shader into one
// shared 4 MB vertex buffer, and a census names such draws only by signature:
// kind, index count, target. Signatures COLLIDE. The loading screen's
// full-view backdrop and the dialog's own box are both 30-index bordered
// panels, and every signature-matched instrument -- the offscreen skip, the
// sub-draw skip -- hits both at once. That co-disappearance was read for a
// while as proof they were one batched call; it was two calls sharing a
// signature, and the first build of this probe, which captured only the FIRST
// match per session, could never have shown the second one.
//
// So: at the first frame containing a match, capture EVERY matching draw --
// each draw's index range, the shared vertex buffer once -- and log, per
// occurrence, its baseVertex, startIndex and each quad's rectangle. Two
// occurrences with the same signature and different extents settle in one
// flight what a week of skip probes could not.
//
// Each quad's line also carries the REST of its first vertex as hex: the
// stride is 24 and only the float2 position at offset 0 is understood, so
// those sixteen bytes are where a colour, a UV or a per-element transform
// would live. Two same-shaped draws that differ only there -- a translucent
// backdrop against a solid black box -- differ in exactly those bytes.
//
// THE INSTRUMENT: GPU-copy at the matched draws, read back a few frames
// later when the copies have certainly executed and mapping will not stall,
// log, stand down for the session. Setting the spec again -- off and back on
// -- asks for another capture without a relaunch. panel_quad.h established
// the copy-settle-map shape for a simpler case.
//
// Off by default, and free when off: nothing is created, nothing is copied,
// and the draw path does not call in.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads advanced.quad_probe = WIDTHxHEIGHT:KIND:COUNT[:SKIPFRAMES] -- the
// skip and clip specs' shape naming one draw signature into one target,
// plus an optional count of matching frames to let pass before capturing
// (the first matching frame is usually mid fade-in; 120 waits about two
// seconds of them for steady state). Empty is off.
//
// ANY target, since 2026-08-28: this probe used to be offered only draws
// that missed the eye textures, because the loader's widget panels are all
// built in an interface surface. The intro movie's panel is not -- it is a
// 6-index quad drawn straight into the eye (docs/intro-video.md) -- and a
// spec naming the eye's own size used to match nothing, silently. It is now
// offered every draw.
//
// COUNT means indices for I and X, six to a quad, and VERTICES for D and N,
// where the only shape decoded is the four-vertex strip quad. The
// non-indexed kinds were refused outright until the intro flight needed
// them: the movie's composite turned out to be a full-screen quad with
// nothing in it to move, which puts the question on the draw that FILLS its
// source surface -- an N of four vertices, a shape this could not be aimed
// at.
//
// For those kinds the caller passes the draw's START VERTEX in baseVertex.
// It plays the same role there that baseVertex plays for an indexed draw --
// what a vertex's index is offset by -- and vscreen's non-indexed thunks
// stash it for exactly this.
void quadProbeConfigure(Config& cfg);

// Is a capture still wanted? False once one has been taken, which keeps this
// out of the draw path's condition for the rest of the session.
bool quadProbeWants();

// Does this draw match? Called per draw; the first match opens a one-frame
// capture window and every match inside it is recorded as an occurrence.
bool quadProbeOnDraw(ID3D11DeviceContext* ctx, uint32_t targetW,
                     uint32_t targetH, char kind, uint32_t count,
                     uint32_t instances, uint32_t startIndex, int baseVertex);

// Once per frame: close a capture window at its frame's edge, retire the
// settled copies, decode and log every occurrence. Cheap when nothing is
// pending.
void quadProbeTick(ID3D11DeviceContext* ctx);

void quadProbeShutdown();

}  // namespace edvr
