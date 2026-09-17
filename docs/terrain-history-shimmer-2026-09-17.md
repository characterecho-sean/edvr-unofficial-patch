# Planetary terrain shimmers under DLSS (2026-09-17)

## Status

- State: cause found offline from one eye dump; fix FLOWN 12:29 on the
  Frontier install (log 122915, build dda993a-dirty = the guard, then
  124139 on e06b549) and CONFIRMED by Sean: "That fixed the shimmering".
  The roll-past-hull check (below) is still unflown. The same flight
  raised the frame-time complaint, which is a separate cause with its own
  doc: docs/terrain-motion-dispatch-cost-2026-09-17.md.
- Report: Sean, Steam install, landed on an airless moon (38 Lyncis 4 F),
  "the terrain is shimmering and not upscaled". Eye dump `eye_114827`
  from `edvr_gfx_20260917_114326.log`, build v0.17.0-rc.3-86-gdda993a
  (= HEAD then), DLSS quality preset K, 2356x2545 in, 3142x3394 out
  (fov trims 5/10/7 live, HMD quality 0.75), Pimax Crystal Super.
- Cause: `backgroundHistoryHidden` (7c6e9ef, the external-camera roll
  stripes) marks NVIDIA's history hidden where last frame's 3x3 depth
  footprint at the reprojected position held a surface more than 3%
  nearer than the pixel's predicted depth. The camera path reprojects
  the 3x3-DILATED depth, so a rock beside the pixel is on both sides of
  that compare and it holds; the terrain path (exact patch transforms)
  hands the pixel's own depth, and the compare fires at every terrain
  depth edge with the camera still. 5.3% of terrain-exact pixels a
  frame (108,273 pixels, 1.81% of the eye), the set walking with the
  jitter; every rock and ridge edge gets a band NVIDIA starts afresh.
- Evidence (all from the dump, no flight): the DLSS output changes
  1.98/255 a frame at those pixels against 0.62 on the rest of the
  terrain, which is as still as the cockpit panel (0.6-0.8); an offline
  replica of the test on Z/PrevZ at the exact previous-raster offset
  (-1,+1) reproduces 108,214 of the 108,273 marked pixels.
- Fix (this branch): the previous footprint's nearest depth is compared
  with the same footprint about the pixel NOW (the 3x3 widened one texel
  per axis toward the rounding of pp, from the mv tile's new two-texel
  apron); a nearer surface still beside the pixel hid nothing. Replica:
  0.0003% of the eye left marked on the still scene (5x5: 0.0002%,
  plain 3x3: 0.35%). Rig cases 20-22 pin it; cases 0-19 (the roll
  stripes) unchanged.
- Instrument: a dump now logs `eye capture: <stamp> history hidden --
  ... at N of M pixels (x% of the eye)`. Stale DLL: no such line.
  Fix inert: ~1.8% on a still terrain scene. Fix working: ~0.00x%.
- Ruled out: the post-DLSS UI resolve / corona-smear hold touching the
  terrain (DlssBeforeUi == final over the terrain, mean diff 0.006);
  fov trims / cull as the cause (the marked set covers the whole
  terrain band, densest at the crater rim mid-frame, not the edges);
  the render-size change 40 s before the dump (2042x2206 -> 2356x2545 =
  x1.15 both axes, the HMD quality slider, not a rebuild; nothing in
  the log names EDVR).
- Open, not this arc: the depth registration Z->PrevZ reads a
  whole-pixel y shift (-1.0) where the recorded jitter delta is -0.556
  (x matches, +0.5), on cockpit and terrain alike. Either the applied y
  jitter differs from the recorded one or PrevZ carries a half-texel
  convention; global, so not the terrain's complaint. See journal.
- Flown: the terrain is still to Sean's eye on the Frontier flight (no eye
  dump was taken, so the `history hidden` census is still unread; take
  one on the next landed view for the record).
- Next flight: a roll on the external camera past a hull tip against sky,
  which is the case 7c6e9ef fixed: no stripes returning.

## Journal

### 2026-09-17 -- the dump, read offline

`eye_114827_*` in the Steam install's `edvr_logs\eyes`: C00..15 raw
input crops (1400x1400 about the input centre), T00..15 DLSS output
crops (1868x1868), L0 whole treated left eye, the EDVRTEX1 inputs (MV,
Z, PrevZ, TerrainIndex, TerrainZ, DlssBeforeUi, ...), motion.csv.

What the pixels say (frames 27944..27959, camera still, MV over the
terrain a slow head drift of (-0.11, +0.15) px):

