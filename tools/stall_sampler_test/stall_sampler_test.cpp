// The rig for the stall sampler (src\common\stall_sampler.h, docs/freeze-diagnostics-2026-10-01.md, commit 2).
//
// It runs the REAL capture, unwind and naming code against threads it blocks in places it knows -- Sleep, a wait on
// an event, a busy loop in this executable, a busy loop in a second DLL, a stack 18 KB deep -- and holds that the
// sampler names them and that the thread it stopped always runs again. Every check's label starts with its case
// id (S1..S9), which is how tools\stall_sampler_test\mutants.py tells a mutation that tripped the right check from
// one that tripped something else:
//
//   S1  the policy: when a sample is due (150, 500, 1000 ms), one episode per Present, the rate limit and its
//       refill, a late first sample, the session cap, a late look taking one sample not three
//   S2  the log line as pure text: the stack, the owner (the first frame that is not an OS wait image), EDVR's own
//       code named, a failed sample, a stack pointer outside the stack
//   S3  capture and naming on a live thread: a Sleep, a wait, a spin in this exe, a spin in a DLL, an 18 KB stack
//   S4  THE RESUME ALWAYS HAPPENS: after every capture the thread's suspend count is back to 0, on the failure
//       paths too (no context, a stack pointer outside the stack, a thread that already exited, a bad handle),
//       and three hundred captures in a row leave the thread running
//   S5  the copy is what is walked: the stack the thread was stopped on is gone (the thread has run on and
//       overwritten it) and the capture still names the frames it had
//   S6  the watchdog, end to end, on a real clock: a thread that presents does nothing; a thread that stops for
//       1.3 s gets its three samples, at the right ages, naming the function it stopped in; a second thread
//       registered later is the one sampled
//   S7  the shape of the code, by source text: no injection API appears in the sampler's sources, and the stopped
//       window of captureThread is straight-line code with the three calls it is allowed
//   S8  the order in the watchdog: the stop and the copy, then the resume, then the walk and the log
//   S9  the cost: a presenting thread costs the beat and nothing else (no sample, no stop)
//
//   stall_sampler_test.exe --dry-run                 touches nothing
//   stall_sampler_test.exe --self-test [root]        root is the repository, for the S7 and S8 source pins
//
// A QUIET RIG: it holds wall-clock intervals (150 ms to a second) against a real thread, so build.bat runs it
// alone, after the others (run_jobs.py --quiet).
#include <windows.h>

#include <malloc.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "stall_sampler.h"

using namespace edvr::stall;

namespace {
unsigned g_checks = 0, g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
}

bool waitFor(const std::function<bool()>& done, unsigned ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        if (done()) return true;
        Sleep(2);
    }
    return done();
}

std::string exeName() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::string out;
    size_t base = 0;
    for (DWORD i = 0; i < n; ++i) {
        if (path[i] == L'\\') base = i + 1;
    }
    for (size_t i = base; i < n; ++i) out.push_back(path[i] < 0x80 ? static_cast<char>(path[i]) : '?');
    return out;
}

// ---- the functions a thread is blocked in -----------------------------------------------------------------------
// Never inlined and with their frames kept, so a sample's frames land inside them and their unwind data is the
// ordinary kind.
struct Target {
    HANDLE thread = nullptr;
    DWORD tid = 0;
    uint64_t lo = 0, hi = 0;
    std::atomic<int> mode{0};
    std::atomic<bool> release{false};
    std::atomic<bool> blocked{false};
    std::atomic<bool> churned{false};
    std::atomic<long long> progress{0};
    HANDLE wake = nullptr;
    HANDLE ready = nullptr;
};

enum Mode { kSleep = 1, kWait = 2, kSpinExe = 3, kSpinDll = 4, kDeep = 5, kSleepThenChurn = 6, kExitAtOnce = 7, kDeepThenChurn = 8 };

using SpinFn = void (*)(volatile LONG*, volatile LONG64*);
using WaitFn = void (*)(HANDLE);
SpinFn g_dllSpin = nullptr;
volatile LONG g_dllStop = 0;
volatile LONG64 g_dllProgress = 0;

#pragma optimize("", off)
__declspec(noinline) void blockInSleepFn(Target* t) {
    t->blocked = true;
    while (!t->release) Sleep(40);
}
__declspec(noinline) void blockInWaitFn(Target* t) {
    t->blocked = true;
    WaitForSingleObject(t->wake, INFINITE);
}
__declspec(noinline) void spinInExeFn(Target* t) {
    t->blocked = true;
    while (!t->release) ++t->progress;
}
__declspec(noinline) void spinInDllFn(Target* t) {
    t->blocked = true;
    g_dllSpin(&g_dllStop, &g_dllProgress);
}
__declspec(noinline) int deepFrame(Target* t, int depth) {
    volatile char pad[3000];
    // A dynamic allocation makes the compiler address the frame through RBP (an unwind code that sets the frame
    // register), so the walk of the copy has to rebase RBP as well as RSP.
    volatile char* dyn = static_cast<volatile char*>(_alloca(48 + 16 * static_cast<size_t>(depth)));
    dyn[0] = static_cast<char>(depth);
    for (int i = 0; i < 3000; i += 512) pad[i] = static_cast<char>(depth);
    if (depth > 0) return deepFrame(t, depth - 1) + pad[0];
    t->blocked = true;
    while (!t->release) Sleep(40);
    return pad[0];
}
// Overwrites the stack below its caller: whatever frames a sleeping call had there are gone afterwards.
__declspec(noinline) void churnStack(int depth) {
    volatile unsigned char pad[2048];
    for (size_t i = 0; i < sizeof(pad); ++i) pad[i] = 0xCC;
    if (depth > 0) churnStack(depth - 1);
    pad[0] = pad[sizeof(pad) - 1];
}
__declspec(noinline) DWORD WINAPI targetMain(LPVOID p) {
    Target* t = static_cast<Target*>(p);
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    t->lo = lo;
    t->hi = hi;
    t->tid = GetCurrentThreadId();
    SetEvent(t->ready);
    switch (t->mode.load()) {
        case kSleep: blockInSleepFn(t); break;
        case kWait: blockInWaitFn(t); break;
        case kSpinExe: spinInExeFn(t); break;
        case kSpinDll: spinInDllFn(t); break;
        case kDeep: deepFrame(t, 6); break;
        case kSleepThenChurn:
            blockInSleepFn(t);
            for (int i = 0; i < 20; ++i) churnStack(12);
            t->churned = true;
            while (t->churned) Sleep(10);   // keep the thread alive until the rig is done with it
            break;
        case kExitAtOnce: break;
        case kDeepThenChurn:
            deepFrame(t, 6);
            for (int i = 0; i < 20; ++i) churnStack(12);
            t->churned = true;
            while (t->churned) Sleep(10);
            break;
    }
    return 0;
}
#pragma optimize("", on)

