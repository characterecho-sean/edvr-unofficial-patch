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

The native census provides candidate later panel writers, including
81216C77F90DEDD6/A2965EC2931A39C8. The candidate at native ordinal 186
reads three PS textures: 1536x512 (resource/view formats 9/11), 512x512
(70/71), and 1107x692 (27/28). Its 208/5376/224-byte constant buffers
are retained, but the needed texture pixels, draw-local depth/stencil,
vertex/index data, input layout and full state descriptors are
incomplete. Specifically, PS t2 is retained as surface 1 in frame 16777,
with 3,064,176 bytes at 1107x692 in format 28; PS t0/t1 pixels and the
remaining listed state are absent. The sprite-only diagnostic does not
supply those for a later panel. Native census ordinals and snapshot
ordinals must not be silently equated: the tone operation is 285 in that
census and 286 in its tone snapshot. This supports identifying candidate
shader families, not an exact replay of the panel or a claim that one
particular panel caused the attenuation.

Ruled out: absence of direct writes after the last panel implies an
unknown UAV/copy path caused the attenuation. The last captured sprite
precedes the known later panel draws, so those draws remain candidates.
There is no evidence requiring a hidden compositor operation.

A further limitation of the signed-colour experiment is that adding
fullLDR-cleanLDR to a reconstructed background preserves the difference
between reconstructed and current raw background underneath opaque text.
It proves locality but not exact current text colour. An implementation
must also preserve effective target opacity through later blending, or
otherwise account for that background residual. For example, given a
validated effective opacity A, the expression
modelWorld*(1-A)+fullLDR-cleanLDR*(1-A) preserves opaque current text
and uncovered model background. This is a design constraint, not a
validated production compositor; partial-opacity tone response and
output-scale filtering still need explicit checks.

A cropped 32-frame experiment now runs the saved target contribution
over the cleaned star background at 1280x720 input and 1920x1080 output.
Both combined and background-only preset-K NGX runs complete 32
evaluations. The input includes fractional and fast target motion,
nonzero raster jitter, zero background motion and a glyph block removed
at frame 16 and restored at frame 24. The actual UI shader runs on WARP
with correctly laid-out coverage, source edits and alternating history.
The CPU comparison samples the current signed contribution at the
output-to-input coordinate including the current jitter. Earlier
fixtures with incorrect mask layout/bits, unapplied raster jitter, edit
flags without actual content changes, or a CPU composite that omitted
jitter are invalidated.

Across the final sequence, the combined NGX/UI output differs from the
background-only control on 738,462 pixel-frame samples outside the
filtered current target footprint, counting any byte difference. The
background-only model plus current contribution differs on zero such
samples. The zero follows from explicit layer isolation; it is not a
claim that the experiment has solved opacity, output-resolution text
quality, full-game blending or runtime performance. The test supports
keeping target colour out of world history, while the unresolved
effective-alpha and later-panel requirements above still prevent
shipping that experimental composite.

Code revision ab74d7d was fast-forwarded to main and pushed, rebuilt
there, NVIDIA-smoke-tested, installed to Steam and separately verified
for both DLLs. The installed version is v0.16.2-15-gab74d7d. Subsequent
investigation-only documentation does not change that installed code.

The next capture adds panels_<stamp>.bin (EDVRPNL1) for the later
81216C77F90DEDD6/A2965EC2931A39C8 panel family. It retains up to 16
draws from the first matching HDR target and actual matching frame
within the armed interval. The 054856 census contains 12 candidates per
eye, so this bounds one eye without truncating that observed sequence.
Other eyes/frames and skipped/replaced draws have separate counters.
This is diagnostic instrumentation; target and orbital rendering are
unchanged.

The ledger supplies the original draw arguments and ordinal. Panel
snapshots begin after the existing texture/constant substitutions and
end immediately after the actual draw, before private depth/motion
reissues. Consequently they retain the inputs actually used on screen,
including an upscaled panel texture when that feature is active. Shader
object identity is checked at this boundary. Swallowed native draws are
counted as skipped instead of receiving a misleading before/after pair.

