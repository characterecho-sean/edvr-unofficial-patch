# Per-object motion vectors: a design

*A design document, written before the code, as a companion to
[anti-aliasing.md](anti-aliasing.md) (feature B, the temporal pass) and to
the two reviews of 2026-09-04
([review-motion-vectors-2026-09-04.md](review-motion-vectors-2026-09-04.md),
[review-temporal-far-warp-darkness-2026-09-04.md](review-temporal-far-warp-darkness-2026-09-04.md)).
Claims about EDVR cite the source; claims about the game are labelled
measured (this repo's censuses, flight logs and disassemblies), read (taken
from a captured shader's bytecode, which is a fact about one shader and a
belief about its siblings), or believed; claims about runtimes and SDKs are
labelled vendor-stated or believed; what can only be settled in a live
session is collected under Phase 0. Nothing here is implemented.*

## The ask

The temporal pass moves its history by two motions and no more. The head's,
from the runtime's poses, applied per pixel through the scene's depth
(`src/d3d11/temporal_pass.cpp:193`, the default since 2026-09-03); and the
game's camera's, from the view rows at float 932 of the scene block
(`temporal_pass.cpp:2207-2215`), applied beyond `temporal_aa_ship_metres`
and at the far plane (`temporal_pass.cpp:188-191`; the split at `:1010`,
100 m by default). Everything that moves on its own is in no vector: the
far-warp review's F5 puts a station's rim at about half a pixel a frame at
5 km and calls per-object motion "unreachable"
(`review-temporal-far-warp-darkness-2026-09-04.md:286-296`), and the design
record declined per-object matrices outright as "the engine's own velocity
buffer built from outside and re-built after every update"
(`anti-aliasing.md:1486-1498`).

The question asked on 2026-09-07 was whether per-object motion can be
estimated *performantly*. This document's answer is that the problem is
smaller than the phrase it was declined under, because three things have
been established since it was written: the per-pixel formula is already in
the shader and per-object motion is one more matrix through it; the game
keeps each object's pose in 24 bytes at the head of a record EDVR already
decodes in HLSL; and the scene's depth carries a stencil plane the game's
own draws can be made to tag, at no GPU cost, with a per-draw state twin
this project has shipped twice. What remains is forensic -- one census
flight with the census taught to print two more columns -- and the runtime
cost of the design is a few API calls per moving draw and one load per
pixel.

The tiers, cheapest first, and what each reaches:

| Tier | What | Reaches | Runtime cost | Needs |
|---|---|---|---|---|
| 1 | a mover mask from depth consistency, feeding a reactive weight | silhouettes and anything changing depth; no motion | one depth compare per pixel | nothing new from the game |
| 2 | **tags in the stencil, one matrix per rigid mover** | stations, pads, ships, the SRV, animated parts, NPC root motion; retires the 100 m split | one stencil load and one 3x4 per pixel; a state twin per tagged draw | free stencil bits, the pool visible, the record index per draw |
| 2b | the same tags by a second draw into an ID target | the same, when the stencil is not free | the movers' vertex work again | an EQUAL-depth re-issue and an occlusion check |
| 3 | hardware optical flow for the residue | particles, per-instance tumbling, transparency | about a millisecond an eye (believed), one frame stale, NVIDIA only | the NvOF SDK; better used as an instrument |
| ceiling | patched vertex and pixel shaders writing true velocity | everything, skinning included | as the engine would pay | bytecode patching, per family, per build |

Tier 2 is the design. Tiers 1 and 3 are its floor and its ceiling's
stand-in, and both are worth having independently of it.

## What moves that the pass cannot register

Nothing below is measured as pixels a frame except the station; the rest is
believed from the game and listed so the tiers can be judged against it.

- **A station's rotation.** Measured: the rim at 5 km moves half a pixel a
  frame at 42 px/deg and the hub none, so a ten-frame history smears the
  rim and not the hub, "a slow warp of the whole structure"
  (`review-temporal-far-warp-darkness-2026-09-04.md:286-296`). Inside the
  slot and over a pad the same structure fills the frame and the ship is
  counter-rotating to match it, which is the largest mover any frame has.
- **The pad, the hangar lift, the hangar itself.** Rigid, near, large in
  the frame while landing and launching.
- **Other ships, the SRV, fighters, skimmers.** Rigid bodies, a handful in
  view, small at range and large alongside.
- **The ship's own animated parts.** Gear, hardpoints, the cargo scoop:
  rigid pieces, near, moving for a second at a time.
- **Commanders and NPCs on foot.** Skinned; the root moves rigidly and the
  limbs do not.
- **Asteroid fields and ring particles.** Instanced draws whose instances
  tumble individually inside one draw.
- **Particles and sprites.** Per-sprite records in a structured buffer read
  by the vertex shader, no constants at all (`src/d3d11/cb_peek.h`); each
  sprite has its own motion.
- **Transparent surfaces.** The canopy, engine glows, holograms: no depth
  of their own, so no path through the depth registers them.

Tier 2 reaches the first five exactly (the fifth as root motion), the sixth
as one motion per draw, and the last two not at all.

## What EDVR already holds

**The formula.** `fetchHistoryT` (`temporal_pass.cpp:153`) takes a pixel's
direction, scales it by the scene depth into a point in this eye's view
space, moves the point by a rotation and a translation, projects it through
last frame's frustum and fetches the history there; `mv`
(`temporal_pass.cpp:251`) transcribes the same lines and writes the
displacement out for NVIDIA's history instead (`:274-297`). The world path
is that formula with the rows' delta and `tvCam` (`:188-191`); the head
path is the same formula with the runtime's poses and the eye offset
(`:193`, `src/common/temporal_math.h:153`). A rigid object that moved is
the same formula with one more affine transform composed in, and both
existing paths turn out to be special cases of it (the arithmetic below).
Nothing new happens per pixel except choosing which transform.

**The pose, in 24 bytes.** The Full System Scanner's panel is drawn through
"Elite's general instanced-mesh path" (`fss-panel.md:27-35`), and its
vertex shader was captured and disassembled
(`docs/shaders/fss-panel-vs.asm`). Read from that bytecode: the vertex
carries an `INSTANCEANDMODELDATAINDEX` (`:15`) that indexes a structured
buffer at t33 with a 336-byte stride (`:37`, `:97`); the record's position
is at byte 16 (`:136`) and is made camera-relative by subtracting
`cb1[275]` (`:137`), which is "Elite's world-rebase origin, near the camera
but not at it" (`src/d3d11/fss_panel_vs.h:14-16`, learned the hard way);
the orientation is a quaternion packed as four unorm16 (`:138-142`); the
scale is uniform (`:163`); the result goes through the per-draw
view-projection at `cb0[4..7]` (`:168-171`); and a record whose first
field is non-zero is skinned over up to four bones from t38, 48 bytes each
(`:113-117`). EDVR's replacement shader for that draw already transcribes
the record's head as a struct -- bone base, scale, the two packed quaternion
words, the position, and padding to 336 (`fss_panel_vs.h:50-57`) -- and
decodes the quaternion (`:93-101`). The pose of an object is therefore
bytes 4..27 of its record, and "did it move since last frame?" is a
24-byte compare.

That every hull, station, pad and asteroid uses the same record head is
*believed*, from `fss-panel.md`'s reading that the scanner's quad is
"rendered through the same pipeline as any hull or asteroid". It is the
first thing Phase 0 checks.

**The stencil plane.** The scene's depth pair -- the two eye-sized targets
with the scene's hundreds of draws -- is `R32G8X24_TYPELESS` under a
`D32_FLOAT_S8X24_UINT` view, bind flags 0x48, cleared to 0.0
(`anti-aliasing.md:831-834`, measured docked; 302 and 325 draws a frame).
Eight stencil bits sit beside every depth value the pass already reads. The
game uses some of them: the holo panels draw with stencil enabled and
reference 4 (`review-ui-depth-2026-09-06.md:498-500`), and the lighting
pass of the black-planet hunt ran one draw at reference 8 in the frame the
body survived and 16 in the frame it did not (`src/d3d11/stencil_probe.h`).
Which bits those passes *read* was not recorded: the census printed the
enable and the reference and not the masks. Since 2026-09-07 it prints the
masks and the ops as well, as the `so=` column
(`src/d3d11/draw_census.cpp:837-881`, read by `tools/stencil_census.py`),
so Phase 0's first question now needs only a flight. What the logs already
here can say is that the *references* the scene's draws use span all eight
bits, which is a reason for pessimism rather than an answer (question 1).

**The per-draw twin.** Around one draw, fetch the game's depth-stencil
state and reference, bind a twin with a field changed, forward, restore.
The stencil probe does it with the reference alone
(`src/d3d11/stencil_probe.cpp:157-205`); `ui_depth` does it with a created
twin state whose depth-write field differs, cached per game state, with
the stencil fields copied exactly (`src/d3d11/ui_depth.cpp:327-372`,
`:559-599`). `OMSetDepthStencilState` is not a hooked slot; the `OMGet` at
the draw is the pattern (`review-ui-depth-2026-09-06.md:46-50`), and it
runs today on every classified draw of the interface.

**The tee.** `hookedMap` recognises the scene block by size at the map
(`src/d3d11/vscreen.cpp:2520-2523`, `glitchFrameWantsBuffer`) and
`hookedUnmap` reads it before the real unmap, while the bytes are still
the game's live write (`:2603`, `:2627-2647`), fanning out to four readers
including the temporal pass's ring of view rows. `UpdateSubresource`,
`CopyResource` and `CopySubresourceRegion` are hooked too
(`vscreen.cpp:4733`, `:4748`, `:4750`). So a buffer the game fills by any
CPU path -- a map, an update, a copy from a staging buffer it mapped -- can
be shadowed by machinery that exists; only a buffer filled by a compute
dispatch cannot.

