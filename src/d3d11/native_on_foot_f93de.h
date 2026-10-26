// F93d (native on-foot VR): the pure parts. Header only, no game or D3D11 state, so native_frame_test runs the very functions the frame thread and the
// Animate post-hook use. Journal: the arc doc, 2026-10-24, F93d and F93e (F94a retired F93e: the body stands at its animated height and the view's height comes
// from the neck, native_on_foot_f94.h).
//
// F93d, the game's camera. The camera suite's controller (VesselCameraMountControl) keeps the suite's mode in its byte +0x3E0, 0 while the suite is closed and the
// game's own first-person camera is the view, 1..6 while the suite is open (a preset such as "Commander control" or "Commander profile", the free camera and its locks).
// The avatar hooks' controller hook (native_on_foot_hooks.cpp) reads it after each game update and publishes it (native_on_foot.h, nativeOnFootCameraPublish); every EDVR change to the commander's
// body is first-person behaviour and stands down while it is not 0. CameraWatch is the frame thread's side: which lines are owed and when the published flag is no
// longer evidence (native stereo on foot ended, or the controller stopped updating with the suite open).
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f93 {

// ---- F93d ---------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kWatchMissMs = 5000;     // a stretch on foot with no controller read for this long is said once (the body changes then stay on in every camera)
constexpr uint64_t kWatchSilentMs = 1000;   // the controller quiet this long with the suite open at its last read: the flag is no longer evidence

struct WatchIn {
    bool active = false;     // native stereo on foot
    uint64_t reads = 0;      // the hook's read count (g_nativeOnFootCamReads)
    bool flag = false;       // the hook's flag: the suite was open at the last read (g_nativeOnFootCamExternal)
    uint64_t nowMs = 0;
};
struct WatchOut {
    bool clearFlag = false;  // the frame thread clears the hook's flag: off foot, or the controller went quiet with the suite open
    bool watching = false;   // the first read of a stretch on foot (one line)
    bool missing = false;    // no read in kWatchMissMs of a stretch on foot (one line)
    bool silent = false;     // the controller quiet over kWatchSilentMs with the suite open (one line; the flag cleared)
    bool changed = false;    // the camera differs from the one last logged: a transition line
    bool ended = false;      // native stereo on foot ended with the third-person camera last logged: the closing line
    bool external = false;   // the camera after this tick: true third-person
    uint64_t quietMs = 0;    // with silent: how long the controller had been quiet
};
struct CameraWatch {
    bool last = false, reading = false, missNoted = false, silentNoted = false, activeLast = false;
    uint64_t activeSinceMs = 0, readsLast = 0, readsMs = 0;
    WatchOut step(const WatchIn& in) noexcept {
        WatchOut o;
        if (in.active != activeLast) {
            activeLast = in.active;
            activeSinceMs = in.nowMs;
            reading = false;
            missNoted = false;
            silentNoted = false;
        }
        if (!in.active) {
            o.clearFlag = true;   // a suite state never outlives native stereo on foot
            if (last) {
                last = false;
                o.ended = true;
            }
            return o;
        }
        bool flag = in.flag;
        if (in.reads != readsLast) {
            readsLast = in.reads;
            readsMs = in.nowMs;
            silentNoted = false;
            if (!reading) {
                reading = true;
                o.watching = true;
            }
        } else if (!reading) {
            if (!missNoted && in.nowMs - activeSinceMs >= kWatchMissMs) {
                missNoted = true;
                o.missing = true;
            }
        } else if (flag && in.nowMs - readsMs > kWatchSilentMs && !silentNoted) {
            silentNoted = true;
            o.silent = true;
            o.quietMs = in.nowMs - readsMs;
            o.clearFlag = true;
            flag = false;
        }
        o.external = flag;
        if (flag != last) {
            last = flag;
            o.changed = true;
        }
        return o;
    }
};

}  // namespace f93
}  // namespace nofoot
}  // namespace edvr
