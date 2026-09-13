#pragma once

#include "../../src/openvr/compat/openvr_v0_9_20.h"
#include <cmath>
#include <cstdint>

namespace edvr::openxr::module_test {

struct SystemQuerySnapshot {
  uint32_t width = 0, height = 0;
  vr::HmdMatrix34_t eyeToHead[2]{};
  vr::HmdMatrix44_t projection[2][3]{};
  bool geometryValid = false;
  bool poseQueried = false;
  bool poseValid = false;
  bool modelPropertyValid = false;
  bool trackingPropertyValid = false;
  bool frequencyPropertyValid = false;
  vr::ETrackedPropertyError modelPropertyError = vr::TrackedProp_UnknownProperty;
  vr::ETrackedPropertyError trackingPropertyError = vr::TrackedProp_UnknownProperty;
  vr::ETrackedPropertyError frequencyPropertyError = vr::TrackedProp_UnknownProperty;
};

inline bool finiteMatrix(const vr::HmdMatrix34_t& value) {
  for (unsigned row = 0; row != 3; ++row)
    for (unsigned column = 0; column != 4; ++column)
      if (!std::isfinite(value.m[row][column])) return false;
  return true;
}

inline bool rigidMatrix(const vr::HmdMatrix34_t& value) {
  if (!finiteMatrix(value)) return false;
  for (unsigned row = 0; row != 3; ++row) {
    double length = 0.0;
    for (unsigned column = 0; column != 3; ++column)
      length += double(value.m[row][column]) * value.m[row][column];
    if (std::fabs(length - 1.0) > 2.0e-3) return false;
  }
  for (unsigned first = 0; first != 3; ++first)
    for (unsigned second = first + 1; second != 3; ++second) {
      double dot = 0.0;
      for (unsigned column = 0; column != 3; ++column)
        dot += double(value.m[first][column]) * value.m[second][column];
      if (std::fabs(dot) > 2.0e-3) return false;
    }
  const double determinant =
      double(value.m[0][0]) * (double(value.m[1][1]) * value.m[2][2] - double(value.m[1][2]) * value.m[2][1]) -
      double(value.m[0][1]) * (double(value.m[1][0]) * value.m[2][2] - double(value.m[1][2]) * value.m[2][0]) +
      double(value.m[0][2]) * (double(value.m[1][0]) * value.m[2][1] - double(value.m[1][1]) * value.m[2][0]);
  return std::fabs(determinant - 1.0) <= 3.0e-3;
}

inline bool finiteProjection(const vr::HmdMatrix44_t& value,
                             float nearZ, float farZ) {
  for (unsigned row = 0; row != 4; ++row)
    for (unsigned column = 0; column != 4; ++column)
      if (!std::isfinite(value.m[row][column])) return false;
  if (!(nearZ > 0.0f) || !(farZ > nearZ) || value.m[0][0] <= 0.0f ||
      value.m[1][1] <= 0.0f || std::fabs(value.m[3][2] + 1.0f) > 1.0e-4f)
    return false;
  // Independently check the clip-plane mapping: DirectX maps near to 0, far to 1.
  for (unsigned row = 0; row != 4; ++row)
    for (unsigned column = 0; column != 4; ++column)
      if (row != column && column != 2 && !(row == 2 && column == 3) &&
          std::fabs(value.m[row][column]) > 1.0e-5f) return false;
  if (std::fabs(value.m[3][3]) > 1.0e-5f) return false;
  const double nearDepth = (-double(nearZ) * value.m[2][2] + value.m[2][3]) / nearZ;
  const double farDepth = (-double(farZ) * value.m[2][2] + value.m[2][3]) / farZ;
  return std::isfinite(nearDepth) && std::isfinite(farDepth) &&
         std::fabs(nearDepth) <= 1.0e-4 && std::fabs(farDepth - 1.0) <= 1.0e-4;
}

inline bool validHeadPose(const vr::TrackedDevicePose_t& pose) {
  if (!pose.bPoseIsValid || !pose.bDeviceIsConnected ||
      (pose.eTrackingResult != vr::TrackingResult_Running_OK &&
       pose.eTrackingResult != vr::TrackingResult_Running_OutOfRange) ||
      !rigidMatrix(pose.mDeviceToAbsoluteTracking)) return false;
  for (unsigned i = 0; i != 3; ++i)
    if (!std::isfinite(pose.vVelocity.v[i]) || !std::isfinite(pose.vAngularVelocity.v[i])) return false;
  return true;
}

inline bool collectSystemQueries(vr::IVRSystem& system,
                                 SystemQuerySnapshot& out,
                                 bool requireNativeProperties = true) {
  out = {};
  system.GetRecommendedRenderTargetSize(&out.width, &out.height);
  if (!out.width || !out.height) return false;
  constexpr float nearZ = .025f;
  constexpr float farZ = 50000.f;
  for (unsigned eye = 0; eye != 2; ++eye) {
    out.eyeToHead[eye] = system.GetEyeToHeadTransform(static_cast<vr::EVREye>(eye));
    if (!rigidMatrix(out.eyeToHead[eye])) return false;
    for (unsigned pair = 0; pair != 3; ++pair) {
      // The three observed startup pairs are retained explicitly so every
      // projection path is exercised before application WaitGetPoses.
      const float pairNear = pair == 0 ? nearZ : pair == 1 ? .1f : 1.f;
      const float pairFar = pair == 0 ? farZ : pair == 1 ? 1000.f : farZ;
      out.projection[eye][pair] = system.GetProjectionMatrix(
          static_cast<vr::EVREye>(eye), pairNear, pairFar, vr::API_DirectX);
      if (!finiteProjection(out.projection[eye][pair], pairNear, pairFar)) return false;
    }
  }
  vr::TrackedDevicePose_t pose{};
  system.GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, 0.f, &pose, 1);
  out.poseQueried = true;
  out.poseValid = validHeadPose(pose);

