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
  v0.17.0-107-g5fe9c004-dirty. Update 10:38: capture v2 (arm-seed fix,
  32,768-sample cap, gap re-log backstop) flight-proven clean on 103339
  -- zero startup gap events, samples survive the full window, drone
  per-frame series complete (settlement doc 10:38 entry). Update 10:52:
  the Phase-1 implementation spec landed below (tracker + quarter-res
  ownership coverage + static-zero compose veto, fix.engine_motion
  default off) -- the build spec for the next branch, with its own
  test rig and flight-verification plan.
  Update 13:30: stage-A.5 -- the stage-B extents question is resolved
  OFFLINE (settlement doc same-time entry): the record bounds are a
  bounding SPHERE, centre float4 at +0x270 and radius at +0x280
  (writer FUN_14433C870 from model data +0x20/+0x2C, unioned over
  children by sphere-merge FUN_140A8C9A0). The planned wide
  +0x130..+0x250 capture is replaced by a targeted 32-byte sphere read
  in the dump line (svalid/s=, v0.17.0-123-gdae06c51-dirty, installed
  to frontier, verified). Flight 132856 then CONFIRMED the sphere end
  to end (settlement doc 13:45 entry): sane radii, and world centre
  +0x240 == R^T x local + T exactly on all statics. Stage B unblocked.
  Update 13:55: stage B spec'd below against the flight-proven sphere
  layout -- sphere projection replaces the 10:52 AABB-corners sketch,
  the stasis compare extends to pose+sphere (the LOD-rewrite case),
  world centre is computed (R^T x local + T), never read.
  Update 14:45: stage B LANDED (6b90d0b on codex/stage-b-ownership,
  frontier install d3d11 sha256 81d5d27f434b2458) -- landed entry at
  the foot; the 13:55 verification flight is next, not yet flown.
  Update 15:20: the 13-finding review of 6b90d0b keeps the mask
  DIAGNOSTIC-ONLY (fix.engine_motion_veto, default off, arms the veto);
  findings 1/4/5 fixed (5fb4f62, frontier d6d5ef39ceb2a0ca) -- review
  response entry at the foot. Flight protocol unchanged, veto dark.
  Update 15:40: flight 152934 (same-date entry) -- all three fixes hold
  in flight, tracker healthy at settlement scale (~2.6k published); the
  movers-view cyan question is OPEN: the eye burst missed the debug
  toggle window. Re-fly with the movers view HELD through the capture.
  Update 16:10: Sean's headset-only movers test (15:45) answered with a
  NEW symptom: everything outside the cockpit paints one rapidly-cycling
  colour -- near-certainly the stage-B cyan owning ~the whole scene and
  strobing. No log (headset-only, burst missed the toggle again).
  Hypotheses enumerated in the same-date 16:10 entry: H1 a straddling
  sphere paints the whole quarter-res texture (the player's own landed
  ship is the prime suspect), H2 the bind bit flaps per frame, H3
  interval-union mis-ownership at scale. Instrumented, not fixed blind:
  the eye burst now also dumps both eyes' coverage pair
  (KCNear/KCFar0-1.bin, EDVRTEX1 R32_UINT), the GPU sphere upload
  (KinSpheres.bin, EDVRKSP1) and per-frame kin_bound/kin_veto in
  decisions.json schema 2. Gates green; frontier d3d11
  ac6758093e45419d. Re-fly: movers view HELD through the burst.
  Update 16:20: dump 160734 (ship landing, movers HELD) settles the
  movers-view question -- and Sean's 16:08 correction reframes 15:45:
  that strobing was the MOTION view (mvUsed paint), not movers, so the
  cyan H1/H2/H3 framing never applied to it. From the dump: the
  coverage pair is 100% claimed with ONE uniform [0.05 m, 671 m]
  interval in both eyes (straddle paint-all fired; 10 straddlers by
  the cameraR rows, 92-94 by the now/prev rows); kin_bound solid
  16/16 (bind flapping refuted); T15 shows ground and settlement cyan,
  sky correctly rejected by the depth gate, and the LANDING SHIP cyan
  (Sean confirms in-headset) -- the mask owning a known mover through
  other spheres' intervals, review finding 2 made visible. Fix
  direction (design call, unbuilt): straddle -> paint-none. The
  motion-view whole-scene pulse is the MV field itself -- the
  pathology this arc is fixing, not a stage-B regression (veto dark);
  its capture is a burst held in the MOTION view.

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
yaw while every static record's lanes stayed bit-constant. Flight
103339 quantified a rotating-in-place class: 292 records translate
<0.1 m over a 51-frame window yet rotate 0.8-1.6 deg/present every
present -- flight evidence that static-zero must gate on BOTH
translation and rotation stasis, never translation alone. Remaining
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
## 2026-09-20 (10:52) -- Phase-1 implementation spec: kinematic_motion

Written after flight 103339 proved the capture instrument clean end to
end (zero startup gaps, samples survive, drone series complete,
rotating-in-place class quantified). This is the build spec for the
tracker itself. Anchors verified against the tree at 516e93d.

### Scope and ship gate

Phase 1 injects ZERO object motion for proven-static records only.
Movers keep the existing path (rigid-delta injection is phase 2).
Ship gate, unchanged: shimmer gone in the temporal_aa_debug motion
view on the drone scene, with the ownership diagnostic showing a
negligible mis-own rate at occlusion edges. DLSS sees nothing until
then -- the injection writes into the same e.dlMv composition the
diagnostic view already shows.

### Module shape

