# External-camera hull motion, September 12

The user confirmed the weapon/night-vision update and then reported hull
blurring and shimmering after taking off in the external camera. This
investigation confirms a large error in the DLSS motion supplied for
that hull. The change in this commit extends the explicit eye capture;
it does not claim to correct the rendering yet.

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

## Validation

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
