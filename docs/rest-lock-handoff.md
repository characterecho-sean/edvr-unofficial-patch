# Handoff: the rest lock meets the temporal pass

*For the agent working on `claude/temporal-aa`. Written 2026-09-03 at the
end of the day the shimmer was traced to its cause; everything below is
measured on Sean's rig unless it says believed. Read this before merging
main into your branch — the merge has one ordering rule that decides
whether the temporal pass gets the benefit at all.*

## The short version

The shimmer on a steady ship — the flicker you logged as "faint shimmer at
rest" in flights 5 to 7, and blamed on the variance clip's box hopping
with the jitter — has one cause, and it is not in the pass. The runtime's
reported orientation wanders about a tenth of an arcminute every frame
while the headset is physically still, the game renders every frame from
that pose, and the compositor re-warps every frame by the same motion. A
one-pixel line blinks as it crosses pixel rows. Holding the render pose
and telling the compositor the frame's predicted display pose stopped it
in the headset. That is now `fix.shimmer_rest` on main.

For you this means two things:

1. **Your history can be exactly registered at rest.** With the render
   pose held, consecutive frames are rendered from the same pose, so the
   head delta the pass reprojects by is zero while the head is still. No
   history resampling, no compounding softness at rest — the trade Sean
   rejected ("text much sharper without the TAA") was mostly resampling
   under sub-pixel motion that no longer exists while held.
2. **Your jitter becomes a true supersampler of a static scene.** With the
   pose held, the eight Halton offsets sample the same scene at eight
   sub-pixel positions, and the history converges to their mean. That is
   what draws the seam's dashes (below) as a continuous line, which no
   filter at the door can do.

There is one ordering rule for the merge and it is in the section after
the state of the branches. Get that one right and the rest is conflict
resolution.

## State of the branches

| Branch | Tip | Carries |
|---|---|---|
| `main` | `f0aa415` | `shimmer_rest` (defaults still 0.3 / 1.5 there), the `pose_hold` instrument, the turn and drift columns in the pose history, the Frontier report, the conservative-raster revert |
| `claude/beautiful-thompson-8f3a52` | `55d1923` | main plus `fix.texture_lod_bias` (unflown as a fix, off by default), the anisotropic-filter finding in the docs, and the rest lock's defaults moved to **0.6 / 2.0** — the values a worn head needs |
| `claude/temporal-aa` | `02e2582` | your sixteen commits off `00e96d5`; none of the above |

Take main first. Cherry-pick `55d1923` for the 0.6 / 2.0 defaults (it
touches the rest lock's fields in `compositor_hook.cpp`, the `[advanced]`
block in `edvr.ini`, and three docs; it applies without the bias key). Take
the bias commit `293fd90` only if Sean has said the bias stays.

Files both branches touch: `src/openvr/compositor_hook.cpp`, `edvr.ini`,
`docs/anti-aliasing.md`. Expect conflicts in all three; the code ones are
listed below.

## The measurements, so you do not have to re-derive them

Headset lying on a desk, Pimax Crystal Super, native SteamVR, two captures
of 1200 frames at 90 Hz, the HMD pose from `WaitGetPoses`:

| Quantity | Reading |
|---|---|
| Position change per frame | under 0.05 mm, every frame |
| Orientation change per frame, mean | 0.11 to 0.14 arcmin |
| Largest one-frame turn | 0.46 arcmin, 1.63 arcmin |
| Frames turning more than 0.6 arcmin (about half a rendered pixel) | 26 of 2400 |
| A worn head "held still", smoothed speed | 0.3 to 0.6 arcmin per frame |

The orientation figures come from the antisymmetric part of the relative
rotation between frames. Do not use the trace route — `acos((tr - 1) / 2)`
reads float32 rounding of the pose matrix as about two arcminutes of
phantom turn per frame, and the first field dump did exactly that. The
helper is `turnArcminBetween` in `compositor_hook.cpp`.

What was excluded and how (the long form is in
`docs/frontier-shimmer-report.md`): streaming compression (same on
DisplayPort), lighting (indifferent to head roll; the shimmer is not), the
compositor and panel (the game's mirror window, which shows the left eye
as rendered, blinks with the headset on a desk), the resolve kernel (calm
and crisp at radius 2 at 1.5x, unchanged), post AA (same with Off and
SMAA), tracking position (steady). A 25-degree head roll stops it, because
a diagonal line only slides its own stair pattern along itself.

