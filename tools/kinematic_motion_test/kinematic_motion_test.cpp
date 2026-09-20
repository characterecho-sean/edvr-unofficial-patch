// Build gate for the production kinematic tracker (src/d3d11/kinematic_motion.cpp).
// The tracker decides which records are proven-static; a wrong label becomes a
// wrong motion vector in stage B, and compilation cannot catch a state-machine
// bug. The rig feeds synthetic eval-hook streams through the real module and
// asserts the emitted labels and counters (the 8 cases of the 2026-09-20
// 10:52 spec in docs/kinematic-motion-injection-2026-09-19.md).
#include "../../src/d3d11/kinematic_motion.h"
#include "../../src/d3d11/kinematic_eval_hook.h"
#include "../../src/common/log.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <vector>

namespace edvr {
// Linker stubs: the rig never hooks anything (the tracker is driven directly),
// and the log swallows lines. The pattern is set by mesh_motion_test.
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char*, ...) {}
const char* kinematicEvalTrackerAttach() noexcept { return "installed"; }
void kinematicEvalTrackerDetach() noexcept {}
void kinematicEvalSetTrackerObserver(KinematicTrackerObserverFn) noexcept {}
}
using namespace edvr;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

// The record as the tracker sees it: node at +0x18, bounds at +0xB0 (128 B),
// pose at +0x170 (20 B: 3 float translation + 4 uint16 quat lanes),
// center at +0x240. Descriptor carries the record pointer at +0x10.
struct FakeRecord { alignas(16) uint8_t b[0x280]; };
struct FakeDesc { uint8_t b[0x60]; };

FakeDesc descFor(FakeRecord& r) {
    FakeDesc d{};
    const uintptr_t p = reinterpret_cast<uintptr_t>(&r);
    std::memcpy(d.b + 0x10, &p, 8);
    return d;
}
void setNode(FakeRecord& r, uint64_t node) { std::memcpy(r.b + 0x18, &node, 8); }
void setPose(FakeRecord& r, float x, float y, float z,
             uint16_t q0, uint16_t q1, uint16_t q2, uint16_t q3) {
    std::memcpy(r.b + 0x170, &x, 4);
    std::memcpy(r.b + 0x174, &y, 4);
    std::memcpy(r.b + 0x178, &z, 4);
    std::memcpy(r.b + 0x17C, &q0, 2);
    std::memcpy(r.b + 0x17E, &q1, 2);
    std::memcpy(r.b + 0x180, &q2, 2);
    std::memcpy(r.b + 0x182, &q3, 2);
}
void setBounds(FakeRecord& r, uint8_t seed) {
    for (unsigned i = 0; i < 128; ++i) r.b[0xB0 + i] = static_cast<uint8_t>(seed + i);
}
void initRecord(FakeRecord& r, uint64_t node, float x, float y, float z, uint8_t boundsSeed) {
    std::memset(&r, 0, sizeof(r));
    setNode(r, node);
    setPose(r, x, y, z, 0, 0, 0, 65535); // identity quat
    setBounds(r, boundsSeed);
}
uint64_t ptrOf(FakeRecord& r) { return reinterpret_cast<uint64_t>(&r); }

uint32_t g_frame = 5000;
void seedClock() { kinematicMotionNotePresentFrame(g_frame); } // seeds, counts nothing
void endFrame() { kinematicMotionNotePresentFrame(++g_frame); } // ends the frame just run
void fresh() {
    kinematicMotionConfigure(false);
    kinematicMotionConfigure(true);
    seedClock();
}

void caseFanout() {
    fresh();
    FakeRecord a; initRecord(a, 0xA1, 100.f, 200.f, 300.f, 1);
    FakeDesc d = descFor(a);
    for (int i = 0; i < 11; ++i) kinematicMotionObserve(reinterpret_cast<uintptr_t>(&d));
    KinematicMotionStats s = kinematicMotionStats();
    check(s.observed == 11, "1: observed counts every call");
    check(s.dupInFrame == 10, "1: fan-out dups counted");
    endFrame();
    s = kinematicMotionStats();
    check(s.framesCounted == 1 && s.seenLast == 1 && s.tracked == 1,
          "1: 11 calls are one record, one frame");
    check(s.eligibleLast == 0, "1: first-sight baseline is never eligible");
}

