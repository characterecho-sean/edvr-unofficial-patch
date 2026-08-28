# The loading dialog's oversized backdrop

Sixth architecture (the element-index model), built 2026-08-28 after four
flights, NOT yet flown -- and the first one READ FROM THE SHADER rather
than inferred from disappearances. Everything measured here came from the
field rig (Frontier launcher install, game build 330683, eye textures
5424x5356) on 2026-08-28; everything else says what it is.

## The defect, stated exactly

While Elite's loader shows its progress dialogs, a dark bordered panel is
drawn at the **full size of the interface surface** (4259x2395 on this rig --
that size moves with render scale, do not pin it). On a monitor that is an
ordinary modal scrim. In a headset it covers most of the field of view, and
with `fix.menu_backdrop = splash` showing good artwork underneath, what it
covers is worth seeing.

The field's stated ideal, in their words: *"find the dynamic size of these
modals and scale the background scrim to their exact size and shape, for this
intro sequence only"*.

Two dialogs appear in sequence, and the field describes both as drawn ON TOP
of the oversized backdrop:

* **"PREPARING SHADERS"**: no border to speak of, but the game renders a
  solid black background behind its text, over the scrim.
* A taller dialog **bounded in the orange UI colour**, about the same width,
  extending further above and below.

## The model: the element index in the vertex, the matrix in a table

Flight 3 (11:42, 2026-08-28) refuted the viewport model completely: every
solid's viewport is the full surface (`0,0 4259x2395`) and every scissor is
off, on every draw, both dialogs. That left exactly one unread place, and
the answer was already on disk -- `edvr_logs\shaders` holds
`vs_666EF0C4C616F67E.dxbc` from an earlier `glare_shader_dump` session,
and its disassembly (now `docs/shaders/ui-widget-vs.asm`, with the pixel
shader beside it) names the whole mechanism:

