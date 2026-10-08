#pragma once
// The line that names the writes that cost the first-person history its priors, and what range-aware invalidation kept (design doc section 104,
// the 10-07 settlement walk). Plain text from plain counters: tools\flat_temporal_test builds the line from known counts and holds its keys to
// the names the counters carry, so a log reader and the code that writes the line cannot drift apart.
//
// `flat foreground history 5s:` carries, for the 5 s window, with every zero printed:
//  - the history's size: records and bytes held, their peaks, and the allocate() calls (a record's four buffers and six views);
//  - the erase paths (HistoryErase): where records left, by path;
//  - the write notifications by entry (animated_history_writes.h): observed, touching a live record's vertex or index buffer (by timing), the
//    ones that invalidated, how many carried a range, and what the range saved: writes that touched records and invalidated none, and the
//    records a range left alone. `whole-resource` entries cannot save anything; if map, copy-resource, clear and dispatch-uav carry the
//    invalidating writes, range-aware invalidation is inert and these counters say so;
//  - the vertex extents: requests, reads, failures, and the records hit only because their extent was not read yet;
//  - the resources written most.
#include <cstdint>
#include <cstdio>
#include "animated_history_writes.h"

namespace edvr {

struct FlatHistoryWindow {
    HistoryWriteStats writes;
    unsigned records = 0, bytes = 0, peakRecords = 0, peakBytes = 0;
    HistoryWriteTop top[3];
    unsigned topCount = 0;
};

inline const char* historyEraseName(unsigned i) {
    static const char* const names[static_cast<unsigned>(HistoryErase::Count)] = {
        "none", "advance-invalidated", "pressure-invalidated", "pressure-spent", "advance-aged"};
    return i < static_cast<unsigned>(HistoryErase::Count) ? names[i] : "unknown";
}

inline int flatHistoryLine(char* out, size_t n, const FlatHistoryWindow& w) {
    const HistoryWriteStats& s = w.writes;
    char erased[256];
    size_t at = 0;
    erased[0] = 0;
    for (unsigned i = 1; i < static_cast<unsigned>(HistoryErase::Count); ++i) {
        const int k = std::snprintf(erased + at, sizeof(erased) - at, " %s=%llu", historyEraseName(i), static_cast<unsigned long long>(s.erased[i]));
        if (k < 0 || at + static_cast<size_t>(k) >= sizeof(erased)) break;
        at += static_cast<size_t>(k);
    }
    char entries[2048];
    at = 0;
    entries[0] = 0;
    for (unsigned e = 0; e < kHistoryWriteEntries; ++e) {
        // touching and invalidating: vertices gap/window, indices gap/window
        const int k = std::snprintf(entries + at, sizeof(entries) - at,
            " %s=%llu touching=%llu/%llu/%llu/%llu invalidating=%llu/%llu/%llu/%llu ranged=%llu saved-writes=%llu records-invalidated=%llu;",
            historyWriteEntryName(e), static_cast<unsigned long long>(s.observed[e]),
            static_cast<unsigned long long>(s.touching[e][0][0]), static_cast<unsigned long long>(s.touching[e][0][1]),
            static_cast<unsigned long long>(s.touching[e][1][0]), static_cast<unsigned long long>(s.touching[e][1][1]),
            static_cast<unsigned long long>(s.invalidating[e][0][0]), static_cast<unsigned long long>(s.invalidating[e][0][1]),
            static_cast<unsigned long long>(s.invalidating[e][1][0]), static_cast<unsigned long long>(s.invalidating[e][1][1]),
            static_cast<unsigned long long>(s.ranged[e]), static_cast<unsigned long long>(s.savedWrites[e]),
            static_cast<unsigned long long>(s.recordsInvalidated[e]));
        if (k < 0 || at + static_cast<size_t>(k) >= sizeof(entries)) break;
        at += static_cast<size_t>(k);
    }
    char top[512];
    at = 0;
    top[0] = 0;
    for (unsigned i = 0; i < w.topCount && i < 3; ++i) {
        const int k = std::snprintf(top + at, sizeof(top) - at, " %p=%llu/%llu/%llu", w.top[i].resource,
            static_cast<unsigned long long>(w.top[i].touching), static_cast<unsigned long long>(w.top[i].invalidating),
            static_cast<unsigned long long>(w.top[i].saved));
        if (k < 0 || at + static_cast<size_t>(k) >= sizeof(top)) break;
        at += static_cast<size_t>(k);
    }
    if (!at) std::snprintf(top, sizeof(top), " none");
    return std::snprintf(out, n,
        "flat foreground history 5s: records=%u bytes=%u peak-records=%u peak-bytes=%u allocations=%llu allocation-failures=%llu; erased:%s; "
        "writes (observed, touching a live record's buffer as vertices-gap/vertices-window/indices-gap/indices-window, invalidating the same way, "
        "carrying a range, ranged and invalidating nothing, records invalidated):%s unknown-invalidating=%llu/%llu (gap/window); "
        "spared-records=%llu (live records of a written buffer that a range left alone); extent-unknown-hits=%llu; vertex extents: issued=%llu "
        "read=%llu failed=%llu; top resources written (touching/invalidating/saved):%s; the gap is every write before the frame's first capture or after "
        "its H, the window is between them (a hit there fails the frame); a write is counted once, by the API-level report; map covers Unmap, "
        "map/copy-resource/clear/dispatch-uav/other/unknown are whole-resource writes and cannot be saved by a range; a record found invalidated is "
        "named by the invalidated-* patterns of the no-candidate line, one found gone by reclaimed-by-advance or reclaimed-by-pressure",
        w.records, w.bytes, w.peakRecords, w.peakBytes, static_cast<unsigned long long>(s.allocations),
        static_cast<unsigned long long>(s.allocationFailures), erased, entries,
        static_cast<unsigned long long>(s.unknownInvalidating[0]), static_cast<unsigned long long>(s.unknownInvalidating[1]),
        static_cast<unsigned long long>(s.sparedRecords), static_cast<unsigned long long>(s.extentUnknownHits),
        static_cast<unsigned long long>(s.extentIssued), static_cast<unsigned long long>(s.extentRead),
        static_cast<unsigned long long>(s.extentFailed), top);
}

}  // namespace edvr
