// F129 (native on-foot VR): look to aim reads its turn from the player's own look processor. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the hooks and the frame thread use. Journal: the arc doc, 2026-10-27, F129. RE: analysis\decomp\verify_20261027_onfoot_lift_look.txt (build 334925).
//
// The bug. Look to aim (F109) writes its ask into the game's mouse look actions in the update hook and reads the game's stored delta pair (comp+0x624 / +0x628) in the
// processor's post-hook to learn what the game applied. The FIRST processor call after each update consumed the ask. In the station hangar lift, the ship's lift and the
// SRV exit that call read 0 (or 0.01 deg) while the aim really turned one-for-one with the asks, so the ask was filed as blocked, nothing was cancelled, and the 1 deg
// probes walked the view until the guard latched look to aim off. The processor (0x1A9EB50) runs once per component and the factory builds one component per
// player-controlled humanoid (remote commanders, perhaps AI humanoids), so the first call after the update can be another humanoid's, whose pair holds nothing of ours.
//
// The fix, in three parts, all pure here:
//  - classify (Path): every processor call is path A (byte comp+0x678 != 0 and comp+0x7A9 == 0: the sink is not read, the pair is (heading, 0)), SKIP (nothing is stored,
//    seen by a sentinel written into the pair before the call that is still there after it) or path B (the sink is read: the only path that carries our ask);
//  - Gate: only the LOCAL component's call (its sink [comp+0x2E0] is the update's sink [act+0x88]) takes the pending ask; a LOCAL path A or SKIP call is a blocked call
//    (the pair says nothing about our ask), an ask no LOCAL call took before the next update is a blocked call, and a key that never matches falls back to the first call;
//  - the instrument (Rec, Ring formatting, Counters, Episode, RateCap, CompTable): what the log shows if the cause is something else.
// The probes while the look is blocked become zero-sum (native_on_foot_f109.h, ProbePair).
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f129 {

// ---- the classification ---------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kSentinelBits = 0x00000001u;   // written into both words of the pair before the call (a denormal float, 1.4e-45: nothing stores it)
enum Path : uint8_t { kPathB = 0, kPathA = 1, kSkip = 2, kUnknown = 3 };
inline const char* pathName(int p) noexcept { return p == kPathA ? "A" : p == kSkip ? "SKIP" : p == kUnknown ? "UNKNOWN" : "B"; }
// Path B and UNKNOWN take the split (today's behaviour); A and SKIP do not.
inline bool pathCarriesAsk(int p) noexcept { return p == kPathB || p == kUnknown; }

struct ClassIn {
    bool preOk = false;        // the bytes A and O and the pair were read before the call
    bool sentinelOk = false;   // the sentinel was written
    bool afterOk = false;      // the pair was read after the call
    uint32_t yawBits = 0, pitchBits = 0;   // the pair after the call
    uint8_t a = 0, o = 0;      // comp+0x678, comp+0x7A9 as read BEFORE the call (path B can also store pitch bits 0)
};
struct ClassOut {
    Path path = kUnknown;
    bool partial = false;                          // exactly one word is still the sentinel
    bool restoreYaw = false, restorePitch = false; // the words to put back to what they were before the call
};
inline ClassOut classify(const ClassIn& in) noexcept {
    ClassOut c;
    if (!in.preOk || !in.sentinelOk || !in.afterOk) return c;   // UNKNOWN: classified as path B by the caller
    const bool yawIntact = in.yawBits == kSentinelBits, pitchIntact = in.pitchBits == kSentinelBits;
    if (yawIntact && pitchIntact) {
        c.path = kSkip;
        c.restoreYaw = c.restorePitch = true;
    } else if (yawIntact || pitchIntact) {
        c.path = kSkip;   // a half-stored pair is not a call that carried anything: SKIP, counted
        c.partial = true;
        c.restoreYaw = yawIntact;
        c.restorePitch = pitchIntact;
    } else {
        c.path = (in.a != 0 && in.o == 0) ? kPathA : kPathB;
    }
    return c;
}

// What a pending ask meets: the split (path B), or a blocked call without one (path A, SKIP).
enum Act : uint8_t { kActNone = 0, kActSplit = 1, kActNoSplit = 2 };
inline Act decide(bool takesPend, int path) noexcept {
    if (!takesPend) return kActNone;
    return pathCarriesAsk(path) ? kActSplit : kActNoSplit;
}

// ---- the gate: which call takes the ask --------------------------------------------------------------------------------------------------------------
// Game thread only. The update tells the gate its pending record (updateEntry at the start of the update hook, updateEnd after the original); every processor call
// asks call(key) with the key it read, [comp+0x2E0].
constexpr uint32_t kNoLocalUpdatesMax = 120;   // updates with processor calls and none of them the local comp's: the key does not match, fall back
enum FallbackWhy : uint8_t { kWhyNone = 0, kWhyNoSink = 1, kWhyNoLocal = 2 };
struct UpdateEntryOut {
    bool unconsumed = false;       // the previous update's ask was taken by no local call before this update
    bool processorRan = false;     // some processor call ran since the previous update
    uint32_t calls = 0, otherCalls = 0;   // ... and how many of them were not the local comp's
    bool fallbackEntered = false;
    uint8_t why = kWhyNone;
};
struct CallOut {
    bool local = false;            // the player's own call (the key matched; in the fallback, the first call after the update)
    bool first = false;           // the first processor call since the update
    bool keyMatch = false;         // [comp+0x2E0] equals the update's sink (both non-zero)
    bool takes = false;            // takes the pending record
    bool leftFallback = false;
    bool firstNotLocalAsk = false; // the first call after an update that asked was not the local comp's: today's rule would have given our ask to another comp
};
struct Gate {
    bool pendValid = false, pendAsked = false;
    uint64_t sink = 0;             // the update's sink pointer [act+0x88]; 0 = unread
    bool fallback = false;
    uint8_t fallbackWhy = kWhyNone;
    uint32_t noLocalUpdates = 0;   // consecutive updates with processor calls and no local one
    uint32_t callsSince = 0, localSince = 0;
    // The first call after the update: ours or not, over the last updates (bit 0 the newest), and the streak of "not ours".
    uint32_t firstHist = 0, firstN = 0, firstNotStreak = 0;

    void dropPend() noexcept { pendValid = pendAsked = false; }
    UpdateEntryOut updateEntry() noexcept {
        UpdateEntryOut o;
        o.unconsumed = pendValid && pendAsked;
        o.calls = callsSince;
        o.otherCalls = callsSince - localSince;
        o.processorRan = callsSince > 0;
        if (callsSince > 0 && localSince == 0) ++noLocalUpdates;
        if (!fallback && noLocalUpdates >= kNoLocalUpdatesMax) {
            fallback = true;
            fallbackWhy = kWhyNoLocal;
            o.fallbackEntered = true;
            o.why = kWhyNoLocal;
        }
        pendValid = pendAsked = false;
        callsSince = localSince = 0;
        return o;
    }
    // After the original update: the pending record is valid (the update ran its look; false: a refused update, nothing pending), it carried an ask, and the sink was read.
    UpdateEntryOut updateEnd(bool valid, bool asked, bool sinkOk, uint64_t sinkPtr) noexcept {
        UpdateEntryOut o;
        pendValid = valid;
        pendAsked = valid && asked;
        sink = sinkOk && sinkPtr != 0 ? sinkPtr : 0;
        if (sink == 0 && !fallback) {
            fallback = true;
            fallbackWhy = kWhyNoSink;
            o.fallbackEntered = true;
            o.why = kWhyNoSink;
        }
        return o;
    }
    CallOut call(uint64_t key) noexcept {
        CallOut o;
        o.first = callsSince == 0;
        ++callsSince;
        o.keyMatch = key != 0 && sink != 0 && key == sink;
        if (fallback) {
            if (o.keyMatch) {
                fallback = false;
                fallbackWhy = kWhyNone;
                noLocalUpdates = 0;
                o.leftFallback = true;
                o.local = true;
            } else {
                o.local = o.first;   // before F129's rule: the first call after the update
            }
        } else {
            o.local = o.keyMatch;
        }
        if (o.local) ++localSince;
        if (o.keyMatch) noLocalUpdates = 0;
        o.takes = o.local && pendValid;
        o.firstNotLocalAsk = o.first && !o.local && pendValid && pendAsked;
        if (o.takes) pendValid = false;
        if (o.first && sink != 0) {   // with the sink unread there is nothing to be ahead of
            firstHist = (firstHist << 1) | (o.keyMatch ? 1u : 0u);
            if (firstN < 32) ++firstN;
            firstNotStreak = o.keyMatch ? 0 : firstNotStreak + 1;
        }
        return o;
    }
    // "the first call after the update is ours in X of the last Y updates"
    uint32_t firstOursOf(uint32_t* y) const noexcept;
};
// The count behind that phrase, from a history word (bit 0 the newest of n updates) as the frame thread reads it.
inline uint32_t oursOf(uint32_t hist, uint32_t n, uint32_t* y) noexcept {
    if (n > 32) n = 32;
    *y = n;
    uint32_t x = 0;
    for (uint32_t i = 0; i < n; ++i) x += (hist >> i) & 1u;
    return x;
}
inline uint32_t Gate::firstOursOf(uint32_t* y) const noexcept { return oursOf(firstHist, firstN, y); }

// ---- the order check and the sink's outcome ---------------------------------------------------------------------------------------------------------
// The sink's mouse fields as the local call read them before it ran, against what the update left in them: equal means the update ran before this call (the ask is
// what the processor reads); different means something changed them in between, or the call before the update's ran (the processor sees an older input).
enum Order : uint8_t { kOrderUnknown = 0, kOrderOk = 1, kOrderFlipped = 2 };
inline uint32_t floatBits(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}
inline float bitsFloat(uint32_t u) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline Order orderCheck(bool haveBoth, float preYaw, float prePitch, float afterYaw, float afterPitch) noexcept {
    if (!haveBoth) return kOrderUnknown;
    return floatBits(preYaw) == floatBits(afterYaw) && floatBits(prePitch) == floatBits(afterPitch) ? kOrderOk : kOrderFlipped;
}
// What the update's sink holds for one axis after the update ran, against the value written into the mouse action: reached it (the same value), was zeroed there (the
// game's look block: the update drops the mouse terms before the sink), or came out changed.
enum SinkState : uint8_t { kSinkNone = 0, kSinkReached = 1, kSinkZeroed = 2, kSinkAltered = 3 };
inline SinkState sinkAxis(double ask, bool sinkRead, float want, float got) noexcept {
    if (ask == 0.0 || !sinkRead) return kSinkNone;
    const double w = want, g = got;
    if (!std::isfinite(w) || !std::isfinite(g)) return kSinkAltered;
    if (std::fabs(g - w) <= 1e-4 * std::fmax(std::fabs(w), std::fabs(g)) + 1e-9) return kSinkReached;
    if (std::fabs(g) < 1e-12 && std::fabs(w) > 1e-9) return kSinkZeroed;
    return kSinkAltered;
}
inline SinkState sinkOutcome(SinkState yaw, SinkState pitch) noexcept {
    if (yaw == kSinkZeroed || pitch == kSinkZeroed) return kSinkZeroed;
    if (yaw == kSinkAltered || pitch == kSinkAltered) return kSinkAltered;
    if (yaw == kSinkReached || pitch == kSinkReached) return kSinkReached;
    return kSinkNone;
}
inline const char* sinkName(int s) noexcept { return s == kSinkReached ? "reached" : s == kSinkZeroed ? "ZEROED" : s == kSinkAltered ? "altered" : "-"; }

// ---- the ring ---------------------------------------------------------------------------------------------------------------------------------------
// The last calls, written by the game thread (both hooks) and read by the frame thread under a sequence lock. No allocation, logging or locks.
constexpr size_t kRingSize = 32;
enum Kind : uint8_t { kKindNone = 0, kKindUpdate = 1, kKindProc = 2 };
enum Flag : uint8_t {
    kFlLocal = 1, kFlFirst = 2, kFlAsked = 4, kFlTook = 8, kFlOrderOk = 16, kFlOrderFlipped = 32, kFlRestored = 64, kFlPartial = 128
};
struct Rec {
    uint32_t seq = 0;      // the update's index (an update) or the index of the update the call followed (a processor call)
    uint32_t id = 0;       // the activity (an update) or the component (a call), low 32 bits
    uint32_t sink = 0;     // [act+0x88] after the update, or [comp+0x2E0] of the call, low 32 bits
    float a = 0.0f, b = 0.0f;     // an update: the ask written (degrees, yaw and pitch); a call: the stored pair after it (degrees)
    float sa = 0.0f, sb = 0.0f;   // an update: the sink's mouse fields after it (yaw, pitch; game input units)
    uint8_t kind = kKindNone, path = kUnknown, flags = 0, sinkState = kSinkNone;
    uint8_t A = 0, O = 0, D = 0, M = 0;   // comp+0x678, +0x7A9, +0x7D4, +0x4D8 (255: unread)
};
// One record as text. Compact on purpose: 16 of them must fit one log line.
inline int formatRec(char* out, size_t cap, const Rec& r) noexcept {
    if (!out || cap == 0) return 0;
    int n = 0;
    if (r.kind == kKindUpdate) {
        n = std::snprintf(out, cap, "U%u %08X ask %+.3f/%+.3f sink %08X %+.3g/%+.3g %s", r.seq, r.id, static_cast<double>(r.a), static_cast<double>(r.b), r.sink,
                          static_cast<double>(r.sa), static_cast<double>(r.sb), sinkName(r.sinkState));
    } else {
        n = std::snprintf(out, cap, "P%u %08X %s %s A%u O%u D%u M%u %+.3f/%+.3f%s%s%s%s%s", r.seq, r.id, (r.flags & kFlLocal) ? "L" : "o", pathName(r.path), r.A, r.O, r.D,
                          r.M, static_cast<double>(r.a), static_cast<double>(r.b), (r.flags & kFlFirst) ? " 1st" : "", (r.flags & kFlTook) ? " TOOK" : "",
                          (r.flags & kFlOrderFlipped) ? " FLIPPED" : (r.flags & kFlOrderOk) ? " order-ok" : "", (r.flags & kFlPartial) ? " PARTIAL" : "",
                          (r.flags & kFlRestored) ? " restored" : "");
    }
    if (n < 0) return 0;
    return n >= static_cast<int>(cap) ? static_cast<int>(cap) - 1 : n;
}
// The newest `n` of `count` records (oldest first in recs), separated by " | ". A buffer too small for all of them keeps the newest and says how many older ones were
// left out, ahead of the rest; it never writes past `cap`.
inline int formatRing(char* out, size_t cap, const Rec* recs, size_t count, size_t n) noexcept {
    if (!out || cap < 8) return 0;
    out[0] = 0;
    if (!recs || count == 0 || n == 0) return 0;
    const size_t from = count > n ? count - n : 0;
    const size_t budget = cap > 48 ? cap - 48 : 0;   // room for the note ahead of the records and the terminator
    size_t used = 0;
    size_t start = count;   // the oldest record that fits
    for (size_t i = count; i > from; --i) {
        char one[160];
        const int len = formatRec(one, sizeof(one), recs[i - 1]);
        const size_t need = static_cast<size_t>(len) + (start < count ? 3u : 0u);
        if (used + need > budget) break;
        used += need;
        start = i - 1;
    }
    size_t at = 0;
    if (start > from) {
        const int h = std::snprintf(out, cap, "(+%zu older not shown) | ", start - from);
        if (h > 0 && static_cast<size_t>(h) < cap) at = static_cast<size_t>(h);
    }
    for (size_t i = start; i < count; ++i) {
        char one[160];
        const int len = formatRec(one, sizeof(one), recs[i]);
        const size_t sep = i > start ? 3u : 0u;
        if (at + sep + static_cast<size_t>(len) + 1 > cap) break;
        if (sep) {
            std::memcpy(out + at, " | ", 3);
            at += 3;
        }
        std::memcpy(out + at, one, static_cast<size_t>(len));
        at += static_cast<size_t>(len);
    }
    out[at] = 0;
    return static_cast<int>(at);
}
struct Ring {
    std::atomic<uint32_t> seq{0};
    uint32_t head = 0;
    Rec rec[kRingSize];
    void push(const Rec& r) noexcept {   // the game thread
        const uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        rec[head % kRingSize] = r;
        ++head;
        std::atomic_thread_fence(std::memory_order_release);
        seq.store(s + 2u, std::memory_order_release);
    }
    // The newest `want` records, oldest first, into out (room for want); the count read, 0 when the writer would not hold still for three tries.
    size_t read(Rec* out, size_t want) const noexcept {
        if (want > kRingSize) want = kRingSize;
        for (int tries = 0; tries < 3; ++tries) {
            const uint32_t a = seq.load(std::memory_order_acquire);
            if (a & 1u) continue;
            const uint32_t h = head;
            const size_t have = h < kRingSize ? h : kRingSize;
            const size_t n = have < want ? have : want;
            for (size_t i = 0; i < n; ++i) out[i] = rec[(h - n + i) % kRingSize];
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == a) return n;
        }
        return 0;
    }
    // The game thread's own read of its ring (no writer to wait for).
    size_t readOwn(Rec* out, size_t want) const noexcept {
        if (want > kRingSize) want = kRingSize;
        const size_t have = head < kRingSize ? head : kRingSize;
        const size_t n = have < want ? have : want;
        for (size_t i = 0; i < n; ++i) out[i] = rec[(head - n + i) % kRingSize];
        return n;
    }
};

