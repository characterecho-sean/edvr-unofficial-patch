#pragma once

// The eye-to-head matrix in the handedness Elite composes it in
// (docs/canted-projection.md). Pure math and no runtime state, so a rig can
// drive every case.
//
//   gameHandedness     S*E*S of an eye-to-head matrix, S = diag(1,1,-1): the eye
//                      matrix in the handedness Elite composes it in. It is the
//                      identity, to the bit, on parallel panels, which have no
//                      per-eye rotation; on a canted headset it is what keeps
//                      each eye's yaw and pitch from arriving inverted.
//   forwardYawDegrees  the signed yaw of an eye's forward axis in head space
//                      (for the rig: it shows the sign the correction flips).
#include "../openvr/compat/openvr_v0_9_20.h"
#include <cmath>

namespace edvr::openxr {

// S*E*S, S = diag(1,1,-1): the 3x4's m[0][2], m[1][2], m[2][0], m[2][1] and
// m[2][3] change sign, everything else stays. 0 - x rather than -x so a zero
// stays +0.
inline vr::HmdMatrix34_t gameHandedness(const vr::HmdMatrix34_t& eyeToHead) {
  vr::HmdMatrix34_t out = eyeToHead;
  out.m[0][2] = 0.0f - eyeToHead.m[0][2];
  out.m[1][2] = 0.0f - eyeToHead.m[1][2];
  out.m[2][0] = 0.0f - eyeToHead.m[2][0];
  out.m[2][1] = 0.0f - eyeToHead.m[2][1];
  out.m[2][3] = 0.0f - eyeToHead.m[2][3];
  return out;
}

// The signed yaw, degrees, of an eye's forward axis from the head's forward:
// the axis (0,0,-1) carried through the matrix's rotation, measured about +y,
// positive toward +x. A left eye canted outward reads negative.
inline float forwardYawDegrees(const vr::HmdMatrix34_t& eyeToHead) {
  const double x = -double(eyeToHead.m[0][2]), z = -double(eyeToHead.m[2][2]);
  return float(std::atan2(x, -z)*(180.0/3.14159265358979323846));
}

}  // namespace edvr::openxr
