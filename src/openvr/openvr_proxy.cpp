// EDVR openvr_api.dll proxy.
//
// Deployment: rename the game's openvr_api.dll to openvr_api_orig.dll and put
// this one in its place. Every export is forwarded through generated thunks;
// only VR_GetGenericInterface is wrapped, because that is where the game is
// handed the IVRCompositor pointer.
//
// This exists for one reason: the decision not to show a bad frame has to be
// made where frames are handed to SteamVR, and that is here rather than in
// d3d11.dll. Without this file the d3d11 side still detects the bad frame and
// still logs it -- it just cannot stop it being shown.
//
// Uninstalling is renaming two files back. Nothing is written to the game's
// code, and nothing survives deleting this DLL.
#include <windows.h>

#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/proxy.h"
#include "compositor_hook.h"
#include "openvr_min.h"

extern "C" {
// Provided by the generated assembly: one slot per thunked export.
extern void* edvr_realProcs_openvr[];
void edvr_unresolved_openvr();
}

namespace {

const char* const kExportNames[] = {
#include "edvr_exports_openvr.inc"
};
constexpr size_t kExportCount = sizeof(kExportNames) / sizeof(kExportNames[0]);

HMODULE g_realModule = nullptr;
HMODULE g_selfModule = nullptr;
std::wstring* g_moduleDir = nullptr;

typedef void*(__cdecl* PFN_VR_GetGenericInterface)(const char* interfaceVersion,
                                                   vr::EVRInitError* error);
PFN_VR_GetGenericInterface g_realGetGenericInterface = nullptr;

edvr::FaultBudget g_interfaceBudget("VR_GetGenericInterface", 3);

size_t g_missingExports = 0;

// Runs under loader lock. Kept to the minimum that must happen before any export
// can be called: the generated thunks jump through a table that has to be
// populated, so the real module must be loaded here. Everything that can wait --
// config parsing, creating directories, opening the log -- is deferred to
// ensureInitialised() below, out from under the lock.
void loaderPhase() {
    edvr::breadcrumb("vr: DllMain attach");

    g_moduleDir = new std::wstring(edvr::moduleDirectory(g_selfModule));

    // No system fallback: openvr_api.dll is not an OS component, so the only
    // correct source is the copy that shipped with the game -- which the install
    // steps rename to openvr_api_orig.dll, the default when nothing is set.
    std::string realDll =
        edvr::readConfigStringEarly(*g_moduleDir, L"edvr.ini", "advanced.real_openvr_dll");
    if (realDll.empty()) realDll = "openvr_api_orig.dll";
    g_realModule = edvr::loadRealModule(*g_moduleDir, realDll, nullptr, L"openvr_api.dll");
    if (!g_realModule) {
        edvr::breadcrumb("vr: FAILED to load openvr_api_orig.dll");
        return;
    }

    g_missingExports =
        edvr::resolveProcs(g_realModule, kExportNames, kExportCount, edvr_realProcs_openvr,
                           reinterpret_cast<void*>(&edvr_unresolved_openvr));

    g_realGetGenericInterface = reinterpret_cast<PFN_VR_GetGenericInterface>(
        GetProcAddress(g_realModule, "VR_GetGenericInterface"));

    edvr::breadcrumb(g_realGetGenericInterface ? "vr: exports resolved"
                                               : "vr: FAILED no VR_GetGenericInterface");
}

INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK initOnceCallback(PINIT_ONCE, PVOID, PVOID*) {
    edvr::Config& cfg = edvr::Config::get();
    cfg.init(*g_moduleDir);
    edvr::Log::get().open(cfg.logDir(), L"vr");
    edvr::Log::get().note("EDVR openvr proxy attached; module dir %S",
                          g_moduleDir->c_str());
    if (g_missingExports) {
        edvr::Log::get().note(
            "WARNING: %zu of %zu openvr exports did not resolve. The real DLL is a "
            "different build from the one the thunks were generated against; rebuild "
            "with build.bat against your own openvr_api.dll.",
            g_missingExports, kExportCount);
    }
    return TRUE;
}

// Called from our wrapped export, never from DllMain.
void ensureInitialised() {
    InitOnceExecuteOnce(&g_initOnce, initOnceCallback, nullptr, nullptr);
}

void shutdown() {
    edvr::shutdownCompositorHook();
    edvr::Log::get().note("EDVR openvr proxy detaching");
    edvr::Log::get().close();
}

}  // namespace

// Exported as VR_GetGenericInterface by the generated .def. VR_CALLTYPE is
// __cdecl on Windows.
extern "C" void* __cdecl edvr_impl_VR_GetGenericInterface(const char* interfaceVersion,
                                                          vr::EVRInitError* error) {
    if (!g_realGetGenericInterface) {
        if (error) *error = 1;  // VRInitError_Unknown
        return nullptr;
    }
    ensureInitialised();

    void* iface = g_realGetGenericInterface(interfaceVersion, error);
    if (!iface || !interfaceVersion) return iface;

    void* result = iface;
    edvr::guardedBudget(g_interfaceBudget, [&] {
        result = edvr::interceptInterface(iface, interfaceVersion);
    });
    return result;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_selfModule = module;
            DisableThreadLibraryCalls(module);
            loaderPhase();
            break;

        case DLL_PROCESS_DETACH:
            // reserved != NULL means the process is terminating, and every other
            // thread has already been killed -- possibly holding our spinlock or
            // the heap lock. Joining a thread or freeing memory here is how you
            // turn a clean exit into a crash in ntdll.
            if (reserved != nullptr) {
                edvr::Log::get().detachDuringProcessExit();
            } else {
                shutdown();
            }
            break;

        default:
            break;
    }
    return TRUE;
}
