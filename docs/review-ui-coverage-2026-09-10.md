# Chevron coverage and DLSS detail, 14:16 capture

The user still sees shimmer/blur on the station beneath the green targeting
chevrons after build `6AA30E78`, and reports sharper station textures with
AA off. They authorized prioritizing detail and testing a higher render
scale, while correctly distinguishing that experiment from a fix for
detail lost during temporal reconstruction.

## What the latest run establishes

`edvr_gfx_20260910_141359.log` and `eye_141613` show DLSS receiving
2862 × 2826 input and producing 4404 × 4348 output. The 32 motion records
cover both eyes for sixteen frames, 12136–12151. Body motion and camera
rows remain valid, NVIDIA history remains continuous, and there is no
origin jump or requested history reset. Body-fit residuals range from
0.007969 to 0.008187 metres. These checks do not establish correct motion
at every station pixel.

The automatic texture LOD correction is already active at approximately
−0.62 for the 0.65 render fraction. The game has its own AA disabled
(`AAMode=0`); blur, depth of field and bloom are also disabled. Neither a
missing mip correction nor a second game AA pass explains this run.

The user switched EDVR AA off at 14:16:59 and requested `eye_141704`.
That request saved the draw ledger but no eye images: image capture was
inside the temporal pass and never ran with AA off. Consequently there
is no saved AA-off image from that request to compare quantitatively.
The user's visual observation remains relevant; it is not contradicted
by the incomplete capture.

Analysis material is under `build/review_motion/station1416`, including
`station-detail.png`, the copied log, motion CSV and shader disassemblies.

## A reproduced coverage error beneath the chevrons

The flight HUD pixel shader `8DEF46452FA459F5` computes stroke opacity
by marching a ray through a capsule and sampling three octaves of value
noise. The previous private coverage shader approximated the capsule's
core geometrically. A geometric core can still have zero or very low
opacity after the game's noise and density calculation.

That false coverage gives visible station pixels UI motion classification
and feeds them into adaptive UI history decisions. It can therefore affect
the background underneath and around a chevron even when fixed UI
reactivity is zero. This is a concrete mechanism for the reported local
artifact, though its visual removal still requires a flight check.

`src/d3d11/ui_depth.cpp` now follows the captured shader's opacity march:
live noise texture and samplers, ray interval, density, fade, and the
integer bit representation of its step count. Only pixels whose resulting
opacity reaches the existing 0.7 coverage floor can own UI motion. The
linear-distance occlusion test and device-depth conversion from the prior
fix remain. Original game colour draws and live scene depth are preserved.

Differential WARP tests execute the original Steam shader bytecode as an
independent reference. Among the tested geometric cores, actual alpha is
0.000000 for zero density/noise, 0.066209 for low density, 0.285815 for
low noise, and 0.995367 for the opaque reference. Coverage now agrees with
the original shader's opacity threshold in both native and trained modes.
These are synthetic input cases checked against the real shader; they
are not alpha measurements extracted from the flight screenshot.

## Smoke and UI evidence have separate identities

The shared R8 mask previously used odd/even values to select camera/body
motion. Smoke also used an odd marker and could be mistaken for UI by
the adaptive evidence pass.

The low two bits now encode floating UI (1), attached UI (2), and smoke
(3). The upper six bits encode optional fixed bias. Floating UI and smoke
retain the camera path; attached UI can follow body motion. Only classes
1 and 2 enter adaptive UI evidence. Smoke strength is decoded separately
from the optional fixed UI strength. The private-depth smoke-hole fix is
preserved, and zero fixed UI bias continues to retain motion classification.

## Captures now cover the actual comparison

AA-off submissions reach a copy-only capture entry point through the
OpenVR proxy. They save sixteen C frames, left/right overviews and a
`_capture.csv` identifying the mode and dimensions. Capturing does not
apply AA or change the submitted texture.

Full-frame trained captures also save their first left-eye input motion
vectors (`_MV.bin`), reconstructed device depth (`_Z.bin`), UI coverage
when available (`_UI.bin`), computed bias mask (`_Bias.bin`), and source
scene depth when available (`_SceneZ.bin`). The first-frame metadata records
whether UI was bound and which evidence flags were active. These files
allow a future capture to distinguish station motion errors, UI ownership
errors and excessive history rejection. They are not retrospective data
for the 14:16 flight. This input capture is currently in the full-frame
trained path, not the foveated or native TAA paths.

`tools/eye_inputs.py` reads the tightly packed `EDVRTEX1` binary format;
its module docstring defines the 44-byte header, units and mask classes.
Scene-frame IDs align the inputs with C00 in the paired motion CSV.

## Quality experiment and remaining limits

The installed test restores `fix.temporal_aa=dlss` after the user's AA-off
comparison and changes the game's `HMDRenderTargetMultiplier` from
0.650000 to 0.800000. That requests approximately 52% more input pixels
at the same headset output target. Actual dimensions and GPU timing must
be checked in the next flight. All other saved settings are preserved;
automatic texture LOD correction follows the new scale.

Higher input resolution may preserve finer structure, but does not repair
wrong reprojection or explain why an AA-off image appears sharper. No
additional sharpening or speculative station-motion adjustment is applied.
The new input maps are needed to investigate remaining softness away
from the corrected chevron mask.

This is still a temporal treatment of the game's composited image, not an
independently rendered UI layer. Transparent glyphs share pixels with the
station, and stock HUD noise remains present. Correct coverage removes a
demonstrated false-ownership error; it does not establish that all UI
shimmer or station blur is solved.

## Validation and deployment

Build `6AA316A0` passes the full build and configuration contract
(247 keys), including 5,664 private UI-depth checks and 3,895 drive-switch
checks. The optional differential suite against the Steam HUD shader
passes 7,276 checks. Proprietary shader bytecode remains outside tracked
source.

The NVIDIA hardware smoke suite passes, including its motion/jitter
convention comparison. A separate capture probe exercises 96 eye
submissions: paired DLSS, AA off, and bounded AA-off textures. It verifies
caller compute SRV/UAV state preservation. Python then checks every saved
source frame's exact colours, all image dimensions, both AA-off overviews,
the CSV rows, and the saved motion/depth/bias binary inputs. This proves
capture plumbing on synthetic hardware inputs, not headset image quality.

The installation plan and rollback copies for both DLLs, `edvr.ini`, and
the Frontier graphics XML are in `build/review_motion/station1416/install`.
Installation requires the game to be closed and verifies source, destination
and backup hashes before any replacement, with rollback on failure.
Build `6AA316A0` was installed with the game closed; all four destination
hashes matched the prepared plan. Headset validation is still outstanding.

Next flight: inspect the station beneath the green chevrons, then take a
DLSS dump and an AA-off dump from a similar distance and orientation.
Keep render scale the same between those two captures so the AA comparison
is useful. Any subsequent 65% versus 80% comparison should also use the
same AA mode and comparable scene motion.
