# Engine render performance: a design space

## Status

* **State (2026-09-21, offline trace complete):** the consumer/scheduling
  trace is done — 21 new decompiles plus DAT_145f27db4/vtable xref and
  +0x2a0/+0x2b0 offset scans, all under `analysis/decomp/` and VA-specific to
  binary E6BE8BBE…988. The bucket lifecycle is mapped end to end: factory →
  vtable-worker entry → rekey → four producers append (0x150 record, u64 key)
  batch nodes → drainer → per-bit weighted counting → lightweight command
  enqueue into ring buffers. No L2 suppression is implemented; none is now
  justified. This doc owns the engine-performance arc; the settlement flicker
  arc shares instruments and flights.

* **Suppression refuted as scoped (2026-09-21):** five independent grounds —
  (1) merge keys are copies of DAT_145f27db4, an unrelated Wwise
  memory-category counter bumped unconditionally in FUN_1405d6260, so there is
  no render generation or dirty surface to key on; (2) bucket items are not
  draws — consumption is per-bit counting plus pointer/offset command records
  (FUN_144c837b0 → FUN_144c7ef70), so the census 82% eye share has no proven
  route from these lists; (3) the +0x2B0 dirty flag is written only inside
  FUN_1401ea920's rekey sequence and has no conditional reader in .text; (4)
  the frame scheduler is reachable only through vtable-table callbacks and is
  unnamed by static RE; (5) one producer appends transform-derived packed
  vectors (FUN_14434d470), so the subsystem may be light/probe batching, not
  mesh draws. Do not build bucket clearing, rekeying or mask edits. See the
  2026-09-21 trace entry at the end.

* **Boundary-first attribution (2026-09-21):** the armed-census tally of the
  2026-09-19..21 settlement flights isolates ~78% of the retained eye-pass
  draws to one instanced sourceMesh family (vh=EB5234DB6ADB491D, ~282 verts x
  ~5 instances per draw, stable across three flights), ~9% to an unidentified
  instanced family (vh=8056C9D5F22007F9) and ~3% to small-instance draws
  (vh=2684F02B9B0BB0DE). Eye pass is ~18.3k draws/frame, offscreen ~4.1k. The
  high settlement draw count is one dominant small-batch scenery family — not
  the bucket subsystem, whose items never reach the boundary as draws.

* **Open (flight-worthy only):** one matched A/B census flight at a
  settlement: game material/LOD keys at minimum vs the current Custom preset
  (today MaterialQuality=3, LODDistanceScale=1.0 in Custom.4.4.fxcfg), same
  spot and heading, census armed in both passes. Discriminators: EB-family
  draw count, total eye draws, the two unknown-family shares. If the family
  collapses at min settings, the recoverable-draw lever is proven and
  quantified; if it does not move, settlement scenery ignores the sliders and
  the only remaining lever is EDVR-side work at the boundary. Optional add-on:
  arm EyeDrawSnapshot in the A pass to name the two unknown hashes via
  tools/dxbc_disasm.py. Also still open from the trace: ring-buffer command
  consumers and the frame-scheduler identity (offline-unnamed).

* **Ruled out (pointers, do not re-propose):** draw-call identity/motion
  estimation as a class — kinematic-motion-injection-2026-09-19.md.
  Compensation-style tweaks (sharpening over blur, threshold nudges) —
  AGENTS.md diagnosis discipline. Reusing prior zero samples solely from
  unchanged bindings/payload was refuted by the settlement arc's 2026-09-18
  targeted flight: three identical captures became visible next frame.
  Merge-key control of any kind — the key is an incidental audio counter
  sampled at rekey. The 82% bucket attribution and 40-frame cadence — no
  evidence route exists from the bucket lists to eye-pass draws.

## Frame budget philosophy

Root causes over compensation. Every lever must name (a) the measured cost it
attacks, (b) the instrument that proves the cost, (c) the expected saving, (d)
the observable regression risk. A lever without a measurement does not get
built — one flight per hypothesis is the most expensive way to run this
project, so instruments batch.

## What is measured so far (pointers, not restatements)

* \~23k D3D11 calls per frame at settlements, \~76% zero-sample draws (draws
  whose query never samples a pixel) — settlement-flicker arc, draw census +
  original-draw probe.

* KinematicRig eval FUN\_14430EFE0 and its LOD-metric/traversal callers remain
  CPU candidates. Render update, batch and physics brackets have measurements
  below; worker overlap, unmatched scenes and unhooked PrePhysicsAdvanceJob
  prevent a complete CPU critical-path attribution.

* The 0x4320340 caller/job-dispatch path and the visibility masks around
  0x431B11D–0x431B221 (what decides a record dispatches at all) are RESOLVED
  offline (2026-09-20 20:05 entry; no flight spent — every piece was already in
  analysis\decomp). Four dispatch levels named, and the content-derived
  render-record +0x570 mask is consumed by dispatch and draw building. The
  known copy chain is model+0x8E0 -> source+0x560 -> render-record+0x570. No
  per-frame writer was found in the inspected paths; this is not itself a
  current-view occlusion result or proof of instance-local ownership. Census
  attribution, bit/pass scope and safe mutation/restoration remain open even
  for whole-word suppression.

## Levers, cheapest measurement first

### L1. Know the split: CPU job brackets (first measurement 2026-09-19, eye run 205251)

