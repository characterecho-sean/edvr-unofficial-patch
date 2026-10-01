// The rig for the breadcrumb heartbeat's writer thread (src\common\heartbeat_writer.h, src\common\proxy.cpp,
// docs/freeze-diagnostics-2026-10-01.md, commit 3).
//
// The heartbeat used to write edvr_breadcrumbs.txt on the render thread. It is written by a thread of its own now,
// and what has to survive the move is what the file MEANS: the last line before a crash or a kill must still say
// something true. Every check's label starts with its case id (H1..H10), which is how
// tools\heartbeat_writer_test\mutants.py tells a mutation that tripped the right check from one that tripped
// something else:
//
//   H1  THE RENDER THREAD DOES NOT WRITE: the sink runs on another thread, and a sink that blocks for 600 ms does
//       not slow post() down
//   H2  a writer held up by a slow disk writes the newest post when it comes back and never an old one after a new
//       one: the frame numbers in the file never go backwards
//   H3  in the ordinary case every post is written, in order
//   H4  the writer has no clock of its own: with no posts nothing is written, however long it waits (a hung render
//       thread stops the heartbeat)
//   H5  a record is whole: the frame and the uptime of one line were posted together (three hundred thousand
//       posts at full speed against a reader)
//   H6  close() and closeAndDrain(): a pending post is dropped, a post after the close is dropped, a write that
//       has begun is waited for -- for a bounded time -- and one that has not begun never begins
//   H7  THROUGH THE REAL breadcrumbHeartbeat (proxy.cpp) and the real breadcrumb file: lines appear, the frames in
//       them increase, and after breadcrumbHeartbeatClose none appears
//   H8  A CRASH: the process dies on an access violation after writing heartbeats; the file's last line is the
//       crash filter's, never a heartbeat, and at least one heartbeat is above it
//   H9  A KILL: the process is terminated while it is posting; the file's last line is a whole heartbeat line
//   H10 the code, by source text: breadcrumbHeartbeat posts and never writes; the crash filter closes the
//       heartbeat before its first line; DllMain closes it on both detach paths before the closing crumb
//
//   heartbeat_writer_test.exe --dry-run                   touches nothing
//   heartbeat_writer_test.exe --self-test [root]          root is the repository, for the H10 source pins
//   heartbeat_writer_test.exe --child-crash | --child-kill   (the processes H8 and H9 start; not for hands)
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "heartbeat_writer.h"
#include "proxy.h"

using namespace edvr;

namespace {
unsigned g_checks = 0, g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
}

bool waitFor(bool (*done)(void*), void* arg, unsigned ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        if (done(arg)) return true;
        Sleep(2);
    }
    return done(arg);
}

// ---- a sink that records who called it, with what, and can be told to block -------------------------------------
struct Call {
    DWORD thread;
    uint64_t frame, uptime;
};
std::mutex g_mutex;
std::vector<Call> g_calls;
std::atomic<unsigned> g_blockMs{0};
std::atomic<unsigned> g_inSink{0};

void recordingSink(uint64_t frame, uint64_t uptime) {
    g_inSink.fetch_add(1);
    const unsigned block = g_blockMs.load();
    if (block) Sleep(block);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_calls.push_back({GetCurrentThreadId(), frame, uptime});
    }
    g_inSink.fetch_sub(1);
}

std::vector<Call> calls() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_calls;
}

void reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_calls.clear();
    g_blockMs = 0;
}

