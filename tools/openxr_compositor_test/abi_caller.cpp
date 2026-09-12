#include "../../src/openvr/compat/openvr_v0_9_20.h"
// Compiled separately without LTCG so calls cannot be devirtualized from a
// locally known concrete object. All behavioral tests use this base pointer.
extern "C" __declspec(noinline) vr::IVRCompositor* compositorAbiCaller(vr::IVRCompositor* object) {return object;}
