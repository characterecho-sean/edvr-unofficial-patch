#pragma once

#if !defined(EDVR_VSCREEN_PREDICATE_TEST)
#error "This test interface is available only to the VScreen predicate rig."
#endif

#include "../../src/d3d11/draw_ladder_trace.h"
#include "../../src/d3d11/eye_census_observation.h"
#include "../../src/d3d11/resolve_bind_observation.h"
#include "../../src/d3d11/loader_panel_observation.h"
#include "../../src/d3d11/fss_dump_observation.h"
#include "../../src/d3d11/forwarding_observation.h"
#include "../../src/d3d11/target_sharp_observation.h"
#include "../../src/d3d11/sunglare_nomination_observation.h"

struct ID3D11DeviceContext;

namespace edvr {

struct VScreenPredicateTestResult final {
    draw_ladder_trace::Token token{};
    draw_ladder::SiteResult siteResult{};
    std::uint32_t glareClampAfter = 0;
    std::uint64_t censusSkippedAfter = 0;
    void* sceneCbNominatedAfter = nullptr;
};

struct VScreenForwardingTestInput final {
    std::int16_t verdictOrdinal = 0;
    char kind = 'N';
    std::uint32_t count = 3;
    std::uint32_t instances = 1;
    edvr::DrawArgs args{};
    std::int32_t engineVelocityCacheFamily = -1;
    bool issueBlockedEntry = false;
    bool objectProbeLedgerOn = false;
    bool uiDepthThisDraw = false;
    bool holoDepthThisDraw = false;
    bool compositeThisDraw = false;
    bool curveThisDraw = false;
    bool seedDiagnostics = false;
    bool uiLayerLive = false;
    bool uiLayerWatching = false;
    bool callbackReturns = true;
    bool changeIssueBlockedAfter = false;
    bool issueBlockedAfter = false;
    bool changeCrispPendingAfter = false;
    bool crispPendingAfter = false;
    bool changePlanetPendingAfter = false;
    bool planetPendingAfter = false;
    bool planetSolarPendingAfter = false;
};

struct VScreenForwardingTestResult final {
    draw_ladder_trace::Token token{};
    std::uint32_t originalCalls = 0;
    std::uint8_t alteredClass = 0;
    bool callbackReturned = false;
    bool issueBlockedAfter = false;
    bool crispPendingAfter = false;
    bool planetPendingAfter = false;
    bool planetSolarPendingAfter = false;
    bool curveThisDrawAfter = false;
    std::int32_t engineVelocityCacheFamilyAfter = -1;
};

// Runs the production PanelDistance claim, forwarded draw, and restoration
// through the test-only saved-original callbacks. The collector's owner/context
// and API-frame state are configured by the serialized rig before this call.
// This is a prequalified warm-site seam: it does not run the common foreign
// context exit or panel eligibility sites. Context mismatch tests API selection.
struct VScreenPanelDistanceApiTestInput final {
    ID3D11DeviceContext* context = nullptr;
    ID3D11DeviceContext* ownerContext = nullptr;
    bool traceEnabled = false;
    bool cpuSample = false;
    bool distanceEnabled = true;
    std::uint8_t shadow[256]{};
    std::uint32_t shadowBytes = 0;
    std::uint32_t distanceIndex = 0;
    float distanceScale = 1.0f;
    void* compositeCb = nullptr;
    void* ourCb = nullptr;
    std::int32_t mapHresult = 0;
    bool mapReturnsNull = false;
    std::uint8_t* mappedStorage = nullptr;
    std::uint32_t mappedStorageBytes = 0;
    // Test-only full Common+Eye selector traversal. Requires a real panel-size
    // SRV/CB binding and uses the injected Map callback for the terminal miss.
    bool fullClassifier = false;
    void* panelSrv = nullptr;
    void* eyeRtv = nullptr;
    char kind = 'I';
    std::uint32_t drawCount = 3;
    std::uint32_t drawInstances = 1;
    edvr::DrawArgs drawArgs{};
};

struct VScreenClassifierSiteEvent final {
    std::uint16_t siteId = 0;
    std::uint8_t kind = 0;
    std::uint8_t outcome = 0;
    std::uint16_t subsite = 0;
    std::int16_t verdict = -1;
};

enum class VScreenPanelDistanceApiTestEvent : std::uint8_t {
    Map = 1,
    Unmap = 2,
    OverrideBind = 3,
    OriginalDraw = 4,
    RestoreBind = 5,
};

struct VScreenPanelDistanceApiTestResult final {
    draw_ladder_trace::Token token{};
    draw_ladder::SiteResult siteResult{};
    std::int16_t winner = -1;
    std::int16_t verdict = 0;
    std::uint32_t mapCalls = 0;
    std::uint32_t unmapCalls = 0;
    std::uint32_t constantBufferCalls = 0;
    std::uint32_t originalDrawCalls = 0;
    std::uint32_t mapSubresource = 0;
    std::uint32_t mapType = 0;
    std::uint32_t mapFlags = 0;
    std::uint32_t unmapSubresource = 0;
    std::uint32_t bindStartSlots[2]{};
    std::uint32_t bindCounts[2]{};
    void* bindBuffers[2]{};
    void* finalBoundCb = nullptr;
    bool mapArgumentsValid = false;
    bool unmapArgumentsValid = false;
    bool overrideBindArgumentsValid = false;
    bool restoreBindArgumentsValid = false;
    bool drawArgumentsValid = false;
    bool eventOverflow = false;
    std::uint8_t eventCount = 0;
    std::uint8_t events[5]{};
    std::uint32_t mappedBytes = 0;
    std::uint8_t mappedSnapshot[256]{};
    std::uint8_t classifierSiteCount = 0;
    bool classifierSiteOverflow = false;
    VScreenClassifierSiteEvent classifierSites[48]{};
};

bool vScreenPanelDistanceApiTransactionTest(
    const VScreenPanelDistanceApiTestInput& input,
    VScreenPanelDistanceApiTestResult* result) noexcept;

struct VScreenEyeCensusTestFilter final {
    std::uint8_t mode = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct VScreenEyeCensusTestRule final {
    char kind = 'N';
    std::uint32_t count = 0;
    std::uint32_t countHigh = 0;
    std::uint64_t vsHash = 0;
    VScreenEyeCensusTestFilter filters[4]{};
};

// Called only by a serialized typed COM callback during the active test visit.
struct VScreenEyeCensusTestMutation final {
    bool changeFilter = false;
    std::uint32_t ruleIndex = 0;
    std::uint32_t filterIndex = 0;
    VScreenEyeCensusTestFilter filter{};
    bool changeLoopCount = false;
    std::uint32_t loopCount = 0;
    bool changeCounter = false;
    std::uint64_t counter = 0;
};
bool vScreenEyeCensusPredicateTestMutate(
    const VScreenEyeCensusTestMutation& mutation) noexcept;

// Serialized test-owner calls only: temporarily replaces g_state and restores
// the thread-local draw flags touched by the real owner-context visitor.
bool vScreenPredicateTestVisit(std::uint16_t siteId,
                               ID3D11DeviceContext* context,
                               ID3D11DeviceContext* ownerContext,
                               std::uint32_t glareClamp,
                               bool distanceEnabled,
                               bool traceEnabled,
                               VScreenPredicateTestResult* result) noexcept;

bool vScreenEyeCensusPredicateTestVisit(
    ID3D11DeviceContext* context, char kind, std::uint32_t count,
    const VScreenEyeCensusTestRule* rules, std::uint32_t ruleCount,
    void* const psSrvs[4], std::uint64_t heldVsHash,
    std::uint64_t censusSkippedSeed, bool traceEnabled,
    VScreenPredicateTestResult* result) noexcept;

bool vScreenResolveBindPredicateTestVisit(
    ID3D11DeviceContext* context, bool traceEnabled,
    VScreenPredicateTestResult* result) noexcept;

bool vScreenLoaderPanelPredicateTestVisit(
    ID3D11DeviceContext* context, void* renderTargetView,
    std::uint32_t eyeDrawsLastFrame, std::uint32_t qsStartIndex,
    std::int32_t qsBaseVertex, char kind, std::uint32_t count,
    bool traceEnabled, VScreenPredicateTestResult* result) noexcept;

bool vScreenFssDumpPredicateTestVisit(
    ID3D11DeviceContext* context, char kind, std::uint32_t count,
    std::uint32_t instances, std::uint32_t frameNo,
    std::uint32_t fssBodyFrame, bool traceEnabled,
    VScreenPredicateTestResult* result) noexcept;

bool vScreenForwardingPredicateTestVisit(
    ID3D11DeviceContext* context, bool traceEnabled,
    const VScreenForwardingTestInput& input,
    VScreenForwardingTestResult* result) noexcept;

bool vScreenTargetSharpPredicateTestVisit(
    ID3D11DeviceContext* context, char kind, std::uint32_t count,
    std::uint32_t instances, std::uint32_t eyeW, std::uint32_t eyeH,
    std::uint32_t renderW, std::uint32_t renderH, bool traceEnabled,
    VScreenPredicateTestResult* result) noexcept;

bool vScreenSunglareNominationPredicateTestVisit(
    ID3D11DeviceContext* context, char kind, std::uint32_t count,
    void* nominatedBefore, bool traceEnabled,
    VScreenPredicateTestResult* result) noexcept;

} // namespace edvr