Each draw retains central 1400-square native HDR before/after images,
pre-draw depth/stencil, the complete mip chains of PS t0/t1/t2, all five
required constant buffers, complete structured buffers at VS t33/t38,
their exact SRV views, bounded geometry, shader bytes, layout and render
state. Texture storage/view pairs include the observed 9/11, 70/71 and
27/28 formats. Mapped padding is omitted while BC1 sub-block mip rows
remain intact. The 32-MiB native index buffer is captured only over the
submitted index window. Vertex windows are capped at 256 KiB per slot;
the reader validates every referenced element, including signed base
vertices, binding offsets and per-instance step rates, before calling
the geometry replayable.

Retained GPU payloads are capped at 768 MiB, with a separate reusable
128-MiB ceiling for the full depth scratch. Depth is copied whole to an
unbound default texture before cropping, as required by D3D11. Source
textures are capped at 64 MiB each, structured buffers at 32 MiB and
constant buffers at 8192 bytes. All copies are confined to explicitly
armed eye dumps. There are no new normal-play GPU passes, binding
changes, flushes or draw-time readbacks. Deferred maps use DO_NOT_WAIT;
unavailable data is empty and explicitly incomplete, never valid black.

The production writer is exercised by a WARP fixture with an actual
indexed, instanced draw. The strict reader verifies the retained source
bytes after every buffer, all texture mips, depth/stencil and the target
are overwritten. This covers typeless textures, nonzero SRV mip/element
ranges, geometry binding offsets, a changed target binding at End, and
idempotent End. Separate files demonstrate missing PS b1, missing End
and an interior index outside the retained vertex window. The latter has
complete input bytes but deliberately fails geometry verification. The
fixture also checks frame/eye selection and the 16-draw cap. These
checks validate capture integrity, not a full replay of the game's panel
shader or a finished target-text compositor. A fresh overlap capture is
still needed after installing this instrument.

Validation: the full absolute-path worktree build passes, including the
production panel writer/reader gate and the config contract. The
completed-build NVIDIA smoke test also passes with live NGX evaluations
and motion/jitter convention checks. No D3D debug-layer validation is
claimed; that SDK component is unavailable on this machine.

## Native OpenXR captures after reboot: 152121/152136

The user reports that restarting Windows stopped the startup crashes.
The new successful flight is gfx log edvr_gfx_20260914_151812.log,
v0.16.2-119-gd160499, graphics build 6AA85C83, with native OpenXR log
151813_548_6608. This is the standard native OpenXR main revision, not
an incompatible experimental transport. SteamVR supplies 4980x4916
output; DLSS receives 3237x3195. The first dump concerns high-speed
corona smear; the second contains UNIDENTIFIED SIGNAL SOURCE against the
stellar limb. The separate crash-reporting change is documented in
startup-input-crash-2026-09-14.md and does not claim to fix the
intermittent exception.

Both dumps now have complete panel and tone-map inputs, including the
previously missing exposure texture. Panel snapshots retain twelve draws
with zero declines or failures. The first actual captured frames are
16833 and 17801, respectively, one after the logical temporal arm
counters. The native crop origin is (918,897), size 1400x1400. The
treated crop origin is (1413,1380), size 2154x2155. In 152136, T00 is
byte-identical to that central L0 crop. In both dumps, the captured
tone-map output RGB is byte-identical to C00.

In 152136 the post-NGX UI resolver changes 77,854 output pixels by more
than one byte. Mean signed RGB over those changed pixels is
(-2.3276,-0.4236,-0.1632) bytes. The moving-label ROI
(2500,2000)-(3500,2850) includes 59,649 retained-only pixels; 42,763
change by more than one byte. This confirms that the resolver
contributes darkening outside the current label. It does not establish
that NGX contributes nothing. The six native sprite before/after pairs
contain clean current text without the broad plume.

