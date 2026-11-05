#pragma once
// visual.cockpit_glass = legacy: EDVR's own dirt pattern for the canopy (docs\cockpit-glass-legacy-port.md sections 10 to 13). The legacy client's canopy carries a
// dark, faint wipe over the whole pane: many overlapping brush strokes with visible bristles and ragged ends, running one way with a gentle bend. That texture is
// Frontier's artwork and is not shipped. This is the stand-in EDVR makes itself. Only the legacy texture's summary statistics (its linear mean 0.024 and max 0.09,
// and the contrast figures of section 13) were used to set it; its pixels are not read, embedded, traced or fitted to by this code.
//
// A pure function: no Windows API, no globals, deterministic (the same bytes every call, on every machine), and tileable (every stroke is drawn modulo the tile, so the
// texture wraps with no seam). The result is a single channel, stored normalised: the brightest texel is 255 and the mean is kTargetMean of it, so the shader's one
// scale (kStreakMax, 0.09) gives the legacy max, and the mean comes out at the legacy 0.024. tools\cockpit_glass_test holds all of this.
//
// How it is made: the texture is PAINTED. Spectrum targets (band energy, elongation) cannot tell a painted stroke from stretched noise, because they throw the phase
// away: a stroke has an identity, edges and ends, which noise does not. So there are a few hundred strokes, large first and fine last, each composited over what is
// beneath it (a lerp by the stroke's own opacity), so later strokes overlap and cut across earlier ones. A stroke is:
//   path      a gentle quadratic arc, its direction about kFlowAngle plus a slowly varying bias across the tile (the flow bends) plus its own spread;
//   bristles  across its width a 1D noise profile of its own (nearly constant along the length, with a slow drift): parallel fibres running the length of the stroke;
//   ends      opacity ramps up at the start and frays out at the end, per bristle: each bristle starts and stops at its own distance, so the ends are ragged;
//   edges     fairly soft but definite across the width;
//   value     its own, in a narrow range around the base, so the contrast stays low.
// The painting is remapped by quantile onto legacy's summary distribution (percentile ratios to the mean), so the stored mean is the target and the maximum about 1.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace edvr {
namespace glass_streaks {

constexpr int kSize = 512;          // texels a side; the texture wraps
constexpr int kMips = 10;           // 512 down to 1
constexpr float kFlowAngle = 0.87f; // the strokes' direction, about 50 degrees in image coordinates (y down): upper-left to lower-right
constexpr float kLegacyMean = 0.024f;   // the legacy dirt texture's linear mean (section 10)
constexpr float kLegacyMax = 0.09f;     // and its max
// The shader multiplies the stored value by this, so the stored 1.0 is the legacy max.
constexpr float kStreakMax = kLegacyMax;
// The patch multiplier the shader takes from a high mip of this texture (cockpit_glass_shader.h: kPatchMip, kPatchContrast, kStreakStoredMean): 1 + contrast x (stored - mean) / mean.
constexpr int kPatchMip = 5;
constexpr float kPatchContrast = 0.7f;
constexpr float kStreakStoredMean = 0.2687f;
// The stored mean that, times kStreakMax, is the legacy mean.
constexpr float kTargetMean = kLegacyMean / kLegacyMax;

// The stroke classes, painted in this order: how many, their length (as a share of the tile) and width in texels (at kSize), their opacity range, the range of their own
// value around the base, and how much of a stroke's value and opacity its bristles take. Strokes are wide smears of paint, never lines: none is narrower than 60 texels.
struct StrokeClass { int count; float lenLo, lenHi, widthLo, widthHi, opacityLo, opacityHi, valueRange, bristleContrast; };
constexpr int kClassCount = 3;
constexpr StrokeClass kClasses[kClassCount] = {
    {40, 0.90f, 1.40f, 120.0f, 200.0f, 0.30f, 0.60f, 0.07f, 0.12f},   // broad smears
    {70, 0.50f, 1.10f, 60.0f, 130.0f, 0.25f, 0.50f, 0.05f, 0.13f},    // narrower smears
    {110, 0.30f, 0.70f, 40.0f, 70.0f, 0.20f, 0.40f, 0.05f, 0.14f},    // the narrowest: still broad, and soft-edged
};

namespace detail {

inline uint32_t mix(uint32_t h) {
    h ^= h >> 16; h *= 0x7feb352dU;
    h ^= h >> 15; h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}
inline float hash01(int x, int y, uint32_t seed) {
    return static_cast<float>(mix(static_cast<uint32_t>(x) * 0x9E3779B1U ^ mix(static_cast<uint32_t>(y) * 0x85EBCA77U ^ seed))) * (1.0f / 4294967296.0f);
}
inline float smooth(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t * t * (3.0f - 2.0f * t)); }
inline int wrap(int v, int size) { return ((v % size) + size) % size; }

// Value noise on a lattice of cellsX x cellsY over [0, size) in both axes, periodic: the lattice wraps, so the field does.
inline float periodicNoise(float x, float y, int size, int cellsX, int cellsY, uint32_t seed) {
    const float gx = x * static_cast<float>(cellsX) / static_cast<float>(size), gy = y * static_cast<float>(cellsY) / static_cast<float>(size);
    const int x0 = static_cast<int>(std::floor(gx)), y0 = static_cast<int>(std::floor(gy));
    const float tx = smooth(gx - static_cast<float>(x0)), ty = smooth(gy - static_cast<float>(y0));
    auto cell = [&](int cx, int cy) { return hash01(((cx % cellsX) + cellsX) % cellsX, ((cy % cellsY) + cellsY) % cellsY, seed); };
    const float a = cell(x0, y0), b = cell(x0 + 1, y0), c = cell(x0, y0 + 1), d = cell(x0 + 1, y0 + 1);
    const float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

// 1D value noise at x (lattice spacing 1), a function of a seed: the bristle profile.
inline float noise1(float x, uint32_t seed) {
    const int i = static_cast<int>(std::floor(x));
    const float t = smooth(x - static_cast<float>(i));
    const float a = hash01(i, 0, seed), b = hash01(i + 1, 0, seed);
    return a + (b - a) * t;
}

}  // namespace detail

// size x size bytes, row-major, the painted pattern described above. `size` must be a power of two of at least 16.
inline void generate(uint8_t* out, int size = kSize) {
    using namespace detail;
    const size_t n = static_cast<size_t>(size) * size;
    const float scale = static_cast<float>(size) / static_cast<float>(kSize);
    std::vector<float> field(n, 0.5f);          // the painting, a mid-dark base
    std::vector<float> markAlpha(n, 0.0f), markValue(n, 0.0f);   // one stroke's own coverage, per texel, kept at its maximum
    std::vector<uint32_t> markId(n, 0xFFFFFFFFu);
    std::vector<uint32_t> touched;
    uint32_t id = 0;
    for (int cls = 0; cls < kClassCount; ++cls) {
        const StrokeClass& c = kClasses[cls];
        for (int k = 0; k < c.count; ++k, ++id) {
            const uint32_t seed = 0x57A0C000u + static_cast<uint32_t>(cls) * 4096u + static_cast<uint32_t>(k);
            auto r = [&](int j) { return hash01(static_cast<int>(id), j, seed); };
            const float x0 = r(0) * static_cast<float>(size), y0 = r(1) * static_cast<float>(size);
            // The flow bends slowly across the tile (a periodic bias, +-0.35 rad) and each stroke has its own spread (+-0.25 rad).
            const float bias = (periodicNoise(x0, y0, size, 3, 3, 0xB1A50001u) - 0.5f) * 0.4f;
            const float theta = kFlowAngle + bias + (r(2) - 0.5f) * 0.20f;
            const float len = (c.lenLo + (c.lenHi - c.lenLo) * r(3)) * static_cast<float>(size);
            const float width = std::max(1.2f, (c.widthLo + (c.widthHi - c.widthLo) * r(4)) * scale);
            const float curve = (r(5) - 0.5f) * 0.06f;   // the arc's sideways bulge as a share of the length: nearly straight, well under the width
            const float opacity = c.opacityLo + (c.opacityHi - c.opacityLo) * r(6);
            const float value = 0.5f + (r(7) - 0.5f) * c.valueRange;   // its own value, in a narrow range around the base
            const float cs = std::cos(theta), sn = std::sin(theta);
            const float nx0 = -sn, ny0 = cs;
            const float bristle = std::max(1.0f, 3.5f * scale);   // texels per bristle across the width
            const float slowShift = (r(8) - 0.5f) * 2.0f;
            const float stepSize = 0.7f;
            const int steps = std::max(2, static_cast<int>(len / stepSize));
            const int across = std::max(2, static_cast<int>(width / stepSize));
            touched.clear();
            for (int ti = 0; ti <= steps; ++ti) {
                const float t = static_cast<float>(ti) / static_cast<float>(steps);
                // The quadratic arc and its tangent.
                const float bulge = curve * len * 4.0f * t * (1.0f - t);
                const float px = x0 + len * t * cs + nx0 * bulge, py = y0 + len * t * sn + ny0 * bulge;
                float tx = cs * len + nx0 * curve * len * 4.0f * (1.0f - 2.0f * t), ty = sn * len + ny0 * curve * len * 4.0f * (1.0f - 2.0f * t);
                const float tl = std::sqrt(tx * tx + ty * ty) + 1e-9f;
                tx /= tl; ty /= tl;
                const float nx = -ty, ny = tx;
                // The bristle profile drifts slowly along the stroke (a few texels over its length), the fibres otherwise running its whole length.
                const float drift = slowShift * 3.0f * scale * (t - 0.5f) + 2.0f * scale * (noise1(t * 3.0f, seed ^ 0x5151u) - 0.5f);
                const float startRamp = 0.10f + 0.06f * r(9), endRamp = 0.10f;
                for (int si = 0; si <= across; ++si) {
                    const float u = static_cast<float>(si) / static_cast<float>(across);   // 0..1 across the width
                    const float s = (u - 0.5f) * width;
                    const float sp = s + drift;
                    // The bristle noise, in the across coordinate only (this stroke's own): a fibre every bristle width, and a broader unevenness.
                    float b = 0.65f * noise1(sp / bristle, seed ^ 0xB415u) + 0.35f * noise1(sp / (bristle * 5.0f), seed ^ 0xB416u);
                    b = std::min(0.7f, std::max(0.3f, b));   // clamped: no single bristle stands out against its neighbours
                    // Each bristle starts and stops at its own distance along the stroke: ragged ends.
                    const int bi = static_cast<int>(std::floor(sp / bristle));
                    const float start = 0.02f + 0.10f * hash01(bi, 1, seed ^ 0xE0u), stop = 0.74f + 0.26f * hash01(bi, 2, seed ^ 0xE1u);
                    const float along = smooth((t - start) / startRamp) * (1.0f - smooth((t - (stop - endRamp)) / endRamp));
                    // The edges across the width: fairly soft, but definite.
                    const float edge = smooth(u / 0.33f) * smooth((1.0f - u) / 0.33f);
                    const float alpha = opacity * edge * along * ((1.0f - c.bristleContrast) + c.bristleContrast * b);
                    if (alpha <= 0.002f) continue;
                    const int ix = wrap(static_cast<int>(std::floor(px + nx * s)), size), iy = wrap(static_cast<int>(std::floor(py + ny * s)), size);
                    const size_t at = static_cast<size_t>(iy) * size + ix;
                    const float v = value * ((1.0f - 0.5f * c.bristleContrast) + c.bristleContrast * b);
                    if (markId[at] != id) {
                        markId[at] = id;
                        markAlpha[at] = alpha;
                        markValue[at] = v;
                        touched.push_back(static_cast<uint32_t>(at));
                    } else if (alpha > markAlpha[at]) {
                        markAlpha[at] = alpha;
                        markValue[at] = v;
                    }
                }
            }
            // Composited over what is beneath: a lerp by the stroke's own opacity, never additive.
            for (uint32_t at : touched) field[at] += (markValue[at] - field[at]) * markAlpha[at];
        }
    }
    // The remap is a quantile mapping onto the legacy texture's summary distribution (section 13): each texel keeps its RANK in the painting, and its value, as a ratio to the
    // mean, is read off the legacy texture's percentile ratios (p5 0.571, p50 0.884, p90 1.43, p95 1.59, p99 2.14, p99.9 2.55, a floor so nothing is black). Only those summary
    // figures are used. Spatial structure (the strokes) is the painting's; the distribution of values, and so how bright the bright parts are, is legacy's.
    static const float kQuantile[][2] = {{0.0f, 0.30f}, {1.0f, 0.52f}, {5.0f, 0.571f}, {10.0f, 0.62f}, {25.0f, 0.78f}, {50.0f, 0.884f}, {75.0f, 1.231f},
                                         {90.0f, 1.43f}, {95.0f, 1.591f}, {99.0f, 2.135f}, {99.9f, 2.551f}, {100.0f, 3.75f}};
    constexpr int kQuantiles = sizeof(kQuantile) / sizeof(kQuantile[0]);
    std::vector<uint32_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = static_cast<uint32_t>(i);
    std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return field[x] < field[y] || (field[x] == field[y] && x < y); });
    std::vector<float> ratio(n);
    double mean = 0;
    for (size_t rank = 0; rank < n; ++rank) {
        const float q = 100.0f * (static_cast<float>(rank) + 0.5f) / static_cast<float>(n);
        int k = 1;
        while (k < kQuantiles - 1 && kQuantile[k][0] < q) ++k;
        const float t = (q - kQuantile[k - 1][0]) / (kQuantile[k][0] - kQuantile[k - 1][0]);
        ratio[order[rank]] = kQuantile[k - 1][1] + (kQuantile[k][1] - kQuantile[k - 1][1]) * t;
        mean += ratio[order[rank]];
    }
    mean /= static_cast<double>(n);
    // The stored value is the ratio over the table's top: the maximum is 1 and the mean about the target.
    float top = 0;
    for (float r : ratio) top = std::max(top, r);
    const float norm = 1.0f / top;   // the brightest texel is 1; the mean is then (the table's mean ratio) / (its top ratio), within 1% of the target
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(std::lround(255.0f * ratio[i] * norm));
}

// The mip chain of a wrapping texture, box filtered (a wrap is a no-op for a power-of-two box). `levels` receives kMips arrays, base first.
inline void buildMips(const uint8_t* base, int size, std::vector<std::vector<uint8_t>>& levels) {
    levels.clear();
    levels.emplace_back(base, base + static_cast<size_t>(size) * size);
    for (int s = size / 2; s >= 1; s /= 2) {
        const std::vector<uint8_t>& prev = levels.back();
        std::vector<uint8_t> next(static_cast<size_t>(s) * s);
        for (int y = 0; y < s; ++y)
            for (int x = 0; x < s; ++x) {
                const int p = 2 * s;
                const int sum = prev[(2 * y) * p + 2 * x] + prev[(2 * y) * p + 2 * x + 1] + prev[(2 * y + 1) * p + 2 * x] + prev[(2 * y + 1) * p + 2 * x + 1];
                next[static_cast<size_t>(y) * s + x] = static_cast<uint8_t>((sum + 2) / 4);
            }
        levels.push_back(std::move(next));
    }
}

}  // namespace glass_streaks
}  // namespace edvr
