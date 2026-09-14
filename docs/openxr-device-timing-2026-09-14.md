# OpenXR device timing and system runtime selection

This extends the [producer timing
checkpoint](openxr-native-timing-2026-09-13.md) with separate measurements of
EDVR work on the XR device. It also makes the direct test install follow
Windows' active OpenXR runtime. The new pair is installed and hash-verified in
Frontier. Its first matching VDXR flight selected the expected runtime and
completed native startup, then Elite requested a zero-sized depth texture and
aborted. The [startup investigation and
correction](openxr-vdxr-startup-2026-09-14.md) record that failure; device
timing still needs a successful manual flight. Desktop validation is recorded
below.

## Measurement contract

`XR COPY/COMPOSE` shows two millisecond values: both consumer eye copies
combined, followed by both eye composition command lists combined. Each copy is
bracketed after its successful keyed-mutex acquisition and before the existing
flush. Each composition interval brackets only immediate-context command-list
execution, after recording and runtime image acquisition/wait and before the
existing flush. Prior-frame retry copies, producer copies, runtime waits,
diagnostics and loading skyboxes are excluded.

Eight timestamp queries share one disjoint scope per measured stereo frame. The
separate device uses a bounded eight-slot ring, reuses completed queries and
polls asynchronously with DONOTFLUSH at actual GPU boundaries. Neither timing
nor readback adds a flush or wait. Reverse eye order is supported. The captured
frame must be accepted by the native submission path before its result can be
published. Query failures, partial frames, ring exhaustion, disabled timing and
stale data remain explicit unavailable states; they are never displayed as zero
GPU time.

The existing `advanced.app_gpu_timing` setting controls both local GPU sources.
CPU wall data, producer GPU data and XR-device GPU data retain independent
validity and age. The private paired-module timing capability is version 2.
Optional capability failure does not block rendering. Borrowed-context mode
does not open an independent disjoint scope inside the game's existing scope;
its XR-device tile reports that a separate device is not in use.

These are elapsed timestamp intervals for EDVR's own commands. They exclude the
runtime's final compositor and must not be added to the producer-device elapsed
span as a total GPU busy time. Exact runtime compositor timing, dropped frames
and reprojection counters remain unavailable without a validated public runtime
capability. No vendor SDK is required.

CPU wait, acceptance, invalidation and close issue no query commands.
Cancellation closes an unfinished scope only at the next actual XR-device
boundary. Permanent device retirement releases abandoned queries without
initiating new graphics work. An uncertain disjoint End is never retried and
disables further query issuance on that collector.

## Runtime selection

A Quest/Virtual Desktop launch exposed an explicit Pimax manifest in the
previous test configuration. The matching native log selected Pimax OpenXR and
returned `XR_ERROR_FORM_FACTOR_UNAVAILABLE` from `xrGetSystem`, with no
submitted frames. This is evidence of the wrong selected runtime, not evidence
that VDXR rendering failed.

The direct installer now defaults to `runtime=system`. The OpenXR loader
resolves Windows' active runtime at startup. An inherited `XR_RUNTIME_JSON`
override is temporarily removed only inside Elite's process, then restored when
the native backend closes, unless another component changed it. Windows
registry settings and persistent environment variables are untouched. A later
game launch follows a newly selected runtime without reinstalling EDVR.
Existing sessions are not migrated between runtimes.

An explicit absolute manifest remains available through `--native-runtime` for
diagnostics. Both routes preserve Frontier's import/export validation and the
paired DLL verification. The default route requires a loader path but no
headset-specific manifest or SDK. The installed loader's location does not
select SteamVR as the runtime.

## Qualification

Desktop gates cover query ownership, asynchronous completion, phase order,
partial frames, query allocation/readback failures, disabled/re-enabled timing,
ring pressure, cancellation, device retirement and real WARP copy/command-list
work. Existing shared-transfer and stereo fixtures exercise the observer
bindings on their actual contexts. Provider fixtures validate ABI fields,
independent sources, stale metadata and retired sequences. Installer/parser
fixtures cover system selection, explicit overrides, dry-run behavior and
environment restoration.

The full build passed with all 501 source hashes unchanged. It includes 83
native timing provider checks, 102 producer GPU checks and 101 new device GPU
checks; shared-transfer, stereo rendering, module startup and shutdown gates
also pass. The new paired DLLs and system-runtime configuration are installed
and verified through the sanctioned installer. Existing game settings and the
original OpenVR DLL are preserved.

After the startup correction, the next game flight should use Windows' selected
OpenXR runtime, keep the main-menu scene steady for about 30 seconds, inspect
`SUBMIT WALL`, `RENDER GPU` and `XR COPY/COMPOSE` in F8, then exit normally. A
matching log must show the expected runtime, valid current-session device GPU
samples, both-eye submission and complete shutdown. Timing overhead,
matched-resolution performance/quality and broader feature parity remain
separate gates.

## Remaining port work

After measurement qualification, compare actual input/output dimensions and
temporal settings against the established path, then address measured
performance or image differences. Remaining submission features include terrain
overscan/cropping, supersample resolve and sharpening, withholding, Explorer
Cam, theater/heal and gaze foveation. Each needs its own
pose/projection/history correctness checks and appropriate in-game transitions.

Qualify VDXR/Quest, the Meta runtime with supported Oculus/Meta headsets,
SteamVR OpenXR and PiOpenXR independently. Follow with release packaging,
runtime selection, recovery and rollback checks. The [legacy Oculus selection
investigation](openxr-oculus-selection-2026-09-14.md) addresses Elite choosing
LibOVR before entering the OpenVR facade. It is an investigation target, not an
implemented suppression policy.
