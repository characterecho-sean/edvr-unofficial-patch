#include "../../src/common/plugin_cost.h"
#include <stddef.h>

_Static_assert(sizeof(EdvrPluginCostOwnerV1) == 128, "owner report x64 layout changed");
_Static_assert(EDVR_PLUGIN_COST_OWNER_COUNT == 10u, "C owner count changed");
_Static_assert(EDVR_PLUGIN_COST_API_CLASS_COUNT == 5u, "C API class count changed");
_Static_assert(EDVR_PLUGIN_COST_MAX_SITE_ID == 127u, "C V1/CPU site mask range changed");
_Static_assert(EDVR_PLUGIN_COST_MAX_API_SITE_ID == 255u, "C V2 API site mask range changed");
_Static_assert(offsetof(EdvrPluginCostOwnerV1, cpuSiteMask) == 40, "owner CPU mask offset changed");
_Static_assert(offsetof(EdvrPluginCostOwnerV1, apiCalls) == 72, "owner API counts offset changed");
_Static_assert(offsetof(EdvrPluginCostOwnerV1, apiSiteMask) == 112, "owner API mask offset changed");
_Static_assert(sizeof(((EdvrPluginCostOwnerV1*)0)->apiCalls) == 40, "owner API class array changed");
_Static_assert(sizeof(EdvrPluginCostWindowV1) == 1328, "window report x64 layout changed");
_Static_assert(offsetof(EdvrPluginCostWindowV1, owners) == 48, "window owner array offset changed");
_Static_assert(sizeof(EdvrPluginCostOwnerV2) == 144, "owner V2 report x64 layout changed");
_Static_assert(offsetof(EdvrPluginCostOwnerV2, cpuSiteMask) == 40, "owner V2 CPU mask offset changed");
_Static_assert(offsetof(EdvrPluginCostOwnerV2, apiCalls) == 72, "owner V2 API counts offset changed");
_Static_assert(offsetof(EdvrPluginCostOwnerV2, apiSiteMask) == 112, "owner V2 API mask offset changed");
_Static_assert(sizeof(((EdvrPluginCostOwnerV2*)0)->cpuSiteMask) == 16, "owner V2 CPU mask stays two words");
_Static_assert(sizeof(((EdvrPluginCostOwnerV2*)0)->apiSiteMask) == 32, "owner V2 API mask is four words");
_Static_assert(sizeof(EdvrPluginCostWindowV2) == 1488, "window V2 report x64 layout changed");
_Static_assert(offsetof(EdvrPluginCostWindowV2, owners) == 48, "window V2 owner array offset changed");

typedef uint8_t (*EdvrPluginCostFrameBoundaryFn)(uint32_t, uint8_t, uint8_t, uint8_t, uint8_t,
                                                 EdvrPluginCostWindowV1*);
typedef uint8_t (*EdvrPluginCostFrameBoundaryV2Fn)(uint32_t, uint8_t, uint8_t, uint8_t, uint8_t,
                                                   EdvrPluginCostWindowV2*);
typedef uint8_t (*EdvrPluginCostApiSampleFrameFn)(void);
typedef void (*EdvrPluginCostSetOwnerContextFn)(void*);
typedef uint8_t (*EdvrPluginCostApiSampleContextFn)(const void*);
typedef uint8_t (*EdvrPluginCostApiSampleOwnerThreadFn)(void);

int plugin_cost_header_c_smoke(void) {
    EdvrPluginCostWindowV1 window = {0};
    EdvrPluginCostWindowV2 windowV2 = {0};
    EdvrPluginCostFrameBoundaryFn frameBoundary = &edvrPluginCostFrameBoundary;
    EdvrPluginCostFrameBoundaryV2Fn frameBoundaryV2 = &edvrPluginCostFrameBoundaryV2;
    EdvrPluginCostApiSampleFrameFn apiSampleFrame = &edvrPluginCostApiSampleFrame;
    EdvrPluginCostSetOwnerContextFn setOwnerContext = &edvrPluginCostSetOwnerContext;
    EdvrPluginCostApiSampleContextFn apiSampleContext = &edvrPluginCostApiSampleContext;
    EdvrPluginCostApiSampleOwnerThreadFn apiSampleOwnerThread = &edvrPluginCostApiSampleOwnerThread;
    int contextToken = 0;
    edvrPluginCostConfigure(1u, 1000000u);
    setOwnerContext(&contextToken);
    edvrPluginCostSetApiSampleFrame(1u);
    (void)apiSampleFrame();
    (void)apiSampleContext(&contextToken);
    (void)apiSampleOwnerThread();
    edvrPluginCostNoteSite(1u, 2u, 1u);
    edvrPluginCostNoteCpuTicks(1u, 2u, 3u);
    edvrPluginCostNoteD3dCall(1u, 2u, 3u);
    (void)frameBoundary(1u, 1u, 1u, 0u, 0u, &window);
    (void)frameBoundaryV2(2u, 1u, 1u, 0u, 0u, &windowV2);
    edvrPluginCostMarkTraceSuppressed();
    edvrPluginCostShutdown();
    return (int)(window.version + windowV2.version);
}
