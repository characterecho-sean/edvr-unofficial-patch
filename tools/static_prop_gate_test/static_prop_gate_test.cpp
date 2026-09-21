// Build gate for the StaticPropGate's cache logic -- the part the job-0
// bracket exercises at ~432 calls/frame: the change test (first-sight run,
// hit skip, change miss, count change, slot identity), the forced-refresh
// failsafe at N frames, invalidation epochs (reset + journal poll),
// oldest-evict at capacity and the SEH fault tolerance. arm/enable() is NOT
// used (it would attach hooks to the test process); state is seeded
// directly, the pattern kinematic_probe_test and
// scheduler_stack_probe_test set. Record arrays are synthetic buffers this
// process owns; the fault fixtures run against VirtualProtect guard pages.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#define private public
#include "../../src/d3d11/static_prop_gate.h"
#undef private
// The implementation under test is compiled INTO this TU: MSVC encodes
// access specifiers in decorated names, so a separately linked
// static_prop_gate.obj would not resolve the private members this rig
// seeds (the scheduler_stack_probe_test pattern).
#include "../../src/d3d11/static_prop_gate.cpp"

namespace edvr {
// Linker stubs: this rig never attaches hooks and never reads a real
// journal; the controllable journal stubs below stand in for
// journal_watch.cpp. The Log stub stands in for src\common\log.cpp
// (note() must not write files here).
uint32_t g_stubGameplay = 0, gStubDisembarks = 0, gStubEmbarks = 0,
         gStubJumpTunnel = 0;
const char* kinematicEvalStaticGateAttach() noexcept { return "installed"; }
void kinematicEvalStaticGateDetach() noexcept {}
void kinematicEvalSetStaticGateObserver(StaticGateDecideFn) noexcept {}
const char* schedulerStackGateAttach() noexcept { return "installed"; }
void schedulerStackGateDetach() noexcept {}
void schedulerStackSetResetObserver(SchedulerResetObserverFn) noexcept {}
bool journalGameplay() { return g_stubGameplay != 0; }
uint32_t journalDisembarks() { return gStubDisembarks; }
uint32_t journalEmbarks() { return gStubEmbarks; }
bool journalInJumpTunnel() { return gStubJumpTunnel != 0; }
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char*, ...) {}
} // namespace edvr

using namespace edvr;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

// Seed as enable() leaves the gate, minus the hook attach: cleared, active,
// the report timer just fired so no cadence line can fire mid-case.
void seed(StaticPropGate& g) {
    g.clearLocked();
    g.lastReportTickMs_ = GetTickCount64();
    g.active_.store(true, std::memory_order_release);
}

// A synthetic collection: `count` records at stride 0x2F0 inside one owned
// buffer, with headroom past the last record so the +0x570 truth read of
// every record stays mapped (in flight it lands in the next record's tail).
constexpr uint32_t kFakeCount = 4;
constexpr size_t kFakeBytes = size_t(kFakeCount) * StaticPropGate::kRecordStride + 0x600;

struct FakeCollection {
    std::vector<uint8_t> storage;
    uint64_t params[9]{};
    explicit FakeCollection(uint32_t count = kFakeCount): storage(kFakeBytes, 0xA5) {
        params[5] = reinterpret_cast<uint64_t>(storage.data());
        params[6] = count;
    }
    uintptr_t param() const { return reinterpret_cast<uintptr_t>(params); }
    uint8_t* record(uint32_t i) { return storage.data() + size_t(i) * StaticPropGate::kRecordStride; }
    void setTruth(uint32_t i, uint8_t v) {
        std::memset(record(i) + StaticPropGate::kTruthOffset1, v, StaticPropGate::kTruthBytes1);
        std::memset(record(i) + StaticPropGate::kTruthOffset2, uint8_t(~v),
                    StaticPropGate::kTruthBytes2);
    }
    void setCount(uint64_t count) { params[6] = count; }
};

// An inactive gate never skips: the relay is unreachable while off, and a
// direct call must agree.
void caseInactiveRuns() {
    static StaticPropGate g; // ~2 MB table: a stack local overflows the 1 MB default stack
    FakeCollection c;
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "inactive: the gate never skips while dark");
}