// ---- the counters -----------------------------------------------------------------------------------------------------------------------------------
enum Ctr : int {
    kUpdates,           // update hook runs
    kAsks,              // updates that ran with an ask in the mouse look
    kCallsAll,          // processor calls, every component
    kCallsLocal,        // ... the player's own
    kCallsOther,        // ... any other component's
    kLocalB, kLocalA, kLocalSkip, kLocalUnknown,   // the local calls by path
    kOrderOkN, kOrderFlippedN,
    kConsumedB,         // an ask taken by a local path B (or unknown) call
    kAskAorSkip,        // an ask that met a local path A or SKIP call
    kUnconsumed,        // an ask no local call took before the next update
    kFirstNotLocalAsk,  // an ask that the first-call rule would have given to another component
    kReached, kZeroed, kAltered,   // the asks by what the update's sink held after it
    kFallbackEntered, kFallbackLeft,
    kPartial, kRestores, kRestoreFailed,
    kFirstOurs, kFirstNot,   // the first call after an update: the local comp's / another's
    kEpisodes,
    kCtrCount
};
struct Counters {
    std::atomic<uint32_t> n[kCtrCount];
    std::atomic<uint32_t> otherPerUpdateMax{0};   // the most other-component calls between two updates (the frame thread takes it with exchange(0))
    std::atomic<uint32_t> lastBytes{0};           // the last local call's A, O, D, M (one byte each, A lowest)
    std::atomic<uint32_t> lastPath{kUnknown};
    Counters() noexcept {
        for (auto& c : n) c.store(0, std::memory_order_relaxed);
    }
    void add(Ctr c, uint32_t v = 1) noexcept { n[c].fetch_add(v, std::memory_order_relaxed); }
    uint32_t get(Ctr c) const noexcept { return n[c].load(std::memory_order_relaxed); }
    void maxOther(uint32_t v) noexcept {
        uint32_t cur = otherPerUpdateMax.load(std::memory_order_relaxed);
        while (v > cur && !otherPerUpdateMax.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
        }
    }
};
inline uint32_t packBytes(uint8_t a, uint8_t o, uint8_t d, uint8_t m) noexcept {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(o) << 8) | (static_cast<uint32_t>(d) << 16) | (static_cast<uint32_t>(m) << 24);
}