New files src\d3d11\kinematic_motion.h / .cpp, free functions
mirroring mesh_motion.h's pattern:

    void kinematicMotionConfigure(bool on);
    void kinematicMotionShutdown();
    void kinematicMotionNotePresentFrame(uint32_t presentFrame) noexcept;
    void kinematicMotionObserve(uintptr_t record) noexcept;   // eval-hook feed
    void kinematicMotionViews(ID3D11DeviceContext*, ID3D11Texture2D* scene,
                              ID3D11ShaderResourceView** out); // 1 slot: KC

Configure is called beside meshMotionConfigure
(temporal_pass.cpp:6865-6866). NotePresentFrame is called beside the
probe's (device_hook.cpp:1001, immediately after ++g_state->frameCounter
-- NOT beside vScreenFrameBoundary, which sits behind the
graphicsRuntimeDisabled early return). Views is called beside
meshMotionViews (temporal_pass.cpp:3715), ensures this eye's ownership
coverage for the current frame, and returns the SRV or nullptr
(feature off / stand-down -- nullptr is the stock path, byte-identical).

### Observation feed

kinematic_eval_hook.cpp's relay currently gates one observer (the
probe's armed state). Add a second observer atomic for the tracker:
when tracker-active, the relay also calls kinematicMotionObserve with
the record pointer (descriptor+0x10, the same pointer the probe reads).
Cost when off: one extra atomic load per call -- same standard the
hook header already states ("an unarmed hook is one atomic load plus
the trampoline call"). When on: ~33k observe calls/frame (3,008
records x ~11x fan-out, flight-measured), each O(1). Locking mirrors
the probe's: a short mutex in observe; job timings across two flights
show the probe's identical pattern costs nothing measurable.

### Tracker (CPU)

Bounded table, cap 4,096 records (= probe kRecordCap; flight max seen
3,086). One entry per live record pointer:

    struct TrackedRecord {
        uint64_t record = 0;      // key; never trusted across a gap
        uint64_t node = 0;        // reuse discriminator (record+node)
        uint32_t lastFrame = 0;   // last present-domain frame observed
        uint8_t  prevPose[20]{};  // 12 B translation (+0x170) + 8 B quat (+0x17C)
        float    bMin[3]{}, bMax[3]{};  // world bounds record+0xB0..0xEC
        uint32_t staticRun = 0;   // consecutive frames of bit-exact zero delta
        uint32_t flags = 0;       // hasPrev, seenThisFrame
    };

Rules, each flight-grounded:

- First-sample-wins dedup per frame (the ~11x fan-out; 094158/103339).
- Stasis: BIT-EXACT zero delta across all 20 pose bytes between the two
  latest samples of the same identity, sustained staticRun >= 3 present
  frames before the record is eligible. Any non-zero byte resets the
  run instantly -- a static label is per-frame evidence, never sticky.
  The quat lanes are part of the compare: 103339's 292 rotating-in-place
  records (0.8-1.6 deg/present, zero translation) must NEVER be labeled.
  Phase 1 compares raw bits (a q == -q sign flip just resets the run --
  conservative); canonicalization is a phase-2 requirement, as the
  design doc already states.
- Near-miss escape hatch: pose changed but translation moved < 1 mm --
  count them (nearMiss). If a flight shows real statics accumulating
  near-misses instead of bit-exact zeros, bit-exactness is refuted and
  the compare, not a threshold, gets rethought. No epsilon tuning.
- Identity: keyed on the record pointer, node as discriminator. A
  pointer absent ANY frame, then re-seen -- new identity, run restarts,
  no injection until re-proven. A pointer re-seen with a different
  node -- new identity, full stop. Re-reading a pointer never stands
  in for sameness (design doc). Post-gap pointer reuse is still
  flight-untested; these rules are the conservative posture for it.
- Reads via the probe's guardedRead idiom; read faults counted.
- Overflow: pointer beyond the cap is never tracked, never injected;
  recordOverflow counts; the existing path is preserved for those
  pixels. Never evict to make room.

### Ownership (GPU)

One coverage texture per eye, QUARTER resolution of the eye target,
R32G32_UINT: per texel the [near,far] reversed-Z depth span (24-bit
quantized) of the union of eligible static records projecting there;
a stated empty sentinel. One bounded compute pass per eye per frame,
one thread per eligible record (<= 4,096, no selection step): project
the bounds' 8 corners with THIS frame's per-eye view-projection,
atomically InterlockedMin/Max the depth span over the screen rect.

