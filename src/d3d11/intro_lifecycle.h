#pragma once

#include "plugin_registry.h"

namespace edvr {

// Intro video lifecycle callbacks are owned by the plugin registry while
// VScreen owns the module's configure, frame, and shutdown callsites.
const EdvrPluginLifecycleOps* introVideoLifecycleOps();

}  // namespace edvr
