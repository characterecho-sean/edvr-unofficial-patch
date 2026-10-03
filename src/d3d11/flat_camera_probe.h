#pragma once
#include <cstddef>
#include <cstdint>

namespace edvr {
// F10 diagnostic budget only. These pairs receive no rendering or camera admission.
struct FlatCameraProbeKey { uint64_t vs, ps; };
inline constexpr FlatCameraProbeKey kFlatCameraProbePairs[] = {
    {0x88DCF1164C640EC3ull, 0x494506A63091DF8Cull}, // qualified laser rifle
    {0x025B4B9FF54622EDull, 0x46F92DC71BF8DFA5ull}, // new weapon/tool material
    {0x9AEC596A2B036EA6ull, 0x3789CA2062E196FBull}, // new effect
};
inline constexpr size_t kFlatCameraProbePairCount =
    sizeof(kFlatCameraProbePairs) / sizeof(kFlatCameraProbePairs[0]);

struct FlatCameraProbePair {
    uint64_t observed = 0, conflicts = 0, firstFrame = 0, lastFrame = 0;
    uint32_t attempts = 0, complete = 0, missing = 0, actualMismatch = 0;
    const char* result() const {
        return !observed ? "exact-pair-never-observed" : !conflicts ? "observed-without-HDR-camera-conflict" :
            actualMismatch ? "actual-shader-mismatch" : missing ? "partial-missing-evidence" :
            complete ? "captured" : "conflict-without-capture";
    }
};

struct FlatCameraProbe {
    FlatCameraProbePair pairs[kFlatCameraProbePairCount]{};
    // Token 1..6 names both pair and attempt, linking detailed lines to one summary.
    static constexpr uint32_t token(size_t pair, uint32_t attempt) {
        return uint32_t(pair * 2 + attempt);
    }
    static constexpr size_t pairForToken(uint32_t value) { return (value - 1) / 2; }
    static constexpr size_t pairIndex(uint64_t vs, uint64_t ps) {
        for (size_t i = 0; i < kFlatCameraProbePairCount; ++i)
            if (kFlatCameraProbePairs[i].vs == vs && kFlatCameraProbePairs[i].ps == ps) return i;
        return kFlatCameraProbePairCount;
    }
    uint32_t begin(bool armed, uint64_t vs, uint64_t ps, uint64_t frame, bool conflict) {
        if (!armed) return 0;
        const size_t i = pairIndex(vs, ps);
        if (i == kFlatCameraProbePairCount) return 0;
        auto& p = pairs[i];
        ++p.observed;
        if (!conflict) return 0;
        ++p.conflicts;
        if (p.attempts == 2 || (p.attempts && frame <= p.lastFrame)) return 0;
        if (!p.attempts) p.firstFrame = frame;
        p.lastFrame = frame; ++p.attempts;
        return token(i, p.attempts);
    }
    void finish(uint32_t value, bool available, bool actualMatches) {
        if (!value || value > kFlatCameraProbePairCount * 2) return;
        auto& p = pairs[pairForToken(value)];
        if (!actualMatches) ++p.actualMismatch;
        else if (available) ++p.complete;
        else ++p.missing;
    }
};
static_assert(kFlatCameraProbePairCount == 3, "camera probe pair budget changed");
} // namespace edvr