// ---- the components seen ---------------------------------------------------------------------------------------------------------------------------
// Up to 16 pointers with the time each was last called. The game thread touches; the frame thread counts the ones seen within a window. A full table replaces the
// longest unseen entry.
constexpr size_t kCompTableSize = 16;
struct CompTable {
    std::atomic<uint64_t> ptr[kCompTableSize];
    std::atomic<uint64_t> seenMs[kCompTableSize];
    CompTable() noexcept {
        for (size_t i = 0; i < kCompTableSize; ++i) {
            ptr[i].store(0, std::memory_order_relaxed);
            seenMs[i].store(0, std::memory_order_relaxed);
        }
    }
    void touch(uint64_t p, uint64_t nowMs) noexcept {
        if (p == 0) return;
        size_t free = kCompTableSize, oldest = 0;
        uint64_t oldestMs = ~0ull;
        for (size_t i = 0; i < kCompTableSize; ++i) {
            const uint64_t q = ptr[i].load(std::memory_order_relaxed);
            if (q == p) {
                seenMs[i].store(nowMs, std::memory_order_relaxed);
                return;
            }
            if (q == 0) {
                if (free == kCompTableSize) free = i;
            } else {
                const uint64_t t = seenMs[i].load(std::memory_order_relaxed);
                if (t < oldestMs) {
                    oldestMs = t;
                    oldest = i;
                }
            }
        }
        const size_t slot = free != kCompTableSize ? free : oldest;
        seenMs[slot].store(nowMs, std::memory_order_relaxed);
        ptr[slot].store(p, std::memory_order_relaxed);
    }
    uint32_t live(uint64_t nowMs, uint64_t windowMs) const noexcept {
        uint32_t n = 0;
        for (size_t i = 0; i < kCompTableSize; ++i) {
            if (ptr[i].load(std::memory_order_relaxed) == 0) continue;
            const uint64_t t = seenMs[i].load(std::memory_order_relaxed);
            if (t + windowMs >= nowMs) ++n;
        }
        return n;
    }
};

