# Per-object motion vectors: a design

**Reading this on 2026-09-11:** this is a historical design followed by a
flight journal; the opening's "Nothing here is implemented" describes its
original state. Later entries implement and revise several approaches.
For the current review of blur during movement, including two reproduced
origin-continuity bugs and a correction to the DLAA resolution test, see
[the distance-and-motion review](review-distance-motion-2026-09-10.md).
The [follow-up smoke review](review-smoke-voids-2026-09-10.md) checks the
branch through `44edb30`, verifies those two fixes, and examines the new
10:25–10:26 eye runs and draw ledgers.
The [latest-run and switch review](review-smoke-toggles-2026-09-10.md)
checks `794ee65` and the 11:14 capture, and documents the takeover changes
on `codex/smoke-temporal-history`.
The [12:02 capture review](review-smoke-capture-1202.md) retracts the
earlier ship-smoke-volume identification: that shader also renders
planetary bodies/rings. It documents the trails surviving both switches
and the particle families selected for a controlled diagnostic.
The [12:19 capture review](review-smoke-capture-1219.md) records that those
particle skips did not remove the confirmed blue-haze gaps. It connects
their AA-only occurrence to shared upstream processing and documents the
successful UI-depth-off diagnostic. The follow-up implementation moves
HUD/interface depth into private targets; the user confirmed that it fixed
the smoke holes with UI depth enabled again.
The [13:02 flight review](review-station-flicker-2026-09-10.md) investigates
the remaining whole-station flicker, reproduces a carried-motion origin
mismatch, and adds matching raw/treated captures with per-eye motion data.
The [13:33 UI and panel review](review-ui-panels-2026-09-10.md) identifies
two targeting-chevron motion errors, adds adaptive UI history, and fixes
paired crop coverage under DLSS for the remaining solar-panel investigation.
The [14:16 coverage review](review-ui-coverage-2026-09-10.md) reproduces
false chevron coverage against the game's shader, separates smoke from UI
history evidence, and repairs AA-off dumps. It distinguishes the approved
higher-resolution experiment from the unresolved loss of station detail
during temporal reconstruction.
The [14:53 station-depth review](review-station-depth-2026-09-10.md)
uses the new GPU inputs to identify near-plane depth beneath the chevrons
and the finite-far fallback misplacing distant station surfaces by kilometres.
It corrects the fallback and preserves original scene depth under floating HUD.
The [15:20 yaw and sprite review](review-yaw-sprite-depth-2026-09-10.md)
measures a false world-motion shutdown during opposing ship/head turns,
and traces persistent chevron depth one to the sprite vertex shader's
forced raster depth. It corrects both without changing resolution or sharpening.
The [September 11 panel and approach review](review-planet-panels-2026-09-11.md)
measures missing dim rank-label coverage and celestial expansion absent from
the supplied vectors. It rules out the cockpit split for these bodies and
adds draw-time transform and source-alpha capture; it is not a rendering fix.

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
- **The chooser fix flew the same afternoon** (`edvr_gfx_20260908_095212.log`,
  `v0.14.1-22-gdbcace9`): through the approach the world path was up at
  55-68% of pixels with the bound block chosen on 1776-1800 of 1800 frames,
  the resync firing five times during the launch transition and never
  after. And the station still looked "soft, slightly fuzzy on all moving
  surfaces under motion". Two things to know before reading that log's
  registration line. First, its `k` figures in flight (-0.3 to -0.55) are
  not a registration error: `k` projects the rows' residual against the
  head onto the head's turn, and in flight the residual IS the ship's
  turn, which anti-correlates with the head's whenever the head
  counter-rotates to keep a station in view -- docked and still it reads
  -0.003. Second, and the reason the question is still open: **no line in
  any log had ever measured registration on the trained path.** The
  "best match sat (x, y) px from the prediction" probes live in the pass's
  own shader entry, and the flown path is NVIDIA's. The `mv` entry now
  runs the same 5x5 luma SAD search against NVIDIA's previous output --
  its last frame is still in `dlOut` when the vectors are computed, bound
  at `t1` in the own history's place, stepped by the output-to-render
  scale so the window spans the same render pixels -- into the same
  stats slots, so the next approach prints the residual in pixels for the
  world, the ship and the sky. Read it against the candidates the eye
  cannot separate: DLSS at 50% per axis (this session's "performance"
  mode) is soft under motion by construction; station structure nearer
  than `temporal_aa_ship_metres` takes the head's delta and smears by the
  ship's own motion (the split's known limit, and tier 2's job); and the
  mask at strength 1.0 hands NVIDIA the fresh frame alone at its 0.4-0.5%
  of edge pixels, which at a 2x upscale is a soft edge. The first two
  have live knobs (`temporal_aa_movers = off`, `temporal_aa_ship_metres =
  10`, Elite's HMD Quality); the probe decides the third.
  *The probe's first reading* (the session of 11:12, `v0.14.1-47-gde86ca5`,
  DLSS at 50% per axis): docked and still, the ship's best match sat
  (+0.20, +0.22) px from the prediction; in flight the world with depth
  (+0.20 to +0.51, +0.29 to +0.47) px, the ship (+0.13 to +0.24, +0.18 to
  +0.24), the sky (+0.10 to +0.28, +0.03 to +0.24), `k` within 0.013 of
  zero everywhere. A quarter of a pixel of that on every class -- the sky
  and the still cockpit included -- was the probe's own: at a 2x upscale
  it read the output texel just past the render pixel's centre
  (`round((q + 0.5)·2 − 0.5)` is `2q + 1`, whose centre is `q + 0.75`).
  It now samples the output bilinearly at the pixel's own centre, the
  mean of the 2x2 it covers. With that bias off, the trained path is
  registered to 0.1-0.2 px with the scale right to 1%: **the softness is
  not misregistration.** What is left of the three is DLSS at 50% and
  structure inside `ship_metres` taking the head's delta; the mask was
  off for that flight, so not the mask.
