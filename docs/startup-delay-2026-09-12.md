# Black screen before the intro video

## Status

*Written 2026-10-01 from the entries below; restates them. Update it whenever this doc changes.*

- **State:** the 18.4 s first-Present compile is gone (FLOWN OK 2026-09-12, 58d1566: "No more
  initial black screen"). The three temporal shaders, and since then every fixed shader of the
  d3d11 proxy, are compiled by `tools/temporal_shader_build` and created with
  `shaderSwapCreate*`; the log says `precompiled <stage> shader <name> created (0x..., N ms)`.
  2026-10-01 (BUILT, NOT FLOWN): the OpenXR runtime's last four compiles (the stereo renderer's
  blit and skybox, four `D3DCompile` calls inside `VR_InitInternal`) moved to the same tool
  (`--stereo-output`), so `edvr_openxr_runtime.dll` no longer imports d3dcompiler_47.dll. The
  build's PE gate (`openxr_pe.py --forbid-import`) holds that for the runtime and the proxy.
- **Still compiled at runtime, by design:** two shaders of the d3d11 proxy whose HLSL macros are
  the user's own settings, unknowable at build time: `fss_panel.cpp` (`EDVR_FSS_DIST`, from
  `experimental.fss_panel_distance`, any float 0.2-3.0; off at the default 1.0) and
  `target_sharp.cpp` (`EDVR_RCAS_SHARP`, `EDVR_SCALE_PROBE`, from
  `advanced.target_indicator_sharpen` and `advanced.target_indicator_scale_probe`; off by
  default, `experimental.target_indicator = stock`). They are the only EDVR code that loads
  d3dcompiler_47.dll, by `LoadLibraryW` on demand and only in those opt-in settings.
- **Open:** the flight reading in the last entry (the runtime's `runtime_shaders` line and the
  `shaders=` stretch against the ~443 ms of the 2026-09-15 flight); whether the two
  settings-bound shaders should become a closed set of build-time steps or read their value from
  a constant buffer. Both change the shader, so neither is a pure move.
- **Ruled out:** OpenVR initialization as the cause of the 18 s (the VR proxy started 1.3 s after
  the interval ended); `fss_dump`'s series reducer as a runtime compile (it is
  `kFssSeriesBytecode`; only an include comment said otherwise).
- **Next flight:** any VR launch on the native runtime: `runtime_shaders,source=precompiled,
  created=4,stretch_ms=...` in the runtime log and `shaders=` in `runtime_startup_steps` a few
  milliseconds, not ~443. The pictures do not change (same shaders, same bytes).
- **Detail:** the measured cause and the first correction are "Measured cause" and
  "Correction"; the 2026-10-01 inventory, the runtime's pipeline, the pins and what was left are
  the last entry.

## The report

Users reported a longer black screen before Elite's intro video. The Frontier
run on `e9802b7` supplies direct timing evidence: both graphics and VR logs
passed `tools/edvr_log.py --target frontier --expect-build e9802b7`, queried
separately by tag. The graphics log is `edvr_gfx_20260912_042547.log`, linked
stamp `6AA4C1E3`; the VR log is `edvr_vr_20260912_042607.log`, stamp
`6AA4C1EA`.

## Measured cause

| Graphics log event | Timestamp |
|---|---|
| Graphics proxy starts | 04:25:47.952 |
| First PresentEnter | 04:25:48.304 |
| Supersample shader compile completes | 04:25:48.325 |
| Fast motion shader compile completes | 04:25:48.836 |
| Diagnostic motion shader compile completes | 04:25:49.931 |
| Temporal-AA shader compile completes | 04:26:06.669 |
| First PresentExit | 04:26:06.669 |
| VR proxy starts | 04:26:07.992 |

The first Present hook occupies 18.365 seconds. `device_hook.cpp` calls the
real Present before its menu/frame-boundary work; `vScreenFrameBoundary` calls
`temporalPassTick`, which synchronously compiles both motion-vector variants
and the temporal-AA shader. The delay blocks the render thread before it can
present the intro's next frame. The log measures the hook interval, not the
driver's Present alone.

Ruled out: OpenVR initialization causes this observed 18-second interval,
because the VR proxy starts more than a second after the interval ends. Other
launch costs can still exist; this finding applies to the measured interval.

The run uses SteamVR, with recorded eye textures of `2268x2240` and temporal
DLSS selected. The headset and connection are unreported. This compile path is
in the graphics proxy and does not depend on OpenXR or the headset runtime.
DLSS also needs the motion shader, and the temporal-AA shader remains available
for its existing fallback and fovea paths.

## Correction

Move the fixed temporal HLSL into `src/d3d11/temporal_shader_source.h`,
retaining the exact adjacent C++ raw-string declaration.
`tools/temporal_shader_build` compiles all three variants during every build
and atomically emits a generated header only after all succeed. Compile
settings remain `cs_5_0`, flags zero: entry `mv` with
`EDVR_TEMPORAL_DIAGNOSTICS=0`, entry `mv` with the macro set to `1`, and entry
`main` with the existing default macros.

