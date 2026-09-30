// journal_unload_dll -- the real journal worker and the real pin, in a DLL a host
// can unload.
//
// WHY THIS IS A DLL. The RC4 review (2026-09-29, F1) worried that the journal
// watcher's worker is a detached thread running d3d11.dll's code, and that a
// FreeLibrary of the DLL could unmap the image under it: DllMain's
// `reserved == nullptr` branch runs shutdown(), which signals the worker and
// returns, and the loader then unmaps. gate_test links journal_watch.cpp into an
// exe, which cannot be unloaded, so nothing there could ask. This is the same code
// in a module that can be.
//
// WHAT IS REAL. journal_watch.cpp, unchanged, and src/common/module_pin.h: the
// pin below is the call initOnceCallback (d3d11_proxy.cpp) makes, in the same
// order relative to opening the log and to configuring the watcher. DllMain does
// what d3d11_proxy.cpp's DllMain does with a FreeLibrary: runs the journal
// watcher's shutdown. Everything else here is scaffolding for the host.
//
// THREE SHAPES OF "CODE OF THIS DLL RUNNING ON A THREAD NOBODY WAITS FOR", each
// of which the host holds inside the module while it calls FreeLibrary:
//
//   the worker      journal_watch.cpp's own, a detached std::thread: the real one.
//                   The static UCRT's _beginthreadex references the module that
//                   contains the thread routine and ends the thread through
//                   FreeLibraryAndExitThread (ucrt\startup\thread.cpp), so this
//                   one holds its DLL by itself.
//   a raw thread    CreateThread, which holds nothing: the shape the review feared
//                   of the worker, and what any thread made without the CRT is.
//   a pool callback the shape of ui_surfaces.cpp's
//                   TrySubmitThreadpoolCallback(hmdRefresh, nullptr, nullptr).
//                   Nothing holds the module for this one either.
//
// JuInit's `pin` argument is the control: FALSE leaves the pin out, and the host
// then has to SEE the image go under the two that nothing else holds. A hold that
// is never released keeps that from crashing the host: the thread of an unmapped
// module stays parked in a kernel wait and the process ends around it.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/module_pin.h"
#include "../../src/d3d11/journal_watch.h"

using namespace edvr;

