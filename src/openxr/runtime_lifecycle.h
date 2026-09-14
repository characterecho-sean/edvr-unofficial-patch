#pragma once
#include "../openvr/compat/openvr_v0_9_20.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <limits>
#include <cstring>

namespace edvr::openxr {
struct RuntimeInterfaces {
  vr::IVRSystem* system=nullptr;
  vr::IVRCompositor* compositor=nullptr;
  vr::IVRChaperone* chaperone=nullptr;
  vr::IVRExtendedDisplay* display=nullptr;
  bool complete()const{return system&&compositor&&chaperone&&display;}
};
// Resource owner, independent of exported names. start must publish coherent
// startup geometry before returning success. It observes cancellation between
// bounded startup operations. stop joins/excludes all handle users and cleans
// partial startup too; false permanently blocks restart. Neither is DllMain work.
// Interface objects and this backend outlive the coordinator and every caller;
// callers must stop using returned pointers at Shutdown, per the pinned SDK.
class RuntimeBackend {
 public:
  virtual ~RuntimeBackend()=default;
  virtual vr::EVRInitError start(uint32_t token,const std::atomic<bool>& cancelled,RuntimeInterfaces& out)=0;
  virtual bool stop()noexcept=0;
};
class RuntimeLifecycle {
 public:
  explicit RuntimeLifecycle(RuntimeBackend& backend,uint32_t initialToken=0):backend_(backend),token_(initialToken){}
  RuntimeLifecycle(const RuntimeLifecycle&)=delete;
  RuntimeLifecycle& operator=(const RuntimeLifecycle&)=delete;
  // No destructor calls: the external owner must finish shutdown and join any
  // initializer before destroying this coordinator or unloading its module.
  uint32_t init(vr::EVRInitError* error,vr::EVRApplicationType application) {
    uint32_t attempt=0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if(application!=vr::VRApplication_Scene)return fail(error,vr::VRInitError_Init_NotSupportedWithCompositor);
      if(state_==State::Running){set(error,vr::VRInitError_None);return token_;}
      if(state_==State::Starting)return fail(error,vr::VRInitError_Init_Retry);
      if(state_==State::Stopping)return fail(error,vr::VRInitError_Init_ShuttingDown);
      if(state_==State::Failed)return fail(error,vr::VRInitError_Init_Internal);
      // Reserve the following token for invalidation on shutdown; never wrap.
      if(token_>(std::numeric_limits<uint32_t>::max)()-2)return fail(error,vr::VRInitError_Init_TooManyObjects);
      attempt=++token_;cancelled_.store(false,std::memory_order_release);state_=State::Starting;
    }
    RuntimeInterfaces candidate{};vr::EVRInitError result=vr::VRInitError_Init_Internal;
    try{result=backend_.start(attempt,cancelled_,candidate);}catch(...){result=vr::VRInitError_Init_Internal;}
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if(state_==State::Starting&&result==vr::VRInitError_None&&candidate.complete()) {
        interfaces_=candidate;state_=State::Running;set(error,vr::VRInitError_None);return token_;
      }
      if(cancelled_.load(std::memory_order_acquire))result=vr::VRInitError_Init_ShuttingDown;
      else if(result==vr::VRInitError_None)result=vr::VRInitError_Init_InterfaceNotFound;
      // The initializer alone owns cleanup when startup fails or is cancelled.
      state_=State::Stopping;interfaces_={};
    }
    const bool stopped=backend_.stop();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state_=stopped?State::Idle:State::Failed;
    }
    return fail(error,result);
  }
  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if(state_==State::Idle||state_==State::Failed||state_==State::Stopping)return;
      const bool starting=state_==State::Starting;
      state_=State::Stopping;interfaces_={};++token_;cancelled_.store(true,std::memory_order_release);
      // Never race destruction against an in-flight start. Its owner cleans up
      // on return; new init calls remain rejected until that cleanup finishes.
      if(starting)return;
    }
    const bool stopped=backend_.stop();
    std::lock_guard<std::mutex> lock(mutex_);state_=stopped?State::Idle:State::Failed;
  }
  uint32_t token()const{std::lock_guard<std::mutex> lock(mutex_);return token_;}
  static bool valid(const char* version){return slot(version)>=0;}
  void* getInterface(const char* version,vr::EVRInitError* error)const {
    const int index=slot(version);
    if(index<0){set(error,version?vr::VRInitError_Init_InterfaceNotFound:vr::VRInitError_Init_InvalidInterface);return nullptr;}
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_!=State::Running){set(error,vr::VRInitError_Init_NotInitialized);return nullptr;}
    set(error,vr::VRInitError_None);
    switch(index){case 0:return interfaces_.system;case 1:return interfaces_.compositor;case 2:return interfaces_.chaperone;default:return interfaces_.display;}
  }
  bool running()const{std::lock_guard<std::mutex> lock(mutex_);return state_==State::Running;}
 private:
  static int slot(const char* version) {
    if(!version)return -1;
    const char* const names[]={vr::IVRSystem_Version,vr::IVRCompositor_Version,vr::IVRChaperone_Version,vr::IVRExtendedDisplay_Version};
    // Inputs are caller-owned readable C strings. Bounded comparisons never
    // scan an arbitrarily long unsupported version looking for its terminator.
    for(int i=0;i<4;++i)if(std::strncmp(version,names[i],std::strlen(names[i])+1)==0)return i;
    return -1;
  }
  static void set(vr::EVRInitError* error,vr::EVRInitError value){if(error)*error=value;}
  static uint32_t fail(vr::EVRInitError* error,vr::EVRInitError value){set(error,value);return 0;}
  enum class State{Idle,Starting,Running,Stopping,Failed};
  RuntimeBackend& backend_;mutable std::mutex mutex_;RuntimeInterfaces interfaces_{};
  State state_=State::Idle;uint32_t token_=0;std::atomic<bool> cancelled_{false};
};
} // namespace edvr::openxr
