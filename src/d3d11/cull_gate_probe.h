#pragma once
// The cull gate probe (advanced.cull_gate_capture): with an armed eye run,
// record for three frames what the engine's per-view admission tests decided
// for every engine record, beside what the depth capture and the ledger see,
// so one parked capture can settle which test admits the pool draws and
// price a cull against the depth truth (design-occlusion-culling-2026-09-22.md
// §9). No reject, nothing on screen: it observes through relays that are
// already installed.
//
// Two engine functions, both already patched by kinematic_eval_hook.cpp:
//
//   * FUN_14430EFE0 -- the kinematic "evaluator" hook's own target -- IS the
//     traversal's per-(record, view) gate (decomp_430EFE0.txt): param_1 the
//     gate context (param_1[0] -> the traversal's param_1, whose [0] is the
//     render context; param_1[2] the engine record, stride 0x2F0), param_2 the
//     output {u32 LOD index, u8 passed}, param_3 the VIEW (ctx+0x40+i*0x6A0).
//     Its verdict feeds only the type-2 item path (§9 close-out 1); recorded
//     after the forward.
//   * FUN_1442B4420 -- the bucket bracket's target -- is the draw-item
//     builder: param_1 the record's pose context (rec+0x290), param_2 the
//     render context, param_3 the collection's active view mask, param_4
//     rec+0x210 (so the record is param_4-0x210). Its own per-view admission
//     (decomp_42B4420.txt:250-275): view+0x570 & mask, then FUN_1404F4E10
//     (the view's planes, +0x30 pointer / +0x44 count) on the pose's
//     transformed bounds, then -- under the global DAT_145ea3399 and the
//     view's +0x68D bit 0 -- the FUN_14288AC40 visibility callback. The probe
//     recomputes the first two with the engine's own FUN_1404F4E10 on the
//     builder's inputs (before the forward) and records where the third
//     would apply; it never calls the callback.
//
// Per builder call it also records the record's pose (+0x170 position, +0x17C
// packed quaternion), its bounding sphere (+0x240 world centre, +0x270 local,
// +0x280 radius), its view mask (+0x208) and LOD nibbles (+0x210), and the
// pose context's instance entries (count +0x48, array +0x50 stride 0x58;
// per entry the model at +0x00 -- its local sphere, centre at model+0x00 and
// radius at model+0x10 -- the sub-item count at +0x20 and array at +0x28,
// 32-byte sub-items = float4 quaternion + float4 position, the local
// transforms FUN_14433DB20 composes into each part's world matrix) -- the
// offline join to the t33 pool's per-part records. Once per (context,
// frame) it dumps the context's view array: every view's raw 0x6A0 bytes and
// its plane array, the context's LOD scale (+0x30) and bit table (+0x1A840).
//
// A third function, through its own patch (kinematic_eval_hook.cpp, installed
// only for this probe after a build-keyed signature): FUN_1442B3FC0, which the
// builder calls per (sub-item, admitted view) from its sub-item loop
// (decomp_42B4420.txt:504-523) -- the per-PART admission. It tests the part's
// own sphere: param_1[0] -> the world centre the builder composed from the
// model's +0x00 through the sub-item and the pose, param_1[1] -> a copy of
// the model's +0x10 (its first float the radius), with FUN_1404F4E10 for the
// frustum half and a screen-size and LOD pick against view +0x540/+0x550/
// +0x560 (decomp_42B3FC0.txt), and writes {u32 LOD, u8 passed} to param_2;
// the builder ORs the view's +0x570 bits into the item's mask on a pass. The
// probe records each call AFTER its forward (the verdict never touched), with
// the part's identity read from the builder's frame around the call -- the
// current entry and sub-item, verified against the engine's own arrays before
// it is believed -- and the builder row of the enclosing call, joined through
// a thread-local the builder bracket holds across its forward.
//
// Frames are the eye run's ledger frames (object_probe's g_frame+1, the frame
// the draws submitted now belong to), published by the render thread; the
// jobs run on worker threads and are stamped with the value current when
// they observe, so the reader tests the alignment against the ledger rather
// than assuming it. A part row takes its builder row's frame, so a builder
// call the stamp moves across keeps all its parts. Everything is
// preallocated at arm; the workers append with one atomic reservation each
// and never allocate. The file is edvr_logs\pool\gate_<stamp>.bin
// ('EDVRGATE' v2; v1 had no part rows and no model spheres);
// tools/cull_gate_probe.py reads both.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace edvr {

