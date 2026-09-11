# Panel text and celestial approach motion — 2026-09-11

## Captures and provenance

Eight Steam eye runs from `edvr_gfx_20260911_060025.log`: `060232`, `060240`,
`060245`, `060301`, `060347`, `060416`, `060435`, `060502`. The first four show
cockpit panels; the remaining four show the targeted moon, gas giant, rings and
orbital lines during approach. Each contains 16 consecutive raw/treated crops,
a 32-row two-eye motion CSV, first-frame GPU input maps and an object ledger
with its own crop-to-frame map.

The installed graphics DLL is `v0.14.1-173-g188ffbd`, build `6AA340FB`; the VR
DLL is build `6AA34103`. This predates source HEAD `71186fb`. The diff from
`188ffbd` to HEAD was inspected: UI depth and the OpenVR temporal code are
identical; intervening graphics changes concern device recovery/ownership and
logging. These captures are evidence for the unchanged rendering paths below,
not for the later recovery fixes.

Runtime is SteamVR (Valve). The installed DLSS DLL reports file/product version
`310,7,0,0`. The captures use DLSS at **2268×2240 input to 4536×4480 output per
eye**, preset K, with eight-phase jitter. The headset model was not established
from this log. The central raw crops are 1400×1400 at input offset `(434,420)`;
treated crops are 2800×2800. Analysis products are under
`build/review_motion/sep11/` (ignored scratch).

## Findings

The dim rank labels are visible but completely absent from the private UI
coverage in `060232`. In the three text rectangles below, none of the glyph
pixels whose maximum RGB channel exceeds 100/255 are classified as UI; the
result is also zero at thresholds 25, 40 and 70. Coordinates are in the raw
central crop, with exclusive right/bottom edges.

| Text | Rectangle | Sampled bright pixels | Median decoded depth |
|---|---|---:|---:|
| COMBAT RANK | `(801,531)-(885,544)` | 122 | 5.50 m |
| EXPLORER RANK | `(965,531)-(1069,550)` | 102 | 3.58 m |
| CQC RANK | `(977,616)-(1044,631)` | 100 | 2.58 m |

The adjacent brighter rank values have covered cores at approximately 1.0–1.1
m. Thus the dim labels inherit depth from geometry behind the panel, changing
their response to head translation. `ranks-coverage.png` shows raw colour, UI
class and depth side by side. This is a verified input defect. The existing
holo coverage shader clips source t2 alpha below 0.5; the game shader adds glow
and accepts much lower opacity. However, the original source texture is absent
from these dumps, so there is not yet evidence for a safe replacement coverage
rule separating dim lettering, panel scrim and world-marker glow. Globally
lowering the holo threshold would also affect target markers.

The late approach sequence supplies almost rotation-only motion for the
celestial bodies even while their images translate and expand. Offline image
registration fits translation, uniform scale and rotation between each adjacent
pair of raw crops, with modest prefiltering and robust weights. Removing the
recorded jitter difference and comparing against the **corresponding frame's**
captured camera matrix gives:

| Run/region | Median unexplained translation (input px/frame) | Fitted scale change/frame | Camera-predicted scale change/frame |
|---|---|---:|---:|
| `060502` moon | `(-0.083, +0.396)` | −0.14365% | +0.00020% |
| `060502` gas giant | `(-0.383, -0.281)` | −0.10842% | −0.00108% |
| `060435` gas giant | `(-0.061, +0.051)` | −0.02782% | −0.00062% |

These are estimates from image registration, not engine ground truth. Negative
scale here maps a growing current image back into the smaller previous image.
The first-frame reconstruction from the CSV agrees with the actual GPU motion
texture at the sampled region centres within 0.0003 input pixels; subsequent
comparisons use their own CSV rows, not the first GPU texture reused as if it
described the whole sequence. The late moon's vertical residual is positive in
all 15 fitted pairs (approximately 0.33–0.89 input pixels). At the output
scale, the median is about 0.8 pixels per frame, enough to warrant correcting
reprojection before judging the DLSS preset or sharpening.

