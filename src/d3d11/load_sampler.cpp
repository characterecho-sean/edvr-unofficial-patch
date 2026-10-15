#include "load_sampler.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../common/log.h"
#include "../common/stall_sampler.h"
#include "stall_watch.h"

namespace edvr {
namespace {

// The sampler's own state. Written by the frame thread (wanted, why) and by the sampler's thread (everything else).
std::atomic<bool> g_wanted{false};
std::atomic<const char*> g_why{"not started"};
std::atomic<bool> g_started{false};
bool g_wasWanted = false;         // frame thread only
uint64_t g_wantedSinceMs = 0;     // sampler thread only: when the current wanted run began (0 = not wanted)
uint64_t g_lastLineNever = 0;     // sampler thread only

// F25: the 5 s thread census (Toolhelp snapshot, GetThreadTimes deltas, start addresses) runs on the sampler's thread.
constexpr uint64_t kCensusMs = 5000;
uint64_t g_censusMs = 0;                       // sampler thread only
std::map<DWORD, uint64_t> g_prevCpu;           // sampler thread only: kernel+user 100 ns per thread at the last census
bool g_haveCensus = false;                     // sampler thread only

// NtQueryInformationThread: a thread's TEB (the stack limits live in its NT_TIB) and its Win32 start address.
using NtQueryInformationThreadFn = LONG(WINAPI*)(HANDLE, LONG, PVOID, ULONG, PULONG);
struct ThreadBasicInfo {
    LONG status;
    PVOID tebBase;
    ULONG_PTR uniqueProcess;
    ULONG_PTR uniqueThread;
    ULONG_PTR affinity;
    LONG priority;
    LONG basePriority;
};
constexpr LONG kThreadBasicInformation = 0;
constexpr LONG kThreadQuerySetWin32StartAddress = 9;

NtQueryInformationThreadFn ntQueryThread() {
    static NtQueryInformationThreadFn fn = [] {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        return nt ? reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(nt, "NtQueryInformationThread")) : nullptr;
    }();
    return fn;
}

bool readU64(uintptr_t address, uint64_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const uint64_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The stack limits of a thread of this process, from its TEB: the reserved region's base and committed limit.
bool tebStackLimits(HANDLE h, uint64_t* lo, uint64_t* hi) {
    NtQueryInformationThreadFn query = ntQueryThread();
    ThreadBasicInfo info{};
    uint64_t base = 0, limit = 0;
    if (!query || query(h, kThreadBasicInformation, &info, sizeof(info), nullptr) != 0 || !info.tebBase) return false;
    if (!readU64(reinterpret_cast<uintptr_t>(info.tebBase) + 0x08, &base) || !readU64(reinterpret_cast<uintptr_t>(info.tebBase) + 0x10, &limit)) return false;
    if (!base || limit >= base) return false;
    *lo = limit;
    *hi = base;
    return true;
}

// One thread the sampler stops. The render thread's handle is the watchdog's (never closed); a CPU thread's is ours (closed when dropped).
struct Target {
    const char* kind = "";
    HANDLE handle = nullptr;
    bool ownsHandle = false;
    uint64_t lo = 0, hi = 0;
    DWORD tid = 0;
    std::string where;                        // the thread's start address, as module+RVA
    stall::Capture* cap = nullptr;            // allocated once, outside any stop
    bool resolved = false;
    const char* why = "not resolved yet";
    uint64_t resolveMs = 0;
    uint32_t samples = 0, failed = 0, noGame = 0, samplesTotal = 0;   // in the last 5 s; samplesTotal since the run began
    double maxStopMs = 0.0;                   // in the last 5 s
    std::map<std::string, uint32_t> firstGame;   // in the last 5 s: the first EliteDangerous64.exe frame of each stack, innermost first
    uint64_t lineMs = 0;
};
Target g_render{"render"};
Target g_cpu[3] = {Target{"cpu 1"}, Target{"cpu 2"}, Target{"cpu 3"}};

std::string resolvedName(const stall::Resolved& r) {
    if (!r.known) return "?";
    char b[96];
    std::snprintf(b, sizeof(b), "%s+0x%x", r.module, static_cast<unsigned>(r.rva));
    return b;
}

// The render thread: the stall sampler's own registered slot.
void resolveRender(uint64_t nowMs) {
    void* h = nullptr;
    uint64_t lo = 0, hi = 0;
    uint32_t tid = 0;
    g_render.resolveMs = nowMs;
    if (!stallWatchRenderTarget(&h, &lo, &hi, &tid)) {
        g_render.why = "the stall sampler has not registered the render thread (no Present yet, or the sampler is off)";
        return;
    }
    g_render.handle = static_cast<HANDLE>(h);
    g_render.ownsHandle = false;
    g_render.lo = lo;
    g_render.hi = hi;
    g_render.tid = static_cast<DWORD>(tid);
    g_render.resolved = true;
    g_render.why = "resolved from the stall sampler's slot";
    if (!g_render.cap) g_render.cap = new (std::nothrow) stall::Capture;
    if (!g_render.cap) {
        g_render.resolved = false;
        g_render.why = "no memory for the capture";
    }
}

// F25: one row of the census.
struct ThreadRow {
    DWORD tid = 0;
    uint64_t delta = 0;     // 100 ns of CPU since the last census
    std::string where;      // the start address, module+RVA
    bool edvr = false;      // this DLL's own thread (its start address is in this module, or it is the sampler)
};

// The census: every thread of this process, its CPU since the last census, and where it started. Owner: the sampler's thread, never while a
// thread is stopped.
std::vector<ThreadRow> censusThreads() {
    std::vector<ThreadRow> rows;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return rows;
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    std::map<DWORD, uint64_t> now;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            FILETIME c{}, e{}, k{}, u{};
            uint64_t cpu = 0;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                cpu = (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
            }
            uintptr_t start = 0;
            NtQueryInformationThreadFn query = ntQueryThread();
            ULONG got = 0;
            if (query) query(h, kThreadQuerySetWin32StartAddress, &start, sizeof(start), &got);
            CloseHandle(h);
            ThreadRow row;
            row.tid = te.th32ThreadID;
            now[row.tid] = cpu;
            const auto prev = g_prevCpu.find(row.tid);
            if (g_haveCensus && prev != g_prevCpu.end() && cpu >= prev->second) row.delta = cpu - prev->second;
            stall::Resolved r;
            if (start) stall::resolveFrame(start, r);
            row.where = start ? resolvedName(r) : "?";
            row.edvr = (start && r.edvr) || row.tid == self;
            rows.push_back(row);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    g_prevCpu = std::move(now);
    return rows;
}

// Opens a CPU thread for sampling: the suspend rights, the owning process checked before any stop, and the stack limits from its TEB.
bool openCpuTarget(Target& t, DWORD tid) {
    t.tid = tid;
    t.resolved = false;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) {
        t.why = "OpenThread refused (the thread cannot be stopped)";
        return false;
    }
    if (GetProcessIdOfThread(h) != GetCurrentProcessId()) {
        CloseHandle(h);
        t.why = "the thread belongs to another process (refused, never stopped)";
        return false;
    }
    uint64_t lo = 0, hi = 0;
    if (!tebStackLimits(h, &lo, &hi)) {
        CloseHandle(h);
        t.why = "its stack limits are not readable from the TEB";
        return false;
    }
    t.handle = h;
    t.ownsHandle = true;
    t.lo = lo;
    t.hi = hi;
    t.resolved = true;
    t.why = "opened by the census";
    if (!t.cap) t.cap = new (std::nothrow) stall::Capture;
    if (!t.cap) {
        t.resolved = false;
        t.why = "no memory for the capture";
    }
    return t.resolved;
}

void dropCpuTarget(Target& t) {
    if (t.handle && t.ownsHandle) CloseHandle(t.handle);
    t.handle = nullptr;
    t.ownsHandle = false;
    t.resolved = false;
    t.tid = 0;
    t.where.clear();
}

// F25: picks the top three non-EDVR threads by CPU since the last census (and not the render thread) as the CPU targets.
void assignCpuTargets(std::vector<ThreadRow> rows) {
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const ThreadRow& r) { return r.edvr; }), rows.end());
    std::sort(rows.begin(), rows.end(), [](const ThreadRow& a, const ThreadRow& b) { return a.delta > b.delta; });
    std::string top;
    unsigned shown = 0;
    for (const ThreadRow& r : rows) {
        if (shown >= 8) break;
        if (g_render.resolved && r.tid == g_render.tid) continue;
        char b[160];
        std::snprintf(b, sizeof(b), "%s tid %lu %s %.1f ms", shown ? ";" : "", static_cast<unsigned long>(r.tid), r.where.c_str(),
                      static_cast<double>(r.delta) / 10000.0);
        top += b;
        ++shown;
    }
    Log::get().note("load sampler: census (last 5 s, %u threads of this process, EDVR's own skipped): top CPU %s", static_cast<unsigned>(rows.size()),
                    shown ? top.c_str() : " none (no CPU since the last census)");
    unsigned slot = 0;
    for (const ThreadRow& r : rows) {
        if (slot >= 3) break;
        if (g_render.resolved && r.tid == g_render.tid) continue;
        if (r.delta == 0) break;
        Target& t = g_cpu[slot];
        if (t.resolved && t.tid == r.tid) {
            t.where = r.where;
            ++slot;
            continue;
        }
        dropCpuTarget(t);
        const bool ok = openCpuTarget(t, r.tid);
        t.where = r.where;
        Log::get().note("load sampler: cpu slot %u = tid %lu (%s): %s", slot + 1, static_cast<unsigned long>(r.tid), r.where.c_str(),
                        ok ? "sampled from now" : t.why);
        ++slot;
    }
    for (; slot < 3; ++slot) dropCpuTarget(g_cpu[slot]);
}

