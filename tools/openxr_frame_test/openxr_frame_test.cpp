#include "../../src/openxr/frame_boundary.h"
#include "../../src/openxr/runtime_gate.h"
#include "../../src/openxr/system_publication.h"
#include "../../src/openxr/loading_state.h"
#include "../../src/openxr/frame_cycle_stats.h"

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
// atomic: turboPacingTest's fixtures construct FrameBoundary on a worker
// thread (its ownerThread() check requires every call on the thread that
// constructed it, exactly like blockedRuntimeIntegrationTest's fixture
// below) and check() runs there directly rather than only after a join.
std::atomic<unsigned> checks{0}, failures{0};
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
  // Monotonic count of blocking entries, plus turboPacingTest's own bookmark
  // of how much of it its awaitEntered() helper has consumed. waitEntered
  // alone cannot tell a fixture's Nth kick apart from its (N-1)th: release()
  // returning is not synchronized with the released Fake::wait call actually
  // waking up and resetting waitEntered, so a caller's very next wait for
  // "the next kick entered" can be satisfied by the previous kick's own
  // flag, still sitting true. A count only turboPacingTest increments/reads
  // has no such ambiguity: awaitEntered() waits for it to exceed what it has
  // already consumed, which is true only once, on a genuinely new entry, no
  // matter how the reset and the next kick happen to interleave.
  unsigned enterGeneration = 0, consumedEnterGeneration = 0;
  std::mutex waitMutex;
  std::condition_variable waitCv;
  unsigned waitCalls = 0, beginCalls = 0, endCalls = 0, captureCalls = 0, composeCalls = 0;
  unsigned backgroundCalls = 0;
  XrTime displayTime = 100;
  // How much each wait() advances displayTime, and the period it reports.
  // Defaults reproduce the old hardcoded displayTime++ / 11 for every
  // existing test; turboPacingTest sets step = period so real predictions
  // advance by one period per wait, as an actual runtime's do.
  XrTime step = 1;
  XrDuration period = 11;
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
      std::unique_lock<std::mutex> lock(current->waitMutex);
      current->waitEntered = true; ++current->enterGeneration; current->waitCv.notify_all();
      if (!current->waitCv.wait_for(lock, std::chrono::seconds(2), [] { return current->releaseWait; })) {
        current->waitTimedOut = true; return XR_ERROR_RUNTIME_FAILURE;
      }
      // Consume this release so a second (and third...) blocked call -- the
      // turbo pacer kicks and blocks repeatedly across one test -- waits for
      // its own fresh signal instead of sailing through on the last one's.
      // No existing single-use-per-Fake test ever reads these fields again
      // after its one release, so this is invisible to every one of them.
      current->waitEntered = false; current->releaseWait = false;
    }
    *out = {XR_TYPE_FRAME_STATE}; out->predictedDisplayTime = current->displayTime;
    current->displayTime += current->step;
    out->predictedDisplayPeriod = current->period; out->shouldRender = current->shouldRender ? XR_TRUE : XR_FALSE;
    if (!current->waits.empty()) { const auto r = current->waits.front(); current->waits.erase(current->waits.begin()); return r; }
    return XR_SUCCESS;
  }
  static XrResult beginFrame(XrSession, const XrFrameBeginInfo*) { current->trace.push_back('B'); return XR_SUCCESS; }
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
  bool available=true,finishedPixels=false;unsigned finished=0;XrResult finishedResult=XR_SUCCESS;
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
  bool sceneLayerAvailable() const override {return available;}
  void sceneFinished(bool pixels,XrResult result) override {++finished;finishedPixels=pixels;finishedResult=result;}
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
  fake.shouldRender=true;sink.available=false;
  check(boundary.waitAndBegin()==XR_SUCCESS,"withheld frame begins");boundary.setGeometryReady(true);
  const auto finishedBefore=sink.finished,composedBefore=fake.composeCalls;
  boundary.submit(vr::Eye_Right,&texture);
  check(sink.finished==finishedBefore,"partial pair cannot commit transition shadow");
  check(boundary.submit(vr::Eye_Left,&texture)==vr::VRCompositorError_None &&
    sink.finished==finishedBefore+1&&!sink.finishedPixels&&!fake.lastHadLayers&&fake.composeCalls==composedBefore,
    "withheld frame without previous image closes zero-layer frame without calling compositor");
  sink.available=true;boundary.waitAndBegin();boundary.setGeometryReady(true);
  fake.endResult=XR_ERROR_RUNTIME_FAILURE;boundary.submit(vr::Eye_Left,&texture);
  check(boundary.submit(vr::Eye_Right,&texture)==vr::VRCompositorError_InvalidTexture&&
    sink.finishedResult==XR_ERROR_RUNTIME_FAILURE,"failed endFrame reports failure to shadow commit policy");
}

