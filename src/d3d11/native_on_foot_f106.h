// F106 (native on-foot VR): ADS, the sight in both eyes (fix.native_on_foot_ads_sight_eye = both). The pure part: the share's decision. Design and journal: the arc doc,
// 2026-10-25, F106.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very function the frame thread uses.
//
// In ADS the right eye is the eye the sight lock lines up (F70; F162: unless the key says left, and then there is no share). With the key at both (keyOn), the left eye shows the
// right eye's image, turned into the left eye's view by rotation only, by the ADS lock's weight (0.15 s in, 0.3 s out); the runtime draws it in the left eye's scene pass
// (src\openxr\d3d11_stereo.cpp, eye_share.h). It replaced F89's close, which faded the left eye to black by the same weight (its key retired in F106). share() is the whole
// decision: never above 0 outside ADS, with the key at right or left (keyOn false), unarmed or off foot (the guard, whose trip is logged).
#pragma once

namespace edvr {
namespace nofoot {
namespace f106 {

struct Share {
    double weight = 0.0;    // 0 (the left eye's own image) to 1 (the right eye's image, turned into the left view): the left eye only
    bool tripped = false;   // the guard forced 0 while the ADS lock's weight was above 0 (unarmed or off foot with the key on)
};
// lockWeight: the ADS lock's weight (the head-aim ramp, 0.15 s in and 0.3 s out), 0 outside ADS. The share is that weight with the key on, in native on foot, with a
// weapon or tool drawn; anything else is exactly 0 (positive zero), and an armed/foot failure while the weight is above 0 is the guard's trip.
inline Share share(bool keyOn, bool onFoot, bool armed, double lockWeight) noexcept {
    Share s;
    const double w = lockWeight == lockWeight ? (lockWeight < 0.0 ? 0.0 : lockWeight > 1.0 ? 1.0 : lockWeight) : 0.0;
    if (!keyOn || !(w > 0.0)) return s;
    if (!onFoot || !armed) {
        s.tripped = true;
        return s;
    }
    s.weight = w;
    return s;
}

}  // namespace f106
}  // namespace nofoot
}  // namespace edvr
