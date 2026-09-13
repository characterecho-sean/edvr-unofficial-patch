#pragma once
// Shared native runtime host. Device ownership, graphics admission and owner
// lifetime are supplied by the embedding host; this does not select a backend.
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <memory>
#include "native_device.h"
#include "render_route.h"
#include "shutdown_trace.h"
#include "session_state.h"
#include "d3d11_stereo.h"
#include "projection_math.h"
#include "geometry_locator.h"
#include "openvr_system.h"
#include "system_publication.h"
#include "head_locator.h"
#include "runtime_gate.h"
#include "frame_boundary.h"
#include "eye_capture.h"
#include "skybox_capture.h"
#include "loading_state.h"
#include "openvr_compositor.h"
#include "compositor_publication.h"
#include "space_pose.h"
#include "seated_origin.h"
#include "seated_space.h"
#include "reference_changes.h"
#include "reset_events.h"
#include "runtime_exports.h"
#include "openvr_auxiliary.h"
#include "owner_service.h"
#include "render_thread_dispatcher.h"
#include "present_work_queue.h"

namespace edvr::openxr {
struct RuntimeOptions {
  std::wstring loader, graphicsProxy;
  // Borrowed validated provider from NativeRenderBinding. Its device and
  // module references remain alive through owner shutdown and callback release.
  HMODULE graphicsProvider = nullptr;
};
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
inline bool validSize(const XrViewConfigurationView& v) {
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
inline bool result(const char* operation,XrResult r) {
  if(r==XR_SUCCESS) return true;
  std::printf("result,%s,%d\n",operation,int(r)); return false;
}
template<class T> bool load(Api& a,XrInstance instance,const char* name,T& destination) {
  PFN_xrVoidFunction fn=nullptr; const XrResult r=a.get(instance,name,&fn);
  if(!result(name,r)) return false;
  if(!fn) return result(name,XR_ERROR_FUNCTION_UNSUPPORTED);
  destination=reinterpret_cast<T>(fn); return true;
}
inline bool counterNow(LARGE_INTEGER* value) { return QueryPerformanceCounter(value)!=FALSE; }
class NativeRuntimeHost : public SystemSource, public FrameSink, public CompositorSource, public AuxiliarySource, public RuntimeBackend {
 public:
  NativeRuntimeHost(OwnerService& owner,RenderThreadDispatcher& dispatcher,RenderRoute& route,ID3D11Device* supplied=nullptr)
      :service(owner),renderRoute(route),graphicsCalls(dispatcher),externalDevice(supplied){}
  OwnerService& service;
  RenderRoute& renderRoute;
  CountedGraphics graphicsCalls;
  ID3D11Device* externalDevice=nullptr; // host keeps device alive through owner join
  // The hook-owning proxy remains loaded until process exit, as in the WARP
  // fixture. Unloading a graphics proxy with retained hook contexts is unsafe.
  HMODULE graphicsProxy=nullptr;
  using BridgeCounts=void (*)(uint64_t*,uint64_t*);
  BridgeCounts bridgeCounts=nullptr;
  RuntimeOptions startupOptions;
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
    // Idle service work is CPU/XR only. A loading projection needs an explicit
    // render-caller boundary; it cannot borrow the game context between calls.
  }
  void loadingBoundary() {
    if(GetCurrentThreadId()!=ownerThread||serviceStopped||serviceFailed)return;
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation){serviceFailed=true;return;}
    if(loading.work(state.frameOpen())==LoadingWork::None||!state.running())return;
    const auto r=loadingStep();
    if(r!=XR_SUCCESS){serviceFailed=true;result("loading_frame",r);}
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
    auto& host=*static_cast<NativeRuntimeHost*>(context);
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
      return renderRoute.invoke([&]{result=submitEye(generation,eye,texture,bounds,flags);})?result:vr::VRCompositorError_InvalidTexture;
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
      return renderRoute.invoke([&]{result=setSkybox(generation,textures,count);})?result:vr::VRCompositorError_InvalidTexture;
    }
    auto operation=gate.tryEnter(runtimeGeneration);
    if(!operation||generation!=compositorGeneration||!poses.read().connected||!state.running()||state.terminal()||serviceStopped||serviceFailed)
      return vr::VRCompositorError_InvalidTexture;
    auto result=vr::VRCompositorError_InvalidTexture;
    if(!graphicsCalls.invoke([&]{result=skybox.set(textures,count);}))return vr::VRCompositorError_InvalidTexture;
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
    auto r=vr::VRCompositorError_InvalidTexture;
    if(!graphicsCalls.invoke([&]{r=captured.capture(eye,texture,bounds,flags,copyPixels);}))return r;
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
  ~NativeRuntimeHost() {close();}
  bool close() {
    const bool tracing=runtimeGeneration||session||instance||stereo.needsGpuDrain();
    ShutdownTrace gateStage("host_gate",tracing);
    if(runtimeGeneration) {
      gate.requestStop(runtimeGeneration);
      // The diagnostic joins all operations before destruction. A future
      // multi-thread host must defer its own lifetime too until this succeeds.
      if(!gate.canDestroy(runtimeGeneration)) { gateStage.end(false); return false; }
    }
    gateStage.end(true);
    geometry.retire(geometryGeneration);
    poses.retire(compositorGeneration);
    resetEvents.retire(geometryGeneration);changes.clear();
    loading={};
    ShutdownTrace captureStage("host_capture_release",tracing);
    skybox.shutdown();captured.shutdown();captureStage.end(true);
    ShutdownTrace stereoStage("host_stereo_shutdown",tracing);
    const auto stereoResult=stereo.shutdown();
    clean=result("destroy_swapchains",stereoResult)&&clean;stereoStage.end(stereoResult==XR_SUCCESS);
    if(stereo.needsGpuDrain())return clean=false; // do not invalidate its session/images
    ShutdownTrace seatedStage("host_seated_shutdown",tracing);
    const auto seatedResult=seated.shutdown();
    clean=result("destroy_seated_space",seatedResult)&&clean;seatedStage.end(seatedResult==XR_SUCCESS);
    // xrDestroySession is allowed in any state once handle users are excluded.
    // Normal exit ends a STOPPING session in the loop; failed/cancelled startup
    // may destroy directly, without an invalid xrEndSession in another state.
    ShutdownTrace bindingStage("host_binding_shutdown",tracing);
    const auto bindingResult=binding.shutdown();
    clean=result("destroy_binding",bindingResult)&&clean;bindingStage.end(bindingResult==XR_SUCCESS);
    view=local=XR_NULL_HANDLE;session=XR_NULL_HANDLE;
    state.abandonAfterOwnerDestruction();
    ShutdownTrace graphicsStage("host_graphics_reset",tracing);
    graphics.reset();externalDevice=nullptr;graphicsStage.end(true);
    ShutdownTrace instanceStage("host_instance_destroy",tracing);
    XrResult instanceResult=XR_SUCCESS;
    if(instance && api.destroyInstance) {
      instanceResult=api.destroyInstance(instance);
      clean=result("xrDestroyInstance",instanceResult)&&clean;instance=XR_NULL_HANDLE;
    }
    instanceStage.end(instanceResult==XR_SUCCESS&&!instance);
    ShutdownTrace loaderStage("host_loader_free",tracing);
    BOOL loaderFreed=TRUE;
    if(api.module) {loaderFreed=FreeLibrary(api.module);api.module=nullptr;}
    loaderStage.end(loaderFreed!=FALSE);
    if(runtimeGeneration) {
      ShutdownTrace finishStage("host_gate_finish",tracing);
      const bool finished=gate.finishGeneration(runtimeGeneration);
      clean=finished&&clean;finishStage.end(finished);
      runtimeGeneration=0;
    }
    return clean;
  }
  bool open(const RuntimeOptions& options) {
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
    decltype(&D3D11CreateDevice) createDevice=&D3D11CreateDevice;
    if(options.graphicsProvider || !options.graphicsProxy.empty()) {
      if(options.graphicsProvider && !externalDevice)
        return result("paired_graphics_device_missing",XR_ERROR_GRAPHICS_DEVICE_INVALID);
      graphicsProxy=options.graphicsProvider ? options.graphicsProvider :
        LoadLibraryExW(options.graphicsProxy.c_str(),nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if(!graphicsProxy){std::printf("error,graphics_proxy_load,%lu\n",GetLastError());return false;}
      createDevice=reinterpret_cast<decltype(createDevice)>(GetProcAddress(graphicsProxy,"D3D11CreateDevice"));
      bridgeCounts=reinterpret_cast<BridgeCounts>(GetProcAddress(graphicsProxy,"edvr_selftest_graphics_bridge"));
      if(!createDevice||!bridgeCounts||!GetProcAddress(graphicsProxy,"edvrAcquireGraphicsBridge"))
        return result("graphics_proxy_capability",XR_ERROR_FUNCTION_UNSUPPORTED);
    }
    HRESULT hr=E_FAIL;
    if(!graphicsCalls.invoke([&]{hr=externalDevice?
        graphics.initializeExisting(externalDevice,req.adapterLuid,req.minFeatureLevel):
        graphics.initialize(req.adapterLuid,req.minFeatureLevel,createDevice);}))
      return result("device_render_boundary",XR_ERROR_INITIALIZATION_FAILED);
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
    if(!result("stereo_initialize",stereo.initialize(api.stereo,session,graphics.device(),sizes,graphicsProxy,&graphicsCalls))) return false;
    bool capturesReady=false;
    if(!graphicsCalls.invoke([&]{capturesReady=SUCCEEDED(captured.initialize(graphics.device()))&&SUCCEEDED(skybox.initialize(graphics.device()));})||!capturesReady)
      return result("capture_initialize",XR_ERROR_GRAPHICS_DEVICE_INVALID);
    std::printf("swapchain_format,%lld\n",(long long)stereo.format());
    if(!result("policy_initialize",state.reset(api.frames,instance,session,XR_ENVIRONMENT_BLEND_MODE_OPAQUE)))return false;
    if(!seated.begin({api.createSpace,api.destroySpace},session,local)||!changes.begin(session)||!resetEvents.begin(geometryGeneration))return false;
    state.setUnhandledEventSink(referenceEvent,this);
    runtimeGeneration=gate.beginGeneration();
    compositorGeneration=poses.begin();
    return runtimeGeneration!=0&&compositorGeneration!=0;
  }
};
} // namespace edvr::openxr