// publish()/finishPair(): submit()'s own split, native_runtime_host.h's
// frame_end_overlap defers exactly the finishPair() half past a Submit
// call's return. publish() must never call finish() itself -- that is the
// one property the deferral relies on -- and submit() must still behave
// exactly as frameBoundaryTest() above already pins it doing.
void publishFinishPairSplitTest() {
  Fake fake; SessionState session; Sink sink(fake); vr::Texture_t texture{};
  check(start(session, fake), "split: session starts");
  FrameBoundary boundary(session, sink);
  check(boundary.waitAndBegin() == XR_SUCCESS, "split: wait begins frame");
  const auto predicted = boundary.frame().predictedDisplayTime; boundary.setGeometryReady(true);
  bool pairReady = true;
  check(boundary.publish(vr::Eye_Right, &texture, nullptr, vr::Submit_Default, pairReady) == vr::VRCompositorError_None &&
    !pairReady && fake.captureCalls == 1 && !fake.composeCalls && !fake.endCalls,
    "split: one eye published, pair not ready, finish not called");
  check(boundary.publish(vr::Eye_Left, &texture, nullptr, vr::Submit_Default, pairReady) == vr::VRCompositorError_None &&
    pairReady && fake.captureCalls == 2 && !fake.composeCalls && !fake.endCalls,
    "split: second eye makes the pair ready but STILL does not finish it");
  check(boundary.publish(vr::Eye_Right, &texture, nullptr, vr::Submit_Default, pairReady) == vr::VRCompositorError_InvalidTexture &&
    !pairReady && fake.captureCalls == 2, "split: duplicate eye rejected without a third capture, pairReady reset");
  check(boundary.finishPair() == XR_SUCCESS && fake.composeCalls == 1 && fake.endCalls == 1 &&
    fake.lastHadLayers && fake.lastDisplayTime == predicted, "split: finishPair composes and ends the pair publish() left open");

  // Deferred by a whole wait: nothing about a pair sitting ready between
  // publish() and finishPair() may depend on finishing it before returning
  // to the caller, so drive one from a different call than the one that
  // completed it -- the shape a queued finish (frame_end_overlap) takes.
  check(boundary.waitAndBegin() == XR_SUCCESS, "split: next wait begins frame");boundary.setGeometryReady(true);
  check(boundary.publish(vr::Eye_Left, &texture, nullptr, vr::Submit_Default, pairReady) == vr::VRCompositorError_None && !pairReady,
    "split: deferred fixture first eye");
  const auto composedBefore = fake.composeCalls, endedBefore = fake.endCalls;
  check(boundary.publish(vr::Eye_Right, &texture, nullptr, vr::Submit_Default, pairReady) == vr::VRCompositorError_None && pairReady &&
    fake.composeCalls == composedBefore && fake.endCalls == endedBefore, "split: deferred fixture pair ready, still unfinished");
  check(boundary.finishPair() == XR_SUCCESS && fake.composeCalls == composedBefore + 1 && fake.endCalls == endedBefore + 1,
    "split: finishPair from a later call still completes the pair exactly once");

  // finishPair() failure reports through failed()/lastResult() exactly as
  // submit()'s own compose/end failures do in boundaryFailures() below --
  // the deferred caller (native_runtime_host.h's finishPendingFrameEnd) reads
  // both instead of a return value nobody is still waiting on.
  check(boundary.waitAndBegin() == XR_SUCCESS, "split: failure fixture wait");boundary.setGeometryReady(true);
  fake.endResult = XR_ERROR_RUNTIME_FAILURE;
  boundary.publish(vr::Eye_Left, &texture, nullptr, vr::Submit_Default, pairReady);
  check(boundary.publish(vr::Eye_Right, &texture, nullptr, vr::Submit_Default, pairReady) == vr::VRCompositorError_None && pairReady,
    "split: failure fixture pair ready");
  check(boundary.finishPair() == XR_ERROR_RUNTIME_FAILURE && boundary.failed() && boundary.lastResult() == XR_ERROR_RUNTIME_FAILURE,
    "split: finishPair failure is reported through failed()/lastResult()");
  session.abandonAfterOwnerDestruction();

  // submit() is publish()+finishPair() glued back together for every caller
  // that still wants one call: same capture/compose/end counts and outcome
  // as driving the two halves by hand above, on a fresh, otherwise identical
  // pair.
  Fake glued; SessionState gluedSession; check(start(gluedSession, glued), "split: submit() comparison session starts");
  Sink gluedSink(glued); FrameBoundary gluedBoundary(gluedSession, gluedSink);
  check(gluedBoundary.waitAndBegin() == XR_SUCCESS, "split: submit() comparison wait");gluedBoundary.setGeometryReady(true);
  check(gluedBoundary.submit(vr::Eye_Right, &texture) == vr::VRCompositorError_None &&
    glued.captureCalls == 1 && !glued.composeCalls, "split: submit() first eye matches publish() alone");
  check(gluedBoundary.submit(vr::Eye_Left, &texture) == vr::VRCompositorError_None &&
    glued.captureCalls == 2 && glued.composeCalls == 1 && glued.endCalls == 1,
    "split: submit()'s second eye still finishes inline, same totals as publish()+finishPair()");
  gluedSession.abandonAfterOwnerDestruction();
}

