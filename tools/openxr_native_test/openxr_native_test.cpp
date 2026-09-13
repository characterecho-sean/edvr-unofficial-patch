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
#include "../../src/openxr/runtime_gate.h"
#include "../../src/openxr/frame_boundary.h"
#include "../../src/openxr/eye_capture.h"
#include "../../src/openxr/skybox_capture.h"
#include "../../src/openxr/loading_state.h"
#include "../../src/openxr/openvr_compositor.h"
#include "../../src/openxr/compositor_publication.h"
#include "../../src/openxr/space_pose.h"
#include "../../src/openxr/seated_origin.h"
#include "../../src/openxr/seated_space.h"
#include "../../src/openxr/reference_changes.h"
#include "../../src/openxr/reset_events.h"
#include "../../src/openxr/runtime_exports.h"
#include "../../src/openxr/openvr_auxiliary.h"
#include "../../src/openxr/owner_service.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <thread>
#include <memory>

using namespace edvr::openxr;
extern "C" vr::IVRCompositor* nativeCompositorCaller(vr::IVRCompositor*);
extern "C" vr::IVRSystem* nativeSystemCaller(vr::IVRSystem*);
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
class Host : public SystemSource, public FrameSink, public CompositorSource, public AuxiliarySource, public RuntimeBackend {
 public:
  explicit Host(OwnerService& owner):service(owner){}
  OwnerService& service;
  Options startupOptions;
  uint64_t starts=0,stops=0,startupFrames=0;
  uint64_t eventPumps=0;
  std::atomic<uint64_t> publishedPumps{0};
  bool serviceStopped=false,serviceFailed=false;
  OpenVRExtendedDisplay displayInterface{*this};OpenVRChaperone chaperoneInterface{*this};
  Api api; XrInstance instance=XR_NULL_HANDLE; XrSession session=XR_NULL_HANDLE;
  XrSpace local=XR_NULL_HANDLE, view=XR_NULL_HANDLE; XrSystemId system=XR_NULL_SYSTEM_ID;
  NativeDevice graphics; SessionBinding binding; D3D11Stereo stereo; SessionState state;
  EyeCapture captured;
  SkyboxCapture skybox;
  LoadingState loading;
  uint64_t loadingClears=0;
  uint64_t skyboxSets=0,skyboxClears=0,loadingFrames=0,loadingLayers=0,loadingEmpty=0,loadingToScene=0;
  SeatedSpace seated;ReferenceChanges changes;ResetEvents resetEvents;
  XrSpace frameSpace=XR_NULL_HANDLE;
  uint64_t recenters=0,resetPolls=0,referenceChanges=0;
  XrResult lastResetResult=XR_SUCCESS;
  XrTime lastResetTime=0;
  float resetPositionError=0,resetYawError=0;
  FrameBoundary boundary{state,*this};
  RuntimeGate gate; uint64_t runtimeGeneration=0;
  XrView frameViews[2]{};
  uint64_t copiedEyes=0, composedPairs=0;
  SystemPublication geometry; uint64_t geometryGeneration=0;
  OpenVRSystem systemInterface{*this};
  CompositorPublication poses;uint64_t compositorGeneration=0;
  OpenVRCompositor compositorInterface{this};
  GeometryInput frameGeometry{};bool frameGeometryAvailable=false;
  XrResult lastCompositorResult=XR_SUCCESS;
  uint64_t compositorWaits=0,compositorSubmits=0,compositorHandoffs=0,validGamePoses=0;
  DWORD ownerThread=GetCurrentThreadId();
  XrResult lastHeadResult=XR_SUCCESS;XrTime lastHeadTime=0;
  XrViewConfigurationView sizes[2]{};
  bool clean=true;
  AuxiliaryRead readAuxiliary()const override {
    const auto snapshot=geometry.read();AuxiliaryRead out{};
    out.generation=snapshot.generation;out.connected=snapshot.connected;
    if(snapshot.geometryValid)for(unsigned eye=0;eye<2;++eye){out.width[eye]=snapshot.geometry.native.width[eye];out.height[eye]=snapshot.geometry.native.height[eye];}
    return out;
  }
  void auxiliaryUnsupported(unsigned interfaceId,unsigned slot)noexcept override {
    std::printf("auxiliary_unavailable,interface=%u,slot=%u\n",interfaceId,slot);
  }
  vr::EVRInitError start(uint32_t token,const std::atomic<bool>& cancelled,RuntimeInterfaces& out) override {
    ++starts;
    // Constructed on the service thread with its thread-bound frame/device
    // members. The diagnostic owns its device; game-device use remains separate.
    if(GetCurrentThreadId()!=ownerThread||starts!=1)return vr::VRInitError_Init_Internal;
    if(cancelled.load(std::memory_order_acquire))return vr::VRInitError_Init_ShuttingDown;
    if(!open(startupOptions))return vr::VRInitError_Init_Internal;
    const auto began=GetTickCount64();
    while(!cancelled.load(std::memory_order_acquire)&&GetTickCount64()-began<15000) {
      auto operation=gate.tryEnter(runtimeGeneration);
      if(!operation)return vr::VRInitError_Init_Internal;
      XrResult r=state.pollEvents();
      if(r!=XR_SUCCESS||state.terminal()||!changes.active())return vr::VRInitError_Init_Internal;
      if(state.lifecycle()==Lifecycle::Stopping)return vr::VRInitError_Init_ShuttingDown;
      if(state.lifecycle()==Lifecycle::Ready&&!state.running()) {
        r=state.startIfReady();if(r!=XR_SUCCESS)return vr::VRInitError_Init_Internal;
      }
      if(!state.running()){operation=RuntimeGate::Lease{};Sleep(10);continue;}
      operation=RuntimeGate::Lease{};
      CompositorRead initial{};
      if(waitPoses(compositorGeneration,initial)!=vr::VRCompositorError_None)return vr::VRInitError_Init_Internal;
      ++startupFrames;
      if(closeDiagnosticFrame()!=XR_SUCCESS)return vr::VRInitError_Init_Internal;
      const auto snapshot=read();
      if(snapshot.geometryValid) {
        out={&systemInterface,&compositorInterface,&chaperoneInterface,&displayInterface};
        std::printf("runtime_startup,token=%u,zero_layer_frames=%llu,geometry_sequence=%llu,prior_submits=%llu,geometry_ready=1\n",
          token,(unsigned long long)startupFrames,(unsigned long long)snapshot.geometry.native.sequence,(unsigned long long)compositorSubmits);
        return vr::VRInitError_None;
      }
    }
    std::puts("error,runtime_startup_cancelled_or_deadline");
    return cancelled.load(std::memory_order_acquire)?vr::VRInitError_Init_ShuttingDown:vr::VRInitError_Init_HmdNotFound;
  }
  bool stop()noexcept override {
    ++stops;
    if(runtimeGeneration)gate.requestStop(runtimeGeneration);
    geometry.retire(geometryGeneration);poses.retire(compositorGeneration);resetEvents.retire(geometryGeneration);
    if(GetCurrentThreadId()!=ownerThread)return false;
    return close();
  }
  void pumpEvents() {
    if(GetCurrentThreadId()!=ownerThread||!runtimeGeneration||serviceStopped||serviceFailed)return;
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation){serviceFailed=true;return;}
    ++eventPumps;
    XrResult r=state.pollEvents();
    publishedPumps.store(eventPumps,std::memory_order_release);
    const auto lifecycle=state.lifecycle();
    poses.focus(compositorGeneration,true,state.running()&&!state.terminal()&&lifecycle==Lifecycle::Focused);
    if(XR_FAILED(r)||state.terminal()||!changes.active()) {
      serviceFailed=true;geometry.invalidate(geometryGeneration);poses.invalidate(compositorGeneration);
      result("service_poll_terminal",r);return;
    }
    if(lifecycle==Lifecycle::Stopping) {
      r=boundary.clear();
      if(r==XR_SUCCESS)r=state.stop();
      serviceStopped=r==XR_SUCCESS;serviceFailed=!serviceStopped;
      geometry.invalidate(geometryGeneration);poses.invalidate(compositorGeneration);
      result("service_xrEndSession",r);
      return;
    }
    // Only the owner advances loading frames. Queued API work has priority,
    // and an open game frame is never replaced by background work. Loading
    // predictions are deliberately not published as GetLastPoses results.
    if(loading.work(state.frameOpen())!=LoadingWork::None&&state.running()&&!service.pending()) {
      r=loadingStep();
      if(r!=XR_SUCCESS){serviceFailed=true;result("loading_frame",r);}
    }
  }
  XrResult loadingStep() {
    if(GetCurrentThreadId()!=ownerThread||state.frameOpen())return XR_ERROR_CALL_ORDER_INVALID;
    const auto work=loading.work(false);
    if(work==LoadingWork::None)return XR_ERROR_CALL_ORDER_INVALID;
    auto r=boundary.waitAndBegin();
    if(r!=XR_SUCCESS&&r!=XR_FRAME_DISCARDED&&r!=XR_SESSION_LOSS_PENDING)return r;
    const auto frame=boundary.frame();
    if(state.terminal())return boundary.clear();
    if(work==LoadingWork::Empty) {
      r=boundary.clear();
      if(r==XR_SUCCESS){loading.emptyCompleted();++loadingClears;++loadingFrames;++loadingEmpty;}
      return r;
    }
    uint64_t applied=0;
    if(!changes.advance(frame.predictedDisplayTime,applied))return XR_ERROR_TIME_INVALID;
    if(applied){referenceChanges+=applied;if(!invalidateOrigin())return XR_ERROR_LIMIT_REACHED;}
    frameSpace=seated.space();
    GeometryInput located{};
    r=locateGeometry({api.locateViews,api.locateSpace},binding,frame,geometryGeneration,sizes,located,nullptr,frameSpace);
    if(r!=XR_SUCCESS){if(!XR_FAILED(r))boundary.clear();return r;}
    GeometrySnapshot snapshot{};
    const bool valid=makeGeometrySnapshot(located,snapshot);
    frameViews[0]=located.views[0];frameViews[1]=located.views[1];
    boundary.setGeometryReady(valid);
    r=boundary.background();
    if(r==XR_SUCCESS){++loadingFrames;if(valid&&frame.shouldRender)++loadingLayers;else ++loadingEmpty;}
    return r;
  }
  static void referenceEvent(const XrEventDataBuffer& buffer,void* context) noexcept {
    auto& host=*static_cast<Host*>(context);
    if(buffer.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
      const auto& event=*reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&buffer);
      if(!host.changes.note(event))std::puts("error,reference_change_policy");
    }
  }
  bool invalidateOrigin() {
    geometry.invalidate(geometryGeneration);frameGeometryAvailable=false;
    return poses.resetOrigin(compositorGeneration);
  }
  CompositorRead compositorRead() const override {return poses.read();}
  void compositorUnsupported(unsigned slot) noexcept override {std::printf("compositor_unavailable,slot=%u\n",slot);}
  vr::EVRCompositorError waitPoses(uint64_t generation,CompositorRead& out) override {
    if(!service.isOwner()) {
      auto result=vr::VRCompositorError_InvalidTexture;
      return service.invoke([&]{result=waitPoses(generation,out);})?result:vr::VRCompositorError_InvalidTexture;
    }
    if(GetCurrentThreadId()!=ownerThread)return vr::VRCompositorError_InvalidTexture;
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation||generation!=compositorGeneration||!poses.read().connected||!state.running()||state.terminal()||!seated.space()||!changes.active())
      return vr::VRCompositorError_InvalidTexture;
    ++compositorWaits;frameGeometryAvailable=false;frameGeometry={};
    auto fail=[&](XrResult error){lastCompositorResult=error;poses.invalidate(generation);return vr::VRCompositorError_InvalidTexture;};
    lastCompositorResult=boundary.waitAndBegin();
    if(lastCompositorResult!=XR_SUCCESS&&lastCompositorResult!=XR_FRAME_DISCARDED&&lastCompositorResult!=XR_SESSION_LOSS_PENDING)
      return fail(lastCompositorResult);
    loading.sceneWaited();
    const Frame frame=boundary.frame();
    if(state.terminal()){boundary.clear();return fail(XR_SESSION_LOSS_PENDING);}
    uint64_t applied=0;
    if(!changes.advance(frame.predictedDisplayTime,applied)){boundary.clear();return fail(XR_ERROR_TIME_INVALID);}
    if(applied){referenceChanges+=applied;if(!invalidateOrigin()){boundary.clear();return fail(XR_ERROR_LIMIT_REACHED);}}
    frameSpace=seated.space();
    GeometryInput located{};XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    auto r=locateGeometry({api.locateViews,api.locateSpace},binding,frame,geometryGeneration,sizes,located,&velocity,frameSpace);
    if(r!=XR_SUCCESS){if(!XR_FAILED(r))boundary.clear();return fail(r);}
    TimedHeadPose render{},game{};
    if(!makeHeadPose(located.headPose,located.headFlags,velocity.velocityFlags,velocity.linearVelocity,velocity.angularVelocity,true,render))
      {boundary.clear();return fail(XR_ERROR_POSE_INVALID);}
    render.time=frame.predictedDisplayTime;game.pose=invalidHeadPose(true);
    XrTime gameplayTime=0;
    if(nextPredictionTime(frame.predictedDisplayTime,frame.predictedDisplayPeriod,gameplayTime)&&
       !changes.crosses(frame.predictedDisplayTime,gameplayTime)) {
      r=locateHeadAt(api.locateSpace,view,frameSpace,gameplayTime,true,game);
      // A runtime may not locate an additional frame ahead. Leave that
      // independent gameplay prediction invalid rather than reusing render.
      if(r!=XR_SUCCESS&&r!=XR_ERROR_TIME_INVALID) {if(!XR_FAILED(r))boundary.clear();return fail(r);}
    }
    if(game.pose.bPoseIsValid)++validGamePoses;
    const bool geometryValid=geometry.publish(located,false,false);
    boundary.setGeometryReady(geometryValid);
    frameGeometry=located;frameGeometryAvailable=true;
    frameViews[0]=located.views[0];frameViews[1]=located.views[1];
    auto snapshot=poses.read();snapshot.sequence=frame.sequence;snapshot.renderTime=render.time;snapshot.gameTime=game.time;
    snapshot.renderPose=render.pose;snapshot.gamePose=game.pose;snapshot.posesAvailable=true;
    if(!poses.publish(snapshot)){boundary.clear();return fail(XR_ERROR_VALIDATION_FAILURE);}
    out=snapshot;lastCompositorResult=XR_SUCCESS;return vr::VRCompositorError_None;
  }
  bool setTrackingSpace(uint64_t generation,vr::ETrackingUniverseOrigin origin) override {
    if(!service.isOwner()) {bool result=false;return service.invoke([&]{result=setTrackingSpace(generation,origin);})&&result;}
    if(GetCurrentThreadId()!=ownerThread)return false;
    auto operation=gate.tryEnter(runtimeGeneration);
    // Standing/raw need their own supported runtime spaces and remain absent.
    return operation&&generation==compositorGeneration&&seated.space()&&origin==vr::TrackingUniverseSeated;
  }
  vr::EVRCompositorError submitEye(uint64_t generation,vr::EVREye eye,const vr::Texture_t* texture,
      const vr::VRTextureBounds_t* bounds,vr::EVRSubmitFlags flags) override {
    if(!service.isOwner()) {
      auto result=vr::VRCompositorError_InvalidTexture;
      return service.invoke([&]{result=submitEye(generation,eye,texture,bounds,flags);})?result:vr::VRCompositorError_InvalidTexture;
    }
    if(GetCurrentThreadId()!=ownerThread)return vr::VRCompositorError_InvalidTexture;
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation||generation!=compositorGeneration)return vr::VRCompositorError_InvalidTexture;
    const auto r=boundary.submit(eye,texture,bounds,flags);
    lastCompositorResult=boundary.lastResult();
    if(r==vr::VRCompositorError_None){++compositorSubmits;if(loading.sceneSubmitted())++loadingToScene;}
    return r;
  }
  bool clearSubmitted(uint64_t generation) override {
    if(!service.isOwner()) {bool result=false;return service.invoke([&]{result=clearSubmitted(generation);})&&result;}
    if(GetCurrentThreadId()!=ownerThread)return false;
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation||generation!=compositorGeneration)return false;
    if(boundary.clear()!=XR_SUCCESS)return false;
    loading.sceneCleared();
    return loading.visible(); // no default historical grid when no override exists
  }
  vr::EVRCompositorError setSkybox(uint64_t generation,const vr::Texture_t* textures,uint32_t count) override {
    if(!service.isOwner()) {
      auto result=vr::VRCompositorError_InvalidTexture;
      return service.invoke([&]{result=setSkybox(generation,textures,count);})?result:vr::VRCompositorError_InvalidTexture;
    }
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation||generation!=compositorGeneration||!poses.read().connected||!state.running()||state.terminal()||serviceStopped||serviceFailed)
      return vr::VRCompositorError_InvalidTexture;
    const auto result=skybox.set(textures,count);
    if(result==vr::VRCompositorError_None){++skyboxSets;loading.overrideSet();}
    return result;
  }
  bool clearSkybox(uint64_t generation) override {
    if(!service.isOwner()){bool result=false;return service.invoke([&]{result=clearSkybox(generation);})&&result;}
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation||generation!=compositorGeneration||!poses.read().connected)return false;
    skybox.clear();loading.overrideCleared();++skyboxClears;return true;
  }
  bool handoff(uint64_t generation) override {
    if(!service.isOwner()) {bool result=false;return service.invoke([&]{result=handoff(generation);})&&result;}
    if(GetCurrentThreadId()!=ownerThread)return false;
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation||generation!=compositorGeneration||state.frameOpen())return false;
    ++compositorHandoffs;return true; // the completed pair already ended its frame
  }
  XrResult drawDiagnosticEye(unsigned eye,ID3D11Texture2D*& out) {
    out=nullptr;
    if(GetCurrentThreadId()!=ownerThread||eye>=2)return XR_ERROR_CALL_ORDER_INVALID;
    auto operation=gate.tryEnter(runtimeGeneration);
    return operation?stereo.drawEye(eye,frameViews[eye],out):XR_ERROR_CALL_ORDER_INVALID;
  }
  XrResult closeDiagnosticFrame() {
    if(GetCurrentThreadId()!=ownerThread)return XR_ERROR_CALL_ORDER_INVALID;
    auto operation=gate.tryEnter(runtimeGeneration);
    return operation?boundary.clear():XR_ERROR_CALL_ORDER_INVALID;
  }
  vr::EVRCompositorError capture(vr::EVREye eye,const vr::Texture_t* texture,
      const vr::VRTextureBounds_t* bounds,vr::EVRSubmitFlags flags,bool copyPixels) override {
    const auto r=captured.capture(eye,texture,bounds,flags,copyPixels);
    if(r==vr::VRCompositorError_None && copyPixels)++copiedEyes;
    return r;
  }
  XrResult compose(XrCompositionLayerProjection& layer) override {
    const auto r=stereo.renderCaptured(frameViews,frameSpace,captured,layer);
    if(r==XR_SUCCESS)++composedPairs;
    return r;
  }
  XrResult composeBackground(XrCompositionLayerProjection& layer) override {
    return stereo.renderSkybox(frameViews,frameSpace,skybox,layer);
  }
  SystemRead read() const override {return geometry.read();}
  bool locateHead(uint64_t generation,vr::ETrackingUniverseOrigin origin,float prediction,vr::TrackedDevicePose_t& out) override {
    if(!service.isOwner()) {bool result=false;return service.invoke([&]{result=locateHead(generation,origin,prediction,out);})&&result;}
    // Cached reads need no operation lease. Handle-using calls fail promptly
    // while another operation is in flight or shutdown has been requested.
    if(GetCurrentThreadId()!=ownerThread)return false;
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation||generation!=geometryGeneration||!read().connected||
       !state.running()||state.terminal()||!seated.space()||origin!=vr::TrackingUniverseSeated)return false;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    lastHeadResult=HeadLocator{}.locate({api.convertTime,api.locateSpace},instance,view,seated.space(),prediction,head,&lastHeadTime,counterNow);
    if(lastHeadResult!=XR_SUCCESS)return false;
    vr::TrackedDevicePose_t pose{};pose.bDeviceIsConnected=true;
    pose.mDeviceToAbsoluteTracking.m[0][0]=pose.mDeviceToAbsoluteTracking.m[1][1]=pose.mDeviceToAbsoluteTracking.m[2][2]=1;
    constexpr auto valid=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    pose.bPoseIsValid=(head.locationFlags&valid)==valid;
    pose.eTrackingResult=pose.bPoseIsValid?vr::TrackingResult_Running_OK:vr::TrackingResult_Running_OutOfRange;
    if(pose.bPoseIsValid){double matrix[4][4];detail::rigid(head.pose,matrix);if(!detail::narrow(matrix,pose.mDeviceToAbsoluteTracking))return false;}
    out=pose;return true;
  }
  bool resetSeated(uint64_t generation) override {
    if(!service.isOwner()) {bool result=false;return service.invoke([&]{result=resetSeated(generation);})&&result;}
    if(GetCurrentThreadId()!=ownerThread)return false;
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation||generation!=geometryGeneration||!state.running()||state.terminal()||
       !seated.space()||!changes.active()||!resetEvents.room(generation))return false;
    // Finish a partial pair before replacing the space referenced by it.
    lastResetResult=boundary.clear();if(lastResetResult!=XR_SUCCESS)return false;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    lastResetResult=HeadLocator{}.locate({api.convertTime,api.locateSpace},instance,view,local,0,head,&lastResetTime,counterNow);
    if(lastResetResult!=XR_SUCCESS)return false;
    XrPosef origin{};
    if(!seatedOriginFromHead(head.pose,head.locationFlags,origin)){lastResetResult=XR_ERROR_POSE_INVALID;return false;}
    lastResetResult=seated.replace(origin);
    if(lastResetResult!=XR_SUCCESS){geometry.invalidate(generation);poses.invalidate(compositorGeneration);return false;}
    if(!invalidateOrigin()){lastResetResult=XR_ERROR_LIMIT_REACHED;return false;}
    // Verify and attach an event-time sample, not a later cached render pose.
    TimedHeadPose atReset{};
    lastResetResult=locateHeadAt(api.locateSpace,view,seated.space(),lastResetTime,true,atReset);
    if(lastResetResult!=XR_SUCCESS)return false;
    if(!atReset.pose.bPoseIsValid){lastResetResult=XR_ERROR_POSE_INVALID;return false;}
    const auto& matrix=atReset.pose.mDeviceToAbsoluteTracking.m;
    resetPositionError=std::sqrt(matrix[0][3]*matrix[0][3]+matrix[1][3]*matrix[1][3]+matrix[2][3]*matrix[2][3]);
    resetYawError=std::fabs(std::atan2(matrix[0][2],matrix[2][2]));
    if(!resetEvents.push(generation,poses.read().originGeneration,GetTickCount64(),atReset.pose)){
      lastResetResult=XR_ERROR_LIMIT_REACHED;return false;
    }
    ++recenters;return true;
  }
  bool pollEvent(uint64_t generation,vr::ETrackingUniverseOrigin origin,vr::VREvent_t& event,vr::TrackedDevicePose_t& pose) override {
    if(!service.isOwner()) {bool result=false;return service.invoke([&]{result=pollEvent(generation,origin,event,pose);})&&result;}
    if(generation!=geometryGeneration)return false;
    const bool found=resetEvents.pop(generation,poses.read().originGeneration,origin,GetTickCount64(),event,pose);
    if(found)++resetPolls;return found;
  }
  void unsupported(unsigned slot) noexcept override {std::printf("system_unavailable,slot=%u\n",slot);}
  ~Host() {close();}
  bool close() {
    if(runtimeGeneration) {
      gate.requestStop(runtimeGeneration);
      // The diagnostic joins all operations before destruction. A future
      // multi-thread host must defer its own lifetime too until this succeeds.
      if(!gate.canDestroy(runtimeGeneration))return false;
    }
    geometry.retire(geometryGeneration);
    poses.retire(compositorGeneration);
    resetEvents.retire(geometryGeneration);changes.clear();
    loading={};skybox.shutdown();captured.shutdown();
    clean=result("destroy_swapchains",stereo.shutdown())&&clean;
    clean=result("destroy_seated_space",seated.shutdown())&&clean;
    // xrDestroySession is allowed in any state once handle users are excluded.
    // Normal exit ends a STOPPING session in the loop; failed/cancelled startup
    // may destroy directly, without an invalid xrEndSession in another state.
    clean=result("destroy_binding",binding.shutdown())&&clean;
    view=local=XR_NULL_HANDLE;session=XR_NULL_HANDLE;
    state.abandonAfterOwnerDestruction();
    graphics.reset();
    if(instance && api.destroyInstance) {clean=result("xrDestroyInstance",api.destroyInstance(instance))&&clean;instance=XR_NULL_HANDLE;}
    if(api.module) {FreeLibrary(api.module);api.module=nullptr;}
    if(runtimeGeneration) {
      clean=gate.finishGeneration(runtimeGeneration)&&clean;
      runtimeGeneration=0;
    }
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
    if(FAILED(captured.initialize(graphics.device())))return result("capture_initialize",XR_ERROR_GRAPHICS_DEVICE_INVALID);
    if(FAILED(skybox.initialize(graphics.device())))return result("skybox_initialize",XR_ERROR_GRAPHICS_DEVICE_INVALID);
    std::printf("swapchain_format,%lld\n",(long long)stereo.format());
    if(!result("policy_initialize",state.reset(api.frames,instance,session,XR_ENVIRONMENT_BLEND_MODE_OPAQUE)))return false;
    if(!seated.begin({api.createSpace,api.destroySpace},session,local)||!changes.begin(session)||!resetEvents.begin(geometryGeneration))return false;
    state.setUnhandledEventSink(referenceEvent,this);
    runtimeGeneration=gate.beginGeneration();
    compositorGeneration=poses.begin();
    return runtimeGeneration!=0&&compositorGeneration!=0;
  }
};
// The facade objects survive explicit Shutdown. Their resources and all
// thread-bound members are constructed/cleaned on the service thread.
class NativeBackend final:public RuntimeBackend {
 public:
  OwnerService owner;
  std::unique_ptr<Host> host;
  Options options;
  vr::EVRInitError start(uint32_t token,const std::atomic<bool>& cancelled,RuntimeInterfaces& out)override {
    if(host)return vr::VRInitError_Init_Internal; // native diagnostic: one generation per process
    if(!owner.start([this]{if(host)host->pumpEvents();}))return vr::VRInitError_Init_Internal;
    auto error=vr::VRInitError_Init_Internal;
    if(!owner.invoke([&]{
      host=std::make_unique<Host>(owner);host->startupOptions=options;
      error=host->start(token,cancelled,out);
    }))return vr::VRInitError_Init_Internal;
    return error;
  }
  bool stop()noexcept override {
    bool cleaned=!host;
    const bool joined=owner.stop([&]{cleaned=host?host->stop():true;});
    return joined&&cleaned;
  }
};
struct StopServiceOnExit {
  OwnerService& service;
  ~StopServiceOnExit(){service.stop();}
};
void printPose(const char* name,const XrPosef& p) {
  std::printf("%s,position=%.9g/%.9g/%.9g,orientation=%.9g/%.9g/%.9g/%.9g\n",name,
    p.position.x,p.position.y,p.position.z,p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w);
}
bool samePose(const vr::TrackedDevicePose_t& a,const vr::TrackedDevicePose_t& b) {
  return std::memcmp(&a.mDeviceToAbsoluteTracking,&b.mDeviceToAbsoluteTracking,sizeof(a.mDeviceToAbsoluteTracking))==0&&
    std::memcmp(&a.vVelocity,&b.vVelocity,sizeof(a.vVelocity))==0&&std::memcmp(&a.vAngularVelocity,&b.vAngularVelocity,sizeof(a.vAngularVelocity))==0&&
    a.eTrackingResult==b.eTrackingResult&&a.bPoseIsValid==b.bPoseIsValid&&a.bDeviceIsConnected==b.bDeviceIsConnected;
}
int run(const Options& options) {
  // Process-lifetime objects back the exported function pointers. Their
  // resources are still explicitly retired through Shutdown before return.
  static NativeBackend backend;backend.options=options;
  static RuntimeLifecycle runtime(backend);
  if(!bindRuntimeExports(runtime))return 3;
  vr::EVRInitError initError=vr::VRInitError_Unknown;
  const uint32_t initToken=edvr_native_VR_InitInternal(&initError,vr::VRApplication_Scene);
  if(!initToken||initError!=vr::VRInitError_None){std::printf("error,runtime_init,%d\n",int(initError));return 3;}
  struct ShutdownOnExit {~ShutdownOnExit(){edvr_native_VR_ShutdownInternal();}} shutdownOnExit;
  Host& host=*backend.host;
  const DWORD initThread=GetCurrentThreadId();
  auto* system=static_cast<vr::IVRSystem*>(edvr_native_VR_GetGenericInterface(vr::IVRSystem_Version,&initError));
  auto* display=static_cast<vr::IVRExtendedDisplay*>(edvr_native_VR_GetGenericInterface(vr::IVRExtendedDisplay_Version,&initError));
  if(!system||!display||initError!=vr::VRInitError_None)return 3;
  system=nativeSystemCaller(system);
  uint32_t earlyWidth=0,earlyHeight=0;system->GetRecommendedRenderTargetSize(&earlyWidth,&earlyHeight);
  int32_t windowX=0,windowY=0;uint32_t windowWidth=0,windowHeight=0;
  display->GetWindowBounds(&windowX,&windowY,&windowWidth,&windowHeight);
  OwnerService systemClient;
  std::atomic<bool> liveQueries{false};
  DWORD systemThread=0;uint64_t systemQueries=0,validSystemQueries=0;
  if(!systemClient.start([&]{
    if(!liveQueries.load(std::memory_order_acquire))return;
    vr::TrackedDevicePose_t pose{};
    system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated,0,&pose,1);
    ++systemQueries;if(pose.bPoseIsValid)++validSystemQueries;
  }))return 3;
  StopServiceOnExit stopSystemClient{systemClient};
  bool workerRead=false;
  if(!systemClient.invoke([&]{systemThread=GetCurrentThreadId();uint32_t width=0,height=0;system->GetRecommendedRenderTargetSize(&width,&height);
    const auto matrix=system->GetProjectionMatrix(vr::Eye_Left,.025f,50000,vr::API_DirectX);
    workerRead=width==earlyWidth&&height==earlyHeight&&width&&height&&matrix.m[3][2]==-1;
  }))return 3;
  bool untouched=false;
  if(!backend.owner.invoke([&]{untouched=host.compositorSubmits==0&&host.starts==1;}))return 3;
  if(!earlyWidth||!earlyHeight||!windowWidth||!windowHeight||!workerRead||!untouched)return 3;
  auto* compositor=static_cast<vr::IVRCompositor*>(edvr_native_VR_GetGenericInterface(vr::IVRCompositor_Version,&initError));
  auto* chaperone=static_cast<vr::IVRChaperone*>(edvr_native_VR_GetGenericInterface(vr::IVRChaperone_Version,&initError));
  if(!compositor||!chaperone||initError!=vr::VRInitError_None)return 3;
  compositor=nativeCompositorCaller(compositor);
  if(edvr_native_VR_InitInternal(nullptr,vr::VRApplication_Scene)!=initToken||
     edvr_native_VR_GetInitToken()!=initToken||edvr_native_VR_GetGenericInterface(vr::IVRCompositor_Version,nullptr)!=compositor||
     edvr_native_VR_IsInterfaceVersionValid("IVROverlay_011")||edvr_native_VR_GetGenericInterface("IVROverlay_011",&initError)||
     initError!=vr::VRInitError_Init_InterfaceNotFound)return 3;
  for(const char* version:{vr::IVRSystem_Version,vr::IVRExtendedDisplay_Version,vr::IVRCompositor_Version,vr::IVRChaperone_Version})
    if(!edvr_native_VR_IsInterfaceVersionValid(version))return 3;
  std::printf("runtime_exports,token=%u,interfaces=4,repeated_init=1,stable_identity=1,overlay_rejected=1,pre_compositor_geometry=1,cross_thread_cached_geometry=1,virtual_window=%ux%u\n",
    initToken,windowWidth,windowHeight);
  compositor->SetTrackingSpace(vr::TrackingUniverseSeated);
  if(compositor->GetTrackingSpace()!=vr::TrackingUniverseSeated)return 3;
  bool resetsPassed=true;
  if(!systemClient.invoke([&]{
    for(unsigned reset=0;reset<2;++reset) {
      system->ResetSeatedZeroPose();
      vr::VREvent_t event{};vr::TrackedDevicePose_t atEvent{},stale{};
      const auto cache=compositor->GetLastPoses(&stale,1,nullptr,0);
      const bool eventValid=system->PollNextEventWithPose(vr::TrackingUniverseSeated,&event,sizeof(event),&atEvent)&&
        event.eventType==vr::VREvent_SeatedZeroPoseReset&&!event.data.seatedZeroPoseReset.bResetBySystemMenu&&
        atEvent.bPoseIsValid&&!system->PollNextEvent(&event,sizeof(event));
      bool stateValid=false;
      if(!backend.owner.invoke([&]{
        stateValid=host.recenters==reset+1&&host.lastResetResult==XR_SUCCESS&&!host.read().geometryValid&&
          cache!=vr::VRCompositorError_None&&!stale.bPoseIsValid&&host.resetPositionError<=.01f&&host.resetYawError<=.01f;
        std::printf("seated_reset,count=%llu,time=%lld,origin_generation=%llu,position_error=%g,yaw_error=%g,event=804,cache_invalidated=%u\n",
          (unsigned long long)host.recenters,(long long)host.lastResetTime,(unsigned long long)host.compositorRead().originGeneration,
          host.resetPositionError,host.resetYawError,unsigned(stateValid));
      })||!eventValid||!stateValid){resetsPassed=false;break;}
    }
  })||!resetsPassed)return 3;
  // No render commands during this interval: the persistent owner must still
  // poll lifecycle events, without advancing the game's frame sequence.
  const auto beforePumps=host.publishedPumps.load(std::memory_order_acquire);
  const auto pumpDeadline=GetTickCount64()+1000;
  while(host.publishedPumps.load(std::memory_order_acquire)<beforePumps+2&&GetTickCount64()<pumpDeadline)Sleep(2);
  if(host.publishedPumps.load(std::memory_order_acquire)<beforePumps+2)return 3;
  if(initThread==systemThread||initThread==host.ownerThread||systemThread==host.ownerThread)return 3;
  std::printf("runtime_threads,init=%lu,system=%lu,render_owner=%lu,idle_pump=1,system_resets=2\n",
    (unsigned long)initThread,(unsigned long)systemThread,(unsigned long)host.ownerThread);
  liveQueries.store(true,std::memory_order_release);
  const ULONGLONG startup=GetTickCount64(); ULONGLONG started=startup,exitRequested=0;
  // A copied six-face loading scene runs with no application Wait/Submit.
  // Destroy and overwrite the caller sources before the timed viewing phase.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> skySources[6];vr::Texture_t skyTextures[6]{};
  bool skyCreated=true;
  if(!backend.owner.invoke([&]{
    constexpr unsigned side=256;
    const unsigned colors[6][3]={{35,70,110},{80,35,90},{35,90,65},{95,60,30},{50,65,100},{65,45,35}};
    std::vector<uint32_t> pixels(side*side);
    for(unsigned face=0;face<6;++face) {
      for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
        const bool line=x%32<2||y%32<2;
        const bool mark=x>112&&x<144&&y>64&&y<96;
        unsigned rgb[3]{};for(unsigned c=0;c<3;++c)rgb[c]=mark?210:line?130:colors[face][c];
        pixels[y*side+x]=0xff000000u|rgb[0]|(rgb[1]<<8)|(rgb[2]<<16);
      }
      D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=side;desc.MipLevels=desc.ArraySize=1;
      desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
      desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
      const D3D11_SUBRESOURCE_DATA data{pixels.data(),side*4,0};
      if(FAILED(host.graphics.device()->CreateTexture2D(&desc,&data,&skySources[face]))){skyCreated=false;break;}
      skyTextures[face]={skySources[face].Get(),vr::API_DirectX,vr::ColorSpace_Auto};
    }
  })||!skyCreated||compositor->SetSkyboxOverride(skyTextures,6)!=vr::VRCompositorError_None)return 3;
  if(!backend.owner.invoke([&]{
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;host.graphics.device()->GetImmediateContext(&context);
    std::vector<uint32_t> black(256*256,0xff000000u);
    for(auto& source:skySources){context->UpdateSubresource(source.Get(),0,nullptr,black.data(),256*4,0);source.Reset();}
  }))return 3;
  compositor->ClearLastSubmittedFrame();
  const auto cacheBeforeLoading=host.compositorRead();
  const auto loadingDeadline=startup+(std::min)(3000ULL,options.seconds*250ULL);
  std::puts("loading_phase,begin=1,six_faces=1,sources_overwritten_and_released=1");
  while(GetTickCount64()<loadingDeadline)Sleep(10);
  bool loadingGate=false;
  if(!backend.owner.invoke([&]{
    const auto after=host.compositorRead();
    loadingGate=!host.serviceFailed&&!host.serviceStopped&&host.loadingLayers>=2&&host.compositorSubmits==0&&
      host.compositorWaits==host.startupFrames&&after.sequence==cacheBeforeLoading.sequence&&
      after.renderTime==cacheBeforeLoading.renderTime&&after.gameTime==cacheBeforeLoading.gameTime&&
      after.posesAvailable==cacheBeforeLoading.posesAvailable;
    std::printf("loading_phase,completed_frames=%llu,projection_frames=%llu,game_waits=%llu,game_submits=%llu,cache_unchanged=%u\n",
      (unsigned long long)host.loadingFrames,(unsigned long long)host.loadingLayers,
      (unsigned long long)host.compositorWaits,(unsigned long long)host.compositorSubmits,unsigned(loadingGate));
  })||!loadingGate)return 3;
  uint64_t frames=0,layers=0,empty=0,valid=0,invalid=0,headValid=0,cachedChecks=0;
  bool stopped=false, failed=false, bootstrapComplete=false; Lifecycle previous=Lifecycle::Uninitialized;
  auto renderStep=[&] { do {
    if(host.serviceStopped){stopped=true;break;}
    if(host.serviceFailed){failed=true;break;}
    auto operation=host.gate.tryEnter(host.runtimeGeneration);
    if(!operation){std::puts("error,runtime_operation_unavailable");failed=true;break;}
    XrResult r=host.state.lastResult();
    const auto lifecycle=host.state.lifecycle();
    if(lifecycle!=previous) {std::printf("lifecycle,%d\n",int(lifecycle));previous=lifecycle;}
    if(XR_FAILED(r)||host.state.terminal()||!host.changes.active()) {result("poll_terminal",r);failed=true;break;}
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
    operation=RuntimeGate::Lease{};
    vr::TrackedDevicePose_t renderPoses[4]{},gamePoses[2]{};
    const auto waited=compositor->WaitGetPoses(renderPoses,4,gamePoses,2);
    if(waited!=vr::VRCompositorError_None){std::printf("compositor_wait_failed,vr=%d,xr=%d\n",int(waited),int(host.lastCompositorResult));failed=true;break;}
    const Frame frame=host.boundary.frame();
    vr::TrackedDevicePose_t cachedRender[4]{},cachedGame[2]{},singleRender{},singleGame{};
    const auto waitsBefore=host.compositorWaits;
    if(compositor->GetLastPoses(cachedRender,4,cachedGame,2)!=vr::VRCompositorError_None||
       compositor->GetLastPoseForTrackedDeviceIndex(0,&singleRender,&singleGame)!=vr::VRCompositorError_None||
       host.compositorWaits!=waitsBefore||!samePose(renderPoses[0],cachedRender[0])||!samePose(gamePoses[0],cachedGame[0])||
       !samePose(singleRender,renderPoses[0])||!samePose(singleGame,gamePoses[0])||
       renderPoses[1].bDeviceIsConnected||renderPoses[2].bPoseIsValid||renderPoses[3].bDeviceIsConnected||gamePoses[1].bPoseIsValid)
      {std::puts("error,compositor_pose_cache");failed=true;host.closeDiagnosticFrame();break;}
    ++cachedChecks;
    ++frames; bool submittedLayer=false;
    bool abortAfterEnd=host.state.terminal();
    GeometryInput located=host.frameGeometry;bool haveLocation=host.frameGeometryAvailable;
    if(!exitRequested && !abortAfterEnd) {
      GeometrySnapshot candidate{};
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
        // Alternate Submit order each frame; capture pixels before drawing
        // the other eye, then compose the private pair at the second Submit.
        for(unsigned n=0;n<2;++n) {
          const unsigned eye=n ^ unsigned(frames&1);
          ID3D11Texture2D* source=nullptr;
          r=host.drawDiagnosticEye(eye,source);
          if(r!=XR_SUCCESS){result("draw_eye",r);failed=true;break;}
          const vr::Texture_t texture{source,vr::API_DirectX,vr::ColorSpace_Gamma};
          const auto submitted=compositor->Submit(vr::EVREye(eye),&texture);
          if(submitted!=vr::VRCompositorError_None) {
            std::printf("error,submit_eye,%u,%d,xr=%d\n",eye,int(submitted),int(host.boundary.lastResult()));failed=true;break;
          }
        }
        if(failed) {
          // A rejected texture or local offscreen allocation failure still
          // leaves a valid session's frame to close. The boundary itself
          // refuses further dispatch after an uncertain external XR failure.
          // Device loss instead requires owner destruction without more work.
          if(SUCCEEDED(host.graphics.device()->GetDeviceRemovedReason()))
            result("close_failed_local_frame",host.closeDiagnosticFrame());
          break;
        }
        submittedLayer=true;
        compositor->PostPresentHandoff();
        if(!layers) {
          if(host.loading.visible()||host.loadingToScene!=1){std::puts("error,loading_to_scene");failed=true;break;}
          compositor->ClearSkyboxOverride();
          if(host.skybox.ready()||host.skyboxClears!=1){std::puts("error,skybox_clear");failed=true;break;}
        }
      }
    }
    r=host.closeDiagnosticFrame();
    if(!result("xrEndFrame",r)) {failed=true;break;}
    if(submittedLayer) ++layers; else ++empty;
    if(abortAfterEnd) {std::puts("error,pending_or_external_frame_result");failed=true;break;}
    if(haveLocation) {
      const bool published=host.read().geometryValid;
      if(published&&!bootstrapComplete) {
        const auto snapshot=host.read();
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
        const auto poseFrame=host.compositorRead();
        std::printf("compositor_abi,version=IVRCompositor_014,render_time=%lld,game_time=%lld,period=%lld,render_valid=%u,game_valid=%u,cached_equal=1\n",
          (long long)poseFrame.renderTime,(long long)poseFrame.gameTime,(long long)frame.predictedDisplayPeriod,
          unsigned(renderPoses[0].bPoseIsValid),unsigned(gamePoses[0].bPoseIsValid));
      }
    }
  } while(false); };
  while(!failed&&!stopped) {
    if(!backend.owner.invoke(renderStep)){failed=true;break;}
  }
  // Stop the System producer, then issue exported Shutdown from that same
  // caller thread. The runtime owner cancels queued work, cleans and joins.
  DWORD shutdownThread=0;
  const bool systemJoined=systemClient.stop([&]{shutdownThread=GetCurrentThreadId();edvr_native_VR_ShutdownInternal();});
  const bool cleanup=systemJoined&&shutdownThread==systemThread&&!backend.owner.running()&&host.clean&&!runtime.running()&&host.runtimeGeneration==0&&host.stops==1;
  std::printf("runtime_service,event_pumps=%llu,system_queries=%llu,valid_system_queries=%llu,shutdown_on_system=%u,owner_joined=%u\n",
    (unsigned long long)host.eventPumps,(unsigned long long)systemQueries,(unsigned long long)validSystemQueries,
    unsigned(shutdownThread==systemThread),unsigned(systemJoined&&!backend.owner.running()));
  if(edvr_native_VR_GetInitToken()==initToken||edvr_native_VR_GetGenericInterface(vr::IVRSystem_Version,&initError)||
     initError!=vr::VRInitError_Init_NotInitialized)failed=true;
  std::printf("runtime_shutdown,token=%u,starts=%llu,stops=%llu,interfaces_retired=%u,startup_frames=%llu\n",
    edvr_native_VR_GetInitToken(),(unsigned long long)host.starts,(unsigned long long)host.stops,unsigned(!runtime.running()),(unsigned long long)host.startupFrames);
  std::printf("origin_boundary,resets=%llu,reset_events=%llu,reference_changes=%llu\n",
    (unsigned long long)host.recenters,(unsigned long long)host.resetPolls,(unsigned long long)host.referenceChanges);
  std::printf("compositor_boundary,waits=%llu,submits=%llu,handoffs=%llu,cached_checks=%llu,valid_game_poses=%llu\n",
    (unsigned long long)host.compositorWaits,(unsigned long long)host.compositorSubmits,(unsigned long long)host.compositorHandoffs,
    (unsigned long long)cachedChecks,(unsigned long long)host.validGamePoses);
  std::printf("frame_boundary,copied_eyes=%llu,composed_pairs=%llu,alternate_order=1,lifetime_gate=1\n",
    (unsigned long long)host.copiedEyes,(unsigned long long)host.composedPairs);
  std::printf("skybox_boundary,sets=%llu,clears=%llu,loading_frames=%llu,loading_layers=%llu,loading_empty=%llu,clear_frames=%llu,to_scene=%llu,private_textures_retired=%u\n",
    (unsigned long long)host.skyboxSets,(unsigned long long)host.skyboxClears,(unsigned long long)host.loadingFrames,
    (unsigned long long)host.loadingLayers,(unsigned long long)host.loadingEmpty,(unsigned long long)host.loadingClears,(unsigned long long)host.loadingToScene,unsigned(!host.skybox.ready()));
  std::printf("summary,frames=%llu,stereo=%llu,empty=%llu,valid_views=%llu,invalid_views=%llu,valid_head=%llu,normal_stop=%u,cleanup=%u\n",
    (unsigned long long)frames,(unsigned long long)layers,(unsigned long long)empty,(unsigned long long)valid,(unsigned long long)invalid,
    (unsigned long long)headValid,unsigned(stopped),unsigned(cleanup));
  const bool passed=!failed && stopped && cleanup && layers && headValid && bootstrapComplete &&host.recenters==2&&host.resetPolls==2&&
    systemQueries&&validSystemQueries&&host.eventPumps>beforePumps&&loadingGate&&host.skyboxSets==1&&host.skyboxClears==1&&
    host.loadingToScene==1&&host.loadingLayers>=2&&host.loadingFrames==host.loadingLayers+host.loadingEmpty&&!host.skybox.ready()&&
    host.copiedEyes==layers*2 && host.composedPairs==layers && host.compositorSubmits==layers*2 &&
    host.compositorWaits==frames+host.startupFrames&&cachedChecks==frames&&host.compositorHandoffs==layers&&host.validGamePoses;
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
  // Exercise the real native adapter's worker construction and partial-start
  // cleanup without ever opening a loader or contacting an installed runtime.
  NativeBackend backend;std::atomic<bool> cancelled{true};RuntimeInterfaces interfaces{};
  const auto callerThread=GetCurrentThreadId();
  check(backend.start(7,cancelled,interfaces)==vr::VRInitError_Init_ShuttingDown&&!interfaces.complete(),"cancelled native startup stays unpublished");
  bool owned=false,unopened=false;
  check(backend.owner.invoke([&]{
    owned=backend.host&&backend.host->ownerThread==GetCurrentThreadId()&&GetCurrentThreadId()!=callerThread;
    unopened=backend.host&&!backend.host->api.module&&!backend.host->runtimeGeneration&&backend.host->starts==1;
  })&&owned&&unopened,"native members constructed on owner before cancelled open");
  check(backend.stop(),"cancelled native startup cleanup and join");
  check(!backend.owner.running()&&backend.host&&backend.host->clean&&backend.host->stops==1&&
    !backend.host->api.module&&!backend.host->runtimeGeneration,"native cleanup finished once before returning");
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
