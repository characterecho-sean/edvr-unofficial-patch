// Reading the four vertices the on-foot panel is drawn on.
//
// WHY THIS EXISTS
//
// The curved-screen work (docs/screen-curvature.md) substitutes a bent grid
// for the panel composite's flat quad and leaves everything else of the
// game's alone -- its vertex shader, its pixel shader, its transform. That
// only works if the grid is in the SAME vertex format as the quad it
// replaces, and the census settled the shape of that format without settling
// the format itself: an 80-byte vertex buffer at stride 20, offset 0, drawn
// as six indices in a triangle list. Four vertices of twenty bytes.
//
// Twenty bytes is float3 position plus float2 UV and very little else, but
// "very little else" is not a format. A grid built on a guess renders as a
// skew, a mirrored image or nothing, and each of those costs a flight to
// diagnose. Eighty bytes read once costs nothing and answers it outright --
// including the two things the stride cannot say: which corner is which, and
// whether V runs up or down.
//
// THE INSTRUMENT: at a recognised composite, GPU-copy the whole vertex buffer
// into a staging buffer; read it back a few frames later, when the copy has
// certainly executed and mapping it will not stall; log the vertices as
// floats and as hex; then stand down for the session. The quad is a static
// asset, so once is all there is to learn.
//
// Off by default, and free when off: nothing is created, nothing is copied,
// and the composite path does not call in.
#pragma once

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Retired instrument (no key read; always off) -- its capture is in the docs, and turning it
// off and on again arms a second capture, which is the only way to ask for
// one in a different mode without restarting.
void panelQuadConfigure(Config& cfg);

// Is a capture still wanted? False once one has been taken, which is what
// keeps this out of the draw path's early-return condition for the rest of
// the session.
bool panelQuadWants();

// Called at a draw already recognised as the panel composite -- eye-sized
// target, panel-sized SRV0. Runs the copy on the first such draw and the
// readback on a later one.
void panelQuadOnComposite(ID3D11DeviceContext* ctx);

// Releases the staging buffer. Called from the vScreen shutdown, which is the
// only place that knows the device is still alive.
void panelQuadShutdown();

}  // namespace edvr
