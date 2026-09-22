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
// per entry the model at +0x00, the sub-item count at +0x20 and array at
// +0x28, 32-byte sub-items = float4 quaternion + float4 position, the local
// transforms FUN_14433DB20 composes into each part's world matrix) -- the
// offline join to the t33 pool's per-part records. Once per (context,
// frame) it dumps the context's view array: every view's raw 0x6A0 bytes and
// its plane array, the context's LOD scale (+0x30) and bit table (+0x1A840).
//
// Frames are the eye run's ledger frames (object_probe's g_frame+1, the frame
// the draws submitted now belong to), published by the render thread; the
// jobs run on worker threads and are stamped with the value current when
// they observe, so the reader tests the alignment against the ledger rather
// than assuming it. Everything is preallocated at arm; the workers append
// with one atomic reservation each and never allocate. The file is
// edvr_logs\pool\gate_<stamp>.bin ('EDVRGATE' v1); tools/cull_gate_probe.py
// reads it.
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

    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kFrames = 3;          // the first complete frame and two after it
    static constexpr uint32_t kMaxViews = 64;       // ctx+0x1A940 is capped at 0x40 by its builder
    static constexpr uint32_t kViewBytes = 0x6A0;
    static constexpr uint32_t kMaxPlanes = 32;
    static constexpr uint32_t kMaxDumps = 16;       // (context, frame) pairs
    // Window caps, preallocated at arm (~50 MB): ~11 views x ~10k records a
    // frame reach the gate, ~1.5k rigs the builder, ~12k parts the pool.
    static constexpr uint32_t kGateCap = 1u << 19;  // gate observations over the window
    static constexpr uint32_t kBuilderCap = 1u << 15;
    static constexpr uint32_t kEntryCap = 1u << 18;
    static constexpr uint32_t kSubItemCap = 1u << 19;
    static constexpr uint32_t kEntriesPerRecord = 256;

    // Engine offsets (build 332841, the hash-verified executable).
    static constexpr uintptr_t kFrustumRva = 0x4F4E10u;     // FUN_1404F4E10
    static constexpr uintptr_t kVisGlobalRva = 0x5EA3399u;  // DAT_145ea3399

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
    };
    struct SubItem { float q[4], p[4]; };
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
    void noteBuilder(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept;

    // Writes gate_<stamp>.bin. False on an I/O failure (the counters still
    // say what was captured).
    bool write(const wchar_t* path) noexcept;
    void reset() noexcept;

    struct Counts {
        uint32_t gateCalls = 0, gateKept = 0, gateDropped = 0;
        uint32_t builderCalls = 0, builderKept = 0, builderDropped = 0;
        uint32_t entriesDropped = 0, subItemsDropped = 0;
        uint32_t faults = 0, recordMismatch = 0, dumps = 0, dumpsDropped = 0;
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

    std::vector<GateObs> gate_;
    std::vector<BuilderObs> builder_;
    std::vector<EntryObs> entries_;
    std::vector<SubItem> subs_;
    std::unique_ptr<ViewDump[]> dumps_;
    std::atomic<uint32_t> gateNext_{0}, builderNext_{0}, entryNext_{0}, subNext_{0};
    std::atomic<uint32_t> gateCalls_{0}, builderCalls_{0}, faults_{0}, recordMismatch_{0};
    std::atomic<uint32_t> entriesDropped_{0}, subItemsDropped_{0}, dumpsDropped_{0};
    std::atomic<uint32_t> dumpCount_{0};
    std::atomic<uint64_t> dumpKey_[kMaxDumps];   // ctx ^ (frame << 48): claimed slots, zeroed by arm()
    std::mutex dumpMutex_;
};

extern CullGateProbe cullGateProbe;

// Free-function observers for kinematicEvalSetGateProbeObservers: forward to
// the global probe while it is armed.
void cullGateProbeGateObserver(uintptr_t gateCtx, uintptr_t out, uintptr_t view) noexcept;
void cullGateProbeBuilderObserver(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept;

}  // namespace edvr
