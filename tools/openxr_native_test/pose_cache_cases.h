#pragma once
// The display-time pose cache (src/openxr/pose_cache.h, poll_ring.h; native_pose_cache), through the real host on its own owner thread
// against the fake XR runtime and a clock the rig sets. A display call made under a live publication is answered on the caller's thread
// with no owner locate; its answer equals the hop's answer for the same instant and pose; a call made while a wait is in progress is
// answered at the next frame's instant, which is the hop's answer after that wait when the frames are one period apart; an invalidation
// forces the hop; the key off gives zero hits; the window counts and the in-wait error are exact; the poll ring keeps order, refuses at
// full and counts the refusal; and pollEvent drains the ring with a queued quit first, as the owner path does.
#include "head_pose_cases.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <string>
#include <thread>

namespace edvr::openxr::test {
namespace pose_cache_fixture {
inline std::atomic<uint64_t> gUs{1000000};
inline uint64_t clockUs() { return gUs.load(); }
inline bool samePose(const vr::TrackedDevicePose_t& a, const vr::TrackedDevicePose_t& b) {
  return a.bPoseIsValid == b.bPoseIsValid && a.eTrackingResult == b.eTrackingResult &&
         std::memcmp(&a.mDeviceToAbsoluteTracking, &b.mDeviceToAbsoluteTracking, sizeof(a.mDeviceToAbsoluteTracking)) == 0;
}
}

template<class Check> void runPoseCacheCases(Check&& check) {
  using namespace launch_fixture;
  using namespace head_pose_fixture;
  using namespace pose_cache_fixture;
  OwnerBuiltFixture f;
  check(f.route.bind() && f.owner.start() && f.owner.invoke([&] { f.build(); }) && f.initialized,
        "(the pose-cache cases' host is built on its own owner thread)");
  if (!f.initialized) return;
  auto& h = *f.hostPtr;
  h.headClock = readClock;
  h.poseCache.setClock(&clockUs);
  h.poseCacheOn.store(true);
  const DWORD me = GetCurrentThreadId();
  const XrTime display = 5000000000, period = 11111111, now = display - 42000000;
  setNow(now);
  f.fake.head.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
  const XrVector3f turn{0.0f, 0.5235988f, 0.0f};
  std::string lastGap;
  const auto collectGap = [&](const char* line) { lastGap = line; };
  // The WaitGetPoses publish point, then the cache publication made from it, both on the owner.
  const auto publishFrame = [&](XrTime d) {
    return f.owner.invoke([&] {
      h.publishPoseGap(d, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, XR_SPACE_VELOCITY_ANGULAR_VALID_BIT, turn, 1000, collectGap);
      h.publishPoseCache();
    });
  };
  const auto callDisplay = [&](vr::TrackedDevicePose_t& p, uint32_t rva = 0x4E3881) {
    HeadCall call; call.thread = me; call.rva = rva; call.display = true;
    p = {};
    return h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseSeated, 0.0f, call, p);
  };
  // Runs a display call on a worker and waits at most `ms` for it. A call that had to wait for the owner would not return in time.
  const auto callWithin = [&](vr::TrackedDevicePose_t& p, int ms, uint32_t rva, bool& finished) {
    auto fut = std::async(std::launch::async, [&, rva] {
      vr::TrackedDevicePose_t local{};
      const bool ok = callDisplay(local, rva);
      return std::make_pair(ok, local);
    });
    finished = fut.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
    if (!finished) return false;
    const auto result = fut.get();
    p = result.second;
    return result.first;
  };

