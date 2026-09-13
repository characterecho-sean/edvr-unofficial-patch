# Reusable OpenXR host and callback-held teardown

This checkpoint prepares the validated native runtime for game startup
integration. The previous [Present-hook
gate](openxr-present-boundary-2026-09-13.md) passed on PiOpenXR as `20cad71`,
including normal tracking and shutdown. Its stop handshake depended on the
diagnostic controlling the application's entire Present loop. Frontier owns
that loop, so closing a callback lease alone cannot supply the same exclusion
during session destruction.

## Reusable host

`src/openxr/native_runtime_host.h` now owns `NativeRuntimeHost`, its dispatch
table and enumeration helpers, and `RuntimeOptions` containing the explicitly
selected loader and graphics-provider paths. `native_device.h`,
`render_route.h` and `shutdown_trace.h` contain the shared device validation,
owner/render rendezvous and tracing. The old diagnostic device header forwards
to the shared header so existing fixtures keep compiling.

The host was moved with its startup geometry, owned historical interfaces,
frame/capture/loading policies, recenter logic, counters and cleanup intact.
Parent comparison against `a2e7b3b` verified the class body apart from the type
name and the narrowed options type, and the moved device class apart from
newline representation. The diagnostic includes the host header first to check
that it supplies its own dependencies. It retains the CLI, the test scene,
Present driver, lifecycle coordinator and diagnostic failure policy.

The embedding host must still choose trusted absolute module paths, provide the
validated game device, preserve exclusive context use on the recorded render
caller, construct the runtime host on its XR owner, and retain all
objects/modules until explicit shutdown completes. Moving the class does not
select a shipping backend or connect the game's exports to it.

## Teardown inside the callback

`shutdownAtRenderBoundary` marshals a foreign shutdown request through the
existing bounded Present queue. Once the final callback is entered, it closes
the render dispatcher and joins the XR owner with its resource finalizer. The
render caller remains inside that callback throughout session destruction. The
real DXGI Present and existing graphics-proxy frame work have already finished,
and the game cannot execute its next frame on that caller until the callback
returns. A caller already on the recorded render thread executes directly
instead of waiting for its own next Present.

This keeps the callback lease and user state alive during teardown. The
consumer releases them after the callback unwinds; `close` and `release` still
return `E_PENDING` while it is active. The diagnostic no longer sends a pause
request to its outer Present loop. Further Presents after completed runtime
cleanup are permitted. This differs from the previously qualified loop-owned
stop, so its native result must be measured separately.

GPU completion remains a prerequisite. If the request times out while queued,
its captures are retired without starting cleanup; an entered finalizer must
finish before its caller returns. The diagnostic retains uncertain resources
until process exit on failure to enter the teardown boundary. This is not a
production recovery policy.

The finalizer must not request further render-dispatcher work or depend on the
parked caller dispatching window messages. No nested game message pump is
introduced: arbitrary message handlers could render while teardown owns the
context. The native test must establish progress on the selected runtime; a
passing WARP fixture does not establish that property for PiOpenXR or for
Frontier's window behavior. Other game threads using the immediate context
would also require explicit exclusion; this contract relies on the measured
single render caller.

The new success signature is
`present_teardown,in_callback=1,owner_completed=1,callback_retired=1,loop_pause_requested=0`,
together with successful teardown stage endings, normal native cleanup and both
native PASS markers. The previous `quiesced` signature describes the earlier
loop-owned stop and is not reused for this contract.

## Validation and remaining integration

The actual-DLL WARP fixture now enters shutdown through a real Present
callback, executes representative GPU clear/copy/readback work on the XR owner
while the render caller is paused, and checks that callback release remains
pending until unwind. A CPU fixture covers a missing Present cancelling before
resource retirement, and shutdown directly on the bound render caller without
self-deadlock. The earlier loop-owned quiescence fixture remains in the suite.
Luna supplied the initial extraction and WARP test; parent review completed the
host move, restored the original formatting, added the shared shutdown helper
and timeout fixture, and integrated the diagnostic.

The full absolute-path `build.bat` passed with exit 0. It passed 101 actual-DLL
Present checks, 67 CPU queue checks, 100 render-dispatcher checks, 504
proxy-state checks, 10,774 stereo checks, 35 native desktop checks, 15
existing-device checks and 29 runner checks. The 80,493 weapon-motion and 1,242
mesh-motion checks and 254-key configuration contract also passed. The output
is `build/openxr-shared-host-build.log`; exact hashes are recorded in
`build/openxr-shared-host-validation.json`.

The `3fcadf7` 20-second PiOpenXR headset gate passed on September 13 after
explicit user readiness. The child ran from 15:26:59.171428 to 15:27:19.950254
UTC, exited 0 in 20.765 seconds and did not trigger its 60-second watchdog. The
user confirmed that the grid and triangle appeared normally in both eyes,
stayed fixed in space during head movement, had normal color and clarity, and
closed without becoming headlocked.

The run completed 268 loading projections, 1,529 stereo pairs and 3,058 eye
copies. All 1,530 sampled views were valid. The graphics proxy recorded 6,652
private command lists against 6,652 expected, with zero unknown lists and zero
wrong-thread callbacks. The runtime logged normal stop and cleanup, both native
PASS markers and the new `in_callback=1` teardown signature. All 19 shutdown
stage occurrences completed successfully. Session binding shutdown took 15 ms
on the XR owner while the render caller remained inside the callback; callback
close and release completed afterward. No outer-loop pause was requested.

The distinct callers were Init 32924, System 33644, XR owner 9360 and render
19936. The runtime was Pimax OpenXR 0.1.0 with 5424 x 5356 recommended pixels
per eye. No game, SteamVR or native diagnostic process was observed in the
preflight and postflight snapshots; transient processes between those snapshots
were not monitored. Source, binary, environment and build-log hashes matched
all 457 recorded entries before and after the run. The executable SHA256 was
`8ab2d3571c052a722afbb6031979eede57631bf3930838e1ab2baa9784575d38` and the
graphics DLL SHA256 was
`91b2ba9c30dc6a621faa399f8e3f4a6fe823add6611ec8db63cfb48f714e3fa0`. Exact
binaries, output, receipt, validation and qualification are archived locally
under `build/openxr-native-20260913-092659/`.

This qualifies teardown inside the real Present callback in the standalone
PiOpenXR diagnostic, including progress without pumping messages on the parked
render caller. Frontier's window behavior and context ownership still require
game integration evidence. No game files, live INI or saved runtime selection
changed; the Frontier install remains `f3c205e`.

The startup audit also identified explicit remaining work: connect native
export discovery to the shared host and a trusted paired-device registration;
handle absent render progress during Init/Shutdown; and make bridge capability
independent of optional feature installation. The current graphics owner is
registered only after vScreen's ExecuteCommandList hook is committed. If that
installation is disabled or skipped, native startup must report unavailable
instead of silently accepting an unregistered provider. Complete legacy export
policy, game feature integration and a native Frontier flight remain open.
