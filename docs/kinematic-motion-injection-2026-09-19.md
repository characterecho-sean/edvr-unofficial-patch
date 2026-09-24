# Kinematic-sourced motion-vector injection: a design

## Status

- **State:** DECIDED DIRECTION, 2026-09-19 (Sean): engine-level injection from
  KinematicRig truth, not draw-call interpretation (ruled out as a class); phases
  0/A/B flown clean 2026-09-20 (B's veto since removed); four engine-truth flag routes
  closed. RESUMED 2026-09-23 with Sean's requirement -- GENERALIZED, no estimation
  anywhere, not even as a fallback: camera-only for statics, engine-record delta via
  substituted pool shaders for movers (design C fed by B), the reactive mask the only
  fallback. Design A (pool content-pairing) measured wrong and an engine velocity
  buffer ruled out with blur off and on (the 2026-09-23 entries); B+C IS THE DESIGN.
  PHASE 1 + FIX ROUND MERGED and FLOWN (093817, "The re-fly" entry): eye-frames bound
  and given, invalidated 0, corrupt codes 0, engine-joined 67-73k px an eye-frame.
  FOLLOW-UPS FLOWN (114958): cockpit prep 0.15-0.23 ms a pair, no faults. STAGE B
  REMOVED (engine motion is part of fix.temporal_aa); PERFORMANCE ROUND merged. ON
  FOOT FLOWN (flight 5, 140351, "Flight 5" entry): standing, source views given
  2938/2938; WALKING, EVERY SOURCE FRAME DROPPED (2031/2031) by the eyes' rows rule --
  the source is drawn by more than one camera. FIXED, MERGED, NOT FLOWN: each source
  pool draw held to the naming draw's camera, the others declined, the frame kept.
  HANGAR (flight 6, "The hangar" entry): no terrain draw, so NOTHING NAMED the source
  (no on-foot line at all); now the screen's own depth names it. BUILT, NOT FLOWN.
  STATION (eye run 143416, "The station" entry): the joined station pixels are EXACT
  (one rigid motion to 0.002 px at the records' own 0.106-0.110 deg a frame), but 55%
  of its pixels kept the camera term, 0.31-0.35 px a frame short. KEYED,
  harness-proven, NOT FLOWN: ps_CB42 (DE54) and ps_451A (61AE); then from flight 6's
  dump ("Flight 6" entry) vs_4361 + ps_1694 (547 of the station's 1193 instances a
  frame) and vs_889A + ps_B46E / ps_EBA9. vs_DE54's ps_91F8 and ps_A607: refused by
  the patcher. TODAY (162703, 16:27, build f05c84bf): the hangar and the station FLOWN
  OK -- no faults or stand-downs; vs_4361+ps_1694 and vs_889A+ps_B46E/ps_EBA9 live and
  substituted at the station (2982/29422 and 360/2206 draws per window); the hangar
  source named by its own depth as built (5088x2862 R32G32, 116.5 MB); on-foot windows
  clean (764/764 then 5398/5398 given, 0 invalidated); Sean judged both good.
  TEARDOWN (last entry): the per-object estimates (per-object-motion.md) and the legacy
  kinematic tracker retired 2026-09-23; the emit's census still counts the undrawn movers.
- **Open:** walkers (vs_F516BF0201303B87, not a pool family; w=2 on the panel) wait on
  phase 2's previous bone palette -- a walking NPC still blurs after the on-foot fix;
  which camera the walk's other draws use (the new line's rows and distance say); a
  temporal pass on the flat source for its own aliasing; the stale cockpit (the
  commander's legs under ps_B7D5 and vs_7B0DC42D, DECIDED not keyed; 35.4% undecided, low
  priority); the census's evaluated-but-not-drawn movers (the 09:38 entry); the boarding
  flicker (the LOD governor, not this arc); vs_DE54's ps_91F8 and ps_A607 (the family's
  SV_Position register holds another semantic in them: a patcher extension); ships in
  space; builder-path movers and articulated parts (phase 2).
- **Ruled out (do not re-propose; each closed in its dated entry):** draw-shape memo
  identity, pool-slot identity, 3x3 SAD camera-vs-body match, hidden-bone-spin
  estimation (pre-2026-09-20); four engine-truth mover/static routes (2026-09-20
  21:25); an engine velocity buffer with blur off or on, and record+0x1C0..0x1F8 as
  previous-frame truth (2026-09-23 entries); design A pool content-pairing (evening
  entry); the fix round's five hypotheses -- a scaled history delta, a straddling
  tick, rows 270..275 mid-pass, MRT6-blend coverage loss, and the substituted shaders
  altering the G-buffer (Fix round entry); one camera per on-foot source frame (Flight
  5); the on-foot world packed per eye, enginePixel's fetch as the ~3 ms prep, and a
  depth pre-pass/bias as the stale cockpit (the re-fly entry); green cockpit panels as
  a mover bug, and engine-path CPU cost as the frame-time cause (09:38 entry); the
  station's joined motion wrong at range (The station entry).
- **Next:** the controlled diagnostics 1 vs 0 comparison, and a walker near a drone
  with the motion_source view.

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
would have meant. Sean sanctioned it at 16:22; BUILT at 16:35 (next
entry). After the flip, re-check mover ownership in a re-flight: if
the landing ship is still cyan via non-straddling spheres' unioned
intervals, the finding-2 tightening (per-pixel interval overlap vs
scene depth) is next.

## 2026-09-20 (16:35: straddle -> paint-none built, paint-frame stamp closes the provenance gap)

Two changes, one build (Sean sanctioned at 16:22 "go"):

