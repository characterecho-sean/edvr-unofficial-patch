// The book kept of a session's long frames: what was logged, what was not, how big each was, and the
// worst few with when they ended (docs/freeze-diagnostics-2026-10-01.md, issue 63).
//
// WHY. Both halves of EDVR log a long frame under a rate limit and a session cap, which is right for a
// scene that drops a frame every few seconds and wrong for a freeze. In the issue 63 flight 44 of the 59
// mid-session LONG FRAME lines were one-frame blips (a 22-58 ms gap between Presents while the runtime's
// own cycle stayed under twice its period), they used the 60-line cap up, and the worst freeze of the
// session -- 1858 ms, the one the reporter had not noticed -- was kept out of the graphics log by the
// five-second limiter. Only the runtime log had it.
//
// THE RULES THIS HEADER HOLDS, for both halves:
//   * a frame (or cycle) of kFreezeAlwaysLogMs or more ALWAYS gets its line: the caller does not ask the
//     limiter, and this book counts any such frame that was not written, so a bug that let one through
//     the limiter shows as a nonzero over_250ms_unwritten instead of as nothing;
//   * a long frame that was not written is COUNTED, by size bucket, and the counts are printed every
//     few minutes and at the end of the session (formatCounts);
//   * the worst kWorst frames are kept with their time and runtime sequence, so the end-of-session line
//     names the freezes without anyone having to find them among the written ones.
//
// WHAT IT DOES NOT DO. It decides nothing about what is long: the graphics half judges by the runtime's
// cycle (perf_monitor.cpp), the runtime by its own cycle against twice the predicted period
// (native_runtime_host.h). It writes nothing and reads no clock: the caller passes the time as a string
// in the clock its own log uses (the graphics log's local time, the runtime log's UTC), so a rig can drive
// it deterministically. Single writer; a caller that prints from another thread copies the book under its
// own lock first (the copy is a plain struct).
//
// Header only and pure, so tools\freeze_log_test can hold every number and every line.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

// A frame this long is a freeze: it always gets a line, with no cap and no rate limit.
constexpr double kFreezeAlwaysLogMs = 250.0;
// How the counts and the worst few are repeated while the session runs (every few minutes), so a session
// that ends without a clean exit still left them in the log.
constexpr uint64_t kFreezeCountsEveryMs = 5ull * 60ull * 1000ull;

// What a Present gap is, judged (graphics half). The runtime half needs no judge: it logs a cycle of more
// than twice its predicted period, which is already the runtime's own cycle.
//
//   NotLong   not over twice the predicted period (or no period to compare with): nothing to say
//   Blip      over twice the period, but the runtime's cycle around it was not: one frame's Present moved
//             inside its cycle. Counted by size, never written
//   Long      over twice the period and the cycle confirms it, or the cycle cannot be read (the flat
//             profile, a runtime that has not completed two waits): the gap is trusted. Written under the
//             rate limit, counted by size either way
//   Freeze    a gap of kFreezeAlwaysLogMs or more, whatever the cycle says: written, always
//
// cycleMs is the longest runtime cycle the gap touches (perf_monitor.cpp's CycleView::longestMs): the
// previous cycle whole, and the current one up to this Present.
enum class FreezeVerdict { NotLong, Blip, Long, Freeze };

inline FreezeVerdict freezeJudge(double gapMs, double budgetMs, bool cycleKnown, double cycleMs) noexcept {
    if (!(budgetMs > 0.0) || !(gapMs > 2.0 * budgetMs)) return FreezeVerdict::NotLong;
    if (gapMs >= kFreezeAlwaysLogMs) return FreezeVerdict::Freeze;
    if (cycleKnown && !(cycleMs > 2.0 * budgetMs)) return FreezeVerdict::Blip;
    return FreezeVerdict::Long;
}

// Does a long frame get its line? A freeze always does; a long frame does when the rate limit allows;
// a blip and a non-long frame never do.
inline bool freezeWritesLine(FreezeVerdict v, bool limiterAllows) noexcept {
    return v == FreezeVerdict::Freeze || (v == FreezeVerdict::Long && limiterAllows);
}

// The judge and the write rule together, as the graphics half applies them to one Present gap.
// `limiterAllows` is the rate limit's answer for an ordinary long frame; it is not consulted for a
// freeze and does not matter for a blip.
struct FreezeOutcome {
    FreezeVerdict verdict = FreezeVerdict::NotLong;
    bool write = false;
};
inline FreezeOutcome freezeDecide(double gapMs, double budgetMs, bool cycleKnown, double cycleMs,
                                  bool limiterAllows) noexcept {
    FreezeOutcome o;
    o.verdict = freezeJudge(gapMs, budgetMs, cycleKnown, cycleMs);
    o.write = freezeWritesLine(o.verdict, limiterAllows);
    return o;
}

