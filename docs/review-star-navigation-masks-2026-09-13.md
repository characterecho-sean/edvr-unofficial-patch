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

## Targeting source captured, 175452 and 175515

The next flight, edvr_gfx_20260913_175245.log, identifies
v0.16.2-8-g79f6934, build 6AA73372, linked 23:36:18 UTC. Both requested
captures now contain seven surfaces, zero failed copies, zero missing
shaders and no capped draws. There are 1,034 and 1,036 draw snapshots,
respectively, including 190 instances of the exact targeting sprite
shader pair in each capture.

Ruled out: an alpha backing across the targeting sprite's empty quad,
because all nonzero source alpha within the sampled UV rectangle lies
inside the label and reticle bounds. The exact draw's VS b2 constants
and packed vertices select source pixels (1741,435)-(2178,871). Within
that rectangle, the nonzero alpha bounds are (1930,603)-(2119,684).
There are 8,903 and 8,893 nonzero-alpha samples. The first label reads
2.06Ls and the second 1.68Ls. Low-alpha antialiasing samples include
some zero RGB, but there is no full-quad alpha noise. These are coloured
RGBA sources; an alpha-only preview is not the original source colour.

The captures use 2774x2740 input and 4268x4216 output. Their first
temporal frames are 8292 and 10307, with jitter (0.125,0.277777791) and
(-0.375,-0.0555555522). All same-frame resource headers agree. An exact
CPU replay of the existing final UI shader reproduces T00 with mean
absolute RGB errors of 0.001261 and 0.001821 byte per channel. The
predicted next-history alpha agrees within R8 quantization.

In the target ROI, retained-only influence outside current expanded UI
coverage occupies 6.44% of the first output crop and 17.53% of the
second. These pixels lose an average 3.185 and 4.155 red-channel bytes
relative to the captured DLSS output. Inactive background is unchanged
exactly. Content-edit pixels are sparse, 0.80% and 1.17%, and their mean
red changes are positive. Thus the larger dark footprint tracks the
retained whole-RGB clamp, not the source's empty quad or the sparse
changing-digit replacement.

This still does not justify turning off stale-text cleanup. The second
capture's model output reads the old 1.69Ls while the current source and
final image read 1.68Ls. A replacement must preserve that correction.
The two captures contain only the first frame's motion and history
resources; the later colour crops cannot establish exact multi-frame
history transport. Scratch transport results require explicit storage
quantization and meaningful on-screen movement controls before being
used as evidence for a rendering change.

The local motion supports testing fractional transport specifically. In
raw ROI (1447,1390)-(1727,1570), excluding the current 3x3-expanded UI
marks, 2,484 of 2,540 retained input cells in 175452 have valid motion;
their median vector is (-0.08032,-0.43286) input pixels. In 175515,
7,988 of 8,029 retained cells have valid motion, with median
(0.36572,-0.30640). Jitter rounds to zero in both cases, as in the
shipping resolver's coverage lookup. The current maximum over four
history taps assigns a full neighbour's remaining lifetime even when its
interpolation weight is tiny. Repeated feedback can therefore move the
cleanup boundary faster than these recorded fractional vectors.

The correction samples the transported remaining influence bilinearly,
then takes the maximum against the unchanged stationary sample. It keeps
the current coverage, fresh content-edit reconstruction,
finite/in-bounds motion gate and 32-frame upper bound. The same four
history taps and existing textures are used; no draw, full-screen pass
or resource is added. Exact integer motion still selects the same
previous cell.

This is deliberately narrower than a separate UI layer. Interpolating
the influence attenuates partially covered transported cells, so their
effective retention can be shorter than that of a full stationary seed.
It does not make the already blended corona and text independently
reconstructable. A remaining halo close to genuine current or departing
glyphs would require further evidence; removing recursive max expansion
does not establish that every visible halo has been eliminated.

