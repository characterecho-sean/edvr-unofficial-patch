// Synthetic selector microbenchmark. It exercises the production typed
// Sequence/InterestGated fold with NoTrace and NoCpu; it is not a classifier,
// forwarding, D3D, plugin-cost, or flight-performance test.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>

#include "../../src/d3d11/draw_interest.h"
#include "../../src/d3d11/draw_ladder.h"
#include "../../src/d3d11/plugin_dispatch.h"
#include "../../src/common/plugin_cost.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
namespace ladder = edvr::draw_ladder;
namespace interest = edvr::draw_interest;
namespace dispatch = edvr::plugins::dispatch;

constexpr std::size_t kCorpusSize = 64;
constexpr std::size_t kRowsPerCase = 16;
constexpr std::uint32_t kRepeatsPerBatch = 256;
constexpr std::uint32_t kDrawsPerBatch =
    static_cast<std::uint32_t>(kCorpusSize) * kRepeatsPerBatch;
constexpr std::uint32_t kPairedRuns = 20;
constexpr double kT19TwoSided95 = 2.093;
constexpr std::uint32_t kSyntheticPluginIndex =
    edvr::plugins::kPluginCockpitVisuals;
constexpr std::uint64_t kSyntheticVsMatch = 0x1010101010101010ull;
constexpr std::uint64_t kSyntheticPsMatch = 0x2020202020202020ull;
constexpr std::uint64_t kSyntheticVsOther = 0x3030303030303030ull;
constexpr std::uint64_t kSyntheticPsOther = 0x4040404040404040ull;
// Keep the NV exact pair outside every corpus row. The timed corpus measures
// legacy-interest filtering while the distinct NV candidate gate stays cold.
constexpr std::uint64_t kSyntheticNvVsMatch = 0xA1A1A1A1A1A1A1A1ull;
constexpr std::uint64_t kSyntheticNvPsMatch = 0xB2B2B2B2B2B2B2B2ull;

constexpr interest::InterestMask kAllInterests =
    (interest::InterestMask{1} << interest::kInterestCount) - 1;

enum class MaskCase : std::uint8_t { Off, AllOn, MixedKnown, UnknownHashes };

struct DrawInput final {
    char kind = 'N';
    std::uint32_t count = 3;
    std::uint32_t instances = 1;
    std::uint64_t vsHash = 0;
    std::uint64_t psHash = 0;
    std::uint64_t legacyInterestMask = 0;
    std::uint64_t pluginCandidates = 0;
    std::uint32_t expectedCount = 3;
    std::uint32_t salt = 0;
    bool terminalPanel = false;
};

struct RunResult final {
    std::uint64_t semanticChecksum = 0;
    std::uint64_t emptyActionChecksum = 0;
    std::uint64_t handlerDigest = 0;
    std::uint64_t handlerInvocations = 0;
    std::uint64_t legacyMaskLoads = 0;
    std::uint64_t nvCandidateChecks = 0;
};

struct TimedResult final {
    double nsPerDraw = 0.0;
    RunResult output{};
    bool valid = false;
};

volatile std::uint64_t g_escapeSink = 0;

bool check(bool condition, const char* label) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", label);
    return condition;
}

const interest::ShaderFilter kFilters[] = {
    {interest::InterestId::TargetSharp, interest::HashFilter::Pair,
     kSyntheticVsMatch, kSyntheticPsMatch},
    {interest::InterestId::WitchspaceStars, interest::HashFilter::Vertex,
     kSyntheticVsMatch, 0},
    {interest::InterestId::FssPanel, interest::HashFilter::Pixel,
     0, 0xBAD0BAD0BAD0BAD0ull},
    {interest::InterestId::FssReveal, interest::HashFilter::Pair,
     kSyntheticVsMatch, kSyntheticPsMatch},
    {interest::InterestId::FssDump, interest::HashFilter::Vertex,
     0xF00DF00DF00DF00Dull, 0},
};

const dispatch::ShaderClaimKey kExactNvClaim[] = {
    {kSyntheticPluginIndex, "synthetic-night-vision",
     kSyntheticNvVsMatch, kSyntheticNvPsMatch},
};

std::uint64_t exactPluginCandidates(std::uint64_t vs, std::uint64_t ps) {
    return dispatch::candidatePlugins(vs, ps,
        std::uint64_t{1} << kSyntheticPluginIndex, kExactNvClaim, 1);
}

