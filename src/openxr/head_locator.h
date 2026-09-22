#pragma once
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#include <windows.h>
#include <openxr/openxr_platform.h>
#include "geometry_snapshot.h"
#include <cmath>
#include <limits>

namespace edvr::openxr {
struct LocatorDispatch {
  PFN_xrConvertWin32PerformanceCounterToTimeKHR convert=nullptr;
  PFN_xrLocateSpace locate=nullptr;
};
using CounterNow=bool (*)(LARGE_INTEGER*);
// Which underlying call a failed locate() came from, for diagnostics only
// (docs/linux-native-openxr-launch-centre-2026-09-22.md): callers that don't
// care leave the out-param null and pay nothing for it.
enum class HeadLocatorStage { None, Convert, Locate };
inline const char* headLocatorStageName(HeadLocatorStage s) {
  switch(s) {
    case HeadLocatorStage::Convert: return "convert";
    case HeadLocatorStage::Locate: return "locate";
    default: return "none";
  }
}
// Borrowed handles; caller excludes concurrent destruction/origin replacement.
// No frame call, cached display prediction or extrapolation.
class HeadLocator {
 public:
  // fallbackNow, when nonzero, is a caller-supplied XrTime instant from a
  // source that never calls xrConvertWin32PerformanceCounterToTimeKHR (the
  // frame loop's own calibrated estimate) -- used only if that runtime call
  // itself fails. Some Linux/Proton OpenXR stacks (WiVRn observed) never
  // implement it (docs/linux-native-openxr-launch-centre-2026-09-22.md);
  // on a runtime where it works, this changes nothing.
  XrResult locate(const LocatorDispatch& d,XrInstance instance,XrSpace view,XrSpace origin,
                  float prediction,XrSpaceLocation& out,XrTime* exact=nullptr,CounterNow now=nullptr,
                  HeadLocatorStage* stage=nullptr,XrTime fallbackNow=0) const {
    if(stage)*stage=HeadLocatorStage::None;
    if(!d.convert||!d.locate)return XR_ERROR_FUNCTION_UNSUPPORTED;
    if(!instance||!view||!origin)return XR_ERROR_HANDLE_INVALID;
    if(!std::isfinite(prediction))return XR_ERROR_TIME_INVALID;
    const double delta=std::round(double(prediction)*1e9),bound=std::ldexp(1.0,63);
    if(delta>=bound||delta< -bound)return XR_ERROR_TIME_INVALID;
    const XrTime offset=static_cast<XrTime>(delta);
    LARGE_INTEGER counter{};
    XrTime time=0;
    const XrResult convertResult=(!now||!now(&counter))?XR_ERROR_RUNTIME_FAILURE:d.convert(instance,&counter,&time);
    if(convertResult!=XR_SUCCESS) {
      if(stage)*stage=HeadLocatorStage::Convert;
      if(!fallbackNow)return convertResult;
      time=fallbackNow;
    }
    if((offset>0&&time>(std::numeric_limits<XrTime>::max)()-offset)||
       (offset<0&&time<(std::numeric_limits<XrTime>::min)()-offset))return XR_ERROR_TIME_INVALID;
    const XrTime target=time+offset;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    const XrResult r=d.locate(view,origin,target,&location);
    if(r!=XR_SUCCESS){if(stage)*stage=HeadLocatorStage::Locate;return r;}
    if(location.type!=XR_TYPE_SPACE_LOCATION||location.next)return XR_ERROR_VALIDATION_FAILURE;
    constexpr auto valid=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if((location.locationFlags&valid)==valid) {
      if(!detail::poseValid(location.pose))return XR_ERROR_POSE_INVALID;
    } else {
      // Partial tracking cannot produce a valid OpenVR six-degree pose.
      // Never read unspecified position/orientation fields in that case.
      location.locationFlags&=~valid;location.pose={};location.pose.orientation.w=1;
    }
    out=location;if(exact)*exact=target;return XR_SUCCESS;
  }
};
}
