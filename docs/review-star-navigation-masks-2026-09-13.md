# Navigation masks against stellar glow: issue 36

[Issue
36](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/36)
reports visible masks around navigation targets and orbital lines near a
star. Both attached screenshots show broad, sharply bounded regions of
reduced glow around the navigation graphics. The most useful examples
are below the star's bright disc, in its surrounding glow. This is a
spatial brightness discontinuity, not sufficient evidence of temporal
ghosting from two still images.

At investigation time the report has two screenshots and no comments,
logs, paired raw/treated eye captures, EDVR version, AA mode, runtime or
headset details. Do not attribute the reporter's configuration or build
from the preceding local hangar flight. The code reviewed is d3e3faf;
the subsequently fetched main changes are outside these rendering paths.

Follow-up from the user: the reporter confirms that the mask disappears
with EDVR AA Off. Whether the enabled mode is DLSS, native TAA or both
is not yet confirmed. This result was relayed in the working thread; the
public issue still has no additional comments or capture attachments.

Ruled out: AA-independent stellar-glow masking alone explains the
reported defect, because the reporter's AA-Off comparison removes it.
The existence of the captured alpha-masking shader below is therefore
not sufficient reason to change the star-glare implementation.

AA Off disables both temporal reconstruction and its private UI
coverage: `uiDepthConfigure` sets `g_on = g_keyOn && g_passOn`.
Consequently this comparison still permits either a reconstruction
artifact or an earlier AA-enabled draw/state interaction. It does not
establish a DLSS-model fault or a post-DLSS-resolve fault. Native TAA
has no post-DLSS resolve; a confirmed native-TAA reproduction would
exclude that specific pass.

## Candidates and distinguishing evidence

1. Game/UI compositing or stellar-glow masking before temporal AA. A
   paired raw eye crop would already contain the dark region.
   Reproducing it with EDVR AA Off excludes the temporal pass. Comparing
   Sun glare Stock with the reporter's current mode then isolates EDVR's
   glare replacement from the remaining game/mod compositing paths.
   Persistence in Stock alone does not establish a vanilla-game bug:
   other fixes are still active.
2. Temporal UI reconstruction. Raw colour is clean but the treated crop
   develops a dark region corresponding to UI coverage, edits or their
   retained influence. The post-DLSS UI resolve bounds current model
   colour against raw pixels; it cannot be blamed merely because the
   screenshot's region looks like a mask. It has no star-specific
   branch.
3. Draw-state leakage affecting a later glow pass. A raw colour change
   caused only by enabling AA could still originate before DLSS if a
   coverage reissue leaves a game binding changed. Compare actual scene
   depth, stencil/bindings and the later glow draw, not just the final
   UI mask. Existing tests argue against the simple live-depth-write
   theory in their covered cases, but cannot rule out an uncaptured
   state.

## Code and shader findings

`ui_depth.cpp` routes recognized orbital, hologram, sprite and HUD
coverage through `UiDepthLayer`. Its depth target is a private copy; the
game's original colour draw precedes the coverage reissue. The orbital
replacement VS is used only for private coverage, then restored. Unknown
coverage declines without modifying the original game draw.

The post-DLSS path binds raw and model colour through the same UNORM
representation. A simple raw-versus-model sRGB view mismatch is not
present in that binding. The influence history can outlive a departed
glyph, so moving-head captures still need inspection if only the treated
image has the defect.

The local game's captured shader `D56F859BE4781431` has a flare-like
signature and contains a concrete alpha-mask operation. When input
TEXCOORD2.y exceeds 0.05, instructions 9--13 sample t0.w at raster
SV_Position.xy multiplied by cb1[332].zw and multiply emitted alpha by
one minus that sample. Thus a screen-space alpha footprint can attenuate
this shader before any temporal AA. Its exact use in the reporter's
frame, texture contents and blend state remain unknown; this is a
candidate mechanism, not a confirmed diagnosis.

Do not confuse that shader with `912477AEF6958379`, the glare PS seen
with the recognized art-sheet pair in the local earlier flight census.
The latter has no screen-alpha masking branch. Signature similarity
alone does not authorize substituting one for the other. Neither shader
nor its disassembly is added to source control.

