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

// Variants are separate DLLs built from this one file (build.bat :rig_addon_manager_test):
//   (none)                   the healthy addon, registers no settings
//   FIXTURE_VARIANT_b        healthy, but different bytes from the default (the swap in the hash-at-load case)
//   FIXTURE_VARIANT_regfault EdvrPluginRegister writes through a null pointer
//   FIXTURE_VARIANT_initfail registers two settings, then onInitialize returns an error
//   FIXTURE_VARIANT_initfault registers two settings, then onInitialize access-violates
#if defined(FIXTURE_VARIANT_b)
#define FIXTURE_ID 2
#else
#define FIXTURE_ID 1
#endif

extern "C" __declspec(dllexport) int WINAPI fixtureGetVariant() {
    return FIXTURE_ID;
}

#if defined(FIXTURE_VARIANT_initfail) || defined(FIXTURE_VARIANT_initfault)
static void EDVR_API settingSetter(const char*, int64_t, void*) {}

static void registerTwoSettings(const EdvrHostServices* host) {
    static const char* kKeys[2] = { "first", "second" };
    for (int i = 0; i < 2; ++i) {
        EdvrPluginSettingDef d{};
        d.structSize = sizeof(d);
        d.header = "Fixture";
        d.key = kKeys[i];
        d.label = kKeys[i];
        d.type = EDVR_PLUGIN_SETTING_BOOL;
        d.maxValue = 1;
        d.setter = &settingSetter;
        host->registerSetting(&d);
    }
}
#endif

static int EDVR_API pluginOnInitialize(const EdvrHostServices* host) {
    if (!host || host->structSize < sizeof(EdvrHostServices)) return -1;
#if defined(FIXTURE_VARIANT_initfail)
    registerTwoSettings(host);
    return -2;
#elif defined(FIXTURE_VARIANT_initfault)
    registerTwoSettings(host);
    *reinterpret_cast<volatile int*>(nullptr) = 42; // deliberate fault
#endif
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

#if defined(FIXTURE_VARIANT_regfault)
    *reinterpret_cast<volatile int*>(nullptr) = 42; // deliberate fault while filling the table
#endif
    callbacks->pluginName = "Test Addon Fixture";
    callbacks->pluginVersion = "1.0.0";

    callbacks->onInitialize = &pluginOnInitialize;
    callbacks->onShutdown = &pluginOnShutdown;
    callbacks->onUpdate = &pluginOnUpdate;
    callbacks->onRenderEye = &pluginOnRenderEye;
    callbacks->onFilterInput = &pluginOnFilterInput;

    return 0;
}