void caseBitExactStatic() {
    fresh();
    FakeRecord a; initRecord(a, 0xA2, 100.f, 200.f, 300.f, 2);
    FakeDesc d = descFor(a);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    kinematicMotionObserve(dp); endFrame();          // baseline, run 0
    kinematicMotionObserve(dp); endFrame();          // run 1
    kinematicMotionObserve(dp); endFrame();          // run 2
    KinematicMotionStats s = kinematicMotionStats();
    check(s.eligibleLast == 0, "2: two change-free frames are not enough");
    kinematicMotionObserve(dp);                      // run 3
    check(kinematicMotionRecordEligible(ptrOf(a)), "2: bit-exact 3 frames -> eligible");
    endFrame();
    s = kinematicMotionStats();
    check(s.eligibleLast == 1 && s.eligibleFrames == 1, "2: eligibility counted per frame");
    check(s.poseChanges == 0 && s.moversTotal == 0, "2: a static record is not a mover");
}

void caseNearMiss() {
    fresh();
    FakeRecord b; initRecord(b, 0xB3, 100.f, 200.f, 300.f, 3);
    FakeDesc d = descFor(b);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    kinematicMotionObserve(dp); endFrame();          // baseline
    // 1-ulp wobble on translation.x: ~7.6e-6 m at this magnitude
    float x = 100.f;
    for (int i = 0; i < 3; ++i) {
        uint32_t bits; std::memcpy(&bits, &x, 4); bits ^= 1u; std::memcpy(&x, &bits, 4);
        setPose(b, x, 200.f, 300.f, 0, 0, 0, 65535);
        kinematicMotionObserve(dp); endFrame();
    }
    const KinematicMotionStats s = kinematicMotionStats();
    check(s.translationChanges == 3, "3: 1-ulp wobble is a translation change");
    check(s.nearMiss == 3, "3: sub-1mm wobble counted as near-miss");
    check(s.eligibleLast == 0 && !kinematicMotionRecordEligible(ptrOf(b)),
          "3: 1-ulp wobble is never eligible");
    check(s.quatOnlyChanges == 0, "3: quat untouched, no quat-only count");
    check(s.moversTotal == 1, "3: a wobbling record is a mover");
}

void caseQuatChurn() {
    fresh();
    FakeRecord c; initRecord(c, 0xC4, 100.f, 200.f, 300.f, 4);
    FakeDesc d = descFor(c);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    kinematicMotionObserve(dp); endFrame();          // baseline
    for (uint16_t i = 0; i < 3; ++i) {
        // Constant translation, quat lanes churn: the 103339 rotating-in-place
        // class. Never eligible, ever.
        setPose(c, 100.f, 200.f, 300.f, static_cast<uint16_t>(100 + i * 97),
                static_cast<uint16_t>(55127 - i * 131), 34165, 9639);
        kinematicMotionObserve(dp); endFrame();
    }
    const KinematicMotionStats s = kinematicMotionStats();
    check(s.quatOnlyChanges == 3 && s.translationChanges == 0,
          "4: quat-only churn classified");
    check(s.nearMiss == 0, "4: zero translation delta is no near-miss");
    check(s.eligibleLast == 0 && !kinematicMotionRecordEligible(ptrOf(c)),
          "4: rotating-in-place is never eligible (103339 regression case)");
    check(s.moversTotal == 1, "4: a quat churner is a mover");
}

void caseGap() {
    fresh();
    FakeRecord e; initRecord(e, 0xE5, 100.f, 200.f, 300.f, 5);
    FakeDesc d = descFor(e);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    for (int i = 0; i < 4; ++i) { kinematicMotionObserve(dp); endFrame(); } // run 3
    endFrame();                                    // a frame absent
    KinematicMotionStats s = kinematicMotionStats();
    check(s.zeroRecordFrames == 1, "5: the absent frame reads as zero-record");
    kinematicMotionObserve(dp);                    // back after one gap frame
    s = kinematicMotionStats();
    check(s.gapDrops == 1, "5: gap-resume is a drop");
    check(!kinematicMotionRecordEligible(ptrOf(e)), "5: a gap kills the label");
    endFrame();
    kinematicMotionObserve(dp); endFrame();        // run 1
    kinematicMotionObserve(dp); endFrame();        // run 2
    kinematicMotionObserve(dp);                    // run 3
    check(kinematicMotionRecordEligible(ptrOf(e)), "5: re-proven after 3 clean frames");
}

