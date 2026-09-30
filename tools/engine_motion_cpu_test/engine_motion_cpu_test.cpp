// Engine motion's CPU instrument (src\d3d11\engine_motion_cpu.h): the accumulator,
// the sampling gate and the fold, with a fake clock, so every figure is exact.
//
// What it holds to, each a way the first flights of this instrument could have
// lied:
//   - exclusive time: a scope's figure is its own EDVR work; a forward pause
//     leaves the game's code out, a scope nested in EDVR's work is subtracted from
//     its parent, a scope nested in the paused forward is nobody's but its own
//   - no lost update, no contended atomic: four threads write their own slots
//     while another cuts frames; the deltas add back up to the exact call count
//   - thread attribution: the thread that cuts is the render thread, the rest are
//     "other", counted as threads; a frame with no other activity counts none
//   - the sampling gate: the render thread's every call is clocked on every frame;
//     the other threads' only on the sampled frames, one in 32 at a random place in
//     each block of 32; a hook on a frame that is not sampled reads no clock, takes
//     no slot and counts nothing; a scope keeps the decision it made at entry
//   - the sampled frames are exact (every call clocked, the game's forward excluded)
//     and the unsampled frames leave nothing; the other threads' statistics are per
//     sampled frame, and a scope that straddles a cut is not lost from the window
//   - the window's percentiles, per part and total, against known samples in a
//     hostile order
//   - "the code never ran" is "-", never 0.00; a part that ran and rounded to
//     nothing is 0.00; the unclocked evaluator says it is not clocked
//   - the report's four lines state the scheme, and at their worst stay under the
//     log line's limit
//   - the instrument's cost model, weighed by the sampled fraction, reproduces the
//     flown scheme's 1.17 ms a frame at the carrier's call rates, and comes in
//     under 0.15 ms sampled one frame in 32
//   - the first frame primes: the session's earlier work is never one frame's
//   - the window closes at 30 s and when full, and the per-call maxima are the
//     window's own
//   - the instrument's clock floor (a clocked scope, a skipped one, a counted call)
//     is measured, on the real clock, and printed
#include <windows.h>

#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rig {
thread_local bool t_fake = false;
thread_local int64_t t_now = 0;
thread_local int64_t t_step = 0;   // ticks each reading advances the fake clock by (0: it only moves when told)
// A preemption, simulated: after the reading with this ordinal (counted from fake()), the clock jumps a million ticks.
thread_local int64_t t_reads = 0, t_spikeA = -1, t_spikeB = -1;
// After the reading with the ordinal t_stepAt[i], the clock's step becomes t_stepTo[i] (a clock whose speed changes).
thread_local int64_t t_stepAt[3] = {-1, -1, -1}, t_stepTo[3] = {0, 0, 0};
inline int64_t now() {
    if (t_fake) {
        const int64_t v = t_now;
        ++t_reads;
        if (t_reads == t_spikeA || t_reads == t_spikeB) t_now += 1000000;
        for (int i = 0; i < 3; ++i)
            if (t_reads == t_stepAt[i]) t_step = t_stepTo[i];
        t_now += t_step;
        return v;
    }
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
}  // namespace rig
#define EDVR_EMCPU_NOW() (::rig::now())
#include "../../src/d3d11/engine_motion_cpu.h"

using namespace edvr::emcpu;

