# Engine render performance: a design space

## Status

- **State:** seeded 2026-09-19 (Sean: "secondary goal — improve engine
  render performance where possible"). Nothing built. This doc owns the
  performance arc; settlement-flicker-2026-09-17.md keeps the flicker
  diagnosis. The two share instruments and flights.
- **Open:** every lever below is a hypothesis until its named instrument
  produces a number. The first shared measurement is the job-bracket
  flight (probes installed 2026-09-19 evening, commit 116a2e0).
- **Ruled out (pointers, do not re-propose):** draw-call identity/motion
  estimation as a class — kinematic-motion-injection-2026-09-19.md.
  Compensation-style tweaks (sharpening over blur, threshold nudges) —
  AGENTS.md diagnosis discipline.
- **Next flight:** one settlement eye burst on the census-join build
  (bucket item counter landed 2026-09-21 06:37; frontier d3d11
  sha256:16 01f727ef8fe1e808). Read kinematicEval.bucket_items in the
  classification JSON against the gfx-log draw census; discriminators
  named in the 06:37 entry. Verify build first:
  `python tools\edvr_log.py --target frontier --expect-build HEAD`.

## Frame budget philosophy

Root causes over compensation. Every lever must name (a) the measured
cost it attacks, (b) the instrument that proves the cost, (c) the
expected saving, (d) the observable regression risk. A lever without a
measurement does not get built — one flight per hypothesis is the most
expensive way to run this project, so instruments batch.

## What is measured so far (pointers, not restatements)

- ~23k D3D11 calls per frame at settlements, ~76% zero-sample draws
  (draws whose query never samples a pixel) — settlement-flicker arc,
  draw census + original-draw probe.
- KinematicRig eval FUN_14430EFE0 and its LOD-metric/traversal callers
  are the suspected settlement CPU tax; the sibling jobs (physics
  advance, render-data update, the 0x4320340 batch) are unmeasured.
- The 0x4320340 caller/job-dispatch path and the visibility masks around
  0x431B11D–0x431B221 (what decides a record dispatches at all) are
  RESOLVED offline (2026-09-20 20:05 entry; no flight spent — every piece
  was already in analysis\decomp). Four dispatch levels named, and the
  engine's feedable visibility bit identified: render-record +0x570 mask
  word. The +0x570 writer is IDENTIFIED (20:25 entry): the ctx builder
  copies it from the source model record at ctx build time and nothing
  rewrites it per frame — a cleared bit persists until content rebuild.
  L2's remaining unknown: the census join. (Bit-to-draw-class semantics
  resolved to the model at 21:45: source-record +0x560 is copied whole from
  model+0x8E0 by the source-record builder; per-bit class names sit one
  hop up, not needed for whole-word suppression.)

## Levers, cheapest measurement first

### L1. Know the split: CPU job brackets (first measurement 2026-09-19, eye run 205251)

First numbers: UpdateRenderDataJob ~7.0 ms summed per frame (138 calls
x 51 us), RenderDataBatch (0x4320340) ~2.4 ms, physics <= 0.05 ms.
**Do not read this as proved dominance** (Sean's review, settlement doc
21:23 entry): summed job durations can overlap across worker threads;
the detailed observer takes the probe mutex thousands of times per frame
inside the measured jobs; PrePhysicsAdvanceJob was never hooked. Next
measurement: brackets only (detailed observer disabled), job-3 included,
before any lever is sized on these numbers.

### L2. Draw suppression of proven-zero-sample draws

If the census shows the same draw families sample zero pixels for many
consecutive frames, suppress the original draw (not just skip our own
work). The engine's own visibility bits are now mapped (2026-09-20 20:05
entry): the feedable point is the render-record +0x570 mask word, ANDed
against the dispatch mask at draw-build time in FUN_1442B4420 and ORed
into the rig accumulator in FUN_14431AFE0 — clearing a proven-zero-sample
record's bits there feeds the engine's own mechanism instead of
second-guessing it. The +0x570 writer is now identified (2026-09-20
20:25 entry) and is content-static: the mask is copied once from the
source model record at ctx build and never rewritten per frame, so a
cleared bit persists until the ctx is rebuilt — re-assert on rebuild,
not per frame. The content pipeline is mapped end to end (2026-09-20
21:45 entry): the mask is born on the model object (+0x8E0) and copied
down whole at every hop — MOV-only at all three levels, never
accumulated in place. The census join is INSTRUMENTED (2026-09-21
06:37 entry): the bucket item counter bracket on the draw-item
builder reads engine item production per frame, joining against the
D3D11 draw census on one settlement flight. Remaining before
building: per-bit draw-class names (the model+0x8E0 writer — 30
candidates listed in analysis/decomp/stores_8e0.txt; a ctor default
of 0x3C hints bits are draw-list slots) and the join flight itself.
Risk: suppressing a draw that becomes visible needs a one-frame
restore path, proven on the smoke/static-surface rigs.

