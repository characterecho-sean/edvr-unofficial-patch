// GENERATED from src/d3d11/glitch_frame.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 eef107b514392522]
// The one-frame flash at a transition.
//
// Jumping, dropping out of supercruise, or closing the galaxy map occasionally
// shows a single frame drawn from the wrong viewpoint -- a hard cut to somewhere
// else and straight back. On a monitor it is a blink. In a headset it reads as
// the world lurching, which is the part that hurts.
//
// This spots that frame while it is still being drawn and declines to hand it to
// the headset. SteamVR then reprojects the previous frame, which is exactly what
// it does for any frame a game misses, and the flash is not shown.
//
// It cannot repair the frame -- the geometry has already been drawn from the
// wrong place by the time anything here can see it. Withholding is the only
// move available from outside the game. See docs/transition-flash.md for what is
// actually wrong and what a fix inside the game would look like.
//
// Read-only with respect to the game: it looks at a constant buffer the game has
// already filled and writes nothing to it. No game code is modified.
#pragma once

#include <cstdint>

namespace edvr {

void installGlitchFrameFix();

// Called from the Map/Unmap hooks. The detector picks out the buffers it cares
// about by size, so passing it everything is intended.
bool glitchFrameWantsBuffer(uint32_t bytes);
// `resource` identifies WHICH buffer this is. The camera buffer is recognised by
// its size alone, and a size is not unique -- when validation fails, the first
// thing worth knowing is whether several buffers of that size exist and which one
// was being watched.
void glitchFrameObserve(const void* data, uint32_t bytes, const void* resource);

// Called once per frame, after Present. eyeDraws is the number of draws that
// reached the eye textures in the frame just finished -- used to tell a rendered
// scene from a menu or a loading screen, where the camera legitimately teleports
// and withholding achieves nothing.
void glitchFrameBoundary(uint32_t eyeDraws);

// Does the detector need that count at all?
//
// It cannot act without one -- it refuses to judge a frame that does not look
// like a rendered scene -- and the count is produced by the panel-distance code
// in vscreen.cpp as a by-product. That made this fix silently depend on an
// unrelated setting being non-default: with panel_distance at its shipped 1.0
// the counting never ran, so the count was always 0, so no frame was ever
// withheld. Asking here is what keeps the two independent.
bool glitchFrameNeedsEyeDraws();

// Writes the recorded camera history to the log. Bound to a key so a player who
// sees a flash can capture the seconds around it on their own machine.
void dumpCameraRing();

void shutdownGlitchFrameFix();

}  // namespace edvr
