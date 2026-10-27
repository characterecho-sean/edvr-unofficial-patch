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
//   Cut F: classifier_memo.h (built 2026-10-27, the ui_depth eye-draw memo): a call hits only when something is stored, both halves
//          of its key equal the stored key and the stored exit is a plain decline; a moved generation or epoch is a miss.
//   Cut G: ui_family_memo.h (built 2026-10-27, trim F, the UI layer's family decision): a hit needs every field of the key equal and a
//          stored plain decline; the store rule never stores target kind 2, the menu panel's or the loader's vertex shader, a family, or a
//          run whose on-foot flag moved; a sweep of every named vertex and pixel shader, the flag both ways, through sequences of draws
//          (with controls: each field dropped from the key, or a forbidden store, makes the sweep fail); the verify frames and the window
//          line's worst-case length; and the source's wiring (the gather reads nothing but the key's inputs below kind 2, and every reset
//          of the target-kind cache bumps the epoch). The step clock's f:after-ui split is in Cut E.
//   Cut H: draw_fast_path_shadow.h (built 2026-10-27, TEMPORARY, the repeat-draw fast path's Phase 1 shadow instrument): the args class at its
//          edges (64, 65, 240, 10000, 10001, instances 0); the refusal order over every combination of the six stages, and that a later
//          stage's flags are not gathered once an earlier one refuses; the ticket's update rules (a non-ordinary draw invalidates, an
//          off-screen target unsettled or a wake surface does not record, an unknown class does not record); the mismatch tally per term
//          and the class change; the first five named; the certified set; the window line's text, its "-" cases and its worst-case length;
//          and the source's wiring (the gate's list and the subscriber set agree term for term, the shadow sits after the flat and world-route
//          returns, and adds no return to the draw path).
//
// Exit codes: 0 pass, 1 a check failed, 2 usage. --dry-run touches nothing.
#include "../../src/common/gpu_disjoint_clock.h"
#include "../../src/d3d11/classifier_memo.h"
#include "../../src/d3d11/draw_fast_path.h"
#include "../../src/d3d11/draw_fast_path_shadow.h"
#include "../../src/d3d11/draw_step_clock.h"
#include "../../src/d3d11/frame_span_tile.h"
#include "../../src/d3d11/gpu_frame_timing.h"
#include "../../src/d3d11/map_wait.h"
#include "../../src/d3d11/ui_family_memo.h"
#include "../../src/d3d11/ui_layer_math.h"

// The frame span holds at most kFrameSpanSlots pairs of two timestamps each (gpu_census.cpp); every one must fit the
// clock's per-record lease budget (kLeases, 512), or a pair would be refused rather than held.
static_assert(edvr::kFrameSpanSlots * 2u <= edvr::DisjointClock::kLeases,
              "the frame span's timestamps fit the disjoint clock's lease budget");

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

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

