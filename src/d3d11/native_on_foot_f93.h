// F93b (native on-foot VR): the ADS sight eye lock's input taken from the alignment the measured eye was rendered with (fix.native_on_foot_ads_eye_steady, TEMPORARY A/B),
// and the per-frame eye chain instrument. Journal: the arc doc, F93b.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks use.
//
// The vibration that stayed with the head aim on and off (the F92 flight). The sight eye lock (F70) sets an absolute target, applied shift + measured error, each tick, and
// a critically damped filter follows it. The error is measured on the right eye's rows, which were rendered with the eye alignment the runtime applied two ticks earlier
// (the alignment serial's lag), while "applied" is the shift published now: the target counts the last two ticks' moves twice, a loop with a two-frame delay. In every
// still rifle window of the F92 flight the error's frame-to-frame step had a lag-2 autocorrelation of -0.41 and a lag-1 of about 0 (the signature of an error corrected
// two frames late; white measurement noise would give -0.5 at lag 1 and 0 at lag 2), 0.08-0.09 mm rms a frame along and across the sight line: about 1.3 px of the near
// sight against the world at 33 px/deg, whatever the head aim did. The F89 hold had the same loop (its input carried its own output, applied late) and the same cure: the
// runtime reports, with each pose, the serial of the alignment its seated origin carried (F89); the frame thread keeps the sight lock's part of every alignment it published
// by serial (SightHistory) and, by the rows' frame, the serial reported with that frame's pose (FrameSerials). The target is then the shift the measured eye was rendered with
// plus the error measured on it: the lock's input no longer contains its own output, and the loop is open whatever the delay.
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f93 {

constexpr uint32_t kHistN = 256;
// The sight lock's shift (seated metres) in every eye alignment published, by serial. Serial 0 is the zero alignment before any publication.
struct SightHistory {
    struct Slot {
        uint32_t serial = ~0u;
        double v[3] = {};
    };
    Slot s[kHistN];
    uint32_t newest = 0;
    void record(uint32_t serial, const double v[3]) noexcept {
        Slot& d = s[serial % kHistN];
        d.serial = serial;
        for (int i = 0; i < 3; ++i) d.v[i] = v[i];
        newest = serial;
    }
    struct Found {
        bool reported = false;   // the runtime reported a serial (a word above 0)
        bool matched = false;    // ... and the history still holds it
        double v[3] = {};
        uint32_t lag = 0;        // publications since
    };
    // word: the frame input's applied-alignment word (0: not reported, else the serial plus 1).
    Found find(uint32_t word) const noexcept {
        Found f;
        if (word == 0) return f;
        f.reported = true;
        const uint32_t serial = word - 1u;
        f.lag = newest >= serial ? newest - serial : 0u;
        if (serial == 0) {
            f.matched = true;
            return f;
        }
        const Slot& d = s[serial % kHistN];
        if (d.serial != serial) return f;
        f.matched = true;
        for (int i = 0; i < 3; ++i) f.v[i] = d.v[i];
        return f;
    }
};
// The applied-alignment word the runtime reported with each frame's pose, by the frame (the frame thread's frame index when that pose was the frame's): the rows of a
// frame were rendered with that frame's pose.
constexpr uint32_t kFrameN = 16;
struct FrameSerials {
    struct Slot {
        uint32_t frame = ~0u, word = 0;
    };
    Slot s[kFrameN];
    void record(uint32_t frame, uint32_t word) noexcept {
        Slot& d = s[frame % kFrameN];
        d.frame = frame;
        d.word = word;
    }
    bool find(uint32_t frame, uint32_t* word) const noexcept {
        const Slot& d = s[frame % kFrameN];
        if (d.frame != frame) return false;
        *word = d.word;
        return true;
    }
};
enum Source : int { kMatched = 0, kLegacy = 1, kMissed = 2, kModel = 3 };
inline const char* sourceName(int s) noexcept {
    return s == kMatched ? "matched" : s == kLegacy ? "legacy" : s == kMissed ? "missed" : "model eye";
}
struct Target {
    bool have = false;     // false: hold the last target (the measurement's alignment is not in the history)
    double err[3] = {};    // the error to hand adsEyeStep (it sets target = pos + err), so the absolute target is the shift the eye was rendered with + the measured error
    int source = kLegacy;
    uint32_t lag = 0;
};
// steady: fix.native_on_foot_ads_eye_steady (on: this; off: F70's target, pos + e). solved: the error was measured on the solved rendered eye (the rows; else the F63
// model eye, which is the job camera's and carries no alignment lag of its own: legacy). frameKnown/word: the serial reported with the measured rows' frame. pos: the shift
// published now; e: the measured error (seated metres).
inline Target sightTarget(bool steady, bool haveMeasure, bool solved, bool frameKnown, uint32_t word, const SightHistory& h, const double pos[3], const double e[3]) noexcept {
    Target t;
    if (!haveMeasure) return t;
    if (!steady || !solved || !frameKnown || word == 0) {
        t.have = true;
        t.source = !solved ? kModel : kLegacy;
        for (int i = 0; i < 3; ++i) t.err[i] = e[i];
        return t;
    }
    const SightHistory::Found f = h.find(word);
    t.lag = f.lag;
    if (!f.matched) {
        t.source = kMissed;   // hold: the alignment the eye was rendered with is gone from the history
        return t;
    }
    t.have = true;
    t.source = kMatched;
    for (int i = 0; i < 3; ++i) t.err[i] = f.v[i] + e[i] - pos[i];
    return t;
}

// ---- the eye chain's 1 s line (adsSightTick and the trace; every ADS second, not only the traced ones) ----
struct ChainSecond {
    uint32_t n = 0, matched = 0, legacy = 0, missed = 0, model = 0;
    uint64_t lagSum = 0;
    uint32_t lagMax = 0;
    double shiftSs = 0.0, lockSs = 0.0, headSs = 0.0;   // sums of squares of the per-frame steps (metres): the sight shift, the position lock, the head's translation
    uint32_t steps = 0;
    // the along error's frame-to-frame step and its autocorrelation at lags 1 and 2 (the two-frame loop's signature: lag 2 near -0.4)
    double alongPrev = 0.0, d1 = 0.0, d2 = 0.0;
    uint32_t alongN = 0;
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, sum = 0.0;
    uint32_t acN = 0;
    void addSource(int source, uint32_t lag) noexcept {
        ++n;
        if (source == kMatched) {
            ++matched;
            lagSum += lag;
            if (lag > lagMax) lagMax = lag;
        } else if (source == kLegacy) {
            ++legacy;
        } else if (source == kMissed) {
            ++missed;
        } else {
            ++model;
        }
    }
    void addSteps(double shiftM, double lockM, double headM) noexcept {
        if (!std::isfinite(shiftM) || !std::isfinite(lockM) || !std::isfinite(headM)) return;
        shiftSs += shiftM * shiftM;
        lockSs += lockM * lockM;
        headSs += headM * headM;
        ++steps;
    }
    // along: this frame's along error (metres); fresh: it was measured this frame (else the run restarts)
    void addAlong(bool fresh, double along) noexcept {
        if (!fresh || !std::isfinite(along)) {
            alongN = 0;
            return;
        }
        if (alongN >= 1) {
            const double d0 = along - alongPrev;
            if (alongN >= 3) {
                s0 += d0 * d0;
                s1 += d0 * d1;
                s2 += d0 * d2;
                sum += d0;
                ++acN;
            }
            d2 = d1;
            d1 = d0;
        }
        alongPrev = along;
        ++alongN;
    }
    double rmsMm(double ss) const noexcept { return steps ? std::sqrt(ss / steps) * 1000.0 : 0.0; }
    double lagMean() const noexcept { return matched ? static_cast<double>(lagSum) / matched : 0.0; }
    // the step's autocorrelation at lag k (1 or 2) about its mean; 0 when too few
    double autocorr(int k) const noexcept {
        if (acN < 8) return 0.0;
        const double m = sum / acN, c0 = s0 / acN - m * m, ck = (k == 1 ? s1 : s2) / acN - m * m;
        return c0 > 1e-24 ? ck / c0 : 0.0;
    }
    double alongStepRmsMm() const noexcept { return acN ? std::sqrt(s0 / acN) * 1000.0 : 0.0; }
    void reset() noexcept {
        const double keepPrev = alongPrev, keep1 = d1, keep2 = d2;
        const uint32_t keepN = alongN;
        *this = ChainSecond{};
        alongPrev = keepPrev;   // the step run carries over the second's boundary
        d1 = keep1;
        d2 = keep2;
        alongN = keepN;
    }
};

}  // namespace f93
}  // namespace nofoot
}  // namespace edvr
