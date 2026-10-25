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
// neck reference's raise out (the view is the game's own camera, as for any camera the game moves away) and back in when the camera returns.
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

// One episode of the camera away, for the edge lines: the calls, the time, the nearest and farthest distance (m; a camera that is not a number counts apart).
struct Away {
    bool away = false;
    uint32_t calls = 0, notNumber = 0, episodes = 0;
    double startS = 0.0, lastS = 0.0, nearestM = 0.0, farthestM = 0.0;
    enum Edge : uint8_t { kNone = 0, kLeft = 1, kBack = 2 };
    // One pinned call: its eye - neck distance and its time (s, monotonic). Returns the edge this call made.
    Edge step(double eyeNeckM, double tS) noexcept {
        const bool awayNow = !onCommander(eyeNeckM);
        Edge e = kNone;
        if (awayNow && !away) {
            away = true;
            ++episodes;
            calls = notNumber = 0;
            startS = tS;
            nearestM = farthestM = 0.0;
            e = kLeft;
        } else if (!awayNow && away) {
            away = false;
            e = kBack;
        }
        if (awayNow) {
            ++calls;
            lastS = tS;
            if (eyeNeckM == eyeNeckM) {
                if (nearestM == 0.0 || eyeNeckM < nearestM) nearestM = eyeNeckM;
                if (eyeNeckM > farthestM) farthestM = eyeNeckM;
            } else {
                ++notNumber;
            }
        }
        return e;
    }
};

}  // namespace f107
}  // namespace nofoot
}  // namespace edvr