### L3. Kinematic eval narrowing

**Blocked pending proven signals** (Sean's review, settlement doc 21:23
entry). The earlier formulation rested on two unproven foundations:
render-record+0x688 is the engine's own intra-frame evaluation sequencer
(its gates already consume it), and record+0x268 is a render-config hash
that never reads the transform — not a motion signal.

What the eval actually does per record per frame (FUN_14430EFE0,
decomp_430EFE0.txt): descriptor mask gate, node liveness bytes, two
flag-word skip gates, then view-dependent work — LOD distance test and a
plane-loop frustum test (FUN_1404f4e10). **LOD and frustum are
view-dependent: they must still run for stationary objects** (buildings
stand still while the head moves). The valid lever, if a clean L1
remeasure still shows eval work dominant, is narrower: avoid REBUILDING
unchanged object data while preserving every view-dependent decision.
What proves "unchanged" is unresolved — the transform/dirty-state
dependency chain is an open offline item.

### L4. Physics decimation for attached-static children

If physics jobs dominate: children rigidly attached to a static parent
do not need per-frame physics advance. Depends on the parent-physics
interface map (KinematicRig +0x1C0, slots +0x90/+0x170) proving which
classes distinguish static/kinematic/parent-relative — the other agent's
current reversing target. Do not build on planet attachment alone.

### L5. Render-thread / submission overlap

Only if L1 shows neither job set dominant: the 23k-call volume itself is
the tax. Options: deferred-context command lists for the swallow/redraw
paths we already own, or batching the swallow quads. Defer until the
openxr-submit-performance arc notes are re-read; there is history there.

## Cross-cutting constraints

- Every lever ships behind a config key naming the user-visible effect,
  off or auto by default per the existing convention.
- Every lever states its environment dependency at flight time: VR
  runtime, headset, per-eye render size, DLSS version, record-table
  sizes.
- Instruments are part of the deliverable. A lever that cannot report
  its own before/after cost in the log does not merge.

## 2026-09-20 (17:55) -- the L1 catch in code: timing is gated on the observer

Verified while wiring the job-attribution TLS mask (kinematic doc 17:50
entry, build installed to frontier): bracket() early-returns the
untimed forward when `!probe || !probe->active()`, so the QPC brackets
measure ONLY while the detailed observer is capturing. That is the L1
catch made concrete: probe on = the observer's per-record mutex/reads
inside every measured job; probe off = no numbers at all, even with the
tracker live (fix.engine_motion on). The 17:50 instrument does not
touch the timing path (its cost is one TLS read/OR/restore per job
call), so L1's remeasure is neither advanced nor worsened -- it is
blocked on exactly this gate.

The unblock is small and probe-side: hoist the QPC bracket off the
active() gate (time whenever the hooks own the job bodies), keeping
observe()/noteOwnership gated as today. Every tracker flight then
returns brackets-only job costs in the jobs[] JSON with no extra
flight, and a probe-armed window still gives the with-observer
comparison for the overlap correction. Semantics change to note:
jobs[] becomes per-session rather than per-capture-window. Job-3
(0x42DF530) still refuses the CodeHook (thunk-shaped leading
instruction) -- extend the hook for that prologue or accept the gap;
physics was <= 0.05 ms on eye run 205251, so job-3 is unlikely to
dominate, but the 21:23 remeasure note explicitly includes it.

If hoisted before the job-attribution flight, ONE flight batches both
measurements: the mover/static discriminator census AND the L1
brackets-only numbers.

### 2026-09-20 18:12 -- L1 unblock landed: brackets time without the observer

Sean sanctioned the hoist ("go"). bracket() now times whenever the hooks
own the job bodies, on the process-lifetime probe global -- the probe's
observer pointer is only consulted for noteOwnership, which keeps its
active() gate alongside observe(). The detailed observer's cost enters
the measured region only while it is capturing: the with/without
comparison L1 asked for now falls out of one log (probe-armed eye-burst
windows vs the tracker-only baseline around them). jobs[] semantics
changed as flagged: per-session accumulation, not per capture window.
Gates unchanged and green (87/0 tracker, 28/0 probe, json self-test,
contract 252/252); frontier d3d11 87d036727d54c0c6, verified. Job-3
remains unhooked (thunk-shaped prologue; physics <= 0.05 ms on 205251,
so the gap is accepted, not extended). The next settlement flight with
fix.engine_motion on batches both measurements: the mover/static
discriminator census (kinematic doc 17:50) and the L1 brackets-only
job costs.