The matrix source is the #1 expected bug class: use the same
current-frame camera rows the compose pass itself uses for zSceneAt's
frame (chooseCameraRows' frame), never last frame's. The ownership
debug paint (below) catches a mismatch directly.

Quarter-res coverage + full-res depth gate is the precision split:
bounds are coarse by nature (the design doc: bounds+depth is not
unique-mesh), so coverage may be coarse; the per-pixel depth gate is
where ownership is actually decided.

### Compose integration

temporal_shader_source.h: registers t0..t18 are taken; add

    Texture2D<uint2> KC : register(t19);  // kinematic static ownership,
                                          // quarter-res; sentinel = unowned

Flag word at temporal_pass.cpp:4696 gains (kinSrv ? 256u : 0u) beside
the existing 32/64/128 bits. Bind KC at all three SRV-array sites
(5629 main compose; 6125 and 6409, which already pass nullptr for
staticOwner when a path lacks it -- same precedent: nullptr = off for
that path, no behavior change).

New helper kinematicStatic(p) beside meshPixel (485): load KC at
quarter-res, reject unowned, reject uiCovered(q) (UI pixels never take
an object-motion override), accept when zSceneAt(q) lies inside the
recorded [near,far] span with a stated relative margin (volumetric
bounds are looser than meshPixel's exact-depth 1e-6; the margin is a
named constant, and the mis-own diagnostic below is what sizes it --
no silent tuning).

Semantics at BOTH meshPixel call sites (687, 1065): consult
kinematicStatic FIRST; owned pixels take the camera/depth motion and
SKIP every object-motion candidate (mesh/screen/holo). Unowned pixels
are byte-identical to today -- this is "unknown preserves the existing
path, never guesses" in shader form. Precedence safety: a real mover
in front of a static wall fails the depth span (its zScene is nearer),
so only genuinely static-surface pixels are ever vetoed; a static
surface mesh_motion also tracks computes the same camera-only vector
anyway -- the two sources cannot disagree on an owned pixel.

Debug: temporal_aa_debug = movers paints kinematic-owned pixels a
distinct colour. Occlusion-edge mis-owning is visible as tinted mover
pixels; the ship gate reads this view.

### Config

fix.engine_motion on|off|auto, [fix] section, commented out, default
off. on = tracker + injection live. auto is parsed and behaves as off
with a one-time log line ("reserved until the scene gate lands") --
the enum is stable for when phase 1 proves out (design doc). off =
one configure line, zero runtime cost beyond the hook's atomic load.
Read site: temporal_pass.cpp:6865 area. Contract: add the commented
key + doc block to edvr.ini in the same commit
(tools/check_config_contract.py and the generated audit header enforce
agreement; gen_settings_schema.py requires no ui:/dev: line for a
commented [fix] key -- developer instrument, the log names it).
Config off -> kinematicMotionShutdown() clears all state; labels
re-prove from scratch on re-enable.

### Failure modes, each logged distinctly

- fix.engine_motion=off: exactly one line at configure. (Never-ran.)
- on but zero observations 5 s after configure: stand-down note
  (hook not feeding) -- distinct from healthy.
- A frame with zero records while active: stand-down counter, NOT a
  pass (103339's terminal-present teardown is the reference shape).
- recordOverflow, readFaults, nearMiss, stale-identity drops: counters
  in a periodic summary (20 s cadence, the existing pattern), each
  named, zero included -- absence of the summary with the feature on
  reads as dead instrument, never as success.
- "No mover ever seen" note after a window with zero pose changes
  anywhere: the mover-control principle from 064047, as a runtime
  dead-feed detector.

### Gates and tests

New C++ rig tools\kinematic_motion_test\kinematic_motion_test.cpp,
mirroring the kinematic_json_test pattern (links
src\d3d11\kinematic_motion.cpp with stub Log/hook functions; the
object_record_writer_probe/hook separation precedent). Feeds synthetic
streams and asserts emitted labels + counters:

1. 11x fan-out dups in one frame -> one state update, one sample.
2. Bit-exact static for 3 frames -> eligible; injection set contains it.
3. 1-ulp translation wobble -> never eligible; nearMiss counted when
   < 1 mm.
4. Quat lane churn with constant translation (the rotating-in-place
   class) -> never eligible. THE 103339 regression case.
5. Gap of 1+ frames, same node -> new identity, run restarts.
6. Same pointer, different node -> new identity.
7. > 4,096 distinct pointers -> recordOverflow counted, excess never
   injected, no eviction.
8. A zero-record frame mid-stream -> stand-down counter, not a pass.

Added to build.bat beside :rig_kinematic_json_test (build.bat:1959-1973
pattern), so tracker-logic drift fails the build, not a flight.
UNTOUCHED: KinematicEvalProbe, its writeJson, its fixture, and
tools/kinematic_json_selftest.py -- the tracker shares the hook feed,
not the probe's JSON. temporal_shader_build.exe --self-test stays
green; check the new shader symbol names against the entry-point
collision class in AGENTS.md before flying.

### Build, install, commit flow

Branch codex/kinematic-tracker. Build via the detached PowerShell
idiom (handoff file), ~5 min, read the tail for all gates green.
Install: python tools\install_edvr.py --target frontier, then
--verify-only. Journal: dated settlement-doc entry + both Status
blocks. Commit message to a file, UTF-8 no BOM, git commit -F; merge
--no-ff to main; push; git log origin/main -1. Trace every new
instrument end to end before asking Sean to fly: what does the log
show if the tracker never ran, and is that distinguishable from
success (the logging section above is that contract).

### Flight verification plan

Protocol: unchanged (eye dump with the drone visibly moving, 30 s
wait), fix.engine_motion = on, advanced.temporal_aa_debug = motion
then movers. Expected: configure line; periodic summaries with tracked
~3k, eligible ~2.5k, overflow/faults zero; stand-down counters zero.
In the motion view: settlement geometry carries camera-only vectors;
the drone's pixels unchanged from stock. In the movers view: static
tint covers settlement surfaces, NOT the drone, and NOT occlusion
edges against nearer geometry (the mis-own gate). Quantitative: the
tracker's eligible-set size vs the census's <=1 mm band (2,583 on
103339's window) must agree within the near-miss population. Ship
gate per the design doc: shimmer gone in the diagnostic view; only
then does any DLSS-consuming change get discussed. State the
environment line at flight time (runtime, headset, per-eye size, DLSS
version, kTrackCap).

### Phase-2 seam (not built now)

The tracker already stores prevPose per frame; the rigid delta is
(curT - prevT) plus the canonicalized quat delta. The GPU record
struct reserves kind (0 = static-zero, 1 = rigid-delta) and a 3x4
prev-frame map slot so phase 2 is an upload-side change, not a
re-plumb. Rotation injection stays out of v1 per the design table.
## 2026-09-20 (11:20) -- stage split: A landed, B deferred

The 10:52 spec shipped in two stages. Stage A landed in
v0.17.0-117-g0d042f5c-dirty: the tracker, the shared eval-hook gate,
the present tick, fix.engine_motion and the 35-check build gate -- and
NO rendering change. kinematicMotionViews is not wired and the compose
shader is untouched.

Stage B (the ownership coverage texture and the t19 compose veto) is
deferred on one specific unknown: the world-bounds layout is not
flight-proven and the two decomps conflict (FUN_14432CCC0's four
float4 rows at +0xB0..0xEC vs FUN_14433DB20's 64-byte current/previous
pair at +0xF0->+0x1C0, change-test block at +0x120, center at +0x240).
Projecting the wrong bytes as bounds would feed a wrong motion field
-- the failure this project never ships on a guess. Stage A dumps the
raw +0xB0..+0x130 block for movers and statics on one flight; the
layout decodes offline (position fields shift by the +0x170 delta
between consecutive frames; view products track the head). Stage B
gets spec'd against that decode, not before.
Review round (12:45 settlement entry): nine findings fixed before any
flight -- same-frame dup verification and the both-populations bounds
trigger in the tracker (rig cases 9-12), identity-before-motion,
no-state-from-partial-reads, seed-without-flush and non-finite
rejection in the probe (kinematic_probe_test, 25 checks), and the hook
gate/validation/job-timing races closed by construction.
## 2026-09-20 (13:05) -- the bounds block is the world transform, not bounds

Flight 125207's dump (settlement doc 13:05 entry, raw lines at
12:53:40 in the log): +0xB0..+0x130 is bit-identical across the two
dumped frames for all 16 records and contains no min/max pair.
+0xF0..+0x11F is a rotation (three padded float4 rows),
+0x120..+0x12B the translation -- numerically equal to the +0x170
pose in every dumped record -- and +0xB0..+0xEF a constant identity
3x4 (likely parent-relative, unproven). The "world-bounds recompute at
record+0xB0..0x12C" reading of FUN_14432CCC0/FUN_14433DB20 is refuted;
what recomputes there is the transform. +0x240 is confirmed as the
world centre (statics).

Stage B's ownership coverage therefore needs the extents from
elsewhere: the local AABB (LOD/model data) transformed by this matrix,
or a field past +0x240. The next stage-A instrument widens the raw
capture toward +0x130..+0x250 for the same mover/static dump set
before any GPU work is spec'd.

## 2026-09-20 (13:30) -- the extents are a sphere radius, found offline

Sean's question: can the disassembly answer the extents location
instead of spending a flight on the widened capture? Yes -- the wide
net is cancelled. The full store scan found no direct writer of
record+0x270/+0x280 in the record family, so the write had to go
through a computed pointer; the LEA scan found exactly one family
hit, FUN_14433C870 (LEA RDI,[RSI+0x270]).

That function is the bounds writer, recursive over the record's child
array (+0x2A8 pointers, +0x2B0 count): local centre float3 copied
from model data *(*(record+0x18)+8)+0x20, radius from +0x2C stored as
a single float lane at +0x280 (lanes 1-3 zero), zeroed when the model
is absent. Children are folded in by FUN_140A8C9A0, a bounding-sphere
union (containment both ways, else Ritter-style expand) operating on
{float4 centre; float radius}. FUN_144330194 independently confirms
the record stride 0x2F0 and adds the identity layout: +0x250..+0x264
node qwords + model pointer, +0x268 combined hash, +0x2B8 source node.
The updater's visibility query is a virtual thunk fed with the world
centre (+0x240) and the +0x280 lane -- the game culls on spheres, not
AABBs. Movers' zero +0x240 in flight 125207 is the updater's guard
(no children at +0x2B0, no modifier at +0x290), not missing bounds.

Stage B consequence: world extents = radius at +0x280 scaled by the
max 3x3 column scale at +0xF0; world centre is +0x240 when present,
else M x local centre computed from +0xF0/+0x120. The stage-A.5
capture is therefore targeted: dumpBoundsLocked live-reads
+0x270..+0x28F (32 B) as svalid/s= next to the existing b=/c= blocks
(kinematic_motion.cpp, kSphereBytes). Built v0.17.0-123-gdae06c51-dirty,
gates green (kinematic_motion_test 52, kinematic_probe_test 25),
installed to frontier (d3d11 sha256 5F7626D7). Next flight confirms:
radius > 0 for statics, finite values everywhere, and M x local
centre == +0x240 where the guard allows. Detail: settlement doc,
same-time entry.

Flight 132856 (settlement doc 13:45 entry) confirmed every prediction
on 24 dump lines: s= valid throughout; radii 3.78-12.77 m, plausible
per model and bit-stable across frames; padding lanes and +0x12C zero;
and world centre +0x240 == R^T x local centre + T EXACTLY on all eight
statics -- the +0xF0 3x3 is applied transposed, its rows the basis
vectors' images, unit scale. Movers show the predicted guard-off
pattern (c=0 with the local sphere populated). One merge artifact: a
unioned record's centre w drifts off zero (id=148, w=0.00097); lanes
0-2 are unaffected. Stage B is unblocked on flight evidence, not just
the decomp: world sphere = +0x240 where written, else R^T x +0x270 +
+0x120; world radius = +0x280 x max column scale (1.0 observed).

## 2026-09-20 (13:55) -- stage-B spec: sphere-backed ownership coverage

Stage B unblocked by flight 132856 (13:45 entry). This supersedes the
10:52 spec's Ownership (GPU) section wherever they disagree; everything
else in the 10:52 spec stands. Ship gate unchanged: shimmer visibly
gone in the temporal_aa_debug MV view, measured before/after, before
any DLSS-consuming change is discussed.

### What the flight changed

The 10:52 spec assumed a world AABB (bMin/bMax from +0xB0..0xEC, "the
bounds' 8 corners"). That reading was refuted (13:05 entry) and the
truth is flight-proven (13:45): each record carries a bounding SPHERE
-- local centre float4 at +0x270, local radius float at +0x280 --
written at spawn/LOD refresh by FUN_14433C870, unioned over children.
World centre is computed, never read: R^T x local + T with the +0xF0
3x3 (rows = basis-vector images, unit scale observed) and +0x120
translation; exact to 0.0000 on all 8 dumped statics. +0x240 is the
game's own world centre but is zero for guard-off records, so the
injector does not consume it. World radius = local radius x max 3x3
column scale (1.0 observed; the multiply stays, scales exist in ElDorado
content even if the settlement set didn't show one).

### Tracker change (the only CPU behavior change)

The stasis compare extends from the 20 pose bytes to 20 + 32 bytes:
pose (+0x170..+0x183) plus the sphere (+0x270..+0x28F). Reason: the
sphere has a writer INDEPENDENT of pose -- an LOD refresh rewrites
+0x270/+0x280 with zero pose change, and a stale sphere injected after
an LOD swap is exactly the wrong-MV class stage B exists to kill.
Folding the sphere into the bit-exact compare turns an LOD rewrite
into the standard 3-frame re-proof. The sphere bytes are read per
record per frame alongside the pose (3.5k records x 52 B -- noise).

Eligible-record state gains a cached world sphere (centre float3 +
radius float), computed at eligibility time from +0x270/+0x280 and
+0xF0/+0x120. Recompute happens only on re-eligibility, which the
extended compare forces on any sphere or pose change; a bit-static
pose implies a bit-static matrix (125207: the whole +0xB0..0x130
block bit-identical across frames), so the cache is exact while
eligible. The one-shot diagnostic dump (dumpBoundsLocked) is
untouched -- it stays the regression anchor for future flights.

### GPU upload

Structured buffer, 80 B per record, cap = kTrackCap (4,096; the
tracker never hands over more, so overflow is impossible by
construction -- stated, not handled):

    struct KinSphereGpu {
        float    centre[3];      // world
        float    radius;         // world
        uint32_t kind;           // 0 = static-zero (phase-2 seam)
        uint32_t reserved[3];
        float    prevMap[12];    // 3x4, zero-filled in phase 1 (seam)
    };

Versioned upload: a generation counter bumps when the eligible set or
any member's world sphere changes; the upload is skipped when the
generation is unchanged (the coverage texture still rebuilds every
frame -- the camera moves). Typical settlement frame: zero upload,
one bounded compute pass per eye.

### Coverage pass (replaces "8 corners")

One thread per uploaded record per eye, quarter-res R32G32_UINT
[near,far] reversed-Z span texture, empty sentinel stated, as in the
10:52 spec -- but the projection is the conservative SPHERE rect, not
8 AABB corners: view-space centre (xv,yv,zv) via the eye's
current-frame view rows (the 10:52 camera-rows rule stands and is the
#1 expected bug class); screen rect = NDC(centre) +-
(fx*r/zv, fy*r/zv) with fx/fy from the same frame's projection;
depth span [zv - r, zv + r] mapped through the frame's reversed-Z
mapping, 24-bit quantized, InterlockedMin/Max. Tighter and cheaper
than 8 corner projections; the coarse coverage + full-res depth-gate
precision split is unchanged.

### Compose veto

Unchanged from the 10:52 spec (Compose integration): KC at t19, flag
bit 256, bound at all three SRV-array sites with the nullptr = off
precedent, kinematicStatic(p) beside meshPixel consulted FIRST at both
call sites, uiCovered rejection, the named relative depth margin sized
by the mis-own diagnostic (no silent tuning), and the movers-view
debug paint as the ship-gate readout. The only delta: the [near,far]
span is now the sphere's [zv - r, zv + r] instead of AABB corners.

### Config

No new key. fix.engine_motion=on completes its documented meaning
(tracker + coverage + veto). The edvr.ini doc block's "this build is
tracker plus diagnostics only -- no rendering change yet" sentence is
replaced in the same commit with the phase-1 behavior (coverage +
static-zero veto, movers and unknowns untouched) -- the key, its
position, and the commented form stay; check_config_contract.py runs
in the gate.

### Failure modes (added to the 10:52 list, each logged distinctly)

- Coverage pass with zero uploaded records while active: stand-down
  counter, not a pass (same shape as the zero-record frame rule).
- A record eligible with radius <= 0 or non-finite sphere bytes:
  counted and never uploaded (the failed-reads-fabricate-motion class
  from the 12:45 review -- no state from implausible reads).
- Upload generation unchanged for a full summary window while the
  eligible count moved: counted; stale-buffer suspicion is named in
  the log, never silently patched.

### Gates and tests (extend kinematic_motion_test, currently 52 checks)

1. LOD-swap case: sphere bytes change with pose bit-static ->
   invalidation, 3-frame re-proof, no upload in between.
2. Flight fixtures: 3 dumped records from 132856 (R, T, local centre ->
   expected +0x240 world centre) asserted under 1 mm -- this anchors
   the transpose convention against any future "cleanup".
3. Radius scale multiply: a fixture 3x3 with column scale 2 doubles
   the uploaded radius.
4. Upload set contents and generation: bump on set/member change,
   skip when unchanged; eligible > cap impossible by construction
   (asserted, not handled).
5. Implausible sphere (radius <= 0, non-finite) never uploads and is
   counted.
Shader side: temporal_shader_build.exe --self-test green; the t19
symbol and any new helper names checked against the entry-point
collision class before flying; temporal_pass flag-word bit 256 as
spec'd at 10:52.

### Flight verification plan

Protocol unchanged (settlement, drone visibly moving, 30 s,
fix.engine_motion=on, eye dump; temporal_aa_debug = motion then
movers). Expected: the 10:52 tracker expectations plus generation/
stand-down counters at zero; movers view paints static tint on
settlement surfaces, NOT the drone, NOT occlusion edges against
nearer geometry; motion view shows settlement geometry carrying
camera-only vectors. Quantitative: eligible count vs the census band
as before. Environment line mandatory: VR runtime, headset, per-eye
render size, DLSS version, kTrackCap.

### Non-goals (unchanged, stated so they are not folded in)

No mover injection (phase 2, the kind=1 seam), no rotation injection,
no DLSS-path change, no new config key, no eviction, no epsilon
tuning anywhere in the chain.

## 2026-09-20 (stage-B landed)

Stage B shipped as 6b90d0b on codex/stage-b-ownership, built as
v0.17.0-129-g305eeffc-dirty (pre-commit describe), installed to
frontier (d3d11.dll sha256 81d5d27f434b2458, install_edvr.py
--verify-only green). Gates: kinematic_motion_test 78 checks, 0
failures (rig grew 0x280 -> 0x2C0 for the +0x270 sphere read; cases
13-17 cover the LOD swap, three 132856 flight fixtures under 1 mm,
radius x2 column scale, generation semantics across drop/republish,
implausible-sphere rejection); temporal_shader_build self-test green
with kTemporalKinBytecode in the generated header; config contract
252/252; full native build green.

What shipped, per the 13:55 spec: stasis compare extended to
pose+sphere at all five observe() sites; worldSphereLocked (centre =
R^T x local + T, radius x max column scale); rebuildSnapshotLocked
content-addressed per ended frame; kinematicMotionSphereSnapshot with
count = full set size under truncation; sphereChanges/sphereRejected/
uploadGeneration/uploadStuck/uploadedLast stats and the stale-upload
named log line; kinCover quarter-res ownership coverage on t19/t20;
the kinematicStatic veto at both meshPixel sites and down the mv()
owner chain; cyan paint last in both movers views; the edvr.ini doc
block now describes phase 1 (proven-static takes camera-only motion;
movers/unknowns stock).

Deviations from the 13:55 spec, all forced by the API or the existing
bit layout:
1. Two R32_UINT coverage textures (near/far) instead of one
   R32G32_UINT -- SM5 atomics (InterlockedMin/Max) require
   single-component 32-bit UAVs.
2. probe.w ownership bit 512u, not 256u -- 256 is the existing
   staticOwner bit.
3. The spec's 24-bit depth quantization dropped for full asuint float
   bits (monotone for positive floats; no precision lost).

Flight-untested by construction: kinCover runtime behaviour
(projection, atomic clears), the stale-upload summary check, the
kcBound plumbing on the own (non-DLSS) path -- Sean flies DLSS, so
the NVIDIA path is what the verification flight exercises. Protocol
unchanged from the 13:55 flight-verification plan; fix.engine_motion
must be on in the live ini.

## 2026-09-20 (stage-B review response: diagnostic-only + three fixes)

The 13-finding review of 6b90d0b (frozen copy at build/review-stageb/
review.md) recommends the ownership mask stay diagnostic-only until
current-frame validity, camera inputs and actual pixel ownership are
established. Accepted and landed as 5fb4f62 on codex/stage-b-review-fixes:
ownership alone now only paints the movers-view cyan; the compose veto
arms per frame with fix.engine_motion_veto (new key, default off) via
probe.w bit 1024, and only while coverage is bound. Every finding was
verified against the code before the triage below.

Fixed now -- the three findings that gate a trustworthy diagnostic flight:

1. Finding 1 (P1, all-zero camera transform): CONFIRMED -- PassParams is
   zero-initialized and wR0..2 are filled only by the body/ship motion
   paths; with neither, kinCover read every sphere as straddling the eye
   (full-eye paint plus ~2.2B atomics/eye at cap). The coverage pass now
   calls chooseCameraRows() (idempotent in-frame), stands down with a
   one-time log line when g_curValid is false, and copies g_curRows --
   the frame's chosen camera rows -- into KCParams.
2. Finding 4 (P1, generation reuse across tracker restart): CONFIRMED --
   the generation restarts from zero per session while the GPU cache
   survived. The tracker now bumps a process-monotonic session epoch in
   clearLocked (kinematicMotionSession()); the coverage pass uploads on a
   (session, generation) mismatch, so a restart's generation 1 is a fresh
   upload, never a reuse. Rig case 18 pins it.
3. Finding 5 (P2, source-region offset in coverage lookups): CONFIRMED --
   the coverage pair is eye-local but every caller passes source-texture
   coords. kinematicStatic subtracts region.xy before the quarter-res
   lookup; out-of-range loads were already the reject side. The copy path
   (region origin zero) had hidden this.

Deferred to the veto-enable batch, with the reviewer's verdict accepted:

- Finding 2 (interval union is not pixel ownership): structurally correct
  -- one [near,far] span per texel merges overlapping spheres, and no
  sphere representation establishes visible-surface ownership anyway. THIS
  is what the diagnostic flight quantifies: the cyan must not land on the
  drone or across occlusion gaps. If it does, phase 1 needs surface-level
  ownership, not margin tuning.
- Finding 3 (movement-start frame keeps the veto): confirmed a one-present
  stale window; it self-heals at the next frame census (unobserved records
  drop out automatically). One frame of lag is harmless for the paint;
  immediate invalidation on stasis-break lands with the veto.
- Findings 6-10 (own-path veto consistency, UI-mask demand, conservative
  tangent projection, sigma-max radius, scale-change revocation): all
  confirmed in code; each matters only when the veto arms. Finding 9's
  under-estimate and finding 8's under-coverage are the safe direction
  (fewer false vetoes); 6/7/10 are wrong-veto directions and are
  veto-blockers, all fixed before fix.engine_motion_veto loses its
  developer-instrument warning.
- Findings 11-13 (probe capture concurrency): the probe is flight
  instrumentation, not the compose path; next probe touch, before the
  next capture flight.

Performance note accepted: kinCover's per-sphere rectangle expansion is
unbounded work -- with the camera fix the full-eye case needs a real
eye-straddling sphere, but the pass gets its own frame-time measurement
before the veto is treated as an optimization.

Gates: kinematic_motion_test 82 checks 0 failures (case 18 added),
shader self-test green, config contract 253/253 (fix.engine_motion_veto
documented), full native build green. Installed to frontier: d3d11.dll
sha256 d6d5ef39ceb2a0ca, --verify-only green. The verification flight
protocol is unchanged and now carries zero motion-corruption risk:
fix.engine_motion = on alone yields coverage + cyan with byte-identical
motion; the movers view IS the instrument.

## 2026-09-20 (flight 152934: review fixes hold, cyan question open)

Flight 152934, frontier install d6d5ef39ceb2a0ca (= 5fb4f62, log build
v0.17.0-132-g2c38d076-dirty), settlement, ~3 min, Quest 3 /
VirtualDesktopXR / RTX 5090, DLSS DLAA 1996x2121 -> 3072x3264 per eye --
the NVIDIA path, the one the coverage plumbing serves.

All three review fixes behave in flight:

- Finding 1: the no-camera-rows stand-down fired EXACTLY ONCE, at
  15:29:40 during menu/load (zero-record phase), and never again -- in
  the settlement every frame had valid chosen rows (upload generation
  advanced continuously 27 -> 652 -> 827 -> 926 with the census).
  Pre-fix this flight would have painted the whole eye off a zero
  camera at menu.
- Finding 4: no stale-upload suspicion line anywhere in the log; the
  generation tracked content (sphere changes 12,448 cumulative into 926
  bumps -- multiple same-frame changes collapse into one rebuild, as
  designed).
- Finding 5: no shifted-coverage symptom is visible at this level of
  instrumentation (it would have read as mis-owning cyan -- see the
  open question below).

Tracker health at settlement scale: 3,247 tracked, ~2,600 eligible and
published (spheres 2,569-2,606), movers 378-437 per frame, 87.7M
observations with 91% fan-out dups (expected ~11x eval fan-out, all
deduped), faults 0, overflow 0, same-frame invalidations 0, gap drops 5
late (scene exit). The empty-snapshot stand-down note fired only
pre-scene (menu/loading), as it should. Bounds dump part A/B re-fired
clean (8 movers + 8 statics, svalid=1 on all 24 lines) -- the sphere
layout re-confirmed on this build.

One curiosity, not a defect: sphereRejected accumulates ~2.9 per
eligible frame (8,886 total) -- a small persistent population of
eligible records whose sphere bytes fail plausibility (zero radius,
likely LOD'd sub-records). Excluded and counted as designed; worth a
decode only if the coverage flight shows holes.

OPEN -- the movers-view cyan question has no evidence: the eye burst
(frames 11233-11248, ~0.18 s at 90 Hz) shows the plain render in every
channel (C/P/T) with constant flags; the temporal_aa_debug toggle was
not inside the burst window. The mask's pixel quality -- cyan on
settlement surfaces, NOT on the drone, NOT across occlusion gaps (the
finding-2 question) -- awaits a dump taken while the movers view is
HELD active.

## 2026-09-20 (16:10: movers view = whole scene strobing; burst instrumented)

Sean, 15:45, headset-only with fix.engine_motion=on and
temporal_aa_debug=movers: "everything outside my cockpit is a single
color that cycles rapidly". Read: the stage-B ownership paint (cyan)
claims ~the whole non-cockpit scene and strobes -- either the coverage
is genuinely full-screen (vast over-ownership) or the bind flaps per
frame (paint/no-paint strobing). No log or dump: the test was visual
only, and the burst again missed the toggle. Per diagnosis discipline
this is instrumented, not fixed blind -- three hypotheses, one flight
eliminates all:

- **H1 straddle paint-all.** kinCover treats a sphere straddling the
  eye (zv - r <= 0.05) as covering the WHOLE quarter-res texture
  (temporal_shader_source.h kinCover). The player's own landed ship is
  static, eligible, and its sphere is centred on the player -- it
  straddles the camera by construction. Head sway (or the gen churn
  seen in 152934: 926 bumps / 2,200 eligible frames) would strobe it.
  Signature in the dump: a sphere in KinSpheres.bin whose centre is
  within radius of the camera position (motion.csv cameraTv), AND
  KCNear/KCFar at ~full coverage.
- **H2 bind flapping.** Bit 512 (kcBound) oscillating frame to frame
  paints/unpaints cyan without any coverage change. The 152934 log
  shows the empty-snapshot stand-down fired only twice, pre-scene --
  but per-frame bit state was unlogged until now. Signature:
  decisions.json kin_bound alternating within one burst.
- **H3 interval-union mis-ownership at scale** (review finding 2
  realized): 2,606 spheres merged into one nearest-to-farthest depth
  interval per texel claim all mid-field depths. Signature: coverage
  wide but NOT full; sky rejected by the depth gate (zraw <= knobs.x);
  no straddling sphere in the dump.

A settled design note, held until the dump lands: straddle-paint-all is
the DANGEROUS direction for an ownership veto (a sphere containing the
camera proves nothing about pixels); paint-none is the defensible flip.
Do not ship it off H1 alone -- the dump decides.

The instrument (this commit, temporal_pass.cpp only, diagnostic-only,
no rendering change): writeEyeDecisionArtifacts now also writes

- eye_<stamp>_KCNear0/1.bin, KCFar0/1.bin -- EDVRTEX1 R32_UINT readback
  of each eye's coverage pair as of the run's last treated frame
  (textures persist post-run; header frame = that frame);
- eye_<stamp>_KinSpheres.bin -- EDVRKSP1: count, session, generation,
  then count x 80-byte KinematicSphereGpu rows, copied from the GPU
  UPLOAD BUFFER (what the last frame composed with, not a fresh tracker
  snapshot -- the tracker may have re-published during the burst);
- decisions.json schema 2: top-level "kinematic" block (session,
  generation, count, kc_frame, kc_size, file names, nulls when stage B
  stood down) and per-frame "kin_bound" (bit 512) / "kin_veto"
  (bit 1024) stamped at both kcBound call sites (NVIDIA path and own
  path), gated on the live eye-run slot.

Offline analysis plan (no flight needed once the dump exists):
recompute the shader's kinematicStatic per pixel from KCNear/KCFar +
KinSpheres + the camera rows in eye_<stamp>_motion.csv, classify
H1/H2/H3, and count claimed vs total texels per eye.

Gates: kinematic_motion_test 82/0, kinematic_probe_test 25/0,
kinematic_json_test green, shader self-test PASS, config contract
253/253, full build green. Installed to frontier: d3d11.dll sha256
ac6758093e45419d (= build output, --verify-only green).

Flight ask: settlement, a mover in view (the drone), fix.engine_motion
= on, temporal_aa_debug = movers HELD active, THEN trigger the eye
burst. Two questions for Sean while watching: is the colour cyan, and
does the SKY paint too? Sky painting would refute all three hypotheses
(the depth gate should reject it) and point somewhere deeper.

## 2026-09-20 (16:20: dump 160734 read-out -- paint-all confirmed, mover owned, 15:45 reframed)

Flight 160359, dump stamp 160734 (frames 14939-14954), settlement, a
ship landing in view, temporal_aa_debug = movers HELD through the
burst -- the protocol the 15:40 entry asked for. Right build
(v0.17.0-137-g784f017d-dirty == the instrumented temporal_pass.cpp of
6fd1e17; HEAD's merge commit 1fe33b9 post-dates the link, hence the
edvr_log.py --expect-build mismatch note).

Findings, all from the new burst artifacts:

1. **H1 CONFIRMED -- straddle paint-all owns the whole eye.** KCNear /
   KCFar (499x531 quarter-res, both eyes) are 100% claimed with ONE
   uniform interval: near bits = asuint(0.5) -- every winning thread
   hit the 0.05 m zNearM clamp (knobs.z/0.05); far bits =
   asuint(3.7287e-05) ~= 0.025/671 m. A uniform value over 264,969
   texels can only come from the paint-all branch
   (temporal_shader_source.h:1742); any projected-rect path would leave
   near < 0.5 somewhere. Straddlers at the kc frame: 10 under the
   cameraR rows -- the own-ship candidate pair sphere[876]/[980] (r=30
   m, centre ~10 m off, containing the eye under every row set) and
   sphere[1284] (r=467.7 m, a settlement-scale structure) among them --
   and 92-94 under the now/prev rows. Which row set g_curRows held at
   14954 is OPEN (the KC textures reflect the last pass frame, which
   can post-date the last captured frame; their far value matches
   sphere[1284] at |zv| ~ 203 m with the sign flipped vs cameraR.
   kc_frame currently labels the last CAPTURED frame, not the pass's
   paint frame -- an instrumentation gap to close if the provenance
   matters). Either way containing spheres are pervasive at a
   settlement, so paint-all fires every frame there.
2. **H2 REFUTED.** kin_bound true on all 16 frames, kin_veto false
   (veto dark, as designed). The bind does not flap.
3. **The mask owns a mover.** T15 (the movers paint, last burst frame):
   ground and settlement cyan, sky correctly dark (the depth gate
   rejects zraw = 0 -- the 16:10 sky check PASSES, nothing deeper is
   wrong), the horizon band beyond 671 m correctly unowned -- and the
   LANDING SHIP cyan, top right. Sean confirms in-headset: the landing
   ship was cyan. The published set held only static records (2,569
   spheres, gen 1510, session 1), so the ship's pixels are claimed
   through OTHER spheres' intervals -- review finding 2 (bounding
   spheres do not establish pixel ownership) made visible on a known
   mover. Had the veto been armed, that ship would have composed
   camera-only motion: the exact corruption class the design must
   never ship. The diagnostic-only stance is vindicated.
4. **The 15:45 strobing is reframed.** Sean's 16:08 correction: it was
   the MOTION view (split.y == 1: mvUsed as RG, worldTaken as B), not
   movers. The H1/H2/H3 cyan framing never applied to it. A
   whole-scene rapid pulse in that view is the MV FIELD itself
   oscillating -- the settlement MV pathology this arc exists to fix,
   made directly visible -- independent of stage B (veto dark; stage B
   never touches MVs without it). Its discriminating capture is a
   burst held in the MOTION view: 16 consecutive T crops of the mvUsed
   paint plus motion.csv (originStep cadence is a suspect: a floating-
   origin rebase mishandled for one frame pulses every static MV at
   once).

Fix direction (design call for Sean, not yet built): straddle ->
paint-none in kinCover (`return` instead of the full-rect paint). A
sphere containing the camera proves nothing about pixel ownership, and
the eye-plane grazers (sphere[914] r=2.6 m zv=1.9 m; sphere[1017]/
[1047] r<=1.1 crossing zv=0 with millimetre head motion) are
flicker-prone by construction -- they are what a movers-view strobe
would have meant. After the flip, re-check mover ownership in a
re-flight: if the landing ship is still cyan via non-straddling
spheres' unioned intervals, the finding-2 tightening (per-pixel
interval overlap vs scene depth) is next.
