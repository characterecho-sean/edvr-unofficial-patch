# Foveated DLSS in native OpenXR: feasibility and design

## Status

- **State (2026-09-15, 19:52, Stage 0 FLOWN):** one A/B/A flight in the
  FRONTIER install, Pimax Crystal Super on Pimax OpenXR, 2576x2544 per eye
  in, 3964x3913 out, 90 Hz, temporal_aa = dlss model k, log
  edvr_gfx_20260915_195200.log on build v0.17.0-rc.2-5-g749a4e9 (on main;
  read it with `--expect-build 749a4e9`). The whole-frame gate PASSED
  against the A arm before it: fovea 40 deg with the steady periphery at
  0.5 took the native benchmark gpu p50 from 8.84-9.10 ms (A, three
  windows) to 7.95-8.13 ms (B, four windows), 0.76-1.02 ms and 8-11% off,
  above the 0.26 ms drift inside A; p95 9.91-10.37 -> 9.22-9.55, no rise.
  The pass itself went 4.10 -> 2.61 ms per stereo pair (full 3.35 ->
  centre 0.60 + periphery 0.99 + reduce 0.09 + compose 0.25; prep 0.50 ->
  0.68; ui 0.25 -> 0.00) with every drop counter zero after the first
  minute. CPU p50 rose 0.2 ms in every B window (3.61-3.68 -> 3.77-3.90),
  a price, not a stall; the frame is GPU-bound. INCOMPLETE: the trailing A
  ran 25 s, so it has price lines (identical to the leading A) but no
  benchmark window, and Sean has not yet said what the seam looked like.
  The lines are verbatim in the journal (2026-09-15, flown).