// 2026-10-27: f:ui-layer is split. forwardWithVerdict marks f:ui-layer where the family decision ends, f:after-ui where the after-UI branch
// ends, and f:ui-layer once more where the composite census ends (a composite draw only). The new step sits between f:ui-layer and
// f:verdict-begin in the order a draw meets them, is named, and is cut apart from the decision's time.
void testStepClockUiLayerSplit() {
    using namespace edvr::drawstep;
    check(kFwScopes < kFwUiLayer && kFwUiLayer < kFwAfterUi && kFwAfterUi < kFwVerdict && kFwVerdict < kFwBefore,
          "ui-layer split: f:after-ui comes after f:ui-layer and before f:verdict-begin, in the order a draw meets them");
    check(std::strcmp(stepName(kFwAfterUi), "f:after-ui") == 0 && std::strcmp(stepName(kFwUiLayer), "f:ui-layer") == 0,
          "ui-layer split: the new step is named f:after-ui and f:ui-layer keeps its name");
    bool unique = true;
    for (unsigned i = 0; i < kStepCount; ++i)
        for (unsigned j = i + 1; j < kStepCount; ++j) unique = unique && std::strcmp(stepName(i), stepName(j)) != 0;
    check(unique && std::strcmp(stepName(kStepCount), "?") == 0, "ui-layer split: every step has its own name");
    // An ordinary eye draw: scopes, the family decision, the after-UI branch, then on (no composite, so no second f:ui-layer mark).
    FakeEnv::reset();
    FakeEnv::g_sampled = true;
    FakeEnv::g_ordinal = 31;
    FakeEnv::g_time = 1000;
    {
        TestClock c('X');
        FakeEnv::g_time = 1010;
        subMark(kFwScopes);      // 10
        FakeEnv::g_time = 1030;
        subMark(kFwUiLayer);     // 20: the family decision
        FakeEnv::g_time = 1037;
        subMark(kFwAfterUi);     // 7: the after-UI branch
        FakeEnv::g_time = 1040;
        subMark(kFwVerdict);     // 3: after the census test, up to the verdict's Begin
        FakeEnv::g_time = 1050;
    }
    const StepAccum& a = FakeEnv::g_lastRun;
    check(a.ticks[kFwUiLayer] == 20 && a.ticks[kFwAfterUi] == 7 && a.ticks[kFwVerdict] == 3 && a.ticks[kFwScopes] == 10,
          "ui-layer split: the family decision and the after-UI branch are cut apart, each interval to the step that ends it");
    check(a.marks[kFwUiLayer] == 1 && a.marks[kFwAfterUi] == 1, "ui-layer split: an ordinary draw closes each once");
    // A composite draw: the census closes f:ui-layer a second time, after f:after-ui, and its time is f:ui-layer's.
    FakeEnv::reset();
    FakeEnv::g_sampled = true;
    FakeEnv::g_ordinal = 31;
    FakeEnv::g_time = 2000;
    {
        TestClock c('X');
        FakeEnv::g_time = 2010;
        subMark(kFwScopes);
        FakeEnv::g_time = 2030;
        subMark(kFwUiLayer);     // 20
        FakeEnv::g_time = 2037;
        subMark(kFwAfterUi);     // 7
        FakeEnv::g_time = 2045;
        subMark(kFwUiLayer);     // 8: the census
        FakeEnv::g_time = 2048;
        subMark(kFwVerdict);     // 3
        FakeEnv::g_time = 2050;
    }
    const StepAccum& b = FakeEnv::g_lastRun;
    check(b.ticks[kFwUiLayer] == 28 && b.marks[kFwUiLayer] == 2 && b.ticks[kFwAfterUi] == 7 && b.marks[kFwAfterUi] == 1,
          "ui-layer split: a composite draw's census adds its 8 ticks and its mark to f:ui-layer, and f:after-ui keeps its 7");
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
    check(std::strstr(line, "f:ui-layer -, f:after-ui -, f:verdict-begin -, ") != nullptr,
          "text: f:after-ui is in the steps line, between f:ui-layer and f:verdict-begin");
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
        check(std::strstr(a, "f:after-ui 99999.999, ") != nullptr && std::strstr(b, "f:after-ui 99999999, ") != nullptr,
              "text: f:after-ui is in both lines at its widest");
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

// A hit needs something stored, both halves of the key equal AND the stored exit to be a plain decline; every other call is a miss.
void testMemoHit() {
    using namespace edvr::classifiermemo;
    Memo m;
    const Key k{5, 7};
    check(!hit(m, k), "memo: nothing stored is a miss");
    check(!hit(m, Key{0, 0}), "memo: nothing stored is a miss even for the zero key");
    store(m, k, Exit::kPlainDecline);
    check(hit(m, k), "memo: both halves equal after a plain decline is a hit");
    check(!hit(m, Key{6, 7}), "memo: a moved generation is a miss");
    check(!hit(m, Key{5, 8}), "memo: a moved epoch is a miss");
    check(!hit(m, Key{6, 8}), "memo: both moved is a miss");
    check(!hit(m, Key{4, 7}) && !hit(m, Key{5, 6}), "memo: a key that moved backwards is a miss as well, never a hit");
    check(!hit(m, Key{0, 0}), "memo: the zero key does not match a stored one");
    // The stored exit decides what an equal key means.
    store(m, k, Exit::kOther);
    check(!hit(m, k), "memo: an equal key after an exit that was not a plain decline is a miss");
    check(!hit(m, Key{6, 7}) && !hit(m, Key{5, 8}), "memo: a moved key after a non-plain exit is a miss as well");
    store(m, k, Exit::kPlainDecline);
    check(hit(m, k), "memo: storing the plain decline again restores the hit");
    store(m, Key{9, 9}, Exit::kPlainDecline);
    check(!hit(m, k) && hit(m, Key{9, 9}), "memo: a later store replaces the earlier key");
    invalidate(m);
    check(!hit(m, Key{9, 9}) && !m.stored, "memo: invalidate forgets the answer, so the same key misses");
    // Both classifiers keep a memo of their own: storing in one leaves the other alone.
    Memo eye, holo;
    store(eye, k, Exit::kPlainDecline);
    check(!hit(holo, k) && hit(eye, k), "memo: two memos are independent");
    // Over a grid of keys against one stored key, exactly the stored key after a plain decline hits.
    unsigned hits = 0;
    for (uint32_t g = 3; g < 8; ++g) {
        for (uint32_t e = 3; e < 8; ++e) {
            for (int plain = 0; plain < 2; ++plain) {
                Memo c;
                store(c, Key{5, 5}, plain ? Exit::kPlainDecline : Exit::kOther);
                if (hit(c, Key{g, e})) ++hits;
            }
        }
    }
    check(hits == 1, "memo: over a grid of keys and exits exactly one cell is a hit");
}

// A model of a run of draws through the memo, as uiDepthOnEyeDraw drives it: key, hit test, and on a miss the full run's exit stored.
void testMemoRun() {
    using namespace edvr::classifiermemo;
    Memo m;
    unsigned fullRuns = 0;
    const auto call = [&](Key now, Exit exitIfRun) {
        if (hit(m, now)) return true;
        ++fullRuns;
        store(m, now, exitIfRun);
        return false;
    };
    const Key a{10, 3}, b{11, 3}, c{11, 4};
    check(!call(a, Exit::kPlainDecline), "run: the first call finds nothing stored and runs in full");
    check(call(a, Exit::kPlainDecline) && call(a, Exit::kPlainDecline), "run: the repeats hit");
    check(!call(b, Exit::kOther), "run: a draw whose bindings moved runs in full, and its exit is not a plain decline");
    check(!call(b, Exit::kOther), "run: its repeat finds the key but not a plain decline, so it runs in full again");
    check(!call(a, Exit::kPlainDecline), "run: back to the plain draw, the generation has moved");
    check(call(a, Exit::kPlainDecline), "run: ...and it hits again");
    check(!call(c, Exit::kPlainDecline), "run: a bump of both runs in full");
    check(!call(Key{11, 5}, Exit::kPlainDecline), "run: an epoch move alone runs in full");
    check(fullRuns == 6, "run: of nine calls three hit and six ran in full");
}

// ---- Cut G: ui_family_memo.h, the UI layer's family decision (trim F) -----------------------------------------------------------------------
namespace fm = edvr::uifamilymemo;
using edvr::UiLayerFamily;

fm::Key keyOf(uint64_t vs, uint64_t ps, uint32_t generation, uint32_t epoch, bool onFoot) {
    fm::Key k;
    k.vs = vs;
    k.ps = ps;
    k.rtvGeneration = generation;
    k.epoch = epoch;
    k.onFoot = onFoot;
    return k;
}

// A hit needs something stored, a plain decline stored, and every field of the key the stored one's.
void testFamilyMemoKey() {
    fm::Memo m;
    const fm::Key k = keyOf(0xAAAA, 0xBBBB, 7, 3, false);
    check(!fm::hit(m, k) && !fm::hit(m, fm::Key()), "family memo: nothing stored is a miss, the zero key included");
    check(fm::missCause(m) == fm::MissCause::kNothingStored, "family memo: a miss with nothing stored says so");
    fm::store(m, k, fm::Exit::kPlainDecline);
    check(fm::hit(m, k), "family memo: the same key after a plain decline is a hit");
    const fm::Key moved[] = {keyOf(0xAAAB, 0xBBBB, 7, 3, false), keyOf(0xAAAA, 0xBBBC, 7, 3, false), keyOf(0xAAAA, 0xBBBB, 8, 3, false),
                             keyOf(0xAAAA, 0xBBBB, 7, 4, false), keyOf(0xAAAA, 0xBBBB, 7, 3, true),  keyOf(0xAAAA, 0xBBBB, 6, 3, false),
                             keyOf(0xAAAA, 0xBBBB, 7, 2, false)};
    bool allMiss = true, allMoved = true;
    for (const fm::Key& n : moved) {
        allMiss = allMiss && !fm::hit(m, n);
        allMoved = allMoved && fm::missCause(m) == fm::MissCause::kKeyMoved;
    }
    check(allMiss, "family memo: any one field moved (either shader hash, Rtv0's generation, the epoch, the on-foot flag, forwards or back) is a miss");
    check(allMoved, "family memo: a miss with a plain decline standing is 'the key moved'");
    fm::store(m, k, fm::Exit::kOther);
    check(!fm::hit(m, k) && fm::missCause(m) == fm::MissCause::kNothingStored,
          "family memo: the same key after a run that was not a plain decline is a miss, and counts as nothing stored");
    fm::store(m, keyOf(1, 2, 3, 4, true), fm::Exit::kPlainDecline);
    check(!fm::hit(m, k) && fm::hit(m, keyOf(1, 2, 3, 4, true)), "family memo: a later store replaces the earlier key");
    // Over a grid of single-field differences against one stored key, exactly the stored key hits.
    unsigned hits = 0;
    for (uint32_t vs = 0; vs < 3; ++vs)
        for (uint32_t ps = 0; ps < 3; ++ps)
            for (uint32_t g = 0; g < 3; ++g)
                for (uint32_t e = 0; e < 3; ++e)
                    for (int f = 0; f < 2; ++f) {
                        fm::Memo c;
                        fm::store(c, keyOf(1, 1, 1, 1, true), fm::Exit::kPlainDecline);
                        hits += fm::hit(c, keyOf(vs, ps, g, e, f != 0)) ? 1u : 0u;
                    }
    check(hits == 1, "family memo: over a grid of 162 keys against one stored key exactly one hits");
}

// The store rule: a plain decline needs a none answer, a target kind other than 2, a vertex shader that is neither the menu panel's nor the
// loader's, and the on-foot flag still the key's. Nothing else earns a hit.
void testFamilyMemoStoreRule() {
    unsigned plain = 0;
    bool right = true;
    for (int none = 0; none < 2; ++none)
        for (int kind = 0; kind < 3; ++kind)
            for (int probe = 0; probe < 2; ++probe)
                for (int still = 0; still < 2; ++still) {
                    const fm::Exit e = fm::exitOf(none != 0, kind, probe != 0, still != 0);
                    const bool want = none && kind != 2 && !probe && still;
                    right = right && (e == fm::Exit::kPlainDecline) == want;
                    plain += e == fm::Exit::kPlainDecline;
                }
    check(right && plain == 2, "family memo store rule: of 24 outcomes exactly the 2 (none, kind 0 or 1, not panel or loader, flag still) are plain declines");
    check(fm::exitOf(true, 2, false, true) == fm::Exit::kOther, "family memo store rule: the post-tonemap target (kind 2) is never stored");
    check(fm::exitOf(true, 1, true, true) == fm::Exit::kOther && fm::exitOf(true, 0, true, true) == fm::Exit::kOther,
          "family memo store rule: the panel's and the loader's vertex shaders are never stored, at any kind");
    check(fm::exitOf(false, 1, false, true) == fm::Exit::kOther && fm::exitOf(false, 0, false, true) == fm::Exit::kOther,
          "family memo store rule: a run that named a family is never stored");
    check(fm::exitOf(true, 1, false, false) == fm::Exit::kOther, "family memo store rule: a run whose on-foot flag moved under it stores nothing a hit can use");
    check(fm::exitOf(true, 0, false, true) == fm::Exit::kPlainDecline && fm::exitOf(true, 1, false, true) == fm::Exit::kPlainDecline,
          "family memo store rule: none at kind 0 or kind 1, for another vertex shader, is stored");
}

// The facts the kind-0 and kind-1 branches of vscreen.cpp's uiLayerFamilyOf gather, as the code reads them (the wiring test pins the
// transcription against the source): the pixel shader's hash only for the vertex shaders the rule names by a pair.
UiLayerFamily gatheredFamily(int targetKind, uint64_t vs, uint64_t boundPs, bool onFoot) {
    edvr::UiFamilyFacts f;
    f.targetKind = targetKind;
    f.vs = vs;
    f.nativeOnFoot = onFoot;
    if (targetKind == 1 && (vs == edvr::kHoloTargetSphere || vs == edvr::kUiVsOrbitLines || vs == edvr::kUiVsSupercruiseBars || vs == edvr::kUiVsSpaceDust))
        f.ps = boundPs;
    else if (targetKind == 1 && onFoot && edvr::uiOnFootHudVs(vs))
        f.ps = boundPs;
    else if (targetKind == 1 && (edvr::weaponsFireVs(vs) || edvr::shieldRefractVs(vs)))
        f.ps = boundPs;
    return edvr::uiLayerFamilyFor(f);
}

// One draw as the draw path meets it: the shaders, the on-foot flag, the target's kind (with Rtv0's generation and the layer's epoch, the
// kind's keys), and the state a kind-2 decision reads that no key holds.
struct FamilyDraw {
    uint64_t vs = 0, ps = 0;
    bool onFoot = false;
    uint32_t generation = 1, epoch = 0;
    int kind = 1;
    bool external = false;
};

// The uncached decision. A kind-2 answer depends on facts the key cannot hold (the learned surfaces, the exclude list, the panel-size test),
// modelled as one bit that moves between any two draws.
UiLayerFamily uncachedFamily(const FamilyDraw& d, unsigned* probeNotes) {
    if (probeNotes && (d.vs == edvr::kUiVsPanel || d.vs == edvr::kUiVsLoader)) ++*probeNotes;   // uiLayerNoteFamilyProbe counts: a side effect
    if (d.kind == 2) return d.external ? UiLayerFamily::kScreen : UiLayerFamily::kNone;
    return gatheredFamily(d.kind, d.vs, d.ps, d.onFoot);
}

// What a deliberately broken memo does, for the controls: each makes one thing the key or the rule must hold go missing.
enum class Mutant { kNone, kKeyIgnoresPs, kKeyIgnoresVs, kKeyIgnoresGeneration, kKeyIgnoresEpoch, kKeyIgnoresOnFoot, kStoresKind2, kStoresProbeVs, kStoresFamilies };

struct FamilyRun {
    unsigned draws = 0, hits = 0, families = 0, mismatches = 0, noteDiff = 0;
    fm::Counters count;
};

// The thunk's path (vscreen.cpp uiLayerFamilyMemoised and uiLayerFamilyRun): key, hit test, and on a miss the full run, counted by cause,
// with its exit stored. The cached answer is compared with the uncached one for every draw.
struct FamilyModel {
    fm::Memo memo;
    Mutant mutant;
    unsigned notesCached = 0, notesFull = 0;
    explicit FamilyModel(Mutant m) : mutant(m) {}
    fm::Key keyFor(const FamilyDraw& d) const {
        fm::Key k = keyOf(d.vs, d.ps, d.generation, d.epoch, d.onFoot);
        if (mutant == Mutant::kKeyIgnoresPs) k.ps = 0;
        if (mutant == Mutant::kKeyIgnoresVs) k.vs = 0;
        if (mutant == Mutant::kKeyIgnoresGeneration) k.rtvGeneration = 0;
        if (mutant == Mutant::kKeyIgnoresEpoch) k.epoch = 0;
        if (mutant == Mutant::kKeyIgnoresOnFoot) k.onFoot = false;
        return k;
    }
    UiLayerFamily draw(const FamilyDraw& d, FamilyRun& run) {
        ++run.draws;
        ++run.count.calls;
        UiLayerFamily answer;
        const fm::Key key = keyFor(d);
        if (fm::hit(memo, key)) {
            ++run.hits;
            ++run.count.hits;
            answer = UiLayerFamily::kNone;
        } else {
            if (fm::missCause(memo) == fm::MissCause::kKeyMoved) ++run.count.missMoved;
            else ++run.count.missNothing;
            answer = uncachedFamily(d, &notesCached);
            const bool none = answer == UiLayerFamily::kNone;
            const bool probeVs = d.vs == edvr::kUiVsPanel || d.vs == edvr::kUiVsLoader;
            fm::Exit exit = fm::exitOf(none, d.kind, probeVs, true);
            if (mutant == Mutant::kStoresKind2 && none && d.kind == 2) exit = fm::Exit::kPlainDecline;
            if (mutant == Mutant::kStoresProbeVs && none && probeVs && d.kind != 2) exit = fm::Exit::kPlainDecline;
            if (mutant == Mutant::kStoresFamilies && !none) exit = fm::Exit::kPlainDecline;
            fm::store(memo, key, exit);
        }
        const UiLayerFamily full = uncachedFamily(d, &notesFull);
        // The verification the DLL does on a hit: the same comparison, for every draw here.
        if (answer != full) ++run.mismatches;
        run.families += full != UiLayerFamily::kNone;
        return answer;
    }
};

// The named shaders. Every vertex shader the family rule or the gather names, the pixel shaders of every pair, and two unnamed of each.
std::vector<uint64_t> namedVertexShaders() {
    std::vector<uint64_t> v = {edvr::kUiVsPanel, edvr::kUiVsLoader, edvr::kUiVsHolo, edvr::kUiVsHoloGuiFxOff, edvr::kUiVsFlightHud, edvr::kUiVsSprite,
                               edvr::kUiVsGuiVector, edvr::kUiVsGuiText, edvr::kUiVsGuiIcons, edvr::kUiVsOrbitLines, edvr::kUiVsSupercruiseBars,
                               edvr::kUiVsSpaceDust, edvr::kHoloTargetSphere, edvr::kUiVsShieldRefract, edvr::kUiVsOnFootHudA, edvr::kUiVsOnFootHudB,
                               0x0ull, 0x1234567890ABCDEFull};
    for (uint64_t h : edvr::kHoloFamiliesTake) v.push_back(h);
    for (size_t i = 0; i < edvr::kWeaponPairCount; ++i) v.push_back(edvr::kWeaponPairs[i].vs);
    return v;
}
std::vector<uint64_t> namedPixelShaders() {
    std::vector<uint64_t> v = {edvr::kUiPsOrbitLines, edvr::kUiPsSupercruiseBars, edvr::kUiPsSpaceDust, edvr::kUiPsShieldRefract,
                               0xEA02FAC2BD6C643Cull, 0xE95634B0F61D218Full,   // the target sphere's two exact pixel shaders
                               0x067CBE05E7EF7F32ull, 0xCF4ECC0397199145ull, 0xF10792B40AE3ED42ull, 0xAD050E528C0E8B17ull, 0xEFDF9BCB97E166A1ull,   // on-foot HUD
                               0x0ull, 0xDEADBEEFull};
    for (size_t i = 0; i < edvr::kWeaponPairCount; ++i) v.push_back(edvr::kWeaponPairs[i].ps);
    for (uint64_t h : edvr::kUiPanelPs) v.push_back(h);
    for (uint64_t h : edvr::kUiLoaderPs) v.push_back(h);
    return v;
}

// A small deterministic generator: the sweep needs the same sequences on every run.
struct Lcg {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    uint32_t below(uint32_t n) { return next() % n; }
};

// Every named vertex shader against every named pixel shader, both flags, target kinds 0 and 1 (the kinds the key decides): each draw three
// times in a row through one memo, so the first runs in full and the repeats hit exactly where the uncached answer is a plain decline.
FamilyRun sweepAllPairs(Mutant mutant, unsigned* storableRepeatsMissed) {
    FamilyRun run;
    FamilyModel model(mutant);
    const std::vector<uint64_t> vss = namedVertexShaders(), pss = namedPixelShaders();
    uint32_t generation = 1;
    for (int kind = 0; kind < 2; ++kind)
        for (int flag = 0; flag < 2; ++flag)
            for (uint64_t vs : vss)
                for (uint64_t ps : pss) {
                    FamilyDraw d;
                    d.vs = vs;
                    d.ps = ps;
                    d.onFoot = flag != 0;
                    d.kind = kind;
                    d.generation = generation + static_cast<uint32_t>(kind);   // a target of another kind is another generation: the kind is a function of it
                    d.epoch = 0;
                    const unsigned hitsBefore = run.hits;
                    model.draw(d, run);
                    const bool storable = uncachedFamily(d, nullptr) == UiLayerFamily::kNone && vs != edvr::kUiVsPanel && vs != edvr::kUiVsLoader;
                    model.draw(d, run);   // the repeats: hits exactly when the first run was a plain decline
                    model.draw(d, run);
                    if (storableRepeatsMissed && storable && run.hits < hitsBefore + 2) ++*storableRepeatsMissed;
                }
    run.noteDiff = model.notesCached == model.notesFull ? 0u : 1u;
    return run;
}

// Random sequences of draws with the key's own events: a draw repeats the last one, changes a shader, flips the on-foot flag, moves Rtv0 (a new
// generation, with a new target kind), or resets the layer's target cache (the epoch, with a new kind); and the state only a kind-2 decision reads
// moves between any two draws.
FamilyRun sweepSequences(Mutant mutant, uint32_t seed, unsigned draws) {
    FamilyRun run;
    FamilyModel model(mutant);
    const std::vector<uint64_t> vss = namedVertexShaders(), pss = namedPixelShaders();
    Lcg rng{seed};
    FamilyDraw d;
    d.vs = vss[0];
    d.ps = pss[0];
    for (unsigned i = 0; i < draws; ++i) {
        const uint32_t e = rng.below(100);
        if (e < 35) {
            // the same bindings again
        } else if (e < 55) {
            d.ps = pss[rng.below(static_cast<uint32_t>(pss.size()))];
        } else if (e < 75) {
            d.vs = vss[rng.below(static_cast<uint32_t>(vss.size()))];
            d.ps = pss[rng.below(static_cast<uint32_t>(pss.size()))];
        } else if (e < 85) {
            d.onFoot = !d.onFoot;
        } else if (e < 95) {
            ++d.generation;
            d.kind = static_cast<int>(rng.below(3));
        } else {
            ++d.epoch;
            d.kind = static_cast<int>(rng.below(3));
        }
        d.external = rng.below(3) == 0;
        model.draw(d, run);
    }
    run.noteDiff = model.notesCached == model.notesFull ? 0u : 1u;
    return run;
}

void testFamilyMemoSweep() {
    unsigned missed = 0;
    const FamilyRun all = sweepAllPairs(Mutant::kNone, &missed);
    check(all.mismatches == 0 && all.noteDiff == 0, "family sweep: every named vertex shader and pixel shader, kinds 0 and 1, the flag both ways: the cached answer is the uncached one");
    check(missed == 0, "family sweep: every repeat of a plain decline hit, so the memo is not trivially empty");
    check(all.families > 0 && all.hits * 3 > all.draws, "family sweep: the pairs include family answers (they never hit) and plain declines (their repeats do)");
    const FamilyRun seq = sweepSequences(Mutant::kNone, 12345, 400000);
    check(seq.mismatches == 0 && seq.noteDiff == 0, "family sweep: 400000 random draws through generation, epoch, kind, flag and shader changes: cached equals uncached every time");
    check(seq.hits > 20000 && seq.families > 5000, "family sweep: the random run met both hits and family answers in numbers");
    check(seq.count.calls == seq.count.hits + seq.count.missNothing + seq.count.missMoved && seq.count.calls == seq.draws &&
              seq.count.missNothing > 0 && seq.count.missMoved > 0,
          "family sweep: the counters add up (calls = hits + both kinds of miss), both miss causes occur");
    std::printf("aa_off_test: family sweep: %u pair draws (%u hits), %u random draws (%u hits, %u of them family answers), 0 mismatches\n",
                all.draws, all.hits, seq.draws, seq.hits, seq.families);
    // Controls: each fault makes the sweep fail, so the sweep can fail.
    const Mutant mutants[] = {Mutant::kKeyIgnoresPs,    Mutant::kKeyIgnoresVs,   Mutant::kKeyIgnoresGeneration, Mutant::kKeyIgnoresEpoch,
                              Mutant::kKeyIgnoresOnFoot, Mutant::kStoresKind2,    Mutant::kStoresProbeVs,        Mutant::kStoresFamilies};
    const char* const names[] = {"the shader PS missing from the key", "the VS missing from the key", "Rtv0's generation missing from the key",
                                 "the epoch missing from the key", "the on-foot flag missing from the key", "kind 2 stored",
                                 "the panel's and the loader's vertex shaders stored", "a family answer stored"};
    for (size_t i = 0; i < sizeof(mutants) / sizeof(mutants[0]); ++i) {
        unsigned ignored = 0;
        const FamilyRun a = sweepAllPairs(mutants[i], &ignored);
        const FamilyRun b = sweepSequences(mutants[i], 12345, 400000);
        char what[160];
        std::snprintf(what, sizeof(what), "family sweep control: with %s the sweep fails (a wrong family or a missed census note)", names[i]);
        check(a.mismatches + a.noteDiff + b.mismatches + b.noteDiff > 0, what);
    }
}

// The verify frames, and the window line.
void testFamilyMemoVerification() {
    unsigned verify = 0, verifyOnSampleCadence = 0, verifyNow = 0;
    bool phases[16] = {};
    bool formula = true;
    for (uint64_t n = 0; n < 256; ++n) {
        const bool v = fm::verifyFrame(n);
        formula = formula && v == ((n + n / 16) % 16 == 8);
        verify += v;
        if (v) {
            phases[n % 16] = true;
            verifyOnSampleCadence += (n % 16) == 0;   // the perf monitor samples draws on frames 0 mod 16
        }
        verifyNow += fm::verifyNow(n, (n % 16) == 0);
    }
    unsigned distinct = 0;
    for (bool p : phases) distinct += p;
    check(formula && verify == 16, "family verification: one frame in sixteen verifies, by (n + n/16) % 16 == 8");
    check(distinct == 16, "family verification: over 256 frames the verify frames meet every phase of the 16-frame cadence");
    check(verifyOnSampleCadence == 1 && verifyNow == 15, "family verification: one verify frame in 256 lands on a sampled frame, and a hit there is not verified");
    check(!fm::verifyNow(8, true) && fm::verifyNow(8, false) && !fm::verifyNow(9, false), "family verification: a sampled frame never verifies; frame 8 does when it is not sampled");
    check(fm::verifyFrame(8) && !fm::verifyFrame(24) && fm::verifyFrame(23), "family verification: the phase slips by one every 16 frames (8, then 23, ...)");
    check(fm::verifyFrame(0xFFFFFFFFull + 17) == (((0xFFFFFFFFull + 17) + (0xFFFFFFFFull + 17) / 16) % 16 == 8),
          "family verification: the frame count is 64 bits wide, so the phase arithmetic does not wrap");

    // The line, at its worst: every counter at the 64-bit maximum, the frame at its 32-bit maximum.
    fm::Counters worst;
    worst.calls = worst.hits = worst.missNothing = worst.missMoved = worst.verified = worst.mismatches = 18446744073709551615ull;
    char line[2000];
    const size_t n = fm::formatLine(line, sizeof(line), 4294967295u, worst);
    std::printf("aa_off_test: ui layer family cache line at its worst: %zu characters (limit 1150; the log holds about 1170)\n", n);
    check(n == std::strlen(line) && n < 1150 && line[n - 1] == '.', "family line: at its worst it fits the log line (under 1150 characters) and ends whole");
    check(std::strncmp(line, "ui layer family cache: ", 23) == 0, "family line: it starts 'ui layer family cache:'");
    fm::Counters some;
    some.calls = 100;
    some.hits = 60;
    some.missNothing = 25;
    some.missMoved = 15;
    some.verified = 4;
    some.mismatches = 1;
    fm::formatLine(line, sizeof(line), 5400, some);
    check(std::strstr(line, "window ending 5400") != nullptr && std::strstr(line, "decision: 100;") != nullptr &&
              std::strstr(line, "from the memo: 60;") != nullptr && std::strstr(line, "ran in full: 40 (") != nullptr &&
              std::strstr(line, "last run was not a plain decline 25, the key moved 15)") != nullptr &&
              std::strstr(line, "4 hits also ran the full decision, 1 of them named a family") != nullptr,
          "family line: it says the calls, the hits, the two kinds of miss, the verified hits and the mismatches");
    fm::formatLine(line, sizeof(line), 5400, fm::Counters());
    check(std::strstr(line, "decision: 0;") != nullptr && std::strstr(line, "1 of them") == nullptr,
          "family line: a window with no calls is written, as zeros (the line's absence would mean a build without it)");
    char tiny[24];
    std::memset(tiny, 'x', sizeof(tiny));
    const size_t nt = fm::formatLine(tiny, sizeof(tiny), 5400, some);
    check(nt < sizeof(tiny) && tiny[nt] == '\0', "family line: a short buffer is cut and terminated, never overrun");
    char one[1] = {'x'};
    check(fm::formatLine(one, 1, 5400, some) == 0 && one[0] == '\0', "family line: a one-byte buffer holds the terminator alone");
}

// The source, read from the repo root: the model above transcribes vscreen.cpp's gather, so this pins the transcription; and the epoch is
// bumped at every reset of the target-kind cache the key depends on.
std::string slurpFile(const char* path) {
    std::string out;
    if (std::FILE* f = std::fopen(path, "rb")) {
        char buf[8192];
        size_t got;
        while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);
        std::fclose(f);
    }
    return out;
}
// The code only, whitespace gone: // and /* */ comments removed first, so a comment can neither satisfy nor spoil a pin.
std::string squeezedCode(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (text.compare(i, 2, "/*") == 0) {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 2;
        } else {
            if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') out.push_back(text[i]);
            ++i;
        }
    }
    return out;
}
size_t countOf(const std::string& s, const std::string& part) {
    size_t n = 0;
    for (size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + part.size())) ++n;
    return n;
}
bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

