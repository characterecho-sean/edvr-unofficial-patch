// F93d and F93e (native on-foot VR): the pure parts. Header only, no game or D3D11 state, so native_frame_test runs the very functions the frame thread and the
// Animate post-hook use. Journal: the arc doc, 2026-10-24, F93d and F93e.
//
// F93d, the game's camera. The camera suite's controller (VesselCameraMountControl) keeps the suite's mode in its byte +0x3E0, 0 while the suite is closed and the
// game's own first-person camera is the view, 1..6 while the suite is open (a preset such as "Commander control" or "Commander profile", the free camera and its locks).
// Explorer Cam's controller hook reads it after each game update and publishes it (native_on_foot.h, nativeOnFootCameraPublish); every EDVR change to the commander's
// body is first-person behaviour and stands down while it is not 0. CameraWatch is the frame thread's side: which lines are owed and when the published flag is no
// longer evidence (native stereo on foot ended, or the controller stopped updating with the suite open).
//
// F93e, the stance's height. The body pin puts the animated neck where the camera says (the eye less the neck-to-eye rest vector, 15.5 cm up and 9 cm forward,
// turned by the head's pitch). The game's first-person camera stands at about 1.58-1.59 m in the third-person model with every item, but the third-person stance's
// own eye height depends on the item: the rifle and pistol stances lean (neck 1.30-1.35 m), the tools, fists and empty hands stand upright (neck 1.54 m, the eye
// 1.70 m). So for an upright stance the pin lowered the upper body by 11 cm looking level and up to 17 cm with the aim pitched down (the game's camera pivots down
// about 6 cm at -21 deg), and the leg solve bent the knees (hip to foot 0.80 of the leg against 0.99 animated; the F92 flight, the Energylink standing). The head is
// hidden, so the height the stance has over the camera is taken by the head instead: the neck-to-eye vector pitches forward about the neck until the neck sits at
// its animated height, as a person lowers the eye by bending the neck, and the eye keeps its distance from the neck (never behind it, never below it). Only what the
// head cannot take (the eye level with the neck) still lowers the hips. A raise (the leaning stances) is untouched, and the game's own crouch is in the animated pose.
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f93 {

// ---- F93d ---------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kWatchMissMs = 5000;     // a stretch on foot with no controller read for this long is said once (the body changes then stay on in every camera)
constexpr uint64_t kWatchSilentMs = 1000;   // the controller quiet this long with the suite open at its last read: the flag is no longer evidence

struct WatchIn {
    bool active = false;     // native stereo on foot
    uint64_t reads = 0;      // the hook's read count (g_nativeOnFootCamReads)
    bool flag = false;       // the hook's flag: the suite was open at the last read (g_nativeOnFootCamExternal)
    uint64_t nowMs = 0;
};
struct WatchOut {
    bool clearFlag = false;  // the frame thread clears the hook's flag: off foot, or the controller went quiet with the suite open
    bool watching = false;   // the first read of a stretch on foot (one line)
    bool missing = false;    // no read in kWatchMissMs of a stretch on foot (one line)
    bool silent = false;     // the controller quiet over kWatchSilentMs with the suite open (one line; the flag cleared)
    bool changed = false;    // the camera differs from the one last logged: a transition line
    bool ended = false;      // native stereo on foot ended with the third-person camera last logged: the closing line
    bool external = false;   // the camera after this tick: true third-person
    uint64_t quietMs = 0;    // with silent: how long the controller had been quiet
};
struct CameraWatch {
    bool last = false, reading = false, missNoted = false, silentNoted = false, activeLast = false;
    uint64_t activeSinceMs = 0, readsLast = 0, readsMs = 0;
    WatchOut step(const WatchIn& in) noexcept {
        WatchOut o;
        if (in.active != activeLast) {
            activeLast = in.active;
            activeSinceMs = in.nowMs;
            reading = false;
            missNoted = false;
            silentNoted = false;
        }
        if (!in.active) {
            o.clearFlag = true;   // a suite state never outlives native stereo on foot
            if (last) {
                last = false;
                o.ended = true;
            }
            return o;
        }
        bool flag = in.flag;
        if (in.reads != readsLast) {
            readsLast = in.reads;
            readsMs = in.nowMs;
            silentNoted = false;
            if (!reading) {
                reading = true;
                o.watching = true;
            }
        } else if (!reading) {
            if (!missNoted && in.nowMs - activeSinceMs >= kWatchMissMs) {
                missNoted = true;
                o.missing = true;
            }
        } else if (flag && in.nowMs - readsMs > kWatchSilentMs && !silentNoted) {
            silentNoted = true;
            o.silent = true;
            o.quietMs = in.nowMs - readsMs;
            o.clearFlag = true;
            flag = false;
        }
        o.external = flag;
        if (flag != last) {
            last = flag;
            o.changed = true;
        }
        return o;
    }
};

// ---- F93e ---------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr double kNeckTakeMinUpM = 0.0;   // the eye is never taken below the neck joint's height: what is left then goes to the hips
struct NeckTake {
    bool applied = false;   // the target was moved
    double gapM = 0.0;      // the animated neck less the rest target's height: > 0 is how far the pin would have lowered the upper body
    double takenM = 0.0;    // the part the head took (the target raised by it)
    double pitchDeg = 0.0;  // the forward pitch of the neck-to-eye vector that took it
    double backM = 0.0;     // how much further the neck sits behind the eye (the body moves back by it)
    double hipsM = 0.0;     // the part left to the hips (gap - taken)
};
// eye: the pin's eye in the model (metres, y up); target: the neck target the rest vector gave (moved in place); hx, hz: the eye's unit heading in the model's
// horizontal plane; n0y: the animated neck's height. The vector eye - target is split into its forward part along the heading, its up part and a lateral part
// (kept); the forward and up parts turn about the lateral axis, the eye's distance from the neck kept, until the target's height is the animated neck's or the
// eye is level with the neck.
inline NeckTake neckTakesHeight(const double eye[3], double target[3], double hx, double hz, double n0y, double minUpM = kNeckTakeMinUpM) noexcept {
    NeckTake r;
    const double gap = n0y - target[1];
    if (!std::isfinite(gap) || !std::isfinite(hx) || !std::isfinite(hz)) return r;
    r.gapM = gap;
    if (!(gap > 0.0)) return r;
    r.hipsM = gap;
    const double v[3] = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    const double f = v[0] * hx + v[2] * hz;
    const double u = v[1];
    const double lat[2] = {v[0] - f * hx, v[2] - f * hz};
    const double rad = std::sqrt(f * f + u * u);
    if (!(rad > 1e-6) || !(f >= 0.0) || !(u > minUpM) || !std::isfinite(rad)) return r;   // the eye ahead of and above the neck, or nothing to turn
    const double uNew = u - gap > minUpM ? u - gap : minUpM;
    const double s = uNew / rad;
    const double phi1 = std::asin(s > 1.0 ? 1.0 : s < -1.0 ? -1.0 : s);
    const double phi0 = std::atan2(u, f);
    const double fNew = rad * std::cos(phi1);
    target[0] = eye[0] - (fNew * hx + lat[0]);
    target[1] = eye[1] - uNew;
    target[2] = eye[2] - (fNew * hz + lat[1]);
    r.applied = true;
    r.takenM = u - uNew;
    r.pitchDeg = (phi0 - phi1) * 180.0 / 3.14159265358979323846;
    r.backM = fNew - f;
    r.hipsM = gap - r.takenM;
    return r;
}

}  // namespace f93
}  // namespace nofoot
}  // namespace edvr
