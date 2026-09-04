// Temporal anti-aliasing at the door -- the openvr half of
// docs/anti-aliasing.md Feature B, v1.
//
// This half owns three things the pass in the d3d11 half cannot: the
// JITTER (a sub-pixel shift of the projection the game renders through,
// advanced every frame at the boundary and told to the game through the
// system hook's tangent edit, exactly as the cull guard's lie is), the
// HEAD's motion between frames (the runtime's pose at WaitGetPoses, the
// v1 reprojection source), and the DECISION -- which forwarded frames are
// treated, which are not (the theater's rendering, the withhold's shadow,
// which also resets the history), and when to stand down.
//
// Order at the door: this pass FIRST, on the game's texture at render size
// (wide under the cull guard), then the guard's crop, then the supersample
// resolve. experimental.temporal_aa = off | on, experimental.temporal_aa_jitter = on | off,
// experimental.temporal_aa_blend, experimental.temporal_aa_clamp -- all live;
// advanced.temporal_aa_motion = head | camera | none for the field's A/B.
#pragma once

#include <cstdint>

#include "openvr_min.h"

namespace edvr {

// Reads the keys. Called at the compositor hook's install and from the
// reload poll.
void temporalAaConfigure();

// The mode is on and the pass has not stood down.
bool temporalAaWanted();

// The runtime's head pose for the frame about to render, from
// WaitGetPoses, before any EDVR offset touches it (the offset moves the
// viewpoint, not its orientation). Kept as the pair this frame / last
// frame the reprojection needs.
void temporalAaNotePose(const vr::HmdMatrix34_t& pose, bool valid);
// The same frame's HMD pose from WaitGetPoses's game-pose array (predicted
// a frame further than the render pose), for the registration instrument
// only: a renderer that draws with the game poses registers with their
// delta, not the render poses'. Invalid when the game asked for none.
void temporalAaNoteGamePose(const vr::HmdMatrix34_t& pose, bool valid);

// The frame boundary, after the system hook's: advances the jitter and
// tells the system hook the tangent shift for the coming frame.
void temporalAaFrameBoundary();

// The treatment. Called with the game's own texture and bounds on the
// forward path; returns the treated texture (full-span bounds written to
// *outBounds, the orientation kept) or null, meaning forward what was
// passed in.
void* temporalAaTreat(vr::EVREye eye, void* handle,
                      const vr::VRTextureBounds_t* bounds,
                      vr::VRTextureBounds_t* outBounds);

// A frame for this eye was withheld (the shadow copy or nothing went
// out): continuity is broken, and the next treated frame starts the
// history afresh.
void temporalAaNoteWithheld(vr::EVREye eye);

void temporalAaStandDown(const char* why);

void temporalAaShutdown();

}  // namespace edvr
