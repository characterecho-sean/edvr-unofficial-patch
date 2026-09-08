// The settings menu at the door -- the openvr half (docs/settings-menu.md).
//
// The d3d11 half owns the menu: its rows, its keys, its bitmap and the
// composite. This half owns the one thing only it can know -- which frame
// is leaving, from which pose -- and calls the composite LAST on every
// forwarding path of hookedSubmit, after the temporal pass, the crop, the
// resolve and the sharpen, on whatever texture they produced. The game is
// fed the LIVE pose throughout, so the compositor's reprojection is right
// for the panel and the frame alike; the panel's world lock is the anchor
// the d3d11 half published at summon, turned into each eye's transform
// here exactly as the FSS theater's is, with the eye's real offset from
// GetEyeToHeadTransform rather than the theater's constant.
//
// Nothing here reads config: the menu's every knob is the d3d11 half's.
#pragma once

#include "openvr_min.h"

namespace edvr {

// Is a panel up (the d3d11 half published a visibility above zero within
// the last few of its frames)? The per-submit short-circuit.
bool menuDoorWanted();

// The treatment, the sharpen's contract: the texture and bounds a path is
// about to forward, the head pose the frame was rendered from; returns the
// composited texture (full-span bounds written to *outBounds, direction
// kept) or null, meaning forward exactly what was passed in.
void* menuDoorTreat(vr::EVREye eye, void* handle, const vr::VRTextureBounds_t* bounds,
                    vr::VRTextureBounds_t* outBounds, const vr::HmdMatrix34_t& renderPose,
                    bool poseValid);

// The eye transform, pure, for the test: anchor and current poses (3x4,
// row-major), this eye's offset from the head in head space, out the 12
// floats the composite wants (D = Ra^T Rc as rows, then the eye's origin in
// anchor space).
void menuDoorXform(const float anchor[12], const float current[12], const float eyeOffset[3],
                   float xf[12]);

// The head-locked readout's anchor, pure, for the test: this frame's pose
// turned by the overlay's offsets. POSITIVE YAW PUTS IT TO THE RIGHT and
// positive pitch up, which is what edvr.ini promises; the negation that
// makes the first of those true lives in the implementation.
void menuHeadLockAnchor(const float current[12], float yawDeg, float pitchDeg, float out[12]);

void menuDoorShutdown();

}  // namespace edvr
