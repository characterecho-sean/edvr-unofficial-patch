// F121b (native on-foot VR, measure only): a census of the HDR scene after the helmet visor's draw. The pure parts: header only, no game or D3D11 state, so the rig
// (tools\visor_axis_test) and remlok_fix.cpp run the very decode, CPU model and format strings the headset does. The shader is visor_census_shader.h. Journal: the arc
// doc, 2026-10-25, F121b.
//
// WHY. With F121's vertex shader bound the scene went black in the pilot's flight (two transitions at the first visor draw, log edvr_gfx_20261025_132827), and the
// visor's pixel shader, run offline on the shifted uv, gave nothing non-finite. One mechanism left standing: a few NaN or Inf pixels in the R11G11B10 HDR target (the
// format holds both) that the game's exposure or bloom pass then averages into a non-finite number, after which every frame tonemaps to black. This counts them.
//
// WHAT. Right after the visor draw in an eye (every sixth draw an eye), the HDR target is copied and one dispatch reads 256 x 256 taps of the copy (64 x 64 threads of
// 4 x 4 taps, tap s along an axis at pixel ((2 s + 1) extent) / 512): per tap, non-finite by the float bits (exponent all ones: NaN with a mantissa, else Inf) and,
// of the finite ones, black (luma under 1e-4), the largest channel and the luma. It also reads the first two texels of the visor pixel shader's t0, the game's exposure
// (the first two texels of the 6 x 1 texture), where that view can be read. Out (uints, kWords): see Word.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace visorcensus {

constexpr unsigned kThreads = 64;                       // threads along an axis (8 x 8 groups of 8 x 8)
constexpr unsigned kTaps = 4;                           // taps along a thread's axis
constexpr unsigned kAxis = kThreads * kTaps;            // 256 taps along an axis
constexpr unsigned kSamples = kAxis * kAxis;            // 65536 taps an eye sample
constexpr unsigned kWords = 20;                         // the output, in uints
constexpr unsigned kEvery = 6;                          // one sample every this many visor draws an eye
constexpr float kBlackLuma = 1.0e-4f;                   // a finite tap whose luma is under this is black
constexpr float kLumaClamp = 100.0f;                    // a thread's mean luma is clamped to this before the fixed point
constexpr float kLumaScale = 1024.0f;                   // the fixed point of a thread's mean luma (so the sum of 4096 threads fits 32 bits)
constexpr uint32_t kBboxBias = 0xFFFFu;                 // the minima are stored as kBboxBias - value, so a zero-cleared buffer and InterlockedMax do both ends

enum Word : unsigned {
    kSeq = 0, kWidth = 1, kHeight = 2, kTapsWord = 3,
    kNonFinite = 4, kNan = 5, kInf = 6, kBlack = 7,
    kMaxBits = 8, kLuma = 9,
    kMinXInv = 10, kMinYInv = 11, kMaxX = 12, kMaxY = 13,
    kExpo0 = 14, kExpo1 = 15, kExpoValid = 16
};

struct Census {
    bool ok = false;
    uint32_t width = 0, height = 0, taps = 0;
    uint32_t nonFinite = 0, nan = 0, inf = 0, black = 0;
    float maxFinite = 0.0f;
    double meanLuma = 0.0;                              // over all taps, a non-finite tap counting as 0
    uint32_t minX = 0, minY = 0, maxX = 0, maxY = 0;    // the box of the non-finite taps (valid when nonFinite > 0)
    bool expoValid = false;
    float expo0 = 0.0f, expo1 = 0.0f;
};

inline float bitsToFloat(uint32_t b) noexcept {
    float f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}
inline uint32_t floatToBits(float f) noexcept {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}
// The float's class by its bits, as the shader reads it (not by arithmetic: the shader compiler may fold isnan).
inline bool bitsNonFinite(uint32_t b) noexcept { return (b & 0x7F800000u) == 0x7F800000u; }
inline bool bitsNan(uint32_t b) noexcept { return bitsNonFinite(b) && (b & 0x007FFFFFu) != 0u; }

// The pixel tap s (0..kAxis-1) falls on, along an axis of the given extent.
inline uint32_t tapPixel(uint32_t s, uint32_t extent) noexcept {
    const uint64_t p = (static_cast<uint64_t>(2u * s + 1u) * extent) / (2u * kAxis);
    return static_cast<uint32_t>(p >= extent ? extent - 1u : p);
}

