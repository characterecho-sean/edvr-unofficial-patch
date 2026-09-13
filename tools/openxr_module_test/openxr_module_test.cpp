#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/native_module.h"
#include "../../src/openxr/native_bootstrap.h"
#include "../../src/openvr/compat/openvr_v0_9_20.h"
#include "../openxr_native_test/present_device.h"
#include "scene.h"
#include "viewing_phase.h"
#include "bootstrap_tests.h"
#include "init_error_tests.h"
#include "system_caller.h"
#include "system_caller_tests.h"
#include "system_queries.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <limits>
#include <climits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace edvr::openxr;
using Microsoft::WRL::ComPtr;
namespace {
std::atomic<unsigned> checks{0},failures{0};
void check(bool result,const char* label) { ++checks; if(!result){++failures;std::printf("FAIL: %s\n",label);} }
struct Watchdog {
  std::atomic<bool> done{false};
  std::thread thread;
  explicit Watchdog(unsigned seconds=30):thread([this,seconds] {
    const auto deadline=GetTickCount64()+seconds*1000ULL;
    while(!done.load()) { if(GetTickCount64()>=deadline)std::_Exit(9);Sleep(10); }
  }){}
  ~Watchdog(){done=true;thread.join();}
};
struct Exports {
  HMODULE module=nullptr;
  using Init=uint32_t(__cdecl*)(vr::EVRInitError*,vr::EVRApplicationType);
  using Shutdown=void(__cdecl*)();
  using Generic=void*(__cdecl*)(const char*,vr::EVRInitError*);
  using Valid=bool(__cdecl*)(const char*);
  using Token=uint32_t(__cdecl*)();
  Init init=nullptr; Shutdown shutdown=nullptr; Generic generic=nullptr; Valid valid=nullptr; Token token=nullptr;
  using ErrorText=const char*(__cdecl*)(vr::EVRInitError);
  ErrorText symbol=nullptr,description=nullptr,legacyDescription=nullptr;
  decltype(&edvrConfigureNativeRuntime) configure=nullptr;
  decltype(&edvrGetNativeRuntimeStatus) getStatus=nullptr;
  bool load(const std::wstring& path) {
    module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module)return false;
    init=reinterpret_cast<Init>(GetProcAddress(module,"VR_InitInternal"));
    shutdown=reinterpret_cast<Shutdown>(GetProcAddress(module,"VR_ShutdownInternal"));
    generic=reinterpret_cast<Generic>(GetProcAddress(module,"VR_GetGenericInterface"));
    valid=reinterpret_cast<Valid>(GetProcAddress(module,"VR_IsInterfaceVersionValid"));
    token=reinterpret_cast<Token>(GetProcAddress(module,"VR_GetInitToken"));
    symbol=reinterpret_cast<ErrorText>(GetProcAddress(module,"VR_GetVRInitErrorAsSymbol"));
    description=reinterpret_cast<ErrorText>(GetProcAddress(module,"VR_GetVRInitErrorAsEnglishDescription"));
    legacyDescription=reinterpret_cast<ErrorText>(GetProcAddress(module,"VR_GetStringForHmdError"));
    configure=reinterpret_cast<decltype(configure)>(GetProcAddress(module,"edvrConfigureNativeRuntime"));
    getStatus=reinterpret_cast<decltype(getStatus)>(GetProcAddress(module,"edvrGetNativeRuntimeStatus"));
    return init&&shutdown&&generic&&valid&&token&&configure&&getStatus&&symbol&&description&&legacyDescription;
  }
  EdvrNativeRuntimeStatus status() const {
    EdvrNativeRuntimeStatus out{sizeof(out),EDVR_NATIVE_MODULE_VERSION_1};
    if(FAILED(getStatus(&out)))check(false,"module status snapshot");return out;
  }
  // Configured module and hook-owning graphics provider are process lifetime.
};
std::atomic<uint64_t>* g_presentCount=nullptr;
HRESULT countedPresent(PresentDevice& device) {
  if(g_presentCount)g_presentCount->fetch_add(1,std::memory_order_relaxed);
  return device.present();
}
std::filesystem::path executableDirectory() {
  wchar_t path[32768]{};const auto n=GetModuleFileNameW(nullptr,path,32768);
  return n&&n<32768?std::filesystem::path(path).parent_path():std::filesystem::path{};
}
bool absolute(const std::wstring& p) {
  return p.size()>3&&p.find(L'\0')==std::wstring::npos&&
    ((p[0]>=L'A'&&p[0]<=L'Z')||(p[0]>=L'a'&&p[0]<=L'z'))&&p[1]==L':'&&(p[2]==L'\\'||p[2]==L'/');
}
struct Options {std::wstring loader,graphics,module;unsigned seconds=10;bool present=false,bootstrap=false,separate=false;};
bool parse(int argc,wchar_t** argv,Options& options) {
  bool seenSeconds=false;
  for(int i=1;i<argc;++i) {
    const std::wstring flag=argv[i];
    if(flag==L"--present-boundary"&&!options.present){options.present=true;continue;}
    if(flag==L"--bootstrap"&&!options.bootstrap){options.bootstrap=true;continue;}
    if(flag==L"--separate-device"&&!options.separate){options.separate=true;continue;}
    if(i+1==argc)return false;
    const std::wstring value=argv[++i];
    if(flag==L"--loader"&&options.loader.empty())options.loader=value;
    else if(flag==L"--graphics-proxy"&&options.graphics.empty())options.graphics=value;
    else if(flag==L"--runtime-module"&&options.module.empty())options.module=value;
    else if(flag==L"--seconds"&&!seenSeconds) {
      if(value.empty()||value.size()>2)return false;
      unsigned seconds=0;for(wchar_t c:value){if(c<L'0'||c>L'9')return false;seconds=seconds*10+c-L'0';}
      if(seconds<1||seconds>60)return false;options.seconds=seconds;seenSeconds=true;
    } else return false;
  }
  return options.present&&! (options.bootstrap&&options.separate)&&absolute(options.loader)&&absolute(options.graphics)&&absolute(options.module);
}

