// F213: the on-foot flight recorder's writer thread, the real file sink and the activation (onfoot_recorder.h says what it is and how it stops).
//
// WHAT THE LOG SHOWS. The writer says when it opens a segment and when it closes one, naming the file; the frame thread says one line a window (native_on_foot_logwin.h:
// a second with log.on_foot_detail on) with the records and drops of each type. A window line with a type at 0 is a site that did not run; "not running: ..." is a
// recording that could not start; a log with on-foot time and detail on and no line at all is this code never being called.
#include "onfoot_recorder.h"

#include "../common/game_build.h"
#include "../common/log.h"
#include "native_on_foot_logwin.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <new>
#include <thread>

#ifdef EDVR_HAVE_VERSION_HEADER
#include "edvr_version.h"
#endif
#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

namespace edvr {
namespace onfootrec {

namespace {

constexpr DWORD kDrainMs = 100;              // the writer drains this often
constexpr uint64_t kIdleCloseMs = 2000;      // inactive this long: the session's file is closed (a flicker of the gate keeps one file)
constexpr unsigned kOpenTries = 3;

uint64_t ticksNow() noexcept {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

// The folder's files, through Win32. Names are ASCII.
class WinSink final : public FileSink {
 public:
    explicit WinSink(std::wstring dir) : dir_(std::move(dir)) {}
    ~WinSink() override { close(); }
    bool list(std::vector<DirEntry>* out) override {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir_ + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            DirEntry e;
            for (const wchar_t* c = fd.cFileName; *c; ++c) e.name += *c < 0x80 ? static_cast<char>(*c) : '?';
            e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            out->push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return true;
    }
    bool remove(const std::string& name) override { return DeleteFileW(pathOf(name).c_str()) != 0; }
    bool create(const std::string& name) override {
        close();
        // FILE_SHARE_READ: a checker may read the file while the game runs. CREATE_NEW: a name that exists is never overwritten.
        h_ = CreateFileW(pathOf(name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        return h_ != INVALID_HANDLE_VALUE;
    }
    bool write(const void* data, size_t n) override {
        if (h_ == INVALID_HANDLE_VALUE) return false;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (n) {
            const DWORD chunk = n > (1u << 20) ? (1u << 20) : static_cast<DWORD>(n);
            DWORD done = 0;
            if (!WriteFile(h_, p, chunk, &done, nullptr) || done == 0) return false;
            p += done;
            n -= done;
        }
        return true;
    }
    void close() override {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

 private:
    std::wstring pathOf(const std::string& name) const { return dir_ + L"\\" + std::wstring(name.begin(), name.end()); }
    std::wstring dir_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

struct Session {
    Recorder rec;
    HANDLE wake = nullptr;                       // auto-reset: stop
    std::atomic<bool> stop{false};
    std::atomic<bool> exited{false};
    std::wstring folder;
    // the writer's, read by the frame thread's window line
    std::atomic<bool> open{false};               // a segment is open
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint32_t> segments{0};
    std::atomic<uint32_t> stallMaxMs{0};
    std::atomic<uint64_t> lost{0};               // popped with no segment open
    std::atomic<const char*> why{"starting"};    // when the recording is not running
    // the producers'
    std::atomic<uint64_t> runtimePushes{0};
    std::atomic<uint64_t> ikTicks{0}, ikCount{0}, ikMaxTicks{0};
    // the segment header's
    std::mutex mutex;
    std::string keysJson, fileName;
};
std::atomic<Session*> g_session{nullptr};   // never freed
std::atomic<bool> g_failed{false};
std::atomic<const char*> g_failedWhy{""};

void stampNow(char* stamp, size_t cap, char* local, size_t localCap) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::snprintf(stamp, cap, "%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::snprintf(local, localCap, "%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

void raiseStall(Session& s, uint64_t startTicks) noexcept {
    const uint64_t freq = qpcFreq();
    const uint64_t ms = freq ? (ticksNow() - startTicks) * 1000u / freq : 0;
    uint32_t seen = s.stallMaxMs.load(std::memory_order_relaxed);
    while (ms > seen && !s.stallMaxMs.compare_exchange_weak(seen, static_cast<uint32_t>(ms), std::memory_order_relaxed)) {
    }
}

void workerBody(Session& s) {
    SetThreadDescription(GetCurrentThread(), L"edvr-onfoot-recorder");
    WinSink sink(s.folder);
    SegmentWriter writer(
        sink, Limits{},
        [&s](uint32_t segment, const std::string& fileName) {
            HeaderInfo h;
            char stamp[32], local[48];
            stampNow(stamp, sizeof(stamp), local, sizeof(local));
            h.build = EDVR_VERSION_STRING;
            h.gameBuild = gameBuildText() ? gameBuildText() : "";
            h.localTime = local;
            h.pid = GetCurrentProcessId();
            h.qpcFreq = qpcFreq();
            h.qpcAtOpen = ticksNow();
            h.segment = segment;
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                h.keysJson = s.keysJson;
                s.fileName = fileName;
            }
            s.segments.fetch_add(1, std::memory_order_relaxed);
            Log::get().note("native on foot recorder (F213): segment %u opened: %s", segment, fileName.c_str());
            return segmentHeader(h);
        });
    bool haveSession = false;
    uint64_t inactiveSince = 0;
    uint64_t lastBytes = 0;
    unsigned tries = 0;
    alignas(8) uint8_t buf[kSlotBytes];
    for (;;) {
        const bool stopping = s.stop.load(std::memory_order_acquire);
        const uint64_t nowMs = GetTickCount64();
        const bool act = g_active.load(std::memory_order_relaxed) && !stopping;
        if (act && !haveSession) {
            char stamp[32], local[48];
            stampNow(stamp, sizeof(stamp), local, sizeof(local));
            const uint64_t t0 = ticksNow();
            const bool ok = writer.begin(stamp);
            raiseStall(s, t0);
            if (ok) {
                haveSession = true;
                tries = 0;
                s.open.store(true, std::memory_order_release);
                s.why.store("", std::memory_order_relaxed);
            } else if (++tries >= kOpenTries) {
                s.why.store("could not create the file in edvr_logs\\onfoot", std::memory_order_relaxed);
                g_failedWhy.store("could not create the file in edvr_logs\\onfoot", std::memory_order_relaxed);
                g_failed.store(true, std::memory_order_release);
                g_active.store(false, std::memory_order_relaxed);
                Log::get().note("native on foot recorder (F213): not recording: could not create a file in edvr_logs\\onfoot (%u tries)", tries);
            }
        }
        // drain: everything published so far
        uint32_t size = 0;
        while (s.rec.ring().tryPop(buf, static_cast<uint32_t>(sizeof(buf)), &size)) {
            if (!haveSession) {
                s.lost.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const uint64_t t0 = ticksNow();
            writer.append(buf, size);
            raiseStall(s, t0);
        }
        if (haveSession) {
            const uint64_t t0 = ticksNow();
            writer.flush();
            raiseStall(s, t0);
            s.bytes.fetch_add(writer.stats().bytes - lastBytes, std::memory_order_relaxed);
            lastBytes = writer.stats().bytes;
            if (!writer.isOpen()) {   // a write failed: the recording stops here, said once
                s.why.store("a write to the file failed", std::memory_order_relaxed);
                g_failedWhy.store("a write to the file failed", std::memory_order_relaxed);
                g_failed.store(true, std::memory_order_release);
                g_active.store(false, std::memory_order_relaxed);
                Log::get().note("native on foot recorder (F213): not recording: a write to %s failed", writer.currentName().c_str());
            }
            if (act) inactiveSince = 0;
            else if (inactiveSince == 0) inactiveSince = nowMs;
            if (stopping || !writer.isOpen() || (!act && nowMs - inactiveSince >= kIdleCloseMs)) {
                const std::string name = writer.currentName();
                const SegmentWriter::Stats st = writer.stats();
                writer.end();
                s.open.store(false, std::memory_order_release);
                Log::get().note("native on foot recorder (F213): closed %s (the session: %llu records, %llu bytes, %u segment(s), %u older file(s) removed at the start, %u segment(s) removed by rotation)%s",
                                name.c_str(), static_cast<unsigned long long>(st.records), static_cast<unsigned long long>(st.bytes), st.segments,
                                st.retentionDeleted, st.rotationDeleted, stopping ? " at shutdown" : "");
                haveSession = false;
                inactiveSince = 0;
                lastBytes = writer.stats().bytes;
                s.why.store(stopping ? "stopped at shutdown" : "idle: not on foot or the detail log is off", std::memory_order_relaxed);
            }
        }
        if (stopping) break;
        WaitForSingleObject(s.wake, kDrainMs);
    }
}

void workerMain(Session* sp) {
    Session& s = *sp;
    // No exception leaves this thread: an uncaught one is std::terminate, which is the game.
    try {
        workerBody(s);
    } catch (...) {
        g_failedWhy.store("the writer thread faulted", std::memory_order_relaxed);
        g_failed.store(true, std::memory_order_release);
        g_active.store(false, std::memory_order_relaxed);
        s.open.store(false, std::memory_order_release);
        try {
            Log::get().note("native on foot recorder (F213): not recording: the writer thread faulted");
        } catch (...) {
        }
    }
    s.exited.store(true, std::memory_order_release);
}

// Frame thread, first activation: the ring, the folder, the thread. On any failure the recording is off for the session, said once.
Session* startSession() {
    static bool tried = false;
    if (tried) return g_session.load(std::memory_order_acquire);
    tried = true;
    const std::wstring base = Log::get().dir();
    const char* why = nullptr;
    Session* s = new (std::nothrow) Session;
    if (!s) why = "out of memory";
    else if (base.empty()) why = "the log has no folder";
    else if (!s->rec.init()) why = "the ring could not be allocated";
    else {
        s->folder = base + L"\\onfoot";
        CreateDirectoryW(s->folder.c_str(), nullptr);
        s->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s->wake) why = "no wake event";
    }
    if (!why) {
        try {
            std::thread(workerMain, s).detach();
        } catch (...) {
            why = "the writer thread did not start";
        }
    }
    if (why) {
        g_failedWhy.store(why, std::memory_order_relaxed);
        g_failed.store(true, std::memory_order_release);
        Log::get().note("native on foot recorder (F213): not recording: %s", why);
        return nullptr;   // a half-built session is left (never freed); nothing reads it
    }
    g_session.store(s, std::memory_order_release);
    return s;
}

// Frame thread only: the window's baselines.
struct Window {
    uint64_t startMs = 0;
    uint64_t pushed[kRecTypes] = {}, dropped[kRecTypes] = {}, rt = 0, bytes = 0, ikTicks = 0, ikCount = 0;
    uint32_t keysGeneration = ~0u;
    bool wasWanted = false;
} g_win;

void printWindow(Session* s, uint32_t seconds) {
    WindowFacts w;
    w.seconds = seconds;
    const bool failed = g_failed.load(std::memory_order_acquire);
    w.running = s && !failed && s->open.load(std::memory_order_acquire);
    if (!w.running) w.notRunning = failed ? g_failedWhy.load(std::memory_order_relaxed) : s ? s->why.load(std::memory_order_relaxed) : "no session";
    char file[64] = "";
    uint64_t ikTicks = 0, ikCount = 0, rt = 0, bytes = 0;
    if (s) {
        for (unsigned i = 0; i < kRecTypes; ++i) {
            const uint64_t p = s->rec.counters(i + 1).pushed.load(std::memory_order_relaxed), d = s->rec.counters(i + 1).dropped.load(std::memory_order_relaxed);
            w.records[i] = p - g_win.pushed[i];
            w.drops[i] = d - g_win.dropped[i];
            g_win.pushed[i] = p;
            g_win.dropped[i] = d;
        }
        rt = s->runtimePushes.load(std::memory_order_relaxed);
        bytes = s->bytes.load(std::memory_order_relaxed);
        ikTicks = s->ikTicks.load(std::memory_order_relaxed);
        ikCount = s->ikCount.load(std::memory_order_relaxed);
        w.runtimePushes = rt - g_win.rt;
        w.bytes = bytes - g_win.bytes;
        w.segments = s->segments.load(std::memory_order_relaxed);
        w.stallMs = s->stallMaxMs.exchange(0, std::memory_order_relaxed);
        w.ikCalls = ikCount - g_win.ikCount;
        const double perTick = qpcFreq() ? 1e6 / static_cast<double>(qpcFreq()) : 0.0;
        if (w.ikCalls) w.ikMeanUs = static_cast<double>(ikTicks - g_win.ikTicks) * perTick / static_cast<double>(w.ikCalls);
        w.ikMaxUs = static_cast<double>(s->ikMaxTicks.exchange(0, std::memory_order_relaxed)) * perTick;
        g_win.rt = rt;
        g_win.bytes = bytes;
        g_win.ikTicks = ikTicks;
        g_win.ikCount = ikCount;
        std::lock_guard<std::mutex> lock(s->mutex);
        std::snprintf(file, sizeof(file), "%s", s->fileName.empty() ? "none" : s->fileName.c_str());
    }
    w.file = file;
    char line[640];
    formatWindowLine(line, sizeof(line), w);
    Log::get().note("%s", line);
}

}  // namespace

uint64_t qpcNow() noexcept { return ticksNow(); }
uint64_t qpcFreq() noexcept {
    static const uint64_t f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return static_cast<uint64_t>(q.QuadPart);
    }();
    return f;
}

bool pushRecord(unsigned id, void* rec, uint32_t size) noexcept {
    if (!active()) return false;
    Session* s = g_session.load(std::memory_order_acquire);
    if (!s) return false;
    return s->rec.pushRaw(id, rec, size, ticksNow());
}

void noteIkCost(uint64_t ticks) noexcept {
    Session* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    s->ikTicks.fetch_add(ticks, std::memory_order_relaxed);
    s->ikCount.fetch_add(1, std::memory_order_relaxed);
    uint64_t seen = s->ikMaxTicks.load(std::memory_order_relaxed);
    while (ticks > seen && !s->ikMaxTicks.compare_exchange_weak(seen, ticks, std::memory_order_relaxed)) {
    }
}

void frame(bool wantActive, uint64_t nowMs, const std::string& keysJson, uint32_t keysGeneration) {
    static bool announced = false;
    if (!announced) {   // once, whatever the gate: proves this build has the recorder and that the frame tick reaches it
        announced = true;
        Log::get().note("native on foot recorder (F213): ready; it records to edvr_logs\\onfoot while log.on_foot_detail is on and native stereo is on foot "
                        "(32 MiB a file, a session keeps its newest 8; hotkey.on_foot_mark marks a moment)");
    }
    Session* s = g_session.load(std::memory_order_acquire);
    if (wantActive && !s && !g_failed.load(std::memory_order_acquire)) s = startSession();
    if (s && wantActive && keysGeneration != g_win.keysGeneration) {
        g_win.keysGeneration = keysGeneration;
        std::lock_guard<std::mutex> lock(s->mutex);
        s->keysJson = keysJson;
    }
    const bool run = wantActive && s && !g_failed.load(std::memory_order_acquire) && !s->stop.load(std::memory_order_relaxed);
    g_active.store(run, std::memory_order_relaxed);
    if (wantActive) {
        const uint32_t seconds = nofoot::logwin::window(g_win.startMs, nowMs);
        if (seconds) printWindow(s, seconds);
    } else if (g_win.wasWanted) {
        const uint32_t seconds = nofoot::logwin::flush(g_win.startMs, nowMs);
        if (seconds) printWindow(s, seconds);
    }
    g_win.wasWanted = wantActive;
}

void mark() {
    static std::atomic<uint32_t> count{0};
    const uint32_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
    const char* said = "recorder off";
    if (active()) {
        MarkRec r;
        initRecord(r);
        r.number = n;
        said = push(r) ? "recorded" : "recorder busy, not recorded";
    }
    Log::get().note("native on foot mark %u (F213): %s", n, said);
}

void shutdown() noexcept {
    g_active.store(false, std::memory_order_relaxed);
    Session* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    s->stop.store(true, std::memory_order_release);
    if (s->wake) SetEvent(s->wake);
}

}  // namespace onfootrec
}  // namespace edvr

extern "C" bool __stdcall edvrOnFootRecordPush(const void* record, uint32_t size) {
    using namespace edvr::onfootrec;
    if (!active() || !record || size < sizeof(Prefix) || size > kSlotBytes) return false;
    Session* s = g_session.load(std::memory_order_acquire);
    if (!s) return false;
    alignas(8) uint8_t buf[kSlotBytes];
    std::memcpy(buf, record, size);
    const unsigned id = reinterpret_cast<const Prefix*>(buf)->id;
    if (id != kRecRtEye && id != kRecRtRecentre) return false;   // the runtime owns these two types and no others
    s->runtimePushes.fetch_add(1, std::memory_order_relaxed);
    return s->rec.pushRaw(id, buf, size, 0);
}
