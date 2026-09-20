# Kinematic-sourced motion-vector injection: a design

## Status

- **State:** DECIDED DIRECTION, 2026-09-19 (Sean): the final design for
  DLSS motion is engine-level injection from KinematicRig truth, not
  draw-call interpretation in the render pipeline. Draw-call identity
  and motion estimation are ruled out as a class (see below). The
  DLSS-path MV replacement (surface 2) is the end goal; EDVR's own
  temporal pass (surface 1) is the diagnostic stepping stone that proves
  the injected MVs are right before they touch DLSS history.
- **Open:** everything in Phasing; phase 0 shares the next-flight capture
  spec in settlement-flicker-2026-09-17.md (predicate vtable + flags).
  Probes built and installed to the frontier copy 2026-09-19 evening
  (commit 116a2e0); phase 1 stays unbuilt until that log is read.
- **Ruled out (inherited, do not re-propose):** draw-shape memo identity
  (~96% misnaming); pool-slot identity (repacks); 3x3 SAD camera-vs-body
  match (self-confirming); estimating hidden-bone spin from the pool.

## Premise

EDVR's temporal pass already composes its motion-vector field from
per-class motion sources (screen_motion, weapon_motion, celestial_motion,
mesh_motion, object_probe). Each source computes exact motion for one
object family. This design adds a **kinematic source** fed by engine
truth instead of GPU-side estimation, and optionally a corrected MV copy
at the DLSS hand-off.

Engine-level injection replaces the identity and motion problems, not the
coverage problem: which pixels a record owns still comes from its world
bounds projected and depth-gated. Do not let "engine level" read as "no
screen-space work".

## What the engine provides (from the settlement-flicker arc)

- Exact per-node world transforms every frame: the traversal
  FUN_144312040 (RVA 0x4312040) recomposes parent x child 3x4 matrices
  per node, recursing children at record+0x2A8/+0x2B0. Reaches the
  hidden skinning bone that defeated pool-based estimation.
- Engine-validated stasis/change per record (stride 0x2F0, base
  collection+0x280): content hash record+0x268, cached has-work bool
  record+0x234, change predicate record+0x2C0 (slots +0x70/+0x58),
  skip bit render-record+0x688 bit 0x1000.
- Current-frame record transform: 3x4 at record+0x130..0x16C, view
  products at record+0xF0..0x12C; world bounds at record+0xB0..0xEC.
- Stable identity: node pointer record+0x18 is the game object. Key on
  it, not on the record address (the collection vector can realloc).

## Where injection makes sense

| Class | Signal | Injected MV | Expected win |
|---|---|---|---|
| Proven-static settlement geometry | record+0x268 hash unchanged | exact zero object motion | kills settlement shimmer/ghosting at the source |
| Rigid movers (stations, ships) | per-frame delta of record+0x130..0x16C | exact rigid MV | closes tier-2's up-to-8-frame staleness; reaches hidden-bone spin |
| Skinned/particle/smoke | none reliable | inject nothing | avoids the smoke-voids regression class |

## Injection surfaces

1. **EDVR's own temporal pass** (easy, diagnostic stepping stone): a
   kinematic_motion module mirroring mesh_motion's interface. Per frame,
   walk hooked collections, compute deltas, write MVs into the pass's MV
   field over each record's projected bounds, depth-gated (tier-1 lesson:
   a mover's interior still ghosts without depth consistency). Proves the
   injected MVs are right with the existing temporal_aa_debug MV view
   before anything touches DLSS history.
2. **The game's DLSS path** (medium, the end goal): dlaa.cpp already
   holds the game's MV texture at the NGX hand-off (pInMotionVectors,
   MVLowRes, unjittered, scale 1.0). Copy it, overwrite regions belonging
   to known records, pass the copy. No game-internal hooking; one bounded
   compute pass.

## Phasing

- **Phase 0 (gate, shared flight):** record+0x268 hash stability census
  on known-static settlement records + predicate vtable capture (spec in
  settlement-flicker-2026-09-17.md). No build before this lands.
- **Phase 1:** static-zero injection in EDVR's own pass only, config
  key, depth-gated, settlements only. One hook (traversal or collection
  walk), one compute shader, one motion module. Diagnosable with the
  existing temporal_aa_debug MV view.
- **Phase 2:** rigid-delta injection for movers (per-frame).
- **Phase 3:** DLSS-path MV replacement, only if 1-2 show measured wins;
  a wrong MV in DLSS history is worse than none.

## Costs and risks

- CPU: a few hundred record reads per frame (trivial). GPU: one bounded
  compute pass per eye.
- Wrong-MV artifacts: mitigated by depth gating and engine-validated
  change signals, not heuristics.
- Identity drift on collection realloc: mitigated by node-pointer keys.
- Environment dependency to state at flight time: VR runtime, headset,
  per-eye render size, DLSS version, and the fixed record-table sizes.
