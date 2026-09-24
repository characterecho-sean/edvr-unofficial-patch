// Small, platform-independent parts of the flat discovery admission contract.
#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>

namespace edvr {

template<class T, size_t N, class Match>
T* flatFindOrAdd(T (&items)[N], uint32_t& used, uint32_t& overflow,
                 Match&& matches) {
    for (uint32_t i = 0; i < used; ++i)
        if (matches(items[i])) return &items[i];
    if (used == N) { ++overflow; return nullptr; }
    T* item = &items[used++];
    *item = T{};
    return item;
}

inline bool flatCaptureExpired(uint64_t startMs, uint64_t nowMs,
                               uint32_t presents, uint64_t maxMs,
                               uint32_t maxPresents) {
    return nowMs - startMs >= maxMs || presents >= maxPresents;
}

inline bool flatCaptureThreadEligible(bool active, uint32_t owner,
                                      uint32_t caller) {
    return active && owner != 0 && caller == owner;
}

// A discovered shape is never a certificate. These flags require independent
// desktop evidence; this discovery build deliberately sets none of them.
struct FlatTemporalProof {
    bool sceneAndCamera = false;
    bool consumedProjection = false;
    bool matchedDepthAndMotion = false;
    bool completionAndUiOrder = false;
    bool outputAndModOrder = false;
    bool jitterRollback = false;
    bool unknownDeferredWork = false;
    bool duplicateTreatment = false;
};
inline bool flatTemporalEvidenceComplete(const FlatTemporalProof& p) {
    return p.sceneAndCamera && p.consumedProjection &&
           p.matchedDepthAndMotion && p.completionAndUiOrder &&
           p.outputAndModOrder && p.jitterRollback &&
           !p.unknownDeferredWork && !p.duplicateTreatment;
}
}  // namespace edvr
