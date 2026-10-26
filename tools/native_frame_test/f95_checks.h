// F95a checks (native on-foot VR): the neck reference from the stance's settled height, and the view-motion instrument's pure parts. Header only, run by
// native_frame_test (run95). The figures are the F94a flight's (log edvr_gfx_20261024_135403): standing, the animated neck at 1.543 m within 0.5 cm a second;
// walking with fists, the neck through 1.456-1.527 m every second (the walk cycle, about 2 Hz) under a game camera that does not follow it.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f94.h"
#include "../../src/d3d11/native_on_foot_f95.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace f95test {

namespace f94 = edvr::nofoot::f94;
namespace f95 = edvr::nofoot::f95;

inline bool within(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
constexpr double kDt = 1.0 / 90.0;
constexpr double kH = 0.155;
constexpr double kPi = 3.14159265358979323846;

struct Pose {
    double g = 1.582, n = 1.543;   // the game's camera with the head level, the animated neck (model, m)
    bool still = true;             // the body not walking or airborne
};
// The loop on a plant, as F94a's run94 has it, with the settle window in front of the target (gate on) or F94a's per-frame target (gate off, the control).
// d: the runtime's delay in frames; legacy: the newest published raise taken out instead of the pose's own.
struct Trace {
    std::vector<double> view, raise, viewOver;
    uint32_t settled = 0;
};
template <class PoseAt>
inline Trace runLoop(int d, bool legacy, bool gate, double seconds, PoseAt poseAt) {
    Trace o;
    f94::SeatedHistory hist;
    f94::NeckRaise nr;
    f95::StanceSettle ss;
    std::vector<double> seat(1, 0.0);
    hist.record(0, 0.0);
    double lastPubT = -1.0, pubTarget = 0.0;
    const int n = static_cast<int>(seconds / kDt + 0.5);
    for (int i = 0; i < n; ++i) {
        const double t = i * kDt;
        const Pose p = poseAt(t);
        const uint32_t newest = static_cast<uint32_t>(seat.size() - 1);
        const uint32_t applied = static_cast<int>(newest) - d > 0 ? newest - static_cast<uint32_t>(d) : 0u;
        const double cam = p.g - seat[applied];
        const double sub = legacy ? seat[newest] : hist.find(applied + 1u).seatedY;
        const double e = cam + sub;
        if (gate) {
            f95::SettleIn si;
            si.valid = true;
            si.still = p.still;
            si.tS = t;
            si.neckY = p.n;
            si.camY = e;
            const f95::SettleOut so = ss.step(si);
            if (so.settled) {
                pubTarget = f94::raiseTarget(so.neckY, kH, so.camY);
                lastPubT = t;
                ++o.settled;
            }
        } else {
            pubTarget = f94::raiseTarget(p.n, kH, e);
            lastPubT = t;
        }
        f94::In in;
        in.scope = true;
        in.targetFresh = lastPubT >= 0.0 && t - lastPubT <= 0.25;
        in.targetM = pubTarget;
        in.dt = kDt;
        in.settleS = f95::settleS(0.1);
        const double seatedY = nr.step(in).seatedY;
        seat.push_back(seatedY);
        hist.record(static_cast<uint32_t>(seat.size() - 1), seatedY);
        o.view.push_back(cam);
        o.viewOver.push_back(cam - p.n);
        o.raise.push_back(-seatedY);
    }
    return o;
}
// Standing 2 s, walking 3 s (the neck through 7 cm at 2 Hz about 1.49 m, the game's camera still), standing 2 s.
inline Pose walk(double t) {
    Pose p;
    if (t >= 2.0 && t < 5.0) {
        p.n = 1.4915 + 0.0355 * std::sin(2.0 * kPi * 2.0 * (t - 2.0));
        p.still = false;
    }
    return p;
}
// The same walk below the speed threshold (a slow step the pin calls standing): only the neck's own motion can tell.
inline Pose slowWalk(double t) {
    Pose p = walk(t);
    p.still = true;
    return p;
}
// Standing 2 s, a crouch over 0.4 s (the neck 50 cm down, the camera 40 cm), crouched to 4 s, up again over 0.4 s.
inline Pose crouch(double t) {
    Pose p;
    const double a = t < 2.0 ? 0.0 : t < 2.4 ? (t - 2.0) / 0.4 : t < 4.0 ? 1.0 : t < 4.4 ? 1.0 - (t - 4.0) / 0.4 : 0.0;
    p.n -= 0.50 * a;
    p.g -= 0.40 * a;
    return p;
}
inline double range(const std::vector<double>& v, double from, double to) {
    double lo = 1e9, hi = -1e9;
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) * kDt;
        if (t >= from && t < to) {
            lo = std::fmin(lo, v[i]);
            hi = std::fmax(hi, v[i]);
        }
    }
    return hi - lo;
}
inline double maxStep(const std::vector<double>& v, double from, double to) {
    double m = 0.0;
    for (size_t i = 1; i < v.size(); ++i) {
        const double t = static_cast<double>(i) * kDt;
        if (t >= from && t < to) m = std::fmax(m, std::fabs(v[i] - v[i - 1]));
    }
    return m;
}
inline double maxErr(const std::vector<double>& v, double from, double to, double want) {
    double m = 0.0;
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) * kDt;
        if (t >= from && t < to) m = std::fmax(m, std::fabs(v[i] - want));
    }
    return m;
}

