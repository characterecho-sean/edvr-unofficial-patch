# Ship yaw and chevron depth, 15:20 capture

The user reports a perfect stationary station, but blur during ship yaw,
sky smearing and persistent chevron artifacts. Ship yaw alone triggers
the symptom; they also turned their head to take the dumps.

The completed log is `edvr_gfx_20260910_151820.log`, from installed build
`6AA31D06`. Runs `eye_152024` and `eye_152045` contain sixteen paired raw
and reconstructed frames each, at 3523 × 3478 input and 4404 × 4348 output.
Analysis scripts, copied traces and measured star matches are in
`build/review_motion/station1520`. Render scale remains 80%, sharpening zero.

## Correct world motion was available but disabled

Both runs use the corrected infinite scene depth: projection A=0,
B=0.025. The stationary run has continuous world/body paths. The moving
run has no origin jump, no requested history reset and acceptable camera
rows throughout, but disables the world and body paths for frames
12952–12957 in both eyes. NVIDIA history continues across these changes.
The log repeatedly identifies the scene as a camera that does not follow
the head, even while the populated flight scene is in view.

The detector compares the magnitude of the camera rotation with the head
rotation. The camera contains both ship and head rotation: opposing turns
can cancel. Their magnitudes alone cannot identify an unrelated camera.
Worse, the old score only updated while the head moved by over 0.1 degrees;
after a false rejection, ship yaw with a stationary head could leave the
world path disabled until another sufficiently large head turn.

This capture lets us check the rejected rows against actual image motion.
The analysis matches isolated stars between successive raw C frames in
crop rectangle `(660,270)..(1300,470)`, which lies in unobstructed sky.
It uses local intensity centroids and mutual nearest matching within
20 pixels, then compares current-to-previous projection with the recorded
camera and head matrices, accounting for both frames' jitter.

Across the six disabled frames there are 473 star matches. Median positional
error is 0.1714 input pixels for the camera prediction and 5.6245 pixels for
the head-only prediction the shader selects when the world path is off.
On the remaining moving frames, the camera error is 0.1673 pixels across
715 matches. These centroid measurements have sampling and brightness
uncertainty; they distinguish a multi-pixel motion error, not a claim of
perfect subpixel calibration. They validate the camera independently of
the temporal output. The binary MV dump covers the first frame only;
later path selections come from each frame's recorded parameters.

`temporalCameraFollowScore` now trusts the selected bound scene block in a
scene with at least 50 draws. This also immediately recovers with a still
head. The existing continuity/rotation plausibility and translation-jump
checks remain. Sparse menu scenes remain excluded from world motion, and
ambiguous auxiliary camera chains retain the head-follow detector and
resynchronization mechanism. Captures now append `rowsBound`, `rowsFollow`
and `sceneDraws` to expose these decisions directly.

## The remaining chevron depth comes from the sprite vertex shader

In stationary run 152024, all 375 marked UI pixels in station rectangle
`(2000,1430)..(2520,1800)` still have temporal depth **1.0**. Their original
scene depths are `1.5638e-6..1.6574e-6`. Thus the previous flight-HUD
correction did not remove this input error. The old mask identifies UI
coverage, not the shader family that wrote it; it could not by itself
attribute these pixels to the flight HUD.

The installed sprite vertex shader `E508648660A352B2` provides the missing
evidence. Instructions 83–85 calculate clip W, write `abs(W)` to clip Z,
and write W unchanged. Visible sprites therefore rasterize at depth one.
The captured sprite draws disable depth testing. Our coverage pass reused
the ordinary screen-composite shader, enabled depth writes, and thereby
turned this intentional draw-order device into a false physical surface.
The neighbouring 3 × 3 motion-depth search spread that error around strokes.

The draw ledger independently places both sprite records (323 and 324)
at projected input position approximately `(2118.5,1598.5)`, view distance
16,587.594 m. They lie over the target. The 22 drawn holo-panel records
instead lie approximately 0.53–1.96 m from the view in this snapshot.
This distinguishes the sprite contract from the ordinary flight-HUD
coverage shader corrected in the previous build.

The dedicated sprite coverage shader retains its sampled-alpha threshold
and reconstructs physical scene depth as `A + B / SV_Position.w`.
D3D11's pixel-stage W behavior is exercised by the GPU regression using
the sprite vertex shader's forced-Z contract at both one metre and the
captured 16.6 km distance. The result is merged with original scene depth
using nearer-wins: a station in front retains its correct depth while the
visible overlay remains marked, matching the sprite draw's disabled depth
test. Over the sky, the marker gets its actual distance. The original game
depth is untouched, and the caller's PS resource slot 2 is restored.

## Validation and limits

Focused WARP tests pass 8,247 checks, including the new sprite cases in
native and trained modes, zero fixed reactivity, sky and nearer geometry,
transparent fringes, caller state restoration, and R32, D24 and D32/S8
depth formats. The existing differential tests against the installed
flight-HUD pixel shader and adaptive UI tests also pass. Pure temporal
tests cover opposing turns, recovery with a still head, sparse menus and
auxiliary-camera recovery.

Build `6AA3235C` passed the full build/configuration checks, the same 8,247
UI checks on the hardware driver, and the NVIDIA DLL smoke/conventions
test. The capture probe passed 96 eye submissions across paired DLSS,
AA-off and bounded AA-off modes. Saved images, binary inputs and CSVs
were verified, including the new camera-decision fields.

Both DLLs were installed with the game closed and destination hashes
verified. Rollback copies and the exact manifest are in
`build/review_motion/station1520/install/plan.json`. The installed INI and
Frontier graphics XML hashes are unchanged.

The fix changes motion selection and an incorrect depth input. It does
not increase sharpening, render resolution or UI reactivity. UI still
passes through temporal reconstruction; independent post-AA UI rendering
has not been implemented. The user subsequently confirmed the new build
looks perfect, following the ship-yaw and chevron report. The follow-up
[DLSS performance review](review-dlss-performance-2026-09-10.md) uses this
confirmed appearance as its baseline.
