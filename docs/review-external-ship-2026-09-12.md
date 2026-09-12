# External-camera hull motion, September 12

The user confirmed the weapon/night-vision update and then reported hull
blurring and shimmering after taking off in the external camera, then
reported cockpit geometry ghosting during roll. This investigation
confirms a large error in the DLSS motion supplied for the exterior
hull. The correction now uses original draw transforms and visible
coverage for supported rigid hull/cockpit geometry; it no longer relies
on the ship-metres split for those parts. Headset validation is still
pending.

## Flight and image evidence

The sanctioned log reader verified `edvr_gfx_20260912_115917.log` and
its VR companion against `e56cb65`, version `v0.15.1-47-ge56cb65`. This
was Valve SteamVR through EDVR's OpenVR proxy, not OpenComposite. The
logs identify a 103-by-103-degree headset signature, not a verified
headset model. DLSS preset K receives 2774-by-2740 inputs and produces
4268-by-4216 outputs at HMD quality 0.65. Night vision brightness was
changed live to 4.0 at 12:00:52.

There are two runs: `120229` shows the ship from the front exterior
camera over terrain; `120250` is back in the cockpit. The exterior run
has no night-vision draws. Both include source/treated crops, submitted
motion/depth, terrain coverage, pool copies, the draw ledger and
existing UI/terrain snapshots.

In `eye_120229_SceneZ.bin`, the hull's 10-to-100-metre depth band
contains 694,443 pixels (median depth 19.46 m, maximum 57.95 m). It is
well separated from the terrain, which starts at 16.88 km in this view.
Excluding UI coverage, the median difference between the supplied MV and
head-only prediction over the hull is 170.75 input pixels; the 90th
percentile is 553.72. In contrast, its median difference from the
world-camera prediction is 0.534 pixels. This is a world-motion
assignment on geometry nearly stationary in the exterior view.

A rigid image fit of bright hull pixels in successive raw crops, after a
two-pixel Gaussian blur solely for registration, gives subpixel
translations for the first three pairs: (+0.25,+0.56), (-0.81,+0.44),
(+0.24,+0.77) pixels. These measurements include raster jitter and are
not a substitute for per-vertex ground truth, but they decisively
contradict the supplied hundreds-of-pixels vectors.

The current shader uses the world-camera motion beyond the configured
10-metre near split. `takeShips` in `object_probe.cpp` rejects parts
within 20 metres and rejects clusters whose centroid translates with the
camera. Those assumptions were written for the seat view. They leave the
player's exterior hull dependent on the world fallback. Occasional
accepted moving-ship log entries are not proof of continuous or correct
hull coverage; the saved MV is the evidence of the path actually used
here.

## Ruled out and remaining evidence

- Ruled out: stale installation, because both halves report `e56cb65`.
- Ruled out: night-vision contour ghosting, because this exterior run
  contains zero night-vision draws.
- Ruled out: a simple resolution/sharpening issue as the explanation for
  the temporal blur, because the submitted hull motion is hundreds of
  pixels while the raw hull barely moves.
- Ruled out: selecting the hull from cockpit stencil bit 16. Its stencil
  values are predominantly 0 and 4; only 1,669 pixels in this depth band
  have value 20.
- Not accepted as a fix: simply remove the own-ship rejection and reuse
  sampled motion rates. The pool ledger is captured at the frame
  boundary, while camera records describe the rendering interval.
  Combining consecutive boundary pools with those cameras leaves
  residual translations ranging from centimetres to 17.77 metres in this
  run, despite the stable raw hull. They must not be combined by nominal
  frame number.

A follow-up offset test resolves that timing discrepancy: use the pool
pair one frame later than the camera interval. For representative hull
record 4, all fourteen tested displacement residuals then fall between
0.00012 and 0.00090 metres. This confirms that the hull travels with
this camera and that the boundary ledger is offset; it is not evidence
of erratic ship movement. It still does not supply the missing hull draw
constants, geometry or animated part transforms. The additional
draw-time capture is for that remaining gap, not because aligned pool
data is inherently unusable.

The desired correction uses actual hull transforms and visible geometry,
including external-camera movement relative to the ship. Substituting
head-only motion for an arbitrary distance band would fail for a free
camera, other ships and nearby scenery.

The existing `drawstate_120229.bin` contains terrain/UI transforms, but
no hull mesh snapshots. Mesh capture was restricted to the on-foot
source scene; it did not run for eye-rendered hulls. Therefore this run
cannot yet supply the original per-draw hull camera, geometry and pose
together for a replay.

