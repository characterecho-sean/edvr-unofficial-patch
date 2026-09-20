# Kinematic-sourced motion-vector injection: a design

## Status

- **State:** DECIDED DIRECTION, 2026-09-19 (Sean): the final design for
  DLSS motion is engine-level injection from KinematicRig truth, not
  draw-call interpretation in the render pipeline. Draw-call identity
  and motion estimation are ruled out as a class (see below). The
  DLSS-path MV replacement (surface 2) is the end goal; EDVR's own
  temporal pass (surface 1) is the diagnostic stepping stone that proves
  the injected MVs are right before they touch DLSS history.
- **Open:** phase 0 is PARTLY RESOLVED offline after Sean's 2026-09-19
  21:23 review (settlement doc, same-time entry): the 20:52 flight's "hash
  stable across 1.4M calls" was never established (transition cap 8192 with
  1.44M overflowed, no hash-only counter), and +0x268 is a render-config
  hash that never reads the transform (FUN_14433C750) — retracted as a
  stasis signal. The 21:58 offline trace resolved collection+0x18 as a
  backing-owner pointer (ctor FUN_14430A060 param_3, single writer) and
  mapped the transform chain (FUN_144331300 LOD evaluator ->
  FUN_14433DB20 world updater; epoch pair collection+0x90 vs record+0x1B8
  as change-signal candidate). Flight 043344 (settlement doc 05:05
  entry) then REFUTED owner == rig — collection+0x18 is one shared
  global owner for every collection, not a per-rig object — and killed
  the epoch pair as captured (record+0x1B8 a constant small enum, never
  changing; the eval-hook descriptor +0x38 is a pointer, not the job
  descriptor's epoch field). Flight 054002 (settlement doc 05:45
  entry) then CLOSED per-rig reachability: rig -> *(rig+0x348)
  collection is proven (58/58 job-0 collections joined a rig; one
  shared global owner on both sides), with ~1,572 stable rigs, one
  collection each, one FUN_14431AFE0 dispatch per rig per frame. The
  engine-sourced motion path is now walkable: rig -> collection ->
  records (+0x280, stride 0x2F0) -> record+0x130..0x16C transforms.
  Still open: per-record identity (~5 records share one node
  pointer), pixel ownership, and the job-cost remeasure with the
  detailed observer disabled (job-3 = PrePhysicsAdvanceJob was never
  hooked — CodeHook refused a thunk/foreign-hook leading instruction;
  CurveJob hooked but never invoked). No per-record identity,
  pixel-ownership, or stasis claim survives without that work.
- **Ruled out (inherited, do not re-propose):** draw-shape memo identity
  (~96% misnaming); pool-slot identity (repacks); 3x3 SAD camera-vs-body
  match (self-confirming); estimating hidden-bone spin from the pool.
- **Capture landed (2026-09-20, settlement doc 08:22 entry):** the
  frame-aligned pose/identity capture is in KinematicEvalProbe
  (v0.17.0-103-g267d0430-dirty, installed to frontier) -- per-record
  per-frame translation+quat samples, dup-in-frame, gap-resume and
  node-change identity events (the slot-reuse probe for the shared-node
  records), frame stats, bounded mover sample log. Not yet flown.
  Update 09:35: re-clocked per-present (flight 083323 refuted the mesh
  clock) with a clock_samples mesh-staleness discriminator,
  v0.17.0-107-g5fe9c004-dirty.

## Premise

EDVR's temporal pass already composes its motion-vector field from
per-class motion sources (screen_motion, weapon_motion, celestial_motion,
mesh_motion, object_probe). Each source computes exact motion for one
object family. This design adds a **kinematic source** fed by engine
truth instead of GPU-side estimation, writing into the same MV field the
pass already hands to NGX.

Engine-level injection replaces the identity and motion problems, not the
coverage problem: which pixels a record owns still comes from its world
bounds projected and depth-gated. Do not let "engine level" read as "no
screen-space work".

## What the engine provides (from the settlement-flicker arc)

- Exact per-node world transforms every frame: the traversal
  FUN_144312040 (RVA 0x4312040) recomposes parent x child 3x4 matrices
  per node, recursing children at record+0x2A8/+0x2B0. Reaches the
  hidden skinning bone that defeated pool-based estimation.
