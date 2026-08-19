// The loading hologram's head-locked shimmer, held still.
//
// The loading screen's spinning ship is not the mesh you would expect: the
// model passes render only a DEPTH silhouette, and the visible hologram is
// synthesized by one fullscreen quad per eye that reads the resolved depth
// and modulates a 256x256 pattern over everywhere the ship is. The pattern
// is sampled in SCREEN space -- correct on a monitor, where the camera never
// moves during loading. In a headset the head IS the camera, so the pattern
// is head-locked while the silhouette is world-locked: a low-res, UI-orange
// sheet that scrolls across the hull as you move and sits at zero disparity
// between the eyes. Focusing on it is genuinely nauseating, which is what
// brought it here.
//
// Found with the same instruments as the RemLok overlay (draw census, then
// spec suppression): suppressing the quad removed the ENTIRE visible model,
// which is what proved the model IS the quad -- the ghost is one term of the
// draw that paints the ship, not a draw of its own.
//
// THE FIX: for that one matched draw, substitute sampler slot 1 -- the
// pattern -- with a uniform 1x1 texture, and put the game's texture back
// immediately after. The modulation term becomes a constant, the hologram
// renders steady, and nothing else changes. The uniform's level is
// live-tunable because the right constant depends on how the shader uses
// the term; the field verification (2026-08-19) found 255 correct first
// try, proving the shader multiplies -- steady is the default since.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Read fix.holo_pattern (stock | steady) and advanced.holo_pattern_level.
// Both paths, install and reload; both live.
void holoConfigure(Config& cfg);

// False in stock mode, which keeps the per-draw path free when off.
bool holoWantsDraws();

// Is this eye draw the hologram composite? Matched by shape: a 6-index
// instanced quad, no depth via... the quad BINDS depth for masking, so the
// discriminator is what it samples: slot 0 resolves to the eye-sized depth
// texture and slot 1 to the 256x256 pattern. True means the thunk should
// wrap the draw in holoBegin/holoEnd.
bool holoOnEyeDraw(char kind, uint32_t count, uint32_t instances);

// Around the real draw: bind the uniform texture into PS slot 1, restore
// the game's binding after. begin failing to build the substitute degrades
// to the draw running untouched.
void holoBegin(ID3D11DeviceContext* ctx);
void holoEnd(ID3D11DeviceContext* ctx);

void holoShutdown();

}  // namespace edvr
