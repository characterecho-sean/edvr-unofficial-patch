# The black planet in one eye (DSS and FSS)

## Status

- **State: CLOSED 2026-10-09, on main.** Root cause: Frontier's f3d
  vertex-buffer state cache. EDVR repairs it at the entry of the game's own
  input-assembler flush, always on. The older workaround, `fix.scanner_body`
  (the lend), is retired.
- **Root cause** (READ = an instruction of build 332841, disassembled in
  `analysis\decomp\scanner_vb_*`; INFERRED = what they imply; the first dated
  entry has the whole mechanism):
  - READ: SetVertexBuffer 0x1404E9D70 compares only the DESIRED state
    (`list+0x60+8*slot` wrapper, `+0xE0` offset) and returns at 0x1404E9DAF
    when it is equal; on a change it writes desired and the APPLIED cache
    (`list+0x408` buffer, `+0x4C8` offset) together.
  - READ: FlushIA 0x140522A50 zeroes applied buffer, offset and stride for the
    slots from the draw's new count up to the last draw's
    (0x140522B7C..0x140522BA0), never touches desired, and binds from applied
    (0x140522BF5).
  - INFERRED: resolve 1 binds Q; two zero-slot draws zero applied[0]; the
    second eye's SetVertexBuffer(Q) equals desired and is skipped; FlushIA
    binds (NULL, stride 20, offset 0): the resolve with slot 0 empty.
- **What ships** (`vertex_resync_hook.cpp`, logic in `vertex_resync_core.h`):
  a CodeHook at FlushIA's entry (EliteDangerous64.exe+0x522A50) makes applied
  agree with desired for the slots the layout uses, then runs the original.
  Always on, no key, VR and flat. Build-gated on the PE pair (stamp
  1788384820, image 104894464) and FlushIA's first 16 bytes; elsewhere it is
  inert with one log line (`NOT installed -- why`) and never retried.
- **Log:** `hook armed ...`; 8 first-sighting lines; `N stale vertex-buffer
  bindings in the last 60 s (repaired)` while non-zero; and, so quiet can be
  told from absent, `vertex resync: armed; N stale vertex-buffer bindings
  repaired in the last 10 min (M flushes seen)` every 10 minutes, zero counts
  included, and once at the end (`... repaired this session ...`). A hook that
  never armed, or was stood down, says neither.
- **Flight evidence** (test build e3692ed4, Steam, Crystal Super, 2026-10-09):
  - hook armed at 20:10:47 (`stolen=5 bytes, prologue 16/16 bytes verified`);
  - 3 stale bindings repaired at 20:17:55-20:18:00, each logged with slot 0,
    desired wrapper 0x25C03BCDE60, applied 0x0, layout slots 1, thread 7840
    (render), game stack starting 0x510409;
  - no `scanner body fix: lent` summary in the whole session. That line prints
    only non-zero counts and the lend was enabled, so this is no non-zero lend
    count, not proof the lend never ran;
  - the 26 s window with the repair off saw no stale binding: inconclusive;
  - a boarding crash in the same session was Elite's own allocator lock
    (ED+0x5F02F00) on a job thread, no EDVR frames, matching a 2026-09-25 flat
    crash from before the hook existed. Sean approved shipping.
- **Retired:** `fix.scanner_body`, the lend (`resolve_bind_fix`), its menu
  row, census row and rig; an old line is carried under "this version no
  longer uses it" (config_test, installer_test). The A/B key
  `advanced.vertex_resync` is removed: the hook always repairs, so review
  finding 2 (counts mislabelled across a live on/off change) is moot. Finding
  4 is the heartbeat; its flush count is a relaxed load and store on its own
  cache line, no lock prefix.
- **Open:** a second SteamVR user saw a dim, not black, right-eye body with the
  old fix (the lend) apparently on; logs pending; possibly a different variant
  this repair would not address. Also: whether FlushIA's early return (no
  relevant dirty bit) can hide a stale slot, and whether deferred command
  lists see it. Ruled out: nothing new.

## The report

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

## The first fix: the lend (retired 2026-10-09)

`fix.scanner_body = on` (field-verified 2026-09-01; the code is gone, see the
dated entries): every resolve draw that **had** a vertex buffer refreshed a
remembered (buffer, stride, offset), a reference held so the object could
never be dead when needed. A resolve draw that arrived with slot 0 empty was
drawn with the remembered buffer and the empty binding put back afterwards.
The healthy eye drew first in all ten recorded failing frames, so the lend was
always backward in time; on a rig that never dropped the binding it never
engaged. It healed what the draw was given. The repair at FlushIA's entry fixes
why the draw was given nothing.

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


## 2026-10-09: the root cause is Frontier's f3d state cache; the repair at FlushIA's entry (built)

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
buffer is not native it is a stale binding, counted, and applied buffer = native and applied offset = the desired offset. Slots at or above the count, empty desired
slots and every stride are left as the game has them, so stock behaviour is otherwise identical. The original runs next through the relay. The reads are SEH-guarded (eight
faults stand it down); a build or prologue mismatch, a CodeHook refusal or no relay memory is one log line, the hook stays out, and it is never retried. Installed once at
startup by device_hook.cpp in the VR and the flat profile alike.

