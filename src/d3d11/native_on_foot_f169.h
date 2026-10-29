// F169 (native on foot VR): the stance while the aim is pitched, and the pistol sprint's arm swing (TEMPORARY dial). Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the Animate post-hook and the frame thread use. Journal: the arc doc, F169.
//
// ---- the stance while the aim is pitched -------------------------------------------------------------------------------------------------------------------------------------------------
// The flight edvr_gfx_20261028_201339 (build df917db6), looking straight up with the shotgun drawn (20:19:00 to 20:19:05, the aim's pitch held at +85.0 deg, mouse look): the animated
// neck stood at 1.361 to 1.364 m (the minimum and maximum within 3 mm of each other), so the body was not crouching, while the pin's crouched bin took 11, 22, 33 and 33 of about 90 calls
// each second, with w_c (the camera's crouch weight) at 0.54 to 0.65 and the camera's height 34 cm under its standing mean. The game's camera sinks as the look pitches up (F137, F138);
// w_c reads that sink as a crouch. Every consumer of w_c then flips with it: the stance class of the settled raise (F98c: "the standing unarmed raise over the crouch", +10.2 cm, against
// "the unarmed eye of the stance", +27 to +43 cm: the eye they put the view at 1.24 m against 1.693 m), and F68e's neck-to-eye vector (weighted by w_c: 2.8 cm forward against 12.4). The
// seconds that flipped moved the view by the raise 305 to 627 mm with the head still; the seconds before them, at the same pitch and with no crouched call, 134 and 171 mm.
//
// The fix keeps w_c as it is while the aim is level and caps it while the aim is pitched: the crouch is the animated neck's, which holds in the sights and under the look (F163), so w_c
// is at most the posture weight (the animated neck's drop under the standing neck, smoothstep over 33 to 43 cm) for as long as |aim pitch| is at F138's gate, 20 deg or more. The camera's
// sink (-14 to -49 cm at +37 to +70 deg under mouse look, F137) can no longer make a crouch, and a real crouch (the neck 0.46 m or more under the standing one, and lower with the look) still
// does, at once and with no memory: nothing here holds a value. Level, the cap is 1 and w_c is bit for bit what it was.
//
// ---- the pistol sprint's arm swing ---------------------------------------------------------------------------------------------------------------------------------------------------------
// F167 lets go of the pistol's off hand while sprinting, so the left arm is the game's animated one; the right hand is on the first-person pistol and moves with that rig's sprint animation.
// The pilot: the arms swing almost in unison, can they swing in opposition. The left arm's animated pose is read from a ring of recent animated poses, a fraction of the measured stride
// period earlier (experimental.native_on_foot_pistol_sprint_swing_phase, 0 to 1 of a stride, TEMPORARY), and blended in by F167's release weight. The stride's period is measured live from the
// animated left hand's forward swing (against the chest): the time between upward crossings of its running mean. The pose delayed is the clavicle, the upper arm, the forearm and the hand
// (each as its local transform against its parent, so the delayed arm hangs from the torso as it is now); everything under them (the fingers, the twist joints) comes with its owner
// unchanged against it. With no period yet, too little history, or the dial at 0, there is no delay and nothing is touched.
#pragma once

#include "native_on_foot_core.h"
#include "native_on_foot_f138.h"
#include "native_on_foot_f166.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f169 {

// ======================================================== the stance while the aim is pitched ========================================================

constexpr double kPitchedGateDeg = f138::kSettleGateDeg;   // |the aim's pitch| from which the camera's sink may not make a crouch: F138's gate (the sink at 10 to 20 deg is +3.5 cm at most)
constexpr uint32_t kMinSecondFrames = 10;                   // fewer pitched calls than this are not a second of it
constexpr double kCrouchedAt = kPinCrouchWc;                // a weight from here is the crouched class (the pin's own threshold)

// True when the aim is pitched past the gate (the FP root's pitch, nose up positive; a pitch that is not a number is not pitched).
inline bool pitchedPast(double aimPitchDeg) noexcept { return std::isfinite(aimPitchDeg) && std::fabs(aimPitchDeg) >= kPitchedGateDeg; }

