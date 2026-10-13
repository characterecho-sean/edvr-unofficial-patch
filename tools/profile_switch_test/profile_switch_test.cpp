// The live profile switch's mechanics (src\d3d11\mode_watch.cpp commitPosted), pinned in the build. Design: docs\design-unified-install-2026-10-12.md
// section 6.
//
// What this covers, and what it cannot. The rig loads a copy of build\d3d11.dll on WARP and drives its real Present hook with synthetic
// frames (tools\openxr_native_test\present_device.h), so every Present runs presentFrameBoundary and with it the watch's commit. It checks
// the switch's state (the profile, the settings file, Config's generation, the switch and teardown counts, the menu's configure flag, the
// Supersampling hold, the flat runtime's live flag) and the log lines the switch writes, through the test-only exports
// edvr_selftest_profile_*. It does NOT cover the flat runtime or the VR UI layers under real draws: those key on Elite's own shaders, so
// on WARP they never engage. The pipelines' own teardown under real draws is covered by a flight with the VRAM watch.
//
// Cases: a launch in VR; flips to flat and back, two frames each; a re-post of VR that stays; 50 flat/vr cycles with the private bytes
// after cycles 5 and 50; the settings file falling back to edvr.ini; then the log's switch lines, in order. The control is a second
// process (--control) with the teardown skipped, flipping vr -> flat: the rig passes only when that process's teardown and menu checks
// fail, which is what shows the rig can see a skipped teardown.
//
// Usage: profile_switch_test.exe <path to d3d11.dll>. Exit 0 when every case passes, 1 when one does not.
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <process.h>
#include <string>
#include <vector>

#include "../openxr_native_test/present_device.h"

namespace {

// RuntimeProfile (src\common\install_scope.h: LegacyVr 0, Vr 1, Flat 2, Invalid 3, Passthrough 4).
constexpr uint32_t kVr = 1;
constexpr uint32_t kFlat = 2;
constexpr unsigned kCycles = 50;
constexpr unsigned long long kPrivateToleranceBytes = 8ull * 1024 * 1024;

// The slots edvr_selftest_profile_state fills, in order.
enum Slot : unsigned { kProfile, kFlatFile, kGeneration, kSwitches, kConfigured, kHold, kFlatLive, kTeardowns, kSlots };

using PostFn = void (*)(unsigned int);
using StateFn = unsigned int (*)(uint32_t*, unsigned int);
using SkipFn = void (*)(unsigned int);

int g_checks = 0;
int g_bad = 0;
bool g_control = false;  // the second process: a failing check is the detection the rig wants, not a failure of this run

void report(bool ok, const std::string& text) {
    ++g_checks;
    if (ok) {
        std::printf("  PASS  %s\n", text.c_str());
        return;
    }
    ++g_bad;
    std::printf("  %s  %s\n", g_control ? "detected" : "FAIL", text.c_str());
}

struct Snapshot {
    uint32_t v[kSlots] = {};
    bool ok = false;
    uint32_t operator[](Slot s) const { return v[s]; }
};

std::string describe(const Snapshot& s) {
    if (!s.ok) return "(state export failed)";
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "profile %u, file %s, generation %u, switches %u, teardowns %u, menu configured %u, hold %u, flat live %u",
                  s[kProfile], s[kFlatFile] ? "edvr-flat.ini" : "edvr.ini", s[kGeneration], s[kSwitches], s[kTeardowns],
                  s[kConfigured], s[kHold], s[kFlatLive]);
    return buf;
}

struct Proxy {
    HMODULE module = nullptr;
    PostFn post = nullptr;
    StateFn stateFn = nullptr;
    SkipFn skipFn = nullptr;

    bool load(const std::wstring& dll) {
        module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) return false;
        post = reinterpret_cast<PostFn>(GetProcAddress(module, "edvr_selftest_profile_post"));
        stateFn = reinterpret_cast<StateFn>(GetProcAddress(module, "edvr_selftest_profile_state"));
        skipFn = reinterpret_cast<SkipFn>(GetProcAddress(module, "edvr_selftest_profile_skip_teardown"));
        return post && stateFn && skipFn;
    }
    Snapshot read() const {
        Snapshot s;
        s.ok = stateFn && stateFn(s.v, static_cast<unsigned>(kSlots)) == static_cast<unsigned>(kSlots);
        return s;
    }
};

std::wstring exeDirectory() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring path(buf, n);
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

