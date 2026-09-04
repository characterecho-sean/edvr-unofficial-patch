# Code review: the temporal pass at distance, and the "darker than stock" report

A read of the temporal anti-aliasing chain on `main` at `61761c8` (the
`claude/temporal-aa` work as merged), asked two questions: what in it can
warp a rigid object several kilometres away, a station being the case in
point; and what in EDVR can make the game read darker than stock, which a
user reported. Nothing was built or flown. The evidence is the source, the
design record in `docs/anti-aliasing.md`, the review of the same morning
(`docs/review-motion-vectors-2026-09-04.md`), the tester handout, the live
`edvr.ini` on the Steam install, and today's flight logs in
`C:\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64\edvr_logs`
(the 13:48, 13:59 and 14:04 sessions, build `v0.13.3-58-g139d2bc`, DLSS
then DLAA on the Pimax).

Companion to the morning's review, not a repeat of it: that one followed the
motion-vector chain for soft text; this one follows the same chain out to the
far plane and then follows the brightness through every pass at the door.

**Built the same evening, on this branch, unflown:** F2, F3, D1 (with the
morning review's F9 family gate) and F4's default, in the code; F1 in the
handout, the live ini and the key's comment. The design record's Feature B
section carries the summary.

---

## The answers in one screen

**Warping at distance.** The reprojection of far content is sound: the
station at 4.9 to 7.2 km reads its true depth in the scene pair, the rows'
delta matches the head's within six hundredths in steady space flight, and
the arithmetic loses nothing at five kilometres. What warps a distant station
on this rig is a *setting*, not the chain: `advanced.temporal_aa_hud_metres`
is set to 2.4 in the live ini and in the tester handout, and its rule takes
every bright pixel with bright neighbours that reads farther than eight
metres for HUD text and reprojects it as if it sat 2.4 m from the eye on the
head's own path. A distant station's lit faces, lights and windows are
exactly that. Their motion vectors then omit the ship's turn and carry a
2.4 m parallax for every millimetre of head sway, while the dark faces beside
them take the world path; on the trained path NVIDIA's depth copy is
rewritten to 2.4 m for the same pixels. That is a heat-haze on lit structure
at long range, worse on a noisier tracker. The key's own comment names this
price; the handout's "station panels shimmer at long range, heat-haze
quality, worse on a Quest 3" is what it looks like. Set it to 0 (live) with a
distant station in view before anything is built. Behind it, in order: the
station's own rotation (in no vector; inherent), a real chooser regression on
supercruise and arrival frames (F2 below), a latent carry of a floating-origin
jump into the translation term (F3), and an accumulating lag from the rest
snap that only the pass's own history has (F4).

**Darker than stock.** No pass at the door shifts a flat area: the pass's own
history blends with weights that sum to one, the resolve decodes and encodes
sRGB once each around a linear-light filter (the log says so verbatim), the
sharpen is off, and every format is read and written through the plain typed
view of the game's own family. Two things do change what reaches the eye.
Every temporal or supersampling filter converges to a coverage-weighted
image, so a star that stock draws at full brightness when it lands on a pixel
is shown at its fraction of a pixel instead; hairlines, glints and white text
lose the same way, and the pass's own history (`temporal_aa = on`) loses
about twice as much again because it averages in gamma space through a
Gaussian. A space scene reads darker and duller for it, though nothing in it
is uniformly dimmer. The one uniform candidate the code cannot exclude is on
the trained path only: the frame NVIDIA writes is a typed `R8G8B8A8_UNORM`
where the game submits `R8G8B8A8_TYPELESS`, and the compositor's view over
the two can differ. One copy removes the variable. The reporter's mode,
headset and whether menus are darker too decide between these; the questions
are listed at the end.

---

## Part A: what can warp a station several kilometres away

### F1 (live setting; CONFIRMED by construction). `temporal_aa_hud_metres = 2.4` reassigns distant lit structure to the head's path at 2.4 m.

**Where.** `src/d3d11/temporal_pass.cpp:224` (the history fetch) and `:327`
(the motion-vector entry for DLAA and DLSS):

```
if (hud.x > 0.0 && split.w == 0.0 && (far || z > hud.y) && brightAround(int2(p)) >= 3) {
    far = false;
    z = hud.x;
    zraw = knobs.z * (knobs.w - hud.x) / (hud.x * (knobs.w - knobs.z));   // :331, the mv entry only
}
```

with `hud.y = 8.0` (`:1975`) and `brightAround` (`:164`) counting the pixels
of the 3x3, the centre included, whose luma `0.25 R + 0.5 G + 0.25 B` on the
gamma-encoded frame exceeds 0.6. The pixel itself need not be bright: a dark
pixel with three bright neighbours qualifies, so the rim around lit structure
comes with it. The pixel then takes the ship path with `z = 2.4`: the head's
rotation with the head's translation term, no ship turn, and on the trained
path NVIDIA is also told its depth is 2.4 m.

**Who runs it.** The shipped default is 0. The Steam install's live
`edvr.ini` has `temporal_aa_hud_metres = 2.4` under `[advanced]`, and the
tester handout ("Temporal AA Field Test", section 04) tells every tester to
set 2.4. So the rig the observation was made on and every tester's rig run it.

**Mechanism, with numbers at the Pimax's 42 px/deg.**

- A bright face of a station at 5 km moves, under a ship yaw of 5°/s at
  90 Hz, by 0.056° = 2.3 px a frame. Its vector says the head's turn only.
  The dark face beside it takes the world path and moves correctly. The
  own pass clips the bright pixels' history; NVIDIA's history is handed
  vectors 2.3 px wrong on those pixels and correct on their neighbours.
- One millimetre of head translation at 2.4 m is 0.024° = 1.0 px. Tracking
  noise of a few tenths of a millimetre a frame moves the bright pixels by
  a few tenths of a pixel in a random direction every frame while their
  neighbours hold: the "underwater" look the second depth flight described
  for text, now on lit panels. A noisier tracker (the Quest 3's inside-out
  tracking) shows more of it, which matches the handout's "worse on a Quest
  3 even at matched density".
- NVIDIA's own vector dilation takes the nearest depth, and 2.4 m beats
  5 km, so the wrong vectors spread one pixel further into the dark
  neighbours on the trained path.

**Also worth knowing.** Elite's standard orange HUD text (about 255, 128, 0
in sRGB) has a luma of 0.50 and does not cross the 0.6 bar; white and pale
highlight text does, and so do sunlit hull, station lights, and any star
drawn as a blob of two or more pixels. The rule as written catches more
distant structure than it catches HUD text.

