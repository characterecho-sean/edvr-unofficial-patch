# OpenXR runtime owner service

This checkpoint moves the standalone diagnostic's runtime and diagnostic D3D11
objects onto an explicit owner thread. It exercises Init from one caller,
cached and live System calls from a second caller, and rendering on the runtime
owner. Exported Shutdown runs from the System caller and joins the runtime
owner after cleanup. Neither shipping proxy nor the tested `f3c205e` Frontier
installation is changed.

The [Frontier census](openxr-semantic-flight-2026-09-12.md) established
separate Init, later System, and rendering threads. The [previous startup
checkpoint](openxr-runtime-startup-2026-09-12.md) supported cached geometry on
a second thread, but live pose/reset calls and shutdown still required the
diagnostic's original main thread. This change supplies the next ownership
boundary; it is not yet a production game-device adapter.

## Service contract

`owner_service.h` is a CPU-only executor with explicit start and stop. An
external invocation queues a synchronous request; an owner-thread invocation
runs inline when admission is open. At most 16 requests may be queued, and a
full queue rejects another request. No service mutex remains held while a
callback, idle pump or cleanup finalizer executes. Cached publications continue
to use their own short locks and bypass this queue entirely.

The idle callback runs about every 5 ms when there is no caller work and
between queued jobs when that cadence has elapsed. A long active call delays
it: this is a serialized owner, not an independent thread that races OpenXR
operations during xrWaitFrame. Queued work cannot otherwise starve the pump.

First stop closes admission, cancels queued requests and chooses the cleanup
finalizer. Cancelled callers return false; their callbacks never execute.
Active work finishes before the owner runs the finalizer. One external stopper
joins the worker and other concurrent stoppers receive the same outcome. A stop
request from the owner cannot join itself; an external join is still mandatory.
Restart is rejected until the previous join and its admitted stop callers have
completed. Thread identity is cleared before worker exit so a reused OS thread
ID cannot impersonate an unjoined owner.

Invocation exceptions are contained and reported as failure. Finalizer and idle
callback exceptions make the stop outcome fail; a repeated stop retains that
result. No callback is detached or interrupted. An active synchronous
invocation retains its caller-owned captures until completion, so a timeout
cannot release output storage while a runtime call is still writing it.
Construction of a by-value std::function argument can still allocate/throw
before API entry. Callers must keep the service and captured state alive until
their calls and join complete, and must not introduce cyclic synchronous waits.
The destructor does not perform implicit runtime cleanup and fails fast if the
explicit join was omitted. None of this work belongs in DllMain.

## Native ownership and lifecycle

`NativeBackend` starts the service and constructs Host inside it. This includes
FrameBoundary, which captures its owner thread at construction, and the
diagnostic device/session resources. Moving only the calls would have left the
frame boundary rejecting its new thread.

Init's bounded bootstrap still publishes native geometry before the exported
interfaces become available. After it returns, the owner pumps runtime events
even when no render commands arrive. STOPPING closes any remaining frame and
ends the session on the owner; terminal state invalidates cached poses and
geometry. The existing generation gate still excludes handle users during
resource destruction. The backend stop finalizer is independent of queue
capacity and always runs after the active callback, before the owner is joined.

Handle-using System methods now dispatch synchronously to the owner. The
diagnostic's System caller performs both seated resets and consumes their
events before rendering starts, then issues live QPC-based absolute-pose
queries while frame work runs. QPC is sampled when the owner executes the
query; queue wait and production prediction latency still require
qualification. Geometry and cached compositor poses remain direct publication
reads.

Each diagnostic render iteration executes as one owner job, with the historical
Compositor virtual calls and all native handle/device state serialized inside
it. Compositor source methods also reject or dispatch non-owner calls, but the
native render sequence itself calls them on the render/owner thread. This
flight does not qualify moving an actual game immediate context to another
thread or running EDVR's production graphics hooks there.

The main caller stops the System producer with a finalizer that invokes
VR_ShutdownInternal on that System thread. The runtime coordinator retires its
interface table, the runtime service cancels pending work, and Host destroys
its resources on the owner. Both services are joined before counters and final
cleanup state are read. The normal runtime stop and the explicit resource
cleanup remain distinct operations.

## Review and desktop gate

