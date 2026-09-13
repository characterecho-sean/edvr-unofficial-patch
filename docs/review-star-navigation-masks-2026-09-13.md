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

No production rendering change is justified yet. The smallest useful
test is the same view with the navigation graphic over the glow, first
with the current AA setting and then EDVR AA Off. Capture paired eyes
and the corresponding log/settings. If the mask remains with AA Off,
compare Sun glare Stock in the same view. These live controls need no
new diagnostic build or manual INI edits. Keep navigation graphics
visible during the comparisons so removing the marker is not mistaken
for repairing its compositing.

The existing paired capture's raw/treated crops and UI/depth inputs can
separate these branches. Broad off-centre regions may lie outside the
raw crop, so place the affected marker near the centre of the eye for
the comparison. Raw input copies are more diagnostic than another
treated screenshot alone. No issue comment has been posted, no fix is
claimed, and no install is performed by this investigation.
