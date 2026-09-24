#pragma once
#include "treatment_cases.h"
#include <vector>
#include <string>
#include <cstdio>
#include <memory>

// Private timing-table fixture exported by the test EXE, alongside
// treatment_cases.h's fss/temporal/sharpen/menu ones: the actual host
// dispatcher and NativeTimingClient::acquire's module-ownership validation,
// without a real D3D11 timing provider. Mirrors native_timing.cpp's own
// gate closely enough to observe frame_end_overlap's ordering: c->published
// (here state.published) rejects every gpuEye/publishCpu call once a
// sequence's CPU record has been published, regardless of which eye value
// asks.
namespace edvr::openxr::test::timing_fixture {
struct LogEntry { std::string tag; uint64_t sequence=0; bool accepted=false; };
struct State {
  uint64_t waitSequence=0;
  bool published=false;
  std::vector<LogEntry> log;
} inline state;
inline void note(const char* tag,uint64_t sequence,bool accepted) { state.log.push_back({tag,sequence,accepted}); }
inline uint64_t WINAPI waitBegin(void*) { state.published=false; return ++state.waitSequence; }
inline HRESULT WINAPI waitEnd(void*,uint64_t,uint32_t,int64_t) { return S_OK; }
inline uint32_t WINAPI gpuEye(void*,uint64_t sequence,uint32_t eye,uint32_t begin,uint32_t,ID3D11Texture2D*) {
  char tag[16]; std::snprintf(tag,sizeof(tag),"eye%ub%u",eye,begin);
  const bool ok=!state.published&&sequence==state.waitSequence;
  note(tag,sequence,ok);
  return ok?1u:0u;
}
inline HRESULT WINAPI publishCpu(void*,const EdvrNativeTimingFrame* frame) {
  const uint64_t sequence=frame?frame->sequence:0;
  const bool ok=frame&&!state.published&&sequence==state.waitSequence;
  note("publishCpu",sequence,ok);
  if(!ok)return E_INVALIDARG;
  state.published=true;
  return S_OK;
}
inline HRESULT WINAPI invalidate(void*) { note("invalidate",state.waitSequence,true); state.published=false; return S_OK; }
inline HRESULT WINAPI close(void*) { return S_OK; }
inline uint32_t WINAPI gpuEnabled(void*) { return 0u; }
inline HRESULT WINAPI publishDeviceGpu(void*,const EdvrNativeDeviceGpuSample*) { return S_OK; }
} // namespace edvr::openxr::test::timing_fixture

#pragma comment(linker, "/export:edvrAcquireNativeTiming")
extern "C" HRESULT WINAPI edvrAcquireNativeTiming(const EdvrNativeTimingRequest*,EdvrNativeTimingTable* table) {
  using namespace edvr::openxr::test::timing_fixture;
  *table={sizeof(*table),EDVR_NATIVE_TIMING_VERSION_3,&state,waitBegin,waitEnd,gpuEye,publishCpu,invalidate,close,gpuEnabled,publishDeviceGpu};
  return S_OK;
}

