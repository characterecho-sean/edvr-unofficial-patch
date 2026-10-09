// The flat HDR route's luminance census: TEMPORARY instrument for the 2026-10-09 DLAA dimming entry in
// docs/design-flat-temporal-aa-2026-09-23.md (listed in its Status block; removed when that arc closes).
//
// Once a second, on a frame the HDR route resolves H, one compute pass (flat_hdr_luma_shader.h) counts the scene image
// the backend is handed (H before the resolve; the backend's private copy is made from it) and again H after the
// resolve has written it back (finishHdr). Each pass lands in its own small buffer, copied to a staging buffer read
// with DO_NOT_WAIT on later frames (two slots, so nothing stalls). Every 5 s one line, "flat hdr luma:", gives the
// in/out mean, the ratio, p99/p99.9, max, and three local-maximum counts: above an absolute threshold, above the same
// threshold scaled by the last out/in ratio, and scale-free peaks (twice their neighbours' mean).
//
// The pure half (layout, decode, percentiles) is here so tools\flat_temporal_test can pin it.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace edvr {

constexpr uint32_t kFlatHdrLumaBins = 64;
constexpr uint32_t kFlatHdrLumaStarsA = 64, kFlatHdrLumaStarsB = 65, kFlatHdrLumaPeaks = 66, kFlatHdrLumaMax = 67,
                   kFlatHdrLumaBad = 68, kFlatHdrLumaSums = 72, kFlatHdrLumaGroups = 32 * 32;
constexpr uint32_t kFlatHdrLumaWords = kFlatHdrLumaSums + kFlatHdrLumaGroups;

// A bin's upper edge: two bins a stop, bin 0 ending at 2^-15.5.
inline double flatHdrLumaBinUpper(uint32_t bin) { return std::exp2((static_cast<double>(bin) + 1.0) * 0.5 - 16.0); }

// The upper edge of the bin holding the q-quantile (0 < q <= 1); 0 for an empty histogram.
inline double flatHdrLumaPercentile(const uint32_t* hist, double q) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < kFlatHdrLumaBins; ++i) total += hist[i];
    if (!total) return 0.0;
    const double target = q * static_cast<double>(total);
    uint64_t running = 0;
    for (uint32_t i = 0; i < kFlatHdrLumaBins; ++i) {
        running += hist[i];
        if (static_cast<double>(running) >= target) return flatHdrLumaBinUpper(i);
    }
    return flatHdrLumaBinUpper(kFlatHdrLumaBins - 1);
}

struct FlatHdrLumaSample {
    double mean = 0, p99 = 0, p999 = 0, max = 0;
    uint32_t starsA = 0, starsB = 0, peaks = 0, bad = 0;
};

inline FlatHdrLumaSample flatHdrLumaDecode(const uint32_t* words, uint32_t width, uint32_t height) {
    FlatHdrLumaSample s{};
    double sum = 0;
    for (uint32_t g = 0; g < kFlatHdrLumaGroups; ++g) {
        float f; std::memcpy(&f, &words[kFlatHdrLumaSums + g], sizeof(f));
        sum += f;
    }
    s.bad = words[kFlatHdrLumaBad];
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    const uint64_t counted = pixels > s.bad ? pixels - s.bad : 0;
    s.mean = counted ? sum / static_cast<double>(counted) : 0.0;
    s.p99 = flatHdrLumaPercentile(words, 0.99);
    s.p999 = flatHdrLumaPercentile(words, 0.999);
    float mx; std::memcpy(&mx, &words[kFlatHdrLumaMax], sizeof(mx));
    s.max = mx;
    s.starsA = words[kFlatHdrLumaStarsA]; s.starsB = words[kFlatHdrLumaStarsB]; s.peaks = words[kFlatHdrLumaPeaks];
    return s;
}

// The DLSS preset number (dlaaSetPreset's) as its letter, for the line.
inline const char* flatHdrLumaPresetName(unsigned p) {
    switch (p) { case 0: return "auto"; case 10: return "J"; case 11: return "K"; case 12: return "L"; case 13: return "M"; default: return "other"; }
}

// The runtime half (flat_hdr_luma.cpp). Owner render thread only, inside FlatComputeInternalScope.
// Before the resolve: true when this frame samples (once a second, a slot free); `in` is counted now.
bool flatHdrLumaBegin(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, uint32_t width, uint32_t height);
// After a successful resolve on a sampling frame: H, now the backend's output, is counted into the same slot.
void flatHdrLumaEnd(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, const char* backend, unsigned preset,
                    bool exposureFixed);
// The resolve refused or fell back after flatHdrLumaBegin: the slot's input is dropped.
void flatHdrLumaAbandon();
// Every frame: reads finished slots without waiting, and says the 5 s line.
void flatHdrLumaPoll(ID3D11DeviceContext* ctx);

}  // namespace edvr
