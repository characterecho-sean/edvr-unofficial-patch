#pragma once

#include <cstdint>
#include <tuple>
#include <type_traits>
#include "draw_interest.h"
#include "../common/plugin_cost.h"

// Stable identities for the ordered draw classifier. These are intentionally
// independent of DrawVerdict: a verdict says what was selected, while a site
// says which ordered test or exit selected it.
namespace edvr::draw_ladder {

enum class SiteKind : std::uint8_t {
    Observe = 1,
    Claim = 2,
    Exit = 3,
};

enum class SiteOutcome : std::uint8_t {
    Observed = 1,
    Declined = 2,
    Claimed = 3,
    Exited = 4,
    NotEligible = 5,
};

enum class Flow : std::uint8_t {
    Continue = 0,
    Stop = 1,
};

// IDs are append-only. Do not renumber an existing value; recorded traces use
// these values across builds. Each terminal Skip, Backdrop, and None route has
// its own identity even where the resulting DrawVerdict is shared.
enum class SiteId : std::uint16_t {
    kFssChromeSkip = 1,
    kForeignContextNone = 2,
    kDrawGateDisabledNone = 3,
    kParticleProbe = 4,
    kParticleSubstitute = 5,
    kWitchspaceStarsSkip = 6,
    kStateSnapshot = 7,
    kRouteSelected = 8,
    // 9 and 10 remain reserved after the pre-release schema was simplified to
    // one selection observation plus a RouteId payload.
    kReserved09 = 9,
    kReserved10 = 10,

    kOffscreenViewport = 20,
    kOffscreenAutoState = 21,
    kOffscreenBackdropBlit = 22,
    kOffscreenWakePulseSkip = 23,
    kOffscreenCensusSkip = 24,
    kOffscreenLoaderPanel = 25,
    kOffscreenQuadSkip = 26,
    kOffscreenBodyLayerUpdate = 27,
    kOffscreenFallthroughNone = 28,

    kEyeDepthAndCount = 40,
    kEyeUiDepthProbe = 41,
    kEyeHoloDepthProbe = 42,
    kIntroPanelClaim = 43,
    kIntroCurveObserve = 44,
    kSunglareNomination = 45,
    kUiCrispProbe = 46,
    kObjectProbe = 47,
    kEyeCensusSkip = 48,
    kEyeRangeSkip = 49,
    kNightVisionClaim = 50,
    kRemlokHideSkip = 51,
    kRemlokScissorClaim = 52,
    kHoloClaim = 53,
    kTargetSharpClaim = 54,
    kScrimClaim = 55,
    kEyeBackdropComposite = 56,
    kFssPanelClaim = 57,
    kFssRevealClaim = 58,
    kFssDumpClaim = 59,
    kResolveBindClaim = 60,
    kSunglareSkip = 61,
    kSunglareSteadyClaim = 62,
    kGlareClampClaim = 63,
    kHeadOffsetObserve = 64,
    kPanelCurveObserve = 65,
    kPanelDistanceClaim = 66,
    kEyeNoDistanceNone = 67,
    kPanelEligibilityNone = 68,
    kPanelTailNone = 69,

    kOffscreenSubmittedDraw = 70,
    kEyeCensusSubmitted = 71,
    kFlatRuntimeBypass = 72,
    kAutoRuntimeBypass = 73,
    kDrawIndexedInstancedIndirectRuntimeBypass = 74,
    kDrawInstancedIndirectRuntimeBypass = 75,
    kInternalWorldBypass = 76,