## Capture change and next comparison

### Cockpit roll follow-up

The user also reports ship geometry ghosting when rolling from inside
the cockpit. Include geometry seen from the seat in this investigation;
a correction limited to the external-camera mode would not cover the
report.

The existing `120250` cockpit capture begins during a predominantly
rolling ship turn: 1.49 and 1.64 degrees relative to head rotation in
its first two frames. Reconstructing the production 3-by-3 nearest-depth
selection from its saved SceneZ, the 2,666,787 unmarked geometry pixels
within 10 metres match head-motion prediction to 0.00014 input pixels at
the median and 0.00157 at the 99th percentile. None differ by more than
one pixel. Stencil-bit-128 cockpit geometry likewise follows the head
path, with a median error of 0.00014 pixels.

- Ruled out: the external hull's large world-vector assignment over the
  nearby geometry in this particular cockpit frame. The captured MV
  matches the head path.
- Still unresolved: whether head-only motion registers the actual
  rendered cockpit/hull during roll. These vector comparisons validate
  the chosen path, not the geometry's true movement. Draw-time
  transforms can reveal camera-relative motion absent from the headset
  delta. Changing specular/reflection shading or a translucent layer can
  also ghost with otherwise correct geometry vectors.

Build `f1dac91` already captures recognized eye meshes in both cockpit
and external views; no additional DLL or settings change is needed for
this follow-up. After restarting with that build, include an eye dump
while rolling, looking directly at the ghosting geometry so it is in the
central paired crop. As of this follow-up, the newest flight is still
the historical `e56cb65` run; there are no new `.eyemesh.bin` captures
yet. Source code is unchanged by the subsequent documentation commits.

### Additional hull snapshot

An armed eye run now also writes `drawstate_<stamp>.eyemesh.bin`, in the
existing version-7 format. It records the first three matching eye
frames for the already recognized mesh shader families, preserving the
eye-ledger draw ordinal, target identity, original VS b0/b1/b2 and PS
b2, input layout, draw offsets and instance count. Full t33/t38 and VB0
copies are retained at first use per resource, frame and target;
separate eye targets cannot inherit each other's rewritten buffers.
`firstDraw` records when a shared copy was taken, so any later in-frame
rewrites remain auditable against the draw census.

The snapshot has separate limits of 4096 draws, 256 MiB of
pool/palette/instance data, and 32 MiB of vertex/index windows (256 KiB
per stream, first matching frame). It cannot crowd out the existing
UI/terrain captures. Copies are asynchronous, and writing uses the
existing readback grace period with explicit failure/decline counters.
The shader input layout is remembered at creation; eye-mesh GPU copies
occur only during an explicitly armed dump. The log always reports the
result, including zero matches or failures.

The next capture should repeat the exterior-camera flight while the hull
blurs. A second capture while orbiting the free camera would distinguish
camera attachment from ship-relative motion in the same session. No AA,
cockpit, weapon or night-vision settings need changing.

## Follow-up capture, 12:28 flight

The new flight verifies installed code `f1dac91`; the intervening
commits only changed these notes. Captures `123222` (cockpit) and
`123235` (exterior) contain 144 and 702 original eye mesh draws,
respectively, with no failed buffer copies or missing vertex shaders.
Draw frame IDs are one above the paired motion CSV IDs; the original
scene projection confirms the alignment. The user also confirms the
exterior ship is sharp while stationary.

The original EB5234DB6ADB491D and DE545DC8EE4FBB87 shaders use the
draw-time t33 pool and scene b1[270..275], including packed quaternion
rounding. A CPU decode of captured indexed vertices, skinning and these
transforms confirms the exterior hull's motion differs from the world
prediction by tens to hundreds of input pixels. After accounting for the
recorded raster jitter, the main exterior hull happens to agree with
head motion within 0.006 pixels in this attached camera. That does not
justify assigning head motion to an arbitrary distance band.

Cockpit rigid rails differ from head-only prediction by up to 0.30 input
pixels in the measured pair; captured nearby skinned parts differ by up
to 0.14 pixels. These are smaller than the exterior failure but
establish that the head delta is not exact draw motion. The correction
tracks rigid mesh transforms and exact visible coverage, independent of
the ship-metres split. Animated geometry must not be silently treated as
rigid. The completed original-shader replay below is the ground truth
for the implemented transform arithmetic.