1. **kinCover's straddle branch flipped to paint-NONE**
   (temporal_shader_source.h): a sphere straddling or containing the
   eye returns without painting. The projected rect is unbounded at
   the eye plane; a containing sphere proves nothing about pixel
   ownership (the 16:20 entry's evidence). The eye-plane grazers this
   drops were flicker-prone by construction. Ownership of big
   containing geometry (the landed own-ship's r=30 m pair) now comes
   from tighter per-record spheres or not at all -- those pixels
   revert to stock handling, never to a whole-eye claim.
2. **e.kcPaintFrame stamps the game frame at each coverage paint**;
   the burst dump writes it into the KC bin headers (per eye) and the
   manifest as kc_paint_frame [eye0, eye1], closing the 16:20
   provenance gap (kc_frame remains the last captured crop frame).

Gates: kinematic_motion_test 82/0, kinematic_probe_test 25/0,
kinematic_json_test green, temporal shader compiler PASS (bytecode
rebuilt), config contract 252/252 (the vscreen merge took one key;
internally consistent). Installed to frontier: d3d11.dll sha256
d0caa44f0e926188, verified against the build. UNFLOWN.

Flight protocol: settlement, fix.engine_motion = on,
temporal_aa_debug = movers HELD, eye burst. Expected: no whole-eye
cyan, no uniform full-claim interval -- honest rects only. A mover
still cyan means non-straddling unions own it and the finding-2
per-pixel tightening is next. Separately, a burst with
temporal_aa_debug = motion HELD captures the 15:45 MV-field pulse
(16 consecutive mvUsed paints + motion.csv). The burst read-out is
now one command: python tools/kin_coverage.py --target frontier
(paint-all signature, bind flap, straddlers per row set; --self-test
18 checks, wired into the build gate).

## 2026-09-20 (17:05: flip flown -- whole-eye cyan gone; movers owned via unions; the MV-field oscillation captured)

Flight 163300 (the flip build, gc4bceb50-dirty == 5313a81's content;
HEAD had moved on docs/tools only). Three bursts: 163502 with
temporal_aa_debug = motion, then 163619 and 163645 with movers HELD.
Tracker healthy (3,503 tracked, ~2,570-2,682 eligible, 433-558 movers,
0 faults/overflow, upload gen advancing 1,502 -> 1,757).

**The flip holds in flight.** Movers bursts: claimed 49.9/52.9% and
45.2/43.5% of texels (was 100%), NO paint-all signature, dominant
interval share 25-31% (was 100%) -- honest varied rects. The 10-94
straddlers still exist but paint nothing, as designed. kc_paint_frame
stamps correctly and equals kc_frame (the textures WERE the last
captured frame's this time).

**Movers are still cyan -- finding 2 confirmed on movers.** Sean,
in-headset: a visibly moving gun turret is cyan, and a landing ship
painted cyan once it got close to the ground. T15 of 163645 shows a
ship silhouette cyan against (correctly dark) sky, with a partial dark
cutout -- partial-depth ownership. Mechanism: unioned [near,far]
intervals from OTHER spheres claim the mover's pixels when its depth
falls inside their span; near the ground the ship's depth enters the
ground structures' intervals. The published set holds statics only
(gen 1,558, count 2,569) -- the mover's own records are not the
source.

**The offline rect reproduction is not yet faithful.** Spheres whose
projected rects should cover a claimed texel explain only 52.2% of
claimed texels under the now/prev rows and 76.8% under cameraR/headR --
never ~100%, so the pass's exact inputs differ from every set
motion.csv carries (g_curRows provenance, tan variant, or a publish
between the paint and the dump). Next instrument: dump the 96-byte
KCParams cbuffer (tanNow, wR0..2, knobs, size/count) per eye with the
burst -- exact inputs, no row-set guessing. Until then, per-mover
attribution (which sphere owns the turret's pixels) is deferred, and
with it the finding-2 tightening design.

**Motion burst 163502 settles the 15:45 strobing.** The motion view
paints mvUsed only (R/G = mv/16 px, B = worldTaken), so the capture is
16 consecutive frames of the MV field itself:

- The field OSCILLATES sign-alternating frame to frame: median
  frame-to-frame change 59/255 levels (~3.7 px at output scale), and
  45% of strongly-changing pixels flip sign >=10 times in 14
  opportunities. Geometry edges saturate (255). This is the cycling
  colour Sean saw -- the MV field pulsing, not a paint artifact.
- The cadence matches the game's own jitter: jitter0/1 flip sign every
  frame (Halton-like, amplitude <=0.44). But jitter at that amplitude
  explains only ~1/5 of the observed oscillation (~0.7 px
  output-scaled vs ~3.7 px) -- either the jitter term enters the
  composed MV at a wrong scale (and possibly twice), or another
  per-frame alternator exists. Enumerated, not concluded.
- originStep is 0 on all 16 frames: the floating-origin rebase
  hypothesis is REFUTED for this window.
- worldTaken (the B channel, path selection) is steady at ~140 --
  paths don't flip; the VALUES oscillate.
- Caveat for future bursts: dtMs quadrupled mid-burst (11.1 -> 43.5 ms
  at frame 12952) -- the burst's own staging perturbs frametime. The
  oscillation is present in both halves (T00-T05 wobble too), so the
  finding stands, but frametime-sensitive read-outs should use the
  11 ms prefix.
- Tracker's camera arbitration shows in the log: "the camera's delta
  was dropped on 34 eye-frames as another camera's (over 3 deg from
  the head's)" -- multiple camera sources exist and are arbitrated;
  relevant to the g_curRows provenance question above.

No rendering change this entry; the veto remains dark. Next steps, in
order: (1) KCParams cbuffer in the burst -> exact mover attribution ->
finding-2 tightening design; (2) the MV oscillation's alternating
term: jitter units/scale audit in the compose (input vs output pixels,
single vs double application) is an OFFLINE code read before any
flight is spent.

## 2026-09-20 (17:02: disassembly answers -- render path mover-blind; landscape-cyan expected)

Two questions from Sean (16:56), answered offline against
analysis/decomp and dump 163645. No code change; no flight spent.

**Shouldn't the landscape be entirely cyan? No.** Cross-tab on dump
163645's movers view (499x531 sampled grid; TerrainIndex semantics per
the terrain-history-shimmer doc: set = terrain pixel, the terrain
path's exact gate being TerrainZ == sceneZ):

| region                | claimed | not claimed |
|-----------------------|---------|-------------|
| terrain (46.6%)       | 67,228  | 56,285      |
| non-terrain (53.4%)   | 52,472  | 88,984      |

54.4% of terrain pixels are claimed; 37.1% of non-terrain are. The mask
is built ONLY from rig-record bounding spheres, and terrain publishes no
rig records -- terrain paints only where a structure sphere's rect +
depth interval happens to overlap it; open ground stays dark by design.
Where terrain IS claimed it is benign once the veto arms: static
terrain's MV equals camera-only, so the veto emits the same value
either way. The wrong case remains finding 2 -- a mover in front of
terrain/structure claimed by unioned intervals (Sean's cyan turret, the
landing ship near the ground).

**Can disassembly categorize movers from static? Partly -- and it
re-drew the map:**

- eval 0x430EFE0's ONLY in-code caller is FUN_144312040+0x4C4
  (0x4312504). Capturing the return address at the eval hook is
  degenerate -- it always names the traversal. Ruled out as designed.
- FUN_144312040's callers: job0 (UpdateRenderDataJob 0x4321940), job1
  (render-data batch 0x4320340), and ITSELF -- recursion over the
  child-context array at param_5+0xAA. Two jobs x recursive descent
  explains the ~11x per-record fan-out with no physics involvement.
- Neither the traversal nor job0's body branches on any static/dynamic
  flag: the per-record gates are render eligibility (ptr+0x290 != 0,
  bool234, byte+0x298) plus a 4-bit draw-distance category nibble
  compared to a threshold. The render path treats movers and statics
  uniformly -- MOVER-BLIND.
- Rig eval 0x431AFE0's only caller is a -0xD0 this-adjust vtable thunk
  (FUN_1442DF460, vtable slot 0x45591CB8): virtual dispatch, no
  static/dynamic signal there either.
- The engine's dynamic truth, if explicit, lives in the physics jobs:
  UpdatePhysicsObjectsJob 0x432B2A0, PrePhysicsAdvance 0x42DF530,
  CurveJob 0x42DF550. None have decomps in analysis/decomp, and no
  direct xrefs to the eval/traversal exist (indirect calls would not
  show) -- whether physics routes records through this eval is unknown.

The instrument that answers it empirically, no new hook sites:
bracket() already wraps all six job bodies; add a TLS current-job
bitmask set at bracket entry and cleared at exit, and evalObserved ORs
the bit into the record's per-frame mask. Movers evaluated under
physics jobs light bits 2/3/4; statics never do. If physics never
reaches this eval, that null result is itself the answer -- then
decompile the three physics bodies to find the dynamic-list walk and
the right hook point. Zero-flight fallback, runnable today on existing
capture JSON: cluster the probe's per-record fields (pred_vtable_rva,
flags, bool234, gate2) seeded with the known drone records (064047)
against settlement statics.

Order proposed: (a) offline vtable/flag clustering, no flight; (b) the
TLS job-attribution bit rides the next instrumented build alongside the
KCParams cbuffer dump (both probe-side, no new hook sites); (c)
physics-job decomps if (b) comes back null.

## 2026-09-20 (17:10: clustering flown offline -- the record population is homogeneous)

Step (a) of the 17:02 plan, run across every classification capture in
the pool (analysis/kinematic_vtable_cluster.py, report at
analysis/kinematic_vtable_cluster.txt). Ground truth per the 06:45
protocol: net +0x170 displacement (xf words 8/9) > 1 cm = mover;
xf_changes == 0 = static; churn-without-displacement and node_changes
excluded. Label validation: flight 064047 yields exactly the documented
125 movers > 1 cm. 12 flights carry real labels (043344/061722/205251
unparseable pre-quote-fix JSON; 050820/054002 pre-xf-capture, all
ambiguous; 203342 pre-capture, zero records).

VERDICT: NULL. Across all 12 labeled flights (~2.6k statics and
35-569 movers per flight):

- pred_vtable_rva: ONE value (0x52e9288) for every record, mover or
  static, every flight. pred2_vtable_rva 0x0 and flags 0x4040 likewise.
- gate2: 16 values, ALL shared between movers and statics.
- pred_byte: both values shared. epoch1b8: epoch-counter coincidences
  only (movers refresh in epochs statics skip -- consequence, not type).
- Sole anomaly: bool234 == 0 appears on a handful of movers in 5
  flights, never on a static -- but that byte is render eligibility in
  job0's own gate, so those are teardown-frame records, not a class.

ruled out: record-field clustering (pred vtable, flags, gate bytes,
pred_byte) as a mover/static discriminator -- the eval population is
homogeneous in every captured field across 12 flights. Combined with
the 17:02 decomp result (render path mover-blind), the engine-truth
candidates narrow to ONE: which jobs touch the record. The TLS
job-attribution bit on the existing brackets is now THE discriminator
experiment, and the behavioral displacement signal (what the tracker
already ships) remains the only proven in-population split.

## 2026-09-20 (17:50: job-attribution instrument landed -- the discriminator flies next)

Sean sanctioned the 17:10 plan's step (b). Built, gated, installed
(frontier d3d11 5502228f87889997, verified):

- bracket() maintains a TLS current-job bitmask (1u<<jobId), set before
  the early-return path so a stood-down probe never drops attribution;
  a scoped guard restores on exit, so nested jobs keep both bits.
  evalObserved reads it and passes it to both consumers; the tracker
  observer signature gained the mask (default zero, rigs unchanged in
  shape).
- Probe: RecordState.jobMask accumulates the OR of every job the record
  was EVER observed under; serialized as job_mask in the records array
  (fixture 0x15/0x1, the selftest asserts the round-trip).
- Tracker: TrackedRecord.jobMask accumulates the same way; the
  ended-frame census adds eligiblePhysLast/moversPhysLast over the
  physics bits (2|3|4 = UpdatePhysicsObjectsJob, PrePhysicsAdvance[Curve])
  and the 20 s summary line reports "phys-touched eligible %u movers %u"
  -- the discriminator answer lands in the gfx log with no eye dump.
- No new hook sites, no config key, no rendering change; the veto stays
  dark.

The flight question, verbatim: do records that MOVE ever carry bits
2/3/4, and do proven-static records never carry them? Reading it:
moversPhysLast > 0 with eligiblePhysLast == 0 across a settlement
flight with a known mover = the engine schedules dynamics under physics
jobs and (jobMask & 0x1C)==0 becomes a publish gate candidate.
eligiblePhysLast > 0 = physics jobs touch the render-eligible set too;
attribution then does not discriminate, it joins the ruled-out list,
and the physics-job decomps (0x432B2A0 / 0x42DF530 / 0x42DF550) become
the next offline step. Per-record job_mask in the classification JSON
gives the distribution offline. Known caveat: job3 refused to hook in
flight (thunk/foreign-hook leading instruction), so bit 3 may stay dark
even if that job evaluates records -- bits 2 and 4 are the live probes.

Gates: 87/0 tracker (5 new checks incl. a phys-touched census case),
28/0 probe (3 new: mask accumulation + serialization), kinematic json
self-test passed, contract 252/252. Flight protocol: fix.engine_motion
on, a settlement with a known mover (pad drone or ship traffic), >=60 s;
the summary lines carry the census answer, an eye burst adds the
per-record job_mask distribution.

## 2026-09-20 (19:10: flight 190122 -- job attribution is a symmetric null)

Flight edvr_gfx_20260920_190122.log (build v0.17.0-160-g4ee8c97d-dirty
= the 89bbfd5 content, hoist included; verified via edvr_log.py).
fix.engine_motion on, >60 s, normal view, no eye burst. CodeHook:
eval, jobs 0/1/2/4/5 and rig-eval all installed; job3 refused as usual
(thunk prologue). One STAND-DOWN note is the menu period (10,554 of
13,872 frames zero-record -- load + menus), scene records from ~19:02,
settlement-scale from 19:03.

The settlement window (19:03:02-19:04:02): ~2,570 eligible statics and
426-510 movers per ended frame, 672 movers total, feed healthy (95.6M
observations, 63% dup fan-out as expected). The discriminator answer:
**phys-touched eligible 0, movers 0 -- on every summary line.** With
~500 records moving per frame under live job-2 (UpdatePhysicsObjects)
and job-4 (Curve) brackets, no eval observation ever landed inside a
physics bracket. The 205251 JSON measured jobs 2/4 firing (>0 calls),
so the brackets work; the physics path simply never routes these
records through FUN_14430EFE0.

ruled out: job attribution as the mover/static discriminator -- not
because physics touches everything, but because it touches NOTHING in
this population. All three engine-truth routes at the current hook
points are now closed: mover-blind render path (17:02), homogeneous
record fields (17:10), physics never on the eval stream (19:10).

Consequences:

- The tracker's behavioral stasis proof IS the mover/static classifier;
  no cheaper engine truth exists at the hooked points. Stage B's
  publish gate stays as built -- and it worked this flight: ~2,570
  eligible published per frame, movers excluded, no stand-down after
  scene load, bounds part A/B fired 19:03:01 (decode offline, pending).
- The next engine-truth hook point is the +0x170 updater itself
  (FUN_14433DB20 in the 21:58 transform chain): decompile the physics
  job bodies (0x432B2A0 / 0x42DF530 / 0x42DF550) offline and find where
  dynamics actually advance. Serves perf L4 (physics decimation) too.
- L1's brackets-only numbers are NOT in this log: jobs[] lands in the
  classification JSON at dump time and no eye burst was taken. The L1
  remeasure still owes one burst next flight.

## 2026-09-20 (19:11: physics jobs decompiled -- physics WRITES nodes, the render side READS; the dirty queue is the surviving truth)

Offline, no flight. Tooling: analysis/run_ghidra_decomp.bat (headless
Ghidra 12.1.3, EDAnalysis project) with DecompileTargets.java pointed at
the three physics bodies; outputs in analysis/decomp/ (decomp_432B2A0,
decomp_42DF530, decomp_42DF550).

**UpdatePhysicsObjectsJob (0x432B2A0).** Iterates the physics-object
array (count at job+8), gated on state==4 (obj+0x380); a virtual call
(+0x1F8) yields the body context. Inner loop over the body's elements
(*(ctx+0x290), stride 0x30, count +0x2A0): element+0x18 is the NODE,
+0x08 the physics-state matrix. It composes physics x parent-chain
(parent pointer at matrix+0x50), compares BIT-EXACT against the node's
current 4x4 at node+0x90..+0xCC, and WRITES ONLY ON CHANGE -- appending
the changed node to a dirty queue (job descriptor param_1[2], atomic
append counter param_1[3], flushed in batches of 0x20 by FUN_144899F50).
Node+8 flag bits 2/4 are maintained on element state flips.

**The updater closes the chain.** FUN_14433DB20 (decomp_433DB20.txt)
writes record+0x170 by composing the record's own matrix chain (matrix
pointer record+0x190, parent at matrix+0x50) -- the same compose shape
the physics job performs. So: physics writes NODE matrices on change;
the render traversal's updater composes the chain into record+0x170 for
EVERY visited record. That is exactly why flight 190122's attribution
was a symmetric null -- movers and statics are all evaluated by the
render side; physics never evaluates records at all.

**Job3/job4.** PrePhysicsAdvanceJob (0x42DF530) is a 13-byte adapter to
FUN_14431E860(*desc, desc[1]) -- its first instruction is the jump
CodeHook rightly refused; the real body (0x431E860) is not yet
decompiled. PrePhysicsAdvanceCurveJob (0x42DF550) is a time-gated
curve/animation advance over a stride-0x18 array (FUN_141B41550 per
live entry) -- a different subsystem, not render records.

**The discriminator that survives: the physics dirty-node queue.**
Nodes appended during a job-2 run = moved under physics this frame;
records join by node pointer (record+0x18). It is a REFUTER, not a
complete mover oracle: keyframed/curve-driven movers never appear
(job4's curves move objects without physics), but an eligible-static
record whose node lands in the queue is a false static caught with
engine truth -- exactly the veto's need. Instrument sketch (next build,
no new hook sites): at the job-2 bracket, snapshot the atomic count at
entry and exit and log the per-frame delta for one flight (queue
lifecycle -- consumer/reset cadence -- is unknown; counts first, node
capture second). The queue's consumer can also be found offline by
decompiling FUN_14431E860 and the job descriptor's other readers.

Perf cross-reference (L4): a sleeping dynamic costs one bit-exact
compare per element, no write; the compose cost concentrates in
state==4 bodies with live elements. The dirty queue's per-frame size
IS the physics-write volume L4 would decimate.

## 2026-09-20 (19:30: dirty-queue count probe landed -- counts only, no new hook sites)

The 19:11 instrument sketch, built exactly as scoped: no new hook sites.
The existing job-2 bracket reads the dirty queue atomic append counter
at entry and exit; the probe accumulates counts only. Node capture waits
for the lifecycle decode this flight provides.

- Hook (kinematic_eval_hook.cpp): readQueueCount() guarded-reads the
  counter pointer at descriptor +0x18 (decomp_432B2A0 param_1[3]) and
  the uint32 count behind it. The bracket takes the entry read BEFORE
  the timed region and the exit read AFTER it, so L1 keeps measuring
  the job body alone. Job 2 only; the pair goes to the process-lifetime
  kinematicEvalProbe global, not the observer pointer, so tracker-only
  flights (fix.engine_motion on, probe never attached) capture it too.
- Probe (kinematic_eval_probe): notePhysQueue(entry,exit) accumulates
  per-session runs / appended / max_delta / resets -- exit < entry is a
  mid-run drain or reset and adds only the post-reset residue -- plus
  the first 64 NON-TRIVIAL (entry,exit) pairs under the mutex. Zero-
  delta runs still count in runs/appended but are not sampled: a static
  scene would fill the cap with zeros and crowd out the append/reset
  pattern that decodes the lifecycle. Per-session like jobs[] --
  clearLocked deliberately does not touch any of it.
- JSON: new top-level kinematicEval key phys_queue = {runs, appended,
  max_delta, resets, samples[{entry,exit}]}. The exact-match gate fixture
  sets runs=3 appended=7 max_delta=5 resets=1 samples [(10,14),(20,21)];
  max_delta exceeding every kept sample delta proves the counter is
  independent of the sample cap. First build attempt dropped the
  kinematicEval closing brace -- caught by the classification pipeline
  offline reader before the json gate even ran; fixed and re-gated. The
  phys_queue addition itself is exactly what the new-chapter top-level
  key is for: summary/records/jobs stay byte-stable for the gate.
- Absence is distinguishable in the same JSON: jobs[2].calls > 0 with
  phys_queue.runs == 0 reads as the counter read failing (wrong offset
  or torn descriptor); jobs[2].calls == 0 is the job-2 stand-down.

Gates: kinematic_motion_test 87/0, kinematic_probe_test 32/0 (4 new),
kinematic json self-test passed, config contract 252/252, all gates
passed. Frontier d3d11 sha256 34f9195432f98190, install verified.

FLIGHT PROTOCOL (next flight): fix.engine_motion on; a settlement with
a visible mover (landing pad or the turret); >= 60 s; ONE eye burst --
any view, no debug toggle needed. The burst classification JSON carries
phys_queue (queue lifecycle: per-run append counts, reset cadence) AND
jobs[] (the brackets-only L1 remeasure flight 190122 owed). Reading it:
runs should track the frame count once job 2 is warm; appended/runs is
the per-frame physics write volume (perf L4); resets 0-or-rare with a
sawtooth-free sample tail means a steady consumer -- a large resets
share or sawtooth samples mark a drain boundary the node capture must
respect. appended == 0 with movers visibly moving refutes the queue as
the mover truth and kills the node-capture plan before it costs a build.

## 2026-09-20 (19:45: flight 193356 -- the dirty-queue lifecycle is DECODED; node capture unblocked)

Flight 193356 (log 19:33:56, burst classification_193539.json 19:35:39,
~103 s, 10758 frames, tracker live, settlement scale 2819 tracked /
2573 eligible / 37 movers in-frame by 19:35:36). Build stamp reads
v0.17.0-166-g71cbed3f-dirty because the install preceded the commit;
installed d3d11 sha256 34f9195432f98190 == the 227cd41 content, so
--expect-build HEAD mismatches on the label only (same situation as
the 17:05 flight note). DLL re-hashed unchanged post-flight.

phys_queue: runs 8124, appended 15174, max_delta 133, resets 0, 64/64
non-trivial samples kept (cap fills in the opening seconds -- the
uncapped counters are what answer the cadence question, exactly the
split the design intended).

**Lifecycle, decoded.** The append counter is CUMULATIVE across job-2
runs and is drained entirely BETWEEN runs by an external consumer:
sampled entries are exactly 0 in 33/64 pairs, small residues (1-28)
otherwise, and exits climb across consecutive runs (25, 66, 71, 91,
105, 122) before dropping between runs. Job 2 never observes the
counter go backward: resets 0 in 8124 runs, zero in-run drops. So a
node capture at job-2 exit reads queue[entry..exit) = this run's
appended nodes, intact, synchronously inside the bracket. The capture
the 19:11 entry deferred is now a buildable instrument: at job-2 exit,
walk queue-base[entry..exit) (param_1[2]) reading node pointers, join
to records by record+0x18. ~12.7 nodes/run steady, 133 peak -- a 256-
slot cap with an overflow counter covers it.

**Write volume (perf L4).** 15174 appends over 1196 working runs:
~12.7 node writes per working run, peak 133 (busy frame). Working
runs concentrate at settlement scale (idle-array runs early-out); a
parked-at-settlement window would tighten the per-frame rate, but the
order is set: tens of node writes per frame, not hundreds.

**runs 8124 vs jobs[2].calls 1196 -- explained, not a defect.**
notePhysQueue counts every bracket with a successful pair of reads;
the jobs[] timing commit gates on elapsed>0, which drops sub-QPC-tick
runs. Job 2 is dispatched ~every frame (~79 Hz here) but early-outs
when the physics array is idle -- most of this flight pre-settlement
-- and those bodies finish inside one QPC tick. The 1196 timed runs
are the working ones (mean 7.6 us). Consequence for readers:
jobs[2].calls undercounts DISPATCHES by design; phys_queue.runs is
the dispatch count. No code change warranted -- noted here so the
next reader does not burn a session on the 6.8x gap.

jobs[] this flight (brackets-only, tracker-only path; the L1 remeasure
190122 owed): UpdateRenderDataJob 32656 calls, mean 55.1 us, max 25.4
ms (scene-load hitch), total 1.799 s; RenderDataBatch 5772 calls,
mean 176.0 us, max 3.34 ms, total 1.016 s; UpdatePhysicsObjectsJob
1196 calls, mean 7.6 us; Unnamed_BA0 2395 x 0.5 us;
PrePhysicsAdvanceJob 0 (unhooked thunk, known); CurveJob 0 (hooked,
never fired -- consistent with 17:50). Full L1 read-out in the perf
doc same-time entry.

## 2026-09-20 (20:50: the node-capture refuter LANDED)

Sanctioned 20:27, built on the 19:45 lifecycle decode. The queue's
node-pointer array base is descriptor +0x10 (param_1[2]) -- confirmed in
decomp_432B2A0 lines 257-268: the batch flush is
FUN_144899f50(param_1[2] + idx*8, batch, count*8), a memcpy of 8-byte
node pointers at the CAS-claimed index. New readQueueNodes()
(kinematic_eval_hook.cpp) walks queue[entry..exit) at job-2 exit, inside
the bracket but outside the timed region, riding the same exit read as
notePhysQueue. Clamps: newest min(delta, 256) entries kept, the clamped
excess counts as overflow; a reset (exit < entry) captures only the
post-reset residue [0, exit) (the pre-reset slice was drained mid-run;
phys_queue.resets flags it); an exit over 0x100000 or a read fault drops
the run's capture under __try, never the flight.

Probe side (kinematic_eval_probe.{h,cpp}): notePhysNodes() keeps the
last kPhysNodeCap = 256 pointers per session, oldest-first ring, plus
total and overflow counters -- per-session like phys_queue (never gated
on active(), not cleared by clearLocked), so tracker-only flights
harvest it too. Serialized as kinematicEval.phys_nodes {total, overflow,
nodes[]}. Invariant, asserted by the new casePhysNodes gate:
total == ring + overflow. 256 covers the observed burst shape (steady
~12.7/run, peak 133 on 193356); the tail suffices for the join because
movers re-append EVERY frame -- at dump time the ring holds every
recently-active mover, while statics never append at all.

End-to-end trace (what "dead" looks like, per the build-discipline
rule): if the capture never fires, phys_nodes reads
{"total":0,"overflow":0,"nodes":[]} while phys_queue.appended > 0 --
distinct from a healthy flight, where total ~= appended (it counts the
same walked nodes pre-clamp) and overflow << total.

Gates: kinematic_probe_test 35/0 (casePhysNodes drives over-cap clamp,
ring drop-oldest and the invariant), kinematic_json_test round-trip
with the fixture seeded total 519 == 3 + 516, config contract 253/253.
Full build green (build log build\build_physnodes.log). Installed to
frontier, d3d11 sha256 aa88369d7a608415, install verified. Built and
installed pre-commit (the 19:45 pattern): --expect-build HEAD will
mismatch on the label; the content hash above is the truth.

Flight protocol: one eye burst at a settlement with movers in-frame (a
landing ship, tracking turret or drone), movers view not needed. The
burst's kinematicEval JSON now carries phys_nodes. Offline join: ring
nodes intersect records[].node -- any overlap proves physics touches
tracked records; a mover record whose node never appears in the tail is
the physics-inert case the veto needs. Cross-check phys_nodes.total
against phys_queue.appended before trusting the join.

## 2026-09-20 (21:05: flight 205106 -- node capture healthy, join a COMPLETE NULL; node-type premise in doubt)

Flight 205106 (log 20:51:06, burst 20:54:07, ~3 min at a settlement;
tracker at the burst: tracked 3284, movers 400 in-frame / 623 total,
1.34M pose changes). Build stamp v0.17.0-176-gd015f7b0-dirty == the
d7108cf content (installed pre-commit); DLL re-hashed unchanged
post-flight: sha256 aa88369d7a608415.

**Instrument health, end to end.** phys_queue: runs 94445, appended
178650, max_delta 122, resets 0. phys_nodes: total 178650 == appended
EXACTLY -- the capture saw every append through every job-2 dispatch --
overflow 178394, ring 256/256, and the invariant total == ring +
overflow holds. The 20:50 dead-capture discriminator (total 0 with
appended > 0) does not fire. The instrument is proven; what follows is
the measurement, not a defect.

**The join is a complete null.** The ring holds 11 DISTINCT nodes in a
repeating pattern (4 x32, 1 x20, 6 x18 -- a small always-dirty set,
arenas 0x1effa89*/0x1eb33f9*/0x1eb3584*). Overlap with the capture's
records: 0 of 2971 records, 0 of 625 distinct record+0x18 values, 0 of
408 xf-changed records, 0 of 173 distinct xf-mover nodes. The 11 ring
nodes appear nowhere else either (riglinks, ownership, pose_events all
negative). Movers were in-frame and changing at the dump frame.

**But the join premise is in doubt (offline type check, same night;
corrected 21:25 -- see the same-date entry below).** The composer
FUN_14433DB20 dereferences record+0x18 ONLY as a +8 non-null gate --
the count at +0x48 / array at +0x50 of 0x58-stride entries belongs to
record+0x290's pose ctx (this entry's first writing mis-attributed it
to record+0x18). Job 2's dirty nodes instead carry a flag word at +8
(bits 2/4 toggled), a guard pointer at +0x70, and a 4x4 matrix written
at +0x90..0xCC. Two different layouts. Pointer equality between the
queue family and record+0x18 may be a category error; if so, the null
join is expected by construction and says nothing about movers. NOT
ruled out either: a big struct could host both layouts, and the
composer only null-checks node+8 (a flags value of 2/4/6 would pass).
Genuinely ambiguous -- resolved toward family mismatch in the 21:25
entry.

**Coverage gaps ruled out / noted.** Job 4 (CurveJob) fired 0 times, as
on 17:50 -- not the path at settlement scale. Job 3
(PrePhysicsAdvanceJob) is UNHOOKED; a same-shaped append there would be
invisible to this capture. jobs[2].calls 1196 again (the elapsed>0
gate); phys_queue.runs 94445 (~522/s) vs 8124 (~79/s) on 193356 -- the
job-2 dispatch rate scales with scene activity; noted, unexplained.

**Verdict.** The refuter instrument works. Its first flight says the
physics dirty queue, as joined by record+0x18, touches nothing the
tracker sees -- either movers are physics-inert through job 2 (the
veto-relevant case) or the join pairs the wrong node families. The
type question must settle before the veto leans on this signal.

**Next discriminators, both offline, no flight needed.** (a) Decompile
job 3's body (0x42DF530, run IS the body) for the same append shape
(param_1[3] CAS + param_1[2] array) -- closes the coverage gap.
(b) Resolve the node-type question: find who writes record+0x18 (a
FindStores scan) and what the 0x58-stride entries carry (a node
pointer? the 88-B xf?). The 11 always-dirty ring nodes are the foot in
the door -- any engine structure referencing one names the family
(player ship / head rig suspected).

## 2026-09-20 (21:25: discriminators resolved -- coverage CLOSED, node families distinct; dirty-queue route closed)

Both 21:05 discriminators ran offline (Ghidra, no flight). This entry
also corrects a mis-attribution in the 21:05 entry: the +0x48 count /
+0x50 array of 0x58-stride entries belongs to record+0x290's pose ctx,
not record+0x18 (the composer reads record+0x18 only as a +8 non-null
gate). Fixed in place there and in the settlement cross-entry.

**Discriminator (a), coverage -- CLOSED, no gap.** Job 3's hooked thunk
0x42DF530 is a 13-byte forward to the real body FUN_14431E860
(decomp_431E860.txt): it iterates a stride-0x14 float array
(+0xC0, count +0xC8), detects per-entry changes into a local bitmap,
then walks RENDER RECORDS directly (base +0x280, stride 0x2F0, count
+0x298) calling FUN_14432CCC0 per record gated by the bitmap. No CAS
append, no node-pointer queue. Job 4's body 0x42DF550 is a 287-byte
curve evaluator (FUN_141B41550), no queue, and fired 0 times on both
flights. The job-2 queue is the ONLY dirty-node queue of its shape;
the capture's coverage of that mechanism is complete. The null join is
not a coverage artifact.

**Discriminator (b), node origin -- the families are distinct by
construction.** The render-record ctor FUN_1442BE1F0 creates
record+0x18 at construction via FUN_140869EC0(slot, source model
record, DAT_145F02E58) (decomp_42BE1F0.txt lines 46-48), the same
helper pattern that builds the +0x10/+0x20/+0x28 siblings. An xref scan
on DAT_145F02E58 (FindDataXrefs.java; data_xrefs.txt) returns 89 hits
-- it is a ubiquitous engine global, not a per-type id, so it cannot
discriminate families; the record+0x18 object is nonetheless a
render-side sub-object built per record (factory sharing by model
explains the ~5-records-per-node pattern). Job 2's dirty nodes are
reached the other way: physics object (state +0x380 == 4) -> +0x58
vtable slot 0x1F8 -> container (array +0x290, count +0x2A0) ->
stride-0x30 entries -> node pointer at entry+0x18, whose +0x90..0xCC
4x4 the job bit-compare-rewrites (decomp_432B2A0 lines 106-146). Two
construction paths, two layouts, and 178k appends against 625 record
nodes producing zero overlap: the queue family and record+0x18 do not
share pointers.

**Consequence for the arc.** The dirty-queue join by record+0x18 is
dead by family mismatch -- NOT evidence of physics-inert movers. That
closes the fourth engine-truth route for the mover/static question
(vtable/flag clustering null 17:10, job-attribution symmetric null
19:10, +0x268 retracted at 21:23 review, dirty-queue family null here).
The veto's false-static protection therefore leans on the tracker's own
pose evidence (mover_samples / xf changes), which is flight-proven --
that is the design signal, and it is already in hand. Open curiosity,
not a blocker: the identity of the 11 always-dirty nodes (nothing in
the capture references them; the gfx log never prints them).

**Also corrected from the 21:05 entry:** its worry that job 3 could
hide mover appends is disposed of by the decompile above. The 21:05
runs-vs-calls note stands (94445 runs vs jobs[2].calls 1196 -- the
elapsed>0 gate).

### 2026-09-23 -- Resumed offline: no engine velocity buffer, the previous-frame block is a change detector, movers are rig records; three designs

Resumed offline after the 2026-09-20 21:25 close (four engine-truth routes to a
mover/static flag, 0-for-4). An opus research agent ran a read-only pass today and
reframed the question -- not "is this record a mover" but "where can a
previous-frame pose be read at all." Findings below are quoted from that pass;
scratch scripts and decompiles are named under Sources at the foot.

**No velocity buffer in the VR path (confidence high for geometry).** All 413 pixel
shaders in edvr_logs\shaders (one two-minute dump, 2026-09-06, covering 75-86% of
eye-sized draws in the Sept 15-18 settlement censuses) were parsed: none writes a
two-channel target; 134 write several targets and every one writes all four
channels; the four-target G-buffer's last target is emissive or zero (the dominant
family ps_CB9F297EFF264251 writes `mov o3.xyzw, l(0,0,0,0)`, disasm line 244; across
the 71 four-target shaders 38 computed, 30 zero, 3 constant, the computed ones
emissive-shaped or in shaders with no position input). The three pool vertex-shader
families (EB52, 5B4D, BBE5, 87% of pool draws) output one SV_POSITION and read one
pose (t33 +0/+16), one bone palette (t38), the clip matrix cb1[270..273] and the
origin cb1[275]; no semantic in the 637 dumped shaders is named prev, velocity,
motion or history. The only motion output anywhere is ps_94106BE6A6ADBF21, a
ray-marched volume writing o2.xy = uv_prev - uv_cur from cb1[192..195] (camera-only,
in no census). The game's Custom.4.4.fxcfg has BlurEnabled false (line 3), as do the
six other saved configs; blur-on shader variants were never dumped. This closes the
gap per-object-motion.md:1199-1216 left open (the census records only render-target
slot 0).

