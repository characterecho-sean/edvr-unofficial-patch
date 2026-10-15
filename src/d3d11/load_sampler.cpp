#include "load_sampler.h"

#include <windows.h>

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
bool g_wasWanted = false;   // frame thread only
uint64_t g_wantedSinceMs = 0;   // sampler thread only: when the current wanted run began (0 = not wanted)

// NtQueryInformationThread, for a thread's TEB (the stack limits live in its NT_TIB: StackBase at +0x08, StackLimit at +0x10).
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

// One thread the sampler stops. The render thread's handle is the watchdog's (never closed); the window thread's is ours (never closed).
struct Target {
    const char* name = "";
    HANDLE handle = nullptr;
    uint64_t lo = 0, hi = 0;
    DWORD tid = 0;
    stall::Capture* cap = nullptr;            // allocated once, outside any stop
    bool resolved = false;
    const char* why = "not resolved yet";
    uint64_t resolveMs = 0;
    uint32_t samples = 0, failed = 0;         // in the last 5 s
    uint32_t samplesTotal = 0;
    double maxStopMs = 0.0;                   // in the last 5 s
    std::map<std::string, uint32_t> exclusive, callers;   // in the last 5 s
    uint64_t lineMs = 0;
};
Target g_render{"render"};
Target g_window{"window"};
uint64_t g_lastLineNever = 0;

bool readU64(uintptr_t address, uint64_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const uint64_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

struct FindWindow {
    DWORD pid;
    HWND found;
    DWORD tid;
    bool foreground;
};
BOOL CALLBACK findWindowProc(HWND hwnd, LPARAM lp) {
    FindWindow* f = reinterpret_cast<FindWindow*>(lp);
    DWORD wpid = 0;
    const DWORD wtid = GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != f->pid || !IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0) return TRUE;
    const bool fg = hwnd == GetForegroundWindow();
    if (!f->found || (fg && !f->foreground)) {
        f->found = hwnd;
        f->tid = wtid;
        f->foreground = fg;
    }
    return TRUE;
}

// The game window's thread: found by enumeration (a visible, titled window of this process; the foreground one first), opened by id with the
// stop rights, and its stack limits read from its TEB. A window thread that is the render thread is not sampled twice.
void resolveWindow(uint64_t nowMs) {
    g_window.resolveMs = nowMs;
    FindWindow f{GetCurrentProcessId(), nullptr, 0, false};
    EnumWindows(findWindowProc, reinterpret_cast<LPARAM>(&f));
    if (!f.found || !f.tid) {
        g_window.why = "no visible titled window of this process";
        return;
    }
    if (g_render.resolved && f.tid == g_render.tid) {
        g_window.why = "the window's thread is the render thread (sampled as render)";
        return;
    }
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, f.tid);
    if (!h) {
        g_window.why = "OpenThread refused (the window thread cannot be stopped)";
        return;
    }
    // The thread id came from an enumeration of this process, but it can be reused: confirm the handle is still this process's thread
    // before any stop. A thread of another process is never suspended.
    if (GetProcessIdOfThread(h) != GetCurrentProcessId()) {
        CloseHandle(h);
        g_window.why = "the window thread's handle belongs to another process (refused, never stopped)";
        return;
    }
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    NtQueryInformationThreadFn query = ntdll ? reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread")) : nullptr;
    ThreadBasicInfo info{};
    uint64_t base = 0, limit = 0;
    if (!query || query(h, 0, &info, sizeof(info), nullptr) != 0 || !info.tebBase ||
        !readU64(reinterpret_cast<uintptr_t>(info.tebBase) + 0x08, &base) || !readU64(reinterpret_cast<uintptr_t>(info.tebBase) + 0x10, &limit) ||
        !base || limit >= base) {
        CloseHandle(h);
        g_window.why = "its stack limits are not readable from the TEB";
        return;
    }
    g_window.handle = h;
    g_window.tid = f.tid;
    g_window.lo = limit;
    g_window.hi = base;
    g_window.resolved = true;
    g_window.why = "resolved by window enumeration";
    if (!g_window.cap) g_window.cap = new (std::nothrow) stall::Capture;
    if (!g_window.cap) {
        g_window.resolved = false;
        g_window.why = "no memory for the capture";
    }
}

