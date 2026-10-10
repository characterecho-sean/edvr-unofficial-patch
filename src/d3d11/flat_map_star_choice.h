// The System Map's stars choice (2026-10-10): the CPU mirror of the rule the prep shader applies (flat_mono_shader_source.h, starLog,
// starChoice). The same constant, the same log luminance and the same pick, so tools\ui_quality_test pins the rule the shader
// computes. The shader's candidate-validity tests (finite and in-range positions) and the bilinear taps are not mirrored here.
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {

// The sum over a 3x3 of |log luminance difference| that both candidates must beat for a tie (kStarTieEpsilon in the shader).
constexpr float kFlatStarTieEpsilon = 0.05f;

// The shader's starLog: log of the Rec.709 luminance (negatives clamped to zero) plus 1e-4.
inline float flatStarLog(float r, float g, float b) {
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return std::log((y > 0.0f ? y : 0.0f) + 1e-4f);
}

// The sum of |cur - prev| over the nine taps of the 3x3 (the shader's sa and sb).
inline float flatStarSad(const float cur[9], const float prev[9]) {
    float s = 0.0f;
    for (int i = 0; i < 9; ++i) s += std::fabs(cur[i] - prev[i]);
    return s;
}

// What the shader picks: plane (candidate A, also the tie's), still (candidate B).
enum class FlatStarPick : uint8_t { Plane = 0, Still = 1, Tie = 2 };

// The rule. A non-finite sum keeps the plane. Both sums under the epsilon are a tie (the plane is kept). Otherwise the lower sum wins,
// and the still term only when it is strictly lower.
inline FlatStarPick flatStarPick(float sumPlane, float sumStill) {
    if (!std::isfinite(sumPlane) || !std::isfinite(sumStill)) return FlatStarPick::Plane;
    if (sumPlane < kFlatStarTieEpsilon && sumStill < kFlatStarTieEpsilon) return FlatStarPick::Tie;
    return sumStill < sumPlane ? FlatStarPick::Still : FlatStarPick::Plane;
}

}  // namespace edvr
