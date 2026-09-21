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
* **Open, highest value first:** (1) per-record identity and change signal
  in the rig chain (stage 2 — the motion arc's remaining pre-design item);
  (2) where LODDistanceScale gates B's layer selection and why A has no
  gate (stage 1/2 join — offline; the A-layer finding is recorded in the
  engine arc as a design input); (3) optionally close the scheduler's
  runtime-built payload vtables (stage 0); (4) ring-buffer command
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
  need the game's external shader assets, not the binary).
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
  `fix.terrain_cull_guard`.
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
  2026-09-21 (identified, allow-list flight 141800): unknown-A/B are two
  MATERIAL LAYERS of the same pooled settlement-structure system as
  EB52/E508 (t33 12,288-record pool; 16 draw-ranges byte-identical between
  A and B — the same instances drawn twice). A = heavy weathering/detail
  layer (world-space projected detail, deferred G-buffer, PS distance-fade,
  NO LOD gate — ~550 draws/frame submitted unconditionally and invisible
  at range); B = light baked-lit layer (per-instance light-class table),
  LOD-gated. Corrections: the 8-byte stream is the INSTANCE data (vertex
  stride 40 B, skinning idle); "CB-less" resolves to frame constants in
  CB1 + instance data in the t33/t36/t38 SRV pool. Naming A/B visually
  (which specific props) needs a crop flight; the shader identity is
  proven.
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

## Next flight — batched protocol (one settlement visit)

Every open runtime question, one visit. Install the probe build first
(`tools/install_edvr.py --target frontier`, then `--verify-only`); preserve
the live INI unless a change is requested; identity-check logs with
`edvr_log.py --expect-build HEAD`. Instruments, in flight order:

1. **Settings A/B (stage 1 lever):** arm the census from the cockpit,
   landed at the settlement (native VR — the state every settlement
   capture in the evidence base came from), current `Custom.4.4.fxcfg`
   (pass A). Game closed; set MaterialQuality=0, SurfaceMaterialQuality=0,
   LODDistanceScale=0.1; relaunch, same spot and heading, arm again
   (pass B). Discriminators: EB52-family vh= count and total eye draws
   per frame (use `edvr_log.py --tally vh`); outcome (a) family collapses
   → the sliders gate settlement scenery, quantified; (b) unchanged →
   scenery ignores the knobs, no settings-side lever; (c) partial →
   bisect which knob. Validity rule: pass A and pass B must see the same
   scene, viewpoint, AND render path — stay in the ship for both passes:
   disembarking switches to the flat vscreen panel (the scene then renders
   ONCE into the 5120×2880 panel target, which EDVR excludes from
   eye-draw counting), a different path that invalidates the comparison.
   Sanity gate before pass A: 3 armed frames must show ~18k eye draws
   into two 1996×2121 eye targets (native-VR cockpit state); if the scene
   lands in a 5120×2880 target and eye draws collapse to compositor quads,
   you are on the flat panel — stop and re-seat in the cockpit.
2. **EB52-armed EyeDrawSnapshot (stage 4/5 gap):** in pass A, arm the
   snapshot with EB5234DB6ADB491D watched; captures the family's per-eye
   instance-buffer bytes the cb-staged captures cannot see, and names the
   two unknown census hashes if their draws enter the snapshot window.
3. **Scheduler stack capture (stage 0):** `advanced.scheduler_probe = 1`
   in the install INI for the flight (default off); the built probe records
   return-address stacks at FUN_144321940/0x144320340/0x1442df940/
   0x1436a0f50 and reports top-3 stack signatures per target every 20 s.
   Discriminator: the VA recurring at a consistent frame position above any
   0x1462b…/0x145dd… stub is the scheduler; a named .text VA is a candidate
   directly.
4. Environment record for the log: VR runtime, headset, per-eye render
   size (1996×2121 baseline), DLSS version, game build.

Signatures are stated per instrument so the visit cannot come back
ambiguous; each outcome above already names its next move.

## How to update

Arcs keep their own journals under docs/. When an arc's finding changes a
stage here, edit that stage's bullets and the Status block; do not duplicate
the journal. New stages append at the end; do not renumber.
