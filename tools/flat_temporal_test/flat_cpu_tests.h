#pragma once

// The flat CPU census (src\d3d11\flat_cpu.h), driven on a fake clock so every figure is exact.
//
// What it holds to, each a way the first flight of this instrument could have lied:
//   - exclusive time: a family's figure is its own work, a nested family is subtracted from its
//     parent, and the families add up to the render thread's total with no tick in two of them
//   - every family the runtime times reaches the line, under the name the rest of the flat log
//     uses, with its calls; a window in which nothing ran still prints every family, as zeros
//   - the render thread is clocked on one frame in 16, exactly one in each block of 16, and on a
//     clocked frame every call is clocked, so a per-call cost is exact and a per-frame figure is
//     the mean over the clocked frames (the line says how many: "clocked on K of N frames"); a
//     sampled window agrees with a fully clocked one on the same workload; present p50 and p95
//     still come from every frame; another thread is clocked only on the sampled frames, one in
//     32, exactly one in each block of 32 and always one of the render thread's clocked frames,
//     and its figure is per SAMPLED frame; a window with no clocked frame says "not clocked", it
//     does not print zeros
//   - engine motion's hook parts (engine_motion_cpu.h) are cut at the same edge and folded in,
//     except its draw side, which the flat draw wrapper's own span already contains
//   - the camera witness prints its cost per write; the draw wrapper prints its D3D calls and the
//     draws they belong to; a GPU span nobody timed prints "-", never 0.00
//   - the census stops when asked (both gates close, nothing more is clocked) and starts afresh
//   - the clock floor is measured once, at the end of the first window
//   - a window with more to say than a log line holds goes out as continuation lines, none over
//     the limit, none dropped, at the worst values the counters can hold
//
// The runtime's wiring (which entry point carries which family's scope, and that the census
// changes no decision) is a source scan in flat_temporal_test.cpp, testFlatCpuWiring.

#include <windows.h>

#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace flat_cpu_rig {
// A clock the rig moves by hand, per thread: a thread that has not switched it on reads the real one.
inline thread_local bool t_fake = false;
inline thread_local int64_t t_now = 0;
inline thread_local int64_t t_step = 0;    // ticks each reading advances the clock by (0: only the rig moves it)
inline thread_local int64_t t_reads = 0;   // readings taken since the clock was switched on
inline int64_t now() {
    if (t_fake) {
        const int64_t v = t_now;
        ++t_reads;
        t_now += t_step;
        return v;
    }
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
}  // namespace flat_cpu_rig
#define EDVR_FLATCPU_NOW() (::flat_cpu_rig::now())
#define EDVR_EMCPU_NOW() (::flat_cpu_rig::now())
#include "../../src/d3d11/flat_cpu.h"

namespace flat_cpu_rig {

inline void fake(int64_t at) {
    t_fake = true;
    t_now = at;
    t_step = 0;
    t_reads = 0;
}

// One worker thread that runs what it is told on its own fake clock, one job at a time.
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
        fake(0);
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

// The frame clock the Present hands the census: 1 tick a microsecond.
constexpr int64_t kFreq = 1000000;

// The lines, joined with a newline, for a search that does not care where a break fell.
inline std::string joined(const edvr::flatcpu::Lines& lines) {
    std::string s;
    for (int i = 0; i < lines.count; ++i) {
        if (i) s += '\n';
        s += lines.line[i];
    }
    return s;
}
inline bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// Runs `frames` frames of `interval` ticks, each doing `work` on the render thread (the caller's),
// and `otherWork` on the worker thread when there is one; returns the window closed at the end.
struct Run {
    std::unique_ptr<edvr::flatcpu::Census> census;
    int64_t now = 0;
    bool primed = false;
    Run() : census(new edvr::flatcpu::Census) {}
    // renderPeriod 1: the render thread clocked on every frame, the census as it was before 2026-09-30.
    explicit Run(unsigned renderPeriod) : census(new edvr::flatcpu::Census(renderPeriod)) {}
    void frame(int64_t interval, bool paused, const std::function<void()>& work, Worker* worker = nullptr,
               const std::function<void()>& otherWork = {}) {
        if (!primed) {
            census->onFrame(now, kFreq, false);
            primed = true;
        }
        if (work) work();
        if (worker && otherWork) worker->run(otherWork);
        now += interval;
        census->onFrame(now, kFreq, paused);
    }
};

}  // namespace flat_cpu_rig

