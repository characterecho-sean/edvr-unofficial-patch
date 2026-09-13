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
includes the preflight configuration snapshot and install receipt. The Frontier
flight is pending.

Use the established Frontier installation with Pimax through SteamVR. Run a
short normal session and exit normally; no standalone grid/triangle test is
needed for this observer. Verify both new logs against the build's precommit
version before interpreting shutdown. Record the user's visual/exit result, the
paired measured summary and begin/end render records, installed hashes and any
configuration changes.

Native teardown still first queues a GPU drain and then a callback-held cleanup
request on the Present caller. Zero progress can prevent either request from
being serviced. The new observations will guide a safe contract for stopped
application rendering; they do not authorize direct cleanup on the System
caller or replace the previously qualified callback-held path. Native Frontier
export/launch integration remains open.
