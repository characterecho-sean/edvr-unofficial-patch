// The worker that reads Elite's terrain checkerboard rendering (design doc section 84): see terrain_checkerboard.h for the rule
// (no file on the render thread), terrain_checkerboard_reader.h for what is read and terrain_checkerboard_notice.h for the words.
//
// THE LIFETIME is the journal worker's (journal_watch.cpp startWorker): a detached std::thread, an exception net around its body,
// a stop flag and a wake event, a session that is never freed so a worker still winding down cannot touch freed memory. std::thread
// starts it through the static UCRT's _beginthreadex, which holds the module of the thread routine, and the graphics DLL is pinned
// (module_pin.h) at the first device creation, before the first Present tick gets here: either alone keeps the code mapped under
// the worker. It starts from the Present thread on the first VR frame boundary, never from DllMain.
//
// WHAT THE LOG SHOWS. The worker's first line says it started; the first read and every change after it get one line each (at
// most tcn::kLogMax); a worker that could not start, or faulted for good, says so. No `vr terrain checkerboard:` line at all means
// the worker never started: a flat session, a build from before this, or a VR frame boundary that never reached the menu's tick.
// A read that found the option off, or found nothing, is a line too (OFF, unknown): silence is never "read, off".
#include "terrain_checkerboard.h"

#include "terrain_checkerboard_reader.h"
#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"

#include <windows.h>

#include <atomic>
#include <new>
#include <string>
#include <thread>