void testFamilyMemoWiring() {
    const std::string vscreen = slurpFile("src/d3d11/vscreen.cpp"), layer = slurpFile("src/d3d11/ui_layer.cpp");
    check(vscreen.size() > 100000 && layer.size() > 100000, "family wiring: vscreen.cpp and ui_layer.cpp are readable from the repo root");
    const std::string head = "__declspec(noinline) UiLayerFamily uiLayerFamilyOf(State* s, char kind, UINT count) {";
    const size_t fn = vscreen.find(head);
    check(fn != std::string::npos, "family wiring: uiLayerFamilyOf is in vscreen.cpp");
    if (fn == std::string::npos) return;
    const size_t kind2 = vscreen.find("    if (f.targetKind == 2) {\n", fn);
    const size_t end = vscreen.find("\n}\n", fn);
    check(kind2 != std::string::npos && end != std::string::npos && kind2 < end, "family wiring: the kind-2 branch is inside uiLayerFamilyOf");
    if (kind2 == std::string::npos || end == std::string::npos) return;
    const std::string below = squeezedCode(vscreen.substr(fn + head.size(), kind2 - fn - head.size()));
    const std::string kind2Block = squeezedCode(vscreen.substr(kind2, end - kind2));
    // Below kind 2 the decision reads: the target kind, the quality gate (kind 2's early return), the vertex shader's hash, the on-foot flag, and
    // the pixel shader's hash only where the rule names a pair -- the three conditions the model transcribes.
    check(contains(below, "f.targetKind=uiLayerTargetKind();") && contains(below, "if(f.targetKind==2&&!uiLayerQualityLive())returnUiLayerFamily::kNone;") &&
              contains(below, "f.vs=bindingShaderHash(BindSlot::Vs);") && contains(below, "f.nativeOnFoot=nativeOnFootSharpHudWanted();"),
          "family wiring: below kind 2 the gather reads the target kind, the vertex shader's hash and the on-foot flag");
    check(contains(below, "if(f.targetKind==1&&(f.vs==kHoloTargetSphere||f.vs==kUiVsOrbitLines||f.vs==kUiVsSupercruiseBars||f.vs==kUiVsSpaceDust))f.ps=bindingShaderHash(BindSlot::Ps);") &&
              contains(below, "elseif(f.targetKind==1&&f.nativeOnFoot&&uiOnFootHudVs(f.vs))f.ps=bindingShaderHash(BindSlot::Ps);") &&
              contains(below, "elseif(f.targetKind==1&&(weaponsFireVs(f.vs)||shieldRefractVs(f.vs)))f.ps=bindingShaderHash(BindSlot::Ps);") &&
              countOf(below, "bindingShaderHash(BindSlot::Ps)") == 3,
          "family wiring: the pixel shader's hash is read in exactly the three places the sweep's gather reads it");
    // Every call below kind 2 is one of those; anything else would be a new input the key does not hold.
    static const char* const kAllowed[] = {"uiLayerTargetKind", "uiLayerQualityLive", "bindingShaderHash", "nativeOnFootSharpHudWanted", "uiOnFootHudVs",
                                           "weaponsFireVs", "shieldRefractVs", "if", "elseif"};
    std::string stray;
    for (size_t i = 0; i < below.size(); ++i) {
        if (below[i] != '(') continue;
        size_t b = i;
        while (b > 0 && (std::isalnum(static_cast<unsigned char>(below[b - 1])) || below[b - 1] == '_')) --b;
        const std::string name = below.substr(b, i - b);
        if (name.empty()) continue;
        bool known = false;
        for (const char* a : kAllowed) known = known || name == a;
        if (!known) stray += name + " ";
    }
    check(stray.empty(), "family wiring: below kind 2 the gather calls nothing but the key's inputs (a stray call would be an input the memo's key lacks)");
    if (!stray.empty()) std::printf("aa_off_test: stray calls below kind 2: %s\n", stray.c_str());
    // The state a kind-2 decision reads lives in the kind-2 block alone.
    check(contains(kind2Block, "uiDepthIsExcluded(") && contains(kind2Block, "srv0IsPanelSized(") && contains(kind2Block, "uiDepthSampledSurfaceSlot(") &&
              !contains(below, "uiDepthIsExcluded(") && !contains(below, "srv0IsPanelSized(") && !contains(below, "uiDepthSampledSurfaceSlot("),
          "family wiring: ui_depth's exclude list, the panel-size test and the learned surfaces are read in the kind-2 block alone (the memo never stores kind 2)");
    const std::string whole = squeezedCode(vscreen.substr(fn, end - fn));
    check(countOf(whole, "uiLayerNoteFamilyProbe(") == 1 && contains(whole, "if(f.vs==kUiVsPanel||f.vs==kUiVsLoader){if(why==UiFamilyWhy::kNoSurface&&!f.ps)f.ps=bindingShaderHash(BindSlot::Ps);uiLayerNoteFamilyProbe("),
          "family wiring: the family census is fed for the panel's and the loader's vertex shaders alone (the memo never stores those)");
    // The memo's own wiring in vscreen.cpp.
    const std::string code = squeezedCode(vscreen);
    check(countOf(code, "uiLayerFamilyOf(") == 3 &&
              contains(code, "constUiLayerFamilyfull=uiLayerFamilyOf(s,kind,count);++m.count.verified;") &&
              contains(code, "constUiLayerFamilyfamily=uiLayerFamilyOf(s,kind,count);constboolnone=family==UiLayerFamily::kNone;") &&
              contains(code, "constUiLayerFamilyuiFamily=uiLayerFamilyMemoised(g_state,kind,count);"),
          "family wiring: uiLayerFamilyOf is called from the memo's miss path and its verification alone, and the draw path asks the memo");
    check(contains(code, "constboolprobeVs=key.vs==kUiVsPanel||key.vs==kUiVsLoader;") && contains(code, "constboolcandidate=none&&!probeVs;") &&
              contains(code, "constboolflagStill=!candidate||nativeOnFootSharpHudWanted()==key.onFoot;") &&
              contains(code, "uifamilymemo::store(m.memo,key,uifamilymemo::exitOf(none,targetKind,probeVs,flagStill));") &&
              contains(code, "constinttargetKind=candidate&&flagStill?uiLayerTargetKind():0;"),
          "family wiring: the run stores through exitOf, with the panel and loader test, the on-foot flag re-read and the target kind read after the run");
    check(contains(code, "key.vs=bindingShaderHash(BindSlot::Vs);key.ps=bindingShaderHash(BindSlot::Ps);key.rtvGeneration=bindingGeneration(BindSlot::Rtv0);key.epoch=uiLayerFamilyEpoch();key.onFoot=nativeOnFootSharpHudWanted();"),
          "family wiring: the key is the two shader hashes, Rtv0's generation, the layer's epoch and the on-foot flag");
    check(contains(code, "if(m.verifyFrame&&!perfMonitorSampleDraws())returnuiLayerFamilyVerifyHit(s,kind,count,key);") &&
              contains(code, "++g_uiFamilyMemo.frames;g_uiFamilyMemo.verifyFrame=uifamilymemo::verifyFrame(g_uiFamilyMemo.frames);"),
          "family wiring: a hit is verified on a verify frame off a sampled frame, and the frame clock advances at the frame boundary");
    // The epoch: every reset of the target-kind cache but its own refresh is followed by a bump, and the layer's liveness refresh bumps it.
    const std::string lcode = squeezedCode(layer);
    check(countOf(lcode, "g_tc=TargetCache{};") == 3 && countOf(lcode, "g_tc=TargetCache{};++detail::g_uiLayerFamilyEpoch;") == 2 &&
              contains(lcode, "if(gen==g_tc.gen)returng_tc.kind;g_tc=TargetCache{};g_tc.gen=gen;") && countOf(lcode, "++detail::g_uiLayerFamilyEpoch;") == 3 &&
              contains(lcode, "detail::g_uiLayerCrispOn=detail::g_uiLayerLive&&!g_crispStoodDown;++detail::g_uiLayerFamilyEpoch;}"),
          "family wiring: ui_layer.cpp resets the target-kind cache in three places, bumps the epoch after the two that are not its own refresh, and in refreshLive");
}

