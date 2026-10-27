// aa_off_test -- the AA-off draw path and the cheap instruments it rides on (2026-10-13).
//
//   Cut B: draw_fast_path.h's decisions, the same functions vscreen.cpp's draw thunks and bind hooks call. A model
//          of a run of draws (a claimed draw, a gate-open draw, an unclaimed one) counts the verdict scopes, the
//          engine velocity calls and the shadow records the thunks would make. The model is a transcription of the
//          thunk's branches; the real thunks cannot run without a device, so the rig pins the decisions they take.
//   Cut C: map_wait.h's call counts (exact) and sampled ticks (scaled at take), and map_wait_sample.h's sampler.
//   Cut D: gpu_frame_timing.h's gpuFrameCommandIfLive, with a counting stub for the out-of-line gpuFrameCommand:
//          with the instrument off, no call is made; on, every call is made.
//   Cut E: draw_step_clock.h (built 2026-10-27, the temporary draw hook CPU by step instrument), driven with a scripted
//          clock: which draws are clocked how, what a mark attributes, the game's draw against reissues, the clock floor,
//          the window's totals and scaling, and the two log lines' text and worst-case length.
//   Cut F: classifier_memo.h (built 2026-10-27, the ui_depth eye-draw memo and its temporary verification): when a call hits, and
//          why one misses (counted by cause); the verify frame's cadence; the tally of verified hits and mismatches per output; and
//          the "ui depth memo:" line's text, with its "-" cases and its worst-case length.
//
// Exit codes: 0 pass, 1 a check failed, 2 usage. --dry-run touches nothing.
#include "../../src/common/gpu_disjoint_clock.h"
#include "../../src/d3d11/classifier_memo.h"
#include "../../src/d3d11/draw_fast_path.h"
#include "../../src/d3d11/draw_step_clock.h"
#include "../../src/d3d11/frame_span_tile.h"
#include "../../src/d3d11/gpu_frame_timing.h"
#include "../../src/d3d11/map_wait.h"

// The frame span holds at most kFrameSpanSlots pairs of two timestamps each (gpu_census.cpp); every one must fit the
// clock's per-record lease budget (kLeases, 512), or a pair would be refused rather than held.
static_assert(edvr::kFrameSpanSlots * 2u <= edvr::DisjointClock::kLeases,
              "the frame span's timestamps fit the disjoint clock's lease budget");

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace edvr {

// The stub of gpu_frame_timing.cpp's out-of-line call, counting what the thunks would have made.
static unsigned g_gpuCommandCalls = 0;
void gpuFrameCommand(ID3D11DeviceContext*) noexcept { ++g_gpuCommandCalls; }

namespace detail {
thread_local bool g_gpuFrameInternal = false;
std::atomic<bool> g_gpuFrameCommandLive{false};
std::atomic<bool> g_frameWorkArmed{false};
}  // namespace detail

// The stub of gpu_census.cpp's first-command hook (2026-10-14), counting the calls the armed branch makes.
static unsigned g_frameWorkCalls = 0;
void frameWorkFirstCommand(ID3D11DeviceContext*) noexcept { ++g_frameWorkCalls; }

}  // namespace edvr

namespace {

int g_failed = 0, g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

// Cut B: the fast-path decision over all 32 input combinations.
void testFastPathTable() {
    unsigned taken = 0, mismatch = 0;
    for (unsigned bits = 0; bits < 32; ++bits) {
        const edvr::DrawPathInputs in{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0, (bits & 16) != 0};
        const bool expect = !in.claimed && in.aaOff && !in.gateWanted && !in.engineVelocityLive && !in.vrWorld;
        const bool got = edvr::drawFastPathTaken(in);
        taken += got;
        mismatch += got != expect;
    }
    check(mismatch == 0, "fast path: the decision matches its rule for all 32 input combinations");
    check(taken == 1, "fast path: exactly one combination takes it (AA off, no claim, no gate, no engine velocity, no world route)");
    // Each non-AA-off or claimed or gate-open or EV-live or world-route draw is kept on the full path.
    const edvr::DrawPathInputs base{true, false, false, false, false};
    check(edvr::drawFastPathTaken(base), "fast path: the unclaimed AA-off draw is taken");
    edvr::DrawPathInputs c = base; c.claimed = true;
    check(!edvr::drawFastPathTaken(c), "fast path: a fix-claimed draw keeps the full path");
    c = base; c.gateWanted = true;
    check(!edvr::drawFastPathTaken(c), "fast path: an open subscriber gate keeps the full path");
    c = base; c.engineVelocityLive = true;
    check(!edvr::drawFastPathTaken(c), "fast path: live engine velocity (a diagnostic) keeps the full path");
    c = base; c.vrWorld = true;
    check(!edvr::drawFastPathTaken(c), "fast path: the VR world route keeps the full path");
    c = base; c.aaOff = false;
    check(!edvr::drawFastPathTaken(c), "fast path: AA on never takes it (behaviour with AA on is unchanged)");
}

// Cut B: the bind records. Only AA off with engine velocity closed skips VsSrv33 and Blend.
void testBindShadows() {
    check(edvr::bindShadowsSkipped(true, false), "bind shadows: skipped with AA off and engine velocity closed");
    check(!edvr::bindShadowsSkipped(true, true), "bind shadows: recorded while engine velocity is live");
    check(!edvr::bindShadowsSkipped(false, false), "bind shadows: recorded with AA on");
    check(!edvr::bindShadowsSkipped(false, true), "bind shadows: recorded with AA on and engine velocity live");
}

// Cut B: a model of the thunks over a run of draws. The model is the branch structure of hookedDraw / hookedDrawIndexed
// / hookedDrawInstanced: the verdict decides the scope; the fast path decides the scope and engine velocity; an
// unclaimed owner draw calls engine velocity on the full path only.
struct DrawTally {
    unsigned scopes = 0;          // forwardWithVerdict scopes entered
    unsigned engineVelocity = 0;  // engineVelocityBeforeDraw calls
    unsigned fastDraws = 0;       // draws issued directly (fast path)
    unsigned realDraws = 0;       // game draws issued, either way
};

DrawTally runDraws(bool aaOff, bool gateWanted, unsigned draws, unsigned claimEvery) {
    DrawTally t;
    for (unsigned i = 0; i < draws; ++i) {
        const bool claimed = claimEvery && (i % claimEvery) == 0;
        const edvr::DrawPathInputs in{aaOff, gateWanted, false, false, claimed};
        const bool fast = edvr::drawFastPathTaken(in);
        if (!fast && !claimed) ++t.engineVelocity;   // the full path's engineVelocityBeforeDraw on an unclaimed owner draw
        if (fast) {
            ++t.fastDraws;
        } else {
            ++t.scopes;
        }
        ++t.realDraws;
    }
    return t;
}

void testThunkModel() {
    // AA off, gate closed, no claims: every draw is fast, no scope, no engine velocity call.
    DrawTally a = runDraws(true, false, 1000, 0);
    check(a.fastDraws == 1000 && a.scopes == 0 && a.engineVelocity == 0 && a.realDraws == 1000,
          "thunk model: AA off, no subscriber, no claims: all 1000 draws skip the scope and engine velocity");
    // AA off, every 10th draw claimed: the claimed draws keep the full path, the rest are fast.
    DrawTally b = runDraws(true, false, 1000, 10);
    check(b.scopes == 100 && b.fastDraws == 900, "thunk model: every claimed draw (100 of 1000) keeps the full path");
    check(b.engineVelocity == 0, "thunk model: no unclaimed draw calls engine velocity while AA is off");
    // AA off with an open gate (a subscriber on): nothing is fast, every draw keeps the full path.
    DrawTally c = runDraws(true, true, 1000, 0);
    check(c.fastDraws == 0 && c.scopes == 1000, "thunk model: an open subscriber gate keeps every draw on the full path");
    // AA on: nothing is fast, whatever the gate and the claims.
    DrawTally d = runDraws(false, false, 1000, 10);
    check(d.fastDraws == 0 && d.scopes == 1000 && d.engineVelocity == 900,
          "thunk model: AA on keeps the full path (unclaimed draws still call engine velocity, as before)");
}

// Cut B: the bind-side model across an AA toggle (on, then off, then on). Recorded binds only when the records are
// wanted; the frame boundary invalidates skipped slots once they are wanted again.
void testBindToggle() {
    unsigned recorded = 0, skipped = 0;
    bool skippedFlag = false;
    // The sequence is AA on, on, off, off, off, on, on: aaOff is the negation.
    const bool aaOffSequence[] = {false, false, true, true, true, false, false};
    for (bool aaOff : aaOffSequence) {
        for (unsigned bind = 0; bind < 10; ++bind) {
            if (edvr::bindShadowsSkipped(aaOff, false)) {
                ++skipped;
                skippedFlag = true;
            } else {
                ++recorded;
            }
        }
        // The frame boundary: skipped slots are invalidated once the records are wanted again.
        if (skippedFlag && !edvr::bindShadowsSkipped(aaOff, false)) skippedFlag = false;
    }
    check(recorded == 40 && skipped == 30, "bind toggle: 40 binds recorded, 30 skipped across on-on-off-off-off-on-on");
    check(!skippedFlag, "bind toggle: back on, the skipped flag is cleared by the frame boundary");
}

// Cut C: the sampler and the scaling.
void testSampler() {
    uint32_t seq = 0;
    unsigned sampled = 0;
    unsigned firstAt = 0;
    for (unsigned i = 1; i <= 1600; ++i) {
        if (edvr::mapWaitSampleNext(seq)) {
            if (!sampled) firstAt = i;
            ++sampled;
        }
    }
    check(sampled == 100, "sampler: 1 in 16 -- 100 of 1600 calls are sampled");
    check(firstAt == 16, "sampler: the first sampled call is the 16th");
    check(edvr::mapWaitScaleSampled(3) == 48, "sampler: a sampled total scales by 16");
    check(edvr::mapWaitScaleSampled(0) == 0, "sampler: an empty total scales to zero");
}

// Cut C: the Map totals through the real map_wait.cpp: calls exact, ticks scaled by the rate at take.
void testMapTotals() {
    edvr::mapWaitTake();   // zero the totals
    for (int i = 0; i < 1600; ++i) edvr::mapWaitCall(D3D11_MAP_WRITE_DISCARD);
    for (int i = 0; i < 100; ++i) edvr::mapWaitCall(D3D11_MAP_READ);
    edvr::mapWaitNote(D3D11_MAP_READ, 1000);     // one sampled read of 1000 ticks
    edvr::mapWaitNote(D3D11_MAP_WRITE, 500);     // one sampled write of 500 ticks
    const edvr::MapWaitTotals t = edvr::mapWaitTake();
    check(t.writeCalls == 1600 && t.readCalls == 100, "map totals: call counts are exact (1600 writes, 100 reads)");
    check(t.readTicks == 16000 && t.writeTicks == 8000, "map totals: the sampled ticks are scaled by 16 at take");
    check(t.longestTicks == 1000, "map totals: the longest call is a measured one, not scaled");
    const edvr::MapWaitTotals again = edvr::mapWaitTake();
    check(again.readCalls == 0 && again.writeCalls == 0 && again.readTicks == 0,
          "map totals: taking zeroes the window");
}

// Cut D: the inline guard. With the instrument off, the thunks make no call; on, every call is made.
void testGpuGuard() {
    using namespace edvr;
    // The work span's arm (2026-10-14): one more relaxed load in the same gate. Armed, every command reaches the first-command
    // hook; disarmed, none does, and the frame's first command is the one that begins the span.
    detail::g_frameWorkArmed.store(false, std::memory_order_relaxed);
    g_frameWorkCalls = 0;
    for (int i = 0; i < 100; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_frameWorkCalls == 0, "work span: disarmed, no command reaches the first-command hook");
    detail::g_frameWorkArmed.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 100; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_frameWorkCalls == 100, "work span: armed, every command reaches the first-command hook");
    detail::g_frameWorkArmed.store(false, std::memory_order_relaxed);
    detail::g_gpuFrameCommandLive.store(false, std::memory_order_relaxed);
    g_gpuCommandCalls = 0;
    for (int i = 0; i < 1000; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_gpuCommandCalls == 0, "gpu guard: with app_gpu_timing off, 1000 thunk calls make no gpuFrameCommand call");
    detail::g_gpuFrameCommandLive.store(true, std::memory_order_relaxed);
    g_gpuCommandCalls = 0;
    for (int i = 0; i < 1000; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_gpuCommandCalls == 1000, "gpu guard: with app_gpu_timing on, every thunk call is made");
    detail::g_gpuFrameInternal = true;   // inside EDVR's own GPU timing call: never re-entered
    g_gpuCommandCalls = 0;
    gpuFrameCommandIfLive(nullptr);
    check(g_gpuCommandCalls == 0, "gpu guard: the reentrancy flag suppresses the call");
    detail::g_gpuFrameInternal = false;
    detail::g_gpuFrameCommandLive.store(false, std::memory_order_relaxed);
}