// The runtime half's rate limit on ordinary long cycles: at most perSecond lines in one wall-clock second
// and perSession in all. (The graphics half's is "one every five seconds, sixty a session", kept in
// perf_monitor.cpp beside its own clock.) A freeze is never asked and never charged: allows() is only
// consulted for a cycle under kFreezeAlwaysLogMs, and charge() only follows a line the limiter let through.
struct FreezeSecondLimiter {
    unsigned perSecond = 4;
    uint64_t perSession = 400;
    uint64_t second = 0;       // the wall-clock second the window counts
    unsigned window = 0;       // lines charged in it
    uint64_t charged = 0;      // lines charged this session

    bool allows(uint64_t nowSecond) noexcept {
        if (nowSecond != second) {
            second = nowSecond;
            window = 0;
        }
        return window < perSecond && charged < perSession;
    }
    void charge() noexcept {
        ++window;
        ++charged;
    }
};

// Size buckets, in milliseconds of the frame (graphics half) or cycle (runtime half).
enum FreezeBucket : unsigned {
    kFzUnder50 = 0,   // under 50 ms (a long frame starts at twice the period, 22 ms at 90 Hz)
    kFz50To100,
    kFz100To250,
    kFz250To1000,     // from here on every frame is written
    kFzOver1000,
    kFzBuckets
};

inline unsigned freezeBucketOf(double ms) noexcept {
    if (ms < 50.0) return kFzUnder50;
    if (ms < 100.0) return kFz50To100;
    if (ms < 250.0) return kFz100To250;
    if (ms < 1000.0) return kFz250To1000;
    return kFzOver1000;
}

// The key suffix of a bucket in the counts lines: long_lt50, long_50_100, long_100_250, long_250_1000,
// long_ge1000. Fixed words, so a reader's regular expression never meets a computed name.
inline const char* freezeBucketKey(unsigned bucket) noexcept {
    switch (bucket) {
        case kFzUnder50: return "lt50";
        case kFz50To100: return "50_100";
        case kFz100To250: return "100_250";
        case kFz250To1000: return "250_1000";
        default: return "ge1000";
    }
}

// One of the worst frames: how long, when it ended (a string in the writing log's own clock), and what
// is known about it.
struct FreezeWorst {
    double ms = 0.0;
    uint64_t sequence = 0;     // the runtime's sequence (0 when there is none)
    uint64_t frame = 0;        // the graphics half's frame number (0 on the runtime side)
    double cycleMs = 0.0;      // the runtime's cycle around it (0 when unknown)
    char stamp[40] = {};       // when it ended
    char note[120] = {};       // where the stall sampler found the thread, or "" when it did not
    // The runtime half keeps the cycle's own fields here, formatted when it made the list (a line of
    // long_cycle_line.h's formatLongCycleFields), so the end-of-session line is the line the cycle had
    // when it was logged. Empty in the graphics half, which prints its worst lines from the numbers above.
    char detail[1024] = {};
};

class FreezeBook {
public:
    static constexpr unsigned kWorst = 5;

    // Frames (or cycles) judged long, and what happened to each.
    uint64_t candidates = 0;                       // every gap over twice the period that reached the judge
    uint64_t blips = 0;                            // ...that the runtime's cycle did not confirm (graphics half only)
    uint64_t blipBucket[kFzBuckets] = {};
    uint64_t longCount = 0;                        // ...that it did (or that the judge could not question)
    uint64_t longBucket[kFzBuckets] = {};
    uint64_t written = 0;                          // long frames that got their line
    uint64_t unwritten = 0;                        // long frames the limiter or the cap kept out
    uint64_t unwrittenBucket[kFzBuckets] = {};
    uint64_t freezes = 0;                          // long frames of kFreezeAlwaysLogMs or more
    uint64_t freezesUnwritten = 0;                 // ...of which no line was written: always 0, printed so it is seen to be
    uint32_t worstRevision = 0;                    // moves whenever the worst list changes

    // What the judge made of a gap of `ms`, counted. NotLong is not a candidate and is not counted.
    void record(const FreezeOutcome& o, double ms) noexcept {
        if (o.verdict == FreezeVerdict::Blip) noteBlip(ms);
        else if (o.verdict == FreezeVerdict::Long || o.verdict == FreezeVerdict::Freeze) noteLong(ms, o.write);
    }

    // A candidate the runtime's cycle did not confirm: counted by its size, never written.
    void noteBlip(double ms) noexcept {
        ++candidates;
        ++blips;
        ++blipBucket[freezeBucketOf(ms)];
    }

