// The one pair of axes every world quad of the VR Galaxy Map is spanned on for a frame (docs/galaxy-map-vr-2026-11-03.md, "After the fifth flight"), from stage 0's rows of the
// map camera's pose: right rows[0], up rows[1], forward rows[2]. They are the game's own planes, with the triangles joining them. No ray, no eye rows and no position enter them, so a
// quad does not turn when the head turns or the camera moves sideways. galaxy_map_ui.cpp writes the result into the MapCam constants; tools/galaxy_map_vs_test runs this same function.
#pragma once

#include <cmath>

namespace edvr {
namespace mapui {

struct MapAxes {
    float R[3], U[3], F[3];
};

inline MapAxes mapCamAxes(const float rows[3][3]) {
    MapAxes a{};
    for (int k = 0; k < 3; ++k) {
        a.R[k] = rows[0][k];
        a.U[k] = rows[1][k];
    }
    // F = normalize(cross(R, U)); a pose that makes it degenerate falls back to the camera's own forward
    float f[3] = {a.R[1] * a.U[2] - a.R[2] * a.U[1], a.R[2] * a.U[0] - a.R[0] * a.U[2], a.R[0] * a.U[1] - a.R[1] * a.U[0]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int k = 0; k < 3; ++k) a.F[k] = fl > 1e-6f ? f[k] / fl : rows[2][k];
    return a;
}

} // namespace mapui
} // namespace edvr