The new regression seeds a localized text mark, removes it, and feeds
the GPU-written R8 history back for three frames under tiny positive and
negative axial/diagonal motion. The model deliberately differs from the
raw background, exposing false darkening outside the text footprint.
Running the same test against the original HEAD shader fails at the
first off-glyph retained cell, confirming the test detects the old
transport bug. A separate half-pixel check retains protection for a
partially transported stale glyph. Existing six-pixel/four-pixel motion,
invalid off-screen motion, changed digits, output ownership and alpha
checks remain in the suite.

Validation: the absolute-path full build and NVIDIA DLL smoke test pass.
The UI suite passes 24,662 checks on WARP and 26,408 on NVIDIA with the
captured HUD pixel shader reference. The new test fails against the
original shader at the expected off-glyph influence assertion. These
checks establish the transport correction and preserve the existing
fast-motion, edited-text, alpha and output-ownership controls; the next
headset comparison is still needed to judge the complete visible halo.

## Remaining orbital band, capture 194149

The user confirms targeting text looks good on 23deddd, while a slight
dark band remains around orbital lines. The new graphics flight
edvr_gfx_20260913_193915.log verifies v0.16.2-9-g23deddd, build
6AA74103, linked 00:34:11 UTC. The paired VR log identifies Valve
SteamVR. Capture 194149 has 844 draw snapshots, six surfaces and zero
capped draws, failed copies or missing shaders. First temporal frame
13639 uses 2774x2740 input, 4268x4216 output and jitter
(0,-0.166666657).

Ruled out: the remaining isolated orbital band is produced by retained
text cleanup, because the full-input ROI (1487,750)-(1727,1090) contains
only UI class 0, empty previous/next UI influence, and zero changed
output pixels between DlssBeforeUi and T00. There are 1,004 invalid
motion-vector pixels in this region, beside the orbital curve.

Three raw-crop row samples locate the curve at (914,120), (884,240) and
(859,360). Its HC record index is 7 and its private depth is about
3e-11, while the original scene depth is zero. The preceding merged
depth also contains the line. Immediately outside its left coverage
edge, two or three input pixels carry (5548,5480), the explicit invalid
history sentinel, instead of the approximately (1,-1) motion of their
neighbours. These are unmarked background pixels with zero current
depth. The backgroundHistoryHidden helper takes a maximum of the
previous merged depth over 3x3 samples and treats any positive depth as
an occluder when current depth is zero. Thus private navigation-line
depth rejects history in the surrounding transparent corona.

The correction separates the orbital line's geometric motion depth from
opaque scene depth. Orbital coverage keeps its private DSV bound with
GEQUAL testing and depth writes disabled, matching the original orbital
draw. Its HC record still carries the line's exact geometric depth and
instance transform. Keeping the depth test prevents a hidden line from
overwriting another object's motion record.

The mode-2 motion consumer accepts HC depth in front of or equal to the
merged scene depth, while rejecting nearer occluders, unmatched records
and ordinary text coverage. Other motion families retain their
exact-depth equality gate. Both the current DLSS depth and subsequent
history now contain the underlying scene at the line, so it cannot
invalidate adjacent corona history as though it were a solid object. No
extra texture, render pass or copy is introduced.

Validation: the absolute-path full build passes. The actual orbital
reissue preserves zero and finite private depth, retains per-instance
HC, preserves an existing HC record behind a nearer hull, and follows
the original draw order for overlapping lines at different depths. The
UI suite passes 25,084 checks on WARP and 26,830 on hardware with the
captured HUD pixel shader. The 55,320-check screen-motion suite verifies
source plus both shipping MV variants retain orbital affine motion over
sky, write underlying scene depth to ZC, preserve neighbouring camera
motion, and still invalidate background hidden by a physical hull.
Native TAA's shared HC consumer and far-sky call order were reviewed.

A scratch control restoring only the old orbital depth-writing state
compiles, then fails at "orbital coverage leaves the private scene-depth
seed unchanged" after the same 48/8/8 HC footprint. The final NVIDIA DLL
smoke passes, including DLSS evaluation, foveation and jitter/motion
convention checks, with runtime 310.7.0.0. An earlier smoke launched
before the full build finished skipped NGX initialization; the
completed-build rerun exercises those checks successfully. Preset K is
confirmed in the flight log. These checks establish the depth-history
correction; headset confirmation of the remaining visible halo is
pending.

