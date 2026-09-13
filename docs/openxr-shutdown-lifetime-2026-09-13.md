# Frontier render-caller lifetime at shutdown

The [previous Frontier flight](openxr-shutdown-progress-2026-09-13.md) looked
normal and measured zero successful owned Presents reaching the native callback
service point during the 51.8276 ms forwarded shutdown. That result rules out
assuming the continuously pumped diagnostic loop describes Frontier shutdown.
It does not distinguish an exited render caller from one that remains alive
outside Present or inside an unfinished Present.

This checkpoint extends the passive shipping OpenVR census to distinguish those
cases in one Frontier flight. It does not change native teardown or qualify use
of the application's immediate context from another thread.

## Observation contract

With `advanced.openvr_census` enabled, the graphics proxy retains a real,
synchronization-only handle to the first successful, non-TEST owned Present
caller. It duplicates that caller's current-thread handle while executing on
the caller. It never reopens the thread by ID or replaces the retained handle,
so ID reuse cannot make an exited original caller appear alive. Subsequent
observed owned Presents on another caller, or after the original handle
signals, set a sticky owner-change flag.

A stack scope records entry and exit around each owned hooked Present,
including the real Present, graphics frame work and native callback. These
cumulative counts include failed and TEST Presents; foreign swapchains are
excluded. The existing service-point count still includes only successful,
non-TEST owned Presents reaching the point immediately before
`renderBoundaryPresent`. Entry, exit and service timestamps share the
observer's short SRW lock with window snapshots. The service timestamp and
active token are read under that same lock, avoiding a sample being omitted
from a newly opened window.

Begin and End poll the retained thread handle with a zero timeout and copy the
lifetime/activity state. These observations neither wait for the thread nor
suspend it. No lock is held across real Present, runtime shutdown, graphics
work or a native callback. When the census is disabled, these new hook calls do
no observer work. Unlike the first probe, an enabled census now records
activity throughout the run, including outside an active shutdown window.

The paired bridge is version 2 with an exact 232-byte snapshot. Both proxies
must be installed together. Rejected versions, sizes and tokens leave caller
storage and the active window untouched. Existing 64-window limits, module
lifetime protection and exactly-once runtime forwarding remain in place.

## Discriminating evidence

Each measured `VR shutdown Present census:` summary is followed by two `VR
shutdown render census:` records sharing its call ID, with `phase=begin` and
`phase=end`. They contain the original owner ID, state and error, the
owner-change flag, active/entered/exited Present counts, last
entry/exit/service QPC timestamps, last entry/exit caller IDs and an
activity-invalid flag. An unavailable observation does not emit invented render
state. Missing records are not a measured zero.

| Hypothesis | Discriminating record | Limit |
|---|---|---|
| The original render caller has exited | `owner_state=exited`, unchanged owner, `active_present=0` and valid balanced counts | Proves the retained thread terminated; does not prove every immediate-context user is gone. |
| The original caller remains alive outside Present | `owner_state=alive`, `active_present=0`, valid balanced counts | The caller may still issue graphics work or enter Present later. |
| An owned Present is still in progress | `owner_state=alive`, `active_present>0`, valid entry/exit difference | May be inside real Present, framework work or a callback; this probe does not identify which. |
| Ownership or activity cannot be established | Unobserved/unavailable state, nonzero error, changed owner or invalid activity | No cleanup qualification can be inferred. |

The begin and end samples can differ. An alive sample is only a point-in-time
observation, while a signaled retained thread handle establishes termination of
that original thread. Correlate activity timestamps with the separate receiver
and forwarded-call QPC intervals rather than treating the entire shutdown
interval as one unchanged state.

## Desktop qualification

CPU tests cover live and exited original callers, activity crossing a window,
owner changes on unsuccessful/TEST-style activity, unchanged rejected v1
snapshots, stale tokens, concurrent samples and window exhaustion. Repeated
exited-thread cases check process handle counts before and after observer
destruction.

The actual paired-DLL fixture exhausts the startup and normal VR budgets before
shutdown. Its positive window counts exactly three service points but five
whole owned Presents: three successful calls, one TEST and one failed call. A
foreign call is excluded. Further windows distinguish a live idle caller from a
caller held inside the real graphics callback, both with zero service-point
progress. Another child joins its render thread before shutdown and must report
the retained owner as exited with no active Present. Fake-runtime counters
verify normal forwarding exactly once for each call.

The remaining children preserve unavailable/disabled behavior and application
render-to-submit pixels with application GPU timing enabled and disabled while
both census settings are off. All seven children use isolated desktop fixtures;
no headset or installed game is used. Parent review checked the actual hook
boundaries, handle lifetime, paired version contract, rejected snapshots and
log correlation before the full build.

The absolute-path full `build.bat` completed with exit 0, including all seven
paired-DLL children, the CPU census tests, 201 explicit-module checks, 207
bootstrap checks, 168 Present checks, the five-case transport matrix and the
254-key configuration contract. All 471 source hashes remained unchanged during
the build.

