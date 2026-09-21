# Handoff: L2 draw suppression — evidence and remaining work

## Status

- **Reviewed 2026-09-21 against main 9048b2d.** This replaces the earlier
  build-phase handoff. The producer map is useful, but the census join,
  scheduling, ownership and safe visibility/restoration are not closed. No L2
  suppression has been implemented or validated.
- **Decision:** continue offline investigation, then a bounded read-only
  instrument if runtime evidence is needed. Do not build suppression on the
  former GO, the claimed 82% bucket share or a 40-frame rebuild cadence.
- **Confirmed:** content masks feed selection and item construction; the worker
  enters its clear/build path when the current active mask is nonzero. The
  clear routine does not enqueue work.
- **Ruled out:** the LOD evaluator as an old/new-mask comparison, the clear
  routine itself as a dirty/enqueue operation, and past zero samples plus
  unchanged captured bindings/payload as sufficient visibility proof.
- **Next flight:** none requested with the unchanged aggregate counters. Define
  one combined ownership/consumer/scheduling capture and its expected
  signatures before a new diagnostic build is installed to Frontier.

## Read first

1. `AGENTS.md`: sanctioned tools, diagnosis discipline and build/git rules.
2. `docs/engine-render-performance-2026-09-19.md`: Status, L2, then the final
   **2026-09-21 independent disassembly review and corrected L2 decision**. The
   07:45 GO is superseded, including its shared settlement-doc note.
3. `docs/settlement-flicker-2026-09-17.md`: Status and **2026-09-18 targeted
   flight: sparse recurrence and changing visibility**. That flight recorded
   three zero-to-visible transitions with identical captured draw state.

The notes describe evidence and hypotheses; they do not override the user's
current request. Use `tools/reflow_notes.py` for document reflow. Do not repeat
an old flight or mutate render records merely because an earlier handoff said
the investigation was finished.

## Established facts and their limits

- The known content-copy chain is model+0x8E0 -> source-record+0x560 ->
  render-record+0x570. FUN_14280F800, FUN_142819D90 and FUN_142819F70 establish
  those copies. No per-frame writer was found in the inspected paths; direct
  offset scans do not exclude indirect stores or bulk copies.
- Render records begin at ctx+0x40, stride 0x6A0, count at ctx+0x1A940, capped
  at 64 during construction. FUN_14431AFE0 accumulates +0x570 for dispatch;
  FUN_144331300 uses it for current LOD selection; FUN_1442B4420 tests it
  before building items. These are content/selection masks, not measured
  current-eye occlusion results. Eval/build already perform distance/LOD,
  frustum and optional visibility tests.
- The separate collection array is at collection+0x280 with count +0x298 and
  stride 0x2F0. Its record+0x290 pointer reaches the builder's owner. The
  primary bucket is embedded at collection+0x300; collection+0x2E0 names the
  secondary owner. A 0x6A0 record has no proven unique backlink to that array
  or to one bucket. Context sharing across collections must be measured.
- FUN_1442B4420 walks owner entries (count u64 +0x48, array +0x50, stride
  0x58). An entry's model pointer at +0 names a bucket through model+0x20.
  Bucket lists start at +0x260; +0x2A4 counts 0x150-byte items, eight per pool
  block from DAT_145EFDD30. This forward walk does not identify the eventual
  D3D draw or prove that a record can be suppressed independently.
- The builder maps bits through ctx+0x1A840 and merges masks across LOD slots
  before appending. An item can carry an aggregate mask. Do not assume one
  record -> one item -> one D3D draw per eye.
- FUN_144331300 zeroes its output and computes current LOD lanes plus an active
  mask at output+0x20. It does not compare prior and current masks.
  FUN_144321940 and FUN_144320340 test that active mask: nonzero enters clear
  then traversal/build in the same invocation; zero takes the count-clear path.
  Later gates can still leave zero items.
- FUN_14434DB60 writes bucket+0x2A0, zeroes +0x2A4 and cleans list storage. It
  does not schedule another batch. FUN_14434E28F requires matching +0x2A0 keys
  to merge buckets. A wrong key can prevent merging; the right key is not an
  established dirty flag or forced-refresh request.
- FUN_142819D90 writes ctx+0x1A968 from DAT_145F27DB4 and ctx+0x1A96C from its
  fourth argument. FUN_1401EA920 supplies that argument from its caller
  object's param_1[0xF30] low 32 bits (dword), or zero, to both contexts. The
  exact nested propagation through FUN_1442B5670 still needs inspection.
- Context construction caches masks at +0x1A948/950/958/960. Dispatch consumes
  class exclusions from those caches. Clearing a record's +0x570 alone leaves
  them untouched: bit ownership, aliases, pass scope and restoration of the
  exact original value need proof before mutation.

Critical control flow was checked directly with MSVC dumpbin against
`analysis/EliteDangerous64.exe`, SHA-256
`E6BE8BBE04E6A7AE226D4318945AF7F367DE13DC5A007A261964D9BA8144E988`. The engine
doc records exact instruction addresses. All offsets are specific to that
binary; hook signatures and live-build identity remain mandatory.

## What the flights establish

