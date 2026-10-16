// What the flat draw scope's constructor learns from a draw's shader pair alone, remembered per pair.
//
// WHY (the settlement CPU arc, docs\flat-cz-fps-2026-10-11.md, 2026-10-16 round 3). The constructor asked the same questions of the
// same (vertex shader, pixel shader) hashes on every one of 14.6k draws a frame: is the pair a keyed pool family (two table scans),
// is the vertex shader a pool family's, is it a known tone pass or the map's tonemap or a UI-only source tone, is it the final copy's
// pair, and could the UI layer's family rule ever name it. Every answer is a pure function of the two hashes and of whether the
// profile is flat (the family table has flat-only and VR-only rows); none reads a binding, a frame, a target or any mutable state.
//
// THE KEY, all of it: (vs hash, ps hash, flat profile). Nothing else enters an answer, and the rig holds that: it compares the memo
// with a fresh computation over every table entry, unknown hashes, both profiles and a table small enough to collide, and it
// instantiates the memo with each key part dropped in turn and requires the comparison to FAIL (Dropped = 1 vs, 2 ps, 4 flat).
//
// What is NOT here, deliberately: anything per target or per frame (contract kind, the copy draw's target test, the UI layer's
// target class, map-open): those are recomputed per draw as before. The UI "may ask" answer is the OR over the four combinations
// of (map open, HDR target) of the family rule, so it cannot be wrong for any frame; a pair that can never be asked skips the
// layer's input gathering and its decision, which would have returned "not asked" without a side effect (flatUiLayerDecide's
// first lines; the rig's wiring pin holds that).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine_velocity_families.h"
#include "flat_mono_frame.h"
#include "flat_ui_layer_math.h"

namespace edvr {

struct FlatPairFacts {
    bool supported = false;       // a keyed pool-family pair (engineVelocityPoolFamilyPair)
    bool familyVs = false;        // a pool family's vertex shader, keyed pixel shader or not (engineVelocityPoolFamilyVs)
    uint32_t toneSlot = ~0u;      // flat_mono_detail::toneHdrSlot: the HDR lineage slot of a registry tone pass, else ~0u
    uint32_t mapToneSlot = ~0u;   // flatUiMapToneSlotOf: the map frames' tonemap (the UI layer's own table), else ~0u
    bool sourceTone = false;      // flatUiSourceTonePair: the UI-only LDR-source tone pairs (E894, 0D43)
    bool copyPair = false;        // the final copy's shader pair (the draw must also write the output to be the copy)
    bool uiMayAsk = false;        // flatUiFamilyFor names the pair for some (map open, HDR target)
    friend bool operator==(const FlatPairFacts& a, const FlatPairFacts& b) {
        return a.supported == b.supported && a.familyVs == b.familyVs && a.toneSlot == b.toneSlot && a.mapToneSlot == b.mapToneSlot &&
               a.sourceTone == b.sourceTone && a.copyPair == b.copyPair && a.uiMayAsk == b.uiMayAsk;
    }
};

// The reference: each field computed the way the constructor computed it before the memo. engineVelocityPoolFamilyPair and
// engineVelocityPoolFamilyVs are these two inline calls (engine_velocity.cpp, pinned by the wiring scan).
inline FlatPairFacts flatPairFactsFresh(uint64_t vs, uint64_t ps, bool flat) noexcept {
    FlatPairFacts f;
    const int family = engine_velocity_family::familyForProfile(vs, flat);
    f.supported = engine_velocity_family::keyedPs(family, ps, flat);
    f.familyVs = family >= 0;
    f.toneSlot = flat_mono_detail::toneHdrSlot(vs, ps);
    f.mapToneSlot = flatUiMapToneSlotOf(vs, ps);
    f.sourceTone = flatUiSourceTonePair(vs, ps);
    f.copyPair = vs == flat_mono_detail::kCopyVs && ps == flat_mono_detail::kCopyPs;
    for (int map = 0; map < 2 && !f.uiMayAsk; ++map)
        for (int hdr = 0; hdr < 2 && !f.uiMayAsk; ++hdr)
            f.uiMayAsk = flatUiFamilyFor(vs, ps, map != 0, hdr != 0) != UiLayerFamily::kNone;
    return f;
}

// Direct-mapped, owner thread only (the draw scope's constructor runs only there). `Slots` a power of two. `Dropped` is the rig's: a
// memo that ignores a key part, to be seen to fail (1 = vs, 2 = ps, 4 = flat profile). Production uses 0.
template <size_t Slots = 1024, unsigned Dropped = 0>
class FlatPairFactsMemoT {
    static_assert((Slots & (Slots - 1)) == 0, "the index is a mask");
public:
    const FlatPairFacts& get(uint64_t vs, uint64_t ps, bool flat) noexcept {
        const uint64_t kv = (Dropped & 1u) ? 0 : vs, kp = (Dropped & 2u) ? 0 : ps;
        const bool kf = (Dropped & 4u) ? false : flat;
        // (the profile is in the index too, so the two profiles of one pair do not evict each other)
        Entry& e = entries_[static_cast<size_t>(((kv * 0x9E3779B97F4A7C15ull) ^ (kp * 0xC2B2AE3D27D4EB4Full) ^ (kf ? 0xD6E8FEB86659FD93ull : 0)) >> 40) & (Slots - 1)];
        if (!e.valid || e.vs != kv || e.ps != kp || e.flat != kf) {
            e.valid = true;
            e.vs = kv;
            e.ps = kp;
            e.flat = kf;
            e.facts = flatPairFactsFresh(vs, ps, flat);
            ++misses_;
        }
        return e.facts;
    }
    uint64_t misses() const noexcept { return misses_; }
    void clear() noexcept { *this = FlatPairFactsMemoT(); }

private:
    struct Entry {
        bool valid = false, flat = false;
        uint64_t vs = 0, ps = 0;
        FlatPairFacts facts;
    };
    Entry entries_[Slots];
    uint64_t misses_ = 0;
};
using FlatPairFactsMemo = FlatPairFactsMemoT<>;

}  // namespace edvr
