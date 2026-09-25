// Layout-specific projection algebra only. Callers own admission, CB storage and
// shader/resource lifetime; these helpers do not establish any of those contracts.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace edvr {

struct FlatProjectionJitter {
    float ndcX = 0, ndcY = 0, uvX = 0, uvY = 0;
};

namespace flat_projection_detail {
inline bool checkedFloat(double value, float& result) {
    const double limit = (std::numeric_limits<float>::max)();
    if (!std::isfinite(value) || value > limit || value < -limit) return false;
    result = static_cast<float>(value);
    return true;
}
inline bool finite(const FlatProjectionJitter& jitter) {
    return std::isfinite(jitter.ndcX) && std::isfinite(jitter.ndcY) &&
        std::isfinite(jitter.uvX) && std::isfinite(jitter.uvY);
}
template<size_t N> inline bool finite(const float (&rows)[N][4]) {
    for (const auto& row : rows)
        for (float value : row) if (!std::isfinite(value)) return false;
    return true;
}
} // namespace flat_projection_detail

// Pixel offsets are positive right/down. Failure never changes the output.
inline bool flatProjectionJitter(float pixelX, float pixelY, uint32_t width,
                                 uint32_t height, FlatProjectionJitter& out) {
    if (!width || !height || !std::isfinite(pixelX) || !std::isfinite(pixelY)) return false;
    FlatProjectionJitter candidate{};
    using flat_projection_detail::checkedFloat;
    if (!checkedFloat(static_cast<double>(pixelX) / width, candidate.uvX) ||
        !checkedFloat(static_cast<double>(pixelY) / height, candidate.uvY) ||
        !checkedFloat(2.0 * pixelX / width, candidate.ndcX) ||
        !checkedFloat(-2.0 * pixelY / height, candidate.ndcY)) return false;
    out = candidate;
    return true;
}

// Scene b1[270..273]: output clip vector is the input-weighted sum of rows.
inline bool flatJitterForwardColumns(float (&rows)[4][4], const FlatProjectionJitter& jitter) {
    using namespace flat_projection_detail;
    if (!finite(jitter) || !finite(rows)) return false;
    if (jitter.ndcX == 0 && jitter.ndcY == 0) return true;
    float next[4][4]; std::memcpy(next, rows, sizeof(next));
    for (size_t i = 0; i < 4; ++i) {
        if (jitter.ndcX != 0 && !checkedFloat(rows[i][0] + static_cast<double>(jitter.ndcX) * rows[i][3], next[i][0])) return false;
        if (jitter.ndcY != 0 && !checkedFloat(rows[i][1] + static_cast<double>(jitter.ndcY) * rows[i][3], next[i][1])) return false;
    }
    std::memcpy(rows, next, sizeof(next));
    return true;
}

// b0[4..7] / b2[10..13]: each row is dotted with the input position.
// This is a homogeneous clip-space shift, valid after local/model transforms
// too. Equality with the main camera matrix is not a precondition. Callers
// still establish target ownership and a common phase for depth consumers.
inline bool flatJitterForwardDp4(float (&rows)[4][4], const FlatProjectionJitter& jitter) {
    using namespace flat_projection_detail;
    if (!finite(jitter) || !finite(rows)) return false;
    if (jitter.ndcX == 0 && jitter.ndcY == 0) return true;
    float next[4][4]; std::memcpy(next, rows, sizeof(next));
    for (size_t i = 0; i < 4; ++i) {
        if (jitter.ndcX != 0 && !checkedFloat(rows[0][i] + static_cast<double>(jitter.ndcX) * rows[3][i], next[0][i])) return false;
        if (jitter.ndcY != 0 && !checkedFloat(rows[1][i] + static_cast<double>(jitter.ndcY) * rows[3][i], next[1][i])) return false;
    }
    std::memcpy(rows, next, sizeof(next));
    return true;
}

// Deferred VS 7E38... b2[14..16]: ray components = dot(row.xyw, (u,v,1)).
// Sampling at the displaced pixel must reconstruct the original ray.
inline bool flatJitterInverseUvRay(float (&rows)[3][4], const FlatProjectionJitter& jitter) {
    using namespace flat_projection_detail;
    if (!finite(jitter) || !finite(rows)) return false;
    if (jitter.uvX == 0 && jitter.uvY == 0) return true;
    float next[3][4]; std::memcpy(next, rows, sizeof(next));
    for (size_t i = 0; i < 3; ++i)
        if (!checkedFloat(rows[i][3] - static_cast<double>(rows[i][0]) * jitter.uvX -
                          static_cast<double>(rows[i][1]) * jitter.uvY, next[i][3])) return false;
    std::memcpy(rows, next, sizeof(next));
    return true;
}

// Sky VS F8FA... b2[11..14]: inverse clip transform, row-vector convention.
inline bool flatJitterInverseClip(float (&rows)[4][4], const FlatProjectionJitter& jitter) {
    using namespace flat_projection_detail;
    if (!finite(jitter) || !finite(rows)) return false;
    if (jitter.ndcX == 0 && jitter.ndcY == 0) return true;
    float next[4][4]; std::memcpy(next, rows, sizeof(next));
    for (size_t i = 0; i < 4; ++i)
        if (!checkedFloat(rows[3][i] - static_cast<double>(jitter.ndcX) * rows[0][i] -
                          static_cast<double>(jitter.ndcY) * rows[1][i], next[3][i])) return false;
    std::memcpy(rows, next, sizeof(next));
    return true;
}

// PS 7EAC... b2[1..4]: ray.xyz = ndc.x*row1.xyz + ndc.y*row2.xyz + row4.xyz.
// The shader uses row4.xyz only; preserve its w and the depth row3 bit exactly.
inline bool flatJitterInverseScreenRay(float (&rows)[4][4], const FlatProjectionJitter& jitter) {
    using namespace flat_projection_detail;
    if (!finite(jitter) || !finite(rows)) return false;
    if (jitter.ndcX == 0 && jitter.ndcY == 0) return true;
    float next[4][4]; std::memcpy(next, rows, sizeof(next));
    for (size_t i = 0; i < 3; ++i)
        if (!checkedFloat(rows[3][i] - static_cast<double>(jitter.ndcX) * rows[0][i] -
                          static_cast<double>(jitter.ndcY) * rows[1][i], next[3][i])) return false;
    std::memcpy(rows, next, sizeof(next));
    return true;
}

} // namespace edvr
