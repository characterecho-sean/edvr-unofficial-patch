# Station flicker and supercruise planet blur

September 13 investigation, starting at 59216e8. The Steam graphics and
VR logs were verified against that build with tools/edvr_log.py. The
local flight uses SteamVR, RTX 5090, DLSS K, 2774 x 2740 input and 4268
x 4216 output per eye. It is distinct from the supporter's Quest 3 /
Steam Link / 4080 Super performance report. The user reports station
performance has improved but intermittent flicker has returned since
0.15.1.

## Captured evidence

- Eye 051017: supercruise approach, ringed Earth-like planet. Detail in
  the reconstructed planet alternates between softer and clearer over
  the 16-frame sequence. The supplied motion mostly follows the camera;
  the opaque surface is VS 71DD9863DCFC0986, PS 43E5E6EB67AC751B, DC 0
  #49 (2304 indices). This VS computes position from cb0[9..11] and
  cb1[270..273]. It is outside the existing exact terrain, ring and
  rigid-pool motion families and outside the constant-buffer snapshot
  watch list. Atmospheric VS 9FFA5D5E79F04873 is a different draw and
  cannot stand in for its opaque geometry.
- Eye 051208: nearby station. All 512 rigid records are occupied; 504
  are valid transforms but only 124 have unambiguous predecessors. Most
  station pixels therefore use the existing fallback motion. The
  repeated station keys contain distinct transforms, not identical
  repeated passes. Increasing the cap or deduplicating transforms is not
  an established fix.
- Both 16-frame motion ledgers have flags=2, dlHistory=1, rowsOk=1,
  jumped=0 and rowsBound=1 throughout. No captured frame requests a
  history reset.
- Pause at 05:10:34.522 and follow-up at 05:10:36.522 cover 1380 unique
  frames, f32355..33734. No frame in those histories was withheld.
  Scene/pool geometry is coherent. The current Pause history contains no
  per-frame temporal outcome, jitter, depth or history-reset state.
- The newer background-history rejection (7c6e9ef) invalidates 45825 of
  7600760 input pixels in 051017, and 44114 in 051208. These lie mostly
  along silhouettes and UI edges; they do not cover the entire world or
  explain the planet's interior blur. Its unconditional 3x3
  nearest-depth test can reject an unchanged silhouette's neighboring
  background. Test that separately from an actual uncovered hull.

## Ruled out or not established

- Ruled out: transition-frame replacement caused the marked supercruise
  flicker, because none of the 1380 retained Pause frames was withheld.
- Ruled out: repeated station records are merely identical duplicate
  passes, because their projected transforms and positions differ.
- Ruled out: DLSS was explicitly reset during either eye sequence,
  because all 16 ledger entries retain history. This does not cover the
  later Pause event.
- Not established: the 59216e8 batching optimization caused the visual
  regression. The source-write flushes, unchanged-data batch boundaries,
  predication restoration and GPU-writable fallback remain in place.
- Not established: all transition detections elsewhere in the flight
  were false. The user also entered/exited supercruise during this run.

## Discriminating evidence needed

Record temporal pass results alongside Pause: requested mode/reset
flags, actual output mode and extent, input jitter, consecutive history,
depth, camera/body validity and exact-motion bindings. Missing calls
must be distinguishable from successful reconstruction. Keep this
bounded and CPU only; do not add continuous GPU readback or per-draw
queries.

Capture the actual opaque planet and cloud-layer draw transforms and
geometry during an armed eye dump. Save the previous depth used for
background rejection. The existing eye images, current depth and ledger
then distinguish an incorrect object transform from rejection at edges.

## Controlled checks and diagnostic build

The existing GPU motion-kernel runner reproduces the static-edge
problem: an unchanged depth image, identity camera and zero jitter agree
with the history-preserving reference for the 1 x 1 control. Fixture 01,
an 8 x 8 image split between constant-depth geometry and sky, differs in
the motion output when production background rejection runs. The
reference differs only by disabling that rejection. This establishes
inappropriate static edge invalidation in 7c6e9ef; it does not establish
the cause of the marked whole-world flash. Scratch reproduction:
static_edge.py and static-edge-gpu.log under build/review_motion/sep13.

The captured Earth atmosphere's center supplies an independent approach
check. Project cb0[4].w, cb0[5].w and cb0[7].w into pixels, subtract the
consecutive raster jitter, and compare against the camera path recorded
in the eye CSV. Across the captured sequence the disagreement is
0.50--4.50 input pixels/frame. The planet center moves by 55--674 km in
depth between captured frames while the fallback camera translation is
sub-millimetric. This proves missing approach motion for the
atmosphere's center; the opaque surface and cloud transforms still need
their own capture before applying a geometry correction. Capture stalls
affect the time between these frames; these are not normal-flight speed
estimates.

