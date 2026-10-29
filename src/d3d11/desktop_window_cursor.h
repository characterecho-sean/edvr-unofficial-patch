// visual.desktop_window, the Windows cursor (docs\desktop-window.md, "The cursor"). Two jobs, neither of which touches the game's mouse lock:
//   HIDDEN OVER THE PICTURE (round 4). While the desktop window is active the cursor is hidden when the window is the foreground one and the cursor is inside its client area,
//   and shown otherwise, so the title bar and edges stay grabbable. The ShowCursor count belongs to the thread that owns the window; see desktopWindowCursorHide.
//   WHAT THE GAME DOES WITH THE CURSOR (round 3's instruments, kept). SetCursorPos and GetCursorPos are only counted, and the cooperative level of the game's DirectInput mouse is
//   logged once (input_gate.cpp). Round 3 found the game confines the cursor with ClipCursor to the client rectangle (H1); that lock is what on-foot VR needs, so it is
//   never touched: no ClipCursor hook, no call. The cursor stays confined while the game holds it, so moving or resizing the window means switching away from the game first.
// The executable's import table is patched for SetCursorPos, GetCursorPos and ShowCursor (src\common\iat_hook.h), once at startup in a VR-scope install. They pass through; only
// ShowCursor's count is changed, and only while the cursor is hidden. They run on any thread, so every counter and flag is atomic; no Win32 window call is made in them beyond
// the real function.
#pragma once

#include <windows.h>

#include <cstddef>

namespace edvr {

// Patch the executable's import table (VR scope only: the caller checks). Logs which hooks installed, once.
void desktopWindowCursorInstall();
void desktopWindowCursorUninstall();

// An activation committed (true) or ended (false). Ending it gives back every hide EDVR made.
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