## Validation and next capture

Fresh builds of the current production-code regressions pass:

- UI coverage/resolve: 21,110 checks, including live scene depth,
  transparent fringes, shader/constant restoration and post-resolve
  alpha.
- Stellar motion: 94 synthetic checks. This invocation has no captured
  orbital replay pairs; it does not reproduce the issue screenshot.

No production rendering change is justified yet. The AA-Off comparison
is complete. The next useful evidence is an AA-enabled paired eye dump
with the mask visible and the corresponding log/settings. If its raw
crop is clean, inspect the temporal UI/depth/motion inputs and final
reconstruction. If the raw crop already has the mask, inspect AA-enabled
draws and their restoration before the later glow/compositing passes. No
new diagnostic build or manual INI edits are needed for that split. Keep
navigation graphics visible during capture so removing the marker is not
mistaken for repairing its compositing.

The existing paired capture's raw/treated crops and UI/depth inputs can
separate these branches. Broad off-centre regions may lie outside the
raw crop, so place the affected marker near the centre of the eye for
the comparison. Raw input copies are more diagnostic than another
treated screenshot alone. No issue comment has been posted, no fix is
claimed, and no install is performed by this investigation.

## Local solar motion captures, 13 September

The later 13:07:14 and 13:10:25 eye runs reproduce solar-surface smear
and angular brightness boundaries beneath a coronal arc. The user did
not reproduce the reporter's navigation mask exactly. Keep these local
findings separate from a confirmed fix for issue 36.

Both captures are from 2aa0c2d, build 6AA6C9BA. The intervening changes
through 694f993 do not touch these rendering paths. The verified flight
uses OpenVR through Valve SteamVR, 90 Hz, preset K, 2774x2740 input and
4268x4216 output per eye. The HMD model and NVIDIA DLL version are not
established by the lines used here. The existing opaque-planet motion
table has 128 shared hologram records; these frames have no planet-mode
records or active terrain records.

The paired C00/T00 crops cover the same angular region at their native
input/output resolutions. The raw sun texture is detailed; the treated
texture streaks. The second treated crop adds large angular brightness
regions in the glow below the limb, absent from its raw crop. Frame IDs
are 30902 and 48033 in the motion traces. Draw snapshot labels are one
frame higher in this established capture convention.

Ruled out for these local examples: the dark regions already exist in
the AA-enabled raw colour. They appear later. This rules against an
original draw-state leak as the sufficient cause of these particular
paired differences, but does not isolate DLSS from the post-DLSS UI
resolve.

The current UI low-bit classes 1/2 and content-edit map do not fill the
broad angular regions. Class 3 covers a band at the stellar limb.
Invalid-history motion appears along some polygon boundaries; ordinary
sky motion remains inside them. Neither observation alone proves which
pass creates the brightness boundary. The retained UI influence and
pre-resolve model colour were not captured by this build.

The original draw census identifies a different surface from the planet
fix: 147456 indices, one instance, stride-12 positions. Its depth pass
uses VS 0EE43D81E394E70C with null PS; colour uses VS 4D516EF05C68FFA5
and PS 147E748F4CD3AE9A. In 13:10:25 the colour draw is original-eye
ordinal 220. It writes depth and blends. It is neither the recognized
71DD9863DCFC0986 planet surface nor a rigid instance-pool mesh, so no
draw-specific motion currently covers it. The saved sun depth is around
1.7e-10 while the generic camera translation is centimetres per frame.
Those facts motivate capturing the actual solar coordinate frame; they
do not justify guessing its transform from a different celestial draw.

The subsequent solar families are D95905C18B7FAD93, 8BD7C37ABCEE7E45,
D1281DF454A153AD, 5E417E9DF2E7F9E6 and 1F3AD1584D7FA3C8. The
5E417E9DF2E7F9E6/BD801F2FB02522EB pair is also the existing private
smoke-coverage family. Its solar use must not be mistaken for proof of
an engine-smoke defect. The other solar bytecode and draw-time b0/b1
were not retained in these snapshots. Auxiliary instance captures keep
b2 and vertex streams, so cannot supply the missing b0/b1.