struct Range {
    uint32_t begin = 0, end = 0;
    bool ok = false;
};

Range rangeOf(void* fn) {
    Range r;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(fn), &base, nullptr);
    if (!f) return r;
    r.begin = f->BeginAddress;
    r.end = f->EndAddress;
    r.ok = true;
    return r;
}

bool frameIn(const Resolved& f, const std::string& module, const Range& r) {
    return r.ok && f.known && equalsNoCase(f.module, module.c_str()) && f.rva >= r.begin && f.rva < r.end;
}

int findFrame(const Report& rep, const std::string& module, const Range& r) {
    for (unsigned i = 0; i < rep.frames; ++i) {
        if (frameIn(rep.at[i], module, r)) return static_cast<int>(i);
    }
    return -1;
}

unsigned countFrames(const Report& rep, const std::string& module, const Range& r) {
    unsigned n = 0;
    for (unsigned i = 0; i < rep.frames; ++i) n += frameIn(rep.at[i], module, r) ? 1u : 0u;
    return n;
}

void startTarget(Target& t, int mode) {
    t.mode = mode;
    t.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    t.wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    t.thread = CreateThread(nullptr, 0, &targetMain, &t, 0, nullptr);
    WaitForSingleObject(t.ready, 5000);
}

void stopTarget(Target& t) {
    t.release = true;
    t.churned = false;
    if (t.wake) SetEvent(t.wake);
    InterlockedExchange(&g_dllStop, 1);
    if (t.thread) {
        WaitForSingleObject(t.thread, 3000);
        CloseHandle(t.thread);
    }
    if (t.ready) CloseHandle(t.ready);
    if (t.wake) CloseHandle(t.wake);
    InterlockedExchange(&g_dllStop, 0);
}

Capture& theCapture() {
    static Capture* c = new Capture;
    return *c;
}

// The suspend count of `h` right now: stop it and read what SuspendThread says it was, then let it go.
// 0 means nobody has it stopped.
DWORD suspendCountOf(HANDLE h) {
    const DWORD was = SuspendThread(h);
    if (was != static_cast<DWORD>(-1)) ResumeThread(h);
    return was;
}

