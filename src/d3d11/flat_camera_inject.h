#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

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

// The runtime's stand-down (flat_standdown.h) pauses the refresh hook while every
// frame is refused: the relay's gate closes and the game's camera refresh runs
// straight through to the original, and reopens on resume. Called every frame from
// the owner thread with the frame's desired state. A hook that stood down for good
// (write failures) is never reopened by it, and the gate closes only once no camera
// still holds an injected phase, because that camera's first un-injected call is the
// flush and a closed relay would never see it.
void flatCameraInjectPause(bool paused);

// What the rest of flat_runtime needs:
bool flatCameraInjectUpstreamOwns();  // this frame's ownership decision is Upstream
bool flatCameraInjectBypassRefusal(const char* reason); // the legacy-only refusal classes

// ---------------------------------------------------------------------------
// THE OBSERVE-ONLY MODE (2026-09-30): the VR camera census
// (src/d3d11/vr_camera_census.cpp, design doc section 82, "Pre-build
// findings") runs this same detour in the VR profile to learn which cameras
// reach the refresh and how an eye camera differs from the world's. In this
// mode the detour NEVER writes a camera: no bound pair, no dirty flag, no row,
// no flush, and flatCameraAdmit cannot answer Inject (FlatCameraAdmit::Observed).
// The one store it makes is the body's return slot, redirected to stubB so the
// post half can read what the body derived; stubB jumps to the real return
// address, exactly as it does for an injected call. The flat runtime is never
// asked anything: no phase, no legacy plan, no "camera applied" note.
//
// The census's per-frame protocol replaces the flat one above:
//   flatCameraInjectDisarm()         the Present edge: this thread is the owner
//   flatCameraInjectObserveFrame()   installs the hook (once), opens the window
// and it registers its observer with flatCameraInjectSetObserver before the
// first call can arrive.
// ---------------------------------------------------------------------------
struct FlatCameraObserveCall {
    uintptr_t camera = 0;      // r8, the camera struct (non-null: a null camera is not reported)
    uintptr_t ctx = 0;         // rcx, the view-constant context
    uintptr_t p2 = 0;          // rdx
    uint64_t callerRva = 0;    // the call site's offset from the game module, 0 when unknown
    uint64_t callNo = 0;       // the detour's own call counter, 1-based
    uint32_t kind = 0;         // camera+0x264 (meaningless unless kindReadable)
    bool kindReadable = false;
    uint8_t window = 0;        // the frame window at the call: 0 open, 1 closed, 2 lapsed
    // The VR world route's injection mode (below): what the detour decided for this call BEFORE the observer hears of it.
    bool willInject = false;   // the call will be injected with a non-zero phase (the route's stage 2)
    uint8_t role = 255;        // FlatCameraVrRole of a kind-3 call in injection mode (0 scene, 1 first-person, 2 auxiliary); 255 otherwise
};
struct FlatCameraObserver {
    // The owner thread, before the game's body runs. True asks for post() after it (the return is redirected).
    bool (*pre)(const FlatCameraObserveCall& call) noexcept = nullptr;
    // The owner thread, after the body returned.
    void (*post)(uintptr_t camera, uintptr_t ctx) noexcept = nullptr;
    // ANY thread but the owner's (so it must be lock-free): the call is counted and passed through untouched.
    void (*offThread)(uintptr_t camera, uint64_t callerRva, uint32_t kind, bool kindReadable,
                      uint32_t thread) noexcept = nullptr;
};
// The observer the detour reports to; null detaches it. The pointed-to struct must outlive the process's last call.
void flatCameraInjectSetObserver(const FlatCameraObserver* observer);
// The per-frame step of observe mode: switches the detour to observe-only (before the first install, so its first
// call already is), installs the hook once, opens the frame window. False while there is no live hook (a failed or
// stood-down install): the census then reports what it did not see.
bool flatCameraInjectObserveFrame();
// "pending" (no install tried), "installed", "failed" (a refused install is final for the session) or "down".
const char* flatCameraInjectObserveStatus();

