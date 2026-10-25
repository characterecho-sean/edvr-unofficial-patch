// F109 (native on-foot VR): look to aim on foot, the player's own turn in ADS, snap or smooth turning. Header only, no game or D3D11 state, so native_frame_test
// runs the very functions the hooks and the frame thread use. Journal: the arc doc, 2026-10-25, F109. RE: analysis\decomp\verify_20261025_onfoot_look_input.txt
// (build 334925).
//
// The game's look, as read in build 334925: the look update (PlayerHumanoidControlActivity, 0x1999190) reads four actions, the right stick's yaw and pitch
// (act+0x180, +0x188) and the mouse's (act+0x190, +0x198), each a float at +0x18, and hands them to an input sink; the processor (0x1A9EB50) sums, per call,
//     yaw   = c * (dt * rateA * stick yaw + g * mouse yaw + dt * spring yaw rate)
//     pitch = c * (dt * rateB * stick pitch + g * mouse pitch - dt * spring pitch rate)
// with g the mouse path's gain (linear: the hip and ADS ends of one gain, blended by the zoom) and the springs a game-driven turn (comp+0x780, +0x79C), and the aim
// takes the whole delta in that call (0x18FF1E0). EDVR's head aim writes its input into the mouse's actions for one update call (F62b), so
//  - the split of the game's delta into EDVR's share and the player's must count only the mouse path's gain times EDVR's input as EDVR's: the stick and the springs
//    are the player's (or the game's) own. F62b split the delta by the mouse input alone (ours = d * in / (mouse + in)), which counted a stick turn as EDVR's
//    whenever the head aim wrote that call: the cancel then held the view while the avatar turned (a tester's report, ADS, the right stick). F109 takes the gain only
//    from clean calls (no stick, no spring) and EDVR's share as gain * in on every call (Split);
//  - with look to aim on, the update hook runs the look with the player's look dropped as the turn style says (feed): the head is the only look; snap keeps a turn
//    of one step for a push of the stick or a flick of the mouse, smooth leaves the stick's yaw and the mouse's horizontal to turn the player continuously. Both are
//    the player's own turn: the split leaves them out of the cancel, so the view turns with the body.
#pragma once