- Ruled out: a uniform world-motion error in both views; the original
  cockpit transforms disagree with that prediction by much more than
  with the head delta.
- Ruled out: increasing render scale as a correction for this hull
  registration error; the source geometry and supplied motion disagree.

### Exact rigid mesh correction

`mesh_motion` captures the actual scene b1 and selected t33 records at
each supported opaque indexed draw. It reuses the original vertex shader
to record private coverage with equal-depth testing, then matches all
transforms in one batched GPU dispatch at eye consumption. There is no
normal-play CPU readback. Each eye has an independent 512-instance
history. Geometry buffer, original shader, layout, index range and pool
material flags identify candidates; mutable pool slots do not. Repeated
indistinguishable instances decline instead of borrowing a neighbour's
history. Vertex/index writes and missing frames invalidate history.

The consumer uses the actual part's projective mapping and verifies its
coverage against final scene depth, excluding UI and later foreground
draws. Both native TAA and DLSS remove the recorded raster-jitter delta.
This path overrides `advanced.temporal_aa_ship_metres` for supported
rigid geometry at any distance. The setting still supplies fallback
motion for unsupported/animated or ambiguous geometry; it has not been
removed or renamed. The known EB5234DB6ADB491D and DE545DC8EE4FBB87
families cover the measured opaque hull and rigid cockpit pieces.
Skinned pilot/body geometry retains its existing motion.

Two replay findings are enforced by the tests. Visible cockpit rails
have their mesh origin behind the eye (about -2.835 metres in clip W),
so projected-origin tests that require positive W reject them. Matching
uses homogeneous origin direction instead. The original shaders decode
their packed quaternion using the literal `0x38000100` (`1/32767`), not
`2/65535`; substituting the latter produced a 0.025-pixel error in the
cockpit replay. The production correction uses the actual literal.
Original inter-stage register layouts, including unused lighting/UV
outputs, are preserved in the coverage pixel shaders.

Six captured rigid meshes, including both vertex shader families and
three consecutive states, were replayed through the original DXBC and
packed geometry on WARP and NVIDIA hardware. Stream-output positions
provide independent ground truth: 109,785 visible vertex comparisons
give maximum reconstruction errors of 0.001979 input pixels on WARP and
0.001658 on hardware. The gate requires less than 0.003 pixels.

The offline hardware stress repeats 32 draws of captured hull geometry
at 2774 by 2740. It measured 0.040 ms CPU per eye, 11.796 microseconds
per sampled capture/reissue bracket (21 completed samples), and 5.621
microseconds per batched match (29 samples). This is an added-pass
microbenchmark, not an in-flight total. The flight log reports sampled
draw cost separately from batched matching. Explicit eye dumps now
include `MeshCoverage` and `Mesh` records, with eligible/matched counts
and explicit absent/failed-readback reporting.

The full absolute-path worktree build passed, including 844 mesh-motion
checks, original coverage/register linkage, independent stereo history,
pool reorder, origin-behind-eye, ambiguity, missing frames, geometry
writes, capacity bounds, exact dump serialization and both temporal
consumer grids. Existing UI, weapon, night-vision and configuration
gates passed as well. The NVIDIA DLL smoke test passed. Logs and local
original-shader replay assets remain under the ignored
`build/review_motion/sep12/flight1228/` directory.

## Follow-up capture, 13:18 flight

The installed build is verified as `da4736f`. The user reports no
visible improvement. Captures `132013` and `132016` are cockpit rolls;
`132033` is the exterior ship. `131903` is the main menu/hangar.

- Ruled out: an inactive mesh hook or missing GPU history. Both cockpit
  captures match all four rigid records; their saved DLSS vectors agree
  with the saved exact mesh mapping after the measured raster jitter
  delta, within half-float and arithmetic rounding.
- Ruled out: fixing the two initial shader families covers the exterior
  hull. Only 950 pixels in `132033` pass matched coverage, final depth
  and UI checks. The existing coverage is mainly hidden parts, correctly
  rejected behind nearer hull surfaces.
- Confirmed coverage gap: the first-eye census draws 249 and 250 render
  the main hull through previously unsupported vertex shaders
  `66DE2CADB1F4AE6B` (30,540 indices) and `61AE8EB05FDC18DD` (157,398
  indices). Both use rigid pool record 204, also used by adjacent
  supported hull parts. The original shaders use the same packed
  position, quaternion, pool and scene projection, with different
  material outputs. Additional `AACFDCF2FB9AD809` opaque material draws
  also use this transform path.

