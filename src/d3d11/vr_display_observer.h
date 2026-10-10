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

}  // namespace edvr