ruled out: an engine velocity buffer in the VR path, because none of 413 dumped
pixel shaders writes a two-channel target and BlurEnabled is false (2026-09-23).

**The previous-frame block is a change detector, not a previous transform
(confidence high on what and when).** +0xF0..0x12F of a rig record is its world
transform (three padded rotation rows, translation at +0x120; flight 125207 showed
+0x120 == +0x170 in 16 of 16 records), written by FUN_14432CCC0
(decomp_432CCC0:442-468) from FUN_14431E860, the PrePhysicsAdvanceJob body (:79),
only when a collection channel changed or collection+0x98 is set (:44-73). The
updater FUN_14433DB20 writes +0x170..0x178 (lines 196-198), the packed quaternion
+0x17C (273), the sphere centre +0x240 conditionally (302-305), the pose context via
FUN_1442B6410 (355) and the flag +0x2E8 (372, 514); at its TAIL it copies +0xF0 to
+0x1C0 (506-513), after its change test (+0x120 against +0x1F0 plus rotation vectors
from FUN_1405E0A20, 398-424), the notify (427) and the render handoff (436-491);
both early returns (363-374) skip the copy. So +0x1C0 holds whatever was last
submitted and equals +0xF0 once the updater has run: at draw time it is not last
frame's pose. +0x170/+0x17C have no previous copy anywhere in the 0x2F0-byte record;
their old values exist only at updater entry. The updater runs every frame for every
record in the LOD/distance view mask (4312040:137-153), no dirty gate.

ruled out: record+0x1C0..0x1F8 as a previous-frame transform at draw time, because
FUN_14433DB20 copies +0xF0 into it at its tail after the render handoff: a change
detector that equals +0xF0 once the updater has run (2026-09-23).