void whilePresenting(PresentDevice& device,const std::function<void()>& task) {
  std::atomic<bool> finished{false};
  std::thread caller([&] {try{task();}catch(...){check(false,"foreign caller exception");}finished=true;});
  bool presented=true;
  while(!finished.load(std::memory_order_acquire)) {presented=SUCCEEDED(countedPresent(device))&&presented;Sleep(1);}
  caller.join();
  check(presented,"Present progress");
}

struct StopOnExit {
  Exports& api; PresentDevice& device; edvr::openxr::OwnerService* systemOwner=nullptr;
  DWORD* shutdownCaller=nullptr; bool allowPresentPump=true, needed=true;
  void stop() {
    if (!needed) return;
    if (systemOwner) {
      const auto shutdown = edvr::openxr::module_test::submitWithPump(
          *systemOwner, [&] { return !allowPresentPump || SUCCEEDED(countedPresent(device)); }, [&] {
            if (shutdownCaller) *shutdownCaller = GetCurrentThreadId();
            api.shutdown();
          });
      check(shutdown.submitted && shutdown.completed && shutdown.succeeded &&
            shutdown.pumpSucceeded, "persistent System caller shuts down native module");
      check(systemOwner->stop(), "persistent System caller joins after shutdown");
    } else {
      whilePresenting(device, [&] { api.shutdown(); });
    }
    needed=false;
  }
  ~StopOnExit() {try{stop();}catch(...){std::puts("error,module_cleanup_exception");}}
};

bool samePose(const vr::TrackedDevicePose_t& a,const vr::TrackedDevicePose_t& b) {
  return std::memcmp(a.mDeviceToAbsoluteTracking.m,b.mDeviceToAbsoluteTracking.m,sizeof(a.mDeviceToAbsoluteTracking.m))==0&&
      std::memcmp(a.vVelocity.v,b.vVelocity.v,sizeof(a.vVelocity.v))==0&&
      std::memcmp(a.vAngularVelocity.v,b.vAngularVelocity.v,sizeof(a.vAngularVelocity.v))==0&&
      a.eTrackingResult==b.eTrackingResult&&a.bPoseIsValid==b.bPoseIsValid&&a.bDeviceIsConnected==b.bDeviceIsConnected;
}

void sceneTest(ID3D11Device* device,const std::filesystem::path& directory) {
  HMODULE fixture=LoadLibraryExW((directory/L"openxr_export_fixture.dll").c_str(),nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  check(fixture!=nullptr,"load independent System ABI fixture");if(!fixture)return;
  const auto bind=reinterpret_cast<bool(__cdecl*)()>(GetProcAddress(fixture,"edvrBindRuntimeFixture"));
  const auto init=reinterpret_cast<Exports::Init>(GetProcAddress(fixture,"VR_InitInternal"));
  const auto generic=reinterpret_cast<Exports::Generic>(GetProcAddress(fixture,"VR_GetGenericInterface"));
  const auto shutdown=reinterpret_cast<Exports::Shutdown>(GetProcAddress(fixture,"VR_ShutdownInternal"));
  check(bind&&init&&generic&&shutdown,"scene fixture exports");if(!(bind&&init&&generic&&shutdown))return;
  vr::EVRInitError error{};check(bind()&&init(&error,vr::VRApplication_Scene)!=0,"scene fixture init");
  auto* system=static_cast<vr::IVRSystem*>(generic(vr::IVRSystem_Version,&error));
  check(system!=nullptr,"scene fixture System");if(!system)return;
  edvr::openxr::OwnerService fixtureOwner;
  check(fixtureOwner.start(),"scene fixture persistent System owner");
  module_test::SystemQuerySnapshot fixtureQueries{};
  module_test::PeriodicSystemSample fixturePeriodic{};
  const auto fixtureQuery = fixtureOwner.running()
      ? module_test::submitWithPump(fixtureOwner, [] { return true; }, [&] {
          fixtureQueries = {};
          check(module_test::collectSystemQueries(*system, fixtureQueries, false),
                "scene fixture shared System query validation");
          fixturePeriodic=module_test::collectPeriodicSystemSample(*system);
        })
      : module_test::OwnerTaskResult{};
  check(fixtureQuery.submitted && fixtureQuery.completed && fixtureQuery.succeeded,
        "scene fixture System query completion");
  check(fixtureQueries.width == 640 && fixtureQueries.height == 480 &&
        fixtureQueries.geometryValid && fixtureQueries.poseQueried &&
        !fixtureQueries.poseValid,
        "scene fixture publishes bounded geometry and invalid pose");
  check(!fixtureQueries.modelPropertyValid && !fixtureQueries.trackingPropertyValid,
        "scene fixture keeps unsupported empty metadata honest");
  check(fixturePeriodic.geometryValid && !fixturePeriodic.poseValid && fixturePeriodic.eventQueried,
        "periodic observer exercises real System ABI with empty event queue");
  ModuleTestScene scene;
  check(scene.initialize(device,0,128)==E_INVALIDARG&&scene.initialize(device,2049,128)==E_INVALIDARG,
        "scene dimensions are bounded");
  check(scene.initialize(device,128,128)==S_OK,"scene shader and resource creation");
  vr::TrackedDevicePose_t pose{};pose.bPoseIsValid=pose.bDeviceIsConnected=true;
  for(unsigned i=0;i<3;++i)pose.mDeviceToAbsoluteTracking.m[i][i]=1;
  check(scene.render(*system,pose)==S_OK,"scene draws both eyes from OpenVR matrices");
  ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
  for(unsigned eye=0;eye<2;++eye) {
    auto* texture=scene.eye(eye);check(texture!=nullptr,"scene eye exists");if(!texture)continue;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&readback)),"scene readback texture");
    if(!readback)continue;
    context->CopyResource(readback.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto mappedResult=context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped);check(SUCCEEDED(mappedResult),"scene readback completes");
    if(SUCCEEDED(mappedResult)) {
      auto* center=static_cast<unsigned char*>(mapped.pData)+64*mapped.RowPitch+64*4;
      auto* corner=static_cast<unsigned char*>(mapped.pData);
      check(unsigned(center[0])+center[1]+center[2]>100&&center[3]==255,"projected triangle covers center");
      check(corner[0]<16&&corner[1]<16&&corner[2]<16&&corner[3]==255,"scene clear remains outside triangle");
      context->Unmap(readback.Get(),0);
    }
  }
  pose.mDeviceToAbsoluteTracking.m[0][3]=std::numeric_limits<float>::quiet_NaN();
  check(scene.render(*system,pose)==E_INVALIDARG,"scene rejects nonfinite pose");
  check(scene.eye(2)==nullptr,"scene eye indexing bounded");
  if (fixtureOwner.running()) {
    const auto fixtureShutdown = module_test::submitWithPump(
        fixtureOwner, [] { return true; }, [&] { shutdown(); });
    check(fixtureShutdown.submitted && fixtureShutdown.completed && fixtureShutdown.succeeded,
          "scene fixture shutdown runs on persistent System owner");
    check(fixtureOwner.stop(), "scene fixture System owner joins");
  } else shutdown();
  FreeLibrary(fixture);
}

