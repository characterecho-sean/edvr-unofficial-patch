#include "kinematic_motion.h"
#include "kinematic_eval_hook.h"
#include "../common/log.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace edvr {
namespace kinematic_motion_detail {

// Flight-grounded bounds: the probe's kRecordCap has never overflowed
// (3,086 distinct records was the largest capture window, flights
// 094158/103339). No eviction: a dropped pointer is a dead identity, and
// re-using its slot would re-prove stasis on somebody else's history.
constexpr uint32_t kTrackCap = 4096;
// Present frames of bit-exact pose stasis before a record is eligible.
// 3 frames at 58-90 fps is 33-52 ms of proof; instant reset on any byte.
constexpr uint32_t kStaticRunRequired = 3;
constexpr double kNearMissM = 0.001; // 0 < translation delta < 1 mm
constexpr size_t kPoseBytes = 20;    // +0x170 translation (12) + +0x17C quat (8), contiguous
constexpr size_t kBoundsBytes = 128; // stage-A diagnostic: +0xB0..+0x130 raw
constexpr size_t kCenterBytes = 16;  // stage-A diagnostic: +0x240 center float4
// Stage-A.5 diagnostic: the local bounding SPHERE, +0x270 centre float4 +
// +0x280 radius float4 (lanes 1-3 padding). Found offline 2026-09-20: writer
// FUN_14433c870 seeds centre from model data +0x20 and radius from +0x2c and
// unions over children via sphere-merge FUN_140a8c9a0; the updater then maps
// centre to world at +0x240. The game culls on spheres, not AABBs.
constexpr size_t kSphereBytes = 32;
constexpr uint64_t kSummaryMs = 20000;
constexpr uint64_t kStandDownMs = 5000;
constexpr uint64_t kNoMoverMs = 10000;
constexpr uint64_t kBoundsWaitMs = 20000; // one-time note if part A never fires
constexpr uint32_t kBoundsDumpMovers = 8, kBoundsDumpStatics = 8;

struct TrackedRecord {
    uint64_t record = 0;    // key: the live record pointer
    uint64_t node = 0;      // record+0x18: the reuse discriminator
    uint32_t lastFrame = 0; // last present-domain frame this pointer was seen
    uint32_t staticRun = 0; // consecutive frames of bit-exact zero pose delta
    uint32_t lastChangeFrame = 0; // last frame the pose bytes changed
    uint8_t prevPose[kPoseBytes]{};
    uint8_t bounds[kBoundsBytes]{};
    uint8_t prevSphere[kSphereBytes]{}; // +0x270..+0x28F: part of the stasis compare (LOD refresh writes it with zero pose change)
    double path = 0;        // summed translation deltas, for the dump ranking
    bool hasPrev = false, boundsValid = false, sphereValid = false, everMoved = false;
    uint64_t calls = 0;
};

std::mutex mutex_;
std::atomic<bool> active_{false};
std::atomic<uint32_t> frame_{0};
bool clockSeeded_ = false; // observe() writes no table state until the clock lives
std::unordered_map<uint64_t, uint32_t> index_;
std::vector<TrackedRecord> records_;
KinematicMotionStats stats_;
uint64_t seenThisFrame_ = 0;
uint64_t configureTickMs_ = 0, lastSummaryMs_ = 0;
// Stale-upload suspicion window (2026-09-20 13:55 spec failure mode): the
// previous summary's eligible census, published generation and rejection
// count. The census moving while the generation AND the rejections hold
// means the temporal pass would be vetoing off a stale sphere buffer.
uint32_t lastSummaryEligible_ = 0;
uint64_t lastSummaryGeneration_ = 0, lastSummaryRejected_ = 0;
bool standDownNoted_ = false, noMoverNoted_ = false, boundsWaitNoted_ = false;
// Bounds-layout decode dump: part A on the first ended frame with BOTH
// populations present (>=8 movers AND >=8 eligible statics -- movers qualify
// after two samples, statics need baseline + 3 equal frames, so a movers-only
// trigger would dump zero comparison statics; 2026-09-20 review finding 6),
// part B on the next ended frame for the same mover table indices (the delta
// between parts separates position-like fields from extent-like fields; head
// motion separates both from the view products). Once per session.
int boundsDumpStage_ = 0;
std::vector<uint32_t> boundsDumpIds_;
// The stage-B upload snapshot: world spheres of the records eligible at the
// last ended frame, rebuilt at the frame census (kinematicMotionNotePresentFrame).
// The generation bumps only on content change; the temporal pass compares it
// against its last upload and skips the copy when unchanged.
std::vector<KinematicSphereGpu> snapshot_;
uint64_t snapshotGeneration_ = 0;
uint32_t snapshotFrame_ = 0;

bool guardedRead(uintptr_t address, void* output, size_t bytes) noexcept {
    __try { std::memcpy(output, reinterpret_cast<const void*>(address), bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint64_t read64(uintptr_t address, bool& ok) noexcept {
    uint64_t v = 0; if (!guardedRead(address, &v, sizeof(v))) ok = false; return v;
}

void clearLocked() {
    index_.clear(); records_.clear();
    stats_ = KinematicMotionStats{};
    seenThisFrame_ = 0;
    clockSeeded_ = false;
    standDownNoted_ = false; noMoverNoted_ = false; boundsWaitNoted_ = false;
    boundsDumpStage_ = 0; boundsDumpIds_.clear();
    snapshot_.clear(); snapshotGeneration_ = 0; snapshotFrame_ = 0;
    lastSummaryEligible_ = 0; lastSummaryGeneration_ = 0; lastSummaryRejected_ = 0;
}

bool eligibleLocked(const TrackedRecord& r, uint32_t frame) {
    return r.hasPrev && r.lastFrame == frame && r.staticRun >= kStaticRunRequired;
}

// The uploadable sphere needs finite centre lanes and a positive finite
// radius (a centre lane w drifted off zero by the sphere-merge is normal,
// flight 132856 id=148, and the padding lanes carry nothing). Anything less
// is a bad read or an uninitialised record: counted, never uploaded -- the
// failed-reads-fabricate-motion class (2026-09-20 review finding 2).
bool spherePlausibleBytes(const uint8_t* sphere) {
    float f[8];
    std::memcpy(f, sphere, sizeof(f));
    for (int i = 0; i < 3; ++i) if (!std::isfinite(f[i])) return false;
    return std::isfinite(f[4]) && f[4] > 0.0f;
}

// The flight-proven world-sphere map (132856): the +0xF0 3x3's rows are the
// basis vectors' images, so the world centre is R^T x local + T -- exact to
// 0.0000 on every dumped static -- and the world radius scales by the max
// 3x3 column scale. The matrix lives inside the record's bounds block
// (+0xF0 rows at block +0x40, +0x120 translation at block +0x70). +0x240 is
// the game's own world centre but zero for guard-off records; we never read
// it. Returns false when the inputs were never validly read.
bool worldSphereLocked(const TrackedRecord& r, KinematicSphereGpu& out) {
    if (!r.boundsValid || !r.sphereValid || !spherePlausibleBytes(r.prevSphere)) return false;
    float rot[12], t[3], lc[4];
    std::memcpy(rot, r.bounds + 0x40, sizeof(rot));   // three padded float4 rows
    std::memcpy(t, r.bounds + 0x70, sizeof(t));
    std::memcpy(lc, r.prevSphere, sizeof(lc));        // local centre, lane 3 ignored
    const float* r0 = rot;      const float* r1 = rot + 4;      const float* r2 = rot + 8;
    const double cx = (double)r0[0] * lc[0] + (double)r1[0] * lc[1] + (double)r2[0] * lc[2] + t[0];
    const double cy = (double)r0[1] * lc[0] + (double)r1[1] * lc[1] + (double)r2[1] * lc[2] + t[1];
    const double cz = (double)r0[2] * lc[0] + (double)r1[2] * lc[1] + (double)r2[2] * lc[2] + t[2];
    const double s0 = std::sqrt((double)r0[0]*r0[0] + (double)r1[0]*r1[0] + (double)r2[0]*r2[0]);
    const double s1 = std::sqrt((double)r0[1]*r0[1] + (double)r1[1]*r1[1] + (double)r2[1]*r2[1]);
    const double s2 = std::sqrt((double)r0[2]*r0[2] + (double)r1[2]*r1[2] + (double)r2[2]*r2[2]);
    float radius;
    std::memcpy(&radius, r.prevSphere + 16, 4);
    const double wr = (double)radius * std::max(s0, std::max(s1, s2));
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cz) ||
        !std::isfinite(wr) || wr <= 0.0) return false;
    out.centre[0] = (float)cx; out.centre[1] = (float)cy; out.centre[2] = (float)cz;
    out.radius = (float)wr;
    out.kind = 0;                // static-zero (phase 1)
    out.reserved[0] = out.reserved[1] = out.reserved[2] = 0;
    std::memset(out.prevMap, 0, sizeof(out.prevMap));
    return true;
}

// Rebuild the upload snapshot from the just-ended frame's eligibility census.
// Content-addressed: the generation bumps only when the set or a member's
// sphere bytes change, so an idle settlement uploads nothing per frame.
void rebuildSnapshotLocked(uint32_t endedFrame) {
    std::vector<KinematicSphereGpu> next;
    for (const TrackedRecord& r : records_) {
        if (!eligibleLocked(r, endedFrame)) continue;
        KinematicSphereGpu s;
        if (!worldSphereLocked(r, s)) { ++stats_.sphereRejected; continue; }
        next.push_back(s);
    }
    if (next.size() != snapshot_.size() ||
        (!next.empty() && std::memcmp(next.data(), snapshot_.data(),
                                      next.size() * sizeof(KinematicSphereGpu)) != 0)) {
        snapshot_.swap(next);
        ++snapshotGeneration_;
    }
    snapshotFrame_ = endedFrame;
    stats_.uploadedLast = static_cast<uint32_t>(snapshot_.size());
    stats_.uploadGeneration = snapshotGeneration_;
}

// One pose change, classified: translation vs quat-only (the 103339
// rotating-in-place class), the sub-1 mm near-miss counter, the dump-ranking
// path length. Shared by the cross-frame and the same-frame-dup paths so both
// invalidate identically (2026-09-20 review finding 1).
void notePoseChangeLocked(TrackedRecord& r, const uint8_t* pose, uint32_t frame) {
    float t0[3], t1[3];
    std::memcpy(t0, r.prevPose, sizeof(t0));
    std::memcpy(t1, pose, sizeof(t1));
    const double dx = (double)t1[0] - t0[0], dy = (double)t1[1] - t0[1], dz = (double)t1[2] - t0[2];
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    ++stats_.poseChanges;
    r.lastChangeFrame = frame;
    if (std::memcmp(pose, r.prevPose, 12) != 0) {
        ++stats_.translationChanges;
        if (d < kNearMissM) ++stats_.nearMiss;
        r.path += d;
    } else {
        ++stats_.quatOnlyChanges;
    }
    if (!r.everMoved) { r.everMoved = true; ++stats_.moversTotal; }
    r.staticRun = 0;
}

void hexWords(const uint8_t* p, size_t bytes, std::string& out) {
    char buf[16];
    for (size_t i = 0; i < bytes; i += 4) {
        uint32_t w; std::memcpy(&w, p + i, 4);
        std::snprintf(buf, sizeof(buf), "%s%08x", i ? " " : "", w);
        out += buf;
    }
}

// One bounded bounds-block dump line per record. Read faults here do not
// touch the tracker's readFaults counter -- the dump is diagnostic-only and
// says so in its own line.
void dumpBoundsLocked(int part, uint32_t frame, uint32_t id, const TrackedRecord& r) {
    float t[3] = {};
    std::memcpy(t, r.prevPose, sizeof(t));
    uint8_t center[kCenterBytes]{};
    const bool cok = guardedRead(static_cast<uintptr_t>(r.record) + 0x240, center, sizeof(center));
    uint8_t sphere[kSphereBytes]{};
    const bool sok = guardedRead(static_cast<uintptr_t>(r.record) + 0x270, sphere, sizeof(sphere));
    std::string b, c, s;
    hexWords(r.bounds, sizeof(r.bounds), b);
    hexWords(center, sizeof(center), c);
    hexWords(sphere, sizeof(sphere), s);
    Log::get().note(
        "engine motion bounds: part=%c frame=%u id=%u rec=0x%llx node=0x%llx run=%u path=%.3f "
        "pos=(%.3f,%.3f,%.3f) bvalid=%u cvalid=%u svalid=%u b=%s c=%s s=%s",
        part ? 'B' : 'A', frame, id, (unsigned long long)r.record, (unsigned long long)r.node,
        r.staticRun, r.path, (double)t[0], (double)t[1], (double)t[2],
        r.boundsValid ? 1u : 0u, cok ? 1u : 0u, sok ? 1u : 0u, b.c_str(), c.c_str(), s.c_str());
}

void maybeDumpBoundsLocked(uint32_t endedFrame, uint32_t movers, uint32_t eligible) {
    if (boundsDumpStage_ == 0) {
        // Both populations or neither: movers qualify after two samples while
        // statics need baseline + 3 equal frames, so a movers-only trigger
        // completes the one-shot dump with zero static comparisons under a
        // normal startup (2026-09-20 review finding 6). The drone alone
        // fields 11 movers; a settlement fields thousands of statics.
        if (movers < kBoundsDumpMovers || eligible < kBoundsDumpStatics) return;
        // Top movers by accumulated path, seen in the ended frame.
        std::vector<uint32_t> moversByPath;
        for (uint32_t i = 0; i < records_.size(); ++i)
            if (records_[i].everMoved && records_[i].lastFrame == endedFrame)
                moversByPath.push_back(i);
        // partial selection sort, top kBoundsDumpMovers
        for (size_t a = 0; a < moversByPath.size() && boundsDumpIds_.size() < kBoundsDumpMovers; ++a) {
            size_t best = a;
            for (size_t k = a + 1; k < moversByPath.size(); ++k)
                if (records_[moversByPath[k]].path > records_[moversByPath[best]].path) best = k;
            std::swap(moversByPath[a], moversByPath[best]);
            boundsDumpIds_.push_back(moversByPath[a]);
        }
        for (uint32_t i = 0; i < boundsDumpIds_.size(); ++i)
            dumpBoundsLocked(0, endedFrame, boundsDumpIds_[i], records_[boundsDumpIds_[i]]);
        // And a static sample: the first eligibles of the ended frame.
        uint32_t statics = 0;
        for (uint32_t i = 0; i < records_.size() && statics < kBoundsDumpStatics; ++i) {
            if (!eligibleLocked(records_[i], endedFrame)) continue;
            dumpBoundsLocked(0, endedFrame, i, records_[i]);
            ++statics;
        }
        Log::get().note("engine motion bounds: part A dumped for %u movers, %u statics at frame %u; "
                        "part B follows next frame for the same mover ids.",
                        (unsigned)boundsDumpIds_.size(), statics, endedFrame);
        boundsDumpStage_ = 1;
    } else if (boundsDumpStage_ == 1) {
        for (uint32_t i = 0; i < boundsDumpIds_.size(); ++i)
            dumpBoundsLocked(1, endedFrame, boundsDumpIds_[i], records_[boundsDumpIds_[i]]);
        Log::get().note("engine motion bounds: part B dumped at frame %u; decode is offline "
                        "(position-like fields shift by the record's +0x170 delta between parts).",
                        endedFrame);
        boundsDumpStage_ = 2;
    }
}

void summaryLocked(uint64_t now) {
    if (now - lastSummaryMs_ < kSummaryMs) return;
    lastSummaryMs_ = now;
    // Stale-upload suspicion (2026-09-20 13:55 spec): the eligible census
    // moved over the window yet the published generation AND the rejection
    // count both held -- the set did not change when it should have, so the
    // coverage pass would be vetoing off a stale sphere buffer. A census
    // move explained by rejections (new eligibles with implausible spheres)
    // is legitimate and does not trip this.
    if (stats_.eligibleLast != lastSummaryEligible_ &&
        snapshotGeneration_ == lastSummaryGeneration_ &&
        stats_.sphereRejected == lastSummaryRejected_) {
        ++stats_.uploadStuck;
        Log::get().note("engine motion: STALE-UPLOAD suspicion -- eligible census moved "
                        "(%u -> %u) over the summary window but the upload generation held "
                        "at %llu with no new rejections; the coverage pass would be reading "
                        "a stale sphere buffer.",
                        lastSummaryEligible_, stats_.eligibleLast,
                        (unsigned long long)snapshotGeneration_);
    }
    lastSummaryEligible_ = stats_.eligibleLast;
    lastSummaryGeneration_ = snapshotGeneration_;
    lastSummaryRejected_ = stats_.sphereRejected;
    Log::get().note(
        "engine motion: tracked %llu; last frame seen %llu, eligible %llu, movers %llu; "
        "observed %llu dup %llu faults %llu overflow %llu; pose changes %llu "
        "(translation %llu, quat-only %llu, near-miss %llu), movers total %llu; "
        "gap drops %llu, node changes %llu, bounds changes %llu, same-frame "
        "invalidations %llu; sphere changes %llu, rejected %llu; frames %llu, "
        "zero-record %llu, eligible-frames %llu; upload gen %llu, spheres %u. "
        "Absence of this line with fix.engine_motion on reads as a dead "
        "instrument, never as success.",
        (unsigned long long)records_.size(),
        (unsigned long long)stats_.seenLast, (unsigned long long)stats_.eligibleLast,
        (unsigned long long)stats_.moversLast,
        (unsigned long long)stats_.observed, (unsigned long long)stats_.dupInFrame,
        (unsigned long long)stats_.readFaults, (unsigned long long)stats_.recordOverflow,
        (unsigned long long)stats_.poseChanges, (unsigned long long)stats_.translationChanges,
        (unsigned long long)stats_.quatOnlyChanges, (unsigned long long)stats_.nearMiss,
        (unsigned long long)stats_.moversTotal,
        (unsigned long long)stats_.gapDrops, (unsigned long long)stats_.nodeChanges,
        (unsigned long long)stats_.boundsChanges, (unsigned long long)stats_.sameFrameChanges,
        (unsigned long long)stats_.sphereChanges, (unsigned long long)stats_.sphereRejected,
        (unsigned long long)stats_.framesCounted, (unsigned long long)stats_.zeroRecordFrames,
        (unsigned long long)stats_.eligibleFrames,
        (unsigned long long)stats_.uploadGeneration, stats_.uploadedLast);
}

} // namespace kinematic_motion_detail

using namespace kinematic_motion_detail;

bool kinematicMotionActive() noexcept { return active_.load(std::memory_order_acquire); }

void kinematicMotionConfigure(bool on) {
    if (on == active_.load(std::memory_order_acquire)) return; // 1 Hz re-poll idempotency
    if (!on) { kinematicMotionShutdown(); return; }
    // The tracker never hooks anything itself: the hook owner validates the
    // executable (PE + prologue / our own patch) and installs the feed.
    const char* result = kinematicEvalTrackerAttach();
    if (std::strcmp(result, "installed") != 0) {
        static std::string lastFail;
        if (lastFail != result) {
            lastFail = result;
            Log::get().note("engine motion: fix.engine_motion is on but the kinematic eval hook "
                            "refused (%s) -- the tracker stands down and the stock motion path "
                            "is untouched. Retried quietly on later config polls.", result);
        }
        return;
    }
    kinematicEvalSetTrackerObserver(&kinematicMotionObserve);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clearLocked();
        configureTickMs_ = GetTickCount64();
        lastSummaryMs_ = configureTickMs_;
    }
    active_.store(true, std::memory_order_release);
    Log::get().note("engine motion: tracker live (fix.engine_motion=on) -- proven-static records' "
                    "world spheres feed the temporal pass's ownership coverage (the t19/t20 compose "
                    "veto): geometry proven bit-static takes camera-only motion; movers and unknowns "
                    "keep the stock path. Stand-downs and zero-record frames are logged, never read "
                    "as pass.");
}

