#pragma once

#include <cstdint>
#include <type_traits>

namespace edvr {

template <class T>
struct EyeCensusRead final {
    bool reached = false;
    bool known = false;
    T value{};
};

struct EyeCensusFilterObservation final {
    EyeCensusRead<std::uint8_t> modeOffGate;
    EyeCensusRead<std::uint8_t> modeAnyGate;
    EyeCensusRead<bool> boundNonNull;
    EyeCensusRead<std::uint8_t> modeAfterBound;
    EyeCensusRead<bool> resolved;
    EyeCensusRead<bool> isTexture2D;
    EyeCensusRead<std::uint32_t> width;
    EyeCensusRead<std::uint32_t> height;
    EyeCensusRead<std::uint8_t> modeAfterResolve;
    EyeCensusRead<std::uint32_t> configuredWidth;
    EyeCensusRead<std::uint32_t> configuredHeight;
    EyeCensusRead<bool> eyeSizeAvailable;
    EyeCensusRead<std::uint32_t> eyeWidth;
    EyeCensusRead<std::uint32_t> eyeHeight;
};

struct EyeCensusRuleObservation final {
    EyeCensusRead<std::uint32_t> loopCount;
    EyeCensusRead<std::uint64_t> vsHashGate;
    EyeCensusRead<std::uint64_t> heldVsHash;
    EyeCensusRead<std::uint64_t> vsHashCompareExpected;
    EyeCensusRead<std::uint8_t> ruleKind;
    EyeCensusRead<std::uint32_t> countHighGate;
    EyeCensusRead<std::uint32_t> countMinimum;
    EyeCensusRead<std::uint32_t> countHighBound;
    EyeCensusRead<std::uint32_t> exactCount;
    EyeCensusFilterObservation filters[4]{};
};

struct EyeCensusObservation final {
    std::uint16_t siteId = 48;
    std::uint8_t kind = 17;
    EyeCensusRead<std::uint32_t> skipCountGate;
    EyeCensusRuleObservation rules[8]{};
    EyeCensusRead<std::uint32_t> terminalLoopCount;
    EyeCensusRead<std::uint64_t> censusSkippedBefore;
    EyeCensusRead<std::uint64_t> censusSkippedAfter;
};

static_assert(std::is_standard_layout<EyeCensusObservation>::value &&
                  std::is_trivially_copyable<EyeCensusObservation>::value,
              "Eye census observations must remain pointer-free PODs");

} // namespace edvr
