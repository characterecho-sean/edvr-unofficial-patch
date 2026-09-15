# OpenXR submit overlap and recurring measurements

## Baseline and hypotheses

The September 15 Frontier flight loaded `v0.16.2-132-gc5e88dc-dirty` and used
the Windows-selected SteamVR OpenXR runtime at approximately 90 Hz. Sean
reported normal rendering and exit, with a possible roughly 1 ms frame-time
disadvantage against the former OpenVR path at a similar resolution. Similar
resolution and a monitor reading do not establish a controlled regression: the
runtime, actual input/output dimensions, cull guard, scene and timing
boundaries must match.

The matching native log is `edvr_openxr_20260915_063113_245_14688.log`, paired
with `edvr_gfx_20260915_063111.log` in Frontier's `edvr_logs`. The runtime eye
target was 4068 by 4016. The active cull guard widened the game recommendation
to 4857 by 4016 (+19.4% horizontally), and the submitted DLSS input became 3157
by 2610. DLSS was enabled after the original, single 256-pair diagnostic window
had completed.

Across 29 five-second snapshots from 06:32:36 through 06:34:56, the median
logged consumer-copy GPU pair was 0.075 ms and composition GPU pair was 0.084
ms. Their combined narrow spans had a median of 0.158 ms. Submit wall had a
median of 1.378 ms and transfer wall 0.359 ms. These are medians of snapshots,
not per-frame percentiles, and the GPU spans omit the game's producer work and
runtime compositor. The initial detailed window recorded six graphics callbacks
per pair and four cached scene SRVs.

Ruled out: hidden-area masks already reducing work in that flight, because the
runtime supplied zero mask triangles and the modified-frustum path exposed no
mask. The real refresh-rate response and its landable-body LOD/exit fix remain
unchanged; see the [compatibility
qualification](openxr-capabilities-2026-09-15.md).

Three questions motivate this change:

- Can the first Submit return after its producer snapshot without waiting for
  consumer acquisition? The shared-texture fixture must preserve the original
  pixels after the producer overwrites the source, and the hardware comparison
  measures first-eye and pair wall time.
- Can the final scene draw avoid recording and restoring a command list on
  EDVR's separate XR device? The stereo fixture compares pixels, image
  ownership and failure behavior across both paths, and a hardware comparison
  measures CPU submission wall time.
- Is remaining time in producer dispatch, mutex acquisition, swapchain wait, or
  `xrEndFrame`? Recurring windows expose these separately after startup and
  after the user chooses DLSS and a scene.

## Ownership and submission

The producer still snapshots the selected FSS/temporal/sharpen/menu output into
an EDVR-owned shared texture before Submit returns. Its copy, flush and
keyed-mutex publication remain synchronous on the bound render thread. Source
pointers are not retained for later producer work. Each eye has one bounded
pending slot; a second enqueue cannot overwrite an unresolved slot.

The XR owner acquires and copies both pending slots into its private textures
immediately before stereo composition. An incomplete or discarded pair cannot
publish stale pixels. Reuse and shutdown drain pending transfers on the
consumer without invoking a stopped producer. Retaining a prior pair still
swaps completed capture banks without copying them; exchanging banks with
unresolved transfers is rejected. Both source copies remain in place.

Captured scenes draw directly on EDVR's owned immediate context. Every pass
still binds all required state, unbinds its resources and flushes before
releasing the runtime image. GPU retirement is marked before commands are
issued. The borrowed-device path retains command-list execution with state
restoration; the owned mode rejects a graphics provider/executor and
foreign-thread scene calls. Diagnostic triangle and skybox paths retain their
existing behavior.

The producer timing markers remain after Submit. This wave does not reduce the
six graphics callbacks per stereo pair and does not move GPU markers to
manufacture a lower monitor reading.

## Measurement contract

`native_submit_path` identifies deferred consumer capture and direct scene
submission at startup. A `native_submit_window_begin` line arms a bounded
window at startup and, after a completed window, no sooner than 30 seconds from
its start. A window skips 64 eligible consecutive pairs and retains the next
256. Interrupted sequences, input/output dimension changes, selected treatment
bits and feature-epoch changes restart warmup. An unfinished window is never
declared complete merely because time elapsed.

The treatment bits identify FSS healing, a produced temporal output, sharpening
output and menu output. They are not a complete configuration hash or proof of
a stable scene. Keep the scene and settings fixed while measuring, and
correlate the log with actual AA mode and runtime configuration. A begin line
without its completed report is distinguishable from a successful measurement.

Each completed window emits `native_submit_window` and `native_submit_phases`,
including sequence range, input and output sizes, runtime targets, selected
treatments and p50/p95/p99 wall milliseconds. New phase fields are:

| Field | Interval |
|---|---|
| `producer_dispatch` | Copy callback rendezvous, including its work |
| `producer_acquire`, `producer_flush` | Producer keyed-mutex acquisition and explicit flush |
| `consumer_acquire`, `consumer_flush` | Consumer keyed-mutex acquisition and explicit flush |
| `receive` | Both pending consumer transfers, including the private copies |
| `xr_acquire`, `xr_wait` | Runtime image acquisition and waiting for permission to render |
| `xr_draw_submit` | Final scene draw setup, submission and flush |
| `xr_release` | Runtime image release |
| `xr_end_frame` | Session end-frame call |

These intervals are nested CPU wall measurements, not exclusive CPU or GPU
work. Do not sum them or subtract independent percentiles to derive another
percentile. The total Submit boundary stays unchanged. The existing transfer
wall field now covers producer publication; compose wall includes the deferred
consumer receive. Comparing those two nested fields with older builds must
account for the changed boundaries. The separate-device GPU fields still cover
the same private-copy and final-draw commands.

