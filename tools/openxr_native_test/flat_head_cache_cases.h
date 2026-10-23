#pragma once
// The flat head locate (locateHeadFor's flat route, publishFlatHead, PoseCache::answerFlat, and the flat_cache_* fields of flat_owner_waits),
// through the real host on its own owner thread against the fake XR runtime, with the pose cache's clock and the flat gate set by the rig. While
// flat a locate on any thread reads the owner's per-headset-frame publication and never hops; one it cannot answer hops and is counted by
// reason; VR keeps the old path.
#include "flat_owner_waits_cases.h"
#include "pose_cache_cases.h"

namespace edvr::openxr::test {

template<class Check> void runFlatHeadCacheCases(Check&& check) {
  using namespace launch_fixture;
  using namespace head_pose_fixture;
  using namespace pose_cache_fixture;
  using namespace flatwaits_detail;
  OwnerBuiltFixture f;
  check(f.route.bind() && f.owner.start() && f.owner.invoke([&] { f.build(); }) && f.initialized,
        "(the flat head cache cases' host is built on its own owner thread)");
  if (!f.initialized) return;
  auto& h = *f.hostPtr;
  h.headClock = readClock;
  h.poseCache.setClock(&clockUs);
  h.poseCacheOn.store(true);
  // The host's own instrument, replaced by one on the rig's fake flat gate and clock.
  auto waits = std::make_shared<FlatOwnerWaits>(hooks());
  h.flatWaits = waits;
  f.owner.setFlatWaits(waits);
  gFlat = true; gNowUs = 1000; tTid = 0; clearLines();
  const DWORD me = GetCurrentThreadId();
  const XrTime display = 5000000000, period = 11111111, now = display - 42000000;
  setNow(now);
  f.fake.head.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
  const auto locate = [&](vr::TrackedDevicePose_t& p, uint32_t rva, bool displayCall = true,
                          vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseSeated, uint64_t generation = ~uint64_t(0)) {
    HeadCall call; call.thread = me; call.rva = rva; call.display = displayCall;
    p = {};
    return h.locateHeadFor(generation == ~uint64_t(0) ? h.geometryGeneration : generation, origin, 0.0f, call, p);
  };
  const auto publishFlat = [&](XrTime d) { return f.owner.invoke([&] { h.publishFlatHead(d, period); }); };

  // ---- flat, nothing published: the hop, counted as unpublished ----------------------------------------------------------------------------------
  gUs = 1000000;
  vr::TrackedDevicePose_t hopped{}, cached{}, p{};
  unsigned locates = f.fake.locates;
  check(locate(hopped, 0x4E3921) && f.fake.locates == locates + 1, "flat, nothing published: the locate takes the owner hop");
  auto w = waits->take();
  check(w.cacheFallbacks == 1 && w.cacheFallbackReasons[unsigned(FlatOwnerWaits::CacheFallback::Unpublished)] == 1 && w.cacheAnswers == 0 &&
        w.jobs == 1 && w.sites[unsigned(OwnerJobSite::LocateHead)].n == 1,
        "flat, nothing published: counted as one fallback (unpublished) and one locate_head owner job");

  // ---- flat, published: answered on the caller's thread, no hop, the age reported -----------------------------------------------------------------
  check(publishFlat(display), "(a flat head frame is published on the owner)");
  waits->take();   // the publication was made by an owner job of this rig's own: leave it out of the window
  gUs += 40000;
  locates = f.fake.locates;
  check(locate(cached, 0x4E3921) && f.fake.locates == locates, "flat, published: the locate is answered from the publication, no owner locate");
  w = waits->take();
  check(w.cacheAnswers == 1 && w.cacheFallbacks == 0 && w.jobs == 0 && std::fabs(w.cacheAgeP50Ms - 40.0) < 1e-6 &&
        std::fabs(w.cacheAgeP95Ms - 40.0) < 1e-6 && std::fabs(w.cacheAgeMaxMs - 40.0) < 1e-6,
        "flat, published: one answer, no owner job, and its age (40 ms) in p50, p95 and max");
  check(cached.bPoseIsValid, "flat, published: the answered pose is valid");
  gUs += 60000;
  locates = f.fake.locates;
  check(locate(p, 0x4E1111, false) && f.fake.locates == locates && p.bPoseIsValid,
        "flat, published: a call that is not a display call is answered from the publication too");
  check(!locate(p, 0x4E1111, true, vr::TrackingUniverseStanding) && f.fake.locates == locates,
        "flat, published: a locate for another origin is refused locally, as the hop would refuse it");
  w = waits->take();
  check(w.cacheAnswers == 1 && w.cacheRefused == 1 && w.jobs == 0 && std::fabs(w.cacheAgeMaxMs - 100.0) < 1e-6,
        "flat: the second answer is 100 ms old, the refusal is counted, and neither reached the owner");

  // The publication is the head at the empty frame's own display time, the instant the hop would locate at.
  gFlat = false; h.poseCacheOn.store(false);
  vr::TrackedDevicePose_t viaHop{};
  check(locate(viaHop, 0x4E3921) && f.fake.lastTime == display && viaHop.bPoseIsValid &&
        std::memcmp(&viaHop.mDeviceToAbsoluteTracking, &cached.mDeviceToAbsoluteTracking, sizeof(viaHop.mDeviceToAbsoluteTracking)) == 0,
        "the published flat pose equals the hop's pose, located at the empty frame's display time");
  gFlat = true; h.poseCacheOn.store(true);
  waits->take();

  // ---- stale and other keys: the hop, by reason ------------------------------------------------------------------------------------------------
  gUs += 1500000;
  locates = f.fake.locates;
  check(locate(p, 0x4E3921) && f.fake.locates == locates + 1, "flat, a publication older than the cap: the locate takes the hop");
  check(!locate(p, 0x4E3921, true, vr::TrackingUniverseSeated, h.geometryGeneration + 1), "flat, other geometry keys: the locate takes the hop");
  w = waits->take();
  check(w.cacheFallbacks == 2 && w.cacheFallbackReasons[unsigned(FlatOwnerWaits::CacheFallback::Stale)] == 1 &&
        w.cacheFallbackReasons[unsigned(FlatOwnerWaits::CacheFallback::Keys)] == 1 && w.cacheAnswers == 0,
        "flat: the stale and the other-keys locate are counted under their reasons");
  f.owner.invoke([&] { h.dropPoseFrame(); });
  locates = f.fake.locates;
  check(locate(p, 0x4E3921) && f.fake.locates == locates + 1 && waits->take().cacheFallbackReasons[unsigned(FlatOwnerWaits::CacheFallback::Unpublished)] == 1,
        "flat, publication dropped: the locate takes the hop (unpublished)");

  // ---- the key off in flat: the old hop, no flat counters ----------------------------------------------------------------------------------------
  check(publishFlat(display), "(a flat head frame is published again)");
  h.poseCacheOn.store(false);
  locates = f.fake.locates;
  check(locate(p, 0x4E3921) && f.fake.locates == locates + 1, "flat with pose_cache off: the locate takes the hop as before");
  w = waits->take();
  check(w.cacheAnswers == 0 && w.cacheFallbacks == 0 && w.cacheRefused == 0, "flat with pose_cache off: no flat cache figure is counted");
  h.poseCacheOn.store(true);

  // ---- VR: the old path, unchanged ---------------------------------------------------------------------------------------------------------------
  gFlat = false;
  f.owner.invoke([&] { h.dropPoseFrame(); });
  const XrVector3f turn{0.0f, 0.5235988f, 0.0f};
  check(f.owner.invoke([&] {
          h.publishPoseGap(display, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, XR_SPACE_VELOCITY_ANGULAR_VALID_BIT, turn, 1000, [](const char*) {});
          h.publishPoseCache();
        }), "(a VR frame and its publication are made on the owner)");
  waits->take();
  locates = f.fake.locates;
  check(locate(p, 0x4E3881) && f.fake.locates == locates + 1 && f.fake.lastTime == display + period,
        "VR: a caller's first display call takes the hop, located at display + period, as before");
  check(locate(p, 0x4E3881) && f.fake.locates == locates + 1, "VR: the same caller's next display call is answered from the publication, as before");
  w = waits->take();
  check(w.cacheAnswers == 0 && w.cacheFallbacks == 0 && w.cacheRefused == 0, "VR: no flat cache figure is counted");
  PoseCacheReport report{};
  h.poseCache.takeWindow(report);
  check(report.hits == 1, "VR: the cache's own hit counter counted the answer");

  // ---- back to flat: the flat route takes over at once -----------------------------------------------------------------------------------------
  gFlat = true;
  check(publishFlat(display), "(a flat head frame is published after the switch back)");
  locates = f.fake.locates;
  check(locate(p, 0x4E5555) && f.fake.locates == locates && waits->take().cacheAnswers == 1, "VR to flat: the next locate is answered by the flat route");
  f.owner.stop();
  gFlat = true;
}

} // namespace edvr::openxr::test