    kSiteIdCount = 77,
};

// Wire ordinals for DrawVerdict. Production keeps its local enum; it pins each
// corresponding value with static_asserts so a recorded verdict stays stable.
enum class VerdictOrdinal : std::int16_t {
    kNone = 0,
    kPanel = 1,
    kSkip = 2,
    kRemlok = 3,
    kHolo = 4,
    kTarget = 5,
    kNightVision = 6,
    kIntro = 7,
    kGlareClamp = 8,
    kSteady = 9,
    kParticle = 10,
    kFssPanel = 11,
    kReveal = 12,
    kDump = 13,
    kResolve = 14,
    kQuadSkip = 15,
    kLoader = 16,
    kScrim = 17,
    kBackdrop = 18,
};

// Action identities are separate from classifier sites because forwarding can
// compose multiple actions after one claim (for example, route then add depth).
enum class ActionId : std::uint16_t {
    kDrawBegin = 1,
    kOriginalDraw = 2,
    kSwallowOriginal = 3,
    kReplaceDraw = 4,
    kUiLayerDraw = 5,
    kCurveStripDraw = 6,
    kWorldRouteDraw = 7,
    kCrispHudDraw = 8,
    kDepthWriteDraw = 9,
    kHoloDepthDraw = 10,
    kBackdropEarlyEnd = 11,
    kBackdropDimDraw = 12,
    kPanelConstantBufferRestore = 13,
    kGlareInstanceClamp = 14,
    kFallbackOriginal = 15,
    kSecondUiDraw = 16,
    kDrawEnd = 17,
    kAutoDraw = 18,
    kDrawIndexedInstancedIndirect = 19,
    kDrawInstancedIndirect = 20,
    kActionIdCount = 21,
};

enum class ActionPhase : std::uint8_t {
    Begin = 1,
    Issue = 2,
    End = 3,
    Restore = 4,
    Clamp = 5,
    EarlyEnd = 6,
};

enum class ActionOutcome : std::uint8_t {
    Attempted = 1,
    Applied = 2,
    Declined = 3,
};

enum class DrawCallKind : std::uint8_t {
    Draw = 1,
    DrawIndexed = 2,
    DrawInstanced = 3,
    DrawIndexedInstanced = 4,
    Auto = 5,
    DrawIndexedInstancedIndirect = 6,
    DrawInstancedIndirect = 7,
};

// Pointer-free forwarding record. Counts/indices describe the actual issued
// call; flags are reserved for action-specific scalar details (never resources).
struct ActionRecord final {
    ActionPhase phase = ActionPhase::Issue;
    ActionOutcome outcome = ActionOutcome::Applied;
    DrawCallKind call = DrawCallKind::Draw;
    std::uint16_t flags = 0;
    std::uint16_t issueCount = 0;
    std::uint32_t count = 0;
    std::uint32_t instances = 1;
    std::uint32_t start = 0;
    std::uint32_t startInstance = 0;
    std::int32_t baseVertex = 0;
};

// Set when an action's number of issued calls is unavailable (for example,
// an early exit before the forwarding path reaches the issue site).
constexpr std::uint16_t kActionIssueCountUnknown = 0x8000;
// A generated helper draw was issued, but its wrapper cannot observe the
// helper's concrete D3D scalar arguments (for example a generated strip).
constexpr std::uint16_t kActionGeneratedDrawArgsUnavailable = 0x2000;
// Indirect commands record their buffer identity/offset, but GPU-owned draw
// arguments are not read back into the trace.
constexpr std::uint16_t kActionGpuDrawArgsUnavailable = 0x4000;

enum class RouteId : std::uint8_t {
    kCommon = 1,
    kForeignOwner = 2,
    kDrawGateOff = 3,
    kOffscreen = 4,
    kReservedRoute05 = 5,
    kVrEye = 6,
    kFlatRuntimeBypass = 7,
    kAutoBypass = 8,
    kDrawIndexedInstancedIndirectBypass = 9,
    kDrawInstancedIndirectBypass = 10,
    kInternalWorldBypass = 11,
};

enum class SequenceId : std::uint8_t {
    kCommon = 1,
    kOffscreen = 2,
    kReservedSequence03 = 3,
    kVrEye = 4,
    kFlatRuntimeBypass = 5,
    kAutoBypass = 6,
    kDrawIndexedInstancedIndirectBypass = 7,
    kDrawInstancedIndirectBypass = 8,
    kInternalWorldBypass = 9,
};

template <SiteId Id, SiteKind Kind>
struct Site final {
    static constexpr SiteId id = Id;
    static constexpr SiteKind kind = Kind;
    static constexpr bool interestGated = false;
    static constexpr bool shaderCandidateGated = false;
};

template <SiteId Id, SiteKind Kind, draw_interest::InterestId Interest>
struct InterestGated final {
    static constexpr SiteId id = Id;
    static constexpr SiteKind kind = Kind;
    static constexpr bool interestGated = true;
    static constexpr bool shaderCandidateGated = false;
    static constexpr draw_interest::InterestId interestId = Interest;
};

template <SiteId Id>
struct ShaderCandidateGatedClaim final {
    static constexpr SiteId id = Id;
    static constexpr SiteKind kind = SiteKind::Claim;
    static constexpr bool interestGated = true;
    static constexpr bool shaderCandidateGated = true;
};

template <class... Sites>
struct Sequence final {
    static constexpr std::uint16_t size = static_cast<std::uint16_t>(sizeof...(Sites));
    using Types = std::tuple<Sites...>;
};

struct SiteResult final {
    Flow flow = Flow::Continue;
    SiteOutcome outcome = SiteOutcome::Observed;
    std::uint16_t subsite = 0;
    std::int16_t verdict = -1;

