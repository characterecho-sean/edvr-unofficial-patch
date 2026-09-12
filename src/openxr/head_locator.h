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
// Borrowed handles; caller excludes concurrent destruction/origin replacement.
// No frame call, cached display prediction or extrapolation.
class HeadLocator {
 public:
  XrResult locate(const LocatorDispatch& d,XrInstance instance,XrSpace view,XrSpace origin,
                  float prediction,XrSpaceLocation& out,XrTime* exact=nullptr,CounterNow now=nullptr) const {
    if(!d.convert||!d.locate)return XR_ERROR_FUNCTION_UNSUPPORTED;
    if(!instance||!view||!origin)return XR_ERROR_HANDLE_INVALID;
    if(!std::isfinite(prediction))return XR_ERROR_TIME_INVALID;
    const double delta=std::round(double(prediction)*1e9),bound=std::ldexp(1.0,63);
    if(delta>=bound||delta< -bound)return XR_ERROR_TIME_INVALID;
    const XrTime offset=static_cast<XrTime>(delta);
    LARGE_INTEGER counter{};
    if(!now||!now(&counter))return XR_ERROR_RUNTIME_FAILURE;
    XrTime time=0;XrResult r=d.convert(instance,&counter,&time);
    if(r!=XR_SUCCESS)return r;
    if((offset>0&&time>(std::numeric_limits<XrTime>::max)()-offset)||
       (offset<0&&time<(std::numeric_limits<XrTime>::min)()-offset))return XR_ERROR_TIME_INVALID;
    const XrTime target=time+offset;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    r=d.locate(view,origin,target,&location);
    if(r!=XR_SUCCESS)return r;
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
