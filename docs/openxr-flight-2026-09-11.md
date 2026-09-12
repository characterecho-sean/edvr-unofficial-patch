# Frontier OpenXR census flight, 2026-09-11

## Evidence identity

Both logs were located and build-checked with `tools/edvr_log.py --target
frontier --expect-build 070e49d`, separately for `--tag gfx` and `--tag vr`.

- Build: `v0.15.1-20-g070e49d`.
- Graphics log: `edvr_gfx_20260911_185819.log`, linked build `6AA4A22C`.
- VR log: `edvr_vr_20260911_185839.log`, linked build `6AA4A236`.
- Recorded window: approximately 18:58:19 through 19:01:21 local time.
- User reported the Frontier test complete. Headset, connection/runtime choice,
  visual observations and completion of individual test steps need the user's
  confirmation; the logs alone cannot establish them.

## Findings

The first published D3D11 device was `000001C39F68DA70`, created at
18:58:19.300 on thread `32292`, feature level `0xC000`, adapter LUID
`00000000:00013F22`. All 16 sampled eye texture observations reported that
exact device and `sameDevice=1`. The observed texture descriptors were
`2268x2240`, then `2708x2240`, format `27`, single sample, one mip and one
array slice. Both eyes used colour-space value `1`, default submit flags, and
initially null bounds. The width increase followed the existing cull guard's
stage-one request; resource identity also changed during later mode
transitions.

This supports using the published device on this capture. It does not prove all
configurations use the first device, and the resource probe starts only after
validation and stops after 16 observations.

The method census observed 19 of the 84 exact methods: 13 System, five
Compositor and one Chaperone. ExtendedDisplay was requested, but no method on
it was observed. All captured method entries were attributed to the game
executable. The observed Compositor methods were `SetTrackingSpace`,
`WaitGetPoses`, `Submit`, `PostPresentHandoff` and `SetSkyboxOverride`.
Chaperone's observed method was `SetSceneColor`. The System set includes render
size, raw/matrix projections, eye transforms, hidden mesh, properties, tracking
poses, event polling, connection/display queries and recentering.

The captured WaitGetPoses, Submit and graphics commands all used thread
`32292`; every recorded command context was immediate (`000001C39F74ACE8`). The
capture contains no deferred-context observation. It does not prove deferred
contexts are absent later in the session.

System calls are demonstrably multi-threaded. Render-size requests arrived on
`38776`, `35404` and `32292`; event polling and initial tracking-pose requests
arrived on `35404`. Recenter requests were observed on both `32292` and
`35404`. The first render-size request was at 18:58:39.831, before the first
Compositor acquisition at 18:58:42.586 and first WaitGetPoses at 18:58:42.626.
An owned backend must support initial geometry before the normal compositor
frame loop and synchronize these callers.

No logged census fault, exhausted wrapper cache, validation failure or
exception was found. Existing shader compiler warnings and seven transition
flash replacements were present; neither is evidence of a new census fault. The
logs have no explicit clean-exit acknowledgement, so do not infer complete
lifecycle qualification from their ending.

The graphics runtime detector initially announced Oculus at 18:58:39.052, then
corrected itself to EDVR OpenVR at 18:58:40.050. OpenVR calls and eye
submissions confirm the latter path was active. The VR export heuristic
classified the forwarded library as Valve at 18:58:42.589. That is not enough
to identify the actual OpenXR runtime or headset; record the user's runtime
choice separately.

## Capture limitation and next work

Ruled out: the first 64 process-startup Presents provide an overlapping VR
frame trace, because the last recorded PresentExit was QPC `2392303787701` and
the first WaitEnter was QPC `2392338920324`. There is no overlap.

The Present capture and most common GPU-command budgets were consumed by
startup/loading work before VR began. Increasing flight duration cannot fill
this gap. Graphics records also carry `frame=0`, so that field is not a shared
frame association. QPC provides ordering, not a common frame sequence.

Preserve the startup evidence and add a separate bounded capture window that
begins at the first compositor pose wait, with an acknowledged CPU-only signal
to the graphics DLL. Test that exhausting startup budgets cannot exhaust this
second window, and prove the signal reaches both DLLs in the desk harness. Do
not issue GPU queries from the signalling path. The single-context GPU bracket
remains gated on the corrected overlapping trace.

## Capture follow-up

The CPU census now retains separate startup and VR budgets. The first owned
compositor pose wait switches the OpenVR bank and requests a versioned,
acknowledged switch from the graphics DLL beside the game executable. A
missing, old, disabled or not-yet-initialized receiver is reported explicitly,
with retries bounded to 64 pose waits. Neither the bridge nor its receiver
loads a DLL, initializes graphics or submits GPU work. Repeated
acknowledgements cannot refill a bank. Enabling the census also keeps the
compositor observation hook installed when the optional rendering features are
off.

The full build passed with Frontier's original OpenVR DLL and the local DLSS
SDK. The regression harness runs the actual paired proxies in fresh child
processes with a concrete historical Compositor fixture and a hidden WARP
swapchain. It exhausts startup Present/clear samples before entering the real
WaitGetPoses hook, then verifies both logs: 64 samples per Present entry/exit
kind and 16 clears in each graphics phase, 64 wait entries/exits in the VR
phase, and exactly one acknowledged transition. Separate cases verify a late
graphics initialization succeeds on retry and a disabled graphics census never
acknowledges or produces graphics samples. CPU budget tests cover separate
events, saturation, repeated transition requests and concurrent claims.

The repeat launch below supplies the missing overlap. No new GPU timing
instrument or OpenXR backend is enabled by this follow-up.

## Repeat flight: build 2a56da3, 20:05 local