## Bounded follow-up diagnostics

The diagnostic change adds these seven solar VS families to the explicit
eye-run snapshot. It retains original b0/b1/b2, PS b2, both shader
stages, and original unpacked vertex/index bindings and layout. Geometry
is limited to the first three watched frames, 256 KiB per stream and the
existing 32 MiB total; larger streams are partial, not a full replay
promise. Constants continue throughout the run. Known shader bytes are
retained at creation and written only for a requested dump. The log
reports solar draws, available constants, geometry/layout and missing
shaders, including zero counts when the scene contains no solar draws.

The first DLSS eye also saves `DlssBeforeUi`, `UiPrevious` and `UiNext`
using the existing typed input format. `DlssBeforeUi` is copied after
NGX and before UI bounds; the other two contain incoming/outgoing UI
influence in alpha on the input grid. Invalid incoming history has no
`UiPrevious` file. The header names the current evaluation frame,
matching the existing `PrevZ` convention. Colour decoding preserves the
stored colour space. These copies happen only during an armed eye dump.

One repeat capture with the sun and arc near eye centre while moving can
now distinguish the candidates together:

- Wrong solar motion: project the same original solar vertices through
  consecutive b0 transforms, account for jitter, and compare with the
  supplied motion and raw image registration. Do not extend the planet
  coverage path until the solar VS and depth encoding agree.
- Model/history artifact: the angular boundary is already in
  `DlssBeforeUi`. Inspect scene/private depth and history rejection at
  the boundary with the newly identified draw transforms.
- UI cleanup artifact: `DlssBeforeUi` is clean and the treated image
  acquires the boundary inside the captured influence. Replay those
  exact resolve inputs before changing UI history behavior.

No solar rendering correction is claimed by these diagnostics. They
avoid a speculative coverage, depth, opacity or sharpening change.

Validation: the full absolute-path build and NVIDIA hardware DLL smoke
test pass. The GPU snapshot fixture captures all seven solar families
over four frames, verifies three-frame geometry limits, retained
draw-time constants after source-buffer reuse, both shader-stage files,
and explicit reporting for an unexpected missing pixel shader. The
typed-image reader tests the added RGBA formats without changing gamma.
These are diagnostic integrity checks, not a replay of the visual fault.

## Solar transform comparison, capture 134146

The next flight verifies diagnostic build 7f6582b (graphics build
6AA6FA5C, VR build 6AA6FA64). The capture uses SteamVR/OpenVR, preset K,
2774x2740 input and 4268x4216 output at 90 Hz. The HMD model and DLSS
DLL version have not been established from this evidence. Required solar
colour b0 transforms and both shader stages are present. The snapshot
reports 38 aggregate failures; this count is not evidence that the
required solar transforms are missing, nor proof of a particular failed
optional capture.

Snapshot frame 15037 corresponds to motion CSV frame 15036/C00, and
15038 to 15037/C01. Projecting the same local positions through the
colour draw's consecutive b0 rows 4, 5 and 7 gives a different motion
from the generic camera transform. Row 6 is `[0, 0, 0, 0.02500000037]`,
agreeing with the temporal projection B. The comparison adds the
current-minus-previous jitter to raw projected displacement before
comparing unjittered motion. A relative SceneZ gate rejects back faces
and nearer cockpit surfaces; an absolute tolerance would be invalid at
these depths, around 2e-10.

The raw crop starts at input pixel (687,670), with size 1400x1400.
Whole-eye vertices outside that crop are not examples of raw texture
registration. Projected motion establishes the geometric correction; it
does not by itself measure the sun's animated material shading.

At a 0.5% relative depth gate, 1,725 visible candidate vertices differ
from generic camera motion by a median 1.58 input pixels and a 95th
percentile 4.07 pixels. Within the actual raw crop, excluding UI mask
kinds 1/2, 1,175 candidates give 1.36 and 3.14 pixels respectively.
Tightening the depth gate to 0.1% still gives a whole-eye median 1.84
pixels. The discrepancy is not created by a loose far-depth gate.

