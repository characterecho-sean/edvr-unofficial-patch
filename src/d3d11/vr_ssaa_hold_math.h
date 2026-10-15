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

// The 3D-on switch. The game applies its settings (the Supersampling setter and the sizing pass) BEFORE the profile switch commits, so
// at that apply the profile is still flat. The runtime's VR session going live while a flat profile is active is the earliest signal
// that the apply is a 3D-on one: Settings.xml still says mode 0 at the apply and for the next 0.4 s (flight 2026-10-14, the 0.65 screen).
// "Arriving" is latched on that rising edge only, so a VR-to-flat switch (live stays up while the session tears down) never arms it.
// profileAllows is the VR profile (the startup rule); scopeIncludesVr and featuresAllowed are the install and profile gates.
inline bool arrivingAtRise(bool profileAllows, bool scopeIncludesVr, bool featuresAllowed) {
    return !profileAllows && scopeIncludesVr && featuresAllowed;
}
// The hold is allowed by the VR profile, or by an arriving 3D-on ahead of the profile switch.
inline bool holdAllowedFor(bool profileAllows, bool arriving) { return profileAllows || arriving; }

// The 3D mode in the game's own settings block (flight 2026-10-14, 11:57-11:58): the settings object's dword at +0x30, which the
// settings copy W carries to ctx+0x33D0, reads 4 while Settings.xml's StereoscopicMode is 4 and 0 when 3D is off. It changes at the
// apply (4->0 at call 5, 0->4 at call 7, 0.7 s before the profile switch), whatever the profile and whatever the runtime session does
// (the session stays up through a live flat switch). Values are Settings.xml's 0..6, so on is nonzero, the same rule as `decide`.
constexpr unsigned kMaxStereoscopicMode = 6;
inline bool blockModeKnown(unsigned raw) { return raw <= kMaxStereoscopicMode; }

// Whether the settings copy holds the sizing field at 1.0.
//   modeKnown: the block's mode dword read and is 0..6. Then the block says: hold when 3D is on and the install's scope includes VR and the
//     profile's features are on, if the hold is not simply a flat launch with no session (a VR profile, a live runtime session, or the
//     block's mode having just risen from 0 at this copy, a 3D-on apply).
//   unknown (the block did not read): the earlier rule, the VR profile or an arriving 3D-on, and a live session.
inline bool copyHolds(bool modeKnown, unsigned mode, bool modeRose, bool scopeIncludesVr, bool featuresAllowed, bool profileAllows,
                      bool vrLive, bool arriving) {
    if (modeKnown) return mode != 0 && scopeIncludesVr && featuresAllowed && (profileAllows || vrLive || modeRose);
    return holdAllowedFor(profileAllows, arriving) && vrLive;
}

// The menu's toast, once per change of the requested value (one line; the toast card holds one).
inline int formatHoldToast(char* out, size_t size, float requested) {
    return std::snprintf(out, size, "Supersampling %.2f is held at 1.0 in VR: use HMD Image Quality to set resolution", static_cast<double>(requested));
}

}  // namespace ssaahold
}  // namespace edvr
