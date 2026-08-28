# The loading dialog's oversized backdrop

Fifth architecture (the viewport model), built 2026-08-28 after two flights
of the fourth, NOT yet flown. Everything measured here came from the field
rig (Frontier launcher install, game build 330683, eye textures 5424x5356)
on 2026-08-28; everything else says what it is.

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

## The model: the viewport is the widget rect

Elite draws every solid panel -- scrim, box, letterbox -- as the SAME
30-index bordered widget (fill plus four strips) in one normalized space of
about +/-32765 across, and **sizes it with the viewport**. Flight 2 (11:27,
2026-08-28) proved this by exhaustion: its steady-state probe caught two
X:30 occurrences whose vertices BOTH span the full space -- one solid black
(alpha `FF`, strips 46/109 thick), one white with uv floats, a letterbox --
while the fix's refusal dumps show a third full-space X:30 every frame and
**no modal-sized solid anywhere in any vertex buffer**. The screen shows a
modal-sized box; the vertices never do; the only per-draw state left that
can place it is the rasterizer's -- viewport (which SCALES the widget into
a rect) or scissor (which crops to one). Two corollaries:

* **Vertex bounds must never be compared across draws.** Each draw's
  coordinates are normalized to its own viewport. The doc's oldest open
  question -- "do these draws share one coordinate space?" -- is answered:
  no, and every bounds-union across draws (attempts 3 and 4) was arithmetic
  on incommensurable numbers.
* **The roles are told apart by colour + viewport, not geometry.** The
  scrim's fill carries RGBA8 `00 00 00 66` at vertex offset 8 -- black at
  40% -- with a full-surface viewport. The box is black at alpha `FF` with
  the modal's own rect as its viewport. The letterbox is white; ignored.

Earlier wrong turns, kept because each was manufactured by an instrument's
semantics: the skip probes match every draw sharing a signature, so scrim
and box always vanished together ("one batched call" -- refuted by the
census counting three X:30s per frame at positions #1/#2/#4); the original
one-shot probe only ever sampled the first occurrence, mid fade-in ("the
panel IS the box"); and the vertex-space unions of attempts 3 and 4 caught
text runs and corner solids respectively, because they were unions over
incompatible spaces.

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
draw's indices (the shared vertex buffer once) for the colours, plus each
draw's **viewport, scissor and scissor-enable** for the rects. A collection
whose frame moved mid-capture is discarded. This is the answer to the
1-3-6-11-9-12 instability: transitional frames can no longer be measured.

**Classify by colour and viewport.** Among the captured 30-index panels:
the **scrim** is dark (all channels < 0x40), translucent (alpha < 0xF0),
with a viewport >= 90% of the surface in both axes; the **box** is dark,
opaque, with a viewport <= 80% in both axes -- or, if its viewport is full,
an enabled scissor that is boxed, in which case the scissor rect is the
modal. The white letterbox fails the darkness test and is never touched.
No scrim, or no box, records a no-verdict: **stock, with a log line saying
why and one line per solid** -- extent in its own units, RGBA8, viewport,
scissor -- so a wrong threshold names itself in one flight.

**Substitute through the box's viewport.** At the scrim's position (trusted
only while the frame matches the measured sequence draw by draw), the
game's own draw call is swallowed and re-issued with one change: the
viewport is the box's, read fresh **at the box's own draw every frame**, so
the modal never has to be assumed static. No geometry is built and no
vertex is touched -- the widget system itself sizes the scrim into the
modal's rect, borders scaling exactly as the box's own do. Inside the box
the scrim composites exactly as stock (invisible under the opaque box, and
unchanged under a translucent one); outside it, it no longer exists. The
box rect goes stale after 2 frames without the box drawing -- stock. A
dialog switch runs stock for the handful of frames a fresh measurement
takes (two stable + one capture + four settle).

**Log lines to look for** (`edvr_logs`):

* `loading panel: FIT -- measurement N from S solids of D interface draws.
  The scrim at draw P (rgba 66000000) is redrawn through the box's
  viewport; the box at draw Q (rgba FF000000) sits at X,Y WxH px of WxH.`
  -- the engage line. After the first four, it logs again only when the box
  moved; the percent text re-measures constantly and the box holds still.
* `loading panel: measured N draw(s) and drew no conclusion -- REASON.` --
  every no-verdict, preceded by one `solid k: ...` line per captured draw
  with extent, colour, viewport and scissor.
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

One flight answers everything, because the probe and the fix now read the
same state:

1. `advanced.quad_probe = 4259x2395:X:30:120` (surface size moves with
   render scale) + `fix.loading_panel = fit`. The probe logs every X:30
   occurrence ~2s into the first dialog **with its viewport and scissor**:
   the model predicts the opaque-black occurrence carries the modal's rect
   in one of them and the scrim's carries the full surface. If instead every
   occurrence's viewport AND scissor are full-surface, the model is wrong,
   the fix will have refused with dumps saying so, and the sizing lives in
   the one place left unread -- the pixel shader's b2 constants.
2. Watch both intro dialogs. Expect the scrim snapped to each modal, box
   and border untouched, brief full-view scrim during fade-in (measurement
   gating). The engage and refusal lines above carry everything worth
   pasting back.
3. On success, flip the shipped default to `fit` in a release commit.

Flights already flown: 11:11 (fourth architecture, union rule -- every
measurement refused, union spanned the surface); 11:27 (seeded growth --
same refusal, and its dumps plus the steady-state probe delivered the
viewport model above).

## What must not regress

`fix.loading_dim = off` is settled and separate (commit `77b7119`,
`docs/loading-scrim.md`). The scrim_fix mechanism -- wash discriminated by
its 16x16 BC1 multiplier texture -- shares nothing with the panel path.

## A note on scale

This is a dark rectangle behind a dialog that shows for a few seconds. It
has cost a dozen field flights and five architectures, and each dead one
died of a measurement its own instruments could not take: signature-blind
skips, a first-frame probe, unions over per-viewport spaces. The fifth is
the first whose mechanism -- the viewport is the widget rect -- explains
every byte measured so far, fails toward stock with its reasons and its
evidence in the log, and needs exactly one flight to confirm or refute.
Worth doing properly or not at all; this is the properly.