- **The interface's text swam again with the mask on** (the session of
  10:43, after main's interface reactive mask had been merged in and the
  player had judged that mask good on main). Two things in the mask did
  it, both fixed the same hour. The fold handed NVIDIA `max(interface,
  mover)`: wherever the mover test tripped on a text stroke the
  interface's 0.5 became 1.0 -- fresh frame only, text that never
  accumulates, the swim `ui_depth` exists to fix. And the test trips on
  strokes by construction: "a surface now where only sky was" is for a
  hull's leading edge arriving over space, but `ui_depth` writes depth
  only under the strokes, so on every frame the head moves a stroke
  reprojects onto texels that had no depth last frame. Now the
  interface's value stands wherever it marked a pixel (that module knows
  its pixels and chose its strength with the player's eyes on the text),
  and the "only sky was" rule requires a THICK surface now -- six of the
  nine texels around the pixel with a depth: a hull, not a stroke or a
  wire -- with thin features getting the range test alone. The same rule
  is in `temporalMoverTest`, two more cases pinned. **Tier 1's standing
  after the day:** built, measured, cheap, and parked off by default. It
  cannot reach a mover's interior, and on the trained path it competes
  with the interface's mask for the one bias input NVIDIA offers; tier 2
  corrects the vectors instead and never touches that input, which is why
  it composes with the interface's mask by construction and is where the
  work goes next.
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
  *The A/B flew* (the session of 11:12, `transition_flash = 0`, nothing
  withheld all session): the long frames came anyway -- 328, 125, 67,
  108, 172, 474, 165 and 143 ms in the first two minutes at the station,
  then bursts of 13-30 ms in space. Not the withholds. What is left is
  the game's own streaming near a station, which no setting of EDVR's
  reaches; the coincidence in the earlier log was the station, not the
  detector.
  *The second reading* (the session of 11:36, still nothing withheld):
  99, 171 and 416 ms frames at the launch -- the monitor timed the 423 ms
  one with the render thread BUSY for 415 of them and EDVR's share at
  0.2 ms -- then 143 frames over 13 ms in the 35 s of the approach while
  the pool's live count climbed from 963 to 1563 with 80-110 records
  allocated and freed a pair: detail streaming in, which is how the
  player read it from the headset ("as more detail gets added to the
  station"). The monitor's long-frame line now counts the game's own
  creations in the frame -- textures, buffers, shaders, and their bytes
  about -- so the next such frame says whether it was streaming rather
  than leaving it inferred.
  *The third reading* (the session of 12:00), with those counts on the
  lines: the 154 ms frame at 12:02:15 created 681 buffers (612 MB) and 64
  shaders; the 113 ms one before it, 42 textures; the 32 ms one at
  12:02:52, 52 textures and 25 buffers (156 MB); the 28 ms one at
  12:03:17, 190 buffers. That is the station's detail level arriving --
  its meshes, its textures, and the shader permutations for their
  materials, which the driver compiles at creation -- on the render
  thread, as the player read it ("when a new LOD for the station is
  loaded"). One 85 ms frame at 12:01:28 created nothing, the exception in
  seventeen. Not EDVR's to fix. What EDVR owes it is not to make the frame
  after it worse, which is the history lag of the next bullet.
  *The fourth reading* (the session of 13:47): the same load at the same
  point -- 211 ms with 768 buffers (615 MB) and 74 shaders at 13:49:16 --
  and the approach's share of long frames the same as before (198 of
  2,589), though the player noticed less. The two minutes landed before
  it ran at 13 fps for a different reason: 75 ms waits in WaitGetPoses
  with the game busy 2 ms and nothing created, which is the runtime
  pacing the app -- a headset off the head or the dashboard up -- and not
  a hitch of anyone's.
- **On a hitch the station's turn seems to step back and resume** (the
  player, 11:36). The history lags the turn by construction: the vectors
  carry the camera's motion and not the station's rotation, so the
  history is blended in at the station's OLD angle every frame -- a
  hundredth of a degree at a station's rate, invisible -- and a 400 ms
  frame is tens of frames of turn at once, metres at the rim and pixels
  on the eye, which the blend then re-converges over its window: a step
  back and a catch-up. Tier 2's vectors carry the turn and close it. The
  A/B is the same approach with `temporal_aa = off`, on which the game's
  frame has no history to lag; the runtime's own reprojection through a
  hitch is rotation-only and would shift the whole scene, not the
  station's angle alone, which tells the two apart by eye.

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

   *Started 2026-09-08, the pool probe* (`advanced.object_probe`,
   `src/d3d11/object_probe.cpp`): the first stage's first instrument, and
   the one that gates the rest now that question 4 has closed. It
   recognises the pool from a scene draw's `t33` binding (a 336-stride
   structured buffer; a few reads a frame until found, one a second after),
   copies two consecutive frames of it on the GPU every eighth frame into
   a staging ring, maps them three frames later without waiting -- a
   mapped `WRITE_DISCARD` pointer is write-combined memory and reading
   megabytes of it on the CPU costs milliseconds, which rules out the tee
   the design first proposed for the pool -- and diffs the pair record by
   record: unchanged, pose changed with the rest intact, rewritten, found
   at another slot. The changed records' motions `D = W_prev · W_now⁻¹`
   are bucketed within a hundredth of a degree and a centimetre (the same
   `D` for every part of one assembly, so a station is one bucket), and a
   pair where more than half the pool took one pure translation is an
   origin rebase. The totals line every 20 s reads: **question 3** is
   "found at another slot" near zero (records keep their slots) or not (a
   repacked pool, and the classifier matches by content instead);
   **question 6** is the motions figure against the two codes;
   **question 7's first half** is the rebase count and size.

   *Its first flight* (11:12 the same day, `v0.14.1-47-gde86ca5`, the
   Steam copy): the pool was found at once -- 2048 records (0.7 MB), then
   a second pool of 4096 -- and no pair was skipped. About 180 records
   changed per frame docked, 1,150-1,300 EVERY frame in space, 1,900-2,100
   of 4,096 later; every changed record counted as "rewritten" and none
   as "pose only", so the game rewrites more than the pose each frame
   and the rest-of-record criterion never fired: nothing was bucketed.
   "Found at another slot" read 95-1022 per pair, inflated by slots
   freed to zeros matching every other empty slot. Neither question 3
   nor 6 is answered by it. The probe was corrected the same hour: an
   empty record is nobody (allocation and freeing are counted apart), a
   pose change is a pose change whatever else changed, a moved record
   needs its source slot to have changed since, and a per-byte change
   histogram over the interval prints as ranges with the share of
   changed records each range changed in -- the record's per-frame
   fields read off its behaviour, so the identity test can key on the
   bytes that do not move for a live object. The second flight's totals
   line reads: the ranges (which of the 336 bytes are per-frame -- the
   position at 16-27 if positions are camera-relative, and whatever
   else), "found at another slot" against the live count, the motions
   figure, the rebase count. Next in the stage: the instance stream's
   slot and element from the input layout at creation (one device-side
   hook), the record index read at each draw's own start instance, the
   classifier, and the records' delta as a registration candidate
   against the head on cockpit pixels.

   *Its second flight* (11:36 the same day, `v0.14.1-48-gc4622d3`; on the
   pad, out, and back to the slot): one pool of 2048 records all session,
   no pair skipped, and the byte histogram read the record's layout off
   its behaviour. Bytes 8-27 -- the quaternion at 8-15 as unorm16x4, the
   position at 16-27 -- change on 95% of rewritten records in flight, the
   low bytes every frame and the high bytes on 22-48% of them: a turn of
   0.2-0.4 deg a frame and metres of translation, which is the SHIP's turn
   and speed and no station's. **The pool's poses are in a frame that
   rotates with the ship**; on the pad with the head moving, 2 records
   changed a pair of 181 live, so not the head's frame either. Bytes
   288-319 change with exactly the first block's pattern (292-303 as
   16-27, 312-319 as 8-15): the record carries a SECOND pose. If it is last
   frame's -- the third flight compares it byte for byte with the slot's
   previous first block -- then the per-object motion is inside the record
   and no frame-to-frame identity is needed at all. Bytes 0-7 change on
   5-11% of rewritten records (the bone base and the scale), 30-55 on
   10-40% (per-instance parameters), and 28-29, 31, 56-287, 304-307 and
   320-335 never: the identity's signature, which the third flight keys
   on. "Found at another slot" read 0 whenever nothing was allocated or
   freed (the pad, the slot's approach) and 270-520 a pair while the set
   churned (80-110 allocated and freed a pair): **the pool is repacked
   when the visible set changes and stable otherwise**, so the classifier
   matches by signature, not slot. The exact-key buckets overflowed (64 of
   64, every interval in flight): the quaternion's quantum, 0.0035 deg, is
   six centimetres of translation at a kilometre, so one motion split by
   distance. Question 6 waits on the third flight's tolerance clustering
   (0.03 deg, 3 cm plus 0.2 mm per metre of distance), which also names
   the common motion -- the largest cluster's turn and translation, to be
   read against the ship's on the registration line -- and counts what
   lies outside it. And 175-206 records "allocated" a pair on the pad
   against 3-20 freed says the buffer is renamed: a dynamic buffer's
   unwritten slots hold whatever the allocation held last time round,
   stale records counted live; the third flight's engage line prints the
   usage.

   *Its third flight* (12:00 the same day, `v0.14.1-50-gd54c1ff`; distant,
   the slot, the landing pad): the pool buffer IS dynamic (cpu write,
   structured, a shader resource), so unwritten slots do carry stale
   records. **The second pose block is a copy of the record's own pose** --
   equal to this frame's first block on 100% of changed records in every
   interval, and to last frame's only where the pose had not changed -- so
   the game does not hand over the previous pose, with motion blur off at
   least: the field is shaped like a previous-pose slot, and one flight with
   Elite's motion blur on says whether it then fills. **The signature
   identifies the type, not the instance**: 0-8% of live records carry one
   no other does, so "kept at slot" (91-95% of changed records) is a
   type-level fact and instance identity needs pose continuity on top. The
   order is stable while the set is stable and reshuffles with churn: at
   the landing pad 820 records a pair were byte for byte at a new slot,
   static objects re-slotted. And the motions did not cluster even on the
   steady approach -- 63 clusters, the largest 45% at 0.02 deg and 0.6 m a
   frame against the eye's 2.2 m -- which no totals line can separate into
   a frame that moves, stale slots mixing multi-frame deltas, or a decode
   that is off (the decode matches the game's own in `fss_panel_vs.h`, whose
   `camRel = inst.pos - cb1[275]` also says the record's frame is not the
   camera's). So the probe now writes raw pairs to disk --
   `edvr_logs\pool\pool_HHMMSS_<frame>.bin`, a 32-byte header, the scene
   block from VS b1 with each frame, then the records; eight pairs a
   session, one every 30 s -- and `tools/pool_pair.py` answers on the desk
   what a flight's totals cannot: the camera's own delta from cb1[275]
   beside the records', the per-slot steps, the rigid clusters, and the
   same records matched by content instead of slot.

   *Its fourth flight* (13:47 the same day, `v0.14.1-59-g5ad454e`; landed,
   distant, the slot), the first with pairs on disk, read by
   `tools/pool_pair.py` -- and questions 3, 6 and 7 close on it. **The
   frame is the world's, the one the camera rows share.** The scene block
   the pairs carry is the big block the pass reads its camera from: rows
   233-235 are that camera, row 241 its world position, and rows 275-279
   are the HEAD's pose in the ship (cb1[275] reads (0.030, -0.002, 0.000)
   m, the pilot in the cockpit, its basis turning with the head). Three
   things place the records in the camera's frame and not the ship's: in
   flight (11064-11065) the camera's world position moved 1.49 m in the
   frame while the station's parts moved by a turn of 0.043 deg with a
   translation of 0.19 m, so the ship's motion is not in the records; the
   largest cluster's axis is the same on 11064-11065 and 13768-13769
   (within a few degrees) with the rate 0.035-0.043 deg a frame -- a
   station's axis, fixed in the world, turning once in a hundred seconds
   or so -- and the records' median displacement of 1.1 m a frame at 1.5
   km is that turn's lever arm; and on a distant pair (5784-5785) four
   records turned 0.207 deg a frame about another axis, the SHIP's own
   parts at the ship's yaw rate. (A first reading of these pairs called
   the frame the ship's, from the head's rows; the camera rows in the same
   block said otherwise.) A pair with the head turning and the ship
   moving 1.1 m (8360-8361, 1,876 live) had not one record change, which
   is a frame the game did not rewrite, not a frame that stands still.
   Docked, nothing moves (4680-4681): the game's frame co-rotates with the
   station once the ship is attached to it. **The slot is not an identity,
   even at rest.** Landed, ship still, set stable (4680-4681: nothing
   allocated, 11 freed), 160 of 279 live records sat byte for byte at a
   NEW slot and the nearest same-signature record was at the same slot for
   only 67: the pool is partly re-ordered every frame, so a quarter to a
   half of the per-slot deltas are two different objects' poses (medians
   of 90 deg and 20-70 m at rest), which is what filled the in-game totals
   with clusters. Identity is by content: the signature (the type) and
   the nearest pose within the frame's own motion, unambiguous for a
   static part since parts sit metres apart and move centimetres a frame.
   **The second block is the current pose**, every pair, 100%. So:
   question 3, no slot identity, content identity instead; question 6,
   one dominant rigid motion (the station) with a few small bodies beside
   it; question 7, the frame is the world's -- the camera rows' -- with
   no rebase seen. What follows for the design: the world path already
   carries the ship's own motion through the camera rows, so a static
   station part is registered by it, and the per-object work reduces to
   the STATION's own turn in the world -- one rigid motion for all its
   parts, which the pool's largest cluster gives directly, in the frame
   the pass already reprojects in -- plus the few other bodies.

   *Built the same afternoon, the body's path* (`fix.temporal_aa_objects`,
   the player's choice of the two ways to apply it; the stencil tag is
   not built). No tag and no draw classification, since the pool's slots
   are re-ordered every frame and nothing per instance survives to the
   next. Instead the largest rigid cluster's delta `[Rd | td]` -- a point
   at w now was at Rd w + td last frame, in the world -- is composed with
   the camera rows into a SECOND reprojection for every pixel the world
   path takes with a depth (`temporalBodyPath`: W = R_p^T Rd R_n, tv =
   R_p^T (Rd c_n + td - c_p), the camera path's own z-flip; with Rd = I it
   is the camera path, which the test pins). Per pixel the two landings
   are compared by a 3x3 luma SAD against last frame's image -- the pass's
   own history on its path, NVIDIA's previous output on the trained one,
   sampled at the render pixel's centre as the registration probe learned
   to -- and the body's is taken where it matches by a margin
   (`advanced.temporal_aa_objects_margin`, 0.85: the camera's is the
   default, and a flat patch that matches both stays with it); landings
   under a third of a pixel apart ask no question and pay nothing. The
   body comes from object_probe.cpp's pair diff (every eighth frame, held
   120 frames -- a station's rate is constant), gated as a body and not a
   scatter: forty records and a quarter of the pose changes at least,
   under a degree and twenty metres a frame. On the trained path it stands
   down on the frames the probe does (a debug view painting into the
   output, a restart, no history yet). Stats slot 29 counts the pixels
   that took it; the registration line prints the share with the body's
   records, turn and age, and `temporal_aa_debug = objects` paints them.
   What it does not do yet: a second body (another ship, the other
   station), the ship's own instanced parts (a small cluster at the yaw
   rate, which the head path already registers), and a diff off the
   render thread (the pair diff is a millisecond or two every eighth
   frame while the fix is on). The first flight reads the share against
   the slot's approach: a few percent far off, most of the frame in the
   slot, and the panels' smear gone with it.

   *Its first flight* (14:49 the same day, `v0.14.1-61-g2d8b165`; the
   player's A/B "didn't see much difference at all if any"): the path
   engaged -- the gates rejected the docked scatter (2% at 5-8 deg) and
   accepted the station in flight (31-71% at 0.034-0.056 deg a frame) --
   and took 0.00-0.69% of pixels. Two reasons, both on the desk from the
   flight's own pairs. The station was 10.6 km away for the A/B: at that
   range its turn moves its parts 0.07-0.11 px a frame, under the
   "no question" third of a pixel and under anything an eye or a history
   could show; the smear the player reported lives at the slot, where the
   same turn is 2-5 px a frame. And the choice could not have worked at
   any range: a 3x3 match against a history that was ACCUMULATED at the
   camera's landing is self-confirming -- its smear is centred exactly
   there -- so the camera wins every comparison, and the registration
   probe's near-zero residuals on the world path are the same fact from
   the other side. The composition itself is exact: on every pair the
   body path predicts a part's previous view position to 0.01-0.07 px
   where the camera path misses by the turn's lever arm, and it nails a
   passing ship too (19 records at 138 m doing 4 m a frame: 0.001 px
   against the camera path's 41). So the comparison went, and membership
   became GEOMETRY: the probe boxes the body's parts' positions (padded
   by `advanced.temporal_aa_objects_reach`, 60 m) and marks a 64-cell
   occupancy grid over the box, each part marking the cells within the
   reach of it; the pass uploads the grid when it changes and each
   world-path pixel with a depth is placed in the world by this frame's
   camera rows and tested against it -- one load, no comparison, and a
   ship crossing the empty space between the arms stays the camera's. The
   next flight's A/B belongs at the slot.

   *Its second flight* (15:28 the same day, `v0.14.1-62-gafc5d9b`; the
   A/B at the slot: "better with it off, on it shimmers more"). The log
   names the defect. During the A/B the body in hand read 0.0124 deg and
   0.000 m; the pair before it 0.000 deg and 1.570 m; the one before
   that 0.0373 deg -- while the interval's turn was a steady 0.021-0.023
   deg a frame. The clustering tolerance of 0.03 deg was WIDER than the
   turn, so every static thing in view merged into the station's cluster
   (the fifteen-kilometre box of the third flight), the cluster's motion
   was whichever record came first, and every eighth frame the
   station's pixels were re-registered by a different wrong vector, a
   pixel or several at slot range: the shimmer. Three corrections. The
   tolerance is 0.01 deg, three quanta, under the turn, and a cluster
   carries the running mean of its members' deltas. The motion handed to
   the pass is the least-squares rigid fit over ALL the cluster's parts'
   positions (`temporalRigidFit`: the small-angle w x p + t about the
   centroid, a 6x6 solve; the residual gates the body as one rigid thing
   at half a metre) -- one part's quantised delta is noisy by a quarter
   of the turn, the fit over hundreds is not. And the motion is exported
   as rates per millisecond over the pair's own interval and scaled by
   the pass to its frame's length, so a pair measured on a long frame no
   longer over-turns the frames after it; a body must also TURN (0.004
   deg a pair), since a pure translation is another ship or the player's
   own parts and handed the station a shift it never made. The
   registration line now prints the fit's residual and the pair's
   interval beside the turn. The player's second note -- "the station's
   parts near the outer edge shimmered as it moved" -- adds the rim: it
   is farthest from the axis and moves fastest, so a wrong rate costs the
   most pixels there; and the grid's box was cut to the parts' extent
   every pair, so its cell boundaries moved by up to a cell as members
   came and went and the pixels along the outer skin flipped between the
   body's vector and the camera's. The grid's cells now sit on a fixed
   world lattice (a power of two of metres a side, the box's corner at a
   multiple of it), so the same place is the same cell pair after pair.

   *Its third flight* (16:11 the same day, `v0.14.1-64-g790244a`; the
   slot, on): **"an improvement -- the textures on the face of the
   station resolve much more cleanly"**, with the outer rim's textures
   still vibrating every few frames and the turn stuttering. The log had
   the cause on its face: the pair's interval read 15.0 ms on two lines
   and 11.1 on the third. The stamps were the millisecond tick, whose
   grain is 15.6 ms, so a 90 Hz frame read as 15 or as nothing, and the
   rate handed to the pass swung by a quarter from pair to pair -- at the
   rim a step of a fraction of a pixel every eighth frame. Now the
   interval is QPC to the microsecond; the rates are blended into the
   held ones when a pair agrees with them (the same body at its constant
   rate), so a pair nudges the vectors and never steps them; the pass
   eases its own frame interval the same way, taking a frame a fifth
   longer or shorter as real; and the lattice's cell size holds unless
   the extent outgrows the grid or shrinks under a third of it. The
   cluster's extent was checked on the flight's pairs and is real: half
   the parts within 2.5 km of the centroid, ninety percent within 4, the
   far ones turning with the rest to a hundredth of a degree -- the
   station and whatever the game turns with it -- so the sixteen-
   kilometre cube stands, at 256 m cells. The body path's own accuracy on
   those pairs: 0.016-0.064 px.

   *Its fourth flight* (16:24 the same day, `v0.14.1-65-g6879098`; the
   slot, on): **"rim looks steady now. Much better"; at the slot "crystal
   clear."** The approach was "quite blurry", with "artifacts particularly
   with the 3d targeting UI for the station" and the target's text
   "smearing and shimmering" once targeted. The pair intervals read
   10.7-12.3 ms with decimals, the residuals 0.008-0.192 m, the share
   4-36% of the frame; the world path's own registration on the approach
   sat within 0.1 px. The targeting UI is the interface's: ui_depth
   writes the flight HUD's depth, so the station's brackets and its label
   sit at the station's distance in depth, inside its grid, and took the
   station's turn, which they do not share. The interface's own coverage
   mask (marked wherever the interface covers a pixel, by the draw that
   writes its depth) is now the exclusion on every dispatch: a pixel it
   marks keeps the camera's vector. Two things that are not the body
   path's: the text's SHIMMER is `advanced.ui_depth_reactive = 1` in the
   player's own ini (set that morning from the menu; at 1 the interface
   stops accumulating altogether, the sharp-but-shimmering interface the
   module's header describes, against the shipped 0.5), and the
   approach's general softness is DLSS at 50% per axis under 5 m a frame
   of motion, the candidate that has stood since the third flight of the
   day, with the station small on screen.

   *Its fifth flight* (16:42, `v0.14.1-66-gc245d88`; reactive 0.5 and
   then 0): the station clear, the bracket still "a quad that is blurred
   under the bracket" at either strength -- so not the mask. The player
   was right that the brackets have depth, and that is the fault: the
   flight HUD family draws into the scene's pair under ui_depth's WRITING
   twin, and its pixel shader (`8DEF46452FA459F5`, disassembled from the
   dump) is no vector rasteriser -- it marches a noise-modulated capsule
   for each stroke and emits the empty corners of the stroke's bounding
   quad at alpha nought without a discard, so the twin wrote the
   bracket's depth over the whole quad and the station beneath reprojected
   at it. The family had no coverage shader either, so the reactive mask
   never covered it, which is why the strength changed nothing. Fixed in
   ui_depth (the crisp-UI workstream's module, at the player's word):
   `kHudDepthHlsl` transcribes the shader's pre-march part register for
   register and bounds the march with a ramp to nought at q = 0.35, and
   the family goes through the second draw in the scene's own projection
   (`Mode::kReissueScene`) -- depth under the strokes' cores, the mask
   marked by the same draw, and the game's own draw left without a
   writing twin. What this flight taught about distance is recorded with
   the settings: the render is 2514 in both modes, DLSS is a 2x round
   trip through the supersample resolve (whose kernel had been the calm
   Gaussian since a menu toggle), and the levers are the crisp kernel,
   `texture_lod_bias = auto`, and Elite's HMD Quality.

   *Its sixth flight* (17:08, `v0.14.1-66-gc245d88`, the bias at auto):
   "the solar panels rotating on the station badly blur at distance and
   even at the mail slot they get crisp for a few frames and then jerk
   and blur and then get crisp again". The log read the body's
   translation term at 0.44, 0.29, 0.77 and 1.11 m from pair to pair at a
   steady turn, and the desk found the station cluster's fit at 0.35,
   0.49 and then 1.26 m with the residual tripled: contaminated. The
   contaminants are the next clusters -- 53 and 107 records turning 3.27
   and 0.46 deg about the station's OWN axis, whose rigid fits come out
   at 12 deg and two kilometres -- which are not bodies but SLOT SHUFFLES
   between neighbouring ring parts: the per-slot delta of two parts that
   swapped slots is a turn about the station's axis by their angular
   spacing, and the distance-scaled position tolerance (2 m at ten
   kilometres) let the near-angle ones into the station's cluster. The
   ship's own parts, meanwhile, are a clean separate cluster (75-109
   records within 100 m of the camera, co-rotating in the slot). So the
   fit is robust now -- fit, drop what the fit does not explain (a quarter
   of a metre, or three times the rms), fit again -- records inside the
   ship's radius of the pair's camera are left out of the body and its
   grid, and the tolerance's lever arm is halved. Distance is a different
   matter: at ten kilometres the turn is a tenth of a pixel a frame and
   the panels' fine structure is under-sampled by the 2514 render, which
   no vector can restore.

   *Its seventh flight* (17:21, `v0.14.1-68-g407cb11`, the robust fit
   and the HUD's coverage pass in, crisp on): "better, but the solar
   panels still appear jerky at any distance", with the mirror window
   showing the panel arrays on their long booms. The fit was clean now --
   residuals of five to eleven millimetres, the turn steady, the trim
   dropping nothing on the desk -- so the vectors were right where the
   grid claimed them, and the desk found where it did not: **548 of 956
   live records of the station's types sat outside its cluster** on the
   pair nearest the slot (153 and 222 on the two before). A record not
   rewritten that frame, or in a shuffled slot, is never a pose change
   and never a member, and the grid was marked around members alone; the
   dense core stayed claimed because some member always marked its cells,
   while a boom with a few records depended on those few being members
   THAT pair, so its claim flickered every eighth frame -- and the booms
   are the farthest from the axis, moving the most. The grid is now marked
   by every live record whose signature is one of the body's types (the
   fourth flight found the signature to be the type), inside the members'
   box and outside the ship's radius; the box, the lattice and the fit
   are the members' as before. The HUD's family line read as designed:
   "the flight HUD; its depth written by the coverage pass in the scene's
   projection".

   *Its eighth flight* (18:28, `v0.14.1-70-g4a3b71f`, the type-keyed
   grid): "panels still shimmer and do not stay crystal clear at all
   distances. Within a few hundred meters they stop shimmering." The
   fits are exact now -- residuals of one to ten millimetres, the turn
   steady at 0.039-0.047 deg, the claim 12-17% of the frame at the slot
   -- so the vectors on the panels are right, and what is left is
   SAMPLING. The panel struts are under a metre wide; beyond a few
   hundred metres they are narrower than a render pixel at 2514, so each
   frame's jittered sample lands on strut or gap by chance, the depth
   buffer flickers between panel and space the same way, and a pattern
   moving a non-integer number of pixels a frame cannot be accumulated
   by any reconstruction. That is the threshold the player describes:
   within a few hundred metres the struts reach two pixels and it stops.
   It is the distant-station shimmer the anti-aliasing work has carried
   as open since the temporal pass shipped, and its lever is input
   resolution (Elite's HMD Quality), or the calm kernel to soften it. The
   body path has done what it set out to do for one station: the face
   crisp, the rim steady, the booms claimed. One more thing the log
   showed: the body's translation term stepped between pairs (7.8, 4.3,
   1.2, 2.1 m) with millimetre residuals, which is the game's world
   ORIGIN rebasing during the approach -- question 7 revisited: it does
   -- and a held body is then in the old origin's frame for up to eleven
   frames. The flight's dumps settle it: between the 18:30:19 and
   18:30:49 pairs the station's moving parts' centroid moved 13.1 km in
   the frame while the camera moved 4.3 km, and the camera then stood
   fifty metres from the origin -- the origin had been put at the ship.
   Landed on the pad, the origin was at the station (its parts' centroid
   at nought, the camera a hundred metres off). The pass already flags a
   camera jump of over fifty metres in
   a frame; the body now stands down for twelve frames after one, and
   the registration line counts them. Not every jump is a rebase,
   though: the same log counted two jumps an interval at rest in the
   slot while the body's origin term stood at 3.06 km on three lines
   running, which is another camera's rows in for a frame and out again
   (the transition-flash instrument's "parked" cameras), and a body
   composed with those rows is wrong on both frames of the flip. A jump
   that lands within fifty metres of where the camera stood before the
   hold began is read as the flip's return, and the body is back the
   frame after; the frames whose delta the world path carried (another
   camera's rotation, over three degrees from the head's) stand the body
   down the same way, since the body's path takes the raw rows and the
   carried delta is not theirs. The player's word on the fifth
   flight's fix, given with this one: "The UI targeting symbols seem to
   be fixed" -- the HUD coverage pass holds.

   *Its ninth flight* (19:52, `v0.14.1-73-g6eac4de`, the camera-jump
   guard): parked some ten kilometres from the station and facing it,
   "if I hold my head still the station flashes between clear and
   blurry as it rotates; if I'm moving my head it remains blurry", and
   the target brackets artifact again. The log says why before any
   geometry does: the body's path took 0.06 to 0.37 percent of the
   frame at that range, against 12 to 17 at the slot, with the body in
   hand (335-477 records, fits to 2-7 mm) and the world path holding 66
   percent -- the station had depth and took the world path, and the
   grid said it was not the body's. The flight's dumps put the parts
   where the rows say they should be (833 of 838 in front of the camera
   read as world = M view + c, and 5 the other way round), so the
   transform is right and the DEPTH is wrong: the pass decoded the
   scene's depth with the planes the game asks the runtime for,
   0.025..50000 m, while the scene block's own projection row (row 198:
   [0 0 0 0.025] over [0 0 1 0]) is reversed-Z with no far plane at all,
   depth = 0.025 / z. The two agree to a part in a thousand at the slot
   and part company with distance: 3 km decoded as 2.8, 10 km as 8.3.
   Every world-path pixel of the station reconstructed 1.7 km short of
   it and outside every marked cell, and the body's per-pixel vectors on
   the few that got in were 17 percent too long. The pass now takes A
   and B from the block's row (latched with the camera rows, from the
   same write) and decodes z = B / (depth - A) everywhere it reads depth
   in metres: the history fetch, the mover mask, the debug views. This
   retires the eighth flight's verdict: the shimmer beyond a few hundred
   metres was not sampling, it was the body's path never reaching that
   far, and "within a few hundred metres" was the range at which the
   decode's error still fell inside the grid's cell. The brackets are
   the same story at that range -- the station under them unclaimed and
   smeared, the bracket's own region treated differently by the reactive
   mask -- and the coverage pass itself did not change between the
   flights; if they still artifact once the station is claimed at range,
   that is the next question. One more reading from the same dumps, for
   the record: the body's box is 16 km a side because the station's
   rigid cluster really spans 14 km along its axis in the pool (the hub
   and rings in a 7 km stretch, the spine's module clusters at 7, 9, 11
   and 12 km, 233 records of two types, all within 2.5 km of the axis
   and fitting the same turn to millimetres), so the sixty-four-cell
   lattice lands on 256 m cells; the marked region is the parts plus a
   cell either side, coarse but a superset, and a finer grid is the
   lever if a ship near a station is ever seen taking the station's turn.

   *Its tenth flight* (2026-09-09 04:42, `v0.14.1-75-g0714529`, the
   depth decoded by the game's own row): "much better! Most of the
   station is clear now", and three things left. First, "the lattice
   texture is blurred, but only partially ... a very clear angled
   delineation between clear on the left and blurred on the right,
   which also extends to the bottom half sphere": the grid's cut. The
   claim is per pixel by the pixel's own surface position against cells
   marked around each RECORDED part, and a station's biggest single
   parts -- the docking hub's skin, a ring's deck -- reach several
   hundred metres from where the game records them; with the box at
   256 m cells for this station and the reach at sixty (one cell either
   side), the hub's skin ran past the marked cells along a lattice
   plane, which is the angled line. Rebuilding the flight's grid on the
   desk from its 04:46 dump pair (the camera at the hub, 98 m off the
   axis), 31 to 60 of 1200 samples on the hub's skin fell outside it at
   one cell of dilation and none at two. The reach is now 400 m (two
   cells at 256 m, four at 128, seven at 64), and the dilation is three
   one-dimensional passes of a prefix count over the grid, so its cost
   is the grid's size whatever the reach. Second, "an occasional
   flicker where the whole thing blurs and then resolves sharp again":
   the twelve-frame stand-downs after camera jumps, 13 to 26 frames an
   interval on the line, each one the station dropping to the camera's
   path for a seventh of a second -- and the return of a flip was not
   being recognised either, since the ship had moved on before the rows
   came back. The hold is gone; in its place the body is taken up only
   when the pair's own camera position, carried in the motion record,
   stands within 500 m of this frame's rows -- the actual condition,
   since the box is in the pair's frame -- and a fresh pair satisfies
   it within a few frames. Third, the hangar: "close to my landing pad,
   details around the pad were blurred until I became latched", and
   the same on launch. Inside the ship split (100 m) every pixel had
   been the ship's, moving with the head, and a hangar's walls turn
   with the station until the ship is latched to the pad. The body may
   now claim from 50 m out: past the ship's own hull as seen from the
   seat, short of the pad under it, which stays with the ship's path
   until latched. The risk taken with it, to watch: a hull part farther
   than 50 m from the head, inside a station's marked cells, takes the
   station's turn -- a big ship's wing tips at the slot are the case.
   The record's exclusion of parts near the camera follows the same
   50 m. Also on the line: the body held "1289 records, 120 frames old"
   with nothing claimed through the docking, the last pair before the
   origin moved to the station; the frame test retires that too.

   *Its eleventh flight* (05:12, `v0.14.1-77-g1316a17`: the reach at
   400, the pair-frame test, the hangar floor): the angled cut gone;
   "still seeing the occasional flash of the whole station turning
   blurry"; the target brackets "have small blurry quads under each of
   the four brackets" while the label under the target does not
   ("though it does swim somewhat"); the hub's end sphere "still wasn't
   totally clear"; and "it started off blurry at a greater distance and
   then resolved once I got closer". The player had also set the ship
   split to ten metres on his own (`advanced.temporal_aa_ship_metres =
   10`) and found it "greatly helped with blurring on landing" -- which
   puts a hangar past the split and on the world path outright, the
   body's floor moot at that setting. The flash, from the line: "the
   body stood down 8, 25, 47 frames with its pair in another frame" on
   the approach and after launch -- the frame test doing what it said,
   the body down after each origin move until a pair taken in the new
   frame, up to twenty frames at the probe's cadence, each one the
   station on the camera's path. Replaced: the jump's own vector,
   summed over jumps, carries the held body into the new frame (the box
   by it, the translation term by (I - R) times it, the pair's camera
   by it for the test), cleared when a pair agrees unshifted; a flip's
   return sums back to nought. The body stands down on the jump frame
   itself only. The brackets: the log names an interface family
   appearing at the second of targeting, vs 81216C77F90DEDD6 with ps
   A2965EC2931A39C8 -- the HOLO MATERIAL, the cockpit panels' shader,
   which also draws the target markers instanced from the pool. Its
   disassembly: the alpha is the surface's own plus an eight-tap smear
   of it along a direction (the hologram's glow), discarded only under
   1e-5, and its depth was written IN PLACE under the writing twin, so
   each marker corner wrote its depth over the station around it in the
   glow's square, and the station there, reconstructed at the marker's
   depth near the axis, barely moved. The family now goes through the
   coverage pass in the scene's projection like the flight HUD (its
   stand-in takes the surface's alpha at the floor: the strokes, not the
   glow); a panel's translucent background under the floor keeps the
   scene's depth, a flat dark colour no reprojection can smear visibly,
   and the label was never the issue, text being the surface's own
   alpha at one. The sphere: the desk rebuilt the grid from the 05:16
   pairs and sampled a 500 m hemisphere at each hub end -- none of 200
   samples outside the marked cells at reach 400 -- so the grid is not
   the cut there, and the question goes to the objects debug view on
   the next flight (`advanced.temporal_aa_debug = objects`: the body's
   pixels white over the frame dimmed). The distance: the dumps show the
   pool holding 14 records until the station's 1070 arrive at 9.3 km;
   beyond that the station is not in the instance pool at all, and
   there is nothing for the body to fit.

   *Its twelfth flight* (05:48, `v0.14.1-78-g5f08a6c`, with the objects
   debug view up and a phone held to the lens): "a weird arc that
   shifts position in a fixed space", on the hub, and "other sections of
   the station further down look like this too". The arc is the reach's
   fringe. The marked cells are the recorded parts' cells plus the reach
   in whole cells either side, and the parts turn with the station, so
   the fringe steps a cell as records cross cell boundaries; where a
   part's skin lies near the fringe -- the hub's end sphere, five
   hundred metres from the one record at its centre, against a reach of
   four hundred that reaches 512 to 768 m at this station's cells -- the
   claim's edge runs across the skin and wanders with the turn. The
   desk's hemispheres had been centred inside the end record and never
   reached the fringe; re-sampled on the record itself they are still
   inside, which says the real sphere is a little larger than the guess,
   and the reach goes to 700 m (three cells here: 768 to 1024 m from
   every record). The same log put a second thing right: "the body stood
   down 16 and 25 frames with its pair in another frame" with nothing
   carried over, and no jump to carry. The probe stamped its pairs with
   the scene buffer's end-of-frame contents, which are whichever camera
   wrote it last -- another's, often enough -- so the pair's camera and
   the pass's rows disagreed with no origin move between them. The pass
   now hands the probe its chosen camera each frame and the probe stamps
   the pair with that: the same chooser on both sides, and a difference
   between them is an origin move and nothing else. And a third thing,
   asked for: a way to see what the player sees. `hotkey.dump_eyes` and
   the settings menu's "Dump both eyes as seen" write the next treated
   frame's two eyes, as the compositor receives them, to
   `edvr_logs\eyes\eye_HHMMSS_L.bmp` and `_R.bmp` -- a debug view can
   be read off the desk instead of photographed through the lens
   (`tools/eye_bmp_to_png.py` shrinks or crops one to a PNG to look at).

   *Its thirteenth flight* (06:11, `v0.14.1-81-ge403278`): "the arc I
   saw before is fixed", and the first eye dump -- the objects debug
   view at 5028 by 4964, the hub a white capsule with the ring's rim a
   thin white curve to its left and the ring's parts white to its right,
   the cockpit dim, nothing of the cockpit claimed. "The targeting
   brackets showed up still": at the hub's centre, four chevrons, each
   a dark hole in the white with a few pixels of halo -- the coverage's
   footprint at the floor, excluded from the body's path and handed to
   the camera's, which is a small smear under each. The exclusion was
   for the interface proper, whose pixels do not turn with a station;
   the markers should ride it, and the mask's value is the only way to
   tell the two apart at a pixel. So the holo material's coverage marks
   the mask three quanta of 255 under the strength, and the pass keeps
   off the body's path only what sits above the strength less a quantum
   and a half: the flight HUD's strokes and the composites stay off it,
   the chevrons ride it. NVIDIA reads three quanta as the same strength.

   *Its fourteenth flight* (06:23, `v0.14.1-82-g620bfce`): "no change
   to the brackets". The chevrons are not the holo material's: a chevron
   in the dump is two capsule strokes with rounded ends, which is the
   flight HUD's shader, and the log shows a third family appearing
   within a second of the target being taken -- the target-time sprite
   (`E508648660A352B2`, in place under the writing twin, its own alpha
   discard letting its soft fringe write the target's depth over the
   station). So the mask offset is by FAMILY now, not by stand-in, and
   all three families drawn at the target ride the body's path: the
   flight HUD's strokes, the holo material's markers, the sprite -- the
   flight HUD draws strokes and not text, so nothing that should hold
   still is among them -- while the composites (panels, labels) stay off
   it. The sprite goes through the coverage pass in the scene's
   projection too, so only its opaque core writes depth.

   *Its fifteenth flight* (06:39, `v0.14.1-84-gf31342f`), with two eye
   dumps: the chevrons "look better (more thinly drawn), but I still see
   some blurring around" them, and "the tips of the station structure
   ... do not seem to be included in the object". The dumps say what
   both are. The chevrons are dark in the objects view with no halo at
   all now, so nothing excludes them by the mask any more; they are
   unclaimed by GEOMETRY. A target's reticle sits near, in front of
   everything (the shader's own depth test is what lets it show over the
   station), and the coverage pass wrote that near depth under the whole
   of each stroke's glow band, so the station seen through the band
   reprojected as something twenty metres off and smeared around the
   chevrons under any head motion. The coverage now writes the stroke's
   own depth under its core alone (its alpha at seven tenths and over)
   and, under the fringe, the scene's depth read from the resolve and
   written back as it is; the mask is marked under both. The tips: the
   pool pairs around the dumps put the members' span at 13.6 km and the
   spine's last parts a kilometre or two beyond it, "shuffled" in every
   pair -- landing in new slots each frame, so never members -- and of
   the body's types, so the type-keyed marking would take them, but only
   inside the box, and the box was the members'. Any live record of the
   body's types within three kilometres of the members' box widens it
   now; a stray of the kind farther off does not. Fitting that in
   sixty-two cells would have doubled the cell to 512 m for this
   station, so the grid is 128 cells a side (2 MB): 256 m here as
   before, 32 m at a Coriolis, the dilation's cost being the occupied
   lines and not the reach.

   *Its sixteenth flight* (07:01, `v0.14.1-86-g22eacd9`): "it appears
   to have regressed", with a dump: the station seen through its rings
   from 8.5 km, the rings and the spine white, and the hub's central
   section DARK -- a long capsule with its radial struts, unclaimed. The
   desk rebuilt the grid from the pair nearest the dump and found every
   station record in a marked cell, so the dark is surface far from any
   record; and along the axis the station's core parts are recorded
   about a kilometre apart, one record per spine module, with the hub's
   central section recorded at its two ends 1.7 km apart. Its middle is
   850 m from any record. At 256 m cells the reach of seven hundred
   reached 768 to 1024 m and covered it by the luck of where the records
   sat in their cells; the finer grid gave this station 128 m cells and
   a reach of 768 to 896, and the luck ran out. The reach is a thousand
   metres now (eight cells here, 1024 to 1152 m), which is the rule the
   station sets: the reach must cover half the largest gap between a
   part's records, not the largest part's radius alone.

   *Its seventeenth flight* (07:11, `v0.14.1-87-gc177443`): "still
   seeing pieces of the station (including the tips) not included", and
   the chevrons "blurring and swimming themselves now", with a dump from
   9.6 km through the rings. The hub's central drum and its radial
   struts are dark in it AGAIN, at a reach of a thousand -- and the desk
   rebuilt the grid from the pair nearest the dump and found no point of
   the station's axis farther than 830 m from a keyed record, well
   inside the 1024 m the cells give. So the reach is not it, and the
   dark drum is unclaimed for some other reason: a depth the pass does
   not have (a far representation of the hub at that range, written
   somewhere the pass's depth is not), a depth that reconstructs outside
   the cells, or the mask. Rather than guess a third time the objects
   view now says which: under DLSS every world-path pixel the body did
   not take is coloured by its reason -- blue no depth, green the mask,
   red outside the cells, yellow inside them but predicted off the
   image -- and the next dump at the hub reads as a verdict. The tips
   in the same dump are the same question. The chevrons: with the flight
   HUD's fringe riding the body's path the reticle's glow moved with the
   station while its core did not, which is the swim; the flight HUD
   stays off the body's path again, its core with its own depth, its
   fringe with the scene's, so the station under the glow reprojects at
   its own depth with only the turn unvectored there -- the lesser of
   the two, by the player's own words.

   *Its eighteenth flight* (08:00, `v0.14.1-88-g089750d`), the verdict
   dump at the hub. Two colours, two causes. The hub's drum is GREEN:
   the interface's mask, which means a HUD element the size of the
   drum is drawn over it -- the docking hologram's strokes, with the
   docking granted -- and the pass kept the whole area off the body's
   path for it; inside the green, patches of the frame dimmed, where
   those strokes' cores carry their own near depth. The spine's cargo
   blocks carry RED stripes on their outer faces: depth that lands
   outside the cells. The blocks are recorded at the spine and reach
   well over a kilometre out, past a thousand metres of reach; it is
   now fifteen hundred (twelve cells here, 1536 to 1664 m), and the
   rule is restated: the reach covers the farthest surface from any
   record, whichever way the part hangs. For the HUD the third way of
   three: the coverage marks and writes depth under a stroke's core
   alone (alpha at seven tenths and over), and nothing under the glow,
   so those pixels are the scene's with the scene's depth and the
   scene's motion, the glow blended over them. The glow with the
   stroke's own near depth smeared the station through it; the glow
   with the scene's depth but marked and riding the turn made the
   reticle swim; marked and held off the turn it smeared the drum
   under the hologram. All three families drawn at the target ride the
   body's path where their pixels sit on it; a core that sits near
   reconstructs outside the cells and takes the camera's path whatever
   the mask says.

   *Its nineteenth flight* (08:1x, `v0.14.1-89-g90f845e`): "much
   better, solid white for the most part"; the tips red for a moment
   at first and then healed; "still bad swim/shimmering on the hud
   sprites"; and the occasional whole-station flicker, inside and out,
   once caught as the whole body going from white to totally red for a
   split second and back. Red for the whole body is a pair whose box
   is around something else -- its largest rigid cluster another
   object (a ship's parts on a frame the station's slots were
   shuffled), or one end of the station alone -- and the frame test
   cannot see it, since the pair's own camera is fine. Two guards. The
   probe keeps a body's parts' centroid relative to the camera (the
   floating origin's moves drop out) and a pair whose centroid sits
   more than two kilometres from the last body's, while that body is
   under sixty frames old, keeps the last body and says so in the log.
   And the box is grown until nothing of the body's types is left
   within three kilometres of it, so a pair whose members are one end
   of the station still boxes all of it; that is the tips' first-moment
   red as well. The sprites: a floating stroke's core written at its
   own depth of tens of metres reprojected under the SHIP's motion by
   whole degrees a frame, the history never matching, so it shimmered
   and swam. A core drawn at the surface (its own depth within half
   again of the scene's) keeps its depth; a floating core is written
   at one metre -- inside the ship split, so it reprojects with the head
   alone and holds still the way the game draws it. The pass gives
   ui_depth the depth value for a metre from the scene's own projection.

   *Its twentieth flight* (09:13, `v0.14.1-90-g7a5ea1b`), with a dump
   of the picture itself: the docking face crisp to its pad labels,
   the outer ring and the far booms smeared along the turn. The log
   has the reason in the fitted turns: 0.040 degrees a pair most of
   the time, 0.031 to 0.047 on others at the same interval, and 0.076
   over one pair of 6.1 ms -- the game's step landing late in a short
   frame -- and the rule had been that a pair disagreeing with the
   held rate by over thirty percent REPLACES it, as a new body. A late
   step then put vectors three times too long on the whole station
   until the next pair, and the outer ring, which moves the most, took
   the worst of every wobble: the "occasional flickers where the whole
   world object seems to blur", inside and out. A pair that disagrees
   with a rate the last three pairs agreed on, while the body is under
   sixty frames old, now keeps the held rate and gives only its
   positions, and the log counts it. Seen with it: ships' contrails
   over the station show red in the objects view -- they carry their
   own depth, nearer than the station and outside its cells -- so the
   station seen through a contrail takes the contrail's path and
   smears under it. That is the translucency limit of a depth-owned
   pixel, the same one the reticle's glow met, and it is left as it is.

   *Its twenty-first flight* (09:52, `v0.14.1-91-gb841283`): "much
   better"; the target indicator "still flickering a little bit and
   swims"; and the station's flicker "seems to correlate with movement,
   either my head or the ship relative to the station -- holding still
   doesn't seem to trigger it". The log put numbers under all three.
   The rate guard fired seventy-seven times in the flight: the fitted
   turn per pair is steady near 0.040 degrees while the pair's clock
   interval jitters (8.8 to 12.6 ms for the same turn), so a rate taken
   as angle over interval is noisy where the station is not, and a
   thirty-percent test on each pair is the wrong tool. The held rate is
   now the rolling median of the last sixteen pairs' rates, magnitude
   and axis apart; a pair far from it is counted and said, and a new
   body starts its own ring. The movement correlation: the body stood
   down on every frame whose camera delta the world path carried -- a
   fast head turn trips the three-degree test against the head's own
   delta -- and on every jump's frame, and each such frame dropped the
   station to the camera's path. The body now composes with the carried
   delta on those frames (temporalBodyPathCarried: its own turn taken
   into last frame's view by the rows, then the carried delta; the
   test pins it to the rows' form when the camera only turns), and
   stands down only when its pair is in another frame. The indicator:
   at one metre it sat on the ship's path and held under the head but
   not under the ship's turn, which the ship's path does not carry, so
   it swam whenever the ship turned; a floating stroke's core takes the
   scene's depth behind it now, where the camera's path carries a far
   point rightly under the ship's turn and the head's alike, with no
   parallax to speak of. Asked at the same time: the spars' white
   edge lines alias as they turn. That is the render's own sampling of
   a bright line thinner than a pixel at 2514, moving a fraction of a
   pixel a frame; no vector fixes it, and the levers are Elite's HMD
   Quality, the calm resolve kernel, and NVIDIA's preset.

   *Its twenty-second flight* (10:11, `v0.14.1-92-gcf226ca`), a dump
   of the picture: the docking face crisp to its labels from eight
   kilometres, the outer ring's rim single. Two things reported. "The
   targeting indicator is still not as steady/solid as it should be":
   its strokes were marked for NVIDIA at the same half-fresh strength
   as the panels' text, which is the right strength for a readout that
   changes in place and more than a stroke needs whose motion is now
   the scene's own; the flight HUD's strokes are marked at half the
   strength now, the panels and the holo material as before. And
   "hitching/blurring doing any high boost manoeuvres": the monitor's
   long frames in the flight run 22 to 29 ms and each names the game's
   own creations in it -- 8 to 50 textures and 18 to 59 buffers, up to
   250 MB in one frame -- which is the game streaming the station's
   assets as the ship covers ground fast, with the pass's own cost on
   those frames under a millisecond; the blur is the frame the game
   spent loading. The two 250 to 290 ms frames are the eye dumps
   themselves. The rate's rolling median held: two pairs in the flight
   sat 2.5 and 3.1 times the held rate and were counted, not adopted.

   *Its twenty-third flight* (10:40, `v0.14.1-93-ge06cfd7`; the dump at
   10:49 is of a targeted ship, not the station): "the target indicator
   on the station was still shimmering on just one side", and the ask
   that opens the next stage, "can we start the work to do the same
   thing with moving ships in our field of view? It probably only needs
   to happen within a certain distance, maybe 1k or less". The one side
   is the mask's doing, and the code says so without the dump. A
   stroke's core that floats takes the scene's depth behind it, and the
   flight HUD's and the holo material's strokes both sat under the mask
   value the pass read as "the interface proper", so they rode the
   body's path wherever that depth fell inside the station's cells: the
   bracket's side over the station's silhouette was carried by the
   station's spin, which a bracket tracking the station's centre never
   makes, and its side over the sky beside it kept the camera's path.
   The mask value's quantum now carries the word by its parity -- even
   rides, odd floats -- so each family keeps the reactive strength it
   needs and the pass leaves a floating core alone at any strength; a
   core drawn at the surface, the docking hologram over the drum, still
   rides. The moving ships are the section at the end of this document.

   *Its twenty-fourth flight* (11:19, `v0.14.1-94-g4e40a72`, the ships'
   first; dumps at 11:22 near the slot with ships): the probe took ships
   every pair from two hundred to nine hundred metres, four to
   fifty-nine parts each, fit to a millimetre or two, three to six
   metres a frame -- and the pass claimed a hundredth of a percent of
   pixels with one in hand at two hundred metres; the dump shows that
   ship's trailing edges doubled. The pair's positions were up to eleven
   frames old when applied, three from the copy to the diff and eight
   to the next pair, and a ship at five metres a frame leaves its own
   padded box in six. The box is carried now by minus the ship's
   translation times the frames since the copy (the age counts from the
   copy) and widened by a fifth of the way plus five metres. Two more
   things the log said. The cluster table filled every pair, sixty of
   its sixty-four clusters one record each: a record whose signature
   changed at its slot is another object's pose, not a motion, and is
   counted and not clustered now, and the table is 256 with the angle
   test a dot against a cosine. And the probe's thirty-second notes
   fired every pair, because `dueMs` reads its stamp and leaves the
   stamping to the caller; they stamp. One cluster of thirty parts,
   seven hundred metres long, turning at the station's rate and moving
   1.4 m, passed the slice test at five hundred metres once; a station
   slice fitted as its own body claims its own pixels with the
   station's motion, so it costs a line in the log and nothing seen.

   *Its twenty-fifth flight* (11:57, `v0.14.1-96-g779afd4`, on one ship;
   a dump at 12:01 of a security ship at 263 m, a sliver on the image):
   "weird rectangular artifacts in the smoke trail behind the ship, is
   there any way we can ignore those ... it's fine if that smoke is
   blurry". The probe took ships on six pairs in ten, up to eight in
   one, the nearest from 250 to 1000 m, and the table held them all (256
   deep now; the singletons it holds, 130 to 250 a pair, are the
   station's far parts split by the quaternion's quantum, not repacked
   slots, which the diff no longer clusters). The ships' share stayed a
   few hundredths of a percent, which a twenty-metre ship at three
   hundred metres would give if claimed whole, so the share alone does
   not say whether the hull is claimed; the objects view now paints a
   pixel in a ship's footprint that was not claimed teal (no depth) or
   magenta (a depth the claim refused), which the next dump in that
   view will settle. The smoke: the plume's particles are left in space
   and inside the box at the ship's depth they took the ship's motion,
   each quad's history fetched from where the ship had been. The claim
   is the parts' reach and the tail plane now (the section at the end
   says), and the plume keeps the camera's path.

   *Its twenty-sixth flight* (12:23, `v0.14.1-99-g1800d73`; a dump in
   the normal view at 12:26 and two in the objects view on a ship at
   12:27): "I still see the long rectangular shapes". The normal-view
   dump has them across a trail with no ship in hand -- the ship 1.8 km
   off, past the range -- so they are not the ships' path. A trail's
   quads write their depth over the whole quad, clear part and all, and
   the pass reprojects everything seen through a quad at the quad's
   distance: the stars behind it smear into a rectangle under the
   player's own motion, and the earlier "contrails overly blur the
   station" was the same depth over the station. That is a fix in
   ui_depth, whose hook already wraps every eye draw: a draw into the
   scene pair that samples the scene's depth (the resource the flight
   HUD binds at t0 for its own depth test, which soft particles read to
   fade near geometry) and is not the interface's has its depth write
   muted around the draw (`fix.temporal_aa_particles`, on; the game's
   state with its write off, cached per state as the writing twin is).
   The smoke then takes the motion of what is behind it, blurry as
   smoke, and nothing behind it tears. The objects-view dump showed the
   targeted ship magenta inside a teal box -- a depth the claim refused,
   or another ship's box over it (eight were in hand that pair) -- which
   the dump cannot tell apart, so the registration line now counts the
   footprints' pixels by outcome: claimed, without depth, outside the
   box at their depth with the mean offset along the ray from the box's
   centre, behind the tail, beyond the parts' reach. And the player's
   own parts are told by their motion now, not by a radius: a cluster
   whose translation over the pair is the camera's own is the player's
   ship, the ships' radius is twenty metres, so a ship within a hundred
   gets its path and a big hull's far parts (61 parts at 105 m moving
   13.6 m a frame, this flight) are not a ship of their own.

   *Its twenty-seventh flight* (12:49, `v0.14.1-102-gdb695f2`, near a
   trail and a ship; two dumps in the normal view at 12:53 and three in
   the objects view at 12:55). The ship is claimed: the security ship at
   six to seven hundred metres reads solid cyan in the objects view,
   which the counters could not say (they read eleven trillion pixels:
   the group counters past forty were never zeroed, and a ship within
   thirty metres put its box around the eye, so every ray hit it and
   the whole sky read teal; both fixed in the build after). The
   soft-particle mute engages -- the flight HUD's t0 is a 3743 by 3695
   R32 resolve, and thirty-five to a hundred draws a frame across a
   dozen pixel-shader families had their depth write muted -- but the
   trail's thin core lines still carry depth: white in the objects view
   where the station's grid reaches them, so their draw does not sample
   the resolve and was not among the muted. Whether the rectangles
   themselves survived the flight did not say; the smoke band beside
   the ship at 534 m reads soft in the normal dump. The draw that owns
   the trail's depth is the next thing to name, and the draw census
   (`hotkey.dump_draws`, once with no trail in view and once with one,
   `tools/diff_draw_census.py` between them) is the instrument built
   for exactly that.

   *Its twenty-eighth flight* (13:03, `v0.14.1-104-g8af0dd7`; three
   draw censuses, the first with no trail in view): "still see the
   rectangles in the smoke trails", and two things new since the
   ships: "the skybox stars are flickering now and I still feel like
   the station is juddering despite my frametime staying constant", with
   the ask for an adversarial review. Three reviewers read e06cfd7..HEAD
   (one file each) while the census was read. What they found:

   - *The cluster angle test was degenerate in float from the start.*
     A dot of two unit quaternions a hundredth of a degree apart is
     1 - 4e-9, under the float's own step below one (6e-8), so a record
     joined a cluster when its dot rounded to 1.0 and not otherwise --
     luck per record per pair -- and the old acos form and the cosine
     form both reduced to that; the 0.03 to 0.01 change of 2026-09-08
     was a no-op. The reviewer emulated it: at a station's rate the
     largest cluster held 51-60% of the parts and the rest split into
     135-194 clusters of one -- the "100-250 with under 3 parts" every
     report showed. The test is the distance between the quaternions'
     xyz parts now (half the angle in radians, every quantum kept), and
     the per-record angle is atan2 of xyz against w.
   - *The splinters were taken as ships.* Half of the station's
     secondary clusters passed the slice test (degenerate the same way)
     and got a one-pair rate, carried by the translation term's lever
     arm -- station pixels alternating between the held median path and
     a per-pair path every eighth frame: a judder at steady frame times.
     The slice test compares the candidate's rigid fit with the body's
     now, turn and term alike.
   - *The translation term is not the motion.* The fit's t is the rigid
     motion's term about the world origin, which carries (I - R) times
     the parts' distance from the floating origin: metres a frame for a
     turning hull kilometres out, in a direction it does not move. The
     own-parts test (the player's hull's far parts read as a ship moving
     13.6 m a frame on every turn), the still, speed and tail tests, and
     the box's carry all take the centroid's own motion -(w x c + t)
     now; the path keeps t.
   - *The station's frame test was too tight for a boost.* Five hundred
     metres between the pair's camera and the rows', with a body held up
     to 120 frames, is a second at boost: "stood down 10-28 frames an
     interval", each a frame the station fell to the camera's path. Four
     kilometres now; a rebase is thirteen.
   - *The ships' frame test corrupted the body's.* It cleared the body's
     origin shift when the ships' newer pair agreed unshifted, and shared
     the body's local shift. Each caller has its own now, and only the
     body's clears.
   - *The mute caught the wrong thing.* The census decodes each draw's
     depth state: the trail's haze ribbons (vs 0A298DE7DF833A46, ps
     6FD4C38BA927C8C7, up to thirty-four a frame near a ship) sample the
     resolve, blend as translucents, and carry depth test OFF with the
     write mask ALL -- a state the mute declined as "writes nothing".
     The one family it did mute was a depth-tested translucent drawn
     every frame near the eye -- the space dust, on the evidence -- which
     then flickered like stars. `fix.temporal_aa_particles` is off by
     default; its rule is test-off, write-all, translucent now, it counts
     only engagements, honours the exclude list, validates the resolve
     it learns, and costs nothing with the key off (its four view
     resolves a draw ran on every scene draw before). Whether Direct3D
     writes depth with the test off is what the next flight with the key
     on will say: the trail's core lines read teal in the objects view if
     it does.
   - Smaller: the along-ray counter overflowed int32 in a frame
     (clamped to a hundred metres); a ship claim whose prediction fell
     off the image now falls back to the body's grid; the largest
     cluster is a ship candidate when it was not taken as the body (a
     ship alone in open space); the rebase gate wants kilometres.

   Not done from the reviews: hysteresis on the flight HUD's per-pixel
   attached verdict (a path flip under a stroke over the drum), the
   footprint percentages' denominator, and the mask fold's `region.xy`
   offset on a side-by-side submit.

   *Its twenty-ninth flight* (14:06, `v0.14.1-107-ga701fec`): "stars are
   steady, station still judders a bit. CPU time was also high, I tried
   with aa particles on and off and saw no difference, the rectangles
   are still there. Can we remove those heatwaves entirely and just keep
   the smoke?" The stars settle the dust question. The angle fix moved
   the station's cluster from about half of its parts to 65-82% (948
   records in the body, its path 10-20% of pixels near the slot), but
   the pool still split into 125-154 clusters a pair: the translation
   term about the world origin carries the quaternion's quantum on the
   lever arm from the floating origin -- a metre at ten kilometres --
   against a position tolerance of three centimetres plus a tenth of a
   millimetre a metre, so the position test split what the angle test
   no longer did. The term is taken about the camera now, where the arm
   is the part's distance in view. The mute engaged on the haze ribbons
   with the key on (17.8 a frame) and changed nothing, so it comes out
   entirely; in its place `fix.heat_haze` (auto: withheld under the
   temporal pass; on; off) does not forward the three shaders the census
   named -- the refraction ribbons trailing a ship and the shimmer at
   its nozzles -- and the smoke, the glow and everything else stay. The
   CPU question gets an instrument: the probe's 20 s report says what
   the diff took on the render thread, a pair and at most, and the
   ships' member gathering is a counting sort in place of a scan of the
   pool per cluster (half a million steps a pair at 256 clusters). The
   judder that is left has no named mechanism yet; the next flight's
   cluster count and diff time say whether the pool's noise or the
   probe's own cost is in it.

   *Its thirtieth flight* (14:28, `v0.14.1-109-ga551ee4`): "heat haze is
   gone it seems, but cpu frame time is [high] and judders with lots of
   ships around I'm guessing. Maybe we should do a performance review?"
   The report's new figure named it: the diff took 2 to 9 ms a pair on
   the render thread on average and 13 at most near a station with
   ships about, half a millisecond with no body in hand -- a frame's
   budget every eighth frame, which is a judder at steady frame times
   and the CPU figure both. Nothing in the diff touches the device (the
   pair's copies are bytes once mapped; the results are a struct, eight
   ships and a grid), so it runs on a thread of its own now, below
   normal priority, one job at a time: poll copies the pair into the
   job, a pair that finds the worker still on the last is dropped and
   counted, and the results are published under a lock in short
   sections -- the body's struct with its grid's pointer (the grid
   double-buffered, so the pass's upload from the last pointer is never
   overwritten under it), the ships, and the ages the render thread
   counts. The report says the diff's time on its thread and the pairs
   dropped. The cluster count was 95 to 151 a pair still, with the
   translation term about the camera: the two quantised quaternions a
   delta is made of reach a hundredth of a degree between parts of one
   body often enough, so the tolerance is two hundredths, still under a
   station's own turn. A performance review of the rest of the hot path
   runs alongside.

   *Its thirty-first flight* (14:52, `v0.14.1-111-g7fddbe2`, the worker's
   first; a dump at 14:56): "much better, but still seeing some
   rectangles in the smoke. Also I think the heat haze effect is when
   the smoke fades out there's some shader that appears to render
   wrong". The worker took 4 to 10 ms a pair with none dropped, and the
   frame no longer pays it. The pool still split into 135-239 clusters a
   pair with the largest at 14-57%: the position tolerance, three
   centimetres plus a tenth of a millimetre a metre, equalled the
   quaternion quantum's worst case on the lever arm, so half of a body's
   parts fell outside it on every flight so far; five centimetres plus
   four tenths now. The performance review's per-draw findings went in
   with it: the vertex and pixel shader setters are hooked and the
   binding shadow carries the bound shader with its hash, so the
   billboard variant, the interface classifier, the scanner's chrome
   tracker and the skips read a pointer instead of asking VSGetShader
   (three device critical sections and Releases a draw, about two
   milliseconds a frame busy); the panel-size check returns early for
   any draw over sixty-four indices; the Map hook memoises a resource's
   kind and size. The remaining rectangles are in the dump: the smoke
   trail itself is a ribbon of fifty quads, additive, sampling the
   resolve and a 1024 by 512 streak (vs 5E417E9DF2E7F9E6, ps
   BD801F2FB02522EB), and its segments show as a chain of dark
   rectangles. It has no depth, so the pass carries it at the far plane
   while the ship's translation moves it, and each segment's fade
   accumulates a different history from its neighbour's. Two ways on:
   confirm it by withholding that shader alone (`advanced.census_skip =
   vs:5E417E9DF2E7F9E6`, an experiment, not a fix), and give the smoke
   its depth for the pass through the coverage machinery the HUD uses
   -- a depth-only second draw under the streak's alpha -- which needs
   its vertex shader's output layout from the shader dump.

   *The same afternoon, the pilot's A/B*: "I did an A/B with and without
   DLSS turned on (no TAA at all) and yeah the rectangles are an
   artifact of this code path. As are the shimmering haze fade out
   shader being noticeable." The dump held both of the smoke ribbon's
   shaders, so the depth-only draw is built: `tools/dxbc_disasm.py`
   reads a dumped shader through D3DDisassemble, and the smoke's pixel
   shader, register for register -- a sphere test and a soft fade
   against the depth resolve at t0, two scrolled samples of the streak
   at t1, the alpha their product -- became `kSmokeDepthHlsl`, the
   fifth coverage shader, clipping at eight percent of alpha (additive
   smoke is faint by design) and writing the quad's own depth. The
   smoke is a direct family of ui_depth under `fix.temporal_aa_smoke`
   (on), marked at one quantum, odd, so it keeps the camera's path at
   its own depth and is as good as unmarked to NVIDIA. The heat haze
   stays withheld under the pass; with its depth known the same way it
   could come back, but its shaders were not in the dump.

   *The thirty-second flight, 15:13 (v0.14.1-115)*: three dumps beside a
   ship's trail near the station -- the normal view, the depth view and
   the objects view. The smoke's family engaged at 15:15:18 ("the drives'
   smoke; its dense core's depth written by the coverage pass") and the
   trails carry depth now: thin dark lines in the depth view, red lines
   in the objects view, which is depth in hand and outside the body's
   cells, exactly what a trail that keeps the camera's path should show.
   The interface's passes treated 74 draws a frame; the clusters ran 38
   to 65 a pair with the largest at 75-79% (the rotation-vector test),
   and the worker took 10-11 ms a pair with nothing dropped. The pilot:
   "I'm still seeing the heat haze shimmer at a distance." The log
   agrees, and says why in its silence: the heat haze skip's thirty-second
   note, which the flight of 14:52 printed at 1, 5682 and 13524 draws
   withheld, never printed at all. The skip had not changed. What had
   changed underneath it was the perf build (v0.14.1-113): the
   VSGetShader per draw became a binding shadow set by the VSSetShader
   hook, with the content hash from a 32-slot memo, and the skip read
   the shadow. The smoke's family was recognised through the same shadow
   in the same session, so the shadow answers correctly for some draws
   and wrongly for the haze's, and a shadow that is wrong is a shadow
   that answers -- nothing logged a miss. The fix does not guess which:
   the skip's shape prefilter leaves a handful of ribbon-shaped draws a
   frame, and those ask the context again as the 14:52 build did, while
   the shadow's answer is compared with the context's and counted, by
   pointer (a set the hook never saw) or by hash (the memo's or the
   registry's), for the note to say. Underneath, the memo asks the
   registry again when its generation has moved (a destroyed shader's
   address comes back as another shader's) and when it holds a zero, a
   held zero falls back to the Get in every reader, and every 1024th
   owner draw audits the shadow against the context and reports a
   disagreement at most every thirty seconds. Built as v0.14.1-117-gd0beb52.
   The next flight's haze note carries the verdict in its bracket.

   *The thirty-third flight, 15:40 (v0.14.1-117)*: "the station is
   blurred upon my initial load", two dumps. The game loaded in at
   15:42:14 about ten kilometres from Macleod Market, the station seen
   face-on along its axis. For thirty seconds the pool held no station --
   193 to 283 live records, 145 to 208 clusters a pair, the largest at
   two or three percent -- so the body's path had nothing; then at
   15:42:46 the records came in, the body was taken (618 records, 63% of
   the movers, fit to 0.009 m) and the objects view of 15:43:19 shows the
   station claimed whole. The normal view of 15:43:30 shows it ghosted:
   the docking hub's face smeared tangentially by ten to fifteen pixels,
   the ring largely crisp at three times zoom. Neither the haze note nor
   the binding shadow's audit printed, so the fix of the flight before is
   untested still. The pool pairs on disk (`tools/pool_pair.py` and two
   desk scripts on top of it) say what the log could not: two rigid
   clusters at the station in every pair of this flight, 641 parts out to
   2.4 km from the axis turning 0.043 deg a frame one way and 211 parts
   within 475 m of it turning 0.044 deg the other, and the sense is in
   the POSITIONS -- each part's step about the axis, signed, is +0.044 for
   the one and -0.044 for the other -- not only in the stored
   orientations. The ring and spine, and the docking hub, counter-rotate
   in the game's records at this range. The hub's parts are of the ring's
   types, and the grid seeds every record of the body's types, so the hub
   sat in the body's cells and took the ring's path: wrong there by twice
   the turn, the smear the pilot saw. The flights before were closer,
   two to seven kilometres, where the same pairs show no second cluster
   -- the hub is not in the pool at all there, or is one mesh -- and the
   hub took the ring's path unpunished. Built as v0.14.1-119-gea86768: the
   second body (the section below). The haze and the shadow wait for a
   flight beside a ship's drives.

   *The thirty-fourth flight, 16:22 (v0.14.1-119)*: "took 3 eye dumps
   with dlss on, off, and TAA", the same load at ten kilometres. The
   station is crisp in all three -- the hub's face and the ring under
   DLSS at 16:24:39, under the pass's own history at 16:25:22 (the render
   at 2862x2826, "first treated frame ... history in R10G10B10A2"), and
   at 16:25:38 with a ship exploding beside the hub. Crisp at a glance:
   at twice the zoom the DLSS frame has the ring's spokes and panels
   resolved and the hub's docking face smeared in arcs, the same
   tangential smear as the flight before, while the pass's own history at
   16:25:22 has the hub crisp. The pilot's reading: "this feels like what
   the station would look like at close range before you fixed it --
   maybe the vectors aren't vectors at this distance?" For the hub they
   are not. The pool pairs of this session show ONE cluster at the
   station, 623 parts, and 271 to 298 records at the station's distance
   -- a contiguous run at the pool's tail, the hub's among them -- live
   in the SECOND frame of every saved pair and never in the first (824
   and 851 live against 1122; the probe's own line: 182 to 287 allocated
   a pair, none freed). The game writes those records on alternate frames
   only, so a pair of consecutive frames holds them once at most, the
   diff never saw the hub, and the hub took the ring's path, wrong by
   twice the turn as before. The flight before had them in both frames
   (25 in the second only). The hub counter-turns at range in every
   session, then; what varied was whether its records were diffable. So
   the pair is now two frames apart, from a start that alternates between
   even and odd frames: every other pair holds the hub twice, and the
   second body's hold spans the pairs between.
   What the detector did find, once a window, was a fragment of the ring
   -- 267 and 394 parts turning the body's own 0.042 deg, the cluster
   split at the table's overflow -- and took it, harmlessly, as a second
   body with the same path. Now a second body must turn otherwise than
   the body, a quarter of the body's turn apart at least, and the report
   counts the fragments. The heat haze's note printed at 16:25:32 on its
   first withheld draw with the bracket the fix of 15:13 added: 1,115,192
   ribbon-shaped draws asked the context in three minutes -- seventy a
   frame, not a handful -- and the binding shadow disagreed on none of
   them, by pointer or by hash. The shadow was never wrong; the 15:13
   silence was the drives' absence, and the distant shimmer of that
   flight is not the three haze shaders. So the skip reads the shadow
   again and checks one draw in sixty-four. Built as v0.14.1-121-gbc3db07,
   and the two-frame pair as v0.14.1-123-g553638e.

   *The thirty-fifth flight, 16:55 (v0.14.1-123)*: a census and a dump,
   and the pilot's crop: "the most obvious thing here is the solar panels
   blur as they move" -- the hexagonal arrays smeared along the ring's
   motion, the spokes beside them crisp. And on the counter-turn: "to my
   eye they all appear to spin in one direction." The two-frame pair
   held the tail in both frames now (nothing allocated or freed a pair)
   and no second body was found -- and 455 records held the same pose
   across the pair while the ring's 655 parts turned 0.075 deg: 310 of
   them beyond the pilot's own ship, 290 within 500 m of the axis (the
   hub's skin) and 19 at 1.2 to 2 km (the panel arrays), none at the
   ring's radius. The same parts that had smeared in every flight at
   range. The game updates them at a lower rate than the frame -- held
   across two frames here, present on alternate frames the flight before,
   and the "counter-turn" of the flight before that was such a part
   caught stepping back to an older buffered pose -- and what it draws
   for them steps or alternates, which no smooth vector can match, so
   DLSS smears them and the eye sees them spin with the rest. The second
   body was the wrong reading and stays only for a genuine one, now
   needing three pairs turning the same way. The fix is per frame: the
   pool copied every frame, each record's own turn measured against the
   body's as a multiple, an eight-frame history per slot, the multiple
   for the frame the pass draws next predicted from it (a stepping part
   repeats with a period of two), their cells stamped with it, and the
   pass reprojecting them by a table of twelve composite deltas, one per
   multiple. Built as v0.14.1-126-g79bd1b8; the 20 s probe line says how many
   parts step, in what pattern, and how often the prediction was right.

   *The thirty-sixth flight, 18:00 (v0.14.1-126)*: "no change to my eye."
   A short session -- the station in the pool from 18:01:50, the census
   and the dump at 18:02:12, the game closed at 18:02:21. The 20 s line
   had the machinery running: 86 stepped records a frame, 55 of them with
   a period of two, the next multiple predicted right on 97% of 2206
   checks, 0.16 ms a frame, cells stamped on every frame. And the
   registration line: the stepped parts took 0.00% of pixels. The stamps
   were made and no pixel took the path -- either they sit where no pixel
   with depth lands, or the path refuses them. The pairs on disk cannot
   say: two are from before the station loaded and the last caught the
   game frozen at exit, every record still. So this build asks instead:
   two counters straight from the shader, the pixels whose cell was
   stamped and those the path refused; the tracker compares each record
   with the last frame it was live (an absent frame is its pose held), a
   floor under the axis slack for the quaternion's quantum (357 records a
   frame had been turning "off the axis", the body's own parts in the
   quantum's noise), periods of one to four frames, the multiples'
   histogram and a sample of six stepped slots with their histories in
   the 20 s line, and beside each pair dump the last four consecutive
   frames, for what a two-frame pair cannot show. Built as
   v0.14.1-130-g03e84b2; the next flight's objects view, orange or not, and
   those counters decide where the fault is.

   *The thirty-seventh flight, 18:19 (v0.14.1-130)*: "captured a few eye
   dumps, saw flickering orange at times". The objects view has the
   station white with a few orange specks at the hub's edge: the stamped
   cells miss the hub's visible pixels almost entirely (0.000% of pixels
   sat in one), and the 20 s line's sample shows what they carried --
   slots at 9850 m alternating -3 and +8, the clamp's ends. The run of
   four consecutive frames on disk is the first direct look at the hub's
   records from one frame to the next, and it ends the stepped-parts
   reading: slots 815 to 837, at 340 to 420 m from the axis, turn 0.033
   deg one way and 0.033 deg back, frame about, with a 0.22 m tangential
   step and back -- no net motion at all -- while the ring's records turn
   0.05 deg a frame the same way every frame. The hub's records do not
   turn; the drawn hub does. The shader dump says how: every vertex
   shader that reads the pool (62 of them, a 336-byte structured buffer
   at t33) also reads a 48-byte bone palette at t0 and applies the pool
   record's quaternion over the skinned vertex, so a part's spin can live
   in a bone the pool never shows. Whether the bone turns the hub at the
   station's rate, steps, or turns at some other rate is not knowable
   from the pool at all, and the "stepped parts" machinery was built on
   the records' jitter. The stamps are off; the tracking stays for the
   20 s line. The dump key now takes an EYE RUN -- four consecutive
   frames of the left eye, copied to staging as they go out and written
   after the fourth, eye_HHMMSS_L0..L3.bmp -- and `tools/eye_run_spin.py`
   unwraps each frame into polar bins about a centre and cross-correlates
   two rings' angular profiles between frames: the hub's turn and the
   ring's, frame by frame, to a hundredth of a degree (a synthetic 0.05
   deg came back as 0.035 to 0.061). Built as v0.14.1-132-ge4eb2d2.

   *The thirty-eighth flight, 18:40 (v0.14.1-132)*: "took an eye run at
   the station. Interesting, it seemed to be flickering into sharpness
   briefly." The run, four consecutive frames at 18:43:05, has the answer
   to the flicker in it: frames 0 and 1 show the hub's face smeared in
   arcs and the ring soft, frame 2 is a RAW frame -- sharp, aliased, every
   tile of the hub's face distinct -- and frame 3 is the blur beginning
   again. NVIDIA's history was reset between frames 1 and 2, the raw
   render showed for a frame, and the history rebuilt on the pass's
   vectors. So the station's blur is accumulated history on wrong
   vectors, not NVIDIA's softness, and the flicker is the resets. The
   spin tool on the run, with the head's own one-pixel-a-frame drift
   removed, read the ring's turn at a hundredth of a degree a frame and
   the hub's face at a few hundredths, against the pool's 0.05 -- but a
   turn measured on NVIDIA's output is the history's reprojection as
   much as the object's, and the raw frames of the run are one and a
   half, not two. So the run now writes the pass's INPUT frames beside
   the treated ones, eye_HHMMSS_R0..R3.bmp, the game's render before any
   history, and the object's own motion is read off those; and the
   registration line counts NVIDIA's history resets and how many the
   openvr half asked for (a withheld frame, or a pose without a delta).
   Built as v0.14.1-134-gbf9df4c.

   *The thirty-ninth flight, 20:27 (v0.14.1-134)*: an eye run at the
   station, four treated and four raw frames at 20:29:20. NVIDIA's
   history was reset on no frame of the station's window (four at the
   load, two of them asked for), so this run had no raw frame among the
   treated ones, and the raw frames were the measurement. Too small a
   one: at ten kilometres the station is four hundred pixels across in
   the 2862 render, the ring 190 from the axis, and four frames are a
   0.15 deg baseline -- a tenth of a pixel on that ring -- under the
   aliasing of a raw frame. The spin tool read the hub's face at 0.05 to
   0.09 deg a frame and the ring at 0.02 to 0.05, with the three-frame
   totals disagreeing with their own sums. Not a result. So the run is
   long now: sixteen consecutive crops of the raw input, 1400 pixels
   square about its centre, written after the sixteenth with the first
   treated frame whole -- a 0.75 deg baseline, two pixels on the ring, and
   the pattern frame to frame. Built as v0.14.1-136-gafbdc50.

   *The fortieth flight, 20:36 (v0.14.1-136)*: the long run at 20:38:43,
   sixteen raw crops and the first treated frame, at about six
   kilometres this time, twenty-five seconds after the load. No history
   reset in the station's window. The treated frame is the best the
   station has looked at range: the ring crisp, the panels crisp, the
   hub's outer ring of tiles crisp -- and the hub's INNER face, where the
   docked and moving ships sit, smeared in arcs, which is the traffic
   (the pool's 33-54 records at 337 m from the axis turning 0.45 deg a
   frame) beyond the ships' stage's range, not the hub. On the crops the
   spin tool, with the head's shift removed, read per-frame turns within
   their own noise (the hub's outer face 0.03, the ring 0.025 to 0.03,
   scatter 0.03 to 0.05) -- at six kilometres the ring is 200 px from the
   axis in the crop and a frame's turn under a fifth of a pixel, which
   the aliasing locks toward zero. So the absolute rates are biased low;
   the RATIO between rings of the same run is the number to keep. From
   the ten-kilometre run of 20:29 the raw frames gave the hub's outer
   face 0.066, 0.088 and 0.053 deg a frame against the ring's 0.027,
   0.047 and 0.024: the hub about twice the ring, both biased alike. Put
   with everything else -- the hub's face and the panels smeared at ten
   kilometres and not at six; the hub's pool records jittering with no
   net turn while its drawn face turns; the pilot's "they all spin one
   way" -- the reading is that the station's FAR detail level spins the
   hub (and perhaps the panels) at about twice the ring's rate, through
   the skinning bone the pool never shows, while the near level spins
   everything as one. Under the pass's one rate for the whole body, a
   part at twice the rate smears by one rate's worth, and only at range.
   Not proven: the run that decides it is a long run at ten kilometres,
   the hub's face against the ring over fifteen frames, and a second run
   there with the body's path off for the baseline smear.

   *The forty-first flight, 04:16 on 2026-09-10 (v0.14.1-136)*: the deciding
   runs, both at the load position -- the pool puts the station's records
   at a median 10.0 km, and so it did for the 20:36 run, which the passage
   above wrongly calls six kilometres. A long run with the path on at
   04:20:13, the menu's "rotating stations" off at 04:23:13, a second run
   at 04:23:23. THE TREATED FRAMES LOOK THE SAME. By a fine-detail score
   (the Laplacian's variance over the region's, the treated frame brought
   to the raw's scale) the OFF frame is no softer and slightly sharper:
   the hub's tiles 1.27 against 1.00, its inner face 2.91 against 2.10,
   the ring 2.30 against 1.54, the raw frames 5.5-9.6. The path took 1.7%
   of pixels -- the station's -- and changed nothing the eye could see.
   So at ten kilometres the vectors are not the lever, though they are
   applied. The raw crops, measured three ways (tools/eye_run_fit.py, a
   rigid fit of each frame to the one before over an annulus of the
   2-px-blurred picture, the pass's own sub-pixel jitter falling out as
   the fitted shift, validated on synthetic rotations of the same frame
   smooth and re-rasterised to within 0.003 deg; the ring's and the hub's
   lights tracked as blobs; the angular profiles' correlation between
   frames 0 and 15), on both runs and on 20:36's: the HUB'S FACE turns
   about 0.035 deg a frame and the RING about 0.013 to 0.020, both
   clockwise on the picture, while the pool's records -- the 912
   unskinned and the 105 skinned alike, over the four-frame run of
   04:20:39 -- turn 0.031, 0.047 and 0.042 in successive frames, 0.040 on
   average. The hub's face turns at about its records' rate; the drawn
   ring at a third of it. Reprojecting both by 0.040 is nearly right for
   the hub and wrong for the ring by 0.025 a frame; reprojecting neither
   is wrong for both by less. Hence on and off alike. The 16:58 census of
   the day before, at this range, shows the far station drawn through the
   pool shaders (2966 instances a frame over 1334 draws) beside big
   instanced draws that read no pool -- 454 lights, 101 matrices at t0,
   1456 sprites, the 4784-instance starfield -- and nothing in hand says
   which of them paint the ring. Built: THE EYE RUN'S LEDGER
   (object_probe.h): the dump key's run also has the object probe keep
   every frame's pool copy, copy the scene's instance stream (the
   per-instance record indices every pool draw reads) and the first
   megabyte of the bone palette at VS t38, and note every eye draw with
   its shader, counts, start instance and whether t33 was the pool --
   written beside the crops, named by the run's stamp, for
   tools/eye_run_ledger.py: which records each draw took, how far each
   turned between consecutive crops, how the bones moved, and the big
   non-pool draws for the next look. Built as v0.14.1-139-g01463e1.

   *The forty-second flight, 05:17 (v0.14.1-139)*: one press at the load
   position with the path on, the run at 05:19:51, the ledger written a
   few frames after it -- twenty pool copies, nineteen instance streams,
   nineteen palette copies, 29254 eye draws, the crops C00..C15 at frames
   17771..17786. THE WHOLE STATION IS DRAWN FROM THE POOL: of its records
   within 500 m of the fitted axis 268 of 545 were taken by pool draws in
   both frames of every pair, of those beyond 800 m 176 of 288, and the
   drawn ones turned exactly as the undrawn -- 0.030 to 0.056 a frame,
   0.042 on average, the hub's, the ring's and the skinned records alike
   -- through the three big pool shaders (436193B352A2897E,
   EB5234DB6ADB491D, DE545DC8EE4FBB87) with meshes up to 14499 indices.
   The one palette copied at t38 read all zeros at the bases the hub's
   records carry: the census counts four 48-byte palettes and the hub's
   is another. The crops of this run measured like the others: the hub's
   face 0.027 to 0.037 a frame, the ring 0.010 to 0.020, against the
   records' 0.042; the cockpit and the stars in the same crops turn
   within 0.003 of nothing, so no head roll hides in it. The light
   tracker fails its own synthetic (a re-rasterised 0.046 comes back as
   0.017 on the ring, 0.003 on the hub: a sub-pixel light's centroid
   snaps to the grid), so its readings are withdrawn; the rigid fit
   passes the same test and stands. So the picture's slow ring is drawn
   by something that reads no pool -- the candidates are the instanced
   draws beside the station's: 512 and 57 lights (0357BBB2DEE43C1F, a
   per-instance stream and a world matrix in cb2), 180 matrices at t0
   (8289669D93A18C1D), 192 in a stream (A1B7CFCD0BE7493E), 83 of 112-byte
   records at t0 (963B52C73B4143AC) -- and at ten kilometres the lights
   are most of what the eye, and the fit, can see of the ring. THE
   SECOND LOOK (v0.14.1-142-g21bf0fd): every distinct palette the pool
   draws bind at t38 is copied each frame (up to four, bones<p>_), and
   each shader drawing fifty or more instances with no pool has, at its
   first draw of the frame, its cb2, its t0 and its first two vertex
   buffers copied (aux_<stamp>_<frame>.bin); the desk tool reads the turn
   of each such draw's world rows between consecutive crops, and the
   palette whose rows at the bases are rotations is the hub's.

   *The forty-third flight, 05:33 (v0.14.1-142)*: the run at 05:37:02,
   nearly face-on this time (the hub's face at the ring's centre), the
   ledger written with two palettes (both 8 MB, the game alternating) and
   NO aux file: the draws the capture was for bind nothing at t33, and
   the pool question's early return on an empty slot came first. And
   both palettes read zeros at the bases -- copied at the frame boundary,
   after the game had discarded and rewritten them for the next frame.
   The face-on crops by radius band about the hub's centre (974, 579):
   the hub's inner face 0.042-0.044 deg a frame (the records' rate), its
   outer face 0.032-0.037, the spokes 0.033-0.037, the ring 0.022-0.029.
   The synthetic check redone with the run's own jitter sequence, nearest
   and bilinear, returns 0.036-0.049 in every band for a true 0.043, so
   the shortfall is in the picture, not the fit; and the pool shader's
   position path (436193B352A2897E, read whole) is the record's
   quaternion, its scale, its position and the camera, no other
   transform -- a pool-drawn part turns exactly as its record. So the
   outer parts' slow turn is drawn by something that reads no pool, most
   likely the 512-instance lights, weighted more the farther out and
   fainter the structure gets. The pool flag in the ledger says only that
   t33 HELD the pool at the draw, true of every draw after a pool draw,
   the game never unbinding it; tools/eye_run_ledger.py takes --pool-vs,
   the hashes that read t33, instead. Built (v0.14.1-144-gd7c52cc): the aux
   capture before the pool question, and each palette copied at the
   frame's first pool draw that binds it.

   *The forty-fourth flight, 05:44 (v0.14.1-144)*: the run at 05:46:36,
   face-on, and the ledger whole this time -- twenty pool copies, the
   instance streams, both palettes copied at their draws, and nineteen
   aux files for the five shaders drawing fifty or more instances in a
   draw: 0357BBB2DEE43C1F x392 (the lights), 8289669D93A18C1D x146,
   A1B7CFCD0BE7493E x141, the 4784-instance starfield and the 1334
   sprites. THE LIGHTS: their constant buffer's rows 2-4 are a world-view
   matrix that turns only with the head (0.002 to 0.05 deg a frame,
   shared with 8289's draws); their per-instance stream, 32 bytes a
   light with a camera-frame position first, moves each light 1.3 to
   1.8 m a frame -- the station's rate at the ring's radius -- and the
   146 instances at t0 (a position, a size of 350 and parameters, not a
   matrix) 0.5 to 0.7 m a frame likewise. The pool records turn 0.040.
   And the crops, by radius band about the hub's centre: the inner face
   0.045 deg a frame, the outer face 0.040, the spokes 0.039, the ring
   0.039 -- everything drawn turns as its records do, and frame 0 against
   frame 15 magnified shows the ring's lights shifted the nine pixels
   the records predict. The earlier runs' slower readings cannot be
   re-examined: only this run's frames remain on disk, the rest deleted
   (not by the DLL). THE TREATED FRAME, with the path on and the vectors
   right, keeps 0.81 (the hub) and 0.91 (the ring) of the fine detail of
   a bicubic upscale of the raw frame, against 0.71 to 1.25 for the
   static cockpit dashboard: DLSS treats the station like anything else
   here. What the eye misses at ten kilometres is the LIGHTS: each is one
   pixel in the render, which the upscale reconstructs as a blob two or
   three wide, so the rows of lights on the ring and the hub's tiles fuse
   into bands, where the raw frame -- what the mod's AA off shows -- has
   single aliased pixels that twinkle with the jitter. That is the
   crispness the pilot likes, and it is aliasing. The model is already
   NVIDIA's K. The lever that is not sharpening: NVIDIA's
   bias-current-colour mask, which the interface already fills
   (ui_depth.h): marking the light sprites' pixels in it renders the
   lights from the current frame, as points, and leaves the structure to
   the history. Built next as a switch, off by default. Then the pilot
   named his real concern: THE SOLAR PANELS' blur, flickering into
   sharpness briefly. The panels in this run: the 117 outermost records
   (2174-2277 m from the axis), 71 drawn through 436193B352A2897E and
   EB5234DB6ADB491D -- opaque, depth written (the census's ds=17wA,
   bl=02), no discard in their pixel shaders -- turning smoothly at 0.040
   a frame with the body; frame 0 against 15 shows the array shifted the
   2.5 px the records predict; the path took 0.37% of the frame against
   the station's 0.29% lit. Vectors, depth and ownership all right, and
   the treated panel washed out while the ring beside it resolves: the
   lattice's period is near a pixel at this range, the game's own turn
   per frame is irregular by a fifth (0.031 to 0.052) which a rate-based
   vector cannot follow, and NVIDIA's model averages a near-pixel lattice
   as aliasing. The flicker is a history reset (a pose without a delta;
   two in twenty seconds in the 05:33 session) showing one raw frame.
   Offered as flights, not builds: DLAA (temporal_aa = dlaa -- WRONG as
   advised: the mode keeps the frame Elite submitted, at its 0.650
   fraction; the input test is Elite's HMD Quality at 1.0 with DLAA,
   verified in the graphics log's sizes; the review of 2026-09-10), the
   model J
   (temporal_aa_model = responsive), and an eye run in the objects view
   (advanced.temporal_aa_debug = objects) to see the panels white. Held
   back: vectors from the pool's actual step (two frames late), and a
   current-frame mark on the panels (the moire would crawl).

   *The forty-fifth flight, 06:07 (v0.14.1-144)*: "j actually looks much
   better with it. I did still see some flickering, can we fix that?"
   The session's log: NVIDIA's history was reset on four frames at the
   load and two at the pilot's own on/off toggle of the pass at 06:11,
   none in between; the camera's delta was dropped as another camera's
   on 30 and 12 frames in the load's intervals and on none while the
   station was in view. So the flicker under J is not the reset flash of
   the earlier flights and not a dropped frame: it is J's own trade,
   named in the ini -- it keeps more of each new frame, and a pattern
   near the pixel size twinkles as the jitter (Halton 2,3 over eight
   frames) moves its aliasing, the raw's own frame-to-frame change on
   the panels being a quarter of their contrast, the hub's and the
   ring's the same (tools/eye_run_shimmer.py on the 05:46 raw crops).
   The mip bias is -0.62 (auto, from Elite's 0.650 render fraction),
   which sharpens the lattice's texture below the render's own grid --
   more aliasing for J to show; a bias nearer nought is the A/B, at a
   restart. Built (v0.14.1-147-g35e77ff): the eye run's TREATED form,
   advanced.eye_run_treated = 1 -- the sixteen crops are NVIDIA's output
   about its centre (eye_HHMMSS_T00..T15.bmp), the first frame whole as
   before -- and tools/eye_run_shimmer.py, which aligns the frames and
   prints each region's change between consecutive frames and its
   per-pixel deviation over the run against its contrast, and shows a
   flash as every region changing at once. Two runs at the same spot,
   K and J, are the measurement.

   *The forty-sixth session, 06:21 and 06:36 (v0.14.1-147)*: "K seems fine
   too when stationary. When in motion, boosted flight, the station and
   especially the solar panels blur. Also still seeing that strange
   haze/blur shader" -- then: "the haze is from me turning around on my
   own smoke trail, but heat haze should not be a thing in space." The
   ribbon's vertex shader skipped by hand (advanced.census_skip) did not
   remove it, and the census taken with the trail in view says why: that
   shader draws the ribbon alone (two draws a frame, one pixel shader),
   and right after each ribbon draw comes a second one, vs
   203DF51758AADC4D, a 5334-index VOLUME under a scattering shader over
   the depth resolve, two gradients and a cubemap, premultiplied under the
   depth test with the write off -- four draws in two frames there, none
   in the census of the day before with no trail about. Built
   (v0.14.1-149-g0ce4d6e): fix.drives_smoke, on (the game's) or off (the ribbon
   and the volume withheld; the glow and the heat haze stay their own
   keys), live, with a menu row. "The station's arms are not lit properly
   anymore, did we break something?" -- not by the skip (it never touched
   another draw), and the arms' point lights (8289669D93A18C1D, sixty
   light volumes reading the depth resolve and the G-buffer at each pixel)
   are in the census; the check is the mod's AA off against on in the same
   view, since the treated frame is the only one on disk. THE BOOST BLUR:
   the boost intervals are the ones where the body stood down seventeen
   frames on origin jumps whose carried shift was wrong (the same frames
   dropped as another camera's) -- the candidate, unverified; the run's own
   write hitch shows in the pool as a 0.7 deg step in one frame, sixteen
   times the turn, which the rate times the frame's length cannot follow
   either. Asked for: two presses while boosting, treated and raw.

   *The review of 2026-09-10* (another agent's, docs/
   review-distance-motion-2026-09-10.md in its worktree; source 3340c87):
   two coordinate-continuity faults in the station's path, both confirmed
   here and fixed (v0.14.1-151-g944ba90). ONE: the pass added a camera jump of
   over fifty metres to the body's shift in EACH EYE's evaluation, on rows
   chosen once a frame -- the second eye doubled it (13 km became 26), the
   agreement gate at 4 km refused the held pair, and the station fell to
   the camera's path until a new pair agreed unshifted: the boost
   intervals' seventeen stand-downs, including the 06:26:09 interval that
   had refusals and no new jump. The jump is added once a scene frame now.
   TWO: the probe's sixteen-pair rate ring kept translations fitted in the
   OLD origin after the floating origin moved and medianed them with the
   new pairs' -- the fit's t is about the pool's origin, and the same turn
   reads t + (I - R) s after a move by s -- so the published translation
   was wrong for up to eight pairs (0.7 s) at a low residual, 9 m a frame
   at ten kilometres by the review's reproduction. Each held sample is
   re-expressed in the new origin (t -= w x s per millisecond) when the
   pair's camera moves by 200 m or more within the pair or since the last
   pair, before the new sample joins them. The registration line's per-eye
   counters say eye-frames now. Also taken from the review: DLAA at the
   present render fraction is not more input (above); a full capture
   should save raw and treated for the same frames with depth, vectors,
   ownership, the applied matrices, the pair's origin, the reset flags and
   the timing, batched; the rate predictor's eased interval and its
   5..50 ms clamp underpredict long frames; and the treated-frame
   sharpness scores say nothing about motion fidelity. The boost data
   asked for stands, now against the fixed build.

   *The review's second note, the trail's rectangles*: the trail is a
   ribbon of some fifty segments whose texture scrolls and fades along
   them, and three things expose their edges under the history -- the
   smoke and the stars behind it need different motion where one pixel
   holds both and the pass gives it one depth; the coverage writes the
   smoke's depth only above an opacity floor capped at 8%, so each
   segment steps between the smoke's depth and the sky's at its own time
   as it fades; and the smoke is marked at one quantum, as good as
   unmarked, so its scrolling texture accumulates in full. The proposed
   experiment: a stronger mark that follows the smoke's opacity, measured
   through a fade-out with the stars behind. Agreed, with one addition:
   the mark alone leaves the fringe to the floor's step, so the floor is a
   knob too. Built (v0.14.1-154-g4e3b074), defaults unchanged, both live:
   advanced.temporal_aa_smoke_floor (0.08) and
   advanced.temporal_aa_smoke_reactive (0 = the one-quantum mark; above
   it, the mark is the smoke's opacity times this, quantised odd so the
   pass keeps the camera's path). The pilot's own answer to the trail is
   fix.drives_smoke = off; the knobs are for the default's sake.

   *The forty-seventh session, 07:56 (v0.14.1-154)*: a census of the
   smoke trail, an eye dump of it, and an eye dump of "the station
   blurring under motion". The motion dump (08:02:17, 4.3 km, under way):
   the panels and the ring are CRISP in the treated frame; the smear is a
   long feathered band along the ship's own canopy frame -- the world
   uncovered behind the ship as it moves, disocclusion, which the movers
   mask (temporal_aa_movers, off) exists for; not the station's path. The
   smoke dump's crops did not contain the trail (the run crops the
   centre 1400 px of the left eye; the trail must be in the middle of
   the view). THE FLICKER UNDER MOTION, named: the openvr half's
   transition-flash guard withholds the first frame of every jump, and
   near a station the game moves its floating origin every few seconds
   under way -- 26 frames withheld this session, 36 jumps judged, every
   one "the camera did not return: a change of reference frame". Each
   withhold asked NVIDIA for a history restart on the frame after (the
   registration's "reset on 6 eye-frames, 6 asked by the openvr half" an
   interval), BEFORE the guard's verdict; under DLSS a restart is a flash
   to the raw frame and a third of a second of re-accumulation. Built
   (v0.14.1-156-ga9c3a7a): the detector's came-back verdict crosses the shared
   mapping (noteJumpVerdict, _v30) and the restart on a jump-withheld
   frame waits for it -- stayed keeps the history (the withheld frame
   never entered it; the pass carries the station over the jump itself),
   returned restarts it as before, no verdict within four treats restarts
   it too; holds, the theater and the FSS heal restart at once. The
   shutdown line counts the deferred restarts by outcome. No new keys.

   *The forty-eighth session, 10:19 (v0.14.1-156), and the review of the
   trail's voids*: the verdict gate flown near the station. Twenty-one
   frames withheld in pairs at the origin's moves, seven judged "the
   camera did not return", the body's stand-downs at the moves gone
   (0 in the 10:25:42 interval) -- and the registration still counted
   two to four eye-frames an interval reset at the openvr half's asking,
   which the line could not attribute (a withheld frame or a pose without
   a delta, it said) and the vr log's shutdown line never printed (the
   game ends without the DLL's detach). Built: the openvr half says why
   in the flags' bits 2-5 (a hold or a healed frame, a withheld jump the
   camera came back from, one left unjudged, a pose without a delta) and
   the registration line counts each; a 30-s vr note gives the deferred
   restarts' outcomes. THE REVIEW (another agent's, docs/
   review-smoke-voids-2026-09-10.md in its worktree): three findings on
   the smoke's coverage, all taken (v0.14.1-159-g627d62f). ONE, a defect since
   the coverage was built: it wrote the smoke's depth into the SCENE's
   depth target in the middle of the frame, and the game draws on after
   the ribbon with the depth test on -- the trail's scattering volume
   right after it, GREATER_EQUAL -- so an opaque depth surface under a
   translucent effect cut out whatever came later behind it, segment by
   segment: a plausible source of the dark rectangles, though the
   10:25:22 run had the gaps with no volume draw in its ledger, so not
   proven the whole cause. The coverage now writes into a depth target of
   EDVR's own (one per eye, the scene depth's size, cleared before its
   first draw each frame) and the pass folds it into the scene's depth as
   it reads (zSceneAt, t6: the nearer wins), so the vectors, NVIDIA's
   depth input and the depth view see the smoke where it is and the
   game's depth is never touched. TWO, the encoding: the ribbon's vertex
   shader adds 15.01 to the clip z before the divide, so the raster's z
   the coverage wrote depends on the matrix the game uploads (the
   review's synthetic put a kilometre at 1.7 m; the depth view's black
   trail says the real matrix compensates, since its palette turns white
   under 2 m). The depth is now encoded from the ribbon's own view depth
   (TEXCOORD1.z, the value its shader compares with the depth resolve)
   with the pass's projection pair, exact by construction; the raster's
   z stays the fallback when no projection is known. THREE, the fringe:
   the clip at the floor came before both outputs, so the faint smoke
   got neither depth nor mask, a step at the floor whatever the strength.
   Under the floor the fringe now writes no depth (the far value into
   EDVR's own target changes nothing) and marks the mask only where the
   strength gives it a quantum, so the mark fades with the smoke. The
   review's decisive follow-up stands and is the next flight: with DLSS,
   drives_smoke on and the heat haze as it is, two eye runs of the trail
   mid-view with advanced.eye_run_treated = 0 (raw crops and a treated
   overview), fix.temporal_aa_smoke on then off -- raw holes that vanish
   with the coverage off implicate the injected pass, clean raw colour
   with gaps in the treated frame implicates the history, holes in both
   implicate the game's own smoke draws.
4. **Tier 2b** only if question 1 says no bits.
5. **Tier 3** as `tools/` work against dumped frames, never on the hot
   path.

## The moving ships

Asked on the twenty-third flight (2026-09-09): "the same thing with
moving ships in our field of view ... within a certain distance, maybe
1k or less". The pieces were in hand. The probe clusters every pose
change of a pair by its rigid delta and took the largest cluster as the
body; the others were counted and dropped. A ship in view is one of
them: its parts are records of the same pool (the player's own ship is
a hundred and thirteen of them within a hundred metres of the seat),
they move as one rigid thing, and their delta is nothing like the
station's. So the stage takes, from each pair, every other cluster that

- has at least three parts outside the player's own hundred metres (the
  rigid fit's floor; a ship of one or two recorded parts is not taken),
- moves -- a turn of 0.004 degrees or two centimetres in the pair; a
  cluster that does neither is the world's, and the camera's path has
  it,
- is not a slice of the station: a cluster within twice the angle
  tolerance and four times the position tolerance of the station's own
  delta, whose parts split from the station's cluster by the
  quaternion's quantum on their lever arm from the origin, and whose
  pixels the station's grid claims already,
- sits within `advanced.temporal_aa_objects_ships_metres` of the camera
  (a thousand by default; nought takes none), and
- fits as one rigid motion to half a metre, the same least-squares fit
  as the station's over its parts' positions,

nearest first, eight at most, and hands them to the pass with the
pair's camera position. On a rebase pair (the live records shifted
together without a turn) it takes none: the deltas are the origin's. A
pair that finds none leaves the last pair's ships to age out over three
pair intervals, so a slot shuffle that hides a ship for one pair does
not drop it.

