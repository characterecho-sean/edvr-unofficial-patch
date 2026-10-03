#pragma once

#include "basic_draw_observation.h"

#include <cstdint>
#include <type_traits>

namespace edvr {

struct SunglareNominationObservation final {
    std::uint16_t siteId = 45;
    std::uint8_t kind = 22;
    BasicDrawRead<std::int32_t> worldMode;
    BasicDrawRead<std::uintptr_t> boundCbIdentity;
    BasicDrawRead<std::uintptr_t> nominatedBeforeIdentity;
    BasicDrawRead<bool> resourceResolved;
    BasicDrawRead<bool> isBuffer;
    BasicDrawRead<std::uint32_t> byteWidth;
    BasicDrawRead<bool> callbackInvoked;
    BasicDrawRead<std::uintptr_t> nominatedAfterIdentity;
    BasicDrawRead<std::uintptr_t> callbackTargetAfterIdentity;
};

static_assert(std::is_standard_layout<SunglareNominationObservation>::value &&
                  std::is_trivially_copyable<SunglareNominationObservation>::value,
              "Sunglare nomination observation must remain a scalar POD");

}  // namespace edvr
