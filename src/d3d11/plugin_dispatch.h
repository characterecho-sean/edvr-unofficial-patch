#pragma once

#include <cstddef>
#include <cstdint>
#include "plugin_manifest.inc"

namespace edvr { namespace plugins { namespace dispatch {

struct ShaderClaimKey {
    uint32_t pluginIndex;
    const char* claimId;
    uint64_t vsHash;
    uint64_t psHash;
};

template <typename Shape>
inline bool matchesShape(const Shape& shape, uint8_t kind,
                         uint32_t count, uint32_t instances) {
    return shape.kind == kind && shape.count == count && shape.instances == instances;
}

// Called only when a shader is bound or plugin configuration changes. The
// resulting mask is the exact set of plugin candidates for this shader pair;
// draw shape and live state remain per-draw claim predicates.
inline uint64_t candidatePlugins(uint64_t vsHash, uint64_t psHash,
                                 uint64_t activePlugins,
                                 const ShaderClaimKey* claims, size_t claimCount,
    size_t* shaderPairChecks = nullptr) {
    uint64_t candidates = 0;
    if (!claims || !activePlugins) return 0;
    for (size_t i = 0; i < claimCount; ++i) {
        if (shaderPairChecks) ++*shaderPairChecks;
        const ShaderClaimKey& claim = claims[i];
        if (claim.pluginIndex < 64 && (activePlugins & (uint64_t{1} << claim.pluginIndex)) &&
            claim.vsHash == vsHash && claim.psHash == psHash)
            candidates |= uint64_t{1} << claim.pluginIndex;
    }
    return candidates;
}

inline bool hasCandidate(uint64_t candidates, uint32_t pluginIndex) {
    return pluginIndex < 64 && (candidates & (uint64_t{1} << pluginIndex)) != 0;
}

template <typename ShapePredicate, typename ClaimCallback>
inline uint32_t resolveCandidate(uint64_t candidates, uint32_t pluginIndex,
                                 ShapePredicate&& shapeMatches, ClaimCallback&& claim) {
    if (!hasCandidate(candidates, pluginIndex) || !shapeMatches()) return 0;
    return claim();
}

}}}  // namespace edvr::plugins::dispatch