**Movers are rig records (confidence medium).** Stations and ships are drawn from
the t33 pool (per-object-motion.md:2871, 3170-3172). New, offline: at the same frame
a moving t33 record's position equals a tracked record's +0x170 to under 1 mm for
235 of 415 (landing-ship run 160734), 34 of 151 (turret run 163619), 44 of 252
(flight 205106's burst, capture 205407); against neighbouring frames only 3-5 match,
so pool and record agree within the frame; over 97% of pool movers lie within 2 m of
a tracked mover against 23-29% of a static control (the offset is parts sitting away
from the record origin). They are NOT the draw-item builder's records: on capture
165433 builder parts claim 85.4% of static t33 records but 2 of 82 movers and none
of 184 skinned records; the likely emitter is FUN_144312E00, the type-2 item path,
which copies +0x170/+0x17C into a 0x150-byte batch item (decomp_4312E00:66-76).
Unknown: ships in space, stations, SRVs, doors, lifts (no tracker capture exists in
space or at a station).

**At draw time.** No pool-family constant buffer carries a per-instance or previous
matrix (the pool's non-pose words, bytes 28-55 and 288, are packed material words).
Pool slots are not identities: on a parked run 5.7k-13k of 12.3k slots jump 14-1,100
m between frames as contents repack. Same-frame joins that exist: draw -> t33 exact
through the instance range (occlusion design doc section 9); t33 -> record exact for
single-part movers by pose; multi-part movers need the per-part local transforms in
the record's pose context (0x58-stride groups at *(record+0x290)+0x50), captured
today for builder records only. Cost basis: EDVR's per-draw code 1.19 ms for ~18k
draws; an extra draw with a shader and render-target switch 0.3-0.4 us on the render
thread and 0.1-0.2 us on the calling thread (terrain-motion-dispatch-cost:280-301);
per-draw copies cost 2 us plus a sync, avoid; today's tier-2 rigid path samples pool
pairs every 8th frame (per-object-motion.md:3203-3209).

**Sizes** (offline, consecutive frames; "mover" = the record's pose bytes changed;
bone-palette limb motion not counted, so lower bounds). Parked views (152632,
012514, 165433): pool eye draws 16.8k-18.3k, mover draws 132-322 (0.8-1.8%), moving
t33 records 32-82. Captures 184120 / 201515: 13.7k / 18.1k, movers 22-307 / 266-666,
records 4-241. Turret (163619): 13.4k, 292-313 (2.2%), 140-151. Flight 205106 burst
(205407) / capture 163645: 18.3k / 17.2k, 746-908 (4.1-5.3%), 242-333. Landing ship
(160734): 18.1k, 845-879 (4.7-4.9%), 416-428. Station approach: no captures since
09-19; from the docs, at 10 km the station alone is 1,334 pool draws (2,966
instances) and 1.7% of pixels, all rotating (per-object-motion.md:2834, 2851-2856);
near it and in the slot it fills the frame.

**Three designs the evidence permits.** A, frame-to-frame pool diff (no engine
hooks): t33 headers this frame against last, matched by content; ownership exact by
re-issuing only mover draws through an EDVR vertex shader (current header plus
matched previous) into an RG16F target, depth-equal against the game's depth; same
frame; cost ~0.2-0.3 ms hashing (worker-able) + 0.1-0.2 ms mover bit + <= 0.36 ms
re-issue at settlements, ~0.5 ms+ at a station; look-alike props moving together can
swap identities. B, engine-record truth: +0x170/+0x17C each frame with the previous
pose from the existing tracker (engine identity, so repacking and look-alikes cannot
confuse it), joined to t33 by exact pose in the same frame (multi-part movers need
the per-part local transforms or a sphere-containment join); ownership by the same
re-issue; same frame; cost the job-thread hooks already running plus a small match
table. C, a fifth G-buffer target written by patched vertex/pixel shaders of the
three pool families from a previous-pose table built by A or B: exact for every pool
pixel and cheapest at stations, at the price of shader-bytecode patching per family
redone after every game update, declined before. Skinned lead, unverified: every
capture alternates two bone-palette buffers by frame parity (bones0_* even, bones1_*
odd), so the previous palette may still be on the GPU.

**What decides between A and B** (one flight, no new code): a tracker-on eye run at
a station approach with ship traffic, read with the scripts named under Sources
below. If the station's and ships' pool movers match tracked records exactly in the
same frame as settlement movers do, B covers every case; if stations produce no
tracked records, A (or C at stations) is the general design.

**Sources.** Today's decompiles are in analysis\decomp\ (gitignored, alongside the
arc's existing decompiles). Scripts and capture data are in analysis\motion\
(gitignored, new today): dxbc_sig.py, mrt_writes.txt, o3_detail.txt,
mover_draws3.py, mover_join.py, mover_exact.py, mover_vs_tracker.py,
parts_165433.csv.

### 2026-09-23, later -- The requirement: generalized, no estimation even as a fallback; the blur check

**Sean's requirement (2026-09-23).** A generalized solution for motion vectors, with no
guesswork estimation anywhere -- not even as a fallback.

**The overseer's answer: possible, with three sources and nothing else.** (a)
Camera-only through depth for everything static relative to the world (settlement
statics, terrain, station structure, the cockpit): exact by construction. (b) Exact
object motion from engine data, written by the game's own draws: the truth is the
kinematic record's pose each frame, with the previous pose held by the tracker EDVR
already runs, keyed by the engine's record; the delta reaches each draw as a per-slot
table, and the pool families' shaders are substituted -- hash-keyed like every other
keyed fix -- to write a velocity target in the same pass: design C fed by design B, so
no re-issued draws, exact ownership including occlusion, and the same cost at a station
as at a settlement; articulated parts come from the record's per-part transforms,
skinned characters from the previous bone palette (the engine appears to keep it
resident by frame parity, unverified), and procedurally animated geometry by evaluating
the same animation at the previous time. (c) A declared "no history" mask for anything
not joined or not coherent (particles): EDVR already drives DLSS's bias-current-colour
mask from the UI depth classifier (src\d3d11\dlaa.cpp:576, `ep.pInBiasCurrentColorMask =
reactive`) and FSR's reactive input (fsr3_engine.cpp:834), so the fallback is never an
invented vector. The rigid fit and the occupancy grid retire once movers are covered.

Still to verify before code: stations and ships in space as rig records; the previous
bone palette's residency; the particle families' inputs; the substitution cost for the
three pool families and the skinned ones.

**The blur check (Sean's question; the overseer's answer: yes, first).** The shader dump
that ruled out a velocity buffer was taken with motion blur off, and blur-on variants
were never dumped. If the engine's blur is per-object, its blur-on vertex shaders carry
a previous pose and its pixel shaders a two-channel target: the engine would then
already render the buffer above for every class, and EDVR would enable blur, read that
target, and drop the blur's composite draw (the draw hook already skips draws). If the
blur is camera-only, the shader set shows it and the substitution design stands.

**Next flight, combined, no code.** Motion blur ON in the game's graphics options;
`advanced.glare_shader_dump = 1` (needs a restart, writes files during loading); the
tracker on; one eye-run capture during a station approach with ship traffic, then a
settlement with a landing ship. Read: the blur-on shaders through
analysis\motion\dxbc_sig.py for two-channel targets and previous-pose SRVs; the
station's movers through mover_exact.py / mover_vs_tracker.py against tracked records.
Awaiting Sean's go.

### 2026-09-23, evening -- Blur ON changes nothing; the landing ship's movers join the tracker 64/64; design A measured wrong

Sean flew the first half of the combined flight named in the entry above: build fd25af9, a
ship landing at a settlement, motion blur ON in the game's graphics options,
`advanced.glare_shader_dump = 1`, the tracker on (2026-09-23 04:34 local). An opus
read-only pass analysed the capture -- gfx log edvr_gfx_20260923_043410.log, eye run
043720, 19 pool frames, depth, eye images, ledger and drawstate -- with artefacts under
the main checkout's gitignored analysis\motion\blur_on\ and the scripts in
analysis\motion\ (mover_draws.py is now a shared import for the mover_*.py scripts).

**Blur ON renders no per-object velocity (confidence high).** The 250 new shader files are
160 compute (compute dumping only began 2026-09-07, so all count as "new"), 52 pixel, 38
vertex; of the 45 new VS/PS hashes bound in this eye run, 39 are also bound in blur-off
runs from 09-20 to 09-23. Every one of the 99 targets the 52 new PS write is xyzw, none
two-channel: the four-target ones write emissive or a constant to o3 (`mul o3.xyz,
r0.xyzx, cb1[90].yyyy` in F1670378; `mov o3.xyzw, l(0,0,0,0)` in 669CC896), the two-target
glass ones (29D8624F, D0B9213C, B9F7DA33) a transmittance tint. All 38 new VS have one
SV_POSITION; the only clip-like extra output, CURRENTCLIPPOSITION in vs_65503A08168D246C,
is the same value (`mov [precise] o1.xyzw, r0.xyzw`) feeding a depth fade
(ps_D016068F24E2A565). The extra SRV slots are bounds (t36) and a mask texture (t0), not
poses. Across all 89 pool-type VS in the dump, t33 is read only at 0, 4, 12, 16, 320, 324,
332 and the material blocks at 32+32k; nothing reads bytes 288-319; no VS reads
cb1[270..273] twice or cb1[192..195] at all.

ruled out: an engine velocity buffer with motion blur ON, because blur-on run 043720 binds
the same pool shaders, writes no two-channel target, keeps the second pose block a
same-frame copy, and adds no pass before the tone-map (2026-09-23).

**The pool families are unchanged.** Frame 2 binds EB5234DB6ADB491D 4,499 draws, 5B4D/4375
2,521, BBE5/DB3E 922 (7,942 of 10,083 pool rows), the same hashes as blur off; t33 stride
336 (7,168 records), t38 stride 48; the record's second pose block (bytes 292-303 and
312-319) equals the first byte for byte on 100% of 7,168 records in frames 3-20, including
every record that moved since the previous frame (147-195 per frame), same as blur-off run
165433; cb1[192..195] is zero in the scene pass in both eyes (and in blur-off run 012514);
the pool buffer is renamed each frame, so no previous t33 is resident.

**No blur pass.** The eye-sized full-screen sequence is identical to blur-off runs 165433
and 012514 (15 draws, ending per eye with the tone-map 2D78DC3F/99C21CEB then a
2-instruction copy); the tone-map's colour input is the lighting composite's target
(resource @753, 1995x1970, fmt 26); the 13 pairs unique to this ledger are cockpit
forward, glass and holo materials. Caveats: the census truncated inside frame 0
(16,384-line ring), so late offscreen or compute passes are unseen; the capture was at
landing speed, and a speed-gated camera blur compiled on first use would not appear.

**The landing ship.** 9,488-9,491 pool eye draws per frame, 138-198 moving records,
402-536 mover draws (4.2-5.6%); the ship is a rigid group of 33-48 records about 0.54 km
out moving about (0.4, -1.8, -0.6) m per frame; its 184-196 draws per frame bind no
blur-on variant (DE545DC8/E46E3E48, AACFDCF2/CF534B32, 66DE2CAD/864F1F94, EB52/CB9F,
61AE8EB0/FC43E427, 5B4D/4375: all from the 09-06 dump, all reading t33 and t38 only with
cb1[270..273] and cb1[275]). mover_exact: ledger frame f maps to tracker frame 16867 + f;
at f = 5, 10 and 16, 64 drawn movers match a tracked record's +0x170 and quaternion
EXACTLY, including all 56 fast (ship) movers; any other frame offset matches at most 1;
the tracker's own delta equals the true pool motion for all 56 ship movers, while the
pool-side content pairing (design A) picked a look-alike sibling part for 11-23 of the 56
per frame -- design A's failure, measured.

ruled out: design A, pool content-pairing as the previous-pose source, because on the
landing ship it mis-paired 11-23 of 56 fast movers per frame while the engine-record join
matched 64/64 exactly (2026-09-23).

**What EDVR still needs.** The current pose from t33 at draw time, the previous pose from
the tracker keyed by the engine record, and its own previous view-projection for the
camera term. An implementation lead: bytes 288-319 of each t33 record (a float, then
position, base, scale and quaternion) are a same-frame copy no dumped shader reads, so
EDVR could write the previous pose there at the pool's Unmap and a substituted VS would
read t33 +292/+312 with no extra SRV; whether the engine reads that block on the CPU side
is unknown. The ship adds four families with the pool layout to C's substitution set
(DE54, AACF, 66DE, 61AE) beside the three pool families, blur setting irrelevant.

### 2026-09-23 -- Phase 1 built: engine-record velocity for movers

Built on the worktree branch (not merged, not flown), gate green with symbols.
`fix.engine_motion = on` now means this path; off is unchanged. Exact per-pixel motion
for rig-record movers from the engine's own records, no estimate anywhere: the only
fallback is no history. Three parts.

**1. Emit (job threads): the previous pose travels with the record, so no slot map is
needed.** The record -> slot question dissolves at the source. FUN_144312E00
(param_1 = the 0x2F0-byte rig record, param_2 = the owner) builds each pool record on its
stack at RSP+0x60 via the initializer FUN_144c835d0, copies rig+0x170/+0x178 (position)
and +0x17C/+0x180 (packed quaternion) into BOTH pose blocks (decomp_4312E00:67-76; the
second block is record +0x124/+0x12C/+0x138/+0x13C, RBP+0x84..+0x9C in
funasm_4312E00), then per LOD with a mask appends the whole 0x150 bytes to the tail node
of `FUN_143696fa0(param_2 + 0x260, param_1 + 0x250)` (:179) and counts it
(`plVar10[3] + 1`, `*(param_2 + 0x2a4) + 1`, :248-250; asm 0x144313185-0x14431319B:
`mov rax,[rdx+18h]; mov [rdx+rax*8+0AA0h],r15; inc qword [rdx+18h]; inc dword
[r13+2A4h]`). The copier 0x4C81BE0 later copies whole records from node+0x20+i*0x150 into
the mapped pool (settlement-flicker doc, 2026-09-19). A post-forward bracket on
FUN_144312E00 -- the direct-producer relay kinematic_eval_hook.cpp already installs, with
a new emit observer -- takes k = the +0x2A4 increase (k <= 7, one entry per call), looks
the entry up again (FUN_143696FA0 is a pure read on a hit, no lock; the owner is per
collection with one writer, the job running the call), takes the tail node's last k
records (and the full previous node's last k-n when they span), checks each one's pose
words against the rig record's +0x170/+0x17C bit for bit (the disagreement gate: a
mismatch is counted and left unwritten), and writes the record's PREVIOUS engine pose
into bytes 292-303 and 312-319 plus a marker at byte 288. Build-keyed: PE 332841, the
lookup's first 32 bytes and the append sequence above; a mismatch stands the emit down
alone (every record then reads as not a rig record: the camera term).

The previous pose comes from a per-record table in engine_velocity_emit.h, filled at the
same bracket: keyed by the record pointer with record+0x18 as the reuse discriminator
(the tracker's rule), stamped with the present-frame clock. It holds exactly what the
previous frame's upload carried for that record, which is the tracker's +0x170/+0x17C
history captured at the moment the engine copies it. First seen, a gap or a reused
pointer: the current pose (delta zero, the spec's rule). Two emissions of one record under
one clock tick with different poses: masked (two frames' data, ambiguous). Table full:
masked. Stale entries (two frames unseen) are reclaimed.

CPU readers of bytes 288-319 (a read-only research pass today, decompiles and asm scans;
files decomp_4C835D0, 3696FA0, 4C81BE0, 4C7BD80..4C7D8A0, pose_block_*.txt under
analysis\decomp): none but whole-record copies (the producers, the merge FUN_14434e200 /
FUN_14434e740, the copier). The bridge 0x434D7A0, 0x4C837B0 and 0x4C7EF70 pass pointers
or read masks; the eight instance-index emitters read only word28; no readback of the
pool. Static, not a runtime capture. Correction to the 2026-09-23 first entry: byte 288
is not a packed material word -- no producer writes it at all (uninitialised stack in
FUN_144312E00, 0x42B4130, the builder's inline producer, FUN_14369C9C0 and
FUN_14434D470). So the marker checks itself: tag XOR a hash of both pose blocks
(joined 0x7FC0ED01, masked 0x7FC0ED02), recomputed on the GPU.

**2. Draw (render thread): the game's own draws write which record owns each pixel.**
The pool families are hash-keyed (engine_velocity.cpp kFamilies: vs EB52 -> ps
CB9F/9ABF/3434, 5B4D -> 4375, BBE5 -> DB3E, DE54 -> E46E, AACF -> CF53, 66DE -> 864F,
61AE -> FC43; any other pairing is left stock and named in the log). Their pixel shaders
are patched (dxbc_engine_velocity.h) to add one float2 export at MRT6: x = the t33 slot
(the identity the VS already exports, `bfi o0.x, l(31), l(0), v0.x, flag` -> masked
0x7fffffff), y = SV_Position.z. The UV-only family (5B4D/4375) exports no slot, so its
VS gains `mov o2.x, v0.x` under a new EDVRPOOLSLOT output and its PS reads that. No other
instruction changes: position math, depth bias and G-buffer exports stay byte-for-byte.
Substitution happens when the game rebinds (the binding shadow's generations), not per
draw: one PSSetShader per game PS change, MRT6 added once per pass binding, the per-draw
cost four compares. At the first family draw of each eye-frame the slot target is
cleared to (-1, 0) and the pool (VS t33, view at element 0) and the scene constants (VS
b1) are copied on the GPU; a later write to either source while that eye's pass is
open, or a later pass drawing from other ones, invalidates the eye-frame (counted).

**3. Compose (temporal pass): exact motion where the slot's depth is the scene's.**
ENGINE_MOTION_HLSL (temporal_shader_source.h, the block the rig also compiles) and
enginePixel: MRT6 depth must equal the scene depth bit for bit (a later draw covering a
slot fails it); the record's marker decides joined / masked / not a rig record. Joined:
the surface point under the pixel from this frame's clip rows (x, y, w columns, w =
EN[273].z / depth), carried by the record's own motion (the exact adjugate inverse of the
VS's turn() for the current pose -- quantised quaternions are not unit -- then the
previous pose), projected by last frame's rows; holoJitter as meshPixel. A record that did
not move reduces to the camera term exactly. Masked: MV gets the history-invalidate
sentinel (what modern DLSS presets honour) and the bias mask 1 (preset F, FSR with its
reactive key, own TAA returns no history). Neither: the camera term. The engine pixel is
taken LAST in `mv`, over the body/ship paths, mesh and the owner promotion (decision
path 11); in `main` before the mesh block.

**Mechanism, and why this one.** The spec's sketch was a VS computing the previous clip
position per vertex. Rejected for phase 1: the seven VS families place the pose
transform differently (skinning loops, and 5B4D/BBE5 apply a cb2 depth bias to o.z), so a
per-vertex previous position means family-specific surgery on the position math and an
invariance risk on SV_POSITION. Re-issue was the other exact option (0.3-0.4 us render
thread per mover draw). Chosen: a generic, family-independent PS patch writing ownership
(slot + depth), with the motion computed from engine poses in the compose, where the
camera handling (jitter, rows) already lives. Exact under the same definition; the one
caveat: the surface point comes from the scene depth, so 5B4D/BBE5 draws reconstruct
at their engine-biased depth exactly as the camera term already does. Rig evidence
(tools\engine_velocity_test, in the gate): three synthetic families (DATAID.y,
FACEINVARIANT.x + SV_Position, UV-only) patch, disassemble, reflect and DRAW on WARP --
MRT6 holds the exact slot and depth bits, an unpatched occluder leaves stale slots the
depth test rejects; the emit bracket against a fake owner/entry/nodes laid out as 332841;
the compose's arithmetic from the header's own text against a double reference (worst
previous-NDC error 2.7e-7 over a still record, a mover 540 m out, a scaled part and a
quantised 90-degree turn; markers, behind-camera, foreign projection). Local only
(`--corpus <edvr_logs>`): all nine real (VS, PS) pairs derive, patch, reflect and create.

**Keys.** `fix.engine_motion = on` (this path, needs temporal_aa; auto still reserved).
`advanced.mesh_motion` and the new `advanced.temporal_aa_estimated_objects` (tier 2's
body/ship paths and occupancy grid; not the retired fix.temporal_aa_objects) default off
while it is on, on for an A/B; engine pixels override them either way. New debug view
`advanced.temporal_aa_debug = motion_source`: green joined, red masked, blue a pool
surface that is not a rig record, yellow a stale slot. `fix.engine_motion_veto` is
untouched (still the stage-B compose veto; it acts only where no engine pixel is taken).
Contract 260 -> 261 keys. `advanced.temporal_aa_static_surfaces` also replaces these pixel
shaders; with both on, it declines on substituted draws (noted once).

**Log lines (every 30 s; every zero printed).** `engine motion: emit (...)` -- calls,
appended, pool records joined (with motion) / masked, first seen / gap / reused pointer,
same-frame changes, table full, **pose disagreements (must be 0)**, locate failures,
read/write faults, bracket us/call and ms/frame on the job threads. `engine motion:
movers joined N records/frame ... against the tracker's M` -- plus eye-frames bound,
invalidations by cause, views asked/given/refused by cause (another frame = a clock-order
problem), and the draw hook's slow half us/call and ms/frame on the caller thread.
`engine motion: pixels per eye-frame` -- joined, masked, pool-not-rig (camera), stale;
counted only by the instrumented DLSS/FSR shader (advanced.temporal_aa_diagnostics = 1 or
a debug view), else the line says "not counted", never zero. `engine motion: family
vs_...: live | STOOD DOWN -- reason | not drawn yet` with binds, draws and patched PS.
If the emit never runs: calls 0 (and the status says why); if the substitution never
runs: MRT6 bound 0 and every family "not drawn yet"; if the compose never runs: views
asked 0.

**What the first flight must show** (Frontier or Steam, the build's own log first):
`fix.engine_motion = on`, `fix.temporal_aa = dlss`, `advanced.temporal_aa_diagnostics =
1`, a ship landing at a settlement. (1) pose disagreements 0, locate failures 0, write
faults 0. (2) movers joined per frame near the tracker's movers (the research joined 64/64
on the landing ship); records with motion > 0 while the ship moves. (3) the ship's families
(DE54, AACF, 66DE, 61AE, EB52, 5B4D) "live" with draws, no refusals. (4) eye-frames
bound = eye-frames, views given = asked, invalidations near 0. (5) pixels: joined on the
order of the ship's silhouette, masked near 0. (6) `motion_source` view: green on the
ship and its turret only, blue on the settlement's buildings. (7) no ghosting on the ship
under DLSS against `advanced.mesh_motion = on` + `advanced.temporal_aa_estimated_objects
= on` (the old estimates). (8) cost: the draw hook's slow half ms/frame (target under 0.5
on the caller thread) and the emit's job-thread ms/frame.

**Not covered, stated plainly.** Movers the draw-item builder (0x42B4130) emits -- 2 of 82
in capture 165433 -- are "not a rig record" to the compose and keep the camera term (their
per-part previous transforms are phase 2); articulated parts on still records likewise.
Skinned records move as their record (phase 1 by spec). Stations and ships in space are
untested (the coverage flight). The frame clock is the present counter: one render-data
update per present is what the research's exact per-frame join measured; a violation shows
as same-frame changes (masked) or gaps. The per-eye scene-constant snapshot assumes one cb1
per eye pass; a violation shows as invalidations. RT6 inherits the pass's blend state; a
blended or write-masked pass loses coverage (camera term, never a wrong vector) and only
the pixel counts would show it. Observed while building: one extra group barrier at the
end of `mv` changed the instrumented variant's depth output on WARP
(screen_consumer_test); not investigated, not shipped -- engine pixel counts ride the
existing gCount instead.

### 2026-09-23 -- Fix round: the first flight's three failures, the review's six items

**The flight.** `edvr_gfx_20260923_065324.log` (Frontier install), `version
v0.17.0-390-g21ab1f7d` -- the phase-1 merge, the right build. EDVR's native OpenXR, Pimax
Crystal Super at 3070x3032 per eye (56.6%), DLSS on an RTX 5090 (the build does not read
the DLSS version), ship-scene depth 1597x1835 `D32_FLOAT_S8X24_UINT`, 90 Hz.

**1. Every eye-frame refused.** `[06:57:35.614] engine motion: movers joined 42.7
records/frame ... eye-frames 2639, with MRT6 bound 2639, refused: invalidated 2639 (pool
re-uploaded 0, scene constants re-mapped 2639, sources changed 0) ... views asked 2638,
given 0, refused: ... invalidated 2638`. Phase 1 dropped an open eye-frame on ANY write to
its scene constants. Capture 043720 (`drawstate`, VS b1 as each draw saw it) shows why that
is every eye-frame: ONE cb1 buffer (identity 23EF98F0960) serves both eyes and every pass;
inside each eye pass registers 270..275 hold one value (1 distinct block per frame and
target) while registers 90, 124-126, 287-291, 301, 310-311 and 333 change 3-5 times a pass;
the two eyes' rows differ, and the passes interleave (target BF60 ordinals 22..65, 56E0
85..128, BF60 130..2985, 56E0 5050..7784). So a same-rows re-map, or the other eye's rows
going through the same buffer, dropped the eye-frame. Only its first pool draw was ever
substituted (family EB52, ps_3434 alone), and the depth-equality gate never ran for real:
the flight's "stale" pixel counts are an artifact of the next bug, not evidence about it.

**The shadowing bug** (found reading the flight against the code): the trained block of
`temporal_pass.cpp` declares its own `const bool engineAvailable = amdEngine ?
fsr3Available(...) : dlaaAvailable(...)` in an inner scope, so `engineBound =
engineAvailable && depthSrv` read the UPSCALER's availability. Every DLSS dispatch of the
flight bound the engine inputs (null views), set probe bit 2048, and cleared CS t21/t22
after (the save covered t0..t18, so they stayed null). The outer flag is now
`engineViewsGiven`. Its signature in the log, with views given 0: `[06:55:35.410] engine
motion: pixels per eye-frame on the trained path: engine-joined 0, masked 0 (no history),
pool surface not a rig record 0 (camera term), stale slot 2832665` -- the whole eye
"stale", because an unbound ES reads (0, 0) and phase 1's decode let x = 0 through.

**Fix.** A write no longer decides anything. The Map tee keeps a watched source's mapped
pointer and map type; the Unmap tee (before the real Unmap) reads registers 270..275 back
and counts a pool write as an append (NO_OVERWRITE) or a replacement; the NEXT substituted
draw of that eye-frame then compares: rows unchanged -- kept, counted; rows changed --
dropped ("scene rows 270..275 changed"); a copy or update into cb1 (rows not seen) --
dropped ("rows unknown"); pool appended -- the snapshot copy refreshed (earlier slots are
untouched by the NO_OVERWRITE contract); pool replaced -- dropped. A write after the eye's
last draw changes nothing.

**2. History gaps.** `[06:55:35.410] engine motion: emit (live) over 30 s, 1688 frames: ...
first seen 1056, gap 13069, reused pointer 0; same-frame repeats 0` (7.74 gaps/frame) and
`movers joined 9.3 records/frame ... against the tracker's 349.7`; every window (8) had
same-frame repeats 0 and pose changes
within one frame 0. Per frame the emit sees 39-693 records with items and 0.16-20.8 gaps
(at most 3% of emissions), so gaps cannot explain a 10-40x mover shortfall. Most calls
append nothing (06:57:05: 3,330 calls/frame, 486 with items). Hypothesis for the next
flight: the tracker's movers are mostly records evaluated but not drawn this frame (culled,
or selected for no view). The census below measures exactly that. Per the review, no delta
scaling: a gap is masked.