void viewingPhaseTests() {
  using S=ViewingPhase::State;
  ViewingPhase late(0,20);
  check(late.tick(0,false)==S::Warmup&&late.tick(11000,false)==S::Warmup,"standby time does not spend viewing budget");
  check(late.tick(11500,true)==S::Warmup&&late.tick(11999,true)==S::Warmup,"focus must settle before grid budget");
  check(late.tick(12000,true)==S::Grid&&late.phaseElapsedMs()==0,"grid starts after focused warmup");
  check(late.tick(14999,true)==S::Grid&&late.tick(15000,true)==S::Scene,"late wake retains full three-second grid");
  check(late.tick(31999,true)==S::Scene&&late.tick(32000,true)==S::Complete,"late wake retains seventeen-second scene");
  check(late.tick(60000,false)==S::Complete,"completed viewing remains complete");
  ViewingPhase reset(0,20);reset.tick(0,true);reset.tick(500,true);
  check(reset.tick(1500,false)==S::Warmup&&reset.phaseElapsedMs()==0,"grid focus loss restarts readiness");
  check(reset.tick(2000,true)==S::Warmup&&reset.tick(2500,true)==S::Grid,"focus recovery repeats warmup");
  check(reset.tick(5499,true)==S::Grid&&reset.tick(5500,true)==S::Scene,"interrupted grid is replayed for full interval");
  ViewingPhase pause(0,20);pause.tick(0,true);pause.tick(500,true);pause.tick(3500,true);pause.tick(4500,true);
  check(pause.tick(14500,false)==S::Scene&&pause.phaseElapsedMs()==1000,"unfocused scene interval is not credited");
  check(pause.tick(25000,true)==S::Scene&&pause.phaseElapsedMs()==1000,"focus recovery does not charge paused time");
  check(pause.tick(40999,true)==S::Scene&&pause.tick(41000,true)==S::Complete,"scene resumes its remaining viewing budget");
  ViewingPhase absent(0,20);
  check(absent.tick(29999,false)==S::Warmup&&absent.tick(30000,false)==S::Expired,"absent focus expires startup as failure");
  ViewingPhase hard(0,20);hard.tick(0,true);hard.tick(500,true);hard.tick(3500,true);
  check(hard.tick(49999,false)==S::Scene&&hard.tick(50000,false)==S::Expired,"focus loss cannot extend fixed hard deadline");
  ViewingPhase shortRun(0,1);
  check(shortRun.gridBudgetMs()==250&&shortRun.sceneBudgetMs()==750,"short diagnostic retains both viewing phases");
  check(shortRun.tick(0,true)==S::Warmup&&shortRun.tick(500,true)==S::Grid&&
        shortRun.tick(750,true)==S::Scene&&shortRun.tick(1500,true)==S::Complete,"zero epoch and short run boundaries");
  ViewingPhase backwards(1000,20);
  check(backwards.tick(999,true)==S::Expired,"clock reversal cannot credit viewing time");
  check(ViewingPhase(0,0).state()==S::Expired&&ViewingPhase(0,61).state()==S::Expired,"invalid viewing duration rejected");
}