    // A long frame. `lineWritten` says whether its line was written.
    void noteLong(double ms, bool lineWritten) noexcept {
        ++candidates;
        ++longCount;
        const unsigned b = freezeBucketOf(ms);
        ++longBucket[b];
        if (lineWritten) {
            ++written;
        } else {
            ++unwritten;
            ++unwrittenBucket[b];
        }
        if (ms >= kFreezeAlwaysLogMs) {
            ++freezes;
            if (!lineWritten) ++freezesUnwritten;
        }
    }

    // Would a frame of `ms` make the worst list? Asked before the entry's text is built, so a caller whose
    // text is costly (the runtime's, ~600 characters) builds it only for the few that are kept.
    bool wouldKeep(double ms) const noexcept {
        if (worstCount_ < kWorst) return true;
        return ms > worst_[kWorst - 1].ms;
    }

    // Offered to the worst list; kept when it is among the worst kWorst so far. Longest first; of two equal
    // frames the earlier stays ahead.
    void offerWorst(const FreezeWorst& w) noexcept {
        unsigned at = worstCount_;
        for (unsigned i = 0; i < worstCount_; ++i) {
            if (w.ms > worst_[i].ms) { at = i; break; }
        }
        if (at >= kWorst) return;
        const unsigned last = worstCount_ < kWorst ? worstCount_ : kWorst - 1;
        for (unsigned i = last; i > at; --i) worst_[i] = worst_[i - 1];
        worst_[at] = w;
        if (worstCount_ < kWorst) ++worstCount_;
        ++worstRevision;
    }

    unsigned worstCount() const noexcept { return worstCount_; }
    const FreezeWorst& worstAt(unsigned rank) const noexcept { return worst_[rank < kWorst ? rank : kWorst - 1]; }

    // The counts, as key=value pairs joined by `sep` (a space in the graphics log, a comma in the
    // runtime's), without a prefix and without a trailing separator:
    //   candidates=412 long=61 blips=351 written=59 unwritten=2 over_250ms=2 over_250ms_unwritten=0
    //   long_lt50=40 long_50_100=15 long_100_250=4 long_250_1000=1 long_ge1000=1
    //   blip_lt50=330 blip_50_100=20 ... unwritten_lt50=1 unwritten_50_100=1 unwritten_100_250=0
    // `withBlips` false leaves out the blip keys (the runtime judges by its own cycle: it has none).
    size_t formatCounts(char* buf, size_t cap, char sep, bool withBlips) const noexcept {
        if (!buf || !cap) return 0;
        size_t len = 0;
        buf[0] = 0;
        const char sepText[2] = {sep, 0};
        const auto add = [&](const char* key, uint64_t value) {
            if (len + 1 >= cap) return;
            const int n = std::snprintf(buf + len, cap - len, "%s%s=%llu", len ? sepText : "", key,
                                        static_cast<unsigned long long>(value));
            if (n < 0) return;
            len = static_cast<size_t>(n) < cap - len ? len + static_cast<size_t>(n) : cap - 1;
        };
        add("candidates", candidates);
        add("long", longCount);
        if (withBlips) add("blips", blips);
        add("written", written);
        add("unwritten", unwritten);
        add("over_250ms", freezes);
        add("over_250ms_unwritten", freezesUnwritten);
        char key[40];
        for (unsigned b = 0; b < kFzBuckets; ++b) {
            std::snprintf(key, sizeof(key), "long_%s", freezeBucketKey(b));
            add(key, longBucket[b]);
        }
        if (withBlips) {
            for (unsigned b = 0; b < kFzBuckets; ++b) {
                std::snprintf(key, sizeof(key), "blip_%s", freezeBucketKey(b));
                add(key, blipBucket[b]);
            }
        }
        // The buckets from 250 ms up are always written, so only the first three can be unwritten.
        for (unsigned b = 0; b < kFz250To1000; ++b) {
            std::snprintf(key, sizeof(key), "unwritten_%s", freezeBucketKey(b));
            add(key, unwrittenBucket[b]);
        }
        return len;
    }

private:
    FreezeWorst worst_[kWorst] = {};
    unsigned worstCount_ = 0;
};

// A bounded copy of text into a FreezeWorst field: cut at the buffer, always terminated.
inline void freezeCopyText(char* dst, size_t cap, const char* src) noexcept {
    if (!dst || !cap) return;
    size_t n = 0;
    if (src) {
        while (src[n] && n + 1 < cap) { dst[n] = src[n]; ++n; }
    }
    dst[n] = 0;
}

}  // namespace edvr
