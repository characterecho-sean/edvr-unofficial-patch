#include "../../include/edvr_plugin_api.h"
#include <windows.h>
#include <atomic>

static std::atomic<int> g_updateCount{0};
static std::atomic<int> g_renderCount{0};
static std::atomic<int> g_crashMode{0}; // 0 = normal, 1 = crash onUpdate, 2 = crash onRenderEye, 3 = crash onFilterInput, 4 = filter input without ever swallowing

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
//   FIXTURE_VARIANT_settings registers two settings and initialises cleanly
//   FIXTURE_VARIANT_renderfault registers two settings, initialises cleanly, then access-violates in onRenderEye
//   FIXTURE_VARIANT_markshutdown onShutdown creates the file named by EDVR_FIXTURE_SHUTDOWN_MARK (shows whether the host ran it)
//   FIXTURE_VARIANT_threadreg   keeps the host's registerSetting and calls it from a worker thread during onInitialize
//                               (fixtureGetThreadRegResult), and again on request later (fixtureCallRegister)
#if defined(FIXTURE_VARIANT_b)
#define FIXTURE_ID 2
#else
#define FIXTURE_ID 1
#endif

extern "C" __declspec(dllexport) int WINAPI fixtureGetVariant() {
    return FIXTURE_ID;
}

#if defined(FIXTURE_VARIANT_initfail) || defined(FIXTURE_VARIANT_initfault) || defined(FIXTURE_VARIANT_settings) || defined(FIXTURE_VARIANT_renderfault)
#if defined(FIXTURE_VARIANT_initfail)
#define FIXTURE_HEADER "FixtureFail"
#elif defined(FIXTURE_VARIANT_initfault)
#define FIXTURE_HEADER "FixtureFault"
#elif defined(FIXTURE_VARIANT_renderfault)
#define FIXTURE_HEADER "FixtureRender"
#else
#define FIXTURE_HEADER "FixtureSettings"
#endif
static void EDVR_API settingSetter(const char*, int64_t, void*) {}

static void registerTwoSettings(const EdvrHostServices* host) {
    static const char* kKeys[2] = { "first", "second" };
    for (int i = 0; i < 2; ++i) {
        EdvrPluginSettingDef d{};
        d.structSize = sizeof(d);
        d.header = FIXTURE_HEADER;
        d.key = kKeys[i];
        d.label = kKeys[i];
        d.type = EDVR_PLUGIN_SETTING_BOOL;
        d.maxValue = 1;
        d.setter = &settingSetter;
        host->registerSetting(&d);
    }
}
#endif

#if defined(FIXTURE_VARIANT_threadreg)
static int (EDVR_API *g_retainedRegister)(const EdvrPluginSettingDef*) = nullptr;
static std::atomic<int> g_threadRegResult{12345};

static int registerOne() {
    EdvrPluginSettingDef d{};
    d.structSize = sizeof(d);
    d.header = "FixtureThread";
    d.key = "fromworker";
    d.label = "fromworker";
    d.type = EDVR_PLUGIN_SETTING_BOOL;
    d.maxValue = 1;
    return g_retainedRegister ? g_retainedRegister(&d) : -99;
}

static DWORD WINAPI workerRegister(LPVOID) {
    g_threadRegResult.store(registerOne());
    return 0;
}

extern "C" __declspec(dllexport) int WINAPI fixtureGetThreadRegResult() {
    return g_threadRegResult.load();
}

extern "C" __declspec(dllexport) int WINAPI fixtureCallRegister() {
    return registerOne();
}
#endif

static int EDVR_API pluginOnInitialize(const EdvrHostServices* host) {
    if (!host || host->structSize < sizeof(EdvrHostServices)) return -1;
#if defined(FIXTURE_VARIANT_threadreg)
    g_retainedRegister = host->registerSetting;
    HANDLE worker = CreateThread(nullptr, 0, &workerRegister, nullptr, 0, nullptr);
    if (worker) {
        WaitForSingleObject(worker, 10000);
        CloseHandle(worker);
    }
    return 0;
#endif
#if defined(FIXTURE_VARIANT_initfail)
    registerTwoSettings(host);
    return -2;
#elif defined(FIXTURE_VARIANT_settings) || defined(FIXTURE_VARIANT_renderfault)
    registerTwoSettings(host);
#elif defined(FIXTURE_VARIANT_initfault)
    registerTwoSettings(host);
    *reinterpret_cast<volatile int*>(nullptr) = 42; // deliberate fault
#endif
    return 0;
}

static void EDVR_API pluginOnShutdown(void) {
#if defined(FIXTURE_VARIANT_markshutdown)
    char path[MAX_PATH]{};
    if (GetEnvironmentVariableA("EDVR_FIXTURE_SHUTDOWN_MARK", path, MAX_PATH) > 0) {
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
#endif
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
#if defined(FIXTURE_VARIANT_renderfault)
    *reinterpret_cast<volatile int*>(nullptr) = 42; // deliberate fault, every call
#endif
    if (g_crashMode.load() == 2) {
        *reinterpret_cast<volatile int*>(nullptr) = 42; // deliberate fault
    }
    g_renderCount.fetch_add(1);
}

static void EDVR_API pluginOnFilterInput(EdvrInputContext* inputCtx) {
    if (g_crashMode.load() == 3) {
        *reinterpret_cast<volatile int*>(nullptr) = 42; // deliberate fault
    }
    if (g_crashMode.load() == 4) return;   // healthy, but never holds input
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
