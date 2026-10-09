# Explorer Cam

## Status

- **State (2026-10-08, redesign branch, not on main).** Built: the Explorer key
  (default `F5`) starts and ends a session; the view is placed at the
  commander's head and locked; the commander's head is hidden; the eye follows
  the head joint; the camera's own keys are blocked while placed; the key that
  ends a session stays the exit until it ends. The old route, which counted
  camera-key presses and offset the headset pose, is deleted. Design, evidence
  and flights are in
  [design-explorer-cam-free-camera-2026-10-07.md](design-explorer-cam-free-camera-2026-10-07.md).
- **Flight qualification.** Flown on Frontier, F0 to F8 (2026-10-07/08): the
  free-camera placement and lock, the head hidden, the eye following the head
  joint, the camera-suite isolation, and F5 from first person ("It works so
  well!", F8). Built and **not flown**: the comfort fade (black while the view
  is not locked and steady, on and off), the F8 menu's Hotkeys page for this key
  (set, change and clear, pad and HOTAS buttons), and the retry of a skeleton
  whose pose is not built yet.
- **Armed by `hotkey.explorer_cam` alone** (default `F5`); empty turns it off.
  The old `[fix] explorer_cam` switch is gone. It is a VR setting and supports
  only Elite build 332841.
- **Your settings (2026-10-08):** `fix.explorer_cam_eye_trim_up`, `_forward`
  and `_right` and `fix.explorer_cam_follow_smoothing_ms` are permanent personal
  preferences, shipped at Sean's own tuning (0.15, -0.08, 0.0 m; 0 ms) and
  set on the F8 menu's Explorer Cam page (built, not flown). No temporary key
  is left: the log-only `advanced.explorer_cam_probe` was removed 2026-10-09.
  The comfort fade has no key.

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
state, the local commander's head draw and the avatars' dither-fade mode,
described below.

## Setting it up

1. Press the Explorer key (`F5` by default) on foot, in first person, in the
   game's camera on any preset, or in its free camera. EDVR opens the camera if
   it is closed, switches to the free camera, places it at your commander's
   head facing the way they face, presses the game's own "lock relative to the
   commander" and hides the camera's on-screen controls. You walk and turn with
   it. Press the key again to leave: the controls come back and the camera
   closes.

   The game's own camera key and TAB place nothing; the Explorer key is the only
   way in. While the view is placed, the camera's own keys are blocked (34
   actions: the free camera's 11, the camera controller's 15, the camera UI's
   one and the zoom/DOF seven), so a stray press cannot unlock, move or close
   it. EDVR's own presses go through. Walking and turning are not touched.

2. `hotkey.explorer_cam` is the key. Set, change or clear it on the F8 menu's
   Hotkeys page (a keyboard key, or a pad or HOTAS button) or in `edvr.ini`.
   Empty turns Explorer Cam off: no hooks are installed and nothing is written
   to the game.

   A change takes effect between sessions. If you change or clear the key while
   a session is on, the old key stays the exit until the session ends, the F8
   row is locked, and the new value applies then. The log says when a change is
   held back.

3. Pressing the key, to go in or to come out, fades the view to black first
   (200 ms) so you never see the camera open or its controls come and go, and
   fades it back in (300 ms) once the camera is placed, locked and steady (on the
   way out, once it has closed). Black never lasts more than 3 seconds, and there
   is no setting for it. If the camera is detached while the session is on, the
   view fades to black in 100 ms and back in 200 ms once it is placed again.

4. The view follows your commander's head joint, so it rises and falls as they
   crouch or raise a weapon, and the head is hidden. If the head joint cannot be
   read (the log says why) the view sits at a fixed eye instead, and only then
   do these three matter. They are metres from your commander's feet and reload
   about once a second:

   ```
   fix.explorer_cam_eye_up      = 1.68   eye height, held to 0.5..2.5
   fix.explorer_cam_eye_forward = 0.10   + is the way your commander faces, -0.5..0.5
   fix.explorer_cam_eye_right   = 0.0    + is to their right, -0.5..0.5
   ```

   To move the head-joint eye to your taste, open the F8 menu's **Explorer Cam**
   page and use its four rows, live and also during a session: Eye height,
   Eye forward and Eye sideways (metres, in steps of 0.01, held to +-0.5;
   `fix.explorer_cam_eye_trim_up`, `_forward` and `_right`, shipped at +0.15,
   -0.08 and 0.00) and Head-follow smoothing (0 to 200 ms in steps of 10;
   `fix.explorer_cam_follow_smoothing_ms`, 0 follows the head exactly). R on a
   row puts back the shipped value. The page tunes the head-joint eye only; the
   three fixed-eye keys above stay in the file.

## What it does under the hood

Explorer Cam writes into the game's memory, for one camera, one avatar and one
global. The free camera has a small block of state, and Explorer Cam changes
three things in it:

- **The pose.** Before each update of the free camera, EDVR writes the camera's
  position and orientation in your commander's frame (16 floats), so the game's
  own update puts the camera at the eye.
- **The lock.** Once per entry it sets the camera's "lock relative to the
  commander" pressed flag, and restores it after the update. That is the same
  press you could make by hand.
- **The collision.** The camera's update sweeps a small sphere to keep it out of
  walls, which would push it out of your commander's head. While placed, EDVR
  makes that sweep report no hit for this camera and no other.

Around those:

- **The head.** The local commander's head parts (head, eyes, helmet, hair and
  the like, twelve of the avatar's fifty-four) are skipped in the draw by
  zeroing their view masks. Only the avatar that is provably yours; no other
  avatar, and nothing else of yours.
- **The camera's own keys.** While placed, the camera's action objects are
  emptied for the game and put back, so their keys reach nothing. EDVR's own
  presses are made through the same objects.
- **The avatars' dither fade.** The game dithers an avatar away when the camera
  is inside it. While placed, EDVR sets the game's dither-fade mode to 0 (every
  avatar draws opaque) and puts it back to -1, the game's own, when the session
  ends. If something else already owns it, EDVR leaves it alone.

It never writes the shared record the camera's mode, speed and range are
mirrored from. It does not count your camera keys, does not read the camera's
preset, and does not move the headset pose the game is told about.

These safeguards are the reason to trust it:

- It hooks the game's code only after the executable's timestamp and size and
  the first bytes at each hook point match build 332841, and it stands down with
  one log line otherwise.
- It writes only while placed. It lets go when you leave the free camera, when
  the session ends, when the game stops calling the camera's update, or on a
  fault; eight faults end it for the session. Eight faults reading the head
  joint stand down only the head source: the placement goes on at the fixed eye.
- Every access to the game's memory is under a fault guard, and the code that
  runs inside the game's own call takes no lock, allocates nothing and logs
  nothing.

The log's `explorer cam:` lines say what happened: which build, whether the
hooks armed, when the camera was entered and placed, the lock press, which
source the eye comes from, a heartbeat every five seconds while placed, the
comfort fade's transitions, and why it released.
