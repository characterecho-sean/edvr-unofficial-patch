#pragma once
// The settlement LOD governor (fix.settlement_detail). Every frame it computes
// a factor k from the engine's own signals and, while it acts, hands it to
// the engine the one way the engine already takes it: the render context's
// LOD scale s (ctx+0x30), which the game's own level-of-detail tests read.
// The engine rebuilds that context every frame and ends by storing s from
// the settings (FUN_142819D90, decomp_2819D90.txt:108; per-frame evidence in
// docs/design-settlement-lod-bias-2026-09-22.md section 9). A bracket on that
// function (kinematic_eval_hook.cpp) calls lodGovernorSetterObserver right
// after the store; the governor reads the value the engine just wrote -- the
// game's s -- and, while acting, writes s x k back. Every test of the frame
// then runs at the reduced detail, unchanged: no verdict is overridden, no
// draw or mesh touched. The next frame's rebuild writes the game's value
// again, so k = 1, observe mode, or EDVR standing down restores the game's
// detail by the engine's own hand within a frame.
//
// SIGNALS, once a frame at the frame boundary (the caller thread):
//   * density -- the draw-item builder's calls since the last boundary (one
//     call builds one engine record, FUN_1442B4420) and the per-part tests
//     inside them (FUN_1442B3FC0), counted in the builder bracket and the
//     part relay with one owner-thread increment per call;
//   * frame work -- the caller work per cycle the native runtime sends with
//     the newest submitted frame, EdvrNativeTimingFrame::callerWorkMs
//     (version 5): wall milliseconds on the game's caller thread from one
//     pose wait's return to the next one's entry, the runtime's cycle minus
//     its next-wait roundtrip -- the game's work, both submits and the waits
//     inside them, everything the frame must fit -- for the cycle before
//     that frame. A version 3 or 4 runtime sends none, and the producer's
//     application time stands in (NativeTimingSnapshot::applicationMs, the
//     perf monitor's "app CPU": pose wait end to submit plus the eye
//     treatments -- the pre-submit phase only, which on the first shadow
//     flight read 8.4 ms against 14.4 ms of caller work and held k at 1);
//     the log names which, and without caller work auto holds. Held against
//     the display period, 1000 / baseDisplayHz (version 4 and later), else
//     the session's first predicted period. FRESH evidence only: a sample
//     counts once, at the boundary that first sees its new sequence, and
//     only if it was captured within 2 s. A lost lease, a sample already
//     older than 2 s, a sequence unchanged for more than 2 s, an invalid
//     frame, or a change of the timing generation, source or display period
//     EXPIRES the evidence: the policy empties its ring and drops the second
//     in progress, and k holds (reviews/lod-governor-review-2026-09-23.md,
//     finding 1: a frozen feed climbed k 1.25 -> 2.75 in 6.6 s on the same
//     30 samples);
//   * the display slot -- one QueryPerformanceCounter read a boundary, the
//     interval since the previous boundary this frame's cycle: longer than
//     1.5 x the period, it took two slots (a miss). The first boundary after
//     enabling, after an expiry or a bad sample, on foot and after leaving
//     the settlement has no interval. A miss is classed on its frame's
//     fresh sample: GPU-bound while the application's GPU render time -- the
//     newest valid application-render sample of gpu_frame_timing.h, the
//     "Application-render GPU" instrument -- was at or over the period - 0.5
//     ms (the caller work cannot tell: it spans Present, where a GPU-bound
//     game blocks); else UNEXPLAINED while the caller work was under the
//     period - 0.3 ms (the GPU under that or unknown); else the CPU's. Only
//     the CPU's trigger; the other two are counted and named, and none is
//     called LOD-fixable or LOD-inelastic on its class alone;
//   * the lever's effect -- per frame, both eyes, the builder records, the
//     parts tested (a record that loses the eye at EDVR's scale never
//     reaches the builder, so its parts leave this count) and the parts
//     passed at EDVR's scale (acting: the engine's own passes, run at
//     s_game x k; observing: those less the shadow's would-drop), beside the
//     caller work; and the eye camera (view A's +0x540) as the view's
//     identity: a step's benefit is what they did across it, on a stable
//     scene only;
//   * the game's LOD scale s_game, from the setter bracket (the value the
//     engine stored on its last call for that context; 1.0 at the slider's
//     maximum detail, 1.5 at its lowest), and the scale the tests ran with,
//     ctx+0x30 as the builder observer reads it (s_game x k while acting);
//   * on foot -- the journal watcher's reading of the game's Status.json
//     (journalOnFootKnown() && journalOnFoot(), Flags2 bit 0), the one signal
//     fix.weapon_stability's deferred frame pacing keys on (native_frame.cpp).
//     The arc measured the cockpit only; with the watcher off it cannot be
//     told, the log says so once, and the governor runs as in the cockpit.
//
// THE WRITE (lodGovernorSetterObserver, the engine's thread, twice a frame):
// only a context the draw-item builder has used (a table of at most 8; a
// ninth stands acting down), only a plausible s_game (0.25..8, else counted
// and left alone), only while acting and k > 1; the page is VirtualQuery'd
// once before the first write (committed, writable, no guard page) and every
// read and write is SEH-guarded. A fault stands acting down for the process;
// observing goes on. On disable and on shutdown the game's value is written
// back, guarded, to every context still holding EDVR's.
//
// THE SHADOW, on the worker threads, read-only -- the correctness gate and the
// price, the same quantity in both modes:
//   * per part, after each FUN_1442B3FC0 forward, with the engine's own
//     inputs (decomp_42B3FC0.txt): the part's world centre and sphere, the
//     view's camera (+0x540), A = +0x550, B = +0x560, the LOD scale the test
//     ran with s, and the builder's copy of the part's 0x80-byte LOD table
//     (param_1[4]): d = rsqrt(rcp(|c - cam|^2)) with the engine's own
//     approximations, f = A*(d - r)*s + B. The recompute at s must reproduce
//     an engine pass and its nibble; a part where it does not is a
//     disagreement and never shadowed. The price is a part the game's own
//     setting passes and s_game x k fails: observing (s is s_game), a pass
//     that fails at s x k ("would drop"); acting (s is s_game x k), an engine
//     reject whose LOD term fails at s and passes at s_game while its
//     screen-size term and the engine's plane test FUN_1404F4E10 pass
//     ("dropped: the game's setting would have kept it").
//   * per record, at the builder bracket before its forward: the same for
//     FUN_144308B30, the record-level test the traversal ran on the record's
//     centre (+0x240), radius (+0x280) and table (*(rec+0x20)), whose
//     results the builder's dispatch read (rec+0x208 mask, +0x210 nibbles,
//     node+0x6A LOD count; decomp_4320340.txt:64-95). Acting, a record that
//     lost an eye (or every view) at EDVR's scale is not visible there: the
//     traversal keeps no pre-test mask, and a record with no view left never
//     reaches the builder. Its parts are then never tested for that eye, so
//     acting's "dropped" is a lower bound of the price; the fall of "engine
//     passed" per frame from a k = 1 window is the whole of it.
//
// THE POLICY (Policy below, pure; tools\lod_governor_test): auto -- k in
// [1, k_max], quantised to 0.05, decided ONCE A SECOND of wall time on the
// cycles completed in that second with a fresh sample (about 90 at 90 Hz):
// non-overlapping windows, so scattered misses cannot creep k up (a sliding
// 30-sample window read every frame fired on 3% of random misses 19 times a
// minute). A second of fewer than 20 such cycles decides nothing and counts
// neither way. It TRIGGERS when a tenth or more of its cycles took two slots
// as the CPU's (the real slot outcome, not a threshold on the mean: a mean at
// the period hid a third of the frames taking two slots on the 04:23 flight,
// and at half rate the game's own work grows ~1.2 ms -- 05:05). A triggering
// second, in a frame with at least 200 builder records: one step up -- 0.25
// when a quarter or more of its cycles were the CPU's misses or its mean
// caller work ran more than 1.0 ms over (good frames cancel bad ones in the
// mean: 31% misses at a mean near the period took eleven fine steps), else
// 0.05, and 0.05 within 0.25 below a remembered working point; held to
// k_max. Ten triggering seconds in a row below k_max put k at k_max at once
// (a KICK, at most one per 30 s, never in reduced), a bounded trial: if the
// fifth second at k_max still triggers, the pre-kick k comes back at once and
// no kick follows for 60 s; if not, five clean seconds at a time bring k back
// 0.25 to the pre-kick k + 0.25 (unless a second triggers again). Down, 0.05,
// after five clean seconds in a row -- no cycle of any kind taking two slots
// and the mean more than 1.0 ms under the period -- then five more before the
// next. Each down step is a recovery TRIAL: the k before it is the working
// point, and a second that triggers within 10 s restores it at once and
// doubles the clean seconds the next trial waits for (5, 10, 20, 40, 60;
// back to 5 once a step down has held 60 s without a trigger -- counted from
// the step, as 60 clean seconds of waiting are 60 s without one too). Each
// up step's BENEFIT is measured on its whole effect -- the parts tested and
// the parts passed at EDVR's scale a frame and the caller work, over the 30
// frames and samples after it against the 30 before -- and only on a stable
// scene (every frame's records within 5% and parts tested within 10% of its
// side's mean, neither mean rising 2% across the step, the eye camera within
// 2 m; 07:15 judged one during the approach): none when the parts tested and
// passed fall less than 1% (judged at 200 tested a frame or more) and the
// caller work less than 0.2 ms; otherwise not judged, never a reason to hold.
// Two such 0.25 steps in a row, or four 0.05 ones, and the lever is INERT at
// this view (05:53: about 4 of 4,800 parts dropped at k 6): no up step and no
// kick, said once; one step is retried every 30 s or when the parts tested a
// frame move by more than 20%, and a retry with a benefit re-arms; a held
// second starts the kick's run of ten over; down still applies. At k_max with
// five triggering seconds in a row a line gives the outcome -- target
// reached, residual benefit, no observed benefit, or unknown (no fresh
// evidence) -- on the same whole effect, against the k = 1 baseline of the
// same view when there is one (07:15 said residual benefit while the dropped
// count read 4 a frame), with the parts tested and passed and the caller work
// at both ends; nothing acts on it. Without caller work (a timing v3/v4
// runtime) a miss cannot be attributed: nothing triggers, and auto holds.
// Back to 1 when the records have stayed under 150 for 30 frames, and at once
// on foot, held there while it lasts; back aboard, a frame with 200 records
// within 5 s brings the k from before the hold back in one step, else it
// starts from 1 (06:53: re-ramping from 1 in 0.25 steps after re-boarding
// flickered every structure of the settlement at each step). reduced -- k =
// k_max at once from the
// frame the records reach 200, 1 when they have stayed under 150 for 30
// frames or on foot; no ramp and no kick (the ceiling line still speaks).
// k_max is advanced.settlement_detail_max (default 6.0, held to [1, 8]): the
// ceiling in auto, the factor in reduced. It multiplies the game's own scale
// -- 1.0 at the slider's default, 1.5 at its floor -- and the removal levels
// off between an effective s x k of 4.5 and 6 (the LOD note, section 8), so
// 6 reaches that from the default slider. The first acting flight held k
// 2.70-2.75 on s 1.5 (s x k about 4.1): k ~4.1 from s 1.0.
// advanced.settlement_detail_observe = 1 computes and logs all of it and
// never writes.
#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

