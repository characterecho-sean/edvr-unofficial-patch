#pragma once

// CPU-only evidence about a draw's projection rows. A match here is a
// diagnostic; shader consumption, resource provenance and jitter admission are
// separate contracts owned by the caller.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace edvr {

enum class FlatProjectionOwnershipLayout : uint8_t {
    Unsupported,
    CanonicalVsB1, // candidate starts at VS b1[270], six float4 rows
    ForwardDp4     // candidate starts at a known forward b0/b2 matrix, four rows
};

enum class FlatProjectionOwnershipKind : uint8_t {
    Unavailable,          // missing, stale, short or non-finite CPU evidence
    Unsupported,          // no proven forward layout for these rows
    CanonicalSceneCamera, // current VS b1 identity and exact 96 camera bytes
    SceneBasisMatch,      // exact scene spatial/depth relation; translation reported
    Unmatched
};

struct FlatProjectionOwnershipInput {
    const void* referenceBuffer = nullptr; // selected current scene VS b1
    const unsigned char* referenceCamera = nullptr; // b1[270..275], raw 96 bytes
    size_t referenceBytes = 0;
    bool currentReference = false;
    const void* candidateBuffer = nullptr;
    const unsigned char* candidateRows = nullptr; // starts at the layout's first row
    size_t candidateBytes = 0;
    bool currentCandidate = false;
    FlatProjectionOwnershipLayout layout = FlatProjectionOwnershipLayout::Unsupported;
};

struct FlatProjectionOwnershipResult {
    FlatProjectionOwnershipKind kind = FlatProjectionOwnershipKind::Unavailable;
    bool residualsAvailable = false;
    // Absolute coefficient difference, including dp4 row 2 and near term.
    // These are observations, never thresholds or permission to jitter.
    double spatialDepthError = 0;
    // max |candidate translation + dot(scene spatial row, camera position)|
    // over clip x/y/w. The scene b0 camera-relative path need not have zero
    // translation, so this value never determines SceneBasisMatch.
    double translationResidual = 0;
};

inline FlatProjectionOwnershipResult flatClassifyProjectionOwnership(
    const FlatProjectionOwnershipInput& in) {
    FlatProjectionOwnershipResult out{};
    constexpr size_t kCameraBytes = 6u * 4u * sizeof(float);
    constexpr size_t kForwardBytes = 4u * 4u * sizeof(float);
    if (in.layout != FlatProjectionOwnershipLayout::CanonicalVsB1 &&
        in.layout != FlatProjectionOwnershipLayout::ForwardDp4) {
        out.kind = FlatProjectionOwnershipKind::Unsupported;
        return out;
    }
    if (!in.currentReference || !in.currentCandidate || !in.referenceBuffer ||
        !in.candidateBuffer || !in.referenceCamera || !in.candidateRows ||
        in.referenceBytes < kCameraBytes) return out;
    const size_t needed = in.layout == FlatProjectionOwnershipLayout::CanonicalVsB1
        ? kCameraBytes : kForwardBytes;
    if (in.candidateBytes < needed) return out;

    float camera[6][4];
    std::memcpy(camera, in.referenceCamera, sizeof(camera));
    for (const auto& row : camera)
        for (float value : row)
            if (!std::isfinite(value)) return out;
    // The measured scene-camera encoding: column-vector clip, zero clip-z
    // spatial terms, positive near, and repeated view direction in row 274.
    if (camera[0][2] != 0 || camera[1][2] != 0 || camera[2][2] != 0 ||
        camera[3][0] != 0 || camera[3][1] != 0 || camera[3][3] != 0 ||
        camera[3][2] <= 0 || camera[4][0] != camera[0][3] ||
        camera[4][1] != camera[1][3] || camera[4][2] != camera[2][3])
        return out;
    const double a = camera[0][0], b = camera[0][1], c = camera[0][3];
    const double d = camera[1][0], e = camera[1][1], f = camera[1][3];
    const double g = camera[2][0], h = camera[2][1], i = camera[2][3];
    const double determinant = (b * f - c * e) * g + (c * d - a * f) * h +
                               (a * e - b * d) * i;
    // Certify nonzero rank only outside the determinant's rounding error.
    // Each determinant term has at most five rounded operations on its path;
    // the six-term absolute sum has at most seven. gamma5/(1-gamma7)
    // therefore bounds error using the computed sum. Full machine epsilon
    // (twice unit roundoff) also conservatively covers computing this bound.
    // Float inputs cannot overflow/underflow these double triple products.
    // This is arithmetic uncertainty, not a camera-matching tolerance.
    const double magnitude = std::abs(b * f * g) + std::abs(c * e * g) +
        std::abs(c * d * h) + std::abs(a * f * h) +
        std::abs(a * e * i) + std::abs(b * d * i);
    constexpr double epsilon = std::numeric_limits<double>::epsilon();
    constexpr double gamma5 = 5 * epsilon / (1 - 5 * epsilon);
    constexpr double gamma7 = 7 * epsilon / (1 - 7 * epsilon);
    const double rankUncertainty = gamma5 / (1 - gamma7) * magnitude;
    if (!std::isfinite(determinant) || std::abs(determinant) <= rankUncertainty)
        return out;

    if (in.layout == FlatProjectionOwnershipLayout::CanonicalVsB1) {
        float candidate[6][4];
        std::memcpy(candidate, in.candidateRows, sizeof(candidate));
        for (const auto& row : candidate)
            for (float value : row)
                if (!std::isfinite(value)) return out;
        out.kind = in.candidateBuffer == in.referenceBuffer &&
            std::memcmp(in.candidateRows, in.referenceCamera, kCameraBytes) == 0
            ? FlatProjectionOwnershipKind::CanonicalSceneCamera
            : FlatProjectionOwnershipKind::Unmatched;
        return out;
    }

    float candidate[4][4];
    std::memcpy(candidate, in.candidateRows, sizeof(candidate));
    for (const auto& row : candidate)
        for (float value : row)
            if (!std::isfinite(value)) return out;

    bool exact = true;
    // The forward dp4 shader computes clip component i as dot(row i, input).
    // VS b1 instead sums input-weighted rows. Compare the measured transpose
    // for the three spatial columns, plus the complete depth row. No matrix
    // inversion, norm tolerance or local-transform compensation is inferred.
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            const float expected = camera[column][row];
            const double error = std::abs(static_cast<double>(candidate[row][column]) - expected);
            if (error > out.spatialDepthError) out.spatialDepthError = error;
            if (candidate[row][column] != expected) exact = false;
        }
    }
    const double depthError = std::abs(static_cast<double>(candidate[2][3]) - camera[3][2]);
    if (depthError > out.spatialDepthError) out.spatialDepthError = depthError;
    if (candidate[2][3] != camera[3][2]) exact = false;
    for (size_t row : {size_t(0), size_t(1), size_t(3)}) {
        double residual = candidate[row][3];
        for (size_t column = 0; column < 3; ++column)
            residual += static_cast<double>(camera[column][row]) * camera[5][column];
        residual = std::abs(residual);
        if (residual > out.translationResidual) out.translationResidual = residual;
    }
    out.residualsAvailable = true;
    out.kind = exact ? FlatProjectionOwnershipKind::SceneBasisMatch
                     : FlatProjectionOwnershipKind::Unmatched;
    return out;
}

} // namespace edvr
