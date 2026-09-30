#pragma once

#include <cstdint>

#include "flat_camera_phase.h"

// The upstream camera injector (docs/design-flat-camera-integration.md, the
// C3 wiring plan addendum): a CodeHook detour on the game's view-constant
// refresh (FUN_1405921f0) that applies the temporal phase to the camera's
// frustum parameters, transiently, so every projection-dependent consumer in
// the call derives from the jittered values through the game's own
// finalizers. On whenever the flat profile has a temporal mode selected (no
// setting: the draw-time adapter is only the automatic fallback -- no
// injectable camera, a prologue mismatch after a game update, or too many
// failed writes); with the mode off nothing is installed or logged, and a hook
// that stood down stays inert behind its gate for process lifetime (the
// producer probe's discipline).
//
// The frame protocol flat_runtime drives, in order, once per Present:
//   flatCameraInjectDisarm()      the Present edge: the frame window closes
//   flatCameraInjectClose(...)    the frame that ended is closed (history)
//   flatCameraInjectFrame(n, on)  owner selection for the new frame, the tick
//   flatCameraInjectTakeHistoryReset()  one-shot, honored before the phase
//   flatCameraInjectRoute()       decides whether the phase machine runs
//   flatCameraInjectArm()         the phase is chosen: the window opens
namespace edvr {

// Frame start, called BEFORE the phase machine's beginFrame: ownership begin
// and selection for the new frame, the hook install (once), the 5s tick.
// temporalModeEnabled: a temporal mode is selected (flatCameraPathWanted).
void flatCameraInjectFrame(uint64_t frame, bool temporalModeEnabled);

// This frame's route (Off when the injector is not wanted).
FlatCameraRoute flatCameraInjectRoute();

// True once for a decision that switched the history identity between the
// Legacy and Upstream routes: the runtime resets its temporal history.
bool flatCameraInjectTakeHistoryReset();

// The window the refresh detour may inject in: open after the frame's phase is
// chosen, closed at every Present edge (on the thread that runs Present, which
// becomes the only thread the detour may act on).
void flatCameraInjectArm();
void flatCameraInjectDisarm();

// The frame that just ended, after the phase machine finished it: phaseNonzero
// is its phase, applied whether an injection landed, clean the phase machine's
// own verdict on it, sceneNamed whether a scene depth was named.
void flatCameraInjectClose(bool phaseNonzero, bool applied, bool clean, bool sceneNamed);

// Resize or device change: history and the decision do not survive; cameras
// this session injected stay known so their first un-injected call flushes.
void flatCameraInjectReset();

// What the rest of flat_runtime needs:
bool flatCameraInjectUpstreamOwns();  // this frame's ownership decision is Upstream
bool flatCameraInjectBypassRefusal(const char* reason); // the legacy-only refusal classes

} // namespace edvr
