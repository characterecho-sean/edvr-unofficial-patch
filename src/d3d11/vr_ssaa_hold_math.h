// The VR Supersampling hold, pure (vr_ssaa_hold.h says what and why). Header-only and free of any Windows API: tools\vr_ssaa_hold_test
// drives every function here, and the DLL calls the very same ones.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "../common/stereo_mode.h"

namespace edvr {
namespace ssaahold {

using edvr::parseStereoscopicMode;

// The value the game's Supersampling is held at while Elite's 3D mode is on: HMD Image Quality is then the only render control.
constexpr float kHeldValue = 1.0f;

// The one decision the loader and the setter both take. holdAllowed is the VR profile (the flat profile is never held); modeKnown
// says Settings.xml's mode was read. Held only when all of that holds and the mode is not 0 (off).
struct Decision {
    bool hold;
    float value;  // the value the game's field takes: kHeldValue when held, the requested value otherwise
};
inline Decision decide(bool holdAllowed, bool modeKnown, int mode, float requested) {
    const bool hold = holdAllowed && modeKnown && mode != 0;
    return Decision{hold, hold ? kHeldValue : requested};
}

// What a profile change does to the hold (vrSsaaHoldProfileChanged): release a held field, engage a field that should be held
// while VR is live, or nothing. The hold is held only while the profile allows it AND VR is live, so a switch to flat (or out of
// VR) must not leave the game's Supersampling at 1.0.
enum class ProfileAction { None, Release, Engage };
inline ProfileAction profileChangeAction(bool holdAllowed, bool vrLive, bool held) {
    const bool want = holdAllowed && vrLive;
    if (held && !want) return ProfileAction::Release;
    if (!held && want) return ProfileAction::Engage;
    return ProfileAction::None;
}

// The menu's toast, once per change of the requested value (one line; the toast card holds one).
inline int formatHoldToast(char* out, size_t size, float requested) {
    return std::snprintf(out, size, "Supersampling %.2f is held at 1.0 in VR: use HMD Image Quality to set resolution", static_cast<double>(requested));
}

}  // namespace ssaahold
}  // namespace edvr
