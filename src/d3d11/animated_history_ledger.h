#pragma once
// WHY A DRAW HAD NO SAME-KEY PRIOR (design doc section 104, the pistol's no-candidate bursts).
//
// The flat adapter's history (AnimatedVertexHistory) hands a draw the records of its exact geometry key that the frame before used. The
// 2026-10-07 pistol flight (11:54:26..36) counted draws with none (no-candidate: 13 to 18 percent of the captures in the worst windows) and
// nothing in the log said which part of the key had changed, or whether last frame's record had simply not been kept. This ledger is the
// history's memory of what it was asked, and classify() names, for one such draw, the nearest thing the frame before did hold.
//
// Nothing here touches a resource. Keys are raw pointers compared for identity only (never dereferenced, never a reference: a ledger that
// held a COM reference would keep a released buffer alive), and the ledger is a ring, so the facts it holds are the last few frames'.
// The classifier is a pure function of the ledger and of the facts about the history's records that only the history can read
// (HistoryRecordFacts), so tools\flat_temporal_test runs it with no device.
#include <cstdint>
#include <cstring>
#include <vector>

namespace edvr {

// One capture's geometry key as the history sees it.
struct HistoryKey {
    const void* vs = nullptr, * layout = nullptr, * vertices = nullptr, * indices = nullptr;
    unsigned count = 0, start = 0, offset = 0, stride = 0, indexOffset = 0, format = 0;
    int base = 0;
};

// One bit per key field that differs between two keys.
enum HistoryKeyDiff : unsigned {
    kDiffVs = 1, kDiffLayout = 2, kDiffVertices = 4, kDiffIndices = 8, kDiffCount = 16, kDiffStart = 32, kDiffBase = 64,
    kDiffOffset = 128, kDiffStride = 256, kDiffIndexOffset = 512, kDiffFormat = 1024
};
inline unsigned historyKeyDiff(const HistoryKey& a, const HistoryKey& b) {
    unsigned d = 0;
    if (a.vs != b.vs) d |= kDiffVs;
    if (a.layout != b.layout) d |= kDiffLayout;
    if (a.vertices != b.vertices) d |= kDiffVertices;
    if (a.indices != b.indices) d |= kDiffIndices;
    if (a.count != b.count) d |= kDiffCount;
    if (a.start != b.start) d |= kDiffStart;
    if (a.base != b.base) d |= kDiffBase;
    if (a.offset != b.offset) d |= kDiffOffset;
    if (a.stride != b.stride) d |= kDiffStride;
    if (a.indexOffset != b.indexOffset) d |= kDiffIndexOffset;
    if (a.format != b.format) d |= kDiffFormat;
    return d;
}
// The fields a re-pack moves: where the mesh starts in buffers that are otherwise the same.
inline constexpr unsigned kDiffPlacement = kDiffStart | kDiffBase | kDiffOffset | kDiffIndexOffset;
// The fields that make a prior the same mesh at another place: everything the prior's vertex stream depends on except the placement.
inline constexpr unsigned kDiffIdentityOfMesh = kDiffVs | kDiffLayout | kDiffVertices | kDiffIndices | kDiffCount | kDiffStride | kDiffFormat;

// Why a draw found no record of its exact key that the frame before used. Mutually exclusive, in the order classify() tests them; the
// first two say the frame before gave the history nothing to remember, the next group that this key was drawn and its record did not
// survive, then the nearest other key, and last how long ago this key was seen. Names are the 5 s line's keys (kHistoryGapNames).
enum class HistoryGap : uint8_t {
    PreviousFrameEmpty,        // nothing was offered to the history in the frame before: a skipped frame, or the weapon was not drawn
    PreviousFrameNotCaptured,  // the frame before offered draws, but every one was refused before it had a key
    RefusedLastFrame,          // this key was drawn the frame before and refused: the occurrence cap
    RefusedBudget,             // ... the history budget
    RefusedOther,              // ... any other refusal after the key was built
    UnpublishedLastFrame,      // captured the frame before, but the record was never published (the capture did not complete)
    InvalidatedUnknown,        // the record was invalidated by a write nothing could name (every record falls with it)
    InvalidatedVertices,       // ... by a write to its vertex buffer
    InvalidatedIndices,        // ... by a write to its index buffer
    RecordReclaimed,           // published the frame before, and gone: reclaimed under budget pressure
    RecordUnusable,            // published, present, not invalidated, and still not a prior (must be zero)
    OffsetShift,               // the same mesh was drawn the frame before at another place in the same buffers, one record: matched
    OffsetShiftAmbiguous,      // ... but several records, or the record is already another draw's: refused
    OffsetShiftUnusable,       // ... but its record is gone or invalidated: refused
    CountChange,               // the same buffers were drawn the frame before with another vertex count (a mesh or LOD swap)
    BufferChange,              // the same vertex shader was drawn the frame before from other buffers
    FormatChange,              // the same buffers and count, another layout, stride or index format
    AbsentShort,               // this key was last drawn two to eight frames ago
    AbsentLong,                // ... longer ago, still in the ledger
    NewKey,                    // nothing like it in the ledger
    Count
};
inline constexpr unsigned kHistoryGapCount = static_cast<unsigned>(HistoryGap::Count);
inline const char* historyGapName(HistoryGap g) {
    static const char* const names[kHistoryGapCount] = {
        "previous-frame-empty", "previous-frame-not-captured", "refused-last-frame-cap", "refused-last-frame-budget",
        "refused-last-frame-other", "unpublished-last-frame", "invalidated-unknown", "invalidated-vertices", "invalidated-indices",
        "record-reclaimed", "record-unusable", "offset-shift", "offset-shift-ambiguous", "offset-shift-unusable", "count-change",
        "buffer-change", "format-change", "absent-short", "absent-long", "new-key"};
    const unsigned i = static_cast<unsigned>(g);
    return i < kHistoryGapCount ? names[i] : "unknown";
}

// What only the history can read about its own records for the draw being classified.
struct HistoryRecordFacts {
    bool exactPresent = false;          // a record of the exact key is in the history
    bool exactInvalidated = false;      // ... and was invalidated
    unsigned exactInvalidReasons = 0;   // ... by these writes (bit 0 unknown, bit 1 vertex buffer, bit 2 index buffer)
    unsigned shiftUsable = 0;           // records of the same mesh at another placement that the frame before used and nothing invalidated
    unsigned shiftClaimed = 0;          // of those, records another draw of this frame already used or claimed
};

struct HistoryClass {
    HistoryGap gap = HistoryGap::NewKey;
    unsigned diff = 0;                  // the key fields that differ from the nearest entry (kDiff*), when there is one
    unsigned age = 0;                   // frames since the key was last seen (AbsentShort, AbsentLong)
    bool hasNearest = false;            // `nearest` holds the entry the pattern was read from
    HistoryKey nearest;
};

class HistoryLedger {
public:
    static constexpr unsigned capacity = 512;
    enum Outcome : uint8_t { Captured = 0, RefusedOccurrence = 1, RefusedBudget = 2, RefusedOther = 3 };

