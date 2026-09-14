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
#include "../../src/common/shutdown_present_census.h"

namespace {
void require(bool ok, const char* why) {
    if (!ok) { std::printf("FAIL: %s\n", why); std::exit(1); }
}

using Budget = edvr::VrCensusBudget<3>;
constexpr auto kShutdownVersion = edvr::kShutdownCensusVersion;

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

void testShutdownWindows() {
    edvr::ShutdownPresentCensus census;
    edvr::ShutdownCensusSnapshot snapshot{};
    require(!census.begin(1, true) && !census.begin(kShutdownVersion, false), "old shutdown protocol and disabled gate reject");
    require(!census.end(0, &snapshot), "no active shutdown window");
    census.notePresent(); // Outside-window observations must not leak into one.
    auto token = census.begin(kShutdownVersion, true);
    require(token && !census.begin(kShutdownVersion, true), "exactly one active shutdown window");
    auto invalid = snapshot;
    invalid.version = 1;
    const auto beforeInvalid = invalid;
    require(!census.end(token, &invalid) && !std::memcmp(&invalid, &beforeInvalid, sizeof(invalid)),
            "old snapshot version left entirely untouched");
    invalid = snapshot;
    --invalid.size;
    require(!census.end(token, &invalid) && invalid.size == sizeof(snapshot) - 1,
            "bad snapshot size left untouched");
    require(!census.end(token, nullptr) && !census.end(token + 1, &snapshot), "invalid end cannot steal window");
    require(census.end(token, &snapshot) && snapshot.token == token && snapshot.samples == 0 &&
            snapshot.beginQpc > 0 && snapshot.beginQpc <= snapshot.endQpc &&
            !snapshot.firstQpc && !snapshot.lastQpc && !snapshot.firstThread && !snapshot.lastThread,
            "measured zero window retains honest bounds and empty sample fields");
    for (unsigned i = 1; i < edvr::ShutdownPresentCensus::kMaxWindows; ++i) {
        const auto previous = token;
        token = census.begin(kShutdownVersion, true);
        require(token > previous && !census.end(previous, &snapshot), "fresh token rejects prior generation");
        census.notePresent();
        require(census.end(token, &snapshot) && snapshot.samples == 1 &&
                snapshot.firstThread == GetCurrentThreadId() && snapshot.firstThread == snapshot.lastThread &&
                snapshot.beginQpc <= snapshot.firstQpc && snapshot.firstQpc == snapshot.lastQpc &&
                snapshot.lastQpc <= snapshot.endQpc && !snapshot.mixedThreads && !snapshot.saturated,
                "new shutdown generation resets sample state");
    }
    require(!census.begin(kShutdownVersion, true) && !census.end(token, &snapshot), "64 windows exhaust without token reuse");
}

void testConcurrentShutdownSamples() {
    edvr::ShutdownPresentCensus census;
    const auto token = census.begin(kShutdownVersion, true);
    require(token != 0, "concurrent shutdown window begins");
    census.notePresent(); // Known first and last caller surround the workers.
    constexpr unsigned kWorkers = 8, kSamples = 1000;
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < kWorkers; ++i) workers.emplace_back([&] {
        for (unsigned j = 0; j < kSamples; ++j) census.notePresent();
    });
    for (auto& worker : workers) worker.join();
    census.notePresent();
    edvr::ShutdownCensusSnapshot result{};
    require(census.end(token, &result) && result.samples == kWorkers * kSamples + 2,
            "concurrent shutdown samples retain exact coherent count");
    require(result.firstThread == GetCurrentThreadId() && result.lastThread == result.firstThread &&
            result.mixedThreads == 1 && !result.saturated &&
            result.beginQpc <= result.firstQpc && result.firstQpc <= result.lastQpc && result.lastQpc <= result.endQpc,
            "mixed callers detected even when first and last caller match");
}