std::string resolvedName(const stall::Resolved& r) {
    if (!r.known) return "?";
    char b[64];
    std::snprintf(b, sizeof(b), "%s+0x%x", r.module, static_cast<unsigned>(r.rva));
    return b;
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
    stall::Resolved r0;
    stall::resolveFrame(pcs[0], r0);
    ++t.exclusive[resolvedName(r0)];
    std::string sig;
    for (unsigned i = 0; i < n && i < 3; ++i) {
        stall::Resolved r;
        stall::resolveFrame(pcs[i], r);
        if (i) sig += " | ";
        sig += resolvedName(r);
    }
    ++t.callers[sig];
}

// The top k entries, most frequent first.
std::vector<std::pair<std::string, uint32_t>> topOf(const std::map<std::string, uint32_t>& m, size_t k) {
    std::vector<std::pair<std::string, uint32_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (v.size() > k) v.resize(k);
    return v;
}

// The 5 s lines of one thread: its samples, its top exclusive locations, its top three-frame callers.
void lineOf(Target& t, uint64_t nowMs) {
    if (!t.lineMs) {
        t.lineMs = nowMs;
        return;
    }
    if (nowMs - t.lineMs < 5000) return;
    t.lineMs = nowMs;
    Log::get().note("load sampler: %s thread (tid %lu): %u samples in the last 5 s, %u failed, longest stop %.3f ms, %s; %u taken this run",
                    t.name, static_cast<unsigned long>(t.tid), t.samples, t.failed, t.maxStopMs,
                    t.resolved ? t.why : t.why, t.samplesTotal);
    if (t.samples) {
        std::string ex;
        for (const auto& e : topOf(t.exclusive, 8)) {
            char b[120];
            std::snprintf(b, sizeof(b), "%s%s x%u", ex.empty() ? "" : ", ", e.first.c_str(), static_cast<unsigned>(e.second));
            ex += b;
        }
        std::string cs;
        for (const auto& e : topOf(t.callers, 5)) {
            char b[200];
            std::snprintf(b, sizeof(b), "%s[%s] x%u", cs.empty() ? "" : "; ", e.first.c_str(), static_cast<unsigned>(e.second));
            cs += b;
        }
        Log::get().note("load sampler: %s thread top exclusive: %s", t.name, ex.c_str());
        Log::get().note("load sampler: %s thread top callers (three frames, innermost first): %s", t.name, cs.c_str());
    } else {
        Log::get().note("load sampler: %s thread: no sample in the last 5 s (sampled, nothing counted, or not stopped; see the reason above)", t.name);
    }
    t.samples = 0;
    t.failed = 0;
    t.maxStopMs = 0.0;
    t.exclusive.clear();
    t.callers.clear();
}

// The sampler's thread: wakes every 100 ms. Never exits: the thread is detached and the process ends it.
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
        if (!g_window.resolved && now - g_window.resolveMs >= 2000) resolveWindow(now);
        sampleOne(g_render);
        sampleOne(g_window);
        lineOf(g_render, now);
        lineOf(g_window, now);
        // The "never sampled" line: wanted for 30 s with no sample from any thread says why, once per 30 s.
        if (now - g_wantedSinceMs >= 30000 && g_render.samplesTotal == 0 && g_window.samplesTotal == 0 && now - g_lastLineNever >= 30000) {
            g_lastLineNever = now;
            Log::get().note("load sampler: never sampled in %.0f s while wanted (render: %s; window: %s)",
                            static_cast<double>(now - g_wantedSinceMs) / 1000.0, g_render.why, g_window.why);
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
        Log::get().note("load sampler: armed (TEMPORARY, read only, no key): the render thread and the game window's thread are stopped once every "
                        "100 ms while wanted, and the top locations are logged every 5 s per thread");
    }
}

}  // namespace edvr