// ---------------------------------------------------------------------------
// THE VR WORLD ROUTE'S INJECTION MODE (design doc section 82, stage 2, 2026-09-30)
//
// In the VR profile the route (src/d3d11/vr_world_route.cpp) puts the world's sub-pixel phase into the game's own camera
// construction, the way the flat profile does through flatCameraInjectFrame, for the on-foot 2D screen's world passes
// only. The census (flight 1, 4.63 million refresh calls) settled what may be injected: the VR eye cameras are kind 5
// (refreshed once a frame per eye AFTER the tone, at the eye composite draw), the world is kind 3 (about 60 calls a frame
// BEFORE the tone: the scene camera and the first-person weapon camera, the SAME camera object with a tighter field of
// view and a larger near plane), and shadow and light cameras are kinds 0 and 1. The camera OBJECT is no identity (the
// left eye camera of the cockpit became the world camera on foot): the KIND is read on every call, as flatCameraAdmit
// does, and kinds 4 and 5 stay Unsupported, named and never mutated.
//
// What is injected: a kind-3 call whose camera is a SCREEN VIEW: its aspect field is within 4% of the route's panel
// aspect (renderW / renderH). Scene and first-person are both screen views and get the SAME phase (the bound pair is an
// NDC shift, FOV-independent, so the same bound is the same pixel shift). Every other kind-3 call is AUXILIARY (a probe,
// a spot light's square camera...): excluded, counted, its signature logged by the route; an unknown role is never
// guessed. Calls after the route's trigger (the tone) are not injected: the world passes all precede it.
//
// The route drives it ONCE A FRAME on the thread that runs Present (vrWorldRouteFrameBoundary, which runs before the
// census's boundary), and once more when the trigger draw is seen. It is the only writer of the injection switch, and it
// asks for injection only while experimental.temporal_aa_on_foot_world is auto, the route is Warming or Owned and the
// world jitter key is on: with the route key off nothing in this section is ever called with inject = true.
// ---------------------------------------------------------------------------
struct FlatCameraVrFrame {
    bool inject = false;      // inject kind-3 screen-view calls before the trigger, with the phase below
    bool observe = false;     // the VR camera census drives the same detour (its observer is registered): it hears every call
    float phaseX = 0.0f, phaseY = 0.0f;   // the frame's raster phase in RENDER pixels, positive right/down; (0,0) admits and writes nothing
    uint32_t renderW = 0, renderH = 0;    // the render size the phase is in (the world's H, 5040x2835)
    float screenAspect = 0.0f;            // renderW / renderH: the role test's anchor
};
enum class FlatCameraVrRole : uint8_t { Scene = 0, FirstPerson = 1, Auxiliary = 2 };
// What the frame's calls did (reset by flatCameraVrFrame, read by the route at the trigger and at the next boundary).
// Every call reaches exactly one of the first-level outcomes below.
struct FlatCameraVrCounters {
    uint32_t calls = 0;                 // refresh calls the detour saw this frame on the owner thread
    uint32_t sceneInjected = 0, sceneRefused = 0;             // Scene-role calls: phase written / not written for want of a write (a failed write, a refused redirect)
    uint32_t firstPersonInjected = 0, firstPersonRefused = 0; // First-person-role calls, likewise
    uint32_t warming = 0;               // screen-view calls admitted with a zero phase (nothing written)
    uint32_t auxiliary = 0;             // kind-3 calls excluded by role (not a screen view)
    uint32_t afterTrigger = 0;          // screen-view calls after the trigger (window closed): not injected
    uint32_t unsupported = 0;           // kinds 4 and 5
    uint32_t otherKind = 0;             // kinds 0, 1, 2 ...
    uint32_t unreadable = 0;            // the kind could not be read
    uint32_t stale = 0;                 // kind-3 calls with the frame window closed or lapsed
    uint32_t writeFailures = 0;         // mutation, flush or redirect writes that failed
    uint32_t offThread = 0;             // calls on a thread other than Present's (lock-free count, any thread)
    uint32_t injectedKind[8] = {};      // INJECTED calls by kind: 0..5, 6 = other, 7 = unreadable. Only index 3 may ever be non-zero
    // Appended by the injector (2026-09-30), after the declared fields so nothing that names them moves:
    uint32_t notActive = 0;             // the first-level outcome the list above lacks: kind-3 calls in a frame that is not injecting (observe-only or
                                        // pass-through). With it, calls == the sum of the first-level outcomes in every mode (flat_camera_vr.h, flatCameraVrOutcomeSum)
    uint32_t flushed = 0;               // cameras flushed: the one-time dirty-bit write on a camera injected earlier and now not (not an outcome: a
                                        // flush accompanies whichever outcome the call had)
    // Appended by the stage 2 experiment build (design doc section 82): the narrowest and the widest struct field of view (rad) among
    // the frame's screen views (Scene or First-person role, whose frustum was read: an injected, warming or after-trigger call), NaN
    // when none was read. Two different values say the struct carries a tighter weapon camera; one says the field-of-view test of the
    // role cannot tell the weapon from the scene (the route's inject line prints them as fov=narrowest..widest).
    float fovNarrowest = std::numeric_limits<float>::quiet_NaN();
    float fovWidest = std::numeric_limits<float>::quiet_NaN();
};
// One distinct excluded signature (the route logs the first eight, once each).
struct FlatCameraVrExcluded {
    float aspect = 0, fov = 0, nearZ = 0, farZ = 0;
    uint64_t callerRva = 0;
    uint64_t calls = 0;
};
// The route's frame step. Installs the hook when it is wanted (inject or observe) and absent, sets this frame's mode
// (observe-only unless inject), stores the phase and the role anchor, resets the frame's counters, re-opens the injection
// window and arms the frame window. With neither inject nor observe the detour is pass-through: the relay gate stays open
// only while a camera this session injected has not yet had its first un-injected call (the flush), then closes.
// Returns false while there is no live hook (not installed, failed, stood down): inject was not honoured.
bool flatCameraVrFrame(const FlatCameraVrFrame& frame);
// The route's trigger: calls after this one, this frame, are not injected (counted as afterTrigger).
void flatCameraVrCloseWindow();
// This frame's counters so far.
FlatCameraVrCounters flatCameraVrCounters();
// The distinct excluded signatures seen this session (up to `max`, at most 8 are kept), oldest first; returns how many were written.
size_t flatCameraVrExcluded(FlatCameraVrExcluded* out, size_t max);
// The same words flatCameraInjectObserveStatus gives: "pending", "installed", "failed", "down".
const char* flatCameraVrStatus();
// True when the detour is quiet for the route's purposes: this frame asked for neither injection nor observation, and no camera this session injected is still waiting for its first un-injected call (its flush). The route stops calling flatCameraVrFrame once its key is off and this is true. False while there is no live hook only if a flush could still be pending; a hook that never installed is quiet.
bool flatCameraVrQuiet();

} // namespace edvr
