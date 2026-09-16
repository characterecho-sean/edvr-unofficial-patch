# Navigation masks against stellar glow: issue 36

## Status

Updated 2026-09-16 (night). Sean chose A. The mask's rule is fixed on branch
claude/star-corona-ui-smearing-d1d2ae, built clean, and installed to Steam
for a confirmation flight (numbers and the brief in the night entry).

What was measured (Steam dumps 131013 and 134857, no flight):
- a numpy port of `kUiResolve` reproduces `L0` to 0.076/255 and 0.002/255
  mean, bit-exact on inactive texels, and `UiNext.a` to 99.7% and 99.9%;
- NVIDIA's output leaves the 2x2 raw range on clean corona pixels by a
  median 6/255 and a 99th percentile 12..16/255 (max over rgb), and the
  label footprint's distribution is the same, so it held no ghost;
- with the bound widened by 12/255 the footprint's dimming goes from -3.6
  to -0.1 luma on both dumps, the rim around the text from -1.4/-1.6 to
  -0.2/-0.3, and a real ghost is still cut to 12/255;
- a second effect, a box the size of the label while its distance ticks
  (-1.8 luma on the label's background, 70% of it moved): the edited branch
  rebuilt every edited texel from the raw cubic; rebuilding only where the
  cubic disagrees beyond the tolerance leaves +0.2 with 13% moved.

The change: `src/d3d11/ui_resolve.h` widens the clamp by a tolerance
carried in a b1 constant buffer and gates the edited rebuild on it;
`advanced.ui_ghost_tolerance` (default 12, in 8-bit colour steps, live)
sets it, and 0 restores the old shader to the bit; rig cases in
`tools/ui_depth_test` cover both sides of the tolerance and the edited
branch; all gates green (25150 ui_depth checks, config contract 262/262).

Ruled out on the way (details in the journal): the game drawing a dark
backing; NVIDIA producing the label mask; the mask lying where the UI
content changed; the format-9 change ending the deferred route's latch-off;
a higher `stale` threshold alone (it shortens the trail, but the clamp still
dims every active corona pixel each frame); a tolerance relative to the raw
level (a fraction of the red channel passes a ghost at near half strength).

Next flight (a confirmation, not a search): a label over the corona while
the distance ticks, then sweeping across it. Expect no polygon, no box, no
trail, clean digits. `python tools\edvr_log.py --target steam
--expect-build HEAD` must name the build; take an INSERT eye dump on the
label. If anything remains: `ui_ghost_tolerance = 0` in the Steam ini is
the old rule live, 16 removes the last speckle; report which looked right.

The deferred UI route (src/d3d11/ui_deferred.cpp) is untouched by this. It
was refused for the tenth time on 2026-09-16 13:47 (`panel vertex buffer
contract changed`) and has never rendered a label in the headset; its
format-9/11 change was carried into this branch (1b3d864), and the Codex
worktree c009 itself is still dirty with the same diff.


## Investigation

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

## Captured replay rejection, 113532 (2026-09-15)

The flight's bounded census records VS `6DB587D29F43A9A6` / PS
`B2DE0A41A4C2B4F5` at DC 0 draw 170 and the other eye's draw 232. The
first has 342 indices, one instance, target @236, and `bl=12,7,1/2,7,1
bm=F`. The formatter in `draw_census.cpp` prints the enable digit
immediately before SrcBlend: this means enabled, ONE, DEST_ALPHA, ADD
for both colour and alpha. It does not mean enum 12. `material()`
rejects destination factor 7 before attempting shader fanout. Its
permitted destination factors were written for scalar UI transmission
but also applied to world draws.

The retained `edvr_logs/shaders/ps_B2DE0A41A4C2B4F5.dxbc` is 2020 bytes.
Disassembly shows a full RGBA target and a single final return; the
RGB-only output hypothesis is ruled out. The correction keeps the
original world blend on both original and clean HDR targets and keeps
the narrower restrictions for UI composition. Material cache keys now
include the UI/world role. A rejected world material reports the
specific blend or shader reason before the existing failure latch
disables replay. The 128-entry material cache remains bounded; no new
frame readbacks or GPU waits are introduced.

The retained shader passes fanout (2020 to 2152 bytes) and WARP shader
creation. The isolated controller passes 1086 checks, including the
exact destination-alpha blend as an interleaved world draw, byte-exact
original HDR and LDR preservation, and clean input compared with an
independent world-only render. The same blend still correctly rejects UI
scalar transmission. Full build and hardware validation follow
separately.

Independent capture review aligns `DlssBeforeUi` and C00/T00 to scene
frame 19834 using the binary header and motion CSV. The ledger sentence
uses 19835 because its draw counter deliberately records `g_frame + 1`.
The 2155-square final crop at output origin 956,930 is 98.81% identical
to the matching before-UI crop, with mean absolute error 0.0337/255. The
target label overlaps separate panel text/rules already in raw input and
in `DlssBeforeUi`; it is essentially unchanged afterward.

Ruled out: post-DLSS UI replay or final cropping creates the frame-00
target smear, because it is already present before that stage. This does
not localize softer text in frames 03/07/12, whose before-UI images were
not retained, or prove which draw family renders the label. Initial
C03/C07/C12 comparison composites used the wrong scale and included 200
padded black columns; discard their pixel comparisons. Correct alignment
scales the full 1400-square raw crop to the 2155-square output crop
before selecting the same target region.

A bounded first-eye census review found no additional definite blocker
in the reconstructable interval, same target @236 through draw 218.
Interleaved world draws use supported blends, complete RGB write masks,
and no conflicting stencil comparisons. Available shaders write full
RGBA with one final return. Bytecode is missing for families 5E72, 2B15,
and 1619. The census records only RTV0 and basic blend state, so it
cannot rule out extra MRTs, OM UAVs, or logic operations. No retained
census line reconstructs the earlier B779 failure interval itself. These
limits must remain explicit when assessing the next flight's
activation/failure log.

Build setup: the first full build skipped legacy OpenVR export
generation because its default Frontier runtime path was absent. That
left the prior native `build/openvr_api.dll` in place for the legacy
census bridge test. Both children exited 1 immediately; direct child
execution reported `real typed compositor through proxy`, not a timeout.
Rebuild with `--openvr` pointing to Steam's
`Openvr/win64/openvr_api_orig.dll` to create the required legacy test
fixture before the final native outputs replace it. This requires no
source or installed-runtime changes.

The next build passed its gates but compiled DLSS out because SDK
discovery was not configured. Its smoke explicitly reported no DLSS SDK;
do not install those binaries for this task. The previously working SDK
is `C:/Users/seanm/AppData/Local/EDVR/ngx-sdk` (310.7.0), selected using
`EDVR_NGX_SDK`. The final build must verify it before compilation and
its smoke must execute live NGX and motion/jitter conventions, not skip
them.

Final validation passed with the pinned SDK configured and the build
process granted access to it. `build-ui-world-blend-113532-ngx.log`
records SDK 310.7.0 verification, legacy OpenVR rebuilt, all seven
census bridge cases passing, the 255-key config contract, DLSS runtime
carried, and all gates passed. The completed controller passes 1086
checks on WARP and NVIDIA hardware. Retained B2DE fanout passes four
checks. The live NVIDIA smoke passes DLSS evaluations and the
motion/jitter convention rig without skipped checks. No D3D debug-layer
validation is claimed.

The final test package is `v0.17.0-rc.1-1-ga44ae48-dirty`. Graphics PE
`6AA98CD6` was linked at 2026-09-15 18:22:14 UTC; its SHA-256 is
`9b451b8916a61a67204e8bdf0ab84ae781a3f610482956e8f259b4f7472c58ba`. The
native runtime SHA-256 is
`ed12a9230ee51904019c93102c91a4fb858127afb7725d1d2a51d2230cc87c06`. The
subsequent source commit does not alter these tested binaries. The next
flight must show `Deferred UI: active` and advancing applied totals; if
it falls back, use its new specific failure reason before drawing a
visual conclusion about native UI replay.

## Empty tone route, 154217 (2026-09-15)

Sean reports the same targeting-label smear. Flight
`edvr_gfx_20260915_153936.log` matches the installed graphics PE and
SHA-256 recorded above; `install_edvr.py --all --verify-only` also
passes. The source commit followed the build, explaining the literal
HEAD version mismatch. This is evidence from the intended correction.

At 15:41:18.579 the controller captures 8121/A296 holo draws at
2644x2610, with eleven draws retained per eye. At 15:41:18.584 it
reports `submitted=0000024E0981F520 tone0=0 tone1=0`, then disables
replay with `submitted colour has no complete matching replay`. The
bound shader pair is DEF19B035D5EDEDC/831DF02EBA8AE814. No tone-pass
validation failure or successful composition is logged. The 15:42:17
dump is after that latch.

Ruled out: destination-alpha world blend support alone enables the new
UI path in this flight, because a separate tone/submit handoff fails
before any successful application. No conclusion about the native UI
path's image quality follows from this fallback capture.

Candidates and discriminating evidence before any rendering change:

- Tone hook not reached: the actual tone VS/PS pair or hook ordering
  must differ from the recognized pair and controller test sequence.
- HDR source mismatch: tone PS t1 resolves to a different texture from
  the captured UI's HDR target, potentially after a fullscreen pass.
- Alias invalidation: a recognized tone output was recorded, then a draw
  or resource write removed it before submission.
- Early submission: reconstruction was requested before the matching
  tone pass completed in the same frame.

The later census contains the expected 2D78/99C2 tone pair and two valid
retained tone snapshots (first matching frame 15055). It cannot directly
reconstruct the startup failure at 15:41:18. Decode render-target and
SRV views through their resource records; `c=` is a constant-buffer
binding, not the colour target. The existing controller's tone helper
omits the intervening full game hook sequence; cover that before another
test flight.

The later census resolves the first-eye resource chain exactly. UI HDR
RTV @224 and tone t1 SRV @1547 both view `0000024E098244E0`. Tone draw
261 writes @1564 (`0000024E09825560`). Draw 262 samples the same texture
through @1578 and writes @1577 (`0000024E0981F520`), exactly the
submitted resource in the startup failure. The other eye follows the
same chain: HDR `098289A0`, tone output `09828C60`, final output
`0981EFA0`.

Ruled out: an HDR ping-pong copy causes the later tone source mismatch,
because the UI RTV and tone HDR SRV share the same underlying resource.
This later evidence does not reconstruct startup resource lifetimes.

Draw 262 uses VS `20F383BBAC05C031` and PS `DED8796049C7BB4A`: the VS
forwards position/UV; the PS samples t0 through s0 directly into RGBA.
It is N4, triangle strip, same-size UNORM source/destination,
depth/stencil/blending disabled, full viewport and write mask. The
shader does no AA, sharpening, or gamma math. The sampler and vertex UV
alignment still matter, so do not treat it as a bit-exact CopyResource
alias without validation or replay. Current UI routing follows resource
copies only.

Further full-size writes at q1985/q1991 use A888/015 after those blits.
Do not ignore them or preserve a clean-image alias through an
unclassified write. The earlier bounded claim that the blit was the
terminal write was incomplete; ordered census review found these panel
draws afterward.

Native code permits both direct render-thread treatment and queued
Present-boundary work. Present clears the current UI frame state before
pumping queued work. Existing logs give the treatment thread but not the
Submit caller/path, so a previous/current-frame association error
remains an open hypothesis. The retained DEF19/831 binding alone cannot
prove it.

The terminal writes are indexed six-vertex A888D51024D9798E /
015EF9349EC097E8 composites over both final eye textures. They use
premultiplied-over RGB, preserve destination alpha, and have depth
disabled. PS t1 is a 259x154 interface surface; its retained frame-15055
payload is entirely zero. PS t0 is a 512x512 BC1 effect texture whose
pixels were not retained. The PS includes an eight-tap holographic smear
and material/tone terms. An empty t1 does not prove no visible write
from t0, so these draws must be preserved or replayed, never silently
ignored. The exact pair is recognized among final GUI canvases by
screen_motion, but A888 is also the game's general world-quad pipeline.

Sean had already closed Elite, so an AA Off/On control capture could not
test whether this is startup-only. The next diagnostic build will retain
bounded tone, sampled-blit, panel-tail, alias invalidation and native
submission-path events together. No rendering or frame-lifetime change
is justified by the evidence yet; first resolve which branch caused the
empty aliases.

The diagnostic implementation records UI frame generations, both pending
eyes at tone observation and preparation, alias creation/removal, the
exact sampled blit and late composite pairs, and the boundary that
clears the packets. Native submission reports its first direct and
queued routes with caller/render/owner thread IDs. On rejection,
observations continue through the failed frame and one following frame
without reactivating replay or changing the fallback. Event budgets
bound logging and stop diagnostic-only resource queries once exhausted.

The isolated controller passes 1,320 checks on both WARP and NVIDIA.
Synthetic cases cover preparation before tone, the controller's
BeforeDraw/BeforeTone/Begin/draw/End hook order, sampled-copy
destination nonmatching, alias invalidation, both candidate families
after failure, and diagnostic expiry after the following frame boundary.
These are controller tests, not a complete replay of the captured game
shaders or the full panel override pipeline. The native harness passes
622 checks with no runtime.

Full validation completed with SDK 310.7.0 explicitly verified and
Steam's original OpenVR DLL used to rebuild the legacy fixture. The
absolute build.bat returned 0 and reported DLSS runtime CARRIED and all
native OpenXR gates passed. Live NVIDIA NGX smoke against the final
graphics DLL returned 0, including DLAA/DLSS evaluation, crop, motion
and cost probes, and the jitter/motion convention rig (17.54 error for
the shipped pairing versus 19.43 runner-up). The final hardware
controller passed 1,320 checks. This does not claim a headset or D3D
debug-layer test.

Candidate version is `v0.17.0-rc.1-2-geeb1928-dirty`, built before its
source commit. Graphics PE `6AA9C287` links at 2026-09-15 22:11:19 UTC;
runtime PE `6AA9C363` at 22:14:59 UTC. SHA-256:

- Graphics:
  `627FBC149681154C750C0C996E2557AE0142DE027C798C7B32F3F13B94E49CE2`
- Runtime:
  `4CEB42A4AF383D4EB398765549EC547D319FECB91804E22F31E2763FF53C4C41`

Validation logs are under `build/review_motion/issue36/`:
`build-ui-route-154217.log`, `smoke-ui-route-154217-live-ngx.log`, and
`controller-ui-route-154217-hardware.log`. This build only adds
diagnostics; the next flight must establish the tone/submission ordering
before a rendering correction is proposed.

## Missing tone hook, 174541 (2026-09-15)

The diagnostic build ran: Steam package verification and both DLL hashes
match the candidate recorded above. GFX log
`edvr_gfx_20260915_174305.log` carries PE `6AA9C287` and exact version
`v0.17.0-rc.1-2-geeb1928-dirty`; the matching native log is
`edvr_openxr_20260915_174307_292_21772.log`. HEAD comparison was checked
first, then the known precommit stamp and installed hashes resolved the
expected mismatch. No stale-DLL inference is used.

At 17:44:38.314/.316 generation 8583 captures eleven 8121/A296 draws per
eye into HDR resources `00000283852A0060` and `00000283852A6BA0`. At
.318 the exact 20F/DED sampled blits read `00000283852A0320` and
`00000283852A73E0` and write final resources `000002838529E7A0` and
`000002838529ADE0`. Both A888/015 composites follow into those same
final resources. At .319 prepare receives `000002838529E7A0`, with both
tone aliases empty, and disables replay. Only at .320 does the frame
boundary clear the pending captures. Generation 8584 still emits both
candidate families before the diagnostic window expires.

Neither generation emits an expected-tone observation, alias addition,
or alias removal. This distinguishes failure to enter tone processing
from failure after a successful tone capture. The later dump's
`tonemap_174541.bin` has two draws, zero declines and zero failures,
matching frame 14108. A recorded tone pass later in the flight alone
cannot reconstruct startup, but the missing hook observation now has a
specific code path to investigate.

The native trace reports `path=direct`, caller/render thread 20812 and
XR owner thread 25348; no queued submission path is reported. Shutdown
reports `graphics_wrong_thread=0`.

Ruled out: early submit or a preceding Present clear caused the empty
tone aliases in this run, because final copies/composites precede
prepare and the frame boundary follows it on the direct route.

Ruled out: a recognized tone alias was erased by a later draw, because
there is no tone observation or alias creation before rejection or in
the following diagnostic frame.

Root inspected converted overview, T15 and C15. T15 has dark shapes
above the target circle and label against the corona; the raw C15 image
does not have those same broad shapes. This confirms the reported
temporal artifact while replay remains inactive, not a defect in the
as-yet-unused native UI composition. Review PNGs are under
`build/review_motion/issue36/eye_174541_*_review.png`.

Code audit found no ordinary same-draw shader replacement for the known
2D78/99C2 tone pair between the draw snapshot and BeforeTone. The FSS
dump handler's tone case does not alter bindings; the terrain original
shader helper only matches its named terrain shader. Earlier wrapper
returns remain a possibility, and a later snapshot is not proof that the
same producer ran during startup.

The remaining diagnostic blind spot is the actual writer of the sampled
source. The current expected-tone observation is silent before any UI
capture unless the post-failure diagnostic window is active. Record a
bounded resource writer history and forward-path outcome so one trace
can distinguish an alternate producer, an earlier-generation source, a
tone before UI capture, and an original draw skipped by a wrapper. Do
not infer any of these solely from an unobserved tone pair.

The added diagnostic keeps 24 draw records with retained resource and
shader identities. Each records generation, draw ordinal, original
shape/verdict, shadow and actual shaders at entry and before BeforeTone,
and whether the original draw was issued. It rolls silently before UI
capture, so an early tone is not lost merely because no UI was pending.
The existing sampled-blit report identifies the newest observed attempt
and newest observed issued draw separately. Known resource-write hooks
invalidate matching older records; unobserved writes remain a
limitation, so the log deliberately describes observed draws, not a
guaranteed last writer of every possible resource operation.

Cached eye-target/format/depth information filters candidates before
direct context queries. The observer accepts the game's typeless RGBA
resource viewed through an UNORM RTV; it performs no GPU readback. After
failure and the following diagnostic frame, records are cleared and
target/shader queries stop. Up to eight unique producer stage/hash pairs
can be saved from existing shader-private bytecode into the log
directory's `shaders` folder. Existing, missing and failed exports are
reported explicitly. This is diagnostic-only; the unsupported sampled
copy and terminal canvas route is not silently accepted or bypassed.

Focused validation passes 1,620 controller checks on WARP and NVIDIA.
Tests include pre-capture tone history, actual-versus-shadow shader
fields, skipped and issued draw outcomes, known-write invalidation,
history eviction, typeless resources with UNORM views, diagnostic query
expiry, byte-exact producer shader export, repeat/cap behavior, and
missing-bytecode reporting. Shader tests create and remove their own
scratch files. The synthetic harness verifies the controller's staged
calls and real D3D resources; it does not instantiate the whole game
classification pipeline. Production draw-wrapper callsites were reviewed
separately.

Final validation passed the absolute full build with SDK 310.7.0
verified, the Steam original OpenVR fixture rebuilt, DLSS runtime
CARRIED, and all gates passed. Live NVIDIA NGX smoke against the final
graphics DLL exercised DLAA and DLSS, including the convention rig
(17.54 error for the shipped pairing versus 19.43 runner-up), and passed
without runtime skips. The final hardware controller passed 1,620
checks. This is not a headset or D3D debug-layer validation.

Candidate version is `v0.17.0-rc.1-3-g3b6715a-dirty`, built before the
subsequent source commit. Graphics PE `6AA9DFA6` links at 2026-09-16
00:15:34 UTC; runtime PE `6AA9E097` at 00:19:35 UTC. SHA-256:

- Graphics:
  `6A705C8B748E4EE24923D4A160B7F924D3B893EA49B01830484CE2CF2142B1F9`
- Runtime:
  `9B293B6DF29ACFD21E179A080C112299129B5983A685E17A77EC999FF02171D7`

Validation logs under `build/review_motion/issue36/` are
`build-producer-174541.log`, `smoke-producer-174541.log`, and
`controller-hardware-producer-174541.log`. The next flight must use the
new producer evidence to resolve the missing tone path before changing
rendering behavior.

## Observed tone variant, 183301 — 2026-09-15

Steam run `edvr_gfx_20260915_183004.log` and the native OpenXR log
`edvr_openxr_20260915_183005_354_5428.log` identify the tested fc32749
package. The expected HEAD/precommit version difference was checked
first, then resolved against the exact stamp, PE and hashes recorded
above; installer verification passed. This is the same Pimax OpenXR /
Crystal Super, 2644x2610 input and 4068x4016 output, preset K, DLSS
310.7.0.0 environment. Native submission uses the direct route; shutdown
reports `graphics_wrong_thread=0`.

At 18:32:15.693, generation 5797, the sampled-copy report identifies
ordinal 540 as the observed original-issued producer of eye 0's source
`0000012D814306A0`. Entry, actual entry, BeforeTone and actual
BeforeTone all name VS `642017A6FEDAE0E8` / PS `99C21CEB7A699821`,
N3/i1, verdict 0. Eye 1 repeats this at ordinal 545 for
`0000012D81435920`. Both have `before-tone=1` and `original-issued=1`.
Generation 5798 repeats the same pair at ordinals 565 and 570. The
controller's exact VS gate only accepts `2D78DC3FD2C0C543`, so it
rejects the producer before capturing the tone pass. The submitted final
targets are `0000012D81448D20` and `0000012D814487A0`, after the known
sampled blit and terminal GUI.

Ruled out: a same-draw wrapper changes or skips the tone producer,
because actual and shadow shaders agree at both observation points,
BeforeTone is reached, and the original draw is issued in both frames.

Independent shader inspection finds identical geometry and input/output
signatures for the two tone vertex shaders. Exposure differs: VS 642
reads CB2[2].y, while VS 2D78 samples VS t0/s0. The shared pixel shader
consumes that scalar. Exact-pair recognition must retain each original
shader and its reflected resources, not substitute one exposure path for
the other.

The later frame 9859 census has two unique tone draws (#261 and #263)
using the already-known 2D78 variant. Their HDR inputs and LDR outputs
match the same eye resources; the tone snapshots correspond to that
later instance. They do not contradict or supply the exposure state for
the earlier 642 producer. Startup failure latches the old controller off
before this later valid variant is reached.

The implementation review covers all three evidenced stages together:
variant-specific tone capture, exact sampled-blit replay, and bounded
native replay of terminal GUI canvases. The clean DLSS input must remain
world-only. Unsupported state must preserve original game colour. No new
flight is requested to rediscover the already-recorded copy or
terminal-canvas rejection.

The 20F/DED copy has N4/i1 triangle-strip geometry, an 80-byte vertex
buffer at stride 20, same-size typeless RGBA8 textures viewed as UNORM,
full viewport, no DSV, depth/stencil/blend off, and all RGBA channels
writable. Its shaders pass through UV and sample t0/s0. Exact live
geometry and sampler state must be replayed; an identity CopyResource is
not established by the retained shader alone.

The A888/015 terminal canvas is X6/i1 with per-eye/per-frame changing
startInstance and constant buffers. It uses premultiplied ONE /
INV_SRC_ALPHA blending, RGB-only writes, disabled depth and stencil
ALWAYS / REPLACE with ref 4, read mask 0 and write mask 4. Its live
packet must preserve geometry, CBs, structured resources and samplers.
The latest census identifies DSVs @1479/@1490 as resource fmt 19, view
fmt 20: R32G8X24_TYPELESS / D32_FLOAT_S8X24_UINT. A D32_FLOAT-only
validation would reject this observed route. The original game draw
retains its stencil side effects; native replay does not need a DSV
because this verified terminal draw cannot test depth or stencil. An
all-zero retained t1 does not prove invisibility: the shader also uses
t0 and material state. The retained files lack enough of those inputs
for a faithful complete offline replay of this actual draw.

Ruled out: the terminal LDR projection is unjittered, because its
retained VS CB0 offsets across frames 9859-9873 follow the exact
eight-frame Halton cycle. The measured deltas are +2*jx/2644 in x and
-2*jy/2610 in y. Draw 9859 matches temporal frame 9858's (+0.25,
-0.3888889), yielding (+0.00018911, +0.00029800); the diagnostic frame
labels are offset by one. Scaling the viewport alone would scale the
jitter into native output. Tail replay needs the same viewport
translation as the established HDR UI replay: -jx*outWidth/inWidth and
-jy*outHeight/inHeight, with consistent scissor placement. Keep the
captured matrices themselves intact.

The final resource audit finds another deterministic rejection before
any flight: the terminal VS t38 is census @51 / resource
`0000012D81301220`. Its complete descriptor in `panels_183301.bin` is
`[8388624,0,136,0,64,48]`: 8,388,624 bytes, SRV|UAV bind flags 0x88,
structured flag and stride 48. The snapshot pool currently rejects every
UAV-capable buffer. Terminal t33 (@50) instead has descriptor
`[688128,2,8,65536,64,336]` and is already supported. The new route must
snapshot t38 into a private SRV-only structured buffer, preserving
stride and contents. A UAV-capable source needs a fresh copy per capture
because tracked compute dispatches do not prove that all possible
graphics UAV writes were observed. Keep the 256 MiB snapshot budget and
reject stream-output resources.

The active-route integration audit finds a second handoff conflict in
`temporal_pass.cpp`. On the later 2D78 tone frames, legacy separation
and deferred replay can both complete. Existing code chooses the legacy
separated colour for NGX even after copying the more complete deferred
world image into dlColour. It then runs legacy UI resolve into dlSubmit,
which the recorded deferred command list samples as its world input.
Thus the legacy result can contaminate the deferred composition.
Successful deferred preparation must exclusively own the clean input and
final UI output; failure must retain the existing legacy path. Ownership
changes also need a history reset: a single boolean saying "some
separation is active" cannot distinguish the two clean images. Do not
advance legacy UI influence history while deferred replay owns the
output, or enable the old adaptive colour work by accidentally treating
skipped legacy resolve as unavailable resolve.

Before final validation, origin/main advanced to b12122a. The worktree
fast-forwarded without conflicts, retaining unrelated uncommitted
landing-HUD notes. The final build must include its native-device
loading fix and build/shader-cache updates as well as this UI work.

Implementation review preserved the direct tone-only route and added
only the evidenced sampled-copy/terminal route. The sampled packet maps
native tone-base, tone-with-UI and scalar transmission consistently;
terminal canvases are drawn after their combination with reconstructed
world colour. Composite storage supports RTV and UAV together, avoiding
an additional full-frame copy for the terminal pass. Mapped resources
are allocated only when that route is used. Tail capture is bounded to
eight packets per eye; the existing 256 MiB snapshot budget remains.

The original wrapper must have verdict kNone for these new route draws.
A per-draw handled token suppresses the old UI-depth reissue without
skipping its cleanup. Snapshot/allocation exceptions, unsupported state,
duplicate sampled copies and tail-cap failures retain original colour.
Captured packets and aliases reset at each frame boundary. UAV-capable
tail SRVs use fresh copies, preserve structured stride and remove only
the UAV bind flag from the private buffer; ordinary snapshots retain
their original bind capabilities.

Focused validation passes 2,040 checks on WARP and NVIDIA hardware,
including both tone-pair gates, unknown-variant rejection, a nonidentity
sampled mapping, transmission placement, jitter-free native tail pixels,
original-image preservation, mutated CB/texture/structured-buffer
contents, verdict refusal, the tail cap and existing direct-route
fallback tests. The exact-size 8 MiB structured-buffer test mutates its
float value to 10.0 through a UAV after capture, which would visibly
shift geometry by 0.5 NDC if replay used the changed data. A separate
readback test verifies two fresh snapshots without write-hook evidence.

The RTX 5090 synthetic sampled/tail benchmark at 2644x2610 input and
4068x4016 output measures 0.5723 ms GPU and 0.0450 ms prepare CPU per
eye over 20 samples, with 9.00 MiB of synthetic snapshot storage. It
uses one HDR UI draw, one sampled mapping and one terminal canvas. It
excludes DLSS, game rendering, pre-tone work and the actual game
materials, so it does not establish total flight cost or headset visual
quality.

Final validation passed the absolute full build with SDK 310.7.0
verified, the Steam original OpenVR fixture rebuilt (nine thunks, five
wrapped), DLSS runtime CARRIED and every gate passed. Live NVIDIA smoke
against the final graphics binary passed without runtime skips and
exercised DLAA, DLSS and the convention rig: the shipped pairing scored
17.54 versus 19.43 for the runner-up. The final hardware controller
passed 2,040 checks. These are GPU/controller checks, not a headset
flight or D3D debug-layer validation.

The final RTX 5090 route benchmark measured 0.5589 ms GPU and 0.0501 ms
prepare CPU per eye over 20 samples, with the same synthetic workload
and exclusions stated above. Logs under `build/review_motion/issue36/`:
`build-route-183301.log`, `smoke-route-183301.log`,
`controller-hardware-route-183301.log`, `benchmark-route-183301.log`.

Candidate version `v0.17.0-rc.2-dirty` was built on b12122a before the
subsequent source commit. Graphics PE `6AA9EC43` links at 2026-09-16
01:09:23 UTC; runtime PE `6AA9ECE2` at 01:12:02 UTC. SHA-256:

- Graphics (d3d11.dll and edvr_openxr_graphics.dll):
  `C1E5996F4AA4963813E572E5342D694AEF7E55B8FBB6B91C3F1DB510BC0A1F0F`
- Runtime (openvr_api.dll and edvr_openxr_runtime.dll):
  `D32AE9111AE2FF4E6171C0734AF429EE625E815804E6773BA9BE069894EBB9B4`

The sanctioned Steam install dry run passed and wrote nothing. It
selected the native OpenXR pair, loader and available DLSS payload,
confirmed Elite was stopped, and retained the existing INI. The
pre-install INI hash is
`BDF474BB2A929773FE66AADC790549FA59714830EB9A9FEE5EB4CDD06F45BF9B`.

Source fix e97e2fe was merged with main's documentation-only a82a19a as
96902e6 and pushed; the additional merge did not change compiled
sources. The sanctioned Steam install then completed, and a separate
`--all --verify-only` passed. Installed graphics/runtime hashes match
the final build above. The original and final INI are both 144,372 bytes
and have the same 64-character hash recorded above. An extra trailing
character in the agent's initial prose report was corrected against its
original preflight tool output and a final read-only hash check.

Install receipt backup in the Steam game directory:
`edvr_native_receipt.json.pre-96902e6-20260915-192019.bak`.

## World shader rejection, 194533 — 2026-09-15

The user reports continued smearing. The first log check compared HEAD
and reported the expected precommit mismatch. The literal stamp and PE
match the installed package recorded above; the sanctioned installer
verification and exact graphics/runtime hashes confirm this is the
intended build. GFX log is `edvr_gfx_20260915_194316.log`; matching
native log is `edvr_openxr_20260915_194317_509_13804.log`. Runtime,
headset and eye sizes are unchanged, native submission remains direct,
and shutdown reports `graphics_wrong_thread=0`.

At 19:44:52.334, generation 10014, the first captured X draw is flight
HUD VS `B7790CBFC6554097` / PS `8DEF46452FA459F5`, eye 0 HDR
`000001F7055C2A60`. At .339 the interleaved world path rejects PS
`3EAF4DB5B3E21089` with `pixel shader fanout: unsupported opcode 77` and
latches the controller off. VS is `8C091FFD08644E02`; count remains one
and no native UI replay applies. The following 642/99 tone passes,
sampled copies and terminal canvases are observed only in disabled
diagnostic mode. Thus the repaired handoff is not exercised in this
flight.

Ruled out: the current eye dump shows an artifact while deferred native
UI replay is successfully active, because applied stays zero and the
first capture sequence aborts before tone mapping.

Eye stamp 194533 captures scene frame 13519, with crops C00..C15 from
13520..13535. All twelve eye inputs and the panel, tone, drawstate,
eye-mesh, GUI and draw snapshots are written; zero missing shader files
or snapshot declines are reported. This is distinct from the production
controller's rejection. The rejected 20,740-byte pixel shader is
retained as `shaders/ps_3EAF4DB5B3E21089.dxbc`.

Discriminating checks before another build:

- Confirm opcode 77 and every destination operand in the actual shader,
  including whether an output or null destination is present.
- Scan the relevant retained shader corpus through the real fanout
  transformer and D3D shader creation, rather than testing only that PS.
- Audit the ordered HDR/UI interval for later blend, stencil, resource
  or wrapper-state blockers which would disable the route next.
- Defer visual-composition hypotheses until a run actually records
  successful applied frames; sharpening or mask changes cannot repair a
  route that never runs.

The actual rejected PS disassembles to six `sincos r2.z, null, r2.z`
instructions and one `imul null, r3.y, r2.w, l(48)`. These are opcodes
77 and 38, with two destinations and SDK operand type 13 for NULL. The
469 retained pixel shaders parse structurally; the unsupported
arithmetic encountered consists of IMUL, SINCOS, UDIV and SWAPC. Two
instructions in retained PS `1B2C9D080A23CCE6` use `imul null, o0.xy,
...`: a parser which only redirects the first destination would silently
leave output writes unredirected.

The conservative ordered route contains sixteen distinct pixel shaders.
Eleven retained shaders already pass transformation and WARP creation;
3EAF is the arithmetic rejection. Four hashes have not yet been found in
the Steam retained corpus: `6EEF165A350DA30F`, `5E72F436FC8A5736`,
`2B156A05E98F2D5D`, `16196F69ADE35E77`. Prior local replay artifacts are
being checked before these are called unavailable.

The downstream A296 holo packets have separate retained resource
blockers. Reflected VS t38 is an 8,388,624-byte SRV/UAV buffer, while
the current opt-in applies only to terminal canvases. The IA stream uses
a 130,023,424-byte VB1; its retained draw-window bytes differ on all
twelve UI draws. Whole-allocation copies plus the 32 MiB index buffer
would exceed the shared 256 MiB budget by the second or third packet.
Merely allowing the UAV-backed SRV would not make this route usable. The
corrective contract must preserve draw addressing and resource versions
without synchronous index-buffer readback.

The decoder correction passed 2,236 controller checks on WARP and the
hardware GPU, including original/patched RGBA equality and clean-colour
and alpha-influence outputs. The actual 3EAF program transforms from
20,740 to 20,872 bytes and D3D creates the result. Of 469 retained pixel
shaders, 320 applicable programs transform and create successfully; 143
output-signature and six early-return cases retain the previous safety
declines. There are no opcode or destination-cursor failures. The twelve
available route shaders all pass. This focused result does not yet
validate the resource changes or establish successful in-game replay.
Logs: `controller-fanout-warp-194533.log`,
`controller-fanout-hardware-194533.log`, `fanout-3EAF-fixed-194533.log`,
`fanout-route16-fixed.csv`, `fanout-corpus-fixed.csv` under
`build/review_motion/issue36/`.

The retained A296 layout proves VB1 is per-vertex, with stride 40 and
16-bit indices; VB0 is an eight-byte, step-one instance stream. The VS
has no vertex/instance system-ID input. The bounded capture therefore
copies the complete 65,536-entry index domain from BaseVertexLocation,
the exact index window and the selected instance, preserving every
possible fetched byte while rebasing the replay arguments. It does not
infer the highest index from a partial dump. The policy is restricted to
the proven shader pair and exact reflected layout, with checked
arithmetic and fail-closed bounds. Shader-readable UAV snapshots must
advance on graphics and compute writes; identical retained t38 bytes
alone are insufficient proof that a later flight cannot update them.

Main was fast-forwarded to 749a4e9 before final validation. Its temporal
region timing changes do not overlap the capture/decoder patch and will
be included in the full build and live NGX smoke.

All four missing PS programs were recovered from the Steam installed
`Win64/EffectsBinary/Effects2_Win64_SM50.arc`. Its 13,109,377-byte file
contains raw-deflate data beginning at byte 16, expanding to 71,115,408
bytes with 1,831 DXBC containers. Exact whole-bytecode FNV identities
match the recovered files: 6EEF is 1,804 bytes, 5E72 is 2,784, 2B156 is
1,400, and 16196 is 1,448. Artifacts are under
`build/review_motion/issue36/deferred-ui-shaders/recovered-route-ps/`.
An initial repository search compared DXBC header checksums, which is
not the production identity algorithm. That search was redone using FNV
and the known A296 blob as a control before archive recovery; checksum
matching is not counted as evidence of shader-hash absence.

All sixteen route programs now transform and D3D-create successfully.
Final focused controller validation passes 2,251 checks on WARP and
hardware, including actual CS and OM write callbacks producing distinct
retained t38 versions. Compact-IA replay matches a nonblank original
image byte for byte with nonzero base, start-index and start-instance
arguments. Mutating the source leaves older packets intact; changed
layouts and system-ID inputs are rejected. The 24-draw stereo stress
case copies and allocates 60.00 MiB, within the existing 256 MiB budget,
and reuses storage across frames with different index counts.

The synthetic 24-draw IA capture benchmark measures CPU-plus-GPU wall
clock including completion, not isolated GPU time: RTX 5090 cold 1.593
ms, warm 0.555 ms; WARP cold 15.360 ms, warm 15.367 ms. It excludes
DLSS, UI shading/composition and the game's rendering, and is not a
claim about headset frame time. Shader reflection is cached per shader
pair, rather than repeated for every label.

Startup B779 was not present in the retained steady-state packets. Its
exact stencil effect before orbital 6EEF cannot be proven from these
dumps. The existing first-twelve capture log now includes depth
enable/write and stencil enable/read/write/reference plus both face
functions and operations. An unsupported stencil dependency still
retains the original frame and declines; the guard was not weakened.
This remains a validation limit to inspect in the next flight.

Main advanced again to 377c520 with concurrent build-test orchestration.
It was fast-forwarded before the final build, without overlapping the
five changed production/test files. Source freeze began after the
focused checks above; full build and live NGX validation use this merged
baseline.

The first full build compiled the DLLs but stopped at the newly merged
runner with `the script defines no :rig_<label> subroutines`.
`build.bat` saves ROOT before parsing arguments, but `shift` advances
`%0`; the later `--script "%~f0"` therefore names the last `--openvr`
argument rather than the batch file. The unchanged script already
supports inherited `OPENVR_SRC`, so final validation is rerun with that
variable set to the verified Steam original and no CLI arguments. This
runs the same gates; it does not skip the concurrent runner. The
launcher implementation is outside this rendering patch.

The environment-path retry reached the concurrent rigs but failed seven
`openxr_module_test` lifecycle/timing assertions under load. Its exact
six-command gate then passed in isolation from the same binary
(`openxr-module-standalone.log`, including the 28-check local test).
Final full validation therefore uses inherited `EDVR_JOBS=1` to run the
unchanged 68 jobs serially. The failing build logs are retained
separately; no failing gate is treated as passed or removed.

Final validation passed using the unchanged serial runner: all 65 pool
jobs plus three quiet jobs, SDK 310.7 verified, config 255/255, DLSS
runtime CARRIED and installer resources matching the release files. The
previously failing module test reports 247 checks and zero failures.
Live smoke against the final graphics DLL passes with DLAA available,
eleven evaluations and two resets, live crop/motion/cost/fovea/price
probes, zero drops and no runtime skips. The shipped motion/jitter
convention scores 17.54 versus the runner-up 19.43.

The final hardware controller passes 2,251 checks and the draw-replay
suite passes. Its 24-draw compact-IA workload still uses 60.00 MiB; the
final run measures cold 2.010 ms and warm 1.169 ms CPU-plus-GPU wall
clock. The variation from the earlier 0.555 ms warm result is reported
rather than selecting only the faster result. Neither is isolated GPU
time or total flight cost. No additional source changes were made after
the final build.

Final logs under `build/review_motion/issue36/`:
`fullbuild-compact-panel-serial.log`,
`smoke-compact-panel-live-ngx.log`, and `deferred-hardware-final.log`.
Version is `v0.17.0-rc.2-6-g377c520-dirty`. Exact final binary
provenance:

- Graphics (`d3d11.dll` and identical `edvr_openxr_graphics.dll`): PE
  `6AA9FDBA`, link 2026-09-16 02:23:54 UTC, SHA-256
  `AC580E3A2FDA8AAA6F23E6B248286A50CCF3FE18A32379017B54052740887158`.
- Runtime: PE `6AA9FDBF`, link 2026-09-16 02:23:59 UTC, SHA-256
  `9AA88D3B90BD015BA7891584006AFB454D2177C6F5AA41A9B6CA70E93C2FF19B`.

Steam dry-run installation passed and wrote nothing. The subsequent
sanctioned `--all` install and separate `--all --verify-only` both
exited zero. Installed graphics/runtime hashes match the final build
above. The live INI was 144,372 bytes before and after, with unchanged
SHA-256
`BDF474BB2A929773FE66AADC790549FA59714830EB9A9FEE5EB4CDD06F45BF9B`. No
INI overwrite, forced install or process termination was requested.
Receipt backup:
`edvr_native_receipt.json.pre-377c520-20260915-203111.bak`.

Next flight: inspect target text near the corona, then confirm the log
records successful deferred application rather than the legacy fallback.
The offline results fix confirmed blockers; they do not establish the
headset result or eliminate the unretained startup-stencil limitation.

## Steam run 051237, 2026-09-16

The installed package was verified before the log was used. The latest
Steam graphics log, `edvr_gfx_20260916_050901.log`, reports literal
version `v0.17.0-rc.2-6-g377c520-dirty` and graphics PE `6AA9FDBA`.
Installer verification reports graphics SHA-256
`AC580E3A2FDA8AAA6F23E6B248286A50CCF3FE18A32379017B54052740887158` and
runtime SHA-256
`9AA88D3B90BD015BA7891584006AFB454D2177C6F5AA41A9B6CA70E93C2FF19B`. HEAD
`b327617` was checked first; its known precommit version mismatch was
resolved by the matching hashes and PE timestamp.

The matching `051237` eye dump is scene frame 21325. All twelve eye
inputs were written, twenty pool frames were retained, and zero copies
were skipped. Panel, tone-map, eye-draw, eye-mesh, target-colour, and
related snapshots were written with zero reported capture failures.

At 05:10:43.921, generation 11282 captured the B779/8DEF flight-HUD draw
at depth 1/0 with stencil disabled (`StencilEnable=0`). Its read and
write masks were both `FF`, its reference was 0, and its front and back
operations were `ALWAYS`/`KEEP`. A296 was a separate UI draw with
stencil enabled. After tone and post-tone mapping, both eyes reached
`complete=1 aliases=2 restored=1`. The route became active at
05:10:43.968, and totals reached `captured=37293 applied=2638 declined=0`
at 05:10:58.663.

At 05:11:02.853, VS `F512712C40D93C12` and PS `4A71EB0D34E9F2EF` hit the
enabled world-blend unsupported path: colour `2/16/1`, alpha `2/18/1`.
The shader is confirmed as world glass using dual-source blending. The
route latched off and fell back before the `051237` eye dump.

The log contains no explicit AA-toggle timestamp. The user reported that
the whole loading cockpit view was black, AA Off restored it
immediately, and re-enabling AA did not make it black again. Target text
still smeared afterward. Because fallback preceded the dump and the
later smear report, neither establishes active-route headset quality.
The cause of the startup black view remains unproven; clean HDR, tone,
and composite are the paths to investigate.

### Offline black-path diagnosis, 2026-09-16

The bounded GPU test reused the retained `054906` tone packet at frame 17574,
ordinal 304: real PS `99C21CEB7A699821`, LUT, HDR crop, vertex buffer,
samplers, blend, raster, viewport, and 272-byte CB2. It substituted real
constant-exposure VS `642017A6FEDAE0E8` for the packet's VS2D78 and bound that
retained CB2 to VS slot b2. WARP produced mean RGB 47.7855, maximum RGB 255,
and 28.375% fully black RGB pixels, versus captured expected mean 47.7836. RGB
MAE was 0.0297, maximum difference 2, with 8.545% of RGB pixels differing. This
rules out an inherently black 642/99C2 path for that retained state. It is not
an exact startup replay: `051237` did not retain the startup VS642 b2, and the
reused CB2 is a proxy from the earlier packet.

The exact post-tone bytecode is VS `20F383BBAC05C031` and PS
`DED8796049C7BB4A`. The VS writes the input UV unchanged and forwards the input
position. The PS performs only `o0 = t0.Sample(s0, uv)`; it has no constant
buffer, arithmetic, or discard. A constant-one R32 t0 therefore writes one to
the R32 target at every rasterized sample regardless of filtering. This does
not prove that every output pixel is rasterized or that border samples are one:
the actual four-vertex payload and sampler were not retained. The live route
captures and replays both, while its state gate requires an N4 triangle strip,
full viewport, no scissor/depth/stencil/blend/logic operation, and the full
colour write mask. No exact GPU replay of this draw was claimed.

An exact A888/015 tail GPU replay was also unavailable. The retained evidence
does not contain its geometry, layout, t33, t38, PS b1, t0, and samplers as one
complete packet. Shader disassembly and the live state contract do not replace
that missing packet, so the tail remains inside the `final` stage of the next
probe rather than being ruled out offline.

The current fanout rig checked all 36 pixel-shader hashes in the retained
`051237` HDR-target census. Twenty-nine whole DXBC containers now pass,
including four checks each for the recovered `4888F2B05460FA9B` at 2,528 bytes
and `702C3974A260DE14` at 85,384 bytes. Three remain absent from all 1,831
valid DXBC containers in the installed `Effects2_Win64_SM50.arc`:
`188A933094FB422A`, `869FFF43E875906E`, and `DBF1725726018F52`.

Production `fnv1a64` hashes the whole DXBC with offset basis
`1469598103934665603` and prime `1099511628211`. The canonical FNV-1a64 offset
basis is `14695981039346656037`; using it produces different names. Independent
whole-container hashing, archive-slice comparison, DXBC bounds, chunk, and
pixel-stage checks confirm the two recovered production hashes. Four retained
shaders correctly fail the strict single-output contract: `50364C9D994141D5`,
`7CECABDE34FFBE9E`, `81812EF97FB4A361`, and `C6E6E419DA9F6FAD`. Each already
declares `o0.xyz` and `o1.x`. Exact retained ordering places all four before
the first captured UI draw and shows no later world draw, so they did not enter
this active route. They remain a bounded risk if another scene reorders one
after UI capture.

The five-stage luma probe on main `16adaae` measures game submit, clean HDR,
DLSS input, DLSS output before UI, and final output. Its first implementation
copied `clean_hdr` immediately after the first UI seed. A later mirrored world
draw could modify `e.cleanHdr` after that staging copy, making the evidence
stale. The sample was moved to `prepare` after a complete route match and
before renderer seed/tone work, so it now measures the final clean HDR consumed
by replay. The other four stages already bracket the path correctly. This
instrumentation correction and the separate world-glass work are not yet a
built or flight-tested fix for the startup black view.

If the black view recurs, leave it visible for a few seconds before changing AA
so the probe can complete a round. Its first-black-stage line will separate the
clean snapshot, replay/format, NGX, apply/tail, and downstream VR paths. After
that sample, inspect target text near the corona.

### Exact dual-source glass replay, 2026-09-16

The F512/4A71 world-glass rejection now has a narrow replay route. It accepts
only the retained indexed-instanced triangle-list shape with one instance,
wrapper verdict `kNone`, exact shadow and actual VS `F512712C40D93C12` and PS
`4A71EB0D34E9F2EF`, no ancillary shader, stream output, predication, UAV, logic
operation, or partial sample mask, and the exact dual-source blend equation:
source `ONE`, destination `SRC1_COLOR` for colour and `SRC1_ALPHA` for alpha,
both `ADD`, with all channels writable.

The depth/stencil contract is also exact: depth enabled, comparison
`GREATER_EQUAL`, depth writes disabled; stencil read mask zero, write mask 4,
reference 4, front `ALWAYS/KEEP/KEEP/REPLACE`, and back
`ALWAYS/KEEP/KEEP/KEEP`. The original game draw retains its colour and depth
target. Only after the wrapper reports that the original draw was actually
issued does the same raw draw run once into clean HDR with the original DSV.
Depth cannot change because writes are disabled. Replacing the already-written
front stencil value with 4 again is idempotent and read-independent. An
abandoned or skipped original never replays.

An active counting query is an explicit refusal condition. Begin/End brackets
track counting query identities, including duplicates and bounded overflow,
without changing query results or adding a poll. This prevents the private draw
from changing an occlusion or statistics count. A successful replay increments
a runtime counter and emits at most four bounded `replayed dual-source glass
into clean HDR` lines; unsupported state retains the original fallback.

Focused validation is green: the deferred controller passes 2,326 checks on
WARP and 2,326 on hardware, including the exact nontrivial dual-source
equation, depth-pass and depth-fail pixels, unchanged original
colour/depth/stencil, state restoration, the `originalIssued` negative case,
counting-query refusal, wrapper-verdict refusal, and partial-sample-mask
refusal. The query/live-hook rig passes its dry run and 158 checks. The completed full build and live NGX smoke are recorded below. Startup black remains unproven and unfixed pending the five-stage luma
flight.

### Final build validation, 2026-09-16

The absolute serial build and live NVIDIA NGX smoke passed at HEAD `fc18b4d`.
The built literal version is `v0.17.0-rc.2-14-gfc18b4d-dirty`. All 65 pool rigs
passed, as did the quiet and package gates, 256/256 config checks, NGX SDK
310.7.0 verification, carried DLSS runtime, and exact installer-resource
comparison. The controller remains green at 2,326 WARP and 2,326 hardware
checks, and the query/live-hook rig at 158 checks. The two recovered shaders
each pass their four-check preflight, making the corpus result 29 passes, four
strict pre-UI dual-output rejections, and three absent blobs.

The full-build log is
`build/review_motion/issue36/fullbuild-ui-glass-fc18b4d-serial.log`; the live
smoke log is `build/review_motion/issue36/smoke-ui-glass-fc18b4d-live-ngx.log`.
All eight modified source/test hash rows were identical before and after the
build; only the freeze file's UTC header changed.

Graphics `d3d11.dll` and the identical `edvr_openxr_graphics.dll` are 4,480,512
bytes, PE `6AAA8106`, linked 2026-09-16 11:44:06 UTC, SHA-256
`53013B93A7568E10488ED389DBAA5E41667F7EFB803C0926351721244F90D274`. The runtime
is 584,704 bytes, PE `6AAA810C`, linked 2026-09-16 11:44:12 UTC, SHA-256
`11F40DDD0F4E26E22A0E5E17A11E6039BC7FA9E8C4EA3C89F904E2506868C9FD`. The
installer SHA-256 is
`5DDE4BB426D35E4644B2DA73328D07E60C5A55AE82C28591C991D02CD38B470C`.

The Steam `--all` dry run exited 0 and wrote nothing. The actual `--all`
install completed and verified, and a separate `--all --verify-only` verified
the native pair, loader, and config. Installed graphics and runtime bytes match
the hashes above. The live INI stayed 144,372 bytes with SHA-256
`BDF474BB2A929773FE66AADC790549FA59714830EB9A9FEE5EB4CDD06F45BF9B`. The prior
receipt was preserved as
`edvr_native_receipt.json.pre-fc18b4d-20260916-055133.bak` in the Steam game
folder.

This proves build, live-NGX, and installed-byte integrity, not the headset
result. Startup black remains unproven and unfixed pending the five-stage luma
flight. The literal build predates the final validation commit; future HEAD
checks must resolve that precommit version using the exact hashes and PE stamps
above.

### Steam run 055630, 2026-09-16

The user reported the cockpit black on loading before the smear test. The
flight log is `edvr_logs/edvr_gfx_20260916_055441.log`, literal
`v0.17.0-rc.2-14-gfc18b4d-dirty`, PE `6AAA8106`. The native log records Pimax
OpenXR and Crystal Super at `4068x4016` per eye, with `2644x2610` scene input
and DLSS `310.7.0.0`.

The retained eye capture is stamp `055630`, scene frame `10780`; the tone
snapshot begins at frame `10781`. From 05:56:26 through 05:56:35 both eyes'
`game` and `clean_hdr` were nonblack while `dlss_in`, `dlss_out`, and `final`
were zero. AA Off at 05:56:37.758 restored a nonblack final. AA On at 05:56:39
made the input zero again. At 05:56:41.228 the submitted route mismatched
(`VS=01C3B84C82172B56`, `PS=DED8796049C7BB4A`), and the original frame was
retained before fallback restored a nonblack frame.

The dump retained `eyes/eye_055630_MV.bin`, `Z.bin`, `UI.bin`, `Bias.bin`,
`SceneZ.bin`, `HoloCoverage.bin`, `MeshCoverage.bin`, `PrevZ.bin`,
`DlssBeforeUi.bin`, and `DlssColour.bin`, plus `pool/panels_055630.bin`,
`tonemap_055630.bin`, `drawstate_055630.bin`, `drawstate_055630.eyemesh.bin`,
`gui_055630.bin`, and the frame pool, instance, bones, aux, and draw ledger
files. Snapshot writes reported zero copy/capture failures and zero missing
shader files.

Offline WARP replay of the original tone shader/state was nonblack (MAE eye 0
`0.01258`, eye 1 `0.01128`, max `2`); clean pre-UI HDR original-tone mean was
`12.896`. Game C00 identity matched the tone expected crop and original copy
succeeded. This validates original shader/state only; it does not validate live
Deferred UI draw or snapshot clean-LDR replay.

Ruled out: NGX/headset as the first cause, because DLSS input was already zero.
Ruled out: CB offset `=1` as the explanation, because D3D11.1 offset
granularity is 256 B while physical original-tone B2 is 272 B. Open: private
passes between clean HDR and clean LDR/clean final. No new fix or test flight
is approved while retained data is audited.

### Production reproduction and staged input discovery, 2026-09-16

The latest flight provenance is gfx run `055441`, literal
`v0.17.0-rc.2-14-gfc18b4d-dirty`, PE `6AAA8106`; the retained eye dump is stamp
`055630`, scene frame `10780`, with tone snapshot frame `10781`. The production
`UiDeferredDraw+Snapshots` WARP reproduction uses the actual dump shader
resources.

A full-black overwrite of a magenta marker reproduces the black output while
the IA override remains unchanged. D3DReflect succeeds with `BoundResources=0`,
`maskcbVs=cbPs=0`, and `srvVs0=srvPs0=srvPs1=0`: the exact PS chunks are `ISGN
76`, `OSGN 44`, `SHEX 2440`, and the VS chunks are `ISGN 76`, `OSGN 112`, `SHEX
260`, with no RDEF. Packet getters for VS t0, PS b2, and LUT PS t0 all return
null. Samplers are retained unconditionally.

Restoring the three original resources produces `802880` nonblack pixels, mean
`3.651934`, across the full `2644x2610` image. Only a `1400x1400` HDR crop was
retained; post-tone faithfully preserves the restored result. This rules out
post-tone and IA missing coverage as the root cause because the production
reproduction is already black before those stages.

A bounded executable input-discovery patch and regression are staged. They are
not validated yet. No fix is implemented and no additional flight has been
requested while retained data is audited.

### Accepted declaration input fix, pre-build validation, 2026-09-16

Root review and independent review accepted the `src/d3d11/ui_deferred_draw.h`
declaration-input fix. Against the actual `055630` packet, the controller
passes 2,375 WARP and 2,375 hardware checks without overrides. Masks use
`cbVs=0`, `cbPs=4`, `vs0=1`, `ps0=1`, and `ps1=1`; the 24-byte CB2 exposure is
272 B and the LUT exposure is 16,384 B, byte-exact. WARP output is mean
`3.651934` with `802880` nonblack pixels; hardware is mean `3.652029` with
`793418` nonblack pixels over `2644x2610`. Post-tone preserves both; the HDR
capture is partial.

The accepted SM5.0 declaration handling uses fixed slots CB<14, SRV<128, and
sampler<16, accepts dynamic indexing within a fixed CB, unknown-size (size 0) CB declarations, and raw/structured forms including
t33/t38, and declines malformed or unsupported declarations. Valid RDEF is
optional validation only; signatures and reflection are unchanged. The test
strips RDEF, proves the BoundResources=0 path, captures resources, mutates the
originals before replay, and covers bounds, empty, and unsupported forms.

The gfx provenance is run `055441`, literal `v0.17.0-rc.2-14-gfc18b4d-dirty`,
PE `6AAA8106`; the eye dump is `055630`. Frozen header SHA-256 is
`184B78CCF269D8FCDAFD8AEFE8A5F1898A88BB5D45ECB4D7246B7577881A5D1D`; test
SHA-256 is `22EFB3D8186C322CBB903053325D22C3FC48E8B90439F4E31DE048D3970549B7`.
Full build and live NGX smoke have started; no rebuilt package is installed.

### Final build, smoke, and Steam install, 2026-09-16

The full build and live NVIDIA NGX smoke passed. All 65 rigs, three quiet
gates, 256/256 config checks, SDK `310.7.0`, carried runtime, and actual
installer resource checks passed. Native NVIDIA exercises reported zero drops
and no runtime skips. Logs are
`build/review_motion/issue36/fullbuild-ui-inputs-9d30ac6-serial.log` and
`build/review_motion/issue36/smoke-ui-inputs-9d30ac6-live-ngx.log`. The final
literal is `v0.17.0-rc.2-15-g9d30ac6-dirty`; graphics linked at 12:27:17 UTC,
runtime at 12:27:22 UTC, and installer at 12:29:39 UTC.

Graphics SHA-256 is
`9714E5DD320FFAAB6793A45666870716198A332FEEE561BB7FB3797849DC00BE`; runtime
SHA-256 is `DCE75E4E6D26275374782CB2B9211EFFEC4C07EE6AD25F644DF620C04EEC4A1D`;
installer SHA-256 is
`85FBE587D549F65D2E1FA0ED79260E9E704CE3D3BF08B96BFCAC7472B45E86E0`.

The sanctioned Steam `--all --dry-run` and actual `--all` both exited 0. A
separate `--all --verify-only` exited 0 and verified the native pair, loader,
and config. The game was closed before installation. `edvr.ini` remained
144,372 bytes with SHA-256
`BDF474BB2A929773FE66AADC790549FA59714830EB9A9FEE5EB4CDD06F45BF9B`. The
preserved receipt is
`edvr_native_receipt.json.pre-9d30ac6-20260916-063210.bak`.

This validates build, smoke, and installed bytes; it does not claim HMD-flight
verification. The next check is loading cockpit with DLSS enabled.

### User confirms black fixed; target-smear flight 131013, 2026-09-16

The user confirmed the black cockpit is fixed and resumed checking smear around
target text. The latest verified gfx log is
`edvr_logs/edvr_gfx_20260916_130708.log`, version
`v0.17.0-rc.2-15-g9d30ac6-dirty`, PE `6AAA8B25`; current HEAD is `2c6e820`. The
literal is known precommit provenance reconciled by exact recorded installed
hashes and fresh Steam `--all --verify-only`. The OpenXR log records Pimax
OpenXR / Crystal Super, output `5424x5356` per eye.

The eye/offscreen census requested at 13:10:13.719 wrote eye stamp `131013`,
scene frame `14111`. Eye inputs are `2712x2678`; `DlssBeforeUi` is `5424x5356`.
The route-specific sequence is 13:08:30.189: `Deferred UI: captured X draw,
gen=6669 VS=B7790CBFC6554097 PS=8DEF46452FA459F5 eye=0 3525x3481
HDR=000001D702D1A560; depth=1/0 stencil=0 read=FF write=FF ref=0 front=8/1/1/1
back=8/1/1/1.` At 13:08:30.195, the next draw latched the route off: `original
frame retained: UI state, resources or allocation unsupported (draws=1,
VS=81216C77F90DEDD6 PS=A2965EC2931A39C8)`. The shader snapshot was unavailable.
Deferred totals through 13:08:28 were `captured=0 applied=0 declined=0`; the
log has no later totals summary. No active-route capture/evaluation occurred
for this eye dump, so it records fallback smear only.

The input files written include MV, Z, UI, Bias, SceneZ, HoloCoverage, UiEdits,
MeshCoverage, PrevZ, DlssBeforeUi, UiPrevious, and UiNext. Motion CSV, paired
raw/treated crops, and mesh/holo motion records were written. Snapshot reports
show zero declines, readback/capture failures, failed copies, capped draws, or
missing shaders for panels, tone-map, drawstate, eye mesh, GUI, and
target-color artifacts. Target-color snapshots contain six exact draws and six
completed before/after pairs.

Around the eye capture (13:10:15.669–19.875), both eyes are nonblack at game;
DLSS output/final track game, while clean_hdr and dlss_in are unsampled. At
13:10:21.910/.932 and 13:10:23.934/.966 both eyes read all-zero at game, with
DLSS output/final zero and Deferred UI inactive. First-black-stage lines name
game at startup 13:07:14.328/.389 and again at 13:10:21.910/.932.

The user-described black fix remains visually confirmed, but this flight does
not test the active Deferred UI path. The user's overview shows black trails
above UNIDENTIFIED SIGNAL SOURCE near the corona. Investigation is underway on
exact resource formats, budget, and state. The discriminating records are PS
t0/t1/t2 descriptor and imageSize, cumulative snapshot budget, and actual
captured fields; no cause or new fix is established yet.

### First rejected PS input identified, 2026-09-16

The retained target-smear run used gfx log `edvr_gfx_20260916_130708.log`,
literal `v0.17.0-rc.2-15-g9d30ac6-dirty`, PE `6AAA8B25`; its latest eye dump is
`131013`, frame `14111`. Root has fast-forwarded the branch to `ff88cd1`;
flight provenance remains the verified precommit binary.

Panels artifact `panels_131013.bin`, draw 0 (PS hash A296), identifies the
first PS t0 input descriptor as resource format `R16G16B16A16_TYPELESS` (enum
9), SRV format `R16G16B16A16_UNORM` (enum 11),
`1536x512`, default usage, one sample, bind flags `0x88` (SRV|UAV),
`6,291,456` bytes. The current `formatSize` table does not recognize resource
enum 9, making `imageSize` return zero and causing capture rejection before
budget or allocation checks. This rules out snapshot budget as the reason for
this specific rejection; it does not rule out later memory-footprint limits.

Other first-PS inputs are supported. Review is checking remaining descriptors,
frame footprint, and state. A narrow typeless/UNORM 8-byte support change with
exact-descriptor replay and version tests is underway; it is not validated yet.

### Independent retained-resource footprint audit, 2026-09-16

Independent review validates all 12 retained A296 draws and their input
resource descriptors. For A296 PS t0, storage is format 9 typeless and the SRV
is format 11 UNORM, fixed at 6 MiB (`1536x512`, 8 bytes per pixel). PS t1 is
supported BC1 typeless at 174,776 bytes. Five PS t2 inputs are RGBA8, totaling
12.3047 MiB. VS t33 is 688,128 bytes and t38 is 8,388,624 bytes.

The unique retained full-input footprint is 30.50 MiB for one eye. A
conservative stereo estimate without sharing is 61 MiB at Performance input
`2712x2678`. The first rejection occurred earlier on Quality input `3525x3481`;
startup B779 descriptors were unavailable, so no complete startup-route
footprint or total budget conclusion is established. The prior `051237` 186.5
MiB figure is not a valid complete-input ceiling because that build did not
contain the executable-declaration parser. Snapshot budget is ruled out only as
the cause of the format-9-specific `imageSize=0` rejection before allocation;
later memory limits remain open.

### Focused descriptor-support validation, 2026-09-16

Root fast-forwarded to `e4b923a` before the full build, including native-frame
sizing and per-headset trimming. The three-file `ui_deferred.cpp`, header, and
controller-test change supports 8-byte formats 9 and 11. The 256 MiB capture
cap remains unchanged. Refusal diagnostics now include stage/slot and
allocated/copied byte counts through the existing maximum-32-entry ledger.

Root and Sol reviewer reported no findings. The focused controller suite passes
2,407 WARP and 2,407 hardware checks. Tests cover the exact descriptor and 6
MiB resource, replay against two immutable version descriptions, and an
unsupported PS t5 diagnostic. Full build and live NGX smoke are still underway
under the GPU-owner's control; these focused passes do not replace those gates.

### Takeover review: flight 133757, dump 134857, stage attribution, 2026-09-16 evening

Build and flight. The three-file change above was built from `e4b923a` with
the dirty tree as `v0.17.0-rc.2-39-ge4b923a-dirty` (build `6AAAEE18`, linked
19:29:28 UTC), installed to Steam and flown; the gfx log is
`edvr_gfx_20260916_133757.log`. The change itself was carried into the
`claude/star-corona-ui-smearing-d1d2ae` worktree with this entry.

Deferred UI on that flight. Totals stayed `captured=0 applied=0 declined=0`
from 13:38 to 13:47. At 13:47:35.677 ui depth compiled the flight-HUD family
(B779/8DEF) and at .682 the interface-coverage family (8121/A296); at .678 the
route captured the X draw of B779/8DEF (gen 11297, 2535x2503) and at .683
refused the 8121/A296 draw: `panel vertex buffer contract changed
(allocated=26192756 copied=26192756)`, then `disabled until AA is switched Off
and back on`. The contract (`capturePanelIa`, ui_deferred_draw.h) requires
vertex buffer 1 to be exactly 130,023,424 bytes and buffer 0 exactly 32,768
bytes; the flight's buffer was not. So the format-9 change worked as far as
it went and the next exact-match contract took over. Sean was changing the
per-headset fov trims during this flight (13:47:37 to 13:47:47) and the frame
shrank to 1695x1614 at 13:47:58; NVIDIA kept evaluating throughout (dlaa
totals rose about 175 eye-frames a second through 13:48:38), so dump `134857`
(13:48:57, frame 18586, `DlssBeforeUi` 3390x3228) is a DLSS Performance dump
like `131013`.

Dump reading, method. Session scripts (eye_view.py, probe.py, stage_diff.py,
footprint.py) decoded the `EDVRTEX1` inputs and aligned the crops: C crops are
1400x1400 about the input's centre, T crops 2800x2800 about the output's
centre, `L0` the whole treated eye, and `DlssBeforeUi` carries the same frame
id as T00 (14111 and 18586). Regions are 480x360 output pixels around the
label: 131013 at (3150, 2320), UNIDENTIFIED SIGNAL SOURCE at 2.00 Ls; 134857
at (1900, 1350), GRAF LED ZEPPELIN GPL VNY-99Z at 6.53 Ls. Bands are the
class-1 coverage from `UI.bin` dilated by 10, 30 and 60 output pixels; class 3
(the star limb the corona is filed under as smoke) is kept apart. Luma is
0..255 of the 8-bit frames; raw is C00 bilinearly upscaled.

131013, mean luma per band (n, raw, dlss, final, dlss-raw, final-dlss):
class-1 coverage 30696, 47.0, 48.1, 47.0, +1.10, -1.11; ring 1-10 px 19168,
15.1, 17.1, 15.4, +2.03, -1.70; ring 11-30 px 29944, 16.2, 19.8, 17.7, +3.56,
-2.10; ring 31-60 px 30828, 20.1, 24.8, 23.2, +4.72, -1.58; beyond 60 px
39156, 29.5, 33.0, 31.8, +3.52, -1.22; class 3 23008, 123.1, 121.7, 121.7,
-1.39, 0.00.

134857, the same: class-1 coverage 16912, 46.9, 49.0, 47.4, +2.14, -1.58;
ring 1-10 px 14892, 18.9, 21.7, 19.6, +2.77, -2.08; ring 11-30 px 22620,
19.9, 24.6, 22.6, +4.70, -2.02; ring 31-60 px 36664, 21.3, 25.4, 24.9, +4.07,
-0.49; beyond 60 px 68340, 15.0, 18.1, 18.1, +3.17, -0.05; class 3 13372,
147.0, 144.3, 144.3, -2.77, 0.00.

Reading: NVIDIA's output is a few luma above raw everywhere near the label
with no differential by distance, so the polygon is not in `DlssBeforeUi`;
the resolve stage then lowers pixels near the label and none beyond 60 px.
The pixels more than 6 luma below raw outside the coverage (1,635 and 2,343
in the two crops) are already that dark in `DlssBeforeUi` and lie mostly on
the class-3 limb (846 and 2,057): that band is the limb lagging under its
private depth, not the label mask.

Footprint. F = pixels outside classes 1 and 3 where final-dlss < -1.5: 43,388
px on 131013 (mean -4.21 inside, -0.12 outside), 22,362 on 134857 (-4.16,
-0.04). Jaccard of F with `UiPrevious.a > 0`: 0.772 and 0.734; with
`UiNext.a > 0`: 0.784 and 0.728; with `UiEdits > 0`: 0.000 and 0.000; with the
class-1 coverage dilated by 5..50 px: at best 0.349 (r=50) and 0.402 (r=30).
`UiPrevious`/`UiNext` RGB is zero throughout both crops; their alpha is the
field. A column through the 131013 footprint (crop x=229) reads -5.5 to -7.4
luma from y=60 to y=156, above the text rows, i.e. the trail up and right of
the label where it had been, and 0.0 beyond the field.

Inputs to NVIDIA at the label (input pixels, probe.py). `UI.bin` holds only
0, 1 and 3: 1 is the label's dilated coverage (7,674 px in the 131013 crop),
3 the limb region (5,752). `Bias.bin` is zero in both dumps. `Z` differs from
`SceneZ` on every class-1 pixel and most class-3 pixels (11,659) and on no
class-0 pixel. Inside the coverage the motion vector is the label's own
(0.29, -1.50 px/frame on 131013) against the scene's (0.78, -2.06); a one- to
two-pixel ring of class-0 pixels around every coverage island carries the
sentinel vector equal to the output size, (5424, 5356) on 131013 and (3390,
3228) on 134857 (1,206 px in the 134857 crop). None of these produce the
polygon, by the stage numbers above; they are recorded for the limb band and
for the text-swim work.

ruled out: the game draws a dark backing behind the label, because the raw C
crops of both dumps carry no darkening around it.
ruled out: NVIDIA's reconstruction produces the label mask, because
`DlssBeforeUi` has no polygon and sits uniformly above raw around the label.
ruled out: the mask lies where the UI content changed, because `UiEdits`
overlaps the footprint at 0.00 in both dumps.
ruled out: the format-9/11 change ends the latch-off, because the 13:37 flight
passed format 9 and was refused 6 ms later by the vertex-buffer contract.

Assessment of the deferred route. Since c27fe74 (2026-09-14) it has taken
nine commits, about 21 flights and 28 dump stamps, and ten blockers, each an
exact-match contract allowlisted after a flight: destination-alpha blend, the
tone VS variant, the terminal canvas, the UAV buffer, the SINCOS opcode,
dual-source glass, the missing RDEF, format 9, and now the vertex-buffer byte
width; the B779 startup descriptors are still unmeasured. The refusal latches
the route off for the session, so one odd draw ends the experiment before the
dump. Nothing in the design bounds the number of contracts left, and no flight
has yet shown the active route on a target label. Meanwhile the shipping
path's own resolve is the measured source of the mask.

The rule, from the source (src/d3d11/ui_resolve.h, `kUiResolve`, dispatched
from temporal_pass.cpp when DLSS ran and the legacy resolve applies). Inputs:
`Raw` t0 is the colour NVIDIA consumed, `Trained` t1 NVIDIA's output,
`Coverage` t2 the UI mask, `Previous` t3 last frame's influence, `Motion` t4
the motion vectors, `Edits` t5 the content changes. Per input texel: `here` =
class 1 or 2 in the 3x3 around it (line 28-35; class 3 is excluded by
`marked`, line 21); `remaining` = `Previous.a`, and when not `here` also the
bilinear `Previous.a` fetched through the motion vector (lines 44-56);
`active = here || edited || remaining > 0` (line 57). For every output pixel
of an active texel, `Trained` is clamped to the min/max of the 2x2 raw texels
under it and `stale` is set if that moved it by more than 1/255 (lines 68-75);
edited texels are instead rebuilt by a 4x4 cubic of `Raw` (lines 76-85). Then
`Next.a = here ? 1 : stale ? max(remaining - 1/32, 0) : 0` (line 111).

Why the corona and not the sky: NVIDIA's output is 3..5 luma above the raw
2x2 range on the glow (the dlss-raw column above), so the clamp bites on every
active pixel there, `stale` holds, and the footprint decays over 32 frames
while being transported along the motion vectors. Over dark space the clamp
does nothing, `stale` is false, and the footprint drops to zero the frame the
label leaves. The dimming inside the footprint is the clamp pulling NVIDIA's
value down to the raw range, which is what the final-dlss column measures.
`Bias.bin` being zero is the shipped state (advanced.ui_depth_reactive = 0,
movers off); the transformer presets ignore that NGX input anyway.

Next, if Sean chooses A: write `kUiResolve` in Python over `DlssBeforeUi`,
raw C00, `UI`, `UiPrevious`, `MV` and `UiEdits` and check that it reproduces
`L0` in these crops (the jitter `jit.xy` is not in the dump; search the
half-pixel square for the best match). Then, on the same data, test the
candidates: a `stale` threshold well above a reconstruction offset (a few
luma) and below a departed glyph (tens), and a bound tolerance of the same
size; keep whichever leaves the footprint's final-dlss at 0 while the
changed-digit pixels still clean up. Build only after that, and the first
flight is a confirmation, not a search.


### The fix: widen the bound to the measured envelope, 2026-09-16 night

Sean chose A. Everything below was done on the two Steam dumps without a
flight; the confirmation flight is briefed at the end.

**The port.** `resolve_repro.py` (a session scratch script) is `kUiResolve`
in numpy over `DlssBeforeUi`, raw C00, `UI`, `UiEdits`, `UiPrevious` and
`MV`, run on the label crops (131013 at output 3150,2320 480x360; 134857 at
1900,1350 480x360) with a search over the jitter square. It reproduces `L0`:
131013 at jit (-0.370, -0.060) mean |diff| 0.076/255, 99.2% of pixels within
1/255 and all within 2; 134857 at jit (-0.440, +0.390) mean 0.002/255, all
within 1. Pixels of inactive texels match bit-exactly in both, so nothing
edits the frame after this shader. Its `Next.a` agrees with the recorded
`UiNext.a` on 99.68% and 99.94% of texels; every disagreement is a decayed
value within 1/255 of zero. The port is the shader; the numbers below are
what the shader does.

**The envelope.** How far NVIDIA's output leaves the 2x2 raw range under an
output pixel, on pixels with no UI influence at all (not within a texel of
class 1/2, `UiPrevious.a` 0, `UiEdits` 0), in 1/255, max over rgb:

| set | n | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| 131013 clean, whole window | 7.35 M | 0 | 5 | 13 | 18 | 166 |
| 131013 clean, raw luma 8..32 (the corona) | 619 k | 6 | 13 | 16 | 20 | 139 |
| 131013 footprint (`UiPrevious.a` > 0, not here) | 115 k | 8 | 13 | 17 | 29 | 108 |
| 134857 clean, whole window | 7.50 M | 0 | 4 | 12 | 31 | 199 |
| 134857 clean, raw luma 8..32 | 1.18 M | 1 | 10 | 14 | 31 | 199 |
| 134857 footprint | 38 k | 5 | 12 | 15 | 26 | 130 |

Two readings. The offset is not the 3..5 luma the crop means suggested but a
broad distribution: a median of 6 and a 99th percentile of 12..16 on the
corona. And the footprint's distribution is the clean corona's, so the
footprint held no ghost to remove: the clamp was paying a 30 px polygon for
nothing.

**The sweep.** The port with the bound widened by t (`lo -= t`, `hi += t`,
the `stale` test unchanged), over the crops. dL is output minus
`DlssBeforeUi` in luma; the footprint is `UiPrevious.a` > 0 and not `here`;
the here band is `here` and not edited; alive is texels outside `here` with
`Next.a` > 0 after the frame.

| dump | t | footprint dL | % over 1.5 | here band dL | edited dL | alive |
|---|---|---|---|---|---|---|
| 131013 | 0 | -3.57 | 80.8 | -1.36 | -1.13 | 12221 |
| | 4 | -1.42 | 42.4 | -0.59 | -1.27 | 10227 |
| | 8 | -0.56 | 9.2 | -0.37 | -1.28 | 7928 |
| | 12 | -0.11 | 0.5 | -0.24 | -1.28 | 4324 |
| | 16 | -0.01 | 0.1 | -0.18 | -1.28 | 372 |
| 134857 | 0 | -3.61 | 82.1 | -1.62 | +0.60 | 6060 |
| | 4 | -1.34 | 39.4 | -0.72 | +0.36 | 5057 |
| | 8 | -0.50 | 7.4 | -0.44 | +0.31 | 3968 |
| | 12 | -0.09 | 0.1 | -0.29 | +0.31 | 1637 |
| | 16 | -0.01 | 0.1 | -0.20 | +0.32 | 33 |

At 12 the polygon is gone (under 0.1 luma, no band of pixels above 1.5), the
rim around the text clears with it, and a real ghost is still cut to 12/255
(under 5% brightness) and still marks its texel stale. 8 leaves a third of
the footprint nudged by more than half a luma; 16 removes the last speckle.
Default 12. A contact sheet of `DlssBeforeUi`, the old rule and t 12 on the
131013 crop shows the polygon in the middle tile only.

**The edited branch's box.** The same port showed a second effect the sweep
cannot touch: edited texels (`UiEdits` > 0) are rebuilt from the raw cubic
regardless of t, and while the distance readout ticks the whole label is
edited (32,864 output pixels of the 131013 crop). The raw sits below
NVIDIA's level on the corona, so the label's own background became a box: at
t 12 the label's background pixels (raw luma under 40) were -1.83 luma with
70% of them moved by more than 1.5, ink +0.05. Rebuilding only where the
cubic disagrees with the temporal output by more than the tolerance gives
background +0.17 with 13% over 1.5, ink +0.31, and 29% of the edited pixels
rebuilt instead of 99%; on 134857 (952 edited pixels, digits only) the
background goes from -0.61 to +1.08. A changed digit still differs from the
temporal blend by far more than 12/255 at every ink pixel, so it is still
rebuilt; a stale residual is bounded by the same 12/255 as a ghost anywhere
else. Both rules are switched by the one tolerance: at 0 the shader is the
old one to the bit.

**The change** (branch claude/star-corona-ui-smearing-d1d2ae):
- `src/d3d11/ui_resolve.h`: a second constant buffer `R` at b1 carrying the
  tolerance; `lo -= resolve.x; hi += resolve.x` before the clamp; the edited
  branch rebuilds only where the cubic disagrees beyond it. An unbound b1
  reads zero, which is the old rule; that is what the rigs get unless a case
  binds one.
- `advanced.ui_ghost_tolerance` (default 12, in 8-bit colour steps, 0..64,
  live; read beside `ui_depth_reactive` in ui_depth.cpp, written to b1 at the
  dispatch in temporal_pass.cpp; documented commented-out in edvr.ini).
- `tools/ui_depth_test`: cases for an offset inside the tolerance (left
  alone, footprint expires), a ghost beyond it (cut to it, footprint kept),
  tolerance zero (the exact clamp), and the edited branch on both sides of
  it. `ui_colour_layer_test` compiles the shader unchanged with b1 unbound.

Not chosen: a higher `stale` threshold alone, because it only shortens the
trail; the clamp still dims every active pixel on the corona each frame (the
here band by 1.4..1.6 luma, the first-frame footprint by 3.6). Not chosen: a
tolerance relative to the raw level, because on the red corona a fraction of
the red channel is tens of /255, wide enough to pass a glyph's ghost at near
half strength, while the measured offset is 12..16 at the 99th percentile.

**Confirmation flight.** The build named in the Status block is installed to
Steam. Target a signal source or a ship whose label sits over the star's
corona and hold it there while the distance ticks; then let the label sweep
across the corona. Expect no dark polygon at the label, no box behind it, no
trail when it moves, and digits still clean. `python tools\edvr_log.py
--target steam --expect-build HEAD` must name this build. Take an INSERT eye
dump with the label on the corona. If anything remains, set
`ui_ghost_tolerance = 0` in the Steam ini for a live side-by-side with the
old rule, and 16 to remove the last speckle; report which value looked right.
