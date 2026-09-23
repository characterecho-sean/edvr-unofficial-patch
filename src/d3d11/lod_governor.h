#pragma once
// The settlement LOD governor (fix.settlement_detail) -- SHADOW MODE ONLY in
// this build. Every frame it computes a factor k from the engine's own
// signals, recomputes what the engine's LOD tests would decide with the
// render context's LOD scale times k, and counts the difference. It never
// changes a verdict, a draw or a mesh: its observers only read, and nothing
// it computes is written anywhere but its own counters and the log
// (docs/design-settlement-lod-bias-2026-09-22.md, "Shadow governor").
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
//   * the LOD scale s the engine holds, render context +0x30 (= 2 -
//     LODDistanceScale above the slider's floor, FUN_142819D90; 1.5 at its
//     lowest setting), read by the builder observer.
//
// THE SHADOW, on the worker threads, read-only:
//   * per part, after each FUN_1442B3FC0 forward, with the engine's own
//     inputs (decomp_42B3FC0.txt): the part's world centre and radius, the
//     view's camera (+0x540), A = +0x550, B = +0x560, s, and the builder's
//     copy of the part's 0x80-byte LOD table (param_1[4]): d = rsqrt(rcp(
//     |c - cam|^2)) with the engine's own approximations, f = A*(d - r)*s + B
//     and f_k = A*(d - r)*(s*k) + B. A part the engine passed would be
//     dropped when t0 < f_k and would change level when the nibble at f_k
//     differs. The recompute at k = 1 must reproduce the engine's verdict
//     and nibble; a part where it does not is counted as a disagreement and
//     never shadowed.
//   * per record, at the builder bracket before its forward: the same for
//     FUN_144308B30, the record-level test the traversal ran on the record's
//     centre (+0x240), radius (+0x280) and table (*(rec+0x20)), whose
//     results the builder's dispatch read (rec+0x208 mask, +0x210 nibbles,
//     node+0x6A LOD count; decomp_4320340.txt:64-95): per eye, and whether
//     the record would still be dispatched at all.
//
// THE POLICY (Policy below, pure; tools\lod_governor_test): k in [1, k_max]
// in steps of 0.05. One step up while the frame has at least 200 builder
// records AND the frame work has exceeded the period by more than 0.3 ms for
// 30 consecutive samples; one step down while it has been under the period
// minus 1.0 ms for 30; at most one step a second; back to 1 when the records
// have stayed under 150 for 30 frames. k_max is advanced.settlement_detail_max
// (default 4.0, held to [1, 4]); nothing else is a key. k multiplies whatever
// s the game holds, and the game's own slider only reaches s = 1.5 (s = 1.0
// at maximum detail): the LOD note prices removal by s x k (section 8), and
// s x k = 3 from maximum detail takes k = 3, past the old default of 2.0.
#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

namespace edvr {

class Config;

namespace lodgov {

// --- The policy's parameters (documented defaults; not keys) --------------
constexpr int kQuantaPerUnit = 20;                 // k moves in steps of 1/20 = 0.05
constexpr uint32_t kSettlementRecords = 200;       // density: builder records a frame
constexpr uint32_t kSettlementBand = 50;           // leave only under 200 - 50 = 150
constexpr double kOverMarginMs = 0.3;              // rise: work > period + 0.3 ms ...
constexpr double kUnderMarginMs = 1.0;             // fall: work < period - 1.0 ms ...
constexpr uint32_t kConsecutive = 30;              // ... for 30 consecutive samples
constexpr uint64_t kRampIntervalMs = 1000;         // at most one step a second
constexpr float kDefaultMax = 4.0f;                // advanced.settlement_detail_max
constexpr float kMaxCeiling = 4.0f;                // ... held to [1, 4]

// The frame boundary's inputs to one policy step.
enum class Work : uint8_t { None, Invalid, Valid };   // no new sample / a bad one / a good one
// Which figure the frame work is, fixed by the runtime's timing version:
// Caller = EdvrNativeTimingFrame::callerWorkMs (version 5); App = the
// producer's applicationMs, the fallback for a version 3 or 4 runtime.
enum class WorkSource : uint8_t { None, Caller, App };
struct FrameSignals {
    uint32_t records = 0;       // builder calls since the last boundary
    Work work = Work::None;
    WorkSource source = WorkSource::None;   // set with every new sample, valid or not
    bool callerAbsent = false;  // a version 5 frame without valid caller work (Work::Invalid)
    uint32_t timingVersion = 0; // the new sample's EdvrNativeTimingFrame version
    double workMs = 0;          // the new sample's frame work ms (Work::Valid)
    double periodMs = 0;        // the budget it is held against
    uint64_t nowMs = 0;
};
enum class Step : uint8_t { None, Up, Down, Reset, Clamp };

class Policy {
public:
    // k_max, quantised to the step and held to [1, kMaxCeiling]. Lowering it
    // below the current k clamps k at once (Step::Clamp from the next update).
    void configure(float kMax) noexcept;
    Step update(const FrameSignals& s) noexcept;
    void reset() noexcept;

