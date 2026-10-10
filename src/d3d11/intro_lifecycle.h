#pragma once

#include "plugin_registry.h"

namespace edvr {

enum : uint32_t {
    kIntroLifecycleLoader = 1,
    kIntroLifecycleSplashDim = 2,
    kIntroLifecyclePanel = 3,
    kIntroLifecycleCurve = 4,
    kIntroLifecycleBackdrop = 5,
};

// Intro lifecycle callbacks are owned by the plugin registry while
// VScreen owns the module's configure, frame, and shutdown callsites.
const EdvrPluginLifecycleOps* introVideoLifecycleOps();

}  // namespace edvr
