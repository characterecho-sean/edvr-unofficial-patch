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

* **State (2026-09-21):** seeded from the L2 trace and boundary attribution
  (engine-render-performance-2026-09-19.md, 2026-09-21 entries), the
  kinematic-motion-injection decision doc, terrain-culling, settlement-flicker
  and canted-projection arcs. Six stages mapped at index depth; stages 2, 3
  and 4 are the deep ones. Stage 5 (VR frame) is the least mapped and the
  highest value for both current levers.
* **Open, highest value first:** (1) VR frame layer — per-eye emission order,
  stereo culling share, where truth becomes final per eye (stage 5); (2)
  frame-scheduler identity and cadence — runtime stack capture at the worker
  entries (stage 0); (3) the two unknown census families (stage 4); (4)
  per-record identity and change signal in the rig chain (stage 2).
* **Ruled out (do not re-propose):** boundary-side draw-call motion estimation
  as a class — kinematic-motion-injection-2026-09-19.md (2026-09-19
  decision). Bucket suppression — five independent grounds,
  engine-render-performance-2026-09-19.md. Merge-key or bucket-key control —
  the key is an incidental Wwise counter. The 82% bucket attribution and
  40-frame cadence — no evidence route exists.
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
* **Open:** scheduler identity, thread, cadence. Static RE cannot see through
  the tables; runtime return-address stacks at the entries are the cheap path.
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
  `fix.terrain_cull_guard`.
* **Open:** where the vh=EB5234DB scenery family's instance draw list is
  finalized; whether stereo culling is shared across eyes.
* **Lever:** scene optimization — the selection gate for the ~78% family the
  census found; the settings A/B (MaterialQuality/LODDistanceScale today at
  max) discriminates it.
* **Fragility:** VA-bound internals; the census family key (vh=) is
  cross-session-stable and survives builds unless shaders recompile.

## Stage 2 — Kinematic eval and rig transforms (motion truth source)

* **Known:** rig → *(rig+0x348) collection (58/58 joined; ~1,572 stable rigs;
  one collection each; one FUN_14431AFE0 dispatch per rig per frame);
  collection+0x280 records, stride 0x2F0; transforms at record+0x130..0x16C;
  eval → world-update chain FUN_144331300 → FUN_14433DB20. collection+0x18 is
  ONE shared global owner, not per-rig (flight-refuted); the +0x90/+0x1B8
  epoch pair is refuted. Source: kinematic-motion-injection-2026-09-19.md.
* **Open:** per-record identity (~5 records share one node); the change
  signal that says a transform is new this frame.
* **Lever:** cost-effective motion vectors — engine-truth injection (decided
  2026-09-19); replaces boundary-side estimation classes.
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
  b0/b1/b2 + PS b2 + three IA streams per draw (binary EDVRDRW1 + per-shader
  dxbc). Census caveats: 16384-line cap truncates to the first ~16% of the
  eye pass and frame 0 is copy-flooded — tally frames 1+.
* **Open:** name the two unknown hashes (armed EyeDrawSnapshot pass);
  compare staged constants across eye passes (ties to stage 5).
* **Lever:** motion write-point selection; A/B attribution for stage 1.
* **Fragility:** hash families cross-session-stable; tokens session-local.

## Stage 5 — VR submit and compositing (least mapped)

* **Known:** the engine renders each eye as its own pass into eye textures
  (census r=@ tokens, 1996×2121 RGBA8 + D32); the canted-projection arc
  mapped the projection adjustment (docs/canted-projection.md); EDVR's own
  OpenXR runtime observes the frame from the runtime side (src/openxr).
* **Open:** per-eye emission order; where the canted projection enters per
  eye; whether stereo culling/submission shares any work; where engine truth
  becomes final per eye — the motion write-point question.
* **Lever:** motion-vector truth finality; any single-pass-stereo question
  (likely out of scope).
* **Fragility:** VA-bound; partially observable from EDVR's runtime without
  engine RE.

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
  332753.md). Each row's fragility note says what re-verification costs.
* **Tool gap (recorded):** tools/edvr_log.py has no aggregation mode; the
  census tallies ran as one-off Python. A sanctioned `--tally <field>` mode
  with self-test is the fix when the tally becomes routine.
* **Flight economy:** stages 0, 2, 4 and the stage-1 A/B all want runtime
  evidence; batch them into as few flights as the discriminators allow
  (AGENTS.md: enumerate before you build).

## How to update

Arcs keep their own journals under docs/. When an arc's finding changes a
stage here, edit that stage's bullets and the Status block; do not duplicate
the journal. New stages append at the end; do not renumber.
