#pragma once

// Pure conversion between OpenXR view FOVs and the historical OpenVR
// projection representations.  No runtime state or fallback geometry lives
// here; callers must provide a valid located view and clip planes.
#include <openxr/openxr.h>
#include "../openvr/compat/openvr_v0_9_20.h"

#include <cmath>
#include <limits>

namespace edvr::openxr {

struct RawFov {
  float left;
  float right;
  float top;
  float bottom;
};

inline bool finite(float value) { return std::isfinite(value) != 0; }

inline bool fovToRaw(const XrFovf& fov, RawFov& out) {
  // OpenXR angles are left, right, up, down.  A usable frustum has a positive
  // angular width/height and all rays remain in front of the eye.
  if (!finite(fov.angleLeft) || !finite(fov.angleRight) ||
      !finite(fov.angleUp) || !finite(fov.angleDown) ||
      fov.angleLeft >= fov.angleRight || fov.angleDown >= fov.angleUp ||
      fov.angleLeft <= -1.5707963267948966f ||
      fov.angleRight >= 1.5707963267948966f ||
      fov.angleDown <= -1.5707963267948966f ||
      fov.angleUp >= 1.5707963267948966f)
    return false;

  // Valve's raw API names are reversed vertically: top is the -Y ray,
  // bottom is the +Y ray (Driver_API_Documentation.md in ValveSoftware/openvr).
  RawFov candidate{std::tan(fov.angleLeft), std::tan(fov.angleRight),
                   std::tan(fov.angleDown), std::tan(fov.angleUp)};
  if (!finite(candidate.left) || !finite(candidate.right) ||
      !finite(candidate.top) || !finite(candidate.bottom) ||
      candidate.left >= candidate.right || candidate.top >= candidate.bottom)
    return false;
  out = candidate;
  return true;
}

inline bool projectionMatrix(const XrFovf& fov, float nearZ, float farZ,
                             vr::EGraphicsAPIConvention convention,
                             vr::HmdMatrix44_t& out) {
  RawFov raw{};
  if (!fovToRaw(fov, raw) || !finite(nearZ) || !finite(farZ) ||
      nearZ <= 0.0f || farZ <= nearZ)
    return false;
  if (convention != vr::API_DirectX && convention != vr::API_OpenGL)
    return false;

  const double width = static_cast<double>(raw.right) - raw.left;
  const double height = static_cast<double>(raw.bottom) - raw.top;
  const double n = nearZ;
  const double f = farZ;
  const double invDepth = 1.0 / (n - f);
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 ||
      height <= 0.0 || !std::isfinite(invDepth))
    return false;
  double values[4][4]{};
  values[0][0] = 2.0 / width;
  values[1][1] = 2.0 / height;
  values[0][2] = (double(raw.right) + raw.left) / width;
  values[1][2] = (double(raw.bottom) + raw.top) / height;
  values[3][2] = -1.0;
  if (convention == vr::API_DirectX) {
    values[2][2] = f * invDepth;
    values[2][3] = n * f * invDepth;
  } else {
    values[2][2] = (f + n) * invDepth;
    values[2][3] = 2.0 * n * f * invDepth;
  }
  vr::HmdMatrix44_t candidate{};
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col) {
      const double value = values[row][col];
      // Check representability before narrowing, including tiny FOV widths.
      if (!std::isfinite(value) || std::fabs(value) > (std::numeric_limits<float>::max)()) return false;
      candidate.m[row][col] = static_cast<float>(value);
    }
  out = candidate;
  return true;
}

} // namespace edvr::openxr
