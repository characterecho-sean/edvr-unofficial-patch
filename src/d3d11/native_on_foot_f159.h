// F159 (native on-foot VR): the left elbow's stable pole reseeded when it parks the elbow at the shoulder's height, and each seed's birth logged. Header only, no game or D3D11
// state, so native_frame_test runs the very functions the Animate post-hook and the frame thread use.
//
// The left elbow was sometimes seen unnaturally raised on the two-handed long guns F84's body-down pole does not cover (the kinetic AR, the SMGs, the plasma sniper, the
// rocket). Flight edvr_gfx_20261028_113409, dumps 113746344001 and 113817988003 (kinetic AR, hip): the solved elbow sat at the shoulder's height (elbow minus shoulder +0.0 and
// -0.5 cm) against -8 to -17 cm when natural. F98a's stable pole was the source of the swivel on every call. At the same hand-to-shoulder geometry (about 41 to 45 cm forward, 20
// to 24 right, 5 up) the natural events have a swivel of -15 to -31 deg with the chosen elbow equal to the pole's reference; the raised ones have -57 to -64 deg with the chosen
// elbow 2 to 13 deg off the reference. The transported reference points the elbow up, and the nearest feasible swivel parks it on the E.y <= S.y ceiling. A new seed ends it
// (every dump's stall reseeds the pole and the elbow drops at the same reach); the seeds that went wrong were born at the SMG-to-rifle swap and at the first solve on an item.
//
// The reseed: when the preferred elbow (the transported reference placed on the circle) is within 1 cm of the shoulder's height or above it, with the arm bent (the elbow 8 cm or
// more off the shoulder-to-hand axis), for three solves in a row, the seed is rebuilt from the body-down pole. At most one reseed every 0.5 s. The births: each new seed is counted by
// the reason the earlier one was gone, and logged at most once every 0.5 s.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f159 {

constexpr double kCeilingBandM = 0.01;         // the preferred or solved elbow is at the ceiling when it is above the shoulder's height less this
constexpr double kBentArmM = 0.08;             // the elbow's offset from the shoulder-to-hand axis from which the arm is bent (a nearly straight arm has no ceiling to park on)
constexpr uint32_t kHoldSolves = 3;            // solves in a row with the condition before a reseed
constexpr uint64_t kReseedGapMs = 500;         // at most one reseed in this
constexpr uint64_t kSolveGapMs = 100;          // a gap between two solves longer than this breaks the run
constexpr uint64_t kBirthLineGapMs = 500;      // at most one birth line in this
constexpr double kHipDetachBelow = 0.05;       // the body's ADS detach weight under which the call is at the hip

// ---- the decision ---------------------------------------------------------------------------------------------------------------------------------------------------------

// The elbow (height over the shoulder, m) is within 1 cm of the shoulder's height, or above it.
inline bool atCeiling(double overShoulderM) noexcept { return std::isfinite(overShoulderM) && overShoulderM > -kCeilingBandM; }
// The arm is bent: the elbow's offset from the shoulder-to-hand axis (IkTwoBone::h, m) is 8 cm or more.
inline bool bentArm(double elbowOffsetM) noexcept { return std::isfinite(elbowOffsetM) && elbowOffsetM >= kBentArmM; }
// The reseed's condition for one solve: the preferred elbow at the ceiling and the arm bent.
inline bool reseedCondition(double preferredOverShoulderM, double elbowOffsetM) noexcept { return atCeiling(preferredOverShoulderM) && bentArm(elbowOffsetM); }
// The call is at the hip: the body's ADS detach weight is under 0.05.
inline bool atHip(double detachW) noexcept { return !(detachW >= kHipDetachBelow); }

// The gate: the condition must hold for kHoldSolves solves in a row (a gap of over 100 ms between solves breaks the run), and a reseed is at most one every 0.5 s. A reseed or a new
// seed starts the run again. One writer (the Animate post-hook).
class ReseedGate {
public:
    // One solve with the stable pole; true when the pole is to be reseeded now.
    bool step(bool condition, uint64_t nowMs) noexcept {
        if (haveStep_ && nowMs > lastStepMs_ && nowMs - lastStepMs_ > kSolveGapMs) held_ = 0;
        haveStep_ = true;
        lastStepMs_ = nowMs;
        if (!condition) {
            held_ = 0;
            return false;
        }
        if (held_ < kHoldSolves) ++held_;
        if (held_ < kHoldSolves) return false;
        if (haveReseed_ && nowMs >= lastReseedMs_ && nowMs - lastReseedMs_ < kReseedGapMs) return false;
        haveReseed_ = true;
        lastReseedMs_ = nowMs;
        held_ = 0;
        return true;
    }
    void breakRun() noexcept { held_ = 0; }
    uint32_t held() const noexcept { return held_; }

private:
    uint32_t held_ = 0;
    uint64_t lastStepMs_ = 0, lastReseedMs_ = 0;
    bool haveStep_ = false, haveReseed_ = false;
};

