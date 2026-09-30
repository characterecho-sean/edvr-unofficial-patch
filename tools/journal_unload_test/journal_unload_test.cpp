// journal_unload_test -- does the graphics DLL's pin hold its code mapped under a
// thread that is running it when the host lets go of the DLL?
//
// THE QUESTION (RC4 review 2026-09-29, F1). d3d11.dll runs its code on threads
// nobody waits for: the journal watcher's worker (a detached thread) and, with
// fix.ui_quality on, a thread-pool callback that refreshes HMD Quality
// (ui_surfaces.cpp, TrySubmitThreadpoolCallback). On a FreeLibrary unload DllMain's
// `reserved == nullptr` branch runs shutdown(), which signals the worker and
// returns; if the loader then unmaps the image, a thread still inside the module
// returns into unmapped memory. gate_test cannot ask: it links journal_watch.cpp
// into an exe, and an exe is never unloaded.
//
// WHAT IT FOUND when it first ran, which is not what the review supposed:
//
//   - The pool callback IS exposed. Nothing holds the module for a pool callback
//     submitted with no environment; unpinned, FreeLibrary unmaps the image under
//     it (scenario 2).
//   - So is a thread made with CreateThread (scenario 1) -- the shape the review
//     took the worker to have.
//   - The journal worker is NOT. It is a std::thread, and the static UCRT's
//     _beginthreadex (ucrt\startup\thread.cpp) takes a GetModuleHandleEx reference
//     on the module that contains the thread routine and ends the thread through
//     FreeLibraryAndExitThread. Unpinned, a FreeLibrary during a file call leaves
//     the image mapped, and the unload completes on the worker's own thread once it
//     has stopped (scenario 3, printed, not asserted: it is the CRT's behaviour).
//   - Neither is the log's flusher, for the same reason (scenario 7). It is stopped
//     only by shutdown(), which is what DllMain's FreeLibrary branch runs, so in a
//     session with logging on that branch cannot be reached at all.
//
// THE FIX is to keep the image mapped whatever the thread's origin:
// initOnceCallback pins the DLL once (src/common/module_pin.h), before anything
// detached can start, so a FreeLibrary cannot unmap it. It does not depend on the
// CRT, and it is the only thing that covers the pool callback. Elite's exe imports
// d3d11.dll statically, so in the game the load count never reached zero either;
// the pin makes that the DLL's own doing.
//
// WHAT THIS RUNS, against journal_unload_dll.dll, built from the real
// journal_watch.cpp and the real pin, whose DllMain does what the proxy's does with
// a FreeLibrary:
//
//   1. CONTROL, a CreateThread thread, unpinned: held inside the module, then
//      FreeLibrary. The module must be UNMAPPED -- the early unmap this rig exists
//      to keep from happening. If it is not, the rig cannot tell a pinned DLL from
//      an unpinned one and says so.
//   2. CONTROL, a pool callback of ui_surfaces.cpp's shape, unpinned: the same.
//   3. The real worker, unpinned: reported, not asserted.
//   4. The real worker, pinned: blocked inside a file call, FreeLibrary. The module
//      stays mapped, DllMain(DETACH) does not run, and once released the worker
//      runs its whole pass, stops, and leaves -- and the module is STILL mapped
//      after it has gone. That last check is the one a broken pin fails.
//   5. The CreateThread thread and the pool callback, pinned: the same.
//   6. The pin's two log lines and its once-only rule, through the log.
//   7. A DLL with its log open, unpinned: reported, not asserted.
//   8. The REAL proxy: build\d3d11.dll loaded and given a device, and its own log
//      must carry `graphics module pinned=1` -- the call in initOnceCallback, as
//      shipped.
//
// A BROKEN PIN MUST FAIL THIS RIG, NOT CRASH IT. Nothing is called in a DLL once
// GetModuleHandle says it is gone, and what is held in an unmapped image is never
// released: it is parked in a kernel wait and the process ends around it. The
// pinned worker gets the same care: the worker stops the watcher itself from inside
// its hook, and this host calls nothing in the DLL while the worker leaves, because
// a host thread inside a DLL whose last holder is that worker would return into
// unmapped code the instant a broken pin let the image go with it. What the worker
// did is read from outside -- a count the DLL keeps in this process's own memory,
// and the journal file it closes. The pinned worker runs WITHOUT a log: the log's
// flusher is a std::thread whose CRT reference would keep the image mapped and hide
// a pin that did nothing.
//
// Every directory this makes sits directly under the exe's own and is named
// journal_unload_test_<pid>_<tick>, the convention tools\run_jobs.py cleans up
// after a passing rig and keeps after a failing one. The names under it are short
// on purpose: the log's own file names are cut at MAX_PATH.
#include <windows.h>

