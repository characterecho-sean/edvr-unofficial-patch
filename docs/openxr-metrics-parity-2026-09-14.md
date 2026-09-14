# Native monitor history and metric coverage

The user has deferred the SteamVR performance comparison and prioritizes metric
coverage, then feature parity. This checkpoint completes the
history/presentation path for the existing native measurements. It does not
change rendering or add a runtime-specific SDK.

## Monitor coverage

| Readout | Native source and behavior |
|---|---|
| Frame rate, period and 1% low | Existing Present interval history; application cadence, not physical headset presentation. |
| Submit wall | Stereo Submit elapsed wall time, averaged over distinct completions in the last 200 ms. Includes waits and producer rendezvous, not exclusive CPU execution. |
| Render GPU | Independent producer-device elapsed span, averaged over the same duration. F8 and the overlay use the same history. Excludes the XR device and final runtime compositor. |
| XR copy / compose | Two separate 200 ms means, each summing its two eyes on the XR device. Two decimal places preserve small valid costs. Never added to the producer span. |
| Graphs | Producer GPU and Submit wall, newest 120 distinct observations from separate bounded histories, oldest first. Labels identify samples, not correlated CPU/GPU frames. |
| XR period | Fresh runtime-predicted period in milliseconds. Graph reference lines and native long-frame detection use this prediction, not an assumed 90 Hz rate. |
| EDVR pass costs and hardware statistics | Existing local pass measurements and independently available hardware samplers remain. |
| Total compositor GPU, compositor drops, reprojection and exclusive app CPU | Remain explicitly unavailable in the current native path. Existing app timings are not substitutes for those sources. |

When the predicted period is unavailable, graphs fit their observed values, use
neutral colors and omit the reference line. Native long-frame detection does
not use the legacy 11.1 ms fallback. A prediction is not a measured
display-refresh rate or proof that a frame missed physical presentation.

## History contract

The menu producer observes CPU-only snapshots on its existing frame boundary.
Fixed 900-entry rings keep CPU, producer GPU and XR-device GPU completions
independent. Re-reading an unchanged snapshot does not add a sample or give it
extra weight. GPU age accumulated before publication is included; a later read
cannot refresh an old completion. Only observations within the requested
averaging window contribute, and no empty window falls back to an older sample.

Invalid, disabled, missing and stale source states clear the affected history.
Observed timing invalidation clears all native histories and the predicted
period. Consumed sequence floors survive stream clearing, preventing old
snapshots from repopulating a history after recovery. New session identity
resets the floors; prior-session samples are rejected. A pending XR-device
result can still resolve to valid on the same unconsumed sequence.

Finite, nonnegative measured zeros remain samples. Missing, future and
overflowing timestamps cannot become fresh. Graph output uses the newest
bounded tail and stops when the current stream becomes stale. Graphs do not
attach a delayed GPU result to the Present or CPU sample that happened to
observe it.

No history operation issues queries, waits, flushes, XR calls, allocations or
file I/O. Bounded diagnostic summaries record sample counts and means every
five seconds, up to 60 records per session; a count of zero explicitly means
unavailable. The original native timing and device-timing records remain
available for measurement-boundary checks.

## Qualification and next gate

The desktop regression checks deduplication, asynchronous ages, independent
means, measured zero, missing/future/invalid data, disabled/re-enabled streams,
recenter/retirement, period replacement, ring wrapping and bounded graph order.
Shared raster-policy checks cover zero versus missing samples,
unavailable-reference scaling and valid-reference colors.

The full build passed with all 505 source hashes unchanged, including 43
history checks, the six shared graph-policy checks, and the existing timing,
transport, startup and shutdown gates. The paired DLLs are installed and
hash-verified in Frontier with game settings and the original OpenVR DLL
preserved. The diagnostic build is `v0.16.2-87-g09b003a-dirty`; exact sources,
binaries and qualification records remain in the local archive. Windows
selected PiOpenXR at installation, while the install continues to use
`runtime=system`. The manual presentation gate remains pending.

For the next manual Frontier flight, use the selected native OpenXR runtime and
open F8's Monitor page after rendering settles. Confirm that the two labelled
graphs update, Submit wall and Render GPU show averages, XR copy/compose shows
separate values, and XR period is labelled as a prediction. If using the small
FPS overlay, its producer average should follow the same trend. Confirm normal
viewing and exit; the matching logs must contain nonzero sample counts for
available sources and complete native teardown.

After this presentation gate, proceed with the remaining submission features:
terrain overscan/cropping, supersample resolve and sharpening, withholding,
Explorer Cam, theater/heal and gaze foveation, with pose/projection/history
checks at each stage. The legacy Oculus-selection investigation remains
separate: Windows runtime selection alone does not suppress Elite's LibOVR
entry path. Matched-resolution performance testing remains deferred by the
user.
