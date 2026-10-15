// Planet patch motion for the flat profile (celestial_motion.h): the tangents of the flat camera rows, pure math with no D3D.
#pragma once

#include <cmath>
#include <limits>

namespace edvr {

// Tangents (l, r, b, t) of the flat camera rows with the raster phase they carry removed, in celestial_motion's order
// (the ray of render uv (u, v) is (l + u*(r-l), t - v*(t-b), -1)). rows[c][k]: c the input component, k the clip
// output, as FlatMonoResolveFrame::camera holds them; ndcX/ndcY the NDC shift the rows carry (FlatProjectionJitter).
// False unless the rows are a symmetric perspective: |w row| within 1e-3 of 1 and the x, y and w rows mutually
// orthogonal to 1e-3 of their lengths' product. The unjitter is the shader's unjitterRow: x[c] = rows[c][0] - ndcX*rows[c][3],
// y[c] = rows[c][1] - ndcY*rows[c][3], w[c] = rows[c][3], for the input components c = 0..2. wLength and worstDot, when given, are
// the measured |w row| and the largest of the three orthogonality ratios (|x.y|/(|x||y|) and so on), set before any test, so a refusal
// can say what it measured (the flat resolver's one-shot note).
inline bool flatCelestialTangents(const float rows[6][4], float ndcX, float ndcY, float out[4], float* wLength = nullptr, float* worstDot = nullptr) {
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return false;
    float x[3], y[3], w[3];
    for (int c = 0; c < 3; ++c) {
        x[c] = rows[c][0] - ndcX * rows[c][3];
        y[c] = rows[c][1] - ndcY * rows[c][3];
        w[c] = rows[c][3];
    }
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float wl = std::sqrt(dot(w, w));
    const float sx = std::sqrt(dot(x, x)), sy = std::sqrt(dot(y, y));
    // A zero length makes its ratio infinite, so a degenerate row reports as the worst case.
    const auto ratio = [](float d, float a, float b) { return a > 0 && b > 0 ? std::fabs(d) / (a * b) : std::numeric_limits<float>::infinity(); };
    if (wLength) *wLength = wl;
    if (worstDot) *worstDot = std::fmax(std::fmax(ratio(dot(x, y), sx, sy), ratio(dot(x, w), sx, wl)), ratio(dot(y, w), sy, wl));
    // Each comparison is written so that a NaN fails it.
    if (!(std::fabs(wl - 1.0f) <= 1e-3f)) return false;
    if (!(sx > 0 && sy > 0 && std::isfinite(sx) && std::isfinite(sy))) return false;
    if (!(std::fabs(dot(x, y)) <= 1e-3f * sx * sy)) return false;
    if (!(std::fabs(dot(x, w)) <= 1e-3f * sx * wl)) return false;
    if (!(std::fabs(dot(y, w)) <= 1e-3f * sy * wl)) return false;
    out[0] = -1.0f / sx;
    out[1] = 1.0f / sx;
    out[2] = -1.0f / sy;
    out[3] = 1.0f / sy;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]) && std::isfinite(out[3]);
}

}  // namespace edvr
