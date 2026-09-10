#pragma once

#include <cstring>
#include <cmath>
#include <string>

namespace edvr {

// Every temporal mode needs the same depth and rigid-object motion inputs.
// Keep their producers coupled to the mode, including live on/off changes.
inline bool temporalModeEnabled(const std::string& mode) {
    return _stricmp(mode.c_str(), "on") == 0 ||
           _stricmp(mode.c_str(), "dlaa") == 0 ||
           _stricmp(mode.c_str(), "dlss") == 0;
}

inline constexpr float kTemporalShipMetres = 10.0f;

// A display name only: automatic NVIDIA mode remains "dlss" in the ini.
inline const char* temporalNvidiaLabel(float hmdQuality) {
    if (!std::isfinite(hmdQuality) || hmdQuality <= 0.0f) return "DLSS / DLAA";
    return hmdQuality >= 1.0f ? "DLAA" : "DLSS";
}

}  // namespace edvr
