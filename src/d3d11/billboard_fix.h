// Billboard sprites that keep their orientation when your head moves.
//
// THE CLASS (measured 2026-08-19/20 at a parked sun; the witchspace corona
// is the same artifact): world-positioned sprites -- the sun's flare
// starburst, coronas -- whose POSITION is correct but whose ORIENTATION is
// built from the camera's axes, so they spin about their own centre under
// head roll and yaw. A monitor never shows it; a headset always does.
//
// THE MEASUREMENT THAT MAKES THE FIX (cb_peek, 2026-08-20): each sprite's
// vertex constants -- a small per-sprite buffer the game multiplexes
// write-by-write -- carry BOTH orientations. Floats [16..22] are the DRAWN
// basis: two orthogonal vectors, magnitude = the sprite's scale, rotating
// with the head across a recorded roll/yaw sweep. Floats [36..46] are a
// clean world-anchored frame -- middle row exactly (0,1,0) -- that never
// moved through the same sweep. The game hands us the correct answer and
// then draws with the wrong one.
//
// THE FIX: for each matched draw whose freshly-written constants pass a
// shape check, substitute a copy in which the drawn basis is rebuilt from
// the buffer's OWN world frame, preserving the original magnitude --
// orientation from the world, scale from the sprite, per write. No head
// data is consulted at all; the correction is self-contained in what the
// game wrote. The game's buffer is never modified -- the panel-distance
// substitution discipline, aimed at eleven floats.
//
// The shape check is the safety: two equal-magnitude orthogonal vectors in
// the drawn slots, an orthonormal frame in the world slots. A write that is
// not a billboard's fails it and draws untouched.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.billboard (stock | steady). Both config paths, live.
void billboardConfigure(Config& cfg);
bool billboardWantsDraws();

// Every eye draw while enabled: matches the sprite family (X, eye-sized
// depth in PS slot 0, a texture in slot 1, 64+ indices) and tracks which
// constant buffer the family is writing through. True when THIS draw is a
// match with a captured, shape-approved write to substitute -- the thunk
// then wraps the draw in billboardBegin/End.
bool billboardOnEyeDraw(char kind, uint32_t count, uint32_t instances);

// The glare-steady loan: sun_glare_steady borrows this module's shadow
// and substitution wholesale -- the glare train writes the same 208-byte
// camera-standard layout. The matching already happened in sunglare_fix;
// this is the family path minus the family test. billboardGlareWatch
// arms the tee while the borrower is configured on.
void billboardGlareWatch(bool on);
bool billboardOnGlareDraw(uint32_t count, uint32_t instances);

// The shadowed write's floats, for a borrower that only MEASURES: the
// glare steer reads the head's roll out of the camera rows and touches
// nothing -- both replacement formulas displaced the elements per eye,
// because the rows are the view matrix and position flows through them.
// Null until a write has been captured for the current target.
const float* billboardShadowFloats(uint32_t* count);

// Milliseconds since the shadow content last updated (~0 if none). A
// fresh-looking shadow of the WRONG buffer stays fresh forever; the age
// alone cannot prove identity, but a stale one disproves it.
uint64_t billboardShadowAgeMs();

// The buffer whose writes the Map/Unmap tee should capture, or null.
// Compared by the Map hook, never dereferenced there.
void* billboardTarget();

// A fresh write to the watched buffer, from the Unmap tee.
void billboardCapture(const void* data, uint32_t bytes);

// Around the real draw: bind our substituted copy of the constants;
// restore the game's buffer after. Begin failing to build degrades to the
// draw running untouched.
void billboardBegin(ID3D11DeviceContext* ctx);
void billboardEnd(ID3D11DeviceContext* ctx);

void billboardShutdown();

}  // namespace edvr
