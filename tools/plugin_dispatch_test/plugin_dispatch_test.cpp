#include "../../src/d3d11/plugin_dispatch.h"
#include "../../src/d3d11/plugin_registry.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/log.h"

#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <type_traits>

namespace edvr { uint32_t loggerNoteCalls = 0; }

namespace {
constexpr uint32_t kPlugin = edvr::plugins::kPluginCockpitVisuals;
constexpr uint32_t kNightVisionClaim = 0x1001u;
constexpr uint32_t kEarlierClaim = 0x2001u;
constexpr uint64_t kVs = 0xFCF7BD2896751D96ull;
constexpr uint64_t kPs = 0xF786D34B5E118D5Eull;
constexpr uint64_t kActive = uint64_t{1} << kPlugin;
constexpr uint64_t kLegacyVs = 0x1122334455667788ull;
constexpr auto kManifestNightVisionClaim = edvr::plugins::kManifest[kPlugin].claims[
    edvr::plugins::kClaimCockpitVisualsNightVision];
constexpr edvr::plugins::dispatch::ShaderClaimKey kClaims[] = {
    {kPlugin, kManifestNightVisionClaim.id,
     kManifestNightVisionClaim.shaderPairs[0].vertexShaderHash,
     kManifestNightVisionClaim.shaderPairs[0].pixelShaderHash},
};
uint32_t checks = 0;

// Frozen public prefix from before trace-only callbacks were appended.
struct LegacyEdvrPluginOpsPrefix final {
    uint32_t structSize;
    uint32_t manifestIndex;
    const char* manifestId;
    const char* drawGateName;
    const char* const* claimIds;
    uint32_t claimCount;
    void* state;
    EdvrPluginConfigureFn configure;
    EdvrPluginWantsDrawsFn wantsDraws;
    EdvrPluginStartupHooksWantedFn startupHooksWanted;
    EdvrPluginClaimDrawFn claimDraw;
    EdvrPluginDrawFn begin;
    EdvrPluginDrawFn end;
    EdvrPluginShutdownFn shutdown;
};
static_assert(std::is_standard_layout<EdvrPluginOps>::value,
              "plugin ops ABI must remain standard-layout");
static_assert(sizeof(LegacyEdvrPluginOpsPrefix) ==
                  offsetof(EdvrPluginOps, claimDrawObserved),
              "trace-only callbacks must append after the exact legacy ops prefix");
static_assert(offsetof(EdvrPluginOps, shutdown) ==
                  offsetof(LegacyEdvrPluginOpsPrefix, shutdown),
              "legacy shutdown callback offset must remain stable");
static_assert(std::is_standard_layout<EdvrPluginLifecycleOps>::value,
              "lifecycle ops ABI must remain standard-layout");

struct RegistryState {
    bool wants = false;
    bool legacyWants = false;
    uint32_t configureCalls = 0;
    uint32_t claimCalls = 0;
    uint32_t beginCalls = 0;
    uint32_t endCalls = 0;
    uint32_t shutdownCalls = 0;
    uint32_t observedCalls = 0;
    uint8_t traceMode = 3;
    bool traceModeAvailable = true;
    bool failed = false;
};

struct LifecycleState {
    uint32_t configureCalls = 0;
    uint32_t frameCalls = 0;
    uint32_t shutdownCalls = 0;
    uint32_t lastFrame = 0;
    bool configured = false;
    bool reenterShutdown = false;
    uint32_t manifestIndex = 0;
};
LifecycleState lifecycleState;
RegistryState registryState;
const char* const kRegistryClaimIds[] = {"night-vision"};
const char* const kNullRegistryClaimIds[] = {nullptr};
const char* const kDuplicateRegistryClaimIds[] = {"night-vision", "night-vision"};

void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        std::printf("FAIL: %s\n", message);
        std::exit(1);
    }
}