The graphics DLL embeds these bytecode arrays and creates the shaders on the
same device/context path as before. No HLSL compilation, cache directory or
background GPU work is needed for these variants at game startup. Both motion
variants and the temporal-AA feature remain available. Smaller unrelated
shaders retain their current compilation paths.

The generator's self-test and a WARP check of all three embedded shader
variants gate the build. Existing motion, screen, celestial and private UI
depth tests continue to consume the same source. Each production shader
creation logs its name, result and elapsed milliseconds, so a missing or failed
path is distinguishable from a successful warmup.

## Desk validation

The complete build passed in `build/startup-precompiled-validation.log`.
Compiler timings were 578 ms for the fast motion variant, 1203 ms for the
diagnostic variant, and 17328 ms for temporal AA. All three generated blobs
created successfully on WARP (50824, 69840 and 110512 bytes respectively). The
existing shader-consumer regressions and both menu-worker exit children also
passed. A direct comparison against the previous commit verified that the
extracted temporal shader declaration is identical. Existing HLSL warnings also
appear in the pre-change Frontier log; shader arithmetic is unchanged by this
correction.

## Frontier verification: 58d1566

The clean build passed in `build/frontier-58d1566.log` and was installed and
hash-verified through `tools/install_edvr.py`, preserving the Frontier INI. The
subsequent graphics log `edvr_gfx_20260912_044925.log` (stamp `6AA52D5B`) and
VR log `edvr_vr_20260912_044927.log` (stamp `6AA52D63`) both passed
`--expect-build 58d1566` and report `v0.15.1-25-g58d1566`.

| Graphics log event | Timestamp | Creation duration |
|---|---|---|
| First PresentEnter | 04:49:25.971 | |
| Precompiled fast motion shader created | 04:49:25.993 | 0.162 ms |
| Precompiled diagnostic motion shader created | 04:49:25.993 | 0.242 ms |
| Precompiled temporal-AA shader created | 04:49:25.994 | 0.405 ms |
| First PresentExit | 04:49:25.994 | |

The first Present hook fell from 18.365 seconds to approximately 23 ms. All
three shader creations returned `S_OK`, totaling 0.809 ms, and the warmup
explicitly reports no runtime HLSL compilation. Smaller unrelated shaders
retain their existing compile paths. This comparison measures the identified
startup hook, not the duration of the entire game launch.

Sean reported: "Ran it, did not crash. No more initial black screen". Windows
Application Error / Windows Error Reporting events (IDs 1000/1001) contained no
matching crash since 04:49:20 local, just before this launch. The startup
correction and repeated exit check pass on this Frontier build. This report
does not establish new headset/runtime compatibility or complete the OpenXR
transport and GPU-timing qualification.

## 2026-10-01: the last four compiles in the runtime, and the two that stay (BUILT, NOT FLOWN)