// Only used inside this isolated desktop test process. Restore inherited values
// after all workers join; never write user or machine environment settings.
struct TestEnvironment {
  const wchar_t* names[2]={L"EDVR_OPENXR_LOADER",L"EDVR_OPENXR_GRAPHICS"};
  std::wstring saved[2]; bool existed[2]{};
  TestEnvironment() {
    for(unsigned i=0;i<2;++i) {
      SetLastError(ERROR_SUCCESS);const DWORD size=GetEnvironmentVariableW(names[i],nullptr,0);
      existed[i]=size!=0||GetLastError()==ERROR_SUCCESS;
      if(size){std::vector<wchar_t> buffer(size);const DWORD n=GetEnvironmentVariableW(names[i],buffer.data(),size);
        if(n>=size)throw std::runtime_error("test environment changed during capture");saved[i].assign(buffer.data(),n);}
    }
    for(const auto* name:names)check(SetEnvironmentVariableW(name,nullptr)!=FALSE,"clear child bootstrap environment");
  }
  ~TestEnvironment(){for(unsigned i=0;i<2;++i)SetEnvironmentVariableW(names[i],existed[i]?saved[i].c_str():nullptr);}
  void set(const wchar_t* loader,const wchar_t* graphics) {
    check(SetEnvironmentVariableW(names[0],loader)!=FALSE&&SetEnvironmentVariableW(names[1],graphics)!=FALSE,"set child bootstrap environment");
  }
};

void exportedErrorTests(const Exports& api) {
  check(!std::strcmp(api.symbol(vr::VRInitError_None),"VRInitError_None"),"DLL returns exact success symbol");
  check(!std::strcmp(api.symbol(vr::VRInitError_Init_NotInitialized),"VRInitError_Init_NotInitialized"),"DLL returns exact native failure symbol");
  for(const auto value:{vr::VRInitError_None,vr::VRInitError_Init_InterfaceNotFound,
      vr::VRInitError_Init_InstallationCorrupt,static_cast<vr::EVRInitError>(-97)}) {
    const auto text=api.description(value);
    check(text&&*text&&text==api.description(value)&&text==api.legacyDescription(value),"DLL error description and legacy alias have stable lifetime");
  }
  std::atomic<bool> consistent{true};const auto text=api.symbol(vr::VRInitError_Init_NotInitialized);
  std::thread reader([&]{for(unsigned i=0;i<100;++i)if(api.symbol(vr::VRInitError_Init_NotInitialized)!=text||
      std::strcmp(api.symbol(static_cast<vr::EVRInitError>(INT_MAX)),"VRInitError_Unknown"))consistent=false;});
  reader.join();check(consistent,"DLL error strings are safe across callers");
}

void parserModeTests(const std::filesystem::path& directory) {
  const std::wstring loader=(directory/L"loader.dll").wstring();
  const std::wstring graphics=(directory/L"d3d11.dll").wstring();
  const std::wstring module=(directory/L"module.dll").wstring();
  auto run=[&](std::initializer_list<const wchar_t*> args) {
    std::vector<std::wstring> values;for(const auto* value:args)values.emplace_back(value);
    std::vector<wchar_t*> pointers;for(auto& value:values)pointers.push_back(value.data());
    Options parsed{};return parse(static_cast<int>(pointers.size()),pointers.data(),parsed);
  };
  check(run({L"test",L"--loader",loader.c_str(),L"--graphics-proxy",graphics.c_str(),L"--runtime-module",module.c_str(),L"--present-boundary",L"--separate-device"}),
    "separate-device command mode parses");
  check(!run({L"test",L"--loader",loader.c_str(),L"--graphics-proxy",graphics.c_str(),L"--runtime-module",module.c_str(),L"--present-boundary",L"--bootstrap",L"--separate-device"}),
    "bootstrap and separate-device modes cannot combine");
}