**Distinguish it from the rest.** `temporal_aa_hud_metres = 0`, live, with a
distant station in view, head still then head swaying, ship still then ship
turning. What stops is F1; what remains is F5 and the sub-pixel geometry the
handout already names. `temporal_aa_debug = depth` paints the pixels the rule
takes in yellow, so the extent on a station can be seen directly.

**If the lever is kept.** What separates HUD text from a station is that the
text sits inside the canopy's angular region and the station does not, and
that the rule should not fire on a pixel whose dilated 3x3 already carries a
depth in the 100 m..10 km band (a lit face of something real). Neither is a
fix to make blind; both are cheap to instrument with the depth view.

### F2 (CONFIRMED in the log). The chooser's latest-write tiebreak takes another block's write on half the frames of supercruise and arrival intervals, and the world path's registration collapses there.

**Where.** `src/d3d11/temporal_pass.cpp:1209`

```
const bool better = bestIdx < 0 || w.seq > bestSeq;
```

Commit `6677fca` changed this from

```
const bool better = bestIdx < 0 || (bound && !bestBound) ||
                    (bound == bestBound && w.seq > bestSeq);
```

on the argument that "the frame's last view matrix is the one the eyes were
drawn with". The function's own header comment still says "the bound
object's preferred, else the latest such write", which is no longer what it
does.

**Evidence.** The registration line's chooser counts and its regression of
the rows' turn on the head's (k = 0 is exact; the world path uses these
rows), per interval today:

| interval | scene | world path | chosen: bound / another | k (x, y, z) | lead |
|---|---|---|---|---|---|
| 13:50:08 | loading to space, 6 origin jumps | 9.4% | 537 / 1184 | -0.754 (-0.92, -0.64, -0.95) | -0.45 frames |
| 13:50:28 | space, ship still | 64.3% | 1761 / 0 | +0.000 | 0.00 |
| 13:50:48 | space | 62.7% | 1743 / 0 | -0.000 | 0.00 |
| 13:51:08 | space, 100 m/s | 65.2% | 1702 / 0 | -0.040 (+0.00, -0.06, +0.01) | 0.00 |
| 14:01:43 | loading to space, 6 jumps | 2.1% | 182 / 1485 | -0.507 (-0.99, -0.06, -0.73) | -0.09 |
| 14:02:03 | space | 61.5% | 1391 / 0 | -0.000 | 0.00 |
| 14:02:23 | space, 525 m/s | 63.1% | 1371 / 0 | -0.060 (-0.16, -0.04, +0.06) | +0.01 |
| 14:02:43 | space | 64.9% | 1546 / 0 | +0.034 (+0.12, +0.09, -0.42) | -0.02 |
| 14:06:40 | loading to space, 6 jumps | 29.0% | 587 / 654 | -0.378 (-0.92, -0.06, -0.53) | -0.43 |

In steady space flight the bound block's write is the latest on every frame
and the rows turn with the head. On every interval that carries floating
origin jumps (the arrival at a star and supercruise) another object's write
wins on 50 to 90% of frames, the rows turn a quarter to a half of the head,
and the lead is negative: a lag, which is what choosing a stale write on
alternate frames looks like. On those frames the far plane, the planets and
anything with a depth beyond the split move under a head turn by a fraction
of the truth, then by more than the truth on the next frame.

**What it is not.** It is not the steady-state cause of a station warping in
normal space: in every normal-space interval today the chooser took the bound
block's write on all frames. It is the sky and the planets wobbling under
head motion in supercruise and for the first seconds after a drop.

**Fix.** Restore the bound preference among continuous writes: the bound
object's latest continuous write first, another object's only when the bound
object has none this frame. The bound object is the scene camera's by
construction (the latch fires at the first draw into the scene pair,
`src/d3d11/depth_probe.cpp`, `depthProbeNoteDraw`), and continuity already
excludes the reflection and shadow cameras; the "staler prediction of the
same head" that motivated latest-wins is a write of the same object, which
the bound-first rule still resolves by sequence.

**Verify.** The registration line already carries the numbers: on a
supercruise interval, "another's" must fall to near zero and k to within a
tenth of zero, with the lead at zero. Docked and in normal space nothing may
change (the bound block's is already the latest there).

### F3 (CONFIRMED, latent). A floating-origin jump is stored as the "last good" translation and carried into the next dropped frame.

**Where.** `src/d3d11/temporal_pass.cpp:1892-1911`:

```
if (diffDeg > 3.0f) {
    ++g_camDropRot;
    if (g_lastGoodValid) {
        memcpy(worldDelta, g_lastGoodC, sizeof(worldDelta));
        memcpy(tvCam, g_lastGoodTv, sizeof(tvCam));      // carries whatever was stored
        ...
    }
} else {
    memcpy(g_lastGoodC, worldDelta, sizeof(g_lastGoodC));
    memcpy(g_lastGoodTv, tvCam, sizeof(g_lastGoodTv));    // :1905, stored BEFORE the jump check
    g_lastGoodValid = true;
}
if (move >= 50.0) {                                        // :1908
    for (int i = 0; i < 3; ++i) tvCam[i] = 0.0f;
    ++g_camDropMove;
}
```

A frame whose translation is a jump (the origin moved; hundreds of metres to
tens of kilometres) has its `tvCam` zeroed for its own use, but the jump has
already been stored as `g_lastGoodTv`. The next frame whose rotation is
dropped as another camera's carries that translation into every pixel with a
depth on the world path. At 5 km a carried 5 km term is a shift of the whole
station by tens of degrees for one frame; on the trained path NVIDIA is
handed those vectors and its history for the station is fetched from
nowhere. Today's arrival intervals had 6 jumps and 10 carries in 1,241
frames, so the coincidence is rare, but it is the shape of a one-frame pulse
on a distant object and nothing else in the chain produces one.

**Fix.** Zero `tvCam` before the store (move the `move >= 50` block above the
`else`), or store `g_lastGoodTv` as zero on a jump frame. One line either
way. Verify by counting, on the registration line, carries whose stored
translation exceeded 50 m (add the counter; it must read zero after).

### F4 (CONFIRMED by analysis; `temporal_aa = on` only). The rest snap accumulates into a lag of up to about 0.7 px on slowly moving distant content, and the lag varies across the image.

**Where.** `src/d3d11/temporal_pass.cpp:270`

```
float2 dpp = pp - p;
if (knobs.x > 0.0) {
    float m = max(abs(dpp.x), abs(dpp.y));
    pp = p + dpp * smoothstep(0.5 * knobs.x, 1.5 * knobs.x, m);
}
```

with `knobs.x = advanced.temporal_aa_snap`, default 0.15 (`:1102`). The
motion-vector entry has no snap, so DLAA and DLSS are not affected.

**Mechanism.** The comment at `:259` says the snap's error "is bounded by its
threshold and never accumulates past it, since each frame re-registers the
history afresh". It re-registers by the *frame's* delta, not by the
accumulated error, so the suppressed motion does accumulate in the history.
With content moving v pixels a frame, v under the 0.075 px hard threshold,
the history's content position h obeys h_n = (1 - b) x_n + b h_(n-1) with
the fetch unshifted, whose steady state lags the truth by b v / (1 - b): nine
times v at the default blend of 0.90, so 0.67 px at v = 0.074, and the same
through the smoothstep band (at v = 0.15 half the motion is suppressed and
the lag is again about 0.67 px). Past 0.225 px a frame nothing is suppressed
and the lag unwinds over about ten frames: a small forward jolt whenever a
slow drift speeds up.

**Why it is a far-distance effect.** The band of 0.05 to 0.2 px a frame is
where distant content lives under slow ship motion: a station at 5 km under
a turn of a fifth of a degree a second, or under the ship's own slow drift.
The cockpit under head motion is always above the band. And the suppression
is per pixel, so under a slow roll the pixels near the roll's centre lag and
the pixels far from it do not, which is a warp of the field rather than a
shift. With the rest lock merged, the snap's original purpose (the resample
at rest under tracking noise) is served by the held pose, and the design
record already expects the key to be moot.

