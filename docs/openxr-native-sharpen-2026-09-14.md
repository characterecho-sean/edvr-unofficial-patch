# Native render sharpening and shared log directory

The metrics presentation flight has passed. This checkpoint restores
`fix.render_sharpness` on native OpenXR using the same RCAS pass, strength
mapping and default-off behavior as the existing OpenVR submission path. The
user has excluded features listed under `[experimental]` from parity work;
supersample resolve, FSS theater and gaze foveation are deferred. Existing
settings and the temporal behavior already implemented are preserved.

## Submission contract

The native host acquires a private sharpening capability from the paired
graphics DLL on its bound game producer. The provider validates device
identity, thread, sequence, eye, texture shape and bounds before calling the
existing shader. It latches the current strength on the first valid eye of each
sequence so a live setting change cannot split a stereo pair. Reversed eye
order is supported, duplicate or retired sequences are rejected, and invalid
inputs cannot advance the setting latch.

The shader operates on the temporal result when available, otherwise on the
original submitted eye. It runs before menu composition and shared capture,
preserves the selected color-space metadata and full-span flip directions, and
does not change the projection selected for the temporal result. In particular,
a spatially sharpened raw image still uses the projection with which that image
was rendered. The output has an owned reference until capture completes. The
source image is never written.

Zero strength passes through without dispatching the shader. A shader refusal
passes through and stands sharpening down for the session; contract or
producer-dispatch failures reject the submission. Bounded engagement/failure
records and final counters distinguish disabled, active and refused operation.
Invalidation preserves the sequence floor; close is CPU-only and does not
require a producer callback. A dedicated native availability flag prevents the
legacy missing-compositor warning without enabling the legacy withholding
consumer.

The existing pass-cost measurement remains available. The producer GPU span and
Submit wall include this work; the separately named temporal, menu and
XR-device measurements keep their existing boundaries. This checkpoint makes no
performance-parity claim.

The user also reported missing CPU/GPU values in the floating monitor. Its
native branch omitted CPU timing entirely and called GPU timing only
"producer". At the user's request, the corrected readout uses the concise
labels `gpu` and `cpu`. These show producer GPU time and CPU submit wall from
the same independent 200 ms averages as F8. Either source can independently
display unavailable; measured zero remains a valid value. CPU submit wall
includes blocking and producer rendezvous, not exclusive CPU execution or the
game's full CPU frame time.

## Log location

At the user's request, native trace files now go into `edvr_logs` beside the
process executable. The path is independent of the DLL location and working
directory. Scene initialization creates the directory if necessary; discovery
probes do not open a log. A failure to create the destination leaves durable
tracing unavailable without falling back beside the DLL. Existing logs are
retained in their original location.

New names use local time, matching the legacy session-grouping convention:
`edvr_openxr_YYYYMMDD_HHMMSS_mmm_pid.log`. The trace body still records UTC and
thread/process IDs. Native files written by previous builds beside the DLL had
UTC filenames. The 8 MiB trace budget, unique creation, concurrent reader
sharing and stdout diagnostics are unchanged.

`tools/edvr_log.py --tag openxr` discovers the new files; `--tag all` also
searches the native directory when legacy logging uses a custom `log.dir`. The
support-bundle timestamp parser recognizes the native suffix, so the default
shared log directory includes matching graphics and native logs in the same
session.

## Qualification

Provider tests exercise both eyes over consecutive sequences, mid-pair changes,
clamping and disabled settings, malformed input, wrong device/thread, owned
output references, flips, invalidation, retirement and shader refusal. The
real-shader fixture compares native output against the existing direct shader
export on a WARP device, checks that sharpening changes the input, and verifies
source immutability, bounds, dimensions and caller bindings. Both enabled and
disabled configurations run outside the game.

Trace checks cover path construction and actual file creation beside the test
executable. Python discovery checks cover native names, milliseconds, version
extraction and redirected legacy directories. The installer fixture checks that
the native log is bundled with the matching graphics session.

The final full build passed with all 511 source hashes unchanged. This includes
108 sharpening provider/client checks, 13 trace checks, 43 history checks and
the existing transport, startup, menu, timing and shutdown gates. The actual
WARP shader fixture passed 58 checks enabled and 22 disabled. An initial build
exposed a timing-test sleep that did not reliably cross the collector's clock
deadline; the test now waits for that deadline, and all 101 device-timing
checks pass without a production timing change.

The paired `v0.16.2-88-gbe44d38-dirty` DLLs are installed and hash-verified in
Frontier. The game INI and original OpenVR DLL were preserved, and startup
remains `runtime=system`. Exact sources, binaries, build and shader-test logs,
and installer verification are archived locally under
`build/openxr-native-sharpen-20260914/`. The manual gate remains pending.

The next manual Frontier gate is normal rendering/menu behavior, checking the
floating `gpu`/`cpu` values, changing F8's Sharpening control from zero to a
visible strength and back, and normal exit. Both-eye engagement and clean
teardown must be visible in the matching logs under `edvr_logs`.
