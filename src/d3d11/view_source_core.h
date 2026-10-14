#pragma once
// The view the look probe and the sealed-hangar trial aim from (TEMPORARY; view_source.cpp, look_probe.cpp, sealed_trial.cpp). No Windows,
// no engine memory: which source the frame's camera comes from, and the camera pose read from the game's three view rows. The rig
// tools\view_source_test compiles this header alone.
//
// Sources: explorer cam (EDVR's free camera, the camera Elite draws through the eye path while its session is on: its rows follow it),
// eye rows (seated VR: the rows temporal_pass chose for the frame), origin (the render context's view 0 origin, used when the source has no
// rows; on foot it is the only reading, because the on-foot camera's rows are not read yet), and vscreen (reserved: the on-foot panel's own
// camera, not read yet).
//
// The rows are the game's view-to-world [R | c], rows[4r + k] for r = 0..2 and k = 0..3, as temporal_math.h states: c = (rows[3], rows[7],
// rows[11]) is the camera's place in the world; the axes are the columns of R: right = column 0 = (rows[0], rows[4], rows[8]), up = column 1
// = (rows[1], rows[5], rows[9]), forward = column 2 = (rows[2], rows[6], rows[10]), the game's view +Z (DirectX forward). The forward follows
// from the DirectX convention and the depth test (z forward); the flight checks it against the look.
#include <cmath>
#include <cstdint>

namespace edvr::view {

enum class Source : uint8_t { kExplorerCam = 0, kEyeRows = 1, kVscreen = 2, kOrigin = 3 };

inline const char* sourceName(Source s) noexcept {
    switch (s) {
        case Source::kExplorerCam: return "explorer cam";
        case Source::kEyeRows: return "eye rows";
        case Source::kVscreen: return "vscreen";
        default: return "origin";
    }
}

// The source for the frame: the explorer cam while its session is on (its eye rows); seated, the eye rows. On foot without the explorer cam
// the rows are not the on-foot camera, so the caller passes rowsOk false and the source is the origin. A source whose rows are not
// available falls back to the origin, and the name says so.
inline Source chooseSource(bool explorerCamOn, bool onFoot, bool rowsOk) noexcept {
    if (explorerCamOn) return rowsOk ? Source::kExplorerCam : Source::kOrigin;
    if (onFoot) return rowsOk ? Source::kVscreen : Source::kOrigin;
    return rowsOk ? Source::kEyeRows : Source::kOrigin;
}

struct Pose {
    float pos[3] = {};   // camera position, world
    float fwd[3] = {};   // camera forward, world, unit
};

// Sanity limits for rows read from the engine: the three axes (the columns of R) must be unit length (within 2%) and mutually orthogonal
// (within 2%), every value finite, and the position within a million metres.
constexpr float kRowLengthTol = 0.02f;
constexpr float kRowOrthoTol = 0.02f;
constexpr float kPosLimitM = 1.0e6f;

// Reads a pose from the game's three view-to-world rows, rows[4r + k] (see the header). False when a value is not finite, an axis is not
// unit length, two axes are not orthogonal, or the position is out of range; *out is then untouched.
inline bool poseFromRows(const float rows[12], Pose* out) noexcept {
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(rows[i])) return false;
    float axis[3][3];   // axis[k][r] = R[r][k]: column k of R, the k-th world axis of the camera
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) axis[k][r] = rows[4 * r + k];
    for (int k = 0; k < 3; ++k) {
        const float len = std::sqrt(axis[k][0] * axis[k][0] + axis[k][1] * axis[k][1] + axis[k][2] * axis[k][2]);
        if (std::fabs(len - 1.0f) > kRowLengthTol) return false;
    }
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const float d = axis[a][0] * axis[b][0] + axis[a][1] * axis[b][1] + axis[a][2] * axis[b][2];
            if (std::fabs(d) > kRowOrthoTol) return false;
        }
    Pose p;
    for (int r = 0; r < 3; ++r) p.pos[r] = rows[4 * r + 3];
    for (int r = 0; r < 3; ++r)
        if (std::fabs(p.pos[r]) > kPosLimitM) return false;
    for (int r = 0; r < 3; ++r) p.fwd[r] = axis[2][r];   // column 2: the game's view +Z, unit by the check above
    *out = p;
    return true;
}

// Reads a pose from a world block of the row-vector kind (the explorer cam's activity block at +0x70, float[16]): m[4i + k] for i = 0..2 are the
// camera's world axes (right, up, forward) as rows, and row 3 (m[12..14]) is its origin. The forward is row 2's first three entries. The same
// sanity limits as poseFromRows apply: the axes are unit length and mutually orthogonal, and the origin is finite and within the limit.
inline bool poseFromWorldBlock(const float m[16], Pose* out) noexcept {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    for (int i = 0; i < 3; ++i) {
        const float len = std::sqrt(m[4 * i] * m[4 * i] + m[4 * i + 1] * m[4 * i + 1] + m[4 * i + 2] * m[4 * i + 2]);
        if (std::fabs(len - 1.0f) > kRowLengthTol) return false;
    }
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const float d = m[4 * a] * m[4 * b] + m[4 * a + 1] * m[4 * b + 1] + m[4 * a + 2] * m[4 * b + 2];
            if (std::fabs(d) > kRowOrthoTol) return false;
        }
    Pose p;
    for (int k = 0; k < 3; ++k) {
        p.pos[k] = m[12 + k];
        if (std::fabs(p.pos[k]) > kPosLimitM) return false;
    }
    for (int k = 0; k < 3; ++k) p.fwd[k] = m[8 + k];   // row 2: the camera's forward axis
    *out = p;
    return true;
}

}  // namespace edvr::view
