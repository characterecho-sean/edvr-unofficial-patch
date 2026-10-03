#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr::draw_interest {

// Stable configured-interest identities shared by the registry, ordered draw
// ladder, and deterministic tests. Add values at the end; traces record the
// ladder SiteId, while this mask remains an internal cached eligibility set.
enum class InterestId : std::uint8_t {
    TargetSharp = 0,
    WitchspaceStars = 1,
    FssPanel = 2,
    FssReveal = 3,
    FssDump = 4,
    ParticleSubstitute = 5,
    IntroCurveObserve = 6,
    PanelCurveObserve = 7,
    Count = 8,
};

using InterestMask = std::uint64_t;
constexpr std::size_t kInterestCount = static_cast<std::size_t>(InterestId::Count);
static_assert(kInterestCount <= 64, "interest IDs must fit the published mask");

constexpr InterestMask bit(InterestId id) noexcept {
    const auto index = static_cast<std::uint8_t>(id);
    return index < kInterestCount ? (InterestMask{1} << index) : InterestMask{0};
}

constexpr bool contains(InterestMask mask, InterestId id) noexcept {
    return (mask & bit(id)) != 0;
}

enum class HashFilter : std::uint8_t {
    Vertex = 1,
    Pixel = 2,
    Pair = 3,
};

struct ShaderFilter final {
    InterestId id = InterestId::TargetSharp;
    HashFilter kind = HashFilter::Vertex;
    std::uint64_t vsHash = 0;
    std::uint64_t psHash = 0;
};

constexpr bool filterMayMatch(const ShaderFilter& filter, std::uint64_t vs,
                              std::uint64_t ps) noexcept {
    switch (filter.kind) {
        case HashFilter::Vertex:
            return vs == 0 || filter.vsHash == 0 || vs == filter.vsHash;
        case HashFilter::Pixel:
            return ps == 0 || filter.psHash == 0 || ps == filter.psHash;
        case HashFilter::Pair:
            return vs == 0 || ps == 0 || filter.vsHash == 0 || filter.psHash == 0 ||
                   (vs == filter.vsHash && ps == filter.psHash);
    }
    // Unknown filter metadata must preserve the configured path.
    return true;
}

// Intersect configured interests with known shader filters. A configured
// interest with no filter remains eligible. Unknown observed hashes and
// wildcard expected components conservatively retain eligibility; a bit is
// removed only when every filter for it is a known mismatch.
constexpr InterestMask buildCandidateMask(InterestMask configuredMask,
                                         std::uint64_t vs, std::uint64_t ps,
                                         const ShaderFilter* filters,
                                         std::size_t filterCount) noexcept {
    InterestMask result = 0;
    for (std::size_t i = 0; i < kInterestCount; ++i) {
        const auto id = static_cast<InterestId>(i);
        const InterestMask interestBit = InterestMask{1} << i;
        if ((configuredMask & interestBit) == 0) continue;

        bool hasFilter = false;
        bool mayMatch = false;
        if (filters) {
            for (std::size_t j = 0; j < filterCount; ++j) {
                if (filters[j].id != id) continue;
                hasFilter = true;
                if (filterMayMatch(filters[j], vs, ps)) {
                    mayMatch = true;
                    break;
                }
            }
        }
        if (!hasFilter || mayMatch) result |= interestBit;
    }
    return result;
}

}  // namespace edvr::draw_interest
