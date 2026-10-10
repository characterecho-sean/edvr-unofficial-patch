#include "exposure_lifecycle.h"

#include "exposure_dispatch.h"

#include "../../common/config.h"

namespace edvr::plugins::exposure {
namespace {

void configureState(void* rawState, void* rawConfig) {
    if (!rawState || !rawConfig) return;
    exposurePluginConfigure(static_cast<ExposureActionState*>(rawState),
                            *static_cast<Config*>(rawConfig));
}

void frameStage(void* rawState, uint32_t stage,
                ID3D11DeviceContext*, uint32_t) {
    if (!rawState) return;
    auto* actionState = static_cast<ExposureActionState*>(rawState);
    auto* observerState =
        static_cast<ExposureDispatchObserverState*>(actionState);
    switch (stage) {
    case kExposureLifecycleResetPairing:
        exposurePluginResetDispatchFrame(observerState);
        break;
    case kExposureLifecycleExpireVerdicts:
        exposurePluginExpireDispatchVerdicts(observerState);
        break;
    default:
        break;
    }
}

void frame(void* state, uint32_t) {
    frameStage(state, kExposureLifecycleResetPairing, nullptr, 0);
    frameStage(state, kExposureLifecycleExpireVerdicts, nullptr, 0);
}

void shutdown(void* rawState) {
    exposurePluginShutdownResources(
        static_cast<ExposureActionState*>(rawState));
}

}  // namespace

void initializeExposureLifecycleOps(EdvrPluginLifecycleOps* ops,
                                    ExposureActionState* state) {
    if (!ops) return;
    *ops = {
        sizeof(EdvrPluginLifecycleOps),
        plugins::kPluginExposure,
        "exposure",
        state,
        nullptr,
        frame,
        shutdown,
        nullptr,
        frameStage,
        nullptr,
        configureState,
    };
}

}  // namespace edvr::plugins::exposure