// ---- S1 ---------------------------------------------------------------------------------------------------------
void policyCases() {
    double next = 0.0;
    {
        StallPolicy p;
        Decision d = p.poll(1, 10.0, 1000.0, &next);
        check(!d.sample && next > 139.0 && next < 141.0,
              "S1.thresholds: a thread that presented 10 ms ago is not stalled and is looked at again when 150 ms could be up");
        d = p.poll(1, 149.0, 1139.0, &next);
        check(!d.sample, "S1.thresholds: 149 ms without a Present is not a stall");
        d = p.poll(1, 150.0, 1140.0, &next);
        check(d.sample && d.index == 0 && !d.lateStart, "S1.thresholds: 150 ms is the first sample");
        d = p.poll(1, 151.0, 1141.0, &next);
        check(!d.sample && next > 348.0 && next < 350.0, "S1.thresholds: the next look is when 500 ms could be up");
        d = p.poll(1, 499.0, 1489.0, &next);
        check(!d.sample, "S1.thresholds: 499 ms is not the second sample");
        d = p.poll(1, 500.0, 1490.0, &next);
        check(d.sample && d.index == 1, "S1.thresholds: 500 ms is the second sample");
        d = p.poll(1, 1000.0, 1990.0, &next);
        check(d.sample && d.index == 2, "S1.thresholds: 1000 ms is the third sample");
        d = p.poll(1, 1200.0, 2190.0, &next);
        check(!d.sample && next == StallPolicy::kPollMs, "S1.thresholds: after three samples the episode is done and the next Present is awaited");
        d = p.poll(1, 5000.0, 6000.0, &next);
        check(!d.sample, "S1.thresholds: never a fourth sample of one episode");
        d = p.poll(2, 160.0, 6100.0, &next);
        check(d.sample && d.index == 0 && p.episodes() == 2 && p.samples() == 4,
              "S1.episode: a new Present is a new episode, sampled from its first threshold again");
    }
    {
        // A look that comes late: the watchdog was not scheduled for 1.1 s. One sample, at the last threshold passed.
        StallPolicy p;
        Decision d = p.poll(7, 1100.0, 5000.0, &next);
        check(d.sample && d.index == 2, "S1.late: a first look at 1100 ms takes the third sample, once");
        d = p.poll(7, 1105.0, 5005.0, &next);
        check(!d.sample, "S1.late: and not the two it skipped");
    }
    {
        // The rate limit: six episodes may start together, then one more every two seconds.
        StallPolicy p;
        unsigned started = 0;
        for (uint64_t beat = 1; beat <= 8; ++beat) {
            const Decision d = p.poll(beat, 150.0, 10000.0, &next);
            if (d.sample) ++started;
        }
        check(started == 6 && p.episodes() == 6 && p.skipped() == 2,
              "S1.rate: of eight stalls at once six are sampled and two are skipped");
        // A refused episode that keeps going and is refused again at 500 and 1000 ms is still ONE skipped episode.
        p.poll(8, 500.0, 10000.0, &next);
        p.poll(8, 1000.0, 10000.0, &next);
        check(p.skipped() == 2 && p.episodes() == 6, "S1.rate: an episode refused at 150, 500 and 1000 ms is counted skipped once, not three times");
        Decision d = p.poll(9, 150.0, 10000.0 + 2100.0, &next);
        check(d.sample && p.skipped() == 2, "S1.rate: two seconds on, one more may start");
        d = p.poll(10, 150.0, 10000.0 + 2100.0 + 50.0, &next);
        check(!d.sample && p.skipped() == 3, "S1.rate: and only one");
        // The refused one is still going when 500 ms is up and a token has come back: it starts late.
        d = p.poll(10, 500.0, 10000.0 + 2100.0 + 2100.0, &next);
        check(d.sample && d.index == 1 && d.lateStart && p.skipped() == 3,
              "S1.rate: a stall refused at 150 ms and still going at 500 with a token to spend starts then, flagged late, counted skipped once");
    }
    {
        PolicyConfig cfg;
        cfg.sessionEpisodes = 3;
        cfg.burst = 100.0;
        StallPolicy p(cfg);
        unsigned started = 0;
        for (uint64_t beat = 1; beat <= 6; ++beat) {
            if (p.poll(beat, 150.0, 1000.0, &next).sample) ++started;
        }
        check(started == 3 && p.episodes() == 3 && p.skipped() == 3, "S1.cap: no more than the session's episodes, however many tokens there are");
    }
    {
        // The default: two hundred a session, however long the session and however well spaced the stalls.
        StallPolicy p;
        unsigned started = 0;
        double now = 1000.0;
        for (uint64_t beat = 1; beat <= 260; ++beat) {
            now += 2100.0;   // a token is always back by the next stall
            if (p.poll(beat, 150.0, now, &next).sample) ++started;
        }
        check(PolicyConfig{}.sessionEpisodes == 200 && started == 200 && p.episodes() == 200 && p.skipped() == 60,
              "S1.cap: with the default policy 260 well-spaced stalls get 200 episodes, and 60 are skipped");
    }
    {
        // Later samples of a started episode are free: they do not need a token.
        PolicyConfig cfg;
        cfg.burst = 1.0;
        StallPolicy p(cfg);
        check(p.poll(1, 150.0, 100.0, &next).sample, "S1.free: the first sample takes the only token");
        check(p.poll(1, 500.0, 600.0, &next).sample && p.poll(1, 1000.0, 1100.0, &next).sample,
              "S1.free: the second and third samples of that episode need no token");
    }
}

// ---- S2 ---------------------------------------------------------------------------------------------------------
Resolved mk(const char* module, uint32_t rva, bool system, bool edvr = false) {
    Resolved r;
    std::snprintf(r.module, sizeof(r.module), "%s", module);
    r.rva = rva;
    r.known = true;
    r.system = system;
    r.edvr = edvr;
    return r;
}