**3. The boarding flicker** (Sean: right after re-boarding, ~06:57:10-14). Timeline, all
quoted from the log: 06:57:10.333 `luma probe: eye=0 first black stage is game`; 06:57:11.731
`the scene's depth is in hand -- the depth probe's 1597x1835 target` (the pair re-created);
06:57:13.671 `settlement detail (acting): no longer on foot ... the governor resumes`;
06:57:14.269 `k 1.00 -> 1.25, up 0.25`; 06:57:14.416 `first black stage is none`; 06:57:25.228
`dropped (the game's setting would have kept it) 108.2/frame (max 1565)` for each eye. The
first ship eye-frames of engine motion fall in the 06:57:05-06:57:35 window (2639; the
on-foot window before it had 0); phase 1 logged neither the slot target's creation nor the
first substitution. From 06:56:50 to 06:57:10 engine motion substituted nothing
(`[06:57:05.409] ... eye-frames 0, with MRT6 bound 0`) and its emit writes only bytes
288-319, which no pool VS reads (the scan of 89 pool VS above). What did change in that
stretch: the temporal pass's world path flipped between the scene camera and auxiliary
camera rows, eleven lines of `auxiliary camera rows do not follow the head; the world path
waits for the scene camera` / `scene camera accepted` from 06:56:50.111 to 06:57:09.460, the
transition-flash detector stopped withholding frames that land on a parked auxiliary
camera (06:57:08.142), and one 22.4 ms frame (06:57:09.838, `LONG FRAME`). Candidates: (a) from 06:57:14.269 the LOD governor ramping from k 1 --
the prime candidate; nothing changed there (not this arc's code); (b) from ~06:57:11.7 the
substituted ps_3434 on the first EB52 draw of each ship eye-frame -- CLEARED on WARP: all
nine real pairs, stock vs patched over the same synthetic inputs and patterned resources,
two data sets, write SV_Target0..3 and depth byte for byte identically (40,960 texels a
pair, 0 mismatches; a deliberate one-instruction mutation is caught on every pair), MRT6
exact (the corpus identity test below; WARP, not the RTX 5090's compiler); (c) the whole
flight: CS t21/t22 left null after every DLSS dispatch -- present in
every window, not boarding-specific; fixed; (d) MRT6's binding -- the ship depth is a valid
D32 pair, so the set was legal; it is now validated first and its acceptance read back;
(e) MRT6 inheriting the blend -- touches MRT6 only (rig: the game's four targets
bit-identical). The fix round logs `engine motion: eye N slot target created/re-created`
and `engine motion: substitution starts at present frame N ... after M frames without one`,
so the next flicker can be lined up against them and against the governor's `k ... up`
lines.

**The review** (`reviews\engine-motion-review-2026-09-23.md`), all five findings folded
into this round, plus its stand-down note:
1. *Missing history masked.* First sight, a gap, a reused pointer: MASKED, the pose kept as
   the baseline, JOINED only on the next uninterrupted frame. Rig: emit cases at frames
   100/101, 106/107, 108/109, 118-120; the consumer test shows a masked pixel gets the
   no-history vector (`float2(size)*2`) AND MK = 1.
2. *Invalidity carried.* Each table entry says whether its pose is CERTIFIED (one validated
   pose for its frame). A same-tick pose change, a call whose items do not all validate,
   a locate failure or an unreadable record uncertifies it: the next frame is masked and
   re-baselines, the frame after joins. Every item is validated before the table moves; a
   mixed call writes its valid items masked and its failed ones not at all. Rig: the
   review's A / B+X / C repro across 111-114 (113 masked, not joined with B), the same-tick
   p3 held still (102 masked, 103 joined with zero motion), the mixed call 115-117.
3. *Source rebinds.* VS t33 and VS b1 are hooked (VSSetShaderResources, slot 1 of
   VSSetConstantBuffers) and in the draw's cache; the snapshot holds both objects and checks
   the shadow against the context; every later substituted draw compares view, buffer,
   FirstElement/NumElements. Rig (engine_velocity.cpp linked in): t33 to another pool,
   b1 to another buffer, a view with FirstElement 1 -- each drops the eye-frame, by
   reason; the same view again, another view over the same elements, a PS change alone --
   kept. Not hooked: D3D11.1's VSSetConstantBuffers1; a b1 bound through it shows as
   "binding shadow disagreed" at the next eye-frame's snapshot (that eye-frame dropped).
4. *Blend.* OMSetBlendState is hooked; substituted draws run under a DERIVED state (the
   game's on its targets, independent blend on, target 0's state copied where the game had
   it off, MRT6 unblended R|G), re-derived after any blend set, the game's put back by the
   next unsubstituted draw and at the frame boundary. The slot is now written ODD (2 * slot
   + 1): a sum, a blend or a clear is never an odd whole number, and the compose declines it
   (kind 5, magenta in `motion_source`). Rig: overwrite, additive, blend factor, R|A mask,
   MAX, independent per-target states -- the game's four targets bit-identical to the stock
   pair's, MRT6 exact; the inherited state's arithmetic is declined, never another record
   (440 pixels); a mid-pass change; depth-write-disabled passes (stale unless over their own
   depth; the EQUAL pass keeps its record).
5. *Compute slots.* `cs_stage_save.h` saves t0..t22, u0..u6, b0..b2, s0 and the shader for
   every branch (t19/t20 had the same gap before this arc), with static_asserts tying the
   counts to this feature's slots. Rig: sentinels back by identity after the engine path,
   the fallback path and an unrestored save.
6. *Stand-down.* `kinematicEvalEmitHookLive` reports whether direct producer 0's relay is
   installed and gated open. If not (or the build check fails): `engine motion: STOOD DOWN
   -- the emit hook is not installed (<why>)` at configure, on the change, and in every
   30 s block; nothing substituted; every view refused (counted). Rig: all three lines,
   and standing up again.

**Binding.** MRT6 joins a pass only when the depth view is a 2D texture at mip 0, 32-bit
float, single-sample and the slot target's size, and every game target a single-sample 2D
texture at mip 0 of the same size; refusals counted by reason, and a set the runtime drops
is detected (read back) and the game's own put back. A depth texture change inside an
eye-frame drops it ("depth changed"). Rig: a 24-bit depth refused and counted, a new depth
pair re-created (logged) and given.

**Rig.** `tools\engine_velocity_test`, in the gate: 838 checks -- the patcher and the
blend states (shader_tests.h), the history rules across following frames and the census
(emit_tests.h), the compose's arithmetic (math_tests.h), the production `mv` entry with
engine inputs bound (consumer_tests.h: joined exact to 3e-7 px; masked gets the
no-history vector and MK = 1; even, fractional, zero, stale, cleared and out-of-range
codes all equal the no-engine result; Stats 50..54 match), and engine_velocity.cpp
itself linked in and driven on WARP (lifecycle_tests.h). Locally (`--corpus`, 1,073
checks): all nine real pairs patch, reflect and create with the odd-code tail, and
corpus_identity.h draws each stock and patched pair over the same inputs (a synthetic VS
built from the patched PS's input signature; the game's shaders carry no RDEF, so the
resource declarations come from the disassembly) -- SV_Target0..3 and depth bit-identical,
MRT6 exact, every pair.

**New counters.** Emit: masked for first seen / gap / reused / same-frame change /
previous frame not certified / this call unproven / table full; tainted. History: gap
ages (2, 3-4, 5-8, 9-64, over 64), burst frames (32 or more gaps in one frame -- a clock or
scene event), and the CENSUS -- one record in eight by address, every call with or without
items: record-frames evaluated, moving ones drawn vs evaluated-but-not-drawn (scaled by 8 to
compare with the tracker), and each sampled gap classed "evaluated without items in
between" or "not evaluated at all". Draw side: invalidations by reason, kept re-maps,
refreshed pools, MRT6 refusals by reason, runtime rejections, blend states bound /
refused / shadow disagreed, views refused while stood down, and pixels "corrupt slot
code" (must be 0).

**Corrected from phase 1.** "RT6 inherits the pass's blend state; a blended or
write-masked pass loses coverage (camera term, never a wrong vector)" was wrong: the
review's WARP counterexample turned (slot 1, 0.5) over (-1, 0) into (slot 0, 0.5) under
ONE+ONE. Ruled out: the tick straddling two frames as the gap cause, because same-frame
repeats and pose changes within one frame were 0 in all eight windows; rows 270..275
changing inside an eye pass, because capture 043720 holds one block per pass.

**What the next flight must show** (the build's own log first:
`python tools\edvr_log.py --target frontier --expect-build HEAD`): the same scenario --
landing ship, fix.engine_motion=on, fix.temporal_aa=dlss, advanced.temporal_aa_diagnostics
=1, the motion_source view. (1) no STOOD DOWN line; views given ~ eye-frames; invalidated
~0 (if not, the reason names the next fact), binding shadow disagreed 0, blend refused 0,
MRT6 refusals 0 on the ship. (2) pixels: engine-joined on the ship and turret (green),
corrupt 0. (3) movers joined ~ the tracker's -- or the census's "evaluated but not drawn"
movers account for the difference, which would close the gap question as visibility,
not a defect. (4) pose disagreements, locate failures, write faults 0. (5) cost: the slow
half's ms/frame on the caller thread (the substitution now runs for the whole pass; target
under 0.5). (6) flicker: if it recurs, which of `substitution starts`, `slot target
re-created` or the governor's `k ... up` lines it lines up with.

### 2026-09-23 09:38 re-fly (build 7fdc17a9, engine_motion on, DLSS, 1597x1835 -> 2458x2824, settlement pad; cockpit 09:38:45-09:41:04, on foot 09:41:05-09:43:08, back aboard after)

The overseer read Sean's 09:38 re-fly capture and the 09:28 UI test from the same session;
do not re-derive these facts, and this entry does not re-read the logs whole. This is the
engine-motion half -- the fix round's owed flight (previous Next).

**Cockpit.** Substitution from present frame 4160; pool records joined 737k per 30 s
window, with motion 10930, masked 13757 (gap 12669, first seen 1012); disagreements,
locate failures, faults all 0; movers joined 6.2 then 12.5 records/frame against the
tracker's 164 and 426 moving records/frame (the difference is records evaluated but not
drawn, ~160/frame by the one-in-eight census); eye-frames invalidated 0; derived blend
states bound 52341, refused 0; views asked 3498, given 2390, refused other depth 1106;
pixels per eye-frame: engine-joined 67640-76904, masked 0-2, camera term 59-90k, stale
172-225k (the largest class), corrupt 0; bracket 0.18-0.2 us/call, 0.015-0.2 ms/frame on
the job threads.

