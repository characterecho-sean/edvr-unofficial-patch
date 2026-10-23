#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace edvr {
// Log quotas only. This partition never feeds the camera classifier or pixels.
inline unsigned flatOriginOpticalCategory(float step,double nowLength,double previousLength,bool reset) noexcept {
    const bool toward=nowLength<previousLength;
    if(reset)return 4;
    if(toward && step>512)return 1; // long periodic reanchors cannot spend short-inward quota
    if(toward && nowLength<(std::max)(250.0,2.0*double(step)))return 0;
    return toward?2:3;
}
inline const char* flatOriginOpticalSignal(const uint32_t* counts) noexcept {
    if(!counts || counts[7]<64 || counts[13]+counts[14]<16)return "inconclusive";
    if(counts[13]>=16 && counts[13]>=3*counts[14])return "raw-travel-depth-favored";
    if(counts[14]>=16 && counts[14]>=3*counts[13])return "zero-translation-depth-favored";
    return "inconclusive";
}
inline bool flatOriginOpticalPriorDomainSupported(bool foreground,bool untrusted,bool firstPerson=false) noexcept {
    // No previous-frame owner mask accompanies the canonical depth buffer.
    return !foreground && !untrusted && !firstPerson;
}
}
