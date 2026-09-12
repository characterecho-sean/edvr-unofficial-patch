# Temporal reconstruction under the raw-only culling probe

The reported head-movement shimmer and blur persisted after Frontier was
restored from the render-to-submit timing checkpoint to `cc3d882`. Both logs
and DLL hashes verified the rollback; current INI bytes still matched the
earlier run. Geometry and text were affected, and switching AA off made the
moving image clearer.

The capture environment is OpenVR on Pimax through SteamVR, DLSS Quality with
preset K and runtime file version 310.7.0.0, at 2774x2740 input and 4268x4216
output per eye. The probe bug depends on an asymmetric projection widened only
through the raw channel; a symmetric projection or ordinary both-channel guard
does not exhibit this particular disagreement.

Ruled out: the new outer GPU instrument as a necessary cause, because the
verified baseline reproduces the symptom without that implementation.

## Captured cause

The two requested captures show the main-menu ship, rather than a cockpit. They
contain a nearly still sequence and a slow head turn, with sixteen paired
raw/processed frames each, motion metadata for both eyes, and the first frame's
motion/depth/UI inputs. They are useful evidence for the reported geometry and
text problem; they do not qualify other scenes.

History remains valid in both sequences, with no reset requests or detected
camera jumps. Head and camera rotations agree closely when measured with a
small-angle calculation that avoids trace/acos error on float matrices. The
scene depth is populated, and the recorded motion texture agrees with a CPU
reconstruction using the captured shader parameters to within expected numeric
precision on most pixels.

The VR log identifies an active `advanced.cull_guard_channel = raw` probe. This
deliberately widens `GetProjectionRaw` alone, leaving `GetProjectionMatrix`,
target sizes and submission cropping unchanged. However,
`systemHookEffectiveTangents` selected widened tangents whenever the guard was
live, without considering the selected channel. Both temporal reprojection and
the conversion of pixel jitter to projection offsets consume that function.

Consequently, the game renders through its original asymmetric projection while
the temporal pass reconstructs through a symmetric one. The same wrong span
increases horizontal raster jitter by about 19 percent while the pass and DLSS
are told the original pixel offset. The captured input motion texture
faithfully contains the wrong reconstruction; this is not a missing shader or a
timer suppressing the pass.

## Independent image check

Consecutive raw frames were fitted against the captured camera/depth warp,
using blurred luminance and a residual translation fit on separate hull, floor
and text regions. The alternative uses the original projection reported by the
runtime and accounts for the larger jitter that the existing bug actually
injected. It does not assume that the corrected jitter was already rendered.

Residual RMS in input pixels during the moving sequence:

| Region | Captured projection, X/Y | Original projection and actual jitter, X/Y |
| --- | --- | --- |
| Hull | 0.5305 / 0.1168 | 0.0242 / 0.0029 |
| Floor | 0.2435 / 0.4416 | 0.0070 / 0.0033 |
| Text | 0.1140 / 0.3172 | 0.1476 / 0.0228 |

The still sequence also agrees with the original projection and enlarged
jitter. A one-frame jitter lag or reversed sign fits substantially worse. Ruled
out for these captures: repeated history resets and a one-frame jitter lag as
the cause of the measured projection mismatch.

This fit uses first-frame depth for the short sequence; it is not a complete
per-frame optical-flow ground truth. Text retains a horizontal residual and may
involve additional UI-specific motion. The strong agreement on two separate
geometry regions, the actual matrix/raw split, and the shader's captured inputs
establish the projection bug without claiming every remaining artifact has the
same cause. Full capture files and analysis stay local.

## Correction and next gate

Temporal consumers must use the unjittered projection actually supplied by the
matrix path: true tangents for the raw-only probe, widened tangents for a live
matrix or both-channel guard. Both motion reconstruction and pixel-jitter
conversion then use the same projection as the rasterizer. The diagnostic raw
answer, its independent matrix counterpart, and its no-crop/no-resize behavior
retain their existing meaning.

The regression fixture compares the temporal tangents with the actual hooked
matrix for both eyes, using asymmetric horizontal and vertical projections. It
covers the raw-only and matrix-only probes and the ordinary guard before and
after activation, preserving their existing raw/matrix and image-path checks. A
read-only export exposes the same helper used by the temporal consumers;
expected values are derived independently from the matrix received through the
historical OpenVR interface.

The full paired build passed, including the extended actual-proxy projection
checks, existing ABI and rendering tests, the shared/frame timing suite and the
config/Python gates. The production change is the projection selection; no
shader math, AA preset, sharpening setting or live INI was adjusted.

The next headset gate repeats the same head movement on the corrected build,
then checks cockpit and on-foot rendering. Visual recovery is not yet verified.
