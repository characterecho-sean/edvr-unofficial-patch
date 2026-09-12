#include "../../src/openvr/compat/openvr_v0_9_20.h"

// Separate translation unit, no link-time optimization: the test must use the
// historical virtual ABI, not calls devirtualized to the concrete class.
extern "C" __declspec(noinline) vr::IVRSystem* openxrAbiCaller(vr::IVRSystem* system) {
  return system;
}
