// The census of unkeyed pairs (flat, 2026-09-29): which (vertex shader, pixel
// shader) pairs of a KNOWN pool family bind without a keyed pixel shader, and how
// often, per 5 s window. The Krait's main-menu capture had 22.5% of the frame
// rejected because its hull plating's pixel shader was in no family list; the
// engine motion block's own suffix names one hash per family per 30 s, which a
// 25 s visit to the menu never reaches. This names every pair (four at most, the
// rest counted) on the flat 5 s block, so a ship sweep needs no F10 per ship.
//
// A bind event is a visit to the draw path's slow half (the game changed a
// binding the cache watches), not a draw: consecutive draws with identical
// bindings are one. Pure and allocation-free: the draw path writes it under the
// engine mutex, the flat 5 s block reads and clears it under the same mutex.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace engine_velocity_unkeyed {

constexpr size_t kCapacity = 4;   // pairs named per window; the rest are counted, not named

struct Entry { uint64_t vs = 0, ps = 0, binds = 0; };

class Table {
public:
    // One bind event of an unkeyed pair.
    void note(uint64_t vs, uint64_t ps) {
        ++total_;
        for (size_t i = 0; i < used_; ++i)
            if (e_[i].vs == vs && e_[i].ps == ps) { ++e_[i].binds; return; }
        if (used_ < kCapacity) { e_[used_].vs = vs; e_[used_].ps = ps; e_[used_].binds = 1; ++used_; return; }
        ++overflow_;
    }
    void reset() { *this = Table{}; }
    size_t used() const { return used_; }
    uint64_t total() const { return total_; }
    uint64_t overflow() const { return overflow_; }
    uint64_t bindsOf(uint64_t vs, uint64_t ps) const {
        for (size_t i = 0; i < used_; ++i) if (e_[i].vs == vs && e_[i].ps == ps) return e_[i].binds;
        return 0;
    }

    // The line, built here so the rig prints exactly what the DLL writes. Present
    // even when empty (pairs=[]): a missing line is what "never ran" looks like.
    // live=0 says the engine-record velocity was not live this window.
    int format(char* out, size_t size, bool live) const {
        Entry sorted[kCapacity];
        for (size_t i = 0; i < used_; ++i) sorted[i] = e_[i];
        for (size_t i = 1; i < used_; ++i)   // busiest first; insertion sort over four
            for (size_t j = i; j > 0 && sorted[j].binds > sorted[j - 1].binds; --j) {
                const Entry t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t;
            }
        int n = std::snprintf(out, size, "flat engine motion unkeyed 5s: live=%u binds=%llu distinct=%u overflow=%llu pairs=[",
                              live ? 1u : 0u, (unsigned long long)total_, (unsigned)used_, (unsigned long long)overflow_);
        if (n < 0 || static_cast<size_t>(n) >= size) return n;
        for (size_t i = 0; i < used_; ++i) {
            const int m = std::snprintf(out + n, size - static_cast<size_t>(n), "%svs_%016llX/ps_%016llX:%llu",
                                        i ? " " : "", (unsigned long long)sorted[i].vs,
                                        (unsigned long long)sorted[i].ps, (unsigned long long)sorted[i].binds);
            if (m < 0 || static_cast<size_t>(m) >= size - static_cast<size_t>(n)) return n + (m < 0 ? 0 : m);
            n += m;
        }
        const int m = std::snprintf(out + n, size - static_cast<size_t>(n), "]");
        return m < 0 ? n : n + m;
    }
private:
    Entry e_[kCapacity] = {};
    size_t used_ = 0;
    uint64_t total_ = 0, overflow_ = 0;
};

} // namespace engine_velocity_unkeyed
} // namespace edvr