### 2026-09-20 19:10 -- flight 190122: brackets fine, but L1 numbers need a burst

Cross-entry (full read-out in the kinematic doc's same-time foot
entry). The hoisted timing ran all flight on the tracker-only path and
the discriminator census came back a symmetric null (physics jobs never
touch the eval stream -- ruled out as a classifier; this also narrows
L4: physics decimation needs the physics-job decomps to find where
dynamics actually advance, now the shared offline step). But L1's
brackets-only NUMBERS are not in this log: jobs[] accumulates
per-session as designed, yet it only lands in the classification JSON
at dump time -- and no eye burst was taken. The L1 remeasure owes one
burst next flight (any view; the JSON writes regardless).

### 2026-09-20 19:11 -- L4 evidence: physics writes are change-gated; the dirty queue sizes the volume

Cross-entry from the kinematic arc (full entry in the kinematic doc,
same time). UpdatePhysicsObjectsJob (decomp_432B2A0.txt) composes
physics x parent-chain per element (stride 0x30, state==4 bodies) and
writes the node 4x4 ONLY on a bit-exact change, appending changed nodes
to a dirty queue. For L4: sleeping dynamics already cost just one
compare per element, so the decimatable volume is the compose+queue
work of bodies that actually move -- measurable per frame as the dirty
queue's append count (the planned job-2 bracket counter logs it).
PrePhysicsAdvanceCurveJob (0x42DF550) advances animation curves on a
stride-0x18 array, a separate subsystem. PrePhysicsAdvanceJob's real
body is FUN_14431E860 (the 0x42DF530 adapter explains the CodeHook
refusal: it begins with a jump).

### 2026-09-20 19:30 -- the dirty-queue append counter is now measured

Cross-entry from the kinematic arc (full entry in the kinematic doc,
same time). The planned job-2 queue counter landed: bracket reads the
atomic append count (descriptor +0x18) at job entry and exit, probe
accumulates per-session runs/appended/max_delta/resets plus the first
64 non-trivial (entry,exit) pairs into the kinematicEval JSON's new
phys_queue key. For L4, `appended / runs` IS the per-frame physics
write volume -- the decimatable work -- and it arrives in the same eye
burst that finally harvests the L1 brackets-only jobs[] numbers (owed
from flight 190122). Gates green (87/0, 32/0, json self-test, contract
252/252); frontier d3d11 34f9195432f98190, verified. UNFLOWN.

### 2026-09-20 19:45 -- flight 193356: L1 remeasured brackets-only; L4 write volume has its number

The same burst (classification_193539.json) paid both debts. Build
label caveat in the kinematic doc 19:45 entry (installed sha
34f9195432f98190 == 227cd41 content).

**L1, brackets-only, tracker-only path (~103 s, 10758 frames):**
UpdateRenderDataJob 32656 calls, mean 55.1 us, max 25.4 ms (the
scene-load hitch), total 1.799 s (~1.7% of one core); RenderDataBatch
5772 calls, mean 176.0 us, max 3.34 ms, total 1.016 s;
UpdatePhysicsObjectsJob 1196 working runs, mean 7.6 us, 9 ms total --
negligible; Unnamed_BA0 2395 x 0.5 us; PrePhysicsAdvanceJob still
unhooked, CurveJob still never fires. The observer-tax question is
answered for job 0: 51 us/call with the detailed observer (205251)
vs 55.1 us/call without -- the tax is within noise, so 205251's
settlement-scale ~7 ms/frame stands modulo the cross-thread overlap
caveat brackets cannot resolve. Per-FRAME normalization still needs a
parked-at-settlement window: this flight averaged only ~3 job-0
calls/frame because settlement scale (2573 eligible) arrived in the
last ~20 s. Note for readers: jobs[2].calls counts runs whose body
took >= 1 QPC tick; phys_queue.runs (8124) counts dispatches -- the
6.8x gap is the elapsed>0 gate, not a probe defect.

