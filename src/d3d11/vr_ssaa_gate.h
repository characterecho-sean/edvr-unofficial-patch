// The VR Supersampling gate, step 1: INSTRUMENTS ONLY (docs/vr-supersampling-gate-2026-10-10.md). Log lines, nothing else.
// No call here writes game memory, changes a value the game reads or EDVR's own sizing, or adds a game-memory read
// beyond the ones ui_panel_scale.cpp already makes behind the same build gate.
//
// The arc's goal (step 2, not this file): while Elite's 3D mode is not off, the game sees its Supersampling as 1.0, so
// HMD Quality is the only render-resolution control in VR. These lines are what a flight needs to decide the mechanism:
// the startup files, the setter and getter as the game calls them, the order of the first Present, the setter, the
// getter, and whether the in-memory 3D mode can be read at all.
//
// Everything is behind the build gate (PE stamp and image size, build 332841). On any other build the first call says
// so once, and every other entry point here returns at once.
#pragma once

#include <cstdint>

namespace edvr {

// Once per process, from hookDevice: the build gate, then one line with the startup Settings.xml's StereoscopicMode and
// the newest .fxcfg's SSAAMultiplier and HMDRenderTargetMultiplier, and the timestamp of the read.
void vrSsaaGateStartup();

// hookedPresent, for the game's own swap chain: every call counts a frame; the first logs the order line.
void vrSsaaGateNotePresent();

// ui_panel_scale.cpp's setter thunk: before the game's setter runs, with the value ctx+0x3564 holds then (NaN if unread).
void vrSsaaGateNoteSetterBefore(float before);

// ui_panel_scale.cpp's setter thunk: after the game's setter ran, with the value passed, ctx+0x3564 and the game's range
// as read after it (NaN for the value if it could not be read). Writes at most one line: the first 64 calls each, then
// a call only when the passed, before or after value differs from the last one logged, up to a hard cap.
void vrSsaaGateNoteSetterAfter(uintptr_t ctx, float passed, float after, float lo, float hi);

// ui_panel_scale.cpp's getter thunk: true until the first getter read is logged, so the caller reads the value only then.
bool vrSsaaGateGetterPending();

// ui_panel_scale.cpp's getter thunk, once: the value the game returned (ctx+0x3564).
void vrSsaaGateNoteGetter(float value);

}  // namespace edvr
