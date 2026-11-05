# Holo-Me head flick in VR (2026-11-05)

## Status

- **State: fixed by Sean's eye and merged to main on his word.** Two separate
  defects, both measured. Flown on `ae4e29b8` (Frontier, log
  `edvr_gfx_20261105_134508`): Sean, "Fixed". That flight took no eye dump, so the
  numbers after the fix are not measured, and the cockpit and on-foot checks below
  were not reported. No ini edited.
- **Reports (Sean, 2026-11-05):** "just found my head flicking in Holo-Me"; after
  the test flight, "definitely flickers more when the ship is flying. Though the
  helmet frame still seems to at rest".
- **Evidence:** dump `120817532001` (log `edvr_gfx_20261105_120548`, `852102d1`).
  Test flight log `edvr_gfx_20261105_124851` (`dabfff15`, the second skin files
  unchanged from `852102d1`): dumps `125144765001` stopped and `125253331002`
  flying. All Frontier, profile `vr`, DLSS preset K, 2016x2017 in, 4032x4034 out.
- **Defect 1, the face when flying (proven):** the second skin stores motion (E,
  from F2) in `R16G16B16A16_FLOAT` centimetres. On a flying ship the commander's E
  also holds the ship's travel, a median of 348 cm a frame in the test against
  0.018 cm stopped, so one half-float step is 0.25 to 1 cm. The face's motion is up
  to 5.0 native px wrong (8.3 in a hitch frame) when flying and at most 0.17 when
  stopped.
- **Defect 2, the helmet frame at rest (proven):** draw `01DA82F8`/`2C3ADCD2`
  (helmet, headset, collar, suit) exports no E, so its motion leaves out the
  avatar's own animation. Stopped, the helmet is up to 1.75 input px (3.5 native)
  wrong, and that error tracks the head's motion in the same frames (r = 0.985,
  slope 0.93). The same class as the NPC hair of 2026-11-01.
- **Scope:** defect 1 needs a fast-moving frame: of 133 log windows with joined
  pixels in the Frontier and Epic logs (the compose appears to count them only in a
  window holding an eye dump), six had a median |E| of 100 cm or more (before this
  test), five of them aboard a ship. Defect 2 reaches any draw of this pair in VR
  while the character animates; only Holo-Me is measured.
