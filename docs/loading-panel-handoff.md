# Handoff: the loading dialog's oversized panel

For whoever takes the fourth attempt. Everything here was measured on the
field rig (Frontier launcher install, game build 330683, eye textures
5424x5356) on 2026-08-28. Nothing in it is inferred unless it says so.

## The defect, stated exactly

While Elite's loader shows its progress dialog, the panel behind that dialog
-- a dark fill inside four border strips -- is drawn at the **full size of the
interface surface**. On a monitor that is an ordinary modal scrim. In a
headset it covers most of the field of view, and with `fix.menu_backdrop =
splash` showing good artwork underneath, what it covers is worth seeing.

The field's stated ideal, in their words: *"draw solid black behind each modal
and have no extraneous tint"*, and later *"find the dynamic size of these
modals and scale the background scrim to their exact size and shape, for this
intro sequence only"*.

Two dialogs appear in sequence: "PREPARING SHADERS", then a taller
orange-bordered one. Similar widths; the second extends further above and
below.

## What is established

**The draw.** `X` (DrawIndexedInstanced), **30 indices**, one instance,
topology 4 (trianglelist), into the interface surface (4259x2395 on this rig
-- that size moves with render scale, do not pin it). Vertex stride **24**,
offset 0, from a shared **4 MB dynamic** vertex buffer. No per-draw constant
buffer: the census column reads `c=-`.

**Its geometry**, read by `advanced.quad_probe`:

```
quad 0: x -32750..32749   y -32738..32737   (65499 x 65475)   the fill
quad 1: x -32765..32764   y -32765..-32738  (65529 x    27)   top edge
quad 2: x  32749..32764   y -32765..32764   (   15 x 65529)   right edge
quad 3: x -32765..-32750  y -32765..32764   (   15 x 65529)   left edge
quad 4: x -32765..32764   y  32737..32764   (65529 x    27)   bottom edge
```

A bordered panel filling its coordinate space. **Position is a `float2` at
offset 0** -- the third float came back as 1.5e23, so it is not a z.

**The mesh re-tessellates.** The composite that lifts this surface into the
eye was 5760 indices under one dialog and 360 under the other. Any signature
pinning an index count matches one state and silently misses the other.

**Other draws into the same surface**, from one loader frame:

| indices | textures | what |
|---|---|---|
| 30, 648, 2508 | none | solid fills |
| 372, 96, 6 | 2048x2048 `A8_UNORM` | text (font atlas) |

## What has been tried, and why each failed

**1. Suppression.** `census_skip_offscreen = 4259x2395` dropped 13,754 draws
and removed the whole interface, wash included. Narrowed to `X:2508` -- no
change. To `X:648, X:30` -- scrim *and* the dialog's backing both went.
`X:30` alone -- same. Conclusion: they are quads in one batched call.

**2. Sub-draw omission.** `advanced.census_skip_quad` re-issues the draw
without a quad range. Omitting quad 0 removed both the wash and the backing,
which is how the batching was proven. Cannot separate them.

**3. Scissor clip.** `census_clip_width/height` clips the range to a centred
box. Produced "a small tinted rectangle": a scissor **crops**, so it cannot
scale a panel and cannot make a translucent fill opaque. Wrong tool for a
sizing problem, and it also revealed that clipping quad 0 alone leaves quads
1-4 -- the border strips -- still spanning the view.

**4. Uniform scale** (`fix.loading_panel_scale`, since removed). Rejected by
the field as the wrong interface, correctly: a magic ratio is wrong on another
rig, wrong for the second dialog, and wrong the day Frontier changes either.

**5. Measured fit** (`src/d3d11/loader_panel.cpp`, present, off by default).
Captures every draw into the surface in one frame, calls the full-surface
30-index draw the panel and the union of the rest the dialog, and remaps the
panel's vertices onto it. **Flown and wrong.** Against a panel of 65529x65529
it measured the dialog at 913x568, then 11221x568, then 0x0, and the panel
collapsed to a sliver.

## Why attempt 5 failed, which is the useful part

**The premise is circular.** The recurring `568` is the height of one line of
text. What the module measures as "the dialog" is a text run. There is no
separate dialog box to fit to, because **the panel IS the dialog's box** --
everything else in that buffer is its contents.

**Collection is unstable.** Draw counts per measurement were 1, 3, 6, 11, 9,
12. Collection runs for a single frame and catches a different subset each
time, so even content-derived bounds would not be stable as written.

## What a fourth attempt should establish first

**Do all these draws share one coordinate space?** `loader_panel.cpp` assumes
so and nothing proves it. If each element carries its own transform -- in the
16 bytes after the position, or in a constant not yet found -- then comparing
raw vertex positions across draws is meaningless and every bounds calculation
here is void. This is the first thing to settle, and it is cheap: dump two
draws' positions and compare them against where those elements actually
appear.

**Then, if the space is shared:** size the panel to its CONTENT plus padding,
not to a "dialog". Collect reliably -- accumulate across frames until the set
of draw shapes repeats, or hook the shared buffer's `Map` and read every
element written in one pass rather than sampling draws.

## Instruments available

All in `[advanced]`, all off by default, all live.

| key | does |
|---|---|
| `quad_probe = WxH:KIND:COUNT` | copies a draw's index range and the vertex buffer, logs each quad's rectangle |
| `census_skip_offscreen = WxH[:KIND:COUNT]` | drops draws into a surface, optionally one shape only |
| `census_skip_quad = WxH:KIND:COUNT:LO[-HI]` | re-issues a draw without a quad range, in the game's own order |
| `census_clip_width/height` | clips that range to a centred box instead of omitting it |
| `census_cb_watch = VSHASH` | dumps a draw's b0 constants as DCW lines (note: b0 only -- the pixel shader here reads **b2**, never captured) |
| `glare_shader_dump = 1` | writes every shader to `edvr_logs\shaders` by hash |

`tools/diff_draw_census.py` diffs two censuses from one session. It is what
finally identified the wash after five failed hypotheses; reach for it before
reading signatures out of a single capture.

## What is already fixed and must not regress

`fix.loading_dim = off` (committed, `77b7119`) collapses the **frosted blur**
layer -- an eight-tap directional smear the field called "squiggly lines". It
works by substituting a uniform for PS slot 0 at level **0**, because the
16x16 texture there *multiplies* the blur layer. That is settled and separate
from the sizing problem. `docs/loading-scrim.md` has the disassembly.

## A note on scale

This is a dark rectangle behind a dialog that shows for a few seconds. It has
cost about a dozen field flights and three architectures. It is worth doing
properly or not at all; it is not worth a fifth guess.