The correction must extend original-shader coverage to these material
layouts and validate their actual DXBC, including pixel-stage linkage.
Increasing the ship-metres cutoff or relaxing final-depth rejection
would conceal this gap by assigning hidden geometry's motion to other
surfaces. Cockpit skinned geometry remains a separate limitation; the
working rigid path does not establish that all cockpit shading is
temporally stable.

Local evidence is retained under
`build/review_motion/sep12/flight1318/`; raw captures and game shader
assets are not distributed.

The user clarified the cockpit report with a screenshot: the two forward
ship tips trail repeated edges when rolling. The first-eye cockpit
census also has the missing material families: draws 59/60
(28,668/38,610 indices) use rigid pool record 115, followed by two
smaller pairs on records 27 and 3. The large pair shares record 115 with
the tracked rigid frame. The screenshot's white surfaces have no old
mesh coverage. This is distinct from the correctly tracked frame and the
skinned pilot.

Coverage now handles five original output layouts, preserving the
multi-UV hull's additional registers and the detailed material's two
integer outputs. Each uses the original VS and the same exact rigid
transform calculation. Final-depth rejection, UI exclusion and the
512-instance bound stay in force. The two previously uncaptured hull
families are included in future explicit draw snapshots.

Validation: all five layouts pass the WARP regression (1,046 checks).
The expanded local replay runs the three added original DXBC shaders on
captured packed geometry and pose inputs, alongside the six original
cases. These added cases isolate shader/layout compatibility; they are
not a reconstruction of the new hull's complete triangle buffers. Every
rasterized depth sample receives matching coverage on WARP and NVIDIA
hardware. Across 240,105 vertex comparisons the maximum motion error is
0.001979 input pixels on WARP and 0.001658 on hardware.

A separate controlled NVIDIA DLSS K test at 65% input scale checks a
stationary white hull tip against rolling sky vectors. With correct
surface motion, it does not reproduce the long trails, and adding the
experimental depth-rejection mask makes no measurable difference in the
tail region (maximum 2/255). This does not rule out every disocclusion
case, but does not justify enabling that broader experimental feature
for this report. No live INI setting was changed.

The full absolute-path build and NVIDIA DLL smoke test passed. Visual
confirmation of the newly covered hull/tip surfaces still requires the
next flight; the previous build's working frame vectors were not proof
that these separate surfaces were fixed.

## Follow-up capture, 13:48 flight

Installed `32a6009` is verified. The user reports improvement in
straight flight, with trails still visible during rolls. Captures
`135127` and `135134` show the exterior; `135221` and `135230` show the
cockpit.

- Ruled out: the main hull still lacks exact motion coverage. Exterior
  captures contain 751,445 and 740,890 eligible pixels. All eleven rigid
  cockpit records match; the forward tips now have coverage. Saved
  vectors agree with the exact mesh maps after the raster jitter delta,
  within half-float rounding.
- Confirmed: repeated stripes above the left tip in `135230` appear in
  DLSS output but not its raw input. The tip moves a fraction of a pixel
  while the adjacent sky's roll vectors move 7–9 pixels per frame. Those
  sky vectors lead back into the previous hull silhouette.
- A local NVIDIA preset K replay freezes the captured colour/depth and
  repeats the measured background vectors, with zero motion on the fixed
  geometry. It reproduces the stripes. Rejecting history where the
  reprojected background was occupied removes them; checking the
  surrounding depth footprint also catches filtered edge colour outside
  the exact raster coverage.
- Ruled out: enabling the old reactive mover mask solves this. The
  replay with bias set on these disocclusions is identical to the
  baseline; preset K does not consume that input. Dilating the current
  hull's vectors by one pixel also leaves the stripes.

The correction now rejects hidden background history through an invalid
DLSS lookup, using consecutive per-eye depth and a jitter-corrected
previous-raster coordinate. It checks both sky and distant scenery
behind nearer geometry, with a 3% relative depth margin. UI, source
screens, exact hull and exact hologram motion retain their own paths.
Current colour/depth and physical vectors used by diagnostics are not
changed. The old experimental mover setting remains independent.

The full and foveated DLSS paths retain their previous depth using the
existing texture swap. This adds one R32_FLOAT surface per eye (about 58
MiB for both eyes here), with no additional frame copy or draw replay.
The native TAA path does not enable this DLSS-specific rejection. Reset,
missing-frame, viewport-size and source-screen changes invalidate the
carry before it can be read.

