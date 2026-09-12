# DLSS performance review — 2026-09-10

Implemented subsequently in the
[comms and optimization update](review-comms-optimization-2026-09-10.md), with
integration tests and a fresh benchmark. The findings below record the original review.

The largest opportunities are in EDVR's motion preparation and diagnostics.
The user confirmed build `6AA3235C` now looks correct, including the station,
ship yaw and targeting chevrons. This review preserves that quality baseline.
No production shader, DLL, render setting or installed file was changed for
the review. Shader experiments are isolated in `build/review_motion/dlss_perf`.

## Measured baseline

The latest completed flight, `edvr_gfx_20260910_154417.log`, reports 90,395 eye
evaluations, eight history resets, **1.82 ms per eye inside NVIDIA's evaluation**
and **2.22 ms per eye for EDVR's enclosing temporal pass**. The last reported
interval was 90 frames/second. These are cumulative averages across both
3523×3478 input (80%) and, from 15:54:27, 2862×2826 input (65%). Both produce
4404×4348 per eye. They are not measurements of one steady scene at 65%.

The enclosing GPU timer excludes earlier UI/smoke coverage draws and CPU object
tracking. Its 209.93 ms session maximum is not a representative steady-state
cost; this review did not isolate its cause. CPU and GPU savings below must not
be added into a claimed frame-rate improvement.

## 1. Stop producing stepped motion that rendering does not consume

**Highest-confidence CPU opportunity: approximately 0.52 ms per scene frame,
plus unmeasured copies and readback overhead.**

In `src/d3d11/temporal_pass.cpp:1851`, `kSteppedStampOn` is permanently false.
It prevents the stepped parts from being applied to the body grid at line 4114.
Nevertheless, `src/d3d11/object_probe.cpp:2896` calls `trackFrame` for every
completed pool readback. The last two flight reporting intervals measure
0.52 ms/frame inside this tracker. Its two full-pool CPU copies at lines
2367–2370 happen **after** that timer. The render path also constructs the
unused table of twelve motion transforms at `temporal_pass.cpp:4053`.

`objectProbeFrameBoundary` still issues a pool copy every frame at
`object_probe.cpp:3163`. The working rigid-body estimator needs only the two
designated samples in each eight-frame period, with their existing two-frame
separation and alternating starting phase. Outside diagnostic captures,
gating the unused tracker would also permit 75% fewer pool readback copies.

Use one explicit need for stepped tracking/full-rate diagnostics, shared by
producer and consumer. Keep the rigid-body pairs, publication timing, camera
data and body grid intact. Preserve or deliberately arm/warm up the rolling
four-frame dump and eye ledger when captures need every frame. Simply turning
off `advanced.object_probe` is insufficient: `fix.temporal_aa_objects=on`
keeps `g_on` true at line 2985. Turning off object motion would discard the
station fix and is not the proposed optimization.

## 2. Share depth samples within each motion-vector workgroup

**Best measured GPU candidate: about 0.41 ms per stereo frame at 65%, including
the diagnostic specialization in finding 3. Integration remains to be tested.**

The 3×3 loop in `temporal_pass.cpp:686` reads the same neighborhood repeatedly.
Each sample merges scene, smoke and private UI depth. An 8×8 workgroup asks for
576 neighborhood positions, although its complete neighborhood contains only
100 distinct positions. Texture caches reduce physical traffic, but the shader
still performs the repeated loads and merges.

A benchmark-only variant loads a 10×10 tile into group-shared memory once,
then uses it for both the center depth and the 3×3 dilation. It preserves the
distinction between undilated output depth and dilated motion selection.
All four image outputs—motion vectors, depth, bias mask and UI evidence—were
bit-for-bit identical to the current shader for both tested input cases.

Median GPU milliseconds per eye on the RTX 5090:

| Motion-vector kernel variant | 65% input | 80% input |
| --- | ---: | ---: |
| Current shader | 0.31357 | 0.45203 |
| Registration search removed | 0.27933 | 0.41654 |
| All diagnostic instrumentation removed | 0.27168 | 0.40800 |
| Diagnostics removed + shared depth tile | 0.10634 | 0.15750 |
| Combined saving, two eyes | **0.41446** | **0.58906** |

Before shipping, exercise separate smoke/UI depth inputs, packed-eye offsets,
partial workgroups, mover/ship paths, and the existing UI/camera regressions.
Every thread must reach the tile-loading barrier, including threads outside
the final image bounds. These timings are isolated kernel measurements, not
an end-to-end flight speedup or proof across all scenes.

## 3. Compile a normal-play shader without registration diagnostics