The existing GPU affine-record builder reproduces eight captured point
projections across the first frame pair to below 0.00006 input pixels.
These fixture inputs are current clip X/Y/W with expected previous clip
X/Y/W, not local XYZ or clip X/Y/Z. The test validates the GPU transform
calculation; it is not a DLSS visual replay.

A local image check provides independent support. The textured patch at
C00 crop (480,80), size 280x280, gives 34 depth-consistent vertices.
Their median raw draw displacement is (1.146,0.973) pixels, while raw
camera prediction is (-0.369,-1.054). After removing broad illumination
with a Gaussian high-pass, bilinear normalized correlation is 0.96684 at
the draw prediction and 0.75493 at the camera prediction. The best
quarter-pixel search gives (1.25,1.25), correlation 0.97239. Local
transform variation and animated solar shading prevent a claim of exact
subpixel texture registration, but the comparison favors the draw
transform. Raw draw displacement already includes jitter; only the
unjittered camera prediction needs the jitter difference removed.

The solar colour PS's sole discard samples a full-eye linear depth
resolve at t0 and rejects samples closer than the solar fragment's clip
W. That resource is a per-eye R32 texture, not an opacity artwork. The
stable solar artwork is t1, shared by the two eyes in the census. The
final alpha comes from scene b1[126].z. The colour pass blends and
writes depth, with no PS depth export.

The depth prepass uses the same local-to-clip rows but subtracts 0.001
from clip Z. Therefore the original colour draw writes B/W while
discarded samples retain (B-0.001)/W. Exact EQUAL coverage at the colour
depth can distinguish them without retaining or resampling the linear
depth texture. Reissuing the shifted prepass would not provide the same
visibility. Any blended-surface extension must remain restricted to the
captured solar colour VS/PS pair; this is not a general exception for
transparent objects.

The new view does not show an unambiguous broad coronal loop and angular
mask comparable to 131025. Comparing DlssBeforeUi with treated colour
shows sparse UI cleanup differences, not evidence that this flight
isolates the original mask. Ruled out as a justified change from this
capture: adjusting UI cleanup or globally relaxing history rejection
based on broad luma-edge correlations. The earlier angular mask and the
reporter's navigation mask remain separate unresolved observations.

## Solar surface correction

The exact solar colour pair now shares affine motion mode 4 with opaque
planets. Its history identity uses solar artwork at t1; planets keep t0.
Changing the solar depth resolve cannot break identity, while changing
the artwork or mesh rejects history. Animated material constants remain
outside geometry identity. The blend exception applies only after the
caller verifies the solar VS/PS pair. Coverage reuses the original
colour VS and exact EQUAL scene depth, without game colour or depth
writes. The existing temporal consumer preserves text motion and rejects
coverage hidden by later cockpit/private depth.

This adds one coverage reissue and one affine-record dispatch for each
eligible solar surface draw, using the existing 128-record eye budget.
There is no new full-screen pass or normal-flight CPU readback. Shader
identity lookup is restricted to the two candidate vertex families. AA
Off disables this path through the existing temporal-pass gate. The
first accepted solar draw logs `solar motion: exact surface coverage and
affine approach motion active`; an absent line means acceptance has not
been demonstrated. The unchanged eye-run coverage and record files can
verify per-pixel use in the next flight.

Validation: the absolute-path full build, all its regression gates and
NVIDIA DLL smoke pass. The UI coverage suite passes 21,421 checks on
WARP and 23,167 on NVIDIA with the captured flight-HUD reference shader.
The added solar case checks every pixel across shifted-prepass-only,
alpha-colour-surviving and nearer-cockpit zones at clip W=1e8. It also
checks exact preservation of all scene colour channels and depth,
coverage depth, and restoration of shader/constants/targets/depth/blend
state. The opaque planet regression remains in place. Stellar motion
passes 140 synthetic checks, including independent t0/t1 identity tests,
and 277 checks with the eight captured solar point pairs.

These tests establish geometry motion and visibility, not a confirmed
in-headset resolution of the solar smear or the separate coronal mask.