  // ---- a frame published; the first call from a caller takes the hop (its RVA is new to the cache) -----------------------------------
  check(publishFrame(display), "(the frame and its publication are made on the owner)");
  vr::TrackedDevicePose_t first{}, cached{};
  check(callDisplay(first) && f.fake.lastTime == display + period && h.lastHeadTime == display + period,
        "a caller's first display call takes the owner hop and is located at display + period, as before");
  const unsigned locatesBefore = f.fake.locates;
  check(callDisplay(cached) && f.fake.locates == locatesBefore && samePose(cached, first),
        "the same caller's next display call is answered from the publication: no owner locate, and the same pose the hop gave");
  f.owner.invoke([&] { h.poseGap.flushNow(0, collectGap); });
  check(lastGap.find("tid " + std::to_string(static_cast<unsigned long>(me)) + " calls ") != std::string::npos,
        "a cache answer still feeds the pose-gap diagnostic, under the caller's thread");

  // ---- a call made while the next wait is in progress --------------------------------------------------------------------------------------
  std::promise<void> entered, release;
  auto releaseFuture = release.get_future().share();
  std::thread holder([&] {
    f.owner.invoke([&] {
      h.poseCache.beginWait();   // what waitPoses does at its entry; the owner now stays in the wait
      entered.set_value();
      releaseFuture.wait();
    });
  });
  entered.get_future().wait();
  vr::TrackedDevicePose_t inWait{};
  bool finished = false;
  const bool inWaitOk = callWithin(inWait, 2000, 0x4E3881, finished);
  check(finished && inWaitOk, "a call made during the owner's wait is answered without waiting for the owner (within 2 s)");
  release.set_value();
  holder.join();

  // The hop after that wait (the owner is free again) publishes the next frame, one period on. With the key off the hop answers.
  check(publishFrame(display + period), "(the next frame is published, one period on)");
  vr::TrackedDevicePose_t hopAfterWait{};
  h.poseCacheOn.store(false);
  const bool hopOk = callDisplay(hopAfterWait);
  h.poseCacheOn.store(true);
  check(hopOk && f.fake.lastTime == display + 2 * period && samePose(inWait, hopAfterWait),
        "a call made during a wait is answered at the next frame's display + period: the hop's answer after that wait, pose and instant");

  // ---- invalidation forces the hop -----------------------------------------------------------------------------------------------------------
  f.owner.invoke([&] { h.dropPoseFrame(); });
  vr::TrackedDevicePose_t afterDrop{};
  check(callDisplay(afterDrop) && f.fake.lastTime == now,
        "after the frame is dropped the display call takes the hop, which has no frame to answer at (now + prediction)");

  // The window so far: two cache answers (one of them during a wait), a new caller and an unpublished call, both hopped.
  PoseCacheReport w{};
  h.poseCache.takeWindow(w);
  check(w.hits == 2 && w.hitsInWait == 1 && w.misses == 2 && w.reasons[unsigned(PoseCacheMiss::NewCaller)] == 1 &&
        w.reasons[unsigned(PoseCacheMiss::Unpublished)] == 1,
        "the window counts two hits (one in a wait) and two hopped calls (new caller, unpublished) exactly");
  check(w.publishes == 2 && w.targetValid && std::fabs(w.targetP50Ms) < 1e-9 && w.angleValid && w.angleP50Deg < 1e-6,
        "the in-wait error of the steady frame is zero: the cached next answer equalled the hop's (target and angle)");

  // ---- a frame that skips one period: the in-wait answer is early by that period --------------------------------------------------------
  publishFrame(display + 10 * period);
  publishFrame(display + 12 * period);
  PoseCacheReport skipped{};
  h.poseCache.takeWindow(skipped);
  check(skipped.targetValid && std::fabs(skipped.targetP50Ms + double(period) / 1e6) < 1e-6,
        "a skipped frame makes the in-wait answer one period early, and the window's target_delta says so (-11.111 ms)");

  // ---- the key off: no answers from the cache at all ---------------------------------------------------------------------------------------
  publishFrame(display + 12 * period);
  h.poseCacheOn.store(false);
  vr::TrackedDevicePose_t offA{}, offB{};
  const bool offOk = callDisplay(offA) && callDisplay(offB);
  h.poseCacheOn.store(true);
  PoseCacheReport off{};
  h.poseCache.takeWindow(off);
  check(offOk && off.hits == 0 && off.misses == 0, "with pose_cache off, a display call never reaches the cache: zero hits, zero misses");

