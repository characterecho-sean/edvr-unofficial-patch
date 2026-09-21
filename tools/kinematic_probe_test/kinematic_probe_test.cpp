// Build gate for the KinematicEvalProbe's observe/clock logic -- the four
// probe-side findings of the 2026-09-20 review (2, 3, 7, 9), reproduced
// against the production source. The JSON writer's own gate
// (kinematic_json_test) only exercises serialization; these cases drive
// observe() itself. arm()/finish() are NOT used (they would hook the test
// process); state is seeded directly, the pattern the review harness used.
// VirtualProtect read-fault fixtures run on pages this process owns.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#define private public
#include "../../src/d3d11/kinematic_eval_probe.h"
#undef private
#include "../../src/d3d11/kinematic_eval_hook.h"
// The implementation under test is compiled INTO this TU: MSVC encodes
// access specifiers in decorated names, so a separately linked
// kinematic_eval_probe.obj would mangle clearLocked as private and not
// resolve (the review harness used this same single-TU pattern).
#include "../../src/d3d11/kinematic_eval_probe.cpp"

namespace edvr {
// Linker stubs: this rig never hooks anything (arm/finish/reset unused).
const char* attachKinematicEvalHooks(KinematicEvalProbe*) noexcept { return "installed"; }
void detachKinematicEvalHooks(KinematicEvalProbe*) noexcept {}
bool kinematicEvalHooksMatch(uintptr_t) noexcept { return false; }
}
using namespace edvr;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

// The record as the probe reads it: node +0x18, xf block +0x130 (88 B;
// translation floats xf[8] lo/hi + xf[9] lo, quat lanes xf[9] hi + xf[10]
// lo), hash +0x268, epoch +0x1B8. Descriptor: record pointer at +0x10,
// epoch at +0x38. Render record: flags at +0x688.
struct FakeRec { alignas(16) uint8_t b[0x300]; };
struct FakeDesc { uint8_t b[0x60]; };
struct FakeRender { uint8_t b[0x700]; };

void put64(uint8_t* base, size_t off, uint64_t v) { std::memcpy(base + off, &v, 8); }
void put32(uint8_t* base, size_t off, uint32_t v) { std::memcpy(base + off, &v, 4); }

void setNode(uint8_t* rec, uint64_t node) { put64(rec, 0x18, node); }
// Writes the xf block's translation floats and neutral quat lanes
// (32768,32768,32768,65535), matching the flight-validated packing.
void setPose(uint8_t* rec, float x, float y, float z) {
    uint32_t xb, yb, zb;
    std::memcpy(&xb, &x, 4); std::memcpy(&yb, &y, 4); std::memcpy(&zb, &z, 4);
    const uint64_t w8 = uint64_t(xb) | (uint64_t(yb) << 32);
    const uint64_t w9 = uint64_t(zb) | (uint64_t(32768) << 32) | (uint64_t(32768) << 48);
    const uint64_t w10 = uint64_t(32768) | (uint64_t(65535) << 16);
    put64(rec, 0x130 + 64, w8);
    put64(rec, 0x130 + 72, w9);
    put64(rec, 0x130 + 80, w10);
}
void setTranslationBits(uint8_t* rec, uint32_t xb, float y, float z) {
    uint32_t yb, zb;
    std::memcpy(&yb, &y, 4); std::memcpy(&zb, &z, 4);
    const uint64_t w8 = uint64_t(xb) | (uint64_t(yb) << 32);
    const uint64_t w9 = uint64_t(zb) | (uint64_t(32768) << 32) | (uint64_t(32768) << 48);
    put64(rec, 0x130 + 64, w8);
    put64(rec, 0x130 + 72, w9);
}

FakeDesc descFor(uint8_t* rec) {
    FakeDesc d{};
    put64(d.b, 0x10, reinterpret_cast<uint64_t>(rec));
    return d;
}

// Seed as arm() leaves the probe, minus the hook install: cleared, active,
// frame_ carrying the mesh-domain stamp the first present must overwrite.
void seed(KinematicEvalProbe& p, uint32_t meshStamp) {
    p.clearLocked();
    p.active_.store(true);
    p.frame_.store(meshStamp);
}

