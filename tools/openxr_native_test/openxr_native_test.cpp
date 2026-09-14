// Standalone diagnostic only. The game backend must bind Elite's own device.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/native_runtime_host.h"
#include "present_device.h"
#include "../../src/openxr/render_shutdown.h"
#include "../../src/openxr/native_render_binding.h"
#include "launch_centre_cases.h"
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
struct Options : RuntimeOptions { unsigned seconds=10; bool dry=false, self=false, presentBoundary=false; };
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
  Options o; bool seenLoader=false, seenSeconds=false, seenGraphics=false;
  if(args.size()==1 && args[0]==L"--dry-run") {o.dry=true;out=o;return true;}
  if(args.size()==1 && args[0]==L"--self-test") {o.self=true;out=o;return true;}
  for(size_t i=0;i<args.size();++i) {
    if(args[i]==L"--loader" && !seenLoader && i+1<args.size()) {
      o.loader=args[++i]; seenLoader=true;
    } else if(args[i]==L"--seconds" && !seenSeconds && i+1<args.size()) {
      if(!duration(args[++i],o.seconds)) return false; seenSeconds=true;
    } else if(args[i]==L"--graphics-proxy" && !seenGraphics && i+1<args.size()) {
      o.graphicsProxy=args[++i];seenGraphics=true;
    } else if(args[i]==L"--present-boundary" && !o.presentBoundary) {
      o.presentBoundary=true;
    } else return false;
  }
  if(!absolute(o.loader)||(seenGraphics&&!absolute(o.graphicsProxy))||(o.presentBoundary&&!seenGraphics)) return false; out=o; return true;
}
struct PresentHost {
  NativeRenderBinding binding;
  bool teardownInCallback=false,teardownCompleted=false;
};
// The facade objects survive explicit Shutdown. Their resources and all
// thread-bound members are constructed/cleaned on the service thread.
class NativeBackend final:public RuntimeBackend {
 public:
  OwnerService owner;
  RenderThreadDispatcher render{owner};
  RenderRoute route{render};
  PresentHost* present=nullptr;
  std::unique_ptr<NativeRuntimeHost> host;
  Options options;
  vr::EVRInitError start(uint32_t token,const std::atomic<bool>& cancelled,RuntimeInterfaces& out)override {
    if(host)return vr::VRInitError_Init_Internal; // native diagnostic: one generation per process
    route.present=present?&present->binding.work():nullptr;
    if(!route.bind())return vr::VRInitError_Init_Internal;
    if(!owner.start([this]{if(host)host->pumpEvents();}))return vr::VRInitError_Init_Internal;
    auto error=vr::VRInitError_Init_Internal;
    if(!route.invoke([&]{
      host=std::make_unique<NativeRuntimeHost>(owner,render,route,present?present->binding.device():nullptr);host->startupOptions=options;
      error=host->start(token,cancelled,out);
    }))return vr::VRInitError_Init_Internal;
    return error;
  }
  bool prepareStop() {
    bool drained=false;
    const bool tracing=owner.running()||host!=nullptr;
    ShutdownTrace stage("backend_prepare_stop",tracing);
    const bool invoked=route.invoke([&] {
      const auto r=host?host->stereo.drain():XR_SUCCESS;
      drained=r==XR_SUCCESS;
      if(host)host->clean=result("gpu_drain",r)&&host->clean;
      std::printf("gpu_drain,result=%d,pending=%u\n",int(r),unsigned(host&&host->stereo.needsGpuDrain()));
      std::fflush(stdout);
    });
    stage.end(invoked&&drained); return invoked&&drained;
  }
  bool stop()noexcept override {
    // Early exits still have their Init/render caller available. Normal
    // System-thread Shutdown comes only after that caller explicitly drains.
    const bool tracing=owner.running()||host!=nullptr;
    ShutdownTrace prep("backend_stop_prepare",tracing);
    if(owner.running()&&(present||render.isRenderThread()))prep.end(prepareStop());
    else prep.end(true);
    bool cleaned=!host;
    ShutdownTrace joinStage("backend_owner_join",tracing);
    auto finalizer=[&]{
      ShutdownTrace finalizer("backend_owner_finalizer",tracing);
      cleaned=host?host->stop():true;
      finalizer.end(cleaned);
    };
    bool joined=false;
    if(present) {
      ShutdownTrace boundaryStage("backend_present_teardown",tracing);
      // Keep the render caller inside this final callback until runtime
      // destruction finishes. Callback user state remains alive throughout;
      // the controller releases its lease only after the callback unwinds.
      bool entered=false;
      const bool requested=present->binding.work().invoke([&]{
        present->teardownInCallback=present->binding.callbackActive()&&render.isRenderThread();
        if(!present->teardownInCallback)return;
        const auto stopped=shutdownAtRenderBoundary(present->binding.work(),render,owner,finalizer);
        entered=stopped.entered;joined=stopped.joined;
        present->teardownCompleted=entered&&joined&&cleaned;
      });
      boundaryStage.end(requested&&entered&&joined&&cleaned);
      if(!requested||!entered) {
        std::puts("error,present_teardown_not_entered_resources_retained_until_process_exit");
        std::fflush(stdout);std::_Exit(3);
      }
    } else {
      ShutdownTrace closeStage("backend_render_close",tracing);
      render.close();closeStage.end(true);
      joined=owner.stop(finalizer);
    }
    joinStage.end(joined&&cleaned);
    if(joined&&host&&host->stereo.needsGpuDrain()) {
      // Diagnostic-only last resort: never destroy a live XR session beneath
      // uncompleted GPU work. Exit this isolated child, without a crash dialog.
      std::puts("error,gpu_drain_failed_resources_retained_until_process_exit");
      std::fflush(stdout);std::_Exit(3);
    }
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
int run(const Options& options,PresentHost* present=nullptr) {
  // Process-lifetime objects back the exported function pointers. Their
  // resources are still explicitly retired through Shutdown before return.
  static NativeBackend backend;backend.options=options;backend.present=present;
  static RuntimeLifecycle runtime(backend);
  if(!bindRuntimeExports(runtime))return 3;
  vr::EVRInitError initError=vr::VRInitError_Unknown;
  const uint32_t initToken=edvr_native_VR_InitInternal(&initError,vr::VRApplication_Scene);
  if(!initToken||initError!=vr::VRInitError_None){std::printf("error,runtime_init,%d\n",int(initError));return 3;}
  struct ShutdownOnExit {~ShutdownOnExit(){edvr_native_VR_ShutdownInternal();}} shutdownOnExit;
  NativeRuntimeHost& host=*backend.host;
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
  uint64_t startupRecenters=0;
  if(!backend.owner.invoke([&]{startupRecenters=host.recenters;}))return 3;
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
        stateValid=host.recenters==startupRecenters+reset+1&&host.lastResetResult==XR_SUCCESS&&!host.read().geometryValid&&
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
  std::printf("runtime_threads,init=%lu,system=%lu,xr_owner=%lu,graphics_caller=%lu,idle_pump=1,system_resets=2\n",
    (unsigned long)initThread,(unsigned long)systemThread,(unsigned long)host.ownerThread,(unsigned long)host.graphicsCalls.thread);
  liveQueries.store(true,std::memory_order_release);
  const ULONGLONG startup=GetTickCount64(); ULONGLONG started=startup,exitRequested=0;
  // Explicit render callbacks advance a copied six-face loading scene with
  // no application Wait/Submit. The owner never renders from its idle pump.
  // Destroy and overwrite the caller sources before the timed viewing phase.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> skySources[6];vr::Texture_t skyTextures[6]{};
  bool skyCreated=true;
  if(!backend.route.invoke([&]{skyCreated=host.graphicsCalls.invoke([&]{
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
  })&&skyCreated;})||!skyCreated||compositor->SetSkyboxOverride(skyTextures,6)!=vr::VRCompositorError_None)return 3;
  bool overwritten=false;
  if(!backend.route.invoke([&]{overwritten=host.graphicsCalls.invoke([&]{
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;host.graphics.device()->GetImmediateContext(&context);
    std::vector<uint32_t> black(256*256,0xff000000u);
    for(auto& source:skySources){context->UpdateSubresource(source.Get(),0,nullptr,black.data(),256*4,0);source.Reset();}
  });})||!overwritten)return 3;
  compositor->ClearLastSubmittedFrame();
  const auto cacheBeforeLoading=host.compositorRead();
  bool outsideRejected=false,outsideRan=false;
  uint64_t idleGraphics=0,idleLoading=0;
  if(!backend.owner.invoke([&]{
    idleGraphics=host.graphicsCalls.calls;idleLoading=host.loadingFrames;
    outsideRejected=!host.graphicsCalls.invoke([&]{outsideRan=true;});
  })||!outsideRejected||outsideRan)return 3;
  const auto idlePumps=host.publishedPumps.load(std::memory_order_acquire);
  const auto idleDeadline=GetTickCount64()+1000;
  while(host.publishedPumps.load(std::memory_order_acquire)<idlePumps+2&&GetTickCount64()<idleDeadline)Sleep(2);
  bool idleSafe=false;
  if(!backend.owner.invoke([&]{idleSafe=host.graphicsCalls.calls==idleGraphics&&host.loadingFrames==idleLoading;})||
     !idleSafe||host.publishedPumps.load(std::memory_order_acquire)<idlePumps+2)return 3;
  std::puts("render_idle,cpu_pump_continues=1,graphics_unchanged=1,loading_unchanged=1,outside_boundary_rejected=1");
  const auto loadingDeadline=startup+(std::min)(3000ULL,options.seconds*250ULL);
  std::puts("loading_phase,begin=1,six_faces=1,sources_overwritten_and_released=1");
  while(GetTickCount64()<loadingDeadline)
    if(!backend.route.invoke([&]{host.loadingBoundary();}))return 3;
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
    if(!backend.route.invoke(renderStep)){failed=true;break;}
  }
  const bool gpuDrained=backend.prepareStop();
  // Stop the System producer, then issue exported Shutdown from that same
  // caller thread. The runtime owner cancels queued work, cleans and joins.
  DWORD shutdownThread=0;
  ShutdownTrace systemJoin("controller_system_join");
  const bool systemJoined=systemClient.stop([&]{shutdownThread=GetCurrentThreadId();edvr_native_VR_ShutdownInternal();});
  systemJoin.end(systemJoined);
  const bool cleanup=gpuDrained&&systemJoined&&shutdownThread==systemThread&&!backend.owner.running()&&host.clean&&!runtime.running()&&host.runtimeGeneration==0&&host.stops==1;
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
  uint64_t privateLists=0,unknownLists=0;
  if(host.bridgeCounts)host.bridgeCounts(&privateLists,&unknownLists);
  const uint64_t expectedPrivate=4*layers+2*host.loadingLayers;
  const bool bridgeGate=!host.graphicsProxy||(privateLists==expectedPrivate&&unknownLists==0);
  const DWORD graphicsThread=present?present->binding.renderThread():initThread;
  const bool callerGate=!present||graphicsThread!=initThread;
  const bool renderGate=callerGate&&host.graphicsCalls.calls>0&&host.graphicsCalls.thread==graphicsThread&&
    host.graphicsCalls.thread!=host.ownerThread&&host.graphicsCalls.wrongThread==0&&host.graphicsCalls.rejected==1&&idleSafe&&bridgeGate;
  std::printf("render_boundary,callbacks=%llu,thread=%lu,xr_owner=%lu,wrong_thread=%llu,rejected=%llu,explicit_loading=1,paired_proxy=%u,private_lists=%llu,expected_private=%llu,unknown_lists=%llu,passed=%u\n",
    (unsigned long long)host.graphicsCalls.calls,(unsigned long)host.graphicsCalls.thread,(unsigned long)host.ownerThread,
    (unsigned long long)host.graphicsCalls.wrongThread,(unsigned long long)host.graphicsCalls.rejected,unsigned(host.graphicsProxy!=nullptr),
    (unsigned long long)privateLists,(unsigned long long)expectedPrivate,(unsigned long long)unknownLists,unsigned(renderGate));
  std::printf("summary,frames=%llu,stereo=%llu,empty=%llu,valid_views=%llu,invalid_views=%llu,valid_head=%llu,normal_stop=%u,cleanup=%u\n",
    (unsigned long long)frames,(unsigned long long)layers,(unsigned long long)empty,(unsigned long long)valid,(unsigned long long)invalid,
    (unsigned long long)headValid,unsigned(stopped),unsigned(cleanup));
  const bool passed=!failed && stopped && cleanup && renderGate && layers && headValid && bootstrapComplete &&host.recenters==startupRecenters+2&&host.resetPolls==2&&
    systemQueries&&validSystemQueries&&host.eventPumps>beforePumps&&loadingGate&&host.skyboxSets==1&&host.skyboxClears==1&&
    host.loadingToScene==1&&host.loadingLayers>=2&&host.loadingFrames==host.loadingLayers+host.loadingEmpty&&!host.skybox.ready()&&
    host.copiedEyes==layers*2 && host.composedPairs==layers && host.compositorSubmits==layers*2 &&
    host.compositorWaits==frames+host.startupFrames&&cachedChecks==frames&&host.compositorHandoffs==layers&&host.validGamePoses;
  std::puts(passed?"native_stereo: PASS":"native_stereo: INCOMPLETE_OR_FAILED");return passed?0:4;
}
int runWithPresent(const Options& options) {
  // Hook owner and callback code are process lifetime; don't unload the proxy.
  const auto proxy=LoadLibraryExW(options.graphicsProxy.c_str(),nullptr,
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if(!proxy){std::puts("error,present_proxy_load");return 3;}
  PresentDevice device;
  const auto created=device.initialize(proxy,D3D_DRIVER_TYPE_HARDWARE);
  if(FAILED(created)){std::printf("error,present_device,%08lx\n",(unsigned long)created);return 3;}
  PresentHost present;
  std::atomic<bool> finished{false};int outcome=5;DWORD initCaller=0;
  std::thread controller([&] {
    initCaller=GetCurrentThreadId();
    try {
      const HRESULT acquired=present.binding.acquire(options.graphicsProxy);
      if(acquired!=S_OK) {
        std::printf("error,native_graphics_discovery,%08lx\n",(unsigned long)acquired);
        outcome=3;
      } else if(!present.binding.waitForRender()) {
        std::puts("error,native_startup_no_present_before_runtime");
        outcome=3;
      } else {
        // Only this assertion knows the fixture device. Runtime startup uses
        // the independently acquired provider snapshot and observed callback.
        const bool same=present.binding.device()==device.device()&&present.binding.context()==device.context();
        std::printf("paired_startup,discovered=1,init_thread=%lu,render_thread=%lu,device_matches_fixture=%u,provider_matches=%u\n",
          (unsigned long)initCaller,(unsigned long)present.binding.renderThread(),unsigned(same),unsigned(present.binding.provider()==proxy));
        Options nativeOptions=options;
        nativeOptions.graphicsProvider=present.binding.provider();
        outcome=same&&nativeOptions.graphicsProvider==proxy?run(nativeOptions,&present):3;
      }
    }
    catch(const std::exception& e){std::printf("error,present_controller,%s\n",e.what());}
    catch(...){std::puts("error,present_controller_exception");}
    finished.store(true,std::memory_order_release);
  });
  uint64_t presents=0;
  bool presenting=true;
  while(!finished.load(std::memory_order_acquire)) {
    // The loop receives no stop/pause request from the runtime. Its final
    // callback holds this caller until XR teardown completes, just as a
    // callback in a game-controlled loop must do.
    if(FAILED(device.present())) {
      presenting=false;present.binding.work().close();
      ++presents;
    } else {
      ++presents;
    }
    Sleep(1);
  }
  ShutdownTrace controllerJoin("present_controller_join");
  controller.join();controllerJoin.end(true);
  ShutdownTrace callbackClose("present_callback_close");
  const auto closed=present.binding.close();
  callbackClose.end(closed==S_OK);
  ShutdownTrace callbackRelease("present_callback_release");
  const auto released=present.binding.release();
  callbackRelease.end(released==S_OK);
  const bool retired=closed==S_OK&&released==S_OK&&!present.binding.callbackActive();
  const bool teardown=present.teardownInCallback&&present.teardownCompleted&&retired;
  const bool passed=outcome==0&&presenting&&present.binding.correct()&&present.binding.callbacks()>0&&teardown&&
    initCaller!=present.binding.renderThread()&&closed==S_OK&&released==S_OK;
  std::printf("present_boundary,presents=%llu,callbacks=%llu,render_thread=%lu,init_thread=%lu,device_before_init=1,close=%08lx,release=%08lx,passed=%u\n",
    (unsigned long long)presents,(unsigned long long)present.binding.callbacks(),(unsigned long)present.binding.renderThread(),
    (unsigned long)initCaller,(unsigned long)closed,(unsigned long)released,unsigned(passed));
  std::printf("present_teardown,in_callback=%u,owner_completed=%u,callback_retired=%u,loop_pause_requested=0\n",
    unsigned(present.teardownInCallback),unsigned(present.teardownCompleted),unsigned(retired));
  std::puts(passed?"native_present: PASS":"native_present: INCOMPLETE_OR_FAILED");
  return passed?0:4;
}
int selfTest() {
  unsigned checks=0,failures=0;auto check=[&](bool yes,const char* msg){++checks;if(!yes){++failures;std::printf("FAIL: %s\n",msg);}};
  edvr::openxr::test::runLaunchCentreCases(check);
  edvr::openxr::test::runFeatureHostCases(check);
  Options o;
  check(parse({L"--loader",L"C:\\runtime\\loader.dll"},o)&&o.seconds==10,"default duration");
  check(parse({L"--seconds",L"60",L"--loader",L"D:/a.dll"},o)&&o.seconds==60,"bounded duration");
  for(const auto* s:{L"0",L"61",L"1x",L"-1",L"",L"999999999999",L" 1"})
    check(!parse({L"--loader",L"C:/a.dll",L"--seconds",s},o),"invalid duration");
  for(const auto* s:{L"x.dll",L"C:a.dll",L"/a.dll",L"1:/a.dll",L"C:/"}) check(!parse({L"--loader",s},o),"absolute loader required");
  check(parse({L"--dry-run"},o)&&o.dry,"dry-run standalone");
  check(!parse({L"--dry-run",L"--loader",L"C:/a.dll"},o),"dry-run cannot hide invalid operational args");
  check(!parse({L"--loader",L"C:/a.dll",L"--loader",L"C:/b.dll"},o),"duplicate rejected");
  check(parse({L"--loader",L"C:/a.dll",L"--graphics-proxy",L"D:/d3d11.dll"},o)&&o.graphicsProxy==L"D:/d3d11.dll","explicit graphics proxy");
  check(!parse({L"--loader",L"C:/a.dll",L"--graphics-proxy",L"d3d11.dll"},o),"relative graphics proxy rejected");
  check(!parse({L"--loader",L"C:/a.dll",L"--graphics-proxy"},o),"missing graphics proxy rejected");
  check(!parse({L"--loader",L"C:/a.dll",L"--graphics-proxy",L"D:/a.dll",L"--graphics-proxy",L"D:/b.dll"},o),"duplicate graphics proxy rejected");
  check(parse({L"--loader",L"C:/a.dll",L"--graphics-proxy",L"D:/d3d11.dll",L"--present-boundary"},o)&&o.presentBoundary,"explicit Present boundary");
  check(!parse({L"--loader",L"C:/a.dll",L"--present-boundary"},o),"Present boundary requires paired proxy");
  check(!parse({L"--loader",L"C:/a.dll",L"--graphics-proxy",L"D:/d3d11.dll",L"--present-boundary",L"--present-boundary"},o),"duplicate Present boundary rejected");
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
  // Session display dimensions survive transient geometry invalidation. This
  // inert host opens no loader, device, session, or XR operation.
  {
    OwnerService inertOwner; RenderThreadDispatcher inertDispatcher(inertOwner);
    RenderRoute inertRoute{inertDispatcher};
    NativeRuntimeHost inertHost(inertOwner,inertDispatcher,inertRoute);
    SystemRead metadata{}; metadata.connected=true;
    metadata.recommendedWidth[0]=3072; metadata.recommendedWidth[1]=3072;
    metadata.recommendedHeight[0]=3264; metadata.recommendedHeight[1]=3264;
    const auto generation=inertHost.geometry.begin(metadata);
    check(generation!=0,"inert host begins display metadata generation");
    uint32_t width=0,height=0; inertHost.systemInterface.GetRecommendedRenderTargetSize(&width,&height);
    check(width==3072&&height==3264,"native system returns metadata dimensions before pose");
    auto aux=inertHost.readAuxiliary();
    check(aux.width[0]==3072&&aux.width[1]==3072&&aux.height[0]==3264&&aux.height[1]==3264,
      "native auxiliary returns metadata dimensions before pose");
    inertHost.geometry.invalidate(generation);
    width=height=0; inertHost.systemInterface.GetRecommendedRenderTargetSize(&width,&height);
    aux=inertHost.readAuxiliary();
    check(width==3072&&height==3264&&aux.width[0]==3072&&aux.height[0]==3264,
      "native dimensions survive geometry invalidation");
    int32_t x=0,y=0;uint32_t windowWidth=0,windowHeight=0;
    inertHost.displayInterface.GetWindowBounds(&x,&y,&windowWidth,&windowHeight);
    uint32_t eyeX=0,eyeY=0,eyeWidth=0,eyeHeight=0;
    inertHost.displayInterface.GetEyeOutputViewport(vr::Eye_Right,&eyeX,&eyeY,&eyeWidth,&eyeHeight);
    check(windowWidth==6144&&windowHeight==3264&&eyeX==3072&&eyeWidth==3072&&eyeHeight==3264,
      "native extended display preserves dimensions after invalidation");
    inertHost.geometry.retire(generation);
    width=height=0; inertHost.systemInterface.GetRecommendedRenderTargetSize(&width,&height);
    aux=inertHost.readAuxiliary();
    check(width==0&&height==0&&!aux.width[0]&&!aux.height[0],"native retirement clears display dimensions");
  }
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
  if(!parse(std::vector<std::wstring>(argv+1,argv+argc),options)) {std::puts("usage: --loader ABSOLUTE_DLL [--seconds 1..60] [--graphics-proxy ABSOLUTE_DLL [--present-boundary]] | --self-test | --dry-run");return 2;}
  if(options.dry) {std::puts("Would create a standalone OpenXR stereo diagnostic; no files, loader, device or runtime used.");return 0;}
  if(options.self) return selfTest();
  try {return options.presentBoundary?runWithPresent(options):run(options);} catch(const std::exception& e) {std::printf("error,exception,%s\n",e.what());return 5;}
}
