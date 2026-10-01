// Where was the render thread when the game stopped presenting? (docs/freeze-diagnostics-2026-10-01.md, issue 63)
//
// In every freeze of the issue 63 flight EDVR's own work was under 2 ms and the render thread simply stopped, at
// a different place in the game's frame each time: before the first submit, between the eyes, in the real
// Present, after it. Nothing recorded WHERE. A watchdog notices the thread going about 150 ms without a Present,
// stops it for the length of a context read and a stack copy, lets it go, and then -- on its own thread, with
// the render thread running again -- walks the copied stack and names the modules: nvwgf2umx, the game, the
// chained EDHM layer, an ntdll wait. One flight names the owner of the stall.
//
// THE SAFETY ARGUMENT, because this suspends the thread the game renders on.
//
//   1. WHAT RUNS WHILE THE THREAD IS STOPPED is one function, captureThread, and it calls exactly three things:
//      SuspendThread, GetThreadContext, ResumeThread (and QueryPerformanceCounter between them, which reads the
//      clock and takes no lock). It writes into a buffer that exists before the first suspend. It allocates
//      nothing, formats nothing, logs nothing, takes no lock of its own and calls no routine that could: the
//      stopped thread may be holding the heap lock, the loader lock, the log's spin lock or any lock in the
//      driver, and a watchdog that waited for one of them would never see it released -- it would be the
//      freeze, forever, and made by us. The stack is therefore COPIED while the thread is stopped (a bounded
//      memcpy of the bytes above its stack pointer) and WALKED AFTER IT HAS BEEN RESUMED, because
//      RtlLookupFunctionEntry, which the walk needs, reads ntdll's function tables under a lock the stopped
//      thread could be holding in the middle of a LoadLibrary. Chromium's and Firefox's samplers are built the
//      same way for the same reason.
//   2. THE RESUME IS UNCONDITIONAL. Every path after a successful SuspendThread reaches ResumeThread: a failed
//      GetThreadContext, a stack pointer outside the registered stack, a copy of zero bytes. The only code
//      between the two calls is straight-line with no early return. tools\stall_sampler_test holds this, with a
//      mutant that skips the resume on the failure path.
//   3. NO SEH IN THE STOPPED WINDOW. The copy is bounded by the stack limits the render thread registered
//      (GetCurrentThreadStackLimits, the reserved region), so it cannot read outside the stack; a structured
//      handler would be reached through exception dispatch, which looks up unwind data under the same locks.
//      If the stack pointer is not inside the registered region (the thread is running a fiber's stack) the
//      copy is skipped, the context alone is kept, and the sample names one frame.
//   4. SAME PROCESS, ONE THREAD, NO WRITES. The target's handle is a DuplicateHandle of its own pseudo-handle,
//      made by the thread itself on its first beat, with THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
//      THREAD_QUERY_LIMITED_INFORMATION. Nothing is enumerated, no thread is opened by id, nothing is written
//      to another thread's memory or registers (there is no SetThreadContext in this file), no remote thread,
//      no memory in another process. That is what a sampling profiler does, and it shares nothing with the
//      shapes an injector uses.
//   5. COST. When the thread is presenting the cost is one relaxed store per frame (beat) and about seven
//      wakeups a second of a thread that reads a clock and goes back to sleep. A stall costs at most three
//      stops per episode, each a few tens of microseconds, and the stops are rate limited across episodes.
//   6. THE WATCHDOG NEVER BLOCKS ON THE GAME. It takes no lock the game takes, writes to the log (a spin lock
//      and a memory append, not a file) only after the thread has been resumed, and does no file I/O at all.
//
// HEADER ONLY, so tools\stall_sampler_test can run the real code against a thread it blocks in a known place.
// The graphics half's glue (src\d3d11\stall_watch.cpp) adds the log, the configuration key and the Present beat.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace edvr::stall {

// ---- the policy: when to sample, pure so a rig can drive it with a fake clock ---------------------------------

