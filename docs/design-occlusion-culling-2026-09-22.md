# Design: dynamic analytic occlusion culling for settlement CPU

**For review.** Author: Kimi Code session with Sean, 2026-09-22. Revised
2026-09-22 after an in-repo review (its ledger is kept out of tree under
`reviews\`); the sections it corrected are marked *(revised)*. Status:
DESIGN ONLY — nothing implemented. The reader is assumed to have zero
context; every load-bearing claim cites its evidence.

## Status

- **State:** design for external review, one iteration after three measured
  refutations of simpler approaches (evidence §2), then an in-repo review
  that replaced the per-frame test (§3.2), moved the integration point to
  the engine's own frustum-reject path (§3.3), swapped the oracle for an
  exact one (§3.4) and put the one flight that can kill the arc first (§4).
  Not implemented; no flight committed.
- **Prize (re-priced by Phase B', §9):** at the parked cockpit pose
  (three eye runs) 71-72% of the settlement's eye draws come ONLY from
  records unseen in both eyes (R = 1 m; 51-88% over R = 3..0.3 m), i.e.
  6.0-6.1 ms of the caller thread's ~8.5 ms settlement share. The old
  job-pipeline figure (~4.8-5.1 of ~5.3 ms) is dead with Phase A.
- **Open (§9):** B' PASSES on the exact join and on its measurable
  ceiling: a 256x128 coverage buffer fed IDEAL occluders removes 36-41%
  of the pool draws (3.1-3.5 ms against the 1.5 ms bar). It is
  CONDITIONAL on one unmeasured number: the occluders are the front
  buildings' exteriors 30-100 m out (83% of the unseen set; the cockpit
  carries <= 2% of draws at buffer resolution), and a qualified
  inventory of their solid parts must reproduce >= ~45% of the ideal
  (68-79% if EDVR's own per-draw cost were cut to zero: the two levers
  price the same draws). SITE (§9 close-out 1): FUN_144308B30 is a
  per-view distance/LOD test with no planes; FUN_14430EFE0's frustum
  verdict reaches only the type-2 items. The pool draws' only per-view
  frustum is the draw-item builder's view loop (FUN_1442B4420, one view
  array for all three tests), which becomes the candidate site,
  conditional on its items being the instanced pool draws.
- **Next (§9):** fly the gate probe (BUILT, gated, not flown:
  advanced.cull_gate_capture with eye_depth_capture, the eye-run
  hotkey, parked pose): the builder's and the gate's per-view verdicts
  beside each engine record's t33 records, the view array and the
  occluders' geometry; tools\cull_gate_probe.py names the admitting
  test, then the inventory's recall is measured offline.
- **Scope (Sean, 2026-09-22):** cockpit only — true stereoscopic settlement
  rendering from the ship. On foot is out of scope for now; the corpus and
  the profile legs are cockpit poses. Also to instrument: CPU frame time
  rises noticeably once the ship is within ~1 km of the settlement.
- **Ruled out (do not re-propose; evidence in §5):** draw-dedup culling;
  static/PVS culling; skip-based work gating (the change-gate abort); GPU
  occlusion queries as the runtime mechanism (they remain the offline
  oracle, §3.4); same-frame depth readback; boundary-side draw-call motion
  estimation (pipeline doc, 2026-09-19 decision); whole-structure boxes as
  occluders; single-occluder rect coverage as the test. Phase B' (§9):
  the cockpit as the parked prize's occluder; nesting as what hides the
  parked view; the 2026-09-21 "<0.15 ms for 3.5k draws" baseline (§1:
  its windows held no settlement frames); a reject at FUN_14430EFE0 as
  the pool-draw cull (type-2 items only).
- **VERDICT (2026-09-22 evening, valid Phase A): KILL.** Two clean
  file-mode legs on build 2d8fbda (engine arc, "Phase A, valid: KILL"
  entry). Parked cockpit: the job pipeline is 5.24 ms thread-summed on
  seven workers (the ~5.3 ms reproduced) but only 0.81 ms on the caller
  thread's critical path (0.67 ms running, all post-present, + 0.14 ms
  of pipeline-attributed waits; share 0.155): recall x critical path =
  0.78 ms against the ~1.5 ms bar. The caller thread's wall is draw
  submission (11 ms before first submit, 98.6% running, 3.94 ms of it
  innermost in EDVR's own d3d11.dll), which no eval-level cull touches.
  Nothing of §3 is built; §4 stops at Phase A. The arc's successor is
  the EDVR per-draw cost on the caller thread (engine arc Status).

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
2. **Job pipeline is the content cost — with a caveat** *(revised)*.
   `UpdateRenderDataJob` (FUN_144321940) ~432 calls/frame × 55 µs +
   `RenderDataBatch` ~176 µs × 79 at the settlement (L1 brackets flight
   193356 + scheduler probe invocation counts). Those products (~24 ms +
   ~14 ms) are CPU time SUMMED ACROSS WORKER THREADS — "~4–5 ms wall if
   overlap is poor"; the journal marks thread overlap unproven and the L1
   brackets were never placed on the critical path. Whether the main thread
   waits on this pipeline decides whether any of the prize is on the wall.
3. **Output is consumed per frame.** The change-gate (skip job-0 for
   unchanged records, 91.2% skip, zero faults) made 93% of structures vanish
   and flicker on the forced-refresh wave: job-0's products are consumed each
   frame, not persisted — production must run every frame. The job-0 hook
   point itself is proven installable (`0381cff`, the merge of branch
   static-prop-gate-2026-09-21; key `fix.static_prop_updates`, default off).
   → *Culling ≠ skipping: the frame must still be produced, with less
   content in it, and a culled record must take a path the engine already
   takes for records it does not draw (§3.3).*
4. **One record ≈ 8 submissions.** The 8.08× submission duplication is
   architecture: ~2 eyes × 2–4 part/LOD draws × material layers (v2 draw
   ledger with PS/RT, flight 184120; `docs/engine-render-pipeline.md`,
   "LEDGER DECOMPOSITION"). Pass map: exactly 3 render targets (2 eyes + 1
   offscreen), one pass per eye, no shadow pass. Fully-identical dedup
   target: ~213/frame (0.3%) — the draw-dedup lever is dead.
5. **The prize.** Depth-capture join (run 055252, both eyes): 12,287 submitted
   records; 90.7% unseen at r=1 m, 96.2% at r=0.3 m; unseen are near-field
   clutter (7,273 < 300 m; zero > 4 km; only 29 frustum-killed). Prize ≈
   f × 5.3 ms = **4.8–5.1 ms** IF the job time is on the wall (§2.2).
   Caveats (engine arc 2026-09-22): the test is a conservative footprint
   RADIUS per record against the stored per-eye depth, unseen-in-both vs
   visible-in-either; the unseen fraction is a CEILING (the footprint test
   under-claims visible); nothing in the capture says WHAT occludes the
   unseen records — other structures, terrain, or the ship's own hull and
   dashboard, which fill much of a parked cockpit view — nor whether the
   12,287 include the ship; it is one pose; a static cull would be the
   settlement-flicker class exactly.
6. **The flicker class is defined and instrumented**
   (`docs/settlement-flicker-2026-09-17.md`): identical captured draw state
   became visible next frame; moving NPCs/drones are occluders. Any culler's
   culled set must be a subset of the per-frame provably-unseen set, with
   movers running. Movers are identifiable: the engine is mover-blind, but a
   record whose pose bytes at +0x170 never change is engine-proven static
   and one changing every frame is a mover (kinematic doc;
   `kinematic_eval_probe` `xf_movers`); the change gate already snapshots
   those bytes per record.

## 3. Design

### 3.1 Offline model (Phase B — existing captures, no flight) *(revised)*

- **Identity.** Boxes are keyed by MESH identity and attached to the engine's
  record (the job-0 slot + key the change gate keys by), never to a t33 pool
  slot: pool slots are not identities across frames
  (`tools/object_classification.py` limitations; the kinematic arc's
  "pool-slot identity (repacks)" ruling). The join record → mesh(es) is a
  prerequisite to establish offline; the content mask at record+0x570 and
  the ledger's instance stream are the candidates.
- **Poses.** At the cull point a record's pose is its own +0x170 position /
  +0x17C packed quaternion — engine truth, the bytes the change gate
  compares. Not the pool's +16: job-0 itself writes the pool translations
  (kinematic doc: FUN_144312040 is called from job-0), so at job-0 entry the
  pool holds LAST frame's positions. The 336-byte pool record carries scale
  at +4, a unorm16x4 quaternion at +8 and the position at +16
  (`tools/pool_pair.py`).
- **Outer (occludee) boxes.** Per mesh, the AABB over ALL captured LODs and
  parts (the LOD choice is the engine's), transformed by the record's full
  pose — a translated-only mesh-local AABB does not bound a rotated record;
  a bounding sphere is the rotation-invariant fallback. A record whose mesh
  was never captured has no box and is NEVER culled (unknown ⇒ visible).
  Animated/skinned meshes: bind-pose bounds inflated by the animation
  extent, or excluded with the movers (§3.2).
- **Occluder volumes.** Only parts that are INDIVIDUALLY solid and opaque:
  wall/floor/roof slabs, solid props (tanks, crates, rock), the terrain and
  the player's own hull where the attribution below says they matter. Never
  a whole structure: settlement buildings are hollow shells with doors,
  windows and hangar mouths the player walks through, so an inner box of a
  structure encloses empty space and would cull the interior and everything
  seen through a doorway. Qualification is offline and mechanical: the
  draw's PS is opaque (v2 ledger PS/RT; no blend, no alpha test), the mesh
  is closed (every edge shared by exactly two triangles, from the captured
  index buffers) or a slab thicker than the margin; the inner box is then
  eroded by the margins of §3.2. Movers are never occluders.
- **Grouping.** A spatial hierarchy over records (a BVH refit per frame from
  record poses; ~12k leaves is ~0.1 ms) rather than the pool's 8-record
  blocks: the ledger's `startInstance` +8 stepping shows an allocator's
  blocks, but nothing measured says a block is spatially coherent, and an
  incoherent block's union box is covered by nothing. Phase B measures block
  coherence first; blocks are used only if they pass.
- **Attribution.** Before any of the above, Phase B attributes the measured
  unseen set to what occludes it (the nearest depth owner over each unseen
  record's footprint: structure part, terrain, own ship). If the parked-view
  prize is mostly behind the ship's own hull and dashboard, the occluder
  inventory is the ship interior — static per ship type, the safest occluder
  there is — and the settlement model becomes a second stage.

### 3.2 Per-frame test (CPU only, same frame; target < 0.3 ms) *(revised)*

Per eye, with that eye's EFFECTIVE view-projection — the one the game
renders with: `fix.fov_trim_*` and the cull guard narrow the projection the
game is told (`src/openxr/native_cull_guard.h`) — and a record is culled
only if it is occluded in BOTH eyes. Two test forms:

- **Pairwise, exact for convex occluders (the fallback).** Outer box B is
  occluded by inner box O iff B lies inside O's shadow volume: the eye-apex
  frustum through O's silhouette edges (4–6 planes) intersected with the far
  half-spaces of O's eye-facing faces (1–3 planes). Test B's 8 corners
  against those ≤ 9 planes; the eye must be outside O. It needs a broad
  phase (occluders sorted by projected area, screen-tile buckets, hierarchy
  early-out): 2k × 300 pairs per eye is ~40M dot products naively.
- **Coverage buffer (preferred).** Rasterise the eye-facing faces of the
  eroded inner boxes into a small per-eye conservative depth buffer (e.g.
  256×128, farthest depth per tile — the CPU "masked occlusion culling"
  scheme; Intel's Apache-2 library of that name is the reference, built for
  thousands of occluder triangles per frame on one core; its cost here is
  measured in Phase B, not assumed). Then test each outer box: its screen
  rect dilated by one buffer pixel, its nearest depth against the farthest
  stored depth of every tile it touches; occluded iff every such tile is
  fully covered and farther. This gives OCCLUDER FUSION — a box half behind
  wall A and half behind wall B is culled — which no single-occluder test
  can, and cost is O(occluder faces + occludees). The rasterisation is CPU,
  same-frame, no GPU, no readback.

**Margins (all fail-open):** a depth epsilon; one buffer-pixel erosion of
occluders and dilation of occludees; an eye inside an occluder disables that
occluder; a record with no box, or a frame with no validated view, culls
nothing. If the view at the cull point proves a frame stale (§3.3), add a
motion bound: EDVR's own runtime knows this frame's and last frame's head
pose exactly, and the anchor's (ship/body) motion is bounded from the last
two frames' camera; erode occluders by the angular bound, add the positional
bound to the depth margin, and above a cap (a fast turn) cull nothing.

**Hysteresis:** un-cull is immediate; cull only after K consecutive occluded
frames. It does not repair a wrong verdict; it filters verdicts oscillating
at a margin and bounds the flicker rate.

**Soundness sketch (coverage buffer):** real occludee geometry ⊆ outer box
(all LODs, full pose) ⊆ its dilated rect; eroded inner box ⊆ real opaque
solid occluder, and conservative rasterisation marks a pixel covered only
where the eroded face covers it; the stored depth is the farthest occluder
depth in the tile, so "nearest occludee depth farther than every covering
tile's stored depth" ⇒ every real occludee pixel lies behind real opaque
geometry in that eye. Both eyes ⇒ invisible this frame. *Reviewer: attack
the two solidity assumptions (opaque PS, closed mesh) — they carry it.*

### 3.3 Integration (Phase C — config `fix.occlusion_cull`, default off) *(revised)*

**Primary point: the engine's own frustum-reject path.** Stage 1's LOD
evaluator (FUN_144331300) writes a per-record active mask (output+0x20) and
FUN_14430EFE0 applies the distance/LOD/frustum gates; a frustum-rejected
record is simply absent from the mask, and job-0 walks what the mask keeps
(`docs/engine-render-pipeline.md`, Stage 1). Clearing an occluded record
from that mask makes it indistinguishable from a frustum reject to every
downstream consumer — the change-gate abort (§2.3) is what a non-native
skip path costs — and removes BOTH the eval walk and the emission. Two
things must be established from the decompile first: (a) the camera the
frustum gate reads — frustum culling needs the frame's own view, so that
camera IS the same-frame view the test needs (the job-0 hook itself sees
the record pointer and 24 pose/mask bytes, no matrices,
`src/d3d11/static_prop_gate.h`); the per-eye views are that camera composed
with EDVR's own eyeToHead and effective per-eye FOV, which the runtime half
holds (`src/openxr/geometry_snapshot.h`) and must publish across the module
ABI (`src/common/native_frame.h` carries only the head pose today); (b) the
readers of the active mask, which must be render-only — the pipeline doc
records no other consumer, and no proof either.

**Fallback: emission-skip at the proven job-0 hook** (`kinematic-job-0` in
`src/d3d11/kinematic_eval_hook.cpp`, the change gate's install point). It
removes the record's ~8 submissions and compose cost but not the walk, and
at job-0 entry the only view the game offers is last frame's (its VS
cb1[270..273] is written at draw time, which is how `eye_depth_capture.h`
reads it), so this route needs the motion bound of §3.2 and its prize is
the emit share only.

**Hook validation:** the new hook joins the existing VA-bound convention:
`targetValid()` checks the PE timestamp + image size against a hash-verified
baseline AND a 14-byte prologue pattern at the site; a mismatch logs
`identity_mismatch` / `opcode_mismatch` and the feature stands down
(`kinematic_eval_hook.cpp`; `tools/build_diff.py` fingerprints the targets
and is in `build.bat`'s gate).

### 3.4 Safety architecture (three nested bounds) *(revised)*

1. **Offline oracle validation (Phase B) against an EXACT truth.** The
   footprint test under-claims visible (§2.5), so a zero-violation gate
   against it can bless culling a visible record. Two exact per-record
   truths exist in tree: (a) the MeshCoverage eye input — a re-draw writing
   (record index, depth) per pixel, owned only where it equals SceneZ
   exactly (`tools/object_classification.py`, `owned_visible_records`);
   (b) per-draw D3D11 occlusion queries counting passed samples, polled
   asynchronously, 64 slots/frame today (`src/d3d11/original_draw_probe.cpp`),
   generalised to every draw of a captured frame. Violation = a culled record
   that owns ≥ 1 pixel or passes ≥ 1 sample in EITHER eye (with a depth
   tolerance on the ownership test so the truth errs visible). Corpus: ≥ 5
   poses — parked, on foot outside, inside a structure looking out through a
   doorway, walking past a door, in the SRV — every armed eye run yields
   depth. Gate: zero violations; recall reported against the same truth;
   kill if violations > 0 or recall × critical-path share × 5.3 ms < ~1.5 ms.
2. **Runtime bound:** a SHADOW mode computes and counts verdicts and applies
   none; the census draw count with shadow mode on must equal the count with
   the feature off (the Phase-C gate). That equality is a manual procedure
   via `dump_draws` and `tools/diff_draw_census.py`, not code — nothing in
   `draw_census.cpp` asserts it. Live mode: counters (culled/frame,
   per-record state, hysteresis transitions), and the draws removed per
   frame must equal the ledger-predicted submissions of the culled records.
   Kill switches: hook validation failure, no validated view, any census
   inequality in test ⇒ stand down for the session, logged.
3. **Flight gates (Phase D):** parked leg vs pre-written verdicts; moving
   view; movers crossing occluders; on foot through a doorway; an SRV
   drive; a jump transition. Flicker instruments are the outer harness.

## 4. Staged plan *(revised)*

| Phase | Work | Flight? | Kill gate |
|---|---|---|---|
| A | ONE instrumented flight: sampled CPU profile with context switches (`tools/cpu_profile.py`; the WPR profile records SampledProfile + CSwitch/ReadyThread stacks) → job-0 walk-vs-emit split by instruction address, and whether the main thread waits on the job pipeline; depth + MeshCoverage/occlusion-query truth at ≥ 5 poses | One | main thread does not wait on the pipeline; or recall ceiling × critical-path share × 5.3 ms < ~1.5 ms |
| B | Offline: attribution of the unseen set; solid-part inventory; boxes and the identity join; block coherence; recall of pairwise vs fused test; oracle validation | No | violations > 0; recall × share × 5.3 ms < ~1.5 ms |
| C | Build: shadow mode, counters, config-gated, hook validation | No | census inequality in test |
| D | Validation flights | Yes | flicker-instrument hits; mover/doorway failures; hysteresis pops |

Phase A goes first because it is the cheapest kill: the instrument exists
(the sampled profile attributes by address, so no in-tree span is needed),
and if the pipeline is off the critical path no offline work is worth doing.

## 5. Alternatives rejected (with reasons)

| Alternative | Why dead |
|---|---|
| Draw dedup | 0.3% provably redundant (v2 ledger) |
| Static/PVS cull of the measured unseen set | View-dependent; flicker class (measured: head rotation un-occludes) |
| Skip-based work gating (change-gate) | Output consumed per frame — abort, proven in flight |
| GPU occlusion queries at runtime | 1-frame-late single bits: nothing to make conservative under a fast VR head turn. They remain the right OFFLINE oracle (§3.4) |
| Same-frame depth readback | Stalls. An ASYNC readback does not (`eye_depth_capture.h` maps DO_NOT_WAIT up to two frames later); its real objections are movers baked into old depth (an NPC vehicle that has driven off still culls what was behind it) and a dilation that must scale with head velocity. Reprojected previous-frame depth stays the fallback if the occluder model cannot be made solid: it needs no occluder model and handles glass, doors, terrain and the hull by construction |
| Whole-structure inner boxes as occluders | Hollow shells: cull the interior and the view through a doorway |
| Single-occluder rect coverage + nearest-depth test | Unsound: a projected box's rect contains pixels its silhouette does not, and "occluder nearest depth < occludee front" says nothing about the occluder's depth at the covering pixels (a wall running away from the eye "covers" a crate beside its far end). And no occluder fusion |
| Boundary-draw occlusion culling | Removes GPU only; eval/jobs (the wall) still run; per-record granularity = same safety problem with 8× bookkeeping |

## 6. Risks / open questions (for the reviewer) *(revised)*

1. **Solidity, of which glass is one case.** Opaque PS + closed mesh is the
   offline qualification (§3.1). A wall with a window is not closed at the
   window if the glass is a separate draw, and is closed if it is one mesh
   with a glass material — the oracle catches the second at a recall cost.
   Is there a structural test better than those two?
2. **Critical path.** The job time is thread-summed and overlap is unproven;
   Phase A's context-switch trace decides. If the main thread does not wait
   on the pipeline, the prize is not on the wall.
3. **Movers:** excluded from both roles in v1 — never occluders, never
   culled while their +0x170 bytes changed in the last K frames. Recall loss
   accepted; the prize is near-field static clutter (7,273 records < 300 m).
4. **Version fragility:** answered by the `targetValid()` convention (§3.3);
   the new hook must carry its own prologue pattern.
5. **Dynamic average:** the 90.7–96.2% is one parked pose; the ≥ 5-pose
   corpus of §3.4 gates Phase C and the prize is re-stated per pose.
6. **The active mask's readers** and the frustum gate's camera source (§3.3)
   are decompile work; if the mask feeds anything but draws, the fallback is
   emission-skip with the motion bound.
7. **Identity join** record → mesh (§3.1) is unestablished; without it no
   outer box can be attached and nothing can be culled.

## 7. Sources

- `docs/engine-render-performance-2026-09-19.md` — the arc journal: cost
  baseline, change-gate design + abort, thread-sum caveat, visibility prize
  (2026-09-21/22 entries).
- `docs/engine-render-pipeline.md` — stage map (eval chain, active mask and
  frustum gate, scheduler, emission structure, ledger decomposition,
  per-eye constants at draw time, the boundary-side ruling).
- `docs/settlement-flicker-2026-09-17.md` — the flicker class + instruments.
- `docs/kinematic-motion-injection-2026-09-19.md` — engine-truth decision;
  mover identity by +0x170 delta; the pool written from inside job-0.
- Captures (local, gitignored): `edvr_logs\pool\` run 055252 (depth dumps,
  pool/inst/ledger), run 184120 (v2 ledger), flight 173802 (jobs[] baseline).
- Instruments in-tree: draw census + `edvr_log.py --tally vh`, v2 draw
  ledger (PS/RT), `EyeDrawSnapshot` (EDVRDRW1), `eye_depth_capture`
  (EDVRDEPT, asynchronous), MeshCoverage ownership
  (`tools/object_classification.py`), per-draw occlusion queries
  (`src/d3d11/original_draw_probe.cpp`), pool record decode
  (`tools/pool_pair.py`), scheduler stack probe, L1 job brackets,
  `tools/cpu_profile.py` (WPR sampled + CSwitch), `tools/build_diff.py`,
  `targetValid()` in `src/d3d11/kinematic_eval_hook.cpp`.

## 8. Re-scope after the valid Phase A (2026-09-22 evening, Sean's call)

The Phase A KILL stands for the prize this document priced: the job
pipeline's CPU (§2.2) is 5.24 ms thread-summed on seven workers and
0.81 ms on the caller thread's critical path (engine arc, "Phase A,
valid: KILL"). The same flight measured a different prize on the same
records, and the re-scope targets that one.

**Prize: draw submission on the caller thread.** The parked cockpit's
caller thread runs 15.1 ms per frame; the settlement's share of it,
read as the difference between 5.3 km out and parked on the approach
leg, is ~7 ms before first submit plus ~1.5 ms after Present, and it
scales with what the settlement submits (R1 ramps with the job-0 sample
count, r = 0.97). Every admitted record becomes draws; the depth join
of §2 measured 90.7-96.2% of the settlement's submitted records as
unseen in both eyes from that view. A cull at admission therefore
removes draw submission on the critical path (and the GPU vertex work
early-z never saves), not just worker time. Even at half the measured
recall the recoverable time is several milliseconds against the same
1.5 ms bar.

**Site: unchanged from §3.3** - the frustum-reject decision of the
distance/LOD gate (FUN_14430EFE0). A record rejected there is
indistinguishable from one outside the frustum, a state the engine
already handles every frame as the view moves; the engine-state risk is
the one the engine takes for itself. The oracle risk (a wrong reject is
a missing object, the settlement-flicker failure class) is unchanged
and so is §3.4: shadow mode with counters before any reject, inflated
bounds and hysteresis, the per-frame test under 0.3 ms for 12k records,
the flicker instrument and census equality as validation gates. The
engine's cull body is not replaced; the reject is added in front of it.

**Gates, replacing §4 from Phase B on:**

| Phase | Work | Flight? | Kill gate |
|---|---|---|---|
| B' | Offline: the record -> draw join (depth corpus run 055252 + census), so unseen records are priced in draws; the unseen share of the caller thread's draw time at cockpit poses; the oracle's camera source (§3.3) | No | unseen share x recall x settlement draw time < 1.5 ms, or no join |
| C | Build: shadow mode, counters, config-gated, hook validation | No | census inequality in test |
| D | Validation flights, the same two legs as Phase A (parked, then the approach), compared with the analyzer's R1 module split | Yes | flicker-instrument hits; mover/doorway failures; hysteresis pops |

Two things precede B' and are independent of it: EDVR's own per-draw
path (3.94 ms per frame of EDVR leaf time on the caller thread at the
same view, named in the engine arc's 2026-09-22 per-draw entry) is pure
overhead and is being cut first; and the GPU side at Pimax resolution is
unmeasured in the CPU trace and must be read next to any CPU saving.

## 9. Phase B' (2026-09-22, offline): the join is exact and the ceiling clears the bar; the occluders and the site are open

**Corpus and method.** Three armed eye runs of the one parked cockpit
pose at Cranfield (055252, 082019, 094038; Pimax OpenXR, two render
scales). Their record distances (p10/p50/p90 91/158/356 m) agree to
1 m: they replicate ONE pose, they are not the >= 5-pose corpus of
§3.4. Frame 2 of each: the v2 ledger (every eye draw: VS, PS, RT token,
start and instance count), the instance stream (8 bytes an instance,
first u32 = the t33 record), the pool (position at +16), both eyes'
depth (R32, reversed-Z, near 0.025 m) and, for the camera, the eyemesh
snapshot's VS b1: registers cb1[270..273] are the clip matrix's columns
and cb1[275] the eye origin in the record frame (the game's VS computes
clip = M (pos - cb1[275]); vs_EB5234DB6ADB491D, instructions 101-141).
Eye, camera and depth pair by self-consistency (record origins landing
exactly on their own depth: 36-54 hits right, <= 13 wrong). Pool draws
are the eye draws whose VS reads t33 (21 dumped VS, by disassembly)
plus the undumped families whose instance ranges come from the pool's
allocator: 5B4D8E894EEDA8B4 and BBE58E40FE88EC80, which the flicker
arc's disassembly also shows reading t33. That is 18,005-18,577 draws a
frame, 98.5% of the eye pass. Visibility is §2.5's footprint test (a
sphere of radius R about each record origin against the stored depth);
on 055252 it reproduces 96.2% / 90.7% as 96.5% / 88.9% (12,199
records; the earlier union counted 12,287). The scripts are throwaway,
in the session's scratch.

**1. The record -> draw join is exact** (every eye draw names its pool
records through its instance range), so neither bound is needed. R =
1 m, the trustworthy band of §2.5; brackets span R = 0.3..3 m:

| per frame | 055252 | 082019 | 094038 |
|---|---|---|---|
| pool eye draws / records | 18,200 / 12,199 | 18,005 / 12,181 | 18,577 / 12,220 |
| records unseen in both eyes | 88.9% [96.5..75.6] | 89.1% [96.5..76.1] | 88.9% [96.4..75.2] |
| draws ALL of whose records are unseen | 70.9% [88.1..51.3] | 71.8% [88.0..52.6] | 71.5% [87.7..51.2] |
| draws mixing seen and unseen records | 25.3% | 24.2% | 24.5% |
| instances of unseen records | 87.6% | 87.7% | 87.7% |
| draws per record, unseen / seen | 7.44 / 8.43 | 7.15 / 8.25 | 7.55 / 8.48 |

Per family (055252, R = 1 m; share of the pool draws, then all-unseen /
mixed / all-seen): EB52 58.1%, 70.4 / 26.2 / 3.5; 5B4D 19.7%, 64.7 /
31.8 / 3.5; BBE5 9.2%, 72.3 / 24.5 / 3.1; 8056 3.3%, 78.9 / 19.9 / 1.2;
7B0D 1.9%, 96.0 / 0 / 4.0; 2684 1.5%, 72.3 / 19.3 / 8.3; 23 others
6.3%, 81.2 / 9.6 / 9.2. The other two runs agree within 3 points except
7B0D (86%). Every family is mostly unseen. The bounds the brief asked
for land where the evidence puts them: unseen records carry 0.87-0.89
of the seen records' draws, so uniform multiplicity (the record share,
88.9%) holds for instances and half multiplicity (80%) is refuted. The
draw share (71%) sits below both because a quarter of the draws mix
seen and unseen instances. It is the conservative price (a draw goes
only when every instance goes); re-batching the survivors would raise
it to 85%. An exact truth at other poses agrees: the flicker arc's
per-draw query census found ~76% of stable-settlement draws passing
zero samples (its Status), inside the R = 0.3..1 m band here.

**2. The gate**, on the caller thread's ~8.5 ms settlement share (§8),
per removed draw:

| | removed pool draws | x 8.5 ms |
|---|---|---|
| depth truth (the ceiling), R = 1 m | 70.9-71.8% | 6.0-6.1 ms (4.4-7.5 over R) |
| §3.2 ceiling: 256x128 buffer, IDEAL occluders | 35.9-41.2% | 3.1-3.5 ms (1.9-3.9 over R) |
| same, 256x256 | 46.0-52.8% | 3.9-4.5 ms |
| cockpit-only occluders, even at 512x512 | <= 1.9% | <= 0.16 ms |

The ideal-occluder rows are the most the §3.2 test can deliver: the
stored depth itself as the occluder, the farthest occluder depth per
tile, the occludee's rect dilated by one tile, culled only if occluded
in both eyes. They cull no record outside the truth, and recall 72% of
its records and 53% of its draws at 256x128 (055252). **Verdict: B'
passes** — the join is exact and the ceiling clears the 1.5 ms bar by
2.0-2.3x at R = 1 m (1.3-1.7x at the pessimistic R = 3 m) — **and it is
conditional on one number this corpus cannot measure:** the qualified
occluder inventory must reproduce >= 43-49% of the ideal removal (R =
1 m; up to 78% at R = 3 m). Two things price it down. (1) The 8.5 ms
predates the per-draw cut and holds ~3.2 ms of EDVR's own per-draw time
on the settlement's draws (Phase A's R1 EDVR leaf 3.94 ms parked against
the gfx log's draw-hook line at 0.5-1.0 ms away from the settlement). At
zero EDVR cost the same draws are worth ~5.3 ms, the ceiling 1.9-2.2 ms
and the requirement 68-79%: the cut and the cull price the same draws
and do not add. (2) The gate's unit is the engine record (stride 0x2F0,
below), not the t33 record priced here. One record's sphere bounds all
it emits and the link between the two is unrecorded (§6.7), so the
engine-record recall can only be lower.

**3a. The camera at the gate (§3.3 (a), from the decompiles in
analysis\decomp).** FUN_14430EFE0's third argument IS the view. The
traversal FUN_144312040 walks the render context's view array
(ctx+0x40, stride 0x6A0, count ctx+0x1A940) and calls the gate once per
view for each engine record (decomp_4312040.txt:303-304). In the gate
(decomp_430EFE0.txt): the view's bits +0x570 against the incoming mask
(line 76); the record's world sphere (record+0x240 centre; +0x280, the
pair plane distances are compared against — the extents the flicker
arc lists as unlocated) against the view's frustum (lines 84-90;
FUN_1404F4E10 reads float4 planes at view+0x30 and a u16 count at
view+0x44, -1 = outside: decomp_04F4E10.txt:54-78); distance and
screen size from the view's camera position view+0x540 and LOD scale
+0x550/+0x560 (lines 92-128). So the camera is a context field handed
to the call — not a constant buffer (cb1[270..275] is written at draw
time, after the gate) and not job-0's arguments. The draw-item
builder's Level-4 frustum loop reads the same array (FUN_1404F4E10 from
FUN_1442B4420). The eyes are separate entries: every record drawn in
one eye only projects outside the other eye's rendered viewport
(055252: 358 and 152 records, 100%), and records drawn but centred
outside their own viewport straddle its edge (median 0.013 NDC), so the
engine culls per eye with the rendered projection. From any call the
other eye's entry is in the same array (ctx = **gate): per-eye and
both-eyes verdicts are both available (the per-eye rule adds 1-2.5
points of draws here). Unknown until a dump: which index is which eye,
whether the entry stores the full view-projection (its planes and
+0x540 determine it) and whether the planes are this frame's.

**A correction to the site (§3.3, §8), from the same decompile.** The
gate's per-view verdicts (local_288, lines 206-321) feed only the
type-2 item path (FUN_142817260 -> FUN_144312E00, lines 326-337). The
record's stored view mask rec+0x208, Level 3's dispatch input toward
the draw-item builder FUN_1442B4420, is local_290 (line 346), fixed
BEFORE the gate loop by the per-view LOD-nibble filter (lines 137-149)
and FUN_144308B30 on the record's own sphere (lines 156-165; not
decompiled). Which of the three per-view tests (FUN_144308B30, the
gate, Level 4) admits the instanced eye draws is unproven; the
2026-09-21 trace found no route from either item list to the D3D
draws. Clearing a bit at FUN_14430EFE0 may therefore not remove the
pool draws at all. The camera answer holds for all three; the site does
not yet.

**3b. Occluders.** By the nearer eye's surface at each unseen record
(R = 1 m, three runs): surfaces >= 30 m out 83.0-84.1%, the cockpit
(< 3 m: canopy frame, dashboard, consoles) 15.1-16.4%, own hull and pad
(3-30 m) 0.5-0.6%. The >= 30 m occluders are the settlement's own
structures, not bare terrain (055252, eye A): 93.4% of their surface
points lie within 5 m of a SEEN record's origin (control, all >= 30 m
texels: 29.3%); 61% face sideways (wall-like normals), 21.5% are
ground-like (ground, pads or roofs), and 5.7% are ground-like AND away
from any seen record (the bare-terrain candidates). They stand far in
front of what they hide: median 73 m, within 2 m for 0.1-0.2%. The
parked view's unseen set is behind the settlement's FRONT — the
exteriors of the nearest buildings 30-100 m out — not nested inside
each record's own shell. The nearest-seen-origin association names
~310-340 occluder records; ~25 carry half of the structure class, the
top 10 a third. §3.2's representation (a coverage buffer rasterising
eroded inner boxes of individually solid opaque parts; whole-structure
boxes are rejected in §5) therefore needs the front buildings' wall and
roof parts, and this corpus cannot qualify them: no record -> mesh
identity, no index buffers (no closedness test), vertex data for 127 of
the snapshot's 4,096 draws (its 32 MB cap), no per-pixel ownership.
The occludees need no offline model: the gate already holds each
engine record's sphere.

**3c. Cost (instruction count; Phase C measures it).** Occludee test
per (record, eye): transform the centre (4 SIMD FMAs), divide, rect
from the radius, clamp to tiles (~25 instructions), then the depth
compares over the dilated rect. At 256x128 a tile is ~8x17 eye pixels
and a 1 m sphere at the median 158 m spans 1-2 tiles, so 9-12 tiles,
2-4 SIMD compares: ~50-60 instructions, ~20-25 cycles cached. 24k
tests (12k records x 2 eyes; fewer at the engine-record unit) are
~0.1-0.15 ms thread-summed, on the job workers where the traversal
runs, not the caller. Occluder rasterisation, once per eye per frame:
~300 parts x <= 3 eye-facing faces x 2 triangles is ~2k triangles an
eye, each over a few 8x4 blocks at this size: ~0.05-0.3 ms for both
eyes, depending on SIMD batching. Total ~0.15-0.45 ms against the
0.3 ms target. The rasterisation must finish before the traversal
starts; on the caller thread it would be paid in R1.

**4. Safety at the site (§3.4 mapped).** (1) Oracle: this depth join is
a ceiling (§2.5), not the exact truth. The site's unit is (engine
record, view), so validation must chain engine record -> t33 records
-> draws (this entry's join) -> per-draw truth (passed samples); the
first link is unrecorded. (2) Shadow mode, a hook in front of the
admitting test: after the engine's own verdict passes, compute the
occlusion verdict for that view and apply none. Counters per frame and
per eye view: calls, engine passes, would-reject; fail-open by reason
(no validated view, eye inside an occluder, a mover whose +0x170
changed in the last K frames, a non-eye view); hysteresis promotions
(after K frames) and immediate demotions; the draws predicted removed
(the would-reject records' draws through the instance stream the draw
hook already reads). The counter that makes a wrong reject provably
absent: every eye draw whose instances are all would-reject in that
eye runs inside an occlusion query; a violation is a query with passed
samples > 0. It must read zero over the whole corpus before any real
reject, with the queries dropped for want of slots reported beside it
(original_draw_probe.cpp holds 64 a frame against ~6-13k would-reject
draws: a rotating sample, or a wider pool). Census equality shadow-on
vs off stays the Phase C gate. (3) The flicker instrument, the flicker
arc's per-draw query census of zero<->nonzero sample transitions at
held draw identities (2026-09-18: three zero->nonzero at identical
captured state), would show a pop as an identity ABSENT from frame N's
census that returns with passed samples in frame N+1 after the
immediate un-cull. At the draw level that is also what a legitimate
reveal looks like, which is why the zero-violation proof has to come
from shadow mode, where the would-reject draw still runs inside its
query. Live mode adds one check: the draws removed per frame equal the
ledger-predicted draws of the culled records.

ruled out: the ship's own cockpit as the occluder that carries the
parked prize, because with ideal cockpit geometry the coverage buffer
culls <= 6% of the unseen records and <= 1.9% of the pool draws at any
R, even at 512x512 (the dashboard's top edge is where the settlement
sits; the canopy struts are thinner than a tile).
ruled out: nesting inside the record's own shell as what hides the
parked view, because the occluding surface sits a median 73 m in front
of the record it hides and within 2 m of it for 0.1-0.2%.
ruled out: half draw multiplicity for unseen records as the pricing
worst case, because unseen records carry 0.87-0.89 of the seen
records' draws per record in all three runs.
ruled out: "no join" as a B' kill, because each eye draw names its pool
records through its instance range (98.5% of the eye pass).
ruled out: §1's "removing 3.5k draws recovered < 0.15 ms" (the
2026-09-21 A/B/C cost baseline) as evidence against a
draw-proportional caller cost, because its mined windows held no
settlement frames: the gfx logs' draw-hook CPU line reads 0.023 ms per
sampled frame through them (passes A and B) against 3.84-3.98 ms parked
at ~18k draws; the heavy scene was drawn only in the seconds before
each census.
ruled out: keying as the reason the depth files' constants block is
unusable (§2.5; the engine arc's caveat 4), because
src/d3d11/eye_depth_capture.h:191-192 and 258-271 copy cb1 BYTES
1024-2367 (float4 registers 64-147) while the view-projection is
registers 270-273 (bytes 4320-4383) and the eye origin register 275:
no key draw can supply them in that window. The fix is the offset, not
the key; not made here (the snapshot's cb1 served instead). FIXED in
close-out 2: file version 2 copies registers [256, 336) (bytes 4096-
5375, clamped to the buffer, none if it stops short of register 275)
and names the block's first cb1 float in the header;
tools/eye_depth_dump.py reads both versions and yields a camera only
from version 2.

**Next (offline first, then one armed capture at the parked pose, no
reject).** (1) Decompile FUN_144308B30 and settle which per-view test
admits the pool eye draws; the capture then confirms it by logging, for
the armed frame, each engine record's three per-view results beside
its t33 slots (the missing record -> t33 link) and matching them to the
ledger's per-eye records. (2) In the same frame, dump the view array
at the first gate call (count; each entry's +0x30 planes and +0x44
count, +0x540, +0x550/+0x560, +0x570/+0x578/+0x688) to name the eye
entries, prove same-frame against that frame's cb1[270..273] and find
a stored view-projection. (3) Mesh identity and geometry for the ~300
occluder records (the snapshot's vertex cap lifted to first use per
mesh, index buffers, PS blend state), so the solid-part inventory can
be built and rasterised offline against the three depth captures.
Gate before Phase C: the site proven, and the inventory reproducing
>= ~45% of the ideal removal at R = 1 m (>= 1.5 ms), re-priced after the
per-draw cut; a second cockpit pose (the approach, ~1 km) before the
flight gates.

**Close-out 1 (2026-09-22): which per-view test admits the pool draws,
from decomp_4308B30.txt** (analysis\decomp, headless Ghidra, read-only
project; the callees FUN_142817260, FUN_142842E90, FUN_14288AC40 and
FUN_14288A1E0 decompiled beside it). rec+0x208 is written once per
record per frame by the traversal from local_290
(decomp_4312040.txt:325/338 -> 346), and local_290 never sees a
frustum. It starts as the collection's ACTIVE MASK: FUN_144320340
passes FUN_144331300's output+0x20 as the traversal's param_2
(decomp_4320340.txt:44, 55; 4312040:136), and that mask is itself a
per-view distance/LOD result with no planes (decomp_4331300.txt:129-190,
view+0x540 camera, +0x550/+0x560 scale and bias, +0x570 view bits). The
traversal drops views whose parent LOD nibble exceeds the node's LOD
count (4312040:137-149), then views failing FUN_144308B30 (4312040:165).
FUN_144308B30 is a per-view DISTANCE/SCREEN-SIZE test, 17 lines, no
callees, no planes: per view bit it finds the view through the bit
table ctx+0x1A840, measures the record's centre (rec+0x240) against the
view's camera (view+0x540, read as ctx+0x580+i*0x6A0), scales by view
+0x550/+0x560 and the context's ctx+0x30, and either writes the view's
LOD index into the nibble table (decomp_4308B30.txt:94-98; param_1[3] =
local_230 -> rec+0x210) or clears the bit through param_1[4] =
&local_290 (4308B30:101). FUN_14430EFE0's frustum verdict edits only
local_288 (4312040:206-321), which feeds the type-2 path FUN_142817260
-> FUN_144312E00 (4312040:326-337); rec+0x208, the nibble table and the
children's incoming mask (4312040:353) never see it. Level 3 dispatches
a record whose rec+0x208 holds any view within the node's LOD count
(4320340:64-86) to FUN_1442B4420, handing it the collection's active
mask rather than rec+0x208 (4320340:95), and the builder runs its OWN
per-view admission: view+0x570 against that mask, FUN_1404F4E10 on the
pose's transformed bounds, then (under DAT_145ea3399 and view+0x68D
bit 0) the visibility callback FUN_14288AC40 (decomp_42B4420.txt:250-
275), ORing each surviving view into local_4b8, the mask its items are
built from. **So a reject at FUN_14430EFE0 removes at most the type-2
singleton items** (FUN_144312E00, <= 3 items a call, 92 calls a frame
in flight 073348) **and not** rec+0x208, the Level-3 dispatch, the
builder's items (its own frustum loop re-admits the view), the
children's traversal, or job-0's pose writes (FUN_14433DB20 runs at
4312040:153, before the gate loop). The per-eye record sets of the pool
draws (3a: 358 and 152 single-eye records, each outside the other eye's
viewport) need a per-view frustum; of the three per-view tests only the
builder's applies one to non-type-2 records. The site for removing pool
draws is FUN_1442B4420's view loop (42B4420:250-275: clearing a view
there before local_4b8 is ORed), conditional on the one link no
decompile has shown, that the builder's items become the instanced pool
draws: the 2026-09-21 trace found no route from either item list to the
D3D draws, and Phase A's caller-thread "draw-item chain" (0x4C81CA0 <
0x4C8227C) sitting beside the bucket drain's command enqueue
FUN_144C7EF70 is suggestive, not proof. The gate probe (close-out 3)
records the builder's per-view verdicts beside the t33 slots so one
armed frame proves or refutes it. The camera answer of 3a stands: all
three per-view tests read the same view array (FUN_144308B30 and
FUN_144331300 its +0x540/+0x550/+0x560, the builder and FUN_14430EFE0
its +0x30 planes).

ruled out: a reject at FUN_14430EFE0 as the pool-draw cull, because its
verdict (local_288) reaches only the type-2 item path, while rec+0x208
and the draw-item builder's input come from the distance/LOD tests
(4312040:137-165, 346; 4320340:95) and the builder re-tests the frustum
itself (42B4420:250-275).

**Close-out 3 (2026-09-22): the gate probe, built and gated, NOT FLOWN.**
`advanced.cull_gate_capture = 1` (default 0) rides the eye-run hotkey
like eye_depth_capture; pair the two. For the run's first three complete
ledger frames it observes, through relays already installed (no new
patch): FUN_14430EFE0 -- which IS the kinematic "evaluator" target
(kEvalRva) -- after each forward, as (engine record, view index, passed,
LOD); and the draw-item builder's bracket before each forward: the
record (param_4 - 0x210), its +0x170/+0x17C pose, +0x240/+0x270/+0x280
sphere, +0x208 view mask and +0x210 nibbles, and the builder's own
per-view admission recomputed with the engine's FUN_1404F4E10 on the
builder's inputs (prologue-checked; views where the FUN_14288AC40
callback would also apply are flagged, never called), plus the pose
context's entries and their 32-byte sub-items (the parts' local
transforms, which join to the t33 records). Once per (context, frame):
the view array, raw 0x6A0 bytes and plane array per view. Files:
edvr_logs\pool\gate_<stamp>.bin ('EDVRGATE' v1) and
drawstate_<stamp>.eyemesh.bin version 9 (EDVRDRW1 extended: every pool
draw of the first frame mapped by ledger ordinal to one copy per
distinct mesh -- vertex and index windows, input layout, topology -- and
per distinct blend/depth-stencil/raster state). Log lines: "cull gate
probe: armed ..." (hook status, builder hook, plane-test prologue), then
at the ledger write either the counts line or "NOTHING captured" with
the reason. tools/cull_gate_probe.py reads a run: seen/unseen per t33
record, the eye views by plane normals and the engine-to-pool offset,
the engine record behind each t33 record, per-eye verdicts of the mask,
the gate and the builder against the ledger's per-eye draws (the tally
that names the admitting test), and the occluder set's triangles and
state. Proven offline: on 055252 it reproduces 88.9% unseen; with a
synthetic gate file over 055252's real files it names both eyes (|cos|
1.00000), recovers a planted offset to 0.000 m, joins 4,360 t33
records and tallies the planted builder verdicts 943/0. Build gates:
the cull_gate_probe_test rig (12 checks on synthetic engine memory,
including a wild pointer and a pose mismatch), the reader's self-test
and its fixture check, the snapshot rig's version-9 geometry.

**Next: one parked capture, both keys on, no reject.** Read with
`python tools\cull_gate_probe.py <pool> <stamp>`: the builder tally
must agree with the ledger's per-eye draws where the gate does not; if
the builder also disagrees, the per-eye split is decided after the
items and the site moves again.
