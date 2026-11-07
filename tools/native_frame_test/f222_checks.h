// F222 checks (native on-foot VR), run222 below. Header only, run by native_frame_test.
//
// F222 (native_on_foot_f222.h, native_on_foot.cpp): the eye rows are a frame old, so their forward, up and offset are taken into the model through the frame they were drawn in and written back as the
// world vectors that give them in this call's model. The rig turns a body with uneven yaw steps under a heading that is constant in the model, the rows always one frame old: through today's path
// the heading is out by the step, through OwnFrame it matches on every call and the offset likewise. It also runs a repeated frame, no previous rotation, zero yaw, the horizontal angle, the line,
// and pins the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_f222.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f222test {

namespace f222 = edvr::nofoot::f222;

inline std::string slurp222(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The matrix of a body yawed by theta about y (row vectors: the model's x, y and z axes in world are the rows); -theta gives the inverse.
inline void m3pOf(double theta, float m[16]) noexcept {
    std::memset(m, 0, 16 * sizeof(float));
    const float c = static_cast<float>(std::cos(theta)), s = static_cast<float>(std::sin(theta));
    m[0] = c;
    m[2] = -s;
    m[5] = 1.0f;
    m[8] = s;
    m[10] = c;
    m[15] = 1.0f;
}

template <class Check>
inline void run222(Check&& check) {
    const double kPi = f222::kPi;
    const auto rad = [&](double deg) { return deg * kPi / 180.0; };
    char msg[400];
    const double stepsDeg[4] = {5.3, 2.3, 2.3, 5.3};
    const double hdgDeg = -1.55;
    const double fwdM[3] = {std::sin(rad(hdgDeg)), 0.0, std::cos(rad(hdgDeg))}, upMv[3] = {0.0, 1.0, 0.0}, offMv[3] = {0.03, 0.01, -0.02};
    const auto dist = [](const double a[3], const double b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); };
    double worst = 0.0, worstOff = 0.0, bestOffToday = 1e9;
    bool turnOk = true, upOk = true;
    {
        f222::OwnFrame of;
        double theta = 0.0, prevTheta = 0.0;
        float prevM3p[16];
        for (int k = 0; k < 40; ++k) {
            prevTheta = theta;
            m3pOf(prevTheta, prevM3p);
            theta += rad(stepsDeg[k % 4]);
            float m3p[16], inv[16];
            m3pOf(theta, m3p);
            m3pOf(-theta, inv);
            // the rows, drawn at the previous call's frame (the first call's own): world vectors for a model heading that is constant
            const float* drawn = k == 0 ? m3p : prevM3p;
            double rotDrawn[9], off[3], fwd[3], up[3];
            f222::rotationOf(drawn, rotDrawn);
            f222::through(rotDrawn, offMv, off);
            f222::through(rotDrawn, fwdM, fwd);
            f222::through(rotDrawn, upMv, up);
            double rotInv[9], todayF[3], todayO[3];
            f222::rotationOf(inv, rotInv);
            f222::through(rotInv, fwd, todayF);
            f222::through(rotInv, off, todayO);
            of.note(static_cast<uint32_t>(100 + k), inv);
            of.take(static_cast<uint32_t>(k), off, fwd, up);
            double w[3] = {off[0], off[1], off[2]}, f[3] = {fwd[0], fwd[1], fwd[2]}, u[3] = {up[0], up[1], up[2]};
            of.world(m3p, w, f, u);
            double ownF[3], ownO[3], ownU[3];
            f222::through(rotInv, f, ownF);
            f222::through(rotInv, w, ownO);
            f222::through(rotInv, u, ownU);
            if (k >= 1) {
                const double eF = f222::horizontalAngleDeg(ownF, fwdM), eT = f222::horizontalAngleDeg(todayF, fwdM);
                worst = std::fmax(worst, eF);
                worstOff = std::fmax(worstOff, dist(ownO, offMv));
                bestOffToday = std::fmin(bestOffToday, dist(todayO, offMv));
                turnOk = turnOk && std::fabs(eT - stepsDeg[k % 4]) <= 1e-3;
                upOk = upOk && std::fabs(ownU[1] - 1.0) < 1e-5 && std::fabs(ownU[0]) < 1e-5 && std::fabs(ownU[2]) < 1e-5;
            }
        }
        std::snprintf(msg, sizeof(msg), "F222: through today's path the heading is out by exactly the call's yaw step; through OwnFrame it is out by %.2e deg", worst);
        check(turnOk && worst < 1e-3, msg);
        std::snprintf(msg, sizeof(msg), "F222: the offset is out by up to %.3e m through OwnFrame (today's, at least %.3e m), the up is kept", worstOff, bestOffToday);
        check(worstOff < 1e-5 && upOk, msg);
    }
    {   // the same frame's rows again are not taken again, and the same p1 changes no rotation
        f222::OwnFrame of;
        float inv0[16], inv1[16], other[16];
        m3pOf(0.0, inv0);
        m3pOf(-rad(10.0), inv1);
        m3pOf(-rad(40.0), other);
        const double off[3] = {0.0, 0.0, 0.0}, fwdA[3] = {0.0, 0.0, 1.0}, fwdB[3] = {1.0, 0.0, 0.0}, up[3] = {0.0, 1.0, 0.0};
        of.note(1, inv0);
        const bool first = of.take(7, off, fwdA, up);
        of.note(2, inv1);
        const bool again = of.take(7, off, fwdB, up);
        check(first && !again && of.fwdM[2] == 1.0 && of.fwdM[0] == 0.0, "F222: rows of the frame already held are not taken again: what is stored stands");
        of.note(2, other);   // the same p1: nothing changes
        const bool newFrame = of.take(8, off, fwdB, up);
        double r[9], want[3];
        f222::rotationOf(inv0, r);   // the previous call's rotation is call 1's: a second note of p1 2 changed nothing
        f222::through(r, fwdB, want);
        check(newFrame && std::fabs(of.fwdM[0] - want[0]) < 1e-9 && std::fabs(of.fwdM[2] - want[2]) < 1e-9, "F222: a second note with the same p1 changes nothing: a new frame goes through the previous call's rotation");
    }
    {   // no previous rotation: the current one; and nothing before a first note
        f222::OwnFrame of;
        const double off[3] = {0.0, 0.0, 0.0}, fwd[3] = {0.6, 0.0, 0.8}, up[3] = {0.0, 1.0, 0.0};
        check(!of.take(1, off, fwd, up) && !of.held, "F222: with no rotation noted nothing is taken");
        float inv[16];
        m3pOf(-rad(30.0), inv);
        of.note(5, inv);
        of.take(1, off, fwd, up);
        double r[9], want[3];
        f222::rotationOf(inv, r);
        f222::through(r, fwd, want);
        check(of.held && std::fabs(of.fwdM[0] - want[0]) < 1e-12 && std::fabs(of.fwdM[2] - want[2]) < 1e-12, "F222: with no previous frame the current rotation is used, which is today's value");
        of.reset();
        check(!of.held && !of.haveCur && !of.havePrev, "F222: reset forgets it all");
    }
    {   // zero yaw: today's values
        f222::OwnFrame of;
        float id[16], m[16];
        m3pOf(0.0, id);
        m3pOf(0.0, m);
        const double off[3] = {0.03, 0.01, -0.02}, fwd[3] = {0.1, 0.0, 0.995}, up[3] = {0.0, 1.0, 0.0};
        of.note(1, id);
        of.note(2, id);
        of.take(1, off, fwd, up);
        double w[3], f[3], u[3];
        of.world(m, w, f, u);
        check(std::fabs(w[0] - off[0]) < 1e-5 && std::fabs(f[0] - fwd[0]) < 1e-5 && std::fabs(f[2] - fwd[2]) < 1e-5 && std::fabs(u[1] - 1.0) < 1e-5, "F222: with no yaw the vectors come back as they went in");
    }
    {   // the horizontal angle and the line
        const double a[3] = {0.0, 0.3, 1.0}, b[3] = {std::sin(rad(10.0)), -0.2, std::cos(rad(10.0))}, flat[3] = {0.0, 1.0, 0.0};
        check(std::fabs(f222::horizontalAngleDeg(a, b) - 10.0) < 1e-9 && f222::horizontalAngleDeg(a, flat) == 0.0, "F222: the horizontal angle ignores the height and is 0 for a vertical vector");
        f222::Stats st;
        check(!st.any(), "F222: an empty window has nothing to say");
        st.add(true, 4.0);
        st.add(false, 2.0);
        st.add(true, -1.0);
        char line[420];
        f222::formatLine(line, sizeof(line), 10, true, st);
        check(std::string(line) == "native on foot eye in its own frame (F222) 10 s: experimental.native_on_foot_steady_body_in_turns is on; 3 pin calls, 2 rows taken; the body's turn taken out of the heading mean 3.00 deg, largest 4.00", line);
        f222::formatLine(line, sizeof(line), 10, false, f222::Stats{});
        check(std::string(line).find("is off; 0 pin calls, 0 rows taken") != std::string::npos, "F222: the line names the key off");
    }
    {   // the sites
        const std::string cpp = slurp222("src/d3d11/native_on_foot.cpp");
        const auto has = [&](const char* needle) { return cpp.find(needle) != std::string::npos; };
        check(has("static_cast<float>(static_cast<double>(in.camRaw[0]) + eyeOffW[0])") && has("static_cast<float>(static_cast<double>(in.camRaw[2]) + eyeOffW[2])"), "F222: the drawn eye's offset is the call's own (eyeOffW)");
        check(has("if (!g_f222Key.load(std::memory_order_relaxed)) {\n                g_f222.reset();\n            } else if (cycSrc != kCycNone) {") && has("g_f222.note(in.p1, invM);") &&
                  has("if (cycSrc == kCycRows) f222Took = g_f222.take(g_pinCyc.frame, eyeOffW, fwdW, upW);") && has("g_f222.world(in.m3p, eyeOffW, fwdW, upW);") && has("for (int i = 0; i < 3; ++i) eyeW[i] += eyeOffW[i];"),
              "F222: the key gates the path, the rows are taken only when new, kept vectors are written back through this call's matrix, and the offset is added after");
        check(has("g_pinCyc.frame = fr[0];") && has("g_f222.reset();   // F222: and no rotation of the last frame"), "F222: the rows' frame id is kept with the cache and an edge resets the memory");
        check(has("cfg.getString(\"experimental.native_on_foot_steady_body_in_turns\", \"on\") != \"off\"") && has("g_f222Key.store(on, std::memory_order_relaxed)") && has("str(\"experimental.native_on_foot_steady_body_in_turns\""),
              "F222: the key is read on the frame thread, default on, and dumped");
    }
}

}  // namespace f222test