**L4, write volume measured:** 15174 node appends over 1196 working
runs = ~12.7 node writes per working run, peak 133. The queue drains
entirely between runs (resets 0 in 8124), so decimation math can
treat the append count as the batch unit. Order settled: tens of
node writes per frame at settlement scale, not hundreds -- L4's
expected saving is capped accordingly.

### 2026-09-20 20:05 -- L2 gate RESOLVED offline: what decides a record dispatches

Zero flights, zero new Ghidra runs: the mask region 0x431B11D-0x431B221
turned out to live INSIDE FUN_14431AFE0 (the rig-link function the probe
already hooks), and every piece of the dispatch path was already in
analysis\decomp from the settlement arc. Assembled end to end:

**Level 1, per rig per frame -- FUN_14431AFE0(rig, ctx).** Gate: rig
state +0x380 == 4. If rig byte +0x3D1 set, walk the render records
(stride 0x6A0, count *(ctx+0x1A940), base ctx+0x40); per record require
context match (FUN_142840840 vs rig+0x350, byte rig+0x3D2), the
+0x68C-bit-5/dependency-token condition (+0x188 == -1 && rig+0x190),
the per-record predicate FUN_14432C520, and (if DAT_145ea3479) three
further predicates plus the rig+0x46C float gate. Each passing record
ORs its mask word render-record+0x570 into the accumulator. Then the
0x431B11D region proper: clear the per-class suppression bits
(*(ctx+0x1A950/58/60), selected by collection+0x70 bit 7) from the
accumulator. **Accumulator == 0 or collection+0x70 bit 0 clear => the
whole collection is skipped** (collection+0x629 = 0, cleanup via
FUN_14434DD50 / FUN_1442B5760); nonzero => dispatch UpdateRenderDataJob
(FUN_144321940) or its worker-slot equivalent with the mask in the
descriptor (local_50).

**Level 2, per collection -- FUN_144320340 (RenderDataBatch).** The LOD
evaluator FUN_144331300 must return nonzero, else the collection takes
the empty path. Nonzero runs the traversal FUN_144312040 (the world
update / eval fan-out) and then the per-record loop.

**Level 3, per record (stride 0x2F0).** Dispatch requires: pose ctx
rec+0x290 != 0, byte rec+0x234 != 0, byte rec+0x298 != 0 (the probe's
count298 -- the draw-distance nibble), and at least one pending bit in
rec+0x208 whose 4-bit entry in the rec+0x210 nibble table (lane
bit>>4, nibble (bit&0xF)*4) is <= *(ushort*)(node+0x6A), node =
rec+0x18. Passing records call FUN_1442B4420(poseCtx, collection,
traversalMask, nibbleTable) with the predicate pointer rec+0x2C0.

**Level 4, draw-item build -- FUN_1442B4420.** Gated on DAT_145ea3398.
Per render record of the collection: require (rec+0x570 & passedMask)
!= 0, survive the frustum plane-loop FUN_1404F4E10 (return -1 = culled),
and (if DAT_145ea3399 && rec+0x68D bit 0) a further visibility test on
rec+0x688. Visible records OR +0x570 into the visible mask; per set bit,
a table at collection+0x1A840 (uint per bit) maps bit -> render-record
index, FUN_1442B3FC0 tests the candidate, and survivors are appended to
the per-bucket draw lists (FUN_143696FA0 list nodes; item counter at
bucket+0x2A4). Draws are born here.

