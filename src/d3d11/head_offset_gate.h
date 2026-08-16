// GENERATED from src/d3d11/head_offset_gate.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 1e5e6a046d8c4a46]
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

// Drop every latch, counter and intent, and publish OFF.
//
// Called when the gate is switched off, and by the frame-feed test between
// scenarios. Turning the gate off used to take the early return and simply stop
// running -- which froze the latch rather than clearing it, so the offset stayed
// applied for about a second and, worse, turning the gate back on republished
// the stale latch wherever the player had gone in the meantime.
//
// "Stop deciding" and "decide no" are different answers, and only one of them is
// safe to leave behind.
void headOffsetGateReset();

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

// Tell the gate whether hotkey.external_camera_next is CONFIGURED. Same
// distinction as headOffsetGateSetKeyBound: the bridge's log line and the
// dead-config warning need to know a key exists before its first press.
void headOffsetGateSetNextKeyBound(bool bound);

// A new on-foot session has begun: the game resets its external-camera view
// to 0 across this boundary (6ay), so the counted view and any held view
// restart from 0 with it. Two detectors call this for the same landing --
// the journal's Disembark (authoritative, wired in device_hook) and the
// panel-return heuristic (fallback, internal) -- and it dedupes, so the
// first to speak does the work. `source` names the caller in the log.
void headOffsetGateNewFootSession(const char* source);

// The game's own live word on whether the player is on foot (Status.json's
// OnFoot flag, fed per frame from the journal watcher). What it buys is
// KEYLESS camera detection: on foot per the game, with the on-foot screen
// gone and a stereo scene rendering, is the external camera -- boarding
// drops the flag and announces Embark, and 6bb measured the flag HOLDING
// through the entire camera window. `known` false (menus, no Status.json,
// watcher off) restores the key-only behaviour exactly.
// `sample` is the running count of Status.json reads: keyless arming
// requires an on-foot sample taken AFTER the panel stopped, so boarding
// from the camera -- panel gone, previous sample still saying on-foot for
// up to a second -- cannot put the offset in the boarding animation.
void headOffsetGateSetOnFootLive(bool known, bool onFoot, uint32_t sample);

// Rising-edge counter of camera entries, for the caller that nudges the
// view scanner: fresh candidates at every entry is what makes the anchored
// certification land while the player is still cycling to their view.
uint32_t headOffsetGateEnterCount();

// The gate's current counted view -- the anchor a candidate record must
// read at priming time for the two-step anchored certification (0 after a
// disembark, the last confirmed view within a session).
int headOffsetGateCountedView();

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

// Is the player in the external camera right now, whatever the view?
//
// Exported for camera_view's certification: the true preset can only change
// while the player is IN the camera pressing the view key -- the game freezes
// it everywhere else -- so a candidate record whose value moves outside the
// camera has disqualified itself as the preset for that stretch (6aw: the
// array contains a counter that rebuilds increment sequentially, and it
// certified under every shape-based rule; context is the discriminator no
// observed impostor satisfies).
bool headOffsetGateInCamera();

// Has the flat panel been composited steadily for a while?
//
// This is "the game is drawing the on-foot screen", and it is deliberately NOT
// "the player is definitely on foot in the world", because nothing here can
// tell those apart. The panel is recognised by its size, which with the
// resolution fix off is 1920x1080 -- and so is plenty of what the main menu
// draws. A default install therefore sees a settled "panel" about four seconds
// after launch, in a menu, with half the game's memory yet to be allocated.
//
// TWO WRONG ANSWERS ARE ON RECORD HERE, and the second is the more instructive.
//
// The first was the FIRST panel frame, which fired in the menu as above.
//
// The second added "and a full scene is in the eyes", reasoning that the helmet
// HUD puts hundreds of draws there on foot. It never fired at all -- because on
// foot the world is drawn to the PANEL, and a full scene in the eyes is what
// happens when the panel STOPS. The two conditions are close to mutually
// exclusive, so ANDing them asked for a state that barely occurs. The gate's own
// log proved it by absence: a run of 90 settled panel frames, and no scan.
//
// So this stays a weak signal, honestly labelled. What makes the feature work is
// not this being right; it is that a scan which finds nothing tries again later.
// Do not add a third condition here without evidence that the state it names
// actually occurs.
bool headOffsetGatePanelSettled();

}  // namespace edvr