void caseChangeTest() {
    static StaticPropGate g; // ~2 MB table: a stack local overflows the 1 MB default stack
    seed(g);
    FakeCollection c;
    for (uint32_t i = 0; i < kFakeCount; ++i) c.setTruth(i, uint8_t(0x10 + i));
    g.notePresentFrame(100);
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "first sight: the collection runs and populates the cache");
    check(g.summary().callsRun == 1 && g.summary().cacheSize == 1,
          "first sight: counted as a run, one cached collection");
    check(g.decide(c.param()) == StaticPropGate::Verdict::Skip,
          "unchanged second call: skipped");
    check(g.summary().callsSkipped == 1, "unchanged second call: counted as a skip");
    // One truth byte at +0x170 changes: the whole collection re-runs.
    c.record(2)[StaticPropGate::kTruthOffset1 + 3] ^= 0xFF;
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "a changed position byte: run");
    check(g.summary().callsRun == 2 && g.summary().callsSkipped == 1,
          "the change is counted as a run, not a skip");
    check(g.decide(c.param()) == StaticPropGate::Verdict::Skip,
          "the refreshed cache matches again: skip");
    // A changed content-mask byte at +0x570 is a miss too.
    c.record(1)[StaticPropGate::kTruthOffset2 + 1] ^= 0xFF;
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "a changed content-mask byte: run");
    check(g.decide(c.param()) == StaticPropGate::Verdict::Skip,
          "mask refreshed: skip");
    // A new collection at a different array pointer is first-sight.
    FakeCollection other;
    for (uint32_t i = 0; i < kFakeCount; ++i) other.setTruth(i, uint8_t(0x10 + i));
    check(g.decide(other.param()) == StaticPropGate::Verdict::Run,
          "a different collection pointer: first sight, run");
    check(g.summary().cacheSize == 2, "both collections cached");
}

void caseCountChangeAndEpoch() {
    static StaticPropGate g; // ~2 MB table: a stack local overflows the 1 MB default stack
    seed(g);
    FakeCollection c;
    for (uint32_t i = 0; i < kFakeCount; ++i) c.setTruth(i, uint8_t(0x20 + i));
    g.notePresentFrame(1);
    g.decide(c.param()); // populate
    c.setCount(3);
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "a shrunk record count: run");
    check(g.decide(c.param()) == StaticPropGate::Verdict::Skip,
          "the shrunk count is the new baseline: skip");
    g.noteReset();
    check(g.summary().invalidationsReset == 1, "reset: one reset invalidation");
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "after a reset invalidation every collection re-runs once");
    check(g.decide(c.param()) == StaticPropGate::Verdict::Skip,
          "after that one run the cache is authoritative again");
}

void caseForcedRefresh() {
    static StaticPropGate g; // ~2 MB table: a stack local overflows the 1 MB default stack
    seed(g);
    FakeCollection c;
    for (uint32_t i = 0; i < kFakeCount; ++i) c.setTruth(i, uint8_t(0x30 + i));
    g.notePresentFrame(1000);
    g.decide(c.param()); // populate at frame 1000
    g.notePresentFrame(1000 + StaticPropGate::kForcedRefreshFrames - 1);
    check(g.decide(c.param()) == StaticPropGate::Verdict::Skip,
          "one frame short of N: still skipped");
    g.notePresentFrame(1000 + StaticPropGate::kForcedRefreshFrames);
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "N frames old: forced refresh runs");
    check(g.summary().forcedRefreshes == 1, "the forced refresh is counted");
    g.notePresentFrame(1000 + StaticPropGate::kForcedRefreshFrames + 1);
    check(g.decide(c.param()) == StaticPropGate::Verdict::Skip,
          "the refresh repopulated the cache: skip again");
}

void caseJournalPoll() {
    static StaticPropGate g; // ~2 MB table: a stack local overflows the 1 MB default stack
    seed(g);
    FakeCollection c;
    for (uint32_t i = 0; i < kFakeCount; ++i) c.setTruth(i, uint8_t(0x40 + i));
    g.notePresentFrame(1);
    g.decide(c.param()); // populate
    check(g.decide(c.param()) == StaticPropGate::Verdict::Skip, "baseline: skip");
    gStubDisembarks = 1; // a Disembark landed since the last poll
    g.notePresentFrame(2);
    check(g.summary().invalidationsJournal == 1, "journal: a Disembark step invalidates");
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "journal invalidation: the collection re-runs once");
    g_stubGameplay = 1; // LoadGame rising: a session boundary
    g.notePresentFrame(3);
    check(g.summary().invalidationsSession == 1, "journal: a LoadGame latch is a session invalidation");
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "session invalidation: the collection re-runs once");
    // Steady state: no further changes, no invalidations.
    g.notePresentFrame(4);
    check(g.summary().invalidationsJournal == 1 && g.summary().invalidationsSession == 1,
          "journal: steady state invalidates nothing more");
    check(g.decide(c.param()) == StaticPropGate::Verdict::Skip, "steady state: skip");
}

void caseOversizeRunsUncached() {
    static StaticPropGate g; // ~2 MB table: a stack local overflows the 1 MB default stack
    seed(g);
    FakeCollection c(StaticPropGate::kRecordsPerCollection + 1);
    g.notePresentFrame(1);
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "oversize collection: runs (the fixed pool cannot hold it)");
    check(g.decide(c.param()) == StaticPropGate::Verdict::Run,
          "oversize collection: re-runs every frame, never cached");
    const StaticPropGate::Summary s = g.summary();
    check(s.oversize == 2 && s.cacheSize == 0 && s.callsSkipped == 0,
          "oversize: counted, and nothing entered the cache");
}