void lineCases() {
    Report r;
    r.sampleIndex = 0;
    r.ageMs = 163;
    r.thread = 12345;
    r.frame = 47210;
    r.status = CaptureStatus::Ok;
    r.suspendedUs = 14;
    r.at[0] = mk("ntdll.dll", 0x9d5c4, true);
    r.at[1] = mk("KERNELBASE.dll", 0x4f2, true);
    r.at[2] = mk("nvwgf2umx.dll", 0x1a2b3c4, false);
    r.at[3] = mk("EliteDangerous64.exe", 0x99c0f4, false);
    r.frames = 4;
    r.owner = 2;
    char line[1150];
    size_t n = formatStallLine(line, sizeof(line), r);
    check(std::string(line) ==
              "stall: the render thread stalled 163 ms in ntdll.dll+0x9d5c4; owner nvwgf2umx.dll+0x1a2b3c4; stack "
              "ntdll.dll+0x9d5c4 < KERNELBASE.dll+0x4f2 < nvwgf2umx.dll+0x1a2b3c4 < EliteDangerous64.exe+0x99c0f4; "
              "sample 1 of 3, last Present returned in frame 47210, thread 12345, suspended 14 us; EDVR code on the stack: no." &&
              n == std::strlen(line),
          "S2.line: the stall line: age, top frame, owner, stack innermost first, sample k of 3, frame, thread, how long the thread was stopped, EDVR");
    r.at[3] = mk("d3d11.dll", 0x1c4a3, false, true);
    r.edvrFrames = 1;
    r.edvrInnermost = 3;
    r.sampleIndex = 2;
    n = formatStallLine(line, sizeof(line), r);
    check(std::string(line).find("sample 3 of 3,") != std::string::npos &&
              std::string(line).find("; EDVR code on the stack: yes (1 frame, innermost d3d11.dll+0x1c4a3).") != std::string::npos,
          "S2.edvr: EDVR's own code on the stack is said, with its innermost frame and the plural right");
    r.edvrFrames = 2;
    formatStallLine(line, sizeof(line), r);
    check(std::string(line).find("yes (2 frames, innermost") != std::string::npos, "S2.edvr: two frames read 'frames'");
    r.at[0] = mk("d3d11.dll", 0x1c4a3, false, true);
    r.owner = 0;
    r.edvrInnermost = 0;
    formatStallLine(line, sizeof(line), r);
    check(std::string(line).find("in d3d11.dll+0x1c4a3; owner d3d11.dll+0x1c4a3;") != std::string::npos,
          "S2.owner: with no system frame on top the owner is the top frame itself");
    r.status = CaptureStatus::StackOutsideRange;
    r.frames = 1;
    r.edvrFrames = 0;
    r.edvrInnermost = -1;
    formatStallLine(line, sizeof(line), r);
    check(std::string(line).find("suspended 14 us (the stack pointer was outside the thread's stack: one frame only); EDVR code on the stack: no.") !=
              std::string::npos,
          "S2.range: a stack pointer outside the stack is said, not left to read as a short stack");
    Report f;
    f.sampleIndex = 1;
    f.ageMs = 512;
    f.thread = 12345;
    f.frame = 47210;
    f.status = CaptureStatus::SuspendFailed;
    f.frames = 0;
    formatStallLine(line, sizeof(line), f);
    check(std::string(line) == "stall: sample 2 of 3 at 512 ms failed: suspend_failed; last Present returned in frame 47210, thread 12345.",
          "S2.failed: a sample that could not stop the thread says so and why");
    f.status = CaptureStatus::ContextFailed;
    formatStallLine(line, sizeof(line), f);
    check(std::string(line).find("failed: context_failed;") != std::string::npos, "S2.failed: no registers is its own reason");
    Report unknown = r;
    unknown.status = CaptureStatus::Ok;
    unknown.frames = 2;
    unknown.at[0] = Resolved{};
    std::snprintf(unknown.at[0].module, sizeof(unknown.at[0].module), "?");
    unknown.at[1] = mk("a.dll", 1, false);
    unknown.owner = 1;
    unknown.edvrFrames = 0;
    formatStallLine(line, sizeof(line), unknown);
    check(std::string(line).find("stalled 163 ms in ?;") != std::string::npos && std::string(line).find("stack ? < a.dll+0x1;") != std::string::npos,
          "S2.unknown: an address in no image is a question mark, not a made-up module");
    Report wide;
    wide.ageMs = 4294967295u;
    wide.thread = 4294967295u;
    wide.frame = 18446744073709551615ull;
    wide.suspendedUs = 4294967295u;
    wide.status = CaptureStatus::Ok;
    wide.frames = kMaxFrames;
    for (unsigned i = 0; i < kMaxFrames; ++i) wide.at[i] = mk("a_very_long_module_name_for_the_widest_stack.dll", 0xFFFFFFFFu, i < 2);
    wide.owner = 3;
    wide.edvrFrames = kMaxFrames;
    wide.edvrInnermost = 0;
    n = formatStallLine(line, sizeof(line), wide);
    check(n > 0 && n < 1150 && std::string(line).size() == n && line[n - 1] == '.',
          "S2.fit: the widest line (fourteen frames of the longest names, every number at its largest) fits the 1150 bytes the log line gets, tail intact");
}

// ---- S3, S4, S5 --------------------------------------------------------------------------------------------------
double ticksToMs(int64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(qpcTicksPerSecond());
}

// The resume check every capture gets: nobody has the thread stopped any more, and what ResumeThread saw was our own stop.
bool resumed(const Capture& cap, HANDLE h) {
    return cap.suspendResult == 0 && cap.resumeResult == 1 && suspendCountOf(h) == 0;
}

