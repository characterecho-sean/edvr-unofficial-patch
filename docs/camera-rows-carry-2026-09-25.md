# Camera rows after a drop: a parked camera's zero

## Status

*Update this block whenever the doc changes. Last updated 2026-09-25, fix
built.*

- **State:** BUILT 2026-09-25 (branch `claude/camera-rows-carry`, merged to
  main), NOT FLOWN. The world path's gate now tracks whether the rows a
  frame measures from are the view's own. A delta that does not turn at all,
  measured from rows a drop left behind, is refused, and the last good real
  delta is carried. The rig (`tools\temporal_test`) replays the dumped
  event and passes. With the refusal disabled it fails exactly those checks.
- **Symptom:** a brief sky/world smear during a roll or turn, two frames
  long, wherever the transition-flash tracker logs a "camera parked at"
  line. Found in eye run 050423 (flight log `edvr_gfx_20260925_050051`,
  Frontier, build c9cab91e = v0.18.0-rc.1).
- **Cause (confirmed from the dump):** after the chooser landed on a parked
  auxiliary pass (148 deg from the view, rejected, carried), the next frame
  took the same pass's identical write. That delta was zero, and under a
  still head a zero passes the 3-degree test. The old gate ACCEPTED it and
  stored it as the last good. The next frame, back on the view, carried
  the zero.
- **Open hypothesis, the entry (H1):** the chooser left the view because
  nothing in 19996 was within 3 deg of 19995's rows: a hitch of about 16
  frames of flight (below). In this dump the hitch was probably the eye
  run's own draw census. The fix does not depend on H1.
- **Open, long stays (H2):** a parked stay carries one delta for its whole
  length. When the pass writes the bound block, nothing else ends a stay,
  because the head-follow score trusts that block. This stay was one
  frame. If stays run long, a stale carry is a worse guess than the head
  path, and the world path should stand down after a few frames. The
  census's "longest stay" is the witness; no limit is built until it says
  one is needed.
- **Ruled out / declined:** see "Declined" at the end. It covers measuring
  from the last accepted rows, and refusing every delta after a drop.
- **Next flight:** Frontier install. Roll near a station or a planet where
  the log shows "transition flash: a camera parked at" lines, and take an
  eye dump while rolling. Pass condition in "Verification".
- **Environment:** depends on the world path (`fix.temporal_aa`, depth
  motion) and the game's view rows at float 932 of the scene block. It does
  not depend on the VR runtime, headset or DLSS version, since it reads the
  game's own rows. Flown rig: Pimax (runtime "Pimax OpenXR") on EDVR's
  native runtime, DLSS 2037x1969 -> 4074x3938 per eye. Fixed-size table:
  the chooser's ring of 256 writes (unchanged).

## The event (eye run 050423, eye 0)

`eye_050423_motion.csv`, crops 0 to 5, turn and move computed from the
rows:

| frame | rows prev -> now | turn | move | rowsOk | world delta used |
|---|---|---|---|---|---|
| 19994 | view -> view | 0.72 deg | 0.73 | 1 | its own |
| 19995 | view -> view | 0.88 deg | 0.76 | 1 | its own (D95) |
| 19996 | view -> AUX | 148.45 deg | 21.65 | 0 | D95 carried |
| 19997 | AUX -> AUX (identical) | 0.000 | 0.000 | **1** | **identity, accepted** |
| 19998 | AUX -> view | 148.36 deg | 7.47 | 0 | **identity carried** |
| 19999 | view -> view | 1.56 deg | 1.73 | 1 | its own |

AUX is `(-199.5, 1491.6, 779.8)`. It matches the transition-flash tracker's
"parked" cameras in the same session (`(-170 +1170 +595)` at 05:04:18,
`(-204 +1548 +812)` at 05:04:26). Those cameras sit along the ship's path,
re-placed every second or two. `rowsBound` is 1 on every frame, so the
auxiliary pass writes into the same block as the view.