// The overlay's GPU TIME tile (2026-10-14): the application mean when it has samples; with the application clock off, the
// frame span when a pair has completed; "--" with its reason before either.
void testGpuTimeTile() {
    char v[16], sub[160];
    bool shown = edvr::gpuTimeTileText(true, 12.34, false, 0.0, "reason", v, sizeof(v), sub, sizeof(sub));
    check(shown && std::strcmp(v, "12.3") == 0 && std::strstr(sub, "application render") != nullptr,
          "tile: with app_gpu_timing on and samples, the application mean is shown");
    // Application clock off (no app samples) and one frame span completed: a number, labelled as the span.
    shown = edvr::gpuTimeTileText(false, 0.0, true, 4.26, "reason", v, sizeof(v), sub, sizeof(sub));
    check(shown && std::strcmp(v, "4.3") == 0, "tile: app clock off with a completed frame span shows that number");
    check(std::strstr(sub, "frame GPU work span (first command to last; includes GPU idle inside the frame)") != nullptr,
          "tile: the work span carries the required label (first command to last; includes GPU idle inside the frame)");
    // Nothing completed yet: "--" with the reason, never a number.
    shown = edvr::gpuTimeTileText(false, 0.0, false, 0.0, "frame GPU span: no sample yet", v, sizeof(v), sub, sizeof(sub));
    check(!shown && std::strcmp(v, "--") == 0 && std::strcmp(sub, "frame GPU span: no sample yet") == 0,
          "tile: '--' appears only when no sample has completed yet, with its reason");
    // A tiny buffer truncates, never overruns.
    char tiny[2];
    edvr::gpuTimeTileText(false, 0.0, true, 123.4, "", tiny, sizeof(tiny), sub, sizeof(sub));
    check(std::strlen(tiny) < sizeof(tiny), "tile: a short value buffer is truncated, not overrun");
}

// The work span against the frame period (2026-10-14), in a simulated slack frame: 90 Hz, the GPU idle until the game's first command,
// then busy briefly, then idle until the next boundary. The work span is first command to door end; the boundary-to-boundary figure
// is the frame period. The arithmetic only (frameWorkSpanMs); the GPU's own timestamps are not simulated here.
void testFrameWorkSlack() {
    const double period = 1000.0 / 90.0;   // 11.11 ms at 90 Hz
    // One slack frame: the first command at 6.0 ms into the frame, the door end at 8.5 ms. The span is the work, not the period.
    const double slack = edvr::frameWorkSpanMs(6.0, 8.5);
    check(slack > 0.0 && slack < period, "work span: a slack frame's span is less than the frame period (11.1 ms at 90 Hz)");
    check(std::fabs(slack - 2.5) < 1e-9, "work span: the span is the door end minus the first command");
    // A sequence of slack frames: every span stays under the period, and the boundary-to-boundary figure would be the period each time.
    bool allUnder = true;
    for (int i = 0; i < 100; ++i) {
        const double first = 4.0 + (i % 5) * 0.5;      // the GPU starts the frame's work between 4.0 and 6.0 ms
        const double end = first + 2.0 + (i % 3) * 0.7; // and works for 2.0 to 3.4 ms
        const double span = edvr::frameWorkSpanMs(first, end);
        allUnder = allUnder && span > 0.0 && span < period;
    }
    check(allUnder, "work span: every frame of a 100-frame slack run is under the frame period");
    check(edvr::frameWorkSpanMs(8.0, 7.0) == 0.0, "work span: an out-of-order pair clamps to zero, not a negative frame");
}

