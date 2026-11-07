// F215 (native on-foot VR): unarmed and upright, the body follows the view. Header only, no game or D3D11 state, so native_frame_test runs the very functions the pin hook uses.
//
// What the flights established. Recordings on a hill, native on-foot VR, unarmed. E below is the game's job camera with EDVR's own terms removed, in the model's space; N0 is the animated
// neck; the raise is the view's height over E.
//  - Flight 20261106_042552, 14 in-place turns: the point 0.425 m ahead of the model root along the facing stays still (the body root swings on that circle). The game's camera moved a median
//    0.9 cm in the world, the body root 10.6 cm (at most 18), and the view 11.7 cm, because the raise keeps the view a set height over the neck. The body pin's forward move is already a
//    rigid move of the whole skeleton, feet included.
//  - On level stretches of two flights, E over the 0.6 s mean of the animated neck reads +8.8 and +9.9 cm, whether walking or standing (E follows the neck's gait height), so the level-ground
//    value is one constant (nofoot::f94::kCamOverUprightNeckM).
//
// The rule. The raise becomes a constant R0 = the eye height key less that constant. The whole skeleton, feet included, moves vertically by y:
//     target y = E.y - mean(N0.y over kNeckMeanS) - kCamOverUprightNeckM, clamped to +-kCapM
// E itself is not filtered. The view is then the game's camera plus a constant, so it holds still in a turn, and the neck stays the eye height under the view at any facing or speed. The shift
// goes only on the writes into the game's pose; every internal figure of the pin stays what it was, so with y = 0 everything is as it was before this rule.
//
// F217: the same move with an item at the hip. A flight with the arc cutter on a tilted hull showed why the unarmed rule alone is not enough: E stood 55 cm over the neck, the hands (placed from
// the camera through the FP-to-3P map G) rose 60 cm, the neck was held by the planted feet, the settle then pulled the view down 25 cm so the tool stood above the view, and walking off the
// stale raise left the view 20 cm under the neck for 8.6 s. Now the whole body follows E there too, so hands, neck and view keep their level-ground relation on any slope. With an item at the
// hip the target is E less one level height (levelCamY), not E over the animated neck: on level ground E read 1.60-1.61 m with the arc cutter and 1.56-1.59 with the laser rifle (recording
// 20261106_090823), at any aim from -60 to +10 deg, while E over the animated neck varied from +12 to +28 cm by item and gait. An aim pitched up 20 deg or more holds y (the camera sinks as the
// aim pitches up); the sights hold it; a low posture releases it.
//
// One internal space. The pin and the arm IK work in the model's space with the body unshifted, and every write adds y. So every input that comes from the true camera or the FP rig enters
// less y (shiftWorldToModel on the world-to-model matrix and on G), and the y used in a call is the last call's result: the step's result takes effect on the next call. The neck's true height
// is then a function of the true camera whatever y is.
//
// F226: the head's pitch no longer holds y. Recording 20261106_185259 (the flight of F224 and F225): kHeadGateDeg froze y at -0.40 m for 7.24 s after a landing's camera dip, the pilot looking down
// the whole time. The rule reads pc.eyeYSelected, which has the head's pitch taken out, so a nod moves its input only -0.30 to +0.11 cm per 10 deg armed and -0.05 to 0 unarmed: there is nothing for
// a hold to protect. A head past the gate now falls through to the same rule as one inside it (the hip item's active state, or the unarmed neck window, which keeps filling), and the pose-less
// case stays held (without a pose the normalisation is off). The settle is as it was: Out::headPitched is true exactly where the old head check sat, and the pin refuses the settle on it, so the
// stance's store is never taken with the head pitched. The F224 lead reads the raw camera (e223 in native_on_foot.cpp), so a nod reaches the shift for that call only and does not accumulate; standing
// with the head past the gate the raw camera steps a median of 0.27 to 0.37 mm a call, 2.7 to 3.1 mm at the 99th percentile and 12 mm at the largest. The lead is deliberately left as it is. It now also
// runs in jumps with the head down, where it was off before.
//
// One writer: the pin hook (under g_pfzBusy). The key (experimental.native_on_foot_body_follows_view) is TEMPORARY, an A/B switch for the pilot; with it off nothing here runs and F214 is
// in force as it was.
#pragma once

