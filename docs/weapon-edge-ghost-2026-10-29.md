# Held weapon's edges double under fast movement, native on-foot VR (2026-10-29)

## Status

- **State: v4 FLOWN 2026-10-30 on Epic. Sean: "objectively worse ... more
  of the gun shimmers now, rather than just the muzzle under ADS" than main.
  Not merged; nothing ships from this branch as it stands. Next: split the
  key so the rim pass and the hidden-test fix can be judged apart, with main's
  behaviour as the third state (flight 4 below).** Issue #146. #147 is a
  separate defect. Branch `claude/weapon-edge-ghost-4a079e`. Testing is on
  Epic from now on (Sean).
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
- **Versions, all flown:**
  - v1 put the sentinel on every rim texel.
  - v2 (cause 1) writes the terrain's vector, or the sentinel, only into rim
    texels that no gun pixel reads.
  - v3 (cause 2) treats a pixel as hidden when last frame's depth was nearer
    on all four texels around its reprojected point.
  - v4 judges that against the pixel's own depth.

  The journal entries below hold each flight.
- **Next flight: the split-key build, BUILDING for Epic.** Brief: ADS and
  hip, fast swings and head turns, in four states. Sean judges each, with one
  eye dump per state:
  - both rows off (main);
  - "Weapon outline (test)" alone;
  - "Weapon uncovered ground (test)" alone;
  - both on.

  The log tells each key's state at every change (`--grep "weapon edges"`).
  The both-off ADS dump is the first look at main's own muzzle shimmer.
- **Temporary keys** (both `on` / `off`, default `on`, live, VR only):
  - `experimental.clean_weapon_edges`, the menu row "Weapon outline (test)":
    the rim pass (cause 1).
  - `experimental.weapon_uncovered_ground`, the row "Weapon uncovered ground
    (test)": the hidden test's covered rule (cause 2).

  Both off is main's behaviour. When the flights settle them, ship the winner
  as the behaviour and propose removing both keys.
