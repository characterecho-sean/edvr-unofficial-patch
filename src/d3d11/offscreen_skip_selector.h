// Pure selector predicates shared by armed offscreen capture and its host rig.
// Ordinary NoTrace selection retains its previously validated source bodies.
// Callers own resource probing, config parsing, counters, and draw forwarding.
#pragma once

#include <cstdint>

namespace edvr {

// Layout used by the offscreen census rules and the configured quad skip.
struct OffscreenPredicateRule final {
    std::uint8_t kind = 0;  // zero means any draw kind for census rules
    std::uint32_t n = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

constexpr std::uint32_t kMaxOffscreenPredicateRules = 4;

// Returns the first matching rule, preserving the visitor's ordered search.
// The parser bounds ruleCount to four and a positive count supplies a non-null
// array. The caller has already made the existing target probe; this helper
// performs no resource or D3D work.
template <class Rule, class Info>
inline int matchingCensusRule(const Rule* rules, std::uint32_t ruleCount,
                              char kind, std::uint32_t count,
                              const Info& target) noexcept {
    if (!target.isTexture2D) return -1;
    for (std::uint32_t i = 0; i < ruleCount; ++i) {
        const Rule& rule = rules[i];
        if (target.a != rule.w || target.b != rule.h) continue;
        // A wildcard rule names the whole target and ignores draw shape.
        if (rule.kind != 0 &&
            (rule.kind != static_cast<std::uint8_t>(kind) || rule.n != count))
            continue;
        return static_cast<int>(i);
    }
    return -1;
}

// The exact scalar prefix of the offscreen quad-skip selector. The caller
// probes the RTV only after this returns true, matching the production order.
template <class Rule>
inline bool quadPrefixMatches(bool armed, std::uint32_t eyeDrawsLastFrame,
                              std::uint32_t sceneEyeDrawCutoff, char kind,
                              std::uint32_t count,
                              const Rule& rule) noexcept {
    return armed && eyeDrawsLastFrame < sceneEyeDrawCutoff &&
        kind == static_cast<char>(rule.kind) && count == rule.n;
}

// Final target test after the scalar prefix and the caller's existing probe.
template <class Info, class Rule>
inline bool quadTargetMatches(const Info& target,
                              const Rule& rule) noexcept {
    return target.isTexture2D && target.a == rule.w && target.b == rule.h;
}

}  // namespace edvr