inline int flatCpuTests() {
    using namespace edvr;
    using namespace flat_cpu_rig;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: census %s\n", name); ++failures; }
    };
    auto nearly = [](double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; };

    // ---- 1: exclusive nesting, in the scope itself -------------------------------------------
    {
        flatcpu::resetForTest();
        fake(0);
        {
            flatcpu::Scope outer(flatcpu::kOther);
            t_now = 10;
            {
                flatcpu::Scope inner(flatcpu::kReduce);
                t_now = 30;
                {
                    flatcpu::Scope innermost(flatcpu::kTrace);
                    t_now = 35;
                }
                t_now = 40;
            }
            t_now = 100;
        }
        const flatcpu::Slot* s = flatcpu::t_ctx.slot;
        expect(s && s->cell[flatcpu::kTrace].ticks.load() == 5 && s->cell[flatcpu::kTrace].calls.load() == 1,
               "the innermost scope is its own 5 ticks");
        expect(s && s->cell[flatcpu::kReduce].ticks.load() == 25 && s->cell[flatcpu::kReduce].calls.load() == 1,
               "the middle scope is its 30 ticks (10 to 40) less the innermost's 5, and none of the outer's");
        expect(s && s->cell[flatcpu::kOther].ticks.load() == 70 && s->cell[flatcpu::kOther].calls.load() == 1,
               "the outer scope is its 100 ticks less the middle's whole 30: 70; the three add up to 100");
    }

    // ---- 2: a frame's figures, in the census and in the words ---------------------------------
    // 300 frames of 16,667 ticks: the shell 50 own ticks, contract reduction 40, the witness 30
    // (three writes of 10), the trace ring 20, then 10 more of the shell's own.
    {
        flatcpu::resetForTest();
        fake(0);
        Run run;
        for (int i = 0; i < 300; ++i) {
            run.frame(16667, false, [] {
                flatcpu::Scope shell(flatcpu::kOther);
                t_now += 50;
                { flatcpu::Scope r(flatcpu::kReduce); t_now += 40; }
                for (int w = 0; w < 3; ++w) { flatcpu::Scope wt(flatcpu::kWitness); t_now += 10; }
                { flatcpu::Scope t(flatcpu::kTrace); t_now += 20; }
                t_now += 10;
            });
        }
        flatcpu::WindowReport r;
        const bool got = run.census->take(run.now, false, r);
        // The render thread is clocked on one frame in each block of 16: 300 frames are 18 whole blocks and
        // 12 frames of a 19th, which clock 18 or 19 frames. Every clocked frame does the same work, so the
        // per-frame figures do not depend on which.
        const unsigned K = r.renderClockedFrames;
        const double k = static_cast<double>(K);
        expect(got && r.valid && r.frames == 300 && r.pausedFrames == 0, "the window closes after 5 s with 300 frames");
        expect(K == 18 || K == 19, "the render thread is clocked on one frame in each block of 16: 18 or 19 of 300");
        expect(nearly(r.renderMs[flatcpu::kOther] / k, 0.060) && r.renderCalls[flatcpu::kOther] == K,
               "other: 60 ticks and one call on each clocked frame");
        expect(nearly(r.renderMs[flatcpu::kReduce] / k, 0.040) && r.renderCalls[flatcpu::kReduce] == K,
               "contract reduction: 40 ticks and one call on each clocked frame");
        expect(nearly(r.renderMs[flatcpu::kWitness] / k, 0.030) && r.renderCalls[flatcpu::kWitness] == 3u * K,
               "camera witness: 30 ticks and three calls on each clocked frame");
        expect(nearly(r.presentP50Ms, 16.667) && nearly(r.presentP95Ms, 16.667), "present p50 and p95 are the frame interval");
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        const std::string text = joined(lines);
        const std::string promised =
            "flat cpu 5s: frames=300 (stood down 0) present p50 16.67 ms (p95 16.67); render thread clocked on " +
            std::to_string(K) + " of 300 frames, one in 16 (its figures are per clocked frame); EDVR per frame total 0.150 ms = "
            "other 0.060 ms (calls 1.0) + contract reduction 0.040 ms (calls 1.0) + copy checks 0.000 ms (calls 0.0) + "
            "camera rows 0.000 ms (calls 0.0) + trace ring 0.020 ms (calls 1.0) + resource lookup 0.000 ms (calls 0.0) + "
            "coverage 0.000 ms (calls 0.0) + projection readiness 0.000 ms (calls 0.0) + cb shadows 0.000 ms (calls 0.0) + "
            "camera witness 0.030 ms (calls 3.0) + engine motion draw wrapper 0.000 ms (calls 0.0) + resolve 0.000 ms (calls 0.0) + "
            "backend 0.000 ms (calls 0.0) + discovery 0.000 ms (calls 0.0) + state trackers 0.000 ms (calls 0.0) + "
            "hdr route 0.000 ms (calls 0.0) + foreground capture 0.000 ms (calls 0.0) + camera inject 0.000 ms (calls 0.0) + engine motion hooks 0.000 ms (calls 0.0)";
        expect(std::string(lines.line[0]).compare(0, promised.size(), promised) == 0,
               "the first line has the promised shape, in the promised order, with the figures the frames spent");
        expect(has(text, "camera witness 10.00 us/write (" + std::to_string(3u * K) + " writes clocked)"),
               "the witness prints its cost per write, and how many writes it clocked");
        expect(has(text, "engine motion hooks 0.000 ms (calls 0.0)"), "engine motion's hooks print as zero when they never ran");
        expect(lines.count >= 1 && !lines.truncated, "a window fits the lines the log holds");
        expect(std::strncmp(lines.line[0], "flat cpu 5s:", 12) == 0, "the first line is the flat cpu 5s line");
        for (int i = 1; i < lines.count; ++i)
            expect(std::strncmp(lines.line[i], "flat cpu 5s (cont.): ", 21) == 0, "a continuation line says it is one");
        for (int i = 0; i < lines.count; ++i)
            expect(std::strlen(lines.line[i]) <= flatcpu::kLineLimit, "no line is longer than the log keeps");
    }

    // ---- 3: every family reaches the line, under its name ---------------------------------------
    {
        for (unsigned fam = 0; fam < flatcpu::kFamilies; ++fam) {
            flatcpu::resetForTest();
            fake(0);
            Run run;
            for (int i = 0; i < 300; ++i) {
                run.frame(16667, false, [fam] {
                    flatcpu::Scope one(fam);
                    t_now += 7;
                });
            }
            flatcpu::WindowReport r;
            const bool got = run.census->take(run.now, false, r);
            flatcpu::Lines lines;
            flatcpu::formatWindow(r, &lines);
            const std::string text = joined(lines);
            const std::string entry = std::string(flatcpu::kInfo[fam].name) + " 0.007 ms (calls 1.0)";
            expect(got && r.renderClockedFrames > 0 && nearly(r.renderMs[fam] / r.renderClockedFrames, 0.007) &&
                       r.renderCalls[fam] == r.renderClockedFrames,
                   "each family accumulates the ticks and calls its scope was given, on the frames it was clocked");
            // A game-thread family with render-thread time prints it in the total's chain like every other.
            expect(has(text, entry), "each family's figure is on the line under the name the flat log uses");
            for (unsigned other = 0; other < flatcpu::kFamilies; ++other) {
                if (other == fam) continue;
                expect(r.renderCalls[other] == 0, "a family nothing ran in stays at zero calls");
            }
        }
    }

    // ---- 4: a window in which nothing ran still prints everything ---------------------------------
    {
        flatcpu::resetForTest();
        fake(0);
        Run run;
        for (int i = 0; i < 300; ++i) run.frame(16667, false, {});
        flatcpu::WindowReport r;
        const bool got = run.census->take(run.now, false, r);
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        const std::string text = joined(lines);
        expect(got && lines.count >= 1, "an empty window still closes and prints");
        bool all = true;
        for (unsigned fam = 0; fam < flatcpu::kFamilies; ++fam)
            all = all && has(text, std::string(flatcpu::kInfo[fam].name) + " 0.000 ms (calls 0.0)");
        expect(all, "every family is on the line of an empty window, as zeros");
        expect(has(text, "EDVR per frame total 0.000 ms"), "and the total is zero");
        expect(has(text, "GPU frame - (") && has(text, "GPU resolve - ("), "a GPU span nobody timed prints a dash, not 0.00");
        expect(!has(text, "GPU frame p50") && !has(text, "GPU resolve p50"), "and no percentile");
        expect(has(text, "engine motion wrapper D3D calls 0/frame over 0.0 substituted draws/frame"),
               "the draw wrapper's calls print as zero");
    }

    // ---- 5: another thread is clocked on one frame in 32, and its figure is per sampled frame ----
    {
        flatcpu::resetForTest();
        fake(0);
        Run run;
        Worker worker;
        // 320 frames of 15,625 ticks is exactly 5 s, and ten blocks of 32.
        for (int i = 0; i < 320; ++i) {
            run.frame(15625, false,
                      [] { flatcpu::Scope one(flatcpu::kInject); t_now += 3; },   // the render thread: 3 ticks
                      &worker,
                      [] {
                          // The game's thread: five camera inject calls of 20 ticks, every frame.
                          for (int c = 0; c < 5; ++c) { flatcpu::Scope one(flatcpu::kInject); t_now += 20; }
                      });
        }
        flatcpu::WindowReport r;
        const bool got = run.census->take(run.now, false, r);
        expect(got && r.frames == 320, "320 frames of 15.625 ms close a 5 s window");
        expect(r.sampledFrames == 10, "exactly one frame in each block of 32 is sampled: ten in ten blocks");
        expect(r.renderClockedFrames == 20, "the render thread is clocked on one frame in each block of 16: twenty in twenty blocks");
        expect(r.renderCalls[flatcpu::kInject] == 20, "the render thread is clocked on every call of its twenty clocked frames, the sampled ones among them");
        expect(r.otherCalls[flatcpu::kInject] == 50, "the other thread is clocked on the ten sampled frames only: 10 x 5 calls");
        expect(nearly(r.otherMs[flatcpu::kInject], 50 * 20 * 0.001), "and its time is those calls' 20 ticks each");
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        const std::string text = joined(lines);
        expect(has(text, "other threads (clocked on 10 of 320 frames, one in 32; thread-ms per clocked frame): camera inject 0.100 ms (calls 5.0)"),
               "the other threads' line says how many frames it was clocked on and prints per clocked frame");
        expect(has(text, "camera inject 0.003 ms (calls 1.0)"), "the render thread's own inject time is on the render chain");
    }

    // ---- 6: a window with no sampled frame, or no clocked frame, says so --------------------------------
    {
        flatcpu::WindowReport r;
        r.valid = true;
        r.frames = 40;
        r.sampledFrames = 0;
        r.renderClockedFrames = 2;
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        const std::string text = joined(lines);
        expect(has(text, "other threads: not clocked in this window (one frame in 32 is, and none was)"),
               "no sampled frame: the other threads' line says they were not clocked");
        expect(!has(text, "camera inject 0.000 ms (calls 0.0) + engine emit"),
               "and does not print their figures as zeros");
        expect(has(text, "render thread clocked on 2 of 40 frames, one in 16"), "the render thread's clocked frames are counted on the line");

        // Not one frame of the window was clocked on the render thread (a few slow frames): a dash, never a zero.
        flatcpu::WindowReport none;
        none.valid = true;
        none.frames = 5;
        none.sampledFrames = 0;
        none.renderClockedFrames = 0;
        flatcpu::formatWindow(none, &lines);
        const std::string noneText = joined(lines);
        expect(has(noneText, "render thread clocked on 0 of 5 frames, one in 16") && has(noneText, "EDVR per frame total -"),
               "no clocked frame: the total is a dash");
        expect(has(noneText, "render thread: not clocked in this window (one frame in 16 is, and none was)"),
               "and the render thread's line says it was not clocked");
        expect(!has(noneText, "other 0.000 ms (calls 0.0)") && !has(noneText, "engine motion hooks 0.000 ms"),
               "and prints none of its families as zeros");
        expect(has(noneText, "present p50 ") && has(noneText, "GPU frame - ("), "the figures that do not need the clocks still print");
    }

    // ---- 7: engine motion's parts, driven and folded ----------------------------------------------------
    {
        flatcpu::resetForTest();
        fake(0);
        Run run;
        Worker worker;
        for (int i = 0; i < 320; ++i) {
            run.frame(15625, false,
                      [] {
                          // The draw side is inside the flat draw wrapper's own span: not folded in twice.
                          { emcpu::Scope draw(emcpu::kDraw); t_now += 50; }
                          { emcpu::Scope apply(emcpu::kApply); t_now += 9; }
                          { emcpu::Scope patch(emcpu::kPatch); t_now += 8; }
                          { emcpu::Scope tee(emcpu::kTee); t_now += 20; }
                      },
                      &worker,
                      [] {
                          { emcpu::Scope emit(emcpu::kEmit); t_now += 30; }
                          { emcpu::Scope copier(emcpu::kCopier); t_now += 10; }
                          { emcpu::Scope merge(emcpu::kMerge); t_now += 6; }
                          { emcpu::Scope clear(emcpu::kClear); t_now += 4; }
                          for (int c = 0; c < 8; ++c) emcpu::count(emcpu::kEval);
                      });
        }
        flatcpu::WindowReport r;
        const bool got = run.census->take(run.now, false, r);
        expect(got, "the window closes with engine motion driven by the census");
        expect(r.renderClockedFrames == 20, "engine motion's render-thread hooks follow the census's gate: twenty clocked frames of 320");
        expect(nearly(r.emRenderMs[emcpu::kTee] / 20.0, 0.020) && r.emRenderCalls[emcpu::kTee] == 20,
               "the render thread's tees are cut at the frame edge, on the clocked frames");
        expect(r.emRenderCalls[emcpu::kDraw] == 20 && nearly(r.emRenderMs[emcpu::kDraw] / 20.0, 0.050),
               "the draw side is cut too (it is in the report, and not in the fold)");
        expect(r.emOtherCalls[emcpu::kEmit] == 10 && r.emOtherCalls[emcpu::kCopier] == 10 &&
               r.emOtherCalls[emcpu::kMerge] == 10 && r.emOtherCalls[emcpu::kClear] == 10,
               "the other thread's hooks are clocked on the ten sampled frames only");
        expect(r.emRenderCalls[emcpu::kEval] + r.emOtherCalls[emcpu::kEval] == 80,
               "the evaluator relays are counted, on the sampled frames only: 10 x 8");
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        const std::string text = joined(lines);
        expect(has(text, "engine motion hooks 0.020 ms (calls 1.0)"),
               "the render thread's engine motion hooks are the tees alone: the draw side, apply and patch are not counted twice");
        expect(has(text, "EDVR per frame total 0.020 ms"), "and the total is the same 0.020 ms, no draw side in it");
        expect(has(text, "engine emit 0.030 ms (calls 1.0)") && has(text, "engine copier 0.010 ms (calls 1.0)") &&
               has(text, "engine merge 0.006 ms (calls 1.0)") && has(text, "engine clear 0.004 ms (calls 1.0)"),
               "the emit, copier, merge and clear callbacks are on the other threads' line, per sampled frame");
        expect(has(text, "engine jobs 0.000 ms (calls 0.0)") && has(text, "engine builder 0.000 ms (calls 0.0)") &&
               has(text, "engine rigid emit 0.000 ms (calls 0.0)") && has(text, "engine tees 0.000 ms (calls 0.0)"),
               "the parts that did not run print as zeros");
        expect(has(text, "engine eval relays 8.0 calls (counted, not clocked)"), "the evaluator relays print as a count");
    }

    // ---- 8: the draw wrapper's D3D calls, the GPU spans, the paused frames -------------------------------
    {
        flatcpu::resetForTest();
        fake(0);
        Run run;
        for (int i = 0; i < 300; ++i) {
            run.frame(16667, i % 6 == 0, {});
            run.census->noteWrapper(4700, 430);
        }
        for (double ms : {1.0, 2.0, 3.0, 4.0, 5.0}) run.census->noteGpuFrame(ms);
        for (double ms : {0.5, 0.7, 0.9}) run.census->noteGpuResolve(ms);
        run.census->noteGpuSkipped();
        run.census->noteGpuSkipped();
        run.census->noteGpuSkipped();
        run.census->noteGpuInvalid();
        flatcpu::WindowReport r;
        const bool got = run.census->take(run.now, true, r);
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        const std::string text = joined(lines);
        expect(got && r.pausedFrames == 50, "a paused frame is counted as one");
        expect(has(text, "frames=300 (stood down 50, stood down now)"), "the window says how many frames were stood down, and that it is now");
        expect(has(text, "engine motion wrapper D3D calls 4700/frame over 430.0 substituted draws/frame"),
               "the draw wrapper's D3D calls and substituted draws print per frame");
        expect(has(text, "GPU frame p50 3.00 / p95 5.00 ms (first game draw to Present; 5 timed, 3 skipped, 1 invalid)"),
               "the whole-frame GPU span prints its median, p95 and what could not be timed");
        expect(has(text, "GPU resolve p50 0.70 / p95 0.90 ms (3 timed)"), "the resolver's GPU span prints its median and p95");
        // The next window starts empty.
        for (int i = 0; i < 300; ++i) run.frame(16667, false, {});
        flatcpu::WindowReport next;
        const bool gotNext = run.census->take(run.now, false, next);
        expect(gotNext && next.pausedFrames == 0 && next.gpuFrameSamples == 0 && next.stateCalls == 0 && next.gpuSkipped == 0,
               "a window is emptied when it is taken");
    }

    // ---- 8b: the query shortcuts' counts (flat_query_cut.h) -------------------------------------------------
    {
        flatcpu::resetForTest();
        fake(0);
        Run run;
        const unsigned depth = static_cast<unsigned>(edvr::FlatQuery::CoverageDepth), blend = static_cast<unsigned>(edvr::FlatQuery::GameBlend);
        for (int i = 0; i < 300; ++i) {
            run.frame(16667, false, {});
            edvr::FlatQueryCounts c;
            c.served[depth] = 1130;
            c.served[blend] = 40;
            if (i % 64 == 0) { c.sampled[depth] = 4; c.sampled[blend] = 2; }
            run.census->noteQueries(c);
        }
        flatcpu::WindowReport r;
        const bool got = run.census->take(run.now, false, r);
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        const std::string text = joined(lines);
        expect(got && r.queries.served[depth] == 300ull * 1130 && r.queries.sampled[depth] == 20 && r.queries.sampled[blend] == 10,
               "the window sums the frames' query counts");
        expect(has(text, "query shortcuts (answers a frame from what the runtime tracks; one frame in 64 also asks the context and compares): "
                         "coverage depth view 1130.0 (checked 20, wrong 0), coverage shader identity 0.0 (checked 0, wrong 0), "
                         "game blend state 40.0 (checked 10, wrong 0)"),
               "the line says how many questions a frame were answered from what the runtime tracks, per state, and how many were checked");
        expect(has(text, "no state asks the context again"), "and says that no state fell back");
        expect(!lines.truncated, "and the window still fits the lines");
        // A state that fell back is named, with how often it asks now.
        for (int i = 0; i < 300; ++i) {
            run.frame(16667, false, {});
            edvr::FlatQueryCounts c;
            c.served[depth] = 1130;
            c.asked[blend] = 40;
            c.mismatched[blend] = i == 0 ? 1 : 0;
            c.fellBack = 1u << blend;
            run.census->noteQueries(c);
        }
        flatcpu::WindowReport fell;
        const bool gotFell = run.census->take(run.now, false, fell);
        flatcpu::Lines fellLines;
        flatcpu::formatWindow(fell, &fellLines);
        const std::string fellText = joined(fellLines);
        expect(gotFell && fell.queries.served[blend] == 0 && fell.queries.mismatched[blend] == 1,
               "the next window starts empty, and counts the disagreement that came");
        expect(has(fellText, "game blend state 0.0 (checked 0, wrong 1)") && has(fellText, "ASKS THE CONTEXT AGAIN: game blend state (40.0 a frame)") &&
                   !has(fellText, "no state asks the context again"),
               "a state that fell back is named on the line, with how often it asks now");
        expect(!fellLines.truncated, "and that window fits the lines too");
    }
    // ---- 9: the census stops when asked, and starts afresh ------------------------------------------------
    {
        flatcpu::resetForTest();
        fake(0);
        Run run;
        for (int i = 0; i < 10; ++i) run.frame(16667, false, [] { flatcpu::Scope one(flatcpu::kReduce); t_now += 5; });
        expect(run.census->running(), "the census is running once it has had a frame");
        run.census->idle();
        expect(!run.census->running(), "idle stops it");
        expect(flatcpu::g_gate.load() == 0 && emcpu::g_gate.load() == 0, "both gates are closed");
        const flatcpu::Slot* s = flatcpu::t_ctx.slot;
        const uint64_t callsBefore = s ? s->cell[flatcpu::kReduce].calls.load() : 0;
        { flatcpu::Scope one(flatcpu::kReduce); t_now += 5; }
        { emcpu::Scope one(emcpu::kTee); t_now += 5; }
        expect(s && s->cell[flatcpu::kReduce].calls.load() == callsBefore, "a scope opened while the census is stopped is not clocked");
        flatcpu::WindowReport stoppedReport;
        expect(!run.census->take(run.now + 60000000, false, stoppedReport), "a stopped census takes no window");
        // Afresh: the first frame primes, nothing from before is in the next window.
        Run again;
        again.census = std::move(run.census);
        again.now = run.now;
        for (int i = 0; i < 300; ++i) again.frame(16667, false, {});
        flatcpu::WindowReport r;
        const bool got = again.census->take(again.now, false, r);
        expect(got && r.frames == 300 && r.renderCalls[flatcpu::kReduce] == 0,
               "after a stop the next window holds only its own frames");
    }

    // ---- 10: the clock floor is measured once, at the end of the first window ----------------------------------
    {
        flatcpu::resetForTest();
        fake(0);
        t_step = 2;   // every reading advances the clock two ticks: a floor exists
        Run run;
        for (int i = 0; i < 300; ++i) run.frame(16667, false, {});
        flatcpu::WindowReport first;
        const int64_t readsBefore = t_reads;
        const bool got = run.census->take(run.now, false, first);
        const int64_t calibrationReads = t_reads - readsBefore;
        expect(got && first.floor.measured && first.emFloor.measured, "the first window carries a measured floor, this instrument's and engine motion's");
        expect(first.floor.costNs > 0 && first.emFloor.plainCostNs > 0, "the floor costs something on a clock that moves");
        expect(calibrationReads > 1000, "the first take read the clock to measure it");
        for (int i = 0; i < 300; ++i) run.frame(16667, false, {});
        flatcpu::WindowReport second;
        const int64_t readsBeforeSecond = t_reads;
        const bool gotSecond = run.census->take(run.now, false, second);
        expect(gotSecond && second.floor.measured && t_reads == readsBeforeSecond,
               "the second take measures nothing again: the floor is measured once");
        expect(nearly(first.floor.costNs, second.floor.costNs), "and reports the same floor");
        flatcpu::Lines lines;
        flatcpu::formatWindow(first, &lines);
        expect(has(joined(lines), "clock floor ") && has(joined(lines), "engine hooks floor "), "the floor is on the line");
    }

    // ---- 11: the worst values still fit the lines, split not dropped --------------------------------------------
    {
        flatcpu::WindowReport r;
        r.valid = true;
        r.frames = 5000;
        r.pausedFrames = 4999;
        r.sampledFrames = 156;
        r.renderClockedFrames = 313;
        r.standingDown = true;
        r.presentP50Ms = 99999.99;
        r.presentP95Ms = 99999.99;
        for (unsigned f = 0; f < flatcpu::kFamilies; ++f) {
            r.renderMs[f] = 9.9e9;
            r.otherMs[f] = 9.9e9;
            r.renderCalls[f] = 999999999999ull;
            r.otherCalls[f] = 999999999999ull;
        }
        for (unsigned p = 0; p < emcpu::kParts; ++p) {
            r.emRenderMs[p] = 9.9e9;
            r.emOtherMs[p] = 9.9e9;
            r.emRenderCalls[p] = 999999999999ull;
            r.emOtherCalls[p] = 999999999999ull;
        }
        r.stateCalls = r.substitutedDraws = 99999999999999ull;
        r.gpuFrameSamples = r.gpuResolveSamples = 2048;
        r.gpuFrameP50 = r.gpuFrameP95 = r.gpuResolveP50 = r.gpuResolveP95 = 99999.99;
        r.gpuSkipped = r.gpuInvalid = 999999999999ull;
        r.floor.measured = r.emFloor.measured = true;
        r.floor.costNs = r.floor.recordedNs = 99999;
        r.emFloor.plainCostNs = r.emFloor.pausedCostNs = r.emFloor.plainRecordedNs = r.emFloor.pausedRecordedNs = 99999;
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        bool within = true;
        for (int i = 0; i < lines.count; ++i) within = within && std::strlen(lines.line[i]) <= flatcpu::kLineLimit;
        expect(lines.count >= 2 && lines.count <= flatcpu::kMaxLines && !lines.truncated && within,
               "at the worst values the window is a few lines, none over the limit, nothing dropped");
        const std::string text = joined(lines);
        bool named = true;
        for (unsigned f = 0; f < flatcpu::kFamilies; ++f) named = named && has(text, flatcpu::kInfo[f].name);
        for (unsigned i = 0; i < flatcpu::kEmNameCount; ++i) named = named && has(text, flatcpu::kEmNames[i].name);
        named = named && has(text, "engine motion hooks") && has(text, "GPU frame") && has(text, "GPU resolve") &&
                has(text, "clock floor") && has(text, "camera witness") && has(text, "engine motion wrapper D3D calls") &&
                has(text, "render thread clocked on 313 of 5000 frames, one in 16") && has(text, "the clocks cost the render thread");
        expect(named, "and every family, engine part and figure is still on them");
        for (int i = 1; i < lines.count; ++i)
            expect(std::strncmp(lines.line[i], "flat cpu 5s (cont.): ", 21) == 0, "every line after the first says it continues");
    }

    // ---- 12: the sampling schedules --------------------------------------------------------------------------------
    {
        flatcpu::SampleSchedule schedule;
        bool exactlyOne = true;
        for (int block = 0; block < 100; ++block) {
            unsigned n = 0;
            for (unsigned i = 0; i < flatcpu::kSamplePeriod; ++i) n += schedule.next() ? 1u : 0u;
            if (n != 1) exactlyOne = false;
        }
        expect(exactlyOne, "the schedule samples exactly one frame in each block of 32");

        // Which frames are clocked: the render thread on one in each block of 16, every thread on one in each
        // block of 32, and every sampled frame is one of the render thread's clocked frames.
        flatcpu::ClockSchedule clock;
        bool oneRender = true, oneSampled = true, subset = true;
        unsigned landed[flatcpu::kRenderPeriod] = {};
        for (int block = 0; block < 200; ++block) {
            unsigned sampled = 0;
            for (int half = 0; half < 2; ++half) {
                unsigned renders = 0;
                for (unsigned i = 0; i < flatcpu::kRenderPeriod; ++i) {
                    const flatcpu::ClockSchedule::Frame f = clock.next();
                    renders += f.render ? 1u : 0u;
                    sampled += f.all ? 1u : 0u;
                    if (f.all && !f.render) subset = false;
                    if (f.render) ++landed[i];
                }
                if (renders != 1) oneRender = false;
            }
            if (sampled != 1) oneSampled = false;
        }
        expect(oneRender, "the render thread is clocked on exactly one frame in each block of 16");
        expect(oneSampled, "and every thread on exactly one frame in each block of 32");
        expect(subset, "a sampled frame is always one the render thread is clocked on");
        unsigned positions = 0;
        for (unsigned n : landed) positions += n ? 1u : 0u;
        expect(positions >= 12, "the position in a block is drawn at random, not a fixed stride: it lands on most of the 16");
        // A period of 1 is the census as it was: the render thread on every frame, one in 32 sampled.
        flatcpu::ClockSchedule every(1, flatcpu::kSamplePeriod);
        unsigned renders = 0, sampled = 0;
        for (unsigned i = 0; i < flatcpu::kSamplePeriod * 50; ++i) {
            const flatcpu::ClockSchedule::Frame f = every.next();
            renders += f.render ? 1u : 0u;
            sampled += f.all ? 1u : 0u;
        }
        expect(renders == flatcpu::kSamplePeriod * 50 && sampled == 50,
               "with a period of 1 the render thread is clocked on every frame and one in 32 is still sampled");
    }

    // ---- 13: a slow present shows in the tail, not the median ---------------------------------------------------------
    {
        flatcpu::resetForTest();
        fake(0);
        Run run;
        for (int i = 0; i < 300; ++i) run.frame(i % 10 == 0 ? 60000 : 16000, false, {});
        flatcpu::WindowReport r;
        // The window closes when 5 s have passed: this run is 30 x 60 ms + 270 x 16 ms = 6.1 s.
        const bool got = run.census->take(run.now, false, r);
        expect(got && nearly(r.presentP50Ms, 16.0) && nearly(r.presentP95Ms, 60.0),
               "a tenth of the frames at 60 ms leave the median at 16 ms and the p95 at 60");
    }

    // ---- 14: the instrument's own price is on the line ------------------------------------------------------------------
    {
        flatcpu::WindowReport r;
        r.valid = true;
        r.frames = 100;
        r.renderClockedFrames = 100;   // every frame clocked: what a clocked frame costs is what the frame cost
        r.floor.measured = true;
        r.floor.costNs = 50;
        r.floor.recordedNs = 20;
        r.emFloor.measured = true;
        r.emFloor.plainCostNs = 60;
        r.emFloor.pausedCostNs = 120;
        r.emFloor.plainRecordedNs = 24;
        r.emFloor.pausedRecordedNs = 44;
        r.renderCalls[flatcpu::kReduce] = 100 * 4000;   // 4000 scopes a frame of this instrument's
        r.emRenderCalls[emcpu::kEmit] = 100 * 1000;     // 1000 of a paused shape
        r.emRenderCalls[emcpu::kTee] = 100 * 500;       // 500 plain
        r.emRenderCalls[emcpu::kDraw] = 100 * 400;      // 400 of the draw side: clocked, though not folded into the total
        r.emRenderCalls[emcpu::kEval] = 100 * 999;      // counted and never clocked: not a scope
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        const std::string text = joined(lines);
        expect(has(text, "the clocks cost the render thread about 0.374 ms a frame on average (0.374 ms on a clocked frame, 5900 scopes; "
                         "the gate checks on the other frames are not counted) and put about 0.146 ms of the per-clocked-frame total above into it"),
               "the line prices its own clocks: scopes a clocked frame times the calibrated floor, the draw side included, the counted relays not");

        // One frame in five clocked: the counts are sums over 20 frames, so a clocked frame is the same 5,900
        // scopes and 0.374 ms, and 0.146 ms of the total is the clock reads; but only a fifth of the frames pay it,
        // so the run paid 0.075 ms a frame. That is the figure the line leads with.
        flatcpu::WindowReport sampled = r;
        sampled.renderClockedFrames = 20;
        sampled.renderCalls[flatcpu::kReduce] = 20 * 4000;
        sampled.emRenderCalls[emcpu::kEmit] = 20 * 1000;
        sampled.emRenderCalls[emcpu::kTee] = 20 * 500;
        sampled.emRenderCalls[emcpu::kDraw] = 20 * 400;
        sampled.emRenderCalls[emcpu::kEval] = 20 * 999;
        flatcpu::formatWindow(sampled, &lines);
        expect(has(joined(lines), "about 0.075 ms a frame on average (0.374 ms on a clocked frame, 5900 scopes; "
                                  "the gate checks on the other frames are not counted) and put about 0.146 ms of the per-clocked-frame total above into it"),
               "a sampled window reports the sampled cost: a clocked frame's price times the share of frames that were clocked");
        const flatcpu::InstrumentCost price = flatcpu::instrumentCost(sampled);
        expect(nearly(price.clockedMs, 0.374) && nearly(price.costMs, 0.0748) && nearly(price.recordedMs, 0.1456) && nearly(price.scopesPerFrame, 5900.0),
               "the price's parts: 0.374 ms and 5,900 scopes a clocked frame, 0.0748 ms a frame on average, 0.1456 ms recorded");

        flatcpu::WindowReport unmeasured;
        unmeasured.valid = true;
        unmeasured.frames = 100;
        unmeasured.renderClockedFrames = 100;
        unmeasured.renderCalls[flatcpu::kReduce] = 100 * 4000;
        flatcpu::formatWindow(unmeasured, &lines);
        expect(has(joined(lines), "about 0.000 ms a frame on average (0.000 ms on a clocked frame, 0 scopes;"), "with no floor measured it claims no price");
    }

    // ---- 15: on a frame the render thread is not clocked on, nothing reads the clock and nothing is counted ----------
    {
        flatcpu::resetForTest();
        fake(0);
        t_step = 1;   // every reading advances the clock a tick, so a reading can be counted
        Run run;
        const uint64_t tid = GetCurrentThreadId();
        unsigned unclocked = 0, clockedOnly = 0, sampled = 0;
        bool gatesAgree = true, gateWords = true;
        int64_t unclockedReads = 0, clockedReads = 0;
        for (int i = 0; i < 320; ++i) {
            run.frame(15625, false, [&] {
                const uint64_t gate = flatcpu::g_gate.load();
                gatesAgree = gatesAgree && gate == emcpu::g_gate.load();
                const int64_t before = t_reads;
                for (int s = 0; s < 40; ++s) { flatcpu::Scope one(flatcpu::kReduce); }
                for (int e = 0; e < 5; ++e) { emcpu::Scope one(emcpu::kTee); }
                const int64_t reads = t_reads - before;
                if (gate == flatcpu::kNoThread) { ++unclocked; unclockedReads += reads; }
                else if (gate == tid) { ++clockedOnly; clockedReads += reads; }
                else if (gate == (flatcpu::kSampledBit | tid)) { ++sampled; clockedReads += reads; }
                else gateWords = false;
            });
        }
        expect(gateWords, "the gate is one of three words: nobody, the render thread, everybody");
        expect(gatesAgree, "and engine motion's gate is the same word");
        expect(unclocked == 300 && clockedOnly == 10 && sampled == 10,
               "320 frames: 300 clock no one, 10 clock the render thread, 10 clock every thread (and the render thread with it)");
        expect(unclockedReads == 0, "a scope on a frame that clocks no one reads the clock not once, in this instrument or engine motion's");
        expect(clockedReads == 20 * (40 * 2 + 5 * 2), "a scope on a clocked frame reads it twice: 20 frames of 45 scopes");
        flatcpu::WindowReport r;
        const bool got = run.census->take(run.now, false, r);
        expect(got && r.renderClockedFrames == 20 && r.renderCalls[flatcpu::kReduce] == 20 * 40 && r.emRenderCalls[emcpu::kTee] == 20 * 5,
               "and only the clocked frames' scopes are in the window: 20 x 40 and 20 x 5, out of 320 frames' worth");
    }

    // ---- 16: a sampled window agrees with a fully clocked one on the same workload -------------------------------------
    {
        // 1,600 frames of about 3.4 ms make the 5 s window, each of an interval that looks random (so the
        // percentiles of a subset of the frames differ from the percentiles of all of them). Each frame does the
        // same kinds of work in amounts that vary with its number: the shell's own 30 to 54 ticks, one to five
        // contract reduction scopes of a constant 12 ticks, two witness writes of a constant 9, and a trace
        // scope of a constant 6.
        auto interval = [](int i) {
            uint32_t x = static_cast<uint32_t>(i) * 2654435761u;
            x ^= x >> 15;
            x *= 2246822519u;
            x ^= x >> 13;
            return static_cast<int64_t>(2800 + x % 1200);
        };
        auto workload = [](int i) {
            flatcpu::Scope shell(flatcpu::kOther);
            t_now += 30 + (i % 7) * 4;
            { flatcpu::Scope t(flatcpu::kTrace); t_now += 6; }
            for (int c = 0; c < 1 + i % 5; ++c) { flatcpu::Scope red(flatcpu::kReduce); t_now += 12; }
            for (int w = 0; w < 2; ++w) { flatcpu::Scope wt(flatcpu::kWitness); t_now += 9; }
        };
        auto window = [&](unsigned renderPeriod, flatcpu::WindowReport* out) {
            flatcpu::resetForTest();
            fake(0);
            Run run(renderPeriod);
            for (int i = 0; i < 1600; ++i) {
                run.frame(interval(i), false, [&workload, i] { workload(i); });
                run.census->noteWrapper(300, 20 + i % 3);
                run.census->noteGpuFrame(1.0 + (i % 4));
            }
            return run.census->take(run.now, false, *out);
        };
        flatcpu::WindowReport full, sampled;
        const bool gotFull = window(1, &full);
        const bool gotSampled = window(flatcpu::kRenderPeriod, &sampled);
        expect(gotFull && gotSampled && full.frames == 1600 && sampled.frames == 1600, "both windows close over the same 1,600 frames");
        expect(full.renderClockedFrames == 1600 && sampled.renderClockedFrames >= 99 && sampled.renderClockedFrames <= 101,
               "the fully clocked window clocks every frame, the sampled one about a sixteenth of them: 99 to 101");
        // A per-call cost is exact, sampled or not, wherever every call costs the same.
        for (unsigned f : {static_cast<unsigned>(flatcpu::kReduce), static_cast<unsigned>(flatcpu::kWitness), static_cast<unsigned>(flatcpu::kTrace)}) {
            expect(full.renderCalls[f] > 0 && sampled.renderCalls[f] > 0 &&
                       nearly(sampled.renderMs[f] / static_cast<double>(sampled.renderCalls[f]),
                              full.renderMs[f] / static_cast<double>(full.renderCalls[f]), 1e-12),
                   "a sampled window reports the same per-call cost as a fully clocked one");
        }
        // The shell's per-call cost varies with the frame; the sampled mean is within 10% of the full one.
        const double fullOtherPerCall = full.renderMs[flatcpu::kOther] / static_cast<double>(full.renderCalls[flatcpu::kOther]);
        const double sampledOtherPerCall = sampled.renderMs[flatcpu::kOther] / static_cast<double>(sampled.renderCalls[flatcpu::kOther]);
        expect(std::fabs(sampledOtherPerCall - fullOtherPerCall) <= 0.10 * fullOtherPerCall,
               "and where a call's cost varies from frame to frame, the sampled mean is within 10% of the full one");
        // A per-frame figure is a mean over the clocked frames. Constant per frame: exact. Varying: within 15%.
        const double kFull = static_cast<double>(full.renderClockedFrames), kSampled = static_cast<double>(sampled.renderClockedFrames);
        for (unsigned f : {static_cast<unsigned>(flatcpu::kWitness), static_cast<unsigned>(flatcpu::kTrace)}) {
            expect(nearly(sampled.renderMs[f] / kSampled, full.renderMs[f] / kFull, 1e-12) &&
                       nearly(static_cast<double>(sampled.renderCalls[f]) / kSampled, static_cast<double>(full.renderCalls[f]) / kFull, 1e-12),
                   "a per-frame figure that is the same every frame comes out exactly, from the clocked frames");
        }
        for (unsigned f : {static_cast<unsigned>(flatcpu::kOther), static_cast<unsigned>(flatcpu::kReduce)}) {
            const double a = full.renderMs[f] / kFull, b = sampled.renderMs[f] / kSampled;
            const double ca = static_cast<double>(full.renderCalls[f]) / kFull, cb = static_cast<double>(sampled.renderCalls[f]) / kSampled;
            expect(std::fabs(b - a) <= 0.15 * a && std::fabs(cb - ca) <= 0.15 * ca,
                   "and one that varies with the frame is within 15% of the fully clocked window's, in time and in calls");
        }
        // Everything that is not a clocked figure is the same to the digit: every frame's present, the counters, the GPU spans.
        expect(nearly(sampled.presentP50Ms, full.presentP50Ms) && nearly(sampled.presentP95Ms, full.presentP95Ms) && full.presentP95Ms > full.presentP50Ms,
               "present p50 and p95 come from every frame, not the clocked ones");
        expect(sampled.stateCalls == full.stateCalls && sampled.substitutedDraws == full.substitutedDraws,
               "the draw wrapper's counters are counted every frame");
        expect(sampled.gpuFrameSamples == full.gpuFrameSamples && nearly(sampled.gpuFrameP50, full.gpuFrameP50) && nearly(sampled.gpuFrameP95, full.gpuFrameP95),
               "and the GPU spans are unchanged");
        flatcpu::Lines fullLines, sampledLines;
        flatcpu::formatWindow(full, &fullLines);
        flatcpu::formatWindow(sampled, &sampledLines);
        const std::string fullText = joined(fullLines), sampledText = joined(sampledLines);
        expect(has(fullText, "render thread clocked on 1600 of 1600 frames, one in 1 "), "the fully clocked line says every frame was clocked");
        expect(has(sampledText, "render thread clocked on " + std::to_string(sampled.renderClockedFrames) + " of 1600 frames, one in 16 "),
               "the sampled line says how many of the frames the render thread was clocked on");
        expect(has(sampledText, "engine motion wrapper D3D calls 300/frame over 21.0 substituted draws/frame") &&
                   has(fullText, "engine motion wrapper D3D calls 300/frame over 21.0 substituted draws/frame"),
               "the wrapper's counts print per frame, over every frame, in both");
    }

    // ---- 17: the price the line leads with is the sampled one, on a clock that moves ------------------------------------
    {
        flatcpu::resetForTest();
        fake(0);
        t_step = 2;   // every reading advances the clock two ticks: a floor exists
        Run run;
        auto busy = [] {
            for (int s = 0; s < 200; ++s) { flatcpu::Scope one(flatcpu::kReduce); }
        };
        for (int i = 0; i < 320; ++i) run.frame(15625, false, busy);
        flatcpu::WindowReport first;
        const bool gotFirst = run.census->take(run.now, false, first);   // measures the floor, once
        for (int i = 0; i < 320; ++i) run.frame(15625, false, busy);
        flatcpu::WindowReport r;
        const bool got = run.census->take(run.now, false, r);
        expect(gotFirst && got && r.floor.measured && r.floor.costNs > 0, "a window on a moving clock carries a floor");
        const flatcpu::InstrumentCost price = flatcpu::instrumentCost(r);
        const double share = static_cast<double>(r.renderClockedFrames) / static_cast<double>(r.frames);
        expect(r.renderClockedFrames == 20 && price.scopesPerFrame == 200.0,
               "20 clocked frames of 320, 200 scopes on each");
        expect(price.clockedMs > 0 && nearly(price.costMs, price.clockedMs * share, 1e-12) && price.costMs < price.clockedMs / 10.0,
               "the run's price is a clocked frame's price times the share of frames clocked: a sixteenth, not the whole");
        char expected[160];
        std::snprintf(expected, sizeof(expected), "about %.3f ms a frame on average (%.3f ms on a clocked frame, 200 scopes;",
                      price.costMs, price.clockedMs);
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        expect(has(joined(lines), expected), "and it is the figure the line leads with, the clocked frame's price after it");
    }

    // ---- 18: a census built with its clocks off clocks no thread on any frame, and the line says so ---------------------
    {
        flatcpu::resetForTest();
        fake(0);
        Run run;
        run.census.reset(new flatcpu::Census(flatcpu::kRenderPeriod, false));
        Worker worker;
        for (int i = 0; i < 320; ++i) {
            run.frame(15625, false, [] { flatcpu::Scope one(flatcpu::kInject); t_now += 3; }, &worker,
                      [] { flatcpu::Scope one(flatcpu::kInject); t_now += 20; });
        }
        flatcpu::WindowReport r;
        const bool got = run.census->take(run.now, false, r);
        expect(got && r.frames == 320 && r.clocksOff, "the window still closes, and knows its clocks were off");
        expect(r.renderClockedFrames == 0 && r.sampledFrames == 0, "no frame is clocked on any thread");
        expect(r.renderCalls[flatcpu::kInject] == 0 && r.otherCalls[flatcpu::kInject] == 0, "and no call is counted");
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        expect(has(joined(lines), "render thread: clocks off (EDVR_FLAT_CPU_CLOCKS=1 turns them on)"), "the line says the clocks are off");
        expect(!r.floor.measured && !r.emFloor.measured, "the first window's close calibrates no floor: nothing clocked needs one");
        expect(!has(joined(lines), "one in ") && !has(joined(lines), "one frame in 16") && !has(joined(lines), "one frame in 32"),
               "and the line claims no clocking schedule");
        expect(has(joined(lines), "other threads: clocks off"), "the other threads' part says so too");
        // The policy: on under Windows, off under Wine, the variable deciding it either way.
        expect(flatcpu::clocksPolicy(false, nullptr) && !flatcpu::clocksPolicy(true, nullptr), "on under Windows, off under Wine");
        expect(flatcpu::clocksPolicy(true, "1") && !flatcpu::clocksPolicy(false, "0"), "EDVR_FLAT_CPU_CLOCKS=1/0 overrides both");
        expect(flatcpu::clocksPolicy(false, "x") && !flatcpu::clocksPolicy(true, "x"), "any other value leaves the default");
    }

    fake(0);
    t_fake = false;
    return failures;
}
