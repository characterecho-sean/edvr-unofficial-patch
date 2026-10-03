#include "../../src/d3d11/draw_ladder.h"
#include "../../src/d3d11/draw_ladder_trace.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <windows.h>
#include <vector>

namespace ladder = edvr::draw_ladder;
namespace trace = edvr::draw_ladder_trace;
namespace draw_interest = edvr::draw_interest;
namespace plugin_cost = edvr::plugin_cost;

struct SiteEvent final {
    ladder::SiteId id;
    ladder::SiteKind kind;
    ladder::SiteResult result;
};

struct ActionEvent final {
    ladder::ActionId id;
    ladder::ActionRecord record;
};

struct TraceCapture final {
    static constexpr bool enabled = true;
    std::vector<SiteEvent> sites;
    std::vector<ActionEvent> actions;

    template <ladder::SiteId Id, ladder::SiteKind Kind>
    void site(const ladder::SiteResult& result) {
        sites.push_back({Id, Kind, result});
    }

    template <ladder::ActionId Id>
    void action(const ladder::ActionRecord& record) {
        actions.push_back({Id, record});
    }
};

struct NoTraceProbe final {
    static constexpr bool enabled = false;
    unsigned callbacks = 0;

    template <ladder::SiteId, ladder::SiteKind>
    void site(const ladder::SiteResult&) { ++callbacks; }

    template <ladder::ActionId>
    void action(const ladder::ActionRecord&) { ++callbacks; }
};

struct Scenario final {
    ladder::RouteId route = ladder::RouteId::kVrEye;
    ladder::SiteId claimAt = ladder::SiteId::kSiteIdCount;
    ladder::SiteId exitAt = ladder::SiteId::kSiteIdCount;
    bool foreignOwner = false;
    bool drawGateOff = false;
    std::uint16_t exitSubsite = 0;
    std::int16_t verdict = static_cast<std::int16_t>(ladder::VerdictOrdinal::kHolo);
};

struct ModelVisitor final {
    Scenario scenario;
    std::vector<ladder::SiteId> visited;
    std::array<unsigned, 128> handlerCalls{};
    unsigned handlers = 0;

    template <class SiteType>
    ladder::SiteResult visit() {
        ++handlers;
        const auto index = static_cast<std::uint16_t>(SiteType::id);
        if (index < handlerCalls.size()) ++handlerCalls[index];
        visited.push_back(SiteType::id);
        return handle(SiteType::id, SiteType::kind);
    }

    ladder::SiteResult handle(ladder::SiteId id, ladder::SiteKind kind) {
        if (id == ladder::SiteId::kRouteSelected) {
            return ladder::SiteResult::observed(static_cast<std::uint16_t>(scenario.route));
        }
        if (id == ladder::SiteId::kForeignContextNone && scenario.foreignOwner) {
            return ladder::SiteResult::exited(
                static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone), 1);
        }
        if (id == ladder::SiteId::kDrawGateDisabledNone && scenario.drawGateOff) {
            return ladder::SiteResult::exited(
                static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
        }
        if (id == ladder::SiteId::kDrawGateDisabledNone) {
            return ladder::SiteResult::observed();
        }
        if (id == ladder::SiteId::kEyeRangeSkip &&
            id == scenario.exitAt) {
            return ladder::SiteResult::exited(
                static_cast<std::int16_t>(ladder::VerdictOrdinal::kSkip));
        }
        if (id == ladder::SiteId::kWitchspaceStarsSkip &&
            id == scenario.exitAt) {
            return ladder::SiteResult::exited(
                static_cast<std::int16_t>(ladder::VerdictOrdinal::kSkip));
        }
        if (id == ladder::SiteId::kOffscreenCensusSkip &&
            id == scenario.exitAt) {
            return ladder::SiteResult::exited(
                static_cast<std::int16_t>(ladder::VerdictOrdinal::kSkip));
        }
        if (kind == ladder::SiteKind::Claim && id == scenario.claimAt) {
            return ladder::SiteResult::claimed(scenario.verdict);
        }
        if (kind == ladder::SiteKind::Exit && id == scenario.exitAt) {
            return ladder::SiteResult::exited(
                static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone), scenario.exitSubsite);
        }
        if (kind == ladder::SiteKind::Observe) return ladder::SiteResult::observed();
        return ladder::SiteResult::declined();
    }
};

struct CandidateInterest final {
    bool pluginDispatchEnabled = true;
    bool candidate = true;
    draw_interest::InterestMask publishedInterests =
        (draw_interest::InterestMask{1} << draw_interest::kInterestCount) - 1;
    draw_interest::InterestMask cachedInterests = 0;
    bool legacyMaskLoaded = false;
    unsigned legacyQueries = 0;
    unsigned legacyMaskLoads = 0;
    unsigned candidateQueries = 0;
    unsigned candidateLoads = 0;

    template <class SiteType>
    bool eligible() noexcept {
        if constexpr (SiteType::shaderCandidateGated) {
            ++candidateQueries;
            if (!pluginDispatchEnabled) return false;
            ++candidateLoads;
            return candidate;
        } else {
            ++legacyQueries;
            if (!legacyMaskLoaded) {
                cachedInterests = publishedInterests;
                legacyMaskLoaded = true;
                ++legacyMaskLoads;
            }
            return draw_interest::contains(cachedInterests, SiteType::interestId);
        }
    }
};

struct CpuSiteEvent final {
    plugin_cost::Owner owner;
    std::uint16_t site;
    plugin_cost::SiteEvent event;
};

struct CpuTickEvent final {
    plugin_cost::Owner owner;
    std::uint16_t site;
    std::uint64_t ticks;
};

struct LadderFakeClock final {
    static inline std::int64_t next = 100;
    static inline unsigned reads = 0;
    static std::int64_t now() noexcept {
        ++reads;
        const std::int64_t result = next;
        next += 10;
        return result;
    }
    static void reset() noexcept { next = 100; reads = 0; }
};

struct LadderFakeSink final {
    static inline std::vector<CpuSiteEvent> sites;
    static inline std::vector<CpuTickEvent> tickEvents;
    static void site(plugin_cost::Owner owner, std::uint16_t siteId,
                     plugin_cost::SiteEvent event) noexcept {
        sites.push_back({owner, siteId, event});
    }
    static void ticks(plugin_cost::Owner owner, std::uint16_t siteId,
                      std::uint64_t value) noexcept {
        tickEvents.push_back({owner, siteId, value});
    }
    static void reset() { sites.clear(); tickEvents.clear(); }
};

using LadderSampledCpu = plugin_cost::SampledCpu<LadderFakeClock, LadderFakeSink>;

bool check(bool condition, const char* label);

template <class... Sites, class TracePolicy>
ladder::Flow runSequence(ladder::Sequence<Sites...> sequence, ModelVisitor& visitor,
                         CandidateInterest& interest, TracePolicy& trace) {
    return ladder::visitOrdered(sequence, visitor, interest, trace);
}

std::size_t traceSiteIndex(const TraceCapture& trace, ladder::SiteId id) {
    for (std::size_t i = 0; i < trace.sites.size(); ++i)
        if (trace.sites[i].id == id) return i;
    return trace.sites.size();
}

bool typedInterestChecks() {
    struct GateCase final {
        ladder::SiteId site;
        draw_interest::InterestId interest;
        ladder::SiteKind kind;
    };
    constexpr GateCase cases[] = {
        {ladder::SiteId::kParticleSubstitute, draw_interest::InterestId::ParticleSubstitute,
         ladder::SiteKind::Claim},
        {ladder::SiteId::kWitchspaceStarsSkip, draw_interest::InterestId::WitchspaceStars,
         ladder::SiteKind::Claim},
        {ladder::SiteId::kTargetSharpClaim, draw_interest::InterestId::TargetSharp,
         ladder::SiteKind::Claim},
        {ladder::SiteId::kFssPanelClaim, draw_interest::InterestId::FssPanel,
         ladder::SiteKind::Claim},
        {ladder::SiteId::kFssRevealClaim, draw_interest::InterestId::FssReveal,
         ladder::SiteKind::Claim},
        {ladder::SiteId::kFssDumpClaim, draw_interest::InterestId::FssDump,
         ladder::SiteKind::Claim},
        {ladder::SiteId::kIntroCurveObserve, draw_interest::InterestId::IntroCurveObserve,
         ladder::SiteKind::Observe},
        {ladder::SiteId::kPanelCurveObserve, draw_interest::InterestId::PanelCurveObserve,
         ladder::SiteKind::Observe},
    };
    bool ok = true;
    for (const GateCase& gate : cases) {
        Scenario scenario;
        scenario.route = ladder::RouteId::kVrEye;
        scenario.verdict = static_cast<std::int16_t>(ladder::VerdictOrdinal::kHolo);
        if (gate.kind == ladder::SiteKind::Claim) scenario.claimAt = gate.site;
        else scenario.exitAt = ladder::SiteId::kEyeNoDistanceNone;

        ModelVisitor visitor{scenario};
        CandidateInterest interest;
        interest.publishedInterests = draw_interest::bit(gate.interest);
        TraceCapture trace;
        ladder::Flow flow = runSequence(ladder::CommonSequence{}, visitor, interest, trace);
        if (flow == ladder::Flow::Continue)
            flow = runSequence(ladder::VrEyeSequence{}, visitor, interest, trace);

        const std::size_t gateIndex = traceSiteIndex(trace, gate.site);
        const bool reachesExpectedTerminal = gate.kind == ladder::SiteKind::Claim
            ? flow == ladder::Flow::Stop && visitor.visited.back() == gate.site
            : flow == ladder::Flow::Stop &&
                  visitor.visited.back() == ladder::SiteId::kEyeNoDistanceNone;
        ok &= check(reachesExpectedTerminal && visitor.handlerCalls[static_cast<std::uint16_t>(gate.site)] == 1,
                    "enabled typed interest reaches its actual shared-selector handler once");
        ok &= check(gateIndex < trace.sites.size() && trace.sites[gateIndex].kind == gate.kind &&
                    trace.sites[gateIndex].result.outcome ==
                        (gate.kind == ladder::SiteKind::Claim ? ladder::SiteOutcome::Claimed
                                                              : ladder::SiteOutcome::Observed),
                    "enabled typed interest traces the stable site kind and outcome");
        ok &= check(interest.legacyMaskLoads == 1,
                    "multiple typed gates share one lazy per-draw interest snapshot");
        unsigned reachedInterestSites = 0;
        for (const SiteEvent& event : trace.sites) {
            for (const GateCase& candidate : cases) {
                if (event.id == candidate.site) ++reachedInterestSites;
            }
        }
        ok &= check(interest.legacyQueries == reachedInterestSites,
                    "each reached optional site performs exactly one cached-bit check");
        ok &= check(visitor.handlerCalls[static_cast<std::uint16_t>(ladder::SiteId::kParticleProbe)] == 1,
                    "mandatory particle observation still runs when its substitute is not selected");
        if (gate.site != ladder::SiteId::kParticleSubstitute &&
            gate.site != ladder::SiteId::kWitchspaceStarsSkip) {
            const std::size_t particleProbe = traceSiteIndex(trace, ladder::SiteId::kParticleProbe);
            const std::size_t stateSnapshot = traceSiteIndex(trace, ladder::SiteId::kStateSnapshot);
            const std::size_t routeSelected = traceSiteIndex(trace, ladder::SiteId::kRouteSelected);
            const std::size_t eyeDepth = traceSiteIndex(trace, ladder::SiteId::kEyeDepthAndCount);
            const std::size_t eyeUiDepth = traceSiteIndex(trace, ladder::SiteId::kEyeUiDepthProbe);
            const std::size_t eyeHoloDepth = traceSiteIndex(trace, ladder::SiteId::kEyeHoloDepthProbe);
            ok &= check(particleProbe < stateSnapshot && stateSnapshot < routeSelected &&
                        routeSelected < eyeDepth && eyeDepth < eyeUiDepth &&
                        eyeUiDepth < eyeHoloDepth && eyeHoloDepth < gateIndex,
                        "mandatory common and eye observations preserve order around optional sites");
        }
        const std::size_t introPanel = traceSiteIndex(trace, ladder::SiteId::kIntroPanelClaim);
        const std::size_t introCurve = traceSiteIndex(trace, ladder::SiteId::kIntroCurveObserve);
        const std::size_t sunglare = traceSiteIndex(trace, ladder::SiteId::kSunglareNomination);
        const std::size_t census = traceSiteIndex(trace, ladder::SiteId::kEyeCensusSubmitted);
        const std::size_t crisp = traceSiteIndex(trace, ladder::SiteId::kUiCrispProbe);
        const std::size_t object = traceSiteIndex(trace, ladder::SiteId::kObjectProbe);
        if (gate.site != ladder::SiteId::kParticleSubstitute &&
            gate.site != ladder::SiteId::kWitchspaceStarsSkip) {
            bool censusOrder = introPanel < introCurve;
            if (gate.site == ladder::SiteId::kIntroCurveObserve) {
                censusOrder &= introCurve == gateIndex && gateIndex < sunglare &&
                               sunglare < census && census < crisp && crisp < object;
            } else {
                censusOrder &= introCurve < sunglare && sunglare < census &&
                               census < crisp && crisp < object && object < gateIndex;
            }
            ok &= check(censusOrder,
                        "mandatory census and quality observers retain their canonical order");
        }

        bool inactiveHandlersSkipped = true;
        for (const GateCase& other : cases) {
            if (other.site == gate.site) continue;
            const unsigned calls = visitor.handlerCalls[static_cast<std::uint16_t>(other.site)];
            if (calls != 0) inactiveHandlersSkipped = false;
            const std::size_t otherIndex = traceSiteIndex(trace, other.site);
            if (otherIndex < trace.sites.size()) {
                ok &= check(trace.sites[otherIndex].result.outcome == ladder::SiteOutcome::NotEligible &&
                            trace.sites[otherIndex].kind == other.kind &&
                            trace.sites[otherIndex].result.flow == ladder::Flow::Continue,
                            "reached inactive site is trace-only and retains its stable kind");
            }
        }
        ok &= check(inactiveHandlersSkipped,
                    "inactive optional-site handlers are not executed");

        bool foundNotEligible = false;
        for (const auto& event : trace.sites) {
            if (event.id == gate.site) continue;
            for (const GateCase& other : cases) {
                if (event.id == other.site && event.result.outcome == ladder::SiteOutcome::NotEligible) {
                    foundNotEligible = true;
                    ok &= check(event.kind == other.kind && event.result.flow == ladder::Flow::Continue,
                                "inactive trace event preserves typed kind and nonterminal flow");
                }
            }
        }
        // At least the earlier common optional sites are always reached before
        // every later gate; the selected key itself is eligible, never NotEligible.
        if (gate.site != ladder::SiteId::kParticleSubstitute)
            ok &= check(foundNotEligible, "disabled reached interest emits a trace-only NotEligible event");
    }

    // Refresh the published mask between otherwise identical selector runs.
    // This models the per-draw snapshot boundary; registry tests separately pin
    // configure-time reseeding against an unchanged shader shadow.
    Scenario toggle;
    toggle.route = ladder::RouteId::kVrEye;
    toggle.claimAt = ladder::SiteId::kTargetSharpClaim;
    ModelVisitor disabledVisitor{toggle};
    CandidateInterest disabled;
    disabled.publishedInterests = 0;
    TraceCapture disabledTrace;
    (void)runSequence(ladder::CommonSequence{}, disabledVisitor, disabled, disabledTrace);
    (void)runSequence(ladder::VrEyeSequence{}, disabledVisitor, disabled, disabledTrace);
    ok &= check(disabledVisitor.handlerCalls[static_cast<std::uint16_t>(toggle.claimAt)] == 0,
                "published-off snapshot suppresses target-sharp predicate");

    ModelVisitor enabledVisitor{toggle};
    CandidateInterest enabled;
    enabled.publishedInterests = draw_interest::bit(draw_interest::InterestId::TargetSharp);
    TraceCapture enabledTrace;
    (void)runSequence(ladder::CommonSequence{}, enabledVisitor, enabled, enabledTrace);
    const auto enabledFlow = runSequence(ladder::VrEyeSequence{}, enabledVisitor, enabled,
                                         enabledTrace);
    ok &= check(enabledFlow == ladder::Flow::Stop &&
                enabledVisitor.handlerCalls[static_cast<std::uint16_t>(toggle.claimAt)] == 1,
                "refreshed published snapshot enables the same selector site without changing ladder inputs");
    ok &= check(traceSiteIndex(disabledTrace, toggle.claimAt) < disabledTrace.sites.size() &&
                disabledTrace.sites[traceSiteIndex(disabledTrace, toggle.claimAt)].result.outcome ==
                    ladder::SiteOutcome::NotEligible &&
                enabledTrace.sites[traceSiteIndex(enabledTrace, toggle.claimAt)].result.outcome ==
                    ladder::SiteOutcome::Claimed,
                "interest refresh changes the typed trace outcome at the same site");

    for (unsigned mode = 0; mode < 2; ++mode) {
        const bool gateOff = mode != 0;
        Scenario earlyExit;
        earlyExit.foreignOwner = !gateOff;
        earlyExit.drawGateOff = gateOff;
        ModelVisitor earlyVisitor{earlyExit};
        CandidateInterest earlyInterest;
        earlyInterest.publishedInterests = 0;
        TraceCapture earlyTrace;
        const auto earlyFlow = runSequence(ladder::CommonSequence{}, earlyVisitor,
                                           earlyInterest, earlyTrace);
        const ladder::SiteId terminal = gateOff ? ladder::SiteId::kDrawGateDisabledNone
                                                 : ladder::SiteId::kForeignContextNone;
        ok &= check(earlyFlow == ladder::Flow::Stop && earlyVisitor.visited.back() == terminal &&
                    earlyInterest.legacyMaskLoads == 0 && earlyInterest.legacyQueries == 0 &&
                    earlyInterest.candidateQueries == 0 && earlyInterest.candidateLoads == 0 &&
                    earlyVisitor.handlerCalls[static_cast<std::uint16_t>(
                        ladder::SiteId::kParticleSubstitute)] == 0,
                    "terminal common exits precede every interest and candidate query");
    }
    return ok;
}

