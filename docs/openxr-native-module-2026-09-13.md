# Separate native runtime DLL and application fixture

The [startup-discovery checkpoint](openxr-native-discovery-2026-09-13.md)
passed its `b8db929` PiOpenXR headset gate. This checkpoint moves the native
backend behind a separately loaded DLL and exercises it from an application
that uses the historical OpenVR ABI. The full desktop build passed. Its first
native DLL headset gate failed visual inspection despite passing counters: the
user saw a grey void without the loading grid, followed by a triangle at the
end. After correcting the fixture's viewing timer, the `7ec4f85` focused native
DLL run passed: the user confirmed the grid and triangle in both eyes, normal
tracking, color and clarity, and clean closure without headlock.

## Module boundary

`build/edvr_openxr_runtime.dll` owns the runtime lifecycle, native host, XR
owner, render dispatcher and graphics callback binding. Its export table
contains the five names imported by Frontier: `VR_InitInternal`,
`VR_ShutdownInternal`, `VR_GetGenericInterface`, `VR_IsInterfaceVersionValid`
and `VR_GetInitToken`. The two additional exports are explicit embedding APIs:
`edvrConfigureNativeRuntime` and `edvrGetNativeRuntimeStatus`.

Configuration runs once outside DllMain, copies trusted absolute
loader/graphics paths, validates its versioned structure and pins the DLL for
process lifetime. It creates no runtime or device. The graphics DLL must
already be loaded and have published the application's device and hook owners.
The first actual Present callback establishes render readiness before the
module starts OpenXR. There is no fallback device or runtime selection change.

Each initialization attempt owns a separate generation. Clean failed startup
can retry; repeated successful Init preserves the token and interface identity.
Retired facade addresses remain allocated for the pinned module lifetime, with
a maximum of 16 attempted generations. A failed cleanup blocks restart. Callers
must stop using returned interfaces at Shutdown, as required by the existing
lifecycle contract.

`NativeRenderBinding` now accepts optional frame work after its queued work.
The module uses that callback to deliver loading frames through ordinary
application Present calls. Closing the binding prevents further frame work.
Exceptions close admission and report callback failure while allowing the
graphics proxy to preserve the application's Present result.

Shutdown stops loading admission, drains submitted GPU work and runs the native
finalizer while the render caller is held inside the final callback. The owner
joins before callback/device references retire. If cleanup cannot complete
safely, the module preserves the remaining generation and prevents another
Init. Entered runtime or driver calls still require completion; this is not a
bounded production shutdown guarantee. The diagnostic watchdog terminates only
its own child. Retained-generation recovery, driver hangs and failure after a
partially running native session remain unqualified.

## Application fixture

`openxr_module_test.exe` dynamically loads the DLL and resolves its exports. It
does not link the private native host. It creates the application D3D11 device
through the graphics proxy before foreign-thread Init, while the main caller
continues real Present calls. The returned System and Compositor interfaces
provide all poses, projection matrices and submission calls used by the scene.

The fixture recenters, supplies six colored grid faces through
`SetSkyboxOverride`, releases the source faces after the owned capture and
continues Present calls for the loading phase. It then clears the skybox and
renders a world-fixed RGB triangle into application-owned eye textures, capped
at 2048 pixels on their longest side. It checks cached render/game poses field
by field and alternates Submit eye order. Its System queries and scene
rendering share the render caller; this does not qualify all Frontier caller
combinations or game image quality.

Foreign-thread `VR_ShutdownInternal` closes the session while the application
continues Present progress. Unlike the preceding diagnostic's requested-exit
loop, this fixture explicitly calls Shutdown after the scene interval. Final
status requires successful cleanup, interface retirement, token invalidation,
matching frame/pair/copy totals, loading projections and zero wrong-thread
graphics calls. Its success marker is `native_module: PASS`.

The Python runner selects this executable only when `--runtime-module` is
supplied with both `--graphics-proxy` and `--present-boundary`. It records the
module path and hash with the executable, graphics proxy, loader and runtime
manifest. The PiOpenXR override remains confined to the child environment. The
existing diagnostic command remains available without the new option.

## Desktop validation

