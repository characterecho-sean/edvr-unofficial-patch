#pragma once
// Pose helpers shared by the camera pick rules (camera_nearest.h, camera_jump_guard.h) and the camera-row event detector (camera_row_events.h).
// Ported 2026-10-13 from the helper half of src/common/camera_ipd_pair.h (branch claude/hangar-test-4, commits 998921da and e40d6674), which is
// not on main: only the pieces the two A/B rules and the detector use came across, with the IPD rule itself left behind. docs\sky-flicker-2026-10-13.md.
//
// Rows are a row-major 3x4: element (r, c) at r*4 + c; the translation is column 3 (indices 3, 7, 11). Pure: no Windows, no globals.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::campose {

constexpr float kJumpMeters = 0.03f;      // the guard's first test: a candidate this far from the previous pose...
constexpr float kStepMeters = 0.01f;      // ...is taken only when it continues the previous step, to this plus kStepFraction of the step
constexpr float kStepFraction = 0.05f;
constexpr float kStepDeg = 0.5f;          // ...and its rotation continues the previous step to this many degrees
constexpr float kPi = 3.14159265358979323846f;

// The angle in degrees between two 3x3 rotations (row-major, stride 3): acos((trace(A^T B) - 1) / 2).
inline float angle3Deg(const float a[9], const float b[9]) {
    float tr = 0.0f;
    for (int i = 0; i < 9; ++i) tr += a[i] * b[i];
    float c = (tr - 1.0f) * 0.5f;
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return std::acos(c) * 180.0f / kPi;
}

// The 3x3 rotation of a row-major 3x4, stride 3 out.
inline void rot3Of34(const float m[12], float r[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = m[i * 4 + j];
}

// A^T B for 3x3 row-major matrices.
inline void mulTransposeLeft3(const float a[9], const float b[9], float out[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i * 3 + j] = a[0 * 3 + i] * b[0 * 3 + j] + a[1 * 3 + i] * b[1 * 3 + j] + a[2 * 3 + i] * b[2 * 3 + j];
}

// The rotation between two rigid 3x4s, degrees.
inline float rotationDeg(const float a[12], const float b[12]) {
    float ra[9], rb[9];
    rot3Of34(a, ra);
    rot3Of34(b, rb);
    return angle3Deg(ra, rb);
}

// The translation distance between two rigid 3x4s, metres.
inline float translationM(const float a[12], const float b[12]) {
    const float dx = a[3] - b[3], dy = a[7] - b[7], dz = a[11] - b[11];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The same pose: translations within 1 mm and rotations within 0.1 deg.
inline bool samePose(const float a[12], const float b[12]) {
    if (!(translationM(a, b) <= 0.001f)) return false;
    return rotationDeg(a, b) <= 0.1f;
}

// The guard's first test: the candidate's translation is more than kJumpMeters from the previous chosen pose.
inline bool departs(const float cand[12], const float prev[12]) {
    return translationM(cand, prev) > kJumpMeters;
}

// The guard's exception: rotation and translation both continue the previous step, a constant-velocity prediction from the last two chosen
// poses (before is the one chosen before prev; null when there is none, which predicts no motion).
inline bool continuesStep(const float cand[12], const float prev[12], const float before[12]) {
    float pred[12];
    float stepLen = 0.0f;
    if (before) {
        float m[9], pr[9], pb[9];
        rot3Of34(before, pb);
        rot3Of34(prev, pr);
        mulTransposeLeft3(pb, pr, m);   // before^T prev: the last step, in the body's axes
        float prot[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) prot[i * 3 + j] = pr[i * 3 + 0] * m[0 * 3 + j] + pr[i * 3 + 1] * m[1 * 3 + j] + pr[i * 3 + 2] * m[2 * 3 + j];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) pred[i * 4 + j] = prot[i * 3 + j];
            pred[i * 4 + 3] = 2.0f * prev[i * 4 + 3] - before[i * 4 + 3];
        }
        const float sx = prev[3] - before[3], sy = prev[7] - before[7], sz = prev[11] - before[11];
        stepLen = std::sqrt(sx * sx + sy * sy + sz * sz);
    } else {
        std::memcpy(pred, prev, sizeof(pred));
    }
    const float dx = cand[3] - pred[3], dy = cand[7] - pred[7], dz = cand[11] - pred[11];
    const float tol = kStepMeters + kStepFraction * stepLen;
    if (!(std::sqrt(dx * dx + dy * dy + dz * dz) <= tol)) return false;
    float rc[9], rp[9];
    rot3Of34(cand, rc);
    rot3Of34(pred, rp);
    return angle3Deg(rc, rp) <= kStepDeg;
}

// The applied poses, last two: the state both rules measure from.
struct Chain {
    bool havePose = false;
    bool havePrev = false;
    float pose[12] = {};
    float before[12] = {};
};

inline void commitPose(Chain& c, const float* rows) {
    if (c.havePose) {
        std::memcpy(c.before, c.pose, sizeof(c.pose));
        c.havePrev = true;
    }
    std::memcpy(c.pose, rows, sizeof(c.pose));
    c.havePose = true;
}

}  // namespace edvr::campose
