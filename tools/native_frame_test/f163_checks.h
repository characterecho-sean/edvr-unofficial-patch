// F163 checks (native on-foot VR), run163 below. Header only, run by native_frame_test.
//
// F163 (native_on_foot_f163.h): the crouched stance F161 holds F97a's lowering through ADS for is read from the animated neck's drop under the standing neck, not from the pin's camera
// weight (F69a), which is 0 on every call without the standing calibration. The figures are the flights edvr_gfx_20261028_153030 and edvr_gfx_20261028_135901 (no calibration in any
// ADS second, no stored unarmed neck): the animated neck per second, ADS with an item, crouched 0.847 to 1.080 m, standing 1.247 to 1.550 m; the eye dumps 153247282001 (the neck
// 0.855 m, crouched still) and 153252659002 (1.0545 m, crouch-walking), both in the sights.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f163.h"
#include "f97_checks.h"

#include <cmath>
#include <cstdint>

namespace f163test {

namespace nf = edvr::nofoot;
namespace f163 = edvr::nofoot::f163;
namespace f97 = edvr::nofoot::f97;
using f97test::Call97;
using f97test::armedNeckTargetBeforeF161;

template <class Check>
inline void run163(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    constexpr double kH = 0.155;
    // the weight of an animated neck with no stored unarmed neck (the flights had none in their crouched ADS seconds)
    const auto weightOf = [](double neckM) { return f163::postureWeight(f163::neckDrop(false, 0.0, neckM)); };
    // ---- 1. the reference ----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f163::standingNeck(false, 0.0) == f163::kStandingNeckM && f163::standingNeck(true, 1.542) == 1.542 && f163::standingNeck(true, 1.60) == 1.60,
              "F163: the upright neck is the stored unarmed standing neck when there is one (1.542 m in the flights), else the commander's 1.543 m");
        check(f163::standingNeck(true, 1.306) == f163::kStandingNeckM && f163::standingNeck(true, std::nan("")) == f163::kStandingNeckM && f163::standingNeck(true, 0.0) == f163::kStandingNeckM &&
                  f163::standingNeck(true, 2.5) == f163::kStandingNeckM,
              "F163: a stored neck that is not an upright commander's (the fists' hunch at 1.306 m, not a number, 0, 2.5 m) is not the reference: the commander's stands");
        check(eq(f163::neckDrop(false, 0.0, 0.855), f163::kStandingNeckM - 0.855, 1e-12) && eq(f163::neckDrop(true, 1.55, 1.30), 0.25, 1e-12) && std::isnan(f163::neckDrop(true, 1.55, std::nan(""))) &&
                  f163::postureWeight(std::nan("")) == 0.0,
              "F163: the drop is the upright neck less the animated one; a neck that is not a number has no drop and reads standing");
    }
    // ---- 2. the flights' numbers: crouched in the sights 1, standing 0 -----------------------------------------------------------------------------------------------------------
    {
        // the animated necks (m) of the crouched ADS seconds, per second's smallest and largest (the shotgun and the plasma AR, both flights), and the two eye dumps' necks
        const double crouched[] = {0.847, 0.850, 0.853, 0.856, 0.864, 0.871, 0.903, 0.927, 0.985, 1.012, 1.031, 1.047, 1.057, 1.065, 1.074, 1.076, 1.080, 0.855066121, 1.05453885};
        // the standing ADS seconds' smallest and largest, with every item (rifles, the shotgun, SMGs, the sniper, the launcher, pistols) over the flights of 10-27 and 10-28
        const double standing[] = {1.247, 1.250, 1.276, 1.277, 1.292, 1.297, 1.30, 1.316, 1.328, 1.337, 1.339, 1.358, 1.395, 1.436, 1.467, 1.509, 1.543, 1.55};
        bool allCrouched = true, allStanding = true;
        double crouchedLeast = 1.0, standingMost = 0.0;
        for (const double neck : crouched) {
            const double w = weightOf(neck);
            allCrouched = allCrouched && w == 1.0;
            crouchedLeast = std::fmin(crouchedLeast, w);
        }
        for (const double neck : standing) {
            const double w = weightOf(neck);
            allStanding = allStanding && w == 0.0;
            standingMost = std::fmax(standingMost, w);
        }
        check(allCrouched && crouchedLeast == 1.0, "F163: every crouched ADS neck of the flights (0.847 to 1.080 m, the two dumps' 0.855 and 1.0545 m) reads crouched exactly 1");
        check(allStanding && standingMost == 0.0, "F163: every standing ADS neck of the flights (1.247 to 1.55 m, every item) reads standing exactly 0");
        check(nf::pinCrouchedWc(weightOf(1.0545)) && !nf::pinCrouchedWc(weightOf(1.292)) && f163::kStandingNeckM - 1.080 > f163::kDropHiM && f163::kStandingNeckM - 1.247 < f163::kDropLoM,
              "F163: the band (a 33 to 43 cm drop) lies in the gap between the highest crouched neck (1.080 m) and the lowest standing one (1.247 m)");
        // what the earlier weight did on the same calls: the camera's drop 0.63 m under the standing mean read as 0 for want of the calibration, and as 1 with it
        check(nf::pinCrouchWeight(false, 1.60, 0.97) == 0.0 && nf::pinCrouchWeight(true, 1.60, 0.97) == 1.0 && nf::pinCrouchWeight(true, 1.60, 1.57) == 0.0,
              "F163: the pin's camera weight is 0 without a calibration at a camera 63 cm down (the flights), 1 with one; the neck's weight needs none");
    }
    // ---- 3. standing ADS is the earlier formula bit for bit; crouched ADS holds the lowering ---------------------------------------------------------------------------------
    {
        bool same = true;
        for (const double neck : {1.247, 1.292, 1.30, 1.358, 1.395, 1.543})
            for (const double eyeOver : {-0.05, 0.0, 0.10, 0.27})   // the camera over the animated neck (m): the standing stance 0.27 up, a bobbing one under the neck
                for (const double w : {0.0, 0.5, 1.0})
                    for (const double dw : {0.0, 0.05, 0.5, 1.0})
                        for (const double shift : {0.0, 0.012}) {
                            const double eyeHip = neck + eyeOver, pre = eyeHip - kH;
                            const double cw = weightOf(neck);
                            Call97 call{neck, pre, shift, eyeHip, kH, dw, w, cw};
                            const f97::ArmedTarget a = call.run(), b = armedNeckTargetBeforeF161(neck, call.t93(), eyeHip, kH, dw, w);
                            same = same && cw == 0.0 && a.y == b.y && a.asked == b.asked && a.lowerEye == b.lowerEye && a.raiseF93 == b.raiseF93;
                        }
        check(same, "F163: a standing neck (1.247 m and up) gives weight 0 and F97a's target is the earlier formula bit for bit, at every detach, weapon weight, shift and eye");
        // the flight's crouch-walk (edvr_gfx_20261028_135901): the neck at 0.985 m under a camera at 0.95 m
        const double n0 = 0.985, eyeHip = 0.95, pre = eyeHip - kH;
        const double cw = weightOf(n0);
        const f97::ArmedTarget held = Call97{n0, pre, 0.0, eyeHip, kH, 1.0, 1.0, cw}.run();
        check(cw == 1.0 && eq(held.y, eyeHip - kH, 1e-12) && held.y < n0 - 0.15,
              "F163: the flight's crouch-walk in ADS (neck 0.985 m, camera 0.95 m) reads crouched and the body keeps the lowering: the neck target 19 cm under the authored neck, as at the hip");
        // F161's own rule on the same call: crouched here is the gate's own 0.5
        check(nf::pinCrouchedWc(cw) && !nf::pinCrouchedWc(weightOf(1.292)) && !nf::pinCrouchedWc(std::nan("")),
              "F163: F161's context (crouched from a weight of 0.5) files the crouch-walk as ADS crouched and the rifle's standing stance as ADS standing");
    }
    // ---- 4. continuity through a crouch and a stand ------------------------------------------------------------------------------------------------------------------------------
    {
        // the weight against the neck: monotone, and no step of more than 1.6% in a millimetre of neck (the smoothstep's steepest slope, 1.5 over 10 cm)
        double prev = weightOf(1.543), maxStep = 0.0;
        bool mono = true;
        for (int i = 1; i <= 800; ++i) {
            const double w = weightOf(1.543 - i * 0.001);
            maxStep = std::fmax(maxStep, std::fabs(w - prev));
            mono = mono && w >= prev - 1e-15;
            prev = w;
        }
        check(mono && maxStep < 0.0157 && prev == 1.0 && eq(weightOf(f163::kStandingNeckM - 0.5 * (f163::kDropLoM + f163::kDropHiM)), 0.5, 1e-12),
              "F163: the weight rises one way with the neck's drop, at most 1.6% a millimetre, 0.5 half way up the band");
        // a crouch (the neck 1.30 -> 0.86 m in 0.4 s at 90 Hz, a cosine profile) with the camera 3.5 cm under the neck (the worst of the flights: a 21 cm lowering), in the sights, replayed
        // through F97a's target: the body target falls one way, to the whole lowering, and no frame moves it more than 7.5 cm (6.8 measured; the neck itself moves 1.9 cm a frame at its fastest)
        const auto replay = [&](bool down, double* maxStepOut, bool* monoOut, double* endY, double* endLow) {
            const int N = 36;
            double prevY = 0.0, step = 0.0;
            bool monotone = true, first = true;
            for (int i = 0; i <= N; ++i) {
                const double s = static_cast<double>(down ? i : N - i) / N, prof = 0.5 - 0.5 * std::cos(3.14159265358979323846 * s);
                const double n0 = 1.30 + (0.86 - 1.30) * prof, eyeHip = n0 - 0.035, pre = eyeHip - kH;
                const f97::ArmedTarget t = Call97{n0, pre, 0.0, eyeHip, kH, 1.0, 1.0, weightOf(n0)}.run();
                if (!first) {
                    step = std::fmax(step, std::fabs(t.y - prevY));
                    monotone = monotone && (down ? t.y <= prevY + 1e-12 : t.y >= prevY - 1e-12);
                }
                first = false;
                prevY = t.y;
                *endLow = eyeHip - kH;
            }
            *maxStepOut = step;
            *monoOut = monotone;
            *endY = prevY;
        };
        double stepDown = 0.0, stepUp = 0.0, yDown = 0.0, yUp = 0.0, lowDown = 0.0, lowUp = 0.0;
        bool monoDown = false, monoUp = false;
        replay(true, &stepDown, &monoDown, &yDown, &lowDown);
        replay(false, &stepUp, &monoUp, &yUp, &lowUp);
        check(monoDown && stepDown < 0.075 && stepDown > 0.03 &&eq(yDown, lowDown, 1e-12) && monoUp && stepUp < 0.075 && eq(yUp, 1.30, 1e-12),
              "F163: a crouch and a stand in the sights move the neck target one way and without a jump (under 7.5 cm a frame at 90 Hz, to the whole lowering and back to the authored neck)");
    }
}

}  // namespace f163test