// ---- Cut H: draw_fast_path_shadow.h, the repeat-draw fast path's Phase 1 shadow instrument (TEMPORARY, built 2026-10-27) --------------------
namespace fsh = edvr::fastshadow;

fsh::Key shadowKey(uint32_t bind, uint32_t classifier, uint32_t family) {
    fsh::Key k;
    k.bind = bind;
    k.classifier = classifier;
    k.family = family;
    return k;
}

// The class of draw the fast path would take: kind X, a count over 64 and up to 10000 but not 240, at least one instance.
void testShadowArgsClass() {
    check(!fsh::argsClass('X', 64, 1) && fsh::argsClass('X', 65, 1), "shadow args class: 64 is out and 65 is in");
    check(fsh::argsClass('X', 239, 1) && !fsh::argsClass('X', 240, 1) && fsh::argsClass('X', 241, 1), "shadow args class: 240 (night vision) alone is out");
    check(fsh::argsClass('X', 10000, 1) && !fsh::argsClass('X', 10001, 1), "shadow args class: 10000 is in and 10001 (the sun-glare nomination) is out");
    check(!fsh::argsClass('X', 500, 0) && fsh::argsClass('X', 500, 1) && fsh::argsClass('X', 500, 64), "shadow args class: no instances is out, one or more is in");
    check(!fsh::argsClass('N', 500, 1) && !fsh::argsClass('D', 500, 1) && !fsh::argsClass('I', 500, 1) && !fsh::argsClass('Z', 500, 1),
          "shadow args class: only kind X");
    check(!fsh::argsClass('X', 0, 1) && !fsh::argsClass('X', 6, 1) && !fsh::argsClass('X', 3, 1), "shadow args class: the quads and the small draws are out");
    unsigned in = 0;
    for (uint32_t count = 0; count <= 12000; ++count) in += fsh::argsClass('X', count, 1) ? 1u : 0u;
    check(in == 9935, "shadow args class: of the counts 0 to 12000 exactly 65 to 10000 except 240 are in");
}