// Finding 7: the first present-domain tick must seed WITHOUT flushing --
// before it, sampling is gated, so a flush fabricates an empty frame and
// forces min_frame_records to zero on a healthy feed.
void caseSeedWithoutFlush() {
    KinematicEvalProbe p;
    seed(p, 998);
    p.notePresentFrame(1000, 998);
    KinematicEvalProbe::Summary s = p.summary();
    check(s.framesCounted == 0, "1: the seed tick counts no frame");
    check(s.zeroRecordFrames == 0, "1: the seed tick is not a zero-record frame");
    check(s.minFrameRecords == ~0ull, "1: min untouched until a real frame ends");
    FakeRec r{}; setNode(r.b, 0xA1); setPose(r.b, 10.f, 20.f, 30.f);
    FakeDesc d = descFor(r.b); FakeRender rd{};
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    p.notePresentFrame(1001, 998);
    s = p.summary();
    check(s.framesCounted == 1 && s.zeroRecordFrames == 0 && s.minFrameRecords == 1,
          "1: the first real frame counts exactly one record");
    p.notePresentFrame(1002, 998); // nothing observed
    s = p.summary();
    check(s.zeroRecordFrames == 1, "1: a genuinely empty frame still counts");
}

// Finding 2: a record straddling into an unreadable page must not be created
// from a partial read -- the zero-filled transform baseline used to fabricate
// a 100 m mover on recovery.
void caseFaultedReadCreatesNothing() {
    KinematicEvalProbe p;
    seed(p, 998);
    p.notePresentFrame(2000, 998);
    auto* pages = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    check(pages != nullptr, "2: VirtualAlloc");
    if (!pages) return;
    // Low half of the record (node, xf block) in page 0; epoch word (+0x1B8)
    // in page 1, so the early epoch gate can pass while the rest faults.
    uint8_t* rec = pages + 4096 - 0x190;
    setNode(rec, 0xB1); setPose(rec, 100.f, 20.f, 30.f);
    FakeDesc d = descFor(rec); FakeRender rd{};
    DWORD old = 0;
    if (!VirtualProtect(pages, 4096, PAGE_NOACCESS, &old)) { check(false, "2: VirtualProtect"); VirtualFree(pages, 0, MEM_RELEASE); return; }
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    KinematicEvalProbe::Summary s = p.summary();
    check(s.readFaults > 0, "2: the fault is counted");
    check(p.records_.empty(), "2: no record from a partial read");
    if (!VirtualProtect(pages, 4096, PAGE_READWRITE, &old)) { check(false, "2: unprotect"); VirtualFree(pages, 0, MEM_RELEASE); return; }
    p.notePresentFrame(2001, 998);
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    p.notePresentFrame(2002, 998);
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    s = p.summary();
    check(p.records_.size() == 1, "2: recovery creates the record once");
    check(s.xfMovers == 0 && s.xfChanges == 0, "2: recovery is not motion");
    check(p.records_[0].totalJump == 0.0 && p.records_[0].maxJump == 0.f,
          "2: no jump fabricated from a zero baseline");
    check(p.records_[0].framesSampled == 2, "2: both good frames sampled");
    VirtualFree(pages, 0, MEM_RELEASE);
}

// Finding 3: a node swap on a continuously-seen pointer ends the previous
// occupant's history -- neither the xf diff nor the pose jump crosses the
// identity boundary.
void caseNodeSwapCrossesNothing() {
    KinematicEvalProbe p;
    seed(p, 998);
    p.notePresentFrame(3000, 998);
    FakeRec r{}; setNode(r.b, 0xC1); setPose(r.b, 10.f, 20.f, 30.f);
    FakeDesc d = descFor(r.b); FakeRender rd{};
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    p.notePresentFrame(3001, 998);
    setNode(r.b, 0xC2); setPose(r.b, 100.f, 20.f, 30.f); // new occupant, 90 m away
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    const KinematicEvalProbe::Summary s = p.summary();
    check(s.nodeChangeEvents == 1, "3: the identity event is kept");
    check(s.xfMovers == 0 && s.xfChanges == 0, "3: the boundary is not an xf change");
    check(p.records_[0].totalJump == 0.0 && p.records_[0].maxJump == 0.f,
          "3: the boundary is not a 90 m jump");
    check(p.records_[0].framesSampled == 2, "3: both occupants sampled");
    check(p.events_.size() == 1 && p.events_[0].kind == 2 &&
          p.events_[0].oldNode == 0xC1 && p.events_[0].newNode == 0xC2,
          "3: the event carries both nodes");
    p.notePresentFrame(3002, 998);
    setPose(r.b, 103.f, 20.f, 30.f); // the new occupant really moves 3 m
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    check(p.records_[0].totalJump > 2.9 && p.records_[0].totalJump < 3.1,
          "3: post-boundary motion measures from the new baseline");
    const KinematicEvalProbe::Summary s2 = p.summary();
    check(s2.xfMovers == 1, "3: the new occupant's own move is a mover");
}