#include "native_on_foot_f214.h"
#include "native_on_foot_f94.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f215 {

constexpr double kNeckMeanS = 0.6;      // the animated neck's mean window (f214::Mean's window)
constexpr double kFillS = 0.2;          // the window must span this much before the rule acts
constexpr double kHeadGateDeg = 20.0;   // the physical head's pitch, either way, at or over which the stance's settle is refused (F226: y no longer holds on it)
constexpr double kCapM = 0.6;           // |target y| is held to this
constexpr double kTrackMps = 3.0;       // y follows its target at most this fast
constexpr double kCatchMps = 0.6;       // ... or this fast while catching (after any change of state into or out of the rule, and while releasing)
constexpr double kCatchDoneM = 0.005;   // catching ends when the rule is active and the target is this near
constexpr double kMaxDtS = 0.1;
static_assert(f214::kWindowS == kNeckMeanS, "the neck's mean uses F214's window");

// The level-ground constant: the game's camera over the mean animated neck (f94::kCamOverUprightNeckM, 0.08 m).
constexpr double kCamOverNeckM = f94::kCamOverUprightNeckM;
// R0, the constant raise.
inline double levelRaiseM(double eyeHeightM) noexcept { return eyeHeightM - kCamOverNeckM; }

// The level-ground camera with an item at the hip. On level ground E read 1.60-1.61 m with the arc cutter and 1.56-1.59 with the laser rifle (recording 20261106_090823), at any aim from -60 to
// +10 deg, while E over the animated neck varied from +12 to +28 cm by item and gait, so the rule takes E against one height rather than against the neck. The stored unarmed standing neck read
// 1.542 m in that flight; kCamOverNeckItemM is the camera's mean height over it, kDefaultLevelCamM the height with no store.
constexpr double kCamOverNeckItemM = 0.06;
constexpr double kDefaultLevelCamM = 1.60;
constexpr double kAimUpHoldDeg = 20.0;   // an aim pitched up this far or more holds y with an item at the hip (only upward: the camera sinks as the aim pitches up)
inline double levelCamY(bool haveStandingNeck, double storedNeckY) noexcept {
    return haveStandingNeck && std::isfinite(storedNeckY) ? storedNeckY + kCamOverNeckItemM : kDefaultLevelCamM;
}
// What a pin call did, in the recording's order (onfoot_recorder_core.h ONF_ENUM(F215State)): not_run is a call that never reached the step.
enum State : uint8_t {
    kOff = 0, kReleasedAway, kReleasedItem, kReleasedPosture, kHeldAim, kHeldPose, kFilling, kActive, kHeldSights, kActiveItem, kNotRun, kStates
};
inline const char* stateName(unsigned s) noexcept {
    static const char* const n[kStates] = {"off", "released_away", "released_item", "released_posture", "held_aim", "held_pose", "filling", "active", "held_sights", "active_item", "not_run"};
    return s < kStates ? n[s] : "?";
}
inline bool isReleased(unsigned s) noexcept { return s == kOff || s == kReleasedAway || s == kReleasedItem || s == kReleasedPosture; }
inline bool isActive(unsigned s) noexcept { return s == kActive || s == kActiveItem; }
// The states in which y holds while the camera moves on: a settle then would read the slope as a stance, so the settle is not taken.
inline bool settleHeld(unsigned s) noexcept { return s == kHeldAim || s == kHeldPose || s == kFilling || s == kHeldSights; }
struct In {
    bool keyOn = false;
    bool away = false;          // the game's camera is away from the commander
    bool inSights = false;      // pc.detachW over 0: the sights
    bool hipItem = false;       // an item drawn and not in the sights
    bool lowPosture = false;    // unarmed: pc.postureW at 0.5 or over, or the head joint not upright
    bool lowPostureItem = false;   // with an item at the hip: pc.postureW at 0.5 or over (the armed stances hold the head low, and postureW reads 0 for them)
    bool aimRefused = false;    // F138's aim gate refused the settle
    bool poseValid = false;     // the physical head's pose is known
    bool fresh = false;         // a new game frame since the last call
    double headPitchDeg = std::nan("");
    double tS = 0.0;
    double eyeY = std::nan("");        // E.y: the game's camera in the model, the head's pitch taken out (pc.eyeYSelected)
    double neckY = std::nan("");       // N0.y: the animated neck
    double eyeHeightM = 0.15;          // the eye height key
    double levelCamY = kDefaultLevelCamM;   // the level camera height for an item at the hip (levelCamY())
    double aimPitchDeg = std::nan("");      // pc.f86AimPitch when known
};
struct Out {
    State state = kNotRun;
    double y = 0.0;             // the shift in force after this call (m)
    double targetY = 0.0;       // where it is going (m): 0 released, y held, the rule's value active
    double r0M = 0.0;           // the constant raise R0
    bool headPitched = false;   // F226: the call reached the place the head gate stood (key on, not away, not in the sights, no low posture, aim and pose clear) with the head at or over kHeadGateDeg: the settle is refused
    bool active() const noexcept { return isActive(state); }
};

