#pragma once

#include "draw_census.h"
#include "draw_ladder.h"

#include <atomic>
#include <cstdint>

namespace edvr::draw_ladder_trace {

// One complete owner-context frame is retained. These caps are part of the
// trace format contract: exceeding any cap invalidates the whole capture and
// the reader refuses it rather than silently replaying a prefix.
// A documented on-foot frame can contain 17,180 draw-hook calls. Leave room
// for busy frames without making the opt-in capture fail at that known case.
constexpr std::uint32_t kMaxDraws = 65536;
constexpr std::uint16_t kMaxSiteEventsPerDraw = 48;
constexpr std::uint16_t kMaxActionEventsPerDraw = 32;
constexpr std::uint8_t kMaxPredicateFactsPerDraw = 4;
static_assert(kMaxDraws >= 17180, "replay capacity must cover the documented on-foot frame");

enum class Status : std::uint8_t {
    Disabled = 0,
    PathInvalid = 1,
    AllocationFailed = 2,
    Ready = 3,
    Armed = 4,
    Capturing = 5,
    CompleteWritten = 6,
    InvalidCapture = 7,
    WriteFailed = 8,
};

// Returned by shutdown() so production can report whether an armed or partial
// capture was intentionally discarded during cold teardown.
struct ShutdownResult final {
    Status previousStatus = Status::Disabled;
    bool discardedPendingArm = false;
    bool discardedCapture = false;
};

struct Token final {
    std::uint32_t drawIndex = UINT32_MAX;
    std::uint32_t generation = 0;
    constexpr bool valid() const noexcept { return drawIndex != UINT32_MAX; }
};

struct FrameFacts final {
    std::uint32_t frameNo = 0;
    std::uint32_t configEpoch = 0;
    std::uint32_t eyeDrawsThisFrame = 0;
    std::uint32_t eyeDrawsLastFrame = 0;
    std::uint32_t sceneDrawsThisFrame = 0;
    std::uint32_t stateFlags = 0;
    std::uint32_t sceneCounters[4]{};
};

struct DrawFacts final {
    std::uint32_t eyeDrawIndex = 0;
    std::uint8_t kind = 0;  // D/I/N/X direct, A Auto, Z/Y indirect.
    draw_ladder::RouteId route = draw_ladder::RouteId::kCommon;
    draw_ladder::SequenceId sequence = draw_ladder::SequenceId::kCommon;
    std::uint32_t count = 0;
    std::uint32_t instances = 1;
    DrawArgs args{};
    std::uint64_t vsHash = 0;
    std::uint64_t psHash = 0;
    std::uintptr_t vsIdentity = 0;
    std::uintptr_t psIdentity = 0;
    std::uintptr_t rtv0Identity = 0;
    std::uintptr_t dsv0Identity = 0;
    std::uint32_t rtv0Generation = 0;
    std::uint32_t dsv0Generation = 0;
    std::uint32_t rtv0Width = 0;
    std::uint32_t rtv0Height = 0;
    std::uintptr_t argumentBufferIdentity = 0;
    std::uint32_t argumentByteOffset = 0;
    std::uint64_t candidateMask = 0;
    std::uint32_t flags = 0;
    bool argumentBufferKnown = false;
    bool drawParametersKnown = true;
};

enum class TriState : std::uint8_t {
    Unknown = 0,
    No = 1,
    Yes = 2,
};

// Inputs for the small, independently re-evaluable predicate slice. These
// facts are recorded only by TracePolicy at already-reached sites; expected
// SiteEvents remain outputs and are never used as selector inputs.
enum class PredicateFactKind : std::uint8_t {
    DrawGateWanted = 1,
    EyeRangeSkip = 2,
    NightVisionClaim = 3,
    WitchspaceStarsSkip = 4,
};

struct PredicateRange final {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
};

struct PredicateFact final {
    std::uint16_t siteId = 0;
    PredicateFactKind kind = PredicateFactKind::DrawGateWanted;
    TriState known = TriState::Unknown;
    TriState gateWanted = TriState::Unknown;
    std::uint8_t rangeCount = 0;
    std::uint32_t eyeDrawIndex = 0;
    PredicateRange ranges[4]{};
    // EyeRangeSkip's observed post-increment counter delta. False means the
    // input fact is valid but the mutation observation is unavailable.
    bool censusSkippedDeltaKnown = false;
    std::uint32_t censusSkippedDelta = 0;
    // NightVisionClaim source inputs. Availability is explicit because the
    // legacy ladder short-circuits before later stages on dispatch/candidate
    // misses. The callback observation is sampled during its single call.
    TriState dispatchEnabled = TriState::Unknown;
    TriState activeMaskKnown = TriState::Unknown;
    std::uint64_t activePluginMask = 0;
    TriState candidateKnown = TriState::Unknown;
    TriState candidatePresent = TriState::Unknown;
    TriState modeKnown = TriState::Unknown;
    std::uint8_t mode = 0;
    TriState shapeReached = TriState::Unknown;
    TriState shapeMatched = TriState::Unknown;
    TriState callbackReached = TriState::Unknown;
    TriState callbackModeKnown = TriState::Unknown;
    std::uint8_t callbackMode = 0;
    TriState failedKnown = TriState::Unknown;
    TriState failed = TriState::Unknown;
    // WitchspaceStarsSkip source inputs and the observed single-call hash.
    // The cached interest mask validates whether the ladder staged the site;
    // it is not used as the frozen helper's selector oracle.
    TriState interestMaskKnown = TriState::Unknown;
    std::uint64_t legacyInterestMask = 0;
    TriState starsHelperReached = TriState::Unknown;
    TriState hiddenKnown = TriState::Unknown;
    TriState hidden = TriState::Unknown;
    TriState contextKnown = TriState::Unknown;
    TriState contextValid = TriState::Unknown;
    TriState starsShapeReached = TriState::Unknown;
    TriState starsShapeMatched = TriState::Unknown;
    TriState starsHashKnown = TriState::Unknown;
    std::uint8_t starsHashSource = 0;
    std::uint64_t starsVsHash = 0;
    bool starsSkippedDeltaKnown = false;
    std::uint32_t starsSkippedDelta = 0;
    bool detailsFinalized = false;  // internal capture validity; not serialized
};

// The census counter is uint64_t. Keep only the supported single-step delta
// in the trace's uint32_t field; sentinel 2 intentionally trips its validator
// for any other observed jump. Unsigned subtraction preserves a real wrap.
constexpr std::uint32_t boundedCensusSkippedDelta(std::uint64_t before,
                                                  std::uint64_t after) noexcept {
    const std::uint64_t delta = after - before;
    return delta <= 1 ? static_cast<std::uint32_t>(delta) : 2u;
}

// Each bit marks a decision field actually supplied by the caller. A clear
// bit/Unknown value means the decision was not reached or was unavailable.
enum ForwardFact : std::uint32_t {
    kForwardOwner = 1u << 0,
    kForwardVerdict = 1u << 1,
    kForwardVerdictForwards = 1u << 2,
    kForwardFamily = 1u << 3,
    kForwardInitialUiTake = 1u << 4,
    kForwardAfterUiTake = 1u << 5,
    kForwardWorldReissue = 1u << 6,
    kForwardCurveThisDraw = 1u << 7,
    kForwardIntroCurveThisDraw = 1u << 8,
    kForwardUiDepth = 1u << 9,
    kForwardHoloDepth = 1u << 10,
    kForwardComposite = 1u << 11,
    kForwardCrispPending = 1u << 12,
    kForwardIssueBlocked = 1u << 13,
};

struct ForwardFacts final {
    std::uint32_t presentMask = 0;
    std::int16_t verdictOrdinal = -1;
    std::uint16_t family = 0;
    TriState owner = TriState::Unknown;
    TriState verdictForwards = TriState::Unknown;
    TriState familyAvailable = TriState::Unknown;
    TriState initialUiTake = TriState::Unknown;
    TriState afterUiTake = TriState::Unknown;
    TriState worldReissue = TriState::Unknown;
    TriState curveThisDraw = TriState::Unknown;
    TriState introCurveThisDraw = TriState::Unknown;
    TriState uiDepth = TriState::Unknown;
    TriState holoDepth = TriState::Unknown;
    TriState composite = TriState::Unknown;
    TriState crispPending = TriState::Unknown;
    TriState issueBlocked = TriState::Unknown;
};

// Call after Config and Log initialization. The user switch is read by the
// caller; when disabled this allocates no storage and performs no file work.
// Pass the exact path selected by Log::open; this avoids associating a sidecar
// with another concurrent or flat-profile session.
void configure(bool enabled, const wchar_t* logFilePath) noexcept;
// Call only after draw hooks are uninstalled and no trace callback can run.
// This cold path discards partial data, frees opt-in storage, and never writes
// a sidecar. The returned prior state distinguishes discard from success.
ShutdownResult shutdown() noexcept;
void armManual() noexcept;

// Called only at owner-frame boundaries. frameBegin starts a capture only if
// the manual request was pending at that boundary. frameEnd writes only after
// the entire captured frame has completed.
void frameBegin(const FrameFacts& facts) noexcept;
void frameEnd(std::uint32_t completedFrameNo) noexcept;
void invalidateActiveCapture() noexcept;

// Called only from the trace-enabled draw specialization. No resource query,
// allocation, lock, or log call occurs in these append functions.
Token beginDraw(const DrawFacts& facts) noexcept;
void appendSite(Token token, std::uint16_t id, std::uint8_t kind,
                std::uint8_t outcome, std::uint8_t flow,
                std::uint16_t subsite, std::int16_t siteVerdict) noexcept;
void appendAction(Token token, std::uint16_t id,
                  const draw_ladder::ActionRecord& action) noexcept;
void recordForwardFacts(Token token, const ForwardFacts& facts) noexcept;
void appendPredicateFact(Token token, const PredicateFact& fact) noexcept;
void completeNightVisionFact(Token token, const PredicateFact& fact) noexcept;
void completeWitchspaceStarsFact(Token token, const PredicateFact& fact) noexcept;
void updateCandidates(Token token, std::uint64_t mask) noexcept;
void updateRoute(Token token, draw_ladder::RouteId route,
                 draw_ladder::SequenceId sequence) noexcept;
void finishDraw(Token token, std::int16_t winnerSiteId,
                std::int16_t verdictOrdinal) noexcept;

bool configured() noexcept;
bool capturing() noexcept;
bool overflowed() noexcept;
Status status() noexcept;
const char* statusName(Status value) noexcept;

namespace detail {
extern std::atomic<bool> g_captureActive;
}

// Exactly one draw-level branch chooses TracePolicy or draw_ladder::NoTrace.
// The disabled specialization has no per-site calls or payload construction.
inline bool drawLadderTraceCaptureActive() noexcept {
    return detail::g_captureActive.load(std::memory_order_relaxed);
}

struct TracePolicy final {
    static constexpr bool enabled = true;
    Token token{};