Storage is fixed, sampling adds no GPU waits, and detailed clocks stop after
the window fills until the next window arms. Reports are bounded log records,
with no per-frame file output. There is no new INI setting or hotkey.

## Desktop qualification

The native-host fixture runs the actual treatment/capture route with separate
WARP devices, overwrites the final treatment output after the first Submit
returns, and verifies distinct left/right snapshots after producer admission
closes. Shared-transfer fixtures cover both eye orders, all supported format
families, pending-slot rejection, partial pairs, discarded metadata and
shutdown without producer callbacks. Stereo fixtures cover both rendering paths
across formats, gamma/linear interpretation, crops/flips, asymmetric views and
runtime errors; borrowed-context state preservation remains checked.

The RTX 5090 stereo comparison uses fixed 4068 by 4016 eyes, 32 warmup pairs
and 128 samples per round, in A/B/B/A order. A one-pixel staging readback
completes both draws between samples, outside the timed submission interval.
The median submission results were 0.1212 ms (deferred/restored), 0.0735 ms
(direct), 0.0896 ms (direct), and 0.1108 ms (deferred/restored). The readback
barrier itself had medians of 1.06–1.11 ms. These isolated CPU results support
a small reduction in submission overhead, not a 1 ms GPU saving or a measured
improvement in Elite.

The original per-sample event-query drain spent about four seconds waiting on
this hardware after drawing. Its first run reached the 240-second watchdog; an
instrumented retry confirmed repeated roughly 4,015 ms drains and was stopped.
The transfer harness exhibited the same completion-wait limitation and was also
changed to a readback barrier outside its timed interval. Those aborted runs
provide no timing comparison. This is a desktop benchmark limitation, not
evidence that a game frame or native runtime waits four seconds. Production
completion and shutdown code is unchanged.

The transfer comparison uses the same adapter, dimensions, warmup, sample count
and A/B/B/A ordering. Both paths enqueue the same producer snapshots and
consumer-private copies inside the pair interval; a readback barrier is outside
it. No synthetic game work is inserted between eyes. Results in CPU wall
milliseconds:

| Round | Consumer scheduling | First eye p50 | Pair p50 | Pair p95 | Pair p99 |
|---|---|---:|---:|---:|---:|
| A1 | Immediately after each producer copy | 0.1201 | 0.2250 | 0.2802 | 0.3597 |
| B1 | After both producer copies | 0.0690 | 0.1922 | 0.2428 | 0.3259 |
| B2 | After both producer copies | 0.0706 | 0.1982 | 0.2422 | 0.2664 |
| A2 | Immediately after each producer copy | 0.1202 | 0.2249 | 0.2822 | 0.3390 |

This supports deferring the first consumer wait without simply moving all of
its measured cost onto the second eye in this isolated workload. It does not
establish a game-frame improvement under concurrent Elite GPU work. Do not add
the transfer and draw medians from separate benchmarks to predict an in-game
saving.

The successful records are archived in `build/submit-overlap/benchmark-3.log`
(stereo) and `build/submit-overlap/benchmark-4.log` (transfer). Reproduce the
comparisons with `openxr_stereo_test.exe --benchmark` and
`openxr_shared_texture_test.exe --benchmark`, with Elite closed and a
240-second process timeout. Neither creates an OpenXR session or changes game
files. The WARP correctness fixtures passed 622 native-host checks, 16,284
stereo checks and 361 shared-transfer checks. The RTX 5090 shared-transfer
fixture also passed 361 checks, with no failures.

The absolute-path full `build.bat` passed every gate for
`v0.16.2-134-g7b0859f-dirty`, including the same native/stereo/transfer counts
above, the 255-key configuration contract and installer payload verification.
DLSS 310.7.0 matched the pinned SDK. The final build log, source hashes, binary
hashes and installation report are under
`build/frontier-lod-callers-20260915/attempt-7/`. No C++ source changed after
qualification.

The sanctioned installer installed and independently verified the native pair,
loader and bootstrap configuration in Frontier after a successful dry run. The
live `edvr.ini` was byte-unchanged. The backup receipt is
`edvr_native_receipt.json.pre-openxr-submit-overlap-20260915-20260915-072435.bak`.
The installed runtime SHA-256 is
`7c2c643abbec5050df0f107d3e4704e72a5c060157bf85cfc672c49c172b3c82`; the
graphics DLL is
`a2084bb91dde8030c0be7e3ef5b01ece8bef2e3d86fbb39dc1a72aa8c39b9660`. The build
retains its pre-commit version stamp; use that stamp and these hashes to
identify the next flight. No game or headset session was launched during
qualification.

## Combined Frontier flight

Launch Frontier normally with the Windows-selected runtime. Use a fixed menu or
cockpit scene and unchanged DLSS/render-resolution settings for at least 40
seconds, then check motion, scene geometry and text in both eyes. Check F8 and
the floating GPU/CPU monitor, sharpening, recenter and an Insert eye dump.
Revisit a landable body to confirm LOD still updates, and exit normally. Repeat
the same scene/settings on another runtime when convenient; runtime-specific
performance cannot be inferred from the desktop fixture.

Read the matching native and graphics logs through `tools/edvr_log.py`,
checking the build first. Confirm a completed recurring window, valid GPU
timings, treatment activity and clean shutdown. A correct flight qualifies
integration; a speedup requires a controlled comparison against the baseline at
matching dimensions and settings.