The ring region in `060435` and orbital-line region in `060416` have no depth
in the captured input. Much of the orbital curve is also unmarked as UI. They
therefore take the rotation-only world path. The orbital line fit has a
consistent vertical residual, but a thin curve does not constrain full 2D
motion well; its fit must not be treated as an exact orbital transform. Ring
texture registration is likewise weaker than the textured moon. These are
separate transparent-geometry coverage and motion questions, not proof that the
panel fix will repair them.

## Ruled out and remaining discrimination

- Ruled out: the 10 m cockpit split for the captured planet/moon blur, because
  their nonzero decoded depth is many orders of magnitude farther away and the
  world path is selected.
- Ruled out: the prior opposing-head/ship-turn world-motion shutdown, because
  `cameraTv3=1`, bound camera rows and `rowsFollow=30` persist throughout all
  eight captures, with 147–220 scene draws.
- Ruled out: an observed origin-jump/reset in these sequences, because
  `jumped=0` and `dlHistory=1` throughout. The native TAA `history=0` column
  does not describe NVIDIA's history state.
- Ruled out: render scale as the source of the measured displacement mismatch.
  All these runs use the same sizes; the raw images already contain motion
  absent from the vectors.

The remaining celestial hypothesis is a separate transform/scale domain:
supercruise approach moves the bodies through their own draw matrices while the
selected ordinary scene-camera translation stays near zero. The dumped
planetary VS families `9FFA5D5E79F04873`, `B12F7A618E1BDE98` and
`203DF51758AADC4D` compute clip position directly from **cb0[4..7]**. Another
sphere family `19F70CE80DA3242B` uses cb0 model rows and cb1 clip rows. These
transforms are not the pool motion. The existing aux ledger only captures 50+
instance draws and cb2, so it cannot confirm this hypothesis for the
one-instance bodies. Do not substitute a guessed physical speed or distance
scale.

## Added diagnostics; no rendering correction yet

The eye-dump key now also writes `pool/drawstate_<stamp>.bin`:

- Every watched draw retains VS b0/b1/b2 and PS b2 before those resources can
  be overwritten, including repeated shader hashes and both eyes. Target
  identity/size and ledger frame/ordinal accompany each draw.
- Each distinct holo t2 source gets one original RGBA image, including alpha.
  It records the first draw that supplied it; it is not a sequence of
  source-texture animation frames.
- Watched vertex bytecode is retained at creation and saved beside the ledger
  on request. This includes the surface/depth candidates and orbital-line
  candidates missing from the earlier shader directory.
- Capture is limited to 2,048 watched draws and 24 textures, at most 16 MiB
  each/64 MiB total. Buffer prefixes are capped at 1 KiB for VS b0 and 8 KiB
  for the other slots; original sizes are retained.
- Readback occurs after the ledger's existing grace period, without a flush or
  blocking map. Failed copies, truncation by the draw cap, missing shader files
  and file-write failures are reported explicitly. No draw snapshots or disk
  writes occur during ordinary play; the fixed watched shader set alone is
  retained at shader creation.

`tools/eye_draw_snapshot.py` validates/reads the format and optionally exports
panel sources with `--surfaces <directory>`; `--dry-run` writes nothing. Its
`read()` exposes constant-buffer bytes for comparing each draw's projected
transform with the measured image motion. The existing ledger crop map is
required: its frame counter differs from the motion CSV's scene counter.

The next useful capture is the same rank panel under head movement and the same
bodies/lines during approach, using this diagnostic build. A motion correction
should reproduce the measured expansion from those draw transforms, while a
coverage correction should retain dim glyphs and reject the source
background/glow. Neither is implemented by this diagnostic change.

## Verification

The new GPU fixture captures repeated draws using the same constant buffer,
then overwrites that buffer immediately after each draw. The Python reader must
recover the three distinct original values. It also verifies first-draw RGBA
source retention, tight rows from a padded staging texture, unchanged
render-target/constant/texture bindings, duplicate-source handling, capture
limits, reset, and retained vertex-bytecode contents. Reader self-tests cover
malformed/truncated files and a dry-run export that creates no files.

The capture call is before the ordinary ledger's pool gate. Completion and
readback are now outside the pool-found branch, so an absent pool cannot leave
a non-pool diagnostic armed indefinitely. Zero captures and copy/write failures
have explicit summaries; an absent summary is not success.

