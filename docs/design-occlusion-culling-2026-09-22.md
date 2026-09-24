# Design: dynamic analytic occlusion culling for settlement CPU

**For review.** Author: Kimi Code session with Sean, 2026-09-22. Revised
2026-09-22 after an in-repo review (its ledger is kept out of tree under
`reviews\`); the sections it corrected are marked *(revised)*. Status:
DESIGN ONLY — nothing implemented. The reader is assumed to have zero
context; every load-bearing claim cites its evidence.

## Status

- **VERDICT (2026-09-22 night, §12, run 165433, offline): B' FAILS at
  the part site.** The site is PROVEN: FUN_1442B3FC0's per-(part, eye)
  verdicts admit exactly the builder parts' pool draws (0 rejected and
  drawn, 0 admitted and undrawn, 20,728 admitted and drawn, 0 untested;
  reach 85.2% of the pool draws). The occluders are REFUTED: the
  settlement's occluding surface is open panels and shells (89.6%;
  closed meshes own 3.5%), and §3.2 built from qualified closed solids
  -- raw triangles or eroded inner boxes -- removes 0 pool draws at
  256x128 and 256x256: 0% of the ideal (bar ~45%), 0 ms (bar 1.5 ms at
  the 6.3 ms post-cut share). 0 false rejects (slot and draw level)
  against the exact re-draw truth.
- **Ceiling at the site (§12):** ideal occluders on the engine's own part
  spheres remove 19.5 / 25.9% of the pool draws (256x128 / 256x256):
  1.66 / 2.20 ms at the 8.5 ms pre-cut share, 1.23 / 1.63 ms post-cut.
  Only raw opaque open-panel triangles plus the terrain reach it
  (1.57-1.63 ms at 256x256), at 0.5-2.3 M triangles an eye against room
  for ~30-60 k in 0.3 ms.
- **Prize (exact re-draw truth, §12):** 74.1% of the pool eye draws hold
  only slots unseen in both eyes (6.30 / 4.67 ms pre / post-cut); the
  builder parts' share is 63.0% (5.35 / 3.97 ms).
- **State:** B' measured to its verdict offline (§9 on 055252/082019/
  094038, §10 on 152632, §12 on 165433). Nothing of §3 is built; the
  only code is the instrument (advanced.cull_gate_capture, default off)
  and its reader, tools/cull_gate_probe.py.
- **Open:** a sound, cheap representation of one-sided opaque panels
  (quads on their faces, cull mode kept) plus a terrain occluder, whose
  geometry no capture holds (version 9 carries pool draws only); 14.8%
  of the pool draws hold a slot no builder part claims, out of the part
  site's reach (the 197 unclaimed one-eye slots, all VS
  4435F2E50020E7F3, belong to no captured record).
- **Next:** Sean's call, no flight: close B' (the site's own post-cut
  ceiling is 1.63 ms, 9% over the bar), or measure the panel-quad
  inventory offline on 165433 against >= 92% of the 256x256 ideal.
- **Scope (Sean, 2026-09-22):** cockpit only -- true stereoscopic
  settlement rendering from the ship; on foot is out of scope for now.
  Also to instrument: CPU frame time rises noticeably once the ship is
  within ~1 km of the settlement.
- **Ruled out (do not re-propose; evidence in §5, §9-§12):** draw-dedup
  culling; static/PVS culling; skip-based work gating (the change-gate
  abort); GPU occlusion queries as the runtime mechanism (the offline
  oracle, §3.4); same-frame depth readback; boundary-side draw-call
  motion estimation; whole-structure boxes as occluders; single-occluder
  rect coverage; the cockpit as the parked prize's occluder; nesting as
  what hides the parked view; the "<0.15 ms for 3.5k draws" baseline; a
  reject at FUN_14430EFE0 as the pool-draw cull; a record-level reject in
  the builder's view loop; the collection mask as the admission; full
  occluder meshes within 0.3 ms. From §12: closed solids and their
  eroded inner boxes as the occluders; R = 1 m about the slot origin as
  the occludee, and the R = 1 m footprint as a zero-violation truth; the
  nearest-seen-origin association and the reader's range as occluder
  identity and selection.
- **Phase A (2026-09-22 evening, valid): KILL** of the job-pipeline
  prize: 5.24 ms thread-summed on seven workers, 0.81 ms on the caller
  thread's critical path (x recall = 0.78 ms against ~1.5 ms). The
  caller thread's wall is draw submission (3.94 ms of it innermost in
  EDVR's own d3d11.dll); §8 re-scoped the arc to it (engine arc Status).
- **2026-09-24:** `src/d3d11/original_draw_probe.cpp`, cited in §3, §7
  and §9 as the in-tree per-draw occlusion-query truth, was deleted with
  its rig; the citations stand as history.

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

## 10. Phase B' close-out (2026-09-22, run 152632): the builder is the site, its record unit fails the bar, the part unit is unmeasured

**Capture.** Eye run 152632 (session B, 15:26, parked cockpit at
Cranfield on the pad, Pimax OpenXR, EDVR a606157), both keys on:
gate_152632.bin (ledger frames 2..4: 86,760 gate calls, 2,037 builder
calls on 679 engine records, nothing dropped, 0 faults, 0 pose
mismatches), version 2 depth for frame 2 (3070x3032 an eye, camera in
the block), pool, instance stream and ledger for frames 2..20. Frame 2:
18,151 pool eye draws (A 9,075, B 9,076), 12,274 t33 records. Scripts
are throwaway, in the session scratch (phaseB2).

**1. The reader**, corrected here in two places: it read rec+0x208 at
each eye view's array INDEX, but masks carry the view's own +0x570 bits
(eye A = view 0 = bit 1, eye B = view 5 = bit 22; its first run scored
the mask 0 of 544); and its join kept one owner per t33 slot, so
shadow-only twins counted as builder disagreements (18 of 544). It now
reads the bits and tallies per slot with every claimant kept
(self-test extended). Frame 2: 10,852 of 12,274 drawn t33 records
(88.4%) unseen in both eyes at R = 1 m; 10,426 joined to a builder
record (484 slots claimed by two or more). Over the 679 builder records
no test ever splits the eyes: builder AB 614 / none 65, mask AB 632 /
none 47, gate AB 72 / none 607 (911 of its 2,892). Against the ledger's
per-eye draws (frames 3 and 4 have no depth file, which the reader
needs; their tallies, from the scratch, are identical):

| test | drawn (slot, eye) it rejects | admitted (record, eye), nothing drawn | admitted, drawn |
|---|---|---|---|
| builder view loop | 0 | 0 | 1,062 |
| collection mask rec+0x208 | 0 | 16 | 1,062 |
| gate FUN_14430EFE0 | 20,232 | 0 | 144 |

**2. The site.** The builder's view loop (42B4420:250-275) admits
exactly the pool draws of the records it sees. The mask over-admits 8
records an eye (a 212-part structure among them) whose slots the pool
holds and nothing draws, exactly those the builder's frustum rejects;
472 of the 544 drawn records have gate passed = 0 in both eyes. The 18
"disagreements" were twins: 22 records whose active mask holds only
non-eye bits (views 4, 7, 8) have drawn slots at the pivots of
eye-admitted records, and all 93 of those slots are also claimed by the
admitted twin. Two facts bound the site. Its unit is the WHOLE engine
record: one call a record a frame, every entry and sub-item under one
view mask (local_4b8), and records are coarse (the top one draws 1,287
parts, 238 of them seen).
And the per-eye split is finer than a record: 217 t33 parts are drawn
in one eye only (139 A, 78 B), each outside the other eye's viewport
(NDC > 1), 19 of them on records admitted in both eyes. The sub-item
loop explains it: per sub-item and admitted view the builder calls
FUN_1442B3FC0 (decompiled here, analysis\decomp\decomp_42B3FC0.txt:
distance and screen size against view +0x540/+0x550/+0x560, then
FUN_1404F4E10 on the part's own sphere, then the LOD pick) and ORs only
the passing views into the item's mask (42B4420:504-523, stored at 732)
-- a per-part, per-eye admission inside the same builder, whose verdicts
the probe does not record. Records the builder never sees (2,213 of the
gate's 2,892; 14.5% of the pool draws hold a part joined to no builder
record) take the gate's verdict and the type-2 path: of the 794 drawn
slots at such a record's origin, the gate passes the drawn eye for 784
in each eye.

**Eyes and same frame.** View 0 is eye A (bits 0x2), view 5 eye B (bits
0x400000), in all three dumps (|cos| 1.00000). Each has five planes: a
near plane 25.0 mm ahead and four sides through its own camera (+0x540)
to 0.001 mm. Against the frame-2 depth files' cb1[270..275]: cameras
0.087 mm apart, side planes within 0.08 mm and 0.0006 degrees; the dumps
stamped 3 and 4 sit 1.3 and 2.2 mm and up to 0.03 degrees away (the
head's drift), so the dump stamped 2 is the depth's frame. The pose
entries cannot discriminate frames here: no builder record's
+0x170/+0x17C changed over frames 2..4, their parts land on frame 2's
slots at a median 0.6 mm (consistent with the packed quaternion's
quantization) and join pools 2, 3 and 4 identically. Views 1-4 (bits
4-7, cameras 2.2 km and 340 km out), 6 (bit 39) and 7-9 (bits 25-27,
cascade-like) are the others.

**3. Occluders.** §9's association on eye A: of 10,784 unseen records
in its view, 97.6% sit behind a surface >= 30 m out (1.8% at 3-30 m,
0.6% the cockpit); 93.9% of those points lie within 5 m of a seen t33
origin, naming 344 occluder parts (top 10: 30.6% of the points, top 25:
47.6%). Through the join they are 54 engine records with 95.8% of the
points (31 unjoined parts, 4.2%); the top record alone 37.3%, the top
two 51.4%, the top eight 74.8%. Triangles are the ledger's index count
/ 3 an instance (every eye pool draw is indexed; lists assumed, the eye
pass's topology was not captured):

| set | parts | eye draws | triangles an eye |
|---|---|---|---|
| §9 association (>= 30 m) | 344 | 1,664 | 155,250 (a part: p50 132, p90 1,120, max 3,466) |
| of which the top record 0x1f8f5a1a750 | 78 | | 32,292 |
| the reader's range (seen, <= 120 m) | 523 | | 199,213 |

Solid versus not is NOT MEASURABLE from this capture, so neither are
the solid set's triangles (155,250 bound them from above) nor its share
of the points. The version 9 section that should carry every pool
draw's vertex and index windows and blend / depth-stencil / raster
state is empty (0 draws mapped, header frame 0), and the census logged
only offscreen state (4,207 lines, the depth-only shadow passes). Cause,
from the code and the log: armGateProbe armed it (armGeometry(2),
object_probe.cpp:669, 15:26:32.036); 60 ms later the object probe saw
the pool for the first time (15:26:32.096, "the instanced-mesh pool is
at VS t33"), and that path (object_probe.cpp:3535-3537) runs
releasePool() -> ledgerRelease() -> g_eyeMeshSnapshot.reset() ->
armGeometry(0) (1906, 700; eye_draw_snapshot.h:312), after which
captureGeometry returns at `!geoFrame` on every draw. The snapshot's own
draws survive (firstFrame re-arms at the next frame it sees); the
geometry arm is one-shot.

**4. Recall** (frame 2, R = 1 m, both-eyes rule: a draw goes only when
every record in it goes in both eyes; ms = share of the 18,151 pool
draws x 8.5; brackets R = 0.3..3 m). The ideal is §9's method recomputed
on this capture (the stored depth as the occluder, the farthest depth a
tile, the occludee's rect dilated one tile); 055252's rows reproduce
within a point (70.9 / 35.9 / 46.0%):

| unit (site) | depth truth | ideal 256x128 | ideal 256x256 |
|---|---|---|---|
| t33 part, every record (§9's unit) | 70.3%, 5.98 ms [86.8..49.5] | 35.0%, 2.97 [40.0..21.8] | 45.9%, 3.90 [55.4..32.4] |
| part, builder parts only (FUN_1442B3FC0) | 59.5%, 5.05 [74.8..41.2] | 29.8%, 2.54 [34.3..18.3] | 38.5%, 3.27 [46.6..26.8] |
| entry: one model in a record (no site) | 49.7%, 4.22 [70.1..33.0] | 22.7%, 1.93 | 31.3%, 2.66 |
| engine record, every part occluded (view loop) | 13.2%, 1.12 [33.4..11.4] | 5.9%, 0.50 | 10.7%, 0.91 |
| engine record, its own sphere (view loop) | - | 2.0%, 0.17 | 3.5%, 0.29 |
| real solid occluders | - | NOT MEASURED | NOT MEASURED |

The part rows use §9's R about each slot (the engine's own per-part
sphere, the model's +0x00 centre and +0x10 radius, is not captured).
The record's own sphere is the builder's frustum-test bound (pose +0x80
transformed, +0x90.x; rec+0x240/+0x280 differ from it by up to 42 and 47
m), and 43 of 553 records have a part origin up to 10 m outside it, so
it is no safe occludee either. False rejects against the depth truth: 0
in every ideal row, at every R and size.

**Verdict: B' FAILS at the proven site; at the per-part one it stays
CONDITIONAL and unmeasured.** A reject in the builder's view loop culls
whole engine records; there, ideal occluders remove 0.17-0.91 ms and
the depth truth itself 1.12 ms, against 1.5 ms. At FUN_1442B3FC0
(decompile plus the 217 one-eye parts; its verdicts unrecorded) ideal
occluders remove 2.54 / 3.27 ms of builder parts (2.97 / 3.90 if the
parts the builder never sees were culled too): real solid occluders
must reproduce >= 59% / 46% of the ideal (50% / 38%) at the 8.5 ms
price, or >= 95% / 74% (81% / 62%) if EDVR's per-draw cost is taken as
already cut (5.3 ms). The real ratio needs the geometry this capture
lost.

**5. Cost** (instruction level; Phase C measures). Occludee test per
(part, eye): project the centre (4 SIMD FMAs, a reciprocal), the rect
from the radius, clamp to tiles (~20 instructions), then the dilated
rect: a 1 m part at 100 m is ~12 px in radius against 12x24-px tiles at
256x128, ~4x3 tiles, two 8-wide compares; ~40 instructions, ~15-20
cycles. The part site sees ~13,500 builder sub-items a frame: ~27k
tests, ~0.12 ms thread-summed on the job workers (the record site's
1,358 tests: ~0.01 ms). Occluder rasterisation per eye: the named
occluders' full meshes are 155,250 triangles; a masked 8-wide
rasteriser spends ~30 instructions a triangle (transform amortised, edge
setup, 1-2 tiles each at this size), ~10-20 cycles: 0.4-0.8 ms an eye,
0.8-1.6 ms for both, 3-5x the 0.3 ms budget. §3.2's eroded boxes (~2k
triangles an eye) cost < 0.02 ms an eye, but only a qualified solid
inventory yields them, and this capture cannot qualify one.

ruled out: a record-level reject in the builder's view loop
(42B4420:250-275) as the B' site, because its unit is the whole engine
record: ideal occluders remove 5.9-10.7% of the pool draws there
(0.50-0.91 ms) by the parts form and 2.0-3.5% (0.17-0.29 ms) by the
record's own sphere, and the depth truth at that unit is 13.2% (1.12
ms; 0.97-2.84 ms over R), below 1.5 ms before any real occluder.
ruled out: the collection mask rec+0x208 as the pool-draw admission,
because it admits 8 records an eye whose slots are written and never
drawn, which the builder's frustum rejects.
ruled out: FUN_14430EFE0's verdict as the admission for builder records,
because 20,232 drawn (slot, eye) pairs belong only to records it
rejects.
ruled out: the first reader run's 18 builder disagreements as evidence
against the builder, because they are shadow-only twins (active mask
without the eye bits) at the pivots of eye-admitted records, whose 93
drawn slots the admitted twin also claims.
ruled out: declined draws as why the version 9 section is empty,
because its counters read 0 declined and 0 over the cap with header
frame 0: the arm was wiped 60 ms after it (above).
ruled out: the occluder set's full meshes as §3.2's rasterised
occluders within 0.3 ms, because they are 155,250 triangles an eye
(0.4-0.8 ms an eye by the estimate above).

**Next (no capture until the probe is fixed).** (1) The geometry arm:
re-arm it after ledgerRelease while g_gateRun, or arm it lazily at the
window's first frame (C++, object_probe.cpp). (2) Extend the gate probe
to the per-part site: at FUN_1442B3FC0 (its call at 0x1442B4B91 in the
sub-item loop) record per (sub-item, view) the verdict and LOD beside
the model's +0x00 centre and +0x10 radius, the part's own occludee. (3)
One parked capture, both keys, no reject: prove the part site against
the ledger's one-eye parts, qualify the solid inventory from the
geometry, and measure real/ideal against the thresholds above.

## 11. The part-site probe (2026-09-22): built and gated, NOT FLOWN

Both instruments of §10's Next (1) and (2); no cull, no reject.

**The geometry arm.** ledgerRelease, while an armed ledger runs a gate
run, now calls EyeDrawSnapshot::resetKeepingGeometry(g_frame + 1)
instead of reset(): the arm comes back when the release cost it
nothing (the armed frame not over, none of its draws mapped -- 152632's
case: g_pool was null until the sighting, so nothing could have been
mapped), else it stays disarmed and the loss is kept. It touches only
geo* state, so it cannot clear another capture's arm (deferring
ledgerRelease would have kept the instance stream and palettes of the
old pool). Log: at the release "eye mesh snapshot: ... re-armed" or
"... disarmed by pool release at ledger frame N"; at the write
"eye mesh snapshot: geometry armed for N draws of ledger frame F", "...
disarmed by pool release ...", or, if the arm was cleared by any other
path, "... disarmed by a reset outside a recorded pool release".

**FUN_1442B3FC0.** Its own patch (kinematic_eval_hook.cpp), installed
only by the gate probe's attach, relay gated on its own cell (open only
while attached), standing down alone. Signature: PE timestamp/size of
332841, the 21-byte prologue (48 89 5C 24 10 48 89 74 24 18 57 48 83 EC
50 48 8B 01 49 8B F8), and the 15 builder instructions the observer's
frame offsets were read from (0x1442B4429 .. the call at 0x1442B4B91).
After the forward it reads param_2 {LOD, passed} and param_1: [0] the
world centre, [1] the model's +0x10 copy, [2] the pose, [5] the context;
the part's identity (entry, sub-item) comes from the builder's frame
(rbp-0x58, rbp-0x70), believed only when the pointer block, the view
(local_4c8), pose and context agree and entry / sub-item index the
engine's own arrays. The builder bracket holds its row in a thread-local
across the forward, so each row names its builder row. 2^18 rows (three
frames at up to ~87k tests), drop counter; EDVRGATE v2 (reader
docstring). Offsets checked against the decompiles and the machine
code: the test never reads a model; param_1[0] is model+0x00 (float4)
composed through sub-item and pose (0x1442B48D5 .. 0x1442B4B15),
param_1[1] a copy of model+0x10 (0x1442B48D1/48D9) whose [0] is the
radius in all three uses (screen size 3FC0:68-71, FUN_1404F4E10's
interval, the LOD distance 3FC0:79); [1..3] never reach the verdict.
FUN_1404F4E10 is the frustum half (called at 0x1442B4066); not hooked.

**Reader.** tools/cull_gate_probe.py reads v1 and v2. On 152632 (v1)
its output is unchanged plus one line: 88.4% unseen at R = 1 m, builder
0 / 0 / 1,062. With verdicts planted over 152632's real files (a part
passes an eye iff the ledger drew a slot of it there) the v2 path joins
10,697 builder parts and tallies 0 / 0 / 20,789; five flipped verdicts
read as 5 rejected-and-drawn. Existing data, not planted: of the 217
one-eye slots only 19 are claimed by a builder part; 198 by none (the
records the builder never sees), so the part site can explain at most
19 of them.

## 12. Phase B' at the part site (2026-09-22, run 165433, offline): the site is proven, qualified solid occluders remove nothing

**Capture.** Eye run 165433 (16:54, parked on the pad at Cranfield
Nutrition Biosphere, Pimax OpenXR; the gfx log's version line names
b706df9), both keys on, no reject: gate_165433.bin (EDVRGATE v2, ledger
frames 2..4: 95,008 gate calls, 2,037 builder calls on 679 engine
records, 3 view dumps, 0 faults, 0 pose mismatches; 109,291
FUN_1442B3FC0 rows, all kept, 0 unverified, 0 foreign, 0 outside a
builder row), version 2 depth for frame 2, and version 9 geometry for
frame 2 (19,482 draws, 4,865 meshes, 32 states, 0 declined; §11's
re-arm fired 70 ms after the arm). Frame 2: 18,267 pool eye draws (A
9,133, B 9,134), 12,285 t33 slots, 88.0% unseen at R = 1 m. Scripts are
throwaway, in the session scratch (phaseB3), with a small C rasteriser.

**1. The reader** (tools/cull_gate_probe.py, unchanged) repeats §10 at
the record unit and proves the part unit:

| test | drawn (slot, eye) it rejects | admitted, nothing drawn | admitted, drawn |
|---|---|---|---|
| builder view loop (record, eye) | 0 | 0 | 1,062 |
| collection mask rec+0x208 (record, eye) | 0 | 16 | 1,062 |
| gate FUN_14430EFE0 (record, eye) | 20,232 | 0 | 144 |
| **FUN_1442B3FC0 (part, eye)** | **0** | **0** | **20,728** |

The part row also has 512 (part, eye) pairs rejected and undrawn and 0
untested. 12,013 parts carry rows in frame 2; 10,698 builder parts
claim a slot (10,427 of the 12,285 drawn slots are claimed, 473 by two
or more records); 10,647 were tested, the other 51 sit on records whose
view loop rejected the eye. Eyes: view 0 (bits 0x2) and view 5
(0x400000), |cos| 1.00000. The engine stores a plane as an outward n
with n.x = d: the four sides meet at the view's camera (+0x540) to
0.001 mm and the near plane is 25 mm ahead. Against the frame-2 depth
files: cameras 0.31 / 0.32 mm apart, planes within 0.035 degrees and
4.9 mm. The view record holds no clip matrix (only column 3, the
generic (0, 0, 0.025, 0), matches), so every projection below is the
depth files' cb1[270..273]; 0.035 degrees is under one texel of the
3070-wide eye.

**The one-eye slots:** 219 (A 136, B 83). 20 are claimed by a part the
part test rejects in the other eye. 2 are aliases: slots 12935 and
12936 are two of four slots at one pivot that three parts claim
(records 0x1e645fd7c30 and 0x1e645fd67a0), each part admitted in both
eyes; eye A draws the pivot through 12856, 12911 and 12936, eye B
through 12856, 12911 and 12935. The other 197 belong to no captured
record: one family draws them all (VS 4435F2E50020E7F3: 82 draws,
1,499 instances, 0.4% of the pool draws, no claimed slot among them),
each lies outside the other eye's viewport, and the nearest engine
record origin is 343 / 667 / 781 m away (p10/50/90). Neither the
builder's parts nor the gate's records hold them; whatever admits them
per eye, this probe does not see it. (At the origins of records the
builder never sees: 798 slots, all drawn in both eyes, the gate passing
the drawn eye for 788 -- §10's 784 of 794.) 2,704 pool draws (14.8%)
hold a slot no builder part claims.

**Site verdict: PROVEN at the part unit.** FUN_1442B3FC0's verdict per
(part, eye) admits exactly the builder parts' pool draws: no drawn slot
all of whose claimants it rejects, no admitted part with nothing drawn,
nothing untested, and it explains every one-eye slot it can reach. A
reject there removes one part in one eye; its reach is the 85.2% of the
pool draws all of whose slots a builder part claims.

**2. The geometry, and an exact truth.** The pool families pack
positions the way src/d3d11/fss_panel_vs.h's edvrDecodePos reads them
(PACKEDVERTEXDATAA; two encodings by pva.z bits 24..30) and place them
by the t33 head (boneBase +0, scale +4, unorm16 quaternion +8, position
+16): world = R(q) p scale + position, clip = M (world - cb1[275]).
Checked by re-drawing every static instance of the frame (4.70 / 4.47 M
triangles an eye) against the stored depth: of the texels that opaque,
depth-writing, non-discarding instances cover, 38.1 / 36.9% land on the
stored depth (0.02% + 2 mm) and 0.03% in front of it, the rest hidden
(on a 15k-instance subset, 46.8% land on it with the draw's cull mode
and 2.6% with it flipped). Of the stored non-sky texels (eyes A / B)
the pool's depth-writing instances reproduce 51.7 / 48.8%; the rest is
terrain, own hull and pad (non-pool, >= 3 m: 38.1 / 41.1%) and the
cockpit (10.1 / 10.0%). The re-draw is the exact
per-instance truth of §3.4 (a): an instance is seen if a texel of it
equals the stored depth (a non-depth-writing one: is not behind it);
skinned instances (200 an eye) and the 29 draws without geometry count
seen. It and §9's footprint (R = 1 m about the slot origin) disagree on
1,261 of the 12,285 slots: 861 the footprint calls seen are hidden, and
400 it calls unseen are visible.

The engine's part sphere is a sound occludee: over the 7,639 drawn
slots one part claims, every vertex of every mesh drawn there lies
within 1.026 radii of the v2 centre (p99 1.001; 8 slots exceed the
radius by more than 1 cm, the worst by 0.13 m). Centre to slot origin
p50 1.68 m; radius p50 1.63 m, p90 3.62 m.

**3. The occluders.** Of the 10,743 slots unseen in both eyes that lie
in eye A's view, 98.0% sit behind a surface >= 30 m out, and the
re-draw names that surface's owner exactly: an opaque OPEN pool mesh
89.6%, a closed one 3.5%, no pool instance (terrain, hull) 6.9%, a
discarding pixel shader 0.0%. §10's association (the nearest seen slot
origin within 5 m) names the owner's own slot for 13.7% of the points
the re-draw attributes. The settlement's walls are open by
construction: 187 of the 4,864 meshes are closed (every welded edge
shared by exactly two oppositely wound triangles; the 154 instanced in
eye A are all outward: for all 1.57 M of their instance triangles the
rasteriser's front face is the outside), and of the 15 meshes that own
half the occluding points 14 are open and keep their boundary edges
(within 8%) from a 0.1 mm weld to a 5 cm one: panels with 58-96% of
their area facing one way, and shells open along their edges.

Per instance, solid = static, opaque, depth-writing, a pixel shader
dumped and without discard (31 of the 40 pool PS dumped, 9 of them with
discard), AND a closed mesh; "opaque, open" = the same without
closedness. Eye A, the reader's range (slots seen at R = 1 m within 120
m: 531) and the association (359 slots):

| set | class | instances | distinct meshes (triangles) | instance triangles | states |
|---|---|---|---|---|---|
| reader range | solid (closed) | 67 | 32 (1,884) | 4,482 | 4 |
| | opaque, open | 1,371 | 493 (33,738) | 78,639 | 4 |
| | PS discard or undumped | 15 | 6 (5,041) | 5,077 | 5, 4 |
| | blended or no depth write | 763 | 254 (16,363) | 52,203 | 8, 12, 14 |
| | skinned | 16 | 16 (56,386) | 56,386 | 1, 2, 8 |
| association | solid (closed) | 38 | 15 (864) | 3,114 | 4 |
| | opaque, open | 1,022 | 409 (31,206) | 81,902 | 4 |
| | PS discard or undumped | 8 | 5 (6,910) | 6,922 | 5, 4 |
| | blended or no depth write | 620 | 252 (17,530) | 58,274 | 8, 12, 14 |
| every instance an eye draws | solid (closed) | 957 | 114 (6,128) | 53,842 | |
| | opaque (open or closed) | 30,527 | 3,109 (480,251) | 2,266,034 | |

States: 4 opaque, depth write, cull back; 5 the same, cull none; 1 and
2 as 4; 8 blended, no depth write; 12 opaque, no depth write; 14
blended, no depth write, cull front. The solid set's z-buffer
reproduces the stored depth at 3.5% of the >= 30 m occluding points,
its eroded boxes at 1.7%, the opaque set at 93.1%. Form (b), §3.2's
representation: per solid mesh the largest axis-aligned box inside it
(voxelised in the mesh frame, 40 cells an axis, parity rays, eroded to
the inside cells' centres): 81 of the 114 meshes (the rest thin or
zero-volume shells), box / mesh volume p50 0.28; 679 boxes an eye,
2,011 eye-facing faces, 4,022 triangles; no box texel in front of the
solid surface.

**4. Recall** (frame 2, 18,267 pool eye draws). Unit: FUN_1442B3FC0's
part with its own sphere. Both-eyes rule with the engine's verdicts: a
part goes when, in each eye, the part test rejected it or it is
occluded (or off-screen); a slot goes when every part claiming it goes
(a part the engine rejects in both eyes keeps no slot); a draw goes
when every slot of its instance range goes. Coverage buffer as §9/§10:
per eye a full-resolution z-buffer of the occluders (the stored depth
for the ideal), the farthest depth per tile, a tile with any uncovered
texel open; the occludee's rect x 1.05, dilated one tile. ms = share x
8.5 (pre-cut) / x 6.3 (post-cut):

| occluders (triangles an eye) | 256x128 | 256x256 | real / ideal |
|---|---|---|---|
| truth, exact re-draw (part unit) | 63.0%, 5.35 / 3.97 ms | - | |
| truth, footprint R = 1 m (part unit) | 58.9%, 5.00 / 3.71 [75.5..40.3 over R 0.3..3] | - | |
| ideal (the stored depth) | 19.5%, 1.66 / 1.23 | 25.9%, 2.20 / 1.63 | 1 |
| **(a) solid set, raw triangles (53.8 k)** | **0 draws** | **0 draws** (4 slots) | **0 / 0** |
| **(b) solid set, eroded inner boxes (4.0 k)** | **0** | **0** | **0 / 0** |
| bound: every opaque instance, open meshes too (2.27 M) | 14.6%, 1.24 / 0.92 | 20.7%, 1.76 / 1.30 | 0.75 / 0.80 |
| bound: the same + terrain (stored non-pool, >= 3 m) | 19.5%, 1.66 / 1.23 | 25.9%, 2.20 / 1.63 | 1.00 / 1.00 |
| bound: opaque instances the re-draw shows seen (519 k) + terrain | 18.4%, 1.57 / 1.16 | 24.9%, 2.12 / 1.57 | 0.94 / 0.96 |
| same, slots within 120 m (282 k) + terrain | 16.2%, 1.38 / 1.02 | 19.1%, 1.62 / 1.20 | 0.83 / 0.74 |
| opaque on the reader's range, + terrain | 4 draws | 30 draws (0.2%) | 0.00 / 0.01 |

The bridge to §10 on this capture (§10's unit, R = 1 m about each
claimed slot): ideal 29.2 / 37.8% (§10: 29.8 / 38.5); the engine's own
spheres, larger and off the pivot, cost a third of it. At 512x512
(context, not the brief) the ideal is 31.0%, 2.64 / 1.95 ms, the solid
set 2 draws. Admitting the five undumped pixel shaders the re-draw
shows hole-free (texels in front of the stored depth < 0.01%; chiefly
026709B54867F893 on family 4435F2E50020E7F3's closed ~2k-triangle
meshes) grows the solid set to 1,730 instances and 1.56 M triangles an
eye, and it still removes 0 draws. Where the ideal comes from: the
pool-owned texels of the stored depth alone remove 0.0 / 0.6%, the
non-pool ones alone 0; it is fusion, building fronts with the terrain
between and below them.
Without the terrain the opaque set keeps 75-80% of the ideal because
hidden instances stand in for it; with the terrain it matches the ideal
to the draw. The reader's range fails as a selection because it picks
occluders by the R = 1 m footprint: 41.8% of the texel weight the full
opaque set culls with sits on slots the re-draw shows seen and the
footprint calls unseen (a panel whose pivot hides behind its own face).
The terrain rows are bounds: the version 9 section carries only pool
draws, so the terrain's own geometry, and what it would cost, are not
in this capture.

**5. False rejects: 0** against the exact re-draw, at slot and draw
level, in every row above and at 512x512. Two classes of candidates,
neither a loss: (i) 3-5 culled parts (8 at 512x512) claim a seen slot
that two to four parts share at one pivot; the slot stays because a
co-claimant is not culled, and each culled part's own sphere is
occluded in both eyes, so the seen texels are the co-claimant's; (ii)
against the footprint, 1-2 removed slots (10230, 8425; 0-6 draws) that
it calls seen and the re-draw shows hidden in both eyes. The footprint
is no zero-violation truth at this unit: its own part-unit truth would
remove 1,058 draws holding a slot the re-draw shows seen, and §10's
occludee (R = 1 m about the slot) with ideal occluders removes 176 /
342 such draws on claimed slots (388 / 644 on every slot) at 256x128 /
256x256; the engine's sphere removes none.

**6. Cost at the part site** (instruction level, as §10). The test runs
only where the engine passed a part in an eye view: 20,728 (part, eye)
tests a frame (of 11,771 rows an eye view), ~40 instructions / 15-20
cycles each, ~0.1-0.12 ms thread-summed on the builder's thread. Form
(b) rasterises 4,022 triangles an eye: < 0.02 ms an eye, ~0.15 ms in
all, inside 0.3 ms -- for zero draws. What does recover draws costs
triangles: the seen opaque set is 519 k an eye (282 k within 120 m),
~2-3 ms an eye at 10-20 cycles a triangle; 0.3 ms less the tests
leaves room for ~30-60 k triangles for both eyes.

**Verdict: B' FAILS at the part site with a qualified inventory.** The
site is proven. The recall is not: real solid occluders, raw or as
§3.2's eroded boxes, reproduce 0% of the ideal at 256x128 and 256x256
(FAIL against >= ~45% at the 8.5 ms pre-cut share) and remove 0 ms
against the 1.5 ms bar at the 6.3 ms post-cut share (FAIL). The bar is
also steeper than §9's 45% at this unit: the ideal itself is 1.66 /
2.20 ms pre-cut and 1.23 / 1.63 ms post-cut, so a real set needs >= 90%
/ 68% of it pre-cut and >= 92% at 256x256 post-cut (at 256x128 not even
ideal occluders reach 1.5 ms). Only raw open-panel triangles plus the
terrain get there (1.57-1.63 ms at 256x256), at 0.5-2.3 M triangles an
eye, on a soundness argument §3.2 never made (a one-sided panel
occludes only from its front, so the draw's cull mode must ride with
every triangle) and with a terrain this capture does not carry.

ruled out: a closed-solid inventory (§3.1's qualification) as the
occluder for the parked prize, because closed meshes own 3.5% of the
occluding surface and their raw triangles remove 0 pool draws at
256x128 and 256x256 (2 at 512x512; still 0 with the hole-free undumped
shaders admitted, 1.56 M triangles an eye).
ruled out: §3.2's eroded inner boxes as the representation for this
content, because they cover 1.7% of the occluding points (a subset of
the closed solids') and remove 0 draws.
ruled out: R = 1 m about the slot origin as the occludee (§9/§10's
unit), because it does not bound the part (centre offset p50 1.7 m,
radius p50 1.6 m): with ideal occluders it removes 176-644 draws that
hold a slot the exact re-draw shows seen; the engine's part sphere
(every vertex within 1.026 radii) removes none.
ruled out: the R = 1 m footprint as the truth of a zero-violation gate
at the part unit, because 400 of the 12,285 slots it calls unseen are
visible in the exact re-draw (its part-unit truth would remove 1,058
draws holding one).
ruled out: §10's nearest-seen-origin association as the occluder's
identity, because it names the owning slot for 13.7% of the occluding
points the re-draw attributes.
ruled out: the reader's range (slots seen at R = 1 m within 120 m) as
the occluder selection, because the opaque set on it removes 0.0-0.2%
of draws: panels whose pivot hides behind their own face are
footprint-unseen yet carry 41.8% of the occluding texel weight.
ruled out: the part test as the admission of the 197 unclaimed one-eye
slots, because no builder part or engine record lies within 343 m of
90% of them (one family, VS 4435F2E50020E7F3).

**Next.** Sean's call; nothing here needs a flight. Either close B'
(the §3.2 test's own post-cut ceiling at the proven site is 1.63 ms at
256x256, 9% over the bar, and the qualified inventory recovers
nothing), or measure one more representation offline on 165433: a few
inner quads per open opaque panel on its planar faces (cull mode kept)
plus a terrain occluder -- whose geometry must first be captured, the
version 9 section holding only pool draws -- against the >= 92% of the
256x256 ideal it would need within ~30-60 k triangles for both eyes.