template <class Check>
inline void run95(Check&& check) {
    // ---- the settle time --------------------------------------------------------------------------------------------------------------------------------------------
    check(f95::settleS(0.1) == 0.1 && f95::settleS(0.0) == 0.1 && f95::settleS(0.05) == 0.1 && f95::settleS(0.3) == 0.3 && f95::settleS(std::nan("")) == 0.1,
          "F95a: the raise follows with the hold's settle time (the pilot's 0.1 s), never under 0.1 s, a value that is not a number reads 0.1");

    // ---- the settle window ------------------------------------------------------------------------------------------------------------------------------------------
    {
        // Standing with idle breathing (the neck 2.5 mm either way over 4 s): filling for 0.3 s, then settled, the means within a millimetre.
        f95::StanceSettle ss;
        f95::SettleIn in;
        in.valid = true;
        in.still = true;
        in.camY = 1.582;
        bool fillingFirst = true, settledAfter = true;
        double worst = 0.0;
        for (int i = 0; i < 270; ++i) {
            in.tS = 10.0 + i * kDt;
            in.neckY = 1.543 + 0.0025 * std::sin(2.0 * kPi * (i * kDt) / 4.0);
            const f95::SettleOut o = ss.step(in);
            if (i * kDt < 0.29) fillingFirst = fillingFirst && !o.settled && o.why == f95::kFilling;
            if (i * kDt > 0.31) {
                settledAfter = settledAfter && o.settled;
                if (o.settled) worst = std::fmax(worst, std::fmax(std::fabs(o.neckY - 1.543), std::fabs(o.camY - 1.582)));
            }
        }
        check(fillingFirst && settledAfter && worst <= 0.0026,
              "F95a: standing, breathing: the window fills for 0.3 s, then the stance is settled every call, its means within the breathing's 2.5 mm of the stance's");
        // Walking (the pin's speed over the walk threshold): never settled, the window starts again.
        ss.reset();
        bool neverWalk = true;
        for (int i = 0; i < 180; ++i) {
            in.tS = 20.0 + i * kDt;
            in.neckY = 1.4915 + 0.0355 * std::sin(2.0 * kPi * 2.0 * i * kDt);
            in.still = false;
            const f95::SettleOut o = ss.step(in);
            neverWalk = neverWalk && !o.settled && o.why == f95::kMoving;
        }
        check(neverWalk, "F95a: walking (the AMC over the walk speed): never settled");
        // A step under the speed threshold with the walk cycle's neck: the neck's own motion keeps it unsettled.
        ss.reset();
        bool neverSlow = true;
        uint32_t neckMoving = 0;
        for (int i = 0; i < 180; ++i) {
            in.tS = 30.0 + i * kDt;
            in.neckY = 1.4915 + 0.0355 * std::sin(2.0 * kPi * 2.0 * i * kDt);
            in.still = true;
            const f95::SettleOut o = ss.step(in);
            neverSlow = neverSlow && !o.settled;
            if (o.why == f95::kNeckMoving) ++neckMoving;
        }
        check(neverSlow && neckMoving > 100, "F95a: the walk cycle's neck (7 cm at 2 Hz) under the speed threshold: never settled, the neck's range says why");
        // A landing: the neck dips 4 cm over 0.3 s; unsettled through it and for the window after, settled again at the old height.
        ss.reset();
        int lastUnsettled = -1;
        bool backAt = false;
        for (int i = 0; i < 180; ++i) {
            const double t = i * kDt;
            in.tS = 40.0 + t;
            in.neckY = 1.543 - (t >= 0.6 && t < 0.9 ? 0.04 * std::sin(kPi * (t - 0.6) / 0.3) : 0.0);
            in.camY = 1.582;
            const f95::SettleOut o = ss.step(in);
            if (t > 0.35 && !o.settled) lastUnsettled = i;
            if (t > 1.4) backAt = o.settled && within(o.neckY, 1.543, 1e-9);
        }
        check(lastUnsettled > 0 && lastUnsettled * kDt > 0.9 && lastUnsettled * kDt < 1.25 && backAt,
              "F95a: a landing dip of 4 cm: unsettled from the dip to 0.3 s after it, then settled at the stance's own height");
        // A crouch: settled again in the crouch with its own means.
        ss.reset();
        f95::SettleOut last;
        for (int i = 0; i < 180; ++i) {
            const double t = i * kDt;
            const double a = t < 0.5 ? 0.0 : t < 0.9 ? (t - 0.5) / 0.4 : 1.0;
            in.tS = 50.0 + t;
            in.neckY = 1.543 - 0.50 * a;
            in.camY = 1.582 - 0.40 * a;
            last = ss.step(in);
        }
        check(last.settled && within(last.neckY, 1.043, 1e-9) && within(last.camY, 1.182, 1e-9), "F95a: a crouch settles with the crouch's neck and camera");
        // A gap in the calls (over 0.1 s) and an input that is not a number start the window again.
        in.tS += 0.25;
        f95::SettleOut g = ss.step(in);
        in.neckY = std::nan("");
        in.tS += kDt;
        f95::SettleOut bad = ss.step(in);
        in.neckY = 1.043;
        in.tS += kDt;
        f95::SettleOut after = ss.step(in);
        check(!g.settled && g.why == f95::kFilling && !bad.settled && bad.why == f95::kInvalid && !after.settled && after.why == f95::kFilling,
              "F95a: a gap of 0.25 s and a neck that is not a number each start the window again");
    }

    // ---- the loop with the settle window ----------------------------------------------------------------------------------------------------------------------------
    {
        // The walk: with the gate the raise holds exactly through the walk (the view is the game's camera plus the standing raise), and the eye is 15.5 cm over the
        // neck standing before and after; F94a's per-frame target (the control) moved the raise with the walk cycle.
        const Trace on = runLoop(2, false, true, 7.0, walk), off = runLoop(2, false, false, 7.0, walk);
        check(range(on.raise, 2.0, 5.0) == 0.0 && range(on.view, 2.0, 5.0) == 0.0, "F95a loop: walking 3 s, the raise and the view do not move at all (the walk cycle is not followed)");
        check(maxErr(on.viewOver, 1.0, 2.0, kH) <= 0.0011 && maxErr(on.viewOver, 6.0, 7.0, kH) <= 0.0011, "F95a loop: standing before and after the walk, the eye 15.5 cm over the neck");
        check(range(off.raise, 2.5, 5.0) > 0.03, "F95a loop (control): F94a's per-frame target moved the raise through more than 3 cm with the walk cycle");
        const Trace slow = runLoop(2, false, true, 7.0, slowWalk);
        check(range(slow.raise, 2.0, 5.0) <= 0.0011, "F95a loop: the walk cycle under the speed threshold: the raise holds within a millimetre (the neck's own range gates it)");
        // A crouch: the raise moves only once the crouch has settled, to the crouched stance's height, within 0.3 s + the settle; no step over 1 cm a tick.
        const Trace c = runLoop(2, false, true, 6.0, crouch);
        // (a 10 cm change at the 0.1 s settle moves at most 1.9 cm a tick: Δ omega / e)
        check(range(c.raise, 2.0, 2.4) == 0.0 && maxErr(c.viewOver, 3.0, 4.0, kH) <= 0.0011 && maxErr(c.viewOver, 5.2, 6.0, kH) <= 0.0011 && maxStep(c.raise, 1.0, 6.0) <= 0.02,
              "F95a loop: a crouch: the raise holds through the transition, then puts the eye 15.5 cm over the crouched neck (and back up), at most 2 cm a tick");
        double tAt = -1.0;
        for (size_t i = 0; i < c.viewOver.size(); ++i)
            if (tAt < 0.0 && i * kDt > 2.4 && within(c.viewOver[i], kH, 0.0011)) tAt = i * kDt;
        check(tAt > 2.4 + 0.3 && tAt < 2.4 + 0.3 + 0.3, "F95a loop: the crouched height reached after the window (0.3 s) and within the 0.1 s settle and the frames of delay");
        // A runtime that reports no serial: still settles with the gate at 1, 2 and 3 frames late.
        bool stable = true;
        for (int d = 1; d <= 3; ++d) {
            const Trace l = runLoop(d, true, true, 6.0, crouch);
            stable = stable && maxErr(l.viewOver, 5.3, 6.0, kH) <= 0.0011 && maxErr(l.viewOver, 3.2, 4.0, kH) <= 0.0011;
        }
        check(stable, "F95a loop: a runtime that reports no serial: the raise settles within a millimetre at 1, 2 and 3 frames late");
    }

    // ---- the instrument's pure parts --------------------------------------------------------------------------------------------------------------------------------
    {
        f95::StepStat s;
        s.add(0.001);
        s.add(-0.002);
        s.add(std::nan(""));
        check(s.n == 2 && within(s.rms(), std::sqrt(2.5), 1e-12) && within(s.maxAbs, 2.0, 1e-12) && f95::StepStat{}.rms() == 0.0,
              "F95a: the step statistic in mm (rms, largest), a step that is not a number skipped");
        check(f95::headStill(0.0005, 0.2) && !f95::headStill(0.0012, 0.0) && !f95::headStill(0.0, 0.4), "F95a: the head is still under 1 mm and 0.3 deg between frames");
    }
}

}  // namespace f95test
