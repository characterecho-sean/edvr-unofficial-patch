// F125 (native on-foot VR): the on-foot log's windows. Header only, no game or D3D11 state and no locks, so native_frame_test runs the very functions the lines
// use. Journal: the arc doc, 2026-10-25, F125.
//
// On foot about a hundred measurements each wrote a line a second, about 4 MB of log a minute, and the default size cap (log.max_mb, 16 MB) stopped a tester's
// log some 4.5 minutes after going on foot. With log.on_foot_detail off (the default) a measurement writes one summary line per 10 s; with it on, the lines are
// written as before, one a second. Events (a press, a boarding, a reset, an episode, a first refusal, an error) are written as they happen either way.
//
// Only the PRINT and the reset that goes with it move to the window. Whatever a line's code does each second for the feature itself (a rebase, a ring, a
// decision, a generation) keeps its own 1 s cadence, and nothing here reads or scales the clock it is given.
//
// The three shapes of line, and the helper each uses:
// - A detail line (a per-frame trace, the measurement of a settled feature) is written only while detail() is on. Its gate and its reset run as before and only
//   the note and the formatting are skipped, so no accumulator or ring grows because a line is not printed.
// - A summary of sums, minima, maxima or means: window() takes the place of the 1 s gate, and the line prints the seconds it returns where it printed "1 s".
//   The accumulators already divide by their own counts, so a longer window needs nothing else.
// - A summary of a per-second flag (armed, ADS, holding): the old 1 s gate stays, as a fold point (second()). Each fold counts the flag into a SecCount, and
//   the line prints "N of M s" when due() says the window is full.
// A flush closes a window early (a held-item change, a stance change, an ADS end, a changed setting the line echoes, the on-foot exit), so one line describes
// one state. A flushed window prints its own length, 1 to 10 s.
//
// Threads: detail() is a relaxed load any thread may make; a hook thread (the Animate job, the P1 callback, the render or HUD thread) reads it and nothing
// else here. The windows and the counts belong to the frame thread that prints the line.
#pragma once

#include <atomic>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace logwin {

constexpr uint32_t kSummarySeconds = 10;   // a summary line's window in the default log
constexpr uint32_t kDetailSeconds = 1;     // ... and in the detail log: the 1 s lines as they were before F125

// The live value of log.on_foot_detail. readKey (native_on_foot.cpp) reads the key on the frame thread when Config::generation() moves and stores it here, so a
// line on a game thread never takes Config's lock to find out. Off until that first read, as the key is by default.
inline std::atomic<bool> g_detail{false};
inline bool detail() noexcept { return g_detail.load(std::memory_order_relaxed); }
inline void setDetail(bool on) noexcept { g_detail.store(on, std::memory_order_relaxed); }

inline uint32_t windowSeconds(bool detailOn) noexcept { return detailOn ? kDetailSeconds : kSummarySeconds; }
inline uint32_t windowSeconds() noexcept { return windowSeconds(detail()); }

// The whole seconds a closed window covered: rounded, at least 1 (a window flushed a moment after it opened still says how long it was) and at most
// kSummarySeconds. A window runs past its length only when the line was not called for a while (a stalled frame thread, or time off foot with no flush at the
// exit); the cap keeps that gap from being counted as seconds of the measurement.
inline uint32_t closedSeconds(uint64_t elapsedMs) noexcept {
    const uint64_t s = (elapsedMs + 500u) / 1000u;
    return s < 1u ? 1u : s > kSummarySeconds ? kSummarySeconds : static_cast<uint32_t>(s);
}

// A summary of sums, called where the 1 s gate was. startMs is the line's window start, 0 while none is open. The first call opens a window and returns 0 (so a
// window never starts at the clock's zero and claims the whole uptime); later calls return 0 while it is open, and the first call at or past windowSeconds()
// closes it: the next window starts at nowMs and the call returns the closed one's seconds, which the line prints and then resets its sums. In the detail log
// the first line therefore comes a second after the line's first call, not at it.
inline uint32_t window(uint64_t& startMs, uint64_t nowMs, bool detailOn) noexcept {
    if (startMs == 0 || nowMs < startMs) {
        startMs = nowMs;
        return 0;
    }
    const uint64_t elapsed = nowMs - startMs;
    if (elapsed < 1000ull * windowSeconds(detailOn)) return 0;
    startMs = nowMs;
    return closedSeconds(elapsed);
}
inline uint32_t window(uint64_t& startMs, uint64_t nowMs) noexcept { return window(startMs, nowMs, detail()); }

// Closes an open window at once: its seconds for the line to print now, or 0 when none was open (nothing to print). The next window() opens a new window, so
// one that was flushed at the on-foot exit does not run on through the time off foot.
inline uint32_t flush(uint64_t& startMs, uint64_t nowMs) noexcept {
    if (startMs == 0) return 0;
    const uint64_t elapsed = nowMs > startMs ? nowMs - startMs : 0;
    startMs = 0;
    return closedSeconds(elapsed);
}

// The fold tick of a summary of per-second flags: exactly the old 1 s gate (true at the first call, then at the first call a second or more after the last
// true), so the work the line's code does each second keeps its cadence.
inline bool second(uint64_t& lastMs, uint64_t nowMs) noexcept {
    if (nowMs - lastMs < 1000u) return false;
    lastMs = nowMs;
    return true;
}

// The seconds a flag was set over a window of folded seconds, printed "N of M s". A line with several flags keeps one per flag; each is folded once a second,
// so their m agree and any of them is the window's length.
struct SecCount {
    uint32_t n = 0;   // folded seconds with the flag set
    uint32_t m = 0;   // folded seconds: the window's length
    void fold(bool flag) noexcept {
        ++m;
        if (flag) ++n;
    }
    void clear() noexcept { n = m = 0; }
};

// Whether a window of folded seconds is full: windowSeconds() folds (one in the detail log, so each fold prints, as the 1 s line did). A window part filled
// when the key was turned on is full at the next fold and prints its own length.
inline bool due(uint32_t folded, bool detailOn) noexcept { return folded >= windowSeconds(detailOn); }
inline bool due(uint32_t folded) noexcept { return due(folded, detail()); }

}  // namespace logwin
}  // namespace nofoot
}  // namespace edvr
