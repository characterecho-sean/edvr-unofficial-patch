// The graphics DLL, pinned for the life of the process.
//
// WHY. d3d11.dll runs its code on threads nobody waits for: the journal watcher's
// worker (journal_watch.cpp, a detached thread that lives for the session) and the
// thread-pool callback that refreshes HMD Quality (ui_surfaces.cpp,
// TrySubmitThreadpoolCallback, fire and forget, while fix.ui_quality is on). If
// the loader unmaps the DLL while such a thread is inside it, the thread returns
// into unmapped memory. The RC4 review (2026-09-29, F1) named the route: on a
// FreeLibrary unload, DllMain's `reserved == nullptr` branch runs shutdown(),
// which signals the worker and returns -- it cannot wait, because it runs under
// the loader lock and a thread's exit needs that lock -- and the loader then
// unmaps the image.
//
// WHO IS EXPOSED, measured by tools/journal_unload_test against a DLL built from
// the real journal_watch.cpp and this header. The pool callback is: nothing holds
// the module for it, and unpinned, FreeLibrary unmaps the image under it. So is any
// thread made with CreateThread. The journal worker is NOT: it is a std::thread,
// and the static UCRT's _beginthreadex references the module that contains the
// thread routine and ends the thread through FreeLibraryAndExitThread
// (ucrt\startup\thread.cpp), so a FreeLibrary leaves the image mapped under the
// worker and the unload completes on the worker's own thread once it has stopped.
// That is the CRT's doing, not this code's, and the pin does not lean on it.
//
// THE FIX IS TO KEEP THAT FROM HAPPENING, not to race it. GetModuleHandleExW with
// GET_MODULE_HANDLE_EX_FLAG_PIN keeps the module mapped until the process ends,
// whatever FreeLibrary calls it gets, so code and globals are there for as long as
// any thread can be in them, whoever started it and however. The OpenXR runtime
// pins itself the same way (native_module.cpp, configureModule). Elite's exe
// imports d3d11.dll statically, read from the PE import table of the Frontier exe,
// so in the game this DLL's load count never reached zero anyway; the pin makes
// that a property of the DLL rather than of whoever loaded it.
//
// WHERE. Once, from initOnceCallback (d3d11_proxy.cpp), which the first device
// creation (D3D11CreateDevice or D3D11CreateDeviceAndSwapChain) runs out from
// under the loader lock, and before anything detached can start: the worker starts
// at the first Present tick and the pool callback at the first frame boundary,
// both after a device exists. Never from DllMain: nothing needs it there, and
// loaderPhase() (d3d11_proxy.cpp) is written to do as little as it can under the
// loader lock.
//
// WHAT IT DOES NOT COVER. The threads shutdown() joins (the log flusher, the menu
// ini writer, the panel raster worker) are finished before the module goes, so the
// pin is not what protects them. And a host that loads this DLL and lets go of it
// before its first device creation has pinned nothing and started nothing
// detached, so DllMain's FreeLibrary branch is still the right teardown there.
//
// The line it writes, once, is the one a flight log needs to tell a pinned session
// from one that was not:
//   graphics module pinned=1 (path=<the DLL>; ...)
//   graphics module pinned=0 (error <n>; ...)
#pragma once

#include <windows.h>

#include <atomic>

#include "log.h"

namespace edvr {

// Pin the module that contains `address` and say so in the log, every time it is
// called; pinGraphicsModuleOnce below is the once-only entry the DLL uses. True
// when the loader agreed.
inline bool pinModuleAndLog(const void* address) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module) {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) wcscpy_s(path, L"(unknown)");
        Log::get().note(
            "graphics module pinned=1 (path=%S; it stays mapped for the life of the process, so no "
            "thread of it -- the journal worker, a thread-pool callback -- can return into unmapped "
            "code).",
            path);
        return true;
    }
    Log::get().note(
        "graphics module pinned=0 (error %lu; a host that unloads this DLL while a thread-pool "
        "callback, or a thread made without the CRT, is running in it would fault).",
        GetLastError());
    return false;
}

// Whether the once-only pin below has succeeded.
inline std::atomic<bool> g_graphicsModulePinned{false};

inline bool graphicsModulePinned() {
    return g_graphicsModulePinned.load(std::memory_order_acquire);
}

// The pin, taken once. `address` is any address inside this DLL. A second call
// does nothing and says nothing.
inline bool pinGraphicsModuleOnce(const void* address) {
    static std::atomic<bool> taken{false};
    bool expected = false;
    if (!taken.compare_exchange_strong(expected, true)) return graphicsModulePinned();
    const bool pinned = pinModuleAndLog(address);
    g_graphicsModulePinned.store(pinned, std::memory_order_release);
    return pinned;
}

}  // namespace edvr
