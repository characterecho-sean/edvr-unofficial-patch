// F165 checks (native on-foot VR), run165 below. Header only, run by native_frame_test.
//
// F165 (native_on_foot_f165.h): the left elbow of every two-handed gun takes the body-down pole while the body is crouched in the sights. The swivel goes from the stable pole's toward
// the body-down pole's by w = (F163's crouched weight) x (the ADS detach weight), along the shorter way round the circle; w = 0 returns the stable angle as it came. The flight
// edvr_gfx_20261028_162638 (build b9e27e57, dumps 163023144001 and 163028557002) is replayed on the real solver (native_on_foot_core.h): the left arm in the sights, the hand on the
// foregrip about 47 cm ahead of the shoulder, 14 cm to its right and level with it, an arm of 0.30 + 0.28 m, and a stable seed that points the elbow 43 deg from the body-down pole
// (the figure F98a measured with the shotgun, 43.1 deg; 39.8 deg with the plasma AR). The animated arm is not in the repository: "off the plane" here is the swivel's distance from the
// body-down pole's plane, not F98a's plane change against the animated hinge.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f159.h"
#include "../../src/d3d11/native_on_foot_f163.h"
#include "../../src/d3d11/native_on_foot_f165.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f165test {

namespace nf = edvr::nofoot;
namespace f165 = edvr::nofoot::f165;
namespace f163 = edvr::nofoot::f163;
namespace f159 = edvr::nofoot::f159;

// The shortest angular distance between two swivels (rad), 0 to pi.
inline double circ(double a, double b) { return std::fabs(nf::ikWrapPi(a - b)); }