double nowMs() {
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

HeartbeatWriter* fresh() {
    reset();
    auto* w = new HeartbeatWriter;   // leaked: the thread holds it until the process ends
    w->start(&recordingSink);
    return w;
}

// ---- H1 ---------------------------------------------------------------------------------------------------------
void threadCases() {
    HeartbeatWriter* w = fresh();
    const DWORD poster = GetCurrentThreadId();
    w->post(1, 10);
    struct Arg { HeartbeatWriter* w; } arg{w};
    waitFor([](void* a) { return static_cast<Arg*>(a)->w->written() >= 1; }, &arg, 3000);
    std::vector<Call> c = calls();
    check(c.size() == 1 && c[0].thread != poster && c[0].frame == 1 && c[0].uptime == 10,
          "H1.thread: the line is written by another thread than the one that posted, with the frame and uptime posted");
    // A sink stuck for 600 ms (a stalled disk): the poster is not held.
    g_blockMs = 600;
    w->post(2, 20);
    waitFor([](void*) { return g_inSink.load() > 0; }, nullptr, 2000);   // the writer is inside the slow sink now
    double worst = 0.0;
    for (uint64_t i = 3; i < 10; ++i) {
        const double t0 = nowMs();
        w->post(i, i * 10);
        const double dt = nowMs() - t0;
        if (dt > worst) worst = dt;
        Sleep(20);
    }
    check(worst < 250.0, "H1.nonblocking: post() returns while the writer is held up for 600 ms in its sink (the slowest took under 250 ms; it does not wait for the 600)");
    // H2 rides on the same run: what the writer wrote when it came back.
    waitFor([](void* a) { return static_cast<Arg*>(a)->w->written() >= 3; }, &arg, 4000);
    Sleep(100);
    c = calls();
    bool increasing = true;
    for (size_t i = 1; i < c.size(); ++i) increasing = increasing && c[i].frame > c[i - 1].frame;
    check(c.size() >= 3 && c.size() <= 4 && increasing && c.back().frame == 9,
          "H2.order: held up, it wrote the one it was in the middle of and then the NEWEST post (9), never an older one after a newer, and not one line per post");
    check(w->posts() == 9 && w->written() == c.size(), "H2.counts: nine posts, and the writer's own count of lines written is the sink's");
    w->stop();
}

// ---- H3, H4 -----------------------------------------------------------------------------------------------------
void ordinaryCases() {
    HeartbeatWriter* w = fresh();
    struct Arg { HeartbeatWriter* w; } arg{w};
    for (uint64_t i = 1; i <= 20; ++i) {
        w->post(i, i * 30);
        waitFor([](void* a) { return static_cast<Arg*>(a)->w->written() >= static_cast<Arg*>(a)->w->posts(); }, &arg, 2000);
    }
    std::vector<Call> c = calls();
    bool ordered = c.size() == 20;
    for (size_t i = 0; i < c.size(); ++i) ordered = ordered && c[i].frame == i + 1 && c[i].uptime == (i + 1) * 30;
    check(ordered, "H3.every: posts 30 s apart (here, ms apart) are each written, in order, with their own frame and uptime");
    // H4: no posts, nothing written.
    const uint64_t before = w->written();
    Sleep(600);
    check(w->written() == before && calls().size() == 20, "H4.silent: 600 ms with no post: nothing written (the writer has no clock of its own)");
    // A wake with nothing new writes nothing, and a writer with nothing to do is asleep: it costs no CPU.
    FILETIME created, exited, kernel0, user0, kernel1, user1;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel0, &user0);
    for (int i = 0; i < 6; ++i) {
        w->wake();
        Sleep(100);
    }
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel1, &user1);
    const auto ticks = [](const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    const double cpuMs = static_cast<double>((ticks(kernel1) - ticks(kernel0)) + (ticks(user1) - ticks(user0))) / 10000.0;
    check(w->written() == before && calls().size() == 20,
          "H4.spurious: six wakes with nothing new write nothing");
    check(cpuMs < 150.0, "H4.idle: and 600 ms of idle writers cost under 150 ms of CPU in all: a writer with nothing to do sleeps (it does not spin)");
    w->stop();
}

