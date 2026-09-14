#pragma once
#include "../openvr/compat/openvr_v0_9_20.h"
#include <cmath>

namespace edvr::openxr {
// Matches the legacy Explorer Cam transform: yaw changes orientation, while
// translation is an offset in tracking coordinates, not a rotation about zero.
inline void applyNativeHeadOffset(vr::TrackedDevicePose_t& pose, const float (&offset)[3], float yaw) {
  if (!pose.bPoseIsValid) return;
  auto& m=pose.mDeviceToAbsoluteTracking.m;
  const float c=std::cos(yaw),s=std::sin(yaw);
  for(unsigned column=0;column<3;++column) {
    const float x=m[0][column],z=m[2][column];
    m[0][column]=c*x+s*z;m[2][column]=-s*x+c*z;
  }
  for(unsigned axis=0;axis<3;++axis)m[axis][3]+=offset[axis];
}
inline vr::VRTextureBounds_t nativeCropBounds(const vr::VRTextureBounds_t* input,
    float left,float top,float right,float bottom) {
  const vr::VRTextureBounds_t b=input?*input:vr::VRTextureBounds_t{0,0,1,1};
  return {b.uMin+(b.uMax-b.uMin)*left,b.vMin+(b.vMax-b.vMin)*top,
          b.uMin+(b.uMax-b.uMin)*right,b.vMin+(b.vMax-b.vMin)*bottom};
}
}
