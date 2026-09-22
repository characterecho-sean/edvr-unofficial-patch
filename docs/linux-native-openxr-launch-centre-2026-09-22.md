# Linux/Proton native OpenXR startup failure (2026-09-22)

## Status

- State: a supporter's Elite worked in VR on Linux/Proton (WiVRn/Quest2)
  through 0.16.2 (legacy OpenVR proxy chained to their real OpenVR
  runtime) but got no VR on 0.17.0, which replaced that proxy with
  EDVR's own native OpenXR runtime (`edvr-native-steamvr-only-goal`
  memory, DONE 2026-09-16). Root cause CONFIRMED by flight (see journal):
  `xrConvertWin32PerformanceCounterToTimeKHR` fails 100% of the time on
  this stack (`XR_ERROR_INITIALIZATION_FAILED`), never `xrLocateSpace`,
  never recovering. A real fix is BUILT + PUSHED, NOT YET FLOWN:
  `linux-openxr-launch-centre-diag` branch (off `v0.17.0`), commit
  `e40dd73` (`v0.17.0-2-ge40dd73`) — bypasses that broken call using the
  frame loop's own already-working time source instead of widening
  tolerance around it. Zip sent to Sean for the supporter.
- The fix: `FrameBoundary` (`frame_boundary.h`) already calibrates a
  `(steady_clock, XrTime)` pair from every real `xrWaitFrame` result, for
  turbo mode's `synthesizedTime()` — proven working on this stack, since
  the same flight's `projection_query` lines carried real matrices.
  `FrameBoundary::estimateNow()` (new) exposes that calibration.
  `HeadLocator::locate` (`head_locator.h`) gained a `fallbackNow`
  parameter: tried ONLY when `xrConvertWin32PerformanceCounterToTimeKHR`
  itself fails, so a runtime where that call works (everything flown so
  far — SteamVR, Quest3, Pimax) sees no behavior change at all. All four
  callers (`locateHead`, `resetSeated`, `applyIntroRecentre`,
  `centreAtStartup` in `native_runtime_host.h`) now pass
  `boundary.estimateNow()`. Builds clean, all existing self-tests pass.
- Ruled out: not a game-side crash (gfx log runs fine when VR fails to
  start); not transient (100% failure rate across 20700+ calls in one
  session); not `xrLocateSpace` (never reached — `convert()` fails
  first, every time); the earlier tolerance-only patch (`9693a8a`)
  proved VR could start but left head tracking completely dead, which is
  why this second commit exists. E_ACCESSDENIED device-acquire flake
  from the first flight (two launches before WiVRn succeeded) didn't
  recur on the second flight — still unresolved either way, not worth
  its own arc yet.
- Next flight: the supporter needs to test `v0.17.0-2-ge40dd73`
  (zip already sent). Confirms or refutes: does VR now start AND track
  the head? Watch for `head_locate_failed` — should disappear entirely,
  or at least stop being 100% of calls, once `estimateNow()` is
  supplying `fallbackNow`.

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

## Fix: bypass convert() using the frame loop's own time (2026-09-22)

Traced where a working `XrTime` already exists on this stack.
`geometry_locator.h:31` locates the head for the main per-frame geometry
snapshot with `api.locateSpace(...,frame.predictedDisplayTime,...)` —
straight from the current `Frame`, no Win32 conversion. `session_state.h`
(`SessionState::predictedDisplayTime()`) mirrors that value but zeroes it
on every frame close (`clearFrame()`, line 278) — no good for
`centreAtStartup`/`resetSeated`/`applyIntroRecentre`/`locateHead`, all
four of which explicitly run *between* frames.

`FrameBoundary` (owns the actual `xrWaitFrame` calls) turned out to
already solve this for a different reason: `noteReal()` records
`lastReal_` (an `XrTime`) and `lastRealAt_` (a `std::chrono::steady_clock`
timestamp) on every real, non-synthesized wait result, purely to support
turbo-mode's `synthesizedTime()` prediction. Critically, `lastReal_`/
`lastRealAt_` are never cleared by `clear()`/`finish()` — they persist
across frame boundaries, unlike `SessionState`'s own copy. That is
exactly the calibration needed: a `(wall-clock, XrTime)` anchor that
survives between frames and lets any later instant be estimated by
elapsed-time arithmetic, no runtime call required.

Added `FrameBoundary::estimateNow()`: returns `lastReal_ + elapsed since
lastRealAt_` (0 if no real frame observed yet — `canSynthesize()`
false). Gave `HeadLocator::locate` a `fallbackNow` parameter, used only
when `xrConvertWin32PerformanceCounterToTimeKHR` itself returns non-
success (checked via the existing `stage` tracking from the prior
commit). All four call sites in `native_runtime_host.h` now pass
`boundary.estimateNow()`. On any runtime where `convert()` succeeds
(everything flown to date), `fallbackNow` is computed but never used —
zero behavior change there.

`centreAtStartup` calls this before the first `waitPoses()`... no —
confirmed order: `start()`'s loop calls `waitPoses()` (which drives
`boundary.waitAndBegin()`, populating the calibration) *before*
`centreAtStartup` runs each iteration, so by the time `centreAtStartup`
needs `estimateNow()`, at least one real frame has already been
observed. Matches the flight log: `waiting_for_tracking=1` was already
`launchCentreSamples==1` (first attempt), which happens after the first
successful wait.

Built, all `build.bat` gates and self-tests green (`vtable_test`,
`openxr_module_test`, `openxr_native_tests`, config contract, etc.).
Not flown. Zip (`edvr-linux-openxr-diag-v0.17.0-2-ge40dd73.zip`) sent to
Sean for the supporter; branch pushed as `e40dd73` on
`linux-openxr-launch-centre-diag`.
