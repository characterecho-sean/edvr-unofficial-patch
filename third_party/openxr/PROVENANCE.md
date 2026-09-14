# OpenXR SDK dependency provenance

This directory contains generated headers from the official Khronos OpenXR SDK
release 1.1.46. The headers are fetched from the upstream release tag:

* Repository: https://github.com/KhronosGroup/OpenXR-SDK
* Tag: `release-1.1.46`
* Files: `include/openxr/openxr.h`, `include/openxr/openxr_platform.h`,
  `include/openxr/openxr_platform_defines.h`
* License: `LICENSE` (Apache License 2.0, as distributed by Khronos)

The native release bundles the official Khronos Windows x64 loader from
[OpenXR.Loader.1.1.46.nupkg](https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-1.1.46/OpenXR.Loader.1.1.46.nupkg).
`tools/fetch_openxr_loader.py` verifies the archive and exact DLL member,
`native/x64/release/bin/openxr_loader.dll`. It places the dependency under
`third_party/openxr/loader` (ignored by Git). The build verifies the cached
bytes offline before copying them into `build` and embedding them in the
installer. No installed SteamVR or vendor loader is used for release packaging.

Verified on 2026-09-14:

| File | SHA-256 |
|---|---|
| NuGet package | `bbbf8e0a63d7241c9186c7d52692c843151256fa9928a3e607fe1223fae0bce8` |
| Windows x64 release DLL | `a231a20944153cfda9551af135a3e58519f77007f28afc76d3c5d23763be8bde` |
| `LOADER-NOTICES.txt` | `b57895e08b22074931f17605841dda431bd8e3c06ee7962f6a9371a5ff68431c` |

The notice includes Khronos/contributor copyright notices, Apache 2.0 terms,
and the JsonCpp license from the official release source archive. It ships as
`OPENXR-LOADER-LICENSE.txt` beside the loader. The loader selects Windows'
active OpenXR runtime. Bundling it does not install a headset runtime.

Verified against the tag on 2026-09-11 (SHA-256 of upstream bytes):

| File | SHA-256 |
|---|---|
| `openxr.h` | `df412d0088098c91f28ac3f6eff45d88d17bf50e14197370c3e51ce97be1f323` |
| `openxr_platform.h` | `1db2c45a1747dd5c62d2a7521ddbe1952a5af36e866485979eb78eada2a40b10` |
| `openxr_platform_defines.h` | `59a5369c34013beeea505aa0465768acad558ac016bab3320780e2a653a07bfb` |
| `LICENSE` | `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` |