// The shader's words, decoded. False for a dispatch that is not the one asked for (the sequence, size or tap count differ), or for words that cannot be a census.
inline bool decode(const uint32_t* words, uint32_t seq, uint32_t width, uint32_t height, Census* out) noexcept {
    if (!words || !out) return false;
    if (words[kSeq] != seq || words[kWidth] != width || words[kHeight] != height || words[kTapsWord] != kSamples) return false;
    Census c;
    c.width = width;
    c.height = height;
    c.taps = words[kTapsWord];
    c.nonFinite = words[kNonFinite];
    c.nan = words[kNan];
    c.inf = words[kInf];
    c.black = words[kBlack];
    if (c.nan + c.inf != c.nonFinite || c.nonFinite > c.taps || c.black > c.taps - c.nonFinite) return false;
    c.maxFinite = bitsToFloat(words[kMaxBits]);
    c.meanLuma = static_cast<double>(words[kLuma]) / static_cast<double>(kLumaScale) / static_cast<double>(kThreads * kThreads);
    if (c.nonFinite) {
        c.minX = kBboxBias - words[kMinXInv];
        c.minY = kBboxBias - words[kMinYInv];
        c.maxX = words[kMaxX];
        c.maxY = words[kMaxY];
    }
    c.expoValid = words[kExpoValid] == 1u;
    if (c.expoValid) {
        c.expo0 = bitsToFloat(words[kExpo0]);
        c.expo1 = bitsToFloat(words[kExpo1]);
    }
    c.ok = true;
    *out = c;
    return true;
}

// The CPU's answer for the same taps over an image of float RGB (width x height x 3), the shader's own arithmetic: the rig compares the GPU's words with this.
inline Census cpuModel(const float* rgb, uint32_t width, uint32_t height, bool expoValid, float e0, float e1) noexcept {
    Census c;
    c.width = width;
    c.height = height;
    c.taps = kSamples;
    uint32_t lumaSum = 0, maxBits = 0;
    uint32_t minXInv = 0, minYInv = 0, maxX = 0, maxY = 0;
    for (uint32_t ty = 0; ty < kThreads; ++ty) {
        for (uint32_t tx = 0; tx < kThreads; ++tx) {
            float lsum = 0.0f, mx = 0.0f;
            for (uint32_t j = 0; j < kTaps; ++j) {
                for (uint32_t i = 0; i < kTaps; ++i) {
                    const uint32_t px = tapPixel(tx * kTaps + i, width), py = tapPixel(ty * kTaps + j, height);
                    const float* p = rgb + (static_cast<size_t>(py) * width + px) * 3u;
                    const uint32_t b[3] = {floatToBits(p[0]), floatToBits(p[1]), floatToBits(p[2])};
                    const bool bad = bitsNonFinite(b[0]) || bitsNonFinite(b[1]) || bitsNonFinite(b[2]);
                    if (bad) {
                        ++c.nonFinite;
                        if (bitsNan(b[0]) || bitsNan(b[1]) || bitsNan(b[2])) ++c.nan; else ++c.inf;
                        const uint32_t ix = kBboxBias - px, iy = kBboxBias - py;
                        if (ix > minXInv) minXInv = ix;
                        if (iy > minYInv) minYInv = iy;
                        if (px > maxX) maxX = px;
                        if (py > maxY) maxY = py;
                    } else {
                        const float lum = p[0] * 0.2126f + p[1] * 0.7152f + p[2] * 0.0722f;
                        lsum += lum;
                        const float m = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
                        if (m > mx) mx = m;
                        if (lum < kBlackLuma && m < kBlackLuma) ++c.black;
                    }
                }
            }
            float tm = lsum / static_cast<float>(kTaps * kTaps);
            if (tm > kLumaClamp) tm = kLumaClamp;
            lumaSum += static_cast<uint32_t>(tm * kLumaScale);
            const uint32_t mb = floatToBits(mx);
            if (mb > maxBits) maxBits = mb;
        }
    }
    c.maxFinite = bitsToFloat(maxBits);
    c.meanLuma = static_cast<double>(lumaSum) / static_cast<double>(kLumaScale) / static_cast<double>(kThreads * kThreads);
    if (c.nonFinite) {
        c.minX = kBboxBias - minXInv;
        c.minY = kBboxBias - minYInv;
        c.maxX = maxX;
        c.maxY = maxY;
    }
    c.expoValid = expoValid;
    c.expo0 = e0;
    c.expo1 = e1;
    c.ok = true;
    return c;
}

