// F93b checks (native on-foot VR), run93 below: the ADS eye chain on a plant with the F92 flight's timing. The sight lock's steady input (native_on_foot_f93.h) drives the
// real filter (nofoot::adsEyeStep, native_on_foot_core.h); the eye hold (F62b's position lock, now fix.native_on_foot_ads_eye_hold) takes the head's translation out
// through the eye alignment. Header only, run by native_frame_test.
//
// The plant (one axis, metres): the eye is drawn at H(F) + lock(F - 2) + sight(F - 2): the head's own position, and the hold's and the sight lock's shifts as the pose of
// frame F carries them, published two ticks earlier (the F92 flight's alignment serial lag 2). At tick N the sight lock measures the rows of frame N - 1:
// e = D - eye(N - 1) + noise, D the eye point on the sight line. The frame thread records, by the rows' frame, the serial reported with that frame's pose, and the sight
// shift of every serial it published.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"   // adsEyeStep
#include "../../src/d3d11/native_on_foot_f93.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace f93test {

namespace nf = edvr::nofoot;
namespace f93 = edvr::nofoot::f93;

struct Lcg {
    uint64_t s = 0x9E3779B97F4A7C15ull ^ 0x93ull;
    double next() noexcept {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>((s >> 11) & ((1ull << 53) - 1)) / static_cast<double>(1ull << 52) - 1.0;
    }
};
struct Run {
    std::vector<double> eye;     // the drawn eye, metres
    std::vector<double> ideal;   // where it should be: D with the hold (the eye rigid in the character's frame), the head's own position + D without it
    std::vector<double> err;     // the measured error
    std::vector<int> source;
};
struct Opts {
    bool steady = true, hold = false, report = true;
    int forgetFrom = -1;         // from this tick the history no longer holds the serials the measurement names (a missed match)
};
// n ticks; Hf(i) the head's own position; D(i) the eye point; noise(i) the measurement's noise.
template <class HFn, class DFn, class NFn>
inline Run runLock(int n, HFn Hf, DFn D, NFn noise, const Opts& o) {
    const double dt = 1.0 / 90.0;
    double pos[3] = {}, vel[3] = {}, target[3] = {};
    std::vector<double> sightPub(static_cast<size_t>(n), 0.0), lockPub(static_cast<size_t>(n), 0.0);
    f93::SightHistory hist;
    f93::FrameSerials frames;
    Run r;
    const double h0 = Hf(0);
    for (int i = 0; i < n; ++i) {
        const int carried = i - 2;   // the pose of frame i carries the alignment published at tick i - 2 (serial i - 1; serial 0, the zero alignment, before)
        const double lock = carried >= 0 ? lockPub[static_cast<size_t>(carried)] : 0.0, sight = carried >= 0 ? sightPub[static_cast<size_t>(carried)] : 0.0;
        frames.record(static_cast<uint32_t>(i), o.report ? (carried >= 0 ? static_cast<uint32_t>(carried + 2) : 1u) : 0u);
        r.eye.push_back(Hf(i) + lock + sight);
        r.ideal.push_back(o.hold ? D(i) + h0 : D(i) + Hf(i));   // the frame reference: rigid in the character's frame with the hold, moving with the head without it
        if (i >= 1) {   // this tick measures the rows of frame i - 1 against the sight line's eye point (fixed in the character's frame)
            const double e1 = D(i - 1) + h0 - r.eye[static_cast<size_t>(i - 1)] + noise(i);
            const double e[3] = {e1, 0.0, 0.0};
            uint32_t w = 0;
            const bool known = frames.find(static_cast<uint32_t>(i - 1), &w);
            if (o.forgetFrom >= 0 && i >= o.forgetFrom) w = w ? w + 1000u : 0u;
            const f93::Target t = f93::sightTarget(o.steady, true, true, known, w, hist, pos, e);
            nf::adsEyeStep(pos, vel, target, t.err, t.have, true, false, dt);
            r.err.push_back(e1);
            r.source.push_back(t.source);
        }
        lockPub[static_cast<size_t>(i)] = o.hold ? -(Hf(i) - h0) : 0.0;   // F62b's position lock: the head's translation since the press, taken out
        sightPub[static_cast<size_t>(i)] = pos[0];
        hist.record(static_cast<uint32_t>(i + 1), pos);
    }
    return r;
}
inline double stepRms(const std::vector<double>& x, size_t from) {
    double s = 0.0;
    size_t n = 0;
    for (size_t i = from + 1; i < x.size(); ++i, ++n) s += (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
    return n ? std::sqrt(s / n) : 0.0;
}
inline double offStepRms(const Run& r, size_t from) {   // the drawn eye against its ideal, frame to frame
    std::vector<double> d;
    for (size_t i = 0; i < r.eye.size(); ++i) d.push_back(r.eye[i] - r.ideal[i]);
    return stepRms(d, from);
}
inline double stepAutocorr(const std::vector<double>& x, size_t from, int k) {
    std::vector<double> d;
    for (size_t i = from + 1; i < x.size(); ++i) d.push_back(x[i] - x[i - 1]);
    double m = 0.0;
    for (double v : d) m += v;
    m /= d.size();
    double c0 = 0.0, ck = 0.0;
    for (size_t i = 0; i < d.size(); ++i) {
        c0 += (d[i] - m) * (d[i] - m);
        if (i >= static_cast<size_t>(k)) ck += (d[i] - m) * (d[i - static_cast<size_t>(k)] - m);
    }
    return c0 > 0.0 ? ck / c0 : 0.0;
}

template <class Check>
inline void run93(Check&& check) {
    // ---- the F92 flight's still windows: the head's translation tremor, 0.057 mm a frame, and 0.02 mm of measurement noise ----
    {
        std::vector<double> head(3000), nz(3000);
        Lcg rng;
        double h = 0.0;
        for (size_t i = 0; i < head.size(); ++i) {
            h += 0.000057 * 1.7320508 * rng.next();
            head[i] = h;
            nz[i] = 0.00002 * 1.7320508 * rng.next();
        }
        const auto H = [&](int i) { return head[static_cast<size_t>(i)]; };
        const auto D = [](int) { return 0.05; };
        const auto N = [&](int i) { return nz[static_cast<size_t>(i)]; };
        Opts holdLegacy, holdSteady, free, freeLegacy;
        holdLegacy.hold = true;
        holdLegacy.steady = false;
        holdSteady.hold = true;
        freeLegacy.steady = false;
        const Run a = runLock(2900, H, D, N, holdLegacy), b = runLock(2900, H, D, N, holdSteady), c = runLock(2900, H, D, N, free), d = runLock(2900, H, D, N, freeLegacy);
        const double errA = stepRms(a.err, 300) * 1000.0, ac1A = stepAutocorr(a.err, 300, 1), ac2A = stepAutocorr(a.err, 300, 2), ac2B = stepAutocorr(b.err, 300, 2);
        const double offA = offStepRms(a, 300) * 1000.0, offB = offStepRms(b, 300) * 1000.0, offC = offStepRms(c, 300) * 1000.0, offD = offStepRms(d, 300) * 1000.0;
        const double ac2C = stepAutocorr(c.err, 300, 2);
        check(errA > 0.07 && errA < 0.10 && std::fabs(ac1A) < 0.15 && ac2A < -0.35 && ac2B < -0.35 && std::fabs(offA - offB) < 0.01 && offA > 0.07,
              "F93b: the eye hold taken out two frames late reproduces the F92 flight's still windows (the sight error's step 0.08-0.09 mm rms, autocorrelation about 0 at "
              "lag 1 and -0.41 at lag 2) and moves the eye 0.08 mm a frame against where it should be, with either sight lock input (the sight lock is not the cause)");
        check(offC < 0.25 * offA && offD < 0.25 * offA && std::fabs(ac2C) < 0.15,
              "F93b: with the eye hold off the eye follows the head's own translation and the sight lock keeps it on the line: the eye is under a quarter as far off where it "
              "should be frame to frame, and the error's lag-2 signature is gone");
    }
    // ---- a deliberate lean of 2 cm with the hold off: the sight lock brings the eye back onto the line within 0.5 s ----
    {
        const auto H = [](int i) { return i < 50 ? 0.0 : i < 68 ? 0.02 * (i - 50) / 18.0 : 0.02; };   // 2 cm over 0.2 s
        const auto D = [](int) { return 0.05; };
        const auto N = [](int) { return 0.0; };
        Opts free;
        const Run r = runLock(250, H, D, N, free);
        double peak = 0.0;
        bool onLine = true;
        for (size_t i = 50; i < r.eye.size(); ++i) {
            peak = std::max(peak, std::fabs(r.eye[i] - 0.05));                          // the sight line's eye point stays with the character, not the head
            if (i >= 68 + 45) onLine = onLine && std::fabs(r.eye[i] - 0.05) < 0.0005;
        }
        check(onLine && peak > 0.001, "F93b: a deliberate 2 cm lean with the hold off takes the eye off the sight line for a moment and the sight lock brings it back to 0.5 mm "
                                      "within 0.5 s of the lean's end (it does the hold's job at the lean's speed)");
    }
    // ---- the press's entry: a 10 cm move onto the sight line settles without the loop's overshoot ----
    {
        const auto H = [](int) { return 0.0; };
        const auto D = [](int i) { return i < 20 ? 0.0 : 0.10; };
        const auto N = [](int) { return 0.0; };
        Opts legacyO, steadyO;
        legacyO.steady = false;
        const Run legacy = runLock(200, H, D, N, legacyO), steady = runLock(200, H, D, N, steadyO);
        double overL = 0.0, overS = 0.0;
        int settledS = -1;
        for (size_t i = 0; i < steady.eye.size(); ++i) {
            overL = std::max(overL, legacy.eye[i] - 0.10);
            overS = std::max(overS, steady.eye[i] - 0.10);
            if (settledS < 0 && i > 20 && std::fabs(steady.eye[i] - 0.10) < 0.0005) settledS = static_cast<int>(i);
        }
        check(settledS > 20 && settledS < 20 + 45 && overS < 0.0005 && overL > 0.002 && std::fabs(steady.eye.back() - 0.10) < 1e-6,
              "F93b: the entry's 10 cm move onto the sight line settles to 0.5 mm within 0.5 s without overshoot with the steady input (F70's target, the shift published "
              "now plus the error on the eye drawn two frames ago, overshoots it by more than 2 mm)");
    }
    // ---- the fallbacks: no serial reported (an older runtime), a serial the history no longer holds, the key off ----
    {
        const auto H = [](int) { return 0.0; };
        const auto D = [](int) { return 0.05; };
        const auto N = [](int) { return 0.0; };
        Opts noReportO, legacyO, missedO;
        noReportO.report = false;
        legacyO.steady = false;
        missedO.forgetFrom = 150;
        const Run noReport = runLock(200, H, D, N, noReportO), legacy = runLock(200, H, D, N, legacyO), missed = runLock(200, H, D, N, missedO);
        bool same = noReport.eye.size() == legacy.eye.size();
        for (size_t i = 0; same && i < legacy.eye.size(); ++i) same = noReport.eye[i] == legacy.eye[i];
        bool held = true, missedSeen = false;
        for (size_t i = 160; i < missed.eye.size(); ++i) {
            held = held && std::fabs(missed.eye[i] - missed.eye[159]) < 1e-4;
            missedSeen = missedSeen || missed.source[i - 1] == f93::kMissed;
        }
        const double pos[3] = {0.01, 0.0, 0.0}, e[3] = {0.002, 0.0, 0.0};
        f93::SightHistory h;
        const double v[3] = {0.008, 0.0, 0.0};
        h.record(7, v);
        const f93::Target model = f93::sightTarget(true, true, false, true, 8, h, pos, e), on = f93::sightTarget(true, true, true, true, 8, h, pos, e),
                          off = f93::sightTarget(false, true, true, true, 8, h, pos, e), none = f93::sightTarget(true, false, true, true, 8, h, pos, e);
        check(same && held && missedSeen && model.source == f93::kModel && model.err[0] == 0.002 && on.source == f93::kMatched &&
                  std::fabs(on.err[0] - (0.008 + 0.002 - 0.01)) < 1e-15 && off.source == f93::kLegacy && off.err[0] == 0.002 && !none.have,
              "F93b: a runtime that reports no serial gets F70's target exactly, a serial the history no longer holds holds the last target, the model eye (no rows) and the "
              "key off take F70's, and a matched serial makes the target the shift the eye was drawn with plus the error");
    }
    // ---- the eye chain's 1 s sums ----
    {
        f93::ChainSecond c;
        c.addSource(f93::kMatched, 2);
        c.addSource(f93::kMatched, 2);
        c.addSource(f93::kLegacy, 0);
        c.addSource(f93::kMissed, 0);
        c.addSteps(0.0001, 0.0002, 0.0003);
        c.addSteps(0.0001, 0.0002, 0.0003);
        const double seq[] = {0.0, 0.0001, 0.0, -0.0001, 0.0, 0.0001, 0.0, -0.0001, 0.0, 0.0001, 0.0, -0.0001, 0.0, 0.0001, 0.0, -0.0001};
        for (double a : seq) c.addAlong(true, a);
        const bool sums = c.n == 4 && c.matched == 2 && c.legacy == 1 && c.missed == 1 && c.lagMean() == 2.0 && c.lagMax == 2 && std::fabs(c.rmsMm(c.shiftSs) - 0.1) < 1e-9 &&
                          std::fabs(c.rmsMm(c.headSs) - 0.3) < 1e-9;
        const bool ac = c.autocorr(2) < -0.9 && std::fabs(c.autocorr(1)) < 0.2 && c.acN == 13;   // a period of four frames: lag 2 is -1
        c.addAlong(false, 0.0);   // a frame without a measurement restarts the run
        c.reset();
        check(sums && ac && c.n == 0 && c.alongN == 0, "F93b: the eye chain's second counts the target's sources and serial lag, the steps' rms, and the along error's step "
                                                      "autocorrelation (a four-frame period reads -1 at lag 2); a gap restarts the step run");
    }
}

}  // namespace f93test
