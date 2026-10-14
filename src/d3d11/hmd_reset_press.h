#pragma once
// The player's press of Elite's HMD recentre key, watched and never consumed (docs/seat-memory-2026-10-14.md, "the press").
//
// The game's automatic ResetSeatedZeroPose calls and the player's key arrive through the same caller, so the runtime cannot tell them apart
// from the call. The press itself can be seen: this reads the HMDReset action from the player's active .binds (src/d3d11/elite_binds.h), watches
// each bound slot with the machinery hotkeys already use (the keyboard through GetAsyncKeyState with its modifiers, the XInput pad, a joystick
// button the game is seen reading through DirectInput; nothing here opens a device or touches the game's input), and publishes to the runtime
// (frame_flag.h publishHmdResetPress): whether the key can be observed at all, how many presses have been seen, and when the last was.
//
// A pad or joystick slot is only trusted after it has seen one press (HmdPressStatus::Unproven until then): a watcher that cannot see its
// device must not be able to turn every recentre of the player's into "no press". The keyboard needs no proof.
namespace edvr {

// Once a frame, from the journal's tick. Cheap when nothing changes: the bindings are re-read only when the files change.
void hmdResetPressFrame();

}  // namespace edvr
