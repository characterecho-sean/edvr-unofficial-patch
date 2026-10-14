// The VR Supersampling hold (docs/vr-supersampling-gate-2026-10-10.md). While the runtime's VR session is live, the game's Supersampling
// field (render context +0x3564, the value every sizing and shader consumer reads) is held at 1.0, so HMD Image Quality is the only
// render control in VR. When the session ends, the game's own value is restored, so flat uses the user's Supersampling.
//
// THE LIVE DECISION. "VR active" is the runtime's published render sizing (edvrQueryNativeRenderSizing, valid == 1), set when an OpenXR
// session has its eye sizes and cleared at teardown (native_runtime_host.h). Settings.xml's <StereoscopicMode> is read only at startup,
// for the loader's hold; it lagged the game's own apply, so it does not decide.
//
// HOW IT IS HELD.
//   * The copy trampoline (vr_window_trampolines.cpp, the game's settings copy W): after the copy, while VR is live, the field is set to
//     1.0. The size writer reads it next, so the sizes use 1.0. The source block is not written.
//   * The setter (ui_panel_scale.cpp hookedSetter): while VR is live, the value passed on is 1.0 and the game's value is remembered.
//   * The frame boundary: a VR transition sets the field to 1.0 (on) or restores the game's remembered value (off).
//   * The panel factor (ui_panel_scale.cpp chooseSupersampling) takes 1.0 while held, so the panels follow the same decision.
//   * At startup, the loader's temporary is written before the game copies it (the startup write reaches the copy, not the field).
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// DllMain's attach: the build gate, the loader's slot, and the Supersampling setter/getter hooks (ui_panel_scale.cpp's
// uiPanelScaleEarlyHooks). Pure memory reads and writes; no logging here (the reason is kept for vrSsaaHoldReport).
void vrSsaaHoldEarlyInstall();

// vrSsaaGateStartup (device creation): one line on whether the hold's hooks are in, and why not if not.
void vrSsaaHoldReport();

// DLL unload: the loader's slot is put back. Idempotent.
void vrSsaaHoldShutdown();

// The setter's value: 1.0 while VR is live (and the game's value is remembered), the requested value otherwise.
float vrSsaaHoldSetterValue(float requested);

// Whether the field is held at 1.0 right now (the panel factor reads 1.0 then). Lock-free.
bool vrSsaaHoldActive();

// Whether the runtime's VR session is live (the render thread's last read). Lock-free.
bool vrSsaaHoldVrLive();

// The render thread, once a frame: reads the live VR decision and applies a transition (see the .cpp).
void vrSsaaHoldFrameBoundary();
// Called at a profile switch, render thread, frame boundary: the hold follows the active profile, not only the live VR session.
void vrSsaaHoldProfileChanged();

// The settings copy's hold (called from the W relay with the render context, after the copy). Holds 1.0 when the copied block's 3D mode (settings object +0x30) is on, whatever the profile and the session (vr_ssaa_hold_math.h copyHolds).
void vrSsaaHoldAfterCopy(uintptr_t ctx);

// ui_panel_scale.cpp's setter and getter thunks: the render context the call was made on.
void vrSsaaHoldNoteContext(uintptr_t ctx);

// The menu, once per change of the requested value while held and not 1.0: true with the value to say.
bool vrSsaaHoldNoticeDue(float* requested);

// Settings.xml's <StereoscopicMode>: true with *mode when it reads; raw gets "0".."6", "absent", "unparsed" or "unreadable". Startup only.
bool vrSsaaHoldReadSettings(int* mode, char* raw, size_t rawLen);

// The loader object the startup hold wrote to (its SS field at +0x13C), or 0 before the loader ran. Logged only.
unsigned long long vrSsaaHoldLoaderObject();

// The Settings.xml mode read at startup (*known false when unread or unparsed).
int vrSsaaHoldLastMode(bool* known);

}  // namespace edvr