int selfTest(bool bootstrap=false,bool separate=false) {
  Watchdog watchdog;
  TestEnvironment environment;
  viewingPhaseTests();
  bootstrap_test::runBootstrapTests(check);
  runInitErrorTests(check);
  module_test::runSystemCallerTests(check);
  const auto directory=executableDirectory();const auto graphicsPath=(directory/L"d3d11.dll").wstring();
  parserModeTests(directory);
  const auto missingLoader=(directory/L"intentionally-absent-openxr-loader.dll").wstring();
  check(!std::filesystem::exists(missingLoader),"negative loader fixture is absent");
  if(std::filesystem::exists(missingLoader))return 1;
  Exports api;check(api.load((directory/L"edvr_openxr_runtime.dll").wstring()),"load actual native module exports");
  if(!api.configure||!api.symbol||!api.description||!api.legacyDescription)return 1;
  exportedErrorTests(api);
  EdvrNativeRuntimeStatus before{sizeof(before),EDVR_NATIVE_MODULE_VERSION_1};
  check(api.getStatus(&before)==S_FALSE&&before.phase==0&&before.initAttempts==0&&api.token()==0,"error exports leave module unconfigured");
  vr::EVRInitError error=vr::VRInitError_Unknown;
  check(api.init(&error,vr::VRApplication_Scene)==0&&error==vr::VRInitError_Init_NotInitialized,"unconfigured Init fails without a runtime");
  check(api.token()==0&&api.valid(vr::IVRSystem_Version)&&!api.valid("IVROverlay_011"),"exact support table exists before Init");
  check(!api.generic(vr::IVRSystem_Version,&error)&&error==vr::VRInitError_Init_NotInitialized,"uninitialized interface retrieval");
  api.shutdown();
  EdvrNativeRuntimeConfig config{sizeof(config),EDVR_NATIVE_MODULE_VERSION_1,missingLoader.c_str(),graphicsPath.c_str(),100,0};
  auto invalid=config;invalid.size=4;check(api.configure(&invalid)==E_INVALIDARG,"short module config rejected");
  invalid=config;invalid.version=99;check(api.configure(&invalid)==E_INVALIDARG,"unknown module config version rejected");
  invalid=config;invalid.reserved=EDVR_NATIVE_GRAPHICS_SEPARATE_DEVICE;check(api.configure(&invalid)==E_INVALIDARG,"v1 separate-device flag rejected");
  invalid=config;invalid.version=EDVR_NATIVE_MODULE_VERSION_2;invalid.reserved=2;check(api.configure(&invalid)==E_INVALIDARG,"v2 unknown graphics flag rejected");
  invalid=config;invalid.version=EDVR_NATIVE_MODULE_VERSION_2;check(api.configure(&invalid)==E_INVALIDARG,"v2 requires explicit separate-device flag");
  invalid=config;invalid.loaderPath=L"loader.dll";check(api.configure(&invalid)==E_INVALIDARG,"relative runtime loader rejected");
  invalid=config;invalid.renderWaitMilliseconds=0;check(api.configure(&invalid)==E_INVALIDARG,"zero startup wait rejected");
  if(bootstrap) {
    environment.set(missingLoader.c_str(),nullptr);
    check(api.init(&error,vr::VRApplication_Scene)==0&&error==vr::VRInitError_Init_InstallationCorrupt,"partial bootstrap paths rejected before configuration");
    environment.set(L"loader.dll",graphicsPath.c_str());
    check(api.init(&error,vr::VRApplication_Scene)==0&&error==vr::VRInitError_Init_InstallationCorrupt,"relative bootstrap path rejected before configuration");
    before={sizeof(before),EDVR_NATIVE_MODULE_VERSION_1};
    check(api.getStatus(&before)==S_FALSE&&api.token()==0,"rejected bootstrap does not publish a module or generation");
    environment.set(missingLoader.c_str(),graphicsPath.c_str());
  } else {
    config.version=separate?EDVR_NATIVE_MODULE_VERSION_2:EDVR_NATIVE_MODULE_VERSION_1;
    config.reserved=separate?EDVR_NATIVE_GRAPHICS_SEPARATE_DEVICE:0;
    check(api.configure(&config)==S_OK&&api.configure(&config)==E_PENDING,"configure once outside loader lock");
  }
  check(api.init(&error,vr::VRApplication_Overlay)==0&&error==vr::VRInitError_Init_NotSupportedWithCompositor,"unsupported application does not start backend");
  check(api.status().initAttempts==0,"unsupported application creates no generation");
  check(api.init(&error,vr::VRApplication_Scene)==0&&error==vr::VRInitError_Init_HmdNotFound,"unloaded graphics provider fails before runtime");
  check(api.configure(&config)==E_PENDING,"first Init fixes the module configuration");
  if(bootstrap)environment.set(L"invalid-after-configuration",nullptr);
  check(api.status().cleanup==1&&api.status().ownerThread==0,"absent provider cleanup needs no XR owner");
  HMODULE proxy=LoadLibraryExW(graphicsPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  check(proxy!=nullptr,"load actual graphics proxy");if(!proxy)return 1;
  check(api.init(&error,vr::VRApplication_Scene)==0&&error==vr::VRInitError_Init_HmdNotFound,"unpublished device rejected");
  PresentDevice device;check(device.initialize(proxy,D3D_DRIVER_TYPE_WARP)==S_OK,"create WARP app device before Init");
  if(!device.device())return 1;
  // This caller deliberately makes no Present while the separate Init waits.
  std::atomic<bool> initDone{false};vr::EVRInitError cancelledError=vr::VRInitError_Unknown;
  std::thread waiting([&]{check(api.init(&cancelledError,vr::VRApplication_Scene)==0,"cancelled Init returns no token");initDone=true;});
  const auto deadline=GetTickCount64()+2000;
  while(!initDone&&api.status().phase!=EDVR_NATIVE_STARTING&&GetTickCount64()<deadline)Sleep(1);
  vr::EVRInitError retry{};
  check(api.init(&retry,vr::VRApplication_Scene)==0&&retry==vr::VRInitError_Init_Retry,"concurrent Init rejected during startup");
  api.shutdown();waiting.join();
  check(cancelledError==vr::VRInitError_Init_ShuttingDown,"Shutdown cancels the waiting initializer");
  auto status=api.status();check(status.cleanup==1&&status.ownerThread==0&&status.renderCallbacks==0,"missing Present/cancellation leaves no native owner or callback work");
  // A subsequent generation gets real Present progress, reaches the missing
  // loader, and cleans partial owner startup through the DLL's real exports.
  for(unsigned attempt=0;attempt<2;++attempt) {
    whilePresenting(device,[&] {check(api.init(&error,vr::VRApplication_Scene)==0&&error==vr::VRInitError_Init_Internal,"missing loader fails after render admission");});
    status=api.status();
    check(status.cleanup==1&&!status.retained&&status.ownerThread&&status.renderThread&&
        status.initThread!=status.renderThread&&status.ownerThread!=status.renderThread,
        "failed native startup joins owner and retires callback for retry");
  }
  check(status.initAttempts==5,"failed generations remain distinct across retries");
  exportedErrorTests(api);
  const auto afterErrors=api.status();
  check(api.token()!=0&&std::memcmp(&status,&afterErrors,sizeof(status))==0,"error strings do not mutate failed-startup status");
  check(GetModuleHandleW(L"intentionally-absent-openxr-loader.dll")==nullptr,"desktop fixture loads no OpenXR runtime");
  sceneTest(device.device(),directory);
  return failures?1:0;
}

bool makeSkybox(ID3D11Device* device,ComPtr<ID3D11Texture2D> (&textures)[6]) {
  constexpr unsigned side=256;std::vector<uint32_t> pixels(side*side);
  const unsigned colors[6][3]={{70,20,20},{20,70,20},{20,20,70},{70,70,20},{70,20,70},{20,70,70}};
  for(unsigned face=0;face<6;++face) {
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
      const bool line=x%32<2||y%32<2;const bool marker=x>112&&x<144&&y>64&&y<96;
      uint32_t pixel=0xff000000u;
      for(unsigned c=0;c<3;++c)pixel|=(marker?210:line?130:colors[face][c])<<(c*8);
      pixels[y*side+x]=pixel;
    }
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=side;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(),side*4,0};
    if(FAILED(device->CreateTexture2D(&desc,&data,&textures[face])))return false;
  }
  return true;
}

