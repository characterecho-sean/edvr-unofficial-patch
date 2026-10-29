// F175 checks (native on-foot VR), run175 below. Header only, run by native_frame_test.
//
// F175 (native_on_foot_f175.h): the view's roll and pitch against the headset's, by look mode and item. Measure only. The rig checks the geometry on synthetic rotations (a camera rolled 5 deg
// reads 5; yaw and pitch alone read roll 0; the sign: right side up is positive; the model's axes and LOCAL's give the same roll and elevation for the same physical camera), the angular
// rate between consecutive LOCAL orientations (the yaw part about the vertical), the rate tracker's holding and restarting, the window's statistics and its slope on synthetic pairs
// (1, 0 and -1), and the line: its words, its length inside the log's budget, and that a window without frames prints nothing while one with error 0 prints zeros.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f175.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f175test {

namespace nf = edvr::nofoot;
namespace f175 = edvr::nofoot::f175;

struct Frame {
    double right[3], up[3], fwd[3];
};
// A camera looking yaw psi (about the vertical) and pitch theta (nose up), rolled phi about its forward (positive: the right side up), in the MODEL's axes (x right, y up, z forward).
inline Frame modelFrame(double psi, double theta, double phi) {
    const double f[3] = {std::sin(psi) * std::cos(theta), std::sin(theta), std::cos(psi) * std::cos(theta)};
    const double u0[3] = {-std::sin(psi) * std::sin(theta), std::cos(theta), -std::cos(psi) * std::sin(theta)};
    const double r0[3] = {u0[1] * f[2] - u0[2] * f[1], u0[2] * f[0] - u0[0] * f[2], u0[0] * f[1] - u0[1] * f[0]};   // up x forward
    Frame fr;
    for (int i = 0; i < 3; ++i) {
        fr.fwd[i] = f[i];
        fr.right[i] = r0[i] * std::cos(phi) + u0[i] * std::sin(phi);
        fr.up[i] = u0[i] * std::cos(phi) - r0[i] * std::sin(phi);
    }
    return fr;
}
// The same physical camera as an OpenXR LOCAL pose (x right, y up, -z forward): the third axis flipped; the 3x4 row-major has right, up and back as its columns.
inline void localPose(double psi, double theta, double phi, float m[12], double x = 0.0, double y = 1.6, double z = 0.0) {
    const Frame fr = modelFrame(psi, theta, phi);
    const double flip[3] = {1.0, 1.0, -1.0};
    for (int r = 0; r < 3; ++r) {
        m[4 * r + 0] = static_cast<float>(fr.right[r] * flip[r]);
        m[4 * r + 1] = static_cast<float>(fr.up[r] * flip[r]);
        m[4 * r + 2] = static_cast<float>(-fr.fwd[r] * flip[r]);
    }
    m[3] = static_cast<float>(x);
    m[7] = static_cast<float>(y);
    m[11] = static_cast<float>(z);
}
// A LOCAL pose turned only about the vertical by psi: for the rate tests.
inline void yawPose(double psi, float m[12]) { localPose(psi, 0.0, 0.0, m); }

