#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../src/common/gpu_span_state.h"

using namespace edvr;
using R = GpuSpanReason;
static unsigned checks = 0;
static void check(bool pass, const char* why) {
    ++checks;
    if (!pass) { std::printf("FAIL: %s\n", why); std::exit(1); }
}
static constexpr GpuSpanOwner owner{1, 2, 3, true};

// Values come from the actual issued markers. Missing, duplicate or reordered
// markers cannot be hidden by a canned valid sample. The fake also asserts
// resource and outer-scope ownership at every driver entry.
struct Driver final : GpuSpanDriver {
    struct Slot { bool made = false, open = false; unsigned mask = 0; GpuSpanRawSample raw{}; };
    std::array<Slot, 8> slots{};
    unsigned creates = 0, destroys = 0, begins = 0, ends = 0, polls = 0, calls = 0;
    int liveOuter = -1, failStamp = -1, pendingSlot = -1;
    bool pending = true, partial = false, failCreate = false, failBegin = false, failEnd = false;
    bool failPoll = false, equalTicks = false;
    uint64_t tick = UINT64_MAX - 100000;
    enum class Corrupt { None, Disjoint, Frequency, Outer, Eye, Outside, Order } corrupt = Corrupt::None;
    std::vector<unsigned> markers;
    bool create(unsigned i) noexcept override {
        ++calls; ++creates;
        check(!slots[i].made, "query resources not created over live resources");
        if (failCreate) return false;
        slots[i].made = true;
        return true;
    }
    bool begin(unsigned i) noexcept override {
        ++calls; ++begins;
        check(slots[i].made && liveOuter < 0, "only one live outer scope");
        if (failBegin) return false;
        slots[i].open = true;
        slots[i].mask = 0;
        slots[i].raw = GpuSpanRawSample{};
        slots[i].raw.frequency = 1000;
        liveOuter = static_cast<int>(i);
        return true;
    }
    bool timestamp(unsigned i, unsigned index) noexcept override {
        ++calls;
        check(index < 6 && liveOuter == static_cast<int>(i), "timestamp inside its outer scope");
        check((slots[i].mask & (1u << index)) == 0, "timestamp index is issued once");
        markers.push_back(index);
        if (static_cast<int>(index) == failStamp) return false;
        slots[i].mask |= 1u << index;
        slots[i].raw.ticks[index] = tick;
        // Distinct eyes AND frames expose swapped or mixed delayed results.
        if (!equalTicks) tick += index == 2 ? 11 + begins : index == 4 ? 17 + begins : 10;
        return true;
    }
    bool end(unsigned i) noexcept override {
        ++calls; ++ends;
        check(liveOuter == static_cast<int>(i) && slots[i].open, "end has matching begin");
        if (failEnd) return false; // Closure is deliberately left uncertain.
        slots[i].open = false;
        liveOuter = -1;
        return true;
    }
    GpuSpanPoll poll(unsigned i, GpuSpanRawSample& raw) noexcept override {
        ++calls; ++polls;
        check(slots[i].made && !slots[i].open, "only closed query resources are polled");
        if (pending || static_cast<int>(i) == pendingSlot) return GpuSpanPoll::Pending;
        if (failPoll) return GpuSpanPoll::Failed;
        raw = slots[i].raw;
        raw.timestampsReady = !partial;
        switch (corrupt) {
        case Corrupt::Disjoint: raw.disjoint = true; break;
        case Corrupt::Frequency: raw.frequency = 0; break;
        case Corrupt::Outer: raw.ticks[1] = raw.ticks[0] - 1; break;
        case Corrupt::Eye: raw.ticks[3] = raw.ticks[2] - 1; break;
        case Corrupt::Outside: raw.ticks[5] = raw.ticks[1] + 1; break;
        case Corrupt::Order: raw.ticks[3] = raw.ticks[4] + 1; break;
        default: break;
        }
        return GpuSpanPoll::Ready;
    }
    void destroy(unsigned i) noexcept override {
        ++calls; ++destroys;
        check(slots[i].made, "destroy exactly one owned query set");
        slots[i] = Slot{};
        if (liveOuter == static_cast<int>(i)) liveOuter = -1;
    }
};
struct Fixture {
    Driver driver;
    GpuSpanState state{driver, owner};
    GpuSpanState::Results results{};
    ~Fixture() {
        state.shutdown(100, owner);
        check(driver.liveOuter < 0, "no outer scope after cleanup");
        for (const auto& slot : driver.slots) check(!slot.made, "no resources leaked after cleanup");
    }
    void pair(uint64_t seq, unsigned first = 0, uint64_t closed = 10) {
        check(state.beginFrame(seq, seq + 100, 0, owner) == R::Valid, "open frame");
        check(state.beginEye(first, 1, owner) == R::Valid, "first eye begin");
        check(state.endEye(first, 2, owner) == R::Valid, "first eye end");
        check(driver.liveOuter >= 0, "first eye leaves outer open");
        check(state.beginEye(1 - first, 3, owner) == R::Valid, "second eye begin");
        check(state.endEye(1 - first, closed, owner) == R::Valid, "second eye end");
        check(driver.liveOuter >= 0, "second eye leaves outer open for final submit work");
        check(state.finishFrame(closed, owner) == R::Valid, "finish complete stereo frame");
        check(driver.liveOuter < 0, "explicit final boundary closes outer");
    }
};