int nativeRun(const Options& options) {
  Exports api;if(!api.load(options.module)){std::puts("error,module_load_or_exports");return 3;}
  HMODULE proxy=LoadLibraryExW(options.graphics.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if(!proxy)return 3;
  PresentDevice device;if(FAILED(device.initialize(proxy,D3D_DRIVER_TYPE_HARDWARE)))return 3;
  std::atomic<uint64_t> presentCount{0};g_presentCount=&presentCount;
  EdvrNativeRuntimeConfig config{sizeof(config),options.separate?EDVR_NATIVE_MODULE_VERSION_2:EDVR_NATIVE_MODULE_VERSION_1,
      options.loader.c_str(),options.graphics.c_str(),5000,options.separate?EDVR_NATIVE_GRAPHICS_SEPARATE_DEVICE:0};
  if(options.bootstrap) {
    BootstrapPaths paths;
    if(readBootstrapPaths(paths)!=BootstrapResult::Ready||paths.loader!=options.loader||paths.graphics!=options.graphics){
      std::puts("error,module_bootstrap_paths_do_not_match_command");return 3;
    }
    std::puts("module_configuration,source=bootstrap_requested,embedding_call=0");
  } else if(api.configure(&config)!=S_OK)return 3;
  ModuleTestScene scene; // Shutdown guard runs before app eye resources retire.
  edvr::openxr::OwnerService systemOwner;
  const bool systemOwnerStarted = systemOwner.start();
  check(systemOwnerStarted, "native persistent System owner starts before Init");
  if (!systemOwnerStarted) return 3;
  DWORD initCaller=0, systemCaller=0, renderCaller=GetCurrentThreadId(), shutdownCaller=0;
  StopOnExit stop{api,device,&systemOwner,&shutdownCaller,!options.separate,true};
  uint32_t token=0;vr::EVRInitError error{};vr::IVRSystem* system=nullptr;
  vr::IVRSystem* initialSystem=nullptr;vr::IVRCompositor* compositor=nullptr;
  whilePresenting(device,[&] {
    initCaller=GetCurrentThreadId();
    token=api.init(&error,vr::VRApplication_Scene);
    if(!token||error!=vr::VRInitError_None)return;
    initialSystem=static_cast<vr::IVRSystem*>(api.generic(vr::IVRSystem_Version,&error));
    compositor=static_cast<vr::IVRCompositor*>(api.generic(vr::IVRCompositor_Version,&error));
    check(compositor,"owned native compositor crosses DLL boundary");
    check(api.init(&error,vr::VRApplication_Scene)==token,"repeated exported Init preserves token");
    check(!api.generic("IVROverlay_011",&error)&&error==vr::VRInitError_Init_InterfaceNotFound,"unsupported interface rejected across DLL boundary");
  });
  if(!token||!compositor){std::printf("error,module_init,%d,%s,%s\n",int(error),api.symbol(error),api.description(error));return 3;}
  const auto systemIdentity = module_test::submitWithPump(
      systemOwner, [&] { return SUCCEEDED(device.present()); }, [&] {
        systemCaller=GetCurrentThreadId();
        system=static_cast<vr::IVRSystem*>(api.generic(vr::IVRSystem_Version,&error));
      });
  check(systemIdentity.submitted && systemIdentity.completed && systemIdentity.succeeded &&
        systemIdentity.pumpSucceeded && system && system==initialSystem,
        "native System interface retrieval runs on persistent caller");
  if(!system){std::printf("error,module_system,%d,%s,%s\n",int(error),api.symbol(error),api.description(error));return 3;}
  module_test::SystemQuerySnapshot startupQueries{};
  const auto startup = module_test::submitWithPump(
      systemOwner, [&] { return SUCCEEDED(device.present()); }, [&] {
        check(module_test::collectSystemQueries(*system, startupQueries, true),
              "native startup System geometry and properties");
      });
  check(startup.submitted && startup.completed && startup.succeeded && startup.pumpSucceeded &&
        startupQueries.geometryValid &&
        startupQueries.poseQueried && startupQueries.modelPropertyValid &&
        startupQueries.trackingPropertyValid,
        "native startup System queries complete before application wait");
  if(!startupQueries.geometryValid)return 3;
  const uint32_t width=startupQueries.width,height=startupQueries.height;
  check(initCaller!=systemCaller && systemCaller!=renderCaller,
        "Init, persistent System, and Present callers are distinct");
  std::printf("module_callers,init=%lu,system=%lu,render=%lu,startup_geometry=%ux%u,pose_valid=%u\n",
      static_cast<unsigned long>(initCaller), static_cast<unsigned long>(systemCaller),
      static_cast<unsigned long>(renderCaller), width, height,
      unsigned(startupQueries.poseValid));
  std::printf("module_startup_queries,geometry=1,properties=%u,pose_queried=%u,before_application_wait=1\n",
      unsigned(startupQueries.modelPropertyValid && startupQueries.trackingPropertyValid),
      unsigned(startupQueries.poseQueried));
  std::fflush(stdout);
  const double scale=(std::min)(1.0,2048.0/(std::max)(width,height));
  bool success=SUCCEEDED(scene.initialize(device.device(),uint32_t(width*scale),uint32_t(height*scale)));
  ComPtr<ID3D11Texture2D> sky[6];success=makeSkybox(device.device(),sky)&&success;
  vr::Texture_t skyTextures[6]{};for(unsigned i=0;i<6;++i)skyTextures[i]={sky[i].Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
  success=success&&compositor->SetSkyboxOverride(skyTextures,6)==vr::VRCompositorError_None;
  for(auto& texture:sky)texture.Reset(); // owned DLL capture must survive source release
  const auto began=GetTickCount64();ViewingPhase viewing(began,options.seconds);
  using Phase=ViewingPhase::State;Phase previousPhase=Phase::Warmup;
  bool previousFocus=false,focusLogged=false;
  std::printf("module_phase,name=waiting,elapsed_ms=0,grid_ms=%llu,scene_ms=%llu\n",
      (unsigned long long)viewing.gridBudgetMs(),(unsigned long long)viewing.sceneBudgetMs());
  std::fflush(stdout);
  uint64_t frames=0,invalid=0,cacheChecks=0,systemSamples=0,validSystemSamples=0,applicationWaits=0;
  bool applicationWaitLogged=false;
  uint64_t nextSystemSample=GetTickCount64();
  while(success) {
    const auto now=GetTickCount64();const bool focused=compositor->CanRenderScene();
    if(!focusLogged||focused!=previousFocus) {
      std::printf("module_focus,focused=%u,elapsed_ms=%llu\n",unsigned(focused),(unsigned long long)(now-began));
      std::fflush(stdout);focusLogged=true;previousFocus=focused;
    }
    const auto phase=viewing.tick(now,focused);
    if(phase!=previousPhase) {
      const char* name=phase==Phase::Grid?"grid":phase==Phase::Scene?"scene":phase==Phase::Complete?"complete":phase==Phase::Expired?"expired":"waiting";
      std::printf("module_phase,name=%s,elapsed_ms=%llu,focused=%u\n",name,(unsigned long long)(now-began),unsigned(focused));
      std::fflush(stdout);
      if(phase==Phase::Grid) {
        // Recenter after readiness, so putting on the headset cannot leave the
        // scene anchored to the orientation sampled while it was on the desk.
        bool resetValid=false;
        const auto recenter = module_test::submitWithPump(
            systemOwner, [&] { return SUCCEEDED(countedPresent(device)); }, [&] {
              system->ResetSeatedZeroPose();vr::VREvent_t reset{};vr::TrackedDevicePose_t pose{};
              resetValid=system->PollNextEventWithPose(vr::TrackingUniverseSeated,&reset,sizeof(reset),&pose)&&
                  reset.eventType==vr::VREvent_SeatedZeroPoseReset&&pose.bPoseIsValid;
            });
        check(recenter.submitted&&recenter.completed&&recenter.succeeded&&recenter.pumpSucceeded,
              "focused startup recenter runs on persistent System caller");
        check(resetValid,"focused startup recenter event");success=success&&resetValid;
        std::printf("module_recenter,valid=%u,elapsed_ms=%llu\n",unsigned(resetValid),(unsigned long long)(GetTickCount64()-began));
      }
      if(phase==Phase::Scene)compositor->ClearSkyboxOverride();
      previousPhase=phase;
    }
    if(phase==Phase::Complete)break;
    if(phase==Phase::Expired){std::puts("error,module_viewing_deadline");success=false;break;}
    if(!success)break;
    if(phase!=Phase::Scene){success=SUCCEEDED(countedPresent(device));Sleep(1);continue;}
    // Sample only between completed scene pairs. There is no mid-pair reset
    // or general asynchronous game-call scheduler in this diagnostic gate.
    if (focused && frames && GetTickCount64() >= nextSystemSample) {
      module_test::PeriodicSystemSample sample{};
      const auto periodic = module_test::submitWithPump(
          systemOwner, [&] { return SUCCEEDED(countedPresent(device)); }, [&] {
            sample=module_test::collectPeriodicSystemSample(*system);
          });
      ++systemSamples;
      if (periodic.succeeded && sample.geometryValid && sample.poseValid) ++validSystemSamples;
      check(periodic.submitted&&periodic.completed&&periodic.succeeded&&periodic.pumpSucceeded&&
            sample.geometryValid&&sample.eventQueried,
            "periodic System geometry and event query");
      if (!periodic.succeeded || !periodic.pumpSucceeded || !sample.geometryValid || !sample.poseValid) {
        ++invalid;
        success=false;
        break;
      }
      nextSystemSample=GetTickCount64()+250;
    }
    vr::TrackedDevicePose_t poses[2]{},game[2]{},cached[2]{},cachedGame[2]{};
    const auto waited=compositor->WaitGetPoses(poses,2,game,2);
    if(waited!=vr::VRCompositorError_None){success=false;break;}
    ++applicationWaits;
    if (!applicationWaitLogged) {
      std::printf("module_application_wait,count=1,after_startup_queries=1\n");
      std::fflush(stdout);
      applicationWaitLogged=true;
    }
    if(!focused||!poses[0].bPoseIsValid){if(focused)++invalid;compositor->ClearLastSubmittedFrame();success=SUCCEEDED(countedPresent(device));continue;}
    if(compositor->GetLastPoses(cached,2,cachedGame,2)!=vr::VRCompositorError_None||
       !samePose(cached[0],poses[0])||!samePose(cached[1],poses[1])||
       !samePose(cachedGame[0],game[0])||!samePose(cachedGame[1],game[1])){success=false;break;}
    ++cacheChecks;
    if(FAILED(scene.render(*system,poses[0]))){success=false;break;}
    for(unsigned n=0;n<2;++n) {
      const unsigned eye=n^unsigned(frames&1);
      const vr::Texture_t texture{scene.eye(eye),vr::API_DirectX,vr::ColorSpace_Gamma};
      if(compositor->Submit(vr::EVREye(eye),&texture)!=vr::VRCompositorError_None){success=false;break;}
    }
    if(!success)break;
    compositor->PostPresentHandoff();++frames;
    if(frames==1){std::printf("module_scene,first_pair=1,elapsed_ms=%llu\n",(unsigned long long)(GetTickCount64()-began));std::fflush(stdout);}
    success=SUCCEEDED(countedPresent(device));
  }
  compositor->ClearLastSubmittedFrame();
  const auto presentsBeforeShutdown=presentCount.load(std::memory_order_acquire);
  stop.stop();
  const auto status=api.status();
  if(options.separate) {
    std::printf("module_owned_shutdown,presents_before=%llu,presents_after=%llu,cleanup=%u,retained=%u\n",
      (unsigned long long)presentsBeforeShutdown,(unsigned long long)presentCount.load(std::memory_order_acquire),
      status.cleanup,status.retained);
    check(presentCount.load(std::memory_order_acquire)==presentsBeforeShutdown,
      "separate-device shutdown uses CPU-only System wait without Present service");
    check(status.phase==EDVR_NATIVE_STOPPED&&status.cleanup&&!status.retained&&status.renderCallbacks>0,
      "separate-device shutdown cleans dedicated native resources and retires callback");
  }
  const bool callerSummary = initCaller && systemCaller && shutdownCaller &&
      initCaller != systemCaller && initCaller != renderCaller && systemCaller != renderCaller &&
      systemCaller == shutdownCaller && status.initThread == initCaller &&
      status.renderThread == renderCaller && status.shutdownThread == shutdownCaller &&
      status.ownerThread && status.ownerThread != systemCaller &&
      status.ownerThread != renderCaller && status.ownerThread != initCaller;
  check(callerSummary, "native caller summary identifies persistent System and native XR owners");
  success=success&&viewing.state()==Phase::Complete&&!failures&&frames>0&&invalid==0&&status.phase==EDVR_NATIVE_STOPPED&&status.cleanup&&!status.retained&&
      status.copiedEyes==frames*2&&status.stereoPairs==frames&&status.loadingLayers>0&&!status.wrongThread&&
      systemSamples>0&&validSystemSamples>0&&applicationWaits>0&&callerSummary&&api.token()!=token;
  check(!api.generic(vr::IVRSystem_Version,&error)&&error==vr::VRInitError_Init_NotInitialized,"exported interfaces retire after Shutdown");
  std::printf("module_summary,frames=%llu,cached=%llu,invalid=%llu,copies=%llu,pairs=%llu,loading=%llu,callbacks=%llu,system_samples=%llu,valid_system_samples=%llu,application_waits=%llu,cleanup=%u,retained=%u,init=%u,system=%lu,render=%u,owner=%u,shutdown=%u,app_shutdown=%lu\n",
      (unsigned long long)frames,(unsigned long long)cacheChecks,(unsigned long long)invalid,
      (unsigned long long)status.copiedEyes,(unsigned long long)status.stereoPairs,(unsigned long long)status.loadingLayers,
      (unsigned long long)status.renderCallbacks,(unsigned long long)systemSamples,
      (unsigned long long)validSystemSamples,(unsigned long long)applicationWaits,
      status.cleanup,status.retained,status.initThread,
      static_cast<unsigned long>(systemCaller),status.renderThread,status.ownerThread,status.shutdownThread,
      static_cast<unsigned long>(shutdownCaller));
  std::puts(success&&!failures?"native_module: PASS":"native_module: INCOMPLETE_OR_FAILED");
  return success&&!failures?0:4;
}
}
int wmain(int argc,wchar_t** argv) {
  if(argc==2&&!std::wcscmp(argv[1],L"--dry-run")) {std::puts("Would test the separate native DLL exports; no DLL, device, runtime or files created.");return 0;}
  if(argc==2&&!std::wcscmp(argv[1],L"--self-test")) {const auto result=selfTest();std::printf("openxr_module_test: %u checks, %u failures (no OpenXR runtime)\n",checks.load(),failures.load());return result;}
  if(argc==2&&!std::wcscmp(argv[1],L"--self-test-bootstrap")) {const auto result=selfTest(true);std::printf("openxr_module_bootstrap_test: %u checks, %u failures (no OpenXR runtime)\n",checks.load(),failures.load());return result;}
  if(argc==2&&!std::wcscmp(argv[1],L"--self-test-separate")) {const auto result=selfTest(false,true);std::printf("openxr_module_separate_test: %u checks, %u failures (no OpenXR runtime)\n",checks.load(),failures.load());return result;}
  Options options;if(!parse(argc,argv,options)){std::fputs("usage: --self-test|--self-test-bootstrap|--self-test-separate|--dry-run|--loader ABS --graphics-proxy ABS --runtime-module ABS --present-boundary [--bootstrap|--separate-device] [--seconds 1..60]\n",stderr);return 2;}
  try{return nativeRun(options);}catch(...){std::puts("error,module_test_exception");return 5;}
}
