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

HMODULE g_realModule = nullptr;    // what exports currently forward to
HMODULE g_systemModule = nullptr;  // Windows' own, always resolved, never null
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

    // ALWAYS the system copy here, never a configured chain target.
    //
    // This runs inside DllMain. LoadLibrary of a DLL that is not already mapped
    // runs that DLL's own DllMain while the loader lock is held -- from inside
    // ours -- which Windows does not support and which crashed the game for a
    // user running ReShade 6.8.0 with EDHM v22.01. The system d3d11.dll is safe
    // only because it is already mapped, so the call bumps a refcount and runs
    // no entry point.
    //
    // Chaining happens later, in chainThroughOtherProxy(), once DllMain has
    // returned. Loading the system copy first is not wasted: it guarantees every
    // export has somewhere to go for the window between DllMain and the first
    // export call, which is the window a deferred-only design would leave empty.
    g_realModule = edvr::loadRealModule(*g_moduleDir, "", L"d3d11.dll", L"d3d11.dll");
    g_systemModule = g_realModule;   // kept so chaining can fall back to it
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

// Loads another d3d11 proxy -- EDHM, ReShade -- and re-points every export at
// it, so both mods end up in the chain.
//
// Runs from the first export call, NOT from DllMain. That distinction is the
// whole fix: by now the loader lock is released and the other mod's startup
// code can run normally, which is exactly what it could not do before.
//
// Nothing here is required for edvr to work. Every failure leaves the system
// d3d11.dll in place, which is what was already resolved during the loader
// phase, and says why in the log.
void chainThroughOtherProxy(edvr::Config& cfg) {
    const std::string configured = cfg.getString("advanced.real_dll", "");
    if (configured.empty()) return;

    std::wstring path = edvr::widenUtf8(configured);
    if (path.empty()) return;
    if (path.find(L':') == std::wstring::npos && path.find(L'\\') == std::wstring::npos) {
        path = *g_moduleDir + L"\\" + path;
    }

    // Refuse to load ourselves. Pointing this at a copy of edvr, or at the very
    // file this code is running from, would recurse until the stack ran out.
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(g_selfModule, self, MAX_PATH);
    wchar_t want[MAX_PATH]{};
    if (GetFullPathNameW(path.c_str(), MAX_PATH, want, nullptr) &&
        _wcsicmp(self, want) == 0) {
        edvr::Log::get().note(
            "advanced.real_dll points at edvr itself (%S). Ignored -- chaining to "
            "ourselves would recurse forever. Point it at the OTHER mod's renamed dll.",
            want);
        return;
    }

    const HMODULE chained = LoadLibraryW(path.c_str());
    if (!chained) {
        edvr::Log::get().note(
            "advanced.real_dll = %s could not be loaded from %S (error %lu). Still "
            "forwarding to the system d3d11.dll, so edvr works and the other mod does "
            "not. Check the name and that the file is really there.",
            configured.c_str(), path.c_str(), GetLastError());
        return;
    }

    // Only swap once the new module actually provides the entry points. A DLL
    // that loads but exports nothing useful would otherwise leave every call
    // jumping into a stub.
    auto create = reinterpret_cast<PFN_D3D11CreateDevice>(
        GetProcAddress(chained, "D3D11CreateDevice"));
    if (!create) {
        edvr::Log::get().note(
            "advanced.real_dll = %s loaded but exports no D3D11CreateDevice, so it is "
            "not a d3d11 proxy. Ignored; still forwarding to the system d3d11.dll.",
            configured.c_str());
        FreeLibrary(chained);
        return;
    }

    // Prefer the chained module, fall back to the system copy for anything it
    // does not export. EDHM and ReShade wrap the entry points they care about
    // and no more; without the fallback the rest would become no-op stubs and
    // the game would lose functions that work fine in the system dll.
    size_t fromSystem = 0;
    const size_t missing = edvr::resolveProcsChained(
        chained, g_systemModule, kExportNames, kExportCount, edvr_realProcs_d3d11,
        reinterpret_cast<void*>(&edvr_unresolved_d3d11), &fromSystem);

    g_realModule = chained;
    g_realCreateDevice = create;
    if (auto* swap = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(
            GetProcAddress(chained, "D3D11CreateDeviceAndSwapChain"))) {
        g_realCreateDeviceAndSwapChain = swap;
    }
    // else: keep the system one resolved during the loader phase.

    edvr::Log::get().note(
        "chaining through %S -- %zu export(s) from it, %zu from the system d3d11.dll, "
        "%zu unresolved",
        path.c_str(), kExportCount - fromSystem - missing, fromSystem, missing);
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
    // Now, out from under the loader lock, is when another proxy can safely be
    // brought in.
    chainThroughOtherProxy(cfg);

    // Which DLL is really being forwarded to, asked of the module itself rather
    // than assumed -- intent and outcome can differ. If someone reports that
    // another mod stopped working, this line is the first thing worth seeing.
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