namespace edvr {

class Config;

namespace lodgov {

// --- The policy's parameters (documented defaults; not keys) --------------
constexpr int kQuantaPerUnit = 20;                 // k moves in steps of 1/20 = 0.05
constexpr int kCoarseQuanta = 5;                   // ... or 5/20 = 0.25 up, far over budget
constexpr uint32_t kSettlementRecords = 200;       // density: builder records a frame
constexpr uint32_t kSettlementBand = 50;           // leave only under 200 - 50 = 150
constexpr double kOverMarginMs = 0.3;              // the summary's "over": work > period + 0.3 ms
constexpr double kMissFactor = 1.5;                // a cycle > 1.5 x the period took two display slots
constexpr double kGpuBoundMarginMs = 0.5;          // ... GPU-bound with its render time >= period - 0.5 ms
constexpr double kUnexplainedMarginMs = 0.3;       // ... else unexplained with the caller work < period - 0.3 ms
constexpr double kUnderMarginMs = 1.0;             // headroom: a window's mean caller work < period - 1.0 ms
constexpr uint32_t kSampleWindow = 30;             // the ring of the latest samples (the lines, a step's before)
constexpr uint64_t kFreshMs = 2000;                // evidence: a new sample captured within 2 s, else it expires
constexpr uint64_t kWindowMs = 1000;               // a decision: once a second of wall time ...
constexpr uint32_t kWindowMinCycles = 20;          // ... over at least 20 cycles with a fresh sample, or none
constexpr uint32_t kTriggerDivisor = 10;           // a window triggers with >= 1/10 of its cycles the CPU's misses
constexpr uint32_t kCoarseDivisor = 4;             // up 0.25 with >= 1/4 of them the CPU's misses ...
constexpr double kCoarseExcessMs = 1.0;            // ... or the window > 1.0 ms over on average
constexpr uint32_t kConsecutive = 30;              // leaving: 30 consecutive frames under 150 records
constexpr uint32_t kCleanWindows = 5;              // down: 5 windows with headroom in a row ...
constexpr uint32_t kCleanWindowsMax = 60;          // ... doubled after each failed trial, to at most 60
constexpr uint64_t kTrialMs = 10000;               // a trigger within 10 s of a down step fails the trial
constexpr uint64_t kBackoffResetMs = 60000;        // a step down held 60 s without a trigger: the wait is 5 again
constexpr uint32_t kKickWindows = 10;              // kick: 10 triggering windows in a row below k_max ...
constexpr uint64_t kKickIntervalMs = 30000;        // ... at most one per 30 s ...
constexpr uint32_t kKickTrialWindows = 5;          // ... judged on the 5th window at k_max ...
constexpr uint64_t kKickBlockMs = 60000;           // ... failed: the pre-kick k, and no kick for 60 s
constexpr uint32_t kCeilingWindows = 5;            // the lever spent: 5 triggering windows in a row at k_max
constexpr uint32_t kEffectFrames = 30;             // a step's benefit: 30 frames/samples after vs 30 before
constexpr uint32_t kEffectMin = 10;                // ... each side judged on at least 10 ...
constexpr double kStableRecordsShare = 0.05;       // ... on a stable scene: every frame's records within 5% ...
constexpr double kStableTestedShare = 0.10;        // ... and parts tested within 10% of its side's mean ...
constexpr double kSceneGrowShare = 0.02;           // ... neither mean rising > 2% across it (the lever only removes) ...
constexpr double kSameViewM = 2.0;                 // ... and the eye camera within 2 m
constexpr double kBenefitShare = 0.01;             // a benefit: parts tested or passed falling >= 1% ...
constexpr double kBenefitCallerMs = 0.2;           // ... or the caller work falling >= 0.2 ms
constexpr double kJudgeMinTested = 200.0;          // the parts judged at >= 200 tested a frame
constexpr uint32_t kInertUnits = 4;                // inert: steps without benefit, 0.25 = 2 units, 0.05 = 1
constexpr double kRearmShare = 0.20;               // retry a step: the parts tested a frame moved > 20% ...
constexpr uint64_t kRetryMs = 30000;               // ... or 30 s since the hold or the last retry
constexpr uint64_t kAboardMs = 5000;               // back aboard: the k from before on foot if 200 records within 5 s
constexpr float kDefaultMax = 6.0f;                // advanced.settlement_detail_max (100 steps of 0.05)
constexpr float kMaxCeiling = 8.0f;                // ... held to [1, 8]
// The write's plausibility band for the game's own LOD scale: the setter
// stores 1 + (1 - x) with x the settings' LODDistanceScale; anything outside
// this is not a value the game's slider produces and is left alone.
constexpr float kScaleLow = 0.25f, kScaleHigh = 8.0f;
constexpr uint32_t kContexts = 8;                  // builder contexts the write may touch

// The frame boundary's inputs to one policy step.
enum class Work : uint8_t { None, Invalid, Valid };   // no new sample / a bad one / a good one
// Which figure the frame work is, fixed by the runtime's timing version:
// Caller = EdvrNativeTimingFrame::callerWorkMs (version 5); App = the
// producer's applicationMs, the fallback for a version 3 or 4 runtime.
enum class WorkSource : uint8_t { None, Caller, App };
struct FrameSignals {
    uint32_t records = 0;       // builder calls since the last boundary
    bool onFoot = false;        // Status.json says on foot (journalOnFootKnown && journalOnFoot)
    Work work = Work::None;     // Valid only for a NEW sequence captured within 2 s
    WorkSource source = WorkSource::None;   // set with every new sample, valid or not
    bool callerAbsent = false;  // a version 5 frame without valid caller work (Work::Invalid)
    bool expired = false;       // the evidence expired at this boundary (lease lost, stale, epoch changed)
    uint32_t timingVersion = 0; // the new sample's EdvrNativeTimingFrame version
    double workMs = 0;          // the new sample's frame work ms (Work::Valid)
    double periodMs = 0;        // the budget it is held against
    uint64_t nowMs = 0;         // wall time: the policy's clock for its seconds
    double clockMs = -1;        // this boundary's QueryPerformanceCounter time, ms (< 0: none)
    bool gpuValid = false;      // a fresh, valid application-render GPU sample (gpu_frame_timing.h)
    double gpuMs = 0;           // ... its render time, ms
    // The lever's effect, both eyes, this frame: parts tested, parts passed
    // at EDVR's scale (acting: the engine's passes; observing: those less the
    // shadow's would-drop), parts dropped at EDVR's scale.
    uint32_t tested = 0, passed = 0, dropped = 0;
    // The eye camera (view A's +0x540, the part test's own camera): the
    // view's identity -- a step is judged, and a k = 1 baseline compared,
    // only while it stays within 2 m.
    bool camValid = false;
    float cam[3] = {};
};
// Enter: reduced mode's k = k_max at once (the settlement started, or k_max
// rose while in it). Foot: k back to 1 at once, on foot. Kick: k = k_max at
// once, ten triggering windows in a row having not been cleared. Restore: a
// failed trial undone at once -- the working point after a down step, or the
// pre-kick k after a kick (restoredAfterKick() says which).
// Aboard: back from on foot into a settlement, the k in force when the hold
// began, in one step.
enum class Step : uint8_t { None, Up, Down, Reset, Clamp, Enter, Foot, Kick, Restore, Aboard };
// This boundary's cycle: an interval to the previous boundary or none, its
// length, whether it took two display slots, and whose: GPU-bound (the
// application's GPU render at or over the period - 0.5 ms), unexplained (the
// GPU under that or unknown and the caller work under the period - 0.3 ms),
// else the CPU's. The caller work cannot say on its own: it spans Present,
// where a GPU-bound game blocks.
struct Cycle {
    bool measured = false, missed = false, gpuBound = false, unexplained = false;
    double ms = 0;
};
// One decision window: the cycles completed in one second of wall time.
struct WindowResult {
    bool decided = false;      // at least 20 cycles with a fresh sample: it counts, one way or the other
    uint32_t cycles = 0;       // cycles measured on a frame with a fresh valid sample
    uint32_t misses = 0;       // of those, two slots and the CPU's (the trigger's)
    uint32_t gpuBound = 0;     // ... two slots, GPU-bound
    uint32_t unexplained = 0;  // ... two slots, neither: counted, never a reason to step
    uint32_t anyMisses = 0;    // two-slot cycles of any kind, with or without a sample
    double meanExcessMs = 0;   // the cycles' mean caller work over the period
    bool triggered = false;    // decided, >= a tenth of the cycles the CPU's misses, caller work to say so
    bool headroom = false;     // decided, no two-slot cycle at all, the mean more than 1.0 ms under
};
// A step's measured benefit, the whole effect: a record that loses the eye at
// EDVR's scale never reaches the builder, so its parts leave the TESTED
// count, and a part that fails leaves the PASSED one. Judged only on a stable
// scene -- every frame's records within 5% and parts tested within 10% of its
// side's mean, 10 frames or more each side, neither mean rising more than 2%
// across the step, the eye camera within 2 m (07:15: a judgement during the
// approach, the tested count 2,109 -> 4,205 -> 2,477 across windows, was
// meaningless). Benefit: the parts tested or passed falling 1% or more, or
// the caller work 0.2 ms or more; NoBenefit: neither, with the parts (200
// tested a frame or more) and the caller work judged; else NotJudged, never
// a reason to hold.
enum class Effect : uint8_t { NotJudged, Benefit, NoBenefit };
// What a step, or the ceiling against a k = 1 baseline, moved: the parts
// tested and passed at EDVR's scale a frame and the caller work ms, before
// and after (-1: not measured); baseline: against the k = 1 baseline.
struct EffectFigures {
    double tested[2] = {-1, -1}, passed[2] = {-1, -1}, caller[2] = {-1, -1};
    bool baseline = false;
};
// At k_max: what the lever did there.
enum class Outcome : uint8_t { Reached, Residual, NoBenefit, Unknown };

class Policy {
public:
    // k_max, quantised to the step and held to [1, kMaxCeiling]. Lowering it
    // below the current k clamps k at once (Step::Clamp from the next update).
    void configure(float kMax) noexcept;
    // reduced (fixed = true): k = k_max for as long as the settlement lasts,
    // 1 outside it, no ramp, no kick; auto (false): the governed ramp.
    void setFixed(bool fixed) noexcept { fixed_ = fixed; }
    Step update(const FrameSignals& s) noexcept;
    void reset() noexcept;

