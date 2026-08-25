# The FSS panel

Opened 2026-08-25, from a field suggestion at the end of the ring-split arc
(docs/fss-scanner.md): the scanner's screen in VR inherits neither the
panel resolution settings nor the panel curvature, and identifying it
properly would be worth having whatever else it explained. One capture and
two shader disassemblies later, the whole pipeline is mapped.

## What the FSS panel is, as measured

**The chrome is a pair of persistent surfaces, not a per-frame render.**
Three textures at `3408x1917` — two color (fmt 27) plus a depth (fmt 44) —
where 3408x1917 is exactly the on-foot panel size times 213/320 (5120x2880
here; a stock 1920x1080 panel would put the scanner's UI at a soft
1278x719, which is why `vscreen_res` already helps the FSS without knowing
it). The surfaces are updated INCREMENTALLY — about two draws a frame
across four shader families (`1012E00B3CB44469`, `666EF0C4C616F67E`,
`A3E5D3FCBC1165F8`, `019E81F4EECFB371`) — damage-style, never redrawn
whole. No copy ever touches them.

**The composite is a world-placed mesh, not a screen blit.** Per eye, per
frame: a position-only depth prepass (`B018D143700AB803`) and textured
passes (`A888D51024D9798E`) sampling the chrome at **PS slot 1** — both
color surfaces composite, two UI strata. Slot 1 is the whole reason the
on-foot machinery never engaged: the panel recognition tests slot 0.

**The vertex pipeline is Elite's general instanced-mesh path** — the
disassembly (docs/fss-panel-vs.asm) reads, in order: packed-uint vertex
decode (two position encodings, 1/127 normal decode, 1/32768 UVs), a
per-instance record fetched from a structured buffer at t33 (stride 336:
flags, position at byte 16, orientation quaternion, uniform scale), an
optional four-bone skinning loop over t38 (stride 48), camera-relative
positioning (`instancePos - cb1[275].xyz` — CB1 is a 4416-byte scene
block), a quaternion basis rotation, and finally projection through
**cb0[4..7]** — which is the 208-byte block the DCW capture dumped:

```
f[ 0..15]  1 1 1 1 | 0 0 0 0 | 16 16 0 0 | 16 16 0 0
f[16..31]  the fused view-projection rows (cb0[4..7]); per-eye difference
           confined to row 0 (f[16..19]) and f[23], head-tracking moves
           f[16..30] frame to frame
f[36..47]  a secondary transform (cb0[9..11]), identity for these draws
```

So in VR the scanner's screen is **a quad instanced into the world**,
placed by instance data in a structured-buffer pool, rendered through the
same pipeline as any hull or asteroid — with its picture painted on from
the persistent chrome surfaces. (The zoomed BODY is separate: its own
half-eye layer and its own dedicated composite, per the scanner doc.)

## The feature ladder

1. **Recognition.** The composite pair is identifiable by vertex-shader
   hash plus a chrome-class texture at PS slot 1 (ratio-of-panel sizing,
   any headset). Cheap; everything below stands on it.
2. **Panel distance for the FSS.** The quad's placement lives in instance
   data, not in a substitutable constant buffer — but the shader is
   camera-relative, so a REPLACEMENT VERTEX SHADER that scales the
   camera-relative position by the existing `panel_distance` factor before
   projection gives the on-foot feature exactly, through the paved
   shader_swap road (the particle and sun-glare fixes transcribed shaders
   of this same family, packed inputs and all). Both shaders need the
   treatment — prepass and color pass must agree or the depth test eats
   the quad. B018 is small (position-only) and is the natural first
   transcription.
3. **Chrome resolution.** Undo the 213/320 derate by creating the chrome
   surfaces at full panel size. The fss_res machinery is the template, but
   generalised: per-texture scale factors (320/213 is not the x2 the body
   fix hardcodes) and fractional viewport scaling for the four updater
   families.
4. **Curvature.** A six-vertex quad cannot bend; curvature needs geometry
   substitution (the panel_curve strip) driven through the transcribed
   shader's placement math. Parked until 2 lands and proves the
   transcription.

## Standing notes

- The ring-split/checkerboard question stays parked (scanner doc, rounds
  1-8); nothing in this mapping reopened it, and the chrome pipeline is
  now excluded as its home — the ring lives in the body layer.
- The DCW capture that decoded the block: `edvr_gfx_20260825_151705`,
  census_cb_watch on `A888D51024D9798E`, thirty frames, four dumps per
  frame.
- The shader blobs came from earlier `glare_shader_dump` sessions already
  on disk — the dump-by-hash discipline paying off a third time.