In 152121 the stellar-limb/glow ROI, crop-local (1500,0)-(2154,1500),
has 16,058 pixels whose mean absolute RGB change exceeds one byte
between DlssBeforeUi and T00. All belong to current or retained UI
resolve activity. This ROI includes the MIDNIGHT SUN banner; it is not
an isolated corona-motion measurement. The audit uses
q=floor(output*input/outputSize), r=floor(q+jitter+0.5), current class
1/2 and edits around r, stationary Previous[q], and bilinear history at
q+MV[r], rejecting off-screen history. Class 3 is smoke/corona, not UI.
Earlier scratch audits using a jittered q, outgoing alpha, or omitting
stationary history are invalid. Their apparent 603 unowned changed
pixels disappear with the correct ownership rule.

Mode-5 record 2 is matched and covers 72,052 input pixels. Its submitted
motion agrees with its recorded affine transform after the jitter delta;
this checks the consumer, not independently the original geometry. The
original 5E417E9DF2E7F9E6/BD801F2FB02522EB shader pair and retained
vertices show that fixed width/geometry pairs admit the producer's
affine mapping. The first retained Holo map points to the preceding
frame, whereas draw geometry begins at the current frame. Comparing that
map with the following frame was invalid and its apparent 6.376-pixel
error is discarded. These captures do not establish a separate corona
motion-vector defect or rule out smearing within NGX itself.

The exact panel word comparison finds visible changes only at ordinal
234 (27,404 pixels, lower-left HUD) and ordinal 252 (21,331 pixels,
MIDNIGHT SUN/INFO). The other ten panel draws change zero packed words.
None of these panel change masks overlaps the first targeting sprite's
changed pixels. There are nevertheless 41 later writes to the first-eye
target before tone mapping, including draws outside the instrumented
panel family. Ruled out: copying the HDR scene before the first sprite
and restoring it at tone mapping is a general clean-background solution.
It would omit valid later writers; their identity cannot be inferred
from draw size.

The corrected original-panel WARP replay changes the same 21,331 packed
pixels, within crop bounds (514,657)-(1019,838). Against the captured
after image it differs at 3,516 packed pixels, maximum native RGB error
0.03125. Mean absolute RGB error on those residual pixels is
0.000399012. These are freshly verified panel metrics; older BINET'S
FOLLY metrics accidentally reused in an intermediate scratch report are
invalid here. Initial no-op runs were caused by uploading the retained
VB0 window at byte zero instead of its captured byte offset 1440.
Neither bypassing depth nor a solid pixel shader validates a no-op
replay.

WARP tone-map replay now uses the actual captured six-texel exposure,
original shaders, lookup table, constants and viewport. Both complete
output crops differ from the native capture by at most two channel
bytes; 184,951 and 245,546 RGB pixels differ respectively. This is a
bounded software-versus-hardware replay discrepancy, not byte-exact
reproduction. No exposure fitting or old inferred exposure is used.

A further original-panel replay retains accumulated alpha using an
RGBA16_FLOAT target. The alpha footprint has 21,331 pixels, including
618 above one; the maximum is 1.033203125. Rendering the same original
background into that format produces thirty pixels with negative RGB,
down to -0.02192688. The game's unsigned R11G11B10_FLOAT target clips
those negative components. For example, crop pixel (984,795) produces
blue -0.000225067 in the half-float target and zero in the
original-format replay. Ruled out: replacing the clean HDR target with
RGBA16_FLOAT solely to store UI opacity is behaviorally equivalent. It
changes blend clamping as well as quantization, and the stored alpha is
not necessarily opacity in [0,1]. Clamping it to one is also incorrect.

