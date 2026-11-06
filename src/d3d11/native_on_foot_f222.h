// F222 (native on-foot VR): the eye's heading, up and offset taken in the frame they were drawn in. Header only, no game or D3D11 state, so native_frame_test runs the very functions the pin
// uses.
//
// What the flight established. Recording 20261106_145502 (build 569e8c49), a steady turn at 289 deg/s with the rifle: both hands stay within 0.1 mm a call in the model, but the neck target swings
// about 7 mm side to side on alternate calls, and the neck, chest and pelvis follow it one for one (the animated chest and pelvis do not), so the elbow moves about 5 mm up and down each call.
// pinStage0 takes the eye's forward, up and offset from the cache of the LAST frame's eye rows (this frame's are not drawn yet) and puts them through THIS frame's world-to-model matrix, so the
// heading in the model is out by exactly this call's body yaw step: over the steady turn the recorded heading reads -1.55 deg less the step (steps 2.64, 4.95, 2.44, 4.42 read -4.21, -6.53, -4.02,
// -5.98). The steps are uneven (5.3, 2.3, 2.3, 5.3 deg a call), so the error alternates. Offline, turning the neck-to-eye vector by the step cuts the neck target's residual over 150 deg/s about
// four times (rifle 1.35 to 0.37 mm middle half, 3.33 to 0.86 mm 90th percentile); the opposite sign doubles it.
//
// The rule. The rows seen new at a pin call were drawn from the previous pin call's frame, so their three vectors are taken into the model through the previous call's rotation (this call's when
// there is none yet) and kept with the rows' frame id. While the id is the same the kept model vectors stand; the world vectors the pin works from are rebuilt from them through this call's matrix, so
// they come out in the model as they went in. The F215 shift touches only the matrix's translation, so the rotation is the same with it.
//
// Journal: the arc doc, F222. TEMPORARY A/B key experimental.native_on_foot_steady_body_in_turns.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f222 {

constexpr double kPi = 3.14159265358979323846;

// The rotation part of a 4x4 row-vector matrix: r[3 * k + i] = m[4 * k + i].
inline void rotationOf(const float m[16], double r[9]) noexcept {
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 3; ++i) r[3 * k + i] = static_cast<double>(m[4 * k + i]);
}

inline void through(const double r[9], const double v[3], double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = v[0] * r[i] + v[1] * r[3 + i] + v[2] * r[6 + i];
}

struct OwnFrame {
    double cur[9] = {}, prev[9] = {};
    bool haveCur = false, havePrev = false;
    uint32_t p1 = 0;
    bool haveP1 = false;
    uint32_t frame = 0;
    bool held = false;
    double offM[3] = {}, fwdM[3] = {}, upM[3] = {};

    void reset() noexcept { *this = OwnFrame{}; }

    // The pin call's game frame and its world-to-model matrix: on a new frame the current rotation becomes the previous one. A second call with the same p1 changes nothing.
    void note(uint32_t p1Now, const float invM[16]) noexcept {
        if (haveP1 && p1Now == p1) return;
        if (haveCur) {
            for (int i = 0; i < 9; ++i) prev[i] = cur[i];
            havePrev = true;
        }
        rotationOf(invM, cur);
        haveCur = true;
        p1 = p1Now;
        haveP1 = true;
    }

    // The rows' three world vectors, new at this call, into model axes through the previous call's rotation (this call's with no previous). True when taken now; false when the id is the one
    // held (what is stored stands) or there is no rotation yet.
    bool take(uint32_t rowsFrame, const double off[3], const double fwd[3], const double up[3]) noexcept {
        if (held && rowsFrame == frame) return false;
        if (!haveCur) return false;
        const double* r = havePrev ? prev : cur;
        through(r, off, offM);
        through(r, fwd, fwdM);
        through(r, up, upM);
        frame = rowsFrame;
        held = true;
        return true;
    }

    // The world vectors which, through this call's world-to-model matrix, give the stored model vectors: the stored vectors through this call's model-to-world rotation (m3p).
    void world(const float m3p[16], double off[3], double fwd[3], double up[3]) const noexcept {
        double r[9];
        rotationOf(m3p, r);
        through(r, offM, off);
        through(r, fwdM, fwd);
        through(r, upM, up);
    }
};

// The horizontal angle (deg, unsigned) between two model-axis vectors, about the model's y; 0 when either has no horizontal part.
inline double horizontalAngleDeg(const double a[3], const double b[3]) noexcept {
    const double la = std::sqrt(a[0] * a[0] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[2] * b[2]);
    if (!(la > 1e-9) || !(lb > 1e-9)) return 0.0;
    const double c = a[2] * b[0] - a[0] * b[2], d = a[0] * b[0] + a[2] * b[2];
    const double ang = std::atan2(c, d) * 180.0 / kPi;
    return std::isfinite(ang) ? std::fabs(ang) : 0.0;
}

// ---- the log window ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Stats {
    uint32_t calls = 0, taken = 0, measured = 0;
    double turnSumDeg = 0.0, turnMaxDeg = 0.0;
    // One pin call: whether rows were taken in it, and with the key on the horizontal angle between today's heading and the stored one (a negative figure: not measured).
    void add(bool tookRows, double turnDeg) noexcept {
        ++calls;
        if (tookRows) ++taken;
        if (turnDeg >= 0.0 && std::isfinite(turnDeg)) {
            ++measured;
            turnSumDeg += turnDeg;
            if (turnDeg > turnMaxDeg) turnMaxDeg = turnDeg;
        }
    }
    bool any() const noexcept { return calls > 0; }
    void clear() noexcept { *this = Stats{}; }
};

// The window's line. tools\edvr_log.py reads it.
inline int formatLine(char* buf, size_t n, uint32_t seconds, bool keyOn, const Stats& s) noexcept {
    return std::snprintf(buf, n, "native on foot eye in its own frame (F222) %u s: experimental.native_on_foot_steady_body_in_turns is %s; %u pin calls, %u rows taken; the body's turn taken out of the heading mean %.2f deg, largest %.2f",
                         seconds, keyOn ? "on" : "off", s.calls, s.taken, s.measured ? s.turnSumDeg / s.measured : 0.0, s.turnMaxDeg);
}

}  // namespace f222
}  // namespace nofoot
}  // namespace edvr
