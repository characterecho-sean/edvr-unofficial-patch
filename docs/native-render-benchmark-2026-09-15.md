# Native render benchmark window

## Status

- **State (2026-09-15):** Implemented and reviewed; full build and all gates
  passed. Matching binaries are installed and verified in Steam and Frontier,
  with user INIs unchanged. Qualification details are below.
- **Open:** Headset validation and hardware scheduling calibration remain. CPU
  and GPU values measure elapsed rendering intervals, not exclusive busy time;
  metadata alone does not establish a matched scene benchmark.
- **Ruled out:** The former render-to-submit interval is unsuitable for this
  native application metric because it includes transfer/handoff gaps. Exact
  tick fixtures now prove those gaps are excluded from the summed segments.
- **Next flight:** Follow "Combined headset check" below; settle the scene,
  open/close F8, and hold each configuration for 40 seconds.

## Measurement and accounting

The native monitor now keeps recurring bounded benchmark windows while native
timing is active. A window warms for two seconds, records individual frame
observations for 30 seconds, then allows a two-second drain for delayed GPU
timestamp completions. It reports p50, p95 and p99 for CPU and GPU separately,
along with valid, invalid and missing counterpart counts. A report is emitted
only once; the next window starts with the next observed scope.

CPU and GPU observations carry independent sequence IDs. CPU publication admits
a frame to the measured window. GPU completion can arrive later or out of order
and is matched by that sequence, including during the drain. Duplicate
observations are ignored. A GPU record that has no CPU-admitted frame is held
in a small bounded orphan table and is excluded if its CPU frame never arrives.
Unresolved counterparts are marked missing only after the drain, so a delayed
query is not mistaken for a failed frame.

The collector retains up to 16,384 frame entries. If bounded storage fills, the
window ends with an overflow status rather than silently dropping measurements.
A completion queue overwrite is reported as transport loss and invalid coverage
so a partial queue cannot be presented as a successful run. A scope change
during warmup restarts warmup. A scope change during sampling or drain emits an
aborted partial report, then the next observation begins a new warmup. Scope
metadata includes the native session, actual per-eye input and output sizes, a
stable refresh-rate value, AA/DLSS mode, runtime and headset labels, and build
string. Timestamps, head poses and frame-to-frame prediction jitter are
excluded from the scope key. The metadata describes the run; it does not detect
or prove a same-scene comparison.

The GPU metric is application render elapsed time from non-overlapping producer
render segments. Its conversion uses the GPU clock frequency, independent of
the headset refresh setting, and excludes XR-device work and runtime compositor
work. The CPU metric is render-thread wall elapsed time from the native timing
producer, with known XR waits and producer interop blocking excluded by that
producer's boundaries. Both are elapsed intervals, not exclusive hardware busy
time. Submit wall time, pose wait, XR transfer, composition and other
diagnostics remain separate and must not be added to either metric or to each
other. Four CPU intervals cover initial game work, first-eye treatment,
between-eye game work and second-eye treatment. GPU intervals cover the same
work, with an empty between-eye interval omitted. GPU endpoints close before
the producer transfer; successful stereo acceptance closes the query scope
without moving those endpoints. Incomplete CPU intervals remain invalid even
when other wall-time diagnostics are available.

The paired timing capability is version 3. It carries actual submitted input
regions, active XR targets, game field of view before temporal jitter,
treatment flags and the host feature epoch with each frame. Resolution, field
of view, treatment and settings changes restart the benchmark scope. Runtime
recommendation sizes are not substituted for actual game input. Reports include
the planned window duration, the actual measured interval and the actual drain
duration; aborted partial windows are not described as completed 30-second
runs.

Desktop checks cover exact GPU tick sums with large artificial handoff delays,
unchanged results when only excluded work grows, independent GPU frequency
conversion, disjoint and incomplete samples, queue ordering, missing data,
bounded capacity, colliding delayed GPU records and final-deadline batching.
These fixtures establish timing boundaries and accounting, not exclusive GPU
execution time or cross-headset hardware calibration.

The floating monitor should show short GPU and CPU labels and `--` when the
corresponding stream has no fresh valid sample. It must not substitute a stale
value or zero, and it must not add CPU and GPU values. Detailed benchmark lines
are log diagnostics rather than a claim of a controlled scene benchmark.

For a useful headset comparison, hold one scene and all settings steady and
record the actual input/output dimensions, runtime, headset, refresh, AA/DLSS
mode and build from the completed report. Matching quality labels or refresh
rates does not establish matching work. GPU elapsed boundaries still require
vendor-neutral GPUView/WPR calibration when hardware scheduling attribution is
needed; the benchmark itself does not claim that calibration has been done.

## Combined headset check

The absolute-path full build passed every gate. Qualification is archived in
`build/frontier-lod-callers-20260915/attempt-12/`, including source and binary
hashes. The tested binaries carry the precommit stamp
`v0.16.2-135-gd0971fd-dirty`. The native timing contract passed 911 checks,
real-query GPU integration 150, history/benchmark 70, shared timer lifecycle
701, span policy 2,804, overlay 77, native host 622 and native stereo 16,284.
Installer payload and configuration checks also passed. The legacy diagnostic
timer retains its prior render-to-submit definition; only explicitly tagged
native application segments enter this benchmark.

Use the matching qualified DLLs on each install. Confirm that the floating
readout and F8 show both application timings, that submit wall remains
separate, and that changing OpenXR resolution does not resize or clip the
floating card. Check AA off and the usual DLSS mode, text-size changes, menu
open/close and a normal game exit. GPU timing disabled should hide GPU
measurements while CPU measurements continue.

For each benchmark, settle in the chosen scene, open and close F8 to restart
the scope, then hold the scene and settings for at least 40 seconds. Look for a
completed `native benchmark:` report in the graphics log under `edvr_logs`,
plus its matching `native benchmark workload:` line. Check both
valid/invalid/missing counts and actual dimensions before comparing
percentiles. After changing resolution or AA, hold steady for another 40
seconds; the changed scope should produce a new window. Compare headsets only
with the same build, scene, AA settings, input/output dimensions and comparable
field of view. Record runtime and refresh differences instead of treating them
as matched workloads. Headset readability and hardware timing calibration
remain unqualified until those checks are performed.
