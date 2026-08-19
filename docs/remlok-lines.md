# The RemLok helmet lines, drawn along your nose

*Frontier issue [69074](https://issues.frontierstore.net/issue-detail/69074) —
"VR RemLok helmet edges in wrong eyes", open since November 2024.*

This is a write-up of what the bug actually does, measured rather than
guessed, and what EDVR does about it from outside the game. It is written for
whoever might fix it properly, inside the game, where the whole fix is a
per-eye placement of one overlay.

**Short version: the helmet edge overlay is not drawn in the wrong eyes — the
same full-width overlay is drawn identically into *both* eyes, with no per-eye
placement at all.** On a monitor that is correct: the image's edges are the
edges of your view. Per eye in a headset it is not, because each eye's frustum
is strongly asymmetric: the image's inner edge sits only ~40° from straight
ahead — well inside your binocular view — while the outer edge sits past 50°,
at the blurry rim of the lens. So each eye prominently sees the line that
belongs to the *other* side of the helmet, unmatched in the other eye, and the
pair fuses into "something draped along your nose." The report's reading —
content swapped between eyes — describes the percept exactly, but the draw
itself is a replication, which is simpler and easier to fix.

---

## The symptom

Blow your canopy, or switch life support off in the right panel's Modules tab,
and the RemLok emergency helmet deploys, with faint edge lines showing the
helmet's frame at the borders of your vision. In VR the lines sit in a thin
vertical pair near the *middle* of your view, one per eye, unfused —
commenters describe "hair draped over your face that you can't brush away" and
"a small bit of stereo mismatch in the centre of your view." The tracker
confirms it on Quest 2 and Quest 3; the rig measured here is a Pimax through
OpenComposite. Two smaller symptoms come with it, and both fall out of the
same measurement: the lines are much lower resolution than the scene around
them, and their colour reads as faint grey rather than the HUD's palette.

## What is actually happening

Everything below was measured through a `d3d11.dll` proxy that watches the
game's draw calls, can log a census of them, and can withhold or wrap an
individual draw. No game file, game memory or game code is touched at any
point; identification was verified in the field by suppressing exactly one
draw and watching only the helmet lines vanish.

**The overlay is one fullscreen triangle per eye.** With the helmet deployed,
each eye's frame gains exactly one extra composite draw: `DrawInstanced`,
3 vertices, 1 instance, no depth view bound, drawn onto that eye's final
8-bit render target *after* tonemapping, sampling a single **1024×512**
overlay texture. It is the same texture, at the same screen placement, in
both eyes. Nothing about the draw differs per eye — same geometry, same
constants bound, same texture.

**Each eye's frustum is strongly asymmetric.** On the measured rig the
per-eye half-angles are ~54° outward and ~40° inward (the normal shape of VR
optics; the same numbers appear in
[the terrain-culling write-up](terrain-culling.md)). An overlay stretched
across the eye's image therefore has its two edges at very different angular
positions: the outer edge at ~54°, the inner at ~40° on the *other side* of
straight ahead.

**Follow one line through both eyes** — say the helmet's left edge, drawn at
the image's left border in both eyes:

| Eye | Where the image's left border sits | What the player sees |
|---|---|---|
| left | ~54° to the left — the lens's outer rim | barely visible periphery |
| right | ~40° to the left of centre — nasal, mid-view | a clear line hanging left of centre, seen by this eye only |

That is precisely the report's "the left edge of the helmet is displayed on
the left edge of the right eye." The two copies of the line sit ~14° apart in
angle — an order of magnitude beyond what the visual system can fuse — so
they are perceived as two independent monocular lines, and the nasal one
dominates because it is sharp, central, and unmatched in the other eye.

**The two side symptoms are the same draw.** A 1024×512 texture stretched
over a ~4340×4284 eye leaves each texel covering roughly 4×8 screen pixels —
the lines are soft because their source image is a quarter-megapixel. And
because the composite runs after tonemapping, the overlay bypasses the HDR
colour pipeline entirely, which is why its colour drifts from the palette
(reported "neon blue" on some rigs, measured faint white-grey here).

## What EDVR does about it, from outside

`fix.remlok_lines` in `edvr.ini`, live within a second of saving:

- **`outer`** — the overlay draw runs inside a substituted scissor rectangle:
  each eye keeps the fraction of the overlay nearest its own temple
  (`advanced.remlok_keep_fraction`, default 0.55) and the nasal remainder is
  clipped. Each eye then shows one line, at its outer edge — which is what a
  real helmet looks like, since your nose hides the inner edge. The draw, its
  texture and its constants are untouched; the game's rasterizer state is
  cloned once with scissoring enabled and restored immediately after the
  draw.
- **`hide`** — the overlay draw is not forwarded at all.
- **`stock`** — nothing is done. This is the default until `outer` has been
  verified in the field.

Which eye is which is taken from draw order within the frame (the left eye's
final pass draws first on every build measured), reset every frame, with
`advanced.remlok_swap_eyes` as the escape hatch if any rig shows otherwise.
The overlay is recognised by its shape — 3 vertices, 1 instance, no depth,
1024×512 in the first sampler slot — not by a game version, so an update that
changes the overlay makes this feature do nothing rather than clip something
else.

## What a fix inside the game would look like

The engine already has everything it needs; this is a placement bug, not a
pipeline one.

1. **Cheapest correct: clip per eye.** Where the overlay composite is issued
   per eye, scissor (or shift UVs so) each eye keeps only its outward
   portion. Three lines of code, zero new assets, and it reproduces the
   physical truth that a nose occludes the helmet's inner edge. This is
   exactly what EDVR does from outside, and nobody has to trust an external
   patch's eye-order heuristic — the engine knows which eye it is drawing.

2. **Better: place the overlay in angular space.** The composite already runs
   per eye; give it the eye's projection (the asymmetric tangents are in hand
   — the terrain culler consumes them) and map the overlay so its edges land
   at fixed *angles* rather than at the image borders — e.g. both helmet
   edges at ±50° in each eye. The lines then either fuse as real stereo
   content or sit outside the nasal field entirely, and the effect scales
   correctly across every headset's FOV instead of inheriting the panel
   shape.

3. **While in there:** the overlay texture is 1024×512 against ~4300-pixel
   eyes — a resolution bump or a procedural edge would fix the softness the
   tracker comments mention — and compositing it before tonemapping (or
   colour-correcting it) would restore the intended palette.

Reproduction for verification is cheap: life support off in the Modules tab
deploys the helmet in-flight without damage; in any GPU capture the overlay
is the only depthless fullscreen triangle sampling a 1024×512 texture in each
eye's final pass.