void captureCases(const std::string& exe, HMODULE dll) {
    Capture& cap = theCapture();
    const Range inSleep = rangeOf(reinterpret_cast<void*>(&blockInSleepFn));
    const Range inWait = rangeOf(reinterpret_cast<void*>(&blockInWaitFn));
    const Range inSpin = rangeOf(reinterpret_cast<void*>(&spinInExeFn));
    const Range inSpinDll = rangeOf(reinterpret_cast<void*>(&spinInDllFn));
    const Range inDeep = rangeOf(reinterpret_cast<void*>(&deepFrame));
    const Range inMain = rangeOf(reinterpret_cast<void*>(&targetMain));
    check(inSleep.ok && inWait.ok && inSpin.ok && inSpinDll.ok && inDeep.ok && inMain.ok,
          "S3.setup: the functions the rig blocks threads in have unwind data of their own");

    // A thread asleep in Sleep, called from blockInSleepFn.
    {
        Target t;
        startTarget(t, kSleep);
        waitFor([&] { return t.blocked.load(); }, 2000);
        Sleep(120);
        Report rep;
        const bool ok = captureThread(t.thread, t.lo, t.hi, cap);
        nameCapture(cap, rep);
        check(ok && cap.status == CaptureStatus::Ok && rep.frames >= 4, "S3.sleep: a thread in Sleep is stopped, read, and its stack walked");
        check(rep.at[0].known && rep.at[0].system, "S3.sleep: its top frame is an OS image (ntdll's wait)");
        const int at = findFrame(rep, exe, inSleep);
        check(at >= 0 && rep.owner == at, "S3.sleep: the owner is the function the thread sleeps in, in this executable, not ntdll");
        check(findFrame(rep, exe, inMain) > at && at >= 0, "S3.sleep: and its caller, the thread's own start routine, is further out on the stack");
        check(rep.at[rep.owner].edvr, "S3.sleep: the owner is in the sampler's own module, said as EDVR (the rig stands for the DLL)");
        check(resumed(cap, t.thread), "S4.sleep: the thread is running again, nobody has it stopped");
        check(ticksToMs(cap.stoppedTicks) < 50.0, "S4.sleep: it was stopped for less than 50 ms");
        stopTarget(t);
    }
    // A thread waiting on an event.
    {
        Target t;
        startTarget(t, kWait);
        waitFor([&] { return t.blocked.load(); }, 2000);
        Sleep(100);
        Report rep;
        captureThread(t.thread, t.lo, t.hi, cap);
        nameCapture(cap, rep);
        const int at = findFrame(rep, exe, inWait);
        check(rep.at[0].system && at >= 0 && rep.owner == at, "S3.wait: a wait on an event: an OS frame on top, the waiting function as owner");
        check(resumed(cap, t.thread), "S4.wait: the waiting thread is running again");
        stopTarget(t);
    }
    // A busy loop in this executable.
    {
        Target t;
        startTarget(t, kSpinExe);
        waitFor([&] { return t.blocked.load(); }, 2000);
        Sleep(50);
        Report rep;
        captureThread(t.thread, t.lo, t.hi, cap);
        nameCapture(cap, rep);
        const int spinAt = findFrame(rep, exe, inSpin);
        check(rep.frames >= 2 && spinAt >= 0 && spinAt <= 2 && rep.owner == 0 && !rep.at[0].system,
              "S3.spin: a busy loop: the thread's RIP is in this executable (the loop, or the atomic helper it calls) with the loop's function within two frames, and that is the owner");
        check(resumed(cap, t.thread), "S4.spin: the spinning thread is running again");
        const long long before = t.progress.load();
        Sleep(60);
        check(t.progress.load() > before, "S4.spin: and it is making progress");
        stopTarget(t);
    }
    // A busy loop in a DLL.
    {
        g_dllSpin = reinterpret_cast<SpinFn>(GetProcAddress(dll, "stall_target_spin"));
        const Range inDll = rangeOf(reinterpret_cast<void*>(g_dllSpin));
        Target t;
        startTarget(t, kSpinDll);
        waitFor([&] { return t.blocked.load(); }, 2000);
        Sleep(50);
        Report rep;
        captureThread(t.thread, t.lo, t.hi, cap);
        nameCapture(cap, rep);
        check(inDll.ok && frameIn(rep.at[0], "stall_target.dll", inDll) && rep.owner == 0 && !rep.at[0].edvr && !rep.at[0].system,
              "S3.dll: a busy loop in another DLL is named by that DLL's own name, with its RVA, and is not EDVR's");
        check(rep.frames >= 2 && frameIn(rep.at[1], exe, inSpinDll), "S3.dll: its caller in this executable is the next frame");
        check(resumed(cap, t.thread), "S4.dll: the thread in the DLL is running again");
        stopTarget(t);
    }
    // A stack 18 KB deep: the copy reaches the thread's start routine from inside six nested frames.
    {
        Target t;
        startTarget(t, kDeep);
        waitFor([&] { return t.blocked.load(); }, 2000);
        Sleep(100);
        Report rep;
        captureThread(t.thread, t.lo, t.hi, cap);
        nameCapture(cap, rep);
        check(cap.stackBytes >= 18000 && countFrames(rep, exe, inDeep) == 7,
              "S3.deep: seven nested frames of 3 KB each (21 KB) are all named");
        check(findFrame(rep, exe, inMain) >= 0, "S3.deep: and the thread's start routine beyond them");
        check(resumed(cap, t.thread), "S4.deep: the thread is running again");
        stopTarget(t);
    }
    // S5: the stack the thread was stopped on is gone by the time it is walked, and the capture still names it.
    {
        Target t;
        startTarget(t, kSleepThenChurn);
        waitFor([&] { return t.blocked.load(); }, 2000);
        Sleep(100);
        Report before;
        captureThread(t.thread, t.lo, t.hi, cap);
        nameCapture(cap, before);
        const int atBefore = findFrame(before, exe, inSleep);
        t.release = true;                                  // the thread leaves Sleep and overwrites the stack below its start routine
        const bool churned = waitFor([&] { return t.churned.load(); }, 3000);
        Report after;
        nameCapture(cap, after);                           // walked now, from the copy
        check(churned && atBefore >= 0 && after.frames == before.frames && findFrame(after, exe, inSleep) == atBefore,
              "S5.copy: the thread has run on and overwritten the stack, and the capture still names the frames it had (the walk reads the copy)");
        // The live stack really was changed: a walk of the live stack now would not find the sleeping function.
        CONTEXT live{};
        live.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        SuspendThread(t.thread);
        GetThreadContext(t.thread, &live);
        ResumeThread(t.thread);
        check(live.Rip != cap.context.Rip || live.Rsp != cap.context.Rsp, "S5.setup: the thread is no longer where it was stopped");
        stopTarget(t);
    }
    // The same with the deep stack: seven frames that address themselves through RBP (a dynamic allocation makes the
    // compiler use it), overwritten after the capture. Naming them from the copy needs RBP rebased into the copy
    // at every step; a walk that left it pointing at the live stack reads what the churn left there.
    {
        Target t;
        startTarget(t, kDeepThenChurn);
        waitFor([&] { return t.blocked.load(); }, 2000);
        Sleep(100);
        Report before;
        captureThread(t.thread, t.lo, t.hi, cap);
        nameCapture(cap, before);
        t.release = true;
        const bool churned = waitFor([&] { return t.churned.load(); }, 3000);
        Report after;
        nameCapture(cap, after);
        check(churned && countFrames(before, exe, inDeep) == 7 && countFrames(after, exe, inDeep) == 7 && after.frames == before.frames,
              "S5.deep: seven RBP-framed frames, overwritten after the capture, are all still named from the copy");
        stopTarget(t);
    }
}