// FrameCycleStats::frameEndOwnerBegin/End and nextWaitQueueDelay:
// native_frame_end_overlap's frame_end_owner_body and next_wait_queue_delay
// nested phases (native_runtime_host.h's reportFrameCycles). No submit()
// eyes needed; a bare wait/submit/wait cycle admits one sample, exactly like
// latePacingSteadyTest above drives noteReal with bare waits.
void frameEndOwnerBodyStatsTest() {
  FrameCycleStats stats;
  FrameCycleStats::Shape shape{}; shape.width[0]=shape.width[1]=shape.height[0]=shape.height[1]=2;
  // Every tick advance is its own statement (never a mutating +=  alongside
  // another read of the same variable in one call's argument list, where
  // evaluation order is unspecified) so this stays deterministic.
  uint64_t tick=1000;
  const auto waitToken=stats.waitCallerBegin(tick,7); check(waitToken!=0,"owner-body stats: first wait admitted");
  tick+=10;stats.waitOwnerBegin(waitToken,tick);
  tick+=5;stats.waitOwnerEnd(waitToken,tick);
  tick+=5;stats.waitCallerEnd(waitToken,1,tick,tick,7,shape,true);
  tick+=20;const auto submitToken1=stats.submitCallerBegin(0,tick,7);check(submitToken1!=0,"owner-body stats: first submit admitted");
  tick+=1;stats.submitOwnerBegin(submitToken1,tick);
  tick+=1;stats.submitOwnerEnd(submitToken1,tick);
  stats.submitCallerEnd(submitToken1,0,1,tick,7,0.0,true);
  tick+=5;const auto submitToken2=stats.submitCallerBegin(1,tick,7);check(submitToken2!=0,"owner-body stats: second submit admitted");
  tick+=1;stats.submitOwnerBegin(submitToken2,tick);
  tick+=1;stats.submitOwnerEnd(submitToken2,tick);
  stats.submitCallerEnd(submitToken2,1,1,tick,7,0.0,true);
  // A deferred finish now runs on the owner, overlapping the caller's own
  // work: frameEndOwnerBegin/End bracket it, entirely inside the interval
  // the next waitCallerBegin below measures as afterSecond/next_wait.
  tick+=3;const auto ownerBodyBegan=tick;stats.frameEndOwnerBegin(ownerBodyBegan);
  tick+=40;const auto ownerBodyEnded=tick;stats.frameEndOwnerEnd(ownerBodyEnded);
  tick+=10;const auto queueDelayBegan=tick; // the caller invokes the next wait...
  tick+=6;const auto ownerPicksUp=tick;     // ...but it queues behind that finish
  const auto nextWaitToken=stats.waitCallerBegin(queueDelayBegan,7);check(nextWaitToken!=0,"owner-body stats: next wait admitted");
  stats.waitOwnerBegin(nextWaitToken,ownerPicksUp);
  tick+=2;stats.waitOwnerEnd(nextWaitToken,tick);
  tick+=8;stats.waitCallerEnd(nextWaitToken,2,tick,tick,7,shape,true);
  FrameCycleStats::Report r{}; check(stats.takeReport(r)==false,"owner-body stats: window still open, no report yet");
  // Force the report: a second (partial, discarded) cycle 30s later by clock.
  tick+=20;const auto laterToken=stats.waitCallerBegin(tick,7);
  tick+=1;stats.waitOwnerBegin(laterToken,tick);
  tick+=1;stats.waitOwnerEnd(laterToken,tick);
  const auto laterNowMs=tick+30001;
  tick+=1;stats.waitCallerEnd(laterToken,3,tick,laterNowMs,7,shape,true);
  check(stats.takeReport(r) && r.valid>=1, "owner-body stats: report produced after the window elapsed");
  check(r.frameEndOwnerBody.max >= double(ownerBodyEnded-ownerBodyBegan)/1000.0 - 0.001,
    "owner-body stats: frame_end_owner_body sees the deferred finish's own wall time");
  check(r.nextWaitQueueDelay.max >= double(ownerPicksUp-queueDelayBegan)/1000.0 - 0.001,
    "owner-body stats: next_wait_queue_delay sees the wait sitting queued behind it");
}