  // ---- the keys: a caller under another geometry generation is refused by the cache and by the hop --------------------------------------
  publishFrame(display + 12 * period);
  vr::TrackedDevicePose_t wrongKey{};
  HeadCall keyed; keyed.thread = me; keyed.rva = 0x4E3881; keyed.display = true;
  check(!h.locateHeadFor(h.geometryGeneration + 1, vr::TrackingUniverseSeated, 0.0f, keyed, wrongKey),
        "a display call for another geometry generation is not answered from the publication");
  PoseCacheReport keys{};
  h.poseCache.takeWindow(keys);
  check(keys.reasons[unsigned(PoseCacheMiss::Keys)] == 1, "...and the miss is counted as a key miss");

  // ---- the poll ring: order, refusal at full, counts, and a generation that is not the head's ------------------------------------------
  PollRing ring;
  PollRing::Item a, b, c, out;
  a.generation = b.generation = c.generation = 7;
  a.reset.nowMs = 1; b.reset.nowMs = 2; c.reset.nowMs = 3;
  check(ring.push(a) && ring.push(b) && ring.push(c) && ring.size() == 3, "the ring holds three resets");
  check(!ring.pop(8, out) && ring.size() == 3, "a generation other than the head's takes nothing");
  check(ring.pop(7, out) && out.reset.nowMs == 1 && ring.pop(7, out) && out.reset.nowMs == 2 && ring.pop(7, out) && out.reset.nowMs == 3 &&
        !ring.pop(7, out), "the ring drains in order, then is empty");
  for (unsigned i = 0; i < PollRing::capacity; ++i) ring.push(a);
  check(!ring.room() && !ring.push(b) && ring.size() == PollRing::capacity, "a full ring refuses a push and keeps what it holds");
  uint64_t drained = 0, dropped = 0;
  ring.takeCounts(drained, dropped);
  check(drained == 3 && dropped == 1, "the ring counts the three drained and the one refused");
  ring.clear();

  // ---- pollEvent on the caller's thread: a queued quit first, then the ring's resets, one-shot -----------------------------------------
  vr::TrackedDevicePose_t rp{};
  rp.bDeviceIsConnected = true;
  const uint64_t origin = h.poses.read().originGeneration;
  check(h.resetEvents.push(h.geometryGeneration, origin, GetTickCount64(), rp) &&
        h.resetEvents.push(h.geometryGeneration, origin, GetTickCount64(), rp),
        "(two seated resets are queued)");
  f.owner.invoke([&] { h.fillPollRing(); });
  vr::VREvent_t ev{};
  vr::TrackedDevicePose_t ep{};
  check(h.pollRing.size() == 2 && h.pollEvent(h.geometryGeneration, vr::TrackingUniverseSeated, ev, ep) &&
        ev.eventType == vr::VREvent_SeatedZeroPoseReset && h.resetPolls == 1,
        "the owner's fill moves the resets into the ring; pollEvent drains one on the caller's thread");
  f.owner.invoke([&] { h.queueQuitEvent(XR_SUCCESS); });
  check(h.pollEvent(h.geometryGeneration, vr::TrackingUniverseSeated, ev, ep) && ev.eventType == vr::VREvent_Quit,
        "a queued quit is polled before the remaining reset");
  check(h.pollEvent(h.geometryGeneration, vr::TrackingUniverseSeated, ev, ep) && ev.eventType == vr::VREvent_SeatedZeroPoseReset &&
        !h.pollEvent(h.geometryGeneration, vr::TrackingUniverseSeated, ev, ep) && !h.quitEventPending,
        "then the second reset, and nothing more: the quit is one-shot");
  f.owner.stop();
}
} // namespace edvr::openxr::test