- Current-frame record WORLD POSITION: 3 floats at record+0x170,
  flight-proven 2026-09-20 (settlement doc 06:45 entry): a moving
  drone's ~11 sub-mesh records tracked 2.59 m there while the 4x4 at
  record+0x130..0x16C stayed bit-static for every record — that
  matrix is a static local/default, NOT the per-frame transform.
  Object motion = per-frame delta of record+0x170. View products at
  record+0xF0..0x12C; world bounds at record+0xB0..0xEC.
- Current-frame record ORIENTATION: packed quaternion at record+0x17C
  (4x uint16 lanes, component = (lane - 32768)/32767, degenerate ->
  (0,0,0,65535) identity), decoded offline and flight-verified on the
  064047 data (drone lanes tracked its yaw; statics bit-constant).
  Sign canonicalization (q == -q) required before differencing.
- NOT provided, after the 2026-09-19 review (settlement doc 21:23
  entry): a proven stasis/change signal. record+0x268 is a
  render-config hash that never reads the transform (FUN_14433C750);
  render-record+0x688 is the engine's intra-frame evaluation sequencer;
  record+0x2C0 is a shared render-graph view object, not a per-record
  predicate. The transform/dirty-state dependency chain is an open
  offline item.
- Identity is NOT solved by node pointer alone: 2633 records share 510
  node pointers (~5 records per node), so per-record history needs a
  finer key; and projected bounds + depth gating do not uniquely own a
  mesh's pixels. Both must be resolved before any MV overwrite.

## Where injection makes sense

| Class | Signal | Injected MV | Expected win |
|---|---|---|---|
| Proven-static settlement geometry | record+0x170 delta == 0 across a window WITH a mover control present (flight-proven 064047) | exact zero object motion | kills settlement shimmer/ghosting at the source |
| Rigid movers (stations, ships, drones) | per-frame delta of record+0x170 (flight-proven: the drone, 11 sub-mesh records, 2.59 m) | exact rigid translation MV | closes tier-2's up-to-8-frame staleness; rotation still open (v1 limit) |
| Skinned/particle/smoke | none reliable | inject nothing | avoids the smoke-voids regression class |

Zero OBJECT motion always means the camera/head term stays in the final
vector — the injection composes object motion into the existing camera
motion, never replaces the whole vector.

## Injection surface (singular, after the 2026-09-19 review)

There is only one: **EDVR's own temporal pass**. NGX already consumes
EDVR's own MV texture (e.dlMv at the dlaaEvaluate/fsr3Evaluate hand-off
in temporal_pass.cpp) — the "game's DLSS path" surface was imaginary;
the diagnostic MV view and DLSS consume the same composition. A
kinematic_motion module mirroring mesh_motion's interface: per frame,
walk hooked collections, compute deltas, write object-motion deltas over
each record's verified pixel ownership, depth-gated (tier-1 lesson: a
mover's interior still ghosts without depth consistency). Proven right
with the existing temporal_aa_debug MV view before it ships.

## Phasing

- **Phase 0 (RESOLVED by flights, 2026-09-20):** reachability CLOSED
  (rig -> *(rig+0x348) collection, 58/58 join; ~1,572 stable rigs) and
  the motion field PROVEN (record+0x170 per-frame world position; the
  +0x130 4x4 ruled static; the epoch pair dead as captured) — see the
  settlement doc 05:05/05:45/06:45 entries and the Status block above.
  Remaining phase-0 debts: per-record identity finer than node
  (sub-mesh records share positions, pairs seen at identical starts),
  pixel ownership, and the observer-off job-cost remeasure.
- **Phase 1:** static-zero object-motion injection, config key,
  depth-gated, settlements only, gated on a PROVEN stasis signal from
  phase 0. One hook (traversal or collection walk), one compute shader,
  one motion module. Diagnosable with the temporal_aa_debug MV view.
- **Phase 2:** rigid-delta injection for movers (per-frame).
- A wrong MV in DLSS history is worse than none; each phase ships only
  with a measured before/after.

## Costs and risks

- CPU: a few hundred record reads per frame (trivial). GPU: one bounded
  compute pass per eye.
- Wrong-MV artifacts: the biggest risk. Mitigation requires per-record
  identity finer than the node pointer, verified pixel ownership, depth
  gating, and an engine-proven change signal — none of which exists yet;
  that is what phase 0 is for.
- Identity drift on collection realloc: key on record identity resolved
  in phase 0, never on the record address (the collection vector can
  realloc).
- Environment dependency to state at flight time: VR runtime, headset,
  per-eye render size, DLSS version, and the fixed record-table sizes.

