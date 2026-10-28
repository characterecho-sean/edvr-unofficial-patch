// visual.desktop_window, round 3 (docs\desktop-window.md, "The cursor"): the mouse lock, while the desktop window is active. Sean wants the cursor free to leave Elite's window
// (to move it, or to use the desktop). Which of Elite's calls makes the lock is not known; one flight tells the three hypotheses apart:
//   H1  ClipCursor with a rectangle confines the cursor to the window;
//   H2  SetCursorPos re-centres it every frame, the mouse-look lock: if Elite also reads GetCursorPos every frame it steers by the cursor's movement, and suppressing
//       SetCursorPos would break mouse flight, so it is counted and not changed;
//   H3  a DirectInput mouse in exclusive mode, with neither user32 call involved (the cooperative level is observed in input_gate.cpp).
// The executable's import table is patched for ClipCursor, SetCursorPos, GetCursorPos and ShowCursor (src\common\iat_hook.h), once at startup in a VR-scope install. They pass
// through, except ClipCursor with a rectangle while the window is active: that is released (the real ClipCursor(nullptr)), counted, and answered TRUE. Not active, everything is
// untouched. They run on any thread, so every counter and flag is atomic; no Win32 window call is made in them beyond the real function.
#pragma once

#include <windows.h>

#include <cstddef>

namespace edvr {

// Patch the executable's import table (VR scope only: the caller checks). Logs which hooks installed, once.
void desktopWindowCursorInstall();
void desktopWindowCursorUninstall();

// An activation committed (true) or ended (false). Setting it true also releases whatever clip Elite had, once, from the render thread.
void desktopWindowCursorSetActive(bool active);
// The window's client centre in screen coordinates, as the render thread last measured it, for "SetCursorPos at the client centre".
void desktopWindowCursorPublishCentre(int x, int y);
// Round 4, at each owned Present while the window is active: the Windows cursor is hidden while the window is the foreground one and the cursor is inside its client area,
// and shown otherwise (the title bar and frame stay grabbable). The show count belongs to the thread that owns the window, so this does nothing unless the Present hook
// runs on it (said once). EDVR keeps an offset of its own hides (never above 0); the ShowCursor hook, on that thread only and only while hiding is wanted, pushes the count
// below 0 again after each of the game's calls and gives the game the count it would have seen without EDVR. The offset is given back when hiding is no longer wanted and
// in desktopWindowCursorSetActive(false).
void desktopWindowCursorHide(HWND hwnd);
// The 10 s cursor line, and the counters start again. Returns the length written.
int desktopWindowCursorTakeLine(char* out, size_t size);

// The game's DirectInput mouse: SetCooperativeLevel was called with these flags and returned hr (input_gate.cpp observes it, pass-through). Said once per distinct flags.
void desktopWindowNoteMouseCooperative(DWORD flags, long hr);

}  // namespace edvr