    void clear() { *this = HistoryLedger{}; }
    // A capture that reached a key. The token names the entry for markPublished: the record exists and the next frame can use it.
    uint64_t note(unsigned frame, const HistoryKey& key, Outcome outcome) {
        if (entries_.empty()) entries_.resize(capacity);
        Entry& e = entries_[next_ % capacity];
        e = Entry{};
        e.seq = next_ + 1; e.frame = frame; e.key = key; e.outcome = outcome; e.keyed = true;
        return next_++ + 1;
    }
    // A draw offered to the history that never had a key: the adapter's preflight refused it, or the history did before the key was built.
    void noteUnkeyed(unsigned frame) {
        if (entries_.empty()) entries_.resize(capacity);
        Entry& e = entries_[next_ % capacity];
        e = Entry{};
        e.seq = next_ + 1; e.frame = frame; e.keyed = false;
        ++next_;
    }
    void markPublished(uint64_t token) {
        if (!token || entries_.empty()) return;
        Entry& e = entries_[(token - 1) % capacity];
        if (e.seq == token) e.published = true;
    }
    uint64_t entries() const { return next_; }

    // Names, for a draw of `key` at `frame` that found no record of that exact key the frame before used, the nearest thing the ledger and
    // the history's records say the frame before did hold. The tests run in the order of HistoryGap.
    HistoryClass classify(unsigned frame, const HistoryKey& key, const HistoryRecordFacts& facts) const {
        HistoryClass c;
        const unsigned previous = frame - 1;
        bool anyPrevious = false, keyedPrevious = false;
        unsigned exactCount = 0, exactPublished = 0; Outcome exactRefusal = Captured; bool exactRefused = false;
        const Entry* exactEntry = nullptr;
        const Entry* shiftEntry = nullptr, * countEntry = nullptr, * formatEntry = nullptr, * bufferEntry = nullptr, * olderExact = nullptr;
        unsigned olderAge = 0;
        const uint64_t first = next_ > capacity ? next_ - capacity : 0;
        for (uint64_t s = first; s < next_; ++s) {
            const Entry& e = entries_[s % capacity];
            if (e.seq != s + 1) continue;
            if (e.frame == previous) {
                anyPrevious = true;
                if (!e.keyed) continue;
                keyedPrevious = true;
                const unsigned d = historyKeyDiff(e.key, key);
                if (!d) {
                    ++exactCount;
                    if (!exactEntry) exactEntry = &e;
                    if (e.outcome != Captured && !exactRefused) { exactRefused = true; exactRefusal = e.outcome; exactEntry = &e; }
                    if (e.outcome == Captured && e.published) ++exactPublished;
                    continue;
                }
                // The relations below are read from drawn-and-published entries only: a refused draw left no record to be a prior.
                if (e.outcome != Captured || !e.published) continue;
                const bool sameShader = !(d & kDiffVs), sameBuffers = !(d & (kDiffVertices | kDiffIndices));
                if (sameShader && sameBuffers && !(d & (kDiffLayout | kDiffCount | kDiffStride | kDiffFormat))) { if (!shiftEntry) shiftEntry = &e; }
                else if (sameShader && sameBuffers && (d & kDiffCount)) { if (!countEntry) countEntry = &e; }
                else if (sameShader && sameBuffers && (d & (kDiffLayout | kDiffStride | kDiffFormat))) { if (!formatEntry) formatEntry = &e; }
                else if (sameShader) { if (!bufferEntry) bufferEntry = &e; }
            } else if (e.keyed && e.frame < previous && !historyKeyDiff(e.key, key)) {
                const unsigned age = frame - e.frame;
                if (!olderExact || age < olderAge) { olderExact = &e; olderAge = age; }
            }
        }
        auto at = [&](const Entry* e, HistoryGap gap) {
            c.gap = gap;
            if (e) { c.hasNearest = true; c.nearest = e->key; c.diff = historyKeyDiff(e->key, key); }
            return c;
        };
        if (!anyPrevious) return at(nullptr, HistoryGap::PreviousFrameEmpty);
        if (!keyedPrevious) return at(nullptr, HistoryGap::PreviousFrameNotCaptured);
        if (exactCount) {
            if (exactRefused)
                return at(exactEntry, exactRefusal == RefusedOccurrence ? HistoryGap::RefusedLastFrame :
                                       exactRefusal == RefusedBudget ? HistoryGap::RefusedBudget : HistoryGap::RefusedOther);
            if (!exactPublished) return at(exactEntry, HistoryGap::UnpublishedLastFrame);
            if (!facts.exactPresent) return at(exactEntry, HistoryGap::RecordReclaimed);
            if (facts.exactInvalidated)
                return at(exactEntry, (facts.exactInvalidReasons & 1u) ? HistoryGap::InvalidatedUnknown :
                                       (facts.exactInvalidReasons & 2u) ? HistoryGap::InvalidatedVertices :
                                       (facts.exactInvalidReasons & 4u) ? HistoryGap::InvalidatedIndices : HistoryGap::InvalidatedUnknown);
            return at(exactEntry, HistoryGap::RecordUnusable);
        }
        // The same mesh at another placement. The records decide whether it is usable (the ledger only says it was drawn).
        if (shiftEntry || facts.shiftUsable) {
            if (facts.shiftUsable == 1 && facts.shiftClaimed == 0) return at(shiftEntry, HistoryGap::OffsetShift);
            if (facts.shiftUsable == 0) return at(shiftEntry, HistoryGap::OffsetShiftUnusable);
            return at(shiftEntry, HistoryGap::OffsetShiftAmbiguous);
        }
        if (countEntry) return at(countEntry, HistoryGap::CountChange);
        if (formatEntry) return at(formatEntry, HistoryGap::FormatChange);
        if (bufferEntry) return at(bufferEntry, HistoryGap::BufferChange);
        if (olderExact) {
            c = at(olderExact, olderAge <= 8 ? HistoryGap::AbsentShort : HistoryGap::AbsentLong);
            c.age = olderAge;
            return c;
        }
        return at(nullptr, HistoryGap::NewKey);
    }

private:
    struct Entry {
        uint64_t seq = 0;
        unsigned frame = ~0u;
        HistoryKey key;
        Outcome outcome = Captured;
        bool keyed = false, published = false;
    };
    // Allocated at the first note, so a history that never notes (the default policy, VR) holds none of it.
    std::vector<Entry> entries_;
    uint64_t next_ = 0;
};

}  // namespace edvr
