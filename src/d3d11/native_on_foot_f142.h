// F142 (native on-foot VR): the pure parts of three changes. Header only, no game or D3D11 state, so native_frame_test runs the very functions the render thread, the
// avatar hooks and the frame thread use. Journal: the arc doc, 2026-10-27, "the F139 to F141 flight" and F142.
//
// 1. The icon mesh table's slot reuse (F67d's g_iconMesh). The table held six slots, each taken for good by every distinct draw configuration, the oversize index ranges it
//    then refused included. In the 2026-10-27 flight all six were gone before the first weapon readout drew, so iconCapture returned -1, the readout's mesh stayed unread (0.00
//    x 0.00) and F90, F92 and F140 never applied. A configuration that is still drawing keeps its slot, refused or not (a refused slot is how the draw is not copied again).
//    A new configuration finds a free slot, else the slot not seen for the longest, and only one not seen for kIconReuseMinAgeFrames or more; a slot a copy is in flight for,
//    one the staging ring names, and one a live record's icon index names is never reused.
//
// 2. The full outfit (behind a `fix.` switch until F146 removed it: it is always on; F147: only EVASuit_Gear_Back is promoted, and only for an outer garment). The local third-person avatar's parts each have a
//    visibility class (AMC+0x1820+idx*0x680: 0 never, 1 every view, 2 the shadow views only, 3 the main views only; native_on_foot_f95c.h). In the coat outfit the game gives the
//    coat's part (EVASuit_Gear_Back) class 2 in first person, so it is in the shadows only and the suit shows through. The full outfit gives class 1 to every class-2 part that is
//    not a head part and not the first-person skeleton for the length of the avatar's visibility call (F95c's compare-exchange swap, the other way) and puts the game's class back
//    after it.
//
// 3. The outfit census: per part of the local third-person avatar, the instances present, the class, the flags, the current and pending item and the view masks, written when
//    any of them but the masks changes (the masks come and go with the scene's views).
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "native_on_foot_f95c.h"