class CullGateProbe {
public:
    // FUN_1404F4E10's shape: (view, float4 point, float2 interval) -> -1
    // outside a plane, 1 inside all, 0 straddling (decomp_04F4E10.txt).
    using FrustumFn = uint64_t (__fastcall*)(uintptr_t view, const float* point, const float* interval);

    static constexpr uint32_t kVersion = 2;
    static constexpr uint32_t kNoRow = 0xFFFFFFFFu;  // == kGateProbeNoRow (kinematic_eval_hook.h)
    static constexpr uint32_t kFrames = 3;          // the first complete frame and two after it
    static constexpr uint32_t kMaxViews = 64;       // ctx+0x1A940 is capped at 0x40 by its builder
    static constexpr uint32_t kViewBytes = 0x6A0;
    static constexpr uint32_t kMaxPlanes = 32;
    static constexpr uint32_t kMaxDumps = 16;       // (context, frame) pairs
    // Window caps, preallocated at arm (~80 MB): ~11 views x ~10k records a
    // frame reach the gate, ~1.5k rigs the builder, ~12k parts the pool.
    static constexpr uint32_t kGateCap = 1u << 19;  // gate observations over the window
    static constexpr uint32_t kBuilderCap = 1u << 15;
    static constexpr uint32_t kEntryCap = 1u << 18;
    static constexpr uint32_t kSubItemCap = 1u << 19;
    static constexpr uint32_t kEntriesPerRecord = 256;
    // Part tests over the window: ~13.5k builder sub-items a frame, each
    // tested per admitted view -- the two eyes (~27k, §10) plus the
    // shadow-like views the record admits -- so 2^18 rows hold three frames
    // at up to ~87k tests a frame (~6.5 views a part); past it, counted.
    static constexpr uint32_t kPartCap = 1u << 18;

    // Engine offsets (build 332841, the hash-verified executable).
    static constexpr uintptr_t kFrustumRva = 0x4F4E10u;     // FUN_1404F4E10
    static constexpr uintptr_t kVisGlobalRva = 0x5EA3399u;  // DAT_145ea3399
    // The builder's frame around its FUN_1442B3FC0 call, from the part test's
    // param_1 (= the builder's rbp+0x70, a six-pointer block it fills per
    // sub-item). Read out of the builder's machine code 0x1442B4429..
    // 0x1442B4B91 (decomp_42B4420.txt:434-512); the hook installs only when
    // those instructions are byte-for-byte in place, and notePart still
    // checks every relation below before it believes an identity.
    static constexpr intptr_t kFrameCentre = -0x20;     // param_1[0] -> the world centre (local_3a8, rbp+0x50)
    static constexpr intptr_t kFrameSphere = -0x10;     // param_1[1] -> the model's +0x10 copy (local_398, rbp+0x60)
    static constexpr intptr_t kFrameMeshCell = -0xC0;   // param_1[3] -> *(model+0x40) (local_448, rbp-0x50)
    static constexpr intptr_t kFrameLodTable = 0xD0;    // param_1[4] -> *(entry+8)'s 0x80 bytes (rbp+0x140)
    static constexpr intptr_t kFrameView = -0x140;      // the view it passes as param_3 (local_4c8, rsp+0x30)
    static constexpr intptr_t kFramePose = -0xD8;       // the builder's param_1 (rbp-0x68) == param_1[2]
    static constexpr intptr_t kFrameCtx = -0xB8;        // the render context (rbp-0x48) == param_1[5]
    static constexpr intptr_t kFrameEntry = -0xC8;      // the current instance entry (local_450, rbp-0x58)
    static constexpr intptr_t kFrameModel = -0xB0;      // the entry's model (local_438, rbp-0x40)
    static constexpr intptr_t kFrameSub = -0xE0;        // the current 32-byte sub-item (local_468, rbp-0x70)