// One stop of one thread: the capture is the stall sampler's own code. Everything after the resume is the walk and the names.
void sampleOne(Target& t) {
    if (!t.resolved || !t.handle || !t.cap) return;
    const bool captured = stall::captureThread(t.handle, t.lo, t.hi, *t.cap);
    if (!captured) {
        ++t.failed;
        return;
    }
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    const double stopMs = f.QuadPart > 0 ? static_cast<double>(t.cap->stoppedTicks) * 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
    if (stopMs > t.maxStopMs) t.maxStopMs = stopMs;
    uintptr_t pcs[stall::kMaxFrames] = {};
    const unsigned n = stall::unwindCapture(*t.cap, pcs, stall::kMaxFrames);
    if (!n) {
        ++t.failed;
        return;
    }
    ++t.samples;
    ++t.samplesTotal;
    // The innermost EliteDangerous64.exe frame among the first eight: a wait inside a system image shows which game function is waiting.
    bool found = false;
    for (unsigned i = 0; i < n && i < 8 && !found; ++i) {
        stall::Resolved r;
        stall::resolveFrame(pcs[i], r);
        if (r.known && stall::equalsNoCase(r.module, "EliteDangerous64.exe")) {
            ++t.firstGame[resolvedName(r)];
            found = true;
        }
    }
    if (!found) ++t.noGame;
}

