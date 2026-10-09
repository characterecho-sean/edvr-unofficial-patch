#include "../../src/openvr/compat/openvr_v0_9_20.h"

// Separate translation unit, no link-time optimization: the test must use the
// historical virtual ABI, not calls devirtualized to the concrete class.
extern "C" __declspec(noinline) vr::IVRSystem* openxrAbiCaller(vr::IVRSystem* system) {
  return system;
}

// The census records the game's own return address as frame 1, so the calls the
// test makes to learn it must come from a real call site here: noinline, and
// with work after the call so the compiler cannot turn it into a tail jump.
extern "C" __declspec(noinline) int openxrAbiCallRaw(vr::IVRSystem* system, vr::EVREye eye, float* out) {
  system->GetProjectionRaw(eye, out, out + 1, out + 2, out + 3);
  return out[0] != 0.0f;
}
extern "C" __declspec(noinline) int openxrAbiCallMatrix(vr::IVRSystem* system, vr::EVREye eye, vr::HmdMatrix44_t* out) {
  *out = system->GetProjectionMatrix(eye, 0.1f, 1000.0f, vr::API_DirectX);
  return out->m[0][0] != 0.0f;
}
extern "C" __declspec(noinline) int openxrAbiCallEyeToHead(vr::IVRSystem* system, vr::EVREye eye, vr::HmdMatrix34_t* out) {
  *out = system->GetEyeToHeadTransform(eye);
  return out->m[0][3] != 0.0f;
}
// One layer above openxrAbiCallRaw, so the first-sight stack capture has a known
// function for frame 2 and the test's own caller for frame 3.
extern "C" __declspec(noinline) int openxrAbiOuterRaw(vr::IVRSystem* system, vr::EVREye eye, float* out) {
  const int answered = openxrAbiCallRaw(system, eye, out);
  return answered + 1;
}