// The in-headset fps overlay's line (2026-10-14): it chose its GPU figure on its own and only the tile learned the work span, so a flight with
// the application clock off read "gpu -- ms". Now both call chooseGpuTime; this runs the choice and the line the overlay prints.
void testOverlayLine() {
    char line[120];
    using edvr::GpuTimeSource;
    // Clock off, a span in the series: a number, from the work span.
    edvr::GpuTimeChoice c = edvr::chooseGpuTime(false, false, true, 4.26, true, 3.0);
    check(c.source == GpuTimeSource::WorkSpan && std::fabs(c.ms - 4.26) < 1e-9, "overlay: clock off with the series holding spans picks the work span mean");
    edvr::overlayLineText(line, sizeof(line), 89.6, c, "cpu", true, 2.34);
    check(std::strcmp(line, "90 fps   gpu 4.3 ms   cpu 2.3 ms") == 0, "overlay: clock off with a span prints a number in the line");
    // Clock off, the series aged out but a pair completed: the last pair.
    c = edvr::chooseGpuTime(false, false, false, 0.0, true, 5.04);
    check(c.source == GpuTimeSource::WorkSpan && std::fabs(c.ms - 5.04) < 1e-9, "overlay: clock off, series empty, a completed pair: the last pair");
    // Clock off, no span anywhere: "--", never a number.
    c = edvr::chooseGpuTime(false, false, false, 0.0, false, 0.0);
    edvr::overlayLineText(line, sizeof(line), 90.0, c, "cpu", false, 0.0);
    check(c.source == GpuTimeSource::None && std::strcmp(line, "90 fps   gpu -- ms   cpu -- ms") == 0,
          "overlay: clock off with no span prints '--'");
    // Clock on keeps today's behaviour: the application mean when ready and sampled, else '--' (the work span is never read).
    c = edvr::chooseGpuTime(true, true, true, 12.34, true, 4.0);
    check(c.source == GpuTimeSource::ApplicationRender && std::fabs(c.ms - 12.34) < 1e-9, "overlay: clock on and ready shows the application render mean");
    c = edvr::chooseGpuTime(true, false, true, 12.34, true, 4.0);
    check(c.source == GpuTimeSource::None, "overlay: clock on but the snapshot not ready is '--', not the span");
    c = edvr::chooseGpuTime(true, true, false, 0.0, true, 4.0);
    check(c.source == GpuTimeSource::None, "overlay: clock on with no application samples is '--', not the span");
    edvr::overlayLineText(line, sizeof(line), 72.0, edvr::chooseGpuTime(true, true, true, 9.96, false, 0.0), "cpu (pre-submit)", true, 6.0);
    check(std::strcmp(line, "72 fps   gpu 10.0 ms   cpu (pre-submit) 6.0 ms") == 0, "overlay: clock on prints the application mean and the stand-in label");
    // A short buffer truncates, never overruns.
    char tiny[8];
    edvr::overlayLineText(tiny, sizeof(tiny), 90.0, c, "cpu", true, 1.0);
    check(std::strlen(tiny) < sizeof(tiny), "overlay: a short line buffer is truncated, not overrun");
    // The census field the log parser reads (tools\edvr_log.py --gpu-work-span): its exact text.
    char field[160];
    edvr::formatFrameWorkSpanField(field, sizeof(field), 0, 0, 0, 0.0);
    check(std::strcmp(field, "frame work span: completed 0, skipped 0, invalid 0, last - ms") == 0, "census field: nothing completed prints 'last -'");
    edvr::formatFrameWorkSpanField(field, sizeof(field), 5400, 3, 2, 4.2567);
    check(std::strcmp(field, "frame work span: completed 5400, skipped 3, invalid 2, last 4.257 ms") == 0, "census field: counts and the last pair in ms");
}

// ---- Cut E: the draw step clock (draw_step_clock.h) -------------------------------------------------------------------
// The clock's environment, scripted: it reads whatever time the test last set, counts the reads, and keeps what the sinks are given.
struct FakeEnv {
    static constexpr uint32_t kStride = 64;
    static inline bool g_sampled = false;
    static inline uint32_t g_ordinal = 0;
    static inline int64_t g_time = 0;
    static inline unsigned g_reads = 0;
    static inline edvr::drawstep::Run g_run;
    static inline edvr::drawstep::Run* g_sub = nullptr;
    static inline unsigned g_wholeReports = 0;
    static inline int64_t g_wholeTicks = 0, g_realTicks = 0;
    static inline unsigned g_stepReports = 0;
    static inline edvr::drawstep::StepAccum g_lastRun;
    static inline edvr::drawstep::Window g_window;

    static bool sampled() { return g_sampled; }
    static uint32_t nextOrdinal() { return ++g_ordinal; }
    static int64_t now() {
        ++g_reads;
        return g_time;
    }
    static edvr::drawstep::Run& run() { return g_run; }
    static edvr::drawstep::Run*& sub() { return g_sub; }
    static inline int g_live = 0;
    static void stepsLive(int delta) { g_live += delta; }
    static void reportWhole(int64_t whole, int64_t real) {
        ++g_wholeReports;
        g_wholeTicks = whole;
        g_realTicks = real;
    }
    static void reportSteps(char kind, const edvr::drawstep::StepAccum& a) {
        ++g_stepReports;
        g_lastRun = a;
        g_window.add(kind, a);
    }
    static void reset() {
        g_sampled = false;
        g_ordinal = 0;
        g_time = 0;
        g_reads = 0;
        g_run = edvr::drawstep::Run();
        g_sub = nullptr;
        g_live = 0;
        g_wholeReports = 0;
        g_wholeTicks = g_realTicks = 0;
        g_stepReports = 0;
        g_lastRun = edvr::drawstep::StepAccum();
        g_window.reset();
    }
};
using TestClock = edvr::drawstep::DrawClockT<FakeEnv>;
using edvr::drawstep::Mode;

// The sub-marks' route, as vscreen.cpp's stepMark takes it: the live count, then the thread's pointer, a scripted clock, the same
// markRun the draw uses.
struct FakeNow {
    int64_t operator()() const { return FakeEnv::g_time; }
};
void subMark(edvr::drawstep::Step step) {
    if (FakeEnv::g_live == 0) return;
    if (edvr::drawstep::Run* r = FakeEnv::g_sub) edvr::drawstep::markRun(*r, step, FakeNow{});
}

bool closeTo(double a, double b) { return std::fabs(a - b) < 1e-6; }

// Which draws are clocked how: the whole clock's ordinal % 64 == 0 is untouched, the steps mode takes % 64 == 32, the rest are off, and
// only a sampled frame's draws are counted at all.
void testStepClockModes() {
    using namespace edvr::drawstep;
    unsigned whole = 0, steps = 0, off = 0;
    bool tableRight = true;
    for (uint32_t o = 0; o < 256; ++o) {
        const Mode m = modeFor(o, 64);
        const Mode want = (o % 64) == 0 ? Mode::kWhole : ((o % 64) == 32 ? Mode::kSteps : Mode::kOff);
        tableRight = tableRight && m == want;
        whole += m == Mode::kWhole;
        steps += m == Mode::kSteps;
        off += m == Mode::kOff;
    }
    check(tableRight, "step clock modes: ordinal 0 mod 64 is whole, 32 mod 64 is steps, every other ordinal is off");
    check(whole == 4 && steps == 4 && off == 248, "step clock modes: of 256 ordinals, 4 are whole, 4 are steps and 248 are off");
    check(modeFor(0, 64) == Mode::kWhole && modeFor(32, 64) == Mode::kSteps && modeFor(31, 64) == Mode::kOff &&
              modeFor(33, 64) == Mode::kOff && modeFor(96, 64) == Mode::kSteps,
          "step clock modes: ordinals 0 and 64 are whole, 32 and 96 are steps, 31 and 33 are off");
    check(modeFor(5, 0) == Mode::kOff && modeFor(5, 1) == Mode::kOff, "step clock modes: a stride under 2 clocks nothing");

    // Through the clock itself. An unsampled frame: no ordinal advance, no clock read, no mode.
    FakeEnv::reset();
    FakeEnv::g_sampled = false;
    bool allOff = true;
    for (int i = 0; i < 200; ++i) {
        TestClock c('D');
        allOff = allOff && c.mode() == Mode::kOff && !c.on;
    }
    check(allOff && FakeEnv::g_ordinal == 0 && FakeEnv::g_reads == 0,
          "step clock modes: on an unsampled frame every draw is off, the ordinal does not advance and no clock is read");
    // A sampled frame: the draw the old clock took (the 64th) is whole, the 32nd and 96th are steps, the rest are off.
    FakeEnv::reset();
    FakeEnv::g_sampled = true;
    bool shape = true;
    for (uint32_t i = 1; i <= 128; ++i) {
        TestClock c('D');
        const Mode want = (i % 64) == 0 ? Mode::kWhole : ((i % 64) == 32 ? Mode::kSteps : Mode::kOff);
        shape = shape && c.mode() == want && c.on == (want == Mode::kWhole);
    }
    check(shape && FakeEnv::g_ordinal == 128, "step clock modes: on a sampled frame the 32nd, 96th are steps, the 64th, 128th are whole, the rest off");
    check(FakeEnv::g_wholeReports == 2 && FakeEnv::g_stepReports == 2,
          "step clock modes: 128 sampled draws report two whole and two steps clocks");
}

// Whole and off modes record nothing through marks: the whole clock reads its clock where it always did, and the marks add no reads.
void testStepClockOtherModes() {
    using namespace edvr::drawstep;
    FakeEnv::reset();
    FakeEnv::g_sampled = true;
    FakeEnv::g_ordinal = 63;   // the next draw is the 64th: whole
    FakeEnv::g_time = 1000;
    {
        TestClock c('X');
        check(c.mode() == Mode::kWhole && c.on, "whole mode: the 64th draw of a sampled frame is clocked whole");
        FakeEnv::g_time = 1100;
        c.mark(kPre);
        c.mark(kVRest);
        c.markForward();
        const int64_t r0 = c.on ? FakeEnv::now() : 0;   // the thunk's own read
        FakeEnv::g_time = 1160;
        c.stepDraw();
        c.realCall(r0);
        FakeEnv::g_time = 1300;
        c.realCall(1160);   // a reissue's call: not the game's, not counted
        c.mark(kTail);
        FakeEnv::g_time = 1400;
    }
    check(FakeEnv::g_wholeReports == 1 && FakeEnv::g_wholeTicks == 400 && FakeEnv::g_realTicks == 60,
          "whole mode: reports the whole draw (400) and the first real call (60), as the old clock did");
    check(FakeEnv::g_stepReports == 0 && FakeEnv::g_sub == nullptr && FakeEnv::g_window.clocked == 0 && FakeEnv::g_live == 0,
          "whole mode: marks record nothing and no steps run is parked, counted live or reported");
    check(FakeEnv::g_reads == 4, "whole mode: the clock is read four times (start, the thunk's r0, the first real call, the end), marks add none");
    // An off draw: nothing read, nothing reported.
    FakeEnv::reset();
    FakeEnv::g_sampled = true;
    {
        TestClock c('I');
        check(c.mode() == Mode::kOff, "off mode: the first draw of a sampled frame is not clocked");
        c.mark(kPre);
        c.markForward();
        c.stepDraw();
        c.mark(kTail);
    }
    check(FakeEnv::g_reads == 0 && FakeEnv::g_wholeReports == 0 && FakeEnv::g_stepReports == 0 && FakeEnv::g_sub == nullptr,
          "off mode: marks read no clock and report nothing");
}