bool writeFile(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size();
    CloseHandle(f);
    return ok;
}

// A scratch root holding the DLL's own copy and the three settings files the rig is about: the install descriptor (two readers must
// agree on scope), edvr.ini and edvr-flat.ini, each with one harmless key of its own. Its logs go under it.
bool prepare(const std::wstring& root, const std::wstring& dll) {
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\logs").c_str(), nullptr);
    return CopyFileW(dll.c_str(), (root + L"\\d3d11.dll").c_str(), FALSE) &&
           writeFile(root + L"\\edvr_profile.ini", "[install]\r\nschema = 2\r\nscope = both\r\n") &&
           writeFile(root + L"\\edvr.ini", "[hotkey]\r\nmenu = F9\r\n") &&
           writeFile(root + L"\\edvr-flat.ini", "[hotkey]\r\nmenu = F10\r\n");
}

// The log lines this run writes: every edvr_gfx_*.log in the rig's log directory, read whole.
std::string readLogs(const std::wstring& dir) {
    std::string out;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\edvr_gfx_*.log").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        HANDLE f = CreateFileW((dir + L"\\" + fd.cFileName).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
        CloseHandle(f);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
}

size_t countOf(const std::string& text, const char* needle) {
    size_t n = 0;
    const size_t len = std::strlen(needle);
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + len)) ++n;
    return n;
}

// The switch lines in order, each as the text after "profile switch: " up to " (": "vr -> flat", "flat -> vr", "none, stays vr".
std::vector<std::string> switchTokens(const std::string& text) {
    static const char kPrefix[] = "profile switch: ";
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t at = line.find(kPrefix);
        if (at == std::string::npos) continue;
        std::string token = line.substr(at + sizeof(kPrefix) - 1);
        const size_t paren = token.find(" (");
        if (paren != std::string::npos) token.resize(paren);
        while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) token.pop_back();
        tokens.push_back(token);
    }
    return tokens;
}

template <class F>
bool waitFor(F again, unsigned ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (!again()) {
        if (GetTickCount64() >= until) return false;
        Sleep(20);
    }
    return true;
}

unsigned long long privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage;
}

// Two presents, the commit at the first one's boundary. framesOk is false when a Present failed.
Snapshot flipTo(const Proxy& p, edvr::openxr::PresentDevice& d, bool vr, bool& framesOk) {
    p.post(vr ? 1u : 0u);
    framesOk = SUCCEEDED(d.present(0)) && SUCCEEDED(d.present(0));
    return p.read();
}

// A real switch: the profile and file are the expected ones, one switch and one teardown more than before, the generation moved, and the
// flat runtime still idle (it never engages on WARP).
bool realSwitch(const Snapshot& before, const Snapshot& after, uint32_t wantProfile, uint32_t wantFile) {
    return before.ok && after.ok && after[kProfile] == wantProfile && after[kFlatFile] == wantFile &&
           after[kSwitches] == before[kSwitches] + 1 && after[kTeardowns] == before[kTeardowns] + 1 &&
           after[kGeneration] != before[kGeneration] && after[kFlatLive] == 0;
}

bool launchState(const Snapshot& s) {
    return s.ok && s[kProfile] == kVr && s[kFlatFile] == 0 && s[kSwitches] == 0 && s[kTeardowns] == 0 && s[kFlatLive] == 0;
}

// The summary. It says what the pass covers: the switch's mechanics on WARP, not the flat runtime or the VR UI layers, which never engage here.
int finish() {
    if (g_bad == 0) {
        std::printf("profile switch test ok: %d assertion(s); the switch's mechanics on WARP only (the flat runtime and VR UI layers do not "
                    "engage here; their teardown under real draws needs a flight with the VRAM watch); the control's skipped teardown is detected\n",
                    g_checks);
        return 0;
    }
    std::printf("PROFILE SWITCH TEST FAILED (%d of %d)\n", g_bad, g_checks);
    return 1;
}