#include "native_on_foot_core.h"   // AdsInFlight (the follower's in-flight account), adsHeadAngles

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f109 {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

inline bool fin(double v) noexcept { return std::isfinite(v); }
inline double clampd(double v, double lo, double hi) noexcept { return !(v == v) ? 0.0 : v < lo ? lo : v > hi ? hi : v; }

// ---- the split ------------------------------------------------------------------------------------------------------------------------------------------
// One axis of one processor call. in: the head aim's input written into the mouse's action (game input units); mouse: the player's mouse value the call ran with
// (after a drop); stick: the stick's value the call ran with (after a drop); springTerm: dt times the spring's rate (radians, the sign it enters the sum with);
// d: the game's delta for the axis (comp+0x624 / +0x628).
// gainLo, gainHi: the mouse path's gain at its two ends as the game holds them (comp+0x808 / +0x80C; 0 and 0: unread).
struct SplitIn {
    double in = 0.0, mouse = 0.0, stick = 0.0, springTerm = 0.0, d = 0.0;
    double gainLo = 0.0, gainHi = 0.0;
};
struct SplitOut {
    double ours = 0.0, player = 0.0;   // the game's delta, EDVR's share and the rest (radians, the game's sign)
    bool clean = false;                // no stick and no spring: the call's delta is the mouse path's alone
    bool gainTaken = false;            // ... and the gain was taken from it
    bool foreign = false;              // F109c: a clean call whose delta / input is not a gain between the game's two ends (a term of the game's own): not taken
    bool blocked = false;              // EDVR wrote, the game applied nothing and no spring ran: the game's own look block (it zeroes the look in the update)
    bool unsplit = false;              // a call with a stick or a spring before any clean call: split by the mouse ratio (F62b's rule), counted
};
constexpr double kGainMin = 1e-3, kGainMax = 1e3;   // with the ends unread, a measured gain outside this (or not finite) is not taken
constexpr double kSpringIdleRad = 1e-7;            // a spring term under this (radians a call) is idle
// F109c. The mouse path's gain is lerp(comp+0x808, comp+0x80C, the zoom blend) (RE, verify_20261025_onfoot_look_input): it lies between those two ends and has
// their sign. The F109b flight (edvr_gfx_20261025_085120, the Arc Cutter, 08:57:09-10) took -405.4 (yaw), 741.1 and 22.2 (pitch) from clean calls whose delta held
// a term of the game's own, against 0.85 to 1.00 in every other second; the head aim's asks then went to the game divided by them. A measured gain is taken only
// between the ends, kGainEndTol either side; with the ends unread (both 0, not finite, or of opposite signs), F109's window.
constexpr double kGainEndTol = 0.05;
inline bool gainWithinEnds(double g, double lo, double hi) noexcept {
    if (!fin(g)) return false;
    if (!fin(lo) || !fin(hi) || (lo == 0.0 && hi == 0.0) || lo * hi < 0.0) return std::fabs(g) >= kGainMin && std::fabs(g) <= kGainMax;
    const double a = std::fabs(lo), b = std::fabs(hi);
    const double mn = a == 0.0 ? b : b == 0.0 ? a : (a < b ? a : b), mx = a > b ? a : b;
    const double sign = (lo != 0.0 ? lo : hi) > 0.0 ? 1.0 : -1.0;
    return g * sign > 0.0 && std::fabs(g) >= mn * (1.0 - kGainEndTol) && std::fabs(g) <= mx * (1.0 + kGainEndTol);
}
struct Split {
    double gain = 0.0;   // the mouse path's gain (the game's radians per input unit, its sign included), from clean calls only; 0 = none measured yet
    void reset() noexcept { gain = 0.0; }
    SplitOut step(const SplitIn& s) noexcept {
        SplitOut o;
        if (!fin(s.in) || !fin(s.mouse) || !fin(s.stick) || !fin(s.springTerm) || !fin(s.d)) {
            o.player = fin(s.d) ? s.d : 0.0;
            return o;
        }
        const double sum = s.mouse + s.in;
        const bool springIdle = std::fabs(s.springTerm) < kSpringIdleRad;
        if (s.in != 0.0 && s.d == 0.0 && springIdle) {
            o.blocked = true;
            return o;
        }
        o.clean = s.stick == 0.0 && springIdle && std::fabs(sum) > 1e-12 && s.d != 0.0;
        if (o.clean) {
            const double g = s.d / sum;
            if (gainWithinEnds(g, s.gainLo, s.gainHi)) {
                gain = g;
                o.gainTaken = true;
            } else {
                o.foreign = true;
            }
            o.ours = s.d * (s.in / sum);
        } else if (s.in != 0.0) {
            if (gain != 0.0) {
                o.ours = gain * s.in;
            } else {
                o.ours = std::fabs(sum) > 1e-12 ? s.d * (s.in / sum) : 0.0;
                o.unsplit = true;
            }
        }
        o.player = s.d - o.ours;
        return o;
    }
};

// ---- the turn style and the drop -------------------------------------------------------------------------------------------------------------------------
enum TurnStyle : int { kSnap = 0, kSmooth = 1 };
inline int turnStyleFromKey(const char* v) noexcept { return v && std::strcmp(v, "smooth") == 0 ? kSmooth : kSnap; }
inline const char* turnStyleName(int s) noexcept { return s == kSmooth ? "smooth" : "snap"; }
constexpr int kSnapDegMin = 15, kSnapDegMax = 90, kSnapDegDefault = 45;
inline int snapDegClamp(int v) noexcept { return v < kSnapDegMin ? kSnapDegMin : v > kSnapDegMax ? kSnapDegMax : v; }

// The values the game's look update runs with. drop: look to aim is engaged (the head is the look): the stick's and the mouse's pitch go; snap also takes the stick's
// yaw and the mouse's horizontal (they only trigger snaps), smooth leaves them to turn the player. snapIn (game input units) rides the mouse's yaw as the player's
// own turn. Not dropping is the game's own look, unchanged.
struct Feed {
    double stickYaw = 0.0, stickPitch = 0.0, mouseYaw = 0.0, mousePitch = 0.0;
};
inline Feed feed(bool drop, int style, double stickYaw, double stickPitch, double mouseYaw, double mousePitch, double snapIn) noexcept {
    Feed f{stickYaw, stickPitch, mouseYaw, mousePitch};
    if (!drop) return f;
    f.stickPitch = 0.0;
    f.mousePitch = 0.0;
    if (style != kSmooth) {
        f.stickYaw = 0.0;
        f.mouseYaw = 0.0;
    }
    f.mouseYaw += fin(snapIn) ? snapIn : 0.0;
    return f;
}

// ---- the follower (look to aim) --------------------------------------------------------------------------------------------------------------------------
// The aim follows the head through F68a's map: the target is the head's own turn since the engage (the LOCAL head's steps) plus the centring (the reported head's
// offset from the aim, once, at each engage); the ask each tick is alpha of what is left, the target less the turn the game applied for EDVR's asks (the split's
// share, exactly what the map cancels) less what is still in flight. An ask the game drops is asked again once it leaves the in-flight account: nothing is lost.
// The view is the headset's whatever the ask is (the cancel is of what the game applied), so the smoothing and the rate cap shape the body and the weapon only.
constexpr double kFollowRateRad = 180.0 * kDeg;   // the most the follower asks a second, each axis
// The smoothing's time constant: smoothing_ms with nothing drawn, ramped out by F88's armed weight (a weapon or tool drawn) and by the ADS weight: 0 in ADS.
inline double followerTau(double smoothingMs, double armedW, double adsW) noexcept {
    const double a = clampd(armedW, 0.0, 1.0), s = clampd(adsW, 0.0, 1.0);
    const double w = a > s ? a : s;
    const double ms = fin(smoothingMs) && smoothingMs > 0.0 ? smoothingMs : 0.0;
    return ms / 1000.0 * (1.0 - w);
}
inline double followerAlpha(double dt, double tau) noexcept {
    if (!(tau > 1e-6)) return 1.0;
    const double t = dt > 0.0 ? dt : 0.0;
    return 1.0 - std::exp(-t / tau);
}
// The in-flight account after this tick's applied turn is taken off it (the map's step does the same before it asks).
inline double inFlightAfter(AdsInFlight f, double applied) noexcept {
    f.tick();
    f.apply(applied);
    return f.sum();
}
struct Follower {
    double target[2] = {};    // seat radians (yaw + left, pitch + up): the head's turn since the engage plus the centring
    double applied[2] = {};   // the game's turn for EDVR's asks since the engage (the split's share, in the seat convention)
    void reset() noexcept { *this = Follower{}; }
    void addHead(double dYaw, double dPitch) noexcept {
        if (fin(dYaw)) target[0] += dYaw;
        if (fin(dPitch)) target[1] += dPitch;
    }
    void addApplied(double yaw, double pitch) noexcept {
        if (fin(yaw)) applied[0] += yaw;
        if (fin(pitch)) applied[1] += pitch;
    }
    // What is left to ask on an axis, with inFlight the account after this tick's applied turn.
    double left(int axis, double inFlight) const noexcept { return target[axis] - applied[axis] - (fin(inFlight) ? inFlight : 0.0); }
    // The ask this tick (seat radians), each axis held to the rate cap.
    void ask(double dt, double tau, double inFlightYaw, double inFlightPitch, double* askYaw, double* askPitch) const noexcept {
        const double a = followerAlpha(dt, tau), cap = kFollowRateRad * (dt > 0.0 ? dt : 0.0);
        const double y = clampd(a * left(0, inFlightYaw), -cap, cap), p = clampd(a * left(1, inFlightPitch), -cap, cap);
        *askYaw = fin(y) ? y : 0.0;
        *askPitch = fin(p) ? p : 0.0;
    }
};
// The look blocked by the game: this many processor calls in a row dropped an ask, and the follower holds (one probe of at most kProbeRad every kProbeMs finds
// the end of it); the follower's target keeps the head's turn, so the aim catches up when the game takes the look again.
constexpr uint32_t kBlockedCalls = 3;
constexpr uint64_t kProbeMs = 100;
constexpr double kProbeRad = 1.0 * kDeg;
// The centring at an engage: the reported head's offset from the aim (the render head in the seated space: the game composes its camera as the aim turned by the
// reported head, F61), in the seat convention; an offset under kCentreMinDeg is left alone, one over kCentreMaxDeg (the head turned away) too.
constexpr double kCentreMinDeg = 0.5, kCentreMaxDeg = 120.0;
inline bool centreOffset(const float render[12], double* yawRad, double* pitchRad) noexcept {
    double y = 0.0, p = 0.0;
    if (!adsHeadAngles(render, &y, &p)) return false;
    *yawRad = y;
    *pitchRad = p;
    return true;
}

// ---- the snap detectors ----------------------------------------------------------------------------------------------------------------------------------
// The stick: a push past kStickFire turns one step (one per push), the next only after the stick came back under kStickRearm. A stick already pushed when the
// detector starts does not fire until it has been near the centre.
constexpr double kStickFire = 0.70, kStickRearm = 0.25;
struct StickSnap {
    bool armed = false;
    void reset() noexcept { armed = false; }
    // +1 a step to the right (the game's yaw sign of a positive stick), -1 to the left, 0 none.
    int step(double x) noexcept {
        if (!fin(x)) return 0;
        const double a = std::fabs(x);
        if (!armed) {
            if (a < kStickRearm) armed = true;
            return 0;
        }
        if (a > kStickFire) {
            armed = false;
            return x > 0.0 ? 1 : -1;
        }
        return 0;
    }
};
// The mouse: a deliberate horizontal flick, kFlickDeg of would-be turn (the mouse's input through the measured gain) within kFlickWindowS, turns one step; then a
// cooldown of kFlickCooldownS, and the next only once the window's turn fell under a third of kFlickDeg (one snap for one long swipe).
constexpr double kFlickDeg = 20.0, kFlickWindowS = 0.150, kFlickCooldownS = 0.400, kFlickRearmDeg = kFlickDeg / 3.0;
struct MouseFlick {
    static constexpr int kRing = 64;
    double t[kRing] = {}, deg[kRing] = {};
    int n = 0, head = 0;
    bool armed = true;
    double coolUntil = 0.0;
    void reset() noexcept { *this = MouseFlick{}; }
    double windowSum(double now) const noexcept {
        double s = 0.0;
        for (int i = 0; i < n; ++i) {
            const int k = (head - 1 - i + kRing) % kRing;
            if (now - t[k] <= kFlickWindowS) s += deg[k];
        }
        return s;
    }
    // now: seconds; addDeg: this tick's would-be turn (degrees, the game's yaw sign). +1 / -1 a step, 0 none.
    int step(double now, double addDeg) noexcept {
        if (fin(addDeg) && addDeg != 0.0) {
            t[head] = now;
            deg[head] = addDeg;
            head = (head + 1) % kRing;
            if (n < kRing) ++n;
        }
        const double sum = windowSum(now);
        if (!armed) {
            if (now >= coolUntil && std::fabs(sum) < kFlickRearmDeg) armed = true;
            return 0;
        }
        if (now < coolUntil) return 0;
        if (std::fabs(sum) >= kFlickDeg) {
            armed = false;
            coolUntil = now + kFlickCooldownS;
            n = 0;
            return sum > 0.0 ? 1 : -1;
        }
        return 0;
    }
};

// ---- the snap's run: the comfort fade around the turn ------------------------------------------------------------------------------------------------------
// The view goes dark over kFadeInS, the turn is injected once it is black, held dark until the game applied it (at least kHoldMinS after the injection, at most
// kHoldMaxS), and comes back over kFadeOutS.
constexpr double kFadeInS = 0.020, kHoldMinS = 0.025, kHoldMaxS = 0.100, kFadeOutS = 0.030;
struct SnapRun {
    enum Phase : int { kIdle = 0, kFadeIn = 1, kHold = 2, kFadeOut = 3 };
    int phase = kIdle;
    int dir = 0;
    double t0 = 0.0, tInject = 0.0, tOut = 0.0;
    bool begin(double now, int d) noexcept {
        if (phase != kIdle || d == 0) return false;
        phase = kFadeIn;
        dir = d;
        t0 = now;
        return true;
    }
    // The fade level this tick (0 clear .. 1 black); *inject is set on the one tick the turn goes in; applied: the game applied it (the split saw the snap's share).
    double step(double now, bool applied, bool* inject) noexcept {
        *inject = false;
        switch (phase) {
        case kFadeIn: {
            const double l = (now - t0) / kFadeInS;
            if (l >= 1.0) {
                phase = kHold;
                tInject = now;
                *inject = true;
                return 1.0;
            }
            return clampd(l, 0.0, 1.0);
        }
        case kHold:
            if ((applied && now - tInject >= kHoldMinS) || now - tInject >= kHoldMaxS) {
                phase = kFadeOut;
                tOut = now;
            }
            return 1.0;
        case kFadeOut: {
            const double l = 1.0 - (now - tOut) / kFadeOutS;
            if (l <= 0.0) {
                phase = kIdle;
                return 0.0;
            }
            return l;
        }
        default:
            return 0.0;
        }
    }
};

// ---- the player's turn reached the view ------------------------------------------------------------------------------------------------------------------
// Over one second: the avatar's turn (the game's aim yaw, comp+0x7B8) less the head aim's asks is the player's own (stick, mouse, snap, spring); the view's turn less
// the head's own (the right eye's rows less the LOCAL head) is what reached the view beyond the head. They agree within kAgreeDeg or a quarter of the turn. A second
// whose player turn is under kTurnMinDeg says nothing.
constexpr double kTurnMinDeg = 3.0, kAgreeDeg = 1.0;
inline bool turnAgrees(double playerDeg, double viewDeg) noexcept {
    const double tol = std::fabs(playerDeg) * 0.25 > kAgreeDeg ? std::fabs(playerDeg) * 0.25 : kAgreeDeg;
    return fin(playerDeg) && fin(viewDeg) && std::fabs(viewDeg - playerDeg) <= tol;
}
// The aim's yaw step between two reads of comp+0x7B8 (radians, wrapped to +-pi).
inline double aimStep(double before, double now) noexcept {
    if (!fin(before) || !fin(now)) return 0.0;
    double d = now - before;
    while (d > kPi) d -= 2.0 * kPi;
    while (d < -kPi) d += 2.0 * kPi;
    return d;
}

}  // namespace f109
}  // namespace nofoot
}  // namespace edvr