// Marks attribute the interval to the step they close, in the thunks' order, with the sub-marks through the thread's pointer.
void testStepClockMarks() {
    using namespace edvr::drawstep;
    FakeEnv::reset();
    FakeEnv::g_sampled = true;
    FakeEnv::g_ordinal = 31;   // the next is the 32nd: steps
    FakeEnv::g_time = 1000;
    {
        TestClock c('X');
        check(c.mode() == Mode::kSteps && !c.on, "step marks: the 32nd draw of a sampled frame is clocked in steps mode, not whole");
        check(FakeEnv::g_sub == &FakeEnv::g_run, "step marks: a live steps clock parks its run as the thread's sub-mark target");
        check(FakeEnv::g_live == 1, "step marks: a live steps clock counts itself in the process-wide count the sub-marks test first");
        FakeEnv::g_time = 1010;
        c.mark(kPre);                        // pre 10
        FakeEnv::g_time = 1025;
        subMark(kVHudFss);                   // 15, inside beginPanelOverride
        FakeEnv::g_time = 1030;
        subMark(kVCensus);                   // 5
        FakeEnv::g_time = 1050;
        c.mark(kVRest);                      // 20: the rest of the verdict call, after its sub-marks
        FakeEnv::g_time = 1055;
        c.mark(kEngineVelocity);             // 5
        FakeEnv::g_time = 1058;
        c.mark(kProbes);                     // 3
        FakeEnv::g_time = 1068;
        subMark(kFwScopes);                  // 10, inside forwardWithVerdict
        FakeEnv::g_time = 1100;
        c.markForward();                     // 32: the lambda's entry before the first real draw
        FakeEnv::g_time = 1300;
        c.stepDraw();                        // 200: the game's own draw
        FakeEnv::g_time = 1340;
        c.mark(kWeapon);                     // 40
        FakeEnv::g_time = 1344;
        c.mark(kPostRest);                   // 4
        FakeEnv::g_time = 1350;
        c.mark(kPostRigid);                  // 6
        FakeEnv::g_time = 1360;
        c.mark(kPostScreen);                 // 10
        FakeEnv::g_time = 1375;
        subMark(kFwLayerEnd);                // 15, after the draw
        FakeEnv::g_time = 1380;
        c.markForward();                     // 5: forwardWithVerdict's return, after the draw
        FakeEnv::g_time = 1400;
    }                                        // tail 20
    const StepAccum& a = FakeEnv::g_lastRun;
    check(FakeEnv::g_stepReports == 1 && FakeEnv::g_window.clocked == 1 && FakeEnv::g_window.kindDraws[3] == 1,
          "step marks: the run is reported once, as an X draw");
    check(a.ticks[kPre] == 10 && a.ticks[kVHudFss] == 15 && a.ticks[kVCensus] == 5 && a.ticks[kVRest] == 20,
          "step marks: each interval goes to the step that ends it (pre, two sub-steps, the verdict's rest)");
    check(a.ticks[kEngineVelocity] == 5 && a.ticks[kProbes] == 3 && a.ticks[kFwScopes] == 10 && a.ticks[kFwBefore] == 32,
          "step marks: engine velocity, probes, the forward scope's sub-step and its lambda entry are cut apart");
    check(a.ticks[kGameDraw] == 200 && a.ticks[kWeapon] == 40 && a.ticks[kReissue] == 0,
          "step marks: the game's draw (200) and the weapon motion (40) are separate, and nothing is a reissue");
    check(a.ticks[kPostRest] == 4 && a.ticks[kPostRigid] == 6 && a.ticks[kPostScreen] == 10 && a.ticks[kFwLayerEnd] == 15 &&
              a.ticks[kFwAfter] == 5 && a.ticks[kTail] == 20,
          "step marks: the post-draw steps, the forward scope's after-draw sub-step and return, and the tail are cut apart");
    int64_t total = 0;
    unsigned marked = 0;
    for (unsigned i = 0; i < kStepCount; ++i) {
        total += a.ticks[i];
        marked += a.marks[i];
    }
    check(total == 400 && marked == 16, "step marks: the steps' ticks sum to the whole draw (400) and each mark was counted once (16)");
    check(a.marks[kGameDraw] == 1 && a.marks[kVHudFss] == 1 && a.marks[kFwUiDepth] == 0,
          "step marks: a step that was never marked has no marks, so the report prints it '-'");
    check(FakeEnv::g_sub == nullptr, "step marks: the thread's sub-mark target is put back when the clock ends");
    check(FakeEnv::g_live == 0, "step marks: ...and the clock leaves the live count as it ends");
    // A sub-mark with no steps clock alive finds null and does nothing.
    const int64_t before = FakeEnv::g_run.acc.ticks[kVCensus];
    FakeEnv::g_time = 5000;
    subMark(kVCensus);
    check(before == 5 && FakeEnv::g_run.acc.ticks[kVCensus] == before && FakeEnv::g_run.acc.marks[kVCensus] == 1,
          "step marks: a sub-mark outside a steps clock adds nothing");
    // The previous sub-mark target is restored, not cleared.
    FakeEnv::reset();
    FakeEnv::g_sampled = true;
    FakeEnv::g_ordinal = 31;
    Run outer;
    FakeEnv::g_sub = &outer;
    {
        TestClock c('D');
        check(FakeEnv::g_sub == &FakeEnv::g_run && FakeEnv::g_run.outer == &outer, "step marks: a clock started under another run remembers it");
    }
    check(FakeEnv::g_sub == &outer, "step marks: ...and puts it back at its end");
    FakeEnv::g_sub = nullptr;   // `outer` is local
}

// The AA-off fast path's draw (D, I, N): pre, verdict, probes, the game's draw, tail. No forward scope, no engine velocity.
void testStepClockFastPath() {
    using namespace edvr::drawstep;
    FakeEnv::reset();
    FakeEnv::g_sampled = true;
    FakeEnv::g_ordinal = 31;
    FakeEnv::g_time = 100;
    {
        TestClock c('N');
        FakeEnv::g_time = 110;
        c.mark(kPre);
        FakeEnv::g_time = 150;
        subMark(kVContext);
        FakeEnv::g_time = 160;
        c.mark(kVRest);
        FakeEnv::g_time = 165;
        c.mark(kProbes);
        FakeEnv::g_time = 265;
        c.stepDraw();
        FakeEnv::g_time = 270;
    }
    const StepAccum& a = FakeEnv::g_lastRun;
    check(a.ticks[kPre] == 10 && a.ticks[kVContext] == 40 && a.ticks[kVRest] == 10 && a.ticks[kProbes] == 5 && a.ticks[kGameDraw] == 100 &&
              a.ticks[kTail] == 5,
          "fast path: a draw goes pre, verdict (sub-step and rest), probes, the game's draw, tail");
    check(a.marks[kFwBefore] == 0 && a.marks[kFwAfter] == 0 && a.marks[kEngineVelocity] == 0 && a.marks[kReissue] == 0,
          "fast path: no forward scope, engine velocity or reissue is marked");
    check(FakeEnv::g_window.kindDraws[2] == 1 && FakeEnv::g_window.kindTicks[2] == 70 && FakeEnv::g_window.kindMarks[2] == 5,
          "fast path: the N draw's EDVR ticks are everything but the game's draw (70) and its marks the five that closed them");
}

