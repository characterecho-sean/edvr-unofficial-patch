// The VR camera census (design doc section 82, "Pre-build findings and the stop"): a ZERO-MUTATION observation of the
// game's view-constant refresh (the camera injector's detour) in the VR profile, to answer the one question the world
// route's jitter waits on: how to tell the eye views' cameras from the world's. Key: advanced.vr_camera_census = off|on,
// default off. With it on in the VR profile the census registers an observer with the detour and the CENSUS never writes a
// camera, a bound pair, a flag or a row (when the world route injects its phase it is the DETOUR that writes the world's
// cameras, for the route; the census hears of each call and of what the detour decided for it); with it off, nothing is
// installed and nothing changes: every entry point below returns at once, nothing is allocated and no line is logged
// (tools\vr_camera_census_test pins each).
//
// What it records, bounded (vr_camera_census_core.h has the tables, the budget and the text of every line):
//   - a 5 s line every window (zeros included): calls, calls on other threads, kinds, callers, distinct cameras, where in
//     the frame the calls fell against the tone draw;
//   - per distinct camera (the first 64): its kind, caller, view and field signature, and any later change of it;
//   - the FULL call sequence of the first three on-foot frames, one line a call, with the rows the composer produced, what
//     the detour decided for the call (inj, role) and, in the sequence's header, the phase the route chose for the frame;
//   - at the eye composite draw of the first four on-foot frames (eight draws): the eye's b1 rows 270..273 read back
//     from the GPU, what EDVR advertised for that eye, the leak measure and the frame's phase.
// An on-foot frame is one in which the world route's detector saw the tone AND Elite's journal, when it is read, says on
// foot (vrCensusSamplesFrame in the core): the detector draws the same tone in a cockpit, a hangar and a menu, and those
// frames must not spend the samples. When the route reports no draw progress at all (vrWorldRouteDrawProgress false) there
// is no tone and the journal alone decides. While the route IS jittering the world (vrWorldRouteWorldPhase true), a frame is
// also sampled only when its phase is non-zero: the route's warm-up frames carry none, and only a frame that does can show a
// leak into the eyes. Flight 1 spent its whole sample on warm-up frames. With the route not jittering the rule is unchanged.
//
// EPISODES (design-world-camera-motion-2026-09-30.md section 6, Phase 0): the first three on-foot frames are all of that, and never an aboard frame, so
// the census could see neither a map nor the cockpit. An episode is ONE frame sampled 30 frames after a trigger (the journal's on-foot reading flips,
// the naming of the 2D screen's source flips and holds three frames, Status.json's GuiFocus changes, or the census turns on), whatever the frame is, at
// most ten a session: every refresh call of it, aboard ones included, tallied by kind and caller; at its first draw into each kind of depth the screen's
// size or an eye's, the vertex and pixel shader, whether the draw writes depth, the b1 size and the b1 rows 270..273 read back and matched to the calls;
// and the temporal pass's chosen rows matched to the calls' view axes. The 5 s line is unchanged; three companion lines follow it in each window: the
// episodes' counters (taken, triggers, skipped and state, zeros included), the on-foot naming runs and the observer halves' own CPU. Every part runs with
// the key on only: with it off nothing is installed, allocated, timed or logged, and the per-draw hook below is never set.
// `python tools\edvr_log.py --camera-census` reads the log back, does the join and judges the injection (the stage 2 verdict), and reports each episode.
#pragma once
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// The episodes' join hook (see above): the world route's per-draw hook (vr_world_route.cpp vrWorldRouteDraw, which runs for every game draw on the owner
// context while the census is wanted) calls through this pointer, which is null always except while an episode's sampled frame is running. So every other
// draw, and every draw with the key off, costs one load of a null pointer, and no rig that links the route needs a definition (a C++17 inline variable).
namespace detail {
using VrCensusJoinDrawFn = void (*)(ID3D11DeviceContext* ctx, uint32_t drawOrdinal);
inline VrCensusJoinDrawFn g_vrCensusJoinDraw = nullptr;
}  // namespace detail

// The key is on in the VR profile (read at the last boundary). Render thread. The world route's detector watches draws while
// this is true, so the census can say where in the frame each refresh call falls.
bool vrCameraCensusWanted();
// Once a frame at the Present boundary, AFTER vrWorldRouteFrameBoundary(): runs the injector's per-frame protocol in observe
// mode (the Present edge, the hook, the window), rolls the per-frame call sequence, prints the 5 s line and the bounded
// per-camera lines, and latches the world route's phase for the frame that starts (the route's boundary ran first, so
// vrWorldRouteWorldPhase() is that frame's).
void vrCameraCensusFrameBoundary();
// At each eye composite draw (2 a frame), after the game's own draw: logs, for the first few on-foot frames, the eye's VS b1
// rows 270..273 and the frustum and shift EDVR advertised for that eye this sequence. Render thread.
void vrCameraCensusEyeDraw(ID3D11DeviceContext* ctx, uint32_t eye);

// What EDVR advertised for `eye` this sequence (native_temporal.cpp, beside nativeTemporalDrawJitter): the frustum
// {left, right, down, up} tangents the host was given and the tangent shift the eye jitter moved it by. The render thread,
// outside treat(); false when there is no frame to describe.
bool nativeTemporalEyeGeometry(uint32_t eye, uint64_t* sequence, float frustum[4], float shift[2]);

}  // namespace edvr
