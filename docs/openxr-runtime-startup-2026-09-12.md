# OpenXR runtime startup and imported exports

This checkpoint joins the standalone native diagnostic to a runtime lifecycle
coordinator and the five historical C entry points imported by Frontier. It
also supplies the exact ExtendedDisplay and Chaperone interface shapes. The
game backend, complete legacy export surface and production thread ownership
remain separate work. Neither shipping proxy discovers these objects, and the
tested `f3c205e` Frontier installation remains unchanged.

## Startup contract

The [Frontier census](openxr-semantic-flight-2026-09-12.md) observed System
geometry queries about 2.62 seconds before the first compositor wait. Init,
later System calls and rendering use different threads. Returning an interface
whose geometry becomes valid only after the game's first WaitGetPoses would not
satisfy that ordering.

`RuntimeLifecycle` delegates resource ownership to `RuntimeBackend`. A
successful backend start must provide all four owned interface pointers and
coherent startup geometry. The native diagnostic now opens its D3D11 session,
pumps READY, and completes zero-layer frames until valid native geometry is
cached, all inside its Init call. It then retrieves System and ExtendedDisplay
and queries geometry before retrieving Compositor. A joined second thread
checks cached render size and the aggregate-return projection method.

The post-open bootstrap has a 15-second deadline and observes cancellation
between runtime operations. Individual runtime calls cannot be interrupted; the
existing diagnostic runner bounds the whole child to the requested run duration
plus 40 seconds. No fallback geometry is invented to pass startup.

The diagnostic adapter still owns its device and frame operations on its main
thread. This is not yet a game service that can marshal Init, live pose
queries, frame work and Shutdown across Frontier's observed threads. Its native
adapter accepts one initialization per process; repeated Init while running
returns the existing token without starting again. Clean resource
reinitialization is tested in the desktop coordinator/fixture, not claimed for
that native adapter.

## Publication, cancellation and cleanup

Only Scene initialization is accepted. The first admitted attempt advances the
token; repeated successful Init keeps the same token and interface identities.
Concurrent startup returns Retry. Getters publish the complete table only after
startup succeeds, and return NotInitialized before startup or after retirement.
Unknown and null versions receive explicit historical errors. Version
validation accepts only the four exact pinned interface names.

Shutdown retires the table and advances the token before cleanup. During
startup it requests cancellation and returns, leaving the initializer solely
responsible for cleaning its partial resources. New Init calls are rejected
until cleanup finishes. Repeated Shutdown is idempotent. Backend callbacks run
outside the coordinator mutex, so token/getter reads cannot deadlock behind a
blocked startup or shutdown. A cleanup failure permanently blocks restart;
tokens reserve shutdown invalidation and never wrap.

The backend and its interface objects must outlive the coordinator and every
caller. The pinned OpenVR SDK invalidates returned pointers at Shutdown; this
code does not promise that callers can retain and use old pointers after a
restart. A future production owner must also join/exclude resource users before
destruction. Binding and explicit initialization happen outside DllMain.

The diagnostic's ordinary exit requests STOPPING and ends the session. Failed
or cancelled startup may destroy the session directly after excluding handle
users: Khronos permits [xrDestroySession in any session
state](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrDestroySession.html).
Calling [xrEndSession before
STOPPING](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrEndSession.html)
would itself be invalid. The coordinator does not attempt to force that call as
a generic cleanup step.

## Export and auxiliary scope

`runtime_exports.cpp` provides typed internal entry points for VR_InitInternal,
VR_ShutdownInternal, VR_GetGenericInterface, VR_IsInterfaceVersionValid and
VR_GetInitToken. A once-bound coordinator owns their state. The standalone
executable calls these entry points, while a headset-free test DLL aliases them
to the exact historical PE export names and is exercised through
LoadLibrary/GetProcAddress by a separate executable.

This fixture is named `openxr_export_fixture.dll`, never `openvr_api.dll`. It
includes a test-only explicit binding export and real owned interface objects.
Its synthetic geometry is confined to that test. The existing 14-export
replacement gate is not complete. In particular, the exact legacy
VRDashboardManager declaration remains unresolved; a guessed void return from
an inert fake runtime is not ABI evidence. Presence, runtime-path, error-string
and legacy accessor behavior still need an explicit production policy.

The auxiliary objects use the pinned v0.9.20 declarations:

| Interface | Implemented policy |
| --- | --- |
| IVRExtendedDisplay_001, three methods | Cached virtual side-by-side eye framebuffer, each eye's viewport, origin zero. Invalid/disconnected generations, zero dimensions or width overflow return initialized zero rectangles. DXGI physical output is unavailable, with both indices -1. |
| IVRChaperone_003, eight methods | Calibration error and unavailable play area; failed queries initialize their outputs. No boundary renderer is claimed. Color requests are initialized within caller capacity, visibility is false and setters/reload do not change runtime state. |

The exact names above are checked by the C++ declarations and tests. The
ExtendedDisplay framebuffer does not claim an HMD desktop monitor rectangle.
System's separately validated adapter query remains the path to the runtime's
graphics adapter. Unsupported auxiliary calls report once per object/method.
Actual STAGE bounds and boundary display remain unimplemented.

## Review and desktop gate

Luna contributed the auxiliary facade, lifecycle fixture and read-only ABI and
ownership audits. Parent review corrected invalid-generation/overflow handling,
added the coordinator and export integration, replaced weak test coverage and
connected native startup. The concurrency fixtures use separate error outputs,
bounded backend waits, joined threads and a deadlock watchdog.

The focused desktop checks passed with 73 lifecycle, 234 auxiliary and 35
export checks. These counts include repeated buffers, versions and states; they
are not counts of independent scenarios. Coverage includes partial, exceptional
and cancelled startup, blocked shutdown, reentrant coordinator reads, permanent
cleanup failure, token exhaustion, all auxiliary virtual slots, null output
combinations, caller-buffer guards and DLL-returned interface calls from
another thread. Each executable has strict self-test and no-write dry-run modes
and is included in the full build gate.

The full absolute-path build passed with Frontier's original OpenVR DLL. It
repeated all three new test gates and passed the existing native, compositor,
binding, rendering and Python regressions plus the 252-key config contract.
Exact source, harness, export-fixture, loader and manifest hashes are retained
in `build/openxr-runtime-validation.json`; the full build log is
`build/openxr-runtime-build.log`. The matching native result follows below;
earlier native results were not reused to qualify this revised executable.

## Native gate and result

Run the reviewed 20-second PiOpenXR diagnostic with Frontier and SteamVR closed
and the user ready in the Pimax headset. Match executable, source, loader and
manifest hashes to the build record first.

Require runtime_startup to report valid geometry before any Submit, then
runtime_exports to confirm all four interfaces, stable repeated Init/identity,
unknown-interface rejection and the cross-thread cached geometry check. The
existing two reset/event checks and stereo copy/Submit/handoff checks remain
required. Startup frames are counted separately: compositor waits must equal
render-loop frames plus startup frames; cached comparisons count render-loop
frames only. Shutdown must retire the interfaces, advance the token, clean
resources once and close normally.

The `d7623ed` diagnostic passed the 20-second PiOpenXR run with executable,
sources, loader and manifest matched to the full-build record. Frontier and
SteamVR were absent before and after. Pimax OpenXR 0.1.0 reported D3D11.1, 5424
x 5356 per eye and sRGB swapchains. The child exited 0 after 20.39 seconds
without the watchdog firing.

Init completed one zero-layer frame, published geometry on sequence 1 with no
prior Submit, and exposed all four interfaces. Repeated Init kept token 1 and
the same Compositor identity. The second-thread cached geometry check passed
before Compositor retrieval; unknown-interface rejection passed afterward. The
virtual display reported 10848 x 5356.

Both seated resets passed the position/yaw bounds, advanced origin generation
to 2 then 3, invalidated the caches and produced one event each. Geometry was
republished on sequence 4 before stereo. The frame checks completed 1801 waits
(one startup plus 1800 render-loop frames), 1800 cache comparisons, 1801 valid
gameplay poses, 3592 Submit calls/private eye copies, 1796 stereo pairs and
handoffs, and four render-loop zero-layer frames. There were 1799 valid
view/head samples and no invalid views. Shutdown advanced the token to 2,
retired the interfaces and cleaned resources exactly once with normal session
stop. No natural runtime-origin change occurred; that policy still has
desktop-only qualification.

The user confirmed the triangle in front in both eyes, upright and fixed in
space during head movement, normal color/clarity and normal closure. The
receipt and output remain under `build/openxr-native-20260912-170532`, with
parsed counters and visual confirmation in the validation record.

This completes the standalone startup sequence gate. Persistent lifecycle
pumping, game-thread dispatch, required skybox/fade/event behavior, complete
legacy exports, the paired feature handshake and a production rendering pass
that preserves EDVR's graphics state remain open. The run did not install or
test a native Frontier backend.
