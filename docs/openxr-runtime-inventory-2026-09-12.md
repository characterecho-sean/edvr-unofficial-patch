# Installed OpenXR capability inventory

The existing Phase 0 probe was run against the installed 64-bit PiOpenXR,
SteamVR OpenXR and VirtualDesktopXR runtimes. It used the installed SteamVR
OpenXR loader, file version 1.1.58.0, with the repository's pinned Khronos
1.1.46 declarations and an OpenXR 1.0 instance request. Exact paths, manifest
and executable hashes, output and execution receipts remain local.

The machine's active-runtime selection points to PiOpenXR. There was no
inherited `XR_RUNTIME_JSON` override. SteamVR and VDXR were selected only in
their individual probe subprocess environments. Neither the registry nor the
game's environment/configuration was changed. This distinction matters: the
current Frontier proxy tests use Pimax through SteamVR's OpenVR path, whereas
the proposed default OpenXR selection would choose PiOpenXR on this machine.

The probe creates an instance, queries capabilities, and destroys the instance.
It creates no session, graphics device, swapchain or projection layer.

| Runtime-reported identity | Reported version | HMD system query | Stereo recommendation per eye | Limits per eye | Minimum D3D11 feature level |
| --- | --- | --- | --- | --- | --- |
| Pimax OpenXR (installed PiOpenXR) | 0.1.0 | Pimax Crystal Super available | 5424x5356 | 16384x16384 | 11.0 |
| SteamVR/OpenXR | 2.17.9 | HMD system available | 4268x4216 | 8192x8192 | 11.0 |
| VirtualDesktopXR | 1.0.10 | `XR_ERROR_FORM_FACTOR_UNAVAILABLE` | Not queried after failure | Not queried after failure | Not queried after failure |

Versions above decode the runtime's `XrVersion`; they are not inferred package
or headset firmware versions. Both successful HMD queries report primary
stereo, opaque blending, one recommended/maximum sample, mutable FOV, and a
16-layer limit. Their D3D11 adapter LUIDs match each other and the published
game device in the verified Frontier captures. This supports the adapter
candidate; it does not establish texture compatibility between distinct D3D11
devices or prove session creation on the game's device.

All three runtimes advertise D3D11 binding, QPC/time conversion, gaze
interaction, visibility masks, refresh-rate enumeration and composition depth.
SteamVR also advertises performance metrics; the other two do not. These are
advertisements only. The probe does not enable or exercise these optional
features, query gaze system support, enumerate metrics paths, obtain actual
swapchain formats, or establish a physical display rate.

VDXR's instance and extension enumeration succeeded, then the HMD-system query
returned unavailable. Treat that as the observed headset availability result,
not a broken loader or unsupported OpenXR implementation. It needs a connected
compatible headset for the remaining checks.

The next runtime gate is a separate session harness for real startup geometry,
spaces, formats, frame ordering and lifecycle. Existing extension enumeration
cannot substitute for those results. The backend must preserve the configured
runtime choice and log its actual identity; do not assume that replacing the
OpenVR transport keeps the same runtime or recommended image dimensions.
