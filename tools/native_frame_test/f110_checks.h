// F110 checks (native on-foot VR): the ADS sight lock's model of the right eye. Header only, run by native_frame_test (run110).
// The job camera is the left eye (F66e; the F102 flight over Virtual Desktop: L - cam 0.0 cm, R - cam +6.2 cm, IPD 6.18 cm); a tester's eyes' offsets read
// -/+0.0313 m (log edvr_gfx_20261025_114257), and F63's model (the job camera plus half the IPD) stood at the nose there.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"

#include <cmath>

namespace f110test {

namespace nf = edvr::nofoot;

inline bool near3(const double a[3], const double b[3], double tol) {
    return std::fabs(a[0] - b[0]) <= tol && std::fabs(a[1] - b[1]) <= tol && std::fabs(a[2] - b[2]) <= tol;
}

template <class Check>
inline void run110(Check&& check) {
    const double rHat[3] = {1.0, 0.0, 0.0};
    const double leftEye[3] = {0.0, 1.7, 0.0}, rightEye[3] = {0.0626, 1.7, 0.0};
    {   // the job camera is the left eye: the model lands on the right eye; F63's half lands at the midpoint (the control)
        bool measured = false;
        const double ipd = nf::adsSightIpd(true, -0.0313, true, 0.0313, &measured);
        double model[3] = {};
        const uint8_t src = nf::adsModelRightEye(leftEye, false, leftEye, rHat, ipd, model);
        const double f63[3] = {leftEye[0] + 0.0313 * rHat[0], leftEye[1], leftEye[2]};
        check(measured && std::fabs(ipd - 0.0626) < 1e-12 && src == nf::kAdsModelFromCamera && near3(model, rightEye, 1e-12) &&
                  std::fabs((rightEye[0] - f63[0]) - 0.0313) < 1e-12,
              "F110: from the job camera (the left eye) the model is the right eye (the IPD 6.26 cm from the offsets -/+3.13 cm); F63's half stood 3.13 cm short, at the nose");
    }
    {   // only the left eye's rows: the solved left eye plus the IPD, even with the job camera 4 mm off it
        const double cam[3] = {0.004, 1.698, 0.002}, solvedLeft[3] = {0.0, 1.7, 0.0};
        double model[3] = {};
        const uint8_t src = nf::adsModelRightEye(cam, true, solvedLeft, rHat, 0.0626, model);
        check(src == nf::kAdsModelFromLeftEye && near3(model, rightEye, 1e-12),
              "F110: with only the left eye's rows the model is the solved left eye plus the IPD (the job camera's 4 mm error left out)");
    }
    {   // the fallback: no offsets, the right eye's alone, out of range
        bool m1 = true, m2 = false, m3 = true;
        const double none = nf::adsSightIpd(false, 0.0, false, 0.0, &m1);
        const double rightOnly = nf::adsSightIpd(false, 0.0, true, 0.0313, &m2);
        const double wide = nf::adsSightIpd(false, 0.0, true, 0.06, &m3);
        check(none == nf::kAdsSightIpdFallbackM && !m1 && std::fabs(nf::kAdsSightIpdFallbackM - 2.0 * 0.0315) < 1e-12 && std::fabs(rightOnly - 0.0626) < 1e-12 &&
                  m2 && wide == nf::kAdsSightIpdFallbackM && !m3,
              "F110: no offsets: the fallback 6.3 cm (twice F63's 3.15); the right eye's offset alone: twice it; out of 4..9 cm: the fallback");
    }
    {   // world scale: Elite's eye pair is the headset's times 100 over the percent the runtime was told, so the window and the fallback move with it (eyeScale 0.5 at 200 percent)
        const double at200 = 0.0626 * 100.0 / 200.0;   // 31.3 mm: under the 4 cm floor of the unscaled window
        bool unscaled = true, scaled = false, fell = true, atDefault = false;
        const double refused = nf::adsSightIpd(true, -0.5 * at200, true, 0.5 * at200, &unscaled);
        const double taken = nf::adsSightIpd(true, -0.5 * at200, true, 0.5 * at200, &scaled, 0.5);
        const double fallback = nf::adsSightIpd(false, 0.0, false, 0.0, &fell, 0.5);
        const double wideAt50 = nf::adsSightIpd(true, -0.0626, true, 0.0626, &atDefault, 2.0);   // 125.2 mm: a 50 percent pair, inside the scaled 8..18 cm window
        check(!unscaled && refused == nf::kAdsSightIpdFallbackM && scaled && std::fabs(taken - at200) < 1e-12 && !fell && std::fabs(fallback - 0.5 * nf::kAdsSightIpdFallbackM) < 1e-12 &&
                  atDefault && std::fabs(wideAt50 - 0.1252) < 1e-12,
              "F110: world scale: at 200 percent the 31.3 mm pair is inside the window and the fallback is 3.15 cm; at 50 percent a 12.5 cm pair is; unscaled, the same 31.3 mm pair is refused");
        check(nf::adsSightIpd(true, -0.0313, true, 0.0313, nullptr, 1.0) == nf::adsSightIpd(true, -0.0313, true, 0.0313, nullptr) &&
                  nf::adsSightIpd(false, 0.0, false, 0.0, nullptr, 1.0) == nf::kAdsSightIpdFallbackM,
              "F110: world scale: at 100 percent (scale 1) the answer is the unscaled one to the bit");
    }
    {   // the sign: a runtime whose eyes' x offsets are mirrored still gives the IPD as a distance; the model goes along R^ (the FP right), not the offsets' sign
        const double ipd = nf::adsSightIpd(true, 0.0313, true, -0.0313, nullptr);
        double model[3] = {};
        nf::adsModelRightEye(leftEye, false, leftEye, rHat, ipd, model);
        const double kPi = 3.14159265358979323846, c = std::cos(30.0 * kPi / 180.0), s = std::sin(30.0 * kPi / 180.0);
        const double yawed[3] = {c, 0.0, -s};
        double modelYawed[3] = {};
        nf::adsModelRightEye(leftEye, false, leftEye, yawed, ipd, modelYawed);
        const double want[3] = {0.0626 * c, 1.7, -0.0626 * s};
        check(std::fabs(ipd - 0.0626) < 1e-12 && near3(model, rightEye, 1e-12) && near3(modelYawed, want, 1e-12),
              "F110: mirrored offsets (+/-3.13 cm) give the same 6.26 cm; the model goes along the FP right, also yawed 30 deg");
    }
}

}  // namespace f110test