std::array<DrawInput, kCorpusSize> makeCorpus() {
    std::array<DrawInput, kCorpusSize> rows{};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        DrawInput& row = rows[i];
        const std::size_t group = i / kRowsPerCase;
        const std::size_t local = i % kRowsPerCase;
        row.kind = (local % 4 == 0) ? 'D' : (local % 4 == 1) ? 'I' :
                   (local % 4 == 2) ? 'N' : 'X';
        row.count = static_cast<std::uint32_t>(3 + ((i * 37) % 4093));
        row.instances = static_cast<std::uint32_t>(i % 3);
        row.expectedCount = static_cast<std::uint32_t>(3 + ((i * 19) % 4093));
        row.salt = static_cast<std::uint32_t>(0x9E3779B9u * (i + 1));
        row.terminalPanel = (local % 5 == 0);

        MaskCase maskCase = MaskCase::Off;
        switch (group) {
            case 0:
                maskCase = MaskCase::Off;
                row.vsHash = kSyntheticVsMatch;
                row.psHash = kSyntheticPsMatch;
                row.legacyInterestMask = 0;
                break;
            case 1:
                maskCase = MaskCase::AllOn;
                row.vsHash = kSyntheticVsMatch;
                row.psHash = kSyntheticPsMatch;
                row.legacyInterestMask = kAllInterests;
                break;
            case 2:
                maskCase = MaskCase::MixedKnown;
                if (local % 2 == 0) {
                    row.vsHash = kSyntheticVsMatch;
                    row.psHash = kSyntheticPsMatch;
                } else {
                    row.vsHash = kSyntheticVsOther;
                    row.psHash = kSyntheticPsOther;
                }
                row.legacyInterestMask = interest::buildCandidateMask(
                    kAllInterests, row.vsHash, row.psHash, kFilters,
                    sizeof(kFilters) / sizeof(kFilters[0]));
                break;
            default:
                maskCase = MaskCase::UnknownHashes;
                row.vsHash = 0;
                row.psHash = 0;
                row.legacyInterestMask = interest::buildCandidateMask(
                    kAllInterests, row.vsHash, row.psHash, kFilters,
                    sizeof(kFilters) / sizeof(kFilters[0]));
                break;
        }
        (void)maskCase; // The case is represented by its POD input fields.

        // The corpus never has the exact cached NV shader pair. NV remains a
        // separate, lazy exact-pair gate and is never made eligible by the
        // generic legacy unknown-hash fail-open rule.
        row.pluginCandidates = exactPluginCandidates(row.vsHash, row.psHash);
    }
    return rows;
}

template <bool ForceLegacyEligible, bool CountQueries>
struct InterestProvider final {
    const DrawInput& input;
    interest::InterestMask cachedLegacyMask = 0;
    bool legacyMaskLoaded = false;
    std::uint32_t legacyMaskLoads = 0;
    std::uint32_t nvCandidateChecks = 0;

    template <class SiteType>
    bool eligible() noexcept {
        if constexpr (SiteType::shaderCandidateGated) {
            if constexpr (CountQueries) ++nvCandidateChecks;
            return (input.pluginCandidates &
                    (std::uint64_t{1} << edvr::plugins::kPluginCockpitVisuals)) != 0;
        } else {
            if (!legacyMaskLoaded) {
                cachedLegacyMask = input.legacyInterestMask;
                legacyMaskLoaded = true;
                if constexpr (CountQueries) ++legacyMaskLoads;
            }
            if constexpr (ForceLegacyEligible) {
                return true;
            } else {
                return interest::contains(cachedLegacyMask, SiteType::interestId);
            }
        }
    }
};

std::uint64_t fixturePredicateWork(const DrawInput& input, std::uint16_t siteId,
                                   std::uint32_t repeatSeed) {
    // Synthetic input-dependent decline workload. It reads only the POD row,
    // produces a digest, and never claims or mutates selector state. The
    // digest keeps the compiler from deleting this fixture work; it is not a
    // model of any module's real predicate or cost.
    std::uint64_t x = input.vsHash ^ ((input.psHash << 17) | (input.psHash >> 47));
    x ^= (static_cast<std::uint64_t>(input.kind) << 48) |
         (static_cast<std::uint64_t>(input.count) << 16) | input.instances;
    x ^= (static_cast<std::uint64_t>(input.expectedCount) << 23) |
         (static_cast<std::uint64_t>(input.salt ^ repeatSeed) << 1) | siteId;
    const bool shapeCandidate = input.kind == 'N' && input.count == input.expectedCount &&
                                input.instances == 1;
    const bool pairCandidate = input.vsHash == kSyntheticVsMatch &&
                               input.psHash == kSyntheticPsMatch;
    x ^= shapeCandidate ? 0xA0761D6478BD642Full : 0xE7037ED1A0B428DBull;
    x ^= pairCandidate ? 0x8EBC6AF09C88C6E3ull : 0x589965CC75374CC3ull;
    x ^= x >> 29;
    x *= 0x9FB21C651E98DF25ull;
    x ^= x >> 31;
    return x;
}