template <class Check>
inline void run175(Check&& check) {
    const double kPi = 3.14159265358979323846;
    const auto rad = [&](double deg) { return deg * kPi / 180.0; };
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    const double up[3] = {0.0, 1.0, 0.0};
    char msg[512];

    // ---- 1. the roll and the elevation on synthetic rotations ----------------------------------------------------------------------------------------------------------------------------
    {
        double roll = 99.0;
        const Frame rolled = modelFrame(0.0, 0.0, rad(5.0));
        check(f175::viewRoll(rolled.fwd, rolled.up, up, &roll) && eq(roll, rad(5.0), 1e-12) && rolled.right[1] > 0.0,
              "F175: a camera rolled 5 deg (its right side up) reads a roll of +5 deg, and its right vector has the upward part");
        const Frame left = modelFrame(0.0, 0.0, rad(-5.0));
        check(f175::viewRoll(left.fwd, left.up, up, &roll) && eq(roll, rad(-5.0), 1e-12) && left.right[1] < 0.0, "F175: rolled the other way (the right side down) it reads -5 deg: the sign is right side up positive");
        bool zero = true, tilted = true;
        const double yaws[6] = {0.0, 30.0, 90.0, 179.0, -60.0, -135.0}, pitches[5] = {0.0, 20.0, -45.0, 60.0, -80.0};
        for (double y : yaws)
            for (double p : pitches) {
                const Frame fr = modelFrame(rad(y), rad(p), 0.0);
                double r0 = 99.0, rr = 99.0;
                zero = zero && f175::viewRoll(fr.fwd, fr.up, up, &r0) && eq(r0, 0.0, 1e-12);
                const Frame rolled5 = modelFrame(rad(y), rad(p), rad(5.0));
                tilted = tilted && f175::viewRoll(rolled5.fwd, rolled5.up, up, &rr) && eq(rr, rad(5.0), 1e-12);
            }
        check(zero, "F175: yaw and pitch alone read roll 0, at every yaw and pitch up to 80 deg either way");
        check(tilted, "F175: a roll of 5 deg reads 5 at every yaw and pitch (the roll about the forward, against the character's up)");
        // a camera up that is not perpendicular to its forward (the mean of two eyes' rows): the roll is still about the forward
        const Frame fr = modelFrame(rad(40.0), rad(25.0), rad(3.0));
        double skew[3] = {fr.up[0] + 0.05 * fr.fwd[0], fr.up[1] + 0.05 * fr.fwd[1], fr.up[2] + 0.05 * fr.fwd[2]};
        check(f175::viewRoll(fr.fwd, skew, up, &roll) && eq(roll, rad(3.0), 1e-12), "F175: an up that leans toward the forward reads the same roll (the right comes from their cross product)");
        double scaled[3] = {fr.fwd[0] * 3.0, fr.fwd[1] * 3.0, fr.fwd[2] * 3.0}, upScaled[3] = {fr.up[0] * 0.5, fr.up[1] * 0.5, fr.up[2] * 0.5};
        check(f175::viewRoll(scaled, upScaled, up, &roll) && eq(roll, rad(3.0), 1e-12), "F175: and vectors of any length");
        const double a[3] = {0.0, 1.0, 0.0};
        check(!f175::viewRoll(a, a, up, &roll) && !f175::viewRoll(up, a, up, &roll) && !f175::viewRoll(a, up, up, &roll), "F175: a forward parallel to the up, or a vector with no length, has no roll");
        const double nanV[3] = {std::nan(""), 0.0, 1.0};
        check(!f175::viewRoll(nanV, up, up, &roll) && !f175::viewRoll(a, nanV, up, &roll), "F175: a vector that is not a number has none");
        // the elevation
        double e = 99.0;
        const Frame pitched = modelFrame(rad(10.0), rad(30.0), rad(4.0));
        check(f175::viewElevation(pitched.fwd, up, &e) && eq(e, rad(30.0), 1e-12), "F175: the view's elevation is its forward against the up, nose up positive (30 deg, whatever the yaw and roll)");
        const Frame down = modelFrame(0.0, rad(-60.0), 0.0);
        check(f175::viewElevation(down.fwd, up, &e) && eq(e, rad(-60.0), 1e-12), "F175: and -60 looking down");

        // the model's axes and LOCAL's give the same roll and elevation for the same physical camera: a view that is the headset's reads an error of 0
        bool same = true, sameElev = true;
        for (double y : yaws)
            for (double p : pitches)
                for (double r : {-7.0, 0.0, 2.5, 12.0}) {
                    float m[12];
                    localPose(rad(y), rad(p), rad(r), m);
                    const Frame fr2 = modelFrame(rad(y), rad(p), rad(r));
                    double rl = 99.0, rv = 99.0, el = 99.0, ev = 99.0;
                    same = same && f175::localRoll(m, &rl) && f175::viewRoll(fr2.fwd, fr2.up, up, &rv) && eq(rl, rv, 1e-6) && eq(rl, rad(r), 1e-6);
                    sameElev = sameElev && f175::localElevation(m, &el) && f175::viewElevation(fr2.fwd, up, &ev) && eq(el, ev, 1e-6) && eq(el, rad(p), 1e-6);
                }
        check(same, "F175: LOCAL's roll (the rotation's first two columns' y parts) and the model's give the same roll for the same camera, the sign convention shared: a view that is the headset's reads an error of 0");
        check(sameElev, "F175: LOCAL's elevation (asin of minus the third column's y) and the model's agree the same way");
        float degenerate[12] = {};
        check(!f175::localRoll(degenerate, &roll) && !f175::localRoll(nullptr, &roll), "F175: a LOCAL pose with no rotation has no roll");
    }

    // ---- 2. the angular rate between consecutive LOCAL poses ---------------------------------------------------------------------------------------------------------------------------------
    {
        float a[12], b[12];
        double speed = 0.0, yaw = 0.0;
        yawPose(rad(10.0), a);
        yawPose(rad(13.0), b);
        check(f175::angularRate(a, b, 0.01, &speed, &yaw) && eq(speed, rad(300.0), 1e-3) && eq(yaw, -rad(300.0), 1e-3), "F175: 3 deg of yaw to the right in 10 ms is 300 deg/s, all of it about the vertical (negative: positive is the head turning left)");
        yawPose(rad(13.0), a);
        yawPose(rad(10.0), b);
        check(f175::angularRate(a, b, 0.01, &speed, &yaw) && eq(speed, rad(300.0), 1e-3) && eq(yaw, rad(300.0), 1e-3), "F175: turning left the speed is the same and the yaw part positive");
        float p0[12], p1[12];
        localPose(0.0, rad(10.0), 0.0, p0);
        localPose(0.0, rad(13.0), 0.0, p1);
        check(f175::angularRate(p0, p1, 0.01, &speed, &yaw) && eq(speed, rad(300.0), 1e-3) && eq(yaw, 0.0, 1e-4), "F175: 3 deg of pitch in 10 ms is 300 deg/s with no yaw part");
        // a turn about a tilted axis: the yaw part is the vertical share (a pitch of 60 deg and a yaw step: the world yaw rate of the step stays the step)
        localPose(rad(10.0), rad(60.0), 0.0, p0);
        localPose(rad(13.0), rad(60.0), 0.0, p1);
        check(f175::angularRate(p0, p1, 0.01, &speed, &yaw) && eq(yaw, -rad(300.0), 1e-3) && eq(speed, rad(300.0), 1e-3), "F175: a yaw step looking up 60 deg is still 300 deg/s about the vertical (yaw of the head about LOCAL's up, not the heading's rate)");
        check(!f175::angularRate(a, b, 0.0, &speed, &yaw) && !f175::angularRate(a, b, 0.0005, &speed, &yaw) && !f175::angularRate(a, b, 0.3, &speed, &yaw), "F175: poses 0, under 1 ms or over 0.25 s apart make no rate");
        b[5] = std::numeric_limits<float>::quiet_NaN();
        check(!f175::angularRate(a, b, 0.01, &speed, &yaw), "F175: a pose that is not a number makes none");
        yawPose(rad(10.0), a);
        yawPose(rad(10.0), b);
        check(f175::angularRate(a, b, 0.01, &speed, &yaw) && eq(speed, 0.0, 1e-6) && eq(yaw, 0.0, 1e-6), "F175: the same pose is a rate of 0");

        // the tracker: a rate from poses that differ, held while the pose does not change, stale after 0.25 s, restarted by a recentre or a pose that goes back in time
        f175::RateTracker t;
        double sp = 0.0, yw = 0.0;
        yawPose(rad(0.0), a);
        yawPose(rad(2.0), b);
        const bool first = t.step(a, 10.000, 10.001, 7, &sp, &yw);
        const bool same = t.step(a, 10.000, 10.011, 7, &sp, &yw);
        const bool second = t.step(b, 10.010, 10.012, 7, &sp, &yw);
        check(!first && !same && second && eq(sp, 200.0, 0.05) && eq(yw, -200.0, 0.05), "F175: the first pose makes no rate, the same pose again none, the next pose that differs 200 deg/s (2 deg in 10 ms)");
        const bool held = t.step(b, 10.010, 10.040, 7, &sp, &yw);
        const bool stale = t.step(b, 10.010, 10.300, 7, &sp, &yw);
        check(held && eq(sp, 200.0, 0.05) && !stale, "F175: the rate is held while the pose does not change, 28 ms later, and is gone after 0.25 s");
        yawPose(rad(4.0), a);
        t.step(b, 10.010, 10.310, 7, &sp, &yw);
        const bool recentred = t.step(a, 10.020, 10.321, 8, &sp, &yw);
        check(!recentred, "F175: a new LOCAL generation (a recentre) starts over: no rate across it");
        const bool back = t.step(b, 10.000, 10.330, 8, &sp, &yw);
        check(!back, "F175: a pose whose own time went back starts over");
        const bool gap = t.step(a, 11.000, 11.001, 8, &sp, &yw);
        check(!gap, "F175: poses more than 0.25 s apart make no rate (the stream paused)");
    }

    // ---- 3. the window: the statistics, the splits, the slope on synthetic pairs --------------------------------------------------------------------------------------------------------------
    {
        const auto frame = [&](int mode, int item, double rollDeg, double pitchDeg, bool rate, double speed, double yawRate, bool lead, double aimDeg, double yawLeadDeg) {
            f175::Sample s;
            s.mode = mode;
            s.item = item;
            s.rollOk = true;
            s.rollErrRad = rad(rollDeg);
            s.pitchOk = true;
            s.pitchErrRad = rad(pitchDeg);
            s.rateOk = rate;
            s.speedDegS = speed;
            s.yawDegS = yawRate;
            s.leadOk = lead;
            s.aimPitchRad = rad(aimDeg);
            s.yawLeadRad = rad(yawLeadDeg);
            s.pitchLeadRad = rad(1.0);
            return s;
        };
        f175::Window w;
        for (int i = 0; i < 40; ++i) w.add(frame(1, 1, i % 2 ? 1.0 : -1.0, 0.5, true, 4.0, 1.0, true, 30.0, 5.0));          // look to aim, armed, the head still: roll error +-1 (rms 1), pitch +0.5
        for (int i = 0; i < 20; ++i) w.add(frame(1, 1, i % 2 ? 3.0 : -3.0, -0.5, true, 120.0, 90.0, true, 30.0, 20.0));    // turning at 90 deg/s of yaw: +-3 deg, over 2 deg
        for (int i = 0; i < 10; ++i) w.add(frame(1, 1, 0.0, 0.0, true, 20.0, 10.0, true, 30.0, 5.0));                      // neither still nor turning horizontally
        for (int i = 0; i < 5; ++i) w.add(frame(1, 1, 0.5, 0.0, false, 0.0, 0.0, false, 0.0, 0.0));                        // rate unknown, no lead
        for (int i = 0; i < 12; ++i) w.add(frame(0, 0, 0.2, 2.0, true, 3.0, 0.0, true, 10.0, 2.0));                        // mouse look, unarmed
        const f175::Group& g = w.g[1][1];
        check(g.frames == 75 && g.roll.n == 75 && g.over == 20 && eq(g.roll.maxAbs, 3.0, 1e-9) && g.still.n == 40 && eq(g.still.rms(), 1.0, 1e-9) && eq(g.still.mean(), 0.0, 1e-9) && g.turning.n == 20 &&
                  eq(g.turning.rms(), 3.0, 1e-9) && eq(g.turning.maxAbs, 3.0, 1e-9),
              "F175: the window counts the frames, the share over 2 deg, and splits the roll error by the head still (under 10 deg/s) and turning horizontally (yaw rate over 30 deg/s); a frame in neither is in neither");
        check(eq(g.roll.rms(), std::sqrt((40.0 * 1.0 + 20.0 * 9.0 + 5.0 * 0.25) / 75.0), 1e-9) && eq(g.roll.mean(), (5.0 * 0.5) / 75.0, 1e-9), "F175: the roll error's mean and rms are over every frame with a roll");
        check(g.pitch.n == 75 && eq(g.pitch.mean(), (40.0 * 0.5 - 20.0 * 0.5) / 75.0, 1e-9) && eq(g.pitch.maxAbs, 0.5, 1e-9) && eq(g.speed.mean(), (40.0 * 4.0 + 20.0 * 120.0 + 10.0 * 20.0) / 70.0, 1e-9),
              "F175: the pitch error's mean, rms and largest and the head's mean speed are over the frames that have them");
        check(w.g[0][0].frames == 12 && w.g[0][1].frames == 0 && w.g[1][0].frames == 0 && w.any(), "F175: the window keeps the look mode and the item apart");
        f175::Window other;
        other.add(frame(1, 1, 2.0, 0.0, true, 1.0, 0.0, true, 30.0, 5.0));
        f175::Group merged = g;
        merged.merge(other.g[1][1]);
        check(merged.frames == 76 && merged.over == 20 && merged.still.n == 41, "F175: two windows' groups add up");
        w.clear();
        check(!w.any() && w.g[1][1].frames == 0, "F175: a cleared window has no frames");
        f175::Sample bad;
        bad.mode = 2;
        w.add(bad);
        check(!w.any(), "F175: a mode or an item out of range is dropped");

        // the slope: the roll error against (aim pitch x the head's yaw lead), radians against radians, through the origin
        const auto slopeOf = [&](double k) {
            f175::Group gg;
            for (int i = 0; i < 60; ++i) {
                const double aim = 10.0 + 2.0 * i, lead = (i % 7) - 3.0 + 4.0;
                f175::Sample made = frame(1, 1, 0.0, 0.0, true, 5.0, 0.0, true, aim, lead);
                made.rollErrRad = k * (made.aimPitchRad * made.yawLeadRad);   // the roll error that hypothesis 1 makes: k x (aim pitch x yaw lead), in radians
                gg.add(made);
            }
            return gg;
        };
        const f175::Group plus = slopeOf(1.0), zero = slopeOf(0.0), minus = slopeOf(-1.0), half = slopeOf(0.5);
        check(plus.slopeN == 60 && eq(plus.slope(), 1.0, 1e-9) && eq(zero.slope(), 0.0, 1e-12) && eq(minus.slope(), -1.0, 1e-9) && eq(half.slope(), 0.5, 1e-9) && plus.xRms() > 0.01,
              "F175: the slope of the roll error against aim pitch x yaw lead reads +1, 0 and -1 on synthetic pairs built to make them (60 pairs), and 0.5 for a half");
        // no spread in x (the aim level, or the head on the aim): no slope, and the line says how small the input was
        f175::Group flat;
        for (int i = 0; i < 30; ++i) flat.add(frame(1, 1, 2.0, 0.0, true, 5.0, 0.0, true, 0.0, 0.0));
        check(flat.slopeN == 30 && flat.slope() == 0.0 && flat.xRms() == 0.0, "F175: with the aim level the input of the slope is 0 and so is the slope (the line prints the pairs and the input's rms beside it)");
        // a frame with no lead is not a pair
        f175::Group nolead;
        for (int i = 0; i < 10; ++i) nolead.add(frame(1, 1, 2.0, 0.0, true, 5.0, 0.0, false, 30.0, 5.0));
        check(nolead.slopeN == 0 && nolead.frames == 10, "F175: a frame with no reported head to read the lead from counts as a frame and not as a pair");
    }

    // ---- 4. the line --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f175::Group g;
        for (int i = 0; i < 90; ++i) {
            f175::Sample s;
            s.mode = 1;
            s.item = 1;
            s.rollOk = s.pitchOk = s.rateOk = s.leadOk = true;
            s.rollErrRad = rad(i % 2 ? 0.8 : -0.4);
            s.pitchErrRad = rad(0.2);
            s.speedDegS = i < 60 ? 5.0 : 90.0;
            s.yawDegS = i < 60 ? 1.0 : 80.0;
            s.aimPitchRad = rad(25.0);
            s.yawLeadRad = rad(2.0);
            s.pitchLeadRad = rad(-1.5);
            g.add(s);
        }
        char line[1300];
        const int len = f175::formatGroup(line, sizeof(line), 1, 1, 1, g, false);
        std::snprintf(msg, sizeof(msg), "F175: the line names the tag, the mode, the item, the rig, the frames and every figure (typical %d bytes)", len);
        check(len > 0 && len < 1166 && std::strstr(line, "native on foot view roll (F175) 1 s: mode look to aim, item armed, F61 rig off; 90 frames") != nullptr &&
                  std::strstr(line, "roll error mean +0.20 rms 0.63 max 0.80 deg, over 2 deg in 0 frames") != nullptr && std::strstr(line, "head still (under 10 deg/s) 60 frames mean +0.20") != nullptr &&
                  std::strstr(line, "head turning (yaw over 30 deg/s) 30 frames mean +0.20") != nullptr && std::strstr(line, "pitch error (the view's elevation less the headset's in LOCAL) 90 frames mean +0.20") != nullptr &&
                  std::strstr(line, "slope of the roll error against aim pitch x the head's yaw lead ") != nullptr && std::strstr(line, " over 90 frames (x rms ") != nullptr &&
                  std::strstr(line, "head speed mean 33.3 deg/s; aim pitch mean +25.0 deg; reported head lead over the aim yaw +2.0 pitch -1.5 deg") != nullptr,
              msg);
        char mouse[1300];
        f175::formatGroup(mouse, sizeof(mouse), 1, 0, 0, g, true);
        check(std::strstr(mouse, "mode mouse look, item unarmed, F61 rig ON;") != nullptr && std::strstr(mouse, "less the headset's in LOCAL plus the aim's pitch)") != nullptr,
              "F175: mouse look names its expected pitch (the headset's plus the aim's), the rig reads ON, an unarmed hand reads unarmed");
        // the worst a second can print: every figure at its limit
        f175::Group worst;
        for (int i = 0; i < 180; ++i) {
            f175::Sample s;
            s.mode = 1;
            s.item = 1;
            s.rollOk = s.pitchOk = s.rateOk = s.leadOk = true;
            s.rollErrRad = rad(i % 2 ? -179.9 : 179.9);
            s.pitchErrRad = rad(i % 2 ? -89.9 : 89.9);
            s.speedDegS = i % 2 ? 720.0 : 3.0;
            s.yawDegS = i % 2 ? -720.0 : 0.0;
            s.aimPitchRad = rad(-89.9);
            s.yawLeadRad = rad(-179.9);
            s.pitchLeadRad = rad(-89.9);
            worst.add(s);
        }
        char widest[1300];
        const int wl = f175::formatGroup(widest, sizeof(widest), 1, 1, 1, worst, true);
        std::snprintf(msg, sizeof(msg), "F175: the line is inside the log's 1166 bytes with every figure at its limit (%d bytes)", wl);
        check(wl > 0 && wl < 1166, msg);
        // a group with frames and error 0 prints zeros (the code ran and measured nothing wrong); a window with no frame has no line to print
        f175::Group zero;
        for (int i = 0; i < 100; ++i) {
            f175::Sample s;
            s.mode = 1;
            s.item = 0;
            s.rollOk = s.pitchOk = s.rateOk = s.leadOk = true;
            s.speedDegS = 2.0;
            zero.add(s);
        }
        char zeroLine[1300];
        f175::formatGroup(zeroLine, sizeof(zeroLine), 1, 1, 0, zero, false);
        f175::Window none;
        check(std::strstr(zeroLine, "100 frames") != nullptr && std::strstr(zeroLine, "roll error mean +0.00 rms 0.00 max 0.00 deg") != nullptr && std::strstr(zeroLine, "nan") == nullptr && !none.any(),
              "F175: a second with frames and an error of 0 prints the frames and zeros; a window with no frame prints nothing (the two read differently in the log)");
        f175::Group empty;
        char emptyLine[1300];
        f175::formatGroup(emptyLine, sizeof(emptyLine), 1, 0, 0, empty, false);
        check(std::strstr(emptyLine, "nan") == nullptr && std::strstr(emptyLine, "inf") == nullptr, "F175: an empty group formats without a number that is not one");
    }

    // ---- 5. the source: where the figures are read ----------------------------------------------------------------------------------------------------------------------------------------------
    {
        std::ifstream f("src/d3d11/native_on_foot.cpp", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string cpp = ss.str();
        const auto has = [&](const char* needle) { return cpp.find(needle) != std::string::npos; };
        check(!cpp.empty() && has("if (freshIn && cycSrc == kCycRows && upMOk && in.localFull && refusal != kPinRefCameraAway && !(pc.detachW > 0.0)) {") &&
                  has("f175::viewRoll(fwdM, upM, worldUp, &viewRoll) && f175::localRoll(in.local, &headRoll)") && has("g_f175Box.write([&](nofoot::f175::Window& win) noexcept { win.add(fs); });") &&
                  has("if (!vw.g[m][i].frames) continue;") && has("g_f175Box.take(&vw) && vw.any()") && has("g_f175Rate.reset();"),
              "F175: the pin hook measures a frame only with this frame's eye rows (kCycRows), the model's up, the whole LOCAL pose, the commander's camera and the sights down; the frame thread prints a line only for a group with frames; the edges reset the rate (source pins, run from the repository root)");
        check(has("g_pinIn.localFull = localPitchValid;") && has("std::memcpy(g_pinIn.local, np.local, sizeof(g_pinIn.local));") && has("std::memcpy(g_pinIn.render, np.render, sizeof(g_pinIn.render));"),
              "F175: the pin's input carries the pose of its own call, the whole LOCAL pose and the render head");
    }
}

}  // namespace f175test
