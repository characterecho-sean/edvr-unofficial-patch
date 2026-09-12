#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "../../src/common/vr_census_budget.h"

namespace {
void require(bool ok, const char* why) {
    if (!ok) { std::printf("FAIL: %s\n", why); std::exit(1); }
}

using Budget = edvr::VrCensusBudget<3>;

void testExhaustionAndTransition() {
    Budget b;
    edvr::VrCensusPhase phase{}; std::uint32_t sample = 0;
    for (unsigned i = 1; i <= 2; ++i)
        require(b.take(0, 2, phase, sample) && phase == edvr::VrCensusPhase::Startup && sample == i,
                "startup samples are 1..limit");
    require(!b.take(0, 2, phase, sample), "startup exhaustion");
    require(b.beginVr(), "first beginVr wins");
    require(!b.beginVr() && b.phase() == edvr::VrCensusPhase::Vr, "phase transition is idempotent");
    require(b.take(0, 2, phase, sample) && phase == edvr::VrCensusPhase::Vr && sample == 1,
            "VR bank starts independently");
    require(b.take(0, 2, phase, sample) && sample == 2, "VR bank reaches its own limit");
    require(!b.take(0, 2, phase, sample), "VR bank exhausts independently");
    require(!b.take(0, 2, phase, sample), "repeated take cannot refill either bank");
}

void testLimitsAndInvalid() {
    Budget b;
    edvr::VrCensusPhase phase{}; std::uint32_t sample = 99;
    require(!b.take(99, 4, phase, sample) && sample == 0, "invalid event is ignored");
    require(!b.take(1, 0, phase, sample) && sample == 0, "zero limit is ignored");
    require(b.take(1, 1, phase, sample) && sample == 1, "separate event has own limit");
    require(!b.take(1, 1, phase, sample), "separate event saturates");
    require(b.take(2, 3, phase, sample) && sample == 1, "third event remains available");
}

void testConcurrentSaturation() {
    Budget b;
    constexpr unsigned kThreads = 8, kAttempts = 1000, kLimit = 64;
    std::atomic<unsigned> successes{0};
    std::vector<std::uint32_t> samples;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < kThreads; ++t) workers.emplace_back([&] {
        for (unsigned i = 0; i < kAttempts; ++i) {
            edvr::VrCensusPhase phase{}; std::uint32_t sample = 0;
            if (b.take(0, kLimit, phase, sample)) {
                require(phase == edvr::VrCensusPhase::Startup, "concurrent startup phase");
                successes.fetch_add(1, std::memory_order_relaxed);
                // Each successful claim is checked after joining below.
                static std::mutex mutex;
                std::lock_guard<std::mutex> lock(mutex);
                samples.push_back(sample);
            }
        }
    });
    for (auto& worker : workers) worker.join();
    require(successes.load() == kLimit && samples.size() == kLimit, "concurrent count saturates");
    std::set<std::uint32_t> unique(samples.begin(), samples.end());
    require(unique.size() == kLimit && *unique.begin() == 1 && *unique.rbegin() == kLimit,
            "concurrent samples are unique and bounded");
    require(b.beginVr(), "concurrent test can transition once");
    edvr::VrCensusPhase phase{}; std::uint32_t sample = 0;
    require(b.take(0, kLimit, phase, sample) && phase == edvr::VrCensusPhase::Vr && sample == 1,
            "transition gives a fresh VR budget");
}

void testConcurrentTransition() {
    Budget b;
    constexpr unsigned kThreads = 8, kVrLimit = kThreads;
    std::atomic<unsigned> ready{0}, beginWinners{0};
    std::atomic<bool> go{false};
    std::mutex mutex;
    std::vector<std::uint32_t> vrSamples;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < kThreads; ++t) workers.emplace_back([&] {
        edvr::VrCensusPhase phase{}; std::uint32_t sample = 0;
        require(b.take(0, 64, phase, sample) && phase == edvr::VrCensusPhase::Startup,
                "workers claim startup before transition");
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        if (b.beginVr()) beginWinners.fetch_add(1, std::memory_order_relaxed);
        require(b.take(1, kVrLimit, phase, sample) && phase == edvr::VrCensusPhase::Vr,
                "racing workers claim from VR bank");
        std::lock_guard<std::mutex> lock(mutex);
        vrSamples.push_back(sample);
    });
    for (unsigned spins = 0; ready.load(std::memory_order_acquire) != kThreads && spins < 1000000; ++spins)
        std::this_thread::yield();
    require(ready.load(std::memory_order_acquire) == kThreads, "all workers reached transition gate");
    go.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    require(beginWinners.load(std::memory_order_relaxed) == 1, "exactly one beginVr winner");
    require(vrSamples.size() == kThreads, "all racing VR claims recorded");
    std::set<std::uint32_t> unique(vrSamples.begin(), vrSamples.end());
    require(unique.size() == kThreads && *unique.begin() == 1 && *unique.rbegin() == kVrLimit,
            "racing VR samples are unique and bounded");
}
} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected --self-test");
    require(std::strcmp(argv[1], "--self-test") == 0, "expected --self-test");
    testExhaustionAndTransition();
    testLimitsAndInvalid();
    testConcurrentSaturation();
    testConcurrentTransition();
    std::puts("PASS: OpenXR census budget startup/VR banks, invalid inputs, and concurrency.");
}