void resumeCases(const std::string& exe) {
    (void)exe;
    Capture& cap = theCapture();
    // A bad handle: nothing is stopped, and the status says so.
    {
        const bool ok = captureThread(nullptr, 0x1000, 0x2000, cap);
        check(!ok && cap.status == CaptureStatus::BadArguments && cap.suspendResult == 0 && cap.resumeResult == 0,
              "S4.bad: no handle: refused before anything is stopped");
        Target t;
        startTarget(t, kSleep);
        waitFor([&] { return t.blocked.load(); }, 2000);
        const bool ok2 = captureThread(t.thread, 0, 0, cap);
        check(!ok2 && cap.status == CaptureStatus::BadArguments && suspendCountOf(t.thread) == 0,
              "S4.bad: no stack limits: refused before anything is stopped");
        stopTarget(t);
    }
    // No GET_CONTEXT access: the thread is stopped, no registers come back, and it is resumed all the same.
    {
        Target t;
        startTarget(t, kSpinExe);
        waitFor([&] { return t.blocked.load(); }, 2000);
        HANDLE weak = OpenThread(THREAD_SUSPEND_RESUME, FALSE, t.tid);
        const bool ok = captureThread(weak, t.lo, t.hi, cap);
        check(!ok && cap.status == CaptureStatus::ContextFailed, "S4.context: a handle that cannot read registers says context_failed");
        check(cap.suspendResult == 0 && cap.resumeResult == 1 && suspendCountOf(t.thread) == 0,
              "S4.context: and the thread it stopped was resumed anyway");
        const long long before = t.progress.load();
        Sleep(60);
        check(t.progress.load() > before, "S4.context: and it is making progress");
        CloseHandle(weak);
        stopTarget(t);
    }
    // A stack pointer outside the registered stack (a fiber's stack, or the guard region): registers only.
    {
        Target t;
        startTarget(t, kSpinExe);
        waitFor([&] { return t.blocked.load(); }, 2000);
        const bool ok = captureThread(t.thread, t.hi - 64, t.hi, cap);   // a 64-byte "stack" that RSP is not in
        check(ok && cap.status == CaptureStatus::StackOutsideRange && cap.stackBytes == 0 && cap.context.Rip != 0,
              "S4.range: a stack pointer outside the registered stack keeps the registers and copies nothing");
        check(cap.suspendResult == 0 && cap.resumeResult == 1 && suspendCountOf(t.thread) == 0,
              "S4.range: and the thread was resumed");
        Report rep;
        nameCapture(cap, rep);
        check(rep.frames == 1 && rep.status == CaptureStatus::StackOutsideRange, "S4.range: the sample names the one frame it has");
        const long long before = t.progress.load();
        Sleep(60);
        check(t.progress.load() > before, "S4.range: and the thread is making progress");
        stopTarget(t);
    }
    // A thread that has already exited.
    {
        Target t;
        startTarget(t, kExitAtOnce);
        WaitForSingleObject(t.thread, 3000);
        const bool ok = captureThread(t.thread, t.lo, t.hi, cap);
        check(!ok && (cap.status == CaptureStatus::SuspendFailed || cap.status == CaptureStatus::ContextFailed),
              "S4.exited: a thread that has exited is a failed sample, not a hang");
        stopTarget(t);
    }
    // Three hundred in a row: the thread is running at the end, and every stop was balanced by its resume.
    {
        Target t;
        startTarget(t, kSpinExe);
        waitFor([&] { return t.blocked.load(); }, 2000);
        unsigned balanced = 0, named = 0;
        for (int i = 0; i < 300; ++i) {
            Report rep;
            captureThread(t.thread, t.lo, t.hi, cap);
            nameCapture(cap, rep);
            if (cap.suspendResult == 0 && cap.resumeResult == 1) ++balanced;
            if (rep.frames >= 1 && rep.at[0].known) ++named;
        }
        check(balanced == 300 && named == 300, "S4.many: three hundred captures, every stop balanced by its resume, every one named");
        const long long before = t.progress.load();
        Sleep(60);
        check(t.progress.load() > before && suspendCountOf(t.thread) == 0, "S4.many: and the thread is running at the end");
        stopTarget(t);
    }
}

// ---- S6, S9 -----------------------------------------------------------------------------------------------------
struct Collected {
    std::mutex m;
    std::vector<Report> reports;
};
void collect(const Report& r, void* user) {
    auto* c = static_cast<Collected*>(user);
    std::lock_guard<std::mutex> lock(c->m);
    c->reports.push_back(r);
}

// A render-like thread: presents every 5 ms for `beatMs`, then blocks in Sleep for `blockMs`, then presents for `tailMs`.
struct Renderer {
    Watchdog* dog = nullptr;
    unsigned beatMs = 0, blockMs = 0, tailMs = 0;
    std::atomic<DWORD> tid{0};
    std::atomic<bool> finished{false};   // its scripted work is done; it keeps presenting until released
    std::atomic<bool> release{false};
    std::atomic<uint64_t> frame{1};
};

#pragma optimize("", off)
__declspec(noinline) void renderStall(Renderer* r) {
    Sleep(r->blockMs);
}
__declspec(noinline) void presentFor(Renderer* r, unsigned ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        r->dog->beat(qpcNowTicks(), r->frame++);
        Sleep(5);
    }
}
__declspec(noinline) DWORD WINAPI rendererMain(LPVOID p) {
    Renderer* r = static_cast<Renderer*>(p);
    r->tid = GetCurrentThreadId();
    presentFor(r, r->beatMs);
    if (r->blockMs) renderStall(r);
    presentFor(r, r->tailMs);
    r->finished = true;
    // Still presenting: a thread that ended would be a stall of a dead thread, which the watchdog would sample too.
    while (!r->release) {
        r->dog->beat(qpcNowTicks(), r->frame++);
        Sleep(5);
    }
    return 0;
}
#pragma optimize("", on)

// Run a renderer to the end of its script, give the watchdog a moment more, and hand back its reports.
void runRenderer(Renderer& r, Collected& got, std::vector<Report>& reports, unsigned timeoutMs) {
    HANDLE h = CreateThread(nullptr, 0, &rendererMain, &r, 0, nullptr);
    waitFor([&] { return r.finished.load(); }, timeoutMs);
    Sleep(200);
    {
        std::lock_guard<std::mutex> lock(got.m);
        reports = got.reports;
    }
    r.release = true;
    WaitForSingleObject(h, 3000);
    CloseHandle(h);
}