The focused GPU gate passes 54,901 checks. New cases run against the
production source and both shipping MV blobs, covering prior occlusion,
valid sky, finite background, same/nearer surfaces, jitter, region
offsets, missing history, out-of-image motion, both UI kinds, source
screens, exact meshes and holograms. Current depth remains unchanged.

An isolated NVIDIA RTX 5090 benchmark at 2774 by 2740, using the
captured depth/UI/mesh inputs, measures median MV dispatch time
increasing from 0.11485 to 0.14019 ms per eye across 40 interleaved
measurements. This is about 0.051 ms per stereo frame, not a measurement
of the entire live game frame. Captures, benchmark and replay files
remain local under `build/review_motion/sep12/flight1348/`. A flight
must still confirm the visible result during both cockpit rolls and
exterior maneuvers.

The complete absolute-path worktree build, all regression/configuration
gates and the NVIDIA DLL smoke test passed. No live INI change is
needed.

## Follow-up capture, 16:00 flight

Both installed DLLs report `7c6e9ef`. The user confirms that the tip
ghosting is gone, with only occasional flickers during forward thrust.
Captures `160325`, `160342`, `160411` and `160446` show the external
ship over terrain, using OpenVR and DLSS K at 2774 by 2740 input and
4268 by 4216 output per eye.

- Ruled out within the captured windows: a missing frame or a DLSS
  history restart. All four first-eye sequences contain sixteen
  consecutive frames, with `dlHistory=1` and `jumped=0` throughout.
- Ruled out in the four saved coverage snapshots: loss of tracking on
  the main hull. Each has 218 records, 217 rigid and 113 matched, with
  678,354 to 731,545 eligible visible pixels. Small unmatched parts
  remain; this does not establish continuous coverage between dumps.
- No clear broad hull flash appears in the captured sequences. Mean
  brightness over an eroded main-hull mask varies by less than about
  1.5/255 in each sequence. That aggregate check cannot exclude a small
  local patch or an event outside these short windows.

The independent VR log does confirm frame replacement by the transition
flash detector: its final periodic report counts 84 withheld eye-frames,
or 42 stereo frames. Individual eye-submit replacements agree with the
graphics log, including pairs around 16:03:38, 16:03:40, 16:04:13 and
16:04:33. None falls inside the four captured sixteen-frame intervals.
The user confirms that this was normal forward flight without any
low-wake transition, identifying these recurring detections as false
positives. Frame repetition is a candidate for the occasional visible
flicker; its timing still needs correlation with a user-marked event.

The detector repeatedly certifies and later relearns separations near
6,870 and 11,740 world units. Expiry alone cannot explain the 6,872
certification at frame 12971 followed by a new 6,857 detection at frame
13930: only 959 frames elapsed, below the 2,000-frame expiry. The log
also shows the recognised residual increasing from 6,876 to 7,625 over
120 frames after that certification. `recordResidual` moves an entry's
mean on every match, while recognised auxiliary cameras do not advance
the view predictor. A continuously drifting entry could therefore leave
its original magnitude behind. Eviction from the sixteen-slot table is
another possibility. These logs lack the table contents and churn
counters needed to distinguish them; do not increase capacity or relax
rejection on this evidence alone.

The next discriminating capture is the camera-history key (default
Pause), pressed immediately after a visible flicker. It writes the
preceding 1,200 frames, headset poses, withheld-frame attribution and
learning tables, then repeats two seconds later. An eye dump starts a
short forward sequence and can miss an event that prompted the keypress.
Clarify whether the whole view jumps or only a hull surface changes; the
latter also needs raw-versus-DLSS image evidence at the affected
surface. No rendering code, live setting or installed DLL changed in
this review. Analysis artifacts remain under the ignored local
`build/review_motion/sep12/flight1600/` directory.

### Earlier diagnostic validation

The GPU snapshot test and Python reader jointly verify three consecutive
frames, two targets reusing and overwriting the same
pool/palette/instance buffers, per-draw constant changes, exact
serialized payloads after later overwrites, retained input layout and
offsets, target-state preservation, separate frame/eye copies,
same-target deduplication and stopping at the frame limit.

The full absolute-path `build.bat` completed successfully, including the
GPU snapshot round-trip, weapon/UI/night-vision regressions and 252-key
configuration contract. The NVIDIA `smoke.exe` run also passed. The
build and smoke logs are under `build/review_motion/sep12/flight1159/`
(local ignored artifacts).
