#include "native_runtime_host.h"
#include "native_render_binding.h"
#include "render_shutdown.h"
#include "native_module.h"
#include "native_bootstrap.h"
#include "init_error.h"
#include <new>
#include <stdexcept>

namespace {
using namespace edvr::openxr;

class ModuleBackend final : public RuntimeBackend {
 public:
  ModuleBackend(RuntimeOptions options, uint32_t wait) : options_(std::move(options)), wait_(wait) {
    status_.size=sizeof(status_); status_.version=EDVR_NATIVE_MODULE_VERSION_1;
    status_.phase=EDVR_NATIVE_CONFIGURED;
  }

  vr::EVRInitError start(uint32_t token, const std::atomic<bool>& cancelled,
                        RuntimeInterfaces& interfaces) override {
    if (attempts_>=16) return vr::VRInitError_Init_TooManyObjects;
    // Preserve retired facade addresses for the module lifetime. No native
    // handles or worker survive successful stop. Failed stop blocks restart
    // in RuntimeLifecycle and retains the entire uncertain generation.
    auto* generation=new Generation;
    generation->previous=current_; current_=generation; ++attempts_;
    update([&](auto& s) {
      s={sizeof(s),EDVR_NATIVE_MODULE_VERSION_1}; s.phase=EDVR_NATIVE_STARTING;
      s.initAttempts=attempts_; s.initThread=GetCurrentThreadId();
    });
    if (cancelled.load(std::memory_order_acquire)) return vr::VRInitError_Init_ShuttingDown;
    const HRESULT acquired=generation->binding.acquire(options_.graphicsProxy,[generation] {
      if (!generation->loadingAdmitted.load(std::memory_order_acquire)) return;
      if (!generation->route.invoke([generation] { generation->host->loadingBoundary(); }))
        throw std::runtime_error("native loading boundary unavailable");
    });
    if (acquired!=S_OK) {
      std::printf("module_startup,graphics_unavailable=%08lx\n",(unsigned long)acquired);
      return vr::VRInitError_Init_HmdNotFound;
    }
    if (!generation->binding.waitForRender(std::chrono::milliseconds(wait_))) {
      std::puts("module_startup,no_present_before_runtime=1");
      return vr::VRInitError_Init_HmdNotFound;
    }
    if (cancelled.load(std::memory_order_acquire)) return vr::VRInitError_Init_ShuttingDown;
    generation->route.present=&generation->binding.work();
    if (!generation->route.bind()) return vr::VRInitError_Init_Internal;
    if (!generation->owner.start([generation] {
      if (generation->host) generation->host->pumpEvents();
    })) return vr::VRInitError_Init_Internal;
    auto error=vr::VRInitError_Init_Internal;
    const bool started=generation->route.invoke([&] {
      generation->host=std::make_unique<NativeRuntimeHost>(generation->owner,
          generation->render,generation->route,generation->binding.device());
      auto options=options_; options.graphicsProvider=generation->binding.provider();
      generation->host->startupOptions=std::move(options);
      error=generation->host->start(token,cancelled,interfaces);
    });
    update([&](auto& s) {
      s.renderThread=generation->binding.renderThread();
      s.ownerThread=generation->host?generation->host->ownerThread:0;
      if (started&&error==vr::VRInitError_None) s.phase=EDVR_NATIVE_RUNNING;
    });
    std::printf("module_startup,token=%u,init=%lu,render=%lu,owner=%lu,result=%d\n",token,
        (unsigned long)GetCurrentThreadId(),(unsigned long)generation->binding.renderThread(),
        (unsigned long)(generation->host?generation->host->ownerThread:0),int(error));
    std::fflush(stdout);
    if (started&&error==vr::VRInitError_None)
      generation->loadingAdmitted.store(true,std::memory_order_release);
    return started?error:vr::VRInitError_Init_Internal;
  }