template <class Check>
inline void run165(Check&& check) {
    constexpr double kPi = nf::kIkPi;
    constexpr double kDeg = 180.0 / nf::kIkPi;
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    // ---- 1. the weight -------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f165::blendWeight(1.0, 1.0) == 1.0 && f165::blendWeight(0.0, 1.0) == 0.0 && f165::blendWeight(1.0, 0.0) == 0.0 && eq(f165::blendWeight(0.5, 0.5), 0.25, 1e-15),
              "F165: the weight is the crouched weight times the ADS detach weight: 1 crouched in the sights, 0 standing or at the hip, a quarter at half of each");
        check(f165::blendWeight(1.5, 1.5) == 1.0 && f165::blendWeight(-0.2, 1.0) == 0.0 && f165::blendWeight(std::nan(""), 1.0) == 0.0 && f165::blendWeight(1.0, std::nan("")) == 0.0,
              "F165: a weight past 0 to 1 is held to it and one that is not a number reads 0");
        check(f165::crouchedAds(1.0, 1.0) && f165::crouchedAds(0.5, 0.05) && !f165::crouchedAds(1.0, 0.049) && !f165::crouchedAds(0.49, 1.0) && !f165::crouchedAds(0.0, 1.0) &&
                  !f165::crouchedAds(std::nan(""), 1.0),
              "F165: crouched in the sights is the detach weight from 0.05 (F159's hip) and the crouched weight from 0.5, as F161 files a call");
        // a crouch and a stand in the sights (the animated neck's drop, F163), and a draw into the sights while crouched: no step in the weight bigger than the signals' own
        double lastW = -1.0, stepMax = 0.0;
        bool monotone = true;
        for (int i = 0; i <= 200; ++i) {
            const double neck = 1.40 - 0.0035 * i;   // 1.40 m standing-ish down to 0.70 m
            const double w = f165::blendWeight(f163::postureWeight(f163::neckDrop(false, 0.0, neck)), 1.0);
            if (lastW >= 0.0) {
                stepMax = std::fmax(stepMax, std::fabs(w - lastW));
                monotone = monotone && w >= lastW - 1e-15;
            }
            lastW = w;
        }
        double detachStepMax = 0.0;
        for (int i = 1; i <= 1000; ++i) detachStepMax = std::fmax(detachStepMax, std::fabs(f165::blendWeight(1.0, i / 1000.0) - f165::blendWeight(1.0, (i - 1) / 1000.0)));
        check(monotone && stepMax < 0.06 && eq(detachStepMax, 0.001, 1e-12) && f165::blendWeight(f163::postureWeight(f163::neckDrop(false, 0.0, 1.30)), 1.0) == 0.0 &&
                  f165::blendWeight(f163::postureWeight(f163::neckDrop(false, 0.0, 1.05)), 1.0) == 1.0,
              "F165: through a crouch in the sights the weight rises from 0 (a neck at 1.30 m) to 1 (1.05 m) without a step over 0.06 per 3.5 cm of neck, and through the draw into the sights "
              "it follows the detach weight one for one");
    }
    // ---- 2. the angle: w = 0 exact, w = 1 the body-down angle, the wrap, continuity ----------------------------------------------------------------------------------------------
    {
        const double ang[] = {0.0, 0.3, -0.3, 1.0, -2.2, 3.0, -3.0, 3.14159, -3.14159, 4.0, -4.5, 7.0};
        bool zeroExact = true, oneDown = true, negExact = true, nanExact = true;
        for (const double s : ang) {
            for (const double d : ang) {
                const double r0 = f165::blendAngle(s, d, 0.0);
                zeroExact = zeroExact && std::memcmp(&r0, &s, sizeof(s)) == 0;
                const double rn = f165::blendAngle(s, d, -0.5), rm = f165::blendAngle(s, d, std::nan(""));
                negExact = negExact && std::memcmp(&rn, &s, sizeof(s)) == 0;
                nanExact = nanExact && std::memcmp(&rm, &s, sizeof(s)) == 0;
                const double r1 = f165::blendAngle(s, d, 1.0), rb = f165::blendAngle(s, d, 1.7), want = nf::ikWrapPi(d);
                oneDown = oneDown && std::memcmp(&r1, &want, sizeof(want)) == 0 && std::memcmp(&rb, &want, sizeof(want)) == 0;
            }
        }
        check(zeroExact && negExact && nanExact, "F165: a weight of 0 (or below, or not a number) returns the stable angle bit for bit, even one outside +-pi that the caller never wrapped");
        check(oneDown, "F165: a weight of 1 (or over) returns the body-down angle wrapped, bit for bit, from every stable angle");
        check(eq(f165::blendAngle(0.2, 1.0, 0.5), 0.6, 1e-12) && eq(f165::blendAngle(1.0, 0.2, 0.25), 0.8, 1e-12) && eq(f165::blendAngle(-0.5, -1.5, 0.5), -1.0, 1e-12),
              "F165: away from the wrap the blend is the plain linear mix of the two angles");
        // the wrap at +-pi: 3.0 and -3.0 are 0.283 rad apart through pi, not 6.0 through 0
        const double q = f165::blendAngle(3.0, -3.0, 0.25), m = f165::blendAngle(3.0, -3.0, 0.5), t = f165::blendAngle(3.0, -3.0, 0.75);
        const double q2 = f165::blendAngle(-3.0, 3.0, 0.25), m2 = f165::blendAngle(-3.0, 3.0, 0.5), t2 = f165::blendAngle(-3.0, 3.0, 0.75);
        check(eq(q, 3.0 + 0.25 * (2.0 * kPi - 6.0), 1e-12) && std::fabs(m) > 3.14 && t < -3.0 && t > -3.14 && eq(q2, -3.0 - 0.25 * (2.0 * kPi - 6.0), 1e-12) && std::fabs(m2) > 3.14 && t2 > 3.0 &&
                  t2 < 3.14,
              "F165: the blend goes the short way round across +-pi: 3.0 toward -3.0 passes pi (0.28 rad), not 0 (6.0 rad), and the same from -3.0 toward 3.0");
        check(circ(f165::blendAngle(3.0, -3.0, 0.5), kPi) < 1e-9 && circ(f165::blendAngle(-3.0, 3.0, 0.5), kPi) < 1e-9 && std::fabs(f165::blendAngle(3.0, -3.0, 0.5)) <= kPi &&
                  std::fabs(f165::blendAngle(-3.0, 3.0, 0.5)) <= kPi,
              "F165: the midpoint across the wrap is pi itself, returned inside (-pi, pi]");
        // continuity in w: successive results (a thousandth of w apart) are never further apart than that share of the swing (measured round the circle, so a result at +pi and one at -pi are one)
        bool continuous = true, endpoints = true, inside = true;
        double worstRatio = 0.0;
        const double pairs[][2] = {{0.1, -0.1}, {3.0, -3.0}, {-3.1, 3.1}, {2.0, -1.0}, {1.0, -2.5}, {-2.9, 2.9}, {0.5, 2.4}, {-1.9, 0.9}, {3.1, 0.0}, {0.0, -3.1}};
        for (const auto& p : pairs) {
            const double swing = circ(p[1], p[0]);
            double prev = f165::blendAngle(p[0], p[1], 0.0);
            for (int i = 1; i <= 1000; ++i) {
                const double w = i / 1000.0, cur = f165::blendAngle(p[0], p[1], w);
                inside = inside && std::fabs(cur) <= kPi + 1e-15;
                const double step = circ(cur, prev);
                worstRatio = std::fmax(worstRatio, swing > 0 ? step / (swing * 0.001) : 0.0);
                continuous = continuous && step <= swing * 0.001 * (1.0 + 1e-9) + 1e-12;
                prev = cur;
            }
            endpoints = endpoints && circ(f165::blendAngle(p[0], p[1], 0.0), p[0]) < 1e-12 && circ(f165::blendAngle(p[0], p[1], 1.0), p[1]) < 1e-12 &&
                        circ(f165::blendAngle(p[0], p[1], 0.999999), p[1]) < swing * 1e-5 + 1e-12;
        }
        check(continuous && endpoints && inside && worstRatio < 1.0 + 1e-6,
              "F165: the blend is continuous in w: across ten stable and body-down pairs (several across +-pi) each thousandth of w moves the angle by that share of the swing and no more, from the stable "
              "angle at 0 to the body-down angle at 1, always inside (-pi, pi]");
    }
    // ---- 3. the real solver: the flight's crouched aim ------------------------------------------------------------------------------------------------------------------------------
    {
        constexpr double kUp[3] = {0.0, 1.0, 0.0};
        const double ref[3] = {0.0, -1.0, 0.0};
        const double outSign = -1.0, outM = nf::kIkAdsElbowOutCm / 100.0, trim = nf::kIkAdsTrimDeg * kPi / 180.0;
        // the left shoulder, and the foregrip 14 cm to its right, 3 cm up and 47 cm ahead (the hand level with the shoulder, the arm bent: the elbow 15 cm off the shoulder-to-hand axis)
        const double S[3] = {-0.19, 0.85, 0.0}, T[3] = {S[0] + 0.14, S[1] + 0.03, S[2] + 0.47};
        const nf::IkTwoBone tb = nf::ikTwoBoneFrame(S, T, 0.30, 0.28, ref);
        const nf::IkPistolDownSwivel down = nf::ikPistolDownSwivel(tb, ref, kUp, outM, trim);
        check(tb.ok && down.ok && tb.h > 0.13 && tb.h < 0.17 && f159::bentArm(tb.h), "F165: the flight's reach in the sights is a bent arm (the elbow 13 to 17 cm off the axis) and has a body-down pole");
        // the stable seed: the reference 43 deg from the body-down pole, toward the arm's outside (the lateral axis dir x up points to the left of a forward arm)
        double lateral[3];
        nf::ikCross(tb.dir, kUp, lateral);
        nf::ikUnit(lateral, lateral);
        const double toLeft = nf::ikWrapPi(tb.angleOf(lateral) - down.theta) >= 0.0 ? 1.0 : -1.0;
        nf::IkPhysicalPole seed;
        seed.valid = nf::ikElbowRadial(tb, nf::ikWrapPi(down.theta + toLeft * 43.0 / kDeg), seed.reference);
        for (int i = 0; i < 3; ++i) {
            seed.previous[i] = seed.reference[i];
            seed.axis[i] = tb.dir[i];
        }
        const nf::IkStableSwivel stable = nf::ikStableSwivel(tb, seed, outSign, outM);
        const double stableTheta = nf::ikWrapPi(stable.theta);
        double Es[3], Ed[3];
        tb.elbowAt(stableTheta, Es);
        tb.elbowAt(down.theta, Ed);
        const double offStable = circ(stableTheta, down.theta) * kDeg;
        check(stable.ok && stable.transportedReference && offStable > 40.0 && offStable < 46.0 && Es[1] - S[1] <= 1e-9,
              "F165: the stable pole of the flight's seed holds the elbow 40 to 46 deg off the body-down pole (F98a: 43.1 and 39.8 deg) and under the shoulder");

        // w = 0: the stable angle, nothing computed, bit for bit
        const f165::Blend none = f165::blendPole(tb, stableTheta, 0.0, seed.previous, outSign, outM, trim);
        check(!none.applied && !none.guarded && std::memcmp(&none.theta, &stableTheta, sizeof(stableTheta)) == 0,
              "F165: at w = 0 (standing, or at the hip) blendPole returns the stable angle bit for bit and applies nothing");
        // w = 1: the body-down pole's angle, the elbow where the pistol's and F84's pole puts it
        const f165::Blend full = f165::blendPole(tb, stableTheta, 1.0, seed.previous, outSign, outM, trim);
        double Ef[3];
        tb.elbowAt(full.theta, Ef);
        check(full.applied && !full.guarded && std::memcmp(&full.theta, &full.downTheta, sizeof(double)) == 0 && circ(full.theta, down.theta) < 1e-12 && eq(Ef[1], Ed[1], 1e-12),
              "F165: at w = 1 the swivel is the body-down pole's, the elbow where F81's pole puts it");
        // the elbow sits lower under the gun: well under the shoulder and 5 cm or more under where the stable pole held it
        check(Ef[1] - S[1] < -0.10 && Es[1] - Ef[1] > 0.05,
              "F165: crouched in the sights the elbow sits lower: over 10 cm under the shoulder, and over 5 cm under where the stable pole held it");
        // and its swivel from the body-down pole's plane drops from 43 deg to 0, monotonically, with the elbow lowering and the ceiling kept all the way
        bool lowerAlways = true, planeFalls = true, ceilingKept = true, unguarded = true;
        double lastOver = 1e9, lastPlane = 1e9;
        for (int i = 0; i <= 100; ++i) {
            const f165::Blend b = f165::blendPole(tb, stableTheta, i / 100.0, seed.previous, outSign, outM, trim);
            double E[3];
            tb.elbowAt(b.theta, E);
            const double plane = circ(b.theta, down.theta) * kDeg;
            lowerAlways = lowerAlways && E[1] - S[1] <= lastOver + 1e-12;
            planeFalls = planeFalls && plane <= lastPlane + 1e-9;
            ceilingKept = ceilingKept && E[1] - S[1] <= 1e-9;
            unguarded = unguarded && !b.guarded;
            lastOver = E[1] - S[1];
            lastPlane = plane;
        }
        check(lowerAlways && planeFalls && ceilingKept && unguarded && lastPlane < 1e-9,
              "F165: from w = 0 to 1 in a hundred steps the elbow only lowers, its swivel from the body-down pole's plane only falls (43 deg to 0) and it never rises over the shoulder");
        const f165::Blend half = f165::blendPole(tb, stableTheta, 0.5, seed.previous, outSign, outM, trim);
        check(eq(circ(half.theta, down.theta) * kDeg, offStable * 0.5, 1e-6),
              "F165: at w = 0.5 the swivel is half way from the stable pole's to the body-down pole's");
        // the stable pole's own state is not the blend's: the reference, the axis and the previous radial of the seed are what they were (blendPole reads them, never writes)
        nf::IkPhysicalPole copy = seed;
        f165::blendPole(tb, stableTheta, 0.7, copy.previous, outSign, outM, trim);
        check(std::memcmp(&copy, &seed, sizeof(seed)) == 0 && nf::ikStableSwivel(tb, copy, outSign, outM).theta == stable.theta,
              "F165: the seed is not touched by the blend: the next solve's stable pole is the same angle, so leaving the sights returns along the pole's own swivel");
        // leaving the sights: the weight falls from 1 to 0 and the angle returns to the stable pole's, continuous the whole way
        bool returns = true;
        double prevTheta = full.theta;
        for (int i = 99; i >= 0; --i) {
            const f165::Blend b = f165::blendPole(tb, stableTheta, i / 100.0, seed.previous, outSign, outM, trim);
            returns = returns && circ(b.theta, prevTheta) <= offStable / kDeg * 0.01 + 1e-9;
            prevTheta = b.theta;
        }
        check(returns && circ(prevTheta, stableTheta) < 1e-12, "F165: leaving the sights (or standing up) the swivel returns to the stable pole's in even steps of 1% of the swing");

        // F159's reseed: the pole's angle is the body-down pole's (same function, same arguments), and the blend of that angle toward itself is itself at every weight
        bool reseedIdle = true;
        for (const double w : {0.05, 0.3, 0.5, 0.9, 1.0}) {
            const f165::Blend b = f165::blendPole(tb, nf::ikWrapPi(down.theta), w, seed.previous, outSign, outM, trim);
            reseedIdle = reseedIdle && b.applied && !b.guarded && std::memcmp(&b.theta, &down.theta, sizeof(double)) == 0;
        }
        check(reseedIdle, "F165: after F159's reseed (the pole's angle is the body-down pole's) the blend returns that angle bit for bit at every weight: the two cannot fight");
        // F84's rifle support (the laser AR) takes the same body-down pole, by (1 - detach): at full detach (the sights) it returns the blend's angle untouched, and part way it moves further
        // toward the same pole, never away
        const f165::Blend mid = f165::blendPole(tb, stableTheta, 0.4, seed.previous, outSign, outM, trim);
        double t84 = 0.0;
        bool fullDetach = nf::ikRifleSupportTheta(tb, mid.theta, 1.0, 1.0, outM, trim, &t84) && std::memcmp(&t84, &mid.theta, sizeof(double)) == 0;
        bool towardDown = true;
        for (const double detach : {0.0, 0.25, 0.5, 0.75}) {
            double t = 0.0;
            towardDown = towardDown && nf::ikRifleSupportTheta(tb, mid.theta, detach, 1.0, outM, trim, &t) && circ(t, down.theta) <= circ(mid.theta, down.theta) + 1e-12;
        }
        check(fullDetach && towardDown,
              "F165: with F84's rifle support after the blend: at full ADS detach it leaves the blend's angle bit for bit, and at a smaller one it moves it toward the same body-down pole, not away");
    }
    // ---- 3b. the clamp: a mix that would put the elbow over the shoulder -----------------------------------------------------------------------------------------------------------
    {
        // a nearly straight arm (the hand 62 cm ahead, 2 cm under the shoulder level): the circle is 1.3 cm across and a stable swivel near the top of it is just under the shoulder, so the short
        // way round to the body-down pole passes over it
        const double ref[3] = {0.0, -1.0, 0.0}, S[3] = {-0.19, 0.85, 0.0};
        const double T[3] = {S[0] - 0.13, S[1] - 0.02, S[2] + 0.61};
        const double outSign = -1.0, outM = 0.058, trim = nf::kIkAdsTrimDeg * nf::kIkPi / 180.0;
        const nf::IkTwoBone tb = nf::ikTwoBoneFrame(S, T, 0.30, 0.28, ref);
        const double stableTheta = 91.0 * nf::kIkPi / 180.0;
        double Es[3];
        tb.elbowAt(stableTheta, Es);
        const f165::Blend b = f165::blendPole(tb, stableTheta, 0.5, ref, outSign, outM, trim);
        const double up[3] = {0.0, 1.0, 0.0};
        const nf::IkPistolDownSwivel down = nf::ikPistolDownSwivel(tb, ref, up, outM, trim);
        double Eu[3], Eg[3];
        tb.elbowAt(f165::blendAngle(stableTheta, nf::ikWrapPi(down.theta), 0.5), Eu);
        tb.elbowAt(b.theta, Eg);
        check(tb.ok && down.ok && Es[1] - S[1] <= 0.0 && Eu[1] - S[1] > 1e-6 && b.applied && b.guarded && Eg[1] - S[1] <= 1e-9,
              "F165: a mix that puts the elbow over the shoulder where the stable pole's was under it goes through the existing clamp (the ceiling alone): this near-straight arm's plain mix is over "
              "the shoulder, the clamped one is not");
        // the property over the whole circle on this arm and the flight's: no weight puts a feasible stable pole's elbow over the shoulder
        bool never = true;
        const double S2[3] = {-0.19, 0.85, 0.0};
        const double reaches[][3] = {{-0.13, -0.02, 0.61}, {0.14, 0.03, 0.47}, {0.0, -0.20, 0.52}, {0.30, 0.20, 0.25}, {-0.20, -0.40, 0.30}, {0.05, 0.40, 0.30}};
        for (const auto& r : reaches) {
            const double Tr[3] = {S2[0] + r[0], S2[1] + r[1], S2[2] + r[2]};
            const nf::IkTwoBone t = nf::ikTwoBoneFrame(S2, Tr, 0.30, 0.28, ref);
            if (!t.ok) continue;
            for (int k = -179; k <= 180; k += 2) {
                const double th = k * nf::kIkPi / 180.0;
                double E[3];
                t.elbowAt(th, E);
                if (E[1] - S2[1] > 0.0) continue;
                for (int i = 1; i < 20; ++i) {
                    const f165::Blend c = f165::blendPole(t, th, i / 20.0, ref, outSign, outM, trim);
                    double E2[3];
                    t.elbowAt(c.theta, E2);
                    never = never && E2[1] - S2[1] <= 1e-6;
                }
            }
        }
        check(never, "F165: over six reaches, every feasible stable swivel (every 2 deg) and nineteen weights, the blended elbow never rises over the shoulder");
    }
    // ---- 4. the per-second figures and the line -----------------------------------------------------------------------------------------------------------------------------------
    {
        f165::Counters c;
        const auto add = [&](double weight, double plane, bool applied, bool crouchedAds) {
            f165::Sample s;
            s.weight = weight;
            s.planeDeg = plane;
            s.applied = applied;
            s.crouchedAds = crouchedAds;
            c.add(s);
        };
        add(1.0, 10.0, true, true);
        add(0.8, 20.0, true, true);
        add(0.0, 30.0, false, false);
        add(0.0, 40.0, false, false);
        add(0.2, 50.0, true, false);
        add(std::nan(""), 5.0, true, true);   // not counted
        add(1.0, std::nan(""), true, true);   // not counted
        check(c.n == 5 && c.ads.n == 2 && c.ads.applied == 2 && c.rest.n == 3 && c.rest.applied == 1 && eq(c.weightSum, 2.0, 1e-12) && eq(c.ads.planeSum, 30.0, 1e-12) &&
                  eq(c.ads.planeMax, 20.0, 1e-12) && eq(c.rest.planeSum, 120.0, 1e-12) && eq(c.rest.planeMax, 50.0, 1e-12),
              "F165: the counters: the stable-pole solves, the weight's sum, and in the sights crouched against the rest the solves, the ones with the blend applied and the plane change's sum and largest "
              "(a figure that is not a number is left out)");
        f165::Counters d;
        d.merge(c);
        d.merge(c);
        check(d.n == 10 && d.ads.n == 4 && d.ads.applied == 4 && d.rest.applied == 2 && eq(d.weightSum, 4.0, 1e-12) && eq(d.rest.planeSum, 240.0, 1e-12) && eq(d.ads.planeMax, 20.0, 1e-12),
              "F165: two seconds merge by sums and largest");

        char buf[1180] = "native on foot left elbow pole (F159) 10 s: item x";
        const size_t head = std::strlen(buf);
        const int n = f165::appendSummary(buf, sizeof(buf), d);
        check(n > 0 && std::strlen(buf) == head + static_cast<size_t>(n) &&
                  std::strstr(buf, "; elbow blend to the body-down pole (F165): weight mean 0.40; in the sights crouched 4 solves, 4 blended, the elbow 15.0 deg off the stance's plane (max 20.0); the rest 6 solves, 2 blended, 40.0 deg (max 50.0)") != nullptr,
              "F165: the line's part names the weight's mean, then the solves crouched in the sights and the rest with the blends applied and the plane change's mean and largest");
        char none[64] = "head";
        check(f165::appendSummary(none, sizeof(none), f165::Counters{}) == 0 && std::strcmp(none, "head") == 0, "F165: a window with no stable-pole solve adds nothing to the line");

        // the line's width at its widest: F159's part at its widest figures and F165's, inside the log's 1200 bytes with its stamp
        f165::Counters w;
        w.n = 99999;
        w.weightSum = 99999.0;
        w.ads.n = 99999;
        w.ads.applied = 99999;
        w.ads.planeSum = 99999.0 * 179.9;
        w.ads.planeMax = 179.9;
        w.rest.n = 99999;
        w.rest.applied = 99999;
        w.rest.planeSum = 99999.0 * 179.9;
        w.rest.planeMax = 179.9;
        f159::Counters c159;
        c159.n = 99999; c159.hipN = 99999; c159.high = 99999; c159.hipHigh = 99999; c159.ceiling = 99999; c159.reseeds = 99999;
        for (uint32_t i = 0; i < f159::kWhyCount; ++i) c159.births[i] = 99999;
        c159.solvedSum = -99999.0 * 123.4; c159.solvedMin = -123.4; c159.solvedMax = -123.4; c159.fpSum = -99999.0 * 123.4; c159.fpMin = -123.4; c159.fpMax = -123.4; c159.dispSum = 99999.0 * 179.9; c159.dispMax = 179.9;
        f159::HighSeconds hs;
        hs.any = hs.hip = hs.run = hs.runMax = 99999;
        char wide[1180];
        f159::formatSummary(wide, sizeof(wide), 99999, "$wpn_m_assaultrifle_kinetic_fauto_name;$wpn_m_assaultrifle_kinetic_fauto_nam", c159, hs);
        const size_t f159Len = std::strlen(wide);
        f165::appendSummary(wide, sizeof(wide), w);
        check(std::strlen(wide) > f159Len + 150 && std::strlen(wide) < 1180 - 1 && std::strlen(wide) + 24 < 1200 && std::strstr(wide, "(max 179.9)") != nullptr,
              "F165: with F159's part at its widest figures and a 63-character item the whole line plus the log's stamp is under 1200 bytes, F165's part intact at its end");
        char tiny[200];
        std::snprintf(tiny, sizeof(tiny), "%s", "native on foot left elbow pole (F159) 10 s: item x");
        f165::appendSummary(tiny, sizeof(tiny), d);
        check(std::strlen(tiny) == sizeof(tiny) - 1 && std::strncmp(tiny, "native on foot left elbow pole (F159) 10 s: item x; elbow blend", 63) == 0,
              "F165: the part cut by a short buffer keeps the line's head and stays terminated");
    }
}

}  // namespace f165test