uint32_t registryWants(void* state) {
    return static_cast<RegistryState*>(state)->wants ? 1u : 0u;
}
uint32_t registryStartupHooksWanted(void* config) {
    return config && *static_cast<const bool*>(config) ? 1u : 0u;
}
void registryConfigure(void* config) {
    ++registryState.configureCalls;
    registryState.wants = config && *static_cast<const bool*>(config);
}
uint32_t registryClaim(void* state, const char* claimId, uint8_t kind,
                       uint32_t count, uint32_t instances) {
    auto* s = static_cast<RegistryState*>(state);
    ++s->claimCalls;
    return claimId && std::strcmp(claimId, "night-vision") == 0 &&
                   edvr::plugins::dispatch::matchesShape(
                       kManifestNightVisionClaim.drawShape, kind, count, instances)
               ? kNightVisionClaim : 0;
}
void registryBegin(void* state, const char* claimId, ID3D11DeviceContext*) {
    if (claimId && std::strcmp(claimId, "night-vision") == 0)
        ++static_cast<RegistryState*>(state)->beginCalls;
}
void registryEnd(void* state, const char* claimId, ID3D11DeviceContext*) {
    if (claimId && std::strcmp(claimId, "night-vision") == 0)
        ++static_cast<RegistryState*>(state)->endCalls;
}
void registryShutdown(void* state) {
    ++static_cast<RegistryState*>(state)->shutdownCalls;
}
uint32_t registryClaimObserved(void* state, const char* claimId, uint8_t kind,
                               uint32_t count, uint32_t instances,
                               EdvrPluginClaimObservation* observation) {
    auto* s = static_cast<RegistryState*>(state);
    ++s->observedCalls;
    if (observation) {
        observation->mode = s->traceMode;
        observation->failedKnown = s->traceMode ? 1u : 0u;
        observation->failed = s->traceMode && s->failed ? 1u : 0u;
    }
    return registryClaim(state, claimId, kind, count, instances);
}
uint32_t registryTraceMode(void* state, uint8_t* mode) {
    auto* s = static_cast<RegistryState*>(state);
    if (!mode || !s->traceModeAvailable) return 0;
    *mode = s->traceMode;
    return 1;
}
uint32_t legacyGate(void* state) {
    return static_cast<RegistryState*>(state)->legacyWants ? 1u : 0u;
}

EdvrPluginOps registryOps() {
    return {sizeof(EdvrPluginOps), edvr::plugins::kPluginCockpitVisuals,
            "cockpit-visuals", "test.cockpit-visuals", kRegistryClaimIds, 1,
            &registryState, &registryConfigure, &registryWants,
            &registryStartupHooksWanted, &registryClaim,
            &registryBegin, &registryEnd, &registryShutdown,
            &registryClaimObserved, &registryTraceMode};
}

void lifecycleConfigure(void* config) {
    ++lifecycleState.configureCalls;
    lifecycleState.configured = config && *static_cast<const bool*>(config);
}
void lifecycleFrame(void* state, uint32_t sceneFrame) {
    auto* s = static_cast<LifecycleState*>(state);
    ++s->frameCalls;
    s->lastFrame = sceneFrame;
}
void lifecycleShutdown(void* state) {
    auto* s = static_cast<LifecycleState*>(state);
    ++s->shutdownCalls;
    if (s->reenterShutdown)
        edvr::pluginRegistryShutdownLifecycle(s->manifestIndex);
}
EdvrPluginLifecycleOps introLifecycleOps() {
    return {sizeof(EdvrPluginLifecycleOps), edvr::plugins::kPluginIntro,
            "intro", &lifecycleState, &lifecycleConfigure, &lifecycleFrame,
            &lifecycleShutdown};
}

bool oldShape(char kind, uint32_t count, uint32_t instances) {
    return kind == 'X' && count == 240 && instances == 1;
}

// Frozen pre-registry behavior at the old night-vision rung. An earlier
// verdict preempts this rung because beginPanelOverride returns at its first
// claim; otherwise the old predicate checked enabled/failure/shape/hash.
uint32_t legacyDecision(uint32_t earlierClaim, char kind, uint32_t count,
                        uint32_t instances, uint64_t vs, uint64_t ps,
                        bool pulse, bool realistic, bool failed) {
    if (earlierClaim) return earlierClaim;
    return (pulse || realistic) && !failed && oldShape(kind, count, instances) &&
                   vs == kVs && ps == kPs
               ? kNightVisionClaim
               : 0;
}