// A stall is no Present for this long; one sample is taken when the age reaches each of these.
constexpr unsigned kSamplesPerEpisode = 3;
constexpr uint32_t kSampleAtMs[kSamplesPerEpisode] = {150, 500, 1000};

struct PolicyConfig {
    uint32_t at[kSamplesPerEpisode] = {kSampleAtMs[0], kSampleAtMs[1], kSampleAtMs[2]};
    double burst = 6.0;               // episodes that may start back to back
    double refillMs = 2000.0;         // one more may start every this long
    uint32_t sessionEpisodes = 200;   // and no more than this many in a session
};

struct Decision {
    bool sample = false;
    unsigned index = 0;               // which sample of the episode: 0, 1 or 2
    bool lateStart = false;           // the episode's first sample was refused and this one is its first
};

class StallPolicy {
public:
    explicit StallPolicy(const PolicyConfig& c = PolicyConfig{}) : cfg_(c), tokens_(c.burst) {}

    // One look at the render thread. `beatId` is anything that changes with every Present (the beat's clock
    // reading); `ageMs` is how long ago that Present was; `nowMs` is a monotonic clock for the rate limit.
    // `nextCheckMs` says when to look again if nothing is due: the time until the next sample's threshold, or a
    // short poll once an episode is finished and the next Present is awaited.
    Decision poll(uint64_t beatId, double ageMs, double nowMs, double* nextCheckMs) noexcept {
        refill(nowMs);
        if (beatId != beat_) {                 // a new Present: whatever was going on is over
            beat_ = beatId;
            taken_ = 0;
            started_ = false;
            skippedCounted_ = false;
        }
        Decision d;
        double next = kPollMs;
        // A look that comes late (the watchdog was not scheduled, the machine hiccuped) finds several
        // thresholds already passed: one sample, at the last of them, not three in a row for one moment.
        while (taken_ + 1 < kSamplesPerEpisode && ageMs >= static_cast<double>(cfg_.at[taken_ + 1])) ++taken_;
        if (taken_ < kSamplesPerEpisode) {
            const double need = static_cast<double>(cfg_.at[taken_]);
            if (ageMs < need) {
                next = need - ageMs;
            } else {
                const unsigned index = taken_++;
                bool allowed = started_;
                if (!started_) {
                    allowed = tokens_ >= 1.0 && episodes_ < cfg_.sessionEpisodes;
                    if (allowed) {
                        tokens_ -= 1.0;
                        ++episodes_;
                        started_ = true;
                        d.lateStart = index > 0;
                    } else if (!skippedCounted_) {
                        skippedCounted_ = true;
                        ++skipped_;
                    }
                }
                if (allowed) {
                    d.sample = true;
                    d.index = index;
                    ++samples_;
                }
                next = taken_ < kSamplesPerEpisode ? 1.0 : kPollMs;   // look again at once: the next threshold may be due too
            }
        }
        if (nextCheckMs) *nextCheckMs = next < 1.0 ? 1.0 : next;
        return d;
    }

    uint32_t episodes() const noexcept { return episodes_; }
    uint32_t skipped() const noexcept { return skipped_; }
    uint32_t samples() const noexcept { return samples_; }

    static constexpr double kPollMs = 50.0;

private:
    void refill(double nowMs) noexcept {
        if (lastMs_ == 0.0) lastMs_ = nowMs;
        if (nowMs > lastMs_) {
            tokens_ += (nowMs - lastMs_) / cfg_.refillMs;
            if (tokens_ > cfg_.burst) tokens_ = cfg_.burst;
            lastMs_ = nowMs;
        }
    }

    PolicyConfig cfg_;
    double tokens_;
    double lastMs_ = 0.0;
    uint64_t beat_ = 0;
    unsigned taken_ = 0;
    bool started_ = false;
    bool skippedCounted_ = false;
    uint32_t episodes_ = 0, skipped_ = 0, samples_ = 0;
};

// ---- capture: the only code that runs while the thread is stopped -------------------------------------------