// The top entries of a count map, most frequent first.
std::vector<std::pair<std::string, uint32_t>> topOf(const std::map<std::string, uint32_t>& m, size_t k) {
    std::vector<std::pair<std::string, uint32_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (v.size() > k) v.resize(k);
    return v;
}

// The 5 s line of one target: its samples, its longest stop, and the top first-game-frame aggregates.
void lineOf(Target& t, uint64_t nowMs) {
    if (!t.lineMs) {
        t.lineMs = nowMs;
        return;
    }
    if (nowMs - t.lineMs < 5000) return;
    t.lineMs = nowMs;
    Log::get().note("load sampler: %s tid %lu (start %s): %u samples in 5 s, %u failed, no game frame in %u, longest stop %.3f ms%s; %u since the run began (%s)",
                    t.kind, static_cast<unsigned long>(t.tid), t.where.empty() ? "?" : t.where.c_str(), t.samples, t.failed, t.noGame, t.maxStopMs,
                    t.maxStopMs > 1.0 ? " (over 1 ms)" : "", t.samplesTotal, t.resolved ? t.why : t.why);
    if (t.samples) {
        std::string top;
        for (const auto& e : topOf(t.firstGame, 8)) {
            char b[120];
            std::snprintf(b, sizeof(b), "%s%s x%u", top.empty() ? "" : ", ", e.first.c_str(), static_cast<unsigned>(e.second));
            top += b;
        }
        Log::get().note("load sampler: %s tid %lu first game frame (8 frames per stack): %s", t.kind, static_cast<unsigned long>(t.tid),
                        top.empty() ? " none" : top.c_str());
    }
    t.samples = 0;
    t.failed = 0;
    t.noGame = 0;
    t.maxStopMs = 0.0;
    t.firstGame.clear();
}

