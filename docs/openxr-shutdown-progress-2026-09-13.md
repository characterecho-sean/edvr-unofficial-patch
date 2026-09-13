# Frontier shutdown Present progress

This records the first probe and its Frontier result. The subsequent
[render-caller lifetime checkpoint](openxr-shutdown-lifetime-2026-09-13.md)
extends it with retained thread identity and whole-Present activity. That
enabled observer records activity outside shutdown windows too, superseding the
first probe's inactive-window optimization described below.

The [application System caller diagnostic](openxr-system-caller-2026-09-13.md)
passed its Pimax gate. Its render caller deliberately keeps Present running
until shutdown returns. The [archived Frontier
census](openxr-semantic-flight-2026-09-12.md) proves a complete Present during
Init, but its bounded Present samples expired before shutdown. Native teardown
needs the graphics callback service point to remain available while the System
caller waits for the XR owner.

## Observation contract

The shipping OpenVR proxy now opens a separate, bounded CPU observation window
around its forwarded `VR_ShutdownInternal` call. This uses the existing
`advanced.openvr_census` setting in both proxies. It does not select OpenXR,
change a game setting, request GPU work or hold a lock across shutdown.

The receiver counts successful owned-swapchain Presents reaching the native
callback service point, immediately before `renderBoundaryPresent`. This point
is after the real Present returns and the preceding graphics frame work.
Foreign swapchains, failed Presents and `DXGI_PRESENT_TEST` calls are excluded.
The count does not measure the timestamp of real Present's return, GPU
completion or how a future OpenXR shutdown will behave.

The OpenVR side looks up the already loaded graphics proxy by its full path
beside the executable and retains it through the observation. Both typed
exports must exist before a window is opened. Discovery never loads a missing
graphics DLL or initializes a device. Shutdown is forwarded exactly once,
including when observation is disabled or unavailable.

One token owns one observation window. Busy or invalid requests cannot replace
or close it. The receiver accepts at most 64 windows per process, independently
of the exhausted startup and VR event budgets. A short SRW lock protects the
snapshot and timestamp ordering. Outside an active window the service-point
observer performs an atomic check without QPC or lock work. A stale observer
cannot join a newer window after acquiring the lock.

## Evidence to read

The VR log writes `VR shutdown Present census:` with
`point=native_callback_service`, the lifecycle call ID and an explicit
`status=measured` or `status=unavailable`. Unavailable includes a reason such
as an absent paired module, missing exports or a rejected Begin request.
Measured `samples=0` is distinct from unavailable; a missing line qualifies
neither outcome. Existing lifecycle records remain available for correlation.

The summary contains the receiver's QPC window, separate QPC timestamps
immediately around the forwarded runtime call, and first/last sample timestamps
and thread IDs. Check these intervals: the observation window slightly encloses
the forwarding interval, so a positive count alone does not prove a sample
inside the runtime call. A first or last sample inside the forwarding interval
is direct evidence of service-point progress during that call. A zero count
means no service-point observation in the enclosing window. Mixed-thread and
saturation flags prevent treating an aggregate as one exact render caller's
unbounded count. The first Frontier result below records a measured zero.

## Desktop checks

CPU tests cover rejected Begin/End inputs, unchanged rejected snapshots,
measured zero, fresh tokens, 64-window exhaustion and coherent counts from
eight concurrent callers. A mixed-thread case starts and ends on the same
caller, ensuring intervening callers are still detected.

The actual paired-DLL fixture exhausts both normal census budgets before a
controlled fake runtime holds shutdown on a separate caller. The render caller
completes three successful owned Presents, a TEST Present, a failed Present and
a foreign-swapchain call through the real hook. The summary must report exactly
three samples within the forwarded call, with the actual render thread distinct
from the shutdown thread. A following shutdown without Present must report a
fresh measured zero. An existing observer rejects another Begin while leaving
normal forwarding and the original token intact.

Separate children exercise graphics not yet initialized, census disabled on the
graphics side, both census settings disabled, and a paired DLL that is present
on disk but has never been loaded. The absent case verifies that the lookup
does not load it. Fake-runtime counters check exactly one forwarded shutdown
per application call. All children use hidden WARP windows and an isolated
watchdog; no installed game or VR runtime is used by these tests.

The absolute-path full `build.bat` passed with exit 0. All six paired-DLL
children passed, along with the CPU census tests and existing regressions,
including 201 explicit-module checks, 207 bootstrap checks, 168 Present checks,
the five-case transport matrix and the 254-key configuration contract. All 471
source hashes remained unchanged throughout the build. Parent review tightened
both-export discovery and exception handling, then verified the actual summary
records through `tools/edvr_log.py`.