// What each stage of the eligibility test has gathered.
struct ShadowCalls {
    int dyn = 0, armed = 0;
};

fsh::Verdict shadowJudge(bool ticket, bool keyEqual, bool args, uint32_t dyn, uint32_t armed, uint32_t wanting, ShadowCalls* calls) {
    fsh::Tracker t;
    const fsh::Key now = shadowKey(5, 6, 7);
    t.ticket.valid = ticket;
    t.ticket.key = keyEqual ? now : shadowKey(5, 6, 8);
    return t.judge(now, args, wanting, [&] { ++calls->dyn; return dyn; }, [&] { ++calls->armed; return armed; });
}

// The refusal reason is the first that applies: no ticket, key moved, args class, a dynamic flag, armed, not certified.
void testShadowRefusalOrder() {
    unsigned wrong = 0, takes = 0;
    for (unsigned bits = 0; bits < 64; ++bits) {
        const bool ticket = (bits & 1) != 0, keyEqual = (bits & 2) != 0, args = (bits & 4) != 0;
        const uint32_t dyn = (bits & 8) ? 1u << fsh::kDynWorldWants : 0u;
        const uint32_t armed = (bits & 16) ? 1u << fsh::kArmCensus : 0u;
        const uint32_t wanting = (bits & 32) ? fsh::kSubCensus : fsh::kSubUiDepth;   // an uncertified subscriber, or a certified one
        ShadowCalls calls;
        const fsh::Verdict v = shadowJudge(ticket, keyEqual, args, dyn, armed, wanting, &calls);
        fsh::Reason want = fsh::Reason::kTake;
        if (!ticket) want = fsh::Reason::kNoTicket;
        else if (!keyEqual) want = fsh::Reason::kKeyMoved;
        else if (!args) want = fsh::Reason::kArgsClass;
        else if (dyn) want = fsh::Reason::kDynamic;
        else if (armed) want = fsh::Reason::kArmed;
        else if (bits & 32) want = fsh::Reason::kNotCertified;
        wrong += v.reason != want;
        takes += v.reason == fsh::Reason::kTake;
    }
    check(wrong == 0, "shadow refusal order: over all 64 combinations the reason is the first of ticket, key, args class, dynamic flag, armed, certified");
    check(takes == 1, "shadow refusal order: a draw takes only with the ticket, the key, the args class, no flag, nothing armed and a certified set (1 of 64)");
    ShadowCalls c;
    shadowJudge(false, true, true, 1, 1, fsh::kSubCensus, &c);
    check(c.dyn == 0 && c.armed == 0, "shadow refusal order: no ticket gathers nothing");
    c = ShadowCalls();
    shadowJudge(true, false, true, 1, 1, fsh::kSubCensus, &c);
    check(c.dyn == 0 && c.armed == 0, "shadow refusal order: a moved key gathers nothing");
    c = ShadowCalls();
    shadowJudge(true, true, false, 1, 1, fsh::kSubCensus, &c);
    check(c.dyn == 0 && c.armed == 0, "shadow refusal order: an args class miss gathers nothing");
    c = ShadowCalls();
    shadowJudge(true, true, true, 1, 1, fsh::kSubCensus, &c);
    check(c.dyn == 1 && c.armed == 0, "shadow refusal order: a dynamic flag stops before the armed flags are gathered");
    c = ShadowCalls();
    shadowJudge(true, true, true, 0, 1, fsh::kSubCensus, &c);
    check(c.dyn == 1 && c.armed == 1, "shadow refusal order: an armed flag is gathered after the dynamic ones are clear");
    // Which flag: the verdict carries the masks, and the counters name the first one set.
    fsh::Tracker t;
    t.ticket.valid = true;
    t.ticket.key = shadowKey(1, 1, 1);
    ShadowCalls calls;
    const fsh::Verdict dv = t.judge(shadowKey(1, 1, 1), true, 0, [&] { ++calls.dyn; return (1u << fsh::kDynCrispPending) | (1u << fsh::kDynProbeChecks); },
                                    [&] { ++calls.armed; return 0u; });
    t.finish(dv, fsh::Class::kEye, 0, true, shadowKey(1, 1, 1));
    check(dv.reason == fsh::Reason::kDynamic && t.count.dynBy[fsh::kDynCrispPending] == 1 && t.count.dynBy[fsh::kDynProbeChecks] == 0 &&
              t.count.refused[static_cast<size_t>(fsh::Reason::kDynamic)] == 1,
          "shadow refusal order: a dynamic refusal is counted under its first flag");
    t.ticket.valid = true;
    t.ticket.key = shadowKey(1, 1, 1);
    const fsh::Verdict av = t.judge(shadowKey(1, 1, 1), true, 0, [] { return 0u; }, [] { return (1u << fsh::kArmPixel) | (1u << fsh::kArmLoaderFrame); });
    t.finish(av, fsh::Class::kOff, 0, true, shadowKey(1, 1, 1));
    check(av.reason == fsh::Reason::kArmed && t.count.armedBy[fsh::kArmPixel] == 1 && t.count.armedBy[fsh::kArmLoaderFrame] == 0,
          "shadow refusal order: an armed refusal is counted under its first flag");
}

// The certified set: a wanting subscriber outside it turns the would-take verdict off by itself; one inside it does not.
void testShadowCertified() {
    check(fsh::certifiedOk(0), "shadow certified: with nothing wanting, nothing acts");
    check((fsh::kCertified & ~fsh::kSubscriberAll) == 0 && fsh::kSubscriberAll == (1u << 28) - 1, "shadow certified: the set names only the gate's 28 subscribers");
    unsigned certified = 0, uncertified = 0;
    for (unsigned i = 0; i < fsh::kSubscriberCount; ++i) {
        const uint32_t bit = 1u << i;
        (fsh::certifiedOk(bit) ? certified : uncertified) += 1;
        check(fsh::certifiedOk(bit) == ((fsh::kCertified & bit) != 0), "shadow certified: one subscriber is certified exactly when it is in the set");
    }
    check(certified == 18 && uncertified == 10, "shadow certified: 18 of the 28 subscribers are certified and 10 are not");
    check(fsh::certifiedOk(fsh::kSubFootprint) && fsh::certifiedOk(fsh::kSubLoaderPanel) && fsh::certifiedOk(fsh::kSubIntroPanel) && fsh::certifiedOk(fsh::kSubWakePulse),
          "shadow certified: the subscribers that are wanting in the shipped VR defaults (footprint at 'auto', the loader panel, the intro movie, the wake pulse) are certified");
    check(fsh::certifiedOk(fsh::kSubDistance) && fsh::certifiedOk(fsh::kSubPanelCurve) && fsh::certifiedOk(fsh::kSubDistance | fsh::kSubPanelCurve | fsh::kSubUiDepth),
          "shadow certified: the panel distance and the curved screen, which a player leaves on, are certified (the args class excludes everything they act on)");
    check(fsh::certifiedOk(fsh::kSubUiDepth | fsh::kSubParticles | fsh::kSubScrim | fsh::kSubSunglare | fsh::kSubHolo | fsh::kSubObjectProbe),
          "shadow certified: the design's own list (interface depth, particles, scrim, sun glare, hologram, the object probe) is certified together");
    check(!fsh::certifiedOk(fsh::kSubUiDepth | fsh::kSubCensus) && !fsh::certifiedOk(fsh::kSubCensusSkip) && !fsh::certifiedOk(fsh::kSubFssRes) &&
              !fsh::certifiedOk(fsh::kSubBlendProbe) && !fsh::certifiedOk(fsh::kSubQuadProbe) && !fsh::certifiedOk(fsh::kSubSpaceDust),
          "shadow certified: one uncertified subscriber among certified ones makes the set uncertified");
    check(!fsh::certifiedOk(1u << 28) && !fsh::certifiedOk(0x80000000u), "shadow certified: a bit the set has never heard of is not certified (a new subscriber turns the path off by itself)");
}

