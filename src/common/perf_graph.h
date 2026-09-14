#pragma once
#include <cmath>

namespace edvr {
inline bool perfGraphSampleVisible(float ms, bool zeroIsValid) {
    return std::isfinite(ms) && (ms > 0.0f || (zeroIsValid && ms == 0.0f));
}
inline bool perfGraphHasReference(float ms) {
    return std::isfinite(ms) && ms > 0.0f && ms <= 600000.0f;
}
inline float perfGraphScale(const float* values, int count, float referenceMs) {
    if (perfGraphHasReference(referenceMs)) return 2.0f * referenceMs;
    // No published reference: fit the observations and draw no budget line.
    float peak = 1.0f;
    if (values) for (int i = 0; i < count; ++i)
        if (std::isfinite(values[i]) && values[i] > peak && values[i] <= 600000.0f) peak = values[i];
    return peak * 1.1f;
}
// -1: no reference; 0/1/2: within, above, or twice the reference.
inline int perfGraphBand(float ms, float referenceMs) {
    if (!perfGraphHasReference(referenceMs)) return -1;
    return ms > 2.0f * referenceMs ? 2 : ms > 1.02f * referenceMs ? 1 : 0;
}
}
