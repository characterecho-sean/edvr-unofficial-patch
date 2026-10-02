// The flat profile's CPU census: what EDVR itself spends on the render thread (and, sampled,
// on the game's other threads) per frame, by the same names the rest of the flat log uses.
//
// WHY. Three users report a large frame-rate loss with a temporal mode selected: two with
// every frame refused (docs section 79), one with frames treated (7-13 fps against 52-56 with
// anti-aliasing off, EDVR's GPU census reading 0.1 ms a frame). The GPU census cannot price
// the flat path -- it does not time the flat resolver or engine motion's substituted draws --
// and the engine-motion CPU recorder (engine_motion_cpu.h) is cut from perfMonitorFrame,
// which the flat menu tick never reaches, so nothing in a flat log says where EDVR's CPU
// goes. reviews/flat-motion-cpu-review-2026-09-29.md names the candidates; this prices all
// of them in one flight, and changes no behaviour.
//
// HOW. Modelled on engine_motion_cpu.h (read its header for the reasoning): a scope reads
// the clock on entry and exit and records its EXCLUSIVE time (a nested scope's whole wall
// time is subtracted from its parent, so the families partition the time and no nanosecond
// is in two of them) and one call, into a per-thread cell -- a relaxed load, add and store
// on a cache line only that thread writes, no lock and no contended atomic. The render
// thread (the one that calls Present) is clocked on one frame in kRenderPeriod, at a random
// position in each block of that many frames, and on a clocked frame every call is clocked: a
// per-call cost is exact, and a per-frame figure is the mean over the clocked frames, which the
// line counts. It was clocked on every call of every frame until 2026-09-30, when an on-foot
// flight showed about 50,000 scopes a frame costing 1.7 ms and putting 0.8 ms of that into the
// very total they measured. Every other thread is clocked only on SAMPLED frames, one in
// kSamplePeriod and a subset of the render thread's clocked frames, because the game's job
// threads make thousands of hook calls a frame; on the frames a thread is not clocked, a
// scope costs a load of the gate and a compare. The gate is one 64-bit word the render thread
// rewrites once a frame.
//
// ENGINE MOTION'S HOOKS ON THE GAME'S CODE (emit, rigid emit, copier, merge, clear, the job
// and builder brackets, the Map/Unmap tees) are already timed by engine_motion_cpu.h, at the
// same sites, with the same exclusive accounting and a pause around the game's own forwarded
// code. Only the VR frame tick drives that instrument. The census drives it here instead --
// it opens its gate with the census's own sampling decision and cuts it at the same edge -- and
// folds the parts into the line. The draw side (kDraw, and kApply and kPatch inside it) is NOT
// folded in: the flat draw scope calls it inside its own "engine motion draw wrapper" span.
//
// WHAT IT PRINTS. One line every 5 s while a temporal mode is selected, zeros included: the
// absence of the line means the block never ran (see formatWindow for the text; a window
// with more to say than the log line holds goes out as continuation lines). An "other"
// bucket is the time inside the outermost hook scopes that no named family owns. The
// instrument's own price is measured once, at the end of the first window (a null scope
// through the same gate, in the CPU state the flight ran in), and printed as the clock floor,
// because every clocked figure includes about one clock read.
//
// Header only, so tools\flat_temporal_test can drive every function with a fake clock
// (define EDVR_FLATCPU_NOW and EDVR_EMCPU_NOW before including it).
#pragma once

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine_motion_cpu.h"
#include "flat_query_cut.h"   // the counts of the questions answered from what the runtime tracks

#ifndef EDVR_FLATCPU_NOW
#define EDVR_FLATCPU_NOW() (::edvr::flatcpu::qpcNow())
#endif