## Orbital halo follow-up, capture 142914

The user describes the orbital artifact as a dark band or halo. Both
flight DLLs verify against f9787bf: graphics build 6AA7055D and VR build
6AA70564. This run again uses SteamVR/OpenVR, preset K, 2774x2740 input
and 4268x4216 output. Solar and orbital motion both log activation. The
snapshot reports 844 draws, no failed copies and no missing shaders.
Temporal frame 14380 and snapshot frame 14381 name the same captured
frame under the existing counter offset; snapshot 14382 is the next
frame, not the predecessor of the first saved motion image.

The useful glow region is C00-local (900,530)-(1200,860), or input
(1587,1200)-(1887,1530). The raw crop begins at (687,670); the treated
crop begins at output (1056,1030). Compare on the output grid using
input = (output+0.5)*inputSize/outputSize-0.5+jitter, with first-frame
jitter (-0.25,0.166666687). Native crop coordinates and full-eye
coordinates are not interchangeable.

Ruled out for this captured boundary: the final UI cleanup introduces
the broad glow discontinuity. It already appears in DlssBeforeUi. The
mean absolute RGB difference between that output and T00 in this region
is 0.00110 in normalized byte colour. The raw-to-model difference is
larger, 0.00933. The signed difference is mixed: much of the model glow
is brighter, with narrow darker regions and sharp changes in brightness
near the private coverage boundary. A threshold selecting only negative
RGB differences misses much of the visible discontinuity.

The class-3 coverage boundary and invalid-history vectors follow the
stellar limb and orbital hairline. Spatial coincidence is insufficient
to blame background-history rejection. A preset-K NVIDIA replay of a
static 600x400 captured crop, repeated for 32 frames, compares zero
motion with the captured invalid-vector positions. The final mean
absolute difference is only 0.01967 byte with merged depth and 0.02143
byte with original scene depth. Repeating the captured physical vectors
instead is a deliberate inconsistent-motion stress test: replacing only
invalid vectors changes the final output by 0.1675 byte on average,
without reproducing or removing an obvious broad angular mask. A
separate full-size synthetic gradient/line experiment also produces
sparse differences rather than a broad halo. These controls do not
reconstruct the flight's earlier DLSS history. They do not justify
disabling the protection that fixed the ship-tip trails.

The solar motion consumer is active on the sun itself. All 1,402,275
solar coverage samples agree with original SceneZ; 1,402,190 also agree
with merged Z at the consumer's relative tolerance. The remaining 85
must be rejected when nearer private coverage wins. Far-depth checks
must use relative error: an absolute tolerance such as 1e-6 would accept
zero depth as matching these approximately 1e-10 surfaces.

There is a separate, measurable corona motion error. Its original
5E417E9DF2E7F9E6/BD801F2FB02522EB draw uses POSITION at byte 0 and
TEXCOORD.xy at byte 36 of a 44-byte vertex. TEXCOORD.y is float 10, not
float 9. Width comes from VS b2[0].w and b2[1].xy; adjusted local
position is projected through b0 rows 4, 5 and 7. The original raster
adds 15.01 to clip Z, while the existing private coverage recovers its
depth from TEXCOORD1.z. That offset is not physical view depth.

Perspective-correct triangle correspondence gives 21 visible corona
samples in the affected band after requiring class 3, zero original
SceneZ and relative agreement within 1e-4 between private Z and B/W.
Their input positions span (1587.5,1224.9)-(1873.2,1284.9). Comparing
the next frame's actual draw projection against that frame's generic
camera projection at the same W, with its jitter removed, gives median
raw backward motion (-1.2805,0.2430) versus (-1.0174,-0.3643). The
95th-percentile vector error is 0.6708 input pixels. This is a
draw-motion error, not proof that correcting it alone resolves the whole
navigation halo.

The captured 6,776-byte VB and 2,400-byte IB are identical over the
first three watched frames. The width fields remain
(287916768,0.083159015,0.34999999); shading fields change. This proves
stability for these frames, not immutable D3D11 usage or stability of
every other asset using the same shader. A production motion extension
must reject geometry writes and width changes, preserve existing solar
coverage behind transparent fragments, and keep ordinary UI motion.