// How much of the stack above the stack pointer is copied. Deep driver frames can be several kilobytes each;
// 32 KB reaches the game's frame from inside nvwgf2umx in every stack measured.
constexpr size_t kStackCopyBytes = 32 * 1024;
constexpr unsigned kMaxFrames = 14;

enum class CaptureStatus : uint8_t {
    Ok = 0,
    BadArguments,       // no handle, or no registered stack: nothing was stopped
    SuspendFailed,      // SuspendThread failed: nothing was stopped
    ContextFailed,      // the thread was stopped and resumed; no registers
    StackOutsideRange,  // stopped and resumed; registers only (a fiber's stack, or a stack pointer in the guard region)
};

inline const char* captureStatusName(CaptureStatus s) noexcept {
    switch (s) {
        case CaptureStatus::Ok: return "ok";
        case CaptureStatus::BadArguments: return "bad_arguments";
        case CaptureStatus::SuspendFailed: return "suspend_failed";
        case CaptureStatus::ContextFailed: return "context_failed";
        case CaptureStatus::StackOutsideRange: return "stack_outside_range";
    }
    return "unknown";
}

// Preallocated: one of these exists for the life of the watchdog, so nothing is allocated while a thread is stopped.
struct Capture {
    CONTEXT context;                      // CONTEXT's own 16-byte alignment holds
    CaptureStatus status = CaptureStatus::BadArguments;
    uint32_t stackBytes = 0;              // bytes copied (0 for a status other than Ok)
    uint64_t stackFrom = 0;               // the address the copy stands for: the stopped thread's RSP
    DWORD suspendResult = 0;              // what SuspendThread returned: the previous suspend count (0 when nobody else stopped it)
    DWORD resumeResult = 0;               // what ResumeThread returned: the count before the resume (1 when only we had stopped it)
    int64_t stoppedTicks = 0;             // QueryPerformanceCounter ticks from just before the stop to just after the resume
    alignas(16) unsigned char stack[kStackCopyBytes + 64];
};

// Stop `thread`, read its registers, copy the bytes at and above its stack pointer, let it go.
//
// `stackLo` and `stackHi` are the thread's reserved stack region as GetCurrentThreadStackLimits gave it to the
// thread itself when it registered. NO EARLY RETURN between SuspendThread and ResumeThread, no call but the
// three below and the clock, no exception handler: see the header's safety argument. noinline so the compiler
// cannot spread the window across the caller.
__declspec(noinline) inline bool captureThread(HANDLE thread, uint64_t stackLo, uint64_t stackHi, Capture& out) noexcept {
    out.status = CaptureStatus::BadArguments;
    out.stackBytes = 0;
    out.stackFrom = 0;
    out.suspendResult = 0;
    out.resumeResult = 0;
    out.stoppedTicks = 0;
    if (!thread || thread == INVALID_HANDLE_VALUE || !stackLo || stackHi <= stackLo) return false;

    LARGE_INTEGER t0{}, t1{};
    out.context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    QueryPerformanceCounter(&t0);
    // ---- the thread is stopped from here ------------------------------------------------------------------
    const DWORD stopped = SuspendThread(thread);
    if (stopped == static_cast<DWORD>(-1)) {
        out.status = CaptureStatus::SuspendFailed;
        return false;
    }
    const BOOL haveContext = GetThreadContext(thread, &out.context);
    uint32_t copied = 0;
    uint64_t from = 0;
    if (haveContext) {
        from = out.context.Rsp;
        if (from >= stackLo && from + sizeof(uint64_t) <= stackHi) {
            uint64_t n = stackHi - from;
            if (n > kStackCopyBytes) n = kStackCopyBytes;
            const volatile uint64_t* source = reinterpret_cast<const volatile uint64_t*>(from);
            uint64_t* target = reinterpret_cast<uint64_t*>(out.stack);
            const uint64_t words = n / sizeof(uint64_t);
            for (uint64_t i = 0; i < words; ++i) target[i] = source[i];
            copied = static_cast<uint32_t>(words * sizeof(uint64_t));
        }
    }
    const DWORD back = ResumeThread(thread);
    // ---- the thread is running again ----------------------------------------------------------------------
    QueryPerformanceCounter(&t1);
    out.suspendResult = stopped;
    out.resumeResult = back;
    out.stoppedTicks = t1.QuadPart - t0.QuadPart;
    out.stackFrom = from;
    out.stackBytes = copied;
    out.status = !haveContext ? CaptureStatus::ContextFailed
                              : (copied ? CaptureStatus::Ok : CaptureStatus::StackOutsideRange);
    return haveContext != FALSE;
}

