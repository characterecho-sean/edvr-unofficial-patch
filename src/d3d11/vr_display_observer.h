// The game window's state (docs/vr-supersampling-gate-2026-10-10.md, H6). Read only: once a frame the window's style and client size are
// read, and one line is written when either changes. The debug hooks that used to observe the display calls are gone.
#pragma once

namespace edvr {

// The game window (the swap chain's output window), set once at hookSwapChain. The frame check reads it.
void vrDisplayObserveWindow(void* hwnd);

// The game window, or null before the swap chain is hooked.
void* vrDisplayGameWindow();

// The game window's style and client size, read as the frame check reads them. False before the swap chain is hooked.
bool vrDisplayReadGameWindow(unsigned long long* style, long* clientWidth, long* clientHeight);

// The render thread, once a frame (from vrSsaaGateNotePresent): the window's style and client size, one line when either changes.
void vrDisplayObserveFrame();

}  // namespace edvr
