# OpenXR seated origin and reset events

This checkpoint adds application-owned seated recentering to the standalone
native diagnostic after the successful `10eb85d` compositor flight. Work
remains on `codex/openxr-port`; Frontier retains the tested `f3c205e` proxy
pair. Shipping exports and game configuration do not change.

## Origin ownership and pose consistency

The pinned historical ResetSeatedZeroPose contract sets the current HMD
position and yaw as seated zero while retaining world-up. The new
`seated_origin.h` helper projects HMD forward onto the horizontal plane and
constructs a yaw-only quaternion with the full XYZ position. Missing validity,
malformed finite/unit pose data, and a nearly vertical forward vector reject
the reset without publishing a guessed heading. The singularity threshold is a
squared horizontal length of 1e-12; it is not a tracking filter or dead band.

`seated_space.h` initially borrows the binding's natural LOCAL space. A reset
creates a new LOCAL reference space with the computed pose as its origin
offset. This changes the application's coordinate system, not Pimax's saved
runtime origin. The host first closes any incomplete frame, then replaces the
space under its runtime operation lease. A later reset creates the replacement
before destroying the old owned space. Frame geometry, render/gameplay HMD
locations, absolute-pose queries and the submitted projection layer all use the
selected seated space. The natural LOCAL space remains available only as the
base for computing the next reset.

Only the specified successful creation results transfer a child handle. Pending
loss and other runtime failures stop use; failure outputs never become spaces.
An uncertain destruction is not retried. Any surviving child is implicitly
destroyed when the owner destroys the session, as required by
[xrDestroySession](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrDestroySession.html).
Shutdown explicitly disposes known owned children before the binding's borrowed
spaces and session. The component does not run XR calls in its destructor or
marshal threads; its caller must exclude in-flight users before replacement and
teardown.

A successful reset increments the compositor's origin generation even when the
selected OpenVR origin enum remains seated. Both pose predictions and System
geometry become unavailable until a new frame is published. An old origin
candidate cannot refill the compositor cache; a repeated old frame cannot
refill System geometry. No previous eye copy is submitted after reset without a
new wait and a new pair.

## Runtime-origin changes and event policy

`reference_changes.h` receives LOCAL change notifications through the existing
session event sink. It keeps a bounded, sorted queue and advances its epoch
only when the frame's predicted display time reaches each changeTime. Equal
timestamps remain distinct events, and late notifications apply on the next
advance. Invalid relevant events, overflow and backward render time retire the
policy rather than silently losing a discontinuity. Foreign-session and other
reference-space notifications do not consume its capacity.

The runtime changes its natural origin at the event's specified time. This
checkpoint invalidates application pose/geometry history then; it does not
apply a second compensation transform. A gameplay prediction that would cross a
pending origin change remains independently invalid for that frame. The policy
never advances to gameplay time, which could otherwise put the next render
prediction behind its watermark. These decisions follow the timing and
undefined-pose rules in
[XrEventDataReferenceSpaceChangePending](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrEventDataReferenceSpaceChangePending.html).
Native runtime changes still require a separate headset qualification; desktop
injection is not evidence of an observed Pimax recenter event.

`reset_events.h` queues the application's own successful seated resets as the
historical SeatedZeroPoseReset event, with bResetBySystemMenu=false and an
event-time HMD sample obtained in the new space. Polling dequeues once, reports
bounded nonnegative age, and does not locate or advance a frame. A changed
origin invalidates an older event's pose while preserving the event itself.
Unavailable standing/raw origins receive an invalid event pose. A full queue
rejects another reset before the space changes. The System facade's existing
buffer-capacity checks run before dequeueing.

No unknown numeric event from the Frontier census is reinterpreted here.
Focus/quit/runtime-reset event translation, standing/raw spaces and
seated/raw-to-standing transforms remain separate work. The host does not
terminate the game or alter a global runtime setting.

## Review and test gate

Luna implemented the yaw helper and reference-change policy. Parent review
connected space ownership, cache invalidation, reset events and the native
calls, expanded the independent pose oracle, and replaced weak event fixtures.
The retained tests use counted checks with strict self-test and no-write
dry-run modes. Fixtures cover repeated replacement, failure outputs, partial
success, both possible outcomes of uncertain destruction, stale publication and
event generations, queue bounds/FIFO, prediction discontinuities, and
selected-space consistency between eye and head queries.

The full absolute-path build passed with Frontier's original OpenVR DLL and the
252-key config contract. Its gates include 131 seated-origin, 120
reference-change, 166 ownership/event/publication and 476 binding checks,
alongside the existing compositor, native and rendering regressions. Exact
source and executable hashes are retained in
`build/openxr-origin-validation.json` with the build log.

The revised native run exercises ResetSeatedZeroPose twice through the
historical System virtual interface before displaying its scene. Each reset
must invalidate old cache data, produce exactly one reset event with a valid
event-time pose, and locate the HMD within 1 cm and 0.01 radians of the new
position/yaw zero at the reset timestamp. Pitch and roll are retained, so a
full identity matrix is not required. These are diagnostic acceptance bounds,
not corrections applied to rendered poses. After the second reset, bootstrap
must republish valid geometry before stereo submissions begin. The existing
compositor wait/cache/Submit/handoff and shutdown gates remain required.

The `66def0b` diagnostic passed its 20-second PiOpenXR run with its executable,
sources, loader and runtime manifest matched to that build record. Frontier and
SteamVR were absent before and after. Pimax OpenXR 0.1.0 reported D3D11.1 and
two 5424 x 5356 sRGB eye swapchains. The child exited 0 after approximately
20.38 seconds, without the watchdog firing.

Both resets passed the position/yaw bounds at their own timestamps, advanced
the compositor origin generation to 2 then 3, invalidated the cache and
produced exactly one reset event each. Startup geometry became ready on frame 3
with no preceding stereo submission. The run completed 1801 wait/cache
comparisons, 1801 valid gameplay poses, 3594 Submit calls and private copies,
1797 stereo pairs and handoffs, and four zero-layer frames. It recorded 1800
valid view/head samples, no invalid views, normal session stop and successful
resource cleanup. The initial gameplay prediction remained one reported display
period beyond render time.

The user confirmed the triangle in front of them in both eyes, world-up and
stability during head movement, normal color/clarity and normal closure. This
completes the standalone application-reset gate. No runtime-origin change
notification occurred in this run, so the separate natural-origin-change policy
retains desktop-only qualification. Exact receipts remain under
`build/openxr-native-20260912-163128`, with counters and confirmation in the
validation record.

Game runtime/export ownership, persistent lifecycle pumping, required startup
compositor functionality, the paired feature handshake and a production pass
that preserves EDVR's graphics state remain incomplete. This run did not
install or test a native Frontier backend.