**The dashes.** With the pose held, the seam that used to shimmer is a row
of bright dashes. Conservative rasterisation on every solid state did not
change them (and drew triangle artifacts elsewhere; reverted), so they are
not geometry. The game's Texture Quality at low did not change them;
switching Texture Filter Quality from 16x anisotropic to trilinear removed
them. They are anisotropic filtering's spaced taps missing a one-texel
ridge across the footprint's long axis on a grazing surface. Your jitter
moves those taps a fraction of a texel per frame, so the history's mean is
the ridge's true coverage. Expect the seam to become a continuous, dimmer
line under the pass with the pose held. That is the headline verdict of
the first flight.

**The residual.** With the hold engaged at 86 to 90 fps, a distant
periodic floor pattern still sparkles. That is the compositor's own
display prediction disagreeing with the one we submit — two predictions
built from different gyro samples, about 0.1 arcmin apart every frame —
applied as a warp after everything EDVR does. A seam tolerates a tenth of
a pixel; a two-pixel-period pattern does not. The pass cannot remove that
warp, but it band-limits the content that exposes it. Judge the floor
pattern as a secondary verdict, not the primary one.

## What `shimmer_rest` does, precisely

`src/openvr/compositor_hook.cpp` (`restApply`, `forwardSubmit`,
`predictDisplayPose`) and `src/common/rest_math.h` (Shepperd's
matrix-to-quaternion, nlerp along the shorter arc, the hold curve).

- **Every frame in `hookedWaitGetPoses`, after the pose ring has recorded
  the raw pose and after the `pose_hold` instrument:** the head's speed is
  the turn since the last frame plus its travel scaled to two metres (a
  millimetre counts 1.7 arcmin), capped at 100, smoothed with an EMA of
  0.25. The hold factor *k* is 0.02 at and under `shimmer_rest_still`,
  1 at and over `shimmer_rest_moving`, linear between. The held rotation
  and position move *k* of the way toward the live ones and go out in
  both pose arrays; velocities are scaled by *k*. A gap over 30 arcmin or
  20 mm while the smoothed speed is under `moving` snaps the held pose to
  the live one. The pose the game got this frame is kept in `restRender`.
- **Every forwarded submit while *k* < 0.999:** the texture leaves as a
  `VRTextureWithPose_t` whose pose is `nlerp(display, render, k)`, where
  `display` is the HMD pose predicted to this frame's photon time, fetched
  once per frame through IVRSystem_012 slots 5, 10 and 22 in the
  compositor's tracking space (slot 1 of the compositor). At *k* = 0 the
  compositor is told the display pose and has nothing to re-warp; at
  *k* = 1 it is told the render pose and warps as stock, so the late warp
  keeps its latency compensation in motion. Both eyes are told the same
  pose: SteamVR keeps only the later submit's (ValveSoftware/openvr#1253).
- **Keys:** `fix.shimmer_rest = off | on` (live), `advanced.shimmer_rest_still`,
  `advanced.shimmer_rest_moving` (live; 0.6 / 2.0 after `55d1923`).
  `advanced.pose_hold = 0 | game | headset` is the instrument that proved
  it and still wins while on.
- **Log:** "shimmer rest: on -- ..." at prime; "shimmer rest totals:" every
  20 s with held / easing / stock shares, snaps, submits that carried a
  pose and fell back, the smoothed speed and the current factor. A rig
  where "carried" stays at zero is one where `systemInterfaceV012()` or its
  vtable prefix refused; the reason is worth a line.

## The merge: one ordering rule, then conflicts

**The rule.** Your `temporalAaNotePose` (and `temporalAaNoteGamePose`)
currently sit inside the pose ring's record block in `hookedWaitGetPoses`,
which records the RAW pose on purpose. After the merge they must record
the pose the game actually renders from — the HELD one — or the pass will
reproject a still frame by the tracker's raw delta and misregister its
history by exactly the motion the rest lock removed. Move both calls to
after `restApply(...)` and after the theater's freeze block, before
`headOffsetApply(...)` (the offset is a constant that cancels in the delta,
and v1 deliberately noted before it). The order in that function must
read:

1. `launchCentreApply` (origin shift)
2. pacing, validation, the glitch and hold bookkeeping
3. the pose ring record (raw)
4. the `pose_hold` instrument
5. `restApply` (the rest lock)
6. the theater's freeze
7. **`temporalAaNotePose` / `temporalAaNoteGamePose`** ← here
8. `headOffsetApply`
9. config reload poll

