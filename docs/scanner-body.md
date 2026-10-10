# The black planet in one eye (DSS and FSS)

## Status

- **State: REOPENED 2026-10-09. The root cause is in Frontier's f3d state
  cache; a game-side fix is built and NOT FLOWN.** `fix.scanner_body`'s lend
  (field-verified 2026-09-01, sections below) heals the same symptom one level
  up and stays until the flight. After a successful flight the key and the
  lend are to be retired (Sean pre-approved); that is NOT in the commit that
  added the fix.
- **Root cause** (READ = an instruction of build 332841, disassembled in
  `analysis\decomp\scanner_vb_*`; INFERRED = the sequence they imply; the
  dated entry at the end has the addresses):
  - READ: SetVertexBuffer 0x1404E9D70 compares only the DESIRED state and
    skips when it is equal; on a change it writes desired and the APPLIED cache
    together. FlushIA 0x140522A50 zeroes applied slots when the draw's layout
    has fewer slots than the last draw's, never touches desired, and binds from
    applied.
  - INFERRED: resolve 1 binds Q; two zero-slot full-screen draws zero
    applied[0]; the second eye's SetVertexBuffer(Q) equals desired and is
    skipped; FlushIA binds applied[0] = NULL with stride 20 from desired. That
    is the resolve with slot 0 empty. The task counted 38 of 38 field frames
    consistent with it; no flight has yet shown the cache stale.
- **The fix under test:** a CodeHook at FlushIA's entry
  (`vertex_resync_hook.cpp`, pure logic in `vertex_resync_core.h`) makes
  applied agree with desired for the slots the layout uses (buffer and
  offset), before the original runs. Installed once at startup in the VR and
  the flat profile, gated on the PE pair (stamp 1788384820, image 104894464)
  and FlushIA's first 16 bytes; a mismatch is one log line and never retried.
- **Temporary key:** `advanced.vertex_resync = on | off`, default on, live,
  allow-listed for flat. Off counts and writes nothing. Remove it when the arc
  closes. `fix.scanner_body` is the other key this arc retires.