**The hooks, and the gaps.** On the immediate context (`vscreen.cpp:4719-4763`):
every draw variant, `OMSetRenderTargets`, `VSSetConstantBuffers`,
`PSSetShaderResources`, `RSSetViewports`, the clears, the copies, the
queries, `Map`, `Unmap`, `UpdateSubresource`. Not hooked, by decision:
`IASetVertexBuffers`, `IASetInputLayout`, `VSSetShader`,
`VSSetShaderResources`, `OMSetDepthStencilState` -- read with `IAGet*` and
`VSGet*` at the draw when a question needs them, because a patched slot on
the hot path is what the 0.7.x line paid for (`src/d3d11/draw_census.h`,
"WHY THE CONTEXT AND NOT THE SHADOW"). On the device: `CreateVertexShader`
and `CreatePixelShader` are hooked for the signature table and the hash
registry (`src/d3d11/device_hook.cpp:41-42`, `:1596-1598`);
`CreateInputLayout` is not, and an input layout cannot be asked for its
description after creation, so learning which stream feeds
`INSTANCEANDMODELDATAINDEX` means one more device-side hook (slot 11 by the
same count as the others, to be verified against the SDK's vtable the way
`CreateTexture2D` was, `device_hook.cpp:37-40`).

**The consumers and the instruments.** The `MV` target the `mv` entry
writes (`temporal_pass.cpp:42`) is what NGX evaluates
(`src/d3d11/dlaa.cpp:362-363`); the registration instrument judges up to
four candidate deltas per interval and has a retired slot
(`temporal_pass.cpp:54-57`, `:1571`, the line at `:2330-2370`); the debug
views paint where each pixel's history was fetched from (`:521`,
`advanced.temporal_aa_debug`); and the depth probe owns the one moment in
a frame when the scene target is complete and unbound
(`src/d3d11/depth_probe.cpp:577-607`), which is where a tag has to be
rescued if it does not survive to Submit.

## The arithmetic

For frame *f*, let `V_f` be the game's view for this eye (a 3x4: rotation
and translation, world to view, composed from the rows at float 932 and
the eye's offset the way the world path's delta and `tvCam` already are),
and for an object let `W_f = [ s_f R(q_f) | p_f ]` be its record's pose in
the rebase frame (scale, rotation from the quaternion, position). The
vertex shader computes the view-space point of a vertex `v` as
`V_f · W_f · v`. For a pixel of that object the pass already has the
view-space point now, `P_now = z · d`. The same surface point last frame
was

    P_prev = V_prev · W_prev · W_now⁻¹ · V_now⁻¹ · P_now
           = M_obj · P_now,      M_obj = V_prev · D · V_now⁻¹,   D = W_prev · W_now⁻¹

`D` is the object's rigid motion between the frames, expressed in the
rebase frame, and `M_obj` is one 3x4 affine transform per object. Three
things follow:

- **The existing paths are the cases `D = I` and `D = the ship's`.** A
  static object has `W_prev = W_now`, so `M_obj = V_prev · V_now⁻¹`, which
  is the world path's delta with `tvCam`. The cockpit's records move with
  the ship every frame, and `V_prev · D_ship · V_now⁻¹` is the head's own
  delta, which the head path takes from the runtime instead. So one matrix
  per object generalises both paths, and where the two must agree -- every
  cockpit pixel -- the instrument can check the records against the
  runtime, frame by frame, before the feature is trusted with anything
  else.
- **The floating origin cancels.** `V_prev` and `W_prev` are both in
  frame *f-1*'s rebase frame, `V_now` and `W_now` both in frame *f*'s; a
  jump of the origin between the frames moves each pair together. The one
  rule is that a frame's `V` and its pool copy are the *same* frame's,
  which is what `chooseCameraRows` (`temporal_pass.cpp:1071`) already
  enforces for the rows and the pool shadow must enforce for the records.
  The far-warp review's F3 (a carried origin jump in the translation term)
  is the failure this rule exists to keep out.
- **The jitter is not in it.** Records and rows know nothing of the
  sub-pixel offset; the pass's tangents exclude it on both frames (the
  motion-vector review's H1); `M_obj` is jitter-free like every other
  delta the pass composes.

`D` is the same transform for every part of one rigid assembly: each part's
`W` differs, but "where the assembly was last frame relative to where it is
now" does not. So tags are assigned to *distinct motions*, not to draws:
every part of a station shares one, every piece of the cockpit another, a
passing ship a third. A frame has a handful. The scale ratio is 1 unless
an object is scaling, and the linear part of `D` is then a pure rotation,
which is cheap to compare: two draws share a bucket when their `D`s agree
within a tolerance of a hundredth of a degree and a centimetre at the
object's distance, with the tolerance itself a Phase 0 number.

Per eye, `M_obj` is composed through that eye's own `V` -- the rows plus
the eye offset the head path already carries (`temporalPassNoteHead`,
`temporal_pass.cpp`) -- because the two eyes' view transforms differ by the
IPD, which matters for a mover at arm's length and not for a station.

## The design: tags from the game's own draws

Per frame, in the order the frame happens:

**1. Shadow the pool.** At the tee, recognise the model-data pool the way
the scene block is recognised (by its size class and its 336-byte
structure stride, learned once from `VSGetShaderResources` at a scene draw
-- the census's own pattern for state nothing hooks -- and thereafter a
pointer compare at Map), and keep this frame's bytes and last frame's.
Which write path the pool takes, how large it is and how many times a
frame it is written are Phase 0 questions, and they decide whether this
step is a memcpy at Unmap or the GPU variant below. The scene block is
written over a hundred times a frame in space (`temporal_pass.cpp:1028-1031`);
if the pool is written like that, a shadow at the tee is off the table.

**2. Classify each scene draw.** For every draw into the scene pair -- the
depth probe already knows those with one pointer compare
(`depthProbeNoteDraw`, `depth_probe.cpp:440`) -- find the record index,
compare the record's pose bytes with last frame's copy, and:

- *unchanged*: nothing. The world path is exact for it already, and it
  costs the compare and no more;
- *changed*: compute `D`, find or open the bucket whose `D` matches,
  and if the bucket has a tag, arm it for this draw.

Opaque draws only: a blended draw (the census's `bl=` column) writes no
depth and must not stamp a tag over the surface behind it. A draw whose
record is skinned gets the root's `D`, which is the right answer for a
walking NPC's body and an approximation for the arms.

**3. Stamp the tag.** Around an armed draw, exactly the stencil probe's
motion (`stencil_probe.cpp:157-205`): `OMGet` the game's state and
reference; bind a twin of that state -- created once per game state and
cached, as `ui_depth` does -- whose stencil is enabled with function
ALWAYS, pass-op REPLACE, fail-ops KEEP, and a *write mask restricted to
the free bits*; set the reference to the tag shifted into those bits;
forward the draw; restore. The game's depth test does the rest: the
stencil op runs only where the pixel passes, so a mover behind a static
object leaves no tag on the pixels it lost, and the final stencil holds
the tag of the visible surface. Under a depth pre-pass laid with GEQUAL and
a colour pass tested EQUAL (the shape the terrain showed, `draw_census.h`),
both passes of a mover carry the same record and stamp the same tag, which
is harmless. Cost per armed draw: one `OMGet`, one `OMSet`, one restore.
Every unarmed draw pays nothing beyond step 2.

**4. Resolve at Submit.** Make a shader view over the scene pair's stencil
plane (`X32_TYPELESS_G8X24_UINT`, vendor-stated: the stencil view of the
`R32G8X24` family, the sibling of the depth view the probe already makes
at `depth_probe.cpp:160`). Upload the bucket table -- tag to `M_obj`, at
most 2^bits entries of 3x4, a few hundred bytes in the pass's constant
block -- composed per eye. In `fetchHistoryT` and in `mv`, after the 3x3
nearest-depth dilation, load the tag at the texel that won the dilation
(so an edge follows the thing in front, the same rule the depth follows),
mask it, and where it is non-zero replace the point's move with
`M_obj · P_now` before the projection that already follows. Tag zero does
exactly what the code does today; a tag the table has no entry for is
treated as zero. For NVIDIA's history nothing changes downstream: the same
`MV` and the same depth copy go out.

If the stencil does not survive to Submit (a later pass clears it, or the
lighting rewrites it), the tag is resolved at the last unbind instead: the
depth probe already stops the frame at the moment the game switches away
from the scene target with its contents complete
(`depthProbeWantsSample`, `depth_probe.cpp:577`), and a copy of the stencil
plane there -- eye-sized `R8_UINT`, 29 MB at 5424x5356, tens of
microseconds -- gives the pass a tag texture of its own to read at Submit.

### The record index

Step 2's one open cost is finding a draw's record index. It is a vertex
attribute, fed from an instance-rate stream at the draw's start-instance
location, and no hook shadows the input assembler. Three ways, in the
order to try them:

- **Read it at the draw, memoised.** `IAGetVertexBuffers` for the one slot
  the input layout feeds `v0` from, and the index is the uint at the
  stream's bound offset plus the start instance times the stride -- *if*
  the stream's bytes are visible, which they are when the game maps it
  (the tee shadows it like the pool). The slot and the element's offset
  come from the input layout at creation (the `CreateInputLayout` hook
  above). One COM call per scene draw is a few hundred nanoseconds; on the
  784-917 eye draws a frame the cockpit shows
  (`review-ui-depth-2026-09-06.md:406`) that is under half a millisecond,
  which is too much for the hot path as a steady state and fine as a
  learning cost: memoise the index against the draw's shape (index buffer,
  start index, index count, base vertex, the VS constant object in slot 0,
  all of which the shadow or the call already has), re-verify every few
  seconds, and the steady state is a hash lookup and a 24-byte compare.
  This assumes a record keeps its slot in the pool from frame to frame,
  which is Phase 0's question 3.