  char model[128]{}, tracking[128]{};
  out.modelPropertyError = vr::TrackedProp_UnknownProperty;
  out.trackingPropertyError = vr::TrackedProp_UnknownProperty;
  const uint32_t modelLength = system.GetStringTrackedDeviceProperty(
      vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_ModelNumber_String, model,
      sizeof(model), &out.modelPropertyError);
  const uint32_t trackingLength = system.GetStringTrackedDeviceProperty(
      vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String,
      tracking, sizeof(tracking), &out.trackingPropertyError);
  out.modelPropertyValid = out.modelPropertyError == vr::TrackedProp_Success &&
                           modelLength > 1 && modelLength <= sizeof(model) &&
                           model[0] != '\0' && model[modelLength - 1] == '\0';
  out.trackingPropertyValid = out.trackingPropertyError == vr::TrackedProp_Success &&
                              trackingLength > 1 && trackingLength <= sizeof(tracking) &&
                              tracking[0] != '\0' && tracking[trackingLength - 1] == '\0';
  float frequency = 0.f;
  out.frequencyPropertyError = vr::TrackedProp_UnknownProperty;
  frequency = system.GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                   vr::Prop_DisplayFrequency_Float,
                                                   &out.frequencyPropertyError);
  out.frequencyPropertyValid = out.frequencyPropertyError == vr::TrackedProp_Success &&
                               std::isfinite(frequency) && frequency > 0.f;
  out.geometryValid = true;
  return !requireNativeProperties ||
         (out.modelPropertyValid && out.trackingPropertyValid &&
          (out.frequencyPropertyValid || out.frequencyPropertyError == vr::TrackedProp_ValueNotProvidedByDevice));
}

struct PeriodicSystemSample {
  bool geometryValid = false;
  bool poseValid = false;
  bool eventQueried = false;
};

inline PeriodicSystemSample collectPeriodicSystemSample(vr::IVRSystem& system) {
  PeriodicSystemSample out{};
  uint32_t width = 0, height = 0;
  system.GetRecommendedRenderTargetSize(&width, &height);
  float left = 0.f, right = 0.f, top = 0.f, bottom = 0.f;
  bool rawValid = true;
  for (unsigned eye = 0; eye != 2; ++eye) {
    left = right = top = bottom = 0.f;
    system.GetProjectionRaw(static_cast<vr::EVREye>(eye), &left, &right, &top, &bottom);
    rawValid = rawValid && std::isfinite(left) && std::isfinite(right) &&
               std::isfinite(top) && std::isfinite(bottom) && left < right && top < bottom;
  }
  out.geometryValid = width != 0 && height != 0 && rawValid;
  vr::TrackedDevicePose_t pose{};
  system.GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, 0.f, &pose, 1);
  out.poseValid = validHeadPose(pose);
  vr::VREvent_t event{};
  vr::TrackedDevicePose_t eventPose{};
  system.PollNextEventWithPose(vr::TrackingUniverseSeated, &event, sizeof(event), &eventPose);
  out.eventQueried = true;
  return out;
}

}  // namespace edvr::openxr::module_test
