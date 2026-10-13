# XeSS for flat EDVR on Intel B390

## Status

- State: round-2 Windows review and test correction pulled at 770069b6. The
  reviewer reports a green Windows 11/MSVC/RTX 5090 full build (140 rigs,
  receipt) and four hardware XeSS flights for that tree. New lifecycle,
  diagnostic and input/install follow-up compiles and passes focused local
  checks; its native Windows full validation and gameplay remain pending.
- Local check: round-2 backend passes isolated MSVC compilation and five Intel
  B390/Proton GPU flights, including a latched failure and device replacement.
  D3D11 key acquisition preserves the measured DXVK shared-resource flags.
- Prior final gameplay: T9 <-> Lynx automatic recovery passed on the verified
  dac1916e-dirty DLL, PE 6ACD6A82, 174055.log: 12,198 treated frames, later
  4,812-frame streak, zero backend failures. It was not a clean fd26814e build.
- Environment: ONEXPLAYER 3, Intel Arc B390 PTL 8086:b080, CachyOS xe, Proton
  Experimental experimental-11.0-20261001-x86_64, DXVK
  v3.1.1-47-g685301564ea3486, private Mesa 26.3.0-devel git-37bf953a58; flat
  only, no headset/eye targets.
- Current Elite file: Custom preset, BloomQuality 0, DOFEnabled 0, AOQuality 0,
  BlurEnabled false, SSAAMultiplier 0.65. Custom.4.4.fxcfg mtime is October 11
  22:32:20 UTC. Earlier captures did not snapshot those settings independently.
  Current files cannot prove every historical flight's settings.
- Branch: flat fix.temporal_aa=xess, pinned SDK v3.0.2 at
  8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0, SR runtime 2.0.2; D3D11/D3D12
  bridge and payload notices. XeSS is flat only; existing AA modes/defaults
  remain.
- Confirmed camera causes: omitted H inverse phase (070705); unrelated 256x256
  domain veto (162004/trace6060); missing paused shadow/context (165802).
  Current proof and ownership repair has no ship/size whitelist. Standdown
  recovery retains the 1.5-second probe cadence, with no GPU work in probes.
- Open hypotheses: final Type 11 behavior, native hangar-surface flicker,
  graphics-setting dependence across systems, bit-1 propagation/culling,
  on-foot weapon continuity at SS < 1, and XeSS GPU-copy cost remain unflown.
- Ruled-out pointer: capability, first-gameplay, nonzero-T9 propagation,
  secondary-domain and cold-probe entries below contain the dated exclusions.
  The Windows comparison did not control OS, GPU or post settings together.
- Next flight: after follow-up checks, test on foot with weapon raised at SS <
  1, then Type 11 load/switch and Off -> XeSS. Windows comparison should cover
  Bloom 3 / DoF 2 and off, with settings recorded. Bit-1 A/B needs a separate
  instrumented culling check; it is not enabled in this repair.
- Build handoff: fetch tools/fetch_xess.py and tools/fetch_ffx_dx11.py pinned
  dependencies, run normal build.bat by absolute path, require the receipt. New
  source has not inherited the prior Windows receipt.
- Review policy: retain controller support explicitly requested by the user;
  retain bounded diagnostics pending Type 11 evidence. Keep exact UI and
  deferred-consumer admission; unknown UI shader pairs can be logged safely.
- Local input setup moved to onexplayer-elite-local-setup-2026-10-11.md. Mesa
  and MinEdLauncher setup remain in planetary-startup-intel-b390-2026-10-11.md.
  Temporary paths identify historical artifacts rather than portable evidence.

## October 11: D3D11 capability probe

The latest official SDK was fetched by tag into /tmp. libxess_dx11.dll has
SHA256 c7cfe86f0c9d94e4fb3696d3cd5035e2bbb6a8b1b0572f8b7395a4cdfd0c625e. An
official, SHA-verified Windows Python 3.14.7 embeddable package ran a ctypes
probe in a fresh disposable Proton prefix through Steam Runtime 4. The private
ICD was selected before the container started. No game files, live prefix, EDVR
config or rendering source were changed.

D3D11CreateDevice returned S_OK with feature level 11_0. xessGetVersion
returned success and version 2.0.2. xessD3D11CreateContext then returned
XESS_RESULT_ERROR_UNSUPPORTED_DRIVER (-2), not unsupported device (-1). This is
an SDK driver-path refusal after successful D3D11 device creation. It does not
establish that every XeSS API path is unusable on this hardware.

Results: /tmp/edvr-xess-probe-20261011/pywin/probe-result.jsonl. Driver
evidence: /tmp/edvr-xess-probe-20261011/logs/python_dxgi.log and
python_d3d11.log. The Windows process does not retain the Linux ICD variables;
DXVK's driver identity establishes the effective selection.

DXVK v3.1.1 exposes IDXGIVkInteropDevice, IDXGIVkInteropSurface and
IDXGIVkInteropFactory. They provide device/queue/image handles and explicit
flushing, layout transitions and queue locking. A Vulkan integration must
respect those ownership and synchronization rules and XeSS's required enabled
features. No Vulkan XeSS context or execution has yet been tested.

## October 11: Vulkan prerequisite gate

The SDK v3.0.2 required-query APIs returned success. They require Vulkan API
1.3.0, no instance extensions, and device extension
VK_EXT_mutable_descriptor_type with
VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT.mutableDescriptorType=true.
DXVK's D3D11/Vulkan interop QueryInterface calls succeeded and the returned
device/factory handles agree, but the DXVK device's 44 enabled extensions omit
VK_EXT_mutable_descriptor_type. XeSS Vulkan context creation was not attempted:
the existing DXVK device does not meet the SDK's prerequisites.

Native private Mesa on B390 advertises the extension at revision 1 and the
mutable-descriptor feature. It also reports
shaderStorageImageWriteWithoutFormat=true and Vulkan 1.4.363. This confirms the
native driver advertises the queried requirements; it does not establish Wine
or DXVK execution support. The following standalone probe confirms Wine
execution, but does not change the DXVK device's missing prerequisite.

Evidence: /tmp/edvr-xess-probe-20261011/probe-vk-result.jsonl, python_d3d11.log
and native-vulkaninfo.log.

## October 11: standalone Wine Vulkan execution

A standalone Wine Vulkan device enabled the required
VK_EXT_mutable_descriptor_type extension and mutableDescriptorType feature.
XeSS Vulkan context creation, initialization and execution all returned 0;
queue submission and wait also returned 0. The probe read back 524,288 bytes
for a 256x256 RGBA16F output. Its cleared 112x112 RGBA16F input was (0.25, 0.5,
0.75, 1), with zero high-resolution motion vectors; the first output pixel
matched the input.

This confirms basic XeSS Vulkan execution with the private Mesa B390 driver in
standalone Wine. It does not measure temporal image quality or validate
execution on DXVK's existing device, whose enabled extension set lacks the
required mutable-descriptor extension.

Evidence: /tmp/edvr-xess-probe-20261011/pywin/
probe-vk-independent-result.jsonl.

The next discriminator is an established D3D11/D3D12 shared-texture and fence
bridge on the same Proton stack. OptiScaler provides a reference path for
running D3D12-only XeSS behind a D3D11 game device; test that bridge here
before considering a DXVK extension change or EDVR integration.

## October 11: bridge implementation and local validation

The D3D11/D3D12 bridge probes passed shared-texture opening for RGBA8, RGBA16F,
R11G11B10F, R32F and RG16F, shared-fence producer/consumer ordering,
same-adapter matching, XeSS execution and D3D11 output readback. This removes
the need to modify DXVK's enabled Vulkan extensions. Probe results remain in
/tmp/edvr-xess-probe-20261011/pywin/probe-bridge-result.jsonl and
probe-adapter-pair-result.jsonl. The actual loaded private Mesa path was also
verified in bridge-loader-debug.log.

The branch backend uses low-resolution motion in render-pixel units, inverted
depth, matched HDR R11 input/output and a precompiled R11-to-FP16 copy into
EDVR's output. It keeps three D3D12 command allocators and shared fence values
for safe reuse. Feature destruction waits for outstanding copies, including
failed evaluations. Slot zero and the original immediate context/render thread
are required.

The disposable hardware rig passed native LDR 640x360, 2x LDR 1280x720, 2x HDR
1280x720 and HDR release/recreation, five frames each. Evidence:
/tmp/edvr-msvc-bootstrap-20261011/current-check/xess-gpu-run-1/gpu-test.log.
These tests establish basic execution and synchronization; an Elite flight must
still establish real camera inputs, route selection and visible quality.

The published SDK jitter requirement exposed an integration gap before the
first flight. The XeSS route now derives its minimum cycle from actual planned
render/evaluation sizes and resets history when the cycle changes. Tests cover
native, 2x, 3x, anisotropic, invalid sizes and a 72-phase cycle. Existing
camera lighting routes cannot all produce arbitrary Halton samples; an
insufficient actual cycle returns flat-xess-jitter-cycle-too-short instead of
claiming an active XeSS upscale.

The user approved Microsoft's compiler license and an isolated build-tool
installation. Native Microsoft MSVC, SDK and shader compiler run through Proton
from /tmp/edvr-msvc-bootstrap-20261011. The full absolute-path build compiled
production DLLs, then failed the pre-existing run_jobs self-test on Wine
process control, inherited error mode and Job Objects. The unchanged baseline
reproduced these failures. Both installer builds compile and pass payload
verification; installer_test additionally hits Wine file-sharing semantics. No
full-build receipt is available and no full success is claimed. The branch is
for local device testing first and later native Windows validation, with no
merge to main.

Final source compiles with the XeSS jitter minimum and the permanent refusal
after any unfenced submission. temporal_test, flat_temporal_test and the XeSS
rig's CPU checks pass. flat_mono_resolve_test compiles but fails 17 Wine/WARP
rendering assertions; the unchanged 99476a02 baseline produces the same 17
failure strings, including identical key-off golden hashes. Evidence:
/tmp/edvr-msvc-bootstrap-20261011/baseline-flat_mono_resolve_test.log and
flat_mono_resolve_test-final.log. The real game's current graphics files say
1920x1200, supersampling 1.0 and AAMode 0; the flight log must establish its
actual R/E/D grids.

## October 11: first Elite flat flight

The sanctioned log reader verifies source commit 644fe39b in
edvr_gfx_20261011_093635.log. The user reports that the menu looks great, but
XeSS appears absent on most gameplay elements. The menu uses the native HDR
route at R=E=D=1920x1200 with SDK AA quality, eight jitter phases and
successful D3D12 initialization. XeSS executes through 3115 evaluations with no
SDK/fence refusal. Repeated menu windows treat every HDR trigger frame.