## Intermittent bands and returning target smears, 202111 and 202117

The next graphics flight, edvr_gfx_20260913_201901.log, verifies
v0.16.2-10-gbd8f6f2, build 6AA757B9, linked 02:11:05 UTC. The paired VR
flight uses Valve SteamVR. DLSS runs preset K at 2774x2740 input and
4268x4216 output. Captures 202111 and 202117 begin at temporal frames
14454 and 14968, with jitter (0.375,0.055555582) and (0,-0.166666657).
Their raw crops are 1400x1400, starting at (687,670); output crops are
2155x2155, starting at (1056,1030). Capture 202117 has 1,034 draw
snapshots, seven surfaces, and no capped draws, failed copies or missing
shaders.

Ruled out: the orbital depth fix failed to take effect, because all
27,340 orbital coverage pixels in 202117 have zero original scene depth
and 27,027 also have zero merged depth. The remaining 313 overlap other
geometry; the orbital line no longer populates private depth. An
orbital-only output ROI (500,0)-(850,600) contains 210,000 pixels and is
byte-identical before and after UI resolve. A separate 125,000 pixel
orbital ROI in 202111 is also unchanged by UI resolve.

The orbital instances do not all have the same history failure. In
202117, records 3 through 6 match; record 7 is eligible but unmatched.
The original instance stream is present for all 19 captured draw frames
14969 through 14987. The first four instances keep their quaternion and
radius, whereas the fifth changes both every frame. Those fields are
currently part of the exact history key, despite already being
represented in the captured geometry transform. Record 7 was also
unmatched in the preceding 194149 dump. An unmatched record falls back
to camera motion; it does not automatically emit invalid motion.

Normalized RGB distinguishes this animated green/yellow curve from the
other four orange curves throughout the captured sequence: its ratio is
(1,1,0), versus (1,0.555556,0.111111). Absolute brightness changes on
both families. This supports testing a unique, colour-family match after
the exact search fails, retaining draw/mesh identity, width and the
existing projected-center/depth continuity checks. The user reports that
only some lines exhibit bands and suggests speed dependence. The
identity failure is consistent with that observation, but it does not
establish that every observed band has this cause.

Ruled out: fractional history transport alone eliminated targeting
smears, because 202117 still has 8,322 retained-only output pixels in
the target ROI, with mean before-to-after RGB change of
(-3.390,-1.448,-0.797) bytes. The post-DLSS resolver clamps complete RGB
values to the current composited raster. It cannot distinguish stale
text from a legitimate difference in the reconstructed corona. Removing
the clamp without an alternative would restore known stale digits; a
replacement must preserve changing text and translucent backgrounds in a
controlled temporal test.

The orbital correction retains exact matching first and only falls back
when no exact candidate exists. The fallback requires one prior record
with the same draw/mesh keys, line width and normalized RGB ratio, plus
the existing center/depth continuity checks. It rejects ambiguous,
nonfinite or zero-colour candidates. No texture or render pass is added.

Validation: all five instances from consecutive captured draw frames
14969 and 14970 now match. Independent double-precision quaternion and
scene projections agree with the generated affine maps within 0.002
input pixels. These compare points on the local orbital plane, without
claiming an exact screen-width extrusion correction. A scratch control
with only the fallback removed fails at the expected animated-instance
match assertion. The 386-check hologram suite covers transformed maps,
instance reordering, exact ambiguity, source/width/colour changes and
continuity rejection. The absolute-path full build and completed-build
NVIDIA smoke test pass, including DLSS evaluation and jitter/motion
convention checks.

## Text cleanup alternatives checked after the orbital correction

Code commit 2427d96 contains only the orbital matching correction and
its tests/documentation. It was fast-forwarded to main, pushed, rebuilt
in the main checkout, smoke-tested on NVIDIA and installed to Steam.
Separate installer verification confirms both DLL hashes. The UI
experiments below remain in scratch files and are not installed.