namespace {
unsigned g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}
bool nearly(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void fake(int64_t at) {
    rig::t_fake = true;
    rig::t_now = at;
    rig::t_step = 0;
    rig::t_reads = 0;
    rig::t_spikeA = rig::t_spikeB = -1;
    for (int i = 0; i < 3; ++i) rig::t_stepAt[i] = -1;
}
// A thread with no slot (a mutant that never registered it) is an answer no expectation equals, not a crash.
uint64_t ticksOf(const Slot* s, unsigned p) { return s ? s->cell[p].ticks.load() : ~0ull; }
uint64_t callsOf(const Slot* s, unsigned p) { return s ? s->cell[p].calls.load() : ~0ull; }
unsigned popcount(const FrameCut& c) {
    unsigned n = 0;
    for (unsigned w = 0; w < kMaskWords; ++w)
        for (uint64_t m = c.otherMask[w]; m; m >>= 1) n += static_cast<unsigned>(m & 1u);
    return n;
}

// One worker thread that runs what it is told, on its own slot and its own fake
// clock, one job at a time (a new thread per job would use up the registry).
class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    ~Worker() {
        {
            std::lock_guard<std::mutex> l(m_);
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    void run(std::function<void()> job) {
        std::unique_lock<std::mutex> l(m_);
        job_ = std::move(job);
        busy_ = true;
        cv_.notify_all();
        cv_.wait(l, [this] { return !busy_; });
    }

private:
    void loop() {
        std::unique_lock<std::mutex> l(m_);
        for (;;) {
            cv_.wait(l, [this] { return busy_ || quit_; });
            if (quit_) return;
            job_();
            busy_ = false;
            cv_.notify_all();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::function<void()> job_;
    bool busy_ = false, quit_ = false;
    std::thread thread_;
};

// ---- 1: exclusive nesting -------------------------------------------------------
void nesting() {
    resetForTest();
    fake(0);
    Token outer, inner;
    enter(outer, kDraw);
    rig::t_now = 10;
    enter(inner, kPatch);
    rig::t_now = 30;
    leave(inner);
    rig::t_now = 100;
    leave(outer);
    const Slot* s = t_ctx.slot;
    check(ticksOf(s, kPatch) == 20 && callsOf(s, kPatch) == 1, "nesting: the inner scope is its own 20 ticks, one call");
    check(ticksOf(s, kDraw) == 80 && callsOf(s, kDraw) == 1,
          "nesting: the outer scope is 100 less the inner's 20: the parts partition");

    // Two levels deep: each level subtracts only its direct child's whole wall time.
    resetForTest();
    fake(0);
    Token a, b, c;
    enter(a, kDraw);
    rig::t_now = 5;
    enter(b, kApply);
    rig::t_now = 8;
    enter(c, kPatch);
    rig::t_now = 18;
    leave(c);   // patch 10
    rig::t_now = 25;
    leave(b);   // apply: 20 wall - 10 = 10
    rig::t_now = 40;
    leave(a);   // draw: 40 wall - 20 = 20
    s = t_ctx.slot;
    check(ticksOf(s, kPatch) == 10 && ticksOf(s, kApply) == 10 && ticksOf(s, kDraw) == 20,
          "nesting: three deep, 10 + 10 + 20 = the 40 ticks of wall time, none twice");
    check(t_ctx.child == 40, "nesting: the top scope leaves its wall time as the (unused) child of no one");
}

// ---- 2: the forward pause ----------------------------------------------------------
void pausing() {
    resetForTest();
    fake(0);
    Token k;
    enter(k, kEmit);
    rig::t_now = 10;
    const int64_t at = pause(k);
    check(at == 10, "pause: returns the reading it took");
    rig::t_now = 60;   // the game's code runs
    const int64_t back = resume(k);
    check(back == 60, "resume: returns the reading it took");
    rig::t_now = 75;
    leave(k);
    const Slot* s = t_ctx.slot;
    check(ticksOf(s, kEmit) == 25 && callsOf(s, kEmit) == 1,
          "pause: 10 before the forward plus 15 after, the 50 in the game's code excluded");

    // A scope opened inside the paused forward (a relay the game's code called) is its
    // own part's time and is not taken from the outer's.
    resetForTest();
    fake(0);
    Token outer, nested;
    enter(outer, kBuilder);
    rig::t_now = 10;
    pause(outer);
    rig::t_now = 20;
    enter(nested, kRigid);
    rig::t_now = 40;
    leave(nested);
    rig::t_now = 60;
    resume(outer);
    rig::t_now = 75;
    leave(outer);
    s = t_ctx.slot;
    check(ticksOf(s, kRigid) == 20 && ticksOf(s, kBuilder) == 25,
          "pause: a nested relay's 20 is its own, and the outer stays 10 + 15");

    // A scope opened after the resume, inside EDVR's own post-forward work, IS subtracted.
    resetForTest();
    fake(0);
    enter(outer, kBuilder);
    rig::t_now = 10;
    pause(outer);
    rig::t_now = 60;
    resume(outer);
    rig::t_now = 62;
    enter(nested, kPatch);
    rig::t_now = 70;
    leave(nested);
    rig::t_now = 80;
    leave(outer);
    s = t_ctx.slot;
    check(ticksOf(s, kPatch) == 8 && ticksOf(s, kBuilder) == 10 + (80 - 60) - 8,
          "pause: a scope in the post-forward work is taken from the outer: 10 + 20 - 8");

    // Leaving while paused (the forward was the last thing): only the head counts.
    resetForTest();
    fake(0);
    enter(k, kClear);
    rig::t_now = 10;
    pause(k);
    rig::t_now = 50;
    leave(k);
    s = t_ctx.slot;
    check(ticksOf(s, kClear) == 10 && callsOf(s, kClear) == 1, "pause: a scope closed while paused keeps only its head");

    // The RAII form, the same arithmetic.
    resetForTest();
    fake(0);
    {
        Scope scope(kJobs);
        rig::t_now = 7;
        scope.pause();
        rig::t_now = 107;
        scope.resume();
        rig::t_now = 110;
    }
    s = t_ctx.slot;
    check(ticksOf(s, kJobs) == 10 && callsOf(s, kJobs) == 1, "Scope: the RAII form pauses and resumes the same way");

    // A backwards clock never records a negative: it clamps to zero.
    resetForTest();
    fake(100);
    enter(k, kTee);
    rig::t_now = 90;
    leave(k);
    s = t_ctx.slot;
    check(ticksOf(s, kTee) == 0 && callsOf(s, kTee) == 1, "clock: a backwards reading records 0 ticks, one call");
}

// ---- 3: counted, not clocked --------------------------------------------------------
void unclocked() {
    resetForTest();
    fake(0);
    for (int i = 0; i < 5; ++i) count(kEval);
    {
        Scope probe(kEval, false);   // the probe branch: clocked, not counted again
        rig::t_now = 12;
    }
    const Slot* s = t_ctx.slot;
    check(callsOf(s, kEval) == 5 && ticksOf(s, kEval) == 12,
          "eval: five calls counted, the probe branch's 12 ticks added without a sixth call");
}

// ---- 4: the per-call maximum belongs to its window ------------------------------------
void callMaxima() {
    resetForTest();
    fake(0);
    FrameCut cut;
    cutFrame(cut);   // this thread is the render thread
    for (int64_t w : {30, 50, 20}) {
        Token k;
        enter(k, kPatch);
        rig::t_now += w;
        leave(k);
    }
    uint64_t renderMax[kParts], otherMax[kParts];
    collectCallMax(renderMax, otherMax);
    check(renderMax[kPatch] == 50 && otherMax[kPatch] == 0, "max: the longest of 30/50/20 is 50, on the render thread");
    Token k;
    enter(k, kPatch);
    rig::t_now += 20;
    leave(k);
    collectCallMax(renderMax, otherMax);
    check(renderMax[kPatch] == 20, "max: the next window starts over (20, not the old 50)");
    collectCallMax(renderMax, otherMax);
    check(renderMax[kPatch] == 0 && renderMax[kDraw] == 0, "max: a window with no scope reports 0, not a stale figure");
}

// ---- 5: thread attribution -------------------------------------------------------------
void attribution() {
    resetForTest();
    fake(0);
    FrameCut cut;
    cutFrame(cut);   // the priming cut: this thread is the render thread
    {
        Token k;
        enter(k, kDraw);
        rig::t_now = 40;
        leave(k);   // render: draw 40
    }
    Worker w1, w2, w3;
    w1.run([] { fake(0); Token k; enter(k, kEmit); rig::t_now = 100; leave(k); });
    w2.run([] { fake(0); Token k; enter(k, kEmit); rig::t_now = 50; leave(k); enter(k, kRigid); rig::t_now = 80; leave(k); });
    w3.run([] { fake(0); count(kEval); count(kEval); });
    cutFrame(cut);
    check(cut.renderTicks[kDraw] == 40 && cut.renderCalls[kDraw] == 1, "attribution: the cutting thread's scope is the render thread's");
    check(cut.renderTicks[kEmit] == 0 && cut.renderCalls[kEmit] == 0, "attribution: no worker's emit is counted as the render thread's");
    check(cut.otherTicks[kEmit] == 150 && cut.otherCalls[kEmit] == 2, "attribution: two workers' emit: 100 + 50 ticks, two calls");
    check(cut.otherTicks[kRigid] == 30 && cut.otherCalls[kEval] == 2, "attribution: rigid 30 ticks, and w3's two counted evals");
    check(popcount(cut) == 3, "attribution: three other threads were active");

    // The next frame: only w1 works. Deltas, not totals; one other thread.
    w1.run([] { Token k; enter(k, kEmit); rig::t_now += 25; leave(k); });
    cutFrame(cut);
    check(cut.otherTicks[kEmit] == 25 && cut.otherCalls[kEmit] == 1 && popcount(cut) == 1 && cut.renderCalls[kDraw] == 0,
          "attribution: the next cut is a delta (25 ticks, one thread), the render thread idle");

    // A worker that appears mid-session starts from zero, not from the session's baseline.
    Worker w4;
    w4.run([] { fake(0); Token k; enter(k, kCopier); rig::t_now = 9; leave(k); });
    cutFrame(cut);
    check(cut.otherTicks[kCopier] == 9 && popcount(cut) == 1, "attribution: a thread first seen mid-session reports its own 9 ticks");
}

// ---- 6: no lost update, real clock, four writers and a cutter ------------------------------
void concurrency() {
    resetForTest();
    rig::t_fake = false;
    FrameCut cut;
    cutFrame(cut);
    constexpr unsigned kThreads = 4, kIterations = 200000;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load()) {
            }
            for (unsigned i = 0; i < kIterations; ++i) {
                Token k;
                enter(k, t & 1 ? kEmit : kCopier);
                pause(k);
                resume(k);
                leave(k);
                count(kEval);
            }
        });
    }
    uint64_t emit = 0, copier = 0, eval = 0, cuts = 0;
    go.store(true);
    for (;;) {
        cutFrame(cut);
        ++cuts;
        emit += cut.otherCalls[kEmit] + cut.renderCalls[kEmit];
        copier += cut.otherCalls[kCopier] + cut.renderCalls[kCopier];
        eval += cut.otherCalls[kEval];
        // Done when the writers have made all their calls: read their running totals.
        uint64_t total = 0;
        const unsigned n = slotCount();
        for (unsigned i = 0; i < n; ++i) total += g_slots[i].cell[kEval].calls.load();
        if (total >= uint64_t(kThreads) * kIterations) break;
        std::this_thread::yield();
    }
    for (auto& th : threads) th.join();
    cutFrame(cut);
    emit += cut.otherCalls[kEmit];
    copier += cut.otherCalls[kCopier];
    eval += cut.otherCalls[kEval];
    check(emit == 2ull * kIterations && copier == 2ull * kIterations && eval == 4ull * kIterations,
          "concurrency: every call of four writers is in the cuts' deltas exactly (no lost update)");
    std::printf("engine_motion_cpu_test: %llu concurrent cuts saw %llu emit, %llu copier calls\n",
                static_cast<unsigned long long>(cuts), static_cast<unsigned long long>(emit),
                static_cast<unsigned long long>(copier));
}