* every panel's vertices span one normalized space (about +/-32765);
* the vertex shader reads a **per-element 4x4 matrix from a structured
  buffer at VS t0, stride 160** (`ld_structured` at offsets 0/16/32/48;
  offsets 64/80 feed the pixel shader the element's styling params), and
  transforms the position with it;
* the element is selected by a **byte carried in the vertex itself**:
  `round(255 * COLOUR1.x)` or `round(255 * COLOUR2.x)` -- bytes 12 and 16
  of the 24-byte vertex -- chosen by flag bits 0x4000/0x8000 in VS
  `cb2[2].x`.

So the 24-byte vertex reads: `float2` position, RGBA8 colour (offset 8),
two more RGBA8 attributes whose `.x` bytes are element indices (offsets 12
and 16), and an unused trailing dword. One widget, one buffer, many
panels, each placed by its element's matrix. This is why five instruments
measured "nothing differs": vertices, viewports and scissors ARE identical
-- the census tracks PS textures and b0 only, and the discriminating state
was a VS structured buffer indexed per vertex.

**Flight 4 (11:56) placed every piece.** The matrix-verified dumps show
all three standalone X:30 panels riding **element 0, the full-view
matrix**: the scrim maps to `0,0 4258x2394`, the opaque black panel to
`2,3 4254x2388` (a full-view layer with a hairline inset -- NOT the
modal's backing; whatever its job, it does not read as black on screen),
the letterbox's centre to `115,437 4029x1522`. **The modal's backing is
therefore a quad INSIDE one of the batches** (the 2508/648-index draws),
carrying its own element byte -- which is precisely what a per-vertex
index is FOR: one draw, hundreds of quads, each placed by its own matrix.
A batch is not one placement, and any draw-level summary of one is
meaningless.

The roles, confirmed by colour across flights: the scrim's fill is RGBA8
`00 00 00 66` at offset 8 -- black at 40%, the ugly wash itself; the
modal backing is black at alpha `FF` (a batch quad); the letterbox is
white (its "uv floats" were its element params all along).

Earlier wrong turns, kept because each was manufactured by an instrument's
semantics: the skip probes match every draw sharing a signature, so scrim
and box always vanished together ("one batched call" -- refuted by the
census counting three X:30s per frame); the original one-shot probe only
ever sampled the first occurrence, mid fade-in ("the panel IS the box");
the vertex-space unions of attempts 3 and 4 were arithmetic over spaces
that were never comparable; and the viewport model of attempt 5 guessed
the right kind of mechanism in the wrong pipeline stage.

## What is established (field-measured)

**The backdrop draw.** `X` (DrawIndexedInstanced), **30 indices**, one
instance, topology 4, into the interface surface. Vertex stride **24**,
offset 0, from a shared **4 MB dynamic** vertex buffer. No per-draw constant
buffer (census `c=-`). Geometry, read by the first-match probe:

```
quad 0: x -32750..32749   y -32738..32737   (65499 x 65475)   the fill
quad 1: x -32765..32764   y -32765..-32738  (65529 x    27)   top edge
quad 2: x  32749..32764   y -32765..32764   (   15 x 65529)   right edge
quad 3: x -32765..-32750  y -32765..32764   (   15 x 65529)   left edge
quad 4: x -32765..32764   y  32737..32764   (65529 x    27)   bottom edge
```

Position is a `float2` at offset 0, and the first fit flight (11:11,
2026-08-28) settled what follows it: an **RGBA8 colour at offset 8**. Every
backdrop quad carries `00 00 00 66` there -- **black at alpha 0x66, 40%** --
which is the ugly translucent scrim, measured. The twelve bytes after that
hold a second dword (`000000FF` on the backdrop) and what looks like uv or
stale pool data. A solid black box will read alpha `FF` at offset 8; the
orange border will name its colour outright.

**Other draws into the same surface**, from one loader frame:

| indices | textures | what |
|---|---|---|
| 30, 648, 2508 | none | solid fills |
| 372, 96, 6 | 2048x2048 `A8_UNORM` | text (font atlas) |

Census rows aggregate by signature: one row can hide several draws per
frame. This is the trap the "one batched call" conclusion fell into.

**The box is X:30-tied.** Narrowing `census_skip_offscreen` to `X:30` alone
removed the scrim *and* the dialog's backing. If the backing lived only in
the 648- or 2508-index batches it would have survived that.

**Multi-occurrence CONFIRMED by census** (session 11:02, 2026-08-28, census
1 at frame 4659, a loading dialog -- 8 eye draws): the interface surface
(4259x2395, the 4 MB buffer) receives **THREE X:30 draws per frame**, at
frame positions #1, #2 and #4, in both censused frames. All three share
vertex shader `vh=666EF0C4C616F67E`, pixel shader `ph=C0C4E6413DF14E9A`,
stride 24, topology 4, no constants, no textures -- signature-identical,
separable only by position, which is exactly what every skip-probe test
could never show.

**Flight 2 named the three** (11:27, steady-state probe + refusal dumps):
the alpha-0x66 scrim, an opaque black panel (alpha `FF`, strips 46/109),
and a white letterboxed frame carrying uv floats -- and every one of them
spans the full +/-32765 space in its vertices. The modal-sized rect exists
on screen and nowhere in any vertex buffer, which is what forced the
viewport model above.

**The loader animates.** The third attempt's one-frame collections caught 1,
3, 6, 11, 9, 12 draws on six consecutive tries: fade-in and progress
re-tessellation change the frame's composition continuously, and any
measurement not gated on stability describes a transitional frame. Its
measured "dialog" of 913x568 was one line of text caught mid-fade -- the 568
is text-line height.

**The frosted wash is separate and solved.** `fix.loading_dim = off`
(commit `77b7119`) collapses the eight-tap blur layer by substituting a
uniform for PS slot 0 at level 0 -- the 16x16 texture there *multiplies* the
blur. Settled, untouched by all of this. `docs/loading-scrim.md` has the
disassembly.

## What is built (this attempt)

`fix.loading_panel = fit`, rewritten in `src/d3d11/loader_panel.cpp`.
Default remains `stock` until field-verified.

**Measure on stability.** Each loader-shaped frame's draws into the
interface surface are recorded as a sequence of shapes (kind, index count,
target size). Only when two consecutive frames record the *identical*
sequence does the next frame get captured: every textureless quad-batch
draw's indices (the shared vertex buffer once), plus the **widget table
(VS t0, first 64 KB, with the view's FirstElement)** and the **flag
constants (VS b2, 48 bytes)** -- all GPU-timeline copies. A collection
whose frame moved mid-capture is discarded. This is the answer to the
1-3-6-11-9-12 instability: transitional frames can no longer be measured.

**Classify by what a quad RENDERS, verify by matrix -- per QUAD.** The
**scrim** is a standalone 30-index panel, dark (all channels < 0x40),
translucent (alpha < 0xF0), whose element maps to >= 90% of clip space in
both axes. The **box** is hunted across **every quad of every captured
solid, batches included** -- and by **estimated rendered colour**, not
vertex colour: flight 5 proved the batches' vertices are colour-neutral
(the only dark opaque vertices anywhere belong to the full-view sheet),
because a quad's look is `vertexColour x paramA + paramB` from its
element's offsets 64/80. The classifier replays the two shaders'
arithmetic (colour source and fade byte by the cb2 flags, the mad, the
two alpha modulations) over the captured table, cb2 and vertices, and
takes the largest quad that lands between 2% and 80% of the view in both
axes AND estimates dark (every channel <= 0.25) and covering (alpha >=
0.90). Blend state is not modelled; this is a classifier, not a renderer.
No scrim, no such quad, no table, mixed tables, or an index past the
64 KB window: **stock, with a log line saying why**, one line per panel,
plus the three largest BOXED quads and the three largest DARK-COVERING
quads with their vertex colour, estimate and mapped px rect -- one of the
two lists names the side a wrong threshold is on.

**Substitute the element index.** At the scrim's position (trusted only
while the frame matches the measured sequence draw by draw), the draw is
swallowed and re-issued from a 30-vertex copy that differs in exactly two
bytes per vertex: the element-index bytes now name the BOX's element. The
game's own shader then places the scrim with the box's matrix -- read from
the game's live table this frame and every frame, so nothing about the
modal's position is ever stored on our side and nothing can go stale. The
scrim keeps its own colours; it inherits the box's styling params, which
only shows where it shows at all -- under the box. A dialog switch runs
stock for the handful of frames a fresh measurement takes (two stable +
one capture + four settle).

**Known risk, accepted until a flight rules:** if the game reallocates
table slots frame to frame within one stable dialog (the sun-glare hunt
met exactly that in the 3D HUD), the captured index would drift off the
box's element. A static dialog most likely keeps its slots; drift would be
immediately visible, and the engage line's element numbers name it.

**Log lines to look for** (`edvr_logs`):

* `loading panel: FIT -- measurement N from S solids of D interface draws.
  The scrim at draw P (rgba 66000000, element E) is re-issued as element
  B -- the box, a quad of the 2508-index draw at position Q (rgba
  FF000000), whose matrix lands at X,Y WxH px of WxH.` -- the engage line.
  After the first four, it logs again only when the box moved; the percent
  text re-measures constantly and the box holds still.
* `loading panel: measured N draw(s) and drew no conclusion -- REASON.` --
  every no-verdict, preceded by one `solid k: ...` line per captured draw
  with colour, element bytes, and the px rect its element maps to.
* `loading panel: the loader's draws have not held still for two
  consecutive frames in 600 frames` -- continuous animation; the fix is
  standing down correctly.

## The instruments (all `[advanced]`, off by default, live)

| key | does |
|---|---|
| `quad_probe = WxH:KIND:COUNT[:SKIPFRAMES]` | **occurrence-aware since 2026-08-28**: after SKIPFRAMES matching frames pass (fade-in insurance), the next frame containing a match has EVERY matching draw copied; each occurrence logs baseVertex, startIndex, **viewport, scissor**, and per-quad rectangles plus the vertex bytes past the position as hex. Re-set the value for another capture. |
| `census_skip_offscreen = WxH[:KIND:COUNT]` | drops draws into a surface -- **every** draw matching the spec, all occurrences |
| `census_skip_quad = WxH:KIND:COUNT:LO[-HI]` | re-issues matching draws without a quad range -- again every occurrence |
| `census_clip_width/height` | clips that range to a centred box instead |
| `census_cb_watch = VSHASH` | dumps b0 constants (VS and PS b0 only -- the pixel shader here reads **b2**, never captured) |
| `glare_shader_dump = 1` | writes every shader to `edvr_logs\shaders` by hash |

`tools/diff_draw_census.py` diffs two censuses from one session; reach for
it before reading signatures out of a single capture.

## The flight plan

1. Keys as already armed: `fix.loading_panel = fit` +
   `advanced.quad_probe = 4259x2395:X:30:120` (surface size moves with
   render scale; the probe's mid-frame skip bug is fixed, so it now shows
   ALL occurrences of its frame, the scrim included). Watch both intro
   dialogs. Expect the scrim snapped to each modal, box and border
   untouched, brief full-view scrim during fade-in (measurement gating),
   and the engage line naming both elements and the box's px rect.
2. If the scrim drifts or jumps mid-dialog, that is the slot-reallocation
   risk above -- the engage/refusal lines carry the element numbers that
   prove it, and the next mechanism is a per-frame index re-read rather
   than a captured one.
3. On success, flip the shipped default to `fit` in a release commit.

Flights already flown, all safe (never engaged): 11:11 (fourth
architecture, union rule -- refused, union spanned the surface); 11:27
(seeded growth -- refused the same way; its dumps plus the steady-state
probe killed vertex-space measurement); 11:42 (fifth, viewport model --
refused: every viewport full, every scissor off, which forced the shader
read that found the real mechanism); 11:56 (sixth, element model with a
panel-level box hunt -- refused correctly: all three standalone panels
ride element 0 at full view, which proved the backing is a batch quad and
sent the hunt per-quad); 12:04 (per-quad hunt by VERTEX colour -- refused,
and its top-3 dump showed the only dark opaque vertices in the frame are
the full-view sheet's: batch quads are colour-neutral and styled by their
element params, which moved the classifier to estimated rendered colour);
12:25 (estimated-colour hunt over untextured quads -- refused, and its
candidate lists finally showed the landscape: the ORANGE MODAL is a batch
element, 611x1235 px, estimating exactly Elite orange ~1.00,0.44,0.08,
which vouches for the estimator -- while the dark-covering list was EMPTY,
so the black backing is in none of the untextured draws. The one excluded
population was TEXTURED draws: an atlas-masked black quad is textured by
construction, so the capture now takes textured draws too, judges their
quads the same way, and the diagnostic lists grew to six entries with
texture and blend-state tags -- blend being the one un-modelled stage that
could make a light quad darken the screen).

## What must not regress

`fix.loading_dim = off` is settled and separate (commit `77b7119`,
`docs/loading-scrim.md`). The scrim_fix mechanism -- wash discriminated by
its 16x16 BC1 multiplier texture -- shares nothing with the panel path.

## A note on scale

This is a dark rectangle behind a dialog that shows for a few seconds. It
has cost a dozen field flights and six architectures, and every dead one
died of a measurement its instruments could not take: signature-blind
skips, a first-frame probe, unions over incomparable spaces, rasterizer
state that never varied. The sixth is the first read out of the shader
itself -- the element index in the vertex, the matrix in the table -- and
it explains every byte measured across every flight, including the two
architectures' worth of "nothing differs". It fails toward stock with its
evidence in the log, and its substitute is two bytes per vertex riding the
game's own live data. Worth doing properly or not at all; this, finally,
is the properly.
