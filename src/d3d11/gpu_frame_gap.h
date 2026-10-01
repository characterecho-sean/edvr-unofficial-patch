#pragma once
// The gap between consecutive frames of the game device's GPU work, on the GPU's
// own clock (docs\openxr-performance-review-2026-09-14.md, the 2026-09-29 carrier
// entry). One question: at a busy scene, is the GPU saturated (the frame is GPU
// bound, the gap is near zero) or does it wait for the CPU (the gap is large)?
//
// WHAT IS MEASURED. Each Application-render span (gpu_span_state.h) brackets one
// frame's producer GPU work on the game's device. Its first timestamp is issued at
// the first producer command after the pose wait, its last at the end of the final
// segment, after the door work (the native treatment of both eyes). A device runs
// its commands in order, so frame N+1's first timestamp cannot precede frame N's
// last, and  gap = first(N+1) - last(N)  is the time on the GPU's timeline that no
// EDVR-timed span covers between the two frames.
//
// WHAT IT IS NOT. It is an upper bound on GPU idle, never idle itself. SteamVR's
// compositor runs in another process on the same GPU and takes its turn in that
// gap; so do the runtime's transfers, the mirror window's Present copy and
// anything else submitted outside the segments. A gap of about 1 ms is therefore
// not proof of idleness. A gap near zero is the strong statement: nothing waited.
//
// CLOCK RULES, the census's own. Only spans that validated (neither disjoint, every
// segment in order, gpu_span_state.h); both spans must report the same frequency;
// the gap must not be negative (two spans that overlap are not on one clock, or the
// results crossed). Pairs are made by
// consecutive sequence number, the pose wait's own count, so a frame with no valid
// span makes no pair on either side: it is reported as fewer pairs than frames,
// never bridged. Results complete out of order (an older slot can settle after a
// newer one), so the last few frames are kept until their neighbours arrive, and
// each pair is made once, by whichever of its two frames arrives second.
//
// A GAP OVER ONE SECOND IS A STALL, AND IS KEPT, NOT DISCARDED. It was thrown away as "not a
// statistic" (and counted among the rejected pairs), which is right for the percentiles and wrong
// for a freeze: in the issue 63 flight the 1858 ms freeze was exactly such a pair, and the line
// that says so was the only place the GPU's side of it could have been read. A stall is left out
// of p50, p95 and the max, which stay what they were, and is reported beside them: how many, the
// longest, and the largest few by the sequence of the frame that ended the gap, which is the
// runtime sequence the long-frame and long-cycle lines carry (docs/freeze-diagnostics-2026-10-01.md).
//
// Header only and pure, so tools\gpu_census_test can feed it fake ticks.

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

class GpuFrameGap {
public:
    static constexpr unsigned kRecent = 16;        // frames held while a neighbour may still arrive
    static constexpr unsigned kCapacity = 8192;    // gap samples kept per window
    static constexpr double kMaxGapMs = 1000.0;    // beyond it the gap is a stall: kept and flagged, not in the percentiles
    static constexpr unsigned kStallKeep = 3;      // stalls a window names (the largest), the rest only counted

    struct Stall {
        uint64_t sequence = 0;     // the frame that ended the gap
        double ms = 0.0;
    };
    struct Report {
        unsigned frames = 0;       // valid frames fed this window
        unsigned pairs = 0;        // pairs made and kept
        unsigned rejected = 0;     // pairs made but not comparable: overlapping, or another clock
        unsigned unusable = 0;     // frames fed with no sequence, no frequency or reversed ticks
        unsigned stalls = 0;       // pairs over kMaxGapMs: kept, flagged, and out of the percentiles
        unsigned stallsListed = 0; // how many of them are in `stall`
        Stall stall[kStallKeep];   // the largest, longest first
        double stallMaxMs = 0.0;
        bool any = false;          // at least one pair
        double p50Ms = 0.0, p95Ms = 0.0, maxMs = 0.0;
    };