Ruled out: replacing the RGB cleanup with invalid motion across all UI
coverage, because a fresh preset-K sequence with translucent small
digits, fractional/fast motion, content changes and nonzero raster
jitter loses current text accuracy when current coverage is invalidated.
The matched valid and invalid inputs differ only in motion vectors. A
separate all-pixel invalid-motion control confirms the runtime consumes
those vectors. These runs use the same 1280x720 input, 1920x1080 output,
310.7.0.0 runtime and 90 Hz evaluation interval.

A narrower experiment invalidates only the departing footprint outside
the current 3x3-expanded UI. It preserves current motion and applies the
existing current-raster bounds and edited-digit cubic replacement, while
dropping retained RGB clamping. A corrected WARP replay executes the
actual UI shader over 32 frames. Its texture dimensions, half-float
motion upload, alternating history bindings, frame-major mask indexing
and source-edit textures were checked explicitly. The optional Windows
D3D debug-layer component is unavailable; the replay therefore runs on
WARP without that component. Both variants still render the changed
digit correctly.

Ruled out: departing-only invalid motion is an established solution to
the background mask, because a matched background-only DLSS control
still shows nearly the same brightness discontinuity with this
alternative. At frame 16, departing-region mean red bias relative to the
background-only model is -2.040 bytes with shipping cleanup and -2.107
with the alternative. At frame 27 the biases are +3.388 and +3.237. Some
samples improve, but the replacement does not reliably separate
stale-text rejection from the background's reconstruction. No new motion
invalidation or UI cleanup change is shipped on that evidence.

Earlier scratch CPU and WARP replicas had data-layout and sampling
errors and are not evidence for this conclusion. In particular, a
single-byte motion read, an incorrect coverage-frame stride, an
input-sized model texture and a float32 upload into an RG16 texture were
caught in review. The final WARP replay fixes these; the original CPU
comparison and its crops remain explicitly invalidated. The unresolved
work is obtaining a UI/background decomposition with correct blending
and draw order, rather than changing another colour-clamp threshold.

## Improved orbital lines, remaining text plume: 041432 and 041439

The user reports improved orbital lines on 2427d96, with a remaining
black smear around targeting text over the corona. Graphics flight
edvr_gfx_20260914_041241.log verifies v0.16.2-11-g2427d96, build
6AA76692, linked 03:14:26 UTC. The paired VR flight verifies the same
code revision and identifies Valve SteamVR. HEAD 50e3023 only added
investigation notes after that installation. This flight uses preset K
with 2644x2610 input and 4068x4016 output. Captures 041432 and 041439
begin at temporal frames 12104 and 12680, both with jitter
(0.125,0.2777778). Their raw crops begin at (622,605); their 2155-square
output crops begin at (956,930).

Ruled out: the target plume is generated entirely inside NGX, because
the new before/after comparison shows the UI resolver adding the dark
plume around UNIDENTIFIED SIGNAL SOURCE. In the full-output ROI
(2050,1750)-(2500,2150), 14,934 pixels change by more than one byte in
at least one channel. Of those, 7,391 lie outside current 3x3-expanded
UI coverage but inside retained influence. The 10,573 retained-only
pixels have mean RGB change (-2.9225,-0.1292,-0.0223) bytes. Every
changed pixel belongs to the resolver's active footprint. This audit
uses the actual disjoint output ownership formula, the unjittered
previous-alpha grid and bilinear transported age; mapping previous alpha
through the jittered raw grid gives different, approximate counts.

The private sprite depth must not be called equal to scene depth using a
generic floating-point tolerance. In this view it differs from zero sky
by roughly 3.7e-11, which is meaningful to the reversed-depth test.
Explicit background-history rejection remains a separate possible
contributor before DLSS. It cannot explain away the measured colour
change introduced after DLSS. A motion magnitude above two pixels is
also not an invalid-vector test; the explicit half-float sentinel is
(5288,5220) for this input size.

