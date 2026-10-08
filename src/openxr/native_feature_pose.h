#pragma once
#include "../openvr/compat/openvr_v0_9_20.h"

namespace edvr::openxr {
inline vr::VRTextureBounds_t nativeCropBounds(const vr::VRTextureBounds_t* input,
    float left,float top,float right,float bottom) {
  const vr::VRTextureBounds_t b=input?*input:vr::VRTextureBounds_t{0,0,1,1};
  return {b.uMin+(b.uMax-b.uMin)*left,b.vMin+(b.vMax-b.vMin)*top,
          b.uMin+(b.uMax-b.uMin)*right,b.vMin+(b.vMax-b.vMin)*bottom};
}
}
