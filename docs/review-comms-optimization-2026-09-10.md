# Comms coverage, temporal optimization and settings — 2026-09-10

Build `6AA33721` implements the performance review and addresses the comms
icons in `eye_164038`. The previous build's successful station/yaw behavior
is the baseline; camera selection, station transforms, render scale and
sharpening are preserved.

## Comms icons

The raw capture shows the five inactive icons, but the corresponding UI mask
and temporal depth contain only the bright chat icon, notification counter,
borders and message panel. The dim icons therefore use sky motion instead of
the near panel's motion during head movement. The input/depth/mask comparison
is saved in `build/review_motion/comms1640/panel-maps.png`.

The screen-composite coverage shader used the general alpha floor of 0.5.
The actual gamma variant, PS `8ADB2A81A45E8A4B`, samples t0/s0 and preserves
that sample's alpha; it adds no hologram glow. It is now explicitly recognized,
and this screen family's coverage floor is capped at one 8-bit alpha step.
Dim strokes and antialiased edges receive their panel depth, while transparent
pixels retain the scene depth. Sprite/target-marker, hologram and smoke
thresholds are unchanged. All added coverage stays in private depth buffers.

GPU regressions cover dim alpha values, the threshold boundary, transparency,
occlusion and preservation of the original scene depth. This fixes a verified
input defect; appearance during head movement still needs the user's flight
check with the new build.

## Implemented optimizations

- The disabled stepped-motion consumer and its producer share one constant.
  Normal play skips stepped tracking, its unused transform table and rolling
  CPU copies. Object readback retains the existing two samples per eight-frame
  period, including the alternating phase and two-frame separation. Captures
  and explicit diagnostics restore full-rate tracking/readback. The old flight
  measured approximately 0.52 ms/frame inside the now-skipped CPU tracker,
  excluding its subsequent copies.
- The motion shader shares a 10×10 depth neighborhood across each 8×8
  workgroup. Its center output depth remains undilated. Scene, smoke and private
  UI depth merge identically; even out-of-image threads participate in the
  barrier so partial groups are safe.
- Normal DLSS uses a separately compiled shader without registration searches
  or pixel counters. Both variants warm at session start. Eye dumps and
  `advanced.temporal_aa_diagnostics=1` select the diagnostic variant. Normal
  enclosing-pass GPU timings are sampled every 32 scene frames; NVIDIA's own
  timing remains available. Statistics are not read back from uninstrumented
  dispatches.
- Native history/output textures allocate on native TAA or actual fallback,
  independently of input dimensions. Successful full-frame DLSS releases them.
  This avoids approximately 185 MiB at the current 65% input size with the
  preferred history format. Native/DLAA/DLSS switches, identical input sizes,
  resizing and unsupported-format fallback were exercised.
- Object-pair worker scratch arrays/buckets retain capacity, and report-only
  duplicate counting, byte histograms and alternate-pose comparisons run only
  during diagnostics. Essential identity matching, clustering, publication and
  prediction cadence remain intact. These changes do not treat worker elapsed
  time as a render-thread stall.

The integrated source was rebenchmarked on the RTX 5090 at
2862×2826 → 4404×4348:

| Motion kernel | Median GPU ms/eye |
| --- | ---: |
| Previous source | 0.32234 |
| Optimized normal shader | 0.10829 |
| Optimized diagnostic shader | 0.20627 |

The normal shader saves **0.42810 ms across two eyes** in this isolated test.
Motion vectors, depth, bias mask and UI evidence match the previous source
bit-for-bit for the benchmark input. The capture reconstruction limitations
from the [performance review](review-dlss-performance-2026-09-10.md) still
apply: this is not a complete game replay or an end-to-end FPS measurement.
The input and submit color copies remain; their format/color contract is
preserved.

## Settings and UI

TAA, DLAA and DLSS now automatically enable UI depth, smoke depth, adaptive UI
history and object motion. The former companion switches are retired. Depth
motion and jitter already default on; the camera/ship split now defaults to the
tested 10 metres. Automatic texture mip bias is the default with temporal AA
enabled at device creation; explicit advanced overrides remain supported.
Changing AA mode still requires a restart to change that default sampler bias.
Optional depth-disagreement rejection remains off under Experimental.

The Performance page exposes **DLSS preset**, default **K**, with J, L, M and
Automatic alternatives. `advanced.temporal_aa_model` migrates to
`fix.temporal_aa_model`, preserving explicit choices. Legacy named aliases
remain accepted.

The automatic NVIDIA selection displays **DLAA** when saved HMD Quality is
at least 1.0 and **DLSS** below 1.0; the preset row uses the same name. The
stored automatic mode remains `dlss`, so a later quality change cannot leave
it pinned to the wrong path. Explicit legacy `dlaa` remains labelled DLAA.
The graphics setting is cached for one second while the menu needs it. An
unreadable setting gets the neutral label “DLSS / DLAA”. Boundary and invalid
value tests cover this labeling rule.

Supersample filtering is on the Experimental page, absent from Performance,
and its resolve defaults off in both DLLs and the shipped INI.
`fix.supersample_filter` migrates to `experimental.supersample_filter`.
TAA/DLSS does not activate this filter or alter render scale.

## Validation and installation

- Full build, configuration contract/schema, config/defaults, menu, temporal,
  OpenVR and other existing build checks pass.
- Hardware UI coverage tests pass **8,341 checks**, including comparison with
  the captured game HUD pixel shader and the previous sprite-depth regression.
- The production motion kernel matches an independent scalar-neighborhood
  variant across **20 GPU fixtures** and both diagnostic modes: separate depth
  layers, tiny/partial groups, packed-region offsets, mover rejection, first
  and second bodies, ship transforms and absent depth.
- **64 runtime eye evaluations** pass across native/DLAA/DLSS switches,
  same-sized inputs, resizing and unsupported-format native fallback, with
  caller shader-resource/UAV slots restored.
- The full NVIDIA/D3D smoke test passes. All **96 capture submissions** pass;
  saved raw/treated images, dimensions, binary inputs and CSV metadata were
  verified in `build/review_motion/comms1640/capture-checks.txt`.

The installed build and rollback manifest is
`build/review_motion/comms1640/install/plan.json`. It preserves HMD Quality
**0.65**, keeps K, moves the preset/filter settings, removes redundant companion
values, and disables verbose object probing outside captures. It does not
change Frontier graphics settings. DLL and INI source/destination hashes are
verified by the installation script, with rollback if any copy fails.
Installation completed with the game closed; both DLLs and the migrated INI
match the manifest. Frontier graphics settings retain their original hash.
