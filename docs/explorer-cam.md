# Explorer Cam

## Status

- **State: the old route is replaced, the new one is built and not flown
  (2026-10-07, redesign branch).** Until then Explorer Cam counted your
  camera-key presses and offset the headset pose; the game-memory read behind
  that had retired 2026-09-29 (code removed, 0fe90f09), taking
  `camera_index_track`, its five sibling keys and
  `fix.head_offset_view_bridge` with it.
- **2026-10-07 on the redesign branch: the old route is deleted; see the design
  doc.** The new route places Elite's own free camera at the commander's head
  and locks it, with no press counting and no pose offsets. The commander's head
  is still visible from inside; hiding it is the next phase. See
  [design-explorer-cam-free-camera-2026-10-07.md](design-explorer-cam-free-camera-2026-10-07.md),
  which also reviews the unmerged head-hiding branch.

Explorer Cam puts your viewpoint at your commander's head while you are on foot
in Elite's free camera, which renders in proper stereo. This page covers setting
it up and what it does under the hood; the [README](../README.md#explorer-cam)
has the short version.

## It gives you no capability you do not already have

This matters more than the effect does. The external camera is Elite's own
feature, opened with your own binding, and inside it you cannot act: you cannot
shoot, scan, open a panel, use a terminal or pick anything up. To act you
switch back to first person, exactly as you do today. Explorer Cam changes
where the camera is while you are already in that mode and nothing else. It
gives no extra reach, reveals nothing the camera was not already showing, and
removes no step that anyone else has to take. A player with it and a player
without it can do the same things in the same order with the same clicks, and
only one of them sees it in 3D.

It touches nothing shared: no network path, no server state, nothing another
player observes. What it writes to the game's memory is that one camera's own
pose, lock and collision, described below.

## Setting it up

1. On foot, open the camera with the binding you already use and press **TAB**
   for the free camera. EDVR puts the view in your commander's head, facing the
   way they face, and presses the game's own "lock relative to the commander"
   for you, so you walk with it. You set no hotkeys and cycle to no preset.

2. `fix.explorer_cam` (default `on`) turns it on or off, live: off releases the
   camera at the next frame. It is a VR setting, and it supports only Elite
   build 332841. On any other build it leaves the game untouched and the log
   says so.

3. Tune the eye with the headset on. The three keys are metres from your
   commander's feet, and they reload about once a second:

   ```
   fix.explorer_cam_eye_up      = 1.68   eye height, held to 0.5..2.5
   fix.explorer_cam_eye_forward = 0.10   + is the way your commander faces, -0.5..0.5
   fix.explorer_cam_eye_right   = 0.0    + is to their right, -0.5..0.5
   ```

Your commander's head is still visible from inside it. Hiding it is the next
phase.

## What it does under the hood

Explorer Cam writes into the game's memory, for one camera only. The free
camera has a small block of state, and the new route changes three things in it:

- **The pose.** Before each update of the free camera, EDVR writes the camera's
  position and orientation in your commander's frame (16 floats), so the game's
  own update puts the camera at the eye you set.
- **The lock.** Once per entry it sets the camera's "lock relative to the
  commander" pressed flag, and restores it after the update. That is the same
  press you could make by hand.
- **The collision.** The camera's update sweeps a small sphere to keep it out of
  walls, which would push it out of your commander's head. While placed, EDVR
  makes that sweep report no hit for this camera and no other.

It never writes the shared record the camera's mode, speed and range are
mirrored from. It does not count your camera keys, does not read the camera's
preset, and does not move the headset pose the game is told about; all three
belonged to the old route.

These safeguards are the reason to trust it:

- It hooks the game's code only after the executable's timestamp and size and
  the first bytes at each hook point match build 332841, and it stands down with
  one log line otherwise.
- It writes only while placed. It lets go when you leave the free camera, when
  you turn `fix.explorer_cam` off, when the game stops calling the camera's
  update, or on a fault; eight faults end it for the session.
- Every access to the game's memory is under a fault guard, and the code that
  runs inside the game's own call takes no lock, allocates nothing and logs
  nothing.

The log's `explorer cam:` lines say what happened: which build, whether the
hooks armed, when the camera was entered and placed, the lock press, a
heartbeat every five seconds while placed, and why it released.
