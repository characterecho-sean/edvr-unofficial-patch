#pragma once

#include <cstdint>

namespace edvr {

// SDK package 3.0.2 contains XeSS-SR runtime 2.0.2. A normal 2.0 patch
// preserves the ABI; patch 90+ denotes a development interface.
inline bool xessSrRuntimeCompatible(uint16_t major, uint16_t minor, uint16_t patch) {
    return major == 2 && minor == 0 && patch < 90;
}

}  // namespace edvr
