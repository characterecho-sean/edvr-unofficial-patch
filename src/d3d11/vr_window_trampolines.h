// The window-mode trampolines (H6, docs/vr-supersampling-gate-2026-10-10.md): the two window entries are OBSERVE ONLY. Installed with
// EDVR's CodeHook (src/common/code_hook.h) through a near relay, they log the game's mode request and window apply and forward every
// call with all its arguments. A third entry, the settings copy at 0x281C0D0 (option B), forwards the same way and, while the 3D mode is
// on, sets the copied Supersampling field to 1.0 after the copy (vrSsaaHoldAfterCopy). It is the only entry that changes a value.
//
//   0x7E9D50  the mode request: rcx = the window object, rdx = the request (mode index [rdx], kind [rdx+0x20]).
//   0x5589B0  the window apply: rcx = the window object, rdx = the state (kind [state+0x20], client w/h [state+0x18]/[+0x1C],
//             monitor rect entry [state+0x28]).
//
// Build-gated: the caller checks the PE stamp and image size first (vrSsaaGateStartup); each target's first bytes must be the ones
// read from build 332841, and CodeHook refuses any prologue it cannot relocate. A refusal is one line and the entry is not patched.
#pragma once

namespace edvr {

// Installs the two entries once, after the build check. Logs one line that says which installed and why not otherwise.
void vrWindowTrampolinesInstall();

}  // namespace edvr