Keeping current text out of world history remains the supported
direction, but a production implementation must preserve later writers,
original-format blending, and world-only depth/motion beneath the
removed text. Removing colour while retaining the target's motion
vectors would feed NGX inconsistent inputs. The CPU algebra sketch and
the first synthetic sidecar GPU fixture do not validate that
implementation; review rejected the fixture's no-op/occluder checks,
blend/depth differences and incorrect packed-format decoding. No new
rendering fix or experimental compositor was installed from this
investigation. The current captures are sufficient for the next offline
implementation work; another user capture is not requested at this
point.

Validation: the full absolute-path build and all gates pass, followed by
the NVIDIA smoke test with live NGX evaluations. These validate the
existing rendering code and the separate crash diagnostics, not the
rejected compositor experiment. Crash diagnostics commit 58cd817 is on
main; the working game remains on d160499.

## Current target colour outside DLSS history

The implementation following the 152121/152136 analysis separates the
recognized planar target-sprite family before DLSS. Its current colour
is restored after temporal reconstruction. Other UI continues through
the existing resolver. This does not change the corona's motion-vector
producer; the captures did not establish an independent defect there.

The original SM5 pixel shader now exports its result to three render
targets during the original draw. MRT0 retains the game's exact target,
blend, raster and depth/stencil state. MRT1 has the same R11G11B10
format and receives every subsequent supported world draw, but omits
separated target sprites. MRT2 stores signed scalar influence as
R32_FLOAT, with source alpha exported in both X and W for fixed-function
blending. Influence is deliberately not clamped to [0,1]. Opaque world
draws erase it; later source-over world draws attenuate it; additive
world draws preserve it. Destination-dependent source factors,
unsupported destination factors, existing MRT/UAV output, linked shaders
and unsafe bytecode decline to the original rendering path.

The game's original tone-map draw runs again on the clean HDR texture,
using its existing exposure, LUT, shader and constants. The measured
152136 tone state satisfies the guards: N/3/1, single R8G8B8A8_UNORM
RTV, 3237x3195 viewport, solid rasterization, no scissor, blend, depth
or stencil. Repeated, partial, predicated or side-effecting tone passes
are rejected. The submitted colour must be the tracked tone-map output.

Private depth, UI mask, content-edit mask and hologram coverage are
snapshotted before the first removed sprite. Later supported coverage
draws replay into those clean metadata targets. The original game depth
is preserved. A missing twin or unsupported later coverage owner
disables separation instead of combining clean colour with target
motion.

The post-DLSS composite uses matching full/clean current tone-map
samples and signed influence. It exactly restores the full current
colour when the reconstructed world equals the clean current sample.
Applying HDR influence to a temporally reconstructed LDR background is a
local reconstruction approximation through the nonlinear tone curve, not
an assertion that tone mapping is linear. Bilinear current sampling
avoids introducing negative filter lobes around glyphs. The existing
resolver must be available before clean colour can be submitted to NGX.
Entering separation or abandoning it on a detected failure resets the
affected temporal history once.

The actual captured panel test uses the production UiColourLayer and the
original 15992-byte pixel shader after fanout. MRT0 is bit-identical to
the original software replay, clean colour is bit-identical to the
pre-panel image, and R32 influence has 21,331 nonzero pixels with
maximum 1.03355. This tests the original-format blend mechanism; the
production classifier currently removes target sprites, not these
panels.

The corrected late-draw audit uses ph= for pixel shader identity, not
vh=. All thirteen available pixel shader programs are accepted by WARP
after the production transformation. EA02FAC2BD6C643C and
E95634B0F61D218F contain immediate constant-buffer data with a separate
length word; the parser now preserves those blocks. Three later pixel
shader binaries were not retained in these captures: 5E72F436FC8A5736,
2B156A05E98F2D5D and 16196F69ADE35E77. Their runtime programs must pass
the same guarded transformation; they are not claimed as replay-tested.

