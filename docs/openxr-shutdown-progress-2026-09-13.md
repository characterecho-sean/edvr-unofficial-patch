# Frontier shutdown Present progress

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
unbounded count. No progress result has yet been recorded from Frontier.

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

The Frontier flight is pending. Use Pimax through SteamVR, enter the cockpit or
go on foot, then exit normally. Check startup, tracking and exit as well as
both DLL hashes and the new shutdown summary. Native game launch remains gated
on this evidence and the remaining export and launch integration.
