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
//     the log names which. Held against the display period, 1000 /
//     baseDisplayHz (version 4 and later), else the session's first
//     predicted period;
//   * the display slot -- one QueryPerformanceCounter read a boundary, the
//     interval since the previous boundary this frame's cycle: longer than
//     1.5 x the period, it took two slots (a miss). The first boundary after
//     enabling, after a bad sample, on foot and after leaving the settlement
//     has no interval. A miss whose caller work was under the period - 0.3
//     ms is not the CPU's (the GPU or the compositor owns it): counted
//     apart, never a reason to step;
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
// [1, k_max], quantised to 0.05, decided on the latest 30 valid samples, each
// carrying its caller-work excess over the period and its display slot: a
// ring that must be full before any step, emptied by a bad sample, by
// leaving the settlement and on foot. The trigger: at least 3 of the 30
// cycles took two slots with the CPU at the period (the real slot outcome,
// not a threshold on the mean: a mean at the period hid a third of the
// frames taking two slots on the 04:23 flight, and at half rate the game's
// own work grows ~1.2 ms, so no mean says whether a frame would fit at full
// rate -- 05:05). While it holds, in a frame with at least 200 builder
// records: one step up, at most one a second, 0.25 when the 30's mean excess
// is more than 1.0 ms, else 0.05 (held to k_max). If it has held 3 s below
// k_max -- the steps have not cleared it -- k goes to k_max at once (a KICK,
// held at least 2 s, at most one per 30 s), so consecutive frames fit and
// the runtime returns to full rate. Down, always 0.05, only once no cycle
// has taken two slots for 5 s AND the 30's mean is more than 1.0 ms under
// the period, and then no sooner than 5 s again: quick up, slow down.
// Between, k holds. At k_max with the trigger held 5 s the lever is spent:
// a line says so once a window; nothing acts on it. Back to 1 when the
// records have stayed under 150 for 30 frames, and at once on foot, held
// there while it lasts. reduced -- k = k_max at once from the frame the
// records reach 200, 1 when they have stayed under 150 for 30 frames or on
// foot; no ramp and no kick (the ceiling line still speaks). k_max is
// advanced.settlement_detail_max (default 6.0, held to [1, 8]): the ceiling
// in auto, the factor in reduced. It multiplies the game's own scale -- 1.0
// at the slider's default, 1.5 at its floor -- and the removal levels off
// between an effective s x k of 4.5 and 6 (the LOD note, section 8), so 6
// reaches that from the default slider. The first acting flight held k
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
constexpr double kCpuUnderMarginMs = 0.3;          // ... not the CPU's with its caller work < period - 0.3 ms
constexpr double kUnderMarginMs = 1.0;             // down: the 30's mean caller work < period - 1.0 ms
constexpr uint32_t kSampleWindow = 30;             // the latest 30 valid samples decide a step
constexpr uint32_t kUpMisses = 3;                  // the trigger: at least 3 of them took two slots, the CPU's
constexpr uint32_t kConsecutive = 30;              // leaving: 30 consecutive frames under 150 records
constexpr double kCoarseExcessMs = 1.0;            // up 0.25 while the 30 ran > 1.0 ms over on average
constexpr uint64_t kRampIntervalMs = 1000;         // up: at most one step a second
constexpr uint64_t kCleanMs = 5000;                // down: no two-slot cycle for 5 s ...
constexpr uint64_t kDownIntervalMs = 5000;         // ... and at most one down step per 5 s
constexpr uint64_t kKickAfterMs = 3000;            // kick: the trigger held 3 s below k_max ...
constexpr uint64_t kKickHoldMs = 2000;             // ... k_max held at least 2 s ...
constexpr uint64_t kKickIntervalMs = 30000;        // ... and at most one kick per 30 s
constexpr uint64_t kCeilingMs = 5000;              // the lever spent: at k_max with the trigger for 5 s
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
    Work work = Work::None;
    WorkSource source = WorkSource::None;   // set with every new sample, valid or not
    bool callerAbsent = false;  // a version 5 frame without valid caller work (Work::Invalid)
    uint32_t timingVersion = 0; // the new sample's EdvrNativeTimingFrame version
    double workMs = 0;          // the new sample's frame work ms (Work::Valid)
    double periodMs = 0;        // the budget it is held against
    uint64_t nowMs = 0;         // wall time: the policy's clock for its seconds
    double clockMs = -1;        // this boundary's QueryPerformanceCounter time, ms (< 0: none)
};
// Enter: reduced mode's k = k_max at once (the settlement started, or k_max
// rose while in it). Foot: k back to 1 at once, on foot. Kick: k = k_max at
// once, the trigger having held 3 s below it.
enum class Step : uint8_t { None, Up, Down, Reset, Clamp, Enter, Foot, Kick };
// This boundary's cycle: an interval to the previous boundary or none, its
// length, whether it took two display slots, and whether the caller work was
// under the period - 0.3 ms then (a miss that is not the CPU's).
struct Cycle {
    bool measured = false, missed = false, cpuUnder = false;
    double ms = 0;
};

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
    // The ring: how many valid samples it holds (a step needs 30), how many
    // took two slots with the CPU at the period (the trigger's count) and
    // with it under (not ours), and their mean excess (caller work - period,
    // ms; 0 when empty).
    uint32_t samples() const noexcept { return count_; }
    uint32_t misses() const noexcept { return missCount_; }
    uint32_t cpuUnderMisses() const noexcept { return underCount_; }
    double meanExcessMs() const noexcept;
    // The trigger: a full ring with at least 3 misses of the CPU's; and for
    // how long it has held without a break (0 when it does not hold).
    bool triggered() const noexcept { return triggered_; }
    uint64_t triggerHeldMs(uint64_t nowMs) const noexcept { return triggered_ ? nowMs - triggerSinceMs_ : 0; }
    // At k_max with the trigger held there for 5 s: the lever is spent.
    bool ceilingMissing(uint64_t nowMs) const noexcept;
    // The period the latest valid sample was held against (0 before one).
    double periodMs() const noexcept { return periodMs_; }
    // The last up, down or kick: the trigger's count and the 30's mean excess
    // behind it; for an up step its size by the rule in quanta (1 = 0.05,
    // kCoarseQuanta = 0.25) and whether k_max cut it short; for a down step
    // how long no cycle had taken two slots; for a kick how long the trigger
    // had held.
    uint32_t stepMisses() const noexcept { return stepMisses_; }
    double stepMeanExcessMs() const noexcept { return stepMeanExcessMs_; }
    int upQuanta() const noexcept { return upQuanta_; }
    bool upHeld() const noexcept { return upHeld_; }
    uint64_t stepCleanMs() const noexcept { return stepCleanMs_; }
    uint64_t kickHeldMs() const noexcept { return kickHeldMs_; }
    static float kOf(int steps) noexcept { return float(kQuantaPerUnit + steps) / float(kQuantaPerUnit); }