// ---- 7: the window's percentiles ---------------------------------------------------------------
Window& sharedWindow() {
    static Window w;   // 340 KB: not for the stack
    return w;
}
void percentileFold() {
    Window& w = sharedWindow();
    w.reset();
    // Render totals 1..100 ms, fed in a hostile order; the draw side is half of each. The other threads
    // work every frame but a scope of theirs is only recorded on the frames the sampler clocked: here the
    // frames whose v is a multiple of 4 are the sampled ones (25 of 100), and the value is (v+1) x 0.1 ms.
    std::vector<unsigned> order;
    for (unsigned i = 0; i < 100; ++i) order.push_back((i * 37 + 11) % 100);
    std::vector<double> sampledOther;
    for (unsigned v : order) {
        FrameCut c;
        std::memset(&c, 0, sizeof(c));
        const uint64_t ticks = (v + 1) * 1000;   // (v+1) ms at 1000 ticks a ms
        c.renderTicks[kDraw] = ticks / 2;
        c.renderTicks[kApply] = ticks - ticks / 2;
        c.renderCalls[kDraw] = 3;
        c.renderCalls[kApply] = 1;
        c.sampled = v % 4 == 0;
        if (c.sampled) {
            c.otherTicks[kEmit] = (v + 1) * 100;    // (v+1) x 0.1 ms
            c.otherCalls[kEmit] = 10;
            sampledOther.push_back((v + 1) * 0.1);
        }
        c.otherMask[0] = c.sampled ? ((v % 8 == 0) ? 0x5ull : 0x2ull) : 0;   // slots 0 and 2, or slot 1: three threads in all
        w.add(c, 0.001);
    }
    // A scope that straddled a cut is recorded on the next, unsampled, frame: the window keeps it, and
    // it is in no sampled frame's own figure.
    {
        FrameCut c;
        std::memset(&c, 0, sizeof(c));
        c.otherTicks[kEmit] = 5000;   // 5 ms
        c.otherCalls[kEmit] = 1;
        c.sampled = false;
        c.otherMask[0] = 0x1ull;
        w.add(c, 0.001);
    }
    std::sort(sampledOther.begin(), sampledOther.end());
    uint64_t renderMax[kParts] = {}, otherMax[kParts] = {};
    renderMax[kDraw] = 60000;
    otherMax[kEmit] = 4000;
    WindowReport r;
    w.finish(r, 0.001, renderMax, otherMax);
    check(r.valid && r.frames == 101 && r.sampledFrames == 25, "fold: 101 frames, 25 of them sampled");
    check(nearly(r.totalP50, 50.0) && nearly(r.totalP95, 95.0) && nearly(r.totalMax, 100.0),
          "fold: the render thread's totals 0..100 ms (the straddler's frame has none) are over every frame, sampled or not: "
          "p50 50 (n/2), p95 95, max 100, whatever the order");
    check(nearly(r.part[kDraw].renderMax, 50.0) && nearly(r.part[kApply].renderMax, 50.0),
          "fold: the draw side and apply split each frame in half; max 50 each");
    check(r.part[kDraw].renderCalls == 300 && r.part[kApply].renderCalls == 100, "fold: render calls summed over the window");
    check(r.part[kEmit].otherCalls == 251 && nearly(r.part[kEmit].otherMs, 5.0 + 0.1 * (1 + 5 + 9 + 13 + 17 + 21 + 25 + 29 + 33 + 37 + 41 + 45 + 49 + 53 + 57 + 61 + 65 + 69 + 73 + 77 + 81 + 85 + 89 + 93 + 97)),
          "fold: other threads: the sampled frames' calls and ms, and the straddler's, summed over the whole window");
    check(nearly(r.otherP50, sampledOther[sampledOther.size() / 2]) && nearly(r.otherMax, 9.7) &&
              nearly(r.otherP95, sampledOther[static_cast<size_t>(0.95 * sampledOther.size())]),
          "fold: other threads' p50, p95 and max are over the 25 SAMPLED frames only, the straddler not one of them");
    check(nearly(r.part[kDraw].renderCallMaxMs, 60.0) && nearly(r.part[kEmit].otherCallMaxMs, 4.0),
          "fold: the per-call maxima come through in ms");
    check(nearly(r.otherMs, r.part[kEmit].otherMs), "fold: the other-thread total is the sum of its parts");
    check(r.otherThreads == 3, "fold: slots 0, 1 and 2 were active: three other threads");
    check(w.frames() == 0 && w.sampledFrames() == 0, "fold: finishing empties the window");

    // A window of one frame and of none.
    FrameCut c;
    std::memset(&c, 0, sizeof(c));
    c.renderTicks[kTee] = 7000;
    c.renderCalls[kTee] = 1;
    w.add(c, 0.001);
    w.finish(r, 0.001, renderMax, otherMax);
    check(r.frames == 1 && r.sampledFrames == 0 && nearly(r.totalP50, 7.0) && nearly(r.totalP95, 7.0) && nearly(r.totalMax, 7.0) &&
              r.otherMax == 0.0,
          "fold: one frame is its own p50, p95 and max; with no sampled frame the other threads have no statistics");
    w.finish(r, 0.001, renderMax, otherMax);
    check(!r.valid && r.frames == 0, "fold: an empty window is not a report");
}

// ---- 8: the gate, and what a skipped scope does --------------------------------------------------------
void gating() {
    // A clock that leaves a trace of every reading (3 ticks each): a scope that reads it is caught.
    resetForTest();
    fake(0);
    rig::t_step = 3;
    const uint32_t me = GetCurrentThreadId();
    check(currentThreadId() == me, "gate: the TEB read of the thread id agrees with GetCurrentThreadId");
    check(makeGate(true, 7) == (kSampledBit | 7ull) && makeGate(false, 7) == 7ull, "gate: bit 63 is the sampled frame, the low word the render thread");

    // An unsampled frame, and this is not the render thread: a scope is skipped -- no reading, no slot, no count.
    g_gate.store(makeGate(false, me + 1));
    {
        Token k;
        enter(k, kEmit);
        const int64_t a = pause(k), b = resume(k);
        pauseAt(k, 5);
        resumeAt(k, 6);
        leave(k);
        check(a == 0 && b == 0, "gate: a skipped scope's pause and resume take no reading and return 0");
    }
    count(kEval);
    {
        Scope scope(kDraw);
        scope.pause();
        scope.resume();
    }
    check(rig::t_reads == 0 && t_ctx.slot == nullptr && slotCount() == 0,
          "gate: on an unsampled frame a thread that is not the render thread reads no clock, registers no slot and counts nothing");

    // The same frame, and this IS the render thread: every call clocked, every frame.
    g_gate.store(makeGate(false, me));
    {
        Token k;
        enter(k, kDraw);
        leave(k);
    }
    count(kEval);   // counted on sampled frames only
    const Slot* s = t_ctx.slot;
    check(s && callsOf(s, kDraw) == 1 && ticksOf(s, kDraw) == 3 && rig::t_reads == 2,
          "gate: the render thread's scope on an unsampled frame is clocked: two readings (3 ticks apart), one call");
    check(callsOf(s, kEval) == 0, "gate: a counted call on an unsampled frame counts nothing, even on the render thread");

    // A sampled frame: every thread clocked, the counted call counts.
    g_gate.store(makeGate(true, me + 1));
    count(kEval);
    {
        Token k;
        enter(k, kMerge);
        leave(k);
    }
    check(callsOf(s, kEval) == 1 && callsOf(s, kMerge) == 1, "gate: on a sampled frame a scope is clocked and a call counted, whoever the thread");

    // A worker is skipped on the unsampled frame and clocked on the sampled one.
    Worker w;
    g_gate.store(makeGate(false, me));
    w.run([] { fake(0); Token k; enter(k, kBuilder); leave(k); count(kEval); });
    FrameCut cut;
    cutFrame(cut);
    check(cut.otherCalls[kBuilder] == 0 && cut.otherCalls[kEval] == 0 && popcount(cut) == 0,
          "gate: the worker's hooks on an unsampled frame leave nothing for the cut to find");
    g_gate.store(makeGate(true, me));
    w.run([] { Token k; enter(k, kBuilder); leave(k); count(kEval); });
    cutFrame(cut);
    check(cut.otherCalls[kBuilder] == 1 && cut.otherCalls[kEval] == 1 && popcount(cut) == 1,
          "gate: the same hooks on a sampled frame are recorded and the thread is found");

    // A scope that decided at entry keeps its decision: flipping the gate mid-scope does not change it.
    g_gate.store(makeGate(false, me + 1));
    Token skipped;
    enter(skipped, kJobs);
    g_gate.store(makeGate(true, me + 1));
    leave(skipped);
    check(callsOf(s, kJobs) == 0, "gate: a scope skipped at entry stays skipped though the frame became a sampled one");
    Token clocked;
    enter(clocked, kJobs);
    g_gate.store(makeGate(false, me + 1));
    leave(clocked);
    check(callsOf(s, kJobs) == 1, "gate: a scope clocked at entry is recorded though the frame ended");

    // The job bracket's own two readings are shared: pauseAt/resumeAt take the caller's, and add none.
    resetForTest();
    fake(0);
    Token j;
    enter(j, kJobs);   // one reading, at 0
    const int64_t r0 = rig::t_reads;
    pauseAt(j, 10);
    resumeAt(j, 60);
    check(rig::t_reads == r0, "gate: pauseAt and resumeAt take no reading of their own");
    rig::t_now = 75;
    leave(j);
    check(ticksOf(t_ctx.slot, kJobs) == 25 && callsOf(t_ctx.slot, kJobs) == 1,
          "gate: a scope paused and resumed at the caller's readings is the same arithmetic: 10 + 15 ticks, the 50 between excluded");
}

