# Smoke, steam and solar flares swim with the camera in VR

## Status

*Updated 2026-10-16 (after the Frontier flight). Update this block whenever the doc changes.*

- **State:** the steady fix is shipped and flown (the sections below). Two arcs rode `claude/billboard-roll-free`,
  flown 2026-10-16 and approved for main: the shadow's CPU cost (entry "2026-10-16", cut from a whole-block read
  to the 160 bytes the fix uses) and the roll-free basis for the untranscribed billboards
  (`billboard-root-cause-2026-10-16.md`).
- **Flown (Frontier, SteamVR aapvr, RTX 5090, log `edvr_gfx_20261016_054341.log`):** explosions no longer roll;
  steady costs about 0.02 ms CPU and 0.05-0.08 ms GPU a frame; no FAILED copies. The one-off `cb1 rows burst`
  showed `[277..279]` IDENTICAL for both eyes and 63-122 writes of the scene block a frame; it is removed.
  The `mapped read probe` found WRITE-COMBINED mappings on NVIDIA too: a full 5376-byte read took 13.6 us, so
  the old whole-block copy was worth about 0.9-1.7 ms a frame there before the 160-byte cut.
- **Hypothesis "native AMD reads are slow":** now generalised; mapped dynamic constant buffers are write-combined
  on NVIDIA as well. Still unmeasured on AMD; the `unmap readers` and `mapped read probe` lines stay, they are cheap.
- **Open:** Sean still sees explosions FLEX when he pitches his head. That is the expected limit of the roll-free
  basis, which keeps the camera's forward, so the quad plane follows pitch. Per-particle viewer facing (a
  transcription, as the plume and flare have) is the fix: branch `claude/billboard-facing`.
- **Temporary keys:** none added. `fix.particle_billboard` is hot-reloadable (edvr.ini, polled once a second).
- **Ruled out:** nothing yet.
- **Next flight:** the facing transcriptions (see the entry at the end).

*Written to accompany a Frontier issue report; no tracker number yet.
Companion write-ups in this folder cover the sun's glare (sun-glare.md),
the RemLok overlay (remlok-lines.md), terrain culling (terrain-culling.md)
and the loading hologram (loading-hologram.md), which share the
measurement method used here.*

This is a write-up of how Odyssey orients its particle billboards, why that
construction — correct on a monitor — makes plumes of smoke and steam, and
the solar flares that erupt off star surfaces, swim and rotate in a
headset, and what EDVR does about it from outside the game.
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
close. **The same construction is used by other particle effects, and that
is now measured rather than supposed.** Disassembling every vertex shader
the game creates and looking for the construction finds **seven** carrying
it. Three have been matched to effects a player can point at:

| effect | vertex shader |
|---|---|
| geyser and smoke plumes | `EB787F983BC1F5A3` |
| solar flares off star surfaces | `6041FD2D3D0164E1` |
| the witchspace starfield | `9AEC596A2B036EA6` |

The solar flare is the same artifact at a very different scale: a
prominence hanging off a star swings and flattens as you turn your head,
which is hard to miss because there is nothing else nearby to anchor it
against. The remaining four draw effects that have not been named yet.

The three are worth listing together because they show how little the
shaders have in common besides the bug. The flare and the starfield are
close enough that a careless comparison calls them the same program — same
inputs, same instruction sequence — and they still write their results to
different output registers under different `TEXCOORD` indices. Anything
that treats one as a stand-in for the other is wrong, and wrong in a way
that only shows up on screen.

An eighth shader in the family, `DB113566B6A59C5B`, does **not** carry the
bug: it selects a supplied axis or world up instead of the camera's, and it
tests its basis length *before* dividing. The engine already contains a
correct version of this construction — which is part of why the one below
looks like an oversight rather than a decision.

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

**One replacement per shader, not one for the family.** The seven affected
shaders share the construction but not their interfaces: the plume's takes
thirteen inputs and writes twelve outputs, the solar flare's takes ten and
writes five, and the others differ again. A replacement can only be
substituted into a draw whose input layout and pixel shader it matches, so
each has to be transcribed separately from its own bytecode — and
transcribed carefully, because the variants differ in more than their
signatures. The flare's axis-aligned modes do not normalise where the
plume's do, and it has no distance-fade term at all; adapting one from the
other rather than reading each would have changed how the flares look
without changing whether they were correct.

