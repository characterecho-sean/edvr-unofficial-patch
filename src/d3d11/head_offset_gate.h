// GENERATED from src/d3d11/head_offset_gate.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 f115b9f54880446f]
// When to move the head pose: on foot, in the external camera, and nowhere
// else.
//
// WHAT THIS IS FOR
//
// Elite renders on-foot first person to a flat mono panel, so moving the head
// pose there moves the view OF that panel and does not make it stereo (EVIDENCE
// 6ac.5a). The external camera renders the world in stereo, which makes it the
// one mode where offsetting the pose gives a real stereoscopic viewpoint. The
// offset itself lives in openvr_api.dll, because that is where the pose passes
// through; only d3d11.dll can tell the modes apart, because that needs the
// render state. So this decides, and frame_flag carries the answer across.
//
// WHY IT IS NOT A HEURISTIC
//
// Render state alone cannot do it (6ac.6b, REFUTED). "The flat panel stopped
// and a full scene is being drawn into the eyes" is equally true of entering
// the camera, boarding a ship, and leaving HMD Cinema Mode. The exit is worse:
// the panel coming back is the only positive signal render state offers, and it
// never happens if the camera is left for somewhere that has no panel -- so the
// latch simply stayed on and followed the player into the cockpit.
//
// The player's keypress is the missing information, not a missing heuristic.
// With hotkey.external_camera bound, the transition is known rather than
// inferred, and the render state goes back to being a sanity check. Without it
// the timing window is all there is, and it is documented as the weaker mode
// rather than pretended otherwise.
//
// FAIL-SAFE DIRECTION
//
// Every uncertain case resolves to OFF. An offset that fails to apply leaves
// the game exactly as it was; one that fails to stop applying moves the
// player's viewpoint in the cockpit, which is the outcome to avoid.
//
// This module reads no game state and writes nothing to the game. It watches
// two counters the render hooks already produce and publishes one bit.
#pragma once

#include <cstdint>

namespace edvr {

// Reads fix.head_offset_*. Safe to call repeatedly; it is on the hot-reload
// path as well as the startup one.
void headOffsetGateConfigure();

// The player pressed hotkey.external_camera. A toggle: the first press is an
// intent to ENTER, the next is an exit.
//
// The exit matters as much as the entry and is the half render state cannot
// supply: boarding a ship from the external camera produces no panel frame
// ever, so without this the latch stayed on in the cockpit.
void headOffsetGateKeyPressed();

// Tell the gate whether hotkey.external_camera is CONFIGURED, which is not the
// same as having been pressed.
//
// It used to infer "the player has a key" from the first press, so a correctly
// set-up user ran the weaker no-key path for the whole first part of every
// session -- and that path is the one that cannot tell entering the camera from
// boarding a ship. The window was open exactly until the moment it stopped
// being needed.
void headOffsetGateSetKeyBound(bool bound);

// The player pressed hotkey.external_camera_next, so the camera has moved to
// the next view in its cycle.
void headOffsetGateViewBumped();

// Supply an authoritative view index, or -1 for "not known".
//
// Counting keypresses works and has no origin (6ac.6d): the game REMEMBERS the
// view across camera toggles, so there is no moment when the count is known to
// be right and one missed press desyncs it for the session. A caller that can
// read the game's own index passes it here and the count stops mattering.
// Nothing in this module knows or cares how such a caller gets it.
void headOffsetGateSetView(int view);

// Is the gate switched on (fix.head_offset_gate)?
//
// Asked by the render hooks, because counting panel draws costs a GetDesc per
// eye-sized draw and is not worth paying for when nothing wants the answer.
bool headOffsetGateWantsPanel();

// Called once per frame from the Present path, with the two counters this
// decision is made from:
//
//   panelDraws  draws into the flat on-foot panel this frame
//   eyeDraws    draws into the stereo eye textures this frame
//
// The caller resets its own counters; this does not touch them.
void headOffsetGateFrame(uint32_t frameNo, uint32_t panelDraws, uint32_t eyeDraws);

// True on the ONE frame the flat panel was first counted -- the earliest moment
// the game is known to be loaded and the player known to be on foot.
//
// Exists so private diagnostics can hang off that moment without this module
// knowing what they are. Public builds ignore it.
bool headOffsetGatePanelFirstSeen();

}  // namespace edvr