// The ceiling on w_c: 1 (none) while the aim is level, the posture weight while it is pitched. A posture weight that is not a number reads 0 (standing).
inline double wcCap(bool pitched, double postureW) noexcept { return pitched ? unit01(postureW) : 1.0; }

// The crouch weight to use: w_c held under the cap. At a cap of 1 it is w_c itself, bit for bit.
inline double wcUsed(double wc, double cap) noexcept { return cap < wc ? cap : wc; }

// One pitched pin call: the weights, the view's step with the head still (m; not a number when unmeasured) and the settled raise target it published (m; not a number: none).
struct Call {
    double wcRaw = 0.0, wcUsed = 0.0, postureW = 0.0;
    double viewStepM = std::nan("");
    double settledTargetM = std::nan("");
};
// The window of pitched calls (the hook adds under the sequence counter; the frame thread takes it once a second).
struct Window {
    uint32_t calls = 0;
    uint32_t sunk = 0;           // w_c read the camera as a crouch (0.5 or more) before the cap
    uint32_t sunkStanding = 0;   // ... with the animated neck standing (posture under 0.5): a sink, not a crouch
    uint32_t vetoed = 0;         // the cap took a crouch away (w_c 0.5 or more, used under it)
    uint32_t crouched = 0;       // used w_c at 0.5 or more
    uint32_t flips = 0;          // the used class changed between two pitched calls with the animated neck standing in both (a crouch the neck did not make): must be 0
    uint32_t rawFlips = 0;       // ... the same for w_c before the cap: what the class would have done
    uint32_t stepN = 0;
    double stepSq = 0.0, stepMax = 0.0;   // the view's step with the head still, m^2 sum and largest (m)
    uint32_t settled = 0;
    double settledMin = 1e9, settledMax = -1e9;   // the settled raise targets published, m
    bool havePrev = false, prevUsedCrouched = false, prevRawCrouched = false, prevStanding = false;
    void clear() noexcept { *this = Window{}; }
    void add(const Call& c) noexcept {
        ++calls;
        const bool raw = c.wcRaw >= kCrouchedAt, used = c.wcUsed >= kCrouchedAt, standing = c.postureW < kCrouchedAt;
        if (raw) {
            ++sunk;
            if (standing) ++sunkStanding;
        }
        if (raw && !used) ++vetoed;
        if (used) ++crouched;
        if (havePrev && standing && prevStanding) {
            if (used != prevUsedCrouched) ++flips;
            if (raw != prevRawCrouched) ++rawFlips;
        }
        havePrev = true;
        prevUsedCrouched = used;
        prevRawCrouched = raw;
        prevStanding = standing;
        if (std::isfinite(c.viewStepM)) {
            ++stepN;
            stepSq += c.viewStepM * c.viewStepM;
            stepMax = std::fabs(c.viewStepM) > stepMax ? std::fabs(c.viewStepM) : stepMax;
        }
        if (std::isfinite(c.settledTargetM)) {
            ++settled;
            settledMin = c.settledTargetM < settledMin ? c.settledTargetM : settledMin;
            settledMax = c.settledTargetM > settledMax ? c.settledTargetM : settledMax;
        }
    }
    void merge(const Window& o) noexcept {
        calls += o.calls; sunk += o.sunk; sunkStanding += o.sunkStanding; vetoed += o.vetoed; crouched += o.crouched; flips += o.flips; rawFlips += o.rawFlips;
        stepN += o.stepN; stepSq += o.stepSq; stepMax = o.stepMax > stepMax ? o.stepMax : stepMax;
        settled += o.settled; settledMin = o.settledMin < settledMin ? o.settledMin : settledMin; settledMax = o.settledMax > settledMax ? o.settledMax : settledMax;
    }
};
// The seconds of a window with a pitched stretch (kMinSecondFrames calls or more) and those among them that flipped the class with the neck standing.
struct Seconds {
    uint32_t pitched = 0, flipped = 0, sunk = 0;
    void fold(const Window& second) noexcept {
        if (second.calls < kMinSecondFrames) return;
        ++pitched;
        if (second.flips) ++flipped;
        if (second.sunkStanding) ++sunk;
    }
};
inline int formatStance(char* buf, size_t n, uint32_t seconds, const Window& w, const Seconds& s) noexcept {
    const double sn = w.stepN ? static_cast<double>(w.stepN) : 1.0;
    return std::snprintf(buf, n,
                         "native on foot pitched stance (F169) %u s: pitched calls (|aim pitch| %.0f deg or more) %u; the camera read as a crouch (w_c 0.5 or more) in %u, with the animated neck "
                         "standing in %u; the cap took the crouch away in %u; used as crouched in %u; the stance flipped %u times with the neck standing (w_c alone would have flipped it %u); "
                         "the view's step with the head still rms %.1f mm max %.1f (%u frames); settled raise targets %u, %+.1f..%+.1f cm; pitched seconds %u, flipped %u, with a sink %u",
                         seconds, kPitchedGateDeg, w.calls, w.sunk, w.sunkStanding, w.vetoed, w.crouched, w.flips, w.rawFlips, 1000.0 * std::sqrt(w.stepSq / sn), 1000.0 * w.stepMax, w.stepN,
                         w.settled, w.settled ? 100.0 * w.settledMin : 0.0, w.settled ? 100.0 * w.settledMax : 0.0, s.pitched, s.flipped, s.sunk);
}