- **Read it on the GPU.** Where the pool or the stream is not CPU-visible
  (filled by a dispatch, or a ring too large to copy), every question
  moves to the GPU: `CopySubresourceRegion` four bytes from the instance
  stream into an EDVR buffer per armed draw (a dynamic buffer is a legal
  copy source, vendor-stated), keep an EDVR copy of the pool from last
  frame by one `CopyResource` a frame, bind the game's pool through an
  EDVR shader view (it has the shader-resource bind flag by construction,
  since the vertex shader reads it), and let a tiny dispatch build the
  bucket table. The CPU can then no longer classify *before* the draw, so
  it tags by the previous frame's answer instead: a readback, one frame
  late, of which draw shapes moved. One frame of lag in *noticing* a mover
  is invisible; a draw order that reshuffles between frames would not be,
  and whether Elite's is stable is Phase 0's question 4.
- **Hook the assembler.** One more patched slot, which the project spends
  reluctantly and which is the fallback if the memo's hit rate in the
  field is poor.

### What it costs

Estimates, all believed until Phase 0 measures the two that matter (the
pool's write path and the memo's hit rate):

| Where | What | Estimate |
|---|---|---|
| GPU, per pixel | one stencil load at the dilation's winner, one 3x4 mat-vec | negligible beside the nine depth loads already there |
| GPU, per draw | nothing | the tag rides the game's own rasterisation |
| GPU, per frame | the stencil copy at the last unbind, only if needed | tens of microseconds |
| CPU, per scene draw | memo lookup, 24-byte compare | tens of nanoseconds; about 0.05 ms a frame |
| CPU, per armed draw | `OMGet`, `OMSet` of a cached twin, restore | under a microsecond; tens of draws a frame |
| CPU, per frame | the pool memcpy at the tee | about 0.1 ms per megabyte written |
| CPU, learning | `IAGetVertexBuffers` on memo misses | a few hundred nanoseconds each |
| Stencil | one tag per distinct motion, not per draw | 3 bits cover 7 movers; 5 cover 31 |
| Memory | two pool copies, one stencil copy per eye if needed | single-digit megabytes |

Overflow degrades to today: a mover that gets no tag takes the world path,
which is what every mover takes now.

### What tier 2 does not reach

- Instances tumbling inside one draw share the draw's `D`. A tag per
  instance needs the vertex shader to write `SV_InstanceID` somewhere,
  which is the ceiling's work.
- Skinned limbs get the root's motion. Exact skinning needs last frame's
  bone palette applied per vertex, which is also the ceiling's work.
- Sprites and particles carry their own records in a buffer the vertex
  shader reads directly (`cb_peek.h`); no constant, no draw-level pose.
- Transparent surfaces have no depth and no tag; they inherit the motion
  of whatever is behind them, as they do today.

## The cheaper thing, and the fallbacks

### Tier 1 -- a mover mask from depth consistency

Keep last frame's depth (the pass already copies it for NVIDIA, `ZC` at
`temporal_pass.cpp:43`), reproject it by the camera-only vectors, and
compare with this frame's: a pixel whose surface is not where the camera
alone would have put it is either a mover or a disocclusion, and both
deserve less history. In the own pass that is a reactive weight -- blend
less where the mask is set -- which is the exact next step the design
record named when it declined the matrices (`anti-aliasing.md:1494-1497`).
On the trained path it is the bias-current-colour mask NVIDIA's evaluation
parameters carry (believed: `pInBiasCurrentColorMask` in the D3D11
evaluation block of the vendored helper header; the block the pass fills
is at `dlaa.cpp:362-376`), which tells DLSS to favour the current frame
where set. It costs one depth compare per pixel, needs nothing from the
game, and it stays useful under tier 2 for the residue tier 2 does not
reach. What it cannot do is *move* anything: a flat hull crossing the
frame at constant depth passes the test in its interior and ghosts there
exactly as it does today, so tier 1 is a floor, not the feature.

**Built 2026-09-08, behind `fix.temporal_aa_movers = off`, unflown.** What
was built, and where it differs from the paragraph above:

- *The carry costs no copy.* Both shader entries write this frame's depth
  into the trained set's depth copy (`ZC`, `dlDepth`), and at the frame's
  end the copy and a twin (`zPrev`) swap pointers, so next frame reads last
  frame's at `t3`. The own path makes the depth copy alone when no trained
  set exists; a rebuild, a withhold or a frame without a depth in hand
  clears `zPrevValid`, and the mask stays off until a frame has written one
  again. One `R32_FLOAT` per eye more resident (74 MB at 4340x4284).
- *The test is against a range, not a texel.* The prediction is the
  reprojection's own point -- `-dp.z` in last frame's eye space, for the
  path the pixel actually took (the head's delta or the camera's) -- and it
  is compared with the 3x3 of last frame's depth around `pp`, in metres,
  with the tolerance (`advanced.temporal_aa_movers_tolerance`, 3% of depth)
  applied to the range's ends: the jitter shifts the grid half a pixel
  between frames and a single-texel compare fires on every silhouette every
  frame whether anything moved or not, which is the same reason the
  reprojection dilates. The far plane is consistent only with the far
  plane, so a pixel that is sky now where a hull was last frame -- a
  mover's trail -- is masked too, and a surface now where only sky was has
  moved in. `temporalMoverTest` in `temporal_math.h` is the reference and
  `tools/temporal_test` pins nine cases of it.
- *What the mask does.* In the own pass the history weight is multiplied
  by `1 - strength` where the mask is set (`advanced.temporal_aa_movers_
  strength`, 1.0: the fresh frame alone, spatially settled by the filter).
  On the trained path the `mv` entry writes `strength` into an `R8_UNORM`
  texture that `dlaaEvaluate` hands NVIDIA as `pInBiasCurrentColorMask`
  (question 9), the full frame only -- the fovea crop and the steady
  periphery evaluations are not handed it, though the own periphery pass
  applies it when it runs.
- *What it costs.* Nine extra `R32` loads a pixel where the reprojection
  landed on the image; the mv entry writes one byte more. The price prints
  in the totals as before.
- *How to read the flight.* The engage line says the mask is on and what
  it was set to; the registration line's second half gains "the mover mask
  set N% of pixels". Docked with the head still, N is the noise floor for
  the tolerance and should read near zero; in the slot it is the rim's
  edges and whatever crosses the view. `advanced.temporal_aa_debug =
  movers` paints the mask white over the frame dimmed, on either path.
  Then Sean's eyes: the distant station's rim without its trail, the
  station lights that blurred under vessel movement (the Pimax flight of
  2026-09-04), and whether anything static flickers at its edges, which is
  what a tolerance set too tight looks like.

**Flown 2026-09-08** (`edvr_gfx_20260908_090950.log`, DLSS at 2514x2482,
the mask switched on live at 09:16:39). The plumbing held: the census's
`DCX` lines show the `mv` dispatch with the mask at `u5` and last frame's
depth at `t3` from the first frame after the engage line, and NVIDIA's
running price read 2.39 ms/eye before and 2.40 after (the pass's own 2.55
-> 2.56), at 85-89 fps -- the cost is below what the running average can
see. The masked share: **0.45% of pixels with the ship and head still**
(the world delta 0.000 deg/frame, the eye 0.0006 m/frame), which is the 3%
tolerance's noise floor and is not zero -- about 28,000 pixels an eye-frame
firing on something that does not move, most likely depth edges of thin
geometry and the interface's depth-written strokes, which the `movers`
debug view would name in one screenshot; **0.6-1.5% under way** (the eye
moving 0.1-1.4 m/frame in the rows), the higher figures with the higher
speeds, as disocclusion and parallax should. What the eyes made of it is
the open half.

**The station approach, same day** (`edvr_gfx_20260908_093227.log`): "the
station's solar panels still seem pretty blurred with their rotational
movement as I approached", and some frame hitching. Two findings, neither
the mask's:

- **The world path was down for the first 58 seconds of the approach.**
  The camera chooser latched onto another object's rows -- one of the
  station's auxiliary render passes, written every frame and continuous
  with itself -- and continuity is self-reinforcing: once the chain is on
  that camera, the bound block's real rows are never within three degrees
  of it again ("another's on 1774 frames, the bound block's on 0", two
  intervals running). The head-follow score did its job and stood the world
  path down at 09:33:03, but nothing re-synced the chain until the
  auxiliary camera stopped writing at 09:34:01. For that minute every pixel
  beyond 100 m took the head's delta alone, so the whole station -- panels,
  hub, rim -- smeared under the ship's motion, which is the 2026-09-04
  regression, not the rotation. Fixed in the chooser: while the rows have
  stopped following the head and the bound block wrote this frame, its
  latest write is taken over the chain (the path is already down, so a
  wrong pick costs nothing more, and the score decides when to bring it
  back); the registration line counts those frames. With the path up
  (09:34:07 on: 62-66% of pixels) what remains on the panels is the
  rotation itself, which is tier 2's job -- the mask set 0.02-0.26% of
  pixels through the approach, blind to in-plane motion at constant depth
  exactly as predicted.
