# XeSS for flat EDVR on Intel B390

## Status

- State: local flat XeSS gameplay confirmed at native AA and 0.75 upscaling.
  HUD/menu motion now stable at UI 125, SS 0.5 and about 0.65. Native hangar
  flicker was absent at 0.75. Keep this branch for Windows testing; no main merge.
- Environment: ONEXPLAYER 3, Intel Arc B390 PTL 8086:b080, CachyOS xe, Proton
  Experimental experimental-11.0-20261001-x86_64, DXVK
  v3.1.1-47-g685301564ea3486, private Mesa 26.3.0-devel git-37bf953a58. Normal
  Elite startup now reaches the menu without EDVR or a debugger; see
  planetary-startup-intel-b390-2026-10-11.md.
- Branch adds fix.temporal_aa=xess for flat only, a pinned SDK fetcher, a
  D3D11/D3D12 shared-texture/fence backend and installer payloads. Existing
  modes and defaults stay unchanged. Initial live flat config selects xess.
- SDK v3.0.2, commit 8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0; XeSS-SR runtime
  2.0.2. Package and component versions differ.
- Windows SDK handoff: run `python tools/fetch_xess.py --dest third_party/xess`
  before the full build; the fetcher pins and verifies the SDK and the build
  stages `libxess.dll` with Intel's license notices for the flat installer.
- Ruled-out pointer: capability entries below record D3D11 unsupported-driver
  refusal and the missing enabled Vulkan extension on DXVK's device. Separate
  Vulkan execution passed; the confirmed D3D12 bridge is the branch backend.
- Confirmed: B390 bridge GPU rig passed 20 evaluations across native LDR, 2x
  LDR/HDR and HDR recreation, four resets and zero failures. XeFX version is
  0.0.0.0; XMX acceleration remains unverified. Local gameplay now executes.
- XeSS jitter minimum follows ceil(8 * max(E/R)^2), including 72 phases at 3x.
  Legacy/Off camera routes stay at eight and explicitly refuse XeSS upscaling
  when insufficient; native AA can use eight.
- Build gate: portable Microsoft MSVC/SDK compiles production DLLs and both
  installers. Full build is not green: unchanged run_jobs process/error-mode
  tests and installer sharing tests fail under Wine. Windows validation is
  still required. The user authorized branch-only work pending that check.
- Local install: clean source commit 4b7bfec4, version v0.19.1-18-g4b7bfec4.
  Sanctioned installer dry-run, install and verify-only passed, including the
  XeSS DLL/notices; settings unchanged. HUD/menu fix verified; main 99476a02.
- Gameplay flight: correct 0f700706, 103114.log. Native HDR AA at 1920x1200
  completes every step. SS 0.75 upscales 1440x900 to 1920x1200 on the LDR copy
  route with 15 jitter phases; XeSS init/evaluations return rc=0.
- User: mostly stable. Station surface below the hangar's Welcome to sign
  flickers at native and stops below 1; sizes imply 0.75. This changes resolve
  route and jitter as well as resolution, so it does not isolate the cause.
- Confirmed: correct 4b7bfec4, 120427.log; E894 UI proof bootstraps and every
  steady frame composites HUD/menus. User reports stable. Cleanup 02f50bd7 and
  exact-pair guard evidence are in design-flat-ui-quality-2026-10-05.md.
  Native surface flicker still needs evidence; FSR3 SDK is absent here.
- Capture: correct build 644fe39b, edvr_gfx_20261011_100319.log and
  flat_trace_14850.bin; three complete frames, zero overflow. Stored/replayed
  contracts match exactly. All reject image-copy-source: 29/31 guards pass;
  destination hdrCamera and current tone camera are missing.
- Confirmed: H has camera-less writes then a laser-pair draw excluded from
  setting tone. Its camera matches 266 prior supported pool draws on the same
  scene depth, including exact camera bytes and current write provenance.
- Ruled out: defective copy bindings/source layout and capture-induced healing;
  all three frames retain the same refusal, with no gameplay backend calls.
- Implemented: actual H laser draw qualified by scene camera and verified image
  continuation. MSVC rig passes; 17 historical traces retain all 46 hashes;
  current capture selects all three HDR frames. Production DLLs/flat installer
  compile; full build stops at baseline Wine run_jobs failures. Fix flown.
- Temporary settings: initial flat profile fix.temporal_aa=xess; no new
  diagnostic config keys. Existing private Mesa launch options verified.

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
