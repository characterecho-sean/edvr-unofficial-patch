#pragma once
// Read-only "scheduler stack-capture" probe: names the frame scheduler that
// drives the engine's kinematic/bucket worker entries (stage 0 of
// docs/engine-render-pipeline.md). Every engine worker entry is invoked
// only through vtable-table callback stubs (Tables A 0x1462b0000+,
// B 0x145dd0000+), so static RE cannot name the dispatcher. This probe
// instead captures, at each target's entry, a bounded scan of the live
// stack for return addresses into the game's image and hashes the
// collected list into a signature. Expected signature, stated here and in
// the commit that added it: ONE VA recurring at a consistent frame
// position across signatures is the dispatcher; VAs inside
// 0x1462b0000..0x1462c0000 or 0x145dd0000..0x145dde000 are table stubs
// (the caller above the stub is then the scheduler); a named .text VA is a
// scheduler candidate directly. READ-ONLY: no engine memory is written,
// nothing is allocated on the capture path (fixed arrays only), and the
// whole probe no-ops when the executable's identity (PE timestamp + image
// size + target prologues) does not match the hash-verified build
// (SHA-256 e6be8bbe...4e988).
//
// Targets (index: RVA, role):
//   0: 0x4321940 worker entry (UpdateRenderDataJob)   -- observed through
//   1: 0x4320340 worker entry, batch path              -- the kinematic
//      eval hook's job-0/1 relays: those RVAs already carry that hook's
//      patch and CodeHook refuses a second patch. The job wrappers run
//      pre-forward exactly where a dedicated hook would sit.
//   2: 0x42DF940 per-record drain
//   3: 0x36A0F50 reset+repopulate
// Targets 2/3 carry this probe's own CodeHooks (scheduler_stack_hook.cpp).
#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>

namespace edvr {

class SchedulerStackProbe {
public:
    static constexpr uint32_t kExpectedTimestamp=1788384820u;
    static constexpr uint32_t kExpectedImageSize=104894464u;
    static constexpr uint32_t kTargetCount=4u;
    static constexpr uint32_t kStackCap=24u;   // return addresses per signature
    static constexpr uint32_t kSigCap=64u;     // distinct signatures per target
    static constexpr uint32_t kScanSlots=256u; // qwords examined at most, per call
    static constexpr uint32_t kMaxGaps=8u;     // tolerated consecutive non-return qwords
    // Return addresses accepted in [imageBase+kRangeLo, imageBase+kRangeHi];
    // at the exe's preferred base 0x140000000 this is [0x140001000,
    // 0x144DB3000] -- all of .text plus the callback tables' neighbourhood.
    static constexpr uintptr_t kRangeLo=0x1000u;
    static constexpr uintptr_t kRangeHi=0x4DB3000u;
    // The two RVAs this probe hooks itself. Prologues verified against the
    // hash-checked binary at install (tools/build_diff_targets.json fields
    // for 0x4321940/0x4320340/0x36A0F50; 0x42DF940 verified by hand from
    // analysis/EliteDangerous64.exe, 2026-09-21).
    static constexpr uintptr_t kHookRvas[2]={0x42DF940u,0x36A0F50u};
    static constexpr uint32_t kHookTargetIndex[2]={2u,3u};

    enum class HookStatus:uint32_t {NotRun,Installed,IdentityMismatch,OpcodeMismatch,InstallFailed};

    struct SigEntry {
        uint64_t fnv=0;
        uint64_t count=0;
        uint32_t depth=0;
        uint64_t stack[kStackCap]={}; // exemplar: the first stack that hashed here
    };
    struct TargetStat {
        // Relaxed atomics, written lock-free on the capture path; the table
        // fields below are plain and only touched under the probe mutex.
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> faults{0};
        uint32_t used=0;
        uint64_t overflow=0;    // distinct signatures past kSigCap
        uint64_t collisions=0;  // same FNV-1a-64, different stack (counted, not merged)
        SigEntry sigs[kSigCap];
    };
    struct Summary {
        uint64_t calls[kTargetCount];
        uint64_t faults[kTargetCount];
        uint32_t unique[kTargetCount];
        uint64_t overflow[kTargetCount];
        uint64_t collisions[kTargetCount];
    };

    // Arm: validate the executable and install the hooks (through
    // attachSchedulerStackHooks), then clear and activate. Fails closed on
    // any identity/opcode/install problem and says so via hookStatus().
    bool arm() noexcept;
    // Stops new callbacks and detaches; hooks stay for the process lifetime.
    void finish() noexcept;
    // Stand down and clear all state.
    void reset() noexcept;
    bool active() const noexcept{return active_.load(std::memory_order_acquire);}
    HookStatus hookStatus() const noexcept{return hookStatus_;}
    const char* hookStatusText() const noexcept;

    // The capture path. Called at a target's entry, BEFORE the call is
    // forwarded to the original, with the observed function's entry RSP
    // (the address of the caller's return-address slot). One atomic load
    // when inactive; no allocation; fixed arrays only.
    void noteEntry(uint32_t target,uintptr_t entryRsp) noexcept;

    // The per-present clock feed (same call site as the kinematic
    // instruments, exactly once per owned Present). Drives the ~20 s
    // totals report.
    void notePresentFrame(uint32_t presentFrame) noexcept;

    Summary summary() const noexcept;
    void writeJson(std::ostringstream& json) const;
    // Deterministic fixture for tools\scheduler_stack_json_test: fills every
    // production writeJson path with distinctive values so the build gate
    // can assert an exact round-trip.
    void selfTestPopulateForJson() noexcept;

    // Scan upward from `top` collecting up to `cap` qwords in
    // [rangeLo,rangeHi], tolerating up to kMaxGaps consecutive misses and
    // at most kScanSlots examined slots. Real stacks interleave locals and
    // saved registers between return addresses; the gap budget keeps the
    // scan deterministic (fixed bounds, no heuristic state). *faulted is
    // set when a read fault truncated the scan. Reads are SEH-guarded; the
    // caller passes a stack region it owns in tests.
    static uint32_t captureStack(uintptr_t top,uintptr_t rangeLo,uintptr_t rangeHi,
                                 uint64_t* out,uint32_t cap,bool* faulted) noexcept;
    static uint64_t fnv1a64(const uint64_t* values,uint32_t count) noexcept;

    static const char* targetName(uint32_t target) noexcept;

private:
    mutable std::mutex mutex_;
    std::atomic<bool> active_{false};
    uintptr_t imageBase_=0;
    HookStatus hookStatus_=HookStatus::NotRun;
    TargetStat targets_[kTargetCount];
    uint32_t frame_=0;
    uint64_t armedTickMs_=0,lastReportTickMs_=0;
    bool reportedDeadWindow_=false;

    void clearLocked() noexcept;
};

extern SchedulerStackProbe schedulerStackProbe;

// Config-facing lifecycle, driven by the once-per-second
// temporalPassConfigure re-poll (the kinematic_motion.cpp shape):
// configure(true) is idempotent while live; configure(false) stands down
// and clears. On any hook refusal the probe logs once per distinct status
// and stays inert -- the degrade philosophy the rest of the instruments
// keep. Key: advanced.scheduler_probe (default off).
void schedulerStackProbeConfigure(bool on);
void schedulerStackProbeShutdown();

} // namespace edvr