// ---- unwinding the copy, after the resume --------------------------------------------------------------------

namespace detail {

inline bool lookupFunction(DWORD64 pc, DWORD64& imageBase, PRUNTIME_FUNCTION& function) noexcept {
    __try {
        function = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        function = nullptr;
        return false;
    }
}

inline bool unwindOne(DWORD64 imageBase, PRUNTIME_FUNCTION function, CONTEXT& c) noexcept {
    __try {
        PVOID handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, function, &c, &handlerData, &establisher, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A register that holds an address inside the stack that was copied now holds the same place in the copy, so the
// unwinder reads the copy and never the live stack (which has moved on since the thread was resumed).
inline void rebase(DWORD64& reg, DWORD64 from, DWORD64 to, DWORD64 copy) noexcept {
    if (reg >= from && reg < to) reg = copy + (reg - from);
}

inline void rebaseAll(CONTEXT& c, DWORD64 from, DWORD64 to, DWORD64 copy) noexcept {
    rebase(c.Rsp, from, to, copy);
    rebase(c.Rbp, from, to, copy);
    rebase(c.Rbx, from, to, copy);
    rebase(c.Rsi, from, to, copy);
    rebase(c.Rdi, from, to, copy);
    rebase(c.R12, from, to, copy);
    rebase(c.R13, from, to, copy);
    rebase(c.R14, from, to, copy);
    rebase(c.R15, from, to, copy);
}

}  // namespace detail

// The program counters of the captured stack, innermost first: the thread's own RIP, then one return address per
// frame, as far as the copy and the unwind data go (at most `max`). The first entry is always the RIP, so a
// capture with registers and no stack still names where the thread was. Returns how many.
//
// RtlVirtualUnwind reads each frame's saved registers and return address through the CONTEXT's stack pointer,
// which points into the copy (rebaseAll); a frame whose stack pointer would leave the copy ends the walk instead
// of being read. The walk is guarded: a corrupt unwind table, a JIT frame, a smashed stack stops it with the
// frames it had.
inline unsigned unwindCapture(const Capture& cap, uintptr_t* pcs, unsigned max) noexcept {
    if (!pcs || !max) return 0;
    if (cap.status != CaptureStatus::Ok && cap.status != CaptureStatus::StackOutsideRange) return 0;
    CONTEXT c = cap.context;
    unsigned n = 0;
    pcs[n++] = static_cast<uintptr_t>(c.Rip);
    if (cap.status != CaptureStatus::Ok || !cap.stackBytes) return n;
    const DWORD64 from = cap.stackFrom;
    const DWORD64 to = from + cap.stackBytes;
    const DWORD64 copy = reinterpret_cast<DWORD64>(cap.stack);
    const DWORD64 copyEnd = copy + cap.stackBytes;
    detail::rebaseAll(c, from, to, copy);
    while (n < max) {
        // The unwinder reads at and above RSP: never let it start a frame without room for the return address.
        if (c.Rsp < copy || c.Rsp + sizeof(DWORD64) > copyEnd || !c.Rip) break;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = nullptr;
        if (!detail::lookupFunction(c.Rip, imageBase, function)) break;
        const DWORD64 oldRsp = c.Rsp;
        if (function) {
            if (!detail::unwindOne(imageBase, function, c)) break;
        } else {
            // A frame with no unwind data is a leaf; its return address is at the stack pointer. Honoured once,
            // for the innermost frame (the same rule as vtable_hook.cpp's writer stack): deeper than that,
            // guessing twice compounds.
            if (n > 1) break;
            c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
            c.Rsp += sizeof(DWORD64);
        }
        detail::rebaseAll(c, from, to, copy);
        // A frame larger than what is left of the copy popped a return address from beyond it: not a frame.
        if (!c.Rip || c.Rsp <= oldRsp || c.Rsp > copyEnd) break;
        pcs[n++] = static_cast<uintptr_t>(c.Rip);
    }
    return n;
}

// ---- naming the modules, after the resume --------------------------------------------------------------------

struct Resolved {
    char module[32] = {};          // the file name of the image the address is in (cut at 31 characters: every module of a
                                   // game's process is shorter), "?" when it is in none
    uint32_t rva = 0;
    bool known = false;
    bool system = false;           // one of the OS images a thread waits or dispatches inside
    bool edvr = false;             // this DLL's own code
};

inline bool equalsNoCase(const char* a, const char* b) noexcept {
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

// The OS images whose frames sit between the game and a wait: a stall's owner is the first frame that is not one.
inline bool isSystemModule(const char* name) noexcept {
    static const char* const kNames[] = {"ntdll.dll", "kernel32.dll", "kernelbase.dll", "win32u.dll", "user32.dll",
                                         "ucrtbase.dll", "sechost.dll"};
    for (const char* n : kNames) {
        if (equalsNoCase(name, n)) return true;
    }
    return false;
}

inline HMODULE thisModule() noexcept {
    static const HMODULE m = [] {
        HMODULE h = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&thisModule), &h);
        return h;
    }();
    return m;
}

inline void resolveFrame(uintptr_t pc, Resolved& r) noexcept {
    r = Resolved{};
    r.module[0] = '?';
    r.module[1] = 0;
    HMODULE module = nullptr;
    if (!pc || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(pc), &module) || !module)
        return;
    wchar_t path[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0) return;
    if (length >= MAX_PATH) length = MAX_PATH - 1;
    DWORD base = 0;
    for (DWORD i = 0; i < length; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') base = i + 1;
    }
    size_t n = 0;
    for (DWORD i = base; i < length && n + 1 < sizeof(r.module); ++i) r.module[n++] = path[i] < 0x80 ? static_cast<char>(path[i]) : '?';
    r.module[n] = 0;
    if (!n) {
        r.module[0] = '?';
        r.module[1] = 0;
    }
    r.rva = static_cast<uint32_t>(pc - reinterpret_cast<uintptr_t>(module));
    r.known = true;
    r.system = isSystemModule(r.module);
    r.edvr = module == thisModule();
}

