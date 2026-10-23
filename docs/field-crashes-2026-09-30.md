# Field crashes: reports from users' rigs

Crash reports from users' rigs, one dated entry per report. Read the Status
block first.

## Status

- **Removed 2026-10-08 (branch claude/remove-settlement-detail):**
  `fix.settlement_detail` no longer exists, so the "set it to game" steps
  below are moot and an old line is carried over as a retired setting.
- **State (2026-09-30, user 5):** open Windows 10/WMR report on v0.18.0-rc.4,
  RTX 3080 10 GB, DLSS K, HMD quality 0.85, UI quality 100; 3/4 sessions
  crashed. Cases include a panel-creation game AV, a menu
  `DEVICE_HUNG`, and a later NVIDIA shader-compiler AV. Ruled out: RTV/DSV
  size mismatch and game-sized EDVR shader loops. Floor test and follow-up
  asks are in that entry below; no result reported yet.
- **State (2026-10-17, anonymous reporter):** separate OpenXR/SteamVR
  report on v0.20.0-rc.2, RTX 4080 SUPER 16 GB, 144 Hz, per-eye input
  1622², output 3244², UI 4055² at quality 125 in the first flight;
  `fix.temporal_aa=dlss`, model K,
  optimized stations and witchspace stars on, panel curvature 0.2. Driver,
  Windows build and DLSS DLL version unknown. At 11:29:15.331 the game got
  `DEVICE_REMOVED` (`0x887A0005`) and removal reason `DEVICE_HUNG`
  (`0x887A0006`); game fatal-handler breadcrumb followed. Evidence and
  limits are in the dated entry. No C++ fix is supported.
- **Second flight (2026-10-17):** same release and DLSS, with only
  `fix.ui_quality` reduced from 125 to 100 (UI layer 3244²). Near a planet,
  Elite likely reached the same fatal HRESULT path at about 11:45:36.075 local.
  Raw stack values suggest device loss, but this run has no refused-create
  line or removal reason: `DEVICE_HUNG` is unconfirmed. Planet patch motion
  was gated off in completed windows through 11:45:32; the final seconds
  are unreported. The short sentinel-recovery launch is not a control.
- **Next for 2026-10-17:** request driver, Windows build, DLSS DLL version,
  and Event Viewer/Reliability Monitor evidence around 15:29:15 and
  15:45:36 UTC. First discriminating test: same release DLLs, route,
  resolution, HMD quality and UI quality 100; switch only
  `fix.temporal_aa=fsr`.
  Count it as valid only with `temporal aa: fsr 3.1.2 engaged` and the
  1622²→3244² sizing in the log. A repeat of the fatal path with FSR
  weakens NGX as a necessary cause; one clean leg weakly supports NGX.
- **Next for 2026-09-30:** request the listed OS/driver and event evidence,
  then follow the floor and staged FSR/DLSS/UI tests below.

## 2026-09-30, user 5

Bundle edvr-logs-20260930-174501.zip; local time is UTC+1. Session A ran
16:55:42-16:56:55 and left no log. B ran 16:57:32-17:17:13 and exited
clean. C ran 17:17:28-17:17:51. D ran 17:18:08-17:23:20.

### C and D

C is an exception-type reset, not a 2 s TDR overrun. The GPU was still
completing work at 17:17:51.056 (`native metrics history ... producer
7.349 ms (11 samples)`). The post-present stall ran from about 51.09
(runtime `seq=724 post_present=281.489`). The game's CreateBuffer, an
immutable structured SRV of 285 KB, was refused at 51.373 with
DEVICE_HUNG: under 0.4 s end to end, with the GPU at 6 ms a frame. The
same event chain ran in D at 17:18:27.59-29.12 and survived. The game
device got HUNG, not RESET, which device_hook.cpp:860-863 reads as this
device being the culprit rather than another process.

