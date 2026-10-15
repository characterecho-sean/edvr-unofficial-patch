#pragma once
// The sky ring (I-2 of docs\sky-flicker-2026-10-13.md): the pure half. TEMPORARY, behind diag.sky_ring (default off).
//
// One small compute dispatch per eye-frame, after the upscaler, samples a 128x128 grid of the eye's pixels, keeps those the scene depth calls far
// (the sky), and writes ONE 32-byte entry into a 2048-entry ring on the GPU: the frame, the eye, how many sky samples, the raw input's luma mean and
// max, the upscaler output's luma mean and max, how many output samples are bright, and the motion vectors' mean and max length over the sky. No
// per-frame readback. On a Pause press the ring is copied to a staging buffer and read back a few frames later without waiting, and this half decodes
// it, picks the frames around each camera-row event and the per-second summary, and formats the lines. Joined to the camera rows (camera_row_events.h)
// by frame number: the same clock as the history's TEMP lines.
//
// This header holds everything the shader and the readback must agree on -- the entry layout, the grid, the fixed-point scales -- and a CPU
// reference of the reduction the shader does, so tools\sky_flicker_test can run the real shader on synthetic textures and compare. Pure: no Windows,
// no globals.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace edvr::skyring {

constexpr uint32_t kRingEntries = 2048;     // eye-frames kept (1024 frames of two eyes)
constexpr uint32_t kEntryDwords = 8;
constexpr uint32_t kEntryBytes = kEntryDwords * 4;
constexpr uint32_t kRingBytes = kRingEntries * kEntryBytes;
constexpr uint32_t kGridDim = 128;          // the sample grid is kGridDim x kGridDim over the eye
constexpr uint32_t kSamples = kGridDim * kGridDim;
constexpr uint32_t kThreads = 1024;         // one group; each thread takes kSamples / kThreads samples
constexpr uint32_t kLumaScale = 4096;       // fixed point of a luma sum (0..1 per sample; 16384 samples fit a u32 many times over)
constexpr uint32_t kMvScale = 16;           // fixed point of a motion-length sum, 1/16 px
constexpr float kBrightLuma = 0.05f;        // an output block with a pixel above this is bright
constexpr uint32_t kWindowFrames = 45;      // frames either side of an event to print
constexpr uint32_t kMaxLines = 1500;        // the most SKY lines one dump prints
constexpr uint32_t kBucketFrames = 90;      // the per-second summary's bucket, in frames (the headset runs about 90 Hz)

// ---- the sample grid ------------------------------------------------------------------------------------------------------------------------------------------