Luna implemented the initial application scene, runner extension and callback
tests. Parent review corrected homogeneous clip coordinates, viewport/depth
state, callback test ordering and thread assertions, the runner's default
executable directory, pose comparison and test cleanup. The application scene
is verified by WARP pixel readback in both eyes through an independent exported
System fixture.

The absolute-path full `build.bat` passed with exit 0. Its 48 new module checks
cover the actual DLL exports, invalid configuration, unsupported
application/interface behavior, absent graphics publication, cancelled startup
without Present, retry after clean failed initialization and scene pixels. The
negative loader is deliberately absent, so these tests create no OpenXR
runtime. The suite also passed 168 actual-DLL Present/discovery checks, 67
queue checks, 100 render-dispatcher checks, 504 proxy-state checks, 10,774
stereo checks, 35 existing native desktop checks, 15 existing-device checks and
36 runner checks. The 80,493 weapon-motion checks, 1,242 mesh-motion checks and
254-key configuration contract passed.

PE inspection confirmed exactly seven named, non-forwarded module exports. The
application executable has only D3DCompiler, User32 and Kernel32 imports; it
loads the graphics and native modules explicitly. The actual 20-second runner
command passed its no-write dry-run and selected `openxr_module_test.exe`, the
staged DLL and a 60-second child watchdog.

Full build output is `build/openxr-native-module-build.log`.
`build/openxr-native-module-validation.json` records 454 source inputs,
including module-definition files, 11 binaries, the loader and runtime
manifest, and the build log: 468 hashes in total. The runtime DLL SHA256 is
`1714b19b67d6d47402ec150ec2f348ae8371085ac68435ef6976ffa6e69b1bf9`, the
application executable is
`4345845c2f8a3a158ae2e768e995b1675cc77578f465c8f5b56254580271359b`, and the
graphics proxy is
`a07edbf940d97f440c8fa3fdbc950c9beed6cd0e97ad8dcd04e183a022f2152f`.

## First native run and visual failure

The `0424065` native DLL run on September 13 used Pimax OpenXR 0.1.0 and the
recorded binaries. The child ran from 16:31:38.417116 to 16:31:59.788671 UTC,
exited 0 in 21.375 seconds, and did not trigger its 60-second watchdog. It
reported 39 loading projections, 1,041 stereo pairs, 2,082 eye copies, 1,041
matching pose-cache checks and zero invalid render poses. All nine logged host
shutdown stages completed successfully; owner join, final callback entry,
callback retirement and cleanup were reported. No excluded game, SteamVR or
native diagnostic process appeared in either process snapshot, and all 468
recorded hashes matched before and after.

The user reported: "No grid, just a grey void. A triangle appeared at the end".
This fails the visual gate; `native_module: PASS` establishes only the
implemented technical checks. Exact binaries, the receipt, output, process
snapshots and failed qualification are archived locally in
`build/openxr-native-20260913-103138/`.

Ruled out: absence of any loading/stereo work, because 39 loading projections
and 1,041 composed pairs were recorded. Ruled out: a stale test binary or
different runtime manifest, because all recorded hashes matched. The log does
not establish the actual submitted pixel contents, their delivery time in the
headset, or visibility/focus throughout the run. Those remain separate
hypotheses; no transport or image-quality correction is justified by these
counters alone.

Pimax's matching runtime-server log supplies a timing discriminator absent from
the application summary. The test was active by 10:31:39.326 local time, but
the server stayed near 12.857 FPS with stale frames through 10:31:48.193. It
logged `standby mode leave` at 10:31:48.516, screen-on at 10:31:48.874, normal
90 FPS rendering by 10:31:51.226 and user-eye detection at 10:31:51.302. The
three-second grid timer had already expired. The preceding successful
diagnostic's matching log showed normal 90 FPS delivery. These excerpts and
both native client logs are preserved in the failed-run archive. This supports
premature viewing-phase expiry during headset wake as the leading explanation;
it does not independently prove the pixel contents or clear the failed visual
gate.

## Viewing readiness correction

The revised application fixture spends its viewing budget only while
`CanRenderScene()` reports runtime focus. It continues loading Present calls
while waiting, requires 500 ms of continuous focus before beginning the grid
interval, and recenters at that transition with a checked reset event. Losing
focus during the grid restarts readiness and replays the full grid interval.
Losing focus during the triangle pauses the scene budget while continuing frame
progress with empty submissions. A 20-second request retains three seconds of
grid and seventeen seconds of scene viewing.