**Consequence for L2.** The engine visibility bit we should feed is
**render-record +0x570**: consumed both at rig-mask accumulation
(FUN_14431AFE0) and at draw-build (FUN_1442B4420's AND), so clearing
a proven-zero-sample record's bits suppresses it through the engine's
own mechanism at both levels. Unresolved before building: (a) the
+0x570 writer -- a FindStores570 variant of the existing FindStores
scripts finds it; the patch must land after the engine's own per-frame
write; (b) bit semantics per draw class/LOD (the writer reveals them);
(c) the join from these bucket lists to the draw census's 23k D3D11
calls (bucket item counter +0x2A4 is countable per frame). Also
correction-noted: the perf doc's L1 framing suspected eval cost without
a dispatch map; level 3's rec+0x208/nibble test now names the per-record
CPU gate precisely.

### 2026-09-20 20:25 -- +0x570 writer found: content-static, copied from the source model record

The FindStores570 scan (analysis/decomp/stores_570.txt; 19.6M
instructions, 132 writer functions) stores to render-record +0x570 by
**MOV only -- zero OR/AND stores anywhere**, so the mask word is never
accumulated in place. The examined writers are all bookkeeping:
FUN_1442BE1F0 and FUN_1443DE430 are constructors (+0x570 zero-init;
the flag688 tag on the second was a vtable-install coincidence at
index 0xd1), FUN_1442BF9D0 is a move/transfer (dst = src, src = 0),
and FUN_1442D46C0 is a std::vector capacity field at +0x570 (offset
collision).

Pivot: the render-record COUNT field ctx+0x1A940 has exactly two
writers (FindStores1A940; analysis/decomp/stores_1a940.txt) --
FUN_1401E4FD0 and FUN_142819D90. FUN_142819D90 calls FUN_142819F70,
the only +0x570 writer also touching the companion fields +0x688 /
+0x68C at the 0x6a0 record stride. Decompiles:
analysis/decomp/decomp_2819F70.txt, decomp_2819D90.txt.

**FUN_142819F70 is the render-record builder.** It copies the mask
verbatim: source model record +0x560 -> render record +0x570. The
same builder copies source +0x558 -> +0x688 (the flag word),
assembles +0x68C from source +0x56C bits, copies source +0x568 ->
+0x578 (the bit -> record-index value used by the ctx+0x1A840 table),
and ORs entry-list masks into +0x580 / +0x588+idx. FUN_142819D90
builds the ctx (called twice from FUN_1401EA920, a content/load path,
not per frame): count at ctx+0x1A940 = (srcEnd - srcBegin) / 0x570
capped at 0x40 (<= 64 records per ctx); source records are stride
0x570; it also builds the bit -> record-index table at ctx+0x1A840
and the exclusion accumulators ctx+0x1A948 (all records' masks) /
+0x1A950 / +0x1A958, partitioned by rec+0x688 & 0x7FF0 / & 0xFF0 --
the exact words FUN_14431AFE0 subtracts per class.

**Nothing rewrites +0x570 per frame.** L2 consequence: clearing a
proven-zero-sample record's bits persists until the ctx is rebuilt;
the re-assert cadence is "hook the builder or re-clear on ctx
rebuild", not per frame. Remaining unknowns before L2 is buildable:
the bit-to-draw-class semantics (the source-record +0x560 writer, in
the content pipeline) and the census join from the bucket lists to
the 23k D3D11 draws. New artifacts: analysis/ghidra_scripts/
FindStores570.java, FindStores1A940.java, run_ghidra_stores570.bat,
run_ghidra_stores1a940.bat; decomp outputs under analysis/decomp/
(gitignored, on disk only).

### 2026-09-20 21:45 -- +0x560 writer found: the mask is born on the model and copied down whole

The L2 bit-semantics scan (FindStores560 + FindStores8E0, new scripts
in analysis/ghidra_scripts; outputs stores_560.txt / stores_8e0.txt)
closes the content pipeline end to end:

**Source-record +0x560 writer = FUN_14280F800, the source-record
builder** (decomp_280F800.txt). It appends stride-0x570 source records
(count at param_1+0x15C00) and fills the new record from the model
object: +0x560 <- model+0x8E0 (the mask), +0x558 <- model+0x18 (flag
word), +0x568 <- model+0x8E8 (bit->index), +0x56C <- the bits that
become render +0x68C, plus the +0x2F0/+0x2F8 pair, +0x300..+0x31C
dwords and the two 16-byte arrays the render-record builder re-copies.
Caller chain closed: the big content-load routine FUN_1401EA920 (the
ctx builder's caller, 20:25 entry) populates its stack arrays via
FUN_140200E20, which calls FUN_14280F800 -- content load -> source
records -> ctx build -> render records, one connected pipeline.

**The other +0x560 writers are bookkeeping.** FUN_143AC3E60 (the other
all-tag candidate) zero-inits +0x560 and sets +0x558=1 (ctor);
FUN_1442BE1F0 / FUN_1443DE430 are the known ctors. MOV-only at every
level, again: no OR/AND store to source+0x560 OR model+0x8E0 exists
anywhere in the binary, so no engine path accumulates mask bits in
place -- every hop copies a fully-computed word.

**Bit semantics live one hop up: model+0x8E0.** 48 writer functions,
30 tagged with the 0x8E8 sibling; byte/word/dword stores are width
collisions (9 dropped). The qword writers are ctor-family (the same
FUN_140869EC0/FUN_140529550 sub-object helpers as the record ctor).
One, FUN_14331C0C0, stores a constant DEFAULT of 0x3C (bits 2-5) --
first semantic hint: bits number draw-list slots, with a standard
four-slot default set and bits 0-1 reserved for something special
(unproven; labelled speculation). Named next hop, not flown/needed
yet: decompile the model+0x8E0 writers (list in stores_8e0.txt) when
pass-selective suppression (shadow vs main) is wanted. L2 as scoped --
clear the whole word of a proven-zero record -- does not need it.

**L2 buildability status:** writer identified (20:25), re-assert
cadence settled (on ctx rebuild, not per frame), pipeline mapped
(21:45). Remaining: the census join from the bucket draw lists to the
23k D3D11 calls, then a build decision.

### 2026-09-21 06:37 -- census-join instrument landed (bucket item counter)

The last L2 unknown Sean sanctioned at 05:07 is now built and
installed: the engine bucket item counter (+0x2A4) rides the existing
kinematic-eval hook set, so ONE settlement flight answers "which
engine draw lists become which D3D11 calls" (join against the draw
census in the gfx log, offline).

**Engine truth, from decomp_42B4420.txt (no flight spent).** The
per-rig draw-item builder FUN_1442B4420 walks its owner's instance
entries -- count u64 @ rigOwner+0x48, array @ +0x50, stride 0x58
(the decompile's param_1+0x12/+0x14 are float*-ELEMENT offsets; the
byte offsets are 0x48/0x50, resolving the overlapping-fields paradox).
Each entry's model (entry+0x0) names its bucket: bucket = *(model+0x20),
list head bucket+0x260, item counter bucket+0x2A4. Items are 0x150
bytes, eight per pool block (pool DAT_145efdd30), with a parallel
per-item mask array after the block's items. The FindBucketOps scan
(new analysis script; output analysis/decomp/bucket_ops.txt) found 136
functions touching +0x2A4 but only FOUR that also reference the block
pool: the two producers inside the bracket (FUN_1442B4420 inline and
its helper FUN_1442B4130), FUN_14369c9c0 (a second producer family,
callers unknown -- caveat below) and FUN_14431305c (a dispatch-side
fragment that also INCs). Resets exist (FUN_14434CD90 /
FUN_14434DB30 zero the counter), so +0x2A4 drains per frame and the
entry/exit delta is the robust read, exactly as designed. Correction
to a line in the 21:45 entry's wake: the local_478 path in
FUN_1442B4420 is NOT dead -- FUN_1442B2730 can set it through a
pointer (the binary's call list shows CALL 0x1442b4130 at 1442b4843);
both append paths run inside the forwarded call, so the bracket
covers both either way.

**The instrument.** kinematic_eval_hook.cpp gains a sixth-param
callback on RVA 0x42B4420 (param_6's 16 bytes are copied into every
item, so the full stack layout must pass through verbatim -- a
four-param forward would have corrupted items). The bracket walks the
entry array at call entry (deduped, cap 64, __try discipline from the
phys-queue capture), reads each bucket's +0x2A4 before and after the
forwarded call, and accumulates per-session into the probe:
calls / items / empty_calls / entry_wild / exit_fault / neg_deltas /
overflow_calls / max_buckets / max_items_per_call, serialized as
kinematicEval.bucket_items in the classification JSON. Per-session,
never active()-gated -- tracker-only flights harvest it. Prologue
verified hookable from the exe bytes (mov r11,rsp; pushes; sub
rsp,0x4D8 -- no thunk). Gates: kinematic_probe_test 38/0 (new case 8
drives every counter path), kinematic json self-test extended and
green, config contract 253/253. Frontier install d3d11 sha256:16
01f727ef8fe1e808 (build predates its commit; hash is truth).

**Flight protocol (next flight).** One settlement eye burst on this
build; read classification_*.json -> kinematicEval.bucket_items.
Dead-instrument discriminators, named before flying: calls == 0 while
draws proceed = the hook stood down at install (CodeHook logs under
kinematic-bucket-build); calls > 0 but empty_calls == calls = the
walk offsets (+0x48/+0x50) are wrong for live rigs -- re-derive, do
not trust a zero. Success shape: calls ~ rig count per frame
(~1.5k), items per call in the tens, and a session items-total that
pairs against the census's ~23k D3D11 calls.

**Caveat for the read.** FUN_14369c9c0 / FUN_14431305c /
FUN_14434D120 INC the same counter pattern on what may be other
passes' lists (shadow etc.). If engine items land far BELOW 23k, the
remainder is those producers plus non-bucket draw sources -- the join
still answers the suppression question for the main list, which is
the only one L2 proposes to feed.
