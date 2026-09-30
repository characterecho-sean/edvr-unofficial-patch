#pragma once
// Engine motion's CPU time on the game's threads, per frame: the render thread's
// every call clocked, the other threads' clocked on sampled frames
// (docs\openxr-performance-review-2026-09-14.md, the 2026-09-29 carrier entries).
//
// WHY. At Sean's busy fleet carrier Elite's render thread runs 11.6-12.4 ms a
// cycle with fix.temporal_aa on and about 5 ms with it off, and two things the
// AA path costs were not measured: the GPU it spends inside Elite's own draws
// (gpu_census.cpp's altered-draw sections) and the CPU it spends in hooks on
// Elite's own code, which the frame tick chain never saw (it covers the Present
// hook and, sampled, the Direct3D draw hooks). Engine motion has fifteen relays
// into game code, and the emit hook alone ran 1,801-2,930 calls a frame there.
// Flight 112704 answered it (the frame is GPU bound; engine motion costs the
// render thread 0.35-0.40 ms p50) and showed the cost of asking: with every call
// clocked on every thread the instrument itself spent about 1.17 ms a frame, most
// of it on the job threads, at 17,000 hook calls a frame. Hence the sampling.
//
// WHAT IS TIMED. EDVR's own work in each hook, never the game function the hook
// calls through: a relay that forwards is bracketed enter / pause / (game code) /
// resume / leave, so the forward is excluded and a nested EDVR scope opened
// inside it (a relay called by game code the outer relay forwarded to) is its
// own part's time and no one else's. A scope nested in EDVR's OWN work (the
// shader patch inside the draw side's slow half) is subtracted from its parent,
// so the parts partition: no nanosecond is in two of them.
//
// WHO IS CLOCKED, AND WHEN. One 64-bit gate word, read by every hook call:
//   the render thread (the thread that calls Present): EVERY call, EVERY frame.
//     About 1,600 calls a frame at the carrier, so its figures stay exact and the
//     LONG FRAME line's "engine motion" field is that frame's own.
//   every other thread: only on SAMPLED frames, one frame in kSamplePeriod, at a
//     position drawn at random within each block of that many frames (a fixed
//     stride would alias with anything the game does every N frames). On a
//     sampled frame every call is clocked, so the frame is exact; the other
//     threads' figures are per-sampled-frame statistics. On the frames between, a
//     hook costs one load of the gate, a compare against the thread id and a
//     branch, and is not counted.
// The gate is written once a frame by the render thread's cut; a scope decides
// once, at entry, and keeps its decision to its end. A scope that straddles a cut
// is recorded when it ends: the window's totals keep it, only the per-frame split
// of a straddler is off by a call.
//
// NO CONTENDED ATOMIC. Each thread owns one Slot of counters, registered on its
// first clocked scope (one fetch_add, once per thread) and written only by that
// thread: a running total is a relaxed load, an add and a relaxed store, plain
// moves on x64, no lock prefix, no shared cache line. The render thread's
// per-frame cut reads every slot's totals and takes the deltas.
//
// THE ONE THING NEVER CLOCKED, and the report says so: the two evaluator relays
// (Part kEval) forward straight through unless a diagnostic is attached, so
// their own work is two loads and a call, about 5 ns. A clock read alone costs
// more than that, so their calls are COUNTED, on sampled frames, and only the
// probe branches, which have real work, are clocked.
//
// THREAD ATTRIBUTION. "Render thread" is the thread that calls Present: the
// Present hook calls cutFrame, and the slot of the thread that cut is the render
// slot from then on. Every other slot is "other threads".
//
// THE INSTRUMENT'S OWN COST is calibrated the way the GPU census states its
// timer floor: null scopes on the render thread at the end of every window, in the
// CPU state the window ran in: a clocked scope (plain, and with a forward pause),
// what it RECORDS (it inflates every clocked figure by that much per call) and
// what it COSTS (wall time per call), a SKIPPED scope, and a counted call. The
// report weighs them by the window's call rates and the sampled fraction. Four
// batches of null pairs, the fastest kept: a batch a preemption or a slow clock
// inflated is not the floor.
//
// "THE CODE NEVER RAN" IS NEVER 0.00. A part with no calls in the window on a
// class of thread prints "-"; a part with calls whose time rounds to nothing
// prints 0.00. The LONG FRAME clause says "none this frame" for no calls.
//
// Header only, so tools\engine_motion_cpu_test can drive every function with a
// fake clock (define EDVR_EMCPU_NOW before including it).

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef EDVR_EMCPU_NOW
#define EDVR_EMCPU_NOW() (::edvr::emcpu::qpcNow())
#endif

