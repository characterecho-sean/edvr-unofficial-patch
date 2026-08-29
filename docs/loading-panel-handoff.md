# The loading dialog's oversized backdrop

Sixth architecture (the element-index model), built 2026-08-28 after four
flights, NOT yet flown -- and the first one READ FROM THE SHADER rather
than inferred from disappearances. Everything measured here came from the
field rig (Frontier launcher install, game build 330683, eye textures
5424x5356) on 2026-08-28; everything else says what it is.

> **Key surface, 2026-08-28 (post-consolidation):** everything this doc
> calls `fix.loading_panel = fit` and `loading_splash_dim = on` now ships
> as ONE key, **`fix.loading_dim = screen`** (default on), which also
> absorbs the frosted-wash removal. `stock` is the game's own; `wash` and
> `panel` remain as developer values, one mechanism each. Old spellings
> parse silently and old ini lines migrate via moved-from. The mechanism
> descriptions below are unchanged and correct.

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

**Flight 4 (11:56) placed the panels.** The matrix-verified dumps show
all three standalone X:30 panels riding **element 0, the full-view
matrix**: the scrim maps to `0,0 4258x2394`, the opaque black panel to
`2,3 4254x2388` (a full-view layer with a hairline inset -- NOT the
modal's backing; whatever its job, it does not read as black on screen),
the letterbox's centre to `115,437 4029x1522`.

**Flights 5-7 then proved the backing is NOWHERE in the interface
surface.** Per-quad hunts by vertex colour, by estimated rendered colour
(replaying both shaders' arithmetic -- validated when it named the ship
hologram's region in exact Elite orange), and finally with textured draws
included, mapped every quad of every draw: wide translucent white text
bars, the hologram's tall panels, glyphs -- and nothing, at any
granularity, renders a dark boxed rect. All blend states are plain
src-alpha (5*6), so nothing light darkens the view either.

**The field's screenshot plus the eye-level census close the case.** The
PREPARING SHADERS box on screen is a sharp black rectangle (~27% x 20% of
the view, wider than tall) that matches no interface quad. The 11:02
census's EYE-level lines show each eye's frame ending with: a textured
single quad, then a single quad with a CONSTANT BUFFER and depth bound
(stride-8 vertices -- a positioned rect), then the 5760-index composite
that lifts the interface in (the wash shader `9107E72C...`). **The black
backing is an eye-level quad drawn beneath the interface composite** --
the same stage as the menu's dark layer that survived emptying the
interface buffer. Seven interface-surface architectures could never have
found it, by construction. (If a true collapse is ever wanted, the rect
is reachable: that eye draw's b0 -- census `c=@136`, dumpable with
`census_cb_watch = A888D51024D9798E` -- or its stride-8 vertex buffer.)

The scrim itself, confirmed across every flight: RGBA8 `00 00 00 66` at
vertex offset 8 -- black at 40% -- on element 0, the full-view matrix.

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

**Classify the scrim, verify by matrix.** The **scrim** is a standalone
30-index panel, dark (all channels < 0x40), translucent (alpha < 0xF0),
whose element maps its fill to >= 90% of clip space in both axes --
verified through the same table the shader reads, so a misclassification
cannot survive its own footprint. No box hunt exists any more: the box is
not in this surface.

**Withhold by ordinal, from the very first frame.** Flight 8 engaged and
the field reported it near-perfect, with the scrim visible briefly at
first -- that was the stability gate plus the fixed settle, machinery
inherited from the positional architectures that the withhold does not
need. Now: until the session's first verdict, the frame's FIRST panel is
withheld ON SPECULATION -- nine flights of measurement say that draw is
always the scrim, so the blip is zero rather than brief, and a refusing
verdict would simply end speculation and return the panel after a couple
of hidden frames, in a case no measurement has ever produced. Meanwhile
the first panel-bearing frame is captured immediately (classification is
frame-local; a mid-fade frame is a valid sample), the copies are polled
with DO_NOT_WAIT (verdict typically lands in 1-2 frames), and the
verified scrim is withheld BY ORDINAL -- the k-th panel of its surface in
the frame, k=0 in every measurement to date -- every frame, through
fade-in, percent ticks and the dialog switch, with no composition
matching at all. The chain's END is the intro's end, not a gap count:
flights 9 and 10 proved gap length is no signal (a 2-frame grace flashed
the scrim at every modal; 300 frames still died inside the several-second
white-text-to-first-modal gap -- silently, which is why every stand-down
now logs). The first rendered-scene frame (the kSceneEyeDraws boundary
the draw hook already gates on, passed into the tick) retires the module
for the session, engaged or not -- which is also what enforces the
documented intro-only scope against any in-game screen that happens to be
loader-shaped. Within the intro, gaps of any length ride through; a
return from a real gap (>= 30 frames) re-verifies immediately with the
withhold carrying across, scattered single-frame panel skips (the
interface's habit -- flight 10 counted 18 of them in seven seconds) ride
free, and the periodic ~2s re-verification stands the chain down the
moment the scrim stops classifying. The scrim first appears in a
white-text-over-scrim phase BEFORE the modals -- field-confirmed already
transparent there under the withhold -- so by the time a dialog pops the
swallow has been live for seconds. After a few refusals, arming falls
back to requiring two identical frames, so a panel-bearing screen that is
not the loader cannot re-trigger a 4 MB capture per animation frame.

**The splash dims instead** (`splash_dim.{h,cpp}`, field-requested after
the withhold flew). The scrim's DESIGN -- the splash stepping back while
a dialog talks -- was right; only its address was wrong. So on exactly
the frames the withhold swallows the scrim (loaderPanelDimWanted, zero
lag: the screen composites draw later in the same frame), the splash
screen's own composite -- the backdrop still's, or the intro movie's --
is re-issued once through a compiled dark pixel shader (0,0,0,0.4 -- the
scrim's measured alpha exactly) under src-alpha blend, after backdropEnd
so that pairing stays pristine. The tint lands on the screen, flat as
the screen is, and nowhere else. Gated to the eye-side composite by RTV
size (the backdrop verdict also wraps an offscreen half). Off switch:
`loading_splash_dim = off`, a commented key -- deliberately not in the
installer's settings window pending the intro knob consolidation.

**Log lines to look for** (`edvr_logs`):

* `loading panel: FIT -- measurement N from S solids of D interface
  draws. The scrim at draw P (rgba 66000000, element 0, full-view by its
  own matrix) is withheld; the dialog's black backing is the game's own
  eye-level layer and stays...` -- the engage line. After the first four,
  it logs again only when the scrim's position moved; the percent text
  re-measures constantly and the scrim holds still.
* `loading panel: measured N draw(s) and drew no conclusion -- REASON.`
  -- every no-verdict, preceded by one `panel ...` line per captured
  panel with colour and element bytes.
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
   `advanced.quad_probe = 4259x2395:X:30:120`. Watch both intro dialogs.
   Expect: no tint anywhere beyond the dialogs, the black box and the
   orange border exactly as the game draws them, a brief full-view scrim
   during fade-in (measurement gating), and the engage line naming the
   withheld scrim. The screen should look like the screenshot, minus the
   dimming of the splash art.
2. On success, flip the shipped default to `fit` in a release commit, and
   consider retiring the probe key from the ini.

Flights already flown: 11:11 (union rule -- refused, union spanned the
surface); 11:27 (seeded growth -- refused; killed vertex-space
measurement); 11:42 (viewport model -- refused: every viewport full,
every scissor off; forced the shader read that found the element
mechanism); 11:56 (element model, panel-level hunt -- refused: all
standalone panels ride element 0 at full view); 12:04 (per-quad hunt by
vertex colour -- refused: batch vertices are colour-neutral); 12:25
(estimated rendered colour -- refused, but validated the estimator on the
hologram's exact Elite orange and emptied the dark-covering list); 12:40
(textured draws included -- refused: still nothing dark and boxed, all
blends plain src-alpha, which with the field's screenshot forced the
eye-level conclusion); ~12:55 (THE WITHHOLD ENGAGED -- field: "almost
perfect now", scrim briefly visible at first: the stability gate's cost);
~13:05 (ordinal chain, 2-frame grace -- field: flash at EVERY modal, the
inter-dialog gaps); 13:19 (300-frame grace -- field: perfect except one
flash after the white text, before the first modal: the several-second
gap there out-lasted the grace SILENTLY, and 18 silent re-measures in 7s
exposed the interface's scattered panel-skip frames -- both fixed by the
scene-retirement chain above).

## What must not regress

`fix.loading_dim = off` is settled and separate (commit `77b7119`,
`docs/loading-scrim.md`). The scrim_fix mechanism -- wash discriminated by
its 16x16 BC1 multiplier texture -- shares nothing with the panel path.

## A note on scale

This is a dark rectangle behind a dialog that shows for a few seconds. It
cost a dozen field flights and seven architectures, and every dead one
died of a measurement its instruments could not take: signature-blind
skips, a first-frame probe, unions over incomparable spaces, rasterizer
state that never varied, colour that lived in a table, and finally a box
that was never in the surface being searched. What the search bought is
exact knowledge of Elite's widget system -- the element table, the vertex
index, the styling params, all committed under docs/shaders/ -- and a
final mechanism whose whole action is to NOT draw one draw, gated by that
knowledge until it is beyond doubt. The scrim's collapse and the scrim's
absence are the same pixels on an opaque box; the game already draws
everything the field wanted kept. Worth doing properly or not at all;
the properly turned out to be almost nothing, known for certain.
