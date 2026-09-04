// The rest lock's arithmetic (experimental.shimmer_rest), header-only and pure, the
// supersample_math.h precedent: one definition for the openvr half that
// applies it and for anything that wants to table-test it.
//
// What it is for. A headset's tracking never quite stops. Measured on a
// Pimax Crystal Super lying on a desk (2026-09-03, the pose history's turn
// column): position steady to 0.0 mm a frame, orientation wandering about
// a tenth of an arcminute every frame with a half-pixel step a few times a
// second. The game renders from that pose and the compositor re-warps every
// frame by the same motion, so a line about a pixel wide -- a hull seam, a
// hairline, text -- blinks as it crosses pixel rows. Holding the render
// pose stilled the game's own frame; telling the compositor the frame's
// display pose stilled the headset (pose_hold = headset, proven the same
// day). This header is the continuous version of that instrument: the pose
// the game renders from follows the tracker with a factor k that is tiny
// while the head is still and one the moment it moves, and the pose the
// compositor is told slides the same way between "the display pose" (k =
// 0, nothing to re-warp) and "the render pose" (k = 1, stock).
//
// Rotations travel as unit quaternions in doubles, from and to the 3x3 of
// an OpenVR row-major 3x4 matrix (m[row][col], v' = M v).
#pragma once

#include <cmath>

namespace edvr {

struct RestQuat {
    double w, x, y, z;
};

// Shepperd's method: the largest diagonal term picks the stable branch.
inline RestQuat restQuatFromMatrix(const float m[3][4]) {
    const double m00 = m[0][0], m01 = m[0][1], m02 = m[0][2];
    const double m10 = m[1][0], m11 = m[1][1], m12 = m[1][2];
    const double m20 = m[2][0], m21 = m[2][1], m22 = m[2][2];
    RestQuat q;
    const double tr = m00 + m11 + m22;
    if (tr > 0.0) {
        const double s = sqrt(tr + 1.0) * 2.0;
        q.w = 0.25 * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q.w = (m21 - m12) / s;
        q.x = 0.25 * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const double s = sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25 * s;
        q.z = (m12 + m21) / s;
    } else {
        const double s = sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25 * s;
    }
    const double n = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n > 0.0) {
        q.w /= n;
        q.x /= n;
        q.y /= n;
        q.z /= n;
    } else {
        q.w = 1.0;
        q.x = q.y = q.z = 0.0;
    }
    return q;
}

// Writes the rotation only; the translation column is the caller's.
inline void restQuatToMatrix(const RestQuat& q, float m[3][4]) {
    const double w = q.w, x = q.x, y = q.y, z = q.z;
    m[0][0] = static_cast<float>(1.0 - 2.0 * (y * y + z * z));
    m[0][1] = static_cast<float>(2.0 * (x * y - w * z));
    m[0][2] = static_cast<float>(2.0 * (x * z + w * y));
    m[1][0] = static_cast<float>(2.0 * (x * y + w * z));
    m[1][1] = static_cast<float>(1.0 - 2.0 * (x * x + z * z));
    m[1][2] = static_cast<float>(2.0 * (y * z - w * x));
    m[2][0] = static_cast<float>(2.0 * (x * z - w * y));
    m[2][1] = static_cast<float>(2.0 * (y * z + w * x));
    m[2][2] = static_cast<float>(1.0 - 2.0 * (x * x + y * y));
}

// a moved k of the way toward b, along the shorter arc, renormalised. For
// the degrees a head turns in a frame, nlerp and slerp are the same curve.
inline RestQuat restQuatNlerp(const RestQuat& a, RestQuat b, double k) {
    if (k <= 0.0) return a;
    if (k >= 1.0) return b;
    const double dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot < 0.0) {
        b.w = -b.w;
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
    }
    RestQuat r;
    r.w = a.w + k * (b.w - a.w);
    r.x = a.x + k * (b.x - a.x);
    r.y = a.y + k * (b.y - a.y);
    r.z = a.z + k * (b.z - a.z);
    const double n = sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    if (n > 0.0) {
        r.w /= n;
        r.x /= n;
        r.y /= n;
        r.z /= n;
    } else {
        r = a;
    }
    return r;
}

// The hold factor from the smoothed head speed, in arcminute-equivalents per
// frame: kMin at and under `still`, 1 at and over `moving`, a straight line
// between. kMin is the fraction of the remaining distance the held pose
// closes every frame while still -- 0.02 is a time constant of about half
// a second at 90 Hz, which lets the tracker's slow wander through unseen
// and takes its per-frame jitter down by fifty times.
inline double restHoldFactor(double speed, double still, double moving,
                             double kMin) {
    if (!(speed == speed)) return 1.0;  // NaN: stock
    if (moving <= still) return speed > still ? 1.0 : kMin;
    if (speed <= still) return kMin;
    if (speed >= moving) return 1.0;
    const double t = (speed - still) / (moving - still);
    return kMin + t * (1.0 - kMin);
}

}  // namespace edvr