void caseEvictionAtCapacity() {
    static StaticPropGate g; // ~2 MB table: a stack local overflows the 1 MB default stack
    seed(g);
    // Partition one owned buffer into cap fake single-record collections;
    // 9.5 MB of BSS, no per-case allocation.
    constexpr uint32_t kCap = StaticPropGate::kCollectionCap;
    constexpr size_t kSlotBytes = StaticPropGate::kRecordStride + 0x600;
    static uint8_t pool[size_t(kCap) * kSlotBytes];
    static uint64_t paramStorage[kCap][9];
    g.notePresentFrame(1);
    for (uint32_t k = 0; k < kCap; ++k) {
        uint8_t* base = pool + size_t(k) * kSlotBytes;
        std::memset(base + StaticPropGate::kTruthOffset1, 0x5A,
                    StaticPropGate::kTruthBytes1);
        std::memset(base + StaticPropGate::kTruthOffset2, 0xA5,
                    StaticPropGate::kTruthBytes2);
        uint64_t* p = paramStorage[k];
        p[5] = reinterpret_cast<uint64_t>(base);
        p[6] = 1;
        check(g.decide(reinterpret_cast<uintptr_t>(p)) == StaticPropGate::Verdict::Run,
              "eviction: first sight runs while filling the table");
    }
    check(g.summary().cacheSize == kCap, "eviction: the table is full");
    check(g.summary().cacheEvictions == 0, "eviction: filling evicts nothing");
    // One more distinct collection: the table is full, the oldest by
    // last-run frame goes.
    static uint8_t extra[kSlotBytes];
    static uint64_t extraParams[9];
    std::memset(extra + StaticPropGate::kTruthOffset1, 0xC3, StaticPropGate::kTruthBytes1);
    std::memset(extra + StaticPropGate::kTruthOffset2, 0x3C, StaticPropGate::kTruthBytes2);
    extraParams[5] = reinterpret_cast<uint64_t>(extra);
    extraParams[6] = 1;
    check(g.decide(reinterpret_cast<uintptr_t>(extraParams)) == StaticPropGate::Verdict::Run,
          "eviction: a collection past capacity still runs (first sight)");
    const StaticPropGate::Summary s = g.summary();
    check(s.cacheEvictions == 1, "eviction: exactly one eviction counted");
    check(s.cacheSize == kCap, "eviction: the table stays at capacity");
    check(g.decide(reinterpret_cast<uintptr_t>(extraParams)) == StaticPropGate::Verdict::Skip,
          "eviction: the newcomer caches and then skips");
}

void caseFaultTolerance() {
    static StaticPropGate g; // ~2 MB table: a stack local overflows the 1 MB default stack
    seed(g);
    g.notePresentFrame(1);
    // Two committed pages; the guard page sits right after the first. The
    // record array starts 0xB00 into page one so its +0x170 read lands but
    // its +0x570 read faults.
    uint8_t* region = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(region != nullptr, "fault: fixture pages allocated");
    if (!region) return;
    DWORD oldProtect = 0;
    check(VirtualProtect(region + 4096, 4096, PAGE_NOACCESS, &oldProtect) != 0,
          "fault: guard page armed");
    uint64_t params[9]{};
    params[5] = reinterpret_cast<uint64_t>(region + 0xB00);
    params[6] = 1;
    check(g.decide(reinterpret_cast<uintptr_t>(params)) == StaticPropGate::Verdict::Run,
          "fault: a faulted truth read runs the call (never skips on a fault)");
    check(g.summary().verifyFaults == 1, "fault: the fault is counted");
    check(g.decide(reinterpret_cast<uintptr_t>(params)) == StaticPropGate::Verdict::Run,
          "fault: a faulted read is never cached, so it re-runs");
    check(g.summary().verifyFaults == 2, "fault: repeated faults keep counting");
    // A wild count runs and is flagged, without touching the records.
    params[5] = reinterpret_cast<uint64_t>(region); // mapped, readable
    params[6] = StaticPropGate::kCountSanityCap + 1;
    check(g.decide(reinterpret_cast<uintptr_t>(params)) == StaticPropGate::Verdict::Run,
          "fault: a wild count runs the call");
    check(g.summary().verifyFaults == 3, "fault: the wild count is counted");
    VirtualFree(region, 0, MEM_RELEASE);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("static_prop_gate_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    caseInactiveRuns();
    caseChangeTest();
    caseCountChangeAndEpoch();
    caseForcedRefresh();
    caseJournalPoll();
    caseOversizeRunsUncached();
    caseEvictionAtCapacity();
    caseFaultTolerance();
    std::fprintf(stderr, "static_prop_gate_test: %u checks, %u failures\n",
                 checks, failures);
    return failures ? 1 : 0;
}