## 2026-09-20 — Phase 1 spec: the diagnostic kinematic motion source

**Inputs, flight-proven — with bounded claims.** The live record set
comes from the eval hook (FUN_14430EFE0), which already sees every
evaluated record every frame via descriptor+0x10 — per-frame
re-resolution makes collection reallocs a non-issue for the live set.
The motion signals are record+0x170 (world translation, 064047) and
the record+0x17C packed quaternion (decoded 2026-09-20, below).
Stasis claims are BOUNDED (Sean's 2026-09-20 review): zero
translation delta across a window with a mover control proves the
instrument detects translation and that those records did not
translate in that window. It does NOT prove they cannot rotate,
deform, or start moving later. A static label is per-frame evidence,
re-evaluated every frame; it never becomes a permanent property.
Rig attribution is available from the riglink rows but not needed
for the MV itself.

**Module shape.** kinematic_motion, mirroring mesh_motion's interface
(the per-class source pattern: compute per frame, write into the
pass's MV field ahead of the e.dlMv hand-off). Three parts:

1. Tracker (CPU): piggybacks the eval hook's per-frame record stream.
   Per record it keeps a short pose history ALIGNED TO RENDERED
   FRAMES (translation +0x170 plus the +0x17C quaternion) — the
   probe's first/latest snapshots are evidence-grade, not
   tracker-grade, and are not reused here. Identity is re-established
   every frame from the live stream: a record pointer not re-seen
   this frame writes nothing this frame; a reallocated record is
   simply a new identity with no history and therefore no injection.
   Re-reading a pointer never stands in for sameness. Bounded (~4k
   records).
2. Ownership: each tracked record's world bounds (record+0xB0..0xEC)
   projected to screen, depth-gated against the depth buffer (the
   tier-1 lesson: a mover's interior still ghosts without depth
   consistency). Ownership is a SHIP PREREQUISITE: bounds+depth is
   not unique-mesh, and phase 1 ships only after the diagnostic view
   shows the mis-own rate at occlusion edges is negligible.
3. Compose: MV = existing camera/head motion + projected object delta
   for owned pixels. Records proven translation- AND rotation-static
   this frame write zero object delta — the camera term stays,
   shimmer dies at the source. Movers write their measured delta.
   Unknown identity, ambiguous coverage, tracker overflow, or any
   doubt PRESERVES THE EXISTING MOTION PATH for those pixels — the
   source never defaults to static-zero and never guesses.

**Config.** One functionality-named key (AGENTS.md: what the user
gets, never the mechanism): proposal `fix.engine_motion on|off|auto`,
default off while diagnostic, auto = settlements/landed scenes only
once phase 1 proves out.

**Verification before any DLSS exposure.** temporal_aa_debug MV view
on the drone scene: the drone's pixels must carry its measured
per-frame delta as screen-space MVs while settlement geometry carries
camera-only vectors. Quantitative check: the tracker logs per-frame
+0x170 deltas for the top group; the MV written for their pixels must
match within projection error. Shimmer gone in the diagnostic view is
the ship gate for phase 1; DLSS sees nothing until then.

**Rotation (decoded 2026-09-20, settlement doc 07:05 entry).** The
updater (FUN_14433DB20) writes an 8-byte orientation value at
record+0x17C immediately after position: four uint16 lanes, component
= (lane - 32768)/32767, degenerate norm -> (0,0,0,65535) = identity.
Flight-verified on 064047 data: the drone group's lanes tracked its
yaw while every static record's lanes stayed bit-constant. Remaining
rotation work: sign canonicalization (q == -q) before differencing,
and frame-aligned quat history (capture extends to the full 8 bytes
from build v0.17.0-96).

**v1 limits (stated, not hidden).** Only eval-population records are
tracked (the drone is a member; skinned/smoke classes are not and get
nothing). Pixel ownership is bounds+depth, not unique-mesh; occlusion
edges can mis-own — the depth gate is the mitigation, not a cure, and
the mis-own rate gates shipping. Per-frame static labels cover
translation and rotation only; deformation (vertex-level) is out of
scope and stays on the existing path.

**Failure modes, each logged distinctly:** tracker overflow (preserve
existing path), zero records seen in a frame (hook stood down —
reads as stand-down, never as pass), bounds read faults, stale-record
drops. End-to-end trace before flying, per build discipline.

**Not in scope:** no DLSS-path change (surface 2 is the same e.dlMv
hand-off), no skinned/particle injection, no sharpening or other
compensation (root causes only).