  bool stop() noexcept override {
    auto* generation=current_;
    if (!generation) return true;
    if (generation->stopped) return !generation->retained;
    try {
      generation->loadingAdmitted.store(false,std::memory_order_release);
      update([&](auto& s) { s.shutdownThread=GetCurrentThreadId(); });
      bool cleaned=true, joined=true, atBoundary=false;
      if (generation->owner.running()) {
        bool drained=false;
        const bool invoked=generation->route.invoke([&] {
          if (generation->host) {
            // Disable idle frame activity before the final GPU fence.
            generation->host->serviceStopped=true;
            drained=generation->host->stereo.drain()==XR_SUCCESS;
          } else drained=true;
        });
        if (!invoked||!drained) return retain(*generation,"gpu_drain_or_render_unavailable");
        const auto stopped=shutdownAtRenderBoundary(generation->binding.work(),
            generation->render,generation->owner,[&] {
              cleaned=!generation->host||generation->host->stop();
            });
        atBoundary=stopped.entered; joined=stopped.joined;
        if (!atBoundary||!joined||!cleaned) return retain(*generation,"owner_teardown_incomplete");
      } else if (generation->host) {
        return retain(*generation,"host_without_running_owner");
      }
      generation->render.close();
      // A foreign Shutdown caller may wake just before the callback epilogue.
      // Close admission now and retain everything until that epilogue finishes.
      HRESULT released=generation->binding.release();
      const auto deadline=GetTickCount64()+5000;
      while (released==E_PENDING&&GetTickCount64()<deadline&&
             GetCurrentThreadId()!=generation->binding.renderThread()) {
        Sleep(1); released=generation->binding.release();
      }
      if (released!=S_OK) return retain(*generation,"callback_release_pending");
      generation->stopped=true;
      update([&](auto& s) {
        s.phase=EDVR_NATIVE_STOPPED; s.cleanup=1;
        s.renderCallbacks=generation->binding.callbacks();
        if (generation->host) {
          s.waits=generation->host->compositorWaits; s.submits=generation->host->compositorSubmits;
          s.stereoPairs=generation->host->composedPairs; s.copiedEyes=generation->host->copiedEyes;
          s.loadingLayers=generation->host->loadingLayers; s.wrongThread=generation->host->graphicsCalls.wrongThread;
        }
      });
      std::printf("module_shutdown,owner_joined=%u,cleanup=%u,boundary=%u,callback_retired=1,retained=0\n",
          unsigned(joined),unsigned(cleaned),unsigned(atBoundary));
      std::fflush(stdout);
      return true;
    } catch (...) { return retain(*generation,"shutdown_exception"); }
  }

  EdvrNativeRuntimeStatus status() const {
    std::lock_guard<std::mutex> lock(statusMutex_); return status_;
  }

 private:
  struct Generation {
    OwnerService owner;
    RenderThreadDispatcher render{owner};
    RenderRoute route{render};
    NativeRenderBinding binding;
    std::unique_ptr<NativeRuntimeHost> host;
    Generation* previous=nullptr;
    std::atomic<bool> loadingAdmitted{false};
    bool stopped=false, retained=false;
  };
  bool retain(Generation& generation, const char* reason) noexcept {
    // No ExitProcess, unsafe destroy, or worker detachment. Prevent further
    // queued work and idle pumping, join CPU work, keep every XR/GPU object.
    // Entered runtime calls still require completion; no timeout frees them.
    generation.retained=true; generation.stopped=true;
    generation.loadingAdmitted.store(false,std::memory_order_release);
    try {
      generation.render.close(); generation.binding.close(); generation.owner.stop();
      update([&](auto& s) { s.phase=EDVR_NATIVE_RETAINED; s.retained=1; s.cleanup=0; });
    } catch (...) { /* The pinned generation remains alive even on failure. */ }
    std::printf("module_shutdown,retained=1,reason=%s\n",reason); std::fflush(stdout);
    return false;
  }
  template<class F> void update(F&& f) {
    std::lock_guard<std::mutex> lock(statusMutex_); f(status_);
  }
  RuntimeOptions options_;
  uint32_t wait_, attempts_=0;
  Generation* current_=nullptr;
  mutable std::mutex statusMutex_;
  EdvrNativeRuntimeStatus status_{};
};
struct Module {
  ModuleBackend backend;
  RuntimeLifecycle lifecycle;
  Module(RuntimeOptions options,uint32_t wait):backend(std::move(options),wait),lifecycle(backend){}
};
std::atomic<Module*> module{nullptr};
std::mutex configureMutex;

bool copyPath(const wchar_t* input,std::wstring& output) {
  if (!input) return false;
  size_t length=0; while(length<32768&&input[length])++length;
  if (length<=3||length>=32768) return false;
  if (!((input[0]>=L'A'&&input[0]<=L'Z')||(input[0]>=L'a'&&input[0]<=L'z'))||
      input[1]!=L':'||(input[2]!=L'\\'&&input[2]!=L'/')) return false;
  output.assign(input,length); return true;
}
}

