# Paired device discovery and native startup admission

The [shared runtime and callback-held
teardown](openxr-shared-host-2026-09-13.md) passed its `3fcadf7` PiOpenXR
headset gate. That diagnostic still handed its device, context and render
thread directly from the test harness to startup. This checkpoint replaces that
handoff with reusable discovery and callback registration on the separate Init
caller. The graphics proxy publishes the device; its first actual callback
identifies the render caller.

## Device discovery

`edvrAcquireNativeGraphics` is an additive versioned C export in the graphics
proxy. It returns independently retained references to the registered device
and immediate context. It requires a ready ExecuteCommandList bridge owner, the
matching registered render-boundary owner, and the published game-device COM
identity. Single-threaded devices are unsupported and device-removal errors are
preserved. Missing owners fail promptly instead of creating a fallback device.
The call performs no rendering and reserves no graphics lease.

Request and output sizes and versions must match exactly. Failures clear only
complete fields within the supplied output size. The caller retains the
provider module until its interface references and bridge leases are released.
The existing bridge and Present export contracts remain unchanged.

`NativeGraphicsClient` accepts a trusted drive-absolute path to an already
loaded module. It retains that module, checks its loaded full path, and
requires all three discovery/graphics/Present exports to resolve inside that
same module. It never loads a replacement, searches by basename or reads a
device pointer directly from the legacy shared mapping. The embedding backend
must supply the trusted path; this API does not discover a game installation or
choose a backend. Path and export checks are module-identity checks, not
cryptographic authentication of an arbitrary third-party DLL.

## Observing the render caller

`NativeRenderBinding` acquires the snapshot and registers a callback on the
Init caller. Its first real owned, non-TEST Present callback verifies the
device/context and binds the queue to that caller. Init waits up to five
seconds for this readiness before creating an XR owner or native runtime
resources. An absent callback is an explicit startup failure, with no queued
GPU work to retire. The application must keep rendering while Init waits; the
Frontier census established separate callers, but did not establish that
progress.

Registration alone does not prove that a swapchain Present hook was committed.
The owner is currently registered after vScreen's ExecuteCommandList hook; it
can exist before the first Present. Waiting for the actual callback avoids
treating the registration as proof of render delivery. If optional hook
installation is disabled or skipped, discovery can report unavailable. This
checkpoint does not install those hooks independently of existing features.

After readiness, the existing owner/render dispatcher and runtime host use the
discovered device and retained provider. The paired route does not reload the
graphics DLL from a name or create another device. The diagnostic's comparison
with its original device is an assertion only, not a source of startup state.
Its `paired_startup` record reports discovery, Init/render callers, device
equality and provider equality before the native runtime startup record.

Shutdown keeps the previously qualified callback-held finalizer. Closing the
binding stops new callback and queued-work admission; an active callback keeps
release pending and retains the device/context/module references until it
unwinds. Only then can the binding release its snapshot. A binding owns one
queue generation; restart/recovery policy remains the embedding backend's
responsibility. Arbitrary game use of the immediate context still requires
external exclusion.

## Validation

Luna implemented the provider export and initial desktop tests. Parent review
added the clients and runtime integration and corrected the test ordering: the
queue rejects work before binding, so the fixture must observe the first
Present before enqueueing Init work. The actual-DLL fixture covers absent
publication, malformed ABI inputs and output canaries, invalid or unloaded
paths, a non-provider system DLL, foreign-thread discovery, missing Present,
first-callback readiness and active-callback release retention. Existing
graphics-state and teardown tests remain in the full build.

The full absolute-path `build.bat` passed with exit 0, including 153 actual-DLL
Present/discovery checks, 67 CPU queue checks, 100 render-dispatcher checks,
504 proxy-state checks, 10,774 stereo checks, 35 native desktop checks, 15
existing-device checks and 29 native-runner checks. The 80,493 weapon-motion,
1,242 mesh-motion checks and 254-key configuration contract also passed. The
generated graphics export table includes `edvrAcquireNativeGraphics`, and the
actual-DLL fixture exercised it through `GetProcAddress`.

The full output is `build/openxr-native-discovery-build.log`. Exact hashes of
448 source inputs, nine binaries, the native loader/runtime manifest and the
build log are recorded in `build/openxr-native-discovery-validation.json`. The
native executable SHA256 is
`ec2fd692cf6d27b77575502dbc77c6bdaab37e3482a58e7c474ef8776d467b15` and the
graphics DLL SHA256 is
`e1441314521067e197568a080ee07c6fed1b44cc30e8f2cf58c65c21101120b9`.

Native qualification is pending explicit headset readiness. The game and
SteamVR were running during the final desktop checks. The next 20-second
PiOpenXR gate must show `paired_startup` with matching device/provider and
distinct Init/render callers before runtime startup, successful loading and
stereo counters, and completed callback-held shutdown. The user's visual,
tracking and normal-closure report remains a separate gate. No Frontier files,
live configuration or saved runtime selection changed; the Frontier install
remains `f3c205e`. Shipping OpenVR exports still forward. Complete legacy
export policy, a game-facing native backend, feature integration, Init/Shutdown
render-progress evidence and a native Frontier flight remain open.