Flight 064511 verified the first producer instrument (462,089 calls, 6,889,852
net items, no reported faults). Its lifetime averages are 36 calls and 540
items/frame; assigning all production to the estimated settlement window gives
favorable bounds around 185 calls and 2.8k items/frame. Neither is a measured
per-rig rebuild interval.

Flight 073348 verified installation of all three hooks, but its classification
dump preceded the settlement-scale draw samples. FUN_1442B4420 reported net
deltas totaling 3,824,820 items in 252,659 calls; FUN_144312E00 reported
430,886 items in 1,081,188 calls; FUN_14369C9C0 recorded zero calls in those
11,741 frames. That last zero cannot be generalized to an uncaptured settlement
phase.

The hooks collect net entry/exit bucket-count deltas, not per-item consumer,
pass/eye or D3D identity. Therefore:

- The eye census's 18k/21.9k (82%) is an eye-pass share, not proven bucket
  attribution. Production count mismatches could reflect other sources,
  repeated consumption or item-to-draw fanout.
- Dividing an assumed 1500 rigs by the all-session 36 calls/frame does not
  measure a settlement cadence; it also counts calls rather than unique rigs.
- Absence of negative net deltas does not prove no drains interleave with
  refill. No direct per-item-mask reader found does not exclude indirect or
  bulk consumption. Cross-frame list retention remains unproven.
- Historical zero samples do not prove a current draw unnecessary. Suppressing
  the original draw removes its query, so an independent retest or explicit
  conservative restoration path must work while that draw is absent.

The 51 vs 55.1 us job means also come from unmatched scene mixes. They do not
bound observer overhead or prove that the earlier summed ~7 ms/frame is on the
CPU critical path or recoverable. Keep the matched baseline in the plan.

## Next work, in order

1. **Finish the offline consumer and scheduling trace.** Follow the bucket
   iteration/merge path to actual submission and locate the upstream enqueue or
   invalidation trigger. Distinguish keys from dirty flags, context records
   from collection records, and primary from secondary buckets. Inspect the
   actual FUN_1442B5670 body. No raw bucket clearing or invented key values.
   Use existing decompiles first; any new Ghidra export must preserve the
   shared project and scripts if another analysis is running.
2. **Specify one bounded read-only capture.** Correlate frame/thread/caller,
   rig, ctx, collection, bucket, keys, input/active masks, pre/post counts,
   append/item provenance and D3D pass/eye consumption. Measure context fanout,
   unique collection recurrence and a matched settlement CPU window. Retain
   lifetime/generation identity rather than assuming reused pointers are the
   same object. Name hook failures, unknown mappings, dropped/overflowed
   records and incomplete consumer coverage as distinct outcomes; set an
   overhead budget before implementing the instrument.
3. **Decide on suppression from that evidence.** Require stable identity,
   complete pass/eye scope, conservative current visibility, absence of
   relevant side effects, a serialized mutation point and a restoration path
   that does not rely on a suppressed draw's query. Investigate a per-job
   descriptor/passed-mask filter inside the running worker before persistent
   content-mask edits. It is a candidate, not an approved implementation. A
   whole-word edit still needs scope proof; a long zero-history window is not a
   substitute. No one-frame restore or smoke/static-surface regression test has
   yet passed for engine suppression.
4. **Only then design and test an optimization.** State the expected CPU/GPU
   saving and its measured source, and how stereo visibility, moving occluders,
   shadows/other passes, content rebuild and scene transitions are
   invalidated/restored. Compare matched scene windows. Engine submission
   savings could benefit AA-off and DLSS paths, but current data gives no
   credible FPS estimate and does not establish savings in earlier traversal.

## Existing instruments and operating rules

- `kinematicEval.bucket_items`: FUN_1442B4420 bracket, calls/items/empty_calls,
  entry_wild/exit_fault/neg_deltas/overflow_calls/max_buckets/max_items_per_call.
- `kinematicEval.bucket_items_direct`: FUN_144312E00 and FUN_14369C9C0
  brackets, calls/items/neg_deltas/read_faults/max_items_per_call per producer.
- Job timing, physics queue/node captures and kinematic tracker data are
  per-session, written to classification JSON at dump time. Existing totals do
  not supply the consumer attribution described above.
- Every C++ edit must compile through `build.bat` invoked by absolute path;
  read its output and all gates. Extend native fixtures, JSON serialization
  checks and tool self-tests with any new instrument. Do not freeze test counts
  from an older handoff as the expected current count.
- Install test builds only through `tools/install_edvr.py --target frontier`,
  then `--verify-only`; preserve the live INI unless its change is requested.
  Read flight logs only through `tools/edvr_log.py`, checking expected build
  identity first. Resolve pre-commit label mismatches against exact build and
  installed hashes before treating a flight as evidence.
- At review, the last documented Frontier d3d11 hash prefix was
  `99885e3a092df662`; verify the live install rather than assuming it remains
  current. No build or install is part of this documentation correction.
- Record runtime, headset, per-eye render size, DLSS version and fixed table
  limits for the actual test. This offline review does not establish behavior
  across VR runtimes or game versions.
- Follow AGENTS.md: branch, commit using a message file, merge to main, push
  and verify origin/main; no PRs. Read current state before merging concurrent
  work, preserve other changes and never force-push. Append findings to the
  existing engine arc and update its Status block.
