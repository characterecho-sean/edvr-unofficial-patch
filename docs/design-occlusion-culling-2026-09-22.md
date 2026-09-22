# Design: dynamic analytic occlusion culling for settlement CPU

**For review.** Author: Kimi Code session with Sean, 2026-09-22. Status: DESIGN
ONLY — nothing implemented. The reader is assumed to have zero context; every
load-bearing claim cites its evidence.

## Status

- **State:** design for external review, one iteration after three measured
  refutations of simpler approaches (evidence §2). Not implemented; no flight
  committed.
- **Prize (measured, not estimated):** ~4.8–5.1 ms of the ~5.3 ms
  view-dependent settlement content cost, at a parked cockpit view — 90.7%
  (r=1 m) to 96.2% (r=0.3 m) of the 12,287 submitted records per frame are
  provably unseen in both eyes (depth-join, run 055252; engine arc
  2026-09-22 entry).
- **Open:** reviewer verdict; Phase 0/1 (offline model + oracle validation)
  not started; walk-vs-emit cost split unknown (Phase 2 instrument exists).
- **Ruled out (do not re-propose; evidence in §5):** draw-dedup culling;
  static/PVS culling; skip-based work gating (the change-gate abort); GPU
  occlusion queries for VR; per-frame depth readback; boundary-side motion
  estimation (2026-09-19 decision).
- **Next:** Phase 0/1 — offline, on already-captured data, no headset.

## 1. Context and goal

EDVR is a `d3d11.dll` proxy plus its own OpenXR runtime for Elite Dangerous:
Odyssey (see `AGENTS.md`). Elite's settlement scenes are CPU-bound. The
project's rule: a lever without a measured cost does not get built; every
stage has a kill gate. This doc designs the only remaining costed lever.

**The wall (all stock-install fpsVR numbers, Sean's tests, recorded in
`docs/engine-render-performance-2026-09-19.md` §cost-baseline):**

| state | CPU ms/frame |
|---|---|
| parked cockpit at settlement, content in view | ~9.5–10 (budget 11.1 @90 Hz) |
| same, on foot (flat panel, single scene render) | 7.8 |
| same, staring at bare ground | 4.2 |
| in space | ~2 |

Decomposition: ~2 ms universal floor + ~2.2 ms planet-surface floor + ~1.7 ms
stereo second pass + **~5.3 ms view-dependent content** (9.5 − 4.2). GPU is
not the wall (6.8 ms, headroom). Draw *submission* is not the wall: removing
3.5k draws/frame recovered < 0.15 ms (passes B/C, mined frame-cycle windows).
The 5.3 ms is eval + kinematic jobs + physics for the content the engine's
gates let through.

## 2. Evidence chain (the load-bearing facts)

1. **No occlusion stage.** The engine's only culling is distance/LOD/frustum
   (decompiles + eval-chain trace, 2026-09-21; pipeline map stage 1). Nested/
   interior structures are submitted whole every frame (user's photographs;
   48% of records are footprint-nesting candidates, sizing study).
2. **Job pipeline is the content cost.** `UpdateRenderDataJob` (FUN_144321940)
   ~432 calls/frame × 55 µs + `RenderDataBatch` ~176 µs × 79 at the settlement
   (L1 brackets flight 193356 + scheduler probe invocation counts) ≈ the bulk
   of the 5.3 ms.
3. **Output is consumed per frame.** The change-gate (skip job-0 for
   unchanged records, 91.2% skip, zero faults) made 93% of structures vanish
   and flicker on the forced-refresh wave: job-0's products are consumed each
   frame — production must run every frame. The job-0 hook point itself is
   proven installable (`0381cff`, default-off, dark). → *Culling ≠ skipping:
   the frame must still be produced — with less content in it.*
4. **One record ≈ 8 submissions.** The 8.08× submission duplication is
   architecture: ~2 eyes × 2–4 part/LOD draws × material layers (v2 draw
   ledger with PS/RT, flight 184120). Pass map: exactly 3 render targets, one
   pass per eye, no shadow pass. Fully-identical dedup target: ~213/frame
   (0.3%) — the draw-dedup lever is dead.
5. **The prize.** Depth-capture join (run 055252, both eyes): 12,287 submitted
   records; 90.7% unseen at r=1 m, 96.2% at r=0.3 m; unseen are near-field
   clutter (7,273 < 300 m; zero > 4 km; only 29 frustum-killed). Prize ≈
   f × 5.3 ms = **4.8–5.1 ms** at the trustworthy radii. Caveats (engine arc
   2026-09-22): the unseen fraction is a CEILING (footprint test under-claims
   visible); it is view-dependent per frame — a static cull would be the
   settlement-flicker class exactly.
6. **The flicker class is defined and instrumented**
   (`docs/settlement-flicker-2026-09-17.md`): identical captured draw state
   became visible next frame; moving NPCs/drones are occluders. Any culler's
   culled set must be a subset of the per-frame provably-unseen set, with
   movers running.

## 3. Design

### 3.1 Offline model (Phase 0 — existing captures, no flight)

- Per-mesh AABBs derived from captured eyemesh vertex data (per-draw,
  mesh-local — noted derivable by the sizing study).
- Cluster boxes: outer-fit unions over the pool's existing 8-record cluster
  blocks (the ledger's `startInstance` +8 stepping shows the allocator's
  blocks), positions from the t33 pool translations (336-byte records,
  translation at +16 — offsets verified empirically).
- Occluder volumes: per structure, INNER-fit boxes (under-estimate the real
  opaque geometry). Two box sets by construction: clusters over-estimate,
  occluders under-estimate.