// The draw lambda can run more than once; only the first real call is the game's, every later one is a reissue and EDVR's.
void testStepClockReissue() {
    using namespace edvr::drawstep;
    FakeEnv::reset();
    FakeEnv::g_sampled = true;
    FakeEnv::g_ordinal = 31;
    FakeEnv::g_time = 0;
    {
        TestClock c('I');
        FakeEnv::g_time = 50;
        c.markForward();   // first lambda entry
        FakeEnv::g_time = 150;
        c.stepDraw();      // the game's draw: 100
        FakeEnv::g_time = 160;
        c.markForward();   // second lambda entry (the splash dim's issue): after the first real call
        FakeEnv::g_time = 200;
        c.stepDraw();      // the reissue: 40
        FakeEnv::g_time = 230;
        c.stepDraw();      // a third call: 30 more, still a reissue
        FakeEnv::g_time = 235;
        c.markForward();   // forwardWithVerdict's return
        FakeEnv::g_time = 240;
    }
    const StepAccum& a = FakeEnv::g_lastRun;
    check(a.ticks[kGameDraw] == 100 && a.marks[kGameDraw] == 1, "reissue: the first real call is the game's draw, once");
    check(a.ticks[kReissue] == 70 && a.marks[kReissue] == 2, "reissue: every later real call goes to reissue (40 + 30 over two calls)");
    check(a.ticks[kFwBefore] == 50 && a.marks[kFwBefore] == 1 && a.ticks[kFwAfter] == 15 && a.marks[kFwAfter] == 2,
          "reissue: before the first real draw the forward scope is 'before', after it every entry and the return is 'after'");
    // A draw the forward scope swallows never calls the lambda: its whole stretch is the scope's 'before'.
    FakeEnv::reset();
    FakeEnv::g_sampled = true;
    FakeEnv::g_ordinal = 31;
    {
        TestClock c('X');
        FakeEnv::g_time = 70;
        c.markForward();   // forwardWithVerdict returned with no draw made
        FakeEnv::g_time = 75;
    }
    check(FakeEnv::g_lastRun.ticks[kFwBefore] == 70 && FakeEnv::g_lastRun.marks[kGameDraw] == 0 && FakeEnv::g_lastRun.marks[kFwAfter] == 0,
          "reissue: a draw the scope swallowed is all 'before', with no game draw");
    // EDVR's part of a run: every step but the game's draw.
    StepAccum e;
    e.ticks[kPre] = 10;
    e.marks[kPre] = 1;
    e.ticks[kGameDraw] = 500;
    e.marks[kGameDraw] = 1;
    e.ticks[kReissue] = 7;
    e.marks[kReissue] = 2;
    check(edvrTicks(e) == 17 && edvrMarks(e) == 3, "reissue: EDVR's sum is every step but the game's draw (17 ticks, 3 marks)");
}

// The window: the runs added by kind, EDVR's ticks and marks excluding the game's draw.
void testStepWindow() {
    using namespace edvr::drawstep;
    Window w;
    w.reset();
    StepAccum d;
    d.ticks[kPre] = 100; d.marks[kPre] = 1;
    d.ticks[kGameDraw] = 900; d.marks[kGameDraw] = 1;
    d.ticks[kTail] = 50; d.marks[kTail] = 1;
    StepAccum x;
    x.ticks[kPre] = 200; x.marks[kPre] = 1;
    x.ticks[kWeapon] = 70; x.marks[kWeapon] = 1;
    x.ticks[kGameDraw] = 5; x.marks[kGameDraw] = 1;
    x.ticks[kTail] = 30; x.marks[kTail] = 1;
    for (int i = 0; i < 3; ++i) w.add('D', d);
    w.add('X', x);
    w.add('I', d);
    check(w.clocked == 5 && w.kindDraws[0] == 3 && w.kindDraws[1] == 1 && w.kindDraws[2] == 0 && w.kindDraws[3] == 1,
          "window: five clocked draws by kind (3 D, 1 I, 0 N, 1 X)");
    check(w.kindTicks[0] == 450 && w.kindMarks[0] == 6 && w.kindTicks[3] == 300 && w.kindMarks[3] == 3,
          "window: per-kind EDVR ticks and marks leave the game's draw out");
    check(w.ticks[kGameDraw] == 3 * 900 + 900 + 5 && w.marks[kPre] == 5 && w.ticks[kPre] == 3 * 100 + 200 + 100,
          "window: the steps' own ticks and marks add up across draws");
    w.reset();
    check(w.clocked == 0 && w.ticks[kPre] == 0 && w.kindDraws[3] == 0 && w.kindTicks[3] == 0, "window: reset clears every total");
}

// The clock floor: net of marks x floor, scaled by the stride over the sampled frames.
void testStepFloorAndScale() {
    using namespace edvr::drawstep;
    Window w;
    w.reset();
    w.clocked = 10;
    w.ticks[kPre] = 10000;       w.marks[kPre] = 500;        // net 10000 - 500 x 10 = 5000 ticks
    w.ticks[kVRest] = 100;       w.marks[kVRest] = 50;        // under its own floor: 0, not negative
    w.ticks[kGameDraw] = 40000;  w.marks[kGameDraw] = 500;    // net 35000 ticks
    w.ticks[kTail] = 3000;       w.marks[kTail] = 100;        // net 2000 ticks
    w.kindDraws[3] = 10;
    w.kindTicks[3] = 13000;      w.kindMarks[3] = 650;        // EDVR's: net 13000 - 6500 = 6500
    FormatInfo in;
    in.endFrame = 1800;
    in.sampledFrames = 100;
    in.stride = 64;
    in.ticksPerSecond = 10000000;   // 100 ns a tick
    in.floorNs = 1000.0;            // 10 ticks a mark
    Totals t;
    check(summarise(w, in, &t), "floor: a window with sampled frames summarises");
    // 5000 ticks / 1e7 per second = 0.5 ms; x64 / 100 sampled frames = 0.32 ms.
    check(closeTo(t.netMs[kPre], 0.32) && t.seen[kPre], "floor: a step's ticks lose marks x floor, then scale by the stride over the sampled frames (0.32 ms)");
    check(t.seen[kVRest] && t.netMs[kVRest] == 0.0, "floor: a step shorter than its own clock reads is 0.000, not negative");
    check(!t.seen[kProbes] && t.netMs[kProbes] == 0.0 && t.callsPerFrame[kProbes] == 0.0, "floor: a step with no marks is not seen");
    check(closeTo(t.netMs[kGameDraw], 35000.0 / 1e7 * 1000.0 * 64.0 / 100.0) && closeTo(t.gameMs, t.netMs[kGameDraw]),
          "floor: the game's draw has its own figure, net of its marks too");
    check(closeTo(t.edvrMs, t.netMs[kPre] + t.netMs[kVRest] + t.netMs[kTail]) && closeTo(t.netMs[kTail], 2000.0 / 1e7 * 1000.0 * 64.0 / 100.0),
          "floor: EDVR's sum is every seen step but the game's draw");
    check(closeTo(t.callsPerFrame[kPre], 500.0 * 64.0 / 100.0), "floor: calls per frame are the marks scaled by the stride over the sampled frames (320)");
    check(closeTo(t.marksPerDraw, (500 + 50 + 500 + 100) / 10.0), "floor: marks per clocked draw count every step's marks (115)");
    check(t.kindSeen[3] && !t.kindSeen[0] && closeTo(t.kindDrawsPerFrame[3], 10.0 * 64.0 / 100.0) &&
              closeTo(t.kindMs[3], 6500.0 / 1e7 * 1000.0 * 64.0 / 100.0),
          "floor: a kind's draws per frame and EDVR ms per frame are scaled the same way, net of its marks");
    in.sampledFrames = 0;
    check(!summarise(w, in, &t) && !t.seen[kPre] && t.edvrMs == 0.0, "floor: no sampled frame in the window is unavailable, not a zero");
}

// The floor's calibration: the fastest of the batches, through markRun.
struct ScriptClock {
    int64_t* t;
    int64_t* calls;
    int64_t  inc;
    int64_t  spikeEvery;   // every this-many calls one read jumps by 5000 ticks (0: never)
    int64_t operator()() const {
        ++*calls;
        *t += inc;
        if (spikeEvery && (*calls % spikeEvery) == 0) *t += 5000;
        return *t;
    }
};
void testStepCalibration() {
    using namespace edvr::drawstep;
    int64_t t = 0, calls = 0;
    ScriptClock clean{&t, &calls, 25, 0};
    // 25 ticks a clock call at 10 MHz is 2500 ns a mark.
    check(closeTo(calibrateMarkNs(clean, 10000000), 2500.0), "calibration: a clock that costs 25 ticks a call at 10 MHz is 2500 ns a mark");
    check(calls == 1 + 64 + 4 * (1 + 512), "calibration: one start read, 64 warming marks, then four batches of 512 marks each timed from its own start read");
    // One batch with a preemption in it is not the floor: the fastest batch is kept.
    t = 0;
    calls = 0;
    ScriptClock noisy{&t, &calls, 25, 700};   // a spike in some batches, not all
    check(closeTo(calibrateMarkNs(noisy, 10000000), 2500.0), "calibration: a batch a preemption inflated is not the floor, the clean batch is");
    t = 0;
    calls = 0;
    ScriptClock spiky{&t, &calls, 25, 3};   // every batch spiked: the best is still the least spiked
    check(calibrateMarkNs(spiky, 10000000) > 2500.0, "calibration: when every batch is spiked the floor reads above the clean figure");
    check(calibrateMarkNs(clean, 0) == 0.0 && calibrateMarkNs(clean, 10000000, 0, 4) == 0.0 && calibrateMarkNs(clean, 10000000, 512, 0) == 0.0,
          "calibration: no frequency, no marks or no batches is 0, not a division by zero");
}