uint32_t tableDecision(uint32_t earlierClaim, char kind, uint32_t count,
                       uint32_t instances, uint64_t candidates,
                       bool pulse, bool realistic, bool failed,
                       uint32_t* shapeCallbacks, uint32_t* claimCallbacks) {
    if (earlierClaim) return earlierClaim;
    return edvr::plugins::dispatch::resolveCandidate(
        candidates, kPlugin, [&]() {
            ++*shapeCallbacks;
            return edvr::plugins::dispatch::matchesShape(
                kManifestNightVisionClaim.drawShape, static_cast<uint8_t>(kind), count, instances);
        }, [&]() {
            ++*claimCallbacks;
            return (pulse || realistic) && !failed ? kNightVisionClaim : 0;
        });
}

void replayMatrix() {
    const char kinds[] = {'X', 'D'};
    const uint32_t counts[] = {240, 6};
    const uint32_t instances[] = {1, 2};
    const uint64_t shaders[][2] = {{kVs, kPs}, {kVs, 0}, {0, kPs}};
    for (bool pulse : {false, true})
        for (bool realistic : {false, true})
            for (bool failed : {false, true})
                for (size_t shape = 0; shape < 2; ++shape)
                    for (const auto& pair : shaders)
                        for (uint32_t earlier : {0u, kEarlierClaim}) {
                            size_t bindChecks = 0;
                            uint32_t shapeCallbacks = 0;
                            uint32_t callbacks = 0;
                            const uint64_t active = (pulse || realistic) ? kActive : 0;
                            const auto candidateMask = edvr::plugins::dispatch::candidatePlugins(
                                pair[0], pair[1], active, kClaims,
                                sizeof(kClaims) / sizeof(kClaims[0]), &bindChecks);
                            const auto old = legacyDecision(earlier, kinds[shape],
                                counts[shape], instances[shape], pair[0], pair[1],
                                pulse, realistic, failed);
                            const auto table = tableDecision(earlier, kinds[shape],
                                counts[shape], instances[shape], candidateMask,
                                pulse, realistic, failed, &shapeCallbacks, &callbacks);
                            check(old == table, "candidate-table replay matches frozen legacy verdict");
                            check(bindChecks == (active ? 1u : 0u),
                                  "shader-pair work occurs only for active plugins at bind/config");
                            const bool reachesCandidate = earlier == 0 && active &&
                                edvr::plugins::dispatch::matchesShape(
                                    kManifestNightVisionClaim.drawShape,
                                    static_cast<uint8_t>(kinds[shape]),
                                    counts[shape], instances[shape]) &&
                                pair[0] == kVs && pair[1] == kPs;
                            check(callbacks == (reachesCandidate ? 1u : 0u),
                                  "per-draw claim callback runs only for a candidate shape");
                            check(shapeCallbacks == (earlier == 0 && active &&
                                  pair[0] == kVs && pair[1] == kPs ? 1u : 0u),
                                  "uninterested/earlier-claimed draws skip shape work");
                        }
}

void costBudgetMetadata() {
    check(edvr::plugins::kPluginCount == 9 &&
              edvr::plugins::kPluginIndexCount == edvr::plugins::kPluginCount,
          "budget metadata remains attached to the complete stable catalog");
    for (uint32_t i = 0; i < edvr::plugins::kPluginCount; ++i) {
        const auto& plugin = edvr::plugins::kManifest[i];
        const auto* budget = plugin.costBudget;
        check(plugin.costNote && *plugin.costNote && budget &&
                  budget->state == edvr::plugins::kCostBudgetUnmeasured &&
                  budget->reference == nullptr && budget->metrics && budget->metricCount == 3,
              "all plugins retain costNote and an unmeasured, reference-free cost budget");
        if (!budget || !budget->metrics || budget->metricCount != 3) continue;
        constexpr const char* expectedMetrics[] = {"cpu", "issuedGpuWork", "directGpu"};
        bool metricsHonest = true;
        for (uint32_t j = 0; j < 3; ++j) {
            const auto& metric = budget->metrics[j];
            metricsHonest &= metric.id && std::strcmp(metric.id, expectedMetrics[j]) == 0 &&
                metric.state == edvr::plugins::kCostMetricUnmeasured &&
                metric.unit && *metric.unit && metric.coverage && *metric.coverage &&
                metric.referenceValue == nullptr && metric.maxRelativeIncrease == nullptr;
        }
        check(metricsHonest,
              "unmeasured CPU, issued-work, and direct-GPU metrics carry scope without numeric values");
    }
}

