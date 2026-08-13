// GENERATED from tools/fakevr/fakevr.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 5517e9283bb8fea5]
// A stand-in for the game's openvr_api.dll, for testing the proxy's startup.
//
// Two jobs.
//
// 1. Have a DllMain that does real work, so "the proxy loaded and nothing hung"
//    means something. It allocates, touches the CRT, and calls LoadLibrary
//    itself -- the nested case.
//
//    It does NOT try to deadlock on purpose. The obvious way to write that --
//    create a thread and wait for it -- deadlocks under ANY LoadLibrary, not
//    just a nested one, because DllMain always runs holding the loader lock and
//    a new thread cannot finish its attach notifications until that lock is
//    free. Such a DLL is simply broken, and a test built on one proves nothing
//    about the proxy. The assertion that actually catches the bug is the second
//    one in openvr_smoke: after loading the proxy, this module must not be
//    mapped yet.
//
// 2. One export takes arguments in every register class a thunk has to leave
//    alone, plus two on the stack, and returns a value derived from all of
//    them. The proxy's lazy path saves and restores rcx/rdx/r8/r9 and
//    xmm0-xmm3 around its initialiser; if it got any of that wrong, the
//    checksum comes back different.
//
// Signatures here are ours, not OpenVR's. The proxy forwards by jumping, so it
// never looks at them -- which is precisely why the argument check has to be
// done through a real call.
#include <windows.h>

#include <cstdlib>
#include <cstring>

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Fault on demand, so the proxy's lazy-load path can be tested for the
        // thing that matters more than whether it works: whether it stays
        // catchable when it does not.
        //
        // The proxy loads this DLL from inside a thunk, through an assembly shim
        // that moves the stack pointer. If that shim has no unwind info, an
        // exception raised here cannot be unwound past it and every SEH handler
        // above -- including the game's -- is skipped, turning a recoverable
        // fault into a hard process kill. That shipped once.
        char buf[8]{};
        if (GetEnvironmentVariableA("EDVR_FAKEVR_FAULT", buf, sizeof(buf)) && buf[0] == '1') {
            volatile int* boom = nullptr;
            *boom = 1;
        }
        void* p = malloc(64 * 1024);
        if (p) {
            memset(p, 0xA5, 64 * 1024);
            free(p);
        }
        // Nested LoadLibrary. Safe because this one is already mapped, which is
        // exactly the distinction the proxy's own comments turn on.
        HMODULE m = LoadLibraryW(L"kernel32.dll");
        if (m) FreeLibrary(m);
    }
    return TRUE;
}

extern "C" {

// The argument-integrity probe. Mixed integer and floating point so the value
// lands in rcx, xmm1, r8, xmm3 and then the stack.
__declspec(dllexport) unsigned long long VR_IsHmdPresent(unsigned long long a, double b,
                                                         unsigned long long c, double d,
                                                         unsigned long long e,
                                                         unsigned long long f) {
    return a + static_cast<unsigned long long>(b) * 10ull +
           c * 100ull + static_cast<unsigned long long>(d) * 1000ull +
           e * 10000ull + f * 100000ull;
}

__declspec(dllexport) unsigned long long VR_GetInitToken() { return 0xABCDEF01ull; }

// The rest exist so the proxy's generated table resolves completely; a missing
// one would be substituted with the stub and the "did every export resolve"
// count would be non-zero for a reason unrelated to what is being tested.
__declspec(dllexport) void VRControlPanel() {}
__declspec(dllexport) void VRDashboardManager() {}
__declspec(dllexport) void VRTrackedCamera() {}
__declspec(dllexport) void VR_GetStringForHmdError() {}
__declspec(dllexport) void VR_GetVRInitErrorAsEnglishDescription() {}
__declspec(dllexport) void VR_GetVRInitErrorAsSymbol() {}
__declspec(dllexport) void VR_InitInternal() {}
__declspec(dllexport) void VR_IsInterfaceVersionValid() {}
__declspec(dllexport) void VR_IsRuntimeInstalled() {}
__declspec(dllexport) void VR_RuntimePath() {}
__declspec(dllexport) void VR_ShutdownInternal() {}
__declspec(dllexport) void* VR_GetGenericInterface(const char*, int*) { return nullptr; }

}  // extern "C"