D's compiler crash comes first; the freezes are aftermath. Breadcrumb
tick 87633656 = 17:23:20.06, anchored on benchmark tick 87606906 =
17:22:53.313 (the same method puts C's abort 4 ms after its REFUSED
line). The first 616 ms stall (`seq=17511`, logged 16:23:20.724 UTC)
began about 40 ms later, and the process kept logging until
16:23:21.456. There were two equal ~600 ms freezes, GPU timers were
valid to 17:23:17.9, and there was no REFUSED line. EDVR's filter
chains to Elite's (proxy.cpp:348), which ends the process, so this is
not a TDR consequence. Caveats: the alignment is +-20 ms, and the
unwind stops at 8 frames, so thread 0x12A4 (a driver worker, or a game
thread inside CreatePixelShader) is unknown. Decisive check: no Display
4101 or nvlddmkm event at 17:23:20 (ask 1 below).

### Causes

The numbers rank the causes in this report's table below.

| # | Cause | For and against | Explains |
|---|---|---|---|
| 1 | Platform: Windows 10 WDDM 2.x, WMR and the NVIDIA driver (HAGS, hardware margin) | For: three signatures in 28 min (A game AV read -1 at +0x4F96D9; C fault; D nvgpucomp64 AV read -1); C is stochastic. EDVR has no OS branch, so its GPU design meets a stack the dev never flew (Win11 26200; Quest/Pimax/VDXR; WMR left Win11 24H2). Against: the user reports stock clocks and "only with EDVR" (their baseline used a different VR path: SteamVR's OpenVR, not WMR's OpenXR). | C, D, maybe A |
| 2 | NGX/DLSS preset K on Ampere (dlaa.cpp:436-498, 695-745; DLSS 310.9.1, tools/fetch_ngx.py:44) | For: the only CUDA-side GPU code; C made features twice (36.95 DLAA 2048x2556; 40.76 1740x2172->2048x2556, "Quality" at ratio 0.85), first evaluation 42.5 ms. Against: 0 `dlaa: NVIDIA evaluation failed` lines; 5 min clean in D; C's fault came 10 s after the last create; inputs are EDVR-owned textures (temporal_pass.cpp:2832). | C |
| 3 | EDVR shader variants against the NVIDIA compiler | For: D's thread is a driver compile; EDVR creates shaders lazily (ui_depth_sprite_ps 17:23:08.001, particle_vs 17:18:31.741, holo-remap PS 17:19:10.748, ~15 engine-motion PS) and sends game pixel shaders to other render-target formats and blends; a new scene at 17:23:15-19.7 (2119 draws) means first-use variants. Against: all of these ran minutes earlier. | D |
| 4 | Depth probe | For: vscreen.cpp:2895-2901 unbinds all RTV/DSV inside the game's OMSetRenderTargets; depth_probe.cpp:937-1012 makes a depth SRV, one dispatch, and a CopyResource into an owned typeless texture re-made per new size (422-444), and restores CS slot 0 only (374-416). Last samples (seconds past 17:17 for C, 17:23 for D): C 50.696 #2 256x256, 50.707 #5 1740x2172 (stall 51.09); D 19.404-19.732 #6/#7 2048x2556, #9, #10 (crash 20.06). Against: the readback is DO_NOT_WAIT (1327); the HLSL is clamped (fixed_extra_shader_source.h:471-481); MSAA, unknown formats and no-SRV cases are skipped (944-967, 990); the same bursts were harmless at every start and resize; the results only feed the census. No key isolates it (line 522). | C and D, by timing only |
| 5 | Code patches in game code | A's unwind (+0x288936A, +0x4F97AE) sits in the same panel-RTT creation code region as the `sizing chain` stacks; the user's ini has fix.settlement_detail = auto, which keeps CodeHooks in game LOD code. | A |
| 6a | VRAM, 10 GB | For: at 17:22:31.363 the game made 162 textures (2737 MB) in one frame (HMD 1.25); NGX and WMR memory are unlogged; no VRAM figure in any log (F8 Monitor page only, perf_monitor.cpp:376). Against: C was at the menu; no paging spikes before D's crash. | D, weakly |
| 6b | Two-device keyed-mutex transfer (shared_texture_transfer.cpp:252-321; 100 ms timeouts, eye_capture.cpp:203-217; a timeout drops the frame, no fault state) | For: WDDM 2.x and HAGS never flown; C's resize parked submits 73/38 ms (seq=135); the producer copy max was 25.1 ms in D's last window (earlier <= 1.0). Against: no timeout event, acquire p99 <= 0.25 ms, C's last submits 0.25 ms when the stall began. | C, weakly |

### Ruled out