The repository GPU test checks original-output identity, later opaque,
translucent and additive world draws, discard/scissor behavior, signed
influence, current-composite identity, frame reset, state restoration
and unsupported-state rejection. Its 415 checks pass on WARP and the RTX
5090. A separate 51-check controller test exercises target draw, later
world draw, tone replay, input publication and safe fallback using real
D3D11 calls with fixture classification and coverage discovery. It also
checks that an internal render-thread scope cannot suppress shader
bytecode capture on a worker thread, and that tracked view writes
decline while unrelated writes remain harmless. Compute UAV bindings and
UAV clears use this guard without adding resource queries to every
dispatch.

A partial GPU cost probe at 3237x3195 input and 4980x4916 output
measured the production resolver at 0.2820 ms/eye without separation and
0.3663 ms/eye with separation on the RTX 5090. Six input-sized seed
copies measured 0.1611 ms/eye. These measured components add
approximately 0.49 ms per stereo frame. The probe uses uniform images;
it is not a flight performance result and excludes the extra tone pass,
later MRT writes and clean coverage replays. Memory and total flight
cost still need observation under the real scene.

Eye dumps now retain DlssColour, UiInfluence and UiDepth when separation
is active. HoloCoverage and UiEdits then hold the clean maps actually
used, and EDVRTEX1 UI-flags bit 64 identifies that path. Logs
distinguish prepared input, actual DLSS evaluation/current compositing,
and a session-level decline. These signals are necessary before
attributing a future visual result to this implementation.

Validation of this implementation: the absolute-path full build and all
gates passed, including 25,123 private-depth checks, 415
colour/composite checks and 51 controller checks. The colour/composite
checks also passed on NVIDIA hardware, followed by the completed DLL's
full smoke test with live NGX evaluations and the motion-convention
probe. One earlier full run stopped in the census bridge's timing
assertion; that child left both logs empty. The identical binaries
passed the isolated repeat, and the next complete full build passed that
gate as well. No timing-test or production-timer change was made to
conceal the failure.

The test package was built before committing, with version
`v0.16.2-121-gf264560-dirty`. Its native graphics SHA-256 is
`c0243e75b1e5b6959c6802b47b33b8ed85f768362873fa8006dd441ba2af7f2b`; the
native runtime SHA-256 is
`d5bd872d26aef5a74035e2cca1a503415125c7445295e29760c6a56a5e3d334d`.
These identify the tested binaries independently of the later source
commit. A user flight has not yet confirmed activation, visual behavior,
or total performance cost.

## Native UI replay after DLSS (2026-09-14)

Run 173641 used the installed binaries identified above. At
17:36:27.030, the old separation controller disabled itself because
submitted colour differed from the tracked tone-map output. Neither
successful preparation nor evaluation was logged. The screenshot
therefore did not exercise that separation code. Ruled out: a simple eye
swap, because the C00 crop matches tone-map output 0 byte for byte and
differs from output 1. The replacement tracks complete resource copies
and canonical COM identity rather than assuming that the submitted
pointer is the original tone target. Partial copies and later writes
cannot qualify as matching colour.

The requested output path captures recognized UI draws and runs their
original vertex and pixel shaders at output resolution after DLSS. The
supported scene families are E508 target sprites, 8121 lit/unlit
holograms, C7FA orbital lines, and B779 planetary flight HUD.
Classification still requires the measured UI-depth scene context, not
only a shader hash. Planets, rings, the corona and smoke remain world
draws. Full-eye DLSS/DLAA uses this path; AA Off, TAA and cropped/foveal
AA retain the existing path.

The capture also proves that UI is not a contiguous final block. Solar
surface 4D516EF05C68FFA5 and several other world draws occur between the
two UI groups. Moving only the colour writes in time would change the
original fallback image. Instead, the game keeps its original colour and
depth/stencil writes. Interleaved world draws export the same shader
colour to a clean HDR target with the original blend state in the same
raster pass. UI is omitted from that target. Elite's original tone
shader, LUT, exposure and constants produce the clean input for DLSS.
Unsupported states leave the original game image intact.

