# OpenXR work at the graphics proxy's Present hook

This checkpoint connects the staged OpenXR renderer to the real owned-swapchain
Present hook. The native diagnostic can now create its D3D11 device before VR
Init, call Init from a different thread, and perform immediate-context work on
the Present caller. This follows the passed [paired-proxy render-caller
gate](openxr-render-thread-2026-09-13.md). Shipping OpenVR exports still
forward to the existing runtime; native Frontier transport and feature
integration remain open.

## Callback ownership

The versioned `edvrAcquireRenderBoundary` C ABI accepts the published hooked
device and one callback lease. Registration is CPU-only and does not require
the Init caller to borrow the immediate context. The first successful owned,
non-TEST Present fixes the callback thread, including when Present precedes
registration. Subsequent delivery follows the graphics proxy's existing frame
work. Foreign swapchains do not deliver it; a different Present thread closes
admission rather than moving the context owner.

The provider validates canonical device identity and rejects another device on
the same adapter, a removed device, single-threaded device creation, unknown
structure versions and duplicate leases. It retains device/context and callback
code references. The client retains the explicitly supplied provider module and
rejects forwarded or missing exports. There is no basename discovery fallback.

Closing admission and admitting a callback share one mutex. `close` and
`release` return `E_PENDING` while a callback is active; the caller must retain
its user state and retry release after the callback unwinds. Lease operations
are externally serialized. Callback failure or a C++ exception closes admission
without replacing the real Present result. Callbacks execute outside the
registry lock, and release frees its module reference outside that lock.

The hook identifies a render boundary; it does not serialize unrelated game
threads that might use the same immediate context. The shipping integration
must preserve the measured game context ownership. Present-time CPU/GPU
instrumentation and feature work are otherwise unchanged, and runtime frame
work is not included in the existing Present-wait measurement.

## Foreign callers and native diagnostic

`PresentWorkQueue` admits at most 16 queued CPU requests. A foreign caller
waits for one request to run at a real Present callback. Requests waiting
longer than five seconds are cancelled only while still queued. An active
request must finish before its caller returns; its captures are destroyed
before wakeup. Close cancels queued work and leaves active work to finish.
Rebinding, render-thread self-enqueue and nested pumping are rejected.

At the callback, `RenderThreadDispatcher` retains the previous synchronous XR
owner/immediate-caller rendezvous. The XR owner records private deferred
command lists and owns session, swapchain and frame operations. Immediate
playback, copies and GPU completion execute on the Present caller. Idle owner
polling does not gain graphics admission. Game loading work still needs
explicit requests; installing a callback alone does not create a frame
scheduler.

The native runner's `--present-boundary` option requires `--graphics-proxy`. It
creates a hidden HWND and hardware swapchain through that exact proxy before
starting the separate Init/controller thread. Window creation, message pumping,
Present and destruction stay together because [DXGI
Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)
may wait on the window's message-pump thread. The selected pre-existing device
must match OpenXR's required adapter and minimum feature level; mismatch fails
without creating a replacement device. The XR owner and System caller remain
separate threads as in the earlier diagnostic.

Startup, loading, scene rendering and GPU drain use this same route. Normal
System-thread Shutdown can request the final drain while the Present caller
continues pumping. Callback admission closes and the lease releases after the
controller joins. If graphics completion cannot be established, the isolated
diagnostic retains resources until process exit as at the previous checkpoint.
An individual driver/runtime call may still block; the runner's external
watchdog bounds the child. Production recovery from blocked rendering or device
loss is not qualified here.

## Qualification

The full absolute-path `build.bat` completed with exit 0. The new CPU queue
fixture passed 41 checks, including queue exhaustion, pending cancellation,
exception/capture lifetime, reentrancy and close after an active request's
pending deadline. The actual-DLL WARP Present fixture passed 65 checks. It
registers from a foreign Init thread before first Present, verifies that TEST
Present leaves startup work queued, records a private command list on the XR
owner and plays it on the Present caller. GPU readbacks confirm the private
cyan pixel and untouched red game pixel; the game's render-target binding is
restored and the private execution counter advances exactly once without an
unknown-list execution. It also checks failed/throwing callback admission and
close/release while a callback is blocked.

