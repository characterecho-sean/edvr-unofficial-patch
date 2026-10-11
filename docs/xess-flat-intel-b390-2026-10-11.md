# XeSS for flat EDVR on Intel B390

## Status

- State: first local flight confirms menu XeSS, but gameplay route refuses. The
  user wants to test locally on this device first, then test on Windows. Keep
  the work on this branch; do not merge to main.
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
- Confirmed: D3D11/D3D12 bridge and production backend execute on the B390. GPU
  rig passed 20 evaluations across native LDR, 2x LDR, 2x HDR and HDR
  recreation, with four resets and zero failures. XeFX version is 0.0.0.0; this
  establishes XeSS execution, not XMX acceleration or image quality.
- XeSS jitter minimum follows ceil(8 * max(E/R)^2), including 72 phases at 3x.
  Legacy/Off camera routes stay at eight and explicitly refuse XeSS upscaling
  when insufficient; native AA can use eight.
- Build gate: portable Microsoft MSVC/SDK compiles production DLLs and both
  installers. Full build is not green: unchanged run_jobs process/error-mode
  tests and installer sharing tests fail under Wine. Windows validation is
  still required. The user authorized branch-only work pending that check.
- Local install: clean source commit 644fe39b, version v0.19.1-11-g644fe39b.
  Sanctioned installer dry-run, install and verify-only passed, including the
  XeSS DLL/notices and reviewed flat INI. Main remains at 99476a02.
- First flight: correct build 644fe39b, edvr_gfx_20261011_093635.log. Native
  HDR R=E=D=1920x1200; XeSS init/execute rc=0, 3115 evaluations. Gameplay
  admission fails with conflicting-hdr-target-or-camera then
  no-known-tone-pass; treated freezes at 3186 and the runtime stands down. User
  reports a great menu and mostly untreated gameplay elements. FSR3 dependency
  is absent in this build.
- Ruled out: XeSS runtime/bridge execution failure as the reason gameplay is
  untreated, because menu evaluations succeed and gameplay backend=0 before any
  SDK call. Actual gameplay temporal quality remains untested.
- Open: first detailed conflict is image-copy-source at frame 6642, q14/q16:
  source VS CFA91824129ECBBC / PS FCFAD73924BF45B9, format 9 with depth but no
  captured camera; copy PS DFCBA0EC70B03C9B writes format 26 without depth. The
  aggregate HDR conflict does not yet prove a mixed-camera error. Prior flat-AA
  section 105 records the same guard; do not repeat its ruled-out
  shader/binding hypotheses without this flight's trace evidence.
- Next capture: user requested Shift+NumLock in the affected scene, then hold
  view 20 seconds. Full capture arms the replay trace and copy provenance;
  ordinary NumLock only requests General and will not save the full trace.
  Replay all preceding H writes and discriminate copy proof, source layout,
  source camera provenance and destination depth/camera agreement together.
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