// ---- H5 ---------------------------------------------------------------------------------------------------------
void tearCases() {
    reset();
    auto* w = new HeartbeatWriter;
    static std::atomic<unsigned long long> bad{0}, seen{0};
    w->start([](uint64_t frame, uint64_t uptime) {
        ++seen;
        if (uptime != frame * 7) ++bad;
    });
    // A second reader of the same record, hammering it while the poster runs flat out: the writer thread reads it only
    // once per wake, which is too rare to catch a tear that lasts two stores.
    std::atomic<bool> go{true};
    std::atomic<unsigned long long> reads{0}, torn{0};
    std::thread reader([&] {
        uint64_t f = 0, u = 0;
        while (go.load(std::memory_order_relaxed)) {
            if (w->latest(f, u)) {
                reads.fetch_add(1, std::memory_order_relaxed);
                if (u != f * 7) torn.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    for (uint64_t i = 1; i <= 300000; ++i) {
        w->post(i, i * 7);
        if ((i & 0x3FF) == 0) Sleep(0);
    }
    Sleep(100);
    go = false;
    reader.join();
    check(seen.load() >= 10 && bad.load() == 0, "H5.whole: of every record the writer read under 300,000 posts at full speed, the uptime belongs to the frame");
    check(reads.load() > 100000 && torn.load() == 0,
          "H5.reader: and so of every record a second thread read, over a hundred thousand reads, while the poster ran flat out");
    w->stop();
}

// ---- H6 ---------------------------------------------------------------------------------------------------------
void closeCases() {
    // A close drops what is pending, and what is posted after it.
    {
        HeartbeatWriter* w = fresh();
        g_blockMs = 300;
        w->post(1, 10);
        waitFor([](void*) { return g_inSink.load() > 0; }, nullptr, 2000);   // writing post 1
        w->post(2, 20);                                                        // pending behind it
        w->close();
        w->post(3, 30);                                                        // after the close
        Sleep(700);
        const std::vector<Call> c = calls();
        check(c.size() == 1 && c[0].frame == 1 && w->closed(),
              "H6.close: the write under way finishes, the pending post is dropped and a post after the close is dropped: nothing is written behind a close");
        check(w->posts() == 2, "H6.dropped: and the post after the close is not even taken (two posts counted, not three)");
        w->stop();
    }
    // The one place a close can land that the writer's first look at it missed: after that look, before it marks itself
    // writing. The rig closes the writer from a hook placed exactly there.
    {
        HeartbeatWriter* w = fresh();
        w->setBetweenChecksHook([](void* a) { static_cast<HeartbeatWriter*>(a)->close(); }, w);
        w->post(1, 10);
        Sleep(300);
        check(calls().empty() && w->written() == 0 && w->closed(),
              "H6.late-close: a close that lands between the writer's last look and the moment it marks itself writing still stops the write");
        w->stop();
    }
    // closeAndDrain waits for a write that has begun, and returns only when it is over.
    {
        HeartbeatWriter* w = fresh();
        g_blockMs = 200;
        w->post(1, 10);
        waitFor([](void*) { return g_inSink.load() > 0; }, nullptr, 2000);
        const double t0 = nowMs();
        const bool drained = w->closeAndDrain(1000);
        const double waited = nowMs() - t0;
        check(drained && !w->writing() && calls().size() == 1 && waited > 20.0 && waited < 900.0,
              "H6.drain: closeAndDrain waited for the write that had begun (the line is in), and not for the full second it was allowed");
        w->post(2, 20);
        Sleep(300);
        check(calls().size() == 1, "H6.drain: and nothing more is written after it returns");
        w->stop();
    }
    // The wait is bounded: a disk that is stalling is not waited for while the process dies.
    {
        HeartbeatWriter* w = fresh();
        g_blockMs = 600;
        w->post(1, 10);
        waitFor([](void*) { return g_inSink.load() > 0; }, nullptr, 2000);
        const double t0 = nowMs();
        const bool drained = w->closeAndDrain(30);
        const double waited = nowMs() - t0;
        check(!drained && waited >= 25.0 && waited < 500.0, "H6.bounded: closeAndDrain(30) against a write stuck for 600 ms gives up after about 30 ms (not 600) and says so");
        Sleep(800);
        w->stop();
    }
    // A close that lands before the write has begun stops it from beginning.
    {
        HeartbeatWriter* w = fresh();
        w->close();
        w->post(1, 10);
        Sleep(200);
        check(calls().empty() && w->written() == 0, "H6.closed-first: a heartbeat posted after the close never begins");
        w->stop();
    }
}

// ---- the real file ----------------------------------------------------------------------------------------------
std::wstring exeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L'\\') + 1);
}

std::string readFile(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool isAlive(const std::string& line) { return line.find("gfx: alive, frame ") != std::string::npos; }

uint64_t aliveFrame(const std::string& line) {
    const size_t at = line.find("gfx: alive, frame ");
    return at == std::string::npos ? 0 : std::strtoull(line.c_str() + at + 18, nullptr, 10);
}

void deleteCrumbs() { DeleteFileW((exeDir() + L"edvr_breadcrumbs.txt").c_str()); }
std::vector<std::string> crumbLines() { return linesOf(readFile(exeDir() + L"edvr_breadcrumbs.txt")); }

// ---- H7 ---------------------------------------------------------------------------------------------------------
void realCases() {
    deleteCrumbs();
    Config::get().set("log.breadcrumb_heartbeat_seconds", "1");
    uint64_t frame = 100;
    const ULONGLONG until = GetTickCount64() + 3700;
    while (GetTickCount64() < until) {
        breadcrumbHeartbeat(frame++);
        Sleep(20);
    }
    Sleep(200);
    std::vector<std::string> lines = crumbLines();
    std::vector<uint64_t> frames;
    for (const std::string& l : lines)
        if (isAlive(l)) frames.push_back(aliveFrame(l));
    bool increasing = frames.size() >= 2;
    for (size_t i = 1; i < frames.size(); ++i) increasing = increasing && frames[i] > frames[i - 1];
    check(frames.size() >= 2 && frames.size() <= 5 && increasing && frames.front() > 100,
          "H7.lines: through the real breadcrumbHeartbeat, a one-second heartbeat for 3.7 s writes its lines into the real breadcrumb file, the frames increasing");
    breadcrumbHeartbeatClose();
    const size_t before = frames.size();
    const ULONGLONG until2 = GetTickCount64() + 2300;
    while (GetTickCount64() < until2) {
        breadcrumbHeartbeat(frame++);
        Sleep(20);
    }
    Sleep(200);
    size_t after = 0;
    for (const std::string& l : crumbLines()) after += isAlive(l) ? 1 : 0;
    check(after == before, "H7.closed: after breadcrumbHeartbeatClose another 2.3 s of heartbeats writes no further line");
    deleteCrumbs();
}

// ---- H8, H9: a process that dies ----------------------------------------------------------------------------------
int runChild(bool crash) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    Config::get().set("log.breadcrumb_heartbeat_seconds", "1");
    breadcrumbInstallCrashHandler();
    uint64_t frame = 1;
    const ULONGLONG until = GetTickCount64() + 2600;
    while (GetTickCount64() < until) {
        breadcrumbHeartbeat(frame++);
        Sleep(10);
    }
    if (crash) {
        volatile int* nothing = nullptr;
        *nothing = 1;   // the access violation the crash filter reports
        return 0;
    }
    Sleep(60000);       // the parent kills this process while it is "running"
    return 0;
}

// The number of heartbeat lines in the breadcrumb file now.
unsigned aliveCount() {
    unsigned n = 0;
    for (const std::string& l : crumbLines()) n += isAlive(l) ? 1u : 0u;
    return n;
}

// The exit code of a child run of this executable. With killWhenAlive true the parent waits until the child has written
// a heartbeat (however late a loaded machine starts it) and then terminates it; otherwise the child ends itself.
DWORD spawn(const wchar_t* mode, bool killWhenAlive) {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" " + mode;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 0xDEAD0001;
    DWORD code = 0xDEAD0002;
    if (killWhenAlive) {
        // Two heartbeats, so the kill lands with the writer idle most of the time and the trail already has more than one line.
        const ULONGLONG until = GetTickCount64() + 30000;
        while (GetTickCount64() < until && aliveCount() < 2) Sleep(20);
        Sleep(30);
        TerminateProcess(pi.hProcess, 0xC0DE);
    }
    if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 0xDEAD0003);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

void deathCases() {
    deleteCrumbs();
    const DWORD crashCode = spawn(L"--child-crash", false);
    std::vector<std::string> lines = crumbLines();
    size_t lastAlive = std::string::npos, firstCrash = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (isAlive(lines[i])) lastAlive = i;
        if (firstCrash == std::string::npos && lines[i].find("gfx: UNHANDLED exception") != std::string::npos) firstCrash = i;
    }
    check(crashCode == 0xC0000005u && firstCrash != std::string::npos,
          "H8.crash: the child died on its access violation and the crash filter wrote its line");
    check(lastAlive != std::string::npos && firstCrash != std::string::npos && lastAlive < firstCrash && !lines.empty() && !isAlive(lines.back()),
          "H8.last: at least one heartbeat is above the crash lines, none is behind them, and the file's last line is not a heartbeat");
    check(!lines.empty() && lines.back().find("gfx: stack:") != std::string::npos,
          "H8.meaning: the last line of a crashed session is the crash filter's own (its stack line), which is what it has always been");
    deleteCrumbs();
    const DWORD killCode = spawn(L"--child-kill", true);
    lines = crumbLines();
    check(killCode == 0xC0DEu && !lines.empty() && isAlive(lines.back()) && aliveFrame(lines.back()) > 1,
          "H9.kill: a killed session's last line is a whole heartbeat line with a frame number: it dated the death to the half-minute it always did");
    const std::string raw = readFile(exeDir() + L"edvr_breadcrumbs.txt");
    check(raw.size() > 2 && raw.compare(raw.size() - 2, 2, "\r\n") == 0, "H9.whole: and the file ends at a line end, not in the middle of one");
    deleteCrumbs();
}