The rendering boundary is now traced through the actual census
identities. In 041432, sprite targets @163 and @505 are HDR resources
000001F265F9CAA0 and 000001F265F9DB20. The tone-map draws at ordinals
307 and 309 read those same resources through PS t1 views @1338 and
@1365. Their vertex shader is 2D78DC3FD2C0C543 and pixel shader is
99C21CEB7A699821. The outputs are RGBA8 through UNORM views. EDVR runs
DLSS later, on the submitted RGBA8 image, and applies kUiResolve after
NGX. The earlier scratch architecture claim that EDVR runs HDR DLSS
before the game's tone mapping was incorrect.

Disassembly identifies the missing replay inputs precisely. The sprite
pixel shader reads PS b1 as well as the already captured PS b2 and
source texture. Its original vertex shader reads structured t33/t38
buffers. The tone-map pixel shader reads PS b2, a three-dimensional
colour-grading LUT at t0, and the HDR eye at t1, with samplers s0/s1.
Its vertex shader samples the exposure texture at VS t0 through s0.
These inputs, their draw-time state, and native before/after colour are
needed to reproduce the compositing and colour conversion without
guessing a tone curve. Source alpha alone does not recover the
background beneath the text.

The next build adds evidence only. It does not change kUiResolve,
motion, depth ownership, or the confirmed orbital-line fix. Drawstate v8
attaches each sprite diagnostic to its existing real draw ordinal,
retains draw-local PS b1 and t33/t38, the input layout and complete
sampler/blend/depth/raster state, and takes paired central 1400-square
HDR crops. Original depth/stencil is copied before the draw. D3D11
requires whole-subresource copies from depth-stencil resources, so a
bounded, unbound intermediate receives the full depth resource before
cropping. This follows the [Microsoft CopySubresourceRegion
contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion).
Colour and depth crops each have a 160 MiB budget and at most ten pairs;
the reusable full-depth intermediate has a 128 MiB cap. Palette copies
share the existing 256 MiB structured-buffer cap. Work happens only for
the first frame of an explicitly requested dump.

A separate EDVRTON1 snapshot captures the exact tone-map shader pair at
up to two draws in that same requested frame. Exposure and the full 3D
colour LUT are copied at draw time, along with PS b2 and the actual VB0
window. HDR input and original output use central crops. The output copy
is armed before the draw and consumed exactly once afterward, even if
bindings have been restored. The format includes resource and view
identities, all relevant state, layout, draw arguments, crop origins and
the original shader bytecode. Texture payload budgets are 48 MiB for
HDR/output and 8 MiB for exposure/LUT; CB/VB copies are each bounded to
8192 bytes. Unsupported shapes are explicitly declined. Readbacks use
DO_NOT_WAIT after the ledger window: missing or pending copies produce
zero-length payloads and failure counts, never valid black images. The
parser is paired with the producer in build.bat.

The WARP fixtures exercise genuine crops larger than 1400 pixels,
distinct LUT slices, separate input/output resources and typed views
over typeless storage. They overwrite the original inputs after capture
and overwrite the output after the end hook, checking retained bytes,
original state and first-frame/two-draw limits. These tests establish
capture fidelity, not a visual fix. The next flight must place moving
target text against the corona inside the central crop; that evidence
will support a faithful UI/background compositing replay.

Validation: the full absolute-path build and NVIDIA smoke test passed.
Both snapshot fixtures and readers run in build.bat. The first full run
exposed an older solar fixture writing before its GPU fence; the test
now waits at that earlier write as well. Production still uses
nonblocking readbacks. The repeated full run passed, including 386
hologram-motion checks, 25,084 UI checks and the config contract. No
in-game claim is made for the remaining target smear.

## Native targeting captures and delayed ledger boundary: 051720/051740

The next flight verifies v0.16.2-13-g0b60cde in
edvr_gfx_20260914_051509.log, graphics build 6AA7D613 linked 11:10:11
UTC. Its paired VR log is 051515, the same revision using Valve SteamVR.
Gameplay uses preset K, 2644x2610 input and 4068x4016 output. Capture
051720 starts at temporal frame 16251 with jitter (0.125,0.277777791);
051740 starts at 17903 with jitter (0,-0.166666657). The draw ledgers
and C00 crops begin one frame later, at 16252 and 17904 respectively.
These frame namespaces must not be silently equated.