## Corona motion implementation

Mode 5 follows the exact streak family's original affine clip rows. Its
history key includes the three width fields, the t1 artwork, draw range,
and VB/IB write generations. It requires the recorded POSITION and
TEXCOORD layout at stride 44 and a single indexed instance. Unsupported
pipeline stages, predication and geometry that can be changed through
GPU writes retain the existing motion path. This is a shader-family
contract, not an identification of every streak as a solar asset.

The existing private smoke coverage draw supplies the motion coverage;
there is no additional raster draw or full-screen pass. A separate
pixel-shader variant reads the original scene depth through a cached
view. Only visible core samples replace the HoloCoverage record. Farther
corona and fringe samples preserve the previous coverage, including the
sun's record. The legacy smoke shader remains the fallback. The temporal
consumer requires class 3 and exact agreement with the final merged
depth, so later foreground depth still rejects the corona record.

HoloCoverage remains R32G32_FLOAT. A reviewed 8x8 independent-MRT test
passes on WARP and NVIDIA: a float4 shader output carries visibility in
its fourth component, and source-alpha blending preserves or replaces
the two stored components exactly. The R8 coverage target remains
opaque. An earlier failing scratch test put visibility in blue and zero
in alpha; that result is invalid and is not evidence against RG32
blending.

The captured fixture contains 21 independent previous/current pairs, 600
indices per draw, and exactly 366,412 bytes in the existing stellar test
format. A strict parser verifies payload sizes and end of file. An
independent double-precision affine inverse applied to its serialized
float32 clip points differs from the serialized expected projections by
at most 0.0000241 input pixels at 2774x2740. This is a fixture check;
the production GPU transform and coverage tests must pass separately.

Validation completed: the absolute-path full build and NVIDIA DLL smoke
test pass. Stellar motion passes 889 synthetic checks and 1,247 with all
21 captured corona pairs; the GPU-generated mapping's maximum error is
0.000047 input pixels. Width-field changes, known and unknown geometry
writes, empty writes, shading-only changes and the shared record limit
are covered. The eye-dump reader names mode 5 `affine streak`; an
accepted draw logs `corona motion: mode-5 affine streak history active`.

The production UI coverage suite passes 22,798 checks on WARP and 24,544
on NVIDIA with the captured HUD reference shader. Core and nonzero-mask
fringe cases run through the actual reissue and optional pixel shader.
They verify exact preservation of earlier sun coverage, replacement by
visible corona, all four original colour channels, original scene and
private UI depth, legacy smoke depth/classification, and restoration of
PS, t2, b12/b13 and output state. The debug queue is checked after both
solar and corona coverage tests. The screen suite passes 55,173 checks,
including class/depth/history rejection and the distinct corona vector
through both shipping MV variants and the source-compiled shader.

These checks establish the corrected geometry motion and preserved
visibility. They do not establish that the entire reported orbital halo
has disappeared in the headset. No background-history rejection, final
UI cleanup, DLSS preset, sharpening or live configuration was changed.

## Continued orbital halo, capture 160340

Ruled out: correcting the corona's affine geometry motion alone resolves
the orbital halo, because the user still sees it on verified 4d7307d.
The graphics log edvr_gfx_20260913_160138.log identifies build 6AA71BFC,
linked 21:56:12 UTC, and logs accepted mode-5 motion at 16:03:08.280.
The 160340 capture again contains 844 draw snapshots without declines.
Input and output remain 2774x2740 and 4268x4216. The first temporal
frame is 10640; raw crop origin is (687,670), output crop origin
(1056,1030), and first jitter is (0,-0.166666657).

Candidate causes and discriminating evidence: an unaccepted mode-5
record would show missing/mismatched HoloCoverage against merged depth;
orbital synthetic depth affecting DLSS would appear in DlssBeforeUi and
respond to a controlled depth/motion replay; final UI bounds affecting
the surrounding corona would appear in T00 minus DlssBeforeUi together
with retained UiPrevious/UiNext influence. The earlier 142914 ruling
about its broad glow boundary must not substitute for this comparison.

