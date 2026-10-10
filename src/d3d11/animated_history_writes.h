#pragma once
// WHICH WRITES COST THE HISTORY ITS PRIORS, AND WHAT RANGE-AWARE INVALIDATION SPARED (design doc section 104, the 10-07 settlement walk).
//
// The flat adapter's history (AnimatedVertexHistory) keeps, for each draw's exact geometry key, the previous frame's original-VS output. A
// record is a prior only while nothing wrote the vertex or index bytes it was made from: a write to the buffer invalidated EVERY record that
// reads the buffer. The 17:18-17:19 settlement walk lost 94 to 100 percent of its misses to that (record-reclaimed, thousands a window, every
// example key on one of two index buffers) because Elite keeps many meshes in a few arena buffers and writes some of them while the player walks.
//
// A write that carries a byte range (UpdateSubresource's box, CopySubresourceRegion's destination) invalidates only the records whose index
// range overlaps it exactly, or whose vertex extent does. This header is the part that needs no device: the entry and timing vocabulary, the
// counters a window of writes is read from, and the overlap test. Counters are cumulative; the runtime prints the difference from the last line.
#include <cstdint>
#include <memory>
#include <vector>
#include "animated_history_ledger.h"

namespace edvr {

// How the write reached the hooks. Update and CopyRegion carry a range when the call has one; Map (and its Unmap), CopyResource, Clear, a compute
// dispatch's UAV and the rest are the whole resource; Unknown is a write nothing could name (a nullptr resource: every record falls with it).
enum class HistoryWriteEntry : uint8_t { Map = 0, Update, CopyRegion, CopyResource, Clear, Dispatch, Other, Unknown, Count };
inline constexpr unsigned kHistoryWriteEntries = static_cast<unsigned>(HistoryWriteEntry::Count);
inline const char* historyWriteEntryName(unsigned i) {
    static const char* const names[kHistoryWriteEntries] = {"map", "update", "copy-region", "copy-resource", "clear", "dispatch-uav", "other", "unknown"};
    return i < kHistoryWriteEntries ? names[i] : "unknown";
}
// Which buffer of a record the write hit.
enum class HistoryWriteRole : uint8_t { Vertices = 0, Indices, Count };
inline constexpr unsigned kHistoryWriteRoles = static_cast<unsigned>(HistoryWriteRole::Count);
// When, against the frame's capture window: the gap (before the frame's first capture, or after the frame's H: whatever the draws of the next
// frame find gone) or the window (between the first capture and H, where a write to a captured draw's geometry fails the frame).
enum class HistoryWriteTiming : uint8_t { Gap = 0, Window, Count };
inline constexpr unsigned kHistoryWriteTimings = static_cast<unsigned>(HistoryWriteTiming::Count);
inline const char* historyWriteTimingName(unsigned i) { return i == 0 ? "gap" : i == 1 ? "window" : "unknown"; }

// The bytes a write touched. end == ~0 is the whole resource (the call carried no range, or the resource is not one a range means anything for).
struct HistoryWriteExtent {
    HistoryWriteEntry entry = HistoryWriteEntry::Other;
    uint64_t first = 0, end = ~uint64_t(0);
    bool ranged() const { return end != ~uint64_t(0); }
};
inline HistoryWriteExtent historyWholeWrite(HistoryWriteEntry entry) { HistoryWriteExtent e; e.entry = entry; return e; }
inline HistoryWriteExtent historyRangedWrite(HistoryWriteEntry entry, uint64_t first, uint64_t end) {
    HistoryWriteExtent e; e.entry = entry; e.first = first; e.end = end; return e;
}

// Half-open byte ranges [a0,a1) and [b0,b1) share a byte. An empty range overlaps nothing.
inline bool historyRangesOverlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    return a0 < a1 && b0 < b1 && a0 < b1 && b0 < a1;
}

