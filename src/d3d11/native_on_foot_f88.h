// F88 (native on-foot VR): the pure parts of the weapon eye offset, live, menu-adjustable eye offsets relative to the held weapon (a). Design: the arc doc, F88;
// journal: the arc doc, 2026-10-24, F88.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks use.
//
// The game places the first-person weapon relative to the character, not to the headset, so moving the eye moves it relative to the weapon. The offset O = (right, up,
// forward) is in the character's axes (the AMC's right, up and forward: the axes of the "eye to weapon root (F87h)" line), so it turns with the body's yaw and not with the
// head. It rides the eye alignment's seated translation: the camera moves by (-S0, -S1, +S2) in those axes for a seated shift S (F40's plant, as the ADS lock uses it), so the
// seated shift for a camera move O is (-O0, -O1, +O2). The weight w is 1 while a weapon or tool is drawn at the hip, 0 unarmed, ramped over kRampS on each change. F89: w does
// not depend on ADS. The F70 sight lock measures the rendered eye and sets its shift from the error to P14w + k f, so O under it only changes the shift the lock has to find and
// the eye still lands on the sight; and on release the lock's shift runs down to zero and leaves the eye at the offset eye, with no dip to the un-offset eye in between (F88
// zeroed w in ADS, which dropped the eye to the un-offset position the moment the sights came down and ramped it back once the journal's ADS flag cleared).
//
// The body pin takes the camera less the ADS shift S (nofoot::adsUnlockedCam). The pin ignores w O through the same mechanism: its seated image is added to S. stay: all of
// it, so the body stays where it would be and only the view moves. follow: the vertical part only, so the body moves with the eye horizontally, as when the pilot leans, and
// never stands taller than its standing height. In ADS the pin ignores the sight lock's shift beside it, and the same share of w O, so the pin's demand is the same at the hip
// and in the sights.
//
// The standing-height hold (native_on_foot_f87.h) measures the head's posture as the render head plus the eye alignment published for it; w O is part of both, so it cancels
// (devOf). Until F89 the runtime applied a published value a frame or two later than the hold assumed, and a moving w O showed as a difference for that long: the hold waited
// while the published up of w O was changing and kHoldFreezeS after (Settle). F89 matches the alignment the pose carries by its serial (native_on_foot_f89.h), so the
// difference is gone; Settle remains for a runtime that does not report the applied serial. The hold's output goes into the sum before w O is added to it, and w O never
// enters the hold.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f88 {

constexpr double kRampS = 0.25;          // w's ramp, either way: draw, holster, ADS in, ADS out
constexpr double kHoldFreezeS = 0.3;     // the hold's wait after the published up of w O last changed (the runtime applies it a frame or two later)
constexpr double kRightMinCm = -10.0, kRightMaxCm = 10.0;
constexpr double kUpMinCm = -10.0, kUpMaxCm = 15.0;
constexpr double kForwardMinCm = -10.0, kForwardMaxCm = 25.0;
// The keys' built-in values are the pilot's: F89 took his F88-flight values (forward 5.0, up 6.0, right 0.5 cm), F90 those of the F89 flight (2026-10-24): forward
// 9.0, up 7.5, right 0.0 cm, body follow. (Keys{} below stays all zero: a struct with no keys set; the config reader falls back to these.)
// F98c: forward 8.5 and right 2.0 are the pilot's; the up key is retired (kUpDefaultCm is its last shipped value, for the retired-key note).
constexpr double kForwardDefaultCm = 8.5, kUpDefaultCm = 7.5, kRightDefaultCm = 2.0;

// A key's value, held to its range; anything not finite reads 0.
inline double clampCm(double cm, double lo, double hi) noexcept { return std::isfinite(cm) ? std::min(hi, std::max(lo, cm)) : 0.0; }

enum Body : int { kFollow = 0, kStay = 1 };
// fix.native_on_foot_weapon_eye_body: follow | stay; anything else reads follow (known false).
inline int bodyFromText(const char* text, bool* known = nullptr) noexcept {
    const bool stay = text && text[0] == 's' && text[1] == 't' && text[2] == 'a' && text[3] == 'y' && !text[4];
    const bool follow = text && text[0] == 'f' && text[1] == 'o' && text[2] == 'l' && text[3] == 'l' && text[4] == 'o' && text[5] == 'w' && !text[6];
    if (known) *known = stay || follow;
    return stay ? kStay : kFollow;
}
inline const char* bodyName(int body) noexcept { return body == kStay ? "stay" : "follow"; }

struct Keys {
    double rightCm = 0.0, upCm = 0.0, forwardCm = 0.0;
    int body = kFollow;
    bool any() const noexcept { return rightCm != 0.0 || upCm != 0.0 || forwardCm != 0.0; }
};

// The weight's target: in the body pin's scope and a weapon or tool drawn (F89: aiming down sights does not change it).
inline bool weightOn(bool pinScope, bool armed) noexcept { return pinScope && armed; }
struct Weight {
    double w = 0.0;
    double step(bool on, double dt) noexcept {
        const double d = !(dt > 0.0) ? 0.0 : dt > 0.25 ? 0.25 : dt;
        w = on ? std::min(1.0, w + d / kRampS) : std::max(0.0, w - d / kRampS);
        return w;
    }
};

struct Out {
    double bodyM[3] = {};    // w O in the character's axes (right, up, forward), metres
    double seated[3] = {};   // added to the eye alignment (after the hold): (-right, -up, +forward)
    double ignore[3] = {};   // added to the ADS shift S for the pin: all of seated (stay), the up part alone (follow)
};
// Exact zeros (positive zeros) when w is 0 or every key is 0: the eye alignment's sum is then F87's, bit for bit.
inline Out make(const Keys& k, double w) noexcept {
    Out o;
    if (!(w > 0.0) || !k.any()) return o;
    const double wc = w > 1.0 ? 1.0 : w;
    o.bodyM[0] = wc * k.rightCm * 0.01;
    o.bodyM[1] = wc * k.upCm * 0.01;
    o.bodyM[2] = wc * k.forwardCm * 0.01;
    o.seated[0] = 0.0 - o.bodyM[0];
    o.seated[1] = 0.0 - o.bodyM[1];
    o.seated[2] = 0.0 + o.bodyM[2];
    if (k.body == kStay) {
        for (int i = 0; i < 3; ++i) o.ignore[i] = o.seated[i];
    } else {
        o.ignore[1] = o.seated[1];
    }
    return o;
}

// True while the published up of w O is changing and kHoldFreezeS after: the hold's filter waits (its output holds). Never true while that up stays 0.
struct Settle {
    double lastY = 0.0, sinceS = 1e9;
    bool step(double seatedY, double dt) noexcept {
        if (std::fabs(seatedY - lastY) > 1e-9) sinceS = 0.0;
        else if (dt > 0.0) sinceS += dt;
        lastY = seatedY;
        return sinceS < kHoldFreezeS;
    }
};

}  // namespace f88
}  // namespace nofoot
}  // namespace edvr