- **The hitching coincides with the transition-flash detector's
  withholds.** It withheld 10 frames between 09:34:23 and 09:35:03 ("drawn
  from 5016 world units off the camera's path", while cataloguing the
  station's auxiliary cameras at radii 947 to 10126 units), each a repeated
  frame AND, under DLSS, a restart of NVIDIA's history (the reset count
  went 4 to 16 in that minute). The vr pacing lines show a ~67 ms frame in
  each burst that holds a withhold, against 13-400 ms long frames scattered
  through the whole session that look like the game's own streaming near a
  station. The pass's and NVIDIA's own GPU maxima never exceeded 49 ms (the
  start-up compile) and 3.1 ms, and no CPU-side wait exists in the frame
  path (the depth probe's blocking `Map` is in its self-test only), so the
  67 ms frames are not timed as EDVR's; the A/B is the same approach with
  `transition_flash = 0`. The interaction itself is a design point for the
  transition-flash workstream: under a trained history, every false
  positive is a visible pop, not just a repeated frame.

### Tier 2b -- the tag by a second draw

If Phase 0 finds no free stencil bits, the tag goes into an EDVR target
instead: after forwarding an armed draw, bind an EDVR `R16_UINT` target
with the game's depth view, a twin state with depth function EQUAL and
writes off, and a pixel shader of EDVR's that reads nothing and writes the
tag from a constant (a pixel shader with an empty input signature links
with any vertex shader that writes a position, vendor-stated), re-issue
the same draw call with the game's own vertex shader, constants and
tessellation still bound, and restore. The game's vertex shader does the
placement, skinning included, so the ID target is exact for the draw. Two
costs the stencil has not: the movers' vertices are rasterised twice
(bounded, since only movers re-issue), and a later *static* draw that
occludes part of the mover does not erase the tag, so the pixel shader
also writes its depth beside the tag and the resolve drops any tag whose
depth is not the scene's at that pixel.

### Tier 3 -- optical flow, as an instrument first

NVIDIA's Optical Flow SDK runs the hardware flow engine on Turing and
later through a D3D11 interface (vendor-stated as believed; the SDK's
interface list decides), returning per-block vectors and a cost. Fed the
door's two frames it estimates every motion at once -- particles,
tumbling instances, transparency -- and at about a millisecond an eye at
half resolution (believed, unmeasured), one frame stale if run beside the
next frame's render, NVIDIA only, on the post-HUD LDR frame with the
jitter in it. As a *source* it is the least in this project's spirit: an
estimate, a black box, a cost. As an *instrument* it is the one thing on
this list that measures the question directly: flow between two dumped
frames (the eye dump machinery exists, `tools/diff_eye_dump.py`) against
the `MV` texture gives a residual map that says which pixels' vectors are
wrong and by how much, before anything is built, and after tier 2 it is
the acceptance test.

### The ceiling -- the engine's own velocity buffer

Patch the scene families' vertex shaders to read last frame's record
(`EDVR`'s copy of the pool, bound at a spare slot) and last frame's bone
palette, compute the previous clip position per vertex, and pass it to a
patched pixel shader that writes true velocity to a spare render target.
Exact for skinning and for instances, and exactly what the design record
declined: DXBC rewriting and re-signing, one patch per family, redone
after every game update. It is listed so the reader knows where tier 2
stops and why it stops there.

## Considered and declined

- **Hooking `IASetVertexBuffers` by default.** The memo should make the
  index a lookup; hook the slot only if the field says the memo misses.
- **A tag per draw rather than per motion.** Eight bits cannot name three
  hundred draws, and a station's forty draws need one number, not forty.
- **Optical flow as the primary source.** Cost, vendor, staleness and the
  HUD in the input; the instrument use is the right one.
- **Deriving the pose from the per-draw constant block.** The FSS quad's
  `cb0[9..11]` is a secondary transform, identity for those draws
  (`fss-panel.md:42`); the particle family's `cb0[9..11]` is a real
  transform, but the general path's pose is in the record, not the
  constants.
- **Reprojecting by the game's own motion blur.** The graphics menu's
  Blur option is believed to be a camera effect and not a velocity
  buffer; if a census with it on shows an eye-sized two-channel float
  target in the G-buffer pass, that changes everything above and is
  checked first (Phase 0, question 8).

## Phase 0 -- what must be measured, not assumed

One flight, three scenes (the slot of a rotating station, a pad with the
gear down, a wing-mate alongside; on foot at a settlement if there is
time), with the census extended. Each question names what answers it.

*Worked on 2026-09-07.* The census column is built and the tool that reads
it exists; questions 2, 5 and 8 are answered from logs already on this
machine, and 1 has a provisional answer. Each is marked below with what
answered it. The rest need the game running -- see
[the flight checklist](#the-flight-checklist) at the end of this section.

1. **Which stencil bits are free.** Extend the census's `ds=` column
   (`draw_census.cpp:839-856`) with the stencil read mask, write mask and
   the three ops, and list the masks of every draw and clear that touches
   the scene pair. The tag's write mask must miss every read mask; a
   lighting pass that reads 0xFF ends the stencil design and starts 2b.

   *The column is built* (2026-09-07): the census now prints `so=` after
   `sm=` -- read mask, write mask, `FrontFace.StencilFunc` and the three
   front ops, with the back face appended after a `+` only when it differs.
   It costs no new API call, because the `GetDesc` the `ds=` column already
   pays for carries all of it. `tools/stencil_census.py` decodes it into
   the four answers this question wants, per depth target.

   *Provisionally answered, and it is a warning* (measured 2026-09-07). Two
   denominators, and they are different. Eye-sized 32-bit stencil-bearing
   depth targets appear in 209 censuses across 78 logs, carrying 199,283
   draws -- but the `st=` column itself only arrived on 2026-09-02, so
   **just 17 logs and 51 censuses, 44,162 of those draws, say anything about
   stencil at all**. Everything below is that subset, and it is the
   REFERENCES only: no log predating the new column can give the masks.

   **74.8% of the scene-depth draws that record it run with stencil
   enabled** (33,052 of 44,162; the other 25.2% have it off). This is not a
   mostly-unused plane. The references seen are 0, 4, 5, 7, 8, 9, 21, 25,
   32, 149 and 255, and their union is **0xFF: every one of the eight bits
   is named by some draw's reference.** The commonest are 5 (10,772 draws),
   4 (7,839) and **149 = 0b10010101, 7,293 draws, which names bit 7**. The
   design's own recollection that "refs 4, 8 and 16 have been seen" is
   wrong in one particular worth fixing: **reference 16 appears in no
   census on this machine**, and the population the design did not know
   about (149, 21, 25, 7) is the larger one.

   A reference is still not a mask -- a draw referencing 149 through a read
   mask of 0x04 reads one bit -- so this neither opens nor closes the
   stencil route. What it does is move the prior: go into the flight
   expecting the masks to be wide, and have tier 2b ready.

   **ANSWERED BY THE FLIGHT: bits 1 and 6 are free, and the stencil route
   lives** (measured 2026-09-07 over 13 scene depth targets across
   `edvr_gfx_20260907_182944.log` and `edvr_gfx_20260907_185152.log`, six
   scenes, ~10,000 stencil-enabled scene draws). The provisional pessimism
   above was wrong, and it was wrong for an instructive reason: it read
   REFERENCES, and a reference names bits a draw never touches.

   - **What the game READS is narrow and specific.** Nine reader families in
     all. `9AEC596A2B036EA6` (the witchspace starfield) and
     `EB787F983BC1F5A3` (the plume) test **bit 7** with EQUAL through mask
     `80`; `53211E8C072CD02E` tests **bit 5** through `20`;
     `D8FCE3CEA16B9B51` and `0C4E76889907B963` test **bit 4** through `10`;
     `9FFA5D5E79F04873` tests **bit 3** through `08`; `E508648660A352B2`
     (the interface composite) tests **bit 0** through `01`. Union of every
     narrow read mask: **0xB9, bits 0, 3, 4, 5 and 7.**
   - **What the game WRITES through a narrow mask** is `15`, `05`, `95`,
     `0D` and `09`: union **0xBD, bits 0, 2, 3, 4, 5 and 7.**
   - **So bits 1 and 6 are touched by no narrow mask at all**, in either
     direction, in any scene flown.
   - **The full-mask writers do not close them.** 825 draws write `w=FF`,
     and 802 of them run `ds=17wA` -- reversed-Z GEQUAL with depth writes
     on -- with ops `KEEP/KEEP/REPLACE`, i.e. **pass-op only**. Such a draw
     changes the stencil only where it WINS the depth test, which is
     precisely the rule the tag design already relies on: a draw that
     overwrites a mover's tag has, by construction, taken that pixel's
     visible surface, and the zero it leaves in bits 1 and 6 means "no tag",
     which sends that pixel down the world path -- the right answer for a
     static surface. The remaining 23 are depth-DISABLED (`ds=02wA`):
     `FC1193AFFC596F74`, a full-screen quad (n<=6) that stamps ref 8 across
     the whole eye, and the GUI vector shader `666EF0C4C616F67E`. **Both run
     early in the scene pass** (q=585..1509 of frames spanning to ~3,800),
     before the movers, so a tag stamped after them survives them.

   **Why the other six bits are out**, each for its own reason and each
   measured in both flights:

   - **Bits 0, 2 and 3 are dead.** `E8FDC0D92EEBA6D7` is a full-screen
     triangle (`n=3`) with the depth test DISABLED (`ds=02wA`), running
     `w=0D` with fail-op AND pass-op REPLACE against reference 0. It zeroes
     those three bits across the whole eye unconditionally, and it runs at
     or near the end of the scene pass -- the literal last draw in three of
     the six passes seen, and 116 to 1,465 draws from the end in the others.
     Nothing survives it.
   - **Bit 5 is read and then wiped, by the same family in a pair.**
     `53211E8C072CD02E` draws twice back to back: first `ds=16wZ ref=0 r=20
     EQUAL`, which READS bit 5, then immediately `ds=02wA ref=0 r=FF w=20
     ALWAYS KEEP/KEEP/ZERO` -- full-screen, depth disabled, which ZEROES it.
     Both late in the pass. The pair appears in five of the six passes.
   - **Bit 4 is read late**, by the `r=10` NOTEQUAL draws, after any point a
     tag could be stamped.
   - **Bit 7 would produce false tags.** The `ref=149 w=95` family sets it
     to 1 on large static geometry (index counts to 101,697), so a reader of
     bit 7 would find "movers" all over the hull of a station.

   **The budget is two codes, not seven.** With bits 1 and 6, four codes
   exist. `00` is what every `w=FF` write with reference 21, 0 or 8 leaves,
   so it is "no tag" -- which is the graceful degradation the design already
   wants. `11` is reachable too: `6D8886012A4C6785` writes reference 255
   through `w=FF`, and although it is depth-tested and so only claims pixels
   it owns, those pixels would read as a tag. Reserve it. **That leaves `01`
   and `10`: two distinct rigid motions a frame**, against the seven the
   cost table assumed.

   Two movers is enough for the acceptance test -- a station at distance is
   one motion, and the cockpit already has the head path -- and it is not
   enough for the slot. So the honest position is that tier 2 ships for the
   case it was designed around and overflows in the busiest one, where
   overflow means the world path, which is what every mover gets today. If
   that proves too tight, **tier 2b's `R16_UINT` target carries 65,535 tags
   for the cost of re-rasterising the movers**, and the choice between them
   is now a measured trade rather than an open question.

   One thing the flight settled in the design's favour: the scene depth's
   bind flags are `0x48`, so `SHADER_RESOURCE` is set and step 4's stencil
   view is creatable directly, with no copy.

   **A consumer the so= column can never see, found and then measured**
   (2026-09-07). Everything above is a statement about DRAWS, because `so=`
   records a draw's depth-stencil state and **a compute shader has none**.
   Chasing an unrelated stale hash turned up two of the game's compute
   shaders sampling the scene depth through an `X32_G8X24_UINT` view -- the
   stencil plane -- which no analysis here had accounted for. They are
   `5998146D464F5C0E` and `EB0245DE0BB23BB6`, the amortised tile renderer
   `fss-scanner.md` already named as the FSS arrival content's real
   producer.

   Reading them needed the shader dump extended to compute, which it had
   never covered. Disassembled
   (`docs/shaders/tile-renderer-cs.asm`), the entire stencil use is:

       ld_indexable(texture2d)(uint,uint,uint,uint) r1.z, r1.xyzw, t4.xzyw
       and  r1.z, r1.z, cb1[26].y
       ieq  r1.z, r1.z, cb1[26].y
       if_nz r1.z

   `(stencil & M) == M`, with **M a runtime constant, so the bytecode gives
   the mechanism and not the mask.** Measured instead, through the census's
   compute constant-buffer watch, which already reads `CSGetConstantBuffers(0, 2)`
   and needed no change: **`cb1[26].y` is 0, raw bits `0x00000000`, in all
   four readbacks across two censuses and both eyes.** A mask of zero makes
   the test unconditionally true, so in that scene the renderer reads the
   stencil and ignores it, and constrains no bit.

   What the branch selects, for anyone judging the risk: the stencil test
   picks the LAST entry of a per-tile list when it passes and the FIRST when
   it fails (`umax count,1` then `-1`, against `mov 0`). So a mask that
   covered a tag bit would change which entry the renderer reads -- real
   behaviour, not a no-op -- which is why it was worth measuring rather than
   waving through.

   One limit stands: the value is a GPU copy taken a few frames after the
   dispatch, so it is strong evidence about the buffer rather than proof
   about that draw.

   *A recommendation withdrawn.* This first said to repeat the reading during
   an FSS scan, on the reasoning that `fss-scanner.md` names this renderer as
   the FSS arrival content's producer, so a mask might be armed only there.
   That was wrong, and the logs already said so: the dispatches appear in
   **three separate census logs across ordinary flight** -- a station at
   distance, an explosion, and a third scene -- with no scan involved, 12,
   24 and 16 of them. The FSS is where earlier work first *noticed* this
   shader, not a mode that changes it. Its constant block also reads like
   static tile geometry (`1, 0, 1/256, 256`) rather than a per-mode toggle.
   Nothing needs flying for this; if more confidence is ever wanted, leaving
   `census_cb_watch` set during ordinary play costs nothing and the value can
   be re-read from any later log.
2. **Whether the stencil survives to Submit.** From the same census: after
   the last scene draw, does anything clear or write the pair's stencil
   before EDVR's `mv` dispatch (which `review-ui-depth-2026-09-06.md:67-71`
   found to be the only reader of the depth after the UI)? The `f=3` clears
   at q=698/703 in census 4 are "the eye depth"; whether that is the scene
   pair or the composite's depth decides whether step 4 reads the stencil
   in place or copies it at the last unbind.

   **Answered YES, it survives** (measured 2026-09-07 by
   `tools/stencil_census.py` from `edvr_gfx_20260906_105837.log`, the
   2026-09-06 ui-depth flight, game build 332841, Pimax 2818x2784, five
   censuses with `census_offscreen = yes`). Two findings, and the first
   settles the question the design left open:

   - **The q=698/703 clears ARE the scene pair, and they open the frame
     rather than closing it.** In census 4 the pair is `@75` and `@83`, the
     two 2818x2784 `R32G8X24_TYPELESS` targets under `D32F_S8X24` views,
     and they are exactly the two the `mv` dispatch samples. The clear at
     q=698 is `DEPTH|STENCIL`, and that target's **first** draw of the
     frame is q=699. It is the scene pass's own opening clear, not a later
     wipe. The same shape holds in all three frames (q=570/571, q=625/626)
     and in every other census of that log.
   - **Nothing at all touches the pair between its last draw and the
     dispatch**, in 24 of 24 target-frames (4 censuses x 2 eyes x 3
     frames). In census 4 frame 0 the last draw is q=1771 and the dispatch
     q=1968, and no draw, clear, copy or other dispatch names that resource
     in between.

   Widening past that one log: across all 209 censuses, **every stencil
   clear of an eye-sized scene depth (258 of them) falls before that
   target's first draw of the frame.** None is mid-frame and none is after
   the last draw. So the plane the scene's draws leave is the plane that is
   still there at the end of the frame, and **step 4 can read the stencil
   in place; the rescue copy at the last unbind is not needed.**

   Split the claim, because its two halves are not equally strong.

   - **"Nothing CLEARS it" is airtight.** `ClearDepthStencilView` is
     recorded unconditionally on `g_framesLeft` and is not on the draw
     path, so no early return can hide one. All 161 `DCL D` lines in that
     log resolve, all 161 are `f=3`, and exactly two per frame hit the
     scene pair, always at the head of the pass.
   - **"Nothing DRAWS into it" carries a blind spot that must be stated.**
     `vscreen.cpp:1497-1499` returns `DrawVerdict::kParticle` *before*
     either census call, so a particle billboard the geyser fix substitutes
     is never written to the log at all. That session had the fix on and
     its own counter reports 4,904 replacement draws in the window holding
     censuses 3 and 4. So the denominator is "every draw, clear, copy and
     dispatch the census recorded, which excludes substituted particle
     billboards". It does not plausibly flip the answer -- a particle quad
     cannot clear stencil, only overwrite pixels it rasterises, and it
     belongs inside the scene pass -- but for a scheme that stamps tags
     into that plane it is exactly the wrong thing to leave unsaid.

   Three further limits. The `mv` dispatch exists only when the temporal
   pass is running, which is one log on this machine, so the 24 clean
   target-frames are one session in a cockpit. Census 5 of that log lost
   its intern table to the size cap, but its DSV side is still readable --
   a draw's `d=` and a clear's `dsv=` are the same pointer and intern to
   the same id -- and it gives four more clean frames and two partial ones,
   same build and session. And none of this says the stencil's CONTENT is
   useful, only that nothing erases it.
3. **The pool.** Its object at t33 (`VSGetShaderResources` at a scene
   draw), size, usage, stride, and its write path (the `DCW`, `DCC` and
   `U` census lines, `draw_census.h`), how many writes a frame, and whether
   a record keeps its index across frames (two frames' dumps, diffed:
   the same pose bytes at the same slot).

   *The instrument is built* (2026-09-07), and it had to be: **every
   shader-resource column this census has ever had was the PIXEL side** --
   `s=` is `PSGetShaderResources(0,8)` and `x=` is `(4,4)`. Nothing had ever
   read a vertex-shader resource, so the pool was invisible to every census
   ever taken and this question was unanswerable for want of one call. Two
   additions close it:

   - **`vt=`**, the vertex-shader resource window, slots 32..39 -- where the
     pool lives at t33 and the bone palette at t38, in 71 of 71 dumped
     shaders. One `VSGetShaderResources` on recorded draws only, the same
     bargain `x=` already makes, and the column is omitted entirely when the
     window is empty, which is most draws.
   - **`stride=`** on interned buffer lines. `binding_shadow` has resolved a
     buffer's `StructureByteStride` into `ResourceInfo::b` since it was
     written and the census threw it away, so a 336-byte pool was
     indistinguishable from any other buffer of the same byte width. Omitted
     when zero, which is every constant and vertex buffer.

   Between them a census now names the pool, sizes it, and gives its record
   stride. What still needs the flight is the rest of the question: its write
   path, how many writes a frame, and whether a record keeps its index
   between frames.
4. **The instance stream and the draw order.** Which slot feeds
   `INSTANCEANDMODELDATAINDEX` in the hull, station and asteroid families
   (the input layout at creation), its usage and write path, and whether
   the scene pair's draw order is stable frame to frame (the census's `q=`
   ordinals, diffed across the three frames of one census).

   **The draw-order half came back NEGATIVE from the field** (the two
   2026-09-07 flights, six scene passes), **and the memo of step 2 cannot
   work as written.** Draw order is not stable across seconds: two censuses
   of one scene three seconds apart agreed only 47-62% positionally, and the
   shape key the census could see -- kind, count, vertex shader, vertex
   buffer, stride, offset, topology, the VS constant object -- does not
   distinguish one draw from the next. `tools/draw_identity.py`, written
   2026-09-08, puts numbers on the same logs: on the scene pair that key
   names **3-4% of draws uniquely** (590 distinct keys over 2,106 draws, the
   rest shared by families of look-alikes), frame-to-frame order within one
   census is 92% positional, and across the three seconds between censuses
   the whole population's order agrees 0%. A memo keyed on that shape would
   answer wrong for 96% of draws and never know it. So: **the memo must be a
   one-frame carry, rebuilt every frame and never held across a scene
   change**, unless a per-draw identity exists that the census had not
   recorded.

   **What could still make one, now measured by the next flight.** The
   census's `DC` lines carry two columns since 2026-09-08, printed after
   `tp=` so nothing before them changes meaning: **`ia=start,base,
   startInstance`**, the draw call's own arguments (the start index, the
   base vertex -- or the start vertex, for the non-indexed kinds -- and the
   start instance), and **`ib=`**, the bound index buffer. Together with the
   shape they are exactly the key the design's memo proposed. The thunks
   have them in hand at no cost and pass them through rather than stash
   them (`vscreen.cpp`, `hookedDraw` says why). `tools/draw_identity.py`
   reports, per frame and for the shape key and the augmented key side by
   side, how many draws each names uniquely and how many recur next frame
   at all, at the same ordinal, and usably (unique in both frames) -- for
   every recorded draw and for the scene pair alone -- and between two
   censuses with every token resolved. **The one number that decides the
   memo's fate is the scene pair's `usable` share under the args key**: near
   the shape key's 3-4% and no per-draw identity exists, the carry is the
   design; near 100% and the memo is back, keyed on the augmented shape. A
   census taken before the column reads as "NO ia= COLUMN" and the args
   figures repeat the shape's, so the two cannot be confused.

   The instance-stream half (the slot, the usage, the write path) still
   needs the input layout at creation and is untouched.

   **ANSWERED 2026-09-08, from one flight with the column** (`edvr_gfx_
   20260908_090950.log`, `v0.14.1-20-g2bf4556`, five complete censuses with
   their intern tables, 2514x2482 under DLSS). The whole args key names
   83-86% of the scene pair's draws uniquely, so a per-draw identity exists
   *within* a frame -- and the field split says which field does what:

   | key, scene pair | unique | recurs next frame, quiet (censuses 1-2) | recurs next frame, in flight (3-5) | recurs 2.8 s later |
   |---|---|---|---|---|
   | shape | 6-9% | 98% | 94-99% | 91% |
   | + index buffer, start index, base vertex | 6-9% | 98% | 89-98% | 91% |
   | + start instance | 77-81% | 98% | 19-29% | 22% |

   The *geometry* is stable and shared: the same mesh drawn many times
   (`n=372 i=2 vh=6D8886… ia=15567904,2077572,*` over and over) is why it
   names nothing. The *start instance* is what names a draw, and it is not
   an identity but an **address in an instance stream packed afresh every
   frame**: monotonic through the pass (182, 301, 995, 1114 … 5927, 5955,
   6045 …), and shifted for every later draw whenever anything before it
   changes count -- `995 -> 994`, `5927 -> 5920`, `6045 -> 6038` between two
   consecutive frames of a scene with particles in it (censuses 3-5 had
   16-70 of them). In a quiet scene nothing shifts and it recurs at 98%.

   So the memo is dead as an identity and unnecessary as a mechanism. The
   draw's own start instance IS the record's location in the stream, handed
   over by the call for nothing; the stream's binding is one thing to learn
   per frame (it is append-only, so one `IAGetVertexBuffers` at the pass's
   first instanced draw, re-verified every hundred draws, is the whole
   learning cost); and the identity that has to survive a frame is the
   RECORD's, not the draw's. That moves the gate to **question 3's second
   half -- does a record keep its index in the pool between frames** --
   which the pool shadow answers at the desk-plus-one-flight: tee the
   pool's writes (it is Map-written, the 2026-09-07 censuses saw no other
   writer), keep last frame's copy, and print once a second how many of the
   first few hundred records changed bytes, changed pose only, or moved
   slot. If records keep their slots, the classifier is `record[i].pose`
   now against `record[i].pose` then and the design proceeds as written; if
   the pool is repacked too, objects are matched by content (bone base,
   scale, a pose within tolerance), which is the fallback to build then.
   Either way the draw-order and draw-identity questions are closed.
