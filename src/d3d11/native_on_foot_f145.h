// F145 (native on-foot VR): the pure parts of two changes. Header only, no game or D3D11 state, so native_frame_test runs the very functions the submit hook and the proof use.
// Journal: the arc doc, 2026-10-27, "the F144 flight" and F145.
//
// 1. The weapon UI's tag slots. F85b tags a readout part's submit with a key, 0x70 + a slot, and the slot is given to a part's name id the first time it is submitted. There are
//    eight slots and a slot was never given back. The game makes new part ids at every loadout switch (the F144 flight: 30900..30915 for "Rocket man", 32104..32689 for "Combat",
//    32334..29237 for "Combat Plasma", 32432/31837 for the plasma pistol drawn later, 32997/32832/29157 for the sniper), so the table was full after the third loadout: the plasma
//    pistol's ADS readout (31837) and all of the scoped sniper's parts were submitted every frame and never tagged, never decided, never placed. A name with no slot now takes the
//    slot whose name was asked for the longest ago, once that was at least kSlotIdleMs ago; a part drawn in the last few seconds keeps its slot.
//
// 2. The proof against the seed. The F144 proof compared a placed part's drawn centre with the same frame's unplaced draw (the game's U), and found the drawn centre by solving a
//    camera from the patched rows. f85::solveK divides by the w row's length, and a size factor (the ADS roles' 16/9 and 4/7) scales the rows, so the solved camera put the part
//    at 1/k of its distance: the ADS roles read "differs" by 15 to 20 cm in every window whatever the seed and the trims (the flight: forward -15.6, left +9.6, up +4.3 cm from a
//    3.5 cm total, unchanged while the trim moved). drawnPoint reads the point through the rows' own map instead, and the reference is the same frame placed at the seed alone, so
//    the check answers the pilot's question: do the trims move the drawn part by the trims. The move against the game's own draw stays as information.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "native_on_foot_f90.h"