namespace edvr {
namespace nofoot {
namespace f142 {

// ---- 1. the icon mesh table ---------------------------------------------------------------------------------------------------------------------------------------
// The slot states (IconMesh::state): free, a copy in flight, decoded (disc valid), refused (kept: the draw is not copied again).
constexpr int kIconFree = 0, kIconInFlight = 1, kIconDecoded = 2, kIconRefused = 3;
// The table's size. 32: the 2026-10-27 flight had six configurations by the first minute (the 1x1 quad, three 3600-index label configurations, two 624-index discs) and a
// weapon adds two or three readout parts; only a configuration drawn in the last 60 frames holds a slot, so starving needs more than 32 distinct configurations drawing in
// one second. IconMesh is about 300 bytes and Rec::icon is an int8_t (127); the victim search keeps its skip set in 64 bits.
constexpr int kIconMeshes = 32;
static_assert(kIconMeshes <= 64 && kIconMeshes <= 127, "the victim search's skip set is 64 bits and Rec::icon is an int8_t");
constexpr uint32_t kIconReuseMinAgeFrames = 60;   // a slot not seen for this many frames may be reused (60 frames is about 0.7 s at 90 Hz)

// Frames since a slot's configuration was last drawn; a stamp from the future (a frame counter that went back) is age 0, never a huge age that would free a live slot.
inline uint32_t iconAge(uint32_t frame, uint32_t seen) noexcept { return frame >= seen ? frame - seen : 0u; }

// The slot to reuse for a new configuration, or -1. state[i] and seen[i] describe n slots (n <= 64); `pinned(i)` says a slot something still points at (the staging ring's
// owner, a live record's icon index). Only decoded or refused slots are candidates (a free slot is the caller's own first choice; one with a copy in flight is never); of those
// aged minAge frames or more and not pinned, the oldest, the lowest index on a tie. The pinned test is asked of a candidate only, oldest first, so a full table's scan of the
// records is made only when a slot has aged out.
template <class PinnedFn>
inline int iconVictim(const int* state, const uint32_t* seen, int n, uint32_t frame, uint32_t minAge, PinnedFn&& pinned) noexcept {
    if (!state || !seen || n <= 0 || n > 64) return -1;
    uint64_t skip = 0;
    for (int round = 0; round < n; ++round) {
        int best = -1;
        uint32_t bestAge = 0;
        for (int i = 0; i < n; ++i) {
            if ((skip >> i) & 1u) continue;
            if (state[i] != kIconDecoded && state[i] != kIconRefused) continue;
            const uint32_t age = iconAge(frame, seen[i]);
            if (age < minAge) continue;
            if (best < 0 || age > bestAge) {
                best = i;
                bestAge = age;
            }
        }
        if (best < 0) return -1;
        if (!pinned(best)) return best;
        skip |= 1ull << best;
    }
    return -1;
}

// The slots by state, for the table's line.
struct IconCounts {
    int free = 0, inFlight = 0, decoded = 0, refused = 0;
    uint32_t oldestIdle = 0;   // frames the longest-idle used slot has not been seen
};
inline IconCounts iconCount(const int* state, const uint32_t* seen, int n, uint32_t frame) noexcept {
    IconCounts c;
    for (int i = 0; i < n; ++i) {
        switch (state[i]) {
            case kIconFree: ++c.free; break;
            case kIconInFlight: ++c.inFlight; break;
            case kIconDecoded: ++c.decoded; break;
            default: ++c.refused; break;
        }
        if (state[i] != kIconFree) {
            const uint32_t a = iconAge(frame, seen[i]);
            if (a > c.oldestIdle) c.oldestIdle = a;
        }
    }
    return c;
}
// A count whose line is written now: 1, 2, 4, 8, ... (the table-full line at each doubling of the draws that had no slot), capped by the caller.
inline bool powerOfTwo(uint32_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

// ---- 2. the full outfit promotion -------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kPartCount = 54;   // avatar_hooks_core.h kPartCount (checked where both are included)
// The class a part takes for the visibility call. A head part is the F95c swap's and the first-person skeleton the game's own (the third-person camera's arms are drawn
// elsewhere): both keep the game's class. Of the rest, the shadow-only class becomes every view; 0, 1 and 3 stay.
inline int32_t promotedClass(int32_t was, bool headPart, bool skeletonPart) noexcept {
    if (headPart || skeletonPart) return was;
    return was == f95c::kClassShadow ? f95c::kClassAll : was;
}
struct OutfitSwap {
    uint8_t* amc = nullptr;
    uint32_t n = 0;                          // parts whose class was changed for the call
    uint32_t refused = 0;                    // classes the game changed between the read and the compare-exchange (left alone, not recorded)
    uint8_t part[kPartCount];                // their indices (not zeroed: only the first n are read, and this is made on every visibility call)
    int32_t was[kPartCount];                 // the game's class
    int32_t set[kPartCount];                 // the class written for the call
    bool armed = false;                      // the classes were written: End restores
};
// The swap's two loops, shared by the avatar hook (volatile LONG reads and InterlockedCompareExchange under SEH) and the rig (plain ints). read(idx) -> the class now;
// cas(idx, desired, expected) -> true when the class was `expected` and is `desired` now. promoteClasses writes the promotion of every part and records what it changed;
// restoreClasses puts each recorded part's game class back if it is still the class written, and returns how many the game changed during the call (they stand).
// F147 (the review's N5): this is a compare-exchange on the class WRITTEN, so a game write of that very value during the call (the game choosing every view for the part itself, say
// on a camera change) looks like our own and is put back to the shadow-only class; `lost` does not count it. The window is the game's one visibility call (microseconds), and the
// class is state the game writes only from its pending requests (native_on_foot_f95c.h), so a write undone this way would stand until the next request touches the part. It cannot be
// closed from here: the four classes 0..3 all mean something to the game, so there is no spare value to tell ours apart by, and a value outside them has not been flown. The same
// shape is F95c's swap. Left as it is; a part found in the shadow-only class after the game chose every view for it would be this.
template <class ReadFn, class CasFn>
inline void promoteClasses(OutfitSwap& s, uint32_t skeletonIdx, bool (*isHead)(uint32_t), ReadFn&& read, CasFn&& cas, uint64_t skipMask = 0) noexcept {
    for (uint32_t i = 0; i < kPartCount; ++i) {
        if ((skipMask >> i) & 1ull) continue;   // F144: a part never promoted (Gear_Back when it is a backpack)
        const int32_t was = read(i);
        const int32_t set = promotedClass(was, isHead && isHead(i), i == skeletonIdx);
        if (set == was) continue;
        if (!cas(i, set, was)) {
            ++s.refused;
            continue;
        }
        s.part[s.n] = static_cast<uint8_t>(i);
        s.was[s.n] = was;
        s.set[s.n] = set;
        ++s.n;
    }
}
template <class CasFn>
inline uint32_t restoreClasses(const OutfitSwap& s, CasFn&& cas) noexcept {
    uint32_t lost = 0;
    for (uint32_t j = 0; j < s.n; ++j)
        if (!cas(s.part[j], s.was[j], s.set[j])) ++lost;
    return lost;
}

// ---- 3. the outfit census ---------------------------------------------------------------------------------------------------------------------------------------------
struct PartRow {
    uint8_t idx = 0;
    uint8_t inst = 0;        // bit k: instance k (0..3) exists
    uint8_t flags = 0;       // +0x1814 (0x10 = excluded)
    int32_t state = 0;       // +0x1820
    int32_t current = 0;     // +0x181C
    int32_t pending = 0;     // +0x1818
    uint64_t masks[4] = {};  // the four instances' view masks, as read (not part of the signature)
};
struct Census {
    uint64_t amc = 0;
    uint32_t n = 0;          // rows used: the parts with an instance
    PartRow row[kPartCount];
};
static_assert(std::is_trivially_copyable<Census>::value, "the census is copied between threads");
inline uint64_t sigMix(uint64_t h, uint64_t v) noexcept { return (h ^ v) * 1099511628211ull; }
// What a change is: the AMC, or any part's presence, instances, class, flags or items. Never the masks.
inline uint64_t censusSig(const Census& c) noexcept {
    uint64_t h = sigMix(14695981039346656037ull, c.amc);
    h = sigMix(h, c.n);
    for (uint32_t i = 0; i < c.n && i < kPartCount; ++i) {
        const PartRow& p = c.row[i];
        h = sigMix(h, (static_cast<uint64_t>(p.idx) << 8) | p.inst);
        h = sigMix(h, p.flags);
        h = sigMix(h, static_cast<uint32_t>(p.state));
        h = sigMix(h, static_cast<uint32_t>(p.current));
        h = sigMix(h, static_cast<uint32_t>(p.pending));
    }
    return h;
}
inline const PartRow* findRow(const Census& c, uint32_t idx) noexcept {
    for (uint32_t i = 0; i < c.n && i < kPartCount; ++i)
        if (c.row[i].idx == idx) return &c.row[i];
    return nullptr;
}
// The parts that differ between two censuses (appeared, vanished, or any signature field changed): their indices into out, up to cap; returns the full count.
inline uint32_t changedParts(const Census& before, const Census& after, uint8_t* out, uint32_t cap) noexcept {
    uint32_t n = 0;
    const auto note = [&](uint32_t idx) {
        if (n < cap) out[n] = static_cast<uint8_t>(idx);
        ++n;
    };
    for (uint32_t i = 0; i < after.n && i < kPartCount; ++i) {
        const PartRow& a = after.row[i];
        const PartRow* b = findRow(before, a.idx);
        if (!b || b->inst != a.inst || b->flags != a.flags || b->state != a.state || b->current != a.current || b->pending != a.pending) note(a.idx);
    }
    for (uint32_t i = 0; i < before.n && i < kPartCount; ++i)
        if (!findRow(after, before.row[i].idx)) note(before.row[i].idx);
    return n;
}
// The class as the log says it.
inline const char* classText(int32_t state) noexcept {
    switch (state) {
        case 0: return "never";
        case 1: return "every view";
        case 2: return "shadow views only";
        case 3: return "main views only";
        default: return "unknown";
    }
}

}  // namespace f142
}  // namespace nofoot
}  // namespace edvr
