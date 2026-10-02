#pragma once

#include <stdint.h>

#define EDVR_PLUGIN_COST_OWNER_COUNT 10u
#define EDVR_PLUGIN_COST_API_CLASS_COUNT 5u
#define EDVR_PLUGIN_COST_MAX_SITE_ID 127u

// Collector configuration, frame-boundary calls, and all note calls are
// render-owner-thread-only; this API is deliberately lock-free and does not
// filter observations by module selection. Coverage is the explicitly
// annotated first slice of classifier handlers and ID3D11DeviceContext calls,
// not a complete per-plugin/module cost total.

// Versioned, fixed-layout result returned at a completed cost-report window.
// These structs contain no pointers so tests and log/report callers can copy
// them without retaining collector storage.
typedef struct EdvrPluginCostOwnerV1 {
    uint8_t owner;
    uint8_t cpuObserved; // At least one timed handler scope was measured.
    uint8_t apiObserved; // At least one annotated context call was observed.
    uint8_t reserved;
    uint64_t cpuTimedScopes;
    uint64_t cpuReached;
    uint64_t cpuInvoked;
    uint64_t cpuNotEligible;
    uint64_t cpuSiteMask[2];
    // Includes sampled QPC probe/branch overhead and only the explicitly
    // timed classifier handler intervals; not a calibrated whole-plugin cost.
    double cpuMeanMs;
    double cpuStdDevMs;
    uint64_t apiCalls[EDVR_PLUGIN_COST_API_CLASS_COUNT]; // Work, Transfer, State, ReadQuery, Instrumentation.
    uint64_t apiSiteMask[2];
} EdvrPluginCostOwnerV1;

typedef struct EdvrPluginCostWindowV1 {
    uint32_t version;
    uint32_t profileBit;
    uint32_t firstFrame;
    uint32_t lastFrame;
    uint32_t windowFrames;
    uint32_t reserved;
    uint64_t completedCpuSampleFrames;
    uint64_t completedApiSampleFrames;
    uint64_t cpuTraceSuppressedFrames;
    EdvrPluginCostOwnerV1 owners[EDVR_PLUGIN_COST_OWNER_COUNT];
} EdvrPluginCostWindowV1;

