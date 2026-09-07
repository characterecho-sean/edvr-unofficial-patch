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
Which bits those passes *read* is not recorded -- the census prints the
enable and the reference and not the masks (`src/d3d11/draw_census.cpp:839-856`)
-- and is Phase 0's first question.

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
time), with the census extended by two columns. Each question names what
answers it.

1. **Which stencil bits are free.** Extend the census's `ds=` column
   (`draw_census.cpp:839-856`) with the stencil read mask, write mask and
   the three ops, and list the masks of every draw and clear that touches
   the scene pair. The tag's write mask must miss every read mask; a
   lighting pass that reads 0xFF ends the stencil design and starts 2b.
2. **Whether the stencil survives to Submit.** From the same census: after
   the last scene draw, does anything clear or write the pair's stencil
   before EDVR's `mv` dispatch (which `review-ui-depth-2026-09-06.md:67-71`
   found to be the only reader of the depth after the UI)? The `f=3` clears
   at q=698/703 in census 4 are "the eye depth"; whether that is the scene
   pair or the composite's depth decides whether step 4 reads the stencil
   in place or copies it at the last unbind.
3. **The pool.** Its object at t33 (`VSGetShaderResources` at a scene
   draw), size, usage, stride, and its write path (the `DCW`, `DCC` and
   `U` census lines, `draw_census.h`), how many writes a frame, and whether
   a record keeps its index across frames (two frames' dumps, diffed:
   the same pose bytes at the same slot).
4. **The instance stream and the draw order.** Which slot feeds
   `INSTANCEANDMODELDATAINDEX` in the hull, station and asteroid families
   (the input layout at creation), its usage and write path, and whether
   the scene pair's draw order is stable frame to frame (the census's `q=`
   ordinals, diffed across the three frames of one census).
5. **The record head across families.** Dump the scene's vertex shaders
   (`glare_shader_dump`, `edvr.ini:1470-1472`) and look for the pattern:
   a structured load at stride 336, the position at byte 16, the subtraction
   of `cb1[275]`, the unorm16 unpack. Count the families that carry it and
   name any scene family that does not.
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
9. **The bias mask.** `pInBiasCurrentColorMask` in the vendored NGX helper
   header, for tier 1 on the trained path.
10. **The learning cost.** Time `IAGetVertexBuffers` on a scene draw with
    the timing the totals line already uses, and the memo's hit rate over
    an interval, printed once.

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
2. **Tier 1**, about a day, behind its own key: the mask, the reactive
   weight, the bias mask on the trained path. It ships on its own merit and
   stays on under tier 2.
3. **Tier 2**, behind `fix.temporal_aa_objects = off`: the pool shadow and
   the classifier first, with the candidate and the debug view and *no
   tags* -- one flight that proves the records against the head on cockpit
   pixels and counts the buckets; then the tags; then the resolve. Retire
   `temporal_aa_ship_metres` only after the candidate has won on a docked
   interval and in the slot.
4. **Tier 2b** only if question 1 says no bits.
5. **Tier 3** as `tools/` work against dumped frames, never on the hot
   path.
