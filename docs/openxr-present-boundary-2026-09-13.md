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
not identify those binaries by themselves. No headset run is claimed for this
checkpoint yet. Luna supplied the provider, queue and initial actual-DLL
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
