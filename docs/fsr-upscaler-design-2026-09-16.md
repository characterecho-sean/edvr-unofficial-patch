# FSR as the every-vendor upscaler: a design (2026-09-16)

Sean's ask: add the latest FSR 3 so that AMD GPUs get the upscaling that
`temporal_aa = dlss` gives RTX cards, and have it work on NVIDIA as well.
Design only; nothing here is built. Claims about EDVR cite the source;
claims about AMD's SDK are vendor-stated (its repository, headers and pages,
read 2026-09-16) unless marked believed.

## Status

- **State (2026-10-24): route 2 FLOWN FLAT, BRANCH ONLY; render-thread
  stall found: the D3D11 key releases.** `fix.temporal_aa = fsr4` (FSR 4.1 through
  AMD's signed D3D12 DLLs over a D3D11/D3D12 bridge, modelled on XeSS's)
  on `claude/fsr-4-1-flat-vr-support-5f6ea4`, flat and VR, beside `fsr`
  (route 1, unchanged). Not merged to main until Sean says it ships (D9).
  Facts, decisions D7 to D9 and the flight plan: journal, "2026-10-23:
  route 2". Flight 5 (journal, "2026-10-23 night"): FSR 4.1.1 ran on the
  Radeon 8060S through `FSR4_UPGRADE=1`, every dispatch clean. Its GPU
  work is small (0.8 ms); the bridge held the render thread 3 to 8 ms a
  frame in DXVK's keyed-mutex `ReleaseSync` (flight 6, journal): held keys
  cut the evaluate from 6.6 to 0.27 ms, 52 to 60 fps. Same spot, FSR 4 35 to 38 fps,
  FSR 3 36 to 38. Sean reports artifacts (vertical streaks in a station,
  FSR 3 comparison pending). A main-menu crash after a display change
  was in Elite's occlusion culler, before FSR 4 ran (set aside).
- **Temporary key (remove or ship when the stall is settled):**
  `diag.fsr4_shared_locks` (`per_frame` default, `held`, `alternate`), the
  A/B of the bridge's D3D11 keyed-mutex locks, held for a context's life
  against taken and released each frame.
- **Route 1 (2026-09-17): MERGED TO MAIN** after flights 1 to 3 (Pimax
  and Quest 3, RTX 5090, clean; fsr 1.1 to 2.1 ms per stereo pair; Sean:
  slightly better than the pass's own history, not DLSS). The community
  D3D11 port (FSR 3.1.2) compiled into d3d11.dll. Build, flights and
  reasoning: Status detail below.
- **Next:** the flicker at flight 7's spot (a hull panel's edge in the
  main menu) is in all three temporal paths, with history kept (yellow in
  the refusal view; flight 7 entry): TAA and FSR 3 there on Windows. Held keys
  looked mostly clean; they become the behaviour once (c) is out, and the
  key retires. Then VR, the AMD licence and the installer payload (D8). Route 1's flight 4 (an AMD
  supporter's log) and the exported-symbols nit (Open) still stand.
- **Open:** the reactive mask under FSR (off in flight 1, D3); the jitter
  phase count above 1:1; FSR 3.1's quality in VR against the pass's own
  history, which is what an AMD user gets today; and a cosmetic nit seen
  in the build's export listing: the port's static libraries carry
  `dllexport` attributes, so EDVR's d3d11.dll now exports nineteen `ffx*`
  symbols (inert: nothing imports them; fix in the fetch tool by building
  the port without its export macro, with `--verify` checking for it);
  and the create's VRAM figure, meaningless in the field (flight 1
  journal). Closed: Track A's build recipe and d3d11-import stop
  (journal), the prebuilt shader compiler (D6, section 6).
- **Ruled out (2026-09-16, vendor-stated; 2026-10-23 additions in the
  journal):** an official AMD D3D11 backend (none exists); FSR 4 on D3D11
  or Vulkan (D3D12-only signed DLLs; officially RX 7000/9000 only);
  reaching FSR 4 through the driver's FSR 3.1-to-4 override (D3D12 ffx-api
  titles only); FSR 3.1.5 on D3D11 (it exists only inside the D3D12 DLL,
  and its one change over 3.1.4 is a negative-RCAS-output fix, moot with
  sharpening off).
- **Environment:** every engine runs inside d3d11.dll on the game's D3D11
  device; the native OpenXR half (src\openxr\) is the only packaged VR half.
  Field rigs: an RTX 5090 with a Pimax Crystal Super and a Quest 3; the
  Steam rig also carries an AMD iGPU (gate on the DXGI vendor id, never on
  adapter count); since 2026-10-23 a Ryzen AI Max+ 395 (Radeon 8060S, RDNA
  3.5, gfx1151) on CachyOS, Mesa 26.2.5 RADV, Proton (DXVK + vkd3d-proton),
  compile-only builds from tools/linux_toolchain.py since 2026-10-24.
  FSR needs typed UAV loads (vendor-stated); cards without them (believed:
  NVIDIA before Maxwell) must get a clean refusal.
- **Detail:** 1 what exists; 2 what AMD ships; 3 the design; 4 flights;
  5 the sidecar; 6 decisions; 7 declined; journal at the end.

## Status detail (moved out of Status 2026-09-29)

- **State (2026-09-17): route 1 MERGED TO MAIN** after flights 1 to 3 on
  Sean's rigs (Pimax and Quest 3, both clean; the VRAM figure fixed
  first, c752ca3). `fix.temporal_aa = fsr` ships in the next release; the
  "works on AMD" note waits on flight 4. The ask reframes on one
  finding: AMD ships no Direct3D 11 backend for the FSR 3.1 upscaler on any
  FidelityFX SDK tag (vendor-stated: `sdk/src/backends` holds dx12, shared
  and vk only, at v1.1.4 and after; issue #58 "Porting to DX11" has been
  open since 2024-02 with no AMD reply). Elite is a D3D11 game and EDVR's
  pass runs on its device, so "the latest FSR 3" can come from only two
  places: (1) the community D3D11 port of the SDK (metarutaiga, hardened by
  the OptiScaler project; MIT; FSR 3.1.2; the path OptiScaler ships to D3D11
  games), compiled into d3d11.dll; (2) a second, D3D12 device beside the
  game's (a "sidecar") running AMD's own signed `amd_fidelityfx_upscaler.dll`
  (FSR 4.1 on RDNA3/RDNA4, FSR 3.1.5 elsewhere), the only road to FSR 4.
- **Route 1 IN PROGRESS (2026-09-16 evening) on branch
  `claude/fsr3-amd-nvidia-upscaling-00b69f`, kept separate from main until
  ready.** Sean took D1-D5 as recommended. Phase 0 landed as Track A
  (`tools\fetch_ffx_dx11.py`, 4d0a5fb: fetch, build, stage and verify the
  port) and Track B (b8cd3b2: the `fsr` value, the engine enum and helpers,
  the seven readers, the seam, the ini text, the panel, and a stub engine
  that refuses without the SDK), merged with main at 2c7c21e and green.
  Track C landed as 6294f1f: the engine body under `EDVR_HAVE_FSR3`, the
  build.bat block, the WARP rig `tools\fsr3_engine_test` (31 checks; the
  jitter and motion signs settled at the engine's defaults, journal). No
  install to any game directory without Sean's approval.
- **Flight 1 FLOWN 2026-09-17 06:00 (e70a44e, Pimax, RTX 5090):** FSR
  ran cleanly, on the upscale path (HMD Quality below 1: 2646x2206 and
  3461x2884 into 4072x3394), contexts made in 2 to 4 ms, remade cleanly
  on every size change, released on a live switch to `on`, remade on the
  switch back; no refusal, no port messages. Price per stereo pair, full
  region, median: fsr 1.6 to 2.1 ms against NGX's 2.9 to 3.6 at the same
  sizes; 83 to 89 fps. Sean's verdict: slightly better than the pass's own
  history, not as good as DLSS, as expected for FSR 3.1. Journal.
- **Flights 2 and 3 answered the same morning (06:07, Quest 3 through
  VirtualDesktopXR, same build):** FSR ran cleanly at 2611x2774 into
  3072x3264 and later 3017x2160 into 3550x2542, 1.1 to 1.4 ms per stereo
  pair, 86 to 90 fps, no refusal; Sean: the UI looks fine without the
  reactive mask, so D3's default (off) stands and the mask stays a live
  advanced key. Journal.

## 1. What exists: the door, the inputs, the seam

The temporal pass treats each eye at the door, on the texture the game
submits, with inputs EDVR makes itself (anti-aliasing.md, Feature B). Three
behaviours already stand behind one live key, `fix.temporal_aa = off | on |
dlss` (`dlaa` accepted, hidden; edvr.ini:460-466). `on` is the pass's own
history. `dlss` is NVIDIA's: 1:1 (DLAA) when the game renders at the
runtime's size, an upscale when HMD Quality or the trim render it smaller.
When NGX refuses, the pass logs "temporal aa: dlaa was asked for, but %s.
The pass's own history runs instead." (temporal_pass.cpp:3956-3964) and
runs its own. An AMD user asking for `dlss` today therefore gets TAA, not a
bare frame: the bar FSR must clear is the pass's own history.

Inputs, per eye, at the render size w x h (temporal_pass.cpp:3981-3991;
dlaa.h:49-66):

- colour `dlColour`, R8G8B8A8_UNORM, copied from the submitted eye:
  post-tonemap, display-referred, [0,1] (measured; the `IsHDR` and
  `AutoExposure` flags were falsified 2026-09-05, and the linear-light
  variant `temporal_aa_light` ghosted in the field);
- depth `dlDepth`, R32_FLOAT, the game's reversed-Z copied (two plane pairs
  seen: 0.025..50000 for the scene, 0.1..1000 elsewhere);