The final full `build.bat` run passed, including the new GPU/reader fixture,
existing UI coverage regression and configuration checks. The production DLL
smoke test also passed. Logs are `build/review_motion/sep11/build-verified.log`
and `smoke.log` in that directory.

The diagnostic DLLs were installed to Steam with `tools/install_edvr.py`, with
the game closed, preserving the INI and DLSS runtime. Previous DLLs are backed
up with suffix `.pre-sep11-eye-diagnostics-20260911-063349.bak`. Build and
installed SHA-256 hashes match:

```text
d3d11.dll     E3BA46CC0EC126630C69ED867E1A405DE9EA732D59C12592012146E5952E68C3
openvr_api.dll EDB1E5073BB641A921B4783738F333944FF398A9D5F552FC48AD61E4C127AC60
```

This is a tested diagnostic installation, not a flight-verified blur fix.

## Follow-up flight: 06:34, captures 063709 / 063821 / 063832

The diagnostic DLL above was flown, confirmed by its installed SHA-256 and
graphics build `6AA3F49E` in `edvr_gfx_20260911_063424.log`. Its pre-commit
`v0.15.1-dirty` label must not be confused with a different binary. All three
draw snapshots completed without truncation, failed copies or missing shader
files. The first shows the rank panel; the other two show moon approach.
SteamVR was the runtime, with a 103-degree headset signature; the log does not
establish the headset model. Each DLSS input is 2268x2240, output 4536x4480,
runtime 310.7.0.0, preset K. The eye-run crops are 1400x1400 at input offset
(434,420), with corresponding 2800x2800 treated crops.

The GUI source confirms the dim-text cause. COMBAT, EXPLORER, TRADE and CQC
labels peak at alpha **124/255**, below the coverage pass's 0.5 cutoff. Bright
ELITE text reaches about 240/255. Blank source texels are zero; a translucent
tile backing is approximately 33/255. Alpha carries dimming as well as
coverage. A single global opacity cutoff cannot distinguish every dim glyph
from translucent backing.

The retained terrain depth VS `ACE405F428C17EF6` and matching colour VS
`72BDD292154158AD` sample a terrain patch and transform it by **VS b2[8]
quaternion and b2[1].xyz translation**. These are patch origins, not planet
centres. The remaining b0/b1 chain reduces to view-space perspective:
b1[270..273] times b0[9..11] agrees with b0[4..7]. Thus their positions are in
DirectX view space, with positive Z forward. Six patches belong to each of the
two terrain bodies in these captures.

For a matched patch, the current-to-previous transform is `R = Qprevious *
transpose(Qcurrent)`, `T = tprevious - R * tcurrent`. Normalising the recorded
quaternions, the first near-body pairs give:

| Capture | Translation in previous view space, metres | Rotation agreement with existing camera |
|---|---|---|
| 063821 | (-170.14, 257.05, 1025.44) | maximum element error 1.31e-7 |
| 063832 | (-2553.68, -1745.95, 3057.65) | maximum element error 2.88e-7 |

The ordinary selected camera translation remains near zero. The missing
component is approach translation, including the disc's expansion. At a moon
pixel in 063821 the decoded depth is about 3.38 million metres; the patch
origin's projected location is on the disc, not its centre. The earlier broad
planet/depth ROIs must not be read as a precise distance measurement of this
moon.

An additional timing trap is visible in scene b1[149]: during the terrain draw
it still carries the prior approach state. Comparing its deltas to
patch-derived translation gives roughly 8% mean relative disagreement without
shifting the sequence. The **next** pair's field delta agrees much more
closely (about 0.2% and 0.016% mean relative disagreement in the two runs).
Later draws in the same frame already contain that next value. Reading that
field directly at a terrain draw would introduce a frame of lag.

Reprojecting the first faster-approach raw image into the next, using its
actual jittered projection and recorded depth, reduces gradient-pixel RMS
intensity error from **8.95 to 5.51** when the patch translation is included.
This is a limited two-frame image check, not a DLSS quality verdict. The
slower pair has only about 0.01 pixel median extra displacement and its RMS
does not improve (5.23 to 5.32); that noisy pair alone cannot measure such a
small correction reliably.

