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
continues pumping. The shutdown correction described below retires the callback
and stops Present before XR-owner finalization. If graphics completion cannot
be established, the isolated diagnostic retains resources until process exit as
at the previous checkpoint. An individual driver/runtime call may still block;
the runner's external watchdog bounds the child. Production recovery from
blocked rendering or device loss is not qualified here.

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
pending at that checkpoint. The installed Frontier pair and runtime selection
remain unchanged.

## Repeated stall and acknowledged Present stop

The traced `74612d5` 20-second run also failed. The user reported "Headlocked
again." Its 60-second watchdog expired after 60.031 seconds; the receipt,
output, exact binaries and validation manifest are archived under
`build/openxr-native-20260913-074627/`. All 451 preflight hashes matched. Init
38004, Present 2220, XR owner 26904 and System 35888 were distinct. Startup,
recenter and loading reached the scene, and both final GPU drains completed.
The last unmatched begin marker was `host_binding_shutdown`. Capture release,
stereo shutdown and seated-space shutdown had already ended successfully; the
System caller was waiting for XR-owner finalization.

A subsequent automated one-second shutdown probe reproduced the same stall
without a headset appearance qualification. Its 41-second watchdog expired
after 41.031 seconds. The local archive
`build/openxr-shutdown-probe-20260913-a/` includes a 261,208-byte minidump
captured before termination. Both runs used executable SHA-256
`79373c6c4bc3488ddb5db7c441e51f5d7f2c12a61374bbc0b69eb45917b731c9` and graphics
proxy SHA-256
`ccd77dd474ecdbc1a2efeedd4ad851e9bf2b1f7b808041428e818ee9068b684d`. Excluded
game, SteamVR and diagnostic processes were absent before and after both runs.
Runtime configuration and the Frontier installation were unchanged.

The dump places both Present thread 20184 and XR-owner thread 23104 at a native
wait. A bounded x64 unwind resolves each prefix through `ntdll`, `KERNELBASE`
and `nvwgf2umx.dll+0x5c0db9`, then stops because further unwind metadata is
unavailable. Separately, raw stack return-address candidates correlate with
local binary disassembly: the XR caller has the exact return site after
`SessionBinding::shutdown` calls `destroySession`, and the render caller has
the exact return site after `IDXGISwapChain::Present`. Runtime return
candidates include a COM call at slot 147, which the installed Windows SDK
identifies as `ID3D11DeviceContext4::Signal`. These are call-site correlations,
not a complete symbolic backtrace. They support a runtime GPU fence operation
during session destruction overlapping the application's Present. The dump does
not establish an internal driver lock cycle or prove that the runtime uses the
application's immediate-context object.

Ruled out: capture, stereo or seated-space destruction as the repeated blocking
stage, because each has a successful end marker before binding shutdown. The
binding call-site evidence narrows the stall to session destruction, before the
binding's final device release. Instance destruction and loader unloading have
not begun.

The correction requires an acknowledged stop before XR teardown. After the last
drain and render-dispatcher closure, the System caller permanently requests
Present to stop and waits up to five seconds. The render caller acknowledges
only after the current real Present returns, the callback has unwound, queued
work is closed, and callback close/release both succeed. It then services
window messages without another Present or immediate-context operation while
the XR owner destroys its resources. A stop request racing the last admission
check may permit one final Present, but session destruction still waits for its
completion. A timeout or failed acknowledgement prevents session teardown and
terminates only the isolated diagnostic with exit 3. This is a diagnostic
failure policy, not production device-loss recovery.

`present_teardown,quiesced=1,callback_retired=1,presents_after_quiesce=0`,
completed binding/instance/loader markers and normal native cleanup together
are the success signature. A flag set without callback retirement, a timed-out
wait, missing final summaries or watchdog termination is a failed gate.

Luna supplied the CPU stop gate and initial tests. Parent review fixed a test
object lifetime error and rejected acknowledgements before a stop request, then
integrated the native loop and actual-DLL teardown fixture. The latter allows a
final real Present after the request, retires its lease, executes
representative immediate-context GPU work on the XR owner, verifies its pixel
readback and keeps the window's render caller out of Present during cleanup.
The CPU fixture covers persistent timeout requests, late acknowledgement,
wrong-thread/self-wait rejection, failed acknowledgement and repeated waiters.
The full absolute-path build passed with exit 0: 63 CPU queue checks and 80
actual-DLL Present checks, with the existing 100 render-dispatcher, 504
proxy-state, 10,774 stereo, 35 native parsing, 15 existing-device, 29 runner,
80,493 weapon-motion and 1,242 mesh-motion checks also passing. The 254-key
configuration contract passed. Output is
`build/openxr-present-quiescence-build.log`; exact hashes for 440 source
inputs, nine binaries, the log and two native environment files are recorded in
`build/openxr-present-quiescence-validation.json`.

The five-second automated PiOpenXR probe at 14:10:43.203 UTC passed and exited
normally after 6.391 seconds, without reaching its 45-second watchdog. Its
archive is `build/openxr-quiescence-probe-20260913-a/`; all 452 preflight
hashes matched and the excluded processes were absent before and after.
Executable SHA-256 is
`e0aeb10e3ecc988043061161800154b00d34eb14faf39c3f63c4534ab9f760c4`; graphics
proxy SHA-256 is
`de94bff4fec6593569176048c097b7f389011dd85b775391f96457c17c1394fb`.

Init 29104, Present 31044, XR owner 40632 and System 28776 remained distinct.
The run completed 17 loading projection frames and 47 stereo pairs with 94 eye
copies. The private command-list total matched its expected 222, with zero
unknown lists. Both final GPU drains succeeded. Present acknowledgement and
callback retirement preceded XR-owner finalization; binding shutdown completed
in 63 ms, followed by successful instance destruction, loader release and
thread joins. The final signatures were
`present_teardown,quiesced=1,callback_retired=1,presents_after_quiesce=0`,
`normal_stop=1,cleanup=1`, `native_stereo: PASS` and `native_present: PASS`.

This establishes successful native shutdown for the corrected sequence in the
short probe. It does not qualify headset appearance or head tracking: no user
visual assessment was requested for that automated run. A fresh full-length
headset gate remains pending, and native Frontier integration remains open.