// ---- the births -----------------------------------------------------------------------------------------------------------------------------------------------------------

// Why the left arm's seed was gone when the new one was born (the latest event that emptied it), or what the new one is: first (no earlier seed was dropped), the held item changed,
// the plan's generation changed, armsOk false (the FP hands or G unusable), stood down (not armed, the arm IK not writing), the left hand not eligible (supportCup invalid), a
// solve refused, the solve did not commit, and F159's own reseed.
enum Why : uint8_t {
    kWhyFirst = 0,
    kWhyItem,
    kWhyGeneration,
    kWhyArmsOk,
    kWhyStandDown,
    kWhySupportCup,
    kWhyRefused,
    kWhyUncommitted,
    kWhyReseed,
    kWhyCount
};
inline const char* whyName(uint8_t why) noexcept {
    static const char* const kNames[kWhyCount] = {"first", "item", "generation", "armsOk", "standDown", "supportCup", "refused", "uncommitted", "F159"};
    return why < kWhyCount ? kNames[why] : "unknown";
}
inline const char* stanceName(int stance) noexcept { return stance == 0 ? "standing" : stance == 1 ? "crouched" : "unknown"; }

// The cause ikPhysicalPoleBegin's reset has, before it runs: not wanted (armsOk false, else stood down), then the plan's generation, then the item. kWhyFirst: nothing new (the set
// has never begun, or only its active flag was down, which an earlier call recorded).
inline uint8_t beginWhy(bool wanted, bool armsOk, uint32_t setGeneration, uint32_t generation, uint64_t setItem, uint64_t item) noexcept {
    if (!wanted) return armsOk ? kWhyStandDown : kWhyArmsOk;
    if (setGeneration == 0xFFFFFFFFu) return kWhyFirst;
    if (setGeneration != generation) return kWhyGeneration;
    if (setItem != item) return kWhyItem;
    return kWhyFirst;
}

struct Birth {
    uint8_t why = kWhyFirst, since = kWhyFirst;
    uint64_t absentMs = 0;
};
// The left arm's seed: what emptied it, the first cause since the last seed and the latest, and when. One writer.
class PoleTracker {
public:
    void gone(uint8_t why, uint64_t nowMs) noexcept {
        if (why >= kWhyCount) return;
        if (!absent_) {
            absent_ = true;
            since_ = why;
            sinceMs_ = nowMs;
        }
        last_ = why;
    }
    // A seed is born: why the old one was gone (the latest cause), the first cause and how long there was no seed; the tracker starts again.
    Birth born(uint64_t nowMs) noexcept {
        Birth b;
        if (absent_) {
            b.why = last_;
            b.since = since_;
            b.absentMs = nowMs >= sinceMs_ ? nowMs - sinceMs_ : 0;
        }
        absent_ = false;
        last_ = since_ = kWhyFirst;
        sinceMs_ = 0;
        return b;
    }
    bool absent() const noexcept { return absent_; }

private:
    bool absent_ = false;
    uint8_t last_ = kWhyFirst, since_ = kWhyFirst;
    uint64_t sinceMs_ = 0;
};

// One line in 0.5 s: the first passes, the rest are counted and the next to pass says how many were not written.
class LineLimit {
public:
    bool take(uint64_t nowMs, uint32_t* skippedBefore) noexcept {
        if (have_ && nowMs >= lastMs_ && nowMs - lastMs_ < kBirthLineGapMs) {
            if (skipped_ < 0xFFFFFFFFu) ++skipped_;
            return false;
        }
        if (skippedBefore) *skippedBefore = skipped_;
        skipped_ = 0;
        have_ = true;
        lastMs_ = nowMs;
        return true;
    }

private:
    bool have_ = false;
    uint64_t lastMs_ = 0;
    uint32_t skipped_ = 0;
};

// ---- the per-second figures -----------------------------------------------------------------------------------------------------------------------------------------------