    // One validated Application-render span, in the order the results COMPLETE.
    void feed(uint64_t sequence, uint64_t firstTick, uint64_t lastTick, uint64_t frequency) noexcept {
        if (!sequence || !frequency || lastTick < firstTick) {
            ++unusable_;
            return;
        }
        for (const Entry& r : recent_)
            if (r.used && r.sequence == sequence) return;   // a duplicate completion: already counted
        ++frames_;
        Entry e;
        e.used = true;
        e.sequence = sequence;
        e.first = firstTick;
        e.last = lastTick;
        e.frequency = frequency;
        for (const Entry& r : recent_) {
            if (!r.used) continue;
            if (r.sequence + 1 == sequence) makePair(r, e);
            else if (sequence + 1 == r.sequence) makePair(e, r);
        }
        // Keep it: into a free slot, else over the oldest sequence held.
        Entry* slot = nullptr;
        for (Entry& r : recent_)
            if (!r.used) { slot = &r; break; }
        if (!slot) {
            slot = &recent_[0];
            for (Entry& r : recent_)
                if (r.sequence < slot->sequence) slot = &r;
        }
        *slot = e;
    }

    // The window's figures, and the window starts over. The frames held for
    // pairing stay: a pair straddling the boundary is made by its later frame and
    // counted in the window that frame arrives in.
    Report finishWindow() noexcept {
        Report r;
        r.frames = frames_;
        r.pairs = pairs_;
        r.rejected = rejected_;
        r.unusable = unusable_;
        r.stalls = stalls_;
        r.stallMaxMs = stallMax_;
        r.stallsListed = stallsListed_;
        for (unsigned i = 0; i < stallsListed_; ++i) r.stall[i] = stallList_[i];
        r.any = stored_ > 0;
        if (stored_) {
            float* a = gaps_;
            float* end = gaps_ + stored_;
            r.maxMs = static_cast<double>(*std::max_element(a, end));
            unsigned i95 = static_cast<unsigned>(0.95 * static_cast<double>(stored_));
            if (i95 >= stored_) i95 = stored_ - 1;
            const unsigned i50 = stored_ / 2;
            std::nth_element(a, a + i95, end);
            r.p95Ms = static_cast<double>(a[i95]);
            if (i50 >= i95) {
                r.p50Ms = r.p95Ms;
            } else {
                std::nth_element(a, a + i50, a + i95);
                r.p50Ms = static_cast<double>(a[i50]);
            }
        }
        frames_ = pairs_ = rejected_ = unusable_ = stored_ = 0;
        stalls_ = stallsListed_ = 0;
        stallMax_ = 0.0;
        return r;
    }

private:
    struct Entry {
        bool used = false;
        uint64_t sequence = 0, first = 0, last = 0, frequency = 0;
    };
    // a is frame N, b is frame N+1.
    void makePair(const Entry& a, const Entry& b) noexcept {
        if (a.frequency != b.frequency || b.first < a.last) {
            ++rejected_;
            return;
        }
        const double ms = static_cast<double>(b.first - a.last) * 1000.0 / static_cast<double>(a.frequency);
        if (ms > kMaxGapMs) {
            noteStall(b.sequence, ms);
            return;
        }
        ++pairs_;
        if (stored_ < kCapacity) gaps_[stored_++] = static_cast<float>(ms);
    }
    // A pair over kMaxGapMs: counted, its length kept if it is among the longest kStallKeep of the window,
    // and not entered in gaps_, so p50, p95 and the max keep describing the gaps the census is about.
    void noteStall(uint64_t sequence, double ms) noexcept {
        ++stalls_;
        if (ms > stallMax_) stallMax_ = ms;
        unsigned at = stallsListed_;
        for (unsigned i = 0; i < stallsListed_; ++i) {
            if (ms > stallList_[i].ms) { at = i; break; }
        }
        if (at >= kStallKeep) return;
        const unsigned last = stallsListed_ < kStallKeep ? stallsListed_ : kStallKeep - 1;
        for (unsigned i = last; i > at; --i) stallList_[i] = stallList_[i - 1];
        stallList_[at] = {sequence, ms};
        if (stallsListed_ < kStallKeep) ++stallsListed_;
    }

