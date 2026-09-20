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
constexpr uint64_t kSummaryMs = 20000;
constexpr uint64_t kStandDownMs = 5000;
constexpr uint64_t kNoMoverMs = 10000;
constexpr uint32_t kBoundsDumpMovers = 8, kBoundsDumpStatics = 8;

struct TrackedRecord {
    uint64_t record = 0;    // key: the live record pointer
    uint64_t node = 0;      // record+0x18: the reuse discriminator
    uint32_t lastFrame = 0; // last present-domain frame this pointer was seen
    uint32_t staticRun = 0; // consecutive frames of bit-exact zero pose delta
    uint32_t lastChangeFrame = 0; // last frame the pose bytes changed
    uint8_t prevPose[kPoseBytes]{};
    uint8_t bounds[kBoundsBytes]{};
    double path = 0;        // summed translation deltas, for the dump ranking
    bool hasPrev = false, boundsValid = false, everMoved = false;
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
bool standDownNoted_ = false, noMoverNoted_ = false;
// Bounds-layout decode dump: part A on the first ended frame with >=8 movers,
// part B on the next ended frame for the same mover table indices (the delta
// between parts separates position-like fields from extent-like fields; head
// motion separates both from the view products). Once per session.
int boundsDumpStage_ = 0;
std::vector<uint32_t> boundsDumpIds_;

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
    standDownNoted_ = false; noMoverNoted_ = false;
    boundsDumpStage_ = 0; boundsDumpIds_.clear();
}

bool eligibleLocked(const TrackedRecord& r, uint32_t frame) {
    return r.hasPrev && r.lastFrame == frame && r.staticRun >= kStaticRunRequired;
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
    std::string b, c;
    hexWords(r.bounds, sizeof(r.bounds), b);
    hexWords(center, sizeof(center), c);
    Log::get().note(
        "engine motion bounds: part=%c frame=%u id=%u rec=0x%llx node=0x%llx run=%u path=%.3f "
        "pos=(%.3f,%.3f,%.3f) bvalid=%u cvalid=%u b=%s c=%s",
        part ? 'B' : 'A', frame, id, (unsigned long long)r.record, (unsigned long long)r.node,
        r.staticRun, r.path, (double)t[0], (double)t[1], (double)t[2],
        r.boundsValid ? 1u : 0u, cok ? 1u : 0u, b.c_str(), c.c_str());
}

void maybeDumpBoundsLocked(uint32_t endedFrame, uint32_t movers) {
    if (boundsDumpStage_ == 0) {
        if (movers < 8) return; // the drone alone fields 11; wait for a mover crowd
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
    Log::get().note(
        "engine motion: tracked %llu; last frame seen %llu, eligible %llu, movers %llu; "
        "observed %llu dup %llu faults %llu overflow %llu; pose changes %llu "
        "(translation %llu, quat-only %llu, near-miss %llu), movers total %llu; "
        "gap drops %llu, node changes %llu, bounds changes %llu; frames %llu, "
        "zero-record %llu, eligible-frames %llu. Absence of this line with "
        "fix.engine_motion on reads as a dead instrument, never as success.",
        (unsigned long long)records_.size(),
        (unsigned long long)stats_.seenLast, (unsigned long long)stats_.eligibleLast,
        (unsigned long long)stats_.moversLast,
        (unsigned long long)stats_.observed, (unsigned long long)stats_.dupInFrame,
        (unsigned long long)stats_.readFaults, (unsigned long long)stats_.recordOverflow,
        (unsigned long long)stats_.poseChanges, (unsigned long long)stats_.translationChanges,
        (unsigned long long)stats_.quatOnlyChanges, (unsigned long long)stats_.nearMiss,
        (unsigned long long)stats_.moversTotal,
        (unsigned long long)stats_.gapDrops, (unsigned long long)stats_.nodeChanges,
        (unsigned long long)stats_.boundsChanges,
        (unsigned long long)stats_.framesCounted, (unsigned long long)stats_.zeroRecordFrames,
        (unsigned long long)stats_.eligibleFrames);
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
    Log::get().note("engine motion: tracker live (fix.engine_motion=on) -- proven-static records "
                    "are tracked, counted and bounds-sampled; this build makes NO rendering change. "
                    "Stand-downs and zero-record frames are logged, never read as pass.");
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
    maybeDumpBoundsLocked(prev, movers);
    seenThisFrame_ = 0;
    const uint64_t now = GetTickCount64();
    if (!standDownNoted_ && configureTickMs_ && now - configureTickMs_ >= kStandDownMs &&
        stats_.observed == 0) {
        standDownNoted_ = true;
        Log::get().note("engine motion: STAND-DOWN -- five seconds live with zero eval "
                        "observations; the hook is not feeding. The tracker is inert and the "
                        "stock motion path is untouched.");
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
        if (!ok) ++stats_.readFaults;
        index_.emplace(record, static_cast<uint32_t>(records_.size()));
        records_.push_back(r);
        ++seenThisFrame_;
        return;
    }
    TrackedRecord& r = records_[it->second];
    ++r.calls;
    if (r.lastFrame == frame) { ++stats_.dupInFrame; return; } // fan-out dup
    // A pointer absent for at least one frame is a NEW identity: never
    // re-proven on somebody else's history (post-gap reuse is flight-untested).
    if (frame > r.lastFrame + 1) {
        ++stats_.gapDrops;
        r.staticRun = 0;
        r.hasPrev = false;
        r.everMoved = false;
        r.boundsValid = false; // the new identity's bounds are not the old's
        r.path = 0;
        r.node = read64(record + 0x18, ok);
        r.lastFrame = frame;
        if (guardedRead(record + 0x170, r.prevPose, kPoseBytes)) r.hasPrev = true;
        else ++stats_.readFaults;
        if (guardedRead(record + 0xB0, r.bounds, kBoundsBytes)) r.boundsValid = true;
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
        r.everMoved = false;
        r.path = 0;
        r.node = node;
        if (guardedRead(record + 0x170, r.prevPose, kPoseBytes)) r.hasPrev = true;
        else ++stats_.readFaults;
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
    if (!r.hasPrev) {
        std::memcpy(r.prevPose, pose, kPoseBytes);
        r.hasPrev = true;
        return;
    }
    if (std::memcmp(pose, r.prevPose, kPoseBytes) == 0) {
        if (r.staticRun < 0xFFFF) ++r.staticRun;
    } else {
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
            // The 103339 rotating-in-place class: translation bit-static,
            // quat lanes churning. Never eligible, by construction.
            ++stats_.quatOnlyChanges;
        }
        if (!r.everMoved) { r.everMoved = true; ++stats_.moversTotal; }
        r.staticRun = 0;
    }
    std::memcpy(r.prevPose, pose, kPoseBytes);
}

KinematicMotionStats kinematicMotionStats() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    KinematicMotionStats out = stats_;
    out.tracked = static_cast<uint32_t>(records_.size());
    return out;
}

bool kinematicMotionRecordEligible(uint64_t record) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = index_.find(record);
    if (it == index_.end()) return false;
    return eligibleLocked(records_[it->second], frame_.load(std::memory_order_acquire));
}

} // namespace edvr