void disabledAndCacheCases() {
    size_t pairChecks = 0;
    uint32_t callbacks = 0;
    const uint64_t disabled = edvr::plugins::dispatch::candidatePlugins(
        kVs, kPs, 0, kClaims, sizeof(kClaims) / sizeof(kClaims[0]), &pairChecks);
    const uint32_t result = edvr::plugins::dispatch::resolveCandidate(
        disabled, kPlugin, [&]() { ++callbacks; return true; },
        [&]() { return kNightVisionClaim; });
    check(pairChecks == 0, "disabled plugin has no shader-pair comparison");
    check(disabled == 0 && result == 0 && callbacks == 0,
          "disabled plugin has no candidate and receives no draw callback");

    pairChecks = 0;
    const auto match = edvr::plugins::dispatch::candidatePlugins(
        kVs, kPs, kActive, kClaims, sizeof(kClaims) / sizeof(kClaims[0]), &pairChecks);
    check(edvr::plugins::dispatch::hasCandidate(match, kPlugin),
          "matching bound shader pair is cached as a candidate");
    check(!edvr::plugins::dispatch::hasCandidate(
              edvr::plugins::dispatch::candidatePlugins(kVs, 0, kActive, kClaims,
                  sizeof(kClaims) / sizeof(kClaims[0])), kPlugin),
          "a pixel-shader bind change invalidates the candidate pair");
    uint32_t shapeCalls = 0;
    uint32_t resolverCalls = 0;
    const uint32_t noShape = edvr::plugins::dispatch::resolveCandidate(
        match, kPlugin, [&]() { ++shapeCalls; return false; },
        [&]() { ++resolverCalls; return kNightVisionClaim; });
    check(noShape == 0 && shapeCalls == 1 && resolverCalls == 0,
          "inline shape guard evaluates the shape but skips the registry resolver when it fails");
    check(kManifestNightVisionClaim.drawShape.kind == static_cast<uint8_t>('X') &&
              kManifestNightVisionClaim.drawShape.count == 240 &&
              kManifestNightVisionClaim.drawShape.instances == 1 &&
              kClaims[0].vsHash == kVs && kClaims[0].psHash == kPs,
          "generated claim metadata matches the frozen legacy shader and draw contract");
}

