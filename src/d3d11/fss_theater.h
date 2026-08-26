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

extern "C" {
// contentTex: the right eye's submitted texture (ID3D11Texture2D*).
// eye: 0 left, 1 right (chooses the frustum orientation and eye offset).
// outerMag/innerMag: horizontal frustum tangent magnitudes.
// delta: row-major 3x3 rotation taking current-head vectors into
// frozen-head space (the head's look-around since the zoom began).
// dist: panel distance in meters.
// Returns the theater-rendered texture for this eye, or null = stock.
__declspec(dllexport) void* edvrFssTheater(void* contentTex, int eye,
                                           float outerMag, float innerMag,
                                           const float* delta, float dist);
}