    struct GateObs {
        uint64_t record = 0, ctx = 0;
        uint32_t frame = 0, lod = 0;
        uint16_t view = 0;
        uint8_t pass = 0, flags = 0;   // flags bit 0: the view pointer is not in the context's array
        uint32_t valid = 0;
    };
    struct BuilderObs {
        uint64_t record = 0, pose = 0, ctx = 0, node = 0, activeMask = 0, recMask = 0;
        uint64_t nibbles[4] = {};
        uint64_t bitOk = 0, frustumPass = 0, frustumInside = 0, visApplies = 0;
        float position[3] = {};
        uint8_t quat[8] = {};
        float worldCentre[4] = {}, localCentre[4] = {}, radius[4] = {};
        float poseQuat[4] = {}, poseT[4] = {}, poseCentre[4] = {}, poseExtents[4] = {};
        float centre[4] = {};   // the builder's transformed centre, as its frustum test sees it
        uint32_t frame = 0, viewCount = 0, flags = 0, entryCount = 0, entryFirst = 0, entriesCopied = 0;
        uint32_t valid = 0;
    };
    // BuilderObs.flags
    static constexpr uint32_t kFlagRecordFault = 1u, kFlagPoseFault = 2u, kFlagPoseMismatch = 4u,
                              kFlagViewsFault = 8u, kFlagNoFrustum = 16u, kFlagEntriesFault = 32u,
                              kFlagEntriesTruncated = 64u;
    struct EntryObs {
        uint64_t model = 0;
        uint32_t subCount = 0, subFirst = 0, subCopied = 0;
        // The model's local sphere as the builder reads it (decomp_42B4420.txt:
        // 391-397): +0x00 the centre (xyz; w carried through the transform),
        // +0x10 the four floats FUN_1442B3FC0 gets as param_1[1], [0] the radius.
        float modelCentre[4] = {}, modelSphere[4] = {};
    };
    struct SubItem { float q[4], p[4]; };
    // One FUN_1442B3FC0 call, read after its forward.
    struct PartObs {
        uint64_t pose = 0;       // param_1[2]: the builder's pose context (its row carries the same)
        uint64_t subItem = 0;    // the sub-item under test (the builder's rbp-0x70); 0 unless verified
        uint64_t viewBits = 0;   // view +0x570: what the builder ORs into the item's mask on a pass
        float centre[3] = {};    // *param_1[0]: the part's world sphere centre, as tested
        float radius = 0;        // *param_1[1]: the model's +0x10, as tested
        uint32_t frame = 0, builderRow = kNoRow, entry = kNoRow, sub = kNoRow, lod = 0;
        uint16_t view = 0xFFFF;  // index in the render context's view array (param_1[5])
        uint8_t pass = 0, flags = 0;
        uint32_t valid = 0;
    };
    // PartObs.flags
    static constexpr uint8_t kPartViewForeign = 1u,      // the view is not in the context's view array
                             kPartUnverified = 2u,       // the builder's frame did not verify: no identity
                             kPartForeignCaller = 4u,    // not called from the builder's sub-item loop
                             kPartOwnerMismatch = 8u,    // the enclosing builder row's pose is not param_1[2]
                             kPartSphereFault = 16u;     // the sphere or the view's bits could not be read
    struct ViewDump {
        uint64_t ctx = 0;
        uint32_t frame = 0, count = 0;
        float lodScale = 0;
        uint32_t bitTable[kMaxViews] = {};
        uint32_t planeCount[kMaxViews] = {};
        float planes[kMaxViews][kMaxPlanes][4] = {};
        uint8_t raw[kMaxViews][kViewBytes] = {};
        uint32_t faults = 0;
        uint32_t valid = 0;
    };

    void configure(bool on) { on_ = on; }
    bool enabled() const { return on_; }
    // Preallocates and opens the window [first, first+kFrames). `frustum` is
    // the engine's own FUN_1404F4E10 after its prologue check, or null (the
    // builder's frustum verdicts are then absent, flagged per record); the
    // CALLER attaches the relays and reports their status.
    bool arm(uint32_t first, FrustumFn frustum, uint8_t visGlobal) noexcept;
    void disarm() noexcept { armed_.store(false, std::memory_order_release); }
    bool armed() const { return armed_.load(std::memory_order_acquire); }
    uint32_t firstFrame() const { return first_; }
    uint32_t lastFrame() const { return first_ + kFrames - 1; }
    void setFrame(uint32_t frame) noexcept { frame_.store(frame, std::memory_order_release); }
    uint32_t frame() const { return frame_.load(std::memory_order_acquire); }

