// The VR Supersampling hold (step 2 of the arc in docs/vr-supersampling-gate-2026-10-10.md). While Elite's 3D mode is on
// (Settings.xml's <StereoscopicMode> is not 0) and the graphics profile is VR, the game's own Supersampling field
// (render context +0x3564, the value every sizing and shader consumer reads) is held at 1.0, so HMD Image Quality is the only
// render control in VR. The flat profile and 3D mode 0 are untouched.
//
// HOW IT IS HELD.
//   * At startup: the game's fxcfg loader (RVA 0x2855A50, reached through the wrapper at RVA 0x2862780, a virtual slot in .rdata
//     at RVA 0x52E8368) writes the .fxcfg's SSAAMultiplier into the loader's object. EDVR swaps that slot for its own thunk when
//     the DLL is attached (DllMain, pure memory: no config, no file, no log under the loader lock). The thunk calls the original,
//     then writes 1.0 to the object's SS field (+0x13C, which is render context +0x3564 offset by 0x3428 -- verified by the log
//     line that gives both pointers) when the decision says held.
//   * In the menu: the game's setter (ui_panel_scale.cpp's hookedSetter, installed the same way) passes 1.0 instead of the value
//     the menu asked for, when held. The requested value is remembered for the log and the toast.
//   * The mode is read from Settings.xml at the loader and re-read ~1.5 s after each setter call, on the render thread at a frame
//     boundary (one small read). A change is logged. A 3 -> 0 change is not chased: the field stays at 1.0 until the next apply or
//     restart, and the log says so. 0 -> on is held at the next apply.
//   * The panel factor (ui_panel_scale.cpp's chooseSupersampling) takes 1.0 while held, so the panels and the size budget match
//     the field.
//
// WHAT IS REFUSED. A build other than 332841 (PE stamp and image size), a wrapper or loader prologue that is not build 332841's,
// or a slot that does not hold the wrapper: nothing is written, and one line says why (vrSsaaHoldReport, at device creation).
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

// The setter's value: 1.0 while held, the requested value otherwise. Called first in ui_panel_scale.cpp's hookedSetter.
float vrSsaaHoldSetterValue(float requested);

// Whether the field is held at 1.0 right now (the panel factor reads 1.0 then). Lock-free.
bool vrSsaaHoldActive();

// The render thread, once a frame (from vrSsaaGateNotePresent): re-reads Settings.xml when a setter call asked for it.
void vrSsaaHoldFrameBoundary();

// The menu, once per change of the requested value while held and not 1.0: true with the value to say.
bool vrSsaaHoldNoticeDue(float* requested);

// Settings.xml's <StereoscopicMode>: true with *mode when it reads; raw gets "0".."6", "absent", "unparsed" or "unreadable".
bool vrSsaaHoldReadSettings(int* mode, char* raw, size_t rawLen);

// The loader object the startup hold wrote to (its SS field at +0x13C), or 0 before the loader ran. The step-1 setter line prints
// the render context beside it, so a flight can check context = object + 0x3428.
unsigned long long vrSsaaHoldLoaderObject();

// The Settings.xml mode as the hold last read it (*known false when unread or unparsed); the display observer's lines carry it.
int vrSsaaHoldLastMode(bool* known);

// ui_panel_scale.cpp's setter thunks: the render context the setter was called on. The hold re-applies to it on a 0 -> on change.
void vrSsaaHoldNoteContext(uintptr_t ctx);

// The sizing watch (H7): called from the game's texture creates (device_hook.cpp). For 3 s after a setter call or a mode change, a
// game render or depth-stencil target is logged with its size, format, bind flags and the callers. One flag test when not armed.
void vrSizingWatchTexture(uint32_t w, uint32_t h, uint32_t format, uint32_t bind);

}  // namespace edvr