// Sample s (0..kSamples-1) of an eye whose input is inW x inH: the grid cell's centre. Integer arithmetic, so the shader and this agree to the pixel.
inline void inputCoord(uint32_t s, uint32_t inW, uint32_t inH, uint32_t* x, uint32_t* y) {
    const uint32_t gx = s & (kGridDim - 1), gy = s / kGridDim;
    *x = ((2u * gx + 1u) * inW) / (2u * kGridDim);
    *y = ((2u * gy + 1u) * inH) / (2u * kGridDim);
}
// The output block covering input pixel (x, y): its top-left corner and its side (1 or 2, from the output/input width ratio).
inline void outputBlock(uint32_t x, uint32_t y, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH,
                        uint32_t* ox, uint32_t* oy, uint32_t* side) {
    const uint32_t ratio = inW ? outW / inW : 1u;
    *side = ratio < 1u ? 1u : (ratio > 2u ? 2u : ratio);
    *ox = inW ? static_cast<uint32_t>((static_cast<uint64_t>(x) * outW) / inW) : 0u;
    *oy = inH ? static_cast<uint32_t>((static_cast<uint64_t>(y) * outH) / inH) : 0u;
}
inline float lumaOf(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// A sky sample: the depth copy holds Elite's reversed-Z value, depth = A + B / z, so a pixel is far when the value is not above A (the temporal
// shader's own test: zr <= 0 || zr - A <= 0). With no depth this frame, nothing is sky.
inline bool isSky(float depth, float projA, bool depthValid) {
    return depthValid && (depth <= 0.0f || depth - projA <= 0.0f);
}

// ---- half floats (the entry packs six values in two dwords each) ----------------------------------------------------------------------------------------------

inline uint16_t f32ToF16(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    const uint32_t sign = (b >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((b >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = b & 0x7FFFFFu;
    if (((b >> 23) & 0xFFu) == 0xFFu) return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));   // inf, nan
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);                                              // overflow
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t h = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u), halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (h & 1u))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return static_cast<uint16_t>(sign | h);
}
inline float f16ToF32(uint16_t h) {
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    uint32_t b;
    if (exp == 0) {
        if (!mant) b = sign;
        else {
            int e = -1;
            uint32_t m = mant;
            do { ++e; m <<= 1; } while (!(m & 0x400u));
            b = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        b = sign | 0x7F800000u | (mant << 13);
    } else {
        b = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

// ---- the entry ------------------------------------------------------------------------------------------------------------------------------------------------

struct Entry {
    uint32_t seq = 0;            // 1-based dispatch number; 0 is an empty slot
    uint32_t frame = 0;
    uint32_t eye = 0;
    bool depthValid = false;     // the frame had a depth copy; without it skyN is 0 by construction
    bool expAvail = false;       // the exposure column holds a value (never set in this build)
    uint32_t skyN = 0;
    uint32_t bright = 0;
    float rawMean = 0, rawMax = 0, outMean = 0, outMax = 0, mvMean = 0, mvMax = 0;
    float exposure = 0;
};

// The shader's layout: dw0 frame, dw1 skyN | bright<<16, dw2..dw4 the half pairs (mean | max<<16), dw5 exposure bits, dw6 eye | depthValid<<1 | expAvail<<2, dw7 seq.
inline void pack(const Entry& e, uint32_t dw[kEntryDwords]) {
    dw[0] = e.frame;
    dw[1] = (e.skyN & 0xFFFFu) | ((e.bright & 0xFFFFu) << 16);
    dw[2] = f32ToF16(e.rawMean) | (static_cast<uint32_t>(f32ToF16(e.rawMax)) << 16);
    dw[3] = f32ToF16(e.outMean) | (static_cast<uint32_t>(f32ToF16(e.outMax)) << 16);
    dw[4] = f32ToF16(e.mvMean) | (static_cast<uint32_t>(f32ToF16(e.mvMax)) << 16);
    std::memcpy(&dw[5], &e.exposure, 4);
    dw[6] = (e.eye & 1u) | (e.depthValid ? 2u : 0u) | (e.expAvail ? 4u : 0u);
    dw[7] = e.seq;
}
// False for an empty slot (seq 0): the ring is zero-filled at creation.
inline bool unpack(const uint32_t dw[kEntryDwords], Entry* e) {
    if (dw[7] == 0) return false;
    e->frame = dw[0];
    e->skyN = dw[1] & 0xFFFFu;
    e->bright = dw[1] >> 16;
    e->rawMean = f16ToF32(static_cast<uint16_t>(dw[2] & 0xFFFFu)); e->rawMax = f16ToF32(static_cast<uint16_t>(dw[2] >> 16));
    e->outMean = f16ToF32(static_cast<uint16_t>(dw[3] & 0xFFFFu)); e->outMax = f16ToF32(static_cast<uint16_t>(dw[3] >> 16));
    e->mvMean = f16ToF32(static_cast<uint16_t>(dw[4] & 0xFFFFu)); e->mvMax = f16ToF32(static_cast<uint16_t>(dw[4] >> 16));
    std::memcpy(&e->exposure, &dw[5], 4);
    e->eye = dw[6] & 1u;
    e->depthValid = (dw[6] & 2u) != 0;
    e->expAvail = (dw[6] & 4u) != 0;
    e->seq = dw[7];
    return true;
}

// The slot of dispatch number seq (1-based): the ring wraps over the oldest.
inline uint32_t slotOf(uint32_t seq) { return (seq - 1u) % kRingEntries; }

// ---- the reduction, as the shader does it (the CPU reference) -------------------------------------------------------------------------------------------------

// One sample's inputs: the raw colour at the input pixel, the output block's pixels (side x side, up to 4), the depth value and the motion vector.
struct Sample {
    float rawRgb[3] = {};
    float outRgb[4][3] = {};
    uint32_t outN = 1;
    float depth = 0;
    float mv[2] = {};
};
struct Reducer {
    uint32_t skyN = 0, bright = 0;
    uint64_t rawSum = 0, outSum = 0, mvSum = 0;
    float rawMax = 0, outMax = 0, mvMax = 0;
    void add(const Sample& s, float projA, bool depthValid) {
        if (!isSky(s.depth, projA, depthValid)) return;
        ++skyN;
        const float raw = lumaOf(s.rawRgb[0], s.rawRgb[1], s.rawRgb[2]);
        float sum = 0, mx = 0;
        for (uint32_t i = 0; i < s.outN; ++i) {
            const float l = lumaOf(s.outRgb[i][0], s.outRgb[i][1], s.outRgb[i][2]);
            sum += l;
            mx = std::max(mx, l);
        }
        const float outMean = s.outN ? sum / static_cast<float>(s.outN) : 0.0f;
        const float mvLen = std::sqrt(s.mv[0] * s.mv[0] + s.mv[1] * s.mv[1]);
        rawSum += static_cast<uint32_t>(raw * kLumaScale + 0.5f);
        outSum += static_cast<uint32_t>(outMean * kLumaScale + 0.5f);
        mvSum += static_cast<uint32_t>(mvLen * kMvScale + 0.5f);
        rawMax = std::max(rawMax, raw);
        outMax = std::max(outMax, mx);
        mvMax = std::max(mvMax, mvLen);
        if (mx > kBrightLuma) ++bright;
    }
    Entry finish(uint32_t seq, uint32_t frame, uint32_t eye, bool depthValid) const {
        Entry e;
        e.seq = seq; e.frame = frame; e.eye = eye; e.depthValid = depthValid;
        e.skyN = skyN; e.bright = bright;
        if (skyN) {
            const float n = static_cast<float>(skyN);
            e.rawMean = static_cast<float>(rawSum) / (static_cast<float>(kLumaScale) * n);
            e.outMean = static_cast<float>(outSum) / (static_cast<float>(kLumaScale) * n);
            e.mvMean = static_cast<float>(mvSum) / (static_cast<float>(kMvScale) * n);
        }
        e.rawMax = rawMax; e.outMax = outMax; e.mvMax = mvMax;
        return e;
    }
};

// ---- decoding a readback --------------------------------------------------------------------------------------------------------------------------------------

// The ring's dwords (kRingEntries * kEntryDwords) as entries, oldest first (by dispatch number). Empty slots are skipped.
inline std::vector<Entry> decodeRing(const uint32_t* dwords) {
    std::vector<Entry> out;
    for (uint32_t i = 0; i < kRingEntries; ++i) {
        Entry e;
        if (unpack(dwords + static_cast<size_t>(i) * kEntryDwords, &e)) out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.seq < b.seq; });
    return out;
}

// One line to print: an entry, and where it sits against the nearest event.
struct Pick {
    size_t entry = 0;
    int32_t delta = 0;           // frames from the nearest event's first frame (negative before it)
    bool inEvent = false;        // the entry's frame is inside an event's run
};

// The entries within kWindowFrames of an event's first frame (or inside its run), in ring order, capped at maxLines. The newest events are taken first
// when the cap bites, so what is cut is the oldest. truncated says whether the cap bit.
inline std::vector<Pick> windowsAround(const std::vector<Entry>& entries, const std::vector<std::pair<uint32_t, uint32_t>>& events /*frame, run*/,
                                       uint32_t maxLines, bool* truncated) {
    std::vector<Pick> out;
    if (truncated) *truncated = false;
    if (events.empty()) return out;
    std::vector<std::pair<uint32_t, uint32_t>> ev = events;
    std::sort(ev.begin(), ev.end());
    std::vector<char> take(entries.size(), 0);
    std::vector<Pick> picks(entries.size());
    // Newest event first, so a cap drops the oldest.
    uint32_t taken = 0;
    for (size_t k = ev.size(); k-- > 0;) {
        const uint32_t ef = ev[k].first, run = ev[k].second ? ev[k].second : 1u;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (take[i]) continue;
            const int64_t d = static_cast<int64_t>(entries[i].frame) - static_cast<int64_t>(ef);
            if (d < -static_cast<int64_t>(kWindowFrames) || d >= static_cast<int64_t>(run + kWindowFrames)) continue;
            if (taken >= maxLines) { if (truncated) *truncated = true; continue; }
            take[i] = 1;
            ++taken;
            picks[i].entry = i;
            picks[i].delta = static_cast<int32_t>(d);
            picks[i].inEvent = d >= 0 && d < static_cast<int64_t>(run);
        }
    }
    for (size_t i = 0; i < entries.size(); ++i)
        if (take[i]) out.push_back(picks[i]);
    return out;
}

struct Bucket {
    uint32_t firstFrame = 0, lastFrame = 0;
    uint32_t eyeFrames = 0, withSky = 0;
    double sky = 0, raw = 0, out = 0, bright = 0, mv = 0;
    float outMin = 1e9f, outMax = 0, mvMax = 0;
    uint32_t mvMaxFrame = 0;
};

// The per-second summary: buckets of kBucketFrames frames over the entries (frame / kBucketFrames), means over the eye-frames with sky samples.
inline std::vector<Bucket> bucketsOf(const std::vector<Entry>& entries) {
    std::vector<Bucket> out;
    for (const Entry& e : entries) {
        const uint32_t key = e.frame / kBucketFrames;
        if (out.empty() || out.back().firstFrame / kBucketFrames != key) {
            Bucket b;
            b.firstFrame = b.lastFrame = e.frame;
            out.push_back(b);
        }
        Bucket& b = out.back();
        b.firstFrame = std::min(b.firstFrame, e.frame);
        b.lastFrame = std::max(b.lastFrame, e.frame);
        ++b.eyeFrames;
        if (!e.skyN) continue;
        ++b.withSky;
        b.sky += e.skyN; b.raw += e.rawMean; b.out += e.outMean; b.bright += e.bright; b.mv += e.mvMean;
        b.outMin = std::min(b.outMin, e.outMean);
        b.outMax = std::max(b.outMax, e.outMean);
        if (e.mvMax > b.mvMax) { b.mvMax = e.mvMax; b.mvMaxFrame = e.frame; }
    }
    return out;
}

// ---- lines ----------------------------------------------------------------------------------------------------------------------------------------------------

// "SKY f36358 eye=0 n=5120 raw=0.0123 rawmax=0.912 out=0.0119 outmax=0.874 bright=12 mv=0.34 mvmax=2.10 exp=n/a". The caller appends the join.
inline int formatEntry(char* buf, size_t n, const Entry& e) {
    char exp[24];
    if (e.expAvail) std::snprintf(exp, sizeof(exp), "%.4g", static_cast<double>(e.exposure)); else std::snprintf(exp, sizeof(exp), "n/a");
    const int len = std::snprintf(buf, n,
        "SKY f%u eye=%u n=%u%s raw=%.4f rawmax=%.3f out=%.4f outmax=%.3f bright=%u mv=%.2f mvmax=%.2f exp=%s",
        e.frame, e.eye, e.skyN, e.depthValid ? "" : " (no depth)", static_cast<double>(e.rawMean), static_cast<double>(e.rawMax),
        static_cast<double>(e.outMean), static_cast<double>(e.outMax), e.bright, static_cast<double>(e.mvMean), static_cast<double>(e.mvMax), exp);
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

// "SKYSEC f36300-36389 eyeframes=180 sky=5100 raw=0.0121 out=0.0118 (0.0110..0.0131) bright=11.8 mv=0.31 mvmax=2.10@f36358".
inline int formatBucket(char* buf, size_t n, const Bucket& b) {
    const double d = b.withSky ? static_cast<double>(b.withSky) : 1.0;
    int len;
    if (!b.withSky)
        len = std::snprintf(buf, n, "SKYSEC f%u-%u eyeframes=%u no sky samples", b.firstFrame, b.lastFrame, b.eyeFrames);
    else
        len = std::snprintf(buf, n, "SKYSEC f%u-%u eyeframes=%u sky=%.0f raw=%.4f out=%.4f (%.4f..%.4f) bright=%.1f mv=%.2f mvmax=%.2f@f%u",
                            b.firstFrame, b.lastFrame, b.eyeFrames, b.sky / d, b.raw / d, b.out / d, static_cast<double>(b.outMin),
                            static_cast<double>(b.outMax), b.bright / d, b.mv / d, static_cast<double>(b.mvMax), b.mvMaxFrame);
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

// The dump's header: "sky ring: N eye-frames kept, frames A..B, M with sky samples" -- or, when the ring never ran, the line the brief asks for.
constexpr const char* kNoSamplesText = "sky ring: no samples (off or never ran)";
inline int formatNoSamples(char* buf, size_t n, bool keyOn, uint64_t dispatches) {
    const int len = std::snprintf(buf, n, "%s key=%s dispatches=%llu", kNoSamplesText, keyOn ? "on" : "off", static_cast<unsigned long long>(dispatches));
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}
inline int formatHeader(char* buf, size_t n, const std::vector<Entry>& entries, size_t withSky, size_t events, size_t shown, bool truncated) {
    const int len = std::snprintf(buf, n, "sky ring: %zu eye-frames kept, frames %u..%u, %zu with sky samples; %zu camera-row events, %zu SKY lines%s",
                                  entries.size(), entries.empty() ? 0u : entries.front().frame, entries.empty() ? 0u : entries.back().frame,
                                  withSky, events, shown, truncated ? " (truncated: the oldest events' windows are cut)" : "");
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

}  // namespace edvr::skyring
