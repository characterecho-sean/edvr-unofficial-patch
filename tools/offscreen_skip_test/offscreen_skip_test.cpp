// Pure parity rig for the production offscreen census and quad selectors.
// No D3D objects, device, files, or dynamic allocation are used.
#include "../../src/d3d11/offscreen_skip_selector.h"
#include "../../src/d3d11/vscreen.h"

#include <cstdio>
#include <cstring>

namespace {

struct TargetInfo final {
    bool isTexture2D;
    std::uint32_t a;
    std::uint32_t b;
};

std::uint32_t checks = 0;
std::uint32_t failures = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) ++failures;
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", label);
}

void testCensusRules() {
    using edvr::OffscreenPredicateRule;
    static_assert(edvr::kSceneEyeDraws == 100,
                  "frozen 14a quad cutoff is 100 eye draws");

    const TargetInfo panel{true, 2048, 1024};
    const OffscreenPredicateRule none[1] = {};
    check(edvr::matchingCensusRule(none, 0, 'I', 33, panel) == -1,
          "zero configured offscreen rules never match");

    const OffscreenPredicateRule one[] = {{'I', 33, 2048, 1024}};
    check(edvr::matchingCensusRule(one, 1, 'I', 33, panel) == 0,
          "one exact shape and target rule matches");
    check(edvr::matchingCensusRule(one, 1, 'I', 34, panel) == -1,
          "exact count mismatch declines");

    // Frozen config order: target-only wildcard, wrong shape, exact hit,
    // duplicate exact hit. The exact match must report index 2.
    const OffscreenPredicateRule four[] = {
        {0, 999, 1920, 1080},
        {'X', 6, 2048, 1024},
        {'I', 33, 2048, 1024},
        {'I', 33, 2048, 1024},
    };
    check(edvr::matchingCensusRule(four, 4, 'I', 33, panel) == 2,
          "four rules preserve first matching configured index");

    const OffscreenPredicateRule wildcard[] = {{0, 0, 2048, 1024}};
    check(edvr::matchingCensusRule(wildcard, 1, 'N', 0, panel) == 0,
          "wildcard kind ignores the stored count, including draw count zero");

    // The offscreen census parser accepts a spelled-out KIND:0; only a
    // missing count is invalid for a narrowed rule.
    const OffscreenPredicateRule zeroCount[] = {{'I', 0, 2048, 1024}};
    check(edvr::matchingCensusRule(zeroCount, 1, 'I', 0, panel) == 0,
          "kind-specific zero count matches the parsed zero-count rule");

    const TargetInfo wrongSize{true, 2048, 1025};
    const TargetInfo buffer{false, 2048, 1024};
    check(edvr::matchingCensusRule(one, 1, 'I', 33, wrongSize) == -1,
          "target dimensions must match exactly");
    check(edvr::matchingCensusRule(one, 1, 'I', 33, buffer) == -1,
          "non-texture target never matches");
}

void testQuadSelector() {
    using edvr::OffscreenPredicateRule;
    const OffscreenPredicateRule quad{'X', 6, 1024, 512};
    const TargetInfo exactTarget{true, 1024, 512};
    const TargetInfo wrongSize{true, 1024, 513};
    const TargetInfo buffer{false, 1024, 512};

    check(edvr::quadPrefixMatches(true, 99, edvr::kSceneEyeDraws, 'X', 6, quad),
          "armed matching draw at 99 reaches target probe below cutoff 100");
    check(!edvr::quadPrefixMatches(false, 99, edvr::kSceneEyeDraws, 'X', 6, quad),
          "unarmed selector stops before target probe");
    check(!edvr::quadPrefixMatches(true, 100, edvr::kSceneEyeDraws, 'X', 6, quad),
          "eye count equal to cutoff 100 stops before target probe");
    check(!edvr::quadPrefixMatches(true, 101, edvr::kSceneEyeDraws, 'X', 6, quad),
          "eye count 101 above cutoff 100 stops before target probe");
    check(!edvr::quadPrefixMatches(true, 99, edvr::kSceneEyeDraws, 'I', 6, quad),
          "draw kind mismatch stops before target probe");
    check(!edvr::quadPrefixMatches(true, 99, edvr::kSceneEyeDraws, 'X', 7, quad),
          "draw count mismatch stops before target probe");

    check(edvr::quadTargetMatches(exactTarget, quad),
          "matching 2D target completes the selector");
    check(!edvr::quadTargetMatches(wrongSize, quad),
          "quad target dimensions must match exactly");
    check(!edvr::quadTargetMatches(buffer, quad),
          "quad selector rejects a non-texture target");

    // Supplemental call-order check: production keeps the existing RTV probe
    // after the scalar prefix; the prefix itself performs no resource access.
    std::uint32_t probes = 0;
    const auto tryWithProbe = [&](bool armed, std::uint32_t eyeDraws,
                                  char kind, std::uint32_t count) {
        if (!edvr::quadPrefixMatches(armed, eyeDraws, edvr::kSceneEyeDraws,
                                     kind, count, quad))
            return false;
        ++probes;
        return edvr::quadTargetMatches(exactTarget, quad);
    };
    check(!tryWithProbe(true, 100, 'X', 6) && probes == 0,
          "scene cutoff prevents an RTV probe");
    check(tryWithProbe(true, 99, 'X', 6) && probes == 1,
          "eligible prefix performs exactly one existing RTV probe");
}

int selfTest() {
    testCensusRules();
    testQuadSelector();
    std::printf("offscreen_skip_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("offscreen_skip_test: dry-run (no device, files, or writes)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: offscreen_skip_test --dry-run | --self-test");
        return 2;
    }
    return selfTest();
}
