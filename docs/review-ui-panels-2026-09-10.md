# UI movement and distant station panels, 13:33 capture

The user reports that the station is much steadier after build `6AA3044B`,
but green targeting chevrons swim over it. The right solar panel looks
softer than the bottom panel. They want sharp UI without motion smear or
shimmer, and asked for a better approach than a fixed reactivity value.

## Evidence from the installed run

The completed flight log is `edvr_gfx_20260910_133006.log`; the eye run is
`eye_133332`. Its input is 2862 × 2826, with DLSS output 4404 × 4348.
The installed configuration has UI depth on, object motion on, mover
rejection off and `advanced.ui_depth_reactive=0`.

The 32 per-eye motion records cover sixteen frames. All have a valid body,
usable camera rows and continuous NVIDIA history. There is no origin jump
or requested history reset in this capture. Both eyes use grid version 627,
with 328 records and an age progressing from 1 to 16 frames. The fitted
body motion has a 22.8293 ms baseline and 0.007764 m RMS residual. This
does not prove every station pixel has the right motion, but rules out a
whole-frame reset or body rejection in this particular sequence.

Analysis images, shader disassemblies and the CSV are under
`build/review_motion/station1333`. Three earlier body rejections recover
after 8, 9 and 14 frames; they are outside the captured sequence and remain
a separate origin/camera-stamp question. The distance gate was not relaxed.

## Two UI motion errors

First, motion classification depended on the fixed NVIDIA bias being
positive. At the user's setting of zero, the pass could lose the mask
that excludes floating UI from station rotation. Native TAA also needs
that classification. `uiDepthCoverageMask` now supplies coverage
independently of `uiDepthReactiveMask`; zero fixed bias preserves both
floating and attached UI identities.

Second, the flight HUD coverage shader compared reverse device depth
(`SV_Position.z`) with a linear distance sampled from the game's depth
resolve. The captured vertex shader `B7790CBFC6554097` writes clip W to
`TEXCOORD1.z`; pixel shader `8DEF46452FA459F5` compares that interpolant
with the resolved scene distance. This establishes the units for these
captured shaders. The coverage replacement now compares those two linear
distances and converts the selected scene distance back to device depth.

For example, a floating stroke at 30 m drawn over a station at 15 km must
keep the camera path at the station's depth, rather than inherit the
station's rotation or become cockpit geometry. A stroke at 14 km can be
classified as attached. WARP tests run the production HUD coverage shader
on those cases, plus glow and occluded geometry, in native and trained
modes with fixed bias zero. The live game depth remains unchanged.

## Adaptive UI history

A fixed bias applies the same compromise to a stationary label and a
changing digit. The temporal pass now keeps two raw colour/coverage
evidence textures per eye and aligns previous evidence with the selected
motion. It compares only marked content, tolerates a one-pixel neighbourhood,
and reduces history where it detects new, erased or recoloured UI.
The previous evidence is not the accumulated output, avoiding feedback
from an already blurred label.

Native TAA reduces its history weight. Full-frame DLAA/DLSS receives the
result in its existing bias-current-colour mask, alongside the optional
fixed bias and mover rejection. Marked UI is protected from the separate
world-mover rejection rule. `advanced.ui_depth_reactive` now defaults to
zero; existing explicit values remain an optional fixed NVIDIA bias.
The user's installed value was already zero.

The evidence textures have independent eye histories, reset on size or
history invalidation, and advance only after a successful treated frame
with an evidence dispatch. Compute state now saves/restores nine SRVs and
seven UAVs, including the new evidence bindings. At the captured render
resolution, the four RGBA8 textures total approximately 123.4 MiB. Live
GPU cost and headset appearance still require flight validation.

This remains a heuristic over the final composited image. A background
change under a translucent stroke can look like a UI change, and nearby
same-colour strokes can conceal a small glyph change within the tolerance.
It does not render UI independently after temporal reconstruction. Such a
path would require retaining a clean scene and reproducing the supported
UI draws, blending and occlusion at the final output resolution; the
current depth reissues do not provide those colour layers. No claim of
perfectly blur-free UI follows from the synthetic tests alone.

## Solar panel finding and capture correction

The right panel lies mostly outside the old treated sequence: 1400 output
pixels cover less of the scene than 1400 input pixels under DLSS. The
bottom panel is included. Paired treated crops now scale with output/input
resolution so both sequences cover the same angular area at their native
pixel scales. For this run's dimensions, that is approximately a 2155-pixel
treated crop corresponding to the 1400-pixel raw crop.

Projecting the actual pool readers' records finds coherent station motion
for the known right and bottom panel instances. The audit uses vertex
shaders `436193B352A2897E`, `EB5234DB6ADB491D` and `DE545DC8EE4FBB87`;
merely having the instance pool bound does not establish that a shader
reads it. The experimental stepped-part override remains disabled.
The existing evidence does not distinguish imperfect pixel coverage from
reconstruction of a thin panel grid over a brighter background. No
speculative panel-motion correction is included.

## Validation

The focused WARP run passes 3016 checks, including production coverage,
depth/occlusion/state handling and the adaptive shader's stable, changed,
new, erased, shifted and unmarked-background cases. It also feeds the
GPU-written evidence into the following frame to verify that unchanged
UI remains stable after storage.

The full NVIDIA SDK build passes, including 3895 drive/capture regression
checks and the 247-key configuration contract. A targeted hardware probe
passes 18 native TAA, DLAA and DLSS evaluations across both eyes with UI
resources enabled, checking output dimensions and preservation of the
caller's occupied SRV slot 8 and UAV slot 6. Its log confirms allocation of
the UI evidence resources. The HLSL compiler emits an internal optimization
convergence warning for the native shader; compilation and evaluations
succeed. The final broad NVIDIA smoke run also passes with the UI resource
path enabled, including native history, DLAA, DLSS, foveated paths and the
motion/jitter conventions rig.

Build `6AA30E78` is installed in Steam. Both DLL hashes were verified;
the previous confirmed station-motion build is backed up under
`build/review_motion/station1333/install`. The installation made no INI
edits and verified its hash remained unchanged during installation.
The currently explicit UI bias is still zero. Headset sharpness, stability
and performance await the user's next flight; solar-panel softness remains
unresolved pending matching treated coverage.
