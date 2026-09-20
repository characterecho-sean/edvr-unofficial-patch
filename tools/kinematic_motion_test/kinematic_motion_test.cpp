// Build gate for the production kinematic tracker (src/d3d11/kinematic_motion.cpp).
// The tracker decides which records are proven-static; a wrong label becomes a
// wrong motion vector in stage B, and compilation cannot catch a state-machine
// bug. The rig feeds synthetic eval-hook streams through the real module and
// asserts the emitted labels and counters (the 8 cases of the 2026-09-20
// 10:52 spec plus the stage-B sphere/upload cases of the 13:55 spec, both in
// docs/kinematic-motion-injection-2026-09-19.md).
#include "../../src/d3d11/kinematic_motion.h"
#include "../../src/d3d11/kinematic_eval_hook.h"
#include "../../src/common/log.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <cmath>
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

// The record as the tracker sees it: node at +0x18, bounds at +0xB0 (128 B;
// the block carries the world transform -- rotation rows at +0xF0, translation
// at +0x120), pose at +0x170 (20 B: 3 float translation + 4 uint16 quat
// lanes), center at +0x240, local sphere at +0x270 (centre float4) and +0x280
// (radius float4, lanes 1-3 padding). Descriptor carries the record pointer
// at +0x10. 0x2C0 so the 32-byte sphere read at +0x270 stays in bounds.
struct FakeRecord { alignas(16) uint8_t b[0x2C0]; };
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
// The world transform inside the bounds block: three padded float4 rows at
// +0xF0 (block +0x40), translation at +0x120 (block +0x70). rows is 9 floats
// (3 rows x xyz); the pad lanes write zero.
void setTransform(FakeRecord& r, const float* rows, const float* t) {
    for (int i = 0; i < 3; ++i) {
        std::memcpy(r.b + 0xF0 + i * 16, rows + i * 3, 12);
        const float pad = 0.f;
        std::memcpy(r.b + 0xF0 + i * 16 + 12, &pad, 4);
    }
    std::memcpy(r.b + 0x120, t, 12);
}
// The local bounding sphere: centre float3 at +0x270 (lane 3 stays zero),
// radius float at +0x280.
void setSphere(FakeRecord& r, float cx, float cy, float cz, float radius) {
    std::memcpy(r.b + 0x270, &cx, 4);
    std::memcpy(r.b + 0x274, &cy, 4);
    std::memcpy(r.b + 0x278, &cz, 4);
    std::memcpy(r.b + 0x280, &radius, 4);
}
void initRecord(FakeRecord& r, uint64_t node, float x, float y, float z, uint8_t boundsSeed) {
    std::memset(&r, 0, sizeof(r));
    setNode(r, node);
    setPose(r, x, y, z, 0, 0, 0, 65535); // identity quat
    setBounds(r, boundsSeed);            // NOTE: garbage transform -- no real sphere map
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
    check(s.sphereChanges == 0, "2: an untouched sphere is no sphere change");
    check(s.sphereRejected == 1 && s.uploadedLast == 0 && s.uploadGeneration == 0,
          "2: eligible but sphere-less (zero radius) uploads nothing, rejection counted");
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

// -- Stage B (2026-09-20 13:55 spec): the sphere is part of the stasis
// compare, and eligible records' world spheres publish to the upload snapshot.

// Spec case 1: the sphere has a writer independent of pose. An LOD refresh
// (sphere bytes change, pose bit-static) invalidates exactly like a pose
// change: the label dies, the upload drops the record at the next ended
// frame, and 3 clean frames re-prove it with the NEW sphere uploaded.
void caseLodSwapInvalidation() {
    fresh();
    FakeRecord a; initRecord(a, 0x1A, 100.f, 200.f, 300.f, 30);
    const float rot[9] = { 1,0,0, 0,1,0, 0,0,1 };
    const float t[3] = { 1000.f, 2000.f, 3000.f };
    setTransform(a, rot, t);
    setSphere(a, 1.f, 2.f, 3.f, 4.f);
    FakeDesc d = descFor(a);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    for (int i = 0; i < 3; ++i) { kinematicMotionObserve(dp); endFrame(); } // runs 0..2
    kinematicMotionObserve(dp);                 // run 3, mid-frame
    check(kinematicMotionRecordEligible(ptrOf(a)), "13: eligible before the LOD refresh");
    endFrame();                                 // the frame's census publishes the sphere
    KinematicSphereGpu sbuf[1]; uint32_t count = 0, sframe = 0;
    uint64_t gen = kinematicMotionSphereSnapshot(sbuf, 1, &count, &sframe);
    check(count == 1 && gen == 1, "13: the static publishes one sphere");
    check(sbuf[0].radius == 4.f && sbuf[0].centre[0] == 1001.f &&
          sbuf[0].centre[1] == 2002.f && sbuf[0].centre[2] == 3003.f,
          "13: the world sphere is R^T x local + T (identity case)");
    setSphere(a, 1.f, 2.f, 3.f, 4.5f);          // the LOD refresh: pose untouched
    kinematicMotionObserve(dp);
    KinematicMotionStats s = kinematicMotionStats();
    check(s.sphereChanges == 1, "13: the sphere change is counted");
    check(s.poseChanges == 0 && s.moversTotal == 0, "13: an LOD refresh is not a mover");
    check(!kinematicMotionRecordEligible(ptrOf(a)), "13: the label dies with the sphere");
    endFrame();
    gen = kinematicMotionSphereSnapshot(sbuf, 1, &count, &sframe);
    check(count == 0 && gen == 2, "13: the upload drops the record at the ended frame");
    kinematicMotionObserve(dp); endFrame();     // run 1
    kinematicMotionObserve(dp); endFrame();     // run 2
    kinematicMotionObserve(dp);                 // run 3
    check(kinematicMotionRecordEligible(ptrOf(a)), "13: re-proven after 3 clean frames");
    endFrame();
    gen = kinematicMotionSphereSnapshot(sbuf, 1, &count, &sframe);
    check(count == 1 && gen == 3 && sbuf[0].radius == 4.5f,
          "13: the re-proved record uploads the NEW sphere");
}

// Spec case 2: the 132856 flight fixtures anchor the transpose convention
// against any future "cleanup": R rows / T / local sphere -> the world centre
// the dump showed at +0x240, asserted under 1 mm.
void caseFlightFixtures() {
    fresh();
    FakeRecord recs[3];
    const float r0[9] = { 0.0649095997f, -0.312504649f, 0.947695911f,      // id=143
                          -0.192730457f, 0.927891433f, 0.319174558f,
                          -0.979102492f, -0.203367367f, -9.93261224e-08f };
    const float t0[3] = { -80.0905457f, 68.4580307f, -232.082138f };
    const float r1[9] = { -0.979102492f, -0.203367487f, 2.42143869e-07f,   // id=147
                          -0.192685574f, 0.927675307f, 0.319829315f,
                          -0.0650431067f, 0.313145638f, -0.947475195f };
    const float t1[3] = { -257.662079f, 44.2694321f, -267.809143f };
    const float r2[9] = { 0.0612900965f, 0.336678147f, -0.939623058f,      // id=149
                          -0.19268547f, 0.927675605f, 0.319828689f,
                          0.979344606f, 0.161449358f, 0.121730298f };
    const float t2[3] = { -146.491287f, 66.0164032f, -281.153717f };
    const float* rots[3] = { r0, r1, r2 };
    const float* ts[3] = { t0, t1, t2 };
    const float lcs[3][3] = { { -2.50109434f, 2.1240499f, -1.44535255f },
                              { -2.50109434f, 2.1240499f, -1.44535255f },
                              { -0.000706672668f, -0.254451215f, -0.00101515651f } };
    const float radii[3] = { 3.77981496f, 3.77981496f, 6.59033394f };
    const float expect[3][3] = { { -79.2471161f, 71.5044556f, -233.77446f },
                                 { -255.528519f, 46.2958946f, -265.760376f },
                                 { -146.443298f, 65.779953f, -281.234558f } };
    FakeDesc ds[3];
    for (int i = 0; i < 3; ++i) {
        initRecord(recs[i], 0x143 + i, 0.f, 0.f, 0.f, static_cast<uint8_t>(50 + i));
        setTransform(recs[i], rots[i], ts[i]);
        setSphere(recs[i], lcs[i][0], lcs[i][1], lcs[i][2], radii[i]);
        ds[i] = descFor(recs[i]);
    }
    for (int f = 0; f < 4; ++f) {
        for (int i = 0; i < 3; ++i) kinematicMotionObserve(reinterpret_cast<uintptr_t>(&ds[i]));
        endFrame();
    }
    KinematicSphereGpu buf[4]; uint32_t count = 0, sframe = 0;
    kinematicMotionSphereSnapshot(buf, 4, &count, &sframe);
    check(count == 3, "14: all three flight fixtures upload");
    bool centresOk = count == 3, radiiOk = count == 3;
    for (int i = 0; i < 3; ++i) {
        for (int a = 0; a < 3; ++a)
            if (std::fabs(buf[i].centre[a] - expect[i][a]) > 0.001f) centresOk = false;
        // unit column scale observed on 132856: world radius == local radius
        if (std::fabs(buf[i].radius - radii[i]) > 0.001f) radiiOk = false;
    }
    check(centresOk, "14: world centres match the dumped +0x240 values under 1 mm");
    check(radiiOk, "14: unit column scale leaves the radius untouched");
    check(count == 3 && buf[0].kind == 0 && buf[0].prevMap[0] == 0.f &&
          buf[0].reserved[0] == 0, "14: the phase-1 seam fields are zero");
}

// Spec case 3: the world radius scales by the max 3x3 column scale.
void caseRadiusScale() {
    fresh();
    FakeRecord a; initRecord(a, 0x2B, 0.f, 0.f, 0.f, 60);
    const float rot[9] = { 2,0,0, 0,2,0, 0,0,2 }; // column scale 2
    const float t[3] = { 10.f, 20.f, 30.f };
    setTransform(a, rot, t);
    setSphere(a, 1.f, 1.f, 1.f, 3.f);
    FakeDesc d = descFor(a);
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    for (int i = 0; i < 4; ++i) { kinematicMotionObserve(dp); endFrame(); }
    KinematicSphereGpu buf[1]; uint32_t count = 0, sframe = 0;
    kinematicMotionSphereSnapshot(buf, 1, &count, &sframe);
    check(count == 1 && std::fabs(buf[0].radius - 6.f) < 1e-4f,
          "15: column scale 2 doubles the uploaded radius");
    check(count == 1 && std::fabs(buf[0].centre[0] - 12.f) < 1e-3f &&
          std::fabs(buf[0].centre[1] - 22.f) < 1e-3f &&
          std::fabs(buf[0].centre[2] - 32.f) < 1e-3f,
          "15: the centre maps through the scaled matrix");
}

// Spec case 4: the generation is content-addressed -- bump on set or member
// change, hold when idle; a record losing eligibility shrinks the set.
void caseUploadGeneration() {
    fresh();
    FakeRecord a, b;
    const float rot[9] = { 1,0,0, 0,1,0, 0,0,1 };
    const float t[3] = { 0.f, 0.f, 0.f };
    initRecord(a, 0x3C, 0.f, 0.f, 0.f, 70); setTransform(a, rot, t); setSphere(a, 0,0,0, 2.f);
    initRecord(b, 0x3D, 0.f, 0.f, 0.f, 71); setTransform(b, rot, t); setSphere(b, 5,5,5, 3.f);
    FakeDesc da = descFor(a), db = descFor(b);
    const uintptr_t pa = reinterpret_cast<uintptr_t>(&da);
    const uintptr_t pb = reinterpret_cast<uintptr_t>(&db);
    for (int i = 0; i < 4; ++i) { kinematicMotionObserve(pa); kinematicMotionObserve(pb); endFrame(); }
    KinematicSphereGpu buf[2]; uint32_t count = 0, sframe = 0;
    const uint64_t gen = kinematicMotionSphereSnapshot(buf, 2, &count, &sframe);
    check(count == 2 && gen == 1, "16: the eligible set publishes once");
    const uint32_t firstFrame = sframe;
    kinematicMotionObserve(pa); kinematicMotionObserve(pb); endFrame(); // idle frame
    uint64_t gen2 = kinematicMotionSphereSnapshot(buf, 2, &count, &sframe);
    check(gen2 == gen && count == 2 && sframe == firstFrame + 1,
          "16: an unchanged set uploads nothing (the generation holds, the frame advances)");
    setPose(a, 50.f, 0.f, 0.f, 0, 0, 0, 65535);   // a starts moving
    kinematicMotionObserve(pa); kinematicMotionObserve(pb); endFrame();
    gen2 = kinematicMotionSphereSnapshot(buf, 2, &count, &sframe);
    check(gen2 == gen + 1 && count == 1, "16: a mover leaving eligibility bumps the generation");
    check(std::fabs(buf[0].centre[0] - 5.f) < 1e-6f, "16: the remaining sphere is b's");
    setSphere(b, 5.f, 5.f, 5.f, 3.25f);           // b LOD-refreshes mid-stream
    for (int i = 0; i < 4; ++i) { kinematicMotionObserve(pb); endFrame(); }
    gen2 = kinematicMotionSphereSnapshot(buf, 2, &count, &sframe);
    check(count == 1 && gen2 == gen + 3 && std::fabs(buf[0].radius - 3.25f) < 1e-6f,
          "16: a member sphere change bumps the generation (drop + republish)");
}

// Spec case 5: radius <= 0 or non-finite sphere bytes never upload, and the
// rejection is counted. The eligible label itself is untouched -- the record
// IS bit-static; it just has nothing the coverage pass may use.
void caseImplausibleSphereRejected() {
    fresh();
    FakeRecord z, n, m;
    const float rot[9] = { 1,0,0, 0,1,0, 0,0,1 };
    const float t[3] = { 0.f, 0.f, 0.f };
    const float nan = std::nanf("");
    initRecord(z, 0x4E, 0.f, 0.f, 0.f, 80); setTransform(z, rot, t); setSphere(z, 0,0,0, 0.f);
    initRecord(n, 0x4F, 0.f, 0.f, 0.f, 81); setTransform(n, rot, t); setSphere(n, nan, 0.f, 0.f, 2.f);
    initRecord(m, 0x50, 0.f, 0.f, 0.f, 82); setTransform(m, rot, t); setSphere(m, 0,0,0, nan);
    FakeDesc dz = descFor(z), dn = descFor(n), dm = descFor(m);
    const uintptr_t pz = reinterpret_cast<uintptr_t>(&dz);
    const uintptr_t pn = reinterpret_cast<uintptr_t>(&dn);
    const uintptr_t pm = reinterpret_cast<uintptr_t>(&dm);
    for (int i = 0; i < 4; ++i) {
        kinematicMotionObserve(pz); kinematicMotionObserve(pn); kinematicMotionObserve(pm);
        endFrame();
    }
    const KinematicMotionStats s = kinematicMotionStats();
    check(s.eligibleLast == 3, "17: implausible spheres still prove static (the label is pose truth)");
    check(s.sphereRejected == 3, "17: every rejection is counted at the rebuild");
    check(s.uploadedLast == 0 && s.uploadGeneration == 0, "17: nothing implausible ever uploads");
    KinematicSphereGpu buf[1]; uint32_t count = 1, sframe = 0;
    kinematicMotionSphereSnapshot(buf, 1, &count, &sframe);
    check(count == 0, "17: the published set is empty");
}

// 2026-09-20 review finding 4: the generation restarts from zero on a
// tracker restart, so the GPU upload cache keys on (session, generation).
// The session epoch must change across any state reset and hold within a
// session, or an off/on cycle can re-publish different spheres under a
// generation the renderer already holds.
void caseSessionEpoch() {
    fresh();
    const uint32_t s0 = kinematicMotionSession();
    check(kinematicMotionSession() == s0, "18: the session holds within a session");
    FakeRecord a;
    const float rot[9] = { 1,0,0, 0,1,0, 0,0,1 };
    const float t[3] = { 0.f, 0.f, 0.f };
    initRecord(a, 0x5A, 0.f, 0.f, 0.f, 90); setTransform(a, rot, t); setSphere(a, 0,0,0, 2.f);
    FakeDesc da = descFor(a);
    const uintptr_t pa = reinterpret_cast<uintptr_t>(&da);
    for (int i = 0; i < 4; ++i) { kinematicMotionObserve(pa); endFrame(); }
    uint32_t count = 0, sframe = 0;
    check(kinematicMotionSphereSnapshot(nullptr, 0, &count, &sframe) == 1 && count == 1,
          "18: a published set before the restart (generation 1)");
    fresh();   // shutdown + configure: state reset twice over
    const uint32_t s1 = kinematicMotionSession();
    check(s1 != s0, "18: a restart bumps the session epoch");
    check(kinematicMotionSphereSnapshot(nullptr, 0, &count, &sframe) == 0 && count == 0,
          "18: the restarted tracker has nothing published");
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
    caseLodSwapInvalidation();
    caseFlightFixtures();
    caseRadiusScale();
    caseUploadGeneration();
    caseImplausibleSphereRejected();
    caseSessionEpoch();
    kinematicMotionShutdown();
    std::printf("kinematic_motion_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