// One sample, named. Everything the log line needs.
struct Report {
    unsigned sampleIndex = 0;         // 0..2
    bool lateStart = false;           // the episode's first sample was refused by the rate limit; this one began it
    int64_t beatQpc = 0;              // the Present the stall began after: the same for every sample of one episode
    uint32_t ageMs = 0;               // no Present for this long when the thread was stopped
    DWORD thread = 0;
    uint64_t frame = 0;               // the frame the last Present returned in
    CaptureStatus status = CaptureStatus::BadArguments;
    uint32_t suspendedUs = 0;
    unsigned frames = 0;
    Resolved at[kMaxFrames];          // the frames, innermost first
    int owner = 0;                    // index of the first frame that is not an OS wait image (0 when all are)
    unsigned edvrFrames = 0;          // frames in this DLL's own code
    int edvrInnermost = -1;
    DWORD suspendResult = 0, resumeResult = 0;
};

inline void nameCapture(const Capture& cap, Report& r) noexcept {
    uintptr_t pcs[kMaxFrames];
    r.status = cap.status;
    r.suspendResult = cap.suspendResult;
    r.resumeResult = cap.resumeResult;
    r.frames = unwindCapture(cap, pcs, kMaxFrames);
    r.owner = 0;
    r.edvrFrames = 0;
    r.edvrInnermost = -1;
    bool ownerFound = false;
    for (unsigned i = 0; i < r.frames; ++i) {
        resolveFrame(pcs[i], r.at[i]);
        if (!ownerFound && !r.at[i].system) {
            r.owner = static_cast<int>(i);
            ownerFound = true;
        }
        if (r.at[i].edvr) {
            ++r.edvrFrames;
            if (r.edvrInnermost < 0) r.edvrInnermost = static_cast<int>(i);
        }
    }
}