void registryLifecycle() {
    edvr::pluginRegistryShutdown();
    registryState = {};
    uint8_t unregisteredMode = 0xff;
    check(!edvr::pluginRegistryTraceMode(&unregisteredMode) &&
              unregisteredMode == 0xff,
          "unregistered module leaves raw trace mode unavailable");
    EdvrPluginOps ops = registryOps();
    EdvrPluginOps invalid = ops;
    invalid.structSize = 0;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects malformed callback record");
    invalid = ops;
    invalid.claimIds = kNullRegistryClaimIds;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects null claim IDs");
    invalid = ops;
    invalid.claimIds = kDuplicateRegistryClaimIds;
    invalid.claimCount = 2;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects duplicate claim IDs");
    invalid = ops;
    invalid.drawGateName = "";
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects empty gate subscription names");
    invalid = ops;
    invalid.end = nullptr;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects a claimed draw without paired End");
    invalid = ops;
    invalid.startupHooksWanted = nullptr;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects modules without startup hook demand callback");
    const auto savedProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(!edvr::pluginRegistryProfileSupports(edvr::plugins::kPluginCockpitVisuals) &&
              !edvr::pluginRegistryRegister(&ops),
          "VR-only module has no registration or shader interest in flat profile");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(1), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(2), kPs);
    bool unsupportedOn = true;
    check(edvr::pluginRegistryShaderCandidates() == 0 &&
              !edvr::pluginRegistryWantsStartupHooks(&unsupportedOn),
          "flat profile has no candidate mask or startup hook demand");
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Invalid;
    check(!edvr::pluginRegistryProfileSupports(edvr::plugins::kPluginCockpitVisuals),
          "invalid install descriptor disables profile-scoped plugin work");
    edvr::g_runtimeProfile = savedProfile;
    check(edvr::pluginRegistryProfileSupports(edvr::plugins::kPluginCockpitVisuals),
          "legacy VR profile supports the pilot module");
    check(edvr::pluginRegistryRegister(&ops), "registry accepts only the pilot manifest module");
    bool startupOn = true;
    check(edvr::pluginRegistryWantsStartupHooks(&startupOn),
          "enabled pilot alone requests the core hook installation");
    startupOn = false;
    check(!edvr::pluginRegistryWantsStartupHooks(&startupOn),
          "disabled pilot does not keep otherwise-unused hooks installed");
    check(!edvr::pluginRegistryRegister(&ops), "registry rejects duplicate plugin registration");
    check(edvr::pluginRegistryRegisterLegacyDrawGate("legacy.test", &legacyGate, &registryState),
          "registry accepts a stable named legacy subscription");
    check(!edvr::pluginRegistryRegisterLegacyDrawGate("legacy.test", &legacyGate, &registryState),
          "registry rejects duplicate subscription names");

    bool off = false;
    const uint32_t noteBaseline = edvr::loggerNoteCalls;
    edvr::pluginRegistryConfigure(&off);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(1), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(2), kPs);
    check(edvr::loggerNoteCalls == noteBaseline,
          "off shader binds and configure do not take the logger path");
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "off-period canonical shader binds do not create plugin candidates");
    check(!edvr::pluginRegistryWantsDraws(&registryState),
          "named and plugin subscriptions both report off");
    uint8_t rawMode = 0xff;
    check(edvr::pluginRegistryTraceMode(&rawMode) && rawMode == registryState.traceMode,
          "raw trace mode remains available while the cached candidate mask is empty");
    EdvrPluginClaimObservation noCandidateObservation{};
    noCandidateObservation.mode = 0xff;
    const uint32_t noCandidateObserverCalls = registryState.observedCalls;
    check(edvr::pluginRegistryResolveDrawObserved(0, 'X', 240, 1,
                  &noCandidateObservation) == edvr::kPluginClaimNone &&
              registryState.observedCalls == noCandidateObserverCalls,
          "candidate miss skips the optional observer callback");

    using edvr::draw_interest::InterestId;
    using edvr::draw_interest::InterestMask;
    using edvr::draw_interest::HashFilter;
    constexpr InterestMask kTargetSharp = edvr::draw_interest::bit(InterestId::TargetSharp);
    constexpr InterestMask kIntroCurve = edvr::draw_interest::bit(InterestId::IntroCurveObserve);
    const edvr::draw_interest::ShaderFilter legacyFilters[] = {
        {InterestId::TargetSharp, HashFilter::Vertex, kLegacyVs, 0},
    };
    check(edvr::pluginRegistryConfigureDrawInterests(
              kTargetSharp | kIntroCurve, legacyFilters, 1),
          "configured VR legacy interest accepts bounded module-owned shader metadata");
    check(edvr::pluginRegistryShaderCandidates() == 0 &&
              edvr::pluginRegistryDrawInterestMask() == kIntroCurve,
          "legacy bind-interest is published separately and known shader mismatch is filtered");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(3), kLegacyVs);
    check(edvr::pluginRegistryDrawInterestMask() == (kTargetSharp | kIntroCurve),
          "legacy match refreshes on bind while config-only curve interest remains active");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(4), kLegacyVs + 1);
    check(edvr::pluginRegistryDrawInterestMask() == kIntroCurve,
          "known VS mismatch filters only its configured interest");
    edvr::bindingSetShader(edvr::BindSlot::Vs, nullptr, 0);
    check(edvr::pluginRegistryDrawInterestMask() == (kTargetSharp | kIntroCurve),
          "unknown VS hash conservatively keeps legacy fallback reachable");

    const auto vrProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(edvr::pluginRegistryConfigureDrawInterests(
              kTargetSharp | kIntroCurve, legacyFilters, 1) &&
              edvr::pluginRegistryDrawInterestMask() == 0,
          "flat profile does not publish VR-only legacy shader interest");
    edvr::g_runtimeProfile = vrProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Invalid;
    check(edvr::pluginRegistryConfigureDrawInterests(
              kTargetSharp | kIntroCurve, legacyFilters, 1) &&
              edvr::pluginRegistryDrawInterestMask() == 0,
          "invalid runtime profile clears legacy shader interest and observer demand");
    edvr::g_runtimeProfile = vrProfile;
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(5), kLegacyVs);
    check(edvr::pluginRegistryConfigureDrawInterests(
              kTargetSharp | kIntroCurve, legacyFilters, 1) &&
              edvr::pluginRegistryDrawInterestMask() == (kTargetSharp | kIntroCurve),
          "VR configure reseeds current shader pair without requiring a new bind");
    edvr::bindingForgetAll();
    check(edvr::pluginRegistryShaderCandidates() == 0 &&
              edvr::pluginRegistryDrawInterestMask() == (kTargetSharp | kIntroCurve),
          "ClearState removes plugin candidates while unknown legacy pair keeps fallback");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(6), kLegacyVs);
    check(edvr::pluginRegistryDrawInterestMask() == (kTargetSharp | kIntroCurve),
          "canonical shader repair republishes the matching legacy interest");
    check(edvr::pluginRegistryConfigureDrawInterests(0, nullptr, 0) &&
              edvr::pluginRegistryDrawInterestMask() == 0,
          "disabling legacy config interest clears the cached mask");

    // Exercise plugin seeding independently after the legacy observer has been
    // removed and the canonical shadow changes quietly.
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(7), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(8), kPs);
    registryState.legacyWants = true;
    check(edvr::pluginRegistryWantsDraws(&registryState),
          "named legacy subscriber opens the gate independently of the plugin");
    registryState.legacyWants = false;
    bool on = true;
    edvr::pluginRegistryConfigure(&on);
    registryState.wants = false;
    edvr::pluginRegistryRefreshShaderCandidates();
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "refresh recomputes active plugin interest instead of using a stale mask");
    registryState.wants = true;
    edvr::pluginRegistryRefreshShaderCandidates();
    check(edvr::pluginRegistryWantsDraws(&registryState),
          "plugin subscription opens the gate independently of legacy consumers");
    check(edvr::pluginRegistryShaderCandidates() ==
              (uint64_t{1} << edvr::plugins::kPluginCockpitVisuals),
          "config-on seeds a candidate from the already-bound canonical pair");
    const uint64_t candidates = edvr::pluginRegistryShaderCandidates();
    EdvrPluginClaimObservation observation{};
    const auto claim = edvr::plugins::dispatch::resolveCandidate(
        candidates, edvr::plugins::kPluginCockpitVisuals, [&] {
            return edvr::plugins::dispatch::matchesShape(
                kManifestNightVisionClaim.drawShape, 'X', 240, 1);
        }, [&] {
            return edvr::pluginRegistryResolveDrawObserved(
                candidates, 'X', 240, 1, &observation);
        });
    check(claim == kNightVisionClaim && registryState.claimCalls == 1 &&
              registryState.observedCalls == 1,
          "positive candidate invokes the observed claim callback exactly once");
    check(observation.observed == 1 && observation.mode == registryState.traceMode &&
              observation.failedKnown == 1 && observation.failed == 0,
          "registry marks a consumed successful observation as known source facts");
    check(edvr::loggerNoteCalls == noteBaseline,
          "candidate shader bind and draw resolution only queue breadcrumbs");
    edvr::pluginRegistryReportActivity();
    check(edvr::loggerNoteCalls == noteBaseline + 2,
          "frame-boundary activity report drains both one-shot breadcrumbs");
    edvr::pluginRegistryReportActivity();
    check(edvr::loggerNoteCalls == noteBaseline + 2,
          "drained activity breadcrumbs do not log twice");
    (void)edvr::plugins::dispatch::resolveCandidate(
        candidates, edvr::plugins::kPluginCockpitVisuals, [] { return true; },
        [&] { return edvr::pluginRegistryResolveDraw(candidates, 'X', 240, 1); });
    edvr::pluginRegistryReportActivity();
    check(edvr::loggerNoteCalls == noteBaseline + 2,
          "later candidate draws do not re-enter the logger");
    edvr::pluginRegistryBegin(claim, nullptr);
    edvr::pluginRegistryEnd(claim, nullptr);
    check(registryState.beginCalls == 1 && registryState.endCalls == 1,
          "registry routes matched begin and end callbacks to the owning module");

    edvr::bindingForgetAll();
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "canonical ClearState/unbind observer drops the cached shader candidate");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(3), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(4), kPs);
    check(edvr::pluginRegistryShaderCandidates() ==
              (uint64_t{1} << edvr::plugins::kPluginCockpitVisuals),
          "active canonical shader repair republishes a matching candidate");
    edvr::pluginRegistrySetDrawGateFailOpen();
    off = false;
    edvr::pluginRegistryConfigure(&off);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(5), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(6), kPs);
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "configure-off removes the observer while shadow tracking continues");
    edvr::pluginRegistryConfigure(&on);
    check(edvr::pluginRegistryShaderCandidates() ==
              (uint64_t{1} << edvr::plugins::kPluginCockpitVisuals),
          "configure-on reseeds after silent off-period shader changes");
    off = false;
    edvr::pluginRegistryConfigure(&off);
    check(edvr::pluginRegistryWantsDraws(&registryState),
          "registration errors can force the aggregate gate open");
    edvr::pluginRegistryShutdown();
    check(registryState.shutdownCalls == 1 &&
              edvr::pluginRegistryShaderCandidates() == 0 &&
              !edvr::pluginRegistryWantsDraws(&registryState),
          "shutdown releases modules and clears dispatch/subscription caches");
    check(edvr::pluginRegistryRegister(&ops), "registry can restart after shutdown");
    const uint32_t shutdownNoteBaseline = edvr::loggerNoteCalls;
    edvr::pluginRegistryConfigure(&on);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(7), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(8), kPs);
    const uint64_t shutdownCandidates = edvr::pluginRegistryShaderCandidates();
    (void)edvr::plugins::dispatch::resolveCandidate(
        shutdownCandidates, edvr::plugins::kPluginCockpitVisuals,
        [] { return true; }, [&] {
            return edvr::pluginRegistryResolveDraw(shutdownCandidates, 'X', 240, 1);
        });
    check(edvr::loggerNoteCalls == shutdownNoteBaseline,
          "pending draw activity remains deferred until a safe reporting point");
    edvr::pluginRegistryShutdown();
    check(edvr::loggerNoteCalls == shutdownNoteBaseline + 2,
          "shutdown drains pending bind and draw breadcrumbs away from draw dispatch");

    // A legacy-prefix ops record remains valid when the optional observation
    // tail is absent. The normal claim may still resolve, but its source facts
    // remain explicitly unsupported rather than being fabricated.
    registryState = {};
    EdvrPluginOps noObserver = registryOps();
    noObserver.claimDrawObserved = nullptr;
    noObserver.traceMode = nullptr;
    check(edvr::pluginRegistryRegister(&noObserver),
          "registry accepts ops with optional observation callbacks absent");
    bool enabled = true;
    edvr::pluginRegistryConfigure(&enabled);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(9), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(10), kPs);
    uint8_t unsupportedMode = 0xff;
    check(!edvr::pluginRegistryTraceMode(&unsupportedMode) && unsupportedMode == 0xff,
          "missing trace-mode callback reports unavailable without fabricating a mode");
    const uint64_t fallbackCandidates = edvr::pluginRegistryShaderCandidates();
    EdvrPluginClaimObservation unsupportedObservation{};
    const uint32_t fallbackClaim = edvr::pluginRegistryResolveDrawObserved(
        fallbackCandidates, 'X', 240, 1, &unsupportedObservation);
    check(fallbackClaim == kNightVisionClaim && registryState.claimCalls == 1 &&
              registryState.observedCalls == 0,
          "missing observer falls back to the ordinary claim callback");
    check(unsupportedObservation.observed == 0 && unsupportedObservation.mode == 0 &&
              unsupportedObservation.failedKnown == 0 && unsupportedObservation.failed == 0,
          "ordinary fallback leaves trace-only observation explicitly unknown");
    edvr::pluginRegistryShutdown();
}

