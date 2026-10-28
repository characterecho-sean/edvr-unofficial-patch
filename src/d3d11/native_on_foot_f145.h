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
// A slot event for the frame thread's log (the submit hook runs on the game's thread and does not write the log): a recycle, or a name that found every slot in use.
enum EventKind : uint8_t { kEvRecycled = 1, kEvNoSlot = 2 };
struct SlotEvent {
    uint8_t kind = 0, slot = 0;
    uint16_t name = 0, oldName = 0;   // name ids (not +1)
    uint32_t idleMs = 0;
};
// One producer (the submit hook), one consumer (the frame thread).
template <uint32_t N>
struct EventRing {
    SlotEvent ev[N];
    std::atomic<uint32_t> head{0}, tail{0};
    bool push(const SlotEvent& e) noexcept {
        const uint32_t h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) >= N) return false;
        ev[h % N] = e;
        head.store(h + 1u, std::memory_order_release);
        return true;
    }
    bool take(SlotEvent* out) noexcept {
        const uint32_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        *out = ev[t % N];
        tail.store(t + 1u, std::memory_order_release);
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