This capture isolates a final-resolve contribution. In raw-local
(900,200)-(1040,650), the aligned raw/model/final comparison shows a
sharp darker wedge beside the intersecting orange orbital lines only
after UI cleanup. The mean absolute final-minus-model difference is
0.37668 byte per colour channel, with 19.11% of output pixels differing
by more than 1.1 bytes in at least one channel. The saved UI mark is a
thin class-1 line, while UiPrevious contains a much wider retained
footprint on its sides. No content-edit pixels occur in this region.

Orbital coverage currently returns class 1, the same category that seeds
the text cleanup's current bounds and 32-frame retained influence. That
causes a moving geometric line to retain a text-cleanup footprint on the
corona. The model's smoother glow can then be clamped to the current raw
colour within that footprint, producing a visible boundary. The intended
correction is to keep the orbital record and private depth while
excluding this geometric family from text-cleanup classification.
Cockpit text, sprites and other UI keep their existing protection.

The new corona path is independently verified in this capture. All
69,704 pixels of mode-5 record 2 have class 3 and agree with final
merged Z at the consumer's relative 1e-6 tolerance. After subtracting
the production eight-phase Halton wrap delta (7/16,-5/9), captured MV
agrees with its record projection to p95 0.000711 input pixels and
maximum 0.001226. Matched orbital records 3--6 also agree after
requiring the same final-depth predicate. Comparing every stored
coverage sample without that predicate incorrectly includes occluded
fragments.

The fifth orbital instance, record 7, is unmatched. Its rotation, scale
and width change in subsequent captured snapshots, unlike the first four
orbital instances. This is a separate history-identity limitation; it
does not explain the cleanup wedge beside matched orbital lines.

The correction changes only the orbital coverage classification and its
corresponding temporal acceptance gate. Orbital coverage writes class 0
while retaining the same private depth and motion record. Mode 2 accepts
a matched record at exact merged depth without a text mark, and rejects
later class-1/2 text. The original colour draw, background-history
rejection and UI cleanup shader are unchanged. There is no added
texture, full-screen pass, coverage draw or readback.

Validation: the absolute-path full build and NVIDIA DLL smoke pass. The
UI suite passes 24,474 checks on WARP and 26,220 on NVIDIA with the
captured HUD reference shader. The real orbital reissue retains both
instance IDs and exact HC depth, leaves game depth unchanged, and writes
no UI classification. Text kinds 1/2 still reject stale glyphs; repeated
history feedback with orbital classification leaves model colour and an
empty influence history intact. The screen suite passes 55,253 checks,
including accepted mode-2 motion with no text mark through both
regenerated shipping MV variants, text precedence, depth mismatch and
unmatched-record rejection. Existing corona, foreground-occlusion and
source-screen tests remain in place.

This establishes and corrects the final-UI contribution captured here.
Whether any separate pre-resolve halo remains requires the user's next
headset comparison.

## Targeting text, capture 170644

Ruled out: excluding orbital geometry from text cleanup also resolves
target-label masks, because the user reproduced the targeting-text case
on verified 91e4094. Genuine target text remains class 1 and needs its
own protection against stale digits and letters.

The graphics log edvr_gfx_20260913_170403.log identifies build 6AA7236D,
linked 22:27:57 UTC. The VR log identifies Valve SteamVR. DLSS uses
preset K, 2774x2740 input and 4268x4216 output. First temporal frame
13266 has jitter (-0.375,-0.0555555522); the raw and output crop origins
are (687,670) and (1056,1030). There are 1,034 draw snapshots and 190
failed copies. Those failures limit source-draw reconstruction, while
the saved model output, motion, UI coverage, edit mask and both UI
history textures permit an independent analysis of the final resolve.

The same-frame DlssBeforeUi and T00 images isolate the final cleanup:
the former contains overlapping old/current distance digits; the latter
cleans those digits but adds a dark footprint beside and below the
reticle. Thus disabling cleanup globally would trade the mask for a
known text regression. Current colour bounds constrain the entire RGB
sample, including the corona behind transparent text. Retained influence
extends that constraint beyond the current glyphs. A difference between
the model's reconstructed glow and the raw glow can therefore be treated
as a stale letter even where no letter remains.