Luna contributed the service and an initial fixture plus read-only ownership
reviews. Parent review corrected callback locking, queue insertion failure,
idle starvation, concurrent stop/restart results and worker identity
retirement. It added cancellation completion ordering and replaced tests that
relied on scheduling yields, conflated cancelled/executed callbacks, raced
assertion counters or omitted the deadline guard. The retained tests use
counted checks, real queue occupancy, bounded waits, joined callers and a
process watchdog.

The focused build passed 62 owner-service checks, the existing 73 lifecycle,
234 auxiliary and 35 export checks, and 28 native checks without an OpenXR
runtime. The native self-test includes actual owner-thread Host construction
and cleanup after cancellation before loader opening. The service fixture
checks an exactly full queue, cancellation while an active callback remains
blocked, finalizer ownership/order, concurrent stop, owner-requested stop,
restart, exceptions and idle pumping with pending work. Counts include repeated
callers and are not counts of independent scenarios.

The owner fixture has strict self-test and no-write dry-run modes and is in the
full build gate. The full absolute-path build passed with Frontier's original
OpenVR DLL, repeating the owner/native checks alongside the existing rendering
and Python regressions and the 252-key config contract. Exact source, harness,
loader and manifest hashes are retained in
`build/openxr-owner-validation.json`; the build log is
`build/openxr-owner-build.log`. The matching native result follows below. No
earlier executable's headset result was reused to qualify this change.

## Native gate, result and limits

Run a fresh 20-second PiOpenXR test with Frontier and SteamVR closed and the
user ready in the Pimax headset. Match the new executable and all source,
loader and runtime-manifest hashes first.

Require distinct Init, System and render/owner thread IDs in runtime_threads,
two successful resets from the System caller and idle event pumping before
render jobs begin. runtime_service must report live System queries with valid
poses, continuing event pumps, Shutdown on the System thread and owner join.
Keep the existing native geometry/export/token, render/gameplay prediction,
cache comparison, paired eye copies, stereo submission and normal-stop checks.
Compositor waits still equal render-loop frames plus the separately counted
startup frames; moving resets before the render loop changes old absolute frame
counts. Use the relationships, not a previous run's hardcoded totals.

The `141ea14` diagnostic passed its 20-second PiOpenXR run, and the user
confirmed that it looked good. Executable, all 70 source inputs, loader and
manifest matched the full-build record. Pimax OpenXR 0.1.0 reported D3D11.1,
5424 x 5356 per eye and sRGB swapchains. The child exited 0 after 20.407
seconds without the watchdog firing.

Init, System and render/owner IDs were distinct (39052, 28988 and 15476).
Startup geometry was ready on sequence 1 before any Submit. Both System-thread
resets passed their position/yaw bounds, invalidated caches and produced one
reset event each. Geometry was republished on sequence 2 before stereo.

The service recorded 1805 event pumps and 1801 live System queries, of which
1800 returned valid poses. The invalid query is not assigned a cause by these
counters. There were 1803 compositor waits (one startup plus 1802 render-loop
frames), 1802 cache comparisons, 1803 valid gameplay poses, 3600 Submit calls
and private eye copies, and 1800 stereo pairs/handoffs. Two render-loop frames
were zero-layer. All 1801 sampled views/head poses were valid. Exported
Shutdown ran on the System thread; the owner joined, interfaces retired, the
token advanced to 2, and resources cleaned exactly once with normal session
stop. No natural runtime-origin change occurred.

Frontier and SteamVR were absent at preflight. Later process observations found
SteamVR processes created 7.272 to 11.913 seconds after the diagnostic exited,
and EliteDangerous64 created 30.031 seconds afterward. The user did not state
what initiated those later launches, so they are not attributed to either the
user or the diagnostic. Do not describe SteamVR as absent in the post-test
observation. These timestamps and the explicit PiOpenXR receipt are retained
under `build/openxr-native-20260912-173306`, with counters and visual
confirmation in the validation record.

This completes the standalone owner-thread visual and normal-lifecycle gate.
The diagnostic still uses one native session generation and its own D3D11
device. In-process native restart, runtime-origin changes and focus/loss
scenarios remain separate qualification. The event pump does not invent
additional game frames while the renderer is idle; persistent loading/skybox
rendering needs its own policy. Full legacy exports, required skybox/fade/event
behavior, the paired feature handshake, production graphics-state preservation
and native Frontier launch remain open.
