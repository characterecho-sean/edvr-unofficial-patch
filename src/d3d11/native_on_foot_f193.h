// F193 (native on foot VR): in the sights the body follows the camera's height, as it does at the hip. Header only, no game or D3D11 state, so native_frame_test runs the very functions the
// pin uses. Journal: the arc doc, F193. The behaviour, no key (it was an A/B key until the pilot flew it on and said to retire it); the rule before F193 stays as the stability stop's fallback.
//
// The pilot: "when crouching or jumping with ADS, the arms move unnaturally, like they're lagging the body; at the hip the arms stay in place". The flight edvr_gfx_20261031_080600
// (v0.22.0-rc.4-8-g58d4f8c8):
//   At the hip the pin's neck target is eye-based every frame (pinNeckTargetAnim). In the sights, at detach weight 1, it was the ANIMATED neck (pinDetachTarget) plus a raise F179a asked of the
//   legs' room (legsRaiseAskedM: the room taken from the animated legs every frame; F184 took the drawn eye out of it, for the loop below), the standing and crouched shares crossfaded by F163's
//   posture weight, a smoothstep on the animated neck's drop that lags the camera. The reach rule corrects horizontally only.
//   A jump in the sights (event 46, 08:12:04.853): the camera flat, the body's vertical against the animated neck climbing +7 to +44 cm, the left hand to the grip 26 to 32 cm off; the same
//   jump at the hip (event 23): the body goes down with the camera, the hand error 3.9 and 2.7 cm. A crouch entered in the sights (event 44): the body snapped 48 cm in 3 frames, 70 to 90 ms
//   after the camera stopped, the hand 27 cm off. The steady states agree: standing in the sights the neck 1.400 against the camera at about 1.54, crouched 0.82 against 0.96: both the camera
//   less 14 to 15 cm, which is what the hip's rule gives.
//
// The rule: in the sights (the detach weight above 0, armed) the neck target's height is the game's camera eye height less the eye height key, both stances; at the hip, as before. The camera
// is the pin's E: the game's own camera with every term EDVR adds taken out (the ADS shift, the weapon view offset, the neck reference's raise by the alignment serial the pose carried, the virtual
// neck, the standing-height hold's transient), normalised for the head's pitch: pc.eyeYSelected. It is never the DRAWN eye (the sight lock puts that on the sight line, which closes a loop through
// the body: F184), and nothing F186 pre-turns. A lowering is allowed (the leg solve bends the knees as it does at the hip); a raise stays capped by the legs' room (a foot must not lift). The
// hip's own demand is blended out and the camera's in by the detach weight, so the press and the release are continuous and the hip is untouched bit for bit.
// Why the value does not depend on the neck the pin writes: the argument is in the arc doc (F193) and at the call site.
//
// The stability stop: if the neck target moves more than 3 cm a frame for 5 frames in a row while the camera's own vertical moves less than 1 cm, the rule falls back to the old one for the rest
// of that press and says so once.
#pragma once

#include "native_on_foot_f97.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f193 {

constexpr double kStopTargetStepM = 0.03, kStopCameraStepM = 0.01;   // the stop: the target moves more than this a frame while the camera moves less than that
constexpr uint32_t kStopFrames = 5;                                  // for this many frames in a row

// Which rule set a call's neck height: the hip's, the legs' room (the rule before F193, in the sights), or the camera's (F193).
enum Source : int { kSrcHip = 0, kSrcLegs = 1, kSrcCamera = 2 };
inline int sourceOf(bool cameraRule, double detachW) noexcept { return cameraRule ? kSrcCamera : (detachW > 0.0 ? kSrcLegs : kSrcHip); }
inline const char* sourceName(int s) noexcept { return s == kSrcCamera ? "camera" : s == kSrcLegs ? "legs" : "hip"; }

// The rule runs when the stop has not tripped in this press, the sights are up (the detach weight above 0) and a weapon is drawn (F88's weight above 0: unarmed the target is the
// animated neck's, as it was).
inline bool wanted(bool stopped, double detachW, double armedW) noexcept { return !stopped && detachW > 0.0 && armedW > 0.0; }