namespace edvr {
namespace nofoot {
namespace f145 {

// ---- 1. the slot table --------------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kSlotIdleMs = 5000;   // a name not asked for this long may give its slot to another
enum SlotHow : int { kSlotHit = 0, kSlotFree, kSlotRecycled, kSlotNone };
struct SlotPick {
    int slot = -1;
    int how = kSlotNone;
    uint32_t oldName = 0;   // the name+1 the slot held (kSlotRecycled)
    uint64_t idleMs = 0;    // how long it had been unasked (kSlotRecycled)
};
// names[i]: the slot's name id + 1 (0 free); seenMs[i]: the tick it was last asked for. The slot already holding `want`, else the first free one, else the one unasked the longest
// provided that is at least kSlotIdleMs, else none (every slot is in use).
inline SlotPick pickSlot(const uint32_t* names, const uint64_t* seenMs, uint32_t n, uint32_t want, uint64_t nowMs) noexcept {
    SlotPick p;
    if (!names || !seenMs || !want) return p;
    for (uint32_t i = 0; i < n; ++i)
        if (names[i] == want) {
            p.slot = static_cast<int>(i);
            p.how = kSlotHit;
            return p;
        }
    for (uint32_t i = 0; i < n; ++i)
        if (names[i] == 0) {
            p.slot = static_cast<int>(i);
            p.how = kSlotFree;
            return p;
        }
    int best = -1;
    uint64_t bestIdle = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t idle = nowMs > seenMs[i] ? nowMs - seenMs[i] : 0;
        if (best < 0 || idle > bestIdle) {
            best = static_cast<int>(i);
            bestIdle = idle;
        }
    }
    if (best >= 0 && bestIdle >= kSlotIdleMs) {
        p.slot = best;
        p.how = kSlotRecycled;
        p.oldName = names[best];
        p.idleMs = bestIdle;
    }
    return p;
}
// F147: the claim the submit hook makes on the shared slot arrays (names[i]: the name id + 1, 0 free; seenMs[i]: the tick it was last asked for; gens[i]: bumped when the slot
// changes hands). pickSlot decides on a snapshot; the claim itself is a compare-exchange on the slot's name, so two submits that picked the same victim (or the same free slot)
// cannot both win it: the loser takes a fresh snapshot and picks again (it may find the winner holding the very name it wants, a hit). The slot's tick is stored BEFORE the
// compare-exchange, so a snapshot that sees the new name (acquire) also sees the new tick and does not take the winner's slot for an idle one; a loser's store leaves the tick
// of a slot someone has just claimed at "now", which it was. The generation is bumped after the name changes, so a reader that sees the new generation never sees the old name
// beside it. `afterPick(pick)` is called between the snapshot and the claim (a no-op in the product; the rig makes a rival claim there). kSlotNone: every slot is in use, or four
// tries lost their race. The one window left: a name asked for again after its slot sat idle for kSlotIdleMs, at the very moment another name recycles that slot; the claim re-checks
// the name after refreshing the tick, which narrows it to a few instructions, and the next ask of that name finds no hit and takes another slot.
template <uint32_t N, class AfterPick>
inline SlotPick claimSlotWith(std::atomic<uint32_t>* names, std::atomic<uint64_t>* seenMs, std::atomic<uint32_t>* gens, uint32_t want, uint64_t nowMs, AfterPick&& afterPick) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint32_t n[N];
        uint64_t s[N];
        for (uint32_t i = 0; i < N; ++i) {
            n[i] = names[i].load(std::memory_order_acquire);   // the name first: a new name seen here makes its tick (stored before it) visible to the next load
            s[i] = seenMs[i].load(std::memory_order_relaxed);
        }
        const SlotPick p = pickSlot(n, s, N, want, nowMs);
        afterPick(p);
        switch (p.how) {
            case kSlotHit:
                seenMs[p.slot].store(nowMs, std::memory_order_relaxed);
                if (names[p.slot].load(std::memory_order_acquire) != want) continue;   // recycled under this call: pick again
                return p;
            case kSlotFree: {
                seenMs[p.slot].store(nowMs, std::memory_order_relaxed);
                uint32_t cur = 0;
                if (!names[p.slot].compare_exchange_strong(cur, want, std::memory_order_acq_rel, std::memory_order_relaxed)) continue;
                return p;
            }
            case kSlotRecycled: {
                seenMs[p.slot].store(nowMs, std::memory_order_relaxed);
                uint32_t cur = p.oldName;
                if (!names[p.slot].compare_exchange_strong(cur, want, std::memory_order_acq_rel, std::memory_order_relaxed)) continue;
                gens[p.slot].fetch_add(1u, std::memory_order_release);
                return p;
            }
            default:
                return p;
        }
    }
    return SlotPick{};
}
template <uint32_t N>
inline SlotPick claimSlot(std::atomic<uint32_t>* names, std::atomic<uint64_t>* seenMs, std::atomic<uint32_t>* gens, uint32_t want, uint64_t nowMs) noexcept {
    return claimSlotWith<N>(names, seenMs, gens, want, nowMs, [](const SlotPick&) {});
}
// A slot event for the frame thread's log (the submit hook runs on the game's thread and does not write the log): a recycle, or a name that found every slot in use.
enum EventKind : uint8_t { kEvRecycled = 1, kEvNoSlot = 2 };
struct SlotEvent {
    uint8_t kind = 0, slot = 0;
    uint16_t name = 0, oldName = 0;   // name ids (not +1)
    uint32_t idleMs = 0;
};
// F147: any number of producers (the submit hooks), one consumer (the frame thread). A bounded queue whose cells each carry a sequence number (Vyukov's): a producer claims a
// position with a compare-exchange only when that cell is free, writes the event, then publishes the cell; the consumer takes a cell only once it is published, so no event is
// read half written and two producers never write the same cell. N is a power of two.
template <uint32_t N>
struct EventRing {
    static_assert(N >= 2u && (N & (N - 1u)) == 0u, "EventRing<N>: N must be a power of two");
    struct Cell {
        std::atomic<uint32_t> seq{0};
        SlotEvent ev;
    };
    Cell cell[N];
    std::atomic<uint32_t> head{0};   // the next position a producer claims
    uint32_t tail = 0;               // the consumer's own
    EventRing() noexcept {
        for (uint32_t i = 0; i < N; ++i) cell[i].seq.store(i, std::memory_order_relaxed);
    }
    bool push(const SlotEvent& e) noexcept {
        uint32_t pos = head.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cell[pos % N];
            const int32_t diff = static_cast<int32_t>(c.seq.load(std::memory_order_acquire) - pos);
            if (diff == 0) {
                if (head.compare_exchange_weak(pos, pos + 1u, std::memory_order_relaxed)) {
                    c.ev = e;
                    c.seq.store(pos + 1u, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;   // the consumer has not freed this cell: full
            } else {
                pos = head.load(std::memory_order_relaxed);   // another producer took it
            }
        }
    }
    bool take(SlotEvent* out) noexcept {
        Cell& c = cell[tail % N];
        if (c.seq.load(std::memory_order_acquire) != tail + 1u) return false;
        *out = c.ev;
        c.seq.store(tail + N, std::memory_order_release);
        ++tail;
        return true;
    }
};

// ---- 2. the proof against the seed ------------------------------------------------------------------------------------------------------------------------------------------
// The world point (relative to the eye's rows' origin) the patched composite rows draw a hud-frame point v at. The patched rows are B . M (f85::applyCorrection: B the unpatched
// composite's rows, M the affine map [M3 | m4] that carries v to where the camera U puts it), and the composite camera K draws M v at K3 (M3 v + m4) + k4. M is recovered from the
// stored rows themselves, M3 = B3^-1 B'3 and m4 = B3^-1 (B'4 - B4), so the float rounding of the rows is in the answer. (Solving a camera K' from the patched rows is not the same:
// f85::solveK divides by the w row's length, so a size factor k, which scales the rows by k, reads as the same part at 1/k times its distance: the ADS roles' proof read 15 cm
// off at 35 cm from the camera, whatever the seed and the trims.)
inline bool drawnPoint(const f85::CamK& K, const f85::Rows& B, const f85::Rows& Bp, const double v[3], double out[3]) noexcept {
    if (!K.ok) return false;
    double B3[3][3], B3i[3][3], Bp3[3][3], M3[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            B3[r][c] = B.r[r][c];
            Bp3[r][c] = Bp.r[r][c];
        }
    if (!f85::inv3(B3, B3i)) return false;
    f85::mul33(B3i, Bp3, M3);
    const double d[3] = {Bp.r[0][3] - B.r[0][3], Bp.r[1][3] - B.r[1][3], Bp.r[2][3] - B.r[2][3]};
    double m4[3], mv[3], p[3];
    f85::mulVec3(B3i, d, m4);
    f85::mulVec3(M3, v, mv);
    for (int i = 0; i < 3; ++i) mv[i] += m4[i];
    f85::mulVec3(K.K3, mv, p);
    for (int i = 0; i < 3; ++i) out[i] = p[i] + K.k4[i];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}
// The map the same frame would have had with the seed alone: the decision's size factor about the content centre (if any) and a move to the seed's target instead of the total's.
// The target is taken from the base the decision took its own from (o.Cref: the part's content centre, except for the pistol's ADS role, which takes the hip part's), in the
// weapon frame the decision used. False when the seed needs what the decision did not have.
inline bool seedOnlyMap(const f90::Out& o, const f90::Keys& seed, const double* eye, f90::Map* out) noexcept {
    if (!out) return false;
    *out = o.map;
    out->move = false;
    for (int i = 0; i < 3; ++i) out->t[i] = 0.0;
    if (!seed.any()) return true;
    double T[3];
    const double* base = o.haveCref ? o.Cref : o.C;
    f90::WeaponFrame wf;
    if (o.haveWf) wf = o.wfUsed;
    if (!f90::target(base, wf, seed, eye, T)) return false;
    out->move = true;
    for (int i = 0; i < 3; ++i) out->t[i] = T[i] - o.C[i];
    return true;
}
// The move the trims ask: the decision's move less the seed-only map's (metres, world).
inline void trimMove(const f90::Map& total, const f90::Map& seed, double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = (total.move ? total.t[i] : 0.0) - (seed.move ? seed.t[i] : 0.0);
}

}  // namespace f145
}  // namespace nofoot
}  // namespace edvr