// ---- the log line, as pure text -----------------------------------------------------------------------------

inline void appendText(char* buf, size_t cap, size_t& len, const char* fmt, ...) noexcept {
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    len = static_cast<size_t>(n) < cap - len ? len + static_cast<size_t>(n) : cap - 1;
}

inline void appendFrame(char* buf, size_t cap, size_t& len, const Resolved& f) noexcept {
    if (f.known) appendText(buf, cap, len, "%s+0x%x", f.module, f.rva);
    else appendText(buf, cap, len, "?");
}

// "stall: the render thread stalled 163 ms in ntdll.dll+0x9d5c4; owner nvwgf2umx.dll+0x1a2b3c4; stack ntdll.dll+0x9d5c4
//  < KERNELBASE.dll+0x4f2 < nvwgf2umx.dll+0x1a2b3c4; sample 1 of 3, last Present returned in frame 47210, thread
//  12345, suspended 14 us; EDVR code on the stack: no."
// or, when the stop did not happen or gave no registers:
// "stall: sample 2 of 3 at 512 ms failed: suspend_failed; last Present returned in frame 47210, thread 12345."
inline size_t formatStallLine(char* buf, size_t cap, const Report& r) noexcept {
    size_t len = 0;
    if (!buf || !cap) return 0;
    buf[0] = 0;
    if (r.frames == 0) {
        appendText(buf, cap, len, "stall: sample %u of %u at %u ms failed: %s; last Present returned in frame %llu, thread %lu.",
                   r.sampleIndex + 1, kSamplesPerEpisode, r.ageMs, captureStatusName(r.status),
                   static_cast<unsigned long long>(r.frame), static_cast<unsigned long>(r.thread));
        return len;
    }
    appendText(buf, cap, len, "stall: the render thread stalled %u ms in ", r.ageMs);
    appendFrame(buf, cap, len, r.at[0]);
    appendText(buf, cap, len, "; owner ");
    appendFrame(buf, cap, len, r.at[r.owner]);
    appendText(buf, cap, len, "; stack ");
    for (unsigned i = 0; i < r.frames; ++i) {
        if (i) appendText(buf, cap, len, " < ");
        appendFrame(buf, cap, len, r.at[i]);
    }
    appendText(buf, cap, len, "; sample %u of %u, last Present returned in frame %llu, thread %lu, suspended %u us",
               r.sampleIndex + 1, kSamplesPerEpisode, static_cast<unsigned long long>(r.frame),
               static_cast<unsigned long>(r.thread), r.suspendedUs);
    if (r.status == CaptureStatus::StackOutsideRange)
        appendText(buf, cap, len, " (the stack pointer was outside the thread's stack: one frame only)");
    if (r.edvrFrames) {
        appendText(buf, cap, len, "; EDVR code on the stack: yes (%u frame%s, innermost ", r.edvrFrames, r.edvrFrames == 1 ? "" : "s");
        appendFrame(buf, cap, len, r.at[r.edvrInnermost]);
        appendText(buf, cap, len, ").");
    } else {
        appendText(buf, cap, len, "; EDVR code on the stack: no.");
    }
    return len;
}

// ---- the watchdog --------------------------------------------------------------------------------------------

// What the watchdog counts, for the periodic and end-of-session line.
struct Counts {
    uint32_t episodes = 0;
    uint32_t samples = 0;
    uint32_t skippedRateLimit = 0;
    uint32_t failures = 0;
    uint32_t longestSuspendUs = 0;
    uint32_t longestStallMs = 0;
    uint32_t handleSlotsUsed = 0;
};

using ReportSink = void (*)(const Report&, void* user);

