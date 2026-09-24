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

#include "glitch_scene.h"

#include <cstdint>

namespace edvr {

void installGlitchFrameFix();

// glitchFrameInvalidatePool's own and only test (glitch_frame.cpp): the fix
// is installed at all. Necessary and sufficient -- unlike the functions
// below, it does not also ask State::observing.
//
// glitchFrameWantsPool's own necessary first test: installed AND
// State::observing. Both mirror g_state/State::observing, kept in sync by
// syncGlitchFrameDetail() at every site that changes either (glitch_frame.cpp).
namespace detail {
extern bool g_glitchFrameInstalled;
extern bool g_glitchFrameObserving;
}  // namespace detail
inline bool glitchFrameInstalled() { return detail::g_glitchFrameInstalled; }
inline bool glitchFrameObserving() { return detail::g_glitchFrameObserving; }
// The camera validation behind "transition flash fix ACTIVE" has passed: the
// scene camera moved through its first rendered frames (flight, not the menu).
bool glitchFrameCameraValidated();

// Called from the Map/Unmap hooks. The detector picks out the buffers it cares
// about by size, so passing it everything is intended.
bool glitchFrameWantsBuffer(uint32_t bytes);
// `resource` identifies WHICH buffer this is. The camera buffer is recognised by
// its size alone, and a size is not unique -- when validation fails, the first
// thing worth knowing is whether several buffers of that size exist and which one
// was being watched.
void glitchFrameObserve(const void* data, uint32_t bytes, const void* resource);

// Read-only cross-check of the camera buffer bound to a recognised opaque
// eye draw. A fresh same-frame write is required. Saved beside the legacy
// furthest-camera history and paired with the bound pool below.
//
// glitchFrameIsSceneDraw is a pure classifier of five rigid-scene vertex
// shader hashes, defined here inline: the draw lambda asks it for every eye
// geometry draw, and as a cross-TU call (/O2, no /GL) that was a call per
// draw for five compares.
inline bool glitchFrameIsSceneDraw(uint64_t vertexShaderHash) {
    return vertexShaderHash==0xEB5234DB6ADB491Dull || vertexShaderHash==0xDE545DC8EE4FBB87ull ||
        vertexShaderHash==0x61AE8EB05FDC18DDull || vertexShaderHash==0x66DE2CADB1F4AE6Bull ||
        vertexShaderHash==0xAACFDCF2FB9AD809ull;
}
bool glitchFrameWantsSceneDraw(uint64_t vertexShaderHash);
bool glitchFrameNoteSceneDraw(const void* resource, float* sampledPosition = nullptr);

// The bounded geometry cross-check follows the pool actually bound at that
// same draw. Writes are read before Unmap; missing/overwritten data stays
// explicitly unavailable. Coherent geometry excuses auxiliary-camera jumps;
// an unmatched reset into head space marks the frame before Submit. A known
// verdict is retained through later auxiliary writes and both eye submits.
void glitchFrameNoteScenePool(const void* resource, uint32_t bytes);
uint32_t glitchFrameWantsPool(const void* resource);
void glitchFrameObservePool(const void* resource, const void* data, uint32_t bytes);
void glitchFrameInvalidatePool(const void* resource);
GlitchSceneGeometry glitchFrameSceneGeometry();

// advanced.transition_flash_eye_base's per-bad-frame latch asks, at a
// head-only fill, whether the pool's OWN upload for this counter frame is
// already in the store, and if so gets the detector's own comparison run on
// a COPY of that upload with its camera lane set to `camera` (the fill's row
// 275 -- the same value s->sceneDrawPos will get at the draw). READ-ONLY
// with respect to the store: never advances p.prev/p.older (only
// glitchFrameNoteScenePool owns that) and never touches p.write. Returns
// false when no pool slot holds this frame's upload -- the caller reads
// that as "no evidence yet". `snapshot`, when non-null, receives the
// triple the comparison used -- [0] the (camera-replaced) frame upload,
// [1] p.prev, [2] p.older, all COPIES -- so the caller can re-evaluate
// later fills against the LATCH-TIME history instead of the live store
// (which NoteScenePool advances; comparing against it measures the upload
// against itself, the 151942 wouldDiffer artifact).
bool glitchFrameScenePoolEvidence(uint32_t frame, const float camera[3], GlitchSceneGeometry* out,
                                  glitch_scene_detail::Sample snapshot[3]);

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
// The trigger names the key that asked for it, and decides what the header says
// zero milliseconds means -- the history key dumps AT the press, the camera key
// two seconds after it, and a reader who does not know which is subtracting a
// reaction time from the wrong end.
void dumpCameraRing(const char* trigger = nullptr, uint32_t msAfterPress = 0);

// How many radii are currently certified as auxiliary render passes.
//
// FOR TESTS, and specifically for the normal-flight corpus. The corpus asserts
// that nothing the view does may certify, and that assertion has to be exact:
// checking it behaviourally would mean firing a probe excursion at every radius
// the corpus ever visited, and any radius missed is a premise failure that ships.
// One number, checked once, covers all of it.
uint32_t glitchFrameCertifiedShells();

// The churn instrument's counters (SPEC-FLASH-FALSE-POSITIVES §1g), one read.
//
// FOR TESTS, on the shell count's argument: the cells assert exact counts --
// one live eviction, one relearn, splits that move only while the cull
// guard's lie is live -- and probing those behaviourally would mean deriving
// them back out of log text. In the field the same numbers are printed by the
// ring dump and the totals line; nothing reads this there.
struct GlitchFrameChurnStats {
    uint32_t withheldGuardLive;      // withholds while the guard's lie was live
    uint32_t suppressedGuardLive;    // recognitions while it was live
    uint32_t sepInsertions;          // novel magnitudes learned
    uint32_t sepEvictedLive;         // in-window entries evicted (knowledge lost)
    uint32_t sepRelearned;           // insertions matching a recent eviction
    uint32_t shellEvictedCertified;  // certified orbits/parks evicted in-window
};
GlitchFrameChurnStats glitchFrameChurnStats();

void shutdownGlitchFrameFix();

}  // namespace edvr