// The two log lines.
edvr::drawstep::Window sampleWindow() {
    using namespace edvr::drawstep;
    Window w;
    w.reset();
    w.clocked = 10;
    w.ticks[kPre] = 10000;      w.marks[kPre] = 500;
    w.ticks[kVCensus] = 8000;   w.marks[kVCensus] = 400;
    w.ticks[kGameDraw] = 40000; w.marks[kGameDraw] = 500;
    w.ticks[kWeapon] = 1500;    w.marks[kWeapon] = 20;
    w.ticks[kTail] = 3000;      w.marks[kTail] = 100;
    w.kindDraws[3] = 10;
    w.kindTicks[3] = 22500;
    w.kindMarks[3] = 1020;
    return w;
}
void testStepText() {
    using namespace edvr::drawstep;
    const Window w = sampleWindow();
    FormatInfo in;
    in.endFrame = 5400;
    in.sampledFrames = 112;
    in.stride = 64;
    in.ticksPerSecond = 10000000;
    in.floorNs = 900.0;
    in.aaOn = true;
    char line[1200];
    const size_t n = formatStepsLine(line, sizeof(line), w, in);
    check(n == std::strlen(line), "text: the steps line's returned length is its length");
    static const char kHead[] = "draw hook CPU by step: 1800-frame window ending 5400; 112 sampled frames, 10 draws clocked by step";
    check(std::strncmp(line, kHead, sizeof(kHead) - 1) == 0,
          "text: the steps line starts 'draw hook CPU by step:' with the window's end, sampled frames and clocked draws");
    check(std::strstr(line, "every 64th draw, offset 32 from the line above's") != nullptr,
          "text: it says the draws are a different set from the line above's (every 64th, offset 32)");
    check(std::strstr(line, "about 900 ns, 152.0 per clocked draw") != nullptr, "text: the floor in ns a mark and the marks per clocked draw are stated once (900, 152.0)");
    Totals t;
    summarise(w, in, &t);
    char want[64];
    std::snprintf(want, sizeof(want), "pre %.3f, ", t.netMs[kPre]);
    check(std::strstr(line, want) != nullptr, "text: a seen step prints its net ms per sampled frame");
    check(std::strstr(line, "v:hud/fss -, ") != nullptr && std::strstr(line, "f:ui-depth -, ") != nullptr && std::strstr(line, "reissue -, ") != nullptr,
          "text: a step with no marks prints '-', never 0.000");
    std::snprintf(want, sizeof(want), "EDVR %.3f ms", t.edvrMs);
    check(std::strstr(line, want) != nullptr, "text: EDVR's sum is in the line");
    std::snprintf(want, sizeof(want), "game draw %.3f ms (the game's own)", t.gameMs);
    check(std::strstr(line, want) != nullptr, "text: the game's draw has its own figure, labelled the game's");
    check(std::strstr(line, "counts weapon as the game's, so it reads near this less weapon") != nullptr,
          "text: it says the line above should read near EDVR's sum less weapon motion");
    check(std::strstr(line, "Temporal AA on.") != nullptr, "text: Temporal AA on");
    in.aaOn = false;
    formatStepsLine(line, sizeof(line), w, in);
    check(std::strstr(line, "Temporal AA off.") != nullptr, "text: Temporal AA off");
    char calls[1200];
    const size_t m = formatCallsLine(calls, sizeof(calls), w, in);
    static const char kCallsHead[] = "draw hook CPU by step, calls and kinds: window ending 5400.";
    check(m == std::strlen(calls) && std::strncmp(calls, kCallsHead, sizeof(kCallsHead) - 1) == 0,
          "text: the calls line follows with its own head");
    std::snprintf(want, sizeof(want), "pre %.0f, ", t.callsPerFrame[kPre]);
    check(std::strstr(calls, want) != nullptr && std::strstr(calls, "v:hud/fss -, ") != nullptr, "text: the calls line prints each step's calls per frame and '-' for none");
    std::snprintf(want, sizeof(want), "D -, I -, N -, X %.0f draws ", t.kindDrawsPerFrame[3]);
    check(std::strstr(calls, want) != nullptr,
          "text: the kind block gives the X draws per frame and '-' for the kinds never clocked");
    // A window with no sampled frames: every figure unavailable.
    in.sampledFrames = 0;
    formatStepsLine(line, sizeof(line), w, in);
    check(std::strstr(line, "EDVR -; game draw -") != nullptr && std::strstr(line, "pre -, ") != nullptr && std::strstr(line, "pre 0") == nullptr,
          "text: no sampled frame prints '-' for every figure");
}

// Both lines at their widest. Every figure is at its clamp (a net far past 99999.999 ms, calls past 99999999, a floor past 99999999 ns), the frame
// numbers at their 32-bit maximum, and the clocked-draw count at both ends of its width: 1 puts the marks per clocked draw at its widest, the
// 64-bit maximum puts the draw count at its widest. The log line holds about 1170 characters; the report stays under 1150 in both lines.
void testStepTextWorstCase() {
    using namespace edvr::drawstep;
    size_t widestA = 0, widestB = 0;
    bool wholeEnds = true;
    char a[2000], b[2000];
    for (int widest = 0; widest < 2; ++widest) {
        Window w;
        w.reset();
        w.clocked = widest ? 18446744073709551615ull : 1ull;
        for (unsigned i = 0; i < kStepCount; ++i) {
            w.ticks[i] = 4000000000000000000ll;
            w.marks[i] = 4000000000ull;
        }
        for (unsigned k = 0; k < kKinds; ++k) {
            w.kindDraws[k] = 4000000000ull;
            w.kindTicks[k] = 4000000000000000000ll;
            w.kindMarks[k] = 4000000000ull;
        }
        FormatInfo in;
        in.endFrame = 4294967295u;
        in.sampledFrames = 1;
        in.stride = 64;
        in.ticksPerSecond = 10000000;
        in.floorNs = 1e8;
        in.aaOn = false;
        const size_t na = formatStepsLine(a, sizeof(a), w, in);
        const size_t nb = formatCallsLine(b, sizeof(b), w, in);
        widestA = na > widestA ? na : widestA;
        widestB = nb > widestB ? nb : widestB;
        wholeEnds = wholeEnds && na == std::strlen(a) && nb == std::strlen(b) && a[na - 1] == '.' && b[nb - 1] == '.';
        check(std::strstr(a, "99999.999") != nullptr && std::strstr(b, "99999999 draws") != nullptr,
              "text: out-of-range figures are clamped to their widths, not widened");
    }
    std::printf("aa_off_test: step lines at their worst: %zu and %zu characters (limit 1150; the log holds about 1170)\n", widestA, widestB);
    check(widestA < 1150 && widestB < 1150, "text: each of the two lines at its worst fits the log line (under 1150 characters)");
    check(wholeEnds, "text: both lines end whole at their worst, not cut");
    // A plausible window is shorter than the worst case.
    const Window ordinary = sampleWindow();
    FormatInfo oi;
    oi.endFrame = 5400;
    oi.sampledFrames = 112;
    oi.ticksPerSecond = 10000000;
    oi.floorNs = 900.0;
    check(formatStepsLine(a, sizeof(a), ordinary, oi) < widestA, "text: an ordinary window's line is shorter than the worst case");
    // A short buffer truncates, never overruns, and stays terminated.
    Window any = sampleWindow();
    char tiny[24];
    std::memset(tiny, 'x', sizeof(tiny));
    const size_t nt = formatStepsLine(tiny, sizeof(tiny), any, oi);
    check(nt < sizeof(tiny) && tiny[nt] == '\0', "text: a short buffer is cut and terminated, never overrun");
    char one[1] = {'x'};
    check(formatCallsLine(one, 1, any, oi) == 0 && one[0] == '\0', "text: a one-byte buffer holds the terminator alone");
}

// ---- Cut F: classifier_memo.h -------------------------------------------------------------------------------------------------------