#include <d3d11.h>   // the device types only; nothing here links d3d11.lib

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_checks = 0;
int g_bad = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) return;
    std::printf("  FAIL  %s\n", what);
    ++g_bad;
}

// Layout shared with journal_unload_dll.cpp. It lives here, in the host's memory,
// so what DllMain and the hook recorded outlive the unmap they describe.
struct Probe {
    volatile LONG detachCalls;
    volatile LONG detachReservedNull;
    volatile LONG detachThread;
    volatile LONG hookThread;
    volatile LONG hookCalls;
};
Probe g_probes[8] = {};

const wchar_t kLeaf[] = L"journal_unload_dll.dll";

std::wstring exeDir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
}

bool writeWhole(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == text.size();
}

std::string readWhole(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(f);
    return out;
}

// Every file in `dir` matching `pattern`, joined.
std::string readMatching(const std::wstring& dir, const wchar_t* pattern) {
    std::string text;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return text;
    do {
        text += readWhole(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return text;
}

size_t countOf(const std::string& text, const char* needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

// The line of `text` that contains `needle`, or empty.
std::string lineWith(const std::string& text, const char* needle) {
    const size_t at = text.find(needle);
    if (at == std::string::npos) return std::string();
    const size_t before = text.rfind('\n', at);
    const size_t start = before == std::string::npos ? 0 : before + 1;
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    return text.substr(start, end - start);
}

void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeTree(path);
            } else {
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

template <class Cond>
bool waitUntil(Cond cond, DWORD timeoutMs) {
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        if (cond()) return true;
        if (GetTickCount64() - t0 >= timeoutMs) return false;
        Sleep(5);
    }
}

// Can the file be opened with no sharing at all? Not while any other handle to it
// is open, which is what tells that the worker has closed the journal.
bool openExclusive(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

// The DLL's exports, resolved once. After a FreeLibrary these stay valid exactly
// as long as the module stays mapped, which is what the rig is asking; nothing
// here is called once GetModuleHandle says the module is gone.
struct Api {
    HMODULE module = nullptr;
    std::wstring leaf;   // the file name the loader lists it under
    void (*setProbe)(void*) = nullptr;
    void (*setHold)(HANDLE, HANDLE, BOOL) = nullptr;
    BOOL (*init)(const wchar_t*, const wchar_t*, BOOL) = nullptr;
    void (*tick)() = nullptr;
    BOOL (*workerRunning)() = nullptr;
    BOOL (*pinAgain)() = nullptr;
    BOOL (*pinBogus)() = nullptr;
    BOOL (*poolSubmit)(HANDLE, HANDLE, HANDLE) = nullptr;
    BOOL (*rawThread)(HANDLE, HANDLE, HANDLE) = nullptr;
    const void* code = nullptr;   // an address inside the module, for VirtualQuery
};

bool loadApi(const std::wstring& path, Api& api) {
    api = Api();
    const size_t slash = path.find_last_of(L"\\/");
    api.leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
    api.module = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!api.module) return false;
    auto get = [&](const char* name) { return GetProcAddress(api.module, name); };
    api.setProbe = reinterpret_cast<decltype(api.setProbe)>(get("JuSetProbe"));
    api.setHold = reinterpret_cast<decltype(api.setHold)>(get("JuSetHold"));
    api.init = reinterpret_cast<decltype(api.init)>(get("JuInit"));
    api.tick = reinterpret_cast<decltype(api.tick)>(get("JuTick"));
    api.workerRunning = reinterpret_cast<decltype(api.workerRunning)>(get("JuWorkerRunning"));
    api.pinAgain = reinterpret_cast<decltype(api.pinAgain)>(get("JuPinAgain"));
    api.pinBogus = reinterpret_cast<decltype(api.pinBogus)>(get("JuPinBogus"));
    api.poolSubmit = reinterpret_cast<decltype(api.poolSubmit)>(get("JuPoolSubmit"));
    api.rawThread = reinterpret_cast<decltype(api.rawThread)>(get("JuRawThread"));
    api.code = reinterpret_cast<const void*>(get("JuTick"));
    return api.setProbe && api.setHold && api.init && api.tick && api.workerRunning &&
           api.pinAgain && api.pinBogus && api.poolSubmit && api.rawThread && api.code;
}

// Is `code` still in a mapped image? The test the module handle cannot fool: an
// address in a range something else has since been mapped over is not "mapped".
bool imageMapped(const void* code, HMODULE module) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(code, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT &&
           mbi.Type == MEM_IMAGE && mbi.AllocationBase == module;
}

// Both halves of "still there": the loader still lists it, and its code is mapped.
bool stillMapped(const Api& api) {
    return GetModuleHandleW(api.leaf.c_str()) == api.module && imageMapped(api.code, api.module);
}

bool unmapped(const Api& api) { return GetModuleHandleW(api.leaf.c_str()) == nullptr; }

HANDLE newEvent(bool signaled = false) {
    return CreateEventW(nullptr, TRUE, signaled ? TRUE : FALSE, nullptr);
}

// The fixtures: a journal the worker will find and read, and a Status.
std::string event(const char* name) {
    return std::string("{ \"timestamp\":\"2026-09-29T10:00:00Z\", \"event\":\"") + name + "\" }\n";
}

void writeFixtures(const std::wstring& saved) {
    writeWhole(saved + L"\\Journal.ju.log", event("Fileheader") + event("LoadGame"));
    writeWhole(saved + L"\\Status.json",
               "{ \"timestamp\":\"2026-09-29T10:00:00Z\", \"event\":\"Status\", \"Flags\":16, "
               "\"Flags2\":0, \"GuiFocus\":0 }\n");
}

// ---------------------------------------------- a thread of the module, held
//
// Two shapes of thread that nothing holds the module for: a CreateThread thread
// and a pool callback of ui_surfaces.cpp's shape. Each is started inside the DLL
// and blocked there until the host says.

using Submit = BOOL (*)(HANDLE, HANDLE, HANDLE);

// The DLL is NOT pinned. The thread is held inside the module, the host lets go of
// the module, and the image has to go. The thread is never released: it is parked
// in a kernel wait and the process ends around it. Events are leaked on purpose for
// the same reason.
void controlThread(const std::wstring& dll, Submit Api::*submit, Probe& probe, const char* label) {
    char what[200];
    Api a;
    if (!loadApi(dll, a)) {
        std::snprintf(what, sizeof(what), "control, %s: the test DLL loads", label);
        check(false, what);
        return;
    }
    a.setProbe(&probe);
    HANDLE entered = newEvent(), release = newEvent(), done = newEvent();
    std::snprintf(what, sizeof(what), "control, %s: the thread is started", label);
    check((a.*submit)(entered, release, done) != FALSE, what);
    std::snprintf(what, sizeof(what), "control, %s: the thread is inside the DLL", label);
    check(WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0, what);
    const DWORD self = GetCurrentThreadId();
    std::snprintf(what, sizeof(what), "control, %s: FreeLibrary reports success", label);
    check(FreeLibrary(a.module) != FALSE, what);
    const bool gone = unmapped(a);
    std::snprintf(what, sizeof(what),
                  "CONTROL, %s: with the pin left out, FreeLibrary unmaps the DLL while a thread of "
                  "it is still running there -- the early unmap this rig exists to see", label);
    check(gone, what);
    std::snprintf(what, sizeof(what),
                  "control, %s: DllMain(DETACH) ran on the host's own thread, as the unload of an "
                  "unheld DLL does", label);
    check(probe.detachCalls == 1 && probe.detachReservedNull == 1 &&
              probe.detachThread == static_cast<LONG>(self), what);
    // No call into the DLL: its thread would return into an image that is gone. If
    // the control did not see the unmap the image is still here, and the thread
    // can be let go.
    if (!gone) SetEvent(release);
}

void pinnedThread(const std::wstring& dll, const std::wstring& saved, Submit Api::*submit,
                  Probe& probe, const char* label) {
    char what[200];
    Api a;
    if (!loadApi(dll, a)) {
        std::snprintf(what, sizeof(what), "pinned, %s: the test DLL loads", label);
        check(false, what);
        return;
    }
    a.setProbe(&probe);
    // Pinned by an earlier run in this process if the image is still here; asking
    // again is harmless, and covers the case that run failed and this is fresh.
    a.init(saved.c_str(), L"", TRUE);
    HANDLE entered = newEvent(), release = newEvent(), done = newEvent();
    std::snprintf(what, sizeof(what), "pinned, %s: the thread is started", label);
    check((a.*submit)(entered, release, done) != FALSE, what);
    std::snprintf(what, sizeof(what), "pinned, %s: the thread is inside the DLL", label);
    check(WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0, what);

    std::snprintf(what, sizeof(what), "pinned, %s: FreeLibrary on the pinned module reports success", label);
    check(FreeLibrary(a.module) != FALSE, what);
    const bool mapped = stillMapped(a);
    std::snprintf(what, sizeof(what),
                  "pinned, %s: the module is still mapped after FreeLibrary, with a thread of it "
                  "running there", label);
    check(mapped, what);
    std::snprintf(what, sizeof(what), "pinned, %s: DllMain(DETACH) did not run", label);
    check(probe.detachCalls == 0, what);
    if (!mapped) {
        std::printf("        (the module is gone: nothing further is called in it)\n");
        return;
    }
    SetEvent(release);
    std::snprintf(what, sizeof(what), "pinned, %s: released, the thread finishes and returns", label);
    check(WaitForSingleObject(done, 10000) == WAIT_OBJECT_0, what);
    // Whatever ran it goes on to run another: one that is already released
    // completes at once.
    HANDLE entered2 = newEvent(), release2 = newEvent(true), done2 = newEvent();
    std::snprintf(what, sizeof(what),
                  "pinned, %s: a second thread of the module's runs after the first returned", label);
    check((a.*submit)(entered2, release2, done2) != FALSE &&
              WaitForSingleObject(done2, 10000) == WAIT_OBJECT_0, what);
    Sleep(50);
    std::snprintf(what, sizeof(what), "pinned, %s: the module is still mapped afterwards", label);
    check(stillMapped(a), what);
    a.setProbe(nullptr);
}

// ---------------------------------------------- the real worker

// A DLL loaded, its worker started and held inside its first file call. False, with
// the failed check already recorded, when it cannot get that far. No log: see the
// header for why the worker runs without one here.
struct Held {
    Api api;
    HANDLE release = nullptr;
};

bool startHeldWorker(const std::wstring& dll, const std::wstring& saved, bool pin, Probe& probe,
                     const char* label, Held& held) {
    char what[160];
    if (!loadApi(dll, held.api)) {
        std::snprintf(what, sizeof(what), "%s: the test DLL loads", label);
        check(false, what);
        return false;
    }
    HANDLE entered = newEvent();
    held.release = newEvent();
    held.api.setProbe(&probe);
    // Released, the worker stops the watcher itself from inside its own hook: no
    // call into the DLL is made from here while it finishes and leaves.
    held.api.setHold(entered, held.release, TRUE);
    std::snprintf(what, sizeof(what), "%s: the watcher is configured", label);
    check(held.api.init(saved.c_str(), L"", pin ? TRUE : FALSE) != FALSE, what);
    held.api.tick();   // the first tick starts the worker
    std::snprintf(what, sizeof(what), "%s: the worker is inside its first file call", label);
    const bool inside = WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0;
    check(inside, what);
    return inside;
}

// Not a control. The worker's thread is started by the static UCRT's
// _beginthreadex, which references the module of the thread routine and ends the
// thread through FreeLibraryAndExitThread. What FreeLibrary does to it is printed,
// with where the unload completes: the finding that the review's F1 does not
// reproduce for the worker. Nothing here fails on it -- the pin does not depend on
// the CRT, and this is the CRT's behaviour, not this code's.
void observeUnpinnedWorker(const std::wstring& dll, const std::wstring& saved) {
    Held h;
    Probe& probe = g_probes[2];
    if (!startHeldWorker(dll, saved, false, probe, "the real worker, no pin", h)) return;
    Api& a = h.api;
    FreeLibrary(a.module);
    const bool held = !unmapped(a);
    std::printf("        note: the real worker, no pin: FreeLibrary %s the module while the worker was "
                "inside a file call%s\n",
                held ? "left" : "UNMAPPED",
                held ? " (the CRT's thread-start reference, ucrt\\startup\\thread.cpp)" : "");
    if (!held) return;   // parked, and never released: the process ends around it
    SetEvent(h.release);   // the worker stops itself and leaves; nothing is called in the DLL meanwhile
    const bool later = waitUntil([&] { return unmapped(a); }, 10000);
    std::printf("        note: ...and once the worker stopped the module %s; DllMain(DETACH) %s on the "
                "worker's own thread\n",
                later ? "unmapped" : "was STILL mapped",
                probe.detachThread != 0 && probe.detachThread == probe.hookThread ? "ran" : "did not run");
}

void pinnedWorker(const std::wstring& dll, const std::wstring& saved, Probe& probe) {
    const char* label = "pinned, the real worker";
    char what[200];
    Held h;
    if (!startHeldWorker(dll, saved, true, probe, label, h)) return;
    Api& a = h.api;
    std::snprintf(what, sizeof(what), "%s: a second call to the once-only pin still says pinned", label);
    check(a.pinAgain() != FALSE, what);
    std::snprintf(what, sizeof(what), "%s: pinning an address in no module is refused", label);
    check(a.pinBogus() == FALSE, what);

    std::snprintf(what, sizeof(what), "%s: FreeLibrary on the pinned module reports success", label);
    check(FreeLibrary(a.module) != FALSE, what);
    const bool mapped = stillMapped(a);
    std::snprintf(what, sizeof(what),
                  "%s: the module is still mapped after FreeLibrary, with the worker inside a file call",
                  label);
    check(mapped, what);
    std::snprintf(what, sizeof(what),
                  "%s: DllMain(DETACH) did not run, so nothing was torn down under the worker", label);
    check(probe.detachCalls == 0, what);
    if (!mapped) {
        std::printf("        (the module is gone: nothing further is called in it)\n");
        return;
    }
    std::snprintf(what, sizeof(what), "%s: the worker is still inside the file call", label);
    check(a.workerRunning() != FALSE, what);
    std::snprintf(what, sizeof(what), "%s: the hook saw the worker's thread", label);
    check(probe.hookThread != 0, what);

    // From here until the worker has gone NOTHING is called in the DLL (see the
    // header). Released, the worker runs its pass, stops the watcher itself and
    // leaves; what it did is read from outside.
    SetEvent(h.release);
    std::snprintf(what, sizeof(what),
                  "%s: released, the worker runs its whole pass (status, walk, open, tail)", label);
    check(waitUntil([&] { return probe.hookCalls >= 4; }, 10000), what);
    const std::wstring journal = saved + L"\\Journal.ju.log";
    std::snprintf(what, sizeof(what), "%s: the worker leaves: the journal it held open is closed", label);
    check(waitUntil([&] { return openExclusive(journal); }, 10000), what);
    Sleep(150);   // its exit, and any unmap a broken pin would let it bring, is over well within this
    const bool stillThere = stillMapped(a);
    std::snprintf(what, sizeof(what),
                  "%s: the module is still mapped after the worker has gone -- pinned for the life of "
                  "the process", label);
    check(stillThere, what);
    std::snprintf(what, sizeof(what), "%s: still no DllMain(DETACH) after the worker left", label);
    check(probe.detachCalls == 0, what);
    if (!stillThere) {
        std::printf("        (the module is gone: nothing further is called in it)\n");
        return;
    }
    std::snprintf(what, sizeof(what), "%s: the worker has left", label);
    check(a.workerRunning() == FALSE, what);
    for (int i = 0; i < 100; ++i) a.tick();
    Sleep(50);
    std::snprintf(what, sizeof(what), "%s: a tick after shutdown starts no other worker", label);
    check(a.workerRunning() == FALSE, what);
    a.setProbe(nullptr);
}

// ---------------------------------------------- the pin's log lines

// A DLL with its log open and the pin taken: the lines the pin writes, and its
// once-only rule. The instance is pinned, so leaving it mapped with its flusher is
// harmless.
void pinLogLines(const std::wstring& dll, const std::wstring& saved, const std::wstring& logs) {
    Api a;
    if (!loadApi(dll, a)) { check(false, "the pin's log lines: the test DLL loads"); return; }
    a.setProbe(&g_probes[5]);
    a.init(saved.c_str(), logs.c_str(), TRUE);   // opens the log, then pins
    check(a.pinAgain() != FALSE, "the pin's log: a second call to the once-only pin still says pinned");
    check(a.pinBogus() == FALSE, "the pin's log: pinning an address in no module is refused");
    std::string log;
    const bool said = waitUntil(
        [&] {
            log = readMatching(logs, L"edvr_ju_*.log");
            return log.find("graphics module pinned=1 (path=") != std::string::npos &&
                   log.find("graphics module pinned=0 (error ") != std::string::npos;
        },
        10000);
    check(said, "the pin's log: both of its lines reach the log");
    check(countOf(log, "graphics module pinned=1 (path=") == 1,
          "the pin's log: graphics module pinned=1 is said once, however often the pin is asked for");
    check(lineWith(log, "graphics module pinned=1 (path=").find("journal_unload_dll") != std::string::npos,
          "the pin's log: ...and that line names the DLL it pinned");
    check(countOf(log, "graphics module pinned=0 (error ") == 1,
          "the pin's log: the refused pin says graphics module pinned=0 with its error, once");
    check(stillMapped(a), "the pin's log: the DLL is still mapped");
    a.setProbe(nullptr);
}

// Not a control either. The log's flusher is a std::thread, started by the first
// note of an open log, so the CRT holds the DLL for it exactly as it does for the
// worker -- and the flusher is stopped only by shutdown(), which is what DllMain's
// FreeLibrary branch runs. So in a session with logging on, which is the default,
// that branch cannot be reached at all, pinned or not. Printed, not asserted, and
// last: the image is left mapped, flusher and all, for the rest of the process.
void observeUnpinnedLog(const std::wstring& dll, const std::wstring& saved, const std::wstring& logs) {
    Api a;
    if (!loadApi(dll, a)) { check(false, "the log observation: the test DLL loads"); return; }
    Probe& probe = g_probes[6];
    a.setProbe(&probe);
    a.init(saved.c_str(), logs.c_str(), FALSE);   // opens the log and writes to it: the flusher starts
    FreeLibrary(a.module);
    const bool held = stillMapped(a);
    std::printf("        note: a DLL with its log open (the flusher is a std::thread), no pin: FreeLibrary "
                "%s the module, and DllMain(DETACH) %s\n",
                held ? "left" : "UNMAPPED", probe.detachCalls == 0 ? "did not run" : "ran");
    if (held) a.setProbe(nullptr);
}

// ---------------------------------------------- the real proxy
//
// build\d3d11.dll as shipped: loaded, given a device through its own
// D3D11CreateDevice, and its own log has to carry the pin line. That is the call
// in initOnceCallback. FreeLibrary is not asked of it here: the pin is proven
// above on the same code, and a real proxy that failed to hold would be a hooked
// device with its DLL gone, which is a crash and not a failure.

void realProxy(const std::wstring& proxy, const std::wstring& logs) {
    // The proxy's log goes where this says, whatever the runner has set for the
    // rig: EDVR_LOG_DIR moves it, and EDVR_LOG_DIR_FOR would limit the move to a
    // directory this exe is not necessarily in.
    SetEnvironmentVariableW(L"EDVR_LOG_DIR", logs.c_str());
    SetEnvironmentVariableW(L"EDVR_LOG_DIR_FOR", nullptr);
    HMODULE module = LoadLibraryExW(proxy.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    check(module != nullptr, "the real graphics proxy loads");
    if (!module) return;
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(module, "D3D11CreateDevice"));
    check(create != nullptr, "the real graphics proxy exports D3D11CreateDevice");
    if (!create) return;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                              &device, nullptr, &context);
    check(SUCCEEDED(hr) && device && context, "a WARP device is created through the real proxy");
    std::string log;
    const bool said = waitUntil(
        [&] {
            log = readMatching(logs, L"edvr_gfx_*.log");
            return log.find("graphics module pinned=") != std::string::npos;
        },
        10000);
    check(said, "the real proxy's own log carries the pin line: the call in initOnceCallback ran");
    check(countOf(log, "graphics module pinned=1 (path=") == 1 && countOf(log, "pinned=0") == 0,
          "the real proxy pinned itself, once");
    check(lineWith(log, "graphics module pinned=1 (path=").find("d3d11.dll") != std::string::npos,
          "...and that line names a d3d11.dll");
    if (context) context->Release();
    if (device) device->Release();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Unbuffered: a run that dies takes its buffered lines with it, and the lines
    // before the death are exactly what says where.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring exe = exeDir();
    if (exe.empty()) {
        std::printf("  FAIL  could not find this executable's own directory\n");
        return 1;
    }
    const std::wstring dll = exe + L"\\" + kLeaf;
    const std::wstring base = exe + L"\\journal_unload_test_" + std::to_wstring(GetCurrentProcessId()) +
                              L"_" + std::to_wstring(GetTickCount64());
    const std::wstring saved = base + L"\\s";
    const std::wstring logs = base + L"\\l";
    const std::wstring logs2 = base + L"\\q";
    const std::wstring proxyLogs = base + L"\\p";
    const std::wstring copies = base + L"\\c";
    // Two more images of the DLL, under other names, for the runs that have to be
    // fresh after the first has been pinned and stays mapped.
    const std::wstring dllForLog = copies + L"\\journal_unload_dll_log.dll";
    const std::wstring dllForFlusher = copies + L"\\journal_unload_dll_flush.dll";
    CreateDirectoryW(base.c_str(), nullptr);
    for (const std::wstring* dir : {&saved, &logs, &logs2, &proxyLogs, &copies}) {
        CreateDirectoryW(dir->c_str(), nullptr);
    }
    CopyFileW(dll.c_str(), dllForLog.c_str(), FALSE);
    CopyFileW(dll.c_str(), dllForFlusher.c_str(), FALSE);
    writeFixtures(saved);
    std::printf("journal unload test -- the graphics DLL's pin against a real FreeLibrary\n");

    // The unpinned runs first, while the DLL unmaps each time and the next load is
    // a fresh image: a pinned instance stays mapped for the rest of the process and
    // is what the next load of its path would return.
    controlThread(dll, &Api::rawThread, g_probes[0], "a CreateThread thread");
    controlThread(dll, &Api::poolSubmit, g_probes[1], "a thread-pool callback");
    observeUnpinnedWorker(dll, saved);
    pinnedWorker(dll, saved, g_probes[3]);
    pinnedThread(dll, saved, &Api::rawThread, g_probes[4], "a CreateThread thread");
    pinnedThread(dll, saved, &Api::poolSubmit, g_probes[7], "a thread-pool callback");
    pinLogLines(dllForLog, saved, logs);
    observeUnpinnedLog(dllForFlusher, saved, logs2);
    if (argc > 1 && argv[1] && *argv[1]) {
        realProxy(argv[1], proxyLogs);
    } else {
        std::printf("        (no proxy path given: the real proxy's pin line is not checked)\n");
    }

    removeTree(base);   // best effort: what a pinned DLL still holds is left to the runner
    if (g_bad) {
        std::printf("\nJOURNAL UNLOAD TEST FAILED (%d of %d)\n", g_bad, g_checks);
        return 1;
    }
    std::printf("  ok    %d assertion(s): unpinned, FreeLibrary unmaps a DLL under a CreateThread thread "
                "and a pool callback and the rig sees it; pinned, the module stays mapped, "
                "DllMain(DETACH) does not run, and the real worker, the raw thread and the callback "
                "all finish cleanly once released, the worker leaving the module mapped behind it; "
                "the pin logs once, and the real proxy's own log says it ran\n", g_checks);
    std::printf("\nJOURNAL UNLOAD TEST PASSED\n");
    return 0;
}