5. **The record head across families.** Dump the scene's vertex shaders
   (`glare_shader_dump`, `edvr.ini:1470-1472`) and look for the pattern:
   a structured load at stride 336, the position at byte 16, the subtraction
   of `cb1[275]`, the unorm16 unpack. Count the families that carry it and
   name any scene family that does not.

   **Answered from the desk, and it holds** (measured 2026-09-07). No
   flight was needed: `glare_shader_dump` ran on 2026-09-06 and left 224
   vertex shaders and 413 pixel shaders in `edvr_logs\shaders`, from the
   same session as `edvr_gfx_20260906_105837.log` (EDVR v0.14.0-12-gc8546cd,
   game build 332841). All 224 disassemble cleanly with `fxc /dumpbin`.

   - **Where the record is read at all, it is read identically.** 71 of the
     224 declare a 336-byte structured buffer, and it is a biconditional:
     exactly the 71 that declare an `INSTANCEANDMODELDATAINDEX` input.
     **All 71 carry the complete pattern and none carries part of it** --
     `t33` in 71/71, position at byte 16 in 71/71, `-cb1[275]` in 71/71,
     the unorm16 quaternion unpack in 71/71, bones at `t38` stride 48 in
     71/71. The design's `t33` and `cb1[275]` are not assumptions any more.
   - **Against the families that actually draw the scene**: 67 distinct
     vertex shaders draw into an eye-sized scene depth in that log's
     censuses. **33 of them carry the record, and those 33 are 82.6% of the
     scene's draws** (5,768 of 6,981); `t33` and `cb1[275]` in 33 of 33.
     The 34 that carry no 336-stride buffer are 17.4% of draws and are
     **the families tier 2 was never going to reach anyway**: the flight
     HUD (`B7790CBFC6554097`), the target indicator
     (`5DA53D8B0133341E`), the instanced-billboard particle cluster
     (`0357BBB2DEE43C1F`, `A1B7CFCD0BE7493E`, `8289669D93A18C1D` and five
     siblings, instance counts to 512), the witchspace starfield
     (`9AEC596A2B036EA6`) and the full-screen post triangles.
   - **No blind spots in that session; some across the corpus.** All 110
     distinct `vh=` hashes in the five censuses of that log have a
     `vs_<hash>.dxbc` on disk, so the intersection is complete *there*.
     Widened to every log on the machine, 144 distinct vertex shaders have
     driven an eye draw at some point and **40 of them have no dump** --
     other sessions, other scenes, and nothing measured excludes a
     different slot or rebase constant in those 40. Pulling the other way:
     48 of the 71 record-carriers do appear as `vh=` on eye draws across
     the corpus, so the record is not a dump artefact. The one *named*
     scene family with no dump is the sun-glare train, which that session
     never flew past; a second dump parked at a star would close it.

   Two method notes for whoever re-runs this. About a third of the dump is
   compiled with `[precise]` modifiers, which `fxc` prints *inside* the
   opcode and which split the unorm16 `mad` into a separate `mul` and
   `add`; a regex that does not allow for it reports 47 false "declares 336
   but never loads it". And the record's field *meanings* (byte 0 the bone
   base, 4 the scale, 8..15 the quaternion, 16..27 the position) are
   inferred from how each is consumed, not stated by the bytecode -- the
   offsets and the arithmetic are measured.
