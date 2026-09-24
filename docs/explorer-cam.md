# Explorer Cam

Explorer Cam moves your viewpoint to your commander's head while you are on
foot in Elite's external camera, which renders in proper stereo. This page
covers setting it up and what it does under the hood; the
[README](../README.md#explorer-cam) has the short version.

**It replaces one camera preset: Commander Right Shoulder.** On that preset the
camera sits at your commander's head in place of the preset's usual framing, so
cycle to it for the 3D view and away from it for normal framing. Every other
preset is untouched, and `advanced.head_offset_view` selects a different preset
to give up.

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
player observes, and no gameplay data read or written.

## Setting it up

1. You do not need to set any hotkeys. EDVR reads your external-camera and
   next-camera-view keys straight from your Elite key configuration, using the
   *on-foot* camera binding, which Elite keeps separate from the ship's. If
   they are on keyboard keys you are done, and the log's first lines name the
   keys it adopted and the file they came from. Rebind them in Elite, even
   mid-session, and EDVR follows within a few seconds. EDVR only *watches*
   these keys; it never presses them or interferes with the game receiving
   them.

   EDVR needs them because on screen, entering the camera looks identical to
   boarding your ship, and the camera key is how EDVR tells which it was. Near
   a planet the game also rebuilds its camera data every few seconds, and the
   next-view key's presses carry "which preset am I on" through the gaps.

   If your camera is bound **only to a controller**, bind a keyboard key for it
   in Elite (Options → Controls) for now. EDVR watches the keyboard, and
   controller support is planned.

2. Get on foot, open the camera, and cycle to **Commander Right Shoulder**, two
   presses from the view the camera opens on.

3. Tune the offsets with the headset on; they reload about once a second:

   ```
   head_offset_right   = -0.25   + is to your commander's right
   head_offset_up      = 0.25    + is up
   head_offset_forward = 1.25    + is the way your commander faces
   ```

   These are tuned for Commander Right Shoulder, which already sits close to
   your commander and faces the way they face, so the numbers are small, and
   the negative `right` brings you off the shoulder onto the centre line. Pick
   a preset several metres further back and `forward` becomes the large one,
   two to three metres instead of one. Treat them as starting points.

These offsets move the viewpoint of a headset you are wearing, so change them a
little at a time. Entering and leaving is a cut, not a glide, because the
game's own camera change is already a cut.

## What it does under the hood

Explorer Cam does two things the other fixes do not:

- It changes the headset position the game is told about. Each frame the game
  asks SteamVR where your head is, and EDVR adds your offset to the answer. The
  game then moves its *own* camera: as far as Elite knows, you leaned. Culling
  and object placement follow that camera, which is what makes it work.
- It reads one number from the game's memory: which external-camera view is
  showing, so the offset applies to the right preset. To find where that number
  lives, it searches once, on the first frame you are on foot, for a marker
  identifying the camera settings. It keeps nothing but the small view index,
  skips the game's code, and never writes.

Like several other fixes, it also follows the game's journal, the documented
file Elite writes in Saved Games for third-party tools, to know when gameplay
has started, when you step onto your feet (where the game resets its camera
view) or back aboard, and when a jump begins and resolves. EDVR reads only the
event names (`LoadGame`, `Disembark`, `Embark`, `StartJump`, `FSDJump`,
`SupercruiseEntry`) and reads or keeps no other content, and
`d3d11.journal_watch = 0` turns it off entirely.

These safeguards are the reason to trust it:

- Nothing happens without your camera key, which EDVR only watches, as
  described above; it never presses or sends it.
- Your viewpoint moves at most 10 m per axis. Beyond that it clamps, because
  refusing outright would snap the view, which is worse when you are wearing
  the headset.
- It counts your camera-key presses, and since build 332753 that is all it
  does. Reading the preset from the game is off by default (`camera_index_track
  = 0`). The read was a correction on top of the press count, and better where
  it worked, because it needs no key bound and cannot drift. But finding the
  records means walking every page the game holds, eleven to seventeen
  gigabytes, and a failed search retries four times. Build 332753 moved the
  marker, so on that build it read fifty to seventy gigabytes per session and
  found nothing. Turning it back on needs a marker measured on your own build;
  [build-332753.md](build-332753.md) has one for 332753 and shows how it was
  arrived at.
- It expires. The two halves of EDVR agree once a frame about which mode you
  are in. If the deciding half stops running, the half that moves your view
  stops trusting it within about a second and puts your viewpoint back.

Counting presses has a cost. The count is anchored to zero at launch and again
at every new on-foot session, which the game's own journal announces, so it is
right unless a press goes unseen. When one does, the count stays wrong until
you leave the camera and come back. With the read on, the next successful read
fixed it for you.
