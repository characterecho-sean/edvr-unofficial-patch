# Station depth and chevrons, 14:53 comparison

Follow-up: the [15:20 yaw/sprite review](review-yaw-sprite-depth-2026-09-10.md)
shows that the station depth correction is active, but chevron depth one
persists. The old UI mask did not identify its producing shader. The new
ledger and sprite vertex shader identify forced raster depth as the
remaining source; the HUD conversion change alone did not fix it.

The user captured DLSS and AA-off views after build `6AA316A0`. They still
see blur beneath the green chevrons and insufficiently defined solar
panels. This run supplies direct evidence of two incorrect depth inputs;
increased render resolution did not correct them.

## Capture and motion evidence

`edvr_gfx_20260910_145116.log` confirms the installed build. DLSS run
`eye_145322` has 3523 × 3478 input and 4404 × 4348 output. AA-off run
`eye_145332` has 3523 × 3478 input and complete images. Both use the same
80% game render scale. The AA-off view is ten seconds later with a changed
head pose and station orientation; it is not an identical-frame A/B.
The paired C/T images in the DLSS run do show the same submitted frame
before and after reconstruction.

All 32 DLSS motion records have valid body motion, usable camera rows,
continuous NVIDIA history and no origin jump. Body-fit residuals are
0.008971–0.012775 m. As in the earlier reviews, a valid body estimate does
not prove that its motion reaches the visible station pixels.

The new binary input dumps are finite and carry scene frame 11002. UI is
bound, fixed bias is off, and adaptive UI history is active (`ui_flags=6`).
Within input rectangle `(2000,1450)..(2450,1820)`, covering the hub,
chevrons and much of the panels, only 79 of 166,500 pixels have nonzero
history bias. Reactivity is not broadly discarding the station's history.

## The station is reconstructed kilometres too close

The saved motion vectors let us identify the projection actually used.
With `A = -0.025 / (50000 - 0.025)`, the finite-far fallback, the body and
camera model reproduces the saved vectors to half-float precision. With
`A=0`, the body vectors no longer match the captured values. The log also
has no accepted scene-projection-row message.

Across the visible geometry in the wider station rectangle, excluding the
chevron depth spikes and their immediate neighbours, 73,951 pixels match
camera-only motion and 12,281 match station motion (0.002-pixel tolerance).
Large portions of the ring, panels and hub are absent from station motion
coverage even though the body estimate itself is valid.

An independent comparison projects 210 drawn station-record origins from
the pool into the captured scene depth using the camera rows. Their view
distances range from 15.5 to 26.8 km. The infinite-far scene conversion
`metres = 0.025 / depth` has a median absolute difference of 20.45 m;
152 records have a depth sample within 300 m. The finite-far conversion's
median absolute difference is 5697.16 m, and none is within 300 m.

This compares record origins with nearby visible mesh depth, not exact
vertex correspondences. It selects the closest depth within a 5 × 5
neighbourhood; occlusion and mesh extent explain some large residuals.
Those limitations cannot explain the consistent multi-kilometre shortfall
of the finite conversion. A 20 km surface illustrates the arithmetic:
scene depth is 0.00000125, but the finite fallback decodes it as 14.286 km.

As a separate geometry check, all 9,686 sampled station pixels reconstructed
with infinite-far depth are within 1500 m of a drawn station part in the
axis-aligned reach metric. Only 648 are with the finite conversion. This
is a proximity check against the recorded parts, not a replay of the exact
GPU occupancy grid, but it explains why the wrong depth misses that grid.

The earlier projection fix used a scene projection row when available, but
its fallback still used OpenVR's finite far plane. In this flight the
camera-row feed does not provide a usable scene projection row, so the
old distance error returns. `temporalSceneProjection` now centralizes the
selection for temporal reconstruction and private UI/smoke depth encoding.
It respects a usable measured row; otherwise it uses Elite's measured
infinite-far scene encoding with the runtime's near distance. Rows with
invalid values or a mismatched near scale are rejected. A directly measured
finite row remains supported.

## Chevron depth spikes

All 387 marked chevron pixels in the inspected rectangle have temporal
depth **1.0**, although their original scene depth is approximately
`1.56e-6..1.66e-6`. Their median motion is `(1.409, 1.962)` input pixels,
where nearby scene motion is roughly `(0.105, 0.378)`.

The nearest-depth search also encounters those spikes in neighbouring
pixels, so the effect extends beneath and beside the strokes. Precise
opacity coverage alone did not correct this error.

The previous shader converted the game's resolved-depth sample using
EDVR's metre-to-device-depth coefficients. The final captured result proves
that depth is wrong, but does not expose the live shader's constants and
resolved-depth sample separately; it therefore cannot identify which
assumption in that conversion failed in the flight.

Floating HUD coverage now reads the original scene depth directly at the
rasterized pixel and preserves it verbatim. It no longer needs to interpret
or re-encode the game's resolved-depth value. The game's own depth/clip-W
comparison and exact opacity march remain for coverage eligibility.
Attached strokes retain their existing depth handling.

The scene read is a cached SRV over the original depth resource. The
coverage draw writes the separate private target, and PS slot 2 is saved
and restored. There is no extra full-frame copy. If the source cannot be
read, that HUD coverage draw is declined without changing game rendering.
The live scene depth and the confirmed smoke-hole repair remain intact.

## Validation and scope

Build `6AA31D06` passes the complete build, the temporal arithmetic suite,
3,895 drive-switch checks and the 247-key configuration contract. The final
focused UI suite passes 7,739 checks against the original Steam HUD shader.
It includes a scaled resolve/clip-W case that previously saturated to the
near plane despite distant scene depth, plus original-depth SRV identity,
format compatibility, frame invalidation and binding restoration checks.
The D3D debug layer reports no warnings or errors in that suite.

The NVIDIA hardware smoke suite also passes, including the motion/jitter
convention rig. These tests establish the corrected calculations and GPU
state handling; headset image quality still requires a flight check.
The capture probe also passes 96 hardware eye submissions across DLSS,
AA off and bounded AA-off captures. Every saved raw frame, image size,
CSV sequence and motion/depth/bias input was checked after readback.

The motion CSV now records the actual projection A/B coefficients and
head-motion parameters, alongside the existing body/camera data, so future
captures do not need to infer the projection from saved vectors.

Both patch DLLs and the INI are prepared with rollback copies and guarded
by hashes under `build/review_motion/station1453/install`. The test restores
DLSS after the AA-off capture. Render scale stays at 80%, and sharpening,
reactivity, body-motion gates and the occupancy reach are unchanged.
Build `6AA31D06` was installed with the game closed. Both DLL hashes and
the restored DLSS INI match the prepared plan; the graphics XML was not
modified. Flight confirmation remains outstanding.

Scripts, input maps, annotated images and numerical results are under
`build/review_motion/station1453`. In particular, `geometry-check.txt`,
`motion-ownership.png` and `maps-detail.png` document the measured defects.
