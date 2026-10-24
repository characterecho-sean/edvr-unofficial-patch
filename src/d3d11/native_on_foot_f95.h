// F95a (native on-foot VR): the neck reference from the stance's settled height, and the view-motion instrument. Header only, no game or D3D11 state, so
// native_frame_test runs the very functions the hooks use. Journal: the arc doc, 2026-10-24, F95a.
//
// The F94a flight (edvr_gfx_20261024_135403): standing, the animated neck held within 0.5 cm a second and the view was still; walking unarmed, the neck went through
// 7 cm every second (1.456-1.527 m, the walk cycle), F94a's raise target through 8-11 cm, and the raise followed it (0.2 s filter), so the eye moved against the
// game's camera frame, where the helmet HUD is anchored, by several centimetres at the walk's rate: the right eye less the FP root moved through a median 6.3 cm a
// second walking with fists against 2.6 cm in the F93 flight (no raise), while standing (0.7 against 0.9 cm) and with a tool or weapon drawn (the raise weighted
// out: 2.2 against 3.1 cm) nothing changed. The HUD bobbed with the commander's walk.
//
// F95a keeps the neck as the source of truth for the view's height, but as the stance's settled height. The raise's target (neck + eye height - the game's camera,
// F94a) is taken only from a settled stance: the body not walking or airborne (the AMC's speed and vertical speed, the pin's own thresholds) and, over the last
// kWindowS, the animated neck and the game's camera each within kBandM; the target is then the window's means. Anything else (the walk cycle, a landing, a stance
// still changing) leaves the raise where the last settled stance put it, so the view follows the game's own camera and the raise only moves when a stance has
// settled at another height (a crouch, a draw or holster, another item). The frame thread follows a new target with the standing-height hold's settle time
// (fix.native_on_foot_standing_height_settle_s, the pilot's 0.1 s), never under kSettleMinS (the loop's guard for a runtime that reports no alignment serial).
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f95 {

constexpr double kWindowS = 0.3;     // a stance is settled when it has held this long
constexpr double kBandM = 0.01;      // the animated neck and the game's camera each within this over the window
constexpr double kSettleMinS = 0.1;  // the raise's settle time never under this (with an unreporting runtime the loop is stable to 3 frames late at 0.1 s)
constexpr int kRingN = 128;          // samples held (the window at up to 400 calls a second)
constexpr double kGapS = 0.1;        // calls further apart than this start the window again
constexpr double kTimeEpsS = 1e-6;   // the window's edge (samples a whole number of frames apart land on it)

inline double settleS(double holdSettleS) noexcept { return !(holdSettleS >= kSettleMinS) ? kSettleMinS : holdSettleS; }

enum Why : uint8_t { kSettled = 0, kMoving = 1, kNeckMoving = 2, kCamMoving = 3, kFilling = 4, kInvalid = 5, kWhyN = 6 };
inline const char* whyName(int w) noexcept {
    return w == kSettled ? "settled" : w == kMoving ? "moving" : w == kNeckMoving ? "the neck moving" : w == kCamMoving ? "the camera moving" : w == kFilling ? "filling" : "no input";
}
struct SettleIn {
    bool valid = false;   // the neck and the camera were read
    bool still = false;   // the body is not walking or airborne (the AMC under the pin's walk speed, its vertical speed under the airborne threshold)
    double tS = 0.0;      // the sample's time (seconds, monotonic)
    double neckY = 0.0;   // the animated neck's height in the model
    double camY = 0.0;    // the game's camera in the model, head pitch and EDVR's terms taken out (the pin's eyeYSelected)
};
struct SettleOut {
    bool settled = false;
    uint8_t why = kInvalid;
    double neckY = 0.0, camY = 0.0;          // the window's means (settled only)
    double neckRange = 0.0, camRange = 0.0;  // over the window
};
struct StanceSettle {
    struct Sample {
        double t = 0.0, neck = 0.0, cam = 0.0;
    };
    Sample ring[kRingN];
    int head = 0, n = 0;
    void reset() noexcept { head = n = 0; }
    SettleOut step(const SettleIn& in) noexcept {
        SettleOut o;
        if (!in.valid || !std::isfinite(in.neckY) || !std::isfinite(in.camY) || !std::isfinite(in.tS)) {
            reset();
            o.why = kInvalid;
            return o;
        }
        if (!in.still) {
            reset();
            o.why = kMoving;
            return o;
        }
        if (n > 0) {   // time went back, or a gap in the calls: start again (a sample from before a gap is no evidence the stance held through it)
            const double gap = in.tS - ring[(head + kRingN - 1) % kRingN].t;
            if (gap < 0.0 || gap > kGapS) reset();
        }
        ring[head] = Sample{in.tS, in.neckY, in.camY};
        head = (head + 1) % kRingN;
        if (n < kRingN) ++n;
        // drop what is older than the window, keeping the newest sample at or before its start (the window's coverage)
        while (n > 1) {
            const Sample& second = ring[(head - n + 1 + kRingN) % kRingN];
            if (in.tS - second.t >= kWindowS - kTimeEpsS) --n;
            else break;
        }
        const Sample& oldest = ring[(head - n + kRingN) % kRingN];
        double nMin = 1e9, nMax = -1e9, cMin = 1e9, cMax = -1e9, nSum = 0.0, cSum = 0.0;
        for (int i = 0; i < n; ++i) {
            const Sample& s = ring[(head - n + i + kRingN) % kRingN];
            nMin = std::fmin(nMin, s.neck);
            nMax = std::fmax(nMax, s.neck);
            cMin = std::fmin(cMin, s.cam);
            cMax = std::fmax(cMax, s.cam);
            nSum += s.neck;
            cSum += s.cam;
        }
        o.neckRange = nMax - nMin;
        o.camRange = cMax - cMin;
        if (in.tS - oldest.t < kWindowS - kTimeEpsS) {
            o.why = kFilling;
            return o;
        }
        if (o.neckRange > kBandM) {
            o.why = kNeckMoving;
            return o;
        }
        if (o.camRange > kBandM) {
            o.why = kCamMoving;
            return o;
        }
        o.settled = true;
        o.why = kSettled;
        o.neckY = nSum / n;
        o.camY = cSum / n;
        return o;
    }
};

// The view-motion instrument: the frame-to-frame vertical step of a quantity (mm), as rms and largest.
struct StepStat {
    uint32_t n = 0, over1cm = 0;   // F97a: the steps over 1 cm counted apart (a model jump or a teleport is one frame and dominates the rms)
    double sumSq = 0.0, maxAbs = 0.0;
    void add(double stepM) noexcept {
        if (!std::isfinite(stepM)) return;
        const double mm = 1000.0 * stepM;
        ++n;
        sumSq += mm * mm;
        maxAbs = std::fmax(maxAbs, std::fabs(mm));
        if (std::fabs(mm) > 10.0) ++over1cm;
    }
    double rms() const noexcept { return n ? std::sqrt(sumSq / n) : 0.0; }
};
// The head counts as still between two frames when its LOCAL height moved under kStillM and its pitch under kStillDeg.
constexpr double kStillM = 0.001, kStillDeg = 0.3;
inline bool headStill(double dLocalY, double dPitchDeg) noexcept { return std::fabs(dLocalY) < kStillM && std::fabs(dPitchDeg) < kStillDeg; }

}  // namespace f95
}  // namespace nofoot
}  // namespace edvr
