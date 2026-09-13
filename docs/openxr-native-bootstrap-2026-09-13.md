# Native startup through the application's Init export

Main through `0d7251b` was merged into the port branch as `9f26dd4`. The full
merge build passed, including 66 native-module checks, 168 Present/discovery
checks, 10,774 stereo checks and the 254-key configuration contract. Conflict
resolution retained the port's call census and GPU boundary alongside main's
graphics updates and removal of the early VR handover. The merge log is
`build/openxr-main-sync-20260913-build.log`.

This follow-up lets the staged native DLL configure itself when the application
calls `VR_InitInternal`, using two explicit paths supplied by its launcher. The
application no longer needs to call the embedding configuration API in this
mode. This advances the startup contract; the DLL remains
`edvr_openxr_runtime.dll`, outside installer payload selection, and is not a
complete Frontier replacement.

## Configuration contract

Before the first Scene Init, the launcher supplies `EDVR_OPENXR_LOADER` and
`EDVR_OPENXR_GRAPHICS` in the child process environment. Both are trusted
drive-absolute paths. The graphics proxy must already be loaded, have published
the application's device and committed the required context hooks, and continue
delivering real Present callbacks while Init and Shutdown progress.

The collector reads only those two values, caps each at 32,767 characters,
rejects partial, empty, relative, UNC, malformed, oversized and embedded-NUL
inputs, and rejects changed lengths between the sizing and value reads. It
preserves path spelling; existing graphics discovery validates the loaded
provider's canonical path and export identities. The launcher must establish
both values before creating the child; this is not an atomic configuration
protocol for concurrent environment writers.

With neither variable supplied, Init returns `VRInitError_Init_NotInitialized`.
Invalid configuration returns `VRInitError_Init_InstallationCorrupt` without
publishing a module or initialization generation. Unsupported application types
are rejected before reading configuration. A valid first Scene Init uses the
existing configuration validator, pins the module, and enters the existing
lifecycle coordinator. The first published configuration wins; subsequent
environment changes do not alter it. Concurrent initialization still uses the
coordinator's Retry/Shutdown rules. Explicit embedding configuration remains
available and takes precedence once published.

This path uses the existing five-second render-readiness wait and the existing
native startup/shutdown ownership. It does not add a fallback runtime or relax
device identity, graphics admission, failure retention or callback-held
teardown. Entered driver/runtime calls still require completion; the isolated
diagnostic watchdog is not a bounded production shutdown guarantee.

## Error exports