// FrameBoundary::noteReal's late-frame counter
// (lateFrames/takeLateFramesWindow, native_pacing_summary/native_frame_
// cycle_window). No submit()/eyes needed: waitAndBegin()'s own clear() closes
// whatever frame is open, so a bare run of waits is enough to drive noteReal.
void latePacingSteadyTest() {
  Fake fake; fake.step = fake.period = 11;
  SessionState session; check(start(session, fake), "late-pacing steady: session starts");
  Sink sink(fake); FrameBoundary boundary(session, sink);
  check(boundary.waitAndBegin() == XR_SUCCESS, "late-pacing steady: first wait");
  check(boundary.lateFrames() == 0, "late-pacing steady: nothing to compare against yet");
  for (unsigned i = 0; i < 5; ++i) check(boundary.waitAndBegin() == XR_SUCCESS, "late-pacing steady: wait");
  check(boundary.lateFrames() == 0, "late-pacing steady: a period-exact cadence counts no late frames");
  check(boundary.takeLateFramesWindow() == 0, "late-pacing steady: the window count is also zero");
  session.abandonAfterOwnerDestruction();
}

void latePacingSkippedPeriodTest() {
  Fake fake; fake.step = fake.period = 11;
  SessionState session; check(start(session, fake), "late-pacing skip: session starts");
  Sink sink(fake); FrameBoundary boundary(session, sink);
  for (unsigned i = 0; i < 4; ++i) check(boundary.waitAndBegin() == XR_SUCCESS, "late-pacing skip: steady wait");
  check(boundary.lateFrames() == 0, "late-pacing skip: steady so far");
  // Fake::wait() hands out the PREVIOUS call's displayTime and advances it by
  // the CURRENT step only afterward, so step set now enlarges the gap the
  // NEXT wait, not this one, returns -- one whole period silently dropped.
  fake.step = fake.period * 2;
  check(boundary.waitAndBegin() == XR_SUCCESS, "late-pacing skip: the wait whose own return still looks steady");
  check(boundary.lateFrames() == 0, "late-pacing skip: the jump has not been observed yet");
  fake.step = fake.period;
  check(boundary.waitAndBegin() == XR_SUCCESS, "late-pacing skip: the wait that sees the two-period jump");
  check(boundary.lateFrames() == 1, "late-pacing skip: exactly one period missed is counted once");
  check(boundary.takeLateFramesWindow() == 1, "late-pacing skip: the window count reads the one miss");
  check(boundary.takeLateFramesWindow() == 0, "late-pacing skip: and clears after being read");
  check(boundary.waitAndBegin() == XR_SUCCESS, "late-pacing skip: back to steady");
  check(boundary.lateFrames() == 1, "late-pacing skip: resuming steady cadence adds no further misses");
  session.abandonAfterOwnerDestruction();
}

void latePacingPeriodChangeTest() {
  Fake fake; fake.step = fake.period = 11;
  SessionState session; check(start(session, fake), "late-pacing period change: session starts");
  Sink sink(fake); FrameBoundary boundary(session, sink);
  for (unsigned i = 0; i < 3; ++i) check(boundary.waitAndBegin() == XR_SUCCESS, "late-pacing period change: steady wait");
  check(boundary.lateFrames() == 0, "late-pacing period change: steady so far");
  // The display's refresh rate is about to rise (period 11 -> 20). Priming
  // step a wait early cancels Fake::wait()'s own one-call lag, so the wait
  // that first REPORTS the new period is also the one whose gap already
  // matches it -- the way a real runtime's synchronized time/period pair
  // would look, not an artifact of the old cadence still playing out.
  fake.step = 20;
  check(boundary.waitAndBegin() == XR_SUCCESS, "late-pacing period change: last wait at the old period");
  check(boundary.lateFrames() == 0, "late-pacing period change: still clean before the period label changes");
  fake.period = 20;
  check(boundary.waitAndBegin() == XR_SUCCESS, "late-pacing period change: first wait reporting the new period");
  // Discriminates against a stale-lastPeriod_ bug: dividing this same 20-tick
  // gap by the OLD period (11) instead of this call's own period (20) would
  // round to 1 missed frame instead of 0.
  check(boundary.lateFrames() == 0, "late-pacing period change: a rate change alone, gap matching it, is not a miss");
  check(boundary.waitAndBegin() == XR_SUCCESS, "late-pacing period change: steady at the new rate");
  check(boundary.lateFrames() == 0, "late-pacing period change: steady at the new rate stays clean");
  session.abandonAfterOwnerDestruction();
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
      fake.trace==std::vector<char>({'W','B','E','W','B','E','W','B'}),"no submit ends empty before following wait");
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

