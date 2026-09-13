#pragma once
#include <windows.h>
#include <stdint.h>

// Explicit embedding API for the staged DLL. Alternatively, the first Scene
// VR_InitInternal reads EDVR_OPENXR_LOADER and EDVR_OPENXR_GRAPHICS from this
// process environment. Both must be trusted drive-absolute paths. This is not
// installer configuration or the full legacy OpenVR replacement surface.
#define EDVR_NATIVE_MODULE_VERSION_1 1u
enum EdvrNativeModulePhase {
    EDVR_NATIVE_CONFIGURED = 1, EDVR_NATIVE_STARTING = 2,
    EDVR_NATIVE_RUNNING = 3, EDVR_NATIVE_STOPPED = 4, EDVR_NATIVE_RETAINED = 5
};
struct EdvrNativeRuntimeConfig {
    uint32_t size, version;
    const wchar_t* loaderPath;
    const wchar_t* graphicsProxyPath;
    uint32_t renderWaitMilliseconds; // 1..5000; native diagnostic uses 5000
    uint32_t reserved;
};
struct EdvrNativeRuntimeStatus {
    uint32_t size, version;
    uint32_t phase, initAttempts;
    uint32_t initThread, renderThread, ownerThread, shutdownThread;
    uint32_t cleanup, retained;
    uint64_t renderCallbacks, waits, submits, stereoPairs, copiedEyes, loadingLayers, wrongThread;
};
extern "C" {
// Call once outside DllMain, before Init. Paths are copied, must be trusted
// drive-absolute readable strings, and apply only to this explicitly loaded DLL.
// Configuration pins this DLL until process exit. No runtime or device is
// created until VR_InitInternal; no module is unloaded from DllMain.
HRESULT WINAPI edvrConfigureNativeRuntime(const EdvrNativeRuntimeConfig*);
// CPU snapshot; counters become final only after successful explicit shutdown.
HRESULT WINAPI edvrGetNativeRuntimeStatus(EdvrNativeRuntimeStatus*);
}
static_assert(sizeof(EdvrNativeRuntimeConfig)==32,"native module config ABI");
static_assert(sizeof(EdvrNativeRuntimeStatus)==96,"native module status ABI");
