// F186 checks (native on-foot VR), run186 below. Header only, run by native_frame_test.
//
// F186 (native_on_foot_f186.h, native_on_foot.cpp): the ADS lock spiral at a steep aim. (a) The sight lock's response measured: pairs of the change commanded (the shift each eye was rendered with, frame
// to frame) and the change of the measured error, fitted per aim-pitch bin by a complex map (gain, angle: a correct loop reads 1 and 0, the game's turn reads the aim pitch). (b) The pre-turn, behaviour
// since F187 (the A/B key experimental.native_on_foot_ads_preturn is retired): under mouse look the ADS terms of the eye alignment (the position lock, the sight lock, the weapon eye offset) go out turned
// by -a in the (forward, up) plane, as F172 turns the neck raise. (c) A stability stop: the lock's e after the lock over 10 cm, or its applied vector at its clamp, for 0.3 s: ramped to zero over 0.15 s and acquired again. The rig
// runs the pre-turn on toy vectors (the game's turn lands them back), the fit on synthetic pairs, a toy loop whose plant is the game's turn (it spirals unturned from 60 deg and settles in one
// step turned), the stop's timing, the lines, and the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f172.h"
#include "../../src/d3d11/native_on_foot_f186.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f186test {

namespace nf = edvr::nofoot;
namespace f172 = edvr::nofoot::f172;
namespace f186 = edvr::nofoot::f186;

constexpr double kPi = 3.14159265358979323846;
inline double rad(double deg) { return deg * kPi / 180.0; }

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline size_t countOf(const std::string& hay, const std::string& needle) {
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
    return n;
}
inline uint64_t next(uint64_t& s) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return s >> 11;
}
inline double unitRand(uint64_t& s) { return static_cast<double>(next(s) & 0xFFFFFFFFull) / 4294967296.0; }