namespace edvr::openxr::test {
// launch_fixture::Fixture builds its NativeRuntimeHost as a direct member, on
// whichever thread constructs the Fixture -- fine for every other case here,
// which only ever reaches the host through capture() wrapped in route.invoke
// or owner.invoke. This is the one that drives waitPoses/submitEye through
// their own caller/owner split for real, and both check the constructing
// thread against the one actually running the owner body: NativeRuntimeHost::
// ownerThread (an in-class GetCurrentThreadId() default) and FrameBoundary's
// own thread_, captured the same way with no public setter. The real runtime
// avoids this by building the host from the thread that becomes its owner;
// this fixture does the same by deferring construction into owner.invoke,
// after start() has actually spawned that thread.
struct OwnerBuiltFixture {
  launch_fixture::Fake fake;
  OwnerService owner;
  RenderThreadDispatcher dispatcher{owner};
  RenderRoute route{dispatcher};
  std::unique_ptr<NativeRuntimeHost> hostPtr;
  bool initialized=false;
  // Call once, after owner.start(), from inside owner.invoke so every
  // constructor runs on that thread. Mirrors launch_fixture::Fixture's own
  // constructor line for line.
  void build() {
    using namespace launch_fixture;
    active=&fake;
    hostPtr=std::make_unique<NativeRuntimeHost>(owner,dispatcher,route);
    auto& host=*hostPtr;
    host.instance=instance();host.session=session();host.local=local();host.view=view();
    host.api.convertTime=convert;host.api.locateSpace=locate;host.api.locateViews=locateViews;
    const Dispatch dispatch{poll,beginSession,endSession,wait,beginFrame,endFrame};
    SystemRead metadata{};metadata.connected=true;
    for(unsigned eye=0;eye<2;++eye){metadata.recommendedWidth[eye]=3072;metadata.recommendedHeight[eye]=3264;}
    host.geometryGeneration=host.geometry.begin(metadata);
    host.compositorGeneration=host.poses.begin();host.runtimeGeneration=host.gate.beginGeneration();
    initialized=host.seated.begin({create,destroy},session(),local())&&host.changes.begin(session())&&
      host.resetEvents.begin(host.geometryGeneration)&&host.state.reset(dispatch,instance(),session(),XR_ENVIRONMENT_BLEND_MODE_OPAQUE)==XR_SUCCESS&&
      host.state.pollEvents()==XR_SUCCESS&&host.state.startIfReady()==XR_SUCCESS;
    GeometryInput geometry{};geometry.generation=host.geometryGeneration;geometry.sequence=1;geometry.displayTime=100;
    geometry.headPose=fake.head;geometry.headFlags=tracked;
    geometry.viewFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    for(unsigned eye=0;eye<2;++eye) {
      geometry.width[eye]=3072;geometry.height[eye]=3264;geometry.views[eye].pose=fake.head;
      geometry.views[eye].pose.position.x=eye?.032f:-.032f;geometry.views[eye].fov={-.7f,.7f,.7f,-.7f};
    }
    initialized&=host.geometry.publish(geometry,false,false);
    host.frameGeometry=geometry;host.frameGeometryAvailable=true;
  }
  // Declared after owner/dispatcher/route so hostPtr, which borrows them,
  // destructs first. Mirrors launch_fixture::Fixture's own teardown (never
  // close(), which is gated on the owner thread this destructor is not run
  // from); the caller stops the owner service itself beforehand.
  ~OwnerBuiltFixture() {
    if(!hostPtr)return;
    auto& host=*hostPtr;
    host.boundary.clear();host.seated.shutdown();host.state.abandonAfterOwnerDestruction();
    host.gate.requestStop(host.runtimeGeneration);host.gate.finishGeneration(host.runtimeGeneration);
    host.runtimeGeneration=0;host.instance=XR_NULL_HANDLE;host.session=XR_NULL_HANDLE;
    host.local=host.view=XR_NULL_HANDLE;host.api={};launch_fixture::active=nullptr;
  }
};

// A real deferred pair through the actual host (submitEye's caller/owner
// split, not just capture() the way runTreatmentCases drives it), with the
// fake timing table above recording every call frame_end_overlap touches,
// in order. pixels=false at finish() (frameWithheld -- sceneLayerAvailable's
// !frameWithheld covers it, since no feature client is acquired to set
// resubmitEnabled) skips compose()/stereo, which nothing here initializes;
// capture() above still runs with copyPixels=true (frame_.shouldRender),
// which is all this needs from it: the CPU-only application segment
// brackets and the pair actually completing through finishPair()'s
// xrEndFrame (the fake dispatch's endFrame, still XR_SUCCESS).
template<class Check> void runFrameEndOverlapCases(Check check) {
  using namespace treatment_fixture;
  using Microsoft::WRL::ComPtr;
  timing_fixture::state.log.clear();timing_fixture::state.published=false;timing_fixture::state.waitSequence=0;
  treatment_fixture::state={};treatment_fixture::state.thread=GetCurrentThreadId();
  ComPtr<ID3D11Device> producer,consumer;ComPtr<ID3D11DeviceContext> producerContext,consumerContext;
  const auto createDevice=systemD3D11CreateDevice();
  check(createDevice&&SUCCEEDED(createDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&producer,nullptr,&producerContext))&&
    SUCCEEDED(createDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&consumer,nullptr,&consumerContext)),"frame_end_overlap devices");
  if(!producer||!consumer)return;
  D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=1;
  desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> left,right;
  check(SUCCEEDED(producer->CreateTexture2D(&desc,nullptr,&left))&&SUCCEEDED(producer->CreateTexture2D(&desc,nullptr,&right)),"frame_end_overlap eye textures");
  if(!left||!right)return;
  OwnerBuiltFixture f;
  f.fake.shouldRender=true;
  check(f.route.bind()&&f.owner.start(),"frame_end_overlap rendezvous started");
  check(f.owner.invoke([&]{f.build();}),"frame_end_overlap host built on the owner thread");
  check(f.initialized,"frame_end_overlap host fixture initialized");
  if(!f.initialized){f.owner.stop();return;}
  auto& h=*f.hostPtr;
  h.frameViews[0]=h.frameGeometry.views[0];h.frameViews[1]=h.frameGeometry.views[1];
  h.frameContentViews[0]=h.frameViews[0];h.frameContentViews[1]=h.frameViews[1];
  h.startupOptions.separateDevice=true;h.externalDevice=producer.Get();h.frameEndOverlapEnabled=true;
  const auto provider=GetModuleHandleW(nullptr);
  check(h.fss.acquire(provider,producer.Get(),1)==S_OK&&h.temporal.acquire(provider,producer.Get(),1)==S_OK&&
    h.sharpen.acquire(provider,producer.Get(),1)==S_OK&&h.menu.acquire(provider,producer.Get(),1)==S_OK&&
    h.timing.acquire(provider,producer.Get(),1)==S_OK,"frame_end_overlap validated provider tables");
  bool initialized=false;
  check(f.route.invoke([&]{initialized=h.captured.initializeShared(producer.Get(),consumer.Get(),&h.graphicsCalls)==S_OK;})&&initialized,
    "frame_end_overlap capture owns separate XR device");
  if(!initialized){f.owner.stop();return;}
  const vr::Texture_t leftTexture{left.Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
  const vr::Texture_t rightTexture{right.Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
  // waitPoses's own geometry/HeadLocator path needs a real SessionBinding --
  // an actual xrCreateSession and reference spaces -- which no fixture here
  // sets up (no existing case reaches it either: they all stop at capture()).
  // Everything submitEye and capture() actually need from a wait is lower
  // down in waitPoses's own body: an open boundary frame, geometryReady_,
  // and the timing context timing.waitBegin() opens. Drive exactly that,
  // on the owner thread, in the same order waitPoses itself uses.
  auto openFrame=[&]{
    h.timingRetire();
    h.timingSequence=h.timing.waitBegin();h.timingFrameActive=h.timingSequence!=0;
    h.timingApplicationSequence.store(h.timingSequence,std::memory_order_release);
    h.timingResetFrame(h.timingSequence);
    h.submitSample={};h.transferWall={};h.submitSample.sequence=h.timingSequence;h.submitCallbacksBegin=h.graphicsCalls.calls;
    h.temporalFrameEyes=0;h.frameWithheld=false;h.frameDecisionReady=false;
    if(h.boundary.waitAndBegin(FramePacing::Runtime)!=XR_SUCCESS)return false;
    h.boundary.setGeometryReady(true);
    h.frameSpace=h.seated.space();h.frameGeometryAvailable=true;
    return true;
  };

  // --- Pair 1: eligible for the overlap (separate device, runtime pacing,
  // frameEndOverlapEnabled) -- the second Submit must defer.
  bool firstWaitOk=false;
  check(f.owner.invoke([&]{firstWaitOk=openFrame();}),"frame_end_overlap first wait invoked on owner");
  check(firstWaitOk,"frame_end_overlap first wait admits a frame");
  h.frameWithheld=true;
  check(h.submitEye(h.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "frame_end_overlap first eye submits");
  timing_fixture::state.log.clear(); // isolate the deferring eye's own calls
  const auto secondResult=h.submitEye(h.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default);
  check(secondResult==vr::VRCompositorError_None,"frame_end_overlap deferred second eye returns immediately");
  // Not h.pendingFrameEndFinish: the FIFO worker can pick up and finish the
  // queued job before this test thread gets back around to checking it here
  // -- the overlap's whole point -- so that flag is racy from this side. The
  // counters are not: submitEye's owner body sets them itself, synchronously,
  // before service.submit even returns.
  check(h.frameEndOverlapCount==1&&h.frameEndSyncCount==0,
    "frame_end_overlap pair actually deferred");
  long publishIndex=-1;
  for(size_t i=0;i<timing_fixture::state.log.size();++i)if(timing_fixture::state.log[i].tag=="publishCpu"){publishIndex=long(i);break;}
  check(publishIndex>=0&&timing_fixture::state.log[size_t(publishIndex)].accepted,
    "frame_end_overlap (a): publishCpu(seq) ran, and before Submit returned to Elite");
  bool sawRejectedReopen=false,sawAcceptedReopenAfterPublish=false;
  for(size_t i=size_t(publishIndex)+1;i<timing_fixture::state.log.size();++i) {
    const auto& e=timing_fixture::state.log[i];
    if(e.tag=="eye2b1"||e.tag=="eye3b1") { if(e.accepted)sawAcceptedReopenAfterPublish=true; else sawRejectedReopen=true; }
  }
  check(sawRejectedReopen&&!sawAcceptedReopenAfterPublish,
    "frame_end_overlap (b): applicationSegment/producerResume(seq,true) after publishCpu find it retired, not reopened");
  // Flush the FIFO queue: finishPendingFrameEnd (queued from inside the
  // owner body above) is guaranteed to run before this no-op returns -- the
  // same ordering production code relies on for the next WaitGetPoses/Submit.
  check(f.owner.invoke([]{}),"frame_end_overlap queue flushed");
  check(!h.pendingFrameEndFinish&&h.lastCompositorResult==vr::VRCompositorError_None,
    "frame_end_overlap (c): the queued job ran finishPair() to completion, after publishCpu had already returned to Elite");

  // --- Pair 2: frame_end_overlap OFF -- must still finish inline, exactly
  // as every pair did before this feature existed.
  h.frameEndOverlapEnabled=false;
  bool secondWaitOk=false;
  check(f.owner.invoke([&]{secondWaitOk=openFrame();}),"frame_end_overlap second wait invoked on owner");
  check(secondWaitOk,"frame_end_overlap second wait admits a frame");
  h.frameWithheld=true;
  check(h.submitEye(h.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "frame_end_overlap sync pair first eye submits");
  timing_fixture::state.log.clear();
  check(h.submitEye(h.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "frame_end_overlap sync pair second eye finishes inline");
  check(h.frameEndOverlapCount==1&&h.frameEndSyncCount==1,"frame_end_overlap second pair took the synchronous path");
  unsigned publishCount=0;for(const auto& e:timing_fixture::state.log)if(e.tag=="publishCpu")++publishCount;
  check(publishCount==1&&!timing_fixture::state.log.empty()&&timing_fixture::state.log[0].tag!="publishCpu",
    "frame_end_overlap (d): the synchronous path still publishes exactly once, and not as its first call -- "
    "publishSubmitTimingIfComplete, unchanged, still polls device timing ahead of it");

  check(f.owner.invoke([&]{check(h.captured.shutdownShared()==S_OK,"frame_end_overlap shared transfer retires");}),
    "frame_end_overlap teardown on owner");
  check(f.owner.stop(),"frame_end_overlap owner stopped");
  h.fss.close();h.temporal.close();h.sharpen.close();h.menu.close();h.timing.close();
  treatment_fixture::state={};timing_fixture::state.log.clear();
}
} // namespace edvr::openxr::test
