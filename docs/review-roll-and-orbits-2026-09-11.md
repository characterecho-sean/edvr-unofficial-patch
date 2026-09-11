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

## Follow-up: 09:30 flight regression

The run `edvr_gfx_20260911_093018.log` matches `fc7358e`, graphics build
`6AA41DC9`. Runs `093139`, `093216`, `093217`, `093259`, and `093301` capture
loading text, the ranks panel, and celestial approach. Loading text visibly
doubles in treated crops during rapid yaw. The panel still ghosts RIKEN.

Ruled out: expanding the previous UI footprint solves the name streak. The user
reports the same streak after `fc7358e`; its synthetic isolated-stroke fixture
did not represent text over a marked dark panel background.

Ruled out: stellar coverage was inactive or its table overflowed. The last two
runs contain 33 records each and 30 matched transforms. Visible ring coverage
and most orbital coverage agree with temporal depth. Three distant orbital
records have ambiguous predecessors; this is separate from the nearby ring and
orbital arcs, which match. The loading capture precedes the first stellar
activation and contains no stellar records.

Two UI-history defects need independent regressions. Colour was sampled at
`q+jitter` while coverage was loaded at `q`; history comparison also omitted
the change in raster jitter. In addition, taking the minimum error across any
previous neighbour accepts a stale glyph whenever a black neighbour matches the
current panel's black background. Current panel captures mark the backing as
UI, so testing only an isolated erased stroke misses this case.

CPU attribution remains open: the run mostly produces 85-90 FPS; sampled
draw-hook times outside compilation are about 0.01-0.28 ms. The first orbital
shader compilation costs 9 ms, and explicit dumps allocate/copy diagnostic
resources. Neither these samples nor FPS measure sustained process CPU use.

### DLSS mask support and the independent UI resolve

Ruled out: correcting or strengthening BiasCurrentColorMask fixes preset L UI
trails. Replaying 32 controlled moving-text frames through the installed DLSS
310.7.0.0 with the old and corrected masks produces byte-identical output.
NVIDIA's March 2026 [DLSS programming guide, section
3.15](https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf)
says only preset F supports this input. Earlier mask tests verified EDVR's
buffer, not that the selected model consumed it. The old reports attributing
modern-model improvements to this mask must not be treated as proof.

The full-eye DLAA/DLSS path now resolves marked UI after NVIDIA reconstruction.
It clips excess colour to the four raw texels surrounding each output sample,
with raster jitter applied consistently. Values within that range retain NVIDIA
reconstruction. Current coverage includes floating and attached UI, including
classified target widgets and orbital strokes. Smoke and unmarked world pixels
are excluded. Departing coverage persists only while clipping stale colour, and
expires within 32 frames so new world detail cannot indefinitely retain an old
UI restriction.

This replaces the existing submit copy and reuses the submit texture and two
UI-history textures. It allocates no additional full-eye textures. The obsolete
adaptive-colour calculation and its evidence write are skipped in the DLSS
motion pass when this resolve runs. Native TAA retains the corrected
raster-aligned adaptive helper. The experimental foveated path and unsupported
submit formats retain their existing path. Explicit legacy bias settings remain
available; their documentation now states the preset limitation.

A controlled 512x192 to 1024x384 preset-L replay changed RIKEN to NOVA during
motion. Bright trailing pixels over the next four frames decreased from 1,848
to 844; background trail energy decreased from 1.132 to 0.613 on a 0-255 scale.
Stable-motion mean absolute error changed from 1.862 to 1.890; after settling,
it improved from 1.772 to 1.658. This is measurable ghost rejection, not proof
that rapid head yaw, ship roll or target approach is fixed in the headset. Fine
text still needs that confirmation. A broader 3x3 input bound admitted more
obsolete glyph colour and was rejected in favor of the actual output sample
footprint.

### CPU attribution and cost