inline int64_t qpcNowTicks() noexcept {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

inline int64_t qpcTicksPerSecond() noexcept {
    static const int64_t f = [] {
        LARGE_INTEGER v{};
        return QueryPerformanceFrequency(&v) ? v.QuadPart : int64_t(0);
    }();
    return f;
}

class Watchdog {
public:
    Watchdog() = default;
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    // Start the watchdog thread. The sink is called on that thread, never while a thread is stopped. False when
    // the thread could not be made (the game is not told; the sampler is simply absent).
    bool start(ReportSink sink, void* user, const PolicyConfig& policy = PolicyConfig{}) {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) return false;
        sink_ = sink;
        user_ = user;
        policy_ = policy;
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent_) return false;
        try {
            // Detached, as the journal worker is: the module is pinned by the time the first Present runs
            // (module_pin.h), and the thread needs nothing joined.
            std::thread(&Watchdog::run, this).detach();
        } catch (...) {
            return false;
        }
        return true;
    }

    // The render thread, once per Present, right after the real Present returns. A relaxed store and a compare.
    void beat(int64_t qpc, uint64_t frame) noexcept {
        const DWORD tid = GetCurrentThreadId();
        if (tid != registeredTid_.load(std::memory_order_relaxed)) registerThisThread(tid);
        beatQpc_.store(qpc, std::memory_order_relaxed);
        beatFrame_.store(frame, std::memory_order_relaxed);
    }

    void stop() noexcept {
        stop_.store(true, std::memory_order_release);
        if (stopEvent_) SetEvent(stopEvent_);
    }

    // True once the watchdog thread has left its loop (a rig waits for it before it deletes the watchdog).
    bool exited() const noexcept { return exited_.load(std::memory_order_acquire); }

    Counts counts() const noexcept {
        Counts c;
        c.episodes = episodes_.load(std::memory_order_relaxed);
        c.samples = samples_.load(std::memory_order_relaxed);
        c.skippedRateLimit = skipped_.load(std::memory_order_relaxed);
        c.failures = failures_.load(std::memory_order_relaxed);
        c.longestSuspendUs = longestSuspendUs_.load(std::memory_order_relaxed);
        c.longestStallMs = longestStallMs_.load(std::memory_order_relaxed);
        c.handleSlotsUsed = slotCount_.load(std::memory_order_acquire);
        return c;
    }

    // Test hooks: the rig changes how often the thread looks and reads what it last sampled.
    DWORD registeredThread() const noexcept { return registeredTid_.load(std::memory_order_relaxed); }
    uint32_t lastSuspendResultForTest() const noexcept { return lastSuspendResult_.load(std::memory_order_relaxed); }

