// visual.desktop_window, the census of the draws it skips (docs\desktop-window.md, "The skipped draws"). The first flight skipped far more of Elite's draws per Present than
// a mirror copy would be, and the count climbed window after window (about 365, 527, 755, 961, 1205 a Present). This says what they are: each skipped draw is counted by its
// vertex and pixel shader hashes in a fixed table of 16 pairs, by the kind of draw call, and by whether a depth-stencil view was bound; one in 32 is also checked against the
// context itself (is its render target 0 really the swap chain's buffer, or is the binding shadow's answer stale?). Every 10 s one line says the window's top six pairs and
// the totals, and the table starts again.
//
// No allocation and no lock: the table is plain memory, written by the draw hooks and read and reset at Present. Both are on the render thread (the draw hooks skip only on
// Elite's immediate context, and Elite's immediate context is used by one thread; the Present hook is that thread's), so there is nothing to race. Pure: no device, no window.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

class DesktopWindowCensus {
public:
    static constexpr unsigned kEntries = 16;   // pairs the table holds
    static constexpr unsigned kTop = 6;        // pairs the line names
    enum Kind : unsigned { Draw = 0, Indexed, Instanced, IndexedInstanced, Indirect, Auto, KindCount };

    // The draw hooks' kind letters (vscreen.cpp): D draw, I indexed, N instanced, X indexed instanced, Y and Z the two indirect forms, A auto. Anything else counts as a draw.
    static Kind kindOf(char letter) {
        switch (letter) {
            case 'I': return Indexed;
            case 'N': return Instanced;
            case 'X': return IndexedInstanced;
            case 'Y': case 'Z': return Indirect;
            case 'A': return Auto;
            default: return Draw;
        }
    }

    // One skipped draw.
    void note(uint64_t vs, uint64_t ps, char kindLetter, bool depthBound) {
        ++total_;
        ++kinds_[kindOf(kindLetter)];
        if (depthBound) ++withDepth_;
        for (unsigned i = 0; i < used_; ++i) {
            if (entry_[i].vs == vs && entry_[i].ps == ps) { ++entry_[i].count; return; }
        }
        if (used_ < kEntries) {
            entry_[used_].vs = vs; entry_[used_].ps = ps; entry_[used_].count = 1;
            ++used_;
        } else {
            ++unplaced_;   // a seventeenth pair: counted in the total and in "other", and the line says the table was full
        }
    }
    // One skipped draw whose render target 0 was read from the context: was it the swap chain's buffer?
    void sample(bool onBuffer) {
        ++sampled_;
        if (onBuffer) ++sampledOnBuffer_;
    }
    void reset() { *this = DesktopWindowCensus(); }
    uint32_t total() const { return total_; }
    unsigned pairs() const { return used_; }
    uint32_t unplaced() const { return unplaced_; }
    uint32_t withDepth() const { return withDepth_; }
    uint32_t kindCount(Kind k) const { return kinds_[k]; }
    uint32_t sampled() const { return sampled_; }
    uint32_t sampledOnBuffer() const { return sampledOnBuffer_; }

    // The window's line. The six biggest pairs by count (the first seen wins a tie), "other" for every skipped draw they do not name (pairs below the six, and any the full
    // table could not place), "pairs" for the table's size (with a + when it was full), the depth count, the kinds, and the context checks.
    int format(char* out, size_t size) const {
        unsigned order[kEntries];
        for (unsigned i = 0; i < used_; ++i) order[i] = i;
        const unsigned top = used_ < kTop ? used_ : kTop;
        for (unsigned i = 1; i < used_; ++i) {   // a stable insertion sort by count, biggest first: the first seen keeps its place on a tie
            const unsigned key = order[i];
            unsigned j = i;
            while (j > 0 && entry_[order[j - 1]].count < entry_[key].count) { order[j] = order[j - 1]; --j; }
            order[j] = key;
        }
        char list[kTop * 48 + 16];
        list[0] = '\0';
        size_t len = 0;
        uint32_t named = 0;
        for (unsigned i = 0; i < top; ++i) {
            const Entry& e = entry_[order[i]];
            const int n = std::snprintf(list + len, sizeof list - len, "%s%016llX/%016llX x%u", i ? ", " : "", static_cast<unsigned long long>(e.vs),
                                        static_cast<unsigned long long>(e.ps), e.count);
            if (n < 0 || static_cast<size_t>(n) >= sizeof list - len) break;
            len += static_cast<size_t>(n);
            named += e.count;
        }
        if (!top) std::snprintf(list, sizeof list, "(none)");
        return std::snprintf(out, size,
                             "desktop window: skipped by shader (10 s): %s; other %u; pairs %u%s; with depth %u; kinds draw %u indexed %u instanced %u indexed_instanced %u "
                             "indirect %u auto %u; render target 0 checked on %u, the buffer on %u",
                             list, total_ - named, used_, unplaced_ ? "+" : "", withDepth_, kinds_[Draw], kinds_[Indexed], kinds_[Instanced], kinds_[IndexedInstanced],
                             kinds_[Indirect], kinds_[Auto], sampled_, sampledOnBuffer_);
    }

private:
    struct Entry { uint64_t vs = 0, ps = 0; uint32_t count = 0; };
    Entry entry_[kEntries] = {};
    unsigned used_ = 0;
    uint32_t total_ = 0, unplaced_ = 0, withDepth_ = 0, sampled_ = 0, sampledOnBuffer_ = 0;
    uint32_t kinds_[KindCount] = {};
};

}  // namespace edvr