// Turbo mode (FramePacing::Deferred): waitAndBegin hands the game a
// synthesized time immediately when the pacer's real xrWaitFrame has not
// returned yet, and the real begin completes at finish()/clear() instead.
// Fake::step/period are set to 11 here so a real prediction advances by one
// period per wait, like an actual runtime's; every helper below is used
// only through a FramePacer bound to that same Fake, never concurrently
// with a different one, so reusing Fake's block/release machinery across
// several kicks in one fixture (see the fix above) is safe.
void turboPacingTest() {
  const auto session = reinterpret_cast<XrSession>(2);
  const auto track = [&](XrResult r, FrameBoundary& b, XrTime& lastSeen) {
    if (!XR_FAILED(r)) {
      check(b.frame().predictedDisplayTime >= lastSeen, "turbo: predictedDisplayTime never decreases within a session");
      lastSeen = b.frame().predictedDisplayTime;
    }
    return r;
  };
  // Waits for the NEXT blocking entry this fixture has not already consumed
  // (see Fake::enterGeneration/consumedEnterGeneration), not merely for
  // waitEntered to read true -- a fixture with several sequential kicks
  // calls this once per kick, and a plain boolean cannot tell a fresh entry
  // from the previous kick's flag still sitting true from before release()
  // woke it (release() returning is not synchronized with that wakeup).
  const auto awaitEntered = [&](Fake& f) {
    std::unique_lock<std::mutex> lock(f.waitMutex);
    const bool ok = f.waitCv.wait_for(lock, std::chrono::seconds(2),
        [&] { return f.enterGeneration > f.consumedEnterGeneration; });
    if (ok) f.consumedEnterGeneration = f.enterGeneration;
    return ok;
  };
  const auto release = [&](Fake& f) {
    std::lock_guard<std::mutex> lock(f.waitMutex); f.releaseWait = true; f.waitCv.notify_all();
  };
  const auto pollReady = [&](FramePacer& p) {
    for (int i = 0; i < 2000 && p.pending() && !p.ready(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return !p.pending() || p.ready();
  };

  // No pacer at all, and a pacer that exists but was never bound, must both
  // fall back to exactly today's blocking wait: turbo mode is opt-in per
  // object, never assumed. (Also proves waitAndBegin never dereferences an
  // unbound/absent pacer: turbo_ is false, so the pacer_->kick() short
  // circuit is never evaluated.)
  {
    Fake fake; SessionState state; check(start(state, fake), "turbo: no-pacer session starts");
    Sink sink(fake); FrameBoundary b(state, sink);
    XrTime lastSeen = 0;
    check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS &&
              !b.turbo() && !b.deferred() && fake.waitCalls == 1 && b.synthesized() == 0 && b.kicks() == 0,
          "turbo: deferred pacing with no pacer object behaves exactly like runtime pacing");
    state.abandonAfterOwnerDestruction();
  }
  {
    Fake fake; SessionState state; check(start(state, fake), "turbo: unbound-pacer session starts");
    Sink sink(fake); FramePacer pacer; FrameBoundary b(state, sink, &pacer);
    check(!pacer.bound(), "turbo: a freshly constructed pacer starts unbound");
    XrTime lastSeen = 0;
    check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS &&
              !b.turbo() && !b.deferred() && fake.waitCalls == 1 && b.synthesized() == 0,
          "turbo: deferred pacing with an unbound pacer behaves exactly like runtime pacing");
    state.abandonAfterOwnerDestruction();
  }

  // The main lifecycle. Bootstrap has nothing to synthesize from and waits
  // synchronously; completing that pair kicks the next wait immediately;
  // finding it still outstanding synthesizes from it (OpenXR Toolkit's
  // formula, clamped to one-to-two periods -- the amendment covering
  // ReferenceChanges::advance's monotonic requirement); the frame the game
  // sees carries the synthesized time while the real xrEndFrame is paced
  // against the runtime's own prediction; the same holds immediately after
  // a deferred frame finishes (the amendment's steady case); and a later
  // runtime-paced call consumes a pending result exactly rather than
  // orphaning it.
  //
  // FrameBoundary (like SessionState, which it wraps) is single-owner-thread
  // -- see frameBoundaryTest's "wrong thread" checks -- so, exactly as
  // blockedRuntimeIntegrationTest does above, b is constructed and used
  // entirely on one worker thread here; the main thread only ever touches
  // fake's own mutex/condvar and pacer's thread-safe pending()/ready().
  {
    Fake fake; fake.step = 11;
    SessionState state; check(start(state, fake), "turbo: main fixture session starts");
    FramePacer pacer;
    vr::Texture_t t{};
    std::thread worker([&] {
      Sink sink(fake); FrameBoundary b(state, sink, &pacer);
      pacer.bind(&Fake::wait, session);
      check(pacer.bound(), "turbo: pacer binds to the session's xrWaitFrame");
      XrTime lastSeen = 0;

      check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS &&
                b.turbo() && !b.deferred() && b.synthesized() == 0 && b.kicks() == 0 && fake.waitCalls == 1,
            "turbo: bootstrap has no reference to synthesize from and waits synchronously");
      b.setGeometryReady(true);
      check(b.submit(vr::Eye_Left, &t) == vr::VRCompositorError_None, "turbo: bootstrap first eye");
      fake.blockWait = true;
      check(b.submit(vr::Eye_Right, &t) == vr::VRCompositorError_None, "turbo: bootstrap pair completes");
      check(fake.endCalls == 1 && b.kicks() == 1, "turbo: completing a turbo-paced pair kicks the next wait immediately");

      check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS,
            "turbo: finding the kicked wait still outstanding synthesizes instead of blocking");
      check(b.deferred() && b.synthesized() == 1, "turbo: first synthesized frame");
      const auto synth1 = b.frame().predictedDisplayTime;
      check(synth1 >= 111 && synth1 <= 122, "turbo: synthesis is the last real time plus one to two periods (100 + [11,22])");
      b.setGeometryReady(true);
      // The second submit below blocks in finish() until the main thread
      // releases the first kick (see the choreography after this thread).
      check(b.submit(vr::Eye_Left, &t) == vr::VRCompositorError_None && b.submit(vr::Eye_Right, &t) == vr::VRCompositorError_None,
            "turbo: submitting a synthesized pair takes the pacer's real result and completes it");
      check(!b.deferred() && b.deferredFrames() == 1 && fake.endCalls == 2,
            "turbo: submitted pair is no longer deferred and closes exactly once");
      check(fake.lastDisplayTime == 111,
            "turbo: xrEndFrame is paced against the runtime's real prediction, not the synthesized one handed to the game");

      check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS,
            "turbo: steady case -- immediately after a deferred frame finishes, the next call synthesizes again");
      check(b.deferred() && b.synthesized() == 2, "turbo: second synthesized frame");
      const auto synth2 = b.frame().predictedDisplayTime;
      check(synth2 >= 122 && synth2 <= 133, "turbo: steady-case synthesis is the new last real time plus one to two periods (111 + [11,22])");
      b.setGeometryReady(true);
      // Blocks until the main thread releases the second kick.
      check(b.submit(vr::Eye_Left, &t) == vr::VRCompositorError_None && b.submit(vr::Eye_Right, &t) == vr::VRCompositorError_None,
            "turbo: second synthesized pair completes against its own real prediction");
      check(!b.deferred() && b.deferredFrames() == 2 && fake.endCalls == 3 && fake.lastDisplayTime == 122,
            "turbo: second synthesized pair took its own real result");

      // The third kick (finish()'s tail again) settles once the main thread
      // releases it; a runtime-paced call that finds a pending result just
      // waits for it briefly, the same as take() always has.
      check(track(b.waitAndBegin(FramePacing::Runtime), b, lastSeen) == XR_SUCCESS && !b.turbo() && !b.deferred(),
            "turbo: switching back to runtime pacing consumes a ready pending result instead of orphaning it");
      check(b.frame().predictedDisplayTime == 133, "turbo: a runtime-paced consume reports the pacer's real value exactly");
      b.setGeometryReady(true);
      check(b.submit(vr::Eye_Left, &t) == vr::VRCompositorError_None && b.submit(vr::Eye_Right, &t) == vr::VRCompositorError_None,
            "turbo: runtime-paced pair completes");
      check(!pacer.pending(), "turbo: runtime pacing does not kick the next wait");
    });

    check(awaitEntered(fake), "turbo: the first kick reaches the runtime's xrWaitFrame");
    check(pacer.pending() && !pacer.ready(), "turbo: the first kick is genuinely outstanding");
    release(fake);
    check(awaitEntered(fake), "turbo: the second kick reaches the runtime");
    check(pacer.pending() && !pacer.ready(), "turbo: the second kick is genuinely outstanding");
    release(fake);
    check(awaitEntered(fake), "turbo: the third kick reaches the runtime");
    check(pacer.pending() && !pacer.ready(), "turbo: the third kick is genuinely outstanding");
    release(fake);
    worker.join();

    state.abandonAfterOwnerDestruction();
    pacer.stop();
    // Read the trace only now, with the worker joined (its wait pushed the
    // 'W's). What a real runtime enforces: every end has its own begin after
    // the previous end, and no begin ever precedes the wait it belongs to.
    { unsigned begins = 0, ends = 0, waits = 0; bool ordered = true;
      for (char c : fake.trace) {
        if (c == 'W') ++waits;
        else if (c == 'B') { if (begins != ends || begins >= waits) ordered = false; ++begins; }
        else if (c == 'E') { if (begins != ends + 1) ordered = false; ++ends; }
      }
      check(ordered && ends == fake.endCalls && begins == ends,
            "turbo: every frame's real begin sits between its wait and its end, deferred ones included"); }
  }

  // Stall case: a runtime-paced wait blocked well past the fake's period,
  // then finished. The real time is learned only once the block ends
  // (noteReal runs after the blocking call returns), so a synthesis right
  // afterward measures elapsed time from there, not from when this frame's
  // own wait was entered -- the whole point of the amendment. A formula
  // that measured entry-to-entry would have inflated the result by the
  // whole stall instead of clamping it to at most two periods.
  {
    Fake fake; fake.step = 11;
    SessionState state; check(start(state, fake), "turbo: stall fixture session starts");
    FramePacer pacer;
    vr::Texture_t t{};
    XrResult stalledResult = XR_ERROR_VALIDATION_FAILURE;
    XrTime stalledFrameTime = 0, synthesizedAfterStall = 0;
    std::thread worker([&] {
      Sink sink(fake); FrameBoundary b(state, sink, &pacer);
      pacer.bind(&Fake::wait, session);
      XrTime lastSeen = 0;

      stalledResult = track(b.waitAndBegin(FramePacing::Runtime), b, lastSeen);
      check(stalledResult == XR_SUCCESS && !fake.waitTimedOut, "turbo: the stalled wait eventually returns");
      stalledFrameTime = b.frame().predictedDisplayTime;
      b.setGeometryReady(true);
      check(b.submit(vr::Eye_Left, &t) == vr::VRCompositorError_None && b.submit(vr::Eye_Right, &t) == vr::VRCompositorError_None,
            "turbo: the stalled pair completes normally");

      // Nothing was kicked by the stalled pair above (it was runtime-paced,
      // not turbo), so this synthesizes via the explicit kick() branch and
      // does not itself block -- only completing it below does.
      check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS,
            "turbo: a deferred call right after the stall synthesizes from the post-stall reference");
      synthesizedAfterStall = b.frame().predictedDisplayTime;
      b.setGeometryReady(true);
      check(b.submit(vr::Eye_Left, &t) == vr::VRCompositorError_None && b.submit(vr::Eye_Right, &t) == vr::VRCompositorError_None,
            "turbo: the post-stall synthesized pair completes");
    });

    fake.blockWait = true;
    check(awaitEntered(fake), "turbo: the stalled runtime-paced wait reaches the runtime");
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // the stall: far larger than the 11 ns fake period
    release(fake);
    // The deferred call right after the stall kicks the next wait itself
    // (the explicit kick() branch, since nothing was pending yet); release
    // it so the post-stall pair's own take() can complete.
    check(awaitEntered(fake), "turbo: the post-stall reference's own kick reaches the runtime");
    release(fake);
    worker.join();
    // Completing the post-stall pair is itself turbo-paced, so it kicked
    // once more before the worker thread returned; flush it here so
    // pacer.stop() below does not wait out Fake::wait's own 2 s timeout for
    // a release that would otherwise never come.
    check(awaitEntered(fake), "turbo: the post-stall pair's own trailing kick reaches the runtime");
    release(fake);

    check(stalledFrameTime == 100, "turbo: the stalled wait still reports the real predicted time");
    check(synthesizedAfterStall >= 111 && synthesizedAfterStall <= 122,
          "turbo: the 50 ms stall never reaches the synthesized time -- it stays bounded to the last real time plus two periods");
    state.abandonAfterOwnerDestruction();
    pacer.stop();
  }

  // clear() (ClearLastSubmittedFrame's path) must close a still-outstanding
  // synthesized frame exactly as submit() does, taking the pacer's real
  // result first so the runtime's own xrWaitFrame/xrBeginFrame pairing
  // never loses a begin it is owed.
  {
    Fake fake; fake.step = 11;
    SessionState state; check(start(state, fake), "turbo: clear-deferred fixture session starts");
    FramePacer pacer;
    vr::Texture_t t{};
    XrResult clearResult = XR_ERROR_VALIDATION_FAILURE;
    bool openedDeferred = false;
    uint64_t drainedFrames = 0;
    std::thread worker([&] {
      Sink sink(fake); FrameBoundary b(state, sink, &pacer);
      pacer.bind(&Fake::wait, session);
      XrTime lastSeen = 0;
      check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS, "turbo: clear-deferred bootstrap");
      b.setGeometryReady(true);
      check(b.submit(vr::Eye_Left, &t) == vr::VRCompositorError_None, "turbo: clear-deferred bootstrap first eye");
      fake.blockWait = true;
      check(b.submit(vr::Eye_Right, &t) == vr::VRCompositorError_None, "turbo: clear-deferred bootstrap pair completes");
      check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS && b.deferred(),
            "turbo: clear-deferred fixture opens a synthesized frame while the wait is still outstanding");
      openedDeferred = b.deferred();
      // clear() below blocks in the same way finish() does until the main
      // thread releases the outstanding kick.
      clearResult = b.clear();
      drainedFrames = b.drainedFrames();
      check(clearResult == XR_SUCCESS && !b.deferred() && drainedFrames == 1,
            "turbo: clear() closes a still-outstanding synthesized frame by taking its real result first");
    });

    check(awaitEntered(fake), "turbo: clear-deferred kick reaches the runtime");
    release(fake);
    worker.join();
    check(openedDeferred, "turbo: clear-deferred fixture actually opened a synthesized frame before calling clear()");
    state.abandonAfterOwnerDestruction();
    pacer.stop();
  }

  // drain() (the STOPPING/close teardown path) must also reach for a wait
  // that was only ever kicked -- finish()'s post-submit kick here, with no
  // frame of this boundary's own left open -- so a worker thread is never
  // left mid xrWaitFrame with nobody left to take its result.
  {
    Fake fake; fake.step = 11;
    SessionState state; check(start(state, fake), "turbo: drain fixture session starts");
    Sink sink(fake); FramePacer pacer; FrameBoundary b(state, sink, &pacer);
    pacer.bind(&Fake::wait, session);
    vr::Texture_t t{};
    XrTime lastSeen = 0;
    check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS, "turbo: drain fixture bootstrap");
    b.setGeometryReady(true);
    check(b.submit(vr::Eye_Left, &t) == vr::VRCompositorError_None && b.submit(vr::Eye_Right, &t) == vr::VRCompositorError_None,
          "turbo: drain fixture bootstrap pair completes and kicks the next wait");
    check(!state.frameOpen(), "turbo: nothing is open on this boundary when drain() runs");
    const auto endsBefore = fake.endCalls;
    check(b.drain() == XR_SUCCESS && b.drainedWaits() == 1 && fake.endCalls == endsBefore + 1,
          "turbo: drain() takes the outstanding kick and begins/ends it with zero layers");
    check(!pacer.pending(), "turbo: drain() leaves nothing outstanding");
    state.abandonAfterOwnerDestruction();
    pacer.stop();
  }

  // readyAtWait(): a deferred-paced call that finds the pacer's result
  // already sitting there (nothing artificially delaying it) consumes it
  // synchronously rather than opening a synthesized frame, and counts it
  // distinctly from a synthesis.
  {
    Fake fake; fake.step = 11;
    SessionState state; check(start(state, fake), "turbo: ready-at-wait fixture session starts");
    Sink sink(fake); FramePacer pacer; FrameBoundary b(state, sink, &pacer);
    pacer.bind(&Fake::wait, session);
    vr::Texture_t t{};
    XrTime lastSeen = 0;
    check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS, "turbo: ready-at-wait bootstrap");
    b.setGeometryReady(true);
    check(b.submit(vr::Eye_Left, &t) == vr::VRCompositorError_None && b.submit(vr::Eye_Right, &t) == vr::VRCompositorError_None,
          "turbo: ready-at-wait bootstrap pair completes and kicks the next wait");
    check(pollReady(pacer), "turbo: with nothing blocking it, the kicked wait settles on its own");
    check(track(b.waitAndBegin(FramePacing::Deferred), b, lastSeen) == XR_SUCCESS && !b.deferred(),
          "turbo: a deferred call that finds the pacer already done consumes it instead of synthesizing");
    check(b.readyAtWait() == 1 && b.synthesized() == 0, "turbo: ready-at-wait is counted distinctly from synthesis");
    b.setGeometryReady(true);
    check(b.submit(vr::Eye_Left, &t) == vr::VRCompositorError_None && b.submit(vr::Eye_Right, &t) == vr::VRCompositorError_None,
          "turbo: ready-at-wait pair completes");
    state.abandonAfterOwnerDestruction();
    pacer.stop();
  }
}
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) { std::puts("Would test injected OpenXR frame boundary; no runtime or files."); return 0; }
  if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
  gateTest(); boundedBlockedGateTest(); frameBoundaryTest(); publishFinishPairSplitTest(); frameEndOwnerBodyStatsTest();
  latePacingSteadyTest(); latePacingSkippedPeriodTest(); latePacingPeriodChangeTest();
  blockedRuntimeIntegrationTest(); boundaryFailures();backgroundFrames();loadingTransitions();turboPacingTest();
  std::printf("openxr_frame_test: %u checks, %u failures\n", checks.load(), failures.load()); return failures.load() ? 1 : 0;
}