bool cpuPolicyChecks() {
    static_assert(ladder::cpuOwnerForSite<ladder::SiteId::kNightVisionClaim>() ==
                  plugin_cost::Owner::CockpitVisuals);
    static_assert(ladder::cpuOwnerForSite<ladder::SiteId::kFssPanelClaim>() ==
                  plugin_cost::Owner::Scanners);
    static_assert(ladder::cpuOwnerForSite<ladder::SiteId::kIntroCurveObserve>() ==
                  plugin_cost::Owner::Intro);
    static_assert(ladder::cpuOwnerForSite<ladder::SiteId::kPanelCurveObserve>() ==
                  plugin_cost::Owner::OnFootPanel);
    static_assert(ladder::cpuOwnerForSite<ladder::SiteId::kEyeUiDepthProbe>() ==
                  plugin_cost::Owner::TemporalAa);
    static_assert(ladder::cpuOwnerForSite<ladder::SiteId::kRouteSelected>() ==
                  plugin_cost::Owner::Core);

    using NvOnly = ladder::Sequence<
        ladder::ShaderCandidateGatedClaim<ladder::SiteId::kNightVisionClaim>>;
    bool ok = true;
    const auto hasSiteEvent = [](plugin_cost::Owner owner, ladder::SiteId site,
                                 plugin_cost::SiteEvent event) {
        for (const auto& row : LadderFakeSink::sites)
            if (row.owner == owner && row.site == static_cast<uint16_t>(site) && row.event == event)
                return true;
        return false;
    };
    const auto hasTick = [](plugin_cost::Owner owner, ladder::SiteId site, uint64_t ticks) {
        for (const auto& row : LadderFakeSink::tickEvents)
            if (row.owner == owner && row.site == static_cast<uint16_t>(site) && row.ticks == ticks)
                return true;
        return false;
    };

    // A reached candidate with a known miss records the decision but never
    // invokes/times its predicate handler.
    LadderFakeClock::reset();
    LadderFakeSink::reset();
    Scenario missScenario;
    missScenario.claimAt = ladder::SiteId::kNightVisionClaim;
    ModelVisitor missVisitor{missScenario};
    CandidateInterest missInterest;
    missInterest.candidate = false;
    ladder::NoTrace noTrace;
    LadderSampledCpu missCpu;
    const auto missFlow = ladder::visitOrdered(NvOnly{}, missVisitor, missInterest, noTrace, missCpu);
    ok &= check(missFlow == ladder::Flow::Continue && missInterest.candidateQueries == 1 &&
                missInterest.candidateLoads == 1 &&
                missVisitor.handlerCalls[static_cast<uint16_t>(ladder::SiteId::kNightVisionClaim)] == 0,
                "sampled known shader mismatch reaches the gate but skips its handler");
    ok &= check(LadderFakeClock::reads == 0 && LadderFakeSink::tickEvents.empty() &&
                hasSiteEvent(plugin_cost::Owner::CockpitVisuals,
                             ladder::SiteId::kNightVisionClaim, plugin_cost::SiteEvent::Reached) &&
                hasSiteEvent(plugin_cost::Owner::CockpitVisuals,
                             ladder::SiteId::kNightVisionClaim, plugin_cost::SiteEvent::NotEligible) &&
                !hasSiteEvent(plugin_cost::Owner::CockpitVisuals,
                              ladder::SiteId::kNightVisionClaim, plugin_cost::SiteEvent::Invoked),
                "eligibility miss has no handler clock, invocation, or owner ticks");

    // An eligible claim times exactly the real shared-selector handler scope.
    LadderFakeClock::reset();
    LadderFakeSink::reset();
    Scenario eligibleScenario;
    eligibleScenario.claimAt = ladder::SiteId::kNightVisionClaim;
    eligibleScenario.verdict = static_cast<int16_t>(ladder::VerdictOrdinal::kNightVision);
    ModelVisitor eligibleVisitor{eligibleScenario};
    CandidateInterest eligibleInterest;
    LadderSampledCpu eligibleCpu;
    const auto eligibleFlow = ladder::visitOrdered(NvOnly{}, eligibleVisitor, eligibleInterest,
                                                   noTrace, eligibleCpu);
    ok &= check(eligibleFlow == ladder::Flow::Stop && eligibleInterest.candidateQueries == 1 &&
                eligibleVisitor.handlerCalls[static_cast<uint16_t>(ladder::SiteId::kNightVisionClaim)] == 1,
                "eligible NV site uses the production typed claim and terminates");
    ok &= check(LadderFakeClock::reads == 2 && LadderFakeSink::tickEvents.size() == 1 &&
                hasSiteEvent(plugin_cost::Owner::CockpitVisuals,
                             ladder::SiteId::kNightVisionClaim, plugin_cost::SiteEvent::Reached) &&
                hasSiteEvent(plugin_cost::Owner::CockpitVisuals,
                             ladder::SiteId::kNightVisionClaim, plugin_cost::SiteEvent::Invoked) &&
                hasTick(plugin_cost::Owner::CockpitVisuals,
                        ladder::SiteId::kNightVisionClaim, 10),
                "eligible handler has exact fake-clock ticks under its logical owner");

    // The same typed selector under NoCpu retains behavior but compiles away
    // every clock/sink callback. Trace recording remains independent.
    LadderFakeClock::reset();
    LadderFakeSink::reset();
    ModelVisitor noCpuVisitor{eligibleScenario};
    CandidateInterest noCpuInterest;
    plugin_cost::NoCpu noCpu;
    TraceCapture capture;
    const auto noCpuFlow = ladder::visitOrdered(NvOnly{}, noCpuVisitor, noCpuInterest,
                                                capture, noCpu);
    ok &= check(noCpuFlow == ladder::Flow::Stop && noCpuVisitor.handlerCalls[
                    static_cast<uint16_t>(ladder::SiteId::kNightVisionClaim)] == 1 &&
                LadderFakeClock::reads == 0 && LadderFakeSink::sites.empty() &&
                LadderFakeSink::tickEvents.empty(),
                "NoCpu preserves a traced selector claim with zero clock or collector calls");
    ok &= check(capture.sites.size() == 1 &&
                capture.sites[0].id == ladder::SiteId::kNightVisionClaim &&
                capture.sites[0].result.outcome == ladder::SiteOutcome::Claimed,
                "trace capture records the same claim while CPU sampling is suppressed");

    // A real earlier terminal rung in the shared EyeSequence prevents all
    // later candidate queries and CPU records, including Night Vision.
    LadderFakeClock::reset();
    LadderFakeSink::reset();
    Scenario earlyScenario;
    earlyScenario.route = ladder::RouteId::kVrEye;
    earlyScenario.claimAt = ladder::SiteId::kEyeCensusSkip;
    earlyScenario.verdict = static_cast<int16_t>(ladder::VerdictOrdinal::kSkip);
    ModelVisitor earlyVisitor{earlyScenario};
    CandidateInterest earlyInterest;
    LadderSampledCpu earlyCpu;
    const auto commonFlow = ladder::visitOrdered(ladder::CommonSequence{}, earlyVisitor,
                                                 earlyInterest, noTrace, earlyCpu);
    const auto eyeFlow = commonFlow == ladder::Flow::Continue
        ? ladder::visitOrdered(ladder::VrEyeSequence{}, earlyVisitor, earlyInterest,
                               noTrace, earlyCpu)
        : commonFlow;
    ok &= check(eyeFlow == ladder::Flow::Stop && !earlyVisitor.visited.empty() &&
                earlyVisitor.visited.back() == ladder::SiteId::kEyeCensusSkip,
                "prior eye claimant remains the terminal ladder rung");
    ok &= check(earlyInterest.candidateQueries == 0 && earlyInterest.candidateLoads == 0,
                "prior eye claimant skips later shader candidate queries");
    ok &= check(!hasSiteEvent(plugin_cost::Owner::CockpitVisuals,
                              ladder::SiteId::kNightVisionClaim, plugin_cost::SiteEvent::Reached) &&
                !hasSiteEvent(plugin_cost::Owner::CockpitVisuals,
                              ladder::SiteId::kNightVisionClaim, plugin_cost::SiteEvent::Invoked) &&
                !hasTick(plugin_cost::Owner::CockpitVisuals,
                         ladder::SiteId::kNightVisionClaim, 10),
                "prior eye claimant records no later NV owner timing or site events");
    return ok;
}

// Frozen ordered reference for the branch/site identities before the shared
// selector was introduced. It is deliberately a literal list, not derived
// from CommonSequence/EyeSequence, so reordering the production table fails.
constexpr ladder::SiteId kFrozenCommon[] = {
    ladder::SiteId::kFssChromeSkip,
    ladder::SiteId::kForeignContextNone,
    ladder::SiteId::kDrawGateDisabledNone,
    ladder::SiteId::kParticleProbe,
    ladder::SiteId::kParticleSubstitute,
    ladder::SiteId::kWitchspaceStarsSkip,
    ladder::SiteId::kStateSnapshot,
    ladder::SiteId::kRouteSelected,
};

constexpr ladder::SiteId kFrozenOffscreen[] = {
    ladder::SiteId::kOffscreenSubmittedDraw,
    ladder::SiteId::kOffscreenBackdropBlit,
    ladder::SiteId::kOffscreenViewport,
    ladder::SiteId::kOffscreenAutoState,
    ladder::SiteId::kOffscreenWakePulseSkip,
    ladder::SiteId::kOffscreenCensusSkip,
    ladder::SiteId::kOffscreenLoaderPanel,
    ladder::SiteId::kOffscreenQuadSkip,
    ladder::SiteId::kOffscreenBodyLayerUpdate,
    ladder::SiteId::kOffscreenFallthroughNone,
};

constexpr ladder::SiteId kFrozenEye[] = {
    ladder::SiteId::kEyeDepthAndCount,
    ladder::SiteId::kEyeUiDepthProbe,
    ladder::SiteId::kEyeHoloDepthProbe,
    ladder::SiteId::kIntroPanelClaim,
    ladder::SiteId::kIntroCurveObserve,
    ladder::SiteId::kSunglareNomination,
    ladder::SiteId::kEyeCensusSubmitted,
    ladder::SiteId::kUiCrispProbe,
    ladder::SiteId::kObjectProbe,
    ladder::SiteId::kEyeCensusSkip,
    ladder::SiteId::kEyeRangeSkip,
    ladder::SiteId::kNightVisionClaim,
    ladder::SiteId::kRemlokHideSkip,
    ladder::SiteId::kRemlokScissorClaim,
    ladder::SiteId::kHoloClaim,
    ladder::SiteId::kTargetSharpClaim,
    ladder::SiteId::kScrimClaim,
    ladder::SiteId::kEyeBackdropComposite,
    ladder::SiteId::kFssPanelClaim,
    ladder::SiteId::kFssRevealClaim,
    ladder::SiteId::kFssDumpClaim,
    ladder::SiteId::kResolveBindClaim,
    ladder::SiteId::kSunglareSkip,
    ladder::SiteId::kSunglareSteadyClaim,
    ladder::SiteId::kGlareClampClaim,
    ladder::SiteId::kHeadOffsetObserve,
    ladder::SiteId::kPanelCurveObserve,
    ladder::SiteId::kEyeNoDistanceNone,
    ladder::SiteId::kPanelEligibilityNone,
    ladder::SiteId::kPanelDistanceClaim,
    ladder::SiteId::kPanelTailNone,
};

bool frozenShouldStop(const Scenario& s, ladder::SiteId id, ladder::SiteKind kind) {
    if (id == ladder::SiteId::kForeignContextNone && s.foreignOwner) return true;
    if (id == ladder::SiteId::kDrawGateDisabledNone && s.drawGateOff) return true;
    if (id == ladder::SiteId::kEyeRangeSkip && id == s.exitAt) return true;
    if (id == ladder::SiteId::kWitchspaceStarsSkip && id == s.exitAt) return true;
    if (id == ladder::SiteId::kOffscreenCensusSkip && id == s.exitAt) return true;
    if (kind == ladder::SiteKind::Claim && id == s.claimAt) return true;
    return kind == ladder::SiteKind::Exit && id == s.exitAt;
}

ladder::SiteKind frozenKind(ladder::SiteId id) {
    if (id == ladder::SiteId::kForeignContextNone ||
        id == ladder::SiteId::kDrawGateDisabledNone ||
        id == ladder::SiteId::kOffscreenFallthroughNone ||
        id == ladder::SiteId::kEyeNoDistanceNone ||
        id == ladder::SiteId::kPanelEligibilityNone ||
        id == ladder::SiteId::kPanelTailNone ||
        id == ladder::SiteId::kFlatRuntimeBypass ||
        id == ladder::SiteId::kAutoRuntimeBypass ||
        id == ladder::SiteId::kDrawIndexedInstancedIndirectRuntimeBypass ||
        id == ladder::SiteId::kDrawInstancedIndirectRuntimeBypass ||
        id == ladder::SiteId::kInternalWorldBypass) return ladder::SiteKind::Exit;
    if (id == ladder::SiteId::kParticleProbe || id == ladder::SiteId::kStateSnapshot ||
        id == ladder::SiteId::kRouteSelected ||
        id == ladder::SiteId::kOffscreenSubmittedDraw ||
        id == ladder::SiteId::kOffscreenViewport || id == ladder::SiteId::kOffscreenAutoState ||
        id == ladder::SiteId::kOffscreenBodyLayerUpdate ||
        id == ladder::SiteId::kEyeDepthAndCount || id == ladder::SiteId::kEyeUiDepthProbe ||
        id == ladder::SiteId::kEyeHoloDepthProbe || id == ladder::SiteId::kIntroCurveObserve ||
        id == ladder::SiteId::kSunglareNomination || id == ladder::SiteId::kEyeCensusSubmitted ||
        id == ladder::SiteId::kUiCrispProbe || id == ladder::SiteId::kObjectProbe ||
        id == ladder::SiteId::kHeadOffsetObserve || id == ladder::SiteId::kPanelCurveObserve) {
        return ladder::SiteKind::Observe;
    }
    return ladder::SiteKind::Claim;
}

std::vector<ladder::SiteId> frozenWalkRanges(const Scenario& s,
                                             const ladder::SiteId* first,
                                             std::size_t firstCount,
                                             const ladder::SiteId* second,
                                             std::size_t secondCount) {
    std::vector<ladder::SiteId> result;
    const auto append = [&](ladder::SiteId id) {
        result.push_back(id);
        return frozenShouldStop(s, id, frozenKind(id));
    };
    for (std::size_t i = 0; i < firstCount; ++i) if (append(first[i])) return result;
    for (std::size_t i = 0; i < secondCount; ++i) if (append(second[i])) return result;
    return result;
}

template <std::size_t N>
std::vector<ladder::SiteId> frozenWalk(const Scenario& s,
                                      const ladder::SiteId (&common)[8],
                                      const ladder::SiteId (&tail)[N]) {
    return frozenWalkRanges(s, common, 8, tail, N);
}

bool same(const std::vector<ladder::SiteId>& a, const std::vector<ladder::SiteId>& b) {
    return a == b;
}

bool check(bool condition, const char* label) {
    if (condition) return true;
    std::fprintf(stderr, "draw ladder test failed: %s\n", label);
    return false;
}

bool interestMaskChecks() {
    using namespace draw_interest;
    constexpr auto target = InterestId::TargetSharp;
    constexpr auto witchspace = InterestId::WitchspaceStars;
    constexpr auto fssPanel = InterestId::FssPanel;
    static_assert(static_cast<std::uint8_t>(InterestId::TargetSharp) == 0);
    static_assert(static_cast<std::uint8_t>(InterestId::PanelCurveObserve) == 7);
    static_assert(bit(InterestId::Count) == 0);

    const InterestMask configured = bit(target) | bit(witchspace) | bit(fssPanel);
    const ShaderFilter filters[] = {
        {target, HashFilter::Pair, 0x1111, 0x2222},
        {target, HashFilter::Pair, 0x3333, 0x4444},
        {witchspace, HashFilter::Vertex, 0x5555, 0},
    };
    bool ok = true;
    ok &= check(buildCandidateMask(0, 0x1111, 0x2222, filters, 3) == 0,
                "configuration-off clears shader-filtered interest");
    ok &= check(buildCandidateMask(configured, 0x1111, 0x2222, nullptr, 0) == configured,
                "configured interests without filter rows remain eligible");
    ok &= check(buildCandidateMask(configured, 0x1111, 0x2222, filters, 3) ==
                    (bit(target) | bit(fssPanel)),
                "matching variants and unfiltered configured interests are retained");
    ok &= check(buildCandidateMask(configured, 0x3333, 0x4444, filters, 3) ==
                    (bit(target) | bit(fssPanel)),
                "shader variants are combined as an any-match set");
    ok &= check(buildCandidateMask(configured, 0x9999, 0x8888, filters, 3) == bit(fssPanel),
                "known mismatch suppresses only interests with complete filter coverage");
    ok &= check(buildCandidateMask(configured, 0, 0x8888, filters, 3) == configured,
                "unknown observed vertex hash conservatively retains configured interests");
    ok &= check(buildCandidateMask(configured, 0x5555, 0, filters, 3) == configured,
                "unknown observed pixel hash conservatively retains configured interests");
    ok &= check(!contains(bit(target), witchspace) && contains(bit(target), target),
                "typed mask membership does not alias adjacent interests");
    return ok;
}