namespace edvr {

namespace {

struct Session {
    std::wstring folder;
    std::wstring gameDir;
    uint32_t pollMs = tcn::kPollMs;
    HANDLE wake = nullptr;              // auto-reset: stop, and an eager change
    std::atomic<bool> stop{false};
    std::atomic<bool> exited{false};
    std::atomic<bool> started{false};
    FaultBudget budget{"terrain_checkerboard.worker", 3};
};

// The current session. Sessions are never freed (see the header comment). One per start.
std::atomic<Session*> g_session{nullptr};
// tcn::pack(state, version): the one word the render thread loads. 0 = nothing read yet.
std::atomic<uint32_t> g_published{0};
// The once-only start, and the stop: a tick after shutdown starts nothing.
std::atomic<bool> g_startTried{false};
std::atomic<bool> g_stopped{false};

// The rig's override of where to read and how often. Plain storage with no destructor, set before the first tick.
struct TestPaths {
    bool set = false;
    wchar_t folder[MAX_PATH] = {};
    wchar_t gameDir[MAX_PATH] = {};
    uint32_t pollMs = tcn::kPollMs;
};
TestPaths g_test;

// One pass: read, and when something changed log it (bounded) and, when the STATE changed, publish the new version. The line is
// written BEFORE the word is published, so a toast line the menu writes after it sees the state always follows the read line.
void pass(tcn::Monitor& monitor, int* logged) {
    bool stateMoved = false;
    if (!monitor.poll(&stateMoved)) return;
    const tcn::Reading& r = monitor.published();
    char line[1100];
    switch (tcn::logDue(logged)) {
    case tcn::LogDue::Line:
        tcn::formatLog(line, sizeof(line), r.state, r.preset, r.source, r.value, r.reason);
        Log::get().note("%s", line);
        break;
    case tcn::LogDue::Limit:
        tcn::formatLimitLine(line, sizeof(line));
        Log::get().note("%s", line);
        break;
    case tcn::LogDue::Nothing:
        break;
    }
    if (stateMoved) {
        const uint32_t before = g_published.load(std::memory_order_relaxed);
        g_published.store(tcn::pack(r.state, tcn::nextVersion(before)), std::memory_order_release);
    }
}

void workerBody(Session& s) {
    SetThreadDescription(GetCurrentThread(), L"edvr-terrain-checkerboard");
    char line[400];
    tcn::formatStartLine(line, sizeof(line), GetCurrentThreadId(), s.pollMs);
    Log::get().note("%s", line);
    tcn::Monitor monitor;
    monitor.configure(s.folder, s.gameDir);
    int logged = 0;
    while (!s.stop.load(std::memory_order_acquire)) {
        // The pass inside the project's fault containment: a fault is absorbed and counted, and when the budget is spent the worker
        // says so and ends, leaving the last published state as it was.
        if (!guardedBudget(s.budget, [&] { pass(monitor, &logged); }) && !s.budget.shouldRun()) {
            tcn::formatFaultLine(line, sizeof(line));
            Log::get().note("%s", line);
            break;
        }
        WaitForSingleObject(s.wake, s.pollMs);
    }
}

void workerMain(Session* sp) {
    Session& s = *sp;
    // No exception leaves this thread: an uncaught one is std::terminate, which is the game.
    try {
        workerBody(s);
    } catch (...) {
        try {
            char line[400];
            tcn::formatFaultLine(line, sizeof(line));
            Log::get().note("%s", line);
        } catch (...) {
        }
    }
    s.exited.store(true, std::memory_order_release);
}

bool startWorker(Session& s) {
    try {
        std::thread(workerMain, &s).detach();
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

void terrainCheckerboardTick() {
    if (g_startTried.load(std::memory_order_relaxed)) return;
    // VR only: the flat profile never starts the reader (it returns before the menu tick's VR branch anyway; this is the second lock).
    if (!runtimeVrProfile() || g_stopped.load(std::memory_order_relaxed)) return;
    g_startTried.store(true, std::memory_order_relaxed);

    Session* s = new (std::nothrow) Session;
    if (s) {
        s->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s->wake) {
            delete s;
            s = nullptr;
        }
    }
    char line[400];
    if (!s) {
        tcn::formatStartFailedLine(line, sizeof(line));
        Log::get().note("%s", line);
        return;
    }
    if (g_test.set) {
        s->folder = g_test.folder;
        s->gameDir = g_test.gameDir;
        s->pollMs = g_test.pollMs;
    } else {
        s->folder = flatEliteGraphicsFolder();
        s->gameDir = executableDirectory();
    }
    g_session.store(s, std::memory_order_release);
    s->started.store(true, std::memory_order_relaxed);
    if (!startWorker(*s)) {
        tcn::formatStartFailedLine(line, sizeof(line));
        Log::get().note("%s", line);
        s->exited.store(true, std::memory_order_release);
    }
}

void terrainCheckerboardPublished(tcn::State* state, uint32_t* version) {
    const uint32_t word = g_published.load(std::memory_order_acquire);
    if (state) *state = tcn::unpackState(word);
    if (version) *version = tcn::unpackVersion(word);
}

bool terrainCheckerboardOn() {
    return runtimeVrProfile() && tcn::unpackState(g_published.load(std::memory_order_acquire)) == tcn::State::On;
}

void terrainCheckerboardShutdown() {
    g_stopped.store(true, std::memory_order_relaxed);
    if (Session* s = g_session.load(std::memory_order_acquire)) {
        s->stop.store(true, std::memory_order_release);
        SetEvent(s->wake);
    }
}

void terrainCheckerboardTestSetPaths(const wchar_t* optionsFolder, const wchar_t* gameDir, uint32_t pollMs) {
    g_test.set = true;
    wcsncpy_s(g_test.folder, optionsFolder ? optionsFolder : L"", _TRUNCATE);
    wcsncpy_s(g_test.gameDir, gameDir ? gameDir : L"", _TRUNCATE);
    g_test.pollMs = pollMs ? pollMs : tcn::kPollMs;
}

bool terrainCheckerboardTestWorkerRunning() {
    const Session* s = g_session.load(std::memory_order_acquire);
    return s && s->started.load(std::memory_order_relaxed) && !s->exited.load(std::memory_order_acquire);
}

void terrainCheckerboardTestReset() {
    if (Session* s = g_session.exchange(nullptr)) {
        s->stop.store(true, std::memory_order_release);
        SetEvent(s->wake);
        for (int i = 0; i < 500 && !s->exited.load(std::memory_order_acquire); ++i) Sleep(10);
    }
    g_published.store(0, std::memory_order_release);
    g_startTried.store(false, std::memory_order_relaxed);
    g_stopped.store(false, std::memory_order_relaxed);
    g_test = TestPaths();
}

}  // namespace edvr