**Fix.** Default `temporal_aa_snap` to 0, or make the decision global rather
than per pixel: hold the fetch only when the head's delta (rotation and
translation) and the rows' delta are all below threshold for the frame, which
is "the scene is at rest" and accumulates nothing. Verify with the own pass's
registration probes on the world class during a slow turn: a steady offset
of the order of half a pixel that follows the motion is this lag; it must go.

### F5 (inherent; the floor). The station's own rotation is in no vector.

A station's rim moves at about 100 m/s; at 5 km that is 1.15°/s, 0.013° a
frame at 90 Hz, half a pixel a frame at 42 px/deg, and zero at the hub. A
history that averages ten frames smears the rim by several pixels and the hub
by nothing, which reads as a slow warp of the whole structure; NVIDIA's
history rejects less of it than the own pass's clip because the motion is
sub-pixel. The design record and the handout both already state this. The
only levers are a shorter memory for the world class (a lower blend or a
tighter clip on world-path pixels, which the own pass could do per class since
it already counts them) or per-object motion, which is unreachable. F1 is the
thing to remove first, because it produces the same look on the same object
and is a setting.

### Cleared for the far field

- **The depth is real and decoded right.** In space at 13:51:16 the scene
  pair's 4x4 nearest-depth map read the cockpit at 0.3 to 1.8 m and two
  blocks at 7156 / 4859 m and 7183 / 4903 m: a station five to seven
  kilometres off, in the 100 m..10 km band, on the world path. The formula
  (`temporalDepthToMetres`) inverts the reversed-Z projection exactly, the
  planes are the scene's (0.025 .. 50000 m), and float32 loses nothing at
  5 km (the denominator is 0.25, the relative error 6e-8). No far-scene
  rescale is in play for objects inside the far plane.
- **The rows' delta in steady space.** k within 0.06 of zero on every
  normal-space interval today, the still-ship translation regression near
  +1 per axis where the ship was still, the z flip verified by the
  earlier flight. The far plane and the station take that delta.
- **One set of rows for both eyes.** The rotation is rigid; the translation
  differs between eyes by the head's turn acting on the eye offset, under
  2e-4 m a frame at 0.3° a frame, which is 0.004 px at 100 m and nothing at
  5 km.
- **The dilation.** Nearest-of-3x3 is the standard velocity dilation; its
  price is a one-pixel fringe of world pixels around cockpit struts that
  takes the strut's path under a ship turn. Known, small, near.
- **The jitter.** Excluded from both frusta (`systemHookEffectiveTangents`),
  read by the game after the boundary (20 reads against 8), pinned on the
  desk for NVIDIA's side. A jitter is a translation of the image, never a
  scale, so it cannot warp.
