# Engine render pipeline map

A living index of what EDVR knows about Elite's render pipeline, stage by
stage, with pointers to the arcs that own the detail. This doc is not a
journal and records no new evidence — every row cites its source; when an arc
closes an item it updates its row here and its own Status block.

Analysis baseline: `analysis/EliteDangerous64.exe`, version resource 4.4.1.1,
SHA-256 E6BE8BBE…988 (binary dated 2026-09-19) — the same update line as the
census flights' game build 332841. Prior build 4.4.1.0 = 332753 is recorded in
build-332753.md. All VAs below are for the baseline binary unless a row says
otherwise.

## Status

* **State (2026-09-21, twice updated):** seeded from the L2 trace and boundary
  attribution (engine-render-performance-2026-09-19.md, 2026-09-21 entries),
  the kinematic-motion-injection decision doc, terrain-culling,
  settlement-flicker and canted-projection arcs. Six stages mapped at index
  depth; stages 2, 3 and 4 are the deep ones. Stage 5 (VR frame) moved off
  "least mapped" same day: existing census + EDVRDRW1 captures answered
  emission order, stereo sharing, and the motion write point (see stage 5).
* **Open, highest value first:** (1) Phase 0 of the change-gate design —
  the attribution flight (park >= 60 s, eye-burst, jobs[] dump, plus a
  cpu_profile capture inside job-0; engine_motion + scheduler_probe
  already on): it replaces the extrapolated ~4-5 ms with measured
  settlement numbers, and its gate kills the design cheaply if the job
  wall share is < ~1.5 ms; (2) per-record identity and change signal
  (stage 2 — motion arc, and the change-gate design's bounded dependency);
  (3) scheduler payload-vtable closure (stage 0); (4) ring-buffer command
  consumers.
* **Ruled out (do not re-propose):** boundary-side draw-call motion estimation
  as a class — kinematic-motion-injection-2026-09-19.md (2026-09-19
  decision). Bucket suppression — five independent grounds,
  engine-render-performance-2026-09-19.md. Merge-key or bucket-key control —
  the key is an incidental Wwise counter. The 82% bucket attribution and
  40-frame cadence — no evidence route exists. Exe-embedded render shaders:
  the exe embeds exactly ONE shader, Frontier FGDK's clear_indirect_buffer
  compute utility (.rdata RVA 0x4E27600–0x4E2B200, full sweep 2026-09-21) —
  census families cannot be named from the exe (only armed snapshots), and
  build_diff cannot do shader-recompile detection from the exe (that would
  need the game's external shader assets, not the binary). Draw-dedup
  culling: the v2 ledger proves the 8.08x submission duplication is
  architecture (eyes x parts/LODs x materials), with only ~213/frame
  (~0.3%) fully identical — no dedup lever exists.
* **Next (no flight):** compare the staged VS b0/b1/b2 constants across the
  two eye passes in existing EDVRDRW1 captures (stereo culling share; where
  truth becomes final per eye). **Next (one flight, user's to spend):** the
  settlement settings A/B (stage 1 lever) with armed census plus an
  EyeDrawSnapshot pass A to name the unknown hashes.

## Stage 0 — Frame scheduler

* **Known:** every kinematic/bucket entry point — FUN_144321940/0x144320340
  (worker), FUN_1436a0f50 (reset+repopulate), FUN_1442df940 (per-record
  drain), FUN_14434e20e/0x14434e28f (merge), factories FUN_1442cd610/0x1442cd680
  — is invoked through vtable-table callbacks (Tables A 0x1462b…, B 0x145dd…,
  C 0x145591…). 214 code refs land in Table C, all from the constructors.
  Nothing named in .text calls any of them. Evidence:
  vtable_key_refs.txt, decomp_42CD610/42CD680, engine doc 2026-09-21 trace.
  2026-09-21 (probe flight + decompile): the scheduler is NAMED. Dispatcher
  FUN_1405d6960 (0x5D6960): pops one 0x20-byte queue item, calls the
  payload vtable's run slot. Frame scheduling is FUN_1405d6b20 (job-table
  scheduler: payload array +0xD0, count +0xE8) driven by FUN_1405d6e40
  (per-worker pump); scheduler object at TLS+0x4480, global manager
  DAT_145f27db8. Cadence is QUEUE-EMPTY — producers enqueue and wake; no
  frame counter exists; "per frame" is emergent. The worker storm precedes
  the census window (producers burst, then the pump drains). Static chain
  dead-ends at runtime-built payload vtables (0x1450C7B80/0x1450C7BA0 via
  FUN_1405d3510); closure wants a startup write-watch or enqueue-side
  (FUN_1405d5040) stack capture. reset-repopulate never fires while parked.
* **Open:** who the per-frame producer is (runtime closure above); the
  reset path's cadence (never fired while parked).
* **Lever:** any engine-side timing decision; where truth sampling is safe.
* **Fragility:** VA-bound; table layout will move per build.

## Stage 1 — Cull and LOD selection

* **Known:** projection wrappers RVA 0x4E2F50 / 0x4E2D30; their fov scalar
  feeds a LOD screen-size gate (docs/terrain-culling.md). Render contexts:
  records at ctx+0x40, stride 0x6A0, count ctx+0x1A940; record+0x570 is a
  content/selection mask (copied model+0x8E0 → source +0x560 → record +0x570);
  caches at ctx+0x1A948/950/958/960. LOD evaluator FUN_144331300 computes a
  current active mask (output+0x20) — no change detection; FUN_14430EFE0
  applies distance/LOD + frustum gates. EDVR already widens the frustum via
  `fix.terrain_cull_guard`. 2026-09-21 architectural confirmation: these
  distance/LOD/frustum gates are the ONLY culling — Elite has NO occlusion
  stage; all in-frustum content (both material layers, nested/enclosed
  structures included) is submitted per frame and occlusion-rejected only
  per-pixel by GPU early-z (vertex processing still runs for occluded
  draws; D3D11 does not cull draws). Consequence: frustum-visible-but-
  occluded content costs its FULL eval+job CPU (part of the 5.3 ms wall) —
  an eval-level occlusion stage is the only cull that touches the wall; a
  depth-based visibility study (instrument gap: eye captures are color-
  only) would size it.
* **Open:** closed for the lever — 2026-09-21 step-3 bisect: LODDistanceScale
  alone reproduces the total drop (EB52 -16.9%; material knobs add a further
  ~5.5%); unknown-A (8056C9D5F22007F9) is immune to every knob (triply
  confirmed). What remains here is WHERE LOD gates the EB52/unknown-B
  instance selection (stage 2's eval chain is the likely gate) — an offline
  question, and whether EDVR should offer an LOD-override below the game's
  minimum (a product decision, not yet made).
* **Lever:** scene optimization — the selection gate for the ~78% family the
  census found; the settings A/B (MaterialQuality/LODDistanceScale today at
  max) discriminates it.
* **Fragility:** VA-bound internals; the census family key (vh=) is
  cross-session-stable and survives builds unless shaders recompile.

## Stage 2 — Kinematic eval and rig transforms (motion truth source)

* **Known:** rig → *(rig+0x348) collection (58/58 joined; ~1,572 stable rigs;
  one collection each; one FUN_14431AFE0 dispatch per rig per frame);
  collection+0x280 records, stride 0x2F0. 2026-09-21 correction from the
  transform trace: the per-frame updater FUN_14433DB20 writes record+0x170
  (world position) + +0x17C (packed quat), +0x240..+0x24C bounds, and copies
  +0xF0..0x128 → +0x1C0..0x1F8 (previous-frame products) — record+0x130..0x16C
  is the static local 4x4 (init at record creation, NO reader in any
  render-chain function; do not key motion on it). The render-ward handoff
  is a vtable interface at record+0x2C8 (methods +0x68/+0x70/+0x78/+0x80):
  slot-array fill ((*(record+0x290)+0x50, 0x58-stride groups, 0x20 items,
  FUN_1405db720), notify, bulk update, matrix+position submit (+0x2D0),
  hide. That slot array is the last offline-proven structure; collection+0x18
  is one shared global owner, not per-rig (flight-refuted); the +0x90/+0x1B8
  epoch pair is refuted. Source: kinematic-motion-injection-2026-09-19.md,
  transform trace 2026-09-21 (decomp_433DB20, transform_field_refs.txt).
* **Open:** the identity of the class behind record+0x2C8's vtable — one
  runtime capture of that pointer (e.g. extending the existing
  KinematicEvalProbe) names the implementing class, i.e. the function that
  fills the instanced/pool data; per-record identity (~5 records share one
  node) and the change signal (a transform is new this frame). Everything
  upstream of the vtable call is proven offline.
* **Lever:** cost-effective motion vectors — engine-truth injection (decided
  2026-09-19); replaces boundary-side estimation classes. 2026-09-21: the
  settlement-CPU design landed here — change-gated render-data updates
  (engine doc's design entry), Phase 1 built (config fix.static_prop_updates,
  default off) and flown same day: ABORTED. The gate skipped 91.2% of job-0
  calls with a healthy change oracle and structures flickered because
  job-0's output is CONSUMED per frame — production must run every frame;
  every downstream skip variant is dead. Survivors: (a) cheaper job-0
  (cpu_profile inside it), (b) inside-job change detection with per-frame
  re-emission. Leg-1 jobs[] baseline retained as the cost reference.
* **Fragility:** VA-bound; identity must be re-proven per build by capture,
  per the kVerifiedBuilds rule in build-332753.md.

## Stage 3 — Bucket / batch workers (light-probe flavor)

* **Known:** complete lifecycle from the 2026-09-21 trace: four producers
  (FUN_1442b4420, FUN_144312e00, FUN_14369c9c0, FUN_14434d120 via
  FUN_14434d470) append (0x150 record, u64 key) batch nodes (8/node, pool
  DAT_145efdd30); drainer FUN_14434d790 → run-length accumulator
  FUN_144c837b0 → command enqueue FUN_144c7ef70 into four ring buffers,
  selected by a 3-bit flag; merge keys are copies of DAT_145f27db4 — an
  unrelated Wwise counter bumped unconditionally in FUN_1405d6260; the +0x2B0
  dirty flag has no conditional reader in .text. One producer packs
  transform-derived vectors (light/probe flavor); the subsystem is not mesh
  draws. Evidence: engine doc 2026-09-21 trace entry + decomp_*.txt files.
* **Open:** what consumes the ring-buffer command records (draws vs lights vs
  statistics); the scheduler callbacks that drive reset/drain.
* **Lever:** none for draw count (suppression refuted); a light-cost lever
  only if the ring consumers turn out to be light submissions.
* **Fragility:** VA-bound.

## Stage 4 — Per-eye draw emission (the boundary input)

* **Known:** settlement eye pass ~18.3k draws/frame, offscreen ~4.1k, copies
  ~1.6k; ~78% of the retained eye sample is one instanced sourceMesh family
  (vh=EB5234DB6ADB491D, ~282 verts × ~5 instances; stable across three
  flights), ~9% vh=8056C9D5F22007F9 and ~3% vh=2684F02B9B0BB0DE are unnamed,
  no DXBC on disk. Per-draw identity exists today: vh=/ph= content hashes,
  draw args, VB/IB tokens, viewport; EyeDrawSnapshot stages full VS
  b0–b3 + PS b2 + three IA streams per draw (binary EDVRDRW1 + per-shader
  dxbc); a 4096-draw capture of flight 064511 exists
  (edvr_logs/pool/drawstate_064511.bin, frames 12761+; joins to the census
  via vh). Census caveats: 16384-line cap truncates to the first ~16% of the
  eye pass and frame 0 is copy-flooded — tally frames 1+.
  2026-09-21 A/B flight: EB52's per-eye instance buffers are CAPTURED and
  readable offline (inst_123844_*.bin, 1.57 MB each, 19 frames, 8-byte
  records, first u32 = t33 pool record index); layout confirmed
  INSTANCEANDMODELDATAINDEX + PACKEDVERTEXDATA; both eye targets seen.
  Named from the pool dump: E508648660A352B2 = pool skinned-prop VS (t33
  336-byte records, t38 48-byte bone palette, <=4 bone rows);
  D95905C18B7FAD93 = constant-driven billboard/impostor VS (per-draw CB1
  world basis, +10 z bias). The two unknowns are characterized, not named:
  kind-88 eye draws, pool=1 t33 readers; A ~547/frame median 198 verts,
  B ~207/frame median 24 verts.
  2026-09-21 (identified, allow-list flight 141800): unknown-A/B are the
  pool's weathering/detail (A, heavy world-space-projected deferred layer,
  NO LOD gate, ~550 draws/frame, PS-faded invisible at range) and
  baked-lit trim (B, LOD-gated) materials — same t33 pool, frame
  constants in CB1, instance data in SRVs. Sizing-study CORRECTION
  (same day): A/B are NOT the same instances drawn twice (6.3% overlap).
  LEDGER DECOMPOSITION (PS/RT instrument, flight 184120, v2 ledger):
  the 8.08x duplication is ARCHITECTURE, not waste — ~2x stereo eyes x
  2-4 part/LOD index-range draws per eye (EB52 up to 12) x material
  layers (EB52 carries 4 pixel shaders per eye); every family's repeats
  stay within ONE render target per record-set; the pass map is exactly
  3 render targets (2 eyes + 1 offscreen), ONE pass per eye, NO shadow/
  depth pass in the ledger. The provably redundant dedup target is ~213
  fully-identical submissions/frame (~0.3%) — the draw-dedup lever is
  dead. KEY STRUCTURAL FACT: the game re-walks the same ~9,200 records
  ~8x per frame to emit part x material x eye draws, so keeping one
  record out of the instance stream removes ~8 submissions — the
  culling prize is at the EVAL/instance-stream level, never the draw
  level. Footprint nesting (48% of records) remains heuristic-only; no
  bounds or enterability in any capture (in-game render-record bounds
  +0xF0/+0x1C0, center +0x240).
* **Open:** none for identification. The A-layer gating question is a
  design input: gate A by its own fade distance (~550 draws/frame at far
  LODs, zero visual change) — product decision, suppression discipline
  from the 2026-09-21 trace entry still mandatory.
* **Lever:** motion write-point selection; A/B attribution for stage 1.
* **Fragility:** hash families cross-session-stable; tokens session-local.

## Stage 5 — VR submit and compositing

* **Known (2026-09-21, from census + the drawstate_064511 EDVRDRW1 capture, no
  new flight):** the engine emits each eye as its own pass into its own
  1996×2121 RGBA8 + D32 target, strictly BLOCKED per layer — A×N then B×N,
  never interleaved — with all offscreen/copies first. Shaders, geometry,
  and constant-buffer *pointers* are shared across eyes; per-eye truth is
  produced by re-staging the same buffers between passes: b0 rows 4–7 are
  the per-eye PROJECTION matrix (transposed layout, infinite-far reversed-Z,
  off-axis stereo) — row 4's z term is the principal-point offset ∓0.2425,
  a mirror pair decoding to the Quest 3 frustum (40°/54° vs 54°/40°,
  matching the canted arc's Quest 3 record; rows 9–11 view rotation and all
  other rows are eye-identical to ≥6 dp); instanced families shift
  startInstance by a constant +80148 into one shared instance buffer whose
  b1 payload (336 float4) is eye-independent (8/336 rows carry per-instance
  animation). Draw-time in the immediate context inside each blocked eye
  pass is therefore a single hook point that sees per-eye-final constants;
  the eye is known from the render target / block ordinal, not from buffer
  contents. On this rig (Quest 3, 0.00° cant) no canted term exists in any
  captured family; the canted arc's only measured cant is 10°/eye on Pimax
  8KX. EDVR's own OpenXR runtime observes the frame from the runtime side
  (src/openxr).
* **Open:** on a canted headset (Pimax 8KX) verify the cant term appears in
  the same re-staged b0 rows (environment-dependent; expected per the canted
  arc's fix design); whether the blocked pairs re-run culling per eye or
  reuse one scene list. (Closed 2026-09-21: EB52's per-eye instance bytes —
  captured by the A/B flight and readable offline; see stage 4.)
* **Lever:** the motion write point — one hook, per-eye-final truth at draw
  time, feeding surface 1 then surface 2 (stage 6); any single-pass-stereo
  question (likely out of scope).
* **Fragility:** VA-bound engine internals; the emission structure itself
  is observable from EDVR's boundary without engine RE.

## Stage 6 — EDVR boundary and temporal surfaces

* **Known:** EDVR's motion machinery (mesh/celestial/planet/holo/screen/
  kinematic motion + object/depth probes) is boundary-side estimation: 48+
  flights accrued, gaps remain (smoke-trail voids, 8-frame-stale ship rates,
  skinning spins the pool cannot see). Surface 1 = EDVR's temporal pass (the
  diagnostic); surface 2 = DLSS MV replacement (the end goal). 2026-09-19
  decision: engine-level injection from KinematicRig truth; estimation ruled
  out as a class.
* **Open:** which boundary machinery retires when truth lands; the per-eye
  write point into surface 1, then surface 2.
* **Lever:** the cost-effective motion vectors goal itself.
* **Fragility:** EDVR-owned code — no build fragility, but every retired
  estimator is a regression surface to re-test.

## Cross-cutting

* **Build identity:** EDVR finds targets by behavior, not address; adding a
  build to kVerifiedBuilds is a claim someone re-ran the capture (build-
  332753.md). Each row's fragility note says what re-verification costs. The
  first capture pass is now offline: `python tools/build_diff.py diff --exe
  <new exe>` fingerprints the VA-bound rows against the 4.4.1.1 baseline
  (tools/build_diff_targets.json) — per-target MATCH/CHANGED/UNRELIABLE plus
  a 4 KiB-block .text change map that tells Ghidra where to look first;
  `capture` re-baselines after a verified update. Extend the targets JSON as
  new VA-bound rows land.
* **Tooling:** `python tools/edvr_log.py --tally vh [--frame N]` aggregates
  census draws per shader content hash with per-eye subcounts, percentages,
  and avg n=/i= (landed 2026-09-21; self-tested, reproduces the flight-064511
  reference counts exactly). Census caveat it encodes: per-draw detail dies
  at the 16384-line cap — later frames may exist only as summaries.
* **Flight economy:** stages 0, 2, 4 and the stage-1 A/B all want runtime
  evidence; batch them into as few flights as the discriminators allow
  (AGENTS.md: enumerate before you build).

## Next flight — Phase 1/2 change-gate validation (one settlement visit)

(The batched protocol above was flown 2026-09-21; its questions are
answered in the engine arc. This is the gate build's validation flight —
install the gate build first, `tools/install_edvr.py --target frontier`,
then `--verify-only`; identity-check logs with
`edvr_log.py --expect-build HEAD`.)

Flight context: `temporal_aa = off` for both legs — the user's running
choice until engine-truth motion vectors land, and the measurement-clean
state (pass A's only frame-time confound was a mid-session temporal
toggle; B/C ran off throughout, so off is the comparable baseline).
`engine_motion` and `advanced.scheduler_probe` stay on; same settlement,
cockpit, same parked spot and heading for both legs; >= 60 s parked per
leg; census armed once per leg (~18k eye draws expected); eye-burst dump
at the end of each leg (jobs[]).

1. **Leg 1, gate OFF** (`fix.static_prop_updates = 0`, the default):
   brackets baseline at the settlement. This leg is the folded Phase 0 —
   it replaces the extrapolated job costs with measured settlement
   numbers.
2. **Leg 2, gate ON** (`fix.static_prop_updates = 1`; game closed to edit
   the INI): same spot. The gate's 20 s report lines (calls seen/skipped/
   run, refreshes, invalidations, evictions) plus the bracket delta.

Verdicts, pre-written: (a) bracket delta (job-0+batch wall share) below
~1.5 ms -> the design DIES on the measured number, gate stays off;
(b) census eye-draw counts differ between legs -> the gate broke the
persistence invariant -> ABORT, default off forever, investigate; (c)
brackets drop by the measured share AND census counts are identical AND
frame-cycle improves -> Phase 2 regression pass (flicker instruments,
mover-heavy window, a jump/dock transition to exercise invalidation)
before default-on is even proposed.

## How to update

Arcs keep their own journals under docs/. When an arc's finding changes a
stage here, edit that stage's bullets and the Status block; do not duplicate
the journal. New stages append at the end; do not renumber.