namespace {

// The host's memory, not this module's: what DllMain saw has to outlive the
// unmap it describes.
struct Probe {
    volatile LONG detachCalls;
    volatile LONG detachReservedNull;   // the FreeLibrary branch: no process exit
    volatile LONG detachThread;         // who ran DllMain(DETACH)
    volatile LONG hookThread;           // the worker, as the file-call hook saw it
    volatile LONG hookCalls;            // file calls the worker has made: 4 is a whole pass
};
Probe* g_probe = nullptr;

HANDLE g_entered = nullptr;   // manual-reset: the worker is now held inside a file call
HANDLE g_release = nullptr;   // manual-reset: let it go

// When set, the worker stops the watcher ITSELF the moment it is released, from
// inside its own hook. The host then makes no call into the DLL while the worker
// finishes and leaves: a host thread that is inside a DLL whose worker is the last
// thing holding it would return into unmapped code the instant the worker's exit
// let the image go, which is exactly what a broken pin would do to it.
std::atomic<bool> g_stopAfterHold{false};

// The worker's first file call is held until the host says. Never released in the
// control, so the worker never returns into an image that is gone.
void holdHook(const char*) {
    if (g_probe) InterlockedIncrement(&g_probe->hookCalls);
    static std::atomic<bool> held{false};
    bool expected = false;
    if (!held.compare_exchange_strong(expected, true)) return;
    if (g_probe) g_probe->hookThread = static_cast<LONG>(GetCurrentThreadId());
    SetEvent(g_entered);
    WaitForSingleObject(g_release, INFINITE);
    if (g_stopAfterHold.load()) journalWatchShutdown();
}

std::string toUtf8(const wchar_t* w) {
    if (!w || !*w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? static_cast<size_t>(n) : 1, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    out.resize(strlen(out.c_str()));
    return out;
}

struct Job {
    HANDLE entered;
    HANDLE release;
    HANDLE done;
};
Job g_pool = {};
Job g_raw = {};

// The shape of ui_surfaces.cpp's hmdRefresh, held: a pool thread inside this
// module's code that nobody waits for.
VOID CALLBACK poolCallback(PTP_CALLBACK_INSTANCE, PVOID) {
    SetEvent(g_pool.entered);
    WaitForSingleObject(g_pool.release, INFINITE);
    SetEvent(g_pool.done);
}

// A thread made with CreateThread: nothing takes a reference on its behalf.
DWORD WINAPI rawProc(LPVOID) {
    SetEvent(g_raw.entered);
    WaitForSingleObject(g_raw.release, INFINITE);
    SetEvent(g_raw.done);
    return 0;
}

}  // namespace

extern "C" {

__declspec(dllexport) void JuSetProbe(void* probe) { g_probe = static_cast<Probe*>(probe); }

__declspec(dllexport) void JuSetHold(HANDLE entered, HANDLE release, BOOL stopAfterHold) {
    g_entered = entered;
    g_release = release;
    g_stopAfterHold.store(stopAfterHold != FALSE);
}

// What initOnceCallback does, in its order: the log opens, this DLL is pinned, and
// then the journal watcher is configured. `pin` false is the control.
__declspec(dllexport) BOOL JuInit(const wchar_t* journalDir, const wchar_t* logDir, BOOL pin) {
    Config::get().set("log.enabled", "1");
    if (logDir && *logDir) Log::get().open(logDir, L"ju");
    if (pin) pinGraphicsModuleOnce(reinterpret_cast<const void*>(&JuInit));
    Config::get().set("d3d11.journal_watch", "1");
    Config::get().set("d3d11.journal_dir", toUtf8(journalDir).c_str());
    journalWatchTestSetWorkHook(&holdHook);
    journalWatchConfigure();
    return journalWatchActive() ? TRUE : FALSE;
}

__declspec(dllexport) void JuTick() { journalWatchTick(); }
__declspec(dllexport) BOOL JuWorkerRunning() { return journalWatchTestWorkerRunning() ? TRUE : FALSE; }
__declspec(dllexport) BOOL JuGameplay() { return journalGameplay() ? TRUE : FALSE; }
__declspec(dllexport) void JuShutdown() { journalWatchShutdown(); }

// A second call to the once-only pin, which must neither pin again nor say so.
__declspec(dllexport) BOOL JuPinAgain() {
    return pinGraphicsModuleOnce(reinterpret_cast<const void*>(&JuInit)) ? TRUE : FALSE;
}

// The pin refused: an address in no module, so the line the log gets is the
// failure's.
__declspec(dllexport) BOOL JuPinBogus() {
    return pinModuleAndLog(reinterpret_cast<const void*>(static_cast<uintptr_t>(0x10))) ? TRUE : FALSE;
}

// The callback of ui_surfaces.cpp's shape, held until the host releases it.
__declspec(dllexport) BOOL JuPoolSubmit(HANDLE entered, HANDLE release, HANDLE done) {
    g_pool.entered = entered;
    g_pool.release = release;
    g_pool.done = done;
    return TrySubmitThreadpoolCallback(poolCallback, nullptr, nullptr) ? TRUE : FALSE;
}

// A CreateThread thread, held until the host releases it.
__declspec(dllexport) BOOL JuRawThread(HANDLE entered, HANDLE release, HANDLE done) {
    g_raw.entered = entered;
    g_raw.release = release;
    g_raw.done = done;
    HANDLE thread = CreateThread(nullptr, 0, &rawProc, nullptr, 0, nullptr);
    if (!thread) return FALSE;
    CloseHandle(thread);
    return TRUE;
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_probe) {
            InterlockedIncrement(&g_probe->detachCalls);
            g_probe->detachReservedNull = reserved == nullptr ? 1 : 0;
            g_probe->detachThread = static_cast<LONG>(GetCurrentThreadId());
        }
        // d3d11_proxy.cpp's FreeLibrary branch runs shutdown(), and shutdown() is
        // what stops the journal worker; process exit (reserved != nullptr) runs
        // neither.
        if (reserved == nullptr) journalWatchShutdown();
    }
    return TRUE;
}