// ======================================================== the pistol sprint's arm swing ========================================================

constexpr uint32_t kChain = 4;                 // the clavicle, the upper arm, the forearm and the hand
constexpr uint32_t kRingCap = 192;             // the ring's entries: 2 s at 90 calls a second
constexpr double kMaxGapSec = 0.12;            // two ring entries further apart than this are not interpolated across
constexpr double kMeanTauSec = 1.0;            // the running mean the swing is taken from
constexpr double kHysteresisM = 0.02;          // the swing's crossing needs this much either side of the mean
constexpr double kMinPeriodSec = 0.30, kMaxPeriodSec = 1.60;   // a stride outside this is not a stride
constexpr double kPeriodAlpha = 0.4;           // the period's smoothing: the weight of a new stride
constexpr double kStaleFactor = 1.5, kStaleExtraSec = 0.2;     // no crossing for this many periods (plus this) and the period is stale
constexpr double kMaxStepSec = 0.10;           // a call after a longer gap steps the delay by this much
constexpr double kDelaySlewSecPerSec = 0.5;    // the applied delay moves toward the wanted one at most this fast
constexpr double kDialMin = 0.0, kDialMax = 1.0;
constexpr double kReleasedFrom = 0.5;          // the measure and its figures count the calls released from this weight (F167's kReleasedHalf)
constexpr double kWarmSec = 0.5;               // the measure's running means settle this long before a call counts
constexpr double kMeasureGapSec = 0.25;        // a gap this long between two calls restarts the measure
constexpr uint32_t kMinFrames = 30;            // the health rule: fewer released frames than this are too few to judge

// The key's value held to its range; a value that is not a number reads 0 (no delay).
inline double clampPhase(double v) noexcept { return !std::isfinite(v) ? 0.0 : v < kDialMin ? kDialMin : v > kDialMax ? kDialMax : v; }

// ---- the plan: which joints are delayed, and which come with them ---------------------------------------------------------------------------------------------------------------