- **Motion-vector precision.** `R16G16_FLOAT` holds a tenth of a pixel to
  four decimals and twenty pixels to two.
- **The scene-pair selection.** The busiest pair of the frame's size with
  hysteresis at half the busiest; in space the pair holds the cockpit and
  the station together, and the census shows no second eye-sized pair with
  scene draws that a far pass could be hiding in.
- **The 50-draw gate and the rows-follow score.** Both gate the world path
  off in menus and loading screens, where it must be. In today's logs the
  path came back within one to two seconds of a scene appearing
  (13:50:05 back, 13:50:07 census at 2077 draws; 14:01:42 back, 14:01:43
  at 2212). One caveat: the score only moves while the head turns faster
  than 0.1° a frame (about 9°/s), so a player who leaves a menu into space
  and holds still keeps the world path down until they look around, with
  the sky on the head's delta meanwhile. Low, and easy to see in the log.
- **Order at the door.** Temporal first on the wide frame, the guard's crop,
  the resolve (idle at HMD Quality 1.0), the sharpen (off). Nothing after
  the pass resamples by a different frustum.

### Instrument notes

- `temporalRotationAngleDeg` (`src/common/temporal_math.h:180`) is an acos of
  a float32 trace, which cannot resolve below about 0.02°. The residuals of
  0.025° a frame in every bucket of the good intervals are that floor, not
  noise between the pose streams. The skew vector `temporalSmallRotVecDeg`
  already computes is exact for small angles; use its norm for the
  residual.