// The sampler's thread: wakes every 100 ms while wanted. Never exits: the thread is detached and the process ends it.
void run() {
    for (;;) {
        Sleep(100);
        const uint64_t now = GetTickCount64();
        if (!g_wanted.load(std::memory_order_acquire)) {
            g_wantedSinceMs = 0;
            continue;
        }
        if (!g_wantedSinceMs) g_wantedSinceMs = now;
        if (!g_render.resolved && now - g_render.resolveMs >= 2000) resolveRender(now);
        if (!g_haveCensus || now - g_censusMs >= kCensusMs) {
            const bool first = !g_haveCensus;
            std::vector<ThreadRow> rows = censusThreads();
            g_haveCensus = true;
            g_censusMs = now;
            if (!first) assignCpuTargets(rows);
            else Log::get().note("load sampler: first census (%u threads); CPU deltas start with the next one", static_cast<unsigned>(rows.size()));
        }
        sampleOne(g_render);
        for (Target& t : g_cpu) sampleOne(t);
        lineOf(g_render, now);
        for (Target& t : g_cpu) lineOf(t, now);
        // The "never sampled" line: wanted for 30 s with no sample from any thread says why, once per 30 s.
        bool anySample = g_render.samplesTotal > 0;
        for (const Target& t : g_cpu) anySample = anySample || t.samplesTotal > 0;
        if (now - g_wantedSinceMs >= 30000 && !anySample && now - g_lastLineNever >= 30000) {
            g_lastLineNever = now;
            Log::get().note("load sampler: never sampled in %.0f s while wanted (render: %s; cpu 1: %s)",
                            static_cast<double>(now - g_wantedSinceMs) / 1000.0, g_render.why, g_cpu[0].why);
        }
    }
}

}  // namespace

void loadSamplerTick(uint64_t nowMs, bool wanted, const char* why) {
    (void)nowMs;
    g_why.store(why ? why : "", std::memory_order_relaxed);
    if (wanted != g_wasWanted) {
        Log::get().note("load sampler: %s (%s)", wanted ? "sampling on" : "sampling off", why ? why : "");
        g_wasWanted = wanted;
    }
    g_wanted.store(wanted, std::memory_order_release);
    if (wanted && !g_started.exchange(true)) {
        std::thread(run).detach();
        Log::get().note("load sampler: armed (TEMPORARY, read only, no key): every 5 s the process's threads are censused (CPU and start address); the render "
                        "thread and the top three CPU threads are stopped every 100 ms while wanted, 8 frames per stack, aggregated by the first game frame");
    }
}

}  // namespace edvr