The CPU clock handles a zero starting timestamp, short runs, interrupted
readiness, delayed wake and resumed scene viewing. It fails distinctly on a
30-second startup deadline, clock reversal or the fixed overall viewing
deadline of requested duration plus 30 seconds. The runner's existing child
watchdog remains an additional bound. Phase and focus transitions, recenter
success and first stereo pair now carry elapsed timestamps. This prevents
logical readiness delays from silently consuming the observation period;
runtime focus is not independent proof that the headset display is physically
awake. Any recurrence of late display output requires correlating these
timestamps with the Pimax server log.

Luna supplied the initial timing helper. Parent review corrected paused-time
accounting, zero-timestamp handling, short-duration budgets and explicit
timeout failure, integrated the loop and added 18 deterministic timing checks.
The full absolute-path build passed with exit 0 and 66 module checks, including
the original 48 ABI/scene checks. All 168 Present/discovery, 67 queue, 100
render-dispatcher, 504 proxy-state, 10,774 stereo, 35 native desktop, 15
device, 36 runner, 80,493 weapon-motion and 1,242 mesh-motion checks passed, as
did the 254-key configuration contract.

The new build log is `build/openxr-native-focus-build.log`.
`build/openxr-native-focus-validation.json` records 455 source inputs, 11
binaries, two environment files and the build log: 469 hashes. The revised
application executable SHA256 is
`3659cd5319e8d897bd4f0930c01ef7f14f9376820093ce5ac90ec69b7225c238`, runtime DLL
is `a819088fad5911474bd7121401d2aa2c429dfc5b49a9c158f655b046184d6213`, and
graphics proxy is
`cf688cb6ff5f1205c574c67dc1e4ff6ca922bc6a42919a340985077f018d21c7`. The
failed-run archive retains the preceding exact binaries and evidence.

## Passed focused native DLL run

The `7ec4f85` repeat on September 13 used Pimax OpenXR 0.1.0, a Pimax Crystal
Super and an RTX 5090, with runtime eye targets of 5424 by 5356 pixels. The
child ran from 16:58:27.629930 to 16:58:48.854748 UTC, exited 0 in 21.219
seconds and did not trigger its 60-second watchdog. Runtime focus was true at
the first sample. The grid and checked recenter began at 500 ms, the triangle
at 3,500 ms, and viewing completed at 20,500 ms. The matching Pimax server
excerpt showed normal delivery near 90 FPS throughout viewing, without a
standby transition.

The module reported 314 loading projections, 1,530 stereo pairs, 3,060 eye
copies, 1,530 matching pose-cache checks, 1,850 callbacks and zero invalid
render poses. All nine host shutdown stages completed successfully. Session
binding shutdown took 16 ms inside the final callback; owner join, callback
retirement and cleanup completed, with no retained generation. Init, render, XR
owner and Shutdown used distinct callers.

All 469 recorded hashes matched before and after the run. Neither process
snapshot contained an excluded game, SteamVR or native diagnostic process;
these snapshots do not monitor transient processes. Exact binaries, the
receipt, output, snapshots, validation, qualification and matching Pimax logs
are archived locally in `build/openxr-native-20260913-105827/`.

The user answered "Yes—all looked normal" to the full visual check: grid
followed by triangle in both eyes, upright and fixed in space during head
movement, normal color and clarity, and normal closure without headlock. This
passes the focused native DLL startup, viewing, submission and callback-held
cleanup gate. Focus was already true at startup, so delayed physical wake and
focus-loss replay remain desktop-tested only; this run does not establish their
behavior on hardware.

## Remaining integration

This DLL is deliberately not named `openvr_api.dll` and is absent from
installer payload selection. It is not the complete 14-name legacy export
replacement; unresolved legacy factory signatures must not be guessed. Native
backend selection, complete export policy, independent graphics-hook
availability, game Init/Shutdown render-progress evidence, feature integration
and a native Frontier flight remain open. No Frontier files, live configuration
or saved runtime selection changed. The Frontier install remains `f3c205e`, and
shipping OpenVR discovery continues to forward.