- **Settled by the flight:** H1 CONFIRMED (1.5 ms and 36% on the pass,
  0.8-1.0 ms and 8-11% on the frame). ruled out: H2 (reduce + periphery +
  compose eat most of the crop's saving), because at 40 deg / 0.5 they
  cost 1.33 ms per pair against the 2.75 ms the crop saves over full-frame
  NGX, 48%, and the pass still nets 1.5 ms. H3 (the seam pulses) UNJUDGED.
  The angle decides the saving: centre 0.60 ms at 40 deg (8.2% of the
  output), 0.71 at 49, 1.73 at 80, where the frame gain is gone (p50 8.99
  vs 9.05-9.10). Engaging costs one 445 ms frame (two NGX features per eye
  and a 303 ms shader compile), reported separately as the gates ask.
- **UI parity is the first Stage 1 item, now field-confirmed:** the fovea
  branch of temporalInner (the block ending at the compose dispatch) runs
  neither the UI resolve nor the deferred replay; both exist only in the
  full-frame block, so `ui` reads 0.00 under the fovea by construction and
  B's UI was the periphery's half-size DLAA upscaled bicubically plus the
  crop's DLSS. A parity build pays the 0.25 ms back, net about 1.25 ms on
  the pass. Until then B's numbers price a different image-processing
  feature, exactly as "UI and failure behavior" warned.
- **Ruled out, do not re-run:** the eye-tracked crop as "no blur where I
  look", structural under upscaling (performance.md, 2026-09-05); gaze
  stays Stage 3 and optional. The pooled "NVIDIA ms/eye" figure mixes both
  eyes and all three roles; nothing measured off it compares with the
  per-role numbers. H2, above.
- **Not yet built (Stage 1):** the UI resolve / deferred replay and the
  reactive mask on the crop path (above); the crop-policy unit test (cropOf
  is a lambda inside temporalInner and must be extracted first); history
  committed only after a successful evaluation (foveaHaveHistory and
  prHaveHistory are set unconditionally each frame); the crop branch's
  unconditional ensureNative, diagnostic motion shader and per-frame stats
  readback (the 0.2 ms CPU candidate), which Stage 0 timed, not removed.
- **Next flight (after Stage 1, on the parity build, FRONTIER install):**
  the same A/B/A with every arm at least 75 s INCLUDING the trailing A, no
  angle sweep inside an arm, and the picture verdict written down. Change
  arms by editing the live edvr.ini, not the F8 menu: every menu open or
  close restarts the benchmark scope (perf_monitor.cpp, kEvMenu), which cut
  every B window short this time; the 17 s one ran only because the menu
  stayed open. The "DLSS where you look ENGAGED" line is the proof B ran.
  Read with
  `python tools\edvr_log.py --target frontier --expect-build HEAD --grep "temporal aa price"`
  and the same with `--grep "native benchmark"`; a price line counts only
  when its drop counters are near zero.

## Investigation (2026-09-14)

Status: investigation and proposed design, 2026-09-14. No rendering code,
settings, installed DLLs or runtime registrations were changed for this
investigation. EDVR baseline:
[`d160499`](https://github.com/characterecho-sean/edvr-unofficial-patch/commit/d160499f44ded472dc38ac54f152482d94810c7c).
Upstream inspected: CheekyFoveatedDLSS
[`a830c74`](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/commit/a830c74d7ef7c150168328b9fd657caee9e1655d),
committed September 11. Upstream source was read, not installed or executed.

## Recommendation

The concept is feasible and deserves a bounded experiment in EDVR's existing
DLSS implementation. It reduces NVIDIA reconstruction work, which our VRS
experiment did not touch. However, EDVR already contains an experimental
crop-plus-periphery implementation, and its earlier flights found the same
fundamental quality tradeoff. This is a proposal to modernize and qualify that
path, not evidence of a newly solved problem.

Start with fixed placement under native OpenXR, preserving current full-frame
DLSS as the default and quality reference. First bring the experimental path up
to date with the UI and motion pipeline, then measure the complete cost and the
image during movement. Eye tracking is a later stage through standard OpenXR,
conditional on useful gaze from the Windows-selected runtime. Do not promise
full-frame sharpness everywhere, an invisible transition, or a particular FPS
gain.

If the requirement remains “no loss of sharpness wherever the eyes move,”
retain full-frame DLSS. A small crop cannot reconstruct unseen history
immediately after a large look. The experiment is justified only as an
optional, measurable quality/performance trade that the user accepts.

## What the concept changes

| Technique | Work reduced | Work still paid |
| --- | --- | --- |
| Existing VRS foveation | Pixel-shader invocations in selected game draws | Full-resolution targets, substantial memory traffic, compute work and EDVR's DLSS evaluation |
| Lower render resolution plus full-frame DLSS | Game rendering and render-size input preparation | Reconstruction across the complete output |
| Foveated DLSS | Reconstruction outside a central region | Game rendering at the selected input size, input preparation, peripheral reconstruction, final composition and OpenXR submission |

The proposed frame has two reconstructions: DLSS for a central crop, and DLAA
for a smaller image covering the entire view. The smaller image supplies the
visible periphery and remains temporally active beneath the center. A blend
joins their outputs. It is not a second high-resolution render of the scene,
and does not require quad views.

Cheeky implements this through NGX/Streamline interception, with D3D11/D3D12
backends and ReShade/UEVR integrations. Its documented default center occupies
55% by 45% of the image; peripheral DLAA uses 75% of the **input render
dimensions**. Its published FPS claims are upstream reports, not Elite
measurements. [Upstream
overview](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/a830c74d7ef7c150168328b9fd657caee9e1655d/README.md),
[settings](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/a830c74d7ef7c150168328b9fd657caee9e1655d/USAGE.md).

For example, at a 65% linear render scale, that peripheral setting is
approximately 48.75% of output width and height. Its area is about 23.8% of the
output, alongside a 24.75% central rectangle. This is a pixel-work
illustration, not a predicted 51% time saving: the two evaluations have fixed
costs, different modes, overlapping coverage and extra passes. An elliptical
blend does not reduce the rectangular NGX evaluation area.

## What our previous experiments actually established

The [historical performance
investigation](performance.md#feature-2--fixed-foveation-by-variable-rate-shading)
records working VRS on the RTX 5090/Pimax rig, with roughly 0.5 ms of
frame-time benefit and little difference between presets. Some runs were
limited by the 90 Hz cap or submission, so FPS alone hid savings. Those results
do not establish a current native-OpenXR budget, nor isolate every source of
GPU cost. In particular, a cull experiment is not a clean measurement of
vertex, bandwidth or compute cost individually.

The same document's [DLSS crop
investigation](performance.md#feature-6--dlss-where-you-look) already
considered Cheeky on September 5 and implemented the central technique. Its
important results are:

- A small DLAA crop could be much cheaper than a full-frame evaluation on that
  rig. The complete path saved less than the crop-only timing suggested because
  the periphery and preparation still cost time.
- Combining the crop with EDVR's own temporal periphery produced shimmering,
  smearing and a visible change during head movement. Switching to
  reduced-resolution DLAA improved continuity but did not guarantee an
  invisible seam.
- A noninteger reduction originally shifted samples; an area-weighted reduction
  corrected that. Crop/output rounding also needed care to avoid a stereo depth
  step.
- Fast motion brought content into the crop without the history available to
  full-frame reconstruction. The experiments recorded slower convergence and a
  visible change in texture. The subsequent design review rejected eye tracking
  as a cure for history missing after a large gaze jump; an integrated
  eye-tracked DLSS flight was not established by that work.

These are recorded historical measurements, observations and design
conclusions, not new tests. The older document also contains superseded design
claims, including an optimistic explanation involving saccadic suppression
followed by a later rejection of that proposal. Neither suppression nor a
history fade is an acceptance criterion here.

A later [September 10 DLSS
review](review-dlss-performance-2026-09-10.md#measured-baseline) reported 1.82
ms per eye inside NGX and 2.22 ms per eye for the enclosing pass, averaged
across two render sizes. That supports investigating reconstruction cost, but
is not a same-scene benchmark for today's build. Preparation has since been
optimized; recapture the baseline rather than combining numbers from different
flights.

## What has changed upstream

The September 5 assessment that Cheeky could not handle OpenVR-only games is
obsolete. The inspected version documents OpenVR as well as OpenXR stereo
calibration, including separate, packed and array-slice paths. Its shared
OpenXR layer supplies mapping and alignment even without eye tracking. EDVR's
new native OpenXR route is also materially different from the old integration
context. Standalone Cheeky compatibility with EDVR's NGX calls remains
untested. [Calibration
contract](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/a830c74d7ef7c150168328b9fd657caee9e1655d/EYE-CALIBRATION.md).

Useful implementation ideas in the inspected source:

| Upstream behavior | Application to EDVR |
| --- | --- |
| Crop extents stay constant while placement moves | Avoid recreating NGX features because edge rounding changed the dimensions by one pixel |
| Private motion vectors compensate for a moved crop | Preserve overlapping history without modifying the vectors used by the periphery |
| Explicit reset reasons and stale-gaze policy | Distinguish a normal crop translation from a discontinuity or lost tracker |
| Shared-forward alignment accounts for eye cant and asymmetric FOV | Use the geometry EDVR already owns, rather than manual stereo offsets |
| Independent peripheral preset and scale | Benchmark peripheral quality and cost independently; do not assume an old preset is supported or fastest |

Sources: [crop
geometry](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/a830c74d7ef7c150168328b9fd657caee9e1655d/src/foveation.cpp),
[crop
motion](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/a830c74d7ef7c150168328b9fd657caee9e1655d/src/crop_motion.hpp),
[gaze
policy](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/a830c74d7ef7c150168328b9fd657caee9e1655d/src/gaze_policy.cpp),
[D3D11
periphery](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/a830c74d7ef7c150168328b9fd657caee9e1655d/src/d3d11_peripheral_dlaa.cpp).

Upstream also offers center output supersampling and experimental DLSS-NR.
Neither belongs in this experiment. Increasing reconstruction output size does
not make Elite render additional scene samples, and NR/D3D12 transport adds a
separate feature and synchronization problem. Its automated tests explicitly do
not evaluate NVIDIA DLSS, so their success cannot resolve our temporal-quality
question. [Development and validation
scope](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/a830c74d7ef7c150168328b9fd657caee9e1655d/DEVELOPMENT.md).

## Existing EDVR implementation and gaps

| Area | Current code | Required work before a meaningful flight |
| --- | --- | --- |
| NGX evaluation | [dlaa.cpp](../src/d3d11/dlaa.cpp): `evaluateCrop`, `dlssEvaluateFovea`, `dlaaEvaluatePeriphery`; separate full, crop and peripheral histories per eye | Reuse these interfaces, audit complete input contracts and distinguish timing by region |
| Crop/reduction/composition | [temporal_pass.cpp](../src/d3d11/temporal_pass.cpp): `foveaWanted`, `cropOf`, reduction and composition shaders | Separate geometry policy from rendering; preserve exact registration and stable extents |
| Native producer path | [native_temporal.cpp](../src/d3d11/native_temporal.cpp) calls the same `edvrTemporalAa` path | Fixed cropping is reachable through shared configuration; this is not yet a qualified native feature |
| Motion and changing UI | Full-frame DLSS passes `reactive` to NGX; crop/periphery function signatures do not | Carry compatible masks and preserve their dimensions, bases and meaning on both reconstructions |
| Post-DLSS UI treatment | The current `uiResolve` block is inside the full-frame branch | Apply equivalent current-raster bounds and retained-UI cleanup to the final foveated output |
| Periphery input preparation | Existing reduction uses area-weighted color, selected depth and associated scaled motion | Keep tested sample registration; extend it to required masks and content-change evidence |
| Normal-play cost | Crop branch selects `motionShader(ctx, true)` and enters `ensureNative` before skipping the own pass | Remove unnecessary diagnostic work and unused native-TAA allocations after correctness is established |
| Center geometry | Existing crop uses tangents and optional eye translation/fixation distance | Use full per-eye rotation as well, and retain the exact unjittered projection convention |
| Gaze | Native host has no gaze action acquisition; [temporal frame ABI](../src/common/native_temporal.h) has no gaze sample | Add optional native gaze ownership and a versioned snapshot only in the later stage |

The crop implementation uses a fixed center. The old OpenVR gaze producer for
VRS is not evidence that DLSS crops follow gaze, and cannot supply the new
native runtime automatically.

The fixed path's current configuration is already read by the shared temporal
pass: `advanced.temporal_aa_fovea` defaults to 0; shape, edge, fixation
distance, periphery type and periphery scale are live settings. Existing
periphery scale means a fraction of **output**, capped at input dimensions.
Setting it to 0.75 does not reproduce Cheeky's 0.75-of-input setting. Preserve
that meaning; use explicit dimension conversions in benchmarks and document any
future new setting separately.

## Integration choice

Implement inside EDVR's producer-side temporal pass. EDVR creates the NGX
inputs and knows each submitted eye, its bounds, frame sequence, render size,
projection and reference generation. An external add-on would have to
rediscover those contracts while intercepting our evaluations. The native host
can provide alignment directly without installing a global OpenXR layer or
using marker readbacks.

Cheeky is GPL-3.0; EDVR's project license is MIT. This proposal imports no
Cheeky source, shaders or binaries. Reuse EDVR's existing implementation and
documented SDK interfaces, with independently implemented geometry and tests.
Any future direct code adoption needs a separate licensing decision before it
is included. [Cheeky
license](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/a830c74d7ef7c150168328b9fd657caee9e1655d/LICENSE),
[EDVR license](../LICENSE).

DLSS remains an NVIDIA RTX feature. Headset/runtime selection remains portable:
Windows chooses OpenXR; fixed placement requires no tracker, and gaze uses a
standard extension rather than a vendor SDK. This work does not reintroduce
SteamVR or LibOVR as alternate backends.

## Proposed frame pipeline

```mermaid
flowchart TD
    A[Native frame: eye identities, geometry and sequence] --> B[Game renders at selected input resolution]
    B --> C[Shared color, depth, motion and UI input preparation]
    C --> D[Central crop DLSS]
    C --> E[Reduce peripheral inputs]
    E --> F[Full-view DLAA at peripheral resolution]
    D --> G[Blend into full output]
    F --> G
    G --> H[Current UI bounds and retained-UI cleanup]
    H --> I[Existing sharpening and EDVR menu]
    I --> J[Existing capture, transfer and OpenXR submission]
```

The drawing expresses data dependencies; the two reconstructions execute
serially on the existing game-device immediate context. It does not propose
parallel GPU queues. The XR owner publishes CPU metadata; GPU work stays on the
producer through the current native bridge. Preserve context state and all
existing shutdown ownership rules.

Prepare inputs once. Pass the central region through a per-eye NGX feature
created for its input/output dimensions with output subrectangles enabled.
Process the full view at peripheral resolution, including the portion beneath
the center, to maintain peripheral history across crop movement. Composite into
an owned output in the current submission format and color-space convention.
Keep EDVR's menu/monitor after reconstruction; Elite's own UI is already in the
eye image and needs the shared UI protections above. NVIDIA exposes the
subrectangle creation option in its [NGX
parameters](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_params.h);
use the shipped SDK's full evaluation contract.

Keep the baseline at exactly the same game input and submitted output
dimensions. Otherwise resolution changes can be mistaken for savings from
foveation. Do not add a new cross-device transfer: the final image follows the
existing native transfer/composition path.

### Geometry and stereo

Use the eye identity and generation supplied by the native bridge. Never infer
left/right from call parity or pointer reuse. Derive both crops from one
frame's unjittered head/eye geometry; handle asymmetric FOV and eye cant using
full eye-to-head rotations. Fixed placement projects the same head-forward
direction into each eye. It is independent of seated yaw and recentering,
although a reference discontinuity still invalidates temporal history.

Compute an angular visible region and an optional surrounding history margin,
then convert to render pixels. Derive the output rectangle through one
consistent mapping from that input region. Account explicitly for noninteger
scale, texel centers, submitted bounds and rounding. A mask may be round while
the enclosing evaluation rectangle remains rectangular. Move the origin within
bounds without changing feature extents; if valid coverage cannot be
maintained, choose the complete reference path for the pair rather than
silently clipping the quality region.

A history margin is input to the quality/cost experiment, not a cure. It can
hide some newly entering content, but a sufficiently fast turn or gaze jump
exceeds any affordable margin. Do not resize the NGX feature every frame in
response to head speed.

### History and motion

Retain independent histories for each eye and each reconstruction. Key
resources by device/session generation, actual dimensions, format, preset and
geometry policy. Commit previous crop origins only after a successful
evaluation; rejected or withheld frames do not advance history.

For motion expressed as current-to-previous displacement in input pixels, the
crop-local relation is:

```text
previous_local = current_local + scene_motion
                 + current_crop_origin - previous_crop_origin
```

Apply the origin correction only to private central vectors. The periphery
retains scene motion, scaled to its grid, and jitter scales with the same
coordinate conversion. Do not apply crop motion to the shared scene vectors,
add the raster jitter twice, or average unrelated object velocities across a
depth boundary. Verify sign and units against EDVR's moving-crop probe and the
actual NGX flags, including odd input/output ratios.

Reset on first use, feature recreation, a real frame/reference discontinuity or
a crop jump whose overlap is insufficient. Ordinary translation with valid
overlap must not reset every frame. Record reset reasons and crop age. Resizing
and preset changes reset both affected eyes at a frame boundary. Feature
recreation can hitch even though it needs no game restart.

A previously inactive full-frame NGX feature has stale history. Returning to it
requires a reset and a measured convergence period. Running it every frame
solely to keep history warm would erase most of the intended saving; do not
hide that cost in a fallback strategy.

### UI and failure behavior

The modern full-frame UI contract is a prerequisite. Updating timers alone
around the old crop code would benchmark a different image-processing feature.
Reuse shared preparation and UI resolve logic rather than maintaining two
progressively different copies. Audit which NGX masks each shipped preset
actually consumes, and use explicit postprocessing where current full-frame
behavior already requires it.

Foveation must not output partially composed eyes, stale peripheral images or
an uninitialized crop. Validate both eyes and select settings before processing
the pair. Latch feature failures to prevent per-frame allocation/retry loops.
Retain originals and make pair-level recovery explicit: if the second eye fails
after the first succeeds, recompute or select a consistent supported treatment
for both, preserving the native jitter/FOV contract. Exercise this in the host
harness before flying it.

Recovery stays inside the native OpenXR path. A DLSS optimization failure does
not select another VR runtime. If reconstruction itself becomes unavailable,
preserve the existing native pass-through/stand-down behavior and its truthful
status, rather than introducing another VR backend.

### Optional gaze stage

Add `XR_EXT_eye_gaze_interaction` capability negotiation to the native host.
Build an action set, pose action and action space, attach once per session, and
synchronize/locate on the XR owner. Publish a generation- and sequence-tagged
immutable sample for the producer. Extend the private temporal ABI with a new
version or separately negotiated capability; do not enlarge a version-1 struct
in place.

The standard exposes one gaze pose, not independent per-eye vergence or a
guaranteed fixation distance. Project its direction separately through each
eye's geometry; do not invent two eye measurements. Require active input,
valid/tracked pose and acceptable timing; extension presence alone is
insufficient. The sample timestamp can be unavailable or express a predicted
pose, so request/receipt time must remain distinguishable from sample age.
[Khronos gaze
contract](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/extensions/ext/ext_eye_gaze_interaction.adoc),
[sample
timestamp](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrEyeGazeSampleTimeEXT.html).

Start with simulated center movement in the GPU harness. On real hardware,
measure smoothing delay, sample freshness, blink behavior, large jumps and
eye-first/head-following movements. A brief hold and a bounded return to the
tested fixed region are candidate policies, not qualified defaults. On focus
loss, session stop, reference change or persistently invalid input, clear gaze
ownership and use the documented fixed/full-frame policy. Log validity and age
summaries without routinely recording a user's gaze trace.

Negotiate support at instance creation and create optional action resources at
session setup if live gaze switching is desired. Switching between prepared
fixed/gaze modes can then be live; enabling previously unnegotiated support or
changing the Windows runtime requires restarting the session/game. Quest 3
fixed placement remains a complete test target; a working Pimax headset session
alone does not prove its selected runtime supplies gaze.

## Controls and resource budget

Keep current full-frame DLSS selected by default. Use existing advanced crop
settings for the development experiment, with their present meanings. A later
user-facing control should distinguish `Full frame`, `Fixed region` and, only
when supported, `Eye tracked`. Report the active treatment, center/peripheral
dimensions, and why a requested mode is unavailable. Preserve the concise
floating `GPU` and `CPU` labels; detailed costs belong in F8 and logs.

Crop width/edge, peripheral scale and supported preset changes can be applied
live at a paired-frame boundary, with history reset and a possible one-time
allocation hitch. They do not change the game's rendering resolution. Gate new
gaze settings and any new public control on the completed validation; this
document neither renames existing keys nor enables experimental VRS.

Track allocated bytes, feature count and retired resources. A single 4404x4348
four-byte output is about 73 MiB per eye, before NGX's private allocations.
Avoid retaining full, crop, peripheral and unused native-TAA surfaces
unnecessarily. Cap scratch storage and retire it through the existing context
ownership rules. Compile any new shaders through the established build/cache
path; do not restore the startup black-screen problem by synchronously
compiling several large shaders on the first visible frame.

## Measurement plan and decision gates

Use current asynchronous GPU timers and associate samples with original frame
sequence and eye. Break the temporal region into shared preparation, peripheral
reduction, peripheral NGX, central NGX, composition/UI resolve, and total.
Preserve the enclosing timer to catch uncategorized work. Existing aggregate
NGX averages mix calls; two evaluations per eye must not be reported as a
single-eye saving merely because the average call got shorter.

The accounting target is:

```text
delta = full_frame_temporal_cost
        - (shared_preparation + peripheral_reduction + peripheral_NGX
           + central_NGX + composition_and_UI + additional_copies)
```

Compare stereo sums for matching valid samples. CPU submission time, submit
wall time, render GPU time and runtime compositor work are different
quantities; do not add nested scopes or infer compositor GPU cost from
`xrEndFrame`. GPU utilization or an unchanged capped FPS is not a substitute
for measured time. No new vendor timing dependency is needed.

Record build, GPU/driver, DLSS runtime hash/preset, OpenXR runtime, headset,
refresh/reprojection state, actual input/output sizes, HMD quality, mip bias,
sharpness and scene. Capture matched A/B/A windows after feature creation and
temporal warm-up, with VRS and other foveation disabled. Start with the Pimax
at a representative high resolution and repeat fixed placement on Quest
3/Virtual Desktop. Compare against both same-resolution full-frame DLSS and a
modestly lower-resolution full-frame option: the latter may provide a better
quality/time trade without a seam.

Proposed acceptance thresholds, to make the experiment falsifiable:

- At least 0.5 ms reduction in stereo render GPU time **and** 5% of the
  matching baseline median in representative GPU-limited scenes, reproduced
  across three A/B/A windows and larger than measured run-to-run variation.
- No persistent increase greater than 2% in p95 render GPU time after warm-up,
  and no new recurring CPU stalls. Report allocation/preset-switch stalls
  separately instead of discarding them from the usability result.
- User accepts peripheral softness and cannot identify an objectionable stereo
  seam, pulsing, delayed sharpness or damaged UI during the movement tests
  below. Timing success cannot override a quality failure.

These are proposed project gates, not measured outcomes. If benefits occur only
on the highest-resolution rig, scope the feature accordingly. Stop if
affordable margins do not pass quality checks, if gaze quality is inadequate,
or if preparation/periphery cost consumes the useful saving.

## Implementation sequence

| Stage | Deliverable | Exit condition |
| --- | --- | --- |
| 0. Reproduce and price | Extend existing crop/motion/cost harnesses; record the current full-frame baseline and old experimental result | Actual NGX measurements separate from WARP/math checks; current quality failures reproducible |
| 1. Restore pipeline parity | Shared input/mask preparation and UI resolve; stable fixed crop geometry; explicit per-eye timing | Foveation off preserves baseline output; cropped path passes UI, motion, format and failure tests |
| 2. Fixed-region pilot | Native producer integration, paired live changes, bounded resources and optional history margin | One combined headset checklist plus repeated timing windows passes; otherwise stop here |
| 3. Optional native gaze | Standard action lifecycle, versioned snapshots, origin correction and loss policy | Simulated motion tests pass first, then real tracker quality/latency and reconnect tests |
| 4. Product decision | Supported controls, diagnostics and documentation based on measured profiles | Ship opt-in only if benefit and accepted image quality justify ongoing support |

Do not turn each checkbox into a separate flight. Complete desktop checks,
gather all discriminating counters/captures, then run one prepared headset
session for each stage that needs perception. No implementation or flight is
required to review this design.

## Tests required before adoption

Desktop checks should extend the existing `tools/temporal_test`,
`tools/native_temporal_test`, `tools/temporal_switch_test` and `tools/smoke`
coverage, adding a focused crop-policy test where appropriate. CPU/WARP tests
validate geometry, state and pixels; they do not qualify a proprietary DLSS
model. A dedicated NVIDIA validation mode must execute actual crop, peripheral
and full-frame evaluations and record a skip honestly when unavailable.

| Test group | Required cases |
| --- | --- |
| Registration | Asymmetric/canted eyes, vertical offset, crop near each edge, 1:1 and noninteger scaling, odd dimensions, nonzero bounds, supported formats; unsupported flipped/array/MSAA inputs keep the documented behavior |
| Temporal quality | Static fine lines, moving text/geometry, slow and fast head-like pans, rapid nods, stopping, large crop jumps, new content at the leading edge; measure crop age and settling against a full-frame reference |
| UI parity | Changing digits, target chevrons, orbital lines, cockpit panels, retained-text removal, smoke/transparent overlap, on-foot tool/HUD and FSS transitions |
| State and lifetime | Off/on and full/crop switches, preset changes, live render scaling, projection/reference change, skipped or duplicate eye, right-eye failure, device/session restart and normal shutdown |
| Isolation and cost | Original inputs unchanged, guard pixels outside subrectangles, context state restored, no busy-wait/readback in play, bounded resources after repeated changes, correct two-eye/two-region timing |
| Gaze, later | No extension/device support, inactive/denied input, stale/zero/future timestamps, blink, focus loss, recenter, tracker recovery, simulated and actual gaze clearly distinguished |

The eventual headset session should cover menu text, cockpit instruments,
on-foot text/tools, ship surfaces and orbital lines; slow turns, fast nods,
eye-first glances, and holding still afterward. Include FSS, a scene
transition, live settings changes, recenter, and normal exit. Confirm both eyes
agree and the menu remains visible. Use Insert eye dumps for registration and a
short sequence for temporal defects: a stationary screenshot cannot demonstrate
that a seam is stable during movement.

## Open questions to settle with evidence

1. How much NGX time remains in the current native build at the user's actual
   resolution, and what is the complete incremental peripheral/composition
   cost?
2. Does the modern UI path keep text readable outside the center, or does
   reduced peripheral resolution itself make the trade unacceptable?
3. What fixed region and history margin survive natural head/eye motion without
   erasing the saving?
4. Which peripheral presets actually work with the shipped DLSS runtime, and
   how do their settling behavior and cost compare? Upstream's preset E label
   is not a local performance guarantee.
5. Does the selected OpenXR runtime supply gaze with sufficient freshness and
   accuracy for a smaller region, including near cockpit text where one
   combined direction does not provide measured vergence?

The recommended next action is Stage 0, followed by pipeline parity if the
renewed measurements justify it. The existing full-frame mode remains the
reference throughout.

## Journal

### 2026-09-15: Stage 0 instrumentation built, reviewed, fixed, traced

**Built** (branch `claude/dlss-foveation-design-e02cfe`, rebased onto main
96902e6, measurement only):

- `temporal_pass.cpp`: seven region timers (prep, reduce, periphery, centre,
  full, compose, ui) nested inside each ring slot's existing total timer, so
  a region never opens a DisjointClock record of its own; "other" is the
  total less the seven, taken per pair before the percentile. Eyes pair by
  the per-frame row counter; pairs sum into windows of 600 keyed by
  treatment, output size, format and config generation; each closed window
  writes one `temporal aa price` line. The treatment label is what ran
  (composited fovea, full-frame NGX, or own history), not what was asked.
  The `ui` region times whichever UI route ran after NGX: the legacy
  resolve, or the deferred replay of the captured UI that main's e97e2fe
  made the live route when it succeeds (the two are exclusive, so the
  region begins once per slot). Without that the deferred route's cost
  would have sat in "other" with `ui` reading 0.00 in the field.
- `dlaa.cpp`: NGX evaluate totals per role (full, centre, periphery) and
  per eye; the adapter name stamped once at the first DLAA/DLSS ask. Nothing
  measured off the old pooled figure compares with these.
- `menu.cpp`: a "Temporal AA price" F8 line from the last closed window.
- `tools/smoke/smoke.cpp`: a stereo price-report probe, six full-frame
  DLAA frames, six steady-periphery fovea frames, three own-history frames,
  both eyes each, asserting each closed window's medians and zero drops.
  `build.bat` compiles `smoke.exe` but does not run it; it was run by hand
  as `build\smoke.exe build\d3d11.dll`.

**Review of the first diff** (main session) found four measurement-integrity
defects and one gap, fixed in a second round:

1. Unmeasured totals priced at 0 ms (no lease, failed end, Invalid poll).
   Now a per-slot validity flag; a pair with an unmeasured eye is dropped
   and counted.
2. A lone eye evicted from the pairing ring was priced as a stereo pair.
   Now dropped and counted; the ring drains at shutdown before the flush.
3. The arms sampled at different rates: full-frame DLSS took a timing slot
   one frame in 32 without diagnostics, the fovea every frame, so a 60 s A
   window would never have filled. Now a slot every frame the timing owner
   accepts, ring 8 to 16 slots, staging readback gate unchanged. The pooled
   F8 "temporal AA ms" average samples every frame as a side effect.
4. The treatment label recorded the intent; a persistent stand-down would
   have labelled own-history frames "foveated ngx".
5. No NGX stereo pair had ever formed on the desk: every smoke probe called
   eye 0 only. Hence the stereo probe above.

**Smoke evidence** (`build\edvr_logs\edvr_gfx_20260915_190533.log`, RTX
5090, 400x304 desk source, both lines verbatim; this is the build of the
same code before the rebase onto 96902e6, and the rebased build is smoked
again before it is installed, that run's lines going in the flight entry):

```
temporal aa price: full-frame ngx, 400x304, 8 stereo pairs (window closed), ms per pair median/p95: prep 0.03/0.04 reduce 0.00/0.00 periphery 0.00/0.00 centre 0.00/0.00 full 0.46/15.12 compose 0.00/0.00 ui 0.00/0.00 other 0.00/0.01 dropped 0 unmeasured pairs, 0 lone eyes, 0 no-slot frames, 0 region leases
temporal aa price: foveated ngx, 400x304, 6 stereo pairs (window closed), ms per pair median/p95: prep 0.02/0.03 reduce 0.01/0.02 periphery 0.41/24.37 centre 0.41/4.40 full 0.00/0.00 compose 0.01/0.02 ui 0.00/0.00 other 0.00/0.00 dropped 0 unmeasured pairs, 0 lone eyes, 0 no-slot frames, 0 region leases
```

The regions land where they should (full only under full-frame; reduce,
periphery, centre and compose only under the fovea) with zero drops. The
p95 outliers are first-frame NGX warm-up inside a six-frame window; the
medians are the figure. Absolute values at 400x304 say nothing about the
flight.

**Whole-frame GPU needs no new code.** Under native OpenXR
`perf_monitor.cpp` runs a recurring native benchmark whenever the native
session is present: 2 s warm-up, 30 s sample, 2 s drain, then one
`native benchmark: ... gpu p50/p95/p99 ...` line carrying sizes, AA and
DLSS mode and the build. An ini reload, an NGX re-create or a menu event
bumps its scope and restarts the window, so A/B/A arms separate by
construction; each arm must last at least 75 s to hold one full cycle.
That line is the acceptance metric; the price line explains where the
difference came from. Folding frame GPU into the price line was dropped
as redundant.

**Known gaps, deliberately left:** the fovea's geometry (degrees, crop
size) is not on the price line, only on the one-shot "DLSS where you look
ENGAGED" line; the door GPU figure reaches the log only on dropped or long
frames (the benchmark line supersedes it); driver and DLSS DLL versions are
not stamped; the fovea arm does a per-frame 208-byte stats readback outside
the timed span that the full-frame arm does not (negligible, but it is a
B-arm-only difference); in the smoke the row counter never advances, so its
pairing relies on the forced GPU completion after every call.

### 2026-09-15: Stage 0 flown in Frontier, A/B/A on the Pimax

**Build, smoke, install.** The rebased build (749a4e9, = main) was smoked
on the desk before the install, `build\edvr_logs\edvr_gfx_20260915_193143.log`,
the lines the entry above promised, verbatim:

```
temporal aa price: full-frame ngx, 400x304, 8 stereo pairs (window closed), ms per pair median/p95: prep 0.03/0.03 reduce 0.00/0.00 periphery 0.00/0.00 centre 0.00/0.00 full 0.37/14.71 compose 0.00/0.00 ui 0.00/0.00 other 0.00/0.01 dropped 0 unmeasured pairs, 0 lone eyes, 0 no-slot frames, 0 region leases
temporal aa price: foveated ngx, 400x304, 6 stereo pairs (window closed), ms per pair median/p95: prep 0.02/0.03 reduce 0.01/0.02 periphery 0.43/22.76 centre 0.41/3.00 full 0.00/0.00 compose 0.01/0.02 ui 0.00/0.00 other 0.00/0.00 dropped 0 unmeasured pairs, 0 lone eyes, 0 no-slot frames, 0 region leases
```

Installed into the Frontier directory with `install_edvr.py --target
frontier` and verified with `--verify-only`. Sean flew it the same
evening: log `edvr_gfx_20260915_195200.log`, 19:52:00 to 19:56:01,
version line `v0.17.0-rc.2-5-g749a4e9 (build 6AA9F0B0)`, and
`--expect-build HEAD` answered "build matches" while HEAD was 749a4e9
(from any later commit ask for `--expect-build 749a4e9`). Pimax Crystal
Super on
"Pimax OpenXR", 2576x2544 per eye in, 3964x3913 out, 90 Hz, aa dlss,
model k, RTX 5090. (The price line says 3964x3914 for the same output:
the two instruments take the height from different places; one pixel,
ignore.)

**What was flown** is not quite the brief. The arms were switched with
the F8 menu, not the ini (same hot reload, same effect on the pass), and
the angle was swept inside B: temporal_aa_fovea 0 -> 40 at 19:54:37,
40 -> 80 at 19:55:02, 80 -> 49 at 19:55:16, 49 -> 40 at 19:55:19, 40 -> 0
at 19:55:36; the game closed at 19:56:02. So A 157 s, B40 25 s, B80 13 s,
B49 4 s, B40 17 s, A 25 s. The first 90 s of A were a different workload
(benchmark cpu p50 0.7-2.6 ms, gpu p50 4.7-6.9 ms: still loading in), and
the five price windows from 19:52:26 to 19:52:54 had 32-150 region-lease
refusals each (of 3600 region begins a window; a refused region prices at
0 for that eye), so both are discounted. From 19:53:32 the benchmark's CPU
p50 held at 3.6-3.9 ms in every window whatever the arm, and no price
window dropped anything: 0 unmeasured pairs, 0 lone eyes, 0 no-slot
frames, 0 lease refusals. The stereo pairing works in the field.

Every menu open or close restarts the benchmark scope
(perfMonitorNoteEvent, kEvMenu), so no B window completed its 30 s: the
longest, 17.4 s, ran while the menu stayed open from 19:54:43 to
19:55:02. The trailing A lasted 25 s against a 34 s cycle and has no
benchmark line at all.

**The lines**, verbatim. Benchmark windows 5 and 7 are A, 9 and 14 are B
at 40 deg, 10 is B at 80; the price lines at 19:54:16 and 19:55:50 are A
(leading and trailing), 19:54:51 B40, 19:55:09 B80, 19:55:19 B49:

```
[19:54:09.693] native benchmark: window 5, scope 3334388546740028794, status completed, sample 30000 ms [103151296..103181296], drain 2000 ms (finished 103183296), cpu p50/p95/p99 3.682/5.074/5.962 ms valid 2688 invalid 2 missing 0, gpu p50/p95/p99 8.841/9.908/10.817 ms valid 2687 invalid 3 missing 0, input 2576x2544/2576x2544 output 3964x3913/3964x3913 refresh 90000 mHz, runtime="Pimax OpenXR" headset="Pimax Crystal Super" aa="dlss" dlss="k" build="v0.17.0-rc.2-5-g749a4e9"; elapsed windows are independent CPU/GPU samples.
[19:54:37.359] native benchmark: window 7, scope 5566703953707618203, status scope-changed, sample 15890 ms [103195078..103210968], drain 0 ms (finished 103210968), cpu p50/p95/p99 3.607/4.898/5.354 ms valid 1430 invalid 0 missing 0, gpu p50/p95/p99 9.099/10.333/10.847 ms valid 1429 invalid 0 missing 1, input 2576x2544/2576x2544 output 3964x3913/3964x3913 refresh 90000 mHz, runtime="Pimax OpenXR" headset="Pimax Crystal Super" aa="dlss" dlss="k" build="v0.17.0-rc.2-5-g749a4e9"; elapsed windows are independent CPU/GPU samples.
[19:55:02.826] native benchmark: window 9, scope 14495965581577975839, status scope-changed, sample 17391 ms [103219046..103236437], drain 0 ms (finished 103236437), cpu p50/p95/p99 3.849/5.177/5.738 ms valid 1564 invalid 0 missing 0, gpu p50/p95/p99 8.082/9.283/9.895 ms valid 1563 invalid 0 missing 1, input 2576x2544/2576x2544 output 3964x3913/3964x3913 refresh 90000 mHz, runtime="Pimax OpenXR" headset="Pimax Crystal Super" aa="dlss" dlss="k" build="v0.17.0-rc.2-5-g749a4e9"; elapsed windows are independent CPU/GPU samples.
[19:55:33.337] native benchmark: window 14, scope 46199368742522664, status scope-changed, sample 9250 ms [103257687..103266937], drain 0 ms (finished 103266937), cpu p50/p95/p99 3.896/5.172/5.894 ms valid 832 invalid 0 missing 0, gpu p50/p95/p99 8.129/9.361/10.105 ms valid 831 invalid 0 missing 1, input 2576x2544/2576x2544 output 3964x3913/3964x3913 refresh 90000 mHz, runtime="Pimax OpenXR" headset="Pimax Crystal Super" aa="dlss" dlss="k" build="v0.17.0-rc.2-5-g749a4e9"; elapsed windows are independent CPU/GPU samples.
[19:55:12.514] native benchmark: window 10, scope 3922609364708865138, status scope-changed, sample 6391 ms [103239734..103246125], drain 0 ms (finished 103246125), cpu p50/p95/p99 3.902/5.250/5.746 ms valid 575 invalid 0 missing 0, gpu p50/p95/p99 8.987/10.277/10.973 ms valid 574 invalid 0 missing 1, input 2576x2544/2576x2544 output 3964x3913/3964x3913 refresh 90000 mHz, runtime="Pimax OpenXR" headset="Pimax Crystal Super" aa="dlss" dlss="k" build="v0.17.0-rc.2-5-g749a4e9"; elapsed windows are independent CPU/GPU samples.
[19:54:16.169] temporal aa price: full-frame ngx, 3964x3914, 600 stereo pairs (600 pairs), ms per pair median/p95: prep 0.50/0.84 reduce 0.00/0.00 periphery 0.00/0.00 centre 0.00/0.00 full 3.34/3.84 compose 0.00/0.00 ui 0.25/0.69 other 0.00/0.00 dropped 0 unmeasured pairs, 0 lone eyes, 0 no-slot frames, 0 region leases
[19:54:51.148] temporal aa price: foveated ngx, 3964x3914, 600 stereo pairs (600 pairs), ms per pair median/p95: prep 0.68/1.03 reduce 0.09/0.09 periphery 0.99/1.51 centre 0.60/0.92 full 0.00/0.00 compose 0.25/0.54 ui 0.00/0.00 other 0.00/0.00 dropped 0 unmeasured pairs, 0 lone eyes, 0 no-slot frames, 0 region leases
[19:55:09.579] temporal aa price: foveated ngx, 3964x3914, 600 stereo pairs (600 pairs), ms per pair median/p95: prep 0.67/1.02 reduce 0.08/0.09 periphery 0.99/1.53 centre 1.73/2.30 full 0.00/0.00 compose 0.25/0.56 ui 0.00/0.00 other 0.00/0.00 dropped 0 unmeasured pairs, 0 lone eyes, 0 no-slot frames, 0 region leases
[19:55:19.965] temporal aa price: foveated ngx, 3964x3914, 331 stereo pairs (window closed), ms per pair median/p95: prep 0.68/1.02 reduce 0.09/0.10 periphery 0.99/1.51 centre 0.71/1.04 full 0.00/0.00 compose 0.25/0.55 ui 0.00/0.00 other 0.00/0.00 dropped 0 unmeasured pairs, 0 lone eyes, 0 no-slot frames, 0 region leases
[19:55:50.181] temporal aa price: full-frame ngx, 3964x3914, 600 stereo pairs (600 pairs), ms per pair median/p95: prep 0.50/0.85 reduce 0.00/0.00 periphery 0.00/0.00 centre 0.00/0.00 full 3.34/3.84 compose 0.00/0.00 ui 0.25/0.57 other 0.00/0.00 dropped 0 unmeasured pairs, 0 lone eyes, 0 no-slot frames, 0 region leases
[19:54:37.752] temporal aa: DLSS where you look ENGAGED -- NVIDIA runs on a 734x734->1128x1128 crop (DLSS, 40 deg, round) around the straight-ahead point (the discs meet at infinity) at (2364, 1954) of the 3964x3914 output, 8.2% of its pixels; the periphery is NVIDIA's too -- DLAA on a 1982x1956 copy (50% of the output each way), upscaled bicubically; the pass's own history stands aside; blended over 6 deg (162 px). NVIDIA's price is in the DLAA totals.
[19:54:37.816] monitor: LONG FRAME -- 444.9 ms between Presents (runtime predicted period 11.1 ms), no WaitGetPoses, CPU busy, compositor, reprojection, or door samples; game creations: 41 textures, 36 buffers, 2 shaders (786.6 MB); EDVR events: reload (303 ms), shader compile (303 ms), raster upload. This is frame 15965; the flip timeline is not armed, so there are no table changes to order against it.
```

**The numbers**, medians; the pass is ms per stereo pair, the frame is
the benchmark's gpu p50 / p95 per frame:

| Arm | Benchmark windows (s) | gpu p50 / p95 | cpu p50 | Pass per pair |
| --- | --- | --- | --- | --- |
| A leading | 5 (30), 6 (7.8), 7 (15.9) | 8.84-9.10 / 9.91-10.37 | 3.61-3.68 | 4.10 = prep 0.50 + full 3.35 + ui 0.25 |
| B 40 deg, periphery 0.5 | 8 (2.2), 9 (17.4), 14 (9.3), 15 (1.5) | 7.95-8.13 / 9.22-9.55 | 3.80-3.90 | 2.61 = prep 0.68 + reduce 0.09 + periphery 0.99 + centre 0.60 + compose 0.25 + ui 0 |
| B 80 deg | 10 (6.4), 11 (1.6) | 8.99-9.00 / 10.28-10.39 | 3.77-3.90 | 3.74, centre 1.73 |
| B 49 deg | 12 (1.7) | 8.20 / 9.67 | 3.88 | 2.72, centre 0.71 |
| A trailing | none (25 s) | | | 4.10 = prep 0.50 + full 3.35 + ui 0.25 |

Inside the leading A the p50 drifted 0.26 ms (window 5 to 7); inside B40
the four windows spread 0.18 ms. In the steady stretch (price windows from
19:53:56 on) full held at 3.34-3.38 and ui at 0.25 window to window, and
every foveated region held to 0.01 ms across the B40 windows; the earlier
A windows ranged full 2.99-3.59 and ui 0.24-0.49 while the scene was still
loading. Prep's median in A sat at either 0.23-0.33 or 0.48-0.55 from
window to window while its p95 stayed at 0.84-0.85 throughout: something
in prep runs on some frames and not others, unexplained, and the 0.50 used
above is the steady-stretch value.

**Against the gates.**

- At least 0.5 ms and 5% off the baseline median: B40 against the
  adjacent A window (7) is -1.02 ms p50, -11.2%; against the one completed
  A window (5) -0.76 ms, -8.6%; both above the drift inside A. PASSED
  against the leading arm. Not reproduced across three A/B/A windows: the
  trailing A has no benchmark window, only price lines, and those match
  the leading A to the hundredth (prep 0.50, full 3.34, ui 0.25), so the
  pass came back to baseline but the frame's return is unmeasured.
- No p95 rise over 2%: B40 p95 9.28 against 9.91-10.37, down 6-10%.
  PASSED.
- No new recurring CPU stalls: none; but the benchmark's cpu p50 sat
  0.2 ms higher in every B window (3.77-3.90 against 3.61-3.68, about 6%),
  a steady price rather than a stall. Candidates: the second NGX
  evaluation per eye, the reduce and compose dispatches, and the per-frame
  208-byte stats readback the entry above listed as B-only. The frame is
  GPU-bound (8-9 ms of 11.1) so it does not offset the GPU saving today;
  Stage 1 removes the readback and measures again. The engage cost one
  444.9 ms frame (the LONG FRAME line: two NGX features per eye, a 303 ms
  shader compile, 41 textures of 786.6 MB), the preset-switch stall the
  gates say to report separately.
- Quality: no verdict yet from Sean, and B did not run the UI route (next
  paragraph), so its UI is not the product's UI in any case.

**The accounting**, the doc's delta at 40 deg / 0.5:

```text
delta = 4.10 - (0.68 prep + 0.09 reduce + 0.99 periphery + 0.60 centre
               + 0.25 compose + 0.00 ui + 0.00 other) = 1.49 ms per pair
```

The crop replaces full 3.35 with centre 0.60, 2.75 ms saved; reduce,
periphery and compose take 1.33 of it (48%), prep another 0.18, and ui's
0.25 is not paid at all. With UI parity the pass nets about 1.25 ms. The
frame nets 0.76-1.02 ms; the gap to the pass figure sits inside the
run-to-run band, and medians do not add exactly. H1 stands confirmed on
both figures. ruled out: H2 (reduce + periphery + compose eat most of the
crop's saving), because they eat 48% at periphery scale 0.5 and the pass
still nets 1.5 ms; a larger periphery scale would move that number.

**The UI route is absent under the fovea, by construction.** `ui` reads
0.00 in every foveated window because nothing ran: in temporal_pass.cpp
the UI resolve dispatch and the deferred replay (`uiDeferredApply`) both
live inside the full-frame NGX block, after `dlaaEvaluate`; the fovea
branch (prep, reduce, periphery, centre, then the compose dispatch into
`foveaOut`) has neither, and `foveaOut` is what goes out. So B's UI was
the periphery's 1982x1956 DLAA upscaled bicubically, with the crop's DLSS
over the centre: c27fe74 and e97e2fe put the UI replay on the full-frame
branch only, as the baseline-drift note said. That makes UI parity the
first Stage 1 deliverable and means the 40 deg picture Sean saw is not the
one that would ship. The instrument is right: 0.00 is the true price of a
route that did not run, and "other" stayed 0.00 so nothing hid elsewhere.

**Angle.** The centre role scales with the crop: 0.60 ms at 40 deg (8.2%
of the output, 1128x1128), 0.71 at 49, 1.73 at 80, where the frame's p50
(8.99-9.00) is back at A's (9.05-9.10). The periphery, reduce and compose
costs did not move with the angle. On this rig the saving lives at the
small angles, which is also where the seam sits nearest the centre of the
view; that is the trade the picture verdict decides.

**Next.** Stage 1 parity (the Status block lists it, UI first), then the
same A/B/A on the parity build with the trailing A held 75 s, arms changed
through the ini so the menu does not restart the benchmark, and the seam
verdict written into this journal. One flight, not two.
