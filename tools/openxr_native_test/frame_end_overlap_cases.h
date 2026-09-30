#pragma once
#include "treatment_cases.h"
#include <vector>
#include <string>
#include <cstdio>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>

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
  std::function<void()> beforePublish;
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
  if(state.beforePublish)state.beforePublish();
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

// ABI provider only: the callback itself is production NativeRenderBinding.
// Keep the WARP graphics and registration alive until binding.release().
namespace edvr::openxr::test::binding_fixture {
struct State {
  ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;
  EdvrRenderBoundaryRequest request{};
  std::atomic<unsigned> active{0};std::atomic<bool> admitted{false};
} inline state;
inline HRESULT WINAPI close(void*) {
  state.admitted.store(false);return state.active.load()?E_PENDING:S_OK;
}
inline HRESULT WINAPI release(void*) {
  if(state.active.load())return E_PENDING;
  state.admitted.store(false);state.request={};return S_OK;
}
inline HRESULT present() {
  if(!state.admitted.load())return S_OK;
  state.active.fetch_add(1);
  const auto result=state.request.callback(state.request.user,state.device,state.context);
  state.active.fetch_sub(1);return result;
}
}
#pragma comment(linker, "/export:edvrAcquireNativeGraphics")
#pragma comment(linker, "/export:edvrAcquireGraphicsBridge")
#pragma comment(linker, "/export:edvrAcquireRenderBoundary")
extern "C" HRESULT WINAPI edvrAcquireNativeGraphics(const EdvrNativeGraphicsRequest*,EdvrNativeGraphicsTable* table) {
  auto& s=edvr::openxr::test::binding_fixture::state;
  if(!table||!s.device||!s.context)return E_NOINTERFACE;
  s.device->AddRef();s.context->AddRef();
  *table={sizeof(*table),EDVR_NATIVE_GRAPHICS_VERSION_1,s.device,s.context};return S_OK;
}
extern "C" HRESULT WINAPI edvrAcquireGraphicsBridge(const EdvrGraphicsBridgeRequest*,EdvrGraphicsBridgeTable*) {
  return E_NOTIMPL; // NativeRenderBinding verifies the export but does not acquire it.
}
extern "C" HRESULT WINAPI edvrAcquireRenderBoundary(const EdvrRenderBoundaryRequest* request,EdvrRenderBoundaryTable* table) {
  using namespace edvr::openxr::test::binding_fixture;
  if(!request||!table||state.request.callback)return E_PENDING;
  state.request=*request;state.admitted.store(true);
  *table={sizeof(*table),EDVR_RENDER_BOUNDARY_VERSION_1,&state,close,release};return S_OK;
}