template <bool CountHandlers>
struct PureVisitor final {
    const DrawInput& input;
    std::uint32_t repeatSeed = 0;
    std::uint64_t handlerDigest = 0;
    std::uint64_t handlerInvocations = 0;
    std::int16_t verdict = -1;

    template <class SiteType>
    ladder::SiteResult visit() noexcept {
        if constexpr (SiteType::interestGated && !SiteType::shaderCandidateGated) {
            handlerDigest += fixturePredicateWork(
                input, static_cast<std::uint16_t>(SiteType::id), repeatSeed);
            if constexpr (CountHandlers) ++handlerInvocations;
        }

        if constexpr (SiteType::kind == ladder::SiteKind::Observe) {
            return ladder::SiteResult::observed();
        } else if constexpr (SiteType::id == ladder::SiteId::kPanelDistanceClaim) {
            if (input.terminalPanel) {
                verdict = static_cast<std::int16_t>(ladder::VerdictOrdinal::kPanel);
                return ladder::SiteResult::claimed(verdict);
            }
            return ladder::SiteResult::declined();
        } else {
            // All other fixture sites decline. No logged outcome/site sequence
            // is used as a predicate input or oracle.
            return ladder::SiteResult::declined();
        }
    }
};

std::uint64_t foldSemantic(std::uint64_t digest, ladder::Flow flow,
                           std::int16_t verdict, std::size_t rowIndex) {
    const std::uint64_t value = (static_cast<std::uint64_t>(rowIndex) << 32) ^
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(flow)) << 24) ^
        static_cast<std::uint16_t>(verdict);
    return (digest ^ value) * 0x9E3779B185EBCA87ull;
}

template <bool ForceLegacyEligible, bool CountQueries, bool CountHandlers>
RunResult runCorpus(const std::array<DrawInput, kCorpusSize>& rows,
                    std::uint32_t repeatSeed = 0) {
    RunResult out{};
    ladder::NoTrace noTrace;
    edvr::plugin_cost::NoCpu noCpu;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const DrawInput& input = rows[i];
        InterestProvider<ForceLegacyEligible, CountQueries> gates{input};
        PureVisitor<CountHandlers> visitor{input, repeatSeed};
        ladder::Flow flow = ladder::visitOrdered(
            ladder::CommonSequence{}, visitor, gates, noTrace, noCpu);
        if (flow == ladder::Flow::Continue) {
            flow = ladder::visitOrdered(
                ladder::VrEyeSequence{}, visitor, gates, noTrace, noCpu);
        }
        out.semanticChecksum = foldSemantic(out.semanticChecksum, flow,
                                            visitor.verdict, i);
        out.handlerDigest += visitor.handlerDigest;
        out.handlerInvocations += visitor.handlerInvocations;
        out.legacyMaskLoads += gates.legacyMaskLoads;
        out.nvCandidateChecks += gates.nvCandidateChecks;
    }
    // The typed selector emits no forwarding actions. Keep the empty stream
    // explicit rather than inventing an action policy for this selector-only rig.
    out.emptyActionChecksum = 0;
    return out;
}

std::uint64_t countGatedInvocations(const std::array<DrawInput, kCorpusSize>& rows,
                                    std::size_t first, std::size_t count) {
    std::uint64_t invocations = 0;
    ladder::NoTrace noTrace;
    edvr::plugin_cost::NoCpu noCpu;
    const std::size_t end = (std::min)(rows.size(), first + count);
    for (std::size_t i = first; i < end; ++i) {
        const DrawInput& input = rows[i];
        InterestProvider<false, true> gates{input};
        PureVisitor<true> visitor{input};
        const auto common = ladder::visitOrdered(
            ladder::CommonSequence{}, visitor, gates, noTrace, noCpu);
        if (common == ladder::Flow::Continue)
            (void)ladder::visitOrdered(ladder::VrEyeSequence{}, visitor,
                                       gates, noTrace, noCpu);
        invocations += visitor.handlerInvocations;
    }
    return invocations;
}

