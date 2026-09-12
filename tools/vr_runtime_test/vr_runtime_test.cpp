// The VR back-end verdict, checked against real DLLs instead of a story.
//
// WHY THIS TEST EXISTS
//
// The bug it guards is not "the detection is wrong", it is "the log asserted
// something it had not measured". A commander with a byte-perfect install was
// told eight times that his openvr_api.dll was not installed, because the only
// thing the code actually knew was that a shared-memory flag was clear. He
// spent three rounds on his install. See src/d3d11/vr_runtime.h.
//
// So the thing worth testing is the one claim that replaced the assertion:
// ours-versus-theirs, decided by an export on a module that is really loaded.
// That is checkable without a headset, a game or a runtime -- load the real
// artifacts this build just produced and ask.
//
//   build\openvr_api.dll   is ours; it exports edvr_selftest_system_hook
//   build\fakevr.dll       is the stand-in for the game's own runtime, and
//                          copied under the name openvr_api.dll it is exactly
//                          what a foreign one looks like from here
//
// Two more properties get checked while a real Log is open, because both have
// already been shipped wrong once in this codebase:
//
//   - the explanation is said ONCE, not once per asking site (the message this
//     replaced went out forty times in a single session);
//   - no line it writes is long enough to truncate. The log's buffer is 1200
//     bytes and glitch_frame.cpp carries a comment about a sentence that
//     truncated to "(do", which reads as a crash.
//
// Usage: vr_runtime_test.exe <scratch-dir> <ours|foreign> <dll-to-load>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/common/log.h"
#include "../../src/d3d11/vr_runtime.h"

using edvr::Log;
using edvr::VrRuntime;

namespace {

int g_fails = 0;

void ok(bool cond, const char* what) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++g_fails;
}

const char* verdictName(VrRuntime v) {
    switch (v) {
        case VrRuntime::EdvrOpenvr:    return "EdvrOpenvr";
        case VrRuntime::ForeignOpenvr: return "ForeignOpenvr";
        case VrRuntime::OculusNative:  return "OculusNative";
        case VrRuntime::NoneLoaded:    return "NoneLoaded";
    }
    return "?";
}

void expectVerdict(VrRuntime got, VrRuntime want, const char* what) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s (verdict is %s, wanted %s)", what, verdictName(got),
             verdictName(want));
    ok(got == want, msg);
}

void expectMentions(const char* text, const char* needle, const char* what) {
    char msg[300];
    snprintf(msg, sizeof(msg), "%s -- \"%.100s\"", what, text);
    ok(text && strstr(text, needle) != nullptr, msg);
}

std::wstring widen(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

// The written log, read back after close() has joined the flusher.
std::string readNewestLog(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\*.log";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    std::wstring newest;
    FILETIME best{};
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &best) >= 0) {
            best = fd.ftLastWriteTime;
            newest = fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (newest.empty()) return std::string();

    const std::wstring path = dir + L"\\" + newest;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::string();
    std::string out;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(f);
    return out;
}

size_t countOccurrences(const std::string& hay, const char* needle) {
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos) {
        ++n;
        at += strlen(needle);
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage: %s <scratch-dir> <ours|foreign> <dll-to-load>\n", argv[0]);
        return 2;
    }
    const std::wstring scratch = widen(argv[1]);
    const bool wantOurs = strcmp(argv[2], "ours") == 0;
    const std::wstring dll = widen(argv[3]);

    printf("\n[edvr] vr runtime test -- %s\n", argv[2]);

    // A clean scratch each run, so the newest-log search cannot find a stale
    // one and pass on yesterday's evidence.
    {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((scratch + L"\\*.log").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                DeleteFileW((scratch + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    if (!Log::get().open(scratch, L"vrtest")) {
        printf("  FAIL  could not open a log in %s\n", argv[1]);
        return 1;
    }

    // BEFORE anything is loaded. A bare console process has no VR module in
    // it, which is the same shape as a flat, non-VR session of the game -- the
    // case the old text called a WARNING.
    expectVerdict(edvr::vrRuntime(), VrRuntime::NoneLoaded, "nothing loaded yet");
    expectMentions(edvr::vrRuntimeShortWhy(), "no VR runtime is loaded",
                   "the clause says so rather than blaming an install");

    // Asked three times, said once.
    edvr::vrRuntimeExplainOnce();
    edvr::vrRuntimeExplainOnce();
    edvr::vrRuntimeExplainOnce();

    const HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) {
        printf("  FAIL  could not load %s (error %lu)\n", argv[3],
               static_cast<unsigned long>(GetLastError()));
        Log::get().close();
        return 1;
    }
    // The verdict is memoised for a second, deliberately -- every real caller
    // is a once-a-session line or a twenty-second summary, and nothing should
    // walk the module list per draw. Wait it out rather than adding a test-only
    // door into the module.
    Sleep(1200);

    const VrRuntime want = wantOurs ? VrRuntime::EdvrOpenvr : VrRuntime::ForeignOpenvr;
    expectVerdict(edvr::vrRuntime(), want, "after the DLL is loaded");
    expectMentions(edvr::vrRuntimeShortWhy(),
                   wantOurs ? "compositor hook" : "not EDVR's",
                   wantOurs ? "ours is recognised by its export, not its path"
                            : "a foreign openvr_api.dll is not claimed as ours");

    // Three more asks at the new verdict: one further line, because the
    // verdict moved, and then silence.
    edvr::vrRuntimeExplainOnce();
    edvr::vrRuntimeExplainOnce();
    edvr::vrRuntimeExplainOnce();

    Log::get().close();

    const std::string text = readNewestLog(scratch);
    ok(!text.empty(), "the log was written and read back");

    const size_t said = countOccurrences(text, "vr runtime: ");
    char msg[160];
    snprintf(msg, sizeof(msg), "the explanation was said twice, not six times (found %zu)", said);
    ok(said == 2, msg);

    ok(countOccurrences(text, "...[truncated]") == 0,
       "no line it writes reaches the log's 1200-byte buffer");

    // The paragraph has to carry the ACTIONABLE part, not just the verdict.
    if (wantOurs) {
        ok(text.find("edvr_vr_") != std::string::npos,
           "the ours-but-silent paragraph points at the vr log");
    } else {
        ok(text.find("openvr_api_orig.dll") != std::string::npos,
           "the foreign paragraph says how to install ours");
    }
    // And it must never again tell somebody their installed file is missing.
    ok(text.find("is NOT installed") == std::string::npos &&
           text.find("is not installed") == std::string::npos,
       "no line asserts that openvr_api.dll is not installed");

    printf(g_fails ? "\nVR RUNTIME TEST FAILED (%d)\n" : "\nVR RUNTIME TEST PASSED\n", g_fails);
    return g_fails ? 1 : 0;
}