Around 09:38:30, loading changes the observed scene. Treatment stops at 3186
frames. The detailed conflict at frame 6642, q14/q16 is image-copy-source:
source VS CFA91824129ECBBC / PS FCFAD73924BF45B9 writes format 9 with depth but
no captured camera; the PS DFCBA0EC70B03C9B copy writes format 26 without
depth. The later half-size HDR consumer is VS DFED8E1C9E191BEC / PS
14C60393D1FC9766. HDR admission refuses before the backend; the copy route has
no known tone pass and the runtime eventually stands down.

Ruled out: XeSS runtime/bridge failure as the cause of these untreated gameplay
frames, because menu initialization/execution succeed while the gameplay
backend counter remains zero. This flight does not test gameplay temporal
quality or any R<D XeSS upscaling. FSR3 separately refuses because the local
build lacks EDVR_HAVE_FSR3; its fallback is not a XeSS error.

The current image-copy guard in flat_runtime_model.h checks verified actual
bindings, source format/layout/depth/order and source camera when present, then
the destination HDR's existing writes/current camera and matching depth. The
source PS already has a camera-independent recipe; a missing source camera
alone is expected and is not a fix hypothesis. Prior flat-AA section 105
records the same copy guard on another device, with binding checks that passed
but incomplete preceding-H writer evidence. Do not repeat those exceptions or
infer a camera mismatch from the aggregate reason.

This flight saved seven unknown scene-projection pairs (14 DXBC stages), but
neither the new HDR consumer stages nor a replay trace. The existing source
witness is gated on marked overlay ambiguity and recorded zero candidates.
Shift+NumLock full capture in the affected scene is requested. It resumes
stand-down work, samples exact copy provenance and records individual draws and
resource-write markers in FTR5. Accept three complete frames with nonzero
events, no overflow and no short write. Ordinary NumLock is General only and
does not arm the full trace. The offline replay must distinguish copy proof,
source invalidation, source camera provenance, destination initialization and
depth/camera agreement before any production code changes.

The existing flat_temporal_test replay rig rebuilt and passed its CPU
self-test. The disposable runner is
/tmp/edvr-msvc-bootstrap-20261011/run_trace_replay.py; it runs trace-check,
trace-chain and trace-structure without using the live prefix. A separate
read-only guard diagnostic is being prepared under /tmp; it is not shipped.

## October 11: gameplay trace isolates the admission gap

The user identified the cockpit/station scene and completed Shift+NumLock.
Correct installed source build 644fe39b (HEAD 4d8e8ab8 changes only this doc)
wrote edvr_gfx_20261011_100319.log and flat_trace_14850.bin. The trace has
three complete frames, 9963 events, 5021512 bytes and zero overflow slots.
Native guard analysis and the existing Windows flat_temporal_test replay agree:
frames 14847-14849 reproduce their stored contract hashes exactly.

Every frame rejects the HDR image copy. At frame 14847 q2261, 29 of 31 guard
predicates pass. Only destination hdrCamera and cameraCurrent(t->tone, frame)
fail. Source format 9 has verified camera-independent image proof, valid
layout/depth/order and no bad state. H has 13 prior draws: 12 camera-less and
one current-camera laser-pair draw. The unconditional laser exemption prevents
that draw from establishing H's scene tone. No earlier bad witness exists; the
failed copy sets firstBad=image-copy-source. The later known HDR consumer then
refuses H, and the final output reports no-known-tone-pass.

The existing prefix already holds one supported pool record on that same scene
depth with 266 draws and current camera provenance. Its b1, hash and exact 96
camera bytes match the laser and image copy in every captured frame. Two
unsupported format-60 draws have a different camera and are not evidence for
scene ownership. A shared depth alone therefore cannot qualify a donor.

Ruled out: broken actual copy binding or image-source layout/proof, because
their guards pass in all three exact replays. Ruled out: full capture heals the
route, because accepted copies remain zero, gameplay backend calls remain zero
and the runtime stands down again. The camera-independent source recipe is
already correct; adding another shader exception would not fix this gap.

Open hypothesis: the unconditional weapon-shader exclusion also suppresses a
verified scene camera when Elite reuses that pair with scene constants. Judge a
narrow qualification against the existing mixed-camera safeguards before
editing production code. A matching supported source must be current and agree
in exact camera bytes and scene depth/layout; a copy's b1 alone is not scene
proof. No production code changed for the initial analysis.

The narrow model change now establishes tone from the actual H laser draw only
when a unique supported pool source precedes H's first write and matches its
current camera binding/hash/96 bytes, depth/DSV/format, dimensions and full
viewport. Existing differing-camera laser handling remains unchanged. The image
copy's constants are not used to invent a camera. Thirteen CPU scenarios cover
the captured ordering and absent, cohort, stale, mismatched, ambiguous, mutated
or late source evidence; the qualifying draw's own sequence and write
provenance are asserted. Native g++ compilation and these fixtures pass.

Replay using the changed production headers now selects the HDR route in all
three captured frames: q2438/2359/2289, scene 1920x1200, first consumer
960x600, 266 supported source draws and the captured scene depth/camera. Each
image copy is accepted once, refused zero times. The final mono-copy contract
hashes still match: its no-known-tone-pass result is unchanged because this
frame is served by the preceding HDR route. This proves the admission change on
the captured draws, not XeSS execution or image quality in a new game flight.

The first MSVC rig compiled and the new fixture passed, but historical
Krait/corpus/detector hash checks failed. Old/new replay found the Krait frames
have no HDR image continuation. Their final copy remained selected, but
unconditional same-scene laser promotion changed an unrelated copy contract.
Ruled out: camera match alone as the complete qualification, because it changes
those three unrelated recorded contracts. No golden was relaxed.

Qualification now also requires one clean, verified camera-independent format-9
source on the same depth/DSV/layout, ordered after H's first write and before
the laser draw. This is the captured image-continuation sequence. All 17
committed traces (46 frames) now reproduce their hashes exactly, while the new
cockpit/station capture still selects the HDR route in all three frames.
Fourteen fixture scenarios pass, including a new absent-image-source refusal.
Targeted MSVC flat_temporal_test now passes, including all historical corpus
and existing second-camera weapon checks. Windows replay independently confirms
image accepted=1/refused=0 and HDR selected=1 in all three current frames, with
no selected conflict and matching stored mono-copy hashes.

The full build.bat ran by absolute path and compiled both production graphics
DLLs, then exited 1 at the previously demonstrated Wine run_jobs process/focus
and inherited-error-mode self-tests. Its tail was read. No full-build receipt
exists and no full green build is claimed. Logs are
/tmp/edvr-msvc-bootstrap-20261011/laser-flat-temporal-rig-final.log and
/tmp/edvr-msvc-bootstrap-20261011/full_xess_laser_scene_20261011.log. The user
authorized branch-only work pending native Windows validation. Commit this
validated source, rebuild for a clean version stamp, install with the
sanctioned tool preserving the live flat INI, then test this device's same
cockpit/station scene. Gameplay XeSS execution and picture quality remain
unverified until that new flight.

Clean source 0f700706 is pushed to codex/xess-flat; main remains 99476a02. The
normal absolute full rebuild stamped v0.19.1-13-g0f700706 and reproduced the
baseline Wine gate failure. The flat installer separately rebuilt and passed
its payload check. Installed graphics SHA256 is
55e0f1fcef4ad708b857e41c0ca46ebe9e98da322d69c13392401c990d19af92. Sanctioned
dry-run, install and verify-only passed after the user closed Elite and its
launcher. The live flat INI was preserved byte-for-byte (SHA256
007d94135805fcb0d01766fe728c00e64ac2aae5ae74df15f9a9becb6d07ea5a), still
selecting xess. The same cockpit/station scene for one minute is requested;
read that new flight with --expect-build 0f700706, even after doc-only commits.

## October 11: local gameplay execution confirmed

Correct installed source 0f700706 wrote edvr_gfx_20261011_103114.log, version
v0.19.1-13-g0f700706, build 6ACBB8D6, linked 2026-10-11 16:27:02 UTC. The user
reports mostly stable gameplay: one station surface below the hangar's Welcome
to sign flickers at native and stops after lowering supersampling. The log's
1440x900 render and 1920x1200 output imply 0.75. No visual cause is established
by that observation.

At 10:34:10 the native HDR route has 191 frames, selected/admitted/reached/
captured/copied/prepped/backend/finished/restored all 191, no late H writes,
and a 979-frame treated streak. The previously refusing gameplay consumer
DFED8E1C9E191BEC/14C60393D1FC9766 is now selected. XeSS execute #1500-#2400
logs rc=0, 1920x1200 to 1920x1200, HDR, R11-to-FP16 copy and reset=0. History
is live with eight jitter phases. Image-copy refusals stop growing while
accepted continuations increase.

At 10:34:15 the renderer changes to 1440x900. The HDR route deliberately yields
to the structural LDR copy route; XeSS opens a 1440x900 to 1920x1200 Ultra
Quality context. At 10:34:50 all 284 copy frames are admitted/treated with
valid history, 15 jitter phases and no reset that window. Later XeSS executes
through #6600 with rc=0, RGBA8 copy and monotonic fence values. The renderer
reaches a 3487-frame history continuation run. Backend-failure totals grow
during FSR mode comparisons and stay fixed throughout this last XeSS run; do
not mislabel those aggregate totals as XeSS SDK errors.

Ruled out: XeSS remaining inactive after the camera admission fix, because this
correct-build flight executes native HDR and below-native LDR with successful
SDK evaluations and accumulating treated frames. Local scope is confirmed:
flat, this Intel device and private Mesa/Proton environment. Native surface
flicker, XMX acceleration, other gameplay scenes, VR and the native Windows
full build remain unqualified. Reducing supersampling changes both the resolve
position (HDR before tone versus later LDR) and jitter sequence; it is a stable
user setting, not a proven correction of the native artifact. Identify the
surface's draw/placement relative to resolve before another production
rendering change. No source or live config changed for this read.

## Sources