Candidate corrections require separate gates. A history-rejection change
must remove an obsolete glyph in actual preset-K evaluation, including
moving and changing text, without creating the dark footprint. A
text-layer solution must retain original alpha, blending, occlusion and
HDR/postprocessing behaviour; a guessed colour or a full-quad overlay
does not establish those properties. Merely releasing retained cleanup
is insufficient unless the model's old text is also removed. No new
rendering change has passed these gates yet.

The same-frame resource headers all identify frame 13266, eye 0. A CPU
replay of the shipping resolver reproduces the target ROI with RGB mean
absolute error 0.000691 byte per channel. Output ownership must be
floor(outputIndex * inputSize / outputSize), matching the shader's
integer ceil partitions. Using pixel-centre reconstruction coordinates
for that ownership was an incorrect earlier scratch implementation.
Retained-only influence covers 10.26% of this output ROI and decreases
red by 5.36 bytes on average; current-only influence decreases it by
2.03 bytes. Actual content edits cover only 0.44% and increase red by
3.06 bytes. This establishes the retained RGB constraint as the main
source of the darkening; it does not establish a safe replacement.

The retained mask's transport also has a conservative-growth limit:
maximizing over floor/ceil history samples can spread support by an
additional pixel per axis per frame for fractional motion, independently
of the fractional displacement's size. Zero motion samples one pixel and
does not cause that expansion. This dump has only one pair of UI history
resources, so its exact multi-frame growth cannot be replayed from the
sixteen colour crops alone. Adding an ownership bit would not resolve
the colour ambiguity: this footprint already originates from genuine
text, but its departing pixels now contain the background.

The missing source texture has a concrete diagnostic cause. The original
E508648660A352B2 / 63ABD86359B57D01 draw reads a 2613x2286 RGBA8
texture, 23,893,272 bytes, exceeding the ordinary 16 MiB snapshot limit.
All 190 attempts to save that texture are counted as failed copies;
these are not 190 failures to copy its constant buffers or geometry. Its
original blend is ONE / INV_SRC_ALPHA into the R11G11B10_FLOAT eye
target.

The diagnostic correction grants only that exact sprite shader pair a 32
MiB per-texture allowance and keeps the normal 64 MiB total limit. The
six existing captured textures plus this missing texture require
37,579,888 bytes, within that total. Deduplication still copies a source
once per requested dump. The installed DLSS library reports file version
310.7.0.0; its reserved transparency-mask parameter is not a supported
substitute for separating the text from its HDR background.

The diagnostic change permits source RGB/alpha inspection. It does not
by itself provide an exact replay of the final HDR composite: the saved
constant buffers include VS b1 and PS b2, while the sprite pixel shader
also reads PS b1. Their identity must be checked before substituting the
vertex-stage buffer for the pixel-stage buffer.

An invalid-motion experiment has not passed the replacement gate. The
first synthetic fixture used the wrong motion sign, and its approximate
UI simulator had grid/history errors; those numeric comparisons are
discarded. A separate C00--C15 cropped NGX sensitivity test reuses C00
depth and motion after the first frame and therefore cannot establish
production ghost removal or compare exact final image quality. It shows
that invalidating a rectangle over the label also changes the
surrounding glow, which is insufficient evidence to replace the current
resolver. No motion invalidation, text bounds or rendering settings are
changed by this diagnostic patch.

Validation: the absolute-path full build, all its regression gates and
the NVIDIA DLL smoke test pass. The snapshot fixture accepts the actual
2613x2286 typeless source through its UNORM view, deduplicates repeated
draws, preserves the source binding, accepts two distinct large sources
and refuses a third at the unchanged total budget. Another pixel shader
keeps the ordinary per-texture limit; the exact pair still refuses a
source above 32 MiB. The reader self-test and binary fixture
verification also pass. The next requested dump is needed to inspect the
source alpha that this diagnostic correction makes available; the halo
fix remains open.