void lifecycleRegistryDispatch() {
    constexpr uint32_t kIntro = edvr::plugins::kPluginIntro;
    constexpr uint32_t kExposure = edvr::plugins::kPluginExposure;
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    lifecycleState = {};
    lifecycleState.manifestIndex = kIntro;
    registryState.legacyWants = false;
    check(edvr::pluginRegistryRegisterLegacyDrawGate(
              "lifecycle.test", &legacyGate, &registryState),
          "draw subscription fixture is registered before lifecycle dispatch");
    const bool drawSubscriptionBefore = edvr::pluginRegistryWantsDraws(&registryState);
    const uint64_t candidatesBefore = edvr::pluginRegistryShaderCandidates();
    const auto interestsBefore = edvr::pluginRegistryDrawInterestMask();
    EdvrPluginLifecycleOps ops = introLifecycleOps();

    EdvrPluginLifecycleOps invalid = ops;
    invalid.structSize = 0;
    check(!edvr::pluginRegistryRegisterLifecycle(&invalid),
          "lifecycle registry rejects a malformed callback record");
    invalid = ops;
    invalid.manifestId = "exposure";
    check(!edvr::pluginRegistryRegisterLifecycle(&invalid),
          "lifecycle registry rejects a mismatched stable manifest ID");
    invalid = ops;
    invalid.frame = nullptr;
    check(!edvr::pluginRegistryRegisterLifecycle(&invalid),
          "lifecycle registry requires configure, frame, and shutdown callbacks");
    EdvrPluginLifecycleOps catalogOnly = {
        sizeof(EdvrPluginLifecycleOps), kExposure, "exposure", &lifecycleState,
        &lifecycleConfigure, &lifecycleFrame, &lifecycleShutdown};
    check(!edvr::pluginRegistryRegisterLifecycle(&catalogOnly),
          "lifecycle registry rejects catalog-only implementations");

    const auto savedProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(!edvr::pluginRegistryRegisterLifecycle(&ops),
          "VR-only lifecycle module is rejected under the flat profile");
    edvr::g_runtimeProfile = savedProfile;

    check(edvr::pluginRegistryRegisterLifecycle(&ops),
          "lifecycle registry accepts the manifest lifecycle implementation");
    check(edvr::pluginRegistryRegisterLifecycle(&ops),
          "same lifecycle ops pointer is idempotent for install retry");
    EdvrPluginLifecycleOps conflict = ops;
    check(!edvr::pluginRegistryRegisterLifecycle(&conflict),
          "lifecycle registry rejects a different record for an occupied index");
    check(edvr::pluginRegistryHasLifecycle(kIntro),
          "registered lifecycle slot is visible by direct manifest index");

    bool enabled = true;
    edvr::pluginRegistryConfigureLifecycle(kIntro, &enabled);
    edvr::pluginRegistryFrameLifecycle(kIntro, 0x1234u);
    check(lifecycleState.configureCalls == 1 && lifecycleState.configured &&
              lifecycleState.frameCalls == 1 && lifecycleState.lastFrame == 0x1234u,
          "lifecycle dispatch delivers config and scene-frame value with module state");
    check(edvr::pluginRegistryShaderCandidates() == candidatesBefore &&
              edvr::pluginRegistryDrawInterestMask() == interestsBefore &&
              edvr::pluginRegistryWantsDraws(&registryState) == drawSubscriptionBefore,
          "lifecycle registration and dispatch do not change draw masks or subscriptions");

    edvr::pluginRegistryShutdown();
    check(edvr::pluginRegistryHasLifecycle(kIntro) &&
              lifecycleState.shutdownCalls == 0,
          "legacy draw shutdown leaves lifecycle slots and their lifetime alone");
    lifecycleState.reenterShutdown = true;
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    check(!edvr::pluginRegistryHasLifecycle(kIntro) &&
              lifecycleState.shutdownCalls == 1,
          "lifecycle shutdown detaches before callback and is reentrant/idempotent");

    const uint32_t configureBeforeAbsent = lifecycleState.configureCalls;
    const uint32_t frameBeforeAbsent = lifecycleState.frameCalls;
    edvr::pluginRegistryConfigureLifecycle(kIntro, nullptr);
    edvr::pluginRegistryFrameLifecycle(kIntro, 44);
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    check(lifecycleState.configureCalls == configureBeforeAbsent &&
              lifecycleState.frameCalls == frameBeforeAbsent &&
              lifecycleState.shutdownCalls == 1,
          "absent lifecycle entries receive no callbacks");

    edvr::g_runtimeProfile = edvr::RuntimeProfile::Invalid;
    check(!edvr::pluginRegistryRegisterLifecycle(&ops),
          "invalid runtime profile rejects lifecycle registration");
    edvr::g_runtimeProfile = savedProfile;
}
}

namespace edvr {
Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char*, ...) { ++loggerNoteCalls; }
void breadcrumb(const char*) {}
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("Usage: plugin_dispatch_test --self-test");
        return 2;
    }
    replayMatrix();
    costBudgetMetadata();
    disabledAndCacheCases();
    registryLifecycle();
    lifecycleRegistryDispatch();
    std::printf("PASS: plugin dispatch (%u checks)\n", checks);
    return 0;
}
