# Hangar DLSS performance review

Verified Steam gfx/VR run 20260913_100913 uses 2aa0c2d. Eye 101455 is
the cockpit in Macleod Market's enclosed hangar. Input is 2774 x 2740,
output 4268 x 4216 per eye, DLSS preset K, Valve SteamVR at 90 Hz. Local
hardware benchmarks use the RTX 5090. Do not attribute these timings to
the earlier supporter's 4080 Super / Quest 3 report.

Before the capture, the 10:14:14, 10:14:34 and 10:14:54 intervals report
73, 70 and 78 fps. Session GPU averages are 1.91 ms/eye for temporal
work and 1.57 ms/eye for NVIDIA evaluation. They include preceding
flight; they are not an isolated hangar benchmark. The sampled door
readings near this period are 3.74--4.34 ms stereo. Earlier draw-time
work lies outside that bracket.

The hangar is not a small draw workload. Census 2 has 1595 original eye
draws per game frame, plus 557 offscreen draws in its first frame. The
eye-mesh snapshot has 823 watched draws in that frame, covering
3,482,790 submitted indices including instances. The motion table
reaches its 512-record eye limit: 504 valid, 122 matched. Its raster
coverage occupies 76.28% of the eye; UI covers 3.79%. No terrain records
exist. The 13 holo records contain twelve cockpit records (mode 0) and
one screen record (mode 3), with zero opaque-planet records (mode 4).
The recent non-landable-planet correction contributes no coverage in
this capture.

The latest two mesh-report intervals estimate 0.84 and 0.97 ms/stereo
frame in extra coverage, using interval differences of cumulative sample
totals and draw counts. Matching adds about 0.044 ms/stereo frame. These
are sampled estimates, not complete-pass GPU timestamps. Capture batches
and UI source/depth work are additional. Do not add overlapping brackets
or equate CPU busy time with measured EDVR CPU cost.

The dump itself creates thousands of snapshot buffers. Its 63 ms frame
and capture-related copies are excluded from steady-state evidence.

## Candidates and discriminators

- NVIDIA reconstruction: output-size-dependent work remains in an
  enclosed room. Existing timestamps distinguish it from wrapper work;
  no resolution or preset reduction is authorized as a
  quality-preserving optimization.
- UI resolve: benchmark the production shader with captured UI, edits
  and motion, preserving every submitted pixel and influence-history
  byte.
- Mesh coverage: benchmark complete capture/reissue/matching, including
  occluded geometry and CPU submission. Preserve visible motion,
  matching rules, draw ordering and source-write boundaries.
- Hook overhead: count actual calls before changing state-query or
  per-draw validation. A synthetic maximum-cap benchmark is not proof of
  the cost in this hangar.

## UI-resolve experiment

Ruled out: sharing the 3 x 3 UI/edits neighbourhood necessarily improves
this kernel. At the captured resolution, a 10 x 10 shared tile costs
0.1535 ms/eye versus 0.1442 for production. Replacing integer ownership
division with a corrected float quotient also gives 0.1529 ms/eye. Both
variants preserve submitted colour/alpha and influence history exactly
at jitter 0 and +/-0.49, but are slower and remain scratch experiments.

The NVIDIA benchmark rotates variants for 21 samples, 12 dispatches per
sample after warmup, rejecting disjoint timestamps. It uses production
compiler flags. UI, edits and motion are captured; raw colour outside
the captured central crop is derived from the treated overview, and
previous influence is approximated by current UI coverage. Thus output
equality is established for those inputs, not every possible flight.
Files are under build/review_motion/sep13/hangar-perf; no game assets
are committed.

## Mesh replay experiment

An early-depth-stencil coverage variant passes 83,694 checks in the
original-vertex-shader replay. Across 48,781 visible vertex comparisons,
the largest reprojection difference is 0.000759 input pixels. Every
original raster depth sample receives the same coverage.

Ruled out: this replay establishes a useful early-depth speedup. Its two
timed mesh windows contain only 0--9 visible pixels, and the timing
differences have no consistent direction. For example, at 512 repeated
draws the first window measures 0.380 ms/eye for production and 0.351
for early depth; at 128 draws it measures 0.123 and 0.161 respectively.
These are repeated bounded geometry windows, not a replay of all visible
hangar geometry or its occluders. Early depth remains unproven on the
large visible meshes, whose complete geometry is not in these fixtures.
No production shader is changed on this evidence.

## Remaining costs and recommendation

The existing station batching optimization is present. It shares
unchanged instance snapshots and transform dispatches, checks the
512-record cap before most draw-state queries, and flushes at resource
write boundaries. The earlier several-millisecond per-draw capture
regression has not returned in this build. Preserve those boundaries:
removing them would reuse transforms from the wrong draw.

The NVIDIA feature is retained per eye, keyed by input/output dimensions
and preset generation. Low-resolution motion vectors are enabled. The
normal DLSS path uses the shader without registration diagnostics; full
capture diagnostics are armed explicitly. Normal query/readback paths
are bounded and nonblocking. The recorded history-reset count stays at
40 across the three slow reporting intervals. There is no evidence here
of continual DLSS recreation or repeated history resets causing the
sustained hangar slowdown.

The 4268 x 4216 output represents about 36 million pixels for two eyes.
NVIDIA reconstruction still processes those images in an enclosed
hangar. Its roughly 3.14 ms stereo session average consumes about 28% of
an 11.11 ms / 90 Hz frame budget before other work. This comparison does
not make the cumulative average an exact hangar-only measurement.

Extra mesh coverage is the largest remaining measured EDVR draw-time
candidate. The interval ending 10:14:38 adds about 393 coverage draws
per game frame. Avoiding those reissues while retaining exact visible
motion is more promising than saving a few microseconds in UI resolve.
One architectural candidate is emitting coverage during the original
opaque material draw. Unlike the existing terrain path, these captured
mesh draws already have material pixel shaders, so this requires
preserving their outputs and adding coverage safely; it is not a
depth-only shader substitution. It needs a representative whole-pass
benchmark, material/state restoration checks, and visual equivalence
before a speedup can be claimed. The estimated 0.8--1.0 ms is the
current coverage cost, not an achievable saving or a prediction of 90
fps.

The remaining bottleneck cannot be assigned entirely to DLSS from an eye
dump. Normal-play depth censuses vary from 8,075 draws at 10:14:26 to
3,103 at 10:14:46. The selected long-frame reports show about 1.8--2.2
ms of sampled draw-hook CPU time, but lack a settled compositor GPU
record; they do not establish typical CPU cost or a GPU-only limit. A
steady-view CPU/GPU timeline would distinguish submission pressure from
shading/reconstruction before investing in the larger rewrite.

This pass changes only this investigation note. The unsuccessful shader
experiments remain local, and the working installed DLL, preset, render
resolution and image-quality behavior are retained. There is no new
performance build or verified frame-rate improvement to install.
