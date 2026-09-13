# Separate native runtime DLL and application fixture

The [startup-discovery checkpoint](openxr-native-discovery-2026-09-13.md)
passed its `b8db929` PiOpenXR headset gate. This checkpoint moves the native
backend behind a separately loaded DLL and exercises it from an application
that uses the historical OpenVR ABI. The full desktop build passed; this new
DLL route has not yet passed its headset gate.

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

## Next gate and remaining integration

The next gate is a 20-second native Pimax run through the new DLL: grid in both
eyes, upright and world-fixed during head movement, transition to the triangle,
correct color and clarity, then normal closure without an end-of-run headlock.
Require fresh user readiness and no game, SteamVR or other native diagnostic in
preflight. Verify the exact recorded inputs and binaries before and after the
run, and inspect the module summary and shutdown stages before recording
success.

This DLL is deliberately not named `openvr_api.dll` and is absent from
installer payload selection. It is not the complete 14-name legacy export
replacement; unresolved legacy factory signatures must not be guessed. Native
backend selection, complete export policy, independent graphics-hook
availability, game Init/Shutdown render-progress evidence, feature integration
and a native Frontier flight remain open. No Frontier files, live configuration
or saved runtime selection changed. The Frontier install remains `f3c205e`, and
shipping OpenVR discovery continues to forward.