int runControl(const std::wstring& root) {
    g_control = true;
    SetEnvironmentVariableW(L"EDVR_LOG_DIR", (root + L"\\logs").c_str());
    SetEnvironmentVariableW(L"EDVR_LOG_DIR_FOR", nullptr);
    Proxy p;
    if (!p.load(root + L"\\d3d11.dll")) {
        std::printf("  FAIL  control: the proxy copy does not load with its test exports\n");
        return 2;
    }
    p.skipFn(1);  // the control: every commit skips its teardown
    edvr::openxr::PresentDevice d;
    if (FAILED(d.initialize(p.module, D3D_DRIVER_TYPE_WARP, 64, 64))) {
        std::printf("  FAIL  control: no WARP device through the proxy\n");
        return 2;
    }
    const Snapshot launch = p.read();
    report(launchState(launch), "control launch: profile Vr, file edvr.ini, switches 0 -- " + describe(launch));

    bool framesOk = false;
    const Snapshot after = flipTo(p, d, false, framesOk);
    const std::wstring logs = root + L"\\logs";
    const bool logged = waitFor([&] { return countOf(readLogs(logs), "profile switch: vr -> flat") > 0; }, 10000);
    if (!framesOk || !logged || !after.ok || after[kProfile] != kFlat || after[kSwitches] != 1) {
        std::printf("  FAIL  control: the switch itself did not commit -- %s\n", describe(after).c_str());
        return 2;
    }
    report(after[kTeardowns] == 1, "control flip to flat: the switch ran its teardown -- " + describe(after));
    const std::string text = readLogs(logs);
    report(countOf(text, "menu: flat graphics panel configured") == 1,
           "control flip to flat: the menu's flat configure line is written once");
    return g_bad > 0 ? 1 : 2;
}

