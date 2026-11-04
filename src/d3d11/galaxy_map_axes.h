// The one pair of axes every world quad of the VR Galaxy Map is spanned on for a frame (docs/galaxy-map-vr-2026-11-03.md, "After the fifth flight"), from stage 0's rows of the
// map camera's pose: right rows[0], up rows[1], forward rows[2]. No ray, no eye rows and no position enter it, so a quad does not turn when the head turns or the camera moves
// sideways. galaxy_map_ui.cpp writes the result into the MapCam constants; tools/galaxy_map_vs_test runs this same function.
#pragma once

#include <cmath>

namespace edvr {
namespace mapui {

// The camera's forward within this |cos| of the vertical hands the up over from the grid's vertical to the camera's (smoothstep between): the camera's own pitch, so near a top-down
// camera, where the grid's vertical has no usable lean in the image, the quad's up is the camera's. kAxesRightMin: the horizontal part of the camera's right below which the
// camera's right is used as it is. kAxesUpFloor bounds the scale that keeps a quad's height at the map's centre.
constexpr float kAxesGridFrom = 0.82f, kAxesGridTo = 0.96f, kAxesRightMin = 0.02f, kAxesUpFloor = 0.25f;

struct MapAxes {
    float R[3], U[3], F[3];
};

inline MapAxes mapCamAxes(const float rows[3][3], bool cameraUp) {
    MapAxes a{};
    const float* r0 = rows[0];
    const float* r1 = rows[1];
    const float* r2 = rows[2];
    float len = std::sqrt(r0[0] * r0[0] + r0[2] * r0[2]);
    for (int k = 0; k < 3; ++k) a.R[k] = r0[k];
    if (!cameraUp && len >= kAxesRightMin) {
        a.R[0] = r0[0] / len;
        a.R[1] = 0.0f;
        a.R[2] = r0[2] / len;
    }
    if (cameraUp) {
        for (int k = 0; k < 3; ++k) a.U[k] = r1[k];
    } else {
        float t = (std::fabs(r2[1]) - kAxesGridFrom) / (kAxesGridTo - kAxesGridFrom);
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        t = t * t * (3.0f - 2.0f * t);
        const float scale = 1.0f / (r1[1] > kAxesUpFloor ? r1[1] : kAxesUpFloor);   // Y / max(dot(Y, rows[1]), 0.25)
        a.U[0] = (1.0f - t) * 0.0f + t * r1[0];
        a.U[1] = (1.0f - t) * scale + t * r1[1];
        a.U[2] = (1.0f - t) * 0.0f + t * r1[2];
    }
    // F = normalize(cross(R, U)); a pose that makes it degenerate falls back to the camera's own forward
    float f[3] = {a.R[1] * a.U[2] - a.R[2] * a.U[1], a.R[2] * a.U[0] - a.R[0] * a.U[2], a.R[0] * a.U[1] - a.R[1] * a.U[0]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl > 1e-6f) {
        for (int k = 0; k < 3; ++k) a.F[k] = f[k] / fl;
    } else {
        for (int k = 0; k < 3; ++k) a.F[k] = r2[k];
    }
    return a;
}

} // namespace mapui
} // namespace edvr