enum Why : uint32_t { kOk = 0, kBadInput = 1, kNotChain = 2, kBadTree = 3 };
inline const char* whyText(uint32_t why) noexcept {
    switch (why) {
        case kOk: return "ok";
        case kBadInput: return "no joints, or a joint out of range";
        case kNotChain: return "the clavicle, upper arm, forearm and hand are not a chain of direct parents";
        case kBadTree: return "a parent out of range or a cycle";
        default: return "?";
    }
}
// The chain parent first (clavicle, upper arm, forearm, hand) with each joint's parent, and for every other joint under the clavicle the slot of its nearest chain ancestor (it comes with that
// joint unchanged against it).
struct Plan {
    uint32_t why = kBadInput;
    uint16_t joints = 0;
    uint16_t joint[kChain] = {};
    uint16_t parent[kChain] = {};
    int8_t parentSlot[kChain] = {};
    uint16_t carriedN = 0;
    uint16_t carried[kPfzMaxJoints] = {};
    uint8_t owner[kPfzMaxJoints] = {};
    bool ok() const noexcept { return why == kOk; }
};
inline Plan buildPlan(const uint16_t* parents, uint32_t joints, uint32_t clav, uint32_t upper, uint32_t fore, uint32_t hand) noexcept {
    Plan p;
    if (!parents || joints == 0 || joints > kPfzMaxJoints || clav >= joints || upper >= joints || fore >= joints || hand >= joints) return p;
    p.joints = static_cast<uint16_t>(joints);
    if (parents[upper] != clav || parents[fore] != upper || parents[hand] != fore || parents[clav] >= joints) {
        p.why = kNotChain;
        return p;
    }
    const uint32_t chain[kChain] = {clav, upper, fore, hand};
    for (uint32_t k = 0; k < kChain; ++k) {
        p.joint[k] = static_cast<uint16_t>(chain[k]);
        p.parent[k] = parents[chain[k]];
        p.parentSlot[k] = k == 0 ? int8_t{-1} : static_cast<int8_t>(k - 1);
    }
    for (uint32_t j = 0; j < joints; ++j) {
        bool isChain = false;
        for (uint32_t k = 0; k < kChain; ++k) isChain = isChain || chain[k] == j;
        if (isChain) continue;
        int slot = -1;
        uint32_t a = parents[j], steps = 0;
        while (a != 0xFFFFu) {
            if (a >= joints || ++steps > joints) {
                p.why = kBadTree;
                return p;
            }
            for (uint32_t k = 0; k < kChain && slot < 0; ++k)
                if (chain[k] == a) slot = static_cast<int>(k);
            if (slot >= 0) break;
            a = parents[a];
        }
        if (slot < 0) continue;
        p.carried[p.carriedN] = static_cast<uint16_t>(j);
        p.owner[p.carriedN] = static_cast<uint8_t>(slot);
        ++p.carriedN;
    }
    p.why = kOk;
    return p;
}

// ---- the animated chain as local records ----------------------------------------------------------------------------------------------------------------------------------------

using Rec = f166::Rec;
struct Chain {
    Rec rec[kChain];
};
// The chain's local records from a pose (the model matrices): each joint against its parent. False when a parent is singular or a local is not a rotation.
inline bool capture(const Plan& pl, const float (*anim)[16], uint32_t count, Chain* out) noexcept {
    if (!pl.ok() || !out || count < pl.joints) return false;
    for (uint32_t k = 0; k < kChain; ++k) {
        const uint32_t j = pl.joint[k], p = pl.parent[k];
        if (j >= count || p >= count) return false;
        float inv[16], local[16];
        if (!pfzAffineInverse(anim[p], inv)) return false;
        pfzMul(anim[j], inv, local);
        if (!f166::recOf(local, &out->rec[k])) return false;
    }
    return true;
}
// The chain at weight w between the records it has (cur) and the ones to take (target): the rotation slerped, the position lerped; w <= 0 is cur, w >= 1 is target.
inline Chain blend(const Chain& cur, const Chain& target, double w) noexcept {
    Chain o;
    for (uint32_t k = 0; k < kChain; ++k) o.rec[k] = f166::blendRec(cur.rec[k], target.rec[k], w);
    return o;
}
// The pose with the chain set to `target` (its records, against each joint's new parent): out holds the new model matrices of the chain and of every joint that comes with it (touched,
// in the order written: the chain parent first, then the carried ones). A joint under a chain joint keeps its animated local against it. False (nothing valid in out) on a singular matrix.
inline bool apply(const Plan& pl, const Chain& target, const float (*anim)[16], uint32_t count, float (*out)[16], uint16_t* touched, uint32_t* touchedN) noexcept {
    if (touchedN) *touchedN = 0;
    if (!pl.ok() || count < pl.joints) return false;
    float delta[kChain][16];
    uint32_t nt = 0;
    for (uint32_t k = 0; k < kChain; ++k) {
        const uint32_t j = pl.joint[k], p = pl.parent[k];
        if (j >= count || p >= count) return false;
        float local[16], inv[16];
        f166::matOf(target.rec[k], local);
        const float* parentNew = pl.parentSlot[k] >= 0 ? out[pl.joint[pl.parentSlot[k]]] : anim[p];
        pfzMul(local, parentNew, out[j]);
        if (!pfzAffineInverse(anim[j], inv)) return false;
        pfzMul(inv, out[j], delta[k]);
        for (int c = 0; c < 16; ++c)
            if (!std::isfinite(out[j][c])) return false;
        if (touched) touched[nt] = static_cast<uint16_t>(j);
        ++nt;
    }
    for (uint32_t i = 0; i < pl.carriedN; ++i) {
        const uint32_t j = pl.carried[i];
        if (j >= count) return false;
        pfzMul(anim[j], delta[pl.owner[i]], out[j]);
        if (touched) touched[nt] = static_cast<uint16_t>(j);
        ++nt;
    }
    if (touchedN) *touchedN = nt;
    return true;
}

