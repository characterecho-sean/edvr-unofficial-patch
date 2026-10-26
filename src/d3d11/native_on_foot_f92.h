// F92b (native on-foot VR): the ADS head aim as a user toggle, live mid-press (fix.native_on_foot_ads_head_aim, the Native on foot page's "ADS: aim with head").
// Journal: the arc doc, F92b.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks use.
//
// The ADS press (adsAimTick's state: idle, engaged, releasing) runs whatever the toggle says: the sight eye lock (F70), the position lock, the left eye's share (F106; F89's close before), the
// body detach (F73) and the item gate (F91) follow the press as before. The toggle decides only the head aim within an engaged press: the head's turn sent to the game as
// look input and the game's turn cancelled in the seated space (F62b/F68a), the guard on it and the press's centring snap (F69i).
//  - The press engages with the toggle on: the head aim engages with it (a fresh press, as before).
//  - Turned off while engaged: the head aim unwinds what it carried as a release does (the opposite injected over the unwind's 0.3 s, the cancel following what the game
//    applies, so the view stays the headset's); the press stays engaged.
//  - Turned on while engaged (off, or while unwinding): a fresh head aim from the current head, as a fresh press: the in-flight account, the guard's base and the centring
//    start again.
//  - The press's release (or its end) owns everything: the release unwinds whatever is carried, whatever the toggle says.
#pragma once

#include "native_on_foot_f91.h"   // f91::kStateIdle, kStateEngaged, kStateReleasing (adsAimTick's states)

namespace edvr {
namespace nofoot {
namespace f92 {

enum AimMode : int { kAimOff = 0, kAimOn = 1, kAimUnwinding = 2 };
inline const char* aimModeName(int m) noexcept { return m == kAimOn ? "on" : m == kAimUnwinding ? "unwinding" : "off"; }

struct AimEdge {
    int mode = kAimOff;
    bool engage = false;        // a fresh head aim: the in-flight account, the guard's base, the head's previous sample and the centring start again
    bool unwindBegin = false;   // turned off while engaged: the map begins its release unwind (the press stays engaged)
};
// Before the map's step, once per tick. adsState: the press's state after this tick's edge; entered: this tick's edge engaged the press (from idle or from its release);
// key: the toggle.
inline AimEdge aimEdge(int mode, int adsState, bool entered, bool key) noexcept {
    AimEdge e;
    e.mode = mode;
    if (adsState != f91::kStateEngaged) {   // idle, or the press's release: the release unwinds whatever is carried
        e.mode = kAimOff;
        return e;
    }
    if (entered) {
        e.mode = key ? kAimOn : kAimOff;
        e.engage = key;
        return e;
    }
    if (key && mode != kAimOn) {
        e.mode = kAimOn;
        e.engage = true;
    } else if (!key && mode == kAimOn) {
        e.mode = kAimUnwinding;
        e.unwindBegin = true;
    }
    return e;
}
// The map's mode this tick: it asks the head's step (engaged) or runs the unwind (releasing: the press's release, or the head aim turned off while engaged).
inline bool mapEngaged(int adsState, int mode) noexcept { return adsState == f91::kStateEngaged && mode == kAimOn; }
inline bool mapReleasing(int adsState, int mode) noexcept {
    return adsState == f91::kStateReleasing || (adsState == f91::kStateEngaged && mode == kAimUnwinding);
}
// After the map's step: an unwind while engaged is done when the map's release is (its schedule ran and nothing is left, or the chase ran out); the head aim is then off for
// the rest of the press (or until turned on again) and whatever is left is dropped, as at the end of a release.
inline bool aimUnwound(int adsState, int mode, bool releaseDone) noexcept {
    return adsState == f91::kStateEngaged && mode == kAimUnwinding && releaseDone;
}

}  // namespace f92
}  // namespace nofoot
}  // namespace edvr