### 3.2 Per-frame test (CPU only; target < 0.3 ms)

For the frame's view (matrices available at the eval hook site): project
cluster boxes (~1.5–2k) and occluder boxes (~100–300) to screen rects with
per-box nearest depth. A cluster is culled iff some occluder is **nearer than
the cluster box's front** AND its screen rect **fully covers** the cluster's
rect. Rectangle coverage + depth ordering — no rasterization, no GPU, no
readback, no queries; the verdict exists the instant the view is known
(same-frame; movers update naturally from per-frame pool translations).

**Soundness sketch:** real cluster geometry ⊆ cluster box (outer fit); inner
occluder box ⊆ real occluder, so real occluder covers ⊇ inner-box rect; if
inner-box rect covers the cluster rect, then real occluder covers all real
cluster pixels; nearer-than-box-front ⇒ nearer than all real cluster
geometry. ⇒ a culled cluster is genuinely invisible this frame. *Reviewer:
attack this — esp. transparency (§7).*

### 3.3 Integration (Phase 3 — config `fix.occlusion_cull`, default off)

Skip emission of culled records at the proven job-0 hook (per-record loop;
the change-gate's install point). Emission-skip removes the record's ~8
submissions and its compose cost; the eval walk over it remains (bounded by
Phase 2's measurement).

### 3.4 Safety architecture (three nested bounds)

1. **Offline oracle validation (Phase 1):** replay captured frames (the
   2026-09-22 depth dumps) through the analytic test against the measured
   depth. Gate: **zero violations** (nothing culled that wins depth anywhere)
   and reported recall (% of depth-unseen caught). Kill if violations > 0 or
   recall < ~60%.
2. **Runtime bound:** cull only what the analytic test marks occluded; counters
   (culled/run, per-cluster state); census draw-count equality is the
   invariant (abort on any delta).
3. **Flight gates (Phase 4):** parked leg vs pre-written verdicts; moving view;
   movers; a jump transition. Flicker instruments are the outer harness.

## 4. Staged plan

| Phase | Work | Flight? | Kill gate |
|---|---|---|---|
| 0/1 | Volume model + analytic test + oracle validation | No | violations > 0 or recall < 60% |
| 2 | `cpu_profile` inside job-0 (walk vs emit split) | One | harvestable fraction < ~1.5 ms |
| 3 | Emission-skip build, config-gated, counters | No | census inequality in test |
| 4 | Validation flights | Yes | flicker-instrument hits; mover failures |

## 5. Alternatives rejected (with reasons)

| Alternative | Why dead |
|---|---|
| Draw dedup | 0.3% provably redundant (v2 ledger) |
| Static/PVS cull of the measured unseen set | View-dependent; flicker class (measured: head rotation un-occludes) |
| Skip-based work gating (change-gate) | Output consumed per frame — abort, proven in flight |
| GPU occlusion queries | 1-frame-late results → pop-in on fast VR head turns, structurally |
| Per-frame depth readback | GPU→CPU sync stall spends the prize |
| Boundary-draw occlusion culling | Removes GPU only; eval/jobs (the wall) still run; per-record granularity = same safety problem with 8× bookkeeping |

## 6. Risks / open questions (for the reviewer)

1. **Glass/transparency:** alpha-blended windows don't write depth, so the
   depth oracle sees through them — but an opaque analytic occluder would
   cull what's visible through glass. The oracle validation must catch this;
   mitigation is occluder segmentation or accepting lower recall. How should
   the design handle it structurally?
2. **Walk-vs-emit split** decides the harvestable fraction; if the eval walk
   dominates, emission-skip captures only part of the prize. Is there a
   defensible integration point *inside* the walk, and what does hooking the
   evaluator cost in fragility?
3. **Movers:** pool records include animated props/NPC vehicles. The
   per-frame analytic test handles them, but cluster boxes for mover-heavy
   clusters must rebuild per frame. Any coherence concern at 2k clusters?
4. **Version fragility:** the job-0 hook is VA-bound (build 332841 / 4.4.1.1,
   SHA-256 E6BE8BBE…988). The project's `tools/build_diff.py` fingerprints
   the target; is the plan's hook-validation story adequate, or should the
   design require a signature-based prologue check of its own?
5. **Dynamic average:** the 4.8–5.1 ms is one parked pose. The design's
   claimed value assumes the settled-view number holds while parked-ish;
   a view sweep (now cheap: every armed eye run yields depth) is scheduled —
   should its result gate Phase 3?

## 7. Sources

- `docs/engine-render-performance-2026-09-19.md` — the arc journal: cost
  baseline, change-gate design + abort, ledger decomposition, visibility
  prize (2026-09-21/22 entries).
- `docs/engine-render-pipeline.md` — stage map (eval chain, scheduler,
  emission structure, instruments).
- `docs/settlement-flicker-2026-09-17.md` — the flicker class + instruments.
- `docs/kinematic-motion-injection-2026-09-19.md` — engine-truth decision
  (bounds what EDVR does engine-side).
- Captures (local, gitignored): `edvr_logs\pool\` run 055252 (depth dumps,
  pool/inst/ledger), run 184120 (v2 ledger), flight 173802 (jobs[] baseline).
- Instruments in-tree: draw census + `edvr_log.py --tally vh`, v2 draw
  ledger (PS/RT), `EyeDrawSnapshot` (EDVRDRW1), `eye_depth_capture`
  (EDVRDEPT), scheduler stack probe, L1 job brackets, `tools/build_diff.py`.
