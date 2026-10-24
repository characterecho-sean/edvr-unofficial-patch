// F94a (native on-foot VR): the commander's neck is the source of truth for the view's height. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the hooks and the frame thread use. Journal: the arc doc, 2026-10-24, F94a.
//
// Until F94a the view stood at the game's own first-person camera (F87h holds the head's lasting posture out of it) and the body pin moved the commander's upper body
// until the animated neck sat 15.5 cm under that camera. The game's camera stands 4 cm over the neck of the upright stances (fists, tools, empty hands: neck 1.543 m,
// camera 1.58 m looking level) and 17-26 cm over the neck of the leaning weapon stances, so the pin lowered the upright body by about 12 cm (the knees bent; F93e
// made the hidden head take it instead, which put the eye low and forward of the neck: "no neck") and raised the weapon stances by 4-11 cm (the legs stretched).
//
// F94a turns it round. The body stands at its animated height in every stance: the pin's neck target keeps the animated neck's height (bodyKeepsNeckHeight), so
// nothing lowers or raises it and the legs are the animation's. The view's height comes from the neck instead: unarmed, the eye sits the key's height
// (fix.native_on_foot_eye_height_cm, the commander's own neck-to-eye height by default) over the animated neck with the head level, and the head's pitch still pivots
// it about the neck as before. That is a raise of the view R = neck + h - (the game's camera with the head level), carried by the eye alignment's y like the
// hold's (the runtime moves the seated origin; seated y = -R raises the camera by R along the character's up).
//
// The loop. R is measured on the camera the game drew, which already carries the R the runtime applied a frame or two earlier. The P1 hook therefore takes out of
// the camera the R the pose carried, found by the alignment serial the runtime reports with each pose (F89's word; SeatedHistory below, lock-free because P1 is
// not the frame thread): the measured camera is then the game's own and R's target does not contain R (open at any delay). A runtime that reports no serial gets
// the R published last (the measurement carries R's last change: the filter below keeps that loop stable at one and two frames of delay; the rig checks it).
//
// The weapon. With a weapon or tool drawn the weapon eye offset (F88) places the eye relative to the item, which the game places relative to its own camera: the
// view is the game's camera plus w O. The neck reference is weighted by 1 - w, so armed it is exactly zero and F88's armed view is the same as before; drawing and
// holstering blend the two over F88's 0.25 s ramp. While w is above 0 the raise's target is frozen (the stance changes under the draw would otherwise pull the
// blend down mid-ramp); it follows again from w 0, through the filter, so a holster in another stance moves the view in about kSettleS and does not jump.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f94 {

// The key: how far the eyes sit above the commander's animated neck with the head level, cm. The default is the commander's own: in the F93 flight (log
// edvr_gfx_20261024_124854, the pin's calibration at 12:50:10) the animated head joint 44 stood 11.7 cm over the neck joint 34 standing level (1.660 m against
// 1.543 m) and the calibrated eye point sits 3.9 cm up the head's frame from it (e_h 0.0033, 0.0391, 0.0381 m): 15.5-15.6 cm, the rest offset's 15.5.
constexpr double kEyeHeightDefaultCm = 15.5;
constexpr double kEyeHeightMinCm = 0.0, kEyeHeightMaxCm = 30.0;
inline double eyeHeightClampCm(double cm) noexcept {
    return !std::isfinite(cm) ? kEyeHeightDefaultCm : cm < kEyeHeightMinCm ? kEyeHeightMinCm : cm > kEyeHeightMaxCm ? kEyeHeightMaxCm : cm;
}

constexpr double kSettleS = 0.2;          // the raise follows its target critically damped, 95% in this long (a crouch, a jump's dip)
constexpr double kCritical95 = 4.743898;  // omega t for 1 - (1 + wt) exp(-wt) = 0.95
constexpr double kRampS = 0.5;            // in and out of the pin's scope, as the hold
constexpr double kMaxM = 0.5;             // the raise's bound either way (the target is clamped; the line counts it)
constexpr double kStepM = 0.001;          // published to the millimetre
constexpr uint64_t kTargetMaxAgeMs = 250; // a target the hook has not renewed in this long is not followed (the raise holds)

// The raise the view needs so the eye stands eyeHeightM over the animated neck: neckY is the animated neck's height in the model, eyeLevelY the game's camera's height
// in the model with the head's pitch taken out (pinEyeHeightNormalized) and everything EDVR adds to the camera taken out (the transient, the shifts the pin ignores, the
// raise the camera carried).
inline double raiseTarget(double neckY, double eyeHeightM, double eyeLevelY) noexcept { return neckY + eyeHeightM - eyeLevelY; }

// F94a's body: the pin's neck target keeps the animated neck's height (no lowering, no raise in any stance). Returns the vertical move the target asked for.
inline double bodyKeepsNeckHeight(double target[3], const double n0[3]) noexcept {
    const double asked = target[1] - n0[1];
    target[1] = n0[1];
    return std::isfinite(asked) ? asked : 0.0;
}

