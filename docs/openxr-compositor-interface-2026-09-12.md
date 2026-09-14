# Owned OpenVR compositor interface checkpoint

This checkpoint connects an owned `IVRCompositor_014` object to the standalone
PiOpenXR diagnostic. It follows the successful copied-eye headset test. Work
stays on `codex/openxr-port`; Frontier retains the tested `f3c205e` paired
proxies. The new object is not advertised by shipping exports and is not a
complete game compositor.

## ABI and pose contract

`openvr_compositor.*` implements the exact 29 virtual methods from the pinned
Valve v0.9.20 declarations. Both desktop and native callers obtain the base
pointer through a separate translation unit with link-time optimization
disabled, so the tests exercise ordinary historical virtual calls. There is no
raw fabricated vtable or import of a modern interface layout.

WaitGetPoses initializes both caller arrays, admits one source wait, then
returns independent render and gameplay HMD predictions. Counts from zero to
the pinned 16-device limit are accepted; null is permitted only for a zero
count. An oversized request is rejected before waiting and initializes at most
the first 16 elements. Non-HMD entries are disconnected, invalid, zero-velocity
identities. Invalid HMD data has the same deterministic shape with the actual
connection state. Invalid source poses never expose an unspecified matrix or
velocity as valid tracking.

GetLastPoses and the single-device getter copy one published snapshot without
waiting or locating again. The single-device getter permits either output to be
null; a valid but unavailable device returns an invalid pose, while an index
outside the historical limit returns IndexOutOfRange. Cache snapshots carry
runtime generation, frame sequence, selected origin and origin generation.
Stale frames or origins cannot replace a newer publication, and retirement
removes the connected state. An admitted failed wait invalidates its pose
cache; a rejected busy/stale operation cannot invalidate somebody else's frame.
The publication lock only covers short copies and transitions, so cached reads
remain available during a blocked runtime wait.

