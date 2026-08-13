// openvr_smoke -- checks the openvr proxy's startup path without the game.
//
// The thing under test is that the proxy does NOT load the real openvr_api.dll
// from DllMain. It used to. LoadLibrary of a module nothing else has mapped runs
// that module's DllMain under the loader lock, re-entrantly, which Windows does
// not support -- the same pattern crashed the game for a user running ReShade
// with EDHM on the d3d11 side, and that side was rebuilt to defer.
//
// Three things are asserted, in order:
//
//   1. Loading the proxy returns promptly. On a worker thread with a timeout, so
//      a hang is reported rather than hanging the test.
//
//   2. fakevr is not mapped yet. THIS is the assertion that catches the bug:
//      if the real module is already loaded when LoadLibrary returns, the proxy
//      loaded it from DllMain, which is the thing being fixed. It fails against
//      the old build and passes against this one.
//
//   3. A thunked export forwards, and its arguments arrive intact. The lazy path
//      runs a C initialiser in the middle of a call whose arguments are still
//      live in rcx/rdx/r8/r9 and xmm0-xmm3; the probe checks all of those plus
//      two stack arguments.
//
// Usage: openvr_smoke.exe <dir containing openvr_api.dll and openvr_api_orig.dll>
#include <windows.h>

#include <cstdio>

namespace {

wchar_t g_proxyPath[MAX_PATH];
HMODULE g_loaded = nullptr;

int fail(const char* what) {
    printf("  FAIL  %s\n", what);
    return 1;
}

DWORD WINAPI loadProxy(LPVOID) {
    g_loaded = LoadLibraryW(g_proxyPath);
    return 0;
}

typedef unsigned long long(*PFN_Probe)(unsigned long long, double, unsigned long long,
                                       double, unsigned long long, unsigned long long);

}  // namespace

int main(int argc, char** argv) {
    printf("edvr openvr smoke\n");
    if (argc < 2) {
        printf("usage: openvr_smoke.exe <dir with openvr_api.dll + openvr_api_orig.dll>\n");
        return 2;
    }
    _snwprintf_s(g_proxyPath, _TRUNCATE, L"%hs\\openvr_api.dll", argv[1]);
    printf("proxy: %ls\n\n", g_proxyPath);

    // 1. The load must not deadlock.
    HANDLE t = CreateThread(nullptr, 0, loadProxy, nullptr, 0, nullptr);
    if (!t) return fail("could not create the loader thread");
    const DWORD waited = WaitForSingleObject(t, 15000);
    if (waited == WAIT_TIMEOUT) {
        printf("  FAIL  loading the proxy did not finish in 15 seconds\n");
        printf("        Something in the load path is blocking under the loader lock.\n");
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;   // the thread is stuck; process exit takes it with us
    }
    CloseHandle(t);
    if (!g_loaded) return fail("the proxy did not load at all");
    printf("  ok    proxy loaded without deadlocking\n");

    // 2. ...and it did so without pulling in the real module.
    if (GetModuleHandleW(L"openvr_api_orig.dll")) {
        printf("  FAIL  openvr_api_orig.dll was mapped by DllMain\n");
        printf("        Deferral is not happening; assertion 1 passed by luck.\n");
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    printf("  ok    real module not loaded yet -- deferred as intended\n");

    // 3. The first export call resolves it and forwards correctly.
    PFN_Probe probe =
        reinterpret_cast<PFN_Probe>(GetProcAddress(g_loaded, "VR_IsHmdPresent"));
    if (!probe) return fail("VR_IsHmdPresent is not exported by the proxy");

    const unsigned long long got = probe(7ull, 6.0, 5ull, 4.0, 3ull, 2ull);
    const unsigned long long want = 7ull + 60ull + 500ull + 4000ull + 30000ull + 200000ull;
    if (got != want) {
        printf("  FAIL  forwarded call returned %llu, expected %llu\n", got, want);
        printf("        An argument was corrupted across the lazy-load path.\n");
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    printf("  ok    first call resolved the table and passed all six arguments\n");

    if (!GetModuleHandleW(L"openvr_api_orig.dll")) {
        return fail("the real module still is not loaded after a forwarded call");
    }
    printf("  ok    real module loaded on demand\n");

    printf("\nOPENVR SMOKE PASSED\n");
    return 0;
}