extern "C" HRESULT WINAPI edvrConfigureNativeRuntime(const EdvrNativeRuntimeConfig* config) try {
  if (!config||config->size!=sizeof(*config)||config->version!=EDVR_NATIVE_MODULE_VERSION_1||
      config->reserved||!config->renderWaitMilliseconds||config->renderWaitMilliseconds>5000) return E_INVALIDARG;
  RuntimeOptions options;
  if (!copyPath(config->loaderPath,options.loader)||!copyPath(config->graphicsProxyPath,options.graphicsProxy)) return E_INVALIDARG;
  std::lock_guard<std::mutex> lock(configureMutex);
  if (module.load(std::memory_order_acquire)) return E_PENDING;
  auto owner=std::make_unique<Module>(std::move(options),config->renderWaitMilliseconds);
  HMODULE pinned=nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(&edvrConfigureNativeRuntime),&pinned)) return HRESULT_FROM_WIN32(GetLastError());
  module.store(owner.release(),std::memory_order_release);
  return S_OK;
} catch (...) { return E_OUTOFMEMORY; }

extern "C" HRESULT WINAPI edvrGetNativeRuntimeStatus(EdvrNativeRuntimeStatus* status) try {
  if (!status||status->size!=sizeof(*status)||status->version!=EDVR_NATIVE_MODULE_VERSION_1) return E_INVALIDARG;
  auto* owner=module.load(std::memory_order_acquire);
  if (!owner) { *status={sizeof(*status),EDVR_NATIVE_MODULE_VERSION_1}; return S_FALSE; }
  *status=owner->backend.status(); return S_OK;
} catch (...) { return E_FAIL; }

extern "C" uint32_t __cdecl edvr_module_VR_InitInternal(vr::EVRInitError* error,vr::EVRApplicationType application) noexcept {
  try {
    if (auto* owner=module.load(std::memory_order_acquire)) return owner->lifecycle.init(error,application);
    // The staged DLL can also be initialized by an unmodified application:
    // its launcher supplies both trusted paths in this process environment.
    // No file/registry selection or fallback to another OpenVR backend occurs.
    if (application!=vr::VRApplication_Scene) {
      if(error)*error=vr::VRInitError_Init_NotSupportedWithCompositor;return 0;
    }
    BootstrapPaths paths;
    const auto result=readBootstrapPaths(paths);
    if (result!=BootstrapResult::Ready) {
      if(error)*error=result==BootstrapResult::Unconfigured?
          vr::VRInitError_Init_NotInitialized:vr::VRInitError_Init_InstallationCorrupt;
      return 0;
    }
    const EdvrNativeRuntimeConfig config{sizeof(config),EDVR_NATIVE_MODULE_VERSION_1,
        paths.loader.c_str(),paths.graphics.c_str(),5000,0};
    const HRESULT configured=edvrConfigureNativeRuntime(&config);
    // Another Init or explicit configuration may have published the winner.
    // Use that immutable configuration; never reconfigure a running module.
    if (configured!=S_OK&&configured!=E_PENDING) {
      if(error)*error=vr::VRInitError_Init_InstallationCorrupt;return 0;
    }
    if (configured==S_OK) {
      std::puts("module_configuration,source=environment");std::fflush(stdout);
    }
    if (auto* owner=module.load(std::memory_order_acquire))return owner->lifecycle.init(error,application);
    if(error)*error=vr::VRInitError_Init_Internal;
  } catch (...) { if(error)*error=vr::VRInitError_Init_Internal; }
  return 0;
}
extern "C" void __cdecl edvr_module_VR_ShutdownInternal() noexcept {
  try { if(auto* owner=module.load(std::memory_order_acquire))owner->lifecycle.shutdown(); } catch (...) {}
}
extern "C" void* __cdecl edvr_module_VR_GetGenericInterface(const char* version,vr::EVRInitError* error) noexcept {
  try { if(auto* owner=module.load(std::memory_order_acquire))return owner->lifecycle.getInterface(version,error); }
  catch (...) { if(error)*error=vr::VRInitError_Init_Internal;return nullptr; }
  if(error)*error=vr::VRInitError_Init_NotInitialized;return nullptr;
}
extern "C" bool __cdecl edvr_module_VR_IsInterfaceVersionValid(const char* version) noexcept {
  return RuntimeLifecycle::valid(version);
}
extern "C" uint32_t __cdecl edvr_module_VR_GetInitToken() noexcept {
  try { if(auto* owner=module.load(std::memory_order_acquire))return owner->lifecycle.token(); } catch (...) {}
  return 0;
}

extern "C" const char* __cdecl edvr_module_VR_GetVRInitErrorAsSymbol(vr::EVRInitError error) noexcept {
  return initErrorSymbol(error);
}
extern "C" const char* __cdecl edvr_module_VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError error) noexcept {
  return initErrorDescription(error);
}
// Valve's openvr_api_public.cpp declares this legacy export with EVRInitError
// and implements it as the English-description alias. Not a generic thunk.
extern "C" const char* __cdecl edvr_module_VR_GetStringForHmdError(vr::EVRInitError error) noexcept {
  return edvr_module_VR_GetVRInitErrorAsEnglishDescription(error);
}