Ruled out: an RTV/DSV mismatch (HDR HUD 2560x3195 against DS 2048x2556,
17:22:41.838), because ui_layer.cpp:1529-1534 binds a layer DS only at
layer size, else seedLayerDepth re-makes it (1086); the line shows an
unused stale DS; and the resize ran 17:22:31-53 while the crash came at
17:23:20.

Ruled out: GPU work sized by game data, because the embedded HLSL loop
bounds are constant or set by EDVR (ui_resolve.h:79, fss_panel_vs.h:143);
the one game-fed bound, fixed_shader_source.h:487, is the game's own march
count.

EDVR CPU corruption (A and D both read -1) cannot be tested without a
dump.

### Windows 10

src has no GPU-priority, D3DKMT, HAGS, OS/WDDM-version, waitable-timer or
timeBeginPeriod use (0 hits). The only priority call is CPU
SetThreadPriority(HIGHEST) on the XR owner thread (native_trace.h:95).
Every interface used exists on Windows 10. So only the stack under
hypotheses 1 and 6 can differ: HAGS, WDDM 2.x shared-fence waits with two
submitting devices (frame_end_overlap=on in `Openvr\win64\edvr_openxr.ini`
overlaps xrEndFrame with the next frame), WMR's compositor, and the
Windows 10 driver branch. None of these shows in the logs.

### Bisect legs

One sitting, stop at the first crash. A clean leg is 20-25 min with no
`UNHANDLED`/`REFUSED`, `alive` crumbs every 30 s, and `gfx: process exit`.

