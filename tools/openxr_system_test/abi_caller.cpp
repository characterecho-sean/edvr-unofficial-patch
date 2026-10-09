#include "../../src/openvr/compat/openvr_v0_9_20.h"

// Separate translation unit, no link-time optimization: the test must use the
// historical virtual ABI, not calls devirtualized to the concrete class.
extern "C" __declspec(noinline) vr::IVRSystem* openxrAbiCaller(vr::IVRSystem* system) {
  return system;
}

// The game's one direct pose call (RVA 0x4E3881 in build 332841): a real call site for the pose-time filter, whose return address is inside this executable.
extern "C" __declspec(noinline) int openxrAbiCallPose(vr::IVRSystem* system, vr::ETrackingUniverseOrigin origin, float prediction, vr::TrackedDevicePose_t* poses, uint32_t count) {
  system->GetDeviceToAbsoluteTrackingPose(origin, prediction, poses, count);
  return poses && poses[0].bPoseIsValid;
}