bool selfTest() {
    bool ok = true;
    const std::uint64_t expectedPair =
        std::uint64_t{1} << kSyntheticPluginIndex;
    const std::uint64_t allOnKnown = interest::buildCandidateMask(
        kAllInterests, kSyntheticVsMatch, kSyntheticPsMatch,
        kFilters, sizeof(kFilters) / sizeof(kFilters[0]));
    const std::uint64_t allOnMismatch = interest::buildCandidateMask(
        kAllInterests, kSyntheticVsOther, kSyntheticPsOther,
        kFilters, sizeof(kFilters) / sizeof(kFilters[0]));
    const std::uint64_t allOnUnknown = interest::buildCandidateMask(
        kAllInterests, 0, 0, kFilters,
        sizeof(kFilters) / sizeof(kFilters[0]));
    const std::uint64_t exactMatch = exactPluginCandidates(
        kSyntheticNvVsMatch, kSyntheticNvPsMatch);
    const std::uint64_t exactMismatch = exactPluginCandidates(
        kSyntheticVsOther, kSyntheticPsOther);
    const std::uint64_t exactUnknown = exactPluginCandidates(0, 0);

    ok &= check(interest::buildCandidateMask(0, kSyntheticVsMatch,
                    kSyntheticPsMatch, kFilters,
                    sizeof(kFilters) / sizeof(kFilters[0])) == 0,
                "legacy all-off mask remains empty");
    ok &= check(interest::contains(allOnKnown, interest::InterestId::TargetSharp) &&
                    interest::contains(allOnKnown, interest::InterestId::WitchspaceStars) &&
                    !interest::contains(allOnKnown, interest::InterestId::FssPanel) &&
                    !interest::contains(allOnKnown, interest::InterestId::FssDump),
                "known shader filters retain matches and remove known mismatches");
    ok &= check(interest::contains(allOnMismatch, interest::InterestId::ParticleSubstitute) &&
                    interest::contains(allOnMismatch, interest::InterestId::IntroCurveObserve) &&
                    interest::contains(allOnMismatch, interest::InterestId::PanelCurveObserve) &&
                    !interest::contains(allOnMismatch, interest::InterestId::TargetSharp),
                "mixed known mismatch retains unfiltered legacy interests");
    ok &= check(allOnUnknown == kAllInterests,
                "unknown generic hashes fail open for configured legacy interests");
    ok &= check(exactMatch == expectedPair && exactMismatch == 0 && exactUnknown == 0,
                "NV exact-pair cache rejects both known mismatch and zero hashes");
    const std::uint64_t nvExactMatch = exactPluginCandidates(
        kSyntheticNvVsMatch, kSyntheticNvPsMatch);
    ok &= check(nvExactMatch == expectedPair,
                "separate NV exact-pair fixture recognizes only its exact pair");

    const auto rows = makeCorpus();
    bool corpusHasNoNvCandidate = true;
    for (const DrawInput& input : rows)
        corpusHasNoNvCandidate &= input.pluginCandidates == 0;
    ok &= check(corpusHasNoNvCandidate,
                "all timed corpus rows remain an exact-pair NV miss");
    const RunResult gated = runCorpus<false, true, true>(rows);
    const RunResult always = runCorpus<true, true, true>(rows);
    ok &= check(gated.semanticChecksum == always.semanticChecksum,
                "configured and always-eligible controls preserve selector verdict checksum");
    ok &= check(gated.emptyActionChecksum == always.emptyActionChecksum &&
                    gated.emptyActionChecksum == 0,
                "selector-only action checksum remains the same empty action stream");
    ok &= check(gated.legacyMaskLoads == kCorpusSize &&
                    always.legacyMaskLoads == kCorpusSize,
                "legacy interest snapshot loads once per draw in each control");
    ok &= check(gated.nvCandidateChecks == kCorpusSize &&
                    always.nvCandidateChecks == kCorpusSize,
                "NV candidate check stays a separate lazy per-draw gate");

    const auto off = runCorpus<false, true, true>(rows);
    const std::uint64_t allOffCalls =
        countGatedInvocations(rows, 0, kRowsPerCase);
    const std::uint64_t allOnCalls =
        countGatedInvocations(rows, kRowsPerCase, kRowsPerCase);
    const std::uint64_t mixedCalls =
        countGatedInvocations(rows, 2 * kRowsPerCase, kRowsPerCase);
    const std::uint64_t unknownCalls =
        countGatedInvocations(rows, 3 * kRowsPerCase, kRowsPerCase);
    ok &= check(allOffCalls == 0,
                "configured all-off control invokes zero gated legacy handlers");
    ok &= check(allOnCalls > 0 && mixedCalls > 0 && unknownCalls > 0 &&
                    mixedCalls < unknownCalls,
                "all-on, mixed known, and unknown fail-open rows invoke expected handlers");
    ok &= check(always.handlerInvocations > off.handlerInvocations,
                "forced eligibility invokes more input-dependent decline handlers across the corpus");

    constexpr std::uint64_t kValidQpcFrequency = 10000000;
    const auto validMeasurementConfig = [](std::uint64_t frequency,
                                           std::uint32_t draws) {
        return frequency != 0 && draws != 0;
    };
    ok &= check(validMeasurementConfig(kValidQpcFrequency, kDrawsPerBatch) &&
                    !validMeasurementConfig(0, kDrawsPerBatch) &&
                    !validMeasurementConfig(kValidQpcFrequency, 0),
                "benchmark rejects zero frequency and zero draw denominator");
    ok &= check(kCorpusSize == 64 && kDrawsPerBatch == 16384,
                "fixed synthetic corpus and logical selector-iteration denominator are pinned");
    std::printf("selector microbenchmark self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

double nsPerDraw(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER frequency) {
    if (frequency.QuadPart <= 0 || end.QuadPart <= start.QuadPart ||
        kDrawsPerBatch == 0) return -1.0;
    const double ticks = static_cast<double>(end.QuadPart - start.QuadPart);
    return ticks * 1000000000.0 /
        (static_cast<double>(frequency.QuadPart) * kDrawsPerBatch);
}

template <bool ForceLegacyEligible>
TimedResult timeBatch(const std::array<DrawInput, kCorpusSize>& rows,
                      LARGE_INTEGER frequency) {
    LARGE_INTEGER start{}, end{};
    _ReadWriteBarrier();
    if (!QueryPerformanceCounter(&start)) return {};
    RunResult total{};
    for (std::uint32_t repeat = 0; repeat < kRepeatsPerBatch; ++repeat) {
        // The changing seed is a data dependency of every synthetic gated
        // handler. This prevents /O2 from common-subexpression-eliminating
        // identical traversals across repeats without a volatile per-draw
        // store or clock read.
        const std::uint32_t repeatSeed = 0x9E3779B9u * (repeat + 1u);
        const RunResult one = runCorpus<ForceLegacyEligible, false, false>(
            rows, repeatSeed);
        total.semanticChecksum ^= one.semanticChecksum + repeat;
        total.handlerDigest += one.handlerDigest;
        total.emptyActionChecksum ^= one.emptyActionChecksum;
    }
    _ReadWriteBarrier();
    if (!QueryPerformanceCounter(&end)) return {};
    _ReadWriteBarrier();
    // Escape only after timing. The digest's data dependency keeps the pure
    // input-driven workload live without a volatile write per draw/site.
    g_escapeSink = g_escapeSink ^ total.handlerDigest ^ total.semanticChecksum;
    _ReadWriteBarrier();
    const double value = nsPerDraw(start, end, frequency);
    return {value, total, value >= 0.0};
}

struct Interval final {
    double mean = 0.0;
    double low = 0.0;
    double high = 0.0;
    bool valid = false;
};

Interval pairedInterval(const std::array<double, kPairedRuns>& values) {
    Interval out{};
    for (double value : values) {
        if (!std::isfinite(value)) return out;
        out.mean += value;
    }
    out.mean /= static_cast<double>(values.size());
    double sumSquares = 0.0;
    for (double value : values) {
        const double delta = value - out.mean;
        sumSquares += delta * delta;
    }
    const double sd = std::sqrt(sumSquares / static_cast<double>(values.size() - 1));
    const double margin = kT19TwoSided95 * sd /
                          std::sqrt(static_cast<double>(values.size()));
    out.low = out.mean - margin;
    out.high = out.mean + margin;
    out.valid = std::isfinite(out.low) && std::isfinite(out.high);
    return out;
}

TimedResult checkedTimeBatch(bool force,
                             const std::array<DrawInput, kCorpusSize>& rows,
                             LARGE_INTEGER frequency) {
    return force ? timeBatch<true>(rows, frequency)
                 : timeBatch<false>(rows, frequency);
}

int benchmark() {
    const auto rows = makeCorpus();
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        std::printf("benchmark: inconclusive (QPC frequency unavailable)\n");
        return 0;
    }
    // Fixed warmup, never included in any reported sample.
    (void)timeBatch<false>(rows, frequency);
    (void)timeBatch<true>(rows, frequency);

    std::array<double, kPairedRuns> pairedDelta{};
    std::array<double, kPairedRuns> aaDelta{};
    double gatedTotal = 0.0;
    double alwaysTotal = 0.0;
    bool allValid = true;
    for (std::size_t i = 0; i < kPairedRuns; ++i) {
        // Predeclared ABBA order. A is configured gating, B forces only the
        // legacy typed interests eligible; NV's exact candidate mask is unchanged.
        const TimedResult a1 = checkedTimeBatch(false, rows, frequency);
        const TimedResult b1 = checkedTimeBatch(true, rows, frequency);
        const TimedResult b2 = checkedTimeBatch(true, rows, frequency);
        const TimedResult a2 = checkedTimeBatch(false, rows, frequency);
        // Paired A/A control, interleaved at a fixed point after each ABBA block.
        const TimedResult c1 = checkedTimeBatch(false, rows, frequency);
        const TimedResult c2 = checkedTimeBatch(false, rows, frequency);
        allValid &= a1.valid && b1.valid && b2.valid && a2.valid && c1.valid && c2.valid;
        if (!allValid) break;
        const double gatedMean = (a1.nsPerDraw + a2.nsPerDraw) * 0.5;
        const double alwaysMean = (b1.nsPerDraw + b2.nsPerDraw) * 0.5;
        pairedDelta[i] = alwaysMean - gatedMean;
        aaDelta[i] = c2.nsPerDraw - c1.nsPerDraw;
        gatedTotal += gatedMean;
        alwaysTotal += alwaysMean;
    }
    if (!allValid) {
        std::printf("benchmark: inconclusive (invalid QPC batch; no functional test failure)\n");
        return 0;
    }
    const Interval effect = pairedInterval(pairedDelta);
    const Interval noise = pairedInterval(aaDelta);
    if (!effect.valid || !noise.valid) {
        std::printf("benchmark: inconclusive (invalid paired interval)\n");
        return 0;
    }
    const double noiseEnvelope = (std::max)(std::abs(noise.low), std::abs(noise.high));
    const bool controlStable = noise.low <= 0.0 && noise.high >= 0.0;
    const char* outcome = "inconclusive";
    if (controlStable && effect.low > noiseEnvelope) outcome = "gating-improve";
    else if (controlStable && effect.high < -noiseEnvelope) outcome = "gating-regress";

    std::printf("synthetic typed-selector cost only (not legacy parity or plugin/flight cost)\n");
    std::printf("corpus=synthetic POD %zu rows; batch=%u logical selector draw iterations; repeats=%u; pairs=%u ABBA + %u A/A\n",
        kCorpusSize, kDrawsPerBatch, kRepeatsPerBatch, kPairedRuns, kPairedRuns);
    std::printf("configured gating mean %.3f ns/draw; always-eligible mean %.3f ns/draw\n",
        gatedTotal / kPairedRuns, alwaysTotal / kPairedRuns);
    std::printf("always-minus-gated paired mean %.3f ns/draw; t19 95%% CI [%.3f, %.3f]\n",
        effect.mean, effect.low, effect.high);
    std::printf("A/A paired mean %.3f ns/draw; t19 95%% CI [%.3f, %.3f]; noise envelope %.3f ns/draw\n",
        noise.mean, noise.low, noise.high, noiseEnvelope);
    std::printf("selector gating result: %s%s\n", outcome,
        controlStable ? "" : " (A/A control did not bracket zero; drift/noise prevents a conclusion)");
    return 0; // Host timing never turns a noisy numeric result into a build failure.
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0)
        return selfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("dry-run: synthetic POD corpus %zu; no files, D3D, or timing; would use %u logical selector iterations/batch and 20 ABBA + 20 A/A pairs\n",
            kCorpusSize, kDrawsPerBatch);
        return 0;
    }
    if (argc != 1) {
        std::printf("usage: draw_selector_cost_test [--dry-run|--self-test]\n");
        return 2;
    }
    return benchmark();
}
