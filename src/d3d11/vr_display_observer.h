// The display observer (docs/vr-supersampling-gate-2026-10-10.md, H6): LOG ONLY. Elite's 3D mode switching from Off to HMD puts the
// monitor window in windowed mode at 1280x768; the observer says, per call, what the game asked of the swap chain and from where,
// and what the game window looks like on each frame. The hooks around SetFullscreenState and ResizeTarget (device_hook.cpp) call the
// original first and return its result, so behaviour is unchanged.
//
// Every note is capped: the first 64 calls each, then only a call whose arguments differ from the last one logged, to a hard cap.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// Once the swap chain's hooks are set (device_hook.cpp hookSwapChain): one line, installed or why not.
void vrDisplayObserveInstalled(bool setFullscreenState, bool resizeTarget, const char* why);

// The game window, from the swap chain's description (once, at hookSwapChain). The frame check reads it.
void vrDisplayObserveWindow(void* hwnd);

// The render thread, once a frame (from vrSsaaGateNotePresent): the window's style and client size, one line when either changes.
void vrDisplayObserveFrame();

// The hooks' notes. ret is the caller's return address; ours says whether the swap chain is the game's own (the vtable is shared).
void vrDisplayNoteFullscreen(int fullscreen, bool targetGiven, void* ret, long hr, bool ours);
void vrDisplayNoteResizeTarget(uint32_t width, uint32_t height, uint32_t refreshNum, uint32_t refreshDen, uint32_t format,
                               uint32_t scaling, void* ret, long hr, bool ours);
void vrDisplayNoteResizeBuffers(const char* api, uint32_t width, uint32_t height, uint32_t format, uint32_t flags, void* ret,
                                long hr, bool ours);

// An address as text: "game RVA 0x..." inside the game's image, else "<module>+0x...". Shared with the sizing watch.
void vrDisplayCallerText(void* addr, char* out, size_t n);

// The user32 window calls the game makes (H6, the windowed switch): import-table patches on the game's own imports of SetWindowPos,
// SetWindowLongPtrW, ShowWindow, MoveWindow, AdjustWindowRect and AdjustWindowRectEx. Observe only: each call goes to the original
// and returns its result. One line per function says patched or not imported. Called once, from hookSwapChain.
void vrDisplayObserveWindowCallsInstall();

}  // namespace edvr
