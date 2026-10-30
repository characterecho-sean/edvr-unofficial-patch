# Held weapon's edges double under fast movement, native on-foot VR (2026-10-29)

## Status

- **State: v2 (`3b564f21`) FLOWN 2026-10-30 on Frontier. Sean: "the gun
  still seems to lose DLSS at the edges". The dumps found a second cause, in
  EDVR's own hidden test. v3 is PROPOSED, NOT BUILT.** Issue #146. #147 is a
  separate defect. Branch `claude/weapon-edge-ghost-4a079e`.
- **Cause 1, DLSS's dilation:** for each output pixel DLSS takes the vector of
  the nearest-depth texel in its 3x3. The terrain one texel outside the gun
  therefore fetches history along the gun's motion (48 to 84 % of the hidden
  ring in dump `194734935001`), and the gun's dark edge passes the colour
  clamp. EDVR's sentinel works from two texels out but never reaches that ring.
- **Cause 2, EDVR's hidden test (flight 2):** `backgroundHistoryHidden` says
  "hid nothing" whenever a surface at least as near as last frame's occluder
  is still within about two texels of the pixel now
  (`temporal_shader_source.h`, the `nearestDepthNow` exemption). That rule
  holds for a still edge, such as a rock beside terrain. A held gun stays
  beside the pixel while the terrain sweeps behind it. Of the terrain texels
  one or two out that the test kept, 9 to 44 % land on four of four gun
  texels in the previous frame (dumps `062518187001` and `062529570002`, ten
  frames). Those texels are 36 to 86 % of the dark output pixels there: the
  dotted line along the gun's edge.
- **Ruled out:** see "Ruled out" below. The weapon's vectors, its jitter or
  projection, and the background's vectors are right.
- **v1 (`abd7b82c`, flown):** the sentinel on every gun rim texel next to
  differently moving terrain. It fixed the ring, but it reset terrain that was
  visible last frame and the gun's own pixels at the thin muzzle parts (the
  pixelated barrel end).
- **v2 (`3b564f21`, flown, installed):** it rewrites only the rim texels that
  no gun pixel reads, with the terrain's own vector, or the sentinel where the
  terrain was hidden. It takes "hidden" from the test that cause 2 breaks.
- **v3, proposed:** in `backgroundHistoryHidden`, before that exemption: if
  all four texels of last frame's depth around the reprojected point are
  nearer than the pixel's predicted depth by the 3 % margin, the point was
  covered, so it is hidden. A still edge never meets this, because the pixel's
  own texel was background then. It is behind the same key, on foot only.
- **Next flight:** v2 is on Frontier (`v0.22.0-rc.3-57-g3b564f21`, key on).
  v3 needs a build and Sean's go. Brief: fast swings and head turns at the
  hip, with one eye dump with the key on and one with it off.
- **Temporary keys:** `experimental.clean_weapon_edges` (`on` / `off`, default
  `on`), the A/B of this fix. When the flights settle it, ship the winner as
  the behaviour and propose removing the key.
