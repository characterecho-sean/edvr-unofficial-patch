#pragma once
// visual.cockpit_glass = legacy: EDVR's own dirt pattern for the canopy (docs\cockpit-glass-legacy-port.md sections 10 to 13). The legacy client's canopy carries a
// dark, faint, soft wipe over the whole pane: low-contrast strokes running one way with a gentle bend, faint fibres on top, no black holes. That texture is Frontier's
// artwork and is not shipped. This is the stand-in EDVR makes itself. Only the legacy texture's summary statistics were used to set it (its linear mean 0.024 and max
// 0.09, and the contrast, percentile, band-energy and direction figures in section 13); its pixels are not read, embedded or fitted to by this code.
//
// A pure function: no Windows API, no globals, deterministic (the same bytes every call, on every machine), and tileable (every field is periodic in the size, so the
// texture wraps with no seam). The result is a single channel, stored normalised: the brightest texel is 255 and the mean is kTargetMean of it, so the shader's one
// scale (kStreakMax, 0.09) gives the legacy max, and the mean comes out at the legacy 0.024. tools\cockpit_glass_test holds all of this.
//
// How it is made, from EDVR's own value noise:
//   flow    a mostly uniform direction (along x) with a slow periodic bend: the strokes follow y = const + warp(x), warp a low-frequency periodic noise of x alone.
//   blotch  very low frequency, isotropic: where the glass was wiped more.
//   strokes broad soft bands along the flow: value noise stretched along it, two scales.
//   fibres  white noise smeared along the flow (line integral convolution), a low-amplitude layer on top, with a little isotropic grain.
// The four are standardised and added with weights that set the band energies, and the sum goes through an exponential, so the values are positive with a soft long
// upper tail and nothing reaches black; the exponent is found by bisection so the stored mean is the target with the maximum at 1.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace edvr {
namespace glass_streaks {

constexpr int kSize = 512;          // texels a side; the texture wraps
constexpr int kMips = 10;           // 512 down to 1
constexpr int kHalfLength = 10;     // texels either side of a texel the fibres are smeared over
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

// Value noise on a lattice of cellsX x cellsY over [0, size) in both axes, periodic: the lattice wraps, so the field does. x and y may be any real number.
inline float periodicNoise(float x, float y, int size, int cellsX, int cellsY, uint32_t seed) {
    const float gx = x * static_cast<float>(cellsX) / static_cast<float>(size), gy = y * static_cast<float>(cellsY) / static_cast<float>(size);
    const int x0 = static_cast<int>(std::floor(gx)), y0 = static_cast<int>(std::floor(gy));
    const float tx = smooth(gx - static_cast<float>(x0)), ty = smooth(gy - static_cast<float>(y0));
    auto cell = [&](int cx, int cy) { return hash01(((cx % cellsX) + cellsX) % cellsX, ((cy % cellsY) + cellsY) % cellsY, seed); };
    const float a = cell(x0, y0), b = cell(x0 + 1, y0), c = cell(x0, y0 + 1), d = cell(x0 + 1, y0 + 1);
    const float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

inline int wrap(int v, int size) { return ((v % size) + size) % size; }

// Mean 0, standard deviation 1.
inline void standardise(std::vector<float>& f) {
    double mean = 0, var = 0;
    for (float v : f) mean += v;
    mean /= static_cast<double>(f.size());
    for (float v : f) var += (v - mean) * (v - mean);
    const double sd = std::sqrt(var / static_cast<double>(f.size())) + 1e-12;
    for (float& v : f) v = static_cast<float>((v - mean) / sd);
}

}  // namespace detail

// size x size bytes, row-major, the pattern described above. `size` must be a power of two of at least 16.
inline void generate(uint8_t* out, int size = kSize) {
    using namespace detail;
    const int n = size * size;
    const float scale = static_cast<float>(size) / static_cast<float>(kSize);   // the lengths are for kSize
    const int half = std::max(2, static_cast<int>(std::lround(kHalfLength * scale)));
    // The flow: the stroke centreline is displaced by warp(x), a slow periodic bend (two cells across, about +-6% of the size), so the direction at x is the slope of it.
    std::vector<float> warp(size), flowX(size), flowY(size);
    for (int x = 0; x < size; ++x) warp[x] = (periodicNoise(static_cast<float>(x) + 0.5f, 0.5f, size, 2, 1, 0x57A71C01u) - 0.5f) * 0.24f * static_cast<float>(size);
    for (int x = 0; x < size; ++x) {
        const float slope = (warp[wrap(x + 1, size)] - warp[wrap(x - 1, size)]) * 0.5f;
        const float len = std::sqrt(1.0f + slope * slope);
        flowX[x] = 1.0f / len;
        flowY[x] = slope / len;
    }
    std::vector<float> blotch(n), mid(n), fine(n), fibre(n), grain(n), noise(n);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int i = y * size + x;
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float yw = py - warp[x];   // the coordinate across the strokes
            blotch[i] = 0.7f * periodicNoise(px, py, size, 2, 2, 0xB10C4001u) + 0.3f * periodicNoise(px, py, size, 3, 3, 0xB10C4002u);
            mid[i] = periodicNoise(px, yw, size, 4, 12, 0x5780AE01u);
            fine[i] = periodicNoise(px, yw, size, 28, 56, 0x5780AE02u);
            grain[i] = 0.5f * periodicNoise(px, py, size, 24, 24, 0x6A41A001u) + 0.5f * periodicNoise(px, py, size, 130, 130, 0x6A41A002u);
            noise[i] = hash01(x, y, 0xC0FFEE01u);
        }
    // The fibres: the white noise smeared along the flow, both ways from each texel, with a triangular weight.
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float sum = noise[y * size + x] * static_cast<float>(half + 1), weight = static_cast<float>(half + 1);
            for (int dir = -1; dir <= 1; dir += 2) {
                float qx = static_cast<float>(x) + 0.5f, qy = static_cast<float>(y) + 0.5f;
                for (int s = 1; s <= half; ++s) {
                    const int ix = wrap(static_cast<int>(std::floor(qx)), size);
                    qx += static_cast<float>(dir) * flowX[ix];
                    qy += static_cast<float>(dir) * flowY[ix];
                    const float w = static_cast<float>(half + 1 - s);
                    sum += w * noise[wrap(static_cast<int>(std::floor(qy)), size) * size + wrap(static_cast<int>(std::floor(qx)), size)];
                    weight += w;
                }
            }
            fibre[y * size + x] = sum / weight;
        }
    standardise(blotch); standardise(mid); standardise(fine); standardise(fibre); standardise(grain);
    // The weights are the shares of the variance: very low frequency, broad strokes, finer strokes, fibres, and a little isotropic grain (so the strokes are not perfectly clean).
    const float wBlotch = std::sqrt(0.36f), wMid = std::sqrt(0.04f), wFine = std::sqrt(0.12f), wFibre = std::sqrt(0.18f), wGrain = std::sqrt(0.30f);
    std::vector<float> z(n);
    for (int i = 0; i < n; ++i) z[i] = wBlotch * blotch[i] + wMid * mid[i] + wFine * fine[i] + wFibre * fibre[i] + wGrain * grain[i];
    standardise(z);
    // exp(s z), normalised to its maximum: s by bisection so the mean is the target (the mean falls as s rises).
    std::vector<float> v(n);
    float lo = 0.01f, hi = 2.0f;
    for (int it = 0; it < 40; ++it) {
        const float s = 0.5f * (lo + hi);
        float top = 0;
        for (int i = 0; i < n; ++i) top = std::max(top, z[i]);
        double m = 0;
        for (int i = 0; i < n; ++i) m += std::exp(s * (z[i] - top));
        m /= n;
        if (m > kTargetMean) lo = s; else hi = s;
    }
    const float s = 0.5f * (lo + hi);
    float top = 0;
    for (int i = 0; i < n; ++i) top = std::max(top, z[i]);
    for (int i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(std::lround(255.0f * std::exp(s * (z[i] - top))));
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