void caseNodeChange() {
    fresh();
    FakeRecord f; initRecord(f, 0xF6, 100.f, 200.f, 300.f, 6);
    FakeDesc d = descFor(f);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    for (int i = 0; i < 3; ++i) { kinematicMotionObserve(dp); endFrame(); }
    kinematicMotionObserve(dp);                    // run 3, mid-frame
    check(kinematicMotionRecordEligible(ptrOf(f)), "6: eligible before the swap");
    endFrame();
    setNode(f, 0xF7);                              // same pointer, different node
    kinematicMotionObserve(dp);
    const KinematicMotionStats s = kinematicMotionStats();
    check(s.nodeChanges == 1, "6: node change counted");
    check(!kinematicMotionRecordEligible(ptrOf(f)), "6: slot reuse is a new identity");
    check(s.zeroRecordFrames == 0, "6: no gap frame here");
}

void caseOverflow() {
    fresh();
    std::vector<FakeRecord> many(4100);
    std::vector<FakeDesc> descs(4100);
    for (size_t i = 0; i < many.size(); ++i) {
        initRecord(many[i], 0x9000 + i, 1.f, 2.f, 3.f, static_cast<uint8_t>(i));
        descs[i] = descFor(many[i]);
        kinematicMotionObserve(reinterpret_cast<uintptr_t>(&descs[i]));
    }
    endFrame();
    const KinematicMotionStats s = kinematicMotionStats();
    check(s.tracked == 4096, "7: table capped at 4096");
    check(s.recordOverflow == 4, "7: excess counted, never evicted");
    check(s.seenLast == 4096, "7: overflow records are not seen");
}

void caseZeroRecordFrame() {
    fresh();
    FakeRecord g; initRecord(g, 0x68, 1.f, 2.f, 3.f, 7);
    FakeDesc d = descFor(g);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    kinematicMotionObserve(dp); endFrame();
    kinematicMotionObserve(dp); endFrame();
    KinematicMotionStats s = kinematicMotionStats();
    check(s.zeroRecordFrames == 0, "8: healthy frames have records");
    endFrame();                                    // nothing observed
    s = kinematicMotionStats();
    check(s.zeroRecordFrames == 1, "8: a zero-record frame is a stand-down count");
    kinematicMotionObserve(dp);
    s = kinematicMotionStats();
    check(s.gapDrops == 1, "8: re-sight after the dead frame is a new identity");
}

// 2026-09-20 review finding 1: a same-frame dup used to return before the
// identity/pose reads, so an eligible record kept its label through a node
// swap or a translation change inside one Present.
void caseSameFrameNodeSwap() {
    fresh();
    FakeRecord a; initRecord(a, 0xD1, 100.f, 200.f, 300.f, 8);
    FakeDesc d = descFor(a);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    for (int i = 0; i < 3; ++i) { kinematicMotionObserve(dp); endFrame(); }
    kinematicMotionObserve(dp);                    // run 3, mid-frame
    check(kinematicMotionRecordEligible(ptrOf(a)), "9: eligible before the same-frame swap");
    setNode(a, 0xD2);
    kinematicMotionObserve(dp);                    // same frame, new node
    KinematicMotionStats s = kinematicMotionStats();
    check(s.nodeChanges == 1, "9: same-frame node swap counted");
    check(s.sameFrameChanges == 1, "9: same-frame invalidation counted");
    check(!kinematicMotionRecordEligible(ptrOf(a)), "9: the label dies with the node");
    check(s.dupInFrame == 1, "9: the dup itself still counts");
}

void caseSameFramePoseChange() {
    fresh();
    FakeRecord a; initRecord(a, 0xD3, 100.f, 200.f, 300.f, 9);
    FakeDesc d = descFor(a);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    for (int i = 0; i < 3; ++i) { kinematicMotionObserve(dp); endFrame(); }
    kinematicMotionObserve(dp);                    // run 3, mid-frame
    setPose(a, 115.f, 200.f, 300.f, 0, 0, 0, 65535);
    kinematicMotionObserve(dp);                    // same frame, moved 15 m
    KinematicMotionStats s = kinematicMotionStats();
    check(s.poseChanges == 1 && s.translationChanges == 1,
          "10: same-frame translation change counted");
    check(s.sameFrameChanges == 1, "10: same-frame invalidation counted");
    check(!kinematicMotionRecordEligible(ptrOf(a)), "10: the label dies with the pose");
    kinematicMotionObserve(dp); endFrame();        // pose stable again: run 1
    kinematicMotionObserve(dp); endFrame();        // run 2
    check(!kinematicMotionRecordEligible(ptrOf(a)), "10: re-prove restarts from zero");
}