private:
    static constexpr unsigned kSlots = 4;
    struct Slot {
        HANDLE handle = nullptr;
        uint64_t lo = 0, hi = 0;
        DWORD tid = 0;
    };

    // On the thread being watched, from its first beat and again if it ever changes. A handle is never closed:
    // the watchdog may be holding it for a sample, and four of them for the life of the process cost nothing.
    void registerThisThread(DWORD tid) noexcept {
        registeredTid_.store(tid, std::memory_order_relaxed);
        const unsigned used = slotCount_.load(std::memory_order_relaxed);
        if (used >= kSlots) {
            activeSlot_.store(-1, std::memory_order_release);
            return;
        }
        Slot& s = slots_[used];
        s.tid = tid;
        HANDLE h = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
            h = nullptr;
        s.handle = h;
        ULONG_PTR lo = 0, hi = 0;
        GetCurrentThreadStackLimits(&lo, &hi);
        s.lo = lo;
        s.hi = hi;
        slotCount_.store(used + 1, std::memory_order_release);
        activeSlot_.store(h ? static_cast<int>(used) : -1, std::memory_order_release);
    }

    void run() noexcept {
        StallPolicy policy(policy_);
        const double perTickMs = qpcTicksPerSecond() > 0 ? 1000.0 / static_cast<double>(qpcTicksPerSecond()) : 0.0;
        double sleepMs = 100.0;
        // Never allocates after this point: the capture buffer is a member, the report a local.
        while (!stop_.load(std::memory_order_acquire)) {
            if (WaitForSingleObject(stopEvent_, static_cast<DWORD>(sleepMs)) != WAIT_TIMEOUT) break;
            sleepMs = 100.0;
            const int64_t beat = beatQpc_.load(std::memory_order_relaxed);
            const int slot = activeSlot_.load(std::memory_order_acquire);
            if (!beat || perTickMs <= 0.0) continue;
            const int64_t now = qpcNowTicks();
            const double ageMs = static_cast<double>(now - beat) * perTickMs;
            double next = StallPolicy::kPollMs;
            const Decision d = policy.poll(static_cast<uint64_t>(beat), ageMs, static_cast<double>(now) * perTickMs, &next);
            sleepMs = next < 5.0 ? 5.0 : (next > 150.0 ? 150.0 : next);
            if (ageMs > longestAgeSeen_) {
                longestAgeSeen_ = ageMs;
                longestStallMs_.store(static_cast<uint32_t>(ageMs), std::memory_order_relaxed);
            }
            episodes_.store(policy.episodes(), std::memory_order_relaxed);
            skipped_.store(policy.skipped(), std::memory_order_relaxed);
            if (!d.sample) continue;
            sample(d, slot, beat, ageMs, perTickMs);
            samples_.store(policy.samples(), std::memory_order_relaxed);
        }
        exited_.store(true, std::memory_order_release);
    }

    void sample(const Decision& d, int slot, int64_t beat, double ageMs, double perTickMs) noexcept {
        Report& r = report_;
        r = Report{};
        r.sampleIndex = d.index;
        r.lateStart = d.lateStart;
        r.beatQpc = beat;
        r.ageMs = static_cast<uint32_t>(ageMs);
        r.frame = beatFrame_.load(std::memory_order_relaxed);
        if (slot < 0 || static_cast<unsigned>(slot) >= slotCount_.load(std::memory_order_acquire)) {
            r.status = CaptureStatus::BadArguments;
        } else {
            const Slot& s = slots_[slot];
            r.thread = s.tid;
            captureThread(s.handle, s.lo, s.hi, capture_);   // the ONLY stopped window
            r.suspendedUs = static_cast<uint32_t>(static_cast<double>(capture_.stoppedTicks) * perTickMs * 1000.0);
            lastSuspendResult_.store(capture_.suspendResult, std::memory_order_relaxed);
            nameCapture(capture_, r);                         // resumed by now: walk and name
        }
        if (r.frames == 0) failures_.fetch_add(1, std::memory_order_relaxed);
        if (r.suspendedUs > longestSuspendUs_.load(std::memory_order_relaxed))
            longestSuspendUs_.store(r.suspendedUs, std::memory_order_relaxed);
        // The log may allocate; an exception must not leave a noexcept thread (that is std::terminate: the game).
        try {
            if (sink_) sink_(r, user_);
        } catch (...) {
        }
    }

    std::atomic<bool> started_{false}, stop_{false}, exited_{false};
    HANDLE stopEvent_ = nullptr;
    ReportSink sink_ = nullptr;
    void* user_ = nullptr;
    PolicyConfig policy_;

    // Written by the render thread, read by the watchdog.
    std::atomic<int64_t> beatQpc_{0};
    std::atomic<uint64_t> beatFrame_{0};
    std::atomic<int> activeSlot_{-1};
    std::atomic<unsigned> slotCount_{0};
    std::atomic<DWORD> registeredTid_{0};   // the render thread's own record of which thread it last registered
    Slot slots_[kSlots];

    // The watchdog's own, preallocated.
    Capture capture_;
    Report report_;
    double longestAgeSeen_ = 0.0;
    std::atomic<uint32_t> episodes_{0}, samples_{0}, skipped_{0}, failures_{0}, longestSuspendUs_{0}, longestStallMs_{0};
    std::atomic<uint32_t> lastSuspendResult_{0};
};

}  // namespace edvr::stall