void testRenderLifetime() {
    using State = edvr::ShutdownOwnerState;
    edvr::ShutdownPresentCensus census;
    edvr::ShutdownCensusSnapshot result{};
    auto token = census.begin(kShutdownVersion, true);
    require(census.end(token, &result) && result.renderBegin.ownerState == State::Unobserved &&
            result.renderEnd.ownerState == State::Unobserved && !result.renderEnd.ownerThread,
            "no observed owner is distinct from an exited thread");
    census.enterPresent();
    census.noteOwner();
    token = census.begin(kShutdownVersion, true);
    census.notePresent();
    census.leavePresent();
    require(census.end(token, &result), "active Present window ends");
    const auto& begin = result.renderBegin;
    const auto& end = result.renderEnd;
    require(begin.ownerState == State::Alive && end.ownerState == State::Alive &&
            begin.ownerThread == GetCurrentThreadId() && end.ownerThread == begin.ownerThread &&
            !begin.ownerError && !end.ownerChanged, "retained handle identifies the live owner");
    require(begin.activePresents == 1 && begin.enteredPresents == 1 && !begin.exitedPresents &&
            !end.activePresents && end.enteredPresents == 1 && end.exitedPresents == 1,
            "Present entered before Begin is still counted as active");
    require(begin.lastEnterThread == GetCurrentThreadId() && end.lastExitThread == begin.lastEnterThread &&
            begin.lastEnterQpc <= result.beginQpc && end.lastServiceQpc >= result.beginQpc &&
            end.lastServiceQpc <= end.lastExitQpc && end.lastExitQpc <= result.endQpc,
            "whole-Present times and service time retain their different meanings");

    // A failed or TEST call still represents another owned-Present caller,
    // even though it never calls noteOwner or reaches the native service point.
    std::thread other([&] { census.enterPresent(); census.leavePresent(); });
    other.join();
    token = census.begin(kShutdownVersion, true);
    require(census.end(token, &result) && result.renderEnd.ownerChanged &&
            result.renderEnd.ownerThread == GetCurrentThreadId() && result.renderEnd.ownerState == State::Alive &&
            !result.renderEnd.activePresents && !result.renderEnd.activityInvalid,
            "a changed caller never replaces the original retained thread");
}

void testExitedRenderLifetime() {
    DWORD before = 0, after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "read process handle count");
    for (unsigned i = 0; i < 16; ++i) {
        edvr::ShutdownPresentCensus census;
        DWORD renderThread = 0;
        std::thread render([&] {
            renderThread = GetCurrentThreadId();
            census.enterPresent(); census.noteOwner(); census.notePresent(); census.leavePresent();
        });
        render.join(); // The retained handle must remain valid after the original handle closes.
        const auto token = census.begin(kShutdownVersion, true);
        edvr::ShutdownCensusSnapshot result{};
        require(census.end(token, &result) && result.renderBegin.ownerState == edvr::ShutdownOwnerState::Exited &&
                result.renderEnd.ownerState == edvr::ShutdownOwnerState::Exited &&
                result.renderEnd.ownerThread == renderThread && !result.renderEnd.ownerChanged &&
                !result.renderEnd.ownerError && !result.renderEnd.activePresents &&
                result.renderEnd.enteredPresents == 1 && result.renderEnd.exitedPresents == 1,
                "thread termination is observed through the retained real handle");
    }
    require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE && after == before,
            "retained thread handles close with their observer");
}
} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected --self-test");
    require(std::strcmp(argv[1], "--self-test") == 0, "expected --self-test");
    testExhaustionAndTransition();
    testLimitsAndInvalid();
    testConcurrentSaturation();
    testConcurrentTransition();
    testShutdownWindows();
    testConcurrentShutdownSamples();
    testRenderLifetime();
    testExitedRenderLifetime();
    std::puts("PASS: OpenXR census budgets, shutdown windows, retained render-thread lifetime and activity, invalid inputs, and concurrency.");
}
