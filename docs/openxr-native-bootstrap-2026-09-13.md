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
`fdbce5cf3b248665555f78f4cf6c761d1451c8edaa108b81d8ab8c21111d7e30`. The native
bootstrap headset gate remains pending.

## Next native gate

Run the existing 20-second Pimax DLL diagnostic with `--bootstrap` after fresh
user readiness. The runner puts the exact hashed loader and graphics paths in
the child environment, records `bootstrap: true`, and adds the corresponding
executable option. The harness verifies those paths match its command and skips
the embedding call. Require both
`module_configuration,source=bootstrap_requested,embedding_call=0` and
`module_configuration,source=environment` before successful startup, followed
by normal grid/triangle viewing and complete cleanup.

The earlier [7ec4f85 headset
result](openxr-native-module-2026-09-13.md#passed-focused-native-dll-run)
qualifies explicit configuration on its archived binaries; it does not qualify
this new path or the merged graphics build. A successful bootstrap diagnostic
will still leave complete legacy exports, independent transport-hook
availability, Frontier Init/Shutdown render-progress evidence, feature
integration and a native Frontier flight open. The Frontier installation
remains `f3c205e`.