// Finding 9: a non-finite translation must not reach the JSON floats. The
// raw bits stay in prevPose/mover_samples; the measurement is rejected.
void caseNonFiniteRejected() {
    KinematicEvalProbe p;
    seed(p, 998);
    p.notePresentFrame(4000, 998);
    FakeRec r{}; setNode(r.b, 0xD1); setPose(r.b, 1.f, 2.f, 3.f);
    FakeDesc d = descFor(r.b); FakeRender rd{};
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    p.notePresentFrame(4001, 998);
    setTranslationBits(r.b, 0x7FC00000u, 2.f, 3.f); // NaN x
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    KinematicEvalProbe::Summary s = p.summary();
    check(s.nonFinitePose == 1, "4: NaN jump rejected and counted");
    check(p.records_[0].totalJump == 0.0 && p.records_[0].maxJump == 0.f,
          "4: accumulators stay finite");
    check(s.xfMovers == 1, "4: the raw bits still classify as motion");
    p.notePresentFrame(4002, 998);
    setPose(r.b, 4.f, 2.f, 3.f); // recovery: jump from NaN is NaN, rejected too
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    s = p.summary();
    check(s.nonFinitePose == 2 && p.records_[0].totalJump == 0.0,
          "4: recovery off NaN bits stays finite");
    std::ostringstream j;
    p.writeJson(j);
    const std::string out = j.str();
    check(out.find("nan") == std::string::npos && out.find("inf") == std::string::npos,
          "4: the JSON stays parseable");
    check(out.find("\"non_finite_pose\":2") != std::string::npos,
          "4: the counter serializes");
}

// Job attribution (kinematic doc 2026-09-20 17:10): the observation carries
// the bracket TLS bits; the record accumulates the OR of every job it was
// ever seen under, and the mask serializes to the JSON.
void caseJobMaskAccumulates() {
    KinematicEvalProbe p;
    seed(p, 998);
    p.notePresentFrame(5000, 998);
    FakeRec r{}; setNode(r.b, 0xE2); setPose(r.b, 1.f, 2.f, 3.f);
    FakeDesc d = descFor(r.b); FakeRender rd{};
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    const uintptr_t rp = reinterpret_cast<uintptr_t>(&rd);
    p.observe(dp, rp, 0x2u);
    p.notePresentFrame(5001, 998);
    p.observe(dp, rp, 0x10u);
    check(p.records_[0].jobMask == 0x12u, "5: job bits accumulate across observations");
    p.notePresentFrame(5002, 998);
    p.observe(dp, rp); // default zero mask: no bit, no harm
    check(p.records_[0].jobMask == 0x12u, "5: a zero-mask observation keeps the bits");
    std::ostringstream j;
    p.writeJson(j);
    check(j.str().find("\"job_mask\":18") != std::string::npos, "5: the mask serializes");
}

// Physics dirty-queue counts (sanctioned 2026-09-20 19:13): entry/exit
// pairs accumulate runs/appended/max-delta per session; exit < entry is a
// mid-run reset and adds only the post-reset residue; only non-trivial
// pairs reach the capped sample vector. No seed(): like jobs[], the counts
// never gate on active().
void casePhysQueue() {
    KinematicEvalProbe p;
    p.notePhysQueue(10, 14); // +4
    p.notePhysQueue(20, 21); // +1
    p.notePhysQueue(9, 2);   // reset mid-run: +2 residue only
    check(p.physQueueRuns_ == 3 && p.physQueueAppended_ == 7 &&
          p.physQueueMaxDelta_.load() == 4 && p.physQueueResets_ == 1,
          "6: counters accumulate; a reset adds only the residue");
    check(p.physQueueSamples_.size() == 3 &&
          p.physQueueSamples_[0].first == 10 && p.physQueueSamples_[0].second == 14 &&
          p.physQueueSamples_[2].first == 9 && p.physQueueSamples_[2].second == 2,
          "6: non-trivial pairs are kept verbatim, resets included");
    p.notePhysQueue(5, 5); // zero delta: counted, not sampled
    check(p.physQueueRuns_ == 4 && p.physQueueAppended_ == 7 &&
          p.physQueueSamples_.size() == 3,
          "6: a zero-delta run counts but does not sample");
    std::ostringstream j;
    p.writeJson(j);
    check(j.str().find("\"phys_queue\":{\"runs\":4,\"appended\":7,\"max_delta\":4,\"resets\":1,"
                       "\"samples\":[{\"entry\":10,\"exit\":14}") != std::string::npos,
          "6: the counters and samples serialize");
}

