#pragma once
// visual.cockpit_glass = legacy: EDVR's own dirt pattern for the canopy (docs\cockpit-glass-legacy-port.md section 10 and 11). The legacy client's canopy carries a
// dark, faint, swirled wipe-streak texture over the whole pane; that texture is Frontier's artwork and is not shipped. This is the stand-in EDVR makes itself, in
// the same style: long thin curved fibres that follow a slowly turning flow field, in broad patches, like a cloth wipe on glass. Nothing here is traced from the
// legacy texture; the only thing taken from it is two statistics, its linear mean (0.024) and max (0.09), which the shader's scale and the mean below reproduce.
//
// A pure function: no Windows API, no globals, deterministic (the same bytes every call, on every machine), and tileable (every field is periodic in the size, so the
// texture wraps with no seam). The result is a single channel, stored normalised: the brightest texel is 255 and the mean is kTargetMean of it, so the shader's one
// scale (kStreakMax, 0.09) gives the legacy max, and the mean comes out at the legacy 0.024. tools\cockpit_glass_test holds all of this.
//
// How it is made: a white-noise field is smeared along the flow field (line integral convolution, a triangular kernel of +-kHalfLength texels, the path followed
// texel by texel through a precomputed direction grid, wrapping), which turns the noise into curved fibres; the fibre contrast is stretched, broad patches (low
// frequency periodic noise) modulate how much of the pane carries streaks, and a final power curve is solved by bisection so the mean is the target with the max at 1.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace edvr {
namespace glass_streaks {

constexpr int kSize = 256;          // texels a side; the texture wraps
constexpr int kMips = 9;            // 256 down to 1
constexpr int kHalfLength = 18;     // texels either side of a texel the streak is smeared over
constexpr float kLegacyMean = 0.024f;   // the legacy dirt texture's linear mean (section 10)
constexpr float kLegacyMax = 0.09f;     // and its max
// The shader multiplies the stored value by this, so the stored 1.0 is the legacy max.
constexpr float kStreakMax = kLegacyMax;
// The stored mean that, times kStreakMax, is the legacy mean.
constexpr float kTargetMean = kLegacyMean / kLegacyMax;

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
inline float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Value noise on a lattice of `cells` x `cells`, periodic over [0, size): the lattice wraps, so the field does.
inline float periodicNoise(float x, float y, int size, int cells, uint32_t seed) {
    const float f = static_cast<float>(cells) / static_cast<float>(size);
    const float gx = x * f, gy = y * f;
    const int x0 = static_cast<int>(std::floor(gx)), y0 = static_cast<int>(std::floor(gy));
    const float tx = smooth(gx - static_cast<float>(x0)), ty = smooth(gy - static_cast<float>(y0));
    auto cell = [&](int cx, int cy) { return hash01(((cx % cells) + cells) % cells, ((cy % cells) + cells) % cells, seed); };
    const float a = cell(x0, y0), b = cell(x0 + 1, y0), c = cell(x0, y0 + 1), d = cell(x0 + 1, y0 + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

inline int wrap(int v, int size) { return ((v % size) + size) % size; }

}  // namespace detail

// size x size bytes, row-major, the streak pattern as described above. `size` must be a power of two of at least 16.
inline void generate(uint8_t* out, int size = kSize) {
    using namespace detail;
    const int n = size * size;
    const float scale = static_cast<float>(size) / static_cast<float>(kSize);   // the lengths are for kSize; a bigger texture lengthens them
    const int half = static_cast<int>(std::lround(kHalfLength * scale));
    std::vector<float> noise(n), dx(n), dy(n), smear(n), patch(n);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int i = y * size + x;
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            noise[i] = hash01(x, y, 0xC0FFEE01u);
            // The flow: a broad turning (three cells across) with a finer meander (seven), so a fibre bends gently and a neighbouring one curves a little differently.
            const float turn = 6.2831853f * periodicNoise(px, py, size, 3, 0x51A7E001u) + 2.4f * (periodicNoise(px, py, size, 7, 0x51A7E002u) - 0.5f);
            dx[i] = std::cos(turn);
            dy[i] = std::sin(turn);
            // The patches: where the wipe went, low frequency, then pushed toward 0 or 1 so there are bare areas between.
            const float p = 0.6f * periodicNoise(px, py, size, 3, 0x9A7C4001u) + 0.4f * periodicNoise(px, py, size, 6, 0x9A7C4002u);
            patch[i] = smooth(std::min(1.0f, std::max(0.0f, (p - 0.30f) / 0.40f)));
        }
    // Smear the noise along the flow, both ways from each texel, with a triangular weight.
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float sum = noise[y * size + x] * static_cast<float>(half + 1), weight = static_cast<float>(half + 1);
            for (int dir = -1; dir <= 1; dir += 2) {
                float qx = static_cast<float>(x) + 0.5f, qy = static_cast<float>(y) + 0.5f;
                for (int s = 1; s <= half; ++s) {
                    const int ix = wrap(static_cast<int>(qx), size), iy = wrap(static_cast<int>(qy), size);
                    const int j = iy * size + ix;
                    qx += static_cast<float>(dir) * dx[j];
                    qy += static_cast<float>(dir) * dy[j];
                    const float w = static_cast<float>(half + 1 - s);
                    sum += w * noise[wrap(static_cast<int>(qy), size) * size + wrap(static_cast<int>(qx), size)];
                    weight += w;
                }
            }
            smear[y * size + x] = sum / weight;
        }
    // The fibres as a contrast: z-scores of the smear, a dark floor with the brighter fibres standing out of it, modulated by the patches.
    double mean = 0, var = 0;
    for (int i = 0; i < n; ++i) mean += smear[i];
    mean /= n;
    for (int i = 0; i < n; ++i) var += (smear[i] - mean) * (smear[i] - mean);
    const float sd = static_cast<float>(std::sqrt(var / n)) + 1e-9f;
    std::vector<float> v(n);
    float top = 1e-9f;
    for (int i = 0; i < n; ++i) {
        const float z = (smear[i] - static_cast<float>(mean)) / sd;
        const float fibre = 0.15f + 0.85f * smooth(std::min(1.0f, std::max(0.0f, (z + 0.6f) / 2.6f)));
        v[i] = fibre * (0.12f + 0.88f * patch[i]);
        top = std::max(top, v[i]);
    }
    for (float& f : v) f /= top;   // the brightest texel is 1
    // The power curve that puts the mean at the target (the max stays 1): the mean falls as the exponent rises, so bisect.
    float lo = 0.2f, hi = 12.0f;
    for (int it = 0; it < 40; ++it) {
        const float g = 0.5f * (lo + hi);
        double m = 0;
        for (int i = 0; i < n; ++i) m += std::pow(v[i], g);
        m /= n;
        if (m > kTargetMean) lo = g; else hi = g;
    }
    const float g = 0.5f * (lo + hi);
    for (int i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(std::lround(255.0f * std::pow(v[i], g)));
}

// The mip chain of a wrapping texture, box filtered (a wrap is a no-op for a power-of-two box). `levels` receives kMips arrays, base first.
inline void buildMips(const uint8_t* base, int size, std::vector<std::vector<uint8_t>>& levels) {
    levels.clear();
    levels.emplace_back(base, base + size * size);
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
