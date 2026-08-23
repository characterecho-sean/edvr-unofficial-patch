# The sun's glare rides your head in VR

*Written to accompany a Frontier issue report; no tracker number yet. Companion
write-ups in this folder cover the RemLok overlay (remlok-lines.md), terrain
culling (terrain-culling.md) and the loading hologram (loading-hologram.md),
which share the measurement method used here.*

This is a write-up of how Elite Dangerous: Odyssey draws a star's glare, why
that construction — correct on a monitor — produces a family of head-coupled
artifacts in a headset, and what EDVR does about it from outside the game. It
is written for whoever might fix it properly, inside the engine, where most of
the fix is one transform and a real `w`.

**Short version: the entire glare assembly — corona, veiling smudge, light
beams, rays and lens-flare ghosts — is one instanced draw whose vertex shader
emits every element directly in normalized device coordinates with `w = 1`:
flat in the projection plane, no perspective, placed and oriented per frame
from the camera's own axes.** On a monitor that *is* camera glare, and it looks
right. In a headset the camera is your head, so the whole assembly is bolted
to your face: it rolls when you roll, the beams stay horizontal to your eyes
rather than to the world, and the disc tilts, stretches and disagrees between
the eyes as you look around. Every one of those artifacts is the same missing
ingredient — the elements have no world-space existence and no perspective
`w` — and none of them is visible in flat play, where the camera never rolls
and there is only one eye.

---

## The symptom family

Park facing any star and move your head:

- **Roll**, and the corona, rays and beams rotate about the star's centre,
  staying fixed to your face. A real glow would hold still.
- **Yaw or pitch**, and the horizontal light beam stays pinned horizontal *to
  your view*, sliding over the world; the disc's apparent shape changes with
  eccentricity (at large angles it is drawn edge-on — it is a flat card in the
  projection plane, and off-axis in a wide-FOV headset you see it from the
  side).
- **With both eyes open**, the smudge and disc show a slight stereo tilt and
  "breathing": each eye's placement is computed against that eye's own
  strongly asymmetric frustum (the measured rigs' optical axes differ by ~11°
  horizontally), so the two eyes receive slightly different constructions and
  the visual system reads the disagreement as depth and motion that are not
  there.
- The **lens-flare ghosts** slide along the sun–centre axis with view
  direction. That one is intended behaviour for a camera artefact — it is the
  rest of the assembly inheriting the same screen-space construction that
  reads as wrong.

None of this is subtle in a headset; the rolling corona in particular is the
kind of head-coupled motion that VR guidelines exist to forbid.

## What is actually happening

Everything below was measured through a `d3d11.dll` proxy that can log a
census of the game's draw calls, capture the constant and vertex data a
specific draw consumes, and substitute a shader for exactly one draw. No game
file, game memory or game code is modified at any point. The game's own
vertex shader for the draw was captured and disassembled, and the numbers
below come from reading its inputs at the draw, in flight.

**One draw stamps the whole assembly.** Per eye per frame, a single
`DrawInstanced` (6 vertices per instance, typically ~15 instances, more when
several bodies contribute) draws every glare element, sampling two 2048×1024
art sheets laid out as a 16×8 tile atlas. Each instance is one element —
corona, smudge, beam, ray layer, or one lens-flare ghost — described by a
128-byte per-instance record: element position, size, atlas tile, colour,
anchor weights, a slide vector, and flags.

**The vertex shader has no perspective.** It projects the element's position
through the camera rows only to find a screen anchor, then builds the quad
*in NDC* and emits `SV_POSITION` with `w = 1`. The quad is therefore flat in
the projection plane by construction. Everything the headset player sees
wrong follows from this one line:

- flat in the projection plane ⇒ the element's orientation is the camera's
  orientation ⇒ head roll rotates it;
- sized and shaped in NDC per eye ⇒ each eye's asymmetric frustum produces a
  different construction ⇒ stereo disagreement;
- placed at the projected anchor with screen-space offsets ⇒ high-eccentricity
  foreshortening and edge-on discs.

**The element list is dynamic and re-sorted by look direction.** The set of
instances changes as the view moves (elements enter and leave, and the list
reorders); the records themselves are honest — the camera constants carried
by the draw track the true head pose exactly, at every angle, and the element
positions are true world positions (astronomical coordinates at range, a
local sprite placement near a body). The screen-space *construction* is the
whole problem; the data feeding it is good, which is what makes an external
fix possible at all — and would make an internal one small.

**The record layout carries everything a correct construction needs.** In
particular: anchor weights distinguish elements that sit on the star from
lens-flare ghosts that slide; a per-record base-size field distinguishes
elements that can slide from ones that structurally cannot; an axis flag
marks the anamorphic beams; and the camera block already publishes the
camera's world position alongside its projection rows.

## What EDVR does about it, from outside

`fix.sun_glare` in `edvr.ini`, hot-reloadable:

- **`realistic`** — a replacement vertex shader (same inputs, same atlas
  math, same occlusion test, written against the disassembled original) is
  substituted for exactly this draw. Elements that belong on the star —
  corona and smudge — are rebuilt as true world-anchored billboards:
  perpendicular to the camera–star ray, projected through the game's own
  camera rows with a real per-vertex `w`. They are then simply *there*: head
  motion is a camera move over a world object, both eyes agree by
  construction, and the disc keeps a constant angular size at every
  eccentricity, fading only at the frame edge (per axis, so the two eyes can
  never disagree by presence). Camera-artefact elements — beams, rays,
  ghosts — are not drawn. Elements are selected by what their record *is*
  (anchor weights, slide capability, axis flag), never by list position,
  which the re-sorting would break.
- **`vivid`** — every element drawn: the anchored set and the beams
  world-locked as above (the beams horizon-referenced, which is what "a
  horizontal beam" should mean), while the lens-flare ghosts keep their
  original screen-space slide — the movie-camera look, minus the head
  coupling.
- **`stock`** — nothing is done.

The star's own disc is a separate draw and is never touched. The substitution
keys on the draw's signature (instanced, 6 vertices, both samplers bound to
the 2048×1024 sheet pair), not on a game version: an update that changes the
assembly makes this feature do nothing rather than guess.

## What a fix inside the game would look like

The engine already computes everything required; this is a construction
choice, not a data problem.

1. **Give the anchored elements a real `w`.** For the corona/smudge class
   (the records whose base-size field is zero — they cannot slide), build the
   billboard in world or view space at the element's position and project it
   normally. That single change removes head-roll rotation, edge-on discs,
   and the entire class of stereo disagreement, in both eyes, on every
   headset, because the projection does per eye what the NDC construction
   was approximating globally.
2. **Reference the beams to the world, not the view.** The anamorphic beam's
   axis flag currently means "horizontal on screen"; in VR the convincing
   reading is "horizontal on the horizon". The world-up vector is available
   where the records are built.
3. **Keep the ghosts as they are.** Their screen-space slide is the point of
   a lens-flare; players read it as a camera artefact and it does not offend
   in VR. (Their slide lengths do grow very large at extreme view angles —
   worth a clamp.)
4. **A cheap middle ground**, if the full construction is out of budget:
   compute the assembly once against a single centred projection and
   reproject per eye, so at least both eyes agree; and gate the roll
   coupling by rebuilding the element basis from world-up rather than the
   camera's up.

Reproduction for verification is cheap: face any star in VR and roll your
head — the corona rotates with you. In a GPU capture the assembly is the only
instanced draw sampling two 2048×1024 textures in both pixel-shader slots 0
and 1.