void watchdogCases(const std::string& exe) {
    const Range inStall = rangeOf(reinterpret_cast<void*>(&renderStall));
    // S9 first: a thread that presents costs nothing but its beat.
    {
        Watchdog* dog = new Watchdog;
        Collected got;
        check(dog->start(&collect, &got), "S6.start: the watchdog thread starts");
        Renderer r;
        r.dog = dog;
        r.beatMs = 1300;
        std::vector<Report> reports;
        runRenderer(r, got, reports, 8000);
        const Counts c = dog->counts();
        check(reports.empty() && c.samples == 0 && c.episodes == 0 && c.handleSlotsUsed == 1,
              "S9.quiet: 1.5 s of a thread that presents every 5 ms: no sample, no stop, no episode; one thread registered");
        check(dog->registeredThread() == r.tid.load(), "S9.quiet: the thread that beats is the one registered");
        // What a beat costs the render thread, said out loud so a slow clock or a regression shows in the build log. Not an
        // assertion: a loaded machine would make any threshold a flake. (The beat is a thread-id compare and two relaxed
        // stores; the registration happened once, above.)
        {
            struct Timed {
                static DWORD WINAPI run(LPVOID p) {
                    Watchdog* d = static_cast<Watchdog*>(p);
                    const int kBeats = 2000000;
                    const int64_t t0 = qpcNowTicks();
                    for (int i = 0; i < kBeats; ++i) d->beat(t0 + i, static_cast<uint64_t>(i));
                    const int64_t t1 = qpcNowTicks();
                    std::printf("stall_sampler_test: one beat costs %.1f ns here (the render thread pays it once a frame)\n",
                                static_cast<double>(t1 - t0) * 1e9 / static_cast<double>(qpcTicksPerSecond()) / kBeats);
                    return 0;
                }
            };
            HANDLE h = CreateThread(nullptr, 0, &Timed::run, dog, 0, nullptr);
            WaitForSingleObject(h, 10000);
            CloseHandle(h);
        }
        dog->stop();
    }
    // S6: a stall of 1.3 s.
    {
        Watchdog* dog = new Watchdog;
        Collected got;
        dog->start(&collect, &got);
        Renderer r;
        r.dog = dog;
        r.beatMs = 400;
        r.blockMs = 1300;
        r.tailMs = 300;
        std::vector<Report> reports;
        runRenderer(r, got, reports, 10000);
        check(reports.size() == 3, "S6.samples: a stall of 1.3 s gets its three samples (150, 500 and 1000 ms), and no more");
        if (reports.size() != 3) {
            for (const Report& rep : reports)
                std::printf("      sample %u at %u ms: status %s, %u frames, thread %lu, suspended %u us\n", rep.sampleIndex + 1, rep.ageMs,
                            captureStatusName(rep.status), rep.frames, static_cast<unsigned long>(rep.thread), rep.suspendedUs);
            const Counts dc = dog->counts();
            std::printf("      counts: episodes %u samples %u skipped %u failures %u longest stall %u ms\n", dc.episodes, dc.samples,
                        dc.skippedRateLimit, dc.failures, dc.longestStallMs);
        }
        if (reports.size() == 3) {
            check(reports[0].sampleIndex == 0 && reports[0].ageMs >= 150 && reports[0].ageMs < 420,
                  "S6.ages: the first sample is taken at 150 ms of stall (not before it, and well before 500)");
            check(reports[1].sampleIndex == 1 && reports[1].ageMs >= 500 && reports[1].ageMs < 800,
                  "S6.ages: the second at 500 ms");
            check(reports[2].sampleIndex == 2 && reports[2].ageMs >= 1000 && reports[2].ageMs < 1290,
                  "S6.ages: the third at 1000 ms");
            bool named = true, rightThread = true, quick = true;
            for (const Report& rep : reports) {
                named = named && rep.frames >= 3 && rep.at[0].known && rep.at[0].system &&
                        findFrame(rep, exe, inStall) == rep.owner && rep.owner >= 1;
                rightThread = rightThread && rep.thread == r.tid.load();
                quick = quick && rep.suspendedUs < 20000 && rep.status == CaptureStatus::Ok;
            }
            check(named, "S6.owner: every sample names the OS wait on top and the function the thread stalled in as owner");
            check(rightThread, "S6.thread: and the thread it sampled is the render thread");
            check(quick, "S6.quick: and none stopped it for as long as 20 ms");
        }
        const Counts c = dog->counts();
        check(c.episodes == 1 && c.samples == 3 && c.failures == 0 && c.skippedRateLimit == 0 && c.longestStallMs >= 1000,
              "S6.counts: one episode, three samples, no failure, nothing skipped, the longest stall seen over a second");
        check(r.finished.load() && dog->lastSuspendResultForTest() == 0, "S6.after: the render thread ran to the end of its work, and every stop of it had found it running (previous suspend count 0)");
        dog->stop();
    }
    // S6: the thread that presents is registered when it changes, and the new one is the one sampled.
    {
        Watchdog* dog = new Watchdog;
        Collected got;
        dog->start(&collect, &got);
        Renderer a, b;
        a.dog = b.dog = dog;
        a.beatMs = 200;
        b.beatMs = 150;
        b.blockMs = 400;
        b.tailMs = 200;
        std::vector<Report> none, reports;
        runRenderer(a, got, none, 4000);   // a presents, then is released and ends: the watchdog sees no stall of it yet
        runRenderer(b, got, reports, 6000);
        check(!reports.empty() && reports[0].thread == b.tid.load() && reports[0].thread != a.tid.load() &&
                  dog->counts().handleSlotsUsed == 2,
              "S6.rereg: when another thread starts presenting it is registered and it is the one sampled");
        dog->stop();
    }
}

