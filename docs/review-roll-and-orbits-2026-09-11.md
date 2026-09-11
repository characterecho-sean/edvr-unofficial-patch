# Roll streaks, rings, and orbital approach

Follow-up to [the panel and planet
investigation](review-planet-panels-2026-09-11.md). The user confirms the moon
remains sharp and the cockpit panels are much improved. Remaining reports are
intermittent CMDR-name streaks during hard rolls, rings/orbital lines during
approach, and soft target circles and distance text during rapid approach.

## Flight and evidence

The Steam run `edvr_gfx_20260911_083433.log` matches `294992c`,
`v0.15.1-4-g294992c`, graphics build `6AA40F76`. The VR log identifies Valve's
SteamVR runtime, with a 103-by-103 degree headset signature; it does not name a
headset model in the inspected runtime summary. The captured path uses DLSS
Performance, preset **L**, 2268x2240 input and 4536x4480 output per eye. It
briefly created DLAA/K at launch before switching. Do not describe these
captures as a preset K test.

Seven runs are present: `083700`, `083702`, `083705`, `083710`, `083722`,
`083905`, and `083921`. The first five show the ranks panel while rolling. The
last two show the selected gas giant, rings and orbital arcs on approach. The
raw 1400-square crop starts at input pixel (434,420); treated crops have twice
the pixel density. Join image frames through the ledger/CSV mapping: the first
input-map frame is one lower than its corresponding draw-snapshot frame. The
exact motion/depth/coverage maps are only for the first captured frame, not all
sixteen image pairs.

**Ruled out: missing cockpit transform history or missing dim-text coverage
explains this remaining name streak.** Every panel run's first frame has 11/11
eligible transforms matched. All sampled hologram coverage agrees with the
merged temporal depth. The untracked twelfth record has its origin behind the
eye and has no visible coverage. There are no capped draws or failed snapshot
copies in these captures.

**Ruled out: the new panel transform still misses the hard-roll geometry.**
Across 75 adjacent raw-image pairs, reconstructed panel motion agrees with
image registration to roughly 0.02-0.03 input pixels RMS horizontally and
0.008-0.024 vertically, depending on the capture. The actual pool index is read
per draw, so this comparison includes pool reordering. Registration is
performed on a lightly filtered name/header ROI; it is evidence about geometry,
not a measure of reconstructed text quality.

In `083710`, the raw image contains a single clean RIKEN label, while several
treated frames contain vertically repeated copies underneath. In the crop ROI
(1015,601)-(1115,674), 162 pixels satisfy a simple ghost selection: treated red
greater than 40 and blue greater than 30, with raw RGB below 20. Most have zero
UI coverage, zero depth, and zero bias-current-colour. Their sky vectors are
about seven input pixels vertically per frame, whereas the panel's vectors are
subpixel. This is history leaking into transparency, not the panel itself
moving by the trail's displacement.

## UI history correction

`adaptiveUiReactive` used to return zero when both current and reprojected
centres were unmarked. That bypassed its own 3x3 reconstruction-footprint
check. A transparent pixel can sample an old glyph beside that centre,
especially under the very different sky vector during a roll. It then preserves
UI history in a pixel which follows the sky on the next frame.

The empty test now covers the previous 3x3 footprint with four overlapping
alpha gathers. Truly empty history exits cheaply; nearby UI reaches the
existing colour/coverage comparison. If both footprints are empty it returns
zero; if only one contains UI it rejects that history. No fixed bias, global
history weight, or sharpening setting changes. Synthetic regressions cover
transparent centres, diagonal departing strokes, unrelated sky, aligned motion,
stable colours and smoke/UI classification. This corrects a demonstrated
rejection bug; whether it eliminates every perceived DLSS trail still requires
a flight. It is not an offline replay of NVIDIA's model.

## Ring and orbital motion

Both families had zero temporal depth in the earlier flight. The new orbital
capture includes the previously missing 16-byte vertex and 60-byte instance
streams. There are 17 active instances in each approach capture, plus spare
records in the buffer. The original orbital VS is `C7FA0C0F5DD49180`; its PS is
`6EEF165A350DA30F`. The missing PS was recovered from the installed
`Win64/EffectsBinary/Effects2_Win64_SM50.arc` by decompressing its raw deflate
payload and matching the project's actual bytecode hash. No extra flight or
game configuration was needed to retrieve it.

The shared draw-motion table now supports three explicitly recognized paths:

- Existing unskinned cockpit pool transforms.
- Ring VS `B12F7A618E1BDE98` / PS `42AC0CACC9CDF72B`, whose b0 rows 4,5,7
  directly map local position to clip X,Y,W. Material and mesh keys separate
  rings; identical repeated passes may share an identical predecessor.
- Orbital instance position, quaternion, scale and width, projected through the
  draw-time scene constants. Shape/width keys and a unique nearby projected
  origin survive instance-slot changes; colour and opacity changes do not imply
  geometric movement.

