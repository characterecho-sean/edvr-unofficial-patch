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
