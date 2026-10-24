// F89 checks (native on-foot VR): the weapon eye offset through the ADS release, the left eye closing in ADS, the ADS transition trace and the standing-height hold's open-loop
// input (run89 below): the pure functions of src\d3d11\native_on_foot_f88.h and _f89.h and src\common\left_eye_darken.h, on the plants the production code uses. Header only,
// run by native_frame_test.
#pragma once

#include "../../src/common/left_eye_darken.h"
#include "../../src/d3d11/native_on_foot_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace f89test {

namespace nf = edvr::nofoot;
namespace f87 = edvr::nofoot::f87;
namespace f88 = edvr::nofoot::f88;
namespace f89 = edvr::nofoot::f89;

struct Body {
    float f[9];
    double d[9];
};
// a character turned by `a` about the up axis: the rows (right, up, forward) in the world
inline Body bodyRows(double a) {
    Body b;
    const double c = std::cos(a), s = std::sin(a);
    const double r[9] = {c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c};
    for (int i = 0; i < 9; ++i) {
        b.d[i] = r[i];
        b.f[i] = static_cast<float>(r[i]);
    }
    return b;
}
inline double dist3(const double a[3], const double b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

// The ADS eye on the production functions: the eye is the character's eye with every shift at zero (E0) moved by the camera's image of the seated sum (adsCamShiftWorld); the
// sight lock measures the rendered eye `lag` ticks late (the runtime applies a shift late and the scene rows are a frame behind) and steps adsEyeStep toward P14w + k f.
struct AdsRig {
    Body body;
    double e0[3], p14w[3], f[3], k;
    double pos[3] = {}, vel[3] = {}, target[3] = {};
    std::vector<std::array<double, 3>> sums;   // the seated sum applied at the end of each tick (sight lock + w O)
    AdsRig(double yaw, double kM) : body(bodyRows(yaw)), k(kM) {
        const double e0w[3] = {1.0, 1.6, -3.0};
        for (int i = 0; i < 3; ++i) e0[i] = e0w[i];
        f[0] = body.d[6];
        f[1] = body.d[7];
        f[2] = body.d[8];
        // the target eye point: 1.2 cm right, 9 cm up, 22 cm forward of the character's eye (the sight line sits forward of and above the hip eye)
        const double want[3] = {0.012, 0.09, 0.22};
        double pt[3];
        for (int a = 0; a < 3; ++a) pt[a] = e0[a] + want[0] * body.d[a] + want[1] * body.d[3 + a] + want[2] * body.d[6 + a];
        for (int a = 0; a < 3; ++a) p14w[a] = pt[a] - k * f[a];
    }
    void eyeFor(const double seated[3], double out[3]) const {
        double shift[3];
        nf::adsCamShiftWorld(seated, body.f, shift);
        for (int i = 0; i < 3; ++i) out[i] = e0[i] + shift[i];
    }
    void targetEye(double out[3]) const {
        for (int i = 0; i < 3; ++i) out[i] = p14w[i] + k * f[i];
    }
    // one tick: the lock measures the eye it rendered `lag` ticks ago (the sum of that tick) and steps toward the sight (active) or toward zero (released)
    void tick(bool active, const double oSeated[3], int lag, double dt) {
        const int n = static_cast<int>(sums.size());
        const int j = n - 1 - lag;
        double seen[3] = {};
        if (j >= 0) {
            for (int i = 0; i < 3; ++i) seen[i] = sums[static_cast<size_t>(j)][static_cast<size_t>(i)];
        }
        double eye[3];
        eyeFor(seen, eye);
        double rows[9];
        for (int i = 0; i < 9; ++i) rows[i] = body.d[i];
        nf::AdsSightErr E;
        const bool have = nf::adsSightErrorEyeK(p14w, eye, f, rows, k, E);
        nf::adsEyeStep(pos, vel, target, E.seatedFull, have, active, false, dt);
        sums.push_back({pos[0] + oSeated[0], pos[1] + oSeated[1], pos[2] + oSeated[2]});
    }
    void eyeNow(double out[3]) const {
        const auto& s = sums.back();
        const double v[3] = {s[0], s[1], s[2]};
        eyeFor(v, out);
    }
};

template <class Check>
inline void run89(Check&& check) {
    const double dt = 1.0 / 90.0;
    // the pilot's F88 flight values
    f88::Keys pilot;
    pilot.rightCm = f88::kRightDefaultCm;
    pilot.upCm = f88::kUpDefaultCm;
    pilot.forwardCm = f88::kForwardDefaultCm;
    const auto isPlusZero = [](double v) { return v == 0.0 && !std::signbit(v); };

    // ---- the built-in values --------------------------------------------------------------------------------------------------------------------------
    {
        check(f88::kForwardDefaultCm == 5.0 && f88::kUpDefaultCm == 6.0 && f88::kRightDefaultCm == 0.5 && f87::kSettleDefaultS == 0.1 && f88::Keys{}.body == f88::kFollow,
              "F89: the built-in values are the pilot's: weapon eye forward 5.0, up 6.0, right 0.5 cm, body follow, standing height settle 0.1 s");
        check(pilot.rightCm >= f88::kRightMinCm && pilot.rightCm <= f88::kRightMaxCm && pilot.upCm >= f88::kUpMinCm && pilot.upCm <= f88::kUpMaxCm &&
                  pilot.forwardCm >= f88::kForwardMinCm && pilot.forwardCm <= f88::kForwardMaxCm && f88::clampCm(pilot.forwardCm, f88::kForwardMinCm, f88::kForwardMaxCm) == 5.0,
              "F89: the built-in values are inside the keys' ranges and the clamp leaves them as they are");
    }

    // ---- w does not depend on ADS ---------------------------------------------------------------------------------------------------------------------
    {
        f88::Weight w;
        double minInSights = 2.0, maxStep = 0.0, prev = 0.0;
        // 1 s at the hip, the sights up for 2 s, down for 2 s: the production gate is weightOn(pin, armed) and nothing else
        for (int i = 0; i < 90 * 5; ++i) {
            w.step(f88::weightOn(true, true), dt);
            if (i >= 90) minInSights = std::min(minInSights, w.w);
            maxStep = std::max(maxStep, std::fabs(w.w - prev));
            prev = w.w;
        }
        check(minInSights == 1.0 && w.w == 1.0 && maxStep <= dt / f88::kRampS + 1e-12,
              "F89: w stays 1 through the ADS press and the release: the gate has no ADS input, so w never leaves 1 once the weapon is drawn");
    }

    // ---- the ADS eye with O applied -------------------------------------------------------------------------------------------------------------------
    const double yaws[4] = {0.0, 0.6, 1.5707963267948966, -2.4};
    for (const int lag : {1, 2}) {
        bool landsWith = true, landsWithout = true, same = true, clamp = true;
        for (const double yaw : yaws) {
            AdsRig with(yaw, 0.15), without(yaw, 0.15);
            const f88::Out o = f88::make(pilot, 1.0);
            const f88::Out none = f88::make(f88::Keys{}, 1.0);
            // the hip first: 0.5 s with O in the sum and the lock idle (the lock steps toward zero)
            for (int i = 0; i < 45; ++i) {
                with.tick(false, o.seated, lag, dt);
                without.tick(false, none.seated, lag, dt);
            }
            for (int i = 0; i < 90 * 2; ++i) {   // the sights up for 2 s
                with.tick(true, o.seated, lag, dt);
                without.tick(true, none.seated, lag, dt);
            }
            double eyeW[3], eyeN[3], pt[3];
            with.eyeNow(eyeW);
            without.eyeNow(eyeN);
            with.targetEye(pt);
            landsWith = landsWith && dist3(eyeW, pt) < 1e-3;
            landsWithout = landsWithout && dist3(eyeN, pt) < 1e-3;
            same = same && dist3(eyeW, eyeN) < 1e-3;
            // the lock found O: its shift is the sight's less O's, and stays inside its clamp
            double lockW = std::sqrt(with.pos[0] * with.pos[0] + with.pos[1] * with.pos[1] + with.pos[2] * with.pos[2]);
            clamp = clamp && lockW < nf::kAdsSightClampM;
        }
        char what[256];
        std::snprintf(what, sizeof(what),
                      "F89: with O applied (w 1 in ADS) the sight lock still lands the eye on P14w + k f (within 1 mm, any yaw, the measurement %d tick%s late), exactly where it lands without O "
                      "(the lock's shift is the sight's less O's: O does not move the ADS eye)", lag, lag == 1 ? "" : "s");
        check(landsWith && landsWithout && same && clamp, what);
    }

    // ---- the release: from the sight to the offset eye, straight -------------------------------------------------------------------------------------------
    {
        const double yaw = 0.6;
        const f88::Out o = f88::make(pilot, 1.0);
        AdsRig rig(yaw, 0.15);
        for (int i = 0; i < 45; ++i) rig.tick(false, o.seated, 1, dt);
        for (int i = 0; i < 90 * 2; ++i) rig.tick(true, o.seated, 1, dt);
        double sightEye[3], offsetEye[3], un[3];
        rig.eyeNow(sightEye);
        const double zero[3] = {};
        rig.eyeFor(o.seated, offsetEye);   // the offset eye: the character's eye moved by w O, nothing else
        rig.eyeFor(zero, un);              // the un-offset eye
        const double oLen = dist3(offsetEye, un);
        // release: the sight lock runs down, w stays 1
        bool monotone = true, noPause = true;
        double minToUn = 1e9, prevToOffset = dist3(sightEye, offsetEye), prevEye[3] = {sightEye[0], sightEye[1], sightEye[2]};
        int slowRun = 0, slowRunMax = 0;
        double finalErr = 0.0;
        for (int i = 0; i < 90 * 2; ++i) {
            rig.tick(false, o.seated, 1, dt);
            double e[3];
            rig.eyeNow(e);
            const double toOffset = dist3(e, offsetEye);
            monotone = monotone && toOffset <= prevToOffset + 1e-9;
            minToUn = std::min(minToUn, dist3(e, un));
            const double step = dist3(e, prevEye);
            if (toOffset > 0.02 && step < 3e-4) ++slowRun; else slowRun = 0;
            slowRunMax = std::max(slowRunMax, slowRun);
            prevToOffset = toOffset;
            for (int a = 0; a < 3; ++a) prevEye[a] = e[a];
            finalErr = toOffset;
        }
        noPause = slowRunMax < 10;
        check(monotone && finalErr < 1e-3, "F89: on release the eye goes from the sight to the offset eye with its distance to the offset eye never growing, and arrives within 1 mm");
        check(minToUn >= oLen - 1e-6 && oLen > 0.07 && noPause,
              "F89: ...it never dips to the un-offset eye (its closest approach is the offset eye's own 7.8 cm) and never pauses (no 10 ticks of under 0.3 mm a tick while more than 2 cm from the offset eye)");

        // the control: F88's gate. w was 0 in ADS and stayed 0 until the journal's flag cleared (modelled as 0.6 s after the release), then ramped up over 0.25 s
        AdsRig old(yaw, 0.15);
        const f88::Out none = f88::make(f88::Keys{}, 1.0);
        for (int i = 0; i < 45; ++i) old.tick(false, none.seated, 1, dt);
        for (int i = 0; i < 90 * 2; ++i) old.tick(true, none.seated, 1, dt);   // in the sights w was 0: O did not enter
        double oldEye[3];
        old.eyeNow(oldEye);
        f88::Weight wOld;
        double oldMinToUn = 1e9, oldPrev[3] = {oldEye[0], oldEye[1], oldEye[2]};
        int oldSlow = 0, oldSlowMax = 0;
        for (int i = 0; i < 90 * 2; ++i) {
            wOld.step(i >= static_cast<int>(0.6 / dt), dt);
            const f88::Out ow = f88::make(pilot, wOld.w);
            old.tick(false, ow.seated, 1, dt);
            double e[3];
            old.eyeNow(e);
            oldMinToUn = std::min(oldMinToUn, dist3(e, un));
            const double toOffset = dist3(e, offsetEye);
            const double step = dist3(e, oldPrev);
            if (toOffset > 0.02 && step < 3e-4) ++oldSlow; else oldSlow = 0;
            oldSlowMax = std::max(oldSlowMax, oldSlow);
            for (int a = 0; a < 3; ++a) oldPrev[a] = e[a];
        }
        check(oldMinToUn < 0.015 && oldSlowMax >= 10,
              "F89: the control, F88's gate (w 0 in the sights and until the journal's flag clears 0.6 s after the release): the eye falls to within 1.5 cm of the un-offset eye and then pauses, "
              "which is the reported dip and pause, and the checks above fail on it");
    }

    // ---- the pin in the sights and at the hip ----------------------------------------------------------------------------------------------------------------
    {
        bool stayOk = true, followOk = true, noOStay = true;
        const double head[3] = {3.0, 1.6, -2.0};
        const double tSeat[3] = {0.03, -0.08, 0.15};   // what the sights need: the camera's whole seated shift in ADS
        for (const double yaw : yaws) {
            const Body b = bodyRows(yaw);
            const auto pinOf = [&](const double total[3], const double ignored[3]) {
                double shift[3], cam[3];
                nf::adsCamShiftWorld(total, b.f, shift);
                for (int i = 0; i < 3; ++i) cam[i] = head[i] + shift[i];
                const float camF[3] = {float(cam[0]), float(cam[1]), float(cam[2])};
                float out[3];
                nf::adsUnlockedCam(camF, ignored, b.f, out);
                return std::array<double, 3>{double(out[0]), double(out[1]), double(out[2])};
            };
            for (int mode = 0; mode < 2; ++mode) {
                f88::Keys k = pilot;
                k.body = mode == 0 ? f88::kStay : f88::kFollow;
                const f88::Out o = f88::make(k, 1.0);
                // at the hip: the camera moves by w O, the pin ignores its share
                const auto hip = pinOf(o.seated, o.ignore);
                // in the sights: the lock's shift is the sight's less O's, so the camera's whole shift is the sight's; the pin ignores the lock's shift and its share of O
                double lockShift[3], total[3], ignored[3];
                for (int i = 0; i < 3; ++i) {
                    lockShift[i] = tSeat[i] - o.seated[i];
                    total[i] = lockShift[i] + o.seated[i];
                    ignored[i] = lockShift[i] + o.ignore[i];
                }
                const auto sights = pinOf(total, ignored);
                // and the sights without O at all (the lock's shift is the whole of it)
                const auto noO = pinOf(tSeat, tSeat);
                double dHipSights = 0.0, dNoO = 0.0;
                for (int a = 0; a < 3; ++a) {
                    dHipSights = std::max(dHipSights, std::fabs(hip[static_cast<size_t>(a)] - sights[static_cast<size_t>(a)]));
                    dNoO = std::max(dNoO, std::fabs(sights[static_cast<size_t>(a)] - noO[static_cast<size_t>(a)]));
                }
                if (mode == 0) {
                    stayOk = stayOk && dHipSights < 4e-6;
                    noOStay = noOStay && dNoO < 4e-6;   // stay: the pin's demand in the sights is what it would be with no O at all
                } else {
                    // follow: the pin moves with O's horizontal part, the same at the hip and in the sights, and by nothing vertical
                    double dBody[3];
                    for (int a = 0; a < 3; ++a) {
                        dBody[a] = 0.0;
                        for (int c = 0; c < 3; ++c) dBody[a] += (sights[static_cast<size_t>(c)] - noO[static_cast<size_t>(c)]) * b.d[static_cast<size_t>(3 * a + c)];
                    }
                    followOk = followOk && dHipSights < 4e-6 && std::fabs(dBody[0] - 0.005) < 4e-6 && std::fabs(dBody[1]) < 4e-6 && std::fabs(dBody[2] - 0.05) < 4e-6;
                }
            }
        }
        check(stayOk && noOStay, "F89: with body stay the pin's demand in the sights is the same as at the hip and the same as with no O at all: the pin ignores the lock's shift and all of w O, for any yaw");
        check(followOk, "F89: with body follow the pin's demand in the sights is the same as at the hip: it moves with O's right 0.5 and forward 5 cm (the body's axes) and never by the up 6 cm, for any yaw");
    }

    // ---- the hold's input in the sights is the head's posture -----------------------------------------------------------------------------------------------
    {
        bool same = true;
        for (const double w : {0.0, 1.0})
            for (const double lock : {0.0, -0.11}) {
                const f88::Out o = f88::make(pilot, w);
                const double headRaw = -0.07, drop = -0.02;
                const double other = f87::eyeAlignY(0.004, lock, 0.0123, -0.0257, 0.031);
                const double total = other + o.seated[1];
                const double renderY = headRaw - total;
                same = same && std::fabs(f87::devOf(renderY, total, drop) - (headRaw - drop)) < 1e-12;
            }
        check(same, "F89: the hold's input is the head's height less the pivot's drop with O in the sights as at the hip, with the ADS lock's shift in the sum or not");
    }

    // ---- the hold's input is open-loop ------------------------------------------------------------------------------------------------------------------
    // The runtime applies a published alignment `delay` ticks later than the tick after it is published (0: the pose of tick i carries what tick i-1 published).
    struct HoldRun {
        std::vector<double> a, x;
        uint32_t lagMax = 0, missed = 0, legacy = 0;
    };
    const auto runHold = [&](int delay, double settleS, bool matched, bool legacyWait, int ticks, const auto& posture, const auto& oUp) {
        HoldRun r;
        f89::AlignHistory hist;
        f89::HoldFeed feed;
        f87::HeightHold hold;
        f88::Settle settle;
        std::vector<uint32_t> serialEnd(static_cast<size_t>(ticks), 0u);
        std::vector<double> yEnd(static_cast<size_t>(ticks), 0.0);
        double pubNow = 0.0;
        uint32_t serial = 0;
        for (int i = 0; i < ticks; ++i) {
            const int j = i - 1 - delay;
            const uint32_t sApplied = j >= 0 ? serialEnd[static_cast<size_t>(j)] : 0u;
            const double yApplied = j >= 0 ? yEnd[static_cast<size_t>(j)] : 0.0;   // what the pose really carries
            const double renderY = posture(i) - yApplied;
            const f89::Applied ap = hist.applied(f89::appliedWord(sApplied));
            const f89::HoldFeed::Out in0 = feed.step(true, renderY, 0.0, matched ? ap : f89::Applied{}, pubNow);
            r.lagMax = std::max(r.lagMax, matched ? ap.lag : 0u);
            r.missed += in0.source == f89::Source::kMissed ? 1u : 0u;
            r.legacy += in0.source == f89::Source::kLegacy ? 1u : 0u;
            const bool settling = settle.step(oUp(i), dt);
            f87::HoldIn in;
            in.scope = true;
            in.dt = dt;
            in.settleS = settleS;
            in.devValid = in0.devValid;
            in.devM = in0.dev;
            in.frozen = in0.wait || (in0.source == f89::Source::kLegacy && legacyWait && settling);
            const f87::HoldOut out = hold.step(in);
            const double total = out.eyeY + oUp(i);
            if (std::fabs(total - pubNow) > 1e-6) {
                pubNow = total;
                ++serial;
                const double sent = static_cast<double>(static_cast<float>(total));
                hist.record(serial, sent);
            }
            serialEnd[static_cast<size_t>(i)] = serial;
            yEnd[static_cast<size_t>(i)] = static_cast<double>(static_cast<float>(pubNow));   // the float the runtime would apply of the newest published
            r.a.push_back(total);
            r.x.push_back(out.sustainedM);
        }
        return r;
    };
    const auto noO = [](int) { return 0.0; };
    const auto slump = [](int i) { return i < 90 ? 0.0 : -0.07; };   // a 7 cm slump at 1 s, held
    {
        // settle 0: the view's height is held hard. The target is the posture itself.
        bool oneStep = true, noOvershoot = true;
        for (const int delay : {0, 1, 2}) {
            const HoldRun r = runHold(delay, 0.0, true, false, 90 * 4, slump, noO);
            oneStep = oneStep && std::fabs(r.a[90] - (-0.07)) < 5e-4;   // the first tick that sees the slump publishes the whole of it (to a millimetre)
            for (size_t i = 90; i < r.a.size(); ++i) noOvershoot = noOvershoot && std::fabs(r.a[i] - (-0.07)) < 5e-4;
            for (size_t i = 0; i < 90; ++i) noOvershoot = noOvershoot && r.a[i] == 0.0;
        }
        check(oneStep && noOvershoot,
              "F89: settle 0 with the alignment taken out by its serial: a 7 cm slump is published in full by the first tick that sees it, and stays there (never more than half a millimetre off, no overshoot, no ringing) with the runtime applying it 0, 1 and 2 ticks late");
        // the control: the last published alignment taken out (F88): at 1 and 2 ticks late the hold's own output is in its input and it does not settle
        bool rings = true;
        for (const int delay : {1, 2}) {
            const HoldRun r = runHold(delay, 0.0, false, false, 90 * 4, slump, noO);
            double lo = 1e9, hi = -1e9;
            for (size_t i = 90 * 3; i < r.a.size(); ++i) {
                lo = std::min(lo, r.a[i]);
                hi = std::max(hi, r.a[i]);
            }
            rings = rings && (hi - lo) > 0.005;
        }
        const HoldRun zero = runHold(0, 0.0, false, false, 90 * 4, slump, noO);
        check(rings && std::fabs(zero.a.back() - (-0.07)) < 5e-4,
              "F89: the control, the last published alignment taken out (F88): with no delay it settles, and with the runtime 1 or 2 ticks late the settle-0 loop (gain 1 and a delay) never settles: its output keeps moving by more than 5 mm in the last second");
    }
    {
        // settle 0.1: the same first-order response whatever the delay. The reference is the filter on the posture alone (no loop). A 2 mm step is under the rate cap,
        // so the response is the critically damped one exactly; a 7 cm slump is the rate-limited one.
        const double w95 = f87::kHoldCritical95 / 0.1;
        for (const double step : {-0.002, -0.07}) {
            const auto posture = [step](int i) { return i < 90 ? 0.0 : step; };
            f87::HeightHold ref;
            f87::HoldIn in;
            in.scope = true;
            in.dt = dt;
            in.settleS = 0.1;
            in.devValid = true;
            std::vector<double> refX;
            for (int i = 0; i < 90 * 4; ++i) {
                in.devM = posture(i);
                refX.push_back(ref.step(in).sustainedM);
            }
            bool equal = true, analytic = true;
            double maxDiff = 0.0;
            for (const int delay : {0, 1, 2}) {
                const HoldRun r = runHold(delay, 0.1, true, false, 90 * 4, posture, noO);
                for (size_t i = 0; i < r.x.size(); ++i) {
                    maxDiff = std::max(maxDiff, std::fabs(r.x[i] - refX[i]));
                    if (step == -0.002 && i >= 90) {
                        const double t = static_cast<double>(i - 90 + 1) * dt;   // the filter has taken i - 90 + 1 steps since the posture changed
                        const double want = step * (1.0 - (1.0 + w95 * t) * std::exp(-w95 * t));
                        analytic = analytic && std::fabs(r.x[i] - want) < 1e-9;
                    }
                }
            }
            equal = maxDiff < 1e-9;
            char what[256];
            std::snprintf(what, sizeof(what),
                          "F89: settle 0.1 %s: the hold's sustained part is the filter on the posture alone, the same to 1e-9 with the runtime applying the alignment 0, 1 and 2 ticks late%s",
                          step == -0.002 ? "(a 2 mm step, under the rate cap)" : "(a 7 cm slump, rate-limited at 4 cm/s)",
                          step == -0.002 ? ", and equal to 1 - (1 + wt) exp(-wt) of the step at every tick" : "");
            check(equal && analytic, what);
        }
    }
    {
        // The F88 wait: O's up ramps with the weapon draw while the hold is settling; the runtime applies it 2 ticks late. With the alignment matched by serial the hold needs no wait.
        const int lag = 2;
        const auto posture = [](int) { return -0.07; };
        f88::Weight w;
        std::vector<double> oUpTrace;
        for (int i = 0; i < 90 * 8; ++i) {
            w.step(f88::weightOn(true, i >= 90 * 4), dt);
            f88::Keys k = pilot;
            k.upCm = 10.0;
            oUpTrace.push_back(f88::make(k, w.w).seated[1]);
        }
        const auto oUp = [&](int i) { return oUpTrace[static_cast<size_t>(i)]; };
        double dMatched = 0.0, dLegacyWait = 0.0, dLegacyFree = 0.0;
        const HoldRun still = runHold(lag, 3.0, true, false, 90 * 8, posture, noO);   // the same hold with no O at all: its own slow settle only
        for (int mode = 0; mode < 3; ++mode) {
            const HoldRun r = runHold(lag, 3.0, mode == 0, mode == 1, 90 * 8, posture, oUp);
            double x0 = r.x[90 * 4 - 1], dev = 0.0;
            if (mode == 0) {   // matched: the sustained part is the one without O, tick for tick
                for (size_t i = 0; i < r.x.size(); ++i) dev = std::max(dev, std::fabs(r.x[i] - still.x[i]));
            } else {           // a runtime that does not report: the sustained part against where it stood when O began to move
                for (int i = 90 * 4; i < 90 * 4 + 40; ++i) dev = std::max(dev, std::fabs(r.x[static_cast<size_t>(i)] - x0));
            }
            (mode == 0 ? dMatched : mode == 1 ? dLegacyWait : dLegacyFree) = dev;
        }
        check(dMatched < 1e-12 && dLegacyWait == 0.0 && dLegacyFree > 0.001,
              "F89: O's up ramping (the weapon drawn) with the runtime 2 ticks late: matched by serial the hold's sustained part is exactly the one it has with no O, with no wait (F88's wait is "
              "unnecessary there); a runtime that does not report keeps F88's wait and holds; without the wait it would move by more than a millimetre");
    }
    {
        // the input's pieces: a serial the history no longer has, serial 0, no report, and the second's figures
        f89::AlignHistory hist;
        for (uint32_t s = 1; s <= 3; ++s) hist.record(s, 0.01 * s);
        const f89::Applied a2 = hist.applied(f89::appliedWord(2)), a0 = hist.applied(f89::appliedWord(0)), none = hist.applied(0), future = hist.applied(f89::appliedWord(9));
        hist.record(2 + f89::kHistN, 0.5);   // overwrites serial 2's slot
        const f89::Applied gone = hist.applied(f89::appliedWord(2));
        check(a2.matched && a2.reported && std::fabs(a2.y - 0.02) < 1e-15 && a2.lag == 1 && a0.matched && a0.y == 0.0 && a0.reported && !none.reported && !none.matched && !future.matched &&
                  future.reported && !gone.matched && gone.reported,
              "F89: the history finds an applied serial's y, reads serial 0 as the zero alignment, reads word 0 as not reported, and a serial it no longer has (overwritten or never published) as not matched");
        f89::HoldFeed feed;
        const f89::HoldFeed::Out ok = feed.step(true, -0.05, -0.01, a2, 0.123);
        const f89::HoldFeed::Out leg = feed.step(true, -0.05, -0.01, none, 0.123);
        const f89::HoldFeed::Out miss = feed.step(true, -0.05, -0.01, gone, 0.123);
        f89::HoldFeed cold;
        const f89::HoldFeed::Out missCold = cold.step(true, -0.05, -0.01, gone, 0.123);
        check(ok.devValid && std::fabs(ok.dev - (-0.05 + 0.02 + 0.01)) < 1e-12 && ok.source == f89::Source::kMatched && !ok.wait && leg.devValid &&
                  std::fabs(leg.dev - (-0.05 + 0.123 + 0.01)) < 1e-12 && leg.source == f89::Source::kLegacy && !leg.wait && miss.wait && miss.devValid && miss.dev == leg.dev &&
                  miss.source == f89::Source::kMissed && missCold.wait && !missCold.devValid,
              "F89: the hold's input takes out the applied alignment when matched, the last published one when the runtime does not report, and waits on its last good input (none yet: no input) when the serial is gone");
        f89::HoldAlignSecond sec;
        sec.generationSeen(1);
        sec.add(f89::Source::kMatched, 2);
        sec.add(f89::Source::kMatched, 0);
        sec.add(f89::Source::kMissed, 0);
        sec.add(f89::Source::kLegacy, 0);
        sec.addResidual(-0.1000, -0.1000);
        sec.addResidual(-0.1001, -0.0700);
        sec.addResidual(std::numeric_limits<double>::quiet_NaN(), 0.0);
        check(sec.n == 4 && sec.matched == 2 && sec.missed == 1 && sec.legacy == 1 && sec.lagMax == 2 && std::fabs(sec.lagMean() - 1.0) < 1e-12 && sec.residN == 2 &&
                  std::fabs(sec.newSpreadMm() - 0.1) < 1e-9 && std::fabs(sec.oldSpreadMm() - 30.0) < 1e-9,
              "F89: the hold line's second counts matched, not found and unreported frames, the serial lag, and the residual of the input: 0.1 mm taking the applied alignment out, 30 mm taking the last published out");
        sec.generationSeen(2);
        check(sec.residN == 0 && sec.newSpreadMm() == 0.0, "F89: a new reference generation (the seated origin moved) starts the residual again");
    }

    // ---- the left eye closing in ADS ---------------------------------------------------------------------------------------------------------------------
    {
        bool follows = true, outside = true, keyOff = true, exactZero = true;
        for (int i = 0; i <= 20; ++i) {
            const double lock = i / 20.0;
            const f89::Darken on = f89::darken(true, true, true, lock);
            follows = follows && on.level == lock && !on.tripped;
            const f89::Darken off = f89::darken(false, true, true, lock);
            keyOff = keyOff && isPlusZero(off.level) && !off.tripped;
        }
        for (const bool key : {false, true}) {
            const f89::Darken d0 = f89::darken(key, true, true, 0.0);   // outside ADS the lock weight is 0
            outside = outside && isPlusZero(d0.level) && !d0.tripped;
            exactZero = exactZero && isPlusZero(f89::darken(key, false, false, 0.0).level) && isPlusZero(f89::darken(key, true, false, -0.0).level);
        }
        check(follows && outside && keyOff && exactZero,
              "F89: the left eye's darkening is the ADS lock's weight with the key on, in native on foot with a weapon drawn; it is an exact 0 outside ADS (weight 0) and with the key off, whatever the weight");
        bool guard = true;
        for (const bool onFoot : {false, true})
            for (const bool armed : {false, true}) {
                if (onFoot && armed) continue;
                const f89::Darken d = f89::darken(true, onFoot, armed, 0.7);
                guard = guard && isPlusZero(d.level) && d.tripped;
                const f89::Darken quiet = f89::darken(true, onFoot, armed, 0.0);
                guard = guard && isPlusZero(quiet.level) && !quiet.tripped;
            }
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const f89::Darken nanW = f89::darken(true, true, true, nan), negW = f89::darken(true, true, true, -0.3), bigW = f89::darken(true, true, true, 4.0);
        check(guard && isPlusZero(nanW.level) && isPlusZero(negW.level) && bigW.level == 1.0,
              "F89: the guard forces 0 and reports a trip when the lock's weight is above 0 with no weapon drawn or off foot; a weight that is not a number or is negative reads 0, above 1 reads 1");
        // the real ramp: 0.15 s in, 0.3 s out, monotone each way; the key off in the sights opens the eye at once
        nf::AdsRamp ramp;
        double reachedAt = -1.0, prev = 0.0, outAt = -1.0;
        bool monotone = true, never = true;
        for (int i = 0; i < 90; ++i) {
            ramp.step(true, dt);
            const double lv = f89::darken(true, true, true, ramp.w).level;
            monotone = monotone && lv >= prev;
            if (reachedAt < 0.0 && lv >= 1.0) reachedAt = (i + 1) * dt;
            prev = lv;
        }
        for (int i = 0; i < 90; ++i) {
            ramp.step(false, dt);
            const double lv = f89::darken(true, true, true, ramp.w).level;
            monotone = monotone && lv <= prev;
            if (outAt < 0.0 && lv <= 0.0) outAt = (i + 1) * dt;
            prev = lv;
        }
        for (int i = 0; i < 90; ++i) {
            ramp.step(false, dt);
            never = never && f89::darken(true, true, true, ramp.w).level == 0.0;
        }
        nf::AdsRamp held;
        held.w = 1.0;
        const double keyDrop = f89::darken(false, true, true, held.w).level;
        check(monotone && reachedAt > 0.14 && reachedAt < 0.17 && outAt > 0.29 && outAt < 0.32 && never && keyDrop == 0.0,
              "F89: through a press and a release the darkening rises to 1 in 0.15 s and falls to 0 in 0.3 s, monotone each way, stays 0 afterwards, and the key switched off in the sights opens the eye at once");
    }
    {
        // the signal between the halves: the level, its wire form and its staleness
        namespace ed = edvr::eyedarken;
        ed::clear();
        const float unpublished = ed::read(1000);
        ed::publish(0.6f, 1000);
        const float fresh = ed::read(1100), staleHold = ed::read(1000 + edvr::comfort::kStaleHoldMs), decaying = ed::read(1000 + edvr::comfort::kStaleHoldMs + 50),
                    gone = ed::read(1000 + edvr::comfort::kStaleHoldMs + edvr::comfort::kStaleDecayMs);
        ed::publish(std::numeric_limits<float>::quiet_NaN(), 2000);
        const float nanLevel = ed::read(2000);
        ed::publish(-0.5f, 3000);
        const float negLevel = ed::read(3000);
        ed::publish(7.0f, 4000);
        const float bigLevel = ed::read(4000);
        ed::publish(0.0f, 5000);
        const float zeroLevel = ed::read(5000);
        ed::clear();
        check(unpublished == 0.0f && fresh == 0.6f && staleHold == 0.6f && decaying > 0.0f && decaying < 0.6f && gone == 0.0f && nanLevel == 0.0f && negLevel == 0.0f && bigLevel == 1.0f &&
                  zeroLevel == 0.0f && ed::read(5000) == 0.0f,
              "F89: the left eye's signal reads 0 until published, the level while fresh, decays to 0 when the frame thread goes quiet (0.2 s hold, 0.1 s decay), and reads NaN and negative as 0, above 1 as 1");
        check(ed::fromBits(ed::toBits(0.6f)) == 0.6f && ed::fromBits(ed::toBits(1.0f)) == 1.0f && ed::fromBits(0u) == 0.0f && ed::fromBits(0x7FC00000u) == 0.0f && ed::fromBits(0xBF800000u) == 0.0f &&
                  ed::fromBits(0x40000000u) == 1.0f && ed::toBits(std::numeric_limits<float>::quiet_NaN()) == 0u,
              "F89: the wire form is the float's bits: 0.6 and 1 survive, a slot that is zero, NaN or negative reads 0, and 2.0 reads 1");
    }

    // ---- the transition trace ----------------------------------------------------------------------------------------------------------------------------
    {
        f89::AdsTrace tr;
        uint64_t ms = 100000;
        std::vector<int> lines;   // traced frames per event
        std::vector<int> kinds;   // 1 press, 0 release
        int cur = -1;
        uint32_t started = 0, supp = 0;
        const auto frames = [&](bool active, int n) {
            for (int i = 0; i < n; ++i) {
                ms += 11;
                const f89::TraceFrame f = tr.step(active, ms, dt);
                if (f.started) {
                    lines.push_back(0);
                    kinds.push_back(f.press ? 1 : 0);
                    cur = static_cast<int>(lines.size()) - 1;
                    ++started;
                }
                if (f.suppressed) ++supp;
                if (f.trace && cur >= 0) ++lines[static_cast<size_t>(cur)];
            }
        };
        frames(false, 90);
        frames(true, 90 * 3);    // press (event 1)
        frames(false, 90 * 3);   // release (event 2)
        frames(true, 90 * 3);    // press (event 3)
        frames(false, 90 * 3);   // release (event 4)
        frames(true, 90 * 2);    // press: the fifth event in under a minute (not traced)
        frames(false, 90 * 2);   // release: not traced
        const bool capped = started == 4 && supp == 2 && tr.suppressed == 2;
        bool windows = lines.size() == 4;
        for (const int n : lines) windows = windows && n >= 135 && n <= 137;   // 1.5 s at 90 Hz: 136 frames, the edge's own included
        const bool order = kinds.size() == 4 && kinds[0] == 1 && kinds[1] == 0 && kinds[2] == 1 && kinds[3] == 0;
        check(capped && windows && order,
              "F89: the ADS transition trace writes one line a frame for 1.5 s after each press and each release (136 frames at 90 Hz), at most 4 events in a minute: the fifth and sixth are counted and not traced");
        // a minute after the first traced event the cap has room again
        for (int i = 0; i < 90 * 60; ++i) {
            ms += 11;
            tr.step(false, ms, dt);
        }
        ms += 11;
        const f89::TraceFrame again = tr.step(true, ms, dt);
        check(again.started && again.press && again.trace && again.t == 0.0 && tr.count <= f89::kTraceMaxPerMinute,
              "F89: ...and a minute later the next event is traced again, its first line at t = +0.000 s");
        f89::EyeStep es;
        const double p0[3] = {0.0, 0.0, 0.0}, p1[3] = {0.03, 0.04, 0.0}, p2[3] = {0.03, 0.04, 0.12};
        const double s0 = es.step(p0), s1 = es.step(p1), s2 = es.step(p2);
        es.drop();
        const double s3 = es.step(p0);
        check(s0 == 0.0 && std::fabs(s1 - 5.0) < 1e-9 && std::fabs(s2 - 12.0) < 1e-9 && s3 == 0.0,
              "F89: the eye step is the frame-to-frame motion of the eye in the character's frame in centimetres, 0 for the first sample and after a gap");
    }
}

}  // namespace f89test
