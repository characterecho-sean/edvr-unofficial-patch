# Holo-Me head flick in VR (2026-11-05)

## Status

- **State:** a real defect is proven from one eye dump. Whether it is what Sean
  saw is not yet proven; the flight below decides that. No code changed, nothing
  installed, no ini edited.
- **Report (Sean, 2026-11-05):** "just found my head flicking in Holo-Me, took an
  eye dump".
- **Evidence:** dump `120817532001` (12:08:17), log `edvr_gfx_20261105_120548`,
  Frontier Odyssey install, `v0.22.0-rc.6-210-g852102d1`, profile `vr`, no
  profile switch. DLSS preset K, 2016x2017 in, 4032x4034 out.
- **Cause (proven as a defect):** the second skin stores a skinned character's
  motion (E, from F2) in `R16G16B16A16_FLOAT` centimetres. In this dump the ship
  is flying and the commander travels with the camera, so E holds that travel:
  358 cm a frame at 90 Hz, 650 to 1500 cm during the capture. One half-float step
  of its largest component is then 0.25 to 1 cm, while the head's own motion is
  only about 0.05 to 0.3 cm a frame (from a palette fit). Rounding E puts the
  face's motion vector up to 3.3 native px wrong, differently each frame.
- **Why only the head:** only the skin and hair draws export E. The suit, helmet
  and collar are not in the keyed families and take the head path, which is exact
  here (chest error 0).
- **Scope:** rare. Of 133 log windows with joined pixels in the Frontier and Epic
  logs, six had a median |E| of 100 cm or more (two are this flight), and all but
  one of the six were aboard a ship. 23 reach even 16 cm, where the step is a
  harmless 0.016 cm. Every other dump with a skin map holds under 0.2 cm. A
  character aboard a moving ship should have the same defect; no dump shows one.