That asymmetry is the strongest argument for fixing this in the engine.
Inside, it is one vector per vertex and all seven are fixed at once.
Outside, it is seven transcriptions and seven chances to get one wrong.

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

Reproduction for verification is cheap, and there are two ways in: stand at
any geyser field in VR and tilt your head — the plume tilts with you — or
fly to a star with visible prominences and look past one, which swings and
flattens as it leaves the centre of your view. In a GPU capture the effect
is the instanced draw whose vertex shader takes `ALIGNBLENDBRIGHT` and a
flipbook atlas chain; in a disassembly it is the shader that crosses the
two camera basis vectors from its shared scene constants, which is the one
line that separates the seven affected shaders from the one that is right.

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

---

## 2026-10-16: the constants shadow read 5376 bytes of mapped memory on every write

### The report

A user on an AMD RX 9070 XT (native D3D11, Quest 3 through Virtual Desktop's OpenXR runtime, 80 Hz) lost more
than 10 fps with `fix.particle_billboard = steady` (the default) against `stock`, with temporal AA off.

### What the code does with `steady` on

- The Map tee remembers the mapped pointer of the plume shader's slot-1 constant buffer once the fix has
  learned it (vscreen.cpp hookedMap, `res == particleTarget()`).
- The Unmap tee then calls `particleCapture`, which copied the whole scene block (5376 bytes) from that
  pointer into a shadow. It did so on every write of the buffer: about 60 a frame
  (`design-transition-flash-engine-fix-2026-09-23.md`, flight 195435).
- The consumers read rows 270..273 (`solveCamera`) and 278, 279 (`shapeOk`): 160 bytes.
- The target was learned at the first plume or flare draw and kept for the session, so the copy ran every
  frame after the first smoke, whether or not any was on screen.

### Hypothesis

On AMD's native driver the mapped memory of a dynamic constant buffer is write-combined, which makes each CPU
read slow. A 5376-byte read at 18-54 us (the issue 65 figures on DXVK) times 60 is 1-3 ms a frame. Same class
as issue 65 (`issue-65-proton-cb-shadows-2026-10-02.md`, which left native AMD unchecked). Not yet measured on
the user's rig.

### The instrument (log only; `unmap_read_probe.h`)

Every 30 s the owner thread logs one line, a reader that never read as "not exercised":

    unmap readers (2400 frame(s), 1 call in 8 timed, clock pair 40 ns): panel 3.0/frame 208 B/call 0.15 us/call 0.4 us/frame; camera-glitch not exercised; camera-sunglare not exercised; camera-temporal 60.8/frame 56 B/call 2.40 us/call 146.0 us/frame; camera-rows-burst not exercised; scene-cb not exercised; particle 60.8/frame 160 B/call 3.10 us/call 188.6 us/frame; billboard not exercised; total 335.0 us/frame; particle block is the camera block on 146000 of 146000 capture(s); particle target armed, retired 0, re-armed 0

(Rendered by the formatter from synthetic figures: the shape and field order are real, the numbers are not a measurement. In a window with no smoke and a retired target, `particle` reads `not exercised` and the tail reads `particle target retired, retired 1, re-armed 0`.)
Calls a frame, bytes a call (summed over every call), microseconds a call (the mean of the timed one call in
eight, the clock pair's own cost taken off) and a frame. A call is counted only when the reader will read
mapped memory (its own early-outs asked first), so `camera-sunglare` and `scene-cb` read "not exercised" unless
the sun-glare world shader is on, `camera-temporal` unless the temporal pass wants camera rows, and
`camera-glitch` outside an engine-fix event window.

The readers priced, and what each reads FROM THE MAPPED POINTER:

| reader | reads | when |
|---|---|---|
| panel | the composite constants' whole block, at most 512 B | every composite-CB write |
| camera-glitch | 3 floats (12 B) | an engine-fix event window is open |
| camera-sunglare | 12 floats at float 932 (48 B) | fix.sunglare world shader on |
| camera-temporal | 12 floats at 932 and 2 at 792 (56 B) | temporal pass wants the rows |
| camera-rows-burst | rows 275..279 (80 B at byte 4400), see below | only inside a burst (4 frames, twice a session) |
| scene-cb | 48 B at float 932 | world shader on, and a block of 3776 B or more (a 208 B block never qualifies) |
| particle | rows 270..279 (160 B), was the whole block (5376 B) | steady, target armed |
| billboard | at most 512 B | the sun glare train's block |

Not priced, by design: the planet-patch segment copy (`celestial motion` logs its own tee ticks), the census CB
watch (F10 only), the engine-record motion reader (96 B, rows 270..275, only while engine velocity is live and
only the watched scene block) and the flat projection runtime (the flat profile; issue 65's counters).

Two more lines, once each, at the first Map of up to three distinct buffers (scene camera block, plume block):

    mapped read probe: <what>, N bytes mapped at <ptr>, VirtualQuery protect 0x.. (write-combined | no write-combine flag).
    Reading all of it took A, B and C us (the first is cold); the same copy from ordinary memory took D us.

`PAGE_WRITECOMBINE` in `VirtualQuery` is the cheap half and not reliable (a driver can map write-combined
without saying so); the three timed reads against the ordinary copy are the evidence. They read a freshly
mapped WRITE_DISCARD block whose contents are undefined and unused.

### The fix

1. `particleCapture` copies rows 270..279 (byte offsets 4320..4480, guarded by the mapped size), not the block.
   The shadow keeps the block's own layout so `solveCamera` and `shapeOk` index it exactly as before, and its
   `size` is still the mapped size, so their gates are unchanged. The rig holds this byte for byte against the
   old whole-block copy, over mapped sizes from 0 to 65536.
   (The brief named rows 270..273 and 278; `shapeOk` also reads 279, the forward vector, so the range is 10 rows,
   160 bytes, not 144.)
2. Not fed from the camera tee. The camera tee (`camResource`, a 5376-byte buffer) is the same resource, and the
   `unmap readers` line reports how often (`particle block is the camera block on K of M`), but its readers copy
   no block: they read 48-56 bytes at float 932 and 792, nowhere near rows 270..279. Feeding the shadow from
   there would still read those rows from mapped memory, so nothing is saved; the particle read stays its own,
   now 160 bytes.
3. The target retires after 300 frames (about 4 s at 80 Hz) with no plume or flare draw
   (`particleFrameBoundary`, once a frame), and the next matched draw learns it again. A session without smoke
   then pays nothing: `particleTarget()` answers null and the Map tee remembers no pointer. Re-arm costs what
   the first learn always cost: draws of that frame before the next write of the buffer go through as the
   game draws them. Logged for the first four retire/re-arm events.
4. The camera readers are untouched in this branch: they read 12-56 bytes, and the issue 65 map bounce already
   covers the VR side in design (its section 2 names the camera block readers).

### Flight plan

1. Install the branch on Frontier VR (`python tools\install_edvr.py`, `--verify-only`), check the log's version
   line (`python tools\edvr_log.py --target ... --expect-build HEAD`).
2. Same session, hot-reload A/B: `fix.particle_billboard = stock` then `steady` in `edvr.ini` (polled once a
   second), 2 minutes each, away from smoke. With `steady` and the target retired, expect `particle target
   retired` and the `particle` reader `not exercised`.
3. Fly to a geyser field or a star's flares: `particle target armed`, `particle` at 160 B/call, and the
   substitution visible as before (no plume rolling with the head).
4. Read the `mapped read probe` lines: protection flag and the three timings against the ordinary copy.
5. The AMD user: the same A/B on their rig. Prediction if the hypothesis holds: the fps gap closes with the fix
   and the `particle` figure falls from the tens of us a call to a few. If it does not close, the remaining cost
   is not this read, and the line says where to look next (`total` against the frame time).

### The cb1 rows burst: findings, then removed (2026-10-16 flight)

A measurement-only look at camera-block rows 275..279 (two 4-frame bursts per session, one line per Unmap of the
block) flew once and was removed from the branch (`cb1_rows_burst.h`, its tee reader and its rig cases). It
answered both questions it was built for:

- `[277..279]` are IDENTICAL for the two eyes on the flown headset (SteamVR aapvr). H2 (per-eye basis on canted
  displays) is not shown by this flight; a Pimax canted display remains unmeasured.
- The game writes the 5376-byte scene block 63-122 times a frame, so the old whole-block copy read about 340-650
  KB of write-combined memory a frame.

The `unmap readers` line and the `mapped read probe` lines stay. Each probe line now names its buffer (resource
pointer, ByteWidth, usage) so the up to three lines of a session can be told apart.
