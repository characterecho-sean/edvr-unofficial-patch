// F107 (native on-foot VR): the game's camera away from the commander. Header only, no game or D3D11 state, so native_frame_test runs the very functions the
// Animate post-hook uses. Journal: the arc doc, 2026-10-25, F107.
//
// On the pilot's fleet carrier (log edvr_gfx_20261025_044101, 1c26ea5f, Pimax Crystal Super on Pimax OpenXR) the carrier management screen showed the carrier
// from outside: from 04:59:57 to 05:00:23 the game's camera stood 3367 m from the commander's neck (eye - neck +739, +174, +3280 m in the model), while the neck
// read 1.541-1.545 m as before. Native on foot took that camera for the commander's eye: the body pin moved the body toward it (capped at 1.5 m), the neck
// reference's settle window found it still and published a raise of -174 m, clamped to its -50 cm bound (the view 50 cm under the game's camera: the screen's
// UI, placed at that camera, stood high), and after the pilot's two recentres (LOCAL generation 8 -> 10 at 05:00:02-04) the pin's standing calibration was taken
// again on it (h_stand 175.8 m), so from 05:00:24 to the session's end every call read 174 m under the standing eye and was binned crouching (w_c 1.00).
//
// The rule: the game's camera is on the commander while it is within kOnCommanderM of the animated neck. In three flights the largest eye - neck distance in
// any second with the camera on the commander was 1.27 m (a load's first second), else under 0.66 m. A call with the camera elsewhere (or not a number) is
// refused by the pin: the body keeps its animation, the neck reference and the standing calibration take nothing from it, and the frame thread ramps the
// neck reference's raise out (the view is the game's own camera, as for any camera the game moves away) and back in when the camera returns. F208: the return needs kReturnHoldS of a camera on
// the commander (below).
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f107 {

constexpr double kOnCommanderM = 3.0;

inline double eyeNeckDistance(const double eye[3], const double neck[3]) noexcept {
    const double d[3] = {eye[0] - neck[0], eye[1] - neck[1], eye[2] - neck[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
inline bool onCommander(double eyeNeckM) noexcept { return eyeNeckM == eyeNeckM && eyeNeckM <= kOnCommanderM; }

// F208 (the suit shop at Pioneer Supplies, log edvr_gfx_20261101_114724, v0.22.0-rc.5): the game's showroom camera and a second camera at the commander (46 cm over his neck) took turns in the
// hook's reads every call, 45 times a second for 18 s. The episode ended on every near call and began on every far one (632 episodes, 1268 lines), so the neck reference's raise ramped in and
// out at the call rate: the view pumped, it settled near -29 cm (the near camera's target, ramped about half in) and the pilot saw it vibrate and sit low. The episode now ends only when the
// camera has stayed near for kReturnHoldS: a camera that alternates between far and near is one episode, and a stale report alone ends nothing.
//
// The hold. In the shop's 18 s the gaps between a far call and the next far call (frame-thread time) were a call or two (median 11 ms, 41 to 52 ms often) and three times longer: 0.47 s, 0.51
// s (the shop's own menus: the far camera was quiet that long), and 2.1 s, the real return. The hold is 1.0 s: twice the longest flap, half the real return, and a real return costs a second
// of the view being the game's own camera at the commander (the neck reference ramps back in over kRampS after it). Silence is another matter: with no call at all for kSilenceS the episode is
// over (the commander is not animated: a load, a different mode), and the next call starts clean.
constexpr double kReturnHoldS = 1.0;
constexpr double kSilenceS = 5.0;
constexpr uint64_t kAwayStaleMs = 5000;   // the frame thread's: an open episode is taken as over when the hook has not reported for this long (the hook reports on every call of an open episode, near ones too, so a flapping camera never ages)

// The frame thread's reading of the hook's published episode: away while the hook says so and its newest call is within kAwayStaleMs.
inline bool awayNow(bool published, uint64_t lastMs, uint64_t nowMs) noexcept { return published && nowMs >= lastMs && nowMs - lastMs <= kAwayStaleMs; }

// One episode of the camera away, for the edge lines: the calls the pin refused (every call from the first far one until the return), the far calls and the nearest and farthest distance of them
// (m; a camera that is not a number counts apart), the time and how often the camera came back near and left again before the hold ran out.
struct Away {
    bool away = false;
    uint32_t calls = 0, farCalls = 0, notNumber = 0, episodes = 0, alternations = 0;
    double startS = 0.0, lastS = 0.0, nearestM = 0.0, farthestM = 0.0, nearSinceS = -1.0, lastCallS = -1.0;
    enum Edge : uint8_t { kNone = 0, kLeft = 1, kBack = 2 };
    // One pinned call: its eye - neck distance and its time (s, monotonic). Returns the edge this call made. While an episode is open every call is refused (the pin takes nothing from a camera
    // that was away a moment ago), near ones too; the episode ends on the first near call after the camera has been near for kReturnHoldS without a far one between.
    Edge step(double eyeNeckM, double tS) noexcept {
        Edge e = kNone;
        if (away && lastCallS >= 0.0 && tS - lastCallS > kSilenceS) {   // no call for kSilenceS: nothing was pinned, the episode is over
            away = false;
            nearSinceS = -1.0;
            e = kBack;
        }
        lastCallS = tS;
        const bool farNow = !onCommander(eyeNeckM);
        if (farNow) {
            if (!away) {
                away = true;
                ++episodes;
                calls = farCalls = notNumber = alternations = 0;
                startS = tS;
                nearestM = farthestM = 0.0;
                nearSinceS = -1.0;
                e = kLeft;
            } else if (nearSinceS >= 0.0) {   // back near for a while, and gone again before the hold: the same episode
                ++alternations;
                nearSinceS = -1.0;
            }
        } else if (away) {
            if (nearSinceS < 0.0) nearSinceS = tS;
            if (tS - nearSinceS >= kReturnHoldS) {
                away = false;
                nearSinceS = -1.0;
                e = kBack;
            }
        }
        if (away) {
            ++calls;
            lastS = tS;
            if (farNow) {
                ++farCalls;
                if (eyeNeckM == eyeNeckM) {
                    if (nearestM == 0.0 || eyeNeckM < nearestM) nearestM = eyeNeckM;
                    if (eyeNeckM > farthestM) farthestM = eyeNeckM;
                } else {
                    ++notNumber;
                }
            }
        }
        return e;
    }
};

}  // namespace f107
}  // namespace nofoot
}  // namespace edvr
