# OpenXR SDK dependency provenance

This directory contains generated headers from the official Khronos OpenXR SDK
release 1.1.46. The headers are fetched from the upstream release tag:

* Repository: https://github.com/KhronosGroup/OpenXR-SDK
* Tag: `release-1.1.46`
* Files: `include/openxr/openxr.h`, `include/openxr/openxr_platform.h`,
  `include/openxr/openxr_platform_defines.h`
* License: `LICENSE` (Apache License 2.0, as distributed by Khronos)

The probe does not link or redistribute the OpenXR loader. It loads a caller
selected `openxr_loader.dll` by an absolute path, with restricted dependency
search flags, and checks that `xrGetInstanceProcAddr` is exported before
calling it. A loader found on a machine is evidence of a loader being
available; it is not evidence that a particular runtime or headset is
installed.

Verified against the tag on 2026-09-11 (SHA-256 of upstream bytes):

| File | SHA-256 |
|---|---|
| `openxr.h` | `df412d0088098c91f28ac3f6eff45d88d17bf50e14197370c3e51ce97be1f323` |
| `openxr_platform.h` | `1db2c45a1747dd5c62d2a7521ddbe1952a5af36e866485979eb78eada2a40b10` |
| `openxr_platform_defines.h` | `59a5369c34013beeea505aa0465768acad558ac016bab3320780e2a653a07bfb` |
| `LICENSE` | `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` |