// One left-arm solve with the stable pole, as the hook reports it.
struct Sample {
    double solvedOverM = 0.0, fpOverM = 0.0, dispDeg = 0.0;   // the solved elbow's and the FP elbow's height over the shoulder (m); the chosen swivel's distance from the reference (deg)
    bool ceiling = false, hip = true, reseed = false;         // the preferred elbow at the ceiling; at the hip; the pole was reseeded in this solve
    uint8_t birth = 0xFF;                                     // a Why when a seed was born in this solve, else 0xFF
};
struct Counters {
    uint32_t n = 0, hipN = 0, high = 0, hipHigh = 0, ceiling = 0, reseeds = 0, births[kWhyCount] = {};
    double solvedSum = 0.0, solvedMin = 1e9, solvedMax = -1e9, fpSum = 0.0, fpMin = 1e9, fpMax = -1e9, dispSum = 0.0, dispMax = 0.0;
    void add(const Sample& s) noexcept {
        if (!std::isfinite(s.solvedOverM) || !std::isfinite(s.fpOverM) || !std::isfinite(s.dispDeg)) return;
        ++n;
        const bool high1 = atCeiling(s.solvedOverM);
        if (s.hip) ++hipN;
        if (high1) ++high;
        if (high1 && s.hip) ++hipHigh;
        if (s.ceiling) ++ceiling;
        if (s.reseed) ++reseeds;
        if (s.birth < kWhyCount) ++births[s.birth];
        const double sc = s.solvedOverM * 100.0, fc = s.fpOverM * 100.0;
        solvedSum += sc;
        solvedMin = sc < solvedMin ? sc : solvedMin;
        solvedMax = sc > solvedMax ? sc : solvedMax;
        fpSum += fc;
        fpMin = fc < fpMin ? fc : fpMin;
        fpMax = fc > fpMax ? fc : fpMax;
        dispSum += s.dispDeg;
        dispMax = s.dispDeg > dispMax ? s.dispDeg : dispMax;
    }
    void merge(const Counters& o) noexcept {
        n += o.n;
        hipN += o.hipN;
        high += o.high;
        hipHigh += o.hipHigh;
        ceiling += o.ceiling;
        reseeds += o.reseeds;
        for (uint32_t i = 0; i < kWhyCount; ++i) births[i] += o.births[i];
        solvedSum += o.solvedSum;
        solvedMin = o.solvedMin < solvedMin ? o.solvedMin : solvedMin;
        solvedMax = o.solvedMax > solvedMax ? o.solvedMax : solvedMax;
        fpSum += o.fpSum;
        fpMin = o.fpMin < fpMin ? o.fpMin : fpMin;
        fpMax = o.fpMax > fpMax ? o.fpMax : fpMax;
        dispSum += o.dispSum;
        dispMax = o.dispMax > dispMax ? o.dispMax : dispMax;
    }
};

// A second is one with the elbow at the shoulder's height when most of its solves are.
inline bool secondHigh(uint32_t high, uint32_t n) noexcept { return n > 0 && 2u * high > n; }
// The seconds of a window: with the solved elbow within 1 cm of the shoulder's height or above, the same at the hip, and the longest run of such seconds at the hip. The run is
// carried across a window that closes full (carryRun), so three seconds split by the window's edge still count as three.
struct HighSeconds {
    uint32_t any = 0, hip = 0, run = 0, runMax = 0;
    // One folded second (the hook's counters of that second).
    void fold(const Counters& second) noexcept {
        const bool hipHigh = secondHigh(second.hipHigh, second.hipN);
        if (secondHigh(second.high, second.n)) ++any;
        if (hipHigh) {
            ++hip;
            ++run;
        } else {
            run = 0;
        }
        runMax = run > runMax ? run : runMax;
    }
    // The window closes: a fresh one, keeping the run in progress when keepRun.
    HighSeconds next(bool keepRun) const noexcept {
        HighSeconds h;
        if (keepRun) {
            h.run = run;
            h.runMax = run;
        }
        return h;
    }
};

