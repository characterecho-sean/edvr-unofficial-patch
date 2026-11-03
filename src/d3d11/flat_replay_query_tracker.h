#pragma once
#include <cstdint>

namespace edvr {
// A pure, bounded model of query brackets on the immediate context. An
// unpaired count-bearing End or a full table leaves replay unsafe until the
// context is replaced; neither Present nor ClearState ends a GPU query.
struct FlatReplayQueryTracker {
    enum class Kind : uint8_t { EndOnly, Timing, Count, Unknown };
    // Why `uncertain` first became true (log only, 2026-11-03; the decision never reads it). The values are the numbers
    // flat_replay_query_reject.h names, so keep the two in step.
    enum class Cause : uint8_t { None = 0, NullIdentity, DuplicateBegin, TableFull, KindMismatch, UnmatchedEnd };
    struct Entry { const void* identity = nullptr; Kind kind = Kind::Unknown; };
    Entry active[64]{};
    bool uncertain = false;
    // The first escape: its cause, the kind and identity of the call that caused it, and what the tracker had seen by then (that call
    // included). Never cleared except by resetForNewContext.
    Cause firstCause = Cause::None;
    Kind firstCauseKind = Kind::Unknown;
    const void* firstCauseIdentity = nullptr;
    uint64_t begins = 0, ends = 0;
    uint64_t causeBegins = 0, causeEnds = 0;
    uint32_t causeOpen = 0;
    // Ends of Timing queries with no Begin seen (2026-11-03): counted, never an escape. The observer runs only in the flat profile, so after a live VR-to-flat
    // switch its first call can be the End of a timestamp-disjoint bracket the game opened in VR (flight edvr_gfx_20261103_061437: begins=0 ends=1, 0 ms in), and
    // that one End set the sticky flag and refused every on-foot DLSS frame. safe() already ignores an open Timing bracket (it counts no draws), so an unobserved
    // one cannot make a replay unsafe either.
    uint64_t unmatchedTimingEnds = 0;

    uint32_t openCount() const {
        uint32_t n = 0;
        for (const auto& entry : active) if (entry.identity) ++n;
        return n;
    }
    void begin(const void* identity, Kind kind) {
        ++begins;
        if (!identity) { escape(Cause::NullIdentity, kind, identity); return; }
        for (auto& entry : active) if (entry.identity == identity) {
            escape(Cause::DuplicateBegin, kind, identity); return; // duplicate Begin has ambiguous semantics
        }
        for (auto& entry : active) if (!entry.identity) {
            entry = {identity, kind}; return;
        }
        escape(Cause::TableFull, kind, identity);
    }
    void end(const void* identity, Kind kind) {
        ++ends;
        if (!identity) { escape(Cause::NullIdentity, kind, identity); return; }
        for (auto& entry : active) if (entry.identity == identity) {
            if (entry.kind != kind && entry.kind != Kind::Unknown)
                escape(Cause::KindMismatch, kind, identity);
            entry = {}; return;
        }
        // EVENT and TIMESTAMP are End-only D3D11 queries, and a Timing bracket counts nothing: neither one escapes. Any other
        // unmatched End means a count-bearing (or unknown) bracket escaped observation.
        if (kind == Kind::EndOnly) return;
        if (kind == Kind::Timing) { ++unmatchedTimingEnds; return; }
        escape(Cause::UnmatchedEnd, kind, identity);
    }
    bool safe() const {
        if (uncertain) return false;
        for (const auto& entry : active)
            if (entry.identity && entry.kind != Kind::Timing && entry.kind != Kind::EndOnly) return false;
        return true;
    }
    void resetForNewContext() { *this = {}; }

private:
    void escape(Cause cause, Kind kind, const void* identity) {
        if (!uncertain) {
            firstCause = cause; firstCauseKind = kind; firstCauseIdentity = identity;
            causeBegins = begins; causeEnds = ends; causeOpen = openCount();
        }
        uncertain = true;
    }
};
} // namespace edvr
