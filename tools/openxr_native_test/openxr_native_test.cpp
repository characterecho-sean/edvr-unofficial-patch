// Standalone diagnostic only. The game backend must bind Elite's own device.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "native_device.h"
#include "../../src/openxr/session_state.h"
#include "../../src/openxr/d3d11_stereo.h"
#include "../../src/openxr/projection_math.h"
#include "../../src/openxr/geometry_locator.h"
#include "../../src/openxr/openvr_system.h"
#include "../../src/openxr/system_publication.h"
#include "../../src/openxr/head_locator.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

using namespace edvr::openxr;
namespace {
struct Options { std::wstring loader; unsigned seconds=10; bool dry=false, self=false; };
bool absolute(const std::wstring& s) {
  return s.size()>3 && ((s[0]>=L'A' && s[0]<=L'Z') || (s[0]>=L'a' && s[0]<=L'z')) &&
         s[1]==L':' && (s[2]==L'\\' || s[2]==L'/');
}
bool duration(const std::wstring& s, unsigned& out) {
  if(s.empty() || s.size()>2) return false;
  unsigned n=0; for(wchar_t c:s) { if(c<L'0'||c>L'9') return false; n=n*10+c-L'0'; }
  if(n<1 || n>60) return false; out=n; return true;
}
bool parse(const std::vector<std::wstring>& args, Options& out) {
  Options o; bool seenLoader=false, seenSeconds=false;
  if(args.size()==1 && args[0]==L"--dry-run") {o.dry=true;out=o;return true;}
  if(args.size()==1 && args[0]==L"--self-test") {o.self=true;out=o;return true;}
  for(size_t i=0;i<args.size();++i) {
    if(args[i]==L"--loader" && !seenLoader && i+1<args.size()) {
      o.loader=args[++i]; seenLoader=true;
    } else if(args[i]==L"--seconds" && !seenSeconds && i+1<args.size()) {
      if(!duration(args[++i],o.seconds)) return false; seenSeconds=true;
    } else return false;
  }
  if(!absolute(o.loader)) return false; out=o; return true;
}
template<class T,class F> XrResult enumerate(F call,std::vector<T>& out,T initial=T{}) {
  out.clear(); uint32_t n=0; XrResult r=call(0,&n,nullptr);
  if(r!=XR_SUCCESS || !n) return r;
  for(unsigned attempt=0;attempt<3;++attempt) {
    if(n>4096) return XR_ERROR_LIMIT_REACHED;
    std::vector<T> values(n,initial); uint32_t written=0;
    r=call(n,&written,values.data());
    if(r==XR_ERROR_SIZE_INSUFFICIENT && written>n) {n=written;continue;}
    if(r!=XR_SUCCESS) return r;
    if(written>n) return XR_ERROR_RUNTIME_FAILURE;
    values.resize(written); out.swap(values); return XR_SUCCESS;
  }
  return XR_ERROR_SIZE_INSUFFICIENT;
}
bool validSize(const XrViewConfigurationView& v) {
  return v.recommendedImageRectWidth && v.recommendedImageRectHeight &&
    v.recommendedImageRectWidth<=v.maxImageRectWidth && v.recommendedImageRectHeight<=v.maxImageRectHeight &&
    v.maxSwapchainSampleCount>=1 && v.recommendedImageRectWidth<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
    v.recommendedImageRectHeight<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
struct Api {
  HMODULE module=nullptr; PFN_xrGetInstanceProcAddr get=nullptr;
  PFN_xrEnumerateInstanceExtensionProperties extensions=nullptr;
  PFN_xrCreateInstance createInstance=nullptr; PFN_xrDestroyInstance destroyInstance=nullptr;
  PFN_xrGetInstanceProperties instanceProperties=nullptr; PFN_xrGetSystem getSystem=nullptr;
  PFN_xrGetSystemProperties systemProperties=nullptr;
  PFN_xrConvertWin32PerformanceCounterToTimeKHR convertTime=nullptr;
  PFN_xrEnumerateViewConfigurations configurations=nullptr;
  PFN_xrEnumerateViewConfigurationViews viewSizes=nullptr;
  PFN_xrEnumerateEnvironmentBlendModes blends=nullptr;
  PFN_xrGetD3D11GraphicsRequirementsKHR requirements=nullptr;
  PFN_xrCreateSession createSession=nullptr; PFN_xrDestroySession destroySession=nullptr;
  PFN_xrEnumerateReferenceSpaces spaces=nullptr; PFN_xrCreateReferenceSpace createSpace=nullptr;
  PFN_xrDestroySpace destroySpace=nullptr; PFN_xrLocateViews locateViews=nullptr;
  PFN_xrLocateSpace locateSpace=nullptr; PFN_xrRequestExitSession requestExit=nullptr;
  Dispatch frames{}; StereoDispatch stereo{};
};
bool result(const char* operation,XrResult r) {
  if(r==XR_SUCCESS) return true;
  std::printf("result,%s,%d\n",operation,int(r)); return false;
}
template<class T> bool load(Api& a,XrInstance instance,const char* name,T& destination) {
  PFN_xrVoidFunction fn=nullptr; const XrResult r=a.get(instance,name,&fn);
  if(!result(name,r)) return false;
  if(!fn) return result(name,XR_ERROR_FUNCTION_UNSUPPORTED);
  destination=reinterpret_cast<T>(fn); return true;
}
bool counterNow(LARGE_INTEGER* value) { return QueryPerformanceCounter(value)!=FALSE; }
class Host : public SystemSource {
 public:
  Api api; XrInstance instance=XR_NULL_HANDLE; XrSession session=XR_NULL_HANDLE;
  XrSpace local=XR_NULL_HANDLE, view=XR_NULL_HANDLE; XrSystemId system=XR_NULL_SYSTEM_ID;
  NativeDevice graphics; SessionBinding binding; D3D11Stereo stereo; SessionState state;
  SystemPublication geometry; uint64_t geometryGeneration=0;
  OpenVRSystem systemInterface{*this};
  DWORD ownerThread=GetCurrentThreadId();
  XrResult lastHeadResult=XR_SUCCESS;XrTime lastHeadTime=0;
  XrViewConfigurationView sizes[2]{};
  bool clean=true;
  SystemRead read() const override {return geometry.read();}
  bool locateHead(uint64_t generation,vr::ETrackingUniverseOrigin origin,float prediction,vr::TrackedDevicePose_t& out) override {
    // This diagnostic only exercises callers on its existing frame owner.
    // The future game backend must marshal/protect lifetime across threads.
    if(GetCurrentThreadId()!=ownerThread||generation!=geometryGeneration||!read().connected||
       !state.running()||state.terminal()||origin!=vr::TrackingUniverseSeated)return false;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    lastHeadResult=HeadLocator{}.locate({api.convertTime,api.locateSpace},instance,view,local,prediction,head,&lastHeadTime,counterNow);
    if(lastHeadResult!=XR_SUCCESS)return false;
    vr::TrackedDevicePose_t pose{};pose.bDeviceIsConnected=true;
    pose.mDeviceToAbsoluteTracking.m[0][0]=pose.mDeviceToAbsoluteTracking.m[1][1]=pose.mDeviceToAbsoluteTracking.m[2][2]=1;
    constexpr auto valid=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    pose.bPoseIsValid=(head.locationFlags&valid)==valid;
    pose.eTrackingResult=pose.bPoseIsValid?vr::TrackingResult_Running_OK:vr::TrackingResult_Running_OutOfRange;
    if(pose.bPoseIsValid){double matrix[4][4];detail::rigid(head.pose,matrix);if(!detail::narrow(matrix,pose.mDeviceToAbsoluteTracking))return false;}
    out=pose;return true;
  }
  bool resetSeated(uint64_t) override {return false;}
  bool pollEvent(uint64_t,vr::ETrackingUniverseOrigin,vr::VREvent_t&,vr::TrackedDevicePose_t&) override {return false;}
  void unsupported(unsigned slot) noexcept override {std::printf("system_unavailable,slot=%u\n",slot);}
  ~Host() {close();}
  bool close() {
    geometry.retire(geometryGeneration);
    clean=result("destroy_swapchains",stereo.shutdown())&&clean;
    clean=result("destroy_binding",binding.shutdown())&&clean;
    view=local=XR_NULL_HANDLE;session=XR_NULL_HANDLE;
    state.abandonAfterOwnerDestruction();
    graphics.reset();
    if(instance && api.destroyInstance) {clean=result("xrDestroyInstance",api.destroyInstance(instance))&&clean;instance=XR_NULL_HANDLE;}
    if(api.module) {FreeLibrary(api.module);api.module=nullptr;}
    return clean;
  }
  bool open(const Options& options) {
    api.module=LoadLibraryExW(options.loader.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!api.module) {std::printf("error,LoadLibraryExW,%lu\n",GetLastError());return false;}
    api.get=reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(api.module,"xrGetInstanceProcAddr"));
    if(!api.get) return result("xrGetInstanceProcAddr",XR_ERROR_FUNCTION_UNSUPPORTED);
    if(!load(api,XR_NULL_HANDLE,"xrEnumerateInstanceExtensionProperties",api.extensions)||
       !load(api,XR_NULL_HANDLE,"xrCreateInstance",api.createInstance)) return false;
    std::vector<XrExtensionProperties> extensions;
    if(!result("extensions",enumerate<XrExtensionProperties>([&](uint32_t c,uint32_t*n,XrExtensionProperties*p){
      return api.extensions(nullptr,c,n,p);},extensions,{XR_TYPE_EXTENSION_PROPERTIES}))) return false;
    bool d3d=false,timeConversion=false;
    for(const auto& e:extensions) {
      d3d|=std::strncmp(e.extensionName,XR_KHR_D3D11_ENABLE_EXTENSION_NAME,XR_MAX_EXTENSION_NAME_SIZE)==0;
      timeConversion|=std::strncmp(e.extensionName,XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,XR_MAX_EXTENSION_NAME_SIZE)==0;
    }
    if(!d3d) return result("XR_KHR_D3D11_enable",XR_ERROR_EXTENSION_NOT_PRESENT);
    if(!timeConversion)return result("XR_KHR_win32_convert_performance_counter_time",XR_ERROR_EXTENSION_NOT_PRESENT);
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ci.applicationInfo.applicationName,"EDVR native stereo diagnostic");
    std::strcpy(ci.applicationInfo.engineName,"EDVR");ci.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
    const char* enabled[]={XR_KHR_D3D11_ENABLE_EXTENSION_NAME,XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};ci.enabledExtensionCount=2;ci.enabledExtensionNames=enabled;
    if(!result("xrCreateInstance",api.createInstance(&ci,&instance))) return false;
#define LOAD(name,field) if(!load(api,instance,name,api.field)) return false
    LOAD("xrDestroyInstance",destroyInstance); LOAD("xrGetInstanceProperties",instanceProperties);
    LOAD("xrGetSystem",getSystem); LOAD("xrEnumerateViewConfigurations",configurations);
    LOAD("xrGetSystemProperties",systemProperties);LOAD("xrConvertWin32PerformanceCounterToTimeKHR",convertTime);
    LOAD("xrEnumerateViewConfigurationViews",viewSizes); LOAD("xrEnumerateEnvironmentBlendModes",blends);
    LOAD("xrGetD3D11GraphicsRequirementsKHR",requirements);
    LOAD("xrDestroySession",destroySession); LOAD("xrCreateSession",createSession);
    LOAD("xrDestroySpace",destroySpace); LOAD("xrCreateReferenceSpace",createSpace);
    LOAD("xrEnumerateReferenceSpaces",spaces); LOAD("xrLocateViews",locateViews);
    LOAD("xrLocateSpace",locateSpace); LOAD("xrRequestExitSession",requestExit);
    LOAD("xrPollEvent",frames.pollEvent); LOAD("xrBeginSession",frames.beginSession);
    LOAD("xrEndSession",frames.endSession); LOAD("xrWaitFrame",frames.waitFrame);
    LOAD("xrBeginFrame",frames.beginFrame); LOAD("xrEndFrame",frames.endFrame);
    LOAD("xrEnumerateSwapchainFormats",stereo.enumerateSwapchainFormats);
    LOAD("xrCreateSwapchain",stereo.createSwapchain); LOAD("xrDestroySwapchain",stereo.destroySwapchain);
    LOAD("xrEnumerateSwapchainImages",stereo.enumerateSwapchainImages);
    LOAD("xrAcquireSwapchainImage",stereo.acquireSwapchainImage);
    LOAD("xrWaitSwapchainImage",stereo.waitSwapchainImage); LOAD("xrReleaseSwapchainImage",stereo.releaseSwapchainImage);
#undef LOAD
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if(!result("xrGetInstanceProperties",api.instanceProperties(instance,&ip))) return false;
    std::printf("runtime,%.*s,%llu\n",XR_MAX_RUNTIME_NAME_SIZE,ip.runtimeName,(unsigned long long)ip.runtimeVersion);
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};si.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if(!result("xrGetSystem",api.getSystem(instance,&si,&system))) return false;
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if(!result("xrGetSystemProperties",api.systemProperties(instance,system,&properties)))return false;
    std::vector<XrViewConfigurationType> configs;
    if(!result("view_configurations",enumerate<XrViewConfigurationType>([&](uint32_t c,uint32_t*n,XrViewConfigurationType*p){
      return api.configurations(instance,system,c,n,p);},configs))) return false;
    if(std::find(configs.begin(),configs.end(),XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)==configs.end())
      return result("PRIMARY_STEREO",XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
    std::vector<XrEnvironmentBlendMode> blends;
    if(!result("blend_modes",enumerate<XrEnvironmentBlendMode>([&](uint32_t c,uint32_t*n,XrEnvironmentBlendMode*p){
      return api.blends(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,c,n,p);},blends))) return false;
    if(std::find(blends.begin(),blends.end(),XR_ENVIRONMENT_BLEND_MODE_OPAQUE)==blends.end())
      return result("OPAQUE",XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED);
    std::vector<XrViewConfigurationView> views;
    if(!result("view_sizes",enumerate<XrViewConfigurationView>([&](uint32_t c,uint32_t*n,XrViewConfigurationView*p){
      return api.viewSizes(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,c,n,p);},views,{XR_TYPE_VIEW_CONFIGURATION_VIEW}))) return false;
    if(views.size()!=2 || !validSize(views[0]) || !validSize(views[1])) return result("stereo_sizes",XR_ERROR_VALIDATION_FAILURE);
    for(unsigned eye=0;eye<2;++eye) {sizes[eye]=views[eye];std::printf("size,%u,%u,%u\n",eye,sizes[eye].recommendedImageRectWidth,sizes[eye].recommendedImageRectHeight);}
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if(!result("xrGetD3D11GraphicsRequirementsKHR",api.requirements(instance,system,&req))) return false;
    const HRESULT hr=graphics.initialize(req.adapterLuid,req.minFeatureLevel);
    if(FAILED(hr)) {std::printf("error,D3D11Device,%08lx\n",(unsigned long)hr);return false;}
    std::printf("device,adapter=%08lx:%08lx,feature=%x\n",(unsigned long)req.adapterLuid.HighPart,(unsigned long)req.adapterLuid.LowPart,unsigned(graphics.device()->GetFeatureLevel()));
    const BindingDispatch bindingApi{api.requirements,api.createSession,api.destroySession,api.spaces,api.createSpace,api.destroySpace};
    if(!result("bind_existing_device",binding.initialize(bindingApi,instance,system,graphics.device())))return false;
    session=binding.session();local=binding.localSpace();view=binding.viewSpace();
    SystemRead metadata{};metadata.connected=true;
    std::memcpy(metadata.runtimeName,ip.runtimeName,sizeof(metadata.runtimeName));
    std::memcpy(metadata.systemName,properties.systemName,sizeof(metadata.systemName));
    // The index is resolved from the validated adapter, not from an HMD EDID.
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return result("system_adapter_factory",XR_ERROR_RUNTIME_FAILURE);
    for(UINT n=0;;++n){Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;const HRESULT status=factory->EnumAdapters1(n,&adapter);
      if(status==DXGI_ERROR_NOT_FOUND)break;if(FAILED(status))return result("system_adapter_enumeration",XR_ERROR_RUNTIME_FAILURE);
      DXGI_ADAPTER_DESC1 desc{};if(FAILED(adapter->GetDesc1(&desc)))return result("system_adapter_description",XR_ERROR_RUNTIME_FAILURE);
      if(NativeDevice::matchesLuid(desc.AdapterLuid,req.adapterLuid)){metadata.adapterIndex=int32_t(n);break;}
    }
    if(metadata.adapterIndex<0)return result("system_adapter_missing",XR_ERROR_GRAPHICS_DEVICE_INVALID);
    geometryGeneration=geometry.begin(metadata);
    if(!geometryGeneration)return result("geometry_generation",XR_ERROR_LIMIT_REACHED);
    if(!result("stereo_initialize",stereo.initialize(api.stereo,session,graphics.device(),sizes))) return false;
    std::printf("swapchain_format,%lld\n",(long long)stereo.format());
    return result("policy_initialize",state.reset(api.frames,instance,session,XR_ENVIRONMENT_BLEND_MODE_OPAQUE));
  }
};
void printPose(const char* name,const XrPosef& p) {
  std::printf("%s,position=%.9g/%.9g/%.9g,orientation=%.9g/%.9g/%.9g/%.9g\n",name,
    p.position.x,p.position.y,p.position.z,p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w);
}
int run(const Options& options) {
  Host host; if(!host.open(options)) return 3;
  const ULONGLONG startup=GetTickCount64(); ULONGLONG started=0,exitRequested=0;
  uint64_t frames=0,layers=0,empty=0,valid=0,invalid=0,headValid=0;
  bool stopped=false, failed=false, bootstrapComplete=false; Lifecycle previous=Lifecycle::Uninitialized;
  while(true) {
    XrResult r=host.state.pollEvents();
    const auto lifecycle=host.state.lifecycle();
    if(lifecycle!=previous) {std::printf("lifecycle,%d\n",int(lifecycle));previous=lifecycle;}
    if(XR_FAILED(r)||host.state.terminal()) {result("poll_terminal",r);failed=true;break;}
    if(lifecycle==Lifecycle::Stopping) {
      r=host.state.stop();stopped=result("xrEndSession",r);failed=!stopped;break;
    }
    if(lifecycle==Lifecycle::Ready && !host.state.running()) {
      r=host.state.startIfReady(); if(!result("xrBeginSession",r)) {failed=true;break;}
      started=GetTickCount64();
    }
    const auto now=GetTickCount64();
    if(!started && now-startup>=15000) {std::puts("error,startup_deadline");failed=true;break;}
    if(exitRequested && now-exitRequested>=5000) {std::puts("error,exit_deadline");failed=true;break;}
    if(host.state.running() && !exitRequested && started && now-started>=options.seconds*1000ULL) {
      r=host.api.requestExit(host.session);
      if(!result("xrRequestExitSession",r)) {failed=true;break;}
      exitRequested=now;
    }
    if(!host.state.running()) {Sleep(10);continue;}
    Frame frame; r=host.state.waitAndBegin(frame);
    if(r!=XR_SUCCESS && r!=XR_FRAME_DISCARDED && r!=XR_SESSION_LOSS_PENDING) {result("wait_begin",r);failed=true;break;}
    ++frames; XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layerHeader=nullptr;
    bool abortAfterEnd=host.state.terminal();
    GeometryInput located{};bool haveLocation=false;
    if(!exitRequested && !abortAfterEnd) {
      r=locateGeometry({host.api.locateViews,host.api.locateSpace},host.binding,frame,host.geometryGeneration,host.sizes,located);
      // An external hard failure retires this owner; do not use possibly lost handles to end a frame.
      if(XR_FAILED(r)) {result("locateGeometry",r);failed=true;break;}
      abortAfterEnd=r!=XR_SUCCESS;
      haveLocation=r==XR_SUCCESS;GeometrySnapshot candidate{};
      const bool geometry=haveLocation && makeGeometrySnapshot(located,candidate);
      if(geometry) {
        if(!valid) for(unsigned eye=0;eye<2;++eye) {
          const auto& view=located.views[eye];
          std::printf("initial_eye,%u,flags=%llu,fov=%.9g/%.9g/%.9g/%.9g\n",eye,(unsigned long long)located.viewFlags,
            view.fov.angleLeft,view.fov.angleRight,view.fov.angleUp,view.fov.angleDown);
          printPose("initial_eye_pose",view.pose);
        }
        ++valid;
      } else ++invalid;
      if(haveLocation) {
        constexpr auto flags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if((located.headFlags&flags)==flags && detail::poseValid(located.headPose)) {
          if(!headValid) {std::printf("initial_head_flags,%llu\n",(unsigned long long)located.headFlags);printPose("initial_head",located.headPose);} ++headValid;
        }
      }
      // Complete a zero-layer bootstrap frame before the diagnostic renders.
      // System readers can then consume native geometry before first Submit.
      if(!abortAfterEnd && bootstrapComplete && geometry && frame.shouldRender) {
        r=host.stereo.render(located.views,host.local,layer);
        if(XR_FAILED(r)) {result("stereo_render",r);failed=true;break;}
        if(r!=XR_SUCCESS) {result("stereo_render",r);abortAfterEnd=true;}
        else layerHeader=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
      }
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=layerHeader?1:0;end.layers=layerHeader?&layerHeader:nullptr;
    r=host.state.end(frame,end);
    if(!result("xrEndFrame",r)) {failed=true;break;}
    if(layerHeader) ++layers; else ++empty;
    if(abortAfterEnd) {std::puts("error,pending_or_external_frame_result");failed=true;break;}
    if(haveLocation) {
      const bool published=host.geometry.publish(located,false,false);
      if(published&&!bootstrapComplete) {
        const auto snapshot=host.read();vr::IVRSystem* system=&host.systemInterface;
        uint32_t width=0,height=0;system->GetRecommendedRenderTargetSize(&width,&height);
        const auto eye=system->GetEyeToHeadTransform(vr::Eye_Left);
        const auto projection=system->GetProjectionMatrix(vr::Eye_Left,.025f,50000,vr::API_DirectX);
        vr::TrackedDevicePose_t head{};system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated,0,&head,1);
        char name[XR_MAX_SYSTEM_NAME_SIZE]{};vr::ETrackedPropertyError propertyError=vr::TrackedProp_Success;
        const auto nameLength=system->GetStringTrackedDeviceProperty(0,vr::Prop_ModelNumber_String,name,sizeof(name),&propertyError);
        if(!width||!height||projection.m[3][2]!=-1||host.lastHeadResult!=XR_SUCCESS||
           !nameLength||propertyError!=vr::TrackedProp_Success){result("system_bootstrap_pose",host.lastHeadResult);failed=true;break;}
        if(!head.bPoseIsValid)continue; // bounded startup; ordinary invalid tracking may recover
        bootstrapComplete=true;
        std::printf("bootstrap,ready=1,generation=%llu,sequence=%llu,prior_stereo=%llu,size=%ux%u,left_eye_to_head_translation=%.9g/%.9g/%.9g\n",
          (unsigned long long)snapshot.generation,(unsigned long long)snapshot.geometry.native.sequence,(unsigned long long)layers,width,height,eye.m[0][3],eye.m[1][3],eye.m[2][3]);
        std::printf("system_abi,version=IVRSystem_012,absolute_pose_valid=1,prediction_seconds=0,clock=QPC_to_XrTime,model_bytes=%u,adapter=%d\n",nameLength,snapshot.adapterIndex);
      }
    }
  }
  const bool cleanup=host.close();
  std::printf("summary,frames=%llu,stereo=%llu,empty=%llu,valid_views=%llu,invalid_views=%llu,valid_head=%llu,normal_stop=%u,cleanup=%u\n",
    (unsigned long long)frames,(unsigned long long)layers,(unsigned long long)empty,(unsigned long long)valid,(unsigned long long)invalid,
    (unsigned long long)headValid,unsigned(stopped),unsigned(cleanup));
  const bool passed=!failed && stopped && cleanup && layers && headValid && bootstrapComplete;
  std::puts(passed?"native_stereo: PASS":"native_stereo: INCOMPLETE_OR_FAILED");return passed?0:4;
}
int selfTest() {
  unsigned checks=0,failures=0;auto check=[&](bool yes,const char* msg){++checks;if(!yes){++failures;std::printf("FAIL: %s\n",msg);}};
  Options o;
  check(parse({L"--loader",L"C:\\runtime\\loader.dll"},o)&&o.seconds==10,"default duration");
  check(parse({L"--seconds",L"60",L"--loader",L"D:/a.dll"},o)&&o.seconds==60,"bounded duration");
  for(const auto* s:{L"0",L"61",L"1x",L"-1",L"",L"999999999999",L" 1"})
    check(!parse({L"--loader",L"C:/a.dll",L"--seconds",s},o),"invalid duration");
  for(const auto* s:{L"x.dll",L"C:a.dll",L"/a.dll",L"1:/a.dll",L"C:/"}) check(!parse({L"--loader",s},o),"absolute loader required");
  check(parse({L"--dry-run"},o)&&o.dry,"dry-run standalone");
  check(!parse({L"--dry-run",L"--loader",L"C:/a.dll"},o),"dry-run cannot hide invalid operational args");
  check(!parse({L"--loader",L"C:/a.dll",L"--loader",L"C:/b.dll"},o),"duplicate rejected");
  std::vector<uint32_t> values;unsigned calls=0;
  auto grow=[&](uint32_t c,uint32_t*n,uint32_t*p){++calls;*n=c?3:1;if(!c)return XR_SUCCESS;if(c<3)return XR_ERROR_SIZE_INSUFFICIENT;p[0]=7;p[1]=8;p[2]=9;return XR_SUCCESS;};
  check(enumerate<uint32_t>(grow,values)==XR_SUCCESS&&calls==3&&values==std::vector<uint32_t>({7,8,9}),"count growth");
  check(enumerate<uint32_t>([](uint32_t c,uint32_t*n,uint32_t*){*n=c?5000:1;return c?XR_ERROR_SIZE_INSUFFICIENT:XR_SUCCESS;},values)==XR_ERROR_LIMIT_REACHED&&values.empty(),"retry count limit");
  check(enumerate<uint32_t>([](uint32_t c,uint32_t*n,uint32_t*){*n=c+1;return XR_SUCCESS;},values)==XR_ERROR_RUNTIME_FAILURE&&values.empty(),"invalid written count");
  check(enumerate<uint32_t>([](uint32_t,uint32_t*,uint32_t*){return XR_SESSION_LOSS_PENDING;},values)==XR_SESSION_LOSS_PENDING,"positive result retained");
  check(enumerate<uint32_t>([](uint32_t c,uint32_t*n,uint32_t*){*n=c+1;return c?XR_ERROR_SIZE_INSUFFICIENT:XR_SUCCESS;},values)==XR_ERROR_SIZE_INSUFFICIENT,"retry exhausted");
  XrViewConfigurationView size{XR_TYPE_VIEW_CONFIGURATION_VIEW};size.recommendedImageRectWidth=size.recommendedImageRectHeight=128;
  size.maxImageRectWidth=size.maxImageRectHeight=512;size.maxSwapchainSampleCount=1;
  check(validSize(size),"valid size");size.recommendedImageRectHeight=0;check(!validSize(size),"zero height");
  std::printf("openxr_native_test: %u checks, %u failures (no runtime)\n",checks,failures);return failures?1:0;
}
} // namespace
int wmain(int argc,wchar_t** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
  std::setvbuf(stdout,nullptr,_IONBF,0);Options options;
  if(!parse(std::vector<std::wstring>(argv+1,argv+argc),options)) {std::puts("usage: --loader ABSOLUTE_DLL [--seconds 1..60] | --self-test | --dry-run");return 2;}
  if(options.dry) {std::puts("Would create a standalone OpenXR stereo diagnostic; no files, loader, device or runtime used.");return 0;}
  if(options.self) return selfTest();
  try {return run(options);} catch(const std::exception& e) {std::printf("error,exception,%s\n",e.what());return 5;}
}