Both logs were retrieved with `tools/edvr_log.py --target frontier`, separately
for each tag, and passed `--expect-build 2a56da3`. Their version is
`v0.15.1-21-g2a56da3`: graphics `edvr_gfx_20260911_200520.log`, linked build
`6AA4A9B2`, and VR `edvr_vr_20260911_200539.log`, linked build `6AA4A9B9`. The
recorded window ends around 20:08:43. Sean confirmed that this run looked and
tracked normally. No explicit clean-exit log acknowledgement or named
headset/connection report is available here.

The capture correction passed its flight check. The OpenVR phase begins at QPC
`2432533427344`, the graphics phase at `2432533427631`, and the bridge
acknowledges on attempt one, all at 20:05:42.077. Startup Present samples
remain in their own bank; fresh Present samples overlap the VR waits and
submissions.

| VR-phase event | Samples | First QPC | Last QPC |
|---|---:|---:|---:|
| WaitEnter | 64 | 2432533427686 | 2432550325365 |
| WaitExit | 64 | 2432533428003 | 2432550426170 |
| SubmitEnter | 64 | 2432534128832 | 2432546879335 |
| SubmitExit | 64 | 2432534566408 | 2432546879779 |
| PresentEnter | 64 | 2432534941199 | 2432550439796 |
| PresentExit | 64 | 2432534943306 | 2432550441050 |

QPC sorting gives the same complete sequence in all 32 captured stereo pairs:
WaitEnter, WaitExit, left SubmitEnter/Exit, right SubmitEnter/Exit, then
PresentEnter/Exit, before the next WaitEnter. Each of those intervals contains
one Present and both distinct eyes. The remaining wait/Present samples outlast
the Submit budget; missing Submit lines there are not missing-eye evidence.
Every sampled graphics call, pose wait and submission uses thread `23688`.
Every sampled graphics command uses immediate context `000002B77EFAC1C8`;
Present identifies the owned swapchain `000002B7607F8AF0`.

The first sampled VR graphics command is ClearRtv, QPC `2432533430376`, after
the first WaitExit and before Submit. The VR phase also records ClearDsv,
CopyRegion, Copy, DrawInstanced, DrawIndexedInstanced, Dispatch, Update and
DispatchIndirect, each capped at 16. No deferred execution is observed. Common
command budgets expire within the first few frames; DispatchIndirect first
appears later. These per-kind budgets are not a continuous, complete GPU trace.

Ruled out: ending a timer at the second Submit includes every captured command
before the next pose wait, because the first pair's right SubmitExit is QPC
`2432534899359`, followed by Copy at `2432534899656` and eight CopyRegion calls
at `2432534947993` through `2432534948078`. Those CopyRegion calls are even
after PresentExit (`2432534943306`). Updates are also observed inside later
Present scopes. The graphics census does not identify which module requested
each command, so do not attribute these to mirror rendering without further
evidence.

The approved second-eye endpoint remains valid for an explicitly bounded
render-to-submit span, which excludes this later work. Moving the endpoint to
Present alone would still not include every observed command. A prototype must
state its exact coverage, publish only a CPU frame marker at the pose boundary,
and issue queries on the observed immediate-context execution path. This is
evidence for a guarded single-context prototype on this configuration, not a
guarantee about later frames, other contexts or other runtimes.

The first published device is `000002B77EDFFFE0`, created at 20:05:21.105 on
thread `23688`, feature level `0xC000`, adapter LUID `00000000:00013F22`. All
16 resource observations again match it, with both eyes and the same
`2268x2240` then `2708x2240`, format-27, single-sample descriptors. The ABI
census again observes 19 distinct methods (13 System, five Compositor, one
Chaperone), all attributed to the game executable. System callers include
threads `36728`, `28736` and `23688`; even an eye-to-head query is observed on
`28736` alongside frame-thread geometry calls. System snapshots must therefore
be safe for concurrent readers after startup as well.

No logged exception, census fault, sentinel trip, exhausted wrapper cache or
validation failure was found. Sean's normal visual/tracking report is separate
from performance qualification: the logs contain long-frame diagnostics, and
telemetry overhead still needs a matched enabled/disabled comparison. The
missing-overlap gate is now satisfied; no further flight is needed solely to
test the capture-bank correction.

## Later crash-record correction

Windows Application Error records and saved crash dumps show that the repeat
Frontier run ended with `0xc0000409`, fast-fail reason 7, at 20:08:44. The
faulting DLL timestamp `6AA4A9B2` matches the verified `2a56da3` capture. A
Steam run on `39b2eac` (`6AA4BBA5`) failed through the equivalent path at
20:52:46. These records were checked after Sean reported crashes.

Ruled out: a clean exit for the repeat capture, because its matching Windows
crash record reports an abort after the final log lines. The bounded ordering
and visual/tracking findings above still apply; shutdown qualification does
not.

The Frontier dump's `d3d11.dll+0x13bf4b` return address follows a call to
`terminate` in the menu panel worker's static destructor. Its instruction
sequence matches `menu_panel.obj`'s named `dynamic atexit destructor for g_w`,
including the test of the `std::thread` ID at worker offset `0xa0`. Windows has
already terminated other threads before process detach, but the static thread
object remains joinable and its destructor aborts. The settings writer has the
same lifetime hazard if it has been started.

The correction gives both worker states process-lifetime storage while
preserving their normal explicit shutdown/join paths. No join or mutex access
is added to process detach. The menu test now checks actual worker startup
followed by CRT exit in an isolated child, and explicit shutdown/restart in a
second child. Both child cases and the full build passed in
`build/menu-worker-exit-fix.log`; installation and a normal game exit still
need to be verified.

Separately, the new, unintegrated GPU desk harnesses reported test failures,
including an access violation. Their execution was stopped; none of those
adapter/shared-clock drafts was installed into either game directory.