// ---- S7, S8 -----------------------------------------------------------------------------------------------------
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// The code of a source without its comments (// to the end of the line, and /* */), so a word in a note about what
// the file does NOT do is not taken for a call.
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
    const std::string headerRaw = slurp(root + "\\src\\common\\stall_sampler.h");
    const std::string glueRaw = slurp(root + "\\src\\d3d11\\stall_watch.cpp");
    check(!headerRaw.empty() && !glueRaw.empty(), "S7.read: the sampler's two sources were read");
    if (headerRaw.empty() || glueRaw.empty()) return;
    const std::string header = stripComments(headerRaw), glue = stripComments(glueRaw);
    // No call of the shapes an injector uses, in either file. (The header's comments say what it does not do.)
    const char* const kInjection[] = {"SetThreadContext", "WriteProcessMemory", "ReadProcessMemory", "VirtualAllocEx", "VirtualProtectEx",
                                      "CreateRemoteThread", "NtCreateThreadEx", "RtlCreateUserThread", "NtSuspendThread", "NtResumeThread",
                                      "NtGetContextThread", "NtSetContextThread", "OpenThread", "OpenProcess", "CreateToolhelp32Snapshot",
                                      "Thread32First", "QueueUserAPC", "NtQueueApcThread", "SetWindowsHookEx", "LoadLibrary", "GetProcAddress"};
    std::string found;
    for (const char* api : kInjection) {
        if (has(header, api)) found += std::string(" header:") + api;
        if (has(glue, api)) found += std::string(" glue:") + api;
    }
    check(found.empty(), ("S7.shape: no injection-shaped API in the sampler's code:" + found).c_str());
    check(has(header, "DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,") &&
              has(header, "THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION"),
          "S7.shape: the thread's handle is its own pseudo-handle duplicated by itself, with the three rights a sample needs and no more");

    // The stopped window: from SuspendThread to ResumeThread in captureThread.
    const size_t fn = header.find("inline bool captureThread(");
    const size_t suspend = header.find("SuspendThread(thread)", fn);
    const size_t resume = header.find("ResumeThread(thread)", suspend);
    check(fn != std::string::npos && suspend != std::string::npos && resume != std::string::npos && suspend < resume,
          "S7.window: captureThread stops the thread and then lets it go");
    if (suspend == std::string::npos || resume == std::string::npos) return;
    const std::string window = header.substr(suspend, resume - suspend);
    // The one early return in captureThread is the failed SuspendThread, which is before the thread is stopped:
    // it sits inside the `if (stopped == -1)` block, which closes before GetThreadContext. Everything after that
    // up to the resume is straight-line.
    const size_t afterStop = window.find("GetThreadContext(");
    check(afterStop != std::string::npos, "S7.window: the registers are read inside the window");
    const std::string stopped = afterStop == std::string::npos ? window : window.substr(afterStop);
    const char* const kForbidden[] = {"return", "new ", "malloc", "free(", "snprintf", "printf", "Log", "__try", "try", "throw", "lock",
                                      "std::", "Sleep", "Wait", "LoadLibrary", "GetModule", "VirtualQuery", "memcpy", "strcpy",
                                      "Rtl", "Nt"};
    std::string bad;
    for (const char* w : kForbidden) {
        if (has(stopped, w)) bad += std::string(" ") + w;
    }
    check(bad.empty(), ("S7.window: nothing but the register read, the bounded copy and the resume between the stop and the resume:" + bad).c_str());
    check(header.find("GetThreadContext(", suspend) != std::string::npos && header.find("GetThreadContext(", header.find("GetThreadContext(", suspend) + 1) == std::string::npos,
          "S7.window: one register read and no other call");
    check(header.find("ResumeThread(thread)", fn) == resume && header.find("ResumeThread(thread)", resume + 1) == std::string::npos,
          "S7.window: captureThread has exactly one ResumeThread, on the only path out of the window");
    // S8: the walk and the log come after the resume.
    const size_t sampleFn = header.find("void sample(const Decision& d");
    const size_t capture = header.find("captureThread(s.handle", sampleFn);
    const size_t name = header.find("nameCapture(capture_, r)", sampleFn);
    const size_t sink = header.find("sink_(r, user_)", sampleFn);
    check(sampleFn != std::string::npos && capture != std::string::npos && name != std::string::npos && sink != std::string::npos &&
              capture < name && name < sink,
          "S8.order: the watchdog stops and copies first, walks and names after the resume, and only then writes to the log");
    check(has(glue, "Log::get().note(\"%s\", line);") && !has(header, "Log::get"),
          "S8.log: the log is written by the glue's sink, never by the code that stops the thread");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("stall_sampler_test: dry-run (no thread stopped, no file, no log)");
        return 0;
    }
    if (argc < 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dllPath = self;
    dllPath = dllPath.substr(0, dllPath.find_last_of(L'\\') + 1) + L"stall_target.dll";
    HMODULE dll = LoadLibraryW(dllPath.c_str());
    check(dll != nullptr, "S3.setup: the rig's second DLL loads");
    if (!dll) {
        std::printf("FAIL: stall sampler: the rig's second DLL did not load (%ls)\n", dllPath.c_str());
        return 1;
    }
    const std::string exe = exeName();
    policyCases();
    lineCases();
    captureCases(exe, dll);
    resumeCases(exe);
    watchdogCases(exe);
    if (argc >= 3) sourceCases(argv[2]);
    else std::puts("stall_sampler_test: source pins skipped (no repository root given)");
    if (g_failures) {
        std::printf("FAIL: stall sampler: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u stall sampler checks\n", g_checks);
    return 0;
}