static void validAndReuse() {
    for (unsigned first : {0u, 1u}) {
        Fixture f; f.pair(1, first);
        const std::vector<unsigned> expected = first == 0 ?
            std::vector<unsigned>{0, 2, 3, 4, 5, 1} : std::vector<unsigned>{0, 4, 5, 2, 3, 1};
        check(f.driver.markers == expected && f.driver.ends == 1, "all six markers in exact order");
        check(f.state.poll(10, owner, f.results) == 0, "pending remains pending");
        f.driver.pending = false; f.driver.partial = true;
        check(f.state.poll(11, owner, f.results) == 0 && f.driver.destroys == 0,
              "partial timestamp readiness cannot publish or free");
        f.driver.partial = false;
        check(f.state.poll(12, owner, f.results) == 1, "ready result");
        const auto& r = f.results[0];
        check(r.reason == R::Valid && r.sequence == 1 && r.sourceFrame == 101 &&
            r.completedAtMs == 12 && r.ageMs == 2 && r.source == GpuSpanSource::RenderToSubmit,
            "source, original frame and elapsed age preserved");
        check(r.outerMs == 60 && r.leftMs == 12 && r.rightMs == 18,
              "integer subtraction preserves short intervals above 2^53");
        check(f.state.beginFrame(1, 0, 12, owner) == R::OldSequence, "drained sequence stays retired");
        f.pair(2, first);
        check(f.driver.creates == 1, "completed slot resources reused");
        check(f.state.poll(13, owner, f.results) == 1 && f.results[0].sequence == 2 &&
              f.results[0].leftMs == 13 && f.results[0].rightMs == 19, "reuse has fresh identity and eye durations");
    }
    Fixture zero; zero.driver.equalTicks = true; zero.pair(1); zero.driver.pending = false;
    check(zero.state.poll(10, owner, zero.results) == 1 && zero.results[0].reason == R::Valid &&
          zero.results[0].outerMs == 0, "equal timestamps are a valid zero span");
}
static void pairing() {
    for (unsigned mode = 0; mode < 6; ++mode) {
        Fixture f;
        check(f.state.beginFrame(1, 1, 0, owner) == R::Valid, "pairing fixture opens");
        R result{};
        if (mode == 0) result = f.state.beginEye(2, 1, owner);
        else if (mode == 1) result = f.state.endEye(0, 1, owner);
        else {
            check(f.state.beginEye(0, 1, owner) == R::Valid, "pairing first eye begins");
            if (mode == 2) result = f.state.beginEye(1, 2, owner); // Overlap.
            if (mode == 3) result = f.state.endEye(1, 2, owner); // Wrong end.
            if (mode >= 4) {
                check(f.state.endEye(0, 2, owner) == R::Valid, "pairing first eye ends");
                result = mode == 4 ? f.state.beginEye(0, 3, owner) : f.state.endEye(0, 3, owner);
            }
        }
        check(result == R::BadPair && f.driver.ends == 1, "malformed pair closes exactly once as invalid");
        f.driver.pending = false;
        check(f.state.poll(4, owner, f.results) == 1 && f.results[0].reason == R::BadPair &&
              f.results[0].outerMs == 0, "bad pair never publishes a duration");
        f.pair(2);
        check(f.driver.creates == 1, "closed invalid frame can reuse ready resources");
    }
    Fixture missing;
    check(missing.state.beginFrame(1, 1, 0, owner) == R::Valid, "missing-eye frame");
    check(missing.state.beginEye(0, 1, owner) == R::Valid, "unfinished first eye");
    check(missing.state.beginFrame(2, 2, 1000, owner) == R::Valid && missing.driver.ends == 1,
          "new boundary closes incomplete frame before another begin");
    missing.driver.pending = false;
    check(missing.state.poll(1001, owner, missing.results) == 1 &&
          missing.results[0].reason == R::Incomplete && missing.results[0].ageMs == 1,
          "incomplete age starts at closure, not old frame start");
}
static void failures() {
    for (int index = 0; index < 6; ++index) {
        Fixture f; f.driver.failStamp = index;
        R r = f.state.beginFrame(1, 1, 0, owner);
        if (r == R::Valid) r = f.state.beginEye(0, 1, owner);
        if (r == R::Valid) r = f.state.endEye(0, 2, owner);
        if (r == R::Valid) r = f.state.beginEye(1, 3, owner);
        if (r == R::Valid) r = f.state.endEye(1, 4, owner);
        if (r == R::Valid) r = f.state.finishFrame(4, owner);
        check(r == R::DriverFailure && f.driver.ends == 1 && f.driver.liveOuter < 0,
              "every timestamp failure attempts outer closure once");
        f.driver.pending = false;
        check(f.state.poll(5, owner, f.results) == 1 && f.results[0].reason == R::DriverFailure &&
              f.driver.destroys == 1, "failed timestamp resources retire");
    }
    Fixture create; create.driver.failCreate = true;
    check(create.state.beginFrame(1, 1, 0, owner) == R::CreateFailed && create.driver.begins == 0,
          "create failure never issues begin");
    create.driver.failCreate = false; create.pair(2);
    Fixture begin; begin.driver.failBegin = true;
    check(begin.state.beginFrame(1, 1, 0, owner) == R::DriverFailure && begin.driver.ends == 0,
          "failed begin is not ended");
    check(begin.state.poll(1, owner, begin.results) == 1 && begin.driver.destroys == 1,
          "failed begin resources owned and destroyed");
    Fixture end; end.driver.failEnd = true;
    check(end.state.beginFrame(1, 1, 0, owner) == R::Valid, "end failure fixture");
    check(end.state.beginFrame(2, 2, 1, owner) == R::DriverFailure && end.driver.begins == 1,
          "failed closure prevents opening next frame");
    check(end.state.beginFrame(3, 3, 2, owner) == R::Stopped, "failed closure is terminal");
    check(end.state.poll(2, owner, end.results) == 1 && end.driver.polls == 0 &&
          end.results[0].reason == R::DriverFailure, "terminal failure reports without querying uncertain scope");
    check(end.state.shutdown(3, owner) && end.driver.ends == 1, "cleanup never retries uncertain end");
    for (unsigned c = 1; c <= 6; ++c) {
        Fixture f; f.pair(1); f.driver.pending = false;
        f.driver.corrupt = static_cast<Driver::Corrupt>(c);
        const R want = c == 1 ? R::Disjoint : c == 2 ? R::ZeroFrequency : R::BadTimestamps;
        check(f.state.poll(10, owner, f.results) == 1 && f.results[0].reason == want &&
              f.results[0].leftMs == 0 && f.driver.destroys == 1, "invalid data never becomes timing");
    }
    Fixture poll; poll.pair(1); poll.driver.pending = false; poll.driver.failPoll = true;
    check(poll.state.poll(10, owner, poll.results) == 1 && poll.results[0].reason == R::DriverFailure,
          "GetData failure retires explicitly");
}
static void finalBoundary() {
    Fixture f;
    check(f.state.beginFrame(1, 101, 0, owner) == R::Valid, "final boundary frame");
    check(f.state.beginEye(0, 1, owner) == R::Valid && f.state.endEye(0, 2, owner) == R::Valid &&
          f.state.beginEye(1, 3, owner) == R::Valid && f.state.endEye(1, 4, owner) == R::Valid,
          "both EDVR intervals complete before final submit");
    f.driver.pending = false;
    check(f.state.poll(4, owner, f.results) == 0, "complete eyes are not a closed frame");
    f.driver.tick += 100; // Simulated runtime/final-copy work outside EDVR intervals.
    check(f.state.finishFrame(5, owner) == R::Valid &&
          f.state.finishFrame(5, owner) == R::NoOpenFrame, "finish closes exactly once");
    check(f.state.poll(6, owner, f.results) == 1 && f.results[0].outerMs == 160 &&
          f.results[0].leftMs == 12 && f.results[0].rightMs == 18,
          "post-eye work extends only the outer interval");
    for (unsigned mode = 0; mode < 3; ++mode) {
        Fixture partial;
        check(partial.state.beginFrame(1, 1, 0, owner) == R::Valid, "early finish frame");
        if (mode) check(partial.state.beginEye(0, 1, owner) == R::Valid, "early finish eye");
        if (mode == 2) check(partial.state.endEye(0, 2, owner) == R::Valid, "one complete eye");
        check(partial.state.finishFrame(3, owner) == R::Incomplete, "unfinished stereo pair invalid");
        partial.driver.pending = false;
        check(partial.state.poll(4, owner, partial.results) == 1 &&
              partial.results[0].reason == R::Incomplete && partial.results[0].outerMs == 0,
              "early final boundary never publishes a valid duration");
    }
    Fixture rejected;
    check(rejected.state.beginFrame(1, 1, 0, owner) == R::Valid &&
          rejected.state.invalidateFrame(1, owner) == R::Incomplete,
          "rejected submit can explicitly invalidate the open span");
}
static void pressureAndOwner() {
    Fixture f;
    for (uint64_t i = 1; i <= 8; ++i) f.pair(i, static_cast<unsigned>(i % 2), 100);
    const unsigned before = f.driver.calls;
    check(f.state.beginFrame(9, 9, 101, owner) == R::RingFull && f.driver.calls == before,
          "full ring skips without reusing pending queries");
    for (unsigned i = 0; i < 100; ++i) check(f.state.poll(100, owner, f.results) == 0, "API calls cannot age samples");
    check(f.state.poll(2100, owner, f.results) == 0, "age equal to maximum is allowed");
    check(f.state.poll(2101, owner, f.results) == 8 && f.driver.destroys == 8,
          "strictly older pending samples retire by elapsed time");
    for (const auto& r : f.results) check(r.reason == R::Stale && r.ageMs == 2001, "stale provenance");
    check(f.state.beginFrame(9, 9, 2102, owner) == R::OldSequence, "skipped sequence remains consumed");
    f.pair(10); check(f.driver.creates == 9, "stale slot uses new resources");

    Fixture separate; separate.pair(1); separate.pair(2, 1);
    separate.driver.pending = false; separate.driver.pendingSlot = 0;
    check(separate.state.poll(10, owner, separate.results) == 1 && separate.results[0].sequence == 2 &&
          separate.results[0].leftMs == 13 && separate.results[0].rightMs == 19,
          "later completed frame keeps its own two eyes");
    separate.driver.pendingSlot = -1;
    check(separate.state.poll(11, owner, separate.results) == 1 && separate.results[0].sequence == 1 &&
          separate.results[0].leftMs == 12 && separate.results[0].rightMs == 18,
          "older result retains original sequence when it becomes ready");
    for (const GpuSpanOwner bad : {GpuSpanOwner{9,2,3,true}, {1,9,3,true}, {1,2,9,true}, {1,2,3,false}}) {
        Fixture wrong;
        const unsigned calls = wrong.driver.calls;
        check(wrong.state.beginFrame(1,1,0,bad) == R::WrongOwner &&
              wrong.state.beginEye(0,0,bad) == R::WrongOwner &&
              wrong.state.endEye(0,0,bad) == R::WrongOwner &&
              wrong.state.finishFrame(0,bad) == R::WrongOwner &&
              wrong.state.invalidateFrame(0,bad) == R::WrongOwner &&
              wrong.state.poll(0,bad,wrong.results) == 0 && !wrong.state.shutdown(0,bad) &&
              wrong.driver.calls == calls, "wrong owner cannot access driver through any entry");
        wrong.pair(1);
    }
    for (const GpuSpanOwner invalid : {GpuSpanOwner{}, {0,2,3,true}, {1,0,3,true}, {1,2,0,true}, {1,2,3,false}}) {
        Driver d; GpuSpanState s(d, invalid);
        check(s.beginFrame(1,1,0,invalid) == R::WrongOwner && d.calls == 0, "invalid binding never initializes driver");
    }
    Fixture stopped;
    check(stopped.state.shutdown(0, owner), "explicit shutdown");
    check(!stopped.state.shutdown(0, owner) && stopped.state.beginFrame(1,1,0,owner) == R::Stopped &&
          stopped.state.poll(0,owner,stopped.results) == 0, "shutdown terminal and idempotent cleanup");
}
int main(int argc, char** argv) {
    check(argc == 2 && std::strcmp(argv[1], "--self-test") == 0, "expected --self-test");
    validAndReuse(); pairing(); failures(); finalBoundary(); pressureAndOwner();
    std::printf("PASS: %u GPU span CPU policy and command-driven fake-driver checks (no GPU measurement).\n", checks);
    return 0;
}