    // The relay observers (worker threads, noexcept, allocation-free).
    void noteGate(uintptr_t gateCtx, uintptr_t out, uintptr_t view) noexcept;
    // Returns the row it kept, or kNoRow (outside the window, a fault-free
    // reservation past the cap): the bracket hands it to this call's parts.
    uint32_t noteBuilder(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept;
    // FUN_1442B3FC0 after its forward: items = param_1, out = param_2, view =
    // param_3; builderRow = the enclosing builder call's row; fromBuilder =
    // the call came from the builder's sub-item loop (its return address).
    void notePart(uintptr_t items, uintptr_t out, uintptr_t view, uint32_t builderRow, bool fromBuilder) noexcept;
    // Whether FUN_1442B3FC0's patch is live for this window (set by the
    // caller after the attach): the file's header flag bit 1, so a reader
    // tells "stood down, no rows by construction" from "hooked, no calls".
    void setPartHooked(bool on) noexcept { partHooked_ = on; }
    bool partHooked() const noexcept { return partHooked_; }

    // Writes gate_<stamp>.bin. False on an I/O failure (the counters still
    // say what was captured).
    bool write(const wchar_t* path) noexcept;
    void reset() noexcept;

    struct Counts {
        uint32_t gateCalls = 0, gateKept = 0, gateDropped = 0;
        uint32_t builderCalls = 0, builderKept = 0, builderDropped = 0;
        uint32_t entriesDropped = 0, subItemsDropped = 0;
        uint32_t faults = 0, recordMismatch = 0, dumps = 0, dumpsDropped = 0;
        // Part tests in the window; kept rows; reserved past the cap; rows
        // whose builder frame did not verify (verdict kept, no identity);
        // calls from another caller; calls outside a kept builder row.
        uint32_t partCalls = 0, partKept = 0, partDropped = 0;
        uint32_t partUnverified = 0, partForeign = 0, partUnlinked = 0;
    };
    Counts counts() const noexcept;
    uint32_t distinctRecords() const noexcept;   // builder records in the kept observations

private:
    bool inWindow(uint32_t* frame) const noexcept;
    void dumpViews(uintptr_t ctx, uint32_t frame) noexcept;

    bool on_ = false;
    std::atomic<bool> armed_{false};
    std::atomic<uint32_t> frame_{0};
    uint32_t first_ = 0;
    FrustumFn frustum_ = nullptr;
    uint8_t visGlobal_ = 0;
    bool partHooked_ = false;

    std::vector<GateObs> gate_;
    std::vector<BuilderObs> builder_;
    std::vector<EntryObs> entries_;
    std::vector<SubItem> subs_;
    std::vector<PartObs> parts_;
    std::unique_ptr<ViewDump[]> dumps_;
    std::atomic<uint32_t> gateNext_{0}, builderNext_{0}, entryNext_{0}, subNext_{0}, partNext_{0};
    std::atomic<uint32_t> gateCalls_{0}, builderCalls_{0}, faults_{0}, recordMismatch_{0};
    std::atomic<uint32_t> entriesDropped_{0}, subItemsDropped_{0}, dumpsDropped_{0};
    std::atomic<uint32_t> partCalls_{0}, partUnverified_{0}, partForeign_{0}, partUnlinked_{0};
    std::atomic<uint32_t> dumpCount_{0};
    std::atomic<uint64_t> dumpKey_[kMaxDumps];   // ctx ^ (frame << 48): claimed slots, zeroed by arm()
    std::mutex dumpMutex_;
};

extern CullGateProbe cullGateProbe;

// Free-function observers for kinematicEvalSetGateProbeObservers: forward to
// the global probe while it is armed.
void cullGateProbeGateObserver(uintptr_t gateCtx, uintptr_t out, uintptr_t view) noexcept;
uint32_t cullGateProbeBuilderObserver(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept;
void cullGateProbePartObserver(uintptr_t items, uintptr_t out, uintptr_t view, uint32_t builderRow,
                               bool fromBuilder) noexcept;

}  // namespace edvr