// ---- the ring of recent animated poses -------------------------------------------------------------------------------------------------------------------------------------------

struct Entry {
    uint64_t us = 0;
    Chain chain;
};
struct Ring {
    Entry e[kRingCap];
    uint32_t n = 0, next = 0;   // entries held, the slot the next goes into
    void clear() noexcept {
        n = next = 0;
    }
    // A new entry. A time not later than the newest is dropped (false).
    bool push(uint64_t us, const Chain& c) noexcept {
        if (n && us <= newestUs()) return false;
        e[next].us = us;
        e[next].chain = c;
        next = (next + 1) % kRingCap;
        if (n < kRingCap) ++n;
        return true;
    }
    uint64_t newestUs() const noexcept { return n ? e[(next + kRingCap - 1) % kRingCap].us : 0; }
    uint64_t oldestUs() const noexcept { return n ? e[(next + kRingCap - n) % kRingCap].us : 0; }
    double spanSec() const noexcept { return n > 1 ? static_cast<double>(newestUs() - oldestUs()) * 1e-6 : 0.0; }
    const Entry& at(uint32_t i) const noexcept { return e[(next + kRingCap - n + i) % kRingCap]; }   // 0 the oldest
    // The chain at time us: between the two entries that bracket it, by the fraction of the way. False when the ring does not reach back that far, the time is later than the newest entry, or
    // the two entries are further apart than kMaxGapSec.
    bool lookup(uint64_t us, Chain* out) const noexcept {
        if (!out || n == 0 || us > newestUs() || us < oldestUs()) return false;
        uint32_t lo = 0, hi = n - 1;   // the newest entry at or before us
        while (lo < hi) {
            const uint32_t mid = (lo + hi + 1) / 2;
            if (at(mid).us <= us) lo = mid;
            else hi = mid - 1;
        }
        const Entry& a = at(lo);
        if (a.us == us || lo + 1 >= n) {
            *out = a.chain;
            return true;
        }
        const Entry& b = at(lo + 1);
        if (static_cast<double>(b.us - a.us) * 1e-6 > kMaxGapSec) return false;
        const double t = static_cast<double>(us - a.us) / static_cast<double>(b.us - a.us);
        *out = blend(a.chain, b.chain, t);
        return true;
    }
};

// ---- the stride's period, from a swinging signal -----------------------------------------------------------------------------------------------------------------------------------