// ---- 9: the schedule ------------------------------------------------------------------------------------
void schedule() {
    SampleSchedule s(32);
    unsigned sampledTotal = 0;
    std::vector<unsigned> positions;
    bool oneEachBlock = true;
    for (unsigned block = 0; block < 2000; ++block) {
        unsigned inBlock = 0, at = 0;
        for (unsigned i = 0; i < 32; ++i) {
            if (s.next()) {
                ++inBlock;
                at = i;
            }
        }
        oneEachBlock = oneEachBlock && inBlock == 1;
        sampledTotal += inBlock;
        positions.push_back(at);
    }
    check(oneEachBlock && sampledTotal == 2000, "schedule: exactly one sampled frame in every block of 32, 2000 blocks");
    std::vector<unsigned> seen(32, 0);
    for (unsigned p : positions) ++seen[p];
    unsigned distinct = 0, longestRun = 1, run = 1;
    for (unsigned n : seen) distinct += n ? 1u : 0u;
    for (size_t i = 1; i < positions.size(); ++i) {
        run = positions[i] == positions[i - 1] ? run + 1 : 1;
        longestRun = std::max(longestRun, run);
    }
    check(distinct == 32 && longestRun < 6,
          "schedule: the position is drawn afresh for every block (all 32 positions turn up, no long run of one), so nothing periodic in the game aliases with it");
    unsigned lo = ~0u, hi = 0;
    for (unsigned n : seen) {
        lo = std::min(lo, n);
        hi = std::max(hi, n);
    }
    check(lo > 20 && hi < 110, "schedule: every position is used about equally often (2000 / 32 = 62 each)");

    SampleSchedule a(32), b(32), other(32, 12345u);
    bool same = true, differs = false;
    for (unsigned i = 0; i < 320; ++i) {
        const bool x = a.next(), y = b.next(), z = other.next();
        same = same && x == y;
        differs = differs || x != z;
    }
    check(same && differs, "schedule: the same seed replays exactly, another seed is another schedule");

    SampleSchedule every(1);
    bool all = true;
    for (int i = 0; i < 10; ++i) all = all && every.next();
    check(all && every.period() == 1, "schedule: a period of 1 samples every frame");
    SampleSchedule zero(0);
    check(zero.period() == 1 && zero.next(), "schedule: a period of 0 is taken as 1");
    SampleSchedule sixteen(16);
    unsigned n16 = 0;
    for (unsigned i = 0; i < 1600; ++i) n16 += sixteen.next() ? 1u : 0u;
    check(n16 == 100, "schedule: one frame in sixteen for a period of sixteen");
}

// ---- 10: the recorder with sampling, end to end ------------------------------------------------------------
constexpr int64_t kFreq = 10000000;   // 10 MHz: a tick is 100 ns
Recorder& sharedRecorder() {
    static Recorder rec;   // 340 KB: not for the stack
    return rec;
}
void recorderEndToEnd() {
    resetForTest();
    fake(0);
    Recorder& rec = sharedRecorder();
    // The session's earlier work, before the first frame: never one frame's. (The gate is the rig's default,
    // sampled, until the recorder's first frame sets it.)
    {
        Token k;
        enter(k, kDraw);
        rig::t_now += 999999;
        leave(k);
    }
    Worker worker;
    worker.run([] { fake(0); });
    Figures f = rec.onFrame(kFreq, 1000);
    check(!f.measured, "recorder: the priming frame reports nothing");
    check(!rec.floor().measured, "recorder: no floor yet at the first frame: it is measured as a window closes");
    check((g_gate.load() & 0xFFFFFFFFull) == GetCurrentThreadId(), "recorder: the gate names the thread that cut as the render thread");

    WindowReport report;
    bool closed = false;
    uint64_t nowMs = 1000;
    unsigned sampledSeen = 0, unsampledExact = 0, sampledExact = 0, renderExact = 0, flagOk = 0, frames = 0;
    int64_t readsOnUnsampled = 0, readsOnSampled = 0;
    for (int frame = 0; frame < 2700 && !closed; ++frame) {
        const bool sampledThisFrame = rec.sampledNow();   // the gate the recorder set at the last cut
        // The render thread: the draw side 0.21 ms in 3 calls, and on frame 10 a 0.01 ms shader patch: clocked on
        // every frame, sampled or not.
        for (int i = 0; i < 3; ++i) {
            Token k;
            enter(k, kDraw);
            rig::t_now += 700;
            leave(k);
        }
        if (frame == 10) {
            Token k;
            enter(k, kPatch);
            rig::t_now += 100;
            leave(k);
        }
        // A job thread: an emit hook of 0.15 ms before the forward, the game's 9 ms, 0.05 ms after, and one counted
        // evaluator call. It counts its own clock readings: on a frame that is not sampled it must make none.
        int64_t before = 0, after = 0;
        worker.run([&] {
            before = rig::t_reads;
            Token k;
            enter(k, kEmit);
            rig::t_now += 1500;
            pause(k);
            rig::t_now += 90000;
            resume(k);
            rig::t_now += 500;
            leave(k);
            count(kEval);
            after = rig::t_reads;
        });
        (sampledThisFrame ? readsOnSampled : readsOnUnsampled) += after - before;
        nowMs += 11;   // 90 Hz, near enough
        f = rec.onFrame(kFreq, nowMs);
        ++frames;
        flagOk += f.sampled == sampledThisFrame ? 1u : 0u;
        renderExact += (nearly(f.renderMs, frame == 10 ? 0.22 : 0.21) && f.renderCalls == (frame == 10 ? 4u : 3u)) ? 1u : 0u;
        if (sampledThisFrame) {
            ++sampledSeen;
            sampledExact += (nearly(f.otherMs, 0.20) && f.otherCalls == 2) ? 1u : 0u;   // the emit, and the counted eval
        } else {
            unsampledExact += (f.otherMs == 0.0 && f.otherCalls == 0) ? 1u : 0u;
        }
        closed = rec.takeReport(report);
    }
    const unsigned workedSampled = sampledSeen;   // the sampled frames the worker did its work in
    check(flagOk == frames, "recorder: every frame's figures say whether it was a sampled frame");
    check(renderExact == frames,
          "recorder: the render thread's figures are exact on EVERY frame, sampled or not: 0.21 ms in 3 calls, 0.22 in 4 with the patch");
    check(workedSampled >= 84 && workedSampled <= 85 && sampledExact == workedSampled,
          "recorder: one frame in 32 was sampled (84 or 85 of 2700), and on each the worker's hooks were clocked exactly: 0.20 ms, 2 calls");
    check(unsampledExact == frames - workedSampled, "recorder: on every other frame the worker's hooks left no time and no call");
    check(readsOnUnsampled == 0, "recorder: the worker read the clock zero times on the frames that were not sampled");
    check(readsOnSampled == 4 * static_cast<int64_t>(workedSampled),
          "recorder: on a sampled frame the worker's scope read the clock four times: enter, pause, resume, leave, fully clocked");
    check(!closed, "recorder: the window did not close before 30 s (2700 frames of 11 ms is 29.7 s)");
    // Run on, with no work, to the deadline: 28 more frames of 11 ms make the 2728th.
    while (!closed) {
        nowMs += 11;
        const Figures g = rec.onFrame(kFreq, nowMs);
        if (g.sampled) ++sampledSeen;
        closed = rec.takeReport(report);
    }
    check(report.valid && report.frames == 2728 && nearly(report.seconds, 30.008),
          "recorder: the window closes at 30 s, at its 2728th frame");
    check(report.samplePeriod == 32 && !report.samplingOff && report.sampledFrames == sampledSeen,
          "recorder: the report carries the period and the number of sampled frames it counted");
    check(report.renderTid == GetCurrentThreadId(), "recorder: the report names the render thread's id");
    check(report.otherThreads == 1, "recorder: one other thread");
    check(report.part[kPatch].renderCalls == 1 && nearly(report.part[kPatch].renderMs, 0.01) && nearly(report.part[kPatch].renderCallMaxMs, 0.01),
          "recorder: exactly one shader patch this window, 0.01 ms, longest 0.01");
    check(report.part[kEmit].otherCalls == workedSampled && nearly(report.part[kEmit].otherMs, workedSampled * 0.20, 1e-6),
          "recorder: the worker's emit calls are the sampled frames', 0.20 ms each, the game's 9 ms in the forward excluded");
    check(report.part[kEval].otherCalls == workedSampled, "recorder: the evaluator's counted calls are the sampled frames' too");
    check(nearly(report.part[kEmit].otherCallMaxMs, 0.20), "recorder: the emit's longest call is 0.20 ms");
    check(nearly(report.otherP50, 0.20) && nearly(report.otherP95, 0.20) && nearly(report.otherMax, 0.20),
          "recorder: other threads per sampled frame: p50, p95 and max all 0.20 ms");
    check(report.part[kDraw].renderCalls == 8100, "recorder: three draw-side calls a frame, 2700 frames");
    check(nearly(report.part[kDraw].renderP50, 0.21) && nearly(report.totalP50, 0.21) && nearly(report.totalMax, 0.22),
          "recorder: draw side p50 0.21 ms; the total's max is the patch frame's 0.22");
    check(report.floor.measured && report.floor.pairs == 512 && report.floor.batches == 4 && nearly(report.floor.plainCostNs, 0.0) &&
              rec.floor().measured,
          "recorder: the window's report carries the floor measured as it closed (zero on a clock that never moves)");
    check(!rec.takeReport(report), "recorder: a report is handed over once");
}