Existing gates passed: 100 render-dispatcher, 504 actual-proxy
binding/renderer, 10,774 stereo, 80,493 weapon-motion and 1,242 mesh-motion
checks. The revised native executable passed 35 desktop checks, the
existing-device fixture passed 15, and the runner passed 29. The 254-key
configuration contract passed. These tests use WARP or injected XR dispatch;
none opens a headset session.

The complete build output is `build/openxr-present-boundary-build.log`. Exact
source, binary, build-log and native environment hashes are recorded in
`build/openxr-present-boundary-validation.json`; pre-commit version stamps do
not identify those binaries by themselves. The subsequent headset result is
recorded below. Luna supplied the provider, queue and initial actual-DLL
fixture; parent review corrected admission synchronization and fixture
coverage, integrated the native harness, and reviewed the final implementation.

The new fixture covers one real Present callback route and a representative
render-target/pixel preservation case; the existing 504-check fixture retains
the broader binding coverage. It does not force the narrow close/admission
race, real driver hangs, device removal, multiple native adapters or a game's
startup thread blocking its render thread. Those are not implied by a passing
desktop test.

The installed Frontier pair remains `f3c205e`. This checkpoint does not alter
the game installation, live INI, registry or saved runtime selection. The next
headset gate must identify the exact new executable and proxy hashes and verify
distinct Init/Present/XR/System callers, loading-to-scene transition, private
command-list totals and completed shutdown. Passing it will qualify this
diagnostic route, not a native Frontier game flight.

## Failed PiOpenXR shutdown gate

The `2734c15` run on 2026-09-13 did not pass. All 451 source, binary, build-log
and native environment hashes matched before launch. Frontier, SteamVR and
other native diagnostics were absent before launch and after the runner exited.
The 20-second diagnostic began at 13:31:18.648 UTC; its 60-second watchdog
terminated it at 13:32:18.690 UTC, with an elapsed time of 60.046 seconds and
child exit code 1. This was a watchdog termination, not evidence of a native
crash exception.

The exact receipt and log are in `build/openxr-native-20260913-073118/`, along
with archived copies of the tested executable, graphics proxy and preflight
validation manifest. The executable SHA-256 is
`e2a75acbec75134ab46af8a0ed77be25d5f0988337c56fd4a569c2c961c9786d`; the
graphics proxy is
`1b7dd694cae9ebc9825c9770ce6aa738995e54c625fbf274fb91571e83509a62`.

Pimax OpenXR 0.1.0 used D3D11.1, 5424 x 5356 per eye and format 29. The
observed threads were Init 39568, graphics/Present 30036, XR owner 39356 and
System 30816. Startup geometry, both recenter events and idle graphics
exclusion passed. Loading completed 269 projection frames with zero game
Submits and an unchanged pose cache. The run reached scene bootstrap with valid
stereo view and head poses. It then printed two successful
`gpu_drain,result=0,pending=0` records but never reached the final
runtime-service, frame, bridge or shutdown summaries; complete scene totals and
cleanup success are unavailable.

The user reported, "It was fine till the end, then the triangle became
headlocked." The end-of-run symptom is part of this failed gate, not a passed
tracking/closure result.

Ruled out: failure to service either requested final GPU drain, because both
render-route drain callbacks reported success with no pending GPU execution.
The last message precedes render-dispatcher closure and XR-owner finalization
and join. Swapchain/session destruction, resource release, runtime unloading
and a thread wait remain possible blocking points. Continued Present calls
during teardown are a sequencing hypothesis; no thread stack or call-level
trace establishes a runtime/driver lock cycle. Do not change image quality or
claim a teardown fix from this log alone.

The revised diagnostic adds flushed begin/end teardown markers with thread ID,
tick time and operation outcome; an unwound stage without a completed result
prints `abandoned`. It covers the two drains, System-thread join, render
closure, XR-owner join/finalizer, capture release, stereo/seated/binding
shutdown, graphics release, instance destruction, loader unloading and callback
lease release. It does not change rendering or teardown sequencing. These
markers localize a repeat stall; they do not establish a fix.

The revised full build passed with exit 0 and the same desktop check totals
listed above. Its cancellation fixture emits matched backend prepare, render
close, owner-finalizer and join markers. The remaining native resource-cleanup
markers compile but require the next headset run to exercise that path. Build
output is `build/openxr-shutdown-trace-build.log`, and exact hashes are in
`build/openxr-shutdown-trace-validation.json`. A fresh headset gate remains
pending. The installed Frontier pair and runtime selection remain unchanged.
