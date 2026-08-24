# Particle plumes rotate with your head — the find, and the fix design

*Written 2026-08-23, immediately after identification. The plume shader is
found and its rotation mechanism is read from its own bytecode
(`docs/particle-vs.asm`). The fix is designed, not yet built.*

## The symptom

Standing at a geyser field on foot in stereoscopic view (Explorer Cam), the
smoke plumes rotate with the headset — the same head-coupled orientation
disease the sun's corona had, in a completely different pipeline.

## Identification: what it took

The plume could not be found by the differential census, and that failure
is worth recording because it will recur:

- **Differencing failed.** A geyser field has many vents. "No plume" for the
  one you are watching is never "no plume" in the frame, so the family
  appeared with byte-identical counts (429 draws, 2577 instances) in both
  captures. Nothing to subtract.
- **Size signatures collided.** Smoke and rock and terrain all draw
  instanced, with 8-byte corner vertices, sampling the same exposure strip
  and same-sized atlases. A `census_skip` narrow enough to be safe matched
  nothing; one broad enough to catch a plume **deleted the terrain**.

What broke it open was recording, per draw, the **vertex shader's content
hash** — the one key two draws running different code cannot share (see
`census_skip`'s `vs:HASH` term, added the same day). With hashes in the
census, families could be named and skipped one at a time, live, without a
restart. Six probes mapped the field:

| Shader | What it draws |
|---|---|
| `72BDD292154158AD` | terrain (4-byte heightfield verts, colour + 2 normal maps) |
| `ACE405F428C17EF6` | terrain depth prepass (same counts, no samplers) |
| `4435F2E50020E7F3` | generic packed-vertex meshes — rocks and props |
| `8B589D25B2A0ADDC` | populous but invisible when skipped |
| `B7790CBFC6554097` | the water/steam volume — a true 3D mesh, correctly projected |
| **`EB787F983BC1F5A3`** | **the plume** |

## The mechanism, from the bytecode

The plume shader's input signature is a textbook particle billboard:
`POSITION`, `DIRECTION`, `DIMENSIONS`, `AXIS`, `ALIGNBLENDBRIGHT`,
`BRIGHTNESS`, `VERTEXALPHA`, a flipbook atlas chain
(`UVSCURRENTDIFFUSE` / `UVSNEXTDIFFUSE` / `TEXBLENDDIFFUSE` /
`ATLASINDICESDIFFUSE`) and `COLOUR`.

The particle's world position comes from `cb0[9..11]` — the world transform,
untouched by any of this. The quad's ORIENTATION is built from two vectors
in the other constant buffer:

    r0 = normalize(cross(cb1[278], cb1[279]))    // "right"
    r3 = normalize(cross(cb1[279], r0))          // "up"

`cb1[279]` is the camera's view direction — corroborated by its second use,
`dp3(cb1[279], worldPos)`, which is the depth-along-view term feeding the
near-fade. `cb1[278]` is the camera's up. So the billboard basis is the
CAMERA basis, and in a headset the camera is your head: roll your head and
the whole basis rolls, so every particle quad rolls with it.

There is also an alignment switch — `ALIGNBLENDBRIGHT.x * 255`, read as an
integer:

    if (mode == 0)  facing = cb1[279]            // camera-facing
    else            facing = normalize(worldFromLocal(DIRECTION))  // velocity-aligned

Mode 0 is the head-coupled case. Velocity-aligned particles (the streaks)
already reference their own direction and are not the problem.

## The fix, as shipped

**A replacement vertex shader, swapped in for exactly this draw** --
`fix.particle_billboard = steady`. It is a MECHANICAL TRANSCRIPTION of
the game's own shader (register for register, all eleven outputs) with
one change: the billboard basis is built per VERTEX, aiming at the
viewer, instead of once per draw from the camera's plane.

    face  = normalize(particlePosition - viewerPosition)
    right = normalize(cross(worldUp, face))
    up    = cross(face, right)

The viewer's position is solved CPU-side from the game's own clip rows
(the camera annihilates the x, y and w rows of any projective
transform), which lands it in the same space the particles are
transformed into rather than one we assumed. It reads as the origin,
confirming Elite renders these camera-relative.

Verified at the desk before it ever ran: input signature identical to
the original, output signature identical, and an instruction mix that
matches (dp4 3/3, mad 29/29, mul 33/33, rsq 10/10, sincos 1/1). That
check is why the transcription style was chosen -- a semantic rewrite
would have to re-derive eleven outputs feeding a pixel shader we do not
control, and could only be trusted, not checked.

Field-verified 2026-08-23: roll gone, yaw rotation gone, smoke lit and
animated normally.

### The NaN that took the terrain with it

The first swap build removed the plumes' rotation and the terrain. The
cause is a degenerate case the original can never reach: its basis comes
from the camera's own up and forward, which are never parallel, while
ours aims at each particle -- and a particle directly overhead lines up
with world up exactly. The cross collapses, `rsqrt(0)` is infinity,
`0 * infinity` is NaN, and one NaN vertex is a triangle with no finite
corner for the rasteriser to smear across the frame.

The game HAS a guard there, and transcribing it faithfully reproduced
its flaw: it tests `(x != 0)`, and a NaN compares unequal to everything,
so it keeps the NaN it means to reject. Harmless in the original because
the case never arises; fatal in ours. Test the LENGTH before dividing,
with a fallback axis for straight up.

The same trap is latent in any constant-substitution version of this fix
(world up in place of the camera's up degenerates when the VIEW points
straight up), which is one more reason the shader is the right home.

## The earlier approach, and why it was not enough

**Substitute `cb1[278]` — the basis's up vector — with WORLD UP, for
exactly this draw.** Then:

    right = normalize(cross(worldUp, viewDir))   // horizontal in the world
    up    = normalize(cross(viewDir, right))     // world-vertical

The quads still face the camera (which is correct, and what makes a plume
read as volumetric from any angle), but their roll is referenced to the
world instead of to your head. Rolling your head no longer rolls the plume.
This is precisely the correction that fixed the sun's corona, applied one
buffer up the pipeline.

Properties that make this the right first attempt:

- **Three floats changed.** Position, size, colour, atlas, fades, lighting
  and the velocity-aligned path are all untouched.
- **The shader is unchanged** — no reimplementation of a 6 KB shader with
  flipbook atlases and lighting, unlike the sun-glare swap.
- **Degeneracy is already handled** by the game: when the cross product
  collapses (looking straight along the substituted axis) the shader has a
  fallback branch, so looking straight up cannot produce NaNs.
- **Recognition is by shader hash**, which the census-skip work proved
  reliable and which cannot collide with terrain or props.

Mechanism: shadow `cb1` through the Map/Unmap tee and bind a substituted
copy for the matched draw — the panel-distance pattern, with the
save/restore discipline `panel_curve.cpp` documents.

## Before building: one measurement

`cb1[278]` is inferred to be the camera up from the algebra, not yet
observed. Log `cb1[276..279]` at a matched draw over a head sweep: the
vector that tracks head ROLL is the one to replace, and its neighbour that
tracks head DIRECTION is `[279]` (already corroborated by the fade term).
If the roles are swapped, the substitution moves one register and nothing
else about this design changes. The alternative outcome worth watching for
is that `cb1` is shared with draws that must keep the camera basis — in
which case the substitution stays scoped to the matched draw anyway, which
it already is.

## Dead ends worth not repeating

- **Aiming per DRAW instead of per particle.** There is no emitter
  position in these constants: cb0 is 208 bytes bound at register 0 --
  the engine-standard camera block -- and the translation column that
  looked like an emitter position is the accumulator the sun-glare arc
  already convicted. Aiming down it put every quad edge-on.
- **A shared ring buffer.** Suspected when the above failed; measured
  false in one log line (208 bytes, offset 0).

## Open

- Whether the water/steam mesh (`B7790CBFC6554097`) also needs anything: it
  is properly projected 3D geometry, so it should be world-correct already
  — worth confirming by eye once the smoke is fixed.
- Whether other particle systems in the game share `EB787F983BC1F5A3`
  (ship thrusters, explosions, atmospheric effects). If they do, the fix
  reaches them for free; the same hash is the recognition either way.
