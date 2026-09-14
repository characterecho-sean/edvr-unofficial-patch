#pragma once

#include "geometry_snapshot.h"
#include <openxr/openxr.h>
#include <cmath>

namespace edvr::openxr {

// The seated reset pose is the current HMD translation and its heading.  Its
// orientation has no pitch or roll, so the seated world remains +Y-up.
inline bool seatedOriginFromHead(const XrPosef& head, XrSpaceLocationFlags flags,
                                 XrPosef& out) {
  constexpr XrSpaceLocationFlags required =
      XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if ((flags & required) != required || !detail::poseValid(head)) return false;
  double r[3][3]; detail::rotation(head.orientation, r);
  // The HMD's forward direction is local -Z.  Project it onto the XZ plane.
  const double fx = -r[0][2], fz = -r[2][2];
  if (fx * fx + fz * fz <= 1.0e-12) return false;
  const double yaw = std::atan2(-fx, -fz);
  XrPosef candidate{};
  candidate.position = head.position;
  candidate.orientation.x = 0.0f;
  candidate.orientation.y = static_cast<float>(std::sin(yaw * 0.5));
  candidate.orientation.z = 0.0f;
  candidate.orientation.w = static_cast<float>(std::cos(yaw * 0.5));
  out = candidate;
  return true;
}

} // namespace edvr::openxr