- Ruled out: missing camera rotation in these approach captures, because
  patch-derived rotation agrees with the selected camera to below 3e-7 per
  element.
- Ruled out: using the draw's b1[149] delta directly as current-frame
  translation, because the matching physical change arrives one scene update
  later.
- Ruled out: classifying the rank headings with the existing 0.5 alpha cutoff,
  because every sampled heading alpha is below it.

## Corrections implemented after the follow-up flight

`celestial_motion.cpp` preserves the terrain draw's own transforms on the GPU.
It pairs patches using unchanged sampled bounds/UV parameters and retained VS
texture-view identities, rather than draw order or a reused constant-buffer
address. A changed LOD, missing frame, duplicate predecessor key or invalid
projection declines to ordinary camera motion. Each eye has independent
history, bounded to 512 draws. The original terrain depth draw is reissued
with a constant index shader into private index/depth textures. Both native
TAA and DLSS accept its motion only where that depth agrees with the final
scene and no floating UI covers the pixel. Later foreground geometry, sky,
stations and smoke retain their existing paths.

This adds one small compute dispatch and one depth/coverage draw per
recognised terrain patch, without normal-play CPU readback. At this input size
the two private index/depth pairs occupy about 77.5 MiB, plus bounded record
buffers. Geometry/raster cost needs a flight measurement. The shaders return
before reading terrain textures when no terrain layer is bound.

Cockpit-distance holo surfaces now use the comms panel's minimum source-alpha
coverage rule (1/255), with the existing temporal cockpit range as the domain.
The material's extra eight-tap glow is still excluded because the coverage
shader samples the source directly. Distant targeting markers retain the
original cutoff. This deliberately includes translucent panel backing and
source-baked glow at cockpit distance: a composited pixel has only one motion
vector, so the near panel is favoured there. Full separation of panel colour
from the world would be required to give both layers independent motion. This
is a coverage correction, not a claim that cockpit hull geometry has been
automatically identified.

Orbital VS `C7FA0C0F5DD49180` uses stellar instance
positions/scales/quaternions, subtracts b1[275], and adds a screen-width line
offset. Its per-instance vertex data is outside the new constant snapshots.
Ring/atmosphere families `B12F7A618E1BDE98` and `203DF51758AADC4D`, and the
sphere family `19F70CE80DA3242B`, also use separate paths. **These corrections
do not yet supply their missing motion.** Do not report ring, orbital-line or
gas-giant blur as fixed by the terrain path.

Eye dumps now retain `TerrainIndex` and `TerrainZ` alongside the existing
first-frame inputs and a bounded `eye_<stamp>_Terrain.bin` record snapshot.
`tools/terrain_motion.py` reads the latter; the log reports how many patch
transforms matched. This distinguishes an active coverage draw from usable
history. Readback is only for an explicitly requested eye dump, after its
grace period, with a nonblocking map. No record file or a failed readback is
not evidence of a match.

## Verification of the corrections

The production GPU fixture verifies draw-time preservation when the game
buffer is overwritten, prior-depth and motion-vector sign/units, unchanged
live scene depth, foreground/UI rejection, missing-frame and LOD fallback,
duplicate-key rejection, per-eye isolation, draw limits and restored bindings.
Both complete temporal shader entries compile. It also writes a real terrain
dump which the standard-library reader validates; malformed/truncated reader
inputs are rejected.

The optional replay uses **864 actual adjacent transform pairs**, across both
eyes and both approach captures. All projection/key checks pass and GPU
translation agrees with the double-precision reference within float precision
at the captured celestial distances. The UI fixture exercises 124/255 rank
text, faint backing, zero/sub-quantum alpha and a 16 km marker through the
production holo coverage shader. Existing smoke-hole, sprite, HUD, menu and
adaptive-UI regressions pass as well. The captured replay and UI results are
in `build/review_motion/sep11/test-rendering-final.log`.

The full build passed all gates, including the new terrain GPU/reader fixture
and the configuration contract. The production DLL smoke test also passed.
Logs: `build/review_motion/sep11/build-terrain-verified.log` and
`smoke-terrain.log`.

These are bench-tested rendering corrections; flight confirmation remains
necessary, especially for near-panel translucency and performance while
terrain is visible.
