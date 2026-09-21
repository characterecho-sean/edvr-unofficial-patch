# Handoff: L2 draw suppression — build phase

You are taking over the **build phase of L2**: suppressing proven-zero-sample
draws at settlements by feeding the engine's own visibility mask, for the
unofficial Elite Dangerous: Odyssey VR patch. The investigation phase is
done and the decision is **GO** — your job is to decode the remaining
mechanics offline and build the suppression, not to re-run the
investigation.

## Read first, in this order

1. `AGENTS.md` (repo root) — the whole project etiquette: sanctioned tools,
   diagnosis discipline, build gates, git rules, doc discipline. It exists
   because every rule in it was learned the expensive way. Follow it.
2. `docs/engine-render-performance-2026-09-19.md` — read **only the `##
   Status` block**, then the four dated entries `2026-09-21 06:37`, `06:55`,
   `07:29`, `07:45`. The journal is long; the Status block plus those four
   entries is the current state.
3. `docs/settlement-flicker-2026-09-17.md` — its `## Status` block only,
   for the shared-instruments context (the ~23k draws/frame census, ~76%
   zero-sample).

Note: the docs get reflowed by `tools\reflow_notes.py` (bullets become
`*`, underscores/tildes get escaped). Read the current text before any
Edit; do not match stale content from memory.

## What is decided (do not re-litigate)

- The eye pass (18k of ~22k D3D11 draws/frame at a settlement, 82%) is
  **bucket-list draws**. Engine draw items (0x150 bytes) live in per-bucket
  intrusive lists (list head `bucket+0x260`, item counter `bucket+0x2A4`,
  8 items per pool block from pool `DAT_145efdd30`).
- Lists are **rebuilt wholesale when the batch's LOD evaluator reports
  changed masks** (batch clear = `FUN_14434DB60`: `+0x2A0 = key`,
  `+0x2A4 = 0`, list cleanup) and **retained between rebuilds**. `+0x2A4`
  reads the live list size; rebuild churn is small relative to it.
- The feedable suppression point is the **render-record `+0x570` mask
  word** (0x6A0-stride records). It is content-static — copied from the
  source record at ctx build, never rewritten per frame — so a cleared bit
  persists until ctx rebuild. The builder re-tests `+0x570 & passedMask`
  per build; a record whose bits are cleared produces no items at the next
  rebuild.
- **The mechanism is clear-bits + force-rebuild.** Suppression takes effect
  at the rig's next rebuild; natural cadence averages ~40 frames at a
  settlement but is unbounded for static targets, so the build must force
  the victim bucket's clear via the engine's own batch path. Restore is
  symmetric: re-set the bits, force again (the one-frame restore the risk
  line demands).
- Ruled out (do not re-propose): consumption-side instant suppression (no
  reader of the per-item mask array exists anywhere scanned — FindMaskReaders
  found only struct collisions); relying on natural rebuild cadence for
  static targets; reading per-frame production as drawn volume (refuted by
  flight 064511); draw-call identity/motion estimation as a class (kinematic
  arc); compensation-style tweaks (AGENTS.md).

## Your tasks, in order

### 1. Decode the force-rebuild mechanics (offline, no flight)

All evidence is in `analysis/decomp/` (Ghidra outputs, gitignored dir) and
`analysis/ghidra_scripts/`:

- **The clear's key contract.** `FUN_14434DB60(bucket, key)` writes
  `bucket+0x2A0 = key` before clearing. The batch `FUN_144320340`
  (decomp_4320340.txt) passes `plVar10[-6]` and a key read from
  `ctx+0x1A968` (and `ctx+0x1A96C` via `FUN_1442B53F0`). Determine what the
  +0x2A0 key gates (the worker→shared merge `FUN_14434E28F` only merges
  buckets whose +0x2A0 match) and what key value a forced clear must write
  so the next batch refills the bucket rather than treating it as current.
- **The record → bucket reach.** From a victim render record (the
  0x6A0-stride family) to its bucket: the builder `FUN_1442B4420`
  (decomp_42B4420.txt) reaches buckets as `*(model+0x20)` where model =
  `*(entry+0)` and entries hang off the rig owner (count u64 @ owner+0x48,
  array @ +0x50, stride 0x58). Map how a record names its rig owner (the
  dispatch `FUN_144320340` iterates collection records of stride 0x2F0 and
  passes `*(record+0x290)`-family pointers — see its call
  `FUN_1442B4420(*(pcVar9+0x5c), ctx, mask, ...)`), so the build can find
  the bucket from the record the zero-sample census names.
- **The refill trigger.** Confirm that a bucket cleared with the correct
  key is refilled by the next batch with no other intervention (the batch
  runs ~every other frame per collection), and name the exact call or flag
  that forces it — calling `FUN_14434DB60` directly vs setting the
  LOD-dirty input the evaluator `FUN_144331300` consumes.
