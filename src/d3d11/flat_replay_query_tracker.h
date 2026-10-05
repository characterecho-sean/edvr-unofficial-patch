#pragma once
#include <cstdint>

namespace edvr {
// A pure, bounded model of query brackets on the immediate context. An
// unpaired count-bearing End or a full table leaves replay unsafe until the
// context is replaced; neither Present nor ClearState ends a GPU query.
struct FlatReplayQueryTracker {
    enum class Kind : uint8_t { EndOnly, Timing, Count, Unknown };
    struct Entry { const void* identity = nullptr; Kind kind = Kind::Unknown; };
    Entry active[64]{};
    bool uncertain = false;

    void begin(const void* identity, Kind kind) {
        if (!identity) { uncertain = true; return; }
        for (auto& entry : active) if (entry.identity == identity) {
            uncertain = true; return; // duplicate Begin has ambiguous semantics
        }
        for (auto& entry : active) if (!entry.identity) {
            entry = {identity, kind}; return;
        }
        uncertain = true;
    }
    void end(const void* identity, Kind kind) {
        if (!identity) { uncertain = true; return; }
        for (auto& entry : active) if (entry.identity == identity) {
            if (entry.kind != kind && entry.kind != Kind::Unknown)
                uncertain = true;
            entry = {}; return;
        }
        // EVENT and TIMESTAMP are End-only D3D11 queries. Any other
        // unmatched End means a bracket escaped observation.
        if (kind != Kind::EndOnly) uncertain = true;
    }
    bool safe() const {
        if (uncertain) return false;
        for (const auto& entry : active)
            if (entry.identity && entry.kind != Kind::Timing && entry.kind != Kind::EndOnly) return false;
        return true;
    }
    void resetForNewContext() { *this = {}; }
};
} // namespace edvr