    static constexpr SiteResult observed(std::uint16_t subsiteValue = 0) noexcept {
        return {Flow::Continue, SiteOutcome::Observed, subsiteValue, -1};
    }
    static constexpr SiteResult declined(std::uint16_t subsiteValue = 0) noexcept {
        return {Flow::Continue, SiteOutcome::Declined, subsiteValue, -1};
    }
    static constexpr SiteResult claimed(std::int16_t verdictValue,
                                        std::uint16_t subsiteValue = 0) noexcept {
        return {Flow::Stop, SiteOutcome::Claimed, subsiteValue, verdictValue};
    }
    static constexpr SiteResult exited(std::int16_t verdictValue,
                                       std::uint16_t subsiteValue = 0) noexcept {
        return {Flow::Stop, SiteOutcome::Exited, subsiteValue, verdictValue};
    }
    static constexpr SiteResult notEligible() noexcept {
        return {Flow::Continue, SiteOutcome::NotEligible, 0, -1};
    }
};

#if defined(_MSC_VER)
#define EDVR_LADDER_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define EDVR_LADDER_FORCEINLINE inline __attribute__((always_inline))
#else
#define EDVR_LADDER_FORCEINLINE inline
#endif

// CPU-cost rows describe only the time spent in the typed classifier handler.
// Keep the attribution table here beside the ordered sites; ambiguous shared
// state and always-kept census plumbing are Core until a narrower owner is
// evidenced. This is reporting metadata, never plugin selection.
template <SiteId Id>
constexpr plugin_cost::Owner cpuOwnerForSite() noexcept {
    if constexpr (Id == SiteId::kParticleProbe || Id == SiteId::kObjectProbe ||
                  Id == SiteId::kUiCrispProbe ||
                  Id == SiteId::kOffscreenCensusSkip || Id == SiteId::kOffscreenQuadSkip ||
                  Id == SiteId::kEyeCensusSkip || Id == SiteId::kEyeRangeSkip) {
        return plugin_cost::Owner::Diagnostics;
    } else if constexpr (Id == SiteId::kParticleSubstitute ||
                         Id == SiteId::kWitchspaceStarsSkip ||
                         Id == SiteId::kOffscreenWakePulseSkip ||
                         Id == SiteId::kSunglareNomination || Id == SiteId::kNightVisionClaim ||
                         Id == SiteId::kRemlokHideSkip || Id == SiteId::kRemlokScissorClaim ||
                         Id == SiteId::kTargetSharpClaim || Id == SiteId::kSunglareSkip ||
                         Id == SiteId::kSunglareSteadyClaim || Id == SiteId::kGlareClampClaim) {
        return plugin_cost::Owner::CockpitVisuals;
    } else if constexpr (Id == SiteId::kOffscreenViewport ||
                         Id == SiteId::kFssPanelClaim || Id == SiteId::kFssRevealClaim ||
                         Id == SiteId::kFssDumpClaim || Id == SiteId::kResolveBindClaim) {
        return plugin_cost::Owner::Scanners;
    } else if constexpr (Id == SiteId::kOffscreenBackdropBlit ||
                         Id == SiteId::kOffscreenLoaderPanel || Id == SiteId::kIntroPanelClaim ||
                         Id == SiteId::kIntroCurveObserve || Id == SiteId::kHoloClaim ||
                         Id == SiteId::kScrimClaim || Id == SiteId::kEyeBackdropComposite) {
        return plugin_cost::Owner::Intro;
    } else if constexpr (Id == SiteId::kPanelCurveObserve ||
                         Id == SiteId::kEyeNoDistanceNone ||
                         Id == SiteId::kPanelEligibilityNone ||
                         Id == SiteId::kPanelDistanceClaim || Id == SiteId::kPanelTailNone) {
        return plugin_cost::Owner::OnFootPanel;
    } else if constexpr (Id == SiteId::kEyeUiDepthProbe ||
                         Id == SiteId::kEyeHoloDepthProbe) {
        return plugin_cost::Owner::TemporalAa;
    } else if constexpr (Id == SiteId::kHeadOffsetObserve) {
        return plugin_cost::Owner::Comfort;
    } else {
        // Core includes shared classifier state/route work, foreign/draw-gate
        // exits, census observations, and any site not yet assigned narrowly.
        return plugin_cost::Owner::Core;
    }
}

template <class Visitor, class SiteType, class InterestProvider,
          class TracePolicy, class CpuPolicy>
EDVR_LADDER_FORCEINLINE void visitOne(Flow& flow, Visitor& visitor, InterestProvider& interest,
                                      TracePolicy& trace, CpuPolicy& cpu) {
    if constexpr (!CpuPolicy::enabled) (void)cpu;
    if (flow == Flow::Continue) {
        constexpr plugin_cost::Owner owner = cpuOwnerForSite<SiteType::id>();
        constexpr std::uint16_t siteId = static_cast<std::uint16_t>(SiteType::id);
        if constexpr (CpuPolicy::enabled) {
            CpuPolicy::template note<owner, siteId>(plugin_cost::SiteEvent::Reached);
        }
        if constexpr (SiteType::interestGated) {
            if (!interest.template eligible<SiteType>()) {
                if constexpr (TracePolicy::enabled) {
                    trace.template site<SiteType::id, SiteType::kind>(SiteResult::notEligible());
                }
                if constexpr (CpuPolicy::enabled) {
                    CpuPolicy::template note<owner, siteId>(plugin_cost::SiteEvent::NotEligible);
                }
                return;
            }
        }
        static_assert(std::is_same_v<decltype(visitor.template visit<SiteType>()), SiteResult>,
                      "draw-ladder visitors return draw_ladder::SiteResult");
        if constexpr (CpuPolicy::enabled) {
            CpuPolicy::template note<owner, siteId>(plugin_cost::SiteEvent::Invoked);
            SiteResult result{};
            {
                typename CpuPolicy::template Scope<owner, siteId> timed;
                result = visitor.template visit<SiteType>();
            }
            if constexpr (TracePolicy::enabled) {
                trace.template site<SiteType::id, SiteType::kind>(result);
            }
            flow = result.flow;
        } else {
            const SiteResult result = visitor.template visit<SiteType>();
            if constexpr (TracePolicy::enabled) {
                trace.template site<SiteType::id, SiteType::kind>(result);
            }
            flow = result.flow;
        }
    }
}

// A fold expression makes the source order explicit and generates direct,
// typed calls. The Flow test prevents evaluation of every suffix after a
// terminal claim or exit; no runtime array or function-pointer dispatch exists.
template <class... Sites, class Visitor, class InterestProvider,
          class TracePolicy, class CpuPolicy>
EDVR_LADDER_FORCEINLINE Flow visitOrdered(Sequence<Sites...>, Visitor& visitor,
                                          InterestProvider& interest, TracePolicy& trace,
                                          CpuPolicy& cpu) {
    Flow flow = Flow::Continue;
    (visitOne<Visitor, Sites>(flow, visitor, interest, trace, cpu), ...);
    return flow;
}

template <class... Sites, class Visitor, class InterestProvider, class TracePolicy>
EDVR_LADDER_FORCEINLINE Flow visitOrdered(Sequence<Sites...> sequence, Visitor& visitor,
                                          InterestProvider& interest, TracePolicy& trace) {
    plugin_cost::NoCpu cpu;
    return visitOrdered(sequence, visitor, interest, trace, cpu);
}

#undef EDVR_LADDER_FORCEINLINE

template <class TracePolicy, ActionId Id, class MakeRecord>
inline void recordAction(TracePolicy& trace, MakeRecord&& makeRecord) noexcept {
    if constexpr (TracePolicy::enabled) {
        trace.template action<Id>(makeRecord());
    }
}

struct NoTrace final {
    static constexpr bool enabled = false;

