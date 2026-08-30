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
#include "../common/frame_flag.h"
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
// Windows' own, kept separately: when a chained proxy calls back into us these
// are the only way out of the loop.
PFN_D3D11CreateDevice             g_systemCreateDevice = nullptr;
PFN_D3D11CreateDeviceAndSwapChain g_systemCreateDeviceAndSwapChain = nullptr;

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
        // Published before anything is hooked, and deliberately before the
        // config is consulted: the openvr half decides whether it wants this,
        // and it cannot ask for it later -- by the time the game requests an
        // interface the moment has passed. Publishing an unwanted pointer
        // costs one store into a mapping we already own. See early_session.h.
        //
        // The FIRST device wins. attachToDevice runs again for a second
        // device (device_hook.cpp says so in as many words), and the early
        // handover wants the one the game actually renders with, which on
        // every rig measured is the first.
        // AddRef, and never release.
        //
        // This pointer is read by openvr_api.dll from a different call
        // stack entirely -- the handover creates a texture on it before the
        // game has asked for a compositor -- so nothing on that path holds
        // the device alive. Every other consumer of a device pointer in
        // this DLL runs inside a hook body where the caller provably does.
        //
        // Without the reference this is a use-after-free waiting for a game
        // that drops its first device, and the smoke logs show the two
        // devices reported at the SAME ADDRESS on occasion -- the allocator
        // handing the block straight back, which makes staleness
        // undetectable by comparing pointers.
        //
        // One reference held for the process lifetime. The device outlives
        // the game either way.
        if (!edvr::gameDevice()) {
            device->AddRef();
            edvr::publishGameDevice(device);
        }
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

    g_systemCreateDevice = g_realCreateDevice;
    g_systemCreateDeviceAndSwapChain = g_realCreateDeviceAndSwapChain;

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
        // Not a warning. Every fix here identifies its target by what that
        // target does or looks like, so an unfamiliar build is the expected
        // case rather than a degraded one, and saying otherwise would send
        // people chasing a problem they do not have.
        //
        // This line briefly said the resolution fix would refuse on an unknown
        // build, which was true while it was pinned to one. It no longer is:
        // it recognises the code it edits, and reports what it found.
        edvr::Log::get().note(
            "game build %s -- not the build this was developed against (330683). Every "
            "fix here finds its target by what that target does or looks like, not by "
            "which build compiled it, so an unfamiliar build is expected rather than a "
            "problem. If anything fails to match, the lines below say so and nothing is "
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
    // From here on an unhandled exception names the module it came from
    // instead of leaving the trail blank. Faults on our own frame path do not
    // arrive here -- guard.h absorbs those deliberately -- so what this catches
    // is the ones nobody claimed. See breadcrumbInstallCrashHandler.
    edvr::breadcrumbInstallCrashHandler();
    return TRUE;
}

void ensureInitialised() {
    InitOnceExecuteOnce(&g_initOnce, initOnceCallback, nullptr, nullptr);
}

void shutdown() {
    // Per-site fault totals, so "logged once" does not mean "counted once".
    edvr::reportFaultSites();
    edvr::shutdownDeviceHooks();
    // Before the module goes: a filter pointing into an unmapped image would
    // take the game's own crash reporting down with it.
    edvr::breadcrumbRemoveCrashHandler();
    edvr::Log::get().note("edvr d3d11 proxy detaching");
    edvr::Log::get().close();
}

// Depth of this thread's journey through our own device-creation exports.
//
// Breaks the loop when a chained proxy asks for "the original d3d11.dll" BY
// NAME. The module already loaded under that name is us, so it gets our export
// back, calls it, and we call the chain again -- forever. Verified: it ends in
// STATUS_STACK_OVERFLOW, 0xC00000FD, with nothing logged, which is exactly what
// EDHM users reported.
//
// 3Dmigoto resolves the original that way, so this is not a hypothetical.
//
// Depth 1 is the game calling us, and goes to the chain. Depth 2 or more is the
// chain calling back, and must go to the system copy or nothing ever reaches
// Direct3D.
thread_local int g_createDepth = 0;

struct CreateDepth {
    CreateDepth() { ++g_createDepth; }
    ~CreateDepth() { --g_createDepth; }
    bool reentrant() const { return g_createDepth > 1; }
};

bool g_loopReported = false;

void reportLoopOnce() {
    if (g_loopReported) return;
    g_loopReported = true;
    // A breadcrumb as well as the log: breadcrumbs are written immediately with
    // no buffering, so they survive whatever happens next.
    edvr::breadcrumb("gfx: chained proxy called us back, routing it to the system dll");
    edvr::Log::get().note(
        "the chained proxy asked for d3d11.dll and got edvr, because that is the name "
        "edvr is loaded under. Sending it to the system d3d11.dll instead, which is what "
        "it actually wanted. Both mods still work; without this the two would call each "
        "other until the stack ran out.");
}

}  // namespace