    Entry recent_[kRecent];
    unsigned frames_ = 0, pairs_ = 0, rejected_ = 0, unusable_ = 0, stored_ = 0;
    unsigned stalls_ = 0, stallsListed_ = 0;
    double stallMax_ = 0.0;
    Stall stallList_[kStallKeep];
    float gaps_[kCapacity];
};

// The census line's own short form: "frame gap p50 0.42 / p95 1.85 ms over 2650 pairs", or
// "frame gap - (no pairs)" -- absent, never 0.00, when nothing was paired. A window with a stall (a
// gap over one second, kept and flagged: see the class comment) says so after the pairs, and only then:
// "frame gap p50 0.42 / p95 1.85 ms over 2650 pairs, 1 over 1 s".
inline size_t formatGapBrief(char* buf, size_t cap, const GpuFrameGap::Report& r) noexcept {
    if (!buf || !cap) return 0;
    int n;
    if (r.any) {
        n = std::snprintf(buf, cap, "frame gap p50 %.2f / p95 %.2f ms over %u pairs", r.p50Ms, r.p95Ms, r.pairs);
    } else {
        n = std::snprintf(buf, cap, "frame gap - (no pairs)");
    }
    if (n < 0) { buf[0] = 0; return 0; }
    size_t len = static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
    if (r.stalls && len + 1 < cap) {
        const int m = std::snprintf(buf + len, cap - len, ", %u over 1 s", r.stalls);
        if (m > 0) len = static_cast<size_t>(m) < cap - len ? len + static_cast<size_t>(m) : cap - 1;
    }
    return len;
}

// The line that says what the figure means. Printed every window with the census,
// so the caveat travels with the number.
inline size_t formatGapDetail(char* buf, size_t cap, const GpuFrameGap::Report& r) noexcept {
    if (!buf || !cap) return 0;
    size_t len = 0;
    buf[0] = 0;
    const auto add = [&](const char* fmt, auto... args) {
        if (len + 1 >= cap) return;
        const int n = std::snprintf(buf + len, cap - len, fmt, args...);
        if (n < 0) return;
        len = static_cast<size_t>(n) < cap - len ? len + static_cast<size_t>(n) : cap - 1;
    };
    add("EDVR GPU census, frame gap: the time on the GPU clock from the end of one frame's last EDVR-timed span on "
        "the game's device (after the door work) to the start of the next frame's first, ");
    if (r.any) {
        add("p50 %.2f ms, p95 %.2f ms, max %.2f ms over %u pairs of %u valid frames", r.p50Ms, r.p95Ms, r.maxMs, r.pairs,
            r.frames);
    } else {
        add("no pairs this window (%u valid frames; a frame's neighbour was missing or invalid)", r.frames);
    }
    if (r.rejected)
        add(", %u pairs rejected as not comparable (overlapping, or another clock)", r.rejected);
    // BEFORE the closing explanation, which is the part a short buffer should lose.
    if (r.stalls) {
        add(", %u pair%s over 1 s KEPT as stalls (not in the percentiles or the max; longest %.1f ms; each is the gap "
            "before that runtime sequence):",
            r.stalls, r.stalls == 1 ? "" : "s", r.stallMaxMs);
        for (unsigned i = 0; i < r.stallsListed; ++i)
            add("%s sequence %llu %.1f ms", i ? "," : "", static_cast<unsigned long long>(r.stall[i].sequence), r.stall[i].ms);
        if (r.stalls > r.stallsListed) add(" (and %u more)", r.stalls - r.stallsListed);
    }
    if (r.unusable) add(", %u spans unusable (no sequence, no frequency or reversed ticks)", r.unusable);
    add(". This is an upper bound on GPU idle, not idle: SteamVR's compositor (another process on the same GPU), "
        "the runtime's transfers and the Present copy run in it too, so a gap of about 1 ms is not proof of "
        "idleness; a gap near 0 says the GPU never waited.");
    return len;
}

}  // namespace edvr