    template <SiteId, SiteKind>
    inline void site(SiteResult) noexcept {}

    template <ActionId>
    inline void action(ActionRecord) noexcept {}
};

// Canonical sequences shared by the production visitor and its focused rig.
// Runtime route selection chooses one branch sequence after kCommonSequence.
using FirstLegacyInterestSite =
    InterestGated<SiteId::kParticleSubstitute, SiteKind::Claim,
                  draw_interest::InterestId::ParticleSubstitute>;

using CommonSequence = Sequence<
    Site<SiteId::kFssChromeSkip, SiteKind::Claim>,
    Site<SiteId::kForeignContextNone, SiteKind::Exit>,
    Site<SiteId::kDrawGateDisabledNone, SiteKind::Exit>,
    Site<SiteId::kParticleProbe, SiteKind::Observe>,
    FirstLegacyInterestSite,
    InterestGated<SiteId::kWitchspaceStarsSkip, SiteKind::Claim,
                  draw_interest::InterestId::WitchspaceStars>,
    Site<SiteId::kStateSnapshot, SiteKind::Observe>,
    Site<SiteId::kRouteSelected, SiteKind::Observe>>;

using OffscreenSequence = Sequence<
    Site<SiteId::kOffscreenSubmittedDraw, SiteKind::Observe>,
    Site<SiteId::kOffscreenBackdropBlit, SiteKind::Claim>,
    Site<SiteId::kOffscreenViewport, SiteKind::Observe>,
    Site<SiteId::kOffscreenAutoState, SiteKind::Observe>,
    Site<SiteId::kOffscreenWakePulseSkip, SiteKind::Claim>,
    Site<SiteId::kOffscreenCensusSkip, SiteKind::Claim>,
    Site<SiteId::kOffscreenLoaderPanel, SiteKind::Claim>,
    Site<SiteId::kOffscreenQuadSkip, SiteKind::Claim>,
    Site<SiteId::kOffscreenBodyLayerUpdate, SiteKind::Observe>,
    Site<SiteId::kOffscreenFallthroughNone, SiteKind::Exit>>;

using EyeSequence = Sequence<
    Site<SiteId::kEyeDepthAndCount, SiteKind::Observe>,
    Site<SiteId::kEyeUiDepthProbe, SiteKind::Observe>,
    Site<SiteId::kEyeHoloDepthProbe, SiteKind::Observe>,
    Site<SiteId::kIntroPanelClaim, SiteKind::Claim>,
    InterestGated<SiteId::kIntroCurveObserve, SiteKind::Observe,
                  draw_interest::InterestId::IntroCurveObserve>,
    Site<SiteId::kSunglareNomination, SiteKind::Observe>,
    Site<SiteId::kEyeCensusSubmitted, SiteKind::Observe>,
    Site<SiteId::kUiCrispProbe, SiteKind::Observe>,
    Site<SiteId::kObjectProbe, SiteKind::Observe>,
    Site<SiteId::kEyeCensusSkip, SiteKind::Claim>,
    Site<SiteId::kEyeRangeSkip, SiteKind::Claim>,
    ShaderCandidateGatedClaim<SiteId::kNightVisionClaim>,
    Site<SiteId::kRemlokHideSkip, SiteKind::Claim>,
    Site<SiteId::kRemlokScissorClaim, SiteKind::Claim>,
    Site<SiteId::kHoloClaim, SiteKind::Claim>,
    InterestGated<SiteId::kTargetSharpClaim, SiteKind::Claim,
                  draw_interest::InterestId::TargetSharp>,
    Site<SiteId::kScrimClaim, SiteKind::Claim>,
    Site<SiteId::kEyeBackdropComposite, SiteKind::Claim>,
    InterestGated<SiteId::kFssPanelClaim, SiteKind::Claim,
                  draw_interest::InterestId::FssPanel>,
    InterestGated<SiteId::kFssRevealClaim, SiteKind::Claim,
                  draw_interest::InterestId::FssReveal>,
    InterestGated<SiteId::kFssDumpClaim, SiteKind::Claim,
                  draw_interest::InterestId::FssDump>,
    Site<SiteId::kResolveBindClaim, SiteKind::Claim>,
    Site<SiteId::kSunglareSkip, SiteKind::Claim>,
    Site<SiteId::kSunglareSteadyClaim, SiteKind::Claim>,
    Site<SiteId::kGlareClampClaim, SiteKind::Claim>,
    Site<SiteId::kHeadOffsetObserve, SiteKind::Observe>,
    InterestGated<SiteId::kPanelCurveObserve, SiteKind::Observe,
                  draw_interest::InterestId::PanelCurveObserve>,
    Site<SiteId::kEyeNoDistanceNone, SiteKind::Exit>,
    Site<SiteId::kPanelEligibilityNone, SiteKind::Exit>,
    Site<SiteId::kPanelDistanceClaim, SiteKind::Claim>,
    Site<SiteId::kPanelTailNone, SiteKind::Exit>>;

using VrEyeSequence = EyeSequence;

using FlatRuntimeBypassSequence = Sequence<
    Site<SiteId::kFlatRuntimeBypass, SiteKind::Exit>>;

using AutoBypassSequence = Sequence<
    Site<SiteId::kAutoRuntimeBypass, SiteKind::Exit>>;

using DrawIndexedInstancedIndirectBypassSequence = Sequence<
    Site<SiteId::kDrawIndexedInstancedIndirectRuntimeBypass, SiteKind::Exit>>;

using DrawInstancedIndirectBypassSequence = Sequence<
    Site<SiteId::kDrawInstancedIndirectRuntimeBypass, SiteKind::Exit>>;

using InternalWorldBypassSequence = Sequence<
    Site<SiteId::kInternalWorldBypass, SiteKind::Exit>>;

static_assert(CommonSequence::size == 8, "common ladder site count changed");
static_assert(OffscreenSequence::size == 10, "offscreen ladder site count changed");
static_assert(EyeSequence::size == 31, "eye ladder site count changed");
static_assert(FlatRuntimeBypassSequence::size == 1, "flat bypass site count changed");
static_assert(AutoBypassSequence::size == 1, "auto bypass site count changed");
static_assert(DrawIndexedInstancedIndirectBypassSequence::size == 1,
              "indexed indirect bypass site count changed");
static_assert(DrawInstancedIndirectBypassSequence::size == 1,
              "indirect bypass site count changed");
static_assert(InternalWorldBypassSequence::size == 1,
              "internal/world bypass site count changed");
static_assert(CommonSequence::size > 4 &&
              std::is_same_v<std::tuple_element_t<4, CommonSequence::Types>,
                             FirstLegacyInterestSite>,
              "the first legacy interest snapshot must be the Common ParticleSubstitute rung");
static_assert(!std::tuple_element_t<0, CommonSequence::Types>::interestGated &&
              !std::tuple_element_t<1, CommonSequence::Types>::interestGated &&
              !std::tuple_element_t<2, CommonSequence::Types>::interestGated &&
              !std::tuple_element_t<3, CommonSequence::Types>::interestGated,
              "mandatory common claims/exits/observations precede the first interest snapshot");

} // namespace edvr::draw_ladder
