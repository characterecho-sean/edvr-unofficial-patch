// F216 checks (native on-foot VR), run216 below. Header only, run by native_frame_test.
//
// F216 (native_on_foot_f216.h, native_on_foot.cpp; the user setting fix.native_on_foot_view_bob, default off = the bob is taken out): the P1 hook's filter on the camera the game made. The rig
// runs a 3.4 Hz +-2 cm sine on a level path (it drops to under a fifth), the same sine on a 1 m/s ramp (it drops, and the output follows the ramp within 1 cm), a 6.5 mm side sway at half that
// rate, the clamp, the weight's ramp, the speed gate (below 2 m/s, and an in-place turn's swing, nothing and an exact 0.0), the key on (an exact 0.0), the reset on a 2 m step and on a gap, the
// ADS gate, the seated sign (a published view move taken out through nofoot::adsUnlockedCam returns the original camera, all three axes, S[0] honoured), the term's history by serial, the
// line, and the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f216.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f216test {

namespace f216 = edvr::nofoot::f216;
namespace nf = edvr::nofoot;

constexpr double kPi = 3.141592653589793;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A straight run along the body's forward (+z) with the AMC's rows the identity: the camera is the origin plus (side, up, along). The filter's output move is in the same axes.
struct Sim {
    f216::Filter f;
    double t = 0.0, z = 0.0, dt = 1.0 / 90.0;
    explicit Sim(double hz = 90.0) : dt(1.0 / hz) {}
    f216::Out step(double speed, double up, double side, double along, bool remove = true, bool ads = false, const double* head = nullptr) {
        t += dt;
        z += speed * dt;
        f216::In in;
        in.tS = t;
        std::memset(in.m3p, 0, sizeof(in.m3p));
        in.m3p[0] = in.m3p[5] = in.m3p[10] = in.m3p[15] = 1.0f;
        in.m3p[14] = static_cast<float>(z);
        in.camU[0] = side;
        in.camU[1] = up;
        in.camU[2] = static_cast<double>(static_cast<float>(z)) + along;
        in.remove = remove;
        in.ads = ads;
        if (head) {
            in.headOk = true;
            for (int i = 0; i < 3; ++i) in.head[i] = head[i];
        }
        return f.step(in);
    }
};

inline bool plusZero(double x) { return x == 0.0 && !std::signbit(x); }

// The steady state of a sprint at hz with the sine (amplitude m, frequency hz) on the camera's height and a ramp (m/s): the rms of the view's height about the ramp, raw and filtered, over the
// last 3 s of 7 s, and the worst distance of the filtered height from the ramp.
struct Sprint {
    double rawRms = 0.0, outRms = 0.0, worst = 0.0, sideRaw = 0.0, sideOut = 0.0, alongRaw = 0.0, alongOut = 0.0, wMax = 0.0;
    bool clamped = false;
    double moveMaxUp = 0.0;
};
inline Sprint sprint(double hz, double ampM, double freq, double ramp, double sideAmpM = 0.0, double sideFreq = 1.7, double alongAmpM = 0.0) {
    Sim s(hz);
    Sprint r;
    double sumRaw = 0.0, sumOut = 0.0, sideRawSum = 0.0, sideOutSum = 0.0, alongRawSum = 0.0, alongOutSum = 0.0;
    int n = 0;
    for (int i = 0; i < static_cast<int>(7.0 * hz); ++i) {
        const double tt = (i + 1) / hz;
        const double sine = ampM * std::sin(2.0 * kPi * freq * tt);
        const double up = 1.6 + ramp * tt + sine, side = sideAmpM * std::sin(2.0 * kPi * sideFreq * tt), along = 0.3 + alongAmpM * std::sin(2.0 * kPi * freq * tt);
        const f216::Out o = s.step(5.0, up, side, along);
        if (tt < 4.0) continue;
        const double base = 1.6 + ramp * tt;
        r.rawRms += 0.0;
        sumRaw += sine * sine;
        sumOut += (o.move[1] + up - base) * (o.move[1] + up - base);
        r.worst = std::fmax(r.worst, std::fabs(o.move[1] + up - base));
        sideRawSum += side * side;
        sideOutSum += (o.move[0] + side) * (o.move[0] + side);
        alongRawSum += (along - 0.3) * (along - 0.3);
        alongOutSum += (o.move[2] + along - 0.3) * (o.move[2] + along - 0.3);
        r.wMax = std::fmax(r.wMax, o.w);
        r.clamped = r.clamped || o.clamped;
        r.moveMaxUp = std::fmax(r.moveMaxUp, std::fabs(o.move[1]));
        ++n;
    }
    r.rawRms = std::sqrt(sumRaw / n);
    r.outRms = std::sqrt(sumOut / n);
    r.sideRaw = std::sqrt(sideRawSum / n);
    r.sideOut = std::sqrt(sideOutSum / n);
    r.alongRaw = std::sqrt(alongRawSum / n);
    r.alongOut = std::sqrt(alongOutSum / n);
    return r;
}

