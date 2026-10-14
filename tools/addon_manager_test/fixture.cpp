#include "../../include/edvr_plugin_api.h"
#include <windows.h>
#include <atomic>

static std::atomic<int> g_updateCount{0};
static std::atomic<int> g_renderCount{0};
static std::atomic<int> g_crashMode{0}; // 0 = normal, 1 = crash onUpdate, 2 = crash onRenderEye

extern "C" __declspec(dllexport) void WINAPI fixtureSetCrashMode(int mode) {
    g_crashMode.store(mode);
}

extern "C" __declspec(dllexport) int WINAPI fixtureGetUpdateCount() {
    return g_updateCount.load();
}

extern "C" __declspec(dllexport) int WINAPI fixtureGetRenderCount() {
    return g_renderCount.load();
}

static int EDVR_API pluginOnInitialize(const EdvrHostServices* host) {
    if (!host || host->structSize < sizeof(EdvrHostServices)) return -1;
    return 0;
}

static void EDVR_API pluginOnShutdown(void) {
}

static void EDVR_API pluginOnUpdate(const EdvrPosef* headPose, float dtSeconds) {
    (void)headPose;
    (void)dtSeconds;
    if (g_crashMode.load() == 1) {
        *reinterpret_cast<volatile int*>(nullptr) = 42; // deliberate fault
    }
    g_updateCount.fetch_add(1);
}

static void EDVR_API pluginOnRenderEye(const EdvrEyeRenderContext* eyeCtx) {
    (void)eyeCtx;
    if (g_crashMode.load() == 2) {
        *reinterpret_cast<volatile int*>(nullptr) = 42; // deliberate fault
    }
    g_renderCount.fetch_add(1);
}

static void EDVR_API pluginOnFilterInput(EdvrInputContext* inputCtx) {
    if (inputCtx && inputCtx->deviceType == 1) {
        inputCtx->swallowInput = 1;
    }
}

extern "C" __declspec(dllexport) int EDVR_API EdvrPluginRegister(uint32_t hostApiVersion, EdvrPluginCallbacks* callbacks) {
    if (hostApiVersion < EDVR_PLUGIN_API_VERSION) return -1;
    if (!callbacks || callbacks->structSize < sizeof(EdvrPluginCallbacks)) return -1;

    callbacks->pluginName = "Test Addon Fixture";
    callbacks->pluginVersion = "1.0.0";

    callbacks->onInitialize = &pluginOnInitialize;
    callbacks->onShutdown = &pluginOnShutdown;
    callbacks->onUpdate = &pluginOnUpdate;
    callbacks->onRenderEye = &pluginOnRenderEye;
    callbacks->onFilterInput = &pluginOnFilterInput;

    return 0;
}