// A hit needs both halves of the key equal AND the stored exit to be a plain decline; every other call is a miss with one cause.
void testMemoHit() {
    using namespace edvr::classifiermemo;
    Memo m;
    const Key k{5, 7};
    check(probe(m, k) == Probe::kMissNoPlain, "memo: nothing stored is a miss, counted as no plain decline stored");
    store(m, k, Exit::kPlainDecline);
    check(probe(m, k) == Probe::kHit, "memo: both halves equal after a plain decline is a hit");
    check(probe(m, Key{6, 7}) == Probe::kMissGeneration, "memo: a moved generation is a miss, counted as generation moved");
    check(probe(m, Key{5, 8}) == Probe::kMissEpoch, "memo: a moved epoch is a miss, counted as epoch moved");
    check(probe(m, Key{6, 8}) == Probe::kMissGeneration, "memo: both moved is counted once, as the generation");
    check(probe(m, Key{4, 7}) == Probe::kMissGeneration && probe(m, Key{5, 6}) == Probe::kMissEpoch,
          "memo: a key that moved backwards is a miss as well, never a hit");
    check(probe(m, Key{0, 0}) == Probe::kMissGeneration, "memo: the zero key does not match a stored one");
    // The stored exit decides what an equal key means.
    store(m, k, Exit::kOther);
    check(probe(m, k) == Probe::kMissNoPlain, "memo: an equal key after an exit that was not a plain decline is a miss (no plain decline stored)");
    check(probe(m, Key{6, 7}) == Probe::kMissGeneration && probe(m, Key{5, 8}) == Probe::kMissEpoch,
          "memo: a moved key after a non-plain exit is still counted by what moved");
    store(m, k, Exit::kPlainDecline);
    check(probe(m, k) == Probe::kHit, "memo: storing the plain decline again restores the hit");
    store(m, Key{9, 9}, Exit::kPlainDecline);
    check(probe(m, k) == Probe::kMissGeneration && probe(m, Key{9, 9}) == Probe::kHit, "memo: a later store replaces the earlier key");
    invalidate(m);
    check(probe(m, Key{9, 9}) == Probe::kMissNoPlain && !m.stored, "memo: invalidate forgets the answer, so the same key misses");
    // Both classifiers keep a memo of their own: storing in one leaves the other alone.
    Memo eye, holo;
    store(eye, k, Exit::kPlainDecline);
    check(probe(holo, k) == Probe::kMissNoPlain && probe(eye, k) == Probe::kHit, "memo: two memos are independent");
    // The three causes and the hit exhaust the answers: every call is exactly one of them.
    unsigned seen[4] = {};
    for (uint32_t g = 3; g < 8; ++g) {
        for (uint32_t e = 3; e < 8; ++e) {
            for (int plain = 0; plain < 2; ++plain) {
                Memo c;
                store(c, Key{5, 5}, plain ? Exit::kPlainDecline : Exit::kOther);
                ++seen[static_cast<unsigned>(probe(c, Key{g, e}))];
            }
        }
    }
    check(seen[0] + seen[1] + seen[2] + seen[3] == 50 && seen[0] == 1, "memo: over a grid of keys exactly one cell is a hit");
}

// A model of a run of draws through the memo, as uiDepthOnEyeDraw drives it: key, probe, tally, then store the exit the full run took.
void testMemoRun() {
    using namespace edvr::classifiermemo;
    Memo m;
    Tally t;
    const auto call = [&](Key now, Exit exitIfRun) {
        const Probe p = probe(m, now);
        t.noteProbe(p);
        if (p != Probe::kHit) store(m, now, exitIfRun);
        return p;
    };
    const Key a{10, 3}, b{11, 3}, c{11, 4};
    check(call(a, Exit::kPlainDecline) == Probe::kMissNoPlain, "run: the first call finds nothing stored");
    check(call(a, Exit::kPlainDecline) == Probe::kHit && call(a, Exit::kPlainDecline) == Probe::kHit, "run: the repeats hit");
    check(call(b, Exit::kOther) == Probe::kMissGeneration, "run: a draw whose bindings moved runs in full, and its exit is not a plain decline");
    check(call(b, Exit::kOther) == Probe::kMissNoPlain, "run: its repeat finds the key but not a plain decline, so it runs in full again");
    check(call(a, Exit::kPlainDecline) == Probe::kMissGeneration, "run: back to the plain draw, the generation has moved");
    check(call(a, Exit::kPlainDecline) == Probe::kHit, "run: ...and it hits again");
    check(call(c, Exit::kPlainDecline) == Probe::kMissGeneration, "run: a bump of both is counted as the generation");
    check(call(Key{11, 5}, Exit::kPlainDecline) == Probe::kMissEpoch, "run: an epoch move alone is counted as the epoch");
    check(t.hits() == 3 && t.missNoPlain() == 2 && t.missGeneration() == 3 && t.missEpoch() == 1 && t.calls() == 9,
          "run: the window counts 3 hits and 6 misses by cause, and the calls are their sum");
}

// One verify frame in 16, by the module's own counter: exactly one in each block of 16, and the phase moves so every residue of the
// sample cadence is reached.
void testMemoVerifyFrame() {
    using namespace edvr::classifiermemo;
    unsigned total = 0;
    bool oncePerBlock = true;
    unsigned residues[16] = {};
    for (uint32_t block = 0; block < 16; ++block) {
        unsigned inBlock = 0;
        for (uint32_t i = 0; i < 16; ++i) {
            const uint32_t n = block * 16 + i;
            if (!verifyFrame(n)) continue;
            ++inBlock;
            ++residues[n % 16];
        }
        oncePerBlock = oncePerBlock && inBlock == 1;
        total += inBlock;
    }
    check(total == 16 && oncePerBlock, "verify: 16 verify frames in 256, exactly one in each block of 16");
    bool every = true;
    for (unsigned r = 0; r < 16; ++r) every = every && residues[r] == 1;
    check(every, "verify: across 256 frames every residue mod 16 verifies once, so a fixed draw-sample cadence never blocks them all");
    check(verifyFrame(8) && !verifyFrame(0) && !verifyFrame(9), "verify: the first block verifies the frame at offset 8");
    // A draw-sample frame never verifies, whichever way the cadences line up.
    unsigned sampled[16] = {};
    for (unsigned align = 0; align < 16; ++align) {
        for (uint32_t n = 0; n < 4096; ++n) {
            const bool sampleFrame = ((n + align) % 16) == 0;
            if (verifyFrame(n) && !sampleFrame) ++sampled[align];
        }
    }
    bool everyAlignVerifies = true;
    for (unsigned align = 0; align < 16; ++align) everyAlignVerifies = everyAlignVerifies && sampled[align] == 240;
    check(everyAlignVerifies, "verify: with the sample cadence at any alignment, 240 of the 256 verify frames in 4096 are not sampled ones (16 coincide, none is lost)");
}

// The tally: calls are hits plus misses, a verified hit counts once, and a mismatch counts once per output that differed.
void testMemoTally() {
    using namespace edvr::classifiermemo;
    Tally t;
    check(t.calls() == 0 && t.hits() == 0 && t.verified == 0 && t.mismatched == 0, "tally: starts empty");
    for (int i = 0; i < 7; ++i) t.noteProbe(Probe::kHit);
    t.noteProbe(Probe::kMissNoPlain);
    t.noteProbe(Probe::kMissGeneration);
    t.noteProbe(Probe::kMissGeneration);
    t.noteProbe(Probe::kMissEpoch);
    check(t.calls() == 11 && t.hits() == 7 && t.missNoPlain() == 1 && t.missGeneration() == 2 && t.missEpoch() == 1,
          "tally: calls are the hits and the misses by cause");
    t.noteVerified(0);
    t.noteVerified(0);
    check(t.verified == 2 && t.mismatched == 0, "tally: a verified hit with no difference counts as verified only");
    t.noteVerified((1u << kEyeComposite) | (1u << kEyeMode) | (1u << kEyeExit));
    t.noteVerified(1u << kEyeMode);
    check(t.verified == 4 && t.mismatched == 2, "tally: a mismatched hit counts once however many outputs differed");
    check(t.byOutput[kEyeComposite] == 1 && t.byOutput[kEyeMode] == 2 && t.byOutput[kEyeExit] == 1 && t.byOutput[kEyeReturn] == 0 &&
              t.byOutput[kEyeShader] == 0,
          "tally: each differing output is counted on its own");
    // Every output's bit lands in its own counter, none in a neighbour's.
    Tally each;
    for (unsigned i = 0; i < kEyeOutputCount; ++i) each.noteVerified(1u << i);
    bool own = each.mismatched == kEyeOutputCount;
    for (unsigned i = 0; i < kMaxOutputs; ++i) own = own && each.byOutput[i] == (i < kEyeOutputCount ? 1u : 0u);
    check(own, "tally: each of the eye draw's outputs has its own counter");
    // The naming budget: the first three of the session, kept across windows, taken only on request.
    check(t.takeName() && t.takeName() && t.takeName() && !t.takeName() && !t.takeName() && t.named == kMaxNamed,
          "tally: three mismatches are named, then no more");
    t.resetWindow();
    check(t.calls() == 0 && t.verified == 0 && t.mismatched == 0 && t.byOutput[kEyeMode] == 0, "tally: the window's reset clears every count");
    check(!t.takeName() && t.named == kMaxNamed, "tally: the naming budget is the session's, so the reset keeps it");
    // The output names: one per output, distinct, and the two classifiers' own.
    bool distinct = true;
    for (unsigned i = 0; i < kEyeOutputCount; ++i) {
        distinct = distinct && eyeOutputName(i)[0] != '?' && eyeOutputName(i)[0] != '\0';
        for (unsigned j = i + 1; j < kEyeOutputCount; ++j) distinct = distinct && std::strcmp(eyeOutputName(i), eyeOutputName(j)) != 0;
    }
    check(distinct && std::strcmp(eyeOutputName(kEyeOutputCount), "?") == 0, "tally: the eye draw's 15 outputs have 15 distinct names");
    check(kEyeOutputCount == 15 && kHoloOutputCount == 3 && std::strcmp(holoOutputName(kHoloEye), "eye") == 0 &&
              std::strcmp(holoOutputName(kHoloExit), "exit") == 0 && std::strcmp(holoOutputName(kHoloOutputCount), "?") == 0,
          "tally: the hologram's three outputs are the return, the eye and the exit");
}

