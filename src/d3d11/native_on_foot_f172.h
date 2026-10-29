// F172 (native on foot VR): the neck raise published pre-turned by the mouse-look aim pitch, so the game's own turn lands it straight up. Header only, no game or D3D11 state, so
// native_frame_test runs the very functions the frame thread uses. Journal: the arc doc, F172.
//
// The flight edvr_gfx_20261029_054640 (build b0113bea), the raise carriage line (F171), mouse look, per band of |aim pitch|: under 10 deg k_up +0.99, k_fwd +0.11, c -0.01; 10 to 40 deg
// k_up +0.70, k_fwd -0.23, c -0.27; 40 deg and over k_up -0.09, k_fwd -1.04, c -1.09. At level the camera carries the neck raise straight up and the hook's vertical removal is right.
// Pitched, the raise turns with the aim: a published body-up R lands as about (forward -R sin a, up R cos a), a the aim's pitch, nose up positive. Under mouse look the game applies the
// eye alignment (a seated vector) in the aim-pitched frame, so F138's raise, looking up, pushed the view BACKWARD, into the head and the torso, and the hook's vertical removal took more
// out of the camera than the camera carried, which moved G against the raise (c -1): F138's loop.
//
// The convention is F137's. A body offset (forward, up) is seated as (x, y, z) = (-right, -up, +forward) (seatedFromBody at psi 0), and R(p) turns it nose up by p in the (forward, up)
// plane:  R(p) (f, u) = (f cos p - u sin p, f sin p + u cos p). The game's turn is body_out = R(a) body_in. A raise R up (seated y = seatedY = -R) published as it stands comes out as
//     R(a) (0, R) = (-R sin a, R cos a)          so k_up = cos a and k_fwd = -sin a (a = 85 deg: +0.09 and -1.00; the flight read -0.09 and -1.04),
// and the vertical removal, R, takes R out of a camera that carries R cos a of it: G moves by -R (1 - cos a), c = cos a - 1 (a = 85 deg: -0.91; the flight read -1.09).
// Published turned by -a instead, body_in = R(-a) (0, R) = (R sin a, R cos a), the game's R(a) of it is (0, R) exactly: straight up. In seated terms
//     y += seatedY cos a,  z += -seatedY sin a        (seatedY = -R: y = -R cos a, z = +R sin a).
// The hook's removal stays vertical (the record kept under the serial is the unturned seatedY), because the camera now carries the raise straight up.
//
// a is the aim pitch F137 uses: the pin hook's FP root forward elevation (g_f109RootPitch), fresh within F137's kAimMaxAgeMs. Not fresh, look to aim driving the aim (the aim follows
// the head: the game turns nothing), the camera external or off the on-foot camera, no raise, or a = 0: unturned, exactly as before. The readers of the published alignment take it
// whole (F119's history, the runtime's pose) or only its y as published (F87h's feed, F89's history), so the share added to z and the part taken off y cancel where they are read.
#pragma once

#include "native_on_foot_core.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f172 {

// The raise as it is published: its seated y and z shares (the neck reference's part of the eye alignment; the z share is 0 when unturned) and whether it was turned.
struct Raise {
    bool turned = false;
    double y = 0.0, z = 0.0;
    double pitchRad = 0.0;   // the aim pitch used (0 when unturned)
};

// The neck reference's seated y (seatedY, the raise up is -seatedY) published for an aim pitch of aimPitchRad. Unturned (turned false, y = seatedY, z = 0) unless the camera is the
// commander's first-person one, look to aim is not driving the aim, the aim pitch is fresh and a number and not 0, and there is a raise.
inline Raise publish(double seatedY, bool lookToAim, bool aimFresh, double aimPitchRad, bool firstPerson) noexcept {
    Raise r;
    r.y = seatedY;
    if (!firstPerson || lookToAim || !aimFresh || !std::isfinite(aimPitchRad) || !std::isfinite(seatedY) || seatedY == 0.0 || aimPitchRad == 0.0) return r;
    r.turned = true;
    r.pitchRad = aimPitchRad;
    r.y = seatedY * std::cos(aimPitchRad);
    r.z = -seatedY * std::sin(aimPitchRad);
    return r;
}

// The game's turn, as the rig models it: a body offset (forward, up) turned nose up by the aim pitch.
inline void gameTurn(double aimPitchRad, double forward, double up, double* forwardOut, double* upOut) noexcept {
    const double c = std::cos(aimPitchRad), s = std::sin(aimPitchRad);
    *forwardOut = forward * c - up * s;
    *upOut = forward * s + up * c;
}
// A seated alignment (x, y, z) as the body offset (forward, up) the game turns: seatedFromBody's inverse at psi 0, (right, up, forward) = (-x, -y, +z).
inline void bodyOfSeated(const double seated[3], double* forward, double* up) noexcept {
    *forward = seated[2];
    *up = -seated[1];
}

// The second's figures, on the frame thread: the alignment ticks that carried a raise, those that published it turned, the aim pitch (deg) and the z share (cm) summed over the
// turned ones.
struct Counters {
    uint32_t ticks = 0, turned = 0;
    double pitchDegSum = 0.0, zCmSum = 0.0;
    void add(const Raise& r) noexcept {
        ++ticks;
        if (!r.turned) return;
        ++turned;
        pitchDegSum += r.pitchRad * 180.0 / 3.14159265358979323846;
        zCmSum += r.z * 100.0;
    }
    void merge(const Counters& o) noexcept {
        ticks += o.ticks;
        turned += o.turned;
        pitchDegSum += o.pitchDegSum;
        zCmSum += o.zCmSum;
    }
};

// The clause appended to the raise carriage line: the ticks turned of the ticks with a raise, the mean aim pitch used and the mean z share published (over the turned ones). Always every
// field, so the line reads the same every second; "0 of N ticks" is a second the raise went out as it was. Returns the line's length.
inline int appendTurned(char* line, size_t cap, const Counters& c) noexcept {
    const size_t at = std::strlen(line);
    if (at >= cap) return static_cast<int>(at);
    const double n = c.turned ? static_cast<double>(c.turned) : 1.0;
    const int w = std::snprintf(line + at, cap - at, "; turned by the aim pitch (F172): %u of %u ticks with a raise, mean aim pitch %+.1f deg, mean z share %+.1f cm", c.turned, c.ticks,
                                c.pitchDegSum / n, c.zCmSum / n);
    return w < 0 ? static_cast<int>(at) : static_cast<int>(at) + w;
}

}  // namespace f172
}  // namespace nofoot
}  // namespace edvr
