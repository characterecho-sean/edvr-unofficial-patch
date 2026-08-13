// GENERATED from tools/openvr_smoke/openvr_smoke.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 be17693dc6365c61]
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
#include <cstring>

#include "../../src/common/guard.h"

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

// A real module whose DllMain faults must degrade, not kill the process.
//
// This started life as a test that the fault stays CATCHABLE, to lock in the
// unwind info the assembly shim was missing. It does not test that: Windows
// catches a faulting DllMain inside LoadLibraryW and returns failure rather than
// propagating an exception, so nothing ever reaches the handler -- measured, the
// child exits with "no exception seen" either way. The unwind info is still
// required, for faults raised in our own initialiser rather than in someone
// else's DllMain, and build.bat asserts it is present instead.
//
// What this DOES pin down is worth keeping: the export still returns, the
// process survives, and resolveProcs has filled the table with the do-nothing
// stub, so the game loses VR instead of dying at startup.
//
// A child process because the fault is arranged by an environment variable read
// in the stand-in's DllMain, and the table resolves once per process.
int faultChild(const char* dir) {
    wchar_t proxy[MAX_PATH];
    _snwprintf_s(proxy, _TRUNCATE, L"%hs\\openvr_api.dll", dir);
    HMODULE m = LoadLibraryW(proxy);
    if (!m) return 3;
    FARPROC p = GetProcAddress(m, "VR_GetInitToken");
    if (!p) return 4;

    unsigned long long v = 12345;
    __try {
        v = reinterpret_cast<unsigned long long(*)()>(p)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;   // also a pass: it returned control to us either way
    }
    // The stub returns 0. Anything else means we forwarded into a module whose
    // DllMain faulted. Not returning at all -- the process being killed -- is
    // the failure, and shows up as an exit code that is neither 0 nor 6.
    return v == 0 ? 0 : 6;
}

// The crash sentinel's lifecycle, which is shared code with none of its own.
//
// Both of its bugs were lifecycle rather than logic, and both were invisible:
//
//   arm() set its flag whether or not the file was written. The file lives in
//   the log directory, which only Log::open() creates -- and that returns early
//   when log.enabled = 0. So with logging off nothing was ever written, no
//   launch saw a trip, and a hook that really was crashing re-armed every start:
//   the protection was absent in exactly the configuration with no log to
//   diagnose it from.
//
//   A trip was permanent. confirm() runs on validation, on commit failure, and
//   from a shutdown needing FreeLibrary that a closing game never does -- so any
//   session ending early left the file behind and every later launch refused,
//   announcing a crash that never happened.
//
// Asserted here rather than trusted, because neither failure shows up as a
// crash or a wrong pixel; they show up as a fix that quietly is not running.
int sentinelChecks() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    wchar_t dir[MAX_PATH];
    _snwprintf_s(dir, _TRUNCATE, L"%sedvr_sentinel_test_%lu", base, GetCurrentProcessId());
    wchar_t file[MAX_PATH];
    _snwprintf_s(file, _TRUNCATE, L"%s\\probe.armed", dir);

    RemoveDirectoryW(dir);   // from a previous run, if any
    int rc = 0;

    {
        // The directory does NOT exist yet. This is the log.enabled = 0 case.
        edvr::Sentinel s(dir, L"probe");
        if (s.trippedOnStartup()) {
            printf("  FAIL  a fresh sentinel reports a trip with no file present\n");
            rc = 1;
        }
        if (!s.arm()) {
            printf("  FAIL  arm() failed with no log directory -- it must create it\n");
            rc = 1;
        } else if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) {
            printf("  FAIL  arm() reported success but wrote no file\n");
            rc = 1;
        }
    }
    {
        // A new sentinel over the same path is the next launch.
        edvr::Sentinel s(dir, L"probe");
        if (!s.trippedOnStartup()) {
            printf("  FAIL  the armed file did not trip the next sentinel\n");
            rc = 1;
        }
        s.clearTrip();
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) {
            printf("  FAIL  clearTrip() left the file in place -- a false trip would\n");
            printf("        then refuse every launch forever\n");
            rc = 1;
        }
        if (s.trippedOnStartup()) {
            printf("  FAIL  clearTrip() left the in-memory trip set, so a second request\n");
            printf("        this session would install after being refused\n");
            rc = 1;
        }
    }
    {
        // The normal success path: armed, then confirmed.
        edvr::Sentinel s(dir, L"probe");
        s.arm();
        s.confirm();
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) {
            printf("  FAIL  confirm() did not remove the armed file\n");
            rc = 1;
        }
    }
    {
        // Somewhere it cannot possibly write. arm() must say so rather than
        // claiming protection it does not have.
        edvr::Sentinel s(L"\\\\?\\Z:\\nonexistent-volume\\edvr", L"probe");
        if (s.arm()) {
            printf("  FAIL  arm() returned true for a path it cannot write\n");
            rc = 1;
        }
    }

    RemoveDirectoryW(dir);
    if (rc == 0) printf("  ok    crash sentinel arms, trips, clears and confirms\n");
    return rc;
}

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[2], "--fault-child") == 0) return faultChild(argv[1]);

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

    // 5. A real module whose DllMain faults degrades instead of killing us.
    {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        wchar_t cmd[MAX_PATH * 2];
        _snwprintf_s(cmd, _TRUNCATE, L"\"%s\" \"%hs\" --fault-child", self, argv[1]);

        SetEnvironmentVariableW(L"EDVR_FAKEVR_FAULT", L"1");
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr,
                                       nullptr, &si, &pi);
        SetEnvironmentVariableW(L"EDVR_FAKEVR_FAULT", nullptr);
        if (!ok) return fail("could not start the fault child");

        WaitForSingleObject(pi.hProcess, 20000);
        DWORD code = 0xFFFFFFFF;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (code != 0) {
            printf("  FAIL  a faulting real-module DllMain did not degrade cleanly "
                   "(child exit 0x%08lX)\n", code);
            printf("        Expected the export to return the stub's zero and the\n");
            printf("        process to survive. It did not return at all.\n");
            printf("\nOPENVR SMOKE FAILED\n");
            return 1;
        }
        printf("  ok    a faulting real module degrades to stubs, process survives\n");
    }

    // Shared code with no other coverage. Runs last because it touches nothing
    // the assertions above depend on.
    if (sentinelChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }

    printf("\nOPENVR SMOKE PASSED\n");
    return 0;
}
