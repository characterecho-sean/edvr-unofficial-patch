#include "../../src/openxr/frame_boundary.h"
#include "../../src/openxr/runtime_gate.h"
#include "../../src/openxr/system_publication.h"
#include "../../src/openxr/loading_state.h"

#include <condition_variable>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace edvr::openxr;
namespace {
unsigned checks = 0, failures = 0;
void check(bool value, const char* text) { ++checks; if (!value) { ++failures; std::printf("FAIL: %s\n", text); } }

struct Fake {
  std::vector<XrResult> waits;
  XrResult beginResult = XR_SUCCESS, endResult = XR_SUCCESS, composeResult = XR_SUCCESS;
  vr::EVRCompositorError captureResult=vr::VRCompositorError_None;
  bool malformedLayer=false;
  std::vector<char> trace;
  bool shouldRender = true;
  bool readyPending = true;
  bool blockWait = false, waitEntered = false, releaseWait = false, waitTimedOut = false;
  std::mutex waitMutex;
  std::condition_variable waitCv;
  unsigned waitCalls = 0, beginCalls = 0, endCalls = 0, captureCalls = 0, composeCalls = 0;
  unsigned backgroundCalls = 0;
  XrTime displayTime = 100;
  bool lastHadLayers = false;
  XrTime lastDisplayTime = 0;
  static Fake* current;
  static XrResult poll(XrInstance, XrEventDataBuffer* out) {
    if (!current->readyPending) { *out = {XR_TYPE_EVENT_DATA_BUFFER}; return XR_EVENT_UNAVAILABLE; }
    current->readyPending = false;
    XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    event.session = reinterpret_cast<XrSession>(2); event.state = XR_SESSION_STATE_READY;
    std::memcpy(out, &event, sizeof(event)); return XR_SUCCESS;
  }
  static XrResult beginSession(XrSession, const XrSessionBeginInfo*) {
    ++current->beginCalls; return current->beginResult;
  }
  static XrResult endSession(XrSession) { return XR_SUCCESS; }
  static XrResult wait(XrSession, const XrFrameWaitInfo*, XrFrameState* out) {
    current->trace.push_back('W');
    ++current->waitCalls;
    if (current->blockWait) {
      std::unique_lock<std::mutex> lock(current->waitMutex); current->waitEntered = true; current->waitCv.notify_all();
      if (!current->waitCv.wait_for(lock, std::chrono::seconds(2), [] { return current->releaseWait; })) {
        current->waitTimedOut = true; return XR_ERROR_RUNTIME_FAILURE;
      }
    }
    *out = {XR_TYPE_FRAME_STATE}; out->predictedDisplayTime = current->displayTime++;
    out->predictedDisplayPeriod = 11; out->shouldRender = current->shouldRender ? XR_TRUE : XR_FALSE;
    if (!current->waits.empty()) { const auto r = current->waits.front(); current->waits.erase(current->waits.begin()); return r; }
    return XR_SUCCESS;
  }
  static XrResult beginFrame(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }
  static XrResult endFrame(XrSession, const XrFrameEndInfo* info) {
    current->trace.push_back('E');
    ++current->endCalls; current->lastHadLayers = info->layerCount != 0; current->lastDisplayTime = info->displayTime; return current->endResult;
  }
  Dispatch dispatch() {
    current = this; return {poll, beginSession, endSession, wait, beginFrame, endFrame};
  }
};
Fake* Fake::current = nullptr;

struct Sink : FrameSink {
  Fake& fake; std::vector<vr::EVREye> eyes; std::vector<bool> copies;
  explicit Sink(Fake& f) : fake(f) {}
  vr::EVRCompositorError capture(vr::EVREye eye, const vr::Texture_t*, const vr::VRTextureBounds_t*,
      vr::EVRSubmitFlags, bool copyPixels) override {
    ++fake.captureCalls; eyes.push_back(eye); copies.push_back(copyPixels); return fake.captureResult;
  }
  XrResult compose(XrCompositionLayerProjection& layer) override {
    ++fake.composeCalls;
    layer.space = reinterpret_cast<XrSpace>(3); layer.viewCount = 2;
    static XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    layer.views = fake.malformedLayer?nullptr:views; return fake.composeResult;
  }
  XrResult composeBackground(XrCompositionLayerProjection& layer) override {
    ++fake.backgroundCalls;const auto r=compose(layer);--fake.composeCalls;return r;
  }
};

bool start(SessionState& session, Fake& fake) {
  if (session.reset(fake.dispatch(), reinterpret_cast<XrInstance>(1), reinterpret_cast<XrSession>(2), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != XR_SUCCESS) return false;
  return session.pollEvents()==XR_SUCCESS && session.startIfReady()==XR_SUCCESS;
}

void gateTest() {
  RuntimeGate gate;
  static_assert(!std::is_copy_constructible<RuntimeGate::Lease>::value, "lease must not copy");
  const auto g = gate.beginGeneration(); auto lease = gate.tryEnter(g); RuntimeGate::Lease moved(std::move(lease));
  check(!lease && moved, "move constructor transfers lease");
  moved = std::move(moved); check(static_cast<bool>(moved), "self move preserves lease");
  check(gate.requestStop(g) && !gate.canDestroy(g), "stop waits for moved lease");
  moved = {}; check(gate.canDestroy(g) && gate.finishGeneration(g), "release permits finish");
  const auto n = gate.beginGeneration(); check(n > g && !gate.tryEnter(g), "stale generation rejected after reinit");
  auto active = gate.tryEnter(n); RuntimeGate other; const auto og = other.beginGeneration(); auto otherLease = other.tryEnter(og);
  active = std::move(otherLease); check(!otherLease && active, "move assignment releases old and takes new");
  check(gate.canDestroy(n) == false, "old gate lease release does not affect replacement");
  check(other.requestStop(og) && !other.canDestroy(og), "replacement lease remains owned by destination gate");
  active = {}; check(other.canDestroy(og) && other.finishGeneration(og), "replacement lease releases correctly");
  gate.requestStop(n); check(gate.canDestroy(n) && gate.finishGeneration(n), "old gate can finish after move out");
}

void boundedBlockedGateTest() {
  RuntimeGate gate; const auto g = gate.beginGeneration(); std::mutex mutex; std::condition_variable cv;
  bool entered = false, unblock = false, woke=false;
  std::thread worker([&] { auto lease = gate.tryEnter(g); { std::lock_guard<std::mutex> l(mutex); entered = true; cv.notify_one(); }
    std::unique_lock<std::mutex> l(mutex); woke=cv.wait_for(l, std::chrono::seconds(2), [&] { return unblock; }); });
  { std::unique_lock<std::mutex> l(mutex); check(cv.wait_for(l, std::chrono::seconds(2), [&] { return entered; }), "worker entered gate"); }
  check(gate.requestStop(g) && !gate.canDestroy(g), "shutdown returns before blocked wait");
  { std::lock_guard<std::mutex> l(mutex); unblock = true; cv.notify_one(); } worker.join();
  check(woke,"blocked fake wait unblocks");
  check(gate.canDestroy(g) && gate.finishGeneration(g), "blocked gate eventually tears down");
}

void frameBoundaryTest() {
  Fake fake; SessionState session;
  check(session.reset(fake.dispatch(), reinterpret_cast<XrInstance>(1), reinterpret_cast<XrSession>(2), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == XR_SUCCESS,
        "session reset for boundary");
  check(session.pollEvents() == XR_SUCCESS && session.startIfReady() == XR_SUCCESS, "ready session starts");
  Sink sink(fake); FrameBoundary boundary(session, sink); vr::Texture_t texture{};
  check(boundary.waitAndBegin() == XR_SUCCESS, "wait begins frame");
  const auto predicted = boundary.frame().predictedDisplayTime; boundary.setGeometryReady(true);
  check(boundary.submit(vr::Eye_Right, &texture) == vr::VRCompositorError_None, "right submit first");
  check(boundary.submit(vr::Eye_Left, &texture) == vr::VRCompositorError_None, "left submit completes pair");
  check(fake.captureCalls == 2 && fake.composeCalls == 1 && fake.endCalls == 1 && fake.lastHadLayers && fake.lastDisplayTime == predicted,
        "pair composes once with immutable predicted time");
  check(boundary.submit(vr::Eye_Left, &texture) == vr::VRCompositorError_InvalidTexture, "duplicate eye rejected");

  fake.shouldRender = false;
  check(boundary.waitAndBegin() == XR_SUCCESS, "next wait begins frame");
  check(boundary.submit(vr::Eye_Left, &texture) == vr::VRCompositorError_None, "one eye accepted");
  const auto endsBefore = fake.endCalls; check(boundary.waitAndBegin() == XR_SUCCESS && fake.endCalls == endsBefore + 1 && !fake.lastHadLayers,
        "next wait closes incomplete frame empty");
  boundary.setGeometryReady(true); const auto composeBefore = fake.composeCalls;
  check(boundary.submit(vr::Eye_Left, &texture) == vr::VRCompositorError_None && boundary.submit(vr::Eye_Right, &texture) == vr::VRCompositorError_None,
        "no render pair accepted");
  check(fake.composeCalls == composeBefore, "no render skips compose");
  check(!fake.lastHadLayers && !sink.copies.back(), "no render ends empty and skips copies");

  std::atomic<unsigned> beforeWrong{fake.waitCalls + fake.captureCalls + fake.endCalls};
  std::thread wrong([&] { check(boundary.waitAndBegin() == XR_ERROR_CALL_ORDER_INVALID, "wrong thread wait rejected");
    check(boundary.clear() == XR_ERROR_CALL_ORDER_INVALID, "wrong thread clear rejected");
    check(boundary.submit(vr::Eye_Left, &texture) == vr::VRCompositorError_InvalidTexture, "wrong thread submit rejected"); });
  wrong.join(); check(beforeWrong == fake.waitCalls + fake.captureCalls + fake.endCalls, "wrong thread makes no XR or sink calls");
}

void blockedRuntimeIntegrationTest() {
  Fake fake; SessionState session;
  check(session.reset(fake.dispatch(), reinterpret_cast<XrInstance>(1), reinterpret_cast<XrSession>(2), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == XR_SUCCESS,
        "blocked session reset");
  check(session.pollEvents() == XR_SUCCESS && session.startIfReady() == XR_SUCCESS, "blocked session starts");
  SystemPublication publication; SystemRead metadata{}; metadata.connected = true;
  const auto publicationGeneration = publication.begin(metadata); check(publicationGeneration != 0 && publication.read().connected, "cached publication available");
  RuntimeGate gate; const auto gateGeneration = gate.beginGeneration(); fake.blockWait = true;
  std::atomic<bool> workerFailed{false};
  std::thread worker([&] {
    auto lease = gate.tryEnter(gateGeneration); Sink sink(fake); FrameBoundary boundary(session, sink);
    if (!lease || boundary.waitAndBegin() != XR_SUCCESS || boundary.clear() != XR_SUCCESS) workerFailed = true;
  });
  { std::unique_lock<std::mutex> lock(fake.waitMutex); check(fake.waitCv.wait_for(lock, std::chrono::seconds(2), [&] { return fake.waitEntered; }), "real wait call blocks under lease"); }
  const auto cached = publication.read(); check(cached.connected && !gate.canDestroy(gateGeneration), "stop cannot destroy while XR wait blocks");
  check(gate.requestStop(gateGeneration) && !gate.canDestroy(gateGeneration) &&
        !gate.tryEnter(gateGeneration) && !gate.finishGeneration(gateGeneration), "main stop blocks entry and teardown during XR wait");
  { std::lock_guard<std::mutex> lock(fake.waitMutex); fake.releaseWait = true; fake.waitCv.notify_all(); }
  worker.join(); check(!workerFailed && !fake.waitTimedOut && fake.endCalls == 1, "blocked wait resumes and closes frame");
  check(gate.canDestroy(gateGeneration) && gate.finishGeneration(gateGeneration), "gate finishes after blocked operation");
}

void loadingTransitions() {
  Fake fake;SessionState state;check(start(state,fake),"loading transition session");Sink sink(fake);FrameBoundary b(state,sink);LoadingState loading;
  check(loading.work(false)==LoadingWork::None,"no invented initial loading work");
  loading.overrideSet();check(loading.work(false)==LoadingWork::Skybox,"first override activates startup loading");
  check(b.waitAndBegin()==XR_SUCCESS,"startup loading wait");b.setGeometryReady(true);
  check(b.background()==XR_SUCCESS&&fake.lastHadLayers&&!fake.captureCalls,"startup background is its own frame");
  loading.overrideCleared();loading.overrideCleared();
  check(loading.work(false)==LoadingWork::Empty,"repeated clear retains single empty-frame demand");
  check(b.waitAndBegin()==XR_SUCCESS&&b.clear()==XR_SUCCESS&&!fake.lastHadLayers,"clear removes last projection with zero-layer frame");
  const auto ends=fake.endCalls;loading.emptyCompleted();
  check(loading.work(false)==LoadingWork::None&&b.clear()==XR_SUCCESS&&fake.endCalls==ends,"completed clear cannot generate repeated empty frames");
  loading.overrideSet();check(b.waitAndBegin()==XR_SUCCESS,"game begins while startup skybox available");loading.sceneWaited();b.setGeometryReady(true);
  check(loading.work(state.frameOpen())==LoadingWork::None,"open game frame blocks idle skybox work");
  check(b.submit(vr::Eye_Left,nullptr)==vr::VRCompositorError_None&&loading.sceneSubmitted(),"first accepted scene eye leaves loading");
  check(b.background()==XR_ERROR_CALL_ORDER_INVALID,"partial game pair cannot become loading frame");
  check(b.submit(vr::Eye_Right,nullptr)==vr::VRCompositorError_None&&!loading.sceneSubmitted()&&fake.lastHadLayers,"scene pair completes without duplicate transition");
  loading.overrideSet();check(loading.work(false)==LoadingWork::None,"later override replacement does not interrupt scene");
  loading.sceneCleared();check(loading.work(false)==LoadingWork::Skybox,"explicit scene clear reactivates stored override");
  loading.overrideCleared();check(loading.work(false)==LoadingWork::Empty,"clear loading override removes stale last image");
  check(b.waitAndBegin()==XR_SUCCESS,"game takes priority over pending clear");
  check(loading.work(state.frameOpen())==LoadingWork::None,"pending clear cannot consume game frame");loading.sceneWaited();b.setGeometryReady(true);
  check(b.submit(vr::Eye_Left,nullptr)==vr::VRCompositorError_None&&b.submit(vr::Eye_Right,nullptr)==vr::VRCompositorError_None,"game completes after pending clear");loading.sceneSubmitted();
  check(loading.work(false)==LoadingWork::None,"old clear demand cannot blank new scene");
  loading.sceneCleared();check(loading.work(false)==LoadingWork::Empty,"no-override scene clear requests defined zero layer");
  loading.overrideSet();check(loading.work(false)==LoadingWork::None,"replacement after scene clears obsolete empty demand");
  loading.sceneCleared();loading.overrideCleared();loading.overrideSet();
  check(loading.work(false)==LoadingWork::None,"later replacement remains stored until explicit scene clear");
  loading={};loading.overrideSet();loading.overrideCleared();loading.overrideSet();
  check(loading.work(false)==LoadingWork::Skybox,"startup replacement cancels obsolete empty demand");
  loading={};check(loading.work(false)==LoadingWork::None,"retired loading state has no work");
}
void backgroundFrames() {
  Fake fake;SessionState state;check(start(state,fake),"background session starts");Sink sink(fake);FrameBoundary b(state,sink);
  check(b.background()==XR_ERROR_CALL_ORDER_INVALID&&!fake.endCalls,"background needs begun frame");
  for(unsigned variant=0;variant<3;++variant) {
    fake.shouldRender=variant!=1;check(b.waitAndBegin()==XR_SUCCESS,"background wait begins");b.setGeometryReady(variant!=2);
    const auto endCount=fake.endCalls,drawCount=fake.backgroundCalls;
    check(b.background()==XR_SUCCESS&&fake.endCalls==endCount+1&&fake.lastHadLayers==(variant==0)&&
      fake.backgroundCalls==drawCount+(variant==0?1:0)&&!fake.captureCalls&&!fake.composeCalls,"loading only composes with valid render geometry, without eye copies");
    check(b.background()==XR_ERROR_CALL_ORDER_INVALID&&b.clear()==XR_SUCCESS&&fake.endCalls==endCount+1,"background end cannot repeat");
  }
  fake.shouldRender=true;check(b.waitAndBegin()==XR_SUCCESS,"partial scene begins");b.setGeometryReady(true);
  check(b.submit(vr::Eye_Left,nullptr)==vr::VRCompositorError_None,"scene first eye accepted");
  auto endCount=fake.endCalls,drawCount=fake.backgroundCalls;
  check(b.background()==XR_ERROR_CALL_ORDER_INVALID&&fake.endCalls==endCount&&fake.backgroundCalls==drawCount,"background cannot replace partial scene pair");
  check(b.submit(vr::Eye_Right,nullptr)==vr::VRCompositorError_None&&fake.composeCalls==1,"scene second eye still completes");
  check(b.waitAndBegin()==XR_SUCCESS,"owner rejection frame begins");b.setGeometryReady(true);endCount=fake.endCalls;
  XrResult wrong=XR_SUCCESS;std::thread other([&]{wrong=b.background();});other.join();
  check(wrong==XR_ERROR_CALL_ORDER_INVALID&&fake.endCalls==endCount&&fake.backgroundCalls==drawCount,"wrong thread cannot consume loading frame");
  check(b.background()==XR_SUCCESS,"owner retains loading frame after rejection");
  for(auto error:{XR_TIMEOUT_EXPIRED,XR_SESSION_LOSS_PENDING,XR_ERROR_SESSION_LOST}) {
    Fake failure;SessionState failed;check(start(failed,failure),"background failure fixture");Sink failingSink(failure);FrameBoundary boundary(failed,failingSink);
    check(boundary.waitAndBegin()==XR_SUCCESS,"background failure wait");boundary.setGeometryReady(true);failure.composeResult=error;
    check(boundary.background()==error&&failure.backgroundCalls==1&&!failure.captureCalls&&failure.endCalls==(XR_FAILED(error)?0u:1u)&&!failure.lastHadLayers,"background compose failure closes only known-live frame");
    const auto trace=failure.trace;
    check(boundary.background()==error&&boundary.waitAndBegin()==error&&failure.trace==trace,"uncertain loading operation never retried");failed.abandonAfterOwnerDestruction();
  }
  for(bool malformed:{false,true}) {
    Fake failure;SessionState failed;check(start(failed,failure),"background end fixture");Sink failingSink(failure);FrameBoundary boundary(failed,failingSink);
    check(boundary.waitAndBegin()==XR_SUCCESS,"background end wait");boundary.setGeometryReady(true);
    failure.malformedLayer=malformed;if(!malformed)failure.endResult=XR_ERROR_RUNTIME_FAILURE;
    check(boundary.background()!=XR_SUCCESS&&failure.endCalls==1&&(!malformed||!failure.lastHadLayers),"background malformed layer or end failure rejected once");
    check(boundary.clear()!=XR_SUCCESS&&failure.endCalls==1,"failed loading end never repeats");failed.abandonAfterOwnerDestruction();
  }
}
void boundaryFailures() {
  for(unsigned first=0;first<2;++first) {
    Fake fake;SessionState state;check(start(state,fake),"pair fixture starts");Sink sink(fake);FrameBoundary b(state,sink);
    vr::Texture_t t{};
    check(b.submit(vr::Eye_Left,&t)==vr::VRCompositorError_InvalidTexture&&!fake.captureCalls,"submit before wait rejected");
    check(b.waitAndBegin()==XR_SUCCESS,"pair begins");b.setGeometryReady(true);
    fake.captureResult=vr::VRCompositorError_TextureIsOnWrongDevice;
    check(b.submit(vr::EVREye(first),&t)==fake.captureResult&&!fake.endCalls,"bad capture leaves frame open");
    fake.captureResult=vr::VRCompositorError_None;
    check(b.submit(vr::EVREye(first),&t)==vr::VRCompositorError_None,"rejected eye can retry");
    const auto captures=fake.captureCalls;
    check(b.submit(vr::EVREye(first),&t)==vr::VRCompositorError_InvalidTexture&&fake.captureCalls==captures,"duplicate cannot replace accepted capture");
    b.setGeometryReady(false); // pair metadata cannot change halfway through
    auto copied=b.frame();copied.predictedDisplayTime=-99;copied.shouldRender=false;
    check(b.submit(vr::EVREye(first^1),&t)==vr::VRCompositorError_None&&fake.lastHadLayers&&fake.lastDisplayTime==100,"either order retains frame metadata");
    check(b.clear()==XR_SUCCESS&&b.clear()==XR_SUCCESS&&fake.endCalls==1,"completed pair cannot end twice");
    check(b.waitAndBegin()==XR_SUCCESS&&b.waitAndBegin()==XR_SUCCESS&&fake.endCalls==2&&!fake.lastHadLayers&&
      fake.trace==std::vector<char>({'W','E','W','E','W'}),"no submit ends empty before following wait");
    b.setGeometryReady(false);
    check(b.submit(vr::Eye_Left,&t)==vr::VRCompositorError_None&&b.submit(vr::Eye_Right,&t)==vr::VRCompositorError_None&&
      !fake.lastHadLayers&&!sink.copies.back()&&fake.composeCalls==1,"invalid geometry validates but skips copies and compose");
  }
  for(const XrResult error:{XR_TIMEOUT_EXPIRED,XR_SESSION_LOSS_PENDING,XR_ERROR_SESSION_LOST}) {
    Fake fake;SessionState state;check(start(state,fake),"compose failure fixture");Sink sink(fake);FrameBoundary b(state,sink);
    check(b.waitAndBegin()==XR_SUCCESS,"compose failure frame begins");b.setGeometryReady(true);fake.composeResult=error;
    check(b.submit(vr::Eye_Left,nullptr)==vr::VRCompositorError_None&&b.submit(vr::Eye_Right,nullptr)==vr::VRCompositorError_InvalidTexture,"compose failure reported");
    check(fake.endCalls==(XR_FAILED(error)?0u:1u)&&!fake.lastHadLayers,"positive result ends empty; hard lost handle never ends");
    const auto trace=fake.trace;const auto captures=fake.captureCalls;
    check(b.clear()==error&&b.waitAndBegin()==error&&b.submit(vr::Eye_Left,nullptr)==vr::VRCompositorError_InvalidTexture&&
      fake.trace==trace&&fake.captureCalls==captures,"failed boundary never retries runtime or capture");
    state.abandonAfterOwnerDestruction();
  }
  for(bool malformed:{false,true}) {
    Fake fake;SessionState state;check(start(state,fake),"end failure fixture");Sink sink(fake);FrameBoundary b(state,sink);
    check(b.waitAndBegin()==XR_SUCCESS,"end failure frame begins");b.setGeometryReady(true);
    fake.malformedLayer=malformed;if(!malformed)fake.endResult=XR_ERROR_RUNTIME_FAILURE;
    check(b.submit(vr::Eye_Left,nullptr)==vr::VRCompositorError_None&&b.submit(vr::Eye_Right,nullptr)==vr::VRCompositorError_InvalidTexture&&fake.endCalls==1,"bad layer or end failure reported once");
    const auto trace=fake.trace;check(b.clear()!=XR_SUCCESS&&b.waitAndBegin()!=XR_SUCCESS&&fake.trace==trace,"uncertain end never retried");
    check(!malformed||!fake.lastHadLayers,"malformed projection becomes empty end");state.abandonAfterOwnerDestruction();
  }
  Fake fake;SessionState state;check(start(state,fake),"pending wait fixture");Sink sink(fake);FrameBoundary b(state,sink);
  fake.waits={XR_SESSION_LOSS_PENDING};check(b.waitAndBegin()==XR_SESSION_LOSS_PENDING,"wait pending preserved");b.setGeometryReady(true);
  check(b.submit(vr::Eye_Left,nullptr)==vr::VRCompositorError_None&&b.submit(vr::Eye_Right,nullptr)==vr::VRCompositorError_InvalidTexture&&
    fake.endCalls==1&&!fake.lastHadLayers&&!fake.composeCalls&&!sink.copies.back(),"pending loss honors begin/end without rendering");
  state.abandonAfterOwnerDestruction();
}
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) { std::puts("Would test injected OpenXR frame boundary; no runtime or files."); return 0; }
  if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
  gateTest(); boundedBlockedGateTest(); frameBoundaryTest(); blockedRuntimeIntegrationTest(); boundaryFailures();backgroundFrames();loadingTransitions();
  std::printf("openxr_frame_test: %u checks, %u failures\n", checks, failures); return failures ? 1 : 0;
}
