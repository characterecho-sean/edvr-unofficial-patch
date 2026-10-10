#include "../../d3d11/intro_lifecycle.h"

#include "../../common/config.h"
#include "../../d3d11/backdrop_fix.h"
#include "../../d3d11/intro_curve.h"
#include "../../d3d11/intro_panel.h"
#include "../../d3d11/intro_skip.h"
#include "../../d3d11/intro_upscale.h"
#include "../../d3d11/loader_panel.h"
#include "../../d3d11/splash_dim.h"

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

void configureStage(void*, uint32_t stage, void* opaqueConfig) {
    if (!opaqueConfig) return;
    Config& cfg = *static_cast<Config*>(opaqueConfig);
    switch (stage) {
    case kIntroLifecycleLoader: loaderPanelConfigure(cfg); break;
    case kIntroLifecycleSplashDim: splashDimConfigure(cfg); break;
    case kIntroLifecyclePanel: introPanelConfigure(cfg); break;
    case kIntroLifecycleBackdrop: backdropConfigure(cfg); break;
    default: break;
    }
}

void frameStage(void*, uint32_t stage, ID3D11DeviceContext* context,
                uint32_t sceneFrame) {
    const bool scene = sceneFrame != 0;
    switch (stage) {
    case kIntroLifecycleLoader: loaderPanelTick(context, scene); break;
    case kIntroLifecyclePanel: introPanelTick(context, scene); break;
    case kIntroLifecycleCurve: introCurveTick(context, scene); break;
    default: break;
    }
}

void shutdownStage(void*, uint32_t stage) {
    switch (stage) {
    case kIntroLifecycleLoader: loaderPanelShutdown(); break;
    case kIntroLifecycleSplashDim: splashDimShutdown(); break;
    case kIntroLifecyclePanel: introPanelShutdown(); break;
    case kIntroLifecycleCurve: introCurveShutdown(); break;
    case kIntroLifecycleBackdrop: backdropShutdown(); break;
    default: break;
    }
}

const EdvrPluginLifecycleOps kIntroVideoLifecycleOps = {
    sizeof(EdvrPluginLifecycleOps),
    plugins::kPluginIntro,
    "intro",
    nullptr,
    configure,
    frame,
    shutdown,
    configureStage,
    frameStage,
    shutdownStage,
};

}  // namespace

const EdvrPluginLifecycleOps* introVideoLifecycleOps() {
    return &kIntroVideoLifecycleOps;
}

}  // namespace edvr