// ---- H10 --------------------------------------------------------------------------------------------------------
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    std::string text = out.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());   // a checkout may have CRLF; the pins read LF
    return text;
}

// The text of the function that starts at `signature`, to its closing brace in column 0.
std::string functionAt(const std::string& text, const char* signature) {
    const size_t at = text.find(signature);
    if (at == std::string::npos) return std::string();
    const size_t end = text.find("\n}\n", at);
    return end == std::string::npos ? text.substr(at) : text.substr(at, end - at + 3);
}

std::string stripComments(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 2, "//") == 0) {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (s.compare(i, 2, "/*") == 0) {
            const size_t end = s.find("*/", i + 2);
            i = end == std::string::npos ? s.size() : end + 2;
        } else {
            out.push_back(s[i++]);
        }
    }
    return out;
}

bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

void sourceCases(const std::string& root) {
    const std::string proxy = slurp(root + "\\src\\common\\proxy.cpp");
    const std::string dll = slurp(root + "\\src\\d3d11\\d3d11_proxy.cpp");
    check(!proxy.empty() && !dll.empty(), "H10.read: the two sources were read");
    if (proxy.empty() || dll.empty()) return;
    const std::string heartbeat = stripComments(functionAt(proxy, "void breadcrumbHeartbeat(uint64_t frameNo) {"));
    check(!heartbeat.empty() && has(heartbeat, "g_heartbeat.post(frameNo, now / 1000);") && has(heartbeat, "g_heartbeat.start(&heartbeatSink);"),
          "H10.post: breadcrumbHeartbeat hands the frame and the uptime to the writer");
    check(!has(heartbeat, "breadcrumb(") && !has(heartbeat, "CreateFile") && !has(heartbeat, "WriteFile") && !has(heartbeat, "CloseHandle"),
          "H10.nofile: and it writes nothing itself: no breadcrumb(), CreateFile, WriteFile or CloseHandle in the function the render thread runs");
    const std::string sink = stripComments(functionAt(proxy, "void heartbeatSink(uint64_t frameNo, uint64_t uptimeSeconds) {"));
    check(has(sink, "gfx: alive, frame ") && has(sink, "s uptime") && has(sink, "breadcrumb(line);"),
          "H10.sink: the writer thread's sink writes the line the heartbeat has always written");
    const std::string filter = stripComments(functionAt(proxy, "LONG WINAPI edvrCrashFilter(EXCEPTION_POINTERS* info) {"));
    const size_t closeAt = filter.find("g_heartbeat.closeAndDrain(20);");
    const size_t firstCrumb = filter.find("breadcrumb(");
    const size_t guardAt = filter.find("InterlockedExchange(&entered, 1)");
    check(closeAt != std::string::npos && firstCrumb != std::string::npos && guardAt != std::string::npos && guardAt < closeAt && closeAt < firstCrumb,
          "H10.filter: the crash filter closes the heartbeat (bounded to 20 ms) after its re-entrancy guard and before its first crumb");
    const size_t exitAt = dll.find("edvr::breadcrumbHeartbeatClose();\n                edvr::breadcrumb(\"gfx: process exit\");");
    const size_t unloadAt = dll.find("edvr::breadcrumbHeartbeatClose();\n                edvr::breadcrumb(\"gfx: FreeLibrary unload\");");
    check(exitAt != std::string::npos && unloadAt != std::string::npos,
          "H10.detach: DllMain closes the heartbeat before its closing crumb on the process-exit path and on the FreeLibrary path");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("heartbeat_writer_test: dry-run (no thread, no file, no process)");
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--child-crash") == 0) return runChild(true);
    if (argc >= 2 && std::strcmp(argv[1], "--child-kill") == 0) return runChild(false);
    if (argc < 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
    threadCases();
    ordinaryCases();
    tearCases();
    closeCases();
    realCases();
    deathCases();
    if (argc >= 3) sourceCases(argv[2]);
    else std::puts("heartbeat_writer_test: source pins skipped (no repository root given)");
    if (g_failures) {
        std::printf("FAIL: heartbeat writer: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u heartbeat writer checks\n", g_checks);
    return 0;
}
