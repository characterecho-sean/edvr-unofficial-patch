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

A new Frontier launch is still required to collect the missing overlap. The
first minute through the menu and cockpit should be sufficient for this bounded
startup window; a longer flight does not extend the sample budgets. No new GPU
timing instrument or OpenXR backend is enabled by this follow-up.