struct Follow {
    f214::Mean win;
    double y = 0.0, lastT = 0.0;
    bool haveT = false, catching = false;
    State last = kNotRun;
    bool lastHeadPitched = false;
    void reset() noexcept {
        lastHeadPitched = false;
        win.clear();
        y = lastT = 0.0;
        haveT = false;
        catching = false;
        last = kNotRun;
    }
    Out step(const In& in) noexcept {
        Out o;
        o.r0M = levelRaiseM(in.eyeHeightM);
        if (!in.fresh) {   // no new game frame: the last output, no change of state
            o.state = last;
            o.y = y;
            o.targetY = y;
            o.headPitched = lastHeadPitched;
            return o;
        }
        double dt = 0.0;
        if (std::isfinite(in.tS)) {
            if (haveT && in.tS > lastT) dt = in.tS - lastT;
            if (dt > kMaxDtS) dt = kMaxDtS;
            lastT = in.tS;
            haveT = true;
        }
        State s;
        double target = y;
        bool headPitched = false;
        if (!in.keyOn) {
            s = kOff;
            target = 0.0;
            win.clear();
        } else if (in.away || !std::isfinite(in.eyeY)) {
            s = kReleasedAway;
            target = 0.0;
            win.clear();
        } else if (in.inSights) {
            s = kHeldSights;
            win.clear();
        } else if (in.hipItem) {
            win.clear();
            if (in.lowPostureItem) {
                s = kReleasedPosture;
                target = 0.0;
            } else if (std::isfinite(in.aimPitchDeg) && in.aimPitchDeg >= kAimUpHoldDeg) {
                s = kHeldAim;
            } else if (!in.poseValid || !std::isfinite(in.headPitchDeg)) {
                s = kHeldPose;
            } else {
                headPitched = !(std::fabs(in.headPitchDeg) < kHeadGateDeg);
                s = kActiveItem;
                target = in.eyeY - in.levelCamY;
                target = target > kCapM ? kCapM : target < -kCapM ? -kCapM : target;
            }
        } else if (in.lowPosture) {
            s = kReleasedPosture;
            target = 0.0;
            win.clear();
        } else if (in.aimRefused) {
            s = kHeldAim;
        } else if (!in.poseValid || !std::isfinite(in.headPitchDeg)) {
            s = kHeldPose;
        } else {
            headPitched = !(std::fabs(in.headPitchDeg) < kHeadGateDeg);
            win.add(in.tS, in.neckY);
            if (win.n < 2 || win.spanS() < kFillS - 1e-9) {
                s = kFilling;
            } else {
                s = kActive;
                target = in.eyeY - win.mean() - kCamOverNeckM;
                target = target > kCapM ? kCapM : target < -kCapM ? -kCapM : target;
            }
        }
        if (s != last && (isActive(s) || isActive(last) || isReleased(s))) catching = true;
        last = s;
        lastHeadPitched = headPitched;
        const double rate = catching ? kCatchMps : kTrackMps;
        const double step = rate * dt;
        const double diff = target - y;
        y += diff > step ? step : diff < -step ? -step : diff;
        if (isActive(s) && std::fabs(target - y) < kCatchDoneM) catching = false;
        o.state = s;
        o.y = y;
        o.targetY = target;
        o.headPitched = headPitched;
        return o;
    }
};