**Instruments (the test build).** `vertex resync: hook armed ...` (or `NOT installed -- why`); up to 8 `vertex resync: stale binding N of 8: ...` lines; `vertex resync: N stale
vertex-buffer bindings in the last 60 s (repaired|left as the game had them)` per 60 s while non-zero; and `scanner body fix: lent the other eye's buffer N times in the last
60 s` from the lend. The ship commit below changed the middle two and removed the last.

**The test build's key.** `advanced.vertex_resync = on | off` (default on, live; off counted and wrote nothing) was the A/B switch for the flight. Removed in the ship commit below.

**Tests.** `tools\vertex_resync_test`: the failing sequence replayed on a fake list at the real offsets (stock binds NULL, repaired binds Q with its offset, slots past the count,
a null desired slot, a count above 16 and null pointers untouched); the gate and every one of the 16 bytes; the exact lines and windows; the production hook on a synthetic function
with the real prologue, through the real CodeHook; the glue read as text. The ship commit's cells are listed below.

**Not known.** FlushIA returns before binding when the dirty word has no relevant bit; a stale slot there would be repaired in the cache but not bound by that call (the
failing sequence changes the layout, so its flush proceeds). The review of the test branch checked this: 0x140510B10 sets dirty bits 0xFFFE20, and FlushIA's 0xFBFE20 test then
reaches the bind, so the existing replay is consistent with the sequence. A dirty-clear stale cache is still an open hypothesis. Whether deferred command lists see the same
thing, and whether the count is ever non-zero on a rig that never showed the black body, are what the counts answer.

## 2026-10-09 (ship): flown, always on, heartbeat, the lend and the key retired

The flight (test build e3692ed4, Steam, Crystal Super): the hook armed at 20:10:47 and repaired 3 stale bindings at 20:17:55-20:18:00, each slot 0, desired wrapper 0x25C03BCDE60,
applied 0x0, layout slots 1, thread 7840 (render), stack starting 0x510409. 0x510409 is the return address of the call at 0x140510404, the FlushIA call site this document named from the disassembly, so the sequence above happened on a real rig and the cache really was stale there. No `scanner body fix: lent` summary appeared in the whole session; the line
prints only non-zero counts and the lend was enabled. The 26 s `off` window saw no stale binding, so the key's off half is inconclusive and was never a control. Sean approved
shipping and had pre-approved retiring `fix.scanner_body`.

What changed in the one commit:
- **Always on.** `advanced.vertex_resync` (key, flat allow-list entry, edvr.ini entry, the announce-on-change line) is gone. The hook repairs whenever it is armed. Review finding 2
  (counts mislabelled across a live on/off change) is moot: there is no change to mislabel.
- **No per-flush atomic read-modify-write.** The test build bumped `g_calls`, `g_desynced` and `g_repaired` with `fetch_add` on every game flush in both profiles; nothing read them
  in production. They are gone. The rare counts stay (a repair, a fault). The new flush count is `vresync::FlushCount`, one global on a cache line of its own (`alignas(64)`, a
  `static_assert` and a rig cell hold the size), bumped with a relaxed load and a relaxed store: two plain moves, no lock prefix. A second thread flushing at the same instant
  (a deferred context) can lose a count, so M is a lower bound that means activity; the render thread is the one that matters. It is read only by the poll every 10 minutes and at
  shutdown. Chosen over sampling because a count that is exact on the render thread costs the same two moves as a sampled one.
- **Heartbeat (review finding 4).** Every 10 minutes, zero counts included, while the hook is armed and not stood down: `vertex resync: armed; N stale vertex-buffer bindings
  repaired in the last 10 min (M flushes seen)` (N and M are that window's, not the session's). At the end of the session: `vertex resync: armed; N stale vertex-buffer bindings
  repaired this session (M flushes seen)`. A FreeLibrary teardown says it from `vertexResyncShutdown`; the game never unloads the DLL, so its process exit says it through a new
  `Log::setExitLine` callback the hook registers when it arms, written by `Log::detachDuringProcessExit` from a stack buffer (no lock, no heap). The 60 s non-zero line and the
  8 first-sighting lines stay.
- **The lend is retired**: `resolve_bind_fix.cpp/.h`, `fix.scanner_body`, its menu row, its census row, `tools\resolve_bind_test`, its build.bat entries. Retired as 0387a59a and
  50085092 did: `scanner_body` is in config_test's kRetiredKeys, and installer_test pins that an old `scanner_body` line is carried with its value under "this version no longer
  uses it", reported as retired, not duplicated by a second merge.
- **Tests.** `tools\vertex_resync_test` is V1 replay, V2 gate and prologue, V3 instruments, V4 hook, V5 report (60 s, heartbeat with zero and non-zero counts and the 10-minute
  window, the shutdown line, the process-exit line through the real Log), V6 faults and stand-down (no heartbeat, no session line), V7 glue. Sixty mutants, each caught by its case.
- **Open:** a second user on SteamVR saw a dim, not black, right-eye body with the old fix apparently on; logs pending. If it is a different variant, the next evidence is its
  log: the heartbeat now says whether the hook armed, saw flushes and repaired anything on that rig.