The orbital geometry has local Z=0. Giving its unused Z basis a scale
comparable to X/Y prevents an ill-conditioned inverse for billion-metre
ellipses. It does not move a vertex. The coverage VS preserves the original
stroke extrusion and adds a non-interpolated record index. GPU stream-output
comparison against the original game VS agrees across 139,935 visible vertices
from the two captures. Fixed screen-width extrusion can differ slightly from
pure physical reprojection between frames; the motion mapping does not claim to
reconstruct new or missing orbital geometry.

Coverage is written through the existing private temporal-depth pass. The
game's original colour and depth are untouched. Ring alpha is transcribed from
the original material, including radial bands, edge fades, distance and view
angle; 54 GPU comparisons against the original PS agree within 0.00000006. Only
coverage at the existing 0.5 opacity floor owns ring depth. Transparent radial
gaps keep their scene motion. The second volumetric ring material
(`EEAAC839A9F09448`) and the gas-giant sphere are not changed by this path.
Thin orbital strokes use their own smooth alpha and retain nonzero visible
coverage as UI.

The temporal consumer uses the captured current-to-previous mapping, removes
raster-jitter difference, and rejects coverage hidden by later foreground
depth. Ring records are world coverage; cockpit/orbital records require UI
coverage. Native TAA and DLSS share the corrected mapping.

Limits remain **128 records per eye**, now shared with cockpit holograms.
Orbital batches support 1-64 instances with StartInstanceLocation=0, the
observed 16/60-byte input layout, and the normal full-eye viewport. Unsupported
or excess stellar draws decline without writing anonymous depth. Missing or
ambiguous predecessors retain the prior fallback. These are rendering
corrections for the observed shader families, not universal celestial motion.

No additional full-resolution texture is allocated beyond the previously
introduced hologram coverage/history. Rings add one coverage reissue and a
small GPU history dispatch per recognized draw. An orbital batch uses one
coverage reissue and one history dispatch for all its instances. Normal play
does not read these records back to the CPU. The UI empty-footprint check adds
alpha gathers; the full colour comparison remains local to UI.

The last approach census also distinguishes repeated ring passes by their
vertex/index buffers: draws 291, 293, 295 and 298 bind different buffer pairs.
Those identities participate in history keys, so nearby passes with the same
material do not become ambiguous merely because their origins are close.

## Target circle and distance text: remaining evidence

The targeting report must not be conflated with the orbital renderer. Target
widgets use flight HUD `B7790CBFC6554097`, and text also uses sprite
`E508648660A352B2`. Their original shaders were recovered from the installed
Effects archive. HUD vertex positions can change inside a dynamic stream; the
existing scene/pool snapshots do not preserve those draw-time vertices.
Consequently the current evidence cannot distinguish a changing vertex shape or
target position from changing digits on a texture surface. No guessed motion or
sharpening compensation is installed for them.

Eye snapshots now include these VS families, their constants and the sprite
surface at t0. Snapshot version 2 preserves start/base draw arguments and each
target draw's VB0, VB1 and index-buffer bindings/payloads for the first three
watched frames. Captures begin at the binding offset, and preserve stride and
whole-buffer size. Copies are capped at 256 KiB per stream and 32 MiB per run.
The log reports copied draws/bytes and budget declines; copy failures retain
the existing explicit failure count. The overall snapshot limit is 4096 draws.
Ordinary play performs no such copies.

The reader accepts versions 1 and 2. Its GPU fixture overwrites one dynamic
buffer between two draws and proves that both earlier payloads survive with
their distinct values, offsets and signed base vertex. Time and byte-budget
limits are tested. A new eye run while rapidly approaching a selected target is
needed for the remaining target-motion investigation.

## Verification

The targeted logs are under `build/review_motion/sep11/`:

- `test-stellar.log`: 72 existing cockpit transform pairs; 502 captured stellar
  draw pairs / 1,334 matched instances (maximum 0.004039 input-pixel error);
  original orbital VS and ring PS comparisons.
- The private UI GPU fixture checks actual orbital reissue, two instance IDs,
  untouched live depth, and restoration of vertex shader and VS/PS constants,
  alongside existing alpha, smoke-hole and temporal UI tests.
- The new stellar fixture is a normal build gate; proprietary captured assets
  are optional local replay inputs and are not committed.
- `build-stellar.log`: the full production build and all regression gates pass,
  including 6,805 private UI checks, the stellar fixture, version-2 snapshot
  capture/parser fixtures and the 243-key configuration contract.
- `smoke-stellar.log`: the production graphics DLL smoke test passes, including
  native TAA, DLSS evaluation and motion/jitter convention checks.

Flight confirmation remains necessary for the UI trail and both new stellar
paths. The target-widget rendering change remains outstanding pending its
draw-time vertex evidence.
