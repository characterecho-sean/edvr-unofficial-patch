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
  to frontier, verified).

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