Native replay snapshots mutable vertex/index/constant buffers and
sampled resources on the GPU. Draw packets retain input streams,
instancing and offsets, samplers, blend factors, scissor, depth/stencil
and immutable shader state. Source versions share snapshots until a
tracked write; storage is reused after the frame boundary. The pool is
capped at 256 MiB. There is no production CPU readback, wait, flush or
staging map. Original draw ordering is preserved within the native UI
layer. The original world depth seeds its clipping; UI that writes scene
depth or changes a stencil bit needed by another UI draw is declined.
World draws that read stencil bits changed by UI are also declined. An
unsupported sequence latches the new controller off until AA is switched
Off and back on, or the game restarts. Existing UI handling resumes on
subsequent frames, with one history reset per eye. A native overlay
already prepared for the other eye can still complete.

The output viewport removes the input jitter and scales to native output
resolution. Replay uses an R11 HDR scratch target and scalar
transmission. Elite's original tone shader evaluates the clean and
UI-bearing HDR images with identical state; the current UI residual is
then combined with the DLSS world. Outside coverage, output passes
through exactly. Across a nonlinear tone curve this residual remains a
local reconstruction approximation, not an assumption that tone mapping
is linear. Deferred UI does not write the existing UI
motion/reactive/history passes. The entire native command list must be
ready before clean input is published, and it executes only after
successful DLSS evaluation.

The first seeder copied all eight stencil bits. On the RTX 5090, D3D11
reported no pixel-shader stencil-reference support, so that meant eight
stencil passes. Reading the union of the actual UI stencil masks reduces
this to the bits needed for clipping, and a depth pass is omitted when
all captured UI has depth testing disabled. The isolated 2913x2913 to
4482x4482 probe fell from 1.5714 to 0.5570 ms per eye; command
preparation measured 0.0477 ms per eye over 20 warmed samples. These are
synthetic one-draw, simple-tone measurements. They exclude DLSS,
pre-tone work, the actual game materials and whole-flight cost. Output
scratch allocation is shared between eyes; native resolution still has a
material memory cost.

The controller regression performs actual WARP readback for native
scale, 2x scale, positive/negative jitter, premultiplied and additive
UI, stencil clipping, copied submits, later writes and unsupported state
rejection. Opaque, source-over and additive world draws interleaved
between UI groups leave original colour byte-identical and clean colour
equal to a separate world-only reference. The shader test initially had
incompatible VS/PS input register signatures; correcting the fixture,
rather than relaxing pixel comparisons, made both WARP and NVIDIA
comparisons pass. The depth test exercises D24S8, D32S8 and D32,
spatially varying depth, all stencil bits, output reuse and jittered
source addressing. Mutable draw-input snapshots also have independent
GPU cases. The final controller suite contains 840 checks, including
failure latching, per-eye recovery, preservation of a prepared overlay,
and rejecting world draws that depend on UI stencil changes.

Flight logs distinguish captured UI, successful native composition,
route mismatch, unsupported states and allocation failure. Periodic
totals report captured/applied/declined counts and snapshot memory. Eye
input slot 16 contains the actual clean DlssColour when active. A flight
is still needed to confirm activation and appearance of all requested UI
families with Elite's full scene and to measure total cost.

Final validation: the absolute-path full build completed with all gates
passing, including the 255-key config contract and native installer
payload comparison. The completed controller's 840 checks and all depth
cases passed on NVIDIA hardware, and the completed native graphics DLL
passed the full smoke test, including live NGX evaluations and the
motion/jitter convention probe. No D3D debug-layer validation is
claimed.

The tested package is stamped `v0.16.2-127-g66aea5a-dirty`. The native
graphics SHA-256 is
`3d881702d6955b737910212d54a070e223ce2c10ff2d0d14a6d31cf9fad1eef4`; the
native runtime SHA-256 is
`97a0002c489f81df693600e2e80d50308a3943ca31918583afaf0d7c32eb7f18`. The
later source commit does not change those tested binaries.