Each ship's rate is its own pair's. A ship's motion changes, so the
sixteen-pair median that steadies the station's rate has nothing to
work with; what the ship borrows from the station's lesson is the
interval, the pair's delta divided by the median of the last sixteen
pairs' clock intervals instead of its own (the game's step per frame is
steady where the probe's clock on the copies is not, 8.8 to 12.6 ms for
the same turn on the flight of 09:52). A pair comes every eighth frame,
so a ship's rate is up to eight frames old when it is applied: a ship
at two metres a frame that turns its heading three degrees in that
time puts its predicted point a tenth of a metre off, a third of a
pixel at five hundred metres; a ship that flips over in a second is
wrong by whole pixels for those eight frames, and that is the limit of
the stage as built. The box, unlike the rate, is carried to the frame
it is applied on by the ship's translation times the frames since the
copy, and widened by a fifth of the way; the ships' first flight
claimed a hundredth of a percent of pixels before it was. Reading a
pair every frame would close the rate's lag at the cost of a diff a
frame, and is the next step if the flights ask for it.

There is no grid for a ship. Its box -- its parts' recorded positions
padded by thirty metres, a part's mesh around its origin -- is the
first test, and the claim within it is the space within thirty metres
of one of its parts (up to thirty-two of them carried in the constants)
and ahead of its tail, the plane five metres behind its rearmost part
along the way it flies: a world-path pixel whose depth places it there
takes the ship's path, and the boxes are tested before the station's
grid, since a ship crossing the slot sits inside the station's cells
too and took the station's turn there until now. The tail is the plume:
the particles of a ship's drives are left in space as the ship goes,
each quad writing its depth, and inside the box at the ship's depth
they took the ship's motion -- "weird rectangular artifacts in the
smoke trail" on the ships' second flight -- so behind the tail nothing
is the ship's and the plume keeps the camera's path, blurry and whole.
The pass composes each ship's
motion exactly as the station's (`temporalBodyPath`, the carried form
on another camera's frames, the origin's shift) from the same eased
frame length, and the shader carries eight of them as three rows, a
translation and a box each. The player's own ship is never one: the
split has it (`temporal_aa_ship_metres`), and any part within a hundred
metres of the seat is left out of every cluster; nor is anything
within the body's near floor of fifty metres claimed. The registration
line says what share of pixels the ships took and names the nearest;
the probe's twenty-second report counts what it took and what it left
out and why; and the debug view (`temporal_aa_debug = objects`) paints
a ship's claimed pixels cyan.

