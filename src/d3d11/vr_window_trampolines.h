// The game-code trampolines (docs/vr-supersampling-gate-2026-10-10.md). Installed with EDVR's CodeHook (src/common/code_hook.h) through a
// near relay, each forwards every call to the original with all its arguments, and changes nothing else except as stated:
//
//   0x7E9E50  the mode request (rcx = the window object, rdx = the request: kind [request+0x20], mode-table index [request+0x18]). While
//             VR is inactive, a non-windowed request is remembered. While VR is active, a windowed request (kind 0) is rewritten to the
//             remembered kind and mode index before the original runs, so the display mode the user had is kept at 3D on (the
//             maintainer's decision; H6). The rewrite is logged, capped.
//   0x281E650 the settings copy W (rcx = the first argument whose +0x1180 is the render context). After the copy, the hold
//             (vrSsaaHoldAfterCopy) sets the copied Supersampling field to 1.0 while VR is live.
//
// The DisplayConfig save (0x7FA000) is NOT held: its object's field layout is not proven (see the doc, "The save corruption").
//
// Build-gated: the caller checks the PE stamp and image size first (vrSsaaGateStartup); each target's first bytes must be the ones read
// from build 334925, and CodeHook refuses any prologue it cannot relocate. A refusal is one line and the entry is not patched.
#pragma once

namespace edvr {

// Installs the two entries once, after the build check. Logs one line that says which installed and why not otherwise.
void vrWindowTrampolinesInstall();

}  // namespace edvr
