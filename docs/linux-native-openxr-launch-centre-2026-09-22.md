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
- Open hypothesis: `XR_ERROR_INITIALIZATION_FAILED` comes from either
  `xrConvertWin32PerformanceCounterToTimeKHR` or `xrLocateSpace` inside
  `HeadLocator::locate` (`head_locator.h:21-49`) — the log can't
  currently tell which, since both failure paths return the same
  `lastResetResult` up through `centreAtStartup`. Leaning towards the
  Win32 time-conversion extension: it is Windows-specific, only reached
  through whatever Wine/Proton-side shim bridges it to a Linux-native
  runtime, and it is new exposure for EDVR's OpenXR-native path — 0.16.2
  never called it because the legacy proxy just forwarded to the user's
  own real `openvr_api.dll` (OpenComposite) and never spoke OpenXR
  itself. `centreAtStartup`'s tolerance list was set deliberately narrow
  during the 2026-09-12 port work (`openxr-compositor-interface-
  2026-09-12.md:46-48`: "other unexpected locate results stop the
  diagnostic conservatively") against SteamVR/Quest3/Pimax native
  OpenXR, which apparently never produced this code.
- Ruled out: not a game-side crash (gfx log runs fine after); not the
  E_ACCESSDENIED device-acquire flake (that resolved itself by the third
  launch and is a separate, lower-priority issue — possibly a Proton/GPU
  handoff race worth a one-line memory note if it recurs, not yet worth
  its own arc).
- Next flight: needs a decision from Sean before any build goes back to
  the supporter (see chat) — either add trace to disambiguate convert()
  vs locate() inside `HeadLocator::locate`, or widen
  `centreAtStartup`'s tolerance list to also treat
  `XR_ERROR_INITIALIZATION_FAILED` like `TIME_INVALID`/`POSE_INVALID`
  (self-expires after 2 s via the existing `launchCentre` wait/expire
  path — `native_runtime_host.h:1635`) and fly that directly. No code
  changed yet.

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
