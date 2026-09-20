#pragma once
// Phase-1 engine-sourced motion: the kinematic tracker. Feeds on the
// KinematicRig eval-hook stream (the same feed KinematicEvalProbe captures
// from, descriptor+0x10 record pointers) and proves per-record stasis from
// engine truth: record+0x170 world translation plus the record+0x17C packed
// quaternion, bit-exact over consecutive rendered frames, clocked by the
// per-present frame counter (the mesh clock is refuted, flight 083323).
//
// STAGE B: eligible records' world bounding SPHERES are published for the
// temporal pass's ownership coverage (the t19/t20 compose veto). The sphere
// layout is flight-proven (132856): local centre float4 at record+0x270,
// radius float at +0x280, written at spawn/LOD refresh by FUN_14433C870;
// world centre = R^T x local + T off the +0xF0/+0x120 matrix (exact to
// 0.0000 on every dumped static), world radius = local radius x max 3x3
// column scale. The sphere bytes are part of the stasis compare because an
// LOD refresh rewrites them with zero pose change. Spec: docs/kinematic-
// motion-injection-2026-09-19.md, 2026-09-20 13:55 entry.
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
    uint64_t sphereChanges = 0;     // sphere-byte (+0x270..+0x28F) changes: LOD refresh shape
    uint64_t sphereRejected = 0;    // eligible records refused for the upload (bad/absent sphere or matrix)
    uint64_t framesCounted = 0;     // ended frames with the clock live
    uint64_t zeroRecordFrames = 0;  // ended frames with zero records (stand-down shape)
    uint64_t eligibleFrames = 0;    // ended frames with >=1 eligible record
    uint64_t uploadGeneration = 0;  // bumps when the published sphere set changes
    uint64_t uploadStuck = 0;       // eligible census moved but the generation held a whole summary window
    uint32_t tracked = 0;           // table size at snapshot
    uint32_t seenLast = 0;          // records seen in the last ended frame
    uint32_t eligibleLast = 0;      // proven-static records in the last ended frame
    uint32_t moversLast = 0;        // records whose pose changed in it
    uint32_t uploadedLast = 0;      // spheres in the current upload snapshot
};

// One uploaded record for the ownership coverage pass: the world bounding
// sphere plus the phase-2 seam fields. 80 bytes, matching the HLSL side's
// KinSphere layout field for field (temporal_shader_source.h).
struct KinematicSphereGpu {
    float    centre[3];        // world, R^T x local + T
    float    radius;           // world, local x max 3x3 column scale
    uint32_t kind;             // 0 = static-zero (phase 1)
    uint32_t reserved[3];
    float    prevMap[12];      // 3x4 prev-frame map, zero-filled in phase 1
};
static_assert(sizeof(KinematicSphereGpu) == 80, "the HLSL KinSphere stride contract");

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
// The upload snapshot for the coverage pass: reports the current published
// sphere set (built at each ended frame) and copies up to cap entries into
// out, under the module mutex. Returns the generation -- a caller compares it
// against its last-upload generation and skips the copy when unchanged.
// Generation 0 = never published (tracker fresh off); *frame is the ended
// frame the snapshot describes, *count the published set's size (a null out
// with cap 0 peeks count and generation without copying).
uint64_t kinematicMotionSphereSnapshot(KinematicSphereGpu* out, uint32_t cap,
                                       uint32_t* count, uint32_t* frame) noexcept;
// Test-rig introspection: eligibility of one record by its live pointer.
bool kinematicMotionRecordEligible(uint64_t record) noexcept;
// Test-rig introspection: the once-per-session bounds dump stage (0 = armed,
// 1 = part A dumped, 2 = complete).
int kinematicMotionBoundsDumpStage() noexcept;

} // namespace edvr
