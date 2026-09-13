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
