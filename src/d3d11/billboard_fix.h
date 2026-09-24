// The billboard constants' shadow and shape check, loaned to the sun-glare
// fix's world shader as its constants tee.
//
// THE CLASS (measured 2026-08-19/20 at a parked sun; the witchspace corona
// is the same artifact): world-positioned sprites -- the sun's flare
// starburst, coronas -- whose POSITION is correct but whose ORIENTATION is
// built from the camera's axes, so they spin about their own centre under
// head roll and yaw. A monitor never shows it; a headset always does.
//
// THE MEASUREMENT (cb_peek, 2026-08-20): each sprite's vertex constants --
// a small per-sprite buffer the game multiplexes write-by-write -- carry
// BOTH orientations. Floats [16..22] are the DRAWN basis: two orthogonal
// vectors, magnitude = the sprite's scale, rotating with the head across a
// recorded roll/yaw sweep. Floats [36..46] are a clean world-anchored frame
// -- middle row exactly (0,1,0) -- that never moved through the same sweep.
//
// The billboard orientation fix that substituted a world-stable basis into
// these constants was retired on 2026-08-23 (the sprite family did not read
// what it substituted), and its code went on 2026-09-23 with the glare's
// corner-rotation steady path. What remains is the capture of the glare
// train's writes and the shape check, which the world shader's telemetry
// reads.
//
// The shape check is the safety: two equal-magnitude orthogonal vectors in
// the drawn slots, an orthonormal frame in the world slots. A write that is
// not a billboard's fails it and is not offered.
#pragma once

#include <cstdint>

namespace edvr {

// The glare train writes the SAME 208-byte camera-standard layout the family
// matcher was built for (the sweep of 2026-08-21 showed its rows pass the
// shape check exactly). The matching already happened in sunglare_fix;
// billboardGlareWatch arms the tee while fix.sun_glare's world shader or its
// probe is configured on, and billboardOnGlareDraw follows the train's
// buffer and says whether this draw's shadowed write passes the shape check.
void billboardGlareWatch(bool on);
bool billboardOnGlareDraw(uint32_t count, uint32_t instances);

// The shadowed write's floats, for a borrower that only MEASURES: the
// glare telemetry reads the head's roll out of the camera rows and touches
// nothing. Null until a write has been captured for the current target.
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

void billboardShutdown();

}  // namespace edvr