- **Found in passing (#147):** bursts of wrong vectors in fast turns. Dump 1,
  frames 3 and 4: every world vector reads zero while the terrain moved 48
  to 91 px. Dump 2, frames 2, 3 and 12 to 15: the parked ship gets 250 to 830
  px, the world path under-reports, and up to 47 % of the crop is marked
  hidden. Again on Frontier at v2, dump `062518187001`: 8 of 15 pairs carry
  zero world vectors while the terrain moved 8 to 78 px. This is a separate
  investigation.

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
- ruled out: v2's winner test leaving the gun's top edge unfixed (the dotted line of flight 2), because in frame 0's depth it skips 16 of the 100 rim texels on that edge (5 % over the whole gun), and the inner texels there are nearer than the rim (dump `062518187001`).
- ruled out: the gun rim's `own_vector` share as a measure of lost history, because it reads 0.22 to 0.61 in every dump since flight 1, including dump `062518187001`, whose first frame has 0 gun texels off their vector.
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

## Build v2, 2026-10-30

- **What changed:** `3b564f21` changes `weaponEdges` in
  `src\d3d11\temporal_shader_source.h`.
  - The neighbour scan now also records whether a farther neighbour carries
    the sentinel, and the vector of the farthest one.
  - The winner test reads the raw tile depths, which are the values `mv`
    writes to DLSS's depth (`ZC[id.xy] = zraw`). A tie counts as read, because
    DLSS's own tie-break is unknown.
  - The log's on line now reads `weapon edges (#146): on (v2) -- ...`.
- **The reader:** `eye_decisions.py --rim` gives `first_frame_mv` a new count,
  `foreground_rim_rewritten`: rim texels whose `MV.bin` vector is more than
  0.5 px from D's. It also gives a `dilation` block: the 3x3 nearest-depth
  model over the dump's `Z.bin` with `MV.bin` as sent. Without `Z.bin` it is
  `null`.
- **Checks:** the self-test asserts the model on a synthetic fixture. On dump
  `054813060001` (v1) the reader gives ring 3311, hidden 603, hidden sentinel
  603, kept own 0, kept reset 2708, dragged 0, and foreground off vector 1945.
  These equal the scratch model's v1 row.
- **Build:** the full build is green and `--dll-only` gave
  `v0.22.0-rc.3-57-g3b564f21`. Installed on Frontier with `--scope both
  --xess` and verified.

## Flight 2, 2026-10-30 (v2, Frontier)

Sean: "the gun still seems to lose DLSS at the edges." The log is
`edvr_gfx_20261030_062207.log`, its build matches `3b564f21`, and it shows
`on (v2)` with 379 to 1800 rim passes per 10 s. There are two dumps, both with
the key on: `062518187001` and `062529570002`.

- **The pass:** in dump 1's first frame, v2 rewrote 13 of 3340 rim texels;
  little moved relative to the gun in that frame. `dilation` gives 0 gun
  texels off their vector, ring 3293 of 3293 kept on its own vector. In dump
  2: 788 rewritten, 15 gun texels off their vector, ring hidden 3 of 3
  sentinel, kept own 2180 of 2182.
- **The look:** a crop of the gun's top edge in dump 1 is smooth in frames 3,
  5, 7, 8, 10 and 12. Frames 6 and 9 carry a broken dark line one to two
  texels above the edge, at render-resolution steps. Those are the two frames
  in which the terrain's vector is large and correct (-63 and -42 px) while
  the gun barely moves.
- **#147 again:** phase correlation on a terrain window of dump 1 puts the
  terrain's motion at 8 to 78 px in pairs 2 to 5, 7, 8, 10 and 11. In every
  one of those pairs, every world vector reads exactly zero. The edge looks
  clean in those frames, because the gun and the terrain then carry the same
  zero vector, and the pass does nothing.
- **Not the winner test:** frame 0's depth along that top edge has the inner
  texels nearer than the rim (0.0539 against 0.0526). The test skips 16 of the
  100 rim texels there. A depth push behind the inner neighbour changes
  nothing (`062518187001`, `062529570002` and `054813060001` alike).
- **The hidden test:** in the top-edge box (D 990..1070, 725..790), the
  terrain at distance 1 from the gun is 0 % hidden in frames 6 and 9, distance
  2 is 42 and 56 %, and distances 3 and 4 are 100 %. Every dark pixel (P - C
  below -30 luma) is a kept texel at distance 1 or 2. At distance 2, kept
  texels read -31 and -24 luma against the render and hidden ones about 0.
- **Where the kept texels land:** p plus D's vector in the previous frame's
  decisions. Of the kept terrain texels at distance 1 to 2, 9 to 44 % land on
  four of four gun texels, over ten frames of the two dumps. 1 to 3 of four is
  rare (4 to 370). Beyond distance 2, none does, because the test marks them.
  These texels are 36 to 86 % of the dark pixels at distance 1 to 2.
- **Why:** `backgroundHistoryHidden` returns "hid nothing" when
  `nearestDepthNow`, the nearest depth within the directed 4x4 about the pixel
  now, is at least as near as the occluder at the reprojected point. That keeps
  a still silhouette from shimmering, because there the history's filtered
  colour holds the same edge. A held gun breaks it: the gun is beside the
  pixel now, but the terrain has swept 30 to 60 texels behind it, so the
  reprojected point lies inside the gun. v2 then gives the ring the terrain's
  "kept" vector, which fetches the gun.

**v3:** before the exemption, read last frame's depth at the four texels
around the reprojected point. If all four are nearer than the pixel's
predicted depth by the 3 % margin, return hidden. A still edge never meets
this: the pixel's own texel was background in the previous frame, and it lies
within the four. It sits behind `clean_weapon_edges`, through a constant-buffer
bit set when the pass is wanted (on foot, engine motion bound), so the A/B
stays one key. Signature for the flight: a reader count of the kept terrain at
distance 1 to 2 that lands on four of four foreground texels in the previous
frame, which should fall to near 0 with the key on.