// The ticket: written after every owner X draw, valid only for an ordinary draw in the args class that knows its class.
void testShadowTicketRules() {
    check(fsh::ticketValid(0, true, fsh::Class::kEye) && fsh::ticketValid(0, true, fsh::Class::kOff), "shadow ticket: an ordinary draw in the class records, eye or off-screen");
    unsigned invalid = 0;
    for (unsigned i = 0; i < fsh::kTermCount; ++i) invalid += !fsh::ticketValid(1u << i, true, fsh::Class::kEye) ? 1u : 0u;
    check(invalid == fsh::kTermCount, "shadow ticket: any one of the 25 outcome terms not ordinary invalidates it");
    check(!fsh::ticketValid(0, false, fsh::Class::kEye), "shadow ticket: a draw outside the args class does not record");
    check(!fsh::ticketValid(0, true, fsh::Class::kUnresolved), "shadow ticket: a draw whose class is unknown does not record");
    check(fsh::offscreenTerms(fsh::Class::kOff, true, false) == 0 && fsh::offscreenTerms(fsh::Class::kOff, false, false) == fsh::termBit(fsh::kTermUnsettled) &&
              fsh::offscreenTerms(fsh::Class::kOff, true, true) == fsh::termBit(fsh::kTermWake) &&
              fsh::offscreenTerms(fsh::Class::kOff, false, true) == (fsh::termBit(fsh::kTermUnsettled) | fsh::termBit(fsh::kTermWake)),
          "shadow ticket: an off-screen draw's two terms are unsettled and wake surface");
    check(fsh::offscreenTerms(fsh::Class::kEye, false, true) == 0 && fsh::offscreenTerms(fsh::Class::kUnresolved, false, true) == 0,
          "shadow ticket: an eye draw, or one of unknown class, has neither off-screen term");
    check(fsh::ticketValid(fsh::offscreenTerms(fsh::Class::kOff, true, false), true, fsh::Class::kOff) &&
              !fsh::ticketValid(fsh::offscreenTerms(fsh::Class::kOff, false, false), true, fsh::Class::kOff) &&
              !fsh::ticketValid(fsh::offscreenTerms(fsh::Class::kOff, true, true), true, fsh::Class::kOff) &&
              fsh::ticketValid(fsh::offscreenTerms(fsh::Class::kEye, false, true), true, fsh::Class::kEye),
          "shadow ticket: off-screen records only when settled and not a wake surface; an eye draw is not asked");
    // Through the tracker: the key and class written are the draw's, on a full draw and on a would-take draw alike.
    fsh::Tracker t;
    const fsh::Verdict full;   // no ticket yet: a full-path draw
    t.finish(full, fsh::Class::kEye, 0, true, shadowKey(10, 2, 3));
    check(t.ticket.valid && t.ticket.cls == fsh::Class::kEye && fsh::sameKey(t.ticket.key, shadowKey(10, 2, 3)) && t.count.tickets == 1 &&
              t.count.draws[0] == 1 && t.count.refused[static_cast<size_t>(fsh::Reason::kNoTicket)] == 1,
          "shadow ticket: a full-path ordinary draw records its key and class and is counted as refused for no ticket");
    fsh::Verdict take = t.judge(shadowKey(10, 2, 3), true, fsh::kSubUiDepth, [&] { return 0u; }, [&] { return 0u; });
    check(take.reason == fsh::Reason::kTake, "shadow ticket: the next draw with the same key would take");
    t.finish(take, fsh::Class::kEye, 0, true, shadowKey(10, 2, 3));
    check(t.ticket.valid && t.count.tickets == 2 && t.count.take[0] == 1 && t.count.compared == 1 && t.count.mismatchDraws == 0,
          "shadow ticket: a would-take ordinary draw records again, is compared, and matches");
    take = t.judge(shadowKey(10, 2, 3), true, 0, [] { return 0u; }, [] { return 0u; });
    t.finish(take, fsh::Class::kEye, fsh::termBit(fsh::kTermFamily), true, shadowKey(10, 2, 3));
    check(!t.ticket.valid && t.count.tickets == 2, "shadow ticket: a non-ordinary draw invalidates the ticket and records nothing");
    const fsh::Verdict after = t.judge(shadowKey(10, 2, 3), true, 0, [] { return 0u; }, [] { return 0u; });
    check(after.reason == fsh::Reason::kNoTicket, "shadow ticket: the draw after a non-ordinary one has no ticket");
    t.finish(after, fsh::Class::kOff, fsh::offscreenTerms(fsh::Class::kOff, false, false), true, shadowKey(11, 2, 3));
    check(!t.ticket.valid, "shadow ticket: an off-screen draw whose target is unsettled does not record");
    t.finish(after, fsh::Class::kOff, fsh::offscreenTerms(fsh::Class::kOff, true, true), true, shadowKey(11, 2, 3));
    check(!t.ticket.valid, "shadow ticket: an off-screen draw on a wake-pulse surface does not record");
    t.finish(after, fsh::Class::kOff, fsh::offscreenTerms(fsh::Class::kOff, true, false), true, shadowKey(11, 2, 3));
    check(t.ticket.valid && t.ticket.cls == fsh::Class::kOff && t.ticket.key.bind == 11, "shadow ticket: a settled off-screen draw that is not a wake surface records, with its class");
    t.finish(after, fsh::Class::kEye, 0, false, shadowKey(12, 2, 3));
    check(!t.ticket.valid, "shadow ticket: a draw outside the args class invalidates what was there");
    t.finish(after, fsh::Class::kUnresolved, 0, true, shadowKey(12, 2, 3));
    check(!t.ticket.valid && t.count.draws[2] == 1, "shadow ticket: a draw of unknown class does not record, and is counted as unclassed");
}

// The comparison: on a would-take draw every term not ordinary is a mismatch, counted per term, and a class that changed is one too.
void testShadowMismatch() {
    fsh::Tracker t;
    const fsh::Key key = shadowKey(20, 1, 1);
    t.finish(fsh::Verdict(), fsh::Class::kEye, 0, true, key);   // a ticket
    unsigned each = 0;
    for (unsigned i = 0; i < fsh::kTermCount; ++i) {
        fsh::Tracker u;
        u.finish(fsh::Verdict(), fsh::Class::kEye, 0, true, key);
        const fsh::Verdict v = u.judge(key, true, 0, [] { return 0u; }, [] { return 0u; });
        const uint32_t mm = u.finish(v, fsh::Class::kEye, 1u << i, true, key);
        bool only = mm == (1u << i) && u.count.mismatchDraws == 1 && u.count.compared == 1;
        for (unsigned j = 0; j < fsh::kMismatchTermCount; ++j) only = only && u.count.mismatch[j] == (j == i ? 1u : 0u);
        each += only ? 1u : 0u;
    }
    check(each == fsh::kTermCount, "shadow mismatch: each of the 25 outcome terms, alone, is counted under that term and no other");
    const fsh::Verdict v = t.judge(key, true, 0, [] { return 0u; }, [] { return 0u; });
    const uint32_t two = fsh::termBit(fsh::kTermVerdict) | fsh::termBit(fsh::kTermReissue);
    check(t.finish(v, fsh::Class::kEye, two, true, key) == two && t.count.mismatchDraws == 1 && t.count.mismatch[fsh::kTermVerdict] == 1 &&
              t.count.mismatch[fsh::kTermReissue] == 1 && t.count.mismatch[fsh::kTermFamily] == 0,
          "shadow mismatch: a draw with two terms off counts one mismatched draw and both terms");
    // The ticket the draw above left is invalid, so the next draw is not compared at all even if it is full of terms.
    const fsh::Verdict none = t.judge(key, true, 0, [] { return 0u; }, [] { return 0u; });
    check(none.reason == fsh::Reason::kNoTicket, "shadow mismatch: a draw after a non-ordinary one is not a would-take draw");
    const uint64_t before = t.count.compared;
    t.finish(none, fsh::Class::kEye, fsh::kOutcomeMask, true, key);
    check(t.count.compared == before && t.count.mismatchDraws == 1, "shadow mismatch: a refused draw is never compared, whatever its terms");
    // The class: the ticket's class against the draw's.
    fsh::Tracker c;
    c.finish(fsh::Verdict(), fsh::Class::kOff, 0, true, key);
    const fsh::Verdict cv = c.judge(key, true, 0, [] { return 0u; }, [] { return 0u; });
    const uint32_t cm = c.finish(cv, fsh::Class::kEye, 0, true, key);
    check(cm == fsh::termBit(fsh::kTermClass) && c.count.mismatch[fsh::kTermClass] == 1 && c.count.mismatchDraws == 1,
          "shadow mismatch: a would-take draw whose class differs from the ticket's is a class mismatch");
    check(fsh::mismatchTerms(0, fsh::Class::kEye, fsh::Class::kEye) == 0 && fsh::mismatchTerms(fsh::termBit(fsh::kTermWake), fsh::Class::kOff, fsh::Class::kOff) == fsh::termBit(fsh::kTermWake),
          "shadow mismatch: the same class and an ordinary outcome is no mismatch");
    // The first five are named, once each.
    unsigned named = 0;
    fsh::Tracker n;
    for (int i = 0; i < 9; ++i) named += n.takeNameSlot() ? 1u : 0u;
    check(named == fsh::kNamedMismatches && fsh::kNamedMismatches == 5 && n.named == 5, "shadow mismatch: the first five mismatches of a session are named, and no more");
    check(fsh::lowestBit(0) == 32 && fsh::lowestBit(1) == 0 && fsh::lowestBit(0x80) == 7, "shadow mismatch: the first flag of a mask");
}