// ---- the lines ------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line (the F98a line carries too much already: the log's lines end at 1200 bytes). The item is the window's; the figures are the window's sums.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c, const HighSeconds& h) noexcept {
    const double cn = c.n ? static_cast<double>(c.n) : 1.0;
    return std::snprintf(buf, n,
                         "native on foot left elbow pole (F159) %u s: item %s; %u solves with the stable pole (%u at the hip): the solved elbow %+.1f cm over the shoulder "
                         "(min %+.1f max %+.1f), within 1 cm of the shoulder's height or above in %u (%u at the hip); the FP elbow %+.1f cm over the FP shoulder (min %+.1f max %+.1f); "
                         "the pole's preferred elbow within 1 cm of the shoulder's height or above in %u; the chosen elbow %.1f deg from the pole's reference (max %.1f); "
                         "reseeds %u; seed births: first %u, item %u, generation %u, armsOk %u, standDown %u, supportCup %u, refused %u, uncommitted %u, F159 %u; "
                         "seconds with the elbow within 1 cm of the shoulder %u, at the hip %u, longest run at the hip %u s",
                         seconds, item ? item : "none", c.n, c.hipN, c.solvedSum / cn, c.n ? c.solvedMin : 0.0, c.n ? c.solvedMax : 0.0, c.high, c.hipHigh, c.fpSum / cn,
                         c.n ? c.fpMin : 0.0, c.n ? c.fpMax : 0.0, c.ceiling, c.dispSum / cn, c.dispMax, c.reseeds, c.births[kWhyFirst], c.births[kWhyItem],
                         c.births[kWhyGeneration], c.births[kWhyArmsOk], c.births[kWhyStandDown], c.births[kWhySupportCup], c.births[kWhyRefused], c.births[kWhyUncommitted],
                         c.births[kWhyReseed], h.any, h.hip, h.runMax);
}

// One seed's birth.
struct BirthLine {
    uint8_t why = kWhyFirst, since = kWhyFirst;
    uint64_t absentMs = 0;
    double baseDeg = 0.0;                                  // the swivel the seed was born at (a reseed: the body-down pole's)
    bool clamped = false, fpPole = false, reseed = false;  // the swivel was moved onto the feasible side; the pole came from the FP elbow (else the legacy hint); a reseed
    double seededOverCm = 0.0, fpOverCm = 0.0;             // the seeded elbow's and the FP elbow's height over the shoulder (cm)
    double handErrCm = 0.0, armedRamp = 0.0;               // the left hand's attach against its grip (cm) and the armed ramp's weight
    int stance = 2;
    bool pitchOk = false;
    double pitchDeg = 0.0;                                 // the head's pitch (deg)
    double wasOverCm = 0.0, offsetCm = 0.0;                // a reseed: the preferred elbow it replaced (cm over the shoulder) and the elbow's offset from the arm's axis (cm)
    uint32_t skipped = 0;                                  // births since the last line that the rate limit kept out
};
inline int formatBirth(char* buf, size_t n, const BirthLine& b) noexcept {
    char pitch[24], cause[96];
    if (b.pitchOk) std::snprintf(pitch, sizeof(pitch), "%+.1f deg", b.pitchDeg);
    else std::snprintf(pitch, sizeof(pitch), "unread");
    if (b.reseed) std::snprintf(cause, sizeof(cause), "reseeded in place");
    else std::snprintf(cause, sizeof(cause), "first cause %s, no seed for %llu ms", whyName(b.since), static_cast<unsigned long long>(b.absentMs));
    int len = std::snprintf(buf, n,
                            "native on foot left elbow seed (F159): born by %s (%s); base swivel %+.1f deg%s, from %s; seeded elbow %+.1f cm over the "
                            "shoulder, the FP elbow %+.1f cm; the left hand %.1f cm off its grip; armed ramp %.2f; stance %s; head pitch %s",
                            whyName(b.why), cause, b.baseDeg, b.clamped ? ", clamped" : "",
                            b.reseed ? "the body-down pole" : b.fpPole ? "the FP elbow's pole" : "the legacy pole", b.seededOverCm, b.fpOverCm, b.handErrCm, b.armedRamp,
                            stanceName(b.stance), pitch);
    if (len < 0 || static_cast<size_t>(len) >= n) return len;
    if (b.reseed) {
        const int add = std::snprintf(buf + len, n - static_cast<size_t>(len),
                                      "; the pole it replaced put the elbow %+.1f cm over the shoulder with the arm %.1f cm off its axis", b.wasOverCm, b.offsetCm);
        if (add < 0) return len;
        len += add;
        if (static_cast<size_t>(len) >= n) return len;
    }
    if (b.skipped) {
        const int add = std::snprintf(buf + len, n - static_cast<size_t>(len), "; %u earlier seed(s) not logged (one line in 0.5 s)", b.skipped);
        if (add > 0) len += add;
    }
    return len;
}

}  // namespace f159
}  // namespace nofoot
}  // namespace edvr
