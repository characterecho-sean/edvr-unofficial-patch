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
- **Prize:** the unseen FRACTION is measured — 90.7% (r=1 m) to 96.2%
  (r=0.3 m) of the 12,287 submitted records per frame are unseen in both
  eyes at one parked cockpit view (depth-join, run 055252; engine arc
  2026-09-22 entry). The ms figure (~4.8–5.1 of the ~5.3 ms view-dependent
  cost) is an ESTIMATE by proportionality: the job time it rests on is
  summed across worker threads and its wall-clock share is unproven (§2.2).
- **Open:** reviewer verdict; a VALID Phase A (§4) — the 2026-09-22 flight
  was invalid for the gate and its KILL verdict withdrawn the same day
  (engine arc, withdrawal entry: the WPR ring dropped the cockpit leg; the
  analyzer measures only the post-present slice); whether the job pipeline
  sits on the caller thread's critical path; the camera the engine's
  frustum gate reads and the readers of its active mask (§3.3); which
  surfaces occlude the measured unseen set; the record → mesh join.
- **Scope (Sean, 2026-09-22):** cockpit only — true stereoscopic settlement
  rendering from the ship. On foot is out of scope for now; the corpus and
  the profile legs are cockpit poses. Also to instrument: CPU frame time
  rises noticeably once the ship is within ~1 km of the settlement.
- **Ruled out (do not re-propose; evidence in §5):** draw-dedup culling;
  static/PVS culling; skip-based work gating (the change-gate abort); GPU
  occlusion queries as the runtime mechanism (they remain the offline
  oracle, §3.4); same-frame depth readback; boundary-side draw-call motion
  estimation (pipeline doc, 2026-09-19 decision); whole-structure boxes as
  occluders; single-occluder rect coverage as the test.
- **Next:** the analyzer and capture fixes (offline, provable against the
  old ETL), then Phase A — one cockpit flight (§4), then Phase B offline.

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
