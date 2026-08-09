// edvr d3d11.dll proxy.
//
// Deployment: drop next to EliteDangerous64.exe. Every export is forwarded
// through generated thunks to the real d3d11.dll; only the two device-creation
// entry points are wrapped, so we can attach the renderer inventory to the
// device the game gets back.
//
// Chaining: if another d3d11 proxy is already installed (EDHM, ReShade), rename
// theirs and point advanced.real_dll at it in edvr.ini. Ours will load and forward
// through it, and both mods keep working.
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/proxy.h"
#include "device_hook.h"

extern "C" {
extern void* edvr_realProcs_d3d11[];
void edvr_unresolved_d3d11();
}

namespace {

const char* const kExportNames[] = {
#include "edvr_exports_d3d11.inc"
};
constexpr size_t kExportCount = sizeof(kExportNames) / sizeof(kExportNames[0]);

HMODULE g_realModule = nullptr;
HMODULE g_selfModule = nullptr;
std::wstring* g_moduleDir = nullptr;

typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
                                               UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                               ID3D11Device**, D3D_FEATURE_LEVEL*,
                                               ID3D11DeviceContext**);
typedef HRESULT(WINAPI* PFN_D3D11CreateDeviceAndSwapChain)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);

PFN_D3D11CreateDevice             g_realCreateDevice = nullptr;
PFN_D3D11CreateDeviceAndSwapChain g_realCreateDeviceAndSwapChain = nullptr;

edvr::FaultBudget g_createBudget("D3D11CreateDevice", 3);

void logDeviceCreation(ID3D11Device* device, D3D_DRIVER_TYPE driverType, UINT flags,
                       HRESULT hr) {
    const uint32_t level = device ? static_cast<uint32_t>(device->GetFeatureLevel()) : 0;
    const uint32_t created = device ? device->GetCreationFlags() : flags;
    edvr::Log::get().note(
        "D3D11 device %p created: featureLevel=0x%04X flags=0x%08X driverType=%d hr=0x%08lX",
        static_cast<void*>(device), level, created, static_cast<int>(driverType), hr);
}

void attachToDevice(ID3D11Device* device, IDXGISwapChain* swapChain,
                    D3D_DRIVER_TYPE driverType, UINT flags, HRESULT hr) {
    edvr::guardedBudget(g_createBudget, [&] {
        logDeviceCreation(device, driverType, flags, hr);
        if (!device) return;
        edvr::hookDevice(device);
        if (swapChain) {
            edvr::hookSwapChain(swapChain);
        } else {
            // Most engines create the swapchain separately through DXGI, so
            // hook the factory to catch it when they do.
            edvr::hookFactoryForDevice(device);
        }
    });
}

size_t g_missingExports = 0;

// Runs under loader lock. Only what must happen before any export can be
// called: the generated thunks jump through a table that has to be populated.
// Config parsing and opening the log wait for ensureInitialised().
void loaderPhase() {
    edvr::breadcrumb("gfx: DllMain attach");

    g_moduleDir = new std::wstring(edvr::moduleDirectory(g_selfModule));

    // Always the system copy. Chaining through another proxy used to be
    // configurable here and it CRASHED THE GAME on launch, reported by a user
    // running ReShade 6.8.0 and EDHM v22.01.
    //
    // The reason is this line's context, not its arguments. loaderPhase runs
    // inside DllMain, and LoadLibrary of a DLL that is not already mapped runs
    // that DLL's own DllMain while the loader lock is held -- from inside ours.
    // Windows does not support that, and ReShade does real work in its DllMain.
    // Loading the system d3d11.dll is fine only because it is already mapped, so
    // the call bumps a refcount and runs no new entry point.
    //
    // Making chaining safe means loading the target after DllMain returns and
    // re-pointing the export table then. That is a real design, not a tweak, and
    // it needs testing against a live EDHM or ReShade install before it goes
    // anywhere near anyone's game. Until then edvr cannot share a game folder
    // with another d3d11 proxy, and the README says so.
    g_realModule = edvr::loadRealModule(*g_moduleDir, "", L"d3d11.dll", L"d3d11.dll");
    if (!g_realModule) {
        edvr::breadcrumb("gfx: FAILED to load real d3d11.dll");
        return;
    }
    edvr::breadcrumb("gfx: real d3d11 loaded");

    g_missingExports = edvr::resolveProcs(
        g_realModule, kExportNames, kExportCount, edvr_realProcs_d3d11,
        reinterpret_cast<void*>(&edvr_unresolved_d3d11));

    g_realCreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(
        GetProcAddress(g_realModule, "D3D11CreateDevice"));
    g_realCreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(
        GetProcAddress(g_realModule, "D3D11CreateDeviceAndSwapChain"));

    edvr::breadcrumb(g_realCreateDevice ? "gfx: exports resolved, loader phase done"
                                        : "gfx: FAILED no D3D11CreateDevice");
}

INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK initOnceCallback(PINIT_ONCE, PVOID, PVOID*) {
    edvr::breadcrumb("gfx: first export call, opening log");
    edvr::Config& cfg = edvr::Config::get();
    cfg.init(*g_moduleDir);
    edvr::Log::get().open(cfg.logDir(), L"gfx");
    edvr::Log::get().note("edvr d3d11 proxy attached; module dir %S",
                          g_moduleDir->c_str());

    const std::string build = edvr::gameBuildVersion();
    if (edvr::gameBuildIsVerified()) {
        edvr::Log::get().note("game build %s -- this is the build the fix was "
                              "developed against",
                              build.c_str());
    } else {
        // Not a warning. The fix finds its pass by shape, so a build it has never
        // seen is the expected case, not a degraded one -- and saying otherwise
        // would send people chasing a problem they do not have.
        edvr::Log::get().note(
            "game build %s -- not the build the fix was developed against (330683). "
            "That is fine: the pass is found by what it does, not by which build "
            "compiled it. If detection fails, the lines below say so and nothing is "
            "touched.",
            build.empty() ? "(unreadable)" : build.c_str());
    }

    if (g_missingExports) {
        edvr::Log::get().note(
            "WARNING: %zu of %zu d3d11 exports did not resolve; regenerate the thunks "
            "against this machine's d3d11.dll with build.bat",
            g_missingExports, kExportCount);
    }
    // Which DLL is really being forwarded to, asked of the module itself rather
    // than assumed. If someone reports that another mod stopped working, this
    // line is the first thing worth seeing.
    wchar_t realPath[MAX_PATH]{};
    const DWORD n = g_realModule ? GetModuleFileNameW(g_realModule, realPath, MAX_PATH) : 0;
    if (n == 0 || n >= MAX_PATH) wcscpy_s(realPath, L"(unknown)");
    edvr::Log::get().note("forwarding to %S", realPath);
    edvr::breadcrumb("gfx: log open");
    return TRUE;
}

void ensureInitialised() {
    InitOnceExecuteOnce(&g_initOnce, initOnceCallback, nullptr, nullptr);
}

void shutdown() {
    edvr::shutdownDeviceHooks();
    edvr::Log::get().note("edvr d3d11 proxy detaching");
    edvr::Log::get().close();
}

}  // namespace

// Exported as D3D11CreateDevice / D3D11CreateDeviceAndSwapChain by the
// generated .def. The edvr_impl_ prefix keeps our definitions from colliding
// with the declarations in d3d11.h.
extern "C" HRESULT WINAPI edvr_impl_D3D11CreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels, UINT numFeatureLevels, UINT sdkVersion,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppContext) {
    if (!g_realCreateDevice) return E_FAIL;
    ensureInitialised();

    const HRESULT hr =
        g_realCreateDevice(adapter, driverType, software, flags, featureLevels,
                           numFeatureLevels, sdkVersion, ppDevice, pFeatureLevel, ppContext);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        attachToDevice(*ppDevice, nullptr, driverType, flags, hr);
    }
    return hr;
}

extern "C" HRESULT WINAPI edvr_impl_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels, UINT numFeatureLevels, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* swapChainDesc, IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppContext) {
    if (!g_realCreateDeviceAndSwapChain) return E_FAIL;
    ensureInitialised();

    const HRESULT hr = g_realCreateDeviceAndSwapChain(
        adapter, driverType, software, flags, featureLevels, numFeatureLevels, sdkVersion,
        swapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppContext);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        attachToDevice(*ppDevice, ppSwapChain ? *ppSwapChain : nullptr, driverType, flags, hr);
    }
    return hr;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_selfModule = module;
            DisableThreadLibraryCalls(module);
            loaderPhase();
            break;

        case DLL_PROCESS_DETACH:
            // reserved != NULL means process termination: other threads are
            // already dead and may have been holding our spinlock or the heap
            // lock. Do the minimum and leak the rest.
            if (reserved != nullptr) {
                edvr::breadcrumb("gfx: process exit");
                edvr::Log::get().detachDuringProcessExit();
            } else {
                edvr::breadcrumb("gfx: FreeLibrary unload");
                shutdown();
            }
            break;

        default:
            break;
    }
    return TRUE;
}