// One second of samples, by the key's state (a sample under the other state closes the second).
struct Window {
    bool keyOn = false;
    uint64_t samples = 0, eyeSamples[2] = {0, 0}, nonFinite = 0, nan = 0, inf = 0, black = 0, taps = 0;
    float maxFinite = 0.0f;
    double lumaSum = 0.0;                               // the sum of the samples' mean luma
    uint32_t width = 0, height = 0;
    uint32_t minX = 0xFFFFu, minY = 0xFFFFu, maxX = 0, maxY = 0;
    uint32_t expoRead = 0, expoNonFinite = 0;
    float expo0 = 0.0f, expo1 = 0.0f;                   // the last read
    uint32_t skipped = 0;                               // dispatches not answered in time, ring full, or refused
    void add(int eye, const Census& c) noexcept {
        ++samples;
        if (eye == 0 || eye == 1) ++eyeSamples[eye];
        nonFinite += c.nonFinite; nan += c.nan; inf += c.inf; black += c.black; taps += c.taps;
        if (c.maxFinite > maxFinite) maxFinite = c.maxFinite;
        lumaSum += c.meanLuma;
        width = c.width; height = c.height;
        if (c.nonFinite) {
            if (c.minX < minX) minX = c.minX;
            if (c.minY < minY) minY = c.minY;
            if (c.maxX > maxX) maxX = c.maxX;
            if (c.maxY > maxY) maxY = c.maxY;
        }
        if (c.expoValid) {
            ++expoRead;
            if (bitsNonFinite(floatToBits(c.expo0)) || bitsNonFinite(floatToBits(c.expo1))) ++expoNonFinite;
            expo0 = c.expo0; expo1 = c.expo1;
        }
    }
    double meanLuma() const noexcept { return samples ? lumaSum / static_cast<double>(samples) : 0.0; }
};

// A float for a log line: a number, or NaN / Inf by name (printf's own spelling of them differs between runtimes).
inline void floatText(char* out, size_t size, float f) noexcept {
    const uint32_t b = floatToBits(f);
    if (bitsNan(b)) std::snprintf(out, size, "NaN");
    else if (bitsNonFinite(b)) std::snprintf(out, size, (b >> 31) ? "-Inf" : "Inf");
    else std::snprintf(out, size, "%.4g", static_cast<double>(f));
}

inline int formatKey(char* out, size_t size, bool on) noexcept {
    return std::snprintf(out, size,
        "native on foot visor axis (F121b): %s (TEMPORARY, live; on = F121's vertex shader moves the visor's frame onto each eye's axis (the pilot's scene went black with it), off = "
        "the game's own visor vertex shader; remlok_lines outer and the F27 pixel shader variants run either way)", on ? "on" : "off");
}
inline int formatArmed(char* out, size_t size) noexcept {
    return std::snprintf(out, size,
        "visor axis (F121b): census armed: a copy of the HDR target after the visor draw (every sixth draw an eye) read at 256 x 256 taps for non-finite pixels, black, the largest "
        "value and the mean luma, and the visor's exposure texels; measure only; a line a second while visor draws come.");
}
inline int formatNote(char* out, size_t size, const char* what) noexcept {
    return std::snprintf(out, size, "visor axis (F121b): %s", what);
}
inline int formatLine(char* out, size_t size, const Window& w) noexcept {
    char where[96] = "";
    if (w.nonFinite) std::snprintf(where, sizeof(where), " at x %u-%u, y %u-%u of %ux%u", w.minX, w.maxX, w.minY, w.maxY, w.width, w.height);
    char e0[24], e1[24], mx[24];
    floatText(e0, sizeof(e0), w.expo0);
    floatText(e1, sizeof(e1), w.expo1);
    floatText(mx, sizeof(mx), w.maxFinite);
    return std::snprintf(out, size,
        "visor axis (F121b) 1 s: key %s; samples %llu (eye 0 %llu, eye 1 %llu) of %u taps; nonfinite %llu (NaN %llu, Inf %llu)%s; black %llu; max %s; mean %.4f; "
        "exposure e0 %s e1 %s (read in %u samples, non-finite in %u); dropped %u",
        w.keyOn ? "on" : "off", static_cast<unsigned long long>(w.samples), static_cast<unsigned long long>(w.eyeSamples[0]),
        static_cast<unsigned long long>(w.eyeSamples[1]), kSamples, static_cast<unsigned long long>(w.nonFinite), static_cast<unsigned long long>(w.nan),
        static_cast<unsigned long long>(w.inf), where, static_cast<unsigned long long>(w.black), mx, w.meanLuma(), e0, e1, w.expoRead, w.expoNonFinite, w.skipped);
}

}  // namespace visorcensus
}  // namespace edvr
