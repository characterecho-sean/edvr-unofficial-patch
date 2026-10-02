#include "../../src/common/plugin_cost.h"
#include <stddef.h>

_Static_assert(sizeof(EdvrPluginCostOwnerV1) == 128, "owner report x64 layout changed");
_Static_assert(EDVR_PLUGIN_COST_OWNER_COUNT == 10u, "C owner count changed");
_Static_assert(EDVR_PLUGIN_COST_API_CLASS_COUNT == 5u, "C API class count changed");
_Static_assert(EDVR_PLUGIN_COST_MAX_SITE_ID == 127u, "C site mask range changed");
_Static_assert(offsetof(EdvrPluginCostOwnerV1, cpuSiteMask) == 40, "owner CPU mask offset changed");
_Static_assert(offsetof(EdvrPluginCostOwnerV1, apiCalls) == 72, "owner API counts offset changed");
_Static_assert(offsetof(EdvrPluginCostOwnerV1, apiSiteMask) == 112, "owner API mask offset changed");
_Static_assert(sizeof(((EdvrPluginCostOwnerV1*)0)->apiCalls) == 40, "owner API class array changed");
_Static_assert(sizeof(EdvrPluginCostWindowV1) == 1328, "window report x64 layout changed");
_Static_assert(offsetof(EdvrPluginCostWindowV1, owners) == 48, "window owner array offset changed");

typedef uint8_t (*EdvrPluginCostFrameBoundaryFn)(uint32_t, uint8_t, uint8_t, uint8_t, uint8_t,
                                                 EdvrPluginCostWindowV1*);
typedef uint8_t (*EdvrPluginCostApiSampleFrameFn)(void);
typedef void (*EdvrPluginCostSetOwnerContextFn)(void*);
typedef uint8_t (*EdvrPluginCostApiSampleContextFn)(const void*);

int plugin_cost_header_c_smoke(void) {
    EdvrPluginCostWindowV1 window = {0};
    EdvrPluginCostFrameBoundaryFn frameBoundary = &edvrPluginCostFrameBoundary;
    EdvrPluginCostApiSampleFrameFn apiSampleFrame = &edvrPluginCostApiSampleFrame;
    EdvrPluginCostSetOwnerContextFn setOwnerContext = &edvrPluginCostSetOwnerContext;
    EdvrPluginCostApiSampleContextFn apiSampleContext = &edvrPluginCostApiSampleContext;
    int contextToken = 0;
    edvrPluginCostConfigure(1u, 1000000u);
    setOwnerContext(&contextToken);
    edvrPluginCostSetApiSampleFrame(1u);
    (void)apiSampleFrame();
    (void)apiSampleContext(&contextToken);
    edvrPluginCostNoteSite(1u, 2u, 1u);
    edvrPluginCostNoteCpuTicks(1u, 2u, 3u);
    edvrPluginCostNoteD3dCall(1u, 2u, 3u);
    (void)frameBoundary(1u, 1u, 1u, 0u, 0u, &window);
    edvrPluginCostMarkTraceSuppressed();
    edvrPluginCostShutdown();
    return (int)window.version;
}
