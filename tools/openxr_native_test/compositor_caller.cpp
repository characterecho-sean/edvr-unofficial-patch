#include "../../src/openvr/compat/openvr_v0_9_20.h"
// Separate translation unit and no LTCG: exercise historical virtual calls in
// the real runtime diagnostic as well as in the desktop member-ABI fixture.
extern "C" __declspec(noinline) vr::IVRCompositor* nativeCompositorCaller(vr::IVRCompositor* object) {
  return object;
}
extern "C" __declspec(noinline) vr::IVRSystem* nativeSystemCaller(vr::IVRSystem* object) {
  return object;
}
