#pragma once

#include <cstdint>
#include <type_traits>

namespace edvr {

// Pointer-free source facts for the site-59 FSS dump claim. Draw kind/count/
// instances remain in DrawFacts; readers derive the family and claim.
template <class T>
struct FssDumpRead final {
    bool reached = false;
    bool known = false;
    T value{};
};

// Preserve each consumed operand of the short-circuit wants expression.
struct FssDumpWantsObservation final {
    FssDumpRead<std::uint32_t> frame;
    FssDumpRead<bool> done;
    FssDumpRead<std::uint32_t> seriesWant;
    FssDumpRead<bool> seriesDone;
};

struct FssDumpOuterObservation final {
    FssDumpWantsObservation wants;
    FssDumpRead<std::uint32_t> bodyFrame;
    FssDumpRead<std::uint32_t> frameNo;
};

struct FssDumpCountersObservation final {
    FssDumpRead<std::uint8_t> ringBefore;
    FssDumpRead<std::uint8_t> ringAfter;
    FssDumpRead<std::uint8_t> compositeBefore;
    FssDumpRead<std::uint8_t> compositeAfter;
    FssDumpRead<std::uint8_t> tonemapBefore;
    FssDumpRead<std::uint8_t> tonemapAfter;
};

struct FssDumpHelperObservation final {
    FssDumpWantsObservation wants;
    FssDumpRead<bool> contextNonNull;
    FssDumpRead<bool> guardCallReached;
    FssDumpRead<bool> callbackEntered;
    FssDumpRead<bool> vsGetShaderReached;
    FssDumpRead<bool> vsGetShaderCompleted;
    FssDumpRead<bool> shaderNonNull;
    FssDumpRead<bool> lookupReached;
    FssDumpRead<bool> lookupCompleted;
    FssDumpRead<std::uint64_t> lookupHash;
    FssDumpRead<bool> releaseReached;
    FssDumpRead<bool> releaseCompleted;
    FssDumpRead<bool> callbackCompleted;
    FssDumpRead<bool> guardReturned;
    // The selector consumes local h after the guard. A Release fault does not
    // erase a hash already assigned by lookupShaderHash.
    FssDumpRead<std::uint64_t> hashAfterGuard;
    FssDumpRead<bool> dumping;
    FssDumpCountersObservation counters;
    FssDumpRead<std::uint32_t> pendingKindBefore;
    FssDumpRead<std::uint32_t> pendingKindAfter;
    FssDumpRead<std::uint32_t> pendingEyeBefore;
    FssDumpRead<std::uint32_t> pendingEyeAfter;
};

struct FssDumpObservation final {
    std::uint16_t siteId = 59;
    std::uint8_t kind = 20;
    // False for InterestNotEligible. The raw gate groups then remain unused;
    // the reader derives interest admission from site 6's recorded mask.
    bool handlerInvoked = false;
    FssDumpOuterObservation outer;
    FssDumpHelperObservation helper;
};

static_assert(std::is_standard_layout<FssDumpObservation>::value &&
                  std::is_trivially_copyable<FssDumpObservation>::value,
              "FSS dump observations must remain pointer-free PODs");

}  // namespace edvr