// The window line: a quiet window prints dashes where a figure would be a lie, a busy one prints every count under its name, and the
// worst case fits the log line.
void testMemoText() {
    using namespace edvr::classifiermemo;
    char line[1500];
    // No calls at all (the pass is off): the rate and every mismatch figure are "-", the counts are 0.
    Tally quietEye, quietHolo;
    const size_t nq = formatWindowLine(line, sizeof(line), 3600, quietEye, quietHolo);
    check(std::strncmp(line, "ui depth memo: 1800-frame window ending 3600.", 45) == 0, "text: the line opens with its name and the window's end frame");
    check(std::strstr(line, "Eye draws: 0 calls past the on test, 0 hits (-)") != nullptr &&
              std::strstr(line, "hologram draws: 0 calls past the on test, 0 hits (-)") != nullptr,
          "text: a window with no calls has a dash for the hit rate and zero counts");
    check(std::strstr(line, "0 hits verified, mismatches - (return -, composite -, planet -, solar planet -, world screen -, corona -, corona motion -, "
                            "mode -, rebind -, mask -, rebind eye -, draw eye -, shader -, mask slot -, exit -)") != nullptr &&
              std::strstr(line, "mismatches - (return -, eye -, exit -)") != nullptr,
          "text: with no verified hit every mismatch figure is a dash, never a zero that reads as clean");
    check(nq == std::strlen(line) && line[nq - 1] == '.' && std::strstr(line, "nan") == nullptr && std::strstr(line, "inf") == nullptr,
          "text: a quiet window's line ends whole and holds no nan or inf");
    // A busy window: 90% hits, a few misses by cause, verified hits, and a mismatch in two outputs.
    Tally eye, holo;
    for (int i = 0; i < 900; ++i) eye.noteProbe(Probe::kHit);
    for (int i = 0; i < 40; ++i) eye.noteProbe(Probe::kMissNoPlain);
    for (int i = 0; i < 50; ++i) eye.noteProbe(Probe::kMissGeneration);
    for (int i = 0; i < 10; ++i) eye.noteProbe(Probe::kMissEpoch);
    for (int i = 0; i < 60; ++i) eye.noteVerified(0);
    eye.noteVerified((1u << kEyeReturn) | (1u << kEyeShader));
    for (int i = 0; i < 20; ++i) holo.noteProbe(Probe::kHit);
    for (int i = 0; i < 5; ++i) holo.noteVerified(0);
    formatWindowLine(line, sizeof(line), 5400, eye, holo);
    check(std::strstr(line, "ending 5400.") != nullptr, "text: the window's end frame is printed");
    check(std::strstr(line, "Eye draws: 1000 calls past the on test, 900 hits (90.0%), misses: no plain decline stored 40, generation moved 50, epoch moved 10; "
                            "61 hits verified, mismatches 1 (return 1, composite 0,") != nullptr,
          "text: the busy window prints calls, hits, the rate, the three miss causes, the verified hits and the mismatch total");
    check(std::strstr(line, "shader 1, mask slot 0, exit 0)") != nullptr, "text: each output's count is printed under its name");
    check(std::strstr(line, "hologram draws: 20 calls past the on test, 20 hits (100.0%), misses: no plain decline stored 0, generation moved 0, epoch moved 0; "
                            "5 hits verified, mismatches 0 (return 0, eye 0, exit 0)") != nullptr,
          "text: a verified classifier with no mismatch prints zeros, and the rate reaches 100.0%");
    check(std::strstr(line, "one frame in 16") != nullptr && std::strstr(line, "never a draw-sample frame") != nullptr &&
              std::strstr(line, "a temporary check") != nullptr,
          "text: the line says the check is temporary and when it runs");
    // A window one eye hit short of verification: mismatches are a dash for that classifier only.
    Tally mixed;
    mixed.noteProbe(Probe::kHit);
    formatWindowLine(line, sizeof(line), 1800, mixed, holo);
    check(std::strstr(line, "0 hits verified, mismatches - (return -,") != nullptr && std::strstr(line, "5 hits verified, mismatches 0 (return 0, eye 0") != nullptr,
          "text: the dash is per classifier: one with no verified hit, one with five");
    // The mismatch line: which classifier, how many are named, the vertex shader as 16 hex digits, the outputs that differed.
    char ml[400];
    const size_t nm = formatMismatchLine(ml, sizeof(ml), "eye draws", 2, 0xB7790CBFC6554097ull, (1u << kEyeMode) | (1u << kEyeExit), kEyeOutputCount, eyeOutputName);
    check(std::strcmp(ml, "ui depth memo mismatch 2 of 3 named (eye draws): a hit and the full classifier differed for vs B7790CBFC6554097 in: mode, exit. "
                          "The full classifier's answer stood.") == 0 && nm == std::strlen(ml),
          "text: a mismatch line names the classifier, the ordinal, the vertex shader and the outputs");
    formatMismatchLine(ml, sizeof(ml), "hologram draws", 1, 0x0000000000000042ull, 1u << kHoloReturn, kHoloOutputCount, holoOutputName);
    check(std::strstr(ml, "(hologram draws)") != nullptr && std::strstr(ml, "vs 0000000000000042 in: return.") != nullptr,
          "text: the hologram's mismatch line zero-pads the hash and names its own outputs");
    // A mask with no bit inside the classifier's outputs names nothing, and says so.
    formatMismatchLine(ml, sizeof(ml), "hologram draws", 1, 1ull, 1u << 9, kHoloOutputCount, holoOutputName);
    check(std::strstr(ml, "in: nothing named.") != nullptr, "text: a difference mask with no named output says so");
}

// The window line at its widest: every figure at its 32-bit maximum. The log line holds about 1170 characters (Log::note's 1200 less
// its stamp and truncation mark); the line stays under 1150, and the mismatch line under its own buffer.
void testMemoTextWorstCase() {
    using namespace edvr::classifiermemo;
    Tally eye, holo;
    for (Tally* t : {&eye, &holo}) {
        for (unsigned p = 0; p < static_cast<unsigned>(Probe::kCount); ++p) t->probes[p] = 4294967295u;
        t->verified = 4294967295u;
        t->mismatched = 4294967295u;
        for (unsigned i = 0; i < kMaxOutputs; ++i) t->byOutput[i] = 4294967295u;
    }
    char line[2000];
    const size_t n = formatWindowLine(line, sizeof(line), 4294967295u, eye, holo);
    std::printf("aa_off_test: ui depth memo line at its worst: %zu characters (limit 1150; the log holds about 1170)\n", n);
    check(n < 1150, "text: the window line at its worst fits the log line (under 1150 characters)");
    check(n == std::strlen(line) && line[n - 1] == '.', "text: the window line ends whole at its worst, not cut");
    check(std::strstr(line, "ending 4294967295.") != nullptr && std::strstr(line, "calls past the on test") != nullptr,
          "text: the widest frame number is printed in full");
    // Past the u32 sums: calls is four counts that can each reach the maximum, and wraps like the counters themselves; the line still fits.
    char ml[400];
    const size_t nm = formatMismatchLine(ml, sizeof(ml), "hologram draws", 3, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFu, kEyeOutputCount, eyeOutputName);
    check(nm < 399 && nm == std::strlen(ml) && ml[nm - 1] == '.', "text: the mismatch line with every output differing fits and ends whole");
    // A short buffer is cut and terminated, never overrun.
    char tiny[30];
    std::memset(tiny, 'x', sizeof(tiny));
    const size_t nt = formatWindowLine(tiny, sizeof(tiny), 1, eye, holo);
    check(nt < sizeof(tiny) && tiny[nt] == '\0', "text: a short buffer is cut and terminated");
    char one[1] = {'x'};
    check(formatWindowLine(one, 1, 1, eye, holo) == 0 && one[0] == '\0', "text: a one-byte buffer holds the terminator alone");
}

void usage() {
    std::puts("usage: aa_off_test --self-test | --dry-run");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("aa_off_test: dry run, nothing written");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        usage();
        return 2;
    }
    testFastPathTable();
    testBindShadows();
    testThunkModel();
    testBindToggle();
    testSampler();
    testMapTotals();
    testGpuGuard();
    testGpuTimeTile();
    testFrameWorkSlack();
    testOverlayLine();
    testStepClockModes();
    testStepClockOtherModes();
    testStepClockMarks();
    testStepClockFastPath();
    testStepClockReissue();
    testStepWindow();
    testStepFloorAndScale();
    testStepCalibration();
    testStepText();
    testStepTextWorstCase();
    testMemoHit();
    testMemoRun();
    testMemoVerifyFrame();
    testMemoTally();
    testMemoText();
    testMemoTextWorstCase();
    std::printf("aa_off_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