private:
    void emptyRing() noexcept { count_ = missCount_ = underCount_ = 0; }
    void push(double excessMs, bool miss, bool under) noexcept;
    Step finish(Step step, uint64_t nowMs) noexcept;   // keeps the at-k_max clock, returns step

    int steps_ = 0, maxSteps_ = static_cast<int>((kDefaultMax - 1.0f) * kQuantaPerUnit);   // k_max 6.0: 100 steps
    bool fixed_ = false;
    bool inSettlement_ = false, clampPending_ = false;
    uint32_t sparse_ = 0;
    uint64_t lastStepMs_ = 0;
    bool stepped_ = false;
    // The latest valid samples, a ring of 30: each one's excess over the
    // period, and whether its cycle took two slots with the CPU at the period
    // (miss_) or under it (under_); count_ of them are live.
    double excess_[kSampleWindow] = {};
    bool miss_[kSampleWindow] = {}, under_[kSampleWindow] = {};
    uint32_t next_ = 0, count_ = 0, missCount_ = 0, underCount_ = 0;
    double periodMs_ = 0;
    // The clock: the previous boundary's time, if the next one may measure.
    bool haveClock_ = false;
    double lastClockMs_ = 0;
    Cycle cycle_;
    // The seconds: the trigger's start, the last two-slot cycle of any kind
    // (the first update's time until one), the last down step and kick, and
    // since when k has been at k_max.
    bool triggered_ = false, started_ = false, downed_ = false, kicked_ = false, atMax_ = false;
    uint64_t triggerSinceMs_ = 0, lastMissMs_ = 0, lastDownMs_ = 0, lastKickMs_ = 0, atMaxSinceMs_ = 0;
    uint64_t kickHoldUntilMs_ = 0;
    uint32_t stepMisses_ = 0;
    double stepMeanExcessMs_ = 0;
    int upQuanta_ = 0;
    bool upHeld_ = false;
    uint64_t stepCleanMs_ = 0, kickHeldMs_ = 0;
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