bool productionRouteOrderCheck() {
    std::ifstream input("src/d3d11/vscreen.cpp");
    if (!input) return check(false, "read production draw-ladder route source");
    const std::string source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    const std::size_t function = source.find("LadderDecision beginPanelOverride");
    const std::size_t common = source.find(
        "visitOrdered(draw_ladder::CommonSequence{}", function);
    const std::size_t offscreen = source.find(
        "visitOrdered(draw_ladder::OffscreenSequence{}", common);
    const std::size_t eye = source.find(
        "visitOrdered(draw_ladder::EyeSequence{}", offscreen);
    return check(function != std::string::npos && common != std::string::npos &&
                 offscreen != std::string::npos && eye != std::string::npos &&
                 function < common && common < offscreen && offscreen < eye,
                 "production executes Common before the mutually-exclusive Offscreen/Eye route ladders");
}

template <class SequenceType>
bool bypassRouteChecks(ladder::RouteId route, ladder::SiteId site,
                       SequenceType sequence, const char* label) {
    Scenario scenario;
    scenario.route = route;
    scenario.exitAt = site;
    scenario.verdict = static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone);
    ModelVisitor visitor{scenario};
    CandidateInterest interest;
    TraceCapture trace;
    const auto flow = runSequence(sequence, visitor, interest, trace);
    return check(flow == ladder::Flow::Stop && visitor.visited.size() == 1 &&
                 visitor.visited[0] == site && interest.legacyQueries == 0 &&
                 interest.legacyMaskLoads == 0 && interest.candidateQueries == 0 &&
                 interest.candidateLoads == 0 && trace.sites.size() == 1 &&
                 trace.sites[0].result.outcome == ladder::SiteOutcome::Exited &&
                 trace.sites[0].result.verdict ==
                     static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone),
                 label);
}

