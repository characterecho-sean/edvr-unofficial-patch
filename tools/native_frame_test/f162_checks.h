// F162 checks (native on-foot VR), run162 below. Header only, run by native_frame_test.
//
// F162 (native_on_foot_f162.h, the lock in native_on_foot_core.h): fix.native_on_foot_ads_sight_eye = both | right | left. The sight lock lines up the AIMING eye: the right eye for
// both and right (what F63 to F161 did, bit for bit), the left eye for left, where the job camera is the left eye and the right eye is the job camera plus the IPD along R^ (F110).
// What is shown: the key's text; the aiming eye and the share per mode; right and both give exactly the values the right-eye lock gave (the solved right eye, F110's model from the job
// camera, from a solved left eye, with only the right eye's rows, with none, with a degenerate sight); left lines the LEFT eye onto the target eye point with the right eye one IPD to
// its right (and the right mode's mirror: the left eye one IPD to the left), from solved rows and from the model (the job camera); the two locks differ by exactly the IPD along R^.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f162.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f162test {

namespace nf = edvr::nofoot;
namespace f162 = edvr::nofoot::f162;

inline bool same3(const double a[3], const double b[3]) { return std::memcmp(a, b, 3 * sizeof(double)) == 0; }
inline bool sameErr(const nf::AdsSightErr& a, const nf::AdsSightErr& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

template <class Check>
inline void run162(Check&& check) {
    const auto closeTo = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    // ---- 1. the key's text, the aiming eye, the share --------------------------------------------------------------------------------------------------------------------------------
    {
        bool k[9];
        for (bool& b : k) b = true;
        const bool ok = f162::parse("both", &k[0]) == f162::kBoth && f162::parse("Right", &k[1]) == f162::kRight && f162::parse("LEFT", &k[2]) == f162::kLeft &&
                        f162::parse("  left \r\n", &k[3]) == f162::kLeft && f162::parse("on", &k[4]) == f162::kBoth && f162::parse("OFF", &k[5]) == f162::kRight;
        const bool junk = f162::parse("", &k[6]) == f162::kBoth && f162::parse("middle", &k[7]) == f162::kBoth && f162::parse(nullptr, &k[8]) == f162::kBoth;
        check(ok && k[0] && k[1] && k[2] && k[3] && k[4] && k[5] && junk && !k[6] && !k[7] && !k[8] && f162::parse("left", nullptr) == f162::kLeft,
              "F162: the key reads both, right and left in any case with spaces around, the carried on and off as both and right, and anything else as both, not known");
        check(f162::aimingEye(f162::kBoth) == 1 && f162::aimingEye(f162::kRight) == 1 && f162::aimingEye(f162::kLeft) == 0 && std::strcmp(f162::aimingName(0), "left") == 0 &&
                  std::strcmp(f162::aimingName(1), "right") == 0 && std::strcmp(f162::name(f162::kBoth), "both") == 0 && std::strcmp(f162::name(f162::kLeft), "left") == 0,
              "F162: both and right line up the right eye (1), left the left eye (0); the names");
        namespace f106 = nf::f106;
        bool bothShares = true, otherNever = true, noTrip = true;
        for (const double w : {0.0, 0.3, 1.0}) {
            const f106::Share both = f106::share(f162::sharesView(f162::kBoth), true, true, w);
            bothShares = bothShares && both.weight == w && !both.tripped;
            for (const int mode : {int(f162::kRight), int(f162::kLeft)}) {
                const f106::Share s = f106::share(f162::sharesView(mode), true, true, w), unarmed = f106::share(f162::sharesView(mode), true, false, w);
                otherNever = otherNever && s.weight == 0.0 && !std::signbit(s.weight) && unarmed.weight == 0.0;
                noTrip = noTrip && !s.tripped && !unarmed.tripped;   // the guard is for a share that was asked for
            }
        }
        check(bothShares && otherNever && noTrip,
              "F162: the left eye shows the right eye's view (F106's share) in the both mode only: right and left give an exact 0 whatever the lock's weight and never a guard trip");
    }
    // ---- 2. the lock for the aiming eye -------------------------------------------------------------------------------------------------------------------------------------------
    const double kPi = 3.14159265358979323846;
    const auto yawPitch = [&](double yawDeg, double pitchDeg, double out[3]) {
        const double y = yawDeg * kPi / 180.0, p = pitchDeg * kPi / 180.0;
        out[0] = std::sin(y) * std::cos(p);
        out[1] = std::sin(p);
        out[2] = std::cos(y) * std::cos(p);
    };
    double f[3];
    yawPitch(22.0, -6.0, f);
    // the AMC's rows (right, up, forward in the world), the body yawed 35 deg
    const double yb = 35.0 * kPi / 180.0;
    const double rows[9] = {std::cos(yb), 0.0, -std::sin(yb), 0.0, 1.0, 0.0, std::sin(yb), 0.0, std::cos(yb)};
    const double bodyRight[3] = {rows[0], rows[1], rows[2]};
    double rHat[3];
    bool flipped = false;
    const bool haveRHat = nf::adsSightRight(bodyRight, f, bodyRight, rHat, &flipped);
    const double ipd = 0.0626, k = 0.18;
    const double p14w[3] = {1.20, 1.62, -0.35};
    const double cam[3] = {1.02, 1.55, -0.62};   // the job camera: the left eye
    // the rendered eyes: the left eye at the camera, the right eye one IPD along R^ (a roll of the head would tilt this; the solved positions carry whatever it is)
    double eyeW[2][3];
    for (int i = 0; i < 3; ++i) {
        eyeW[0][i] = cam[i];
        eyeW[1][i] = cam[i] + ipd * rHat[i];
    }
    // the same eyes with the solved positions a few millimetres off the model's, as measured ones are
    double eyeNoisy[2][3];
    const double dl[3] = {0.002, -0.003, 0.001}, dr[3] = {-0.001, 0.002, 0.003};
    for (int i = 0; i < 3; ++i) {
        eyeNoisy[0][i] = eyeW[0][i] + dl[i];
        eyeNoisy[1][i] = eyeW[1][i] + dr[i];
    }
    check(haveRHat && !flipped && closeTo(nf::ikLen(rHat), 1.0, 1e-12) && closeTo(rHat[0] * f[0] + rHat[1] * f[1] + rHat[2] * f[2], 0.0, 1e-12),
          "F162: the fixture's R^ is a unit vector across the sight line");

    // what the F63 to F161 lock did, written out here from the core's own pieces (the right eye's rows, else F110's model for the right eye)
    const auto oldLock = [&](const bool ok[2], const double eyes[2][3], bool modelOk, nf::AdsSightErr& E, double modelEye[3], bool* solved, uint8_t* src) {
        *src = nf::adsModelRightEye(cam, ok[0], eyes[0], rHat, ipd, modelEye);
        *solved = false;
        if (ok[1] && nf::adsSightErrorEyeK(p14w, eyes[1], f, rows, k, E)) {
            *solved = true;
            return true;
        }
        return modelOk && nf::adsSightErrorEyeK(p14w, modelEye, f, rows, k, E);
    };
    struct Case { bool ok[2]; const char* what; };
    const Case cases[] = {{{true, true}, "both eyes solved"}, {{true, false}, "only the left eye solved"}, {{false, true}, "only the right eye solved"}, {{false, false}, "no rows"}};
    int sameCases = 0, ranCases = 0;
    for (const double (*eyes)[3] : {eyeW, eyeNoisy}) {
        for (const Case& c : cases) {
            for (const int mode : {int(f162::kBoth), int(f162::kRight)}) {
                nf::AdsSightErr eOld, eNew;
                double mOld[3] = {}, mNew[3] = {};
                bool solvedOld = false;
                uint8_t srcOld = 0;
                const bool okOld = oldLock(c.ok, eyes, true, eOld, mOld, &solvedOld, &srcOld);
                const nf::AdsAimPick pick = nf::adsAimingEyeError(f162::aimingEye(mode), c.ok, eyes, true, cam, rHat, ipd, p14w, f, rows, k, eNew, mNew);
                ++ranCases;
                if (okOld && pick.ok && pick.solved == solvedOld && pick.modelSrc == srcOld && same3(mOld, mNew) && sameErr(eOld, eNew)) ++sameCases;
            }
        }
    }
    check(sameCases == ranCases && ranCases == 16,
          "F162: with both and right the lock takes the right eye exactly as before, bit for bit: the solved right eye, F110's model (the job camera, or a solved left eye, plus the IPD along R^), "
          "the same error, the same source, in four row cases with the eyes on the model and a few millimetres off it");
    {   // the failures are the old ones too: no usable R^ and no rows, a sight with no direction
        nf::AdsSightErr e1, e2;
        double m1[3] = {}, m2[3] = {};
        const bool none[2] = {false, false};
        const nf::AdsAimPick a = nf::adsAimingEyeError(1, none, eyeW, false, cam, rHat, ipd, p14w, f, rows, k, e1, m1);
        const double zero[3] = {0.0, 0.0, 0.0};
        const bool both[2] = {true, true};
        const nf::AdsAimPick b = nf::adsAimingEyeError(1, both, eyeW, true, cam, rHat, ipd, p14w, zero, rows, k, e2, m2);
        const nf::AdsAimPick c = nf::adsAimingEyeError(0, none, eyeW, false, cam, rHat, ipd, p14w, f, rows, k, e1, m1);
        const nf::AdsAimPick d = nf::adsAimingEyeError(0, both, eyeW, true, cam, rHat, ipd, p14w, zero, rows, k, e2, m2);
        check(!a.ok && !b.ok && !c.ok && !d.ok, "F162: no rows and no usable right axis, or a sight line with no direction, give no lock for either eye (as before for the right)");
    }
    // the left eye aims
    {
        nf::AdsSightErr eL, eLref, eR;
        double mL[3] = {}, mR[3] = {};
        const bool both[2] = {true, true};
        const nf::AdsAimPick pl = nf::adsAimingEyeError(0, both, eyeW, true, cam, rHat, ipd, p14w, f, rows, k, eL, mL);
        const bool refOk = nf::adsSightErrorEyeK(p14w, eyeW[0], f, rows, k, eLref);
        const nf::AdsAimPick pr = nf::adsAimingEyeError(1, both, eyeW, true, cam, rHat, ipd, p14w, f, rows, k, eR, mR);
        check(pl.ok && pl.solved && refOk && sameErr(eL, eLref) && pr.ok && pr.solved,
              "F162: with the left eye aiming and its rows this frame's, the error is that of the solved LEFT eye");
        // the lock moves the camera (both eyes) by the full world error to the target eye point P14w + k f: the aiming eye lands on it, the other eye is one IPD to its side
        double target[3];
        for (int i = 0; i < 3; ++i) target[i] = p14w[i] + k * f[i] / nf::ikLen(f);
        double leftAfterL[3], rightAfterL[3], leftAfterR[3], rightAfterR[3];
        for (int i = 0; i < 3; ++i) {
            leftAfterL[i] = eyeW[0][i] + eL.eFullW[i];
            rightAfterL[i] = eyeW[1][i] + eL.eFullW[i];
            leftAfterR[i] = eyeW[0][i] + eR.eFullW[i];
            rightAfterR[i] = eyeW[1][i] + eR.eFullW[i];
        }
        double perpLeft = -1.0, alongLeft = 0.0, perpRight = -1.0, alongRight = 0.0;
        nf::sightLineOfEye(leftAfterL, p14w, f, &perpLeft, &alongLeft);
        nf::sightLineOfEye(rightAfterL, p14w, f, &perpRight, &alongRight);
        const double rightSide[3] = {rightAfterL[0] - leftAfterL[0], rightAfterL[1] - leftAfterL[1], rightAfterL[2] - leftAfterL[2]};
        check(closeTo(perpLeft, 0.0, 1e-12) && closeTo(alongLeft, -k, 1e-12) && closeTo(leftAfterL[0], target[0], 1e-12) && closeTo(leftAfterL[1], target[1], 1e-12) &&
                  closeTo(leftAfterL[2], target[2], 1e-12) && closeTo(perpRight, ipd, 1e-12) && closeTo(rightSide[0] * rHat[0] + rightSide[1] * rHat[1] + rightSide[2] * rHat[2], ipd, 1e-12),
              "F162: left: after the lock the LEFT eye is on the sight line at the target eye point, and the RIGHT eye is one IPD from the line, to its right (along R^)");
        double perpL2 = -1.0, perpR2 = -1.0, aL = 0.0, aR = 0.0;
        nf::sightLineOfEye(rightAfterR, p14w, f, &perpR2, &aR);
        nf::sightLineOfEye(leftAfterR, p14w, f, &perpL2, &aL);
        const double leftSide[3] = {leftAfterR[0] - rightAfterR[0], leftAfterR[1] - rightAfterR[1], leftAfterR[2] - rightAfterR[2]};
        check(closeTo(perpR2, 0.0, 1e-12) && closeTo(aR, -k, 1e-12) && closeTo(perpL2, ipd, 1e-12) && closeTo(leftSide[0] * rHat[0] + leftSide[1] * rHat[1] + leftSide[2] * rHat[2], -ipd, 1e-12),
              "F162: right (the control): after the lock the RIGHT eye is on the line at the target eye point and the LEFT eye is one IPD from it, to its left");
        // the two locks differ by the IPD along R^ exactly: the left lock moves the head that much further to the right
        double diff[3];
        for (int i = 0; i < 3; ++i) diff[i] = eL.eFullW[i] - eR.eFullW[i];
        check(closeTo(diff[0], ipd * rHat[0], 1e-12) && closeTo(diff[1], ipd * rHat[1], 1e-12) && closeTo(diff[2], ipd * rHat[2], 1e-12),
              "F162: the left lock's world error exceeds the right lock's by exactly the IPD along R^ (the eyes are one IPD apart, the target the same)");
        // seated: the lock's output is the seated error, the eye alignment's convention; the same difference seen through the body axes
        double bodyDiff[3], seatedDiff[3], want[3];
        for (size_t i = 0; i < 3; ++i) {
            bodyDiff[i] = (eL.eFullBody[i] - eR.eFullBody[i]);
            want[i] = ipd * (rHat[0] * rows[3 * i] + rHat[1] * rows[3 * i + 1] + rHat[2] * rows[3 * i + 2]);
        }
        nf::seatedFromBody(bodyDiff, 0.0, seatedDiff);
        double wantSeated[3];
        nf::seatedFromBody(want, 0.0, wantSeated);
        check(closeTo(bodyDiff[0], want[0], 1e-12) && closeTo(bodyDiff[1], want[1], 1e-12) && closeTo(bodyDiff[2], want[2], 1e-12) && closeTo(eL.seatedFull[0] - eR.seatedFull[0], wantSeated[0], 1e-12) &&
                  closeTo(eL.seatedFull[1] - eR.seatedFull[1], wantSeated[1], 1e-12) && closeTo(eL.seatedFull[2] - eR.seatedFull[2], wantSeated[2], 1e-12) && closeTo(seatedDiff[0], wantSeated[0], 1e-12),
              "F162: in body axes and seated, the lock for the left eye differs from the right's by the IPD along R^ taken through the AMC rows (what the eye alignment is handed)");
    }
    {   // the model of the left eye is the job camera, whatever the other rows say and whatever the IPD is
        const bool onlyRight[2] = {false, true}, none[2] = {false, false}, onlyLeft[2] = {true, false};
        nf::AdsSightErr eNone, eRef, eOnlyRight, eOnlyLeft;
        double mNone[3] = {}, mOnlyRight[3] = {}, mOnlyLeft[3] = {};
        const nf::AdsAimPick a = nf::adsAimingEyeError(0, none, eyeNoisy, true, cam, rHat, ipd, p14w, f, rows, k, eNone, mNone);
        const bool refOk = nf::adsSightErrorEyeK(p14w, cam, f, rows, k, eRef);
        const nf::AdsAimPick b = nf::adsAimingEyeError(0, onlyRight, eyeNoisy, true, cam, rHat, 0.09, p14w, f, rows, k, eOnlyRight, mOnlyRight);
        const nf::AdsAimPick c = nf::adsAimingEyeError(0, onlyLeft, eyeNoisy, true, cam, rHat, ipd, p14w, f, rows, k, eOnlyLeft, mOnlyLeft);
        check(a.ok && !a.solved && a.modelSrc == nf::kAdsModelFromCamera && same3(mNone, cam) && refOk && sameErr(eNone, eRef) && b.ok && !b.solved && same3(mOnlyRight, cam) && sameErr(eOnlyRight, eRef) &&
                  c.ok && c.solved,
              "F162: left with no rows of its own: the model is the job camera, the left eye, bit for bit (not the solved right eye less the IPD, and not moved by the IPD); with its own rows it is solved");
        // with the right eye aiming and no right rows the model is still the job camera or the solved left eye PLUS the IPD (F110)
        nf::AdsSightErr eA, eB;
        double mA[3] = {}, mB[3] = {};
        const nf::AdsAimPick pa = nf::adsAimingEyeError(1, none, eyeNoisy, true, cam, rHat, ipd, p14w, f, rows, k, eA, mA);
        const nf::AdsAimPick pb = nf::adsAimingEyeError(1, onlyLeft, eyeNoisy, true, cam, rHat, ipd, p14w, f, rows, k, eB, mB);
        double wantA[3], wantB[3];
        for (int i = 0; i < 3; ++i) {
            wantA[i] = cam[i] + ipd * rHat[i];
            wantB[i] = eyeNoisy[0][i] + ipd * rHat[i];
        }
        check(pa.ok && !pa.solved && pa.modelSrc == nf::kAdsModelFromCamera && closeTo(mA[0], wantA[0], 1e-15) && closeTo(mA[1], wantA[1], 1e-15) && closeTo(mA[2], wantA[2], 1e-15) && pb.ok && !pb.solved &&
                  pb.modelSrc == nf::kAdsModelFromLeftEye && closeTo(mB[0], wantB[0], 1e-15) && closeTo(mB[1], wantB[1], 1e-15) && closeTo(mB[2], wantB[2], 1e-15),
              "F162: right with no rows of its own: still F110's model, the job camera or the solved left eye plus the IPD along R^");
    }
    {   // any aim other than 0 is the right eye (the lock never indexes past the two eyes)
        nf::AdsSightErr e0, e1;
        double m0[3] = {}, m1[3] = {};
        const bool ok[2] = {true, true};
        nf::adsAimingEyeError(7, ok, eyeW, true, cam, rHat, ipd, p14w, f, rows, k, e0, m0);
        nf::adsAimingEyeError(1, ok, eyeW, true, cam, rHat, ipd, p14w, f, rows, k, e1, m1);
        double mm0[3], mm1[3];
        const uint8_t s0 = nf::adsModelAimingEye(-3, cam, false, eyeW[0], rHat, ipd, mm0), s1 = nf::adsModelRightEye(cam, false, eyeW[0], rHat, ipd, mm1);
        check(sameErr(e0, e1) && same3(m0, m1) && s0 == s1 && same3(mm0, mm1), "F162: an aim other than 0 is the right eye");
    }
}

}  // namespace f162test