**Eye run 094048 (cockpit, motion_source view).** The dashboard, consoles, throttle, stick
and canopy frame paint STALE (yellow, the slot's depth is not the scene's bit for bit);
the seat paints camera term (blue); the two side-panel surfaces and the settlement's rig
parts (drone, turret, posts) paint green; the settlement's buildings blue. Green is any
rig record with a certified previous pose, still or moving, not a mover: the legend in
edvr.ini said "a moving record's own motion" and misled; the motion agent is rewording it.

ruled out: green on the cockpit side panels is a mover bug, because green is kind 1 = any
certified rig record, still or moving (engineRecordKind/enginePixel).

**Cost.** The temporal pass's prep stage (colour copy + motion-vector dispatch) ran
2.9-3.7 ms per stereo pair whenever the engine path was live in the cockpit, 0.14-0.22
when it had nothing to do (menus, on foot), against 0.12-0.22 in the 07:31 and 09:28
flights with engine_motion off; NGX full unchanged 1.4-1.9. The governor's cockpit
windows: caller work 12.07 and 15.75 ms, 923 of 1749 and 1133 of 1580 two-slot cycles, 783
and 1093 of them GPU-bound. So the CPU figure Sean saw is the caller waiting on a GPU over
the period; the engine path's own CPU cost is ~0.02-0.2 ms/frame. Suspect (unmeasured):
enginePixel fetching the 320-byte pool record per pixel, twice, before the kind is known;
sub-timers and a cut are in flight with the motion agent.

ruled out: the higher CPU figure with engine_motion on is CPU work in the engine path,
because the bracket costs 0.02-0.2 ms/frame on job threads while the misses are GPU-bound
and prep is 2.9-3.7 ms/pair.