// ---- the episodes ---------------------------------------------------------------------------------------------------------------------------------
// An episode is a run of three: asks in a row that no local call took, asks in a row that met a local path A or SKIP call, or updates in a row whose first
// processor call was not the local comp's. It ends when none of the three runs is at three any more.
constexpr uint32_t kEpisodeRun = 3;
enum EpEvent : uint8_t { kEvConsumed, kEvUnconsumed, kEvAorSkip, kEvFirstOurs, kEvFirstNot };
enum EpReason : uint8_t { kRsUnconsumed = 1, kRsAorSkip = 2, kRsFirstNot = 4 };
struct Episode {
    uint32_t runUnc = 0, runAS = 0, runFirst = 0;
    bool active = false;
    uint64_t beginMs = 0;
    uint32_t reasons = 0;
    // inside the episode (the run that opened it included)
    uint32_t unconsumed = 0, aOrSkip = 0, firstNot = 0, asks = 0;
    // Returns +1 when this event opens an episode, -1 when it closes one, else 0.
    int observe(EpEvent e, uint64_t nowMs) noexcept {
        switch (e) {
        case kEvConsumed: runUnc = runAS = 0; if (active) ++asks; break;
        case kEvUnconsumed: ++runUnc; runAS = 0; if (active) { ++unconsumed; ++asks; } break;
        case kEvAorSkip: ++runAS; runUnc = 0; if (active) { ++aOrSkip; ++asks; } break;
        case kEvFirstOurs: runFirst = 0; break;
        case kEvFirstNot: ++runFirst; if (active) ++firstNot; break;
        }
        const bool want = runUnc >= kEpisodeRun || runAS >= kEpisodeRun || runFirst >= kEpisodeRun;
        if (want && !active) {
            active = true;
            beginMs = nowMs;
            reasons = (runUnc >= kEpisodeRun ? kRsUnconsumed : 0u) | (runAS >= kEpisodeRun ? kRsAorSkip : 0u) | (runFirst >= kEpisodeRun ? kRsFirstNot : 0u);
            unconsumed = runUnc;
            aOrSkip = runAS;
            firstNot = runFirst;
            asks = runUnc + runAS;
            return +1;
        }
        if (want && active) {
            reasons |= (runUnc >= kEpisodeRun ? kRsUnconsumed : 0u) | (runAS >= kEpisodeRun ? kRsAorSkip : 0u) | (runFirst >= kEpisodeRun ? kRsFirstNot : 0u);
        }
        if (!want && active) {
            active = false;
            return -1;
        }
        return 0;
    }
};
inline const char* reasonText(uint32_t mask, char* buf, size_t cap) noexcept {
    if (!buf || cap == 0) return "";
    buf[0] = 0;
    size_t used = 0;
    const struct { uint32_t bit; const char* text; } parts[3] = {{kRsUnconsumed, "3 asks in a row that no local call took"},
                                                                   {kRsAorSkip, "3 asks in a row that met a local path A or SKIP call"},
                                                                   {kRsFirstNot, "the first call after the update was not the local comp's for 3 updates in a row"}};
    for (const auto& p : parts) {
        if (!(mask & p.bit)) continue;
        const int n = std::snprintf(buf + used, cap - used, "%s%s", used ? "; " : "", p.text);
        if (n < 0 || static_cast<size_t>(n) >= cap - used) break;
        used += static_cast<size_t>(n);
    }
    return buf;
}

// ---- the event lines' budget --------------------------------------------------------------------------------------------------------------------------
// At most `limit` lines in any 60 s (an event line is a measurement of a rare thing: a flood of them is its own finding, counted and said in the next one).
struct RateCap {
    static constexpr uint32_t kMax = 6;
    uint64_t t[kMax] = {};
    uint32_t n = 0, head = 0, suppressed = 0;
    bool allow(uint64_t nowMs, uint32_t limit) noexcept {
        if (limit > kMax) limit = kMax;
        uint32_t recent = 0;
        for (uint32_t i = 0; i < n; ++i)
            if (nowMs < t[i] || nowMs - t[i] < 60000u) ++recent;
        if (recent >= limit) {
            ++suppressed;
            return false;
        }
        t[head] = nowMs;
        head = (head + 1) % kMax;
        if (n < kMax) ++n;
        return true;
    }
    uint32_t takeSuppressed() noexcept {
        const uint32_t s = suppressed;
        suppressed = 0;
        return s;
    }
};

}  // namespace f129
}  // namespace nofoot
}  // namespace edvr