- The registration probes and the four candidates never run on the trained
  path (they read the pass's own history, which DLAA leaves empty). Every
  DLAA and DLSS flight's only registration evidence is the rows-versus-head
  regression. Probing against last frame's `dlOut` would give the trained
  path the same instrument for a copy per eye.
- The "rows do not follow the head" line fires dozens of times a second in
  menus and loading screens (14:05:28 to 14:06:26 today). Rate-limit it, or
  hold it while the scene gate is already closed.

---

## Part B: what can make the game read darker than stock

### D1 (untested; trained path only). NVIDIA's frame goes to the compositor as a typed `R8G8B8A8_UNORM` where the game submits `R8G8B8A8_TYPELESS`.

**Where.** `src/d3d11/temporal_pass.cpp:2067` creates the output NVIDIA
writes as `DXGI_FORMAT_R8G8B8A8_UNORM`; the own pass keeps the game's format
(`:1666`, `sd.Format` with a UNORM view). The guard's crop after it keeps the
source's format verbatim, so the compositor is handed a typed UNORM texture on
the trained path and a typeless one on every other path. The colour space the
game declares is forwarded unchanged (`src/openvr/compositor_hook.cpp:1298`,
`:1324`), and OpenVR's Auto rule reads gamma for any 8-bit format, so the
compositor's *decision* is the same for both; what may differ is the view it
builds over the texture (an sRGB-typed view over a typeless resource decodes
in hardware; a typed UNORM resource admits only a UNORM view and any decode
is the compositor's own). The morning's review listed this as F9, a latent
format change, with the sRGB question otherwise cleared (H10). It is the only
transformation at the door that could move a *flat* area, and it is only on
`dlaa` and `dlss`.

**Settle it.** Copy `dlOut` into an owned texture created in the game's
format (`sd.Format`, typeless) and hand that out instead; `CopyResource`
between `R8G8B8A8_UNORM` and `R8G8B8A8_TYPELESS` is legal within the family.
About 0.1 ms per eye at 4848x4788. If the darkness the reporter sees is in
menus and panels as well as in space, this is the first suspect; if it goes
with the copy, the format was it. Menus and loading screens are treated too,
so a uniform shift would show there.

### D2 (by construction; both modes, more in `on`). Sub-pixel bright detail converges to its coverage, and the pass's own history dims it further.

- Any jittered temporal supersample, and the supersample resolve before it,
  converges to a coverage-weighted image. A star covering a third of a pixel
  shows at a third of its brightness where stock shows it at full brightness
  on the frames it lands, and not at all on the others. More stars, all
  dimmer. Hull hairlines, specular glints and pale HUD text lose the same
  way. This is what anti-aliasing is; the field's "text is much sharper
  without it" is the same trade seen on text.
- The own history averages in gamma space on purpose (`temporal_pass.cpp:29`,
  the blend at `:491`). For that third-of-a-pixel star the gamma-space
  average is 0.30 of full where a linear-light average would read 0.58: the
  own pass dims thin bright detail about twice as much as the physics.
- The current sample is a Gaussian over the 3x3 (`:431`, sigma 0.47 px) that
  keeps 69% of an isolated pixel's peak before the blend, and the clip at
  `:476` pulls a thin stroke's history toward its 3x3 mean whenever it
  fires (17 to 24% of pixels in the cockpit).
- Together, on `temporal_aa = on`, an isolated bright pixel converges to
  roughly a third to a half of stock. NVIDIA's history dims less, but it too
  is known for thinning stars and particles.

None of this moves a flat area by a single level: the blend's weights sum to
one, the Catmull-Rom's weights sum to one for every C, the Gaussian is
normalised (`cur /= wsum`), and the clip is symmetric about the local mean.
What it does is take the sparkle out of a space scene, which reads as darker.

### D3. Other EDVR features that change brightness, for the reporter's sake

| key | default | what it does to brightness |
|---|---|---|
| `share_exposure` | 1 | near a bright light, one eye's exposure is copied to the other; one eye changes against stock, the other does not; direction depends on which eye sees the light |
| `exposure_damping` | 0 (off) | when on, compresses the game's stop-down while the sun is in view: brighter than stock near the sun, never darker |
| `loading_dim` | screen | dims the loading screen on purpose |
| `black_void` | 1 | the on-foot screen's surround is black instead of grey |

None darkens the game uniformly. The two that a new user could read as "the
game is darker" are the loading dim and the on-foot void, both deliberate
and both documented in the ini.

### What to ask the reporter, and the ladder

1. Which mode: `on`, `dlaa` or `dlss`, and which headset. The log's reload
   line names the mode since `2f040b7`.
2. Whether menus and cockpit panels are darker too, or only space (stars,
   hull lines, lights). Flat areas darker points at D1; only sparkle points
   at D2.
3. `temporal_aa = off` (restart) with the same resolve: if the darkness goes
   with the pass, D1 or D2; if it stays, it is the resolve's own
   supersampling of stars at HMD Quality above 1.0, or the game.
4. On the trained path, the one-copy build of D1, flown docked at a panel
   and in the main menu: any change in a flat area convicts the format.

---

## For the implementer

Ordered by what it buys per line changed. Each carries its verification.

1. **F1, no code.** Take `temporal_aa_hud_metres = 2.4` out of the handout
   and the live ini until the A/B is flown; the key's own comment already
   states the price. Fly a distant station with it at 0 and at 2.4, head
   still, head swaying, ship turning. Keep the depth view's yellow as the
   extent instrument. If the lever is kept for the HUD, gate it by the
   canopy's angular region or by a dilated depth in the 100 m..10 km band,
   and instrument before choosing.
2. **F2, one expression.** `temporal_pass.cpp:1209`: prefer the bound
   object's continuous writes, sequence within them, another object's only
   as the fallback. Update the comment block above it (`:1195-1200`), which
   describes the latest-wins rule, and the function's header comment, which
   describes the bound-first rule; make them agree. Verify on an arrival or
   supercruise interval: "another's" near zero, k within a tenth of zero,
   lead zero; docked and in normal space unchanged.
3. **F3, one move.** `temporal_pass.cpp:1905-1911`: zero the jump before the
   store, or store zero on a jump frame. Add a counter of carries whose
   stored translation exceeded 50 m to the registration line; it must read
   zero.
4. **D1, one copy.** `temporal_pass.cpp:2067` and the result at the end of
   the trained branch: create an owned texture in `sd.Format` beside
   `dlOut`, `CopyResource` into it after the evaluation, return that. Fly
   docked at a panel and in the main menu. If nothing changes by eye, keep
   it anyway: the format the game submits is the format the compositor is
   told, on every path.
5. **F4, one default.** `advanced.temporal_aa_snap` to 0, and correct the
   comment at `temporal_pass.cpp:259` and the design record's sentence
   about the snap never accumulating. If a snap is wanted, make it a
   per-frame decision from the head's and the rows' deltas, not a per-pixel
   one from the fetch offset.
6. **Instruments.** The residual's small-angle formula; the trained path's
   probes against last frame's output; the rate limit on the rows-follow
   line.

Not in scope, and not proposed: per-object motion for a rotating station
(F5), which no reading of the game's buffers provides.