// Nothing at all ran for a window: the report exists and every part reads -.
void recorderIdle() {
    resetForTest();
    fake(0);
    Recorder& idle = *new Recorder();
    idle.onFrame(kFreq, 5000);
    uint64_t nowMs = 5000;
    WindowReport quiet;
    bool got = false;
    while (!got) {
        nowMs += 11;
        idle.onFrame(kFreq, nowMs);
        got = idle.takeReport(quiet);
    }
    bool anyCalls = false;
    for (unsigned p = 0; p < kParts; ++p) anyCalls = anyCalls || quiet.part[p].renderCalls || quiet.part[p].otherCalls;
    check(!anyCalls && quiet.valid && quiet.frames > 2000 && quiet.sampledFrames > 60,
          "recorder: a window in which no hook ran is a full report of zero calls, with its sampled frames counted, not silence");
    delete &idle;
}

// A scope that straddles a cut: entered on a sampled frame, left after the recorder moved on to an unsampled one.
void recorderStraddle() {
    resetForTest();
    fake(0);
    Recorder& rec = *new Recorder(100, 7u);   // one in a hundred: a sampled frame is nearly always followed by an unsampled one
    rec.onFrame(kFreq, 100);
    {
        Worker w;
        w.run([] { fake(0); });
        unsigned guard = 0;
        while (!rec.sampledNow() && guard++ < 400) rec.onFrame(kFreq, 100 + guard);
        check(rec.sampledNow(), "straddle: the fixture reached a sampled frame");
        Token held;
        w.run([&] {
            enter(held, kMerge);
            rig::t_now += 400;
        });   // entered on the sampled frame, still open at its end
        const Figures atCut = rec.onFrame(kFreq, 200);
        check(atCut.sampled && atCut.otherCalls == 0 && atCut.otherMs == 0.0,
              "straddle: a scope still open at the cut is not in that frame's figure");
        check(!rec.sampledNow(), "straddle: and the next frame is an unsampled one");
        w.run([&] {
            rig::t_now += 300;
            leave(held);
        });   // left on the unsampled frame
        const Figures after = rec.onFrame(kFreq, 210);
        check(!after.sampled && after.otherCalls == 1 && nearly(after.otherMs, 0.07),
              "straddle: recorded when it ends, in the frame that follows: 700 ticks, 0.07 ms, one call");
    }
    delete &rec;
}

// The thread-id self-check failing drops the scheme: every thread clocked on every frame, and the report says so.
void recorderFallback() {
    resetForTest();
    fake(0);
    Recorder& off = *new Recorder();
    off.disableSamplingForTest();
    off.onFrame(kFreq, 1000);
    check(off.sampledNow() && (g_gate.load() & kSampledBit) != 0,
          "fallback: with the thread id read failed the gate is a sampled frame, every frame");
    uint64_t nowMs = 1000;
    {
        Worker w;
        w.run([] {
            fake(0);
            Token k;
            enter(k, kEmit);
            rig::t_now += 50;
            leave(k);
        });
        nowMs += 11;
        const Figures fo = off.onFrame(kFreq, nowMs);
        check(fo.sampled && fo.otherCalls == 1, "fallback: the worker's hooks are clocked");
    }
    WindowReport report;
    bool got = false;
    while (!got) {
        nowMs += 11;
        off.onFrame(kFreq, nowMs);
        got = off.takeReport(report);
    }
    check(report.samplingOff && report.sampledFrames == report.frames,
          "fallback: the report says sampling is off, and counts every frame as one the other threads were clocked in");
    char line[1400];
    formatSummary(line, sizeof(line), report);
    check(std::string(line).find("Sampling is OFF") != std::string::npos, "fallback: and the summary line says so in words");
    delete &off;
}

void windowFull() {
    resetForTest();
    fake(0);
    Recorder& rec = *new Recorder();
    rec.onFrame(10000000, 0);
    WindowReport report;
    bool closed = false;
    unsigned frames = 0;
    while (!closed && frames < 7000) {
        rec.onFrame(10000000, 0);   // the clock never advances: only a full window can close it
        ++frames;
        closed = rec.takeReport(report);
    }
    check(closed && report.frames == Window::kMaxFrames && frames == Window::kMaxFrames,
          "window: a full window closes early, at exactly its capacity, however the clock stands");
    check(report.sampledFrames >= Window::kMaxFrames / 32 - 1 && report.sampledFrames <= Window::kMaxFrames / 32 + 1,
          "window: a full window's sampled frames are one in 32");
    delete &rec;
}

