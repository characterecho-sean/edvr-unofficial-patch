# XeSS for flat EDVR on Intel B390

## Status

- State: implemented on codex/xess-flat; first actual Elite flat test pending.
  The user wants to test locally on this device first, then test on Windows.
  Keep the work on this branch; do not merge to main.
- Environment: ONEXPLAYER 3, Intel Arc B390 PTL 8086:b080, CachyOS xe, Proton
  Experimental experimental-11.0-20261001-x86_64, DXVK
  v3.1.1-47-g685301564ea3486, private Mesa 26.3.0-devel git-37bf953a58. Normal
  Elite startup now reaches the menu without EDVR or a debugger; see
  planetary-startup-intel-b390-2026-10-11.md.
- Branch adds fix.temporal_aa=xess for flat only, a pinned SDK fetcher, a
  D3D11/D3D12 shared-texture/fence backend and installer payloads. Existing
  modes and defaults stay unchanged. No live EDVR config exists yet.
- Latest official package: Intel XeSS SDK v3.0.2, peeled commit
  8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0. Its D3D11 DLL reports XeSS-SR
  runtime 2.0.2; package and component versions differ.
- Windows SDK handoff: run `python tools/fetch_xess.py --dest third_party/xess`
  before the full build; the fetcher pins and verifies the SDK and the build
  stages `libxess.dll` with Intel's license notices for the flat installer.
- Ruled out: a straightforward native D3D11 XeSS integration on this
  Proton/Mesa stack. D3D11CreateDevice succeeds, the SDK loads, but
  xessD3D11CreateContext returns -2, XESS_RESULT_ERROR_UNSUPPORTED_DRIVER, and
  a null context. Init/Execute were correctly not called.
- Evidence: disposable-prefix probe and DXVK logs under
  /tmp/edvr-xess-probe-20261011. DXVK identifies B390 and driver 26.2.99, the
  private development build; system Mesa reports 26.2.4.
- Ruled out: immediate XeSS Vulkan use of DXVK's existing device. SDK required
  queries succeed and require Vulkan 1.3.0, VK_EXT_mutable_descriptor_type, and
  mutableDescriptorType=true; DXVK interop returns matching handles, but DXVK
  did not enable the required extension. XeSS Vulkan context creation was not
  called with these missing prerequisites.
- Confirmed: standalone Wine Vulkan XeSS context creation, init, execute, queue
  submit/wait and RGBA16F readback all succeed on private Mesa B390. This is
  basic execution only; it does not prove temporal quality or DXVK interop.
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
- Next flight: compile final phase changes, install/verify the flat branch with
  XeSS enabled, launch using the existing private Mesa wrapper and read the
  correct-build EDVR log for positive xess execute evidence.
- Temporary settings: planned initial flat profile fix.temporal_aa=xess; no new
  diagnostic config keys. No EDVR build has been installed yet.

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