The census line at 05:04:31 covers the whole session (3 min 40 s). The
delta was dropped on 120 eye-frames as another camera's: 60 frames over two
eyes. An event of this shape drops two (the entry and the exit), so that is
up to 30 events, about one every 7 s. The chooser resynced ("nothing
followed last frame's") on 52 frames. Only stays of two frames or more hit
the bug: a one-frame stay has no frame in between to accept a zero. The
accepted zeros were counted nowhere; the new "parked" count is their
witness.

## The entry (H1): a hitch

On every frame the turn and the move keep the same ratio, about 0.9 deg
per unit: a steady roll and speed, with each frame's share scaling with its
frame time (20008, a long frame, has 2.88 deg and 3.19 units). From 19995
to 19998 the view turned 25.9 deg and moved 29.1 units. That is the same
ratio, so it is the same flight, but it is about 16 frames of it in three.
So the view's own write in 19996 was more than 3 deg from 19995's rows, the
chooser found nothing continuous, and its resync took the bound block's
latest write, which was the parked pass's. The eye run armed its draw
census at 05:04:23.499 (`DC begin census=1 frames=3`), which wrote 10,804
log lines in two seconds. That is the likely hitch in this dump. The rest
of the session's events happened without a dump, so hitches in ordinary
flight also enter this way. A dump taken with the census off, or a timed
frame log, would separate the two.

Nothing reprojects a quarter-second hitch. The view's own delta at 19996
was well over 3 deg and would have been dropped anyway. Carrying the last
real delta across it is the best available.

## The mechanism

`chooseCameraRows` follows continuity from last frame's pick, and the
plausibility block measured each frame's delta from last frame's pick. Once
a frame was rejected, its rows (the parked pass) became the next frame's
reference for both of them. The chooser then found the parked pass's
identical write continuous (the twin fallback, "for a camera that truly
stood still"). The gate saw a zero delta against a head turning 0.02 deg
and passed it, because the 3-degree test only bounds the ship's turn and a
zero is a plausible ship turn. Stored as the last good, the zero was then
carried by the exit frame.

## The fix (temporal_math.h: `temporalCameraGateStep`, `temporalCameraGateAdvance`)

- The gate keeps a standing for the reference rows (`refOwn`). It moves on
  once a frame, at `temporalPassFrameBoundary`. The rows are the view's own
  only if a delta to them was measured and neither eye refused it. Rows no
  delta reached, such as the first after a frame with no write, are not.
- A delta measured from rows that are not the view's own, whose orientation
  matches them to the last bit, is refused (`Parked`). A tracked head does
  not hold the same orientation from one frame to the next, so rows that
  did not turn at all belong to a parked or world-fixed camera. The last
  good delta is carried, and the rows stay suspect through the whole stay,
  including a pass re-parked with the same orientation.
- A delta that turns and passes the 3 degrees restores the standing. The
  view resuming after a hitch or a resync (19999) is accepted at once, as
  before, so recovery costs no frame.
- Unchanged: the 3-degree test (`Another`); the floating origin's jump
  (the translation dropped before the last-good store); the carried-delta
  witnesses (`g_camCarried`, and `g_camCarriedJump`, zero by construction);
  a still twin after the view's own rows is still accepted as the identity.
- New census clause: "on N as a parked camera's (not turned at all, from
  rows a drop had left; the longest stay M frames)". N counts eye-frames;
  M counts frames, once for both eyes. The eye trace's `rowsOk` is 0 on a
  refused frame.
- The world delta arithmetic moved unchanged from the pass's lambda into
  `temporalWorldFromRows`, so the rig runs the pass's own code. The rig
  reproduces the DLL's dumped `cameraR`/`cameraTv` for 19995 and 19999 to
  1e-5.

## Verification

Next flight, Frontier install:

```bash
python tools\install_edvr.py --target frontier --verify-only
python tools\edvr_log.py --target frontier --expect-build HEAD --version
python tools\edvr_log.py --target frontier --grep "registration, the rest|camera parked at"
```

- Pass: in `eye_<stamp>_motion.csv`, no frame that follows a `rowsOk 0`
  frame shows a zero prev -> now rotation with `rowsOk 1` during a roll. A
  refused frame shows `rowsOk 0`, and its `cameraR` is the last real delta
  (about a degree of roll), not the identity.
- The census's new "parked camera's" count is non-zero on a flight with
  parked-camera lines. That is the witness: those are the frames the old
  gate would have accepted as zero. Its "longest stay" answers H2: a few
  frames leaves the carry as it is, and tens of frames call for the stand-
  down.
- Fail signatures: a `rowsOk 1` identity after a `rowsOk 0` during a roll.
  Either the fix is not running (check the version line), or the parked
  pass's rows changed orientation by a hair between frames, which is a
  variant that bitwise identity does not catch and needs its own look. A
  world that visibly spins on after a roll has stopped, with a long stay
  on the census, is H2.

## Declined

- **Measuring the next delta from the last ACCEPTED rows** (asked for as
  the first option): after a real jump of more than 3 deg (a hitch), every
  later frame is measured across the jump from a stale anchor, and none is
  accepted again. It would need an escape hatch, and that hatch is this
  fix's recovery rule.
- **Refusing every delta after a drop** (the second option, literally): it
  either never recovers, if a refused frame counts as a drop, or accepts
  the second frame of a two-frame parked stay, if only the 3-degree test
  counts. The standing is the working form of it: refuse what did not turn,
  and restore on what did.
- **A tighter angle test against the carried delta** (the ship's turn rate
  is continuous): it needs a threshold between 0.45 deg (19999 against
  19995's rate, a real change of frame time) and 0.88 deg (a zero against
  19995). That is a threshold nudge, not a cause.
- **Changing the chooser's reference after a drop:** the chooser must keep
  following the rejected rows. After a hitch those rows ARE the view, and
  continuity from them is how the view is found again.
