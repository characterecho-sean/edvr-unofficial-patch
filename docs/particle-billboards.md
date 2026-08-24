# Smoke and steam swim with the camera in VR

*Written to accompany a Frontier issue report; no tracker number yet.
Companion write-ups in this folder cover the sun's glare (sun-glare.md),
the RemLok overlay (remlok-lines.md), terrain culling (terrain-culling.md)
and the loading hologram (loading-hologram.md), which share the
measurement method used here.*

This is a write-up of how Odyssey orients its particle billboards, why that
construction — correct on a monitor — makes plumes of smoke and steam swim
and rotate in a headset, and what EDVR does about it from outside the game.
It is written for whoever might fix it properly, inside the engine, where
the whole fix is one vector per vertex.

**Short version: every particle quad in a draw is built from a single basis
taken from the camera's own up and forward vectors.** The quads are
therefore all parallel to the screen plane rather than each facing the
viewer. On a monitor that is the standard, invisible approximation. In VR it
produces two artifacts at once: the whole plume **rolls when you tilt your
head**, and because each quad's normal points along the view axis rather
than at your eye, a plume off to one side is drawn **foreshortened — and the
foreshortening changes as you look around**, which reads as the smoke
rotating about its own axis. The same construction is visible on a flat
screen too, as smoke that swims when you swing the mouse.

---

## The symptom

Stand at a geyser field on foot — in VR, and ideally in stereo (the external
camera) where depth is real. Then:

- **Tilt your head.** The plume rolls with you, as if painted on your visor.
- **Look past it, or swing the camera.** The plume appears to rotate about
  its own vertical axis and flatten, most obviously when it sits away from
  the centre of your view.
- **On a monitor**, the same thing shows as smoke swimming as the camera
  turns; it is far easier to ignore there, which is presumably why it has
  survived.

Geysers are the clearest case because their plumes are large, tall and
close. The same construction is used by other particle effects.

## What is actually happening

Everything below was measured through a `d3d11.dll` proxy that can log a
census of the game's draw calls, identify a draw by the content hash of the
shader it runs, suppress a draw, and substitute a shader for one draw. No
game file, game memory or game code is modified at any point. The game's own
vertex shader for the effect was captured and disassembled, and the
description below is read from that bytecode rather than inferred.

**The plume is one instanced draw per emitter**, whose vertex shader takes a
per-particle record: position, direction, dimensions, an axis, an alignment
mode, brightness, colour, and a flipbook atlas chain (`UVSCURRENTDIFFUSE` /
`UVSNEXTDIFFUSE` / `TEXBLENDDIFFUSE` / `ATLASINDICESDIFFUSE`) that animates
the smoke.

**The billboard basis is built once, from the camera.** In the shader:

    right = normalize(cross(cameraUp, cameraForward))
    up    = normalize(cross(cameraForward, right))

Both vectors come from the shared scene constants — `cameraForward` is
corroborated by its second use, as the depth term feeding the near-fade.
Every particle in the draw then expands its quad along that one basis:

    corner = particlePosition + right * halfWidth + up * halfHeight

So the quad's normal is the camera's view direction, identical for every
particle in the frame. That is the whole bug:

- the basis contains the camera's **roll**, so in a headset the plume is
  rigidly coupled to head tilt;
- the normal is the **view axis** rather than the direction to the particle,
  so anything off-centre is foreshortened by its eccentricity angle, and
  that angle changes as you look around.

There is also an alignment switch in the shader — velocity-aligned and
axis-aligned modes exist and reference the particle's own direction. Those
are fine. It is the camera-facing mode that carries the artifact.

## What EDVR does about it, from outside

`fix.particle_billboard` in `edvr.ini`, hot-reloadable:

- **`steady`** — a replacement vertex shader is substituted for exactly this
  draw. It is a transcription of the game's own — same inputs, same eleven
  outputs, same atlas, fades, spin and alignment modes — with one change:
  the basis is rebuilt **per vertex**, aimed at the viewer.

      face  = normalize(particlePosition - viewerPosition)
      right = normalize(cross(worldUp, face))
      up    = cross(face, right)

  The plume then behaves as a real volume does: it does not roll with the
  head, and it does not turn as you look past it.
- **`stock`** — nothing is done.

The draw is recognised by the **content hash of its vertex shader**, not by
its size or vertex count — in this scene those collide exactly with the
terrain and prop pipelines. The substitution fails soft: any compile or
lookup failure leaves the game drawing its own shader.

## What a fix inside the game would look like

The engine already has everything required; this is a construction choice,
not a data problem.

1. **Build the basis per particle, not per draw.** For the camera-facing
   mode, use the direction from the eye to the particle rather than the
   camera's forward vector. That single change removes the eccentricity
   foreshortening, and it is correct on a monitor too — it is what
   "camera-facing" is usually taken to mean.
2. **Reference the up vector to the world, not the camera.** For plumes that
   rise, a world-referenced up is both more correct and immune to head roll.
   (Purely spherical billboards work too; the world-up variant keeps a
   rising column looking like a rising column.)
3. **Mind the degenerate case.** Any such basis collapses when the facing
   direction is parallel to the reference up — a particle directly overhead
   or underfoot. The existing guard tests a squared length with `!=`
   **after** a reciprocal square root, which cannot reject the NaN that case
   produces, because a NaN compares unequal to everything. Test the length
   **before** dividing. (EDVR hit exactly this while building the fix: one
   NaN vertex becomes a triangle with no finite corner, and the rasteriser
   is free to smear it across the frame.)
4. **A cheaper middle ground:** if per-particle facing is too costly in the
   hot path, computing the basis per *emitter* rather than per draw removes
   most of the error, since a plume subtends far less angle than the view
   does.

Reproduction for verification is cheap: stand at any geyser field in VR and
tilt your head — the plume tilts with you. In a GPU capture the effect is
the instanced draw whose vertex shader takes `ALIGNBLENDBRIGHT` and a
flipbook atlas chain.

---

## Appendix: how the draw was identified

Recorded because both standard methods failed, and will fail again on
anything similar.

- **Differential census failed.** Capturing one frame set with the effect
  and one without relies on the effect being absent. A geyser field always
  has *some* vent erupting, so the family appeared with byte-identical
  counts in both captures — nothing to subtract.
- **Size-level signatures collided.** Smoke, rock and terrain all draw
  instanced, with the same vertex stride, sampling same-sized textures and
  the same exposure strip. A suppression probe narrow enough to be safe
  matched nothing; one broad enough to catch the plume removed the terrain.
- **The shader's content hash separated them in one pass.** Recording it per
  draw, and letting a suppression probe key on it, made it possible to
  switch whole families off one at a time, live, without a restart. Six
  probes named terrain, its depth prepass, the generic mesh pipeline (rocks
  and props), the water/steam volume mesh — ordinary, correctly projected 3D
  geometry — and finally the plume.

The replacement shader was written as a *mechanical transcription* of the
original rather than a reimplementation, so that it could be checked rather
than trusted: its input signature, output signature and instruction mix were
compared against the game's own before it was ever run.