namespace edvr {
namespace emcpu {

inline int64_t qpcNow() noexcept {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline int64_t qpcFrequency() noexcept {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

// One frame in this many is a sampled frame, for every thread but the render
// thread. At the carrier's call rates (about 17,500 clocked hook calls a frame off
// the render thread, 1,600 on it) 32 keeps the instrument under about 0.15 ms a
// frame; 16 was 0.18 (the rig prints the model with its own measured floor).
constexpr unsigned kSamplePeriod = 32;

// ---- the parts ---------------------------------------------------------------
enum Part : unsigned {
    kEmit = 0,   // FUN_144312E00's bracket (EDVR's work around the forward) and the emit observer
    kRigid,      // FUN_1442B4130, the primary rigid emit: relay + observer
    kCopier,     // FUN_144C81BE0, the engine's pool copier: relay + observer
    kMerge,      // FUN_14434E740, the list merge: begin and end
    kClear,      // FUN_1436819D0, the typed dictionary clear: the observer
    kEval,       // the evaluator and rig-eval relays: counted; only probe work is clocked
    kJobs,       // the six job-body brackets (UpdateRenderDataJob and its siblings)
    kBuilder,    // the draw-item builder bracket, the second direct producer, the part test, the LOD setter
    kDraw,       // the pool-family draw side: the slow half of engineVelocityBeforeDraw
    kApply,      // primaryCopy::apply inside the draw side (nested in it, not part of it)
    kTee,        // the Map/Unmap/write tees on watched resources
    kPatch,      // the lazy shader patch on a cache miss (nested in the draw side)
    kParts
};
constexpr unsigned kCal = kParts;          // the calibration cell, never reported
constexpr unsigned kCells = kParts + 1;

struct PartInfo {
    const char* name;
    bool clocked;   // false: counted, and only the work behind a diagnostic is clocked
    bool paused;    // the scope's shape: enter / pause / resume / leave (four clock reads)
};
inline constexpr PartInfo kInfo[kParts] = {
    {"emit", true, true},        {"rigid emit", true, true}, {"copier", true, true},
    {"merge", true, true},       {"clear", true, false},     {"eval", false, false},
    {"jobs", true, true},        {"builder", true, true},    {"draw side", true, false},
    {"apply", true, false},      {"tees", true, false},      {"shader patch", true, false},
};
// Report order: the render thread's own parts first.
inline constexpr unsigned kOrder[kParts] = {kDraw, kApply, kTee,     kPatch, kEmit,    kRigid,
                                            kCopier, kMerge, kClear, kJobs,  kBuilder, kEval};
constexpr bool orderIsPermutation() {
    for (unsigned p = 0; p < kParts; ++p) {
        unsigned seen = 0;
        for (unsigned i = 0; i < kParts; ++i) seen += kOrder[i] == p ? 1u : 0u;
        if (seen != 1) return false;
    }
    return true;
}
static_assert(orderIsPermutation(), "every part is reported exactly once");

// ---- the gate ------------------------------------------------------------------
// Bit 63: this frame is a sampled frame (every thread's hooks are clocked). Bits
// 0-31: the render thread's id (whose hooks are always clocked). Written by the
// render thread's cut once a frame, read by every hook call.
constexpr uint64_t kSampledBit = uint64_t(1) << 63;
inline std::atomic<uint64_t> g_gate{0};
static_assert(std::atomic<uint64_t>::is_always_lock_free, "the gate and the counters are plain moves on x64");

// The calling thread's id: the TEB's ClientId.UniqueThread, what GetCurrentThreadId
// reads, without the call. (Recorder checks it against GetCurrentThreadId once and
// clocks everything, saying so, if it ever disagreed.)
inline uint32_t currentThreadId() noexcept {
#if defined(_M_X64)
    return static_cast<uint32_t>(__readgsdword(0x48));
#else
    return static_cast<uint32_t>(GetCurrentThreadId());
#endif
}
inline uint64_t makeGate(bool sampled, uint32_t renderTid) noexcept {
    return (sampled ? kSampledBit : uint64_t(0)) | renderTid;
}
inline bool gateClocks(uint64_t gate) noexcept {
    return (gate & kSampledBit) != 0 || static_cast<uint32_t>(gate) == currentThreadId();
}
// The gates the calibration runs through: one that clocks this thread, one that skips it
// (not sampled, and a render thread id no thread has). The sink keeps the skipped loop's
// decision from being compiled away.
inline std::atomic<uint64_t> g_calSampled{kSampledBit};
inline std::atomic<uint64_t> g_calSkipped{0xFFFFFFFFull};
inline volatile uint32_t g_calSink = 0;

// The pseudo-random schedule: exactly one sampled frame in each block of `period`
// frames, its position drawn afresh for every block (xorshift32, a fixed seed, so
// a rig can predict it and two runs agree).
class SampleSchedule {
public:
    explicit SampleSchedule(unsigned period = kSamplePeriod, uint32_t seed = 0x2545F491u) noexcept
        : period_(period ? period : 1u), rng_(seed ? seed : 1u) {}
    unsigned period() const noexcept { return period_; }
    // Whether the frame that starts now is a sampled frame.
    bool next() noexcept {
        if (index_ == 0) {
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            pick_ = rng_ % period_;
        }
        const bool sampled = index_ == pick_;
        if (++index_ >= period_) index_ = 0;
        return sampled;
    }

private:
    unsigned period_, index_ = 0, pick_ = 0;
    uint32_t rng_;
};

// ---- the per-thread counters --------------------------------------------------
struct alignas(32) Cell {
    std::atomic<uint64_t> ticks{0};     // exclusive time, running total (clocked scopes only)
    std::atomic<uint64_t> calls{0};     // running total: clocked scopes, and kEval's counted calls
    std::atomic<uint64_t> maxTicks{0};  // the longest single call of maxEpoch's window
    uint64_t spare_ = 0;                // a cell is one 32-byte half of a cache line
};
struct alignas(64) Slot {
    Cell cell[kCells];
    std::atomic<uint32_t> tid{0};
    std::atomic<uint32_t> maxEpoch{0};
    uint64_t spare_[3] = {};            // fills the slot to seven cache lines
};
static_assert(sizeof(Cell) == 32 && sizeof(Slot) == 448, "the layout the comments describe");

constexpr unsigned kMaxSlots = 128;
inline Slot g_slots[kMaxSlots];
inline Slot g_overflow;   // threads past kMaxSlots share this one: approximate, counted, said
inline std::atomic<unsigned> g_slotCount{0};
inline std::atomic<unsigned> g_overflowThreads{0};
inline std::atomic<uint32_t> g_epoch{1};   // the window the per-call maxima belong to
inline std::atomic<Slot*> g_renderSlot{nullptr};

struct ThreadCtx {
    Slot* slot;
    int64_t child;   // the direct children's wall time inside the scope now open on this thread
};
inline thread_local ThreadCtx t_ctx{nullptr, 0};

__declspec(noinline) inline Slot* registerSlot() noexcept {
    ThreadCtx& c = t_ctx;
    if (c.slot) return c.slot;
    const unsigned i = g_slotCount.fetch_add(1, std::memory_order_acq_rel);
    Slot* s = nullptr;
    if (i < kMaxSlots) {
        s = &g_slots[i];
    } else {
        g_overflowThreads.fetch_add(1, std::memory_order_relaxed);
        s = &g_overflow;
    }
    s->tid.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_relaxed);
    s->maxEpoch.store(g_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
    c.slot = s;
    return s;
}
inline unsigned slotCount() noexcept {
    return (std::min)(g_slotCount.load(std::memory_order_acquire), kMaxSlots);
}

inline void record(Slot* s, unsigned part, int64_t excl, bool counts) noexcept {
    if (excl < 0) excl = 0;
    const uint64_t e = static_cast<uint64_t>(excl);
    Cell& c = s->cell[part];
    c.ticks.store(c.ticks.load(std::memory_order_relaxed) + e, std::memory_order_relaxed);
    if (counts) c.calls.store(c.calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    const uint32_t epoch = g_epoch.load(std::memory_order_relaxed);
    if (s->maxEpoch.load(std::memory_order_relaxed) != epoch) {
        for (unsigned p = 0; p < kCells; ++p) s->cell[p].maxTicks.store(0, std::memory_order_relaxed);
        s->maxEpoch.store(epoch, std::memory_order_relaxed);
    }
    if (e > c.maxTicks.load(std::memory_order_relaxed)) c.maxTicks.store(e, std::memory_order_relaxed);
}

// One counted call with no clock read (Part kEval), on sampled frames only: on the
// rest it is a load of the gate and a branch.
inline void countOn(const std::atomic<uint64_t>& gate, unsigned part) noexcept {
    if (!(gate.load(std::memory_order_relaxed) & kSampledBit)) return;
    Slot* s = t_ctx.slot;
    if (!s) s = registerSlot();
    Cell& c = s->cell[part];
    c.calls.store(c.calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
inline void count(unsigned part) noexcept { countOn(g_gate, part); }

// ---- the scope ----------------------------------------------------------------
// A POD token, for functions with __try/__finally (C2712: no object with a
// destructor there), and the RAII Scope below for the rest. The clock is read
// LAST in enter/pause/resume/leave-before-bookkeeping and FIRST in the closing
// calls, so the bookkeeping is outside the timed span: what a null scope
// records is about one clock read's latency, not the whole call. A scope the
// gate did not clock is `live == false`: enter set one byte, every later call on
// it returns at once, and nothing is read or written.
struct Token {
    ThreadCtx* ctx;
    Slot* slot;
    int64_t begin, seg, excl, savedChild;
    uint8_t part;
    bool counts, paused, live;
};
inline void enterOn(const std::atomic<uint64_t>& gate, Token& k, unsigned part, bool counts) noexcept {
    if (!gateClocks(gate.load(std::memory_order_relaxed))) {
        k.live = false;
        return;
    }
    ThreadCtx& c = t_ctx;
    Slot* s = c.slot;
    if (!s) s = registerSlot();
    k.live = true;
    k.ctx = &c;
    k.slot = s;
    k.part = static_cast<uint8_t>(part);
    k.counts = counts;
    k.paused = false;
    k.excl = 0;
    k.savedChild = c.child;
    c.child = 0;
    const int64_t now = EDVR_EMCPU_NOW();
    k.begin = k.seg = now;
}
inline void enter(Token& k, unsigned part, bool counts = true) noexcept { enterOn(g_gate, k, part, counts); }
// Before calling into game code, with the reading the caller took (kinematic_eval_hook's
// job bracket clocks the game call for its own statistics, and shares that reading).
inline void pauseAt(Token& k, int64_t now) noexcept {
    if (!k.live) return;
    if (!k.paused) {
        k.excl += (now - k.seg) - k.ctx->child;
        k.paused = true;
    }
    k.ctx->child = 0;
}
inline void resumeAt(Token& k, int64_t now) noexcept {
    if (!k.live) return;
    k.seg = now;
    k.paused = false;
    k.ctx->child = 0;
}
// The same, taking their own reading, and returning it (0 for a scope that was not clocked).
inline int64_t pause(Token& k) noexcept {
    if (!k.live) return 0;
    const int64_t now = EDVR_EMCPU_NOW();
    pauseAt(k, now);
    return now;
}
inline int64_t resume(Token& k) noexcept {
    if (!k.live) return 0;
    const int64_t now = EDVR_EMCPU_NOW();
    resumeAt(k, now);
    return now;
}
inline void leave(Token& k) noexcept {
    if (!k.live) return;
    const int64_t now = EDVR_EMCPU_NOW();
    if (!k.paused) k.excl += (now - k.seg) - k.ctx->child;
    record(k.slot, k.part, k.excl, k.counts);
    // The parent sees this scope's whole wall time (its forward included) as
    // its child, whether or not the parent is paused: a paused parent resets it.
    k.ctx->child = k.savedChild + (now - k.begin);
}

class Scope {
public:
    explicit Scope(unsigned part, bool counts = true) noexcept { enter(k_, part, counts); }
    ~Scope() { leave(k_); }
    int64_t pause() noexcept { return emcpu::pause(k_); }
    int64_t resume() noexcept { return emcpu::resume(k_); }
    void pauseAt(int64_t now) noexcept { emcpu::pauseAt(k_, now); }
    void resumeAt(int64_t now) noexcept { emcpu::resumeAt(k_, now); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Token k_;
};

// ---- the render thread's per-frame cut ------------------------------------------
struct CutState {
    uint64_t ticks[kParts];
    uint64_t calls[kParts];
};
inline CutState g_cut[kMaxSlots + 1];
constexpr unsigned kMaskWords = (kMaxSlots + 1 + 63) / 64;

struct FrameCut {
    uint64_t renderTicks[kParts];
    uint64_t renderCalls[kParts];
    uint64_t otherTicks[kParts];
    uint64_t otherCalls[kParts];
    uint64_t otherMask[kMaskWords];   // the slots (threads) with any activity this frame
    bool sampled;                     // this frame was a sampled frame (the caller says so: it set the gate)
};

// Called by the thread that calls Present, once per frame: that thread is the
// render thread. Deltas since the previous cut, split render / other.
inline void cutFrame(FrameCut& out) noexcept {
    std::memset(&out, 0, sizeof(out));
    Slot* me = t_ctx.slot;
    if (!me) me = registerSlot();
    g_renderSlot.store(me, std::memory_order_relaxed);
    const unsigned n = slotCount();
    for (unsigned i = 0; i <= n; ++i) {
        Slot* s = i < n ? &g_slots[i] : &g_overflow;
        const unsigned index = i < n ? i : kMaxSlots;
        CutState& st = g_cut[index];
        const bool render = s == me;
        bool active = false;
        for (unsigned p = 0; p < kParts; ++p) {
            const uint64_t t = s->cell[p].ticks.load(std::memory_order_relaxed);
            const uint64_t k = s->cell[p].calls.load(std::memory_order_relaxed);
            const uint64_t dt = t - st.ticks[p], dk = k - st.calls[p];
            st.ticks[p] = t;
            st.calls[p] = k;
            if (render) {
                out.renderTicks[p] += dt;
                out.renderCalls[p] += dk;
            } else {
                out.otherTicks[p] += dt;
                out.otherCalls[p] += dk;
                if (dt | dk) active = true;
            }
        }
        if (active) out.otherMask[index / 64] |= uint64_t(1) << (index % 64);
    }
}

// The rig only: forget every slot, and clock every scope (a sampled gate with no
// render thread). Not safe with other threads alive.
inline void resetForTest() noexcept {
    for (unsigned i = 0; i < kMaxSlots; ++i) {
        Slot& s = g_slots[i];
        for (unsigned p = 0; p < kCells; ++p) {
            s.cell[p].ticks.store(0);
            s.cell[p].calls.store(0);
            s.cell[p].maxTicks.store(0);
        }
        s.tid.store(0);
        s.maxEpoch.store(0);
    }
    for (unsigned p = 0; p < kCells; ++p) {
        g_overflow.cell[p].ticks.store(0);
        g_overflow.cell[p].calls.store(0);
        g_overflow.cell[p].maxTicks.store(0);
    }
    std::memset(g_cut, 0, sizeof(g_cut));
    g_slotCount.store(0);
    g_overflowThreads.store(0);
    g_epoch.store(1);
    g_renderSlot.store(nullptr);
    g_gate.store(kSampledBit);
    t_ctx.slot = nullptr;
    t_ctx.child = 0;
}

// ---- the clock floor -------------------------------------------------------------
struct Floor {
    bool measured = false;
    unsigned pairs = 0, batches = 0;                // null pairs per batch (the tight loops), and batches
    double plainRecordedNs = 0, plainCostNs = 0;    // a clocked scope: enter / leave
    double pausedRecordedNs = 0, pausedCostNs = 0;  // enter / pause / resume / leave
    double skipNs = 0;                              // a scope the gate did not clock: enter, leave
    double countNs = 0;                             // a counted call (kEval) on a sampled frame
    double countSkipNs = 0;                         // a counted call on a frame that is not sampled: the gate check alone
};
// Null scopes on the calling thread, into the calibration cell (never reported),
// through the calibration gates so the gate load and the thread-id compare are in
// every figure. Two things are measured, with two loops, because they need opposite
// treatment:
//   Cost: wall time per call, the instrument's own price. Tight back-to-back loops,
//     `batches` batches of `pairs` null pairs (kCheapFactor times as many for the
//     three that cost a few ns, or the clock's own granularity would be the answer),
//     and the batch with the LEAST wall time is kept: a preemption only ever adds.
//   Recorded: what a scope with nothing in it writes into its part's ticks, which
//     every clocked figure includes once per call. Back to back, a loop of null
//     scopes runs in step with the clock: at 33 ns a pair it is three pairs to one
//     100 ns tick, so the batch's recorded total was an accident of where it began
//     (0.2 ns a scope in one run, 12 in the next, the same code). A short random wait
//     between the scopes spreads them over the tick, and the MEDIAN of the batches is
//     kept: a preemption inside a scope inflates a batch, only ever upward.
constexpr unsigned kCheapFactor = 32;
constexpr unsigned kMaxBatches = 16;
inline void jitter(uint32_t& rng) noexcept {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    for (uint32_t i = rng & 63u; i; --i) g_calSink = g_calSink + 1;   // volatile: the wait is not compiled away
}
inline Floor calibrate(int64_t freq, unsigned pairs = 512, unsigned batches = 4) noexcept {
    Floor f;
    if (freq <= 0 || pairs == 0 || batches == 0) return f;
    if (batches > kMaxBatches) batches = kMaxBatches;
    Token k;
    Slot* s = t_ctx.slot;
    if (!s) s = registerSlot();
    for (unsigned i = 0; i < 64; ++i) {   // warm: the slot, the thread-local, the code
        enterOn(g_calSampled, k, kCal, false);
        leave(k);
    }
    // Cost.
    for (int shape = 0; shape < 5; ++shape) {
        const unsigned n = shape < 2 ? pairs : pairs * kCheapFactor;
        const double perNs = 1e9 / static_cast<double>(freq) / static_cast<double>(n);
        int64_t best = 0;
        for (unsigned b = 0; b < batches; ++b) {
            const int64_t t0 = EDVR_EMCPU_NOW();
            switch (shape) {
            case 0:
                for (unsigned i = 0; i < n; ++i) {
                    enterOn(g_calSampled, k, kCal, false);
                    leave(k);
                }
                break;
            case 1:
                for (unsigned i = 0; i < n; ++i) {
                    enterOn(g_calSampled, k, kCal, false);
                    pause(k);
                    resume(k);
                    leave(k);
                }
                break;
            case 2: {
                uint32_t sink = 0;
                for (unsigned i = 0; i < n; ++i) {
                    // The compiler may not hoist the gate and thread-id reads out of the loop, nor drop the decision.
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                    enterOn(g_calSkipped, k, kCal, false);
                    sink += k.live ? 1u : 0u;
                    leave(k);
                }
                g_calSink = sink;
                break;
            }
            default: {   // 3: a counted call on a sampled frame, 4: on one that is not
                const std::atomic<uint64_t>& gate = shape == 3 ? g_calSampled : g_calSkipped;
                for (unsigned i = 0; i < n; ++i) {
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                    countOn(gate, kCal);
                }
                break;
            }
            }
            const int64_t t1 = EDVR_EMCPU_NOW();
            if (!b || t1 - t0 < best) best = t1 - t0;
        }
        const double cost = static_cast<double>(best) * perNs;
        switch (shape) {
        case 0: f.plainCostNs = cost; break;
        case 1: f.pausedCostNs = cost; break;
        case 2: f.skipNs = cost; break;
        case 3: f.countNs = cost; break;
        default: f.countSkipNs = cost; break;
        }
    }
    // Recorded.
    uint32_t rng = 0x9E3779B9u;
    const unsigned jn = pairs > 1 ? pairs / 2 : 1;
    const double jPerNs = 1e9 / static_cast<double>(freq) / static_cast<double>(jn);
    for (int shape = 0; shape < 2; ++shape) {
        double rec[kMaxBatches];
        for (unsigned b = 0; b < batches; ++b) {
            const uint64_t r0 = s->cell[kCal].ticks.load(std::memory_order_relaxed);
            for (unsigned i = 0; i < jn; ++i) {
                jitter(rng);
                enterOn(g_calSampled, k, kCal, false);
                if (shape) {
                    pause(k);
                    resume(k);
                }
                leave(k);
            }
            rec[b] = static_cast<double>(s->cell[kCal].ticks.load(std::memory_order_relaxed) - r0) * jPerNs;
        }
        std::sort(rec, rec + batches);
        const double median = (batches & 1u) ? rec[batches / 2] : 0.5 * (rec[batches / 2 - 1] + rec[batches / 2]);
        (shape ? f.pausedRecordedNs : f.plainRecordedNs) = median;
    }
    f.pairs = pairs;
    f.batches = batches;
    f.measured = true;
    return f;
}

// ---- one window ------------------------------------------------------------------
struct PartWindow {
    uint64_t renderCalls = 0, otherCalls = 0;
    double renderMs = 0;                                   // window total
    double renderP50 = 0, renderP95 = 0, renderMax = 0;    // ms per frame, over every frame
    double renderCallMaxMs = 0;                            // the longest single call
    double otherMs = 0;                                    // window total, over the frames that were clocked
    double otherCallMaxMs = 0;
};
struct WindowReport {
    bool valid = false;
    double seconds = 0;
    unsigned frames = 0;
    unsigned sampledFrames = 0;                            // frames the other threads were clocked in
    unsigned samplePeriod = 0;                             // one frame in this many
    bool samplingOff = false;                              // every thread was clocked every frame (the thread id self-check failed)
    unsigned renderTid = 0;
    PartWindow part[kParts];
    double totalP50 = 0, totalP95 = 0, totalMax = 0;       // render thread, every part, ms per frame
    double otherMs = 0;                                    // window total, every part, other threads
    double otherP50 = 0, otherP95 = 0, otherMax = 0;       // other threads, every part, thread-ms per SAMPLED frame
    unsigned otherThreads = 0;
    unsigned overflowThreads = 0;
    Floor floor;
};

inline void percentiles(float* a, unsigned n, double* p50, double* p95, double* mx) noexcept {
    *p50 = *p95 = *mx = 0.0;
    if (!n) return;
    float* end = a + n;
    *mx = static_cast<double>(*std::max_element(a, end));
    unsigned i95 = static_cast<unsigned>(0.95 * static_cast<double>(n));
    if (i95 >= n) i95 = n - 1;
    const unsigned i50 = n / 2;
    std::nth_element(a, a + i95, end);
    *p95 = static_cast<double>(a[i95]);
    if (i50 >= i95) {
        *p50 = static_cast<double>(a[i95]);
    } else {
        std::nth_element(a, a + i50, a + i95);
        *p50 = static_cast<double>(a[i50]);
    }
}

class Window {
public:
    static constexpr unsigned kMaxFrames = 6000;   // 200 Hz for 30 s; a full window closes early

    void reset() noexcept {
        frames_ = 0;
        sampled_ = 0;
        std::memset(renderCalls_, 0, sizeof(renderCalls_));
        std::memset(otherCalls_, 0, sizeof(otherCalls_));
        std::memset(renderTicks_, 0, sizeof(renderTicks_));
        std::memset(otherTicks_, 0, sizeof(otherTicks_));
        std::memset(mask_, 0, sizeof(mask_));
    }
    unsigned frames() const noexcept { return frames_; }
    unsigned sampledFrames() const noexcept { return sampled_; }
    bool full() const noexcept { return frames_ >= kMaxFrames; }

    // The other threads' ticks and calls are summed over EVERY frame (a scope that
    // straddled a cut is recorded in the next frame's delta, and belongs to the
    // window); their per-frame split is kept for the sampled frames only.
    void add(const FrameCut& c, double msPerTick) noexcept {
        if (frames_ >= kMaxFrames) return;
        double total = 0.0, other = 0.0;
        for (unsigned p = 0; p < kParts; ++p) {
            const double ms = static_cast<double>(c.renderTicks[p]) * msPerTick;
            ms_[p][frames_] = static_cast<float>(ms);
            total += ms;
            other += static_cast<double>(c.otherTicks[p]) * msPerTick;
            renderCalls_[p] += c.renderCalls[p];
            otherCalls_[p] += c.otherCalls[p];
            renderTicks_[p] += c.renderTicks[p];
            otherTicks_[p] += c.otherTicks[p];
        }
        ms_[kParts][frames_] = static_cast<float>(total);
        if (c.sampled) otherSampled_[sampled_++] = static_cast<float>(other);
        for (unsigned w = 0; w < kMaskWords; ++w) mask_[w] |= c.otherMask[w];
        ++frames_;
    }

    // Folds the window into out and empties it. renderMax/otherMax are the
    // per-call maxima in ticks (from the slots, collectCallMax).
    void finish(WindowReport& out, double msPerTick, const uint64_t renderMax[kParts],
                const uint64_t otherMax[kParts]) noexcept {
        out = WindowReport{};
        out.valid = frames_ > 0;
        out.frames = frames_;
        out.sampledFrames = sampled_;
        for (unsigned p = 0; p < kParts; ++p) {
            PartWindow& w = out.part[p];
            w.renderCalls = renderCalls_[p];
            w.otherCalls = otherCalls_[p];
            w.renderMs = static_cast<double>(renderTicks_[p]) * msPerTick;
            w.otherMs = static_cast<double>(otherTicks_[p]) * msPerTick;
            w.renderCallMaxMs = static_cast<double>(renderMax[p]) * msPerTick;
            w.otherCallMaxMs = static_cast<double>(otherMax[p]) * msPerTick;
            percentiles(ms_[p], frames_, &w.renderP50, &w.renderP95, &w.renderMax);
            out.otherMs += w.otherMs;
        }
        percentiles(ms_[kParts], frames_, &out.totalP50, &out.totalP95, &out.totalMax);
        percentiles(otherSampled_, sampled_, &out.otherP50, &out.otherP95, &out.otherMax);
        for (unsigned w = 0; w < kMaskWords; ++w) {
            uint64_t m = mask_[w];
            while (m) {
                out.otherThreads += static_cast<unsigned>(m & 1u);
                m >>= 1;
            }
        }
        reset();
    }

private:
    unsigned frames_ = 0, sampled_ = 0;
    uint64_t renderCalls_[kParts] = {}, otherCalls_[kParts] = {};
    uint64_t renderTicks_[kParts] = {}, otherTicks_[kParts] = {};
    uint64_t mask_[kMaskWords] = {};
    float ms_[kParts + 1][kMaxFrames];
    float otherSampled_[kMaxFrames];
};

// The per-call maxima of the window that just ended, from the slots that
// recorded anything in it; then the next window starts.
inline void collectCallMax(uint64_t renderMax[kParts], uint64_t otherMax[kParts]) noexcept {
    std::memset(renderMax, 0, sizeof(uint64_t) * kParts);
    std::memset(otherMax, 0, sizeof(uint64_t) * kParts);
    const uint32_t epoch = g_epoch.load(std::memory_order_relaxed);
    const Slot* render = g_renderSlot.load(std::memory_order_relaxed);
    const unsigned n = slotCount();
    for (unsigned i = 0; i <= n; ++i) {
        const Slot* s = i < n ? &g_slots[i] : &g_overflow;
        if (s->maxEpoch.load(std::memory_order_relaxed) != epoch) continue;
        uint64_t* dst = s == render ? renderMax : otherMax;
        for (unsigned p = 0; p < kParts; ++p)
            dst[p] = (std::max)(dst[p], s->cell[p].maxTicks.load(std::memory_order_relaxed));
    }
    g_epoch.fetch_add(1, std::memory_order_relaxed);
}

// ---- what one frame looked like, for the LONG FRAME clause -------------------------
struct Figures {
    bool measured = false;         // false on the priming frame and with no clock rate
    bool sampled = false;          // the other threads were clocked in this frame
    double renderMs = 0;           // EDVR's engine-motion hook time on the render thread this frame: every call, exact
    uint64_t renderCalls = 0;
    double otherMs = 0;            // the same on every other thread (thread-ms, summed): zero unless sampled
    uint64_t otherCalls = 0;
};

class Recorder {
public:
    static constexpr uint64_t kWindowMs = 30000;

    explicit Recorder(unsigned period = kSamplePeriod, uint32_t seed = 0x2545F491u) noexcept
        : schedule_(period, seed) {}

    // Once per frame, from the Present hook (the render thread). The first call
    // primes the baselines and decides the first frame; it reports nothing. The
    // clock floor is measured as each window closes.
    Figures onFrame(int64_t freq, uint64_t nowMs) noexcept {
        Figures fig;
        FrameCut cut;
        cutFrame(cut);
        cut.sampled = sampledNow_;   // the gate as it stood for the frame that just ended
        if (!primed_) {
            primed_ = true;
            // The thread-id read is checked once against the API. A mismatch would
            // leave the render thread unrecognised on unsampled frames, so the
            // scheme is dropped and every thread clocked, and the report says so.
            samplingOk_ = !forceOff_ && currentThreadId() == static_cast<uint32_t>(GetCurrentThreadId());
            windowStartMs_ = nowMs;
            window_.reset();
            decideNextFrame();
            return fig;
        }
        const double msPerTick = freq > 0 ? 1000.0 / static_cast<double>(freq) : 0.0;
        window_.add(cut, msPerTick);
        fig.measured = freq > 0;
        fig.sampled = cut.sampled;
        for (unsigned p = 0; p < kParts; ++p) {
            fig.renderMs += static_cast<double>(cut.renderTicks[p]) * msPerTick;
            fig.renderCalls += cut.renderCalls[p];
            fig.otherMs += static_cast<double>(cut.otherTicks[p]) * msPerTick;
            fig.otherCalls += cut.otherCalls[p];
        }
        if (nowMs - windowStartMs_ >= kWindowMs || window_.full()) {
            uint64_t renderMax[kParts], otherMax[kParts];
            collectCallMax(renderMax, otherMax);
            window_.finish(report_, msPerTick, renderMax, otherMax);
            report_.seconds = static_cast<double>(nowMs - windowStartMs_) / 1000.0;
            report_.samplePeriod = schedule_.period();
            report_.samplingOff = !samplingOk_;
            const Slot* render = g_renderSlot.load(std::memory_order_relaxed);
            report_.renderTid = render ? render->tid.load(std::memory_order_relaxed) : 0;
            report_.overflowThreads = g_overflowThreads.load(std::memory_order_relaxed);
            floor_ = calibrate(freq);   // in the CPU state the window just ran in
            report_.floor = floor_;
            ready_ = report_.valid;
            windowStartMs_ = nowMs;
        }
        decideNextFrame();
        return fig;
    }
    bool takeReport(WindowReport& out) noexcept {
        if (!ready_) return false;
        out = report_;
        ready_ = false;
        return true;
    }
    const Floor& floor() const noexcept { return floor_; }
    bool sampledNow() const noexcept { return sampledNow_; }
    // The rig only: behave as if the thread id self-check had failed. Call before the first frame.
    void disableSamplingForTest() noexcept { forceOff_ = true; }

private:
    // Sets the gate for the frame that starts now.
    void decideNextFrame() noexcept {
        sampledNow_ = samplingOk_ ? schedule_.next() : true;
        g_gate.store(makeGate(sampledNow_, static_cast<uint32_t>(GetCurrentThreadId())), std::memory_order_relaxed);
    }

    SampleSchedule schedule_;
    bool primed_ = false, ready_ = false, samplingOk_ = true, sampledNow_ = false, forceOff_ = false;
    uint64_t windowStartMs_ = 0;
    Floor floor_;
    Window window_;
    WindowReport report_;
};

// ---- text ---------------------------------------------------------------------------
inline void appendf(char* buf, size_t cap, size_t& len, const char* fmt, ...) noexcept {
    if (!buf || len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    len = static_cast<size_t>(n) < cap - len ? len + static_cast<size_t>(n) : cap - 1;
}

// Calls per frame: one decimal, three when it is under 0.1 so a rare event (a shader
// patch) does not read as 0.0, which would look like code that never ran.
inline void callsText(char* out, size_t cap, double perFrame) noexcept {
    std::snprintf(out, cap, perFrame > 0.0 && perFrame < 0.1 ? "%.3f" : "%.1f", perFrame);
}

// The instrument's own price at this window's call rates and sampled fraction, in
// ms per frame: what it costs (spent), and what its clock reads add to the figures
// (recorded). The render thread's calls are clocked every frame. The other
// threads' rates are per sampled frame (that is all that was counted): on a
// sampled frame each call costs its calibrated shape, on the others it costs the
// skip; the counted evaluator calls cost countNs on sampled frames and a gate
// check on the rest.
struct InstrumentCost {
    double renderMs = 0, otherMs = 0, totalMs = 0;   // spent: the render thread, the other threads, together, per frame
    double renderRecordedMs = 0;                     // floor inside the render thread's figures, per frame
    double otherRecordedMs = 0;                      // floor inside the other threads' figures, per SAMPLED frame
};
inline InstrumentCost instrumentCost(const WindowReport& r) noexcept {
    InstrumentCost c;
    if (!r.floor.measured || !r.frames) return c;
    const double frames = static_cast<double>(r.frames);
    const double sampled = r.sampledFrames ? static_cast<double>(r.sampledFrames) : 1.0;
    const double fraction = r.sampledFrames ? sampled / frames : 1.0;
    for (unsigned p = 0; p < kParts; ++p) {
        const double clockedNs = kInfo[p].clocked ? (kInfo[p].paused ? r.floor.pausedCostNs : r.floor.plainCostNs)
                                                   : r.floor.countNs;
        const double recordedNs = !kInfo[p].clocked ? 0.0
                                  : (kInfo[p].paused ? r.floor.pausedRecordedNs : r.floor.plainRecordedNs);
        // What the same call costs on a frame that is not sampled: a skipped scope, or a counted call's gate check.
        const double skipNs = kInfo[p].clocked ? r.floor.skipNs : r.floor.countSkipNs;
        // The render thread: clocked calls are counted every frame; kEval's counts only on sampled frames
        // (on the others its calls pay the gate check).
        c.renderMs += static_cast<double>(r.part[p].renderCalls) / frames * clockedNs * 1e-6;
        if (!kInfo[p].clocked)
            c.renderMs += (1.0 - fraction) * static_cast<double>(r.part[p].renderCalls) / sampled * skipNs * 1e-6;
        c.renderRecordedMs += static_cast<double>(r.part[p].renderCalls) / frames * recordedNs * 1e-6;
        // The other threads: calls per sampled frame; the same calls happen on the other frames and cost the skip.
        const double perSampled = static_cast<double>(r.part[p].otherCalls) / sampled;
        c.otherMs += fraction * perSampled * clockedNs * 1e-6 + (1.0 - fraction) * perSampled * skipNs * 1e-6;
        c.otherRecordedMs += perSampled * recordedNs * 1e-6;
    }
    c.totalMs = c.renderMs + c.otherMs;
    return c;
}

// Line 1: the scheme, and the totals.
inline size_t formatSummary(char* buf, size_t cap, const WindowReport& r) noexcept {
    size_t len = 0;
    if (!buf || !cap) return 0;
    buf[0] = 0;
    uint64_t renderCalls = 0, otherCalls = 0;   // the clocked scopes; the evaluator's counted calls are on the parts lines
    for (unsigned p = 0; p < kParts; ++p) {
        if (!kInfo[p].clocked) continue;
        renderCalls += r.part[p].renderCalls;
        otherCalls += r.part[p].otherCalls;
    }
    const double frames = r.frames ? static_cast<double>(r.frames) : 1.0;
    const double seconds = r.seconds > 0.0 ? r.seconds : 1.0;
    appendf(buf, cap, len, "engine motion CPU: %.0f s, %u frames; EDVR's own work in its hooks on the game's code, the game's "
                           "forwarded code excluded. ", r.seconds, r.frames);
    appendf(buf, cap, len, "Render thread %u: every call clocked on every frame, total p50 %.2f / p95 %.2f / max %.2f ms per "
                           "frame over %.1f calls per frame. ",
            r.renderTid, r.totalP50, r.totalP95, r.totalMax, static_cast<double>(renderCalls) / frames);
    if (r.samplingOff) {
        appendf(buf, cap, len, "Sampling is OFF (the thread id read failed its self-check): every thread was clocked on every "
                               "frame. Other threads (%u): mean %.2f ms per frame over %.1f calls.",
                r.otherThreads, r.otherMs / frames, static_cast<double>(otherCalls) / frames);
    } else if (!r.sampledFrames) {
        appendf(buf, cap, len, "Other threads: no frame of this window was sampled (one in %u), so they have no figures.",
                r.samplePeriod);
    } else {
        const double sampled = static_cast<double>(r.sampledFrames);
        appendf(buf, cap, len,
                "Other threads (%u) are clocked only on the %u sampled frames (one in %u, at a random position in each block "
                "of %u; on the other frames a hook is one flag check and is not counted), so these are per sampled frame: "
                "mean %.2f ms (p50 %.2f / p95 %.2f / max %.2f) over %.1f calls, about %.1f ms per s.",
                r.otherThreads, r.sampledFrames, r.samplePeriod, r.samplePeriod, r.otherMs / sampled, r.otherP50, r.otherP95,
                r.otherMax, static_cast<double>(otherCalls) / sampled, r.otherMs / sampled * frames / seconds);
    }
    if (r.overflowThreads)
        appendf(buf, cap, len, " %u threads past %u share one counter: their figures are approximate.", r.overflowThreads,
                kMaxSlots);
    return len;
}

// Line 2: the clock, and what the instrument costs.
inline size_t formatClock(char* buf, size_t cap, const WindowReport& r) noexcept {
    size_t len = 0;
    if (!buf || !cap) return 0;
    buf[0] = 0;
    if (!r.floor.measured) {
        appendf(buf, cap, len, "engine motion CPU, clock and cost: not measured.");
        return len;
    }
    const InstrumentCost c = instrumentCost(r);
    appendf(buf, cap, len,
            "engine motion CPU, clock and cost: a clocked scope records %.0f ns and costs %.0f ns (%.0f and %.0f ns with a "
            "forward pause), a skipped one %.1f ns, a counted call %.1f ns (the fastest of %u batches of %u null pairs, "
            "measured as this window closed). The render thread's figures include about %.3f ms per frame of that floor, the "
            "other threads' about %.3f ms per sampled frame; the instrument costs about %.3f ms per frame (render thread "
            "%.3f, other threads %.3f).",
            r.floor.plainRecordedNs, r.floor.plainCostNs, r.floor.pausedRecordedNs, r.floor.pausedCostNs, r.floor.skipNs,
            r.floor.countNs, r.floor.batches, r.floor.pairs, c.renderRecordedMs, c.otherRecordedMs, c.totalMs, c.renderMs,
            c.otherMs);
    return len;
}

// The shader patches across both classes of thread: count, total and longest.
struct PatchTotals {
    uint64_t count;
    double totalMs, longestMs;
};
inline PatchTotals patchTotals(const WindowReport& r) noexcept {
    const PartWindow& w = r.part[kPatch];
    return {w.renderCalls + w.otherCalls, w.renderMs + w.otherMs, (std::max)(w.renderCallMaxMs, w.otherCallMaxMs)};
}

// kEval: counted on the sampled frames, none clocked but the probe branches. The
// words say so; a bare "eval 0.00" would read as measured.
inline void appendEvalNote(char* buf, size_t cap, size_t& len, uint64_t calls, double sampledFrames, double msPerFrame) noexcept {
    if (!calls) {
        appendf(buf, cap, len, "eval -");
        return;
    }
    char perFrame[24];
    callsText(perFrame, sizeof(perFrame), static_cast<double>(calls) / sampledFrames);
    if (msPerFrame > 0.0)
        appendf(buf, cap, len, "eval %s calls per sampled frame, not clocked (a pass-through; probe work clocked %.2f ms)",
                perFrame, msPerFrame);
    else
        appendf(buf, cap, len, "eval %s calls per sampled frame, not clocked (a pass-through)", perFrame);
}

// Line 3: the render thread's parts, exact.
inline size_t formatRenderParts(char* buf, size_t cap, const WindowReport& r) noexcept {
    size_t len = 0;
    if (!buf || !cap) return 0;
    buf[0] = 0;
    const double frames = r.frames ? static_cast<double>(r.frames) : 1.0;
    const double sampled = r.sampledFrames ? static_cast<double>(r.sampledFrames) : 1.0;
    appendf(buf, cap, len,
            "engine motion CPU, render thread, every call clocked, ms per frame p50/p95/max (calls per frame; longest call ms), "
            "\"-\" = the code never ran on this thread in the window: ");
    for (unsigned i = 0; i < kParts; ++i) {
        const unsigned p = kOrder[i];
        const PartWindow& w = r.part[p];
        if (i) appendf(buf, cap, len, ", ");
        if (p == kEval) {
            appendEvalNote(buf, cap, len, w.renderCalls, sampled, w.renderMs / sampled);
        } else if (!w.renderCalls) {
            appendf(buf, cap, len, "%s -", kInfo[p].name);
        } else {
            char perFrame[24];
            callsText(perFrame, sizeof(perFrame), static_cast<double>(w.renderCalls) / frames);
            appendf(buf, cap, len, "%s %.2f/%.2f/%.2f (%s; %.2f)", kInfo[p].name, w.renderP50, w.renderP95, w.renderMax,
                    perFrame, w.renderCallMaxMs);
        }
    }
    const PatchTotals pt = patchTotals(r);
    if (pt.count)
        appendf(buf, cap, len, "; shader patches this window on every thread: %llu, total %.2f ms, longest %.2f ms.",
                static_cast<unsigned long long>(pt.count), pt.totalMs, pt.longestMs);
    else
        appendf(buf, cap, len, "; shader patches this window: none (no cache miss).");
    return len;
}

// Line 4: the other threads' parts, per sampled frame.
inline size_t formatOtherParts(char* buf, size_t cap, const WindowReport& r) noexcept {
    size_t len = 0;
    if (!buf || !cap) return 0;
    buf[0] = 0;
    const double sampled = r.sampledFrames ? static_cast<double>(r.sampledFrames) : 1.0;
    appendf(buf, cap, len,
            "engine motion CPU, other threads (%u), the %u sampled frames only, thread-ms per sampled frame (calls per "
            "sampled frame; longest call ms), \"-\" = the code never ran off the render thread in the sampled frames: ",
            r.otherThreads, r.sampledFrames);
    for (unsigned i = 0; i < kParts; ++i) {
        const unsigned p = kOrder[i];
        const PartWindow& w = r.part[p];
        if (i) appendf(buf, cap, len, ", ");
        if (p == kEval) {
            appendEvalNote(buf, cap, len, w.otherCalls, sampled, w.otherMs / sampled);
        } else if (!w.otherCalls) {
            appendf(buf, cap, len, "%s -", kInfo[p].name);
        } else {
            char perFrame[24];
            callsText(perFrame, sizeof(perFrame), static_cast<double>(w.otherCalls) / sampled);
            appendf(buf, cap, len, "%s %.2f (%s; %.2f)", kInfo[p].name, w.otherMs / sampled, perFrame, w.otherCallMaxMs);
        }
    }
    appendf(buf, cap, len, ".");
    return len;
}

}  // namespace emcpu
}  // namespace edvr
