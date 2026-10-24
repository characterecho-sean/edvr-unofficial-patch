// F98a checks (native on-foot VR): the left elbow's stable pole transported from its seed, not frame to frame. Header only, run by native_frame_test (run98).
// The F97 flight (edvr_gfx_20261024_162758): with the rifle aimed 24 deg down at the hip, the left elbow sat 89-97 deg off the stance's own plane for 5-30 s at a
// time (the hand twist at the forearm's limit in every call) and came back to 1-20 deg only when F84's body-down pole took the swivel. The stable pole carried its
// reference from frame to frame (each frame's transport committed as the next frame's seed), so the reference turned by the solid angle of every loop the
// shoulder-to-hand axis went round: a cone of 40 deg half-angle turns it 84 deg. Transported from the seed in one rotation, it depends on where the arm is now only.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"

#include <cmath>

namespace f98test {

namespace nf = edvr::nofoot;

inline double angleDeg(const double a[3], const double b[3]) {
    const double d = std::fmax(-1.0, std::fmin(1.0, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]));
    return std::acos(d) * 180.0 / nf::kIkPi;
}

template <class Check>
inline void run98(Check&& check) {
    // The left arm: the shoulder, an arm of 0.30 + 0.28 m, the hand target on a cone of 40 deg about "forward and down" at 0.45 m (the axis loops once, 120 steps).
    const double S[3] = {0.18, 1.40, 0.02};
    const double d0[3] = {0.0, -0.6, 0.8}, e1[3] = {1.0, 0.0, 0.0}, e2[3] = {0.0, 0.8, 0.6};   // d0 unit, e1 and e2 unit and perpendicular to it
    const double alpha = 40.0 * nf::kIkPi / 180.0;
    const auto targetAt = [&](double phi, double T[3]) {
        for (int i = 0; i < 3; ++i) T[i] = S[i] + 0.45 * (std::cos(alpha) * d0[i] + std::sin(alpha) * (std::cos(phi) * e1[i] + std::sin(phi) * e2[i]));
    };
    const double ref[3] = {0.0, -1.0, 0.0};
    double T[3];
    targetAt(0.0, T);
    const nf::IkTwoBone tb0 = nf::ikTwoBoneFrame(S, T, 0.30, 0.28, ref);
    nf::IkPhysicalPole seed;
    seed.valid = nf::ikElbowRadial(tb0, 0.3, seed.reference);
    for (int i = 0; i < 3; ++i) {
        seed.previous[i] = seed.reference[i];
        seed.axis[i] = tb0.dir[i];
    }
    const nf::IkStableSwivel start = nf::ikStableSwivel(tb0, seed, 1.0, -1.0);
    nf::IkPhysicalPole carried = seed;   // F76-F97: each frame's transport committed as the next frame's seed
    double maxStepKept = 0.0, lastKept[3] = {start.reference[0], start.reference[1], start.reference[2]};
    nf::IkStableSwivel endKept, endCarried;
    for (int k = 1; k <= 120; ++k) {
        targetAt(2.0 * nf::kIkPi * k / 120.0, T);
        const nf::IkTwoBone tb = nf::ikTwoBoneFrame(S, T, 0.30, 0.28, ref);
        endKept = nf::ikStableSwivel(tb, seed, 1.0, -1.0);       // F98a: from the seed
        endCarried = nf::ikStableSwivel(tb, carried, 1.0, -1.0);
        for (int i = 0; i < 3; ++i) {
            carried.axis[i] = endCarried.axis[i];
            carried.reference[i] = endCarried.reference[i];
        }
        maxStepKept = std::fmax(maxStepKept, angleDeg(lastKept, endKept.reference));
        for (int i = 0; i < 3; ++i) lastKept[i] = endKept.reference[i];
    }
    check(start.ok && endKept.ok && angleDeg(start.reference, endKept.reference) < 1e-6,
          "F98a: the arm's axis round a 40 deg cone and back: the pole transported from its seed comes back to the same reference (no turn from the path)");
    check(endCarried.ok && angleDeg(start.reference, endCarried.reference) > 60.0,
          "F98a (control): carried from frame to frame (F76-F97) the reference comes back turned by the cone's solid angle (about 84 deg): the elbow's drift");
    check(maxStepKept < 6.0, "F98a: from the seed the reference still moves smoothly with the arm (under 6 deg a step on a 3 deg step of the loop)");
}

}  // namespace f98test
