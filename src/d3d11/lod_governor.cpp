#include "lod_governor.h"

#include "journal_watch.h"
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
    sparse_ = 0;
    lastStepMs_ = 0;
    next_ = 0;
    emptyRing();
    stepOver_ = 0;
    stepMeanExcessMs_ = 0;
    upQuanta_ = 0;
    upHeld_ = false;
}

double Policy::meanExcessMs() const noexcept {
    if (!count_) return 0.0;
    double sum = 0.0;
    for (uint32_t i = 0; i < count_; ++i) sum += excess_[(next_ + kSampleWindow - 1 - i) % kSampleWindow];
    return sum / count_;
}

// One valid sample into the ring; once it is full the oldest leaves, and its
// over flag with it.
void Policy::push(double excessMs, bool over) noexcept {
    if (count_ == kSampleWindow) {
        if (over_[next_]) --overCount_;
    } else {
        ++count_;
    }
    excess_[next_] = excessMs;
    over_[next_] = over;
    if (over) ++overCount_;
    next_ = (next_ + 1) % kSampleWindow;
}

Step Policy::update(const FrameSignals& s) noexcept {
    // On foot (the game's Status.json, the flag the on-foot frame pacing keys
    // on): the arc measured the cockpit only, so k is held at 1 exactly as
    // outside a settlement -- at once, the settlement, the samples and a
    // pending clamp forgotten -- for as long as it lasts. Back aboard it
    // starts over: 200 records, then 30 samples, before a step.
    if (s.onFoot) {
        clampPending_ = inSettlement_ = false;
        sparse_ = 0;
        emptyRing();
        if (steps_ > 0) {
            steps_ = 0;
            lastStepMs_ = s.nowMs;
            stepped_ = true;
            return Step::Foot;
        }
        return Step::None;
    }
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
            sparse_ = 0;
            emptyRing();
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
    // Frame work: the latest 30 valid samples, each over (a missed slot) or
    // not. A frame with no new sample holds them; a bad sample empties them.
    if (s.work == Work::Invalid) {
        emptyRing();
    } else if (s.work == Work::Valid) {
        push(s.workMs - s.periodMs, s.workMs > s.periodMs + kOverMarginMs);
    }
    // reduced: k_max for as long as the settlement lasts -- at once, no ramp,
    // no frame-work steps.
    if (fixed_) {
        if (inSettlement_ && steps_ != maxSteps_) {
            steps_ = maxSteps_;
            lastStepMs_ = s.nowMs;
            stepped_ = true;
            return Step::Enter;
        }
        return Step::None;
    }
    if (stepped_ && s.nowMs - lastStepMs_ < kRampIntervalMs) return Step::None;
    if (count_ < kSampleWindow) return Step::None;   // 30 samples before any step
    const double mean = meanExcessMs();
    // Up while 3 or more of the 30 missed their slot: 0.25 while the 30 ran
    // more than 1.0 ms over on average, else 0.05; held to k_max either way.
    if (inSettlement_ && s.records >= kSettlementRecords && overCount_ >= kUpMisses && steps_ < maxSteps_) {
        stepOver_ = overCount_;
        stepMeanExcessMs_ = mean;
        upQuanta_ = mean > kCoarseExcessMs ? kCoarseQuanta : 1;
        upHeld_ = steps_ + upQuanta_ > maxSteps_;
        steps_ = upHeld_ ? maxSteps_ : steps_ + upQuanta_;
        lastStepMs_ = s.nowMs;
        stepped_ = true;
        return Step::Up;
    }
    // Down, 0.05, when none of the 30 did and they had a millisecond to spare.
    // Anything between holds: the dead band.
    if (overCount_ == 0 && mean < -kUnderMarginMs && steps_ > 0) {
        stepOver_ = 0;
        stepMeanExcessMs_ = mean;
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
// Per view class. pActing: tested at EDVR's LOD scale. pDrop: the price --
// observing, a pass that fails at s x k; acting, an engine reject the game's
// setting would have kept (its plane test run and passed). pUnverified: an
// acting reject whose LOD and screen-size terms say dropped but whose plane
// test could not run (no matched FUN_1404F4E10, or it faulted).
enum : uint32_t {
    pSeen, pPassed, pMismatch, pActing, pDrop, pChange, pUnverified, pHist0, pHist1, pHist2, pHist3, kPartFields
};
constexpr uint32_t kClasses = 3;   // eye A, eye B, every other view
enum : uint32_t { rSeen, rActing, rWouldFail, rChange, rMismatch, kRecFields };
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
std::atomic<uint32_t> g_scaleBits{0};            // its +0x30 as the builder last read it: the scale the tests ran at

inline float fromBits(uint32_t b) noexcept { float f; std::memcpy(&f, &b, 4); return f; }
inline uint32_t toBits(float f) noexcept { return lodgov::floatBits(f); }
inline float currentK() noexcept { return fromBits(g_kBits.load(std::memory_order_relaxed)); }

inline uint32_t classOf(uint32_t bit) noexcept {
    const uint32_t eyes = g_eyes.load(std::memory_order_relaxed);
    if (bit == (eyes & 0xFFu)) return 0;
    if (bit == ((eyes >> 8) & 0xFFu)) return 1;
    return 2;
}

// --- The write: the contexts, the acting switch, the setter's counts ---------------
// A context is written only if the draw-item builder has used it (the
// settlement's; the setter also rebuilds a second context each frame that the
// builder never reads) -- at most kContexts of them, a further one stands
// acting down. Slots fill in order and are cleared only while the governor
// is off. Written twice a frame at most: no cache-line padding.
struct CtxSlot {
    std::atomic<uintptr_t> ptr{0};          // the builder's render context (0: free)
    std::atomic<uint32_t> gameBits{0};      // what the setter stored on its last call: the game's s (0: none yet)
    std::atomic<uint32_t> heldBits{0};      // what EDVR wrote after that call (0: the game's value stands)
    std::atomic<uint32_t> writable{0};      // the page, VirtualQuery'd once: 0 unchecked, 1 writable, 2 refused
    std::atomic<uint32_t> firstState{0};    // the first-write line: 0 none, 1 claimed, 2 to log, 3 logged
    std::atomic<uint32_t> firstGame{0}, firstHeld{0}, firstK{0};
    std::atomic<uint32_t> scaledWindow{0};  // the summary window of its latest scaling (g_window)
};
CtxSlot g_contexts[lodgov::kContexts];
std::atomic<bool> g_contextOverflow{false};
// The contexts the setter was called with (the builder's or not), for the
// summary's count only.
struct SetterSeen {
    std::atomic<uintptr_t> ptr{0};
    std::atomic<uint32_t> calledWindow{0};  // the summary window of its latest setter call
};
SetterSeen g_setterSeen[lodgov::kContexts];
// The summary window's number (startWindow advances it): the setter tags what
// it did with it, so the summary counts contexts per window with no clock.
std::atomic<uint32_t> g_window{1};

std::atomic<bool> g_acting{false};               // configure: auto or reduced, observe 0, the setter hooked
std::atomic<const char*> g_standDown{nullptr};   // why acting stood down for the process (the first cause)
std::atomic<uintptr_t> g_standDownCtx{0};
std::atomic<uint32_t> g_setterCalls{0}, g_setterScaled{0}, g_setterImplausible{0}, g_setterFaults{0};
std::atomic<LodGovernorFrustumFn> g_frustum{nullptr};   // FUN_1404F4E10, matched, or null

inline bool actingNow() noexcept {
    return g_acting.load(std::memory_order_acquire) && !g_standDown.load(std::memory_order_acquire) &&
           !g_contextOverflow.load(std::memory_order_acquire);
}

CtxSlot* findContext(uintptr_t ctx) noexcept {
    if (!ctx) return nullptr;
    for (CtxSlot& s : g_contexts) {
        const uintptr_t p = s.ptr.load(std::memory_order_acquire);
        if (p == ctx) return &s;
        if (!p) return nullptr;
    }
    return nullptr;
}

void registerContext(uintptr_t ctx) noexcept {
    if (!ctx) return;
    for (CtxSlot& s : g_contexts) {
        const uintptr_t p = s.ptr.load(std::memory_order_acquire);
        if (p == ctx) return;
        if (!p) {
            uintptr_t expected = 0;
            if (s.ptr.compare_exchange_strong(expected, ctx, std::memory_order_acq_rel)) return;
            if (expected == ctx) return;
        }
    }
    g_contextOverflow.store(true, std::memory_order_release);
}

void noteSetterContext(uintptr_t ctx, uint32_t window) noexcept {
    for (SetterSeen& s : g_setterSeen) {
        uintptr_t p = s.ptr.load(std::memory_order_acquire);
        if (!p) {
            uintptr_t expected = 0;
            if (s.ptr.compare_exchange_strong(expected, ctx, std::memory_order_acq_rel)) p = ctx;
            else p = expected;
        }
        if (p == ctx) {
            s.calledWindow.store(window, std::memory_order_relaxed);
            return;
        }
    }
}

// Only while off (no observer runs): a fresh table for the next enable.
void resetContexts() noexcept {
    for (CtxSlot& s : g_contexts) {
        s.ptr.store(0, std::memory_order_relaxed);
        s.gameBits.store(0, std::memory_order_relaxed);
        s.heldBits.store(0, std::memory_order_relaxed);
        s.writable.store(0, std::memory_order_relaxed);
        s.firstState.store(0, std::memory_order_relaxed);
        s.scaledWindow.store(0, std::memory_order_relaxed);
    }
    for (SetterSeen& s : g_setterSeen) {
        s.ptr.store(0, std::memory_order_relaxed);
        s.calledWindow.store(0, std::memory_order_relaxed);
    }
    g_contextOverflow.store(false, std::memory_order_release);
    // The builder's last context is one it has used, and its observer
    // registers a context only when it differs from that one: it goes back
    // in at once. (A stale pointer here is harmless: only a setter call on
    // that very context, which proves it live, can write it.)
    registerContext(g_ctx.load(std::memory_order_acquire));
}

void standDown(const char* why, uintptr_t ctx) noexcept {
    const char* expected = nullptr;
    if (g_standDown.compare_exchange_strong(expected, why, std::memory_order_acq_rel))
        g_standDownCtx.store(ctx, std::memory_order_release);
}

// The game's own LOD scale for a context whose +0x30 the tests read as s: s
// itself unless the value in force is exactly the one EDVR wrote after the
// setter's last call.
float gameScaleFor(uintptr_t ctx, float s) noexcept {
    const CtxSlot* slot = findContext(ctx);
    if (!slot) return s;
    const uint32_t held = slot->heldBits.load(std::memory_order_acquire);
    if (!held || held != toBits(s)) return s;
    const uint32_t game = slot->gameBits.load(std::memory_order_acquire);
    return game ? fromBits(game) : s;
}

// --- The engine reads and the one write (SEH: a wild pointer drops the observation, never the flight) --
// POD locals only in the __try functions (/EHs units cannot unwind C++
// objects through __try; cull_gate_probe.cpp's rule).

bool readScale(uintptr_t ctx, float* out) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(ctx + 0x30), 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeScale(uintptr_t ctx, float value) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(ctx + 0x30), &value, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Committed, read-write (or execute-read-write), no guard page, for both ends
// of the four bytes.
bool pageWritable(uintptr_t at) noexcept {
    for (uintptr_t a : {at, at + 3}) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD p = mbi.Protect & 0xFFu;
        if (p != PAGE_READWRITE && p != PAGE_EXECUTE_READWRITE) return false;
    }
    return true;
}

bool contextWritable(CtxSlot& slot, uintptr_t ctx) noexcept {
    uint32_t w = slot.writable.load(std::memory_order_acquire);
    if (!w) {
        w = pageWritable(ctx + 0x30) ? 1u : 2u;
        slot.writable.store(w, std::memory_order_release);
    }
    return w == 1;
}

// The game's value back into a context still holding EDVR's: only if the page
// is still writable and the value there is exactly what EDVR wrote (else the
// engine has stored its own since, and that stands).
bool restoreOne(uintptr_t ctx, uint32_t held, uint32_t game) noexcept {
    if (!pageWritable(ctx + 0x30)) return false;
    __try {
        uint32_t now = 0;
        std::memcpy(&now, reinterpret_cast<const void*>(ctx + 0x30), 4);
        if (now != held) return false;
        std::memcpy(reinterpret_cast<void*>(ctx + 0x30), &game, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t restoreScaled() noexcept {
    uint32_t n = 0;
    for (CtxSlot& s : g_contexts) {
        const uintptr_t ctx = s.ptr.load(std::memory_order_acquire);
        if (!ctx) break;
        const uint32_t held = s.heldBits.exchange(0, std::memory_order_acq_rel);
        const uint32_t game = s.gameBits.load(std::memory_order_acquire);
        if (held && game && s.writable.load(std::memory_order_acquire) == 1 && restoreOne(ctx, held, game)) ++n;
    }
    return n;
}

// FUN_1442B3FC0 after its forward: param_1 = items (the builder's six-pointer
// block: [0] centre, [1] the model's +0x10 copy, [4] the LOD table copy, [5]
// the render context), param_2 = out {u32 LOD, u8 passed}, param_3 = view.
bool readPart(uintptr_t items, uintptr_t out, uintptr_t view, lodgov::PartInputs* in, uintptr_t* ctx) noexcept {
    __try {
        uint64_t block[6];
        std::memcpy(block, reinterpret_cast<const void*>(items), sizeof(block));
        uint8_t pass = 0;
        std::memcpy(&in->engineLod, reinterpret_cast<const void*>(out), 4);
        std::memcpy(&pass, reinterpret_cast<const void*>(out + 4), 1);
        in->enginePass = pass != 0;
        std::memcpy(in->centre, reinterpret_cast<const void*>(block[0]), 16);
        std::memcpy(in->sphere, reinterpret_cast<const void*>(block[1]), 16);
        std::memcpy(in->cam, reinterpret_cast<const void*>(view + 0x540), 16);
        std::memcpy(&in->A, reinterpret_cast<const void*>(view + 0x550), 4);
        std::memcpy(&in->B, reinterpret_cast<const void*>(view + 0x560), 4);
        uint64_t bits = 0;
        std::memcpy(&bits, reinterpret_cast<const void*>(view + 0x570), 8);
        std::memcpy(&in->s, reinterpret_cast<const void*>(block[5] + 0x30), 4);
        in->table = lodgov::LodTable::fromBytes(reinterpret_cast<const uint8_t*>(block[4]));
        unsigned long index = 0;
        in->bit = _BitScanForward64(&index, bits) ? static_cast<uint32_t>(index) : 64u;
        *ctx = static_cast<uintptr_t>(block[5]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The engine's own plane test on the part's sphere, as FUN_1442B3FC0 calls it
// (+0x8F..+0xA6: rcx the view, rdx the centre, r8 the sphere copy); its
// verdict's low 32 bits (-1: outside a plane).
bool planeTest(LodGovernorFrustumFn fn, uintptr_t view, const float* point, const float* interval,
               uint32_t* verdict) noexcept {
    __try {
        *verdict = static_cast<uint32_t>(fn(view, point, interval));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct RecordEye {
    bool seen, mismatch, wouldFail, change;
};
struct RecordOutcome {
    RecordEye eye[2];
    bool acting, dispatchMismatch, notDispatched, tableRange;
    uint32_t scaleBits;
};

// FUN_144308B30 (decomp_4308B30.txt) on the record the builder is about to
// build, from the inputs the traversal handed it (decomp_4312040.txt:154-165:
// centre rec+0x240, radius rec+0x280, table *(rec+0x20), the context's +0x30)
// and against the results the traversal stored and the builder's dispatch
// read (rec+0x208 mask, rec+0x210 nibbles by view bit, node +0x6A LOD count;
// decomp_4320340.txt:64-95). Per view bit: view = ctx + 0x40 + 0x6A0 *
// (u32 at ctx+0x1A840 + 4*bit), camera +0x540, A +0x550, B +0x560. Observing,
// what s x k would do; acting (s is EDVR's), the level against the game's s --
// a view the record lost at EDVR's s is not in rec+0x208 and cannot be told
// from one it was never tested for.
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
        const float sGame = gameScaleFor(ctx, s);
        const bool acting = toBits(s) != toBits(sGame);
        o->acting = acting;
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
        // Acting, "would the builder still be called at s x k" has no meaning
        // (s is already EDVR's): treated as settled.
        bool dispatch1 = false, dispatchK = acting;
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
            uint32_t n1 = 0, n2 = 0;
            const bool p1 = lodgov::lodPick(table, lodgov::lodDistance(A, d, radius, s, B), &n1);
            const bool p2 = lodgov::lodPick(table, lodgov::lodDistance(A, d, radius, acting ? sGame : s * k, B), &n2);
            if (p1 && n1 <= lodCount) dispatch1 = true;
            if (!acting && p2 && n2 <= lodCount) dispatchK = true;
            if (isEye) {
                RecordEye& re = o->eye[bit == eyeBit[0] ? 0 : 1];
                re.seen = true;
                if (!p1 || n1 != stored) re.mismatch = true;
                else if (acting) re.change = !p2 || n2 != n1;
                else if (!p2) re.wouldFail = true;
                else if (n2 != n1) re.change = true;
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
    uint32_t up = 0, down = 0, resets = 0, clamps = 0, enters = 0;
    uint32_t upCoarse = 0;     // up steps of 0.25 (inside up)
    uint32_t footFrames = 0;   // frames held at k 1 on foot
    uint64_t sum[kCounters] = {};
    uint32_t dropMax[kClasses] = {};
    uint32_t notDispatchedMax = 0;
    // The setter's counters at the window's start (they only grow).
    uint32_t setterCalls = 0, setterScaled = 0, setterImplausible = 0, setterFaults = 0;
};

struct State {
    Mode mode = Mode::Game;
    bool configured = false;
    bool observe = false;   // advanced.settlement_detail_observe
    std::string modeText = "game";
    float kMaxCfg = lodgov::kDefaultMax;
    const char* attach = "not attempted";
    const char* partStatus = "not requested";
    const char* setterStatus = "not requested";
    bool builderHooked = false;
    bool planesMatched = false;
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
    bool standDownLogged = false;   // process lifetime, like the stand-down
    bool overflowLogged = false;
    // The cockpit gate: on foot as the last boundary saw it, since when, for
    // how many frames; and whether the missing journal watcher was said.
    bool onFoot = false;
    uint64_t footStartMs = 0;
    uint32_t footFrames = 0;
    bool journalNoted = false;
};

std::mutex g_mutex;
State g_state;

const char* modeName(Mode m) noexcept {
    return m == Mode::Auto ? "auto" : m == Mode::Reduced ? "reduced" : "game";
}

// Every line's tag: what the governor is doing to the game right now.
const char* modeTag(const State& st) noexcept {
    if (st.observe) return "observe only, never writes";
    if (g_standDown.load(std::memory_order_acquire) || g_contextOverflow.load(std::memory_order_acquire))
        return "acting stood down, observing";
    if (!g_acting.load(std::memory_order_acquire)) return "cannot act, observing";
    return "acting";
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

// The game's own LOD scale now, for the lines: the value the setter stored for
// the builder's context, else -- nothing having been written without a setter
// call -- the value the builder read. 0 when neither is known.
float gameScaleNow(bool* fromSetter) noexcept {
    *fromSetter = false;
    const CtxSlot* slot = findContext(g_ctx.load(std::memory_order_acquire));
    const uint32_t game = slot ? slot->gameBits.load(std::memory_order_acquire) : 0;
    if (game) {
        *fromSetter = true;
        return fromBits(game);
    }
    const float read = fromBits(g_scaleBits.load(std::memory_order_relaxed));
    return std::isfinite(read) && read > 0.0f ? read : 0.0f;
}

void logSummary(State& st, uint64_t nowMs) {
    const Window& w = st.w;
    if (!w.frames) return;
    const float kNow = st.policy.k();
    const char* tag = modeTag(st);
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
        if (w.footFrames >= w.frames)
            stuck = "; k stayed 1: on foot the whole window (the governor is for the cockpit only)";
        else if (!w.recordsSum)
            stuck = "; k stayed 1: no draw-item builder calls (no settlement records, or the builder hook ran nothing)";
        else if (!w.denseFrames)
            stuck = "; k stayed 1: never 200 builder records in a frame";
        else if (st.policy.fixed())
            stuck = "";
        else if (!w.workSamples)
            stuck = "; k stayed 1: no valid frame-work sample from the native runtime";
        else if (!w.denseOver)
            stuck = "; k stayed 1: the frame work never ran 0.30 ms over the period in a frame with 200 records";
        else
            stuck = "; k stayed 1: never 3 of the last 30 samples over budget in a frame with 200 records (1-2 is the "
                    "dead band)";
    }
    // The LOD scale: the game's own (from the setter), what the tests ran at
    // (the builder's read), and the setter's counts. A write path that never
    // ran reads as setter calls 0 or scaled 0 while k rose above 1.
    bool fromSetter = false;
    const float sGame = gameScaleNow(&fromSetter);
    const float held = fromBits(g_scaleBits.load(std::memory_order_relaxed));
    char effective[48];
    if (sGame > 0.0f)
        std::snprintf(effective, sizeof(effective), "%.3f", double(sGame) * double(kNow));
    else
        std::snprintf(effective, sizeof(effective), "unknown (no LOD scale read yet)");
    const uint32_t calls = g_setterCalls.load(std::memory_order_relaxed) - w.setterCalls;
    const uint32_t scaled = g_setterScaled.load(std::memory_order_relaxed) - w.setterScaled;
    const uint32_t implausible = g_setterImplausible.load(std::memory_order_relaxed) - w.setterImplausible;
    const uint32_t faults = g_setterFaults.load(std::memory_order_relaxed) - w.setterFaults;
    uint32_t scaledPointers = 0, builderContexts = 0, setterPointers = 0;
    const uint32_t window = g_window.load(std::memory_order_relaxed);
    for (const CtxSlot& s : g_contexts) {
        if (!s.ptr.load(std::memory_order_acquire)) break;
        ++builderContexts;
        if (s.scaledWindow.load(std::memory_order_relaxed) == window) ++scaledPointers;
    }
    for (const SetterSeen& s : g_setterSeen)
        if (s.ptr.load(std::memory_order_acquire) && s.calledWindow.load(std::memory_order_relaxed) == window)
            ++setterPointers;
    char gameText[80], heldText[32];
    if (sGame > 0.0f)
        std::snprintf(gameText, sizeof(gameText), fromSetter ? "%.3f" : "%.3f (the builder's read: no setter call yet)",
                      double(sGame));
    else
        std::snprintf(gameText, sizeof(gameText), "unknown");
    if (std::isfinite(held) && held > 0.0f) std::snprintf(heldText, sizeof(heldText), "%.3f", double(held));
    else std::snprintf(heldText, sizeof(heldText), "unknown");
    const char* notActing = "";
    if (std::strcmp(tag, "acting") == 0 && w.kHigh > 1.0f) {
        if (!calls)
            notActing = "; NOT ACTING: k rose above 1 but FUN_142819D90's hook ran 0 times, so nothing was written";
        else if (!scaled)
            notActing = "; NOT ACTING: k rose above 1 but no setter call was on a context the draw-item builder used";
    }
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
    char enters[32] = "";
    if (w.enters) std::snprintf(enters, sizeof(enters), ", %u to k_max", w.enters);
    Log::get().note(
        "settlement detail (%s): %.1f s, %u frames: k now %.2f, effective s x k %s (window %.2f..%.2f of max %.2f; %u "
        "up (%u by 0.25), %u down, %u resets, %u clamps%s; held on foot %u frames); builder records/frame %.1f (max "
        "%u, >= 200 on %u frames), part tests/frame %.1f (max %u); frame work = %s: %.2f ms mean vs period %.2f ms "
        "over %u samples (over by > 0.30 ms: %u, under by > 1.00 ms: %u, invalid %u, caller work absent %u); LOD "
        "scale: game s %s, held %s (k %.2f); setter calls %u (scaled %u) on %u pointers (called with %u; builder "
        "contexts %u); implausible %u; faults %u; %s%s%s.",
        tag, double(nowMs - w.startMs) / 1000.0, w.frames, kNow, effective, w.kLow, w.kHigh, st.policy.kMax(), w.up,
        w.upCoarse, w.down, w.resets, w.clamps, enters, w.footFrames, perFrame(w.recordsSum, w.frames), w.recordsMax,
        w.denseFrames,
        perFrame(w.partsSum, w.frames), w.partsMax, source, w.workSamples ? w.workSum / w.workSamples : 0.0,
        w.workSamples ? w.periodSum / w.workSamples : 0.0, w.workSamples, w.workOver, w.workUnder, w.workInvalid,
        w.callerAbsent, gameText, heldText, kNow, calls, scaled, scaledPointers, setterPointers, builderContexts,
        implausible, faults, eyesText, stuck, notActing);
    if (!w.recordsSum && !w.partsSum) return;   // nothing built: the header says so
    for (uint32_t e = 0; e < 2; ++e) {
        const uint64_t* p = &w.sum[cPartBase + e * kPartFields];
        const uint64_t* r = &w.sum[cRecBase + e * kRecFields];
        const uint32_t bit = e ? (st.eyes >> 8) & 0xFFu : st.eyes & 0xFFu;
        if (p[pActing] || r[rActing]) {
            Log::get().note(
                "settlement detail (%s) eye %c (view bit %u now): parts tested %.1f/frame (%.1f at EDVR's LOD scale), "
                "engine passed %.1f; dropped (the game's setting would have kept it) %.1f/frame (max %u), LOD level "
                "changed from the game's %.1f/frame, plane test not run %llu; dropped angular radius r/d < 0.25 deg "
                "%llu, 0.25-0.5 %llu, 0.5-1 %llu, >= 1 %llu (window totals); records passed %.1f/frame (%.1f at "
                "EDVR's LOD scale), level changed from the game's %.1f, would lose the eye %.1f; a record that lost "
                "the eye at EDVR's scale is not seen; disagreements with the engine at the LOD scale it held: parts "
                "%llu, records %llu.",
                tag, e ? 'B' : 'A', bit, perFrame(p[pSeen], w.frames), perFrame(p[pActing], w.frames),
                perFrame(p[pPassed], w.frames), perFrame(p[pDrop], w.frames), w.dropMax[e],
                perFrame(p[pChange], w.frames), (unsigned long long)p[pUnverified], (unsigned long long)p[pHist0],
                (unsigned long long)p[pHist1], (unsigned long long)p[pHist2], (unsigned long long)p[pHist3],
                perFrame(r[rSeen], w.frames), perFrame(r[rActing], w.frames), perFrame(r[rChange], w.frames),
                perFrame(r[rWouldFail], w.frames), (unsigned long long)p[pMismatch], (unsigned long long)r[rMismatch]);
        } else {
            Log::get().note(
                "settlement detail (%s) eye %c (view bit %u now): parts tested %.1f/frame, engine passed %.1f; at the "
                "shadow k would drop %.1f/frame (max %u), change LOD level %.1f/frame; would-drop angular radius r/d < "
                "0.25 deg %llu, 0.25-0.5 %llu, 0.5-1 %llu, >= 1 %llu (window totals); records passed %.1f/frame, would "
                "lose the eye %.1f, change level %.1f; disagreements with the engine at the LOD scale it held: parts "
                "%llu, records %llu.",
                tag, e ? 'B' : 'A', bit, perFrame(p[pSeen], w.frames), perFrame(p[pPassed], w.frames),
                perFrame(p[pDrop], w.frames), w.dropMax[e], perFrame(p[pChange], w.frames),
                (unsigned long long)p[pHist0], (unsigned long long)p[pHist1], (unsigned long long)p[pHist2],
                (unsigned long long)p[pHist3], perFrame(r[rSeen], w.frames), perFrame(r[rWouldFail], w.frames),
                perFrame(r[rChange], w.frames), (unsigned long long)p[pMismatch], (unsigned long long)r[rMismatch]);
        }
    }
    const uint64_t* o = &w.sum[cPartBase + 2 * kPartFields];
    // Acting, a record left with no view never reaches the builder: what
    // observing counts as "would not be called for" has no acting twin.
    const bool otherActing = o[pActing] != 0;
    char records[128];
    if (otherActing)
        std::snprintf(records, sizeof(records), "records the builder was not called for: not seen at EDVR's scale");
    else
        std::snprintf(records, sizeof(records), "records the builder would not be called for at all %.1f/frame (max %u)",
                      perFrame(w.sum[cRecNotDispatched], w.frames), w.notDispatchedMax);
    Log::get().note(
        "settlement detail (%s) other views: parts tested %.1f/frame, engine passed %.1f, %s %.1f/frame (max %u), %s "
        "%.1f/frame, disagreements %llu; %s, dispatch disagreements %llu; unreadable: parts %llu, records %llu; part "
        "tests from another caller %llu; tables past 7 levels: parts %llu, records %llu.",
        tag, perFrame(o[pSeen], w.frames), perFrame(o[pPassed], w.frames),
        otherActing ? "dropped (the game's setting would have kept it)" : "would drop", perFrame(o[pDrop], w.frames),
        w.dropMax[2], otherActing ? "level changed from the game's" : "change level", perFrame(o[pChange], w.frames),
        (unsigned long long)o[pMismatch], records, (unsigned long long)w.sum[cRecDispatchMismatch],
        (unsigned long long)w.sum[cPartFaults], (unsigned long long)w.sum[cRecordFaults],
        (unsigned long long)w.sum[cPartForeign], (unsigned long long)w.sum[cPartTableRange],
        (unsigned long long)w.sum[cRecordTableRange]);
}

void startWindow(State& st, uint64_t nowMs) {
    g_window.fetch_add(1, std::memory_order_relaxed);
    st.w = Window{};
    st.w.startMs = nowMs;
    st.w.kLow = st.w.kHigh = st.policy.k();
    st.w.setterCalls = g_setterCalls.load(std::memory_order_relaxed);
    st.w.setterScaled = g_setterScaled.load(std::memory_order_relaxed);
    st.w.setterImplausible = g_setterImplausible.load(std::memory_order_relaxed);
    st.w.setterFaults = g_setterFaults.load(std::memory_order_relaxed);
}

void logStep(State& st, lodgov::Step step, float from, const lodgov::FrameSignals& sig, uint64_t nowMs) {
    if (st.lastStepLogMs && nowMs - st.lastStepLogMs < 5000) {
        ++st.stepsUnlogged;
        return;
    }
    // An up or down step names how many of the last 30 samples missed their
    // slot and their mean excess; an up step its size too. Down is always 0.05.
    char why[240];
    const double mean = st.policy.stepMeanExcessMs();
    if (step == lodgov::Step::Up) {
        const bool coarse = st.policy.upQuanta() > 1;
        std::snprintf(why, sizeof(why), "up %.2f: %u of the last 30 samples ran more than 0.30 ms over the period, "
                      "their mean %.2f ms %s (%s%s)", double(st.policy.upQuanta()) / lodgov::kQuantaPerUnit,
                      st.policy.stepOver(), std::fabs(mean), mean < 0.0 ? "under" : "over",
                      coarse ? "more than 1.00 ms over: the coarse step" : "1.00 ms over or less: the fine step",
                      st.policy.upHeld() ? ", held to k_max" : "");
    } else if (step == lodgov::Step::Down) {
        std::snprintf(why, sizeof(why), "down 0.05: none of the last 30 samples ran more than 0.30 ms over the period, "
                      "their mean %.2f ms under (more than 1.00 ms to spare)", std::fabs(mean));
    } else {
        std::snprintf(why, sizeof(why), "%s",
                      step == lodgov::Step::Reset ? "reset: under 150 builder records for 30 frames"
                      : step == lodgov::Step::Enter ? "reduced: in a settlement (>= 200 builder records), k = k_max at once"
                                                    : "clamped to the new advanced.settlement_detail_max");
    }
    char more[48] = "";
    if (st.stepsUnlogged) std::snprintf(more, sizeof(more), " (+%u steps since the last line)", st.stepsUnlogged);
    char work[128] = "no frame-work sample this frame";
    if (sig.work == lodgov::Work::Valid)
        std::snprintf(work, sizeof(work), "frame work = %s: %.2f ms vs period %.2f ms", workSourceName(sig.source),
                      sig.workMs, sig.periodMs);
    // Acting: the scale the next setter call will write for the builder's context.
    char scale[64] = "";
    const char* tag = modeTag(st);
    if (std::strcmp(tag, "acting") == 0) {
        bool fromSetter = false;
        const float sGame = gameScaleNow(&fromSetter);
        if (sGame > 0.0f && fromSetter)
            std::snprintf(scale, sizeof(scale), " -> LOD scale s x k %.3f", double(sGame) * double(st.policy.k()));
        else
            std::snprintf(scale, sizeof(scale), " -> LOD scale s x k unknown (no setter call yet)");
    }
    Log::get().note("settlement detail (%s): k %.2f -> %.2f, %s; %u builder records, %s%s%s.", tag, from,
                    st.policy.k(), why, sig.records, work, scale, more);
    st.lastStepLogMs = nowMs;
    st.stepsUnlogged = 0;
}

// The write's one-off lines, from the caller thread: each context's first
// scaling, and a stand-down (after which the game's value is written back).
void logWriteEvents(State& st) {
    for (CtxSlot& s : g_contexts) {
        const uintptr_t ctx = s.ptr.load(std::memory_order_acquire);
        if (!ctx) break;
        if (s.firstState.load(std::memory_order_acquire) != 2) continue;
        Log::get().note("settlement detail: LOD scale scaled: game s %.3f -> %.3f (k %.2f), ctx 0x%llX.",
                        double(fromBits(s.firstGame.load(std::memory_order_relaxed))),
                        double(fromBits(s.firstHeld.load(std::memory_order_relaxed))),
                        double(fromBits(s.firstK.load(std::memory_order_relaxed))), (unsigned long long)ctx);
        s.firstState.store(3, std::memory_order_release);
    }
    const char* why = g_standDown.load(std::memory_order_acquire);
    if (why && !st.standDownLogged) {
        st.standDownLogged = true;
        const uint32_t restored = restoreScaled();
        Log::get().note("settlement detail: acting STOOD DOWN for this process: %s (ctx 0x%llX); observing only from "
                        "now, the game's LOD scale written back to %u context(s) (and stored by the engine itself "
                        "from the next frame).",
                        why, (unsigned long long)g_standDownCtx.load(std::memory_order_acquire), restored);
    }
    if (g_contextOverflow.load(std::memory_order_acquire) && !st.overflowLogged) {
        st.overflowLogged = true;
        const uint32_t restored = restoreScaled();
        Log::get().note("settlement detail: acting STOOD DOWN: the draw-item builder used more than %u render "
                        "contexts; observing only until fix.settlement_detail is switched off and on (the game's LOD "
                        "scale written back to %u context(s)).",
                        lodgov::kContexts, restored);
    }
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
    // The cockpit gate: the journal watcher's Status.json, read on this same
    // thread each frame before the boundary (device_hook.cpp), exactly as the
    // on-foot frame pacing reads it (native_frame.cpp). Unknown -- menus, the
    // watcher off -- is not on foot.
    sig.onFoot = journalOnFootKnown() && journalOnFoot();
    if (!st.journalNoted && !journalWatchActive()) {
        st.journalNoted = true;
        Log::get().note("settlement detail: the journal watcher is not reading the game's Status.json "
                        "(d3d11.journal_watch off, or the journal folder not found), so on foot cannot be told from "
                        "the cockpit: the governor also runs on foot.");
    }
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
    // The setter observer reads k at the engine's next rebuild: the boundary
    // never writes engine memory itself.
    g_kBits.store(toBits(k), std::memory_order_release);
    logWriteEvents(st);
    // One line per transition of the cockpit gate, never rate-limited.
    if (sig.onFoot != st.onFoot) {
        st.onFoot = sig.onFoot;
        if (sig.onFoot) {
            st.footStartMs = nowMs;
            st.footFrames = 0;
            Log::get().note("settlement detail (%s): on foot (the game's Status.json, the flag the on-foot frame "
                            "pacing reads): k %.2f -> %.2f, held at 1 while on foot -- the governor is for the "
                            "cockpit only; on foot is unmeasured.", modeTag(st), from, k);
        } else {
            Log::get().note("settlement detail (%s): no longer on foot (Status.json) after %.1f s, %u frames held at "
                            "k 1; the governor resumes (a settlement's 200 builder records, then 30 samples, before a "
                            "step).", modeTag(st), double(nowMs - st.footStartMs) / 1000.0, st.footFrames);
        }
    }
    if (sig.onFoot) ++st.footFrames;
    // The window.
    Window& w = st.w;
    ++w.frames;
    if (sig.onFoot) ++w.footFrames;
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
        if (d[cPartBase + cls * kPartFields + pDrop] > w.dropMax[cls])
            w.dropMax[cls] = d[cPartBase + cls * kPartFields + pDrop];
    if (d[cRecNotDispatched] > w.notDispatchedMax) w.notDispatchedMax = d[cRecNotDispatched];
    if (k < w.kLow) w.kLow = k;
    if (k > w.kHigh) w.kHigh = k;
    switch (step) {
    case lodgov::Step::Up:
        ++w.up;
        if (st.policy.upQuanta() > 1) ++w.upCoarse;
        break;
    case lodgov::Step::Down: ++w.down; break;
    case lodgov::Step::Reset: ++w.resets; break;
    case lodgov::Step::Clamp: ++w.clamps; break;
    case lodgov::Step::Enter: ++w.enters; break;
    default: break;   // Foot: the transition line above says it
    }
    if (step != lodgov::Step::None && step != lodgov::Step::Foot) logStep(st, step, from, sig, nowMs);
    if (nowMs - w.startMs >= 30000) {
        logSummary(st, nowMs);
        startWindow(st, nowMs);
    }
}

void configureLine(const State& st) {
    const char* m = modeName(st.mode);
    const bool setterOk = std::strcmp(st.setterStatus, "hooked") == 0;
    char mode[200];
    if (st.observe)
        std::snprintf(mode, sizeof(mode), "%s, observe only: never writes (advanced.settlement_detail_observe = 1)", m);
    else if (!setterOk)
        std::snprintf(mode, sizeof(mode), "%s, but it cannot act: the LOD-scale setter hook stood down; observing, "
                      "never writes", m);
    else
        std::snprintf(mode, sizeof(mode), "%s: acts by scaling the game's LOD scale right after the engine sets it "
                      "each frame (FUN_142819D90): the game's value x k", m);
    const char* stood = g_standDown.load(std::memory_order_acquire)
        ? "; acting STOOD DOWN earlier in this process, observing" : "";
    char policy[400];
    if (st.mode == Mode::Reduced)
        std::snprintf(policy, sizeof(policy), "k = %.2f (advanced.settlement_detail_max) at once from a frame with >= "
                      "200 draw-builder records, 1 after 30 frames under 150 or on foot, no ramp", st.policy.kMax());
    else
        std::snprintf(policy, sizeof(policy), "k in [1, %.2f]: up while a frame has >= 200 draw-builder records and "
                      ">= 3 of the last 30 samples ran > 0.30 ms over the period, 0.25 if their mean ran > 1.00 ms "
                      "over, else 0.05; down 0.05 when none did and the mean ran > 1.00 ms under; at most a step a "
                      "second; 1 after 30 frames under 150 records or on foot", st.policy.kMax());
    char work[400];
    workSourceClause(st.source, st.timingVersion, work, sizeof(work));
    // The hook statuses are the fixed strings kinematic_eval_hook.cpp names;
    // the precision only bounds the line if one ever grows.
    char part[160];
    if (std::strcmp(st.partStatus, "hooked") == 0) std::snprintf(part, sizeof(part), "hooked");
    else std::snprintf(part, sizeof(part), "STOOD DOWN (%.128s)", st.partStatus);
    char setter[160];
    if (setterOk) std::snprintf(setter, sizeof(setter), "hooked");
    else std::snprintf(setter, sizeof(setter), "STOOD DOWN (%.128s)", st.setterStatus);
    Log::get().note(
        "settlement detail: on (%s%s) -- %s; %s. The shadow re-runs the engine's LOD tests (FUN_1442B3FC0 per part, "
        "FUN_144308B30 per record) at the game's scale and at scale x k. Hooks: builder FUN_1442B4420 %s, part test "
        "FUN_1442B3FC0 %s, LOD-scale setter FUN_142819D90 %s, plane test FUN_1404F4E10 %s. Summaries every 30 s.",
        mode, stood, policy, work, st.builderHooked ? "hooked" : "NOT hooked (no records, no record test)", part,
        setter, st.planesMatched ? "matched" : "MISMATCHED (acting drops unverified)");
}

bool enableLocked(State& st, uint64_t nowMs) {
    resetContexts();
    kinematicEvalSetLodGovernorObservers(&lodGovernorBuilderObserver, &lodGovernorPartObserver,
                                         &lodGovernorSetterObserver);
    st.attach = kinematicEvalLodGovernorAttach();
    if (std::strcmp(st.attach, "installed") != 0) {
        kinematicEvalSetLodGovernorObservers(nullptr, nullptr, nullptr);
        return false;
    }
    st.builderHooked = kinematicEvalBuilderHooked();
    st.partStatus = kinematicEvalPartTestStatus();
    st.setterStatus = kinematicEvalLodSetterStatus();
    const LodGovernorFrustumFn planes = kinematicEvalFrustumFn();
    g_frustum.store(planes, std::memory_order_release);
    st.planesMatched = planes != nullptr;
    st.overflowLogged = false;
    // Counts start now: the slots' sums so far are the baseline.
    sumSlots(st.prev);
    st.policy.reset();
    st.lastSeq = 0;
    st.lastStepLogMs = 0;
    st.stepsUnlogged = 0;
    st.onFoot = false;   // so the first boundary on foot says so
    st.footStartMs = 0;
    st.footFrames = 0;
    st.journalNoted = false;
    g_kBits.store(toBits(1.0f), std::memory_order_release);
    identifyEyes(st);   // from the last context seen, if any; else unnamed until the first boundary
    startWindow(st, nowMs);
    g_live.store(true, std::memory_order_release);
    return true;
}

// Returns how many contexts had the game's LOD scale written back.
uint32_t disableLocked(State& st, uint64_t nowMs) {
    if (st.w.frames) logSummary(st, nowMs);   // the partial window, tagged as it ran, before the off line
    g_acting.store(false, std::memory_order_release);
    const uint32_t restored = restoreScaled();
    g_live.store(false, std::memory_order_release);
    kinematicEvalLodGovernorDetach();
    kinematicEvalSetLodGovernorObservers(nullptr, nullptr, nullptr);
    g_kBits.store(toBits(1.0f), std::memory_order_release);
    return restored;
}

// Acting: auto or reduced, observe 0, the setter hooked. Leaving it writes
// the game's value back at once (the engine's next rebuild would anyway);
// returns how many contexts that touched.
uint32_t updateActing(const State& st) noexcept {
    const bool act = st.mode != Mode::Game && !st.observe && std::strcmp(st.setterStatus, "hooked") == 0;
    g_acting.store(act, std::memory_order_release);
    return act ? 0u : restoreScaled();
}

// The configure sweep's decision, testable without a Config.
void applyConfig(const char* modeText, float kMax, bool observe, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State& st = g_state;
    const std::string text = modeText ? modeText : "";
    Mode mode = Mode::Game;
    bool unknown = false;
    // An empty value is the compiled default (auto), as an empty number or
    // switch is theirs (Config::getFloat / getBool).
    if (text.empty() || _stricmp(text.c_str(), "auto") == 0) mode = Mode::Auto;
    else if (_stricmp(text.c_str(), "reduced") == 0) mode = Mode::Reduced;
    else if (_stricmp(text.c_str(), "game") != 0) unknown = true;
    if (st.configured && mode == st.mode && text == st.modeText && kMax == st.kMaxCfg && observe == st.observe)
        return;   // the 1 Hz re-poll
    const bool first = !st.configured;
    st.configured = true;
    st.modeText = text;
    st.kMaxCfg = kMax;
    st.observe = observe;
    const bool live = g_live.load(std::memory_order_acquire);
    st.policy.configure(kMax);
    st.policy.setFixed(mode == Mode::Reduced);
    if (mode == Mode::Game) {
        st.mode = mode;
        const uint32_t restored = live ? disableLocked(st, nowMs) : 0;
        if (unknown)
            Log::get().note("settlement detail: fix.settlement_detail = \"%s\" is not game, auto or reduced; off -- "
                            "the game's own detail, nothing observed or changed (the game's LOD scale written back "
                            "to %u context(s)).", text.c_str(), restored);
        else if (live || first)
            Log::get().note("settlement detail: off (fix.settlement_detail = game: the game's own detail; nothing is "
                            "observed or changed; the game's LOD scale written back to %u context(s)).", restored);
        return;
    }
    st.mode = mode;
    if (!live && !enableLocked(st, nowMs)) {
        g_acting.store(false, std::memory_order_release);
        Log::get().note("settlement detail: the governor could not attach its engine hooks (%s); off, nothing "
                        "observed or changed (fix.settlement_detail = %s).", st.attach, modeName(mode));
        return;
    }
    updateActing(st);
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

// --- The observers ----------------------------------------------------------------------

// The draw-item builder, before its forward (worker threads).
void lodGovernorBuilderObserver(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept {
    (void)pose;
    (void)mask;   // the collection's active mask: the builder's own view loop, not the record test
    if (!g_live.load(std::memory_order_relaxed)) return;
    Slot* s = mySlot();
    bump(s, cRecords);
    if (g_ctx.load(std::memory_order_relaxed) != ctx) {
        g_ctx.store(ctx, std::memory_order_release);
        registerContext(ctx);   // the one table the write consults
    }
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
        if (o.acting) bump(s, base + rActing);
        if (re.mismatch) bump(s, base + rMismatch);
        else if (re.wouldFail) bump(s, base + rWouldFail);
        else if (re.change) bump(s, base + rChange);
    }
    if (o.dispatchMismatch) bump(s, cRecDispatchMismatch);
    else if (o.notDispatched) bump(s, cRecNotDispatched);
}

// FUN_1442B3FC0, after its forward (worker threads).
void lodGovernorPartObserver(uintptr_t items, uintptr_t out, uintptr_t view, bool fromBuilder) noexcept {
    if (!g_live.load(std::memory_order_relaxed)) return;
    Slot* s = mySlot();
    bump(s, cParts);
    if (!fromBuilder) {   // not the builder's sub-item loop: not a part the builder will draw
        bump(s, cPartForeign);
        return;
    }
    lodgov::PartInputs in;
    uintptr_t ctx = 0;
    if (!readPart(items, out, view, &in, &ctx)) {
        bump(s, cPartFaults);
        return;
    }
    if (!in.table.inRange()) {
        bump(s, cPartTableRange);
        return;
    }
    in.sGame = gameScaleFor(ctx, in.s);
    const uint32_t base = cPartBase + classOf(in.bit) * kPartFields;
    bump(s, base + pSeen);
    if (in.enginePass) bump(s, base + pPassed);
    const lodgov::PartOutcome o = lodgov::shadowPart(in, currentK());
    if (o.acting) bump(s, base + pActing);
    if (o.mismatch) {
        bump(s, base + pMismatch);
    } else if (o.drop) {
        bump(s, base + pDrop);
        bump(s, base + pHist0 + o.bucket);
    } else if (o.change) {
        bump(s, base + pChange);
    } else if (o.checkPlanes) {
        // The one term the LOD arithmetic cannot answer for a reject: the
        // engine's own plane test on the part's sphere.
        const LodGovernorFrustumFn planes = g_frustum.load(std::memory_order_relaxed);
        uint32_t verdict = 0;
        if (!planes || !planeTest(planes, view, in.centre, in.sphere, &verdict)) {
            bump(s, base + pUnverified);
        } else if (verdict != 0xFFFFFFFFu) {
            bump(s, base + pDrop);
            bump(s, base + pHist0 + o.bucket);
        }
    }
}

// FUN_142819D90, after its forward (the engine's thread, twice a frame): the
// engine has just stored the game's LOD scale at ctx+0x30.
void lodGovernorSetterObserver(uintptr_t ctx) noexcept {
    if (!g_live.load(std::memory_order_acquire)) return;
    const uint32_t window = g_window.load(std::memory_order_relaxed);
    g_setterCalls.fetch_add(1, std::memory_order_relaxed);
    noteSetterContext(ctx, window);
    float v = 0;
    if (!readScale(ctx, &v)) {
        g_setterFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CtxSlot* slot = findContext(ctx);
    if (!slot) return;   // not a context the draw-item builder has used: never written
    // The engine has just stored its own value over anything of EDVR's.
    slot->heldBits.store(0, std::memory_order_release);
    if (!(v >= lodgov::kScaleLow && v <= lodgov::kScaleHigh)) {   // NaN too
        g_setterImplausible.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t vBits = toBits(v);
    slot->gameBits.store(vBits, std::memory_order_release);
    if (!actingNow()) return;
    const float k = currentK();
    if (!(k > 1.0f)) return;   // k = 1: the game's value stands
    const float target = v * k;
    if (!contextWritable(*slot, ctx)) {
        standDown("ctx+0x30 is not committed, writable memory (VirtualQuery)", ctx);
        return;
    }
    // Held first, then the store: a reader between the two sees the game's
    // value with a held value that does not match it, and counts it as such.
    const uint32_t targetBits = toBits(target);
    slot->heldBits.store(targetBits, std::memory_order_release);
    if (!writeScale(ctx, target)) {
        slot->heldBits.store(0, std::memory_order_release);
        g_setterFaults.fetch_add(1, std::memory_order_relaxed);
        standDown("writing ctx+0x30 faulted", ctx);
        return;
    }
    slot->scaledWindow.store(window, std::memory_order_relaxed);
    g_setterScaled.fetch_add(1, std::memory_order_relaxed);
    uint32_t expected = 0;
    if (slot->firstState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        slot->firstGame.store(vBits, std::memory_order_relaxed);
        slot->firstHeld.store(targetBits, std::memory_order_relaxed);
        slot->firstK.store(toBits(k), std::memory_order_relaxed);
        slot->firstState.store(2, std::memory_order_release);
    }
}

// --- Configuration, the frame boundary and shutdown ----------------------------------------

void lodGovernorConfigure(Config& cfg) {
    // A shipped fix: auto unless the player chose otherwise.
    const std::string mode = cfg.getString("fix.settlement_detail", "auto");
    const float kMax = cfg.getFloat("advanced.settlement_detail_max", lodgov::kDefaultMax);
    const bool observe = cfg.getBool("advanced.settlement_detail_observe", false);
    applyConfig(mode.c_str(), kMax, observe, GetTickCount64());
}

void lodGovernorFrameBoundary() {
    if (!g_live.load(std::memory_order_acquire)) return;
    frameBoundaryAt(GetTickCount64());
}

void lodGovernorShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_live.load(std::memory_order_acquire)) {
        disableLocked(g_state, GetTickCount64());
    } else {
        g_acting.store(false, std::memory_order_release);
        restoreScaled();
    }
}

}  // namespace edvr