// Physics dirty-queue NODE capture (sanctioned 2026-09-20 20:27): the
// probe keeps the last kPhysNodeCap walked pointers per session; overflow
// counts every walked node no longer in the ring, so the gate can assert
// the invariant total == ring + overflow. The hook clamps its walk to the
// cap; the probe still defends against an over-cap batch.
void casePhysNodes() {
    KinematicEvalProbe p;
    uint64_t batch[300];
    for (uint32_t i = 0; i < 300; ++i) batch[i] = 0x1000ull + i;
    p.notePhysNodes(batch, 300, 0); // over-cap batch: newest 256 kept, 44 drop
    check(p.physNodeTotal_ == 300 && p.physNodeOverflow_ == 44 &&
          p.physNodeRing_.size() == KinematicEvalProbe::kPhysNodeCap &&
          p.physNodeRing_.front() == 0x102Cull && p.physNodeRing_.back() == 0x112Bull,
          "7: an over-cap batch keeps the newest and counts the drop");
    uint64_t more[10];
    for (uint32_t i = 0; i < 10; ++i) more[i] = 0xF000ull + i;
    p.notePhysNodes(more, 10, 3); // +10 ring (10 oldest drop), +3 hook-side excess
    check(p.physNodeTotal_ == 313 && p.physNodeOverflow_ == 57 &&
          p.physNodeRing_.size() == KinematicEvalProbe::kPhysNodeCap &&
          p.physNodeRing_.front() == 0x1036ull && p.physNodeRing_.back() == 0xF009ull,
          "7: the ring drops oldest and total stays ring + overflow");
    std::ostringstream j;
    p.writeJson(j);
    check(j.str().find("\"phys_nodes\":{\"total\":313,\"overflow\":57,\"nodes\":[\"0x1036\"") !=
          std::string::npos, "7: the counters and ring serialize oldest-first");
}

// Draw-item-builder bucket counts (the census join, perf arc): per-session
// counters for the FUN_1442B4420 bracket. A wild entry walk counts and
// returns early; an empty walk counts separately; an exit fault drops the
// call's items but keeps its bucket count; overflow and negative deltas
// accumulate; the maxima track only what their path measured.
void caseBucketItems() {
    KinematicEvalProbe p;
    p.noteBucketBuild(3, 25, 1, 0);                                    // normal: 3 buckets, +25, one drain
    p.noteBucketBuild(0, 0, 0, 0);                                     // legitimately empty rig
    p.noteBucketBuild(0, 0, 0, KinematicEvalProbe::kBucketFlagEntryWild); // faulted/insane walk
    p.noteBucketBuild(5, 99, 0, KinematicEvalProbe::kBucketFlagExitFault);// exit fault: items dropped
    p.noteBucketBuild(2, 7, 0, KinematicEvalProbe::kBucketFlagOverflow);  // cap hit, +7
    check(p.bucketCalls_ == 5 && p.bucketItems_ == 32 &&
          p.bucketEmptyCalls_ == 1 && p.bucketEntryWild_ == 1 &&
          p.bucketExitFault_ == 1 && p.bucketNegDeltas_ == 1 &&
          p.bucketOverflowCalls_ == 1,
          "8: every path lands in its own counter, items skip the faulted call");
    check(p.bucketMaxBuckets_.load() == 5 && p.bucketMaxItems_.load() == 25,
          "8: maxima track the largest measured values");
    std::ostringstream j;
    p.writeJson(j);
    check(j.str().find("\"bucket_items\":{\"calls\":5,\"items\":32,\"empty_calls\":1,"
                       "\"entry_wild\":1,\"exit_fault\":1,\"neg_deltas\":1,"
                       "\"overflow_calls\":1,\"max_buckets\":5,\"max_items_per_call\":25}") !=
          std::string::npos, "8: the counters serialize");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("kinematic_probe_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    caseSeedWithoutFlush();
    caseFaultedReadCreatesNothing();
    caseNodeSwapCrossesNothing();
    caseNonFiniteRejected();
    caseJobMaskAccumulates();
    casePhysQueue();
    casePhysNodes();
    caseBucketItems();
    std::printf("kinematic_probe_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