struct Result {
    bool ok = false;                   // the rule made the target (the inputs were numbers); false: the caller keeps the old rule's
    nofoot::f97::ArmedTarget at;       // the target: y, and the demand (asked) blended hip -> camera by the detach weight
    double eyeAskM = std::nan("");     // (the camera eye - the key) over the animated neck, before the legs' cap (m, signed)
    double askedM = std::nan("");      // the same after the cap (a raise held to the legs' room; a lowering as it is)
    bool capped = false;               // the legs' room held the raise
};

// The neck target in the sights. hipAt: F97a's armedNeckTarget evaluated at detach weight 0 (the hip's demand: its asked and F93's raise); n0y: the animated neck's height; w: F88's weight;
// eyeLevelY: the game's camera eye with the head level (pc.eyeYSelected); eyeHeightM: the key; legsRoomM: what the legs give a raise before a foot lifts (not a number without legs: no raise);
// detachW: F73's weight. y = n0y + w x asked, asked = hip's x (1 - dw) + the camera's x dw.
inline Result cameraTarget(const nofoot::f97::ArmedTarget& hipAt, double n0y, double w, double eyeLevelY, double eyeHeightM, double legsRoomM, double detachW) noexcept {
    Result r;
    r.at = hipAt;
    if (!std::isfinite(n0y) || !std::isfinite(eyeLevelY) || !std::isfinite(eyeHeightM) || !std::isfinite(hipAt.asked)) return r;
    const double wc = !(w > 0.0) ? 0.0 : w > 1.0 ? 1.0 : w;
    if (wc == 0.0) return r;
    const double dw = !(detachW > 0.0) ? 0.0 : detachW > 1.0 ? 1.0 : detachW;
    r.eyeAskM = (eyeLevelY - eyeHeightM) - n0y;
    double ask = r.eyeAskM;
    if (ask > 0.0) {
        const double room = std::isfinite(legsRoomM) && legsRoomM > 0.0 ? legsRoomM : 0.0;
        if (ask > room) {
            ask = room;
            r.capped = true;
        }
    }
    r.askedM = ask;
    const double asked = hipAt.asked * (1.0 - dw) + ask * dw;
    r.at.asked = asked;
    r.at.y = n0y + wc * asked;
    r.at.raiseF93 = hipAt.raiseF93 * (1.0 - dw);
    r.at.lowerEye = hipAt.lowerEye * (1.0 - dw) + (ask < 0.0 ? ask * dw : 0.0);
    r.ok = true;
    return r;
}

// ---- the stability stop ---------------------------------------------------------------------------------------------------------------------------------------------------------------

struct StopGuard {
    bool stopped = false, have = false;
    double lastTarget = 0.0, lastCamera = 0.0;
    uint32_t run = 0, stops = 0;   // stops: the session's count (a press's end does not clear it)
    struct Out {
        bool stoppedNow = false;
        double targetStep = std::nan(""), cameraStep = std::nan("");
    };
    void endPress() noexcept {
        stopped = false;
        have = false;
        run = 0;
    }
    // One fresh game frame. inPress: the sights are up (the detach weight above 0); ruleRan: this call made the target by the camera rule; targetY: the neck target it ended with (by whichever rule);
    // cameraY: the camera's own height (the model's, as the rule reads it). The steps are measured on every frame in a press, by either rule (the A/B line compares them); the run is counted only
    // while the camera rule makes the target. The press's end clears the latch.
    Out step(bool inPress, bool ruleRan, double targetY, double cameraY) noexcept {
        Out o;
        if (!inPress) {
            endPress();
            return o;
        }
        if (have && std::isfinite(targetY) && std::isfinite(cameraY)) {
            o.targetStep = std::fabs(targetY - lastTarget);
            o.cameraStep = std::fabs(cameraY - lastCamera);
            if (ruleRan && o.targetStep > kStopTargetStepM && o.cameraStep < kStopCameraStepM) ++run;
            else run = 0;
            if (run >= kStopFrames) {
                stopped = true;
                ++stops;
                o.stoppedNow = true;
                run = 0;
            }
        } else {
            run = 0;
        }
        lastTarget = targetY;
        lastCamera = cameraY;
        have = std::isfinite(targetY) && std::isfinite(cameraY);
        return o;
    }
};