    int steps() const noexcept { return steps_; }
    int maxSteps() const noexcept { return maxSteps_; }
    float k() const noexcept { return kOf(steps_); }
    float kMax() const noexcept { return kOf(maxSteps_); }
    bool fixed() const noexcept { return fixed_; }
    bool inSettlement() const noexcept { return inSettlement_; }
    // The latest update's cycle.
    const Cycle& cycle() const noexcept { return cycle_; }
    // The ring of the latest 30 valid samples: how many it holds, how many
    // took two slots as the CPU's, GPU-bound and unexplained, their mean
    // caller-work excess over the period (ms; 0 when empty), and the mean GPU
    // render time of those with a GPU sample (-1 when none had one).
    uint32_t samples() const noexcept { return count_; }
    uint32_t misses() const noexcept { return missCount_; }
    uint32_t gpuBoundMisses() const noexcept { return gpuCount_; }
    uint32_t unexplainedMisses() const noexcept { return unexCount_; }
    double meanExcessMs() const noexcept;
    double meanGpuMs() const noexcept;
    // The period the latest valid sample was held against (0 before one).
    double periodMs() const noexcept { return periodMs_; }
    // How many times the evidence expired (or a sample was invalid) since reset.
    uint32_t expiries() const noexcept { return expiries_; }
    // The decision windows: whether this update closed one, the last closed
    // one, how many have closed since reset, and the runs of decided ones --
    // triggering in a row, with headroom in a row, triggering in a row at
    // k_max. An undecided window leaves every run as it was; an expiry
    // restarts them.
    bool windowClosed() const noexcept { return windowClosed_; }
    const WindowResult& lastWindow() const noexcept { return lastWin_; }
    uint32_t windows() const noexcept { return windows_; }
    uint32_t triggerRun() const noexcept { return trigRun_; }
    uint32_t cleanRun() const noexcept { return cleanRun_; }
    bool triggered() const noexcept { return lastWin_.triggered; }
    // At k_max with five triggering windows in a row: the lever is spent.
    bool ceilingMissing() const noexcept { return steps_ == maxSteps_ && ceilRun_ >= kCeilingWindows; }
    // What the lever did at k_max: the target reached (the last window did
    // not trigger), else its whole effect -- residual benefit, none, or
    // unknown (no fresh evidence) -- against the k = 1 baseline of this view
    // when there is one (taken each second at k 1 in the settlement on a
    // stable scene; the same view while the eye camera is within 2 m and
    // neither the records nor the parts tested have risen more than 2%),
    // else the last judged step's. The figures behind it, if asked.
    Outcome ceilingOutcome(EffectFigures* figures = nullptr) const noexcept;
    bool haveBaseline() const noexcept { return base_.valid; }
    // The latest valid sample carried no caller work (a timing v3/v4
    // runtime): a miss cannot be attributed to the CPU, nothing triggers.
    bool holding() const noexcept { return holding_; }
    // The kick: a bounded trial, judged on the fifth window at k_max; after a
    // successful one five clean windows at a time bring k back 0.25 (the last
    // step less) to the pre-kick k + 0.25, unless a window triggers again.
    bool kickTrial() const noexcept { return kickTrial_; }
    bool relaxing() const noexcept { return relaxing_; }
    float preKickK() const noexcept { return kOf(preKickSteps_); }
    float relaxTargetK() const noexcept { return kOf(relaxTarget_); }
    // The working point: the k before the last down step (it held with no
    // miss for the clean windows); a trigger within 10 s of the step restores
    // it and doubles the clean windows the next trial waits for (5 .. 60),
    // back to 5 once a step down has held 60 s without a trigger.
    bool haveGood() const noexcept { return good_ >= 0; }
    float goodK() const noexcept { return kOf(good_ < 0 ? 0 : good_); }
    uint32_t downWait() const noexcept { return downWait_; }
    bool restoredAfterKick() const noexcept { return restoreKick_; }
    // The last up or down: its size in quanta (1 = 0.05, kCoarseQuanta =
    // 0.25), whether k_max cut an up step short, whether an up step was held
    // fine by a working point just above, whether a down step was the kick's
    // relaxation.
    int upQuanta() const noexcept { return upQuanta_; }
    bool upHeld() const noexcept { return upHeld_; }
    bool upNearGood() const noexcept { return upNear_; }
    int downQuanta() const noexcept { return downQuanta_; }
    bool relaxStep() const noexcept { return relaxStep_; }
    // The lever's benefit: the last judged step's effect and its figures;
    // INERT at this view after 4 units of steps without a benefit in a row
    // (0.25 = 2, 0.05 = 1): no up step and no kick, one step retried every
    // 30 s or when the parts tested a frame move by more than 20% (a retry
    // with a benefit re-arms). The run that made it inert: its first before,
    // its last after, its steps. How the up steps were judged since reset.
    // The latest 30 frames' means.
    Effect lastEffect() const noexcept { return lastEffect_; }
    const EffectFigures& lastFigures() const noexcept { return lastFig_; }
    bool inert() const noexcept { return inert_; }
    bool inertStarted() const noexcept { return inertStarted_; }
    bool rearmed() const noexcept { return rearmed_; }
    bool retrying() const noexcept { return retry_; }
    uint32_t inertHolds() const noexcept { return inertHolds_; }
    uint32_t retries() const noexcept { return retries_; }
    uint32_t rearms() const noexcept { return rearms_; }
    const EffectFigures& runFigures() const noexcept { return runFig_; }
    uint32_t runCoarse() const noexcept { return runCoarse_; }
    uint32_t runFine() const noexcept { return runFine_; }
    uint32_t judgedBenefit() const noexcept { return judged_[0]; }
    uint32_t judgedNone() const noexcept { return judged_[1]; }
    uint32_t notJudged() const noexcept { return judged_[2]; }
    double testedMean() const noexcept { return partsMean(0); }
    double passedMean() const noexcept { return partsMean(1); }
    double droppedMean() const noexcept { return partsMean(2); }
    double recordsMean() const noexcept { return partsMean(3); }
    static float kOf(int steps) noexcept { return float(kQuantaPerUnit + steps) / float(kQuantaPerUnit); }

private:
    // One window being gathered.
    struct Gather {
        uint32_t cycles = 0, misses = 0, gpuBound = 0, unexplained = 0, anyMisses = 0;
        double excessSum = 0;
    };
    enum class Opened : uint8_t { Up, Kick, Enter };
    // One side of a measurement: frames with builder work -- their records,
    // parts tested and passed -- summed with their extremes; done() turns
    // the sums into means, and stable() asks for 10 frames or more, every one
    // within 5% (records) and 10% (parts tested) of its mean.
    struct Cohort {
        uint32_t n = 0;
        double records = 0, tested = 0, passed = 0;
        double recordsLo = 0, recordsHi = 0, testedLo = 0, testedHi = 0;
        void add(double r, double t, double p) noexcept;
        void done() noexcept;
        bool stable() const noexcept;
    };
    // The k = 1 baseline of a stable view in the settlement.
    struct Baseline {
        bool valid = false, camValid = false;
        double records = 0, tested = 0, passed = 0, caller = -1;
        float cam[3] = {};
    };
    void emptyRing() noexcept { count_ = missCount_ = gpuCount_ = unexCount_ = 0; }
    void push(double excessMs, bool miss, bool gpuBound, bool unexplained, double gpuMs) noexcept;   // gpuMs < 0: none
    // A window restarts on enabling, on foot and on leaving the settlement,
    // and its runs with it -- and the lever's effect and the working point,
    // the view being new.
    void restart(uint64_t nowMs) noexcept;
    // The evidence expired or a sample was invalid: the ring, the second in
    // progress, the runs, an open step's cohorts and a kick's trial go; k holds.
    void expire(uint64_t nowMs) noexcept;
    void closeWindow(uint64_t nowMs) noexcept;
    double partsMean(int field) const noexcept;       // 0 tested, 1 passed at EDVR's scale, 2 dropped, 3 records
    Cohort ringCohort() const noexcept;               // the latest 30 frames as one side, done
    double callerNow() const noexcept;                // the ring's caller work mean, -1 under 10 samples
    bool sameCam(const float a[3], bool aValid) const noexcept;   // within 2 m of the eye camera now
    void openEffect(Opened kind, bool coarse) noexcept;
    void judgeEffect(uint64_t nowMs) noexcept;

