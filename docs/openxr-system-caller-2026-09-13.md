# Native application System caller

The [minimal graphics transport](openxr-transport-hooks-2026-09-13.md) passed
its Pimax headset gate. The [Frontier semantic
census](openxr-semantic-flight-2026-09-12.md) records separate Init, later
System and render callers, with shutdown on the later System caller. Projection
queries occur on two game threads, and the first recommended-size response
precedes the first compositor wait by about 2.62 seconds. The preceding native
DLL diagnostic made most System calls on its render caller and created a new
thread just for shutdown.

## Diagnostic contract

The next diagnostic gives application System work a persistent caller, created
before the separate Init caller. The render caller continues to own the D3D11
device, context, scene rendering and Present. The native module retains its
separate XR owner. Geometry and properties are copied reads; native tracking,
recenter and event operations continue through the existing XR-owner service.

Startup System queries run before the application's first WaitGetPoses. They
check recommended size, rigid eye transforms and both eyes' projections for the
three observed near/far pairs: `0.025 .. 50000`, `0.1 .. 1000` and `1 ..
50000`. They also query seated absolute tracking and runtime-backed model and
tracking-system strings. Display frequency may explicitly report unavailable;
it is never supplied as a made-up device value. Tracking may be invalid before
focus without failing startup geometry.

The focused grid's recenter and reset-event poll use the System caller.
Geometry, seated tracking and event polls then run at a 250 ms cadence while
focused, between completed scene pairs. Each request finishes while the render
caller pumps Present; there is no new asynchronous scene scheduler or mid-pair
recenter. Projection queries from scene rendering still exercise the render
caller. Shutdown uses the same System caller, while the render caller continues
Present until shutdown returns. The System worker joins before the scene eye
resources and device are destroyed.

Accepted work must finish before the caller returns or releases its captures. A
failed render pump is reported, but does not abandon an active callback. Only
the existing isolated diagnostic watchdog can terminate a stuck test process;
there is no new production timeout or thread detachment.

The diagnostic logs real Windows thread IDs for correlation with module startup
and shutdown. Its pass predicate requires distinct Init, System, render and XR
owner IDs, with the module's shutdown ID matching the System caller. It also
requires completed application waits and valid periodic samples, alongside the
existing stereo-copy, cleanup and callback-retirement checks. A flushed
application-wait marker follows the startup-query record. This does not claim
that no OpenXR wait occurred during the module's earlier zero-layer bootstrap.

Luna implemented the caller and shared query helpers. Parent review removed an
early timeout that could outlive borrowed callback captures, corrected the
observed projection ranges and thread labels, and strengthened validation for
rigid transforms, clip-plane mapping, metadata buffers and tracking data.
Production runtime and graphics code are unchanged in this checkpoint.

## Qualification

The existing module self-test modes now exercise the shared observer through
the independent System ABI fixture. That fixture supplies 640x480 geometry, an
initialized invalid pose and unavailable metadata, so these tests do not
pretend that a native runtime succeeded. Fixed vectors check clip-plane
mapping, wrong planes, zero/nonfinite matrices, reflections, scale and invalid
tracking data. Worker checks cover persistent identity, rejected work,
exceptions, callback-capture release before return, continued pumping after
failure and orderly join.

The absolute-path full `build.bat` passed with exit 0. The explicit module mode
passed 201 checks and bootstrap mode passed 207, with no OpenXR runtime loaded.
The existing suites also passed, including 168 Present checks, 184 checks per
minimal shared/private/LiveCopy case, 25 per deliberate-probe case, 100 render
dispatcher checks, 67 queue checks, 504 proxy-state checks, 10,774 stereo
checks and the 254-key configuration contract. Independent read-only review
found no remaining correctness issue in the frozen caller gate.

All 467 source hashes were unchanged during the full build. The record in
`build/openxr-system-caller-validation.json` contains 483 hashes covering those
inputs, 11 binaries, the build log, loader/runtime manifest and staged files.
The build log is `build/openxr-system-caller-build.log`. Native module SHA256
is `01ed7f73adb9ef94bc90f0c7faf25fe3b421e8c762ab250212f05b3497026cec`, module
fixture is `c3a44506677c4b5f899742a7496d705a065f859e8488a8ab683b2508101534e6`,
and graphics DLL is
`4d2fde5d72bc7339bba5501836b11a838f977117a025440441ac8ff414ed3d20`.

## Passed Pimax gate

The `ec3dd92` 20-second bootstrap run passed on Pimax OpenXR 0.1.0, Crystal
Super and RTX 5090, with parallel projection enabled and 5424x5356 native
per-eye geometry. The minimal-LiveCopy stage was
`build/openxr-system-caller-native-stage-20260913/`; its graphics log confirms
the minimal transport installation. All 483 recorded hashes matched before and
after the run. The DLL's precommit stamp is `v0.16.2-65-g27227dd-dirty`; the
source and binary hashes qualify the tested `ec3dd92` checkpoint.

The child exited 0 after 21.235 seconds. It completed 1,527 stereo pairs, 3,054
eye copies, 315 loading projections and 68 valid periodic System samples, with
zero invalid scene poses. Startup geometry, metadata and the seated pose were
available before the first application wait. Init ran on thread 40872, System
on 37788, render on 9992 and the XR owner on 38288. Both shutdown records
identify System thread 37788. All nine shutdown stages completed; session
binding teardown spanned 15 ms on the coarse tick clock, the owner joined and
the final callback retired without a retained generation.

The user confirmed the grid and triangle in both eyes, upright and fixed in
space during head movement, and normal closure without headlock. Focus was
already true at the first sample; the grid began at 500 ms and the scene at
3500 ms. Display frequency's explicit unavailable result is expected and does
not substitute a made-up refresh rate. The scene shaders and dark-grey clear
are unchanged.

The receipt, exact binaries, stage INI, preflight/postflight snapshots,
qualification record, graphics log and matching Pimax client/server evidence
are archived in `build/openxr-native-20260913-125915/`. No excluded process was
observed before or after; transient processes were not monitored. The runtime
override affected only the diagnostic child.

This diagnostic does not establish Frontier's own shutdown progress, or cover
arbitrary concurrent System calls during a partial stereo pair or shutdown.
Complete legacy export policy, game feature integration and a native Frontier
flight remain separate work. No game installation or persistent setting is
changed.
