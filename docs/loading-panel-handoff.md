# The loading dialog's oversized backdrop

Fourth architecture, built 2026-08-28 and NOT yet flown. Everything measured
here came from the field rig (Frontier launcher install, game build 330683,
eye textures 5424x5356) on 2026-08-28; everything else says what it is.

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

## The model, corrected

The frame holds **two kinds of dark rectangle**, and the third attempt died
of conflating them:

* the **backdrop**: a five-quad bordered panel -- fill plus four edge strips,
  30 indices -- spanning the whole coordinate space. The thing to shrink.
* the **box**: the modal the player sees, drawn after the backdrop. Its
  black backing and orange border are game draws that already have exactly
  the bounds the field wants the backdrop collapsed to.

For a while the record said the opposite -- "the panel IS the dialog's box,
there is no separate box to fit to". That conclusion came from instruments
whose semantics guaranteed it:

* `census_skip_offscreen = ...:X:30` and `census_skip_quad` match **every**
  draw sharing a signature. Two X:30 draws -- backdrop and box -- vanish
  together under either. Their co-disappearance proved sharing a signature,
  not being one call.
* the original `quad_probe` captured **only the first match per session**.
  The backdrop draws first (painter's order), so the probe's five
  full-surface quads describe the backdrop, and said nothing about any later
  X:30.
* the arithmetic was against one-call all along: 30 indices is exactly five
  quads, and the probe showed all five spanning the surface. One 30-index
  call cannot also contain a modal-sized box; there is no room in it.

Both instruments are now occurrence-aware or replaced (below), so one flight
settles what the skip probes structurally could not.

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
could never show. Three rather than two likely means backdrop + box fill +
box border as separate panels; the occurrence probe names them by extent
and colour. The fix below handles any split by construction: full-surface
occurrences collapse, modal-sized ones join the target.

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
draw's indices plus the shared vertex buffer, GPU-copied, read back after a
settle. A collection whose frame moved mid-capture is discarded. This is the
answer to the 1-3-6-11-9-12 instability: transitional frames can no longer
be measured at all.

**Classify, then seed and grow.** In the capture, any 30-index draw with a
quad spanning >=80% of both the widest and tallest extents is a backdrop.
The **target** starts from a seed -- the box's own bordered panel when one
draws, else the largest solid in any batch -- and grows by absorbing only
quads that touch its neighbourhood (2% of the surface), a few passes, so the
box's border strips and the bar inside it join while a badge in a far corner
never does. Growth replaced a plain union after the first fit flight: the
loading screen keeps 5-8 solids about, some in the corners, and the union of
everything spanned the surface on every single measurement -- twenty
refusals in twenty seconds, all reading "the union beside the backdrop spans
the surface itself". Text never enters the pool at all (textured draws are
excluded at the draw hook), which keeps the old text-run circularity
impossible by construction. A target that is missing, sliver-sized (<2% of
the backdrop), or surface-sized records a no-verdict: **stock, with a log
line saying why and one line per solid it saw**, once per dialog state.

**Substitute by position.** The backdrop's *position in the frame sequence*
is what gets substituted -- its own vertices, positions remapped linearly
onto the target bounds, colour and the rest of the 24-byte vertex untouched,
exact bounds, no margin. Substitution at a position is valid only while the
current frame matches the measured sequence at every position up to it; the
moment composition diverges (animation ticks, dialog change), later draws
run stock and the next stable window re-measures. The box's own draw is
never substituted -- the third attempt substituted **every** X:30 once it
had geometry, which would have erased the very box the field pointed at. A
dialog switch shows the previous target for the handful of frames a fresh
measurement takes (two stable + one capture + four settle).

**Log lines to look for** (`edvr_logs`):

* `loading panel: FIT -- measurement N from S solids of D interface draws.
  The backdrop at draw P spans WxH; the box on top of it seeded from ... and
  grew to WxH at x .. y ..` -- the engage line; "seeded from the dialog's
  own 30-index panel" vs "...the largest solid in a 648-index batch" reports
  which model the frame matched. After the first four, it logs again only
  when the box moved -- the percent text re-measures constantly and the box
  holds still through it.
* `loading panel: measured N draw(s) and drew no conclusion -- REASON.
  Stock for this dialog state; the next change re-measures.` -- every
  no-verdict, now preceded by one `solid k: ...` line per captured draw with
  its extent, so a refusal names what it refused.
* `loading panel: the loader's draws have not held still for two
  consecutive frames in 600 frames` -- continuous animation; the fix is
  standing down correctly.

## The instruments (all `[advanced]`, off by default, live)

| key | does |
|---|---|
| `quad_probe = WxH:KIND:COUNT` | **occurrence-aware since 2026-08-28**: the first frame containing a match has EVERY matching draw copied; each occurrence logs baseVertex, startIndex, and per-quad rectangles plus the vertex bytes past the position as hex. Re-set the value for another capture. |
| `census_skip_offscreen = WxH[:KIND:COUNT]` | drops draws into a surface -- **every** draw matching the spec, all occurrences |
| `census_skip_quad = WxH:KIND:COUNT:LO[-HI]` | re-issues matching draws without a quad range -- again every occurrence |
| `census_clip_width/height` | clips that range to a centred box instead |
| `census_cb_watch = VSHASH` | dumps b0 constants (VS and PS b0 only -- the pixel shader here reads **b2**, never captured) |
| `glare_shader_dump = 1` | writes every shader to `edvr_logs\shaders` by hash |

`tools/diff_draw_census.py` diffs two censuses from one session; reach for
it before reading signatures out of a single capture.

## The flight plan

1. **Probe flight** (names the occurrences): `advanced.quad_probe =
   4259x2395:X:30:120` -- substitute the rig's own surface size from a
   census if render scale changed. The census has already counted **three
   occurrences per frame** in steady state, and the first probe flight
   (spec without the skip) proved why the skip matters: it fired on the
   first matching frame, mid fade-in, and caught the backdrop alone --
   alpha 0x66 black, all five quads full-surface. With 120 matching frames
   skipped the capture lands ~2s into the dialog: expect all three
   occurrences, the box's quads modal-sized with alpha `FF` at offset 8,
   and the orange border naming its colour. Re-arm (spec off, then on) for
   the second dialog.
2. **Fit flight**: `fix.loading_panel = fit`, watch both intro dialogs.
   Expect the backdrop snapped to each modal, the black backing and orange
   border untouched, a brief full-size backdrop during fade-in (measurement
   gating), and the engage log line above. Any wrongness arrives with its
   own log line naming what was measured -- report the `loading panel:`
   lines with the symptom. The first fit flight (11:11, 2026-08-28) ended
   here with every measurement refusing on the union rule; the seeded
   growth above is that flight's lesson.
3. On success, flip the shipped default to `fit` in a release commit.

## What must not regress

`fix.loading_dim = off` is settled and separate (commit `77b7119`,
`docs/loading-scrim.md`). The scrim_fix mechanism -- wash discriminated by
its 16x16 BC1 multiplier texture -- shares nothing with the panel path.

## A note on scale

This is a dark rectangle behind a dialog that shows for a few seconds. It
has cost about a dozen field flights and three architectures, and the third
architecture's central conclusion was manufactured by its own instruments'
matching rules. The fourth is built on the field's observation that the box
exists, fails toward stock with its reasons in the log, and asks one probe
flight to confirm the model before anyone trusts it. Worth doing properly
or not at all; this is the properly.