// ---- 11: text --------------------------------------------------------------------------------------------
WindowReport sampleReport() {
    WindowReport r;
    r.valid = true;
    r.seconds = 30.0;
    r.frames = 2700;
    r.sampledFrames = 84;
    r.samplePeriod = 32;
    r.renderTid = 4321;
    r.totalP50 = 0.31;
    r.totalP95 = 0.52;
    r.totalMax = 1.94;
    r.otherP50 = 1.20;
    r.otherP95 = 1.90;
    r.otherMax = 2.30;
    r.otherThreads = 7;
    r.floor.measured = true;
    r.floor.pairs = 512;
    r.floor.batches = 4;
    r.floor.plainRecordedNs = 34.0;
    r.floor.plainCostNs = 41.0;
    r.floor.pausedRecordedNs = 60.0;
    r.floor.pausedCostNs = 79.0;
    r.floor.skipNs = 1.5;
    r.floor.countNs = 4.0;
    r.floor.countSkipNs = 0.5;
    PartWindow& draw = r.part[kDraw];
    draw.renderCalls = 140400;
    draw.renderP50 = 0.20;
    draw.renderP95 = 0.35;
    draw.renderMax = 1.20;
    draw.renderCallMaxMs = 0.21;
    draw.renderMs = 540.0;
    PartWindow& apply = r.part[kApply];
    apply.renderCalls = 5400;
    apply.renderP50 = 0.08;
    apply.renderP95 = 0.12;
    apply.renderMax = 0.60;
    apply.renderCallMaxMs = 0.5;
    PartWindow& patch = r.part[kPatch];
    patch.renderCalls = 3;
    patch.renderMs = 4.2;
    patch.renderCallMaxMs = 2.1;
    // The other threads: counted and clocked on the 84 sampled frames only. Emit: 2450 calls in each,
    // 0.5 ms a sampled frame.
    PartWindow& emit = r.part[kEmit];
    emit.otherCalls = 2450ull * 84;
    emit.otherMs = 0.5 * 84;
    emit.otherCallMaxMs = 0.04;
    r.otherMs = 0.5 * 84;
    PartWindow& eval = r.part[kEval];
    eval.otherCalls = 5400ull * 84;
    return r;
}
void textTests() {
    char line[1400];
    WindowReport r = sampleReport();
    formatRenderParts(line, sizeof(line), r);
    const std::string render = line;
    check(render.find("render thread, every call clocked, ms per frame p50/p95/max") != std::string::npos,
          "text: the render thread's line says every call is clocked");
    check(render.find("draw side 0.20/0.35/1.20 (52.0; 0.21)") != std::string::npos,
          "text: the draw side line: p50/p95/max, calls per frame, longest call");
    check(render.find("shader patch 0.00/0.00/0.00 (0.001; 2.10)") != std::string::npos,
          "text: a rare event (three shader patches in 2700 frames) shows 0.001 calls per frame, not 0.0");
    check(render.find("emit -") != std::string::npos && render.find("jobs -") != std::string::npos,
          "text: a part that never ran on the render thread prints -");
    check(render.find("eval -") != std::string::npos, "text: an evaluator that never ran here prints -");
    check(render.find("shader patches this window on every thread: 3, total 4.20 ms, longest 2.10 ms") != std::string::npos,
          "text: shader patches: count, total, longest");

    // Ran, and cost nothing measurable: 0.00, not -.
    r.part[kTee].renderCalls = 2700;
    formatRenderParts(line, sizeof(line), r);
    check(std::string(line).find("tees 0.00/0.00/0.00 (1.0; 0.00)") != std::string::npos,
          "text: a part that ran and rounds to nothing is 0.00, not -");
    r.part[kEval].renderCalls = 84ull * 2461;
    formatRenderParts(line, sizeof(line), r);
    check(std::string(line).find("eval 2461.0 calls per sampled frame, not clocked (a pass-through)") != std::string::npos,
          "text: the render thread's evaluator calls are per SAMPLED frame, and say they are not clocked");
    r.part[kEval].renderCalls = 0;

    formatOtherParts(line, sizeof(line), r);
    const std::string other = line;
    check(other.find("other threads (7), the 84 sampled frames only, thread-ms per sampled frame") != std::string::npos,
          "text: the other threads' line names their count and says it is the sampled frames only, per sampled frame");
    check(other.find("emit 0.50 (2450.0; 0.04)") != std::string::npos,
          "text: emit on other threads: thread-ms per sampled frame, calls per sampled frame, longest call");
    check(other.find("draw side -") != std::string::npos, "text: draw side never ran off the render thread: -");
    check(other.find("eval 5400.0 calls per sampled frame, not clocked (a pass-through)") != std::string::npos,
          "text: the evaluator is counted on the sampled frames and says it is not clocked");
    r.part[kEval].otherMs = 84 * 0.05;
    formatOtherParts(line, sizeof(line), r);
    check(std::string(line).find("probe work clocked 0.05 ms)") != std::string::npos,
          "text: the evaluator's clocked probe work is named separately");

    formatSummary(line, sizeof(line), r);
    const std::string summary = line;
    check(summary.find("30 s, 2700 frames") != std::string::npos && summary.find("Render thread 4321") != std::string::npos,
          "text: the summary names the window and the render thread");
    check(summary.find("Render thread 4321: every call clocked on every frame, total p50 0.31 / p95 0.52 / max 1.94 ms per frame over 55.0 calls per frame")
              != std::string::npos,
          "text: the summary's render thread: every call, every frame, and its call rate (52 draw side + 2 apply + 1 tee + a patch's 0.001)");
    check(summary.find("Other threads (7) are clocked only on the 84 sampled frames (one in 32, at a random position in each block of 32; "
                       "on the other frames a hook is one flag check and is not counted)") != std::string::npos,
          "text: the summary states the scheme: which threads, which frames, one in what, how the frame is chosen, what the rest cost");
    check(summary.find("so these are per sampled frame: mean 0.50 ms (p50 1.20 / p95 1.90 / max 2.30) over 2450.0 calls, about 45.0 ms per s") != std::string::npos,
          "text: the other threads' figures are per sampled frame: mean, p50, p95, max, calls, and the per-second estimate (0.5 ms x 90 frames a second)");

    formatClock(line, sizeof(line), r);
    const std::string clock = line;
    check(clock.find("a clocked scope records 34 ns and costs 41 ns (60 and 79 ns with a forward pause), a skipped one 1.5 ns, a counted call 4.0 ns "
                     "(the fastest of 4 batches of 512 null pairs, measured as this window closed)") != std::string::npos,
          "text: the clock line states the floor for every shape, the skip and the count");
    const InstrumentCost c = instrumentCost(r);
    // Render: plain (140400 + 5400 + 2700 + 3) / 2700 a frame at 41 ns. The other threads: 2450 emit calls a sampled frame at 79 ns,
    // one frame in 32 (84 of 2700), the other 31 pay the skip of 1.5 ns; the evaluator's 5400 calls cost 4 ns sampled, 0.5 ns not.
    const double fraction = 84.0 / 2700.0;
    const double renderExpect = ((140400.0 + 5400.0 + 2700.0 + 3.0) / 2700.0 * 41.0) * 1e-6;
    const double otherExpect = (fraction * (2450.0 * 79.0 + 5400.0 * 4.0) + (1.0 - fraction) * (2450.0 * 1.5 + 5400.0 * 0.5)) * 1e-6;
    check(nearly(c.renderMs, renderExpect, 1e-9) && nearly(c.otherMs, otherExpect, 1e-9) && nearly(c.totalMs, renderExpect + otherExpect, 1e-9),
          "text: the instrument's cost is calls x the calibrated cost by shape, the other threads weighed by the sampled fraction, the skip and the count-skip");
    check(nearly(c.renderRecordedMs, ((140400.0 + 5400.0 + 2700.0 + 3.0) / 2700.0 * 34.0) * 1e-6, 1e-9) &&
              nearly(c.otherRecordedMs, 2450.0 * 60.0 * 1e-6, 1e-9),
          "text: the floor the figures include: per frame for the render thread, per SAMPLED frame for the others");
    check(clock.find("the instrument costs about") != std::string::npos && clock.find("(render thread") != std::string::npos,
          "text: the clock line splits the instrument's cost between the render thread and the others");

    // Nothing ever ran: every part is - and the summary says zero calls, not a fabricated figure.
    WindowReport idle;
    idle.valid = true;
    idle.seconds = 30.0;
    idle.frames = 2700;
    idle.sampledFrames = 84;
    idle.samplePeriod = 32;
    formatRenderParts(line, sizeof(line), idle);
    const std::string idleRender = line;
    bool anyNumbers = false;
    for (unsigned p = 0; p < kParts; ++p) {
        const std::string want = std::string(kInfo[p].name) + " -";
        if (idleRender.find(want) == std::string::npos) anyNumbers = true;
    }
    check(!anyNumbers && idleRender.find("shader patches this window: none") != std::string::npos,
          "text: a window in which nothing ran prints - for every part and says there were no patches");
    formatSummary(line, sizeof(line), idle);
    check(std::string(line).find("over 0.0 calls per frame") != std::string::npos && std::string(line).find("over 0.0 calls, about 0.0 ms per s") != std::string::npos,
          "text: nothing ran: zero calls per frame on the render thread and zero on the sampled frames of the others");
    formatClock(line, sizeof(line), idle);
    check(std::string(line).find("clock and cost: not measured.") != std::string::npos, "text: no floor measured says so");

    // No sampled frame in a window (it closed early), and the fallback.
    WindowReport none = sampleReport();
    none.sampledFrames = 0;
    formatSummary(line, sizeof(line), none);
    check(std::string(line).find("no frame of this window was sampled (one in 32), so they have no figures") != std::string::npos,
          "text: a window with no sampled frame says the other threads have no figures");
    WindowReport off = sampleReport();
    off.samplingOff = true;
    formatSummary(line, sizeof(line), off);
    check(std::string(line).find("Sampling is OFF (the thread id read failed its self-check): every thread was clocked on every frame") != std::string::npos,
          "text: the fallback says sampling is off and why");

    // Overflow threads.
    WindowReport over = sampleReport();
    over.overflowThreads = 3;
    formatSummary(line, sizeof(line), over);
    check(std::string(line).find("3 threads past 128 share one counter: their figures are approximate") != std::string::npos,
          "text: the report says the overflow figures are approximate");
}

void worstCaseLengths() {
    WindowReport r;
    r.valid = true;
    r.seconds = 99999.0;
    r.frames = 999999;
    r.sampledFrames = 999999;
    r.samplePeriod = 4294967295u;
    r.renderTid = 4294967295u;
    r.totalP50 = r.totalP95 = r.totalMax = 99999.99;
    r.otherP50 = r.otherP95 = r.otherMax = 99999.99;
    r.otherMs = 99999999.99;
    r.otherThreads = 128;
    r.overflowThreads = 4294967295u;
    r.floor.measured = true;
    r.floor.pairs = 4294967295u;
    r.floor.batches = 4294967295u;
    r.floor.plainRecordedNs = r.floor.plainCostNs = r.floor.pausedRecordedNs = r.floor.pausedCostNs = 99999.0;
    r.floor.skipNs = r.floor.countNs = r.floor.countSkipNs = 99999.9;
    for (unsigned p = 0; p < kParts; ++p) {
        PartWindow& w = r.part[p];
        w.renderCalls = w.otherCalls = 99999999999ull;
        w.renderP50 = w.renderP95 = w.renderMax = 99999.99;
        w.renderCallMaxMs = w.otherCallMaxMs = 99999.99;
        w.renderMs = w.otherMs = 99999999.99;
    }
    char lines[4][2000];
    const size_t a = formatSummary(lines[0], sizeof(lines[0]), r);
    const size_t b = formatClock(lines[1], sizeof(lines[1]), r);
    const size_t c = formatRenderParts(lines[2], sizeof(lines[2]), r);
    const size_t d = formatOtherParts(lines[3], sizeof(lines[3]), r);
    std::printf("engine_motion_cpu_test: worst-case line lengths: summary %zu, clock %zu, render parts %zu, other parts %zu (limit 1150)\n",
                a, b, c, d);
    check(a < 1150 && b < 1150 && c < 1150 && d < 1150, "text: each of the four lines at its worst fits the log line (about 1166 characters)");
    check(std::strlen(lines[3]) == d && lines[3][d - 1] == '.', "text: the last line ends whole, not cut");
    char tiny[24];
    check(formatSummary(tiny, sizeof(tiny), r) < sizeof(tiny), "text: a short buffer is cut, never overrun");
}