| region | DLSS output diff/frame (settled, 07->15) | raw input diff/frame |
|---|---|---|
| terrain mid | 2.0-2.8 | 7-13 |
| terrain near | 4.85 (mean over run) | 13.0 |
| cockpit panel (static) | 0.6-0.8 | 3-5 |
| sky | 0.08 | 0.13 |

The raw input over terrain re-samples an aliased detail texture under
the jitter (10-13/255 a frame) -- the game's shading at 75%, nothing
EDVR does -- and DLSS is what should average that. It does on the
cockpit. On the terrain, the settled output still moves 3-4x as much,
and that excess sits exactly on the pixels the MV input marks with the
size*2 sentinel: 1.98 at them against 0.62 elsewhere on the terrain
(pair 07->08).

The sentinel's census by frame cell (10 rows x 6 columns, top to
bottom): zero in the sky, 7-13% across the row holding the crater rim
and the mid-distance terrain, 1-3% on the nearer ground, 0.17% of the
non-terrain non-sky pixels. 5.27% of the pixels whose TerrainIndex is
set and whose TerrainZ equals the scene depth (the terrain path's own
"exact" gate).

Offline replica of `backgroundHistoryHidden` on Z/PrevZ: hidden =
max3x3(PrevZ at p+offset) > Z(p) * 1.03. At the exact offset (-1,+1),
which is round(p + motion - jitterDelta) - p for these frames
(jitterDelta = (+0.5,-0.556), motion (-0.11,+0.15)), it reproduces
108,214 of the 108,273 marked pixels. Most of the marked terrain
pixels sit at real depth steps -- 2.96% of terrain pixels have a
neighbour more than 20% nearer -- so the 3% margin is not the lever;
the footprint is.

Why the camera path does not suffer this: it takes `zr`, the 3x3
nearest of this frame's depth, as the pixel's depth before
reprojecting (the "dilation beside the hull"), so `zPred` already IS
the footprint's nearest and the previous footprint's nearest matches
it with the camera still. The terrain path (line ~930 of the shader,
`pp=terrainP; zPred=terrainZ`) uses the exact per-pixel depth for its
exact reprojection, which is right for the vector and wrong for this
compare. The ship and body paths take the dilated `zBody`; holo and
mesh are `trackedForeground` and skip the test.

The guard chosen: compare the previous footprint's nearest with this
frame's nearest about p over the previous footprint's image in this
raster. The previous 3x3 at q = round(pp) maps to p + (q - pp), a
half-pixel either way, so the current footprint is the 3x3 widened by
one texel toward sign(q - pp) per axis. Replica on the dump: plain 3x3
leaves 0.35% of the eye (all rounding), the directed 4x4 0.0003%, the
wrong-way 4x4 0.30%, the 5x5 0.0002%. The directed 4x4 it is: exact
by construction, and the narrowest band kept beside a hull the
background moves past. That band -- sky within a texel or two of a
still hull while the sky rolls -- keeps its history now where 7c6e9ef
rejected it; its current sample already holds the same filtered edge.
The same holds for the ground newly revealed within a texel of a rock
when flying over terrain (13 px/frame of parallax at 100 m/s and 20 m
put the band well inside NVIDIA's own motion blur).

Ruled out along the way:

- ruled out: the UI resolve or the corona-smear hold (0867042)
  replacing DLSS output over the terrain, because DlssBeforeUi and the
  final T00 differ by 0.006 mean over the terrain patch (0.056% of
  pixels by more than 4, all HUD glyphs).
- ruled out: an edge-of-trim or culling effect, because the marked set
  and the output flicker are densest mid-frame on the crater rim, and
  the trims only cut the eye's outer 23%/16%.
- ruled out: the 11:47:46 feature re-creation as EDVR's doing, because
  the game's render simply grew x1.15 on both axes (2042x2206 ->
  2356x2545) with the same output and no rebuild, which is the HMD
  quality slider.

Open (not this arc): registering Z against PrevZ over five windows,
cockpit and terrain, gives a raster shift of (+0.5, -1.0) px against a
recorded jitter delta of (+0.5, -0.556); raw-colour phase correlation
on the cockpit panel also snaps y to 0/+1 while x is sub-pixel and
matches. Either the applied y jitter is not the recorded one, or PrevZ
sits half a texel off in y. Global, so it is not what Sean saw on the
terrain, and it would blur rather than shimmer. Worth its own check:
a dump with the camera still and `advanced.temporal_aa_diagnostics`
registration on, or a rig that renders a known edge through the
runtime's jittered tangents.