namespace edvr {
namespace flatcpu {

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

// One frame in this many is a sampled frame for every thread but the render thread.
constexpr unsigned kSamplePeriod = 32;
// The render thread is clocked on one frame in this many, and the sampled frames above are
// among them (a frame that clocks every thread clocks the render thread too), so the render
// thread is clocked on 1/16 of the frames, not 1/16 and 1/32 of them.
constexpr unsigned kRenderPeriod = 16;
static_assert(kSamplePeriod % kRenderPeriod == 0, "the sampled frames are a subset of the render thread's clocked frames");
// The window the line covers.
constexpr int64_t kWindowMs = 5000;

// ---- the families ---------------------------------------------------------------------
// Named as the rest of the flat log names them. The comments say which entry points each
// one times; flat_runtime.cpp and its neighbours carry the scopes.
enum Family : unsigned {
    kOther = 0,     // the outermost hook scopes' own time: the draw scope's shell, the dispatch scope
    kReduce,        // contract reduction: flatRuntimeObserve / flatRuntimeObserveContract (the reducer)
    kCopyChecks,    // the exact-shader verifications on the copy draws (D3D state reads) and the F10-only captures
    kCameraRows,    // camera lookup and hashing: the draw scope's camera table lookup and rows hash, capture()
    kTrace,         // trace-ring copies: flatTraceRecord, flatTraceMark
    kResource,      // Map/Unmap/Update/Written: prefix target and source lookup, camera lookup
    kCoverage,      // coverage classification of scene draws
    kProjection,    // projection readiness: qualifyProjection (checks, preflight, prepare) and the jitter binding
    kShadows,       // constant-buffer shadow tracking on every write (the projection runtime's observers)
    kWitness,       // the camera-write witness (a stack walk per write until bounded)
    kEngineDraw,    // engine motion's draw wrapper: BeforeDraw / AfterFlatDraw, MRT save and restore, substitution
    kResolve,       // the treatment at the copy draw: the resolver's CPU, the sharpen pass, the replacement
    kBackend,       // the backend evaluation inside the resolve (NGX, FSR3, or the TAA dispatch)
    kDiscovery,     // the passive discovery observers (flatTemporal*)
    kTrackers,      // the O(1) state trackers: constant-buffer binds, viewport, ClearState, UAV binds
    kHdrRoute,      // the HDR route's trigger detector (flat_hdr_route.h) on every draw, observe-only with the key off
    kInject,        // the camera inject callback (refreshPre / refreshPost): game threads
    kFamilies
};
struct FamilyInfo {
    const char* name;
    bool gameThreads;   // its other-thread time is listed by name, per SAMPLED frame
};
inline constexpr FamilyInfo kInfo[kFamilies] = {
    {"other", false},
    {"contract reduction", false},
    {"copy checks", false},
    {"camera rows", false},
    {"trace ring", false},
    {"resource lookup", false},
    {"coverage", false},
    {"projection readiness", false},
    {"cb shadows", false},
    {"camera witness", false},
    {"engine motion draw wrapper", false},
    {"resolve", false},
    {"backend", false},
    {"discovery", false},
    {"state trackers", false},
    {"hdr route", false},
    {"camera inject", true},
};
constexpr unsigned kCal = kFamilies;   // the calibration cell, never reported
constexpr unsigned kCells = kFamilies + 1;

// The engine-motion parts folded into the line: every part but the draw side (see above).
constexpr bool emFolded(unsigned part) noexcept {
    return part != emcpu::kDraw && part != emcpu::kApply && part != emcpu::kPatch;
}
// The names the line uses for them, in the order it prints them.
struct EmName {
    unsigned part;
    const char* name;
};
inline constexpr EmName kEmNames[] = {
    {emcpu::kEmit, "engine emit"},     {emcpu::kRigid, "engine rigid emit"}, {emcpu::kCopier, "engine copier"},
    {emcpu::kMerge, "engine merge"},   {emcpu::kClear, "engine clear"},      {emcpu::kJobs, "engine jobs"},
    {emcpu::kBuilder, "engine builder"}, {emcpu::kTee, "engine tees"},
};
constexpr unsigned kEmNameCount = sizeof(kEmNames) / sizeof(kEmNames[0]);

// ---- the gate -------------------------------------------------------------------------
// Bit 63: this frame is a sampled frame (every thread is clocked). Bits 0-31: the id of the
// one thread clocked whatever the bit says: the render thread on a frame it is clocked on,
// kNoThread on a frame it is not (a thread id no thread has, so the compare fails for all of
// them, and the same word the calibration uses for a scope nobody clocks). Written by the
// render thread once a frame.
constexpr uint64_t kSampledBit = uint64_t(1) << 63;
constexpr uint32_t kNoThread = 0xFFFFFFFFu;
inline std::atomic<uint64_t> g_gate{0};
static_assert(std::atomic<uint64_t>::is_always_lock_free, "the gate and the counters are plain moves on x64");

inline uint32_t currentThreadId() noexcept {
#if defined(_M_X64)
    return static_cast<uint32_t>(__readgsdword(0x48));   // the TEB's ClientId.UniqueThread
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
// Sampled and never-render gates, for the calibration.
inline std::atomic<uint64_t> g_calGate{kSampledBit};

// ---- the per-thread counters ------------------------------------------------------------
struct alignas(32) Cell {
    std::atomic<uint64_t> ticks{0};   // exclusive time, running total
    std::atomic<uint64_t> calls{0};
    uint64_t spare_[2] = {};
};
struct alignas(64) Slot {
    Cell cell[kCells];
    std::atomic<uint32_t> tid{0};
};
constexpr unsigned kMaxSlots = 64;
inline Slot g_slots[kMaxSlots];
inline Slot g_overflow;   // threads past kMaxSlots share this one: approximate
inline std::atomic<unsigned> g_slotCount{0};

struct ThreadCtx {
    Slot* slot;
    int64_t child;   // the direct children's wall time inside the scope now open on this thread
};
inline thread_local ThreadCtx t_ctx{nullptr, 0};

__declspec(noinline) inline Slot* registerSlot() noexcept {
    ThreadCtx& c = t_ctx;
    if (c.slot) return c.slot;
    const unsigned i = g_slotCount.fetch_add(1, std::memory_order_acq_rel);
    Slot* s = i < kMaxSlots ? &g_slots[i] : &g_overflow;
    s->tid.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_relaxed);
    c.slot = s;
    return s;
}
inline unsigned slotCount() noexcept { return (std::min)(g_slotCount.load(std::memory_order_acquire), kMaxSlots); }

// ---- the scope ----------------------------------------------------------------------------
// RAII. A scope the gate did not clock is `live_ == false`: construction set one byte and the
// destructor returns at once.
class Scope {
public:
    explicit Scope(unsigned family) noexcept { open(g_gate, family); }
    // The calibration and the rig give the gate.
    Scope(unsigned family, const std::atomic<uint64_t>& gate) noexcept { open(gate, family); }
    ~Scope() {
        if (!live_) return;
        const int64_t now = EDVR_FLATCPU_NOW();
        int64_t excl = (now - begin_) - ctx_->child;
        if (excl < 0) excl = 0;
        Cell& c = slot_->cell[family_];
        c.ticks.store(c.ticks.load(std::memory_order_relaxed) + static_cast<uint64_t>(excl), std::memory_order_relaxed);
        c.calls.store(c.calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        // The parent sees this scope's whole wall time as its child.
        ctx_->child = savedChild_ + (now - begin_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    void open(const std::atomic<uint64_t>& gate, unsigned family) noexcept {
        if (!gateClocks(gate.load(std::memory_order_relaxed))) return;
        ThreadCtx& c = t_ctx;
        Slot* s = c.slot;
        if (!s) s = registerSlot();
        live_ = true;
        ctx_ = &c;
        slot_ = s;
        family_ = static_cast<uint8_t>(family);
        savedChild_ = c.child;
        c.child = 0;
        begin_ = EDVR_FLATCPU_NOW();   // last: the bookkeeping above is outside the timed span
    }
    bool live_ = false;
    uint8_t family_ = 0;
    ThreadCtx* ctx_ = nullptr;
    Slot* slot_ = nullptr;
    int64_t begin_ = 0, savedChild_ = 0;
};

// ---- the render thread's per-frame cut -----------------------------------------------------
struct CutState {
    uint64_t ticks[kFamilies];
    uint64_t calls[kFamilies];
};
inline CutState g_cut[kMaxSlots + 1];

struct FrameCut {
    uint64_t renderTicks[kFamilies];
    uint64_t renderCalls[kFamilies];
    uint64_t otherTicks[kFamilies];
    uint64_t otherCalls[kFamilies];
};
// Called by the thread that calls Present, once a frame: deltas since the previous cut, that
// thread's slot against everyone else's.
inline void cutFrame(FrameCut& out) noexcept {
    std::memset(&out, 0, sizeof(out));
    Slot* me = t_ctx.slot;
    if (!me) me = registerSlot();
    const unsigned n = slotCount();
    for (unsigned i = 0; i <= n; ++i) {
        Slot* s = i < n ? &g_slots[i] : &g_overflow;
        const unsigned index = i < n ? i : kMaxSlots;
        CutState& st = g_cut[index];
        const bool render = s == me;
        for (unsigned f = 0; f < kFamilies; ++f) {
            const uint64_t t = s->cell[f].ticks.load(std::memory_order_relaxed);
            const uint64_t k = s->cell[f].calls.load(std::memory_order_relaxed);
            const uint64_t dt = t - st.ticks[f], dk = k - st.calls[f];
            st.ticks[f] = t;
            st.calls[f] = k;
            if (render) { out.renderTicks[f] += dt; out.renderCalls[f] += dk; }
            else { out.otherTicks[f] += dt; out.otherCalls[f] += dk; }
        }
    }
}

// The rig only: forget every slot and clock every scope on every thread (this instrument's
// and engine motion's, which the census drives).
inline void resetForTest() noexcept {
    for (unsigned i = 0; i < kMaxSlots; ++i) {
        Slot& s = g_slots[i];
        for (unsigned f = 0; f < kCells; ++f) { s.cell[f].ticks.store(0); s.cell[f].calls.store(0); }
        s.tid.store(0);
    }
    for (unsigned f = 0; f < kCells; ++f) { g_overflow.cell[f].ticks.store(0); g_overflow.cell[f].calls.store(0); }
    std::memset(g_cut, 0, sizeof(g_cut));
    g_slotCount.store(0);
    g_gate.store(kSampledBit);
    t_ctx.slot = nullptr;
    t_ctx.child = 0;
    emcpu::resetForTest();
}

// The pseudo-random schedule: exactly one sampled frame in each block of `period` frames, its
// position drawn afresh for every block (xorshift32, a fixed seed, so a rig can predict it).
class SampleSchedule {
public:
    explicit SampleSchedule(unsigned period = kSamplePeriod, uint32_t seed = 0x2545F491u) noexcept
        : period_(period ? period : 1u), rng_(seed ? seed : 1u) {}
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

// Which frames are clocked. The render thread on exactly one frame in each block of
// `renderPeriod`, at a position drawn afresh for every block; and of each `samplePeriod /
// renderPeriod` of those clocked frames, exactly one is a sampled frame (every thread clocked),
// chosen at random too. So a sampled frame is always a render-clocked one, the render thread
// is clocked on 1/renderPeriod of the frames, and the other threads on 1/samplePeriod, exactly
// one in each block of samplePeriod. Fixed seeds, so a rig can predict it.
class ClockSchedule {
public:
    struct Frame {
        bool render = false;   // the render thread is clocked on this frame
        bool all = false;      // every thread is (a sampled frame); implies render
    };
    explicit ClockSchedule(unsigned renderPeriod = kRenderPeriod, unsigned samplePeriod = kSamplePeriod) noexcept
        : render_(renderPeriod, 0x2545F491u),
          per_(renderPeriod && samplePeriod > renderPeriod ? samplePeriod / renderPeriod : 1u) {}
    Frame next() noexcept {
        Frame f;
        f.render = render_.next();
        if (!f.render) return f;
        if (clocked_ == 0) {
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            pick_ = rng_ % per_;
        }
        f.all = clocked_ == pick_;
        if (++clocked_ >= per_) clocked_ = 0;
        return f;
    }

private:
    SampleSchedule render_;
    unsigned per_, clocked_ = 0, pick_ = 0;
    uint32_t rng_ = 0x9E3779B9u;
};

// The clock floor: what a null scope costs (ns of wall time per scope, the best of a few tight
// batches: a preemption only ever adds) and records (ns of ticks written into a cell, which
// every clocked figure includes once per call).
struct Floor {
    bool measured = false;
    double costNs = 0, recordedNs = 0;
};
inline Floor calibrate(int64_t freq, unsigned pairs = 2048, unsigned batches = 4) noexcept {
    Floor f;
    if (freq <= 0 || !pairs || !batches) return f;
    Slot* s = t_ctx.slot;
    if (!s) s = registerSlot();
    int64_t best = 0;
    for (unsigned b = 0; b < batches; ++b) {
        const int64_t t0 = EDVR_FLATCPU_NOW();
        for (unsigned i = 0; i < pairs; ++i) { Scope null(kCal, g_calGate); }
        const int64_t t1 = EDVR_FLATCPU_NOW();
        if (!b || t1 - t0 < best) best = t1 - t0;
    }
    f.costNs = static_cast<double>(best) * 1e9 / static_cast<double>(freq) / static_cast<double>(pairs);
    const uint64_t r0 = s->cell[kCal].ticks.load(std::memory_order_relaxed);
    const uint64_t c0 = s->cell[kCal].calls.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < pairs; ++i) { Scope null(kCal, g_calGate); }
    const uint64_t r1 = s->cell[kCal].ticks.load(std::memory_order_relaxed);
    const uint64_t c1 = s->cell[kCal].calls.load(std::memory_order_relaxed);
    f.recordedNs = c1 > c0 ? static_cast<double>(r1 - r0) * 1e9 / static_cast<double>(freq) / static_cast<double>(c1 - c0) : 0.0;
    f.measured = true;
    return f;
}

// ---- one window -------------------------------------------------------------------------------
struct WindowReport {
    bool valid = false;
    double seconds = 0;
    unsigned frames = 0, pausedFrames = 0, sampledFrames = 0;
    // The frames the render thread was clocked on (a sampled frame is one of them). Every render
    // figure below is a sum over these, and the line divides by this count, not by `frames`.
    unsigned renderClockedFrames = 0;
    unsigned renderPeriod = kRenderPeriod;
    unsigned samplePeriod = kSamplePeriod;
    // From every frame, clocked or not.
    double presentP50Ms = 0, presentP95Ms = 0;
    double renderMs[kFamilies] = {};
    double otherMs[kFamilies] = {};
    uint64_t renderCalls[kFamilies] = {};
    uint64_t otherCalls[kFamilies] = {};
    // Engine motion's parts (engine_motion_cpu.h), as the census cut them: window totals.
    double emRenderMs[emcpu::kParts] = {};
    double emOtherMs[emcpu::kParts] = {};
    uint64_t emRenderCalls[emcpu::kParts] = {};
    uint64_t emOtherCalls[emcpu::kParts] = {};
    // The engine-motion draw wrapper's D3D immediate-context calls in the window (state gets and
    // sets, clears and copies), and the draws it substituted, which they belong to.
    uint64_t stateCalls = 0, substitutedDraws = 0;
    // The questions the flat path answered from what the runtime already tracks instead of asking the D3D
    // context (flat_query_cut.h): how many, how many were also put to the context and compared, how many of
    // those comparisons were wrong, and which states ask the context again.
    FlatQueryCounts queries;
    // GPU spans, read back without waiting: the whole frame (first game draw to Present) and the
    // flat resolver's dispatches plus backend call. skipped: frames nothing could be timed for
    // (no free timer, or the frame was not watched); invalid: a sample the driver marked disjoint
    // or that expired unread.
    unsigned gpuFrameSamples = 0, gpuResolveSamples = 0;
    double gpuFrameP50 = 0, gpuFrameP95 = 0, gpuResolveP50 = 0, gpuResolveP95 = 0;
    uint64_t gpuSkipped = 0, gpuInvalid = 0;
    bool standingDown = false;   // the work is stood down as the window closes
    bool clocksOff = false;      // the census was built with its clocks off (clocksWanted())
    Floor floor;
    emcpu::Floor emFloor;
};

inline void percentiles(float* a, unsigned n, double* p50, double* p95) noexcept {
    *p50 = *p95 = 0.0;
    if (!n) return;
    float* end = a + n;
    unsigned i95 = static_cast<unsigned>(0.95 * static_cast<double>(n));
    if (i95 >= n) i95 = n - 1;
    const unsigned i50 = n / 2;
    std::nth_element(a, a + i95, end);
    *p95 = static_cast<double>(a[i95]);
    if (i50 >= i95) { *p50 = static_cast<double>(a[i95]); return; }
    std::nth_element(a, a + i50, a + i95);
    *p50 = static_cast<double>(a[i50]);
}

// Whether the census clocks anything. Under Proton an on-foot frame makes about 200,000 scopes,
// and clocking one costs that frame about 21 ms (2026-10-02): a hitch at a random frame in every
// block of kRenderPeriod, p95 present 37 ms against 16.7 ms at 60 Hz. The policy: on under Windows,
// as it always was; off under Wine (Proton), the only place the hitch is measured. ntdll exporting
// wine_get_version is how Wine is told apart. EDVR_FLAT_CPU_CLOCKS=1 or =0 decides it either way.
// With the clocks off the line still prints the present and GPU figures.
inline bool clocksPolicy(bool underWine, const char* env) noexcept {
    if (env && env[0] == '1') return true;
    if (env && env[0] == '0') return false;
    return !underWine;
}
inline bool clocksWanted() noexcept {
    static const bool on = [] {
        char v[4] = {};
        const bool set = GetEnvironmentVariableA("EDVR_FLAT_CPU_CLOCKS", v, sizeof v) > 0;
        const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        const bool underWine = ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
        return clocksPolicy(underWine, set ? v : nullptr);
    }();
    return on;
}

class Census {
public:
    static constexpr unsigned kMaxSamples = 2048;

    // `renderPeriod` 1 clocks the render thread on every frame, as the census did until 2026-09-30:
    // what the rig compares a sampled window with.
    // `clocks` false: no thread is clocked on any frame (clocksWanted()).
    explicit Census(unsigned renderPeriod = kRenderPeriod, bool clocks = true) noexcept
        : renderPeriod_(renderPeriod ? renderPeriod : 1u), schedule_(renderPeriod ? renderPeriod : 1u, kSamplePeriod),
          clocks_(clocks) {}

    // Once per Present, on the render thread. `paused`: the frame that just ended was a stood-down
    // Paused one. Decides the next frame's clocking and writes the gates (this instrument's and
    // engine motion's, which the census drives while it runs).
    void onFrame(int64_t nowTicks, int64_t freq, bool paused) noexcept {
        if (!primed_) {
            primed_ = true;
            freq_ = freq;
            lastTick_ = windowStart_ = nowTicks;
            samplingOk_ = currentThreadId() == static_cast<uint32_t>(GetCurrentThreadId());
            renderTid_ = static_cast<uint32_t>(GetCurrentThreadId());
            FrameCut prime;
            cutFrame(prime);   // baselines: nothing before the census opened is the first frame's
            emcpu::FrameCut emPrime;
            emcpu::cutFrame(emPrime);
            decideNextFrame();
            return;
        }
        FrameCut cut;
        cutFrame(cut);
        for (unsigned f = 0; f < kFamilies; ++f) {
            renderTicks_[f] += cut.renderTicks[f];
            renderCalls_[f] += cut.renderCalls[f];
            otherTicks_[f] += cut.otherTicks[f];
            otherCalls_[f] += cut.otherCalls[f];
        }
        emcpu::FrameCut emCut;
        emcpu::cutFrame(emCut);
        for (unsigned p = 0; p < emcpu::kParts; ++p) {
            emRenderTicks_[p] += emCut.renderTicks[p];
            emRenderCalls_[p] += emCut.renderCalls[p];
            emOtherTicks_[p] += emCut.otherTicks[p];
            emOtherCalls_[p] += emCut.otherCalls[p];
        }
        // The frame that just ended: clocked on the render thread, and sampled for every thread.
        if (renderNow_ || sampledNow_) ++renderClocked_;
        if (sampledNow_) ++sampled_;
        if (paused) ++paused_;
        const double ms = freq > 0 ? static_cast<double>(nowTicks - lastTick_) * 1000.0 / static_cast<double>(freq) : 0.0;
        lastTick_ = nowTicks;
        if (frames_ < kMaxSamples) interval_[frames_] = static_cast<float>(ms);
        ++frames_;
        decideNextFrame();
    }
    void noteGpuFrame(double ms) noexcept { if (gpuFrame_ < kMaxSamples) gpuFrameMs_[gpuFrame_++] = static_cast<float>(ms); }
    void noteGpuResolve(double ms) noexcept { if (gpuResolve_ < kMaxSamples) gpuResolveMs_[gpuResolve_++] = static_cast<float>(ms); }
    void noteGpuSkipped() noexcept { ++gpuSkipped_; }
    void noteGpuInvalid() noexcept { ++gpuInvalid_; }
    // Engine motion's draw wrapper, drained once a frame: the D3D calls it issued and the draws
    // it substituted (engine_velocity.h, engineVelocityTakeWrapperCounts).
    void noteWrapper(uint64_t stateCalls, uint64_t substitutedDraws) noexcept {
        stateCalls_ += stateCalls;
        substitutedDraws_ += substitutedDraws;
    }
    // The query shortcuts' counts for the frame that just ended (flat_query_cut.h, take()), drained with
    // the wrapper's.
    void noteQueries(const FlatQueryCounts& c) noexcept {
        for (unsigned i = 0; i < kFlatQueryCount; ++i) {
            queries_.served[i] += c.served[i];
            queries_.sampled[i] += c.sampled[i];
            queries_.asked[i] += c.asked[i];
            queries_.mismatched[i] += c.mismatched[i];
        }
        queries_.fellBack |= c.fellBack;
    }

    // The census stops when no temporal mode is selected: both gates close (a scope then costs a
    // load and a compare) and the next frame primes afresh.
    void idle() noexcept {
        if (!primed_) return;
        primed_ = false;
        g_gate.store(0, std::memory_order_relaxed);
        emcpu::g_gate.store(0, std::memory_order_relaxed);
        resetWindow();
    }
    bool running() const noexcept { return primed_; }

    // The window, when kWindowMs have passed: folded into out and emptied. False before that.
    bool take(int64_t nowTicks, bool standingDown, WindowReport& out) noexcept {
        if (!primed_ || freq_ <= 0) return false;
        if (nowTicks - windowStart_ < freq_ * kWindowMs / 1000) return false;
        if (!floorsMeasured_ && clocks_) {
            // The end of the first window: the CPU is in the state the flight runs in. With the clocks off
            // nothing is clocked, so nothing needs a floor, and calibrating would clock scopes on this thread.
            floor_ = calibrate(freq_);
            emFloor_ = emcpu::calibrate(freq_);
            floorsMeasured_ = true;
        }
        const double msPerTick = 1000.0 / static_cast<double>(freq_);
        out = WindowReport{};
        out.valid = frames_ > 0;
        out.seconds = static_cast<double>(nowTicks - windowStart_) / static_cast<double>(freq_);
        out.frames = frames_;
        out.pausedFrames = paused_;
        out.sampledFrames = sampled_;
        out.renderClockedFrames = renderClocked_;
        out.renderPeriod = samplingOk_ ? renderPeriod_ : 1u;
        out.samplePeriod = samplingOk_ ? kSamplePeriod : 1u;
        for (unsigned f = 0; f < kFamilies; ++f) {
            out.renderMs[f] = static_cast<double>(renderTicks_[f]) * msPerTick;
            out.otherMs[f] = static_cast<double>(otherTicks_[f]) * msPerTick;
            out.renderCalls[f] = renderCalls_[f];
            out.otherCalls[f] = otherCalls_[f];
        }
        for (unsigned p = 0; p < emcpu::kParts; ++p) {
            out.emRenderMs[p] = static_cast<double>(emRenderTicks_[p]) * msPerTick;
            out.emOtherMs[p] = static_cast<double>(emOtherTicks_[p]) * msPerTick;
            out.emRenderCalls[p] = emRenderCalls_[p];
            out.emOtherCalls[p] = emOtherCalls_[p];
        }
        out.stateCalls = stateCalls_;
        out.substitutedDraws = substitutedDraws_;
        out.queries = queries_;
        percentiles(interval_, (std::min)(frames_, kMaxSamples), &out.presentP50Ms, &out.presentP95Ms);
        out.gpuFrameSamples = gpuFrame_;
        percentiles(gpuFrameMs_, gpuFrame_, &out.gpuFrameP50, &out.gpuFrameP95);
        out.gpuResolveSamples = gpuResolve_;
        percentiles(gpuResolveMs_, gpuResolve_, &out.gpuResolveP50, &out.gpuResolveP95);
        out.gpuSkipped = gpuSkipped_;
        out.gpuInvalid = gpuInvalid_;
        out.standingDown = standingDown;
        out.clocksOff = !clocks_;
        out.floor = floor_;
        out.emFloor = emFloor_;
        resetWindow();
        windowStart_ = nowTicks;
        return true;
    }
    const Floor& floor() const noexcept { return floor_; }

private:
    void decideNextFrame() noexcept {
        if (!clocks_) {
            renderNow_ = sampledNow_ = false;
        } else if (samplingOk_) {
            const ClockSchedule::Frame next = schedule_.next();
            renderNow_ = next.render;
            sampledNow_ = next.all;
        } else {
            renderNow_ = sampledNow_ = true;   // no thread id to compare with: every frame clocks everything
        }
        // A frame that clocks every thread clocks the render thread too. On a frame that clocks
        // it, the gate names it; on one that does not, it names no thread and no one is clocked.
        const uint64_t gate = makeGate(sampledNow_, renderNow_ || sampledNow_ ? renderTid_ : kNoThread);
        g_gate.store(gate, std::memory_order_relaxed);
        emcpu::g_gate.store(gate, std::memory_order_relaxed);
    }
    void resetWindow() noexcept {
        frames_ = paused_ = sampled_ = renderClocked_ = 0;
        std::memset(renderTicks_, 0, sizeof(renderTicks_));
        std::memset(renderCalls_, 0, sizeof(renderCalls_));
        std::memset(otherTicks_, 0, sizeof(otherTicks_));
        std::memset(otherCalls_, 0, sizeof(otherCalls_));
        std::memset(emRenderTicks_, 0, sizeof(emRenderTicks_));
        std::memset(emRenderCalls_, 0, sizeof(emRenderCalls_));
        std::memset(emOtherTicks_, 0, sizeof(emOtherTicks_));
        std::memset(emOtherCalls_, 0, sizeof(emOtherCalls_));
        stateCalls_ = substitutedDraws_ = 0;
        queries_ = FlatQueryCounts{};
        gpuFrame_ = gpuResolve_ = 0;
        gpuSkipped_ = gpuInvalid_ = 0;
    }
    bool primed_ = false, samplingOk_ = true, sampledNow_ = false, renderNow_ = false, floorsMeasured_ = false;
    int64_t freq_ = 0, lastTick_ = 0, windowStart_ = 0;
    uint32_t renderTid_ = 0;
    unsigned renderPeriod_;
    bool clocks_ = true;
    Floor floor_;
    emcpu::Floor emFloor_;
    ClockSchedule schedule_;
    unsigned frames_ = 0, paused_ = 0, sampled_ = 0, renderClocked_ = 0, gpuFrame_ = 0, gpuResolve_ = 0;
    uint64_t renderTicks_[kFamilies] = {}, renderCalls_[kFamilies] = {};
    uint64_t otherTicks_[kFamilies] = {}, otherCalls_[kFamilies] = {};
    uint64_t emRenderTicks_[emcpu::kParts] = {}, emRenderCalls_[emcpu::kParts] = {};
    uint64_t emOtherTicks_[emcpu::kParts] = {}, emOtherCalls_[emcpu::kParts] = {};
    uint64_t stateCalls_ = 0, substitutedDraws_ = 0, gpuSkipped_ = 0, gpuInvalid_ = 0;
    FlatQueryCounts queries_;
    float interval_[kMaxSamples];
    float gpuFrameMs_[kMaxSamples];
    float gpuResolveMs_[kMaxSamples];
};

// ---- the text ------------------------------------------------------------------------------------
// The log truncates a line at about 1,160 characters after its timestamp. The text is built as
// tokens (a family and its figures, one GPU span) and packed into lines of at most kLineLimit
// characters; a token never splits. The first line starts "flat cpu 5s:", the rest
// "flat cpu 5s (cont.):". A typical window is two lines.
constexpr size_t kLineLimit = 1090;
constexpr int kMaxLines = 4;
constexpr int kMaxTokens = 64;
struct Lines {
    char line[kMaxLines][1400];
    int count = 0;
    bool truncated = false;   // more tokens than kMaxLines lines hold (the rig asserts this never happens)
};
struct Tokens {
    struct Token {
        char text[320];
        const char* sep;   // between this token and the one before it, on the same line
    };
    Token t[kMaxTokens];
    int n = 0;
    template <class... A>
    void add(const char* sep, const char* fmt, A... args) {
        if (n >= kMaxTokens) return;
        std::snprintf(t[n].text, sizeof(t[n].text), fmt, args...);
        t[n].sep = sep;
        ++n;
    }
};
// Calls per frame: three decimals when a rare event would otherwise round to nothing, one
// under a hundred, none above.
inline void callsText(char* out, size_t cap, double perFrame) noexcept {
    std::snprintf(out, cap, perFrame > 0.0 && perFrame < 0.1 ? "%.3f" : perFrame < 100.0 ? "%.1f" : "%.0f", perFrame);
}
inline void packLines(const Tokens& tk, Lines* out) {
    out->count = 0;
    out->truncated = false;
    size_t used = 0;
    for (int i = 0; i < tk.n; ++i) {
        const size_t textLen = std::strlen(tk.t[i].text);
        const size_t sepLen = out->count > 0 ? std::strlen(tk.t[i].sep) : 0;
        if (out->count > 0 && used + sepLen + textLen <= kLineLimit) {
            std::snprintf(out->line[out->count - 1] + used, sizeof(out->line[0]) - used, "%s%s", tk.t[i].sep, tk.t[i].text);
            used += sepLen + textLen;
            continue;
        }
        if (out->count >= kMaxLines) { out->truncated = true; break; }
        char* dst = out->line[out->count];
        const int w = std::snprintf(dst, sizeof(out->line[0]), "%s%s", out->count == 0 ? "" : "flat cpu 5s (cont.): ", tk.t[i].text);
        used = w > 0 ? static_cast<size_t>(w) : 0;
        ++out->count;
    }
}
// The clocks' price on the render thread: every clocked scope of this instrument and of engine
// motion's (its draw side included -- it is clocked even though the fold does not count it twice)
// weighed by its calibrated floor. An estimate: the floor is measured in tight loops. The scope
// counts are sums over the clocked frames, so a clocked frame's price is the sum over the scopes
// divided by the clocked frames; only those frames pay it, so what the run paid a frame is that
// price times the share of frames that were clocked. (A scope on a frame it is not clocked on
// costs a load of the gate and a compare, well under a nanosecond in a tight loop: not counted.)
struct InstrumentCost {
    double costMs = 0;          // a frame, on average over every frame of the window: what the run paid
    double clockedMs = 0;       // a clocked frame
    double recordedMs = 0;      // a clocked frame: the clock reads inside the per-clocked-frame total
    double scopesPerFrame = 0;  // a clocked frame
};
inline InstrumentCost instrumentCost(const WindowReport& r) noexcept {
    InstrumentCost c;
    if (!r.floor.measured || !r.frames || !r.renderClockedFrames) return c;
    const double clocked = static_cast<double>(r.renderClockedFrames);
    for (unsigned f = 0; f < kFamilies; ++f) {
        const double perFrame = static_cast<double>(r.renderCalls[f]) / clocked;
        c.scopesPerFrame += perFrame;
        c.clockedMs += perFrame * r.floor.costNs * 1e-6;
        c.recordedMs += perFrame * r.floor.recordedNs * 1e-6;
    }
    if (r.emFloor.measured) {
        for (unsigned p = 0; p < emcpu::kParts; ++p) {
            if (!emcpu::kInfo[p].clocked) continue;
            const double perFrame = static_cast<double>(r.emRenderCalls[p]) / clocked;
            const bool paused = emcpu::kInfo[p].paused;
            c.scopesPerFrame += perFrame;
            c.clockedMs += perFrame * (paused ? r.emFloor.pausedCostNs : r.emFloor.plainCostNs) * 1e-6;
            c.recordedMs += perFrame * (paused ? r.emFloor.pausedRecordedNs : r.emFloor.plainRecordedNs) * 1e-6;
        }
    }
    const double share = (std::min)(1.0, clocked / static_cast<double>(r.frames));
    c.costMs = c.clockedMs * share;
    return c;
}
inline void formatWindow(const WindowReport& r, Lines* out) {
    Tokens tk;
    char calls[32];
    const double frames = r.frames ? static_cast<double>(r.frames) : 1.0;
    // The render thread is clocked on some of the frames only (see the schedule), so its figures are
    // sums over those and its per-frame figures divide by their number, the way the other threads'
    // divide by their sampled frames. "-" is a window with no clocked frame, never a zero.
    const bool anyClocked = r.renderClockedFrames > 0;
    const double clocked = anyClocked ? static_cast<double>(r.renderClockedFrames) : 1.0;
    // Everything the render thread spent in the families, plus engine motion's hooks on it.
    double emRenderMs = 0;
    uint64_t emRenderCalls = 0;
    for (unsigned p = 0; p < emcpu::kParts; ++p) {
        if (!emFolded(p) || !emcpu::kInfo[p].clocked) continue;
        emRenderMs += r.emRenderMs[p];
        emRenderCalls += r.emRenderCalls[p];
    }
    double totalMs = emRenderMs;
    for (unsigned f = 0; f < kFamilies; ++f) totalMs += r.renderMs[f];

    char total[32] = "-";
    if (anyClocked) std::snprintf(total, sizeof(total), "%.3f ms", totalMs / clocked);
    if (r.clocksOff)
        tk.add("", "flat cpu 5s: frames=%u (stood down %u%s) present p50 %.2f ms (p95 %.2f); clocks off, no thread clocked",
               r.frames, r.pausedFrames, r.standingDown ? ", stood down now" : "", r.presentP50Ms, r.presentP95Ms);
    else
        tk.add("", "flat cpu 5s: frames=%u (stood down %u%s) present p50 %.2f ms (p95 %.2f); render thread clocked on %u of %u frames, "
                   "one in %u (its figures are per clocked frame); EDVR per frame total %s",
               r.frames, r.pausedFrames, r.standingDown ? ", stood down now" : "", r.presentP50Ms, r.presentP95Ms,
               r.renderClockedFrames, r.frames, r.renderPeriod, total);
    if (anyClocked) {
        for (unsigned f = 0; f < kFamilies; ++f) {
            callsText(calls, sizeof(calls), static_cast<double>(r.renderCalls[f]) / clocked);
            tk.add(f == 0 ? " = " : " + ", "%s %.3f ms (calls %s)", kInfo[f].name, r.renderMs[f] / clocked, calls);
        }
        callsText(calls, sizeof(calls), static_cast<double>(emRenderCalls) / clocked);
        tk.add(" + ", "engine motion hooks %.3f ms (calls %s)", emRenderMs / clocked, calls);
    } else {
        if (r.clocksOff)
            tk.add(" = ", "render thread: clocks off (EDVR_FLAT_CPU_CLOCKS=1 turns them on)");
        else
            tk.add(" = ", "render thread: not clocked in this window (one frame in %u is, and none was)", r.renderPeriod);
    }

    // The game's other threads: per SAMPLED frame, thread-ms summed, because the rest of the
    // window they were not clocked. "-" is a window with no sampled frame, never a zero.
    if (r.clocksOff) {
        tk.add("; ", "other threads: clocks off");
    } else if (!r.sampledFrames) {
        tk.add("; ", "other threads: not clocked in this window (one frame in %u is, and none was)", r.samplePeriod);
    } else {
        const double sampled = static_cast<double>(r.sampledFrames);
        char inject[160];
        callsText(calls, sizeof(calls), static_cast<double>(r.otherCalls[kInject]) / sampled);
        std::snprintf(inject, sizeof(inject), "%s %.3f ms (calls %s)", kInfo[kInject].name, r.otherMs[kInject] / sampled, calls);
        tk.add("; ", "other threads (clocked on %u of %u frames, one in %u; thread-ms per clocked frame): %s",
               r.sampledFrames, r.frames, r.samplePeriod, inject);
        for (unsigned i = 0; i < kEmNameCount; ++i) {
            const unsigned p = kEmNames[i].part;
            callsText(calls, sizeof(calls), static_cast<double>(r.emOtherCalls[p]) / sampled);
            tk.add(" + ", "%s %.3f ms (calls %s)", kEmNames[i].name, r.emOtherMs[p] / sampled, calls);
        }
        // The evaluator relays are counted, on sampled frames only, never clocked.
        callsText(calls, sizeof(calls), static_cast<double>(r.emRenderCalls[emcpu::kEval] + r.emOtherCalls[emcpu::kEval]) / sampled);
        tk.add(" + ", "engine eval relays %s calls (counted, not clocked)", calls);
        double elsewhere = 0;
        for (unsigned f = 0; f < kFamilies; ++f) if (!kInfo[f].gameThreads) elsewhere += r.otherMs[f];
        tk.add(" + ", "every other family %.3f ms", elsewhere / sampled);
    }

    const double witnessUs = r.renderCalls[kWitness] ? r.renderMs[kWitness] * 1000.0 / static_cast<double>(r.renderCalls[kWitness]) : 0.0;
    tk.add("; ", "camera witness %.2f us/write (%llu writes clocked)", witnessUs, static_cast<unsigned long long>(r.renderCalls[kWitness]));
    tk.add("; ", "engine motion wrapper D3D calls %.0f/frame over %.1f substituted draws/frame",
           static_cast<double>(r.stateCalls) / frames, static_cast<double>(r.substitutedDraws) / frames);
    // The questions answered from what the runtime tracks rather than put to the D3D context (flat_query_cut.h): per
    // frame, and how many of them the one-frame-in-64 check also put to the context and found wrong. A state that was
    // found wrong asks the context again, and is named.
    tk.add("; ", "query shortcuts (answers a frame from what the runtime tracks; one frame in %u also asks the context and compares)",
           FlatQueryCut::kFramePeriod);
    for (unsigned i = 0; i < kFlatQueryCount; ++i) {
        tk.add(i ? ", " : ": ", "%s %.1f (checked %llu, wrong %llu)", flatQueryName(static_cast<FlatQuery>(i)),
               static_cast<double>(r.queries.served[i]) / frames, static_cast<unsigned long long>(r.queries.sampled[i]),
               static_cast<unsigned long long>(r.queries.mismatched[i]));
    }
    if (!r.queries.fellBack) {
        tk.add("; ", "no state asks the context again");
    } else {
        char names[200] = {};
        size_t used = 0;
        for (unsigned i = 0; i < kFlatQueryCount; ++i) {
            if (!(r.queries.fellBack & (1u << i))) continue;
            const int n = std::snprintf(names + used, sizeof(names) - used, "%s%s (%.1f a frame)", used ? ", " : "",
                                        flatQueryName(static_cast<FlatQuery>(i)),
                                        static_cast<double>(r.queries.asked[i]) / frames);
            if (n > 0 && static_cast<size_t>(n) < sizeof(names) - used) used += static_cast<size_t>(n);
        }
        tk.add("; ", "ASKS THE CONTEXT AGAIN: %s", names);
    }
    // The GPU spans, read back without waiting. No sample prints "-", never 0.00: a span that
    // was not timed is not a fast one.
    char frameText[64] = "-", resolveText[64] = "-";
    if (r.gpuFrameSamples) std::snprintf(frameText, sizeof(frameText), "p50 %.2f / p95 %.2f ms", r.gpuFrameP50, r.gpuFrameP95);
    if (r.gpuResolveSamples) std::snprintf(resolveText, sizeof(resolveText), "p50 %.2f / p95 %.2f ms", r.gpuResolveP50, r.gpuResolveP95);
    tk.add("; ", "GPU frame %s (first game draw to Present; %u timed, %llu skipped, %llu invalid)",
           frameText, r.gpuFrameSamples, static_cast<unsigned long long>(r.gpuSkipped), static_cast<unsigned long long>(r.gpuInvalid));
    tk.add("; ", "GPU resolve %s (%u timed)", resolveText, r.gpuResolveSamples);
    tk.add("; ", "clock floor %.0f ns/scope (records %.0f ns); engine hooks floor %.0f ns plain / %.0f ns paused (records %.0f / %.0f ns)",
           r.floor.costNs, r.floor.recordedNs, r.emFloor.plainCostNs, r.emFloor.pausedCostNs,
           r.emFloor.plainRecordedNs, r.emFloor.pausedRecordedNs);
    // The instrument's own price on the render thread at this window's call rates: what the clocks cost
    // a frame on average (only the clocked frames pay it), what a clocked frame costs, and how much of
    // the per-clocked-frame figures above is the clock reads themselves.
    InstrumentCost price = instrumentCost(r);
    tk.add("; ", "the clocks cost the render thread about %.3f ms a frame on average (%.3f ms on a clocked frame, %.0f scopes; "
                 "the gate checks on the other frames are not counted) and put about %.3f ms of the per-clocked-frame total above into it",
           price.costMs, price.clockedMs, price.scopesPerFrame, price.recordedMs);
    packLines(tk, out);
}

}  // namespace flatcpu
}  // namespace edvr