namespace edvr {

// The mechanism decision, made from the fact that actually settles safety:
// whose CODE implements this context's methods?
//
// Not where the vtable ARRAY lives -- the first cut of this asked that, and a
// Pimax + OpenComposite rig refuted it in a day (2026-08-18): D3D11 handed out
// a context whose vtable array is on the heap while every entry points into
// system32\d3d11.dll. The array-location probe called that a wrapper, chose
// InPlace, and lost the 64-round war to the runtime re-pointing its own table
// -- the exact failure CopyVptr exists to dodge, not taken because the probe
// mislabelled the object.
//
// Entries pointing into g_systemModule (Windows' own d3d11.dll) mean the
// runtime owns this object: CopyVptr is safe and immune. A ReShade wrapper's
// entries point into ReShade's module, so they do NOT count here and the
// object stays InPlace -- issue #6 avoided. A threshold, not all-or-nothing,
// because a thin wrapper may forward some slots straight to d3d11.dll; a
// clear majority in d3d11.dll is the runtime's own table.
HookMode contextHookModeFor(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_systemModule) return HookMode::InPlace;
    void** vt = nullptr;
    if (!guarded("contextHookModeFor/read-vptr",
                 [&] { vt = *reinterpret_cast<void***>(ctx); })) {
        return HookMode::InPlace;
    }
    // Sample a generous prefix -- ID3D11DeviceContext has ~140 methods, and a
    // wrapper that forwards a handful while intercepting the rest must not be
    // read as the runtime's own.
    constexpr size_t kSample = 96;
    const size_t inSystem = edvr::vtableEntriesInModule(vt, kSample, g_systemModule);
    const bool arrayInside = edvr::vtableInsideModule(vt, g_systemModule);

    // A HIGH bar, not a bare majority, and the field data is why. A genuine
    // runtime context samples 93-96 of 96 inside d3d11.dll -- essentially all
    // of it (measured 2026-08-18 on the Pimax/OpenComposite/OpenXR-Toolkit
    // rig). A real wrapper (ReShade) points ~every entry at its OWN module, so
    // it samples near zero. The two populations sit at the opposite ends of
    // the range with nothing legitimate in between, so the threshold belongs
    // up near the runtime's own number, not at 50%: a hypothetical thin
    // wrapper that forwarded MOST slots as raw d3d11.dll addresses while
    // intercepting a few would clear a bare majority and get its vptr swapped
    // -- issue #6 by another door. 75% keeps a 20-point margin below the
    // runtime and a 65-point gap above any wrapper, and InPlace (the safe
    // default) catches everything that does not clearly clear it.
    const HookMode mode =
        (inSystem * 4 >= kSample * 3) ? HookMode::CopyVptr : HookMode::InPlace;
    Log::get().note(
        "context hook mode: %s -- %zu of %zu sampled vtable entries are inside "
        "Windows' d3d11.dll, and the vtable array itself is %s it. CopyVptr is "
        "chosen when the runtime's own code backs the methods (immune to the "
        "runtime re-pointing its shared table); InPlace when a wrapper owns "
        "them (issue #6 safety).",
        mode == HookMode::CopyVptr ? "CopyVptr" : "InPlace", inSystem, kSample,
        arrayInside ? "inside" : "outside");
    return mode;
}

}  // namespace edvr

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

    CreateDepth depth;
    PFN_D3D11CreateDevice target = g_realCreateDevice;
    if (depth.reentrant()) {
        reportLoopOnce();
        target = g_systemCreateDevice ? g_systemCreateDevice : g_realCreateDevice;
    }

    const HRESULT hr =
        target(adapter, driverType, software, flags, featureLevels,
               numFeatureLevels, sdkVersion, ppDevice, pFeatureLevel, ppContext);
    // Only hook on the way out of the OUTERMOST call. Hooking the device a
    // second time on the way back up would install our vtable copy twice.
    if (SUCCEEDED(hr) && ppDevice && *ppDevice && !depth.reentrant()) {
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

    CreateDepth depth;
    PFN_D3D11CreateDeviceAndSwapChain target = g_realCreateDeviceAndSwapChain;
    if (depth.reentrant()) {
        reportLoopOnce();
        target = g_systemCreateDeviceAndSwapChain ? g_systemCreateDeviceAndSwapChain
                                                  : g_realCreateDeviceAndSwapChain;
    }

    const HRESULT hr = target(
        adapter, driverType, software, flags, featureLevels, numFeatureLevels, sdkVersion,
        swapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppContext);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice && !depth.reentrant()) {
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
                // Getting here at all means the game exited rather than
                // died, so the sentinel comes down. One DeleteFileW, which
                // is no more than the breadcrumb above already does.
                edvr::deviceHookNoteCleanExit();
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
