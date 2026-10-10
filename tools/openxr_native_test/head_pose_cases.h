#pragma once
// Elite's "now" head pose, answered one display period after the drawn frame's display time (src/openxr/head_pose_time.h, native_runtime_host.h
// locateHeadFor, src/openxr/pose_gap.h; docs\terrain-culling.md): through the real hop to the owner thread, from a foreign thread, the instant a call
// is located at (display time + period), the figures the diagnostic takes from each locate, what the WaitGetPoses publish point keeps, and who is
// said to be answered.
// Then the cache's lifetime: it is dropped wherever the origin, the session or the geometry publication is invalidated (a seated reset, a
// session stop and restart, a failed reset, a fatal failure), it is not used once it has fallen more than one display period behind now, and a
// display-time locate the runtime refuses with XR_ERROR_TIME_INVALID is retried once at now + prediction. Every cell reads the instant the fake
// XR runtime was actually handed, against a clock the rig sets, so "now" is a number the cell chooses. The window arithmetic, the filter, the
// freshness rule and the line formats are held in tools\openxr_pose_test.
#include "launch_centre_cases.h"
#include "frame_end_overlap_cases.h"
#include <cmath>
#include <cstring>

namespace edvr::openxr::test {
namespace head_pose_fixture {
// The host's head clock: the fake XR runtime's convert hands the counter back as the time, so this is "now" to the nanosecond.
inline LARGE_INTEGER counter{};
inline bool readClock(LARGE_INTEGER* value){*value=counter;return true;}
inline void setNow(XrTime now){counter.QuadPart=now;}
// A session that goes STOPPING and READY on cue, for the real service tick.
inline XrSessionState queued=XR_SESSION_STATE_UNKNOWN;
inline XrResult XRAPI_PTR pollQueued(XrInstance,XrEventDataBuffer* out) {
  if(queued==XR_SESSION_STATE_UNKNOWN)return XR_EVENT_UNAVAILABLE;
  XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
  event.session=launch_fixture::session();event.state=queued;queued=XR_SESSION_STATE_UNKNOWN;
  std::memcpy(out,&event,sizeof(event));return XR_SUCCESS;
}
}
template<class Check> void runHeadPoseCases(Check&& check) {
  using namespace launch_fixture;
  using namespace head_pose_fixture;
  const auto close = [](double a, double b, double eps) { return std::fabs(a - b) <= eps; };
  OwnerBuiltFixture f;
  check(f.route.bind() && f.owner.start() && f.owner.invoke([&] { f.build(); }) && f.initialized, "(the head-pose cases' host is built on its own owner thread)");
  if (!f.initialized) return;
  auto& h = *f.hostPtr;
  h.headClock = readClock;
  const DWORD me = GetCurrentThreadId();
  const XrTime display = 5000000000, period = 11111111;
  const XrTime lead = 42000000;   // flight 3: the game thread's "now" was about 42 ms before the drawn frame's display time
  const XrTime now = display - lead;
  setNow(now);
  char lines[8][352]{};
  unsigned lineCount = 0;
  const auto collect = [&](const char* line) { if (lineCount < 8) std::snprintf(lines[lineCount++], sizeof(lines[0]), "%s", line); };
  const double half = 3.14159265358979323846 / 180.0 * 0.5;
  const auto yawed = [&](double degrees) { f.fake.head.orientation = {0.0f, float(std::sin(degrees * half)), 0.0f, float(std::cos(degrees * half))}; };
  // The publish point: the frame the game now holds, kept for the pose calls to be located at and measured against.
  XrVector3f turn{0.0f, 0.5235988f, 0.0f};   // 30 degrees a second about y
  bool published = false;
  check(f.owner.invoke([&] {
    h.publishPoseGap(display, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, XR_SPACE_VELOCITY_ANGULAR_VALID_BIT, turn, 1000, collect);
    published = h.latestPoseFrame.valid;
  }) && published, "(a frame is published to the host)");
  bool kept = false;
  f.owner.invoke([&] {
    const auto& p = h.latestPoseFrame;
    kept = p.displayTime == display && p.period == period && p.orientationKnown && p.speedKnown && close(p.speedDegPerSec, 30.0, 1e-3) &&
           p.orientation[0] == 0 && p.orientation[3] == 1 && h.poseGap.waits() == 1;
  });
  check(kept, "the publish point keeps the frame's display time and period, its orientation, the head speed in degrees a second, and counts the wait");
  // What the WaitGetPoses publish point does, for a cell that needs another frame.
  const auto publish = [&](XrTime displayTime, XrDuration framePeriod, uint64_t atMs) {
    f.owner.invoke([&] { h.publishPoseGap(displayTime, framePeriod, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, atMs, collect); });
  };
  vr::TrackedDevicePose_t pose{};
  const auto locateAs = [&](bool atDisplay, float prediction, uint32_t rva = 0x4E3881) {
    HeadCall call;call.thread = me;call.rva = rva;call.display = atDisplay;
    pose = {};
    return h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseSeated, prediction, call, pose);
  };
  const auto flushLine = [&](std::string& out) {
    lineCount = 0;
    f.owner.invoke([&] { h.poseGap.flushNow(0, collect); });
    out = lineCount ? lines[lineCount - 1] : "";
    return lineCount;
  };
  yawed(0.0);
  // ---- inside the game's image and asking for "now": the display time ----------------------------------------------------------------
  check(locateAs(true, 0.0f) && f.fake.lastTime == display + period && h.lastHeadTime == display + period && pose.bPoseIsValid,
        "a call flagged as Elite's \"now\" is located at exactly the latest frame's display time plus that frame's period");
  check(locateAs(true, 0.003f) && f.fake.lastTime == display + period && locateAs(true, -0.004f) && f.fake.lastTime == display + period,
        "...whatever small prediction Elite passed (the filter, not the host, decides what is \"now\")");
  std::string line;
  flushLine(line);
  char want[512];
  std::snprintf(want, sizeof(want), "pose gap: tid %lu calls 3 from exe+0x4E3881 prediction -0.3 ms target-minus-display mean 11.11 ms (min 11.11 max 11.11) angle-to-drawn mean 0.000 max 0.000 deg head 30.0 deg/s waitgetposes 1 failed 0", (unsigned long)me);
  check(line == want, "the diagnostic records the CALLER's thread (the hop to the owner does not lose it), the return RVA, the mean prediction, a gap of one period (+11.11 ms), an angle of 0 and the wait the publish point counted (the exact line)");
  // ---- everything else: today's answer ------------------------------------------------------------------------------------------------
  check(locateAs(false, 0.0f) && f.fake.lastTime == now && f.fake.lastTime != display + period, "a call that is not flagged (outside the image, or with a real prediction) is located on the wall clock as always, not at the display time plus a period");
  flushLine(line);
  check(line.find("target-minus-display mean -42.00 ms") != std::string::npos && line.find("fallback") == std::string::npos,
        "...and its gap is the true one (-42.00 ms), with no fallback (it was never asked for the display time)");
  check(locateAs(false, 0.25f) && f.fake.lastTime == now + 250000000,
        "...and the prediction it passed is still added: a quarter second after now");
  const double lateGap = double(f.fake.lastTime - display) / 1e6;
  flushLine(line);
  char wantGap[160];
  std::snprintf(wantGap, sizeof(wantGap), "target-minus-display mean %.2f ms (min %.2f max %.2f)", lateGap, lateGap, lateGap);
  check(line.find(wantGap) != std::string::npos, "...and its gap is the located instant less the display time, in milliseconds");
  // The angle: the pose handed back against the pose that frame was drawn with.
  yawed(10.0);
  check(locateAs(true, 0.0f), "(a head turned 10 degrees from the drawn frame)");
  flushLine(line);
  check(line.find("angle-to-drawn mean 10.000 max 10.000 deg") != std::string::npos, "the angle between the pose handed back and the pose drawn is 10.000 degrees");
  yawed(0.0);
  // ---- no frame to be answered at: today's answer, counted ----------------------------------------------------------------------------
  f.owner.invoke([&] { h.latestPoseFrame.valid = false; });
  check(locateAs(true, 0.0f, 0x777777) && f.fake.lastTime == now, "with no frame yet an Elite \"now\" call is located at now + prediction, as before");
  flushLine(line);
  check(line.find("target-minus-display n/a angle-to-drawn n/a head n/a") != std::string::npos && line.find(" fallback 1") != std::string::npos,
        "...and the line says there was nothing to measure against, and one fallback");
  f.owner.invoke([&] { h.latestPoseFrame.valid = true;h.latestPoseFrame.displayTime = 0; });
  check(locateAs(true, 0.0f, 0x777777) && f.fake.lastTime == now, "a display time that is not positive falls back too");
  flushLine(line);
  check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
  f.owner.invoke([&] { h.latestPoseFrame.displayTime = -5; });
  check(locateAs(true, 0.0f, 0x777777) && f.fake.lastTime == now, "...a negative one too");
  flushLine(line);
  f.owner.invoke([&] { h.latestPoseFrame.displayTime = display; });
  // ---- who is said to be answered ------------------------------------------------------------------------------------------------------
  check(h.poseSightings.distinct() == 1, "(so far one Elite caller has been answered one period past the display time: the fallbacks (from another address) and the unflagged calls said nothing)");
  check(locateAs(true, 0.0f, 0x4E3881), "(the same caller again)");
  check(h.poseSightings.distinct() == 1, "the same return address is not said twice");
  check(locateAs(true, 0.0f, 0x1234567), "(another caller in the image)");
  check(h.poseSightings.distinct() == 2, "a further distinct return address is said once (a caller that appears after a game update)");
  flushLine(line);
  // ---- failures: the owner's busy gate, a retired generation, an origin the runtime does not locate, a locate the runtime refuses ---
  HeadCall call;call.thread = me;call.rva = 0x4E3881;call.display = true;
  vr::TrackedDevicePose_t none{};
  check(!h.locateHeadFor(h.geometryGeneration + 1, vr::TrackingUniverseSeated, 0.0f, call, none) &&
            !h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseStanding, 0.0f, call, none),
        "a stale generation and an origin the runtime does not locate return false");
  f.fake.locateResult = XR_ERROR_RUNTIME_FAILURE;
  check(!h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseSeated, 0.0f, call, none), "...so does a locate the runtime refuses");
  f.fake.locateResult = XR_SUCCESS;
  flushLine(line);
  check(line.find(" calls 3 ") != std::string::npos && line.find(" failed 3") != std::string::npos && line.find("target-minus-display n/a") != std::string::npos,
        "all three failures are counted in `failed` on the caller's line, the silent busy-gate path included, and left out of every mean");
  check(h.poseSightings.distinct() == 2, "(a failed locate says nothing of who is answered)");
  h.noteHeadCallFailed(call, 0.5f);
  flushLine(line);
  check(line.find(" calls 1 ") != std::string::npos && line.find(" failed 1") != std::string::npos && line.find("prediction 500.0 ms") != std::string::npos,
        "a pose call that never reached the owner (no live session, a bad origin) is counted too, with its prediction");
  // The old entry point is the plain call.
  check(h.locateHead(h.geometryGeneration, vr::TrackingUniverseSeated, 0.0f, none) && f.fake.lastTime == now, "locateHead (no caller known) is located at now + prediction, as it always was");
  flushLine(line);
  check(line.find(" tid 0 calls 1 from exe+0x0 ") != std::string::npos, "...and is recorded under thread 0, RVA 0");
  // The publish point writes the window when it is 60 s old, and only then.
  f.owner.invoke([&] { h.poseGap.flushNow(0, collect); });
  lineCount = 0;
  f.owner.invoke([&] {
    PoseGapStats::Sample s{};s.thread = 5;s.rva = 0x4E3881;s.located = true;
    h.poseGap.note(s);
    h.publishPoseGap(display, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, 59999, collect);
  });
  check(lineCount == 0, "the publish point writes nothing for a window under 60 s old");
  publish(display, period, 60000 + 1);
  check(lineCount == 1 && std::string(lines[0]).rfind("pose gap: tid 5 calls 1 ", 0) == 0, "...and the window's line once it is 60 s old");

  // ====================================================================================================================================
  // The cache's lifetime. Each cell first proves the cache is live (a qualifying call is located at the display time), so that what follows is
  // the invalidation and not a cache that never worked. A call "before the next wait" is one with no publishPoseGap in between: the publish
  // point is what waitPoses calls with the frame it just got (the wait itself needs a real session binding no fixture has).
  // ====================================================================================================================================
  const auto answeredAt = [&](XrTime instant) { return locateAs(true, 0.0f) && f.fake.lastTime == instant && pose.bPoseIsValid; };
  // ---- the origin: a seated reset, and any other invalidateOrigin ----------------------------------------------------------------------
  {
    setNow(now);
    publish(display, period, 100000);
    check(answeredAt(display + period), "(before a reset the cached display time answers a qualifying call)");
    XrSpace before = XR_NULL_HANDLE, after = XR_NULL_HANDLE;
    bool reset = false, dropped = false;
    f.owner.invoke([&] { before = h.seated.space(); reset = h.resetSeated(h.geometryGeneration); after = h.seated.space(); dropped = !h.latestPoseFrame.valid; });
    check(reset && before != after && dropped, "a seated reset replaces the space and drops the cached frame");
    setNow(now);
    check(answeredAt(now) && f.fake.lastTime != display + period, "...and a qualifying call before the next wait is located at now + prediction, not at the old display time");
    flushLine(line);
    check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
    const XrTime next = display + period;
    setNow(next - lead);
    publish(next, period, 100001);
    check(answeredAt(next + period), "...and after the next wait publishes a frame, the display time answers again");
    // The other callers of invalidateOrigin (a reference-space change in a wait or a loading frame; the session restart below).
    f.owner.invoke([&] { h.invalidateOrigin("reference_change"); dropped = !h.latestPoseFrame.valid; });
    check(dropped, "a reference-space change (invalidateOrigin) drops it too");
    setNow(next - lead);
    check(answeredAt(next - lead) && f.fake.lastTime != next + period, "...and the call after it is located at now + prediction");
  }
  // ---- the session: STOPPING, READY, start, through the real service tick -----------------------------------------------------------------
  {
    const XrTime restartDisplay = 7000000000LL;
    setNow(restartDisplay - lead);
    const Dispatch dispatch{pollQueued, beginSession, endSession, wait, beginFrame, endFrame};
    bool running = false, stopped = false, dropped = false, restarted = false, droppedAtRestart = false, connected = false;
    f.owner.invoke([&] {
      // A session that polls on cue, started afresh (the fixture's own poll only ever says READY once).
      h.state.abandonAfterOwnerDestruction();
      queued = XR_SESSION_STATE_READY;
      running = h.state.reset(dispatch, instance(), session(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == XR_SUCCESS &&
                h.state.pollEvents() == XR_SUCCESS && h.state.startIfReady() == XR_SUCCESS;
      h.publishPoseGap(restartDisplay, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, 200000, collect);
    });
    check(running && answeredAt(restartDisplay + period), "(a started session, with a frame cached)");
    f.owner.invoke([&] {
      queued = XR_SESSION_STATE_STOPPING;
      h.pumpEvents();
      stopped = !h.state.running();
      dropped = !h.latestPoseFrame.valid;
    });
    check(stopped && dropped, "the service tick that ends the session on STOPPING drops the cached frame");
    f.owner.invoke([&] {
      queued = XR_SESSION_STATE_READY;
      h.pumpEvents();
      restarted = h.state.running();
      droppedAtRestart = !h.latestPoseFrame.valid;
      connected = h.read().connected;
    });
    check(restarted && droppedAtRestart && connected, "...and READY restarts it (prepareSessionRestart) with no frame cached, the host still connected and on the same generation");
    setNow(restartDisplay - lead);
    check(answeredAt(restartDisplay - lead) && f.fake.lastTime != restartDisplay + period,
          "a qualifying call after the restart and before the next wait is located at now + prediction, not at the previous session's display time");
    const XrTime afterRestart = restartDisplay + 50 * period;
    setNow(afterRestart - lead);
    publish(afterRestart, period, 200001);
    check(answeredAt(afterRestart + period), "...and after the next wait publishes a frame, the display time answers again");
    // prepareSessionRestart on its own (the review's path): origin invalidation clears it even with the session still the same one.
    bool prepared = false;
    f.owner.invoke([&] {
      queued = XR_SESSION_STATE_STOPPING;
      h.pumpEvents();
      h.publishPoseGap(afterRestart, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, 200002, collect);   // a stale publish into the stopped session
      prepared = !h.state.running() && h.prepareSessionRestart();
      dropped = !h.latestPoseFrame.valid;
      queued = XR_SESSION_STATE_READY;
      h.state.pollEvents();
      restarted = h.state.startIfReady() == XR_SUCCESS;
      h.serviceStopped = false;   // what the service tick does once it has restarted the session
    });
    check(prepared && dropped && restarted, "prepareSessionRestart itself drops the cache (a frame published into the stopped session is gone at the restart)");
    setNow(afterRestart - lead);
    check(answeredAt(afterRestart - lead) && f.fake.lastTime != afterRestart + period, "...and the first call of the new session is located at now + prediction");
    publish(afterRestart, period, 200003);
  }
  // ---- the cache is not used once it is stale ----------------------------------------------------------------------------------------------
  {
    const XrTime base = 9000000000LL;
    publish(base, period, 300000);
    setNow(base + period);
    check(answeredAt(base + period), "a display time exactly one period behind now is still good (the freshness test is on the cached frame, not on the target a period ahead of it)");
    setNow(base + period + 1);
    check(answeredAt(base + period + 1) && f.fake.lastTime != base + period, "...one nanosecond more and it is not: the call is located at now + prediction");
    flushLine(line);
    check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
    setNow(base + 3 * period);
    check(answeredAt(base + 3 * period), "...and three periods behind it is not either");
    setNow(base - lead);
    check(answeredAt(base + period), "(back at flight 3's lead the same frame answers again: nothing was dropped, the cache was only not used)");
    publish(base, 0, 300001);   // a frame that reports no period: there is no target to form
    flushLine(line);
    check(answeredAt(base - lead), "with no period no target can be formed: the call is located at now + prediction");
    flushLine(line);
    check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
    publish(base, -5, 300001);
    check(answeredAt(base - lead), "...and a negative period likewise");
    flushLine(line);
    setNow(base);
    publish(base, 1000000000000LL, 300002);   // a period that is not a display period: the tolerance is held to the 50 ms cap
    setNow(base + 50000000);
    check(answeredAt(base + 1000000000000LL), "a huge period is added as it stands to the target, and the tolerance is capped at 50 ms: good at 50 ms behind");
    setNow(base + 50000001);
    check(answeredAt(base + 50000001), "...and not at 50 ms and a nanosecond");
    // A stale display time never reaches the runtime, so there is nothing for it to refuse or to answer with old tracking.
    publish(base, period, 300003);
    setNow(base + 10 * period);
    const unsigned locatesBefore = f.fake.locates;
    check(answeredAt(base + 10 * period) && f.fake.locates == locatesBefore + 1, "a stale display time costs the runtime exactly one locate, at now");
  }
  // ---- a reference-space change between the display time and the target ------------------------------------------------------------------
  {
    const XrTime base = 12000000000LL;
    setNow(base - lead);
    publish(base, period, 350000);
    check(answeredAt(base + period), "(a fresh frame)");
    flushLine(line);
    XrEventDataReferenceSpaceChangePending event{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
    event.session = session();event.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;event.changeTime = base + period / 2;event.poseValid = XR_FALSE;
    bool noted = false;
    f.owner.invoke([&] { noted = h.changes.note(event); });
    check(noted && answeredAt(base - lead) && f.fake.lastTime != base + period,
          "a change of the reference space pending between the display time and the target: the call is located at now + prediction, not across the change");
    flushLine(line);
    check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
    f.owner.invoke([&] { h.changes.clear();noted = h.changes.begin(session()); });
    check(noted && answeredAt(base + period), "(the change gone, the target answers again)");
    flushLine(line);
  }
  // ---- the runtime refuses the display time --------------------------------------------------------------------------------------------
  {
    const XrTime base = 11000000000LL;
    setNow(base - lead);
    publish(base, period, 400000);
    check(answeredAt(base + period), "(a fresh frame)");
    const unsigned sightings = h.poseSightings.distinct();
    flushLine(line);
    const unsigned locatesBefore = f.fake.locates;
    const uint64_t refusalsBefore = h.displayTimeRefusals;
    f.fake.rejectTime = base + period;
    const bool answered = locateAs(true, 0.0f);
    f.fake.rejectTime = -1;
    check(answered && pose.bPoseIsValid && f.fake.lastTime == base - lead && f.fake.locates == locatesBefore + 2 && h.displayTimeRefusals == refusalsBefore + 1,
          "XR_ERROR_TIME_INVALID on the display-time locate: one retry at now + prediction, and a valid pose, not an invalid one");
    check(h.poseSightings.distinct() == sightings, "...and a retried call is not said to have been answered one period past the display time");
    flushLine(line);
    check(line.find(" fallback 1") != std::string::npos && line.find(" failed 0") != std::string::npos, "...it is counted as a fallback, not as a failure");
    // The retry carries the prediction the caller passed (a prediction inside the filter's window).
    f.fake.rejectTime = base + period;
    check(locateAs(true, 0.004f) && f.fake.lastTime == base - lead + 4000000, "...at now + the prediction Elite passed");
    f.fake.rejectTime = -1;
    // Only that error.
    f.fake.rejectTime = base + period;f.fake.rejectResult = XR_ERROR_RUNTIME_FAILURE;
    const unsigned locatesOther = f.fake.locates;
    const bool failed = locateAs(true, 0.0f);
    f.fake.rejectTime = -1;f.fake.rejectResult = XR_ERROR_TIME_INVALID;
    check(!failed && f.fake.locates == locatesOther + 1, "any other error is not retried: one locate, no pose");
    // The retry is once: if now + prediction is refused too, the call fails with it.
    f.fake.locateResult = XR_ERROR_TIME_INVALID;
    const unsigned locatesTwice = f.fake.locates;
    const bool bothRefused = locateAs(true, 0.0f);
    f.fake.locateResult = XR_SUCCESS;
    check(!bothRefused && f.fake.locates == locatesTwice + 2, "...and only once: when now + prediction is refused as well, there are two locates and no pose");
    // The unflagged path never retries anything.
    f.fake.rejectTime = base - lead;
    const unsigned locatesPlain = f.fake.locates;
    check(!locateAs(false, 0.0f) && f.fake.locates == locatesPlain + 1, "a call that was never asked for the display time is not retried (it has no second instant to try)");
    f.fake.rejectTime = -1;
  }
  // ---- a wait that fails ------------------------------------------------------------------------------------------------------------------
  {
    setNow(display - lead);
    publish(display, period, 440000);
    check(answeredAt(display + period), "(a frame, before a wait that fails)");
    // A wait that gets as far as the frame and then cannot take it (the frame-sequence counter has no room): the runtime's own fail path, with no
    // boundary failure behind it that would end the host and drop the cache by another road.
    CompositorRead polled{};
    vr::EVRCompositorError waited = vr::VRCompositorError_None;
    bool dropped = false, stillRunning = false;
    f.owner.invoke([&] {
      h.frameSequenceOffset = (std::numeric_limits<uint64_t>::max)();
      waited = h.waitPoses(h.compositorGeneration, polled);
      h.frameSequenceOffset = 0;
      dropped = !h.latestPoseFrame.valid;
      stillRunning = !h.serviceFailed && h.state.running();
    });
    check(waited != vr::VRCompositorError_None && stillRunning && dropped, "a WaitGetPoses that fails invalidates the geometry publication and drops the cached frame with it (the host is still up)");
  }
  // ---- the last two cells end the host's usefulness (a reset that cannot make its space, a fatal failure), so they come last ----------------------
  {
    setNow(display - lead);
    publish(display, period, 450000);
    check(answeredAt(display + period), "(a frame again)");
    f.fake.createResult = XR_ERROR_RUNTIME_FAILURE;
    bool refused = false, dropped = false;
    f.owner.invoke([&] { refused = !h.resetSeated(h.geometryGeneration); dropped = !h.latestPoseFrame.valid; });
    f.fake.createResult = XR_SUCCESS;
    check(refused && dropped, "a seated reset whose new space cannot be created has already invalidated the geometry publication, and drops the cached frame with it");
  }
  {
    setNow(display - lead);
    publish(display, period, 500000);
    bool dropped = false;
    f.owner.invoke([&] { h.publishFatalFailure(XR_ERROR_RUNTIME_FAILURE, "head_pose_cases"); dropped = !h.latestPoseFrame.valid; });
    check(dropped, "a fatal failure drops the cached frame");
  }
  f.owner.stop();
}
} // namespace edvr::openxr::test