The native host obtains the render HMD pose and optional velocity in the same
xrLocateSpace call used with that frame's eye geometry, at
predictedDisplayTime. The gameplay HMD prediction uses predictedDisplayTime
plus the runtime's predictedDisplayPeriod. This interprets the historical
header's "two frames out" as one additional frame beyond the render prediction,
consistent with the clarification in [Valve's current compositor
comments](https://raw.githubusercontent.com/ValveSoftware/openvr/master/headers/openvr.h).
The current header supplies semantic context only; the binary ABI remains
pinned. No refresh interval or gameplay pose is synthesized from the render
pose. An unrepresentable timestamp or XR_ERROR_TIME_INVALID leaves the gameplay
sample independently invalid. Other unexpected locate results stop the
diagnostic conservatively.

Linear and angular velocity validity are independent. The conversion preserves
their base-space coordinates and units; unavailable fields are ignored and
reported as zero. This follows [Khronos's XrSpaceVelocity
contract](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrSpaceVelocity.html).
Malformed valid pose/velocity data and damaged output chains are rejected
without partially publishing the destination. The optional velocity chain does
not add a second render-time head location query.

## Submission, lifetime and unavailable methods

Submit forwards the historical eye, texture, bounds and flags through the
existing validated private capture and frame boundary. Bad eyes return
IndexOutOfRange; duplicate submissions retain the previous checkpoint's
InvalidTexture policy. The second distinct eye composes and ends the frame.
PostPresentHandoff is an acknowledged no-op after the completed pair. Clear
closes pending diagnostic work but reports its missing historical grid
semantics through the first-use diagnostic; it is not a complete
ClearLastSubmittedFrame implementation.

The run loop releases its event/lifecycle operation lease before calling the
compositor. Each handle-using callback acquires its own lease and checks the
established owner thread and generation. Cached readers need no runtime lease.
Shutdown blocks new operations, waits for the outstanding lease to leave,
retires both publications, then destroys the owned resources. The host still
must outlive its source/facade and any operation. This does not add thread
marshalling or cancellation of a blocked xrWaitFrame.

Only the existing LOCAL/seated origin is available in the native host.
Unsupported origin changes do not alter the reported origin. The cache policy
supports invalidation after a real origin replacement, but standing/raw spaces,
recenter and reference-space events still need implementation. Focus state
comes from the OpenXR session lifecycle; it does not invent a scene-process ID.

Frame timing returns false and leaves the complete caller buffer untouched.
Skybox installation returns an error. Fades, grid, compositor-window/process
controls, mirror operations, dump images, reprojection policy and suspension
remain unavailable. Their historical void/boolean/scalar shapes use no-op or
neutral returns and a once-per-object, once-per-slot diagnostic. These policies
make missing functionality explicit inside the diagnostic; they are not
sufficient for shipping discovery or Frontier startup compatibility.

## Review and validation

Luna drafted the facade and timed-pose helper. Parent review implemented the
publication and native host connection, corrected bad-eye errors and caller
buffer initialization, hardened invalid cached-pose handling, and replaced or
expanded the fixtures. A separate read-only review found no concrete defects in
lease use, frame closure, publication retirement or native counter paths.

Desktop coverage includes all 29 virtual slots, independently sized pose arrays
and guard elements, null/oversized requests without a wait, distinct
render/game caches, stale-generation and stale-origin rejection, failed waits,
all single-device index boundaries, exact submission arguments, unchanged
timing buffers, once-only missing-method diagnostics, and cached reads during a
blocked source wait. Pose tests check a known rigid transform, independent
velocity validity, ignored unavailable data, atomic malformed-output rejection,
typed locate chains, exact timestamps and prediction overflow. Binding tests
check that velocity comes from the same head query and that failed queries
leave both outputs unchanged.

The full absolute-path build passed with Frontier's original OpenVR DLL and the
252-key config contract. It includes 201 compositor checks, 173 timed-pose
checks and 470 binding checks, plus the existing 3470 stereo, 136 capture, 87
frame-boundary, 18 lifetime, 130 System and 24 native desktop checks. The new
tools run their no-write dry-run and self-test modes in the build. The exact
source and executable hashes are retained in
`build/openxr-compositor-validation.json` with the full-build log.

The `10eb85d` compositor diagnostic passed its 20-second PiOpenXR run with the
executable, sources, loader and runtime manifest matched to the full-build
record. Frontier and SteamVR were absent before and after. Pimax OpenXR 0.1.0
reported D3D11.1 and two 5424 x 5356 sRGB eye swapchains. The child exited 0
after approximately 20.39 seconds, without the watchdog firing.

Startup geometry was published on frame 1 before stereo, and the historical
System query remained valid. The compositor's initial render and gameplay
timestamps differed by exactly the reported 11,111,128 ns display period; both
poses were valid. All 1800 waits passed the cached-pose equality check, and all
1800 gameplay predictions were valid. There were 3596 Submit calls and private
captures, 1798 stereo pairs and handoffs, two zero-layer frames, 1799 valid
view/head samples, and no invalid view sample. Normal session stop and resource
cleanup succeeded. These counters establish execution of the historical
compositor path; they are not a game performance measurement.

The user confirmed both eyes, world stability during head movement, normal
color and clarity, and normal visible closure. This completes the native
functional gate for this standalone compositor checkpoint. The exact receipt
and output are retained under `build/openxr-native-20260912-160707`, with the
validation record updated to include the matching counters and confirmation.

## Remaining integration

After this gate, remaining work includes game runtime/export ownership, startup
origin/recenter/events and required compositor functionality, the paired
device/feature handshake, and preserving D3D11 state and EDVR's binding shadow
in a production composition pass. The diagnostic still draws its own scene on
its own context; no Elite frame has passed through this native path. Game image
quality, timing/performance, focus/loss and broader runtime coverage remain
separate qualification gates. No new config key or live installation change is
included.