- motion vectors `dlMv`, R16G16_FLOAT, pixels, current to previous, at the
  render size: one compute shader (`motionShader`, temporal_pass.cpp:1164,
  dispatched at 4130-4156) writes vectors, depth and mask together from the
  head's reprojection through depth plus the per-object, UI, terrain and
  smoke sources;
- a reactive mask `dlMask`, R8_UNORM, UI and movers unioned
  (temporal_pass.cpp:4162-4165), handed to NGX as
  `pInBiasCurrentColorMask` (dlaa.cpp:601), which only preset F honours;
- no exposure texture (pre-exposure and scale fixed at 1.0,
  dlaa.cpp:593-619); sharpness 0 (`fix.render_sharpness` in
  sharpen_pass.cpp is the one sharpen seam);
- output `dlOut`, R8G8B8A8_UNORM, shader-resource and unordered-access,
  outW x outH, then `CopyResource` into `dlSubmit` in the game's own
  swapchain format (temporal_pass.cpp:4000-4004, 4306).

NGX is created per eye with `MVLowRes | DepthInverted`, keyed on (w, h,
outW, outH): any size change rebuilds the feature (dlaa.cpp:313-314,
380-391). The jitter is Halton(2,3), 8 phases, [-0.5, 0.5) render pixels
(temporal_math.h:65-99, sign convention at 26-28), applied as a tangent
shift through the projection hook and passed unchanged as `InJitterOffset`
(native_temporal.cpp:224-231, dlaa.cpp:607-608); the flips
`advanced.temporal_aa_jitter_sign` and `_jitter_lag` were settled on
tools\smoke's convention rig, never from a flight. Reset is `flags` bit 0 or
a missing history (temporal_pass.cpp:4170); continuity breaks on a gap, a
size or format change, and a withheld frame (native_temporal.cpp:211-213).
The price is a GpuTimer ring per role and eye, printed as `temporal aa
price: %s, %ux%u, %d stereo pairs (%s), ms per pair median/p95:`
(temporal_pass.cpp:752-755), on the perf tile and the settings panel.

The seam is `if ((flags & 2u) != 0 && fmtIndex == 0 && !foveaMode)` then
`dlaaAvailable(dev, &why)`: NGX, else the pass's own history
(temporal_pass.cpp:3954-3965; the flags word is documented at
temporal_pass.h:209-215). The mode is parsed in both front-ends
(`readConfig`, native_temporal.cpp:95-110; src\openvr\temporal_aa.cpp:
180-186), and seven more readers gate "a trained engine wants UI depth,
separation and deferred UI" on the literal strings `dlss|dlaa`
(temporal_mode.h:11-15 `temporalModeEnabled`; ui_deferred.cpp:357;
ui_depth.cpp:1449; ui_separation.cpp:70; temporal_pass.cpp:4952
`g_trainedWanted`; menu.cpp:784, 828, 1700). Both front-ends and the pass
live in the one d3d11.dll (the openvr half resolves `edvrTemporalAa` by
GetProcAddress, temporal_aa.cpp:494-504): a new engine is new code in this
module, not a new DLL.

## 2. What AMD ships, and does not (vendor-stated unless marked)

