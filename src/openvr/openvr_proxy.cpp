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
// Set to 1 by us once the table above is filled. Every thunk tests it and calls
// edvr_lazyInit_openvr below if it is still zero.
extern unsigned char edvr_ready_openvr;
void edvr_lazyInit_openvr();
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

INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;

// Loads the real openvr_api.dll and fills the export table.
//
// NOT called from DllMain. It runs from the first thunked export call, which is
// an ordinary call on an ordinary stack with no loader lock held.
//
// That distinction is the entire point. openvr_api_orig.dll is a module nothing
// else in the process has mapped, so LoadLibrary on it runs its DllMain and CRT
// startup -- nested inside ours, under the loader lock. Windows does not support
// that. It happened to work for Valve's DLL, which is why it survived this long,
// but advanced.real_openvr_dll is documented for chaining another OpenVR wrapper
// and those have real startup code. The d3d11 side was rebuilt to defer after
// exactly this pattern crashed the game for a user running ReShade with EDHM;
// this side kept it because bare `jmp [slot]` thunks need the slot filled before
// the first call. The generated thunks now check first and come here instead.
BOOL CALLBACK loadOnceCallback(PINIT_ONCE, PVOID, PVOID*) {
    g_moduleDir = new std::wstring(edvr::moduleDirectory(g_selfModule));

    // No system fallback: openvr_api.dll is not an OS component, so the only
    // correct source is the copy that shipped with the game -- which the install
    // steps rename to openvr_api_orig.dll, the default when nothing is set.
    std::string realDll =
        edvr::readConfigStringEarly(*g_moduleDir, L"edvr.ini", "advanced.real_openvr_dll");
    if (realDll.empty()) realDll = "openvr_api_orig.dll";
    g_realModule = edvr::loadRealModule(*g_moduleDir, realDll, nullptr, L"openvr_api.dll");
    if (!g_realModule) edvr::breadcrumb("vr: FAILED to load openvr_api_orig.dll");

    // Called even when nothing loaded, which is the whole point: it fills the
    // table with the stub so the thunks have somewhere to go. Returning early
    // here left the generated table all zeros and the first forwarded call
    // jumped through null -- a startup crash on the one mistake the install
    // steps warn about, forgetting to rename the game's openvr_api.dll.
    g_missingExports =
        edvr::resolveProcs(g_realModule, kExportNames, kExportCount, edvr_realProcs_openvr,
                           reinterpret_cast<void*>(&edvr_unresolved_openvr));

    if (g_realModule) {
        g_realGetGenericInterface = reinterpret_cast<PFN_VR_GetGenericInterface>(
            GetProcAddress(g_realModule, "VR_GetGenericInterface"));
        edvr::breadcrumb(g_realGetGenericInterface
                             ? "vr: exports resolved"
                             : "vr: FAILED no VR_GetGenericInterface");
    }
    return TRUE;
}

}  // namespace

// Called from the generated thunks, before they jump through the table.
//
// The ready flag is set LAST and behind a barrier, so a thread that sees it set
// is guaranteed to see the filled table: the compiler may not hoist the store
// above the writes, and x86 does not reorder stores against each other. It is
// set even when the load failed -- resolveProcs has filled every slot with the
// stub by then, so the table is safe to jump through and there is nothing to be
// gained by retrying on every call.
extern "C" void edvr_lazyInit_openvr() {
    // Re-entered on the SAME thread, so return rather than wait.
    //
    // InitOnceExecuteOnce blocks a second caller until the first finishes; if
    // that second caller IS the first, on the same thread, it waits for itself
    // and never comes back. Reachable: the module we load, or a chained OpenVR
    // wrapper, calling an openvr export from its own DllMain -- which runs
    // inside our LoadLibraryW, inside this function. A frozen headset is worse
    // than a crash, and "never a hang" is one of this project's rules.
    //
    // Returning early leaves the table holding the do-nothing stub for that one
    // re-entrant call, which is the same answer it would have given before any
    // of this existed.
    // Per-thread, not a shared "who owns it" word: with a shared one, a second
    // thread arriving mid-init overwrites the owner id, and the thread actually
    // running the callback then fails its own re-entry check and waits for
    // itself after all. A plain thread_local bool has no such race and needs no
    // atomics.
    static thread_local bool s_inProgress = false;
    if (s_inProgress) return;

    // No exception may leave this function.
    //
    // It is extern "C" and called from assembly, and /EHsc lets the compiler
    // assume extern "C" does not throw -- so a std::bad_alloc from the `new`
    // inside the callback would hit the fail-fast handler and take the process
    // down at startup. The catch is not a recovery: the table is stub-filled
    // either way, so the game loses VR rather than the whole process.
    s_inProgress = true;
    try {
        InitOnceExecuteOnce(&g_loadOnce, loadOnceCallback, nullptr, nullptr);
    } catch (...) {
        edvr::breadcrumb("vr: lazy init threw; exports will return failure");
    }
    s_inProgress = false;

    _ReadWriteBarrier();
    edvr_ready_openvr = 1;
}

namespace {

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
    // This one is a real C function rather than a generated thunk, so nothing
    // has checked the ready flag on its behalf. It can legitimately be the first
    // export the game calls.
    edvr_lazyInit_openvr();
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
            // Deliberately nothing else. No LoadLibrary, no file reads, no
            // allocation -- see edvr_lazyInit_openvr for why, and note that
            // breadcrumb() only opens and appends a file, which is safe here and
            // is the last-resort channel when nothing later gets far enough to
            // open a log.
            g_selfModule = module;
            DisableThreadLibraryCalls(module);
            edvr::breadcrumb("vr: DllMain attach (real module load deferred)");
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
