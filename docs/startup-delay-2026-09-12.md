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

## Next Frontier check

After a clean-stamped build and installation through `tools/install_edvr.py`,
launch Frontier normally, note the delay before the intro, check normal VR
rendering/tracking, and exit. Retrieve both logs with the installed commit as
`--expect-build`. Expect three successful `precompiled compute shader` creation
lines, no runtime HLSL compilation for these variants, and a much shorter first
Present interval. Driver shader creation and unrelated startup work still take
time; the post-change duration must be measured in the game.