- Tags: v1.1.4 (2025-05-08) carries FSR 3.1.4 and is the last of the
  source-backend line; v2.0.0 (2025-08-20) through v2.3.0 (2026-06-24)
  carry FSR 4.x plus a 3.1.5 fallback inside `amd_fidelityfx_upscaler.dll`,
  loaded through `amd_fidelityfx_loader.dll`, D3D12 only ("the API will
  automatically select AMD FSR 3.1.5" on other hardware).
- Backends in the SDK: dx12, shared, vk, on every tag. The README says
  DirectX 12 or Vulkan. No AMD document mentions D3D11On12 or D3D11 interop.
- FSR 4.1 (announced May 2026, shipped July): D3D12; RDNA4 (FP8) and RDNA3
  (INT8) officially, RDNA2 planned for 2027. Prebuilt signed DLLs. The
  Adrenalin "upgrade FSR 3.1 to 4" override is for D3D12 ffx-api titles.
- The D3D11 port: github.com/metarutaiga/FidelityFX-SDK-DX11, hardened as
  github.com/optiscaler/FidelityFX-SDK-DX11 (HEAD 9b04fa4 as read; MIT).
  Version constants 3.1.2. `sdk/include/FidelityFX/host/backends/dx11/
  ffx_dx11.h` declares `ffxGetScratchMemorySizeDX11(size_t maxContexts)`,
  `ffxGetDeviceDX11_Fsr31(ID3D11Device*)`, `ffxGetInterfaceDX11(FfxInterface*,
  FfxDevice, void* scratch, size_t, uint32_t maxContexts)`,
  `ffxGetCommandListDX11(ID3D11DeviceContext*)`,
  `ffxGetResourceDX11_Fsr31(const ID3D11Resource*, FfxResourceDescription,
  const wchar_t*, FfxResourceStates)`, `ffxGetSurfaceFormatDX11(DXGI_FORMAT)`
  and `GetFfxResourceDescriptionDX11(ID3D11Resource*)`. Its backend CMake
  builds `ffx_backend_dx11_x64` (static or DLL per `FFX_BUILD_AS_DLL`) and
  compiles every shader with the SDK's own shader compiler at `-T cs_5_0
  -DFFX_HLSL_SM=50` in four permutations (wave32, wave64, 16-bit); the
  upscaler is `ffx_fsr3upscaler_x64`. No CRT setting anywhere in its CMake
  (CMake's MSVC default is /MD). OptiScaler's D3D11 FSR 3.1 feature
  (`upscalers/fsr31/FSR31Feature_Dx11.cpp`) uses exactly this chain:
  scratch, `ffxGetInterfaceDX11`, `ffxFsr3ContextCreate`,
  `ffxFsr3ContextDispatchUpscale` (the combined FSR3 wrapper; the
  upscaler-only `ffxFsr3UpscalerContextCreate` and `Dispatch` in
  `ffx_fsr3upscaler.h` drive the same core and are what EDVR would call).
  Unverified as of writing: whether prebuilt libraries or blob headers are
  committed, and the exact build script.
- The FSR 3.1 upscaler API (ffx_fsr3upscaler.h at v1.1.4; the same shape at
  3.1.2): context {flags, maxRenderSize, maxUpscaleSize, fpMessage,
  backendInterface}; dispatch {commandList, color, depth, motionVectors,
  exposure, reactive, transparencyAndComposition, output, jitterOffset,
  motionVectorScale, renderSize, upscaleSize, enableSharpening, sharpness,
  frameTimeDelta (ms), preExposure, reset, cameraNear, cameraFar,
  cameraFovAngleVertical, viewSpaceToMetersFactor, flags}. Context flags:
  HIGH_DYNAMIC_RANGE, DISPLAY_RESOLUTION_MOTION_VECTORS,
  MOTION_VECTORS_JITTER_CANCELLATION, DEPTH_INVERTED, DEPTH_INFINITE,
  AUTO_EXPOSURE, DYNAMIC_RESOLUTION, DEBUG_CHECKING; dispatch flag
  DRAW_DEBUG_VIEW. Jitter helpers with phase count ceil(8 n^2): 8 at 1:1,
  18 at 1.5x. Quality ratios per axis 1.0, 1.5, 1.7, 2.0, 3.0. Motion
  vectors in [-w, w] x [-h, h] pixels, current to previous, with
  `motionVectorScale` multiplied in. Inverted depth means near at 1. Needs
  typed UAV loads and R16G16B16A16_UNORM.
- Cost, vendor-stated at 4K in native-AA mode: 1.7 ms on an RX 7900 XTX,
  2.9 ms on an RX 6800 XT, 6.5 ms on an RX 5700 XT. Scaled by output pixels
  per stereo pair (believed, linear): Quest 3 at 2064x2208 per eye about
  1.9 / 3.2 / 7 ms; the Pimax at 2576x2544 (the eye-mask flights' input)
  about 2.7 / 4.6 / 10 ms; the Pimax at its full 5424x5356 about 12 / 20 /
  45 ms. FSR on a Pimax needs `openxr_resolution` or the trim, as NGX does
  (3.4 ms per pair on the 5090 at 2576x2544, 1.95 with the trim).

## 3. The design: route 1, the D3D11 port as a third engine

### 3.1 The setting

`fix.temporal_aa = off | on | dlss | fsr` (`dlaa` stays hidden). `fsr` is
AMD's upscaler on any GPU, NVIDIA included, at the sizes `dlss` would use:
1:1 when the game renders at the runtime's size (AMD's "native AA"), an
upscale to that size when it renders smaller. ui line: `choices off,
on=TAA, dlss=DLSS, fsr=FSR`. Values stay literal: `dlss` on a card NGX
refuses keeps running the pass's own history, but the refusal line gains
"set temporal_aa = fsr for AMD's upscaler, which runs on any GPU". No
`auto` until the field has judged FSR a safe default (D1).

`fix.temporal_aa_model` (k, l, m; j and auto removed 2026-10-21) stays NVIDIA's; under `fsr` it
is ignored and the panel hides the preset row. The fovea and periphery keys
(`advanced.temporal_aa_fovea*`, `_periphery*`) belong to NGX's subrect crop
path (dlaa.cpp:665-793); under `fsr` the full-frame path runs and one line
says the fovea keys are ignored.

New advanced keys, live, documented in edvr.ini for the contract check:
`advanced.temporal_aa_fsr_reactive = off | on` (hand the UI-and-movers mask
to FSR as its reactive mask; off first, D3) and
`advanced.temporal_aa_fsr_debug = off | on` (FSR's own debug view,
DRAW_DEBUG_VIEW). FSR's own diagnostics ride
`advanced.temporal_aa_diagnostics`: DEBUG_CHECKING on, `fpMessage` into the
gfx log.

### 3.2 Where it goes

- `src\d3d11\fsr3_engine.cpp/.h`, new, beside dlaa.cpp (not under
  `src\d3d11\fsr\`, which holds FSR 1's headers): the mirror of dlaa.h's
  surface. `fsr3Available(dev, &why)` once per session (built with
  `EDVR_HAVE_FSR3`; feature level 11_0 or better; typed UAV loads per
  `D3D11_FEATURE_D3D11_OPTIONS2`; scratch plus `ffxGetInterfaceDX11` sized
  for two contexts), `fsr3Evaluate(ctx, eye, colour, depth, mv, reactive,
  out, w, h, outW, outH, jx, jy, reset, frameMs, nearZ, farZ, fovY)`,
  `fsr3Warm`, `fsr3ReleaseFeatures`, `fsr3Shutdown`, `fsr3Totals`. Two
  contexts `g_ctx[2]` keyed on (w, h, outW, outH) exactly as
  `ensureFeature` keys NGX: recreate on change, reset on create;
  DYNAMIC_RESOLUTION unused in this phase.
- The seam: an engine enum {own, nvidia, amd} threaded from both
  front-ends' `readConfig` through `edvrTemporalAa` (the flags bit stays for
  the ABI; the enum decides), so the branch at temporal_pass.cpp:3954
  becomes: colour copy, the `mvCs` dispatch, then `dlaaEvaluate` or
  `fsr3Evaluate`, then the `dlSubmit` copy, the timing ring and the price
  line, all shared. The R8G8B8A8-family refusal (temporal_pass.cpp:
  3947-3953) applies to both external engines and names the engine. NGX's
  other refusal, a render size outside every DLSS mode's range for the
  output (flight 4 of the trim, 2026-09-16: 1711x1425 against 3422x3394 at
  HMD Quality 0.5, since sized by the ask the frame in hand was rendered
  for, be05124, which feeds both engines alike), should have no FSR
  counterpart: believed, the context bounds only the maximum render size
  and the quality ratios are helpers, so `fsr` would keep treating through
  a trim rebuild where `dlss` drops to the pass's own history. The rig in
  3.5 checks that ratio.
- The readers: temporal_mode.h gains `temporalExternalEngine(mode)` (true
  for dlaa, dlss, fsr) and `temporalModeEnabled` accepts `fsr`; the seven
  literal-string readers in section 1 switch to the helper, so `fsr`
  reaches UI depth, UI separation, deferred UI and `g_trainedWanted` in one
  place. The panel (menu.cpp:784-828, 1308, 1699) and the installer's
  settings view offer the value; the price line and the perf tile print the
  engine's name (`fsr 3.1.2`), so a session where FSR never ran reads
  differently from one where it did.
- Third-party: `third_party\ffx-dx11\`, gitignored beside `third_party\ngx`,
  populated by `tools\fetch_ffx_dx11.py` (pinned commit SHA and archive
  SHA-256; `--verify`; `--self-test` in build.bat's python gate): fetch the
  optiscaler fork, build its shader compiler and the two libraries with
  CMake and VS2022 once per machine, forcing
  `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` (EDVR is /MT; a mismatch is
  LNK2038 at link, a gate rather than a flight), static
  (`FFX_BUILD_AS_DLL=OFF`), upscaler only. build.bat: an `EDVR_HAVE_FSR3`
  block beside the NGX one (build.bat:319-367) and the two libraries on the
  d3d11.dll link line (448-451). Without the SDK the build stays green and
  `fsr` refuses with "this build was made without AMD's upscaler" (D5).
  Licence: the port's MIT notice goes into `third_party\ffx-dx11\` and the
  README's third-party section, as `src\d3d11\fsr\README.md` does for FSR
  1. Nothing new is shipped: the installer manifest
  (install_edvr.py:884-896) and payload (payload.h:18-23) are untouched.

### 3.3 The input mapping

| Input | NGX today | FSR 3.1 |
|---|---|---|
| colour | `dlColour`, render size | `color`; no HIGH_DYNAMIC_RANGE (display-referred [0,1]: the IsHDR lesson) |
| depth | `dlDepth`, DepthInverted | `depth`; DEPTH_INVERTED; not DEPTH_INFINITE (finite far planes) |
| motion | `dlMv`, MVLowRes, scale 1 | `motionVectors`; `motionVectorScale = {1, 1}`; no DISPLAY_RESOLUTION flag; no JITTER_CANCELLATION (the vectors are computed, not rendered jittered) |
| jitter | `InJitterOffset = (jx, jy)` | `jitterOffset = (jx, jy)`; FSR's own helper unused; the sign settled on the rig (3.5) |
| reactive | `pInBiasCurrentColorMask`, preset F only | `reactive` = the same R8 mask when `temporal_aa_fsr_reactive = on`, else null; `transparencyAndComposition` null |
| exposure | none, 1.0 | `exposure` null; `preExposure = 1`; no AUTO_EXPOSURE |
| reset | `InReset` | `reset` |
| frame time | ms | `frameTimeDelta` in ms |
| camera | not needed | `cameraNear`, `cameraFar` from the frame's plane pair (already passed as nearZ/farZ, temporal_pass.h:199-200; which of the inverted pair FSR calls near is read off the header's comment at build time); `cameraFovAngleVertical = atan(t) + atan(b)` from the eye's tangents; `viewSpaceToMetersFactor = 1` (Elite's units are metres) |
| sharpen | `InSharpness = 0` | `enableSharpening = false` |
| sizes | in and target | `renderSize = (w, h)`, `upscaleSize = (oW, oH)`; the context's maxima the same |
| output | `dlOut` (unordered access) | `output` |
| debug | none | DEBUG_CHECKING plus `fpMessage` into the log; DRAW_DEBUG_VIEW behind its key |

### 3.4 What the D3D11 device sees

- The backend records on the immediate context EDVR hooks, so its Dispatch
  and CSSet calls re-enter EDVR's thunks exactly as NGX's internal
  dispatches do: `fsr3Evaluate` sits inside whatever guard the NGX evaluate
  sits in (the census and the vouch counters must see both the same way),
  and the pass saves and restores compute state around it as it does for
  NGX. Its CreateComputeShader calls pass through the DXBC census once, at
  first use: expected, not a fault.
- `temporal_aa_warm`, the first-Submit warm-up, also creates FSR's
  contexts, so the first treated frame does not pay the allocation.
- VRAM: FSR's internal targets scale with the output size per eye; the
  create logs `+N MB` and its duration as its own perf-monitor event, as
  NGX's does (`kEvNgx`, dlaa.cpp:402).
- Single-threaded, render thread, immediate context only, like the rest.

### 3.5 Phase 0: the desk, before any flight

1. `tools\fetch_ffx_dx11.py`: fetch, build, `--verify`. Verify answers, and
   this doc's journal records: the libraries exist; `dumpbin /directives`
   shows /MT; `dumpbin /imports` over the archive members shows no by-name
   import of d3d11.dll, dxgi.dll or d3dcompiler_47.dll (the proxy IS
   d3d11.dll, so a by-name import would bind to itself: the EDHM lesson,
   system_d3d11.h:11-15; a d3dcompiler import would mean runtime shader
   compilation, which the cs_5_0 blobs should make unnecessary). Any of
   these is a stop until understood.
2. build.bat: the `EDVR_HAVE_FSR3` block, the link line, and a build
   without the SDK proven green.
3. `tools\fsr3_engine_test\` (`:rig_fsr3_engine_test`; WARP; `--dry-run`
   and `--self-test`; joins the pool by its label): two contexts at Quest 3
   sizes; synthetic frames from tools\smoke's convention instrument (a
   moving edge with nearly-right history: the class of test that catches
   ghosting, where the flat-field probes did not); asserts: create and
   dispatch return FFX_OK with DEBUG_CHECKING silent; the output is
   non-black and within tolerance of the input at rest; the registration
   table over jitter sign {as_is, flip_x, flip_y, flip_both} x motion sign
   {+, -} picks exactly one zero-bias combination (this is how NGX's
   convention was settled; it is re-run for FSR, not assumed equal); reset
   clears the history; a size change recreates cleanly, including
   1711x1425 into 3422x3394 (the ratio NGX refused in flight 4 of the
   trim); VRAM per context is logged. cs_5_0 DXBC runs on WARP, so the
   whole engine runs on the desk.
4. The seam refactor, the helper, the readers, the ini text, the contract
   check, the panel and the settings view; `dlss`'s refusal line gains the
   hint.
5. Before the first flight: every log line section 4 looks for exists and
   prints something different when the engine did not run.

## 4. Flights, in order

Read each with `python tools\edvr_log.py --target <t> --expect-build <sha>
--grep "fsr3|temporal aa"`.

- **Flight 1: Sean, RTX 5090, Pimax at the eye-mask flights' resolution,
  `temporal_aa = fsr`, HMD Quality 1.0, reactive off.** Must show, in this
  order (the engine's own lines as built): `fsr3: first asked for on
  <adapter>, AMD's port version fsr 3.1.2.`; `fsr3: the DX11 backend
  initialised in N ms on device ...`; `fsr3: the context is created for
  eye 0 at WxH -> WxH; VRAM ...; the history starts here (made in N ms).`
  and the same for eye 1; `temporal aa: fsr 3.1.2 engaged -- AMD's
  history takes the WxH frame ...`; the price line naming `fsr 3.1.2`.
  Must NOT show: `fsr3: refused`, `fsr3: eye N is stood down`, "The
  pass's own history runs instead", or any `fsr3: <message>` line carrying
  the port's own text (its debug checking is off unless
  `advanced.temporal_aa_diagnostics` is on).
  Judged against `on` and `dlss` in the same session on the known cases:
  cockpit text swim, the distant-station shimmer, the rolling ship's leading
  edge (the `temporal_aa_light` ghost). Answers: does it run, what it costs
  on NVIDIA at that size, and whether it beats the pass's own history.
- **Flight 2: same rig, HMD Quality 0.667 (FSR's "quality", 1.5x per
  axis), reactive on for half the session (live key).** Answers: the
  upscale path, the mask's effect on the HUD, and whether 8 jitter phases
  suffice against FSR's 18.
- **Flight 3: Quest 3.** The other tracker-noise regime; the price at its
  size.
- **Flight 4: an AMD supporter (RDNA2 or RDNA3), any headset.** The same
  lines plus the adapter name from the "first asked for on %s" line
  (dlaa.cpp:510-515, generalised to the engine) and the price. This is the
  gate for a release note saying "works on AMD".

If flight 1 says FSR 3.1 is not better than the pass's own history, route 1
stops there and route 2 is the only remaining value (FSR 4.1 is a different
class of upscaler): that is route 2's go/no-go.

## 5. Route 2: the D3D12 sidecar (a sketch, not for now)

- Why: FSR 4.1 on RDNA3 and RDNA4; AMD's maintained, signed binaries and
  their driver-side upgrades; the same bridge would carry XeSS 2 and DLSS 4
  (out of scope).
- Shape: one D3D12 device on the game's adapter (LUID match, as
  src\common\d3d11_device_identity.h does) with a direct queue. `dlColour`,
  `dlDepth`, `dlMv` and `dlOut` are created on the D3D11 side with
  `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` and opened on D3D12 with
  `OpenSharedHandle`: opened, not copied. Sync by a shared fence, not a
  keyed mutex (D3D12 has none): `ID3D11Device5::CreateFence` with
  `D3D11_FENCE_FLAG_SHARED`, opened as an `ID3D12Fence` (Windows 10 1703
  or later). Per eye: D3D11 `Signal(f, n)` and `Flush`; D3D12 `Wait(f, n)`;
  ffx-api create and dispatch (`amd_fidelityfx_loader.dll` then
  `amd_fidelityfx_upscaler.dll`, D3D12 descriptors); D3D12 `Signal(f,
  n+1)`; D3D11 `Wait(f, n+1)` before the `dlSubmit` copy. Two queues
  serialise, so there is a bubble per pair: believed 0.3-1 ms (OptiScaler
  reports up to 10% for its equivalent mode).
- Precedent in-house: src\openxr\shared_texture_transfer.cpp (NT handles
  plus a keyed mutex between two D3D11 devices) and its WARP rig
  tools\openxr_shared_texture_test; the sidecar reuses the handle plumbing
  with fences in place of the mutex.
- Shipping: two AMD DLLs beside the game's exe by the `nvngx_dlss.dll`
  pattern (conditional RCDATA in tools\gen_installer_rc.py:200-203,
  payload.h and payload.cpp, plan.cpp:417-467, install_edvr.py:945-946)
  and their licence file. Whether the signed DLLs carry terms beyond the
  SDK's MIT is unverified and must be read before shipping. Whether the
  DLL's 3.1.5 path runs on NVIDIA and Intel is unverified (believed yes:
  games ship it for every vendor).
- Not before: route 1 has flown on both rigs, an AMD tester has reported,
  and an RDNA3 or RDNA4 tester exists.

## 6. Decisions for Sean

- **D1** Values stay literal: `fsr` is offered, there is no `auto`, and
  `dlss` never silently becomes FSR. Recommended.
- **D2** Route 1 first; route 2 after route 1's field verdict. Recommended.
- **D3** The reactive mask: a live advanced key, off in flight 1, on for
  half of flight 2. Recommended.
- **D4** The port at the optiscaler fork's HEAD (FSR 3.1.2), pinned by SHA.
  Rebasing it onto v1.1.4's upscaler core (3.1.4) is possible later (the
  core is backend-agnostic; the port's diff is the backend and the cs_5_0
  shader changes), not now.
- **D5** A build without the port's SDK warns, and `fsr` refuses with the
  reason, as NGX does. Recommended.
- **D6** (taken 2026-09-16 night) The port's prebuilt shader compiler is
  accepted as a build-time-only tool: it runs once at fetch time on the
  developer's machine, never ships, and cannot be built from source here
  without the VS ATL component (journal). Its hash sits in the staged
  VERSION.txt.

## 7. Considered and declined

- FSR 1 (already vendored: EASU and RCAS): spatial, no history, not the
  upscaling asked for; it stays for the HUD, the panels and the intro.
- XeSS 2 (Intel): an official D3D11 library on every vendor's DP4a path.
  Not asked for; a fourth engine on the same seam if FSR 3.1 disappoints
  and route 2 is too far.
- Writing our own D3D11 backend against AMD's upscaler core: that is what
  the port already is; only if the port proves unmaintainable.
- Waiting for AMD: issue #58 has had no engagement in two and a half years.
- The pass's own history as "good enough for AMD": it is what they get
  today; the ask is the trained, temporal upscaler class.

## Journal

- 2026-09-16: written from three mapping passes (the DLSS internals, the
  build, installer, rigs and docs, and the FidelityFX SDK's current state).
  No code. File and line pointers are as of ff88cd1; the four commits that
  landed on main the same afternoon (be05124 to e4b923a) touched none of
  the cited files.
- 2026-09-16 evening, Track A (4d0a5fb): `tools\fetch_ffx_dx11.py` pins
  optiscaler/FidelityFX-SDK-DX11 at 9b04fa49 (FSR 3.1.2), shallow sparse
  clone into `%LOCALAPPDATA%\EDVR\ffx-dx11\src`, CMake on `sdk\CMakeLists.txt`
  with `-DFFX_API_DX11=ON -DFFX_FSR=ON -DFFX_FSR3UPSCALER=ON` (not
  `-DFFX_FSR3=ON`, which also builds the frame-generation wrapper), VS2022,
  static libraries, `/MT` forced; stages the headers, `ffx_fsr3upscaler_x64.lib`
  (94,604 bytes), `ffx_backend_dx11_x64.lib` (9,808,892 bytes), the licence
  and a VERSION.txt. `--verify` checks the headers, the LIBCMT directive
  and that neither library imports d3d11, dxgi or d3dcompiler by name (it
  does not: the backend takes the device it is handed). No CMake on PATH or
  in VS here; pip's `cmake` package is the tool's third fallback.
  - Two API-contract findings from a standalone WARP smoke (feature level
    11_1, 1280x720 to 1920x1080, create and dispatch FFX_OK, scratch for two
    contexts 2,788,632 bytes): (a) `ffx_dx11.h` declares
    `ffxGetResourceDX11_Fsr31` `extern "C"` with a `const ID3D11Resource*`,
    but `ffx_dx11.cpp` defines it non-const with C++ linkage, so a caller
    of the declared symbol fails to link (LNK2019); the engine carries a
    local declaration of the real signature. (b) Under DEPTH_INVERTED the
    port's debug check warns unless cameraNear > cameraFar numerically; the
    engine passes the pair swapped.
  - Shader compiler provenance: the fork's prebuilt `FidelityFX_SC.exe`
    (SHA-256 30f9df14..., 283,136 bytes) is not byte-identical to AMD's
    v1.1.4 binary (75480f22..., 474,624 bytes). It runs once, at fetch-build
    time, on the developer's machine, and never ships; only the libraries
    it produced link in. Its source is in the fork
    (`sdk\tools\ffx_shader_compiler\`); against AMD's v1.1.4 the Xbox GDK
    backends and the debug/PDB parameter are gone and FXC's include handling
    is a deduped set seeded with the shader's own directory; the
    `D3DCompile` call is unchanged and d3dcompiler_47 is loaded dynamically.
    Building it from source here fails: `pch.hpp` includes `atlcomcli.h`
    and this VS2022 Community install has no ATL component (the vendored
    tiny-process-library also needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5.0`
    under CMake 4.4). Sean's call, the same night: the prebuilt is fine.
    Accepted as a build-time-only tool (D6); the fetch tool records its
    hash in VERSION.txt so a change would show.
