// The System Map plane's depth range, as the flat reduction stores it and the CPU reads it back (flat_mono_shader_source.h, mapPlaneDepth;
// flat_mono_resolve.cpp, pollMapPlane). Two uint words, both reduced by InterlockedMin:
//   word 0: asuint of the nearest non-zero depth in (0, 1];
//   word 1: ~asuint of the farthest, so the farthest depth is the minimum of the inverted bits.
// Both words are cleared to kMapPlaneEmpty (0xFFFFFFFF) before each reduction. That is the empty state for both: word 0 at 0xFFFFFFFF
// means no non-zero depth in the frame, and word 1 at 0xFFFFFFFF inverts to 0, which can never be a far depth. The clear must write the
// value to every element: a UAV clear that replicates only its first value, or that leaves the second word at 0, reads back as NaN or as an
// empty far depth respectively, and decode refuses both.
//
// The shader decodes the same way (prep: lo = asfloat(span.x), hi = asfloat(~span.y)) and must keep that rule in step with this header.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {

constexpr uint32_t kMapPlaneEmpty = 0xFFFFFFFFu;

inline uint32_t mapPlaneBits(float d) {
    uint32_t b = 0;
    std::memcpy(&b, &d, sizeof(b));
    return b;
}

inline float mapPlaneFloat(uint32_t b) {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

// The word the reduction keeps for the farthest depth d: the inverted bits, so a minimum over the words is a maximum over the depths.
inline uint32_t mapPlaneFarWord(float d) { return ~mapPlaneBits(d); }

struct MapPlaneRange {
    bool valid = false;   // lo and hi are a usable range: finite, 0 < lo <= hi <= 1, and a finite midpoint
    float lo = 0.0f, hi = 0.0f;
    float midpoint() const { return 0.5f * (lo + hi); }
};

// The range the two words name, or invalid. An empty frame, a NaN, an inverted pair and a depth outside (0, 1] all answer invalid, and the
// prep then takes today's camera term, so a bad readback can never reach the motion.
inline MapPlaneRange mapPlaneDecode(uint32_t nearWord, uint32_t farWord) {
    MapPlaneRange r;
    if (nearWord == kMapPlaneEmpty) return r;
    const float lo = mapPlaneFloat(nearWord);
    const float hi = mapPlaneFloat(~farWord);
    if (!std::isfinite(lo) || !std::isfinite(hi)) return r;
    if (!(lo > 0.0f) || !(lo <= hi) || !(hi <= 1.0f)) return r;
    const float mid = 0.5f * (lo + hi);
    if (!std::isfinite(mid)) return r;
    r.valid = true;
    r.lo = lo;
    r.hi = hi;
    return r;
}

}  // namespace edvr
