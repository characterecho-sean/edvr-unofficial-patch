#pragma once
// Phase-1 engine-sourced motion: the kinematic tracker. Feeds on the
// KinematicRig eval-hook stream (the same feed KinematicEvalProbe captures
// from, descriptor+0x10 record pointers) and proves per-record stasis from
// engine truth: record+0x170 world translation plus the record+0x17C packed
// quaternion, bit-exact over consecutive rendered frames, clocked by the
// per-present frame counter (the mesh clock is refuted, flight 083323).
//
// STAGE A IS TRACKER + DIAGNOSTICS ONLY -- no rendering change. Ownership
// and MV injection (stage B) land after the world-bounds layout is
// flight-decoded: the docs conflict (FUN_14432CCC0 writes 16 floats at
// record+0xB0..0xEC; the updater's current/previous pair lives at
// +0xF0/+0x1C0; a 4-float change-test block sits at +0x120), so stage A
// captures the +0xB0..+0x130 block per record and dumps it for known
// movers/statics on one flight. Spec: docs/kinematic-motion-injection-
// 2026-09-19.md, 2026-09-20 10:52 entry.
//
// Rules that are the whole point: a static label is per-frame evidence,
// never sticky; unknown identity, any gap, a node change, a read fault and
// table overflow all preserve the existing motion path; the quat lanes are
// part of the stasis compare because flight 103339's rotating-in-place
// class (292 records, 0.8-1.6 deg/present, zero translation) must never be
// labeled.
#include <cstdint>

namespace edvr {

// A snapshot of the tracker's counters, taken under the module mutex. The
// periodic log line prints these; the test rig asserts them.
struct KinematicMotionStats {
    uint64_t observed = 0;          // eval observations seen while active
    uint64_t dupInFrame = 0;        // fan-out dups (second+ sight in a frame)
    uint64_t readFaults = 0;
    uint64_t recordOverflow = 0;    // distinct pointers past kTrackCap
    uint64_t poseChanges = 0;       // per-record-per-frame pose byte changes
    uint64_t translationChanges = 0;//   ...with translation bytes changed
    uint64_t quatOnlyChanges = 0;   //   ...quat bytes only (rotation-in-place)
    uint64_t nearMiss = 0;          //   ...translation changed by 0 < d < 1 mm
    uint64_t moversTotal = 0;       // records that ever changed pose
    uint64_t gapDrops = 0;          // pointer re-seen after an absent frame
    uint64_t nodeChanges = 0;       // same pointer, different node (reuse)
    uint64_t sameFrameChanges = 0;  // identity/pose changed between dups of one frame
    uint64_t boundsChanges = 0;     // bounds-block byte changes (diagnostic)
    uint64_t framesCounted = 0;     // ended frames with the clock live
    uint64_t zeroRecordFrames = 0;  // ended frames with zero records (stand-down shape)
    uint64_t eligibleFrames = 0;    // ended frames with >=1 eligible record
    uint32_t tracked = 0;           // table size at snapshot
    uint32_t seenLast = 0;          // records seen in the last ended frame
    uint32_t eligibleLast = 0;      // proven-static records in the last ended frame
    uint32_t moversLast = 0;        // records whose pose changed in it
};

// Idempotent across the once-per-second config re-poll: configure(true)
// while live is a no-op, configure(false) stands down and clears state.
void kinematicMotionConfigure(bool on);
void kinematicMotionShutdown();
bool kinematicMotionActive() noexcept;
// The tracker clock: exactly once per owned Present, from device_hook beside
// the probe's call. The first call after configure seeds the clock.
void kinematicMotionNotePresentFrame(uint32_t presentFrame) noexcept;
// The eval-hook feed. Registered with the hook only while active.
void kinematicMotionObserve(uintptr_t descriptor) noexcept;
KinematicMotionStats kinematicMotionStats() noexcept;
// Test-rig introspection: eligibility of one record by its live pointer.
bool kinematicMotionRecordEligible(uint64_t record) noexcept;
// Test-rig introspection: the once-per-session bounds dump stage (0 = armed,
// 1 = part A dumped, 2 = complete).
int kinematicMotionBoundsDumpStage() noexcept;

} // namespace edvr