6. **How many distinct motions a frame has.** With the pool shadow
   running and nothing tagged, count buckets per frame in the three scenes:
   the bit budget against the demand.
7. **The rows' translation against the rebase origin.** The world path's
   `tvCam` already encodes one reading of the rows' translation term
   (verified in steady space, the far-warp review's cleared list); confirm
   that the same reading composes with `p - cb1[275]` before `M_obj` is
   built from it. A desk check against a captured frame's rows, origin and
   one record settles it.
8. **The game's own velocity, if any.** One census with Blur on: any
   eye-sized `R16G16` target written in the G-buffer pass (the interned
   table's `vf=`).

   **Answered NO, with one caveat that is the real finding** (measured
   2026-09-07 over every census on this machine: 80 logs with a census, 212
   complete captures, 49,360 interned texture rows, of which 12,587 at an
   eye size). This goes at the top because a yes would have changed the
   whole design.

   - **No eye-sized game velocity target exists in any census.**
     `R16G16_UNORM`, `R16G16_UINT`, `R16G16_SNORM`, `R16G16_SINT` and
     `R32G32_FLOAT` appear at *no size, in no log, ever*. The only
     eye-sized `R16G16_FLOAT` in the corpus is **EDVR's own**: two per
     census, written as UAV slot 3 of our motion-vector dispatch
     `6D94E9C00DCE909F`, named as such in the same log's prose. No game
     draw writes it.
   - **What does exist is a half-eye reprojection offset, and it is not
     what this question means.** At 1412x1392 (half the eye, rounded to a
     multiple of 8) there is an `R16G16_TYPELESS` under an `R16G16_FLOAT`
     view, one per eye. Its reader (`ps_30F73C5E4A94DB52`, disassembled)
     samples it, divides by an accumulation weight, **adds the result to
     the UV**, and then does three things only a reprojection offset does:
     bounds-tests the offset UV against the unit square, scales the
     history blend by the offset's length in pixels, and samples a depth
     at the *reprojected* UV to difference it against the depth at the
     current one as a confidence. It is nonetheless not a velocity buffer
     in this question's sense, on four measured grounds -- half-sized;
     **no geometry is ever rasterised at that viewport** (all 88 draws
     there are `n=3 i=1` full-screen triangles, against G-buffer draws of
     `n=111048` at full size); it sits in a self-contained half-res chain;
     and it is **cleared to (0,0,0,0) and read back in the same frame in
     22 of 22 recorded instances**, along with that pass's colour input.
     So Elite *allocates, clears and consumes* it; nothing in any log
     shows it *written*. Its chain composites with premultiplied OVER
     (`SrcBlend` ONE, `DestBlend` INV_SRC_ALPHA), so an all-zero layer
     leaves the eye untouched -- consistent with the whole effect having
     been idle in these captures, which is a flight question, not an
     analysis one.
   - **THE CAVEAT, and it weakens the negative: the census cannot see
     multiple render targets.** The binding shadow has exactly one
     render-target slot (`BindSlot::Rtv0`, `binding_shadow.h:58`); `r=` is
     that slot (`draw_census.cpp:654`) and the direct-read path asks
     `OMGetRenderTargets(1, ...)`. The `x=` column is pixel-shader inputs
     4..7, not extra targets. **A velocity buffer written as MRT slot 1, 2
     or 3 of the G-buffer pass -- which is exactly where an engine puts
     one -- is invisible to every census ever taken.** The repo already
     records this blind spot in another context (`resolve_probe.h:27-31`:
     the terrain pass's MRT slots 1 and 2, which "nothing has ever
     captured, in any state"), and it is demonstrable inside the very
     census under discussion: `ps_DFCBA0EC70B03C9B`, the draw at census 4
     frame 0 `q=1613`, declares `SV_TARGET` 0 **and** 1, and its census
     line records only `r=@89`. A second render target, proven by
     disassembly, on a line the instrument wrote down as single-target.
     So the honest form of the answer is **"nothing in render-target slot
     0, in any session"**, and the flight checklist below carries the
     census change that would close it.
   - **A second, smaller blind spot rides with it.** The census records a
     view's underlying resource but not its mip or array subresource
     range. That is what decides whether the 22-of-22 clear-before-read
     above means the buffer really was zero, or merely that a different
     slice of it was cleared. It belongs beside the render-target slots,
     not after them.
   - **One loose end worth naming.** An eye-sized `R8G8_TYPELESS` is
     present in most sessions (328 rows across 29 logs at 5424x5356, 236
     across 25 at 4340x4284). Two channels at eye size is the right
     *shape*; eight bits a channel is poor for velocity and right for SMAA
     edges, which is what `crisp_ui_gates.py`'s label guesses. That guess
     has never been checked against a disassembly, and it is cheap to
     check now that the dump exists.

   The Blur half of the question is unanswered and, as posed, unanswerable
   from these logs: **nothing in any log records Elite's own graphics
   settings**, so no census can be said to have been taken with Blur on or
   off. If the question is worth closing, it needs a deliberate A/B --
   one census with the setting on and one with it off, noted by hand.
9. **The bias mask.** `pInBiasCurrentColorMask` in the vendored NGX helper
   header, for tier 1 on the trained path.

   **Answered YES, at the desk, no flight needed** (2026-09-07). It is a real
   field of the D3D11 evaluation block -- `ID3D11Resource*
   pInBiasCurrentColorMask`, `nvsdk_ngx_helpers.h:152` -- and the helper
   passes it straight through as
   `NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask` (`:235`). So tier
   1's reactive mask has somewhere to go on the trained path as well as in
   the pass's own history, and the block EDVR already fills
   (`dlaa.cpp:362-376`) is the one that carries it.
10. **The learning cost.** Time `IAGetVertexBuffers` on a scene draw with
    the timing the totals line already uses, and the memo's hit rate over
    an interval, printed once.

### The flight checklist

The desk half of Phase 0 was done on 2026-09-07: the census column is built,
`tools/stencil_census.py` exists, and questions 2, 5 and 8 are answered as
far as logs already on the machine can answer them.

**The first flight went the same day and half worked**
(`edvr_gfx_20260907_182944.log`, build `v0.14.1-2-gdd8facb`, game 332841,
Steam install, eye 2514x2482). What it bought: the `so=` column came back
correct on real frames, and **question 1 has an answer** (above). What it
lost: five key presses produced two censuses, and both stopped at the 4096
line cap partway through frame 0, so neither wrote its intern table and
neither reached the `mv` dispatch. So question 2 could not be re-checked on
this build, and no `@N` token in that log resolves to a resource. The three
rows at the top of the table below are the cure; everything above them was
already right.

**Before launching.** In `edvr.ini`:

| Key | Value | Why |
|---|---|---|
| `advanced.census_lines` | `16384` | **the setting the 2026-09-07 flight died on.** The default is 4096 and one frame of a busy scene is about 5,500 events, so both censuses stopped partway through frame 0 -- and a capped census never reaches `finish()`'s intern table, so its `@N` tokens cannot be resolved to resources at all. 16384 is the ceiling |
| `advanced.census_frames` | `2` | 2 x 5,500 fits under the ceiling with margin; 3 does not. Two frames is enough for question 4's draw-order comparison and for question 2, which needs one COMPLETE frame -- the `mv` dispatch is the last thing in it, and a capped frame never gets there |
| `advanced.census_offscreen` | `1`, or `0` for headroom | measured 2026-09-07: **the scene's G-buffer draws land in EYE-SIZED targets and are recorded as `DC` whether this is on or off.** The scene depth pair carried 1,513 and 1,147 draws, all `DC`. What `1` adds is the shadow atlas (about 1,000 draws a frame, `DCO`), the UI surfaces and the half-res chain -- about 22% of the line budget, and none of it needed for questions 1 or 2. An earlier draft of this checklist said the G-buffer needed it; that was wrong |
| `advanced.glare_shader_dump` | `1` for ONE session, then `0` | question 5's disassembly. A dump already exists from 2026-09-06 (224 vertex shaders); a fresh one is only needed if the game build has moved |
| `fix.temporal_aa` | `on` | question 2 measures up to EDVR's own `mv` dispatch, which does not exist when the pass is off. Every census taken without it can answer question 1 and not question 2 |
| `hotkey.dump_draws` | bound, and **verify it before you rely on it** | the census key. On 2026-09-07 it was `NUMLOCK` and five presses armed two censuses. The two instruments that would explain that both stayed silent: no "already running; ignored" note (so nothing landed on a running census) and no "another window had focus" note (so the focus gate did not eat them). The DLL therefore never saw the other presses as edges at all. Press it once on the ground and confirm the "census armed" line appears before flying anywhere |
| `log.max_mb` | `32` | the default 4 lost a whole census to the size cap on 2026-09-06. A capture at the raised line cap is bigger again |
| `fix.particle_billboard` | `stock` for the census | the fix returns at `vscreen.cpp:1497-1499` BEFORE both census calls, so with it on `steady` its substituted draws are absent from the log entirely and the capture silently under-counts the scene. Set it back afterwards |

**The scenes**, one census each, held still for the frames the capture takes.
The order is deliberate: the first is the acceptance test and the cheapest
capture, and the rest widen the draw population from there.

1. **A station at DISTANCE, the rim in view.** This is the scene the whole
   design exists for, and the one with a number already attached to it: the
   far-warp review measured the rim at about half a pixel a frame at 5 km
   against a hub moving none, so the history smears the rim and not the hub
   (`review-temporal-far-warp-darkness-2026-09-04.md:286-296`). Three things
   make it first. It is the **acceptance test** -- everything else on this
   list is diagnostic, and this is where the feature either removes a visible
   smear or does not. It is the **smallest draw population** of any scene
   here, so it is the one most likely to fit inside the census line cap and
   come back with its intern table intact, which the 2026-09-07 flight did
   not. And it **bounds question 6 from below**: one or two rigid movers in
   frame, against the slot's many, and the tag budget has to cover both ends.
2. **The slot of a rotating station.** The largest mover any frame has: the
   interior fills the view and the ship counter-rotates to match it. The
   upper bound on question 6, and the densest census.
3. **A pad with the gear down.** Rigid near movers at scale, and the pad and
   the ship moving together.
4. **A wing-mate alongside.** A separate rigid body at a distance where its
   edges are the thing that shimmers.
5. On foot at a settlement, if there is time: skinned movers, for the root
   motion tier 2 reaches and the limbs it does not.

Take the distant station FIRST in the session, before the dense scenes: if
the line cap or the key turns out to be wrong, the one capture that matters
most is already on disk.

**The `so=` column is proven on real frames** (2026-09-07), so this step is
done and is recorded here only so nobody repeats it. It came back reading as
a coherent stencil state on every line, decoded by the tool without a special
case, and it immediately said something no earlier census could: that the
game's scene draws use write masks `15`, `05`, `95`, `0D`, `09` and `FF`,
almost all with read mask `00` and function ALWAYS. The one thing to keep
checking on a NEW game build is that the masks still look like that rather
than like garbage, which is one line read by eye.

**Afterwards, on the logs** (from the repo root; the logs are in `edvr_logs`
beside `EliteDangerous64.exe`):

```bash
python tools/stencil_census.py "<path>\edvr_logs\edvr_gfx_<stamp>.log"
```

That is questions 1 and 2: per depth target, the stencil states seen, the
bits read, the bits written, the bits free, and whether anything touches the
target between its last draw and the `mv` dispatch. Add `--listing` for every
draw and clear in `q=` order when a number needs explaining.

```bash
python tools/diff_draw_census.py "<log-a>" "<log-b>"
```

Question 4's draw-order half: two censuses of the same scene, and whether the
same draws appear in the same order.

```bash
python tools/draw_identity.py "<path>\edvr_logs\edvr_gfx_<stamp>.log"
```

Question 4's identity half, on a log from the 2026-09-08 DLL or later: per
frame, how many draws the shape key and the `ia=`/`ib=` key each name
uniquely, and frame to frame how many recur, at the same ordinal, and usably.
Read the scene pair's `usable` figure under the args key; the question says
what each end of it means. Take the SAME scene twice, a few seconds apart,
for the cross-census line.

```bash
python tools/crisp_ui_gates.py "<path>\edvr_logs\edvr_gfx_<stamp>.log"
```

The resource picture -- sizes, formats, view formats, who writes and who
reads -- which is how questions 3 and 8 are read off a census.

**Three census gaps, in the order they matter.** None is a new hook; all
three are things the instrument already has in hand and throws away.

1. **Render-target slots 1-3.** The census records slot 0 only (`r=`, from
   `BindSlot::Rtv0`, `binding_shadow.h:58`). A velocity buffer is
   classically slot 1 or 2 of a G-buffer pass, so that is exactly where
   question 8's answer would hide -- and a shader writing two targets on a
   line the census recorded as single-target has already been found in
   these logs (`ps_DFCBA0EC70B03C9B`). The `OMSetRenderTargets` hook
   receives the whole array and its count (`vscreen.cpp:2318-2320`), so
   this is a shadow widening and a column. Until it is done, "no velocity
   target found" means "none in slot 0".
2. **The view's subresource range.** Recorded resources, unrecorded mip and
   array slice. It is the one thing that decides whether the half-res
   buffer's clear-then-read means the buffer was zero or that a different
   slice was cleared, and the same ambiguity will reach any conclusion
   drawn from a view token.
3. **Substituted particle billboards are not recorded at all.**
   `vscreen.cpp:1497-1499` returns before both census calls, so with the
   geyser fix on, thousands of draws a census are absent from the log with
   no counter on the census's own totals line saying so. For a design that
   stamps tags into the scene's stencil, a class of scene draws the
   instrument cannot see is a hole worth closing before the flight, or at
   minimum turning the fix off for the census.

   *Counted since 2026-09-08, still not recorded.* The three returns that
   sit above the census calls -- the particle substitution, the withheld
   witchspace stars and the FSS chrome skip -- each note the draw they
   swallow while a census is recording; the frame and end lines carry
   `unseen=`, and a nonzero total gets one plain line at the end naming
   which fix hid how many. A census taken with `particle_billboard = steady`
   now says so in its own totals instead of quietly under-counting the
   scene. The cure for a census that must see them is unchanged: `stock`.

## Verification

**Desk, in `tools/smoke` and `tools/temporal_test`.**

- *The identities.* On the CPU, `M_obj` with `D = I` must equal the world
  path's delta bit for bit, and with `D` set to a captured cockpit part's
  motion must match the head's delta from the runtime's poses within float
  noise; the mapping tests at `tools/temporal_test/temporal_test.cpp:104-124`
  are the shape.
- *The tag moves the history.* The T2 fixture of the motion-vector review
  (a 400x304 region, `tan = {-1, 1, -1, 1}`): draw a quad at a known depth
  with tag 1 stamped through a twin state, load a table whose entry 1 is a
  10 px translation, and read the `MV` texture back: `(-10, 0)` inside the
  quad within 0.02 px, `(0, 0)` beside it.
- *The stencil view reads.* A twin of `edvrDepthProbeSelftest`
  (`tools/smoke/smoke.cpp:771`): clear an `R32G8X24` target's stencil to 5,
  read it through the `G8X24` view, expect 5 everywhere.

**Field.**

- *The candidate.* Put the records' delta in the registration
  instrument's retired slot 1 (`temporal_pass.cpp:54-57`) and judge it on
  tagged pixels against the world path; the line already reports clip
  share and size per candidate. The cockpit check -- records against the
  runtime's head, every frame -- is a second line: the angle and the
  millimetres between the two, which must read as tracking noise.
- *The view.* A fourth debug view (`advanced.temporal_aa_debug`) painting
  tagged pixels by tag, so a screenshot in the slot shows the station one
  colour and the cockpit another with no bleed across the canopy's edge.
- *The scenes.* The slot: the interior's panels hold still in the history
  while the ship rolls to match. The pad: the gear and the pad rise
  together without a trail. Alongside: a wing-mate's hull edges stop
  ghosting. On foot: an NPC's torso holds and the arms do what tier 1
  lets them.
- *The instrument.* Tier 3's residual map on two dumped frames from each
  scene, before and after.

## Update exposure

Everything the design reads from the game is a shape that a build can
move: the record head's layout (bytes 4..27), the t33 and t38 slots, the
`INSTANCEANDMODELDATAINDEX` semantic, `cb1[275]`, the rows at float 932,
and the free stencil bits. `docs/build-332753.md` is the re-check
procedure and each of these is a row in it. At session start the module
proves the pool before trusting it: a sample of records must decode to
unit quaternions within the unorm16 quantum and positions within the
rows' far plane of the rebase origin, and the cockpit's `D` must match
the runtime's head delta for a few frames; anything else stands the
feature down for the session with one line saying which check failed,
which is this project's contract for every read of the game.

## Phasing

1. **Phase 0**, the census columns and the flight. Questions 1, 2, 3 and 8
   decide the design; 5 and 6 size it.

   *The desk half is done* (2026-09-07). The census prints the stencil masks
   and ops as `so=`, `tools/stencil_census.py` reads them, and the questions
   the logs already on the machine could answer are answered above:
   **8 is no** (subject to the render-target-slot-0 blind spot), **2 is yes,
   the stencil survives**, **5 holds at `t33` and `cb1[275]` across 33 of
   the 67 scene families, 82.6% of scene draws**, and **1 has a provisional
   answer that argues for pessimism** -- every stencil bit is named by some
   draw's reference, so budget for tier 2b. What is left is the flight:
   questions 1 (the masks), 3, 4, 6, 7 and 10.

   *Updated 2026-09-08.* Question 1 is answered from two flights (bits 1 and
   6, two codes); question 4's draw-order half is answered and negative, and
   its identity half has its column (`ia=`/`ib=`) and its reader
   (`tools/draw_identity.py`) built and waiting for one capture of the
   distant station taken twice. The unseen-draw counter closes the third
   census gap as a count. Questions 3, 6, 7 and 10 need the pool shadow,
   which is tier 2's first stage, not a flight.

   *The flight of 2026-09-08, in space* (`edvr_gfx_20260908_090950.log`,
   five censuses), added one reader the station captures had not shown and
   re-confirmed question 2 on the new build. **Question 2:** the scene pair
   is untouched between its last draw and the `mv` dispatch in every frame
   of every census (the tool's `(d)` lines; the dispatch's content hash is
   now `860998BA3E70923D`, since the mover mask changed the shader, and the
   tool keeps every hash the pass has had). **Question 1's caveat:** in
   space `FC1193AFFC596F74` -- a three-vertex full-screen triangle, once
   per eye -- reads the whole plane (`ref=8 r=FF LEQUAL`) and replaces it
   through `w=FF` where it passes, which makes the tool's order-blind union
   read "no bit free". It sits at the START of the pass -- draw #23 of each
   eye's 656, with the narrow late readers at #490-492 -- before the six
   hundred scene draws that follow it. A tag stamped by any later draw is
   never seen by it; and by arithmetic a tag in bit 1 (+2) never changes
   its `>= 8` verdict while a tag in bit 6 (+64) always would, so bit 6 is
   safe only for draws after it and bit 1 is safe regardless. Every other
   reader is the narrow cast of 09-07 (bits 0, 4, 5 and 7). The two codes
   stand, with the rule that the first two dozen draws of a pass (the sky
   dome `84F6596FAF22CCFA` at #0 and the early instanced draws) cannot
   carry one; the tool now prints where in the pass each reading state
   first appears, so the next scene can be judged the same way.
2. **Tier 1**, about a day, behind its own key: the mask, the reactive
   weight, the bias mask on the trained path. It ships on its own merit and
   stays on under tier 2. *Built 2026-09-08* (`fix.temporal_aa_movers`, off
   until flown; the tier's own section says what to read).
3. **Tier 2**, behind `fix.temporal_aa_objects = off`: the pool shadow and
   the classifier first, with the candidate and the debug view and *no
   tags* -- one flight that proves the records against the head on cockpit
   pixels and counts the buckets; then the tags; then the resolve. Retire
   `temporal_aa_ship_metres` only after the candidate has won on a docked
   interval and in the slot.
4. **Tier 2b** only if question 1 says no bits.
5. **Tier 3** as `tools/` work against dumped frames, never on the hot
   path.