// The text.
void testShadowText() {
    char buf[2048];
    fsh::Counters none;
    const size_t emptyLen = fsh::formatLine(buf, sizeof(buf), 5400, 1800, none);
    const std::string empty(buf);
    check(emptyLen == empty.size() && contains(empty, "draw fast path (shadow):") && contains(empty, "TEMPORARY") && contains(empty, "1800-frame window ending 5400, 1800 frames"),
          "shadow text: the line names itself, the window and its frames");
    check(contains(empty, "Would take: eye -, off-screen -") && contains(empty, "Refused by first reason (window totals): none.") && contains(empty, "Mismatches: -."),
          "shadow text: an empty window reads '-' for the shares and the mismatches, and 'none' for refusals");
    fsh::Counters c;
    c.draws[0] = 1800 * 17800;
    c.draws[1] = 1800 * 17000;
    c.take[0] = 1800 * 16000;
    c.take[1] = 1800 * 15000;
    c.refused[static_cast<size_t>(fsh::Reason::kNoTicket)] = 3;
    c.refused[static_cast<size_t>(fsh::Reason::kKeyMoved)] = 2000;
    c.refused[static_cast<size_t>(fsh::Reason::kDynamic)] = 9;
    c.dynBy[fsh::kDynProbeChecks] = 9;
    c.refused[static_cast<size_t>(fsh::Reason::kArmed)] = 7;
    c.armedBy[fsh::kArmDust] = 5;
    c.armedBy[fsh::kArmLayout] = 2;
    c.tickets = 123456;
    fsh::formatLine(buf, sizeof(buf), 7200, 1800, c);
    std::string line(buf);
    check(contains(line, "Owner X draws a frame 34800.0 (eye 17800.0, off-screen 17000.0, unclassed 0.0)") && contains(line, "eye 89.9% (16000.0 a frame), off-screen 88.2% (15000.0 a frame)"),
          "shadow text: the draws a frame and the would-take shares, eye and off-screen apart");
    check(contains(line, "no ticket 3, key moved 2000, dynamic 9 (probe checks 9), armed 7 (dust 5, layout 2)") && !contains(line, "args class") && !contains(line, "not certified"),
          "shadow text: refusals by reason, the largest first, the detail under dynamic and armed, and a zero reason left out");
    check(contains(line, "Tickets recorded 123456.") && contains(line, "Mismatches: -."), "shadow text: the tickets, and '-' while there is nothing to compare");
    c.compared = 1000000;
    c.mismatchDraws = 0;
    fsh::formatLine(buf, sizeof(buf), 7200, 1800, c);
    line = buf;
    check(contains(line, "Mismatches: 0 draws of 1000000 compared (none).") && !contains(line, "Mismatches: -"), "shadow text: zero mismatches over a million says so");
    c.mismatchDraws = 3;
    c.mismatch[fsh::kTermFamily] = 2;
    c.mismatch[fsh::kTermWake] = 3;
    fsh::formatLine(buf, sizeof(buf), 7200, 1800, c);
    line = buf;
    check(contains(line, "Mismatches: 3 draws of 1000000 compared (wake 3, family 2).") , "shadow text: mismatches by term, the largest first");
    char terms[128];
    fsh::formatTerms(terms, sizeof(terms), 0);
    check(std::string(terms) == "-", "shadow text: no terms reads '-'");
    fsh::formatTerms(terms, sizeof(terms), fsh::termBit(fsh::kTermUiDepth) | fsh::termBit(fsh::kTermClass));
    check(std::string(terms) == "ui depth, class", "shadow text: a term mask lists its names in order");
    // Short buffers are cut and terminated, never overrun.
    char tiny[40];
    memset(tiny, 'x', sizeof(tiny));
    const size_t nt = fsh::formatLine(tiny, sizeof(tiny), 7200, 1800, c);
    check(nt < sizeof(tiny) && tiny[nt] == '\0', "shadow text: a short buffer is cut and terminated, never overrun");
    char one[1] = {'x'};
    check(fsh::formatLine(one, 1, 1, 1, c) == 0 && one[0] == '\0', "shadow text: a one-byte buffer holds the terminator alone");
    // The worst case: every counter large and every list full. The log note truncates near 1170.
    fsh::Counters w;
    for (int i = 0; i < 3; ++i) w.draws[i] = w.take[i] = 4000000000ull;
    for (size_t r = 1; r < static_cast<size_t>(fsh::Reason::kCount); ++r) w.refused[r] = 4000000000ull - r;
    for (unsigned i = 0; i < fsh::kDynCount; ++i) w.dynBy[i] = 4000000000ull - i;
    for (unsigned i = 0; i < fsh::kArmCount; ++i) w.armedBy[i] = 4000000000ull - i;
    for (unsigned i = 0; i < fsh::kMismatchTermCount; ++i) w.mismatch[i] = 4000000000ull - i;
    w.tickets = w.compared = w.mismatchDraws = 4000000000ull;
    char big[4096];
    const size_t worst = fsh::formatLine(big, sizeof(big), 4000000000u, 4000000000u, w);
    check(worst > 0 && worst < 1150 && std::strlen(big) == worst, "shadow text: the line is under 1150 characters at its worst (every counter at four billion, every list full)");
    std::printf("aa_off_test: shadow line worst case %zu characters\n", worst);
}

// The source: the gate's list and the subscriber set agree term for term, the shadow is wired where it must be, and the draw path gains no return.
std::set<std::string> callsIn(const std::string& squeezed, const std::set<std::string>& skip) {
    std::set<std::string> out;
    for (size_t i = 0; i < squeezed.size(); ++i) {
        if (squeezed[i] != '(') continue;
        size_t b = i;
        while (b > 0 && (std::isalnum(static_cast<unsigned char>(squeezed[b - 1])) || squeezed[b - 1] == '_')) --b;
        const std::string name = squeezed.substr(b, i - b);
        if (!name.empty() && !skip.count(name)) out.insert(name);
    }
    return out;
}
std::set<std::string> membersIn(const std::string& squeezed, const std::string& prefix) {
    std::set<std::string> out;
    for (size_t at = squeezed.find(prefix); at != std::string::npos; at = squeezed.find(prefix, at + prefix.size())) {
        size_t e = at + prefix.size();
        while (e < squeezed.size() && (std::isalnum(static_cast<unsigned char>(squeezed[e])) || squeezed[e] == '_')) ++e;
        out.insert(squeezed.substr(at + prefix.size(), e - at - prefix.size()));
    }
    return out;
}