void kinematicMotionShutdown() {
    const bool was = active_.exchange(false, std::memory_order_acq_rel);
    kinematicEvalSetTrackerObserver(nullptr);
    kinematicEvalTrackerDetach();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clearLocked();
    }
    if (was) Log::get().note("engine motion: tracker stood down, state cleared.");
}

void kinematicMotionNotePresentFrame(uint32_t presentFrame) noexcept {
    if (!active_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clockSeeded_) {
        // The first tick STARTS the first frame; there is no ended frame to
        // count (the arm-seed seam that fired 3,073 fake gaps on 094158).
        clockSeeded_ = true;
        frame_.store(presentFrame, std::memory_order_release);
        seenThisFrame_ = 0;
        return;
    }
    const uint32_t prev = frame_.exchange(presentFrame, std::memory_order_acq_rel);
    if (prev == presentFrame) return;
    // Frame `prev` just ended.
    ++stats_.framesCounted;
    if (seenThisFrame_ == 0) ++stats_.zeroRecordFrames;
    uint32_t eligible = 0, movers = 0;
    for (const TrackedRecord& r : records_) {
        if (r.lastFrame != prev) continue;
        if (eligibleLocked(r, prev)) ++eligible;
        if (r.everMoved && r.lastChangeFrame == prev) ++movers;
    }
    stats_.seenLast = static_cast<uint32_t>(seenThisFrame_);
    stats_.eligibleLast = eligible;
    stats_.moversLast = movers;
    stats_.tracked = static_cast<uint32_t>(records_.size());
    if (eligible > 0) ++stats_.eligibleFrames;
    rebuildSnapshotLocked(prev); // the coverage pass uploads off this generation
    maybeDumpBoundsLocked(prev, movers, eligible);
    seenThisFrame_ = 0;
    const uint64_t now = GetTickCount64();
    if (!standDownNoted_ && configureTickMs_ && now - configureTickMs_ >= kStandDownMs &&
        stats_.observed == 0) {
        standDownNoted_ = true;
        Log::get().note("engine motion: STAND-DOWN -- five seconds live with zero eval "
                        "observations; the hook is not feeding. The tracker is inert and the "
                        "stock motion path is untouched.");
    }
    if (!boundsWaitNoted_ && configureTickMs_ && now - configureTickMs_ >= kBoundsWaitMs &&
        boundsDumpStage_ == 0 && stats_.observed > 0) {
        boundsWaitNoted_ = true;
        Log::get().note("engine motion bounds: 20 s live and part A has not fired -- it needs "
                        ">=8 movers AND >=8 eligible statics in one ended frame (last ended "
                        "frame: movers %u, eligible %u). If the scene cannot field both, the "
                        "layout decode waits for a settlement flight.",
                        stats_.moversLast, stats_.eligibleLast);
    }
    if (!noMoverNoted_ && configureTickMs_ && now - configureTickMs_ >= kNoMoverMs &&
        stats_.observed > 0 && stats_.poseChanges == 0) {
        noMoverNoted_ = true;
        Log::get().note("engine motion: ten seconds, %u tracked records, zero pose changes -- "
                        "a still scene or a dead feed; the mover-control principle (flight "
                        "064047) says treat this as suspicious.",
                        (unsigned)records_.size());
    }
    summaryLocked(now);
}

