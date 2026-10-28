// F143 checks (native on-foot VR), run143 below. Header only, run by native_frame_test.
//
// F143 (native_on_foot_f143.h): (1) the ADS eye's shift taken from the game's own sight line E -> S (read by F141's hook, expressed relative to P14w in the FP root's axes;
// F144 keeps it settled per (item, scope)): Q is the point on the line at the lock's eye depth k along the root's forward from the origin, the game's shift is Q less
// (origin + k f) less its part along f, and F139's 8 cm bound stands (F146 removed the two live trims). (2) the coat's clip modifiers: the parts the unclip A/B adds (everything but
// the head parts and F19's list), and when the measure reads.
#pragma once

#include "../../src/d3d11/avatar_hooks_core.h"
#include "../../src/d3d11/native_on_foot_f143.h"
#include "f141_checks.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f143test {

namespace f143 = edvr::nofoot::f143;
namespace f139 = edvr::nofoot::f139;

inline bool near143(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
inline double dot143(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

template <class Check>
inline void run143(Check&& check) {
    // ---- the flight's laser SMG: E (-0.50, -4.61, +14.93), S (-0.46, -4.63, +35.9) cm from P14 in the root's axes, k 21 cm: up -4.62, right -0.49
    const double eCm[3] = {-0.50, -4.61, 14.93}, sCm[3] = {-0.46, -4.63, 35.9};
    double e[3], sv[3];
    for (int i = 0; i < 3; ++i) {
        e[i] = 0.01 * eCm[i];
        sv[i] = 0.01 * sCm[i];
    }
    {
        const double p14w[3] = {10.0, 1.6, -5.0}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        const f143::Choice c = f143::lineShift(e, sv, p14w, up, right, fwd, p14w, 0.21);
        check(c.game && c.why == f143::kGame && near143(c.gameUpCm, -4.62, 0.005) && near143(c.gameRightCm, -0.49, 0.005),
              "F143: the flight's laser SMG: E (-0.50, -4.61, +14.93) and S (-0.46, -4.63, +35.9) cm from P14, k 21 cm: the game's shift is up -4.62, right -0.49 cm");
        check(near143(c.shift[0], -0.0049, 5e-5) && near143(c.shift[1], -0.0462, 5e-5) && near143(c.shift[2], 0.0, 1e-12) && near143(c.shiftM, std::sqrt(0.0049 * 0.0049 + 0.0462 * 0.0462), 1e-4),
              "F143: ... as a world shift on a level root (metres), nothing along the forward");
        const double qRel[3] = {c.q[0] - p14w[0], c.q[1] - p14w[1], c.q[2] - p14w[2]};
        check(near143(dot143(qRel, fwd), 0.21, 1e-9), "F143: Q lies at the lock's eye depth along the forward (dot(Q - origin, f) = k)");
        const double lineDeg = std::atan2(std::sqrt(0.0004 * 0.0004 + 0.0002 * 0.0002), 0.2097) * 180.0 / 3.14159265358979323846;
        check(near143(c.lineDeg, lineDeg, 1e-6) && c.lineDeg > 0.1 && c.lineDeg < 0.2, "F143: the line's angle off the forward (0.12 deg) is the figure the line prints");
    }
    {   // a turned, pitched root and P14 elsewhere: the same root coordinates give the same shift in the root's axes
        float froot[16];
        const double rootPos[3] = {-18.4, 48.8, 45.2};
        f141test::rigid(37.0, 22.0, rootPos, froot);
        double r[3], u[3], f[3];
        check(f143::rootAxes(froot, r, u, f), "F143: a rigid root has unit axes");
        const double p14w[3] = {-18.48, 48.9, 45.3};
        const f143::Choice c = f143::lineShift(e, sv, p14w, u, r, f, p14w, 0.21);
        check(c.game && near143(c.gameUpCm, -4.62, 0.005) && near143(c.gameRightCm, -0.49, 0.005) && near143(dot143(c.shift, f), 0.0, 1e-12) && near143(dot143(c.gameShift, f), 0.0, 1e-12),
              "F143: on a root turned 37 deg and pitched 22 deg the shift is the same in the root's axes and perpendicular to the forward");
        const float pos[3] = {static_cast<float>(p14w[0] + 0.0004 * r[0] - 0.0462 * u[0] + 0.21 * f[0]), static_cast<float>(p14w[1] + 0.0004 * r[1] - 0.0462 * u[1] + 0.21 * f[1]),
                              static_cast<float>(p14w[2] + 0.0004 * r[2] - 0.0462 * u[2] + 0.21 * f[2])};
        double back[3];
        check(f143::toRoot(froot, p14w, pos, back) && near143(back[0], 0.0004, 2e-6) && near143(back[1], -0.0462, 2e-6) && near143(back[2], 0.21, 2e-6),
              "F143: a world point expressed in the root's right, up and forward from P14w");
        float zero[16] = {};
        const float nanPos[3] = {std::nanf(""), 0.0f, 0.0f};
        check(!f143::toRoot(zero, p14w, pos, back) && !f143::toRoot(froot, p14w, nanPos, back), "F143: a root with no axes, or a point that is not a number: no coordinates");
    }
    {   // the origin is the lock's (F79's measured origin may replace P14w): the target is Q whatever the origin
        const double p14w[3] = {0.0, 0.0, 0.0}, origin[3] = {0.004, 0.002, 0.03}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        const f143::Choice c = f143::lineShift(e, sv, p14w, up, right, fwd, origin, 0.21);
        const double target[3] = {origin[0] + 0.21 * fwd[0] + c.shift[0], origin[1] + 0.21 * fwd[1] + c.shift[1], origin[2] + 0.21 * fwd[2] + c.shift[2]};
        const double qOnPlane[3] = {c.q[0], c.q[1], origin[2] + 0.21};
        check(c.game && near143(target[0], qOnPlane[0], 1e-9) && near143(target[1], qOnPlane[1], 1e-9) && near143(target[2], qOnPlane[2], 1e-12),
              "F143: with another origin the eye target (origin + k f + shift) is the point of the line at that depth");
    }
    {   // F146: no trims; the bound is F139's 8 cm on the game's own shift
        const double p14w[3] = {0, 0, 0}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        const f143::Choice base = f143::lineShift(e, sv, p14w, up, right, fwd, p14w, 0.21);
        check(base.game && near143(base.shift[0], base.gameShift[0], 1e-15) && near143(base.shift[1], base.gameShift[1], 1e-15) && near143(base.shift[2], base.gameShift[2], 1e-15) &&
                  near143(base.shiftM, std::sqrt(dot143(base.gameShift, base.gameShift)), 1e-15),
              "F143: the shift is the game's shift alone (F146 removed the two live trims)");
        const double e2[3] = {0.0, 0.20, 0.05}, s2[3] = {0.0, 0.20, 0.45};   // a line 20 cm over the sight
        const f143::Choice past = f143::lineShift(e2, s2, p14w, up, right, fwd, p14w, 0.21);
        check(!past.game && past.why == f143::kBound && past.shiftM == 0.0, "F143: a game line 20 cm off the sight at the eye depth is past the bound");
        const double e3[3] = {0.0, 0.07, 0.05}, s3[3] = {0.0, 0.07, 0.45};   // 7 cm over: inside
        const double e4[3] = {0.0, 0.085, 0.05}, s4[3] = {0.0, 0.085, 0.45};   // 8.5 cm over: past
        check(f143::lineShift(e3, s3, p14w, up, right, fwd, p14w, 0.21).game && f143::lineShift(e4, s4, p14w, up, right, fwd, p14w, 0.21).why == f143::kBound,
              "F143: 7 cm off the sight is inside F139's 8 cm bound, 8.5 cm is past it");
    }
    {   // degenerate lines
        const double p14w[3] = {0, 0, 0}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        const double flatE[3] = {0.0, -0.04, 0.20}, flatS[3] = {0.005, -0.04, 0.209};
        check(f143::lineShift(flatE, flatS, p14w, up, right, fwd, p14w, 0.21).why == f143::kDegenerate, "F143: S 0.9 cm further along than E: under 1 cm of depth, refused");
        const double okE[3] = {0.0, -0.04, 0.20}, okS[3] = {0.005, -0.04, 0.212};
        check(f143::lineShift(okE, okS, p14w, up, right, fwd, p14w, 0.21).game, "F143: 1.2 cm of depth is enough");
        const double backE[3] = {0.0, -0.04, 0.30}, backS[3] = {0.0, -0.04, 0.15};
        check(f143::lineShift(backE, backS, p14w, up, right, fwd, p14w, 0.21).game, "F143: a line pointing back (S nearer than E) has depth too: Q is still on it at k");
        const double zeroAxis[3] = {0, 0, 0};
        check(f143::lineShift(e, sv, p14w, zeroAxis, right, fwd, p14w, 0.21).why == f143::kDegenerate &&
                  f143::lineShift(e, sv, p14w, up, right, zeroAxis, p14w, 0.21).why == f143::kDegenerate &&
                  f143::lineShift(e, sv, p14w, up, right, fwd, p14w, std::nan("")).why == f143::kDegenerate,
              "F143: an axis with no length, or an eye depth that is not a number: refused as degenerate");
        double nanS[3] = {sv[0], std::nan(""), sv[2]};
        check(f143::lineShift(e, nanS, p14w, up, right, fwd, p14w, 0.21).why == f143::kDegenerate, "F143: a line that is not a number is refused as degenerate");
        check(std::strcmp(f143::whyText(f143::kGame), "the game's line") == 0 && std::strcmp(f143::whyText(f143::kNoLine), "no settled line yet") == 0 &&
                  std::strcmp(f143::whyText(f143::kBound), "past the 8 cm bound") == 0 && std::strcmp(f143::whyText(99), "?") == 0 && f143::kWhyCount == 6 && f143::Choice{}.why == f143::kNoLine,
              "F143: the reasons' text (F146: no \"key off\" reason; a choice not yet made reads as no line)");
        check(near143(f143::stepM(e, sv), std::sqrt(0.0004 * 0.0004 + 0.0002 * 0.0002 + 0.2097 * 0.2097), 1e-12), "F143: the step between two shifts is their distance");
    }

    // ---- 2. the coat's clip modifiers --------------------------------------------------------------------------------------------------------------------------------
    {
        check(f143::kPartCount == edvr::avh::kPartCount, "F143: the clip snapshot holds every part of the avatar's table");
        uint64_t list = 0, head = 0;
        static const char* const names[] = {"EVASuit_Torso", "EVASuit_Arms", "EVASuit_Hands", "Shoulder_Left", "Shoulder_Right", "EVASuit_Legs", "EVASuit_Feet", "FirstPersonSkeleton",
                                            "EVASuit_Gear_LowerArms", "EVASuit_Gear_LowerArmL", "EVASuit_Gear_LowerArmR"};
        for (const char* n : names) {
            const int i = edvr::avh::partIndexByName(n);
            if (i >= 0) list |= 1ull << i;
        }
        for (uint32_t h : edvr::avh::kHeadParts) head |= 1ull << h;
        const uint64_t extra = f143::extraUnclipMask(list, head);
        int count = 0;
        for (int i = 0; i < 54; ++i) count += (extra >> i) & 1;
        const auto has = [&](const char* n) { return ((extra >> edvr::avh::partIndexByName(n)) & 1ull) != 0; };
        check(count == 54 - 12 - 11, "F143: the A/B adds 31 parts: 54 less the 12 head parts and the 11 the unclip list names");
        check(has("EVASuit_Gear_Back") && has("EVASuit_Gear_UpperArms") && has("EVASuit_Gear_Chest") && has("EVASuit_Gear_Waist") && has("OuterClothing_Torso") && has("EVASuit"),
              "F143: ... among them the coat's Gear_Back and the other gear parts");
        check(!has("EVASuit_Torso") && !has("EVASuit_Arms") && !has("EVASuit_Hands") && !has("Shoulder_Left") && !has("EVASuit_Gear_LowerArmL") && !has("EVASuit_Legs") &&
                  !has("EVASuit_Feet") && !has("FirstPersonSkeleton"),
              "F143: ... not the parts already covered, nor the comparison parts the list leaves clipped, nor the first-person skeleton");
        check(!has("Head") && !has("Hair") && !has("EVASuit_Helmet") && !has("EVASuit_Eyewear") && !has("EVASuit_Gear_Head") && !has("Beard") && !has("Teeth"), "F143: ... and no head part");
        check((extra >> 54) == 0 && f143::extraUnclipMask(0, 0) == (1ull << 54) - 1ull && f143::extraUnclipMask((1ull << 54) - 1ull, 0) == 0, "F143: the mask is the avatar's 54 bits and no more");
        // armed state
        check(f143::armedState(false, "$wpn_m_assaultrifle_laser_fauto_name;") == f143::kUnarmed && f143::armedState(true, "$wpn_m_assaultrifle_laser_fauto_name;") == f143::kWeapon &&
                  f143::armedState(true, "$humanoid_companalyser_name;") == f143::kTool && f143::armedState(true, nullptr) == f143::kTool && f143::armedState(true, "") == f143::kTool,
              "F143: unarmed, a weapon ($wpn_ item) and a tool");
        check(std::strcmp(f143::armedText(0), "unarmed") == 0 && std::strcmp(f143::armedText(1), "weapon") == 0 && std::strcmp(f143::armedText(2), "tool") == 0 && std::strcmp(f143::armedText(9), "?") == 0,
              "F143: the armed state as the log says it");
        // when the measure reads
        check(f143::readWhy(-1, 0, 1000, 0, 0, 300) == f143::kReadState, "F143: the first call reads");
        check(f143::readWhy(0, 0, 1000, 6000, 0, 300) == f143::kReadNo && f143::readWhy(0, 0, 5999, 6000, 0, 300) == f143::kReadNo, "F143: nothing to read before the 5 s clock");
        check(f143::readWhy(0, 0, 6000, 6000, 0, 300) == f143::kReadClock, "F143: the 5 s clock reads");
        check(f143::readWhy(0, 1, 1000, 6000, 0, 300) == f143::kReadState && f143::readWhy(1, 2, 1000, 6000, 0, 300) == f143::kReadState, "F143: a change of the armed state reads at once");
        check(f143::readWhy(0, 0, 9000, 6000, 300, 300) == f143::kReadNo && f143::readWhy(0, 2, 9000, 6000, 300, 300) == f143::kReadState,
              "F143: past the cap the clock stops reading, a change of the armed state does not");
        f143::ClipSnap snap;
        check(snap.n == 0 && snap.why == f143::kReadClock && !snap.flagsOk, "F143: an empty clip snapshot");
    }
}

}  // namespace f143test
