// F136 (native on-foot VR): an upright body under a level head while the aim is pitched down (mouse look). Header only, no game or D3D11 state, so native_frame_test runs the
// very functions the Animate post-hook uses. Journal: the arc doc, 2026-10-27, F136.
//
// With look to aim OFF (the Frontier flight edvr_gfx_20261027_100213, 10:05:30-44; earlier 091608, 09:19-09:21), the pilot aims down with the mouse and his head stays level.
// The game bends the third-person body down to aim: the chest's lean (chest 24 to neck 34, forward positive) is +58 to +61 deg at an aim of -67 to -74 deg against +33 to
// +37 deg at a level aim, and the animated neck is 1.30 m at a level aim (rifle stance) and 1.23 m aimed down. The view is held at the standing eye (1.683 m), so it sits 32 to
// 47 cm over the bent neck and the pilot sees a hunched torso below him. Under look to aim the head pitches with the aim and the same pose looks right. The pilot chose to keep
// the view on the headset and to write back part of the aim-down lean while the head is level:
//   written lean = animated lean - w (animated lean - level lean), the chest rotated back (nose up) about the body's right axis through the spine root,
// distributed over the spine levels exactly as the F58 bend and the square-up distribute theirs (1/3, 2/3, 1 over joints 16, 21, 24 and what hangs under them: not the hips,
// not the legs). w is the head's pitch above the aim's pitch (the gap), through smoothstep(10 deg, 30 deg), only while the aim is pitched down by more than 10 deg, ramped
// over 0.15 s, and 0 in ADS.
//
// Why nothing changes under look to aim: the follower (F109) asks the head's turn since the engage (plus the engage's centring), so the aim's pitch follows the headset's
// pitch and the gap (head pitch - aim pitch) stays near 0 (its smoothing is 150 ms, its rate cap 180 deg/s); a gap under 10 deg is w = 0. The hook also gives w = 0 whenever
// look to aim drops the player's look (g_ltaDrop), so a follower that lags a quick nod writes nothing either.
#pragma once

