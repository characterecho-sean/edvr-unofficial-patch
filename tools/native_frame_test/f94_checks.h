// F94a checks (native on-foot VR): the neck as the source of truth for the view's height, the body at its animated height. Header only, run by native_frame_test
// (run94). The figures are the F93 flight's (log edvr_gfx_20261024_124854) and the F92 flight's standing seconds: the upright stances' animated neck 1.543 m with the
// game's camera 1.58 m over the model looking level; the rifle stance's neck 1.30-1.39 m; the pilot's weapon eye offset up 10, forward 9 cm.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f94.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace f94test {

namespace f88 = edvr::nofoot::f88;
namespace f89 = edvr::nofoot::f89;
namespace f94 = edvr::nofoot::f94;

inline bool within(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

constexpr double kDt = 1.0 / 90.0;
constexpr double kH = 0.155;   // the key's default, metres

// The loop on a plant: each tick the runtime draws with the alignment published d ticks before (its serial reported with the pose), the game's camera G and the
// animated neck N come from the stance, the P1 hook takes out of the drawn camera the neck part that pose carried (matched) or the newest published (legacy), the
// hook's target goes to the frame thread's filter, and its seated y is published under the next serial. The trace keeps the drawn view over the neck and the raise.
struct PlantOut {
    std::vector<double> viewOver, raise;
};
struct Stance {
    double g = 1.582, n = 1.543;   // the game's camera with the head level and the animated neck, the model's metres
};
template <class StanceAt>
inline PlantOut runPlant(int d, bool legacy, bool raw, double seconds, StanceAt stanceAt) {
    PlantOut o;
    f94::SeatedHistory hist;
    f94::NeckRaise nr;
    std::vector<double> seat(1, 0.0);   // seated y by serial (serial 0: the zero alignment)
    hist.record(0, 0.0);
    const int n = static_cast<int>(seconds / kDt + 0.5);
    for (int i = 0; i < n; ++i) {
        const Stance s = stanceAt(i * kDt);
        const uint32_t newest = static_cast<uint32_t>(seat.size() - 1);
        const uint32_t applied = static_cast<int>(newest) - d > 0 ? newest - static_cast<uint32_t>(d) : 0u;
        const double cam = s.g - seat[applied];   // seated y -R raises the camera by R
        double sub = 0.0;
        if (legacy) {
            sub = seat[newest];
        } else {
            const f94::SeatedHistory::Found f = hist.find(applied + 1u);
            sub = f.matched ? f.seatedY : 1e9;
        }
        const double e = cam + sub;   // the camera less the raise: S's y image of seated y is -y
        f94::In in;
        in.scope = true;
        in.targetFresh = true;
        in.targetM = f94::raiseTarget(s.n, kH, e);
        in.dt = kDt;
        double seatedY = 0.0;
        if (raw) {
            seatedY = -std::round(std::fmax(-f94::kMaxM, std::fmin(f94::kMaxM, in.targetM)) / f94::kStepM) * f94::kStepM;
        } else {
            seatedY = nr.step(in).seatedY;
        }
        seat.push_back(seatedY);
        hist.record(static_cast<uint32_t>(seat.size() - 1), seatedY);
        o.viewOver.push_back(cam - s.n);
        o.raise.push_back(-seatedY);
    }
    return o;
}
inline Stance standing(double) { return Stance{}; }
// A crouch from 1.5 s: over 0.4 s the neck drops 50 cm and the game's camera 40 cm (the raise has to grow by 10 cm), back up from 3.0 s.
inline Stance crouch(double t) {
    Stance s;
    double a = t < 1.5 ? 0.0 : t < 1.9 ? (t - 1.5) / 0.4 : t < 3.0 ? 1.0 : t < 3.4 ? 1.0 - (t - 3.0) / 0.4 : 0.0;
    s.g -= 0.40 * a;
    s.n -= 0.50 * a;
    return s;
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
inline void run94(Check&& check) {
    // ---- the key ---------------------------------------------------------------------------------------------------------------------------------------------------
    check(f94::eyeHeightClampCm(std::nan("")) == 15.0 && f94::eyeHeightClampCm(-3.0) == 0.0 && f94::eyeHeightClampCm(45.0) == 30.0 && f94::eyeHeightClampCm(11.5) == 11.5 &&
              f94::kEyeHeightDefaultCm == 15.0 && f94::kEyeHeightOwnCm == 15.5,
          "F94a/F98c: the eye height key: default 15.0 cm (the pilot's; the commander's own neck-to-eye height is 15.5), 0..30, a value that is not a number reads the default");
    // ---- the raise's target ----------------------------------------------------------------------------------------------------------------------------------------
    check(within(f94::raiseTarget(1.543, 0.155, 1.582), 0.116, 1e-12) && within(f94::raiseTarget(1.543, 0.04, 1.582), 0.001, 1e-12),
          "F94a: the F93 flight's fists standing (neck 1.543 m, the game's camera 1.582 m level): the view rises 11.6 cm to put the eye 15.5 cm over the neck; at 4 cm "
          "(a stock file's old value) it is the game's own height within a millimetre");

    // ---- the body at its animated height ---------------------------------------------------------------------------------------------------------------------------
    {
        // The Energylink standing (F92, 10:31:49-50): the rest target 1.446 m against the animated neck 1.540 m: the pin lowered the hips 9.4 cm (F93e: the head took it).
        double t[3] = {0.02, 1.446, 0.291};
        const double n0[3] = {0.0, 1.540, 0.30};
        const double x = t[0], z = t[2];
        const double asked = f94::bodyKeepsNeckHeight(t, n0);
        check(within(asked, -0.094, 1e-12) && t[1] == 1.540 && t[0] == x && t[2] == z,
              "F94a: the Energylink standing: the 9.4 cm lowering is asked and not applied: the neck target is the animated neck's height exactly, the horizontal untouched");
        // The rifle hip stance (F92, 10:29:00): the rest target 11.7 cm ABOVE the neck 1.301 m: F93e left that raise; F94a does not raise either.
        double r[3] = {0.05, 1.418, 0.21};
        const double rn[3] = {0.04, 1.301, 0.25};
        const double ra = f94::bodyKeepsNeckHeight(r, rn);
        check(within(ra, 0.117, 1e-12) && r[1] == 1.301 && r[0] == 0.05 && r[2] == 0.21, "F94a: the rifle stance: the 11.7 cm raise is not applied either (the legs stay the animation's)");
        double q[3] = {0.0, 1.5, 0.0};
        const double qn[3] = {0.0, std::nan(""), 0.0};
        check(f94::bodyKeepsNeckHeight(q, qn) == 0.0, "F94a: a neck that is not a number: no figure (the pin's own guards refuse the call)");
    }

    // ---- the filter: in scope, out of scope, the weight, the freeze, the clamp, a stale target --------------------------------------------------------------------
    {
        f94::NeckRaise nr;
        f94::In in;
        f94::Out o = nr.step(in);
        check(o.seatedY == 0.0 && !std::signbit(o.seatedY) && o.appliedM == 0.0 && o.raiseM == 0.0, "F94a: out of the pin's scope with no target: exact positive zeros");
        in.scope = true;
        in.targetFresh = true;
        in.targetM = 0.116;
        in.dt = kDt;
        double last = 0.0, overshoot = 0.0;
        bool monotone = true;
        for (int i = 0; i < 90; ++i) {
            o = nr.step(in);
            monotone = monotone && o.appliedM >= last - 1e-12;
            overshoot = std::fmax(overshoot, o.appliedM - 0.116);
            last = o.appliedM;
        }
        check(within(o.appliedM, 0.116, 0.0011) && monotone && overshoot <= 0.0010001 && within(o.seatedY, -o.appliedM, 0.0),
              "F94a: entering the scope: the raise rises to 11.6 cm within a second (the 0.5 s ramp, 0.2 s settle), monotone, no overshoot; seated y is its negative");
        // F98c: the raise applies whole with and without an item (no weight, no freeze): a settled armed stance's target (here 5 cm lower) is followed through the
        // filter, under 2 cm a tick, and reached within a millimetre in 0.7 s.
        double maxStep = 0.0, prev = o.appliedM;
        double lastMiss = 0.0;   // the last tick the view was not yet at the new height
        for (int i = 0; i < 135; ++i) {
            in.targetM = 0.066;
            o = nr.step(in);
            maxStep = std::fmax(maxStep, std::fabs(o.appliedM - prev));
            prev = o.appliedM;
            if (!within(o.appliedM, 0.066, 0.0011)) lastMiss = (i + 1) * kDt;
        }
        check(maxStep <= 0.012 && lastMiss < 0.7 && within(o.appliedM, 0.066, 0.0011) && o.appliedM == o.raiseM,
              "F98c: a new settled target (a draw's stance, 5 cm lower) is followed whole through the filter: no step over 1.2 cm a tick, within a millimetre in 0.7 s");
        // A stale target: the raise holds where it is.
        in.targetFresh = false;
        in.targetM = 0.4;
        for (int i = 0; i < 45; ++i) o = nr.step(in);
        check(within(o.appliedM, 0.066, 0.0011), "F94a: a target the hook has not renewed: the raise holds");
        // The clamp.
        in.targetFresh = true;
        in.targetM = 0.9;
        for (int i = 0; i < 90; ++i) o = nr.step(in);
        check(o.clamped && within(o.raiseM, f94::kMaxM, 1e-12), "F94a: a target past 50 cm is clamped (and said)");
        // Out of the scope: the ramp takes it to exact zeros in 0.5 s.
        in.scope = false;
        maxStep = 0.0;
        prev = o.appliedM;
        for (int i = 0; i < 50; ++i) {
            o = nr.step(in);
            maxStep = std::fmax(maxStep, std::fabs(o.appliedM - prev));
            prev = o.appliedM;
        }
        check(o.appliedM == 0.0 && o.seatedY == 0.0 && !std::signbit(o.seatedY) && maxStep <= 0.5 / 45.0 + 0.0011,
              "F94a: leaving the scope: the raise ramps out over 0.5 s to exact zeros, no step over the ramp's");
    }

    // ---- the history the P1 hook reads -----------------------------------------------------------------------------------------------------------------------------
    {
        f94::SeatedHistory h;
        h.record(7, -0.116);
        const f94::SeatedHistory::Found a = h.find(8), none = h.find(0), zero = h.find(1), missing = h.find(9);
        check(a.reported && a.matched && within(a.seatedY, -0.116, 1e-7) && !none.reported && !none.matched && zero.matched && zero.seatedY == 0.0 &&
                  missing.reported && !missing.matched,
              "F94a: the neck history: a serial's part found (float precision), word 0 not reported, serial 0 the zero alignment, an unpublished serial not found");
        h.record(7 + f94::SeatedHistory::kN, -0.05);
        const f94::SeatedHistory::Found over = h.find(8), now = h.find(8 + f94::SeatedHistory::kN);
        check(!over.matched && over.reported && now.matched && within(now.seatedY, -0.05, 1e-7), "F94a: an overwritten slot is not found (256 publications later)");
    }

    // ---- the loop on a plant ---------------------------------------------------------------------------------------------------------------------------------------
    {
        // Matched: the target is measured on the game's own camera, so the raise does not depend on the runtime's delay at all.
        const PlantOut m0 = runPlant(0, false, false, 5.0, crouch), m1 = runPlant(1, false, false, 5.0, crouch), m2 = runPlant(2, false, false, 5.0, crouch);
        double diff = 0.0;
        for (size_t i = 0; i < m0.raise.size(); ++i) diff = std::fmax(diff, std::fmax(std::fabs(m0.raise[i] - m1.raise[i]), std::fabs(m0.raise[i] - m2.raise[i])));
        check(diff < 1e-12, "F94a loop: matched by serial, the raise is the same tick for tick with the runtime 0, 1 and 2 frames late (the target never sees its own output)");
        check(maxErr(m2.viewOver, 0.9, 1.5, kH) <= 0.0011 && maxErr(m2.viewOver, 2.5, 3.0, kH) <= 0.0011 && maxErr(m2.viewOver, 4.0, 5.0, kH) <= 0.0011,
              "F94a loop: standing, crouched and standing again the drawn eye sits 15.5 cm over the animated neck within a millimetre (2 frames of delay)");
        check(maxErr(m2.viewOver, 1.5, 2.5, kH) <= 0.035, "F94a loop: through the crouch (the neck 10 cm further down than the camera in 0.4 s) the eye leaves the neck "
                                                          "reference by 3.5 cm at most");
        // Legacy (a runtime that reports no serial): the newest published taken out. The filter keeps the loop stable at 1, 2 and 3 frames.
        bool stable = true;
        for (int d = 1; d <= 3; ++d) {
            const PlantOut l = runPlant(d, true, false, 6.0, crouch);
            stable = stable && maxErr(l.viewOver, 4.5, 6.0, kH) <= 0.0011 && maxErr(l.viewOver, 1.5, 4.5, kH) <= 0.05;
        }
        check(stable, "F94a loop: a runtime that reports no serial (the last published taken out): settles within a millimetre after the crouch at 1, 2 and 3 frames late");
        // The control: the same legacy loop with no filter runs away at 2 frames (the target follows its own output), so the checks above have teeth.
        const PlantOut raw = runPlant(2, true, true, 6.0, crouch);
        check(maxErr(raw.viewOver, 4.5, 6.0, kH) > 0.05, "F94a loop (control): legacy with the raise following its target unfiltered does not settle at 2 frames late");
        const PlantOut st = runPlant(2, false, false, 2.0, standing);
        check(maxErr(st.viewOver, 1.0, 2.0, kH) <= 0.0011, "F94a loop: standing still, the eye 15.5 cm over the neck from 1 s on (the ramp in)");
    }

    // ---- F98c: one eye height with and without an item --------------------------------------------------------------------------------------------------------------
    {
        // The rifle draw: F88's weight ramps over 0.25 s while the stance leans (the animated neck 1.543 -> 1.300 m over 0.4 s, the game's camera 1.582 -> 1.560 m); F97a
        // raises the leaning stance by w times F93's raise (the neck to 15.5 cm under the camera); the raise's target is the eye height over the neck as written. The
        // view: the game's camera plus the raise (the weapon eye offset's up is retired). Unarmed and drawn the eye is 15.5 cm over the neck as the body is written.
        f88::Weight wt;
        f94::NeckRaise nr;
        f94::In in;
        in.scope = true;
        in.targetFresh = true;
        in.dt = kDt;
        double view = 0.0, prevView = 0.0, maxStep = 0.0, unarmedOver = 0.0, armedOver = 0.0;
        for (int i = 0; i < 360; ++i) {
            const double t = i * kDt;
            const bool draw = t >= 2.0;
            const double a = t < 2.0 ? 0.0 : std::fmin(1.0, (t - 2.0) / 0.4);
            const double g = 1.582 - 0.022 * a, n = 1.543 - 0.243 * a;
            const double w = wt.step(draw, kDt);
            const double raise93 = std::fmax((g - kH) - n, 0.0);   // F93's raise of the leaning stance (none upright)
            const double neckWritten = n + w * raise93;
            in.targetM = f94::raiseTarget(neckWritten, kH, g);
            const f94::Out o = nr.step(in);
            view = g - o.seatedY;
            if (i > 0 && t > 1.0) maxStep = std::fmax(maxStep, std::fabs(view - prevView));
            prevView = view;
            if (i == 179) unarmedOver = view - neckWritten;
            armedOver = view - neckWritten;
        }
        check(within(unarmedOver, kH, 0.0011) && within(armedOver, kH, 0.0011),
              "F98c: one eye height: unarmed (fists) and drawn (the rifle, its stance raised upright by F97a) the eye is 15.5 cm over the neck as written");
        check(maxStep <= 0.02, "F98c: drawing (the weight's 0.25 s ramp, the stance leaning under it) moves the view under 2 cm a tick: no jump");
    }

    // ---- the hold never sees the neck term -------------------------------------------------------------------------------------------------------------------------
    {
        // The pose carries an alignment whose y includes the neck term (-0.116) and the hold's own (+0.03): the render head reads lower by it; the input takes the
        // alignment the pose carried out by its serial and is the head's posture alone.
        f89::HoldFeed feed;
        f89::Applied ap;
        ap.reported = true;
        ap.matched = true;
        ap.y = -0.116 + 0.03;
        const double posture = 0.012, drop = -0.004;
        const double renderY = posture - ap.y;
        const f89::HoldFeed::Out fo = feed.step(true, renderY, drop, ap, 0.0);
        check(fo.devValid && within(fo.dev, posture - drop, 1e-12), "F94a: the hold's input with the neck term in the alignment the pose carried is the head's posture alone");
    }
}

}  // namespace f94test