The DLL adds `VR_GetVRInitErrorAsSymbol`,
`VR_GetVRInitErrorAsEnglishDescription` and the legacy
`VR_GetStringForHmdError` alias. The first two signatures come from the pinned
Valve v0.9.20 header. Valve's [source at revision
0924064](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/src/openvr_api_public.cpp#L323)
establishes the legacy alias's `EVRInitError` argument, string return and
delegation to the English-description function.

The helpers cover all 60 values in the pinned enum, return immutable strings
for the DLL lifetime and work before configuration, during failures and across
callers. Unknown numeric values return an Unknown label. Native failures
receive descriptions matching their actual scope, including graphics readiness
under HmdNotFound and malformed startup paths under InstallationCorrupt; other
historical codes retain explicitly legacy descriptions. These queries do not
initialize a runtime, change tokens or mutate lifecycle status.

The staged surface now has eight historical exports plus two embedding/status
exports. `VR_IsHmdPresent`, `VR_IsRuntimeInstalled` and `VR_RuntimePath` still
need their no-session probe/path policy. The three legacy accessors
`VRControlPanel`, `VRDashboardManager` and `VRTrackedCamera` remain absent
pending exact declaration and failure-policy evidence; the fake runtime's void
placeholders are not that evidence.

## Desktop validation

Luna supplied the bounded environment collector and error helpers with tests.
Parent review corrected the missing-variable fixture, added path-length
boundaries and growth/shrink cases, corrected error descriptions, and
integrated the real exports, lifecycle entry and runner. The focused compile
passed 169 explicit-configuration checks and 175 bootstrap checks against the
actual DLL. Both exercise missing graphics publication, missing Present
progress, cancellation, missing loader, cleanup and retry. Error calls are
checked before configuration and after failed startup without changing status.
The absent-loader fixture prevents either desktop mode from creating an OpenXR
runtime.

The runner passed 46 checks, including bootstrap prerequisites, argument
selection, child-only path injection, removal of inherited bootstrap values in
explicit mode, unchanged parent environment, no-write dry runs and the existing
child watchdog. The absolute-path full `build.bat` passed with exit 0,
including both native-module modes, 168 Present/discovery checks, 67 queue
checks, 100 render-dispatcher checks, 504 proxy-state checks, 10,774 stereo
checks, 35 native desktop checks, 15 device checks and the 254-key
configuration contract.

The full log is `build/openxr-native-bootstrap-build.log`. The record in
`build/openxr-native-bootstrap-validation.json` hashes 463 source inputs, 11
binaries, two environment files and the build log: 477 hashes. PE inspection
confirmed exactly ten named, non-forwarded exports, and an independent
comparison against the pinned enum confirmed both mappings for every one of its
60 values.

The native DLL SHA256 is
`b24dd2f63e5d163c8f9061701075ffd2b8e6a3001ac5740972a50f2a63b0052c`, the
application fixture is
`dc9e8aff1e2fd593b4f3be44e7ae0d5cb0017012b059c1cc71ff8cf4da4f4229`, and the
graphics proxy is
`fdbce5cf3b248665555f78f4cf6c761d1451c8edaa108b81d8ab8c21111d7e30`.

## Passed native bootstrap run

The `eee7a1c` bootstrap gate passed after user readiness on 2026-09-13. The
runner used `--bootstrap`, supplied the exact hashed loader and graphics paths
in the child environment, and recorded `bootstrap: true`. The application
verified those paths and skipped the embedding configuration call. The process
ran from 17:29:42.043 to 17:30:03.278 UTC, exited 0 after 21.235 seconds, and
did not trigger its 60-second watchdog. All 477 recorded hashes matched before
and after the run. No game, SteamVR or diagnostic process was observed in the
preflight or postflight snapshots; these snapshots do not monitor transient
processes.

Both configuration markers are present. The DLL's flushed
`module_configuration,source=environment` precedes runtime startup. The
fixture's `module_configuration,source=bootstrap_requested,embedding_call=0`
appears later in the captured file because its output is buffered by a separate
CRT, although its source call precedes Init. Its file position is not evidence
of cross-module execution order.

Pimax OpenXR 0.1.0 used the Crystal Super with parallel projection enabled, the
RTX 5090 and 5424 by 5356 pixels per eye. Startup published valid geometry
before any stereo submission. Focus was true at the first sample; the grid and
recenter began at 500 ms, the scene at 3500 ms and viewing completed at 20500
ms. The run completed 1529 stereo pairs, 3058 eye copies, 314 loading
projections and 1849 callbacks, with zero invalid pose samples. The matching
Pimax logs identify this process, show about 90 FPS during viewing and record
its normal disconnection.

Init, rendering, OpenXR ownership and shutdown used four distinct callers. All
nine shutdown stages completed while the final Present callback remained held;
the owner joined, cleanup succeeded, the callback retired and no generation was
retained. Session-binding begin/end share one `GetTickCount64` sample. Their
zero tick delta does not measure zero elapsed time.

The user confirmed the grid followed by the triangle in both eyes, upright and
fixed in space during head movement, normal color and clarity, and normal
closure without headlock. They then noted that the triangle's background looked
dark grey rather than completely black. This is expected from the fixture's
explicit RGB `(0.03, 0.03, 0.03)` clear in `tools/openxr_module_test/scene.h`.
That header matches the preceding `7ec4f85` flight, so this checkpoint did not
change the clear color; why the earlier background appeared black is not
established.

The receipt, console output, preflight/postflight records, Pimax log excerpts,
qualification record, validation hashes and exact tested binaries are archived
in `build/openxr-native-20260913-112941/`. Focus was already true at the first
sample, so delayed physical wake and focus-loss replay remain desktop-tested
only.

The earlier [7ec4f85 headset
result](openxr-native-module-2026-09-13.md#passed-focused-native-dll-run)
qualifies explicit configuration on its archived binaries. This new run
qualifies bootstrap startup on the merged graphics build, but leaves complete
legacy exports, independent transport-hook availability, Frontier Init/Shutdown
render-progress evidence, feature integration and a native Frontier flight
open. No live installation changed at this checkpoint; the last recorded
Frontier build is `f3c205e`.
