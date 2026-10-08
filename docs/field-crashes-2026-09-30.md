# Field crashes: reports from users' rigs

Crash reports from users' rigs, one dated entry per report. Read the Status
block first.

## Status

- **Removed 2026-10-08 (branch claude/remove-settlement-detail):**
  `fix.settlement_detail` no longer exists, so the "set it to game" steps
  below are moot and an old line is carried over as a retired setting.
- **State (2026-09-30):** one open report, user 5: v0.18.0-rc.4 (build
  6ABC3FAC) in VR on Windows 10, WMR's OpenXR runtime at 60 Hz (2048x2556
  per eye), RTX 3080 10 GB, DLSS preset K at HMD Quality 0.85,
  fix.ui_quality 100, fix.settlement_detail auto, no wrapper. Three of
  four sessions in 28 minutes crashed. The user says it happens only with
  EDVR installed, on stock clocks and the latest driver. EDVR has never
  been flown on Windows 10 or on WMR (Sean runs Windows 11 26200).
- **The crashes:**
  - A, 73 s in: EliteDangerous64.exe +0x4F96D9, a read of -1. The stack's
    +0x288936A is in the code region that creates panel render-to-texture
    surfaces, which fix.ui_quality's panel patch sizes.
  - C, 22 s in, at the main menu: the game's device went
    DXGI_ERROR_DEVICE_HUNG within 0.4 s of normal frames. That is a GPU
    fault blamed on the device that carries EDVR's passes, not a 2 s
    timeout. Elite then aborted (+0x34C9322).
  - D, 5 minutes in: an access violation (read of -1) in nvgpucomp64.dll,
    NVIDIA's shader compiler, on a non-render thread. The ~600 ms freezes
    after it are aftermath, not a hang.
  - B: 20 minutes, clean exit.
- **Hypotheses, ranked:**
  1. The platform: Windows 10 WDDM 2.x, WMR and the NVIDIA driver, under
     EDVR's GPU work (C, D, perhaps A).
  2. NGX preset K on Ampere (C).
  3. EDVR's shader variants against the NVIDIA compiler. EDVR's lazily
     created shaders, and game pixel shaders sent to other render-target
     formats and blends, force new driver compiles (D).
  4. The depth probe, by timing only: it was active just before C and D,
     but harmless at every start and resize.
  5. Code patches in game code: fix.ui_quality's panel patch (A's stack)
     and the settlement governor's hooks (`auto`).
  6. VRAM on 10 GB, and the two-device keyed-mutex transfer (both weak).
- **Ruled out:**
  - An RTV/DSV size mismatch after a resize. ui_layer.cpp binds a layer
    depth-stencil only at the layer's size; the 2048x2556 one in the
    17:22:41 memory line was stale and unused.
  - GPU work sized by game data: EDVR's shader loop bounds are constant or
    set by EDVR.
- **Windows 10:** src has no GPU-priority, D3DKMT, HAGS, OS- or
  WDDM-version, or waitable-timer code, and every interface it uses exists
  on Windows 10. Only the stack under EDVR differs.
- **Next:**
  - Ask the user for: Event Viewer entries at 17:17:51 and 17:23:20
    (Display 4101, nvlddmkm, WHEA); the exact driver and Windows build;
    whether HAGS is on; any overlays; whether the NVIDIA App's DLSS
    override is on.
  - Then one floor session: 20 minutes on a fixed route (main menu 60 s,
    continue, dock, launch, one jump), with fix.temporal_aa off,
    fix.ui_quality off and fix.settlement_detail game, all set before
    launch.
  - If the floor is clean, add back FSR, then DLSS, then UI quality 100,
    one leg each.
  - If the floor crashes, set edvr_openxr.ini frame_end_overlap off, then
    try HAGS, then an EDVR-free control.

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

The numbers follow the Status ranking.

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