namespace edvr::openxr::test {
namespace overlap_handoff_fixture {
struct EndBarrier {
  std::mutex mutex;std::condition_variable cv;
  bool blocked=false,entered=false,released=false;
  unsigned calls=0;XrResult result=XR_SUCCESS;
  void arm(XrResult outcome=XR_SUCCESS) {std::lock_guard<std::mutex> lock(mutex);blocked=true;entered=released=false;result=outcome;}
  bool awaitEntry() {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock,std::chrono::seconds(2),[&]{return entered;});
  }
  void release() {std::lock_guard<std::mutex> lock(mutex);released=true;cv.notify_all();}
} inline *active=nullptr;
inline XrResult XRAPI_PTR endFrame(XrSession,const XrFrameEndInfo*) {
  if(!active)return XR_SUCCESS;
  std::unique_lock<std::mutex> lock(active->mutex);++active->calls;
  if(active->blocked){active->entered=true;active->cv.notify_all();active->cv.wait(lock,[&]{return active->released;});}
  return active->result;
}
}
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
  overlap_handoff_fixture::EndBarrier endBarrier;
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
    overlap_handoff_fixture::active=&endBarrier;
    hostPtr=std::make_unique<NativeRuntimeHost>(owner,dispatcher,route);
    auto& host=*hostPtr;
    host.instance=instance();host.session=session();host.local=local();host.view=view();
    host.api.convertTime=convert;host.api.locateSpace=locate;host.api.locateViews=locateViews;
    const Dispatch dispatch{poll,beginSession,endSession,wait,beginFrame,overlap_handoff_fixture::endFrame};
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
    overlap_handoff_fixture::active=nullptr;
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
  FramePacing requestedPacing=FramePacing::Runtime;
  auto openFrame=[&]{
    h.clearOverlapHandoff(); // waitPoses clears admission before its owner invocation.
    h.timingRetire();
    h.timingSequence=h.timing.waitBegin();h.timingFrameActive=h.timingSequence!=0;
    h.timingApplicationSequence.store(h.timingSequence,std::memory_order_release);
    h.timingResetFrame(h.timingSequence);
    h.submitSample={};h.transferWall={};h.submitSample.sequence=h.timingSequence;h.submitCallbacksBegin=h.graphicsCalls.calls;
    h.temporalFrameEyes=0;h.frameWithheld=false;h.frameDecisionReady=false;
    if(h.boundary.waitAndBegin(requestedPacing)!=XR_SUCCESS)return false;
    h.boundary.setGeometryReady(true);
    h.frameSpace=h.seated.space();h.frameGeometryAvailable=true;
    return true;
  };

  // Run the actual registered NativeRenderBinding callback, not a copy of
  // its frame-work policy. The ABI fixture above only delivers registration.
  binding_fixture::state.device=producer.Get();binding_fixture::state.context=producerContext.Get();
  wchar_t providerPath[MAX_PATH]{};GetModuleFileNameW(nullptr,providerPath,MAX_PATH);
  NativeRenderBinding presentBinding;
  unsigned presentFallbacks=0;
  check(presentBinding.acquire(providerPath,[&]{
    if(h.consumeOverlappedPresent())return;
    ++presentFallbacks;
    if(!f.route.invoke([&]{h.loadingBoundary();}))throw std::runtime_error("fixture loading boundary unavailable");
  })==S_OK,"actual NativeRenderBinding registers its frame callback");
  f.route.present=&presentBinding.work();
  check(binding_fixture::present()==S_OK&&presentBinding.waitForRender()&&presentFallbacks==1,
    "no-Submit Present retains synchronous loading notification");

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
  check(!h.consumeOverlappedPresent(),"overlap-off pair retains synchronous Present loading work");
  unsigned publishCount=0;for(const auto& e:timing_fixture::state.log)if(e.tag=="publishCpu")++publishCount;
  check(publishCount==1&&!timing_fixture::state.log.empty()&&timing_fixture::state.log[0].tag!="publishCpu",
    "frame_end_overlap (d): the synchronous path still publishes exactly once, and not as its first call -- "
    "publishSubmitTimingIfComplete, unchanged, still polls device timing ahead of it");

  check(h.handoff(h.compositorGeneration),"overlap disabled keeps the completed pair's synchronous handoff");
  check(h.overlapHandoffAccepted.load()==0,"overlap disabled cannot publish async handoff admission");
  check(!h.handoff(h.compositorGeneration+1),"handoff rejects a stale compositor generation");

  // Hold the actual owner's xrEndFrame, rather than a synthetic queue sleep.
  // The caller must finish PostPresentHandoff and some CPU work before that
  // end is released. A failed assertion still releases and joins every thread.
  h.frameEndOverlapEnabled=true;
  bool thirdWaitOk=false;
  check(f.owner.invoke([&]{thirdWaitOk=openFrame();}),"handoff overlap wait admitted on owner");
  check(thirdWaitOk&&!h.handoff(h.compositorGeneration),"handoff still rejects an open frame");
  h.frameWithheld=true;f.endBarrier.arm();
  check(h.submitEye(h.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "handoff overlap first eye submits");
  check(h.submitEye(h.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "handoff overlap second eye releases the caller");
  check(f.endBarrier.awaitEntry(),"handoff fixture reached the real blocked xrEndFrame");
  std::mutex returnedMutex;std::condition_variable returnedCv;bool returned=false;unsigned cpuWork=0;
  OpenVRCompositor compositor(&h);
  std::thread producerWork([&]{
    compositor.PostPresentHandoff();
    unsigned work=0;for(unsigned i=0;i<256;++i)work+=i;
    {std::lock_guard<std::mutex> lock(returnedMutex);cpuWork=work;returned=true;returnedCv.notify_all();}
  });
  bool overlapped=false;
  {std::unique_lock<std::mutex> lock(returnedMutex);overlapped=returnedCv.wait_for(lock,std::chrono::seconds(2),[&]{return returned;});}
  check(overlapped&&cpuWork==32640,"PostPresentHandoff and caller CPU work finish while xrEndFrame is blocked");
  std::mutex presentReturnedMutex;std::condition_variable presentReturnedCv;bool presentReturned=false,returnedWhileHeld=false;
  std::thread releaseStuckPresent([&]{
    std::unique_lock<std::mutex> lock(presentReturnedMutex);
    returnedWhileHeld=presentReturnedCv.wait_for(lock,std::chrono::seconds(2),[&]{return presentReturned;});
    if(!returnedWhileHeld)f.endBarrier.release(); // bounded failure instead of a hung rig
  });
  const auto fallbackBefore=presentFallbacks;
  const auto callbackResult=binding_fixture::present();
  unsigned presentCpuWork=0;for(unsigned i=0;i<256;++i)presentCpuWork+=i;
  {std::lock_guard<std::mutex> lock(presentReturnedMutex);presentReturned=true;presentReturnedCv.notify_all();}
  releaseStuckPresent.join();
  check(callbackResult==S_OK&&returnedWhileHeld&&presentCpuWork==32640&&presentFallbacks==fallbackBefore,
    "actual Present binding callback and caller CPU work return before blocked xrEndFrame");
  check(!h.consumeOverlappedPresent(),"the Present bypass token is one-use and independent of handoff");
  bool nextWaitOk=false,finishBeforeWait=false;
  std::thread nextWait([&]{
    h.clearOverlapHandoff();
    f.owner.invoke([&]{finishBeforeWait=!h.pendingFrameEndFinish&&h.compositorHandoffs>=2;nextWaitOk=openFrame();});
  });
  const auto queuedUntil=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(f.owner.pending()<2&&std::chrono::steady_clock::now()<queuedUntil)std::this_thread::yield();
  check(f.owner.pending()>=2,"next wait queues behind the asynchronous handoff and frame finish");
  f.endBarrier.release();producerWork.join();nextWait.join();
  check(finishBeforeWait&&nextWaitOk,"frame finish and handoff complete before the next frame is admitted");
  check(h.overlapHandoffAccepted.load()==1&&h.overlapHandoffCompleted.load()==1&&h.overlapHandoffInvalid.load()==0,
    "asynchronous handoff completed with owner validation");
  check(!h.handoff(h.compositorGeneration),"the next open frame cannot reuse consumed handoff admission");
  check(f.owner.invoke([&]{h.boundary.clear();}),"handoff fixture closes its last admitted frame");

  // A later caller can invalidate admission while the prior Submit's owner
  // job is still queued. Do not let that delayed job republish eligibility.
  // This exercises the admission protocol only, without claiming concurrent
  // WaitGetPoses/Submit frame semantics that the existing runtime does not have.
  check(f.owner.invoke([&]{check(openFrame(),"delayed publication frame admitted");}),"delayed publication wait invoked");
  h.frameWithheld=true;
  check(h.submitEye(h.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "delayed publication first eye submits");
  overlap_handoff_fixture::EndBarrier ownerAdmission;
  check(f.owner.submit([&]{
    std::unique_lock<std::mutex> lock(ownerAdmission.mutex);ownerAdmission.entered=true;ownerAdmission.cv.notify_all();
    ownerAdmission.cv.wait(lock,[&]{return ownerAdmission.released;});
  })&&ownerAdmission.awaitEntry(),"owner barrier holds the later second Submit before publication");
  bool laterAdmissionObserved=false;
  std::thread laterAdmission([&]{
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!f.owner.pending()&&std::chrono::steady_clock::now()<until)std::this_thread::yield();
    laterAdmissionObserved=f.owner.pending()!=0;
    h.clearOverlapHandoff();ownerAdmission.release();
  });
  check(h.submitEye(h.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "delayed publication second Submit remains valid after caller admission invalidation");
  laterAdmission.join();
  check(laterAdmissionObserved&&f.owner.invoke([]{}),"later caller admission precedes delayed owner publication");
  check(h.handoff(h.compositorGeneration)&&h.overlapHandoffAccepted.load()==1,
    "invalidated Submit epoch cannot resurrect asynchronous handoff admission");
  check(!h.consumeOverlappedPresent(),"delayed publication cannot resurrect a stale Present token");

  // Loading-policy changes must invalidate a real pair's token before their
  // public calls can enqueue. Exercise the production callbacks afterwards.
  auto policyPair=[&]{
    bool opened=false;
    if(!f.owner.invoke([&]{opened=openFrame();})||!opened)return false;
    h.frameWithheld=true;
    return h.submitEye(h.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None&&
      h.submitEye(h.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None&&f.owner.invoke([]{});
  };
  check(policyPair(),"clearSubmitted transition starts from a real deferred pair");
  const auto emptyBefore=h.loadingEmpty;const auto policyFallbackBefore=presentFallbacks;
  h.clearSubmitted(h.compositorGeneration);
  check(binding_fixture::present()==S_OK&&presentFallbacks==policyFallbackBefore+1&&h.loadingEmpty==emptyBefore+1,
    "clearSubmitted invalidation preserves actual Present's synchronous empty loading frame");
  check(policyPair()&&h.clearSkybox(h.compositorGeneration),"clearSkybox follows a real deferred pair");
  const auto clearFallbackBefore=presentFallbacks;
  check(binding_fixture::present()==S_OK&&presentFallbacks==clearFallbackBefore+1,
    "clearSkybox policy change preserves synchronous Present loading notification");
  check(policyPair()&&h.setSkybox(h.compositorGeneration+1,nullptr,0)==vr::VRCompositorError_InvalidTexture,
    "invalid-generation skybox request cannot preserve a prior pair token");
  const auto setFallbackBefore=presentFallbacks;
  check(binding_fixture::present()==S_OK&&presentFallbacks==setFallbackBefore+1,
    "setSkybox caller invalidates admission before owner validation");

  // Borrowed-device capture and turbo each execute a real pair, so their
  // unchanged synchronous path is tested rather than just its config flag.
  check(f.owner.invoke([&]{check(h.captured.shutdownShared()==S_OK&&h.captured.initialize(producer.Get())==S_OK,
    "handoff fixture changes to borrowed-device capture");}),"borrowed capture initialized on owner");
  h.startupOptions.separateDevice=false;
  check(f.owner.invoke([&]{check(openFrame(),"borrowed-device frame admitted");}),"borrowed wait invoked");
  h.frameWithheld=true;
  check(h.submitEye(h.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None&&
    h.submitEye(h.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "borrowed-device pair stays synchronous");
  check(h.handoff(h.compositorGeneration)&&h.overlapHandoffAccepted.load()==1,
    "borrowed-device handoff does not use async admission");
  check(!h.consumeOverlappedPresent(),"borrowed-device pair has no Present bypass");
  h.startupOptions.separateDevice=true;
  check(f.route.invoke([&]{h.captured.shutdown();check(h.captured.initializeShared(producer.Get(),consumer.Get(),&h.graphicsCalls)==S_OK,
    "handoff fixture restores separate capture");}),"separate capture restored on owner");
  check(f.owner.invoke([&]{h.pacer.bind(launch_fixture::wait,launch_fixture::session());}),"turbo pacer bound");
  requestedPacing=FramePacing::Deferred;
  check(f.owner.invoke([&]{check(openFrame()&&h.boundary.turbo(),"turbo frame admitted");}),"turbo wait invoked");
  h.frameWithheld=true;
  check(h.submitEye(h.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None&&
    h.submitEye(h.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "turbo pair stays synchronous");
  check(h.handoff(h.compositorGeneration)&&h.overlapHandoffAccepted.load()==1,"turbo handoff remains synchronous");
  check(!h.consumeOverlappedPresent(),"turbo pair has no Present bypass");
  check(f.owner.invoke([&]{h.boundary.drain();h.pacer.bind(nullptr,XR_NULL_HANDLE);}),"turbo pacer drained and unbound");
  requestedPacing=FramePacing::Runtime;

  check(f.owner.invoke([&]{check(openFrame(),"queue rejection frame admitted");}),"queue rejection wait invoked");
  h.frameWithheld=true;f.endBarrier.arm();
  check(h.submitEye(h.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None&&
    h.submitEye(h.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "queue rejection pair releases its caller");
  check(f.endBarrier.awaitEntry(),"queue rejection holds the owner in xrEndFrame");
  bool filled=true;for(size_t i=0;i<OwnerService::kQueueCapacity;++i)filled=f.owner.submit([]{})&&filled;
  check(filled&&!h.handoff(h.compositorGeneration)&&h.overlapHandoffRejected.load()==1,
    "full owner queue rejects async handoff without losing deferred finish");
  f.endBarrier.release();
  const auto drainedUntil=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(f.owner.pending()&&std::chrono::steady_clock::now()<drainedUntil)std::this_thread::yield();
  check(f.owner.invoke([&]{check(!h.pendingFrameEndFinish&&!h.state.frameOpen(),"rejected handoff still finishes its frame");}),
    "queue rejection fixture drained");

  // stop cancels the queued handoff while the already-running finish remains
  // blocked. Its completion callback runs before the owner finalizer, and the
  // host is retained until stop joins both active work and finalization.
  check(f.owner.invoke([&]{check(openFrame(),"shutdown frame admitted");}),"shutdown wait invoked");
  h.frameWithheld=true;f.endBarrier.arm();
  check(h.submitEye(h.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None&&
    h.submitEye(h.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "shutdown pair releases its caller");
  check(f.endBarrier.awaitEntry()&&h.handoff(h.compositorGeneration),"shutdown accepts a queued async handoff");
  bool stopped=false,shutdownFinished=false;
  std::thread shutdown([&]{stopped=f.owner.stop([&]{
    h.clearOverlapHandoff();h.finishPendingFrameEnd();
    shutdownFinished=!h.pendingFrameEndFinish&&!h.state.frameOpen();
    check(h.captured.shutdownShared()==S_OK,"frame_end_overlap shared transfer retires in owner finalizer");
  });});
  const auto cancelledUntil=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!h.overlapHandoffCancelledOrFailed.load()&&std::chrono::steady_clock::now()<cancelledUntil)std::this_thread::yield();
  check(h.overlapHandoffCancelledOrFailed.load()==1,"shutdown cancels and accounts the queued handoff");
  f.endBarrier.release();shutdown.join();
  check(stopped&&shutdownFinished,"owner shutdown finishes the active pair before releasing host resources");
  check(h.overlapHandoffAccepted.load()==2&&h.overlapHandoffCompleted.load()==1&&
    h.overlapHandoffInvalid.load()==0&&!h.handoff(h.compositorGeneration),"stopped owner cannot accept another handoff");
  check(!h.consumeOverlappedPresent(),"shutdown clears an unconsumed Present token");
  check(presentBinding.close()==S_OK&&presentBinding.release()==S_OK,"actual Present binding retires after owner shutdown");
  f.route.present=nullptr;binding_fixture::state.device=nullptr;binding_fixture::state.context=nullptr;
  h.fss.close();h.temporal.close();h.sharpen.close();h.menu.close();h.timing.close();

  // Also cancel the deferred finish BEFORE it starts. Hold the active second
  // Submit at CPU publication, after finish was queued, and stop the service.
  // Its finalizer must observe and complete the retained pending pair inline.
  OwnerBuiltFixture cancelled;
  cancelled.fake.shouldRender=true;
  check(cancelled.route.bind()&&cancelled.owner.start()&&cancelled.owner.invoke([&]{cancelled.build();}),
    "cancelled finish owner constructed");
  auto& cancelledHost=*cancelled.hostPtr;
  cancelledHost.startupOptions.separateDevice=true;cancelledHost.externalDevice=producer.Get();
  check(cancelledHost.timing.acquire(provider,producer.Get(),1)==S_OK&&
    cancelled.route.invoke([&]{check(cancelledHost.captured.initializeShared(producer.Get(),consumer.Get(),&cancelledHost.graphicsCalls)==S_OK,
      "cancelled finish shared capture initialized");}),"cancelled finish timing acquired");
  check(cancelled.owner.invoke([&]{
    cancelledHost.timingSequence=cancelledHost.timing.waitBegin();cancelledHost.timingFrameActive=true;
    cancelledHost.timingApplicationSequence.store(cancelledHost.timingSequence);
    cancelledHost.timingResetFrame(cancelledHost.timingSequence);
    cancelledHost.submitSample={};cancelledHost.transferWall={};
    cancelledHost.submitSample.sequence=cancelledHost.timingSequence;
    cancelledHost.submitCallbacksBegin=cancelledHost.graphicsCalls.calls;
    check(cancelledHost.boundary.waitAndBegin()==XR_SUCCESS,"cancelled finish frame admitted");
    cancelledHost.boundary.setGeometryReady(true);cancelledHost.frameWithheld=true;
    cancelledHost.frameSpace=cancelledHost.seated.space();cancelledHost.frameGeometryAvailable=true;
  }),"cancelled finish wait invoked");
  check(cancelledHost.submitEye(cancelledHost.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "cancelled finish first eye submits");
  overlap_handoff_fixture::EndBarrier publication;
  timing_fixture::state.beforePublish=[&]{
    std::unique_lock<std::mutex> lock(publication.mutex);publication.entered=true;publication.cv.notify_all();
    publication.cv.wait(lock,[&]{return publication.released;});
  };
  bool cancelledStopped=false,sawPendingFallback=false,completedFallback=false,cancelledCaptureRetired=false;
  std::thread cancelFinish([&]{
    const bool entered=publication.awaitEntry();
    std::thread releasePublication([&]{
      const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      while(cancelled.owner.running()&&std::chrono::steady_clock::now()<until)std::this_thread::yield();
      publication.release();
    });
    cancelledStopped=cancelled.owner.stop([&]{
      sawPendingFallback=entered&&cancelledHost.pendingFrameEndFinish;
      cancelledHost.clearOverlapHandoff();cancelledHost.finishPendingFrameEnd();
      completedFallback=!cancelledHost.pendingFrameEndFinish&&!cancelledHost.state.frameOpen();
      cancelledCaptureRetired=cancelledHost.captured.shutdownShared()==S_OK;
    });
    releasePublication.join();
  });
  check(cancelledHost.submitEye(cancelledHost.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None,
    "cancelled finish active Submit returns without dangling publication references");
  cancelFinish.join();timing_fixture::state.beforePublish={};
  check(cancelledCaptureRetired,"cancelled finish capture retires after fallback");
  check(cancelledStopped&&sawPendingFallback&&completedFallback,
    "shutdown finalizer completes the cancelled queued frame finish before releasing resources");
  cancelledHost.timing.close();

  // A failed deferred finish retires a still-unconsumed Present token through
  // its existing fatal publication, so it cannot authorize a later bypass.
  OwnerBuiltFixture failed;
  check(failed.route.bind()&&failed.owner.start()&&failed.owner.invoke([&]{failed.build();}),
    "failed-finish actual host constructed");
  auto& failedHost=*failed.hostPtr;
  failedHost.startupOptions.separateDevice=true;failedHost.externalDevice=producer.Get();
  check(failed.route.invoke([&]{check(failedHost.captured.initializeShared(producer.Get(),consumer.Get(),&failedHost.graphicsCalls)==S_OK,
    "failed-finish shared producer capture initialized");}),"failed-finish capture rendezvous");
  check(failed.owner.invoke([&]{
    check(failedHost.boundary.waitAndBegin()==XR_SUCCESS,"failed-finish frame admitted");
    failedHost.boundary.setGeometryReady(true);failedHost.frameWithheld=true;
    failedHost.frameSpace=failedHost.seated.space();failedHost.frameGeometryAvailable=true;
  }),"failed-finish frame owner invocation");
  failed.endBarrier.arm(XR_ERROR_RUNTIME_FAILURE);
  check(failedHost.submitEye(failedHost.compositorGeneration,vr::Eye_Left,&leftTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None&&
    !failedHost.consumeOverlappedPresent(),"partial pair cannot bypass Present loading work");
  check(failedHost.submitEye(failedHost.compositorGeneration,vr::Eye_Right,&rightTexture,nullptr,vr::Submit_Default)==vr::VRCompositorError_None&&
    failed.endBarrier.awaitEntry(),"failed finish is deferred through actual captured stereo pair");
  bool failureTokenPublished=false;
  {std::lock_guard<std::mutex> lock(failedHost.overlapHandoffMutex);
    failureTokenPublished=failedHost.overlapPresentAvailable&&failedHost.overlapPresentEpoch==failedHost.overlapHandoffEpoch;}
  check(failureTokenPublished,"valid deferred pair publishes an unconsumed token before its XR result");
  failed.endBarrier.release();
  check(failed.owner.invoke([]{})&&failedHost.serviceFailed&&!failedHost.consumeOverlappedPresent(),
    "deferred XR failure publishes fatal and retires Present admission");
  check(failed.owner.stop([&]{check(failedHost.captured.shutdownShared()==S_OK,"failed-finish shared capture retires on owner");}),
    "failed-finish service joins before host fixture destruction");
  treatment_fixture::state={};timing_fixture::state.log.clear();
}
} // namespace edvr::openxr::test
