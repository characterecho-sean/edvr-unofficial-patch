# OpenXR render-caller handoff

This checkpoint connects the diagnostic's XR owner to an explicitly bound
render caller. The caller keeps exclusive immediate-context ownership while
synchronously pumping graphics callbacks requested by one designated XR
operation. It also connects the native diagnostic to the paired graphics proxy
added in `9dbb93a`. Frontier startup and its separate Init/render threads are
not wired to this transport yet.

## Ownership and ordering

`RenderThreadDispatcher::invokeOwner` submits one bounded CPU request to
`OwnerService`, then executes its immediate-context callbacks on the waiting
render caller. The XR owner keeps swapchain acquire/wait/release, frame
operations and private deferred-context recording. Device creation, bridge
acquisition, eye and skybox copies, command-list playback and GPU completion
execute on the render caller. D3D11 device methods remain distinct from context
methods; Microsoft documents the context serialization requirement in
[Multithreading
Introduction](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro).

A callback receives graphics admission only while its designated owner function
is actually executing. An earlier queued owner request, idle event pumping,
wrong-thread calls and reentrant boundaries cannot borrow it. Owner and
graphics callback captures are destroyed before the other side resumes, outside
coordination locks. Exceptions are reported as failure; submitted renderer work
retains its existing sticky failure policy and is never replayed as recovery.

The new asynchronous owner submission API shares the existing 16-request queue.
Only accepted requests receive one completion, including cancellation.
Completion runs outside service/request locks and must not wait for owner
progress. The dispatcher captures shared coordination state in completion so
unwinding cannot reference an expired render-caller stack.

Admission closure is nonblocking. Pending graphics work is cancelled; an
already executing callback must complete before its waiting owner can resume.
The host must join the owner and drain its render caller before destroying the
dispatcher. There are no detached graphics callbacks, abandoned borrowed
captures or silent changes to D3D11 multithread protection. This mechanism
requires the caller to already own the context; it does not exclude unrelated
game rendering by itself.

## Loading and shutdown

The idle XR service now polls CPU/lifecycle work only. Loading projections
advance from explicit render boundaries with no application Wait/Submit,
preserving the existing copied six-face scene and game pose cache. The native
receipt checks that idle pumps continue while graphics callbacks and loading
frames remain unchanged, and that graphics access outside a boundary is
rejected. A real game integration still needs a render-hook callback during
loading, including the separate startup caller case.

Review identified a pre-existing teardown gap:
[xrDestroySwapchain](https://registry.khronos.org/OpenXR/specs/1.0-khr/html/xrspec.html#xrDestroySwapchain)
requires submitted graphics work referencing its images to have completed.
[Flush](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush)
alone does not establish completion. The renderer now places a [D3D11 event
query](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)
after submitted commands, flushes and observes successful completion on the
render caller before swapchain destruction. Pipeline bindings are preserved.

The normal diagnostic path explicitly drains before closing graphics admission,
then issues exported Shutdown from the System caller and joins the XR owner. An
early exit on the Init/render caller can drain there too. A rejected drain or
five-second polling deadline leaves the renderer's resources and lease alive
for retry, and the host must retain its session. Device removal reports failure
and retires pending execution. The isolated native diagnostic exits with an
error if safe completion remains unproven, preserving resources until process
exit instead of destroying their session. This is a diagnostic failure policy,
not a completed production device-loss recovery design. Individual driver calls
may still block; the external child watchdog remains the final bound.

## Desktop evidence and next gate

The CPU handoff fixture passes 100 checks for ordering, thread ownership,
exception/capture lifetimes, queue exhaustion/cancellation, unrelated owner
work and close during active graphics execution. Its waits observe explicit
events or bounded predicates. It does not deterministically force the narrow
race between graphics enqueue and render dequeue.

The actual-DLL WARP fixture passes 504 checks. It retains the existing bridge
and binding tests, runs real direct/captured/skybox rendering with distinct XR
and graphics threads, and verifies all eight private list classifications,
untouched game pixels and all 14 binding identities/generations. It rejects an
unowned renderer operation, prevents replay, rejects shutdown without an
available caller, retains swapchain images, then completes a real GPU event
before owner-side destruction. Timeout and actual device removal are guarded
but not injected by this fixture. The native executable passes 32 desktop
checks without loading an XR runtime.

The full repository build passed with exit 0, including 10,774 stereo, 80,493
weapon-motion and 1,242 mesh-motion checks, the 26-check runner self-test and
the 254-key configuration contract. The complete output is
`build/openxr-render-thread-build.log`; exact source, binary and native
environment hashes are recorded in
`build/openxr-render-thread-validation.json`. The binaries were built before
commit and retain that earlier dirty version stamp; the recorded hashes
identify this tested build.

Luna supplied initial dispatcher and native factory/runner changes. Parent
review replaced the dispatcher coordination and tests, integrated the renderer
and actual-DLL fixture, added explicit loading scheduling and GPU completion,
and reviewed the final changes.

The runner accepts `--graphics-proxy` as an absolute existing DLL path and
records its exact SHA-256 alongside the executable, loader and runtime
manifest. The next 20-second PiOpenXR gate will require the actual paired
proxy, verify distinct XR/render threads, zero unknown private-list
submissions, expected list totals, idle exclusion and completed teardown, in
addition to the established grid/triangle, tracking, recenter and lifecycle
checks. No new native run has been launched. Earlier headset results qualify
only their archived executables.

Complete legacy exports, native shipping discovery, real Frontier
render-hook/startup/shutdown integration, feature preservation and native game
flights remain open. The installed Frontier pair stays `f3c205e`; no game
installation, live INI, registry or saved runtime settings are changed by this
checkpoint.
