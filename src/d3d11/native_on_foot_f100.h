// F100 (native on-foot VR): the performance round's pure parts. Header only, no game or D3D11 state, so native_frame_test runs the very functions the render
// and frame threads use. Journal: the arc doc, 2026-10-24, F100; the review: reviews\native-onfoot-perf-review-2026-10-24.md.
//
// The F98 flight (edvr_gfx_20261024_192353) measured the marker probe's draw tee at about 2.0M calls a second (17,000-19,300 draws a frame), with only the
// part past the VS hash timed (8.9 ms a second); the load sampler stopping the render, runtime, driver and game threads ten times a second all session (its
// want rule lived here until F102 removed the sampler); the F68d onset dump writing 360 lines in one frame; the eye probe's two lines every frame; and the F70b
// ADS input lines twice a tick off foot.
#pragma once

#include "native_on_foot_core.h"   // hudIssuerIndex, kMarkerCompositeVs

#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f100 {

constexpr uint32_t kNoFrame = 0xFFFFFFFFu;

// ---- 1: the marker tee's prefilter --------------------------------------------------------------------------------------------------------------------
// The tee's slow path (nativeOnFootMarkerProbeDraw) does, for a draw whose vertex shader is neither an on-foot HUD issuer's nor the marker composite's, only
// this once the frame has turned, nothing is pending or bound, no census routes world draws, both eyes have their world rows and no measurement follows every
// draw (F57): it advances the draw serial and counts the call. So the slow path leaves the frame it can be skipped in, and the fast path does just that.
// Only the slow path's own outputs can change these conditions (a marker draw's decision, a world draw's rows, the frame's turn), plus Begin binding a
// private b0 (which clears the fast frame) and the frame boundary (a new frame number).
inline bool teeSlowVs(uint64_t vs) noexcept { return hudIssuerIndex(vs) >= 0 || vs == kMarkerCompositeVs; }

struct TeeState {
    bool measure = false;          // fix.native_on_foot_marker_probe's measurement families are on (F57 follows every draw)
    bool pending = false;          // a decision waits for its draw's issue
    bool bound = false;            // a private b0 is bound and End has not run
    bool census = false;           // this frame routes every world draw to the slow path (F53's census frames)
    uint32_t polledFrame = kNoFrame;   // the frame the slow path last turned
    uint32_t worldOk[2] = {kNoFrame, kNoFrame};   // the frame each eye's world rows are from
    bool check = false;            // F113: a row source's rows wait for a scene mesh's check this frame (once a second an eye at most)
};

// The frame the next draws may take the fast path in, or kNoFrame.
inline uint32_t teeFastFrame(const TeeState& s, uint32_t frame) noexcept {
    const bool ok = !s.measure && !s.pending && !s.bound && !s.census && !s.check && s.polledFrame == frame && s.worldOk[0] == frame && s.worldOk[1] == frame;
    return ok && frame != kNoFrame ? frame : kNoFrame;
}

inline bool teeFast(uint32_t fastFrame, uint32_t frameNow, uint64_t vs) noexcept {
    return fastFrame != kNoFrame && fastFrame == frameNow && !teeSlowVs(vs);
}

// One draw through the tee. `slow(vs)` runs the slow path and returns the fast frame it leaves (teeFastFrame of its state). True when the fast path took it.
template <class Slow>
inline bool teeStep(uint32_t& fastFrame, uint32_t frameNow, uint64_t vs, uint64_t& serial, uint64_t& fastCalls, Slow&& slow) {
    if (teeFast(fastFrame, frameNow, vs)) {
        ++serial;
        ++fastCalls;
        return true;
    }
    fastFrame = slow(vs);
    return false;
}

// The tee's clock: one call in kTeeClockStride on the perf monitor's sample frames is timed whole (fast or slow), so the 1 s cost line can say what the tee
// costs a frame. ticks / clocked is the mean call; times the calls of the second, over its frames, the estimate a frame.
constexpr uint32_t kTeeClockStride = 64;
inline double teeMsPerFrame(uint64_t ticks, uint64_t clocked, uint64_t calls, uint64_t frames, uint64_t qpcFreq) noexcept {
    if (!clocked || !frames || !qpcFreq) return -1.0;
    const double perCall = static_cast<double>(ticks) / static_cast<double>(clocked) / static_cast<double>(qpcFreq);   // seconds
    return perCall * static_cast<double>(calls) / static_cast<double>(frames) * 1000.0;
}
inline double teeNsPerCall(uint64_t ticks, uint64_t clocked, uint64_t qpcFreq) noexcept {
    if (!clocked || !qpcFreq) return -1.0;
    return static_cast<double>(ticks) / static_cast<double>(clocked) / static_cast<double>(qpcFreq) * 1e9;
}

// ---- 2: the load sampler's want: removed with the load sampler in F102 ----------------------------------------------------------------------------------

// ---- 3: the F68d onset window, printed a slice a tick -------------------------------------------------------------------------------------------------
// The window's lines are taken whole at the dump (the same records, the same text, the same order); the frame thread prints at most kOnsetLinesPerTick of
// them a tick, the summary with the last slice. 360 lines at 90 Hz print in 15 ticks.
constexpr uint32_t kOnsetLinesPerTick = 24;
struct PacedLines {
    uint32_t total = 0, next = 0;
    bool active = false;
    void begin(uint32_t n) noexcept {
        total = n;
        next = 0;
        active = true;
    }
    // The [first, first + count) slice to print this tick; count 0 once all are out.
    uint32_t take(uint32_t* first) noexcept {
        *first = next;
        if (!active) return 0;
        const uint32_t left = total - next;
        const uint32_t n = left < kOnsetLinesPerTick ? left : kOnsetLinesPerTick;
        next += n;
        return n;
    }
    // True once, when the last slice has been taken (the same tick): the summary's turn.
    bool finish() noexcept {
        if (!active || next < total) return false;
        active = false;
        return true;
    }
};

// ---- 8: the eye probe's lines, once a second ----------------------------------------------------------------------------------------------------------
// With body align on, the hook samples on every call (the align needs it), and the line printed every new sample: every frame. A sample is printed when
// it is new and at least a second after the last printed one.
inline bool eyeLineDue(uint64_t stampMs, uint64_t lastLoggedMs) noexcept {
    return stampMs != 0 && stampMs != lastLoggedMs && (lastLoggedMs == 0 || stampMs - lastLoggedMs >= 1000u);
}

// ---- 9: the F70b change lines -------------------------------------------------------------------------------------------------------------------------
// The two change detectors restart (so both lines print once) on each edge of the ADS action's eligibility, not on every ineligible tick.
inline bool adsChangeLinesRestart(bool eligible, bool eligibleLast) noexcept { return eligible != eligibleLast; }

}  // namespace f100
}  // namespace nofoot
}  // namespace edvr
