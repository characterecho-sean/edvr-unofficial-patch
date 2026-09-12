# Black screen before the intro video

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
