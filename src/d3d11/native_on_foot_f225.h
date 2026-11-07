// F225 (native on-foot VR): the shoulder's reach margin stops being a step. Header only, no game or D3D11 state, so native_frame_test runs the very functions the arm solve uses.
//
// What the flight established. Recording 20261106_164037 (build 9e1f85d5), the rifle at the hip with the hand steady (under 8 mm a call): 82 elbow steps over 2 cm in 99 s, and 78 are flips of the
// F59 clavicle assist (l_assisted); of the assist's 115 flips, 113 have an elbow step over 2 cm within three calls. Each flip moves the shoulder 2.9 cm and the elbow 4.5 cm in one call, and the
// bend goes 149.7 to 133.9 deg. The flips are at reach 0.97 with no hysteresis (just before on 0.966, just before off 0.971). ikAssistReachMove (native_on_foot_core.h) returns 0 at or under
// over = dist - kIkAssistReach * (l1 + l2) of 0 and over + kIkAssistMarginM above it, so at the threshold the move jumps from 0 to 3 cm, and F67f took the per-call cap off the armed path
// (handCapApplies), so that jump lands in one call.
//
// The rule. The `over` part stays immediate, so the reach never passes 0.97 while the margin ramps and the hand is not delayed; only the 3 cm margin ramps, in over kRiseS while the arm is over the
// reach and out over kFallS otherwise, so the shoulder comes back smoothly. With the margin at its full weight and the arm over the reach the move is ikAssistReachMove's, exactly.
//
// Journal: the arc doc, F225. Behaviour, no key.
#pragma once

#include "native_on_foot_core.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f225 {

constexpr double kRiseS = 0.2;   // the margin's weight takes this long from 0 to 1 while the arm is over the reach
constexpr double kFallS = 0.4;   // and this long from 1 to 0 otherwise

struct Margin {
    double w = 0.0;
    // over: the arm is past the assist's reach this solve. A dt that is not finite or not positive leaves the weight alone.
    double step(bool over, double dt) noexcept {
        if (!std::isfinite(dt) || !(dt > 0.0)) return w;
        w += over ? dt / kRiseS : -dt / kFallS;
        w = w > 1.0 ? 1.0 : w < 0.0 ? 0.0 : w;
        return w;
    }
    void reset() noexcept { w = 0.0; }
};

// The reach part of the assist (m): the part past 0.97 (l1 + l2), immediate, plus the 3 cm margin by its weight, held to kIkAssistMaxM. *excess is the part past the reach (0 when in reach), as
// ikAssistReachMove reports it. At w = 1 and over the reach it is ikAssistReachMove; at w = 0 and in reach it is 0.
inline double reachMove(double dist, double l1, double l2, double w, double* excess) noexcept {
    const double over = dist - kIkAssistReach * (l1 + l2);
    if (excess) *excess = over > 0.0 ? over : 0.0;
    const double wv = std::isfinite(w) ? (w < 0.0 ? 0.0 : w > 1.0 ? 1.0 : w) : 0.0;
    const double m = (over > 0.0 ? over : 0.0) + kIkAssistMarginM * wv;
    return m < kIkAssistMaxM ? m : kIkAssistMaxM;
}

// ---- the log window ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Stats {
    uint32_t solves = 0, over = 0;
    double weightSum = 0.0, moveMaxCm = 0.0;
    // One left-arm armed solve: whether the arm was over the reach, the margin's weight, and the shoulder's assist move (cm).
    void add(bool wasOver, double weight, double moveCm) noexcept {
        ++solves;
        if (wasOver) ++over;
        weightSum += weight;
        if (moveCm > moveMaxCm) moveMaxCm = moveCm;
    }
    bool any() const noexcept { return solves > 0; }
    void clear() noexcept { *this = Stats{}; }
};

// The window's line. tools\edvr_log.py reads it.
inline int formatLine(char* buf, size_t n, uint32_t seconds, const Stats& s) noexcept {
    return std::snprintf(buf, n, "native on foot reach margin (F225) %u s: %u armed solves, over the reach on %u, margin weight mean %.2f; the shoulder's assist move largest %.1f cm", seconds, s.solves, s.over,
                         s.solves ? s.weightSum / s.solves : 0.0, s.moveMaxCm);
}

}  // namespace f225
}  // namespace nofoot
}  // namespace edvr
