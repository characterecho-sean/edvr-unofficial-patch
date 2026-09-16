# Native capture on a separate OpenXR device

## Status

*Added 2026-09-15. Restates the journal below; update it whenever this doc
changes.*

- **State:** separate-device capture is the packaged default of 0.17.0-rc.1
  (`separate_device=1`), qualified on Pimax 2026-09-13 ("Pimax result").
  2026-09-15: rc.1 lost VR entirely whenever EDHM (3Dmigoto 1.3.16) was
  chained behind EDVR. Cause found in 3Dmigoto's source, fixed on main
  (bfdd505) and CONFIRMED BY FLIGHT the same evening ("EDHM redirect"
  below). rc.1 as published still has the bug; rc.2 / 0.17.0 carries the fix.
- **Open:** nothing on this arc. The next packaged build needs a release-note
  line for EDHM users.
- **Ruled out:** listed at the end of "EDHM redirect".
- **Next flight:** none needed. If the signature ever returns, read the
  `device_module,route=...,path=...` line first: it names the d3d11 module
  the XR device came from. Pass is `route=mapped,path=C:\WINDOWS\system32\d3d11.dll`
  then `device,adapter=...` and `module_startup,...,result=0`. No such line
  means an old DLL flew; a game-directory path means a LoadLibrary redirect
  got in again; the System32 path followed by `error,D3D11Device,<hr>` means
  the module was right and device creation itself failed.
- **Environment:** Frontier install, `[advanced] real_dll = d3d11_edhm.dll`,
  EDHM's `d3dx.ini` `[System] load_library_redirect=2`, Pimax Crystal Super
  on Pimax OpenXR, 4068x4016 per eye, 90 Hz, in both the failing and the
  confirming log.

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

## EDHM redirect (2026-09-15)

**Report.** A user lost VR on 0.17.0-rc.1. Sean reproduced it on the Frontier
install with EDHM installed; the Steam install, without EDHM, works on the
same build.

**Evidence.** `edvr_openxr_20260915_175851_097_23448.log`, build
`v0.17.0-rc.1-1-ga44ae48` (checked with `edvr_log.py --expect-build`):
`module_configuration,source=local` -> `runtime,Pimax OpenXR` ->
`system,...,name=Pimax Crystal Super` -> `size,0,4068,4016` ->
`error,D3D11Device,80070005` -> `module_startup,...,result=124` -> shutdown,
`native_summary,waits=0,submits=0`. The graphics log of the same session
shows the chain working for the game's own device: "chaining through
...\d3d11_edhm.dll -- 41 export(s) from it, 8 from the system d3d11.dll",
"the chained proxy asked for d3d11.dll and got edvr... Sending it to the
system d3d11.dll instead", LiveCopy with 96 of 96 sampled vtable entries
inside Windows' d3d11.dll, 5,693 frames. Every Frontier flight earlier that
day, before EDHM was installed at 17:56, ended `result=0` with tens of
thousands of submits.

**Cause.** 80070005 is E_ACCESSDENIED. On the separate-device startup path
the only producer of it before device creation is the identity check in
`NativeDevice::initializeSeparate` (src\openxr\native_device.h): it called
`LoadLibraryExW` with the full System32 path of `d3d11.dll` and required
`GetModuleFileNameW` of the result to equal that path. EDHM's 3Dmigoto
(1.3.16; its `d3dx.ini` has `[System] load_library_redirect=2`) hooks
kernel32 `LoadLibraryExW` from its DllMain (DLLMainHook.cpp:93-95, 338) and
`ReplaceOnMatch` (D3D11Wrapper.cpp:1134-1190, tag 1.3.16) answers a request
for exactly `<System32>\d3d11.dll` with `<3Dmigoto's own directory>\d3d11.dll`
-- the game directory, where that file is EDVR's proxy. The check rejected
it, as designed, and startup ended with no XR device. EDVR's graphics half is
unaffected: it loads the system copy in its own DllMain, before EDHM exists.

**Fix** (this branch). `initializeSeparate` now asks for the already mapped
module by full path with `GetModuleHandleExW`, which 3Dmigoto does not hook
(the precedent is `native_graphics_client.h`) and which the graphics half
keeps mapped for the life of the process; it falls back to `LoadLibraryExW`
only when nothing is mapped at that path (bare test rigs). The identity check
stays. In separate-device mode the host prints, right after the call,
`device_module,route=<mapped|loaded|none>,path=<what was actually obtained>`,
so a redirected module is visible above any `error,D3D11Device` line and an
old DLL (no `device_module` line) is distinguishable from a new one.
`native_device_test --self-test` asserts the mapped route and the System32
path.

- ruled out: a stale build in the log, because `--expect-build` matched.
- ruled out: producer device validation, because that failure prints
  `result,producer_device_validation,...` and never `error,D3D11Device`.
- ruled out: the graphics-bridge and render-callback E_ACCESSDENIED paths
  (graphics_bridge.cpp, native_render_binding.h), because the binding was
  acquired before the failure and both fire only after device creation.
- ruled out: an EDHM chain fault on the game's device, because the graphics
  log shows the chain resolved and LiveCopy hooking Windows' d3d11.dll for
  the whole session.
- ruled out: runtime or headset, because the same Pimax runtime and headset
  flew `result=0` the same day before EDHM went in.
- not tried, on purpose: turning off EDHM's `load_library_redirect` or
  setting `separate_device=0`; neither is a fix EDVR can ship.

**Confirmed by flight (2026-09-15, evening).** Frontier install, EDHM
chained, build `v0.17.0-rc.1-5-gbfdd505` in both logs (`--expect-build HEAD`
exit 0). The graphics log shows the same chain as the failing session
("chaining through ...\d3d11_edhm.dll -- 41 export(s) from it, 8 from the
system d3d11.dll, 0 unresolved", LiveCopy with 96 of 96 sampled entries
inside Windows' d3d11.dll). `edvr_openxr_20260915_185532_954_26200.log`:
`device_module,route=mapped,path=C:\WINDOWS\system32\d3d11.dll` ->
`device,adapter=00000000:00013c19,feature=b100` ->
`module_startup,token=1,...,result=0` -> `native_summary,waits=1985,
submits=3966,pairs=1983,...,pose_failures=0` -> clean shutdown
(`module_shutdown_return,exception=0`). No `error,` line in the session.
Fail C did not happen: the System32 export produced a working device with
3Dmigoto resident, so its own `D3D11CreateDevice` hook does not reach the
System32 module in this install. Sean confirmed VR in the headset.