// ---- the 1 s line ---------------------------------------------------------------------------------------------------------------------------------------------------------------------

// One fresh call in the sights with a weapon: whether the camera rule made the target, the target less the camera's eye (m), and the two frame-to-frame steps (m; not a number on the first).
struct Sample {
    bool camera = false, stoppedNow = false;
    double neckMinusCamM = std::nan(""), camStepM = std::nan(""), tgtStepM = std::nan("");
};
struct Window {
    uint32_t calls = 0, camera = 0, offN = 0, steps = 0, stops = 0;
    double offSum = 0.0, offMin = 1e9, offMax = -1e9;
    double camStepSum = 0.0, camStepMax = 0.0, tgtStepSum = 0.0, tgtStepMax = 0.0;
    void add(const Sample& s) noexcept {
        ++calls;
        if (s.camera) ++camera;
        if (s.stoppedNow) ++stops;
        if (std::isfinite(s.neckMinusCamM)) {
            ++offN;
            offSum += s.neckMinusCamM;
            if (s.neckMinusCamM < offMin) offMin = s.neckMinusCamM;
            if (s.neckMinusCamM > offMax) offMax = s.neckMinusCamM;
        }
        if (std::isfinite(s.camStepM) && std::isfinite(s.tgtStepM)) {
            ++steps;
            camStepSum += s.camStepM;
            tgtStepSum += s.tgtStepM;
            if (s.camStepM > camStepMax) camStepMax = s.camStepM;
            if (s.tgtStepM > tgtStepMax) tgtStepMax = s.tgtStepM;
        }
    }
    bool any() const noexcept { return calls > 0; }
    void clear() noexcept { *this = Window{}; }
};

// "native on foot ADS body height (F193) 1 s: rule camera, ..." The rule is the camera when it made the target in at least half the calls, else the legs' (after a stop, for the rest of that press).
inline int formatLine(char* buf, size_t n, uint32_t seconds, const Window& w, double eyeHeightCm, uint32_t sessionStops) noexcept {
    const double on = w.offN ? static_cast<double>(w.offN) : 1.0, st = w.steps ? static_cast<double>(w.steps) : 1.0;
    return std::snprintf(buf, n,
                         "native on foot ADS body height (F193) %u s: rule %s (the camera's in %u of %u calls in the sights); "
                         "neck target - camera eye mean %+.1f min %+.1f max %+.1f cm (the camera rule reads minus the eye height key, %.1f cm); a frame's step: camera mean %.2f max %.2f cm, "
                         "neck target mean %.2f max %.2f cm; stops %u (session %u)",
                         seconds, w.camera * 2 >= w.calls ? "camera" : "legs", w.camera, w.calls, w.offN ? w.offSum / on * 100.0 : 0.0, w.offN ? w.offMin * 100.0 : 0.0,
                         w.offN ? w.offMax * 100.0 : 0.0, eyeHeightCm, w.steps ? w.camStepSum / st * 100.0 : 0.0, w.camStepMax * 100.0, w.steps ? w.tgtStepSum / st * 100.0 : 0.0,
                         w.tgtStepMax * 100.0, w.stops, sessionStops);
}
// The stop's line, once per press it trips in.
inline int formatStop(char* buf, size_t n, double targetStepM, double cameraStepM, uint32_t sessionStops) noexcept {
    return std::snprintf(buf, n,
                         "native on foot ADS body height stop (F193): the neck target moved over %.0f cm a frame for %u frames while the camera's vertical moved under %.0f cm (the last "
                         "frame: target %.1f cm, camera %.2f cm); the legs' room rule holds for the rest of this press; stops this session %u",
                         kStopTargetStepM * 100.0, kStopFrames, kStopCameraStepM * 100.0, targetStepM * 100.0, cameraStepM * 100.0, sessionStops);
}

}  // namespace f193
}  // namespace nofoot
}  // namespace edvr