template <class Check>
inline void run186(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[800];

    // ---- 1. the pre-turn ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double t[3] = {0.07, 0.12, 0.09};   // the ADS terms' seated sum: 7 cm right-ish, 12 cm (up is -y), 9 cm forward
        const f186::Turned level = f186::turnAds(t, false, true, 0.0, true);
        check(!level.turned && level.dy == 0.0 && level.dz == 0.0, "F186: at an aim pitch of 0 nothing is turned (the terms go out as they did, bit for bit)");
        bool lands = true;
        for (double deg : {10.0, 30.0, 60.0, 81.9, 85.2, -40.0, -85.0}) {
            const double a = rad(deg);
            const f186::Turned r = f186::turnAds(t, false, true, a, true);
            double yOut = 0.0, zOut = 0.0;
            f186::gameTurnSeated(a, t[1] + r.dy, t[2] + r.dz, &yOut, &zOut);
            lands = lands && r.turned && eq(yOut, t[1], 1e-12) && eq(zOut, t[2], 1e-12);
        }
        const f186::Turned r85 = f186::turnAds(t, false, true, rad(85.0), true);
        double ux = 0.0, uz = 0.0;
        f186::gameTurnSeated(rad(85.0), t[1], t[2], &ux, &uz);
        std::snprintf(msg, sizeof(msg), "F186: at an aim pitch of 85 deg the terms (y %.3f, z %.3f) go out as (y %+.3f, z %+.3f): the game's turn of that lands them on (y, z) again at every pitch tried (10, 30, 60, 81.9, 85.2, -40, -85); unturned they land at (y %+.3f, z %+.3f)",
                      t[1], t[2], t[1] + r85.dy, t[2] + r85.dz, ux, uz);
        check(lands && r85.turned && eq(r85.pitchRad, rad(85.0), 1e-15) && std::fabs(ux - t[1]) > 0.05, msg);
        // F172's raise is the same turn: a vector with only y reads F172's published y and z
        const double onlyY[3] = {0.0, -0.15, 0.0};
        const f186::Turned ry = f186::turnAds(onlyY, false, true, rad(60.0), true);
        const f172::Raise f172r = f172::publish(-0.15, false, true, rad(60.0), true);
        check(eq(onlyY[1] + ry.dy, f172r.y, 1e-15) && eq(onlyY[2] + ry.dz, f172r.z, 1e-15), "F186: the turn is F172's: a vector with only a y part comes out as F172 publishes the neck raise (y cos a, z -y sin a)");
        // the conditions: look to aim driving the aim, a stale aim, the camera not the commander's, no terms, a pitch that is not a number
        const double zero[3] = {0.0, 0.0, 0.0}, nan3[3] = {0.0, std::nan(""), 0.0};
        check(!f186::turnAds(t, true, true, rad(85.0), true).turned && !f186::turnAds(t, false, false, rad(85.0), true).turned && !f186::turnAds(t, false, true, rad(85.0), false).turned &&
                  !f186::turnAds(zero, false, true, rad(85.0), true).turned && !f186::turnAds(nan3, false, true, rad(85.0), true).turned && !f186::turnAds(t, false, true, std::nan(""), true).turned &&
                  f186::turnAds(t, false, true, rad(85.0), true).turned,
              "F186: look to aim driving the aim, a stale aim, off the commander's first-person camera, no terms, and figures that are not numbers go out unturned, as before; every other case is turned, with no key to switch it");
    }

    // ---- 2. the toy loop: the controller adds the error each frame, the plant is the game's turn -------------------------------------------------------------------------------------------------
    {
        // seated (y, z); e0 is the error with no shift (the eye 12 cm off the sight in y and 9 cm in z); the controller (F93b's) sets the shift to the shift the eye was rendered with plus the error
        // measured on it; the plant lands the published shift through the game's turn by a (unturned: the shift itself; pre-turned: the shift turned by -a first); the shift is clamped at 40 cm.
        const auto run = [&](double aimDeg, bool preturn, int steps, double* finalErr, int* stopAt, double* angleSpread, double* lastV) {
            const double a = rad(aimDeg), e0[2] = {0.12, 0.09};
            double v[2] = {0.0, 0.0};
            f186::StopGuard guard;
            *stopAt = -1;
            double minAng = 1e9, maxAng = -1e9;
            double err[2] = {e0[0], e0[1]};
            for (int n = 0; n < steps; ++n) {
                const double t[3] = {0.0, v[0], v[1]};
                const f186::Turned tr = preturn ? f186::turnAds(t, false, true, a, true) : f186::Turned{};
                const double pubY = v[0] + (tr.turned ? tr.dy : 0.0), pubZ = v[1] + (tr.turned ? tr.dz : 0.0);
                double ry = 0.0, rz = 0.0;
                f186::gameTurnSeated(a, pubY, pubZ, &ry, &rz);   // the shift as the game lands it
                err[0] = e0[0] - ry;                               // the error measured on the eye rendered with it
                err[1] = e0[1] - rz;
                const double vNorm = std::sqrt(v[0] * v[0] + v[1] * v[1]);
                const double sec = 0.011 * n;
                const double pos3[3] = {0.0, v[0], v[1]};
                const f186::StopGuard::Out so = guard.step(static_cast<uint64_t>(sec * 1000.0), true, true, std::sqrt(err[0] * err[0] + err[1] * err[1]), vNorm, nf::kAdsSightClampM, pos3);
                if (so.stopped && *stopAt < 0) *stopAt = n;
                v[0] += err[0];   // the controller: the shift rendered with plus the error
                v[1] += err[1];
                const double nrm = std::sqrt(v[0] * v[0] + v[1] * v[1]);
                if (nrm > nf::kAdsSightClampM) {
                    v[0] *= nf::kAdsSightClampM / nrm;
                    v[1] *= nf::kAdsSightClampM / nrm;
                }
                if (n >= steps - 12) {
                    const double ang = std::atan2(v[1], v[0]) * 180.0 / kPi;
                    minAng = std::fmin(minAng, ang);
                    maxAng = std::fmax(maxAng, ang);
                }
            }
            *finalErr = std::sqrt(err[0] * err[0] + err[1] * err[1]);
            *angleSpread = maxAng - minAng;
            *lastV = std::sqrt(v[0] * v[0] + v[1] * v[1]);
        };
        double e85off, e85on, e40off, e0off, spread85off, spread85on, spread40off, spread0off, v85off, v85on, v40off, v0off;
        int stop85off, stop85on, stop40off, stop0off;
        run(85.0, false, 120, &e85off, &stop85off, &spread85off, &v85off);
        run(85.0, true, 120, &e85on, &stop85on, &spread85on, &v85on);
        run(40.0, false, 120, &e40off, &stop40off, &spread40off, &v40off);
        run(0.0, false, 120, &e0off, &stop0off, &spread0off, &v0off);
        std::snprintf(msg, sizeof(msg), "F186: a toy loop whose plant is the game's turn: unturned at 85 deg the lock spirals out (the error stays %.1f cm after 120 frames, the shift at %.0f cm, its direction swinging %.0f deg) and the stop fires at frame %d; turned it "
                                        "settles (error %.2f cm, the shift %.1f cm, no stop); unturned at 40 deg (factor %.2f) and at 0 it settles too",
                      100.0 * e85off, 100.0 * v85off, spread85off, stop85off, 100.0 * e85on, 100.0 * v85on, 2.0 * std::sin(rad(20.0)));
        check(e85off > 0.05 && v85off > 0.3 && v85off <= nf::kAdsSightClampM + 1e-12 && spread85off > 20.0 && stop85off > 20 && stop85off < 60 && e85on < 1e-9 && eq(v85on, std::sqrt(0.12 * 0.12 + 0.09 * 0.09), 1e-9) && stop85on < 0 &&
                  e40off < 1e-6 && stop40off < 0 && e0off < 1e-9 && stop0off < 0,
              msg);
        // the response fit on the loop's own steps: turned the angle reads the aim pitch, pre-turned it reads 0
        for (int on = 0; on < 2; ++on) {
            const double a = rad(70.0), e0[2] = {0.12, 0.09};
            double v[2] = {0.01, -0.02};
            f186::Response resp;
            uint64_t s = 777;
            double prevV[2] = {v[0], v[1]}, prevE[2] = {0.0, 0.0};
            bool have = false;
            for (int n = 0; n < 200; ++n) {
                v[0] += 0.02 * (unitRand(s) - 0.5);   // excitation: the commanded change is not only the loop's own
                v[1] += 0.02 * (unitRand(s) - 0.5);
                const double t[3] = {0.0, v[0], v[1]};
                const f186::Turned tr = on == 1 ? f186::turnAds(t, false, true, a, true) : f186::Turned{};
                double ry = 0.0, rz = 0.0;
                f186::gameTurnSeated(a, v[0] + (tr.turned ? tr.dy : 0.0), v[1] + (tr.turned ? tr.dz : 0.0), &ry, &rz);
                const double e[2] = {e0[0] - ry, e0[1] - rz};
                if (have) resp.addPair(70.0, v[0] - prevV[0], v[1] - prevV[1], e[0] - prevE[0], e[1] - prevE[1]);
                have = true;
                prevV[0] = v[0]; prevV[1] = v[1]; prevE[0] = e[0]; prevE[1] = e[1];
            }
            const f186::Bin& b = resp.bin[2];
            std::snprintf(msg, sizeof(msg), "F186: the response fitted on that loop at 70 deg with the pre-turn %s reads gain %.3f angle %+.2f deg (%u pairs): %s", on ? "on" : "off", b.gain(), b.angleDeg(), b.n,
                          on ? "a correct loop: angle 0" : "the game's turn: the aim pitch");
            check(b.n > 150 && eq(b.gain(), 1.0, 1e-6) && eq(b.angleDeg(), on ? 0.0 : 70.0, 1e-6) && b.corr() > 0.999999, msg);
        }
    }

    // ---- 3. the response fit on synthetic pairs ---------------------------------------------------------------------------------------------------------------------------------------------
    {
        uint64_t s = 99;
        bool ok = true;
        double worstAngle = 0.0, worstGain = 0.0;
        for (double phi : {0.0, 40.0, 85.0, -30.0, 170.0}) {
            for (double g : {1.0, 0.6}) {
                f186::Response resp;
                for (int i = 0; i < 300; ++i) {
                    const double m = 0.004 + 0.03 * unitRand(s), ang = 2.0 * kPi * unitRand(s);
                    const double dy = m * std::cos(ang), dz = m * std::sin(ang);
                    const double c = std::cos(rad(phi)), sn = std::sin(rad(phi));
                    const double ry = -g * (dy * c - dz * sn) + 0.0002 * (unitRand(s) - 0.5), rz = -g * (dy * sn + dz * c) + 0.0002 * (unitRand(s) - 0.5);
                    resp.addPair(10.0, dy, dz, ry, rz);
                }
                const f186::Bin& b = resp.bin[0];
                double da = b.angleDeg() - phi;
                while (da > 180.0) da -= 360.0;
                while (da < -180.0) da += 360.0;
                worstAngle = std::fmax(worstAngle, std::fabs(da));
                worstGain = std::fmax(worstGain, std::fabs(b.gain() - g));
                ok = ok && b.n == 300 && b.corr() > 0.99;
            }
        }
        std::snprintf(msg, sizeof(msg), "F186: on synthetic pairs r = -g R(phi) d (phi 0, 40, 85, -30, 170 deg; g 1 and 0.6; 300 pairs, a little noise) the fit reads the angle within %.2f deg and the gain within %.3f", worstAngle, worstGain);
        check(ok && worstAngle < 0.5 && worstGain < 0.01, msg);
        // the bins by the aim's pitch, up or down; a step under 1 mm or over 10 cm and figures that are not numbers are no pair
        f186::Response r;
        check(f186::binOf(0.0) == 0 && f186::binOf(29.9) == 0 && f186::binOf(30.0) == 1 && f186::binOf(59.9) == 1 && f186::binOf(60.0) == 2 && f186::binOf(74.9) == 2 && f186::binOf(75.0) == 3 &&
                  f186::binOf(85.2) == 3 && f186::binOf(-80.0) == 3 && f186::binOf(-10.0) == 0 && f186::kBins == 4,
              "F186: the bins are the aim's pitch under 30, 30-60, 60-75 and over 75 deg, down as up");
        check(!r.addPair(10.0, 0.0005, 0.0005, 0.001, 0.001) && !r.addPair(10.0, 0.2, 0.0, 0.1, 0.0) && !r.addPair(std::nan(""), 0.01, 0.0, 0.01, 0.0) && !r.addPair(10.0, 0.01, 0.0, std::nan(""), 0.0) && r.pairs == 0 &&
                  r.addPair(10.0, 0.01, 0.0, -0.01, 0.0) && r.pairs == 1,
              "F186: a commanded change under 1 mm or over 10 cm and figures that are not numbers make no pair");
        // the pairer: consecutive rendered frames only
        f186::Pairer p;
        double dv[3], de[3];
        const double v1[3] = {0.0, 0.01, 0.02}, e1[3] = {0.0, 0.05, 0.04}, v2[3] = {0.0, 0.02, 0.02}, e2[3] = {0.0, 0.04, 0.04};
        check(!p.step(100, v1, e1, dv, de) && p.step(101, v2, e2, dv, de) && eq(dv[1], 0.01, 1e-15) && eq(de[1], -0.01, 1e-15) && !p.step(103, v1, e1, dv, de) && p.step(104, v2, e2, dv, de) && (p.reset(), !p.step(105, v1, e1, dv, de)),
              "F186: a pair is the change from the rendered frame before to this one; a skipped frame or a reset starts the pairs again");
        // the merge
        f186::Response a, b2;
        a.addPair(10.0, 0.01, 0.0, -0.01, 0.0);
        b2.addPair(80.0, 0.0, 0.01, 0.0, -0.01);
        b2.lockFrames = 5; b2.stops = 1; b2.turnedFrames = 3;
        a.merge(b2);
        check(a.pairs == 2 && a.bin[0].n == 1 && a.bin[3].n == 1 && a.lockFrames == 5 && a.stops == 1 && a.turnedFrames == 3, "F186: two windows add up");
    }

    // ---- 4. the stability stop's timing ----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double pos[3] = {0.07, 0.30, -0.10};
        f186::StopGuard g;
        f186::StopGuard::Out o;
        bool early = false;
        for (int ms = 0; ms < 300; ms += 10) {   // e after the lock 12 cm for 0.29 s: not yet
            o = g.step(1000 + static_cast<uint64_t>(ms), true, true, 0.12, 0.30, nf::kAdsSightClampM, pos);
            early = early || o.stopped;
        }
        const f186::StopGuard::Out at = g.step(1300, true, true, 0.12, 0.30, nf::kAdsSightClampM, pos);
        check(!early && at.stopped && at.reason == f186::kReasonError && at.ramping && at.scale == 1.0 && g.stops == 1 && std::strcmp(f186::reasonName(at.reason), "e after the lock over 10 cm") == 0,
              "F186: e after the lock over 10 cm for 0.29 s does not stop the lock; at 0.30 s it does (reason: the error), and the stop is counted");
        // the ramp: linear to zero over 0.15 s from the vector it began with, then zero whole, then watching again
        const f186::StopGuard::Out r1 = g.step(1300 + 50, true, true, 0.0, 0.0, nf::kAdsSightClampM, pos), r2 = g.step(1300 + 100, true, true, 0.0, 0.0, nf::kAdsSightClampM, pos),
                                   r3 = g.step(1300 + 150, true, true, 0.0, 0.0, nf::kAdsSightClampM, pos);
        check(r1.ramping && eq(r1.scale, 1.0 - 50.0 / 150.0, 1e-12) && r2.ramping && eq(r2.scale, 1.0 - 100.0 / 150.0, 1e-12) && r3.zeroNow && r3.scale == 0.0 && !r3.ramping && g.start[1] == 0.30 && !g.releasing,
              "F186: the lock is ramped to zero over 0.15 s from where it was (the scale 1, 0.67, 0.33 at 0, 50 and 100 ms) and then zeroed whole: acquired again from zero");
        const f186::StopGuard::Out after = g.step(1500, true, true, 0.005, 0.05, nf::kAdsSightClampM, pos);
        check(!after.stopped && !after.ramping && !after.zeroNow && after.scale == 1.0, "F186: after the release the lock is watched afresh");
        // one good measured frame starts the clock again; frames with no measurement leave the condition as it was; the clamp is a reason of its own; both
        f186::StopGuard g2;
        bool any = false;
        for (int ms = 0; ms < 250; ms += 10) any = any || g2.step(2000 + static_cast<uint64_t>(ms), true, true, 0.20, 0.30, nf::kAdsSightClampM, pos).stopped;
        g2.step(2250, true, true, 0.02, 0.10, nf::kAdsSightClampM, pos);   // one good frame
        for (int ms = 260; ms < 500; ms += 10) any = any || g2.step(2000 + static_cast<uint64_t>(ms), true, true, 0.20, 0.30, nf::kAdsSightClampM, pos).stopped;
        f186::StopGuard g3;
        g3.step(3000, true, true, 0.02, nf::kAdsSightClampM, nf::kAdsSightClampM, pos);
        g3.step(3100, true, false, 0.0, 0.0, nf::kAdsSightClampM, pos);   // no measurement: neither advances nor clears
        const f186::StopGuard::Out clampStop = g3.step(3300, true, true, 0.02, nf::kAdsSightClampM, nf::kAdsSightClampM, pos);
        f186::StopGuard g4;
        g4.step(4000, true, true, 0.15, nf::kAdsSightClampM, nf::kAdsSightClampM, pos);
        const f186::StopGuard::Out both = g4.step(4300, true, true, 0.15, nf::kAdsSightClampM, nf::kAdsSightClampM, pos);
        f186::StopGuard g5;
        g5.step(5000, true, true, 0.20, 0.30, nf::kAdsSightClampM, pos);
        const f186::StopGuard::Out inactive = g5.step(5400, false, true, 0.20, 0.30, nf::kAdsSightClampM, pos);
        const f186::StopGuard::Out cold = g5.step(5410, true, true, 0.20, 0.30, nf::kAdsSightClampM, pos);
        check(!any && clampStop.stopped && clampStop.reason == f186::kReasonClamp && both.stopped && both.reason == f186::kReasonBoth && !inactive.stopped && !cold.stopped && g5.stops == 0,
              "F186: a good frame in between starts the 0.3 s again; a frame with no measurement changes nothing; the applied vector at its 40 cm clamp for 0.3 s is a stop of its own, with the error both; the lock not running forgets it");
    }

    // ---- 5. the lines -------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f186::Response r;
        for (int i = 0; i < 60; ++i) r.addPair(10.0, 0.01, 0.005, -0.01, -0.005);
        for (int i = 0; i < 40; ++i) r.addPair(84.0, 0.01, 0.0, -0.001, -0.01);
        r.lockFrames = 100; r.turnedFrames = 40; r.stops = 1;
        char line[1300];
        const int len = f186::formatResponse(line, sizeof(line), 1, r);
        check(len > 0 && len < 1166 && std::strstr(line, "native on foot ADS lock response (F186) 1 s: the ADS terms turned in 40 of 100 lock frames; lock stops 1; response of the measured error to the lock's change "
                                                          "(a correct loop reads gain 1.00 angle +0.0 deg, the game's turn the aim pitch): aim under 30 deg: 60 pairs, gain 1.00, angle +0.0 deg, corr 1.00, mean aim +10.0 deg; "
                                                          "aim over 75 deg: 40 pairs, gain 1.00, angle +84.3 deg, corr 1.00, mean aim +84.0 deg") != nullptr,
              "F186: the window's line names the frames the pre-turn covered, the stops, and each bin with pairs: its pairs, gain, angle, correlation and mean aim");
        f186::Response none;
        none.lockFrames = 3;
        char noneLine[1300];
        f186::formatResponse(noneLine, sizeof(noneLine), 1, none);
        char stopLine[500];
        const int sl = f186::formatStop(stopLine, sizeof(stopLine), f186::kReasonBoth, 21.5, 40.0, 40.0, true, 81.9, 3);
        check(std::strstr(noneLine, "1 s: the ADS terms turned in 0 of 3 lock frames; lock stops 0;") != nullptr && std::strstr(noneLine, "no pairs") != nullptr && sl > 0 && sl < 420 &&
                  std::strstr(stopLine, "native on foot ADS lock stop (F186): the sight lock held over its limit for 0.3 s (e over 10 cm and the lock at its clamp; e after the lock 21.5 cm, applied 40.0 cm of 40) and is released "
                                        "over 0.15 s and acquired again from zero; aim pitch +81.9 deg; stops this session 3") != nullptr,
              "F186: a window with pairs in no bin says so, and a stop's line names the reason, the figures and the aim pitch, and neither line has an A/B state");
        f186::Response big;
        for (int b = 0; b < 4; ++b) {
            big.bin[b].n = 4000000000u;
            big.bin[b].dd = 1.0; big.bin[b].rdRe = -1.0; big.bin[b].rdIm = 1.0e9; big.bin[b].rr = 1.0e9; big.bin[b].aimSum = -8.5e11;
        }
        big.lockFrames = big.turnedFrames = big.stops = 4000000000u;
        char worst[1400];
        const int wl = f186::formatResponse(worst, sizeof(worst), 4000000000u, big);
        check(wl > 0 && wl < 1166 && std::strstr(worst, "nan") == nullptr && std::strstr(worst, "inf") == nullptr, "F186: the line stays within the log's 1166 bytes with every bin at its worst, and prints no number that is not one");
    }

    // ---- 6. the source sites ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        const size_t reachAt = ini.find("\nnative_on_foot_ads_left_reach = "), squareAt = ini.find("\nnative_on_foot_crouch_ads_square = ");
        check(cpp.find("ads_preturn") == std::string::npos && cpp.find("g_adsPreturn") == std::string::npos && ini.find("ads_preturn") == std::string::npos && ini.find("ADS: pre-turn") == std::string::npos &&
                  reachAt != std::string::npos && squareAt != std::string::npos && reachAt < squareAt,
              "F187: the A/B key experimental.native_on_foot_ads_preturn is gone: nothing reads it, no flag holds it, edvr.ini has neither its entry nor its ui row, and the pre-turn is behaviour");
        const size_t turnAt = at("const nofoot::f186::Turned adsTurn = nofoot::f186::turnAds(adsTerms, g_ltaDrop.load(std::memory_order_relaxed), aimFresh, aimRad, onFoot && !camExternal);"),
                     loopAt = at("if (adsTurn.turned && i == 1) eye[i] += adsTurn.dy;"), shiftAt = at("double sShift[3] = {g_adsLock[0] + g_adsSight.applied[0] + weaponEye.out.ignore[0]");
        check(turnAt != std::string::npos && loopAt != std::string::npos && shiftAt != std::string::npos && turnAt < loopAt && loopAt < shiftAt &&
                  has("const double adsTerms[3] = {g_adsLock[0] + g_adsSight.applied[0] + weaponEye.out.seated[0], g_adsLock[1] + g_adsSight.applied[1] + weaponEye.out.seated[1],") &&
                  has("if (adsTurn.turned && i == 2) eye[i] += adsTurn.dz;") && has("if (adsTurn.turned) ++g_f186Win.turnedFrames;") &&
                  has("g_f93Sight.record(serial, g_adsSight.applied);") && has("const double lockSum[3] = {g_adsLock[0] + g_adsSight.applied[0], g_adsLock[1] + g_adsSight.applied[1], g_adsLock[2] + g_adsSight.applied[2]};") &&
                  has("f119Tick(now, np, haveFrame, ads, onFoot && !camExternal, eye, lockSum, weaponEye.out.seated, weaponView.seated, neckRef.out.seatedY, hold.eyeY);"),
              "F186: the ADS terms' sum (the position lock, the sight lock, the weapon eye offset) goes out turned (the y and z shares added after the neck raise's), while the F70 shift, F93b's record of the lock's part and F119's sums stay the unturned vectors; the weapon view bias and F87h's hold are not turned");
        check(has("if (g_f186Pairer.step(f93RowsFrame, vRendered, err, dv, de) && aimFresh) g_f186Win.addPair(") && has("for (int i = 0; i < 3; ++i) vRendered[i] = tg.err[i] - err[i] + beforePos[i];") &&
                  has("tg.source == nofoot::f93::kMatched") && has("++g_f186Win.lockFrames;") &&
                  has("const nofoot::f186::StopGuard::Out stop = g_f186Stop.step(now, active && !g_adsSightTripped, have, E.perpNorm, nofoot::ikLen(g_adsSight.pos), nofoot::kAdsSightClampM, g_adsSight.pos);") &&
                  has("g_adsSight.pos[i] = stop.zeroNow ? 0.0 : g_f186Stop.start[i] * stop.scale;") && has("nofoot::f186::formatStop(stopLine, sizeof(stopLine), stop.reason,") &&
                  has("nofoot::f186::formatResponse(line186, sizeof(line186), seconds, g_f186Win);") && has("if (g_f186Win.lockFrames) {"),
              "F186: the response pairs are the shift each matched eye was rendered with and the error measured on it, frame to frame; the stop watches the error after the lock and the applied vector and ramps the lock to zero; the window's line and each stop's line are printed");
        const size_t stepAt = at("nofoot::adsEyeStep(g_adsSight.pos, g_adsSight.vel, g_adsSight.target, tg.err, tg.have, active, g_adsSightTripped, dt);"), stopAt = at("g_f186Stop.step(now,"), appliedAt = at("for (size_t i = 0; i < 3; ++i) g_adsSight.applied[i] = g_adsSight.pos[i];");
        check(stepAt != std::string::npos && stopAt != std::string::npos && appliedAt != std::string::npos && stepAt < stopAt && stopAt < appliedAt,
              "F186: the stop acts on the lock after its step and before its position is applied");
    }
}

}  // namespace f186test