template <class Check>
void run216(Check&& check) {
    // ---- 1. the time-windowed series ------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f216::Series s;
        for (int i = 0; i <= 200; ++i) s.add(i * 0.0071, std::sin(2.0 * kPi * 3.4 * i * 0.0071), f216::kStrideS);   // an irregular rate
        check(s.covers(f216::kStrideS) && std::fabs(s.mean(f216::kStrideS)) < 0.01, "F216: the mean over a stride nulls a 3.4 Hz sine (any frame rate)");
        f216::Series ramp;
        for (int i = 0; i <= 100; ++i) ramp.add(i * 0.01, 2.0 * i * 0.01, f216::kSwayS);
        check(std::fabs(ramp.mean(0.5) - 2.0 * (1.0 - 0.25)) < 1e-9, "F216: the mean of a ramp over a window is its value at the window's middle");
        double v = 0.0;
        check(ramp.valueAt(0.5, &v) && std::fabs(v - 1.0) < 1e-9 && !ramp.valueAt(2.0, &v), "F216: a value in the past is interpolated, one beyond the last is refused");
        check(!ramp.covers(5.0) && std::isnan(ramp.mean(5.0)), "F216: a window the series does not span has no mean");
        f216::Series big;
        for (int i = 0; i < 3 * f216::kCap; ++i) big.add(i * 0.0001, 1.0, f216::kSwayS);
        check(big.n <= f216::kCap, "F216: a full ring drops the oldest, no allocation");
    }

    // ---- 2. a sprint on level ground ---------------------------------------------------------------------------------------------------------------------------------------------------------
    for (double hz : {72.0, 90.0, 144.0}) {
        const Sprint r = sprint(hz, 0.02, 3.4, 0.0, 0.0065, 1.7, 0.01);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "F216: a 3.4 Hz +-2 cm sine on a level path drops to under a fifth at %.0f Hz (%.2f mm to %.2f mm rms)", hz, r.rawRms * 1000.0, r.outRms * 1000.0);
        check(r.outRms < r.rawRms / 5.0 && r.wMax == 1.0, msg);
        std::snprintf(msg, sizeof(msg), "F216: a 6.5 mm side sway at half that rate drops to under a fifth at %.0f Hz (%.2f mm to %.2f mm rms)", hz, r.sideRaw * 1000.0, r.sideOut * 1000.0);
        check(r.sideOut < r.sideRaw / 5.0, msg);
        std::snprintf(msg, sizeof(msg), "F216: the same sine along the facing drops to under a fifth at %.0f Hz (%.2f mm to %.2f mm rms)", hz, r.alongRaw * 1000.0, r.alongOut * 1000.0);
        check(r.alongOut < r.alongRaw / 5.0, msg);
    }

    // ---- 3. the same sine on a 1 m/s ramp ------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const Sprint r = sprint(90.0, 0.02, 3.4, 1.0);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "F216: the same sine on a 1 m/s ramp also drops (%.2f mm to %.2f mm rms) and the output tracks the ramp within 1 cm (worst %.2f mm)", r.rawRms * 1000.0, r.outRms * 1000.0,
                      r.worst * 1000.0);
        check(r.outRms < r.rawRms / 5.0 && r.worst < 0.01, msg);
    }

    // ---- 4. the clamp -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const Sprint r = sprint(90.0, 0.10, 3.4, 0.0);
        check(r.clamped && std::fabs(r.moveMaxUp - f216::kClampM) < 1e-12, "F216: a +-10 cm sine is held to 5 cm on the axis and says so");
        const Sprint quiet = sprint(90.0, 0.02, 3.4, 0.0);
        check(!quiet.clamped && quiet.moveMaxUp < f216::kClampM, "F216: a +-2 cm sine does not reach the clamp");
    }

    // ---- 5. the weight: ramps over 0.25 s in and out, exact zeros at 0 ---------------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        double firstW = -1.0, fullAt = -1.0, startT = 0.0, lastW = 0.0;
        bool stillZero = true, bounded = true;
        for (int i = 0; i < 90 * 2; ++i) {   // standing: nothing wanted
            const f216::Out o = s.step(0.0, 1.6 + 0.02 * std::sin(2.0 * kPi * 3.4 * s.t), 0.0, 0.3);
            stillZero = stillZero && plusZero(o.move[0]) && plusZero(o.move[1]) && plusZero(o.move[2]) && o.w == 0.0;
        }
        check(stillZero, "F216: standing still, every term is an exact +0.0 and w is 0");
        for (int i = 0; i < 90 * 2; ++i) {   // then a sprint: the weight ramps
            const f216::Out o = s.step(5.0, 1.6 + 0.02 * std::sin(2.0 * kPi * 3.4 * s.t), 0.0, 0.3);
            if (o.w > 0.0 && firstW < 0.0) {
                firstW = o.w;
                startT = s.t;
            }
            if (o.w >= 1.0 && fullAt < 0.0) fullAt = s.t;
            bounded = bounded && o.w >= lastW - 1e-12 && o.w <= 1.0;
            lastW = o.w;
        }
        check(firstW > 0.0 && firstW < 0.1 && bounded && std::fabs((fullAt - startT) - f216::kRampS) < 3.0 / 90.0, "F216: w ramps from 0 to 1 over 0.25 s");
        double fallStart = -1.0, zeroAt = -1.0;
        bool monotone = true;
        double prev = 1.0;
        for (int i = 0; i < 90 * 2; ++i) {   // standing again: the speed over 0.5 s falls under 2 m/s, then w ramps out
            const f216::Out o = s.step(0.0, 1.6 + 0.02 * std::sin(2.0 * kPi * 3.4 * s.t), 0.0, 0.3);
            if (o.w < 1.0 && fallStart < 0.0) fallStart = s.t;
            if (o.w == 0.0 && zeroAt < 0.0) {
                zeroAt = s.t;
                monotone = monotone && plusZero(o.move[0]) && plusZero(o.move[1]) && plusZero(o.move[2]);
            }
            if (zeroAt >= 0.0) monotone = monotone && plusZero(o.move[0]) && plusZero(o.move[1]) && plusZero(o.move[2]) && o.w == 0.0;
            monotone = monotone && o.w <= prev + 1e-12;
            prev = o.w;
        }
        check(fallStart > 0.0 && zeroAt > 0.0 && std::fabs((zeroAt - fallStart) - f216::kRampS) < 3.0 / 90.0 && monotone, "F216: w ramps out over 0.25 s and the terms are an exact +0.0 from then on");
    }

    // ---- 6. the speed gate ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        bool zero = true, ranAt15 = false;
        for (int i = 0; i < 90 * 5; ++i) {
            const f216::Out o = s.step(1.9, 1.6 + 0.02 * std::sin(2.0 * kPi * 3.4 * s.t), 0.0, 0.3);
            ranAt15 = ranAt15 || o.running;
            zero = zero && plusZero(o.move[0]) && plusZero(o.move[1]) && plusZero(o.move[2]);
        }
        check(!ranAt15 && zero, "F216: at 1.9 m/s (a jog under the gate) nothing is wanted and every term is an exact 0.0");
        Sim q;
        bool run = false;
        for (int i = 0; i < 90 * 3; ++i) run = q.step(2.1, 1.6, 0.0, 0.3).running || run;
        check(run, "F216: at 2.1 m/s the filter is wanted");
        // an in-place turn: the root swings on a circle 0.425 m from the point that stays still, at 1.5 m/s of root speed
        Sim t;
        bool anyRunning = false, zeroT = true;
        double ang = 0.0;
        for (int i = 0; i < 90 * 6; ++i) {
            ang += 3.5 / 90.0;
            t.t += t.dt;
            f216::In in;
            in.tS = t.t;
            std::memset(in.m3p, 0, sizeof(in.m3p));
            in.m3p[0] = in.m3p[5] = in.m3p[10] = in.m3p[15] = 1.0f;
            in.m3p[12] = static_cast<float>(0.425 * std::cos(ang));
            in.m3p[14] = static_cast<float>(0.425 * std::sin(ang));
            in.camU[0] = in.m3p[12];
            in.camU[1] = 1.6 + 0.02 * std::sin(2.0 * kPi * 3.4 * t.t);
            in.camU[2] = in.m3p[14] + 0.3;
            in.remove = true;
            const f216::Out o = t.f.step(in);
            anyRunning = anyRunning || o.running;
            zeroT = zeroT && plusZero(o.move[0]) && plusZero(o.move[1]) && plusZero(o.move[2]);
        }
        check(!anyRunning && zeroT, "F216: an in-place turn (the root swinging at about 1.5 m/s on a 0.425 m circle) is not running: nothing wanted, exact zeros");
    }

    // ---- 7. the key on, ADS ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        bool zero = true;
        for (int i = 0; i < 90 * 5; ++i) {
            const f216::Out o = s.step(5.0, 1.6 + 0.02 * std::sin(2.0 * kPi * 3.4 * s.t), 0.01 * std::sin(2.0 * kPi * 1.7 * s.t), 0.3, false);
            zero = zero && plusZero(o.move[0]) && plusZero(o.move[1]) && plusZero(o.move[2]) && o.w == 0.0;
        }
        check(zero, "F216: with the key on (the game's bob kept) every term is an exact +0.0 at a sprint");
        Sim a;
        double wBefore = 0.0, wAfter = 1.0;
        for (int i = 0; i < 90 * 4; ++i) wBefore = a.step(5.0, 1.6 + 0.02 * std::sin(2.0 * kPi * 3.4 * a.t), 0.0, 0.3).w;
        for (int i = 0; i < 90 * 1; ++i) wAfter = a.step(5.0, 1.6 + 0.02 * std::sin(2.0 * kPi * 3.4 * a.t), 0.0, 0.3, true, true).w;
        check(wBefore == 1.0 && wAfter == 0.0, "F216: in the sights the weight ramps out to 0");
    }

    // ---- 8. resets ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        for (int i = 0; i < 90 * 4; ++i) s.step(5.0, 1.6 + 0.02 * std::sin(2.0 * kPi * 3.4 * s.t), 0.0, 0.3);
        check(s.f.w == 1.0, "F216: the reset test starts at a full weight");
        s.z += 2.0;   // a 2 m step of both the origin and the camera (a teleport)
        const f216::Out o = s.step(5.0, 1.6, 0.0, 0.3);
        check(o.restarted && o.w == 0.0 && plusZero(o.move[1]) && !o.full, "F216: a 2 m step restarts the filter: w 0, exact zeros, windows empty");
        bool full = false;
        int framesToFull = 0;
        for (int i = 0; i < 90 * 2 && !full; ++i, ++framesToFull) full = s.step(5.0, 1.6 + 0.02 * std::sin(2.0 * kPi * 3.4 * s.t), 0.0, 0.3).full;
        check(full && framesToFull > 90 * 0.55 && framesToFull < 90 * 0.75, "F216: and it refills in about 0.6 s (two strides)");
        Sim g;
        for (int i = 0; i < 90 * 2; ++i) g.step(5.0, 1.6, 0.0, 0.3);
        g.t += 0.5;   // a gap of half a second
        const f216::Out og = g.step(5.0, 1.6, 0.0, 0.3);
        check(og.restarted && !og.full && og.w == 0.0, "F216: a gap over 0.25 s restarts the filter");
        Sim n;
        n.step(5.0, 1.6, 0.0, 0.3);
        f216::In in;
        in.tS = n.t + n.dt;
        in.m3p[0] = in.m3p[5] = in.m3p[10] = in.m3p[15] = 1.0f;
        in.m3p[12] = std::nanf("");
        in.camU[1] = 1.6;
        check(n.f.step(in).restarted, "F216: a matrix that is not a number restarts the filter");
    }

    // ---- 8b. the pilot's own head is left alone -----------------------------------------------------------------------------------------------------------------------------------------------
    {
        // a head moving 3.4 Hz +-2 cm on each axis, in the camera and in the pose's LOCAL head (body forward is -z of the head): it passes through untouched
        Sim s;
        double worst = 0.0;
        for (int i = 0; i < 90 * 6; ++i) {
            const double ph = std::sin(2.0 * kPi * 3.4 * (s.t + s.dt));
            const double head[3] = {0.02 * ph, 0.02 * std::sin(2.0 * kPi * 3.4 * (s.t + s.dt) + 1.0), 0.02 * std::sin(2.0 * kPi * 3.4 * (s.t + s.dt) + 2.0)};
            const f216::Out o = s.step(5.0, 1.6 + head[1], head[0], 0.3 - head[2], true, false, head);
            if (s.t > 3.0) worst = std::fmax(worst, std::fmax(std::fabs(o.move[0]), std::fmax(std::fabs(o.move[1]), std::fabs(o.move[2]))));
        }
        char msg[160];
        std::snprintf(msg, sizeof(msg), "F216: a head moving 3.4 Hz +-2 cm on each axis, in the camera and the LOCAL head, passes through untouched (move %.3f mm)", worst * 1000.0);
        check(worst < 0.001, msg);
        // the game's bob (3.4 Hz, 1 cm) with a 1 Hz +-3 cm head move on top: the bob goes, the head stays
        Sim q;
        double sumErr = 0.0, sumBob = 0.0;
        int n = 0;
        for (int i = 0; i < 90 * 7; ++i) {
            const double tt = q.t + q.dt;
            const double hy = 0.03 * std::sin(2.0 * kPi * 1.0 * tt), hx = 0.03 * std::sin(2.0 * kPi * 1.0 * tt + 0.7), hz = 0.03 * std::sin(2.0 * kPi * 1.0 * tt + 1.4);
            const double bob = 0.01 * std::sin(2.0 * kPi * 3.4 * tt);
            const double head[3] = {hx, hy, hz};
            const f216::Out o = q.step(5.0, 1.6 + hy + bob, hx, 0.3 - hz, true, false, head);
            if (tt > 4.0) {
                const double err = o.move[1] + bob;   // the view's up less the head's: what is left of the bob
                sumErr += err * err;
                sumBob += bob * bob;
                ++n;
            }
        }
        std::snprintf(msg, sizeof(msg), "F216: the game's bob with a head move on top has the bob taken out (%.2f mm to %.2f mm rms) and the head left in", std::sqrt(sumBob / n) * 1000.0, std::sqrt(sumErr / n) * 1000.0);
        check(std::sqrt(sumErr / n) < std::sqrt(sumBob / n) / 5.0, msg);
        // a pose with no valid LOCAL head keeps the last good value (no step in W from the missing head)
        Sim r;
        const double head[3] = {0.01, 0.02, 0.03};
        double worstMiss = 0.0;
        for (int i = 0; i < 90 * 3; ++i) {
            const f216::Out o = r.step(5.0, 1.6 + 0.02, 0.01, 0.3 - 0.03, true, false, (i % 7 == 3) ? nullptr : head);
            if (r.t > 2.0) worstMiss = std::fmax(worstMiss, std::fabs(o.move[1]));
        }
        check(worstMiss < 1e-9, "F216: a frame whose pose has no valid LOCAL head uses the last good head");
    }

    // ---- 9. the seated sign: a published view move, taken out through adsUnlockedCam, returns the camera -------------------------------------------------------------------------------
    {
        const double yaw = 0.5;
        const double right[3] = {std::cos(yaw), 0.0, -std::sin(yaw)}, up[3] = {0.0, 1.0, 0.0}, fwd[3] = {std::sin(yaw), 0.0, std::cos(yaw)};
        const float rows[9] = {static_cast<float>(right[0]), static_cast<float>(right[1]), static_cast<float>(right[2]), static_cast<float>(up[0]), static_cast<float>(up[1]), static_cast<float>(up[2]),
                               static_cast<float>(fwd[0]), static_cast<float>(fwd[1]), static_cast<float>(fwd[2])};
        const double base[3] = {3.0, 1.7, -2.0};
        const double moves[][3] = {{0.01, 0.0, 0.0}, {0.0, 0.02, 0.0}, {0.0, 0.0, -0.03}, {0.01, 0.02, -0.03}, {-0.04, -0.05, 0.05}};
        bool signsOk = true, roundTrip = true;
        double worstTrip = 0.0;
        for (const auto& m : moves) {
            double seated[3];
            f216::bodyToSeated(m, seated);
            // the view moves by MINUS the published alignment in the seated space (x right, y up, z backward): that is the world displacement
            double disp[3];
            for (int i = 0; i < 3; ++i) disp[i] = -seated[0] * right[i] - seated[1] * up[i] + seated[2] * fwd[i];
            const double want[3] = {m[0] * right[0] + m[1] * up[0] + m[2] * fwd[0], m[0] * right[1] + m[1] * up[1] + m[2] * fwd[1], m[0] * right[2] + m[1] * up[2] + m[2] * fwd[2]};
            for (int i = 0; i < 3; ++i) signsOk = signsOk && std::fabs(disp[i] - want[i]) < 1e-12;
            // the game's camera carries it
            float cam[3], out[3];
            for (int i = 0; i < 3; ++i) cam[i] = static_cast<float>(base[i] + want[i]);
            nf::adsUnlockedCam(cam, seated, rows, out);
            for (int i = 0; i < 3; ++i) worstTrip = std::fmax(worstTrip, std::fabs(static_cast<double>(out[i]) - base[i]));
        }
        roundTrip = worstTrip < 1e-5;
        check(signsOk, "F216: the seated term is minus the view move in body axes with z backward: (-right, -up, +forward)");
        check(roundTrip, "F216: a published view move taken out through adsUnlockedCam (all three axes, S[0] included) returns the original camera");
        const double upMove[3] = {0.0, 0.01, 0.0};
        double seatedUp[3];
        f216::bodyToSeated(upMove, seatedUp);
        check(std::fabs(seatedUp[1] + 0.01) < 1e-15, "F216: an upward view move of 1 cm is seated y -0.01 (F94's seatedY = -appliedM)");
        // the ADS shift in S travels with it: the sum is taken out whole
        const double ads[3] = {0.002, -0.004, 0.006};
        const double mv[3] = {0.01, 0.02, -0.03};
        double seated[3], total[3];
        f216::bodyToSeated(mv, seated);
        for (int i = 0; i < 3; ++i) total[i] = ads[i] + seated[i];
        float cam[3], out[3];
        double world[3];
        nf::adsCamShiftWorld(total, rows, world);
        for (int i = 0; i < 3; ++i) cam[i] = static_cast<float>(base[i] + world[i]);
        nf::adsUnlockedCam(cam, total, rows, out);
        double worst = 0.0;
        for (int i = 0; i < 3; ++i) worst = std::fmax(worst, std::fabs(static_cast<double>(out[i]) - base[i]));
        check(worst < 1e-5, "F216: with an ADS shift in S the sum is taken out whole");
        double z[3];
        const double zero3[3] = {0.0, 0.0, 0.0};
        f216::bodyToSeated(zero3, z);
        check(plusZero(z[0]) && plusZero(z[1]) && plusZero(z[2]), "F216: a zero move is an exact +0.0 in every seated axis");
    }

    // ---- 10. the term's history by serial ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        auto* h = new f216::SeatedHistory();
        const double a[3] = {0.001, -0.002, 0.003}, b[3] = {-0.004, 0.005, -0.006};
        h->record(5, a);
        h->record(6, b);
        f216::SeatedHistory::Found f = h->find(5 + 1);
        check(f.reported && f.matched && std::fabs(f.s[0] - 0.001) < 1e-7 && std::fabs(f.s[1] + 0.002) < 1e-7 && std::fabs(f.s[2] - 0.003) < 1e-7, "F216: the vector of a serial is found by the pose's reported word");
        f = h->find(0);
        check(!f.reported && !f.matched, "F216: a runtime that reports no serial is not matched");
        f = h->find(1);
        check(f.reported && f.matched && f.s[0] == 0.0 && f.s[1] == 0.0 && f.s[2] == 0.0, "F216: serial 0 is the zero alignment");
        f = h->find(5 + 256 + 1);
        check(f.reported && !f.matched, "F216: a serial the ring no longer holds is a miss, never another serial's vector");
        double n[3];
        h->newest(n);
        check(std::fabs(n[0] + 0.004) < 1e-7 && std::fabs(n[2] + 0.006) < 1e-7, "F216: the newest vector is the last recorded");
        delete h;
    }

    // ---- 11. the log line --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f216::Stats s;
        const auto frame = [&](double w, bool running, double up, double right, double along, bool clamped) {
            f216::Out o;
            o.w = w;
            o.running = running;
            o.move[0] = right;
            o.move[1] = up;
            o.move[2] = along;
            o.clamped = clamped;
            s.add(o, false);
        };
        frame(0.0, false, 0.0, 0.0, 0.0, false);
        frame(0.0, true, 0.0, 0.0, 0.0, false);
        frame(1.0, true, 0.003, -0.001, 0.002, false);
        frame(1.0, true, -0.004, 0.001, -0.002, true);
        char line[760];
        f216::formatLine(line, sizeof(line), 10, s);
        check(std::strstr(line, "native on foot view bob (F216) 10 s: fix.native_on_foot_view_bob is off; game frames 4, running 3, removing 2;") == line &&
                  std::strstr(line, "up rms 3.5 max 4.0 mm, right rms 1.0 max 1.0 mm, along rms 2.0 max 2.0 mm; at the clamp 1") != nullptr,
              "F216: the line names the key, the frames, the running and removing frames, each axis's rms and max in mm and the clamp");
        f216::Stats on;
        on.add(f216::Out{}, true);
        f216::formatLine(line, sizeof(line), 10, on);
        check(std::strstr(line, "is on; game frames 1, running 0, removing 0; up rms 0.0 max 0.0 mm") != nullptr, "F216: with the key on the line says so and that nothing was removed");
    }

    // ---- 12. the sites ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), rec = slurp("src/common/onfoot_recorder_core.h");
        const auto has = [&](const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; };
        check(has(cpp, "cfg.getString(\"fix.native_on_foot_view_bob\", \"off\") == \"on\"") && has(cpp, "g_f216Keep.store(keep, std::memory_order_relaxed)"), "F216: the key is read on the frame thread (on keeps the game's bob; off, the default, removes it)");
        check(has(ini, "# ui: View bob | choices off, on | recommended off | live | menu native_on_foot\nnative_on_foot_view_bob = off"), "F216: the shipped key is off, a live choice on the Native on foot page");
        const size_t comfort = ini.find("# Comfort\n"), other = ini.find("# Native on foot: other settings\n"), key = ini.find("native_on_foot_view_bob = off"), fixEnd = ini.find("\n[visual]");
        check(other != std::string::npos && key != std::string::npos && fixEnd != std::string::npos && comfort != std::string::npos && key > comfort && key < other && other < fixEnd, "F216: the key sits under its own Comfort heading in [fix], above the other-settings block");
        const size_t publishG = cpp.find("void ikPublishG("), filter = cpp.find("f216Step(camU, m3p, now, haveCarried, carried)"), removal = cpp.find("f216ForPin(haveCarried, carried, f216Seated, &f216Source);");
        check(publishG != std::string::npos && removal != std::string::npos && filter != std::string::npos && publishG < removal && removal < filter, "F216: the P1 hook takes the term out, then runs the filter on the camera that is left");
        check(has(cpp, "for (int i = 0; i < 3; ++i) S[i] += f216Seated[i];") && has(cpp, "f216Step(camU, m3p, now, haveCarried, carried)") && has(cpp, "in.headOk = haveCarried && carried.localValid;") && has(cpp, "carried.local[3 + 4 * i]"), "F216: the removal adds all three axes to S, and the filter is given the pose's LOCAL head");
        check(!has(cpp, "if (!ads && onFoot && !camExternal && bobPub.valid"), "F216: the bob term is not gated on the sights in alignTick (the filter's ramp fades it)");
        const size_t f137Term = cpp.find("eye[i] += virtualNeck.seated[i];"), bobTerm = cpp.find("if (i == 0) eye[i] += bobSeated[0];"), external = cpp.find("if (camExternal) eye[i] = 0.0;", cpp.find("if (i == 0) eye[i] += bobSeated[0];")), hold = cpp.find("eye[i] += weaponEye.out.seated[i];");
        check(hold != std::string::npos && f137Term != std::string::npos && bobTerm != std::string::npos && external != std::string::npos && hold < bobTerm && bobTerm < external,
              "F216: the term is added after the hold's and before the first-person gates");
        check(has(cpp, "nofoot::f172::publish(bobSeated[1], g_ltaDrop.load(std::memory_order_relaxed), aimFresh, aimRad, onFoot && !camExternal)") && has(cpp, "if (i == 1) eye[i] += bobRaise.y;"),
              "F216: the y share goes through the raise's turn");
        check(has(cpp, "g_f216Hist.record(serial, g_f216Pub);") && has(cpp, "g_f216Pub[i] = camExternal || !onFoot ? 0.0 : bobSeated[i];"), "F216: the unturned vector is recorded under the serial, a zero off the first-person camera");
        check(has(cpp, "g_f216Pub[0] = g_f216Pub[1] = g_f216Pub[2] = 0.0;   // F216: the zero alignment carries no bob term"), "F216: the edge's zero alignment carries no term");
        check(has(cpp, "if (g_f216.have) g_f216.reset();"), "F216: with the key on the filter is not run");
        check(has(cpp, "nofoot::f216::formatLine(bobLine") && has(cpp, "g_f216Box.write("), "F216: the window line is printed");
        check(has(cpp, "r.sub_bob[i] = static_cast<float>(f216Seated[i]);") && has(cpp, "r.bob_seated[i] = static_cast<float>(bobSeated[i]);") && has(cpp, "r.bob_w = static_cast<float>(bobW);") &&
                  has(rec, "A(f32, bob_seated, 3)") && has(rec, "A(f32, sub_bob, 3)"),
              "F216: the align and P1 records carry the term, its weight and what was taken out");
        check(has(cpp, "str(\"fix.native_on_foot_view_bob\""), "F216: the key is in the recording's key list");
    }
}

}  // namespace f216test
