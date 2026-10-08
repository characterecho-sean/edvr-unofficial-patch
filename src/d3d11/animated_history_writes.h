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
    uint64_t sparedRecords = 0, extentUnknownHits = 0;
    uint64_t erased[static_cast<unsigned>(HistoryErase::Count)] = {};
    uint64_t allocations = 0, allocationFailures = 0;
    uint64_t extentIssued = 0, extentRead = 0, extentFailed = 0;
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
        sparedRecords += o.sparedRecords; extentUnknownHits += o.extentUnknownHits;
        for (unsigned i = 0; i < static_cast<unsigned>(HistoryErase::Count); ++i) erased[i] += o.erased[i];
        allocations += o.allocations; allocationFailures += o.allocationFailures;
        extentIssued += o.extentIssued; extentRead += o.extentRead; extentFailed += o.extentFailed;
    }
};

}  // namespace edvr