// ---- 12: the cost model, against the flight it came from ----------------------------------------------------
// Flight 112704's busiest window (11:30:34, 2297 frames): the calls per frame the 30 s lines gave, and the
// instrument's own cost the first line put at 1.172 ms with a floor of 33 ns plain, 64 ns with a forward pause.
WindowReport carrierReport(double sampledFraction, const Floor& fl) {
    WindowReport r;
    r.valid = true;
    r.seconds = 25.5;
    r.frames = 2297;
    r.sampledFrames = static_cast<unsigned>(2297 * sampledFraction + 0.5);
    r.samplePeriod = 32;
    r.floor = fl;
    struct Rate { unsigned part; double render, other; };
    const Rate rates[] = {
        {kDraw, 303.0, 0}, {kApply, 2.0, 0}, {kTee, 657.3, 0}, {kPatch, 0, 0},
        {kEmit, 366.5, 2395.7}, {kRigid, 0.4, 2.5}, {kCopier, 4.0, 0}, {kMerge, 85.7, 727.1},
        {kClear, 82.7, 701.7}, {kJobs, 21.1, 247.3}, {kBuilder, 79.0, 13424.9},
    };
    const double sampled = r.sampledFrames;
    for (const Rate& x : rates) {
        r.part[x.part].renderCalls = static_cast<uint64_t>(x.render * 2297 + 0.5);          // every frame
        r.part[x.part].otherCalls = static_cast<uint64_t>(x.other * sampled + 0.5);         // the sampled frames' calls
    }
    // Evaluator calls: counted on sampled frames only, on every thread.
    r.part[kEval].renderCalls = static_cast<uint64_t>(2461.6 * sampled + 0.5);
    r.part[kEval].otherCalls = static_cast<uint64_t>(12387.5 * sampled + 0.5);
    return r;
}
void costModel() {
    Floor fl;
    fl.measured = true;
    fl.pairs = 512;
    fl.batches = 4;
    fl.plainRecordedNs = 16.0;
    fl.plainCostNs = 33.0;
    fl.pausedRecordedNs = 32.0;
    fl.pausedCostNs = 64.0;
    fl.skipNs = 1.5;
    fl.countNs = 4.0;
    fl.countSkipNs = 0.6;

    // Every frame sampled is the old scheme, every call clocked on every thread: the model must give the flight's own figure.
    Floor old = fl;
    old.countNs = 0.0;   // the old scheme counted the evaluator for free in this figure; the flight's 1.172 leaves it out
    const InstrumentCost every = instrumentCost(carrierReport(1.0, old));
    check(nearly(every.totalMs, 1.172, 0.012), "cost model: with every frame clocked it reproduces the flight's own 1.172 ms a frame");
    std::printf("engine_motion_cpu_test: cost model, the carrier's rates, every call clocked on every thread (the flown scheme): %.3f ms a frame\n",
                every.totalMs);

    const InstrumentCost n32 = instrumentCost(carrierReport(1.0 / 32.0, fl));
    const InstrumentCost n16 = instrumentCost(carrierReport(1.0 / 16.0, fl));
    const InstrumentCost n64 = instrumentCost(carrierReport(1.0 / 64.0, fl));
    std::printf("engine_motion_cpu_test: cost model, the carrier's rates, skip %.1f ns, count %.1f ns: one in 16 %.3f ms a frame, "
                "one in 32 %.3f (render thread %.3f, other threads %.3f), one in 64 %.3f\n",
                fl.skipNs, fl.countNs, n16.totalMs, n32.totalMs, n32.renderMs, n32.otherMs, n64.totalMs);
    check(n32.totalMs < 0.15, "cost model: one frame in 32 costs under 0.15 ms a frame at the carrier's call rates");
    check(n16.totalMs > n32.totalMs && n32.totalMs > n64.totalMs, "cost model: a rarer sample is cheaper");
    check(n32.totalMs < every.totalMs / 7.0, "cost model: sampling cuts the instrument's cost by more than seven times");
    check(nearly(n32.renderMs, (1045.0 * 33.0 + 556.7 * 64.0) * 1e-6, 0.004),
          "cost model: the render thread's share is its clocked calls at their shapes (about 0.07 ms), unchanged by the sampling");
}

// ---- 13: more threads than slots ---------------------------------------------------------------------
void slotOverflow() {
    resetForTest();
    fake(0);
    FrameCut cut;
    cutFrame(cut);
    for (unsigned i = 0; i < kMaxSlots + 3; ++i) {
        std::thread([] {
            Token k;
            enter(k, kJobs);
            leave(k);
        }).join();
    }
    check(g_overflowThreads.load() == 4, "slots: the threads past the registry (127 free slots + the render thread's) are counted");
    cutFrame(cut);
    check(cut.otherCalls[kJobs] == kMaxSlots + 3, "slots: an overflowing thread's calls still count, in the shared slot");
    WindowReport r;
    r.valid = true;
    r.overflowThreads = g_overflowThreads.load();
    char line[1400];
    formatSummary(line, sizeof(line), r);
    check(std::string(line).find("share one counter: their figures are approximate") != std::string::npos,
          "slots: the report says the overflow figures are approximate");
}