    int steps() const noexcept { return steps_; }
    int maxSteps() const noexcept { return maxSteps_; }
    float k() const noexcept { return kOf(steps_); }
    float kMax() const noexcept { return kOf(maxSteps_); }
    bool inSettlement() const noexcept { return inSettlement_; }
    uint32_t overRun() const noexcept { return over_; }
    uint32_t underRun() const noexcept { return under_; }
    static float kOf(int steps) noexcept { return float(kQuantaPerUnit + steps) / float(kQuantaPerUnit); }

private:
    int steps_ = 0, maxSteps_ = static_cast<int>((kDefaultMax - 1.0f) * kQuantaPerUnit);   // k_max 4.0
    bool inSettlement_ = false, clampPending_ = false;
    uint32_t over_ = 0, under_ = 0, sparse_ = 0;
    uint64_t lastStepMs_ = 0;
    bool stepped_ = false;
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

// The would-drop histogram: the part's angular radius r/d in degrees, in
// four buckets: < 0.25, 0.25-0.5, 0.5-1, >= 1 (a camera inside the sphere
// lands in the last).
inline uint32_t angleBucket(float r, float d) noexcept {
    if (!(d > r) || !(d > 0.0f)) return 3;
    const float deg = r / d * 57.2957795f;
    return deg < 0.25f ? 0u : deg < 0.5f ? 1u : deg < 1.0f ? 2u : 3u;
}

// One part's inputs, as read after the engine's forward.
struct PartInputs {
    float centre[4] = {}, cam[4] = {};
    float radius = 0, A = 0, B = 0, s = 0;
    LodTable table;
    uint32_t engineLod = 0, bit = 64;
    bool enginePass = false;
};
// What the shadow makes of it at k.
struct PartOutcome {
    bool mismatch = false;    // the k = 1 recompute disagrees with the engine (never shadowed)
    bool wouldDrop = false;   // the engine passed it; at k, t0 < f_k
    bool wouldChange = false; // still passed at k, another nibble
    uint32_t bucket = 0;      // angleBucket, for a would-drop part
};
inline PartOutcome shadowPart(const PartInputs& in, float k) noexcept {
    PartOutcome o;
    if (!in.enginePass) return o;   // an engine reject stays one: k >= 1 only raises f for d >= r
    const float d = engineDistance(in.centre, in.cam);
    uint32_t n1 = 0, nk = 0;
    if (!lodPick(in.table, lodDistance(in.A, d, in.radius, in.s, in.B), &n1) || n1 != in.engineLod) {
        o.mismatch = true;
        return o;
    }
    if (k == 1.0f) return o;   // s * 1 is s: nothing can differ
    if (!lodPick(in.table, lodDistance(in.A, d, in.radius, in.s * k, in.B), &nk)) {
        o.wouldDrop = true;
        o.bucket = angleBucket(in.radius, d);
    } else if (nk != n1) {
        o.wouldChange = true;
    }
    return o;
}

}  // namespace lodgov

// --- The runtime --------------------------------------------------------------
// fix.settlement_detail: game (default: off, nothing observed, the relays keep
// their one load) | auto (this build: the shadow governor, never acts) |
// reduced (reserved: behaves as auto in this build, and the log says so).
// advanced.settlement_detail_max: k_max. From the startup and reload sweeps.
void lodGovernorConfigure(Config& cfg);
// Once a frame on the caller thread (vScreenFrameBoundary): the signals, the
// policy step, and the log lines. Returns at once while off.
void lodGovernorFrameBoundary();
// The hook-side observers (kinematicEvalSetLodGovernorObservers), worker
// threads, noexcept, allocation-free, SEH-guarded reads only.
void lodGovernorBuilderObserver(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept;
void lodGovernorPartObserver(uintptr_t items, uintptr_t out, uintptr_t view, bool fromBuilder) noexcept;

}  // namespace edvr
