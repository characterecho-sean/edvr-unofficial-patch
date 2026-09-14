#include "runtime_exports.h"

namespace {std::atomic<edvr::openxr::RuntimeLifecycle*> runtime{nullptr};}
bool edvr::openxr::bindRuntimeExports(RuntimeLifecycle& value) {
  RuntimeLifecycle* expected=nullptr;
  return runtime.compare_exchange_strong(expected,&value,std::memory_order_release,std::memory_order_relaxed);
}
extern "C" uint32_t __cdecl edvr_native_VR_InitInternal(vr::EVRInitError* error,vr::EVRApplicationType application) {
  auto* owner=runtime.load(std::memory_order_acquire);
  if(!owner){if(error)*error=vr::VRInitError_Init_NotInitialized;return 0;}
  return owner->init(error,application);
}
extern "C" void __cdecl edvr_native_VR_ShutdownInternal() {
  if(auto* owner=runtime.load(std::memory_order_acquire))owner->shutdown();
}
extern "C" void* __cdecl edvr_native_VR_GetGenericInterface(const char* version,vr::EVRInitError* error) {
  if(auto* owner=runtime.load(std::memory_order_acquire))return owner->getInterface(version,error);
  if(error)*error=vr::VRInitError_Init_NotInitialized;return nullptr;
}
extern "C" bool __cdecl edvr_native_VR_IsInterfaceVersionValid(const char* version) {
  return runtime.load(std::memory_order_acquire)&&edvr::openxr::RuntimeLifecycle::valid(version);
}
extern "C" uint32_t __cdecl edvr_native_VR_GetInitToken() {
  if(auto* owner=runtime.load(std::memory_order_acquire))return owner->token();return 0;
}