#include "native_on_foot_core.h"   // bendRot, kIkSpineWeight, kIkPi

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f136 {

constexpr double kDownAimDeg = 10.0;     // the aim must be pitched down by more than this (aim pitch < -10 deg)
constexpr double kGapLoDeg = 10.0, kGapHiDeg = 30.0;   // w = smoothstep over the head's pitch above the aim's
constexpr double kRampSec = 0.15;        // the applied weight moves at most 1 per kRampSec
constexpr double kLevelAimDeg = 10.0;    // |aim pitch| under this is a level aim (the stance's own lean is learned)
constexpr double kLevelSettleSec = 0.5;  // ... for this long before a sample counts
constexpr double kMaxBackDeg = 45.0;     // the most the chest is turned back

inline double smoothstep(double lo, double hi, double x) noexcept {
    if (!(x == x) || !(hi > lo)) return 0.0;
    const double t = x <= lo ? 0.0 : x >= hi ? 1.0 : (x - lo) / (hi - lo);
    return t * t * (3.0 - 2.0 * t);
}

// The target weight: the aim pitched down by more than kDownAimDeg, the head above it by the gap through smoothstep(10, 30). Degrees, nose up positive, both absolute (the body
// stands upright, so the head's pitch in LOCAL and the aim's pitch against the body's up are the same kind of angle). Unknown angles give 0.
inline double targetWeight(bool valid, double aimPitchDeg, double headPitchDeg) noexcept {
    if (!valid || !std::isfinite(aimPitchDeg) || !std::isfinite(headPitchDeg)) return 0.0;
    if (!(aimPitchDeg < -kDownAimDeg)) return 0.0;
    return smoothstep(kGapLoDeg, kGapHiDeg, headPitchDeg - aimPitchDeg);
}

// The weight the hook asks for: 0 while look to aim drops the player's look (the head is the look: the aim follows it and the follower's lag must not write anything), 0 in ADS
// (adsW, the sight lock owns the eye), else targetWeight.
inline double weightFor(bool lookToAimDropsLook, bool valid, double aimPitchDeg, double headPitchDeg, double adsW) noexcept {
    if (lookToAimDropsLook) return 0.0;
    const double a = adsW < 0.0 ? 0.0 : adsW > 1.0 ? 1.0 : adsW;
    return targetWeight(valid, aimPitchDeg, headPitchDeg) * (1.0 - a);
}

// The applied weight, ramped.
struct Ramp {
    double w = 0.0;
    double step(double target, double dtSec) noexcept {
        const double dt = dtSec > 0.0 && dtSec < 0.5 ? dtSec : 0.0;   // a long gap is no time at all (a stall must not pop it)
        const double d = dt / kRampSec;
        const double t = target < 0.0 ? 0.0 : target > 1.0 ? 1.0 : target;
        if (t > w) w = w + d < t ? w + d : t;
        else w = w - d > t ? w - d : t;
        return w;
    }
};

// The chest's lean from the chest joint (24) to the neck joint (34), forward positive, in the model's sagittal plane (y up, z forward), degrees: the F58 bend's own angle.
inline double leanDeg(const double chest[3], const double neck[3]) noexcept {
    const double dy = neck[1] - chest[1], dz = neck[2] - chest[2];
    if (!(std::fabs(dy) + std::fabs(dz) > 1e-6)) return 0.0;
    return std::atan2(dz, dy) * 180.0 / 3.14159265358979323846;
}

// The angle the chest is turned back by (degrees, >= 0, at most kMaxBackDeg): w times how much more the animated chest leans than the stance does at a level aim. A chest
// leaning less than at the level aim is never turned forward.
inline double backDeg(double w, double leanAnimatedDeg, double leanLevelDeg) noexcept {
    if (!std::isfinite(w) || !std::isfinite(leanAnimatedDeg) || !std::isfinite(leanLevelDeg)) return 0.0;
    const double d = leanAnimatedDeg - leanLevelDeg;
    if (!(d > 0.0) || !(w > 0.0)) return 0.0;
    const double b = (w > 1.0 ? 1.0 : w) * d;
    return b > kMaxBackDeg ? kMaxBackDeg : b;
}

// The row-vector rotation of one spine level that turns the chest back by `backDeg` (nose up, about the body's right axis +x): the F58 bend's own rotation with a pitch of
// -backDeg, times the level's share (kIkSpineWeight: 1/3, 2/3, 1). Applied about the spine root P to every joint of the level (ikPivot).
inline void backRot(double backDeg, double levelShare, double R[9]) noexcept {
    bendRot(-backDeg * kIkPi / 180.0, 0.0, levelShare, R);
}

// The stance's chest lean at a level aim, learned per held item from the animated pose itself: samples count only after the aim has been level (|pitch| < kLevelAimDeg) for
// kLevelSettleSec, and move an average of the item's. Four items are kept (the least recently used goes). Nothing is assumed for an item never seen at a level aim: the
// hook writes nothing for it (the pilot raises a weapon level, so the first level seconds precede any aim down).
struct LevelLean {
    static constexpr int kItems = 4;
    struct Item {
        bool used = false;
        uint32_t serial = 0;
        double lean = 0.0;
        uint32_t samples = 0;
        uint64_t lastMs = 0;
    };
    Item item[kItems];
    double levelSinceSec = -1.0;   // when the aim became level, < 0 while it is not
    Item* find(uint32_t serial) noexcept {
        for (auto& i : item)
            if (i.used && i.serial == serial) return &i;
        return nullptr;
    }
    // One call: the aim's pitch (valid), the animated lean, the item, the clock (seconds and ms).
    void observe(bool valid, double aimPitchDeg, double leanAnimatedDeg, uint32_t serial, double nowSec, uint64_t nowMs) noexcept {
        if (!valid || !std::isfinite(aimPitchDeg) || !std::isfinite(leanAnimatedDeg) || !(std::fabs(aimPitchDeg) < kLevelAimDeg)) {
            levelSinceSec = -1.0;
            return;
        }
        if (levelSinceSec < 0.0) levelSinceSec = nowSec;
        if (nowSec - levelSinceSec < kLevelSettleSec) return;
        Item* it = find(serial);
        if (!it) {
            Item* pick = &item[0];
            for (auto& i : item) {
                if (!i.used) {
                    pick = &i;
                    break;
                }
                if (i.lastMs < pick->lastMs) pick = &i;
            }
            *pick = Item{};
            pick->used = true;
            pick->serial = serial;
            it = pick;
        }
        const double a = it->samples < 10 ? 1.0 / static_cast<double>(it->samples + 1) : 0.1;   // a plain mean of the first ten, then an average over about ten calls
        it->lean = it->samples == 0 ? leanAnimatedDeg : it->lean + a * (leanAnimatedDeg - it->lean);
        if (it->samples < 1000000u) ++it->samples;
        it->lastMs = nowMs ? nowMs : 1;
    }
    bool level(uint32_t serial, double* leanDeg) noexcept {
        const Item* it = find(serial);
        if (!it || it->samples < 5) return false;
        *leanDeg = it->lean;
        return true;
    }
};

// The second's figures for the line.
struct Window {
    uint32_t calls = 0, applied = 0, joints = 0, levelUnknown = 0;
    double wSum = 0.0, wMax = 0.0, aimSum = 0.0, gapSum = 0.0, animSum = 0.0, writtenSum = 0.0, levelSum = 0.0;
    uint32_t aimN = 0, leanN = 0;
    uint32_t serial = 0;
    void clear() noexcept { *this = Window{}; }
};

}  // namespace f136
}  // namespace nofoot
}  // namespace edvr