- 2026-09-16 evening, Track B (b8cd3b2): the `fsr` value; `TemporalEngine`
  {Own, Nvidia, Amd} with `temporalEngineFor`, `temporalExternalEngine` and
  `temporalEngineLabel` in `src\common\temporal_mode.h`; the seven readers
  routed through them; flags bit 6 carries "the external engine is AMD's"
  across the ABI; fovY computed at the seam from the eye's tangents;
  `src\d3d11\fsr3_engine.h/.cpp` as a stub that refuses without
  `EDVR_HAVE_FSR3`; the ini paragraph and the two advanced keys; the menu
  and the settings panel hide the NVIDIA preset row under `fsr`. Found and
  fixed a warm-up log line that said "NVIDIA" whatever the engine. Nits
  handed to Track C: the refusal once-flag is shared across engines, the
  warm-up is 1:1 only (as NGX's), the version label is a placeholder, the
  totals need a timestamp-query ring, the menu row hide is not live within
  an open menu. Merge with main (2c7c21e): main's lean variant (651ddb4)
  and the engine field met in `temporal_pass.cpp`'s Slot, WindowKey and
  treatmentName; both kept; green.
- 2026-09-16 night, Track C (uncommitted while its rig is finished): the
  engine body under `EDVR_HAVE_FSR3` in `src\d3d11\fsr3_engine.cpp`, the
  build.bat block (after the NGX block; `EDVR_FFX_DX11=none` builds without
  the port on purpose; both builds green), `fetch_ffx_dx11.py --self-test`
  in the python gate, the `kEvFsr` perf event, the perf tile and the panel
  reading `fsr3Totals` under `fsr`, and the WARP rig
  `tools\fsr3_engine_test`. Settled facts:
  - **Two defects in the port, both handled.** (a) `RegisterResourceDX11`
    calls `CreateShaderResourceView` on every registered texture, the
    write-only output included, with no bind-flag check; (b) its `TIF`
    helper answers any failed D3D11 call with a bare `throw 1`, an untyped
    C++ exception escaping an otherwise all-`FfxErrorCode` C API. Together
    they crashed the rig (`STATUS_STACK_BUFFER_OVERRUN` from the uncaught
    throw) on a UAV-only output. The pass's own `dlOut` already carries the
    SRV bind flag, so the field never saw this; the engine now wraps create
    and dispatch in try/catch and turns a throw into its normal false-plus-
    reason answer. A dispatch that throws mid-way may leave compute-stage
    bindings on the context; known, not handled (the pass restores its own
    state around the seam).
  - **Feature level.** The port's luma-pyramid shader declares 64 compute
    UAV slots, a D3D11.1 feature: a device negotiated at 11_0 fails context
    creation with `FFX_ERROR_BACKEND_API_ERROR` (seen on WARP when the rig
    passed no feature-level array). The rig now asks for 11_1. The game's
    own device is feature level 12_0 (`featureLevel=0xC000` in the gfx log
    of 2026-09-16 20:26), so the field is unaffected; `fsr3Available` gates
    on 11_1 with a reason for older devices.
  - **Near/far under DEPTH_INVERTED.** The core's
    `setupDeviceDepthToViewSpaceDepthParams` takes the min and max of the
    pair whatever the order, its own comment saying so; the swap silences
    only the debug check.
  - **VRAM line.** WARP answers `QueryVideoMemoryInfo` with zero bytes; the
    "not reported" branch is exercised nowhere yet.
  - **Registration table, first run: NOT settled.** Jitter `as_is` won its
    axis by a real margin, but the motion-vector sign tied exactly for every
    jitter variant and the best error (38/255) was ten times the at-rest
    error (3.6/255): the motion input had no effect in any variant, and no
    history registered. The scene (the smoke pattern's 3.5 to 5 px periods
    under a 1 degree yaw, about one period per frame) cannot separate a sign
    error either. Escalated: instrument the motion field, prove the texture
    reaches the port, rebuild the test on a pure translation of a
    low-frequency hard-edged scene with per-axis motion flips, assert the
    minimum sits near the at-rest error. Result in the next entry.
- 2026-09-16 night, the escalated rig: **the motion vectors never reached
  the port.** A probe that dispatched the same static scene with zero
  motion and with a uniform 40 px vector got byte-identical output. Cause:
  FSR 3.1 moved three working surfaces into the dispatch description for
  the CALLER to allocate (`dilatedDepth`, `dilatedMotionVectors`,
  `reconstructedPrevNearestDepth`, so a frame-interpolation host can share
  them); the engine left them null, the port's `RegisterResourceDX11` maps
  a null pointer to its NULL resource, the dilate pass wrote its vectors
  into nothing and the reprojection read zeros back, every frame, with
  FFX_OK. Fix: each eye's context owns the three, made from
  `ffxFsr3UpscalerGetSharedResourceDescriptions` (asked, not hard-coded),
  released on rekey; about 12 bytes per render pixel, 55 MB per eye at
  Quest 3 sizes. Possible saving, unproven: one set for both eyes, since
  nothing reads them across dispatches. The port's own source, quoted in
  the engine: the scale constant is `motionVectorScale / renderSize`
  (render-pixel vectors with a scale of 1 are exact, as for DLSS), history
  is sampled at `uv + mv` (vectors point current to previous, as `dlMv`
  does) and the unjittered position is `pos - jitter` (content sits at
  +jitter, `dlaa.h`'s convention). **Table, rebuilt** on a pure 3 px/frame
  translation (x then y) of a band-limited two-scale pattern against the
  unjittered point sample, jitter {as_is, flip_x, flip_y, flip_both} x
  motion {as_is, flip_x, flip_y, flip_both}: winner `as_is/as_is` at
  1.38/255 against a 0.93 at-rest floor; runner-up (jitter flip_x) 2.52;
  a single-axis motion flip 6.9 to 7.3; both 12.8. Assertions: exactly one
  minimum, inside twice the floor with every other cell outside, each
  single-axis motion flip at least twice the winner, and EDVR's own
  reprojected yaw field 8.06 settled against 28.36 flipped. A hard-edged
  scene was tried first and discriminated worse (the 1:1 resolve's residual
  at a discontinuity is large for every variant). **The engine's constants
  stay 1, 1, 1, 1.** The rig was traced by sabotage: with
  `dilatedMotionVectors` pointed at null again it fails six checks; the
  first rig passed that state. `fsr3Available` gates on feature level 11_1.
  31 checks, 20 s in the pool (wall-clock unchanged), build green.
- 2026-09-16 night, the adversarial review of the branch (6294f1f against
  main) and its fixes, all on the branch, both builds green, 59 rig checks:
  - **Refusal once-flag** was shared across engines and never cleared: a
    DLSS refusal, then a switch to `fsr` that also refused, logged nothing.
    Now one flag per engine, both cleared on an engine change; NGX's hint
    to try `fsr` prints only when the build has the port.
  - **The try/catch around the port's C API was compiled under `/EHsc`**,
    which tells the compiler `extern "C"` functions never throw, so the
    catch could be skipped and the throw a crash. Now `/EHs` for the d3d11
    half and the rig, and the engine checks every texture's bind flags
    before registering (SRV on all, UAV on the output and the three
    surfaces) and refuses with a reason naming the texture, so the port's
    throw is never the expected path. The rig proves both: a UAV-only
    output is refused by the check, and with the check bypassed (a
    test-only hook) the port's own throw comes back as false plus reason.
  - **The upscale case had never run**: every rig dispatch was 1:1, while
    any HMD Quality below 1 (every Quest 3 flight) starts in it. The rig
    now runs 1376x1472 into 2064x2208 with the sign table at that ratio
    (winner 2.06 against a 2.05 floor; a wrong motion sign 13.6 or worse)
    and a real 1711x1425 into 3422x3394 create and dispatch. The table's
    "nothing else inside twice the floor" rule fails at a ratio (the floor
    carries the reconstruction residual), replaced by a scale-free bar:
    the winner's distance above the floor at most a quarter of the
    runner-up's.
  - **The warm-up is 1:1** and the first upscaled frame pays a create:
    kept, because the render size is not knowable at warm time (only the
    output size is; Elite's render fraction is the launch-time multiplier
    the pass already cross-checks at the seam); the log line says so.
  - A create that throws or fails now latches for that key (one line, no
    retry every frame; the half-made context leaks, named in the comment).
  - The perf tile and the panel status followed whichever engine ran
    first; now one accessor answers for the current engine.
  - `fsr3ReleaseFeatures` had no caller; the treat now releases AMD's
    contexts on the render thread when it first sees the engine change
    (NVIDIA has no per-feature release, left as it was).
  - `advanced.temporal_aa_diagnostics` is part of the context key, so a
    live flip recreates; unknown frame time is a nominal 11.1 ms; the
    90-degree fovY fallback logs once; the message callback's buffer is
    initialised.
  - **The port's MIT notice ships**: `FIDELITYFX-SDK-DX11-LICENSE.txt` in
    `build\` and the release zip beside NVIDIA's, deleted on the no-port
    path. The fetch tool's `--verify` now fails on a stale stage (commit
    pin and the recorded compiler hash read back from VERSION.txt).
  - Sound and left alone: `resetHist` on a switch, the compute-state
    save/restore around the seam, the reactive mask's shape, the shared
    output and copy-out, the seven readers, `texture_lod_bias = auto`
    (keys on the mode, not the vendor), the build wiring, the rekey.
- 2026-09-17 06:00, flight 1 (e70a44e, Frontier, Pimax, RTX 5090; log
  `edvr_gfx_20260917_060039`, right build). The session started under
  `dlss` at 2665x2632 into 4100x4050; the live switch to `fsr` printed
  `fsr3: first asked for on NVIDIA GeForce RTX 5090, AMD's port version
  fsr 3.1.2.`, the backend initialised (0 ms), the two contexts (3 and 4
  ms) and `temporal aa: fsr 3.1.2 engaged -- AMD's history takes the
  2665x2632 frame, its depth and the pass's own motion vectors ...`. The
  sizes then moved three times (the trim to 4072x3394; the render size to
  2646x2206, later 3461x2884) and every rekey printed its reason and
  remade both contexts in 2 to 6 ms; a switch to `on` released AMD's
  contexts and surfaces on the render thread, a switch to `dlss` and back
  remade them. Zero refusals, zero stood-down eyes, zero lines carrying
  the port's own text. **Price**, full region, median per stereo pair:
  `fsr 3.1.2` 1.6 to 2.1 ms (p95 2.0 to 2.8); `full-frame ngx` 2.9 to 3.0
  at 4072x3394 and 3.6 at 4100x4050; the pass's own history 0.5 to 1.0 ms
  in `other`; 83 to 89 fps throughout. **Sean's verdict: slightly better
  than TAA, not as good as DLSS**, which is the expected standing of FSR
  3.1 against DLSS 3.x; route 1's value is the AMD user, who has no DLSS.
  So flight 1 answers all three of its questions: it runs, it costs about
  two thirds of NGX at these sizes on this card, and it beats the pass's
  own history. Two nits: the create's VRAM delta prints -202, -56, -31,
  -12 or +0 MB (`QueryVideoMemoryInfo` does not move at create time;
  replace with a computed estimate from the surface descriptions, or drop
  it); and the flight ran the upscale path, which the plan had reserved
  for flight 2, so flight 2's remaining question is only the reactive
  mask. Ruled out: nothing.
- 2026-09-17 06:07, flights 2 and 3 in one session (e70a44e, Frontier,
  Quest 3 through VirtualDesktopXR at 100% = 3072x3264 per eye, RTX 5090;
  log `edvr_gfx_20260917_060732`, right build). The warm-up made the 1:1
  contexts at 3072x3264 (3 ms each) and the first treated frame rekeyed
  to 2611x2774 into 3072x3264, as the flight 1 review predicted; the
  trims later moved the key to 3017x2160 into 3550x2542. Price, full
  region, median per stereo pair: 1.1 to 1.4 ms (p95 1.3 to 1.5); the
  native benchmark's GPU p50 2.7 to 3.4 ms a frame; 86 to 90 fps. Zero
  refusals, zero port messages. **Sean's verdict: the UI looks fine
  without the reactive mask**, on the Quest 3 as on the Pimax, so D3
  stands: `advanced.temporal_aa_fsr_reactive` stays off by default and
  live. Ruled out: the reactive mask as a needed input for Elite's HUD
  under FSR 3.1, because both headsets read fine without it.
- 2026-10-23: route 2, FSR 4.1 through the D3D12 bridge. Sean's ask: FSR
  4.1 alongside FSR 3.1, flat and VR, on his Strix Halo (Radeon 8060S,
  RDNA 3.5). **Facts (read 2026-10-23, vendor-stated unless marked):**
  FSR 4.1.0 shipped in FidelityFX SDK v2.2.0 (2026-03-23), 4.1.1 in v2.3.0
  (2026-06-24, the latest tag), which added FSR 4.1 on RX 7000 (RDNA 3,
  INT8 model; RDNA 4 runs FP8). AMD's SDK: "FSR4 requires an AMD 7000
  series discrete GPU or AMD 9000 series GPU or later" (`ml.md`); GPUOpen
  puts RDNA 3.5 and RDNA 2 on the FSR 3 path, and the API "will
  automatically select AMD FidelityFX Super Resolution 3.1.5" on other
  hardware. So **RDNA 3.5 is not officially covered**. Files:
  `Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll` (the app
  calls it) and `amd_fidelityfx_upscaler_dx12.dll` (28.8 MB, beside it);
  ffx-api headers `Kits/FidelityFX/api/include/{ffx_api.h,
  ffx_api_types.h, dx12/ffx_api_dx12.h}` and
  `Kits/FidelityFX/upscalers/include/ffx_upscale.h`. No Vulkan FSR 4.
  Licence: `docs/license.md` opens with a binary-only redistribution
  licence and then lists every file, the signed DLLs included, under MIT;
  ambiguous, so read before shipping (D8). **Proton:** Valve's Wine has
  an `amdxc64` shim (proton_11.0, experimental_11.0) whose
  `UpdateFfxApiProvider` loads AMD's driver-side `amdxcffx64.dll` (FSR
  4.1.1, bundled by Proton Experimental and Proton-CachyOS) when
  `FSR4_UPGRADE=1`; Experimental auto-enables it only for discrete Navi3x
  ids, not Strix Halo's 0x1586. vkd3d-proton 3.0 maps AGS WMMA to
  `VK_KHR_cooperative_matrix` (RADV GFX11+ since Mesa 23.3); gfx1151 has
  no FP8, so it would take INT8. Believed (community reports, no gfx1151
  report on 4.1.1 found): the upgrade path runs the INT8 model on RDNA 3.5
  iGPUs at about 25% fps cost. Precedent: OptiScaler's "FSR4 w/Dx12" for
  D3D11 games is this bridge (shared NT-handle textures, shared fence),
  and runs under Proton.
  **Decisions (Sean, 2026-10-23):** **D7** `fsr4` is its own value
  ("FSR 4.1" in the menus) and runs only when AMD's runtime offers a 4.x
  provider for the device (`ffxQueryDescGetVersions`); otherwise it refuses
  once, naming what was offered and, on Wine, the `FSR4_UPGRADE=1` launch
  option, and the frame falls back as every refusal does. It never runs
  3.1.5 in `fsr4`'s name. **D8** this pass ships a dev install only
  (`tools\fetch_fsr4.py`, the build, the workflow, `install_edvr.py
  --fsr4`); the installer payload and release packages wait for a working
  flight and Sean's read of the licence. **D9** the branch is not merged to
  main until Sean says it is ready to ship to users.
  **Design:** a fourth engine, `src\d3d11\fsr4_engine.{h,cpp}`, one context
  per upscaler slot (0 and 1 the eyes, 2 the VR world route; flat uses 0).
  The bridge is XeSS's (`xess_engine.cpp`: D3D12 device by LUID, shared
  textures with `SHARED_KEYEDMUTEX | SHARED_NTHANDLE`, the shared fence,
  an allocator ring), copied rather than factored so the flown XeSS path
  is not touched; factoring both onto one bridge is a follow-up. Inputs
  follow route 1's measured contract (jitter and motion vectors in render
  pixels, scale 1, current to previous; reversed Z; infinite depth and the
  HDR route's HDR bit as `fsr` sets them), believed to carry over because
  ffx-api's upscale contract is FSR 3.1's. FSR 4 differs in three places:
  automatic exposure on both routes, not only the HDR one (AMD's `ml.md`
  makes the exposure input optional only under `AUTO_EXPOSURE` and
  recommends it; route 1's LDR route has neither, which FSR 3.1 accepts);
  LDR colour (display-referred, UNORM) is declared non-linear sRGB
  (`NON_LINEAR_COLORSPACE` at create, `NON_LINEAR_COLOR_SRGB` at dispatch)
  as AMD's FSR 4 notes ask; and inputs are copied into shared textures of
  exactly the render size, because FSR 4's auto exposure reads the whole
  texture (OptiScaler's finding). The D3D12 device is made at feature
  level 12_0 (FSR 4 needs Shader Model 6.6), and AMD's loader is never
  unloaded. No reactive mask, no warm-up and no sharpening in this pass.
  **Flight 5 signatures** (Strix Halo, Proton Experimental or
  Proton-CachyOS, `FSR4_UPGRADE=1 %command%`, flat then VR): the
  provider line names 4.1.x (upgrade path works) or the refusal names
  3.1.5 only (it does not; ruled out without the launch option is the
  control); a bridge refusal names the failing call and HRESULT (shared
  texture or fence plumbing under DXVK/RADV); a stall shows as the
  bounded fence wait's line or a log that stops mid-frame (the D3D11-side
  GPU wait on the D3D12 signal; OptiScaler waits on the CPU on Linux for
  this reason); ghosting or doubled edges point at the jitter or motion
  sign, which route 1 measured but route 2 has not; the 30 s bridge line
  gives the cost.
- 2026-10-23 evening: route 2 built and installed for flight 5. Windows
  workflow run 40031975573 green at 5dcc0608 (runs 40026994003 and
  40029535701 failed on two rig checks the new case moved: the HDR route's
  source count and the flat resolver's key-off golden order; both fixed in
  the rigs, no golden re-recorded). On the runner's Microsoft Basic Render
  adapter (vendor 0x1414) the rig made the D3D12 device, loaded AMD's
  signed loader and got "FSR 3.1.5, 2.3.4" from the version query, so
  `fsr4` refused as D7 says: the first evidence that the bridge's probe
  works against AMD's real DLLs. In that log `ffxConfigure rc=2` is the
  signed loader declining the context-less debug route; AMD's per-context
  messages still reach the log through the create descriptor's
  `fpMessage`, so the line's "routed" overstates it (fix its wording with
  the next source change). Installed on the Strix Halo (Steam, flat scope,
  `--fsr4`, tag ci-40031975573) from the run's `edvr-build` artifact plus
  `fetch_fsr4.py`'s DLLs and a `build\edvr-flat.ini` written by
  `gen_installer_rc.flat_ini_bytes()` (the artifact carries neither);
  `--verify-only` clean; `edvr-flat.ini` set to `temporal_aa = fsr4` (one
  line, diffed). Elite runs Proton Experimental 11.0-20261014, which ships
  `contrib/amdxcffx64.dll` (FSR 4.1.1) and the `amdxc64` shim; the launch
  option `FSR4_UPGRADE=1 %command%` is Sean's to set. CI branch builds are
  stamped 0.0.0-ci, so the flight log's build check is `edvr_log.py
  --expect-build 0.0.0-ci`, with the install receipt's hashes as the
  stronger tie to this run.
- 2026-10-23 night, flight 5 (ci-40031975573 at 5dcc0608; Steam, flat,
  Strix Halo, Proton Experimental, `FSR4_UPGRADE=1` read from the game
  process's environment; log `edvr_gfx_20261023_203819.log`, build check
  passed). **FSR 4.1.1 is offered and runs:** "AMD's runtime offers FSR
  4.1.1, 3.1.5, 2.3.4 for this device", provider 4.1.1 on every context.
  2560x720 to 5120x1440 (2x a side), LDR, infinite depth, flags 0x138.
  Every dispatch rc=0, no message from AMD's runtime, no refusal, no
  stall (dispatch #3000 reached); jitter live at #3000 (0.000, -0.167).
  About a dozen live switches among fsr4, fsr, dlss, on and xess
  destroyed and rebuilt the context cleanly (2.5 ms; 130 ms the first
  time). The bridge line over 1799 evaluations, median/p95: GPU 2.8/3.1 ms
  for the whole step, of which the D3D12 dispatch 1.7/1.7, copy-in 0.39,
  copy-out 0.18; render-thread CPU 6.3/7.1 ms, with the allocator wait 0
  and the submit 0.013, so the CPU goes elsewhere in the evaluate (the
  D3D11 flushes, the timer queries or ffxDispatch's recording under
  vkd3d-proton; not split yet). Hidden at 60 fps here; split it before
  calling FSR 4 cheap. DXVK reports the GPU as vendor 0x10DE
  (`DXVK_ENABLE_NVAPI=1`) with the real device id 0x1586; AMD's runtime
  offered 4.1.1 regardless. The camera position was constant in every
  sample read, so motion was barely exercised; Sean has not yet judged
  the image.
- 2026-10-23 night, the next launch crashed (`edvr_gfx_20261023_204910.log`,
  same build). Run 1 had exited cleanly (breadcrumbs: "process exit" at
  20:48:21) before the machine suspended and resumed (20:48:24 to 20:48:26).
  Run 2 died 21 s in, at the main menu (its game journal holds only the
  Fileheader): `UNHANDLED exception 0xC0000005 at 0x144BD17A0 in
  EliteDangerous64.exe +0x4BD17A0`, watchdog exit code 3221225477. The
  faulting instruction is `vmovups ymm2,[rcx-0x20]`, the head of an AVX2
  loop over 96-byte records, in function 0x4BD1550 (its .pdata chunk). No
  direct call or jmp reaches it; the file holds one absolute pointer to it,
  so it is called through a table or vtable. Ruled out: FSR 4 as the
  cause, because run 2 has no `fsr4:` line (the probe, whose first line is
  the adapter match, never ran, so no D3D12 device existed), the flat
  runtime treated no frame (`treated=0`), and the log, flushed every
  250 ms, runs to 1.2 s before the fault. **Whose code (2026-10-24, READ
  from the exe's RTTI):** the pointer sits in slot 11 of the vtable whose
  locator names `MaskedOcclusionCullingAVX2::MaskedOcclusionCullingPrivate`
  (base `MaskedOcclusionCulling`): Intel's Masked Occlusion Culling
  library. The loop (INFERRED from the instructions) is its hierarchical-Z
  test: per 96-byte tile, two depth vectors and a mask, blended by mask,
  min, then `zMax >= zBuf`; a read past the tile buffer means the buffer
  was freed or sized for another resolution. **What differed:** the
  ultrawide (DP-1) disconnected at 20:48:22, before the suspend; run 1
  opened at 5120x1440, run 2 at 2560x1600 (the laptop panel) and resized
  to 2560x1440 and then 1x1601 within a second, with the game's
  `Settings.xml` now present (run 1: absent). Leading suspicion: the
  game's culler set up for a degenerate window. Weaker lead: the light
  grid cache (`docs\light-grid-cache-2026-10-13.md`; no key, always on,
  detours the cluster builder 0x58B250), which ran at the main menu in
  run 2 (contexts 1920x1080 and 256x256 at 20:49:27) and not in run 1.
  Not checked: whether EDVR's kept display mode made the 1x1601 resize.
  Set aside by Sean on 2026-10-24 in favour of FSR 4; the next launch on
  the same panel (03:10) did not crash.
- 2026-10-24: the render-thread stall. Log `edvr_gfx_20261024_031006.log`
  (same build, laptop panel, 1280x800 to 2560x1600, on foot in a
  station). **Same spot, same minute:** FSR 4 35 fps, FSR 3 36 to 38,
  FSR 4 again 35 to 38 (5 s frame counts across Sean's F8 switches at
  03:12:36 to 03:12:57); in space both held the 60 fps cap. FSR 4's
  D3D12 dispatch is 0.80 ms there (1.7 ms at 2560x720 to 5120x1440); the
  render thread spends 3.2 to 4.3 ms a frame in the evaluate (6.3 to
  7.9 ms on the ultrawide), with the allocator wait and the submit ~0,
  and the span shrinks and grows with the GPU work (3.4 ms when the
  ultrawide's dispatch fell to 0.75 ms): a wait on the GPU, not work.
  Ruled out: a newer Mesa as the fix for this cost, because Mesa 26.2.5
  is current in every repository here (only `mesa-git` 26.3 is newer)
  and the GPU part is under 1 ms at the panel's size; the time is the
  render thread waiting. **Mechanism found in DXVK's source** (Proton
  Experimental ships DXVK v3.1.1-97-ga78a467ca): `IDXGIKeyedMutex::
  ReleaseSync` calls `WaitForResource(image, SynchronizeAll,
  READ_WRITE)`, a CPU wait until the GPU has finished every pending use
  of the texture; `AcquireSync` is a CPU-side lock; D3D11 fence
  `Signal` and `Wait` are GPU-side. The bridge releases its three input
  keys right after queueing the input copies (so the render thread waits
  for the game's frame so far) and the output key after the output copy
  (so it waits for FSR 4 too). D3D12 never uses the keyed mutex: the
  shared fences already order the work. The XeSS bridge does the same.
  **Instrument (flight 6):** the evaluate's CPU span split into phases,
  one 30 s line `fsr4: slot=N cpu phases over ...` beside the bridge
  line, and the temporary key `diag.fsr4_shared_locks`: `held` keeps a
  context's keys from its first use to its destruction, `alternate`
  flips between per-frame and held at each 30 s window. Signatures: H1
  (the releases are the stall) `give-in` and `give-out` carry most of the
  eval median under `locks=per-frame` and fall to ~0 under `locks=held`,
  with the evaluations a second rising at a fixed spot; H2 (AMD's
  recording) `record` carries it under both; H3 (DXVK's flushes)
  `signal-in` or `signal-out` carries it. Held keys that broke the
  interop would show as a stale, black or flickering image only in the
  held windows, or refusals in the log.
- 2026-10-24: flight 6, H1 confirmed. Log `edvr_gfx_20261024_060947.log`
  (the CI build of c967b6ca, v0.0.0-ci, `locks alternate` logged; laptop
  panel, out 2560x1600, in 1280x800 then 2176x1360 after a quality
  change at 06:13:22; read mid-flight, five windows to 06:13:57; every
  dispatch rc=0, no refusals). Medians, ms:

  | window | locks | evals/s | eval | give-in | give-out | record |
  |---|---|---|---|---|---|---|
  | 06:11:27 (79 s, load) | per-frame | 15.3 | 3.620 | 2.228 | 1.229 | 0.076 |
  | 06:11:57 | held | 60.0 | 0.245 | 0.000 | 0.000 | 0.184 |
  | 06:12:27 | per-frame | 52.3 | 6.594 | 4.886 | 1.323 | 0.170 |
  | 06:12:57 | held | 59.7 | 0.271 | 0.000 | 0.000 | 0.199 |
  | 06:13:27 (2176x1360) | per-frame | 46.9 | 7.042 | 5.341 | 1.336 | 0.176 |
  | 06:13:57 (2176x1360) | held | 58.4 | 1.619 | 0.000 | 0.000 | 0.263 |

  The two key releases are the stall: under per-frame they are 95% of
  the evaluate; under held it falls from 6.6 to 0.27 ms and the game
  reaches its 60 fps cap. FSR 4's GPU side did not change (D3D12 wait
  1.2 to 1.4 ms, d3d11 total 1.4 to 1.8 ms in both modes). Ruled out:
  AMD's recording as the stall (H2), because `record` is 0.08 to 0.26 ms
  under both; DXVK's fence flushes (H3), because `signal-in` and
  `signal-out` stay under 0.01 ms. Sean, after the flight: the image
  looked mostly clean, with no stale or black frames; certain parts of it
  flicker, which DLSS on his Windows rig does not show. Not yet tied to
  the held windows. Candidates, each with what would tell it apart:
  (a) the jitter cycle: FSR 3 and FSR 4 get the configured 8 phases
  (`flat jitter: ... phases=8` in this log) where AMD asks for 8 x the
  scale squared, 32 at 1280x800 to 2560x1600 (XeSS-SR alone gets a
  scale-aware count): flicker on fine static detail with an 8-frame
  period, fixed by `advanced.temporal_aa_jitter_phases = 32` (live
  reload, no build); (b) no reactive or transparency mask (both routes
  pass none): flicker or smear on holograms, screens, particles and
  glass, unchanged by (a); (c) the held locks racing the copies: flicker
  that comes and goes with the 30 s `alternate` windows; (d) FSR 4's
  model on RDNA 3.5 (`FSR4_UPGRADE`): FSR 4 flickers where FSR 3 and
  XeSS-SR at the same spot do not, and (a) does not help. A DLSS rig on
  Windows differs in GPU, platform and jitter use, so it is not a control
  here; the same-spot F8 comparison on this rig is.
- 2026-10-24: flight 7 (log `edvr_gfx_20261024_063126.log`, same CI
  build, `locks alternate`). A reproducible spot: the main menu hangar,
  where only the lower edge of one square hull panel flickers (Sean's
  screenshot). FSR 4 ran 2176x1360 to 2560x1600 (1.18x per axis). Ruled
  out at this spot: the jitter cycle (a), because 32 phases took effect at
  06:32:03 (`flat jitter: the cycle is 32 phases`) and the flicker did not
  change; at 1.18x AMD's own count is about 12, so 8 was near it anyway.
  The 2x case (1280x800) is untested. Then, same flight: ruled out (c),
  the held locks, because the flicker is constant, not in 30 s windows;
  ruled out (d), FSR 4's model, because the edge flickers under FSR 3
  (06:39:52 on) and EDVR's own TAA (06:39:46) too, so it is in what all
  three temporal paths share. F7 stops the jitter as well (phase back to
  (0,0), warming), so "everything flickers with AA off" is the game's own
  image. XeSS-SR was not tested: `libxess.dll is missing beside EDVR`
  (the flight 6 install had no `--xess`), so it fell back to spatial.
  The scene is still: `pool records joined 417600 (with motion 0)`,
  `flat camera origin 5s: frames=300 moved=0`, no history resets. Yet the
  refusal census the NumLock press armed (06:38:34 to 06:38:44, FSR 4)
  counts about 250 to 340 refused pixels a frame, classes `stale` (the
  steady-detail depth check failed: last frame's depth at the reprojected
  spot is off by more than 1%) and `range` (the reprojection leaves the
  screen or is not finite); a refused pixel shows the raw jittered frame
  under every engine. Lead, not yet placed on the edge: (e) EDVR refuses
  that edge's pixels, at a depth edge the jitter moves. Discriminator:
  `advanced.flat_refusal_view` at the spot (pink stale refused, white any
  other refusal), and TAA or FSR 3 at the same spot on Windows. Aside:
  the adapter reports vendor 0x10DE with device 0x1586 (the 8060S), a
  spoofed NVIDIA vendor, which matters to anything gated on vendor id.
  Then Sean, refusal view on at the spot: the flickering edge is yellow,
  a stale slot KEPT (`flat_mono_refusal.h`: the pool slot under it is a
  draw's that a later draw covered, and the steady-detail check kept the
  camera-term history, confirmed by last frame's depth). Ruled out: (e)
  as stated, EDVR refusing the edge, because its pixels are kept, not
  refused; the ~300 refused a frame are elsewhere. So the edge has
  history and zero motion under all three paths and still flickers.
  Left: (f) the edge's own content defeats a non-ML history clamp and
  FSR 4 alike (a thin feature the jitter samples in some phases only;
  DLSS holds it), or (g) the covering draw's pixels differ frame to
  frame beyond the jitter (a jitter or camera mismatch on that draw).
  Discriminator for platform against content: TAA and FSR 3 at the same
  spot on Windows. Not asked yet: whether the yellow stays yellow or
  alternates with pink.