// THE VERTICES A DRAW'S INDICES NAME, EXACTLY (design doc section 104, the third and fourth builds). The envelope of an index range (its smallest and
// largest index) is not what the draw reads: indices can leave gaps, and a write into a gap changes nothing the draw uses. The set is held as sorted,
// disjoint runs of index values [first, last] (a mesh's vertices are mostly contiguous, so a few runs); if the runs would be many, or the span huge,
// the set is the envelope alone (exact false), which is a superset and so still sound. It depends on the index bytes only: not on base, offset or
// stride, which the key carries, so it survives the record, a write to the vertices, and a re-make of the same key; a write to the index range
// ends it.
struct HistoryVertexSet {
    std::vector<uint32_t> runs;   // first, last, first, last, ...
    uint32_t low = 0, high = 0;
    bool exact = true;
};
// Where a write of [first,end) bytes meets the vertices a draw reads from a vertex buffer: vertex i occupies [vb0 + i*stride, vb0 + (i+1)*stride)
// with vb0 = offset + base*stride. `inSpan` is set when the write meets the envelope; the return is true when it meets a vertex of the set.
inline bool historyVertexSetMeets(const HistoryVertexSet& set, int64_t vb0, uint64_t stride, uint64_t first, uint64_t end, bool& inSpan) {
    inSpan = false;
    if (!stride || first >= end || set.runs.empty()) return false;
    const auto floorDiv = [](int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; };
    const int64_t s = static_cast<int64_t>(stride);
    const int64_t lo = floorDiv(static_cast<int64_t>(first) - vb0, s);
    const int64_t hi = floorDiv(static_cast<int64_t>(end) - 1 - vb0, s);
    if (hi < static_cast<int64_t>(set.low) || lo > static_cast<int64_t>(set.high)) return false;
    inSpan = true;
    // the first run whose last index is at or after lo
    size_t a = 0, b = set.runs.size() / 2;
    while (a < b) {
        const size_t m = (a + b) / 2;
        if (static_cast<int64_t>(set.runs[m * 2 + 1]) < lo) a = m + 1; else b = m;
    }
    return a < set.runs.size() / 2 && static_cast<int64_t>(set.runs[a * 2]) <= hi;
}

// Where records leave the history. A record published the frame before can only be erased after a write invalidated it (its stamps are reset
// then), so the two erase paths below are the only ones that make the ledger say a published key's record is gone.
enum class HistoryErase : uint8_t {
    None = 0,
    AdvanceInvalidated,   // aged out at the frame boundary after a write invalidated it and its key did not draw again in time
    PressureInvalidated,  // taken for a different key's budget while invalidated
    PressureSpent,        // taken for a different key's budget with every stamp two frames old or more
    AdvanceAged,          // every stamp older than two frames, or never published, at the frame boundary
    Count
};

inline constexpr unsigned kHistoryTopResources = 8;
struct HistoryWriteTop {
    const void* resource = nullptr;
    uint64_t touching = 0, invalidating = 0, saved = 0;
};