- **Flight protocol (Sean's rig, DSS scans, live):** key on, scan; key off,
  scan. Read the log with `edvr_log.py`: `vertex resync: hook armed`, the
  first-sighting lines (at most 8), `vertex resync: N stale vertex-buffer
  bindings in the last 60 s (repaired|left as the game had them)`, and
  `scanner body fix: lent the other eye's buffer N times in the last 60 s`.
  With the key on and the fix working, N stays 0 on the lend line and the body
  is lit in both eyes. With it off the counts show whether the cache went stale
  (the mechanism) and the body goes black as before. Neither line ever
  appearing means the failure is elsewhere.
- **Open:** whether FlushIA's early return (no dirty bit) can hide a stale
  slot the repair would not bind; whether deferred command lists see it; the
  first flight. Ruled out: nothing new.

*Frontier issue [78021](https://issues.frontierstore.net/issue-detail/78021),
"Detailed surface scanner eye mismatch in VR", filed 2025-08-22 against
4.2.0.1, Confirmed.* Investigated 2026-08-30 to 2026-09-01 by remote worksheet
with one volunteer (Quest 3, Virtual Desktop): scanning a planet, the body is a
**featureless black disc in the right eye** with its exact silhouette; markers
and UI are fine; only the DSS and FSS. A stock bug, absent on both dev rigs.

> **2026-09-01, solved at the symptom, field-verified:** the second eye's
> deferred lighting resolve is issued with **no vertex buffer at IA slot 0**, so
> nothing rasterises. `fix.scanner_body = on` lends it the other eye's buffer;
> the ENGAGED line reported stride 20, offset 0. That hunt's instruments were
> retired 2026-09-29 (d38272de).

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

The tracker already holds the report: issue
[78021](https://issues.frontierstore.net/issue-detail/78021), Confirmed,
filed a year before the mechanism was found. This document is the source
material for what to add to it — the second eye's resolve issued with no
vertex buffer, and the lend that heals it.

## Coda: the DSS heat map

With a signal filter up in the DSS, the game shades signal locations in
blue. The first read on this compared the two eyes' hotspot pixels
directly and found a ~7% blue lift in the lit eye, calling the overlay
merely faint in VR. That was a mis-registration: the two eyes' projections
sit off-centre from each other, so the comparison was reading the healthy
eye's planet against the unhealthy eye's empty sky beside it. Registered
properly, the deficit is real but different — the overlay present at the
black eye's hotspots, and at the same positions in a lit eye under 1% of
that value:

| | R | G | B | blue excess B−(R+G)/2 |
|---|---|---|---|---|
| black eye, hotspots (overlay alone over nothing) | 0.0019 | 0.0429 | 0.0847 | 0.062 |
| black eye, mapped-area wash next to hotspots | 0.0013 | 0.0032 | 0.0036 | 0.0014 |
| lit eye, SAME planet positions | 0.0148 | 0.0148 | 0.0143 | -0.0005 |

EDVR tried three mechanisms across 2026-09-01..02 to recover it:
re-issuing the overlay draw extra times to stack its additive
contribution; forcing off fields of the hardware depth-stencil state the
draw used; and transcribing the fill shader's own disassembly into a
replacement able to skip each of its three in-shader discards in turn, to
find which one was rejecting the fragment. None is in the tree any more.
The 2026-09-02 game update (build 332753) restored the heat map in VR on
its own — Arioch confirmed it in the headset — and EDVR now ships nothing
for it.

The lesson that outlives the hunt: register the two eyes before comparing
pixels, the miss that produced the 7% figure above. `tools/diff_eye_split.py`
now does that itself before tiling (commit 8676060).


## 2026-10-09: the root cause is Frontier's f3d state cache; the repair at FlushIA's entry (built, NOT FLOWN)

The lend heals what the draw is given. This finds why the draw was given nothing. All of Elite's D3D11 goes through Frontier's "f3d" layer
(`f3dPipelineState_DX11.cpp`). A command list holds two copies of the vertex-buffer state, at fixed offsets from the list. READ below means an instruction of build 332841
(`analysis\decomp\scanner_vb_dump_*.txt`, `scanner_vb_dump_setvb_1404e9d70.txt`, `scanner_vb_dump_flush_ia_140522a50.txt`; the Epic exe and the analysis copy give the same
bytes, SHA-256 e6be8bbe...4e988). INFERRED means what those instructions imply together.

- **DESIRED** (READ): wrapper `[list+8*slot+0x60]`, offset `[list+4*slot+0xE0]`, stride override `[list+4*slot+0x120]`; the dirty word is `[list+0x1E8]`.
- **APPLIED**, the cache of what was last bound (READ): native `ID3D11Buffer*` `[list+8*slot+0x408]`, stride `[list+4*slot+0x488]`, offset `[list+4*slot+0x4C8]`.
- **SetVertexBuffer 0x1404E9D70** (READ): loads the desired wrapper, offset and override, compares them with its arguments and returns at 0x1404E9DAF when all three are equal.
  Only when they differ does it write desired, set dirty bit 0x20 and write applied: `[list+8*slot+0x408] = [wrapper+0x140]`, `[list+4*slot+0x4C8] = offset`,
  `[list+4*slot+0x488] = 0`. The applied copy is never consulted.
- **FlushIA 0x140522A50** (READ; called from 0x140510404 with rcx = the pipeline state, r8 = list+0x60 desired, r9 = list+0x2A0, so r9+0x168 = applied buffers, +0x228 =
  offsets, +0x1E8 = strides; the call site is 0x140510BE9 in the setters dump, `lea r9,[rbx+0x2a0]` / `lea r8,[rbx+0x60]`): returns early when the dirty word has none of
  0xFBFE20; reads the layout object `[pso+0x188]` and its slot count `[[pso+0x188]+0x60]`; computes each slot's stride from desired (only for a slot with a desired wrapper);
  for slots from the new count up to the old count `[r9+0x118]` writes zero to applied buffer, offset and stride (0x140522B7C..0x140522BA0), never touching desired; binds from
  the applied arrays (the call through `[r10+0x90]` at 0x140522BF5, IASetVertexBuffers).
- **The sequence** (INFERRED): the first eye's resolve binds Q (desired = applied = Q). Two zero-slot full-screen draws follow; their FlushIA zeroes applied[0] and binds
  (NULL, 0, 0), and desired[0] stays Q. The second eye's resolve calls SetVertexBuffer(Q): equal to desired, SKIPPED, applied[0] not rewritten. FlushIA computes stride 20 from
  desired and binds applied[0] = NULL: (NULL, 20, 0). That is the draw with slot 0 empty, stride 20, offset 0 the lend's ENGAGED line named on 2026-09-01.
- **Why only some rigs and modes** (INFERRED): it needs the zero-slot draws between the two resolves and a skipped SetVertexBuffer, which is timing and what the mode inserts
  between the eyes; the same as the "state-cache desync" this document had already guessed at, now located.

**The repair.** A CodeHook at FlushIA's entry (EliteDangerous64.exe+0x522A50; first 16 bytes read from the exe: `48 89 5C 24 18 56 57 41 56 48 83 EC 30 49 8B F8`, CodeHook
steals the first instruction, 5 bytes, no relative operand). For each slot below min(layout count, 16): if desired holds a wrapper, native = `[wrapper+0x140]`; if applied's
buffer is not native it is a desync, counted, and with the key on applied buffer = native and applied offset = the desired offset. Slots at or above the count, empty desired
slots and every stride are left as the game has them, so stock behaviour is otherwise identical. The original runs next through the relay. The reads are SEH-guarded (eight
faults stand it down); a build or prologue mismatch, a CodeHook refusal or no relay memory is one log line, the hook stays out, and it is never retried. Installed once at
startup by device_hook.cpp in the VR and the flat profile alike.

**Instruments.** `vertex resync: hook armed ...` (or `NOT installed -- why`); up to 8 `vertex resync: stale binding N of 8: slot, list, desired wrapper, native buffer, applied
buffer, layout slots, thread, repaired|left as the game had it; game stack (RVAs)` lines; `vertex resync: N stale vertex-buffer bindings in the last 60 s (repaired|left as the
game had them)` per 60 s while non-zero. And `scanner body fix: lent the other eye's buffer N times in the last 60 s` per 60 s while non-zero (the ENGAGED line stays). With the
repair working the lend count stays 0.

**Temporary key:** `advanced.vertex_resync = on | off` (default on, live; off counts and writes nothing). Retire it, `fix.scanner_body` and the lend after the flight.

**Tests.** `tools\vertex_resync_test` (44 mutants): the failing sequence replayed on a fake list at the real offsets (stock binds NULL, repaired binds Q with its offset, off writes
nothing, slots past the count, a null desired slot, a count above 16 and null pointers untouched); the gate and every one of the 16 bytes; the key spellings and the flat
allow-list; the exact lines and windows; the production hook on a synthetic function with the real prologue, through the real CodeHook; the glue read as text.
`resolve_bind_test` holds the lend window.

**Not known.** FlushIA returns before binding when the dirty word has no relevant bit; a stale slot there would be repaired in the cache but not bound by that call (the
failing sequence changes the layout, so its flush proceeds). Whether deferred command lists see the same thing, and whether the count is ever non-zero on a rig that never
showed the black body, are what the flight and the counts answer.
