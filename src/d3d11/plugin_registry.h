#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include "plugin_manifest.inc"
#include "draw_interest.h"

struct ID3D11DeviceContext;

// Private first-party plugin ABI. The records are C-compatible and contain
// only fixed-width values, pointers, and plain callbacks; no ownership crosses
// this boundary. The generated manifest supplies the stable plugin indices.
extern "C" {

typedef uint32_t (*EdvrPluginWantsDrawsFn)(void* state);
typedef uint32_t (*EdvrPluginStartupHooksWantedFn)(void* config);
typedef uint32_t (*EdvrPluginClaimDrawFn)(void* state, const char* claimId,
                                          uint8_t kind, uint32_t count, uint32_t instances);
// Optional trace-only source observation. The claim callback populates this
// during the same evaluation that returns its ordinary claim result.
struct EdvrPluginClaimObservation {
    uint8_t mode;
    uint8_t failedKnown;
    uint8_t failed;
    uint8_t observed;
};
typedef uint32_t (*EdvrPluginClaimDrawObservedFn)(
    void* state, const char* claimId, uint8_t kind, uint32_t count,
    uint32_t instances, EdvrPluginClaimObservation* observation);
typedef uint32_t (*EdvrPluginTraceModeFn)(void* state, uint8_t* mode);
typedef void (*EdvrPluginConfigureFn)(void* config);
typedef void (*EdvrPluginDrawFn)(void* state, const char* claimId,
                                 ID3D11DeviceContext* context);
typedef void (*EdvrPluginShutdownFn)(void* state);

struct EdvrPluginOps {
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
    // Appended so the existing callback offsets remain stable.
    EdvrPluginClaimDrawObservedFn claimDrawObserved;
    EdvrPluginTraceModeFn traceMode;
};
static_assert(offsetof(EdvrPluginOps, claimDrawObserved) >
              offsetof(EdvrPluginOps, shutdown),
              "trace-only callbacks must remain appended after the legacy ABI");

typedef uint32_t (*EdvrLegacyDrawGateFn)(void* state);
}

namespace edvr {

namespace detail {
extern std::atomic<uint64_t> g_pluginShaderCandidates;
extern std::atomic<draw_interest::InterestMask> g_legacyDrawInterestMask;
}

inline uint64_t pluginRegistryShaderCandidates() {
    return detail::g_pluginShaderCandidates.load(std::memory_order_relaxed);
}

// One lazily consumed snapshot for all ordered legacy interest-gated sites.
// Keep this separate from the night-vision plugin candidate mask so a legacy
// site cannot cause the plugin cache to be read before its original rung.
inline draw_interest::InterestMask pluginRegistryDrawInterestMask() {
    return detail::g_legacyDrawInterestMask.load(std::memory_order_relaxed);
}

inline bool pluginRegistryHasCandidate(uint32_t pluginIndex) {
    return pluginIndex < 64 &&
        (pluginRegistryShaderCandidates() & (uint64_t{1} << pluginIndex)) != 0;
}

enum : uint32_t {
    kPluginClaimNone = 0,
    kPluginClaimNightVision = 0x1001u,
    kCockpitVisualsPluginIndex = plugins::kPluginCockpitVisuals,
};

// Core-owned fixed-capacity registry. Calls that inspect subscribers happen
// at configure/frame boundaries or shader binds. The draw path only reads the
// cached candidate mask and visits claims named by that mask.
bool pluginRegistryRegister(const EdvrPluginOps* ops);
bool pluginRegistryProfileSupports(uint32_t manifestIndex);
bool pluginRegistryWantsStartupHooks(void* config);
bool pluginRegistryRegisterLegacyDrawGate(const char* stableName,
                                          EdvrLegacyDrawGateFn predicate,
                                          void* state);
void pluginRegistrySetDrawGateFailOpen();
void pluginRegistryConfigure(void* config);
void pluginRegistryShutdown();
void pluginRegistryOnShaderBind(uint64_t vsHash, uint64_t psHash);
void pluginRegistryRefreshShaderCandidates();
bool pluginRegistryConfigureDrawInterests(
    draw_interest::InterestMask configuredMask,
    const draw_interest::ShaderFilter* filters, size_t filterCount) noexcept;
void pluginRegistryReportActivity();
uint32_t pluginRegistryResolveDraw(uint64_t candidates, uint8_t kind,
                                   uint32_t count, uint32_t instances);
uint32_t pluginRegistryResolveDrawObserved(
    uint64_t candidates, uint8_t kind, uint32_t count, uint32_t instances,
    EdvrPluginClaimObservation* observation);
uint64_t pluginRegistryActivePluginMaskForTrace() noexcept;
bool pluginRegistryTraceMode(uint8_t* mode) noexcept;
void pluginRegistryBegin(uint32_t claim, ID3D11DeviceContext* context);
void pluginRegistryEnd(uint32_t claim, ID3D11DeviceContext* context);
bool pluginRegistryWantsDraws(void* legacyState);

// The pilot module publishes this ops record; later modules follow the same
// registration path while the manifest remains the source of stable indices.
const EdvrPluginOps* cockpitVisualsPluginOps();

}  // namespace edvr
