# Native capture on a separate OpenXR device

The [shared transfer prerequisite](openxr-shared-device-2026-09-13.md) is now
connected to the staged native module's eye and loading captures. An explicit
version-2 embedding configuration creates a second D3D11 device on the OpenXR
runtime's required adapter. The XR worker owns its immediate context. The
existing version-1 and environment-bootstrap paths retain their previous device
and shutdown policy. This is a diagnostic qualification mode; native game
launch is still pending.

## Ownership and publication

`NativeDevice::initializeSeparate` obtains `D3D11CreateDevice` from the
verified System32 DLL and retains that module reference through device/context
release. It bypasses the application's proxy creation export. Both devices must
match the runtime-required adapter LUID and feature minimum, and shared capture
rejects identical or single-threaded devices. The actual producer callback is
validated during capture initialization, before startup can succeed.

Eye submission and six-face skybox updates still enter through the admitted
application render boundary. Only producer resource creation and copy/flush
operations need its synchronous callback. Shared handoffs, private image
publication, XR rendering and final GPU drains run on the XR worker. Each eye
retains its bounds and color-space metadata; no-render submissions validate
without changing its published image. The transfer performs no color conversion
or temporal processing.

Skybox capture keeps two reusable banks of six transfers. All inputs are
validated before any face is transferred, and a successful all-six update
switches the published bank. A failed middle-face transfer leaves the previous
bank and metadata intact. Pending copied images can be consumed without
revisiting an original game texture. Terminal transfer faults retain the owning
generation. Successful explicit shared shutdown is required before normal
capture destruction.

The shared copy textures request shader-resource binding only. They are never
rendered into. An unnecessary render-target flag would also admit producer
allocations to `fssResMaybeInflate`, whose matching size rules rewrite
render-target dimensions. Omitting that flag keeps these private transfer
allocations outside those rules without changing the game's feature settings.

## Shutdown contract

Version 2 closes producer callback and render-dispatch admission, then joins
the XR service with its finalizer on the owner thread. An already-entered
producer callback must retire synchronously; closing admission never abandons
borrowed source pointers. The finalizer drains XR rendering, completes and
drains shared transfers, destroys native rendering/session resources, and
releases the separate device. The module then waits for callback-lease
retirement before releasing the paired graphics binding. A failed drain or
incomplete callback retirement retains the generation and blocks restart.

No new Present is needed by this finalizer, and it never borrows the game's
immediate context. That addresses the dependency exposed by the [Frontier
shutdown observation](openxr-shutdown-lifetime-2026-09-13.md). It does not
prove that PiOpenXR's actual session destruction returns: that remains a
runtime test, with the original headlock failure specifically checked at the
end.

## Desktop validation

The extended fixture passed 261 checks on WARP and on the RTX 5090. It
exercises eye pixels and metadata, independent outputs, flipped bounds,
no-render preservation, wrong-device rejection, reset/reuse, six-format skybox
copies, refusal during an inactive-bank update, preserved publication, retry
and bank reuse. Both capture classes shut down while the producer executor
refuses callbacks; the invocation counts remain unchanged. These tests use
small textures and staging readbacks, so they establish correctness without
measuring full-resolution frame cost.

The device fixture passed 17 WARP checks and 11 hardware checks. On RTX 5090
adapter `00000000:00013f22`, the System32 factory produced distinct device and
immediate-context COM identities on the requested adapter. Repeated
initialization, invalid feature/adapter rejection and idempotent reset passed.
The debug layer was unavailable. Forced device removal, mutex abandonment and
timeout recovery remain unqualified on hardware.

The absolute-path full `build.bat` passed with exit 0 and all 476 source hashes
unchanged. It passed the 261-check WARP transfer/capture fixture, 206
explicit-module checks, 212 bootstrap checks, 206 separate-mode failed-startup
checks, 63 stopped-Present coordinator checks, the transport matrix and the
254-key configuration contract. The final binaries then passed the 261-check
RTX 5090 fixture and all 11 hardware device checks. The runner passed 52
checks, including mode validation and dry-run behavior. The separate-mode
desktop module fixture uses an absent loader; it proves failure cleanup and
retry without establishing a successfully running OpenXR session.

The tested precommit version is `v0.16.2-73-g4b0f2f1-dirty`. Source and binary
hashes, full build and hardware logs, and the explicit native-stage inputs are
recorded in `build/openxr-owned-capture-20260913/qualification.json`. No
Frontier files or runtime selection have been changed, and no headset session
has been launched for this checkpoint.

## Next headset gate

`tools/run_openxr_native.py --separate-device` requires the staged module,
graphics proxy and Present boundary, and rejects combination with
`--bootstrap`. Its receipt records the mode and input hashes. The fixture uses
separate Init, persistent System, application render and XR owner callers. It
displays the loading grid followed by copied-eye scene rendering, then stops
application Present before calling shutdown on the System caller.
`graphics_ownership` must report separate devices; `module_owned_shutdown` must
show identical Present counts before and after teardown, and `module_summary`
must report complete cleanup without retention.

The user must confirm both-eye rendering, stable world tracking, expected
color/clarity, the grid-to-triangle transition and normal closure without the
final triangle becoming headlocked. A successful runtime gate would qualify
this shutdown design on Pimax/PiOpenXR before completing native legacy exports
and opt-in Frontier launch. Full-resolution transfer overhead and game-feature
behavior still require an in-game flight.

## Pimax result

The `8388961` test at 15:48:39 on September 13 passed. All 488 recorded source,
binary, environment, stage and build-log hashes matched before and after the
run. Frontier, SteamVR and other native diagnostics were absent at both process
snapshots. The child used the explicit separate-device configuration with
PiOpenXR, Pimax Crystal Super, RTX 5090, parallel projection enabled and 5424 x
5356 pixels per eye. The graphics log matches the archived precommit build and
confirms the minimal LiveCopy transport.

The run rendered 1,531 stereo pairs from 3,062 copied eyes, displayed 315
loading projections, and returned 68 valid periodic System samples. The user
confirmed normal both-eye grid and triangle rendering, upright world tracking
during head movement, normal color and clarity, and normal closure without
final headlock.

System caller 22124 shut down XR owner 17588 while render caller 33156 remained
alive in the fixture's CPU wait. The application Present count was 1,917 both
before and after shutdown. All ten teardown stages completed on the XR owner,
with callback retirement, `cleanup=1`, `retained=0` and no final render
boundary. The owned GPU drain spanned 328 ms and session-binding destruction 16
ms on the coarse tick clock. The Pimax server records the diagnostic's final
disconnect at 15:49:00.828; the child exited normally with no timeout. Startup
also made a brief preliminary connection, so the final disconnect is
distinguished by its time, not merely its presence in the log.

The archive is `build/openxr-native-20260913-154839/`, including runtime and
graphics logs, matched input binaries, pre/postflight receipts and
`qualification.json`. This qualifies the separate-device design for normal
stopped-Present teardown on this Pimax setup. It does not establish arbitrary
concurrent game calls, error recovery, game-feature compatibility or the
transfer cost in Elite. The next step is completing native legacy exports and
an opt-in Frontier launch using the qualified ownership mode.