    int steps_ = 0, maxSteps_ = static_cast<int>((kDefaultMax - 1.0f) * kQuantaPerUnit);   // k_max 6.0: 100 steps
    bool fixed_ = false;
    bool inSettlement_ = false, clampPending_ = false;
    uint32_t sparse_ = 0;
    // The latest valid samples, a ring of 30: each one's excess over the
    // period, its GPU render time (-1: no sample), and its cycle's class.
    double excess_[kSampleWindow] = {}, gpuMs_[kSampleWindow] = {};
    bool miss_[kSampleWindow] = {}, gpu_[kSampleWindow] = {}, unex_[kSampleWindow] = {};
    uint32_t next_ = 0, count_ = 0, missCount_ = 0, gpuCount_ = 0, unexCount_ = 0;
    double periodMs_ = 0;
    bool holding_ = false;
    uint32_t expiries_ = 0;
    // The clock: the previous boundary's time, if the next one may measure.
    bool haveClock_ = false;
    double lastClockMs_ = 0;
    Cycle cycle_;
    // The windows.
    bool started_ = false, windowClosed_ = false;
    uint64_t winStartMs_ = 0;
    Gather gather_;
    WindowResult lastWin_;
    uint32_t windows_ = 0, trigRun_ = 0, cleanRun_ = 0, ceilRun_ = 0;
    uint64_t lastTriggerMs_ = 0;
    // The kick, its trial and its relaxation.
    bool kicked_ = false, relaxing_ = false, kickTrial_ = false;
    uint64_t lastKickMs_ = 0, kickBlockedUntilMs_ = 0;
    uint32_t trialWindows_ = 0;
    int preKickSteps_ = 0, relaxTarget_ = 0;
    // The working point and its trials.
    int good_ = -1;
    uint64_t lastDownMs_ = 0;
    uint32_t downWait_ = kCleanWindows;
    bool restoreKick_ = false;
    int upQuanta_ = 0, downQuanta_ = 0;
    bool upHeld_ = false, upNear_ = false, relaxStep_ = false;
    // The lever's benefit: the latest 30 frames with builder work (tested,
    // passed, dropped, records), the eye camera, the open step's sides, the
    // last judgement, the run without benefit, the hold, and the k = 1
    // baseline (kept across on foot: the camera tells the same view).
    double parts_[kEffectFrames][4] = {};
    uint32_t partsNext_ = 0, partsCount_ = 0;
    bool camValid_ = false;
    float cam_[3] = {};
    bool effectOpen_ = false, effectCoarse_ = false, effectRetry_ = false, effectCamValid_ = false;
    Opened effectKind_ = Opened::Up;
    Cohort effectBefore_, effectAfter_;
    double effectBeforeCaller_ = -1, effectCallerSum_ = 0;
    uint32_t effectCallerN_ = 0;
    float effectCam_[3] = {};
    Effect lastEffect_ = Effect::NotJudged;
    EffectFigures lastFig_, runFig_;
    uint32_t judged_[3] = {};   // up steps: with a benefit, without, not judged
    uint32_t inertUnits_ = 0, runCoarse_ = 0, runFine_ = 0;
    Baseline base_;
    bool inert_ = false, inertStarted_ = false, rearmed_ = false, retry_ = false, retryArmed_ = false;
    uint64_t inertSinceMs_ = 0;
    double testedAtHold_ = 0;
    uint32_t inertHolds_ = 0, retries_ = 0, rearms_ = 0;
    // On foot: the k in force when the hold began (0: none pending), and
    // when the hold ended -- a frame with 200 records within 5 s of it
    // restores that k in one step.
    bool onFoot_ = false;
    int footSteps_ = 0;
    uint64_t boardedMs_ = 0;
};

// --- The engine's arithmetic, exactly as its code does it ------------------
// decomp_42B3FC0.txt:47-66 (the part test) and decomp_4308B30.txt:52-66 (the
// record test): the squared distance summed ((dx*dx + dy*dy) + dz*dz), then
// rcpps and rsqrtps -- the SSE approximations, bit for bit the engine's on the
// same CPU -- so d is the engine's d, not sqrt's. No FMA: this build is SSE2
// x64 without /fp:fast, like the game's code at these sites.
inline float engineDistance(const float c[4], const float cam[4]) noexcept {
    const float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2];
    const float dsq = dx * dx + dy * dy + dz * dz;
    return _mm_cvtss_f32(_mm_rsqrt_ps(_mm_rcp_ps(_mm_set_ss(dsq))));
}

// f = A*(d - r)*s + B, the engine's association (3FC0:78-80, 4308B30:67-69).
inline float lodDistance(float A, float d, float r, float s, float B) noexcept {
    return A * (d - r) * s + B;
}

// The part test's first term, as its code computes it (FUN_1442B3FC0 +0x63..
// +0x7F in the build-332841 exe: mulss A,d; addss B; mulss by 1.0
// (DAT_144E2F880); mulss by 0.5 (DAT_144E2F870); cmpless against r): the
// part's sphere spans at least one pixel. A NaN fails, as cmpless does.
inline bool screenSizePasses(float A, float d, float B, float r) noexcept {
    return 0.5f * (1.0f * (A * d + B)) <= r;
}

// A LOD table as the tests read it: eight 16-byte rows, the first float of
// each a threshold t[0..6], the u32 at +0x70 the count. t[7] is the count's
// own bits read as a float, which is what the engine's loop reads when the
// count is 7; a count above 7 would read past the table, so it is refused.
struct LodTable {
    float t[8] = {};
    uint32_t count = 0;
    static LodTable fromBytes(const uint8_t* bytes) noexcept {
        LodTable out;
        for (int i = 0; i < 8; ++i) std::memcpy(&out.t[i], bytes + 0x10 * i, 4);
        std::memcpy(&out.count, bytes + 0x70, 4);
        return out;
    }
    bool inRange() const noexcept { return count <= 7; }
};

// 3FC0:81-109 / 4308B30:70-92: rejected when t0 < f (so a NaN f passes, as
// in the engine); else the nibble is the first i < count with !(t[i+1] < f),
// else the count. Only for inRange() tables.
inline bool lodPick(const LodTable& table, float f, uint32_t* nibble) noexcept {
    if (table.t[0] < f) return false;
    for (uint32_t i = 0; i < table.count; ++i) {
        if (!(table.t[i + 1] < f)) {
            *nibble = i;
            return true;
        }
    }
    *nibble = table.count;
    return true;
}

// The price's histogram: the part's angular radius r/d in degrees, in four
// buckets: < 0.25, 0.25-0.5, 0.5-1, >= 1 (a camera inside the sphere lands in
// the last).
inline uint32_t angleBucket(float r, float d) noexcept {
    if (!(d > r) || !(d > 0.0f)) return 3;
    const float deg = r / d * 57.2957795f;
    return deg < 0.25f ? 0u : deg < 0.5f ? 1u : deg < 1.0f ? 2u : 3u;
}

inline uint32_t floatBits(float f) noexcept { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// One part's inputs, as read after the engine's forward.
struct PartInputs {
    float centre[4] = {}, cam[4] = {};
    float sphere[4] = {};     // the builder's copy of the model's +0x10: [0] the radius (the plane test reads all 16 bytes)
    float A = 0, B = 0;
    float s = 0;              // the LOD scale the test ran with (ctx+0x30, read after the forward)
    float sGame = 0;          // the game's own for that context: s, unless EDVR's value was in force
    LodTable table;
    uint32_t engineLod = 0, bit = 64;
    bool enginePass = false;
};
// What the shadow makes of it.
struct PartOutcome {
    bool mismatch = false;     // the recompute at s disagrees with the engine's pass (never shadowed)
    bool acting = false;       // the test ran at EDVR's value (s != sGame)
    bool drop = false;         // observing: passes at s, fails at s x k ("would drop")
    bool change = false;       // passes at both, another nibble (acting: than the game's)
    bool checkPlanes = false;  // acting, an engine reject: its LOD term fails at s and passes at
                               // sGame, its screen-size term passes; dropped iff the plane test passes
    uint32_t bucket = 0;       // angleBucket, for a drop or a checkPlanes part
};
inline PartOutcome shadowPart(const PartInputs& in, float k) noexcept {
    PartOutcome o;
    o.acting = floatBits(in.s) != floatBits(in.sGame);
    const float r = in.sphere[0];
    if (!in.enginePass) {
        // Observing, an engine reject stays one: k >= 1 only raises f for d >= r.
        if (!o.acting) return o;
        const float d = engineDistance(in.centre, in.cam);
        if (!(in.table.t[0] < lodDistance(in.A, d, r, in.s, in.B))) return o;    // the LOD term passed: terms 1-2 rejected it
        if (in.table.t[0] < lodDistance(in.A, d, r, in.sGame, in.B)) return o;   // the game's setting rejects it too
        if (!screenSizePasses(in.A, d, in.B, r)) return o;                        // under a pixel either way
        o.checkPlanes = true;
        o.bucket = angleBucket(r, d);
        return o;
    }
    const float d = engineDistance(in.centre, in.cam);
    uint32_t n1 = 0, n2 = 0;
    if (!lodPick(in.table, lodDistance(in.A, d, r, in.s, in.B), &n1) || n1 != in.engineLod) {
        o.mismatch = true;
        return o;
    }
    if (o.acting) {
        // Passed at EDVR's scale, so it passes at the game's (s_game < s) for
        // d >= r; a camera inside the sphere (d < r) could only flip with
        // t0 < B, which no eye view (B = 0) has -- counted with the changes.
        if (!lodPick(in.table, lodDistance(in.A, d, r, in.sGame, in.B), &n2) || n2 != n1) o.change = true;
        return o;
    }
    if (k == 1.0f) return o;   // s * 1 is s: nothing can differ
    if (!lodPick(in.table, lodDistance(in.A, d, r, in.s * k, in.B), &n2)) {
        o.drop = true;
        o.bucket = angleBucket(r, d);
    } else if (n2 != n1) {
        o.change = true;
    }
    return o;
}

}  // namespace lodgov

// --- The runtime --------------------------------------------------------------
// fix.settlement_detail: auto (the shipped default, also for an empty value:
// the governed k; acts) | game (off: nothing observed or changed, the relays
// keep their one load) | reduced (k_max in a settlement, 1 outside; acts).
// advanced.settlement_detail_max: k_max. advanced.settlement_detail_observe:
// 1 = compute and log, never write. From the startup and reload sweeps.
void lodGovernorConfigure(Config& cfg);
// Once a frame on the caller thread (vScreenFrameBoundary): the signals, the
// policy step, and the log lines. Returns at once while off. Never writes
// engine memory.
void lodGovernorFrameBoundary();
// FreeLibrary teardown (shutdownVScreenFixes): stops acting, writes the
// game's value back to every context still holding EDVR's, detaches.
void lodGovernorShutdown();
// The hook-side observers (kinematicEvalSetLodGovernorObservers): the builder
// and part observers on the worker threads, the setter observer on the
// engine's thread after FUN_142819D90's forward; noexcept, allocation-free,
// SEH-guarded.
void lodGovernorBuilderObserver(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept;
void lodGovernorPartObserver(uintptr_t items, uintptr_t out, uintptr_t view, bool fromBuilder) noexcept;
void lodGovernorSetterObserver(uintptr_t ctx) noexcept;

}  // namespace edvr
