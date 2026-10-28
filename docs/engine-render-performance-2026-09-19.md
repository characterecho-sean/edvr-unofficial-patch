# Engine render performance: a design space

## Status

* **Removed 2026-10-08 (branch claude/remove-settlement-detail):** the
  LOD governor (`fix.settlement_detail`, `advanced.settlement_detail_max`
  and `_observe`) and the static prop gate (`fix.static_prop_updates`),
  with code and rigs. The entries below are their history.

* **State, 2026-09-23 (scope: cockpit, stereo only).** The settlement
  CPU wall is the caller thread's own draw submission, not the job
  pipeline: the pipeline's critical-path share is 0.78-0.81 ms against
  a 1.5 ms bar ("Phase A, valid: KILL" — the occlusion-culling arc is
  KILLED). EDVR's own pre-submit leaf cost is cut 3.94 -> 1.19 ms over
  three measured rounds (-70%); the caller thread is down 15.1 -> 11.9
  ms, 50-52 fps at leg C; GPU is 9.2-10.2 ms at 0.566 ("EDVR's per-draw
  path named", "Per-draw cut, round three", "Leg C", "The GPU side
  of the same parked leg"). The re-scoped occlusion cull FAILED Phase
  B' on evidence — the hiding surface is 89.6% open panels, qualified
  solid occluders remove zero draws — and no cull code was built
  ("Gate probe flown", "Probe v2 flown"). The settlement LOD governor
  flew eight times (2 shadow, 6 acting; "Shadow flight 1" to
  "Refinement 4b flown at the pad": at the ceiling 25-34% of cycles
  still took two slots, the wall being the frame's TAIL) and was then
  removed (the bullet above).

* **Levers still open:** the draw count (its governor lever is gone);
  the last ~0.4-0.6 ms of EDVR's own per-draw
  path — the always-on instruments armed-only, plus a fourth round on
  the resource hooks ("Leg C").

* **Open items (pointers; open the named entry for the evidence):**
  settlement admission (C2) untested from a real arrival ("The ~1 km
  onset"); eye depth capture is native-VR-only by design ("Phase A
  verdict: KILL"); the object classification ownership join,
  unwind_failed ~90% in all four Phase A runs (same entry);
  depth-capture constants keying, a structure-family draw ("The
  visibility prize, measured"); the per-record identity/change signal
  (motion arc); ring-buffer command consumers, the scheduler still
  unnamed ("offline consumer/scheduling trace closed"); leg A at LOD
  1.0 / MaterialQuality 3 unflown ("Leg C").

* **Ruled out (pointers, do not re-propose):** merge-key control of
  any kind — the key is an incidental audio counter ("offline
  consumer/scheduling trace closed"); the 82% bucket attribution and
  40-frame cadence — no evidence route to eye-pass draws (same entry);
  draw-call identity/motion estimation as a class
  (kinematic-motion-injection-2026-09-19.md); compensation-style
  tweaks (AGENTS.md diagnosis discipline); reusing prior zero samples
  from unchanged bindings/payload (settlement arc's 2026-09-18
  flight); the per-draw lever as a route to 90 Hz by itself, exhausted
  at ~2 ms bought ("Round two measured"); the record-level cull unit,
  0.5-0.9 ms even with ideal occluders ("Gate probe flown");
  closed-solid and eroded-box occluders, remove zero draws ("Probe v2
  flown"); LODDistanceScale as a draw-count lever at this view ("Leg
  C"); applicationMs as a frame-fit signal, it omits the post-submit
  phase ("Shadow flight 1").

* **Next (2026-10-27, last entry):** AA on's +1.65 ms is per-draw
  classifiers. ui-depth memo FLOWN: exact, -0.21 ms (shipped, b1561dff).
  Shadow 1c FLOWN: 29-30% would take, 0/28M wrong. 1d (bands) BUILT.

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

2026-09-23 -- carried from the Status block: this doc owns the
engine-performance arc; the settlement-flicker arc shares instruments
and flights with it.


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


### 2026-09-21 -- A/B flight (123700 / 124159): settings lever partial, dispatcher named, EB52 instance truth captured

Flown from the cockpit, landed at a settlement, native VR (the state all
prior settlement evidence came from; on-foot is the flat 5120x2880 vscreen
panel and would have invalidated the comparison). Environment: build
v0.17.0-217-g2b7a471-dirty (6AB174F9), game 332841, VirtualDesktopXR /
Meta Quest 3, 3072x3264 per eye, 90 Hz, no DLSS (EDVR temporal ran).
Identity proven by the probe hook lines, not assumed. Config restored to
the pass-A baseline after pass B (backup Custom.4.4.fxcfg.passA-bak-20260921).

**A/B verdict: (c) partial.** Per-frame DC summaries: pass A eye ~18.4k,
pass B ~14.9k (-19%). The 20-frame draw ledger (authoritative; the 16k-line
census prefix misranks the delta and says EB52 *up* — the prefix is
unrepresentative, do not tally it for A/B):

| family | A /frame | B /frame | delta |
|---|---|---|---|
| EB5234DB6ADB491D | 9,216 | 7,244 | **-21.4%** |
| 8056C9D5F22007F9 (unknown-A) | 547 | 538 | **-1.7% (immune)** |
| 2684F02B9B0BB0DE (unknown-B) | 207 | 169 | -18.4% |

The sliders gate roughly two thousand EB52 draws/frame but a ~550-draw
unnamed pool family ignores them completely. Next: bisect MaterialQuality
vs LODDistanceScale (one more paired pass); name unknown-A
(vh=8056C9D5F22007F9) with an armed watch on it — characterized here as a
kind-88 eye draw, pool=1 (t33 reader), median 198 verts, <=8 instances.

**Scheduler probe (advanced.scheduler_probe, first flight):** 20-second
reports fired for all four targets in both passes. The drain target has ONE
fixed stack (100% of 2,783/1,242 calls). The two worker entries share their
top signature: RVA 0x5D6A81 at the top frame in ~70% of signatures across
both entries and both passes — the instruction after `call qword ptr
[rax+8]` at 0x1405D6A7E, matching the unwind doc's confirmed worker-dispatch
site. The dispatch is INDIRECT through each payload's own vtable, so the
scheduler never appears as a return address: the scheduler is the payload
object's OWNER (data, not a stack frame). No captured VA falls in either
table-stub range. reset-repopulate took 0 calls in all 16 reports — the
reset path never fired during a parked minute (its cadence is not
per-minute). Worker calls run flat for minutes then explode to ~44k/s
exactly at the armed window — the worker storm under study coincides with
the census moment. Follow-up offline: decompile the dispatcher window
(0x5D6A81, 0x5D552F, 0x48D8AF, 0x4AD7BF) to name the owner loop.

**EB52 snapshot (pass A, 12:38:44 window):** both eye targets seen.
drawstate eyemesh ring kept 4,096 eye draws (27,891 dropped) of which 3,956
are EB52; layout confirmed INSTANCEANDMODELDATAINDEX (8-byte instance
stream) + PACKEDVERTEXDATA. **The per-eye instance buffers are fully
captured**: inst_123844_*.bin, 1,572,864 bytes each, 19 frames, 8-byte
records, first u32 = t33 pool record index — the EB52 per-eye truth is
readable offline, no further flight needed for it. Two incidental shaders
named from the pool dump: E508648660A352B2 = pool skinned-prop VS (t33
336-byte model records, t38 48-byte bone palette, up to 4 bone rows,
camera-pivot-subtract, projects via cb0[4..7]); D95905C18B7FAD93 =
constant-driven billboard/impostor VS (per-draw CB1 carries world
center/scale/rotation; camera-facing frame; +10 z bias; 38 quads/frame,
settings-independent). Not captured: dxbc for 8056/2684 (watch list was
EB52); the unknowns stay unnamed until an armed watch on 8056.

ruled out: "the settings sliders don't affect settlement draw count" —
~21% of the dominant family moves with them. ruled out: "the scheduler
appears on the worker-entry stack" — indirect dispatch hides it; the
dispatcher loop and payload-owner trail are the route.


### 2026-09-21 -- Step 3 (pass C, two sessions): LOD is the lever; unknowns are CB-less GPU-driven batches; scheduler named

Pass C isolated LODDistanceScale=0.1 (material knobs at baseline), flown
twice: census-only (134203) and with the eye run armed (135000). Same rig
and build as the A/B flight; probe still on; config restored to baseline
after (backup Custom.4.4.fxcfg.baseline-bak-20260921).

**Bisect verdict: LODDistanceScale is the dominant draw-count lever, with a
material-knob refinement.** Census frame 0 was the COMPLETE eye pass this
time (copies=81, no snapshot flood): EB52 9,216 -> 7,663 (-16.9%) with
LOD-only vs 7,244 (-21.4%) with all three knobs — the material quality
keys cut a further ~5.5% of EB52 on top of LOD. Totals: 18.4k -> 14,952
(LOD-only) vs 14,944 (all-knobs): LOD reproduces the total. Unknown-A
(8056C9D5F22007F9) confirmed immune for the third time (568 vs 547
baseline = jitter): it ignores LOD and material knobs alike. Unknown-B
(2684F02B9B0BB0DE) tracks LOD (-14%).

**Unknown identification: narrowed, not closed.** census_cb_watch was set
for both hashes; the watch matched (568/178 draws ran cbWatchOnDraw) but
produced zero bytes: these shaders bind NO constant buffer at b0 (every
census row c=-; PS b0 also null) — the DCW instrument has no buffer to
read. From the census rows themselves: both are indexed instanced batches,
8-byte vertex stride, triangle strips, shared VB/IB, startInstance stepping
+8 between sibling draws (GPU-driven instancing), VS SRVs at t1/t4/t6;
8056 pairs PS 669CC896CA4AA988 with an 8-texture material set; avg n~432.
Plain terms: instanced prop/rock batches fed by instance streams and SRVs,
not constants. Naming now requires the snapshot path: the 13:51 eye run
again retained zero of their draws — retention is a hardcoded hash
allow-list (eye_draw_snapshot.h sourceMesh(), no config key), so
census_cb_watch cannot influence it. Their t33 transform records ARE in
the shared pool dumps (pool_135147_*.bin, addressable via ledger
startInstance), but geometry/shader bytes were never retained. Fix built
same day: the two VS hashes (plus paired PS if required) added to the
allow-list — one more armed flight then names them.

**Scheduler (from the probe + decompile, closing the 2026-09-21 A/B
entry's follow-up):** the live dispatcher is FUN_1405d6960 (0x5D6960,
441 B): pops ONE 0x20-byte queue item (ring at queue+0x10, CAS ticket
0x1405471d0) and calls [item[0]->vtable+8] — the worker run method. The
frame-level scheduler is the pair FUN_1405d6b20 (job-table scheduler:
payload pointers at scheduler object +0xD0, count +0xE8, descriptors +0x90
stride 0x28, ring cursors +0x108/+0x110) driven by FUN_1405d6e40
(per-worker pump), object at TLS+0x4480, global manager DAT_145f27db8
(per-worker queue array +0x20). Cadence is QUEUE-EMPTY, not a frame
counter: the pump sleeps on a condition variable and producers wake it —
"per frame" is emergent. Minority stack frames were interior code pointers
(a Scaleform-render payload family), not callers. The static chain
dead-ends at runtime-built payload vtables (0x1450C7B80/0x1450C7BA0,
installed by FUN_1405d3510; registration slots 0x1460adcdc..0x1460adf4c in
runtime-only memory). Closure options: a startup write-watch on those
slots, or stack-capture on the enqueue side (FUN_1405d5040) to name the
per-frame producer. Probe timing note: the worker storm now runs
IMMEDIATELY BEFORE the armed window (flat ~2.9k calls parked, +155k in the
20 s pre-arming, census captures the drained aftermath).

ruled out: MaterialQuality/SurfaceMaterialQuality as draw-count levers
(their ~5.5% EB52 effect is secondary); census_cb_watch as an
identification channel for CB-less shaders; ring/budget settings as a fix
for missing unknown-family retention (hardcoded allow-list is the gate).


### 2026-09-21 -- Unknown families identified: the settlement pool's material layers; unknown-A is an ungated detail pass

The allow-list flight (141800 session) captured all four shaders. Identity:
**unknown-A (vs_8056C9D5F22007F9 / ps_669CC896CA4AA988)** and
**unknown-B (vs_2684F02B9B0BB0DE / ps_2376A8D9AA874372)** are the pooled
settlement pool's weathering/detail (A) and baked-lit trim (B) materials —
t33 quat+pose instancing, deferred G-buffer, A world-space projected with
a PS distance-fade and NO LOD gate, B light baked-lit and LOD-gated.
Corrections to the earlier characterization stand (the 8-byte stream is
instance data, vertex stride 40 B, skinning idle; frame constants in CB1,
instance data in SRVs). SIZING-STUDY CORRECTION (same day): A and B are
NOT the same instances drawn twice — instance overlap is only 6.3%. The
real duplication is across ALL pool draws: 75,779 submissions/frame of
9,373 unique records (8.08x); EB52 41,281 of 6,817 (6.06x), 7,951 draws
collapsing to 1,271 record-sets drawn ~6.3x each; 7,298 fully
byte-identical excess commands/frame. Repeat legitimacy (same-pass waste
vs per-eye/per-pass duplicates) is unproven — the draw ledger lacks PS/RT
per row; logging them is the named next instrument.

**Why unknown-A ignores every quality knob (resolved):** no LOD gate
exists for it. The shader has no LOD branch at all — only the PS detail
fade — while sibling layer B IS distance-culled (-18% at LOD 0.1). A's
~550 draws/frame are submitted unconditionally for every visible cluster,
and at LODDistanceScale distances its own fade makes it INVISIBLE — the
settlement pays full draw cost for a detail layer that renders nothing at
range. Optional confirmation test (not yet flown): diff the A-family
t33-record sets via ledger startInstance between LOD 1.0 and 0.1 — an
identical set alongside B's shrinking set proves the absence of any gate.

Design input (not a build order): gating A by its own fade distance —
matching B's behavior — would remove ~550 draws/frame at far LODs with
zero visual change. That is an engine-behavior change; whether EDVR
should approximate it boundary-side (suppress A-family draws beyond the
fade) is a product decision with the usual suppression-discipline
requirements (visibility proof, restoration path) that the 2026-09-21
trace entry records as still mandatory.

Cost model prior (Sean's earlier testing, stock Elite included):
settlements are CPU-bound — interpret draw-count deltas as CPU submission
cost until the frame-time mining of the A/B/C passes proves otherwise;
GPU-side levers (DLSS/render-scale) do not address the settlement wall.

Cost baseline (2026-09-21, mined from the same passes' frame-cycle
windows): the parked cockpit state at any of the three settings rides the
pacer at 88–89 fps with ~9 ms/frame idle in wait_frame; the ~3.5k removed
draws of passes B/C cost < ~0.15 ms/frame (delta mixed-sign within
session scatter). No game-GPU timer exists in the logs; EDVR's own spans
are sub-ms and identical across states. Conclusion: the PARKED census
state is not bounding on this rig — there is no perf problem to fix in
that state, and the draw-attribution lever has no measurable cost there.
Confounds recorded: pass A straddled a live temporal-AA toggle (only its
w3 window is matched); armed-census windows collapse fps (instrument
overhead, excluded); 22–55 ms periodic hitches appear in ALL states.
Side finding: ~9 ms/frame of pacer headroom means draw-time hook work
(e.g. motion-truth sampling) fits the budget on this rig. Costing the
REAL settlement state needs a traverse flight (below).

Stock cross-check (Sean, fpsVR at this same settlement, stock runtime):
GPU ~6.8 ms, CPU ~10 ms against the 11.1 ms budget — CPU-bound confirmed
and quantified; the scene lives ~1 ms from the edge. Paradox with the
mined passes: if the ~10 ms CPU were eye-pass draw submission, removing
3.5k draws (passes B/C) would have recovered >= ~1 ms; it recovered
< 0.15 ms. Conclusion: the settlement CPU wall is NOT in the draw
stream — census draws are near-free CPU-side (GPU-driven instanced
strips; heavy work is upstream). The LOD-override and A-gate ideas are
dead as PERFORMANCE features (preference options at most). The 10 ms
points at the per-frame job pipeline — kinematic eval over ~1,572 rigs,
physics, the bucket worker storm — matching the L1 job brackets' summed
~7 ms/frame that the 2026-09-19/20 flights measured but never placed on
the critical path. The perf lever pivots to the L3/L4 candidates
(kinematic-eval narrowing, physics decimation); this arc has already
mapped their structures (stage 2, the bucket lifecycle, the named
scheduler). Next: mine the existing L1-bracket flights (190122/193356)
for the settlement job breakdown vs the 10 ms; optionally one
calibration flight with fpsVR running alongside EDVR's instruments to
correlate fpsVR's CPU figure with the brackets.

L1 re-read (2026-09-21): the brackets-only flight (193356) sums only
~0.26 ms/frame of kinematic jobs over a MOSTLY NON-SETTLEMENT session
(UpdateRenderDataJob 55 us mean, 0.167 ms/frame; RenderDataBatch 176 us,
0.094) — the old ~7 ms figure was the detailed-observer artifact in a
different scene, not reproduced. BUT the scheduler probe shows the
settlement invocation volume is ~432 job-0 calls/frame + ~79 batch/frame
(vs ~3/frame averaged over 193356): extrapolated ~24 ms + ~14 ms of CPU
spread over worker threads, i.e. ~4-5 ms wall if overlap is poor —
reachable toward the 10 ms wall, but per-call costs come from a
non-settlement scene and thread overlap is unproven. NOT settled by
existing data.

Stock view-dependence test (Sean, fpsVR, stock install, same settlement,
2026-09-21): model-draw-distance UI slider change -> CPU ~9.5 ms, NO
noticeable change (independent stock-side corroboration of the EDVR-side
< 0.15 ms draw finding). On foot (flat panel): 7.8 ms. Looking down at a
bare surface patch: 4.2 ms. In space, CPU frame time is typically HALF the
surface figure (~2 ms). Interpretation: TWO floors - a universal floor of
~2 ms (engine base + VR runtime + fpsVR; the space number) plus a
PLANET-SURFACE floor of ~2.2 ms that runs regardless of view; then ~1.7 ms
for the stereo second eye pass (cockpit vs on-foot), then ~5.3 ms of
view-dependent settlement content. The surface floor's prime suspect is
the terrain subsystem's per-frame CPU (quadtree/LOD evaluation, chunk
culling, streaming - the ground under a 'bare patch' is still resident,
managed terrain; the draw stream is refuted as the cost, so it is
system-side; see docs/terrain-culling.md), with atmosphere/volumetrics
secondary. The eval gates (distance/LOD/frustum - the exact chain this arc
mapped) are what size the view-dependent content: looking away shrinks it,
consistent with the heading-dependent worker storm. Draw submission is
refuted as its cause, so it is eval + kinematic jobs + physics for the
passed set.

Stutter track (LONG FRAME forensics, 2026-09-21): the recurring 22-55 ms
'hitches every >= 5 s' in the flight logs were first mis-read as a 5 s
cadence - that regularity is the LONG FRAME report's OWN throttle ('at
most one of these lines every 5 s'), not a source. The hitches themselves
are ENGINE game-thread CPU stalls that EDVR only observes: thread busy
22-414 ms with EDVR boundary/hook costs ~0.00 ms and 'EDVR events: none'
on the same frames. The 5 s gate HIDES duplicates, so true frequency is
higher than the log shows (frame deltas cluster at 371-900 frames, right
at the gate). No consistent coincidence: the 5 s metrics tick is density
not cause; most hitches show zero texture creations, though occasional
bursts sit near some (681 tex/342 MB; 139/208 MB) - streaming stalls
possible but unproven. Catch the true cadence on stock with PresentMon or
the SteamVR timing overlay; EDVR-side, a de-throttled LONG FRAME variant
would count what the gate hides. The attribution flight (below) now REFINES
the split (UpdateRenderDataJob vs RenderDataBatch vs physics vs
eval-gate overhead; thread overlap) rather than discovering the wall.
  The attribution flight (zero config changes needed — engine_motion and
  scheduler_probe are already on in the flight INI): parked at the
  settlement >= 60 s, then hotkey eye-burst to dump jobs[], census armed
  for scene confirmation (~18k eye draws = same scene as the fpsVR
  numbers), ideally fpsVR alongside to calibrate EDVR's job totals against
  fpsVR's CPU figure. Compare: UpdateRenderDataJob / RenderDataBatch
  calls+mean+total in the settlement window vs the wall.

ruled out: fixed-density scatter (A's instances are ordinary pool
records); an alternate LOD keyed elsewhere (no LOD branch exists);
identification via census_cb_watch (A/B bind no b0 — resolved: frame
constants in b1, instance data in SRVs).


### 2026-09-21 -- Design: change-gated render-data updates (targeting the 5.3 ms view-dependent content)

**Cost (measured):** ~5.3 ms/frame of view-dependent settlement content
(stock fpsVR view test: 9.5 ms cockpit-with-settlement vs 4.2 ms bare
ground, same rig). Composition: eval + kinematic jobs + physics for the
passed set; draw submission refuted (< 0.15 ms for 3.5k draws). Extrapolated
suspect: ~432 UpdateRenderDataJob (55 us) + ~79 RenderDataBatch (176 us)
calls/frame at the settlement = ~4-5 ms wall if thread overlap is poor —
EXTRAPOLATED from a non-settlement scene; Phase 0 replaces this with
measured settlement numbers.

**Mechanism.** Settlement structure/prop records are static: their
per-frame truth (record+0x170 position, +0x17C packed quat — the fields
FUN_14433DB20 writes) is bit-identical frame after frame, yet the engine
re-composes and re-writes their render data every frame; the bucket items
and instance buffers persist cross-frame and the eye-pass draws re-execute
from them unchanged. Skipping the re-production for unchanged records
removes the CPU while producing byte-identical output. This is change
gating (the L4 idiom), not suppression: nothing is hidden, nothing is
culled — redundant work is not repeated. It does NOT touch the ungated
A-layer question, culling, or LOD selection.

**Hook point.** job-0's params carry the collection array + count
(FUN_144321940 param struct [5]/[6]); one call processes one collection
(~1.7 records average, 2,672 records over 1,572 rigs). Gate at the call:
if EVERY record of the collection matches its cache entry, skip the call
(forward past, run nothing). Movers' collections (any changed record)
re-run whole — conservative, correct. The eval hook already brackets
FUN_144321940, so the install point exists in-tree.

**Change test (v1, deliberately conservative).** Cache per collection:
collection pointer + per record {record pointer, +0x170, +0x17C,
record+0x570 mask}. Run the job if any field differs from cache, on first
sight of the collection, or every N frames (forced refresh, N=30
failsafe). Invalidate ALL caches on: journal boundaries EDVR already
watches (LoadGame/Disembark/StartJump — engine doc's journal watcher), the
reset path firing (FUN_1436a0f50 — already probed), and session
boundaries. Open dependency named honestly: per-record identity (~5
records share one node) means the cache keys by ARRAY SLOT (record
pointer) with the truth fields as the change oracle; the slot-reuse
events the landed-but-unflown KinematicEvalProbe capture would detect are
exactly what the forced refresh bounds.

**What must be true for it to be safe (verified in Phase 2, not
assumed):** (a) bucket items persist while their producer is skipped —
the 2026-09-21 trace shows items live in batch nodes until rekey/reset;
the reset probe doubles as the invalidation trigger; (b) skipped
collections produce unchanged instance/slot data — true by construction
(bit-identical inputs, deterministic compose), validated by census
draw-count equality and eye-draw snapshots; (c) stereo/moving occluders/
shadows consume the same persistent items — unchanged items mean all
passes unchanged; (d) scene transitions covered by journal + reset
invalidation.

**Phases and gates.**
- Phase 0 (flight, zero new code): the attribution flight — park >= 60 s,
  eye-burst, jobs[] dump; replaces extrapolated per-call costs with
  settlement-measured ones; ALSO a cpu_profile capture inside job-0 (the
  tool exists) to check the alternative hypothesis that the 55 us is one
  hot inner loop rather than irreducible compose work. Gate: if measured
  job-0+batch wall share < ~1.5 ms, this design is not worth building —
  the 5.3 ms is elsewhere and the design returns to evidence.
  (2026-09-21: folded into the Phase 1 flight by decision — the build
  logs brackets with the gate off vs on, so the kill-gate is evaluated
  from that flight's before/after; the cpu_profile inner-loop check
  remains optional.)
- Phase 1 (build, config `fix.static_prop_updates = on|off`, default off):
  the gate as above + counters (calls skipped/run, forced refreshes,
  invalidations; brackets before/after). One flight: census draw counts
  MUST equal baseline (draws unchanged), brackets must show the expected
  call drop, frame-cycle must improve by the Phase-0-measured wall share
  (not just the CPU sum — thread overlap is the open variable).
- Phase 2 (validation, no new code): settlement-flicker instruments as
  the regression harness (the arc's zero-to-visible detection), a
  mover-heavy window (ship launch/NPC activity) to prove movers re-run,
  and a journal-transition pass (jump out/in, dock) to prove invalidation.
  Gate for default-on: zero census delta + zero flicker-instrument hits +
  measured wall saving >= the Phase-0 number, across all three.

**Expected saving (to be replaced by Phase 0):** most of the static
collections' job time — order 3-4 ms CPU on the settlement mix if the
extrapolation holds; wall saving subject to thread overlap. **Regression
surface:** stale props after missed changes (bounded by the forced
refresh + movers-always-run), invalidation bugs across transitions (the
journal/reset triggers), and identity collisions (bounded by N-frame
refresh; the probe capture quantifies if needed).

**OUTCOME (flown 2026-09-21, leg 1 gate-off / leg 2 gate-on at 'Chanfield
Nutrition', 38 Lyncis): ABORTED — the persistence invariant is refuted.**
The gate worked exactly as designed: 91.2% skip rate at steady state,
zero verify_faults, zero evictions, healthy change oracle. The flicker
is STRUCTURAL, not a bad skip: job-0's (FUN_144321940) output is
CONSUMED per frame, not persisted — a collection's structures render
only on frames where its job ran. Steady-state leg 2: 93% of EB52
instances absent every frame (2,850 vs 15,915 draw rows/frame); the
settlement flashed back at frames 18-19 as the 30-frame forced-refresh
wave (collections cached in one streaming burst, refreshes in phase) —
~2 visible frames per ~30; without the failsafe it would have been
'structures absent', not flicker. Census inequality 5.6x. The design's
core assumption ('items persist, draws re-execute from them') was wrong
for the mesh/settlement path (it held only for the bucket/light
subsystem). Downstream skipping of job-0 is dead in any form: production
must run every frame for content to exist. ruled out: every skip-based
variant of this design. What survives: (a) make the job cheaper
(cpu_profile inside job-0 — the 91% skip rate proves the wall-time win
exists but cannot be harvested by omission), or (b) change detection
INSIDE the job that re-emits identical output every frame at lower cost
— the output side can never be gated. Phase 1 code stays default-off
(dark; one atomic load when off); removal is a pending question. Leg 1's
baseline (jobs[] at this settlement, temporal off) is retained as the
cost reference.


### 2026-09-21 -- Phase 1 implemented (build-gated, unflown)

The change gate landed on branch `static-prop-gate-2026-09-21`:
`src/d3d11/static_prop_gate.{h,cpp}` (bounded 4096-collection cache, 16
records each, raw 24 B truth copies memcmp'd, one mutex, no hot-path
allocation, SEH-guarded reads), consulted from the kinematic eval hook's
existing job-0 bracket BEFORE its timed region (a skip forwards past the
call entirely -- skipped wall time reads off the jobs[0] bracket drop, the
kill-gate comparison the folded Phase-0/1 flight needs). Reset
invalidation rides the existing scheduler-stack hook's resetObserved
(its relays now gate on a cell recomputed from probe OR gate want, the
evalGate discipline verbatim); journal boundaries are polled once per
frame from the present tick (the watcher exposes state, not events --
LoadGame rising, Disembark/Embark counter steps, StartJump's tunnel latch).
Forced refresh N=30. Counters per design (calls seen/skipped/run, forced
refreshes, invalidations split journal/reset/session, cache size,
evictions, verify_faults) at the shared 20 s cadence, `static_prop_gate:`
prefix. Config `fix.static_prop_updates` (bool, default off) documented in
edvr.ini; rig `tools/static_prop_gate_test` (4141 checks: change-test
hit/miss, first-sight, forced refresh, reset/journal/session
invalidation, eviction at capacity, guard-page fault tolerance, oversize
runs-uncached) plus a JSON round-trip gate.

**Param layout verified against decomp_4321940:** param_1[5] = record-array
base (local_78/pcVar3), param_1[6] = count (uVar2), records at
base+i*0x2F0. The design's "collection+0x280" is the collection object's
record-array SLOT (engine-render-pipeline.md stage 2); the call hands the
array directly, so the cache keys on the param[5] pointer.

**Deviations from the design text, both conservative:** (1) the +0x570
mask read exceeds the 0x2F0 record stride (it lands in the next record's
+0x280 tail; on the last record it may cross the array end) -- kept as an
extra oracle field because a false mismatch only ever re-runs, never
wrongly skips, and faults are SEH-counted; (2) collections larger than 16
records run uncached every frame (fixed pool, counted as `oversize` in the
JSON rather than silently cached). The watcher has no consumer callback;
the v1 set is session + reset + journal-poll + forced refresh, per the
design's own fallback.

Next flight: brackets with the gate off vs on at the settlement
(Phase-0 kill-gate: job-0+batch wall share), then census draw-count
equality, movers re-running, and a jump/dock invalidation pass (Phase 2).


### 2026-09-22 -- The visibility prize, measured: ~91-97% of submitted settlement records are provably unseen from a parked view (~4.8-5.1 ms of the 5.3 ms content bound)

The depth-capture join (run 055252, frame 2, both eyes): 12,287 submitted
records (ledger union via the instance stream) projected through per-eye
view-projections recovered from the eyemesh drawstate (the depth files'
own constants block was unusable — keying artifact, below) and tested
against the stored D32 depth. Visible-in-either-eye vs unseen-in-both,
by conservative footprint radius: r=0.3 m -> 96.2% unseen; r=1 m ->
90.7% unseen (content-only variants 96.5-98.8%). Structure of the unseen:
near-field settlement clutter — 7,273 records nearer than 300 m, zero
beyond 4 km, only 29 out-of-frustum in both eyes (frustum already does
its job). Spot checks validated the convention both ways (an own-write
record matching stored depth exactly; a genuinely occluded record).

**Prize: ~4.8-5.1 ms of the 5.3 ms view-dependent content upper bound**
(r=0.3-1 m, the trustworthy band; assumes cost ~linear in submitted
records). This is the measured answer to 'cull what is not visible':
from a parked cockpit view, essentially ALL of the view-dependent CPU is
spent on content nobody sees — occluded by other structures, not
distance-culled, not frustum-culled.

**Caveats that shape the design (not disclaimers — engineering
requirements):** (1) The unseen fraction is a CEILING on the cullable
set: the footprint test under-claims visible (origins inside their own
silhouette read as unseen — the r=3 column's ~5% inflation); the true
cullable set sits between the content-only and all-tap numbers. (2)
Head ROTATION un-occludes: the parked view occludes ~91-97%, but the
set is view-dependent per frame — this sizes the prize for a DYNAMIC
per-frame culler; it is not a cull list. A static cull of this set
would be the flicker arc's bug exactly. (3) One settlement, one parked
pose; moving views will show more visible content — the dynamic average
prize needs a view sweep (now cheap: every armed eye run yields depth).
(4) The depth capture's constants keying is FRAGILE: this frame's first
pool-carrying draw was a 3-vertex eval-like pass whose cb1 is not the
eye view-proj (the join recovered projections from the eyemesh
drawstate instead). Fix the keying (structure-family draw, or N-vertex
threshold) before relying on the constants block in future runs.

**Instrument note**: the depth capture needed three fix rounds (eye-slot
selection by first-seen DSV -> fixed to the depth probe's scene-eye
pick; the probe's watch gated on temporal/eye-mask -> the capture lights
it itself; the eye depth is R32G8X24_TYPELESS 2665x2632 -> converted via
the probe's read path). Declines are now reason-coded (format/bytes/
frame-cap) so a refusal is never blind again. Flown green: 4 files,
0 declines, 190 MB staged.

**Design + review (2026-09-22):** the culling design was written up at
docs/design-occlusion-culling-2026-09-22.md and reviewed in-repo (review
ledger in the gitignored reviews/ dir; the doc was revised in place).
The review kept the evidence chain and rebuilt the mechanism: integration
moved to the engine's own frustum-reject path (clearing a record's bit in
the evaluator's active mask — indistinguishable from the engine's own
reject, removes the walk AND the emission, and the gate has the frame's
own camera); occluders are individually solid opaque parts qualified
offline (opaque PS + closed mesh), never whole structures (buildings are
hollow shells the player walks through); the test is a conservative CPU
coverage buffer with occluder fusion (pairwise shadow-volume as fallback);
the oracle is an EXACT truth (MeshCoverage per-pixel record ownership or
generalised per-draw occlusion queries) over a >= 5-pose corpus, because
the footprint join over-claims unseen. Two findings the session should
have caught itself: the job-0 hook has no view (the map's own stage 5
says per-eye projections exist only at draw time), and the prize's ms
figure rests on thread-summed job time whose critical-path share is
unproven — the journal recorded both. Phase order was corrected: Phase A
(one cpu_profile WPR flight: does the main thread wait on the job
pipeline, walk-vs-emit by address, plus depth/truth at >= 5 poses) goes
FIRST because it is the cheapest kill.

ruled out: draw-dedup culling (~213 fully-identical submissions/frame,
0.3%); static occlusion culling of the measured unseen set (view-
dependent; would reproduce the flicker class). What the number funds:
a DYNAMIC eval-level culling design — per-frame, conservative, the
depth-join as its regression oracle (culled set must be a subset of the
per-frame provably-unseen set, every frame, with movers running). The
design is written for review at docs/design-occlusion-culling-2026-09-22.md
(evidence chain, soundness sketch, staged plan with kill gates, review
questions).


### 2026-09-22 -- Phase A verdict: KILL. The settlement CPU wall is not the job pipeline; the consuming thread has ~7 ms of pacing slack

Phase A flown per the reviewed design: one WPR capture (build/phaseA-cpu-parked,
543 MB ETL, build 1262c8c pinned) + four posed eye runs (parked cockpit, on
foot, inside-looking-out, doorway; the SRV pose was unavailable - no SRV at
this settlement - and the vehicle view is covered by the cockpit pose).

**Critical-path verdict (the kill gate):** the consuming thread (the game's
D3D context thread, tid 34408 - every frame marker and sampled stack is its)
accounts for ~1.9 ms running + <= 0.75 ms waiting + ~7 ms pacing wait per
11.1 ms frame. Its wait wakers: 3,195 generic scheduler signal (0x5d6xxx),
1,784 EDVR frame signal, ~2,000 assorted game code - ZERO in any job-pipeline
range (0x4321940/0x4320340/0x430EFE0/0x433DB20) out of ~14k observations.
CPU contention nil (0.05 ms ready). **There is no ~4-5 ms wall on the
consuming thread to harvest: LOD-attributable stall is ~0; even the
generous <= 0.75 ms x recall bound sits under the design's ~1.5 ms bar.**
With EDVR installed, this rig at this settlement is pacer-limited (88-89
fps), not pipeline-bound. This closes the occlusion-culling arc at Phase A
by its own gate - the cheapest possible kill, which is the design working.

**Capture defect (recorded as a tool gap):** edvr_cpu.wprp's 512x1 MiB
buffer set is a ~13.7 s ring at the measured 37.8 MB/s - the designed clean
parked leg was overwritten before the 300 s stop; the decoded window is
post-pose-4 (stable 401-frame sub-window analyzed, 88 fps). The walk-vs-emit
gate was therefore NOT exercised: the report's samples are window-scoped to
the consuming thread (265 observations, none in pipeline code; the pipeline
runs on workers outside the decoded windows). **Escape hatch (user's call,
recommended against on expected value):** a retaken trace with a larger WPR
buffer plus analyzer coverage of WaitGetPoses-return -> PresentBegin and
worker-thread attribution could only resurrect <= ~0.75 ms of join stall -
under the bar even if found.

**Corpus findings:** depth dumps exist only for the cockpit pose. The three
on-foot poses expose a single flat-panel target, so the depth probe's
two-target scene pair never forms (decline counters all-zero - never
engaged, not refused): eye depth capture is native-VR-only BY DESIGN, now
recorded. The classification runs produced a producer/identity corpus but
ZERO ownership links (unwind_failed ~90%, ancestor_missing ~10%,
no_selected_frame in all four runs) - Phase B's record-to-object join did
not survive; recorded as an instrument gap for the object/flicker arcs.

**What stands for the whole perf question:** on this rig under EDVR, the
settlement is not CPU-bound - the earlier stock fpsVR ~9.5-10 ms CPU figure
is whole-process; the thread that paces the frame works 2.5-4 ms and waits
the rest. The ~5.3 ms view-dependent content cost measured by subtraction
is real CPU time, but it runs on worker threads that the critical path does
not wait on. Remaining perf surface: the ~2 ms universal + ~2.2 ms
planet-surface floors (engine base + terrain system - not reachable from a
proxy) and the ~7 ms pacing slack (headroom, e.g. for the motion work).

ruled out: occlusion culling of the job pipeline (killed at Phase A);
the escape-hatch retake is available but its ceiling is under the bar.
Also closed: any prize arithmetic that multiplies the unseen fraction by
the thread-summed job time - the wall-share was the gate and the gate
failed.

### 2026-09-22 -- Phase A verdict WITHDRAWN: the trace never covered the cockpit leg, and the analyzer's window is the post-present slice

Reviewed the same day against the tool's own output
(build/phaseA-cpu-parked/report.json, 847 frames), the analyzer source
(tools/cpu_profile/Program.cs) and the runtime log's own frame-cycle
instrument (edvr_openxr_20260922_081020_495_38876.log,
native_frame_cycle_window / _phase every 30 s). Review record out of tree:
reviews/phaseA-verdict-review-2026-09-22.md (gitignored, main checkout).

**What the window was.** The 13.47 s ring covers 14:23:58-14:24:11.6 UTC:
the on-foot doorway pose in deferred pacing, then the menu and the exit.
The runtime's pacing-mode change to `runtime` at sequence 40252
(14:24:04.096) is the analyzer's featureEpoch 4->5 at frame 492 of 847;
`module_shutdown_entry` is logged at 14:24:11.903, a quarter second after
the trace ends. The parked-cockpit leg is what the ring overwrote; the
verdict entry never named the state it analysed.

**What the analyzer measures.** Its per-frame window is
[PresentEndUs, NextWaitEntryUs) (Program.cs:408-409): Present returned to
the next WaitGetPoses entered, i.e. EDVR's post_second_submit_to_next_wait
phase (3.5 ms mean in window 31; the report's durationUs p50 is 2.9 ms).
game_before_first_submit - the render phase, 3.3-4.6 ms on foot and
11.4-13 ms in the cockpit - lies entirely outside it, and so does the
pacer block inside the second Submit (3.0-4.6 ms). "~1.9 ms running +
<= 0.75 ms waiting + ~7 ms pacing" is the post-present slice plus
everything else lumped as pacing. Window 31's honest split from the cycle
instrument: ~7.4 ms game work, ~4.6 ms pacer block, 12.0 ms cycle (83.1
fps, p95 15.2 ms - not "88-89 fps pacer-limited").

**The session, caller thread 34408, from the cycle windows:** windows
7-15 (14:12:42-14:16:42, parked cockpit, stereo, pacing=0): 44.5-45.0 fps,
cycle 22.3 ms, game_before_first_submit 11.4-13.0 ms (p95 13-15),
post_submit 5.1-6.2 ms, next_wait 2.1-4.9 ms - half rate, no pacer block,
~18 ms of caller-thread wall per cycle. Windows 25-31 (on foot, deferred
pacing from 14:20:50): 59 -> 88 fps, before_first 9.3 -> 3.3 ms, pacer
0.9 -> 4.6 ms. Caveat: the object probe, eye depth capture and
classification capture were armed all session and may inflate the cockpit
numbers (stock fpsVR: 9.5-10 ms at 90 fps); the cockpit leg needs a clean
re-measure before it is called the wall.

**The waker argument does not hold.** In the analysed slice the caller
thread's largest blocking site is inside the job scheduler: 2,949
switch-outs at +0x5d6d7f under +0x5d4141 / +0x5d67a6 (the job-table and
per-worker pump, ~3.5 per frame); the wakers are the scheduler's signal
path from other threads (+0x5d6dcd: 2,277+204+131+81; +0x5d6abd: 304+95).
A job body cannot appear on a signaller's stack - the worker signals after
the job returns - so searching wakers for 0x4321940 etc. cannot see a join;
the analyzer has no RVA list at all, and the matching was manual over the
top-50 whole-stack groups. "265 observations, none in pipeline code" is
also wrong: ~70 of the 265 caller-thread samples sit in the pipeline's own
chain - the UpdateRenderDataJob call site 0x431B21F
(object_record_writer_probe.cpp:14-15) and job thunks under EDVR's
bracket frame (d3d11.dll+0x13fd17 <- +0x42df9a0 <- +0x434d847).

**"The fpsVR figure is whole-process"** is contradicted: the stock on-foot
fpsVR figure was 7.8 ms; window 30's caller-thread phases sum to 8.3 ms and
window 31's to 7.4 ms. The overlay reports the pacing thread's game work.

**Withdrawn:** the KILL; "the settlement is not CPU-bound"; "pacer-limited
at 88-89 fps"; the <= 0.75 ms retake ceiling (it is the post-present wait
in the lightest state of the session and bounds nothing). **Stands:** the
ring-buffer tool gap (real; it decided the flight); eye depth capture is
native-VR-only; the classification ownership join failed. The design's
gate logic was applied honestly, to data that does not measure the gate.

**Scope from here (Sean, 2026-09-22):** cockpit only - true stereoscopic
settlement rendering from the ship. On foot is out of scope for now. New
observation to instrument: CPU frame time rises noticeably once the ship
comes within ~1 km of the settlement - a distance-gated onset (the eval
chain's distance/LOD gates admitting the settlement's records, or
streaming) that one approach flight can locate with the cycle windows
already in the log plus a distance reading and the per-frame record/draw
counts.

**A valid Phase A needs:** (1) file-mode WPR (the EDVRCPU file profile
exists in edvr_cpu.wprp; cpu_profile.py asserts memory mode - a tool
change), the cockpit leg FIRST and the capture stopped before any eye run;
(2) no capture instruments armed during the leg (a second leg with them on
measures their cost); (3) the analyzer emitting WaitReturnUs /
SecondSubmitReturnUs / PresentBeginUs with running / ready / waiting per
region - [WaitReturn, FirstSubmit), [FirstSubmit, PresentEnd),
[PresentEnd, NextWaitEntry) - reconciled per window against the runtime
log's cycle phases (they must agree, or the analyzer is wrong); (4) a
per-thread busy table so worker load per frame is visible; (5) scheduler
waits attributed by what was signalled (the waker's last sample or its L1
bracket state), not by the signaller's stack; (6) mechanical RVA matching
over all stacks against the job bodies, the thunk table, 0x431B21F and the
scheduler signal sites. The old ETL (build/phaseA-cpu-parked/flight.etl)
plus log windows 30-31 is the offline test input for (3)-(6): the numbers
are known, so the analyzer can be proven before a flight is spent.

ruled out: nothing new. Un-ruled: occlusion culling of the job pipeline -
its Phase A gate is UNMEASURED, not failed.

### 2026-09-22 (addendum) -- The sub-90, decomposed: steady 88-89 is 2-3% one-cycle-late VDXR releases; the mid-40s are VDXR half-rate throttle stretches

Mining the session logs' per-30 s native_frame_cycle_windows (boundary =
host wait-return -> next wait-return, the same Present->Present quantity
the overlay monitor reports, perf_monitor.cpp:714,1068): steady-state
p50 sits exactly on 11.111 ms with the mean 0.22-0.4 ms above - the
signature of 2-3% of xrWaitFrame returns arriving a full 11.111 ms
period late (window mean reproduced exactly by 98% at 11.113 + 2% at
22.2). The game chain carries ~9.5 ms of slack on every phase
(per_frame_residual 0.000), so the lateness is in VDXR's frame-signal
release inside the blocking wait, not the game: ~1.7-3.1 missed display
cycles per second (~100-190/min), plus a 40-90 ms shader-compile stall
every 30-60 s. That is the entire 90 -> 86.9-88.3 gap. Turbo pacing
moves the loss into a broadened jitter band with the same net - not a
fix. No reprojection is logged anywhere (0 DROPPED FRAME lines).

The user's mid-40s/low-60s readings are REAL and different in kind: the
logs show multi-minute stretches where VDXR releases waits at exactly
1/2 of 90 Hz (45.0 fps, cycle p50 22.22 ms - 4.5 min in the 081018
session) and once 1/7 (12.86 fps, p50 77.79 ms - 3.5 min), plus 44-49
fps tails at session ends: VDXR's idle/motion-smoothing throttle
engaging for stretches. No counter in EDVR's runtime names it. The
monitor reads these faithfully - the app paces at 88-89 THROUGH the
throttle (cycle window still counts app frames) while the delivered
rate halves.

Actionable surface: (1) USER-SIDE zero-code experiment first - Virtual
Desktop's motion smoothing / SSW toggle: if the half-rate stretches
vanish with smoothing off, VD is engaging SSW on the engine's periodic
stalls and sticking (its detection misfiring on a 2-3% miss app that
otherwise holds 90 with 9.5 ms slack). (2) INSTRUMENT (small runtime
add): the single missing line - per-frame xrWaitFrame return timestamp
and xrEndFrame submit timestamp vs VDXR's predictedDisplayTime,
window-histogrammed like native_frame_cycle_phase. If submits land
before the deadline while the signal still slips, the throttle is
VDXR-side (out of reach; the user-side setting is the fix); if submits
straddle, EDVR's submit path is the fix. (3) The engine's periodic
stalls (true rate far above the throttled log; shader compiles 40-90 ms
every 30-60 s) remain the engine-side trigger worth a PresentMon stock
capture + the de-throttled counter.

Correction (2026-09-22, later): the 081018 session did not run on VDXR.
Its runtime log names the runtime (headset_key,pimax-openxr/
pimax-crystal-super,runtime=Pimax OpenXR,system=Pimax Crystal Super;
openxr_resolution headset=5424x5356 requested=0.7559), so the half-rate
stretches above are Pimax OpenXR's, and the user-side toggle to try is
Pimax's smart smoothing, not Virtual Desktop's SSW.

### 2026-09-22 (addendum 2) -- Monitor audit: clocks correct, presentation defective; the Pimax wall is real, rig-resolved

Pimax 8KX + SteamVR session (smart smoothing OFF, EDVR native OpenXR
runtime on SteamVR/OpenXR): fpsVR read 42-44 fps / CPU 18-19 ms / GPU
<1.0 ms at the settlement; EDVR's monitor read ~56 fps / CPU-GPU 12-13
ms. Audit (code + the 09:37 session log):
- No clock bug: overlay fps = producer cadence (game Present->Present
  QPC delta, 900-frame ring, perf_monitor.cpp:713-718,1068); CPU = app
  work only (waits excluded); GPU = app render only (compose excluded).
  fpsVR's CPU 18-19 = EDVR's app 12-13 + ~5-6 ms of waits - consistent.
- The defects are presentational: 'FRAME RATE' reads as headset fps but
  is production cadence; on the native path the monitor surfaces NO
  display-side signal (drop/reprojection tiles are 'native OpenXR
  timing unavailable'), and hides the runtime's own predictedPeriod
  (which read 22.222 ms = 45 Hz DURING the settlement stretch - SteamVR
  halving the display rate under the wall). Production 50-57, display
  42-45: both instruments faithful, the page shows the wrong side.
  fpsVR's GPU <1.0 is a layered-submission artifact (SteamVR attributes
  almost no GPU to the app; its own 26 MP distortion is invisible).
- THE PIMAX WALL IS REAL (rig-resolved): EDVR's own numbers at the
  settlement: producer 11.9-13.4 ms vs 0.5-3.3 away, app GPU 8.2-14.9
  ms - at XR 4100x3212 per eye (13.17 MP, 26.3 MP both eyes - ~3.1x the
  Quest rig's per-eye pixels), plus the ~5.3 ms job pipeline. The
  m02 = -+0.1389 asymmetric frusta are the ORDINARY off-axis stereo
  projection (principal-point offset - the same signature the Quest
  analysis proved non-canted, 2026-09-21), NOT canted projection: the
  user's headset is a Pimax Crystal Super micro-OLED with 0 deg cant,
  and an earlier attribution of a 'canted ~1/3 tax' here is retracted.
  Draw counts match the Quest rig (~19.4k) - the cost scales with
  PER-EYE PIXELS (GPU-side, plus GPU-bound backpressure inflating the
  CPU-side intervals and tile-count-scaled compute dispatches), not
  submission. SteamVR halves the display rate under that wall (22.222
  ms predicted period) - fpsVR's 42-44 is the delivered truth.
So: Quest rig = pacer-limited (VDXR), Pimax rig = a genuine CPU+GPU wall
at 26 MP - the user's original 'settlements are CPU-bound' is rig-true.
FIX (small, spec'd): a DISPLAY tile on the native path - surface the
runtime's predictedPeriod (throttle detection) plus a display-cadence
estimate, so the page can never read 56 while the headset shows 45.

Landed 2026-09-22 (branch perf-monitor-display-tile-2026-09-22): DISPLAY
tile shows predictedPeriod as Hz with the base rate and a THROTTLED flag at
>1.5x base; base crosses as EdvrNativeTimingFrame v4 baseDisplayHz (the
display_frequency publication), falling back to the session's first
predicted period for a v3 host. No display-cadence estimate shipped: no
accepted/completed-frame counter with display timestamps crosses the ABI.

### 2026-09-22 -- The ~1 km onset: candidates, their log signatures, and the one approach flight that separates them (desk work, no flight)

Scope is cockpit only (Sean, 2026-09-22). The observation to locate: CPU
frame time rises noticeably once the ship is within ~1 km of a
settlement. Desk research over the pipeline map and the 2026-09-21
entries (analysis\ is gitignored; the docs carry what it found) gives
five candidates, each with a discriminating signature that ONE
file-mode trace of the approach can read at per-frame resolution. The
runtime log's 30 s cycle windows are the coarse cross-check, not the
instrument: at 100 m/s a 30 s window is 3 km.

* **C1 Stage 1 per-record distance/LOD gate** (FUN_14430EFE0 applies the
  distance/LOD + frustum gates, FUN_144331300 computes the active mask;
  LODDistanceScale alone reproduced the EB52 family's drop, pipeline map
  Stage 1). Admission is per record as its screen-size test passes, so
  job-0 load RAMPS with 1/distance, in LOD bands (several small steps),
  with no one-shot event.
* **C2 Collection admission at a radius** (reset-repopulate
  FUN_1436a0f50, RVA 0x36A0F50, never fires while parked; the scheduler
  probe counted ~432 job-0 calls/frame + ~79 batch/frame at a
  settlement vs ~3/frame elsewhere, 2026-09-21). One frame with samples
  inside 0x36A0F50, then job-0 STEPS from ~3 to ~432 calls/frame; the
  journal's ApproachSettlement event may or may not coincide (its
  firing radius is unknown; this flight measures it).
* **C3 Physics/kinematic scope** (job 2, UpdatePhysicsObjectsJob
  0x432B2A0): the passed set's physics grows while job-0 does not.
* **C4 Not the pipeline**: the caller thread's game_before_first_submit
  grows as WAITING at D3D/driver/kernel blocking sites (GPU
  back-pressure, which addendum 2 above names for the Pimax rig, or the
  half-rate throttle), or as EDVR's own per-draw hook time (samples in
  EDVR's d3d11.dll on the caller thread scaling with the draw count).
  The 2026-09-22 cockpit windows (12 ms before first submit, 5.1 ms
  after Present, 22.3 ms cycle) do not say which; the analyzer's
  running/waiting split per region does.
* **C5 Streaming**: no settlement asset-streaming mechanism is
  evidenced (the "streaming burst" of the 2026-09-21 design entry is
  job-0's internal cache refresh); it would show as I/O blocking sites
  and worker time outside every pipeline class.

**Signatures, all from one trace plus two files:** (S1) per-second
series from the analyzer's frames.jsonl: before_first mean, its
running/ready/waiting split, thread-summed samples per RVA class
(job0/job1/job2/eval/record_drain/reset_repopulate/scheduler/EDVR
d3d11/other) and the first frame that samples 0x36A0F50; (S2)
distance: haversine between Status.json (Latitude, Longitude, Altitude,
PlanetRadius, ~1 Hz, sampled by cpu_profile.py into
status_samples.jsonl) and the settlement (ApproachSettlement: Cranfield
Nutrition Biosphere on 38 Lyncis 4 f, lat 68.075294, lon 121.067451);
(S3) the journal's ApproachSettlement timestamp. Draw counts are not in
the trace (the census is hotkey-armed only, tens of ms per census
frame), so one NUMLOCK census at the far end and one when parked give
the eye-draw count at the two ends only. Read: a step at one frame with
reset_repopulate sampled = C2; a ramp or bands without it = C1; job-2
rising alone = C3; waiting share rising with running flat, or the EDVR
module share rising = C4; I/O sites = C5.

**Not evidence:** the 081018 log's ApproachSettlement at 14:11:44 UTC
sits one window before the heavy cockpit windows 7-15, but that session
LOADED IN parked at the settlement (journal: LoadGame 14:11:08,
Location 14:12:02, no Liftoff/Touchdown), so the coincidence says
nothing about a radius.

**The flight (one session, one ini, two legs, two captures):**
environment to state in the entry: build HEAD built with
EDVR_PROFILE_SYMBOLS=1 (so EDVR's own frames resolve; `python
tools\edvr_log.py --target frontier --expect-build HEAD --version`),
Pimax Crystal Super on Pimax OpenXR with the log's openxr_resolution
line, DLSS state, Pimax smart smoothing OFF (a half-rate throttle fills
the regions with waiting; the analyzer separates it, a clean leg is
cheaper). Instruments OFF: `advanced.scheduler_probe = 0` (the live
Frontier ini has 1: a return-address signature walk at every job entry,
~432/frame, and it was ON during the 2026-09-22 cockpit leg),
`advanced.eye_depth_capture = 0` (live ini has 1),
`advanced.object_probe` absent or 0, no NUMLOCK census and no
classification capture during leg 1. Leg 1, the Phase A gate: loaded
parked at the settlement, cockpit, head still; from an elevated console
`python tools\cpu_profile.py --capture --target frontier --start-key F9
--stop-key F11 --max-seconds 90 --output build\phaseA-parked-2`; F9 in
the cockpit, 60-75 s, F11 (pick two keys that are not in the .binds;
F10 is Elite's screenshot). Leg 2, the onset: lift off, fly out past
5 km (nav-target distance on the HUD), turn, F9 at or beyond 5 km,
approach at 100 m/s or slower and 50 m/s inside 2 km, land or park,
F11; second run of the tool with `--max-seconds 420 --output
build\onset-approach`. Analysis: both traces through the analyzer with
`--runtime-log` (the per-window reconciliation must hold before any
number is read); leg 1 gives the gate (caller pipeline running +
pipeline-attributed waits, the critical-path share, recall x share x
5.3 ms against ~1.5 ms); leg 2 gives the onset per the signatures
above, joined to distance by UTC.

2026-09-23 -- carried from the Status block: a valid C2 test needs a
leg that starts from a real 10 km arrival, before any admission (the
flown approach leg began after a reload already at the settlement, so
C2 stayed untested).

### 2026-09-22 -- Retake tooling proven offline: file-mode capture, hotkey legs, and an analyzer that reproduces the runtime's cycle phases to 0.001 ms

Deliverable A of the retake (commits bd8df5c capture tool, 095eb09 +
8c2d768 analyzer, 90454b3 validated-build hashes; gate = build.bat with
EDVR_PROFILE_SYMBOLS=1, which is the only way the analyzer gate runs
and also the build to fly, since it links EDVR's DLLs with symbols).

**Capture (tools\cpu_profile.py):** `--capture` starts the EDVRCPU
file-mode profile (`-filemode -recordtempto <capture dir>`; the file
collector has 256 x 1 MiB of burst slack) unless `--memory-ring` asks
for the old 13.7 s ring. `--start-key`/`--stop-key` (GetAsyncKeyState,
works with the game focused) or `--start-after-seconds` arm a leg from
inside the headset; status.json records recording_started_utc /
recording_stopped_utc and the stop reason. Status.json is sampled at
4 Hz into status_samples.jsonl (one line per change: Flags, Latitude,
Longitude, Altitude, Heading, PlanetRadius, BodyName) and the newest
journal path is noted. --dry-run still writes nothing (self-test, 108
checks). The validated-build stamp now hashes every analyzer .cs file.

**Analyzer (tools\cpu_profile\*.cs, nine files, self-test 296 checks):**
derives first-Submit entry/return and second-Submit entry from the
op-2 spans; emits every timestamp and eight regions R1 [waitReturn,
firstSubmitEntry) .. R6 [nextWaitEntry, nextWaitReturn) with the
caller thread's running/ready/waiting/unknown; a per-thread busy table
with sample counts per RVA class; every caller wait segment with its
blocking site, waker thread, waker ready-stack top and the waker's
class by its LAST SAMPLE before the ready (stale beyond 2 ms), split
per region and cross-tabbed against the top blocking sites; RVA classes
bounded by the decompile sizes (job bodies 503/565/62 bytes, eval 1006,
reset-repopulate 687, record drain 134; the LOD evaluator 0x4331300 is
a 9-byte jmp thunk and will not match; the scheduler range is left
explicit); gate quantities per frame and per window; `--runtime-log`
groups frames by the log's first/last sequence. Per-frame detail goes
to frames.jsonl, report.json stays additive (schemaVersion 1).

**Proof on the old trace** (build\phaseA-cpu-parked\flight.etl, 543 MB,
run with the 081020 log): windows 30 and 31 reproduce all seven phases
to within 0.0009 ms with frame counts equal to the log's valid= (2644,
1116); region sums close to 0.001 µs with unknown 0.000 over 846 x 11
regions; caller running from CSwitch vs samples x 1000 µs interval
(SampledProfileInterval event) agree to 0.017%; ReadyThread stacks
belong to the waker in 26,721 of 26,721 cases; 29 switch-outs with an
unresolved process id were rescued from the thread table (they had left
four threads reading 100% busy). The hand counts of the withdrawal
entry reproduce (265 caller samples, 2693 wakes at 0x5d6dcd, 2973
switch-outs at 0x5d6d7f). Cost 6 s and 400 MB; ETLX 0.62x the ETL, so
a 10 GB approach trace budgets at ~2 min and ~6 GB of ETLX.

**What the old slice shows about the METHOD only** (on foot, doorway,
not gate evidence): pipeline wakers carry time only in R5c (43 µs/frame)
and at the 0x5d7044 scheduler branch; R4 is the pacer (2.4 ms/frame,
wakers stale or in EDVR's openvr_api); R6 is one wait per frame; the
dominant scheduler site 0x5d6d7f splits into 66 DPC-ended ~10 ms waits
and 3117 stale-waker waits. The entry+0x2000 bound of the first pass
had over-claimed (physics 112 -> 0, LOD 206 -> 0, thunks 131 -> 1
samples), which is why the sizes went in before any flight.

ruled out: nothing. The gate is still unmeasured; the instrument is now
proven against the runtime's own accounting.

### 2026-09-22 -- Phase A, valid: KILL. The pipeline is 0.8 ms of the caller thread's 15.1 ms; the wall is draw submission, 3.9 ms of it inside EDVR's own d3d11.dll; the onset is a ramp

Two file-mode legs, one session, build v0.17.0-300-g2d8fbda (log check
exit 0), pid 22184, Pimax Crystal Super on Pimax OpenXR, requested
resolution 0.566 (openxr_resolution line), pacing=0, scheduler_probe
and eye_depth_capture OFF (the gfx log prints no probe or census line),
no capture hotkey pressed. Both traces analysed with --runtime-log:
leg 1 (build\phaseA-parked-2, 2.49 GB, 75 s, 2831 frames, 2830 covered,
eventsLost 0) reproduces windows 15 and 16 of the runtime log to
0.0009 ms; leg 2 (build\onset-approach, 3.52 GB, 103 s, 4424 frames all
covered) reproduces windows 31-33. Region residual 0 in both.

**Leg 1, parked cockpit (~200 m from the settlement point), windows
15/16, 1349/1344 frames, 45.0 fps, cycle 22.25 ms.** Caller thread per
cycle, µs (length / running / waiting): R1 [waitReturn, firstSubmit)
11142 / 10966 / 152; R2-R4 submits 1156 / 357 / 755; R5a-b 472 / 369 /
86; R5c [presentEnd, nextWaitEntry) 4557 / 3361 / 1145; R6 next wait
4920 / 39 / 4878. Caller running 15.1 ms per frame in total: the
thread alone cannot fit the 11.1 ms of a 90 Hz frame, the runtime
delivers at 45 Hz and the caller idles 4.9 ms per cycle in R6. The
pre-submit phase is 98.6% running - not blocking, not back-pressure,
not EDVR parking (C4-waiting is out for the cockpit).

**The gate, measured.** Thread-summed pipeline running (all threads,
samples in the job bodies / thunks / call site / eval / record drain):
5.24 ms per frame - the design's ~5.3 ms reproduced by a second
instrument. Of it on the caller thread: 0.67 ms running (1882 of 42738
caller samples, ALL in R5: record_drain 0.61, batch 0.11, job-0 0.07,
call site 0.06; ZERO pipeline samples in R1) plus 0.14 ms of waits
whose waker's last sample was in the pipeline. Critical-path share
0.155. Recall ceiling x critical-path pipeline: 0.96 x 0.81 = 0.78 ms;
by the design's formula 0.96 x 0.155 x 5.3 = 0.79 ms. The bar is
~1.5 ms. **KILL at Phase A, by the design's own gate, on a valid
measurement this time.** The premise was wrong: the pipeline runs on
seven worker threads at ~0.75 ms each per frame, overlapped, and the
caller thread waits 0.14 ms for it.

**What the caller thread's 11 ms of R1 is** (top-of-stack module of its
14757 R1 samples in window 15, ms per frame): game exe 4.45 (40.7%),
EDVR's own d3d11.dll 3.94 (36.1%), System32 d3d11 0.74, ntdll 0.59,
kernel 0.53, nvwgf2umx 0.50, other 0.2. 46% of R1 samples have an EDVR
d3d11 frame anywhere on the stack; 27% sit on one path, EDVR frames
over game RVAs 0x51bcab < 0x4c82f18 < 0x594ed5 < 0x58f2f4 (a per-draw
or per-state call the game makes through the proxy). R1 is draw
submission. R5c's 3.4 ms running is 77% game code (the record drain,
the call site, the draw-item chain 0x4c81ca0 < 0x4c8227c) with EDVR at
0.09 ms. Window 16 repeats every number within 0.1 ms. Frame
resolution: 1.62 M frames resolved, 73 unresolved, so the module split
is complete.

**Leg 2, the approach (5.35 km 3-D distance at 5.2 km altitude down to
touchdown at 234 m, Status.json joined by UTC, per-second series in the
scratch record).** R1 running is a RAMP: 4.0 ms at 5.35 km, 5.8 at
4.5 km, 6.3 at 3.7 km, 6.9 at 2.0 km, 7.4 at 1.2 km, 8.6 at 1.0 km,
9.0 at 800 m, 9.6 at 430 m, 11.3 at 313 m, 12.7 landed; no
adjacent-second jump above 0.6 ms in 86 s; it tracks the thread-summed
UpdateRenderDataJob samples (1.4 -> 3.9 per frame, r = 0.97) and the
thread-summed pipeline (2.0 -> 5.4 ms). reset_repopulate 0x36A0F50 was
never sampled on any thread in either leg, but this leg cannot test C2:
the game had been reloaded at the settlement at 16:57:09 (journal
LoadGame; ApproachSettlement fired at load-in, not during the descent),
so the collection was admitted before the capture began. The half-rate
throttle engaged at 4.5 km (first 42 of 48 frames over 16 ms) and held
100% from 2.1 km; the "~1 km" the user perceives is where R1 passes
~9 ms (800 m), not where the rate halves.

**Reading against the candidates:** C1 (per-record LOD admission) fits
the ramp and the job-0 correlation; C2 untestable here; C3 out (job 2
never sampled); C4-waiting out (R1 running 98.6%); C4-EDVR-hook is
CONFIRMED as the largest single block (3.9 ms innermost, up to 5.2 ms
with the D3D runtime and driver underneath it); C5 out (no I/O sites).
The admission ramp sets HOW MANY draws the settlement submits; EDVR's
per-draw path sets what each one costs the caller thread.

**Consequence.** The occlusion-culling arc CLOSES at Phase A. The lever
the flight found is EDVR's own per-draw cost on the caller thread: it
is the one block that is ours to cut, and cutting all of it lands the
caller at ~11.2 ms, the edge of 90 Hz (the GPU side is unmeasured in
this trace; addendum 2 above measured 8-15 ms app GPU at 0.7559 scale).
Next, no flight needed: name the EDVR functions behind the 3.9 ms from
this trace (innermost EDVR RVAs from the analyzer + a PDB rebuilt at
2d8fbda whose .text matches the installed DLL), then cut the per-draw
path and remeasure with the same two legs.

ruled out: occlusion culling of the job pipeline, because the pipeline's
critical-path time is 0.81 ms per frame against a 1.5 ms bar (valid
Phase A). ruled out: back-pressure or EDVR parking as the cockpit wall,
because R1 is 98.6% running. ruled out: a step-shaped settlement onset
between 5.3 km and landing, because R1 ramps with no jump over 0.6 ms.
ruled out: the armed instruments as the cause of the 2026-09-22 morning
cockpit numbers, because the clean leg reproduces them (11.1 vs 12 ms).

### 2026-09-22 -- EDVR's per-draw path named; the design re-scoped to draw submission (Sean); the cut begins

**Naming the 3.94 ms.** The parked trace's innermost EDVR RVAs were
symbolized against a PDB from a rebuild of 2d8fbda in a scratch
worktree (analysis in the session's scratch record). Caveat first: the
rebuilt .text differs from the installed DLL in 1.06% of bytes (25,839
of 2,446,336; section sizes and the .pdata function table identical),
so function-level attribution holds but line-level does not for RVAs on
a differing byte (one top-30 entry, the vscreen.cpp:4041 lambda). The
capture tool now keeps the matching PDB beside each trace (commit on
main, this evening) so this never recurs. Population: 5320 of 14757
caller R1 samples have an EDVR leaf frame (3.94 ms/frame; 6826 have an
EDVR frame anywhere); the analyzer's RVA list was capped at 40 rows and
names 2397 of them - the head of a long tail of small functions.

Leaf samples per function (window 15, 1349 frames; ms/frame =
count/1349): hookedMap (vscreen.cpp:2901-2903) 425 + mapWaitNote 114 =
0.40 ms, entered from one game call site on every Map; the per-draw
verdict lambda in forwardWithVerdict under hookedDrawIndexedInstanced
(vscreen.cpp:4041) 271 = 0.20; hookedDevCreate<3,0> = the CreateBuffer
slot (device_hook.cpp:864) 232 = 0.17; __security_check_cookie 221 =
0.16 (/GS cookies on the hot hooks); the guarded<> wrapper (guard.h:60)
153 = 0.11; qpcNow (log.cpp:85) 125 = 0.09, called per draw from the
draw hook; bindingGet + bindingShaderHash (binding_shadow.cpp) 145 =
0.11; hookedPresent 138 = 0.10; edvr::begin (celestial_motion.cpp:423)
88; beginPanelOverride (vscreen.cpp:1666) 85; beforeTone /
uiDeferredBeforeTone (ui_deferred.cpp:582) 80; hookedUnmap 68;
journalGameplay (journal_watch.cpp:554, per draw via
backdropOnComposite) 59; hookedCreateTexture2D 50; readPool 50;
endQuery 46; hashOf (exposure_fix.cpp:363) 44; introProbeWants (per
draw) 39; meshMotionDraw 37; renderBoundaryPresent 32. Two game entry
points carry it: 0x51bcab (the draw hook, several chains converging on
hookedDrawIndexedInstanced, 27% of R1) and 0x50f0e5 (hookedMap). The
shape is the finding: no hot function, a dozen small per-draw
predicates and instruments each run ~18k times per frame, plus the
Map/Unmap hooks on every constant-buffer update. Session state:
temporal_aa off (no NGX module in the trace at all; the "NVIDIA" time
is the D3D11 driver executing the game's own draws), ui_depth on,
engine_motion on.

**Re-scope (Sean, 2026-09-22 evening):** the KILL stands for the job
pipeline as the prize; the same records' draw submission on the caller
thread is the new prize (settlement share ~8.5 ms per frame, from the
approach leg's far-to-parked difference, 91-96% of records unseen), at
the same site (the frustum-reject decision, §3.3) with the same safety
architecture (§3.4) and new gates (design doc §8: B' = the record ->
draw join and the unseen share of the caller's draw time, then C, then
D with the same two legs). Patching the reject decision in memory is
the project's normal practice, not the risk; the oracle is.

**The cut, in flight:** hoist the per-frame-constant predicates
(journalGameplay, introProbeWants, panel-override state) out of the
draw path, instruments free when unarmed (qpcNow, mapWaitNote), thin
guarded<> and mark hot hooks safebuffers, early exits in hookedMap /
hookedUnmap / CreateBuffer / CreateTexture2D before any bookkeeping,
O(1) binding hash per draw, verdict-lambda test order. Measured
afterwards with the same two legs against the analyzer's R1 module
split; the bar is the 3.94 ms itself.

Addendum, the uncapped table (analyzer --edvr-export, all 6826 R1
samples with an EDVR frame keyed by innermost EDVR frame, 1459 RVAs,
symbolized 0 failures): per function, samples and ms/frame -
beginPanelOverride 658 / 0.49 (the single largest), forwardWithVerdict
557 / 0.41 + its verdict lambda 490 / 0.36, hookedMap 432 / 0.32,
edvr::begin (celestial_motion) 249 / 0.18, CreateBuffer hook 228,
/GS cookies 221, hookedDrawIndexedInstanced itself 181, meshMotionDraw
165, guarded<> 158, beforeTone 153, qpcNow 135, gpuFrameCommand 116,
srv0IsPanelSized 115, mapWaitNote 114, screenMotionUiDraw 113,
uiDeferredTraceDrawEnter 105, hookedUnmap 104, bindingShaderHash 97,
bindingGet 97, an unordered_map<u64,u32>::find 96, hashOf 79, then
uiSeparationToneBegin 72, uiDeferredBegin 66, uiDeferredBeforeDraw 66,
journalGameplay 59, staticSurfaceBegin 53, uiDepthDeferredEye 52,
bindingGeneration 51, scrimOnEyeDraw 50, introProbeWants 49,
particleOnDraw 48, screenMotionDraw 48, uiDepthPlanetBegin 44,
Controller::owns 42, billboardVariantFor 39, uiDeferredEnd 37,
quadProbeWants 37, backdropOnComposite 37. By hook entry: the draw hook
(hookedDrawIndexedInstanced) carries 5088 of 6826 (75%), hookedMap
~525, hookedUnmap ~280, CreateBuffer 228, hookedVSSetShader 40. Two
families are retired outright by Sean's rule that fix.ui_depth and
fix.engine_motion do nothing while fix.temporal_aa is off (they feed
the temporal pass and had no consumer in this session): ui_depth's
deferred path (~600 samples, 0.44 ms) and engine motion's draw hooks
(~575, 0.43 ms). The first top-30 table above was the head of this
list; 48% of the samples lay outside it.

### 2026-09-22 -- The per-draw cut, built and gated (commits a49dca4 + a5a668d): the tail was call overhead, not work; NOT FLOWN

**The structural finding.** build.bat compiles with /O2 and no /GL, so
nothing inlines across translation units: every one-line getter that
lives in a .cpp is a real call with a prologue, a frame and a return,
and the draw path asked about forty of them per draw. That, not any
single function, is the profile's long tail. Two corrections to the
entry above follow from reading the code rather than the profile: (1)
fix.ui_depth is not read anywhere in src (config_test asserts it stays
unset: "UI depth is bundled with temporal AA"), and ui_depth's on-state
is derived from temporalModeEnabled(fix.temporal_aa); celestial, mesh
and kinematic motion are configured under g_wanted from
temporal_pass.cpp:7235-7250 - so Sean's rule was already the code's
rule, and the ~1200 samples on those families were the epilogues of
disabled features being CALLED 18-36k times a frame; (2) the
unordered_map::find on the draw path is the shader-hash registry
(hashOf, exposure_fix.cpp:360, under a CRITICAL_SECTION), not the
kinematic tracker, which only runs on the evaluator thread.

**What changed** (all behaviour-preserving; the one timing change is
noted): the forty-term subscriber condition at the top of
beginPanelOverride (658 samples, 0.49 ms) is sampled once per frame
into a published atomic (draw_gate.h) that every arming site also
raises (census hotkey, census_auto, the census_at_ms schedule, the quad
probe, and a settings refresh re-samples), so a feature arming
mid-frame is seen at latest one frame later and a hotkey census never
loses its first frame; bindingGet/bindingGeneration/bindingShaderHash
became inline loads over a published shadow (0.18 ms), with an opt-out
for the eight test rigs that fake the shadow; the quad-skip re-issue
(a D3D11_RECT[16] that put a /GS cookie and a 300-byte frame on all
four forwardWithVerdict instantiations) and noteDeviceCreateFailure's
two char[320] (the same, on every CreateBuffer) moved behind noinline,
which keeps the cookie where the arrays are (no safebuffers anywhere:
on MSVC a function without a GS buffer carries no cookie, so the 221
cookie samples were real buffers); inline live guards in front of
celestial motion (0.18 ms), ui_separation, ui_depth (its mode enum
moved to the header rather than mirrored), mesh motion (its admission
census is read only inside a report that opens with if(!enabled)
return) and screen motion (enabled/failed hoisted with the three
wholesale resets made explicit, screen_motion_test asserts the
coupling); the per-draw DrawClock asks perfMonitorSampleDraws inline
(it samples one frame in 16); the verdict block in
hookedDrawIndexedInstanced tests the draw's shape before the hash and
the temporal predicates; backdropOnComposite tests the shape before
journalGameplay; hookedMap names its eight repeated conditions once;
introProbeWants and quadProbeWants read published flags. The map-wait
instrument's two QueryPerformanceCounter reads per Map now live only
while a native timing context is current - which in a VR flight is the
whole session, so that one saves nothing in the measured scenario and
was kept for its consumer (the native timing line's wait component).

**Left for the flight, with evidence:** srv0IsPanelSized +
bindingResolve (273 samples, 0.20 ms) is a three-COM-call view resolve
per draw because PsSrv0's generation moves every draw - a cache-policy
change that needs a flight; the shader-hash registry lock (hashOf, ~25
per-draw sites) can be cached against the VS binding generation as
ui_depth.cpp:932 and vscreen.cpp:2834 already do, but target_sharp and
panel_upscale (deleted 2026-09-29, 0467e706) read the VS off the context
deliberately, so it is not a
blind substitution; gpuFrameCommand + Controller::owns (158) is the
default-on app GPU timing's foreign-thread check (GetCurrentThreadId
per hooked command), a correctness check left alone.

**Expected saving on paper:** roughly 1.5-2.0 ms of the 3.94 ms of EDVR
leaf time (beginPanelOverride 0.49, the motion and ui families ~0.87,
bindings 0.18, cookies and frames on the two extracted bodies, the
probe and journal calls) - a paper number until the same two legs are
flown against the analyzer's R1 byTopModule EDVR count. Gate: build.bat
with EDVR_PROFILE_SYMBOLS=1 green on the merged tree.

### 2026-09-22 -- Leg 1 re-flown on the cut (build b9c41c0): INCONCLUSIVE, the environment changed three ways; EDVR's per-frame hook cost unchanged by both instruments; beginPanelOverride re-read

build\phaseA-parked-3: 80 s, 3.1 GB, pid 7132, log build v0.17.0-316-
gb9c41c0 (check exit 0), 3006 frames all covered, both PDBs matched by
GUID+age and copied by the capture tool, analyzer symbols enabled
(every EDVR frame named from the matching PDB - the first run of that
path). Reconciliation holds (window 6: 0.001 ms).

**Not a before/after.** The runtime log names a different runtime and
resolution: headset_key steamvr-openxr/steamvr-openxr-aapvr, runtime
SteamVR/OpenXR, requested 0.7559 - every earlier leg ran Pimax OpenXR
at 0.566 (1.78x fewer pixels). The ship was parked at the approach
leg's touchdown point, lat 68.094749 lon 121.076538 heading 195, not
the original pad at lat 68.067474 lon 121.028328 heading 42. The rate
never settled: windows 4-8 read 68 / 52 / 43 / 49 / 56 fps against the
old leg's flat 45.0 for four windows, the next-wait phase collapsed to
0.15 ms (SteamVR does not hold the caller at a half-rate slot the way
Pimax OpenXR did) and the cycle ran flat out at 23.2 ms of caller work.

**What the caller thread did (window 6, 1291 frames, new vs old leg
window 15):** R1 14.33 ms (11.14), running 13.59 (10.97); R5c 6.31
(4.56); caller running per frame 17.9 ms (15.1). By innermost module in
R1, ms/frame new vs old: game exe 5.84 vs 4.45; EDVR d3d11 3.81 vs
3.94; System32 d3d11 1.05 vs 0.74; kernel 1.17 vs 0.53; ntdll 0.72 vs
0.59; nvwgf2umx 0.70 vs 0.50. Thread-summed pipeline 6.06 vs 5.24 ms.
The CreateBuffer hook 359 vs 232 samples, hookedMap 475 vs 425. So the
game-side load rose 30-55% (a different view, more dynamic-resource
churn, the kernel's share doubled under it) while EDVR's own leaf time
stayed within 3%. EDVR's second instrument agrees: the gfx log's
"draw hook CPU" line (EDVR's own time inside its hooks per sampled
frame, one frame in 16) read 3.84-3.98 ms across the old parked leg and
3.70-4.06 ms across this one. Relative to the D3D runtime's own time
under the same draws, EDVR's share fell from 5.3x to 3.6x, which is the
only normalization the data allows and is stated as such.

**A correction to the cut entry, from the named stacks.** The gfx log
prints "vScreen fixes installed: ... panel distance on": s->
distanceEnabled is the FIRST term of beginPanelOverride's forty-term
chain, so the old chain short-circuited after one load and the hoist
saved nothing there. The 658 (now 721) innermost samples are the
function's BODY, which runs for every draw whenever any subscriber is
on - it, and forwardWithVerdict's own inline body (786 innermost
samples, 0.61 ms, the largest single EDVR entry in this leg), are the
next targets. What did vanish from the table: bindingGet/ShaderHash,
introProbeWants, quadProbeWants, journalGameplay, celestial begin,
meshMotionDraw, screenMotion*, and the cookie samples fell from 221 to
~55. What remains, with what grew with the call volume: forwardWith-
Verdict body 0.61, beginPanelOverride body 0.56, hookedMap 0.37,
CreateBuffer hook 0.28, guarded<> 0.22, qpcNow 0.13 (the DrawClock
samples one frame in 16 and spends ~2 ms on THAT frame: a periodic
hitch, to be sampled far less often or removed), beforeTone 0.12
(ui_deferred, a live feature, not temporal-gated - my earlier reading
was wrong), gpuFrameCommand 0.11, hookedCreateTexture2D 0.07.

**Verdict:** the cut's paper saving is not measurable from this leg;
the realized per-frame change is within noise under a heavier scene.
The measurement it needs is the same environment as the 16:50 leg:
Pimax OpenXR at the same render scale, parked on the original pad at
heading 42, the same ini. ruled out: nothing.

### 2026-09-22 -- Leg 1 re-flown on the cut, like for like (build\phaseA-parked-4): EDVR's per-draw cost -0.7 ms per frame (-18%), the caller thread still 15.1 ms, still half rate

build\phaseA-parked-4: 75 s, 2.26 GB, pid 14044, build b9c41c0 (check
exit 0), Pimax OpenXR at requested 0.566 (the baseline's runtime and
scale), parked ~30 m from the original pad at heading 52 (baseline 42),
rate settled at 45.0-46.2 fps for the three windows covering the
recording, both PDBs matched and every EDVR frame named from them.
2855 frames all covered; window 8 (1351 frames) reconciles to 0.0008
ms. Compared with the baseline's window 15 (1349 frames, 16:50 leg):

| caller thread, per frame | baseline | cut |
|---|---|---|
| fps / cycle | 45.0 / 22.25 ms | 45.0 / 22.22 ms |
| R1 length / running | 11.14 / 10.97 ms | 11.17 / 11.03 ms |
| post-present R5c length / running | 4.56 / 3.36 | 4.65 / 3.44 |
| next-wait R6 | 4.92 | 5.00 |
| caller running, whole cycle | 15.13 ms | 15.19 ms |
| thread-summed pipeline | 5.24 ms | 5.25 ms |
| R1 leaf: EDVR d3d11.dll | 3.94 ms | 3.25 ms |
| R1 leaf: game exe | 4.45 | 4.98 |
| R1 leaf: System32 d3d11 / kernel / ntdll / NVIDIA UMD | 0.74 / 0.53 / 0.59 / 0.50 | 0.80 / 0.63 / 0.63 / 0.55 |
| gfx log "draw hook CPU" (EDVR's own hook time per sampled frame) | 3.84-3.98 ms | 2.98-3.17 ms |

**EDVR's own cost fell by 0.7-0.8 ms per frame by both instruments**
(leaf time -0.69, the self-measured hook line -0.8, the
innermost-EDVR-frame population 6826 -> 6008 samples = -0.61). Per
function (innermost EDVR frame, ms/frame, baseline -> cut): gone
entirely - meshMotionDraw 0.12, screenMotionUiDraw 0.08 +
screenMotionDraw 0.04, bindingShaderHash 0.07 + bindingGet 0.07 +
bindingGeneration 0.04, uiSeparationToneBegin 0.05, uiDepthDeferredEye
0.04 + uiDepthPlanetBegin 0.03, introProbeWants 0.04, quadProbeWants
0.03; celestial begin 0.19 -> 0.06; __security_check_cookie 0.16 ->
0.09; uiDeferredTraceDrawEnter 0.08 -> 0.05. Moved rather than removed:
forwardWithVerdict's own body 0.42 -> 0.60 (the inlined binding reads
and live guards are now loads inside it - inlining relocates a load, it
does not delete it); guarded<> 0.14 -> 0.17; beginPanelOverride 0.49 ->
0.51 (the hoist is inert with panel distance on, as the previous entry
found). Unchanged: hookedMap 0.32 -> 0.31 (+ mapWaitNote 0.09), the
CreateBuffer hook 0.17 -> 0.19, the verdict lambda 0.43 -> 0.36, qpcNow
0.10 -> 0.11, beforeTone 0.11 -> 0.10, srv0IsPanelSized 0.09 -> 0.10,
gpuFrameCommand 0.09 -> 0.08, hashOf 0.06 -> 0.05.

**Why the caller thread did not move.** Its R1 running is 11.03 vs
10.97 ms: the game-side draw submission was ~0.7 ms heavier in this
leg (game code +0.53, D3D runtime/driver/kernel +0.2) with the same
pipeline load - the view differs by 10 degrees and 30 m, which changes
the frustum's draw set while the admitted records stay the same. EDVR's
own cost is the like-for-like quantity; the caller's total is not, at
this precision. The frame is still 22.2 ms at half rate with ~5 ms of
slot wait, and the caller still runs ~15 ms of it.

**Where EDVR's remaining 3.2 ms sits (R1, ms/frame):** the draw hook's
own dispatch - forwardWithVerdict body 0.60 + the verdict lambda 0.36 +
beginPanelOverride body 0.51 + hookedDrawIndexedInstanced 0.13 = 1.6;
the resource hooks - hookedMap 0.31 + mapWaitNote 0.09 + hookedUnmap
0.08 + CreateBuffer 0.19 + CreateTexture2D ~0.05 = 0.7; the wrappers -
guarded<> 0.17 + cookies 0.09 = 0.26; per-draw feature calls that are
live - beforeTone and the uiDeferred family ~0.3, srv0IsPanelSized 0.10,
gpuFrameCommand 0.08, hashOf 0.05, qpcNow 0.11 (the DrawClock's 2 ms on
every sixteenth frame).

**Reading.** The cut is real and modest: about a fifth of EDVR's
per-draw cost, a twentieth of the caller thread's frame. A second
round on the dispatch bodies and the Map/CreateBuffer hooks could
plausibly halve what is left, but even removing all 3.2 ms leaves the
caller at ~11.9 ms per frame, over the 11.1 ms of a 90 Hz frame at this
view. EDVR's per-draw cost is necessary to cut and not sufficient: the
draw count itself (the re-scoped cull, design doc §8, or the LOD lever)
is what reaches 90 Hz here. ruled out: nothing new.

### 2026-09-22 -- Per-draw cut, round two: built and gated (a606157), NOT FLOWN; two accounting corrections from the named stacks

Four pieces merged behind the full gate (EDVR_PROFILE_SYMBOLS=1, all
rigs): the verdict ladder, the deferred-UI family and the draw clock
(cd00e4e, f91667f); the Map/Create path and GPU-timing owner check
(440a276); the panel-size resolve cache and two static_surface cookie
blocks (5dad30a); 38 more inline predicates and two object_probe /
particle_fix cookie blocks (10678d6). Two hand-resolved overlaps, both
behaviour-preserving: object_probe.cpp (main's cull-gate lines beside
renamed flags) and ui_deferred (two publications kept in one detail
block: the draw path's uiDeferredMayAct over the file's own enabled /
diagnostic generation, and the Map path's uiDeferredResourceWriteLive
mirror).

**Corrections to the parked-4 table above, from checking each hot
address against the flown DLL:** the draw lambda's 0.36 ms is mostly
NOT EDVR's time - 299 of its 487 samples sit at the return of the
forwarded game draw (call rbx = realDrawIndexedInstanced), i.e. the
runtime and driver executing the game's own draw, which the export's
any-EDVR-frame population attributes to the innermost EDVR frame; the
lambda's own work is ~0.05 ms. And "celestial begin 0.06" was a
ui_deferred static begin() (ui_deferred.cpp:554/461), now behind the
deferred-UI guard. EDVR's leaf time by top-of-stack module (3.25 ms in
parked-4) remains the honest number.

**What changed, expected upper bound (ms/frame, window 8 prices):**
forwardWithVerdict's 43 verdict compares per draw -> one v != kNone
test with the rare bodies in two noinline switches, owner read once
(<= 0.37, order of kResolveProbe's two calls and kBackdrop-before-splash
kept); the deferred-UI family's eight cross-file calls per owner draw ->
one inline test (<= 0.41); the DrawClock -> every 64th draw on the
sampled frame, scaled (the "draw hook CPU" line is now an ESTIMATE and
says so; the 1.8 ms spike on one frame in 16 is gone); stack cookies
moved off noteStaleForward, beginPanelOverride (x2), the draw lambda,
object_probe, particle_fix and two static_surface blocks into cold
functions, verified cookie-free on the built DLL (<= 0.15 in all);
depthProbeWanted and staticSurfaceLive inline guards; 38 xWantsDraws
predicates inline (<= 0.25; three skipped because they call into other
modules); srv0IsPanelSized's four guarded COM calls per draw -> one
guarded private-data tag read (<= 0.29; EDVR now attaches a 12-byte
tag under its own GUID to game SRVs it resolves; a recycled address
reads back untagged, which is why the v0.5.2 pointer-keyed cache bugs
cannot recur); hookedMap/hookedUnmap's ~6 cross-file calls per Map ->
inline guards (<= 0.39); mapWaitNote inline; the CreateBuffer hook's
terrain unknown-write call guarded; Controller::owns caches the thread
id per thread (the TEB route does not compile against this SDK); the
shader-hash memo widened 32 -> 64 entries; uiDepthMotionResourceWritten
inline early-out. Not done, with reasons: gpuFrameCommandMightAct at
the draw hooks (app_gpu_timing defaults on, the guard would only add a
load); hashOf caching at the reader sites (the hot callers are the two
shader-set hooks, hence the memo); no rig can reach srv0IsPanelSized,
so its cache is checked by the flight only.

**Measurement:** the same parked leg, same environment as parked-4
(Pimax OpenXR 0.566, the pad, instruments off), compared on EDVR's
leaf time by top module and the draw-hook line. Paper ceiling of round
two: ~1.5-2 ms of the remaining 3.25; the honest expectation is less,
because inlining relocates loads and the forwarded draws' time is the
game's.

### 2026-09-22 -- Round two measured (build\phaseA-parked-5): EDVR's pre-submit leaf time 3.25 -> 1.69 ms, the caller thread 15.2 -> 13.2 ms per frame; still 45 fps

build\phaseA-parked-5: 75 s, 2.27 GB, pid 22820, build a606157 (check
exit 0), Pimax OpenXR at 0.566, the same pad and heading as parked-4
(lat 68.067238 lon 121.02597 heading 52), windows 5-6 at 45.5 / 45.2
fps, both PDBs matched, 2945 frames all covered, window 6 reconciles to
0.0008 ms. Like for like against parked-4 window 8 (round one) and the
16:50 baseline window 15:

| caller thread, per frame | baseline | round 1 (parked-4) | round 2 (parked-5) |
|---|---|---|---|
| fps / cycle | 45.0 / 22.25 ms | 45.0 / 22.22 | 45.2 / 22.14 |
| R1 length / running | 11.14 / 10.97 | 11.17 / 11.03 | 9.08 / 8.90 |
| R5c length / running | 4.56 / 3.36 | 4.65 / 3.44 | 5.03 / 3.83 |
| R6 next wait | 4.92 | 5.00 | 6.66 |
| caller running, whole cycle | 15.13 | 15.19 | 13.18 |
| thread-summed pipeline | 5.24 | 5.25 | 5.10 |
| R1 leaf: EDVR d3d11.dll | 3.94 | 3.25 | 1.69 |
| R1 leaf: game exe | 4.45 | 4.98 | 4.71 |
| R1 leaf: System32 d3d11 / kernel / ntdll / NVIDIA UMD | 0.74 / 0.53 / 0.59 / 0.50 | 0.80 / 0.63 / 0.63 / 0.55 | 0.73 / 0.53 / 0.61 / 0.51 |
| innermost-EDVR-frame population (ms/frame) | 5.06 | 4.45 | 2.74 |
| gfx log "draw hook CPU" | 3.84-3.98 | 2.98-3.17 | 2.11-2.13 (now an estimate) |

**EDVR's own pre-submit cost is down 57% from the baseline** (3.94 ->
1.69 ms by top-of-stack module) and the caller thread's frame work is
down 2.0 ms (15.2 -> 13.2), with the same pipeline load and the same
game-side leaf time as round one. The runtime still delivers at 45 Hz:
13.2 ms of caller work does not fit 11.1 ms, so the caller idles
6.7 ms per cycle instead of 5.0. Per function (innermost-EDVR-frame
population, ms/frame, round 1 -> round 2): forwardWithVerdict 0.60 ->
0.21; beginPanelOverride 0.51 -> 0.32; the draw lambda 0.36 -> 0.33
(mostly the forwarded game draw, see the correction above); qpcNow
0.11 -> 0.003; beforeTone 0.10, uiDeferredBegin 0.07,
uiDeferredTraceDrawEnter 0.05, uiDeferredBeforeDraw 0.05,
uiDeferredEnd 0.03 -> all 0; mapWaitNote 0.09 -> 0 (inlined, its cost
now inside hookedMap 0.31 -> 0.37); staticSurfaceBegin 0.06 -> 0; the
ui_deferred static begin 0.06 -> 0; __security_check_cookie 0.09 ->
0.01; depthProbeNoteDraw, backdropWantsDraws, headOffsetGateWantsPanel,
drawCensusArmed and the other inlined predicates gone;
srv0IsPanelSized 0.10 -> 0.07 (one guarded private-data read remains).
Unchanged: the CreateBuffer hook 0.19, guarded<> 0.17 -> 0.19,
gpuFrameCommand 0.08 -> 0.09, hookedUnmap 0.08, hashOf 0.05 -> 0.045,
the shader-hash map find 0.075 -> 0.063.

**What remains of EDVR on the caller thread's pre-submit phase, 1.69
ms by top module:** the resource hooks (hookedMap + inlined map-wait
0.37, CreateBuffer 0.19, CreateTexture2D 0.04, hookedUnmap 0.08 = ~0.7)
now lead; then beginPanelOverride's body 0.32, the guard wrapper 0.19,
forwardWithVerdict 0.21, the draw hook 0.09, gpuFrameCommand 0.09,
srv0IsPanelSized 0.07, hashOf + its map 0.11. A third round would chase
the resource hooks and the two bodies; the ceiling is the whole 1.69
ms, which would land the caller at ~11.5 ms - still over 11.1. The
lever is exhausted as a route to 90 Hz at this view; what it bought is
2 ms of caller-thread headroom that the draw-count lever (design doc
§8/§9, gate probe next) will need.

ruled out: nothing new. The round-one accounting corrections stand.

### 2026-09-22 -- Gate probe flown (run 152632): the draw-item builder is the site; at its record unit even ideal occluders reach 0.5-0.9 ms (FAIL); the part unit is the open site (2.5-3.3 ms ideal), unmeasured because the geometry capture came back empty

Session B: one Insert press parked on the pad, build a606157, Pimax
OpenXR; the probe wrote gate_152632.bin (86,760 gate calls, 2,037
builder calls on 679 engine records, nothing dropped), the frame-2
depth pair (file version 2, the camera registers present for the
first time), the pool and instance streams for frames 2-20 and the
draw snapshot. Analysis and the record are the design doc's §10 (on
main, 70e76a3); the reader tools\cull_gate_probe.py had two bugs fixed
on the way (the mask's bits are per view - eye A is view 0 = bit 1,
eye B is view 5 = bit 22 - and shadow-only twin records at the same
positions were being counted against the builder).

**The site, settled by prediction.** Per (record, eye) the builder's
view loop (FUN_1442B4420:250-275) admits exactly the engine records
whose parts are drawn in that eye: 0 drawn slots it rejects, 0
admitted-and-undrawn, 1,062 admitted-and-drawn; the collection mask
rec+0x208 over-admits 8 records per eye (written pool slots, nothing
drawn - the builder's frustum test rejects exactly those); the
frustum gate FUN_14430EFE0 rejects 20,232 drawn (slot, eye) pairs and
so cannot be the admitting test. No test ever gives the two eyes
different verdicts at the record level: the per-eye split happens per
PART in FUN_1442B3FC0 (decompiled, analysis\decomp\decomp_42B3FC0.txt:
a frustum + LOD test on each sub-item's sphere per view, setting the
item's view mask); 217 parts are drawn in one eye only and every one
projects outside the other eye's viewport. Records the builder never
sees (2,213) go through the gate and carry 14.5% of pool draws. Eyes:
view 0 is A, view 5 is B in all three dumps; the frame-2 dump matches
the depth cameras to 0.087 mm and 0.0006 degrees (same frame proven);
the pose entries cannot separate frames 2-4 because nothing moved.

**The prize depends on the unit** (R = 1 m, a draw goes only when every
record in it is rejected in both eyes; ms = share x 8.5, the share as
measured before the per-draw cut):

| unit | depth truth | ideal occluders 256x128 | ideal 256x256 |
|---|---|---|---|
| t33 part, all records | 70.3% / 5.98 ms | 35.0% / 2.97 | 45.9% / 3.90 |
| part, builder parts only (FUN_1442B3FC0) | 59.5% / 5.05 | 29.8% / 2.54 | 38.5% / 3.27 |
| engine record, all parts occluded (the view loop) | 13.2% / 1.12 | 5.9% / 0.50 | 10.7% / 0.91 |
| engine record, its own frustum sphere | - | 2.0% / 0.17 | 3.5% / 0.29 |

False rejects against the depth truth: 0 in every ideal row at every
R and buffer size. So: a record-level reject in the builder's view
loop FAILS the 1.5 ms bar even with perfect occluders (the largest
record has 1,287 parts and one visible part keeps all of them); the
per-part site inside FUN_1442B3FC0 clears it on the ideal ceiling and
is CONDITIONAL on real occluders reproducing >= 59% (256x128) or 46%
(256x256) of the ideal at the 8.5 ms share - and with the per-draw
cut already realized the settlement share is nearer 6.3 ms, which
raises that to roughly 80% / 62%.

**The number is still missing.** The geometry section of the snapshot
came back empty (0 draws mapped): the pool was first seen 60 ms after
the arm, and that path runs releasePool -> ledgerRelease ->
g_eyeMeshSnapshot.reset() -> armGeometry(0) (object_probe.cpp:3535-
3537, 1906, 700), a probe bug, not a declined capture. Without
geometry the solid occluder inventory cannot be built: full meshes
would be 155k triangles per eye (0.4-0.8 ms per eye to rasterize, 3-5x
the 0.3 ms budget); eroded boxes (~2k triangles, under 0.02 ms) need
the inventory. The occludee tests themselves are ~27k per frame at the
part site, ~0.12 ms summed across the workers.

**Verdict:** B' FAILS at the site this capture proves (the record
unit) and stays conditional and unmeasured at the part unit. To close
it: fix the geometry arm in C++; extend the probe to record
FUN_1442B3FC0's per-sub-item, per-view verdicts plus each model's
centre (+0x00) and radius (+0x10); one more parked capture (both keys,
one Insert press). Then the real-occluder recall at the part site
decides Phase C, with the bar restated against the post-cut share.

ruled out: a record-level reject in the builder's view loop (at most
0.91 ms with ideal occluders); the collection mask as the admitting
test (over-admits 8 per eye); FUN_14430EFE0 as the admitting test for
builder records (rejects 20,232 drawn pairs); the first run's 18
"disagreements" (shadow twins); declined draws as the reason the
geometry is empty (a probe bug); full meshes as occluders within
0.3 ms.

### 2026-09-22 -- Probe v2 flown (run 165433): the part site is PROVEN, the occluders are REFUTED - qualified solid occluders remove zero draws; the re-scoped cull FAILS Phase B'

One Insert press parked on the pad, build b706df9, Pimax OpenXR. The
capture held everything the close-out asked for: 109,291 FUN_1442B3FC0
rows (0 unverified, 0 foreign, 0 dropped), the geometry arm survived
the pool re-sighting (19,482 draws, 4,865 distinct meshes decoded and
checked by re-drawing every instance against the stored depth, which
also gives an exact per-instance visibility truth), the frame-2 depth
pair with cameras, the v2 gate dump. Analysis and tables are the design
doc's §12 (on main, d0e6720).

**The site, at the part unit, is proven.** FUN_1442B3FC0's per-(part,
eye) verdicts admit exactly the builder parts' pool draws: 0 rejected-
and-drawn, 0 admitted-and-undrawn, 20,728 admitted-and-drawn, 0
untested; it reaches 85.2% of the pool draws. Of the 219 one-eye
slots, 20 are the part test rejecting the other eye, 2 are slot
copies, and 197 belong to no record the probe captures at all (drawn by
VS 4435F2E50020E7F3, nearest engine record 343-781 m away): whatever
admits those per eye sits outside the builder and the gate.

**The occluders are refuted.** The surface that hides the unseen parts
is 89.6% open opaque panels and shells, 3.5% closed meshes, 6.9%
terrain and the hull. Only 187 of 4,864 meshes are closed, and the top
occluding meshes stay open even after a 5 cm weld; many are one-sided.
§3.2 built from qualified closed solids - raw triangles (a) or eroded
inner boxes (b) - removes ZERO pool draws at both buffer sizes:

| occluders, part unit, both-eyes rule | 256x128 | 256x256 |
|---|---|---|
| truth, exact re-draw | 63.0% = 5.35 / 3.97 ms | same |
| ideal (the stored depth itself) | 19.5% = 1.66 / 1.23 ms | 25.9% = 2.20 / 1.63 ms |
| (a) raw closed-solid triangles | 0 draws | 0 draws |
| (b) eroded inner boxes, 4,022 tris/eye | 0 | 0 |
| bound: every open opaque instance | 14.6% = 1.24 / 0.92 | 20.7% = 1.76 / 1.30 |
| bound: open opaque + terrain | 19.5% | 25.9% = 2.20 / 1.63 |

(ms at the pre-cut 8.5 ms share / the post-cut 6.3 ms share.) The
ratio real/ideal is 0 against the ~45% bar; 0 ms against 1.5 ms. The
bar is steeper at this unit than §9 assumed: even IDEAL occluders at
256x128 reach only 1.23 ms post-cut, and a real set would need 90%
(pre-cut) or 92% at 256x256 (post-cut) of the ideal. The only
representation that reaches the bar is raw open-panel triangles plus
terrain: 0.5-2.3 M triangles per eye, 2-3 ms to rasterize against a
0.3 ms budget that fits 30-60k, and it rests on honouring each draw's
cull mode, which §3.2's soundness argument never covered (a one-sided
panel is not an occluder from behind). False rejects: 0 at slot and
draw level against the exact truth in every row. Occludee tests at the
site: 20,728 per frame, ~0.1 ms. Two smaller findings: the R = 1 m
footprint truth used since §2 is slightly optimistic (400 slots it
calls unseen are visible in the exact re-draw, and 176-644 draws it
would remove under ideal occluders hold a visible slot), and the §10
nearest-origin association names the owning slot for only 13.7% of the
occluding points.

**Verdict: the re-scoped cull FAILS Phase B' on evidence.** With the
occluder representation the design allows (closed solids, eroded
boxes) the prize is nil; with the one that would work (open panels +
terrain) the cost is ten times the budget and the soundness is
unproven. The one remaining offline question - a quads-on-panel-faces
representation plus terrain, measured on 165433 within a 30-60k
triangle budget - needs no flight and is the last thing that could
reopen the arc; its ceiling even if perfect is the ideal row, ~1.2-1.6
ms post-cut, for a per-frame software rasterizer on the workers with a
correctness argument still to be made. Recommendation: close the arc.

ruled out: closed-solid occluders, because their raw triangles remove
0 draws; eroded inner boxes, because they remove 0 draws; the R = 1 m
footprint as a zero-violation truth, because 400 slots it calls unseen
are visible; the §10 nearest-origin association as occluder identity
(13.7% of points); the reader's 120 m range as occluder selection
(0.0-0.2%); the part test as what admits the 197 unclaimed one-eye
slots.

### 2026-09-22 -- The GPU side of the same parked leg, read from the perf monitor: 9.2-10.2 ms per frame at 0.566; the direction after the cull's death

The parked-5 session's gfx log (edvr_gfx_20260922_152002.log) carries
the perf monitor's per-frame application-render GPU lines during the
leg (21:22:57-21:23:17 UTC): render 9.19 / 10.18 / 10.23 / 9.70 / 9.84
ms (valid pairs, age 15 ms), and the native metrics history's producer
9.77-9.89 ms with the runtime's predicted period at 11.111 ms. Pimax
Crystal Super at requested 0.566 (about 3070x3032 per eye, ~18.6 MP
both eyes). So at this scale the GPU sits within ~1 ms of the 90 Hz
budget while the caller thread sits 2 ms over it; at the 0.7559 scale
of addendum 2 it was over on its own (8-15 ms). Any 90 Hz target here
needs both: the caller thread from 13.2 to ~10 ms and the GPU held at
or under ~10.

**Direction (2026-09-22, after the cull closed):** (1) the LOD lever at
the proven per-part site - measure its elasticity offline on run
165433 first (the part rows carry sphere, LOD result and view
distance; a bias on the screen-size threshold is the engine's own
mechanism continued below the slider's floor; visual cost stated per
factor); (2) a third per-draw round on the resource hooks,
beginPanelOverride's body and the guard wrapper (ceiling ~1 ms); (3)
the GPU read beside any CPU saving, as above; (4) 45 Hz with
reprojection as the honest fallback where the budget is not met, which
is what EDVR's per-object motion vectors serve. Both (1) and (2) are
running as offline work; nothing needs a flight until one of them has
a number.

### 2026-09-22 -- LOD-bias elasticity measured offline on run 165433: the screen-size half is a weak lever (<= 0.4 ms), the slider's half is the real one and needs the LOD tables to price beyond its floor

Design note docs\design-settlement-lod-bias-2026-09-22.md (on main,
44c59af) and `tools\cull_gate_probe.py --lod-bias k[,k...]`
[--settlement-ms 6.3,8.5]. The per-part test FUN_1442B3FC0 reproduced
from the decompile and the capture: a part passes a view iff (1) its
sphere spans at least one pixel, 0.5*(A*d + B) <= r with A = view+0x550
= 1/fy (0.000834297 here) and B = 0 for the eyes; (2) the frustum
(FUN_1404F4E10); (3) the LOD distance f = A*(d - r)*s + B <= t0, with s
= ctx+0x30 and t0 the first float of the part's 0x80-byte LOD table;
the LOD nibble is the first i with f <= t[i+1]. Terms (1) and (2)
never reject a part the engine passed (0 mismatches over 109,291
rows); the only mismatch class (8,355 rows) is term (3), whose tables
the capture does not hold, so the nibbles cannot be reproduced and the
LOD-distance form is bounded, not priced.

**Elasticity of the screen-size form** (exact join, 18,267 pool eye
draws, both-eyes rule; ms at the 6.3 ms post-cut share): k = 2 removes
6 draws; k = 3, 40 (0.01 ms); k = 6, 243 (0.08); k = 8, 613 (3.4%,
0.21 ms); k = 10.46 is the last factor whose removed parts all stay
under 0.25 degrees, 1,089 draws, 0.38 ms. What goes: small distant
details beyond ~150 m (radius <= 1.3 m, angular size <= 0.19 deg at
k = 8), never a building; visible parts among them: 2 at k = 3, 24 at
k = 8 (2.5% of removed slots). A LOD shift changes nothing here: 99.0%
of admitted parts sit at nibble 3 or 4 and those draw the same mesh
in 149 of 151 models. ruled out: a screen-size bias as the settlement
lever, because it tops out near 0.4 ms against the 1.5 ms bar.

**The slider's form.** LODDistanceScale does not touch view+0x550/
+0x560 (the projection's pixel size); it enters ctx+0x30 = 2 -
LODDistanceScale (identified by the settings offset +0x124, not traced
end to end; the capture's s = 1.0 matches the config's 1.0). Its floor
of 0.1 is s x 1.9 and moves only term (3). Bounds on 165433 at the
floor: EB52 0.1..34.2% (measured pass C: -16.9%), 2684 3.1..42.0%
(measured -14%), 8056 0.0..2.2% (measured immune - the one tight
agreement), all pool eye draws 0.1..27.2% (measured -18.7%). Every
measured value sits inside its bound. So the game's own slider at its
floor removes ~18.7% of the settlement's eye draws, about 1.2 ms on
the caller thread post-cut, at the game's own low-LOD look; an
EDVR-side continuation below the floor (s > 1.9, one float in the
render context) is the lever with teeth, unpriced beyond the floor
and without a 0.25-degree cap (at k = 1.25 the certain removals
already reach 0.74 degrees and a 4.2 m radius). Pricing it exactly
needs the probe to record each LOD table once per table pointer and
one parked capture.

**Direction:** the cheapest next number costs no code: one parked leg
at LODDistanceScale 0.1 (the floor) with the same analyzer and the
GPU line, to measure the slider's own effect on the caller thread and
the GPU at this view; then decide whether the beyond-the-floor bias is
worth its visual cost.

### 2026-09-22 -- Per-draw cut, round three: built and gated, NOT FLOWN; parked-5 re-read against the flown DLL

**The accounting, corrected from the flown binary.** The per-function
figures of the parked-5 entry are the innermost-EDVR-frame population
(3708 samples, 2.74 ms/frame), and a sample whose innermost EDVR frame
sits at the return of a call out of the module is time in the callee.
Classifying every hot RVA of window 6 against the flown d3d11.dll
(the pre-b706df9 backup, PDB GUID 50D9C55F confirmed) gives: 0.67
ms/frame is the game's own forwarded D3D calls -- hookedMap 186 samples
at the return of realMap, the CreateBuffer hook 261 of its 262 at the
return of the real CreateBuffer, hookedUnmap 85, CreateTexture2D 49 (all
of it), the draw lambda 249 at the forwarded draw; 0.13 ms is the
map-wait instrument's QueryPerformanceCounter pair and 0.085 its two
locked adds (the log's own count: 3,110 Maps a frame, not 1,100);
guarded<> is the COM inside it (bindingResolve's GetResource/GetType/
Release 131 samples, srv0IsPanelSized's GetPrivateData 103), the guard
itself ~9; and the 85-sample unordered_map::find is ui_depth's holo
geometry map, asked twice on every Unmap, not the shader registry,
whose cost is its critical section (55 of hashOf's 61). hookedMap's own
tee work was ~21 samples. So EDVR's true own-instruction remainder
after round two is nearer 1.0 ms than 1.69.

**What changed (all exact unless noted):** the Unmap path's holo finds
behind a published "geometry tracked" flag (set at the one insertion,
recomputed after the frame boundary's pruning) plus an empty() test;
mesh-motion and static-surface write calls behind their own first
tests; hookedMap's eight D3D11_BUFFER_DESC blocks into noinline
mapBufferDesc (the cookie left hookedMap); srv0IsPanelSized split into
an inline fast path and noinline srv0IsPanelSizedSlow (the cookie now
only on a generation miss); per-draw calls behind their own first tests
inline -- objectProbeOnEyeDraw, depthProbeNoteEyeDraw, particleOnEyeDraw,
witchspaceStarsSkip, particleOnDraw (shape + held VS hash), and the
shapes of night vision, the glare train, RemLok, holo, scrim and both
backdrop halves; introPanelWants and deviceHookFssModeLatch asked after
the cheap terms; resolveBind's shadow "No" inline; glitchFrameIsSceneDraw,
panelCurveWants and fssDumpWantsDraws forced inline (MSVC had declined the
last two inside beginPanelOverride); gpuFrameCommand's owns() forced
inline with a guard-free cached thread id; the shader memo 64 -> 1024
slots, Fibonacci-indexed, and shaderRegistryGeneration inline;
forwardWithVerdict's pureDraw closure (which pinned self/kind/count in
memory on every draw) replaced by a noinline function taking values.
One non-exact change: the wake pulse's Rtv0 resolve is kept per binding
generation (successes only; failures retried per draw as before), the
bargain rtv0Eye already makes. Paper ceiling ~0.5 ms/frame of EDVR's
1.69 ms leaf; honest expectation less.

**Not done, with reasons:** the map-wait atomics (cross-thread totals,
kept); a per-resource Map interest flag (the tees are 0.015 ms/frame);
a lock-free registry (it is written at every shader creation for the
whole session, so a snapshot needs reclamation; the memo absorbs the
lookups); scrim/holo per-draw resolves (their generation moves every
draw; the tag route is a measured follow-up). The memcpy (0.046) is
particleCapture on each Unmap of the particle cb1 (5,376 bytes read back
from a WRITE_DISCARD mapping), plus particleCaptureCb0 copying the cb0
ring into g_shadow0, which nothing reads -- per Map/Unmap of those two
buffers, not per draw; left for Sean.

Measurement: the parked-5 leg, same pad and heading, compared on R1
byTopModule EDVR and on this entry's two populations (own instructions
vs forwarded calls).

### 2026-09-23 -- Settings leg (build\phaseA-parked-6): INCONCLUSIVE - two knobs moved at once, no census; the caller thread got 0.7 ms SLOWER and the pipeline 8.6% heavier

Flown on build b706df9 (the round-two DLL plus the unarmed probe),
Pimax OpenXR 0.566, the same pad and heading as parked-5, windows
10-11 at 44.3 / 44.6 fps. The game's Custom.4.4.fxcfg was rewritten at
00:37:30 UTC, four minutes before the leg, to LODDistanceScale 0.55,
MaterialQuality 0, SurfaceMaterialQuality 2 - not the slider floor of
0.1 that pass C used on 2026-09-21 (which kept MaterialQuality 3 and
SurfaceMaterialQuality 2 and measured the draws with the census: EB52
-16.9%, all pool eye draws -18.7%). No census was pressed here, so the
draw count of this leg is unknown.

| caller thread, per frame | parked-5 (1.0 / 3 / 2) | parked-6 (0.55 / 0 / 2) |
|---|---|---|
| cycle / fps | 22.14 ms / 45.2 | 22.4-22.6 / 44.5 |
| R1 length / running | 9.08 / 8.90 | 9.83-9.88 / 9.30-9.46 |
| R5c length / running | 5.03 / 3.83 | 5.29-5.39 / 3.71-3.86 |
| caller running, whole cycle | 13.18 | 13.87 |
| thread-summed pipeline | 5.10 | 5.54 |
| R1 leaf: game exe / EDVR / sys d3d11 / kernel / NVIDIA | 4.71 / 1.69 / 0.73 / 0.53 / 0.51 | 5.11 / 1.67 / 0.78 / 0.61 / 0.52 |
| gfx "draw hook CPU" | 2.11-2.13 | 2.06-2.23 |
| app GPU (perf monitor) | 9.2-10.2 | 9.8-11.6 (one 15.7 spike at the settings change) |

Reading: with these settings the settlement's admitted set grew (the
job pipeline +8.6%, the game's own pre-submit code +0.4 ms) and the
caller thread ended 0.7 ms slower, while EDVR's own leaf time was
unchanged (as it should be on the same build). Either MaterialQuality 0
changes the pool's records or layers in a way that costs more than the
LOD scale saves, or the settings had not fully settled (windows 5-7,
right after the change, read 28-34 fps), or the LOD-distance mapping
s = 2 - LODDistanceScale from the LOD note is the wrong sign - the
capture cannot say which, because the one number that separates them
(the eye-draw count) was not taken. What it does say: lowering these
two knobs together did not buy caller-thread time at this view.

**The controlled leg it needs (leg C):** LODDistanceScale 0.1 ALONE
(MaterialQuality 3, SurfaceMaterialQuality 2, as pass C), settled for
a minute, one NUMLOCK census press during the capture for the eye-draw
count, on the round-three build; read against leg A (the same build at
1.0 / 3 / 2) on the caller thread, the pipeline, the draw count and the
GPU line. ruled out: nothing - this leg measures two knobs and no
count.

### 2026-09-23 -- Leg C (build\phaseA-parked-8, round-three build, LODDistanceScale 0.001 with MaterialQuality still 0): draws UNCHANGED at 18.9k, the caller thread 11.9 ms, EDVR's leaf 1.19 ms, 50-52 fps

Flown on the installed round-three build 8c33258 (check exit 0),
Pimax OpenXR 0.566, the same pad and heading (lat 68.067238 lon
121.02597 heading 52), 18.5 s recorded (452 MB; 599 frames, 598
covered; windows 7-8 partially). The fxcfg at launch: LODDistanceScale
0.00100 (set by hand), MaterialQuality 0 (left from the previous
leg), SurfaceMaterialQuality 2. Leg A (the same build at 1.0 / 3 / 2)
was NOT flown, so the game-side changes below cannot be attributed
between the two knobs; EDVR's own leaf time can, because it does not
depend on them.

| caller thread, per frame | parked-5 (round 2, 1.0/3/2) | parked-8 (round 3, 0.001/0/2), window 7 (304 frames) |
|---|---|---|
| runtime windows' fps | 45.2 | 50.1-51.6 (the trace's own frames: cycle 22.0 ms) |
| R1 length / running | 9.08 / 8.90 | 7.96 / 7.81 |
| R5c length / running | 5.03 / 3.83 | 4.39 / 3.29 |
| R6 next wait | 6.66 | 8.33 |
| caller running, whole cycle | 13.18 | 11.88 |
| thread-summed pipeline | 5.10 | 4.91 |
| R1 leaf: game exe / EDVR / sys d3d11 / kernel / ntdll / NVIDIA | 4.71 / 1.69 / 0.73 / 0.53 / 0.61 / 0.51 | 4.20 / 1.19 / 0.71 / 0.52 / 0.47 / 0.47 |
| gfx "draw hook CPU" (estimate) | 2.11-2.13 | 1.52-1.60 |
| app GPU (perf monitor) | 9.2-10.2 | 8.9-9.5 |
| census eye draws per frame | ~18.3k (2026-09-19..21 flights) | 18,808 / 18,934 / 18,931 (three censuses) |

**Round three works:** EDVR's pre-submit leaf time 1.69 -> 1.19 ms
by top module (3.94 at the baseline: -70% in three rounds) and the
draw-hook line 2.11 -> 1.55, on the same draw count; that is
independent of the settings.

**The LOD setting did not remove draws at this view.** At
LODDistanceScale 0.001 the census still counts 18.8-18.9k eye draws
per frame, the same as at 1.0. This is what the LOD note predicted for
this range: 99% of admitted parts sit at the last two LOD levels and a
lower level is a different mesh, not a removed draw; the 2026-09-21
pass C's -18.7% was measured at another pose. ruled out:
LODDistanceScale as a draw-count lever at the parked cockpit view,
because 0.001 leaves the count at 18.9k.

**What the settings did buy, unattributed:** the game's own pre-submit
code -0.5 ms and its post-present code -0.5 ms on the same draw count,
the pipeline -4%, the GPU -0.5 ms - cheaper meshes and materials per
draw, from LOD 0.001 and/or MaterialQuality 0. With round three's
-0.5 that puts the caller thread at 11.9 ms, 0.8 ms over a 90 Hz
frame, and the runtime's rate moved from a steady 45 to 50-52 (it
alternates between full and half rate when the app sits just over the
period). Leg A at 1.0 / 3 / 2 on this build would split the game-side
saving between the two knobs; MaterialQuality 0 is a large visual
cost, LOD 0.001 at this view a smaller one.

**Where the last millisecond is:** EDVR's remaining 1.19 ms leaf plus
the instruments that are armed all session (the map-wait timer's QPC
pair and locked adds ~0.2 ms; the default-on app GPU timing's per-
command owner check ~0.1 ms) - making those armed-only and a fourth
round on the resource hooks is ~0.4-0.6 ms; the rest is the game's.

### 2026-09-23 -- The settlement LOD governor, SHADOW MODE, built and gated (a081900), NOT FLOWN

Sean's design (2026-09-23): one slider, tuned for kilometres before
settlements existed, cannot serve both regimes; a governor should scale
the LOD-distance term by context - k = 1 in space, k > 1 only at a
settlement and only while the frame does not fit - with hysteresis so
it never pumps (the settlement-flicker failure class). Built in shadow
only: src\d3d11\lod_governor.{h,cpp}; nothing acts. Design section 8 of
docs\design-settlement-lod-bias-2026-09-22.md.

**Signals**, once per frame on the caller thread at vScreenFrameBoundary:
density = the draw-item builder's record count and the part-test count
of the previous frame (one increment per call in the builder bracket
and the part relay, summed per worker); frame work = the runtime's
applicationMs (the perf monitor's "app CPU": pose-wait end to submit
plus the eye treatments), budget = 1000 / baseDisplayHz (11.111 ms at
90); the engine's own s = ctx+0x30 read at the builder. **Policy**
(constants): k in [1, k_max] in 0.05 steps; up one step when a frame
has >= 200 builder records and the last 30 valid samples each ran
> 0.30 ms over the period; down one step after 30 samples each > 1.0
ms under it; at most one step per second; straight back to 1 after 30
frames under 150 records. **Shadow computation** on the workers: after
each engine part test, with the engine's own d, r, A, B, s and the
part's LOD table, recompute term 3 at s*k (the engine's SSE
approximations reproduced; a part counts only when the k = 1 recompute
reproduces the engine's pass and level, else it is a logged
disagreement): would-drop, would-change-level, and an angular-size
histogram (< 0.25, 0.25-0.5, 0.5-1, >= 1 deg) of the would-drops; the
record-level test FUN_144308B30 is reachable from the builder bracket
without a new patch (inputs rec+0x240/+0x280/*(rec+0x20), the views
through ctx+0x1A840; results rec+0x208/+0x210/node+0x6A) and is
recomputed per eye (a lower bound: a parent's lost view removing its
children is not modelled). Eyes are named by view bit (B = 0 and the
finest A; bits 1 and 22 in 165433 - eye B moved from array index 5 to
6 between frames, so the index cannot be used). **Also:** EDVRGATE v3
records each part's 0x80-byte LOD table once per table pointer when the
probe is armed, and the reader (--tables) makes --lod-bias exact for
rows that name their table - the LOD note's missing number.

**Keys:** fix.settlement_detail = game (default; nothing observed) |
auto (this build: shadow only, "never acts" in the configure line) |
reduced (reserved, behaves as auto, logged as such);
advanced.settlement_detail_max = k_max, default 2.0, held to 1..4.
**Log:** a configure line; a step line at most every 5 s; every 30 s a
header, one line per eye and one for the other views with k, its
min/max, the signals' means, would-drop per frame mean/max, would-
change-level, the histogram; a window with k stuck at 1 says why; off
= silent. **Cost:** part observer 14-17 ns and record observer 21-35 ns
per call on the workers (~0.5-0.6 ms of worker CPU per frame at
165433's 34k part tests, plus ~0.1-0.2 ms for the part relay), a few
µs per frame on the caller; with `game`, nothing (the part patch is not
installed unless the probe or the governor attaches). Gate: 76-check
rig, config contract 259 keys.

**First flight (shadow):** one parked leg with settlement_detail =
auto at 1.0 / MaterialQuality 3, plus a short approach; read the 30 s
summaries: does k rise at the pad and fall on the way out, how many
parts would drop per frame at the k it settles on and in which angular
bins, the disagreement count (must be ~0), and the worker-side cost
in the analyzer's per-thread table; with a census press, the draw
count for the removal fraction.

### 2026-09-23 -- Shadow flight 1 (build\gov-parked-1): the recompute reproduces the engine exactly, k never moved because its signal is the pre-submit phase only, the engine saturates at s = 1.5, and the LOD tables make the elasticity exact: k_max 2 = 19% of draws, k 3 = 34%

Parked on the pad (heading 52), build e601452 (check exit 0), 74 s,
2758 frames all covered, windows 5-7 at 42.6 / 45.5 / 46.3 fps. The
settings at launch were still LODDistanceScale 0.001 and
MaterialQuality 0 (not restored), which turned out to be useful: the
probe's view dump shows the engine holding s = ctx+0x30 = 1.5, not the
~2.0 the LOD note's "2 - slider" mapping predicts, while at 1.0 it held
1.0 (run 165433). So the slider's effect saturates at 1.5 - Sean's
clamp suspicion, in one number - and the pool eye draws at s = 1.5
are 16,797 per frame against ~18.3k at s = 1.0 (-8%; the census's
18.6k counts non-pool draws too).

**The governor** logged its configure line and every 30 s summary, k
sat at 1.00 for the whole leg, and the summary says why: "frame work
8.37-8.67 ms mean vs period 11.11 ms". Its signal is the runtime's
applicationMs, the pre-submit phase plus the eye treatments; the
runtime's own cycle instrument for the same windows reads before-first
8.24, post-submit 5.31, next-wait 7.57, cycle 21.99 - the caller's
work per cycle is 14.4 ms and the frame misses the period, invisible
to a signal that stops at the first submit. Being fixed: the runtime
crosses callerWorkMs = cycle - next-wait, the governor reads it, the
policy is unchanged. Everything else held: 0 disagreements over
~1.4 M part recomputations and ~30k record recomputations (the shadow
model IS the engine's test), builder records 679 and part tests
~33k per frame at the pad, eye A = view bit 1, eye B = bit 22; EDVR's
caller-thread leaf unchanged at 1.18 ms (window 6) and the worker-side
cost invisible against the pipeline's 5.2 ms.

**The exact elasticity, from the recorded tables** (EDVRGATE v3, 7
tables covering all 97,008 part rows, 0 disagreements on the 79,151
rows the other two terms pass; the dominant table t0 = 0.2737 with
levels 0.0128 / 0.0257 / 0.0513 / 0.1026, the building shells' t0 =
21.38 which never drops; frame 2, 16,797 pool eye draws, the
both-eyes rule; k multiplies the engine's s of 1.5):

| k (effective s) | draws removed | share | ms at 6.3 / 8.5 |
|---|---|---|---|
| 1.25 (1.875) | 318 | 1.9% | 0.12 / 0.16 |
| 1.5 (2.25) | 1,123 | 6.7% | 0.42 / 0.57 |
| 2 (3.0) | 3,222 | 19.2% | 1.21 / 1.63 |
| 3 (4.5) | 5,634 | 33.5% | 2.11 / 2.85 |
| 4 (6.0) | 5,662 | 33.7% | 2.12 / 2.87 |

Removal saturates near 34%: the remaining parts belong to tables whose
cutoff sits at kilometres. What goes at effective s = 3: parts of the
dominant table beyond ~109 m (f = A(d - r)s > 0.2737) and the small
props' table (t0 0.077) beyond ~31 m - the 100 m scale Sean named. The
screen-size form stays negligible (32 draws at k = 4). Because the
governor multiplies whatever s the game holds, k_max must be read
against it: from s = 1.0 the same removal needs k = 3 (s = 3) and 4.5;
from s = 1.5, k = 2 and 3. The default k_max of 2.0 is therefore too
low at the game's maximum setting; 4 (the key's ceiling) reaches the
saturation from either.

**Reading:** the governor's acting mode has a real prize at this view -
1.2 ms at effective s = 3, 2.1 ms at the saturation - which, on top of
leg C's 11.9 ms, is the difference between 45 and 90 Hz here, at the
cost of detail beyond ~100 m. ruled out: the "s = 2 - LODDistanceScale"
mapping below the slider's floor, because 0.001 yields s = 1.5. ruled
out: applicationMs as the governor's frame signal, because it omits
the post-submit phase and reads 8.5 ms in a 22 ms cycle.

### 2026-09-23 -- Shadow flight 2 (gov-parked-2, fa6565b): the caller-work signal works, k 1 -> 4, the exact table confirmed in parts; why the HUD reads under 10 ms CPU at 50-55 fps

Parked at the Cranfield settlement, same spot and settings as shadow
flight 1 (LODDistanceScale 0.001, MaterialQuality 0), 02:04-02:09 local
(the gfx log is local time, UTC-6; the openxr log is UTC). Build
v0.17.0-365-gfa6565b (edvr_log.py --expect-build HEAD exit 0). Logs:
edvr_gfx_20260923_020437.log, edvr_openxr_20260923_020439_013_4144.log,
Frontier install. fix.settlement_detail = auto (shadow only, never
acts), advanced.settlement_detail_max unset (compiled default 4.0).
Sean's HUD perf monitor read CPU and GPU both under 10 ms while fps sat
at 50-55 -- explained below.

**The signal works.** Configure line at 02:04:37, k in [1, 4.00], both
hooks hooked; the one-off line at 02:04:41 reads `settlement detail:
frame work = caller work per cycle (runtime timing v5: ...)` -- 73bfab3's
fix is in force, not the applicationMs fallback that stalled k at 1 in
shadow flight 1. Caller work absent was 0 in every settlement window; s
held at 1.500 in every window, matching shadow flight 1's floor reading.
Records/frame rose from 474.8 while still loading in to a steady ~680
(>= 200 on nearly every frame from 02:07:08 on); part tests 20.2k ->
27.8k -> ~33-34k.

**30 s summaries** (window end local; mean caller work vs. the 11.11 ms
period; over/under-budget sample counts; k; effective s x k):

| end | frames | caller work ms | over/under | k | s x k |
|---|---|---|---|---|---|
| 02:06:38 | 689 (loading) | 8.82 | 197/410 | 1.00->1.25->1.20 | 1.80-1.88 |
| 02:07:08 | 1793 | 11.38 | 956/550 | ->1.50 | 2.25 |
| 02:07:38 | 1564 | 12.76 | 1404/1 | 2.20 | 3.300 |
| 02:08:08 | 1558 | 12.47 | 1364/0 | 3.00 | 4.500 |
| 02:08:38 | 1690 | 11.80 | 1243/149 | 3.40 | 5.100 |
| 02:09:08 | 1624 | 12.34 | 1377/0 | 4.00 | 6.000 |

Pre-settlement windows (02:05:08-02:06:08, menus/loading, no builder
records): caller work 2.1-2.4 ms, far under the period, as expected. At
02:09:12, k reset 4.00 -> 1.00 after 30 frames under 150 records (Sean
left the view): the reset path works.

**Ramp timing.** First step up at 02:06:26 (11.67 ms vs. 11.11); two
down excursions at 02:06:34 (7.33 ms) and 02:07:02 (9.46 ms) while the
load settled; then monotone up to 4.00 at 02:09:00. 1.25 -> 4.00 took
142 s, about one step per 2.6 s -- slower than the one-step-a-second cap
because each step needs 30 consecutive over-budget samples and any
under-budget sample restarts the run. Not a fault; see the checklist
verdict below.

**Runtime cycle instrument** (openxr log; its windows end 11 s before
the governor's), means in ms:

| end (local) | cycle | before_1st_submit | post_2nd_to_wait | next_wait_roundtrip | caller work (cycle - wait) | fps |
|---|---|---|---|---|---|---|
| 02:06:57 | 20.17 | 6.50 | 4.75 | 8.37 | 11.80 | 49.6 |
| 02:07:27 | 17.05 | 6.73 | 4.44 | 5.35 | 11.70 | 58.7 |
| 02:07:57 | 19.79 | 7.53 | 4.63 | 7.08 | 12.71 | 50.5 |
| 02:08:27 | 17.67 | 6.97 | 4.27 | 5.88 | 11.79 | 56.6 |
| 02:08:57 | 18.25 | 7.21 | 4.36 | 6.14 | 12.11 | 54.8 |

The same figure by construction as the governor's offset windows
(11.38 / 12.76 / 12.47 / 11.80 / 12.34), agreeing within the window
offset. Every window admitted = valid; no invalid cycles.

**Why the HUD read under 10 ms CPU at 50-55 fps.** The perf monitor's
CPU figure is NativeTimingSnapshot::applicationMs, the pre-submit phase
only (game_before_first_submit ~7 ms, plus the eye treatments); it
omits the ~4.4 ms the game spends after the second submit before its
next pose wait. The caller thread's real work is 11.7-12.7 ms a cycle,
over the 11.11 ms period on 80-90% of samples, so most cycles take two
90 Hz slots (p50 cycle ~21 ms = 45 fps) and some fit one, giving the
50-58 fps mean. GPU under 10 ms is consistent with the earlier
9.2-10.2 ms measurement and is not the limiter. Follow-up, NOT done: the
perf monitor should show caller work per cycle (timing v5 carries it)
instead of, or beside, the pre-submit figure.

**Per-eye, saturating exactly as the table predicts.** Window ending
02:08:08 (s x k 3.3 -> 4.5): tested 10,528 / 10,528, passed 9,749 /
9,768, would drop 4,705 / 4,704 (48% of passed), >= 1 deg 1.316M
would-drops (845/frame). Window ending 02:09:08 (s x k 5.1 -> 6.0):
passed 9,536 / 9,616, would drop 4,703 / 4,798 (49-50%), >= 1 deg 1.45M
/ 1.49M (894/919 per frame). This matches the exact draw table's
33.5% -> 33.7% ceiling (reader run 012514): the would-drop share among
PARTS saturates the same way the draw share does, because the dropped
parts are the low-draw ones -- the building shells' tables (t0 21.38)
never drop. 0 disagreements with the engine at k = 1, every window.

**Checklist verdict** (design-settlement-lod-bias-2026-09-22.md section
8's "what the next flight must show", now "what the second flight
showed"): items 1, 2, 4, 5, 6 met exactly as specified. Item 3 met with
a timing caveat: k reached 4.00 in 142 s, not ~60 s, because the
30-consecutive-sample rule outlasts the one-step-a-second cap when the
load settles first (the two down excursions above); its mean matches
the runtime instrument as required. Item 7 (census eye draws vs. a
`game` run) was not evidenced this flight. Item 8 (optional EDVRGATE v3
capture) was not run; the match above uses the existing 012514 tables.

**What it means.** Acting at k = 3 (s x k 4.5) would remove ~33.5% of
the pool eye draws, ~2.1 ms at the post-cut share, taking caller work
from ~12.3 to ~10.2 ms -- under the 11.11 ms period with ~0.9 ms margin.
k = 2 (s x k 3.0) removes 19.2% (1.2 ms), to ~11.1 ms: borderline.
Visual cost at s x k 4.5: ~4.7k parts/eye/frame dropped, 18% of them
>= 1 deg of angular radius (a 1.75 m radius at 100 m), 41% between 0.5
and 1 deg; at s x k 3.0: ~2.5k parts, 10% >= 1 deg. Whether to build the
acting mode is Sean's call.

**Proposal, not a decision:** write s x k into the engine's LOD scale
(ctx+0x30, the float the slider's setter FUN_142819D90 writes) once a
frame on the caller thread, keep the shadow counters as the gate, and
keep a shadow copy so the game's own s can be told from EDVR's write
when the user moves the slider. Open for an implementer: whether ctx is
one persistent object (pointer constant across builder calls) and where
the game re-writes it.

2026-09-23 -- carried from the Status block: the acting mode (built
on 4200182, merged in 1404b1b/b55e06b, NOT FLOWN) is a bracket on the
slider's setter FUN_142819D90 that scales the engine's LOD scale
(ctx+0x30) by k right after it stores the value each frame; writing
ctx+0x30 directly at the builder site was considered and REFUTED
because the setter runs every frame
(docs\design-settlement-lod-bias-2026-09-22.md section 9).
fix.settlement_detail = auto governs k; reduced holds k_max at once
at a settlement; advanced.settlement_detail_observe = 1 keeps the
prior shadow-only behaviour. The caller-work signal is
EdvrNativeTimingFrame::callerWorkMs (timing ABI v5), landed by
73bfab3.

### 2026-09-23 -- First acting flight (b55e06b): the governor finds and holds its operating point, 50 -> 71-75 fps at the parked pad, unnoticed from the cockpit

Build v0.17.0-370-gb55e06b (edvr_log.py --expect-build b55e06b exit
0), 03:30-03:35 local (gfx log edvr_gfx_20260923_033053.log; runtime
log edvr_openxr_20260923_033054_701_6520.log, UTC). Parked at
Cranfield, same spot and settings as the shadow flights
(LODDistanceScale 0.001, so the game's s = 1.500, MaterialQuality 0),
fix.settlement_detail = auto, advanced.settlement_detail_observe = 0,
k_max 4 (compiled default). Sean's report: nothing visible at first,
fps in the upper 40s, then as time went on it jumped into the 80s;
nothing different visually in the parked scene.

**Hooked and acting.** Configure line at 03:30:53: "acts by scaling
the game's LOD scale right after the engine sets it each frame
(FUN_142819D90)", builder, part test and LOD-scale setter hooked,
plane test matched. First "LOD scale scaled: game s 1.500 -> 1.575
(k 1.05)" line at 03:32:34.

**The ramp.** First step up at 03:32:34 (14.35 ms vs 11.11), monotone
up with NO down steps to 2.70 at 03:33:53 (80 s, one 0.05 step per
~2.4 s: each needs 30 consecutive over-budget samples), a pause, 2.75
at 03:34:48, reset to 1.00 at 03:35:03 (under 150 records for 30
frames: Sean left the view). The 80 s ramp is the one fault Sean felt.

**30 s summaries** (window end local; frames; k; s x k; caller work
mean ms; over/under-budget samples):

| end | frames | k | s x k | caller work | over/under |
|---|---|---|---|---|---|
| 03:32:53 | 1507 (loading in) | 1.00 -> 1.75 (15 up, 0 down) | 2.625 | 12.88 | 1267/107 |
| 03:33:23 | 1669 | -> 2.30 (11 up) | 3.450 | 12.42 | 1289/79 |
| 03:33:53 | 1814 | -> 2.70 (8 up) | 4.050 | 12.07 | 1197/75 |
| 03:34:23 | 2344 | 2.70 HELD (0 up, 0 down) | 4.050 | 10.62 | 392/728 |
| 03:34:53 | 2111 | -> 2.75 (1 up) | 4.125 | 11.15 | 731/280 |

At 03:34:23 the work sat inside the dead band (10.11-11.41 ms), 0
up/0 down. Every settlement window: LOD scale game s 1.500, held =
s x k; setter calls 2 per frame, scaled exactly once per frame
(1814/1814, 2344/2344, 2111/2111) on 1 pointer (called with 2;
builder contexts 1); implausible 0; faults 0; disagreements at the
held scale: parts 0, records 0; builder records 680 per frame on
every frame from 03:33:23 (no oscillation: checklist item 7 met).

**Per eye (A / B, per frame).** Window ending 03:33:53 (s x k ->
4.05): parts tested 6169 / 6230 (all at EDVR's scale), engine passed
4865 / 5021, dropped (the game's setting would have kept it) 487 /
519 (max 891), LOD level changed 296 / 305, records passed 519.2.
Window ending 03:34:23 (4.05 held): tested 6158 / 6192, passed 4800 /
4914, dropped 532 / 574 (max 578), level changed 280 / 292, records
passed 529.0; dropped angular radius, eye A over the window: < 0.25
deg 66.5k, 0.25-0.5 236k, 0.5-1 465k, >= 1 deg 479k (per frame 28 /
101 / 198 / 204). Window ending 03:34:53 (4.125): tested 6178 / 6045,
passed 4729 / 4712, dropped 549 / 582 (max 609), records passed
528.7. Plane test not run 0. Other views: dropped 19-215 per frame.

Against shadow flight 2 at k = 1 (parts tested ~10.5k, passed ~9.6k,
records passed ~605): at s x k 4.05 the engine passes ~4.85k parts
per eye (-49%) and 529 records (-12.6%); acting's own "dropped" count
(532-582) is the lower bound the design note predicted (records that
lost the eye never reach the builder); the fall of "engine passed" is
the whole effect.

**Runtime cycle instrument** (window end local; cycle mean ms (p50);
before_1st_submit; post_2nd_to_wait; next_wait_roundtrip; caller
work; fps):

| end | cycle mean (p50) | before_1st | post_2nd | next_wait | caller work | fps |
|---|---|---|---|---|---|---|
| 03:33:12 | 18.21 (21.16) | 7.28 | 4.71 | 5.65 | 12.56 | 54.9 |
| 03:33:42 | 17.51 (20.78) | 7.28 | 4.52 | 5.15 | 12.36 | 57.1 |
| 03:34:12 | 14.06 (11.49) | 6.64 | 3.93 | 2.93 | 11.13 | 71.1 |
| 03:34:42 | 13.26 (11.36) | 6.44 | 3.86 | 2.42 | 10.85 | 75.4 |

All windows admitted = valid. From k 2.7 the MEDIAN cycle is one
90 Hz slot (11.4-11.5 ms) with a two-slot tail (p95 22 ms): mean
71-75 fps, runs in the 80s. Part of the gain is the game's own: as
fps rose, its pre-submit work fell 7.28 -> 6.44 ms and its
post-submit work 4.71 -> 3.86 ms (per-frame work that scales with
frame time).

**Verdict: the acting mode works and is safe at this view.** The
governor found and held its operating point (k 2.70-2.75, s x k
4.05-4.13, caller work 10.6-11.2 ms inside the dead band); the visual
cost at s x k 4 was not noticed from the cockpit with ~half the eye
parts and 13% of the records gone. Checklist items 1-8 met (8:
nothing noticed; design-settlement-lod-bias-2026-09-22.md section 8);
item 9 not exercised (Sean quit rather than switching to game).

**Decisions after the flight (Sean, 2026-09-23), all IN BUILD by two
agents, not merged:** a faster start (up steps of 0.25 while the 30
triggering samples average more than 1.0 ms over the period, 0.05
near the target; down steps stay 0.05; no warm start); the perf
monitor's CPU figure becomes the caller work per cycle (timing v5),
with the pre-submit figure as the labelled fallback; promote
fix.settlement_detail to a shipped fix (compiled default auto, live
in edvr.ini, in the F8 settings menu and the installer's settings
app, README and release notes); k_max default raised to 6 and the
ceiling to 8, because the default slider holds s = 1.0 (Sean is
restoring it); acting gated to the cockpit (on foot holds k = 1)
until an on-foot flight.

ruled out: nothing new this flight.

### 2026-09-23 -- Two more governor flights: sustained 90 Hz at the default slider (03:45, b55e06b), then the shipped build confirms the coarse ramp, cockpit gate and reduced-from-menu but finds the 30-consecutive trigger under-reacting (04:23, fd25af9)

**Flight 1, 03:45 local, build b55e06b** (the first acting build: 0.05
steps, k_max 4, no cockpit gate), the game's detail slider back at
DEFAULT (s = 1.000), Material Quality unknown, parked at Cranfield.
gfx log edvr_gfx_20260923_034557.log, runtime log
edvr_openxr_20260923_034558_980_15616.log.

**The ramp** (window end local; k; caller work mean ms):

| end | k | caller work |
|---|---|---|
| 03:47:58 | 1.00 -> 1.80 (16 up, 0 down) | 10.70 |
| 03:48:28 | -> 2.65 (17 up) | 12.91 |
| 03:48:58 | 2.65 held (0/0) | 11.25 |
| 03:49:28 | 2.65 held | 10.92 |
| 03:49:58 | -> 3.00 (7 up) | 11.57 |
| 03:50:28 | -> 4.00 (20 up; the ceiling) | - |

LOD scale game s = 1.000, held = k.

**Runtime cycle windows** (window ends local, UTC 09:xx = local 03:xx;
cycle mean ms (p50)): 03:47:14 14.13 (11.42); 03:47:44 18.51 (20.09);
03:48:14 16.94 (13.40); 03:48:44 11.23 (11.13); 03:49:14 11.12
(11.10); 03:49:44 11.65 (11.16); 03:50:14 21.30 (20.94); 03:50:44
21.39 (21.79); 03:51:14 16.90.

**The first sustained 90 Hz at this settlement.** From 03:48:14 to
03:49:44, at k 2.65-3.0 on s = 1.0 (effective s x k 2.65-3.0), the
runtime ran a full 90 Hz for 90 s (cycle mean 11.1-11.6, median one
slot). Application-render GPU 6-10 ms parked, then 22-23 ms from
03:50:01 (something else on screen; the caller work rose and k
climbed to the cap of 4.00). The cap at s = 1.0 is what refinement
2's k_max 6 fixed.

**Flight 2, 04:23 local, build fd25af9** (the shipped default: coarse
0.25 steps, k_max 6, cockpit gate, perf-monitor CPU = caller work),
slider at DEFAULT (s = 1.000). gfx log edvr_gfx_20260923_042313.log,
runtime log edvr_openxr_20260923_042314_964_2768.log.

**The coarse ramp.** First step 04:24:51 (k 1.00 -> 1.25, the 30
samples' mean 1.49 ms over: the coarse step), 04:25:00 (-> 1.50, mean
4.39 over), 04:25:06 (-> 2.50, +3 steps, mean 2.49 over): six 0.25
steps in 15 s -- one per ~2.5 s, paced by the 30-consecutive-samples
trigger, not the one-step-a-second cap.

**Windows.** 04:25:14: k 2.50 (6 up, all by 0.25), 9.29 ms mean (ramp
included), over 772 / under 610 of 1700. 04:25:44: k 2.50 HELD (0 up,
0 down), 10.99 ms mean, over 534 / under 378 of 1712 -- a third of the
frames over the period + 0.3 ms; the runtime's window ending 04:25:31:
56.2 fps, cycle mean 17.8, median 21.6 ms (two slots); Application-
render GPU 5.6-8.3 ms through 04:25:04-04:26:14 (NOT the wall).
04:25:48: a coarse step to 2.75 (mean 3.63 over), then 3.00.

**On foot.** Sean disembarked ~04:25:55: the gate held k at 1 -- the
summary's "held on foot" 1843 frames (window ending 04:26:14; window
range 1.00..3.00, 2 up), 2223 frames (04:26:44, every frame), 2240
frames (04:27:14); on foot the caller work read 11.7-13.2 ms at
74-83 fps with the runtime's next-wait roundtrip 0.2 ms (the on-foot
deferred frame pacing), GPU 12-15 ms.

**Reduced from the menu.** At ~04:26:14 Sean switched the mode to
"reduced" from the new F8 menu row (configure line reprinted "on
(reduced: ...)" at line 1578, live). On re-boarding at 04:27:13 the
log reads "k 1.00 -> 6.00, reduced: in a settlement (>= 200 builder
records), k = k_max at once" (caller work 17.66 ms on that frame);
the window ending 04:27:14 shows k 6.00, held 6.000. At 04:27:42 "k
6.00 -> 1.00, reset: under 150 builder records for 30 frames" (Sean
left). Zero faults, zero disagreements at the held scale throughout,
as read from the summaries' fields.

**Verdict.** The coarse ramp, the cockpit gate, the on-foot hold
lines, reduced-from-the-menu and the reset all work as designed;
k_max 4 was the cap at the default slider on 03:45 (fixed at 6 in
fd25af9). The one fault: the 30-consecutive trigger under-reacts at
the operating point -- with a third of frames missing their slot no
run of 30 misses occurs, so the governor holds while the runtime
half-rates a third of the frames (56 fps at k 2.50 held with the CPU
mean 10.99 ms and the GPU 5.6-8.3 ms).

**Refinement 3, ordered by the overseer 2026-09-23, IN BUILD:** a
miss-fraction trigger -- up when at least 3 of the latest 30 valid
samples run over the period + 0.3 ms (0.25 if their mean excess is
over 1.0 ms, else 0.05), down when 0 of 30 are over and the mean is
under by more than 1.0 ms; the dead band targets under 10% of frames
missing.

ruled out: 30 consecutive over-budget samples as the up trigger,
because at the operating point a third of the frames miss their slot
without ever forming a run of 30 (04:23 flight, window ending
04:25:44).

Next flight: the refinement-3 build, parked at Cranfield at the
default slider: expect fine steps once a second until fewer than 3 of
30 frames miss, and the runtime near 90 Hz as on 03:45.

### 2026-09-23 -- Refinement 3 flight ran in reduced mode by accident: the trigger never fired, k saturated at 6, and a caller-work trap at half rate

Build v0.17.0-382-g8ce12926 (refinement 3: the miss-fraction
trigger), 05:05-05:09 local, gfx log edvr_gfx_20260923_050551.log,
runtime log edvr_openxr_20260923_050552_665_11412.log, slider at
default (s = 1.000), Cranfield. The mode was still "reduced": Sean
had switched it in the F8 menu during the 04:23 session and the menu
persists to the ini, so the log reads "k 1.00 -> 6.00, reduced: in a
settlement (>= 200 builder records), k = k_max at once" at 05:07:27
(and again at 05:08:05 after boarding) -- the refinement-3 trigger
never ran. Sean's report: "started off strong in the 80s but then
dipped into the 40s again and didn't fully recover".

**Windows** (end local; k; caller work mean ms; over/under of
samples; on foot):

| end | k | caller work | over/under | on foot |
|---|---|---|---|---|
| 05:07:51 | 1 -> 6 (1 to k_max) | 9.65 | 670/909 of 1880 | 150 frames |
| 05:08:21 | 6.00 | 12.08 | 1069/168 of 1900 | 1123 frames |
| 05:08:51 | 6.00 | 11.95 | 932/94 of 1501 | - |
| 05:09:21 | 6.00 | 11.00 | 542/397 of 1734 | - |
| 05:09:34 | reset (left) | - | - | - |

05:07:51's records 504.6/frame (loading). Sean disembarked ~05:07:55
and re-boarded 05:08:05, inside the 05:08:21 window.

**Runtime windows** (end local, 30 s; cycle mean (p50)): 05:07:22
13.97 (11.29) -- the arrival, ~72 fps, the "80s"; 05:07:52 20.39
(21.91), 45 fps; 05:08:22 12.17 (11.52), next_wait 0.2, on foot,
~82 fps; 05:08:52 20.99 (22.11); 05:09:22 18.86 (21.89); 05:09:52
17.27 (21.13). Application-render GPU 5.0-8.3 ms throughout the
cockpit minutes (0.5-0.8 in the menu): not the wall.

**Reading.** Compared with 03:45 at the same spot and slider (90 Hz
at k 2.65-3.0, 10.9-11.6 ms): at the saturation point of the lever
(s x k 6, ~34% of pool draws gone) the caller work ran 1-1.5 ms
higher. Two candidates, neither confirmed: material quality restored
(leg C measured MaterialQuality 0 worth ~0.7-1 ms of game-side CPU;
Sean asked whether it's back at default here); and a half-rate trap
-- the game's own per-frame work grows ~1.2 ms once the runtime
drops to 45 (the first acting flight measured both its phases
shrinking as fps rose), so a frame that fits at 90 Hz does not fit
at 45, and the compositor needs a run of fitting frames to climb
back out.

ruled out: nothing new -- the flight did not exercise the
miss-fraction trigger.

**Refinement 4, ordered by Sean ("build all 4 refinements"), IN
BUILD:** the miss signal becomes the real slot outcome (a two-slot
cycle measured at the frame boundary; misses with the CPU under the
period do not count); asymmetric response (down only after 5 s
without a miss and 1 ms of margin, one step per 5 s); a kick to
k_max when misses persist 3 s below the ceiling (one per 30 s); a
ceiling line when k_max still misses ("the remaining caller work is
not LOD-elastic").

Next flight: refinement 4, in AUTO (set the F8 row back), slider
default, material quality 0 for a minute then 3 to attribute the
millisecond; expect the kick and the recovery to full rate, or the
ceiling line.

### 2026-09-23 -- The LOD lever is INERT at close range: the 05:53 auto flight parked near the buildings, not the pad, and the governor thinned nothing

Build v0.17.0-382-g8ce12926 (refinement 3), 05:53-06:02 local, gfx
log edvr_gfx_20260923_055323.log, runtime log
edvr_openxr_20260923_055324_909_24412.log. fix.settlement_detail =
auto (the F8 row set back), slider default (s = 1.000), MaterialQuality
0, BlurEnabled false. "Approx the same spot" per Sean, but by the
counts a spot much closer to the buildings than the pad.

05:53-05:58: the headset idle (runtime cycles 77.8 ms, next-wait
72 ms; 12.9 fps; not evidence). 05:59:27 the settlement's records
begin (599.2/frame in the window ending 05:59:54, 680 from 06:00:24).

**Approach and landing** (05:59:24-06:00:27): caller work 6.8-7.8 ms
mean, over 9-54 of ~2,600 samples, runtime windows 87.7 / 88.3 /
86.5 fps (cycle p50 11.12-11.14); eye A parts tested 3,082 -> 5,693
per frame, engine passed 159 -> 3,167 (approaching: far parts fail
the screen-size term); k briefly 1.10 then back to 1.00.

**The ramp.** Landed ~06:00:27 at caller work 12.51 ms (first step
line). 06:00:27 up 0.05 (3/30 over); 06:00:32 up 0.25 to 1.40 (12/30,
mean 1.07 over); 06:00:37 up 0.25 to 2.45 (26/30, mean 1.56 over);
06:00:42 down to 3.00; fine steps up through 3.25, 3.40, 3.65, 4.10
(a down at 4.25), 4.20, 4.35, 4.60, 4.85, 5.10; up 0.25 to 5.55 at
06:01:39; k_max 6.00 by ~06:01:50; then downs 5.95, 5.85, 5.60, 5.55,
an up to 5.80, down to 5.65 (06:02:22).

**Windows** (end local; frames (fps); k; caller work mean ms;
over/under):

| end | frames (fps) | k | caller work | over/under |
|---|---|---|---|---|
| 06:00:54 | 1,863 (62) | 1.00 -> 3.45 (24 up, 7 by 0.25; 3 down) | 10.72 | 545/776 |
| 06:01:24 | 1,881 | -> 4.60 (24 up, 1 by 0.25; 5 down) | 10.53 | 390/772 |
| 06:01:54 | 1,826 | -> 6.00 (19 up, 3 by 0.25) | 11.06 | 503/479 |
| 06:02:24 | 2,330 (78) | 5.95 (11 up, 16 down) | 10.02 | 188/1,456 |

**Runtime windows** (cycle mean (p50) ms): 06:00:54 17.69 (21.58,
half rate typical); 06:01:24 15.50 (11.62); 06:01:54 16.12 (11.85)
and 16.09 (11.98); 06:02:24 17.86 (21.56); 06:02:54 13.35 (11.34).
Application-render GPU 4.6-9.1 ms at the settlement: not the wall.

**THE FINDING, per eye per frame.** At s x k 3.45: EDVR dropped 24.3
parts of 4,879 the engine passed (5,642 tested, 4,807 at EDVR's
scale). At 4.60: 3.8 of 4,778. At 6.00: 4.0 of 4,815. At 5.95: 3.8 of
4,417 -- against the first acting flight on the pad at s x k 4.05:
532 dropped and the passed set halved (9.6k -> 4.85k). At this view
nearly every passing part sits closer than where the LOD term bites
(the dominant table's t0 0.2737 with A 0.000834 per metre: parts
beyond ~55 m at s x k 6, ~73 m at 4.5, ~219 m at the slider's floor
s 1.5), so the governor thinned nothing; the recovery from the
landing's 12.5 ms to ~10 ms and ~78 fps came from the game's own
per-frame work shrinking as the runtime left half rate and from the
post-landing settle, not from LOD. Zero faults; disagreements 0, as
read from the summaries.

**Material quality.** This flight at 0 read 12.5 ms on landing
against the 05:05 flight's 11-12 ms at quality 3 (a different spot):
the spot dominates, so the millisecond is NOT attributable from
these two flights.

ruled out: the LOD lever at close range, because at a view where the
passing parts sit within ~55 m an effective scale of 6 removed ~4
parts per eye per frame of ~4,800 (05:53 flight); the lever's
elasticity is a property of the view's distances, not of k.

**Consequence, ordered by the overseer:** refinement 4b adds inert-
lever detection beside refinement 4's other pieces (the per-second
miss windows, the 10 s kick, the fast post-kick relaxation, GPU-
bound classification of misses): hold and log when two consecutive
up steps change the dropped-part count by under 0.5% of the passed
parts; re-arm when parts tested change by 20% or after 30 s.

Next flight: the refinement-4 build, auto, parked ON THE PAD (the
original spot: lat 68.067474 lon 121.028328 heading 42) so the lever
has something to bite, material quality 0 for a minute then 3.

### 2026-09-23 -- Refinement 4b flown at the pad: every rule fires as designed, but at the ceiling the wall becomes the frame-time TAIL, not the mean

Build v0.17.0-393-g6d34ffd5 (refinement 4b, review folded in),
07:15-07:20 local, gfx log edvr_gfx_20260923_071521.log, runtime log
edvr_openxr_20260923_071527_692_27576.log. auto, slider default
(s = 1.000), fix.engine_motion off, parked on the pad. Sean:
material quality 0 for the first minute with a disembark and
re-board, then material quality 3.

**Every 4b rule fired.** Pre-settlement windows show the fresh-timing
rule ("no fresh timing on 2030 frames, 4 expiries" during the load,
then 0-1). Arrival (07:17:21): k 1.00 -> 2.65, slots missed 330 of
2290 (269 the CPU's, 15 GPU-bound, 46 unexplained), per-second lines
e.g. "26 of 37 cycles in the last second took two display slots as
the CPU's". A coarse step to 2.90, then "k 3.15 -> 6.00, kick: 10
seconds in a row with a tenth or more of the cycles taking two slots
as the CPU's (the last 40 of 48)", then "k 6.00 -> 3.15, restored
k 3.15 after a failed kick: the fifth second at k_max still had a
tenth or more of its cycles take two display slots". An "inert at
this view: no observed benefit: passed parts 9,720 -> 9,618 a frame"
hold fired at 07:17:3x during the approach -- a judgement on a
changing scene, the fix this flight orders (below). On foot: "held
on foot" 161 then 1167 frames; "k 1.00 -> 3.90, back aboard: k
restored to 3.90 (held on foot 1328 frames)"; then steps to 6.00 with
"at the ceiling ... (at k_max now: residual benefit)" for the windows
ending 07:18:21, 07:18:51, 07:19:21; reset at ~07:19:4x on leaving.

**Windows** (end; k; caller work mean ms; slots missed of samples --
the CPU's / GPU-bound / unexplained):

| end | k | caller work | slots missed |
|---|---|---|---|
| 07:17:51 | 1.00 (on foot) | 11.27 | 858 of 1619 (732/2/124) |
| 07:18:21 | 6.00 (1167 on-foot frames in it) | 11.92 | 580 of 806 (531/0/49) |
| 07:18:51 (MQ3) | 6.00 | 10.70 | 792 of 1890 (645/1/146) |
| 07:19:21 (MQ3) | 6.00 | 10.12 | 604 of 2089 (479/0/125) |

**Runtime windows** (window ends; cycle mean (p50) ms): 07:18:27
17.92 (21.60, half rate typical); 07:18:57 12.93 (11.27); 07:19:27
(on foot) 12.34 (11.55), next-wait 0.24; 07:19:57 17.28 (21.42);
07:20:27 14.84 (11.49). Application-render GPU 4.7-7.4 ms at the
settlement (MQ0 and MQ3 alike): not the wall.

**Per eye A.** 07:17:21 (arrival, k -> 2.65): tested 2109 (1945 at
EDVR's scale), passed 1779, dropped 134.6/frame (max 844), dropped
angular radius <0.25 deg 35k, 0.25-0.5 112k, 0.5-1 129k, >= 1 deg
32k. 07:18:21 (k 6): tested 2477, passed 2180, dropped 1.5. 07:18:51:
tested 5515, passed 4650, dropped 4.0. 07:19:21: tested 5432, passed
4420, dropped 3.3. Disagreements at the held scale 0 parts, 0
records, every window.

**Reading.** At the ceiling the passed set is about half of the
k = 1 baseline ("Shadow flight 2" on the pad: tested ~10.5k, passed
~9.6k per eye) -- the removal is at the RECORD level, invisible to
the "dropped" counter, as the design note predicted -- yet 25-34% of
cycles still take two slots with the caller work at 10.1-10.7 ms
MEAN: the frame's tail, not its mean, is the wall now; the kick
could not clear it because the lever is already at its ceiling.

**Material quality.** MQ3's windows read 10.70 and 10.12 ms against
MQ0's 11.92 (with on-foot frames): no CPU cost to MQ3, so the
millisecond between the 03:45 and 05:05 flights was not material
quality.

**The LOD tables.** Recorded at slider 0.001 (gate_012514.bin: t0
21.3833 / 0.273707 / 0.07698, 4 levels 0.01283 / 0.02566 / 0.05132 /
0.10264, etc.) and at slider 1.0 (gate_043720.bin) are IDENTICAL: the
slider does not change the tables. The differing elasticity between
captures is the VIEW's composition -- 043720 at the close spot: 86%
of tested rows in the shell family, whose t0 21.38 nothing reaches;
012514 at the pad: 56%.

ruled out: the draw-distance slider as a change of the LOD tables,
because the tables recorded at 0.001 and 1.0 are byte-identical (gate
012514 vs 043720). ruled out: material quality as the caller-thread
cost between visits, because MQ3 read 10.1-10.7 ms against MQ0's
11.9 in the same flight.

No flicker reported this flight (engine motion off; back-aboard
restore in force).

Next: a cpu_profile leg at the pad at k 6 (F9/F11 hotkeys) to see
what the slow 30% of frames do; the 4b correction to the benefit
judgement (stable scene only, tested+passed) is in build.

### 2026-10-27 -- Static-geometry RE pass (desk work, no flight): the motion path already treats static geometry as static; the one sizeable static redundancy is Elite's per-frame pool rewrite, unpriced

Sean's question: settlement buildings, props and terrain do not move,
so does EDVR or Elite run motion-vector or AA work for them that could
be skipped? Two read-only passes, one over EDVR's source on main, one
over the decompile notes. EDVR's RVAs are build 334925; the decompile
files are 332841, and the render chain is reloc-only between them
(build-334925.md).

**EDVR's motion path (read in code).**
- Pool-family substitution is decided by binding generations, never by
  what the draw's instances hold (engine_velocity.h:163-187). The slow
  half (patched PS/VS, MRT6, blend) runs only when the game rebinds:
  0.02-0.06 ms a frame (kinematic-motion-injection-2026-09-19.md,
  lines ~3977-3979). Unsubstituting static-only draws would add
  toggles, at 11-13 D3D calls each (engine_velocity.h:245-251). No
  online signal maps a draw to its records, except for skinned
  families.
- The emit resolves and writes the previous pose for every joined item,
  static or not. `last != pose` only bumps `recordsMoving`
  (engine_velocity_emit.h:199-213, 502-508). A static skip saves at most
  the emit bracket, 0.015-0.2 ms a frame of job-thread time, which the
  2026-09-22 measurement puts off the critical path. It would also move
  the pixel to the camera term. That path is not bit-identical to the
  engine path for a static surface: it differs in compose order, the
  hidden-history test, the celestial check and the stale gate
  (temporal_shader_source.h ~1002-1037; flat_mono_shader_source.h
  ~393-410).
- The per-pixel record fetch for static pixels was already ruled out as
  a cost (2026-09-23, kinematic doc).
- Flat: the per-draw shell was cut on 2026-10-18 (geometry issue reuse,
  early match). At the measured CZ view flat runs at 118 fps, equal to
  the GPU span (flat-callback-cpu-handoff-2026-10-18.md).

**Elite (decompile, 332841 RVAs).**
- Every frame, for a static record: FUN_14433DB20 recomposes the world
  pose from node x parent, packs the quaternion twice, writes
  +0x170/+0x17C/+0x240, and runs the slot loop (unpack, 4x4 compose,
  vtable +0x78 per slot) when +0x68 returns nonzero. FUN_144312040
  composes again, and the builder FUN_1442B4420 writes the 0x150-byte
  items. The pose outputs are bit-identical for a static record. The
  per-view LOD, frustum and mask outputs depend on the camera and are
  not. The camera never enters the pose math. The floating origin
  enters through the parent translation, so an input-keyed cache would
  invalidate itself on a rebase.
- A cache inside the job (survivor (b) of the 09-21 gate) has two
  pure-function sites. One is FUN_14433DB20's compose, about 1 KB of
  2,389 B with about 31 float inputs. The other is FUN_144312E00
  (985 B). The node append and lookup must still run every frame, so
  the cache saves only arithmetic: an estimated 0.5 ms or less summed
  over threads, 0.1-0.2 ms on the critical path, against 0.78-0.81 ms
  for the whole pipeline. Both sites need mid-function patches.
- The pool copier FUN_144C81BE0 is this pass's one sizeable finding.
  On 334925 it is 0x4C846A0, already hooked as
  `engine-primary-pool-copy` (kinematic_eval_hook.cpp:293, 701-716).
  Each frame it memcpys the items into the WRITE_DISCARD-mapped t33
  pool: stride 336, 15,360 slots (5.16 MB), about 12.3k records, so
  about 4.1 MB a frame. A static record's bytes do not change between
  frames: no builder record's pose changed over 3 frames
  (design-occlusion-culling §10). The copy is UNPRICED: EDVR's copier
  clock pauses around the forward (:714-716), so no log line holds it.
  The "draw-item chain 0x4c81ca0 < 0x4c8227c" in R5c (the "Phase A,
  valid" entry) is this loop and its call site. R5c's 3.36 ms of
  running minus the named pipeline parts bounds it at about 1.7 ms or
  less, by subtraction only. Bandwidth suggests 0.3-0.6 ms for 4.1 MB
  of write-combined stores (inferred).
- FUN_14430EFE0's per-view verdict is consumed only by the class-2
  emit. It runs about 28.9k calls a frame, up to ~23% of them for
  builder records. That is worker CPU only, and not specific to static
  geometry.
- No new static/dynamic flag. rec+0x2B0 is the child count; stage 3's
  "dirty flag" is bucket+0x2B0, another struct. The class byte
  (rec+0x2B8 -> +0x160)+0x4B == 2 splits the singleton path from the
  builder path, and both paths hold statics and movers.

ruled out: unsubstituting static-only pool draws, because substitution
is keyed on pipeline state at 0.02-0.06 ms a frame and toggling per
draw costs more than it saves.
ruled out: skipping the emit's previous-pose write for static records
as a CPU lever, because it is at most 0.2 ms of job-thread time off the
critical path, and the camera term it falls to is not bit-identical.
ruled out: a pose cache inside the job as a wall-time lever, because
the whole pipeline is 0.78-0.81 ms on the critical path and the cache
saves only arithmetic (0.1-0.2 ms).

Open, cheapest first; one parked pad leg reads both:
1. Price the native pool copy. Add a QPC pair around `forward(task)` in
   `poolCopyObserved`, split by thread class (render, job), on the
   existing 30 s line. Gate: at 1 ms a frame or more on the caller
   thread at the pad, design an incremental pool upload, an EDVR-owned
   buffer fed only the changed ranges. Its failure modes are slot
   repacking when the visible set churns, and rebases. It helps the
   mean, not the frame-time tail that refinement 4b found. Under about
   0.3 ms, close it.
2. Price the VR AA-on per-draw increment at a settlement on the current
   build. The last settlement figure is leg C (2026-09-23): 1.55 ms of
   draw hooks at 18.8k draws, from before engine motion existed. Aboard,
   AA on costs 0.55-0.60 us a draw against 0.14-0.16 off
   (stereo-cpu-sharing-2026-10-16.md:23), and the increment is
   unattributed. Read `draw hook CPU:`, `engine motion CPU` and `perf
   baseline F5` with AA on and off. This is not specific to static
   geometry. If the increment is large, the lever is a per-pipeline-state
   verdict in the X thunk (vscreen.cpp:5307-5349), like flat's 10-18 cut.

**Item 1 built (2026-10-27, branch
claude/motion-vector-aa-settlements-e8c512), NOT FLOWN.** The copier
relay hands its own pause and resume readings to
`emcpu::resumeForward`. That prices the game's copy, less any nested
EDVR scope, into a cell of its own (`kGameCopy`), with no extra clock
reading and in no EDVR total. A fifth 30 s line, `engine motion CPU,
the game's own pool copy`, prints the render thread's mean and
p50/p95/max per frame, calls a frame and longest call, then the other
threads' thread-ms per sampled frame. The relay stays installed once AA
has been on, so the AA-off leg should price the copy too (a
cross-check). If it does not, that leg prints "-". Rig:
engine_motion_cpu_test, 160 checks.

Flight: Steam, parked in the cockpit at a settlement pad, head still.
AA on for at least 90 s, then F7 (hotkey.compare_aa, the AA comparison
key) for AA off for at least 90 s, then F7 again for AA on for at least
60 s. No other key: every line read here prints on its own 30 s window.
Read line 5 per window and `draw hook CPU:`, after `edvr_log.py
--expect-build HEAD`. (`perf baseline F5` is the baseline probe's VR
draw-hook line, performance.md. That probe, 5d8f6810, is on neither
this branch nor main, so this build prints no such line.)

**Pad leg flown (2026-10-27, Steam log `edvr_gfx_20261027_093013.log`,
build 22ddf59f; Pimax Crystal Super on Pimax OpenXR, RTX 5090, DLSS
performance preset K, 2016x2017 in and 4032x4034 out per eye).**
Docked at Cranfield Nutrition Biosphere (38 Lyncis 4 f) at 09:37:38,
head still. AA on to 09:39:49, F7 off to 09:41:30, F7 on to 09:42:28.
That leaves four clean AA-on windows, two AA-off ones and one AA-on
one.

1. The game's pool copy costs 0.23-0.25 ms a frame on the render
   thread in the first AA-on leg (p95 0.27-0.30, four calls a frame)
   and 0.28 ms in the last (p95 0.34). No copy ran on any other thread.
   That is under the 0.3 ms line, so item 1 is closed.
   ruled out: an incremental t33 pool upload, because the whole copy
   costs 0.23-0.28 ms a frame at a settlement pad and an incremental
   upload would save only part of it (flown 2026-10-27).
   The AA-off windows print "-". With AA off the relay's scope is never
   entered (the copier part is "-" too), so that leg gave no
   cross-check. Elite still makes the copy then.
2. Draw hook CPU, in ms per sampled frame, with that one frame's eye
   draws: AA on, first leg 3.38 / 3.47 / 3.82 / 3.54 at 14.2-15.8k
   draws (0.22-0.25 us a draw); AA off 2.45 / 2.44 at 20.5-22.6k (0.11
   us); AA on, last leg 4.26 at 21.9k (0.19 us). At matched draw counts
   (the last two legs, about 22k), AA on adds about 1.8 ms a frame,
   about 0.08 us a draw. Engine motion's own render-thread work is p50
   0.32-0.38 ms a frame with AA on, about 0.16 ms of it the instrument's
   floor, and 0.02 ms with AA off. So it accounts for about 0.35 ms of
   the increment at most, and about 1.4 ms is unattributed. Caveats: the
   draw counts are single frames, the matched comparison rests on one
   window per leg, and the draws grew from 15k to 22k at the same pad
   between the first leg and the second (cause not read). Frame rate:
   55-60 fps with AA on, 46 with AA off (native 4032x4034 per eye).

Next: attribute the roughly 1.4 ms inside the AA-on draw path before
any cut is proposed. That means clocking each per-draw step (the X
thunk's verdict, the binding-generation check, the tees) on the same
parked leg.

**Step clock built (2026-10-27, same branch), NOT FLOWN.**
`src\d3d11\draw_step_clock.h` (temporary, no key) clocks the VR draw
thunks step by step on the draws half a stride from the ones the
`draw hook CPU:` line clocks, so that line keeps its meaning. On a
sampled frame the whole clock still takes draw 0 of each 64; the step
clock takes draw 32.

- **Steps.** Each mark ends a step. There are 26 steps:
  - pre;
  - eight verdict sub-steps (`v:hud/fss`, `v:context`, `v:particles`,
    `v:eye-test`, `v:offscreen`, `v:ui-depth`, `v:census`, `v:fixes`)
    and `v:rest`;
  - `engine-vel`, `probes`;
  - the forward scope before the draw (`f:scopes`, `f:ui-layer`,
    `f:verdict-begin`, `f:before`);
  - `GAME` (the game's own draw, excluded), `reissue`, `weapon`;
  - the X draw's post-draw work (`p:rigid`, `p:screen`, `p:rest`);
  - the forward scope after the draw (`f:layer-end`, `f:ui-depth`,
    `f:after`);
  - tail.
- **Clock floor.** Each step is printed net of its marks' clock reads.
  The floor is calibrated as each window closes.
- **Cost to other draws.** Every other draw pays one branch per thunk
  mark and one plain load per sub-mark. The sub-marks test a
  process-wide count of live step clocks before they read the
  thread-local, so the whole-clocked draws add almost nothing to the
  old line.
- **Two new lines** after `draw hook CPU:` in each 1800-frame window:
  - `draw hook CPU by step:` gives net ms a frame per step, EDVR's sum
    and the game's draw;
  - `draw hook CPU by step, calls and kinds:` gives calls a frame per
    step, and draws and EDVR ms a frame for D, I, N and X.
- **Cross-check.** The old line should read near EDVR's sum less
  `weapon`, which the old clock counts with the game's draw. If it does
  not, the floor correction is off and the split is not trusted.
- **Rig.** aa_off_test, 129 checks.

Flight: the same leg on Steam. Park at a settlement pad, head still.
AA on for at least 90 s, F7 for AA off for at least 90 s, F7 for AA on
for at least 60 s. Read the three lines per window after `edvr_log.py
--expect-build HEAD`.

What to read:
- The steps whose net figure moves between AA on and AA off hold the
  increment.
- The D/I/N against X split says whether it is in the X thunk, which
  has no AA-off fast path, or in the fast-path thunks.

**Step clock flown (2026-10-27, Steam log `edvr_gfx_20261027_101918.log`,
build af9c429b; same rig: Pimax Crystal Super on Pimax OpenXR, RTX 5090,
DLSS performance).** Loaded in docked at Cranfield Nutrition Biosphere
at 10:21:55. F7 turned AA off at 10:23:15 and back on at 10:24:46.
- Clean windows: AA on at frames 16200, 18000 and 25200; AA off at
  19800 and 21600.
- Frame rate: 62 fps with AA on, 48 with AA off.
- Draws: 34.5k hooked draws a frame. Of those, 33k are X, about 1.4k
  are D and about 100 are N; 16.8k go into the eyes. Every step ran the
  same number of times in both modes.
- Clock floor: about 16 ns a mark, at 23.8 marks per clocked draw.
  That is a large amount taken off each absolute figure, but it
  cancels in the on/off difference, which is the quantity read here.

EDVR's step sum is 3.73 ms a frame with AA on (mean of the three
windows) and 2.07 with AA off: an increment of 1.65 ms. The old line
reads 3.9-4.2 against 2.6. It sits 0.4-0.6 ms above the step sum
because it does not take off its own clock reads. The increment is
nearly all in the X draws (3.28-3.82 ms against 1.84-1.95) and is
spread across several per-draw classifiers. No one step holds it:

| Step | AA on | AA off | Increment | Calls a frame | Per call |
|---|---|---|---|---|---|
| v:ui-depth | 0.470 | 0.046 | +0.42 | 16.8k (eye) | ~25 ns |
| engine-vel | 0.378 | 0.061 | +0.32 | 34.5k | ~9 ns |
| f:ui-layer | 0.281 | 0.084 | +0.20 | 34.5k | ~6 ns |
| v:census | 0.163 | 0.028 | +0.14 | 16.8k (eye) | ~8 ns |
| p:screen | 0.158 | 0.035 | +0.12 | 33k | ~4 ns |
| f:layer-end | 0.170 | 0.083 | +0.09 | 34.5k | ~3 ns |
| v:eye-test | 0.150 | 0.076 | +0.07 | 34.5k | ~2 ns |
| f:before | 0.178 | 0.108 | +0.07 | 34.5k | ~2 ns |
| the other 16 steps | | | +0.22 together | | |

- **v:ui-depth.** The largest block is the two ui_depth classifiers,
  which run on every eye draw with AA on. uiDepthOnEyeDraw resets a
  dozen globals, reads the depth view, tests four SRV slots against the
  learned surfaces and hashes the bound VS. uiDepthHologramOnEyeDraw
  hashes the VS and walks two family lists. For an ordinary scene draw
  both answer "no", draw after draw.
- **engine-vel** is the generation-compare gate and the skin-draw note
  on all 34.5k draws. The engine motion CPU line's "draw side" counts
  only the 330-400 calls that pass the gate, which is why it read 0.13
  ms.
- **f:ui-layer, v:census and p:screen** are the same pattern: a
  predicate chain asked on every draw, each a few ns.

ruled out: one hot step in the AA-on draw path, because the 1.65 ms
increment is spread over eight steps at 2-25 ns a call, none above 0.42
ms (flown 2026-10-27).

Next lever, not yet evidenced: a per-draw early-out keyed on binding
generations. Most of these classifiers read the same few bindings (VS,
PS, PS SRVs 0-3, RTV0, DSV0), and Elite's 33k X draws a frame may
repeat them between consecutive draws. If they do, one generation
compare can stand for the chain. The repeat rate is the evidence
needed first: per eye draw, count whether any of those slots' bind
generations moved since the previous eye draw.

**Binding-repeat counter built (2026-10-27, same branch), NOT FLOWN.**
`src\d3d11\bind_repeat.h` is a temporary instrument with no key.

- **Capture.** On a sampled frame, each owner-context VR draw reads the
  generation and pointer of eight slots: VS, PS, PS SRV 0-3, RTV0 and
  DSV0. The read happens at the thunk's entry, before the draw clock.
- **Grade.** After the verdict, the draw is graded against the
  previous owner draw and, for an eye draw, against the previous eye
  draw:
  - repeat: no generation moved;
  - rebound: the same pointers, but a generation moved (the binding
    shadow bumps a generation on every rebind, even of the same view,
    and every generation once a frame);
  - changed: some pointer differs;
  - first: no previous draw in the same frame.
- **Answer stability.** For eye draws graded repeat or rebound, the
  counter checks whether the verdict and the three ui_depth flags
  (`t_uiDepthThisDraw`, `t_compositeThisDraw`, `t_holoDepthThisDraw`)
  matched the previous eye draw's, and whether a draw that differed had
  the same draw arguments. That tuple is a proxy: a real cache would
  also have to reproduce ui_depth's other per-draw state.
- **What a cache could skip.** The step clock's figures are split by
  grade. Eye-only steps (v:ui-depth, v:census, v:fixes) use the eye
  grade; the rest use the any-draw grade.
- **Its own cost.** The grading has its own step, `bind-count`. On
  sampled frames it also runs inside the old line's whole-clocked
  draws, so the old line reads about that much higher than in the last
  flight. Measure it as bind-count's figure.
- **Lines.** Two new lines follow the step lines in each 1800-frame
  window: `draw hook CPU by step, by binding repeat:` and `draw
  bindings, sampled frames:`.
- **Rig.** aa_off_test, 188 checks.

Flight: the same pad leg on Steam, AA on, then F7 off, then F7 on.

What to read:
- The eye-draw repeat and rebound shares, and which slot moves most.
- Whether the answers ever differ on repeat draws. Any differing draw
  with the same arguments means the key is not enough.
- The milliseconds on repeat draws in v:ui-depth, f:ui-layer and
  engine-vel: the most a cache could save.

**Binding-repeat counter flown (2026-10-27, Steam log
`edvr_gfx_20261027_110149.log`, build 17714416; same rig).** Loaded in
docked at Cranfield at 11:04:24. F7 turned AA off at 11:05:50 and back
on at 11:07:21.

Another worktree's full build ran beside the flight. It wrote files
from 11:05 to 11:09, which covers the AA-off leg and the second AA-on
leg, so this flight's timings are contaminated. The window ending at
frame 19800 (11:05:13-11:05:48) is unusable for timing: bind-count
reads 11.9 ms there against 0.33-0.41 in every other window, which
looks like render-thread preemption as the build started. The binding
counts and answer comparisons below do not depend on timing.

- **Repeats.** About 33.4k owner draws a frame. Against the previous
  owner draw: repeat 94%, rebound 1.3%, changed 4.6%. About 16.2k eye
  draws a frame. Against the previous eye draw: repeat 90.1-90.9%,
  rebound 2.2-2.6%, changed 6.9-7.3%. This holds in every window, AA
  on or off.
- **Per slot on eye draws** (generation moved / pointer changed):

  | Slot | Generation moved | Pointer changed |
  |---|---|---|
  | VS | 5.0% | 5.0% |
  | PS | 5.4% | 5.4% |
  | SRV0-3 | 8.4-9.8% | 4.4-6.1% |
  | RTV0, DSV0 | 0.2% | 0.1% |

  The SRVs account for the rebound share: Elite rebinds the same
  texture views.
- **Answer stability.** In each AA-on window, about 1.64-1.65M repeat
  eye draws and about 40-44k rebound ones were compared with the
  previous eye draw. Not one differed in the verdict, the interface
  depth, the composite or the hologram flag. (With AA off the ui_depth
  flags are not computed, so the AA-off windows prove nothing here.)
- **Time on repeat draws** (ms a frame, AA on, windows 18000 and
  28800):

  | Step | On repeat draws |
  |---|---|
  | v:ui-depth | 0.33 / 0.32 |
  | v:census | 0.14 / 0.13 |
  | p:screen | 0.13 / 0.13 |
  | f:ui-layer | 0.16 / 0.20 |
  | engine-vel | 0.12 / 0.14 (its changed draws cost more: 0.28 / 0.20) |
  | v:eye-test | 0.12 / 0.12 |
  | f:before | 0.09 / 0.09 |
  | f:layer-end | 0.07 / 0.10 |
  | the eight together | 1.15 / 1.25 |

  Less the AA-off leg's repeat figures for the same steps (window
  23400, contaminated by the build, so these are approximate), the
  AA-on work on repeat draws is about 0.8 ms a frame.
- **The instrument's own cost.** bind-count is 0.33-0.41 ms a frame
  on sampled frames, and the capture comes on top, outside every clock.
  This build's old line reads 4.4-4.9 ms with AA on, not 3.9-4.2, which
  is that cost plus the background build.

Conclusion: a cache keyed on these eight slots' generations would have
given the same answer on every repeat eye draw seen here. It could
take back up to about 0.8 ms a frame of the 1.65 ms AA-on increment,
less its own check.

The cheapest check is one load and compare per draw. That needs an
aggregate generation, bumped by bindingSet for any of the eight slots
and by the per-frame bump, with each classifier remembering the value
it last answered at. This is not yet designed in detail. Before
building it:
- the memo must replay ALL of a classifier's per-draw outputs.
  uiDepthOnEyeDraw sets about a dozen globals besides the flags
  compared here, and counts world-screen draws.
- the per-draw work that must still run (eye-draw counts, the census
  when armed) must be separated from what may be skipped.

v:ui-depth is the place to start: it is the largest step at 0.30 ms
on repeat draws, and it is the one whose answers were compared.

### 2026-10-27 -- Desk design: the ui_depth eye-draw memo (not built)

Read from `ui_depth.cpp` (uiDepthOnEyeDraw at 2620-2865, viewIsSurface
851, the surface table at 745-790, uiDepthConfigure 2435,
uiDepthHologramOnEyeDraw 3532) and `binding_shadow.cpp` (the setters
89-122).

**Where an ordinary scene draw leaves.** uiDepthOnEyeDraw does the
following, in order:
1. resets its outputs: `g_uiDepthDrawComposite`, the planet, solar,
   world-screen and corona pendings, `g_holoDraw = draw`, the mode,
   `g_wantRebind`, `g_wantMask`, `g_rebindEye`, `g_drawEye`,
   `g_reissueShader` and `g_reissueMaskSlot`;
2. tests the on and stood-down flags;
3. returns if no DSV is bound;
4. calls viewIsSurface on PS SRV 0-3, a probe of the view memo each;
5. hashes the VS, and checks the smoke, planet, solar and world-screen
   pairs;
6. leaves at the family gate (line 2693: not stellar, not a composite,
   VS not in `g_families`).

That exit, the plain decline, comes before any counter, log line,
shader lookup, eye resolve or scene-depth judgment. Every positive path
(planets, world screens, families, composites) has side effects:
`++g_wComposite`, `++g_wDirect`, `++g_worldScreenWindow`, noteFamily,
depthShaderFor and eyeIndexFor.

The memo therefore covers the plain decline only. Any other answer is
computed in full every time, so the families and composites keep their
counts and logs exactly.

The hologram classifier is the same shape. Its cheap declines
(`!h`, excluded, and in neither the marker nor the family list) depend
only on the VS hash and three lists. Its later declines read
dsvIsSceneDepth and the eye table, which can change within a frame (the
depth probe learns the scene depth at its first draw), so only the
cheap declines are memoised.

**What the plain-decline answer depends on:**
- bindings: VS, PS (read only for the smoke, planet and world-screen
  pairs), PS SRV 0-3 and DSV0;
- ui_depth's state: the on and stood-down flags (tested before the
  memo), `g_families` and `g_exclude` (set only in uiDepthConfigure),
  the holo family lists (holoDepthConfigure), and the learned-surface
  table, read through the per-view memo.

  The table changes in four places: addSurface; surfaceMatches'
  eviction of a surface whose size or format no longer matches; the
  chrome tracker's `g_viewMemo.put(view, ..., 1)` (line 2553); and the
  reset at 4522. The view memo also expires per frame (its entries live
  a hundred frames).

**The key.**
- **The bindings: one aggregate generation**, `bindingEyeKeyGeneration()`,
  in binding_shadow:
  - bindingSet, bindingInvalidate and bindingSetShader bump it when the
    slot is one of VS, PS, PS SRV 0-3, RTV0 or DSV0. That is a mask
    test, as these are the eight slots the counter measured; RTV0 is
    not read by the decline, but keeping the measured set keeps the
    flown 90.6%.
  - bindingForgetAll and bindingFrameBoundary bump it unconditionally.
  - It is an inline reader, like bindingGeneration. A rig defining
    EDVR_BINDING_SHADOW_EXTERNAL gets a declaration.
  - Its value moves exactly when some one of the eight generations
    moves, so its repeat rate is the measured one.
- **ui_depth's state: a classifier epoch** in ui_depth.cpp, bumped at:
  - uiDepthFrameBoundary;
  - uiDepthConfigure and holoDepthConfigure;
  - addSurface, and the eviction branch in surfaceMatches;
  - the put at 2553;
  - the surface reset and `g_viewMemo.clear()`.

  A view-memo miss that only records what the table already says needs
  no bump.
- **The memo** stores both values and which exit was taken: a plain
  decline, or the hologram's cheap decline. A call hits when both values
  match and the stored exit was a plain decline.

**What still runs on a hit.**
- uiDepthOnEyeDraw:
  - the on and stood-down test;
  - every output reset, including `g_holoDraw = draw` (the arguments
    differ per draw);
  - the two loads and compares;
  - `return false`. `g_uiDepthDrawComposite` stays false, so
    vscreen's `t_compositeThisDraw` is unchanged.
- The hologram classifier: `g_holoEye = -1`, `return false`.
- Everything else in the eye branch is untouched: the eye-draw count,
  the depth probe note, the intro panel and curve, the glare camera.
  The memo lives inside the two functions, so vscreen's call sites do
  not change.

What a hit skips:
- four view-memo probes;
- the VS hash read, or its fallback;
- the pair compares;
- the family and exclude list walks.

None of these has a side effect on a hit: a view memo hit writes
nothing, and a miss would only re-record the table's answer.

**Expected.** v:ui-depth on repeat draws is about 0.30 ms of AA-on
work. At about 14.7k repeat eye draws a frame, a hit should cost 3-5 ns
(the call, the resets, two compares), so the saving is about 0.2-0.25
ms a frame. That is about 15% of the AA-on increment.

The other steps with work on repeat draws (v:census 0.12, p:screen
0.13, f:ui-layer 0.11, engine-vel 0.09, v:eye-test 0.08) can share the
aggregate generation later. Each needs its own read like this one
first.

**Verification, built in, temporary.**
- On verify frames (frame % 16 == 8, never a draw-sample frame, so the
  step clock still measures the memo's real cost), a hit ALSO runs the
  full classifier.
- It then compares every output listed above, and checks that the full
  path left at the plain decline. The hologram classifier is checked
  the same way (`g_holoEye`, the return value).
- The full path's outputs stand, so verify frames behave exactly as
  today. Mismatches are counted per output, and the first few are named
  with the VS hash.
- A 30 s line gives:
  - eye draws, hits and the hit rate;
  - misses by cause: the generation moved, the epoch moved, or the last
    exit was not a plain decline;
  - verified hits and their mismatches.
- The pure memo logic (key, hit, store, invalidate, the verify tally,
  the line) goes in a small header with rig checks in aa_off_test.
  binding_shadow's rig gets a check that the aggregate moves with each
  of the eight slots and with no other.

The binding-repeat counter's question is answered, so it comes out of
this build (it costs 0.35-0.4 ms on sampled frames). The step clock
stays.

**The flight should show:**
1. The memo hit rate on eye draws: about 88-90% (the 90.6% repeat share,
   less the few repeats whose last exit was not a plain decline).
2. Verified hits in the millions and mismatches 0. Any mismatch means
   the key misses an input, and the memo does not ship.
3. v:ui-depth with AA on falling from 0.43-0.54 ms to about 0.15-0.25,
   with the other steps unchanged. The old line falls by about the
   same, once the counter's removal is allowed for.
4. ui_depth's own window counts (composite, direct, no shader, world
   screens) the same as before at the same pad, since every positive
   path still runs in full.
5. Flown parked at the same pad, AA on for at least 3 minutes, with no
   build running on the machine. No AA-off leg is needed.

**Memo built (2026-10-27, same branch), NOT FLOWN.** It is as designed,
with these details:
- **Code.**
  - `bindingEyeKeyGeneration()` in binding_shadow, bumped through a
    slot mask in the three setters, and at forget-all and the frame
    boundary.
  - `g_classifierEpoch` in ui_depth.cpp, bumped at:
    - the eviction in surfaceMatches;
    - a real add in addSurface;
    - holoDepthConfigure and uiDepthConfigure;
    - the chrome put;
    - uiDepthFrameBoundary;
    - the shutdown reset.

    A grep found no other writer of the surface table, the lists or the
    view memo.
  - The bodies moved into `classifyEyeDraw` and `classifyHologramDraw`,
    each reporting its exit. The pure logic is in `classifier_memo.h`.
- **Exits.** Only the family gate is a plain decline. A world-screen
  pair that F108 refused and counted falls through to that gate, and it
  is stored as "other", or a hit would skip its count. Both memos read
  their key before classifying, so an eviction during a full run leaves
  the stored key stale, and the next call runs in full.
- **Verify frames.** One ui_depth frame in 16, with the phase rotating
  by one every 16 frames so it cannot lock onto the draw-sample
  cadence. A frame is also skipped whenever `perfMonitorSampleDraws()`
  is true.
- **Logging.** The `ui depth memo:` line follows the step lines at each
  1800-frame edge. The first three mismatches per classifier are named
  in their own lines.
- **Rigs.**
  - aa_off_test now has 186 checks.
  - scrim_metadata_test has 73 aggregate-generation checks: each of
    the 17 slots through each setter, plus forget-all and the frame
    boundary.
  - ui_depth_test and hologram_depth_test have stubs.
  - ui_composite_census_test's source pin reads the new split.

The binding-repeat counter came out in 18aea473, back to af9c429b's
code.

Flight: parked at the Cranfield pad, AA on for at least 3 minutes,
nothing building on the machine. Read the `ui depth memo:` line (hit
rate, verified hits, mismatches), the step lines (v:ui-depth) and
ui_depth's own window counts, after `edvr_log.py --expect-build HEAD`.

**Memo flown (2026-10-27, Steam log `edvr_gfx_20261027_122848.log`,
build 1fc4deb2; same rig).**
- **The flight.** Loaded in docked at Cranfield at 12:30:49, AA on,
  parked until 12:32:47. That gives three clean windows (16200, 18000,
  19800), not the full three minutes. No build ran during the leg: the
  only build writes were three files at 12:29.
- **Hits and verification.** Eye draws hit 90.9-91.1% of the time,
  about 17.3k calls a frame. The hologram hit rate is the same. Misses
  were almost all a moved generation; "no plain decline stored" was
  0.01-0.03% and the epoch moved once. About 5.0M hits were verified
  across the three windows (6.3M with the loading window), with 0
  mismatches in every output and in the exit, for both classifiers.
- **Cost.** v:ui-depth is 0.253-0.263 ms a frame, against a mean of
  0.47 (0.43-0.54) in the step clock flight. That saves 0.21 ms a
  frame, 45% of the step.
  - Of the remaining 0.26: about 0.05 is the AA-off baseline of the
    step's other work.
  - The rest is the hit path, about 12 ns an eye draw for both
    calls: two cross-TU calls, the resets and the temporary tallies.
    That is more than the 3-5 ns estimated.
  - The other steps are the same within the noise between flights
    (v:census 0.14-0.15, f:ui-layer 0.27-0.31, p:screen 0.17-0.20).
    The old line (3.87-4.26) cannot resolve 0.2 ms between flights;
    the step is the read.
- **The positive paths are unchanged.** World screens: seen 3600,
  marked 3600 per window, as in the 10:22 flight. Hologram depth: 7.05-7.16
  listed draws a frame, two resolved eye-frames a frame, 0 declined, as
  before (7.07-7.14).

Verdict: the memo is exact, and it saves about 0.2 ms a frame at a
settlement pad with AA on. To ship it, remove the verification and the
counters, which are temporary. That also takes their cost off the
hit path. Moving the hit test inline (ui_depth.h, like
uiDepthWantsDraws) would save the two calls, but it means exposing the
memo's state in the header.

The other steps on repeat draws (f:ui-layer, v:census, p:screen,
engine-vel, v:eye-test; about 0.5 ms together) can key on the same
aggregate generation. Each needs its own desk read first, as this one
had.

The verification and counters were stripped in b1561dff (the memo
alone). `main` was merged in at 3003f672.

### 2026-10-27 -- Desk design: three more draw-path trims (not built)

There were three reads: v:fixes (the eye recognisers, 0.21 ms in every
mode), f:ui-layer (+0.20 with AA on) and the engine velocity gate
(+0.25). None of the three supports another binding-keyed memo. Each
has a cheaper, exact trim instead.

**Engine velocity** (`engine_velocity.h:163-196`,
`engine_velocity.cpp:3084-3113`).
- The gate in front of the slow half is nine tests, not four:
  - the Vs, Ps, Rtv0, Dsv0 and Blend generations;
  - the VsSrv33 and VsCb1 pointers;
  - the eye;
  - the guarded SRV3.

  It costs about 4 ns a repeat draw, already near its floor.
- A miss enters `beforeDrawSlow`, which reads `qpcNow()` first even
  though only the locked path uses it (line 3112). It then scans the
  14-record family table to find "not a pool family".
- The flight logged about 1.1k such quick paths a frame (1.96M
  "lock-free looks" per 1800 frames) and 340 locked slow halves, which
  is about 0.14 ms of real work.
- The trims:
  - **(A)** read the clock after the quick-path return;
  - **(B)** a one-entry memo of the family lookup, (VS hash, flat) to
    family. That is a pure function, so it is exact;
  - **(C)** move the 1-in-64 PS probe and its scans out of the inline
    gate into an out-of-line function, so the gate is small enough to
    inline at its six call sites.
- About 0.06-0.12 ms together.
- ruled out: keying the gate on bindingEyeKeyGeneration, because it
  follows SRV 0-2 (moved on 8-10% of draws) and not Blend, which the
  gate needs.

**v:fixes** (vscreen.cpp, between the v:census and v:fixes marks).
- Every recogniser but two rejects an ordinary scene draw with inline
  shape tests.
- The two that cost are:
  - `remlokOnFootDiagnose`, a call into another file on every eye draw,
    for one VS hash;
  - `scrimOnEyeDraw`. Its wash shape (X, one instance, count of at
    least 100) is most scene meshes, so the call runs on most eye
    draws, and its decline comes from a one-entry PsSrv0 metadata
    cache inside that call.
- The trims:
  - **(D)** the visor VS test inline, before the call;
  - **(E)** the scrim PsSrv0 cache's hit-and-decline test inline in
    scrim_fix.h, before the call.
- Both are the same predicate moved, so they are exact. About 0.06-0.09
  ms together.
- ruled out: an "all recognisers declined" memo, because the shape tests
  read count and instances, which change draw to draw, and its epoch
  would need about 14 inputs in 9 files.

**f:ui-layer** (forwardWithVerdict, `vscreen.cpp:4249-4320`).
- For an ordinary eye draw the family answer depends only on:
  - the VS hash;
  - the PS hash, read only for nine named VSs;
  - RTV0's target kind, which is cached on RTV0's generation;
  - the on-foot sharp-HUD flag.
- It reads no other mutable state, and kind 2 (post-tonemap, a few dozen
  draws a frame) is the only branch that does more.
- The trim, **(F)**: a one-entry plain-decline cache in vscreen.cpp,
  before `uiLayerFamilyOf`.
  - **Key:** (VS hash, PS hash, RTV0 generation, the on-foot flag), plus
    a small epoch bumped where ui_layer resets its target-kind cache
    (ui_layer.cpp:6632, 6659).
  - **Stored only on:** a proven kNone, at a target kind other than 2,
    and a VS other than the panel or loader VS (those call the family
    probe).
  - It keys on hashes rather than the aggregate generation, because the
    SRVs the aggregate follows are not inputs here.
- About 0.05-0.10 ms.
- The after-UI branch (4289) runs per draw while the layer is
  "watching", with frame-mutable inputs, and no binding key can skip it.
  Its share of the block is unknown, so the build splits the step there.

**The build:**
- trims A-F;
- the stale "four generation compares" comments corrected;
- a step-clock sub-mark between the family decision and the after-UI
  branch;
- a temporary verification of (F) alone (the only new cache with a
  hand-derived key), as the ui_depth memo had: on rotating non-sampled
  frames a hit also runs `uiLayerFamilyOf`, with mismatches counted and
  named;
- a rig sweep of every named VS and PS through the cached path. A-E
  are code moved, not answers cached.

Expected total: 0.17-0.31 ms a frame with AA on, of which 0.06-0.09
(D, E) also helps with AA off. That is well under the 0.5-0.6 the
repeat-draw split promised, because most of the remaining cost is the
call floor and real work on draws whose bindings changed.

**The flight should show** (parked at the Cranfield pad, AA on for at
least 3 minutes, nothing building), against
`edvr_gfx_20261027_122848.log`:

| Step | Before | Expected |
|---|---|---|
| engine-vel | 0.32 | 0.20-0.26 |
| v:fixes | 0.21 | 0.12-0.15 |
| f:ui-layer | 0.28 | 0.18-0.23, with the after-UI share read off its new sub-step |

The other steps should be unchanged, and (F) should show 0 mismatches.

A further candidate, not in this build: inlining the ui_depth memo's
hit test into ui_depth.h. It would save two calls per eye draw, about
0.1 ms, but it needs a read of who else writes the outputs a hit
resets.

**Trims A-F built (2026-10-27, same branch), NOT FLOWN.**
- **A, B, C** (engine_velocity.cpp, engine_velocity.h).
  - A: `qpcNow()` now comes after the quick-path return.
  - B: `quickFamilyForProfile` is a one-entry memo of (hash, flat) over
    the pure family lookup.
  - C: the 1-in-64 probe's scans moved out of line, into
    `psShadowProbeSampled`.
  - The gate's comments now name its nine tests.
  - A's only reader-side effect: `slowTicks` no longer includes the
    quick test on locked draws.
- **D:** `remlokOnFootVisorVsBound()` inline in remlok_fix.h. The
  diagnostic's first line was that same test.
- **E:** `scrimWashSrv0Declines()` inline in scrim_fix.h, over the
  PsSrv0 cache that has moved to `detail`. It is true exactly when
  `scrimOnEyeDraw` would return false from that cache.
- **F:** `uiLayerFamilyMemoised` in vscreen.cpp, with the rules in
  `ui_family_memo.h`.
  - Key: the VS and PS hashes, Rtv0's generation, the on-foot flag, and
    `uiLayerFamilyEpoch()`.
  - The epoch is bumped in refreshLive and at both of ui_layer's
    target-cache resets.
  - A decline is stored only for kNone, at a target kind other than 2,
    for a VS other than the panel or loader VS, and only with the
    on-foot flag the same after the run as before.
  - Its TEMPORARY verification runs on rotating frames that the draw
    clock does not sample, and prints `ui layer family cache:` per
    window.
- **Step clock:** a new step, `f:after-ui`, for the after-UI branch.
  f:ui-layer is now the family decision plus the composite census; its
  second mark runs on composite draws only.
- **Rigs:**
  - aa_off_test, 215 checks, including a sweep of 7,656 named-pair
    draws and 400,000 random draws, cached against uncached, with 0
    differences and 8 mutation controls that each fail;
  - scrim_metadata_test, for E and D;
  - visor_axis_test V8, a wiring pin for D;
  - engine_velocity_test, 18,913 checks, with two new lifecycle checks
    for C;
  - skin_engine_test, with a mutant re-anchored for B.

Flight: parked at the Cranfield pad, AA on for at least 3 minutes,
nothing building.
- Compare engine-vel, v:fixes, f:ui-layer and f:after-ui with
  `edvr_gfx_20261027_122848.log`.
- Read the `ui layer family cache:` line: hits, and 0 mismatches.
- The other steps should be unchanged.

**Trims flown (2026-10-27, Steam log `edvr_gfx_20261027_144901.log`,
build 036e235f; same rig).** Loaded in docked at Cranfield at 14:54:03,
AA on, parked to 14:57:27. That gives six clean windows (23400-32400),
with no build running.
- **F is exact.** The family memo answered 94.6% of 32.1M owner eye
  draws a window. On the verify frames, about 1.78M hits a window also
  ran the full decision, and none named a family.
- **None of the trims moved its step beyond the window-to-window
  spread.** The spread inside each flight is about ±0.05 ms on
  engine-vel and about ±0.2 on EDVR's sum. Means of the six windows
  against the memo flight's three:

  | Step | Memo flight | Trims flight | Predicted | Result |
  |---|---|---|---|---|
  | engine-vel | 0.315 | 0.341 | 0.20-0.26 | no drop |
  | v:fixes | 0.210 | 0.195 | 0.12-0.15 | about -0.015 |
  | f:ui-layer + f:after-ui | 0.283 | 0.309 (0.172 + 0.137) | 0.18-0.23 | no drop |

  The engine motion line's quick-path count is the same as before (about
  1.1k a frame) and so is the slow half (0.13-0.15 ms).
- **Overall.** EDVR's sum is 3.77, against 3.53. That is within the
  spread, with about 3% more eye draws (17.8k against 17.3k).
- **New measurement: the after-UI branch costs 0.106-0.203 ms a frame**
  (mean 0.137) with AA on. It runs on every draw while the layer is
  watching. f:ui-layer's family decision and census are 0.172.

ruled out: trimming nanoseconds from per-draw predicate chains (inline
early-outs, a qpc read moved, the family scan memoised, the gate's probe
moved out of line, a value-keyed family cache) as a measurable lever.
Six exact trims predicted to save 0.17-0.31 ms moved nothing beyond
about 0.015 ms (flown 2026-10-27). The desk estimates were 4-10x too
high: the predictable branches and calls they removed were already
overlapped by the CPU. The ui_depth memo paid off because it removed
real work, four view-memo probes and list walks a draw, not call
overhead.

What this leaves:
- About 1.45 ms of the AA-on increment. It is spread at 2-5 ns a draw
  over a dozen steps, about 35k draws a frame.
- Desk trims of single steps are now below what a flight can resolve.
- The only lever left at this scale is structural: one fast path for a
  repeat draw of an ordinary scene mesh that skips the whole classifier
  chain, both the verdict and the forward scope. The stability check of
  2026-10-27 (no change in the verdict or the ui_depth flags over 1.6M
  repeat eye draws) supports the premise. But every step's per-draw side
  effects (counts, census, probes, the eye-draw tally) would have to be
  separated out first. That is a design of its own, not a trim.

Proposed for the code here:
- **F:** revert it and its verification. It is exact, but it has no
  measured gain, and it adds an epoch across ui_layer.cpp and vscreen.cpp.
- **A-E:** keep them. They are exact by construction, simple, and v:fixes
  shows the small gain they promise at most.
- **The step clock:** remove it when the arc closes.

### 2026-10-27 -- Desk design: the repeat-draw fast path (not built)

Three read-only inventories covered the X thunk with its pre-draw and
post-draw blocks, all of beginPanelOverride, and all of
forwardWithVerdict. Every statement on the path of an ordinary owner X
draw (kNone, unclaimed, AA on) was classed as one of:
- P, a pure decision on listed inputs;
- S, a per-draw side effect;
- G, gated by a normally-off flag;
- L, learning that changes later answers;
- D, alters the issued draw.

The ruled-out ns trims removed slivers of steps. This removes whole
steps for the draws that repeat. It is behaviour, with no key.

**Where the draws go.**
- About 35k hooked draws a frame. 33-35k of them are X; about 17.8k
  are eye draws and about 17k are off-screen.
- 94% of owner draws repeat the previous draw's eight-slot key.
- EDVR's step sum is about 3.6-3.8 ms. Leaving out pre (0.45),
  engine-vel (0.34), weapon and GAME, about 2.8 ms is the verdict, the
  forward scope and the post-draw blocks.

**The ticket.** A full-path owner X draw that ends "ordinary" records a
ticket.
- The key:
  - `bindingEyeKeyGeneration()`. It moves on any VS, PS, PS SRV 0-3,
    RTV0 or DSV0 set, and every frame, so no first-of-frame event (the
    camera latch, the depth probe's first draw) can be a repeat;
  - the eye or off-screen class, which follows from RTV0;
  - `g_classifierEpoch`;
  - the layer's family epoch (if F is kept) and a new after-UI quiet
    epoch;
  - a new fast-path epoch.
- The next owner X draw takes the fast path when the key is equal and
  its args class and the dynamic flags pass.

"Ordinary" is the conjunction the full path observed. Each term is a
deterministic function of the key while the key holds:
- the verdict was kNone, and every per-draw output was at its reset
  value. That means t_uiDepth, t_composite and t_holo, curveThisDraw,
  introCurveThisDraw, glareClamp, the ui_depth pendings, and
  g_uiLayerCrispPending;
- the UI family was none, the layer was not engaged, and the after-UI
  `uiLayerNoteOther` returned with no effect;
- weapon motion did not want the VS. Screen motion did not match: give
  screenMotionUiDraw and screenMotionDraw bool returns for this. The
  rigid-draw query was false: both eyes are already noted this frame,
  and that latch only goes false to true within a frame;
- the VS is not one of the Vs-keyed specials:
  - celestial patch;
  - orbit lines;
  - the panel or loader VS;
  - the HUD issuer probe set;
  - the sun pair;
  - a billboard;
  - the visor.
- the object probe's `g_checksLeft == 0`;
- off-screen only: the target is settled for uiDepthNoteOffscreenDraw
  (known or checks exhausted), and it is not a wake-pulse surface (the
  1.2224 aspect).

**Args class:**
- kind 'X';
- `64 < count <= 10000`;
- `count != 240`;
- `instances >= 1`.

This keeps out every count test the inventories found:
- the count 6 quads: chrome, intro, holo, backdrop, curve;
- night vision at 240;
- the sun-glare nomination above 10000, which also reads VsCb0;
- the panel-size test and the after-UI watch budget, both at 64 or
  below;
- wake pulse's 576 and 1728, already outside the class by its surface
  test.

Every other matcher that reads instances requires exactly 1, so a
larger value only declines more. D, I and N, about 1.5k draws a frame,
stay on the full path in this version.

**Dynamic flags tested on every fast-path draw** (plain loads; any
true value takes the full path):
- `g_cameraDrawWatch`;
- `g_uiLayerIssueBlocked`;
- `g_uiLayerCrispPending`;
- `g_rollFreePending`;
- `g_uiLayerWorldReissue`;
- `g_vrWorldInternal` and `g_vrWorldWants`;
- the step clock's steps-mode draw. The fast path marks a step of its
  own instead.

**The fast-path epoch, and the contract.** The inventories found about
30 normally-off flags: the census and its skips, the quad, pixel,
blend, object-ledger, marker and footprint probes, the camera census,
the gpu pass profile, the layout sample, the experimental FSS keys,
dust and stars hidden, curvature, distance, and others.
- **The config ones.** One epoch, bumped at vScreenRefreshConfig and
  at each runtime arm or disarm site, invalidates every ticket.
- **The contract for future code.** The fast path runs only while
  every subscriber `drawGateSubscribed()` lists is in a certified set,
  meaning its behaviour on an ordinary draw is captured by the key or
  the ticket. A new draw feature added to that list, and not
  certified, turns the fast path off by itself. Nobody has to remember
  the contract.

**What the fast path still does,** in this order. Each item is the S or
D work the inventories required.
- thunk entry as now: `gpuFrameCommandIfLive`, and
  `temporalPassCountDraw` (`++g_cameraDrawCount`);
- the per-draw stores beginPanelOverride makes:
  - `particleRollFreeClear`;
  - `glareClamp = 0`;
  - `curveThisDraw`, `introCurveThisDraw` and the three t_ flags false;
  - `++bindAuditSeq`, running bindingAudit on its 1024th;
- `depthProbeNoteDraw`'s per-draw tallies, with its every-1024th layout
  sample left G;
- eye only:
  - `++eyeDrawsThisFrame`;
  - the crisp reset that `uiLayerCrispNoteEyeDraw` makes;
  - the ui_depth outputs, which are already at their reset values
    after the ticket's plain decline. Confirm by grep that nothing
    else writes them; `g_holoDraw` is read only after a true
    classification;
- off-screen only: `++cands[rtv0Cand].thisFrame`;
- `engineVelocityBeforeDraw` and `NoteSkinDraw`, unchanged. The gate
  stays the substitution's guard: its nine tests cover Blend, VsSrv33
  and VsCb1, which the ticket's key does not;
- the game's draw, under `GpuCensusAlteredScope` with the class from
  `engineVelocityDrawSubstituted()`. A PoolFamily draw keeps its
  occurrence count and sampled timer.

What it skips: the rest of beginPanelOverride, all of
forwardWithVerdict (every scope there is a no-op for kNone), the
lambda's owner block, and the tail's G checks.

**Expected.**
- The fast path should cost 20-30 ns a draw.
- About 75% of the 2.8 ms lands on repeat draws (the 2026-10-27 class
  split). If 80-90% of owner X draws take the path, the saving is about
  0.8-1.6 ms a frame, with AA on and off.
- The trims' estimates were 4-10x high, so this one is checked first by
  counting, before anything is skipped.

**Phase 1, shadow mode (no behaviour change).**
- Build the key, the ticket and the eligibility test. Run the full path
  on every draw.
- On each draw that WOULD take the fast path, compare the full path's
  actual outcome with the ticket's: the verdict, every per-draw output,
  and each ordinary term.
- Count the would-take draws by class, the mismatches by term (the
  first few named with the VS, the PS and the count), and the reasons
  for refusal (key moved, epoch moved, args class, a dynamic flag, the
  certified-set test).
- A `draw fast path (shadow):` line per 1800-frame window.

**Phase 2.** Skip, keeping Phase 1's comparison on rotating non-sampled
frames as the ui_depth memo had. Add the `fast` step to the step clock.

**The Phase 1 flight should show:**
- the would-take share of owner X draws, eye and off-screen apart;
- mismatches at 0 over millions. A mismatch names the input the key
  misses;
- the refusal reasons, to see what limits the share.

Fly it parked, then looking around the settlement and the cockpit for
a minute (UI takes, holo, terminals), and include an AA-off leg.

**Phase 1 built (2026-10-27, same branch), NOT FLOWN.**
- **Code.** `draw_fast_path_shadow.h` holds the key, the args class,
  the ticket, judge and finish, 25 outcome terms, the counters and the
  line. `draw_gate_subscribers.h` holds the gate's 28 subscribers as a
  bit set. The glue is in the X thunk, forwardWithVerdict and the
  lambda. It is TEMPORARY, and it changes no behaviour: the only new
  returns are inside its helpers.
- **Certified subscribers: 18 of 28.** Distance and panel curve are
  certified, not armed, because they act only on draws of 64 indices
  or fewer, which the args class excludes. The curve's paths were
  traced to `srv0IsPanelSized`, `curveThisDraw`, `introCurveThisDraw`
  and the six-index recognisers. Sean's Steam ini has distance 0.65 and
  curvature 0.25, so armed would have read 0%.
- **Defaults the design had wrong.** The footprint is on by default
  (`vscreen_res_width = auto`), so it is certified, and its 2D-screen
  composite VS is a special. The loader panel is on by default, so it
  is armed only on loader-shaped frames.
- **The after-UI term** is a before/after stamp of ui_layer's effect
  counters, leaving out the DSV memo's diagnostic hit and miss counts.
- **Screen motion's** two draw calls now return whether they recognised
  their pair.
- **Rig:** aa_off_test, 321 checks.
- **Cost.** The instrument costs an estimated 40-60 ns an owner X draw,
  so this flight's timings are not comparable. Its counts are the
  result.

Flight: parked at the Cranfield pad for at least 2 minutes, then a
minute looking around the cockpit and settlement (HUD, holo, a
terminal if one is near), then F7 AA off for 1 minute, then F7 back
on. Read `draw fast path (shadow):`:
- the would-take share, eye and off-screen;
- the refusals by reason;
- the mismatches, which must be 0.

**Phase 1 flown (2026-10-27, Steam log `edvr_gfx_20261027_162747.log`,
build a7ff834d; same rig).** Loaded in docked at Cranfield at 16:29:41.
AA was on until 16:31:21, then F7 off until 16:32:46, then on.
- **Draws.** Owner X draws were 18-29k a frame, about half eye and half
  off-screen.
- **Would take: 0.0%,** eye and off-screen, in every window.
- **Refusals, by first reason, a typical scene window (10800):**

  | Reason | Share | Detail |
  |---|---|---|
  | no ticket | 69% | |
  | armed | 15% | all of it "layout" |
  | args class | 9% | |
  | dynamic | 6% | camera watch 4%, object-probe checks 1.5% |
  | key moved | 0.6% | |

- **Tickets recorded:** about 30% of owner X draws.
- **Mismatches:** 0 of the 4.2k draws compared, all in loading
  windows, which is too few to mean anything.

Two causes, both mine:
1. **"Layout" is not an armed state.** `depthProbeLayoutActive()` reads
   `g_layoutTarget`, which the depth probe sets EVERY frame to the
   busiest non-eye depth target (depth_probe.cpp:1116-1118). The sample
   itself runs only on that target's first draw and every 1024th
   (:615), inside depthProbeNoteDraw. The fast path keeps calling
   depthProbeNoteDraw, so the sample runs anyway. Armed-layout refused
   every draw that held a ticket, which is why the share is exactly 0.
   ruled out: depthProbeLayoutActive as a fast-path gate, because the
   kept depthProbeNoteDraw performs the sample (flown 2026-10-27).
2. **70% of draws are not "ordinary" by some term, and the line cannot
   say which.** Tickets are recorded only on ordinary draws, and the
   tally counts terms only on would-take draws, which there were none
   of.

Suspects, each with the evidence that decides it:
- the special-VS term. `particleOnDrawMayMatch` is a conservative "may"
  test, and a VS whose hash is 0 (unknown) may read as a billboard;
- weaponMotionWants on a common off-eye scene VS (AACF..D809 is in both
  lists);
- the after-UI stamp still moving on ordinary draws while the layer
  watches;
- off-screen targets that never settle for uiDepthNoteOffscreenDraw.

Next build (Phase 1b):
- remove layout from the armed set;
- add a per-window tally of why a draw recorded no ticket, by first
  failing term, eye and off-screen apart, with the three most frequent
  VS hashes for the top two terms;
- fly the same leg.

**Phase 1b built (2026-10-27), NOT FLOWN.**
- **Layout.** It is out of the armed set (14 flags remain), and
  `depthProbeLayoutActive` is removed, so depth_probe matches 12c230c3.
- **The no-ticket tally.** Each owner X draw that records no ticket is
  counted under its first failing reason, in this order:
  1. unclassed;
  2. args class;
  3. off-screen unsettled;
  4. wake surface;
  5. then the 23 outcome terms in enum order, from verdict to special VS.

  A draw failing several terms shows only the earliest. The counts are
  kept for eye and off-screen apart, and the tally plus the tickets
  equals the owner X draws.
- **VS hashes.** Each reason keeps an 8-slot space-saving sketch of the
  VS hashes it counted, with 0 as a value of its own.
- **The second line.** `draw fast path (shadow) no ticket:` gives, per
  class, the top six reasons a frame, then the top three VS hashes,
  with their shares, for the two largest reasons.
- **Rig:** aa_off_test, 353 checks.
- **Flight:** the same leg as Phase 1.

**Phase 1b flown (2026-10-27, Steam log `edvr_gfx_20261027_165254.log`,
build 3ef08f21; same rig).** Loaded in docked at Cranfield at 16:55:05.
AA was on, then F7 off from 16:55:52 to 16:56:57, then on.
- **Off-screen draws would take the fast path 23-40% of the time.**
  That is 1.9-5.5k a frame, rising as the scene settled, and 39.9% with
  AA off. The comparison checked 3.3-10M would-take draws a window, 35M
  over five windows, with 0 mismatches.
- **Eye draws: 0%.** Every eye draw recorded no ticket. The first
  failing reasons were:
  - args class: 52-54% of eye draws;
  - census: 45-46%;
  - after-ui: under 1%;
  - ui depth, composite, roll-free, verdict: a few a frame.
- **Off-screen no-ticket** was almost all args class (5.6k a frame),
  then wake surface (about 10), special VS and unsettled (a few).
- **The args class is the largest limiter overall.** It failed on
  10-14k of 22-29k owner X draws a frame (about 47%), eye and
  off-screen alike. Its top VS hashes were 8BC3FFBCA98BCD4C (31-36%),
  EB5234DB6ADB491D (26-31%) and 5B4D8E894EEDA8B4 (10-11%): common scene
  shaders.

Two causes:
1. **The "census" term is an instrument error.** It was captured as
   `g_vrWorldWants || (eyeGeometry && vscreenFootprintWanted())`. That
   is the gate, not the effect. The footprint is on by default
   (`vscreen_res_width = auto`), so the term was true on every eye
   geometry draw. But `footprintEyeDraw` (vscreen.cpp:4138) acts only
   when VS is 5C36AF051B98B9F1 and PS is CFE84157BC76E921, the 2D
   screen composite, which is already a special. Its top VS
   (EB5234DB.., 5B4D8E89.., BBE58E40..) are ordinary scene shaders.
   The term must be what the call does: the world-route census
   (already a dynamic flag), or the footprint's VS and PS pair.
2. **The args class excludes about half the scene's draws,** eye and
   off-screen alike, and the line does not say which bound: count 64
   or under, over 10000, 240, or instances 0. The suspect is the
   10000 upper bound. Big settlement meshes are over 10000, and the
   only test behind that bound is the sun-glare camera nomination.
   That nomination is pointer-cached (vscreen.cpp ~2498: it resolves
   only when VsCb0 differs from `sceneCbNominated`), so on a repeat
   draw with the same VsCb0 it is a no-op. If so, the bound can become
   "count <= 10000, or VsCb0 already nominated, or the sun-glare world
   inactive".

Next build (Phase 1c):
- capture the census and footprint term from the effect, as above;
- split the args-class reason into its four bounds, eye and off-screen
  apart, each with its top three VS hashes;
- for draws over 10000, count how many have
  `bindingGet(VsCb0) == sceneCbNominated` or the sun-glare world
  inactive.

Fly the same leg. Expected: eye draws join, and the split shows how
much of the 47% a widened class would admit.

**Phase 1c built (2026-10-27), NOT FLOWN.**
- **Census term.** It is now `census || (footprintOn && pair)`. Census
  is `g_vrWorldWants && vrCameraCensusWanted()`; the pair is VS 5C36..
  and PS CFE8.. (vscreen.cpp ~1973).
- **The args class** is four reasons, first applying:
  1. count 64 or under;
  2. count over 10000;
  3. count 240;
  4. no instances.

  They are on the main line and in the no-ticket tally, each with VS
  sketches for eye and off-screen apart. The no-ticket order is now 30
  reasons.
- **Draws over 10000** are counted at thunk entry against the
  nomination's own gate (vscreen.cpp ~2641-2651), as one of:
  - glare off;
  - no cb;
  - cb nominated;
  - acts.

  Off-screen draws never reach the nomination.
- **A third line,** `draw fast path (shadow) args:`, gives each bound's
  top VS for eye and off-screen, then the over-10000 shares.
- **Rig:** aa_off_test, 376 checks.
- **Worst-case lengths:** 1051, 981 and 1144 characters.

**Phase 1c flown (2026-10-27, Steam log `edvr_gfx_20261027_174248.log`,
build 03335ec3; same rig).** Loaded in docked at Cranfield at 17:44:42.
AA was on, then F7 off from 17:45:46 to 17:46:47, then on. Two scene
windows (12600 and 14400) are read below.
- **Would take:**
  - eye: 33.2-34.3% (3.5-4.6k a frame);
  - off-screen: 25.5-26.7% (3.3-4.0k a frame);
  - all owner X draws: 29-30% (23.6-28.6k a frame).
- **Mismatches: 0 of 12.3M and 15.6M compared.** The census fix let the
  eye draws in, and none of them differed from the full path.
- **What still refuses** (refusals per window, as a share of owner X
  draws):

  | Reason | Share | Detail |
  |---|---|---|
  | count of 64 or under | about 45% (10.4-12.8k a frame) | the largest by far |
  | dynamic | about 7% | camera watch about 4.5%, object-probe checks about 3% |
  | key moved | about 3% | |
  | over 10000 | about 3% (0.8k a frame) | |
  | count 240 | about 0.2% | |

  On eye draws the outcome terms that block a ticket are now small:
  after-ui about 85 a frame, special VS about 60, ui depth about 15.
- **The 64-or-under draws are ordinary scene shaders.** Eye: EB5234DB..
  51%, 5B4D8E89.. 19%, BBE58E40.. 11%. Off-screen: 8BC3FFBC.. 75%,
  C2208C16.. 11%. They are small meshes drawn instanced.
- **The over-10000 sun-glare nomination never acts.**

  | Class | Glare off | No cb | cb already nominated | Acts |
  |---|---|---|---|---|
  | Eye | 0% | 21-26% | 74-79% | 0% |
  | Off-screen | 0% | 38-40% | 60-62% | 0% (it is eye-only) |

Why the lower bound is too cautious. I set it at 64 to cover every
count test the inventories found:
- the count 6 quads;
- the after-UI watch budget, at 6 or under;
- `srv0IsPanelSized`, at 64 or under: the distance fix, the curve, and
  the after-UI read test;
- the crisp tonemap at 3 and the N-only 3 and 4 shapes, which do not
  apply to X.

Only `srv0IsPanelSized` acts between 7 and 64, and its answer for a
draw under 64 is a memo keyed on PsSrv0's generation, which the key
covers. So a repeat in that range gets the same answer, but only if
the previous draw was in the same range. A draw over 64 never runs the
test.

Next build (Phase 1d):
- **The args class becomes a band that the ticket records,** and the
  next draw must be in the same band:
  - band A: count 7-64;
  - band B: count 65-10000, except 240;
  - band C: count over 10000, admitted only while the nomination is a
    no-op for this draw: glare off, no VsCb0, or VsCb0 already
    nominated. That is a per-draw test, because VsCb0 is not in the
    key.
- **Kept out:** count 6 or under, 240, and instances 0.
- **Re-grep before building:** every count test reachable on an owner X
  draw, to confirm that nothing between 7 and 64 except
  srv0IsPanelSized depends on count. The shadow comparison catches
  anything missed, as a mismatch.

Expected: most of the 45% joins. Most draws repeat their key, so the
would-take share should rise towards 70-80%.

ruled out: the 10000 count bound as needed, because the sun-glare
nomination did not act on one draw over 10000 in two windows. A
no-op test per draw replaces it.

**Phase 1d built (2026-10-27), NOT FLOWN.**

Step 0 re-grepped every count test reachable on an owner X draw: the
thunk, the verdict, the forward scope, the owner block and their
callees, about 40 sites. The edges and exclusions:
- `srv0IsPanelSized` at count 64 (vscreen.cpp:1402, then a memo keyed
  on PsSrv0's generation);
- the nomination at count 10000;
- count 6: intro, curve, holo, target sharp, backdrop, FSS panel,
  chrome;
- count 240: night vision;
- count 6 or under: after-UI's read watch, and particles below 6 or 3.

Two findings changed the plan:
1. **Scrim** (scrim_fix.h:124: X, instances 1, count 100 or over) splits
   bands B and C by count and by instances.
2. **The distance fix** (Sean's ini: panel_distance 0.65) is not
   repeat-stable on a panel-sized eye draw of 64 or under. It learns
   compositeCb on the first such draw and overrides on later ones,
   from VsCb0 and the cb shadow, which the key does not hold.

Both became one per-draw exclusion, "open": the full path would ask
scrim and scrim's PsSrv0 cache cannot decline, or the distance fix is
on and a band A eye draw's PsSrv0 is not known to be non-panel.

Everything else that splits a band is already a term or armed:
- wake pulse's exact counts sit behind the wake-surface term;
- loader 30 is armed on loader frames;
- the body-cull counts are armed;
- the marker captures are armed;
- weapon motion is the weapon term;
- planets are the planet terms;
- holo-motion runs only in term paths.

**Bands as built:**
- A: count 7-64;
- B: count 65-10000, except 240;
- C: count over 10000, while the nomination is a no-op for this draw
  (glare off, no VsCb0, or VsCb0 already nominated).

The ticket records its band, and a would-take draw must be in the same
band. Excluded, first that applies:
1. count 6 or under;
2. count over 10000 with the nomination acting;
3. count 240;
4. no instances;
5. open.

The refusal order is: no ticket, key moved, the exclusions, band
changed, dynamic, armed, not certified.

**Rig:** aa_off_test, 389 checks. **Worst-case lengths:** 1117, 1010
and 1129 characters.

A risk the comparison will show: weaponMotionWants reads
`g.sourceFrame == g.frame`, which can flip mid-frame and is not in the
key.

**The Phase 2 flight should show:**
- the `fast` step's calls and cost;
- EDVR's sum down by the predicted amount;
- the other steps' calls down by the same share;
- ui_depth's world-screen and hologram counts, the engine motion line
  and the depth-probe tallies unchanged;
- no visual change: HUD, holo, terminals, weapons, sky.