**hookedSubmit.** Main routes every forwarding site after the two early
outs through `forwardSubmit(s, self, eye, tex, bounds, flags)`; there are
five on main (the FSS heal, the withhold shadow, the theater, the plain
forward, and the final path). Your branch has the same five, still on
`s->realSubmit`. After the merge all five go through `forwardSubmit`, and
on the final path the order is `applyTemporal` → `applyCullGuard` →
`applyResolve` → `forwardSubmit`. The pose wrapper then rides on your
pass's output texture, which is what you want. A grep for
`s->realSubmit(self, eye,` after the `if (s->inert)` line should return
nothing.

**`State`.** Both branches add fields; keep both sets. The rest lock's are
the `rest*` block plus `poseHold*`; yours are the pass's.

**`edvr.ini`.** Keep both branches' blocks. `build.bat`'s config contract
will tell you if a documented key and a read key disagree; the count on
main is 172 documented, on my branch 174.

**`docs/anti-aliasing.md`.** Keep both; main's new section is "The
tracker never rests: the rest lock", inserted before "Guidance for
players now". Your Feature B section should gain a sentence that the rest
lock supplies the registered history at rest.

**`openvr_min.h`.** Main adds `VRTextureWithPose_t`. If your branch added
its own definition for the sharpen or DLSS work, keep one.

## Interplay with the pass, in detail

- **The jitter is independent of the pose** (it edits the projection), so
  it keeps running while the pose is held. That is the point.
- **The head delta at rest is zero, not small.** Your registration line's
  "still" bucket (under 0.03 deg/frame) should show its clip share fall
  toward the no-motion floor with the lock on. That line is the cleanest
  instrument you have for "the merge is right".
- **`advanced.temporal_aa_snap` is gone (2026-09-04).** The hold removes
  the sub-pixel drift the snap was for, and the snap turned out to
  accumulate a lag on slowly drifting content (the review of that evening,
  docs/review-temporal-far-warp-darkness-2026-09-04.md, F4); the cleanup
  pass removed it with the other retired levers.
- **Depth reprojection (v2) and the DLSS / DLAA modes** take their motion
  from the noted poses, so they inherit the fix through the same call.
- **Withholds and the theater.** `forwardSubmit` wraps whatever texture
  leaves, including a withheld shadow copy. At rest that is harmless (the
  copy is a still, told the display pose, displayed unwarped); in motion
  those paths are transitional frames anyway. If you ever see a withhold
  land visibly off-pose, exclude those paths from the wrapper and say so.
- **`pose_hold` still wins.** While the instrument is on, `restApply`
  stands aside and the pass sees the instrument's held pose. Useful for
  A/B: `pose_hold = headset` is the hard version of what the lock does
  softly.

## The flight that settles it

Frontier install (it is the experiment rig; the Steam install is yours —
check the version line in the vr log before believing any flight, that
mistake has been made once already today), main menu, headset on:

1. `shimmer_rest = on`, `temporal_aa = on`, jitter on, HMD Quality 1.25 at
   SteamVR 64% (renders 4340, holds 86 to 90 fps on the Pimax; below
   refresh the compositor's repeated frames carry a full vsync of jitter
   and muddy the reading).
2. Head still, look at the ship's near-horizontal seam. Verdict one: the
   dashes should integrate into a continuous dimmer line within a second.
3. Look at the menu text. Verdict two: it should be as sharp as without
   the pass while still, since the history is never resampled while held.
4. Turn the head slowly and stop. Verdict three: no swim, no hop, and the
   seam settles again within a second.
5. Read the totals line and the registration line together: held share
   high at rest, "still" clip share near the floor, carried submits equal
   to forwarded frames, fallbacks zero.

If verdict one fails while the totals show the lock held, the pass's
history is not seeing the held pose — the ordering rule above is the first
thing to check.

## Pointers

- `docs/frontier-shimmer-report.md` — the full measured account, written
  for Frontier; the excluded-suspects table is there.
- `docs/anti-aliasing.md`, "The tracker never rests" — the design-doc
  record, and the guidance bullets.
- `src/common/rest_math.h` — the arithmetic, with the reasoning in its
  header comment.
- Memory (Sean's Claude memory, `edvr-hull-line-shimmer.md`) — the
  day's flight log, in order, if you have access to it.