// The time between upward crossings of the signal's running mean, smoothed. feed() takes (time in us, the signal in m).
struct Stride {
    bool haveMean = false, high = false, low = false, haveNeg = false;
    double mean = 0.0, prevY = 0.0, periodSec = 0.0;
    uint64_t lastUs = 0, prevUs = 0, candUs = 0, crossUs = 0;
    uint32_t strides = 0;
    void reset() noexcept { *this = Stride{}; }
    void feed(uint64_t us, double x) noexcept {
        if (!std::isfinite(x)) return;
        if (haveMean && (us <= lastUs || static_cast<double>(us - lastUs) * 1e-6 > kMeasureGapSec)) reset();
        const double dt = haveMean ? static_cast<double>(us - lastUs) * 1e-6 : 0.0;
        if (!haveMean) {
            mean = x;
            haveMean = true;
        } else {
            mean += (x - mean) * (1.0 - std::exp(-dt / kMeanTauSec));
        }
        const double y = x - mean;
        // an interpolated zero crossing, from a sample at or under the mean to the next one over it
        if (haveNeg && y > 0.0 && prevY <= 0.0) {
            const double f = (0.0 - prevY) / (y - prevY);
            candUs = prevUs + static_cast<uint64_t>(f * static_cast<double>(us - prevUs));
        }
        if (y < -kHysteresisM) {
            low = true;
            high = false;
        } else if (y > kHysteresisM && !high) {
            high = true;
            if (low && candUs) {   // low then high: one upward crossing at candUs
                low = false;
                if (crossUs) {
                    const double period = static_cast<double>(candUs - crossUs) * 1e-6;
                    if (period >= kMinPeriodSec && period <= kMaxPeriodSec) {
                        periodSec = strides == 0 ? period : periodSec + kPeriodAlpha * (period - periodSec);
                        ++strides;
                    } else {
                        strides = 0;
                        periodSec = 0.0;
                    }
                }
                crossUs = candUs;
            }
        }
        if (y <= 0.0) haveNeg = true;
        prevY = y;
        prevUs = us;
        lastUs = us;
    }
    // A period is known and a crossing was seen recently enough.
    bool valid(uint64_t nowUs) const noexcept {
        return strides >= 1 && periodSec > 0.0 && crossUs && nowUs >= crossUs && static_cast<double>(nowUs - crossUs) * 1e-6 <= kStaleFactor * periodSec + kStaleExtraSec;
    }
};

// ---- the delay and the controller -------------------------------------------------------------------------------------------------------------------------------------------------

struct In {
    uint64_t us = 0;
    bool pistolArmed = false;   // an armed pistol (F167's class); anything else resets everything
    bool sprinting = false;     // F167's sprint state
    double release = 0.0;       // F167's release weight
    double phase = 0.0;         // the dial, 0..1 of a stride
    double strideSignal = std::nan("");   // the animated left hand's forward offset against the chest (m)
    Chain cur;                  // the animated chain this call
    bool curValid = false;
};
struct Out {
    bool apply = false;         // set the chain to `target` (the caller writes it)
    Chain target;               // the chain blended by the release weight (valid with apply)
    double weight = 0.0;        // the release weight the blend used
    double delaySec = 0.0;      // the delay applied (valid with apply)
    double wantedSec = 0.0;     // the delay the dial asks of the measured period (0: none)
    bool periodValid = false;
    double periodSec = 0.0;
    bool ringReady = false;     // the ring reaches back as far as the delay asks
};
struct Controller {
    Ring ring;
    Stride stride;
    double delaySec = 0.0;
    uint64_t lastUs = 0;
    void reset() noexcept {
        ring.clear();
        stride.reset();
        delaySec = 0.0;
        lastUs = 0;
    }
    Out step(const In& in) noexcept {
        Out o;
        if (!in.pistolArmed || !in.curValid) {
            reset();
            return o;
        }
        const double dt = lastUs && in.us > lastUs ? (static_cast<double>(in.us - lastUs) * 1e-6 > kMaxStepSec ? kMaxStepSec : static_cast<double>(in.us - lastUs) * 1e-6) : 0.0;
        lastUs = in.us;
        ring.push(in.us, in.cur);
        if (in.sprinting) stride.feed(in.us, in.strideSignal);
        else stride.reset();
        o.periodValid = stride.valid(in.us);
        o.periodSec = o.periodValid ? stride.periodSec : 0.0;
        const double phase = clampPhase(in.phase);
        o.wantedSec = (phase > 0.0 && o.periodValid) ? phase * stride.periodSec : 0.0;
        // the applied delay slides toward the wanted one: a new period or a new dial value is a gradual change of timing, not a jump of the arm
        const double slew = kDelaySlewSecPerSec * dt;
        if (phase <= 0.0) delaySec = 0.0;
        else if (delaySec < o.wantedSec) delaySec = delaySec + slew > o.wantedSec ? o.wantedSec : delaySec + slew;
        else delaySec = delaySec - slew < o.wantedSec ? o.wantedSec : delaySec - slew;
        o.weight = unit01(in.release);
        if (!(phase > 0.0) || !(o.weight > 0.0) || !(delaySec > 0.0)) return o;
        const double back = delaySec * 1e6;
        if (back >= static_cast<double>(in.us)) return o;
        Chain delayed;
        o.ringReady = ring.lookup(in.us - static_cast<uint64_t>(back), &delayed);
        if (!o.ringReady) return o;
        o.target = blend(in.cur, delayed, o.weight);
        o.delaySec = delaySec;
        o.apply = true;
        return o;
    }
};

