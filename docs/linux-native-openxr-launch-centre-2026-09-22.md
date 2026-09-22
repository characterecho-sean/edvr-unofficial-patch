# Linux/Proton native OpenXR startup failure (2026-09-22)

## Status

- State: a supporter's Elite worked in VR on Linux/Proton through 0.16.2
  (legacy OpenVR proxy chained to their real OpenVR runtime, e.g.
  OpenComposite) but gets no VR on 0.17.0, which replaced that proxy with
  EDVR's own native OpenXR runtime (`edvr-native-steamvr-only-goal`
  memory, DONE 2026-09-16). They report trying WiVRn, xrizer,
  OpenComposite and VapoR under Proton; only one attempt's logs are in
  hand (WiVRn — `edvr-logs-20260922-122318.zip`, all three log sets
  confirmed build-matched to this HEAD via
  `tools\edvr_log.py --expect-build HEAD`).
- Two launches (111010, 111049) died at `module_startup,
  graphics_unavailable=80070005` (E_ACCESSDENIED from
  `RenderBinding::acquire` in `native_module.cpp:40-48`) before the
  OpenXR instance/system was even created — no runtime/system/size lines
  follow. The third launch (111203) got past device acquisition, created
  a session against WiVRn (`runtime 'v26.2.3'`, `Oculus Quest2 on
  WiVRn`, correct 1832x2016/eye, 72 Hz), then aborted at the startup
  auto-recentre: `result,launch_centre_locate,-6` (`XR_ERROR_
  INITIALIZATION_FAILED`) inside `centreAtStartup`
  (`native_runtime_host.h:1618-1656`), which only tolerates
  `XR_ERROR_TIME_INVALID`/`XR_ERROR_POSE_INVALID` from that locate and
  aborts native startup (`VRInitError_Init_Internal`, code 124) on
  anything else. The whole native module then shuts down cleanly; the
  game itself is unaffected and keeps rendering flatscreen (gfx log
  shows normal frames, menu, loading panel afterward — this is not a
  game crash, VR just never comes up).
- CONFIRMED 2026-09-22 (flight, diagnostic branch
  `linux-openxr-launch-centre-diag`, `v0.17.0-1-g9693a8a`, same
  supporter/WiVRn/Quest2 rig): `XR_ERROR_INITIALIZATION_FAILED` comes
  from `xrConvertWin32PerformanceCounterToTimeKHR`, never
  `xrLocateSpace` — every `stage=` in the log, at startup and in 20700+
  consecutive per-frame `head_locate_failed` lines, reads `convert`.
  Ruled out: NOT transient. 100% failure rate from session start through
  the whole captured session (~31 s, 20700 calls), zero
  `head_locate_recovered` lines. The tolerance widening itself worked as
  designed — `centreAtStartup` waited 2008 ms/145 samples, hit
  `tracking_deadline`, and let native startup complete this time
  (`module_startup,...,result=0`; frames submit, `native_temporal`/
  `native_sharpen` engage with real projection matrices) — but
  `locateHead`, the per-frame OpenVR-facing pose query
  (`native_runtime_host.h`, `locateHead`), fails every single call
  through the same broken `convert()`. Net effect: VR now starts instead
  of aborting, but delivers no head tracking at all (fixed/frozen view)
  — a different failure, not a fix.
- Ruled out: not a game-side crash (gfx log runs fine after); not the
  E_ACCESSDENIED device-acquire flake (didn't recur this flight — only
  one launch attempt was needed, so still unresolved either way, still
  not worth its own arc).