**On foot.** "The scene's depth went away" at 09:41:04.474, "in hand" only at boarding
09:43:08.360; rotation-only reprojection, no engine motion (10578 frames without a
substitution), no UI depth; the hills shimmered while walking. Cause: the on-foot world is
drawn into a 3840x2160 depth target (#23, D32_FLOAT_S8X24, 5.5-15.6k draws a frame) that
the probe's eye-size filter never accepts; the eye-sized list was empty or junk and the
scene pair drifted to 256x256 targets. CORRECTED the same day (the next entry): there is
no eye scene to find on foot -- Odyssey draws the on-foot world once, flat, into that
target and shows it on a panel in each eye; the shimmer came from fix.ui_quality's layer
lifting that panel out of the eye, so DLSS upscaled black and the on-foot world got no
temporal pass at all. The UI layer now leaves the 2D screen in the eye while on foot.

### 2026-09-23 -- The re-fly (093817): the fix round holds; on-foot depth, the prep price, the stale cockpit

**The fix round in flight** (`edvr_gfx_20260923_093817.log`, build `v0.17.0-409-g7fdc17a9`,
Pimax Crystal Super, DLSS 1597x1835 -> 2458x2824, settlement pad). `[09:39:17.316] ... eye-frames
3018, with MRT6 bound 3018; invalidated 0 ... kept: scene constants re-mapped with rows 270..275
unchanged 33198`; `[09:40:47.312] ... engine-joined 73036, masked 1 (no history), pool surface
not a rig record 67923 (camera term), stale slot 181699 ..., corrupt slot code 0`. The cb1
re-maps are kept, nothing is refused, no slot code was corrupt.

**1. On foot there is no eye scene to take depth from.** From `[09:41:04.474] temporal aa: the
scene's depth went away` to the boarding at 09:43:08 the pass had no depth. The eyes are not
drawn with the world on foot: `[09:41:04.442] ui layer: 2D screen (vs 5C36AF051B98B9F1 ps
CFE84157BC76E921) into a 1597x1835 R8G8B8A8_UNORM target: redirected into the layer` (and
`panel curvature: substituting the panel's quad`, `vScreen: panel distance x0.700 applied`);
`vScreen totals: ... (2-2 per frame ...), largest eye-draw count 2 this window` -- one panel
draw per eye; the world is drawn ONCE into the 2D screen's own 3840x2160 target (#23, `5505
draws bound to it, 0 of them with an eye-sized colour target`), the picture a flat panel then
shows. And with fix.ui_quality on, the UI layer takes that panel draw out of the eye image
into its own layer composited after the upscale: `luma probe: eye=0 game=0.000/0.000/100%` on
every sample from 09:41:05 to 09:43:08, `ui layer totals ... 2.00 draws a frame redirected (2D
screen 2.00)`, prep 0.13-0.14 ms a pair. So on foot DLSS was handed black, and the world reached
the headset without any temporal pass -- the distant hills' shimmer is the 2D screen's own
aliasing. The previous build (065324, 21ab1f7d, before the UI layer) on foot: `depth probe: the
eye-sized targets: .` (none; the busiest #28 3840x2160 with 15121 draws) -- no eye depth then
either -- but the panel was IN the eye image (`luma probe: eye=0 game=0.211/0.806/57%` at
06:56:50) and went through DLSS with the screen map (`screen motion GPU: ... source 3840x2160`).
The "went away / is in hand" wording is ddd3dacd (2026-09-03), not this build; the 07:15 log
(6d34ffd5) has no temporal aa, depth probe, dlaa or luma lines at all -- temporal AA was off in
that flight -- so it says nothing about depth.
ruled out: on foot the world drawn packed side by side per eye (a per-eye region of #23 for the
pass), because each eye gets exactly one 2D-screen panel draw a frame of the whole picture
(vScreen totals 2-2, the panel's curvature and distance lines) and #23 never has an eye-sized
colour target beside it.
Remedy, for the overseer -- it lives in the UI layer's and the screen motion's files, not this
arc's: (a) leave the 2D screen in the eye image when it carries the world (on foot) and redirect
only menu and UI screens, so DLSS and the screen map see it as before the layer; or (b) run a
mono temporal pass on the 3840x2160 source itself (its own depth #23, the desktop camera's
motion) before the panel samples it. Built here, instrument only: `depth probe layout:` in the
20 s census -- for a busiest depth target with more than 1000 draws and no eye-sized colour
target beside any of them, the viewports and scissors its draws set (sampled on the frame's
first draw and every 1024th), the colour target beside it, and the shaders drawing the
eye-sized targets meanwhile. Expected on foot: one viewport `(0,0) 3840x2160`, the 2D screen's
colour target, eye targets drawn by `vs 5C36AF051B98B9F1 ps CFE84157BC76E921` about twice a
frame. Two viewports would reopen the packed hypothesis. No `depth probe layout:` line = the
census never saw such a target (or the probe is off).

**2. The prep price is the coverage pass, not the engine path.** `prep` against the lines
around it: `[09:38:45-09:39:07] prep 3.64-3.73` (the tracker publishing 143 spheres),
`[09:39:13-09:39:48] 0.15-0.22` (menus: `eligible 0`), `[09:40:04-09:41:08] 2.91-3.13` (2570-2606
spheres), `[09:41:16 on] 0.13-0.14` (on foot: 2617-2784 spheres but no scene depth, so the
coverage pass stands down). And the phase-1 flight 065324, whose engine views were NEVER given
(views given 0 in every window), shows the same shape: prep 8.37-8.50 at 06:53:55-06:54:14 (143
spheres), 3.87-4.01 through 06:54:41, 0.17 at 06:54:55-06:55:02 (`eligible 0`), 3.16-3.34 from
06:55:21 (2571-2618). The one thing in prep that runs exactly when spheres are published and
the scene's depth is in hand is stage B's coverage pass (`kinCover`: one thread per sphere
walks that sphere's whole quarter-res rectangle with two atomics a texel; a handful of the
ship's own spheres just in front of the eye keep a wave busy for milliseconds). Its pair has
three readers -- the compose veto (fix.engine_motion_veto, off), the movers view's cyan, the eye
run's dump -- and none was on. The production `mv`, compiled (fxc cs_5_0 /O3): 4 `ld_structured`
from t22 and 1 load from t21 per pixel -- the record fetch is already only the four uint4 the
kinds and the reprojection read, once per pixel, and a stale pixel (the largest class) never
reaches it.
ruled out: enginePixel's record fetch as the ~3 ms prep, because 065324 carried the same 3-8 ms
prep with the engine views never given, and the compiled `mv` reads 64 bytes of record only
for a depth-matched pool pixel (joined + camera + masked: 127-141k of 2.93 MP an eye).
Built: the coverage pass runs only when something reads it (the veto, the movers view, an eye
run), with a one-time `engine motion coverage: not run -- nothing reads the coverage pair ...`;
and the price line times prep's parts inside its own figure: `prep a/b (copy c/d coverage e/f
mv g/h)`, per stereo pair. Not built: the record kind in the slot code -- the pixel shader that
writes the slot never sees the record (the slot comes from the instance data; the kind is the
record's marker checked against its hash), so it would take a t33 read in every substituted
pixel shader or a patched vertex shader in every family, to save a 64-byte fetch on the kind-3
pixels only (59-90k an eye-frame).
Expected next flight, cockpit, fix.engine_motion=on, veto off: `prep ~0.2-0.5 (copy ~0.05
coverage 0.00 mv ~0.1-0.3)`. If the cut does not work, prep stays ~3 and the parts name the
stage (copy, mv, or the rest = prep less the three). A/B: fix.engine_motion_veto = on runs the
pass again and its part prices it directly.

**3. The stale cockpit is mostly the commander's own legs, under unkeyed layers.** Eye run
094048, left eye, ledger frame 13627 (= scene frame 13626), analysed offline (the scripts in
the session's scratch `task3`): L0 has 515,316 yellow pixels, 194,048 at render resolution --
the log's 172-225k. The dashboard and console surfaces are mostly DIM (no MRT6 data: the
cockpit shell is vs_BFE51414CC3024B4 / ps_DB79AE788E049DFD, not a pool family); the big yellow
areas are the commander's legs and lap (the skinned body, record 5472 and 6297-6299, at 1.62
m), with cockpit trim and two side-console patches; the "blue seat" is the same body. For
121,846 of the 194,048 (62.8%) the order is measured: the keyed body layer is drawn first
(vs_EB5234DB6ADB491D / ps_3434, draws 164-168) and writes MRT6; a later, NEARER, unkeyed draw
covers it without writing MRT6 -- vs_EB5234 with ps_B7D50283329322C3 (the keyed VS with an
unkeyed PS; the log's `unkeyed pixel shader ... left stock`) over 65,094 px, the keyed layer a
median 24.6% deeper; vs_7B0DC42D383F694C / ps_0DF03E64DF9DBEF1 (no keyed family) over 56,752
px, 10.8% deeper. The exact depth test declines those slots correctly: the visible surface is
not the slot's record. The other 68,686 (35.4%: 58k cockpit trim at stencil 144/148, 1.5-4 m;
10.5k world) have no rebuildable keyed surface under them; the cockpit shell is drawn before
every keyed draw, so whatever keyed draw wrote MRT6 there passed the depth test without
leaving its depth -- the shape of a keyed overlay drawn with depth writes off (families
5B4D8E and BBE58E subtract a per-material offset from SV_Position.z, decal-like). The ledger
holds no depth-stencil or rasterizer state, and MRT6 is not in the eye dump, so that third is
undecided.
ruled out: a depth pre-pass (H1) as the stale cause, because none of the 31 draws covering the
yellow region (rows 151-181) is drawn twice in eye A's pass and the keyed layer lies 10.8-24.6%
deeper than the visible surface, not an ulp.
ruled out: a pixel-shader depth output or a depth bias (H4) as the stale cause, because none of
the 16 disassembled pixel shaders writes oDepth and a bias cannot open a 10-25% gap.
Remedy, for the overseer (no code yet): (A) key the two missing shaders -- ps_B7D5 into the
EB5234 family, vs_7B0DC42D / ps_0DF03E64 as a family (its VS is EB5234's plus a normal push of
up to 5 cm from cb2[6..9]) -- so those pixels carry their own slot: joined motion for the
commander's legs instead of the decline. Trade-offs: ps_B7D5's depth sits ~1.6e-4 nearer than
its geometry, an unrecorded rasterizer depth bias -- if SV_Position.z does not carry the bias,
its pixels stay stale (declined, no gain; the corpus harness under a biased rasterizer state
answers it on WARP); the 7B0D push is exact rigid motion only if cb2[6..9] hold still across
frames. (B) a bounded ulp tolerance (meshPixel's 1e-6 relative) does NOT help here -- the gap
is 10-25% -- and would only matter if the undecided third proves ulp-close. (C) writing the
slot from a pre-pass: there is none. (D) for the undecided third, instrument first: MRT6 (slot
and depth) beside SceneZ in the eye dump, and depth-stencil/rasterizer state in the ledger's
empty geometry section. Stale is a decline (the pass's other motion sources stand), never a
wrong record; what it costs is coverage on the legs.
Legend fixed: green is a rig record whose certified previous pose gave its exact motion,
moving or still -- a still one carries the camera's motion through the record -- not a mover
count (edvr.ini's temporal_aa_debug block, the shader comment, the pixel line's
`engine-joined` wording).

### 2026-09-23 -- On foot: the source pass gets MRT6, the screen shader carries rig records (built, not flown)

**Hypothesis.** Walking NPCs, the drone and a ship blur on foot because every panel pixel's
motion is the camera term. On foot the world is drawn once into the 2D screen's source (its
own depth, D32_FLOAT_S8X24; 3840x2160 at vscreen_res_width) and each eye gets one panel draw
of it; the screen shader (screen_motion.h) recovers each source pixel from the source depth
and rows 270..275 and reprojects it as static -- exact for scenery, the camera term for
anything that moved in the source. The engine path never reaches it. Confirmed by the 11:50
flight (`edvr_gfx_20260923_114958.log`, build c00af958): on every on-foot window the emit's
CPU half works (pool records joined 1.07M-2.59M per 30 s, movers joined 27-153 records a
frame, masked 41k-364k, disagreements 0) while the draw half reads `eye-frames 0, with MRT6
bound 0; views asked 0, given 0` with every MRT6 refusal 0 -- the bind is never attempted,
because it keys on an eye scene depth and on foot no pool draw targets one (`depth probe
layout:` names the busiest target 3840x2160 with no eye-sized colour target beside any of
its draws; the eyes get 2.0 draws a frame from vs 5C36AF051B98B9F1 ps CFE84157BC76E921).
**Fixtures, offline** (eye runs 115325 and 115351, eye 0, 1597x1835; scripts in the
session's scratch `onfoot`). ScreenMotion: w=1 on 63.0% / 61.3% of the pixels, median 1.25 /
0.58 eye px; one smooth camera-only field, fitted by depth to the surroundings, explains
every valid pixel to p99.9 0.0016 px. Over the ship (115325, 4,440 px at 165-212 m) the
motion is (+0.776, +0.356) against (+0.777, +0.355) in a 6-px ring; over the drone (115351,
238 px at 135-160 m) (+0.184, -0.098) against (+0.185, -0.099): the camera term, nothing of
their own. The pool ledger (`pool_*.bin`, byte-identical in all 40 files) is the last EYE
pool, from before the on-foot scene: it does not hold the on-foot records. `drawstate_*.bin`
does, for one frame (4,095 source draws into 3840x2160, capped at 4,096): a 20,480-record
source pool with 1,137 joined records, 236 of them with a changed pose (217 over 1 mm, 65 in
the captured draws). Through the source rows the drone's own motion is (+0.01, +0.22)
source px a frame, about 0.12 eye px (the scale, ~0.55 eye px per source px, inferred from
a walker's height) -- 50-70x the fit error, and what the lookup adds; the ship's, at 173-204
m, about 0.02 eye px (0.14 at most). The walkers are rig records too (joined, 10-12.6 mm a
frame, limbs up to 11 degrees a frame) but drawn by vs_F516BF0201303B87, not one of the
seven keyed families: this build does not reach them. And today the near walker's whole
figure is w=2 -- no history every frame, not the camera term (inferred cause: the
first-person stencil bit the weapon path keys on, with no weapon motion under it).

**Built** (branch claude/engine-motion-on-foot). The source is a third eye:
- `engine_velocity.cpp`: screen_motion names the source depth each source frame
  (`engineVelocityNoteSource`, at screenMotionSource); a pool family draw into that depth,
  named this frame or the last two, is the SOURCE pass (`slowPath`, `sourcePass`) and takes
  the eye path's whole rule set -- the snapshot of t33 and b1 at its first substituted draw,
  rows 270..275 held or the frame dropped, pool appends refreshed, MRT6 only where the
  binding validates, the derived unblended state, odd codes -- into a slot target made from
  the source depth's own size (never assumed), created only while the source is drawn,
  logged once with its size and reason, released after 120 present frames without the
  source (logged). `engineVelocitySourceViews` gives the screen shader the slot target, the
  pool snapshot and the source's scene constants this frame and last, with the eyes'
  refusal rules and counters of its own.
- `temporal_shader_source.h`: an ENGINE_MOTION_CORE (no resources) inside the block;
  `engineReprojectRows` takes the rows as arguments and `engineReproject` is the eye's
  wrapper over EN/EB, byte for byte the old arithmetic. `tools/temporal_shader_build` emits
  the core as `kEngineMotionCoreHlsl` (key /2; the build fails on a missing, doubled or
  resource-bearing core), and screen_motion.cpp compiles it in front of `kScreenMotionPs`.
- `screen_motion.h`: before the camera term, `sourceEngine` runs enginePixel's tests on the
  source texel -- cleared, stale (the slot's depth is not the source depth, bit for bit),
  corrupt (not an odd whole code), out of range, the marker. Kind 1 is carried by
  `engineReprojectRows` through the source pool draws' own scene constants (this frame's and
  last: the rows that wrote the depth, the same rule as the eye's EN/EB; in a single-camera
  source pass they are src/old) to its previous source UV, and the existing mapping through
  the panel and the previous eye projection does the rest; a previous UV off the source is
  disocclusion (code 2). Kind 2 returns `(0, 0, z, 2)`; 3, 4 and 5 keep the camera term.
  engine.z counts the kinds per eye pixel into a UAV at u1 (diagnostics 1 or the view),
  read back without waiting; engine.y (`advanced.temporal_aa_debug = motion_source`) puts
  16 + the kind in the map's validity and the compose paints it through the panel.
- Cost: the slot target is 66.4 MB at 3840x2160 and is cleared once a source frame; MRT6
  rides the source pass's pool draws; the screen shader adds one texel load a pixel, and a
  record fetch where a slot is.

**Rig.** engine_velocity_test 938 checks (837 before): `panel_tests.h` drives the production
screen shader (core in front) on WARP -- a moving joined record (translated, turned 12
degrees) lands on its CPU-double previous pixel within 4.4e-7 px, (-1.24, -0.25) px against
the camera term's (0.20, 0.08); masked gives `(0, 0, z, 2)`; unmoved, not-a-rig-record,
stale, corrupt and cleared keep the camera term; with engine.x clear every texel is today's
shader; the counts and the view's encoding exact. Lifecycle S1: a pool draw into the named
source depth (D32_FLOAT_S8X24, 40x24) gets a 40x24 slot target (logged), MRT6 names its slot
exactly, views from the second frame; re-made at 56x20 it follows; an unnamed depth of the
same shape gets nothing; 122 frames without the source release it (logged). The corpus
identity harness is unchanged (the patched shaders are the same; only the bound target
differs). screen_motion_test 56541 (+2: each source frame names its depth to the engine).

**What the flight must show** (on foot, dlss, diagnostics 1):
- `engine motion: on-foot source slot target created 3840x2160 R32G32 (66.4 MB) for the
  source depth ...` once, and `substitution starts ... (on-foot source, vs_...)`.
- `screen motion: the source pass's engine data is bound ...` once.
- Each on-foot window: the movers line with `eye-frames N, with MRT6 bound N` non-zero
  (the source frames are eye-frames) and `engine motion: on foot: source frames N, with MRT6
  bound N (slot target 3840x2160); screen views asked ~2N, given ~2N ...; panel pixels per
  eye draw: engine-joined J, masked M, pool surface not a rig record C, stale S, corrupt 0`.
- After boarding: `on-foot source slot target released (3840x2160, 66.4 MB) ...`.
- motion_source leg: the drone and the ship green on the panel, walkers red or dim.
If the new code never runs, the log reads as 114958: no slot-target line, the movers line's
`eye-frames 0, with MRT6 bound 0`, and no `on foot:` line (or `source frames 0`). Bound but
never asked: `screen views asked 0`. Asked but refused: the refusal names the reason.

### 2026-09-23 -- Performance round (reviews\engine-motion-performance-review-2026-09-23.md)

One commit per item, built and rigged, none flown.

**1. The legacy tracker is diagnostic-only.** Its observer took one global mutex per
evaluation on the job threads and its Present tick scanned the tracked population on the
caller thread, whenever temporal AA was on; since stage B its only consumer was the movers
line's "against the tracker's N moving records/frame". On foot in 114958 the emit saw 4.5-7M
calls per 30 s with 390-470 moving records a frame, and the frame was caller-thread bound at
~16 ms: the prime suspect. Now the emit holds the shared eval hooks itself
(`kinematicEvalEmitAttach`: the direct-producer relay gates on the same eval gate, so the
emit's want keeps it open), and the tracker and the emit's census of one record in eight
run only while engine motion's diagnostics want them -- advanced.temporal_aa_diagnostics,
the movers view, or an eye run from its arming to the first config poll after it is written
(`applyEngineMotionDiagnostics`, temporal_pass.cpp). The emit's previous-pose certification
is its own table and is untouched. Measured whenever the tracker runs, per 30 s: `engine
motion: tracker (diagnostic-only) cost: N evaluations (X a frame), each taking its mutex:
lock wait Y us sampled (1 in 64, S samples), ~Z ms/frame summed over the job threads;
Present scan W ms/frame on the caller thread`. With diagnostics off: `engine motion: tracker
off (diagnostic-only ...)`, the movers line reads `(the tracker, diagnostic-only, was off)`
and the census line ends `(the census is off ...)`, emit joins unchanged. Never ran: the old
movers wording with a tracker number and no cost or off line. Rigs: kinematic_motion_test
case 20 (the cost window), engine_velocity_test P1.

**2. Preparation waits for an eligible draw.** The slot target's create and clear, the pool
and scene-constant snapshots and the MRT6 bind ran at an eye-frame's first pool family draw,
before asking whether its pixel shader was keyed and its patch existed; an eye whose family
draws were all unkeyed paid for all of it and was then handed to the compose, which scanned
a cleared map. Now `slowPath` resolves the keyed PS and its patches (VS where the family
needs one) first, prepares only for such a draw, puts the game's state back on a declined
draw as before, and marks the eye usable (`written`) only when a substituted draw is about
to be issued -- MRT6 bound alone no longer counts. One consequence: the first eye-frame after
one with no eligible draw has no last-frame scene constants for that eye and is refused as
`no previous scene constants`; the next gives. Signature, in the movers line: `eye-frames N,
with MRT6 bound B (prepared only for an eligible draw: S eye-frames had a pool family draw,
U a substitution; prepared for nothing P, under the old order Q)` -- P near 0, Q the waste
removed. Never ran: no parenthesis. Rig: engine_velocity_test P2 (an unkeyed-only eye
prepares and gives nothing, Q counts it; one accepted draw restores exact coverage the frame
after).

**3. No repeated shader setters.** A slow-path visit forced only by a source change -- the
re-fly's ~11 harmless cb1 re-maps an eye-frame, a pool append, a blend change -- re-issued
the raw PS (and VS) setters although ours was still bound. Now, after the source checks,
each setter is skipped when the patched shader is still installed: the same patched and
original shader and the stage's binding generation unchanged since ours went in (the game
set nothing there). The blend and the source checks are untouched. Signature, the movers
line's tail: `restores R; shader setters issued I, skipped K (ours still bound)` -- K
roughly the kept re-maps and refreshes that reached a draw. Never ran: no setters clause.
Rig: engine_velocity_test P3 (a re-map inside a pass: the patched PS stays bound, the setter
skipped and counted).

**4. The snapshot copies, measured only.** Each prepared eye-frame copies the whole pool
buffer and the scene constants, and every observed append copies the pool again; the stats
counted refreshes, not bytes. Now, per 30 s: `engine motion: snapshots (measure only): pool
capacity C records (X MB), views expose E; copies: pool P at preparation (Y MB) + R on
append refreshes (Z MB), scene constants S (W KB); ~V MB a frame logical` -- logical bytes
submitted, not GPU time (the GPU time is item 5's). No storage change: if R x capacity is
material, the next step is copying proven dirty ranges or sizing to the exposed range,
never sharing one snapshot between eyes unproven. Never ran: no snapshots line. Rig:
engine_velocity_test P4.

**5. The attribution gaps.** (a) The slot target's clear, the snapshots at preparation and
the append refreshes run in the game's own eye pass, before prep, so no prep figure contained
them: each now sits between GPU timestamps (GpuTimer: a lease on the shared clock, polled at
the owner's frame boundary with DONOTFLUSH, no Flush, no wait), and the price line prints
them inside prep's parenthesis: `prep a/b (copy c/d mv e/f; eye-pass capture per event,
before prep: clear m/p xN, snapshots m/p xN, refreshes m/p xN, U untimed, V invalid)`. (b)
The foveated route's prep now has the same copy/mv parts as the full frame. (c) The price
line names the shader build: `temporal aa price: <treatment>, WxH, lean shader|diagnostic
shader, ...` -- and a change of build closes the window, so a diagnostic capture never prices
the production shader. Never ran: no capture clause (or all three x0 with the capture
counted untimed), the foveated parts at 0.00/0.00, no shader word. Rig: engine_velocity_test
P5 (every capture timed or counted, the take resets).

**6. One depth load fewer per engine pixel.** The mv pass holds the scene depth at its own
texel (`sceneZraw`, from its depth tile at region.xy + id), and its offset-zero engine query
loaded the same texel again inside enginePixel. `enginePixelZ` takes a held depth (a literal
flag at each call, so the branch compiles away); the offset-zero call passes `sceneZraw`, the
jittered callers keep the lookup. Same texel, same value, so the ownership test is unchanged
bit for bit: engine_velocity_test's consumer cases (joined exact, masked, declined kinds)
and the real corpus stay green. No log signature -- the pixel counts are the same; its price,
if any, is inside `mv` in the price line against a build without it.

### 2026-09-23 -- Flight 5 (140351): the on-foot source is drawn by more than one camera

**The flight.** Build 48dcb9b6 (the on-foot path and the performance round), Frontier,
DLSS, settlement; disembarked 14:06:24 (the journal), aboard again ~14:08:48. Sean: the
on-foot movers still blur, no change. The on-foot line per 30 s window:

| window | source frames | views asked / given | dropped (all "scene rows 270..275 changed") | re-maps kept |
|---|---|---|---|---|
| 14:06:51 (standing) | 1470 | 2938 / 2938 | 0 | 134170 (with the ship's ~7k) |
| 14:07:21 (starts walking) | 1902 | 3804 / 2922 | 441 | 109202 |
| 14:07:51 (walking) | 2031 | 4062 / 0 | 2031 | 0 |
| 14:08:21 (walking) | 2405 | 4810 / 0 | 2405 | 0 |

In the two all-dropped windows the only family substituted is vs_AACFDCF2FB9AD809: 4062
and 4810 binds -- exactly two a source frame -- and ~7 draws a frame; every other family
(EB52 included, ~6100 substituted draws a standing source frame) 0. So each walking frame
took its snapshot at an AACF draw, a second AACF visit passed, and the first cb1 re-map
after them carried other rows 270..275: the eyes' rule (hold the eye-frame to its first
draw's rows, correct for an eye pass: capture 043720, one block per pass) dropped the
whole frame. Standing, the ~87 re-maps a frame all carried the snapshot's rows.

**The ledgers (eye runs 115325/115351, 4096 source draws each, standing).** Two camera
blocks through the one cb1 inside a source frame: the world's (4092 draws, every pool
family draw, row 273.z = 0.025) and a first-person one (draws 68-71: vs_7B0DC42D x3,
vs_CFCA8FFC x1; 273.z = 0.0675, rows 270..272 x/y x1.23, rows 274/275 the same). The frame
opens with the walkers (vs_F516 x67), then the terrain vs_ACE405F4 -- screen_motion's
naming draw, whose cb1 its camera term copies -- then the weapon, then the world's pool
draws (vs_EB52 from draw 75). vs_AACF is in weapon_motion's weapon/tool family set (7B0D,
8B58, 114A, AACF, 174E) as well as a generic detail shader. What camera the walk's AACF
draws used is not in this log (no rows are printed); a camera attached to the body (a head
or weapon bob) is the reading that fits "equal standing, different walking".

ruled out: one camera per on-foot source frame (the eyes' rows rule applied to the
source), because flight 5 dropped every walking source frame (2031/2031, 2405/2405) at the
first re-map after an AACF-first snapshot while 0 of 1470 standing frames dropped, and the
eye runs draw a first-person block under a second camera inside the source pass.

**The fix (branch claude/onfoot-source-camera).** The source is held to the NAMING's
camera, draw by draw, instead of to its first draw's. screen_motion names the source at
the terrain/scene draw and now passes the cb1 that draw reads
(src/d3d11/screen_motion.cpp:261); engineVelocityNoteSource records it and its rows
270..275 as the watch last saw them written, for that present frame
(src/d3d11/engine_velocity.cpp:1517). Each source pool draw, before anything is prepared
(engine_velocity.cpp:878, sourceCameraHolds at :749), must be after this frame's naming,
read the naming's cb1, and carry its rows; otherwise it is DECLINED -- not substituted,
the frame and its other draws kept -- and counted by reason (before this frame's naming,
the naming's rows not seen, other scene constants, rows not seen, another camera), another
camera also by family, by which rows differ (270..272, 273, 274, 275) and by the largest
camera-position distance. The frame's snapshot is taken at its first held draw, so SEN is
the world's rows by construction -- the camera screen_motion's camera term already uses.
The other camera's pixels keep what they had: first-person meshes are carried by the
weapon path (the stencil), anything else takes the camera term, as before. Per-camera
views (the other camera's records reprojected with its own rows) were not built: the
slot target cannot say which camera wrote a pixel without another target, and the flight
gives no sign the declined draws are movers. The eyes' rule is unchanged.

The on-foot line now prints the source's own drops by reason (`frames dropped: none` or
the reasons) and `camera rule: namings N (rows not seen U), checks held to the naming's
camera H, declined D in F frames (<by reason>); another camera changed rows 270..272 on
a, 273 on b, 274 on c, 275 on d, its position up to X m from the naming's, by family: ...`.
Checks, not draws: a draw repeating the last one's state (no new binding, no cb1 write)
skips the slow path and runs as its predecessor did.

**The panel's kinds without diagnostics.** The screen shader counted its kinds only with
advanced.temporal_aa_diagnostics or the motion_source view (an atomic per panel pixel with
engine data, millions a frame on five addresses). Now, otherwise, one frame in 300
(kPanelSampleFrames) counts on a 4 x 4 grid of eye pixels (engine.z carries the stride,
src/d3d11/screen_motion.h:145; screen_motion.cpp:367): at most ~370k atomics on the
sampled frame against ~5.9M for a full count, about 0.25 us a frame averaged even at a
pessimistic 1 ns an atomic, plus a 20-byte clear, copy and a DO_NOT_WAIT read. The line
says `panel pixels per sampled eye draw: ... (sampled: one frame in 300, one eye pixel in
16 on a 4x4 grid; raw counts, not scaled)`; with diagnostics it counts every pixel as
before.

**Rigs.** engine_velocity_test S2 (lifecycle_tests.h:896): the walk's interleaving -- the
naming's world rows, two draws of another camera (a 4 cm bob on row 275), the world's
draws -- drops an EYE-frame (the eyes' rule, as the flight counted it) but on the source
declines only the other camera's checks: every frame given from the second, frames
dropped none, the view's scene constants the world's rows 270..275 exactly (this frame's
and last), MRT6 the world's slot and never the other camera's, a draw before the naming
declined as such, the rows and 4 cm distance and family printed, standing nothing
declined. The panel case: the stride-4 grid counts only the pixels on it and changes no
motion. screen_motion_test: the naming hands over the naming draw's VS b1. engine_velocity_test
1008 checks, screen motion 56542, kinematic_motion_test 71; config contract 261.

**What a flight shows.** Walking, near a drone or a landing ship: `frames dropped: none`,
`screen views asked A, given A` less the first frame's `no previous scene constants`, the
camera rule's declines with their rows and distance (which camera it is), and `panel
pixels per sampled eye draw: engine-joined J` with J > 0 when a pool-family rig record is
on screen. If the new code never ran: no `camera rule:` clause (the old line), and walking
frames dropped for `scene rows 270..275 changed` as in flight 5. Walkers (vs_F516) are not
a pool family: a walking NPC still blurs until phase 2's previous bone palette.

### 2026-09-23 -- The station (eye run 143416): the joined motion is exact; half the station is drawn by no keyed shader

**The flight.** Build e1bf2dbd, the trim off, input 2037x2038, DLSS output 4074x4076; the
station from ~14:33:30 to 14:34:45 (log 142856); the eye run at 14:34:16 turned
diagnostics on for the 14:34:27 window (lines 18831-18843): emit joined 993,605 pool
records in 30 s, with motion 986,546 (99%: the station's parts move every frame), masked
122 (gaps 107, none in bursts); movers joined 371.2 records a frame against the
tracker's 1424.2; eye-frames 4796, all bound and given, invalidated 0; pixels an
eye-frame: engine-joined 297,566, masked 0, not a rig record 2,616, STALE 136,878,
corrupt 0 (32 readbacks); stock pixel shaders: ps_B7D5 (vs_EB52), ps_CB42 (vs_DE54, new
here), ps_451A (vs_61AE). Sean: the station's rotation blurry. Two hypotheses: (A) the
blur is on surfaces no substituted draw wrote, which keep the camera term on a turning
station; (B) the joined motion is wrong at range (engineReproject's precision at
kilometres, or a stale previous pose).

**The dump.** The decision crops (D00..D15: 1400 x 1400 of the input, motion, predicted
prior depth and the path per pixel), motion.csv (each frame's camera delta, tangents,
jitter), the ledger (pool copies, instance streams, 40-byte draw rows with the pixel
shader). The camera term, recomputed from motion.csv (P_prev = cameraR P + cameraTv, no
jitter, vector = previous minus current pixel), matches the world path's vectors to
0.0001 px median, so the arithmetic below is the pass's own. Frames 28800 / 28801:

- Station pixels (depth over 50 m; the station 4.6-8.5 km away): engine path 152.7k /
  152.4k, camera term 187.0k / 187.3k -- 55.1% of the station on the camera term.
- The engine pixels' vectors minus the camera term, fitted by ONE rigid motion in view
  space (trimmed least squares): residual 0.0022 / 0.0029 px median (p90 0.017 /
  0.007), 0.1125 / 0.1078 deg a frame. The records' own two pose blocks (quaternion now
  at +8, before at +312): 0.1062 / 0.1084 deg a frame. Neighbouring engine pixels on one
  surface step 0.002 px (p99 0.01): no float noise at 4.6-8.5 km.
- What the camera-term station pixels miss under that motion: 0.31 / 0.35 px a frame
  median, p90 0.63 / 0.70 -- the blur.

ruled out: the station's joined motion wrong at range (B: precision in the world
reconstruction at kilometres, or a stale previous pose), because the engine pixels are
one rigid motion to 0.002 px at the records' own rate, 0.106-0.113 deg a frame.

**What draws the camera-term half (the ledger, frame 28800, per eye, by the records the
instances take).** The station's pool draws: vs_436193B352A2897E + ps_16940F576006BE65
(136 draws, 547 instances at 7.3 km, 589k indices x instances -- the largest set in the
eye pass, and no family), vs_EB52 + ps_3434 (264, keyed), vs_DE54 + ps_E46E (145,
keyed), vs_DE54 + ps_CB42 (108 at 5.5 km, 71k, STOCK), vs_AACF (51, keyed),
vs_889A5279E68F0672 + ps_B46E52A1E0B2F39C (37 at 8.5 km, no family), vs_EB52 + ps_CB9F
(15), vs_66DE (9), vs_61AE + ps_451A (7 at 5.8 km, STOCK), vs_61AE + ps_FC43 (6), vs_EB52
+ ps_9ABF (3), vs_DE54 + ps_03B1 (1). 700 of the station's 1193 pool instances are drawn
by shaders that write no slot, vs_4361 alone 547. Attributed by records, not rasterised;
per-object-motion.md (the forty-third flight) read vs_4361's position path whole: the
record's own quaternion, scale and position. ps_B7D5 (vs_EB52) and ps_91F8 (vs_DE54)
draw the cockpit (records 1-2 m away): the commander's legs, decided, not the station.
So A holds, and its main term is a whole pool shader outside the families, not the
stock pixel shader first named.

**The fix (branch claude/station-keying).** ps_CB429E043DBB2506 keyed for vs_DE54 and
ps_451A82D4DD1BA254 for vs_61AE (src/d3d11/engine_velocity.cpp, kFamilies); both pairs
pass the corpus identity harness on the real bytecode (edvr_logs\shaders, 09-06): 40,960
texels, 0 mismatches, MRT6 8192 checked, 0 bad, each. EDHM or a game update changing
either hash leaves it stock by name, as for every keyed pair. vs_436193B352A2897E
derives (slot v0.y, SV_Position v5: the DATAID.y pattern of vs_EB52) and so does
vs_889A5279E68F0672 (v0.x, v4) -- the harness now checks both, derive only -- but
ps_16940F576006BE65 is in no dump, and nothing is keyed without the harness.

**Coverage.** The station: VERIFIED where a keyed shader draws it (exact, the rigid fit
above); NOT COVERED: vs_4361's surfaces (the bulk of the camera-term half) and vs_889A's,
until their pixel shaders are dumped and proven.

**What a flight shows.** At a station on this build, the family lines read
`vs_DE545DC8EE4FBB87: live; ... patched [ps_E46E3E4832B2FDB0,ps_CB429E043DBB2506]` and
`vs_61AE8EB05FDC18DD: live; ... patched [ps_FC43E42710010343,ps_451A82D4DD1BA254]`; with
diagnostics, engine-joined up and stale down by the keyed share -- a modest one, vs_4361
still dominating. With advanced.glare_shader_dump = 1 for one session (restart to arm),
edvr_logs\shaders\ps_16940F576006BE65.dxbc appears, and vs_4361 becomes a family through
the harness. If the new code never ran, the family lines still name ps_CB42 and ps_451A
"left stock". Rigs: engine_velocity_test 967 checks (the gate), 1254 with --corpus
(eleven real pairs identical, two derive-only).

### 2026-09-23 -- Flight 6 (153446): the station's biggest family keyed

**The flight.** Build 1eae654d (the on-foot and station fixes merged), the shader dump
armed; eye 4074x4076 output from 2037x2038 (DLSS performance, exactly 50%), the 2D screen
5088x2862. Docked at a station (still blurry turning: vs_4361 unkeyed, as expected),
disembarked into the HANGAR on foot 15:37:20-15:38:12. The dump wrote
ps_16940F576006BE65.dxbc (9148 bytes, 15:36:12) beside vs_436193B352A2897E.dxbc (09-06).

**The station's families, through the corpus identity harness** (engine_velocity_test
--corpus edvr_logs: derive, patch, reflect, create, then stock and patched drawn over the
same inputs, SV_Target0..3 and depth compared bit for bit, MRT6 checked against the slot):

| pair | result |
|---|---|
| vs_436193B352A2897E + ps_16940F576006BE65 | 40,960 texels, 0 mismatches; MRT6 8192 checked, 0 bad (slot v0.y, SV_Position v5) -- KEYED |
| vs_889A5279E68F0672 + ps_B46E52A1E0B2F39C (the station's) | 40,960, 0 mismatches; 8192, 0 bad (v0.x, v4) -- KEYED |
| vs_889A5279E68F0672 + ps_EBA95E15B0A66102 | 40,960, 0 mismatches; 8192, 0 bad -- KEYED |
| vs_DE545DC8EE4FBB87 + ps_91F8937EDA723663 | refused by the patcher: "position input register holds another semantic" |
| vs_DE545DC8EE4FBB87 + ps_A6070F9DD1CFB601 | refused, the same |

Two new families in src/d3d11/engine_velocity.cpp kFamilies (nine now; kMaxFamilies 10,
four pixel shaders a family). The refused two stay stock: the family's SV_Position sits
in v4 of its vertex outputs, and these two pixel shaders declare another semantic in
that register -- a patcher that finds SV_Position by semantic in the pixel shader's own
input signature would take them; not built. The rig now splits its corpus into keyed
pairs (each must pass: 14) and candidates (tried and reported, never failing the run:
the refused two), and takes each call's result before its check -- a reason written by
the call was printed from a dangling pointer before (the first run's garbled FAIL).
engine_velocity_test 1008 checks (the gate), 1373 with --corpus.

**What a flight shows.** At a station: `engine motion: family vs_436193B352A2897E: live;
substituted N binds ...; patched [ps_16940F576006BE65]` and the same for vs_889A; with
diagnostics, stale per eye-frame down from 136,878 toward the stock remainder (ps_91F8 and
ps_A607 are the cockpit's and the on-foot views', not the station's). If the new code
never ran: no family line for either.

### 2026-09-23 -- The hangar (flight 6): the source named without terrain; the layout census by target

**The hangar.** On foot in the station's hangar (15:37:20-15:38:12) the source path never
engaged: no "on-foot source slot target created" line and no "engine motion: on foot:"
line in the whole log, and no "screen motion:" line either -- screen motion's own map
hangs on the same naming -- while the world screen was held ("the journal: on foot; the
screen's depth: 420 draws a frame now, 3422 at most", 15:37:46). The source was named
only by a terrain draw (vs_ACE405F428C17EF6) or a settlement scene draw
(vs_4435F2E50020E7F3) into the screen's colour and depth; a hangar has neither.

**The naming now (src/d3d11/screen_motion.cpp:260, screenMotionSource).** Terrain or a
scene draw names first, as before (the world camera by construction). Where neither has
named it for kTerrainHoldFrames (2) frames, the pool family draws (the engine's families,
minus weapon_motion's first-person weapon and tool shaders) into each depth of the
screen's size (vscreen's panel size: 5088x2862 this session) are counted per frame
(screenPoolDraw, :247, each view resolved once a frame), and LAST frame's busiest names the
source at its first such draw this frame, through the same colour and depth checks. That
draw's VS b1 is the source camera for both screen motion's camera term and the engine's
per-draw camera rule, which holds every other pool draw to it as on the walk; the
first-person shaders are skipped because their camera is not the world's (flight 5's
vs_AACF-first frames). Signals in the log: once, "screen motion: no terrain or scene draw
names the on-foot source here (a hangar): it is named by its own depth -- the WxH depth
that took the most pool family draws last frame (N) ..."; on the on-foot line, `camera
rule: namings N (by terrain or a scene draw T, by the screen's own depth S; rows not seen
U)`. When NEITHER names it, after 90 frames of the 2D screen: "screen motion: the 2D screen
showed for 90 frames and nothing named its source -- no terrain or scene draw, and <no
pool family draw went to a depth of the screen's size | X a frame went there without
naming it (its colour target or depth format refused)> -- so no screen motion map is made
and the engine's on-foot path stands idle", and "named again after N" when it resumes.

**The layout census (src/d3d11/depth_probe.cpp:151 LayoutRecord, :178 layoutSample, :222
layoutPickRecord).** Its 15:37:53 line read "the busiest depth target (now #0 2048x1024,
1045 draws last frame) ... viewports [(0,0) 1024x1024 x298, (1024,0) 1024x1024 x263,
(0,0) 5088x2862 x2253]": the samples were pooled across frames while the busiest target
alternated between a 2048x1024 shadow atlas (two 1024 viewports) and the 5088x2862 screen
(up to 3422 draws a frame), and the line named only the current one -- 561 atlas samples
and 2253 screen samples under one name. Each sampled target now keeps its own record
(frames, samples, viewports, scissors, the colour beside it), and a sample whose viewport
lies outside its own target is counted ("N samples with a viewport outside it (drawn with
another depth than the one counted)"), so a per-draw misattribution, if one exists, shows
as a number instead of a mixed list. The probe also HOLDS each tracked depth view now
(:583), released on eviction and shutdown: targets are matched by the view's pointer on
every draw, and a view the game released could come back at the same address for another
texture, its draws counted under the old entry's size. The naming does not read the
census: it counts its own pool family draws per screen-sized view.

**Rigs.** screen_motion_test (the hangar): the first frame only counts; the second names
at the first world pool draw, with the ScreenDepth signal and the draw's VS b1; a shadow
atlas (another size, depth only, two half viewports) that takes MORE pool draws is never
counted and never names; a first-person tool shader never names; 90 screen frames with
only the atlas are counted and said; a naming clears it; terrain names with its own
signal and holds the fallback off even when a pool draw precedes it. depth_scene_pick_test
(the census): a screen-sized target and an atlas taking turns as the busiest keep their own
records (the screen's holds only its viewport, the atlas's its two halves), a draw with a
viewport outside the atlas is counted as such, a tracked view is held and an evicted one
released. engine_velocity_test S3: namings by the screen's own depth counted under their
signal, the frame given, nothing dropped. Screen motion 56558 checks, depth_scene_pick_test
173, engine_velocity_test 1011; contract 261.

**What a flight shows.** In a hangar: the "named by its own depth" line once; `engine motion:
on foot: ... frames dropped: none; screen views asked A, given A`, `camera rule: namings N
(by terrain or a scene draw 0, by the screen's own depth N ...)`; the "screen motion GPU"
lines (the map is made); and engine-joined panel pixels when a pool family mover is on
screen. If the new code never ran: no on-foot line in a hangar, as in flight 6. If nothing
names the source: the "nothing named its source" line with its reason.

### 2026-09-23 16:27 flight (f05c84bf): the hangar and the station, FLOWN OK

Build f05c84bf (v0.17.0-458-gf05c84bf), HMD Quality 0.5 into a 4074x3938 eye, trim off,
ui_quality 1.25, DLSS performance mode at exactly the floor (gfx log
edvr_gfx_20260923_162703.log). FLOWN OK: no faults or stand-downs anywhere in the log.

**The station.** Both rotating families are live and substituted: vs_436193B352A2897E with
ps_16940F576006BE65 (2982 draws per window, then 29422); vs_889A5279E68F0672 with
ps_B46E52A1E0B2F39C (360, then 2206).

**The hangar.** No terrain or scene draw names the on-foot source here (a hangar): it is
named by its own depth instead -- the 5088x2862 depth that took the most pool-family draws
last frame (1156). The source slot target was created at 5088x2862, R32G32 (116.5 MB). The
on-foot windows read "frames dropped: none; screen views asked 764, given 764" then "asked
5398, given 5398", invalidated 0. The world screen held on foot: the journal shows the
screen's own depth at 553-3648 draws a frame.

Sean judged the station rotation and the hangar good ("All looks good to me now").

### 2026-09-23 -- Teardown: the estimates and the legacy tracker retired

Sean's decision: tear down what the engine records superseded. Two commits on
claude/teardown-estimates-tracker.

**A.** Every path that estimated a per-object transform is gone, with its four keys: the
body, second-body, stepped-part and ship paths, mesh_motion's record pairing (records
paired across frames by what they look like), object_probe.cpp's rigid fit and the
rigid-owner promotion. per-object-motion.md's retirement entry has the list. The engine
path, the mover mask, terrain and holo motion, the screen motion map and the eye run's
ledger stay.

**C.** The legacy kinematic tracker is gone: kinematic_motion.*, its eval-hook observer and
gate want, its Present-time population scan, its two cost lines and kinematic_motion_test.
It was diagnostic-only since the performance round, and the emit needs nothing of it. The
eval hook set (kinematic_eval_hook.*: the emit bracket, the probe, the scheduler stack
probe, the static prop gate, the cull gate probe and the settlement LOD governor's hooks)
stays whole, and the previous-pose certification lives in the emit's own table. The
census of one record in eight still counts movers evaluated but not drawn.

The movers line now reads "engine motion: movers joined N records/frame (moving rig
records the emit wrote a previous pose for); eye-frames ...", without "against the
tracker's N moving records/frame" or "(the tracker, diagnostic-only, was off)". The
"engine motion: tracker (diagnostic-only) cost" and "tracker off" lines are gone.