- [Intel XeSS SDK v3.0.2](https://github.com/intel/xess/releases/tag/v3.0.2).
- [XeSS-SR
  guide](https://github.com/intel/xess/blob/v3.0.2/doc/xess_sr_developer_guide_english.md)
  documents Intel-only D3D11 support and separate Vulkan requirements.
- [D3D11 API](https://github.com/intel/xess/blob/v3.0.2/inc/xess/xess_d3d11.h)
  and [result
  codes](https://github.com/intel/xess/blob/v3.0.2/inc/xess/xess.h).
- [DXVK v3.1.1 interop
  interfaces](https://github.com/doitsujin/dxvk/blob/v3.1.1/src/dxgi/dxgi_interfaces.h).
- OptiScaler reference: [D3D11/D3D12 bridge
  (dx11_with_dx12.cpp)](https://github.com/OptiScaler/OptiScaler/blob/master/OptiScaler/with_dx12/dx11_with_dx12.cpp)
  and [XeSS D3D11-on-12 path
  (XeSSFeature_Dx11on12.cpp)](https://github.com/OptiScaler/OptiScaler/blob/master/OptiScaler/upscalers/xess/XeSSFeature_Dx11on12.cpp).

### FSR3 local enablement: current refusal is a missing dependency

The user reports FSR3 still does not work after the verified MinEdLauncher
launch/exit test. Sanctioned reader confirms correct installed source 4b7bfec4
in edvr_gfx_20261011_123101.log. At 12:32:05.684 the F8 mode changes xess ->
fsr; at 12:32:05.717 the fallback says "this build was made without AMD's
upscaler (EDVR_HAVE_FSR3)". The refusal counter reaches 29 at 12:32:06.183. No
FSR3 GPU feature checks or execution are reached. Ruled out: the current logged
refusal demonstrates an unsupported Intel device, because it is the build-time
SDK-absence stub. Actual hardware compatibility remains to be checked after
enabling the engine.

The existing fetch_ffx_dx11.py pins the OptiScaler FidelityFX DX11 port at
9b04fa49abae5aa341c0007af13c8ba1b4ad9913 (FSR 3.1.2), with AMD's MIT notice.
Its offline self-test passes 25 checks. Pinned source is staged at
/tmp/edvr-fsr3-pinned-source and the shader compiler matches the fetcher's
SHA-256 30f9df1412742b7dcd5a8655c309ebfdb2210b070f9e6c1b09290a127c7d6dd7. The
existing fetcher assumes a Windows VS2022 CMake generator; portable MSVC build
adaptation is in progress. No production C++ fix is proposed for this refusal:
verified libraries in third_party/ffx-dx11 enable the existing engine.

Before another flight, a temporary hardware adaptation of the existing FSR3
engine rig will check FL11_1, Options2 typed-UAV additional formats and
individual required format support, then exercise the production engine's
LDR/HDR evaluations and resets in the real DXVK/private-Mesa environment. The
repository rig remains WARP by default for native Windows validation; there is
no Intel vendor exclusion in the production engine. Both the dependency build
and actual GPU results are pending.

## October 11: FSR3 dependency enabled and gameplay confirmed

Installed clean source 75848439, v0.19.1-22-g75848439, includes the pinned
OptiScaler FSR3 3.1.2 libraries. The existing fetcher verifies the staged
dependency, static LIBCMT CRT, imports, pin and shader compiler provenance.
VERSION.txt records CMake 3.31.3, MSVC 14.44.35207 and Ninja, plus a temporary
CMake shader-output-list dereference correction in the dependency build. This
is a build-generation correction, not an EDVR C++ change; the pinned shader
compiler hash remains unchanged. The AMD MIT notice is carried with the build.

The temporary hardware adaptation of the existing FSR3 engine rig passes all 97
checks on Intel Arc B390 through the actual game-profile DXVK/private-Mesa
environment, including production-engine LDR/HDR evaluation and resets. The
repository rig remains WARP by default for Windows validation. The full build
was launched by absolute path and compiled both production DLLs, but unchanged
Wine run_jobs failures still prevent a full-build receipt. A separate flat
installer build and check passed. Sanctioned install dry-run, install and
verify-only passed; the settings hash stayed unchanged. Native Windows full
validation and the user's branch-only/no-main-merge requirement still apply.

The sanctioned reader verifies edvr_gfx_20261011_125209.log against 75848439.
At 12:52:44.611 FSR3 first asks for the Intel Arc B390 and initialises its DX11
backend in 1 ms. At 12:52:44.618 it creates the context for 1248x780 ->
1920x1200, matching game SS 0.65. At 12:52:49.644 the runtime reports
treated-jittered: 281 successful FSR frames since the switch, with one reset
and 280 history continuations. By 12:52:54.649 it has treated 182 more, giving
463 successful FSR frames across the sampled ten seconds and a longest
FSR-treated streak of 463. Tone processing reports trained-upscale, confirming
the output route as well as context creation. No new SDK fallback appears.

The two engine-source-not-ready refusals preceded FSR selection. The later 111
no-3d-scene refusals identify an absent 3D scene, not an SDK failure; the log
alone does not identify which menu or scene transition caused them. The user
confirms FSR3 engagement and stable scene, HUD and menu motion. That is human
visual evidence: HUD counters were not independently measured during the
sampled FSR window, whose initial tone samples had no HUD requests.

Ruled out: the previously observed FSR3 refusal was caused by Intel hardware
incompatibility, because enabling the missing dependency passes the hardware
rig and produces successful FSR gameplay evaluations. This confirms the tested
flat Proton/DXVK environment, not native Windows or an untested VR runtime.

## Local rear-right input setup

The verified device-specific capture and bridge setup is preserved in
[onexplayer-elite-local-setup-2026-10-11.md](onexplayer-elite-local-setup-2026-10-11.md).
It does not add a bundled installer feature.

## October 11: native flat controller navigation

The flat menu adopts focused gamepad navigation alongside keyboard input. The
EXE's XINPUT9_1_0 XInputGetState gate forwards original state and records one
proven slot; only a focused, drawn flat menu consumes it. D-pad moves rows and
changes values, A selects or commits, B cancels an edit or closes, and LB/RB
use the existing page dispatcher. Other controls and VR paths are unchanged,
and no INI key was added; controller privacy is independent of menu.keyboard.
Closing while A/B is held sends a release tail. Final review also covered the
controller footer on actual readiness, reconnect/reselect and priming, and
physical Shift refresh.

The pure `menu_pad.h` model covers mask 0x330F, per-slot isolation, changing
and stable packets, release-tail handling, the original 100 ms ownership expiry
(raised to five seconds in the Windows-review follow-up), held-open
reacquisition seeding, repeat, and action edges. Its native-host rig passes all
45 checks. `input_gate_test` passes; `menu_edit_hold` passes 95 checks and 38
mutation anchors; `hotkey_capture` passes 627 checks. Their logs are under
`/tmp/edvr-msvc-bootstrap-20261011/rig_<label>_20261011.log`. The native menu
rig reports 125 checks and five shared-resource capture/read/pixel failures in
this environment; no baseline comparison establishes them as pre-existing.

The final full absolute build log is
`/tmp/edvr-msvc-bootstrap-20261011/full_menu_pad_shift_final_20261011.log`. It
compiled and linked production `d3d11.dll` and `edvr_openxr_graphics.dll`, then
failed unchanged Wine `run_jobs` process/focus self-tests involving Job
Objects, inherited error mode and window attribution, before pool, packaging
and receipt gates. The complete build is not green.

Reviewed dirty build v0.19.1-26-g85e79e18-dirty was installed through the
sanctioned helper; dry-run, install and verify each exited 0. Source
`d3d11.dll` and `edvr_openxr_graphics.dll` have SHA256
6d297b9a18447263e9953451261ecb80e74d25321720f011e06fb66f46656c89 (18,563,072
bytes); installed `d3d11.dll` matches. The flat INI and LocalAppData mirror
remain at original SHA256
c2ecc7487c6633000b93600a97fbbbffd4928c6b76be61ad9e6c5c2fe3d1c6e5; installation
used no `--ini`.

The human controller flight is recorded in `edvr_gfx_20261011_144257.log` (912
lines, 231.3 KB). The sanctioned `edvr_log.py --expect-build HEAD` matched it:
version v0.19.1-26-g85e79e18-dirty, build 6ACBF433 linked at 20:40:19 UTC. At
14:43:05.686 the EXE XINPUT9_1_0 gate was installed; at .735 Elite slot 0 was
observed and private; at .736 flat pad navigation became active on proven
private slot 0. The log records settings edits during the flight, ending with
temporal_aa=xess, UI 125, sharpness 0.75 and menu F8. It records menu close via
UI_Back at 14:43:17.837. The user confirmed all tested D-pad, value, A/B and
LB/RB controls worked, then confirmed normal Elite controls worked after close.
Controller navigation is human-tested and confirmed. The bridge's
`rear_menu.log` did not yet contain a `CLOSED` line when last read; cleanup at
process exit is not claimed for this run. The full-build gate and five
native-menu shared-resource failures above remain unresolved; no baseline
establishes those failures as pre-existing.

## October 11: Type 11 AA admission capture

The correct installed source `91576e7e` log, `edvr_gfx_20261011_162505.log`,
was verified at HEAD. The docked Type 11 was at the same station; the flight
tried XeSS, TAA and FSR, all reported failed. At NumLock 16:37:40, frames
21036–21039 used XeSS 1248x780→1920x1200 (0.65), UI 125. The capture treated
12,412 frames and refused admission for conflicting HDR target/camera,
subreason `image-copy-source`; every SDK step remained zero.

The four-frame signature was VS `CFA91824129ECBBC`, PS `FCFAD73924BF45B3`,
source `966F0C70`, format 9, depth `7AB55040`; the copy used PS
`DFCBA0EC70B03C9B` to HDR `7AB56300`, format 26, no depth. Later tone VS
`F9CFC798F21E9AEA` / PS `FEE777E92850B390` sampled that same HDR at `@690`.
There were exactly 11 HDR-prefix writes and none was a previously qualified
laser pair. Their scene-camera provenance is unproven: the general census `c=`
field describes VS `CB0`, not VS `b1` or its matrices; a missing `c=` does not
prove the camera is missing.

Ruled out for this refusal: SDK/vendor backend execution, since admission
stopped with every SDK step at zero; map focus, since `focus=0` and
`temporal-aa-off=0`; and history/UI reset after admission, since the SDK was
never reached. Passive discovery truncation belongs to the separate observer,
not the runtime refusal. The unchanged strict guard stays in place until the
required scene-camera field is proven. The requested Shift+NumLock detailed
capture followed on the unchanged scene with existing instrumentation; no new
build or code change.

The follow-up `edvr_gfx_20261011_164313.log` uses verified installed source
`91576e7e`. Replay frames 7793–7795 exactly match stored contract hashes. All
31 image-copy guards pass on each frame (93 pass, zero fail); each accepts the
copy and produces a contract. Runtime had already treated 1,010 frames at
16:45:28; the opening capture line at 16:45:31.482 reports
`last=treated-jittered` and `accepted=1097`. The capture therefore does not
show that it caused recovery.

The accepted destination HDR has current camera and tone. Before the copy, the
camera witness is fixed laser VS `88DCF1164C640EC3` / PS `494506A63091DF8C`;
296 supported pool draws share depth `7AD76140` and those camera bytes. Copy
source `7DB8A0F0` targets HDR `7AD77400`. The first-writer camera-zero
diagnostic is not the final camera. Ruled out: static inability to admit this
image-copy signature, because the fresh trace accepts it. The earlier failed
view remains unresolved; this does not establish that all Type 11 scenes work.

Both complete failed census frames contain zero laser-pair draws, across all
targets; the treated trace contains one per frame in scene HDR/depth. This
supports a view/material-dependent camera-admission gap, without proving
occlusion. The per-frame prefix is replaced with `FlatRuntimePrefix{}` at
`flat_runtime.cpp:3683` for watched frames, so its hdrCamera/imageSourceBad
fields do not retain an earlier frame's values. A failed detailed trace must
distinguish missing current HDR camera, rejected scene-camera proof and
within-frame contamination; no HDR classifier or prefix-reset change is yet
justified.

The user clarified that the detailed capture still looked unfiltered: Elite was
started fresh, AA looked fine in the menu, then failed visually when the Type
11 cockpit loaded. Contract acceptance never established visible AA.

Two separate failures are now distinguished. The original image-copy refusal
still requires failed camera/provenance fields. The detailed audit has an
independent, confirmed failure: under Upstream projection ownership, normal
rendering skips unused legacy preparation, but full audit runs it. Compute
shaders `5998146D464F5C0E` and `EB0245DE0BB23BB6` fail that private preparation
once per frame. Unlike draw refusal, the compute branch calls failPhase
directly. At 16:45:39, all 215 treated frames reset history; history
continuations are zero. XeSS execution 1200 returns 0 with zero jitter and
reset=1. Reset reaches shader flags.x, leaves rejection=1 for every pixel and
the SDK finish selects raw bilinear Color instead of SDK History. This explains
why successful admission and SDK execution still yield an unfiltered capture.

The audit-only failure begins at frame 7793, so it does not explain the earlier
pre-capture report or the original image-copy refusal. Initial private
preparation code 10 is `topology-first-seen-live-no-capability`, later code 8
is InvalidRecipe. The tracked scene CB has 336 constants/5376 bytes and no
private-plan capability. Source anchors: flat_runtime.cpp qualification and
compute failPhase path; flat_mono_resolve.cpp reset propagation; SDK finish in
flat_mono_shader_source.h. The final-copy replay consumes the expected
LDR/output resources, but records no private SDK pixels or substituted SRVs.

The source fix routes the central phase decision through the existing three
Upstream legacy-refusal classes, so compute receives the same protection as
draws. Diagnostics still report unused private preparation; genuine shader,
viewport, extent, depth and foreign-work violations still invalidate history.
Private replacements remain gated off under Upstream ownership. Native phase
tests pass; removing the preparation bypass fails the history-preservation
assertion. Both changed production translation units compile with MSVC, and the
fresh temporal rig preserves all 46 historical contract hashes. Its single menu
source-anchor failure also occurred in the baseline executable: the controller
cleanup added braces but retained the flat branch's return before the native
monitor. The test now checks that invariant directly; removing the return or
adding the monitor inside the flat branch fails its controls. The fresh full
temporal rig passes. The required absolute-path full build compiled production
DLLs, then exited 1 on the same five run_jobs Wine process-control assertions
(focus, inherited error mode, child creation/liveness and heartbeat ownership).
No full-build receipt or green Windows validation is claimed. Build log:
`/tmp/edvr-msvc-bootstrap-20261011/full_type11_audit_20261011.log`.

Sanctioned installer dry-run wrote nothing, then install and verify-only passed
with Elite closed. No INI replacement was requested; the flat INI retains SHA
`85aa64ab9f41dcdee88e8bf0e4b7ff2abece9016fe549c30e604cbaffd7bff02`. Installed
local test version is `v0.19.1-29-gdac1916e-dirty`; graphics DLL SHA
`aefb593e4a41cb1230519939d0cdb0f96dda8446681ce968133035972418a1cc` matches the
compiled artifact. Source remains on codex/xess-flat, unmerged; the original
Type 11 admission fix has not been guessed or declared complete.

Next: collect failed scene-admission evidence without audit altering rendering.
Verify the new dirty version before reading that flight; the old captures were
91576e7e. The original Type 11 cockpit failure remains unresolved until its own
guards or pixels explain it.

## October 12: original Type 11 refusal reproduced

Human report: AA was active until switching to this particular cockpit. Fresh
`edvr_gfx_20261012_050205.log` verifies `v0.19.1-29-gdac1916e-dirty`, build
6ACC18CC linked 2026-10-11 23:16:28 UTC. Before the switch, 2498 frames were
treated, with nine accepted resets and 2489 history continuations. Afterwards
the treated and backend-call counters stay frozen; this is admission failure,
not an SDK evaluation or UI-composite failure.

First conflict at 05:04:30.955, frame 8083: image-copy-source at q244 after the
source writer q240. Writer VS CFA91824129ECBBC / PS FCFAD73924BF45B9; copy PS
DFCBA0EC70B03C9B. Source format 9, destination HDR format 26; 1248x780 to
1920x1200, configured XeSS. HDR resource 7AAE39E0 and source depth 7AAE2720.
The summary's first-writer b1=0/camera=0 does not establish the later pool
camera; exact trace guards must decide that.

Shift+NumLock at 05:04:49.899 produced `flat_trace_9189.bin`: three frames,
8042 events, 4053328 bytes, no overflow. The same image-copy-source refusal
persists across capture and afterwards. At 05:04:51 treated remains 2498,
renderer calls 2500, accepted-continue 2489; no SDK output exists for these
rejected frames. Next: replay exact guards and source/HDR camera provenance,
compare passing traces, and change only the field or policy proved wrong.

Exact replay frames 9186–9188 matches stored contract hashes
`9955d597864425cc`, `4432b1c748ae3414`, `a14b51add16d9dbc`. Each frame passes
29/31 image-copy guards; only destination hdrCamera and its current tone camera
fail. H resource 7AAE39E0 has 11 prior camera-less draws, no bad or
layout-change flag. The copied source 96785490, format 9, passes its
camera-independent source proof. A unique qualified pool camera on depth
7AAE2720/DSV 7A7FB0B0 has 284 draws, q728–1126 before H first q1341 in frame
9186; b1 7AAF5710, camera hash C4BDFB00E579BF8D. No known laser witness
appears. H draws have known CB2 InverseClip/ForwardDp4 recipes, so their
projection provenance must be established before granting inheritance.

Ruled out: an SDK backend or late UI failure for this captured view, because
scene admission refuses before backend work and every SDK call counter freezes.
Ruled out: the audit-only reset as the cause of this original refusal, because
the same image-copy-source conflict starts before audit and persists after it.
No generic same-depth camera inheritance or shader whitelist expansion is
justified solely by these passing source guards.

Evidence boundary: FTR5 nulls all projection byte pointers and stores only
canonical VS b1 camera bytes. Failed frames contain 255/242/238 format-26 draws
with zero populated projection metadata slots or payloads; the earlier accepted
7793–7795 frames likewise have none. Existing local-capture pair 0/1 attempts
and completed captures are zero; those pairs target other shaders. Neither
trace proves CB2 matrix bytes, write freshness, bound subrange or its relation
to upstream jitter. A rendering admission change is not justified yet.

Next flight: extend the existing manual audit/capture to record the actual
scene-H writer's current projection bytes/epochs/ranges, verified shader
recipe, donor camera and resource/depth lineage together. Capture every
relevant writer in bounded frames, with explicit not-run, missing/stale,
ambiguous and complete outcomes. Keep admission/rendering behavior unchanged
and add no config key.

Instrument plan: two bounded forward frames, anchored at the actual H first
deferred inverse-ray writer, all subsequent H writes through copy handoff.
Record original tracked CPU-shadow projection bytes and binding subranges,
shader recipe/actual-stage identities, write generation/epoch/mutation, donor
camera provenance separately, and SRV resource/subresource lineage. Private or
untracked copyConstants fallback data must not be labeled game projection.
Earlier uncaptured writes, writer overflow or missing payload prevent complete
proof; unknown recipes remain explicit. Forward frame/q must be reported,
because the existing FTR dump contains frames preceding the hotkey.

Implementation is observational: no admission, camera, phase or binding
decisions. Sampling permits at most four attempts, at least 90 frames apart,
and stops after two complete payload frames. Original CPU-shadow filtering,
dispatch/depth-write mutation records, expiry/reset reports and immediate final
counts passed independent flow review. "Complete payload" is not camera proof.
The final production translation unit compiled with MSVC; the fresh full
temporal rig passes and all 17 corpus files/46 frames retain their hashes.
Native focused tests pass with warnings as errors. Root launched the required
absolute-path full build; it compiled production DLLs and exited 1 on the same
five run_jobs Wine process-control assertions. Full log:
`/tmp/edvr-msvc-bootstrap-20261011/full_type11_payload_20261012.log`.

Sanctioned dry-run wrote nothing; install and verify-only passed with Elite
closed. Flat settings retain SHA
`85aa64ab9f41dcdee88e8bf0e4b7ff2abece9016fe549c30e604cbaffd7bff02`. Installed
DLL SHA `22060934135177246d775ef8200b8c04bae513684d5ab440aba90aad59a1c5b3`
matches the build artifact. Version is still `v0.19.1-29-gdac1916e-dirty`, but
new PE build ID is **6ACCC3B6**, linked **2026-10-12 11:25:42 UTC**. Verify
that ID/time as well as the version before reading the next flight; the prior
dirty version is the same string but has ID 6ACC18CC. No rendering admission
repair is installed or claimed yet; this build collects the missing evidence.

### Captured payload evidence

User completed the capture. `edvr_gfx_20261012_053211.log` verifies dirty
version plus build **6ACCC3B6**, linked 2026-10-12 11:25:42 UTC. Four forward
attempts end at frame 9201; each observes 13 writers, six payloads labeled
complete, no unknown recipes, one donor and no prior-write, overflow, mutation
or uncertainty flag. AA treats 2407 frames before cockpit refusal, then backend
counters freeze again.

All 100 constant-buffer row captures are original-full-shadow/current-frame;
there is no missing projection, range, shadow or SRV-budget failure. The seven
incomplete writers are 2, 3, 4, 5, 8, 11 and 12: the texture-only SRV helper
cannot describe BUFFER/BUFFEREX views. Their element ranges/flags are absent,
but captured projection bytes remain usable. The 256-entry global write ledger
evicts 104145 identities; every captured CB still retained its current write.
This does not prove a freshness loss. No extra flight is justified merely to
make the payload-completeness counter green.

Frame 9201: H 7AF1E410, donor color 7AF1D8D0/depth 7AF1D150, donor b1 7AF35010.
Writers 1–10 VS b2[10..13] match donor spatial/depth coefficients exactly, near
term .025. Translation residuals are -3.38e-7, -2.73e-7, +8.69e-7. Their
inverse-ray rows satisfy A×rayUV near diag(2,-2,1) with UV-origin offsets
(-1,1,0), maximum residual 1.96e-7. First H PS t0 directly reads donor color;
lighting CS t4 reads donor depth and t9 reads donor color. The lighting CS
inverse UV projection also matches the donor reciprocal FOV within float32
roundoff. Sky orientation differs and its transform semantics remain unproven.
That is adjacent: the existing reducer already accepts one verified actual H
draw as the camera witness while retaining its later-writer conflict checks.
The repair therefore adds an equivalent first-deferred-H witness without
changing sky rules or classifying camera-less writers by FOV.

Next: implement the exact-pair witness with original current CB2 bytes, direct
PS t0 donor input, a unique earlier qualified donor and separate donor/H-write
provenance. Use float32 algebra/error bounds, not a tuned flight threshold.
Runtime freshness must come from tracked CB metadata, not the diagnostic
ledger. Nonzero-jitter disagreement must fail closed and log the current phase;
the next flight must show continuing history after AA starts.

Implementation review: ruled out roundoff agreement alone as an invertibility
certificate. A singular donor with huge cancelling inverse-ray values returned
`match` in the independent `/tmp/edvr-degenerate-review.cpp` control because
its error bound exceeded the identity entries. The new helper must reject
singular/ill-conditioned evidence with a numerical certificate before build.

That blocker is repaired: the normalized inverse residual and its roundoff
uncertainty certify `||(AB-T)T^-1||∞ < 1`. Exact/near-singular cancellation
controls now reject. Independent nonzero-jitter controls match only when both
forward and inverse projections agree; donor-only jitter and a stale inverse
reject with distinct reasons. Captured frame 9201, typed trace/provenance
negatives and later actual camera-conflict tests pass natively. Independent
source review is clear; MSVC production compilation and full temporal replay
validation are next, followed by the absolute full build and local flight.

### Deferred camera witness test build

The narrow repair verifies the exact first deferred-H shader pair, actual CB2
range/current original full-write bytes, direct PS t0 donor-color lineage and a
unique earlier qualified scene donor. Typed records keep donor b1 identity
separate from the actual H draw and CB2 write. Later real camera, layout, depth
and foreign-work vetoes remain. Sky rules are unchanged. FTR5 retains its
layout; a new verified-witness flag carries the validated slice digest and
write provenance without falsely setting HasCamera or inventing an H b1.

Both changed production TUs compiled with MSVC. The fresh full temporal rig
passes; the old corpus remains 17 files / 46 of 46 frames replay-identical.
Native focused tests and independent review pass. The absolute full build
(`full_type11_deferred_20261012.log`) compiles production DLLs, then exits 1 at
the same five Wine run_jobs process-control tests. No green full-build receipt
exists; Windows validation remains pending.

User closed Elite/MinEdLauncher. Sanctioned dry-run, install and verify pass
without `--ini`; live/mirrored flat INI SHA256 remains `85aa64ab...7bff02`.
Installed DLL SHA256 `3ab3b6c2...681fabed`, build **6ACCCDCC**, linked
2026-10-12 12:08:44 UTC. The dirty version string is reused, so the next log
must match this ID/time. Next flight is unarmed normal gameplay: AA and clear
UI after loading/switching into Type 11, continuing backend/history counters,
and a matched witness with nonzero phase.

### Nonzero-phase failure after the witness repair

Correct `edvr_gfx_20261012_061500.log` verifies build **6ACCCDCC** and link
time 12:08:44 UTC. User reports the whole exterior shimmers with AA On and
stabilizes Off; clarification: shimmer starts after cycling Off -> XeSS, not
immediately on cockpit load. Main H 7A7BA770 has actual shader/input/viewport
and current original CB2 writes throughout. Forward/translation agree; only the
inverse relation fails when phase is nonzero. No nonzero match appears. At one
snapshot, 2563 logged zero-phase matches and 2214 nonzero inverse mismatches
were observed.

Before first Off (06:17:17.613), acceptance stops at four (frame 6442), then
refusals reach 1040; stand-down follows. After cycling, treatment restarts with
zero-phase matches and nonzero inverse failures. Steady windows show about 100
failing frames per 300, equal repeated history resets and only two-frame
treated streaks. Backend failures remain zero. Ruled out: the narrow camera
witness alone completes the repair, because its live nonzero inverse proof
fails. Ruled out: the initially stable view proves active accumulation, because
admission was almost continuously refused before the toggle.

Open hypotheses and discriminators: unchanged/unjittered inverse rays would
leave the nonzero `A*B` residual proportional to the injected phase while
neighboring zero-phase rows match; another camera/epoch would give a different
row relation; a legitimate phase-dependent UV convention requires actual
shader/input evidence of that correction. The exact saved VS forwards fixed
input UVs and computes rays solely from b2[14..16].xyw dotted with (u,v,1),
without phase correction. The injector leaves view-side ray snapshots
unchanged, but this does not yet prove the H CB2 composer uses them. Do not
relax the inverse veto or compensate from the counters alone.

Producer boundary: the certified injector changes bounds +0x28C/+0x290 and
marks projection/VP bits 4|8. The observed FUN_1405964c0 ray CB composer uses
view/origin snapshots +0x870/+0x8B0 from FUN_1406be790; those are view-only and
must remain unchanged for pure projection jitter. Separate heap frustum blocks
behind +0x90/+0x98 use bit1 finalizer FUN_1404f3770, but no H CB2 lineage is
established. Existing C2/W2 tests prove projection algebra and view anchoring,
not this H producer. A bit1 repair is unproved. First classify the actual
nonzero rows, then identify their exact composer/cache before an upstream
dirty-flag edit.

This flight has no manual raw-row capture. Requested one while XeSS shimmers.
Existing four-attempt spacing is 90 frames and can alias the three-frame reset
cycle; check the actual captured phase before treating those rows as evidence.

### Type 9 capture and generalized scope

User confirms Type 9, Type 11 and Lynx Highliner have the same issue, and the
latest capture was in Type 9. The repair must target shared camera/projection
production and propagation across cockpits. T9/T11 have log evidence; Lynx
currently has the user's matching symptom report.

Correct `edvr_gfx_20261012_062410.log` has the same 6ACCCDCC build/time. Type 9
uses the exact deferred shader pair with current original CB2, same R1248x780
and one earlier scene donor. Frames 27601, 27691, 27781 and 27871 all captured
phase (0,0), and their donor/CB2 bytes are bit-identical. Forward/near terms
match exactly; maximum inverse residual is 6.8924e-8 and the normalized
nonsingularity certificate is 4.36e-6 < 1. Neighboring nonzero frames fail the
inverse check but have no captured payload. No nonzero match is established.

Ruled out: 90-frame sample spacing can measure this repeating failure, because
all four attempts sampled the same zero phase of its three-frame reset cycle.
Nonzero propagation remains unmeasured, so these rows cannot prove stale rays
or justify a rendering change. Diagnostic-only spacing will become 91, with
boundary/cycle coverage tests and unchanged four-attempt budget, so one capture
can cover the repeating phases. Rendering checks and sky policy stay unchanged.

### October 12: sampling build and producer boundary

Diagnostic build 6ACCD50D, linked 2026-10-12 12:39:41 UTC, changes payload
spacing from 90 to 91 frames. Full MSVC temporal tests pass, including cadence
boundaries and reset-cycle coverage; the existing 17 traces retain all 46
replay hashes. Production DLLs compile. The full build still fails the five
existing Wine process-control checks; Windows validation remains pending.
Sanctioned dry-run, install and verify pass. DLL SHA256 is
`1811e22d107a6886abb0488929144ee60c19441b8d1fb34ec487cc8cc5218c6d`; live and
mirrored flat settings retain SHA256
`85aa64ab9f41dcdee88e8bf0e4b7ff2abece9016fe549c30e604cbaffd7bff02`. The
requested next flight is T9, Off -> XeSS, then Shift+NumLock while shimmering.

Static current-engine evidence identifies a separate projection-dependent
cache: FUN_1404F3770 rebuilds from camera+0x1D0, calls inverse helper 5E9ED0,
writes heap frustum data referenced by camera+0x90/+0x98, and clears flag bit
1. Its connection to actual H CB2 rows 14..16 is not proved. Canonical angular
setters for camera+0x280 use mask 0xD at RVAs 58F374, 58F3B0, 592154 and
5921BA; compound-near camera+0x25C setters use 0xC. The invalidation contract
is field-dependent. No canonical setter for jittered bounds +0x28C/+0x290 has
been verified, so these findings do not yet justify adding bit 1 to injection.
The observed FUN_1405964C0 staging transform is view/origin-relative and is not
the H UV inverse projection. Next evidence must distinguish these producers.

Static lineage stops at the generic pool uploader 501A4A: descriptor WORD
[r14+2] selects pool/offset, [r14+4] supplies byte width, and staging is
[r10+8]+offset. No actual H descriptor or pool instance is joined. Frustum
consumer 5A8B70 copies camera heap arrays +0x90/+0x98 and camera state but has
no direct H upload or CB2 ray assembly. Ruled out: those callsites alone
establish the bit-1 cache as H's inverse producer, because no connected staging
writer has been found. Preserve the strict inverse veto.

### October 12: capture hotkey did not register

User reported the requested T9 capture complete. Sanctioned log inspection
verifies `edvr_gfx_20261012_065912.log` uses 6ACCD50D with the expected link
time, but finds no full-audit arm or HDR projection payload lines. NUMLOCK is
bound at startup; no unfocused-key warning appears. Rejection counters still
show current original CB2 and nonzero inverse mismatches, but do not establish
the numerical cause. Requested another press with Elite focused, left Shift
held first, NumLock held about one second, then an unchanged 20-second view. No
rebuild or rendering change is justified by this missing payload.

### October 12: nonzero T9 inverse propagation established

Focused retry registers in correct `edvr_gfx_20261012_070705.log`, build
6ACCD50D with the expected link time. Frame 9678 captures phase (-0.46875,
0.092592597) at R1248x780. Predicted NDC phase is (-0.000751201923,
-0.000237416915). Measured AB-T constant column is (-0.000751320997,
-0.000237305569, 5.64e-8), differing from prediction by 1.19e-7 and 1.11e-7,
below gamma(12) bounds 1.43e-6 and 2.06e-6. All slope entries pass. Zero frames
9587, 9769 and 9860 have maximum residual 2.06e-7; all four inverse rows 14..16
are bit-identical. Forward coefficients exactly match each same-frame qualified
donor; translation residual is <=5.42e-7.

Main H/donor share the actual depth/DSV and full viewport. Original unmapped VS
b2 is 9150C5E0, first 0, count 49, width 784; complete same-frame write
immediately precedes each H draw. Actual shaders and direct PS t0 donor input
match. This establishes missing current inverse phase, not a stale cache or its
engine producer. The diagnostic's overall incomplete label is the known
BUFFER-view limitation; these 112 projection bytes are present and qualified.

Separate CS 5998/EB02 b0 inverse rays already include phase; blanket patching
would double jitter them. Next repair design is a mathematically certified
shared projection propagation path for the missing inverse, preserving strict
AB=T on corrected constants and refusing every other relation. The observed
missing-phase dataflow is enough to design this consumer correction; it does
not authorize speculative camera-cache invalidation. No ship whitelist,
threshold adjustment or new config key is needed.

Cross-consumer capture check: writers 2..10 reuse the same current VS b2
projection but registered recipes consume forward rows 10..13, already phased.
Only writer 1 proves the inverse omission. CS 5998/EB02 use separate b0
7B48CC60; observed constant changes agree with inverse phase to 2.54e-8. Sky
F8FA uses separate b2 917D5F70 and a rotating basis. Dephasing its center
restores orthogonality within 3.64e-8, below gamma(12) uncertainty; it already
contains phase too. Available bytecode fixtures confirm the known layouts; not
every later H fixture is present, so do not claim exhaustive shader proof.

Repair implementation plan: preserve original current-write/shader/donor/input
guards; certify only coherent or precisely missing-phase inverse relations.
Warm a private inverse-only plan at zero phase, strictly verify uploaded clone
rows against the current donor, bind before admission, retain through the real
draw and restore the exact original binding. Natural whole-buffer binding is
explicitly separate from ranged mode and requires ordinary/Context1 identity
agreement, first 0 and count equal to ByteWidth/16. Preserve original CB2
provenance/hash and use a distinct executed-repair attestation. FTR5 has
neither projection bytes nor phase, so this is a live numerical attestation,
not an independently replayed matrix proof. Legacy audit preparation for the
same private buffer must not run while the certified deferred scope is bound.

### October 12: selective repair implementation and validation

The implementation prepares only the certified omitted VS b2 inverse rows.
Strictly coherent originals stay untouched. Original captures precede binding;
the installed clone is verified before model admission, survives the real draw,
then restores the original. Bit 15 records executed repaired admission while
original CB2 metadata/hash remains original. Later exact consumers whose
inverse becomes unproved invalidate the frame; new propagation failures are
outside the three legacy-audit bypass reasons. Active-buffer preparation guards
prevent a second upload while bound; invalidation revokes future tokens without
changing executing GPU bytes. Ranged alignment rules remain intact.

Native inverse/provenance/refusal tests pass; fresh MSVC temporal tests pass
with the existing 17 traces and all 46 hashes unchanged. First full MSVC build
compiles fresh production DLLs but hits the five existing Wine process-control
failures. The installer resource error appears in an expected negative tool
self-test, not a new build failure. No full-build receipt is produced.

Actual B390/DXVK/private Mesa projection scope tests have no failures. Runtime
tests exposed two fixture assumptions: ordinary GetCB1 returns count 320 for
the 5120-byte foreground fixture here, rather than assumed 4096; the new active
buffer guard refuses before the old topology branch. Test-only corrections
query/validate actual ranges and require the precise new refusal. Natural
49-constant tests now print qualified/prepared/bound and execute installation
and restoration, so the hardware run cannot silently pass by skipping that
path. The repeated full build compiles fresh production DLLs with no C++ errors
and stops at the same five run_jobs Wine failures. The repeated B390 hardware
rig passes scope/runtime tests with zero failures. Both natural-49 paths report
first=0/count=49/qualified=1/bound=1; runtime reports prepared=1.

Final local install: sanctioned flat dry run, install and verify pass for build
6ACCE2F3, linked 2026-10-12 13:38:59 UTC. DLL is 18,597,376 bytes, SHA-256
`8ff4a79c6e6b9313f5187b9a1e7a6e21034ea77dfea7153ded523a36be9c0310`. Live and
mirrored flat INIs retain their pre-install SHA-256
`85aa64ab9f41dcdee88e8bf0e4b7ff2abece9016fe549c30e604cbaffd7bff02`. No
full-build receipt or clean-commit promotion is claimed. Next flight: same Type
9, Off -> XeSS, view motion for about a minute; if convenient switch to Type
11/Lynx in the same session. Verify actual repaired binding and continued
temporal accumulation from this PE build, then compare the human image report.
Windows full validation remains required before main integration.

### October 12: selective repair local regression

Human reports a previously working cockpit now cycles clear -> shimmer with
frequent history resets. Correct `edvr_gfx_20261012_075614.log` is build
6ACCE2F3 with the expected link time. The first H 7AE771A0/source 7AE76660
certifies and binds the inverse-only clone (bound=1, typed-first=1). A second H
90EBAF80/source 90EBA440 uses the same CB2 7DF53700 but frequently has no
qualified donor or inverse-mismatch without certified missing phase. The new
global failure path fires deferred-inverse-unproven about 29-36 frames per 5 s;
compute source invalidation and relevant-observations-truncated follow,
rejecting those frames. Renderer backend-failure/camera-cut/frame-gap stay zero
and accepted resets track these rejections. Next evidence: identify this second
H's depth, extent and consumer lineage before changing the correction/admission
scope.

Ruled out: successful clone binding and CPU/GPU rig passes establish a complete
visual fix, because the verified local flight regresses another cockpit. Ruled
out: SDK execution failure explains these cycles, because the current backend
failure count is zero and global phase rejection matches the resets.

Capture registered at 08:02:54.798, full audit and forward payload arm, then
`flat_trace_23409.bin`: three frames, 7,278 events, 3,668,272 bytes, no
overflow. First forward payload anchors primary H 7AE771A0/depth 7AE75EE0 at
R1248x780. The second target's resource/depth/consumer lineage is being checked
in the trace; no production change is justified merely by its secondary
appearance.

Exact replay: frames 23406-23408 contain only primary H, one candidate and no
ambiguity or uncertainty mark. Its first writer is typed verified/repaired; the
final F9CF/FEE tone reads it at PS1. No secondary H/source appears. This proves
the good interval's primary lineage, not ownership of the failing pass. Ruled
out: this random three-frame capture classifies the intermittent secondary
target, because the target is absent from every captured frame.

Discriminating next evidence, instrumented together: compare the rejected
target/depth/extent with retained phase HDR/depth/extent and named depth;
retain the actual failed frame through final consumer. Different domain plus
absent main-output lineage supports a local refusal. Main identity despite
missing donor requires retaining global protection. A secondary input feeding
the main output needs its own verified projection/domain relation, not an
exemption. The diagnostic will record ownership at the refusal and request one
bounded completed-frame trace there, so the next flight does not depend on
pressing a key during the brief reset. Rendering/admission stays unchanged for
this probe.

Diagnostic implementation: ownership fields log at the propagation refusal.
Snapshot completion follows seal and precedes BeginFrame. The serializer's
explicit opt-in accepts only sealed, non-overflowed frames, and any append
revokes a seal. Success requires the requested failure frame in the emitted
file. At most two actual-failure attempts and one emitted snapshot occur per
process/manual arm, with a 900-frame retry window when the first frame was
already truncated. AA-off, reset, resize or device/output loss report
expiration; never-ran and file-write failure are distinct. No good following
frame can stand in for the failed one. Fresh native policy/serializer/order
tests pass. Independent review and fresh MSVC temporal tests pass, including
the registered snapshot tests and all 17 traces/46 hashes. The final full build
compiles production DLLs without MSVC errors, then fails the same five Wine
run_jobs tests. First attempt was stopped before compilation by this doc's
61-line Status block; it was shortened and the final build passed that gate.

Diagnostic local install: sanctioned dry run, install and verify pass for
6ACCECDD, linked 2026-10-12 14:21:17 UTC, 18,603,520 bytes, SHA-256
`2b772ed83f644ff0127b1665625a60cbc3d82d3ca3d10ebc78c07acfb837f429`. Both flat
INIs retain the previous SHA-256 85aa64ab...ff02. Source remains on
codex/xess-flat; no full-build receipt or main integration is claimed. Next
flight: the regressed cockpit, XeSS selected, view visible about one minute, no
hotkey; confirm PE identity, rejection ownership and automatic snapshot's
actual failure-frame inclusion before proposing a scope fix.

### October 12: Windows main counterexample clarified

User corrects the preceding statement: Windows main does engage AA normally in
these cockpits. TAA, FSR and DLSS work at supersampling 1.0 and 0.5. No Windows
log/build identity has been inspected. This is a functional main test, not
validation of the current branch build. The original device-side admission
failure remains proved by its captures; its cross-platform cause is unresolved.
Branch, GPU, platform and graphics settings are not a controlled comparison.

Ruled out: ship identity alone implies main cannot recognize these cockpits,
because the user reports successful AA on Windows main. Do not infer Mesa or
Proton caused it solely from this comparison. The new branch-wide failure scope
still directly explains the current reset cycle; the automatic ownership and
failure-frame probe remains the next local test. The Windows result spans both
native and reduced rendering, so a single SS route or one AA backend does not
explain its success. Branch versus main provenance/ownership changes are being
audited alongside platform/GPU rendering-path differences. A main flat build on
this device or this branch on Windows would separate those axes; neither is yet
an executed controlled comparison.

Read-only audit main 99476a -> committed branch dac1916e: camera discovery,
injector/table, projection recipes/runtime/scope, HDR selector and final-copy
selector are unchanged. The laser witness addition broadens qualified admission
and removes no existing path. XeSS phase-count changes are gated to XeSS;
TAA/FSR retain the earlier cycle. Thus the original camera-less H draw inputs
would fail the same main guards. Later dirty DeferredB2 acceptance/repair is a
separate stage, and its secondary-H global veto is the confirmed new
regression.

Discriminators: compare actual native/DXVK H and camera-bearing witness draws,
VS b1 rows/write stamps, depth/DSV and final-copy lineage. Separately compare
nonzero same-frame donor/CB2 inverse relation and actual history continuation.
Post-admission UI/backend differences cannot explain the captured earlier
pre-backend admission refusal. Strongest baseline is verified main flat on this
same device/cockpit/settings with TAA/FSR, holding platform fixed; a verified
pre-dirty branch capture on Windows is complementary. No baseline build or
install has been made yet; the installed diagnostic remains the next local
flight.

### October 12: exact secondary-domain failure captured

User says the installed diagnostic ran about a minute. Correct
`edvr_gfx_20261012_162004.log` verifies 6ACCECDD/link 14:21:17 UTC. Ownership
record at frame6060/q1230: H A1E842E0, depth6F6D8780, DSV7DFECCD0, 256x256;
phase H6F988DD0, depth6F987B10, named depth6F987B10, R1248x780. Actual
bindings, input, range, original shadow and current write all pass; donor is
unavailable. Original CB2 9E379710 is first0/count49 with current write q1229.
Automatic snapshot completes after seal: failure frame present, 4 frames/8,503
events, no overflow, emitted `flat_trace_6060.bin`. This is the requested
failed frame, not a random good interval.

Replay frame6060: primary H q1218 is typed/repaired (C000), 1248x780, depth
6F987B10/DSV6F7445F0, direct pool6F988290/current camera. Global uncertainty is
marked just before the secondary H q1230. Secondary has its own 256x256 source
A1E837A0, draws on its distinct depth and later sky draw q1469; no qualified
pool is registered there. Final tone q1474 reads primary H at PS1; final copy
q1476 reads primary LDR6F986FD0. HDR replay has one candidate; frame6057-6059
ordinary HdrExtent(22) becomes PrefixUncertain(3) in6060 after the marker. No
recorded PS0/1 or sampled tone t0..3 reads secondary H; higher unsampled SRV
slots are not proved. Separate depth/extent establishes it is outside the owned
scene phase regardless of its exact visual purpose.

Ruled out: primary H inverse/binding refusal triggered failure6060, because its
first writer carries verified executed-repair attestation and the uncertainty
marker occurs only at the separate-domain writer q1230.

Correction authorized: separate phase ownership from matrix qualification.
Known phaseHdr identity stays protected even with missing/ambiguous donor or
corrupt depth/extent. With HDR-null ownership, scene depth plus phase extent
defines a candidate. A current typed witness may retain protection only when
anchored to that phase domain; generic DeferredHdrVerified cannot promote an
unrelated coherent secondary target. Outside scope, nonzero verification must
neither prepare/bind an inverse clone nor fail phase/mark prefix uncertain.
Zero-phase bootstrap retains strict original matrix/donor proof so scene/size
changes can discover a new main target. No ship or 256x256 whitelist.
Independent review is clear. Tests pin missing/stale donor, corrupt selected
depth/extent, later owned mismatch and coherent-secondary -> unproved-secondary
sequences. The early outside-domain gate is Upstream-only; Legacy behavior is
preserved. A bounded source-order assertion pins it before verifier work,
preflight/repair, global failure and admission. Native tests pass with zero
failures and all 46 hashes across 17 historical traces remain unchanged. Source
is frozen for the final full build and fresh MSVC temporal checks.

Windows comparison clarification: successful main test was another PC with an
Nvidia GPU. TAA/FSR/DLSS at SS1.0/0.5 work there, but this does not isolate
Intel versus Nvidia, native versus DXVK, or branch versus main. User can exit
Elite now; captured evidence is complete and saved before implementation.

### October 12: ownership correction built and installed

Final reviewed source compiled and linked in the absolute-path full build;
`full_deferred_inverse_ownership_20261012_final.log` exits 1 on the same five
Wine `run_jobs` process-control failures. No full-build receipt, commit or
promotion. Fresh MSVC temporal test
`/tmp/edvr-deferred-inverse-msvc-ownership.log` passes deferred camera and
failure snapshot tests with zero failures, 17 traces/46 unchanged frames and
collector policy PASS. Binding code is unchanged since the B390 GPU rig.

Installed with the sanctioned flat installer after dry-run, then verify-only.
Both live and mirrored flat INIs retain SHA256
`85aa64ab9f41dcdee88e8bf0e4b7ff2abece9016fe549c30e604cbaffd7bff02`. Installed
DLL is 18,603,520 bytes, build `v0.19.1-29-gdac1916e-dirty`, PE/link 2026-10-12
22:32:53 UTC, SHA256
`aef81a330b4c7577626175ba4ece95bd0eb7b3ac31a0f25197ffd7361e2e4bdf`. Next human
check: previously working cockpit, then Type 9 Off -> XeSS and view movement;
if practical Type 11 or Lynx Highliner in the same session. Check both stable
scene AA and clear HUD/menus, then read the correct build's history/refusal
counters. No manual capture is needed.

### October 12: automatic activation blocked in cold probes

User reports better results after the ownership correction, but first loading
and switching T9 <-> Lynx leave AA visually inactive. Cycling to FSR or Off and
then XeSS restores it. No explicit "Forced Off" menu label was seen.
`edvr_gfx_20261012_165802.log` verifies build 6ACD6015/link 22:32:53 UTC.
Stand-down enters at 16:58:44 for NoScene and remains for 80.3 seconds/52
probes until mode change wakes Full at 17:00:04.450. A later 7.5-second
stand-down also ends on mode change. Initial no-output stand-down did
automatically recover when the menu became selectable; do not call that
interval backend warmup.

Probe frame6379/q767: primary H7A9A0270, source7A99F730, actual/input=1,
canonical donor B1=7A9B34F0/lastq763, but CB2=null, first/count=0,
original/current=0, relation unavailable, accepted/warmed=0, phase0. After
manual Full wake, frame6947/q810 has the same H/source/donor, CB2=7E43C9E0,
first0/count49, original/current=1, current writeq809/donorq806, relation
match, accepted/warmed=1. Nonzero strict inverse preparation/binding resumes
next. This is unavailable paused tracking, not proof that the game's actual CB2
is unbound. No propagation failure or automatic failure trace; late H writes=0.

Source confirms Full -> Probe/Paused releases projection runtime/context and
original shadows, and runtime recreation is Full-only. Probe still calls the
typed H verifier but cannot supply its required current original CB2 bytes;
only a selector-certified Treatable probe can resume Full. NoScene itself is
camera-independent, so its entry reason is not a matrix-failure verdict. Ruled
out: an explicit AA-mode Forced Off latch, because requested mode stays XeSS
and the user reports visual inactivity only. Ruled out: old phase-H identity
alone blocks zero-phase reacquisition, because the ownership gate is inactive
at phase0 and compatibility drops to zero on an untreated frame.

Hypothesis and discriminator: strict cold certification in the existing Probe
frame should recover the same H/donor without a mode cycle; current original
CB2 + matrix proof + selected final consumer must all pass before Full resumes.
Proposal under independent review: give Probe bounded observational shadow
tracking/context only, prohibit preflight/clone/bind/backend work there, and
release cold tracking again before Paused. Unsupported/stale/partial/ambiguous
or consumer-less scenes must remain stood down. No new setting or ship rule.

Cold Probe implementation is now independently reviewed and frozen. Each
existing probe creates fresh bounded original-shadow tracking and Context1,
allows only strict current original certification, and lets the real selected
handoff drive the existing wake. Evidence is discarded when leaving Probe,
including before fresh Full. Explicit Full gates protect deferred warming,
compute preparation, backend preflight/readback and raster jitter. The paused
work description now names the probe's observational exception.

Native deferred/stand-down tests pass with zero failures: real model-driven
loading -> stand-down -> certified Probe -> selected tone/copy -> Full wake,
plus ten negative current-data/consumer cases that stay paused. Historical
corpus remains 17 files/46 unchanged hashes. Final full build and fresh MSVC
tests are running; no GPU binding implementation change in this step.

Final verification caught a formatter fixture drift: exactly three stand-down
descriptions still said all shadow tracking was paused. Generated a temporary
fixture using the existing rig, independently reviewed its diff, then
regenerated the tracked fixture. All other 28 lines stayed identical. Fresh
complete MSVC temporal self-test now passes, including camera/snapshot tests,
17 traces/46 unchanged frames and collector policy. Passing log:
`/tmp/edvr-deferred-inverse-msvc-coldprobe-fixture.log`.

The final absolute-path full build compiled/linked graphics and OpenXR, then
stalled in its Wine test-rig gate with no completion/failure output and no
receipt. This final attempt is incomplete; the prior cold-Probe full attempt
did exit 1 on the same five Wine process-control failures. Host inspection
identified `python tools\\run_jobs.py --self-test`, sleeping more than six
minutes with no descendants or log progress. Terminated only that owned test
process; build then exited 1. No usual five-failure result was emitted in this
final attempt. Default sandbox process inspection did not see host processes.
No commit/promotion/full-green claim, and Windows validation remains pending.

Installed final stable graphics DLL via sanctioned dry-run/install/verify:
`v0.19.1-29-gdac1916e-dirty`, PE/link 2026-10-12 23:17:22 UTC, 18,603,520
bytes, SHA256
`7d1b0d6565fa6c19854782d7116fd29bf7d541d6c3321c16517223f3f53ca459`. Both flat
INIs remain the same 85aa64ab...7bff02 hash recorded above. Host game/launcher
check and install guard confirmed closed before installation. Next flight
requested: fresh XeSS load into T9, switch to Lynx and back without touching
the AA mode, view/motion for about 20 seconds each, stable scene and HUD/menus;
leave open for exact-build history/recovery log check, no capture.

### October 12: automatic recovery verified in T9 and Lynx

User confirms fresh load and T9 <-> Lynx changes now engage AA automatically
and stay stable with clear HUD/menus. Lynx -> T9 appeared inactive briefly,
then engaged after about one or two seconds without a mode change.
`edvr_gfx_20261012_174055.log` verifies 6ACD6A82/link 23:17:22 UTC. Only the
startup `mode=xess` line occurs; no later menu AA-mode transition.

Cold Probe frame6463/q2345: primary H7A975D70/source7A975230, actual/input=1,
CB2=7D6FD810, first0/count49, original/current=1, current writeq2344, donor
B1=7A98BA90/lastq2030, relation match, accepted1, prepared0/warmed0, phase0,
work=probe. Same frame's selected chain resumes Full automatically. Frame6464
is Full with the same matched original/current data and warmed1; frame6468
applies the certified missing inverse phase with prepared1. This confirms
observation-only cold proof followed by fresh normal preparation.

Final sampled runtime totals: 12,198 treated, seven accepted resets and 12,191
continuations, five lost-history events across load/scene transitions, zero
backend failures/camera cuts, treated streak4,812. Refusal total stays685 while
treatment continues; no deferred propagation failure or automatic failure
snapshot. Separate native HDR route still refuses R1248x780 below E1920x1200;
the admitted main copy route executes XeSS upscaling. This is not AA
inactivity.

Stand-down totals22.9/73.5/6.2 seconds include absence/loading and lack ship
identifiers. They all end on selector recognition, so do not claim that the log
proves exact one-to-two-second readiness latency. Human observation is
consistent with the existing 1.5-second probe cadence once the new scene is
available. Type11 was not part of this final human check; generalized checks
use camera/resource ownership and matrix proof, not ship identity.

User can close Elite; evidence is saved. Final read-only branch audit requested
before pushing `codex/xess-flat` for the user's Windows check. User's earlier
instruction keeps this work on a branch until that test; no main merge or
full-green build claim. Windows/Nvidia-main comparison still changes GPU, OS
and branch, and does not isolate why its scene witness differs.

Final read-only audit is clear. All five new headers, reviewed runtime/tests,
formatter fixture and this arc are saved on `codex/xess-flat` for the user's
explicit branch pull/Windows test. Main is not merged. The locally verified
installed DLL keeps its recorded dirty version/PE/hash; no receipt-guarded
promotion or unnecessary rebuild was performed. Native Windows full build,
installer gates and final Type11 check remain pending.

## October 13: Windows review and Proton bridge verification

PR 88 review 5497823856 reports full native Windows validation at 829dcf00. The
two upstream commits fix native D3D11 keyed-mutex enforcement, avoid
loader-lock teardown of the XeSS bridge, and re-pin the unchanged-behavior
flatHelpers region. Local HEAD fast-forwarded; main was not merged.

A frozen snapshot of 829dcf00 was built with portable MSVC and run in the
existing disposable Proton prefix with the private Intel ICD. All four LDR/HDR
native/upscale/recreation flights passed: 20 evaluations, four resets, zero
failures. Compile and GPU logs are /tmp/edvr-pr88-gpu-829dcf00/compile.log and
gpu-test.log. Those files are local historical artifacts. The Windows review
itself persists at PR 88, review 5497823856.

The live Elite graphics XML currently records BloomQuality 0, DOFEnabled 0,
AOQuality 0, BlurEnabled false, SS 0.65 and Custom preset, with the custom file
last written on October 11 at 22:32:20 UTC. That differs from the review's
Bloom 3 / DoF 2 comparison. A current file is useful evidence for the next
flight, but cannot replace missing per-flight settings snapshots. Therefore
post-chain settings remain a hypothesis rather than the established sole cause
of the cross-system difference.

The controller feature was an explicit user request in this task, so it stays
in this branch. Diagnostic removal is deferred until Type 11's final retest.
Raising camera dirty bit 1 is also deferred: it changes shared ray/frustum and
culling data, and no flight proves that it replaces the draw-local repair. The
shader recipe identifies known semantics; physical topology alone cannot admit
every unknown shader. The qualified laser witness covers a later camera-bearing
source and remains distinct from the first-H deferred witness. GPU-copy
optimization and pipeline prebuild require a measured profile first.

### Review follow-up validation and dispositions

The corrected source compiled and linked both production graphics and OpenXR
DLLs via the normal absolute-path build. The full gate returned 1 on the same
five run_jobs process-control failures under Wine; no receipt was produced. The
earlier green Windows receipt applies to 829dcf00, not this follow-up. Full
build log: /tmp/edvr-msvc-bootstrap-20261011/full_pr88_review_fixes.log.

Nine focused repository rigs passed: flat temporal, flat camera VR, hotkey
capture (636 checks), pad navigation (51 checks), temporal math, input gate,
menu held edits, virtual-screen fitting and XeSS engine. The latter includes
the optional hardware gate and another 20 GPU evaluations/four resets with zero
failures. The strict hardware rig also passed after backend edits.

The UI rig now has exactly the same 206 failing checks as the prior Wine
baseline (3,810 checks total); its new CPU and wiring checks pass. Installer
has the same eight sharing/replace failures as baseline; new XeSS planner,
adoption, uninstall and ownership round-trip checks pass. Linux developer
installer self-test still encounters the pre-existing POSIX read-only-file
prune mismatch; its isolated XeSS opt-in/dry-run/verify checks pass.

The full mono rig is blocked by Wine's predication/null-read failure. The
unchanged baseline executable fails at the equivalent point in the same prefix.
A temporary MSVC-built copy isolated the new positive/negative XeSS preflight,
nonzero-slot refusal and resolve cycle guard, with projection scope/runtime
checks, and passed without reaching the unrelated failing GPU suite. Files:
/tmp/edvr-pr88-mono-probe/compile.log and run.log. No production code was
changed for this isolation. These checks are supplementary evidence, not a
replacement for native Windows's full suite.

Disposition of PR 88 review 5497823856, by inline thread:

- 4253867146: keyed mutex verified on exact reviewed source and corrected
  source, preserving NT-handle flags and shared-fence ordering.
- 4253867155: feature creation failure latches input/output/HDR key and reason;
  same key does not recreate/log; size, mode and device changes permit retry.
- 4253867156: XeSS uses external-upscaler weapon copy ownership below native;
  capture manifests and Python reader name it xess. On-foot flight is pending.
- 4253867161: profile-aware predicate guards the six cited VR readers; existing
  temporal modes remain enabled in both profiles and XeSS remains flat only.
- 4253867164: shadow payload bytes no longer zero-fill at probe allocation;
  full-write validity still precedes reads. Legacy plan is Full-only; comments
  name CPU probe tracking. Cached old admission cannot certify current writes,
  and a scene-start hint alone cannot prove a current camera, so those narrower
  wake alternatives do not replace the bounded fresh probe evidence.
- 4253867170 and 4253867174: retained verified admission semantics, named
  deferred pair/layout constants, added bounded once-per-pair unknown UI tone
  logging with no admission. The later laser witness remains a distinct case.
- 4253867183 and 4253867197: bit-1/culling A/B and GPU-copy/pipeline
  optimization await measured flights; no unproved propagation or performance
  change.
- 4253867185: deferred admission logs use the existing five-second throttle.
- 4253867191 and 4253867192: render-thread device change resets the binding;
  mode change releases features/latch. Module-relative XeSS and System32 D3D12
  loads require SR 2.0.x; error 126 distinguishes present DLL/dependency
  failure.
- 4253867204: optional unreadable XeSS files stay untouched, adoption persists
  ownership with a state-only write, and dev installation requires --xess (or
  --all). Focused planner/round-trip/dry-run tests cover the requested cells.
  No adjacent NGX behavior change was kept.
- 4253867208 and 4253867231: controller lease survives ordinary long frames but
  revokes on focus loss and expires after five seconds. Navigation reserves pad
  buttons while the panel is open; keyboard capture requires its own gate.
- 4253867212: different-original/non-idle topology refusal has a live-plan
  case.
- 4253867216: full build now runs an optional hardware XeSS gate, with explicit
  capability-only skips; loader, bridge, execution and readback errors fail.
  Motion/jitter image-registration expansion remains a separate GPU fixture.
- 4253867219 and 4253867224: Status includes open hypotheses, next flight,
  current graphics settings and the build distinction; local rear-button setup
  moved to its own small doc, and AGENTS lists the XeSS receipt dependency.
- 4253867228: preflight rejects an insufficient XeSS jitter cycle before Ready
  or raster jitter; cycle changes recheck the plan even at unchanged
  dimensions. Positive and short-cycle preflight, slot refusal and resolve
  guard checks pass.

All review changes remain on the feature branch for Windows re-review and full
validation. Controller support is retained because the user explicitly
requested it; bounded diagnostics remain until Type 11's final flight.

## October 13: Windows review round 2

Review 5502633003 identified a stale terrain mutation anchor in 432afc5a.
Windows commit 770069b6 changes only that test anchor; the reviewer reports all
140 rigs and the hardware XeSS gate passing on Windows 11/MSVC/RTX 5090. That
green receipt covers 770069b6, not the new follow-up source.

Confirmed static causes: flatRuntimeResize can clear its device before the old
rebind guard compares it; the latched reason pointed into State that
xessShutdown deletes; rejected unknown-tone shapes were not remembered; raw pad
hotkeys bypassed panel navigation ownership. The corrections follow those
specific branches and fields, without changing camera admission.

Disposition of the six new inline threads:

- 4257900173: xessAvailable compares the actual backend device and tears down
  before rebinding, independent of the flat runtime's resize state.
- 4257900186: latched reasons copy to stable g_reason storage. Key acquisition
  failures latch by feature key; repeated execution errors log first and every
  300th failure. The hardware rig reproduces an invalid feature key, repeats
  its failure, replaces the device and reads the retained reason before a
  successful replacement-device flight.
- 4257900190: a separate 16-pair negative set stops repeated descriptor
  inspection; exhaustion stops further unknown inspections with one notice.
  Both caches are diagnostic only and cannot grant source-tone proof.
- 4257900194: the overlapping summon guard requires proven pad ownership.
  Saving/write failure status takes priority over the keyboard-gate warning.
- 4257900199: plain flat verification checks runtime and notices when the
  effective flat ini selects XeSS, failing with explicit --xess guidance if
  missing or stale. Other modes preserve optional runtime behavior. The
  sanctioned example and installer/build-receipt introductions are corrected.
- 4257900210: raw navigation-button hotkeys yield only while the focused panel
  owns its pad; its summon binding remains available. Closing primes held
  edges. The input-gate rig drives the actual hook through request, foreground
  loss and release; the capture rig exercises flatPadNavigation and pins the
  live summon/capture/cancel/release call chain. Full menu.cpp side effects are
  not simulated in an executable rig; that is the remaining coverage limit.

The normal absolute-path build compiled production DLLs with no C++ or link
errors, then failed the same five Wine run_jobs gates; no receipt was written.
Log: /tmp/edvr-msvc-bootstrap-20261011/full_pr88_round2.log. Focused MSVC rigs
pass for input gate, hotkey capture (645 checks), pad navigation (51 checks),
held edits, terrain checkerboard and XeSS. The UI rig's final isolated run has
3,815 checks and the same 206 baseline failures, with no new failure names; an
initial concurrent run had a transient module-enumeration failure that did not
repeat. New positive/negative cache checks pass.

Strict and in-build XeSS runs both complete 20 original GPU evaluations plus
five on the replacement device, with no execution failures. The intentional
invalid-key failure is separately asserted. XeSS opt-in/selected-runtime
installer cases and build-receipt self-tests pass. Fresh local gameplay and the
updated native Windows full build remain pending. The low-priority Legacy
XeSS-upscale repeated refusal is still deferred; the corrected preflight guard
is covered separately, and no new standdown behavior is inferred without
evidence.