Sean asked for the remaining runtime shader compiles to move to build time. The inventory
first, at origin/main 069ebee4, by search of `src\` for every `shaderSwapCompile*`,
`D3DCompile` and `d3dcompiler_47`:

| site | what it was | outcome |
|---|---|---|
| `src\openxr\d3d11_stereo.cpp` (the native runtime) | four direct `D3DCompile` calls on the first `VR_InitInternal`: the stereo blit's vertex and pixel shader and the skybox's, fixed HLSL, no macros. `d3dcompiler.lib` was on the link line, so the runtime DLL imported d3dcompiler_47.dll at load (`dumpbin /dependents`, build of 2026-10-01 14:40) | MOVED (below) |
| `src\d3d11\fss_dump.cpp` | named in the request as `shaderSwapCompileCs` | already precompiled: `kFssSeriesBytecode` through `shaderSwapCreateCs` (`fss_dump.cpp:218`); only the include's comment still said `shaderSwapCompileCs`: fixed |
| `src\d3d11\fss_panel.cpp:85,88` | two vertex shaders by `shaderSwapCompileVs`, macro `EDVR_FSS_DIST` = `%.6f` of `experimental.fss_panel_distance` | STAYS: the macro is the user's setting (any float 0.2-3.0); the fix is off at the default 1.0 |
| `src\d3d11\target_sharp.cpp:299,309` | the EASU and cubic pixel shaders by `shaderSwapCompilePs`, macros `EDVR_RCAS`, `EDVR_RCAS_SHARP` (`%.8f` of 2^-`advanced.target_indicator_sharpen`, any float 0-2, or off) and `EDVR_SCALE_PROBE` | STAYS: macros from two settings; off by default (`experimental.target_indicator = stock`) |
| `shaderSwapCompileCs` and its helper | the compute form of the runtime compile | no caller anywhere in `src\`: REMOVED (the rigs' own stubs of it are theirs) |
| `sunglare_vs.h`'s header comment | said the sun-glare vertex shader is compiled at runtime | it is `kSunglare*Bytecode`: fixed. `ui_depth.cpp:32`'s include comment names `shaderSwapCompilePs` for the alpha-aware depth shaders (they are `shaderSwapCreatePs`): left, adjacent |

**The rule for what stays**: a shader whose source or macros are runtime data cannot move. `fss_panel` and
`target_sharp` bake a user setting into the HLSL as a macro, and a build cannot know a float the user types. A pure
move would need a closed set of variants (a stepped setting) or a constant buffer in place of the macro; either
changes the shader, so its bytes would no longer equal today's compile, which the request ruled out. `shader_swap.h`
now names the two as the only callers of the compile forms left.

**What moved, and how.**
- `src\openxr\stereo_shader_source.h` holds the two HLSL texts byte for byte as they were
  (`kStereoBlitHlsl`, `kStereoSkyboxHlsl`).
- `tools\temporal_shader_build` takes `--stereo-output` and writes `build\gen\openxr_stereo_shader_bytecode.h` (the
  arrays `kStereoBlitVsBytecode`, `kStereoBlitPsBytecode`, `kStereoSkyboxVsBytecode`, `kStereoSkyboxPsBytecode`: 736,
  1704, 736 and 3432 bytes) with a key of its own over the text, the variant table and the compiler DLL: reused when
  current, written atomically, and a dry run writes nothing. The parameters are the calls it replaces, field for
  field: source names `EDVR captured blit` and `EDVR skybox`, entries `vs` and `ps`, profiles `vs_5_0` and `ps_5_0`, no
  macros, `D3DCOMPILE_ENABLE_STRICTNESS`, no second flag word.
- `D3D11Stereo::initialize` creates the four shaders from those bytes. `build.bat` adds `/I"%GEN%"` to the three compile
  lines that include `d3d11_stereo.cpp` (the runtime and two rigs) and drops `d3dcompiler.lib` from the runtime's link line,
  so a compile that comes back fails to link rather than quietly importing the compiler again.
- The host's startup trace gains one line beside `runtime_startup_steps`: `runtime_shaders,source=precompiled,created=4,
  stretch_ms=<ms>,units=wall_ms`. `stretch_ms` is the `shaders=` stretch: four creations and the blit's buffer and
  sampler, where it held ~443 ms of compile in the 2026-09-15 flight.

**Pins** (the byte-equality test asked for, in two places, and a third that watches the import).
- `temporal_shader_build --self-test` (in the build): independent copies of the four old calls (source name, entry,
  profile, flags) and the FNV-1a64 hash of each text as it stood in `d3d11_stereo.cpp` (blit 0x8F805D7103F7BA83,
  skybox 0x1F8D73118F42ED8B). Each shader is compiled at test time with the old call's arguments and compared byte for byte
  with the tool's output, then reflected for its stage and SM5. A one-byte change to the text changes its hash; the key
  follows the text, the flag word, the profile and the source name; the header's key line, array names and every byte
  round-trip; the CLI (`--stereo-output` needs `--output`, a value, once); reuse, regeneration of a stale header, and a dry
  run that writes nothing.
- `tools\openxr_stereo_test` (the WARP rig that renders with these shaders): the text in the header equals the old text
  frozen in the rig, and each embedded array equals what `D3DCompile` makes of that text at test time with the old call's
  arguments; its pixel checks then run the embedded bytes (13,420 checks, 0 failures in the build).
- `tools\openxr_pe.py --forbid-import d3dcompiler_47.dll`, in the build for both DLLs (full build and `--dll-only`): fails
  if either imports the compiler, at load or by delay import. It reads the PE import tables and never loads the DLL.
  Self-test (42 checks): both tables, any case, a path in the name, a third descriptor added to the synthetic image. Run
  on the 14:40 runtime it names d3dcompiler_47.dll (exit 1); on the 16:07 runtime and on the proxy it passes.
  One control was not run: a changed text in the stereo rig's frozen copy (the build tool's one-byte-hash check is the same
  property).

**Does anything still load d3dcompiler_47.dll?** The runtime: no. `dumpbin /dependents` of the 16:07 build
reads `dxgi.dll`, `ADVAPI32.dll`, `KERNEL32.dll` (the 14:40 build also read `D3DCOMPILER_47.dll`), and the image holds neither
the name nor `D3DCompile` anywhere. The d3d11 proxy: no static import (it never had one: `KERNEL32`, `USER32`, `GDI32`,
`VERSION`, `ADVAPI32`), and the name remains as the one `LoadLibraryW(L"d3dcompiler_47.dll")` of `shader_swap.cpp`'s vertex
and pixel compile and in its two log messages, reached only from `fss_panel.cpp` and `target_sharp.cpp`: an install that
sets `experimental.fss_panel_distance` away from 1.0 or `experimental.target_indicator = sharp`. In a default install
nothing of EDVR's loads it.

**Next flight**: any VR launch on the native runtime. `edvr_log.py --tag openxr --expect-build HEAD` first. The runtime
log has `runtime_shaders,source=precompiled,created=4,stretch_ms=<ms>` and `runtime_startup_steps` has `shaders=` of a few
milliseconds where the 2026-09-15 flight had ~443. The pictures do not change (same shaders, same bytes). With
`experimental.fss_panel_distance = 0.7` or `experimental.target_indicator = sharp`, the proxy's `replacement vertex
shader compiled` / `replacement pixel shader compiled` lines are the compile that stays.
