#pragma once
// Change gate for the engine's per-frame render-data job (FUN_144321940,
// job 0 in the kinematic eval hook's bracket set). Settlement prop records
// are static: their per-frame truth is bit-identical frame over frame, yet
// the engine re-composes and re-writes their render data every frame. The
// gate consults a bounded cache at the job-0 entry: if every record of the
// call's collection matches its cached truth, the bracket forwards past the
// call entirely (no timed region, no engine work). Movers re-run whole.
// This is change gating, not suppression: bucket items persist cross-frame
// and the draws re-execute from them; the Phase-2 invariant is census
// draw-count equality (docs/engine-render-performance-2026-09-19.md,
// 2026-09-21 design entry).
//
// Key: fix.static_prop_updates (default off).
//
// Hook wiring lives in the EXISTING hooks, not here: the job-0 relay belongs
// to kinematic_eval_hook.cpp (CodeHook refuses a second patch) and calls
// the registered decide observer from its bracket; the reset invalidation
// trigger rides scheduler_stack_hook.cpp's resetObserved. This class is the
// pure cache + counters + reporting; test rigs link it with stub hook
// functions (the pattern set by scheduler_stack_probe).
#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>

namespace edvr {

class StaticPropGate {
public:
    static constexpr uint32_t kCollectionCap = 4096;      // bounded table (design)
    static constexpr uint32_t kRecordsPerCollection = 16; // fixed pool; oversize runs
    static constexpr uint32_t kRecordBytes = 24;          // 16 @ +0x170, 8 @ +0x570
    static constexpr uint32_t kRecordStride = 0x2F0;      // decomp_4321940
    static constexpr uint32_t kTruthOffset1 = 0x170;      // position + packed quat start
    static constexpr uint32_t kTruthBytes1 = 16;          // [+0x170,+0x180)
    static constexpr uint32_t kTruthOffset2 = 0x570;      // content mask
    static constexpr uint32_t kTruthBytes2 = 8;           // [+0x570,+0x578)
    static constexpr uint32_t kForcedRefreshFrames = 30;  // design failsafe N
    static constexpr uint32_t kCountSanityCap = 0x10000;  // count guard: above is wild

    enum class Verdict : uint32_t { Run, Skip };
    enum class InvalidSource : uint32_t { Journal, Reset, Session };

    // The hot entry: called by the job-0 bracket with the job's param struct.
    // Returns Skip only when every record matches the cache, the entry is
    // fresh, and no invalidation fired. Never allocates.
    Verdict decide(uintptr_t job0Param) noexcept;
    // The reset path (FUN_1436a0f50) fired: bucket items were rebuilt, so
    // every cached collection must re-run once.
    void noteReset() noexcept;
    // Frame clock + journal-boundary poll + the 20 s report cadence. Called
    // exactly once per owned Present (device_hook), whether armed or not.
    void notePresentFrame(uint32_t presentFrame) noexcept;

    // Config lifecycle (1 Hz re-poll idempotency lives in the caller).
    bool enable() noexcept;   // registers observers, attaches both hook sets
    void disable() noexcept;  // detaches, clears state
    bool enabled() const noexcept { return active_.load(std::memory_order_acquire); }
    const char* statusText() const noexcept { return statusText_; }

    struct Summary {
        uint64_t callsSeen = 0, callsSkipped = 0, callsRun = 0;
        uint64_t forcedRefreshes = 0;
        uint64_t invalidationsJournal = 0, invalidationsReset = 0, invalidationsSession = 0;
        uint64_t cacheEvictions = 0, cacheSize = 0, oversize = 0, verifyFaults = 0;
    };
    Summary summary() const noexcept;

    void writeJson(std::ostringstream& j) const;
    // Deterministic fixture for tools\static_prop_gate_json_test (the
    // scheduler_stack_probe selfTestPopulateForJson pattern).
    void selfTestPopulateForJson() noexcept;

private:
    struct RecordSnap {
        uint64_t record = 0;                     // slot identity (design: key by slot)
        uint8_t bytes[kRecordBytes] = {};        // raw truth copy, memcmp'd
    };
    struct CollectionEntry {
        uint64_t key = 0;                        // job-0 param[5]; 0 = empty slot
        uint32_t count = 0;
        uint32_t lastRunFrame = 0;
        uint32_t epoch = 0;                      // invalidated entries never match
        RecordSnap records[kRecordsPerCollection];
    };

    mutable std::mutex mutex_;
    // Fixed-size pool only: no allocation on the hot path (design).
    CollectionEntry table_[kCollectionCap];
    uint32_t liveCount_ = 0;
    uint32_t epoch_ = 1;                         // 0 would match fresh entries
    uint32_t frame_ = 0;
    uint64_t callsSeen_ = 0, callsSkipped_ = 0, callsRun_ = 0;
    uint64_t forcedRefreshes_ = 0;
    uint64_t invalidationsJournal_ = 0, invalidationsReset_ = 0, invalidationsSession_ = 0;
    uint64_t cacheEvictions_ = 0, oversize_ = 0, verifyFaults_ = 0;
    uint64_t armedTickMs_ = 0, lastReportTickMs_ = 0;
    // Journal poll latches: a CHANGE invalidates; the levels themselves do
    // not (journal_watch has no consumer callback; polling once per frame
    // from the present tick is the v1 consumer pattern, the same way the
    // other consumers read it).
    bool lastGameplay_ = false, lastJumpTunnel_ = false;
    uint32_t lastDisembarks_ = 0, lastEmbarks_ = 0;
    std::atomic<bool> active_{false};
    const char* statusText_ = "not_run";

    void invalidateLocked(InvalidSource source) noexcept;
    void clearLocked() noexcept;
    void reportLocked() noexcept;                // builds + logs the 20 s line
};

extern StaticPropGate staticPropGate;

// Config-facing lifecycle (same configure shape as schedulerStackProbeConfigure,
// driven by the once-per-second temporalPassConfigure re-poll).
void staticPropGateConfigure(bool on);

} // namespace edvr