The diagnostic update changes no reconstruction shader or performance
optimization. Pause and its delayed follow-up now write the last 4096
temporal eye calls, including actual output selection, reset requests
and NVIDIA reset decisions, size/source-screen changes, jitter,
depth/history availability and motion sources. Missing calls and failed
output are explicit. The ongoing record is CPU-only and bounded; log
writing happens only on the diagnostic action. Eye capture adds valid
previous depth (PrevZ), and watches both planet surface/cloud families,
retaining their first-frame geometry and each frame's constants within
the existing caps.

Validation: absolute-path full build passed, including bounded-history
wrap/reset/failure tests and WARP capture tests for both planetary
shader families. NVIDIA graphics smoke passed. The deliberate
static-edge probe fails against current rendering as described above; it
is diagnostic evidence, not a passing visual-fix test. No visual fix is
claimed yet.

## Follow-up: non-landable planet approach

The next Steam flight is verified 857a842 in both DLL logs
(edvr_gfx_20260913_055421.log). The user reports the blur appears
limited to non-landable worlds, and did not observe skybox flicker this
time. That is not evidence that the separate flicker is fixed. Capture
055834 is inside the station; 055925 is the supercruise Earth-like
approach.

This capture uses the OpenVR graphics path through SteamVR, with DLSS
preset K at 2774 x 2740 input and 4268 x 4216 output per eye. The motion
capture is in the graphics proxy and has no SteamVR compositor
dependency.

The new snapshot succeeds: both 71DD9863DCFC0986 surface and
3530A6FD15EDE145 cloud passes retain their first-frame POSITION geometry
and all 19 frames of draw-time constants, for both eyes. Surface draws
use 2304 and 9216 indices respectively. Their original vertex shader
projects cb0[9..11] through cb1[270..273]. The precomposed clip X/Y/W in
cb0[4], cb0[5] and cb0[7] agrees within 0.000220 input pixels across all
captured surface vertices. PS 43E5E6EB67AC751B writes colour targets and
has neither discard nor a depth export.

Correction: retain the opaque planet's affine clip transform using mode
4 of the existing hologram/ring motion records. Match its surface/mesh
and draw slice, excluding animated shading constants from identity. LOD
changes and ambiguous matches decline. Record the original VS's visible
samples into existing motion coverage with depth EQUAL and zero depth
writes. No UI mark, private UI depth, distance cutoff or sharpening is
added. The temporal consumer excludes foreground UI and checks final
scene depth before using that correspondence. Landable terrain keeps its
existing patch-motion path. Skybox history rejection is unchanged.

Scope is the captured opaque surface shader pair. Clouds over its
visible surface inherit that surface motion; independent atmosphere and
cloud shell motion, and other non-landable surface shader variants, are
not claimed corrected.

Integration uses the depth probe's eye assignment, not the UI colour
target table: an opaque deferred target must not consume that table's
two UI eye slots. The shared 128-record per-eye budget remains bounded;
no new full-eye motion texture or continuous GPU readback is added.
Planet coverage uses the normal original-draw wrapper for indexed draws,
independently of the UI reissue flag. Unsupported or predicated
pipelines decline before changing draw state.

Focused validation passes: 864 captured pairs through the GPU matcher,
all matched, maximum error 0.000362 input pixels against the original
surface VS position calculation. The comparison includes both eyes, both
captures and 4440 distinct captured vertices. Production coverage tests
preserve every game colour/depth sample, exclude nearer geometry and
restore graphics bindings. Mode 4 tests cover material animation,
first-frame history, LOD changes and UI/foreground exclusion. In-headset
confirmation of the approach correction is still required.

The initial full build caught incorrect UI-mask coordinates in the new
consumer. Coverage uses the full scene grid, while UI marks use the eye
region grid. Mode 4 now subtracts the region origin and excludes both
text categories. The regression uses a nonzero origin and tests both
categories against source and embedded motion shaders.

Final validation: absolute-path full build passed, including 55069
screen motion checks, 21110 UI coverage checks and the existing weapon,
terrain, night-vision and runtime regressions. NVIDIA hardware smoke
passed. The standalone hologram consumer fixture also declares the
UI-mask input now referenced by the shared helper; its existing 213
checks pass.