int runMain(const std::wstring& dllArg) {
    wchar_t full[MAX_PATH * 4]{};
    GetFullPathNameW(dllArg.c_str(), static_cast<DWORD>(sizeof(full) / sizeof(full[0])), full, nullptr);
    const std::wstring dll = full;
    const std::wstring base = exeDirectory() + L"\\profile_switch_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(GetTickCount64());
    const std::wstring mainRoot = base + L"\\main";
    const std::wstring controlRoot = base + L"\\control";
    CreateDirectoryW(base.c_str(), nullptr);
    if (!prepare(mainRoot, dll) || !prepare(controlRoot, dll)) {
        std::printf("  FAIL  could not prepare the scratch directories under %S\n", base.c_str());
        return 1;
    }
    // The proxy's log goes to this run's own directory, whatever the runner set: EDVR_LOG_DIR moves it for every exe (the journal rig does the same).
    SetEnvironmentVariableW(L"EDVR_LOG_DIR", (mainRoot + L"\\logs").c_str());
    SetEnvironmentVariableW(L"EDVR_LOG_DIR_FOR", nullptr);
    std::printf("profile switch test -- the live switch's mechanics on WARP; the flat runtime and VR UI layers do not engage here\n");

    Proxy p;
    const bool loaded = p.load(mainRoot + L"\\d3d11.dll");
    report(loaded, "the real proxy loads with its profile test exports");
    if (!loaded) return finish();

    std::vector<std::string> expected;  // the switch lines the log must carry, in order
    {
        edvr::openxr::PresentDevice d;
        const HRESULT hr = d.initialize(p.module, D3D_DRIVER_TYPE_WARP, 64, 64);
        report(SUCCEEDED(hr) && d.swapchain() != nullptr, "a WARP device and swap chain through the real proxy");
        if (FAILED(hr)) return finish();

        Snapshot s = p.read();
        report(launchState(s), "launch: profile Vr, file edvr.ini, switches 0 -- " + describe(s));

        bool framesOk = false;
        Snapshot before = s;
        s = flipTo(p, d, false, framesOk);
        report(framesOk && realSwitch(before, s, kFlat, 1) && s[kConfigured] == 1 && s[kHold] == 0,
               "flip to flat, two frames: a real switch, the menu configured, Supersampling hold not held -- " + describe(s));
        expected.push_back("vr -> flat");

        before = s;
        s = flipTo(p, d, true, framesOk);
        report(framesOk && realSwitch(before, s, kVr, 0), "flip to vr, two frames: a real switch back to edvr.ini -- " + describe(s));
        expected.push_back("flat -> vr");

        before = s;
        s = flipTo(p, d, true, framesOk);
        report(framesOk && s.ok && s[kProfile] == kVr && s[kSwitches] == before[kSwitches] &&
                   s[kTeardowns] == before[kTeardowns] && s[kGeneration] == before[kGeneration],
               "flip to vr again: stays (no switch, no teardown, no reparse) -- " + describe(s));
        expected.push_back("none, stays vr");

        bool cyclesOk = true;
        std::string firstBad;
        unsigned long long bytesAt5 = 0;
        for (unsigned i = 1; i <= kCycles; ++i) {
            before = s;
            s = flipTo(p, d, false, framesOk);
            if (!(framesOk && realSwitch(before, s, kFlat, 1)) && cyclesOk) {
                cyclesOk = false;
                firstBad = "cycle " + std::to_string(i) + " to flat: " + describe(s);
            }
            expected.push_back("vr -> flat");
            before = s;
            s = flipTo(p, d, true, framesOk);
            if (!(framesOk && realSwitch(before, s, kVr, 0)) && cyclesOk) {
                cyclesOk = false;
                firstBad = "cycle " + std::to_string(i) + " to vr: " + describe(s);
            }
            expected.push_back("flat -> vr");
            if (i == 5) bytesAt5 = privateBytes();
        }
        const unsigned long long bytesAt50 = privateBytes();
        report(cyclesOk && s[kSwitches] == 2 + 2 * kCycles && s[kTeardowns] == s[kSwitches],
               std::to_string(kCycles) + " flat/vr cycles: every flip a real switch, switches " + std::to_string(2 + 2 * kCycles) +
                   " (want " + std::to_string(2 + 2 * kCycles) + "), no crash" + (cyclesOk ? "" : "; first bad: " + firstBad));
        const unsigned long long delta = bytesAt50 > bytesAt5 ? bytesAt50 - bytesAt5 : bytesAt5 - bytesAt50;
        char bytesText[256];
        std::snprintf(bytesText, sizeof(bytesText),
                      "private bytes after cycle 5: %llu, after cycle 50: %llu (delta %llu, want within %llu)", bytesAt5, bytesAt50,
                      delta, kPrivateToleranceBytes);
        report(delta <= kPrivateToleranceBytes, bytesText);

        DeleteFileW((mainRoot + L"\\edvr-flat.ini").c_str());
        before = s;
        s = flipTo(p, d, false, framesOk);
        report(framesOk && realSwitch(before, s, kFlat, 0),
               "edvr-flat.ini deleted, flip to flat: the settings file falls back to edvr.ini -- " + describe(s));
        expected.push_back("vr -> flat");
    }

    // The log: the switch lines in order, the menu's flat configure once per flip to flat, and no freeze line. The lines may trail the
    // frames by the flusher's interval, so the wait is for the last expected line.
    const std::wstring logs = mainRoot + L"\\logs";
    std::string text;
    waitFor([&] {
        text = readLogs(logs);
        return switchTokens(text).size() >= expected.size();
    }, 20000);
    const std::vector<std::string> got = switchTokens(text);
    size_t flips = 0;
    for (const std::string& t : expected) flips += t == "vr -> flat";
    std::string diff;
    if (got != expected) {
        size_t at = 0;
        while (at < got.size() && at < expected.size() && got[at] == expected[at]) ++at;
        diff = "; first difference at line " + std::to_string(at + 1) + ": got '" + (at < got.size() ? got[at] : "(none)") +
               "', want '" + (at < expected.size() ? expected[at] : "(none)") + "'";
    }
    report(got == expected, "the log's switch lines are the expected sequence, in order (" + std::to_string(got.size()) + " lines, want " +
                                std::to_string(expected.size()) + diff + ")");
    const size_t configured = countOf(text, "menu: flat graphics panel configured");
    report(configured == flips, "the menu's flat configure line once per flip to flat (" + std::to_string(configured) + ", want " +
                                    std::to_string(flips) + ")");
    report(countOf(text, "monitor: FREEZE") == 0, "no monitor: FREEZE line in the log");

    // The control: this run's module cannot have its teardown skipped, so a second process does it, with its own log and its own copy.
    wchar_t self[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof(self) / sizeof(self[0])));
    const intptr_t rc = _wspawnl(_P_WAIT, self, self, L"--control", controlRoot.c_str(), nullptr);
    report(rc == 1, "control: with the teardown skipped, flip to flat is detected (its teardown and menu checks fail; exit " +
                        std::to_string(static_cast<long long>(rc)) + ", want 1)");

    return finish();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Unbuffered: a run that dies takes its buffered lines with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && std::wcscmp(argv[1], L"--control") == 0) return runControl(argv[2]);
    if (argc < 2 || !argv[1] || !*argv[1]) {
        std::printf("usage: profile_switch_test.exe <path to d3d11.dll>\n");
        return 2;
    }
    return runMain(argv[1]);
}