- Ghidra workflow: edit the RVA array in
  `analysis/ghidra_scripts/DecompileTargets.java`, then
  `cmd //c "analysis\\run_ghidra_decomp.bat"` (2–3 min; check the
  `EXITCODE` line in `analysis/ghidra_decomp.log`). Outputs land in
  `analysis/decomp/decomp_%07X.txt`. Never `cmd /c "C:\..."` a batch by
  absolute path — it prints the banner and exits 0 without running (the
  documented false-green trap). New scan scripts follow the
  `FindBucketOps.java` / `FindMaskReaders.java` pattern with a matching
  `analysis/run_ghidra_<name>.bat`.

### 2. Build the suppression

Only after the mechanics are decoded. Design constraints already
established:

- The zero-sample proof comes from the existing census machinery
  (settlement arc: draws whose query never samples a pixel, ~76% of ~23k
  at settlements). L2 suppresses only draws proven zero for many
  consecutive frames — name that window in the design before building.
- Clear the record's +0x570 bits, force its bucket's clear, let the
  engine's own batch refill without the suppressed items. Restore =
  re-set + force again; prove it on the smoke/static-surface rigs.
- Config keys name functionality, never mechanism (AGENTS.md). Read the
  config-contract gate's expectations before adding any key.
- Every C++ edit compiles through `cmd //c "build\\runbuild.bat" >
  build\build_<tag>.log 2>&1; echo EXITCODE=$?`, then **read the gates in
  the log**: `kinematic_probe_test` (currently 41 checks),
  `kinematic json self-test passed`, `config contract ok: 253`, tail
  `Native OpenXR build and all gates passed.` The probe-side conventions
  (per-session counters, never `active()`-gated, seqlocked job stats,
  fixture values asserted by `tools\kinematic_json_selftest.py`, native
  cases in `tools\kinematic_probe_test\`) are already established — extend
  them, don't invent new ones.

### 3. Flight-verify

- Install only via `python tools\install_edvr.py --target frontier`, then
  `--verify-only`. Never copy onto the game directory by hand. Record the
  installed d3d11 sha256 first-16 (a python hashlib one-liner); the
  `--expect-build` label lags the commit for pre-commit installs — **the
  hash is truth**.
- Flight protocol: one settlement eye burst; read
  `edvr_logs\pool\classification_*.json` → `kinematicEval.bucket_items` /
  `bucket_items_direct` plus the gfx-log depth-probe census. Name
  dead-instrument discriminators **before** flying (e.g. `calls == 0` with
  draws proceeding = hook stood down; the CodeHook install lines in the
  gfx log name each site). Read logs only via
  `python tools\edvr_log.py --target frontier`.
- State the environment the fix depends on when reporting: VR runtime,
  headset, per-eye render size, DLSS version.

## Instruments already in the binary (do not rebuild)

- `kinematicEval.bucket_items` — the FUN_1442B4420 bracket (entry-walk,
  per-call item deltas; calls/items/empty_calls/entry_wild/exit_fault/
  neg_deltas/overflow_calls/max_buckets/max_items_per_call).
- `kinematicEval.bucket_items_direct` — the FUN_144312E00 and
  FUN_14369C9C0 brackets (bucket = param_2; calls/items/neg_deltas/
  read_faults/max_items_per_call per producer).
- The job brackets (jobs[0..5] timing), phys-queue + node captures, and
  the kinematic tracker feed — all per-session, all serialized into the
  classification JSON at eye-dump time.
- Flight-proven: flight 064511 (462k calls, zero faults), flight 073348
  (all three hooks confirmed installed; FUN_14369C9C0 never fired in
  11.7k frames — it is not a per-frame producer in those scenes).

## Repo state at handoff (2026-09-21)

- `origin/main` = **2c25a0c** (contains the L2 decision, the three-producer
  instrument, and all journals).
- Frontier install = the three-producer build, d3d11 sha256:16
  `99885e3a092df662`. The Steam install is untouched by this arc.
- Today's commits: `b200b96` (bucket counter), `e201438` (direct
  producers), `3d97bc8` (flight-2 read + GO decision).
- Git: solo project — branch, commit, merge to main, push, verify with
  `git log origin/main -1 --oneline`. Commit messages to a file +
  `git commit -F` (PowerShell quoting; UTF-8 no BOM). origin/main moves
  often (parallel sessions) — `git fetch` before merging, and if the push
  is rejected, merge origin/main and push again; never force-push.
- Journal every finding as a dated foot entry in the arc's doc
  (`docs/engine-render-performance-2026-09-19.md` for this work, short
  cross-entries in the settlement doc), keep the Status block current,
  and record refuted hypotheses as one-line `ruled out:` entries before
  proposing the next one.