- **Ruled out:** see "Ruled out" below (DoF, the flat fix, the switch fix, jitter,
  a flick in the game's pose).
- **Open:** (1) is this what Sean saw; (2) the visor glint, which is real and in
  the game's own image; (3) float32 position precision if the screen stays open
  for minutes in flight.
- **Next flight, no build (below):** Holo-Me with the ship stopped, then flying,
  then F7. A flick that follows ship speed and goes with F7 is this defect.
- **Proposed fix, waiting for Sean's approval:** target 7 becomes
  `R32G32B32A32_FLOAT`, one line in `engine_velocity.cpp`, +65 MB for the two eye
  targets. No key.
- **Temporary keys:** none added. Frontier's `census_offscreen = 1` belongs to the
  lobby DoF work and was left alone.
- **Issues:** none filed yet.
- **Measuring script:** `face_mv_check.py` in the session notes folder, not in the
  repo. It moves to `tools\` with a self-test if the fix goes ahead.

## Environment

Pimax Crystal Super, Pimax OpenXR, through EDVR's native OpenXR runtime, RTX 5090.
The per-eye output is 4032x4034 and DLSS runs in performance mode at 50% per axis.
The log does not read the DLSS runtime's version. E is VR only; flat does not use
this path.

The journal has the ship in normal space over Capoya 2, `Docked=false`, with the
next `Docked` at 19:12:58Z. Its clock runs about 19 s ahead of the log (`LoadGame`
is 19:07:16Z there, 12:06:57 in the log). The camera travels about 3.6 m a frame
at 90 Hz, which is 325 m/s. That this is the ship's own flight is an inference;
the flight below tests it.

## What the dump holds, and what contaminates it

The overview image shows the Commander Creator with the helmet set to always on.
The head is inside the 1400 px crop in both eyes, and the run has no DLSS reset,
with history on in all 16 frames.

- **The capture's hitch.** Camera travel per frame in metres, frames 0 to 15: 3.58,
  3.62, 19.41, 33.20, 7.15, 6.50, 8.87, 7.44, 6.59, 7.00, 6.68, 8.48, 8.15, 8.45,
  15.04, 8.81. Travel follows the time step, so frames 2 and 3 are 5 and 9 normal
  steps, and from frame 4 the capture runs at about half speed. Frames 4 to 15 are
  the ones to read. E, and so the error, is about twice what a 90 Hz frame has.
- **A blink.** The eyelids are closed at frame 0 and open by frame 3. It is in the
  raw input (`C` files), so it is the game's animation.
- **A head turn.** Against the chest the head moves about 16 native px across
  frames 1 to 3 (the hitch), then under 1 px a frame. It does not reverse.

## The measurements

The method for "error": warp the previous raw input (`C`) through the frame's own
motion (`D`) and find the extra offset that best matches this input. On the chest
that offset equals the jitter difference, so it is subtracted from the face's. The
remainder is the face's motion error, with no DLSS response in it.

| Frame | Travel (m) | One E step in MV-x (input px) | Largest jump in MV-x inside the face | Face error (native px) |
|---|---|---|---|---|
| 4 | 7.15 | 0.52 | 0.52 | -0.25, -0.25 |
| 5 | 6.50 | 0.52 | 0.51 | -0.25, +0.75 |
| 6 | 8.87 | 1.04 | 1.02 | -1.00, 0.00 |
| 7 | 7.44 | 0.52 | 0.02 | -1.00, -0.50 |
| 8 | 6.59 | 0.52 | 0.01 | -0.25, 0.00 |
| 9 | 7.00 | 0.52 | 0.53 | -1.75, +1.00 |
| 10 | 6.68 | 0.52 | 0.54 | -2.00, +1.00 |
| 11 | 8.48 | 1.04 | 0.02 | +1.25, 0.00 |
| 12 | 8.15 | 1.04 | 0.04 | +0.75, +1.50 |
| 13 | 8.45 | 1.04 | 1.05 | -0.25, +1.00 |
| 14 | 15.04 | 1.04 | 1.05 | -1.75, +2.75 |
| 15 | 8.81 | 1.04 | 1.01 | -1.50, +0.75 |

**The proof is the fourth column.** A rigid face cannot have its motion step across
a sharp line, and the frame 14 render of `D` shows exactly that, a band edge down
the left of the face. Where a jump exists, it matches one half-float step of E's x
component to within 4%, computed from the camera rows, the face's depth (1.96 m)
and 947 input px per unit of tangent. Frames with no jump have no step crossing
the face; their whole-face error (frames 11, 12) comes from the other components.

Supporting measurements:

- **The stored E.** Frame 0's `SkinE`: 6270 valid pixels over the face, x -114.69
  to -114.50 (4 values), y -283.5 to -283.25 (2 values), z 185.75 to 185.875 (2
  values). The camera rows' travel for that frame is -114.61, -283.55, +185.91.
  The log agrees: `|E| over the joined ones: median >= 562 cm, p99 >= 3.16e+03 cm`
  at 12:08:20.
- **The chest control.** On the DLSS output, warping by the frame's motion gives an
  extra offset of 0 on the chest in 14 of 15 frames and -0.25 px in the other, with
  residual 0.4 to 0.75. The face's residual is 2.7 to 3.4 on good frames and 13.0,
  8.1 and 14.8 on frames 10, 12 and 14.
- **Both eyes, in phase.** Final-image face residual, left/right: frame 8 2.83/2.97,
  frame 9 4.96/5.04, frame 10 5.09/5.14, frame 12 4.85/5.42, frame 14 5.65/5.99.
  Chest 0.35 to 1.5 in both, flat. E is a world-space value, so both eyes get the
  same error.
- **Corroboration, not proof.** Recomputing E from the game's palette ledger (a
  rigid fit to one bone) and predicting each frame's error correlates 0.87 with the
  table, on both axes, over frames 4 to 15, with a mean offset of (-0.12, +0.79)
  native px. The rigid model is not exact: it misses the handed-over motion by about
  1 input px, so it is only used for sign and trend.
- **At 90 Hz.** Frames 0 and 1 have 3.6 m of travel and steps of 0.0625, 0.25 and
  0.125 cm, which bounds the whole-face error at about 0.6 and 1.0 native px. Long
  frames (one or two per 10 s, mostly 23 to 40 ms) raise it two to three times. So
  the expected look is a fine constant wobble plus a jolt at each long frame. This
  is computed, not measured: a dump cannot run at 90 Hz.

## The draws that paint the head

From the census (frames 12789 to 12791, identical draw lists on the eye path) and
the depth writer map of frame 12791.

| Draw (vs/ps) | Indices | Pass | What it is | Motion path |
|---|---|---|---|---|
| `D99AFDC2`/`E86271E4` | 22434 | G-buffer, instance 31 left, 215 right | face skin (owns 5210 px of the head box) | engine, E valid |
| `114AF608`/`A17504A2` | 8145 | G-buffer | hair (1060 px) | engine |
| `01DA82F8`/`2C3ADCD2` | 16122 | G-buffer | helmet, headset, collar (3316 px) | head, no E |
| `01DA82F8`/`2C3ADCD2` | 23430 | G-buffer | suit (1400 px in the box) | head, no E |
| `617C6E44`/`057F8E27` | 22434 | forward into the HDR target, additive | the face mesh a second time | not keyed |
| `025B4B9F`/`C5A5C7E8` | 11904 | forward, alpha blended, no depth write | not identified; it changes probe points off the head, so it is not the visor alone | not keyed |
| `C171BD0C`/`08646D68` | 5244 | forward, additive, no depth write | not identified; a candidate for the visor | not keyed |
| `68345701` and `C506D268`/`428F0731` | 22434 | shadow, five 512x512 and six 2048x2048 views | the face mesh in shadow | none |

The eyes are holes in the engine-path mask; their draw was not named.

## Causes considered

| Cause | What would show it | Verdict |
|---|---|---|
| E rounded to half float | Motion steps inside the rigid face, sized to one step of E | **In: measured.** A defect. Not yet shown to be what Sean saw |
| Visor glint | Bright streak in the raw input moving with the head; stays with F7 off and with the ship stopped | **Open.** Real: on its patch the raw input rises from 66 to 114 by frame 13, then falls to about 100 (the patch tracks the head only roughly). The game's content; its draw is not named |
| Float32 position noise at large camera distance | Whole avatar wobbles, chest included, growing with time open | **Open, unmeasured.** Chest is steady at 2.1 to 2.3 km. At 325 m/s the distance reaches 16 km in under a minute, where a float32 step is about 0.1 cm |
| Light, shadow or light grid | Face patches stepping or alternating in the raw input | **Not supported in these frames:** raw-input forehead 186 to 194 from frame 3 on, a slow drift, no alternation. Only 16 frames |

### Ruled out

- ruled out: depth of field in the Holo-Me scene, because the setting was 2 (the
  next change is `e+0x7C 2->0` at 12:13:07) and FDB74215, 80631A09, E16E27A0 and
  BF2302BC are in none of the three census frames, eye or offscreen. The tone pass
  here is `6F4B7B60`/`3C031C88`, then the copy `DED87960`. The lobby's blur flicker
  is a separate matter.
- ruled out: the flat Holo-Me upscale fix, because this is the VR profile with no
  switch, there are no flat copy lines in the log, and `d933ebab` is not in
  `852102d1`. The game's constant fill `E8FDC0D9`/`5D07ACB4` is in the census but
  nothing of EDVR's reads it here.
- ruled out: the second skin at the switch (`11ef91d5`), because this flight started
  in VR and armed the skin at 12:05:48.606, so that path never ran. E's format dates
  from F2 (`1c23352a`) and main has it unchanged.
- ruled out: jitter as the cause, because the chest takes the same jitter with zero
  error, and the face's error is not locked to the jitter phase (frames 6 and 14
  share a phase and differ, as do 2 and 10).
- ruled out: a flick in the game's pose, because the head bones' rotation per unit
  of time is steady at 0.021 to 0.036 degrees over frames 4 to 15 with no sign
  alternation, and the ledger's previous palette is bit for bit last frame's.
- ruled out: the helmet and collar as the main error, on the evidence available:
  the head-path pixels in the head box show 0.5 native px or less on all but frame
  14 (0.75). Those pixels are mostly neck and shoulders, so the helmet rim alone was
  not separated.

## The next flight: no build, no install

Frontier as it stands (`852102d1`). In normal space:

1. Throttle to zero, wait for the ship to stop. Open Holo-Me, watch the face for
   20 s, press INSERT.
2. Close it, throttle up to about 300 m/s, open Holo-Me again, watch for 20 s, press
   INSERT. Then F7 off for 5 s and F7 on, still watching the face.

| What Sean sees | Reading |
|---|---|
| Flick only when flying, gone with F7 off | This defect. Approve the fix |
| Flick when stopped too, gone with F7 off | The AA path but not E. Read the stopped dump's motion, then the DLSS response on the face |
| Flick stays with F7 off | The game's own image: the visor glint comes first. Not EDVR |

The log and dumps check the same thing without eyes. The `skin join: second skin
this window` line gives |E|: near 0 stopped, 316 cm or more flying.
`face_mv_check.py <install> <stamp>` gives jump counts and error: none when stopped,
as in the table when flying. To test the third open item, hold the second screen for
over a minute before pressing INSERT and read the distance (`now3`, `now7`, `now11`)
in `motion.csv` and the chest's residual.

## The proposed fix

Target 7 at `engine_velocity.cpp:1292` becomes `DXGI_FORMAT_R32G32B32A32_FLOAT`. The
shaders read it as `float4` and do not change. The step at 33 m of travel falls to
about 0.0002 cm. `R32G32B32A32_FLOAT` reports `RENDER_TARGET` and `BLENDABLE` on this
GPU. On a GPU that refuses it, `makeSkinTarget` already logs the failure and that eye
goes on without the second skin.

- **Cost:** the eye targets double, 32.5 to 65 MB each at 2016x2017, so +65 MB, plus
  their clear. F2's on-foot source target doubles too (65.3 to about 131 MB at
  3808x2142) when it is live.
- **Also changes:** the log strings at `engine_velocity.cpp:1302`, `:1306` and
  `:1310`, the `gpu_census.h` comment, and `tools\eye_inputs.py`'s format note. The
  eye capture's handling of the new format has to be read when the fix is built.
- **The alternative:** keep half float and export E without the shared camera travel.
  That costs no memory, but the skin clone needs the previous camera origin as new
  data, and the compose changes with it. A wider patch for 65 MB.
- **Acceptance:** repeat step 2 of the flight on the fix's build. `face_mv_check.py`
  shows no jumps past 0.25 input px and error under 0.6 native px while |E| stays
  large, and Sean says the flick is gone.

No key: a fix that always helps gets none.

## Journal

### 2026-11-05 flight 12:05:48 (`852102d1`)

Diagnosis above, from dump `120817532001` alone. `skin_palette_check.py` on its ledger
passes `recompute`, `prev`, `prev_alternation`, `runsum`, `list_identity` and `views`,
and fails `bases` (434 of 681 record words are not a job's dst); the failure was not
looked into, and nothing above depends on it. Dump `121353903002` (12:13) shows the
lobby-like interior with the commander, not Holo-Me, with a median |E| of 0.01 cm, and
`122537602001` (12:25, the next flight) the same at 0.11 cm, which fits. The only F7 in
the log is at 12:14:02, just after that 12:13 dump, so it says nothing about Holo-Me.
