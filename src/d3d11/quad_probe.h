// Reading the rectangles a batched UI draw actually paints.
//
// WHY THIS EXISTS
//
// Elite draws its solid UI rectangles in batches: one call, one textureless
// shader, several quads. The loading dialog's translucent wash and the
// dialog's own solid black backing are both in such a call, and from outside
// they are indistinguishable -- same shader, same target, same constants,
// nothing in a draw census that separates one rectangle from another.
//
// Everything tried without this measured nothing and cost a flight each:
// dropping the whole draw takes the dialog with the wash; dropping quads by
// index names them only by what disappears, and a verbal report of what
// disappeared is ambiguous when two dark things overlap; clipping to a
// hand-tuned box resizes whichever quad happens to be in the range, which
// turned out to be the one worth keeping.
//
// The positions are right there in the vertex buffer. Six indices to a quad
// at topology 4, stride 24, position first -- so each quad's rectangle can be
// read outright and the wash told from the backing by SIZE, which is the one
// thing that actually distinguishes them.
//
// THE INSTRUMENT: at a matched draw, GPU-copy the index range and the vertex
// buffer into staging; read them back a few frames later, when the copy has
// certainly executed and mapping will not stall; log each quad's rectangle;
// then stand down for the session. panel_quad.h established this shape for a
// simpler case -- an 80-byte buffer and four vertices, no index resolution.
//
// WHAT IT ASSUMES, and how it tells you when it is wrong. Stride 24 with
// float3 position at offset 0 is inferred from the census, not confirmed. The
// log prints the raw floats beside the decoded rectangles: plausible
// coordinates mean the layout is right, and garbage means it is not and the
// offset needs moving rather than the fix needing rethinking.
//
// Off by default, and free when off: nothing is created, nothing is copied,
// and the draw path does not call in.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads advanced.quad_probe = WIDTHxHEIGHT:KIND:COUNT -- the same shape the
// skip and clip specs take, naming one batched draw into one offscreen
// target. Empty is off.
void quadProbeConfigure(Config& cfg);

// Is a capture still wanted? False once one has been taken, which keeps this
// out of the draw path's condition for the rest of the session.
bool quadProbeWants();

// Does this draw match, and has the capture not been taken? The caller passes
// the draw's own arguments because the verdict path cannot see them.
bool quadProbeOnDraw(ID3D11DeviceContext* ctx, uint32_t targetW,
                     uint32_t targetH, char kind, uint32_t count,
                     uint32_t instances, uint32_t startIndex, int baseVertex);

// Once per frame: retire a capture whose copy has had time to execute, decode
// it and log the rectangles. Cheap when nothing is pending.
void quadProbeTick(ID3D11DeviceContext* ctx);

void quadProbeShutdown();

}  // namespace edvr
