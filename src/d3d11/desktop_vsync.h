// The desktop window's vertical sync (2026-10-30). Header only, so tools\desktop_window_test runs the very functions the Present hook uses.
//
// In the VR profile the headset paces the game's frames: the native runtime's xrWaitFrame releases each one at the headset's next slot. The desktop
// window is only a mirror there, and a Present that waits for the monitor's vertical blank could hold the game to the monitor's rate (60 Hz under a
// 90 Hz headset), a rate the runtime then meets by reprojecting at half. So in VR Elite's Present goes out with sync interval 0 whatever Elite asked.
// In the flat profile the desktop is the game's display and Elite's own setting stands. Elite's own swapchain only: another swapchain's Present (an
// overlay's, a mod's) is forwarded untouched. No key: a fix that always helps gets no toggle.
//
// The evidence: one log line at each change of what Elite asks or of the profile, never per frame, so the first VR Present always says whether there
// was anything to override. The openvr half's native_post_submit_window line still counts what Elite asked (present_sync_nonzero, from the trace
// span, which carries Elite's value), so the two lines together tell an override that ran from one that never did.
#pragma once

#include <cstddef>
#include <cstdio>

namespace edvr {

// The sync interval the real Present is called with.
inline unsigned desktopVsyncPresented(bool flatProfile, unsigned asked) noexcept { return flatProfile ? asked : 0u; }

// What was last said: a line is due when the profile or Elite's value changes (and on the first Present).
struct DesktopVsyncNote {
    bool known = false;
    bool flat = false;
    unsigned asked = 0;
    bool changed(bool flatProfile, unsigned askedNow) noexcept {
        if (known && flat == flatProfile && asked == askedNow) return false;
        known = true;
        flat = flatProfile;
        asked = askedNow;
        return true;
    }
};

inline int formatDesktopVsync(char* buf, size_t n, bool flatProfile, unsigned asked) noexcept {
    if (flatProfile) return std::snprintf(buf, n, "desktop vsync: the flat profile: Elite's sync interval %u is presented as asked", asked);
    if (asked == 0) return std::snprintf(buf, n, "desktop vsync (VR): Elite asks sync interval 0; nothing to override");
    return std::snprintf(buf, n, "desktop vsync (VR): Elite asked sync interval %u; presented with 0, so the monitor does not hold the frame (the headset paces VR)", asked);
}

}  // namespace edvr
