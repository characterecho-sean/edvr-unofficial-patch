#pragma once
// The lines that name the writes that cost the first-person history its priors, and what range-aware invalidation kept (design doc section 104,
// the 10-07 settlement walk and its two flights). Plain text from plain counters: tools\flat_temporal_test builds the lines from known counts and
// holds their keys to the names the counters carry, so a log reader and the code that writes the line cannot drift apart.
//
// THE LOGGER CUTS A LINE AT ABOUT 1167 CHARACTERS ("...[truncated]"; src/common/log.cpp). The first version of this report was one line of 3 KB and
// lost its second half on the flight. Every line here is built to stay under kFlatLogLineBudget with 12-digit counters, and the test holds each
// formatter to it; a long explanation does not belong in a line (the design doc has it).
//
// `flat foreground history 5s:`           the history's size and peaks, the allocations, the erase paths, the vertex sets' reads;
// `flat foreground history writes 5s [..]:` the write notifications by entry (three lines): observed, touching a live record's buffer, invalidating,
//                                          ranged, ranged and invalidating nothing (`saved-writes`: what the range kept), records invalidated;
// `flat foreground history vertex writes 5s:` the ranged writes that met a live record's vertex buffer, by what they met, the deferred checks, the
//                                          resources written most;
// `flat foreground history write example:` a few such writes with the record's set and where in it they fell.
// Every counter is printed, zeros included: a line's absence means the code did not run.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "animated_history_writes.h"

namespace edvr {

inline constexpr size_t kFlatLogLineBudget = 1000;

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
    return std::snprintf(out, n,
        "flat foreground history 5s: records=%u bytes=%u peak-records=%u peak-bytes=%u allocations=%llu allocation-failures=%llu; erased:%s; "
        "vertex sets: requested=%llu read=%llu failed=%llu cancelled=%llu from-cache=%llu already-pending=%llu approximate=%llu",
        w.records, w.bytes, w.peakRecords, w.peakBytes, static_cast<unsigned long long>(s.allocations),
        static_cast<unsigned long long>(s.allocationFailures), erased,
        static_cast<unsigned long long>(s.extentIssued), static_cast<unsigned long long>(s.extentRead),
        static_cast<unsigned long long>(s.extentFailed), static_cast<unsigned long long>(s.setCancelled),
        static_cast<unsigned long long>(s.setFromCache), static_cast<unsigned long long>(s.setPending),
        static_cast<unsigned long long>(s.setApproximate));
}

// The write entries of one line: [firstEntry, lastEntry). The legend is in the line: touching and invalidating read vertices-gap / vertices-window /
// indices-gap / indices-window (the gap is every write before the frame's first capture or after its H, the window is between them).
inline constexpr unsigned kFlatHistoryWriteLines = 3;
inline int flatHistoryWritesLine(char* out, size_t n, const FlatHistoryWindow& w, unsigned part) {
    static const unsigned firsts[kFlatHistoryWriteLines + 1] = {0, 3, 6, kHistoryWriteEntries};
    const HistoryWriteStats& s = w.writes;
    if (part >= kFlatHistoryWriteLines) return 0;
    char entries[960];
    size_t at = 0;
    entries[0] = 0;
    for (unsigned e = firsts[part]; e < firsts[part + 1]; ++e) {
        const int k = std::snprintf(entries + at, sizeof(entries) - at,
            " %s: observed=%llu touching(v-gap/v-window/i-gap/i-window)=%llu/%llu/%llu/%llu invalidating=%llu/%llu/%llu/%llu ranged=%llu "
            "saved-writes=%llu records-invalidated=%llu;",
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
    return std::snprintf(out, n, "flat foreground history writes 5s (%u/%u):%s", part + 1, kFlatHistoryWriteLines, entries);
}

inline int flatHistoryVertexLine(char* out, size_t n, const FlatHistoryWindow& w) {
    const HistoryWriteStats& s = w.writes;
    char top[320];
    size_t at = 0;
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
        "flat foreground history vertex writes 5s: unknown-invalidating=%llu/%llu (gap/window) spared-records=%llu; ranged writes meeting a live "
        "record's vertex buffer: extent-unknown=%llu in-gap=%llu outside-span=%llu genuine=%llu; deferred-invalidated=%llu "
        "deferred-conservative=%llu; top resources written (touching/invalidating/saved):%s",
        static_cast<unsigned long long>(s.unknownInvalidating[0]), static_cast<unsigned long long>(s.unknownInvalidating[1]),
        static_cast<unsigned long long>(s.sparedRecords), static_cast<unsigned long long>(s.vertexUnknown),
        static_cast<unsigned long long>(s.vertexInGap), static_cast<unsigned long long>(s.vertexOutside),
        static_cast<unsigned long long>(s.vertexGenuine), static_cast<unsigned long long>(s.deferredInvalidated),
        static_cast<unsigned long long>(s.deferredConservative), top);
}

// One example write: the case, the write, the record's set and where in it the write fell, the record's age and key.
inline int flatHistoryExampleLine(char* out, size_t n, const HistoryWriteExample& x) {
    char where[160];
    if (x.known && x.spanEnd > x.spanFirst) {
        const long long inside = static_cast<long long>(x.first) - static_cast<long long>(x.spanFirst);
        const double percent = 100.0 * static_cast<double>(inside) / static_cast<double>(x.spanEnd - x.spanFirst);
        std::snprintf(where, sizeof(where), "set=[%llu,%llu) %s runs=%u write-starts=%lld bytes into it (%.1f%% of the span)",
            static_cast<unsigned long long>(x.spanFirst), static_cast<unsigned long long>(x.spanEnd), x.exact ? "exact" : "envelope-only",
            x.runs, inside, percent);
    } else {
        std::snprintf(where, sizeof(where), "set=unknown");
    }
    return std::snprintf(out, n,
        "flat foreground history write example: case=%s entry=%s resource=%p write=[%llu,%llu) %s record-age=%u frames key: shader=%p layout=%p "
        "vertices=%p indices=%p count=%u start=%u base=%d vb-offset=%u stride=%u ib-offset=%u format=%u",
        historyWriteCaseName(static_cast<unsigned>(x.kind)), historyWriteEntryName(static_cast<unsigned>(x.entry)), x.resource,
        static_cast<unsigned long long>(x.first), static_cast<unsigned long long>(x.end), where, x.age, x.key.vs, x.key.layout, x.key.vertices,
        x.key.indices, x.key.count, x.key.start, x.key.base, x.key.offset, x.key.stride, x.key.indexOffset, x.key.format);
}

}  // namespace edvr