- **Found in passing (#147):** bursts of wrong vectors in fast turns. The
  world vectors read zero while the terrain moves 8 to 91 px (dumps
  `194734935001` and `062518187001`). The parked ship gets 54 to 830 px while
  it stands still (`194749437002` and `080754384002`). This is a separate
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
- ruled out: v3 or v4 resetting the gun's own texels through the hidden test, because the gun is fully tracked in all seven dumps from v1 to v4 (0 untracked texels at its depth) and no gun texel is flagged hidden. The only gun texels carrying the sentinel are the rim pass's own.
- ruled out (as evidence): reader metrics from key-on dumps alone, as a judge of what Sean sees, because v2 to v4 each did what the metrics asked and each flight was judged no better or worse. Every flight from now on needs a key-off dump, and Sean's verdict decides.
- ruled out: the one-texel terrain that v3 leaves unhidden being the gun's own texels claimed by the world path, because they carry terrain depth (0 of 3378 within 3 % of the gun) and engine kind `unavailable`, like the terrain beyond them (dump `074347679001`, frame 0).
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

## Build v3, 2026-10-30

- **What changed:** `26767fd3`.
  - The test is in `backgroundHistoryHidden`. It runs only when
    `probe.w` bit 512 is set, it skips `zPred <= 0` as the final rule does,
    and it subtracts `knobs.x` per texel.
  - `temporal_pass.cpp` sets the bit for the mv dispatch only when the pass
    is wanted (key on, on foot) and engine motion is bound. `probe[3]` is
    rebuilt from `uiFlags()` at every mv dispatch, and no other entry reads
    the bit.
  - The log's on line reads `weapon edges (#146): on (v3) -- ...`.
- **The reader:** `eye_decisions.py --rim` gives each pair
  `background_kept_on_old_foreground`. For kept terrain at distances 1 and 2
  it counts how many land on four of four foreground texels of frame k-1. On
  dump `062518187001` it equals the scratch script: pair 6 gives 891 of 3208,
  pair 9 1555 of 4934, pair 14 1491 of 3374.
- **Main merged:** `dab85b3a` brings in `origin/main` `22ead26a`, which
  includes the VR scope zoom. The full build failed once at
  `scope_zoom_test`: the lens pass changed no pixel, "0 changed". The test
  passed three times alone, and the full build rerun passed. Filed as #150.
- **Install:** the version is `v0.22.0-rc.3-113-gdab85b3a`, built from a clean
  tree. Installed on Epic with `--scope both --xess` and verified.

## Flight 3, 2026-10-30 (v3, Epic)

Sean: "still seeing edge shimmer on the weapon". The log is
`edvr_gfx_20261030_074055.log`. Its build matches `dab85b3a`, it shows
`on (v3)`, and the pass ran 436 to 1627 rim passes per 10 s. There are two
dumps, both with the key on:

- `074347679001`: head turns of 26 to 47 px, with the gun nearly still on
  screen.
- `074355067002`: its foreground is something about 9 m away, not the gun.
  Pairs 1 to 4 again carry zero world vectors (#147), and pair 3 a gun vector
  of -960 px.

- **v3 two texels out:** `background_kept_on_old_foreground` at distance 2 in
  dump 1 is 0 to 20 a pair. At v2 the same count was 9 to 44 % of kept.
- **v3 one texel out:** at distance 1 the count is 65 to 955 a pair, 2 to
  46 % of kept. The dark pixels (P - C below -30) among kept terrain one or two
  out are 33 to 75 % of these (frames 4, 6, 11, 14 and 15).
- **Not the gun's texels:** those distance-1 texels carry terrain depth in
  `Z.bin`. None is within 3 % of the gun, and their engine kind is
  `unavailable`.
- **The dilated depth:** D's Z channel is `zPred`, the predicted depth last
  frame that the world path used. In dump 1 it is 0.23 m on the gun, 0.33 to
  0.36 m at distance 1, and 2.19 to 2.35 m at distances 2 and 3. The world
  path takes the nearest depth of the 3x3 for its motion and its `zPred`
  (`temporal_shader_source.h`, `zr = max(zr, zs)` in the mv entry). One texel
  from the gun, that is the gun. So v3's test compares last frame's gun with
  the gun's own depth, and it cannot fire.
- **Vectors beside the gun:** the same dilation reprojects the distance-1
  terrain as a world-fixed point at the gun's distance. In these head turns
  that costs little: the median difference from the distance-3 terrain
  within 2 texels is 0.2 to 0.7 px, and none is above 4 px. A head that
  translates would show more parallax at 0.33 m than at 2.2 m. That is a
  candidate for later, not measured here.

**v4:** v3's covered test compares last frame's depth with the pixel's OWN
depth, `(zraw - knobs.x) * 1.03`, instead of the dilated `zPred`. Over one
frame the pixel's own depth moves far less than the 3 % margin. It uses the
same bit and the same key. Signature: distance-1 `on_old_foreground` near 0
with the key on.

## Flight 4, 2026-10-30 (v4, Epic)

Sean: "The effect of the changes on this branch is objectively worse (more of
the gun shimmers now, rather than just the muzzle under ADS) than what is on
main."

The log is `edvr_gfx_20261030_080441.log`, its build matches `d97ef7a8`, it
shows `on (v4)`, and the key stayed on all flight. There are two dumps, both
at the hip with the key on: `080738606001` (head turns, gun nearly still) and
`080754384002` (under the parked ship).

- **v4 did its job.** In dump 1, `background_kept_on_old_foreground` at
  distance 1 is 1 to 81 a pair, and 459 in pair 15, against 65 to 955 at v3.
- **The gun's interior is not reset.** In every dump from v1 to v4, the gun is
  fully tracked (0 untracked texels at its depth) and no gun texel is flagged
  hidden. The only gun texels with the sentinel are the rim pass's own: 25 and
  620 in frame 0.
- **#147 again:** in dump 2, pairs 5 to 8, the parked ship (the engine path)
  carries -54 to -387 px a frame while the terrain moves 25 to 33 px.
- **Output stability:** this is the median frame-to-frame change of DLSS's
  output along each texel's own vector, in pairs where the gun moves under
  8 px. On the gun's outermost texels it reads v4 5.0 luma (26 % above 12),
  v3 8.4 (40 %) and v2 9.1 (41 %). Inner texels are 1 to 3 in all three.
  The scenes differ and every dump is from the hip with the key on, so this
  says nothing against Sean's ADS view against main.

What the four flights say: every version that writes into the gun's
outermost texels (v1 to v4), or resets the terrain right at its silhouette
(v3, v4), was judged no better or worse in the headset. Meanwhile the reader's
metrics improved each time. The 3x3 nearest-depth model of DLSS predicted
that the rim writes reach no gun pixel. The headset disagrees, so either the
model is wrong for preset K, or the reset silhouette reads as shimmer. The two
mechanisms ship behind one key, so the flights cannot tell them apart.

**Next:** split the key into two temporary keys:
- the rim pass (cause 1);
- the hidden-test fix (cause 2), which touches only background texels.

Both off is main's behaviour. One flight, ADS and hip, toggling each live, and
Sean judges each state, with one eye dump per state. Before that, the current
build already gives one free check: with the key off it is main's behaviour.
If the extra shimmer stays with the key off, the cause is on main since
`3d2944fc`, not this branch.