- Leg 0, the floor: fix.temporal_aa off, fix.ui_quality off,
  fix.settlement_detail game, set in edvr.ini before launch (the panel
  patch lingers until panels re-lay). This drops NGX, the probe, the
  engine-motion shader patches and their CodeHooks, UI resolve, the UI
  layer, the holo remap and the panel patch. It is honoured if the log has
  no `depth probe:`, `dlaa:`, `CodeHook kinematic-` or `engine-primary-`
  lines and does have `ui quality: off` (ui_layer.cpp:2550). A crash here
  means not EDVR's GPU passes (hypotheses 1 and 6): `REFUSED ...
  DEVICE_HUNG` is a fault; `UNHANDLED ... nvgpucomp64/nvwgf2umx` is the
  driver. Then 0b: `Openvr\win64\edvr_openxr.ini` frame_end_overlap=off
  and frame_thread_priority=normal (an all-or-nothing file: keep
  version=1, loader, graphics, runtime=system, separate_device=1). Then
  0c: toggle HAGS and reboot. Then an EDVR-free control on the same route.
- Leg 1: F8 fix.temporal_aa = fsr (log `menu: fix.temporal_aa off -> fsr`,
  menu.cpp:2412). Adds the probe, the shader patches, UI resolve and FSR,
  no NGX. A crash points at hypotheses 3 and 4 (a `depth probe: target`
  line in the last second; `nvgpucomp64` means a patched PS) and clears
  NGX.
- Leg 2: fix.temporal_aa = dlss (preset K). A crash points at hypothesis
  2 (watch for `dlaa: NVIDIA evaluation failed ... GetDeviceRemovedReason`,
  dlaa.cpp:733); then try temporal_aa_model = j.
- Leg 3: fix.ui_quality = 100. A crash points at the UI layer, the HDR
  HUD, the holo remap or the panel patch. If every leg is clean, repeat at
  HMD 1.0, noting F8 Monitor VRAM before each transition (hypothesis 6).

### Asks for the user

1. Event Viewer, System log, 30 Sep at 17:17:51 and 17:23:20 local:
   Display 4101, nvlddmkm (13/14, "Graphics Exception"), Kernel-Power 41,
   any WHEA-Logger; Reliability Monitor LiveKernelEvent 141/117.
2. Exact NVIDIA driver, winver build, dxdiag Driver Model (WDDM version),
   HAGS on or off, any TdrDelay override, WMR Portal version, and the
   nvngx_dlss.dll version beside the game (the NVIDIA App's "DLSS
   override" can swap it).
3. Overlays: NVIDIA overlay/ShadowPlay, Game Bar, Afterburner/RTSS,
   Discord, OBS, Steam overlay.
4. F8 Monitor VRAM used/budget at the menu and in the hangar.
5. Dumps: Elite's filter ends the process before WER files anything, so
   use `procdump64 -accepteula -ma -e -w EliteDangerous64.exe C:\dumps`;
   for TDRs also `C:\Windows\LiveKernelReports\WATCHDOG`.
6. Was session B's ini identical, do older logs exist, and how did the
   EDVR-free control run (SteamVR/WMR, which HMD Quality)?

## 2026-10-17, anonymous reporter

Archive `edvr-logs-20261017-113038.zip`; extracted analysis is in
`analysis/hyperspace-crash-2026-10-17`. The report's 11:29 local time is
15:29:15 UTC; do not reinterpret it using the maintainer's timezone. The
game log identifies SteamVR/OpenXR runtime `563027262832644`, headset
`SteamVR/OpenXR : cv` (model unknown), 144 Hz, 1622² per-eye input, 3244²
output and 4055² UI. The reported GPU is RTX 4080 SUPER 16 GB. Settings:
`fix.temporal_aa=dlss`, model K, optimized stations and witchspace stars on, panel
curvature 0.2, UI quality 125. Windows/driver/DLSS DLL versions were not
recorded.

Actual release identity is v0.20.0-rc.2, PE `6AD165AB`, linked
2026-10-15 23:45:47 UTC, tag commit `f2b99bdf`. `edvr_log.py
--expect-build HEAD` accepts the bare tag, but this is not current HEAD
at analysis checkout `fbb391a2`. The latest breadcrumb at tick 90268468 is this report's final
crash; earlier records in that file belong to historical sessions and must
not be combined with it.

At 11:29:15.331, CreateShaderResourceView failed for view format 21,
dimension 4 (`D3D11_SRV_DIMENSION_TEXTURE2D`),
over the 1622² resource in format 19 with bind `0x48`; an 8-byte
CreateBuffer also failed. Both report `DEVICE_REMOVED` `0x887A0005`, and
the subsequent removal-reason query reports `DEVICE_HUNG` `0x887A0006`.
The wrappers forward unchanged and log after failure
(`device_hook.cpp:970-1065`), so these are witnesses after device loss,
not causal descriptors; the depth formats are compatible. The latest
game breadcrumb is C0000005 write at zero in Elite's fatal handler
`+0x34C9322`, with stack `+0x48C1CF0`, `+0x489F868`, `+0x5216CC`,
`+0x50EC00`, etc. Treat that abort as aftermath of device failure, not an
original CPU null-write diagnosis.

Last reported VRAM was 8135/15279 MB (53.2%) at 11:29:13.434. The last
depth-probe readback sampling line was at 11:29:15.284; the last luma-probe
line reporting DLSS output was at 11:29:14.820.
Neither timestamps the GPU's precise last progress. Recent normal logs do
not expose hung duration. No evidence establishes a 2-second TDR or
GPU overload, and one VRAM sample cannot fully exclude pressure.
Microsoft defines [`DXGI_ERROR_DEVICE_HUNG`](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-error)
as badly formed command submission; the offending command is unknown and
EDVR is not exonerated.

The last logged transition hold began at 11:26:52.890/.896, over two
minutes before the crash; no hold appears close to it. `StartJump` and
`FSDJump` silently arm/clear state (`journal_watch.cpp:359-390`), so the
absence of a jump line proves nothing about a jump. Report timing is the
user's description only. Ruled out: treating the fatal-handler null write
as the original CPU bug, because it follows device failure. Ruled out:
an immediate OOM API error, which was not observed; memory pressure is
not fully ruled out.

Open hypotheses are NGX-specific failure, a common EDVR motion/UI GPU
path, or game/driver/runtime interaction. First test: keep the same release
DLLs, resolution, HMD/UI quality and other settings, and switch only
`fix.temporal_aa` to FSR. The release's native temporal path uses the same
DLSS/FSR sizing (`native_temporal` lines 170-173). A valid A/B must log
`temporal aa: fsr 3.1.2 engaged` and 1622²→3244²; fallback text saying FSR
was asked for but its own history was used is invalid. If `DEVICE_HUNG`
recurs with engaged FSR, NGX is not necessary. A clean FSR leg is only
weak support for NGX; then isolate the common path in a later test.

Ask the reporter for the NVIDIA driver, Windows build, DLSS DLL version,
and Event Viewer Display/nvlddmkm plus Reliability Monitor LiveKernelEvent
141/117 at 17 October 11:29:15 local (15:29:15 UTC). The new star fix
`fd84c7ac` is not a crash fix; launch fixes in `769c1a7f` have an unrelated
signature.

## 2026-10-17, anonymous reporter: second flight near a planet

Archive `edvr-logs-20261017-114758.zip` is extracted at
`analysis/planet-crash-2026-10-17-114758`. The flight is graphics log
`edvr_gfx_20261017_113449.log` with runtime log
`edvr_openxr_20261017_113452_273_26788.log`. It used the same original
v0.20.0-rc.2 DLL (PE `6AD165AB`), RTX 4080 SUPER, SteamVR at 144 Hz,
1622² per-eye input and 3244² output. The only ini change from the first
bundle was `fix.ui_quality=125` to `100`: the reported UI layer was
3244² rather than 4055². DLSS preset K remained active; the 11:45:28.640
price report gives full-frame NGX a 3.74 ms median per stereo pair across
600 pairs.
Last reported local VRAM was 7238/15279 MB (47.4%) at 11:45:26.005.

The final breadcrumb, tick 91249218, records `C0000005` writing zero in
Elite's `+0x34C9322` fatal handler. Proper unwind begins at game offsets
`+0x48C1CF0`, `+0x489F868`, `+0x5216CC`, then `+0x50F3F9`, `+0x51ECD8`,
`+0x5928C4`, `+0x200DC2`. The first three unwind frames match the prior flight's
fatal HRESULT path. Two raw-stack words are `0x887A0005`
(`DEVICE_REMOVED`) and one is `0x887A0001` (`INVALID_CALL`) in the same
layout as that flight. Raw stack is circumstantial: this is probably the
same device-loss/fatal-HRESULT class, but the whole graphics log has no
`D3D11 REFUSED` line or removal-reason query. The game-device create hooks
log only intercepted failed creates on the captured device, at most 12
per session (`device_hook.cpp:901-903,1053-1084,2671-2688`). Absence of
that line cannot prove the device stayed healthy. In this run, specifically,
`DEVICE_HUNG` is unknown, as is the failing GPU command.

Anchoring breadcrumb tick 90602640 to the first graphics log line at
11:34:49.497 places the crash at about 11:45:36.075 local (±20 ms).
The last luma-probe line reporting DLSS output was 11:45:36.061;
these readings do not mark the GPU's last progress.
The runtime's `compositor_unavailable,slot=7` at 15:45:36.083 UTC says
`PostPresentHandoff` failed, with several possible unavailable-state
paths (`openvr_compositor.cpp:154-159`, `native_runtime_host.h:1871-1904`);
it is not a device-removal reason. The last graphics line, 11:45:36.794,
samples `dbgcore.DLL` during Elite's dump writing, about 719 ms after
the estimated crash. Its `d3d11.dll` frame is in the exception-filter
chain (`proxy.cpp:352-374`), not evidence of the initiating render call.

The completed five-second planet patch-motion windows through
11:45:32.364 were all gated off because Status did not say supercruise:
no patch capture, records or temporal decision-path-12 pixels in those
windows (`celestial_motion.cpp:465-470`). The 11:45:20 GPU census also
prints `planet -`. The final roughly 3.7 seconds have no completed
five-second report, and these lines say nothing about other planet fixes.
The reporter described the scene as near a planet; that does not identify
an active planet-motion pass or a crash cause.

The earlier short graphics log, `edvr_gfx_20261017_113352.log`, says
`SENTINEL TRIPPED` and disables graphics fixes for that launch. Its runtime
reports `graphics_unavailable=80004002` and a clean module shutdown; the
graphics breadcrumb records process exit at tick 90563406. It is neither
a second crash nor an EDVR-free or FSR control. Reducing UI quality to
100 did not prevent the fatal path, but FSR has still not been tested.
Keep UI quality 100 for the next single-change FSR flight described in
Status. NGX-specific failure, common EDVR motion/UI GPU work, and a
game/driver/runtime interaction remain open; no C++ fix is supported.