#ifdef __cplusplus
extern "C" {
#define EDVR_PLUGIN_COST_NOEXCEPT noexcept
#else
#define EDVR_PLUGIN_COST_NOEXCEPT
#endif
void edvrPluginCostConfigure(uint8_t profileBit, uint64_t qpcFrequency) EDVR_PLUGIN_COST_NOEXCEPT;
void edvrPluginCostShutdown(void) EDVR_PLUGIN_COST_NOEXCEPT;
// Cold hook lifecycle. The vScreen owner registers its already-known
// immediate context after hook commit and clears it after unhook. This does
// not query D3D or enable sampling by itself.
void edvrPluginCostSetOwnerContext(void* context) EDVR_PLUGIN_COST_NOEXCEPT;
// Returns true only on the render-owner thread, for its registered immediate
// context, during an enabled API-sample frame.
uint8_t edvrPluginCostApiSampleContext(const void* context) EDVR_PLUGIN_COST_NOEXCEPT;
void edvrPluginCostNoteSite(uint8_t owner, uint16_t siteId, uint8_t event) EDVR_PLUGIN_COST_NOEXCEPT;
void edvrPluginCostNoteCpuTicks(uint8_t owner, uint16_t siteId, uint64_t ticks) EDVR_PLUGIN_COST_NOEXCEPT;
void edvrPluginCostNoteD3dCall(uint8_t owner, uint16_t siteId, uint8_t apiClass) EDVR_PLUGIN_COST_NOEXCEPT;
void edvrPluginCostSetApiSampleFrame(uint8_t enabled) EDVR_PLUGIN_COST_NOEXCEPT;
uint8_t edvrPluginCostApiSampleFrame(void) EDVR_PLUGIN_COST_NOEXCEPT;
void edvrPluginCostMarkTraceSuppressed(void) EDVR_PLUGIN_COST_NOEXCEPT;
uint8_t edvrPluginCostFrameBoundary(uint32_t frameNo,
                                    uint8_t closedCpuSampleFrame,
                                    uint8_t closedApiSampleFrame,
                                    uint8_t nextApiSampleFrame,
                                    uint8_t traceSuppressed,
                                    EdvrPluginCostWindowV1* out) EDVR_PLUGIN_COST_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#undef EDVR_PLUGIN_COST_NOEXCEPT

#ifdef __cplusplus
#include <cstdint>
#include "log.h"
#include "plugin_manifest.inc"

namespace edvr { namespace plugin_cost {

enum class Owner : uint8_t {
    TemporalAa = static_cast<uint8_t>(plugins::kPluginTemporalAa),
    CockpitVisuals = static_cast<uint8_t>(plugins::kPluginCockpitVisuals),
    Exposure = static_cast<uint8_t>(plugins::kPluginExposure),
    Scanners = static_cast<uint8_t>(plugins::kPluginScanners),
    Intro = static_cast<uint8_t>(plugins::kPluginIntro),
    OnFootPanel = static_cast<uint8_t>(plugins::kPluginOnFootPanel),
    Comfort = static_cast<uint8_t>(plugins::kPluginComfort),
    Performance = static_cast<uint8_t>(plugins::kPluginPerformance),
    Diagnostics = static_cast<uint8_t>(plugins::kPluginDiagnostics),
    Core = static_cast<uint8_t>(plugins::kPluginIndexCount),
    Count = static_cast<uint8_t>(plugins::kPluginIndexCount + 1)
};

enum class SiteEvent : uint8_t {
    Reached = 1,
    Invoked = 2,
    NotEligible = 3
};

enum class ApiClass : uint8_t {
    Work = 0,
    Transfer = 1,
    State = 2,
    ReadQuery = 3,
    Instrumentation = 4,
    Count = 5
};

constexpr uint16_t kMaxSiteId = EDVR_PLUGIN_COST_MAX_SITE_ID;
constexpr uint8_t kOwnerCount = EDVR_PLUGIN_COST_OWNER_COUNT;
constexpr uint8_t kApiClassCount = EDVR_PLUGIN_COST_API_CLASS_COUNT;
constexpr uint32_t kWindowVersion = 1;
constexpr uint32_t kWindowFrameCount = 1800;
constexpr uint32_t kCpuDrawStride = 64;

// Production clock and sink are static policies: no runtime callback or
// virtual dispatch. Tests substitute their own Clock/Sink template arguments.
struct QpcClock {
    static int64_t now() noexcept { return qpcNow(); }
};

struct LiveSink {
    static void site(Owner owner, uint16_t siteId, SiteEvent event) noexcept {
        edvrPluginCostNoteSite(static_cast<uint8_t>(owner), siteId,
                               static_cast<uint8_t>(event));
    }
    static void ticks(Owner owner, uint16_t siteId, uint64_t value) noexcept {
        edvrPluginCostNoteCpuTicks(static_cast<uint8_t>(owner), siteId, value);
    }
};

struct NoCpu {
    static constexpr bool enabled = false;
    template<Owner O, uint16_t Site>
    static void note(SiteEvent) noexcept {}
    template<Owner O, uint16_t Site>
    class Scope {
    public:
        Scope() noexcept = default;
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
};

template<class Clock = QpcClock, class Sink = LiveSink>
struct SampledCpu {
    static constexpr bool enabled = true;

    template<Owner O, uint16_t Site>
    static void note(SiteEvent event) noexcept {
        Sink::site(O, Site, event);
    }

    template<Owner O, uint16_t Site>
    class Scope {
    public:
        Scope() noexcept : start_(Clock::now()) {}
        ~Scope() noexcept {
            const int64_t end = Clock::now();
            if (end >= start_) Sink::ticks(O, Site, static_cast<uint64_t>(end - start_));
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        int64_t start_;
    };
};

static_assert(plugins::kPluginIndexCount == 9, "Cost owner indices must cover the canonical nine plugins.");
static_assert(static_cast<uint8_t>(Owner::Count) == kOwnerCount, "Cost owner count must include Core after all plugins.");
static_assert(static_cast<uint8_t>(ApiClass::Count) == kApiClassCount, "Cost API class count changed without ABI review.");
static_assert(kMaxSiteId < 2 * 64, "Cost site IDs must fit two fixed coverage words.");
static_assert(sizeof(EdvrPluginCostOwnerV1) == 128, "Cost owner POD ABI changed.");
static_assert(sizeof(EdvrPluginCostWindowV1) == 1328, "Cost window POD ABI changed.");

}} // namespace edvr::plugin_cost
#endif
