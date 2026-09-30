#pragma once
#include <windows.h>

namespace edvr {

// The window focus-on-launch (d3d11.focus_on_launch) may take the foreground
// for: a top-level window on the desktop, which is what the game's is.
// Anything whose parent is not the desktop -- a child window, a message-only
// window -- is not the game. A swap chain made on one is an embedded or test
// surface, and moving the keyboard for it takes it from whoever is typing.
//
// The test rigs' fixture windows are exactly that (tools\openxr_native_test\
// present_device.h). Before this rule every rig that made a hidden top-level
// window for its swap chain took the foreground from the developer at each of
// its launches, through a full build. Making the fixture a child window was
// not enough on its own: in one build the step still moved the foreground to
// the child's ancestor, five launches running. So the step is not attempted
// for such a window at all, whatever state the desktop is in.
inline bool focusTargetWindow(HWND hwnd) {
    return hwnd && IsWindow(hwnd) && GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow();
}

}  // namespace edvr