    template <draw_ladder::SiteId Id, draw_ladder::SiteKind Kind,
              class Payload>
    inline void site(const Payload& payload) noexcept {
        appendSite(token, static_cast<std::uint16_t>(Id),
                   static_cast<std::uint8_t>(Kind),
                   static_cast<std::uint8_t>(payload.outcome),
                   static_cast<std::uint8_t>(payload.flow), payload.subsite,
                   payload.verdict);
    }

    template <draw_ladder::ActionId Id>
    inline void action(const draw_ladder::ActionRecord& payload) noexcept {
        appendAction(token, static_cast<std::uint16_t>(Id), payload);
    }

    inline void candidates(std::uint64_t mask) noexcept {
        updateCandidates(token, mask);
    }

    inline void route(draw_ladder::RouteId routeId,
                      draw_ladder::SequenceId sequenceId) noexcept {
        updateRoute(token, routeId, sequenceId);
    }

    inline void forward(const ForwardFacts& facts) noexcept {
        recordForwardFacts(token, facts);
    }

    inline void predicateFact(const PredicateFact& fact) noexcept {
        appendPredicateFact(token, fact);
    }

    inline void completePredicateFact(const PredicateFact& fact) noexcept {
        completeNightVisionFact(token, fact);
    }

    inline void completeWitchspaceStarsPredicateFact(const PredicateFact& fact) noexcept {
        completeWitchspaceStarsFact(token, fact);
    }
};

inline TracePolicy makePolicy(Token token) noexcept {
    TracePolicy policy{};
    policy.token = token;
    return policy;
}

}  // namespace edvr::draw_ladder_trace
