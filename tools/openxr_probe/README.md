# OpenXR capability probe (Phase 0)

This is a desk probe for the approved Phase 0 runtime inventory. It loads the
OpenXR loader dynamically, enumerates instance extensions, creates an OpenXR
instance only to query system capabilities, and reports runtime name/version,
head mounted display availability, primary stereo recommendations, blend
modes, and D3D11 graphics feature level requirements. It does not create a
session, swapchain, graphics device, or change registry/runtime settings.

The repository's `build.bat` builds `build\openxr_probe.exe` and runs its
`--self-test`. The test exercises the same enumeration helper as the probe:
typed output initialization, real count growth/retry, shrinkage, empty
results, runtime errors, invalid returned counts, allocation limits and retry
exhaustion. It does not load a runtime.

To compile only the probe from an x64 Native Tools Command Prompt:

```
mkdir build\openxr_probe
cl /nologo /W4 /O2 /EHsc /std:c++17 /MT /Ithird_party/openxr/include /Fo:build/openxr_probe/ /Fe:build/openxr_probe.exe tools/openxr_probe/openxr_probe.cpp
```

Run `build\openxr_probe.exe --loader C:\path\to\openxr_loader.dll`; the loader
path must be absolute, so an ambiguous current-directory/PATH DLL is never
loaded. Use a trusted installed Khronos loader. The loader selects the active
runtime; the probe does not change that selection. It requests OpenXR 1.0 for
these queries, even though the pinned declarations also support 1.1.
`--session` is deliberately rejected; session and startup geometry testing
remain future work.

An extension being listed only proves that the runtime advertises it. It does
not prove session, graphics binding, gaze, timing, or headset functionality.
PiOpenXR and PimaxXR are recorded as separate runtime identities when the
runtime itself reports them; the probe does not infer one from the other.
