#include "lod_governor.h"

#include "kinematic_eval_hook.h"
#include "native_timing.h"
#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace edvr {

// --- The policy ------------------------------------------------------------------

namespace lodgov {

void Policy::configure(float kMax) noexcept {
    if (!(kMax >= 1.0f)) kMax = 1.0f;   // NaN too
    if (kMax > kMaxCeiling) kMax = kMaxCeiling;
    maxSteps_ = static_cast<int>(std::lround((kMax - 1.0f) * kQuantaPerUnit));
    if (steps_ > maxSteps_) clampPending_ = true;
}

void Policy::reset() noexcept {
    steps_ = 0;
    inSettlement_ = clampPending_ = stepped_ = false;
    over_ = under_ = sparse_ = 0;
    lastStepMs_ = 0;
}

Step Policy::update(const FrameSignals& s) noexcept {
    // A lowered k_max takes effect at once, whatever the signals say.
    if (clampPending_) {
        clampPending_ = false;
        if (steps_ > maxSteps_) {
            steps_ = maxSteps_;
            lastStepMs_ = s.nowMs;
            stepped_ = true;
            return Step::Clamp;
        }
    }
    // Density, with hysteresis: in at 200 records a frame; out only after
    // 30 consecutive frames under 150, and then k is 1 at once.
    if (s.records >= kSettlementRecords) {
        inSettlement_ = true;
        sparse_ = 0;
    } else if (inSettlement_ && s.records + kSettlementBand < kSettlementRecords) {
        if (++sparse_ >= kConsecutive) {
            inSettlement_ = false;
            sparse_ = over_ = under_ = 0;
            if (steps_ > 0) {
                steps_ = 0;
                lastStepMs_ = s.nowMs;
                stepped_ = true;
                return Step::Reset;
            }
            return Step::None;
        }
    } else {
        sparse_ = 0;
    }
    // Frame work: runs of consecutive samples over and under the budget. A
    // frame with no new sample holds both runs; a bad sample breaks both.
    if (s.work == Work::Invalid) {
        over_ = under_ = 0;
    } else if (s.work == Work::Valid) {
        over_ = s.workMs > s.periodMs + kOverMarginMs ? over_ + 1 : 0;
        under_ = s.workMs < s.periodMs - kUnderMarginMs ? under_ + 1 : 0;
    }
    if (stepped_ && s.nowMs - lastStepMs_ < kRampIntervalMs) return Step::None;
    if (inSettlement_ && s.records >= kSettlementRecords && over_ >= kConsecutive && steps_ < maxSteps_) {
        ++steps_;
        lastStepMs_ = s.nowMs;
        stepped_ = true;
        return Step::Up;
    }
    if (under_ >= kConsecutive && steps_ > 0) {
        --steps_;
        lastStepMs_ = s.nowMs;
        stepped_ = true;
        return Step::Down;
    }
    return Step::None;
}

}  // namespace lodgov

