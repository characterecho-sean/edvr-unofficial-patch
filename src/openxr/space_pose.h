#pragma once

#include "geometry_snapshot.h"
#include <openxr/openxr.h>
#include <cstdint>
#include <cmath>
#include <limits>

namespace edvr::openxr {

struct TimedHeadPose {
  vr::TrackedDevicePose_t pose{};
  bool linearVelocityValid = false;
  bool angularVelocityValid = false;
  XrTime time = 0;
};

inline vr::TrackedDevicePose_t invalidHeadPose(bool connected) {
  vr::TrackedDevicePose_t p{};
  p.mDeviceToAbsoluteTracking.m[0][0] = 1.0f;
  p.mDeviceToAbsoluteTracking.m[1][1] = 1.0f;
  p.mDeviceToAbsoluteTracking.m[2][2] = 1.0f;
  p.bDeviceIsConnected = connected;
  p.bPoseIsValid = false;
  p.eTrackingResult = connected ? vr::TrackingResult_Running_OutOfRange
                                : vr::TrackingResult_Uninitialized;
  return p;
}

inline bool makeHeadPose(const XrPosef& pose, XrSpaceLocationFlags locationFlags,
                         XrSpaceVelocityFlags velocityFlags,
                         const XrVector3f& linear, const XrVector3f& angular,
                         bool connected, TimedHeadPose& out) {
  TimedHeadPose candidate{};
  candidate.pose = invalidHeadPose(connected);
  if (!connected) { out = candidate; return true; }

  constexpr XrSpaceLocationFlags required =
      XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if ((locationFlags & required) != required) { out = candidate; return true; }
  if (!detail::poseValid(pose)) return false;

  double r[3][3]; detail::rotation(pose.orientation, r);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) candidate.pose.mDeviceToAbsoluteTracking.m[i][j] = static_cast<float>(r[i][j]);
  candidate.pose.mDeviceToAbsoluteTracking.m[0][3] = pose.position.x;
  candidate.pose.mDeviceToAbsoluteTracking.m[1][3] = pose.position.y;
  candidate.pose.mDeviceToAbsoluteTracking.m[2][3] = pose.position.z;
  candidate.pose.bPoseIsValid = true;
  candidate.pose.eTrackingResult = vr::TrackingResult_Running_OK;

  constexpr XrSpaceVelocityFlags linearBit = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
  constexpr XrSpaceVelocityFlags angularBit = XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
  if ((velocityFlags & linearBit) != 0) {
    if (!finite(linear.x) || !finite(linear.y) || !finite(linear.z)) return false;
    candidate.pose.vVelocity.v[0] = linear.x; candidate.pose.vVelocity.v[1] = linear.y; candidate.pose.vVelocity.v[2] = linear.z; candidate.linearVelocityValid = true;
  }
  if ((velocityFlags & angularBit) != 0) {
    if (!finite(angular.x) || !finite(angular.y) || !finite(angular.z)) return false;
    candidate.pose.vAngularVelocity.v[0] = angular.x; candidate.pose.vAngularVelocity.v[1] = angular.y; candidate.pose.vAngularVelocity.v[2] = angular.z; candidate.angularVelocityValid = true;
  }
  out = candidate; return true;
}

inline XrResult locateHeadAt(PFN_xrLocateSpace fn, XrSpace view, XrSpace origin,
                             XrTime time, bool connected, TimedHeadPose& out) {
  if (!fn) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!view || !origin) return XR_ERROR_HANDLE_INVALID;
  XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &velocity};
  const XrResult result = fn(view, origin, time, &location);
  if (result != XR_SUCCESS) return result;
  if (location.type != XR_TYPE_SPACE_LOCATION || location.next != &velocity ||
      velocity.type != XR_TYPE_SPACE_VELOCITY || velocity.next != nullptr)
    return XR_ERROR_VALIDATION_FAILURE;
  TimedHeadPose candidate{};
  if (!makeHeadPose(location.pose, location.locationFlags, velocity.velocityFlags,
                    velocity.linearVelocity, velocity.angularVelocity, connected, candidate))
    return XR_ERROR_POSE_INVALID;
  candidate.time = time;
  out = candidate;
  return XR_SUCCESS;
}

inline bool nextPredictionTime(XrTime display, XrDuration period, XrTime& out) {
  if (period <= 0) return false;
  if (display > (std::numeric_limits<XrTime>::max)() - period) return false;
  out = display + period; return true;
}

} // namespace edvr::openxr