`temporal_pass.cpp:4716` enables `probeNv` whenever history and diagnostic
resources exist; it does not require a diagnostic request. The sparse search
at line 821 can perform 81 candidate comparisons of 25 luma samples per
selected position. Per-group statistics also use atomics and barriers, with
statistics cleared and read back each eye.

Removing instrumentation alone saved **0.042 ms/eye at 65%** in the benchmark.
Shader reflection reports temporary-register count falling from 30 to 18.
A separate compiled variant permits this reduction; setting `probe.y=0`
at runtime does not provide the same compile-time simplification. Keep the
instrumented variant available for explicit diagnostics and eye captures.
Sample routine timing/statistics rather than collecting every eye when the
numbers are not being investigated. The savings in finding 2 already include
this change; do not count it twice.

## 4. Allocate native-TAA fallback buffers only when needed

`temporal_pass.cpp:3499–3517` allocates `outTex` and both native `hist` textures
before choosing the DLSS path. Successful full-frame DLSS does not use those
images for reconstruction. With the preferred four-byte R10G10B10A2 history
format and four-byte output, these three textures occupy approximately
**185 MiB across both eyes at 65%**, or **280 MiB at 80%**. The float16 history
fallback would use more.

Allocate these on entry to native TAA or an actual NVIDIA fallback. Separate
input dimensions/reset bookkeeping from ownership of native resources:
currently `releaseOwned` and `e.w/e.h` couple those responsibilities. Test
DLSS failure, live AA switches, format changes and resizing. This primarily
reduces VRAM pressure and allocation cost; no steady-state FPS gain is claimed.

## 5. Reduce report-only work in the object-pair worker

`object_probe.cpp:1242–1264` constructs fresh vectors and hash maps per pair.
The `nowHashCount` map serves duplicate reporting, while byte histograms and
alternate-pose comparisons around lines 1305–1329 continue even without
verbose reporting. Gate report-only calculations and reuse storage capacity.

The flight's roughly 12–14 ms pair-diff measurements belong to a background
worker, not a 12–14 ms render-thread stall. Essential identity matching and
rigid clustering must remain. Measure worker turnaround and motion age before
changing its cadence; improving these can matter independently of FPS.

## Copies and synchronization: lower priority

The normal full-frame path makes one input color copy and one output submit
copy. The isolated measurements were 0.02064/0.04218 ms per eye at 65%, and
0.02768/0.04163 ms at 80%. Their combined stereo cost is about 0.13–0.14 ms.
The extra input copy is conditional on the source lacking a usable SRV, not
the normal path. The output copy at `temporal_pass.cpp:4881` preserves the
game's typeless resource/color-space contract; handing the typed NVIDIA output
directly to the compositor previously changed brightness. Removing either
copy needs format and appearance validation and has less measured upside
than the depth kernel.

Profile private UI depth seeding separately because it occurs outside the
temporal timer. Removing it naively would compromise the smoke/UI behavior
just fixed. A cheaper clear-and-merge scheme would require preserving the
original occlusion behavior across each UI shader family.

The ordinary GPU query rings already poll with `DONOTFLUSH`, and staging maps
use `DO_NOT_WAIT`. I did not find a busy-wait in the normal rendering path.
NVIDIA features are retained per eye; the flight does not show continual
feature recreation or history resets. Existing texture reuse and depth/UI
history swapping are worth preserving.

## Benchmark evidence and limits

Artifacts: `build/review_motion/dlss_perf/prepare.py`, `bench.cpp`, `compile.bat`,
and `155720/tiled-results.txt` / `152024/tiled-results.txt`. The recorded source
is copied beside each case. Compile with the batch file from the repository
root, prepare either timestamp with the script, and run `../bench.exe` from
that case directory. Preparation requires the original Steam eye captures.

The harness uses production `D3DCompile` flags (zero), checks all four output
textures, warms up each variant five times, and records twenty GPU timestamp
samples per variant in rotating order. Timestamp-disjoint samples are rejected.
Its blocking readbacks/flushes belong only to this standalone harness.

Inputs use recorded depth, UI masks and transforms. Raw color exists only for
the central 1400×1400 crop; elsewhere the harness uses the downsampled treated
overview. Previous UI evidence is approximated, the body grid is uniformly
occupied within the recorded box, and already-merged depth is supplied through
one layer. Movers and ship transforms are inactive. The 80% case is an earlier
capture with a different scene/build; its absolute time must not be attributed
solely to resolution. Exact output matches therefore establish equivalence for
these cases, not complete replay fidelity or full integration correctness.

Recommended implementation order: gate unused stepped tracking and its
readbacks; integrate the tiled normal-play motion shader with capture-driven
diagnostics; defer native fallback allocations; then reduce worker bookkeeping.
Keep render scale, model K, sharpening and the successful motion/UI rules
unchanged while measuring each change in flight.