What it does not reach: a ship of one or two recorded parts, a ship
holding still against the world (which needs nothing), a ship's turret
or landing gear moving against its hull (the hull's motion, as the
station's panels are the station's), the eight-frame lag above, and
whatever sits inside a ship's padded box at the ship's depth -- a
station wall the ship skims -- which takes the ship's motion for as
long as the ship is over it.

## The second body

A station is one rigid body until it is not. The thirty-third flight's
pool pairs (2026-09-09 15:42 and 15:43, Macleod Market at ten kilometres)
hold two rigid clusters at the station, turning about the same axis at
the same rate in opposite senses: the ring and its spine, 615 to 641
parts at radii from 270 m to 2.4 km, and the docking hub, 211 to 239
parts within 475 m, most of them near 400. The signed turn of each part's
position about the axis is +0.044 deg a frame for the one set and -0.044
for the other, so it is the parts that counter-rotate, not merely their
orientations. Whether an Orbis's hub truly turns against its ring or
only its distant instanced pieces do, the pass must follow what is drawn,
and what is drawn is the pool.

The body's grid seeds a cell for every live record of the body's TYPES
(the tips that shuffle slots every frame are only reachable by type),
and the hub's parts are of the ring's types, so the hub's cells were the
ring's and its pixels took the ring's turn -- wrong by twice the turn,
the tangential smear of the hub's face in the dump of 15:43:30 while the
ring stayed crisp. Under the camera's path alone the hub would be wrong
by one turn: at ten kilometres that is a tenth of a pixel a frame.