In the ready fixture, the three service-point observations occurred on render
thread 40284 inside the forwarded shutdown interval. The next window reported
zero samples with zero sample timestamps and thread IDs. A third, busy-window
attempt explicitly reported `status=unavailable reason=begin_rejected` while
still forwarding shutdown exactly once.

## Installed Frontier gate

The sanctioned installer installed both tested proxies and verified their
hashes on 2026-09-13 at 19:24:32 UTC. Graphics DLL SHA256 is
`9a7804c05dee3834debd46cd7f41a8cd08b5c5ba651782781edb1225db788281`; OpenVR
proxy SHA256 is
`c967cc96a6d17c1f2b4c864e3af94d22744384a0fd81804e50a713b371a2e75e`. Their
precommit version stamp is `v0.16.2-67-g10eee4c-dirty`; qualify the checkpoint
with these binary hashes and the frozen source record. The full build log,
exact binaries, fixture evidence, prior configuration, source hashes and
installation receipt are archived in
`build/frontier-shutdown-progress-20260913/`.

The existing root INI already enables census logging. Its SHA256 remained
`f9f68c8eb2ca63679f679f722988c12c0ca8bc30dc939ebd4da5ee87a82a9a10`; no separate
OpenVR-directory INI was present or created, and the original runtime DLL was
preserved. The installer kept both preceding DLLs in backups tagged
`openxr-shutdown-progress-20260913-132432`.

## Frontier result: no shutdown service-point progress

The user reported, "Ran it, seemed normal," on the established Frontier,
Pimax-through-SteamVR setup. Both installed DLL hashes still matched the
qualified `e4f4ef9` binaries. `tools/edvr_log.py --expect-build 10eee4c`
independently verified the precommit version of each new log:
`edvr_gfx_20260913_132808.log` and `edvr_vr_20260913_132809.log`. Frontier was
absent from the postflight process snapshot.

The shutdown observer was available and returned a valid measured zero. Call
10, on System thread 2836, has matching lifecycle begin/end records. Its
forwarded runtime interval is QPC `3923053192739 .. 3923053711015`; the
receiver's enclosing observation interval is `3923053192738 .. 3923053711020`.
The QPC frequency is 10,000,000 Hz, giving 51.8276 ms for forwarding and
51.8282 ms for observation. The summary records `status=measured reason=none
token=1 samples=0`, with zero sample timestamps and caller IDs and no
mixed-thread or saturation flags. There was no successful owned Present
reaching the native callback service point in this window.

Ruled out: treating the diagnostic's continuously pumped Present loop as a
proven Frontier shutdown contract, because this available observer recorded
zero service-point samples during the real forwarded shutdown. This does not
prove the render thread had exited, that no other graphics work occurred, or
that Present would remain absent during a longer native shutdown.

The graphics log confirms LiveCopy and precompiled temporal shader warmup. Its
last render-to-submit report has 8,684 valid samples and zero invalid samples
on render thread 10208. Those counters support normal instrumentation during
the run; they do not establish graphics inactivity at teardown.

The source and test-binary hashes still matched the qualification record. Both
logs, the user's report, parsed QPC evidence and the postflight configuration
are archived in
`build/frontier-shutdown-progress-20260913/flight-20260913-132808/`. The
original OpenVR runtime DLL is unchanged. The INI changed during this run only
at `fix.intro_video`, from `screen` to `skip`; its postflight SHA256 is
`9aea022b28529565735ac1ad2403ca4ac9dbcd886ffb0df658969ba7d2bb8f54`. Census
logging remained enabled. The postflight setting is preserved.

## Consequence for native teardown

`ModuleBackend::stop` in `src/openxr/native_module.cpp` first routes the final
GPU drain through `RenderRoute::invoke`, which queues it on the bound Present
caller. It later queues `shutdownAtRenderBoundary`, holding that caller inside
the callback while the XR owner destroys the session. The first queued request
can already time out if Present stops. The retained-generation fallback keeps
uncertain resources alive and blocks reinitialization; it is not successful
native cleanup.

The next integration work must establish a safe shutdown contract when the
application stops servicing Present. Absence of callbacks is not proof that the
game's immediate context is unused. Closing the callback lease alone does not
exclude later application graphics work. Direct cleanup on the System caller,
or merely increasing a timeout, does not supply that exclusion and must not
replace the qualified callback-held path without evidence. The earlier Pimax
session-destruction hang while Present continued remains relevant.

The immediate design question is whether Frontier's render caller has already
terminated or is still alive during shutdown, and what explicit graphics
exclusion is available in the latter case. Native game export/launch
integration remains open, with teardown no longer treated as qualified by the
standalone diagnostic's successful exit.
