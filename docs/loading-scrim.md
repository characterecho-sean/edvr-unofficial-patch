# The loading dialog's wash

While Elite's loader shows its progress dialog ("PREPARING SHADERS"), a large
curved surface darkens everything behind it. On a monitor that is an
unremarkable modal scrim. In a headset it covers most of the field of view,
and with the menu backdrop fix showing the good splash art underneath
(`menu-backdrop.md`) what it dims is worth looking at.

The field also described a texture in it -- "squiggly lines baked in" -- which
turned out to be the more useful clue of the two.

## What it is NOT, and how each was ruled out

This took nine flights, and every one of the eliminations below was a
hypothesis of mine that the field refuted. They are recorded because the
pattern in them is the lesson.

| ruled out | how |
|---|---|
| a draw into the interface buffer | 13,754 draws skipped, wash stood |
| the buffer's clear colour | measured `r=0 g=0 b=0 a=0` |
| a texture term | uniform substituted, no change |
| the constant buffer | ten `DCW` dumps, no term varied but the transform |
| the vScreen panel | no 5120x2880 target exists during the loader at all |
| EDVR's own screen curvature | set to 0.0, the surface still curved |

Each of those was a signature read out of a single census and bet on. None of
them was the answer.

## What found it

`tools/diff_draw_census.py`, which the census's own log line advertises and
which should have been the first instrument reached for rather than the
seventh. Two captures in one session -- one with the wash, one without --
and the difference is the whole finding:

```
ADDED    X n=5760  vs=A888D51024D9798E  ph=9107E72CB016CC02  [16x16 BC1, 4259x2395]
REMOVED  X n=5760  vs=4EF6DDB075A927FA  ph=85565E9261812E2F  [4259x2395]
```

The same curved mesh with a different shader pair and one extra texture. The
wash is not a draw of its own; it is a term inside the draw that composites
the interface. That is why no suppression, clear or panel hypothesis could
ever have found it.

A later capture added that the mesh RE-TESSELLATES: 5760 indices under one
modal, 360 under the next, same shaders and textures both times. Any signature
pinning the index count matches one and silently misses the other.

## What the shader does

Read from the game's own bytecode (`fxc /dumpbin` over the dump
`glare_shader_dump` writes), not inferred. ps 9107E72CB016CC02:

1. samples the 16x16 texture twice, into `r0.x` and `r0.y`;
2. samples the interface texture, then blurs it over an **eight-tap loop**
   along a view-dependent vector -- this is the "squiggly lines";
3. desaturates that towards luminance and tints it by `cb2[8]` and `cb2[9]`;
4. then:

```
mad r0.xyzw, r2.xyzw, r0.xxxx, r3.xyzw   ; the two 16x16 samples MULTIPLY it
add r0.xyzw, r0.xyzw, r1.xyzw            ; + the sharp interface on top
```

5. applies brightness and gamma, and discards when every channel is below
   5/255.

So the wash is a frosted-glass pass whose strength the 16x16 texture scales.

The no-wash variant 85565E9261812E2F does none of it: sample, gamma, and a
discard of its own below a magnitude of 0.001.

## The fix

`holo_fix`'s substitution, applied to PS slot 0: for that one matched draw,
bind a uniform 1x1 texture and restore the game's immediately after. At level
0 both modulation terms collapse, `r0` becomes `r1` -- the sharp interface
alone -- and the shader's own discard throws the empty area away, arriving at
what the no-wash variant does by its own route.

Matched by what it SAMPLES, not by shader hash: `A888D51024D9798E` is the
engine's general world-quad pipeline, which the FSS panel work and the
loading screen's text quad have each been caught by. A 16x16 BC1 stretched
over a mesh is the discriminator, with a large interface surface in slot 1 as
the second term.

**It shipped at level 255 first**, on the reasoning that white neutralises a
multiply. White neutralises a multiplied COLOUR; this term multiplies a
LAYER, so 255 turned the wash to full strength and the field reported no
improvement. Reading the disassembly took two minutes, and the shaders had
been sitting in `edvr_logs\shaders` since 2026-08-25 -- no flight was needed
for it at all. The guess was cheap to make and expensive to test.

## What it does not do

Removes the frosted blur layer. A flat dark tint remains, verified in the
field on 2026-08-28. Per the disassembly, with slot 0 black the output is the
direct interface sample times `cb2[5].x` and `cb2[12].xyz`, so the residue is
either content drawn into the interface buffer or that colour tint -- the
`DCW` instrument dumps b0, and this shader reads b2, so the tint has not been
measured. Open.

## Status

Off by default (`fix.loading_dim = stock`). Turning it on overrides a
deliberate legibility decision: Frontier dims the background so the dialog
reads. The dialog keeps its own solid panel, so the text stays readable.
