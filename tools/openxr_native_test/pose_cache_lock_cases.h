#pragma once
// The pose cache's answer lock is held only for counter extraction and swaps (review of main 9fe5b257, finding 3). takeWindow used to reserve
// three 4096-entry vectors (about 96 KiB) while holding the mutex that answer, publish and beginWait share, so a job-thread pose request could
// wait behind a heap call on the owner thread.
//
// The cache is instantiated here with a mutex that counts how deep the calling thread is inside it, and the executable's global operator new
// is replaced to count each allocation made at depth > 0. Nothing in the production build does either: PoseCache is BasicPoseCache<std::mutex>.
// This replaces operator new for the whole rig; it only counts and forwards to malloc.
#include "pose_cache_cases.h"
#include <cstdlib>
#include <mutex>
#include <new>

namespace edvr::openxr::test {
namespace pose_cache_lock_fixture {
inline int& lockDepth() { static thread_local int depth = 0; return depth; }
inline unsigned long long& allocsUnderLock() { static thread_local unsigned long long n = 0; return n; }
inline unsigned long long& allocsTotal() { static thread_local unsigned long long n = 0; return n; }
struct ProbeMutex {
  std::mutex m;
  void lock() { m.lock(); ++lockDepth(); }
  bool try_lock() { if (!m.try_lock()) return false; ++lockDepth(); return true; }
  void unlock() { --lockDepth(); m.unlock(); }
};
} // namespace pose_cache_lock_fixture
} // namespace edvr::openxr::test

void* operator new(size_t size) {
  using namespace edvr::openxr::test::pose_cache_lock_fixture;
  ++allocsTotal();
  if (lockDepth() > 0) ++allocsUnderLock();
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace edvr::openxr::test {

template<class Check> void runPoseCacheLockCases(Check&& check) {
  using namespace pose_cache_lock_fixture;
  using Cache = BasicPoseCache<ProbeMutex>;

  {   // the probe itself: an allocation made while the lock is held is counted, one made after it is released is not
    ProbeMutex m;
    const unsigned long long before = allocsUnderLock();
    {
      std::lock_guard<ProbeMutex> held(m);
      std::vector<int> v;
      v.reserve(64);
      check(allocsUnderLock() > before, "pose cache lock probe: an allocation inside the lock is counted (so a zero below means something)");
    }
    const unsigned long long mid = allocsUnderLock();
    std::vector<int> after;
    after.reserve(64);
    check(allocsUnderLock() == mid, "pose cache lock probe: an allocation after the lock is released is not counted");
  }

  Cache cache;
  cache.setClock(&pose_cache_fixture::clockUs);
  pose_cache_fixture::gUs.store(1000000);
  const auto record = [](uint64_t k) {
    PoseCacheRecord r;
    r.valid = true; r.keys = {1, 1}; r.display = XrTime(k) * 11111111 + 5000000000; r.period = 11111111; r.nowAtPublish = r.display;
    r.now.valid = r.next.valid = true;
    r.now.target = r.display; r.next.target = r.display + 11111111 + XrTime(k % 3) * 1000;     // a varying in-wait error: the target series fills
    const float half = float(0.01 * double(k % 7));
    r.now.orientation[0] = 0; r.now.orientation[3] = 1;
    r.next.orientation[0] = std::sin(half); r.next.orientation[3] = std::cos(half);               // ...and the angle series
    return r;
  };
  PoseCacheAnswer a;PoseCacheMiss why = PoseCacheMiss::Unpublished;
  cache.publish(record(1), pose_cache_fixture::clockUs());
  cache.answer(1, 1, 0x4E3881, a, why);     // teaches the RVA; later answers are hits

  // Several windows, each with every series filled; the second onward start from the swapped-in vectors.
  bool contentOk = true;
  unsigned long long underLock = 0, takeAllocs = 0;
  for (unsigned window = 0; window < 4; ++window) {
    for (uint64_t k = 2; k < 40; ++k) {
      cache.beginWait();
      cache.publish(record(k), pose_cache_fixture::clockUs());
      for (unsigned i = 0; i < 3; ++i) cache.answer(1, 1, 0x4E3881, a, why);
    }
    PoseCacheReport r{};
    const unsigned long long lockedBefore = allocsUnderLock(), totalBefore = allocsTotal();
    cache.takeWindow(r);
    underLock += allocsUnderLock() - lockedBefore;
    takeAllocs += allocsTotal() - totalBefore;
    contentOk = contentOk && r.hits == 38 * 3 && r.publishes == (window == 0 ? 39 : 38) && r.ageValid && r.targetValid && r.angleValid;
  }
  check(underLock == 0, "pose cache lock: takeWindow allocates nothing while holding the answer lock");
  check(takeAllocs == 0, "pose cache lock: takeWindow allocates nothing at all (two reserved sets are swapped, not rebuilt)");
  check(contentOk, "pose cache lock: the windows still report their counters and percentiles");

  PoseCacheReport empty{};
  cache.takeWindow(empty);
  check(empty.hits == 0 && empty.publishes == 0 && !empty.ageValid && !empty.targetValid && !empty.angleValid,
        "pose cache lock: a window taken straight after another is empty (the swapped-in vectors were cleared)");

  const unsigned long long lockedBefore = allocsUnderLock();
  for (uint64_t k = 100; k < 100 + 3 * Cache::kSampleCapacity; ++k) {     // past the sample cap: the series stop growing instead of reallocating
    cache.publish(record(k), pose_cache_fixture::clockUs());
    cache.answer(1, 1, 0x4E3881, a, why);
  }
  check(allocsUnderLock() == lockedBefore, "pose cache lock: answer and publish allocate nothing under the lock, even past the sample cap");
  PoseCacheReport full{};
  cache.takeWindow(full);
  check(full.ageValid && full.hits > Cache::kSampleCapacity, "pose cache lock: a window past the cap still reports");
}
} // namespace edvr::openxr::test