All ten sprite pairs in 051720 and all four in 051740 retained their
native R11G11B10F before/after crops, original depth/stencil, PS b1,
atlas and structured buffers, with zero reported failures. The label
draws are ordinals 176/243 for UNIDENTIFIED SIGNAL SOURCE and 198/264
for BINET'S FOLLY. The native before/after comparison places the visible
change at the text and its reticle; it does not show the broad dark
plume around departed text. Inspection PNGs use a simple display curve
only; quantitative comparisons decode the native unsigned 11/11/10-bit
floating-point values.

The post-DLSS resolver remains a measured contributor. In full-output
ROI (2180,1990)-(2540,2300), capture 051720 has 19,909 retained-only
pixels outside current expanded UI coverage. Of these, 15,869 change by
more than one byte between DlssBeforeUi and T00, with mean signed RGB
change (-3.3751,-0.4483,-0.1776) bytes. Capture 051740 has 8,487
retained-only pixels, 7,369 changed, and mean change
(-4.7050,-0.9934,-0.3766). Every change over one byte belongs to the
resolver's active footprint. Both audits use their own CSV jitter,
unjittered previous-alpha grid and bilinear transported age. These
measurements isolate added darkening; they do not prove that NGX itself
contributes nothing.

Ruled out: the absent tone-map records indicate that the game skipped
tone mapping. The census contains the expected shader pair in the first
rendered capture frame, but the root capture hook required that frame to
equal the earlier logical ledger arm frame. Both files consequently
contained zero draws and zero declines. This was an integration bug in
the diagnostic build, not evidence about the rendering artifact.

The hook now accepts matching tone-map draws throughout the armed ledger
interval. The snapshot still retains only its first actual matching
frame and at most two draws; its log reports that actual frame. The WARP
fixture exercises an arm interval beginning at 41 followed by two draws
at 42, serializes both, and verifies their complete and distinct
payloads. Requests outside the interval are rejected. Unsupported input
metadata retains a reason and available native resource/view
descriptors, with zero payload and incomplete status. Tests cover
missing exposure, a 2D resource bound as the LUT, and an unsupported BC1
HDR input while checking that the independent output and buffers remain
valid.

This correction changes diagnostics only. A faithful replay through the
game's colour conversion still requires a successful exposure/LUT/tone
capture; another UI colour clamp or motion threshold is not justified by
the current evidence.

An offline WARP replay now reproduces the BINET'S FOLLY draw using the
original shaders, atlas, buffers and recorded state. It changes 5,389
packed HDR pixels versus 5,385 in the captured draw. It is not
byte-exact: 1,362 packed words differ from the captured after image,
within crop-relative bounds (869,759)-(1022,835), with maximum native
channel error 0.25. Whole-crop mean error is 2.447738e-6, dominated by
unchanged background, and must not be used as a claim of exact text
reproduction. Earlier no-op runs were invalid: they uploaded
depth/stencil incorrectly and decoded the HDR exponent with unsigned
subtraction. The corrected run initializes the full R32G8X24 depth
resource legally, preserves the captured crop, and explicitly fails
validation if no pixels change. This is useful replay progress, not
validation of a new in-game UI path.

Validation: the corrected capture passed the full absolute-path build,
including byte-exact delayed-frame and unsupported-input fixtures, and
the NVIDIA smoke test with live NGX evaluations. The orbital-line and
temporal rendering paths are unchanged.

## Tone boundary and target separation: 054856/054906

The next flight verifies v0.16.2-14-ga4e2f9b in graphics log
edvr_gfx_20260914_054634.log, build 6AA7DD5C, linked 11:41:16 UTC. The
paired VR log is 054636, build 6AA7DD63, linked 11:41:23 UTC, with the
same version using Valve SteamVR. Gameplay uses preset K, 2644x2610
input and 4068x4016 output. Capture 054856 has logical temporal frame
16776 and first rendered frame 16777, with jitter (-0.25,0.166666687).
Capture 054906 has logical frame 17573 and first rendered frame 17574,
with jitter (0.375,0.055555582).