- New angle: the main frame loop's own time source is NOT broken —
  `projection_query` lines in the same log carry real, sane matrices at
  sequence 146+, meaning whatever supplies `xrWaitFrame`'s
  `predictedDisplayTime` (or the view-locate that rides on it) works
  fine on this stack. Only `HeadLocator`'s on-demand path (`locateHead`,
  `resetSeated`, `applyIntroRecentre`, `centreAtStartup` — all four
  callers in `head_locator.h`/`native_runtime_host.h`) calls
  `xrConvertWin32PerformanceCounterToTimeKHR` to turn an arbitrary QPC
  sample into an `XrTime`. A real fix likely means sourcing that
  mapping from the frame loop's already-working `XrTime` instead (cache
  one `(predictedDisplayTime, QueryPerformanceCounter)` pair per frame,
  then convert any later QPC sample by elapsed-ticks arithmetic,
  bypassing the broken extension entirely) rather than calling
  `xrConvertWin32PerformanceCounterToTimeKHR` at all on this stack. Not
  investigated yet: where in the frame-loop code that `XrTime` is
  produced and whether it's reachable/cacheable for `HeadLocator`'s use.
- Next flight: none queued. Needs investigation into the frame loop's
  `XrTime` source before a real fix is shaped — reported to Sean for
  direction, not started this turn.

## Log evidence (2026-09-22)

Bundle `edvr-logs-20260922-122318.zip`, three launches in one session.
Full grep-only reading via `tools\edvr_log.py --dir`; no whole-file
reads of the 1528-line breadcrumbs file.

- Launch 1 (11:10:08 local): `module_startup,graphics_unavailable=
  80070005`. Launch 2 (11:10:49): same. Launch 3 (11:12:03): succeeded
  past device acquisition.
- Launch 3 openxr log (`edvr_openxr_20260922_111203_846_300.log`, full
  60 lines read directly — small enough): `steam_identity,...,app_id=
  359320`; `runtime, 'v26.2.3',0`; `system,vendor=0,max_size=
  16384x16384,name=Oculus Quest2 on WiVRn`; sizes correct; `device,
  adapter=00000000:000003f2,feature=b100`; session created, `display_
  frequency,hz=72`; all providers (menu/temporal/sharpen/features/
  timing) acquired; then `result,launch_centre_locate,-6` ->
  `present_park,...,result=124,...` -> `module_startup,...,result=124`
  -> full clean shutdown_stage sequence, `native_summary,waits=1,
  submits=0,pairs=0` (zero frames ever submitted to the runtime).
- Matching gfx log (`edvr_gfx_20260922_111202.log`) confirms: `vr
  runtime: EDVR's native OpenXR runtime` detected at 11:12:04.074,
  `native frame: begin #1` at .142, then normal flatscreen frames
  (`frame 728` by 11:12:11) — the game runs on, just without VR.
- `edvr_install_state.ini`: v0.17.0, installed 2026-09-22T16:22:59Z,
  matches build HEAD.

## Diagnostic flight (2026-09-22, 14:24 local / 18:24 UTC)

Build: `linux-openxr-launch-centre-diag` branch (off `v0.17.0`),
commit `9693a8a`, `git describe` = `v0.17.0-1-g9693a8a` — DLL reports
`v0.17.0-dirty (build 6AB2B91F)`. Same supporter, same WiVRn/Quest2 rig
(`v26.2.3`, `Oculus Quest2 on WiVRn`, 1832x2016/eye, 72 Hz). One launch
this time, no repeat of the E_ACCESSDENIED device-acquire flake.

`native_launch_centre,waiting_for_tracking=1,stage=convert,flags=0,
result=-6` at session start, then `tracking_deadline=1,samples=145,
elapsed_ms=2008` — the widened tolerance let `centreAtStartup` give up
gracefully instead of aborting. `runtime_startup,...,geometry_ready=1`
and `module_startup,...,result=0` follow: native startup completed.
`native_temporal`/`native_sharpen` engage `first_treated_eye=0` and `=1`
at sequence 146 with real `projection_query` matrices (`m00=0.930073`
etc., sane FOV numbers) — the render/submit side works.

Then `head_locate_failed,stage=convert,result=-6` fires at
`consecutive=1,2,3`, then every 300th call as designed
(300/600/900/.../20700), continuously for the rest of the captured
session, always `stage=convert`. No `head_locate_recovered` line
anywhere. `xrLocateSpace` itself was never reached in any of these
failures — `convert()` fails first, every time.

Reported to Sean; no code changed this turn. See Status block for the
new-angle hypothesis (source `XrTime` from the frame loop's own
prediction rather than calling the broken conversion extension).