First numbers: UpdateRenderDataJob \~7.0 ms summed per frame (138 calls x 51
us), RenderDataBatch (0x4320340) \~2.4 ms, physics <= 0.05 ms. **Do not read
this as proved dominance** (Sean's review, settlement doc 21:23 entry): summed
job durations can overlap across worker threads; the detailed observer takes
the probe mutex thousands of times per frame inside the measured jobs;
PrePhysicsAdvanceJob was never hooked. A later brackets-only flight is recorded
at 19:45 below, but its unmatched scene mix does not bound observer overhead.
Next sizing measurement needs a matched settlement window and
critical-path/worker overlap evidence.

### L2. Investigate safe suppression of currently unnecessary draws

Repeated zero samples identify candidates for investigation, not permission to
suppress a record. The settlement arc already observed zero-to-visible
transitions with identical captured draw state. A safe decision needs stable
engine identity, current-view validity, both-eye/pass coverage, absence of
relevant side effects and a restoration/retest path that works while the
original draw is suppressed. A longer history window alone supplies none of
those guarantees.

The +0x570 content mask is a candidate control point: FUN\_14431AFE0 ORs it
into dispatch selection; FUN\_144331300 uses it for current LOD selection;
FUN\_1442B4420 tests it before building items. Existing distance, frustum and
optional visibility gates still matter for static objects. Masks can be
combined into one item, and the 0x6A0 records do not yet have a proven unique
link to the 0x2F0 collection records, buckets or D3D draws. Whole-word clearing
therefore still requires ownership and pass-scope proof.

FUN\_14434DB60 clears/rekeys a bucket. In the examined worker and batch paths,
a nonzero current LOD mask causes clear followed by traversal/build in the same
invocation. No old/new-mask comparison or enqueue exists in that clear. The
upstream scheduling trigger and bucket lifetime across frames remain
unresolved; do not implement an external clear as a forced rebuild.

Before a suppression build: close the item-to-draw join, establish context/bit
fanout and lifetime, locate a serialized instance/pass-scoped interception
point, and define conservative visibility invalidation plus restoration. A
per-job mask inside an already-running worker is a candidate to investigate,
not yet a proven safe alternative. No smoke/static-surface restore test has
validated engine suppression. The 2026-09-21 independent review gives the
combined measurement plan.

### L3. Kinematic eval narrowing

**Blocked pending proven signals** (Sean's review, settlement doc 21:23 entry).
The earlier formulation rested on two unproven foundations: render-record+0x688
is the engine's own intra-frame evaluation sequencer (its gates already consume
it), and record+0x268 is a render-config hash that never reads the transform —
not a motion signal.

What the eval actually does per record per frame (FUN\_14430EFE0,
decomp\_430EFE0.txt): descriptor mask gate, node liveness bytes, two flag-word
skip gates, then view-dependent work — LOD distance test and a plane-loop
frustum test (FUN\_1404f4e10). **LOD and frustum are view-dependent: they must
still run for stationary objects** (buildings stand still while the head
moves). The valid lever, if a clean L1 remeasure still shows eval work
dominant, is narrower: avoid REBUILDING unchanged object data while preserving
every view-dependent decision. What proves "unchanged" is unresolved — the
transform/dirty-state dependency chain is an open offline item.

### L4. Physics decimation for attached-static children

The measured physics job was small in the mixed flight and its write queue is
change-gated; no settlement-scale physics saving is established. Only if a
matched baseline shows meaningful physics cost: children rigidly attached to a
static parent do not need per-frame physics advance. Depends on the
parent-physics interface map (KinematicRig +0x1C0, slots +0x90/+0x170) proving
which classes distinguish static/kinematic/parent-relative — the other agent's
current reversing target. Do not build on planet attachment alone.

### L5. Render-thread / submission overlap

Only if L1 shows neither job set dominant: the 23k-call volume itself is the
tax. Options: deferred-context command lists for the swallow/redraw paths we
already own, or batching the swallow quads. Defer until the
openxr-submit-performance arc notes are re-read; there is history there.

## Cross-cutting constraints

* Every lever ships behind a config key naming the user-visible effect, off or
  auto by default per the existing convention.

* Every lever states its environment dependency at flight time: VR runtime,
  headset, per-eye render size, DLSS version, record-table sizes.

* Instruments are part of the deliverable. A lever that cannot report its own
  before/after cost in the log does not merge.

## 2026-09-20 (17:55) -- the L1 catch in code: timing is gated on the observer

Verified while wiring the job-attribution TLS mask (kinematic doc 17:50 entry,
build installed to frontier): bracket() early-returns the
untimed forward when `!probe || !probe->active()`, so the QPC brackets
measure ONLY while the detailed observer is capturing. That is the L1 catch
made concrete: probe on = the observer's per-record mutex/reads inside every
measured job; probe off = no numbers at all, even with the tracker live
(fix.engine\_motion on). The 17:50 instrument does not touch the timing path
(its cost is one TLS read/OR/restore per job call), so L1's remeasure is
neither advanced nor worsened -- it is blocked on exactly this gate.

The unblock is small and probe-side: hoist the QPC bracket off the active()
gate (time whenever the hooks own the job bodies), keeping
observe()/noteOwnership gated as today. Every tracker flight then returns
brackets-only job costs in the jobs\[] JSON with no extra flight, and a
probe-armed window still gives the with-observer comparison for the overlap
correction. Semantics change to note: jobs\[] becomes per-session rather than
per-capture-window. Job-3 (0x42DF530) still refuses the CodeHook (thunk-shaped
leading instruction) -- extend the hook for that prologue or accept the gap;
physics was <= 0.05 ms on eye run 205251, so job-3 is unlikely to dominate, but
the 21:23 remeasure note explicitly includes it.

If hoisted before the job-attribution flight, ONE flight batches both
measurements: the mover/static discriminator census AND the L1 brackets-only
numbers.

### 2026-09-20 18:12 -- L1 unblock landed: brackets time without the observer

Sean sanctioned the hoist ("go"). bracket() now times whenever the hooks own
the job bodies, on the process-lifetime probe global -- the probe's observer
pointer is only consulted for noteOwnership, which keeps its active() gate
alongside observe(). The detailed observer's cost enters the measured region
only while it is capturing: the with/without comparison L1 asked for now falls
out of one log (probe-armed eye-burst windows vs the tracker-only baseline
around them). jobs\[] semantics changed as flagged: per-session accumulation,
not per capture window. Gates unchanged and green (87/0 tracker, 28/0 probe,
json self-test, contract 252/252); frontier d3d11 87d036727d54c0c6, verified.
Job-3 remains unhooked (thunk-shaped prologue; physics <= 0.05 ms on 205251, so
the gap is accepted, not extended). The next settlement flight with
fix.engine\_motion on batches both measurements: the mover/static discriminator
census (kinematic doc 17:50) and the L1 brackets-only job costs.

### 2026-09-20 19:10 -- flight 190122: brackets fine, but L1 numbers need a burst

Cross-entry (full read-out in the kinematic doc's same-time foot entry). The
hoisted timing ran all flight on the tracker-only path and the discriminator
census came back a symmetric null (physics jobs never touch the eval stream --
ruled out as a classifier; this also narrows L4: physics decimation needs the
physics-job decomps to find where dynamics actually advance, now the shared
offline step). But L1's brackets-only NUMBERS are not in this log: jobs\[]
accumulates per-session as designed, yet it only lands in the classification
JSON at dump time -- and no eye burst was taken. The L1 remeasure owes one
burst next flight (any view; the JSON writes regardless).

### 2026-09-20 19:11 -- L4 evidence: physics writes are change-gated; the dirty queue sizes the volume

Cross-entry from the kinematic arc (full entry in the kinematic doc, same
time). UpdatePhysicsObjectsJob (decomp\_432B2A0.txt) composes physics x
parent-chain per element (stride 0x30, state==4 bodies) and writes the node 4x4
ONLY on a bit-exact change, appending changed nodes to a dirty queue. Unchanged
results avoid the node write and queue append; the preceding composition and
comparison can still cost work. The queue's append count measures changed-node
output, not total physics/composition cost. PrePhysicsAdvanceCurveJob
(0x42DF550) advances animation curves on a stride-0x18 array, a separate
subsystem. PrePhysicsAdvanceJob's real body is FUN\_14431E860 (the 0x42DF530
adapter explains the CodeHook refusal: it begins with a jump).

### 2026-09-20 19:30 -- the dirty-queue append counter is now measured

Cross-entry from the kinematic arc (full entry in the kinematic doc, same
time). The planned job-2 queue counter landed: bracket reads the atomic append
count (descriptor +0x18) at job entry and exit, probe accumulates per-session
runs/appended/max\_delta/resets plus the first 64 non-trivial (entry,exit)
pairs into the kinematicEval JSON's new phys\_queue key. For L4, `appended /
runs` measures appends per dispatch; per-frame volume requires a frame delta
over the same window. It arrives in the same eye burst that finally harvests
the L1 brackets-only jobs\[] numbers (owed from flight 190122). Gates green
(87/0, 32/0, json self-test, contract 252/252); frontier d3d11
34f9195432f98190, verified. UNFLOWN.

### 2026-09-20 19:45 -- flight 193356: L1 remeasured brackets-only; L4 write volume has its number

The same burst (classification\_193539.json) paid both debts. Build label
caveat in the kinematic doc 19:45 entry (installed sha 34f9195432f98190 ==
227cd41 content).

**L1, brackets-only, tracker-only path (\~103 s, 10758 frames):**
UpdateRenderDataJob 32656 calls, mean 55.1 us, max 25.4 ms (the scene-load
hitch), total 1.799 s (\~1.7% of one core); RenderDataBatch 5772 calls, mean
176.0 us, max 3.34 ms, total 1.016 s; UpdatePhysicsObjectsJob 1196 working
runs, mean 7.6 us, 9 ms total -- negligible; Unnamed\_BA0 2395 x 0.5 us;
PrePhysicsAdvanceJob still unhooked, CurveJob still never fires. The 51 us/call
with the detailed observer (205251) and 55.1 us/call without come from
different scene mixes; they do not establish that observer tax is within noise
or validate the earlier \~7 ms/frame as removable engine cost. The job timer
also excludes the preceding ownership observation. Per-FRAME normalization
still needs a parked-at-settlement window: this flight averaged only \~3 job-0
calls/frame because settlement scale (2573 eligible) arrived in the last \~20
s. Note for readers: jobs\[2].calls counts runs whose body took >= 1 QPC tick;
phys\_queue.runs (8124) counts dispatches -- the 6.8x gap is the elapsed>0
gate, not a probe defect.

**L4, write volume measured:** 15174 node appends and 1196 timed working runs
give a ratio of \~12.7; dispatches total 8124, peak append count 133. The timed
subset and the queue counter are different denominators. The zero reset/anomaly
counter is consistent with draining but does not turn appends/run into
appends/frame. These mixed-flight totals do not establish "tens of writes per
settlement frame"; use matched-window append and frame deltas before sizing
decimation. The measured total cost remains small.

### 2026-09-20 20:05 -- L2 gate RESOLVED offline: what decides a record dispatches

Zero flights, zero new Ghidra runs: the mask region 0x431B11D-0x431B221 turned
out to live INSIDE FUN\_14431AFE0 (the rig-link function the probe already
hooks), and every piece of the dispatch path was already in analysis\decomp
from the settlement arc. Assembled end to end:

**Level 1, per rig per frame -- FUN\_14431AFE0(rig, ctx).** Gate: rig state
+0x380 == 4. If rig byte +0x3D1 set, walk the render records (stride 0x6A0,
count *(ctx+0x1A940), base ctx+0x40); per record require context match
(FUN\_142840840 vs rig+0x350, byte rig+0x3D2), the
+0x68C-bit-5/dependency-token condition (+0x188 == -1 && rig+0x190), the
per-record predicate FUN\_14432C520, and (if DAT\_145ea3479) three further
predicates plus the rig+0x46C float gate. Each passing record ORs its mask word
render-record+0x570 into the accumulator. Then the 0x431B11D region proper:
clear the per-class suppression bits (*(ctx+0x1A950/58/60), selected by
collection+0x70 bit 7) from the accumulator. **Accumulator == 0 or
collection+0x70 bit 0 clear => the whole collection is skipped**
(collection+0x629 = 0, count-clear via FUN\_14434DD50 / FUN\_1442B5760);
nonzero => dispatch UpdateRenderDataJob (FUN\_144321940) or its worker-slot
equivalent with the mask in the descriptor (local\_50).

**Level 2, per collection -- FUN\_144320340 (RenderDataBatch).** The LOD
evaluator FUN\_144331300 fills an output whose active mask at +0x20 must be
nonzero, else the collection takes the empty path. Its return value is the
output pointer, not a changed flag. Nonzero clears/rekeys the primary bucket,
runs traversal FUN\_144312040, and enters secondary count-clear/build in the
same invocation. This does not establish when the upstream scheduler invokes
it.

**Level 3, per record (stride 0x2F0).** Dispatch requires: pose ctx rec+0x290
!= 0, byte rec+0x234 != 0, byte rec+0x298 != 0 (the probe's count298 -- the
draw-distance nibble), and at least one pending bit in rec+0x208 whose 4-bit
entry in the rec+0x210 nibble table (lane bit>>4, nibble (bit&0xF)\*4) is <=
*(ushort*)(node+0x6A), node = rec+0x18. Passing records call
FUN\_1442B4420(poseCtx, ctx, traversalMask, nibbleTable) with the predicate
pointer rec+0x2C0.

**Level 4, draw-item build -- FUN\_1442B4420.** Gated on DAT\_145ea3398. Per
render record of the context: require (rec+0x570 & passedMask) != 0, survive
the frustum plane-loop FUN\_1404F4E10 (return -1 = culled), and (if
DAT\_145ea3399 && rec+0x68D bit 0) a further visibility test on rec+0x688.
Visible records OR +0x570 into the visible mask; per set bit, a table at
ctx+0x1A840 (uint per bit) maps bit -> render-record index, FUN\_1442B3FC0
tests the candidate, and survivors are appended to the per-bucket draw lists
(FUN\_143696FA0 list nodes; item counter at bucket+0x2A4). These are engine
items; D3D draw attribution remains open.

**Consequence for L2 (corrected 2026-09-21).** Render-record +0x570 controls
selection, but this map does not establish a safe mutable visibility bit. The
later writer analysis identifies content construction, not an observed
per-frame write. Remaining requirements include bit/instance/pass scope, the
item-to-draw join and current visibility/restoration. Level 3's
rec+0x208/nibble test names a CPU gate; its 0x2F0 record family must not be
conflated with the 0x6A0 context records used in levels 1 and 4.

### 2026-09-20 20:25 -- +0x570 writer found: content-static, copied from the source model record

The FindStores570 scan (analysis/decomp/stores\_570.txt; 19.6M instructions,
132 writer functions) stores to render-record +0x570 by **MOV only in the scan
-- no direct OR/AND stores found**. This is evidence about the located writers,
not exclusion of indirect/aliased stores or bulk copies. The examined writers
are bookkeeping: FUN\_1442BE1F0 and FUN\_1443DE430 are constructors (+0x570
zero-init; the flag688 tag on the second was a vtable-install coincidence at
index 0xd1), FUN\_1442BF9D0 is a move/transfer (dst = src, src = 0), and
FUN\_1442D46C0 is a std::vector capacity field at +0x570 (offset collision).

Pivot: the render-record COUNT field ctx+0x1A940 has exactly two writers
(FindStores1A940; analysis/decomp/stores\_1a940.txt) -- FUN\_1401E4FD0 and
FUN\_142819D90. FUN\_142819D90 calls FUN\_142819F70, the only +0x570 writer
also touching the companion fields +0x688 / +0x68C at the 0x6a0 record stride.
Decompiles: analysis/decomp/decomp\_2819F70.txt, decomp\_2819D90.txt.

**FUN\_142819F70 is the render-record builder.** It copies the mask verbatim:
source model record +0x560 -> render record +0x570. The same builder copies
source +0x558 -> +0x688 (the flag word), assembles +0x68C from source +0x56C
bits, copies source +0x568 -> +0x578 (the bit -> record-index value used by the
ctx+0x1A840 table), and ORs entry-list masks into +0x580 / +0x588+idx.
FUN\_142819D90 builds the ctx (called twice from FUN\_1401EA920, a content/load
path, not per frame): count at ctx+0x1A940 = (srcEnd - srcBegin) / 0x570 capped
at 0x40 (<= 64 records per ctx); source records are stride 0x570; it also
builds the bit -> record-index table at ctx+0x1A840 and the exclusion
accumulators ctx+0x1A948 (all records' masks) / +0x1A950 / +0x1A958,
partitioned by rec+0x688 & 0x7FF0 / & 0xFF0 -- the exact words FUN\_14431AFE0
subtracts per class.

**No per-frame +0x570 writer found in the inspected paths.** Content
construction is established; persistent lifetime, safe mutation and
re-assertion on reconstruction still require ownership/generation proof.
Bit/pass scope, census attribution and visibility/restoration also remain open.
New artifacts: analysis/ghidra\_scripts/ FindStores570.java,
FindStores1A940.java, run\_ghidra\_stores570.bat, run\_ghidra\_stores1a940.bat;
decomp outputs under analysis/decomp/ (gitignored, on disk only).

### 2026-09-20 21:45 -- +0x560 writer found: the mask is born on the model and copied down whole

The L2 bit-semantics scan (FindStores560 + FindStores8E0, new scripts in
analysis/ghidra\_scripts; outputs stores\_560.txt / stores\_8e0.txt) closes the
content pipeline end to end:

**Source-record +0x560 writer = FUN\_14280F800, the source-record builder**
(decomp\_280F800.txt). It appends stride-0x570 source records (count at
param\_1+0x15C00) and fills the new record from the model object: +0x560 <-
model+0x8E0 (the mask), +0x558 <- model+0x18 (flag word), +0x568 <- model+0x8E8
(bit->index), +0x56C <- the bits that become render +0x68C, plus the
+0x2F0/+0x2F8 pair, +0x300..+0x31C dwords and the two 16-byte arrays the
render-record builder re-copies. Caller chain closed: the big content-load
routine FUN\_1401EA920 (the ctx builder's caller, 20:25 entry) populates its
stack arrays via FUN\_140200E20, which calls FUN\_14280F800 -- content load ->
source records -> ctx build -> render records, one connected pipeline.

**The other +0x560 writers are bookkeeping.** FUN\_143AC3E60 (the other all-tag
candidate) zero-inits +0x560 and sets +0x558=1 (ctor); FUN\_1442BE1F0 /
FUN\_1443DE430 are the known ctors. MOV-only at every level in the inspected
chain: no direct OR/AND store to source+0x560 or model+0x8E0 was found by these
scans. This establishes the copy chain, not a universal absence of indirect or
bulk mutation.

**Bit semantics live one hop up: model+0x8E0.** 48 writer functions, 30 tagged
with the 0x8E8 sibling; byte/word/dword stores are width collisions (9
dropped). The qword writers are ctor-family (the same
FUN\_140869EC0/FUN\_140529550 sub-object helpers as the record ctor). One,
FUN\_14331C0C0, stores a constant DEFAULT of 0x3C (bits 2-5) -- first semantic
hint: bits number draw-list slots, with a standard four-slot default set and
bits 0-1 reserved for something special (unproven; labelled speculation). Named
next hop, not flown/needed yet: decompile the model+0x8E0 writers (list in
stores\_8e0.txt) to determine bit ownership and pass scope. Whole-word
suppression also needs that scope: an eye-pass query cannot certify shadow or
other-view visibility.

**L2 buildability status (corrected 2026-09-21):** writer and copy chain
identified. Census attribution, instance/pass scope, scheduling and
visibility/restoration remain open; see the independent review below.

### 2026-09-21 06:37 -- census-join instrument landed (bucket item counter)

The engine bucket item counter (+0x2A4) is built and installed in the existing
kinematic-eval hook set. It measures production volume. Review correction:
aggregate deltas alone cannot answer which engine items become which D3D11
calls; that needs consumer and pass/eye provenance.

**Engine truth, from decomp\_42B4420.txt (no flight spent).** The per-rig
draw-item builder FUN\_1442B4420 walks its owner's instance entries -- count
u64 @ rigOwner+0x48, array @ +0x50, stride 0x58 (the decompile's
param\_1+0x12/+0x14 are float\*-ELEMENT offsets; the byte offsets are
0x48/0x50, resolving the overlapping-fields paradox). Each entry's model
(entry+0x0) names its bucket: bucket = \*(model+0x20), list head bucket+0x260,
item counter bucket+0x2A4. Items are 0x150 bytes, eight per pool block (pool
DAT\_145efdd30), with a parallel per-item mask array after the block's items.
The FindBucketOps scan (new analysis script; output
analysis/decomp/bucket\_ops.txt) found 136 functions touching +0x2A4 but only
FOUR that also reference the block pool: the two producers inside the bracket
(FUN\_1442B4420 inline and its helper FUN\_1442B4130), FUN\_14369c9c0 (a second
producer family, callers unknown -- caveat below) and FUN\_14431305c (a
dispatch-side fragment that also INCs). Resets exist (FUN\_14434CD90 /
FUN\_14434DB30 zero the counter). Their existence does not establish a
per-frame drain cadence; entry/exit deltas measure net production inside the
bracket. Correction to a line in the 21:45 entry's wake: the local\_478 path in
FUN\_1442B4420 is NOT dead -- FUN\_1442B2730 can set it through a pointer (the
binary's call list shows CALL 0x1442b4130 at 1442b4843); both append paths run
inside the forwarded call, so the bracket covers both either way.

**The instrument.** kinematic\_eval\_hook.cpp gains a sixth-param callback on
RVA 0x42B4420 (param\_6's 16 bytes are copied into every item, so the full
stack layout must pass through verbatim -- a four-param forward would have
corrupted items). The bracket walks the entry array at call entry (deduped, cap
64, \_\_try discipline from the phys-queue capture), reads each bucket's +0x2A4
before and after the forwarded call, and accumulates per-session into the
probe: calls / items / empty\_calls / entry\_wild / exit\_fault / neg\_deltas /
overflow\_calls / max\_buckets / max\_items\_per\_call, serialized as
kinematicEval.bucket\_items in the classification JSON. Per-session, never
active()-gated -- tracker-only flights harvest it. Prologue verified hookable
from the exe bytes (mov r11,rsp; pushes; sub rsp,0x4D8 -- no thunk). Gates:
kinematic\_probe\_test 38/0 (new case 8 drives every counter path), kinematic
json self-test extended and green, config contract 253/253. Frontier install
d3d11 sha256:16 01f727ef8fe1e808 (build predates its commit; hash is truth).

**Original flight protocol (completed; revised below).** One settlement eye
burst on this build; read classification\_\*.json ->
kinematicEval.bucket\_items. Dead-instrument discriminators, named before
flying: calls == 0 while draws proceed requires checking CodeHook installation
and scene/path coverage. calls > 0 but empty\_calls == calls requires
distinguishing legitimate empty production from bad walk offsets; neither is
success by itself. The original expected shape was calls \~ rig count per frame
(\~1.5k), items per call in the tens, and a session items-total that pairs
against the census's \~23k D3D11 calls.

**Caveat for the read.** FUN\_14369c9c0 / FUN\_14431305c / FUN\_14434D120 INC
the same counter pattern on what may be other passes' lists (shadow etc.). A
gap between produced items and D3D draws could reflect other producers, reuse,
fanout or non-bucket sources. The counters alone do not distinguish them or
establish a suppression join.

### 2026-09-21 06:55 -- flight 064511: instrument live; production is churn, not volume

First bucket-counter flight read (classification\_064511.json; flown DLL
verified by hash 01f727ef8fe1e808 -- the --expect-build label mismatch is the
known pre-commit label, same as the node-capture build). Session: frames
0..12,759 at dump (log 06:43:19 -> 06:45:11); the settlement scene (depth-probe
25,885 draws/frame at 06:45:07; DC census 55,058 draws over the 3 capture
frames) covers roughly the last 2-3k frames.

bucket\_items: calls 462,089; items 6,889,852; empty\_calls 48,960 (10.6%, no
bucket discovered by the entry walk); entry\_wild / exit\_fault / neg\_deltas /
overflow\_calls ALL ZERO -- no reported walk/read faults, negative net changes
or overflow. This does not exclude a drain followed by refill inside a bracket.
max\_buckets = 1 across 462k calls: each observed entry walk found at most ONE
bucket; the dedup path never fired in anger. max\_items\_per\_call = 1,286 (one
big rig build -- settlement arrival).

**Production/draw discrepancy, not a completed join.** Lifetime averages: 36
builder calls/frame, 540 items/frame. Windowed to settlement-only (\~2.5k
frames, the most favorable reading): \~185 calls and \~2.8k items per frame --
still \~10x below the 23-26k D3D11 draws/frame. The instrument's item
production is well below total draw volume. The settlement-only figures assign
the entire session's numerator to an estimated window; they are favorable
bounds, not actual windowed measurements. Possible explanations include other
producers, repeated consumption, one-to-many item submission and non-bucket
paths. They do not prove list retention or a dirty rebuild schedule. A per-item
mask reader, if found, would also not by itself prove that changing a source
record updates the mask already copied into an item. No negative net deltas
does not exclude interleaved drain/refill.

**Ruled out, do not re-propose:** reading per-frame bucket production as the
total drawn volume -- this instrument cannot be equated 1:1 with all D3D
submissions (540 vs 23,000).

**Named next question (offline, no flight):** identify the bucket-list
consumer. The FindBucketOps 0x434Dxxx cluster is the prime suspect --
FUN\_14434D790 (CMP +0x2A4,0: empty-bucket gate), FUN\_14434CD90 /
FUN\_14434DB30 (zero the counter: the resets), FUN\_1436EEFF0 (reads +0x2A4
into a register: loop bound?), FUN\_14434E28F (four +0x2A4 hits, refs260).
Decompile the cluster and its callers; check for a consumption-time re-test of
the per-item mask array and trace it to actual submission. Scope, lifetime and
safe visibility/restoration remain separate unknowns before a build decision.

### 2026-09-21 07:29 -- producer map resolved; instrument extended to all three producers

The consumer hunt (06:55's named question) resolved into a producer map instead
-- all offline, no flight spent.

**The +0x2A4 cluster is list maintenance, not submission.** FUN\_14434CD90
clears a bucket (counter zeroed at REBUILD START; callers include a destructor
FUN\_14430DA30 and rebuild entry FUN\_1442B3E00). FUN\_14434D790
merges/compacts item spans with their mask arrays. FUN\_14434E28F merges
worker-local buckets into shared ones, keyed on +0x2A0, under a critical
section -- parallel build, single merge. FUN\_1436EEFF0's +0x2A4 was a matrix
float (struct collision, ruled out). The mask-array reader hunt
(FindMaskReaders scan, 62 functions touch +0xAA0) found only struct-family
collisions (float/vector/pointer slots), without locating a submission-time
mask test. Indirect consumers and mask semantics remain unresolved. Whole-word
suppression still needs scope and ownership proof.

**Three producers feed the same bucket structure.** FUN\_1442B4420 (hooked
06:37; the per-record gate path), FUN\_144312E00 (bucket = param\_2; called via
FUN\_144312040 from the SAME per-collection batch FUN\_144320340, building a
second item class; the "FUN\_14431305C fragment" is inside it -- two producers,
not four) and FUN\_14369C9C0 (bucket = param\_2; FUN\_1436A0F50's four call
sites). Assigning the third producer to particular non-eye targets remains a
hypothesis. FUN\_144320340 runs the LOD evaluator FUN\_144331300 and builds
items for records passing current selection gates. It does not compare old/new
LOD masks or establish an incremental/dirty schedule. The 36 calls/frame is an
all-session mean, not a measured collection cadence.

**Join hypothesis (still open).** Additional producers and eye/pass fanout
could help explain the count gap, so their production was instrumented. The
\~2.8k figure is a favorable window bound, not measured settlement-only
production. Fitting a multiplicity to the draw total cannot prove attribution;
consumer provenance and actual window deltas are required.

**The extension (installed, d3d11 sha256:16 99885e3a092df662).** Two direct
brackets read param\_2+0x2A4 entry/exit on FUN\_144312E00 (RVA 0x4312E00) and
FUN\_14369C9C0 (RVA 0x369C9C0), both prologue-verified; serialized as
kinematicEval.bucket\_items\_direct (per producer: calls / items / neg\_deltas
/ read\_faults / max\_items\_per\_call). Same per-session, never-gated
discipline. Gates: probe test 41/0 (new case 9), json self-test extended,
config 253/253.

**Original flight #2 protocol (completed; limitations corrected).** One
settlement eye burst on this build. Compare bucket\_items +
bucket\_items\_direct against the per-pass depth-probe census (#2/#5 = the eye
pair \~9k each, #1 256x256 \~3.8k, #7 3072x1024 \~3.4k). Direct calls == 0
requires checking installation and scene/path coverage; calls > 0 with items ==
0 requires distinguishing legitimate zero appends from incorrect layout.
Neither raw totals nor production times an assumed multiplicity proves draw
provenance, complete coverage or suppressibility.

### 2026-09-21 07:45 -- flight 073348: producer coverage; original GO superseded by review

Flight #2 read (classification_073348.json; flown DLL verified by hash
99885e3a092df662). Session 11,741 frames, almost all menu/hangar/space (depth
probe 32-34 draws/frame until 07:33:51, AFTER the dump) -- so this flight
measures the producers, not the settlement join. All three CodeHook install
lines present in the log: every hook hooked, including 14369c9c0.

Producers: FUN_1442B4420 -- 252,659 calls, 3,824,820 items, zero faults (mean
refill 15.1 items/call, max 1,273). FUN_144312E00 -- 1,081,188 calls (92/frame)
but a trickle: 430,886 items, max 3 per call (singleton slot items, the second
item class). FUN_14369C9C0 -- ZERO calls in 11.7k frames with the hook
confirmed installed: it is not an observed producer in this captured scene
window. This cannot assign the later settlement non-eye passes or establish the
eye pair's producer. The earlier eye pair (18k of 21.9k draws = 82%) remains a
draw-census figure, not a measured bucket-list share.

**Correction to the original interpretation.** The clear routine does reset
bucket+0x2A4 and clean the list, but its caller tests a freshly computed active
mask, not a changed mask. Neither retained cross-frame lists nor a forced
enqueue follows from that code. The original GO, 40-frame cadence, completed
census join and claimed one-frame restore are withdrawn. The absence of a
direct per-item mask reader in a scan is also not a proof against indirect or
bulk consumption. The independent review below gives the replacement decision.

### 2026-09-21 -- independent disassembly review and corrected L2 decision

Reviewed main through 9048b2d, including the new handoff, the saved decompiles
and producer-counter implementation. Critical control flow was checked directly
with MSVC dumpbin against `analysis/EliteDangerous64.exe`, SHA-256
`E6BE8BBE04E6A7AE226D4318945AF7F367DE13DC5A007A261964D9BA8144E988`. Addresses
below are VAs at image base 0x140000000 for that binary; do not treat them as a
cross-version hook contract. This was an offline review, not a new flight or
performance result.

**1. Current selection is not change detection.** FUN_144331300 zeroes its
six-qword output at 0x14433134A-0x144331359, walks context records at stride
0x6A0, tests descriptor+0x48 against record+0x570 at 0x14433148B-0x144331497,
computes LOD lanes, and ORs the selected bits into output+0x20 at 0x144331584.
There is no old/new output comparison. The entry field-mismatch check can call
FUN_14433C870; it does not make the returned active mask a changed mask.

In FUN_144321940, 0x144321965 calls the evaluator; 0x14432196A reads
output+0x20; 0x144321973 tests it. Nonzero reaches FUN_14434DB60 at 0x14432199D
and traversal FUN_144312040 at 0x1443219BB, followed by secondary clear/build
in that same job. FUN_144320340 has the corresponding batch path. FUN_14434DB60
itself only writes the key (0x14434DB60), zeroes count (0x14434DB6D) and
tail-calls list cleanup (0x14434DB7B). Direct clearing does not request another
job and is not a safe asynchronous force-rebuild API. Upstream cadence and
serialization need tracing.

ruled out: interpreting FUN_144331300's active mask as changed bits, because it
constructs the output from zero. Ruled out: treating FUN_14434DB60 itself as an
enqueue/dirty operation, because the instructions only clear/rekey storage.

**2. Ownership and granularity are missing links.** The +0x570 records live at
ctx+0x40, stride 0x6A0, count ctx+0x1A940. The separate collection array is at
collection+0x280 with count +0x298 and stride 0x2F0; its +0x290 pointer reaches
the builder's owner. The primary bucket is collection+0x300; collection+0x2E0
names the secondary owner. The known forward links do not supply a unique
context-record -> collection/bucket backlink or prove contexts are unshared.

FUN_1442B4420 maps set bits through ctx+0x1A840, combines record masks, merges
them across LOD slots, and stores an aggregate mask with each appended item
(decomp_42B4420.txt, lines 505-577 and 662-735). Neither one item per record
nor one D3D draw per item/eye is established. FUN_142819D90 also caches
aggregate masks at ctx+0x1A948/950/958/960; FUN_14431AFE0 uses the class
exclusions. Changing +0x570 alone leaves these caches untouched. Bit aliasing,
pass scope, context fanout and exact original-value restoration must be
understood first.

**3. The clear key gates merges, not a proven refresh request.** FUN_142819D90
writes ctx+0x1A968 from DAT_145F27DB4 and ctx+0x1A96C from constructor argument
4. FUN_1401EA920 supplies the latter from its caller object's param_1[0xF30]
low 32 bits (dword), or zero, to both contexts. FUN_14434E28F compares
source/destination bucket+0x2A0 before merging; a mismatch skips the merge.
This is merge compatibility evidence, not proof that a particular key schedules
refresh. The exact FUN_1442B5670 body is not present as a standalone saved
decompile; nested-key propagation remains open.

**4. Counts do not close the census or cadence.** The hooks measure aggregate
entry/exit +0x2A4 deltas. They do not record consumption, frame, pass/eye or
D3D draw identity. Flight 064511 covers settlement volume but only the first
producer; 073348 covers the three hooks but not the same settlement window. The
82% eye figure cannot be assigned to these lists from that combination.
Likewise 1500/36 = 41.7 mixes an assumed rig population with an all-session
call rate. The earlier favorable settlement-window rate is 185, yielding 8.1
under the same assumptions; neither quotient measures a per-rig interval. Count
unique objects and recurrence in one actual window instead.

**5. Historical zero samples are insufficient for suppression.** The settlement
doc's 2026-09-18 targeted flight recorded three zero-to-nonzero transitions
despite identical captured bindings, IDs and t33 payload. Hidden NPCs and a
moving drone are also reasons to validate changing occluders; static geometry
does not make camera/depth visibility static. The builder already runs frustum
and optional visibility gates (decomp_42B4420.txt, lines 250-274), and eval
runs distance/LOD and frustum gates (decomp_430EFE0.txt). A family-level zero
fraction does not certify a particular engine record or its other passes.
Suppression also removes the original draw's query, so retest/restore cannot
depend on that draw becoming nonzero while absent. A conservative independent
visibility test or explicit invalidation/restoration path must precede
mutation.

**Next work, bundled to avoid wasted flights.** First trace the consumer and
upstream enqueue offline. Then one bounded read-only capture should correlate
frame/thread/caller, rig, ctx, collection, bucket, keys, input/active masks,
append/item provenance and actual D3D pass/eye consumption. Include context
fanout and unique collection recurrence, and a matched settlement CPU window.
Unknown mappings, missing hooks, capacity overflow and incomplete consumption
coverage must be explicit outcomes. Define the overhead budget and expected
counter signatures before installing. Only then choose a suppression scope and
visibility/restoration design; prefer investigating a per-job scoped mask over
mutating shared content or externally clearing live lists. No suppression
build, new flight, FPS gain or one-frame restoration is claimed by this review.


### 2026-09-21 -- offline consumer/scheduling trace closed; bucket suppression refuted as scoped

Method: Ghidra headless `-noanalysis` against the shared project (no analysis
running; scripts preserved additively). 21 decompiles via DecompileTargets
(now 21 RVAs), new xref scan FindBucketLinks (`vtable_key_refs.txt`), new
offset scans FindDirtyFlagRefs/FindBucketStructUsers (`dirty_flag_refs.txt`,
`bucket_struct_offsets.txt`). All addresses VA at image base 0x140000000 of
EliteDangerous64.exe SHA-256 E6BE8BBE…988; not a hook contract. This closed
the handoff's step 1 (consumer + scheduling trace) and thereby steps 2–4 as
scoped: the capture spec, suppression decision and optimization design all
collapse once the mechanism is known.

**The lifecycle, end to end (evidence: decomp_*.txt cited per step).**

1. Create: factories FUN_1442cd610/0x1442cd680 (gated by globals
   DAT_146034260/DAT_146034350, pools DAT_1460341a0/DAT_146034290) construct
   the worker via FUN_1442bf530/0x1442bf770, which install the
   multiple-inheritance Table C vtables (0x145591xxx; decomp_42BF530/42BF770).
   Factory callers are callback stubs inside Table A (0x1462b2234/0x1462b2240).
   Bucket init FUN_14434ca90 zeroes key qword +0x2A0, sets +0x2A8, flag
   +0x2B0=0 (decomp_434CA90).
2. Rekey/reset, three paths: (a) worker entry FUN_144321940/FUN_144320340 →
   FUN_1442b5670(owner, ctx) = 15-byte wrapper
   `FUN_14434db60(owner+0x78, *(uint32_t*)(ctx+0x1A968))` (decomp_42B5670);
   (b) batch repopulate FUN_1436a0f50: gates on a combined active mask built
   from ctx records (+0x570 ANDed with interface masks, +0x580 ORed, +0x690
   liveness), then clears the +0x88 bucket (FUN_14434dd50 + FUN_14434db60 with
   key = DAT_145f27db4) and re-appends via FUN_14369c9c0 (decomp_36A0F50);
   (c) FUN_1401ea920 → FUN_14434db30: flag +0x2B0=1, count=0, rekey to
   DAT_145f27db4, then FUN_14434e1b0 drains pending items and re-clears the
   flag (decomp_434DB30/434E1B0). Merge/absorb FUN_14434e20e and FUN_14434e28f
   run under a critical section and merge sibling buckets only when +0x2A0
   keys are equal AND +0x2A4 counts are nonzero (decomp_434E20E/434E28F).
3. Key semantics: bucket+0x2A0 is a copy of ctx+0x1A968, itself a copy of
   DAT_145F27DB4. The global has exactly one writer, FUN_1405d6260, whose
   entire body is `DAT_145f27db4++; AK::MemoryMgr::GetCategoryStats(new_value,
   …)` — an unconditional increment feeding Wwise memory-category stats, sole
   caller FUN_1407f32d0 (decomp_05D6260, vtable_key_refs.txt). The merge key
   is an unrelated audio counter sampled at rekey time.
4. Produce: four producers append (0x150-byte record, u64 key) pairs to batch
   nodes of eight from pool DAT_145efdd30, with a parallel key array at
   node+0x154: FUN_1442b4420, FUN_144312e00, FUN_14369c9c0, and
   FUN_14434d120 via FUN_14434d470 — the last builds its record as a
   transform-derived packed 4-vector through SIMD plane-select/normalize/int-
   pack before appending (decomp_434D120/434D470). A u64 "key" whose bits are
   consumed per-bit (below) plus transform-derived vector payloads is
   consistent with light/probe-style batching; the mesh-draw reading of this
   subsystem is not supported by any decompile.
5. Drain: FUN_1442df940 walks a fixed three-record window (stride 0x308 at
   param_2+0x4f0..+0x610): notify FUN_1405d6df0, then FUN_14434d790 on the
   bucket embedded at record+0x60, then zero record+0x58 (decomp_42DF940).
   FUN_143681980 applies the same liveness gate as FUN_1436a0f50
   (+0x1C378/+0x1C3D0) and drains the same +0x88 bucket that FUN_1436a0f50
   resets — reset and drain are paired vtable methods on one owner
   (decomp_3681980).
6. Consume: the drainer hands each batch node to the shared run-length
   accumulator FUN_144c837b0 (nine callers across the binary), which RLEs
   consecutive equal u64 keys and, per run, adds run_length*n(n+1)/2 (n from
   (*obj)+0x9c) into a 64-entry int table at receiver+0xd8 per set key bit,
   then calls FUN_144c7ef70 (decomp_4C837B0). That function enqueues a
   lightweight 0x48/0x50-byte command record — offsets, pointers, count, the
   touched-bits mask, a sequence number — into one of four ring-buffer arrays
   chosen by a 3-bit flag; it never copies item records and makes no virtual
   calls (decomp_4C7EF70). Item payload reaches the queue only by pointer.

**Why suppression is refuted as scoped.** Each ground independently fails the
handoff's requirements (stable identity, pass/eye scope, conservative
visibility, no side effects, serialized mutation, restoration):

- Key control is impossible: the key is an incidental Wwise counter, not a
  render generation; there is no dirty surface in this subsystem at all.
- Items are not draws: consumption is per-bit weighted counting plus
  pointer/offset command enqueue, so "one record -> one item -> one D3D draw
  per eye" is false at the mechanism level and the census 82% eye share has
  no route from these lists. The producer counters were never measuring
  submitted draw volume.
- The +0x2B0 dirty flag is set/cleared only inside FUN_1401ea920's rekey
  sequence and has no conditional reader in .text; work gating is +0x2A4
  count and +0x2A0 key equality inside the merge routines.
- The frame scheduler — the cadence owner — invokes everything through
  vtable-table callbacks (Tables A/B); no named .text caller exists and a
  positional scan cannot see through the tables. Runtime stack capture is
  the cheap way to name it, if it ever matters.
- One producer's payload is a packed transform vector; the subsystem may not
  be mesh draws in the first place, so "suppress unnecessary draws" was
  likely aimed at the wrong object kind.

ruled out: merge-key or bucket-clear based scheduling/control, because the
key is an audio counter and clearing schedules nothing. ruled out: the
82% eye attribution and the 40-frame rebuild cadence, because no evidence
route connects bucket items to eye-pass draws and the scheduler is unnamed.
ruled out: +0x2B0 as an enqueue trigger readable from .text.

**Next (only if the question stays worth a flight).** One bounded read-only
capture: enqueue counters at FUN_144c7ef70 (ring identity, touched mask,
count, sequence) plus return-address stacks at FUN_144321940/0x144320340/
0x1442df940/0x1436a0f50 to name the scheduler, correlated with the existing
eye census to identify what the command rings drive. No bucket mutation, no
clearing, no suppression build is justified by current evidence.


### 2026-09-21 -- boundary-first attribution: the settlement draws are one scenery family

Prompted by the question of whether the high settlement draw counts had ever
been traced backwards from the d3d11 boundary (they had not — the closed
trace ran forwards from the engine's bucket producers, and its items never
reach the boundary as draws). The armed-census logs of the three settlement
flights (205251, 205106, 064511, 073348; builds v0.17.0-64..-188, game build
332841) were tallied per VS content hash. `vh=`/`ph=` hashes are
cross-session-stable; identification via the named families in
src/d3d11/eye_draw_snapshot.h and doc names.

**Representative frame (064511, frame 0; 3020 retained eye lines).**
Caveat first: the 16384-line census cap is eaten by ~15k DCC copy lines on
frame 0, so retained draws are eye ordinals #1-#3020 of 18357 — the first
16% of the eye pass. The distribution below is therefore a biased sample;
it was stable across three independent flights (EB share 78.2/80.9/80.5%),
which bounds the bias.

| vh hash | share | avg n x i | identity |
|---|---|---|---|
| EB5234DB6ADB491D | 78.2% | 282 x 4.9 | sourceMesh settlement scenery family (named in eye_draw_snapshot.h; the settlement-flicker zero-sample family) |
| 8056C9D5F22007F9 | 9.3% | 413 x 6.7 | UNKNOWN — no DXBC on disk anywhere |
| 2684F02B9B0BB0DE | 3.4% | 36 x 5.1 | UNKNOWN — no DXBC on disk |
| ACE405F428C17EF6 | 3.3% | 88,994 x 1 | large terrain chunks (watches family) |
| F516BF0201303B87 | 2.4% | 221 x 1 | sourceMesh |
| DE545DC8EE4FBB87 | 0.8% | 84 x 2.9 | sourceMesh |
| rest | ~2% | | sky dome, solar, rifle glow, panels, tails |

Per-frame totals from the DC frame summary lines: eye ~18.3k, offscreen
~4.1k, copies ~1.6k (frames 1-2; frame 0's 15k copies are a one-time flood),
dispatches ~105. The eye share of draws+offscreen is 81.8% — the origin of
the "82%" figure. Conclusion: the settlement eye pass is dominated by
thousands of small instanced batches of ONE scenery material family. That
is the "more detail than is strictly necessary" candidate, and it is
measurable from the boundary without any engine mutation.

**Game settings inventory (this machine).** No model-quality key exists by
that name; the game's GraphicsConfiguration.xml defines MaterialQuality
(internal MaterialQualityLevel 0-3), LODsToDrop, LODDistanceScale,
SurfaceMaterialQuality, EnvironmentQuality, TerrainQuality,
TerrainLodBlending. Active profile Custom.4.4.fxcfg (mtime 2026-09-20, in
the flight window): MaterialQuality=3, LODDistanceScale=1.0,
SurfaceMaterialQuality=2, TerrainQuality=3, EnvironmentQuality=0 (already
minimum), DirectionalShadowQuality=2, SpotShadowQuality=2,
HMDRenderTargetMultiplier=0.65. GraphicsConfigurationOverride.xml pins
GalaxyMap/Planets/Envmap only. So the material/LOD headroom is real:
MaterialQuality and LODDistanceScale are at maximum while EnvironmentQuality
is already at minimum — the A/B changes one cluster at a time and the
current config is the high side.

**A/B flight protocol (no build needed; census is hotkey-armed today).**
Game closed, edit Custom.4.4.fxcfg: MaterialQuality=0,
SurfaceMaterialQuality=0, LODDistanceScale=0.1 (keep shadows, terrain, and
EnvironmentQuality untouched to isolate geometry). Relaunch, stand at the
same settlement spot and heading as an A-pass reference (use flight 064511's
position or re-capture pass A first in the same session), arm the census
both passes, 3 frames each. Compare per-frame DC summaries and the vh=
tally: EB-family count, eye draws, unknown-family shares. Expected
signatures: (a) EB count collapses — the sliders gate settlement
source-mesh instance selection; the recoverable fraction is measured, not
assumed; (b) EB count unchanged — settlement scenery ignores these knobs
and draws unconditionally, in which case no settings-side lever exists and
any further win is EDVR-side boundary work; (c) partial — bisect
MaterialQuality vs LODDistanceScale in one further pass. Optional: arm
EyeDrawSnapshot in pass A so the two unknown hashes get named via
tools/dxbc_disasm.py.

**Tool gap (recorded, not yet built):** the per-vh tally needed read-only
python one-liners because tools/edvr_log.py has no aggregation mode; a
`--tally <field>` mode would close it. Also noted: census truncation makes
frame 0 samples copy-flooded — tally frames 1+ in future analyses.