So the probe takes a SECOND BODY (`ObjectMotion::body2`): after the body,
the largest other cluster of forty or more parts whose centroid sits
within six kilometres of the body's, camera-relative -- the same
structure, moving otherwise -- fitted with the body's trimmed fit, its
rates their own sixteen-pair median, continuity against its own last
centroid, held over four pairs that do not find it (its cluster comes and
goes with the table's overflow). Its parts' cells are stamped 128 in the
grid after the body's dilation writes 255 -- a cube of three times their
median spacing around each part, never under two cells nor over the
body's reach -- and its cluster is kept from the ships. The pass carries
a second set of rows composed exactly as the body's, with the same origin
shift; `insideBody` answers 0, 1 or 2, and a 2 takes the second body's
path when one is in hand and the camera's when not. The registration
line says its share and turn, the 20 s probe report how often it was
found and held, and the objects view paints it grey.

Limits, stated: one second body, chosen by size; a station with three
counter-turning sections keeps the third on the body's path. The stamped
cubes are cubes, so a spoke's root within the hub's reach takes the hub's
turn. Its reach is set from its parts' spacing, so a body of few large
parts (a hub of four skins) marks less than it covers. And the pairs on
disk are a sample every thirty seconds: whether the hub's counter-turn
begins at a fixed range, or with a LOD, is not yet known.

The next flight (16:22) read at first as the hub turning with the ring
-- one cluster, 623 parts -- and the dumps said otherwise: the ring crisp
and the hub's face smeared in the same DLSS frame. The pool pairs held
the answer: 271 to 298 records at the station, the hub's among them,
live in the second frame of every pair and never in the first. The game
writes them on alternate frames, a consecutive pair holds them once, and
a record seen once cannot be diffed. So the pair is two frames apart
from a start that alternates between even and odd frames, and every
other pair holds the hub twice; the hub counter-turns at range in every
session, and only its visibility to the diff varied. The detector also
learned that a fragment of the body is not a second body: the ring's
own cluster splits at the cluster table's overflow, and the other half
turns the body's own turn. A second body must turn a quarter of the
body's turn apart at least.

## The stepped parts

The thirty-fifth flight retracted the counter-turn. What the pool showed
across the flights at ten kilometres was one thing seen three ways: the
docking hub's skin (about 290 records within 500 m of the axis) and the
solar panel arrays (about 19 at 1.2 to 2 km) are updated by the game at
a lower rate than the frame. A consecutive pair caught them stepping back
to an older buffered pose (the "counter-turn", 15:40), a consecutive pair
of the next flight found them present on alternate frames only (16:22),
and a two-frame pair found them holding one pose across both frames
while the ring turned (16:55). What the game draws for them steps, or
alternates between two poses a frame apart, and the pilot's eye sees
them spin with the rest -- but a smooth vector cannot match a step, and
DLSS smeared them in every flight at range while the spokes beside them,
updated every frame, stayed crisp.

So the probe copies the pool every frame now (six staging slots, two of
which are the pair's), and `trackFrame` compares each frame with the one
before: every live record at the station turns about the body's axis by
some multiple of the body's own turn that frame -- one for a part the
game updates every frame, nought for one holding an old pose, two or more
for one catching up, minus one for one stepping back. A part that has
turned the body's turn on every frame it was seen is the body's and its
cells are left alone. For the rest, an eight-entry history per slot
names the kind: a period of two (the prediction is the entry of the same
parity as the frame the pass draws next), one value held (kept), or
irregular (the mean). Their cells -- a one-cell cube around each -- are
stamped 64 plus the multiple offset by three, over the worker's grid,
and uploaded each frame as the box that holds them. The pass carries a
table of twelve composite deltas, one per multiple from -3 to 8, each
composed exactly as the body's own from the body's turn: the axis point
c solves (I - R) c = t_perp, and the m-th translation is (I - R^m) c +
m t_par, so m = 1 gives the body's own path and m = 0 the camera's. A
pixel in a stamped cell takes its cell's entry. The objects view paints
them orange; the registration line says their share; the probe's 20 s
line says how many parts step, in what pattern, and how often the
prediction held.

Limits, stated: the prediction is a guess two frames on from what the
readback could see, right for a period of two and for a held value, a
mean for anything else; the cube is a cube, so a spoke's root inside a
panel's cube takes the panel's multiple; and a part whose turn is not
about the body's axis is left to the body's path, which is where the
ships' own stage takes over.

The thirty-seventh flight retracted the stepped parts as built. The
run of four consecutive pool frames shows the hub's records turning 0.033
deg one way and back, frame about, with no net motion, while the ring's
records turn 0.05 deg a frame steadily; and the objects view shows the
stamped cells missing the hub's visible pixels. The drawn hub turns, so
its turn is not in its records: the pool's vertex shaders skin each vertex
by a 48-byte bone palette at t0 before the record's quaternion, and a
bone can carry a spin the pool never shows. The stamps are off. What the
hub actually does from one frame to the next is measured from the
picture now: the dump key takes four consecutive frames of the left eye,
and `tools/eye_run_spin.py` reads a ring's turn per frame off them.
