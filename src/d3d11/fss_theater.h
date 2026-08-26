// FSS Theater: the zoomed scanner as a virtual screen.
//
// THE FIELD'S OWN DESIGN (2026-08-26, docs/fss-scanner.md rounds 33-40):
// when the scanner zooms onto a body, the surrounding world goes entirely
// black -- the view IS a 2D screen already, rendered twice with the
// engine's per-eye reveal inconsistency (the black squares) as the only
// difference. So render it ONCE: capture the right eye's image and show
// that single texture to both eyes as a panel at a chosen distance,
// drawn by EDVR with each eye's own projection. One rendering, two
// displays -- the entire class of per-eye FSS artifacts becomes
// impossible by construction, and the panel gains the distance (and
// later curvature) controls the other screens already have.
//
// The renderer is a compute pass (no graphics state touched): per output
// pixel, build the eye ray from the published frustum tangents, rotate it
// by the head's motion since the zoom began (the game is fed a FROZEN
// pose while the theater is up, so its render is stable and head-look
// moves the viewer relative to the panel, the cinema-mode feel),
// intersect the panel plane, sample the captured image or return black.
//
// fix.fss_theater = 0 off | distance in meters (1.0-10.0). Live.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {
// Compile the panel shader ahead of need. The first engage otherwise pays
// D3DCompile on the submit path -- 142 ms measured 2026-08-26, which is
// most of a squares arrival watched at stock. Any draw-path site may call
// this every draw; only the first call does work.
void fssTheaterWarm(ID3D11DeviceContext* ctx);
}  // namespace edvr

extern "C" {
// contentTex: the right eye's submitted texture (ID3D11Texture2D*).
// eye: 0 left, 1 right (chooses the frustum orientation and eye offset).
// outerMag/innerMag: horizontal frustum tangent magnitudes.
// xf: 12 floats -- a row-major 3x3 rotation taking current-head vectors
// into frozen-head space (the head's look-around since the zoom began),
// then this eye's ray origin in frozen-head space (head translation and
// the eye's lateral offset folded in). The world lock is carried entirely
// here: OpenComposite attributes the render to the live pose it handed
// out, not the frozen pose the game was fed, so the compositor cannot
// reproject the difference for us (the 2026-08-26 first flight's panel
// rode the head).
// dist: panel distance in meters.
// Returns the theater-rendered texture for this eye, or null = stock.
// scale: fraction of the content's native angular width (the on-foot
// screen's idea of size). curve: 0 flat, up to 0.9 wrapped on a cylinder
// whose arc centre sits at the screen distance.
__declspec(dllexport) void* edvrFssTheater(void* contentTex, int eye,
                                           float outerMag, float innerMag,
                                           const float* xf, float dist,
                                           float scale, float curve);
}