void caseIdenticalDupStillDedups() {
    fresh();
    FakeRecord a; initRecord(a, 0xD4, 100.f, 200.f, 300.f, 10);
    FakeDesc d = descFor(a);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    kinematicMotionObserve(dp); kinematicMotionObserve(dp); kinematicMotionObserve(dp);
    KinematicMotionStats s = kinematicMotionStats();
    check(s.dupInFrame == 2 && s.sameFrameChanges == 0 && s.poseChanges == 0,
          "11: an invariant dup is still just a dup");
}

// 2026-09-20 review finding 6: part A used to fire on the first frame with
// >=8 movers -- frame 2 of a session, before any static can be eligible --
// and the one-shot dump completed with zero static comparisons.
void caseBoundsDumpWaitsForStatics() {
    fresh();
    FakeRecord mv[8], st[8];
    FakeDesc dm[8], ds[8];
    for (int i = 0; i < 8; ++i) {
        initRecord(mv[i], 0xE00 + i, 10.f * i, 2.f, 3.f, 20 + i);
        initRecord(st[i], 0xE80 + i, 500.f, 600.f, 700.f + i, 40 + i);
        dm[i] = descFor(mv[i]); ds[i] = descFor(st[i]);
    }
    auto observeAll = [&]() {
        for (int i = 0; i < 8; ++i) {
            kinematicMotionObserve(reinterpret_cast<uintptr_t>(&dm[i]));
            kinematicMotionObserve(reinterpret_cast<uintptr_t>(&ds[i]));
        }
    };
    observeAll(); endFrame();                      // baselines
    KinematicMotionStats s = kinematicMotionStats();
    check(kinematicMotionBoundsDumpStage() == 0, "12: no dump without movers");
    for (int i = 0; i < 8; ++i) setPose(mv[i], 10.f * i + 5.f, 2.f, 3.f, 0, 0, 0, 65535);
    observeAll(); endFrame();                      // 8 movers, statics run 1
    s = kinematicMotionStats();
    check(s.moversLast == 8 && s.eligibleLast == 0, "12: movers before statics qualify");
    check(kinematicMotionBoundsDumpStage() == 0,
          "12: movers alone do not fire part A (finding 6)");
    for (int i = 0; i < 8; ++i) setPose(mv[i], 10.f * i + 6.f, 2.f, 3.f, 0, 0, 0, 65535);
    observeAll(); endFrame();                      // statics run 2
    check(kinematicMotionBoundsDumpStage() == 0, "12: two change-free frames still not enough");
    for (int i = 0; i < 8; ++i) setPose(mv[i], 10.f * i + 7.f, 2.f, 3.f, 0, 0, 0, 65535);
    observeAll(); endFrame();                      // statics run 3 -> eligible
    s = kinematicMotionStats();
    check(s.eligibleLast == 8, "12: eight statics eligible at run 3");
    check(kinematicMotionBoundsDumpStage() == 1, "12: part A fires once both qualify");
    observeAll(); endFrame();
    check(kinematicMotionBoundsDumpStage() == 2, "12: part B completes the one-shot dump");
}

void caseConfigLifecycle() {
    kinematicMotionConfigure(false);
    check(!kinematicMotionActive(), "0: off is inactive");
    kinematicMotionConfigure(true);
    check(kinematicMotionActive(), "0: on is live");
    kinematicMotionConfigure(true);                // the 1 Hz re-poll
    check(kinematicMotionActive(), "0: re-poll configure is idempotent");
    kinematicMotionConfigure(false);
    check(!kinematicMotionActive(), "0: off stands down");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("kinematic_motion_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    caseConfigLifecycle();
    caseFanout();
    caseBitExactStatic();
    caseNearMiss();
    caseQuatChurn();
    caseGap();
    caseNodeChange();
    caseOverflow();
    caseZeroRecordFrame();
    caseSameFrameNodeSwap();
    caseSameFramePoseChange();
    caseIdenticalDupStillDedups();
    caseBoundsDumpWaitsForStatics();
    kinematicMotionShutdown();
    std::printf("kinematic_motion_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