// What the stage tells the rest of the call (PinCall's part): whether it set the chain, with what weight and delay, and the animated left hand and feet (model axes, against the animated
// chest) the measure reads at the end of the call.
struct CallInfo {
    bool ran = false;        // the stage set the chain this call
    bool measured = false;   // the animated figures below were read
    bool failed = false;     // the stage had a chain to set and could not write it
    double weight = 0.0, delaySec = 0.0, periodSec = 0.0;
    double hand[3] = {}, footL[3] = {}, footR[3] = {};
};

// ---- the measure: what the two arms do in a pistol sprint ------------------------------------------------------------------------------------------------------------------------------

// A pair of mean-removed signals: the sums for their amplitudes and their zero-lag correlation.
struct Pair {
    uint32_t n = 0;
    double saa = 0.0, sbb = 0.0, sab = 0.0;
    void add(double a, double b) noexcept {
        ++n;
        saa += a * a;
        sbb += b * b;
        sab += a * b;
    }
    void merge(const Pair& o) noexcept {
        n += o.n;
        saa += o.saa;
        sbb += o.sbb;
        sab += o.sab;
    }
    // +1 in unison, -1 opposing; 0 without frames or a signal.
    double corr() const noexcept { return n && saa > 1e-18 && sbb > 1e-18 ? sab / std::sqrt(saa * sbb) : 0.0; }
    // The amplitude of a sine with this signal's power (m): sqrt(2) x rms.
    double ampA() const noexcept { return n ? std::sqrt(2.0 * saa / static_cast<double>(n)) : 0.0; }
    double ampB() const noexcept { return n ? std::sqrt(2.0 * sbb / static_cast<double>(n)) : 0.0; }
};
// One call released from kReleasedFrom: the figures in the body's axes (forward, up; m) of the left hand as drawn, the right hand (the FP grip as drawn), the animated left hand, and the
// legs (left foot forward less right foot forward, animated). Not numbers where unmeasured.
struct Sample {
    double dtSec = 0.0;
    double drawnL[2] = {std::nan(""), std::nan("")}, right[2] = {std::nan(""), std::nan("")}, animL[2] = {std::nan(""), std::nan("")};
    double legs = std::nan("");
    double periodSec = 0.0;      // the measured stride (0: none yet)
    double delaySec = 0.0;       // the delay applied this call
    double phase = 0.0;          // the dial
};
struct Counters {
    uint32_t released = 0, measured = 0, delayed = 0, withPeriod = 0;
    double phaseSum = 0.0, periodSum = 0.0, delaySum = 0.0;
    Pair lrF, lrU, arF, arU, legD, legA;   // drawn left | right, forward and up; animated left | right; the left hand's forward | the legs (drawn, animated)
    void merge(const Counters& o) noexcept {
        released += o.released; measured += o.measured; delayed += o.delayed; withPeriod += o.withPeriod;
        phaseSum += o.phaseSum; periodSum += o.periodSum; delaySum += o.delaySum;
        lrF.merge(o.lrF); lrU.merge(o.lrU); arF.merge(o.arF); arU.merge(o.arU); legD.merge(o.legD); legA.merge(o.legA);
    }
};
// The running means the signals are taken from and the warm-up; one writer.
struct Meter {
    static constexpr int kChannels = 7;   // drawn L forward, up; right forward, up; animated L forward, up; legs
    double mean[kChannels] = {};
    bool have[kChannels] = {};
    double warmSec = 0.0;
    void reset() noexcept { *this = Meter{}; }
    // One call. A call with a gap restarts the means; a call inside the warm-up counts as released but not measured.
    void add(const Sample& s, Counters& c) noexcept {
        ++c.released;
        if (!(s.dtSec > 0.0) || s.dtSec > kMeasureGapSec) {
            reset();
            return;
        }
        const double v[kChannels] = {s.drawnL[0], s.drawnL[1], s.right[0], s.right[1], s.animL[0], s.animL[1], s.legs};
        const double k = 1.0 - std::exp(-s.dtSec / kMeanTauSec);
        double y[kChannels];
        bool all = true;
        for (int i = 0; i < kChannels; ++i) {
            if (!std::isfinite(v[i])) {
                all = false;
                y[i] = 0.0;
                continue;
            }
            if (!have[i]) {
                mean[i] = v[i];
                have[i] = true;
            } else {
                mean[i] += (v[i] - mean[i]) * k;
            }
            y[i] = v[i] - mean[i];
        }
        warmSec += s.dtSec;
        if (!all || warmSec < kWarmSec) return;
        ++c.measured;
        c.phaseSum += s.phase;
        if (s.periodSec > 0.0) {
            ++c.withPeriod;
            c.periodSum += s.periodSec;
        }
        if (s.delaySec > 0.0) {
            ++c.delayed;
            c.delaySum += s.delaySec;
        }
        c.lrF.add(y[0], y[2]);
        c.lrU.add(y[1], y[3]);
        c.arF.add(y[4], y[2]);
        c.arU.add(y[5], y[3]);
        c.legD.add(y[0], y[6]);
        c.legA.add(y[4], y[6]);
    }
};
// The seconds of a window with a pistol sprint released (kMinSecondFrames calls or more).
struct SprintSeconds {
    uint32_t sprint = 0;
    void fold(const Counters& second) noexcept {
        if (second.released >= kMinSecondFrames) ++sprint;
    }
};

