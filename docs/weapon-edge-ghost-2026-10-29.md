# Held weapon's edges double under fast movement, native on-foot VR (2026-10-29)

## Status

- **State: v1 FLOWN 2026-10-30 on Frontier. The ring no longer follows the
  gun, but the barrel end looks "a little pixelated" (Sean). v2 is designed
  from that flight's dumps and NOT BUILT.** Issue #146. A second defect found
  in the first dumps is #147. Branch `claude/weapon-edge-ghost-4a079e`.
- **Cause:** DLSS gives the background texels beside the weapon the weapon's
  motion vector. For each output pixel it takes the vector of the nearest-depth
  texel around it, so the ring of terrain one texel outside the silhouette
  fetches its history along the gun's motion instead of its own. That terrain
  is dragged along with the gun, and the gun's dark edge passes the colour
  clamp: the smear and the dotted trail. EDVR's inputs are right. Its
  hidden-history sentinel works from two texels out but never reaches the one
  texel that DLSS reads.
- **Numbers** (dump `194734935001`, left eye, 15 pairs, `eye_decisions.py
  --rim`): on the one-texel ring that EDVR marked hidden, the weapon's vector
  explains DLSS's output in 48 to 84 % of discriminating pixels in the 12
  frames with valid background vectors and a ring. The unmoved history
  explains at most 8 %, and the texel's own vector at most 6 %. In the band
  two to three texels out, the current frame explains 75 to 96 %. The weapon's
  own rim keeps its own vector (52 to 89 %).
- **Ruled out:** see "Ruled out" below. The weapon's vectors, its jitter or
  projection, and the background's own vectors are all right.
- **v1, built and flown (`abd7b82c`):** the sentinel goes into the weapon's
  outermost texels wherever the background beside them moves more than half a
  texel differently, or is hidden (the `weaponEdges` entry, right after `mv`).
  "Flight 1, 2026-10-30" below has the result. The hidden ring's gun-vector
  share fell from 48 to 84 % to 0 to 29 %. But v1 also resets terrain that was
  visible last frame (2708 of 3311 ring texels in dump `054813060001`). It
  also resets the gun's own pixels wherever a rim texel is the nearest in its
  own 3x3. There are 261 such texels, densest at the muzzle's thin parts: the
  pixelated barrel end.
- **v2, proposed:** rewrite only the rim texels that no gun pixel reads. Each
  takes the terrain's own vector, or the sentinel where that terrain was
  uncovered. Modelled on dump `054813060001`: the ring gets the right vector in
  3227 of 3311 texels (v1: 603). Gun texels left without a gun vector: 791
  (no fix 749, v1 1945).
- **Next flight:** v1 is still installed on Frontier
  (`v0.22.0-rc.3-53-gabd7b82c`, key on). v2 needs a build, and Sean's go to
  install it. Brief: fast swings at the hip over textured ground, with one
  `hotkey.dump_eyes` key on and one key off in the same flight (flight 1 had
  no key-off dump).
- **Temporary keys:** `experimental.clean_weapon_edges` (`on` / `off`, default
  `on`), the A/B of this fix. When the flights settle it, ship the winner as
  the behaviour and propose removing the key.