// Cumulative counters. `observed` is every write notification the history saw. `touching` is the writes on a resource some live record reads
// (by the role it reads it in; a buffer a record reads as both its vertices and its indices counts as its vertices), `invalidating` the part of them that invalidated a live record, and
// `saved` the ranged writes that invalidated none: the whole-resource rule would have wiped every record they touched, so `savedWrites` and
// `sparedRecords` are what range-aware invalidation kept. `extentUnknownHits` counts records hit by a vertex-buffer range only because their
// vertex extent was not read back yet (the cost of the extent's lag).
struct HistoryWriteStats {
    uint64_t observed[kHistoryWriteEntries] = {};
    uint64_t touching[kHistoryWriteEntries][kHistoryWriteRoles][kHistoryWriteTimings] = {};
    uint64_t invalidating[kHistoryWriteEntries][kHistoryWriteRoles][kHistoryWriteTimings] = {};
    uint64_t unknownInvalidating[kHistoryWriteTimings] = {};
    uint64_t recordsInvalidated[kHistoryWriteEntries] = {};
    uint64_t ranged[kHistoryWriteEntries] = {};
    uint64_t savedWrites[kHistoryWriteEntries] = {};
    uint64_t sparedRecords = 0;
    uint64_t erased[static_cast<unsigned>(HistoryErase::Count)] = {};
    uint64_t allocations = 0, allocationFailures = 0;
    // The vertex sets (sets of the vertices a draw reads, read back from a staging copy of its indices and cached by index range): requests made,
    // sets read, readbacks that failed or were cancelled by a write to the indices, sets that came from the cache for a record made or re-made,
    // requests that found the key's read already pending, and sets held as an envelope alone.
    uint64_t extentIssued = 0, extentRead = 0, extentFailed = 0, setFromCache = 0, setPending = 0, setApproximate = 0, setCancelled = 0;
    // A ranged write that met a live record's vertex buffer, by what it met: the record's set was not known (spared; checked again when the set is
    // read), the write fell inside the envelope but between the vertices (spared), outside the envelope (spared), or on a vertex (a genuine
    // rewrite: invalidated). deferred*: records invalidated when their set was read, because a write spared while it was unknown met it.
    uint64_t vertexUnknown = 0, vertexInGap = 0, vertexOutside = 0, vertexGenuine = 0, deferredInvalidated = 0, deferredConservative = 0;
    void add(const HistoryWriteStats& o) {
        for (unsigned e = 0; e < kHistoryWriteEntries; ++e) {
            observed[e] += o.observed[e]; recordsInvalidated[e] += o.recordsInvalidated[e];
            ranged[e] += o.ranged[e]; savedWrites[e] += o.savedWrites[e];
            for (unsigned r = 0; r < kHistoryWriteRoles; ++r)
                for (unsigned t = 0; t < kHistoryWriteTimings; ++t) {
                    touching[e][r][t] += o.touching[e][r][t]; invalidating[e][r][t] += o.invalidating[e][r][t];
                }
        }
        for (unsigned t = 0; t < kHistoryWriteTimings; ++t) unknownInvalidating[t] += o.unknownInvalidating[t];
        sparedRecords += o.sparedRecords;
        for (unsigned i = 0; i < static_cast<unsigned>(HistoryErase::Count); ++i) erased[i] += o.erased[i];
        allocations += o.allocations; allocationFailures += o.allocationFailures;
        extentIssued += o.extentIssued; extentRead += o.extentRead; extentFailed += o.extentFailed;
        setFromCache += o.setFromCache; setPending += o.setPending; setApproximate += o.setApproximate; setCancelled += o.setCancelled;
        vertexUnknown += o.vertexUnknown; vertexInGap += o.vertexInGap; vertexOutside += o.vertexOutside; vertexGenuine += o.vertexGenuine;
        deferredInvalidated += o.deferredInvalidated; deferredConservative += o.deferredConservative;
    }
};

// One ranged write that met a live record's vertex buffer, kept as an example (a few a window): what the write was, what the record's set was, and
// where in it the write fell.
enum class HistoryWriteCase : uint8_t { Unknown = 0, InGap, Genuine, Count };
inline constexpr unsigned kHistoryWriteCases = static_cast<unsigned>(HistoryWriteCase::Count);
inline const char* historyWriteCaseName(unsigned i) {
    static const char* const names[kHistoryWriteCases] = {"extent-unknown", "in-gap", "genuine"};
    return i < kHistoryWriteCases ? names[i] : "unknown";
}
struct HistoryWriteExample {
    HistoryWriteCase kind = HistoryWriteCase::Unknown;
    HistoryWriteEntry entry = HistoryWriteEntry::Other;
    const void* resource = nullptr;
    uint64_t first = 0, end = 0;              // the write's bytes
    bool known = false;                       // the record's set was known
    uint64_t spanFirst = 0, spanEnd = 0;      // the envelope of its vertices, in bytes of the vertex buffer (known only)
    bool exact = false;                       // the set is exact, not an envelope alone
    unsigned runs = 0;                        // its runs
    unsigned age = 0;                         // frames since the record was made
    HistoryKey key;
};
inline constexpr unsigned kHistoryExamplesPerCase = 2;

}  // namespace edvr