// ---- the log window ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Call {
    State state = kNotRun;
    bool keyOn = false;
    bool unarmed = false;     // nothing drawn, whatever the key
    bool headPitched = false; // F226: the head was past the gate and the move on (Out::headPitched)
    double yM = 0.0;          // the shift in force after the call
    double r0M = std::nan("");
};
struct Stats {
    uint32_t calls = 0, unarmed = 0, active = 0, headPitched = 0;
    uint32_t byState[kStates] = {};
    double ySum = 0.0, yMin = 0.0, yMax = 0.0;   // over the active calls
    double r0M = std::nan("");
    bool keyOn = false;
    void add(const Call& c) noexcept {
        ++calls;
        keyOn = c.keyOn;
        if (c.unarmed) ++unarmed;
        if (c.headPitched) ++headPitched;
        if (c.state < kStates) ++byState[c.state];
        if (std::isfinite(c.r0M)) r0M = c.r0M;
        if (c.state == kActive) {
            if (!active || c.yM < yMin) yMin = c.yM;
            if (!active || c.yM > yMax) yMax = c.yM;
            ySum += c.yM;
            ++active;
        }
    }
    bool any() const noexcept { return calls > 0; }
    void clear() noexcept { *this = Stats{}; }
};

// The window's line. A reader tells the key off, a key on that never ran (no active call), and a rule that ran with a shift near 0 (active calls with a mean near 0) apart; shifted is the count
// of pose writes that carried a shift (a rule that was active but never wrote a shifted pose is a rule that did nothing). tools\edvr_log.py reads it.
inline int formatLine(char* buf, size_t n, uint32_t seconds, const Stats& s, uint32_t shifted) noexcept {
    char mv[112];
    if (s.active) std::snprintf(mv, sizeof(mv), "mean %+.1f cm (min %+.1f, max %+.1f) over %u active calls", s.ySum / s.active * 100.0, s.yMin * 100.0, s.yMax * 100.0, s.active);
    else std::snprintf(mv, sizeof(mv), "none");
    return std::snprintf(buf, n,
                         "native on foot body follows view (F215) %u s: experimental.native_on_foot_body_follows_view is %s; pin calls %u, unarmed %u, active %u, head past the gate with the move on %u, held aim %u pose %u, filling %u, "
                         "released item %u posture %u away %u; body move %s; raise R0 %.1f cm; poses written shifted %u; item at the hip active %u, held in the sights %u",
                         seconds, s.keyOn ? "on" : "off", s.calls, s.unarmed, s.active, s.headPitched, s.byState[kHeldAim], s.byState[kHeldPose], s.byState[kFilling],
                         s.byState[kReleasedItem], s.byState[kReleasedPosture], s.byState[kReleasedAway], mv, std::isfinite(s.r0M) ? s.r0M * 100.0 : 0.0, shifted, s.byState[kActiveItem], s.byState[kHeldSights]);
}

// ---- the shifted write ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// A joint matrix with y added to its y translation (element 13), the one place the shift reaches the game's pose. y = 0 gives the matrix unchanged, bit for bit.
inline void shiftedMatrix(const float* src, double y, float out[16]) noexcept {
    for (int i = 0; i < 16; ++i) out[i] = src[i];
    if (y != 0.0) out[13] = static_cast<float>(static_cast<double>(src[13]) + y);
}

// The world-to-model matrix (or G) less the body's shift, so what is worked out from the true camera enters the internal space (the model's with the body unshifted). Row-vector convention,
// p' = p x m: element 13 is the y translation. y = 0 leaves it unchanged.
inline void shiftWorldToModel(float* m, double y) noexcept {
    if (y != 0.0) m[13] = static_cast<float>(static_cast<double>(m[13]) - y);
}

}  // namespace f215
}  // namespace nofoot
}  // namespace edvr
