# Native shutdown after application Present stops

The [Frontier lifetime flight](openxr-shutdown-lifetime-2026-09-13.md)
established a live render caller outside Present during forwarded shutdown: no
active Present, unchanged balanced activity and zero callback-service samples
over 51.4957 ms. The last Present ended 0.3060 ms before forwarding began. That
observed ordering needs desktop coverage before a native cleanup change.

## Scope

This checkpoint shares the native module's existing two-stage stop coordination
through `src/openxr/render_shutdown_coordinator.h` with a desktop fixture. The
System caller first routes the final GPU drain through the render callback,
then requests cleanup while the render caller is held inside another callback.
The production ordering, default deadlines and conservative retention policy
remain in force. No thread-state snapshot grants permission to borrow the
application context.

The fixture uses the actual graphics proxy, a hidden WARP swapchain,
`NativeRenderBinding`, `PresentWorkQueue`, `RenderRoute`,
`RenderThreadDispatcher` and `OwnerService`. Controlled drain/finalizer
operations and resource-lifetime counters replace the OpenXR runtime. The cases
distinguish a first drain that cannot be serviced, a completed drain followed
by an unavailable cleanup callback, and continued or resumed Present service.

Cancellation must finish before borrowed captures go out of scope. A late
Present must not execute cancelled work or enter a closed callback lease. An
operation that has already entered must finish before its caller returns, even
if a pending deadline has expired. Retained resources are not counted as
completed native cleanup.

## Desktop cases

Eight cases run against a real graphics callback in
`tools/openxr_shutdown_test/openxr_shutdown_test.cpp`:

| Case | Required result |
|---|---|
| Render caller stays alive without another Present | The first queued drain expires after the existing five-second deadline; neither controlled drain nor finalizer runs. |
| One Present services the drain, then Present stops | The second request expires without entering cleanup or destroying the controlled resources. |
| Present service resumes | Drain and finalizer each run once on the owner stand-in; the drain's `CountedGraphics` callback runs on the actual Present caller, distinct from System and owner. |
| A finalizer is held inside the real callback | A 600 ms hold exceeds the fixture's 500 ms pending deadline; the System caller remains blocked, captures stay alive, and close/release return `E_PENDING` until the callback retires. |
| Drain returns false | Finalization is skipped and controlled resources remain intact. |
| Drain throws | The synchronous route reports failure, finalization is skipped and controlled resources remain intact. |
| Finalizer returns false | Owner work joins, but cleanup is reported as unsuccessful. |
| Finalizer throws | Owner shutdown reports failure after joining; cleanup remains unsuccessful. |

In the first case, the fixture follows the module's retention sequence: close
render admission and callback delivery, join the CPU owner, and retain the
paired device/context/module references. A later real Present must deliver no
callback and must not execute cancelled work. A weak reference and destructor
counter check that the operation closures release their captures after the
coordinator call. The held-finalizer case also checks that those captures stay
alive while the call is blocked. Fixture-only disposal releases the graphics
lease after these observations; it does not simulate completed OpenXR
destruction.

Parent review corrected the retained-reference check, ordered the second
Present after the final request was queued, distinguished a failed finalizer
outcome from completed owner-thread retirement, and increased the held-case
deadline to allow scheduling margin. No production timeout was changed.

## Qualification limits

The absolute-path full `build.bat` completed with exit 0. All eight new cases
passed with 63 checks and zero failures, alongside 201 native-module export
checks, 207 bootstrap checks, 168 Present checks, the five-case transport
matrix and the 254-key configuration contract. All 473 source hashes stayed
unchanged throughout the build. The build log, source hashes and five relevant
binaries are archived under `build/openxr-stopped-present-20260913/`;
`qualification.json` records their hashes and the tested precommit version
`v0.16.2-71-g921b6a2-dirty`.

`tools/install_edvr.py --verify-only`, using a staged copy of the archived
Frontier baseline, confirms that the installed proxies still match `2f051db`.
The live INI and original OpenVR runtime are byte-unchanged. No installation or
headset launch was performed. The archive contains the baseline verification
output and hashes separately from the new build.

This fixture exercises shared stop coordination and the real graphics callback
boundary. It does not initialize an OpenXR runtime, reproduce Pimax driver
behavior, prove real GPU completion from mock counters, or qualify game-context
access on a different caller. The existing native module export tests and prior
Pimax callback-held teardown result remain separate evidence.

A live caller outside Present cannot acknowledge a new request through a
callback it never services. Closing the callback lease excludes EDVR callback
delivery only. A production fallback still needs an explicit application
rendering exclusion or a different ownership design; returning from the hook
after a terminal-looking observation cannot promise that the game will stop
using its context.

The Frontier installation remains on the previously qualified `2f051db`
shipping proxies. No additional headset flight is requested for this desktop
checkpoint. Native game integration still needs a cleanup contract that does
not assume further Present service.