// The window's line: the item; the calls released from 0.5 (measured among them, with a period, delayed); the stride and where it is read from; the amplitudes (cm) forward and up of the
// left hand as drawn, the right hand (the FP grip as drawn) and the animated left hand; the zero-lag correlations (+1 in unison, -1 opposing) of the drawn left against the right and of the
// animated left against the right, forward and up; the left hand's forward against the legs; the dial's mean and the delay applied. The log's lines end at 1200 bytes; this one runs to about 900.
inline int formatSwing(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c, const SprintSeconds& s) noexcept {
    const double mn = c.measured ? static_cast<double>(c.measured) : 1.0, pn = c.withPeriod ? static_cast<double>(c.withPeriod) : 1.0, dn = c.delayed ? static_cast<double>(c.delayed) : 1.0;
    return std::snprintf(buf, n,
                         "native on foot pistol sprint swing (F169, TEMPORARY) %u s: item %s; calls released (0.5 or more) %u, measured %u, with a stride %u, delayed %u; seconds %u; "
                         "the stride %.2f s (from the animated left hand's forward swing against the chest); amplitude forward/up (cm) left hand as drawn %.1f/%.1f, right hand (the FP grip as drawn) "
                         "%.1f/%.1f, animated left hand %.1f/%.1f; zero-lag correlation (+1 in unison, -1 opposing) drawn left against right forward %+.2f up %+.2f, animated left against right "
                         "forward %+.2f up %+.2f; the left hand's forward against the legs (left foot forward less right foot forward) drawn %+.2f animated %+.2f; the dial "
                         "experimental.native_on_foot_pistol_sprint_swing_phase mean %.2f, the delay applied %.2f s",
                         seconds, item ? item : "none", c.released, c.measured, c.withPeriod, c.delayed, s.sprint, c.periodSum / pn, 100.0 * c.lrF.ampA(), 100.0 * c.lrU.ampA(),
                         100.0 * c.lrF.ampB(), 100.0 * c.lrU.ampB(), 100.0 * c.arF.ampA(), 100.0 * c.arU.ampA(), c.lrF.corr(), c.lrU.corr(), c.arF.corr(), c.arU.corr(), c.legD.corr(),
                         c.legA.corr(), c.phaseSum / mn, c.delaySum / dn);
}

}  // namespace f169
}  // namespace nofoot
}  // namespace edvr
