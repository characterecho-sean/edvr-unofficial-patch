#pragma once
// TEMPORARY, the repeat-draw fast path's A/B (Phase 2b, built 2026-10-27; docs\engine-render-performance-2026-09-19.md, "Desk design: the repeat-draw fast path").
// No Windows and no D3D here, so tools\aa_off_test drives every decision.
//
// WHY. The Phase 2 flight showed the fast path exact (0 mismatches) and taking about 61% of owner X draws, but could not show that it SAVES time: the draw
// counts differed between flights, the decision ran before the DrawClock, and the full-path draws carried the mechanism's own capture. So the fast path
// is switched on and off INSIDE one flight. The perf monitor's 1800-frame windows alternate: ON is Phase 2 as built; OFF is no mechanism at all -- no
// decision, no outcome capture, no notes in forwardWithVerdict or the lambda, no ticket, no counters -- so the X thunk runs the full path it ran before the
// shadow phases. The arm is set once, at the edge, for the whole next window, through a plain global the draw path reads (vscreen.cpp g_fastPathOn).
// The DrawClock now starts at the top of the X thunk, so the `draw hook CPU` line covers the decision and the same prologue in both arms.
//
// THE ARM. The window that begins after the edge at frame E (E a multiple of 1800) is ON when E / 1800 is even: the session's first window (frames 1 to
// 1800) is ON, the second OFF, and so on. The line the edge writes reports the window that just ended, under the arm it ran with.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "draw_fast_path_shadow.h"

namespace edvr {
namespace fastab {

constexpr uint32_t kWindowFrames = 1800;

// The arm of the window that begins after the edge at `edgeFrame`.
constexpr bool armAfterEdge(uint32_t edgeFrame) { return (edgeFrame / kWindowFrames) % 2u == 0u; }
// The arm before the first edge.
constexpr bool kInitialArm = true;
static_assert(armAfterEdge(0) == kInitialArm, "the session's first window is ON");
// The arm frame `frame` (1 or more) ran under: the window (E, E + 1800] it falls in, E a multiple of 1800. Frame 0 is before the first frame.
constexpr bool armOfFrame(uint32_t frame) { return frame == 0 ? kInitialArm : armAfterEdge(((frame - 1u) / kWindowFrames) * kWindowFrames); }

// The median of the n samples at v (the upper median for an even count, element n/2 of the sorted values, as the Present hook ticks line takes it). The array
// is rearranged. False when there are none.
inline bool median(double* v, size_t n, double* out) {
    if (!n) return false;
    std::nth_element(v, v + n / 2, v + n);
    *out = v[n / 2];
    return true;
}

// One window's figures. A flag false prints '-'.
struct Window {
    uint32_t endFrame = 0;
    bool on = false;
    uint32_t frames = 0;
    double seconds = 0.0;              // the window's wall time; 0 or less is unknown
    bool clockDrawsKnown = false;      // X draws a frame as the step clock sees them (clocked draws scaled by the stride): both arms
    double clockDrawsPerFrame = 0.0;
    uint64_t ownerDraws = 0;           // owner X draws, exact (every one the shadow counted; ON only)
    uint64_t fastDraws = 0;            // ...of them, fast-path draws
    bool oldKnown = false;             // the `draw hook CPU` line's mean over its sampled frames
    double oldMeanMs = 0.0;
    uint32_t oldSamples = 0;
    bool stepKnown = false;            // the step line's EDVR sum
    double stepSumMs = 0.0;
    bool cpuKnown = false;             // the native application CPU's median over the window's frames
    double cpuP50Ms = 0.0;
    uint32_t cpuSamples = 0;
};

inline void putMs(fastshadow::Out& o, bool known, double ms) {
    if (known) o.put("%.3f", ms);
    else o.put("-");
}

// The A/B line, written at the 1800-frame edge after the other draw lines. Under 1150 characters at its worst (the rig pins it).
inline size_t formatLine(char* out, size_t cap, const Window& w) {
    fastshadow::Out o(out, cap);
    o.put("draw fast path A/B: TEMPORARY (built 2026-10-27, Phase 2b), 1800-frame window ending %u, arm %s, %u frames, ", static_cast<unsigned>(w.endFrame),
          w.on ? "ON" : "OFF", static_cast<unsigned>(w.frames));
    if (w.seconds > 0.0) {
        o.put("%.2f s, %.1f fps", w.seconds, static_cast<double>(w.frames) / w.seconds);
    } else {
        o.put("- s, - fps");
    }
    o.put(". X draws a frame ");
    if (w.clockDrawsKnown) o.put("%.0f", w.clockDrawsPerFrame);
    else o.put("-");
    o.put(" (step clock estimate)");
    const double f = w.frames ? static_cast<double>(w.frames) : 1.0;
    if (w.on && w.ownerDraws) {
        o.put("; owner X draws a frame %.1f, fast path %.1f%%", static_cast<double>(w.ownerDraws) / f, 100.0 * static_cast<double>(w.fastDraws) / static_cast<double>(w.ownerDraws));
    } else {
        o.put("; owner X draws and fast share - (%s)", w.on ? "none counted" : "OFF: no counters");
    }
    o.put(". Old draw hook CPU line mean ");
    putMs(o, w.oldKnown, w.oldMeanMs);
    o.put(" ms over %u sampled frames. EDVR step sum ", static_cast<unsigned>(w.oldSamples));
    putMs(o, w.stepKnown, w.stepSumMs);
    o.put(" ms. Native application CPU p50 ");
    putMs(o, w.cpuKnown, w.cpuP50Ms);
    o.put(" ms over %u frames", static_cast<unsigned>(w.cpuSamples));
    if (!w.cpuKnown) o.put(" (none: the native timing queue gave no valid frame in the window)");
    o.put(".");
    return o.len;
}

}  // namespace fastab
}  // namespace edvr