bool createDirectory(const std::string& path) {
    if (CreateDirectoryA(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool createFixtureLog(const std::string& directory, const char* leaf) {
    const std::string path = directory + "\\" + leaf;
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    static const char contents[] = "synthetic draw ladder trace fixture\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, contents, static_cast<DWORD>(sizeof(contents) - 1),
                              &written, nullptr) && written == sizeof(contents) - 1;
    CloseHandle(file);
    return ok;
}

bool createNewerDecoyLog(const std::string& directory) {
    const std::string path = directory + "\\edvr_gfx_newer_decoy.log";
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    static const char contents[] = "decoy log must not be selected\n";
    DWORD written = 0;
    FILETIME future{};
    GetSystemTimeAsFileTime(&future);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = future.dwLowDateTime;
    ticks.HighPart = future.dwHighDateTime;
    ticks.QuadPart += 36000000000ull;
    future.dwLowDateTime = ticks.LowPart;
    future.dwHighDateTime = ticks.HighPart;
    const bool wrote = WriteFile(file, contents, static_cast<DWORD>(sizeof(contents) - 1),
                                 &written, nullptr) && written == sizeof(contents) - 1;
    const bool timestamped = SetFileTime(file, nullptr, nullptr, &future) != FALSE;
    CloseHandle(file);
    return wrote && timestamped;
}

std::wstring widen(const std::string& value) {
    const int length = MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, &result[0], length)) return {};
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

bool fileExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

struct FlatBypassWriter final {
    template <class SiteType>
    ladder::SiteResult visit() {
        static_assert(SiteType::id == ladder::SiteId::kFlatRuntimeBypass,
                      "writer fixture must use the production flat bypass site");
        return ladder::SiteResult::exited(
            static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    }
};

bool beginCapture(const std::string& directory, const char* logLeaf,
                  std::uint32_t frameNo) {
    if (!createDirectory(directory) || !createFixtureLog(directory, logLeaf)) return false;
    const auto wideLogPath = widen(directory + "\\" + logLeaf);
    if (wideLogPath.empty()) return false;
    edvr::draw_ladder_trace::configure(true, wideLogPath.c_str());
    if (!edvr::draw_ladder_trace::configured()) return false;
    edvr::draw_ladder_trace::armManual();
    edvr::draw_ladder_trace::frameBegin({frameNo});
    return edvr::draw_ladder_trace::capturing();
}

std::int16_t expectedTerminalVerdict(ladder::SiteId id) {
    using V = ladder::VerdictOrdinal;
    switch (id) {
    case ladder::SiteId::kFssChromeSkip:
    case ladder::SiteId::kWitchspaceStarsSkip:
    case ladder::SiteId::kOffscreenWakePulseSkip:
    case ladder::SiteId::kOffscreenCensusSkip:
    case ladder::SiteId::kEyeCensusSkip:
    case ladder::SiteId::kEyeRangeSkip:
    case ladder::SiteId::kRemlokHideSkip:
    case ladder::SiteId::kSunglareSkip:
        return static_cast<std::int16_t>(V::kSkip);
    case ladder::SiteId::kParticleSubstitute:
        return static_cast<std::int16_t>(V::kParticle);
    case ladder::SiteId::kOffscreenBackdropBlit:
    case ladder::SiteId::kEyeBackdropComposite:
        return static_cast<std::int16_t>(V::kBackdrop);
    case ladder::SiteId::kOffscreenLoaderPanel:
        return static_cast<std::int16_t>(V::kLoader);
    case ladder::SiteId::kOffscreenQuadSkip:
        return static_cast<std::int16_t>(V::kQuadSkip);
    case ladder::SiteId::kIntroPanelClaim:
        return static_cast<std::int16_t>(V::kIntro);
    case ladder::SiteId::kNightVisionClaim:
        return static_cast<std::int16_t>(V::kNightVision);
    case ladder::SiteId::kRemlokScissorClaim:
        return static_cast<std::int16_t>(V::kRemlok);
    case ladder::SiteId::kHoloClaim:
        return static_cast<std::int16_t>(V::kHolo);
    case ladder::SiteId::kTargetSharpClaim:
        return static_cast<std::int16_t>(V::kTarget);
    case ladder::SiteId::kScrimClaim:
        return static_cast<std::int16_t>(V::kScrim);
    case ladder::SiteId::kFssPanelClaim:
        return static_cast<std::int16_t>(V::kFssPanel);
    case ladder::SiteId::kFssRevealClaim:
        return static_cast<std::int16_t>(V::kReveal);
    case ladder::SiteId::kFssDumpClaim:
        return static_cast<std::int16_t>(V::kDump);
    case ladder::SiteId::kResolveBindClaim:
        return static_cast<std::int16_t>(V::kResolve);
    case ladder::SiteId::kSunglareSteadyClaim:
        return static_cast<std::int16_t>(V::kSteady);
    case ladder::SiteId::kGlareClampClaim:
        return static_cast<std::int16_t>(V::kGlareClamp);
    case ladder::SiteId::kPanelDistanceClaim:
        return static_cast<std::int16_t>(V::kPanel);
    case ladder::SiteId::kForeignContextNone:
    case ladder::SiteId::kDrawGateDisabledNone:
    case ladder::SiteId::kOffscreenFallthroughNone:
    case ladder::SiteId::kEyeNoDistanceNone:
    case ladder::SiteId::kPanelEligibilityNone:
    case ladder::SiteId::kPanelTailNone:
    case ladder::SiteId::kFlatRuntimeBypass:
    case ladder::SiteId::kAutoRuntimeBypass:
    case ladder::SiteId::kDrawIndexedInstancedIndirectRuntimeBypass:
    case ladder::SiteId::kDrawInstancedIndirectRuntimeBypass:
    case ladder::SiteId::kInternalWorldBypass:
        return static_cast<std::int16_t>(V::kNone);
    default:
        return -1;
    }
}

ladder::DrawCallKind directCallKind(char kind) {
    switch (kind) {
    case 'I': return ladder::DrawCallKind::DrawIndexed;
    case 'N': return ladder::DrawCallKind::DrawInstanced;
    case 'X': return ladder::DrawCallKind::DrawIndexedInstanced;
    case 'A': return ladder::DrawCallKind::Auto;
    case 'Z': return ladder::DrawCallKind::DrawIndexedInstancedIndirect;
    case 'Y': return ladder::DrawCallKind::DrawInstancedIndirect;
    default: return ladder::DrawCallKind::Draw;
    }
}

template <ladder::ActionId Id, class TracePolicy>
void appendTestIssue(TracePolicy& policy, ladder::DrawCallKind call,
                     std::uint16_t flags = 0, std::uint32_t instances = 1,
                     ladder::ActionOutcome outcome = ladder::ActionOutcome::Applied,
                     std::uint16_t issueCount = 1) {
    ladder::recordAction<TracePolicy, Id>(policy, [=] {
        ladder::ActionRecord action;
        action.phase = ladder::ActionPhase::Issue;
        action.outcome = outcome;
        action.call = call;
        action.flags = flags;
        action.issueCount = issueCount;
        const bool generatedArgsUnknown =
            (flags & ladder::kActionGeneratedDrawArgsUnavailable) != 0;
        action.count = outcome == ladder::ActionOutcome::Applied && !generatedArgsUnknown ? 240 : 0;
        action.instances = outcome == ladder::ActionOutcome::Applied && !generatedArgsUnknown ? instances : 0;
        if (!generatedArgsUnknown) switch (call) {
        case ladder::DrawCallKind::Draw:
        case ladder::DrawCallKind::DrawInstanced:
            action.start = 17; // StartVertexLocation
            action.startInstance = call == ladder::DrawCallKind::DrawInstanced ? 5 : 0;
            break;
        case ladder::DrawCallKind::DrawIndexed:
            action.start = 23; // StartIndexLocation
            action.baseVertex = -7;
            break;
        case ladder::DrawCallKind::DrawIndexedInstanced:
            action.start = 23; // StartIndexLocation
            action.baseVertex = -7;
            action.startInstance = 5;
            break;
        case ladder::DrawCallKind::DrawIndexedInstancedIndirect:
        case ladder::DrawCallKind::DrawInstancedIndirect:
            action.start = 128; // argumentByteOffset; GPU payload is opaque
            break;
        case ladder::DrawCallKind::Auto:
            break; // Auto carries no CPU-visible draw arguments.
        }
        return action;
    });
}

trace::PredicateFact makeHoloPredicateFact(
    ladder::SiteId site, const trace::DrawFacts& draw,
    bool patternIsBuffer = false) {
    using namespace edvr::holo_scrim_observation;
    trace::PredicateFact fact{};
    fact.siteId = static_cast<std::uint16_t>(site);
    fact.kind = trace::PredicateFactKind::Holo53;
    fact.known = trace::TriState::Yes;
    fact.detailsFinalized = true;
    HoloObservation& observed = fact.holo;
    const bool shape = draw.kind == static_cast<std::uint8_t>('X') && draw.count == 6 &&
                       draw.instances == 1;
    observed.gates.enabled = Tri::Yes;
    observed.gates.shapeReached = Tri::Yes;
    observed.gates.shapeMatched = shape ? Tri::Yes : Tri::No;
    observed.gates.helperReached = shape ? Tri::Yes : Tri::Unknown;
    if (shape) {
        observed.gates.helperEnabled = Tri::Yes;
        observed.gates.helperShapeReached = Tri::Yes;
        observed.gates.helperShapeMatched = Tri::Yes;
    }
    observed.missNotedBefore = Tri::No;
    observed.missNotedAfter = Tri::No;
    observed.missedDeltaKnown = true;
    if (shape) {
        const auto resource = [](std::uint32_t w, std::uint32_t h,
                                 std::uint32_t fmt) {
            ResourceObservation result{};
            result.source = ResourceSource::FreshResolveSuccess;
            result.resolveReached = Tri::Yes;
            result.resolved = Tri::Yes;
            result.rawAvailable = Tri::Yes;
            result.texture2D = Tri::Yes;
            result.a = w;
            result.b = h;
            result.fmt = fmt;
            return result;
        };
        if (patternIsBuffer) {
            observed.pattern = resource(4096, 2, 0);
            observed.pattern.texture2D = Tri::No;
        } else {
            observed.pattern = resource(256, 256, 70);
            observed.depth = resource(2048, 2048, 40);
            observed.eyeSize.reached = Tri::Yes;
            observed.eyeSize.statePresent = Tri::Yes;
            observed.eyeSize.result = Tri::Yes;
            observed.eyeSize.readMask = kDepthWidthRead | kDepthHeightRead |
                                        kEyeWidthRead | kEyeHeightRead;
            observed.eyeSize.depthW = 2048;
            observed.eyeSize.depthH = 2048;
            observed.eyeSize.eyeW = 2048;
            observed.eyeSize.eyeH = 2048;
        }
    }
    const bool patternMatches = observed.pattern.texture2D == Tri::Yes &&
        observed.pattern.a == 256 && observed.pattern.b == 256 &&
        observed.pattern.fmt == 70;
    const bool depthMatches = observed.depth.texture2D == Tri::Yes &&
        observed.eyeSize.result == Tri::Yes;
    observed.predicateResult = observed.gates.enabled == Tri::Yes && shape &&
        observed.gates.helperEnabled == Tri::Yes && patternMatches && depthMatches
        ? Tri::Yes : Tri::No;
    return fact;
}

trace::PredicateFact makeScrimPredicateFact(
    ladder::SiteId site, const trace::DrawFacts& draw,
    bool washIsBuffer = false) {
    using namespace edvr::holo_scrim_observation;
    trace::PredicateFact fact{};
    fact.siteId = static_cast<std::uint16_t>(site);
    fact.kind = trace::PredicateFactKind::Scrim55;
    fact.known = trace::TriState::Yes;
    fact.detailsFinalized = true;
    ScrimObservation& observed = fact.scrim;
    const bool shape = draw.kind == static_cast<std::uint8_t>('X') &&
                       draw.count >= 100 && draw.instances == 1;
    observed.gates.enabled = Tri::Yes;
    observed.gates.shapeReached = Tri::Yes;
    observed.gates.shapeMatched = shape ? Tri::Yes : Tri::No;
    observed.gates.helperReached = shape ? Tri::Yes : Tri::Unknown;
    if (shape) {
        observed.gates.helperEnabled = Tri::Yes;
        observed.gates.helperShapeReached = Tri::Yes;
        observed.gates.helperShapeMatched = Tri::Yes;
    }
    if (shape) {
        const auto resource = [](std::uint32_t w, std::uint32_t h,
                                 std::uint32_t fmt) {
            ResourceObservation result{};
            result.source = ResourceSource::FreshResolveSuccess;
            result.resolveReached = Tri::Yes;
            result.resolved = Tri::Yes;
            result.rawAvailable = Tri::Yes;
            result.texture2D = Tri::Yes;
            result.a = w;
            result.b = h;
            result.fmt = fmt;
            return result;
        };
        if (washIsBuffer) {
            observed.wash = resource(4096, 2, 0);
            observed.wash.texture2D = Tri::No;
        } else {
            observed.wash = resource(16, 16, 71);
            observed.ui = resource(2048, 2048, 40);
        }
    }
    const bool washMatches = observed.wash.texture2D == Tri::Yes &&
        observed.wash.a == 16 && observed.wash.b == 16 &&
        (observed.wash.fmt == 70 || observed.wash.fmt == 71 || observed.wash.fmt == 72);
    const bool uiMatches = observed.ui.texture2D == Tri::Yes && observed.ui.a >= 1024;
    observed.predicateResult = observed.gates.enabled == Tri::Yes && shape &&
        observed.gates.helperEnabled == Tri::Yes && washMatches && uiMatches
        ? Tri::Yes : Tri::No;
    return fact;
}

template <class SequenceType>
bool writeTerminalCase(ladder::RouteId route, ladder::SequenceId sequence,
                       char kind, ladder::SiteId terminal, bool includeCommon,
                       const ladder::SiteId* referenceTail, std::size_t referenceTailCount,
                       SequenceType selectedSequence, std::uint32_t instances = 1,
                       bool blocked = false, bool malformedGenerated = false) {
    namespace trace = edvr::draw_ladder_trace;
    const std::int16_t verdict = expectedTerminalVerdict(terminal);
    if (verdict < 0) return false;
    Scenario scenario;
    scenario.route = route;
    scenario.verdict = verdict;
    scenario.exitSubsite = blocked ? 1 : 0;
    const auto terminalKind = frozenKind(terminal);
    if (terminal == ladder::SiteId::kForeignContextNone) scenario.foreignOwner = true;
    else if (terminal == ladder::SiteId::kDrawGateDisabledNone) scenario.drawGateOff = true;
    else if (terminal == ladder::SiteId::kEyeRangeSkip ||
             terminal == ladder::SiteId::kWitchspaceStarsSkip ||
             terminal == ladder::SiteId::kOffscreenCensusSkip) scenario.exitAt = terminal;
    else if (terminalKind == ladder::SiteKind::Claim) scenario.claimAt = terminal;
    else scenario.exitAt = terminal;

    trace::DrawFacts facts{};
    facts.eyeDrawIndex = 1;
    facts.kind = static_cast<std::uint8_t>(kind);
    if (terminal == ladder::SiteId::kNightVisionClaim) {
        // The terminal-matrix NV row models the actual positive selector.
        facts.kind = static_cast<std::uint8_t>('X');
    }
    facts.route = route;
    facts.sequence = sequence;
    facts.count = 240;
    facts.instances = instances;
    if (terminal == ladder::SiteId::kHoloClaim) {
        facts.kind = static_cast<std::uint8_t>('X');
        facts.count = 6;
    } else if (terminal == ladder::SiteId::kTargetSharpClaim) {
        facts.kind = static_cast<std::uint8_t>('X');
        facts.count = 6;
    } else if (terminal == ladder::SiteId::kScrimClaim) {
        facts.kind = static_cast<std::uint8_t>('X');
        facts.count = 120;
    } else if (terminal == ladder::SiteId::kEyeBackdropComposite) {
        facts.kind = static_cast<std::uint8_t>('X');
        facts.count = 120;
    }
    if (facts.kind == 'D' || facts.kind == 'N') {
        facts.args.base = 17;
        facts.args.startInstance = facts.kind == 'N' ? 5 : 0;
    } else if (facts.kind == 'I' || facts.kind == 'X') {
        facts.args.start = 23;
        facts.args.base = -7;
        facts.args.startInstance = facts.kind == 'X' ? 5 : 0;
    }
    facts.vsHash = 0xFCF7BD2896751D96ull;
    facts.psHash = 0xF786D34B5E118D5Eull;
    if (terminal == ladder::SiteId::kWitchspaceStarsSkip) {
        facts.kind = static_cast<std::uint8_t>('X');
        facts.vsHash = 0x9AEC596A2B036EA6ull;
    }
    facts.candidateMask = 1ull << 1;
    facts.vsIdentity = 1;
    facts.psIdentity = 2;
    facts.rtv0Identity = 3;
    facts.dsv0Identity = 4;
    facts.rtv0Width = 2400;
    facts.rtv0Height = 2400;
    if (facts.kind == 'A' || facts.kind == 'Z' || facts.kind == 'Y') facts.drawParametersKnown = false;
    if (facts.kind == 'Z' || facts.kind == 'Y') {
        facts.argumentBufferIdentity = static_cast<std::uintptr_t>(0xA11E);
        facts.argumentByteOffset = 128;
        facts.argumentBufferKnown = true;
    }
    const trace::Token token = trace::beginDraw(facts);
    if (!token.valid()) return false;
    auto policy = trace::makePolicy(token);
    ladder::recordAction<trace::TracePolicy, ladder::ActionId::kDrawBegin>(policy, [] {
        ladder::ActionRecord action;
        action.phase = ladder::ActionPhase::Begin;
        action.outcome = ladder::ActionOutcome::Applied;
        return action;
    });

    ModelVisitor visitor{scenario};
    CandidateInterest interest;
    ladder::Flow flow = ladder::Flow::Continue;
    if (includeCommon) {
        flow = runSequence(ladder::CommonSequence{}, visitor, interest, policy);
    }
    if (flow == ladder::Flow::Continue) {
        flow = runSequence(selectedSequence, visitor, interest, policy);
    }
    for (const auto visited : visitor.visited) {
        if (visited == ladder::SiteId::kDrawGateDisabledNone) {
            trace::PredicateFact fact{};
            fact.siteId = static_cast<std::uint16_t>(visited);
            fact.kind = trace::PredicateFactKind::DrawGateWanted;
            fact.known = trace::TriState::Yes;
            fact.gateWanted = scenario.drawGateOff ? trace::TriState::No
                                                   : trace::TriState::Yes;
            policy.predicateFact(fact);
        } else if (visited == ladder::SiteId::kEyeRangeSkip) {
            trace::PredicateFact fact{};
            fact.siteId = static_cast<std::uint16_t>(visited);
            fact.kind = trace::PredicateFactKind::EyeRangeSkip;
            fact.known = trace::TriState::Yes;
            fact.eyeDrawIndex = facts.eyeDrawIndex;
            if (terminal == ladder::SiteId::kEyeRangeSkip) {
                // Two overlapping ranges prove replay preserves the legacy
                // first-match subsite (the selector result itself is subsite 0).
                fact.rangeCount = 2;
                fact.ranges[0] = {facts.eyeDrawIndex, facts.eyeDrawIndex};
                fact.ranges[1] = {facts.eyeDrawIndex, facts.eyeDrawIndex};
                fact.censusSkippedDelta = 1;
            }
            fact.censusSkippedDeltaKnown = true;
            policy.predicateFact(fact);
        } else if (visited == ladder::SiteId::kWitchspaceStarsSkip) {
            trace::PredicateFact fact{};
            fact.siteId = static_cast<std::uint16_t>(visited);
            fact.kind = trace::PredicateFactKind::WitchspaceStarsSkip;
            fact.known = trace::TriState::Yes;
            fact.interestMaskKnown = trace::TriState::Yes;
            fact.legacyInterestMask = (1ull << draw_interest::kInterestCount) - 1ull;
            fact.starsHelperReached = trace::TriState::Yes;
            fact.hiddenKnown = trace::TriState::Yes;
            fact.hidden = terminal == ladder::SiteId::kWitchspaceStarsSkip
                ? trace::TriState::Yes : trace::TriState::No;
            if (fact.hidden == trace::TriState::Yes) {
                fact.contextKnown = trace::TriState::Yes;
                fact.contextValid = trace::TriState::Yes;
                fact.starsShapeReached = trace::TriState::Yes;
                fact.starsShapeMatched = trace::TriState::Yes;
                fact.starsHashKnown = trace::TriState::Yes;
                fact.starsHashSource = 1;
                fact.starsVsHash = facts.vsHash;
            }
            fact.starsSkippedDeltaKnown = true;
            fact.starsSkippedDelta = fact.hidden == trace::TriState::Yes ? 1 : 0;
            fact.detailsFinalized = true;
            policy.predicateFact(fact);
        } else if (visited == ladder::SiteId::kNightVisionClaim) {
            trace::PredicateFact fact{};
            fact.siteId = static_cast<std::uint16_t>(visited);
            fact.kind = trace::PredicateFactKind::NightVisionClaim;
            fact.known = trace::TriState::Yes;
            fact.dispatchEnabled = trace::TriState::Yes;
            fact.activeMaskKnown = trace::TriState::Yes;
            fact.activePluginMask = 1ull << 1;
            fact.candidateKnown = trace::TriState::Yes;
            fact.candidatePresent = trace::TriState::Yes;
            fact.modeKnown = trace::TriState::Yes;
            fact.mode = 2;
            fact.shapeReached = trace::TriState::Yes;
            if (terminal == ladder::SiteId::kNightVisionClaim) {
                fact.shapeMatched = trace::TriState::Yes;
                fact.callbackReached = trace::TriState::Yes;
                fact.callbackModeKnown = trace::TriState::Yes;
                fact.callbackMode = 2;
                fact.failedKnown = trace::TriState::Yes;
                fact.failed = trace::TriState::No;
            } else {
                // Matrix rows retain the historical invoked-site order, with
                // a draw-kind miss before the NV callback is reached.
                fact.shapeMatched = trace::TriState::No;
                fact.callbackReached = trace::TriState::No;
            }
            fact.detailsFinalized = true;
            policy.predicateFact(fact);
        } else if (visited == ladder::SiteId::kOffscreenCensusSkip) {
            trace::PredicateFact fact{};
            fact.siteId = static_cast<std::uint16_t>(visited);
            fact.kind = trace::PredicateFactKind::OffscreenCensusSkip;
            fact.known = trace::TriState::Yes;
            fact.quadArmed = trace::TriState::Unknown;
            const bool match = terminal == ladder::SiteId::kOffscreenCensusSkip;
            fact.offscreenRuleCount = match ? 2 : 1;
            fact.offscreenRules[0] = {
                static_cast<std::uint8_t>('D'), match ? facts.count : 0,
                match ? facts.rtv0Width : facts.rtv0Width + 1,
                facts.rtv0Height};
            if (match) fact.offscreenRules[1] = fact.offscreenRules[0];
            fact.offscreenProbeReached = trace::TriState::Yes;
            fact.offscreenProbeResolved = trace::TriState::Yes;
            fact.offscreenProbeTexture2D = trace::TriState::Yes;
            fact.offscreenTargetW = facts.rtv0Width;
            fact.offscreenTargetH = facts.rtv0Height;
            fact.censusSkippedDeltaKnown = true;
            fact.censusSkippedDelta = match ? 1 : 0;
            fact.detailsFinalized = true;
            policy.predicateFact(fact);
        } else if (visited == ladder::SiteId::kOffscreenQuadSkip) {
            trace::PredicateFact fact{};
            fact.siteId = static_cast<std::uint16_t>(visited);
            fact.kind = trace::PredicateFactKind::OffscreenQuadSkip;
            fact.known = trace::TriState::Yes;
            const bool match = terminal == ladder::SiteId::kOffscreenQuadSkip;
            fact.offscreenRuleCount = 1;
            fact.quadArmed = match ? trace::TriState::Yes : trace::TriState::No;
            fact.offscreenEyeDrawsLastFrame = 0;
            if (match) {
                fact.offscreenRules[0] = {
                    static_cast<std::uint8_t>('D'), facts.count,
                    facts.rtv0Width, facts.rtv0Height};
                fact.offscreenProbeReached = trace::TriState::Yes;
                fact.offscreenProbeResolved = trace::TriState::Yes;
                fact.offscreenProbeTexture2D = trace::TriState::Yes;
                fact.offscreenTargetW = facts.rtv0Width;
                fact.offscreenTargetH = facts.rtv0Height;
            } else {
                fact.offscreenProbeReached = trace::TriState::No;
            }
            fact.detailsFinalized = true;
            policy.predicateFact(fact);
        } else if (visited == ladder::SiteId::kHoloClaim) {
            policy.predicateFact(makeHoloPredicateFact(visited, facts,
                terminal == ladder::SiteId::kTargetSharpClaim));
        } else if (visited == ladder::SiteId::kScrimClaim) {
            policy.predicateFact(makeScrimPredicateFact(visited, facts,
                terminal == ladder::SiteId::kEyeBackdropComposite));
        }
    }
    const std::vector<ladder::SiteId> expected = includeCommon
        ? frozenWalkRanges(scenario, kFrozenCommon, 8, referenceTail, referenceTailCount)
        : frozenWalkRanges(scenario, referenceTail, referenceTailCount, nullptr, 0);
    const bool correct = flow == ladder::Flow::Stop && same(visitor.visited, expected) &&
        !visitor.visited.empty() && visitor.visited.back() == terminal &&
        policy.token.valid() && interest.legacyMaskLoads <= 1 &&
        interest.candidateQueries <= 1 && interest.candidateLoads <= 1;
    if (!correct) {
        std::printf("terminal fixture mismatch: terminal=%u route=%u flow=%u "
                    "visited=%zu expected=%zu last=%u maskLoads=%u candidateQueries=%u "
                    "candidateLoads=%u\n",
                    static_cast<unsigned>(terminal), static_cast<unsigned>(route),
                    static_cast<unsigned>(flow), visitor.visited.size(), expected.size(),
                    visitor.visited.empty() ? 0u :
                        static_cast<unsigned>(visitor.visited.back()),
                    interest.legacyMaskLoads, interest.candidateQueries,
                    interest.candidateLoads);
    }
    if (route == ladder::RouteId::kFlatRuntimeBypass) {
        if (kind == 'A') {
            appendTestIssue<ladder::ActionId::kAutoDraw>(policy,
                ladder::DrawCallKind::Auto, ladder::kActionGpuDrawArgsUnavailable, instances);
        } else if (kind == 'Z') {
            appendTestIssue<ladder::ActionId::kDrawIndexedInstancedIndirect>(policy,
                ladder::DrawCallKind::DrawIndexedInstancedIndirect,
                ladder::kActionGpuDrawArgsUnavailable, instances);
        } else if (kind == 'Y') {
            appendTestIssue<ladder::ActionId::kDrawInstancedIndirect>(policy,
                ladder::DrawCallKind::DrawInstancedIndirect,
                ladder::kActionGpuDrawArgsUnavailable, instances);
        } else {
            appendTestIssue<ladder::ActionId::kOriginalDraw>(policy,
                directCallKind(kind), 0, instances);
        }
    } else if (route == ladder::RouteId::kAutoBypass) {
        if (scenario.exitSubsite != 0) {
            appendTestIssue<ladder::ActionId::kAutoDraw>(policy, ladder::DrawCallKind::Auto,
                ladder::kActionGpuDrawArgsUnavailable, instances,
                ladder::ActionOutcome::Declined, 0);
        } else
        appendTestIssue<ladder::ActionId::kAutoDraw>(policy, ladder::DrawCallKind::Auto,
            ladder::kActionGpuDrawArgsUnavailable, instances);
    } else if (route == ladder::RouteId::kDrawIndexedInstancedIndirectBypass) {
        if (scenario.exitSubsite != 0) {
            appendTestIssue<ladder::ActionId::kDrawIndexedInstancedIndirect>(policy,
                ladder::DrawCallKind::DrawIndexedInstancedIndirect,
                ladder::kActionGpuDrawArgsUnavailable, instances,
                ladder::ActionOutcome::Declined, 0);
        } else
        appendTestIssue<ladder::ActionId::kDrawIndexedInstancedIndirect>(policy,
            ladder::DrawCallKind::DrawIndexedInstancedIndirect,
            ladder::kActionGpuDrawArgsUnavailable, instances);
    } else if (route == ladder::RouteId::kDrawInstancedIndirectBypass) {
        if (scenario.exitSubsite != 0) {
            appendTestIssue<ladder::ActionId::kDrawInstancedIndirect>(policy,
                ladder::DrawCallKind::DrawInstancedIndirect,
                ladder::kActionGpuDrawArgsUnavailable, instances,
                ladder::ActionOutcome::Declined, 0);
        } else
        appendTestIssue<ladder::ActionId::kDrawInstancedIndirect>(policy,
            ladder::DrawCallKind::DrawInstancedIndirect,
            ladder::kActionGpuDrawArgsUnavailable, instances);
    } else if (route == ladder::RouteId::kInternalWorldBypass) {
        appendTestIssue<ladder::ActionId::kOriginalDraw>(policy,
            directCallKind(kind), 0, instances);
    }
    if (terminal == ladder::SiteId::kIntroPanelClaim) {
        // A forwardQuadSkip-style replacement and a generated curve strip share one draw envelope.
        appendTestIssue<ladder::ActionId::kReplaceDraw>(policy,
            ladder::DrawCallKind::DrawIndexedInstanced);
        appendTestIssue<ladder::ActionId::kCurveStripDraw>(policy,
            ladder::DrawCallKind::DrawIndexedInstanced,
            ladder::kActionIssueCountUnknown |
                (malformedGenerated ? 0 : ladder::kActionGeneratedDrawArgsUnavailable), 0,
            ladder::ActionOutcome::Applied, 0);
    }
    ladder::recordAction<trace::TracePolicy, ladder::ActionId::kDrawEnd>(policy, [] {
        ladder::ActionRecord action;
        action.phase = ladder::ActionPhase::End;
        action.outcome = ladder::ActionOutcome::Applied;
        return action;
    });
    trace::finishDraw(token, static_cast<std::int16_t>(terminal), verdict);
    return correct;
}

bool traceWriterChecks(const char* rootArg) {
    namespace trace = edvr::draw_ladder_trace;
    const std::string root(rootArg);
    bool ok = true;
    const auto counterMax = (std::numeric_limits<std::uint64_t>::max)();
    ok &= check(trace::boundedCensusSkippedDelta(counterMax, counterMax) == 0 &&
                trace::boundedCensusSkippedDelta(counterMax - 1, counterMax) == 1 &&
                trace::boundedCensusSkippedDelta(counterMax, 0) == 1 &&
                trace::boundedCensusSkippedDelta(100, 102) == 2,
                "bounded census delta preserves high counts and one-step wrap");
    ok &= check(createDirectory(root), "create unique ignored-build scratch root");
    const std::string disabledDir = root + "\\disabled";
    DeleteFileA((disabledDir + "\\edvr_gfx_disabled.log").c_str());
    RemoveDirectoryA(disabledDir.c_str());
    const auto disabledWide = widen(disabledDir);
    trace::configure(false, disabledWide.c_str());
    trace::armManual();
    trace::frameBegin({10});
    ok &= check(!trace::configured() && !trace::capturing() &&
                GetFileAttributesA(disabledDir.c_str()) == INVALID_FILE_ATTRIBUTES,
                "disabled trace config/arm/frame creates no directory or file");

    const std::string validDir = root + "\\valid";
    DeleteFileA((validDir + "\\edvr_gfx_trace_fixture.draw-ladder-12.json").c_str());
    DeleteFileA((validDir + "\\edvr_gfx_trace_fixture.draw-ladder-14.json").c_str());
    ok &= check(createDirectory(validDir) &&
                createFixtureLog(validDir, "edvr_gfx_trace_fixture.log"),
                "create isolated valid trace fixture log");
    ok &= check(createNewerDecoyLog(validDir),
                "create a later decoy log to pin exact log association");
    const auto validLogPath = widen(validDir + "\\edvr_gfx_trace_fixture.log");
    trace::configure(true, validLogPath.c_str());
    ok &= check(trace::configured(), "writer config discovers isolated fixture log");
    trace::frameBegin({11});
    ok &= check(!trace::capturing(), "unarmed frame does not capture");
    const trace::Token beforeArm = trace::beginDraw({});
    ok &= check(!beforeArm.valid(), "draw before armed frame has no trace token");
    trace::frameEnd(11);
    trace::armManual();
    trace::armManual();
    trace::frameBegin({12});
    ok &= check(trace::capturing(), "manual arm starts on the next frame boundary");
    trace::frameBegin({13});
    ok &= check(trace::capturing(), "double arm does not restart or replace an active frame");

    trace::DrawFacts draw{};
    draw.eyeDrawIndex = 1;
    draw.kind = 'X';
    draw.route = ladder::RouteId::kFlatRuntimeBypass;
    draw.sequence = ladder::SequenceId::kFlatRuntimeBypass;
    draw.count = 240;
    draw.instances = 1;
    draw.vsHash = 0xFCF7BD2896751D96ull;
    draw.psHash = 0xF786D34B5E118D5Eull;
    draw.vsIdentity = 1;
    draw.psIdentity = 2;
    draw.rtv0Identity = 3;
    draw.dsv0Identity = 4;
    draw.rtv0Width = 2400;
    draw.rtv0Height = 2400;
    const trace::Token token = trace::beginDraw(draw);
    ok &= check(token.valid(), "armed owner draw receives a trace token");
    auto policy = trace::makePolicy(token);
    ladder::recordAction<trace::TracePolicy, ladder::ActionId::kDrawBegin>(policy, [] {
        ladder::ActionRecord action;
        action.phase = ladder::ActionPhase::Begin;
        action.outcome = ladder::ActionOutcome::Applied;
        return action;
    });
    FlatBypassWriter writer;
    CandidateInterest interest;
    const auto selectorFlow = ladder::visitOrdered(ladder::FlatRuntimeBypassSequence{},
                                                   writer, interest, policy);
    ok &= check(selectorFlow == ladder::Flow::Stop && interest.legacyMaskLoads == 0 &&
                interest.candidateQueries == 0 && interest.candidateLoads == 0,
                "real writer records selector-driven flat bypass without VR interest reads");
    ladder::recordAction<trace::TracePolicy, ladder::ActionId::kOriginalDraw>(policy, [&] {
        ladder::ActionRecord action;
        action.phase = ladder::ActionPhase::Issue;
        action.outcome = ladder::ActionOutcome::Applied;
        action.call = ladder::DrawCallKind::DrawIndexedInstanced;
        action.issueCount = 1;
        action.count = 240;
        action.instances = 1;
        return action;
    });
    ladder::recordAction<trace::TracePolicy, ladder::ActionId::kDrawEnd>(policy, [] {
        ladder::ActionRecord action;
        action.phase = ladder::ActionPhase::End;
        action.outcome = ladder::ActionOutcome::Applied;
        return action;
    });
    trace::finishDraw(token, static_cast<std::int16_t>(ladder::SiteId::kFlatRuntimeBypass),
                      static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::frameEnd(12);
    ok &= check(fileExists(validDir +
                "\\edvr_gfx_trace_fixture.draw-ladder-12.json"),
                "complete frame writes one sidecar after all draw hooks finish");

    // A VR draw that falls through to None has a terminal exit SiteId, just as
    // a claim winner has a terminal claim SiteId. This exercises the real
    // shared selector and the production writer schema together.
    trace::armManual();
    trace::frameBegin({14});
    trace::DrawFacts vrDraw = draw;
    vrDraw.eyeDrawIndex = 2;
    vrDraw.count = 239;  // Candidate pair is present, but NV draw shape misses.
    vrDraw.candidateMask = 1ull << 1;
    vrDraw.route = ladder::RouteId::kVrEye;
    vrDraw.sequence = ladder::SequenceId::kVrEye;
    const trace::Token vrToken = trace::beginDraw(vrDraw);
    ok &= check(vrToken.valid(), "armed VR draw receives a trace token");
    auto vrPolicy = trace::makePolicy(vrToken);
    ladder::recordAction<trace::TracePolicy, ladder::ActionId::kDrawBegin>(vrPolicy, [] {
        ladder::ActionRecord action;
        action.phase = ladder::ActionPhase::Begin;
        action.outcome = ladder::ActionOutcome::Applied;
        return action;
    });
    Scenario vrNoDistance;
    vrNoDistance.route = ladder::RouteId::kVrEye;
    vrNoDistance.exitAt = ladder::SiteId::kEyeNoDistanceNone;
    ModelVisitor vrVisitor{vrNoDistance};
    CandidateInterest vrInterest;
    // Exercise the writer/reader contract for reached optional observation
    // sites: they remain ordered trace events but their predicates stay cold.
    vrInterest.publishedInterests = 0;
    const auto commonFlow = ladder::visitOrdered(ladder::CommonSequence{}, vrVisitor,
                                                  vrInterest, vrPolicy);
    const auto eyeFlow = commonFlow == ladder::Flow::Continue
        ? ladder::visitOrdered(ladder::VrEyeSequence{}, vrVisitor, vrInterest, vrPolicy)
        : commonFlow;
    ok &= check(eyeFlow == ladder::Flow::Stop &&
                vrVisitor.visited.back() == ladder::SiteId::kEyeNoDistanceNone,
                "real VR selector reaches a distinct no-distance exit");
    ok &= check(vrInterest.legacyMaskLoads == 1 &&
                vrVisitor.handlerCalls[static_cast<std::uint16_t>(
                    ladder::SiteId::kIntroCurveObserve)] == 0 &&
                vrVisitor.handlerCalls[static_cast<std::uint16_t>(
                    ladder::SiteId::kPanelCurveObserve)] == 0,
                "real writer fixture records masked observers without running their handlers");
    for (const auto visited : vrVisitor.visited) {
        if (visited == ladder::SiteId::kDrawGateDisabledNone) {
            trace::PredicateFact fact{};
            fact.siteId = static_cast<std::uint16_t>(visited);
            fact.kind = trace::PredicateFactKind::DrawGateWanted;
            fact.known = trace::TriState::Yes;
            fact.gateWanted = trace::TriState::Yes;
            vrPolicy.predicateFact(fact);
        } else if (visited == ladder::SiteId::kEyeRangeSkip) {
            trace::PredicateFact fact{};
            fact.siteId = static_cast<std::uint16_t>(visited);
            fact.kind = trace::PredicateFactKind::EyeRangeSkip;
            fact.known = trace::TriState::Yes;
            fact.eyeDrawIndex = vrDraw.eyeDrawIndex;
            fact.censusSkippedDeltaKnown = true;
            vrPolicy.predicateFact(fact);
        } else if (visited == ladder::SiteId::kNightVisionClaim) {
            trace::PredicateFact fact{};
            fact.siteId = static_cast<std::uint16_t>(visited);
            fact.kind = trace::PredicateFactKind::NightVisionClaim;
            fact.known = trace::TriState::Yes;
            fact.dispatchEnabled = trace::TriState::Yes;
            fact.activeMaskKnown = trace::TriState::Yes;
            fact.activePluginMask = 1ull << 1;
            fact.candidateKnown = trace::TriState::Yes;
            fact.candidatePresent = trace::TriState::Yes;
            fact.modeKnown = trace::TriState::Yes;
            fact.mode = 2;
            fact.shapeReached = trace::TriState::Yes;
            fact.shapeMatched = trace::TriState::No;
            fact.callbackReached = trace::TriState::No;
            fact.detailsFinalized = true;
            vrPolicy.predicateFact(fact);
        } else if (visited == ladder::SiteId::kHoloClaim) {
            vrPolicy.predicateFact(makeHoloPredicateFact(visited, vrDraw));
        } else if (visited == ladder::SiteId::kScrimClaim) {
            vrPolicy.predicateFact(makeScrimPredicateFact(visited, vrDraw, true));
        }
    }
    // Site 6 is a reached trace event even when its interest gate prevents
    // ModelVisitor::visit. Record the fixture's cold inputs explicitly.
    trace::PredicateFact coldStars{};
    coldStars.siteId = static_cast<std::uint16_t>(ladder::SiteId::kWitchspaceStarsSkip);
    coldStars.kind = trace::PredicateFactKind::WitchspaceStarsSkip;
    coldStars.known = trace::TriState::Yes;
    coldStars.interestMaskKnown = trace::TriState::Yes;
    coldStars.legacyInterestMask = vrInterest.publishedInterests;
    coldStars.starsHelperReached = trace::TriState::No;
    coldStars.hiddenKnown = trace::TriState::Yes;
    coldStars.hidden = trace::TriState::No;
    coldStars.contextKnown = trace::TriState::Yes;
    coldStars.contextValid = trace::TriState::No;
    coldStars.detailsFinalized = true;
    vrPolicy.predicateFact(coldStars);
    ladder::recordAction<trace::TracePolicy, ladder::ActionId::kDrawEnd>(vrPolicy, [] {
        ladder::ActionRecord action;
        action.phase = ladder::ActionPhase::End;
        action.outcome = ladder::ActionOutcome::Applied;
        return action;
    });
    trace::finishDraw(vrToken,
        static_cast<std::int16_t>(ladder::SiteId::kEyeNoDistanceNone),
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::frameEnd(14);
    ok &= check(fileExists(validDir +
                "\\edvr_gfx_trace_fixture.draw-ladder-14.json"),
                "complete VR None frame writes a sidecar");

    const std::string matrixDir = root + "\\terminalmatrix";
    DeleteFileA((matrixDir + "\\edvr_gfx_terminal_matrix.draw-ladder-15.json").c_str());
    ok &= check(createDirectory(matrixDir) &&
                createFixtureLog(matrixDir, "edvr_gfx_terminal_matrix.log"),
                "create isolated terminal matrix log");
    const auto matrixLog = widen(matrixDir + "\\edvr_gfx_terminal_matrix.log");
    trace::configure(true, matrixLog.c_str());
    trace::armManual();
    trace::frameBegin({15});
    bool matrixOk = trace::capturing();
    const auto addCommon = [&](ladder::SiteId terminal, bool foreign, bool gate) {
        const auto route = foreign ? ladder::RouteId::kForeignOwner
            : gate ? ladder::RouteId::kDrawGateOff : ladder::RouteId::kCommon;
        matrixOk &= writeTerminalCase(route,
            ladder::SequenceId::kCommon,
            terminal == ladder::SiteId::kWitchspaceStarsSkip ? 'X' : 'D',
            terminal, false,
            kFrozenCommon, 8, ladder::CommonSequence{});
    };
    addCommon(ladder::SiteId::kForeignContextNone, true, false);
    addCommon(ladder::SiteId::kDrawGateDisabledNone, false, true);
    addCommon(ladder::SiteId::kFssChromeSkip, false, false);
    addCommon(ladder::SiteId::kParticleSubstitute, false, false);
    addCommon(ladder::SiteId::kWitchspaceStarsSkip, false, false);

    const ladder::SiteId offscreenTerminals[] = {
        ladder::SiteId::kOffscreenBackdropBlit,
        ladder::SiteId::kOffscreenWakePulseSkip,
        ladder::SiteId::kOffscreenCensusSkip,
        ladder::SiteId::kOffscreenLoaderPanel,
        ladder::SiteId::kOffscreenQuadSkip,
        ladder::SiteId::kOffscreenFallthroughNone,
    };
    for (const auto terminal : offscreenTerminals) {
        matrixOk &= writeTerminalCase(ladder::RouteId::kOffscreen,
            ladder::SequenceId::kOffscreen, 'D', terminal, true,
            kFrozenOffscreen, 10, ladder::OffscreenSequence{});
    }
    const ladder::SiteId eyeTerminals[] = {
        ladder::SiteId::kIntroPanelClaim,
        ladder::SiteId::kEyeCensusSkip,
        ladder::SiteId::kEyeRangeSkip,
        ladder::SiteId::kNightVisionClaim,
        ladder::SiteId::kRemlokHideSkip,
        ladder::SiteId::kRemlokScissorClaim,
        ladder::SiteId::kHoloClaim,
        ladder::SiteId::kTargetSharpClaim,
        ladder::SiteId::kScrimClaim,
        ladder::SiteId::kEyeBackdropComposite,
        ladder::SiteId::kFssPanelClaim,
        ladder::SiteId::kFssRevealClaim,
        ladder::SiteId::kFssDumpClaim,
        ladder::SiteId::kResolveBindClaim,
        ladder::SiteId::kSunglareSkip,
        ladder::SiteId::kSunglareSteadyClaim,
        ladder::SiteId::kGlareClampClaim,
        ladder::SiteId::kEyeNoDistanceNone,
        ladder::SiteId::kPanelEligibilityNone,
        ladder::SiteId::kPanelDistanceClaim,
        ladder::SiteId::kPanelTailNone,
    };
    for (const auto terminal : eyeTerminals) {
        matrixOk &= writeTerminalCase(ladder::RouteId::kVrEye,
            ladder::SequenceId::kVrEye, 'D', terminal, true,
            kFrozenEye, 31, ladder::VrEyeSequence{});
    }
    const ladder::SiteId oneSite[] = {ladder::SiteId::kFlatRuntimeBypass};
    matrixOk &= writeTerminalCase(ladder::RouteId::kFlatRuntimeBypass,
        ladder::SequenceId::kFlatRuntimeBypass, 'X', ladder::SiteId::kFlatRuntimeBypass,
        false, oneSite, 1, ladder::FlatRuntimeBypassSequence{});
    matrixOk &= writeTerminalCase(ladder::RouteId::kFlatRuntimeBypass,
        ladder::SequenceId::kFlatRuntimeBypass, 'X', ladder::SiteId::kFlatRuntimeBypass,
        false, oneSite, 1, ladder::FlatRuntimeBypassSequence{}, 0);
    matrixOk &= writeTerminalCase(ladder::RouteId::kFlatRuntimeBypass,
        ladder::SequenceId::kFlatRuntimeBypass, 'A', ladder::SiteId::kFlatRuntimeBypass,
        false, oneSite, 1, ladder::FlatRuntimeBypassSequence{});
    matrixOk &= writeTerminalCase(ladder::RouteId::kFlatRuntimeBypass,
        ladder::SequenceId::kFlatRuntimeBypass, 'Z', ladder::SiteId::kFlatRuntimeBypass,
        false, oneSite, 1, ladder::FlatRuntimeBypassSequence{});
    matrixOk &= writeTerminalCase(ladder::RouteId::kFlatRuntimeBypass,
        ladder::SequenceId::kFlatRuntimeBypass, 'Y', ladder::SiteId::kFlatRuntimeBypass,
        false, oneSite, 1, ladder::FlatRuntimeBypassSequence{});
    const ladder::SiteId autoSite[] = {ladder::SiteId::kAutoRuntimeBypass};
    matrixOk &= writeTerminalCase(ladder::RouteId::kAutoBypass,
        ladder::SequenceId::kAutoBypass, 'A', ladder::SiteId::kAutoRuntimeBypass,
        false, autoSite, 1, ladder::AutoBypassSequence{});
    matrixOk &= writeTerminalCase(ladder::RouteId::kAutoBypass,
        ladder::SequenceId::kAutoBypass, 'A', ladder::SiteId::kAutoRuntimeBypass,
        false, autoSite, 1, ladder::AutoBypassSequence{}, 1, true);
    const ladder::SiteId indexedIndirectSite[] = {
        ladder::SiteId::kDrawIndexedInstancedIndirectRuntimeBypass};
    matrixOk &= writeTerminalCase(ladder::RouteId::kDrawIndexedInstancedIndirectBypass,
        ladder::SequenceId::kDrawIndexedInstancedIndirectBypass, 'Z',
        ladder::SiteId::kDrawIndexedInstancedIndirectRuntimeBypass,
        false, indexedIndirectSite, 1,
        ladder::DrawIndexedInstancedIndirectBypassSequence{});
    matrixOk &= writeTerminalCase(ladder::RouteId::kDrawIndexedInstancedIndirectBypass,
        ladder::SequenceId::kDrawIndexedInstancedIndirectBypass, 'Z',
        ladder::SiteId::kDrawIndexedInstancedIndirectRuntimeBypass,
        false, indexedIndirectSite, 1,
        ladder::DrawIndexedInstancedIndirectBypassSequence{}, 1, true);
    const ladder::SiteId indirectSite[] = {
        ladder::SiteId::kDrawInstancedIndirectRuntimeBypass};
    matrixOk &= writeTerminalCase(ladder::RouteId::kDrawInstancedIndirectBypass,
        ladder::SequenceId::kDrawInstancedIndirectBypass, 'Y',
        ladder::SiteId::kDrawInstancedIndirectRuntimeBypass,
        false, indirectSite, 1, ladder::DrawInstancedIndirectBypassSequence{});
    matrixOk &= writeTerminalCase(ladder::RouteId::kDrawInstancedIndirectBypass,
        ladder::SequenceId::kDrawInstancedIndirectBypass, 'Y',
        ladder::SiteId::kDrawInstancedIndirectRuntimeBypass,
        false, indirectSite, 1, ladder::DrawInstancedIndirectBypassSequence{}, 1, true);
    const ladder::SiteId internalSite[] = {ladder::SiteId::kInternalWorldBypass};
    matrixOk &= writeTerminalCase(ladder::RouteId::kInternalWorldBypass,
        ladder::SequenceId::kInternalWorldBypass, 'D',
        ladder::SiteId::kInternalWorldBypass, false, internalSite, 1,
        ladder::InternalWorldBypassSequence{});
    trace::frameEnd(15);
    ok &= check(matrixOk && trace::status() == trace::Status::CompleteWritten &&
                fileExists(matrixDir +
                    "\\edvr_gfx_terminal_matrix.draw-ladder-15.json"),
                "real writer records every terminal site against frozen legacy order");

    const std::string generatedInvalidDir = root + "\\generatedinvalid";
    DeleteFileA((generatedInvalidDir + "\\edvr_gfx_generated_invalid.draw-ladder-16.json").c_str());
    ok &= check(createDirectory(generatedInvalidDir) &&
                createFixtureLog(generatedInvalidDir, "edvr_gfx_generated_invalid.log"),
                "create generated-action invalid fixture log");
    const auto generatedInvalidLog = widen(generatedInvalidDir + "\\edvr_gfx_generated_invalid.log");
    trace::configure(true, generatedInvalidLog.c_str());
    trace::armManual();
    trace::frameBegin({16});
    const bool malformedWritten = trace::capturing() && writeTerminalCase(
        ladder::RouteId::kVrEye, ladder::SequenceId::kVrEye, 'D',
        ladder::SiteId::kIntroPanelClaim, true, kFrozenEye, 31,
        ladder::VrEyeSequence{}, 1, false, true);
    trace::frameEnd(16);
    ok &= check(malformedWritten && trace::status() == trace::Status::CompleteWritten &&
                fileExists(generatedInvalidDir +
                    "\\edvr_gfx_generated_invalid.draw-ladder-16.json"),
                "real writer emits generated helper with missing unknown-args flag for reader rejection");

    const std::string invalidTokenDir = root + "\\invalidtoken";
    const bool invalidTokenStarted = beginCapture(invalidTokenDir,
        "edvr_gfx_invalid_token.log", 24);
    const trace::Token validToken = trace::beginDraw(draw);
    trace::appendSite({}, 72, 3, 4, 1, 0,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::appendSite(token, 72, 3, 4, 1, 0,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    ladder::ActionRecord invalidAction{};
    trace::appendAction({}, static_cast<std::uint16_t>(ladder::ActionId::kOriginalDraw), invalidAction);
    trace::updateCandidates({}, UINT64_MAX);
    trace::updateRoute({}, ladder::RouteId::kFlatRuntimeBypass,
                       ladder::SequenceId::kFlatRuntimeBypass);
    trace::finishDraw({}, 72, static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::finishDraw(validToken, 72,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::frameEnd(24);
    ok &= check(invalidTokenStarted && validToken.valid() && trace::overflowed() &&
                trace::status() == trace::Status::InvalidCapture,
                "active capture rejects an invalid token and marks the frame invalid");

    const std::string finalizedDir = root + "\\postfinalized";
    const bool finalizedStarted = beginCapture(finalizedDir,
        "edvr_gfx_postfinalized.log", 25);
    trace::DrawFacts finalizedFacts = draw;
    finalizedFacts.route = ladder::RouteId::kFlatRuntimeBypass;
    finalizedFacts.sequence = ladder::SequenceId::kFlatRuntimeBypass;
    const trace::Token finalizedToken = trace::beginDraw(finalizedFacts);
    trace::appendSite(finalizedToken, 72, 3, 4, 1, 0,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::finishDraw(finalizedToken, 72,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::appendAction(finalizedToken,
        static_cast<std::uint16_t>(ladder::ActionId::kOriginalDraw), invalidAction);
    trace::updateCandidates(finalizedToken, UINT64_MAX);
    trace::updateRoute(finalizedToken, ladder::RouteId::kCommon,
                       ladder::SequenceId::kCommon);
    trace::finishDraw(finalizedToken, 72,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::frameEnd(25);
    ok &= check(finalizedStarted && finalizedToken.valid() && trace::overflowed() &&
                trace::status() == trace::Status::InvalidCapture,
                "post-finalize append/update/double-finish invalidate the frame");

    const std::string duplicateFactsDir = root + "\\duplicatefacts";
    const bool duplicateFactsStarted = beginCapture(duplicateFactsDir,
        "edvr_gfx_duplicate_facts.log", 26);
    const trace::Token duplicateFactsToken = trace::beginDraw(finalizedFacts);
    trace::ForwardFacts forwardFacts{};
    trace::recordForwardFacts(duplicateFactsToken, forwardFacts);
    trace::recordForwardFacts(duplicateFactsToken, forwardFacts);
    trace::appendSite(duplicateFactsToken, 72, 3, 4, 1, 0,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::finishDraw(duplicateFactsToken, 72,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::frameEnd(26);
    ok &= check(duplicateFactsStarted && duplicateFactsToken.valid() && trace::overflowed() &&
                trace::status() == trace::Status::InvalidCapture,
                "duplicate ForwardFacts append invalidates the capture");

    const std::string duplicatePredicateDir = root + "\\duplicatepredicatefacts";
    const bool duplicatePredicateStarted = beginCapture(duplicatePredicateDir,
        "edvr_gfx_duplicate_predicate.log", 28);
    const trace::Token duplicatePredicateToken = trace::beginDraw(finalizedFacts);
    trace::PredicateFact predicateFact{};
    predicateFact.siteId = static_cast<std::uint16_t>(ladder::SiteId::kDrawGateDisabledNone);
    predicateFact.kind = trace::PredicateFactKind::DrawGateWanted;
    predicateFact.known = trace::TriState::Yes;
    predicateFact.gateWanted = trace::TriState::Yes;
    trace::appendPredicateFact(duplicatePredicateToken, predicateFact);
    trace::appendPredicateFact(duplicatePredicateToken, predicateFact);
    trace::appendSite(duplicatePredicateToken, 72, 3, 4, 1, 0,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::finishDraw(duplicatePredicateToken, 72,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::frameEnd(28);
    ok &= check(duplicatePredicateStarted && duplicatePredicateToken.valid() &&
                trace::overflowed() && trace::status() == trace::Status::InvalidCapture,
                "duplicate predicate fact invalidates the capture");

    const std::string invalidDeltaDir = root + "\\invalidcounterdelta";
    const bool invalidDeltaStarted = beginCapture(invalidDeltaDir,
        "edvr_gfx_invalid_counter_delta.log", 30);
    const trace::Token invalidDeltaToken = trace::beginDraw(draw);
    trace::PredicateFact invalidDeltaFact{};
    invalidDeltaFact.siteId = static_cast<std::uint16_t>(ladder::SiteId::kEyeRangeSkip);
    invalidDeltaFact.kind = trace::PredicateFactKind::EyeRangeSkip;
    invalidDeltaFact.known = trace::TriState::Yes;
    invalidDeltaFact.rangeCount = 1;
    invalidDeltaFact.eyeDrawIndex = 1;
    invalidDeltaFact.ranges[0] = {1, 1};
    invalidDeltaFact.censusSkippedDeltaKnown = true;
    invalidDeltaFact.censusSkippedDelta = trace::boundedCensusSkippedDelta(100, 102);
    trace::appendPredicateFact(invalidDeltaToken, invalidDeltaFact);
    const bool oversizedDeltaRejected = trace::overflowed();
    trace::appendSite(invalidDeltaToken, 72, 3, 4, 1, 0,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::finishDraw(invalidDeltaToken, 72,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::frameEnd(30);
    ok &= check(invalidDeltaStarted && invalidDeltaToken.valid() &&
                oversizedDeltaRejected && trace::status() == trace::Status::InvalidCapture,
                "out-of-range census delta is rejected before serialization");

    const std::string missingPredicateDir = root + "\\missingpredicatefacts";
    const bool missingPredicateStarted = beginCapture(missingPredicateDir,
        "edvr_gfx_missing_predicate.log", 29);
    trace::DrawFacts missingPredicateDraw = draw;
    missingPredicateDraw.route = ladder::RouteId::kCommon;
    missingPredicateDraw.sequence = ladder::SequenceId::kCommon;
    const trace::Token missingPredicateToken = trace::beginDraw(missingPredicateDraw);
    trace::appendSite(missingPredicateToken,
        static_cast<std::uint16_t>(ladder::SiteId::kDrawGateDisabledNone), 3,
        1, 0, 0, -1);
    trace::finishDraw(missingPredicateToken,
        static_cast<std::int16_t>(ladder::SiteId::kDrawGateDisabledNone), -1);
    trace::frameEnd(29);
    ok &= check(missingPredicateStarted && missingPredicateToken.valid() &&
                trace::overflowed() && trace::status() == trace::Status::InvalidCapture,
                "missing visited predicate fact invalidates the capture");

    const std::string missingNightVisionDir = root + "\\missingnightvisionfact";
    const bool missingNightVisionStarted = beginCapture(missingNightVisionDir,
        "edvr_gfx_missing_night_vision_fact.log", 31);
    trace::DrawFacts missingNightVisionDraw = draw;
    missingNightVisionDraw.route = ladder::RouteId::kVrEye;
    missingNightVisionDraw.sequence = ladder::SequenceId::kVrEye;
    const trace::Token missingNightVisionToken = trace::beginDraw(missingNightVisionDraw);
    trace::appendSite(missingNightVisionToken,
        static_cast<std::uint16_t>(ladder::SiteId::kNightVisionClaim), 2,
        5, 0, 0, -1);
    trace::finishDraw(missingNightVisionToken,
        static_cast<std::int16_t>(ladder::SiteId::kNightVisionClaim), -1);
    trace::frameEnd(31);
    ok &= check(missingNightVisionStarted && missingNightVisionToken.valid() &&
                trace::overflowed() && trace::status() == trace::Status::InvalidCapture,
                "missing reached NightVisionClaim fact invalidates the capture");

    const std::string missingOffscreenDir = root + "\\missingoffscreenfacts";
    const bool missingOffscreenStarted = beginCapture(missingOffscreenDir,
        "edvr_gfx_missing_offscreen_facts.log", 34);
    trace::DrawFacts missingOffscreenDraw = draw;
    missingOffscreenDraw.route = ladder::RouteId::kOffscreen;
    missingOffscreenDraw.sequence = ladder::SequenceId::kOffscreen;
    const trace::Token missingCensusToken = trace::beginDraw(missingOffscreenDraw);
    trace::appendSite(missingCensusToken,
        static_cast<std::uint16_t>(ladder::SiteId::kOffscreenCensusSkip),
        static_cast<std::uint8_t>(ladder::SiteKind::Exit),
        static_cast<std::uint8_t>(ladder::SiteOutcome::Declined),
        static_cast<std::uint8_t>(ladder::Flow::Continue), 0, -1);
    trace::finishDraw(missingCensusToken,
        static_cast<std::int16_t>(ladder::SiteId::kOffscreenCensusSkip), -1);
    const trace::Token missingQuadToken = trace::beginDraw(missingOffscreenDraw);
    trace::appendSite(missingQuadToken,
        static_cast<std::uint16_t>(ladder::SiteId::kOffscreenQuadSkip),
        static_cast<std::uint8_t>(ladder::SiteKind::Claim),
        static_cast<std::uint8_t>(ladder::SiteOutcome::Declined),
        static_cast<std::uint8_t>(ladder::Flow::Continue), 0, -1);
    trace::finishDraw(missingQuadToken,
        static_cast<std::int16_t>(ladder::SiteId::kOffscreenQuadSkip), -1);
    trace::frameEnd(34);
    ok &= check(missingOffscreenStarted && missingCensusToken.valid() &&
                missingQuadToken.valid() && trace::overflowed() &&
                trace::status() == trace::Status::InvalidCapture,
                "missing site 24 or 26 source fact invalidates the capture");

    const auto checkHoloScrimFactFailure = [&](const char* directoryLeaf,
        const char* logLeaf, std::uint32_t frameNo, ladder::SiteId site,
        bool appendUnfinished) {
        const std::string directory = root + "\\" + directoryLeaf;
        const bool started = beginCapture(directory, logLeaf, frameNo);
        trace::DrawFacts drawFacts = draw;
        drawFacts.route = ladder::RouteId::kVrEye;
        drawFacts.sequence = ladder::SequenceId::kVrEye;
        const trace::Token token = trace::beginDraw(drawFacts);
        if (appendUnfinished) {
            trace::PredicateFact fact{};
            fact.siteId = static_cast<std::uint16_t>(site);
            fact.kind = site == ladder::SiteId::kHoloClaim
                ? trace::PredicateFactKind::Holo53
                : trace::PredicateFactKind::Scrim55;
            fact.known = trace::TriState::Yes;
            trace::appendPredicateFact(token, fact);
        }
        trace::appendSite(token, static_cast<std::uint16_t>(site),
            static_cast<std::uint8_t>(ladder::SiteKind::Claim),
            static_cast<std::uint8_t>(ladder::SiteOutcome::Declined),
            static_cast<std::uint8_t>(ladder::Flow::Continue), 0, -1);
        trace::finishDraw(token, static_cast<std::int16_t>(site), -1);
        trace::frameEnd(frameNo);
        std::string sidecarLeaf(logLeaf);
        const std::size_t extension = sidecarLeaf.rfind(".log");
        if (extension != std::string::npos) sidecarLeaf.resize(extension);
        sidecarLeaf += ".draw-ladder-" + std::to_string(frameNo) + ".json";
        return started && token.valid() && trace::overflowed() &&
               trace::status() == trace::Status::InvalidCapture &&
               fileExists(directory + "\\" + sidecarLeaf);
    };
    ok &= check(checkHoloScrimFactFailure("missingholo53fact",
                "edvr_gfx_missing_holo53_fact.log", 41,
                ladder::SiteId::kHoloClaim, false),
                "missing reached Holo53 source fact invalidates capture");
    ok &= check(checkHoloScrimFactFailure("missingscrim55fact",
                "edvr_gfx_missing_scrim55_fact.log", 42,
                ladder::SiteId::kScrimClaim, false),
                "missing reached Scrim55 source fact invalidates capture");
    ok &= check(checkHoloScrimFactFailure("unfinishedholo53fact",
                "edvr_gfx_unfinished_holo53_fact.log", 43,
                ladder::SiteId::kHoloClaim, true),
                "unfinished Holo53 source fact invalidates capture");
    ok &= check(checkHoloScrimFactFailure("unfinishedscrim55fact",
                "edvr_gfx_unfinished_scrim55_fact.log", 44,
                ladder::SiteId::kScrimClaim, true),
                "unfinished Scrim55 source fact invalidates capture");

    const std::string wrongHoloKindDir = root + "\\wrongholo53kind";
    const bool wrongHoloKindStarted = beginCapture(wrongHoloKindDir,
        "edvr_gfx_wrong_holo53_kind.log", 45);
    const trace::Token wrongHoloKindToken = trace::beginDraw(draw);
    trace::PredicateFact wrongHoloKind = makeHoloPredicateFact(
        ladder::SiteId::kHoloClaim, draw);
    wrongHoloKind.siteId = static_cast<std::uint16_t>(ladder::SiteId::kScrimClaim);
    trace::appendPredicateFact(wrongHoloKindToken, wrongHoloKind);
    const bool wrongHoloKindRejected = trace::overflowed();
    trace::appendSite(wrongHoloKindToken,
        static_cast<std::uint16_t>(ladder::SiteId::kScrimClaim),
        static_cast<std::uint8_t>(ladder::SiteKind::Claim),
        static_cast<std::uint8_t>(ladder::SiteOutcome::Declined),
        static_cast<std::uint8_t>(ladder::Flow::Continue), 0, -1);
    trace::finishDraw(wrongHoloKindToken,
        static_cast<std::int16_t>(ladder::SiteId::kScrimClaim), -1);
    trace::frameEnd(45);
    ok &= check(wrongHoloKindStarted && wrongHoloKindToken.valid() &&
                wrongHoloKindRejected && trace::status() == trace::Status::InvalidCapture,
                "Holo53 fact on site 55 is rejected");

    const std::string duplicateHoloDir = root + "\\duplicateholo53fact";
    const bool duplicateHoloStarted = beginCapture(duplicateHoloDir,
        "edvr_gfx_duplicate_holo53_fact.log", 46);
    trace::DrawFacts duplicateHoloDraw = draw;
    duplicateHoloDraw.kind = static_cast<std::uint8_t>('D');
    duplicateHoloDraw.count = 240;
    const trace::Token duplicateHoloToken = trace::beginDraw(duplicateHoloDraw);
    const trace::PredicateFact duplicateHoloFact = makeHoloPredicateFact(
        ladder::SiteId::kHoloClaim, duplicateHoloDraw);
    trace::appendPredicateFact(duplicateHoloToken, duplicateHoloFact);
    trace::appendPredicateFact(duplicateHoloToken, duplicateHoloFact);
    trace::appendSite(duplicateHoloToken,
        static_cast<std::uint16_t>(ladder::SiteId::kHoloClaim),
        static_cast<std::uint8_t>(ladder::SiteKind::Claim),
        static_cast<std::uint8_t>(ladder::SiteOutcome::Declined),
        static_cast<std::uint8_t>(ladder::Flow::Continue), 0, -1);
    trace::finishDraw(duplicateHoloToken,
        static_cast<std::int16_t>(ladder::SiteId::kHoloClaim), -1);
    trace::frameEnd(46);
    ok &= check(duplicateHoloStarted && duplicateHoloToken.valid() &&
                trace::overflowed() && trace::status() == trace::Status::InvalidCapture,
                "duplicate Holo53 facts invalidate capture");

    const std::string factCapDir = root + "\\factcapoverflow";
    const bool factCapStarted = beginCapture(factCapDir,
        "edvr_gfx_fact_cap_overflow.log", 47);
    trace::DrawFacts factCapDraw = draw;
    factCapDraw.kind = static_cast<std::uint8_t>('D');
    factCapDraw.count = 240;
    factCapDraw.instances = 1;
    const trace::Token factCapToken = trace::beginDraw(factCapDraw);
    auto appendCapSite = [&](std::uint16_t site) {
        trace::appendSite(factCapToken, site,
            static_cast<std::uint8_t>(ladder::SiteKind::Claim),
            static_cast<std::uint8_t>(ladder::SiteOutcome::Declined),
            static_cast<std::uint8_t>(ladder::Flow::Continue), 0, -1);
    };
    trace::PredicateFact capGate{};
    capGate.siteId = static_cast<std::uint16_t>(ladder::SiteId::kDrawGateDisabledNone);
    capGate.kind = trace::PredicateFactKind::DrawGateWanted;
    capGate.known = trace::TriState::Yes;
    capGate.gateWanted = trace::TriState::Yes;
    trace::appendPredicateFact(factCapToken, capGate);
    appendCapSite(capGate.siteId);

    trace::PredicateFact capRange{};
    capRange.siteId = static_cast<std::uint16_t>(ladder::SiteId::kEyeRangeSkip);
    capRange.kind = trace::PredicateFactKind::EyeRangeSkip;
    capRange.known = trace::TriState::Yes;
    capRange.eyeDrawIndex = 1;
    capRange.censusSkippedDeltaKnown = true;
    trace::appendPredicateFact(factCapToken, capRange);
    appendCapSite(capRange.siteId);

    trace::PredicateFact capNv{};
    capNv.siteId = static_cast<std::uint16_t>(ladder::SiteId::kNightVisionClaim);
    capNv.kind = trace::PredicateFactKind::NightVisionClaim;
    capNv.known = trace::TriState::Yes;
    capNv.dispatchEnabled = trace::TriState::No;
    capNv.shapeReached = trace::TriState::No;
    capNv.callbackReached = trace::TriState::No;
    capNv.detailsFinalized = true;
    trace::appendPredicateFact(factCapToken, capNv);
    appendCapSite(capNv.siteId);

    trace::PredicateFact capStars{};
    capStars.siteId = static_cast<std::uint16_t>(ladder::SiteId::kWitchspaceStarsSkip);
    capStars.kind = trace::PredicateFactKind::WitchspaceStarsSkip;
    capStars.known = trace::TriState::Yes;
    capStars.interestMaskKnown = trace::TriState::Yes;
    capStars.legacyInterestMask = 1ull << 1;
    capStars.starsHelperReached = trace::TriState::Yes;
    capStars.hiddenKnown = trace::TriState::Yes;
    capStars.hidden = trace::TriState::No;
    capStars.starsSkippedDeltaKnown = true;
    capStars.detailsFinalized = true;
    trace::appendPredicateFact(factCapToken, capStars);
    appendCapSite(capStars.siteId);

    trace::appendPredicateFact(factCapToken,
        makeHoloPredicateFact(ladder::SiteId::kHoloClaim, factCapDraw));
    appendCapSite(static_cast<std::uint16_t>(ladder::SiteId::kHoloClaim));
    trace::appendPredicateFact(factCapToken,
        makeScrimPredicateFact(ladder::SiteId::kScrimClaim, factCapDraw));
    appendCapSite(static_cast<std::uint16_t>(ladder::SiteId::kScrimClaim));
    const bool sixFactsFit = !trace::overflowed();

    trace::PredicateFact capSeventh{};
    capSeventh.siteId = static_cast<std::uint16_t>(ladder::SiteId::kOffscreenQuadSkip);
    capSeventh.kind = trace::PredicateFactKind::OffscreenQuadSkip;
    capSeventh.known = trace::TriState::Yes;
    capSeventh.offscreenRuleCount = 1;
    capSeventh.quadArmed = trace::TriState::No;
    capSeventh.offscreenProbeReached = trace::TriState::No;
    capSeventh.detailsFinalized = true;
    trace::appendPredicateFact(factCapToken, capSeventh);
    appendCapSite(capSeventh.siteId);
    trace::finishDraw(factCapToken,
        static_cast<std::int16_t>(ladder::SiteId::kOffscreenQuadSkip), -1);
    trace::frameEnd(47);
    ok &= check(factCapStarted && factCapToken.valid() && sixFactsFit &&
                trace::overflowed() && trace::status() == trace::Status::InvalidCapture,
                "six predicate facts fit and the seventh invalidates capture");

    const std::string unfinishedOffscreenDir = root + "\\unfinishedoffscreenfact";
    const bool unfinishedOffscreenStarted = beginCapture(unfinishedOffscreenDir,
        "edvr_gfx_unfinished_offscreen_fact.log", 35);
    const trace::Token unfinishedOffscreenToken = trace::beginDraw(missingOffscreenDraw);
    trace::PredicateFact unfinishedOffscreenFact{};
    unfinishedOffscreenFact.siteId = static_cast<std::uint16_t>(
        ladder::SiteId::kOffscreenQuadSkip);
    unfinishedOffscreenFact.kind = trace::PredicateFactKind::OffscreenQuadSkip;
    unfinishedOffscreenFact.known = trace::TriState::Yes;
    unfinishedOffscreenFact.offscreenRuleCount = 1;
    unfinishedOffscreenFact.quadArmed = trace::TriState::No;
    unfinishedOffscreenFact.offscreenProbeReached = trace::TriState::No;
    trace::appendPredicateFact(unfinishedOffscreenToken, unfinishedOffscreenFact);
    trace::appendSite(unfinishedOffscreenToken,
        static_cast<std::uint16_t>(ladder::SiteId::kOffscreenQuadSkip),
        static_cast<std::uint8_t>(ladder::SiteKind::Claim),
        static_cast<std::uint8_t>(ladder::SiteOutcome::Declined),
        static_cast<std::uint8_t>(ladder::Flow::Continue), 0, -1);
    trace::finishDraw(unfinishedOffscreenToken,
        static_cast<std::int16_t>(ladder::SiteId::kOffscreenQuadSkip), -1);
    trace::frameEnd(35);
    ok &= check(unfinishedOffscreenStarted && unfinishedOffscreenToken.valid() &&
                trace::overflowed() && trace::status() == trace::Status::InvalidCapture,
                "unfinished site 26 source fact invalidates the capture");

    const std::string unfinishedStarsDir = root + "\\unfinishedwitchspacestars";
    const bool unfinishedStarsStarted = beginCapture(unfinishedStarsDir,
        "edvr_gfx_unfinished_witchspace_stars.log", 32);
    trace::DrawFacts unfinishedStarsDraw = draw;
    unfinishedStarsDraw.route = ladder::RouteId::kCommon;
    unfinishedStarsDraw.sequence = ladder::SequenceId::kCommon;
    const trace::Token unfinishedStarsToken = trace::beginDraw(unfinishedStarsDraw);
    trace::PredicateFact unfinishedStarsFact{};
    unfinishedStarsFact.siteId = static_cast<std::uint16_t>(
        ladder::SiteId::kWitchspaceStarsSkip);
    unfinishedStarsFact.kind = trace::PredicateFactKind::WitchspaceStarsSkip;
    unfinishedStarsFact.known = trace::TriState::Yes;
    unfinishedStarsFact.interestMaskKnown = trace::TriState::Yes;
    unfinishedStarsFact.legacyInterestMask = 1ull << 1;
    trace::appendPredicateFact(unfinishedStarsToken, unfinishedStarsFact);
    trace::appendSite(unfinishedStarsToken,
        static_cast<std::uint16_t>(ladder::SiteId::kWitchspaceStarsSkip), 2,
        2, 0, 0, -1);
    trace::finishDraw(unfinishedStarsToken,
        static_cast<std::int16_t>(ladder::SiteId::kWitchspaceStarsSkip), -1);
    trace::frameEnd(32);
    ok &= check(unfinishedStarsStarted && unfinishedStarsToken.valid() &&
                trace::overflowed() && trace::status() == trace::Status::InvalidCapture,
                "unfinished reached WitchspaceStars fact invalidates the capture");

    const std::string reloadInvalidDir = root + "\\reloadinvalid";
    const bool reloadInvalidStarted = beginCapture(reloadInvalidDir,
        "edvr_gfx_reload_invalid.log", 27);
    trace::DrawFacts reloadFacts = finalizedFacts;
    reloadFacts.route = ladder::RouteId::kFlatRuntimeBypass;
    reloadFacts.sequence = ladder::SequenceId::kFlatRuntimeBypass;
    const trace::Token reloadToken = trace::beginDraw(reloadFacts);
    trace::appendSite(reloadToken, 72, 3, 4, 1, 0,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::invalidateActiveCapture();
    trace::finishDraw(reloadToken, 72,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    trace::frameEnd(27);
    ok &= check(reloadInvalidStarted && reloadToken.valid() && trace::overflowed() &&
                trace::status() == trace::Status::InvalidCapture,
                "config reload invalidates the active capture before frame end");

    const auto makeOverflowCapture = [&](const char* child, const char* logLeaf,
                                         std::uint32_t frameNo) {
        const std::string directory = root + "\\" + child;
        DeleteFileA((directory + "\\" + std::string(logLeaf).substr(0,
            std::strlen(logLeaf) - 4) + ".draw-ladder-" + std::to_string(frameNo) + ".json").c_str());
        if (!beginCapture(directory, logLeaf, frameNo)) return trace::Token{};
        return trace::beginDraw(draw);
    };

    const trace::Token siteToken = makeOverflowCapture("siteoverflow", "edvr_gfx_site.log", 20);
    ok &= check(siteToken.valid(), "site-overflow fixture starts");
    for (std::uint16_t i = 0; i < trace::kMaxSiteEventsPerDraw + 1; ++i) {
        trace::appendSite(siteToken, 72, 3, 1, 0, i, -1);
    }
    trace::finishDraw(siteToken, 72,
                      static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    ok &= check(trace::overflowed(), "site event overflow invalidates the whole capture");
    trace::frameEnd(20);

    const trace::Token actionToken = makeOverflowCapture("actionoverflow", "edvr_gfx_action.log", 21);
    ok &= check(actionToken.valid(), "action-overflow fixture starts");
    trace::appendSite(actionToken, 72, 3, 4, 1, 0, -1);
    ladder::ActionRecord action{};
    for (std::uint16_t i = 0; i < trace::kMaxActionEventsPerDraw + 1; ++i) {
        trace::appendAction(actionToken, static_cast<std::uint16_t>(ladder::ActionId::kOriginalDraw), action);
    }
    trace::finishDraw(actionToken, 72,
                      static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    ok &= check(trace::overflowed(), "action event overflow invalidates the whole capture");
    trace::frameEnd(21);

    const trace::Token unfinishedToken = makeOverflowCapture("unfinished", "edvr_gfx_unfinished.log", 22);
    ok &= check(unfinishedToken.valid(), "unfinished-draw fixture starts");
    trace::frameEnd(22);
    ok &= check(trace::overflowed(), "frame end rejects unfinished draws");

    const trace::Token drawOverflowToken = makeOverflowCapture("drawoverflow", "edvr_gfx_draws.log", 23);
    ok &= check(drawOverflowToken.valid(), "draw-overflow fixture starts");
    for (std::uint32_t i = 1; i < trace::kMaxDraws; ++i) {
        (void)trace::beginDraw(draw);
    }
    const trace::Token overflowToken = trace::beginDraw(draw);
    ok &= check(!overflowToken.valid() && trace::overflowed(),
                "draw capacity overflow invalidates capture without writing a prefix");
    trace::configure(false, nullptr);
    ok &= check(!fileExists(root + "\\drawoverflow\\edvr_gfx_draws.draw-ladder-23.json"),
                "draw overflow writes no incomplete sidecar prefix");

    const std::string shutdownDir = root + "\\shutdown";
    ok &= check(beginCapture(shutdownDir, "edvr_gfx_shutdown.log", 28),
                "shutdown fixture starts a manual capture");
    trace::DrawFacts shutdownFacts = draw;
    shutdownFacts.eyeDrawIndex = 1;
    const trace::Token shutdownToken = trace::beginDraw(shutdownFacts);
    trace::appendSite(shutdownToken, 72, 3, 4, 1, 0,
        static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone));
    const trace::ShutdownResult partialShutdown = trace::shutdown();
    const std::string shutdownSidecar = shutdownDir + "\\edvr_gfx_shutdown.draw-ladder-28.json";
    ok &= check(!shutdownToken.valid() && partialShutdown.previousStatus == trace::Status::Capturing &&
                partialShutdown.discardedCapture && !partialShutdown.discardedPendingArm &&
                !trace::configured() && !trace::capturing() && trace::status() == trace::Status::Disabled &&
                !fileExists(shutdownSidecar),
                "cold shutdown discards an unfinished capture without serializing a sidecar");
    const trace::ShutdownResult repeatedShutdown = trace::shutdown();
    ok &= check(repeatedShutdown.previousStatus == trace::Status::Disabled &&
                !repeatedShutdown.discardedCapture && !repeatedShutdown.discardedPendingArm &&
                trace::status() == trace::Status::Disabled,
                "repeated cold shutdown is idempotent and reports no discarded state");

    const std::string armedShutdownDir = root + "\\armedshutdown";
    ok &= check(createDirectory(armedShutdownDir) &&
                createFixtureLog(armedShutdownDir, "edvr_gfx_armed_shutdown.log"),
                "create armed-only shutdown fixture log");
    const auto armedShutdownPath = widen(armedShutdownDir + "\\edvr_gfx_armed_shutdown.log");
    trace::configure(true, armedShutdownPath.c_str());
    trace::armManual();
    const trace::ShutdownResult armedShutdown = trace::shutdown();
    ok &= check(armedShutdown.previousStatus == trace::Status::Armed &&
                armedShutdown.discardedPendingArm && !armedShutdown.discardedCapture &&
                !trace::configured() && trace::status() == trace::Status::Disabled &&
                !fileExists(armedShutdownDir + "\\edvr_gfx_armed_shutdown.draw-ladder-29.json"),
                "cold shutdown clears a pending manual arm without creating a capture");
    return ok;
}

int selfTest(const char* traceDir) {
    bool ok = true;
    ok &= interestMaskChecks();
    ok &= typedInterestChecks();
    ok &= cpuPolicyChecks();
    ok &= productionRouteOrderCheck();
    static_assert(ladder::CommonSequence::size == sizeof(kFrozenCommon) / sizeof(kFrozenCommon[0]));
    static_assert(ladder::OffscreenSequence::size == sizeof(kFrozenOffscreen) / sizeof(kFrozenOffscreen[0]));
    static_assert(ladder::EyeSequence::size == sizeof(kFrozenEye) / sizeof(kFrozenEye[0]));
    static_assert(ladder::SiteId::kForeignContextNone != ladder::SiteId::kOffscreenFallthroughNone);
    static_assert(ladder::SiteId::kOffscreenFallthroughNone != ladder::SiteId::kEyeNoDistanceNone);
static_assert(ladder::SiteId::kOffscreenWakePulseSkip != ladder::SiteId::kEyeCensusSkip);
static_assert(ladder::SiteId::kOffscreenBackdropBlit != ladder::SiteId::kEyeBackdropComposite);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone) == 0);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kPanel) == 1);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kSkip) == 2);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kRemlok) == 3);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kHolo) == 4);
static_assert(trace::kMaxPredicateFactsPerDraw == 6);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kTarget) == 5);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kNightVision) == 6);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kIntro) == 7);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kGlareClamp) == 8);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kSteady) == 9);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kParticle) == 10);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kFssPanel) == 11);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kReveal) == 12);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kDump) == 13);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kResolve) == 14);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kQuadSkip) == 15);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kLoader) == 16);
static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kScrim) == 17);
    static_assert(static_cast<std::int16_t>(ladder::VerdictOrdinal::kBackdrop) == 18);
    static_assert(static_cast<std::uint16_t>(ladder::SiteId::kAutoRuntimeBypass) == 73);
    static_assert(static_cast<std::uint16_t>(ladder::SiteId::kDrawIndexedInstancedIndirectRuntimeBypass) == 74);
    static_assert(static_cast<std::uint16_t>(ladder::SiteId::kDrawInstancedIndirectRuntimeBypass) == 75);
    static_assert(static_cast<std::uint16_t>(ladder::SiteId::kInternalWorldBypass) == 76);
    static_assert(static_cast<std::uint8_t>(ladder::RouteId::kAutoBypass) == 8);
    static_assert(static_cast<std::uint8_t>(ladder::RouteId::kDrawIndexedInstancedIndirectBypass) == 9);
    static_assert(static_cast<std::uint8_t>(ladder::RouteId::kDrawInstancedIndirectBypass) == 10);
    static_assert(static_cast<std::uint8_t>(ladder::RouteId::kInternalWorldBypass) == 11);
    static_assert(static_cast<std::uint8_t>(ladder::SequenceId::kAutoBypass) == 6);
    static_assert(static_cast<std::uint8_t>(ladder::SequenceId::kDrawIndexedInstancedIndirectBypass) == 7);
    static_assert(static_cast<std::uint8_t>(ladder::SequenceId::kDrawInstancedIndirectBypass) == 8);
    static_assert(static_cast<std::uint8_t>(ladder::SequenceId::kInternalWorldBypass) == 9);
    static_assert(static_cast<std::uint16_t>(ladder::ActionId::kAutoDraw) == 18);
    static_assert(static_cast<std::uint16_t>(ladder::ActionId::kDrawIndexedInstancedIndirect) == 19);
    static_assert(static_cast<std::uint16_t>(ladder::ActionId::kDrawInstancedIndirect) == 20);
    static_assert(ladder::kActionIssueCountUnknown == 0x8000);
    static_assert(ladder::kActionGpuDrawArgsUnavailable == 0x4000);
    static_assert(static_cast<std::uint8_t>(ladder::DrawCallKind::Auto) == 5);
    static_assert(static_cast<std::uint8_t>(ladder::DrawCallKind::DrawIndexedInstancedIndirect) == 6);
    static_assert(static_cast<std::uint8_t>(ladder::DrawCallKind::DrawInstancedIndirect) == 7);

    CandidateInterest allCandidates;
    TraceCapture trace;
    Scenario foreign;
    foreign.foreignOwner = true;
    ModelVisitor foreignVisitor{foreign};
    const auto foreignFlow = runSequence(ladder::CommonSequence{}, foreignVisitor,
                                         allCandidates, trace);
    ok &= check(foreignFlow == ladder::Flow::Stop, "foreign owner terminates common route");
    ok &= check(same(foreignVisitor.visited, frozenWalk(foreign, kFrozenCommon, kFrozenEye)),
                "foreign exit preserves frozen prelude order");
    ok &= check(foreignVisitor.visited.size() == 2 &&
                foreignVisitor.visited.back() == ladder::SiteId::kForeignContextNone,
                "foreign owner reaches no later claim");

    Scenario gateOff;
    gateOff.drawGateOff = true;
    ModelVisitor gateVisitor{gateOff};
    TraceCapture gateTrace;
    const auto gateFlow = runSequence(ladder::CommonSequence{}, gateVisitor,
                                      allCandidates, gateTrace);
    ok &= check(gateFlow == ladder::Flow::Stop && gateVisitor.visited.size() == 3 &&
                gateVisitor.visited.back() == ladder::SiteId::kDrawGateDisabledNone,
                "draw gate exits before route predicates");

    Scenario offscreenBackdrop;
    offscreenBackdrop.route = ladder::RouteId::kOffscreen;
    offscreenBackdrop.claimAt = ladder::SiteId::kOffscreenBackdropBlit;
    offscreenBackdrop.verdict = static_cast<std::int16_t>(ladder::VerdictOrdinal::kBackdrop);
    ModelVisitor offscreenVisitor{offscreenBackdrop};
    TraceCapture offscreenTrace;
    ok &= check(runSequence(ladder::CommonSequence{}, offscreenVisitor, allCandidates,
                            offscreenTrace) == ladder::Flow::Continue,
                "offscreen common prelude continues");
    ok &= check(runSequence(ladder::OffscreenSequence{}, offscreenVisitor, allCandidates,
                            offscreenTrace) == ladder::Flow::Stop,
                "offscreen backdrop claims before viewport tail");
    auto expectedOffscreen = frozenWalk(offscreenBackdrop, kFrozenCommon, kFrozenOffscreen);
    ok &= check(same(offscreenVisitor.visited, expectedOffscreen),
                "offscreen claim matches frozen route order");
    ok &= check(offscreenVisitor.visited.back() == ladder::SiteId::kOffscreenBackdropBlit,
                "backdrop site stops offscreen suffix");

    Scenario offscreenNone;
    offscreenNone.route = ladder::RouteId::kOffscreen;
    offscreenNone.exitAt = ladder::SiteId::kOffscreenFallthroughNone;
    ModelVisitor offscreenNoneVisitor{offscreenNone};
    TraceCapture offscreenNoneTrace;
    CandidateInterest offscreenNoneInterest;
    ok &= check(runSequence(ladder::CommonSequence{}, offscreenNoneVisitor,
                            offscreenNoneInterest, offscreenNoneTrace) == ladder::Flow::Continue,
                "offscreen None route common prelude continues");
    ok &= check(runSequence(ladder::OffscreenSequence{}, offscreenNoneVisitor,
                            offscreenNoneInterest, offscreenNoneTrace) == ladder::Flow::Stop,
                "offscreen fallthrough exits with None");
    ok &= check(same(offscreenNoneVisitor.visited,
                     frozenWalk(offscreenNone, kFrozenCommon, kFrozenOffscreen)) &&
                offscreenNoneVisitor.visited.back() == ladder::SiteId::kOffscreenFallthroughNone,
                "offscreen None has its own frozen terminal site");

    Scenario flat;
    flat.route = ladder::RouteId::kFlatRuntimeBypass;
    flat.exitAt = ladder::SiteId::kFlatRuntimeBypass;
    ModelVisitor flatVisitor{flat};
    TraceCapture flatTrace;
    CandidateInterest flatInterest;
    ok &= check(runSequence(ladder::FlatRuntimeBypassSequence{}, flatVisitor,
                            flatInterest, flatTrace) == ladder::Flow::Stop,
                "flat route records its existing runtime bypass");
    ok &= check(flatVisitor.visited.size() == 1 &&
                flatVisitor.visited[0] == ladder::SiteId::kFlatRuntimeBypass,
                "flat route does not enter the VR classifier sequence");
    ok &= check(flatInterest.legacyMaskLoads == 0 && flatInterest.candidateQueries == 0 &&
                flatInterest.candidateLoads == 0,
                "flat bypass performs no candidate eligibility or mask load");
    ok &= check(flatTrace.sites.size() == 1 &&
                flatTrace.sites[0].result.outcome == ladder::SiteOutcome::Exited &&
                flatTrace.sites[0].result.verdict ==
                    static_cast<std::int16_t>(ladder::VerdictOrdinal::kNone),
                "flat bypass is a distinct captured None exit");

    ok &= bypassRouteChecks(ladder::RouteId::kAutoBypass,
                            ladder::SiteId::kAutoRuntimeBypass,
                            ladder::AutoBypassSequence{},
                            "Auto command uses a distinct non-classifier bypass");
    ok &= bypassRouteChecks(ladder::RouteId::kDrawIndexedInstancedIndirectBypass,
                            ladder::SiteId::kDrawIndexedInstancedIndirectRuntimeBypass,
                            ladder::DrawIndexedInstancedIndirectBypassSequence{},
                            "indexed indirect command bypasses VR classification and cache reads");
    ok &= bypassRouteChecks(ladder::RouteId::kDrawInstancedIndirectBypass,
                            ladder::SiteId::kDrawInstancedIndirectRuntimeBypass,
                            ladder::DrawInstancedIndirectBypassSequence{},
                            "indirect command bypasses VR classification and cache reads");
    ok &= bypassRouteChecks(ladder::RouteId::kInternalWorldBypass,
                            ladder::SiteId::kInternalWorldBypass,
                            ladder::InternalWorldBypassSequence{},
                            "standalone internal/world draw is a recorded bypass");

    TraceCapture bypassActions;
    ladder::recordAction<TraceCapture, ladder::ActionId::kAutoDraw>(bypassActions, [] {
        ladder::ActionRecord action;
        action.call = ladder::DrawCallKind::Auto;
        action.flags = ladder::kActionGpuDrawArgsUnavailable;
        action.issueCount = 1;
        return action;
    });
    ladder::recordAction<TraceCapture,
        ladder::ActionId::kDrawIndexedInstancedIndirect>(bypassActions, [] {
        ladder::ActionRecord action;
        action.call = ladder::DrawCallKind::DrawIndexedInstancedIndirect;
        action.flags = ladder::kActionGpuDrawArgsUnavailable;
        action.issueCount = 1;
        return action;
    });
    ladder::recordAction<TraceCapture, ladder::ActionId::kDrawInstancedIndirect>(
        bypassActions, [] {
            ladder::ActionRecord action;
            action.call = ladder::DrawCallKind::DrawInstancedIndirect;
            action.flags = ladder::kActionGpuDrawArgsUnavailable;
            action.issueCount = 1;
            return action;
        });
    ok &= check(bypassActions.actions.size() == 3 &&
                bypassActions.actions[0].id == ladder::ActionId::kAutoDraw &&
                bypassActions.actions[0].record.call == ladder::DrawCallKind::Auto &&
                bypassActions.actions[0].record.flags == ladder::kActionGpuDrawArgsUnavailable &&
                bypassActions.actions[1].record.flags == ladder::kActionGpuDrawArgsUnavailable &&
                bypassActions.actions[2].record.flags == ladder::kActionGpuDrawArgsUnavailable &&
                bypassActions.actions[1].record.call ==
                    ladder::DrawCallKind::DrawIndexedInstancedIndirect &&
                bypassActions.actions[2].record.call == ladder::DrawCallKind::DrawInstancedIndirect,
                "command records distinguish Auto and both indirect calls without GPU argument readback");

    Scenario vrNightVision;
    vrNightVision.route = ladder::RouteId::kVrEye;
    vrNightVision.claimAt = ladder::SiteId::kNightVisionClaim;
    vrNightVision.verdict = static_cast<std::int16_t>(ladder::VerdictOrdinal::kNightVision);
    ModelVisitor vrNightVisitor{vrNightVision};
    TraceCapture vrNightTrace;
    CandidateInterest vrNightInterest;
    ok &= check(runSequence(ladder::CommonSequence{}, vrNightVisitor, vrNightInterest,
                            vrNightTrace) == ladder::Flow::Continue,
                "VR route common prelude continues");
    ok &= check(runSequence(ladder::VrEyeSequence{}, vrNightVisitor, vrNightInterest,
                            vrNightTrace) == ladder::Flow::Stop,
                "VR route reaches candidate claim");
    ok &= check(same(vrNightVisitor.visited, frozenWalk(vrNightVision, kFrozenCommon, kFrozenEye)),
                "VR candidate winner preserves frozen order");
    ok &= check(vrNightTrace.sites.back().result.verdict == vrNightVision.verdict &&
                vrNightTrace.sites.back().result.outcome == ladder::SiteOutcome::Claimed,
                "selected claim records the unchanged verdict ordinal");
    ok &= check(vrNightTrace.sites[7].result.subsite ==
                static_cast<std::uint16_t>(ladder::RouteId::kVrEye),
                "route site records VR route id");
    ok &= check(vrNightInterest.legacyMaskLoads == 1 &&
                vrNightInterest.candidateQueries == 1 && vrNightInterest.candidateLoads == 1,
                "candidate eligibility is read once at its ordered rung");

    Scenario vrConflict;
    vrConflict.route = ladder::RouteId::kVrEye;
    vrConflict.claimAt = ladder::SiteId::kHoloClaim;
    vrConflict.verdict = static_cast<std::int16_t>(ladder::VerdictOrdinal::kHolo);
    ModelVisitor vrVisitor{vrConflict};
    TraceCapture vrTrace;
    CandidateInterest vrInterest;
    ok &= check(runSequence(ladder::CommonSequence{}, vrVisitor, vrInterest, vrTrace) ==
                ladder::Flow::Continue, "VR route common prelude continues");
    ok &= check(runSequence(ladder::VrEyeSequence{}, vrVisitor, vrInterest, vrTrace) ==
                ladder::Flow::Stop, "VR later claim wins after earlier declines");
    ok &= check(same(vrVisitor.visited, frozenWalk(vrConflict, kFrozenCommon, kFrozenEye)),
                "VR claim order matches frozen reference");
    ok &= check(vrVisitor.visited.back() == ladder::SiteId::kHoloClaim,
                "claim conflict selects earliest matching rung");
    ok &= check(vrInterest.legacyMaskLoads == 1 &&
                vrInterest.candidateQueries == 1 && vrInterest.candidateLoads == 1,
                "VR reaches the cached candidate gate only once");

    Scenario earlyClaim;
    earlyClaim.route = ladder::RouteId::kVrEye;
    earlyClaim.claimAt = ladder::SiteId::kIntroPanelClaim;
    earlyClaim.verdict = static_cast<std::int16_t>(ladder::VerdictOrdinal::kIntro);
    ModelVisitor earlyVisitor{earlyClaim};
    TraceCapture earlyTrace;
    CandidateInterest earlyInterest;
    ok &= check(runSequence(ladder::CommonSequence{}, earlyVisitor, earlyInterest, earlyTrace) ==
                ladder::Flow::Continue, "early-claim route common prelude continues");
    ok &= check(runSequence(ladder::VrEyeSequence{}, earlyVisitor, earlyInterest, earlyTrace) ==
                ladder::Flow::Stop, "early eye claim terminates ladder");
    ok &= check(earlyInterest.legacyMaskLoads == 1 &&
                earlyInterest.legacyQueries >= 1 &&
                earlyInterest.candidateQueries == 0 && earlyInterest.candidateLoads == 0,
                "intro claim follows the common-interest rung but avoids the later shader candidate query");

    Scenario profileOff;
    profileOff.claimAt = ladder::SiteId::kHoloClaim;
    profileOff.verdict = static_cast<std::int16_t>(ladder::VerdictOrdinal::kHolo);
    ModelVisitor profileVisitor{profileOff};
    TraceCapture profileTrace;
    CandidateInterest profileInterest;
    profileInterest.pluginDispatchEnabled = false;
    ok &= check(runSequence(ladder::CommonSequence{}, profileVisitor, profileInterest, profileTrace) ==
                ladder::Flow::Continue, "profile-off route common prelude continues");
    ok &= check(runSequence(ladder::VrEyeSequence{}, profileVisitor, profileInterest, profileTrace) ==
                ladder::Flow::Stop, "profile-off later legacy claim remains available");
    ok &= check(profileInterest.legacyMaskLoads == 1 &&
                profileInterest.candidateQueries == 1 && profileInterest.candidateLoads == 0,
                "profile guard prevents candidate cache load while legacy mask loads once");
    bool foundNotEligible = false;
    for (const auto& event : profileTrace.sites) {
        if (event.id == ladder::SiteId::kNightVisionClaim) {
            foundNotEligible = event.result.outcome == ladder::SiteOutcome::NotEligible;
        }
    }
    ok &= check(foundNotEligible, "masked candidate site is recorded as not eligible");

    Scenario noDistance;
    noDistance.route = ladder::RouteId::kVrEye;
    noDistance.exitAt = ladder::SiteId::kEyeNoDistanceNone;
    ModelVisitor noDistanceVisitor{noDistance};
    TraceCapture noDistanceTrace;
    CandidateInterest noDistanceInterest;
    ok &= check(runSequence(ladder::CommonSequence{}, noDistanceVisitor, noDistanceInterest,
                            noDistanceTrace) == ladder::Flow::Continue,
                "no-distance route common prelude continues");
    ok &= check(runSequence(ladder::VrEyeSequence{}, noDistanceVisitor, noDistanceInterest,
                            noDistanceTrace) == ladder::Flow::Stop,
                "no-distance route exits with None");
    ok &= check(noDistanceVisitor.visited.back() == ladder::SiteId::kEyeNoDistanceNone,
                "None route has its own stable site id");

    Scenario panelEligibilityNone;
    panelEligibilityNone.route = ladder::RouteId::kVrEye;
    panelEligibilityNone.exitAt = ladder::SiteId::kPanelEligibilityNone;
    ModelVisitor panelEligibilityVisitor{panelEligibilityNone};
    TraceCapture panelEligibilityTrace;
    CandidateInterest panelEligibilityInterest;
    ok &= check(runSequence(ladder::CommonSequence{}, panelEligibilityVisitor,
                            panelEligibilityInterest, panelEligibilityTrace) == ladder::Flow::Continue,
                "panel eligibility None route common prelude continues");
    ok &= check(runSequence(ladder::VrEyeSequence{}, panelEligibilityVisitor,
                            panelEligibilityInterest, panelEligibilityTrace) == ladder::Flow::Stop,
                "panel eligibility route exits with None");
    ok &= check(same(panelEligibilityVisitor.visited,
                     frozenWalk(panelEligibilityNone, kFrozenCommon, kFrozenEye)) &&
                panelEligibilityVisitor.visited.back() == ladder::SiteId::kPanelEligibilityNone,
                "panel eligibility None remains distinct from other None exits");

    TraceCapture actionTrace;
    ladder::recordAction<TraceCapture, ladder::ActionId::kDrawBegin>(actionTrace, [] {
        ladder::ActionRecord r;
        r.phase = ladder::ActionPhase::Begin;
        r.outcome = ladder::ActionOutcome::Applied;
        r.call = ladder::DrawCallKind::DrawIndexedInstanced;
        r.count = 240;
        r.instances = 3;
        r.start = 9;
        r.startInstance = 2;
        r.baseVertex = -4;
        return r;
    });
    ladder::recordAction<TraceCapture, ladder::ActionId::kBackdropEarlyEnd>(actionTrace, [] {
        ladder::ActionRecord r;
        r.phase = ladder::ActionPhase::EarlyEnd;
        r.outcome = ladder::ActionOutcome::Applied;
        r.issueCount = 1;
        return r;
    });
    ladder::recordAction<TraceCapture, ladder::ActionId::kPanelConstantBufferRestore>(actionTrace, [] {
        ladder::ActionRecord r;
        r.phase = ladder::ActionPhase::Restore;
        r.outcome = ladder::ActionOutcome::Applied;
        return r;
    });
    ladder::recordAction<TraceCapture, ladder::ActionId::kGlareInstanceClamp>(actionTrace, [] {
        ladder::ActionRecord r;
        r.phase = ladder::ActionPhase::Clamp;
        r.outcome = ladder::ActionOutcome::Applied;
        r.instances = 1;
        return r;
    });
    ladder::recordAction<TraceCapture, ladder::ActionId::kDrawEnd>(actionTrace, [] {
        ladder::ActionRecord r;
        r.phase = ladder::ActionPhase::End;
        return r;
    });
    ok &= check(actionTrace.actions.size() == 5 &&
                actionTrace.actions[0].id == ladder::ActionId::kDrawBegin &&
                actionTrace.actions[1].id == ladder::ActionId::kBackdropEarlyEnd &&
                actionTrace.actions[2].id == ladder::ActionId::kPanelConstantBufferRestore &&
                actionTrace.actions[3].id == ladder::ActionId::kGlareInstanceClamp &&
                actionTrace.actions[4].id == ladder::ActionId::kDrawEnd,
                "forwarding action order records wrapper, early-end, restore, clamp, end");
    ok &= check(actionTrace.actions[0].record.count == 240 &&
                actionTrace.actions[0].record.instances == 3 &&
                actionTrace.actions[0].record.baseVertex == -4,
                "action record preserves actual draw issue arguments");

    NoTraceProbe noTraceProbe;
    unsigned payloadBuilds = 0;
    ladder::recordAction<NoTraceProbe, ladder::ActionId::kDrawBegin>(noTraceProbe, [&] {
        ++payloadBuilds;
        return ladder::ActionRecord{};
    });
    Scenario noTraceScenario;
    noTraceScenario.exitAt = ladder::SiteId::kEyeNoDistanceNone;
    ModelVisitor noTraceVisitor{noTraceScenario};
    CandidateInterest noTraceInterest;
    ladder::NoTrace noTrace;
    (void)runSequence(ladder::CommonSequence{}, noTraceVisitor, noTraceInterest, noTrace);
    CandidateInterest noTraceEyeInterest;
    noTraceEyeInterest.publishedInterests = 0;
    (void)runSequence(ladder::VrEyeSequence{}, noTraceVisitor, noTraceEyeInterest,
                      noTraceProbe);
    ok &= check(noTraceProbe.callbacks == 0 && payloadBuilds == 0 &&
                noTraceVisitor.handlerCalls[static_cast<std::uint16_t>(ladder::SiteId::kIntroCurveObserve)] == 0 &&
                noTraceVisitor.handlerCalls[static_cast<std::uint16_t>(ladder::SiteId::kPanelCurveObserve)] == 0,
                "disabled trace erases per-rung callbacks and action payload construction");

    ok &= traceWriterChecks(traceDir);

    std::puts(ok ? "draw_ladder_test: all ordered selector checks passed"
                 : "draw_ladder_test: FAILED");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--self-test") == 0) return selfTest(argv[2]);
    std::puts("Usage: draw_ladder_test --self-test <ignored-build-scratch-dir>");
    return 2;
}
