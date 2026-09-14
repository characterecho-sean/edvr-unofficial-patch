# Native timing checkpoint

The first native DLSS comparison used unsupported monitor fields and different
render resolutions. This checkpoint connects native frame boundaries to the
existing producer GPU instrument and adds CPU wall measurements. It measures
the path before attempting a performance change. Matched-resolution performance
and image-quality parity remain unqualified.

## Measurement boundaries

The graphics provider issues a process-wide sequence for each native pose wait.
The wait and subsequent complete stereo pair share that sequence. CPU samples
contain owner-thread wall elapsed time for the pose-wait body, each Submit
body, temporal treatment, menu treatment, shared capture/transfer, and
composition. Producer rendezvous and driver/runtime waits inside those bodies
are included. Queueing before entry to the owner body is excluded. The extra
timing-marker rendezvous after Submit is excluded from the Submit wall
measurement.

Temporal, menu, transfer and composition are nested inside Submit; adding them
to Submit would double-count elapsed time. They are not exclusive CPU work or
GPU measurements. Pose wait is separate and is never subtracted from a Present
period measured on another thread. The runtime's predicted display period is
identified as a prediction, not a physical refresh measurement.

The local GPU span starts at the first covered command on the game's canonical
immediate context after a successful pose wait. Per-eye markers begin before
native temporal processing. End markers run on the same producer after native
Submit and its producer copies return. The second accepted eye closes the outer
span. Reverse eye order is supported. Existing pass timers share the production
frequency scope; query readback remains asynchronous and uses DONOTFLUSH.

This is an elapsed timestamp span on the producer device. It can include idle
gaps between queued work. It excludes GPU work on the separate OpenXR device,
including that device's consumer copy and swapchain composition. It is neither
total GPU busy time nor compositor GPU time. No independent device spans are
added together. The transfer/composition wall fields help identify where to
investigate, but cannot establish those operations' GPU cost.

## Ownership and failure behavior

The optional versioned capability validates the provider module, device,
generation and producer thread. Its stable context pool is bounded at 16
acquisitions per process. Exhaustion or an unavailable provider disables this
instrument without blocking rendering. Device/module lifetime remains borrowed
through explicit close, as with the other native capabilities.

Pose wait, CPU publication, invalidation and close issue no graphics commands
or graphics rendezvous. GPU markers run only inside admitted producer
callbacks. Marker failures do not change a Submit result. The existing
`advanced.app_gpu_timing` switch controls the GPU instrument independently of
the CPU wall measurements and legacy compositor timing switch.

Only complete successful stereo pairs publish CPU samples. Partial pairs,
failed waits, reset/clear, stopping and close invalidate affected measurements.
A successful publication remains available while the next frame is in progress;
the next wait's metadata does not overwrite it. New sessions reject old GPU
sequences. CPU and GPU samples are aged independently and are unavailable after
two seconds. No missing, invalid, disabled or stale measurement becomes zero.

## Monitor and evidence

Native mode shows `SUBMIT WALL` and `RENDER GPU`, alongside Present cadence,
existing filter measurements and independent hardware statistics. The GPU
caption's detail identifies a producer span and excludes the XR device. The
wall detail spells out the individual phase totals. Unsupported compositor GPU
time, drops and reprojection remain unavailable; the legacy graphs are not
populated with native values from a different source.

The native trace distinguishes provider acquisition from unavailability. The
graphics log periodically records `native timing CPU` with the sequence, wait,
Submit and phase totals. `Render-to-submit GPU` retains the original frame,
sequence, validity and age. That GPU record alone is not evidence of full
native GPU cost; its device boundary is defined above.

The full build passed with all 496 source hashes unchanged, including 60 native
timing contract checks and 102 production GPU checks. The verified DLL pair is
installed in Frontier with current settings preserved. Contract fixtures cover
malformed inputs, incomplete pairs, independent CPU/GPU validity and context
retirement. The production fixture uses actual WARP queries and copies through
the native client and owner/producer dispatcher, including both eye orders,
nested pass timers, disabled GPU timing and CPU-only close. Synthetic durations
are not a game performance benchmark. Actual game call ordering and monitor
presentation remain pending the manual check below.

## Next manual Frontier check

After the verified pair is installed, launch Frontier manually with the native
Pimax runtime and SteamVR closed. Keep the main-menu scene and DLSS settings
steady for about 30 seconds. Open F8 and inspect the Submit wall and render GPU
readouts, then exit normally. Record actual input/output dimensions from the
matching build before choosing a SteamVR comparison. Matching quality-slider
labels across runtimes does not establish matching resolution.

The next log review must confirm valid current-session CPU and producer GPU
samples, both-eye temporal processing and complete shutdown. Unsupported
compositor fields should stay unavailable. This gate qualifies the instrument's
game integration and monitor presentation; performance, measurement overhead,
full feature parity and other runtimes remain separate checks.
