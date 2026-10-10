#pragma once

#include "../../d3d11/plugin_registry.h"
#include "exposure_actions.h"

namespace edvr::plugins::exposure {

enum : uint32_t {
    kExposureLifecycleResetPairing = 1,
    kExposureLifecycleExpireVerdicts = 2,
};

// Build the immutable registry record in the core hook's existing State
// allocation. The callback state is the module-owned base subobject.
void initializeExposureLifecycleOps(EdvrPluginLifecycleOps* ops,
                                    ExposureActionState* state);

}  // namespace edvr::plugins::exposure