The delayed-frame correction works: both tone files contain two eye
draws, at ordinals 286/288 and 304/306 respectively. Their first-eye
converted RGB crops are byte-exact matches to C00, the raw DLSS input:
all 1,960,000 pixels match in each capture. This establishes the final
colour boundary for these views. The game's HDR scene, including target
sprites and later panels, is tone-mapped before EDVR runs NGX and then
kUiResolve.

Each tone draw retains its LUT, native HDR, converted output, constants,
vertex data and shaders. The only missing input is the exposure texture:
6x1 R32_TYPELESS storage (format 39), read through an R32_FLOAT SRV
(format 41), with SRV/RTV/UAV bind flags and no depth binding. Both
files correctly report two declines and two failures. The previous
whitelist accepted R32_FLOAT storage but omitted this storage/view pair.
The capture now accepts only that additional pair; other R32_TYPELESS
views remain rejected. A dedicated fixture retains exposure values 31
through 36 even after the original texture changes to 71 through 76,
while verifying the five independent payloads and the existing typed
path.

Ruled out: the exposure shader input can be ignored. The original vertex
shader samples it and passes the scalar into the pixel shader's gain
calculation. As an offline hypothesis, the retained CB2[2].y values
345.0128479 and 252.795929 reproduce the captured tone conversion very
closely without fitting: full-crop RGB mean absolute errors are 0.0356
and 0.0297 byte units, with a maximum of two bytes in either capture.
The candidate is inferred from the constants; its equality to the
missing exposure texel is not established. The WARP replay uses the
original DXBC, linear LUT sampling and point HDR/exposure sampling. Its
Draw(3,0) with no DSV is equivalent for this shader to the captured
single-instance draw, but is not a complete shipping-state validation.
No Windows D3D debug-layer validation is claimed.

The resolver still changes the departing text footprint. In 054906,
full-output ROI (2180,1990)-(2540,2300) contains 10,026 retained-only
pixels outside current expanded UI. Of those, 7,129 change by more than
one byte, with mean RGB change (-2.3659,-0.5248,-0.3147) across all
retained-only pixels. In 054856 the label is lower: corrected ROI
(2200,2200)-(2610,2470) contains 5,208 retained-only pixels, 2,280
changed, and mean change (+1.2187,+0.0200,0). It would be incorrect to
claim both views show retained-only darkening. All changes over one byte
are inside the resolver's active footprint. These calculations use each
capture's actual jitter and the shader's exact influence ownership.

Ruled out: restoring the first target's before image at tone mapping is
a general background reconstruction. In 054856, BINET'S FOLLY is drawn
at ordinals 184/247, then later work darkens every changed label pixel
before tone mapping. A stale before image would erase that later panel
compositing. In 054906, UNIDENTIFIED SIGNAL SOURCE at ordinals 198/264
has no subsequent changes on its label pixels, so this narrower capture
supports an exact counterfactual source image.

For the first eye of 054906, all 7,361 packed words changed by the first
label draw match the final HDR tone input. Later captured sprites leave
all those words untouched. Replacing only those words with their
before-draw values removes the label while preserving the remaining
final HDR image exactly. Paired WARP tone replays of this original and
clean input differ on 7,259 pixels, with zero changes outside the source
footprint and unchanged alpha. Both sides use the same inferred
exposure, so the paired difference does not introduce the replay's small
rounding discrepancy over unrelated background. This proves a useful
local colour decomposition for this capture. It does not yet validate
DLSS history, output-resolution text quality, arbitrary later draw order
or the cost of a production implementation.

The remaining visual work is separating current target text from the
world history while preserving later blending and visibility. The
orbital correction and production UI resolver are unchanged by the
capture-format correction.

Validation: the full absolute-path build passes, including the typeless
exposure fixture, parser checks and config contract. The completed-build
NVIDIA smoke test passes with live NGX evaluations and motion/jitter
convention checks. The counterfactual remains an offline experiment; the
production change only completes diagnostic exposure capture.
