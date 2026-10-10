#pragma once

#include "basic_draw_observation.h"
#include "holo_scrim_observation.h"

#include <cstdint>
#include <type_traits>

namespace edvr {

// Raw inputs consumed by the TargetSharp claim and its helper. Derived
// selector results are recomputed by the trace reader.
struct TargetSharpObservation final {
    std::uint16_t siteId = 54;
    std::uint8_t kind = 21;
    bool handlerInvoked = false;
    BasicDrawRead<bool> outerSharp;
    BasicDrawRead<bool> outerFailed;
    BasicDrawRead<bool> helperSharp;
    BasicDrawRead<bool> helperFailed;
    BasicDrawRead<bool> srv0Present;
    BasicDrawRead<bool> srv0Resolved;
    BasicDrawRead<bool> srv0Texture2D;
    BasicDrawRead<std::uint32_t> srv0Width;
    BasicDrawRead<std::uint32_t> srv0Height;
    holo_scrim_observation::EyeSizeObservation eyeSize{};
    BasicDrawRead<bool> aux1Present;
    BasicDrawRead<bool> aux2Present;
    BasicDrawRead<bool> aux3Present;
    BasicDrawRead<bool> vsPresent;
    BasicDrawRead<std::uint64_t> queriedShaderHash;
    BasicDrawRead<std::uint64_t> configuredShaderHash;
};

static_assert(std::is_standard_layout<TargetSharpObservation>::value &&
                  std::is_trivially_copyable<TargetSharpObservation>::value,
              "TargetSharp observation must remain a POD");

}  // namespace edvr