void kinematicMotionObserve(uintptr_t descriptor) noexcept {
    if (!active_.load(std::memory_order_acquire)) return;
    if (!descriptor) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.observed;
    if (!clockSeeded_) return; // no table state until the present clock lives
    bool ok = true;
    const uint64_t record = read64(descriptor + 0x10, ok); // the stride-0x2F0 record
    if (!ok || !record) { ++stats_.readFaults; return; }
    const uint32_t frame = frame_.load(std::memory_order_acquire);
    auto it = index_.find(record);
    if (it == index_.end()) {
        if (records_.size() >= kTrackCap) { ++stats_.recordOverflow; return; }
        TrackedRecord r;
        r.record = record;
        r.node = read64(record + 0x18, ok);
        r.lastFrame = frame;
        r.calls = 1;
        if (guardedRead(record + 0x170, r.prevPose, kPoseBytes)) r.hasPrev = true;
        else { ++stats_.readFaults; }
        if (guardedRead(record + 0xB0, r.bounds, kBoundsBytes)) r.boundsValid = true;
        if (guardedRead(record + 0x270, r.prevSphere, kSphereBytes)) r.sphereValid = true;
        if (!ok) ++stats_.readFaults;
        index_.emplace(record, static_cast<uint32_t>(records_.size()));
        records_.push_back(r);
        ++seenThisFrame_;
        return;
    }
    TrackedRecord& r = records_[it->second];
    ++r.calls;
    if (r.lastFrame == frame) {
        ++stats_.dupInFrame;
        // Fan-out dup: history does not advance twice in one frame, but the
        // invariance of repeated observations within a Present is an
        // ASSUMPTION (assembly-established updater ordering, not a contract).
        // Verify cheaply: a same-frame node, pose or sphere disagreement
        // invalidates the label exactly like a cross-frame change
        // (2026-09-20 review finding 1: an eligible record otherwise kept
        // its static label through a same-frame node swap or translation
        // change; the sphere has a writer independent of pose -- LOD
        // refresh -- so it is part of the compare too).
        bool vok = true;
        const uint64_t node = read64(record + 0x18, vok);
        if (!vok) { ++stats_.readFaults; r.staticRun = 0; r.hasPrev = false; return; }
        if (node != r.node) {
            ++stats_.nodeChanges; ++stats_.sameFrameChanges;
            r.staticRun = 0; r.hasPrev = false; r.everMoved = false;
            r.boundsValid = false; r.sphereValid = false; r.path = 0; r.node = node;
            if (guardedRead(record + 0x170, r.prevPose, kPoseBytes)) r.hasPrev = true;
            else ++stats_.readFaults;
            if (guardedRead(record + 0x270, r.prevSphere, kSphereBytes)) r.sphereValid = true;
            return;
        }
        uint8_t vpose[kPoseBytes];
        if (!guardedRead(record + 0x170, vpose, kPoseBytes)) {
            ++stats_.readFaults; r.staticRun = 0; r.hasPrev = false; return;
        }
        uint8_t vsphere[kSphereBytes];
        if (!guardedRead(record + 0x270, vsphere, kSphereBytes)) {
            ++stats_.readFaults; r.staticRun = 0; r.hasPrev = false; r.sphereValid = false; return;
        }
        if (!r.hasPrev) {
            std::memcpy(r.prevPose, vpose, kPoseBytes); r.hasPrev = true;
            std::memcpy(r.prevSphere, vsphere, kSphereBytes); r.sphereValid = true;
            return;
        }
        if (std::memcmp(vpose, r.prevPose, kPoseBytes) != 0) {
            ++stats_.sameFrameChanges;
            notePoseChangeLocked(r, vpose, frame);
        }
        if (r.sphereValid && std::memcmp(vsphere, r.prevSphere, kSphereBytes) != 0) {
            // The sphere's writer is independent of pose (LOD refresh): a
            // sphere byte change invalidates stasis exactly like a pose
            // byte, without branding the record a mover.
            ++stats_.sameFrameChanges; ++stats_.sphereChanges;
            r.staticRun = 0;
        }
        std::memcpy(r.prevPose, vpose, kPoseBytes);
        std::memcpy(r.prevSphere, vsphere, kSphereBytes);
        r.sphereValid = true;
        return;
    }
    // A pointer absent for at least one frame is a NEW identity: never
    // re-proven on somebody else's history (post-gap reuse is flight-untested).
    if (frame > r.lastFrame + 1) {
        ++stats_.gapDrops;
        r.staticRun = 0;
        r.hasPrev = false;
        r.everMoved = false;
        r.boundsValid = false; // the new identity's bounds are not the old's
        r.sphereValid = false;
        r.path = 0;
        r.node = read64(record + 0x18, ok);
        r.lastFrame = frame;
        if (guardedRead(record + 0x170, r.prevPose, kPoseBytes)) r.hasPrev = true;
        else ++stats_.readFaults;
        if (guardedRead(record + 0xB0, r.bounds, kBoundsBytes)) r.boundsValid = true;
        if (guardedRead(record + 0x270, r.prevSphere, kSphereBytes)) r.sphereValid = true;
        if (!ok) ++stats_.readFaults;
        ++seenThisFrame_;
        return;
    }
    // Continuous observation, new frame for this record.
    r.lastFrame = frame;
    ++seenThisFrame_;
    const uint64_t node = read64(record + 0x18, ok);
    if (!ok) { ++stats_.readFaults; r.staticRun = 0; r.hasPrev = false; return; }
    if (node != r.node) {
        // Same pointer, different node: possible slot reuse -- new identity.
        ++stats_.nodeChanges;
        r.staticRun = 0;
        r.hasPrev = false;
        r.boundsValid = false;
        r.sphereValid = false;
        r.everMoved = false;
        r.path = 0;
        r.node = node;
        if (guardedRead(record + 0x170, r.prevPose, kPoseBytes)) r.hasPrev = true;
        else ++stats_.readFaults;
        if (guardedRead(record + 0x270, r.prevSphere, kSphereBytes)) r.sphereValid = true;
        return;
    }
    uint8_t pose[kPoseBytes];
    if (!guardedRead(record + 0x170, pose, kPoseBytes)) {
        ++stats_.readFaults;
        r.staticRun = 0; // a faulted read proves nothing
        r.hasPrev = false;
        return;
    }
    uint8_t bounds[kBoundsBytes];
    if (guardedRead(record + 0xB0, bounds, kBoundsBytes)) {
        if (r.boundsValid && std::memcmp(bounds, r.bounds, kBoundsBytes) != 0) ++stats_.boundsChanges;
        std::memcpy(r.bounds, bounds, kBoundsBytes);
        r.boundsValid = true;
    }
    uint8_t sphere[kSphereBytes];
    if (!guardedRead(record + 0x270, sphere, kSphereBytes)) {
        ++stats_.readFaults;
        r.staticRun = 0; // a faulted read proves nothing
        r.hasPrev = false;
        r.sphereValid = false;
        return;
    }
    if (!r.hasPrev || !r.sphereValid) {
        std::memcpy(r.prevPose, pose, kPoseBytes);
        r.hasPrev = true;
        std::memcpy(r.prevSphere, sphere, kSphereBytes);
        r.sphereValid = true;
        return;
    }
    // The stasis compare is 20 + 32 bytes: pose plus sphere (2026-09-20 13:55
    // spec). A sphere change with a bit-static pose is the LOD-refresh shape;
    // it forces the standard 3-frame re-proof without branding a mover.
    const bool poseSame = std::memcmp(pose, r.prevPose, kPoseBytes) == 0;
    const bool sphereSame = std::memcmp(sphere, r.prevSphere, kSphereBytes) == 0;
    if (!poseSame) notePoseChangeLocked(r, pose, frame);
    if (!sphereSame) { ++stats_.sphereChanges; r.staticRun = 0; }
    if (poseSame && sphereSame && r.staticRun < 0xFFFF) ++r.staticRun;
    std::memcpy(r.prevPose, pose, kPoseBytes);
    std::memcpy(r.prevSphere, sphere, kSphereBytes);
}

KinematicMotionStats kinematicMotionStats() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    KinematicMotionStats out = stats_;
    out.tracked = static_cast<uint32_t>(records_.size());
    return out;
}

uint64_t kinematicMotionSphereSnapshot(KinematicSphereGpu* out, uint32_t cap,
                                       uint32_t* count, uint32_t* frame) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t n = static_cast<uint32_t>(snapshot_.size());
    const uint32_t ncopy = (cap < n) ? cap : n;
    if (out && ncopy) std::memcpy(out, snapshot_.data(), ncopy * sizeof(KinematicSphereGpu));
    if (count) *count = n;   // the published set's size, even when cap truncates the copy
    if (frame) *frame = snapshotFrame_;
    return snapshotGeneration_;
}

bool kinematicMotionRecordEligible(uint64_t record) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = index_.find(record);
    if (it == index_.end()) return false;
    return eligibleLocked(records_[it->second], frame_.load(std::memory_order_acquire));
}

int kinematicMotionBoundsDumpStage() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return boundsDumpStage_;
}

} // namespace edvr
