// TEMPORARY (VR draw path CPU, build 4, item 7; docs\vr-draw-path-cpu-2026-11-01.md): the counters that decide build 5, and the window line that prints them
// beside "draw mask:" in perf_monitor.cpp. Remove with the arc.
//
// bindingSet's calls for the six slots the draw path's per-binding caches read (RTV0, DSV0, PS SRV 0-3), split into the calls that set the pointer already
// bound (compared before it is overwritten) and the null-to-null ones, counted apart; and the mapMemo lookups in hookedMap with the ones that missed. Every
// bindingSet bumps the slot's generation today, so a same-pointer re-set invalidates every cache keyed on it: this is the upper bound of what a content
// generation would save. Plain counters: every bindingSet caller and hookedMap's memo are owner-only, so a window's read is a snapshot, not a lock.
// perf_monitor.cpp prints the line every 1800 frames; tools\edvr_log.py --perf-windows parses it, and its self-test holds the fragments to this file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

// The slots counted, in BindSlot order (binding_shadow.h: Rtv0 = 0, Dsv0, PsSrv0..PsSrv3).
constexpr size_t kBindSetSlots = 6;

inline const char* bindSetSlotName(size_t k) {
    static const char* const names[kBindSetSlots] = {"rtv0", "dsv0", "ps_srv0", "ps_srv1", "ps_srv2", "ps_srv3"};
    return k < kBindSetSlots ? names[k] : "unknown";
}

struct BindSetSlotCounts {
    uint32_t calls = 0;        // bindingSet calls
    uint32_t samePtr = 0;      // ...whose pointer equals the one already bound, both non-null
    uint32_t nullToNull = 0;   // ...that set null over null
};

struct BindSetCounts {
    uint32_t mapMemoLookups = 0;   // hookedMap's mapMemo lookups (a Map of a buffer that is not the composite or the camera block)
    uint32_t mapMemoMisses = 0;    // ...that found another resource in the slot, and paid mapBufferDesc
    BindSetSlotCounts slots[kBindSetSlots];
};

// One compare and increment per call, called by bindingSet before it overwrites the pointer.
inline void bindSetNote(BindSetCounts& c, size_t slot, const void* bound, const void* incoming) {
    if (slot >= kBindSetSlots) return;
    BindSetSlotCounts& s = c.slots[slot];
    ++s.calls;
    if (bound == incoming) {
        if (incoming) ++s.samePtr;
        else ++s.nullToNull;
    }
}

// The change since the last window, formatted: about 300 characters.
inline void formatBindSetWindow(char* out, size_t cap, unsigned frame, const BindSetCounts& c) {
    int n = std::snprintf(out, cap, "binding re-sets: 1800-frame window ending %u; mapMemo lookups %u, misses %u; bindingSet calls/same pointer/null to null: ",
                          frame, c.mapMemoLookups, c.mapMemoMisses);
    if (n < 0 || static_cast<size_t>(n) >= cap) return;
    size_t at = static_cast<size_t>(n);
    for (size_t k = 0; k < kBindSetSlots; ++k) {
        n = std::snprintf(out + at, cap - at, "%s%s %u/%u/%u", k ? ", " : "", bindSetSlotName(k), c.slots[k].calls, c.slots[k].samePtr, c.slots[k].nullToNull);
        if (n < 0 || static_cast<size_t>(n) >= cap - at) return;
        at += static_cast<size_t>(n);
    }
    std::snprintf(out + at, cap - at, ".");
}

}  // namespace edvr