Parent inspection through `tools/edvr_log.py --expect-build 00df52c` verified
the actual paired fixture logs as `v0.16.2-69-g00df52c-dirty`. The ready child
records render caller 15088 with 210 entries/exits before the positive window
and 215 afterward, with three service samples. Its next window is alive with
zero active Presents; the held-callback window is alive with one active Present
and counts 216/215. Both have zero service samples. The exited child records
original caller 16468 as exited with 210 balanced entries/exits, no active
Present and zero service samples. All records have clear
owner-change/error/activity-invalid flags.

The build log, all 471 source hashes, five binary hashes and byte-exact
ready/exited paired logs are archived in
`build/frontier-shutdown-lifetime-20260913/qualification.json` and its sibling
files. The binaries carry the precommit version above; the qualification record
links them to the checkpoint commit after it is pushed.

## Frontier gate

Both shipping proxies were installed through `tools/install_edvr.py` and
independently verified against the qualified build. The graphics SHA256 is
`0561a85a5a904f8278554810875a06b5a568cfbc0e398a9c42865c7b3e6948eb`; the VR
SHA256 is `76f8b09fa3a5a7647ac37d91514d6f3e1eed84f69b9ae7df1d9f2dc6548f9b92`.
The live INI is byte-unchanged at
`9aea022b28529565735ac1ad2403ca4ac9dbcd886ffb0df658969ba7d2bb8f54`, retaining
`fix.intro_video = skip` and enabled census logging. The VR-subdirectory INI
remains absent and the original OpenVR runtime is unchanged. The archive
includes the preflight configuration snapshot and install receipt. The
completed Frontier flight is recorded below.

Use the established Frontier installation with Pimax through SteamVR. Run a
short normal session and exit normally; no standalone grid/triangle test is
needed for this observer. Verify both new logs against the build's precommit
version before interpreting shutdown. Record the user's visual/exit result, the
paired measured summary and begin/end render records, installed hashes and any
configuration changes.

## Frontier result

The user reported "Ran it" for the `2f051db` installation. Visual/tracking and
crash-free exit confirmation was requested separately and is pending; it is not
inferred from that report. Both installed DLL hashes still match the qualified
binaries. `tools/edvr_log.py --expect-build 00df52c` independently verified
`edvr_gfx_20260913_140138.log` and `edvr_vr_20260913_140139.log` against the
precommit build. The postflight process snapshot contains no
`EliteDangerous64.exe`.

Call 10 on System thread 35024 has matching `VR_ShutdownInternal` begin/end
records. The forwarded interval is QPC `3942454444706 .. 3942454959663`,
enclosed by observation interval `3942454444705 .. 3942454959677`. At
10,000,000 Hz these are 51.4957 ms and 51.4972 ms. The observer reports
`status=measured reason=none samples=0`, with zero sample timestamps/thread IDs
and clear mixed-thread/saturation flags.

Both render snapshots identify the original caller as thread 19820,
`owner_state=alive`, with zero owner error, owner change and invalid activity.
Both have `active_present=0` and identical `entries=5785 exits=5785`. The last
Present entered at QPC 3942454440958, reached the callback service point at
3942454441645 and exited at 3942454441646, just 0.3060 ms before forwarding
shutdown. No owned hooked Present was active at either endpoint, and none
entered or exited during the observation window.

Ruled out: the original observed render caller having already terminated, or an
owned hooked Present being in progress, during this measured forwarded shutdown
window. The retained handle was alive at both snapshots and the valid activity
counts remained balanced and unchanged. This is the live caller outside Present
case. It does not establish what that caller was doing outside the hook,
whether another thread used the immediate context, or whether a longer native
shutdown would receive another Present.

The final graphics timing report has 2,537 valid render-to-submit samples and
zero invalid samples on thread 19820; those session counters do not establish
graphics inactivity at shutdown. The live INI is byte-unchanged, the
VR-subdirectory INI remains absent and the original OpenVR runtime is
preserved. All 471 source hashes and the qualified binary hashes still match.
Byte-exact paired logs, parsed QPC/activity evidence, the postflight INI and
process snapshot are archived in
`build/frontier-shutdown-lifetime-20260913/flight-20260913-140138/`.

## Consequence for native teardown

Native teardown still first queues a GPU drain and then a callback-held cleanup
request on the Present caller. Zero progress can prevent either request from
being serviced. The new observations will guide a safe contract for stopped
application rendering; they do not authorize direct cleanup on the System
caller or replace the previously qualified callback-held path. Native Frontier
export/launch integration remains open.

The current paired APIs do not supply the missing exclusion:
`src/common/render_boundary.h` explicitly requires the host to serialize
context use, and closing its lease only prevents new callbacks.
`NativeGraphicsClient` publishes references without granting exclusive context
ownership. `PresentQuiescence` requires the application's render loop to
acknowledge a permanent stop; the Frontier hook cannot manufacture that
acknowledgement from an empty observation window.

The subsequent [stopped-Present desktop
checkpoint](openxr-stopped-present-2026-09-13.md) keeps the application render
caller alive outside Present while a separate System caller requests the shared
native shutdown sequence. It covers the first queued drain and final cleanup
callback, cancellation, retained references and later Present service through
the actual graphics hook, with controlled operations in place of the OpenXR
runtime. The earlier CPU `renderShutdownDeadline` test covers only the final
callback helper. Any cleanup extension still needs an explicit guarantee
against concurrent or subsequent application context use; a thread-state
snapshot, a longer timeout or callback closure alone does not provide it.