namespace {

// --- The counters: one slot per worker thread -----------------------------------
// Tens of thousands of part tests a frame run on the engine's workers. A
// shared atomic per counter would put a contended read-modify-write on every
// one of them, so each thread owns a slot and bumps it with a plain load and
// store (it is the only writer); the frame boundary sums every slot and keeps
// the previous sum, so a frame's count is the difference (u32, wrap-safe).
// Threads past kSlots share one slot with real atomic adds.
enum : uint32_t {
    cRecords, cParts, cPartForeign, cPartFaults, cPartTableRange, cRecordFaults, cRecordTableRange,
    cPartBase,
};
enum : uint32_t { pSeen, pPassed, pMismatch, pWouldDrop, pWouldChange, pHist0, pHist1, pHist2, pHist3, kPartFields };
constexpr uint32_t kClasses = 3;   // eye A, eye B, every other view
enum : uint32_t { rSeen, rWouldFail, rWouldChange, rMismatch, kRecFields };
constexpr uint32_t cRecBase = cPartBase + kClasses * kPartFields;   // eyes A and B
constexpr uint32_t cRecNotDispatched = cRecBase + 2 * kRecFields;
constexpr uint32_t cRecDispatchMismatch = cRecNotDispatched + 1;
constexpr uint32_t kCounters = cRecDispatchMismatch + 1;

// A whole number of cache lines, so no two threads' slots share one.
constexpr uint32_t kSlotWords = (kCounters + 15) / 16 * 16;
struct alignas(64) Slot {
    std::atomic<uint32_t> v[kSlotWords];
};
static_assert(sizeof(Slot) % 64 == 0, "a slot is whole cache lines");
constexpr uint32_t kSlots = 64;
Slot g_slots[kSlots];
Slot g_shared;
std::atomic<uint32_t> g_slotNext{0};
thread_local Slot* t_slot = nullptr;

Slot* mySlot() noexcept {
    Slot* s = t_slot;
    if (s) return s;
    const uint32_t i = g_slotNext.fetch_add(1, std::memory_order_acq_rel);
    s = i < kSlots ? &g_slots[i] : &g_shared;
    t_slot = s;
    return s;
}

inline void bump(Slot* s, uint32_t c) noexcept {
    if (s == &g_shared) s->v[c].fetch_add(1, std::memory_order_relaxed);
    else s->v[c].store(s->v[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void sumSlots(uint32_t out[kCounters]) noexcept {
    for (uint32_t c = 0; c < kCounters; ++c) out[c] = g_shared.v[c].load(std::memory_order_relaxed);
    const uint32_t claimed = g_slotNext.load(std::memory_order_acquire);
    const uint32_t n = claimed < kSlots ? claimed : kSlots;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = 0; c < kCounters; ++c) out[c] += g_slots[i].v[c].load(std::memory_order_relaxed);
}

// --- What the workers read and the boundary publishes -----------------------------
std::atomic<bool> g_live{false};                 // the observers do work
std::atomic<uint32_t> g_kBits{0x3F800000u};      // k as float bits, 1.0 while nothing has stepped
std::atomic<uint32_t> g_eyes{0xFFFFu};           // eye A's view bit | eye B's << 8; 0xFF unknown
std::atomic<uintptr_t> g_ctx{0};                 // the render context the builder saw last
std::atomic<uint32_t> g_scaleBits{0};            // its +0x30, the LOD scale s

inline float fromBits(uint32_t b) noexcept { float f; std::memcpy(&f, &b, 4); return f; }
inline uint32_t toBits(float f) noexcept { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline float currentK() noexcept { return fromBits(g_kBits.load(std::memory_order_relaxed)); }

inline uint32_t classOf(uint32_t bit) noexcept {
    const uint32_t eyes = g_eyes.load(std::memory_order_relaxed);
    if (bit == (eyes & 0xFFu)) return 0;
    if (bit == ((eyes >> 8) & 0xFFu)) return 1;
    return 2;
}

// --- The engine reads (SEH: a wild pointer drops the observation, never the flight) --
// POD locals only in the __try functions (/EHs units cannot unwind C++
// objects through __try; cull_gate_probe.cpp's rule).

// FUN_1442B3FC0 after its forward: param_1 = items (the builder's six-pointer
// block: [0] centre, [1] the model's +0x10 copy, [4] the LOD table copy, [5]
// the render context), param_2 = out {u32 LOD, u8 passed}, param_3 = view.
bool readPart(uintptr_t items, uintptr_t out, uintptr_t view, lodgov::PartInputs* in) noexcept {
    __try {
        uint64_t block[6];
        std::memcpy(block, reinterpret_cast<const void*>(items), sizeof(block));
        uint8_t pass = 0;
        std::memcpy(&in->engineLod, reinterpret_cast<const void*>(out), 4);
        std::memcpy(&pass, reinterpret_cast<const void*>(out + 4), 1);
        in->enginePass = pass != 0;
        std::memcpy(in->centre, reinterpret_cast<const void*>(block[0]), 16);
        std::memcpy(&in->radius, reinterpret_cast<const void*>(block[1]), 4);
        std::memcpy(in->cam, reinterpret_cast<const void*>(view + 0x540), 16);
        std::memcpy(&in->A, reinterpret_cast<const void*>(view + 0x550), 4);
        std::memcpy(&in->B, reinterpret_cast<const void*>(view + 0x560), 4);
        uint64_t bits = 0;
        std::memcpy(&bits, reinterpret_cast<const void*>(view + 0x570), 8);
        std::memcpy(&in->s, reinterpret_cast<const void*>(block[5] + 0x30), 4);
        in->table = lodgov::LodTable::fromBytes(reinterpret_cast<const uint8_t*>(block[4]));
        unsigned long index = 0;
        in->bit = _BitScanForward64(&index, bits) ? static_cast<uint32_t>(index) : 64u;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct RecordEye {
    bool seen, mismatch, wouldFail, wouldChange;
};
struct RecordOutcome {
    RecordEye eye[2];
    bool dispatchMismatch, notDispatched, tableRange;
    uint32_t scaleBits;
};

// FUN_144308B30 (decomp_4308B30.txt) on the record the builder is about to
// build, from the inputs the traversal handed it (decomp_4312040.txt:154-165:
// centre rec+0x240, radius rec+0x280, table *(rec+0x20), the context's +0x30)
// and against the results the traversal stored and the builder's dispatch
// read (rec+0x208 mask, rec+0x210 nibbles by view bit, node +0x6A LOD count;
// decomp_4320340.txt:64-95). Per view bit: view = ctx + 0x40 + 0x6A0 *
// (u32 at ctx+0x1A840 + 4*bit), camera +0x540, A +0x550, B +0x560.
bool shadowRecord(uintptr_t ctx, uintptr_t nibbles, uint32_t eyes, float k, RecordOutcome* o) noexcept {
    __try {
        if (nibbles < 0x210) return false;
        const uintptr_t rec = nibbles - 0x210;
        uint64_t mask = 0, nib[4] = {}, tablePtr = 0, node = 0;
        float centre[4] = {}, radius = 0, s = 0;
        uint16_t lodCount = 0;
        std::memcpy(&mask, reinterpret_cast<const void*>(rec + 0x208), 8);
        std::memcpy(nib, reinterpret_cast<const void*>(rec + 0x210), 32);
        std::memcpy(&tablePtr, reinterpret_cast<const void*>(rec + 0x20), 8);
        std::memcpy(&node, reinterpret_cast<const void*>(rec + 0x18), 8);
        std::memcpy(centre, reinterpret_cast<const void*>(rec + 0x240), 16);
        std::memcpy(&radius, reinterpret_cast<const void*>(rec + 0x280), 4);
        std::memcpy(&lodCount, reinterpret_cast<const void*>(node + 0x6A), 2);
        std::memcpy(&s, reinterpret_cast<const void*>(ctx + 0x30), 4);
        std::memcpy(&o->scaleBits, &s, 4);
        const lodgov::LodTable table = lodgov::LodTable::fromBytes(reinterpret_cast<const uint8_t*>(tablePtr));
        if (!table.inRange()) {
            o->tableRange = true;
            return true;
        }
        const uint32_t eyeBit[2] = {eyes & 0xFFu, (eyes >> 8) & 0xFFu};
        uint64_t eyeMask = 0;
        for (uint32_t e = 0; e < 2; ++e)
            if (eyeBit[e] < 64) eyeMask |= 1ull << eyeBit[e];
        // What the engine's dispatch saw: any view of the mask within the
        // node's LOD count. The builder runs, so this must hold.
        bool storedDispatch = false;
        for (uint64_t m = mask; m; m &= m - 1) {
            unsigned long bit = 0;
            _BitScanForward64(&bit, m);
            if ((static_cast<uint32_t>(nib[bit >> 4] >> ((bit & 15u) * 4u)) & 0xFu) <= lodCount) storedDispatch = true;
        }
        bool dispatch1 = false, dispatchK = false;
        for (uint64_t m = mask; m; m &= m - 1) {
            unsigned long bit = 0;
            _BitScanForward64(&bit, m);
            const bool isEye = ((eyeMask >> bit) & 1u) != 0;
            // Once the record is known to stay dispatched both ways, only the
            // eyes are still worth a recompute.
            if (dispatch1 && dispatchK && !isEye) {
                if (!(m & eyeMask)) break;
                continue;
            }
            const uint32_t stored = static_cast<uint32_t>(nib[bit >> 4] >> ((bit & 15u) * 4u)) & 0xFu;
            uint32_t slot = 0;
            std::memcpy(&slot, reinterpret_cast<const void*>(ctx + 0x1A840 + uintptr_t(bit) * 4), 4);
            if (slot >= 64) {   // not a view this context holds: nothing to recompute against
                if (isEye) {
                    RecordEye& re = o->eye[bit == eyeBit[0] ? 0 : 1];
                    re.seen = re.mismatch = true;
                }
                continue;
            }
            const uintptr_t view = ctx + 0x40 + uintptr_t(slot) * 0x6A0;
            float cam[4] = {}, A = 0, B = 0;
            std::memcpy(cam, reinterpret_cast<const void*>(view + 0x540), 16);
            std::memcpy(&A, reinterpret_cast<const void*>(view + 0x550), 4);
            std::memcpy(&B, reinterpret_cast<const void*>(view + 0x560), 4);
            const float d = lodgov::engineDistance(centre, cam);
            uint32_t n1 = 0, nk = 0;
            const bool p1 = lodgov::lodPick(table, lodgov::lodDistance(A, d, radius, s, B), &n1);
            const bool pk = lodgov::lodPick(table, lodgov::lodDistance(A, d, radius, s * k, B), &nk);
            if (p1 && n1 <= lodCount) dispatch1 = true;
            if (pk && nk <= lodCount) dispatchK = true;
            if (isEye) {
                RecordEye& re = o->eye[bit == eyeBit[0] ? 0 : 1];
                re.seen = true;
                if (!p1 || n1 != stored) re.mismatch = true;
                else if (!pk) re.wouldFail = true;
                else if (nk != n1) re.wouldChange = true;
            }
        }
        if (!storedDispatch || !dispatch1) o->dispatchMismatch = true;
        else if (!dispatchK) o->notDispatched = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The render context's view array at the frame boundary, for naming the eyes.
struct ViewInfo {
    float A, B;
    uint32_t bit;
};
bool readViews(uintptr_t ctx, ViewInfo* views, uint32_t* count) noexcept {
    __try {
        uint64_t n = 0;
        std::memcpy(&n, reinterpret_cast<const void*>(ctx + 0x1A940), 8);
        if (n > 64) return false;
        for (uint32_t i = 0; i < n; ++i) {
            const uintptr_t view = ctx + 0x40 + uintptr_t(i) * 0x6A0;
            uint64_t bits = 0;
            std::memcpy(&views[i].A, reinterpret_cast<const void*>(view + 0x550), 4);
            std::memcpy(&views[i].B, reinterpret_cast<const void*>(view + 0x560), 4);
            std::memcpy(&bits, reinterpret_cast<const void*>(view + 0x570), 8);
            unsigned long index = 0;
            views[i].bit = bits && !(bits & (bits - 1)) && _BitScanForward64(&index, bits) ? uint32_t(index) : 64u;
        }
        *count = static_cast<uint32_t>(n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- The runtime state (the caller thread's, and configure's, under g_mutex) -------
enum class Mode { Game, Auto, Reduced };

struct Window {
    uint64_t startMs = 0;
    uint32_t frames = 0, denseFrames = 0, eyeFrames = 0;
    uint32_t workSamples = 0, workOver = 0, workUnder = 0, workInvalid = 0;
    uint32_t callerSamples = 0, appSamples = 0;   // the valid samples by signal (WorkSource)
    uint32_t callerAbsent = 0;   // version 5 frames without valid caller work (inside workInvalid)
    uint32_t denseOver = 0;   // over-budget samples in frames with >= 200 records: what a rise needs
    double workSum = 0, periodSum = 0;
    uint64_t recordsSum = 0, partsSum = 0;
    uint32_t recordsMax = 0, partsMax = 0;
    float kLow = 1.0f, kHigh = 1.0f;
    uint32_t up = 0, down = 0, resets = 0, clamps = 0;
    uint64_t sum[kCounters] = {};
    uint32_t dropMax[kClasses] = {};
    uint32_t notDispatchedMax = 0;
};

struct State {
    Mode mode = Mode::Game;
    bool configured = false;
    std::string modeText = "game";
    float kMaxCfg = lodgov::kDefaultMax;
    const char* attach = "not attempted";
    const char* partStatus = "not requested";
    bool builderHooked = false;
    lodgov::Policy policy;
    uint32_t prev[kCounters] = {};
    uint64_t lastSeq = 0;
    double firstPeriodMs = 0;
    // The frame-work signal the runtime's timing version fixes (a property of
    // the runtime, kept across off/on), and that version; logged when it is
    // first known and if it ever changes.
    lodgov::WorkSource source = lodgov::WorkSource::None;
    uint32_t timingVersion = 0;
    Window w;
    uint64_t lastStepLogMs = 0;
    uint32_t stepsUnlogged = 0;
    uint32_t eyes = 0xFFFFu;
    float eyePixel = 0;
    uint32_t viewCount = 0;
};

std::mutex g_mutex;
State g_state;

const char* modeName(Mode m) noexcept {
    return m == Mode::Auto ? "auto" : m == Mode::Reduced ? "reduced" : "game";
}

// The eyes: the two perspective views (B = 0, A > 0) with the finest pixel
// (the smallest A, within 1%), ordered by view bit -- A the lower. At the
// parked Cranfield capture (165433) these are bits 1 and 22, A = 0.000834297;
// every other perspective view there is about nine times coarser and every
// orthographic one has A = 0. Keyed by the view's bit, never its array
// index: 165433's eye B was view 5 in frame 2 and view 6 in frames 3-4.
// Anything else leaves the eyes unnamed.
void identifyEyes(State& st) noexcept {
    const uintptr_t ctx = g_ctx.load(std::memory_order_acquire);
    ViewInfo views[64];
    uint32_t n = 0;
    uint32_t eyes = 0xFFFFu;
    float pixel = 0;
    if (ctx && readViews(ctx, views, &n)) {
        float minA = 0;
        for (uint32_t i = 0; i < n; ++i)
            if (views[i].B == 0.0f && views[i].A > 0.0f && std::isfinite(views[i].A) && views[i].bit < 64 &&
                (minA == 0 || views[i].A < minA))
                minA = views[i].A;
        uint32_t found[2] = {64, 64}, count = 0;
        for (uint32_t i = 0; i < n && minA > 0; ++i)
            if (views[i].B == 0.0f && views[i].A > 0.0f && views[i].bit < 64 && views[i].A <= minA * 1.01f) {
                if (count < 2) found[count] = views[i].bit;
                ++count;
            }
        if (count == 2 && found[0] != found[1]) {
            const uint32_t lo = found[0] < found[1] ? found[0] : found[1];
            const uint32_t hi = found[0] < found[1] ? found[1] : found[0];
            eyes = lo | (hi << 8);
            pixel = minA;
        }
    }
    st.eyes = eyes;
    st.eyePixel = pixel;
    st.viewCount = n;
    g_eyes.store(eyes, std::memory_order_release);
}

// The frame-work signal a runtime's timing frame version fixes: version 5 and
// later carry the caller work per cycle; 3 and 4 do not, and the producer's
// application time stands in.
lodgov::WorkSource sourceOf(uint32_t timingVersion) noexcept {
    return timingVersion >= EDVR_NATIVE_TIMING_VERSION_5 ? lodgov::WorkSource::Caller
         : timingVersion >= EDVR_NATIVE_TIMING_VERSION_3 ? lodgov::WorkSource::App
                                                         : lodgov::WorkSource::None;
}

// The signal's name, as the summary and the step lines print it.
const char* workSourceName(lodgov::WorkSource s) noexcept {
    return s == lodgov::WorkSource::Caller ? "caller work per cycle"
         : s == lodgov::WorkSource::App ? "app work (pre-submit only; host older)"
                                        : "no runtime frame yet";
}

// The signal in full, as the configure line and the one-off source line say
// it. Short: the configure line is already most of the log's 1200-byte line.
void workSourceClause(lodgov::WorkSource s, uint32_t timingVersion, char* out, size_t n) noexcept {
    if (s == lodgov::WorkSource::Caller)
        std::snprintf(out, n, "frame work = caller work per cycle (runtime timing v%u: the caller thread from one pose "
                      "wait's return to the next one's entry, submits included)", timingVersion);
    else if (s == lodgov::WorkSource::App)
        std::snprintf(out, n, "frame work = app work (pre-submit only; host older: runtime timing v%u sends no caller "
                      "work, so pose wait end to submit plus the eye treatments stands in)", timingVersion);
    else
        std::snprintf(out, n, "frame work = caller work per cycle if the runtime sends it (timing v5), else app work "
                      "(pre-submit only; host older); the first runtime frame decides and a line names it");
}

// The newest producer sample, once: the runtime's caller work per cycle
// (EdvrNativeTimingFrame version 5, callerWorkMs) against the display period.
// A version 3 or 4 runtime sends none, and NativeTimingSnapshot::applicationMs
// (the monitor's "app CPU", the pre-submit phase only) stands in. A version 5
// frame without valid caller work is an invalid sample: the two figures are
// never mixed in one run.
void readWork(State& st, uint64_t nowMs, lodgov::FrameSignals* sig) noexcept {
    const NativeTimingSnapshot t = nativeTimingSnapshot();
    if (!t.active) return;   // no native timing lease: no sample (Work::None)
    if (t.invalid || !t.haveCpu) {   // the newest frame failed or none is published: breaks the runs
        sig->work = lodgov::Work::Invalid;
        return;
    }
    if (!t.sequence || t.sequence == st.lastSeq) return;   // nothing new since the last boundary
    st.lastSeq = t.sequence;
    if (std::isfinite(t.predictedPeriodMs) && t.predictedPeriodMs > 0 && t.predictedPeriodMs <= 10000 &&
        st.firstPeriodMs == 0)
        st.firstPeriodMs = t.predictedPeriodMs;
    double period = st.firstPeriodMs;
    if (t.cpu.version >= EDVR_NATIVE_TIMING_VERSION_4 && std::isfinite(t.cpu.baseDisplayHz) &&
        t.cpu.baseDisplayHz > 0 && t.cpu.baseDisplayHz <= 1000)
        period = 1000.0 / double(t.cpu.baseDisplayHz);
    const bool fresh = t.capturedAtMs && t.capturedAtMs <= nowMs && nowMs - t.capturedAtMs <= 2000;
    sig->timingVersion = t.cpu.version;
    sig->source = sourceOf(t.cpu.version);
    double ms = 0;
    bool have = false;
    if (sig->source == lodgov::WorkSource::Caller) {
        ms = t.cpu.callerWorkMs;
        have = t.cpu.callerWorkValid == 1 && std::isfinite(ms) && ms >= 0 && ms <= 600000;
        sig->callerAbsent = !have;
    } else if (sig->source == lodgov::WorkSource::App) {
        ms = t.applicationMs;
        have = t.applicationValid && std::isfinite(ms) && ms >= 0 && ms <= 600000;
    }
    const bool ok = have && fresh && period > 0;
    sig->work = ok ? lodgov::Work::Valid : lodgov::Work::Invalid;
    sig->workMs = ok ? ms : 0;
    sig->periodMs = period;
}

double perFrame(uint64_t total, uint32_t frames) noexcept { return frames ? double(total) / frames : 0.0; }

void logSummary(State& st, uint64_t nowMs) {
    const Window& w = st.w;
    if (!w.frames) return;
    const float kNow = st.policy.k();
    char eyesText[96];
    if (st.eyes != 0xFFFFu)
        std::snprintf(eyesText, sizeof(eyesText), "eye views bits %u / %u (pixel %.6g per metre, %u views; named on %u of "
                      "%u frames)", st.eyes & 0xFFu, (st.eyes >> 8) & 0xFFu, st.eyePixel, st.viewCount, w.eyeFrames,
                      w.frames);
    else
        std::snprintf(eyesText, sizeof(eyesText), "eye views NOT named now (named on %u of %u frames)", w.eyeFrames,
                      w.frames);
    const char* stuck = "";
    if (w.kHigh <= 1.0f) {
        if (!w.recordsSum)
            stuck = "; k stayed 1: no draw-item builder calls (no settlement records, or the builder hook ran nothing)";
        else if (!w.denseFrames)
            stuck = "; k stayed 1: never 200 builder records in a frame";
        else if (!w.workSamples)
            stuck = "; k stayed 1: no valid frame-work sample from the native runtime";
        else if (!w.denseOver)
            stuck = "; k stayed 1: the frame work never ran 0.30 ms over the period in a frame with 200 records";
        else
            stuck = "; k stayed 1: over-budget runs with 200 records never reached 30 consecutive samples";
    }
    uint32_t sBits = g_scaleBits.load(std::memory_order_relaxed);
    // The LOD scale the tests would run with: the game's s (ctx+0x30, from the
    // newest builder call) times k -- what the LOD note's table is keyed by.
    const float sNow = fromBits(sBits);
    char effective[48];
    if (std::isfinite(sNow) && sNow > 0.0f)
        std::snprintf(effective, sizeof(effective), "%.3f", double(sNow) * double(kNow));
    else
        std::snprintf(effective, sizeof(effective), "unknown (no LOD scale read yet)");
    // Which figure the frame work was: one per runtime in practice, both
    // named if a window ever saw both.
    char source[160];
    if (w.callerSamples && w.appSamples)
        std::snprintf(source, sizeof(source), "%s on %u samples and %s on %u",
                      workSourceName(lodgov::WorkSource::Caller), w.callerSamples,
                      workSourceName(lodgov::WorkSource::App), w.appSamples);
    else
        std::snprintf(source, sizeof(source), "%s",
                      workSourceName(w.callerSamples ? lodgov::WorkSource::Caller
                                     : w.appSamples  ? lodgov::WorkSource::App
                                                     : st.source));
    Log::get().note(
        "settlement detail (shadow, never acts): %.1f s, %u frames: k now %.2f, effective s x k %s (window %.2f..%.2f "
        "of max %.2f; %u up, %u down, %u resets, %u clamps); builder records/frame %.1f (max %u, >= 200 on %u frames), "
        "part tests/frame %.1f (max %u); frame work = %s: %.2f ms mean vs period %.2f ms over %u samples (over by > "
        "0.30 ms: %u, under by > 1.00 ms: %u, invalid %u, caller work absent %u); LOD scale s (ctx+0x30) %.3f; %s%s.",
        double(nowMs - w.startMs) / 1000.0, w.frames, kNow, effective, w.kLow, w.kHigh, st.policy.kMax(), w.up, w.down,
        w.resets, w.clamps, perFrame(w.recordsSum, w.frames), w.recordsMax, w.denseFrames,
        perFrame(w.partsSum, w.frames), w.partsMax, source, w.workSamples ? w.workSum / w.workSamples : 0.0,
        w.workSamples ? w.periodSum / w.workSamples : 0.0, w.workSamples, w.workOver, w.workUnder, w.workInvalid,
        w.callerAbsent, double(sNow), eyesText, stuck);
    if (!w.recordsSum && !w.partsSum) return;   // nothing built: the header says so
    for (uint32_t e = 0; e < 2; ++e) {
        const uint64_t* p = &w.sum[cPartBase + e * kPartFields];
        const uint64_t* r = &w.sum[cRecBase + e * kRecFields];
        const uint32_t bit = e ? (st.eyes >> 8) & 0xFFu : st.eyes & 0xFFu;
        Log::get().note(
            "settlement detail (shadow) eye %c (view bit %u now): parts tested %.1f/frame, engine passed %.1f; at the "
            "shadow k would drop %.1f/frame (max %u), change LOD level %.1f/frame; would-drop angular radius r/d < "
            "0.25 deg %llu, 0.25-0.5 %llu, 0.5-1 %llu, >= 1 %llu (window totals); records passed %.1f/frame, would "
            "lose the eye %.1f, change level %.1f; disagreements with the engine at k = 1: parts %llu, records %llu.",
            e ? 'B' : 'A', bit, perFrame(p[pSeen], w.frames), perFrame(p[pPassed], w.frames),
            perFrame(p[pWouldDrop], w.frames), w.dropMax[e], perFrame(p[pWouldChange], w.frames),
            (unsigned long long)p[pHist0], (unsigned long long)p[pHist1], (unsigned long long)p[pHist2],
            (unsigned long long)p[pHist3], perFrame(r[rSeen], w.frames), perFrame(r[rWouldFail], w.frames),
            perFrame(r[rWouldChange], w.frames), (unsigned long long)p[pMismatch], (unsigned long long)r[rMismatch]);
    }
    const uint64_t* o = &w.sum[cPartBase + 2 * kPartFields];
    Log::get().note(
        "settlement detail (shadow) other views: parts tested %.1f/frame, engine passed %.1f, would drop %.1f/frame "
        "(max %u), change level %.1f/frame, disagreements %llu; records the builder would not be called for at all "
        "%.1f/frame (max %u), dispatch disagreements %llu; unreadable: parts %llu, records %llu; part tests from "
        "another caller %llu; tables past 7 levels: parts %llu, records %llu.",
        perFrame(o[pSeen], w.frames), perFrame(o[pPassed], w.frames), perFrame(o[pWouldDrop], w.frames), w.dropMax[2],
        perFrame(o[pWouldChange], w.frames), (unsigned long long)o[pMismatch],
        perFrame(w.sum[cRecNotDispatched], w.frames), w.notDispatchedMax,
        (unsigned long long)w.sum[cRecDispatchMismatch], (unsigned long long)w.sum[cPartFaults],
        (unsigned long long)w.sum[cRecordFaults], (unsigned long long)w.sum[cPartForeign],
        (unsigned long long)w.sum[cPartTableRange], (unsigned long long)w.sum[cRecordTableRange]);
}

void startWindow(State& st, uint64_t nowMs) {
    st.w = Window{};
    st.w.startMs = nowMs;
    st.w.kLow = st.w.kHigh = st.policy.k();
}

void logStep(State& st, lodgov::Step step, float from, const lodgov::FrameSignals& sig, uint64_t nowMs) {
    if (st.lastStepLogMs && nowMs - st.lastStepLogMs < 5000) {
        ++st.stepsUnlogged;
        return;
    }
    const char* why = step == lodgov::Step::Up ? "up: the frame work ran more than 0.30 ms over the period for 30 samples"
                    : step == lodgov::Step::Down ? "down: the frame work ran more than 1.00 ms under the period for 30 samples"
                    : step == lodgov::Step::Reset ? "reset: under 150 builder records for 30 frames"
                                                  : "clamped to the new advanced.settlement_detail_max";
    char more[48] = "";
    if (st.stepsUnlogged) std::snprintf(more, sizeof(more), " (+%u steps since the last line)", st.stepsUnlogged);
    char work[128] = "no frame-work sample this frame";
    if (sig.work == lodgov::Work::Valid)
        std::snprintf(work, sizeof(work), "frame work = %s: %.2f ms vs period %.2f ms", workSourceName(sig.source),
                      sig.workMs, sig.periodMs);
    Log::get().note("settlement detail (shadow, never acts): k %.2f -> %.2f, %s; %u builder records, %s%s.",
                    from, st.policy.k(), why, sig.records, work, more);
    st.lastStepLogMs = nowMs;
    st.stepsUnlogged = 0;
}

void frameBoundaryAt(uint64_t nowMs) {
    if (!g_live.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    State& st = g_state;
    uint32_t now[kCounters], d[kCounters];
    sumSlots(now);
    for (uint32_t c = 0; c < kCounters; ++c) {
        d[c] = now[c] - st.prev[c];
        st.prev[c] = now[c];
    }
    identifyEyes(st);
    lodgov::FrameSignals sig;
    sig.records = d[cRecords];
    sig.nowMs = nowMs;
    readWork(st, nowMs, &sig);
    // The signal the runtime's timing version fixes, said once when it is
    // first known (unless the configure line already named it) and again only
    // if it changes: a log that never shows this line never had a new frame.
    if (sig.source != lodgov::WorkSource::None &&
        (sig.source != st.source || sig.timingVersion != st.timingVersion)) {
        st.source = sig.source;
        st.timingVersion = sig.timingVersion;
        char clause[400];
        workSourceClause(st.source, st.timingVersion, clause, sizeof(clause));
        Log::get().note("settlement detail: %s.", clause);
    }
    const float from = st.policy.k();
    const lodgov::Step step = st.policy.update(sig);
    const float k = st.policy.k();
    g_kBits.store(toBits(k), std::memory_order_release);
    // The window.
    Window& w = st.w;
    ++w.frames;
    if (st.eyes != 0xFFFFu) ++w.eyeFrames;
    if (sig.records >= lodgov::kSettlementRecords) ++w.denseFrames;
    w.recordsSum += d[cRecords];
    w.partsSum += d[cParts];
    if (d[cRecords] > w.recordsMax) w.recordsMax = d[cRecords];
    if (d[cParts] > w.partsMax) w.partsMax = d[cParts];
    if (sig.work == lodgov::Work::Valid) {
        ++w.workSamples;
        if (sig.source == lodgov::WorkSource::Caller) ++w.callerSamples;
        else if (sig.source == lodgov::WorkSource::App) ++w.appSamples;
        w.workSum += sig.workMs;
        w.periodSum += sig.periodMs;
        if (sig.workMs > sig.periodMs + lodgov::kOverMarginMs) {
            ++w.workOver;
            if (sig.records >= lodgov::kSettlementRecords) ++w.denseOver;
        }
        if (sig.workMs < sig.periodMs - lodgov::kUnderMarginMs) ++w.workUnder;
    } else if (sig.work == lodgov::Work::Invalid) {
        ++w.workInvalid;
        if (sig.callerAbsent) ++w.callerAbsent;
    }
    for (uint32_t c = 0; c < kCounters; ++c) w.sum[c] += d[c];
    for (uint32_t cls = 0; cls < kClasses; ++cls)
        if (d[cPartBase + cls * kPartFields + pWouldDrop] > w.dropMax[cls])
            w.dropMax[cls] = d[cPartBase + cls * kPartFields + pWouldDrop];
    if (d[cRecNotDispatched] > w.notDispatchedMax) w.notDispatchedMax = d[cRecNotDispatched];
    if (k < w.kLow) w.kLow = k;
    if (k > w.kHigh) w.kHigh = k;
    switch (step) {
    case lodgov::Step::Up: ++w.up; break;
    case lodgov::Step::Down: ++w.down; break;
    case lodgov::Step::Reset: ++w.resets; break;
    case lodgov::Step::Clamp: ++w.clamps; break;
    default: break;
    }
    if (step != lodgov::Step::None) logStep(st, step, from, sig, nowMs);
    if (nowMs - w.startMs >= 30000) {
        logSummary(st, nowMs);
        startWindow(st, nowMs);
    }
}

void configureLine(const State& st) {
    const bool partOk = std::strcmp(st.partStatus, "hooked") == 0;
    char part[160];
    if (partOk) std::snprintf(part, sizeof(part), "hooked");
    else std::snprintf(part, sizeof(part), "STOOD DOWN (%s): no part counts, the record test still runs", st.partStatus);
    char work[400];
    workSourceClause(st.source, st.timingVersion, work, sizeof(work));
    Log::get().note(
        "settlement detail: shadow governor on (%s: shadow, never acts) -- k in [1, %.2f] in steps of 0.05: up one "
        "step while a frame has >= 200 draw-builder records and the frame work has run more than 0.30 ms over the "
        "display period for 30 samples, down one while it has run more than 1.00 ms under it for 30, at most one step "
        "a second, back to 1 after 30 frames under 150 records; %s. It recomputes the engine's LOD tests with the LOD "
        "scale (ctx+0x30) x k -- the per-part test FUN_1442B3FC0 and the record test FUN_144308B30 at the draw-item "
        "builder -- and counts what would change; no verdict, draw or mesh is touched. Hooks: draw-item builder "
        "FUN_1442B4420 %s, per-part test FUN_1442B3FC0 %s. Summaries every 30 s.",
        st.mode == Mode::Reduced ? "reduced is reserved and behaves as auto in this build" : "auto",
        st.policy.kMax(), work, st.builderHooked ? "hooked" : "NOT hooked (no records, no record test)", part);
}

bool enableLocked(State& st, uint64_t nowMs) {
    kinematicEvalSetLodGovernorObservers(&lodGovernorBuilderObserver, &lodGovernorPartObserver);
    st.attach = kinematicEvalLodGovernorAttach();
    if (std::strcmp(st.attach, "installed") != 0) {
        kinematicEvalSetLodGovernorObservers(nullptr, nullptr);
        return false;
    }
    st.builderHooked = kinematicEvalBuilderHooked();
    st.partStatus = kinematicEvalPartTestStatus();
    // Counts start now: the slots' sums so far are the baseline.
    sumSlots(st.prev);
    st.policy.reset();
    st.lastSeq = 0;
    st.lastStepLogMs = 0;
    st.stepsUnlogged = 0;
    g_kBits.store(toBits(1.0f), std::memory_order_release);
    identifyEyes(st);   // from the last context seen, if any; else unnamed until the first boundary
    startWindow(st, nowMs);
    g_live.store(true, std::memory_order_release);
    return true;
}

void disableLocked(State& st, uint64_t nowMs) {
    g_live.store(false, std::memory_order_release);
    kinematicEvalLodGovernorDetach();
    kinematicEvalSetLodGovernorObservers(nullptr, nullptr);
    g_kBits.store(toBits(1.0f), std::memory_order_release);
    if (st.w.frames) logSummary(st, nowMs);   // the partial window, before the off line
}

// The configure sweep's decision, testable without a Config.
void applyConfig(const char* modeText, float kMax, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State& st = g_state;
    const std::string text = modeText ? modeText : "";
    Mode mode = Mode::Game;
    bool unknown = false;
    if (_stricmp(text.c_str(), "auto") == 0) mode = Mode::Auto;
    else if (_stricmp(text.c_str(), "reduced") == 0) mode = Mode::Reduced;
    else if (!text.empty() && _stricmp(text.c_str(), "game") != 0) unknown = true;
    if (st.configured && mode == st.mode && text == st.modeText && kMax == st.kMaxCfg) return;   // the 1 Hz re-poll
    const bool first = !st.configured;
    st.configured = true;
    st.modeText = text;
    st.kMaxCfg = kMax;
    const bool live = g_live.load(std::memory_order_acquire);
    st.policy.configure(kMax);
    if (mode == Mode::Game) {
        st.mode = mode;
        if (live) disableLocked(st, nowMs);
        if (unknown)
            Log::get().note("settlement detail: fix.settlement_detail = \"%s\" is not game, auto or reduced; off -- "
                            "the game's own detail, nothing observed.", text.c_str());
        else if (live || first)
            Log::get().note("settlement detail: off (fix.settlement_detail = game: the game's own detail; nothing is "
                            "observed).");
        return;
    }
    st.mode = mode;
    if (!live && !enableLocked(st, nowMs)) {
        Log::get().note("settlement detail: the shadow governor could not attach its engine hooks (%s); off, nothing "
                        "observed (fix.settlement_detail = %s).", st.attach, modeName(mode));
        return;
    }
    // The configure line names the frame-work signal when a runtime frame has
    // already said which (the runtime is fixed for the process); else it says
    // the first frame decides, and frameBoundaryAt logs that.
    if (st.source == lodgov::WorkSource::None) {
        const NativeTimingSnapshot t = nativeTimingSnapshot();
        if (t.active && t.haveCpu && sourceOf(t.cpu.version) != lodgov::WorkSource::None) {
            st.source = sourceOf(t.cpu.version);
            st.timingVersion = t.cpu.version;
        }
    }
    configureLine(st);
}

}  // namespace

// --- The observers (worker threads) -------------------------------------------------

void lodGovernorBuilderObserver(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept {
    (void)pose;
    (void)mask;   // the collection's active mask: the builder's own view loop, not the record test
    if (!g_live.load(std::memory_order_relaxed)) return;
    Slot* s = mySlot();
    bump(s, cRecords);
    if (g_ctx.load(std::memory_order_relaxed) != ctx) g_ctx.store(ctx, std::memory_order_release);
    RecordOutcome o{};
    if (!shadowRecord(ctx, nibbles, g_eyes.load(std::memory_order_relaxed), currentK(), &o)) {
        bump(s, cRecordFaults);
        return;
    }
    if (g_scaleBits.load(std::memory_order_relaxed) != o.scaleBits)
        g_scaleBits.store(o.scaleBits, std::memory_order_relaxed);
    if (o.tableRange) {
        bump(s, cRecordTableRange);
        return;
    }
    for (uint32_t e = 0; e < 2; ++e) {
        const RecordEye& re = o.eye[e];
        if (!re.seen) continue;
        const uint32_t base = cRecBase + e * kRecFields;
        bump(s, base + rSeen);
        if (re.mismatch) bump(s, base + rMismatch);
        else if (re.wouldFail) bump(s, base + rWouldFail);
        else if (re.wouldChange) bump(s, base + rWouldChange);
    }
    if (o.dispatchMismatch) bump(s, cRecDispatchMismatch);
    else if (o.notDispatched) bump(s, cRecNotDispatched);
}

void lodGovernorPartObserver(uintptr_t items, uintptr_t out, uintptr_t view, bool fromBuilder) noexcept {
    if (!g_live.load(std::memory_order_relaxed)) return;
    Slot* s = mySlot();
    bump(s, cParts);
    if (!fromBuilder) {   // not the builder's sub-item loop: not a part the builder will draw
        bump(s, cPartForeign);
        return;
    }
    lodgov::PartInputs in;
    if (!readPart(items, out, view, &in)) {
        bump(s, cPartFaults);
        return;
    }
    if (!in.table.inRange()) {
        bump(s, cPartTableRange);
        return;
    }
    const uint32_t base = cPartBase + classOf(in.bit) * kPartFields;
    bump(s, base + pSeen);
    if (!in.enginePass) return;
    bump(s, base + pPassed);
    const lodgov::PartOutcome o = lodgov::shadowPart(in, currentK());
    if (o.mismatch) bump(s, base + pMismatch);
    else if (o.wouldDrop) {
        bump(s, base + pWouldDrop);
        bump(s, base + pHist0 + o.bucket);
    } else if (o.wouldChange) {
        bump(s, base + pWouldChange);
    }
}

// --- Configuration and the frame boundary ---------------------------------------------

void lodGovernorConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.settlement_detail", "game");
    const float kMax = cfg.getFloat("advanced.settlement_detail_max", lodgov::kDefaultMax);
    applyConfig(mode.c_str(), kMax, GetTickCount64());
}

void lodGovernorFrameBoundary() {
    if (!g_live.load(std::memory_order_acquire)) return;
    frameBoundaryAt(GetTickCount64());
}

}  // namespace edvr