- **Found in passing (#147):** bursts of wrong vectors in fast turns. Dump 1,
  frames 3 and 4: every world vector reads zero while the terrain moved 48
  to 91 px. Dump 2, frames 2, 3 and 12 to 15: the parked ship gets 250 to 830
  px, the world path under-reports, and up to 47 % of the crop is marked
  hidden. This is a separate investigation.

## The report

Sean, on the Epic install with the Pimax (Pimax OpenXR), DLSS, 2016x2017 render
per eye to 4032x4034, build `c92b5107` (the scope zoom branch, which touches
neither the weapon nor DLSS): a slight double image along the held weapon's
edges when it moves fast, at the hip and in ADS, and "the muzzle area kind of
shimmers". It shows in the right eye with sight eye `right`. The ADS both-eye
share (F162) was ruled out by that. EDVR sends the headset runtime no depth
and no motion (no `XR_KHR_composition_layer_depth` or space warp in
`src\openxr`), so the runtime's reprojection is ruled out too.

Environment: OpenXR (Pimax), DLSS preset K created with `MVLowRes |
DepthInverted`, performance ratio (2016x2017 to 4032x4034). The vectors are in
render pixels, with the jitter removed. Native on foot is active, so
`screen_motion.cpp` turns off EDVR's flat-source weapon motion (F36). The weapon
takes the engine path, decision path 11, which is the record's own motion.

## The dumps

- `eye_194734935001`: the Manticore Oppressor at the hip, swinging, sight eye
  `right`. 16 consecutive frames (46032 to 46047). The left eye has DLSS inputs
  (decisions `D`, input colour `C`, output `P`, all cropped); both eyes have the
  final centre crops (`FinalCrisp_L/R`). The right eye's trail is at crop pixel
  (330, 820) in frames 12 and 13.
- `eye_194749437002`: the Executioner at the hip, beside a parked ship
  (frames 46887 to 46902).

Read with `python tools\eye_decisions.py <dir>\eye_<stamp>_decisions.json
--rim` (numpy). It uses the run's own files and these candidates. For each
consecutive pair, it takes background texels at Chebyshev distance 1 from the
foreground (the ring, split by EDVR's hidden flag) and at distance 2 to 3 (the
band). It asks which candidate best explains DLSS's output `P_k` there: the
current input `C_k` upsampled, `P_{k-1}` fetched along the nearest foreground
texel's vector, `P_{k-1}` unmoved, or `P_{k-1}` along the texel's own vector.
Only discriminating pixels count: those where the foreground fetch differs
from both the current input and the unmoved history by more than 25 luma
levels. The report also gives each path's `vector_fit`, the compensated error
over the uncompensated error, and the sub-pixel residual left after the
vectors (the jitter step) for the foreground and the background.

## Hypotheses and their signatures

| # | Hypothesis | Signature in the dump | Result |
|---|---|---|---|
| 1 | The weapon's vectors miss its real motion in a swing | Block match of the gun between `C_{k-1}` and `C_k` against its `D` vector | Agrees within 1 px every frame: ruled out |
| 2a | The background's vectors at the outline are wrong in EDVR's input | Background block match against `D`; the hidden flag where the band is uncovered | Right within 1 px (but see #147); the band is flagged hidden: ruled out |
| 2b | DLSS's own dilation hands the background the weapon's vector | Ring output matches `P_{k-1}` along the gun's vector, not the current frame, the unmoved history or its own vector | Holds: 48 to 84 % (hidden ring) |
| 3 | The weapon is drawn with its own projection or jitter (a viewmodel FOV) | The gun's sub-pixel residual after its vectors, against the terrain's | Equal frame by frame: ruled out |
| 4 | DLSS mishandles the uncovered background with correct inputs | Band output against the current frame | It honours the sentinel two texels out (75 to 96 % current); only the ring fails, which is 2b |

## Measurements (dump 1, left eye)

Head turn: the background moves 25 to 53 px a frame (frames 2 and 5 to 15). The
gun moves 0.5 to 4.4 px. The hidden band to the left of the gun is as wide as
the turn (26 px in frame 12).

| Pair | Gun vector | Terrain vector | Ring, hidden: n, gun vector / current | Ring, kept: n, gun vector / current | Band: n, current | Gun rim: own vector |
|---|---|---|---|---|---|---|
| 2 | (-1.4, 1.4) | (43.7, 8.5) | 2700, 0.55 / 0.40 | 748, 0.55 / 0.29 | 4625, 0.96 | 0.77 |
| 6 | (1.2, 1.7) | (36.5, 0.3) | 1269, 0.77 / 0.19 | 481, 0.54 / 0.26 | 1887, 0.88 | 0.63 |
| 8 | (1.6, -1.2) | (37.3, -0.8) | 2024, 0.82 / 0.08 | 1076, 0.52 / 0.20 | 1372, 0.75 | 0.75 |
| 12 | (-3.1, -1.4) | (27.8, -1.0) | 1253, 0.84 / 0.13 | 333, 0.42 / 0.27 | 2124, 0.76 | 0.75 |
| 14 | (0.5, -0.1) | (52.9, 0.3) | 1528, 0.78 / 0.18 | 470, 0.52 / 0.19 | 2269, 0.88 | 0.65 |

All pairs with valid background vectors (all but 3 and 4): on the hidden
ring the gun's vector wins 0.48 to 0.84, the unmoved history at most 0.08 and
the texel's own vector at most 0.06. On the kept ring the gun's vector wins
0.42 to 0.86, the unmoved history at most 0.10 and its own vector at most
0.29. Its own history is valid there, on the side where the terrain slides
under the gun. In the band, the current frame wins 0.75 to 0.96 (frame 1 has
no band). In frames 3 and 4 the background vectors are zero (#147) and the
ring's gun share is 0.32 to 0.53. A scratch census of dark outliers (output
darker than anything in the current input within one texel) found them 3 to
540 times denser one to two texels from the gun than in open terrain, 130
times in frame 12.

Jitter residual, gun against terrain, after the vectors (px): frame 1 +0.69/-0.47
against +0.80/-0.30; frame 9 +0.81/-0.37 against +0.94/-0.51; frame 13
+0.63/-0.41 against +0.70/-0.47. The signs alternate with the jitter
sequence, and the two agree to within 0.1 to 0.2 px every frame with valid
background vectors.

Dump 2's clean frames (1 and 4 to 11) agree but are weaker evidence, because
the gun and the terrain move alike there. On the kept ring the gun's vector
wins 0.40 to 0.56, the current frame 0.03 to 0.19 and its own vector 0.33 to
0.51.

## Ruled out

- ruled out: the weapon's own vectors (engine path) missing its motion in a swing, because block matching agrees with them within 1 px in all 15 pairs of dump 1.
- ruled out: a separate weapon projection or jitter (viewmodel FOV), because the gun's sub-pixel residual after its vectors equals the terrain's within 0.1 to 0.2 px, frame by frame.
- ruled out: wrong background vectors in EDVR's input at the outline, because the terrain's vectors agree with block matching within 1 px and the uncovered band is flagged hidden (frames 3 and 4 aside, which are #147).
- ruled out: DLSS ignoring EDVR's hidden-history sentinel in general, because two to three texels out the output follows the current frame (75 to 96 %).
- ruled out: the ADS both-eye share (F162), because the effect shows with sight eye `right`.
- ruled out: the runtime's reprojection, because EDVR sends it no depth or motion.
- ruled out: nudging the depth handed to DLSS so that the gun's own edge pixels read an inner texel, because in DLSS's depth the inner texels are already nearer than 3255 of the 3269 marked rim texels that have one (dump `054813060001`, frame 0).
- ruled out: a sentinel on every marked rim texel (v1) as the final rule, because it also resets terrain that was visible last frame (2708 of 3311 ring texels) and the gun's own pixels at the thin muzzle parts (flight 1).

## Proposed fix

DLSS's dilation rule is part of what EDVR hands it data for. The sentinel only
works where DLSS will read it, and at a silhouette that is the nearer texel. So:

- In the vector pass, or a small pass after it over the written vectors and
  depth, take a tracked-foreground texel (decision path 11) that has a
  background texel within its 3x3, farther by the hidden test's 3 % margin.
  If that background texel is hidden, or its own vector differs from the
  foreground's by more than half a texel, write the `size*2` sentinel into the
  foreground texel.
- DLSS then dilates "no history" over the ring, which is what EDVR already
  decides for the band beyond it. The foreground's interior texels are nearer
  than its rim on a rounded silhouette, so the foreground's own output pixels
  mostly keep its vector.
- The cost: wherever the rim texel itself is the nearest, the foreground's
  outermost output pixels restart from the current frame during fast relative
  motion. When the motion is slow nothing changes.

Rejected: eroding the foreground in the depth handed to DLSS (it falsifies
depth and depends on DLSS's undocumented footprint), and a reactive or bias
mask (compensation; preset K ignores it anyway).

To test: a temporary on/off key, one flight with fast swings, and an eye dump
with the key on and one with it off. Read both with `--rim`. With the fix, the
ring's `foreground_vector` share should fall to near zero and its `current`
share rise, and the foreground rim's `own_vector` share should hold. Sean
judges the silhouette.

## Build, 2026-10-29

- **Where:** `weaponEdges` in `src\d3d11\temporal_shader_source.h`, compiled as
  `kTemporalWeaponEdgesBytecode` by `tools\temporal_shader_build`. It reads a
  copy of the vectors `mv` wrote (`MVW`, t5; a per-eye R16G16_FLOAT texture,
  `dlMvCopy`) and a depth tile filled as `mv` fills its own. A texel takes the
  sentinel when `enginePixelZ` says kind 1 (decision path 11), it has a
  depth, its own vector is valid, and one of its eight neighbours is farther
  by the hidden test's 3 % margin and either carries the sentinel or a vector
  more than half a texel from the texel's own. The dispatch is in
  `temporal_pass.cpp` straight after the `mv` dispatch, inside the prep's Mv
  part and before b1 and b2 are put back, so `stageEyeInputs` (which stages
  `dlMv` into an eye dump's `MV.bin`) sees what DLSS gets; an `--rim` read of a
  dump with the key on counts the sentinel on the foreground's own rim.
- **Wanted when:** the key is on, the journal says on foot (`g_foot.on`), the
  trained path's `mv` ran and engine-record motion is bound. It runs for every
  trained engine (DLSS, XeSS, FSR) because all three dilate by depth; only
  DLSS was measured.
- **Log:** the key's state at start and on each change, and every 10 s while
  on foot with the key on, `weapon edges (#146) 10 s: rim passes <n>
  (eye-frames), not run <m> [no engine binding <a>, not trained <b>]`. A log
  with the key on, on foot and no such line is a build without the pass.

## Flight 1, 2026-10-30 (v1, Frontier)

Sean, Pimax OpenXR, DLSS, same render sizes: "maybe better? It looks a little
pixelated at the barrel end." The log is `edvr_gfx_20261030_054502.log`, and
its build matches `abd7b82c`. The key was on from the start, off at 05:48:01
and on again at 05:48:04. On foot the pass ran 1122 to 1648 eye-frames per
10 s. There are two dumps, both with the key on, so the gun's rim has no
key-off control from the same flight:

- `054813060001`: a fast swing over the plain. The gun moves -86 to 186 px a
  frame, the terrain 32 px.
- `054824956002`: slow, under the parked ship. Most of the engine path there
  is the ship.

Read with `--rim`:

- The pass wrote what it should. The first frame of dump 1 carries the
  sentinel on 3344 of 3344 rim texels. Dump 2 carries it on 2053 of 41733,
  because the ship's rim mostly moves with the terrain.
- The hidden ring no longer follows the gun. Its `foreground_vector` share is
  0.00 to 0.29 (median 0.09 over dump 1's 15 pairs), down from 48 to 84 %, and
  `current` is 0.47 to 1.00.
- The gun rim's `own_vector` share reads 0.30 to 0.61 in both dumps. Dump 2
  reads the same with only 5 % of its rim marked, so this share does not
  separate the fix from the scene. It is no evidence either way.

A model of DLSS's dilation over its own inputs: frame 0's `Z.bin` and
`MV.bin`, taking the nearest depth in each 3x3, which is the rule the ring and
band measurements imply. The scripts were scratch; the tool does not yet have
this model.

- The gun's inner texels are nearer than 3255 of the 3269 marked rim texels
  that have one, so most of the gun's output pixels never read the rim.
- 261 rim texels are the nearest in their own 3x3, so the gun's own pixels
  there read the sentinel and restart from the current frame each frame: render
  resolution. The densest 100-texel blocks are at the muzzle's thin sight posts
  and prongs: 49 of 153 and 46 of 169 rim texels, about 30 %. Elsewhere it is 2
  to 19 %. That is the pixelated barrel end.
- v1 also resets terrain that EDVR found visible in the previous frame, whose
  own vector is right. Of the 3311 ring texels, 603 were uncovered and 2708
  were visible last frame.

Candidate rules, scored by the same model on frame 0 of dump 1. Ring texels:
3311, of which 603 were uncovered. The muzzle block is the 200x200 texels at
D (800, 700).

| Rule | Uncovered ring gets the sentinel | Seen ring gets its own vector | Ring dragged with the gun | Gun texels left without a gun vector (muzzle block) |
|---|---|---|---|---|
| none | 0 | 0 | 3311 | 749 (75) |
| v1, flown | 603 | 0 (2708 reset) | 0 | 1945 (374) |
| v2 | 586 | 2641 | 64 | 791 (107) |
| v2 with the sentinel only | 586 | 0 (2661 reset) | 64 | 791 (107) |

In dump 2, v2 resets 32 seen ring texels where v1 reset 1194. It leaves 2092
gun texels without a gun vector; v1 left 4397, and no fix 2079.

**v2:**

- **Which texels:** take a rim texel that passes v1's test. Rewrite it only if
  no texel within 3 % of its depth or nearer (itself included) has it as the
  nearest in its own 3x3. Such a texel feeds only the ring.
- **What it carries:** the sentinel if a farther neighbour carries it (the
  terrain was uncovered). Otherwise it carries the farthest neighbour's vector,
  so terrain seen last frame keeps its history.
- **Cost:** the pass already loads a 12x12 depth tile with a two-texel apron,
  which covers the 5x5 the test reads. It needs no new resources.
