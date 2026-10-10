#pragma once

#include <cstdint>
#include <type_traits>

namespace edvr {

template <class T>
struct ResolveBindRead final {
    bool reached = false;
    bool known = false;
    T value{};
};

struct ResolveBindOuterObservation final {
    ResolveBindRead<bool> wants;
    ResolveBindRead<bool> psPresent;
    ResolveBindRead<std::uint64_t> psHash;
};

struct ResolveBindHelperObservation final {
    ResolveBindRead<bool> wants;
    ResolveBindRead<bool> contextNonNull;
    ResolveBindRead<bool> psPresent;
    ResolveBindRead<std::uint64_t> psHash;
    ResolveBindRead<bool> lambdaEntered;
    ResolveBindRead<bool> psGetReached;
    ResolveBindRead<bool> psGetCompleted;
    ResolveBindRead<bool> shaderNonNull;
    ResolveBindRead<bool> lookupReached;
    ResolveBindRead<bool> lookupCompleted;
    ResolveBindRead<std::uint64_t> lookupHash;
    ResolveBindRead<bool> cacheBeforePresent;
    ResolveBindRead<std::uint64_t> cacheBeforeHash;
    ResolveBindRead<bool> cacheAfterPresent;
    ResolveBindRead<std::uint64_t> cacheAfterHash;
    ResolveBindRead<bool> releaseReached;
    ResolveBindRead<bool> releaseCompleted;
    ResolveBindRead<bool> guardReturned;
};

struct ResolveBindObservation final {
    std::uint16_t siteId = 60;
    std::uint8_t kind = 18;
    ResolveBindOuterObservation outer;
    ResolveBindHelperObservation helper;
};

static_assert(std::is_standard_layout<ResolveBindObservation>::value &&
                  std::is_trivially_copyable<ResolveBindObservation>::value,
              "ResolveBind observations must remain pointer-free PODs");

} // namespace edvr
