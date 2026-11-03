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
// How it is made, from EDVR's own noise (section 13 of the doc): every band is itself made of strokes, long along one diagonal flow (about 49 degrees, image
// coordinates, y down) and narrow across it, with a slow periodic bend.
//   strokes  three layers of periodic value noise in the diagonal coordinates u = x - y (across) and v = x + y (along), a lattice coarse across and coarser still along
//            it: wide soft bands, narrower bands, and fine strokes. u and v both have period `size`, so every layer tiles on a diagonal flow with no seam.
//   fibres   white noise smeared along the flow (line integral convolution), thin and faint.
//   grain    a little isotropic noise, only in the finest band.
// The layers are standardised and added with variance shares that set the band energies, and the sum goes through an exponential, so the values are positive with a soft
// upper tail and nothing reaches black; the exponent is found by bisection so the stored mean is the target with the maximum at 1.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace edvr {
namespace glass_streaks {

constexpr int kSize = 512;          // texels a side; the texture wraps
constexpr int kMips = 10;           // 512 down to 1
// The layers. Three stroke layers are periodic value noise in the diagonal coordinates u = x - y (across the strokes) and v = x + y (along them), both of period `size`, so a
// layer is periodic in x and in y and tiles; its lattice of kCu x kCv cells is long along v where kCv is small. The across coordinate is bent by a slow periodic noise of v.
// The finest stroke layer is white noise smeared along the flow (line integral convolution), and there is a little isotropic grain. Each layer has a share of the variance.
constexpr float kFlowAngle = 0.85f;     // the fibres' direction, about 49 degrees in image coordinates (y down): upper-left to lower-right
constexpr int kCu0 = 3, kCv0 = 1;       // the coarsest strokes
constexpr int kCu1 = 12, kCv1 = 3;
constexpr int kCu2 = 40, kCv2 = 16;
constexpr int kHalf3 = 14;              // the fibres' half length in texels (at kSize)
constexpr float kShare0 = 0.38f, kShare1 = 0.20f, kShare2 = 0.26f, kShare3 = 0.07f, kShareGrain = 0.07f;
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
    // The fibres' flow: about kFlowAngle with a slow periodic bend in the angle (two cells across, about +-0.1 rad), a function of x alone, so a wrapped path stays on the same field.
    std::vector<float> flowX(size), flowY(size);
    for (int x = 0; x < size; ++x) {
        const float bend = (periodicNoise(static_cast<float>(x) + 0.5f, 0.5f, size, 2, 1, 0x57A71C01u) - 0.5f) * 0.2f;
        flowX[x] = std::cos(kFlowAngle + bend);
        flowY[x] = std::sin(kFlowAngle + bend);
    }
    // White noise smeared along the flow, a triangular kernel of +-half texels: thin fibres.
    auto fibres = [&](const std::vector<float>& base, int half) {
        std::vector<float> res(n);
        const int steps = std::max(2, static_cast<int>(std::lround(half * scale)));
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                float sum = base[static_cast<size_t>(y) * size + x] * static_cast<float>(steps + 1), weight = static_cast<float>(steps + 1);
                for (int dir = -1; dir <= 1; dir += 2) {
                    float qx = static_cast<float>(x) + 0.5f, qy = static_cast<float>(y) + 0.5f;
                    for (int k = 1; k <= steps; ++k) {
                        const int ix = wrap(static_cast<int>(std::floor(qx)), size);
                        qx += static_cast<float>(dir) * flowX[ix];
                        qy += static_cast<float>(dir) * flowY[ix];
                        const float w = static_cast<float>(steps + 1 - k);
                        sum += w * base[static_cast<size_t>(wrap(static_cast<int>(std::floor(qy)), size)) * size + wrap(static_cast<int>(std::floor(qx)), size)];
                        weight += w;
                    }
                }
                res[static_cast<size_t>(y) * size + x] = sum / weight;
            }
        return res;
    };
    std::vector<float> l0(n), l1(n), l2(n), b3(n), grain(n);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int i = y * size + x;
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float v = px + py;
            const float u = px - py + 0.2f * static_cast<float>(size) * (periodicNoise(v, 0.5f, size, 2, 1, 0x57A71C02u) - 0.5f);
            l0[i] = periodicNoise(u, v, size, kCu0, kCv0, 0x5780AE01u);
            l1[i] = periodicNoise(u, v, size, kCu1, kCv1, 0x5780AE02u);
            l2[i] = periodicNoise(u, v, size, kCu2, kCv2, 0x5780AE03u);
            b3[i] = hash01(x, y, 0xC0FFEE01u);
            grain[i] = hash01(x, y, 0x6A41A001u);
        }
    std::vector<float> l3 = fibres(b3, kHalf3);
    standardise(l0); standardise(l1); standardise(l2); standardise(l3); standardise(grain);
    std::vector<float> z(n);
    for (int i = 0; i < n; ++i) z[i] = std::sqrt(kShare0) * l0[i] + std::sqrt(kShare1) * l1[i] + std::sqrt(kShare2) * l2[i] + std::sqrt(kShare3) * l3[i] + std::sqrt(kShareGrain) * grain[i];
    standardise(z);
    // exp(s z), normalised to its maximum: s by bisection so the mean is the target (the mean falls as s rises).
    float top = 0;
    for (int i = 0; i < n; ++i) top = std::max(top, z[i]);
    float lo = 0.01f, hi = 2.0f;
    for (int it = 0; it < 40; ++it) {
        const float s = 0.5f * (lo + hi);
        double m = 0;
        for (int i = 0; i < n; ++i) m += std::exp(s * (z[i] - top));
        m /= n;
        if (m > kTargetMean) lo = s; else hi = s;
    }
    const float s = 0.5f * (lo + hi);
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