void testShadowWiring() {
    const std::string vscreen = slurpFile("src/d3d11/vscreen.cpp"), subs = slurpFile("src/d3d11/draw_gate_subscribers.h");
    check(vscreen.size() > 100000 && subs.size() > 1000, "shadow wiring: vscreen.cpp and draw_gate_subscribers.h are readable from the repo root");
    // The gate's list and the subscriber set.
    const std::string gateHead = "bool drawGateSubscribed(State* s) {", listHead = "inline uint32_t drawGateSubscribers(const DrawGateState& st) {";
    const size_t g0 = vscreen.find(gateHead), l0 = subs.find(listHead);
    check(g0 != std::string::npos && l0 != std::string::npos, "shadow wiring: drawGateSubscribed and drawGateSubscribers are found");
    if (g0 == std::string::npos || l0 == std::string::npos) return;
    const std::string gate = squeezedCode(vscreen.substr(g0, vscreen.find("\n}\n", g0) - g0));
    const std::string list = squeezedCode(subs.substr(l0, subs.find("\n}\n", l0) - l0));
    // (The squeezed text keeps the return type against the name: the two function heads are the names to leave out.)
    const std::set<std::string> gateCalls = callsIn(gate, {"booldrawGateSubscribed"});
    const std::set<std::string> listCalls = callsIn(list, {"inlineuint32_tdrawGateSubscribers", "if"});
    check(gateCalls.size() == 24 && gateCalls == listCalls, "shadow wiring: the gate and the subscriber set call the same 24 getters (a subscriber added to one and not the other fails here)");
    if (gateCalls != listCalls) {
        for (const std::string& n : gateCalls) if (!listCalls.count(n)) std::printf("aa_off_test: in the gate, not in the set: %s\n", n.c_str());
        for (const std::string& n : listCalls) if (!gateCalls.count(n)) std::printf("aa_off_test: in the set, not in the gate: %s\n", n.c_str());
    }
    const std::string resampleHead = "void drawGateResample(State* s) {";
    const size_t r0 = vscreen.find(resampleHead);
    const std::string resample = r0 == std::string::npos ? std::string() : squeezedCode(vscreen.substr(r0, vscreen.find("\n}\n", r0) - r0));
    const std::set<std::string> fields = membersIn(gate, "s->");
    bool fieldsRead = fields.size() == 6;
    for (const std::string& f : fields) fieldsRead = fieldsRead && contains(resample, "s->" + f);
    check(fieldsRead && contains(resample, "drawGateSet(drawGateSubscribed(s));") && countOf(squeezedCode(vscreen), "drawGateSubscribers(") == 1,
          "shadow wiring: the six State fields the gate reads are the ones drawGateResample hands the set, which is built nowhere else");
    // Each of the 28 bits is set by exactly one term.
    static const struct { const char* name; uint32_t bit; } kBits[] = {
        {"kSubDistance", fsh::kSubDistance}, {"kSubCensusSkip", fsh::kSubCensusSkip}, {"kSubQuadSkip", fsh::kSubQuadSkip}, {"kSubCensusAuto", fsh::kSubCensusAuto},
        {"kSubFssRes", fsh::kSubFssRes}, {"kSubFssPanel", fsh::kSubFssPanel}, {"kSubRemlok", fsh::kSubRemlok}, {"kSubHolo", fsh::kSubHolo},
        {"kSubTargetSharp", fsh::kSubTargetSharp}, {"kSubUiDepth", fsh::kSubUiDepth}, {"kSubSunglare", fsh::kSubSunglare}, {"kSubCensus", fsh::kSubCensus},
        {"kSubBlendProbe", fsh::kSubBlendProbe}, {"kSubObjectProbe", fsh::kSubObjectProbe}, {"kSubPanelCurve", fsh::kSubPanelCurve},
        {"kSubParticles", fsh::kSubParticles}, {"kSubBackdrop", fsh::kSubBackdrop}, {"kSubScrim", fsh::kSubScrim}, {"kSubQuadProbe", fsh::kSubQuadProbe},
        {"kSubLoaderPanel", fsh::kSubLoaderPanel}, {"kSubIntroPanel", fsh::kSubIntroPanel}, {"kSubWakePulse", fsh::kSubWakePulse},
        {"kSubNightVision", fsh::kSubNightVision}, {"kSubSpaceDust", fsh::kSubSpaceDust}, {"kSubWitchspaceStars", fsh::kSubWitchspaceStars},
        {"kSubDepthProbe", fsh::kSubDepthProbe}, {"kSubFootprint", fsh::kSubFootprint}, {"kSubIntroCurve", fsh::kSubIntroCurve}};
    uint32_t all = 0;
    bool onceEach = true, distinct = true;
    for (const auto& b : kBits) {
        onceEach = onceEach && countOf(list, std::string("|=") + b.name + ";") == 1;
        distinct = distinct && (all & b.bit) == 0 && b.bit != 0;
        all |= b.bit;
    }
    check(onceEach && distinct && all == fsh::kSubscriberAll && countOf(list, "|=kSub") == fsh::kSubscriberCount && sizeof(kBits) / sizeof(kBits[0]) == fsh::kSubscriberCount,
          "shadow wiring: each of the 28 subscriber bits is distinct and set by exactly one term of the set");
    // The thunk.
    const std::string thunkHead = "void STDMETHODCALLTYPE hookedDrawIndexedInstanced(ID3D11DeviceContext* self,";
    const size_t t0 = vscreen.find(thunkHead);
    const std::string thunk = t0 == std::string::npos ? std::string() : squeezedCode(vscreen.substr(t0, vscreen.find("\n}\n", t0) - t0));
    const size_t world = thunk.find("if(g_vrWorldInternal){"), begin = thunk.find("if(self==g_state->ownerCtx)shadowBegin(shadow,g_state,perInstance,instances);"),
                 clock = thunk.find("DrawClockclock('X');"), verdict = thunk.find("constDrawVerdictv=beginPanelOverride(self,'X',perInstance,instances,args);"),
                 after = thunk.find("if(shadow.live)shadowAfterVerdict(shadow,g_state,v,perInstance,instances);"), pixel = thunk.find("pixelProbeAfterEye(g_state,self,perInstance,instances,false);"),
                 end = thunk.find("if(shadow.live)shadowEnd(shadow,perInstance,instances);");
    check(world != std::string::npos && begin != std::string::npos && clock != std::string::npos && verdict != std::string::npos && after != std::string::npos &&
              pixel != std::string::npos && end != std::string::npos && world < begin && begin < clock && clock < verdict && verdict < after && after < pixel && pixel < end,
          "shadow wiring: the thunk asks before its clock starts and after the flat and world-route returns, captures right after the verdict, and counts at the end");
    check(world != std::string::npos && end != std::string::npos && thunk.substr(0, world).find("shadow") == std::string::npos &&
              thunk.substr(end) == "if(shadow.live)shadowEnd(shadow,perInstance,instances);",
          "shadow wiring: nothing of the shadow is reached by the flat profile's branch, and the thunk's last statement is the shadow's end");
    check(clock != std::string::npos && countOf(thunk.substr(clock), "return") == 1 && contains(thunk.substr(clock), "returntrue;"),
          "shadow wiring: past the clock's start the thunk has one return, the lambda's (the shadow adds no early return to the draw path)");
    check(countOf(squeezedCode(vscreen), "drawGateResample(s);") == 2 && countOf(squeezedCode(vscreen), "drawGateSet(drawGateSubscribed(s))") == 1,
          "shadow wiring: the gate and the subscriber set are resampled together at both sites");
    {
        // Controls: a getter dropped from the set, a getter added to the gate alone, a return added past the clock, a shadow call moved ahead of the
        // flat branch, each makes its pin fail.
        std::string dropped = list;
        const size_t at = dropped.find("if(introCurveWants())m|=kSubIntroCurve;");
        if (at != std::string::npos) dropped.erase(at, std::string("if(introCurveWants())m|=kSubIntroCurve;").size());
        const std::string widened = gate + "||newSubscriberWantsDraws()";
        check(at != std::string::npos && callsIn(dropped, {"inlineuint32_tdrawGateSubscribers", "if"}) != gateCalls &&
                  callsIn(widened, {"booldrawGateSubscribed"}) != listCalls,
              "shadow wiring control: a getter dropped from the set, or added to the gate alone, fails the agreement pin");
        const std::string extraReturn = thunk.substr(clock) + "return;";
        const std::string moved = "shadowBegin(shadow,g_state,perInstance,instances);" + thunk;
        check(clock != std::string::npos && world != std::string::npos && countOf(extraReturn, "return") == 2 &&
                  moved.substr(0, moved.find("if(g_vrWorldInternal){")).find("shadow") != std::string::npos,
              "shadow wiring control: a return added past the clock, or a shadow call ahead of the flat branch, changes what the pins read");
    }
    // The outcome notes sit where the design reads them.
    const std::string code = squeezedCode(vscreen);
    check(contains(code, "constexpruint64_tkShadowScreenCompositeVs=0x5C36AF051B98B9F1ull;") && contains(code, "||vs==kShadowScreenCompositeVs;") &&
              contains(code, "voidfootprintEyeDraw(ID3D11DeviceContext*self,floatapplied,INTbaseVertex,UINTstartInstance){if(bindingShaderHash(BindSlot::Vs)!=0x5C36AF051B98B9F1ull"),
          "shadow wiring: the footprint instrument is certified because it acts only at the 2D screen's composite, and that vertex shader is one of the specials");
    {
        // The panel distance and the curved screen are certified, not armed, because everything they act on is outside the args class (count over 64): the
        // gates sit in the source where the certification says.
        const size_t armedAt = vscreen.find("inline uint32_t shadowArmedFlags(const State* s) {");
        const std::string armedBody = armedAt == std::string::npos ? std::string() : squeezedCode(vscreen.substr(armedAt, vscreen.find("\n}\n", armedAt) - armedAt));
        const std::string intro = squeezedCode(slurpFile("src/d3d11/intro_curve.cpp")), panel = squeezedCode(slurpFile("src/d3d11/intro_panel.cpp"));
        const std::string composite = "if(kind!='X'||count!=6||instances!=1)returnfalse;";
        check(fsh::kArgsCountOver == 64 && !armedBody.empty() && !contains(armedBody, "panelCurveWants") && !contains(armedBody, "distanceEnabled"),
              "shadow wiring: the armed flags leave out the panel distance and the curved screen, and the args class starts above the 64 their tests stop at");
        check(contains(code, "srv0IsPanelSized(State*s,charkind,uint32_tcount){if(count>64)returnfalse;") &&
                  contains(code, "if(!s->distanceEnabled)returnDrawVerdict::kNone;if(!srv0IsPanelSized(s,kind,count))returnDrawVerdict::kNone;") &&
                  contains(code, "if(panelCurveWants()&&srv0IsPanelSized(s,kind,count)&&"),
              "shadow wiring: srv0IsPanelSized answers false above 64 indices before any side effect, and both the distance fix and the curve's recognition sit behind it");
        check(countOf(code, "panelCurveSubstitute(self,") == 1 && countOf(code, "curvedScreenSwallowed(self,") == 1 &&
                  code.find("if(g_state->curveThisDraw){") != std::string::npos &&
                  code.find("if(g_state->curveThisDraw){") < code.find("panelCurveSubstitute(self,") && code.find("panelCurveSubstitute(self,") < code.find("curvedScreenSwallowed(self,") &&
                  countOf(code, "panelCurveSurfaceDraw(self,") == 2 &&
                  contains(code, "if((v==DrawVerdict::kIntroPanel||g_state->introCurveThisDraw)&&owner&&panelCurveSurfaceWanted()){") &&
                  contains(intro, composite) && contains(panel, composite),
              "shadow wiring: the curve's substitution and its reissue are behind curveThisDraw, its surface strip behind a verdict or introCurveThisDraw, and the intro recognisers need exactly six indices");
        const std::string layerCode = squeezedCode(slurpFile("src/d3d11/ui_layer.cpp"));
        const size_t stampAt = layerCode.find("uint64_tuiLayerAfterUiStamp(){");
        const std::string stamp = stampAt == std::string::npos ? std::string() : layerCode.substr(stampAt, layerCode.find("}", stampAt) - stampAt);
        bool countersAll = !stamp.empty();
        for (const char* n : {"w.afterWrites", "w.afterReads", "w.afterFollowed", "w.afterPostPass", "w.afterDeclined", "w.worldLeftDraws", "w.seedStale",
                              "w.depthOnlySeedPreservedWriters", "g_afterSeenCount", "(kWatchPerFrame-g_watchBudget)"})
            countersAll = countersAll && contains(stamp, n);
        check(countersAll && !contains(stamp, "dsvMemo"),
              "shadow wiring: the after-UI stamp sums the decisions and state changes (writes, reads, follows, post passes, declines, world-route losses, seed invalidations, preserved writers, "
              "named lines, the watch budget) and not the depth-view memo's lookup counters");
    }
    check(contains(code, "constuint64_tafterUiStamp=kind=='X'?uiLayerAfterUiStamp():0;") && contains(code, "if(kind=='X'&&uiLayerAfterUiStamp()!=afterUiStamp)shadowNote(fastshadow::kTermAfterUi);") &&
              contains(code, "constboolsmUi=screenMotionUiDraw(") && contains(code, "constboolsmDraw=screenMotionDraw(") && contains(code, "if(smUi||smDraw)shadowNote(fastshadow::kTermScreenMotion);") &&
              contains(code, "shadowNote(fastshadow::kTermWeapon);") && contains(code, "if(temporalWantsScene)shadowNote(fastshadow::kTermRigid);") &&
              contains(code, "shadowNoteProbes(eyeGeometry);") && contains(code, "if(layered)shadowNoteX(owner,kind,fastshadow::kTermLayered);") &&
              contains(code, "if(compositeCounted)shadowNoteX(owner,kind,fastshadow::kTermCompositeCounted);") &&
              contains(code, "if(uiFamily!=UiLayerFamily::kNone)shadowNoteX(owner,kind,fastshadow::kTermFamily);"),
          "shadow wiring: the after-UI stamp, the screen motion, weapon, rigid-draw, census, layered, composite and family notes are where the design reads them");
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
    testStepClockUiLayerSplit();
    testStepClockFastPath();
    testStepClockReissue();
    testStepWindow();
    testStepFloorAndScale();
    testStepCalibration();
    testStepText();
    testStepTextWorstCase();
    testMemoHit();
    testMemoRun();
    testFamilyMemoKey();
    testFamilyMemoStoreRule();
    testFamilyMemoSweep();
    testFamilyMemoVerification();
    testFamilyMemoWiring();
    const int checksBeforeShadow = g_checks;
    testShadowArgsClass();
    testShadowRefusalOrder();
    testShadowCertified();
    testShadowTicketRules();
    testShadowMismatch();
    testShadowText();
    testShadowWiring();
    std::printf("aa_off_test: the repeat-draw fast path's shadow instrument: %d checks\n", g_checks - checksBeforeShadow);
    std::printf("aa_off_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
