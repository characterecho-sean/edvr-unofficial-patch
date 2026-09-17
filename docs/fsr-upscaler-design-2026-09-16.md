# FSR as the every-vendor upscaler: a design (2026-09-16)

Sean's ask: add the latest FSR 3 so that AMD GPUs get the upscaling that
`temporal_aa = dlss` gives RTX cards, and have it work on NVIDIA as well.
Design only; nothing here is built. Claims about EDVR cite the source;
claims about AMD's SDK are vendor-stated (its repository, headers and pages,
read 2026-09-16) unless marked believed.

## Status

- **State (2026-09-16 night): route 1 BUILDING on the branch, not flown,
  not on main.** The ask reframes on one
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
- **Recommendation:** route 1 first, as `fix.temporal_aa = fsr`, a third
  engine at the seam where the pass already chooses between NVIDIA's history
  and its own (section 3); route 2 as a fourth engine (section 5) once route
  1 has flown on both rigs and an AMD supporter has reported.
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
- **Next:** the adversarial review is done and its findings are fixed
  (journal); the branch waits on Sean's go for an install, then flight 1
  (section 4).
- **Open:** the reactive mask under FSR (off in flight 1, D3); the jitter
  phase count above 1:1; FSR 3.1's quality in VR against the pass's own
  history, which is what an AMD user gets today. CLOSED by Track A: the
  build recipe (CMake, `/MT` forced, the upscaler-only targets) and the
  d3d11-import stop of 3.5 (neither library imports d3d11, dxgi or
  d3dcompiler by name). DECIDED (D6): the port's prebuilt shader compiler
  is accepted as a build-time-only tool.
- **Ruled out (2026-09-16, vendor-stated):** an official AMD D3D11 backend
  (none exists); FSR 4 on D3D11 (D3D12-only signed DLLs, RDNA3/RDNA4 only);
  reaching FSR 4 through the driver's FSR 3.1-to-4 override (D3D12 ffx-api
  titles only); FSR 3.1.5 on D3D11 (it exists only inside the D3D12 DLL,
  and its one change over 3.1.4 is a negative-RCAS-output fix, moot with
  sharpening off).
- **Environment:** every engine runs inside d3d11.dll on the game's D3D11
  device; the native OpenXR half (src\openxr\) is the only packaged VR half.
  Field rigs: an RTX 5090 with a Pimax Crystal Super and a Quest 3; the
  Steam rig also carries an AMD iGPU (gate on the DXGI vendor id, never on
  adapter count). No AMD discrete card in-house: AMD verification needs a
  supporter's log. FSR needs typed UAV loads (vendor-stated); cards without
  them (believed: NVIDIA before Maxwell) must get a clean refusal.
- **Detail:** 1 what exists; 2 what AMD ships; 3 the design; 4 flights;
  5 the sidecar; 6 decisions; 7 declined; journal at the end.

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

`fix.temporal_aa_model` (k, j, l, m, auto) stays NVIDIA's; under `fsr` it
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
--grep "temporal aa"`.

- **Flight 1: Sean, RTX 5090, Pimax at the eye-mask flights' resolution,
  `temporal_aa = fsr`, HMD Quality 1.0, reactive off.** Must show
  `temporal aa: fsr 3.1.2 (d3d11 port <sha>) contexts made for both eyes,
  WxH -> WxH, flags <list>, +N MB, create X ms`; the price line naming
  `fsr`; no "The pass's own history runs instead"; no FFX message lines.
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
