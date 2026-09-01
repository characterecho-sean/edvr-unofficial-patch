# The black planet in one eye (DSS and FSS)

Investigated 2026-08-30 to 2026-09-01, from field reports: when scanning a
planet, the body renders as a **featureless black disc in the right eye**
and correctly in the left. The disc keeps the body's exact silhouette. Blue
bio/geo markers and the scanner UI render correctly in both eyes. Normal
space and supercruise are fine; only the DSS and FSS views are affected. A
stock game bug that predates EDVR, present on some machines and absent on
others — it never reproduced on either dev rig, and the entire
investigation ran by remote worksheet with one volunteer (Quest 3, Virtual
Desktop, stand-alone launcher).

> **STATUS: SOLVED, field-verified 2026-09-01.** The frame's second eye's
> deferred lighting resolve is issued by the game with **no vertex buffer
> bound at IA slot 0**. Its vertex shader reads real vertex attributes, so
> the draw rasterises nothing: the eye's lit image never receives its
> lighting, and the body's disc — the one region later layers are
> depth-excluded from — stays black with a perfect silhouette. Healed by
> the shipping key `fix.scanner_body = on` (default), which lends the draw
> the buffer the other eye's resolve just used. Verified in the field the
> same day: the fix's ENGAGED line reports stride 20, offset 0 — exactly
> the disassembly's declared inputs — and the body renders in both eyes.

## The mechanism

The deferred lighting resolve (ps `7CECABDE34FFBE9E`, vs
`7E38A6AA1269C901`) runs once per eye, drawing a full-screen quad whose
vertices carry per-eye ray data:

```
vs_5_0
dcl_input v0.xyw      <- position, from the vertex buffer
dcl_input v1.xy       <- uv,       from the vertex buffer
```

Stride 20 = the five declared floats. On healthy rigs both eyes' resolves
bind **the same** vertex buffer. On the failing rig, the frame's second
resolve arrives with slot 0 empty: the input assembler feeds zeros, all
four vertices land at the same point, the quad is degenerate, and **zero
pixels rasterise**. The draw executes with every recordable state healthy —
depth, stencil, blend, write mask, sample mask, predication, viewport,
scissor all clean — and paints nothing.

The visible symptom follows from what surrounds it. The space backdrop
(scene repaint, starfield, galaxy dome -- general passes that run in
every mode, confirmed against a normal-flight census, not something the
scanner adds) is a mono layer redrawn per eye from its
own vertex buffers, at viewport `z=0–0` with depth func GEQUAL under
reversed-Z — so it only lands where the depth buffer is empty. The planet's
depth, laid by the G-buffer pass (which is fine in both eyes), excludes it
from the disc. In the healthy eye the disc therefore shows the resolve's
lit body; in the failing eye it shows the lit buffer's cleared black.
Everything else in that eye looks normal because everything else brings its
own geometry.

Why one eye, some machines, scanner modes only: the two eyes' passes
interleave, and the binding is lost between the first eye's resolve and the
second's — a state-cache desync in the engine whose window depends on what
the mode inserts between the two and on timing, which is why most rigs
never see it. That half is Frontier's.

## The fix

`fix.scanner_body = on` ([resolve_bind_fix.cpp](../src/d3d11/resolve_bind_fix.cpp)):
every resolve draw that **has** a vertex buffer refreshes a remembered
(buffer, stride, offset) — a reference is held so the object can never be
dead when needed. A resolve draw that arrives with slot 0 empty is drawn
with the remembered buffer and the empty binding is put back afterwards.
The healthy eye drew first in all ten recorded failing frames, so the lend
is always backward in time; on a rig that never drops the binding the fix
never engages. It composes with `advanced.resolve_probe` — the bind heals
underneath whatever the probe swaps on top.

## The evidence

The census's `vb=` column, across every capture of the hunt:

| capture | mode | first eye | second eye | body |
|---|---|---|---|---|
| Aug 31 | DSS | `vb=@151` | `vb=-` | black right eye |
| Sep 1 (clears round) ×2 | DSS | bound | `vb=-` | black right eye |
| Sep 1 (noblend round) ×2 | DSS | bound | `vb=-` | black right eye |
| Sep 1 control | normal flight | `vb=@322` | `vb=@322` | **both eyes fine** |

Ten failing frames, one healthy control, no exceptions — and in the healthy
case both eyes share one buffer, which is what makes the lend exact.

## What the road ruled out (kept because it was expensive)

Fifteen post-resolve draws skipped one at a time; the resolve's shader
replaced with a constant (`white`); its depth and stencil tests disabled
individually and together (`nostencil`/`nodepth`/`noboth`); its blend state
forced to the API default (`noblend`); stencil references overridden both
directions (`stencil_probe`); every b2 constant the resolve reads; CS b1
asymmetry; light-cull output; viewport/scissor; cull_guard; OpenXR Toolkit.
**All null — correctly**: every one of them modified the pixel pipeline,
and no pixel ever ran. The nulls were the elimination that forced the
search down into input assembly.

Three patterns that looked like the fault and are **normal engine
behaviour** — recorded so nobody chases them again:

- The four atmosphere draws (`41E245D488BFE83E`, `D95905C18B7FAD93`,
  `D1281DF454A153AD`, `5E417E9DF2E7F9E6`) vanishing from one eye: ordinary
  alternate-frame parity, present in healthy normal flight.
- The scanner dome's (`9BFC7FD232328391`) stencil reference flipping 8↔16:
  bookkeeping that rides the same parity.
- Per-eye stencil-reference deltas on stencil-disabled draws: inert.

The depth clears were textbook throughout (one `f=3 z=0 s=0` per eye,
immediately before each pass), and the captures contain **no occlusion
queries at all** — only timestamp/event profiling.

## Instruments this hunt paid for (all shipped)

- Census columns: viewport/scissor (`vp=`/`sc=`), output-survival
  (`ds= st= bm= pr=`), blend and sample mask (`bl= sm=`), and the `vb=`
  column that closed the case.
- `DCL` lines: colour and depth clears with the game's requested values,
  and query Begin/End with the query's type — `ClearDepthStencilView`,
  `Begin` and `End` had never been hooked.
- `advanced.eye_split` — every eye-sized target of one frame, both eyes
  ([eye-split.md](eye-split.md)).
- `advanced.resolve_probe` — `white`/`inputs` + `nostencil`/`nodepth`/
  `noboth` + `noblend`, composable with `+`.
- `advanced.stencil_probe` — `vs:HASH:REF`, with a same/other tally that
  distinguishes "no change" from "never reproduced".
- SAVE LOGS now bundles the game's graphics settings (`game_graphics/`
  sweep plus the master `GraphicsConfiguration.xml`).

The vendor-facing report is the natural next step once the release ships;
this document is its source material.
