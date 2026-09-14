#pragma once
#include "runtime_lifecycle.h"

// Internal prototype entry points. Only the five game-imported signatures are
// bound here; the complete legacy export manifest is a separate release gate.
extern "C" {
uint32_t __cdecl edvr_native_VR_InitInternal(vr::EVRInitError*,vr::EVRApplicationType);
void __cdecl edvr_native_VR_ShutdownInternal();
void* __cdecl edvr_native_VR_GetGenericInterface(const char*,vr::EVRInitError*);
bool __cdecl edvr_native_VR_IsInterfaceVersionValid(const char*);
uint32_t __cdecl edvr_native_VR_GetInitToken();
}
namespace edvr::openxr {
// Bind once before any exported call, outside DllMain. The coordinator and
// backing objects remain alive until all callers finish and the module exits.
bool bindRuntimeExports(RuntimeLifecycle&);
}