// ---- 14: the real clock's floor -------------------------------------------------------------------------
void realFloor() {
    resetForTest();
    rig::t_fake = false;
    const Floor f = calibrate(qpcFrequency());
    check(f.measured && f.pairs == 512 && f.batches == 4, "floor: measured on the real clock, four batches of 512 pairs");
    check(f.plainCostNs > 0.0 && f.plainCostNs < 20000.0 && f.pausedCostNs > 0.0 && f.pausedCostNs < 40000.0,
          "floor: a null scope costs a plausible number of nanoseconds (0 < cost < 20 us)");
    check(f.plainRecordedNs >= 0.0 && f.pausedRecordedNs >= 0.0, "floor: what a null scope records is never negative");
    check(f.skipNs >= 0.0 && f.skipNs < f.plainCostNs && f.countNs >= 0.0 && f.countNs < f.plainCostNs && f.countSkipNs >= 0.0 &&
              f.countSkipNs < f.plainCostNs,
          "floor: a skipped scope and a counted call cost less than a clocked scope: no clock read in them");
    std::printf("engine_motion_cpu_test: clock floor on this machine: plain scope records %.1f ns, costs %.1f ns; "
                "with a forward pause records %.1f ns, costs %.1f ns; a skipped scope costs %.2f ns, a counted call %.2f ns "
                "(%.2f ns on an unsampled frame) (fastest of %u batches of %u pairs, QPC %lld Hz)\n",
                f.plainRecordedNs, f.plainCostNs, f.pausedRecordedNs, f.pausedCostNs, f.skipNs, f.countNs, f.countSkipNs, f.batches,
                f.pairs, static_cast<long long>(qpcFrequency()));
    // The model at the carrier's rates, with this machine's own floor.
    const InstrumentCost mine = instrumentCost(carrierReport(1.0 / 32.0, f));
    const InstrumentCost mine16 = instrumentCost(carrierReport(1.0 / 16.0, f));
    const InstrumentCost every = instrumentCost(carrierReport(1.0, f));
    std::printf("engine_motion_cpu_test: at the carrier's call rates with this machine's floor the instrument costs %.3f ms a frame "
                "sampled one in 32 (render thread %.3f, other threads %.3f), %.3f one in 16, against %.3f with every call clocked\n",
                mine.totalMs, mine.renderMs, mine.otherMs, mine16.totalMs, every.totalMs);
    check(mine.totalMs < every.totalMs / 5.0, "floor: with this machine's own floor, sampling still cuts the instrument's cost more than five times");
    const Floor none = calibrate(0);
    check(!none.measured, "floor: no clock rate, no measurement");
    // And on a fake clock everything is exactly zero.
    fake(0);
    const Floor fk = calibrate(10000000);
    check(fk.measured && fk.plainCostNs == 0.0 && fk.plainRecordedNs == 0.0 && fk.skipNs == 0.0 && fk.countNs == 0.0,
          "floor: a fake clock that never moves measures zero");

    // The arithmetic, exactly: a clock that advances 3 ticks (300 ns at 10 MHz) on every reading. A plain
    // pair reads twice: it records 3 ticks (enter to leave) and costs 6 (both readings' advance). A pair
    // with a forward pause reads four times: it records 3 + 3 (the head and the tail, the pause between
    // them left out) and costs 12. The loop's own two bracketing readings add 3 ticks to the total.
    fake(0);
    rig::t_step = 3;
    const Floor step = calibrate(10000000);
    rig::t_step = 0;
    const double pairs = 512.0;
    check(step.measured && nearly(step.plainRecordedNs, 300.0) && nearly(step.plainCostNs, (3.0 + 6.0 * pairs) * 100.0 / pairs),
          "floor: a plain null scope records 300 ns and costs 600 ns (plus the loop's own reading) on a 3-tick-a-reading clock");
    check(nearly(step.pausedRecordedNs, 600.0) && nearly(step.pausedCostNs, (3.0 + 12.0 * pairs) * 100.0 / pairs),
          "floor: a null scope with a forward pause records 600 ns and costs 1200 ns: two more readings, the gap between them out");
    // A skipped scope and a counted call read the clock ZERO times: the only advance across their loops is the two
    // readings that bracket them, 3 ticks, shared over eight times the pairs.
    const double cheap = pairs * kCheapFactor;
    check(nearly(step.skipNs, 3.0 * 100.0 / cheap) && nearly(step.countNs, 3.0 * 100.0 / cheap) && nearly(step.countSkipNs, 3.0 * 100.0 / cheap),
          "floor: a skipped scope, a counted call and a count on an unsampled frame read the clock zero times (only the loop's bracket advances it)");

    // A batch a preemption inflated is not the floor. The clock jumps a million ticks (100 ms) after one reading. Where
    // that lands is set by the order of a calibration's readings (pairs 512, batches 4): 128 to warm up; the tight plain
    // loops 4 x (2 + 1024); the tight paused loops 4 x (2 + 2048); the three cheap loops 4 x 2 each; then the jittered
    // plain scopes 4 x 256 x 2 and the jittered paused scopes 4 x 256 x 4. The COST is the fastest batch: a jump inside the
    // first tight plain batch (reading 300) and the first tight paused batch (reading 5000) leaves the answer the clean one.
    fake(0);
    rig::t_step = 3;
    rig::t_spikeA = 300;
    rig::t_spikeB = 5000;
    const Floor spiked = calibrate(10000000);
    rig::t_step = 0;
    rig::t_spikeA = rig::t_spikeB = -1;
    check(nearly(spiked.plainRecordedNs, 300.0) && nearly(spiked.plainCostNs, (3.0 + 6.0 * pairs) * 100.0 / pairs) &&
              nearly(spiked.pausedRecordedNs, 600.0) && nearly(spiked.pausedCostNs, (3.0 + 12.0 * pairs) * 100.0 / pairs),
          "floor: a preempted batch (a 100 ms jump in the first tight batch of each shape) does not inflate the cost: the fastest batch is kept");

    // The RECORDED time is the median batch: a jump after a scope's opening reading, in the first jittered plain batch
    // and the first jittered paused batch, inflates that batch's recorded total by a hundred milliseconds (a mean of the
    // batches would read about a hundred microseconds a scope); the median of four is the clean 300 and 600.
    const int64_t warm = 128, tightPlain = 4 * (2 + 1024), tightPaused = 4 * (2 + 2048), cheapLoops = 3 * 4 * 2;
    const int64_t jitteredPlain = warm + tightPlain + tightPaused + cheapLoops;   // readings made before the first jittered scope
    const int64_t jitteredPaused = jitteredPlain + 4 * 256 * 2;
    fake(0);
    rig::t_step = 3;
    rig::t_spikeA = jitteredPlain + 1 + 500;    // after the opening reading of the 251st scope of the first plain batch
    rig::t_spikeB = jitteredPaused + 1 + 400;   // after the opening reading of the 101st scope of the first paused batch
    const Floor medians = calibrate(10000000);
    rig::t_step = 0;
    rig::t_spikeA = rig::t_spikeB = -1;
    check(nearly(medians.plainRecordedNs, 300.0) && nearly(medians.pausedRecordedNs, 600.0) &&
              nearly(medians.plainCostNs, (3.0 + 6.0 * pairs) * 100.0 / pairs),
          "floor: a preempted jittered batch does not inflate what a scope records: the median of the four batches is kept");
    // And the same jump really does land inside a measured batch: with a single batch there is no median to save it, and
    // the scope's recorded time is a hundred milliseconds spread over 256 scopes (readings before the first jittered
    // scope of a one-batch calibration: 128 + (2 + 1024) + (2 + 2048) + 3 x 2).
    fake(0);
    rig::t_step = 3;
    rig::t_spikeA = 128 + (2 + 1024) + (2 + 2048) + 3 * 2 + 1 + 500;
    const Floor one = calibrate(10000000, 512, 1);
    rig::t_step = 0;
    rig::t_spikeA = -1;
    check(one.batches == 1 && one.plainRecordedNs > 300000.0 && nearly(one.pausedRecordedNs, 600.0),
          "floor: the jump is real: in a single batch it inflates the recorded time (a hundred milliseconds over 256 scopes)");

    // The median is the middle two of four, not the fastest, the slowest or the mean: four jittered plain batches that
    // record 300, 300, 600 and 1200 ns a scope (the clock's step doubles after the second and again after the third) give
    // 450; the paused batches, the step back at 3, give 600.
    fake(0);
    rig::t_step = 3;
    rig::t_stepAt[0] = jitteredPlain + 2 * 256 * 2;
    rig::t_stepTo[0] = 6;
    rig::t_stepAt[1] = jitteredPlain + 3 * 256 * 2;
    rig::t_stepTo[1] = 12;
    rig::t_stepAt[2] = jitteredPlain + 4 * 256 * 2;
    rig::t_stepTo[2] = 3;
    const Floor stepped = calibrate(10000000);
    rig::t_step = 0;
    for (int i = 0; i < 3; ++i) rig::t_stepAt[i] = -1;
    check(nearly(stepped.plainRecordedNs, 450.0) && nearly(stepped.pausedRecordedNs, 600.0),
          "floor: what a scope records is the median of the batches: 300, 300, 600, 1200 make 450, not 300, 600 or 600");

    // Which gate each cheap shape ran through, which no clock can tell apart: the counted call on a sampled gate counts
    // (4 batches of 512 x kCheapFactor), and the same call on an unsampled gate counts nothing.
    resetForTest();
    fake(0);
    calibrate(10000000);
    check(callsOf(t_ctx.slot, kCal) == 4ull * 512 * kCheapFactor,
          "floor: a counted call is counted on a sampled frame and not on the unsampled one; a skipped scope records nothing");
    // The recorded pass waits a random 0-63 spins between its scopes (2 shapes x 4 batches x 256 scopes, 31.5 on average,
    // about 64,500 in all) so they spread over the clock's tick. The spins increment g_calSink, which the skipped-scope loops
    // leave at zero and nothing else touches: it shows they ran.
    check(g_calSink > 50000 && g_calSink < 80000,
          "floor: the recorded scopes are spread over the clock's tick by a random wait between them (the spins ran)");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
        std::puts("engine_motion_cpu_test: dry-run (no threads, no clock, no files)");
        return 0;
    }
    if (argc != 2 || wcscmp(argv[1], L"--self-test")) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
        return 2;
    }
    nesting();
    pausing();
    unclocked();
    callMaxima();
    attribution();
    concurrency();
    percentileFold();
    gating();
    schedule();
    recorderEndToEnd();
    recorderIdle();
    recorderStraddle();
    recorderFallback();
    windowFull();
    textTests();
    worstCaseLengths();
    costModel();
    slotOverflow();
    realFloor();
    std::printf("engine_motion_cpu_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