The draw-hook timer subtracted every forwarded driver draw, including EDVR
coverage reissues. It now subtracts only the game's original call. Existing
small hook numbers therefore understated EDVR's cost. New `stellar coverage
CPU` log lines separately report ring/orbital call counts, sample counts and
mean preparation/reissue/restoration time. They sample every sixteenth frame
and report every 1,800 frames; they do not wait for the GPU or read back
transforms. Zero samples are printed explicitly, not mistaken for a measured
zero cost.

The final four-texel resolve was benchmarked at the flight's 2268x2240 input /
4536x4480 output, using its captured approach coverage. Local GPU timestamps
were 0.116-0.126 ms per eye, versus 0.042-0.053 ms for CopyResource alone. This
isolated benchmark excludes NVIDIA inference and the adaptive work removed from
the motion pass; it is not a whole-flight performance claim.

Ruled out: packing the orbital history dispatch into one 64-thread group is an
optimization on this GPU. The 16-instance benchmark increased from
0.0052-0.0062 ms to 0.0079-0.0091 ms. The original dispatch was restored.
Sustained supercruise process CPU remains unproven; the corrected attribution
and separate stellar samples are required before blaming the orbital reissue or
declaring the report solved.

### Inner ring and orbital sharpness

The user now distinguishes inner-ring aliasing visible even in local space from
approach blur. The 09:32:59 raw crop already contains a repeating alias pattern
in the bright inner ring. DLSS reduces it but does not eliminate it. This
supports a source sampling/material/geometry issue; it does not identify which
of those causes it. No sharpening or global texture-bias change is installed.
The flight used the automatic -1.00 mip bias for a 0.5 input fraction; a
material-specific filtering investigation remains necessary, including the
separate volumetric ring material not handled by the opaque coverage path.

Ruled out: the tracked ring and orbital VS families do not receive raster
jitter. Across the two approach sequences, projection-derived jitter follows
the requested jitter with slopes 0.99992-1.00007 and residuals below 0.00008
input pixels. Visible opaque-ring transforms and most orbital transforms are
also already matched. Three distant orbital records remain ambiguous. Neither
those three records nor the new UI resolve explains the stationary inner-ring
alias pattern. Orbital-line sharpness remains a visual verification item.

### Target circle, distance and name text

The shared resolve applies to classified target coverage, but it is not a
verified target-motion correction. The newest sprite draws use
StartIndexLocation=1472272 and BaseVertexLocation=585561. Version-2 snapshots
saved only the first 256 KiB of the bound IB/VB1, leaving the actual referenced
geometry outside the payload even though the copy succeeded. This invalidates
any conclusion that those snapshots fully captured target vertex motion.

Snapshot version 3 preserves original binding offsets separately from capture
offsets, copies the IB window at the draw's start and VB1 around its base
vertex, and retains the same per-stream and total byte caps. VB0 continues to
preserve the instance IDs. Out-of-range/budget declines are explicit. A GPU
regression overwrites large shared buffers between two draws and verifies that
each copy contains the correct data above 256 KiB. Older versions remain
readable. Because indices may still reference outside a bounded vertex window,
analysis must check the actual referenced range before using it. No normal-play
readbacks or extra target-motion guesses are introduced.

### Follow-up verification

The focused production shader replay passes the UI resolve and adaptive-history
checks, 72 captured cockpit transform pairs, 502 stellar draw pairs with 1,334
matched instances, 139,935 original orbital VS vertices and 54 original ring PS
opacity comparisons. Tests cover unchanged world/smoke output, submission
alpha, marked dark-panel glyph erasure, disappearing coverage, influence
expiry, preserved subpixel UI values and noninteger output ratios. Full-build,
smoke and installation results are recorded after final validation below.

Final validation: `build-ui-resolve-final.log` passes the full production build
and every build gate, including 14,538 UI checks, the version-3 draw snapshot
GPU/parser fixture and the unchanged 243-key configuration contract.
`smoke-ui-resolve.log` passes the production graphics DLL smoke test, including
actual DLSS evaluation and motion/jitter convention checks. The investigation
remains bounded by the visual and CPU limitations stated above.
