# OpenXR implementation status

The approved design in [openxr-port.md](openxr-port.md) was pushed to main as
`8c617dc` before implementation began. This change starts Phase 0; it is not
an OpenXR backend or a completed Phase 0 qualification.

## Implemented evidence tools

- The original shipping proxy remains the default. The new startup-only
  `advanced.openvr_census = on` setting enables typed forwarding for the exact
  four historical interfaces. The 84 methods use Valve v0.9.20 declarations;
  aggregate returns are ordinary C++ member calls. The bounded cache preserves
  repeated-getter wrapper identity and passes through when exhausted. Runtime
  targets remain owned by the runtime.
- The ABI log records the first four calls per method and caller category,
  with QPC and thread ID. The call site is classified by its containing
  module: game executable, EDVR, another module, or unknown. Calls EDVR makes
  directly to saved runtime pointers bypass these wrappers. Other interface
  versions retain proxy behaviour. This instrumentation does not define the
  future owned backend's supported-interface policy.
- CPU order logs record at most 64 observations per wait/submit/Present event
  kind and 16 per intercepted GPU-command kind. They include actual context
  pointers, immediate/deferred type, thread IDs and QPC. The two DLLs have
  independent ordinals; use QPC to compare them. A command entry is a CPU
  observation, not evidence of GPU completion. Existing unhooked commands are
  outside this capture.
- Device creation logs include adapter LUID, feature level and the published
  first-device identity. Validated eye submissions log texture/device
  identity, full descriptors, colour space and submit flags before EDVR
  substitutes a texture. Changes are bounded to 16 lines; unchanged handles
  are resampled no more often than every six seconds. The initial unvalidated
  submissions and skybox resources are not covered by this descriptor probe.
- `build\openxr_probe.exe --loader C:\absolute\openxr_loader.dll` loads a
  trusted, explicitly selected loader with restricted dependency search. It
  reports extensions, runtime identity, HMD system limits, stereo view sizes,
  blend modes and D3D11 adapter/feature-level requirements. It requests OpenXR
  1.0, creates an instance, and destroys it on exit. It creates no session,
  device or swapchain and does not change runtime selection. The declarations
  are pinned to Khronos SDK 1.1.46 with the upstream license retained.

Set the census key in each proxy's applicable INI before launch. Use
`tools/install_edvr.py` for installation and `tools/edvr_log.py` to retrieve
logs and verify build identity. The selected test installation is Frontier.
Preserve its tuned INI and runtime choice when enabling the census; use a
backup and review the single-key change before installing it. Steam is not the
test target.

## Review decisions and remaining gates

The GPU timing draft failed review: it permitted overlapping outer scopes,
accepted deferred contexts, recreated queries instead of reusing them, aged
samples by API-call count and mishandled zero-frequency results. It and its
insufficient tests were excluded. No new GPU queries or Monitor values are
enabled by this change. Rebuild the bracket after an ownership/order capture,
with the single outer scope and failure tests specified in the approved plan.

The first ABI test draft used a hand-built raw vtable and crashed on a matrix
return, causing a Windows error dialog. That fixture was removed. The retained
tests use concrete C++ implementations and set Windows error mode to suppress
interactive crash dialogs.

Before treating Phase 0 as complete, still collect and review:

1. Exact init/shutdown/re-init and interface-validity export traffic,
   meaningful property/controller/event arguments and returned events, first
   geometry results, skybox descriptors and complete lifecycle behaviour. The
   current bounded method census establishes calls, not this full semantic
   inventory.
2. Real startup and flight order/context/device evidence, including texture
   reuse, both eyes, mirror work and deferred command-list execution. A
   missing line is not proof of absence. The first published device is still
   an unverified candidate for the OpenXR graphics binding.
3. Installed SteamVR, VDXR and Pimax runtime probe reports, then a separate
   session harness for actual formats, startup geometry, refresh rate, gaze,
   tracking and loss/focus behaviour. Extension advertisement alone is not
   functional support.
4. A corrected GPU bracket, matched-frame SteamVR correlation and the Monitor
   source/validity changes. The OpenXR backend, transport parity, field
   qualification and retirement proposal follow those gates.

No configuration key or existing feature has been retired.