struct In {
    bool scope = false;        // the pin's scope (native stereo on foot, the full third-person body pinned)
    bool targetFresh = false;  // the hook renewed the target within kTargetMaxAgeMs
    double targetM = 0.0;      // raiseTarget of the newest pinned call
    double weaponW = 0.0;      // F88's weight (1 with a weapon or tool drawn)
    double dt = 0.0;
    double settleS = kSettleS; // the filter's 95% time (F95a passes the standing-height hold's, at least nofoot::f95::kSettleMinS)
};
struct Out {
    double raiseM = 0.0;     // the neck reference's raise (ramped, quantised): the view over the game's camera unarmed
    double appliedM = 0.0;   // (1 - w) raise, quantised: what goes into the view
    double seatedY = 0.0;    // the eye alignment's y term: -appliedM (an exact positive zero when nothing is applied)
    double ramp = 0.0;
    bool frozen = false;     // the target is held: a weapon or tool is drawn (w above 0)
    bool clamped = false;    // the target was past kMaxM
    bool following = false;  // a fresh target was taken this tick
};
struct NeckRaise {
    double x = 0.0, v = 0.0, ramp = 0.0, target = 0.0;
    void reset() noexcept { *this = NeckRaise{}; }
    Out step(const In& in) noexcept {
        Out o;
        double dt = in.dt;
        dt = !(dt > 0.0) ? 0.0 : dt > 0.25 ? 0.25 : dt;
        ramp = in.scope ? std::fmin(1.0, ramp + dt / kRampS) : std::fmax(0.0, ramp - dt / kRampS);
        if (!in.scope && ramp <= 0.0) {
            x = v = target = 0.0;
        }
        const double w = !(in.weaponW > 0.0) ? 0.0 : in.weaponW > 1.0 ? 1.0 : in.weaponW;
        o.frozen = w > 0.0;
        if (in.scope && in.targetFresh && !o.frozen && std::isfinite(in.targetM)) {
            double t = in.targetM;
            if (t > kMaxM || t < -kMaxM) {
                t = t > 0.0 ? kMaxM : -kMaxM;
                o.clamped = true;
            }
            target = t;
            o.following = true;
        }
        if (dt > 0.0) {   // the filter runs in and out of the scope (out of it the ramp takes the raise down)
            const double settle = std::isfinite(in.settleS) && in.settleS >= 0.01 ? in.settleS : kSettleS;
            const double om = kCritical95 / settle, e = x - target, c = v + om * e, ex = std::exp(-om * dt);
            x = target + (e + c * dt) * ex;
            v = (v - om * c * dt) * ex;
        }
        o.ramp = ramp;
        double raise = ramp * x;
        raise = raise > kMaxM ? kMaxM : raise < -kMaxM ? -kMaxM : raise;
        raise = std::round(raise / kStepM) * kStepM;
        o.raiseM = raise == 0.0 ? 0.0 : raise;
        double applied = std::round((1.0 - w) * o.raiseM / kStepM) * kStepM;
        o.appliedM = applied == 0.0 ? 0.0 : applied;
        o.seatedY = o.appliedM == 0.0 ? 0.0 : -o.appliedM;
        return o;
    }
};

// The neck reference's part of every eye alignment published, by serial: the frame thread writes (f89AlignPublish), the P1 hook reads on another thread, so each
// slot is one atomic word: the serial in the high half, the float bits of the seated y in the low half. Serial 0 is the zero alignment before any publication.
struct SeatedHistory {
    static constexpr uint32_t kN = 256;
    std::atomic<uint64_t> slot[kN];
    std::atomic<uint32_t> newest{0};
    SeatedHistory() noexcept {
        for (uint32_t i = 0; i < kN; ++i) slot[i].store(~0ull, std::memory_order_relaxed);
    }
    static uint64_t pack(uint32_t serial, double y) noexcept {
        const float f = static_cast<float>(y);
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        return (static_cast<uint64_t>(serial) << 32) | bits;
    }
    void record(uint32_t serial, double seatedY) noexcept {
        slot[serial % kN].store(pack(serial, seatedY), std::memory_order_release);
        newest.store(serial, std::memory_order_release);
    }
    struct Found {
        bool reported = false;   // the runtime reported a serial (a word above 0)
        bool matched = false;    // ... and the history still holds it
        double seatedY = 0.0;
    };
    // word: the frame input's applied-alignment word (0: not reported, else the serial plus 1).
    Found find(uint32_t word) const noexcept {
        Found f;
        if (word == 0) return f;
        f.reported = true;
        const uint32_t serial = word - 1u;
        if (serial == 0) {
            f.matched = true;
            return f;
        }
        const uint64_t v = slot[serial % kN].load(std::memory_order_acquire);
        if (static_cast<uint32_t>(v >> 32) != serial) return f;
        const uint32_t bits = static_cast<uint32_t>(v);
        float y = 0.0f;
        std::memcpy(&y, &bits, sizeof(y));
        f.matched = std::isfinite(y);
        f.seatedY = f.matched ? static_cast<double>(y) : 0.0;
        return f;
    }
};
// Where the P1 hook's raise came from: the serial the pose carried (matched), the last published (the runtime reports none), or the last published again because
// the reported serial is no longer in the history (missed).
enum Source : uint8_t { kSrcMatched = 0, kSrcLegacy = 1, kSrcMissed = 2 };

}  // namespace f94
}  // namespace nofoot
}  // namespace edvr
