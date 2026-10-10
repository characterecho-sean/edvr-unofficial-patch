#include "../../d3d11/intro_lifecycle.h"

#include "../../common/config.h"
#include "../../d3d11/intro_skip.h"
#include "../../d3d11/intro_upscale.h"

namespace edvr {
namespace {

void configure(void* opaqueConfig) {
    if (!opaqueConfig) return;
    Config& cfg = *static_cast<Config*>(opaqueConfig);
    introSkipConfigure(cfg);
    introUpscaleConfigure(cfg);
}

void frame(void*, uint32_t sceneFrame) {
    introSkipTick(sceneFrame != 0);
}

void shutdown(void*) {
    introUpscaleShutdown();
    introSkipShutdown();
}

const EdvrPluginLifecycleOps kIntroVideoLifecycleOps = {
    sizeof(EdvrPluginLifecycleOps),
    plugins::kPluginIntro,
    "intro",
    nullptr,
    configure,
    frame,
    shutdown,
};

}  // namespace

const EdvrPluginLifecycleOps* introVideoLifecycleOps() {
    return &kIntroVideoLifecycleOps;
}

}  // namespace edvr