- **Ruled out:** see "Ruled out" below (DoF, the flat fix, the switch fix, jitter, a
  flick in the game's pose).
- **Open:** the visor glint (real, in the game's own image, not what Sean named);
  float32 position precision if the screen stays open for minutes in flight; how
  much the helmet's thin lines shimmer from the input alone (see "After a fix").
- **The build (Sean: "Build both fixes"):** target 7 is `R32G32B32A32_FLOAT` and the
  pair `01DA82F8`/`2C3ADCD2` is keyed as a VR-only skinned family. Keyed alone it
  would spread the rounding to the suit, so the two go together. No key. Full build
  green; the real pair passes the clone test and the corpus identity test.
- **Next flight:** none owed. The next time Holo-Me is open, an INSERT in each state
  gives the numbers in "The fix's flight", and the cockpit and an NPC on foot are worth
  a look for the regression.
- **Temporary keys:** none added. Frontier's `census_offscreen = 1` belongs to the
  lobby DoF work and was left alone.
- **Issues:** none filed yet.
- **Measuring tool:** `python tools\character_motion_check.py --target frontier
  [--stamp S]`, gated in the build. It reads a dump's face, helmet and body against
  the raw input, and exits 3 past its limits. Baselines below.

## Environment

Pimax Crystal Super, Pimax OpenXR, through EDVR's native OpenXR runtime, RTX 5090.
The per-eye output is 4032x4034 and DLSS runs in performance mode at 50% per axis.
The log does not read the DLSS runtime's version. E is VR only; flat does not use
this path.

## How the error is measured

Warp the previous raw input (`C`) through the frame's own motion (`D`), remove the
jitter difference taken from `motion.csv`, and find the extra offset that best
matches this input. Zero means the motion is right. There is no DLSS response in
this measure, because `C` is what DLSS is handed.

Correction: the first version subtracted a control region under the head rather than
the jitter. That is safe only if the body is still. In the stopped dump the body
moves, and the control reported 3.4 native px on a face that is exact. All numbers
here use the jitter from `motion.csv`.

**Contamination.** Each capture has its own hitch: in the first dump the per-frame
travel is 3.58, 3.62, 19.41, 33.20 m, then 6.5 to 8.9 with 15.04 at frame 14. Travel
follows the time step, so from frame 4 the capture runs at about half speed and E,
with its error, is about twice a 90 Hz frame's. Frames 4 to 15 are the ones to read.
Frame 5 of the flying dump is a further hitch (2083 cm). The closed eyes in frames 0
to 2 of the first dump are a blink, and they are in the raw input.

## Defect 1: E rounded to half float

First dump, face only, frames 4 to 15:

| Frame | Travel (m) | One E step in MV-x (input px) | Largest jump in MV-x inside the face | Face error (native px) |
|---|---|---|---|---|
| 4 | 7.15 | 0.52 | 0.52 | -0.25, +0.39 |
| 5 | 6.50 | 0.52 | 0.51 | -0.75, -0.58 |
| 6 | 8.87 | 1.04 | 1.02 | -0.50, +0.42 |
| 7 | 7.44 | 0.52 | 0.02 | -1.00, -0.36 |
| 8 | 6.59 | 0.52 | 0.01 | -0.25, +0.17 |
| 9 | 7.00 | 0.52 | 0.53 | -1.88, +1.17 |
| 10 | 6.68 | 0.52 | 0.54 | -1.88, +1.14 |
| 11 | 8.48 | 1.04 | 0.02 | +1.00, +0.17 |
| 12 | 8.15 | 1.04 | 0.04 | +0.50, +2.14 |
| 13 | 8.45 | 1.04 | 1.05 | +0.25, +0.17 |
| 14 | 15.04 | 1.04 | 1.05 | -1.50, +3.17 |
| 15 | 8.81 | 1.04 | 1.01 | -1.50, +0.89 |

A rigid face cannot have its motion step across a sharp line, and a render of
frame 14's `D` shows exactly that. Where a jump exists it matches one half-float step
of E's x component to within 4%, computed from the camera rows, the face's depth
(1.96 m) and 947 input px per unit of tangent. Frames with no jump have no step
crossing the face; their whole-face error comes from the other components.

The test flight then separates it by speed, on the same screen and the same build:

| Frame | Stopped: face error (native px) | Flying: travel (cm) | Flying: face error (native px) | Flying: head motion E claims (input px) |
|---|---|---|---|---|
| 4 | 0.00, +0.17 | 485 | -0.25, +0.17 | -0.53, -0.45 |
| 5 | 0.00, -0.11 | 2083 | -1.50, +8.14 | +0.60, -3.16 |
| 6 | 0.00, -0.08 | 783 | -1.00, +1.67 | +0.21, -0.97 |
| 7 | +0.12, -0.08 | 886 | +0.75, -3.58 | -0.30, +1.75 |
| 8 | +0.12, -0.11 | 622 | -0.75, +2.64 | +0.43, -1.42 |
| 9 | 0.00, +0.17 | 603 | +0.50, +1.67 | -0.27, -0.99 |
| 10 | 0.00, -0.11 | 677 | -3.38, -2.83 | +1.61, +1.34 |
| 11 | 0.00, -0.08 | 631 | -1.88, -0.11 | +0.51, +0.01 |
| 12 | 0.00, -0.08 | 610 | +1.00, +0.17 | -1.17, -0.49 |
| 13 | 0.00, +0.14 | 694 | -1.25, -3.11 | +1.20, +1.36 |
| 14 | 0.00, -0.08 | 614 | 0.00, +1.92 | -0.45, -0.93 |
| 15 | -0.12, -0.08 | 657 | -4.25, +2.67 | +1.77, -1.22 |

The last two columns are the point. The head motion E claims when flying is almost
exactly the opposite of the measured error (r = -0.93 in x, -0.99 in y, slopes -0.86
and -1.14), so what it claims is noise: the head really moved 0.7 input px or less on
every one of those frames. Frame 0's `SkinE` shows the cause directly: in the first
dump x runs -114.69 to -114.50 (4 values), y -283.5 to -283.25 (2 values), and z
185.75 to 185.875 (2 values), beside camera travel of -114.61, -283.55, +185.91.

Both eyes share it. Final-image face residual, left/right, first dump: frame 8
2.83/2.97, frame 10 5.09/5.14, frame 14 5.65/5.99. E is a world-space value, so the
error is the same in each eye.

At 90 Hz the first dump's frames 0 and 1 (3.6 m of travel) bound the whole-face error
at about 0.6 and 1.0 native px, and long frames raise it two to three times. That is
computed; a dump cannot run at 90 Hz.

## Defect 2: the helmet frame has no skeletal motion

Stopped dump, in input px. The helmet mask is the depth writer map's pixels for
`01DA82F8` with 16122 indices, dilated 2 px, on the head path. Body is head-path
pixels under the face.

| Frame | Head motion (face minus body) | Helmet error | Body error | Helmet residual, then at the best offset | Face residual |
|---|---|---|---|---|---|
| 4 | +0.35, +0.13 | +0.38, +0.25 | +0.25, +0.12 | 5.53, 3.44 | 1.79 |
| 5 | +0.27, +0.19 | +0.25, 0.00 | 0.00, +0.12 | 5.49, 4.93 | 1.92 |
| 6 | +0.34, +0.17 | +0.12, +0.12 | +0.12, +0.12 | 3.89, 3.51 | 1.47 |
| 7 | +0.52, +0.25 | +0.50, 0.00 | +0.25, 0.00 | 6.21, 3.64 | 1.87 |
| 8 | +0.18, +0.06 | +0.25, 0.00 | +0.25, 0.00 | 3.79, 2.84 | 1.53 |
| 9 | +0.35, +0.01 | +0.38, +0.12 | +0.25, +0.12 | 5.42, 3.81 | 1.77 |
| 10 | +1.63, +0.30 | +1.50, 0.00 | +1.12, +0.25 | 12.02, 3.93 | 1.91 |
| 11 | +1.81, +0.22 | +1.75, +0.25 | +1.62, +0.50 | 12.35, 4.32 | 1.78 |
| 12 | +1.34, +0.03 | +1.25, -0.12 | +1.00, 0.00 | 9.61, 3.79 | 1.77 |
| 13 | +1.05, +0.15 | +0.88, +0.12 | +0.75, +0.25 | 8.50, 4.43 | 2.30 |
| 14 | +1.02, +0.13 | +0.75, -0.12 | +0.75, +0.25 | 8.60, 3.71 | 2.35 |
| 15 | +1.35, +0.11 | +1.25, 0.00 | +1.12, +0.38 | 9.65, 3.88 | 1.79 |

In x, helmet error against head motion: r = 0.985, slope 0.93. Body: r = 0.975, slope
0.85. The face right beside it has zero error, because it has E. A frame-to-frame
change map of the final image shows the helmet and suit edges bright and the face
dark except the eyes and moustache. The helmet's error is therefore not noise: it is
the avatar's real motion, missing from the vectors. Thin bright lines show that
more than a broad surface does, which fits what Sean sees on the helmet frame.

The first dump's small helmet error is consistent: its head really moved little in
those frames, and most of its apparent head motion was defect 1's noise. The flying
test dump is the same (helmet 0.5 input px or less).

Why the pair is unkeyed: `kFamilies` and `kSkinPairs` hold D99A, 61AE, 114A, 7B0D, 8B58
and A6A3 only, and no doc has looked at 01DA as a skinned character before. Aboard a
ship these pixels fall inside the 10 m split and get the head path.

## The draws that paint the head

From the census and the depth writer map, first dump.

| Draw (vs/ps) | Indices | Pass | What it is | Motion path |
|---|---|---|---|---|
| `D99AFDC2`/`E86271E4` | 22434 | G-buffer | face skin | engine, E valid |
| `114AF608`/`A17504A2` | 8145 | G-buffer | hair | engine |
| `01DA82F8`/`2C3ADCD2` | 16122 | G-buffer | helmet, headset, collar | head, no E |
| `01DA82F8`/`2C3ADCD2` | 23430 and others | G-buffer | suit | head, no E |
| `617C6E44`/`057F8E27` | 22434 | forward, additive | the face mesh a second time | not keyed |
| `025B4B9F`/`C5A5C7E8` | 11904 | forward, alpha blended, no depth write | not identified; changes probe points off the head | not keyed |
| `C171BD0C`/`08646D68` | 5244 | forward, additive, no depth write | not identified; a candidate for the visor | not keyed |

## Ruled out

- ruled out: depth of field in the Holo-Me scene, because the setting was 2 (the next
  change is `e+0x7C 2->0` at 12:13:07) and FDB74215, 80631A09, E16E27A0 and BF2302BC
  are in none of the first dump's three census frames, eye or offscreen.
- ruled out: the flat Holo-Me upscale fix, because this is the VR profile with no
  switch, there are no flat copy lines, and `d933ebab` is not in `852102d1`.
- ruled out: the second skin at the switch (`11ef91d5`), because these flights started
  in VR with the skin armed at load, so that path never ran. E's format dates from F2
  (`1c23352a`).
- ruled out: jitter as the cause, because the stopped face takes the same jitter with
  no error.
- ruled out: a flick in the game's pose, because the head bones' rotation per unit of
  time is steady at 0.021 to 0.036 degrees over the first dump's frames 4 to 15, and
  the ledger's previous palette is bit for bit last frame's.
- ruled out, then reversed: "the helmet and collar are not the main error". The first
  dump alone suggested it because the head barely moved there. The stopped dump
  reverses it.

## The build

**1. Target 7 (`makeSkinTarget`) is `DXGI_FORMAT_R32G32B32A32_FLOAT`.** The shaders read
it as `float4` and do not change. The step at 33 m of travel falls to about 0.0002 cm.
It reports `RENDER_TARGET` and `BLENDABLE` on this GPU; on one that refuses it,
`makeSkinTarget` logs that and the eye goes on without the second skin. Cost: the eye
targets go from 32.5 to 65 MB each at 2016x2017, so +65 MB, plus their clear, and F2's
on-foot source target doubles when live. The eye dump's writer now saves this format,
and the jump lattice takes it as its source and keeps half-float samples; its reader
accepts old and new files. `L4.e` is the new check: E stays exact at a ship's travel,
where half float would be out by 0.12 cm. The alternative, half float without the
shared camera travel, costs no memory, but the skin clone needs the previous camera
origin as new data: a wider patch for 65 MB.

**2. `01DA82F8`/`2C3ADCD2` is in `kFamilies` (VR only, skinned) and `kSkinPairs`,** as
A6A3 was. It takes the default E register. The real pair, from Frontier's shader dump:

- `skin_clone_test --corpus`: 2067 covered pixels in the identity test, E exactly zero;
  pose, palette, swap, no-history, teeth and limit all pass, 63 checks.
- `engine_velocity_test --corpus --partial`: patched, reflected, created; 40960 texels
  compared, 0 mismatches; 577 covered, MRT6 577 checked, 0 bad.

The full corpus run did not finish in 9 minutes on WARP. Every pair it reached was
identical, but it was stopped before this pair, so this pair ran alone in a folder with
only its two shaders.

Risks the flight has to read:

- **Base 0.** The compose reads E only for a record with a nonzero base, and any other
  record takes the rigid path, joined or masked. The pool copies do not settle which
  these are. The first dump's `EnginePool` shows bases 247, 1490 and 186 on these draws
  (61-bone jobs), but the two test dumps read 0 at the same indices, in records that are
  plainly not the avatar's, so those copies are not of the eye pass's generation. If
  they are base 0, the helmet will be masked or rigid, and it will look worse or no
  better. The dump's `D` flags answer this at once.
- **Other draws of the pair.** The flat design doc lists it among a cockpit's main
  pairs. In the 40 newest Frontier logs its census draws are only the suit's five parts
  (16122, 20388, 23430, 11568, 32358 indices), and only in the Holo-Me flights. A keyed
  draw whose join fails has no history, by design; the test windows show 4,705 to 11,801
  `pose` failures in 35,000 to 46,000 jobs, and which jobs fail is not known.
- **The clone's cost** on every draw of the pair: five draws an eye on this screen.

**After the fix.** The helmet's residual at its best offset is still 2.8 to 4.9, against
1.5 to 2.4 for the face. Thin lines at 50% render alias in the input, so some shimmer may
stay. Whether DLSS resolves it once the motion is right is only measurable now.

`analysis\npc_blur\f2\h1_skin_e.py` still requires the old format and needs the same
change through the analysis intake.

## The fix's flight

On the fix's build, main merged: Holo-Me stopped and flying, INSERT in each, watching
the face and the helmet frame. Then a look round the cockpit in flight, and an NPC on
foot for the regression.

`python tools\character_motion_check.py --target frontier --stamp <stamp>` reads each
dump. Its baselines, on the dumps from before the fixes, with the tighter masks it uses
(so a little different from the tables above):

| Dump | Face, worst (native px) | Helmet, worst (native px) | SkinE format |
|---|---|---|---|
| `120817532001`, flying | 3.58, 8 of 12 frames past 0.6 | 1.25, 8 of 12 past 0.5 | 10 (16-bit) |
| `125144765001`, stopped | 0.00, none past | 3.29, 9 of 12 past | 10 |
| `125253331002`, flying | 7.95, 11 of 12 past | 1.60, 9 of 12 past | 10 |

- Face, flying: error under 0.6 native px on every frame from 4, with travel still
  large, and `SkinE` at DXGI format 2 (32-bit) in the dump.
- Helmet, stopped: error under 0.5 native px, also during a head turn. The draw path
  column says engine on those pixels and how many are masked.
- Log: `engine motion: family vs_01DA82F8D0EA0C99: live`, no `takes no second skin` for
  it, and `masked` near 0 in `skin join: second skin this window`.
- Sean: the flick and the helmet frame.

## Journal

### 2026-11-05 flight 12:05:48 (`852102d1`)

Dump `120817532001`. `skin_palette_check.py` passes `recompute`, `prev`,
`prev_alternation`, `runsum`, `list_identity` and `views`, and fails `bases` (434 of
681 record words are not a job's dst); not looked into, and nothing here depends on it.
Dumps `121353903002` and `122537602001` are the lobby-like interior, with a median |E|
of 0.01 and 0.11 cm, which fits.

### 2026-11-05 flight 12:48:51 (`dabfff15`), the speed test

Sean's report and the two dumps as above. Median |E| over `SkinE`: 0.018 cm stopped
(48,900 pixels), 348 cm flying (48,804). The log's |E| line reads a median of at least
0.0316 cm for the stopped window; the flying window was never written because the game
closed first. F7 was not pressed (no `aa compare` line), and the dumps did not need it.
One jump of 0.60 input px at frame 11 of the stopped dump (179 pairs) cannot be rounding
with E this small; a real boundary between meshes is likely, and it was not located.

### 2026-11-05 flight 13:45:08 (`ae4e29b8`), the fix

Sean: "Fixed", by eye. `--expect-build ae4e29b8` matches. No eye dump, so the error
tables were not repeated and the base-0 question is still open on paper: the suit's
records were not read, only the result seen.

- Both eyes' target 7: `R32G32B32A32_FLOAT`, 65.1 MB each.
- `engine motion: family vs_01DA82F8D0EA0C99: live; substituted 5260 binds, 13150
  draws; patched [ps_2C3ADCD2FCD47298]` at 13:50:08, 6616 and 16540 at 13:50:38. No
  stand-down, refusal or `takes no second skin` line for it.
- `binds writing E` goes from about 5,100 a window to 17,156 and 21,212 once the pair
  draws.
- GPU census, eye target 7 clear: 0.005 to 0.008 ms a frame, with one window at 0.016,
  against 0.003 to 0.004 before. EDVR's total shows no separate step from this.
- The compose's `joined` is 0 in every window of this flight, as in other windows
  without an eye dump, so it says nothing here.
