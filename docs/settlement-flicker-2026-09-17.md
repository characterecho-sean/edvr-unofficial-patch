# Settlement flicker + frame time (2026-09-17)

## Status

- Reversing (2026-09-19): KinematicRig pipeline decoded statically (Ghidra
  headless, exe hash-verified), no flight spent. State machine rig+0x380
  (4 = render-ready); dependency tokens +0x188/+0x1A0/+0x1B8 with -1 =
  resolved, paired interfaces +0x190/+0x1A8/+0x1C0; +0x1C0 slot +0x90 =
  readiness, slot +0x170 writes parent-relative 3x4 pose to rig+0x58.
  0x4321940/0x4320340 dispatch via named job "Kinematic::UpdateRenderDataJob"
  (vtables 0x145591C98/0x145591DC0) through a no-.pdata thunk table. Waves
  4-12: record filler FOUND (FUN_14432A4E8, fragment of undecompilable 30 KB
  parent 0x4329984): fills collection+0x280 stride 0x2F0, creates pose ctx
  record+0x290 (0xC0, registry collection+0x2E0) and content hash record
  +0x268. CORRECTION: the change predicate is at record+0x2C0, not pose
  +0xB0 (undefined4* x4 scaling misread); skip signal = render-record
  (stride 0x6A0) +0x688 bit 0x1000, honored only when predicate slots say
  unchanged (slot+0x70 == 0) and tracking (slot+0x58 != 0). Dirty pipeline:
  FUN_14431E860 float-channel epsilon bitmap -> FUN_14432CCC0 per-record
  mask-AND + world-bounds recompute (record+0xB0..0x12C). Predicate class
  still unidentified (vtable slot +0xB8 install in filler). Dumps in
  analysis\decomp. Waves 11-17 (offline, 21:58 entry): collection+0x18 =
  backing-owner pointer, written once by collection ctor FUN_14430A060
  (param_3); the collection aliases seven owner arrays (+0xA0..+0x340) and
  sizes tables from owner+0x388. Owner == rig REFUTED in flight (05:05
  entry: one shared global owner for all 64 collections, backptr 0,
  state != 4); discriminator was owner = *(collection+0x18),
  *(owner+0x348) == collection, owner+0x380 == 4. UpdateRenderDataJob
  chain mapped: FUN_14431AFE0 accumulates
  LOD-entry masks -> FUN_144331300 per-frame LOD evaluator (epoch
  collection+0x90 vs record+0x1B8 -> refresh FUN_14433C870) -> traversal
  FUN_144312040 -> FUN_14433DB20 per-record world updater (translation
  record+0x170, center +0x240, bounds current +0xF0 copied to previous
  +0x1C0). The traversal mask is recomputed per frame from distance and
  visibility, not a stored dirty flag.

- State: flight 134252 passes the guard but has zero owner links. Seventeen
  saved paths use worker dispatch, with no upstream KinematicRig ancestor; one
  truncates at recursion. Worker descriptors retain collection/registry but no
  direct outer pointer. Submission capture is needed; filtering off. The owner
  discriminator flew (043344, 05:05 entry): collection+0x18 is a SHARED
  GLOBAL, not the rig — owner==rig refuted, submission capture NOT retired.
  Replicated on 050820 (05:15 entry); JSON quote fix flight-proven there.
  The rig-link hook flew (054002, 05:45 entry): rig -> collection via
  *(rig+0x348) CLOSED (58/58 job-0 join, one shared global owner);
  ~1,572 stable rigs, one collection each, one dispatch per rig per
  frame. Job-1's batch arg is NOT a rig collection (0xa12… family).
  Pose/identity capture flew (083323, 08:50 entry): the mesh-frame
  counter is REFUTED as the tracker clock (3 ticks in ~51 rendered
  frames; gap/node-change detectors disabled by the stall — zero
  identity events is uninformative). Tracker clock = EDVR's own
  present/temporal-pass frame index — LANDED in
  v0.17.0-107-g5fe9c004-dirty (09:35 entry) and flight-proven
  (094158, 10:05 entry: mesh==present-2 1:1 all 52 presents; eval
  fan-out ~11x/record/frame, dedup mandatory; node-change zero now
  informative — pointer identity stable while present). Drone group
  re-found per-frame (rotates every frame); +0x130 4x4 static on a
  third flight. Quaternion decode validated (unit norms);
  sign canonicalization required before differencing. Capture v2 flew
  (103339, 10:38 entry): arm-seed fix CONFIRMED (zero gap events,
  identity log unsaturated, re-log backstop unused), pose_samples
  24,520/32,768 survived the window, clock mesh==present-2 1:1 over 52
  presents (second flight), drone per-frame series complete (11/11
  members, 51/51 frames). Rotating-in-place class quantified: 292
  records translate <0.1 m yet rotate 0.8-1.6 deg/present every
  present — static labels must cover rotation, now flight-evidenced.
  One terminal zero-record present = capture teardown (a mid-window
  one would have fired ~3,008 gap events; none fired).
- Open: exclude proven-static objects before expensive EDVR motion work while
  retaining camera/world motion. Classification must be cheaper than the work
  removed and detect new movement without stale labels. The separate coarse
  body-on-buildings and 16:48:57 camera-pulse problems remain unresolved.
  Stage B (ownership coverage + compose veto): the +0xB0..+0x130 block
  decoded as the world TRANSFORM, not bounds (13:05 entry) -- world
  centre is +0x240, the extents are not yet located; a widened raw
  capture is the next instrument.
- Build: v0.17.0-119-g6899d0ff-dirty installed to frontier (CFDEFA79),
  the nine 2026-09-20 review fixes on stage A (12:45 entry),
  flight-proven by 125207 (13:05 entry: tracker clean, bounds block
  decoded); supersedes v0.17.0-117-g0d042f5c-dirty (2AAA67BF). The
  064047 flight proved record+0x170 is the per-frame world position
  (06:45 entry). INIs unchanged; fix.engine_motion defaults off and
  must be set on in the live ini for the tracker (the 12:46 flight
  had it under [advanced] -- dark, not evidence). Flight check:
  --expect-build v0.17.0-119-g6899d0ff-dirty.
- Environment: latest capture is Quest 3 / VirtualDesktopXR / RTX 5090 / 90 Hz,
  DLSS K, input 1996x2121 and output 3072x3264 per eye. Earlier 2481x2121 input
  timings are not directly comparable. Installed DLSS Windows file/product
  version is 310,7,0,0. This is landed cockpit; station rotation/on-foot still
  need separate validation. No draw suppression is active.
- Timing: earlier W7 has CPU/GPU medians 18.394/22.090 ms, separately measured
  elapsed spans. Its 30-second sample ends before eye_190309 begins; capture
  changes the scope during drain. The 19:03:24 ownership window has 18,751,925
  candidates, 6,576 accepted and 17,734,402 cache declines over 1,800 frames.
  Its sampled 0.390 us/check implies about 4.06 ms/frame of added gate work,
  excluding lock acquisition/unsupported families. Earlier low-cost benchmark
  windows include menu/loading and must not be called comparable settlements.
- Capture: 134252, mesh/scene 11084: 126 draws, 29 visible records/153,957
  exact-depth pixels. All 44,303 writer calls completed without faults or caps;
  44,039 ownership attempts report unwind failure, retaining 18 unique traces.
  The 512-record admission limit still prevents a complete scene census.
- Earlier static opportunity: 501/512 records have exact unchanged raw pose,
  across 115/124 whole draws; all 501 also have changed scene origin. Of these,
  446 leave no motion-coverage pixels. These are conditional opportunities, not
  safe skip counts, unique scene-object counts or a forecast of speedup.
- Visibility: 25408 selected draws pass no depth/stencil samples; only 82
  produce zero clipped primitives. This favors investigating depth/stencil
  rejection over simple frustum rejection in the selected material families.
  Held ordinal cohorts bias these rates; they are not a population estimate,
  unique mesh count or removable fraction. The prior broad census found about
  76% zero-sample draws and roughly 23000/19000 original calls/frame.
- Limits: binding changes break 78.51% of landed and 50.92% of on-foot
  follow-ups. The composite counter cannot identify which binding changed.
  Model changes are confined to pose fields, but comparisons can concern
  different objects. IDs, draw order and identical payloads are not proven
  persistent object identity. Coarse family aggregates remain valid despite
  detail-bucket overflow; query times cannot be extrapolated as FPS savings.
- Earlier CPU samples imply 1.241 ms/frame in the mesh hook, with 0.970 ms
  fast/scene/eye/cap checks and 0.202 ms accepted work. These are cost
  envelopes, not savings forecasts; other hooks and Elite's earlier CPU work
  are excluded.
- Prior DLSS work: static identity across rebases and first-visible/moving
  history remain unresolved. Typed fusion was not useful; precomputed capture
  controls suggest only 0.21-0.35 ms per captured subset. Earlier evidence and
  ruled-out batching/cache/screen-motion hypotheses remain in the journal.
  Motion tables remain 512 records/eye and 64 source records; station rotation
  and independently moving ships still need separate validation.
- Next: capture owner-to-collection provenance before task submission at
  431AFE0, and retain worker descriptor context from the existing lookup hook.
  Validate both sides without collapsing ambiguous/reused pointers. Bundle
  normal stack-end and repeated-RIP handling with that instrument; no rerun
  needed yet. Ownership would still not establish a static object.

## Journal

### 2026-09-17 -- eye_165144 read offline (first pass, corrected below)

Tools: `eye_run_shimmer.py`'s `shift_est`/`shifted` for alignment, a hand
decoder for `Mesh.bin` (`EDVRMSH1`, header `<8s II`, 240-byte records `{uint4
key[8]; float4 clip[3]; float4 map[3]; float4 meta}`; meta.x = valid at float
56, meta.w = matched at 59 -- no repo tool reads it), `eye_inputs.py` for the
EDVRTEX1 channels, `eye_bmp_to_png.py --crop`. Crops: C 1400x1400, T 2155x2154
(not 1868), L0 whole eye; T's origin in L0 is (958,620) by residual search, C's
(623,403) by the same convention.

Q1 as first read: baseline 151043 T mean|diff| 0.55-4.95 over 15 pairs
(its worst, T12->13, >8/255 on 14.1% of pixels); 165144 T00->01 10.74 and
T02->03 11.23 with the same raw pairs at 7.16/4.18. SUPERSEDED: the whole-frame
alignment reported T shifts of 0.02/0.03 px for frames 1-2 and +8.02 at frame 3
while C moved 5-6 px at frame 1 -- the correlation locked onto the cockpit; see
the per-band table below. The per-pixel mask built on those two pairs (6.84% of
the crop; holo 28.5%, mesh 8.9%, terrain 7.8%, uncovered 4.4% flicker rate; the
8x MV gap on mesh-covered pixels) inherits the misalignment and is not evidence
of anything. Method note for the next dump with a cockpit in view: align per
band, or mask the cockpit, before differencing; and read the tile argmax, not
the tile centre, when localising.

Q4 (source, stands): `meshPixel()` is tried first and wins on success
(`temporal_shader_source.h:678-683`); it returns false on no coverage, index >
512, UI-covered, stale coverage depth, or `meta.w != 1` (476-488). `meta.w` is
set only by `match()`'s `counts[0]==1` branch (`mesh_motion_shader.h:75-85`):
zero candidates and several candidates both stay 0 and are indistinguishable in
the record. Past the cap (`mesh_motion.cpp:157`) a draw is not entered at all
and its pixels rasterise as index 0. On failure the pixel takes the
camera/depth reprojection (684-693) unless body/ship/terrain/holo claims it
(705-769).

Q5 (logs, stands): census at the dump (3 frames): 59,025 draws, 16,563
offscreen, 18,842 copies, 1,072 dispatches, 42,641 truncated (the buffer
overflows at this volume); 15:10 baseline: 1,710 draws, 7,011 copies, 327
dispatches. Per frame: draws 570 -> 19,675, copies 2,337 -> 6,281, dispatches
109 -> 357. `terrain motion GPU` at 16:51:45: hook CPU 0.40 us/call, 0.036
ms/frame, 46/46 patches matched; holo 18/18. Quest 3 log (`--nth 0`, 17:00,
VDXR, 2307x1652 -> 3072x3264): cpu p50 7.1 -> 13.4, gpu 10.5 -> 16.3 ms at
17,638-18,878 draws -- same shape, not the Pimax runtime's doing.

### 2026-09-17 -- review: alignment, record visibility, native pacing

Per-band phase correlation (four horizontal bands, top to bottom; dx,dy in each
crop's own pixels; scratch `band_shift.py`):

| pair | C (input px) | T (output px) |
|---|---|---|
| 00->01 | -3.9,+6.6 / -3.0,+5.5 / -2.8,+7.1 / -0.8,+5.9 | -5.0,+10.0 / -3.4,+7.3 / -3.4,+10.6 / +0.0,+8.5 |
| 01->02 | +1.2,+1.0 x4 | +1.3..+1.8,+1.0 |
| 02->03 | +0.8,-0.9 x4 | +1.2,-0.3 |
| 03->08 | 0.1-1.9 px steps | 0.3-1.9 px steps |

T moves with C at x1.54 from the first pair; the lower bands' smaller x in C is
the static cockpit weighting the band. Cumulative x over 8 pairs: C x1.54 =
+4.0, T = +3.1 (T is the jitter-free output; C carries the +-0.4 px jitter in
`motion.csv`). Nothing holds still and snaps.

Record visibility (`mesh_visibility.py`, MeshCoverage x Mesh.bin): records 512,
valid 504, matched 36; 669,391 px covered (11.47% of the eye); records covering
>=1 px 81, >=100 px 12, >=1000 px 8, zero 431; origins in front 509, inside the
frustum 501, zero-px records inside 427; pixels on matched records 40,811, on
unmatched valid 2,366, on invalid 626,214 (records 9, 8, 3, 1, 4, 2 at clip w
0.4); 83 distinct key groups, 483 records sharing a key, 468 of them valid and
unmatched.

Mesh path per stereo frame from the `mesh motion` totals (1800-frame marks):
terrain window 16:49:46-16:50:06: 40 reissues, 40 instances, 10 batches;
approach 16:50:48-16:51:10: 193 / 769 / 14; settlement 16:51:10-16:51:41: 212 /
1,023 / 85 at 6.27 us/batch, coverage 2.93 us sampled, match 21.5 us/eye. Cap
line at 16:50:55.318.

Native log `native_submit_phases`: wait_frame p50/p95 9.2/10.1 (w1), 9.1/10.1,
9.3/10.0, [w4 pacing=1 on foot, pacer_block 6.1], 4.4/6.0, 5.5/6.6, 4.8/5.9
(w7), then 0.22/1.7 (w8, 16:50:58), 0.23/2.2 (w9), 0.22/0.33 (w10, 16:52:02);
output 3329x3394 per eye with the trims. `native_pacing_summary
deferred=1283,synthesized=1283` is the on-foot stretch (window 4), not the
settlement.

Static-mesh exclusion, the plan (not built). The transform lives in the game's
pool buffer (VS t33, 336-byte records, indexed by the instance-id stream at
slot 0) and is read only by the capture shader (`mesh_motion_shader.h:28-36`);
the record keeps the camera-dependent clip matrix, which is why `match()` needs
the fuzzy direction/distance test and ties on identical props. Stage 1: keep
the raw pool transform (scale, packed quaternion, position: 8 uints of the
record's 11 spare in `key[5..7]`) and match bit-identical transforms first, the
fuzzy test only for records that changed -- unique by construction, ends the
ambiguity, no cost change. The dump readback then logs how many valid records
were identical to a previous one: ~0 means the game stores positions
camera-relative and Stage 2 is dead before it is built. Same build: a per-frame
count of instances refused by the cap, a CPU timer on `meshMotionDraw` and
`flushCapture`, a per-frame draw-hook CPU total (20k draws/eye is where the
unmeasured CPU would be). Stage 2: shadow the pool and id-stream writes at
Map/Unmap and UpdateSubresource (the terrain fix's technique;
`meshMotionResourceWritten` already receives the byte ranges), hash each draw's
instance transforms against the previous frame's set, return before the reissue
when every instance is identical: no second rasterisation, no record, no
capture dispatch, and the cap left to the movers. A readback-based skip (draw
keys static N frames ago) would be cheaper to build but hands a newly moving
object camera-path vectors for the latency -- not chosen. Visibility ranking of
the cap (tally coverage per record, deprioritise draw keys that covered
nothing) waits until the static skip has been measured.

ruled out: EDVR-side flicker in this dump, because per-band alignment puts
every T pair inside the baseline's range and T follows C x1.54. ruled out: a
history reset at the capture, because dlHistory=1 and jumped=0 on all 16 rows
of motion.csv. ruled out: mesh-motion ambiguity as the buildings' flicker,
because the buildings are not on the mesh path (73 world records, 0.74% of the
eye).

### 2026-09-17 -- mesh optimization measurement pass

Sean authorized the measurement build after review; all test installs for this
work go to Frontier. The rendering decision is unchanged: the same draws,
512-record limit and fuzzy matching feed the same temporal consumer. No object
is skipped for being static or previously invisible, and no live mapped-pool
CPU shadow is introduced.

Two corrections to the earlier plan are gates, not optional refinements.
Raw-static does not imply safe camera fallback: `temporal_shader_source.h`
selects head motion inside the distance split and may then apply ship/body
membership. Losing mesh coverage can therefore change a nearby world surface's
vectors. The older `per-object-motion.md` journal (pool probe, 2026-09-08) also
rejected reads of the mapped write-combined pool as millisecond-scale work; the
successful small terrain-buffer tee does not price this different allocation. A
future CPU shadow needs its own measured extraction cost and complete write
visibility.

ruled out: an unconditional raw-static early return, because removing coverage
can select head/ship/body motion instead of world motion. Identical pose plus
geometry keys is an exact-pose candidate, not proof of unique object identity
or stable slot numbering. Keep previous transforms even for skipped static
objects so the first moving frame has history; freeze the history across both
eyes. An origin change is reported separately, not automatically labelled
object motion.

The measurement captures the six raw pose words, three rebase-origin words and
original draw grouping in unused mesh-record fields. An explicit eye run saves
current and previous records together, with a schema marker written only after
the pair succeeds. The previous file is explicitly empty on a first frame. The
temporal pass also saves the corresponding fallback parameters; body/ship grids
are not captured, so membership-dependent results remain unknown. The existing
coverage and final scene-depth inputs distinguish covered records from pixels
still depth-visible.

The read-only `tools/mesh_motion_probe.py` compares the admitted records by
persistent geometry key and exact raw pose, counts duplicates, fully unchanged
versus mixed draws, origin changes and coverage. These are upper bounds on
avoidable work, not an instruction to skip. It cannot describe records rejected
by the cap. The build gates its self-test alongside the GPU mesh rig.

Runtime counters separate early cap rejections from sampled, fully checked
eligible rejections. Sampled mesh-hook/capture CPU costs, capture flush reasons
and batch sizes identify whether the next change should remove whole reissues
or reduce dispatch boundaries. The full draw-hook CPU window reuses its
existing sampled clocks, so scene-wide hook overhead is visible without timing
every draw. Zero samples/unavailable captures are distinguished from measured
zero cost.

Validation: full absolute-path build passed, including 1,484 mesh WARP checks,
the real exporter-to-analyzer fixture, probe self-tests, all configured rigs
and the 249-key config contract. Rebuild the clean commit before installation
so the DLL version matches the checkout. Use `EDVR_JOBS=6` in the environment
and invoke `build.bat` by absolute path without arguments: the existing
`--jobs` parser shifts `%0` and breaks the later runner's `%~f0` path. No
parser change is included in this measurement pass.

Next flight: Frontier, current native OpenXR runtime, Pimax Crystal Super, DLSS
K, with the resolution and active trims recorded in the new log. Hold each
useful scene for at least a minute: settlement in ship/SRV, settlement on foot,
rotating-station approach with traffic, then the slot/docked view. Take an eye
run at the settlement and station. Include a short stop/start and an occluded
ship emerging into view. Use the same settings as the baseline where practical;
do not count the explicit capture stall as steady-state frame time. Start log
analysis with `tools/edvr_log.py --target frontier --expect-build HEAD
--version` against this checkout.

Acceptance for a later optimization is lower steady-state CPU/GPU cost with
correct first-moving-frame and newly visible motion, not simply a lower record
count. Freeing static records may admit previously capped movers, improving
motion coverage without reducing the final count. The entire measured 1.2-1.5
ms mesh cost is the current GPU ceiling; even removing all of it from 13.4 ms
would still exceed the 11.1 ms refresh budget.

### 2026-09-17 -- Frontier measurement flight, three completed runs

The gfx log `edvr_gfx_20260917_191929.log` and native log
`edvr_openxr_20260917_191930_687_7888.log` both match `v0.17.0-1-gc7c4737`,
verified with `tools/edvr_log.py --target frontier --expect-build c7c4737` (and
`--tag openxr`). The user reported three runs with three captures each; this
matching session contains three completed paired eye runs and three fallback
markers, not nine. The user confirmed 192128 is on foot. Overview images
identify 192246 as the ship hovering above the settlement and 192513 as the
cockpit approaching Macleod Market, an Orbis starport at about 5.92 km. Each
paired run has sixteen treated crops and seventeen raw crops, which are not
independent scene captures.

Environment: native OpenXR, Meta Quest 3 through VirtualDesktopXR, 90 Hz, DLSS
K, 2307x1652 input / 3072x3264 output per eye. On-foot screen/weapon motion
logs a 5120x2880 source. Native pacing changes from deferred at 19:21:10 to
runtime at 19:22:21. Keep these numbers separate from the earlier Pimax
baseline.

| Eye run | Frame / diagnostic window end | Hook CPU mean, ms | Approx. mesh-draw CPU/frame, ms | Stereo accepted draws = capture batches/frame | Cap refusals, draws/frame | Nearby native CPU / GPU p50, ms |
|---|---|---:|---:|---:|---:|---:|
| 192128, on foot | 13629 / 14400 | 2.950 | 0.601 | 0 | 0 | 7.759 / 12.592 |
| 192246 | 19192 / 19800 | 6.160 | 1.924 | 249.37 | 9146.16 | 13.193 / 16.346 |
| 192513 | 31419 / 32400 | 1.630 | 0.937 | 337.61 | 106.77 | 3.490 / 7.095 |

CPU estimates scale the sampled draw time by 256 and divide by 1800 frames.
They include early refusals and are sampling estimates, not stage-exact
attribution. Flush CPU estimates (0.479 and 0.553 ms/frame in the latter
windows) overlap draw time; do not add them to it. Hook CPU uses independent
1/16-frame sampling and excludes forwarded game-draw time. Diagnostic flush
counts reset each window: 434459 input-change + 14400 write/map at 19800, and
581432 + 26272 at 32400. Only cumulative GPU/reissue counters are differenced.
Each latter window accepted 1024 records/frame across both eyes. Cap counts
precede full eligibility; the bounded 32-draw/eye samples were eligible but
cannot establish the population. The last native benchmark ended about three
seconds before 192513 and spans approach, so it is not a steady station-only
measurement. Explicit eye runs stall and are not an isolated performance
benchmark.

192128 has no mesh/previous-mesh marker pair and no captured rigid meshes; the
adjacent 14400 and 16200 windows have zero candidates, accepted records and cap
refusals. The existing on-foot screen camera/depth projection and
original-vertex weapon motion were active. A static rigid-mesh skip cannot
remove capture work which this path was not doing; its many hook calls and
screen/source rendering need separate attribution.

192246 has 512 records, 504 valid and eight invalid, in 120 draw groups. Every
valid record has a unique compatible exact raw-pose match; 112 whole draws
contain those 504 records. All exact pairs change scene origin, so the
analyzer's stricter same-origin set is empty. The shader subtracts scene[275]
from pool position: an origin change does not by itself mean object motion. The
18 singleton-key pairs share an origin delta of about (+0.000011, -0.000015,
+0.000041), while their raw positions are identical. Clip bases change and
cannot be reused unchanged.

The explicitly weaker exact-pose intersection covers 13326 depth-agreeing
pixels: 10937 choose the world base, 2389 (17.93%) the head base in DLSS mv.
Current match-valid records account for 1647 world-base and 2387 head-base
pixels; unmatched records account for 9290 world-base and only two head-base
pixels. Thus the head-base exposure is not merely a count of records already
rejected by mesh matching, although later consumer gates still limit a claim
about final pixel motion. The active ship box excludes the world-base pixels,
but UI coverage, terrain/holo and later overrides remain unresolved. These are
candidates, not proof the final fallback is correct. Ruled out: an
unconditional unchanged-pose skip, because coverage removal can expose
head-motion fallback under unchanged world geometry. Ruled out: requiring equal
raw origin as the definition of a stationary object, because this capture has
unchanged poses with a shared origin shift.

192513 also has 504 valid / eight invalid records, in 167 draw groups, but no
exact raw-pose pairs. Of 27 singleton-key pairs, 23 share about 0.0524 degrees
of rotation (maximum inter-pair error 0.0024 degrees), consistent with the
rotating starport; the other four move with the scene origin. The 477 records
in repeated-key groups were not force-paired. The common rigid fit has 0.016
median / 0.089 maximum position residual on coordinates up to 14418, so it is
evidence of coherent motion, not an exact shared-motion solution or safe camera
fallback. Both captures hit the 512-record cap. Their nonzero/zero coverage
record counts are 259/253 and 351/161; depth-agreeing covered pixels are 13326
and 150614. Zero coverage from the previous frame cannot safely reject a newly
visible object. Bounds and first-moving-frame history remain requirements, not
optional threshold tweaks.

First implementation decision: cache only immutable metadata of the currently
bound depth view, retaining COM identities and clearing at frame/shutdown
boundaries. Keep live scene-depth checks and the original eye-selection loop.
Every capped draw currently repeats GetResource / QueryInterface / descriptor
work; these calls have no new information while the view is unchanged. Do not
replace the eye loop with depthProbeSceneEyeOf: its already-picked shortcut can
miss the refresh the original path performs. Add overlapping input-change cause
counters before redesigning batching; the aggregate count cannot identify
whether scene CB, IDs or pool changes dominate. No static/visibility culling,
cap change, config change, or promised frame-time saving is part of this step.

Offline reports and filtered evidence are in `build/flight-20260917/`. The full
absolute-path build passed: 1547 mesh WARP checks, the exporter/probe roundtrip
and probe self-test, all 61 pool jobs plus three quiet gates, and the 249-key
config contract. The optional FSR3 engine test skips because that SDK is
absent; this is a DLSS test build. Validation output:
`build/mesh-metadata-verified.log`. Rebuild the clean commit before delivery
through `tools/install_edvr.py --target frontier`, with its dry-run and
verify-only checks; preserve the live INI.

Next flight: the same Quest 3 / VirtualDesktopXR / DLSS settings, on foot, ship
above the settlement, then rotating-station approach. Hold each scene for
roughly 90 seconds, then take one completed eye run. Check `mesh motion
diagnostic depth metadata` for hit/fill counts and `input-change causes
(overlapping)` for the batching constraint. Compare steady windows and motion
quality, excluding capture stalls; this change removes repeated metadata
queries and does not reduce the 512-record cap or promise a measured frame-time
improvement before the flight.

### 2026-09-17 -- reverse route exposes the oversized-stream batching limit

Verified graphics log `edvr_gfx_20260917_195056.log` identifies
`v0.17.0-2-g81eeedb`; use `tools/edvr_log.py --target frontier --expect-build
81eeedb`. The user's reverse route maps to 195254 station (scene frame 12423),
195609 settlement from the ship (28930), and 195653 on foot (31463). Each is a
completed paired eye run. Native environment/sizing and benchmark context are
recorded in the offline timing report under `build/flight-20260917-reverse/`.
The native log `edvr_openxr_20260917_195058_200_29064.log` matches the same
build: Quest 3, VirtualDesktopXR, 90 Hz, DLSS K, active XR output 3072x3264.
The user confirmed removing the Q3 trims during the route. Benchmark input
changes from 2307x1652 to 2481x2121, with a transient 2863x2448 allocation;
captured DLSS output before UI changes from 3550x2542 to 3818x3264.
Capture-containing native windows have CPU/GPU p50 4.752/8.245 ms (station),
12.703/16.717 ms (settlement) and 8.710/13.467 ms (on foot). These are
different workloads, not a controlled cache A/B test.

| Window end / scene | Depth metadata hits / fills | Input-change / write-map flushes | Average records per batch | Approx. mesh-draw CPU/frame | Hook CPU mean |
|---|---:|---:|---:|---:|---:|
| 12600 / station | 805988 / 3861 | 0 / 14870 | 124.0 | 0.343 ms | 1.030 ms |
| 28800 / settlement approach | 9698520 / 4080 | 64748 / 23218 | 20.9 | 0.902 ms | 3.568 ms |
| 30600 / settlement, includes transition | 16923243 / 3744 | 402143 / 22937 | 3.7 | 1.633 ms | 5.652 ms |
| 32400 / on foot | 15732911 / 1812 | 0 / 0 | 0 | 0.580 ms | 3.494 ms |

Draw estimates use sampled mean microseconds times sample count times 256,
divided by 1800 and 1000. Flush estimates overlap draw work and are not added.
These are per-window diagnostics, not cumulative counters. The cache is heavily
exercised (99.978% hits in window 30600), but the different scene/pose/size and
batch distributions prevent a clean before/after frame-time claim. Window 30600
accepts 1557744 stereo instances, below the 1843200 full-cap window count, and
includes a transition; do not treat it as a stable 1800-frame settlement
benchmark.

The new overlapping cause counters are decisive: all 64748 input-change flushes
in window 28800 and all 402143 in 30600 are `oversized-stream`; context,
scene-cb, instance-ids, pool and output causes are all zero. At 30600, this is
94.6% of the 425080 total capture dispatches. Station window 12600 has no
input-change flushes and already captures 124 records/batch. Ruled out: changed
scene/pool/ID bindings as the explanation for these settlement input-change
flushes, because their counters are zero. Buffer-size sensitivity and different
batch distributions also prevent crediting the cache with the whole station
timing difference from the first flight.

Source confirmation: `meshMotionDraw` forces a flush whenever `idd.ByteWidth >
maxInstanceBytes` (1 MiB), even if every input identity is unchanged. That
protects a per-draw slice copied repeatedly to offset zero. The accepted-record
budget is only 512 per eye: the actually used eight-byte IDs need at most 4096
bytes in a pending batch, independent of source-buffer size. Append each used
range at `pending.count * 8`, then pass that owned offset to both coverage and
transform capture. Keep the full-stream snapshot path unchanged for smaller
sources. This removes the size-only flush without enlarging the buffer or
copying/reading unused source data.

Correctness boundaries: keep all actual context/scene/IDs/pool/output changes
as flushes, and preserve pre-Map/Update flushes, eye/output changes, frame
consumption, unknown-write invalidation and immediate GPU-writable handling.
Source ID identity also prevents mixing the full-copy and ranged-copy layouts
in a pending batch. Original draw parameters, geometry coverage, record
admission and prior-frame matching stay unchanged. WARP regressions must check
distinct IDs at distant offsets, a 512-record batch, later-draw coverage,
source rewrites, small/large switches, eye switches and GPU-writable sources.
The expected improvement is fewer capture dispatches; it is not a reduction in
rendered game meshes or a demonstrated frame-time gain yet.

The mesh samples repeat the earlier classification constraints. Station 195254
has 504 valid / eight invalid records across 172 draw groups and zero exact
raw-pose pairs. Of 23 singleton-key pairs, 19 share about 0.039819 degrees of
rotation (maximum disagreement 0.00298 degrees); repeated-key groups remain
ambiguous. Settlement 195609 has 497 exact raw-pose matches with changed
origin, seven changed/unmatched valid records, and 108 wholly exact-pose draws.
Of 39389 depth-visible exact-pose candidate pixels, 3413 select the world base
and 35976 the head base. The current-match-valid split is 1463 world / 35956
head, with unmatched 1950 / 20; final UI/body/terrain/layer membership remains
unresolved. Coverage size is 2481x2121 here versus 2307x1652 in the station and
first-flight settlement, so raw pixel totals are not comparable. On foot again
has no captured rigid meshes and needs separate source/screen-path attribution.

The fix is implemented and independently reviewed with no blocking findings.
The full absolute-path build passed: 2183 mesh WARP checks, the exporter/probe
roundtrip and probe self-test, 61 pool jobs plus three quiet gates, and the
249-key config contract. Log: `build/mesh-id-batching-verified.log`. Rebuild
the clean commit for Frontier delivery through the sanctioned install tool,
with dry-run and verify-only checks; preserve the live INI and the user's
removed trims.

Next flight can focus on the ship above the same busy settlement: keep the
trims off, hold the view for roughly 90 seconds, then take one completed eye
run. `mesh motion diagnostic oversized streams` must show nonzero bounded ID
copies/instances while the old size-only flush cause stays zero; the exclusive
batch/flush counts show whether capture dispatches fall. Compare same-size
steady native windows and inspect motion, excluding capture stalls. Coverage
reissues, admitted record count and transform/match math are deliberately
unchanged. On-foot profiling and static/visibility culling remain future work;
this change addresses the confirmed batching defect.

### 2026-09-17 -- batching flight is slower overall; restore for comparison

The user reported worse performance. Both logs match `v0.17.0-3-g9446f4d`:
`edvr_gfx_20260917_203704.log` and `edvr_openxr_20260917_203705_531_9344.log`,
read through `tools/edvr_log.py`. Capture 204142 is scene frame 20347.
Environment remains Quest 3, VirtualDesktopXR, 90 Hz, DLSS K, untrimmed
2481x2121 input and active XR output 3072x3264. Offline evidence and analysis
are under `build/flight-20260917-batching-check/`.

| Native window | CPU p50 | GPU p50 |
|---|---:|---:|
| Prior 81eeedb window 17 | 12.703 ms | 16.717 ms |
| New 9446f4d window 8 | 13.381 ms | 17.596 ms |
| New 9446f4d window 9 | 14.930 ms | 18.971 ms |
| New 9446f4d window 10 | 14.710 ms | 18.696 ms |

The new windows average 14.340 ms CPU and 18.421 ms GPU p50, respectively 12.9%
and 10.2% above the prior window. This average of window medians is not a
pooled median. Average p95 also worsens; p99 improves. Ruled out: the eye-dump
stall explains these slower medians, because even window 10 finishes before
capture begins.

Batching is active: graphics windows 18000 and 19800 each have 14400 batches /
1843200 instances, averaging 128 records per batch with a maximum of 502. All
flushes are write/map boundaries; every input-change cause is zero. Ranged
copies cover 387626 and 390872 accepted draws respectively. Ruled out: the
optimization failed to activate, because the old size-only flushes disappear
and capture dispatches fall to eight per frame.

Approximate sampled mesh-draw CPU is 1.079/1.111 ms per frame, down from 1.633
in the prior mixed window 30600; nested flush CPU is 0.065/0.093 versus 0.465
ms. These overlapping scopes must not be added. Full hook means are 5.792/5.782
versus 5.652 ms. The new full-cap window admits 18.3% more instances than that
prior partial window, has about 17% more cap-rejected draws and about 4% more
sampled mesh calls. The scenes are not identical workloads.

Differencing cumulative GPU sums/counts estimates new coverage work at
1.379/1.492 ms per frame versus 1.130 ms previously, while capture work drops
to about 0.071/0.074 versus 1.311 ms. The old capture query ring skips many
batches, so that extrapolation is uncertain. Lower measured mesh costs do not
clear the batching change: copy/queue scheduling or work outside the timed
scopes could still regress. The old path already issued one ranged copy per
accepted draw; the new path changes destination offsets and dispatch boundaries
rather than adding copy commands. Broader game/render workload is also
unresolved.

Capture 204142 has 512 current/prior records, 504 valid and eight invalid, with
all 504 valid raw poses exact across a uniform origin shift and 33 match-valid
records. All coverage IDs are integral and within 1-512; 232 records have
nonzero coverage. Coverage is 644095 pixels, of which 408554 agree with
captured depth, versus 814009/602768 previously. The 479 unique valid pool
indices span 48-6719 with maximum frequency three. These checks show no
captured signature of broken ranged offsets, but do not prove rendering or
scheduling equivalence across the flight.

For a controlled comparison, the sanctioned installer restored the five package
components from the 9446f4d installation transaction. Every restore hash first
matched the corresponding installed hash in the earlier 81eeedb receipt.
Restore completed and `--native-openxr --dll --native-receipt <81eeedb receipt>
--verify-only` passed. Frontier is now `v0.17.0-2-g81eeedb`; source main
remains 9446f4d plus this investigation update. The live INI and the user's
removed trims were preserved. No speculative rendering change was made.

Next flight: remain at the same settlement position/view and unchanged settings
for roughly 90 seconds, then take one completed eye run. Verify the resulting
logs against 81eeedb explicitly, because HEAD and the current build artifacts
still contain the batching change. A repeatable improvement on the restored
build would implicate batching despite its lower measured capture cost; similar
slow timing would weaken that hypothesis and direct profiling toward the wider
frame workload. Compare before deciding whether to revert or instrument the
change further.

### 2026-09-17 -- restored flight improves; prepare one-run comparison

The restored flight is verified as `v0.17.0-2-g81eeedb` in
`edvr_gfx_20260917_205628.log` and `edvr_openxr_20260917_205629_490_25244.log`,
using the sanctioned reader with explicit `--expect-build 81eeedb`. The user
reports "Better". Offline timing evidence and capture analysis are under
`build/flight-restored-check/`.

| Run / clean native window | CPU p50 / p95 | GPU p50 / p95 |
|---|---:|---:|
| Batching 9446f4d, mean of windows 8-10 | 14.340 / 16.409 ms | 18.421 / 20.735 ms |
| Restored 81eeedb, window 4 | 12.898 / 14.887 ms | 17.183 / 19.117 ms |

The restored median improves by 10.1% CPU and 6.7% GPU; p95 improves by 9.3%
and 7.8%. Window 4 is wholly before capture and overlaps the steady graphics
window for about 28 of its 30 seconds. Restored window 5 overlaps the dump in
its final two seconds; exclude it from the primary comparison. Graphics window
12600 accepts 418892 draws / 1843200 instances, more draws than the batching
windows with the same instance count. It has 418892 capture batches,
approximately 1.665 ms/frame mesh-draw CPU, 0.431 ms nested flush CPU, and
5.821 ms hook mean. Do not add the nested scopes. Game Map write waits differ
by only about 0.04-0.07 ms/frame; native treatment and submit timings improve
slightly but do not explain the full difference.

Capture 205905 has 504 valid / eight invalid records, all 504 uniquely paired
raw poses unchanged with an origin shift, 20 match-valid records and 109 fully
exact groups among 117 draw groups. Coverage IDs are integral and within range;
431 unique pool indices span 29-7444 with maximum frequency two. There is no
captured corruption signature. However, the overview shows the settlement
closer/lower than 204142, and mesh coverage is 147 records / 327771 pixels /
188640 depth-agree pixels versus 232 / 644095 / 408554 in the batching capture.
The sequential before/after/restore evidence supports withdrawing the
optimization, but does not isolate it from scene variance. Do not describe this
as a strict controlled A/B.

The user approved a diagnostic build and suggested keeping the ship landed. Use
the cockpit facing a busy part of the settlement, with unchanged settings and
no camera movement beyond unavoidable headset motion. An explicitly armed
baseline/batching/baseline sequence removes the need to restart the game or
reposition between variants; the repeated baseline exposes drift from moving
NPCs or lighting. Preserve the metadata cache and all original
admission/history/math/write boundaries. Normal rendering returns to the prior
immediate oversized-stream path; the packed batching path is exercised only by
the comparison.

The missing-cost hypothesis is deliberately narrow: batching still performs ID
copies and coverage re-draws per admitted draw. Their relationship with delayed
capture may change command scheduling or synchronization outside the previous
brackets. Add common instrumentation for accepted-path CPU, ID-copy enqueue CPU
and asynchronous GPU intervals, coverage and capture, with copied bytes/counts,
batch sizes and missing/pending query counts. Native frame timing remains the
outcome measure. GPU timestamps are command-stream elapsed intervals, not proof
of a particular stall or a sum that can be equated directly to a native frame.
Use complete scoped benchmark windows; exclude settling, menu transitions,
configuration changes, censuses and eye dumps. A run that never reaches valid
sampling must report unavailable or aborted, never apparent zero cost.

After this comparison, optimize the measured dominant work: static objects
require correct camera-motion fallback even when the existing base is
head-relative; current visibility must not be inferred solely from the previous
frame; coherent station motion and independent ships need distinct history
handling. On-foot captures admitted no rigid meshes, so that path needs its own
cost attribution. These are follow-up implementation targets, not features
enabled by this diagnostic.

Implementation: the Instruments action waits for menu closure, then settles for
ten seconds. A1/B/A2 each advances only after its own completed native 2-second
warmup / 30-second sample / 2-second drain report. A comparison epoch separates
intentional variant changes; an independent disturbance epoch rejects settings,
menu, census or eye-dump activity even during warmup, when the native collector
can restart without an abort report. Mode switches happen only after pending
capture is flushed at the frame boundary. Missing native/component samples,
lingering GPU queries, changed metadata, no eligible oversized draws or a
50-second phase timeout abort with explicit instrument coverage and restore
immediate capture.

Sampling limits: full-hook CPU reuses the existing 1/256 clock, including early
returns; accepted-path and copy sampling use bounded call schedules. Copy CPU
encloses only the copy API call, excluding the diagnostic query calls. Capture
GPU samples at most the first flush in one of sixteen frames. This is a
selected boundary sample, not an unbiased average that can be multiplied by all
batches. During the native sample, the common diagnostic query policy replaces
the normal rolling capture/coverage queries. Compare A/B/A within this
diagnostic; its additional instrumentation and changed query schedule prevent
direct equivalence to historical unarmed runs. Mixed rolling windows are
labelled; phase-tagged component records are authoritative. Components overlap
and must not be summed into a purported native frame cost.

Validation: independent integration review found and closed the warmup
disturbance gap before flight. The full absolute-path build passed with 2265
mesh-motion WARP checks, 71 native performance-history checks, all
Python/native/installer gates and the unchanged 249-key config contract. The
WARP rig exercises both capture layouts, real sampled copy/coverage/flush
intervals, the full A1/B/A2 sequence, cancellation, stale and mixed report
identities, disturbance events, missing/pending metrics and changed metadata.
Log: `build/motion-comparison-verified.log`. Rebuild the clean commit for
delivery and install/verify on Frontier without `--ini`. Normal play uses the
restored baseline; select F8 > Instruments > Compare motion performance to arm
the experiment, and take any eye dump only after the completion notice.

### 2026-09-18 -- landed controlled comparison favors packed capture

The sanctioned log reader verifies c48d231 in both
`edvr_gfx_20260918_052802.log` and `edvr_openxr_20260918_052804_214_21240.log`.
The first arm was cancelled before sampling; the second completed all three
phases. Native windows 15-17 each have 30 seconds of sampling and a completed
two-second drain, with equal valid CPU and GPU counts and no missing or invalid
samples. Settings, FOV, treatments and input size remain fixed. A2 completes at
05:32:13.555; the subsequent dump event invalidates only window 18. The
overview confirms a landed cockpit facing the settlement, with another ship in
view.

| Phase | CPU p50 / p95 / p99 ms | GPU p50 / p95 / p99 ms | Valid frames | Capture batches/frame |
|---|---:|---:|---:|---:|
| A1 immediate | 13.335 / 14.945 / 16.001 | 17.008 / 18.766 / 20.055 | 1644 | 243.60 |
| B packed | 12.415 / 13.936 / 14.735 | 16.301 / 17.982 / 19.850 | 1766 | 10.00 |
| A2 immediate | 12.826 / 14.168 / 14.868 | 16.448 / 18.040 / 19.300 | 1724 | 243.74 |

B beats both baselines on CPU p50/p95/p99 and GPU p50/p95. Against the mean of
the two baseline percentiles, B improves CPU p50 by 0.666 ms (5.1%) and GPU p50
by 0.427 ms (2.6%). This is an average of two percentiles, not a pooled median.
A1 to A2 also improves by 0.509 ms CPU and 0.560 ms GPU, so the full gain
cannot be assigned to batching. GPU p99 is 0.550 ms worse than A2 and 0.173 ms
worse than the baseline mean. Do not claim uniformly better tails.

Ruled out: batching inherently slowing this controlled landed settlement view,
because phase B beat both immediate phases on native CPU p50/p95/p99 and GPU
p50/p95 with unchanged instance density. The cause of the earlier
separate-flight slowdown remains unresolved: those flights changed scene cost,
and the present comparison uses a different profiling schedule from the old
unarmed builds.

Every phase accepts exactly 1024 instances per valid native frame and about 244
draws/frame. Packed capture reduces dispatches by 95.9%, preserving all
instances and write boundaries; it does not reduce coverage draws or ID copies.
Sampled full mesh-hook CPU is 1.688/1.293/1.776 ms/frame (A1/B/A2), a 25.3%
improvement against the baseline mean. Nested accepted-path CPU is
0.904/0.577/0.916 ms and nested flush CPU 0.426/0.047/0.456 ms. Copy enqueue
CPU is nearly unchanged at 0.030/0.032/0.034 ms. These nested costs are not
additive. All component queries retire with zero skipped, invalid or pending
samples. Map write waits show no batching stall in the supporting periodic
summaries.

Copy GPU command intervals average 4.005/3.694/4.580 us and coverage intervals
4.364/3.868/3.894 us. These are not serialized component costs that can be
added to reconstruct native frame time. Capture intervals average
11.303/8.641/14.469 us, selected from the first flush on one of sixteen frames;
they cannot be extrapolated across every dispatch. ID copying totals only 8192
bytes/frame. A whole-source snapshot would copy over 1 MiB even once, and there
are ten observed write boundaries/frame; the intervals do not justify that
bandwidth tradeoff.

The post-comparison capture eye_053218 contains 504 valid and eight invalid
records. Exact raw pose remains unchanged in 500 records / 112 whole draw
groups despite a uniform origin rebase; strict unchanged-origin reuse admits
zero. There are 82 wholly zero-coverage groups / 295 records, overlapping exact
pose in 80 groups / 291 records. Among exact-pose candidates that the offline
fallback probe can classify, 2244 pixels select head motion and 471 world
motion. UI, body, terrain, hologram and later selection gates remain incomplete
in that probe. These counts identify work worth investigating, not safe skips.
Preserve history, current visibility and the correct camera-relative fallback
before removing static records or coverage.

Implementation follows the tested packed path and frame-bounded capture query
schedule: normal rendering uses packed oversized IDs, while the explicit menu
comparison remains A1 immediate / B packed / A2 immediate. Arming preserves the
normal mode until a safe frame boundary; completion, cancellation and failure
restore packed capture. The normal profiler also attempts at most the first
capture flush in one of sixteen frames, avoiding a ring scan on every batch. No
static/visibility skips, config changes or whole-source snapshots are enabled.

Full timing evidence, normalized reports and capture analysis are retained
under `build/flight-20260918-comparison/`. Independent review found no
functional mode-transition defect and requested additional boundary coverage.
The WARP rig now checks pending IDs through A1-to-B, B-to-A2, completion and
cancellation via the public frame boundary, plus query admission across the
1800-frame reporting boundary. Targeted compilation and 2438
WARP/exporter/probe checks pass. The full absolute-path build passes all 61
parallel jobs, three quiet jobs and the unchanged 249-key config contract
(`build/motion-packed-reviewed.log`). Rebuild the clean commit and
install/verify on Frontier without `--ini`.

### 2026-09-18 -- admission costs and static-motion equivalence

The user approved the next pass after 7000eb4 was installed and verified on
Frontier. That build is pushed to main; all build gates and 2438 mesh WARP
checks passed, and the live INI hash remained unchanged. Normal rendering keeps
packed capture and the frame-bounded profiling policy.

Enumerate the remaining hypotheses before changing admission behavior:

- Repeated scene/depth/eye selection dominates rejected draws. Confirm with a
  sampled exclusive scene/eye stage and explicit early-exit counts.
- Pipeline guard queries dominate admitted or near-admitted draws. Confirm with
  their exclusive CPU stage and repeated immutable depth/blend state
  identities.
- IA/resource validation repeats expensive descriptor/resource queries. Confirm
  with its exclusive stage and bounded observations of retained COM identities;
  pointer reuse without retained ownership is not evidence of resource
  equality.
- Static records can use the existing camera fallback. Compare projected motion
  at captured depth-visible pixels, reporting world/head disagreements and
  unavailable body, ship, UI, terrain or later selection gates separately. DLSS
  `mv()` also sets `trackedForeground` when mesh motion succeeds; removing
  coverage can change later background-history invalidation and reactivity even
  when the physical vector agrees. These final decisions require their own
  proof.

The approximately 0.716 ms difference between the prior full-hook and accepted
CPU estimates is an investigation envelope, not an exclusive measured stage or
a promised saving. Additional sampled spans must include early returns, avoid
unsampled clock calls and distinguish never-entered stages from zero elapsed
cost. Descriptor observations do not authorize bypassing live pipeline, eye,
write or first-moving-frame checks.

Start static equivalence work on eye_053218 and synthetic nonidentity camera
cases. An unchanged raw pose across an origin rebase is only a candidate. The
offline tool must not treat a missing final selection gate, unmatched record or
ambiguous object identity as proof that coverage can be removed. On-foot's zero
rigid records make admission profiling useful there, while a rigid static skip
cannot remove capture work that was never submitted.

Implementation: admission uses the existing 1/256 hook selector and reports raw
exclusive CPU totals for fast rejection, scene/depth/eye/cap, pipeline guards,
IA/resources, preparation and accepted work. Per-stage entry, rejection and
sample counts expose an inactive path or an unsampled stage. Fast rejects also
distinguish disabled, failed setup, unknown shader, invalid draw and context.
The final timestamp is taken after local COM releases and nested timers; raw
stage totals sum to the same full-hook interval. A sampled completed draw adds
five boundary clock reads, while unsampled draws add none. These instrumented
timings are not directly interchangeable with the previous uninstrumented stage
costs.

Descriptor observations follow the actual getter calls and retain at most four
COM identities for each of depth state, blend state, ID buffer, scene constant
buffer, pool SRV and pool buffer. Calls, hits, fills and evictions are
explicit; null/default depth and blend states do not invent a GetDesc call.
Frame and shutdown boundaries release all identities. This is a bounded
observation of a possible cache, not an active cache or a change to pipeline
validation. Both normal 1800-frame reports and existing A/B/A phase reports
include the new data.

The offline probe now compares the actual captured, match-valid mesh map with
the DLSS/DLAA `mv()` base projection. It respects the full source size, crop
origin, half-pixel centers, jitter, previous-minus-current convention, exact
coverage depth for meshes and dilated depth for fallback. Far-world projection
uses rotation only. Unknown identity, unmatched maps, missing metadata and
invalid projections are reported separately. The ordinary CLI remains read-only
and writes JSON only to stdout.

On eye_053218, all 2715 depth-visible exact-pose candidate pixels cross an
origin rebase. Only 2249 have matched mesh maps: 2244 choose the head base and
five the world base. The remaining 466 world-base pixels are unmatched and
cannot be used as equivalence evidence. Maximum-component vector differences,
in input pixels:

| Base | Compared pixels | Median difference | Maximum difference |
|---|---:|---:|---:|
| Head | 2244 | 0.00114841 | 0.00158126 |
| World | 5 | 0.000143277 | 0.000151937 |

None meet the tool's fixed 0.0001-pixel diagnostic criterion. The small errors
could include float arithmetic effects, but that explanation has not been
established and the criterion was not widened to fit this capture. These values
do not show a large physical-vector error from selecting the head base in this
landed view. They also do not authorize removing coverage: all 2249 comparisons
have final UI, terrain/hologram and hidden-history decisions unevaluated by
this probe. Those inputs are already captured; this tool does not yet consume
them. Body-grid and ship-part claims are a distinct omitted-input limitation in
cases where those paths can apply. `safe_skippable_claim` remains false.

Evidence:
`build/admission-static-20260918/mesh-motion-static-equivalence-053218.json`.
The Python self-test covers nonidentity head/world projection, crop/jitter,
rebases, unmatched and ambiguous records, invalid math, far rotation and
unknown gates. Independent review caught and corrected far-world translation
handling. The targeted WARP rig passes 2500 checks, including early returns,
zero-clock unsampled paths, cleanup timing, raw-total partition, descriptor
reuse/eviction, frame/shutdown resets and comparison accumulation. The
absolute-path full build passes all 61 parallel jobs, three quiet jobs and the
unchanged 249-key config contract (`build/motion-admission-verified.log`).
Rebuild the clean commit before installing and verifying on Frontier without
`--ini`.

### 2026-09-18 -- 80416a0: landed versus on-foot admission and GPU cost

The graphics log `edvr_gfx_20260918_062040.log` and paired native log
`edvr_openxr_20260918_062041_399_9568.log` both verify as `v0.17.0-7-g80416a0`.
The sanctioned reader commands and selected evidence are in
`build/admission-flight-20260918/evidence.txt`; its companion `report.md`
contains the detailed reduction. The user reports no shimmer on foot and lower
frame time, still short of 90 FPS.

| Stable native windows | CPU p50 / p95, ms | GPU p50 / p95, ms |
|---|---|---|
| Landed, 5 | 13.044 / 14.494 | 16.684 / 18.343 |
| Landed, 6 | 12.784 / 14.271 | 16.529 / 18.196 |
| On foot, 8 | 8.110 / 9.139 | 12.968 / 14.292 |
| On foot, 9 | 8.282 / 9.121 | 13.077 / 13.642 |

Window 4 includes landed onset, 7 ends at a scope change, and 10 ends at
transport loss during shutdown; none is in the primary comparison. At 06:24:05,
pacing becomes turbo, the panel begins applying twice per frame, and rigid-mesh
admission stops. Input/output sizes remain 2481x2121 / 3072x3264 per eye. The
11.111 ms budget is exceeded by both CPU and GPU landed, but by GPU alone on
foot. Rendering and pacing change together, so this is not an A/B measurement
of the mesh path or proof of its relationship to shimmer.

The exclusive admission estimates use raw sampled microseconds times 256 / 1800
/ 1000. They include sampled clock overhead and are cost envelopes, not
forecasts of removable time:

| Scene | Full hook | Fast | Depth / scene / eye / cap | Pipeline + resources | Preparation | Accepted |
|---|---:|---:|---:|---:|---:|---:|
| Landed, 06:23:21 | 1.424 | 0.569 | 0.615 | 0.058 | 0.007 | 0.175 |
| Landed, 06:23:52 | 1.464 | 0.554 | 0.663 | 0.059 | 0.007 | 0.180 |
| On foot, 06:25:07 | 0.728 | 0.449 | 0.279 | 0 | 0 | 0 |
| On foot, 06:25:30 | 0.721 | 0.448 | 0.273 | 0 | 0 | 0 |

At 06:23:21, depth metadata reports 18,859,339 hits and 4624 fills across 1800
frames. On foot at 06:25:07, it reports 14,509,695 hits and 1800 fills: exactly
one fill per frame. Every on-foot scene-stage entry rejects before pipeline
validation; accepted draws, cap decisions, descriptor observations and new mesh
coverage/capture work are zero. The combined stage cannot tell which of its
internal checks rejected a draw. The borrowed-metadata change targets the
redundant AddRef/Release and descriptor copy on each cache hit, about 10,477
times per frame landed and 8061 on foot. It preserves the owning cache and all
live selection checks; it does not skip any mesh or motion work.

Ruled out: increasing depth-metadata cache capacity as a useful fix for this
flight, because the existing single entry already hits on virtually every call.
Ruled out as a priority: pipeline/resource descriptor caching, because the
entire containing stages cost about 0.06 ms landed and are never reached on
foot, despite high observed reuse. The much larger cap-rejected population is
refused work, not a count of mesh coverage draws or successfully matched
records.

The seven-region temporal GPU reports are present and healthy: settled on-foot
windows contain 600 stereo pairs with no dropped pairs or region leases. Median
stereo costs are prep 0.41-0.43 ms, full-frame NGX 2.30-2.32 ms, and UI
0.53-0.54 ms. Landed reports are about 0.30 / 2.52-2.58 / 0.32-0.33 ms. These
component medians indicate scale; their sum is not the measured median of a
total. Native application-render GPU time includes producer rendering and
native treatment while excluding transfer/runtime waits. Its approximately 13
ms on foot leaves substantial work outside the temporal bracket.

The next GPU attribution must distinguish three plausible EDVR costs before
changing rendering: screen UI-mask clear/reissues, per-eye screen-motion clear
and projection, and weapon motion generation. Each needs exact command-bracket
timing and eligible/submitted/ready/invalid/skipped counts. These producers run
before the already measured temporal consumers. Call counts and representative
samples together can test whether they account for a material fraction of the
remaining GPU cost; a cheap first draw alone cannot bound heterogeneous work.
Sparse bounded queries must not span intervening game commands, wait for
results, or report unavailable measurements as zero.

The borrowed-metadata regression passes 2506 WARP checks. New ownership tests
release external texture/view owners while the cache remains alive, check a hit
does not take another reference, and check release at cache clear. Existing
alias-DSV, live scene-pair change, frame, unknown-write and shutdown checks
remain green. An isolated five-million-iteration WARP hit/reject loop measured
median 12.602 ns before and 1.481 ns after; this establishes the removed
mechanical cost, not a game-frame or FPS improvement.

Screen and weapon diagnostics use bounded nonblocking query rings, rotating
positions within fixed call blocks, and explicit per-frame admission budgets.
The screen clear is measured separately from an actual consecutive-frame
projection so a first-frame clear cannot masquerade as a projection sample.
Each source scope collects up to 1800 source frames, stops admissions, and
drains outstanding results before another scope starts. Source/size changes and
inactivity also close a scope. A 120-frame drain cutoff explicitly reports
pending samples as abandoned; late completions cannot enter the next window.
Policy and compact counter lines distinguish zero eligible work, zero ready
results, begin failures and budget skips. Means describe sampled command
intervals; the log does not extrapolate them to a whole-frame cost. Busy-frame
budget skips can bias that sample and must be considered in the analysis.

Final validation: 55,438 screen-motion and 80,503 weapon-motion WARP checks,
including immediate configuration closure, source changes, forced drain cutoff,
same-frame budgets, query retirement and shutdown reset. Independent review
checked owner-context wiring, exact command brackets, skip accounting and the
1200-byte logger limit. The absolute-path full build passes all 61 pool jobs,
three quiet jobs and the unchanged 249-key config contract
(`build/motion-borrow-gpu-verified.log`). Rebuild the clean commit before the
Frontier install, preserve the INI, and verify using the installer tool.

### 2026-09-18 -- 89b9055 final 210 seconds: identity dispatch is the larger target

The user requested the final approximately 3.5 minutes and then clarified that
the weapon remained holstered. Both `edvr_gfx_20260918_070724.log` and
`edvr_openxr_20260918_070725_985_13004.log` verify as `v0.17.0-10-g89b9055`.
The selected graphics interval is 07:10:48.160-07:14:18.160 local; the native
log aligns in UTC at 13:10:48.136-13:14:18.136. Reader evidence and reductions
are saved under `build/flight-89b9055-final/` (`report.md`, `evidence.txt`, and
`screen-weapon-gpu.md`). All flight reads used `tools/edvr_log.py`.

Pacing changes to runtime just before the cutoff, at 07:10:47.617. Stable
landed native windows are 9-11. Turbo begins at 07:12:31.317; stable on-foot
windows are 12-13. Window 14 agrees but ends at a scope change during exit, so
it is supporting evidence only. The mesh window ending 07:12:48 straddles the
transition and is excluded.

| Stable scene | CPU p50 / p95, ms | GPU p50 / p95, ms |
|---|---|---|
| Landed, windows 9-11 | 11.864-12.203 / 13.454-14.005 | 15.643-16.029 / 17.384-17.944 |
| On foot, windows 12-13 | 7.734-7.787 / 8.691-8.811 | 12.415-12.547 / 12.853-13.111 |

The settings remain Quest 3 / VirtualDesktopXR / 90 Hz / DLSS K, with 2481x2121
eye input and 3072x3264 active XR output. The on-foot source is 5120x2880;
temporal-price reports name a 3818x3264 treatment output. These reported
surfaces must not be conflated. At the 11.111 ms budget, landed remains over on
both CPU and GPU; on foot is over on GPU alone.

The borrowed depth-metadata change has the expected direction in its target
stage. On-foot scene-stage estimates are 0.188-0.189 ms/frame versus the
previous 0.273-0.280; full mesh-hook estimates are 0.612-0.616 versus
0.721-0.729. Landed scene-stage estimates are 0.545-0.552 versus 0.615-0.663;
full hook is 1.312-1.341 versus 1.424-1.464. On-foot work still rejects before
pipeline/resource checks, with one metadata fill per frame and almost all
remaining entries hitting. Accepted rigid coverage/capture remains zero there.
Landed accepted draws increase slightly to about 250/frame with 1024 instances.
These are separate flights with similar but unequal workloads and sampled clock
overhead, so the results support the small optimization without proving that it
caused the entire native-frame improvement.

Stable on-foot temporal prices remain prep 0.42 ms, full NGX 2.30-2.31 ms and
UI 0.53 ms, each a stereo-pair median. Their arithmetic sum indicates about
3.25 ms of work, not the median of a measured total. Those windows have no
dropped pairs or failed region leases.

The new producer scopes are healthy. Scopes 6-8 overlap the stable on-foot
native interval; complete later scope 9 agrees. Each has 1800 accepted source
frames. Screen UI clear costs 1.906-1.934 us/sample at one call/frame, UI
reissues 8.243-8.985 us at 5.73-6 calls/frame, eye clears 3.745-4.020 us at two
calls/frame, and actual projections 30.615-32.239 us at two calls/frame. All
selected screen samples are submitted and ready, with no failures, budget
skips, invalids or pending results. Multiplying those means by call rates gives
an indicative 0.123-0.125 ms/frame. Heterogeneous systematic samples are not a
direct whole-frame timing, but these repeated results do not support screen
motion as the missing sustained multi-millisecond cost.

The holstered run still exercises the broad original-vertex motion producer:

| Scope | Eligible calls/frame | Identify, us/sample | Post-VS capture | Motion raster | Scheduled / budget-skipped / ready per stage |
|---|---:|---:|---:|---:|---|
| 6 | 64.00 | 17.330 | 3.430 | 4.175 | 1800 / 109 / 1691 |
| 7 | 64.00 | 14.263 | 2.761 | 5.632 | 1800 / 50 / 1750 |
| 8 | 63.99 | 17.769 | 2.805 | 4.427 | 1800 / 136 / 1664 |
| 9 | 64.00 | 14.272 | 2.451 | 4.410 | 1800 / 30 / 1770 |

The once-per-frame map clear adds about 4.4 us. Every admitted query retires
ready; begin failures, internal timer skips, invalids and pending results are
zero. The budget retains 92.4-98.3% of scheduled samples. Multiplication by the
call rate suggests about 0.91-1.14 ms/frame in identity alone and 1.36-1.60
ms/frame across these producers, including the clear. These are screening
estimates: heterogeneous draw cost and the frame-budget omissions can bias
them. They are not a promised recoverable saving.

The label does not prove these are weapon objects. Admission checks five VS
families, source-depth identity, and a depth/stencil state writing bit 0x10.
The earlier planet-performance investigation already records that this bit also
occurs on generic meshes. The current logs do not identify which objects are
being captured, nor whether their output passes the final depth/stencil test.
They establish substantial producer activity while holstered. The incremental
workload of drawing a weapon remains unmeasured.

Ruled out: treating a holstered weapon as evidence that this producer is idle,
because about 64 eligible calls/frame reach identity, capture and raster. Ruled
out as the leading sustained GPU target: screen UI/projection generation,
because four healthy scopes show the much smaller sampled scale above.

The next target is the separate four-byte instance copy, one-thread identity
dispatch and compute-state transition in `weapon_motion.cpp::identify`, which
reads only two words from the instance pool. Before choosing a production
change, assess exact input reuse and whether identity can be collected with
existing vertex capture. A programmable geometry shader may make that capture
slower than its present pass-through form, so fusion needs a local hardware
comparison first. No static/invisible skip follows from these counters; no
repeat flight solely to draw the weapon is needed to establish this target.

The read-only feasibility check favors that local fusion experiment over an
identity cache. The present stream-output shader uses original vertex bytecode
as an output signature, with no geometry program; this is supported by
[CreateGeometryShaderWithStreamOutput](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-creategeometryshaderwithstreamoutput).
Identity is absent from that output. A programmable point geometry shader could
pass positions and load identity once, but runs for every captured index. The
local prototype must check the complete position count and the exact 16-byte
identity, including whether a short second output buffer truncates capture,
then compare GPU cost against separate identity and capture. A UAV alternative
adds geometry-stage UAV support requirements. None is a production fix until
correctness and a net timing benefit are demonstrated on hardware.

An exact cache needs the instance resource, byte offset, pool identity and
write generations. Current weapon-motion invalidation tracks geometry writes,
not instance/pool writes. Historical on-foot captures contain only 9/42
duplicate tuples in `102403`, and 4.0-8.3% in five others (`064839`, `064854`,
`081113`, `081119`, `081148`). These are optimistic reuse counts because the
captures do not encode intervening writes, and they are different workloads
from this flight. The earlier planet-performance journal also records changing
instance slots and skeleton identities. Thus caching is a secondary candidate,
not evidence that all 64 current identities can be reused safely.

### 2026-09-18 -- local identity/capture fusion benchmark

The proposed optimization removes the per-draw identity compute dispatch by
emitting identity alongside the existing indexed point capture. The local
`tools/identity_fusion_test/` experiment compares the production identity
compute shader plus no-program stream output with a programmable point geometry
shader. Production rendering remains unchanged during this experiment.

The correctness gate compares every position and all four identity words,
including invalid instance indices, valid zero-valued identities, repeated
indices, nonzero index start/base, consecutive draws, and counts through
131072. Separate output statistics check that all positions and exactly one
identity are written. State checks preserve previously bound GS resources even
when the original geometry shader is null. Two output appends need
`maxvertexcount(2)`; one silently exhausts the shared geometry output budget.
The 16-byte identity stream does not truncate the position stream in the
initial WARP check.

Historical draw metadata supplies the mixed timing workload. From Steam
`drawstate_102403.bin`, frame 10963 has 42 source-mesh records in the
compatible shader families; restricting to single-instance, nonzero triangle
counts within the production size limit leaves 40 draws and 342741 indices.
This is not the 64-draw workload in the latest flight, and source-mesh records
do not establish every production depth/stencil admission condition. Five
exported original shader signatures all put float4 `SV_POSITION` in output
register 4; their other outputs differ. The timing fixture uses a synthetic
vertex shader and historical index counts, not the game's complete
skinning/material execution. Local metadata and signatures are saved in
`build/identity-fusion/historical-workloads.json`; game shader assets are not
included in the repository.

An additional compatibility gate exposed that a structured 16-byte identity
buffer cannot be created with stream-output binding on the tested WARP device
(`E_INVALIDARG`). The experiment therefore distinguishes copying the streamed
identity into the current structured format from directly reading a typed uint4
stream-output buffer. Both must preserve the exact identity bits as seen by a
GPU consumer. Performance comparisons must include any required copy and use
separate outputs per draw, matching production resource ownership.

Both WARP and NVIDIA GeForce RTX 5090 pass 270 correctness checks, including
GPU reads of both identity formats. The SDK debug layer is unavailable on this
host and is reported as such. The standalone rig requests it when installed;
all performance measurements use a release device. A typed `R32G32B32A32_UINT`
stream with a `Buffer<uint4>` consumer preserves exact bits without a copy; the
current `StructuredBuffer<uint4>` consumer needs a separate resource and copy
on both tested devices.

Three hardware runs measure the entire 40-draw identity/capture batch, with
separate per-draw output resources, the same four-byte instance copies, state
save/restore, and at least 250 ms each of wall and valid GPU warmup. Each run
rotates path order over 15 rounds (45 samples); all 135 measured samples have
ready, non-disjoint timestamps with positive deltas. There are no per-draw
timestamp pairs inside the measured batch.

| Run | Current CS + no-program capture, ms | Direct typed fusion, ms | Fusion + structured copy, ms |
|---|---:|---:|---:|
| 1 | 0.246304 | 0.220544 | 0.282144 |
| 2 | 0.240928 | 0.220704 | 0.282080 |
| 3 | 0.245344 | 0.220640 | 0.281888 |

These are whole-batch GPU medians, not game-frame costs. The direct typed
variant saves 20.224-25.760 us per 40-draw batch (8.4-10.5%). The copy variant
adds 35.840-41.152 us. CPU submission medians are 7.2-7.6 us for baseline,
4.4-4.6 us typed, and 5.6 us copied, well below the measured GPU intervals.
Artifacts are `build/identity-fusion/run1/` through `run3/`, each with JSON
metadata and CSV samples. The timings do not include actual original shader
resource access/skinning or the immediate motion-raster consumer. They cannot
be scaled into the latest flight's 64-draw cost or a promised FPS gain.

Ruled out as an optimization for this tested workload: fusion followed by a
copy to retain the structured consumer, because all three runs are slower. The
typed path has a repeatable but small local benefit. It does not justify
changing five shader families and the motion-history consumer based on the
earlier sampled identity estimate alone. Keep it as a local experiment; a
future representative replay must include original shaders and immediate motion
consumption before selecting a production change. Frontier remains on 89b9055
and no new test flight is requested.

Validation: the new rig's WARP `--self-test` is registered in `build.bat`.
Hardware correctness and the no-write `--dry-run` check pass. The absolute-path
full build exits 0: all 61 pooled jobs, three quiet jobs, 249-key config
contract and packaging/export gates pass, including the new rig's 270 checks.
The build log is `build/identity-fusion-validation.log`.

### 2026-09-18 -- original shaders and immediate motion consumption

Hypothesis: the synthetic typed-fusion saving survives the original vertex
shaders and immediate motion-map consumer. The discriminator is the GPU time of
an entire selected-mesh batch, after exact position, uint4 identity and full
motion-map comparisons pass. This is a standalone replay; no production
renderer or installed Frontier files change.

`tools/identity_fusion_capture.py` exports two adjacent frames and their used
shader bytecode, constant buffers, instance pairs, pool/bone snapshots, and
complete geometry. It normalizes index/base/instance offsets and preserves
shared geometry groups for the production history candidate selection. It
verifies shader IDs with EDVR's FNV seed, records SHA-256 hashes and rejection
reasons, and rejects ambiguous geometry borrowing. Binary fixtures and game
shader assets remain local under ignored `build/identity-replay/`.

| Capture | Accepted draws / candidates per frame | Accepted indices / candidates per frame | Used VS families |
|---|---:|---:|---:|
| 064839, frames 48601-48602 | 22 / 25 | 200199 / 328830 | 4 |
| 102403, frames 10963-10964 | 19 / 40 | 73482 / 342741 | 3 |

The primary fixture covers four of the five compatible families; it does not
validate the fifth original shader. Truncated or absent geometry is excluded.
Later capture frames omit geometry bytes and binding descriptors, so matching
first-frame geometry is reused only for a unique metadata/payload group. This
is a replay assumption, not proof that live bindings or buffers stayed
unchanged. Pool/bone snapshots retain their original first-draw provenance; the
format cannot reproduce intervening writes that it did not record.

The captured DXBC has signatures and shader instructions but no RDEF resource
table. The rig checks reflected signatures and disassembled declarations:
CB1/CB2 and structured t33/t38, including their sizes and strides. It rejects
unexpected contracts and system vertex/instance IDs that normalization could
change. The fused GS consumes only SV_POSITION, so different non-position
outputs do not need a fabricated common signature.

On RTX 5090, all 44 primary and 38 secondary two-frame draws produce byte-exact
positions and identities. The production motion vertex/pixel shaders also
produce byte-exact complete 5120x2880 maps. The primary map contains 794262
valid-history and 63124 rejected-history pixels; the secondary has 883624 valid
and zero rejected pixels. The typed consumer changes only its five identity
declarations from StructuredBuffer<uint4> to Buffer<uint4>. The D3D SDK debug
layer is unavailable; no debug-layer validation is claimed.

Each timed batch clears depth and motion targets, then interleaves each
original-VS opaque depth draw, identity/position production, and its immediate
motion raster. This avoids a prebuilt final depth buffer suppressing early draw
work. Both paths include the common depth cost. Depth is reconstructed from
selected meshes with CullNone, without material alpha or full-scene occluders.
Clean replay state omits the production hook's Get/Restore traffic. These are
selected-depth-plus-motion costs, not isolated patch overhead or complete
game-frame times. The runtime accepts exactly two adjacent frames.

Three release-device runs per fixture use at least 250 ms each of wall and
valid GPU warmup, alternate path order over 15 rounds, and timestamp whole
batches. All 180 measured samples are ready, non-disjoint and positive.

| Capture / run | Baseline GPU median, ms | Typed fusion GPU median, ms | Fused minus baseline, us |
|---|---:|---:|---:|
| 064839 / 1 | 0.317280 | 0.313600 | -3.680 |
| 064839 / 2 | 0.316864 | 0.317088 | +0.224 |
| 064839 / 3 | 0.318592 | 0.311904 | -6.688 |
| 102403 / 1 | 0.224256 | 0.222432 | -1.824 |
| 102403 / 2 | 0.223744 | 0.224352 | +0.608 |
| 102403 / 3 | 0.223232 | 0.222752 | -0.480 |

The difference changes sign, and even the best result saves less than 7 us per
batch. Mean differences across the three run medians are -3.381 us for 064839
and -0.565 us for 102403. Ruled out as a worthwhile production optimization for
these tested workloads: typed identity fusion, because the small synthetic
advantage largely disappears with original shaders and immediate motion
consumption. This does not establish a universal result for other hardware, the
missing meshes, or the latest 64-draw flight.

Manifests are `build/identity-replay/064839.json` and `102403.json`. JSON/CSV
timings are in `benchmark-run1/` through `benchmark-run3/` and
`102403-benchmark-run1/` through `102403-benchmark-run3/` under the same root.
The exporter self-test covers normalization, shared geometry, ambiguous
matching, missing data, frame gaps, round-trip parsing and no-write dry-run.
The C++ gate adds six parser checks to the existing 270 WARP checks, without
depending on private game assets. Replay CLI dry-run also leaves its absent
output directory absent.

Validation: the absolute-path full build exits 0 in
`build/identity-replay-validation.log`. All 61 pooled jobs, three quiet jobs,
the 249-key config contract and packaging/export gates pass, including the new
parser and exporter gates and all 270 existing identity checks. The new replay
compiles without warnings; existing unrelated build warnings remain.

Next, establish removable producer cost locally before choosing another
optimization. The on-foot producer and the rigid settlement producer are
different paths: static rigid skipping cannot remove on-foot work that was
never admitted. Any static fallback still needs unchanged final DLSS selection,
foreground/reactive flags and first-moving-frame history, not just nearly equal
vectors. No new test flight is requested for this result.

### 2026-09-18 -- controlled removal of on-foot motion stages

This experiment measures the existing on-foot mesh producer using the same two
original-shader fixtures. It does not yet measure the separate rigid settlement
producer. Five modes keep identical motion/depth clears and original per-draw
depth work: depth only, depth plus identity, depth plus identity and capture,
full baseline, and depth plus immediate motion raster using precomputed current
outputs. Precomputation stays outside timing.

The hypotheses and discriminators are:

- Identity lookup dominates: depth-plus-identity adds most of the full-path
  increment over depth only.
- Vertex capture dominates: adding capture after identity is expensive, and
  bypassing both with precomputed outputs preserves the complete map while
  removing a substantial part of the full-path cost.
- Rasterization and its transitions dominate: full baseline costs materially
  more than identity-plus-capture, while precomputed raster remains costly.
- Invisible raster work is common: outside-timing occlusion queries around each
  baseline motion draw report many zero-sample draws. These queries use
  reconstructed partial selected-mesh depth, not complete game visibility.

Whole-batch timestamps avoid inserting separate timing queries into every
stage. Modes rotate within each measured round; signed, paired differences
measure the net effect of removal, including changed synchronization and
pipeline transitions. They are not additive isolated-stage costs. Shared map
clears remain in the depth-only control, so it does not represent removing
every resource or command associated with the feature.

Production inspection also rules out treating current invisibility as a
complete skip condition. `weaponMotionDraw` captures positions and identities
before rasterizing, retains bounded duplicate geometry occurrences, and marks
the next history slot with the frame. The following frame requires adjacent
history with matching skeleton identity and projection. Dropping capture for an
invisible object could therefore remove history needed when it becomes visible.
Any later skip must preserve those invariants or prove an equivalent fallback;
a zero-sample raster draw alone does not do that.

Sean also asked whether EDVR can suppress Elite's original draws to address
potentially poor settlement culling. Yes: `hookedDrawIndexedInstanced` forwards
the real game draw before invoking the motion producers, and
`forwardWithVerdict` already returns without issuing it for `kSkip`. Hook
access is sufficient. Suppression could save GPU execution and some driver
work, but cannot recover Elite's earlier CPU scene traversal and draw setup. An
instanced draw may also contain both visible and invisible instances.

High submitted draw counts do not prove poor culling. A separate measurement
must classify original draws by render pass, eye, depth/stencil state and
existing predication, then distinguish zero-sample work from visible work later
overwritten. Shadows, reflections, depth-only passes and side effects cannot be
judged by main-color visibility alone. Existing census hooks expose pass state
and query brackets, but these two replay fixtures lack complete original
material/pass state and full-scene occluders. Their motion-draw occlusion
counts therefore cannot establish the quality of Elite's culling.

The implemented `--cost-breakdown` mode passes the output-poisoning gate:
identity-only regenerates identity while positions remain poisoned;
identity-plus-capture and full baseline regenerate both byte-exactly. Full
baseline and precomputed-output motion maps match exactly, with the same
nonzero valid/rejected counts as the previous replay. Precomputation is an
idealized control, not an identity cache proposal or proof of safe input reuse.
The five modes retain the same clear operations, original shader inputs, draw
order and reconstructed depth dimensions.

Three release-device RTX 5090 runs per fixture use 15 rounds with rotating
five-mode order. Every mode accumulates at least 250 ms of valid GPU warmup;
all 450 measured samples are ready, non-disjoint and positive, with no
unhealthy warmup samples. Paired differences below are medians of same-round
differences, not differences between independently calculated mode medians.

| Capture / run | Full minus depth control, ms | Full minus precomputed motion, ms | Precomputed motion minus depth control, ms |
|---|---:|---:|---:|
| 064839 / 1 | 0.355616 | 0.346304 | 0.008928 |
| 064839 / 2 | 0.359616 | 0.348128 | 0.009312 |
| 064839 / 3 | 0.349600 | 0.340288 | 0.008896 |
| 102403 / 1 | 0.228032 | 0.219136 | 0.008960 |
| 102403 / 2 | 0.223712 | 0.215136 | 0.009056 |
| 102403 / 3 | 0.221248 | 0.212640 | 0.008832 |

These controlled batch increments show that producing fresh inputs and their
associated dependencies are the larger target in these workloads. Ruled out as
the main explanation for this replay's producer increment: motion-raster
arithmetic alone, because the same raster with precomputed inputs adds only
8.8-9.3 us to the shared depth control. This does not isolate a particular
driver barrier or make the full precomputed saving recoverable in production.

The intermediate slices are less stable. Primary identity-minus-depth paired
medians are 225.152, 156.704 and 155.904 us; adding capture gives 22.560,
30.016 and 79.744 us. Raw samples occupy different timing bands across mode
orders. Secondary identity increments are steadier at 136.544-137.120 us, with
capture adding 6.688-7.008 us. Fresh full raster adds 73.408-112.416 us over
identity-plus-capture, far more than the approximately 9 us with ready inputs.
Do not add these slices or treat them as stable isolated-stage prices. Primary
full mode medians are 0.374-0.385 ms in this mixed experiment, versus
0.317-0.319 ms in the prior two-mode fusion experiment; cross-experiment
absolute timings are not interchangeable.

Outside-timing occlusion probes find 13/22 zero-sample motion draws in 064839,
covering 141600/200199 submitted indices (70.7%), and 10/19 in 102403, covering
56427/73482 indices (76.8%). These are counts for the EDVR motion raster
against reconstructed partial depth. They do not classify offscreen versus
occluded geometry, measure the cost of those particular draws, prove zero
final-game contribution, or establish a cheap current-frame visibility test.
They support evaluating conservative visibility while retaining the
capture/history needed when an object becomes visible.

Artifacts are under `build/identity-replay/064839-cost-run1/` through `run3/`
and `102403-cost-run1/` through `run3/`, each containing
`identity_fusion_replay_cost.json` and `.csv`. They include raw timestamps, CPU
submission time, per-mode warmup and health, paired differences, and per-draw
occlusion counts. The bounded replay scope remains unchanged: two historical
on-foot subsets, opaque CullNone selected-mesh depth, no complete
material/occluder state, and no production Get/Restore traffic. These results
must not be scaled into a promised game-frame improvement.

Validation: `build/motion-producer-cost-validation.log` records an exit-0
absolute-path full build: 61 pooled jobs, three quiet jobs, all 249 config keys
and packaging/export gates pass. The replay parser/plan reports 11 checks; the
identity rig reports 274 checks with zero failures, including the new CLI
contracts. Targeted hardware gates and no-write dry-run pass; the new code
compiles without warnings. Frontier remains on 89b9055 and no new test flight
is required for this local experiment.

### 2026-09-18 -- original-game-draw visibility and cost diagnostic

Sean authorized the next measurement build for Frontier. The target is Elite's
original draws, not EDVR's motion producer. Competing explanations are measured
together:

- Many submitted draws do substantial work yet pass no depth/stencil samples:
  repeated zero-sample buckets with geometry activity and meaningful GPU
  intervals would identify candidates for conservative culling research.
- GPU rejection is already cheap: zero-sample buckets close to the instrument
  floor would weaken a draw-suppression optimization even if counts are high.
- The game already predicates or encloses draws in visibility queries: separate
  guard counters identify this population without changing its query results or
  pretending skipped measurements have zero visible samples.
- Main-view counts include other passes or altered EDVR draws: exact target
  state, eye labels, original shader hashes and modification flags keep those
  populations separate. Unknown eye identity remains unknown.

The exact timing seam is the saved native draw function inside each of the four
ordinary draw lambdas, plus DrawAuto and the two indirect variants. Timing the
enclosing `forwardWithVerdict` lambda would include post-call EDVR motion work
and is therefore incorrect. `t_colourOriginal` limits ordinary samples to the
first issuance; later EDVR reissues are excluded. Original shader/verdict
provenance is captured before fix wrappers, while selected samples describe the
actual pipeline at issuance and flag modifications.

Selection uses three unique draw ordinals derived from the preceding frame's
total, rather than always taking the first match near the front of the frame.
Detailed D3D state is read only for selected draws. Count changes, missing
target ordinals, unknown indirect counts, command-list coverage and fixed-table
overflow are reported. No draw is removed or replaced by this diagnostic.

Occlusion and pipeline-statistics queries accompany a shared-domain GpuTimer.
Predication, active external counting/disjoint queries and uncertain query
tracking must reject admission before any query Begin/End. Raw query
trampolines keep the instrument out of the game's query tracker. Polling is
bounded at later frame boundaries with DONOTFLUSH; pending, failed, expired and
disjoint outcomes remain distinct from a completed zero-sample result.

Interpretation follows Microsoft's definitions: an [occlusion
query](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)
counts samples passing depth/stencil, and [pipeline
statistics](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_query_data_pipeline_statistics)
describe geometry and shader invocations. Neither is a final-color visibility
test or proof that an entire object can be removed. Instrumented draw times
also include query/pipeline effects and are not additive frame-saving
estimates. The earlier replay's large timing changes across command sequences
make that limitation material here.

The diagnostic follows `advanced.app_gpu_timing`, which already defaults to on.
It uses 64 reusable query slots and 128 detailed buckets, collects for 600
frame boundaries, and allows at most 120 more boundaries for outstanding
results. It repeats so the trip to the settlement cannot exhaust the capture.
At most 16 eligible slots are visited at each boundary, starting at least four
frames after submission. A small empty-bracket calibration exposes query cost;
it is not subtracted to manufacture an estimated saving.

The next flight should keep the ship landed, facing a busy settlement for 60-90
seconds, then spend 60-90 seconds on foot in the same area. Keep AA off and
render settings fixed, following Sean's new baseline run. No eye dump or weapon
draw is required for this census. Each window needs to distinguish completed
zero-sample results from unavailable measurements, with early
enabled/observed/submitted messages exposing a dead instrumentation path.
Whole-frame timing remains an instrumented observation, not an optimization
A/B.

Targeted validation passed 66 new probe checks and 162 live-query-hook checks.
WARP draws retain the expected pixels while separating visible and rejected
samples. Guard tests verify no diagnostic bracket is submitted under
predication or active counting/disjoint/overflow state. Other checks cover
owner context/thread, non-flushing reads, query errors, bounded expiration,
ready visibility surviving a pending statistics timeout, clean re-enable,
600-frame report/restart, and the native-call seam closing before motion work.
Both visible and rejected fixtures verify every RGBA pixel of the 16x16 target.
The full build passed 62 pooled rigs, three quiet rigs and the 249-key config
contract. The diagnostic preserves the installed AA/render settings.

### 2026-09-18 -- AA-off landed baseline while the diagnostic was prepared

Verified Frontier `edvr_gfx_20260918_090413.log` as exactly
`v0.17.0-10-g89b9055`, build `6AAD36B4`. The sandbox initially hid the Frontier
Products directory; the sanctioned `edvr_log.py` locator succeeded outside the
sandbox. The stale Steam log/dump was rejected as a build mismatch.

The final 17.64-second AA-off benchmark window records CPU p50/p95/p99
10.199/11.830/13.386 ms and GPU 10.875/12.447/13.613 ms at input 2481x2121,
output 3072x3264. Application-render samples around the settled view are
10.35-11.36 ms. XR copy/compose remains about 0.027/0.054 ms GPU, so transfer
and composition do not explain the approximately 11 ms application cost.

AA and UI depth are off; Deferred UI reports zero, and the dump has no terrain,
mesh or holo motion records. Remaining EDVR work includes particle billboard
replacement (1152-4164 draws per trailing ten seconds), Map write tracking
(0.193-0.318 ms/frame), and draw-hook CPU sampling (2.567 ms mean, 4.745 ms max
at 09:06:39). The hook timer excludes the forwarded game call but can include
EDVR reissues. Therefore this strengthens the case for investigating original
game draws without proving that all remaining CPU/GPU cost belongs to Elite.

`eye_090656` confirms mode=off, frame 4115, 2481x2121 and a nearly unchanged
landed cockpit view across the 16 crops. Cockpit/HUD foreground, exposed
terrain and the settlement share the view. The image cannot identify invisible
submitted objects; MeshFallback is unavailable because there is no matching
first-eye temporal snapshot. Capture causes a scope change at 09:06:56 and a
later 462.9 ms diagnostic frame; those later capture costs are excluded.

### 2026-09-18 -- e11f532 flight: disjoint guard excludes the census

Verified Frontier `edvr_gfx_20260918_092403.log` as `v0.17.0-15-ge11f532`,
build `6AAD56E4`. The original-draw path runs and completes its 600-frame
windows, but reports zero submitted queries. Every reached selection in the
inspected settlement windows is rejected by the disjoint-query guard; other
query conflicts, predication and overflow are zero.

Ruled out: the initial guard admitting enough live original draws for a useful
census, because the verified flight reports only `no-data` windows with
`submitted=0` and all reached selections attributed to `disjoint`. This is
unavailable evidence, not zero visible geometry. Identify whether the tracked
query is genuinely external or an EDVR interval crossing the internal marker
boundary, and reproduce the complete hook interaction before another flight.

All 43 completed windows have no samples; windows 5-43 plan 68786 selections,
miss 806 changed-count ordinals and reject the remaining 67980 on disjoint. The
probe never starts its own timer in this flight. The log does not identify the
active query's owner; a whole-frame game timing query is plausible but not
proven. No correction may assume that attribution.

Usable whole-frame results, with AA off, input 2481x2121, output 3072x3264:

- Landed completed benchmark windows 4-5: CPU p50 10.353/10.768 ms and GPU
  11.045/11.448 ms. GPU p95 is 12.697/12.863 ms.
- Explicit Disembark at 09:27:03.489 precedes stable on-foot windows 7-9: CPU
  p50 7.112/7.384/7.238 ms and GPU 9.750/10.086/9.970 ms. GPU p95 ranges from
  10.576 to 10.849 ms.
- Startup, transition windows 6/10 and the exit scope change are excluded;
  there is no eye-capture contamination. Copy/compose stays near 0.025/0.050 ms
  GPU.
- Draw-hook CPU means are 4.180-4.315 ms landed and 2.814-2.842 ms on foot;
  these include diagnostic work and EDVR reissues, not only feature cost. Map
  tracking is 0.234-0.260 / 0.121-0.134 ms/frame; particle replacement is
  2508-2684 / 1636-1712 draws per ten seconds. The landed totals remain in the
  previous AA-off run's broad range; this is not a controlled overhead A/B.

Local reproduction before a correction: a real WARP disjoint query spanning the
draw frame reproduces zero submitted samples and fails the new admission test,
while all rendered pixels remain correct. A disjoint timing scope does not
justify discarding independent occlusion/pipeline measurements. The implemented
correction borrows timestamps only from an already-open EDVR frame clock, never
starts another disjoint interval, and retains visibility results with
explicitly unavailable timing if that clock is absent.

A separate CPU microbenchmark uses the production shared timing owner lookup on
a WARP context, /O2 x64, five processes with nine alternating 20-million-call
rounds. Repeated shared-owner lookup costs median 18.996-19.086 ns/call; cached
context/thread validation costs 1.550-1.582 ns/call. The measured difference
scales to 0.4005-0.4026 ms at 23000 draws/frame. It does not establish the
cause of the larger flight hook-timer increase. First adoption, selected draws
and frame boundaries still need full validation; unsampled draws can use the
already-proven context/thread identity.

The correction passes 96 original-draw WARP checks and 713 shared-timing
checks. A real external disjoint wraps the shared frame clock and visible or
occluded sampled draws. Both cases retain exact 16x16 RGBA output, valid
pipeline statistics, respectively nonzero/zero passed samples, and valid
borrowed timestamps. Instrumented timing callbacks prove one shared disjoint
Begin/End, with no additional disjoint from the probe. Without a frame clock,
visibility/statistics succeed, timing is explicitly unavailable, and no
timestamp/disjoint markers are issued. An active standalone producer is not
accepted as a frame clock.

The cached ownership check is tested through actual shared-domain shutdown:
Select can still identify planned ordinals, but selected Begin submits no
queries and Frame performs no polling/mutation after the domain disappears.
Clean rebind works. Predication, counting-query and overflow exclusions remain.
The live query's owner remains unattributed; the corrected path is valid under
the real external-query condition reproduced locally. The full build passed all
62 pooled rigs, three quiet rigs and the 249-key config contract. The next test
uses the same AA-off landed/on-foot protocol, without an eye dump, with
existing live settings preserved.

### 2026-09-18 -- ea2a7f9 flight: original-draw census completes

Verified the exact Frontier `edvr_gfx_20260918_094806.log` as
`v0.17.0-16-gea2a7f9`, build `6AAD5C75`. AA is off throughout; the configured
`dlss="k"` token does not mean DLSS is active. Input is 2481x2121 and XR output
3072x3264 on Quest 3 / VirtualDesktopXR at 90 Hz. There is no capture activity.

Stable benchmark windows 6-8 are landed: CPU p50 9.934/10.276/9.610 ms, GPU
10.556/10.782/10.540 ms; GPU p95 11.731-12.107 ms. Disembark is explicit at
09:51:24.633. Completed on-foot windows 10-11 have CPU p50 6.986/7.110 ms and
GPU 9.526/9.675 ms; GPU p95 10.446/10.556 ms. Startup, transitions 5/9, and
scope-changing window 12 are excluded. The means of those window medians are
9.940/10.626 ms CPU/GPU landed and 7.048/9.601 ms on foot, not pooled
percentiles. Particle replacements per ten seconds are about half those of the
preceding flight, so lower whole-frame times are not a controlled A/B.

Original-draw windows 19-29 are fully landed and 31-43 fully on foot. Landed
population is 22743-23827 original native calls/frame; on foot 18747-19180.
About three calls/frame are sampled. Zero depth/stencil-passing samples occur
in 74.6-77.3% / 74.6-77.6% of those calls. Zero pixel-shader invocations occur
in 83.1-84.9% / 86.9-89.7%. These are submitted draw calls across passes, not
unique meshes or final-color visibility. Geometry may still execute when the
pixel shader does not.

The query correction is confirmed live: visibility and pipeline results are
complete, and timing is complete except for one unavailable sample in on-foot
window 36. There are no invalid, expired, ring-full, not-issued or
query-tracking-overflow failures. An external disjoint context remains present
but no longer prevents measurement. The detailed bucket table is a separate
limitation: roughly 1305-1528 records/window cannot acquire a new detail
bucket. All pass aggregates still include these records. A retained top-16
shader ranking is not a ranking of the entire scene.

CPU interpretation correction: `DrawClock` starts its subtracted first-call
interval before `originalDrawNativeBegin` and ends it after
`originalDrawNativeEnd`; the indexed-instanced path also includes
`weaponMotionDraw` inside that interval. The ownership lookup optimized in
ea2a7f9 is therefore excluded from the reported hook-work counter. Its
unchanged 4.2 ms landed / 2.76-2.80 ms on-foot level neither measures nor
refutes that specific optimization. Metadata work outside the first-call
interval can still be included. The local microbenchmark remains a scale
estimate, not a measured flight saving.

Across all 40 ready census windows, 70358/70358 submitted queries return
visibility and pipeline statistics; 70295 return timing and 63 have timing
unavailable, chiefly at startup. The stable subsets are 14700/19271 zero landed
(76.28%) and 17696/23222 zero on foot (76.20%). Empty-bracket calibration
completes 400/400 times, averaging 3.237 us. Zero-result EyeColor draw
intervals average about 9.66 us landed; SourceColor about 8.73 us on foot.
These include query perturbation and cannot be added, extrapolated or reduced
by the empty-bracket average to estimate whole-frame savings.

The 128-key detail table overflows on 76.64% of landed observations and 83.27%
on foot. Printed top-16 retained details cover only 10.55% / 6.52% of the
stable subsets, so they identify candidates without establishing a global
ranking. Complete pass aggregates remain available. Across the entire flight,
unmodified SourceColor is 15993/18259 zero (87.6%) and EyeColor 18421/34895
(52.8%); these include other flight phases and should not replace the stable
subset percentages above. All eye labels are unresolved index 2. SourceColor
denotes a target-size match, not UI or scenery by itself.

Three candidate scene families have mixed outcomes in retained printed details:
`EB5234DB6ADB491D / CB9F297EFF264251` is 1900/2103 zero; `5B4D8E894EEDA8B4 /
4375B72964F386CD` is 482/535; `BBE58E40FE88EC80 / DB3E8D20CF53FBC0` is 202/213.
The first VS is the known material mesh family used by source-mesh motion,
including scenery; family membership does not prove that every instance is
static or rigid. Existing bytecode classifies the other two as instanced
surface-material paths (below). Observed source buckets use triangle-list
indexed instancing, reverse-Z GREATER_EQUAL depth and back-face culling. The
current key does not establish stencil fail/depth-fail operations, so zero
passed samples alone cannot prove absence of stencil side effects.

ruled out: shader-wide suppression of these scene families, because each family
also produces depth/stencil-passing samples in this flight.

Zero results also do not establish negligible shader work: known curved-screen
pair `4EF6DDB075A927FA / 85565E9261812E2F` returns zero in all 738 printed
samples while accumulating 2.03 billion pixel-shader invocations. Known
panel/composite pair `A888D51024D9798E / 015EF9349EC097E8` returns zero in all
1512 printed samples. Neither is a settlement-geometry suppression target.

The existing Frontier shader dumps contain both previously unknown pairs.
Disassembly shows both VS families reading structured t33 (336-byte instance
records), t38 (48-byte records), CB1/CB2 and packed instanced mesh inputs. The
5B4D/4375 pair passes material UVs, samples two texture arrays and writes four
color targets. BBE5/DB3E additionally handles face invariant, normals, tangents
and a reconstructed tangent basis, with screen/2D and texture-array samples
feeding four targets. These are surface-material paths consistent with
deferred/G-buffer rendering, not screen/UI shaders. Bytecode does not establish
the object or material name, static status or a safe culling rule.

Next: narrow measurement to draw identity, recurrence and transitions from zero
to visible in these instanced material families, with coarse family keys that
retain complete aggregates. Any later suppression needs a cheap conservative
current-frame predicate and proven pass/state eligibility, including stencil,
UAV and stream-output effects. Per-draw query brackets are diagnostic, not a
proposed production optimization. Another broad census flight is not needed to
reconfirm the high rejection rate.

### 2026-09-18 -- targeted material-draw recurrence probe

The accepted next step measures the three observed instanced material pairs; it
does not suppress original draws. The existing valid census supplies the reason
to narrow measurement: roughly 76% rejection in the stable settlement, mixed
visible/rejected outcomes within each family, and severe fragmentation of the
128 detailed buckets. Earlier object-probe work already ruled out draw order
and pool slot alone as persistent identity.

The discriminating observations for the next flight are:

- Repeated zero results for an unchanged captured instance/model payload,
  versus zero-to-visible transitions with that same payload. These measure
  recurrence within a sampled cohort, not proof of permanent invisibility.
- Changed draw bindings, instance IDs or model records at the held ordinal.
  These break the comparison chain instead of masquerading as object motion or
  a visibility transition. Reordered query results and missing frames must also
  break a chain.
- Unsupported layouts, missing pools, draws exceeding the payload limit and
  skinned records. These need explicit counts; an inactive payload probe is not
  evidence that no objects change. A nonzero bone base means t38 affects the
  geometry, so identical t33 bytes are insufficient for a rigid comparison.
- Stencil fail/depth-fail operations, UAVs, stream output and extra shader
  stages. Zero passed samples alone do not establish that suppressing a draw
  has no other effects.

Only selected draws may query resource bindings and capture data. The instance
ID range is copied on the GPU; a bounded compute gather reads the referenced
336-byte t33 records into private staging storage. There is no CPU read of game
WRITE_DISCARD memory or whole-pool readback. Query results and payloads are
joined by their submission, with nonblocking polling. Input-layout eligibility
follows the actual descriptor recorded at creation, irrespective of which
compatible shader Elite used when creating a shared layout.

Even exact payload recurrence remains a diagnostic match, not a globally unique
object ID: identical objects, changing material/constant contents and
uncaptured animation state require further evidence. A cohort follows a
selected family-local ordinal for several frames, so its visibility results are
deliberately biased follow-ups, not an updated population percentage. Complete
coarse family aggregates retain the samples that the old detailed table could
not rank.

Local validation exposed a real ordering limit that an isolated run missed.
With 32 concurrent WARP test processes, both samples completed (one zero, one
nonzero), but the poll cursor retired the newer sample first: `same=0 order=1`
with no capture, payload or binding failures. Refusing the backwards comparison
was conservative but lost the useful adjacent pair. Completed samples now wait
in the bounded slot table for earlier samples of their family to retire;
polling remains nonblocking, and failures/timeouts still release the queue.
Calibration slots do not participate in the family ordering barrier.

ruled out: payload corruption as the cause of the parallel transition-test
failure, because both payload/query results completed and the explicit failure
snapshot isolated reversed retirement order.

The final focused Release/WARP rig passes 618 checks. Coverage includes actual
GPU payload capture and state preservation, reused buffer contents, exact
payload zero-to-visible transitions, geometry-binding changes, missing frames,
delayed-query retirement order, more than 128 count variants, and capture
timeout followed by successful reuse. Thirty-two concurrent focused runs
validated the ordering fix; the last calibration exclusion was then compiled
and checked in the final focused run. The full build then passed all 62 pooled
rigs, three quiet rigs and the 249-key configuration contract, including the
618-check targeted probe rig under the parallel build workload.

Live bounds are three families, at most one selected draw per family per frame,
24-frame cohorts, 32 instances per captured draw, 64 in-flight slots, and the
existing 600-frame collection / 120-frame drain windows. Unsupported larger
draws retain visibility queries but cannot establish full-payload recurrence.
The selected gather precedes the original-draw query bracket, so its GPU cost
is outside that per-draw interval and remains part of whole-frame timing. This
is an instrumented test build, not an asserted frame-time improvement.

### 2026-09-18 -- targeted flight: sparse recurrence and changing visibility

Verified `edvr_gfx_20260918_104357.log` with the sanctioned reader against
`dcd0a9f`: version `v0.17.0-18-gdcd0a9f`, build `6AAD6977`, linked at 16:40:23
UTC. This is the targeted probe, with AA off, VirtualDesktopXR / Quest 3 / 90
Hz, input 2481x2121 and output 3072x3264 per eye during gameplay. Startup uses
a narrower input briefly; exclude it. No eye-dump operation is logged.

All 29520 submitted brackets finish with visibility, pipeline statistics,
timing and complete unskinned payloads. All capture-error, unsupported,
out-of-order, invalid, expiry, ring-full and query-overflow counters are zero.
Of 30078 planned brackets, 558 miss their held ordinal. Selected stencil
fail/depth-fail operations, UAV, stream-output and GS/HS/DS flags are zero. The
detailed bucket table overflows 15295 times; coarse family and recurrence
counters do not depend on that table.

Original-draw windows 19-25 are the stable landed EyeColor/pass-1 phase; window
26 mixes the transition; windows 27-34 are FootSourceColor/pass 2. These window
numbers are separate from the native benchmark window numbers.

| Stable phase | Ready samples | Exact binding + payload | Payload changed | Binding changed | Gaps |
|---|---:|---:|---:|---:|---:|
| Landed | 12303 | 193 (1.57%) | 1888 (15.37%) | 9643 (78.51%) | 558 (4.54%) |
| On foot | 14354 | 705 (4.92%) | 5751 (40.13%) | 7297 (50.92%) | 577 (4.03%) |

The four categories partition `ready - 3` in every ready collection window. One
first observation per family has no predecessor: 21 exclusions landed and 24 on
foot give percentage denominators 12282 and 14330. The 24-frame cohort changes
count as gaps, not those initial exclusions. Binding changes include draw
arguments, resources, viewport/scissor and retained pipeline state; IDs are
compared separately as payload. The composite binding counter cannot say which
component changed, nor can these counters estimate static-object counts.

Across the whole flight, payload comparisons record 6554 ID changes, 6851
model-byte changes and 1327 ID-only changes. All model changes include pose
fields; no other model fields differ. These are comparisons at held ordinals,
which may refer to different objects, not evidence that settlement structures
move. Byte-identical model data with changed IDs also does not establish a
unique object or prove a particular pool-management mechanism.

There are three zero-to-nonzero visibility transitions and six in the other
direction, all landed, despite identical captured bindings, IDs and t33 data in
adjacent frames of the same cohort. Camera constants, depth/stencil contents
and other shader/resource contents are not part of that identity. On-foot
absence of transitions is inconclusive given its low exact recurrence.

ruled out: reusing a previous zero-sample result solely because captured draw
bindings and ID/t33 payload are unchanged, because three such draws pass
samples on the following frame.

The held family/pass aggregates have zero-sample fractions of
84.20/81.74/88.62% for families 0/1/2 in pass 1, and 89.59/87.69/91.76% in pass
2. They include all collection windows with that pass and are deliberately
biased follow-ups. They must not replace the prior broad census's population
estimate or be multiplied by per-draw timings to claim savings.

Across all selected samples, 25408 have zero passed depth/stencil samples. Only
82 (0.28%) have nonempty IA/VS work but zero clipped primitives. Another 25304
(85.72%) have clipped primitives but no pixel-shader invocations. These
hierarchical pipeline categories show that simple frustum rejection is unlikely
to recover most of the observed work in these families. The counters do not
separate depth, stencil and other raster rejection, and do not by themselves
prove that a draw has no side effects.

Interpretation uses Microsoft's [pipeline-statistics field
definitions](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_query_data_pipeline_statistics):
`CPrimitives` accounts for primitives after clipping and `PSInvocations` counts
pixel-shader executions. The preferred next investigation is an inference from
these counters, not a direct measurement of occluders.

Completed native benchmark windows 4-5 give landed CPU p50 10.326/10.807 ms and
GPU p50 10.824/11.298 ms, with GPU p95 12.475/12.654 ms. Completed on-foot
window 7 gives CPU p50 7.513 ms and GPU p50/p95/p99 10.073/10.888/11.298 ms.
All these samples are valid and complete. The final 28.047-second window ends
on a scope change: CPU p50 7.709 ms, GPU 10.108 ms, one GPU sample missing.
Exclude the shorter transition window 6. These are slightly slower than
ea2a7f9, but separate flights do not isolate scene variation from probe
overhead. The gather cost sits outside the original draw bracket and inside the
segmented application timing. No performance improvement is claimed, and the
native CPU/GPU medians must not be added together or treated as complete frame
periods.

Offline feasibility check: `weapon_motion.cpp` already captures original
post-VS `SV_POSITION` through stream output for supported indexed draws. It
replays vertex work and is bounded to 131072 indexed vertices. That can help
validate geometry locally, but does not provide a cheap conservative bound
before the original draw. The current recurrence capture has IDs and t33
records, not mesh extents; the shader paths also depend on packed geometry,
constants and potentially t38. Pose alone is insufficient. An NDC bound across
nonpositive clip-w is unsafe; a frustum proof must respect homogeneous
clipping.

The culling candidate is an offline feasibility and break-even check for
conservative bounds against current-frame depth, with uncertain cases retaining
the draw. It must first establish complete geometry coverage, correct
depth/stencil/pass semantics, conservative behavior during camera/occluder
changes, and a net win after its own GPU work and submission cost. Replaying
all vertices or waiting for GPU results on the CPU is not an assumed
optimization. A hook-level skip cannot recover Elite's already-completed scene
traversal and draw preparation. No further headset run or suppression build is
justified by this flight alone. Frontier remains on diagnostic dcd0a9f; this
entry changes documentation only.

The user's FPS follow-up changes the immediate priority: despite application
CPU/GPU readings around 10-11 ms, the landed cockpit runs in the mid 60s. The
log confirms it. Native benchmark windows 4 and 5 admit 1952 and 1894 complete,
valid frame sequences in 30 seconds: 65.07 and 63.13 FPS, or mean frame periods
of 15.37 and 15.84 ms. Independent Present counts report 1265, 1283 and 1253
frames per 20 seconds (63.25, 64.15 and 62.65 FPS). The on-foot completed
window admits 2416 in 30 seconds, or 80.53 FPS. Sample admission is not
downsampled; the asynchronous GPU drain joins results by frame sequence.

Code trace: `native_timing.cpp` requires four CPU application segments,
covering game work after WaitGetPoses, the two eye treatments, and between-eye
game work. `native_runtime_host.h` opens the first only after the WaitGetPoses
caller's `service.invoke` returns; each Submit closes it before the
`renderRoute.invoke` handoff. The between-eye segment resumes after that
handoff returns. GPU application timing likewise sums four producer timestamp
intervals through `gpu_frame_timing.cpp` / `gpu_span_state.h`. These are
neither complete frame-cycle durations nor exclusive CPU/GPU busy-time
measurements. They omit caller-to-owner queue/rendezvous intervals, pose wait,
transfer/composition, and the interval after the right Submit before the next
pose-wait return. Sub-budget segment totals are therefore insufficient to
establish 90 FPS.

The matching native log, `edvr_openxr_20260918_104358_542_21788.log`, also
verifies as dcd0a9f. Landed submission window 6 reports owner Submit p50 0.3223
ms, producer dispatch 0.1135 ms, receive 0.0763 ms, XR swapchain wait 0.0002
ms, xrEndFrame 0.0184 ms and xrWaitFrame 0.0072 ms, with zero pacer block.
These fields are nested, so must not be added. Gfx owner pose-wait averages are
about 0.03 ms. They do not explain the cadence gap by themselves, and do not
include the game caller's wait before the owner starts the body. Landed pacing
is runtime; foot changes to deferred/turbo at 10:46:47.681, so the landed/foot
comparison also changes pacing mode.

Immediate next step: account for one complete cycle on the same caller and
sequence, including WaitGetPoses entry/return, each Submit entry/return,
caller/owner handoffs and the right-Submit-to-next-WaitGetPoses interval.
Correlate those with Present cadence and the existing application segments. Do
not attribute the gap to a fixed 5 ms runtime stall by subtracting a median
from a mean; per-frame attribution is still missing. This accounting takes
priority over a suppression implementation. No pacing or culling fix has yet
been justified, and no additional flight has been requested.

### 2026-09-18 -- complete frame-cycle accounting

The user approved a diagnostic build for the uncovered frame time. Three
possibilities need distinct signatures in the same run:

- Game work or scheduling outside the measured application segments: a large
  interval from the completed stereo Submit return to the next pose-wait entry.
- Caller/owner handoff cost: large caller roundtrips with small owner bodies,
  distinguished from time parked on the render thread during Submit.
- Runtime work inside the owner: a large owner pose-wait or Submit body, with
  the existing nested XR/transfer phases identifying which part accounts for
  it.

The complete-cycle boundary is one host `waitPoses` caller return to the next.
This includes the next frame's pose-wait call. Partition each valid cycle into
game work before the first Submit, first Submit roundtrip, game work between
eyes, second unique-eye Submit roundtrip, the post-stereo gap before the next
pose wait, and that next pose-wait roundtrip. Support either eye order.
Owner-body and render-thread-park measurements are nested inside the caller
roundtrips; they must not be added again. Compute the accounting residual on
each paired cycle, not by subtracting separately aggregated medians.

The fixed 4096-sample collector reports about every 30 seconds, with partial
reports on scope changes. Separate enabled, first-complete and aggregate
markers distinguish an inactive probe from valid zero-cost phases. Incomplete
pairs, duplicate eyes, caller-thread changes, rejected calls, clock/order
failures, scope changes and capacity overflow have visible counts. Actual
caller/wait thread IDs are retained even for zero-valid windows. Scope is
captured on the owner and returned through the completed call, including
runtime generation, feature epoch, pacing, scene readiness, should-render and
dimensions. Frame pairing uses the compositor sequence, independently of the
graphics timing provider's sequence counter.

The log reports per-phase mean/p50/p95, the paired residual, valid cycle sums,
and separate caller-wait and valid-sample rates over the observation interval.
The total cycle and residual are not extra exclusive phases. Compare means on
the same accepted samples; do not add nested owner/park spans or subtract
unpaired percentiles. Collection adds CPU clock reads and bounded storage, with
no GPU queries, readbacks or changes to the existing thread handoffs.

The focused native rig passes 658 checks. Deterministic cases assert exact
exclusive and nested phase durations with injected delays, reverse eye order,
zero post-submit gap, partial/invalid calls, wrong threads, mismatched
sequences, scope flushes and zero-valid reports. Review found and corrected a
collision between scope change and the 30-second gate that could overwrite an
unread report. The fixture also triggers the production reporter and verifies
its first-complete marker; its report has zero accounting residual. The
complete outer host Wait/Submit loop was traced through the existing
owner/render dispatch code rather than adding another synthetic runtime API.
The previous verified native log independently confirms that Elite reaches this
non-owner Submit wrapper and that caller-thread trace output reaches the
durable log.

This is timing instrumentation only. No draw suppression or pacing behavior
change is part of this build. The next flight should hold the same landed
cockpit view with AA off for about 90 seconds; no on-foot leg or eye capture is
needed to answer the immediate question.

Full build validation passes: all 62 pooled rigs, three quiet rigs and the
249-key configuration contract, including the 658-check native rig and the
618-check original-draw rig. Build log:
`build/frame-cycle-build-validation.log`. The Frontier package uses the
sanctioned installer and preserves the live INI; verify the installed package
against the final committed build before flying.

### 2026-09-18 -- landed cycle locates the missing 4 ms after stereo Submit

The user ran the diagnostic. Both Frontier logs match 7cfb5a6, version
`v0.17.0-20-g7cfb5a6`, checked with `tools/edvr_log.py --expect-build 7cfb5a6`:
`edvr_gfx_20260918_112719.log` and `edvr_openxr_20260918_112720_574_6064.log`.
The native log uses UTC, six hours ahead of the graphics log's local time.
Native enabled and first-complete markers are present. The route is direct on
caller/render thread 7364, with runtime owner thread 5236. Environment is Quest
3 / VirtualDesktopXR / 90 Hz, AA off, input 2481x2121 and output 3072x3264 per
eye, runtime pacing throughout. There is no on-foot leg or eye capture in this
run.

Frame-cycle windows 7 and 8 are the clean landed comparison, ending at
17:29:42.051 and 17:30:12.054 UTC. Each covers 30 seconds; all 1984 and 1986
admitted cycles are valid, respectively. Every missing counter and the paired
accounting residual is zero. Both retain generation 1, feature epoch 3,
scene-ready and should-render true, with consistent caller/wait threads.

| Exclusive phase, paired mean ms | Window 7 | Window 8 |
| --- | ---: | ---: |
| Previous pose-wait return to first Submit entry | 10.4218 | 10.5547 |
| First Submit caller roundtrip | 0.1574 | 0.1469 |
| Between eye calls | 0.0005 | 0.0005 |
| Second Submit caller roundtrip | 0.2464 | 0.2437 |
| Second Submit return to next pose-wait entry | 4.2338 | 4.1133 |
| Next pose-wait caller roundtrip | 0.0622 | 0.0481 |
| Complete cycle | 15.1221 | 15.1072 |

The six phase means sum to the full cycle within 0.0001 ms of printed rounding.
Measured caller rates are 66.133 and 66.200 FPS; graphics Present counts
independently show 64.4-69.75 FPS in nearby 20-second windows. This explains
why application-work times near 10-11 ms coexist with mid-60s FPS: those
shorter timers do not cover the complete 15.1 ms cycle. CPU and GPU work
overlap and are not additive.

The post-stereo gap is sustained, with p50 4.056/3.966 ms and p95 5.472/5.215
ms. Nested owner-body means are 0.0452/0.0310 ms for the next pose wait,
0.1355/0.1338 ms for first Submit and 0.2371/0.2344 ms for second Submit. Their
total difference from the corresponding caller means is only 0.0482/0.0395 ms.
Render-park means also closely track Submit roundtrips; these nested spans must
not be added to the exclusive cycle.

Ruled out: the captured native Submit/WaitGetPoses caller handoffs or pose
pacing as the dominant missing landed interval, because all three caller
roundtrips together cost only 0.44-0.47 ms while the separate post-stereo gap
costs 4.11-4.23 ms. This does not rule out other calls between those wrappers,
including PostPresentHandoff, or attribute the whole gap to Elite.

Window 2 has zero valid cycles and is not timing evidence. Windows 4/5 are
menu-like, with short pre-Submit work and long pose waits despite scene-ready
being true; window 6 mixes menu and gameplay. Window 9 ends in a scope change
and includes renewed pose waiting, so it is not the clean landed comparison.
Separate graphics benchmark windows 4-6 report CPU p50 9.993/10.739/9.932 ms,
GPU p50 10.613/11.180/10.860 ms and GPU p95 12.197/12.550/12.095 ms. Their
boundaries differ from the cycle probe; do not subtract these medians from
cycle means. Rendering itself also still exceeds the 11.111 ms budget on some
frames.

Source review narrows the next measurement, not the cause:

- `hookedPresent` calls the real DXGI Present near its start and already
  records that duration with `perfMonitorNotePresentWait`.
- Its later `kCpuBoundary` timer covers the guarded EDVR frame work, but not
  all bookkeeping before it or the trailing `renderBoundaryPresent` callback.
- That callback can pump the native render queue and optional frame work.
  Neither its duration nor the existing Present/boundary totals is persisted by
  the native long-frame logger, so this flight cannot attribute their
  contribution to the gap.
- `PostPresentHandoff` can dispatch to the runtime owner. Its owner body does
  no XR frame submission, but its caller roundtrip is not measured by this
  probe. It must be distinguished if Elite calls it within the gap.

Next diagnostic: bracket owned Present entry/return, real DXGI Present, all
EDVR Present work and its trailing callback, plus PostPresentHandoff if called.
Join them to the same caller and compositor cycle to distinguish game work
before Present, Present blocking, EDVR work, and the remaining game/scheduling
interval before the next pose wait. Report absent/multiple/cross-thread calls
explicitly instead of assuming one Present per cycle. A large raw Present span
supports investigating swapchain/backpressure; a large EDVR span points to its
measured child operation; large outside-call spans require further
Elite/scheduling attribution. None establishes that all 4.2 ms is removable.

No pacing or draw-suppression fix is justified yet. This turn records the
flight and source findings only; the installed diagnostic remains 7cfb5a6.

### 2026-09-18 -- stock SteamVR comparison changes the optimization priority

The user reports the same settlement scene and headset running stock through
SteamVR/fpsVR: GPU about 6.8 ms, CPU averaging above 10 ms, and FPS in the
mid-80s. When asked about AA, resolution and the connection path, the user
confirmed everything was the same except the SteamVR route, with no
OpenComposite installed. Treat the settings as matched by user report; there is
no stock log or fpsVR history artifact attached to this observation.

This is strong evidence to investigate the patched path's cost before
implementing more Elite draw suppression. At approximately 85 versus 66 FPS,
the observed complete-frame periods are about 11.8 versus 15.1 ms, a roughly
3.3 ms difference. The stock CPU reading remains consistent with a demanding
scene, but scene cost alone no longer explains the patched result. Runtime,
EDVR features and active diagnostics changed together, so this comparison does
not isolate which component causes the loss.

The verified 7cfb5a6 flight still ran original-draw query/capture diagnostics
throughout the landed scene. At 11:29:55.997 its sampled draw-hook CPU mean is
4.297 ms/frame, and at 11:30:21.954 it is 4.169 ms/frame. Those counters
exclude forwarded game draw time and include EDVR reissues; they are not a
clean production-overhead measurement or an estimate of recoverable FPS. They
are also distinct from the separately bracketed post-stereo gap. Similar
numbers do not establish a common cause, and neither can be added to GPU
elapsed time as an independent cost.

The `DrawClock` source further limits attribution: it starts after
`gpuFrameCommand`, while its subtracted forwarded-draw interval also contains
the original-draw probe bracket/owner lookup and indexed-instanced weapon work.
The approximately 4.3 ms hook figure therefore is neither total EDVR overhead
nor a measurement of what disabling the probe would save.

`vScreenRefreshConfig` and hook initialization both call
`originalDrawProbeConfigure(gpuTimingEnabled, true)`, with `gpuTimingEnabled`
read from `advanced.app_gpu_timing`. Disabling that setting would remove the
coarse GPU comparison metric along with the selected-draw probe. The next
controlled build should separate that diagnostic admission, retain coarse
frame/GPU timing, and measure the post-stereo Present/caller phases described
above. A substantial improvement with the probe disabled implicates diagnostic
cost; persistence requires isolating the EDVR graphics/runtime path. No
specific overhead fix or draw suppression is justified by these observations.

The [fpsVR developer's timing
definitions](https://steamcommunity.com/app/908520/discussions/0/1735462352484917218/)
say its GPU figure covers scene rendering, post-submit application work and
compositor work, and its displayed frame times are maxima over a refresh
interval. These are different scopes and statistics from our native benchmark's
30-second percentiles. Therefore 6.8 ms versus about 10.6-11.2 ms is a serious
comparison signal, not a measured 4 ms EDVR GPU tax. In particular, it must not
be equated with the CPU-side post-stereo interval.

Native timer audit: `GpuSpanResult.outerMs` in ApplicationRender mode sums
three or four non-overlapping producer-context GPU timestamp pairs
(`gpu_span_state.h`). It includes game command intervals and producer-side EDVR
eye treatment, including any active non-temporal treatment with AA off. Closed
gaps between those pairs, Submit routing, and runtime-device transfer and
composition are excluded; the latter have separate `DeviceGpuTiming` samples.
Idle or scheduling delay inside an open timestamp pair can still extend it, so
this is elapsed GPU time, not pure GPU busy time. The post-second-Submit-return
gap is outside these pairs by construction. Keep the GPU discrepancy and that
wall-time gap as separate observations until a controlled run identifies how
they relate.

### 2026-09-18 -- controlled probe-off baseline with Present attribution

The user approved implementation and Frontier installation. A private
build-scoped gate disables the experimental original-draw query/capture
controller, its GPU resource allocation, probe metadata shader lookups and
metadata TLS writes, and returns before per-draw selection/capture. The
original-color scope remains intact because UI separation also uses it.
`advanced.app_gpu_timing` continues to control the coarse GPU measurement;
there is no new public setting or live INI edit. Startup explicitly logs
`Original draw diagnostic: controlled baseline OFF; coarse application GPU
timing on.` (or `off` if coarse timing is disabled).

Owned swapchain Present records CPU timestamps at hook entry, real DXGI Present
entry/return, the end of ordinary EDVR frame work, and the return of the
trailing render callback. Foreign swapchains are excluded. A private optional
reader uses the existing native-timing lease and QPC microseconds shared with
the native caller. It performs no graphics work, GPU queries, readbacks or
waits. Its 64 completed records and 16 active tokens are bounded; the response
carries at most 16 spans, generation, observed count and loss. Active
overlapping Presents, overwritten history and capacity loss cannot masquerade
as a zero-Present interval. Closed leases and stale completion tokens cannot
contaminate a later runtime generation.

At the next caller pose-wait entry, the runtime reads the exact preceding
post-stereo interval. Present records must belong to the same caller and
runtime generation, be ordered, wholly contained, successful and non-TEST.
Missing provider, no observed hook, incomplete/cross-thread/overlapping calls
and lost records have explicit coverage counts. The existing full-cycle
measurement remains usable even when the Present subdivision is unavailable.
Valid zero-, one- and multiple-Present cycles are counted separately; nonzero
Present sync intervals are also counted.

The `native_post_submit` enabled/first-complete markers and 30-second reports
share frame-cycle window IDs. Exclusive means cover raw Present, EDVR work
before/after it, the trailing render callback, and time outside these recorded
Present spans. Their sum is checked against the gap on exactly the same
accepted samples, with a paired residual. Single-Present before/after gaps use
their own explicit subset. PostPresentHandoff caller roundtrips are reported as
nested spans because they may overlap Present work; overlapping handoff
intervals are unioned, and invalid/missing/overflow calls are counted. Neither
aggregate totals nor nested spans should be added to the exclusive partition.
Time outside recorded spans can still contain Elite work, scheduling or EDVR
calls/instrument bookkeeping; it is not labeled pure game CPU time.

Discriminators for the next flight: a fall in whole-cycle/GPU timing with the
draw probe disabled supports diagnostic overhead; a large raw Present span
points to presentation/backpressure; a large EDVR or trailing callback span
points to that operation; a large outside span requires further caller-side
attribution. None presumes the original 4.2 ms is entirely removable. Pacing,
rendering features and original Elite draws are unchanged by this diagnostic.

Focused provider tests pass 930 checks and the actual client/provider GPU rig
passes 151. The collector adds deterministic cases for exact accounting,
filtered coverage, zero/multiple calls, malformed/partial/cross-thread/lost
records, scope changes and nested handoffs. The production reporter fixture
also exercises its first-complete marker and reports zero paired residual. The
full build passes all 62 pooled rigs, three quiet rigs and the 249-key
configuration contract, including 672 native-runtime checks and 620
original-draw checks. Validation log:
`build/controlled-baseline-build-validated.log`. The committed package is
rebuilt for an unambiguous flight version, then installed and verified with
`tools/install_edvr.py --target frontier`, preserving the live INI. The next
flight should hold the same landed AA-off cockpit view for two minutes without
an eye capture.

Build verification found a pre-existing startup-fixture scheduling limit: one
parallel run took 144.9 ms to admit the missing-loader test against its 100 ms
fixture timeout, producing two cascading assertions. Five isolated reruns of
the exact `openxr_module_test.exe --self-test-separate` gate each passed all
247 checks, with 0.2-0.5 ms admission. The full gate passes with four workers;
neither production nor fixture timeout behavior is changed. Set `EDVR_JOBS=4`
in the build environment: the existing `--jobs` argument path shifts `%0`
before passing `%~f0` to the rig runner, losing the batch path. That separate
script defect is bypassed here, not changed in this diagnostic.

### 2026-09-18 -- probe-off flight: small gain, remaining delay outside Present

The user reports FPS sometimes in the low 70s. Both Frontier logs verify
`v0.17.0-23-g72501fe` using `tools/edvr_log.py --expect-build 72501fe`:
`edvr_gfx_20260918_121326.log` and `edvr_openxr_20260918_121328_077_3424.log`.
The graphics log explicitly confirms `Original draw diagnostic: controlled
baseline OFF; coarse application GPU timing on.` No original-draw diagnostic
windows run. Environment remains VirtualDesktopXR / Quest 3 / 90 Hz, AA off,
input 2481x2121 per eye, output 3072x3264, runtime pacing, feature epoch 3.
There is no on-foot leg or eye capture in these logs. Caller and Present thread
are 4324; native log times are UTC, six hours ahead of the graphics log's local
timestamps.

The four steady landed cycle windows contain 8308 valid/admitted cycles. Each
has exactly one successful Present with sync interval zero and one
PostPresentHandoff; all provider, missing, invalid and overflow counts are
zero. Post-submit and full-cycle valid counts match. Paired residual is zero,
and the printed exclusive phase sums close within 0.0001 ms rounding.

| Cycle window | 7 | 8 | 9 | 10 |
| --- | ---: | ---: | ---: | ---: |
| Valid / admitted cycles | 2034 / 2034 | 2012 / 2012 | 2154 / 2154 | 2108 / 2108 |
| Caller FPS | 67.800 | 67.067 | 71.800 | 70.267 |
| Full cycle mean, ms | 14.7512 | 14.9055 | 13.9323 | 14.2315 |
| Before first Submit, ms | 10.1693 | 10.3286 | 9.7503 | 9.8740 |
| First Submit, ms | 0.1465 | 0.1458 | 0.1438 | 0.1457 |
| Second Submit, ms | 0.2426 | 0.2359 | 0.2317 | 0.2343 |
| Next pose wait, ms | 0.0496 | 0.0486 | 0.0481 | 0.0480 |
| Post-stereo gap, ms | 4.1426 | 4.1461 | 3.7578 | 3.9290 |
| Raw DXGI Present, ms | 0.0717 | 0.0723 | 0.0701 | 0.0701 |
| EDVR before real Present, ms | 0.0006 | 0.0005 | 0.0006 | 0.0005 |
| EDVR after real Present, ms | 0.0837 | 0.0828 | 0.0831 | 0.0832 |
| Trailing render callback, ms | 0.0422 | 0.0413 | 0.0365 | 0.0388 |
| Outside recorded Present span, ms | 3.9443 | 3.9491 | 3.5675 | 3.7363 |
| Before single Present, nested ms | 0.1651 | 0.1643 | 0.1539 | 0.1528 |
| After single Present, nested ms | 3.7792 | 3.7848 | 3.4136 | 3.5836 |
| Handoff roundtrip, nested ms | 0.0071 | 0.0068 | 0.0061 | 0.0064 |

Before/after-single-Present split the outside interval; handoff is another
nested measurement. Do not add these rows to the exclusive partition. The
Present end stamp precedes trace publication and the census destructor, so the
outside interval can still contain that final instrumentation bookkeeping. It
also admits Elite work, driver work/waits, other runtime calls and thread
descheduling. This is not evidence of a fixed sleep or wholly removable cost.

Exclude cycle windows 4-5 from landed comparisons: although their shape and
scene-ready flag are valid, they are menus, with approximately 0.5-0.7 ms
before first Submit and 8.5-8.7 ms in the next pose wait. Window 6 mixes menu
and gameplay. Window 11 is a short shutdown/scope-change tail; its reported 47
FPS is not a steady gameplay rate. Windows 7-10 are the comparison cohort.

Prior build 7cfb5a6 produced 66.133/66.200 FPS and mean cycles 15.1221/15.1072
ms in its two steady windows. The new result supports the user's modest
improvement, but does not assign every difference to probe removal:
scene/head-pose variation and the new Present measurement also differ. The
later gameplay GPU windows remain around 10.6-10.9 ms, so the stock SteamVR
comparison is still unresolved. The distinct graphics benchmark windows are:

| Graphics window / local end | Valid | CPU p50 / p95, ms | GPU p50 / p95, ms |
| --- | ---: | ---: | ---: |
| 5 / 12:16:09 | 1987 | 10.314 / 11.817 | 10.890 / 12.187 |
| 6 / 12:16:43 | 2119 | 9.795 / 11.203 | 10.725 / 11.907 |
| 7 / 12:17:17 | 2115 | 9.786 / 10.964 | 10.622 / 11.700 |

All three have no invalid/missing samples. Their windows differ from the cycle
cohort, and CPU, GPU and full-cycle intervals are not additive. Sampled
draw-hook CPU remains about 4.20-4.30 ms; its previously documented omissions
mean this cannot quantify total EDVR overhead. Game Map counters show about
0.23-0.26 ms/frame in writes and 0.002-0.003 ms/frame in reads. Existing
GetData summaries still see ready and pending queries, but have neither
per-call duration nor frame-phase attribution, so they do not establish a
query-wait cause. Native pacing reports zero changes, deferred frames,
synthesized frames and admission kicks.

- Ruled out: raw DXGI Present blocking dominates the remaining post-stereo gap,
  because it averages only 0.070-0.072 ms with sync interval zero.
- Ruled out: measured EDVR Present work or the trailing render callback
  dominates that gap, because together they average about 0.12 ms.
- Ruled out: PostPresentHandoff dominates that gap, because paired roundtrips
  average 0.006-0.007 ms with complete coverage.
- Ruled out: removing the original-draw probe alone closes the reported stock
  performance gap, because the verified OFF run remains at 67-72 FPS and about
  10.6-10.9 ms GPU. A smaller diagnostic contribution remains possible.

Next discriminator: attribute execution and waits on the caller thread,
correlated to the post-Present interval, before choosing another fix. CPU stack
samples can identify game, EDVR or driver execution; scheduler/wait events are
needed to distinguish running from blocking or descheduling. Prepare and
validate the collection and analysis workflow before another flight. WPR is
available on this machine, but no recording was started and no ETW analysis
pipeline has yet been validated.

The bounded source audit also identifies unmeasured OpenVR calls to cover.
`GetLastPoses`, `GetLastPoseForTrackedDeviceIndex` and `CanRenderScene` use
local snapshots; `GetFrameTiming`, `GetFrameTimeRemaining` and
`GetTimeSinceLastVsync` return unavailable. In contrast,
`GetDeviceToAbsoluteTrackingPose` can enter `locateHead`, whose non-owner path
synchronously invokes the runtime owner. A paired call count and span would
confirm or exclude that path in the residual. These are hypotheses, not
evidence that the game calls them there. Caller-thread cycle counts alone would
not identify a wait reason or provide frequency-independent CPU milliseconds.
The GPU elapsed-time discrepancy remains a separate target. Frontier stays on
verified 72501fe; this review changes no runtime code, rendering behavior or
live configuration.

### 2026-09-18 -- CPU execution/wait capture for the post-Present interval

The user approved preparing the CPU profile after the verified probe-off
flight. The remaining alternatives are: execution in Elite, EDVR or a driver; a
caller blocked in an unmeasured runtime/driver operation; or a runnable caller
waiting to be scheduled. Sampled stacks identify execution sites; context
switches plus ReadyThread events distinguish running, waiting and ready time.
Existing wall-time summaries cannot make that distinction.

The native runtime now registers a private ETW provider after successful Scene
initialization, outside DllMain. It emits only when an external trace session
enables it. Provider registration and error state have a bounded startup log
marker. Normal and exceptional shutdown unregister the provider; short write
locks and registration generations prevent teardown races without holding a
lock across an OpenVR call. Disabled ABI spans add no clock reads or event
writes, and there is no per-frame file logging.

Provider `{D3885FA1-0B70-44F1-AF88-63B2012B111E}` uses version 1 and keyword 1.
Clock, completed-call and completed-frame payloads are fixed at 32, 40 and 96
bytes, with compile-time layout checks. Clock markers carry absolute QPC
ticks/frequency and the same QPC microseconds used by Present accounting. They
appear on capture enable and periodically, so a session started after game
initialization can still correlate its frames.

Completed-frame boundaries come from the existing frame-cycle admission path.
They include sequence, generation, feature epoch, caller and next-wait thread,
the full cycle, second Submit return, and Present bounds only when the existing
single-Present validation passes. Missing-provider and malformed Present cases
retain explicit status; base-cycle failure, scope change, stale completion and
capacity rejection do not emit an admitted completion. Call intervals cover
GetLastPoses, GetLastPoseForTrackedDeviceIndex, GetFrameTiming,
GetFrameTimeRemaining, CanRenderScene, GetTimeSinceLastVsync and
GetDeviceToAbsoluteTrackingPose, as well as WaitGetPoses, Submit and
PostPresentHandoff. Nested intervals are unioned by the analyzer rather than
added twice. These markers add no GPU work and change no rendering or pacing
policy.

`tools/cpu_profile/edvr_cpu.wprp` selects CPU sampling, context switches,
ReadyThread, process/thread lifetime and module-load events, with stacks for
the three CPU event classes. Memory mode bounds the kernel ring to 512 MiB and
the marker ring to 4 MiB. Actual retained history depends on event rate; the
decoder uses the overlapping scheduler/marker interval and reports excluded
prefixes, tails and partial coverage. This follows Microsoft's [memory logging
model](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/logging-mode).

`tools/cpu_profile.py` uses a randomized private WPR instance on every start,
stop and cancellation. It waits for the exact installed Frontier executable,
holds its process handle to avoid PID reuse, and verifies that PID's fresh
native build log before recording. It saves when the game exits or after a hard
maximum of 300 seconds. Waiting for launch is separately bounded. A failed
start/stop never cancels another recording. State and artifacts remain under
the requested new output directory; dry-run creates neither files nor sessions.
No live INI is changed.

The optional .NET analyzer reuses TraceEvent already installed with Visual
Studio; no dependency was downloaded. It decodes fixed marker schemas,
reconstructs exclusive running/ready/waiting/unknown durations on the caller,
and reports sampled and switch-out stacks with full module paths, relative
addresses and raw virtual addresses. Ready-waker stacks are labeled separately
because they belong to the waking thread. Stack counts are not converted into
CPU milliseconds. Lost events, invalid clocks, malformed markers, incomplete
scheduler coverage and sequence gaps are explicit. Indexed per-thread lookups
keep multi-minute traces practical. The ETL is retained for later analysis and
local symbol lookup.

Profiling builds set `EDVR_PROFILE_SYMBOLS=1`, keeping `/O2` and producing
local PDBs with `/Z7 /DEBUG:FULL /OPT:REF /OPT:ICF`. The explicit optimization
flags preserve the release linker defaults that `/DEBUG` otherwise changes, as
documented by
[Microsoft](https://learn.microsoft.com/en-us/cpp/build/reference/debug-generate-debug-info).
Ordinary builds gate the dependency-free Python tests; profiling builds also
compile the analyzer and run its timeline/schema tests. Capture refuses a stale
analyzer using hashes of its sources, schema and binary.

The live pipeline test is `openxr_trace_test.exe --cpu-trace-smoke`: thirty 100
ms busy intervals followed by thirty 100 ms Sleep intervals, with nested call
markers and an explicitly synthetic zero-duration Present. The wrapper requires
all 60 frames, complete decoder coverage, sampled busy stacks, and correct
separation of predominantly running versus waiting phases before arming a
flight. Missing provider enablement or event-write failure fails the fixture.
This validates the actual C++ provider and ETL decoder together.

Local WPR profile validation succeeds. The first kernel recording attempt
failed before starting with `0xc5585011`: the ordinary token has medium
integrity and lacks SeSystemProfilePrivilege. WPR remained idle afterward. This
is a Windows privilege boundary, not an automatic tool-approval rejection. An
administrator process started through Windows UAC is required for the synthetic
and flight captures. Collection will not be called verified until the live
smoke succeeds.

Validation before commit: both the ordinary and symbol-enabled absolute
`build.bat` runs pass all 62 pooled rigs, three quiet rigs and 249
configuration checks. The native fixture passes 4777 checks, including
completion extraction and capacity rejection; the trace fixture passes 15
checks. Python capture tests pass 49 checks, including dry-run filesystem
snapshots, stale PID logs, process-exit races and private-session cleanup. The
profiling build also compiles the analyzer with zero warnings/errors and passes
its scheduler, coverage, schema and nested-span tests. Logs:
`build/cpu-profile-validation.log` and
`build/cpu-profile-symbol-validation.log`. The committed package is rebuilt
before Frontier installation; the elevated live pipeline test is recorded
below.

Post-commit validation: the symbol-enabled build of 2d98775 passes the same
complete build gates (`build/cpu-profile-build-2d98775.log`). Frontier now has
`v0.17.0-25-g2d98775`, installed and hash-verified through `install_edvr.py`
without changing its INI. The collector also accepts the installer's verified
receipt siblings after a reinstall; a regression covers a stale primary receipt
and a valid replacement sibling. Python tests now pass 50 checks.

The first UAC-elevated pipeline check wrote a 49,283,072-byte ETL, all 60
synthetic frame markers and sampled stacks, with zero lost events or provider
write failures. It refused to arm the game capture because nine frames had
unknown scheduling intervals. Raw context switches account for all nine: eight
Running (2) transitions during cross-CPU handoff, totaling 28.2 us, and one
DeferredReady (7) transition lasting 3.7 us. TraceEvent names raw 7 `Unknown`,
unlike the Windows ETW meaning in Microsoft's [CSwitch state
table](https://learn.microsoft.com/en-us/windows/win32/etw/cswitch). The C++
fixture and provider need no change. The correction belongs in the offline
decoder: retain Running through state 2, map state 7 to ready, and make
unsupported states inside retained frame intervals fail coverage explicitly.
The stopped capture left no WPR recording active.

Saved-ETL replay with the corrected decoder passes all 60 frames, zero lost
events, no cohort gaps or duplicates, and no boundary or unexpected unknown
time. Busy intervals total 2994.78 ms running; Sleep intervals total 3237.55 ms
waiting. The strict Python smoke validator passes. The analyzer rebuild has
zero warnings/errors and passes its numeric-state, coverage and equal-QPC
ordering regressions. The fix preserves event order for equal QPC timestamps
and distinguishes an unobserved ring-boundary prefix from an explicit unknown
state, which now fails coverage. These changes affect offline tools only;
Frontier remains pinned to runtime build 2d98775 for the next flight.

A second UAC-elevated live capture also passes all 60 frames with zero lost
events, no unknown time, and correct phase separation: 2996.20 ms busy running
and 3228.57 ms Sleep waiting. Collector process 2940 entered waiting_for_game
at 19:11:42 UTC on September 18, with output under
`build/cpu-profile-frontier-2d98775-r2`. It allows 1800 seconds for Frontier to
launch, verifies the fresh process log against 2d98775, and records until game
exit or 300 seconds. The requested flight keeps AA off and the same landed
cockpit view for 60-90 seconds, then exits normally. No eye dump is required.
This run identifies execution and scheduling costs; profiling overhead means it
is not a clean FPS comparison against the prior probe-off flight.

### 2026-09-18 -- first CPU flight: landed caller execution and decoder audit

The user ran the capture. Both native and graphics logs verify build 2d98775;
the collector watched process 9392 and stopped on normal game exit. The saved
`build/cpu-profile-frontier-2d98775-r2/flight.etl` is 543,162,368 bytes, with
3372 valid frame markers, 28148 call spans, valid clock mappings, and zero ETW
lost events. The kernel ring retains about 22.5 seconds. Its initial report
analyzes 1339 intervals but correctly declines global complete coverage because
one sequence is absent. Native W9 reports `missing_scope=1` at sequence 13422
during exit; this gap is outside the landed cohort and does not establish ETW
event loss.

Native W8 ends at 19:16:07.602 UTC, covers sequences 11056-12937, and admits
all 1882 attempts with every missing counter zero. The retained ring covers its
last roughly 11.7 seconds; sequence 12938 onward belongs to the scene/exit
transition. W8 means are 15.939 ms cycle, 10.767 ms before first Submit, 0.220
and 0.396 ms Submit calls, 4.477 ms from second Submit to next wait, and 0.077
ms next pose wait. The paired post accounting has 0.058 ms raw Present, 4.036
ms after the single Present, and 0.019 ms handoff. All frames have one Present
and handoff. Compared with 72501fe W7/W8, cycles increase about 0.885/1.034 ms
and FPS falls 5.7/6.5 percent. Kernel tracing, event emission, and scene
variance are confounded; this does not measure a rendering change.

An independent raw-ETL decoder reproduces the conservative steady cohort
12225-12935: 711 frames on caller thread 17948, QPC microseconds
5450546059-5461524507. Per-frame means after Present are 3.698079 ms total,
2.616066 ms running, 0.938322 ms waiting, and 0.143691 ms ready but not
scheduled, with zero unknown. The 2844 GetDeviceToAbsoluteTrackingPose spans
average 0.0946 ms/frame; all measured OpenVR spans union to 0.0950 ms/frame.
The 1837 execution samples include 1482 user-leaf observations in Elite, 248 in
ntdll, 40 in EDVR's graphics DLL, 18 in Windows D3D11, 31 in win32u, 10 in
NVIDIA, and three in EDVR's OpenVR runtime. These are sample occurrence counts,
not measured CPU milliseconds or guaranteed removable time.

Local matching runtime symbols resolve the common EDVR ready-waker stack to
`OwnerService::run -> finish -> _Mtx_unlock`. This is the helper completing
synchronous pose queries and notifying the caller.
`NativeRuntimeHost::locateHead` routes non-owner callers through this service.
A waker stack is not evidence of execution on the waking caller; the direct
call-span measurement bounds the cost of these four queries in the selected
interval.

Ruled out: synchronous pose queries as the main post-Present cost, because
their complete measured spans total only 0.095 ms versus 3.698 ms/frame. Ruled
out: ready-to-run scheduling delay as the dominant post-Present cost in this
cohort, because it is 0.144 ms versus 2.616 ms executing. The remaining
execution and waiting are not proven avoidable.

The raw audit also finds two offline analyzer defects before relying on its
stack attribution. First, circular buffers have different retained starts per
CPU: 5449518588.3 us on CPU4 through 5449800919.1 us on CPU0, a 282.331 ms
spread despite zero lost events. Sequences 12159-12176 are incomplete; frame
12160 incorrectly appears entirely ready while its pose calls plainly execute.
The complete scheduler boundary must use the latest first retained CSwitch
among observed CPUs. The conservative cohort starts well after that boundary.

Second, TraceEvent's generic CSwitch call stack belongs to the incoming thread.
The outgoing thread requires `BlockingStack()`, as specified in the [TraceEvent
source](https://github.com/microsoft/perfview/blob/main/src/TraceEvent/TraceLog.cs#L1103).
Of 51030 caller switch-outs, the generic stack mismatches the old thread on
51025, while the blocking stack matches on 51023 (seven missing/edge stacks).
This explains foreign CLR, encoder and driver stacks in the first report.
Sampled and ReadyThread stack ownership are correct; ReadyThread identifies the
waker.

The corrected analyzer uses the common retained CPU boundary, verifies the
outgoing stack's PID and TID before attribution, and reports the top stacks
separately for execution, switch-out and ready-waker events. Release
compilation has zero warnings/errors and the self-tests pass, including
staggered CPU boundaries, stack ownership and per-kind grouping. Both saved
synthetic traces still pass all 60 frames with complete coverage. Flight replay
now admits 1321 intervals beginning at sequence 12177, with zero unknown time
and zero ownership mismatches across 658998 blocking stacks. The conservative
cohort's statistics are unchanged. Global coverage remains false because the
exit-time scope gap remains visible. The corrected report is
`report-fixed-final.json`; the capture's original status/report are retained as
evidence. The sanctioned analyzer wrapper rebuilt and refreshed its validation
stamp. No game code or live configuration changed.

Full-frame caller sampling over the same conservative QPC interval finds 9697
sampled observations: 4190 with their sampled PC in Elite, 2489 in EDVR's
graphics proxy, 1505 in ntdll, 913 in system D3D11, 377 in NVIDIA, 136 in
win32u, eight in EDVR's native runtime, 40 other and 39 unknown. Inclusive
stack presence is different and overlaps: 4078 contain the graphics proxy, 1938
system D3D11, 1039 NVIDIA and 278 the native runtime. In particular, 1589
proxy-inclusive samples actually execute farther down the call chain; do not
charge those samples to proxy computation.

Matching local PDBs symbolize 2487 of the 2489 true proxy sampled PCs into 157
functions. The largest groups are beginPanelOverride (275), forwardWithVerdict
(226), its draw lambda (105), and hookedDrawIndexedInstanced (101): 707
observations across four pieces of draw classification/forwarding. Other groups
include meshMotionDraw (93), foveationOnDraw (83), celestial_motion::begin
(78), ui_deferred::beforeTone (71), gpuFrameCommand (64), bindingShaderHash
(56), screenMotionUiDraw (50), mapWaitNote (49), screenMotionDraw (40), and
glitch_scene::readPool (24). These features can have active correctness
responsibilities even with AA off; their presence alone does not justify
disabling them. The security-cookie check also recurs across these short call
chains; its presence is not a reason to remove stack protection. Exposure hash
lookup (249), hookedMap return/timing regions (184), and the
DrawIndexedInstanced lambda (159) are nearest-proxy-frame counts in inclusive
stacks, not direct sampled-PC counts. No single feature is established as the
dominant cost.

This supports a focused audit of repeated per-draw lookups, inactive observer
calls and timing bookkeeping in our proxy before selecting a safe fast path. It
does not establish a particular removable number of milliseconds. The matching
2d98775 runtime/graphics DLLs and PDBs are preserved with SHA-256 metadata
under the capture's `symbols-2d98775` directory so later builds cannot
invalidate this analysis.

The source audit identifies a narrow first optimization with an existing
discriminating condition. `forwardWithVerdict` eagerly computes two
bindingShaderHash arguments for uiDeferredTraceDrawEnter on every owner draw.
In this AA-off flight, uiDeferredConfigure leaves capture disabled and its
failure diagnostic window was never armed, so the callee returns after clearing
pending state. Move the hash reads inside the callee after its existing active
and candidate checks, while preserving the entry call and pending reset. An
active or diagnostic draw must retain identical binding hashes and trace data.

AA-dependent mesh, celestial, screen and weapon motion helpers are also
configured inactive by this flight's temporal mode but still entered per draw,
sometimes after eager shader-hash reads. A shared configuration-derived gate
can avoid that fanout only if live reload updates it in the same transaction,
enabled call order remains identical, and disabled cleanup/counters are handled
explicitly. The existing foveationWantsDraws predicate is another candidate: it
includes active phases and stale bindings requiring cleanup, so it can gate
entry once fully off. These are proposed runtime changes, not part of this
analysis-only commit.

Do not bypass beginPanelOverride wholesale: transition-flash camera/pool
processing is active and relies on its eye-draw counting. Likewise, Map timing
has measurable overhead but supplies exact slow-call/longest-wait evidence;
changing that instrumentation belongs in a separate controlled comparison.

Scope clarification: the disabled-motion fast path specifically reduces the
AA-off baseline; DLSS needs those motion helpers and must retain their work.
Lazy shader hashes after UI-target rejection can benefit both modes, as can
skipping an inactive foveation path after required cleanup. Shared draw-path
improvements are relevant to DLSS, but this AA-off capture cannot quantify
their DLSS-on benefit or establish stock-game parity. A DLSS-on profile is
still needed to target active motion work toward the original settlement goal.
Final module-path validation also separates the post-Present D3D11 sample
group: 40 belong to EDVR and 18 to Windows, correcting an initial basename-only
grouping of 58. Full-frame module totals and scheduler timings are unchanged.

### 2026-09-18 -- shared draw-hook fast paths for the DLSS follow-up

The user approved the shared-path pass and asked about optimizing Elite's own
CPU cost in settlements. The measured hypothesis for this build is narrower:
draws rejected by the UI writer's existing candidate checks do not need their
shader hashes, and fully inactive foveation does not need its draw callback.
The prior caller sampled-PC profile identifies bindingShaderHash and
foveationOnDraw among the repeated hook costs; source control flow confirms
which results are discarded. These checks are shared by AA-off and DLSS modes.

Move UI entry hashes behind the existing active and target checks while keeping
the unconditional pending-state reset and existing ordinal semantics. Gate
foveationOnDraw with foveationWantsDraws, which also retains stale
bound/unknown state until cleanup. Active DLSS motion work, transition-flash
logic, original draw submission and Map timing remain outside this pass.

Further stack attribution finds an EDVR-caused driver cost in the same saved
cohort: system D3D11 PCs at +0x47ab9 (376 samples) and +0x201dd (78) sit under
resolveBindOnEyeDraw's PSGetShader path. This is the scanner-body fix
(`fix.scanner_body`), whose healthy-case classifier runs on every eye draw. It
queries, hashes and releases the current PS before any healing decision. The
existing owner-context binding shadow may supply that identity without entering
the driver, provided shader substitutions and state invalidation are respected
and unknown identity keeps the guarded getter fallback. This is a shared-path
candidate with direct sampled evidence; it does not suppress any Elite draw or
change the scanner-body repair.

Optimizing Elite itself remains possible only at the layer where work occurs.
The proxy can reduce its own overhead and driver calls it introduces. Avoiding
genuinely redundant game submissions could reduce later driver cost if their
side effects are proven absent. It cannot recover CPU work already spent on
scene traversal, visibility decisions, draw preparation or task scheduling.
Earlier engine interception would require a verified version-specific call site
plus reliable object and visibility semantics; the current censuses and sampled
addresses do not supply those guarantees. Large zero-sample draw counts are not
evidence that their upstream CPU work can safely be omitted.

Before installation, Frontier's nvngx_dlss.dll reports Windows file/product
version `310,7,0,0` (58,977,904 bytes). The live INI has temporal_aa off,
scanner_body on, and foveation off. Installation preserves that file; the next
capture will select DLSS from the F8 menu in the same landed cockpit view.

Implemented all three shared paths. Scanner-body recognition now uses a known
owner-context PS pointer/hash directly; unknown identity retains the guarded
real getter, and a successful lookup repairs that one shadow slot. The UI
fixture covers disabled, non-eye and eye-sized depth-target rejection without
hash reads, pending reset, ordinal preservation, and retained candidate hashes.
A new WARP resolve fixture counts real PSGetShader calls and verifies known
match/nonmatch, unknown fallback and repair, invalidation, null state, and
exact vertex-buffer lend/restoration. Independent source and fixture review
found no blocking issue.

The absolute symbol-enabled build passed all 63 pooled rigs, three quiet rigs,
and the 249-key config contract. Both new/extended fixtures ran successfully;
the complete output is build/shared-draw-fastpaths-validation.log. The clean
committed rebuild c1dbb76 also passed every gate, with its output in
build/shared-draw-fastpaths-c1dbb76.log. The sanctioned installer dry-run,
install and verify-only all passed for Frontier; installed native version is
v0.17.0-29-gc1dbb76. Live settings and the DLSS DLL were preserved.

The elevated collector PID 6132 passed its real ETW smoke validation and
reached waiting_for_game at 19:59:50 UTC. It expects exactly c1dbb76, waits 30
minutes for launch, then records until normal game exit or five minutes.
Output: build/cpu-profile-frontier-c1dbb76-dlss. Exact DLLs and PDBs are
archived there under symbols-c1dbb76 with verified SHA-256 hashes. Select DLSS
through F8 and hold the same landed cockpit view for 60-90 seconds before
quitting. A fresh DLSS-on capture is still required before claiming an in-game
gain.

### 2026-09-18 -- c1dbb76 DLSS flight and retained CPU ownership

Both graphics/native logs verify v0.17.0-29-gc1dbb76. The collector completed
normally on game_exit; output is build/cpu-profile-frontier-c1dbb76-dlss. The
ETL is 540,016,640 bytes. DLSS was selected live at 14:01:21.820 local; the
settled scene uses input 2481x2121, temporal reconstruction 3818x3264, XR
output 3072x3264, Quest 3/VirtualDesktopXR, 90 Hz and preset K. The initial
cheap scene/loading windows must not be used as the settlement measurement.

Native W7 and W8 are the expensive settled scene: 55.733/54.933 FPS and
17.9422/18.1996 ms cycle means. W8 spans seq10577-12224 and reports 12.6892 ms
before first Submit, 4.0765 ms after Present, and 0.0670 ms raw Present. The
last completed benchmark window has CPU median/p95 12.823/14.338 ms and GPU
16.506/18.154 ms. The CPU field is producer elapsed wall time, including
stalls/descheduling and treatment callbacks, not exclusive execution. These
windows and the ETL cohort below have different boundaries.

The report has complete coverage, zero lost events, 3473 valid available frames
and 1246 analyzed intervals (seq11580-12825). Retained gaps, duplicates,
invalid spans/frames and unknown scheduler state are all zero. Earlier marker
gaps precede the retained scheduler tail. The common retained CPU boundary is
8329270708.1 us, 672.637 ms after the global first switch; one partial boundary
frame is correctly discarded.

Use seq11580-12078, QPC8329288099-8338036037 us, caller 15248, as the strict
499-frame cohort. It ends before the 20:04:04.089 UTC shadow census change
(QPC8338050220.1). The mean post-Present 3.625998 ms comprises 2.683319 ms
running, 0.829397 ms waiting and 0.113282 ms ready, with zero unknown. The
native call-span union is 0.083120 ms/frame. The broader W8 intersection
seq11580-12224 (645 frames) gives 3.624879 ms total and nearly identical
scheduler components. Exclude the later scene/exit transition; seq12567 alone
contains a 542.842 ms interval, of which 537.385 ms is waiting.

The previous AA-off cohort's post-Present mean was 3.698 ms, including 2.616 ms
running and 0.095 ms native call spans. Those similar magnitudes reinforce that
the synchronous native calls do not explain the multi-millisecond gap; they do
not establish an optimization delta across different AA modes and scenes. Ruled
out: using this flight versus 2d98775 to claim a net performance gain or
regression, because the baseline had AA off and workloads are unequal.

Depth census samples report roughly 22000-22700 draws/frame, about 18500 in the
two eye targets; at 14:04:04.089 a 256x256 target contributes 4428 draws and
the total reaches 27303. These are submissions, not unique objects. The
instrumented draw-hook mean in two heavy windows is 5.022/5.123 ms per sampled
frame. It excludes original draw forwarding but includes EDVR reissues, waits
and timer overhead, and misses the previously documented work outside its
bracket. It must not be presented as an exclusive/removable CPU total. The
steady temporal price reports roughly 2.51 ms median for NGX reconstruction per
stereo pair, 0.30-0.31 ms prep and 0.32-0.33 ms UI; independent component
medians cannot be added into a measured total, and some pairs were unmeasured.

The strict cohort contains 7946 caller sampled-PC observations. Full module
paths separate Elite (3431), the game-directory graphics proxy (2448, 30.8%),
ntdll (973), Windows system D3D11 (554), NVIDIA (350), win32u (101), other
(42), unknown (39), and the native runtime (8). These are observations, not CPU
milliseconds or removable fractions; the runtime helper threads are outside
this caller-only tally. Inclusive stack presence overlaps and is a different
measurement: proxy 3520, system D3D11 1390, NVIDIA 891, runtime 295. After
Present, the actual PC is in Elite for 1087 observations versus 23 in the
graphics proxy and three in the runtime, reinforcing the engine ownership of
most execution in that interval.

The targeted inactive foveation callback has zero direct sampled PCs (83 in the
previous AA-off cohort). bindingShaderHash has 34 and resolveBindOnEyeDraw
eight. The old system D3D11 +0x47ab9 hotspot has changed caller: 119 of its 124
observations are now under guardedBudget -> bindingResolve -> scrimOnEyeDraw,
not the old dominant scanner-body PSGetShader path. Raw driver offsets alone
would conceal this improvement. The other old driver offset +0x201dd has 63 of
67 observations under the accepted meshMotionDraw path near its function end;
this is system/COM work, not proof of a particular descriptor getter. Different
capture lengths and workloads preclude raw-count speedup arithmetic.

The remaining concrete proxy work includes refreshScenePick (227 direct PCs),
uiDepthOnEyeDraw (163), meshMotionDraw (98) and meshMotionResourceWritten (25).
The central beginPanelOverride/forwardWithVerdict/draw lambda account for
245/186/149 direct observations. Source inspection confirms refreshScenePick
linearly scans the depth-target table for each query, largely using last-frame
counts. Its reusable result must be invalidated by every relevant table/pick
mutation and size change, not just by advancing the frame. scrimOnEyeDraw
resolves PS SRV0 resource type/size/format for every shape candidate and SRV1
only after the rare 16x16 BC1 match. Immutable descriptor reuse is a bounded
next target; preserve resource lifetime, binding invalidation and unknown
resolution semantics. Neither optimization would suppress game draws or alter
object motion eligibility. No engine operation has yet been identified that is
both expensive and proven safe to omit.

Derived samples and symbolized PCs are retained under
build/cpu-profile-frontier-c1dbb76-dlss/derived-hotspots/strict-pre-shadow/.
This flight verifies removal of the targeted hot path and supplies the next CPU
targets; it does not measure the net FPS benefit of c1dbb76. No runtime or live
configuration was changed during this analysis.

### 2026-09-18 -- depth selection and scrim metadata reuse

The user authorized the next two CPU optimizations. The measured hypotheses are
that unchanged scene-depth selection inputs do not need another linear
target-table scan (227 actual proxy PCs in the strict DLSS cohort), and that an
unchanged PS SRV binding does not need its immutable texture metadata resolved
again (119 system-D3D11 PCs under bindingResolve -> scrimOnEyeDraw). Neither
hypothesis permits skipping Elite draws or changing motion eligibility.

The depth path must preserve pick hysteresis, failed-query stale-pick behavior,
alternating sizes and eye ordering, and invalidate on every target-table or
last-frame input change. The scrim path must retain its shape/format/size
recognition, temporary texture substitution and restoration, retry unknown
metadata, and reject stale bindings. Existing binding generations already
advance on setters, frame boundaries and binding invalidation, allowing a
frame-bounded cache without another draw callback. Local production-path
fixtures will check repeated-hit avoidance and all relevant invalidation and
recovery behavior before installation. No predicted millisecond or FPS saving
is assigned to sampled-PC counts.

Implementation uses a single last-size depth-query result. A different size
must rescan because the algorithm updates one global hysteresis pair; a table
of independent cached sizes could replay the wrong pair. Configure, discovery
(including reused slots), frame rollover/eviction and shutdown invalidate the
cache. Eye order is still calculated from the current last-frame ordering. The
original scan and hysteresis logic are unchanged on misses.

Scrim stores only a known classification boolean for each of PS SRV0 and SRV1,
keyed by view pointer and binding generation. It retains no borrowed resource
identity and adds no reference-count operation on hits. Failed resolution is
never cached as a known rejection. Enable-state changes and shutdown reset both
entries; normal binding/frame generations handle the remaining changes. All
query/scan counters used by the fixtures compile out of production.

The focused WARP fixtures compile and pass independently. The depth fixture
passes 84 checks, including positive/negative reuse, size switching,
hysteresis, eye order, new targets, frame rollover, eviction/reuse and
reconfiguration. The scrim fixture exercises the production resolver on real
SRVs, including a same-generation failed GetResource followed by successful
retry. Its context shim forwards real PSSetShaderResources while updating the
common shadow, so scrimBegin/End prove real substitution/restoration and
exactly two generation changes. Both fixtures are now full-build gates.
Independent source and fixture review found no blocking issue. The
symbol-enabled absolute build passed all 65 pooled rigs, three quiet rigs and
the 249-key configuration contract; output is
build/scene-metadata-caches-validation.log. The committed version still needs
its exact-version rebuild and verified Frontier installation.

Committed and pushed as 5efb139. The clean symbol-enabled rebuild also passes
65 pooled rigs, three quiet rigs and all 249 configuration checks; log:
build/scene-metadata-caches-5efb139.log. The sanctioned installer dry-run,
install and verify-only all pass for Frontier, which now has
v0.17.0-32-g5efb139. Live temporal_aa=dlss, scanner_body=on, foveation=off and
DLSS DLL version 310,7,0,0 were preserved.

The elevated collector PID 7804 reached waiting_for_game at 20:28:17 UTC after
its real ETW smoke passed all 60 frames, complete coverage, zero event loss and
zero unknown/invalid intervals. It expects exactly 5efb139, waits 1800 seconds
for launch and records until game exit or 300 seconds. Output is
build/cpu-profile-frontier-5efb139-dlss; matching DLLs/PDBs are archived under
symbols-5efb139 with verified SHA-256 hashes. Hold the same landed cockpit view
with DLSS active for 60-90 seconds, then quit normally. Compare with the
preserved c1dbb76 DLSS capture rather than the older AA-off flight. No in-game
speedup is claimed before that measurement.

### 2026-09-18 -- verified 5efb139 DLSS cache flight

The user ran the installed build. Both graphics and native logs identify
v0.17.0-32-g5efb139, checked with tools/edvr_log.py --expect-build 5efb139.
Graphics log is edvr_gfx_20260918_142925.log; native log is
edvr_openxr_20260918_142927_012_16980.log. The collector completed normally on
game exit at 20:32:43 UTC. Capture, report and matching archived DLLs/PDBs are
under build/cpu-profile-frontier-5efb139-dlss; derived sampled PCs and symbols
are under derived-hotspots/strict-pre-shadow. Environment and DLSS settings
match the Status block. No runtime or live configuration was changed during
this analysis.

Coverage is complete: zero ETW event loss, retained sequence gaps, malformed
records, invalid intervals, scheduler unknowns or stack-owner mismatches. Of
3395 available frames, the scheduler-covered tail retains 1190 continuous
frames, sequences 11416-12605. Provider gaps precede that retained tail. The
strict comparison uses sequences 11416-11969, 554 frames on caller TID 8652,
before the shadow-workload change at 20:32:12.570 UTC. Later luminance changes
and the exit stall are excluded. An isolated 22.975 ms post-Present interval
inside this cohort is retained; it contributes only about 0.034 ms/frame to the
mean, and the increased running time is also present in the median.

The new depth-selection cache reduces direct refreshScenePick observations from
227/499 frames to 6/554, a 97.6% lower sampled-PC rate. Scrim-origin
system-D3D11 observations fall from 120/499 to 2/554, a 98.5% lower rate. The
old 119-PC system-D3D11 +0x47ab9 cluster under bindingResolve -> scrimOnEyeDraw
has no new observations. These are targeted sampling-rate changes, not
percentages of total CPU time saved. Total graphics-proxy exclusive-PC rate
falls from 2448/499 to 2495/554, approximately 8.2%; inclusive stack counts
overlap and are not removable fractions.

The two 1800-frame draw-hook means fall from 5.022/5.123 ms to 4.511/4.687 ms,
about 0.44-0.51 ms or 9-10% lower. That timer includes proxy reissues, waits
and instrumentation but excludes original draw forwarding, the earlier
probe/owner lookup and some weapon work; it is neither total EDVR overhead nor
wholly removable CPU execution. Motion-admission scene/depth/eye/cap samples
fall from 4711 us / 74316 observations to 2381.2 us / 75493, about half the
time per sample. Diagnostic cohorts differ from the ETL cohort, so this is
supporting evidence rather than another additive millisecond saving.

The clean native W7 window improves from 55.733 to 56.700 FPS, with cycle time
17.9422 -> 17.6437 ms and before-Submit time 12.5300 -> 12.2110 ms. Its
post-Present mean is 3.9640 -> 4.0363 ms. New W8 includes the exit transition
and must not be treated as a steady FPS comparison. Last completed benchmark
window medians are essentially unchanged: CPU 12.823 -> 12.848 ms, GPU 16.506
-> 16.459 ms. CPU p95 is 14.338 -> 14.328 ms; GPU p95 is 18.154 -> 18.134 ms.
These elapsed CPU/GPU measures are not additive.

In the stricter sampled trace cohort, post-Present time rises from 3.626 to
4.259 ms: running 2.683 -> 3.281, ready 0.113 -> 0.142, waiting 0.829 -> 0.836
ms. Native call-span union is only 0.083 -> 0.093 ms/frame. New medians are
3.976 ms total, 3.009 running, 0.110 ready and 0.839 waiting. Exclusive
post-Present PCs are predominantly Elite (1349), ntdll (234), win32u (108) and
NVIDIA (80); graphics proxy has 28, system D3D11 has 15, native runtime has six
and other modules have four. Proxy observations per frame are nearly flat
versus the old cohort. The extra execution is mostly outside the graphics
proxy, but ownership alone does not establish its cause or prove a regression
caused by either cache. Separate flights also have some variation in eye draw
counts. The data supports a modest proxy-cost reduction, not a controlled claim
of a large overall FPS gain.

The next measured driver cluster needs precise attribution. System-D3D11
+0x47ab9 has 133 new exclusive PCs, 128 under meshMotionDraw. A line-level PDB
mapping initially suggested BlendState::GetDesc, but that source line spans
several calls. Disassembly of the matching archived graphics DLL places the
observed proxy return RVA 0xb83b5 immediately after call rbx at 0xb83b3; rbx is
loaded from context vtable offset 0x2d8, slot 91, OMGetBlendState. The
BlendState::GetDesc return is later at 0xb83e7, and the observer-cache AddRef
return is at 0xb843d. Ruled out: assigning the 128-PC cluster to
BlendState::GetDesc, because its exact sampled caller return site identifies
OMGetBlendState. Descriptor-only caching does not directly remove this measured
query cost.

Current binding_shadow slots do not include blend or depth state, and vscreen
does not hook OMSetBlendState. The next investigation should audit tracking
blend-state setters on the owned context and using known bindings for motion
eligibility, retaining the real getter for unknown state. ClearState,
ExecuteCommandList restoration, temporary proxy substitutions and COM lifetime
must be handled before a build. Existing descriptor observers measure reuse but
do not bypass getters. No Elite scene-traversal or other engine operation has
yet been proven safe to skip. Native shutdown reports zero temporal, sharpen,
menu or pose failures and zero graphics wrong-thread calls; visual quality
still requires user observation. Keep 5efb139 installed; no additional flight
is requested for this analysis.

### 2026-09-18 -- blend binding tracking audit

The user authorized proceeding. The confirmed target is the 128 sampled driver
PCs whose exact caller return site identifies meshMotionDraw's OMGetBlendState
in the 5efb139 DLSS flight. The hypothesis is that an observed current binding
can supply the same state pointer, four blend factors and sample mask without
another driver query. Unknown state must retain the real getter, and a caller
must hold a reference across temporary coverage-pass substitutions.

Before implementation, audit every mutation path: OMSetBlendState, ClearState,
ExecuteCommandList with and without restoration, SwapDeviceContextState,
alternate context interfaces, hook installation/loss and EDVR's temporary state
changes. Setter tracking itself adds CPU cost, so query hits/fallbacks and
setter activity must be distinguishable in the next flight. Existing motion
eligibility, coverage and original game draws must remain unchanged.

The verified flight uses native LiveCopy with 302 context methods. Restrict
tracking to that private-hook mode; InPlace and unsupported context aliases
keep the real getter. Require all supported context interfaces to alias the
owned pointer and all mutation hooks to install successfully. The setter is
slot 35 and Context1 SwapDeviceContextState is slot 131. Consumers check the
live mutation-hook entries and fall back permanently on detected hook loss or
an owner-context mutation from another thread. ClearState and non-restoring
command-list execution invalidate the snapshot. Restoring command lists
preserve its previous known/unknown status. A non-null context-state swap
permanently disables tracking: a saved state can select D3D10 emulation, where
D3D11 void setters may silently do nothing. Getter fallback remains available.
No new user setting is needed.

Microsoft's [OMSetBlendState
contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetblendstate)
specifies that null factors store four ones and the context retains a bound
state reference. The tracked result must still AddRef before temporarily
unbinding it.
[SwapDeviceContextState](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate)
can replace previously saved bindings, so setter tracking alone is
insufficient. Independent source audit found no remaining blocker for the
verified native LiveCopy context. Implementation and focused restoration tests
are next.

The existing device-threading probe in the same verified flight reports
multithread protection off at install and unchanged after 1800 frames. Tracking
must additionally check that protection remains off before consuming a raw
snapshot. Protected or unavailable threading state uses the real getter.
Microsoft's [D3D11 threading
contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nn-d3d11_4-id3d11multithread)
requires externally serialized context access when protection is off. This
bounds the raw-pointer lifetime assumption without adding locks to every
tracked setter. The new protection query is part of the optimization's net cost
and must be included in measurement.

### 2026-09-18 -- guarded blend cache rejected before flight

Implemented the audited snapshot and real getter fallback, then exercised the
actual published proxy on WARP. The proxy fixture passes 529 checks; separate
swap-before-consumer and protection-before-consumer processes pass six and five
checks, respectively. The motion fixture passes 2506 checks. Independent review
found no remaining correctness blocker after resolving unknown-state
restoration, pre-consumer swaps, threading eligibility, reference lifetime and
COM cleanup ordering. The absolute symbol-enabled full build passes all 65
pooled rigs, three quiet rigs and the 249-key configuration contract; log:
build/blend-state-validation-r2.log. These results establish correctness of the
tested path, not a performance benefit.

The first hardware timing pass was unfavorable, but its fixture asserted a
cache hit before the mandatory first fill and compared the cache's exported
wrapper with a direct getter call. A separate higher-tier verification fixed
the warmup, added equivalent exported getter scaffolding, and used ten paired
rounds of one million calls with alternating order and 20000 warmup calls. The
plain setter's factory is explicitly resolved from Windows System32. Both
devices report the same RTX 5090 adapter LUID, vendor/device 10de:2b85.

Two clean hardware batches each pass 16 checks. Each query case records exactly
10020000 cache hits, zero fallbacks, zero setters and zero swaps during timing.
Each setter case records exactly 10020000 tracked owner setters. The cache
remains armed throughout. Paired median differences are positive in every case:

| Added CPU cost per call | Batch 1 | Batch 2 |
| --- | --- | --- |
| Cached null-state query | +4.34 ns | +4.25 ns |
| Cached non-null-state query | +3.50 ns | +3.53 ns |
| Null-state setter hook | +4.78 ns | +4.87 ns |
| Non-null-state setter hook | +3.36 ns | +3.37 ns |

These are warm, uncontended, repeated-state call measurements, not gameplay
timings or an FPS estimate. They do not rule out every workload-dependent
effect, but give no performance case for spending a flight: the guarded read is
slower even with every query hitting, and setter tracking adds another cost.
There is no positive query/setter break-even under the measured conditions.
Ruled out: shipping this guarded blend-binding cache as a CPU optimization,
because matched local measurements increase both costs. The earlier sampled-PC
cluster correctly identifies OMGetBlendState but does not establish that
replacing it with this bookkeeping is cheaper.

Archived the source patch against 4b5db2b, DLLs/PDBs, benchmark executable,
experimental installer, full-build log and both benchmark logs with SHA-256
checks under build/blend-state-rejected-4b5db2b. The corrected benchmark
fixture was compiled after the full build; production DLLs were unchanged. All
experimental source changes were removed. Restored the verified archived
5efb139 DLLs/PDBs to the ordinary build outputs and moved the experimental
installer into the archive. tools/install_edvr.py --target frontier
--verify-only confirms the installed native pair, loader and config. No game
files or settings were changed, no new collector was armed, and no flight is
requested for this candidate.

The next investigation should measure work repeated across the broad draw path,
including beginPanelOverride and uiDepthOnEyeDraw, before proposing another
narrow getter cache. Treat sampled PCs as locations to investigate; measure
their replacement and total call frequency before assigning savings. No Elite
engine operation has yet been proven safe to omit.

### 2026-09-18 -- per-frame building reconstruction decisions

The user reports settlement buildings repeatedly appearing to gain and lose
DLSS in the cockpit, while the on-foot display stays stable, and authorized a
focused capture build. This takes priority over the next CPU
micro-optimization. A successful full-frame DLSS evaluation treats the eye
image; reaching the 512-record motion limit does not itself bypass DLSS on a
building.

The source-level distinction is useful but not a diagnosis. In
`temporal_shader_source.h`, valid screen motion overrides the eye-space motion
and marks the pixel as tracked foreground. The on-foot screen therefore avoids
`backgroundHistoryHidden`, which can replace cockpit world motion with an
offscreen lookup to force NVIDIA to reject history. Ordinary cockpit pixels
also use the head/world distance split and depth dilation when exact motion
does not claim them. Separately, post-DLSS UI reconstruction can replace or
constrain trained colour where its coverage claims the pixel.

Enumerate the possible failures together before changing any rendering:

- Wrong or changing motion: the final head/world/body/ship/terrain/holo/mesh/
  screen path and its physical motion disagree with the building's movement
  between raw frames. Correct the responsible motion source or classification.
- False history rejection: the hidden-history or screen-invalid bit appears
  over the affected building as it loses stability. Correct the depth/history
  decision after checking actual occlusion; do not globally disable rejection.
- Incorrect UI coverage: pre-UI reconstructed colour is stable but the final
  treated image changes over the building. Trace the UI producer and correct
  the coverage; do not mask the effect with sharpening.

Earlier evidence does not select a fix. Corrected alignment of eye_165144 did
not establish excess reconstruction flicker, and its ledger kept history with
no camera jump. The verified 81eeedb reverse flight recorded hidden-history
sentinels on 0.187% of the landed eye (195609) and 0.002% on foot (195653), but
these are single snapshots, without building-local attribution. They cannot
explain a whole-building effect by themselves. The previous terrain footprint
bug was already fixed and confirmed; do not re-propose it as a new finding.

The measurement extends each paired crop with D00..D15 binary decision maps and
P00..P15 colour before UI reconstruction. The existing T crops remain the final
colour. A manifest ties each slot to its actual scene frame, input and output
extents, crop origins, treatment/history state and UI mode. Missing or
unsupported captures must be explicit. Diagnostic shader work is restricted to
an explicitly requested capture, with production motion/history unchanged.

`tools/eye_decisions.py` reads the manifest without writing files, checks the
binary frame/format/crop contract, and reports path/rejection fractions, motion
magnitudes and P-versus-T differences for the whole crop or an input-crop ROI.
Fixed-raster changes are not object tracking: head motion and jitter can move
geometry through a pixel. Raw C crops and the motion ledger remain necessary to
test whether the vectors follow the building. Capture intervals are not
performance measurements.

Implementation uses a separately compiled MV shader variant; its capture UAV
uses slot 7 only for the explicit full-frame NVIDIA capture. Normal fast and
diagnostic shaders retain their existing outputs. Foveated DLSS cannot share
that slot with its lead vectors and is explicitly unsupported by this trace.
The manifest publishes C/D/P/T files only after successful writes, with a
per-run taken ledger preventing an old staging texture from filling a failed
slot. Diagnostic staging and the GPU decision texture are released after the
run. Screen-invalid pixels have no physical correspondence: their diagnostic
XYZ are zero and the rejection bit identifies them; the game still receives the
original rejection sentinel.

Focused validation: all five embedded temporal shader variants create on WARP;
the screen/motion rig passes 56528 checks, including normal-versus-trace motion
equality, hidden-history versus physical motion, screen-invalid handling and UI
exemptions. The reader self-test covers malformed, missing and stale inputs,
unknown/nonfinite decisions, crop mapping and bottom-up BMP row attribution.
The first full build compiled graphics but its sandboxed test subprocesses lost
the compiler PATH. The elevated run passed the new capture tests but an
existing native missing-loader case missed its 100 ms startup deadline under
eight-worker load; it passed all 247 checks in isolation. Use `EDVR_JOBS=2` in
the build environment: the existing `--jobs` batch argument shifts `%0` before
the runner reads its script path. The reduced-concurrency run exposed another
existing fixture assumption: `original_draw_probe_test` searches binary source
for literal LF sequences, but the restored local `vscreen.cpp` had CRLF. LF
normalization leaves its Git content unchanged; the fixture then passes 620
checks. The next full run compiled the production DLLs and reached 606 checks
in that original-draw fixture before its asynchronous timeout case failed
(`timeout preserves ready visibility while retiring pending statistics`). This
was not a green full-suite result. The user explicitly requested skipping
further reruns and proceeding to the flight; no further suite run or rebuild
was performed.

Deployment: installed the already compiled candidate with the sanctioned
installer after dry-run, then `--verify-only` confirmed the native pair, loader
and runtime config. Live EDVR/DLSS settings were preserved. The binary label is
`v0.17.0-35-g2a23ee7-dirty`; committing this source afterward does not relabel
that DLL. Graphics SHA256 is
`f2bbcfaf23c04b6f1a7ced635b9f8fdf3e9fbb1748a52d54d42d8cc57ee43a74`; runtime
SHA256 is `c418955d6e33f23af977ebb70e9c5c76d2ee8cd6ceda0a1ceee89d873a590d27`.
The exact binaries/PDBs and provenance manifest are retained in
`build/settlement-decisions-frontier-2a23ee7-dirty`. The next flight needs one
paired capture of the visible cockpit failure; it is not a performance run.

### 2026-09-18 -- 160859 catches moving-ship claims on stationary buildings

The user requested the last eye dump from the latest run. The sanctioned log
reader verifies `edvr_gfx_20260918_160709.log` against the installed candidate
`v0.17.0-35-g2a23ee7-dirty` (linked 22:00:39 UTC, build 6AADB487), not the
later source-only commit. Use eye_160859, not the earlier eye_160853. Its 16
slots cover consecutive scene frames 8213-8228. All D/C/P/T files are valid and
matched; both-eye ledger rows retain DLSS history, valid camera rows and raw
capture, with no jump. All manifests report DLSS success and no reset. Input is
2481x2121; output is 3818x3264; UI mode is legacy. The user reports walking
NPCs and a drone flying overhead during this scene.

The building-local trace selects the wrong-motion discriminator. Coordinates
below are relative to the 1400x1400 input crop at full-input origin (540,360).
The central facade ROI is (680,385,150,35), visibly including fixed wall faces
and a few HUD/terrain pixels. Ship-path coverage grows from 6.10% at frame 8213
to 50.76% at 8221 and 52.50% at 8227, then drops to zero at 8228. The world
path correspondingly recovers to 87.87%. This is not just a whole-eye aggregate
or a fixed-raster edge moving through one pixel: the colored ownership overlay
covers stationary facades, tanks and ramps across the settlement.

At frame 8221, the median physical vector on ship-claimed facade pixels is
(6.127,0.018) input pixels; on world-claimed facade pixels it is (0.144,0.003).
Raw-frame phase alignment, corrected for raster jitter, estimates the facade's
physical vector at (0.194,0.173). Subpixel phase estimates are approximate, but
the roughly six-pixel horizontal discrepancy is far beyond that uncertainty.
The pre-UI image also follows the building's near-zero horizontal motion, not
the ship vector. For this comparison, D is previous-minus-current unjittered
motion: D = current-minus-previous raster jitter - raw image displacement.
Output displacement must additionally be divided by output/input scale.

History rejection in this ROI reaches 11.83% at frame 8222, versus 0.21% after
ship claims disappear at 8228. At 8221, 21.35% of ship-claimed facade pixels
reject history, versus 0.36% of world-claimed pixels. Rejection is therefore
associated with an already incorrect correspondence; disabling rejection would
retain history from the wrong part of the image. The first whole-eye hidden
count (814/5262201, 0.015%) hid this localized effect. No screen motion is
active in this cockpit capture; on-foot screen motion can override this coarse
ship path, consistent with the earlier scene difference.

The user asks why on-foot mode avoids this. The distinction is the final
screen-motion override, not proof that Elite culls the scene differently. For
valid on-foot screen pixels, screen motion replaces the earlier world/ship
candidate and sets trackedForeground, bypassing backgroundHistoryHidden. Thus
the coarse ship claim demonstrated here need not reach DLSS for those pixels.
The latest dump is cockpit-only; this explanation follows the source and
earlier on-foot evidence, rather than a paired on-foot capture from this run.

The independent post-UI discriminator does not explain the broad building
switch. Replaying the legacy UI resolve against frame 8213's exact inputs
matches the final building band to within one 8-bit step (99.99% exact pixels).
Only 1.304% of its output pixels differ from pre-UI DLSS: 0.228% of the ROI is
current UI, 0.052% retained UI history, and 1.024% the inactive faint-flat
hold. The left dome remains entirely world motion, without current UI or
retained UI activity; its sparse P/T differences are all explained by the faint
hold, averaging only 0.156/255 absolute RGB across that ROI in frame 8213. The
maximum individual pixel difference alone was misleading. Ruled out here: broad
UI history clamping as the source of the demonstrated wall/tank motion
switches.

Source attribution: `insideShip` tests a padded world-space box and proximity
to a recorded part, then `shipPixel` overrides world reprojection. The captured
`objects.z=900` permits a 30 m radius around each part. Two ship candidates are
present in frame 8213; their final boxes span approximately 132x140x216 m and
89x134x237 m, with four and five sampled parts. Box dimensions alone do not
prove a pixel belongs to the object. The dump lacks each candidate's part
positions, motion matrices and source identities, so it cannot decide whether
the claim comes from a real moving group spilling onto nearby buildings,
mistaken pool correspondence, or both. A low rigid-fit residual is not proof of
object identity. Do not describe the drone or NPC movement as false.

Ruled out for this captured interval: per-building DLSS bypass or whole-eye
history resets, because all frames execute DLSS successfully with history and
the affected buildings change motion source inside the same treatment. The
512-record mesh cap is not this switch: the facade has zero mesh-path pixels.
This evidence does not establish that every reported shimmer has one cause.

The next correction should require evidence that a moving group owns the
visible surface before applying its motion. Preserve true movers and station
rotation; do not reduce an arbitrary distance threshold or globally classify
the settlement as static. An isolated temporary test of the existing coarse
ship fallback can discriminate its visual contribution if needed, but is not a
permanent fix for moving ships. No live settings or production code were
changed during this analysis.

Local evidence is under `build/settlement-decisions-analysis-160859`: copied
capture, integrity `report.json`, `region-report.json`,
`ship-claim-report.json`, and labeled `ship-claim.png`. All 32 motion-ledger
rows were checked. Capture overhead makes this interval unsuitable for FPS
comparisons. No compiler or full-suite rerun was needed for this analysis.

### 2026-09-18 -- coarse moving-object fallback A/B prepared in Frontier

The user clarifies that walking NPCs are usually occluded by the buildings.
Pool-based motion detection reads the shared instance buffer, not a list of
visible pixels, so a moving record does not prove the corresponding NPC was
submitted or survived depth testing. This observation strengthens the need to
tie motion to visible surface ownership; it does not identify the offending
mover or establish unnecessary NPC draws.

The user authorized the controlled A/B. With the game stopped, copied the live
Frontier INI into local staging, used an explicit patch to change only
`#temporal_aa_objects_ships_metres = 1000` to `temporal_aa_objects_ships_metres
= 0`, and verified the complete before/after content including original
encoding and line endings. The staged source and original are under
`build/settlement-ships-off-frontier-56a5815`; the four staged native package
files match the already installed diagnostic. The repository default INI was
not used to replace the tuned live configuration.

Applied through `tools/install_edvr.py --target frontier --root <stage> --ini
--tag settlement-ships-off`, after dry-run and a check that the live INI still
matched the staged original. The installer created transactional backups and
receipt `edvr_native_receipt.json.pre-settlement-ships-off-20260918-162942.bak`
in the Frontier product directory. Both staged-config verification and normal
build verification pass. Live INI SHA256 is
`7802f3347940559ba5899df574e742ac50a957cb0e2bfcc3dc2cedc544c9debe`.

Only the coarse moving-ship handoff is disabled. DLSS, exact mesh motion and
station/body motion remain configured as before. Restore the prior commented
line (default 1000), preserving any subsequent user changes, after the A/B or
when a replacement is ready. This setting persists across restarts and is not
an automatic timed test. Do not call it a completed rendering fix.

Next run: same landed cockpit view, unchanged DLSS/headset settings, one normal
eye dump while observing whether the buildings still switch appearance. Check
the same compiled label `v0.17.0-35-g2a23ee7-dirty`, path 3 absent over the
facades, and correspondence to raw/world motion. Visual improvement would
confirm this fallback's contribution; remaining shimmer requires inspecting the
other recorded paths rather than assuming all causes are resolved. No rebuild
or full-suite rerun was performed for this configuration-only test.

### 2026-09-18 -- 163223 confirms ship suppression; intermittent global flash remains

The user reports substantially less flicker with the coarse ship fallback off,
but occasional flashes remain. Their clarification is important: all buildings
appear to flash together, resembling existing station flicker; the event is
very intermittent, and they are unsure whether the eye dump caught one. They
suggest a history reset. Do not equate this residual event with the previously
demonstrated local moving-ship claims without evidence.

Verified `edvr_gfx_20260918_162958.log` and the matching native runtime log
against `v0.17.0-35-g2a23ee7-dirty`; the live INI still has
`advanced.temporal_aa_objects_ships_metres = 0`. Eye_163223 has all 16 matched
C/D/P/T sets, scene frames 13473-13488, unchanged input/output dimensions and
legacy UI mode. The reader reports complete. All 32 eye-ledger rows retain
valid camera rows and history, with no jump, and all frames execute DLSS
successfully without reset. Across 31.36 million captured decision pixels,
ship, body, second-body, stepped-body and invalid paths each occur zero times.
The coarse ship suppression is therefore effective, not just an INI inference.

Adjusted the building ROIs for the changed head angle; they are not identical
pixel populations to the prior dump. Central facade (664,486,150,25) reaches
only 0.213% hidden-history rejection, versus 11.83% in the prior facade sample.
The new building band (320,370,960,155) peaks at 0.185%. The left dome remains
100% world path with no history-hidden flag. P/T changes affect at most 1.04%
of the building band's pixels, with whole-region mean absolute RGB difference
0.054-0.078/255. This capture does not establish a broad post-UI replacement.
Motion-compensated pre-UI changes on unrejected world pixels are small in the
sampled band (roughly 0.63-1.37/255 mean absolute RGB per adjacent pair), but
that is not proof an intermittent flash outside these 16 frames is resolved.

The full log separates reset evidence from the user's plausible hypothesis.
`dlaa totals` reports 12 reset eye-frames at 16:31:38.779, 16:31:58.792 and
16:32:18.793. Ruled out: additional explicit NVIDIA resets during that bounded
40-second interval. This does not cover the final unsummarized seconds, an
unrecorded internal model response, or a motion discontinuity without reset.
Native omissions end at zero, with zero missing projections or stood-down
frames; the session's reset total includes earlier loading/transitions.

Another discriminating log line is at 16:32:05.607: the dominant-body path
activates on a 64-record cluster (29% of movers), fit residual 0.026 m, turning
0.0145 degrees and moving 0.099 m over a 32 ms pair, with a 4096 m grid extent
and 1500 m reach. This is the separate station/body fallback, not the disabled
moving-ship path. It is absent during the later eye capture. Availability does
not prove that this path claimed buildings at the flash, but broad temporary
body ownership is a candidate for a simultaneous event. Auxiliary-camera jump
messages are also not evidence that those rows reached temporal reconstruction.

Existing instrumentation can discriminate these without another build:
`hotkey.dump_camera = PAUSE` writes the camera ring and
`temporalPassDumpHistory` immediately and again two seconds later. The temporal
ring continuously retains 4096 eye calls, about 23 seconds at 90 FPS or 34 at
60 FPS for stereo. It records requested/actual NVIDIA reset, dimensions,
source-screen changes, treatment output, world/body availability and motion
translation. No temporal-history key dump was present in this run. Press Pause
just after the flash, keep Elite focused and stay in-scene for at least three
seconds; the retrospective rows avoid needing to anticipate the event. These
CPU inputs still do not prove per-pixel body claims; use any matching eye trace
if available before calling the second cause confirmed.

The existing ship-off setting remains temporary. A permanent mover fix needs
actual visible-geometry ownership; stale sampled pool indices or nonunique
material/type signatures cannot safely identify current instances. Existing
depth-tested mesh coverage may provide the raster gate, but association to a
specific sampled mover remains unproven. Do not disable station motion or
change radius thresholds to compensate for this unconfirmed residual cause.

Copied artifacts and reports are in
`build/settlement-decisions-analysis-163223` (`region-report.json`,
`regions.png`, `motion-aligned-report.json` and the integrity audit). No
rendering/config changes, new build or full-suite rerun were made during this
analysis. The next requested capture uses the installed history key rather than
another unsynchronized 16-frame eye dump.

### 2026-09-18 -- Pause after global flash isolates a camera-motion pulse

The user pressed Pause immediately after the buildings flashed. Verified
`edvr_gfx_20260918_164718.log` against the installed literal build label
`v0.17.0-35-g2a23ee7-dirty`; comparing with HEAD would reject this
intentionally unchanged diagnostic. The live coarse ship setting remains zero.
The immediate camera/temporal histories are timestamped 16:48:57.377/.380, with
rows-frame 10833; the delayed histories at 16:48:59.386/.389 reach 10953. Each
temporal snapshot contains 4096 eye calls. Analysis and source provenance are
retained under `build/pause-flicker-analysis-164857`.

Ruled out for this reported event: an explicit NVIDIA reset, caller-requested
reset, size/format change, source-screen switch, treatment-output switch or
dominant-body activation. In the three seconds either side of Pause, all 630
recorded eye calls (315 frames, 10639-10953) have flags 2, events 8, inputs
0x5AEF, output 2 and dimensions 2481x2121 to 3818x3264. World, rows, bound
buffer and history stay valid; body/source/origin bits stay absent and body
translation is zero. This establishes CPU-side state, not individual pixel
ownership or NVIDIA's internal history response.

The aggregate NVIDIA reset count rises from 10 to 12, but both added reset eye
calls are frame 10512, about 4.994 seconds before Pause, during an earlier
origin/rows transition. Its world delta is zero. It is distinct from the
reported flash; the count alone would have misattributed that event.

The positive signal is a two-frame world-translation excursion immediately
before the keypress. Frame 10823, about 166 ms before Pause, supplies
(-0.02813096, -0.09491654, +0.04902185) m. Frame 10824, about 150 ms before
Pause, supplies (+0.02772349, +0.09611722, -0.04694854) m. Both are about
0.1105 m and nearly reverse, while adjacent frames 10822/10825 are only
0.000061/0.000036 m. Both eyes receive the same vectors. These are the exact
`p.tvCam` inputs to motion reconstruction, not a detector's discarded
candidate. The first dump reports -168.8/-152.5 ms relative to its slightly
later dump time; key-relative values account for that difference.

There are six such pairs in the combined 25.5-second history: 10710-10711,
10721-10722, 10812-10813, 10823-10824, 10858-10859 and 10869-10870. Their first
frames span about 1.918 seconds before Pause to 0.589 seconds after it; the
last return is at +0.605 seconds. Every pulse exceeds 0.108 m, with two-frame
residuals about 0.0020-0.0025 m; the largest other translation is 0.00341 m.
The delayed history repeats the pre-key event, excluding Pause's own logging as
the cause of those earlier pulses. Timing supports a motion discontinuity as
the residual-flash candidate; it does not prove which camera write produced it
or that every pulse was visibly noticed.

Source audit: `chooseCameraRows` retains the buffer object bound at an early
scene-depth draw, then picks its latest continuous write at the first temporal
submission. It does not retain the version actually consumed by a building
draw. Identical-to-previous rows are relegated to a fallback, creating another
selection discriminator when a different continuous candidate exists. The
chosen rows feed `worldFromRows` and both eyes share that frame's choice. The
current history omits candidate sequence numbers and draw-time matrices, so a
late rewrite, a different eye/pass, or a genuine draw-camera excursion cannot
yet be separated. The log's stable `CAM.scene` and zero `cameraStep` read float
1100, whereas temporal uses floats 932-943: they are not an independent
measurement of the same full camera matrix.

The next diagnostic records selected-write provenance and matrices observed at
recognized rigid scene draws for both eyes, retrospectively with Pause. Missing
samples must be explicit; a stationary camera or a zero comparison cannot stand
in for an unexecuted probe. This is evidence collection before changing camera
selection. Translation clamps, smaller jump thresholds and landed-only
exceptions would hide the symptom while risking legitimate ship, headset and
station motion.

Implemented the diagnostic in the existing Pause history. `TCAM` records the
selected buffer and observed-write sequence, bound-latch and identical-row
fallback provenance, candidate counts, and the first recognized rigid draw's VS
b1 write for each eye. It includes literal selected/draw matrices and depth
projection, their raw row-origin difference and rotation difference, and a
separate consecutive draw-camera delta in the same view convention as TEMP
world motion. A constant stereo offset alone is not evidence of a fault.

The draw latch shares the existing VS b1 query where possible and requests at
most one additional query per identified eye/frame. The five existing rigid
shader families qualify even when the separate glitch detector is disabled; the
shader hash is recorded so the first draw is not silently equated with every
building. Eye lookup only reads the settled depth pair; it cannot scan or
reselect it. With no recognized draw, unknown eye mapping, missing current
write or invalid overwritten rows, explicit flags distinguish missing evidence
from a zero delta. The separate 32-resource observed-write table records
rejected rotations and counts evictions; the production 256-write chooser ring,
its sequence and selection behavior remain unchanged. This observes mapped
writes, not arbitrary unhooked GPU buffer copies.

All 4096 existing TEMP eye records remain in each Pause snapshot. Full TCAM
detail is bounded to the newest 512 eye calls, about 2.8 seconds at 90 Hz or
4.3 seconds at 60 Hz, with an explicit omission count. Immediate and delayed
snapshots therefore cover the reaction-time event and its aftermath without
writing full matrices for the entire long history into the default 4 MB log.
The literal float rows use nine significant digits; the worst-case formatted
line fits the logger's 1200-byte buffer. The intended environment remains the
Quest 3 / VirtualDesktopXR setup above. Missing first-draw samples on a
different shader family require another observation point, not a conclusion
that camera motion is correct.

Validation and deployment: the absolute `build.bat` with `EDVR_JOBS=2` passes
the full native build and all gates, including 90 depth-pair checks, temporal
history preservation and shader classification checks. Independent source
review found no remaining correctness defects. Installed in Frontier through
`tools/install_edvr.py` after dry-run; `--verify-only` confirms the native
pair, loader and config. Live INI hash is unchanged at
`7802f3347940559ba5899df574e742ac50a957cb0e2bfcc3dc2cedc544c9debe`, including
the temporary ship fallback zero. Receipt is
`edvr_native_receipt.json.pre-camera-provenance-20260918-171452.bak`.

The compiled label is `v0.17.0-39-gde7e272-dirty`; subsequent source commit
does not relabel these binaries. Graphics SHA256 is
`cef22660034842695fbf815afd440336570b9b98c7afdd6e0cfb4c600bb518c7`; runtime
SHA256 is `9e0728ca10f45bd913e32fc5faf619f34b5e9b20b59af5f6778228ebfa2acaa4`.
Exact DLLs/PDBs, loader, full build output and manifest are retained under
`build/camera-provenance-frontier-de7e272-dirty`. Next flight repeats the same
Pause-after-flash action, followed by at least three seconds in-scene. This
build collects evidence; it does not fix the remaining flicker or promise a
frame-time improvement.

### 2026-09-18 -- 171847 Pause: no camera pulse; dominant body activates

Verified `edvr_gfx_20260918_171708.log` against `v0.17.0-39-gde7e272-dirty`,
build 6AADC50E, linked 23:11:10 UTC. It contains both complete Pause snapshots:
first camera history at 17:18:47.092, TEMP at 17:18:47.096 through frame 10679;
delayed TEMP at 17:18:49.099 through 10803. Each has 4096 TEMP and 512 TCAM eye
records. The file is 3149.1 KB and does not hit the log cap or truncate a TCAM
line. The overlapping 3848 TEMP and 264 TCAM records have zero payload
disagreements. Reports are retained under
`build/camera-provenance-analysis-171847`.

Ruled out for this capture: a recurrence of the prior reversing 0.1105 m
world-translation pulse. No world translation exceeds 0.01 m anywhere in the
combined history. The prior pulse remains unexplained; this absence does not
retroactively excuse it. Also ruled out near this Pause: an explicit NVIDIA
reset, caller reset, dimension/output switch, source-screen change, origin
step, lost world/rows/history validity, identical-row fallback or observation
table eviction. The only stereo reset is frame 10416, about 4.095 seconds
before Pause, with zero world/body delta. The later aggregate rise from 12 to
14 therefore does not establish a reset at the reported event.

The new positive signal is body motion: frames 10645-10765, about 514 ms before
Pause to 1368 ms after it, have body availability set on both eyes (242 calls,
inputs 0x5AFF); before and afterward it is absent (0x5AEF). Body translation
magnitude is roughly 0.8-1.5 m while world translation stays small. At
17:18:46.577 the activation line identifies a 41-record cluster, 77% of pool
movers, rigid-fit residual 0.005 m, turning 0.2997 degrees with translation
term 1.815 m over 32.5 ms. Those are transform parameters, not a measurement of
the object's linear speed. The grid extent is 4096 m on every axis and the
configured reach is 1500 m. This is the separate dominant-body fallback, still
enabled while coarse moving-ship fallback is zero.

All 644 eye records within three seconds of Pause have flags 2, events 8,
output 2 and unchanged 2481x2121 to 3818x3264 dimensions. Every corresponding
TCAM is present with choice flags 0x21F and draw flags 0x9F: the same buffer
object, a later selected write, but different selected/draw rows. There are no
twins or observation evictions. Around frames 10666-10667 the frame has 746
valid writes, exceeding the chooser's 256-write ring; all retained candidates
are from the bound object. That is a capacity observation, not proof this
selection is wrong.

TCAM does not justify replacing selection with the first recognized draw. For
example, frame 10667's sampled draw rows equal frame 10666's selected rows,
whereas frame 10666's draw-to-draw delta reports roughly 111 m and 156 degrees
from an earlier auxiliary-looking snapshot. The selected world delta remains
tiny. More fundamentally, the five recognized shaders are generic rigid
material families; their proven original vertex path consumes b1 rows 270-275,
not the diagnostic rows 233-235. TCAM D is a snapshot of those latter rows in
the bound buffer at the draw, not proof the shader used them to render that
building. Its first draw can be cockpit, scenery or another supported object;
depth first-bind ordering provides the eye label. Do not turn this diagnostic
into a first-draw camera fix.

The body classifier's share is measured against pose changes, not the entire
visible scene. After fitting a dominant moving cluster, its grid is seeded from
live records with the accepted signatures and expanded by the configured reach.
The shader gives body motion to depth points in those cells unless a later
exact path overrides it. A compact moving object can therefore produce broad
candidate coverage. The log proves activation at the relevant time, but lacks
this flight's depth/grid/decision images; it cannot identify the 41 records as
the drone or prove a particular facade received path 4.

Next diagnostic: let Insert arm a one-shot paired eye capture that waits for
the next eligible body activation. Reuse the existing C/D/P/T run and capture
the triggering frame before its motion decisions are generated. The CPU history
supplies the preceding off state; D identifies actual body claims on the
visible buildings. This avoids another eye dump taken after the event. There is
no continuous GPU image buffer or speculative rendering change.

Implemented `advanced.eye_run_trigger = manual|motion`, default `manual`.
Motion mode changes Insert into an arm for one capture. If body state is
unknown or already active, it first waits for an observed off frame. An
unsupported rising edge logs every missing condition and leaves the arm waiting
for another off/on edge; becoming eligible mid-episode does not start a partial
capture. Repeated arming preserves the pending request, and config reload or
shutdown explicitly cancels it. Normal manual capture is unchanged.

The trigger checks the established paired, full-frame NVIDIA path, supported
format, existing history/resources at matching input/output dimensions,
world/depth/accepted camera rows, no reset/size/source transition, and no
active on-foot screen motion. A zero stored projection pair is allowed because
the existing depth fallback supports this flight. It starts the existing run
before C00 metadata/raw staging and before D00 shader selection, preserving the
activation frame in C00/D00/P00/T00. The accompanying object draw ledger and
draw census can first contain complete data for the following frame, because
the trigger frame's scene draws have already run; match frame IDs.

The pure trigger tests cover off/on arming, already-active episodes, rejected
edges, retry only after a new off/on edge, and missing eligibility reasons.
Independent review confirms the recorded 2481x2121 to 3818x3264 legacy-UI
NVIDIA cockpit path meets the gates. No camera, object fit, ownership radius or
shader motion formula changes are part of this diagnostic.

Validation/deployment: the absolute `build.bat` with `EDVR_JOBS=2` passes all
gates, including the new trigger tests and 250-key config contract. The
installed label is `v0.17.0-40-gdb78d39-dirty`. Exact DLLs/PDBs, build output,
package and provenance manifest are in `build/motion-trigger-frontier-db78d39`;
the source commit afterward does not change the compiled label. Graphics SHA256
is `bf8d8cb2812ddb93e8073a0d9a6bfbf09f9e18775b25f05a46705a6a7bd0947a`; runtime
SHA256 is `4b610d51baf4574ddf465506592935dc6d0d657ad57586766044c8d5f63fb0c3`.

Staged the current live INI and explicitly added only `advanced.eye_run_trigger
= motion`, preserving its original CRLF and every other byte. The prior ship
fallback zero remains in force. Sanctioned install with the staged root and
`--ini` followed dry-run and a fresh unchanged-source hash check. Both
staged-INI and normal-build `--verify-only` pass. Receipt is
`edvr_native_receipt.json.pre-motion-trigger-20260918-173700.bak`; installed
INI hash is `eca5e5fbaac659c83aaa39ca78377287933ab65c4e2985423ac0dc52ac8baa20`.
The original is staged as `edvr.before.ini`. After this diagnostic, remove only
the added trigger line (or select manual), preserving any newer user changes;
do not blindly restore the complete saved INI. The mode persists across
restarts, but each run needs a fresh Insert arm.

Next flight: keep the affected buildings near the centre of the same landed
cockpit view, press Insert once to arm, then wait for the flash. Press Pause
after it and remain in-scene for at least three seconds before exit. The
one-shot should log armed and triggered, then produce a complete paired
manifest with C00/D00/P00/T00 at the logged trigger frame. Unsupported,
cancelled or never-triggered arms are distinct log outcomes, not successful
captures. Compare decision path 4 with static facades; TEMP/TCAM provide timing
context. This remains a correctness diagnostic, not a frame-time test.

### 2026-09-18 -- 174658 automatic capture proves body claims on buildings

Verified `edvr_gfx_20260918_174448.log` against the installed compiled label
`v0.17.0-40-gdb78d39-dirty`, graphics build stamp `6AADCA58`, before reading
the flight. Insert arms at 17:46:28.593; the next eligible dominant-body
activation triggers eye_174658 at 17:46:58.261, frame 12693. The activation
reports 85 records, 38% of pose changes, 0.027 m fit residual, 0.0044 degrees
and a 0.025 m translation term over 32.3 ms, with a 4096 m box and 1500 m
reach. Translation terms are rigid-transform parameters, not object speeds.

Manifest and motion CSV independently agree: C/D/P/T00..15 correspond to
12693..12708, including the trigger frame. All 64 referenced files exist; the
CSV contains exactly both eyes for each frame. All 32 evaluations have valid
body, world rows and NVIDIA history, no reset request or origin jump, and
stable input/output sizes. All 16 manifest entries report successful DLSS
evaluation and valid history. The legacy `history=0` CSV field is not the
NVIDIA `dlHistory=1` field. No Pause history payload or TEMP/TCAM records occur
in this flight; the automatic trigger alone does not timestamp a user-observed
flash.

The first decision image contains 68,291 path-4 pixels, 3.4842% of its
1400x1400 crop. A body-only overlay forms a near-solid silhouette of the
visible settlement: left cylindrical building, green modules, tanks, central
ramp and facades, roof dome, main dark wall and right ramp/annex. Some rocks
are also claimed. The broad foreground terrain is mostly protected by its own
exact path. This is direct visible-surface evidence, beyond the earlier CPU
flag showing only that body motion was available.

Conservative fixed image rectangles over the main dark wall, white facade,
central ramp, tank wall and left cylinder receive body motion for roughly
72-99% of their pixels across the sequence. These are raster rectangles, not
object tracking: head movement changes how much of a surface remains inside
each rectangle. Whole-crop or whole-band percentages dilute the building claim
with sky, terrain and cockpit; do not interpret them as fractions of building
geometry.

The same-pixel counterfactual reconstructs current depth from D.z and the
recorded body transform, then applies the recorded world transform to those
same path-4 pixels. Reconstructing the original body D.xy agrees within 0.00048
input pixels throughout. At activation, body-minus-world error has median
0.0833 input pixels (0.128 output), p95 0.4406 input (0.678 output), and
maximum 0.7295 input (1.123 output). Across the run, median error ranges
0.056-0.207 input pixels; the worst frame, 12696, has p95 1.126 input (1.733
output) and maximum 2.891 output pixels. This is an incorrect accepted motion,
but its typical initial error is subpixel; it does not by itself establish that
the visible flash was caught. Most body pixels retain history; the settlement
band's pre-UI to submitted-image mean difference peaks at only 0.074/255.
Neither whole-scene history rejection nor the legacy UI resolve accounts for a
global flash in this sequence. There is no pre-activation P/T image in this
run, and the 85 fitted records have not been identified as NPCs or a drone.

Ruled out: a DLSS history reset or source-size switch during these 16 frames,
because every manifest entry has valid history, reset=false and stable
dimensions. Ruled out: dominant-body availability without visible building
claims in this capture, because final decision path 4 covers the actual
facades. This does not rule out another cause of flashes elsewhere.

Source explains the unsupported ownership: `object_probe.cpp::buildGrid`
expands from fitted members to all live records sharing their signatures, then
dilates their cells by the configured reach. Signatures identify types, not
persistent instances. `insideBody` accepts a pixel's reconstructed depth point
in that grid; it has no building-instance ownership proof. Later
terrain/holo/mesh paths can override it only where their own data is valid. No
new rendering change is justified merely by shrinking the radius, requiring a
stronger fit or declaring every settlement object stationary.

The existing mesh data cannot supply a broad static veto. Of 68,291 body pixels
at C00, 67,276 (98.51%) have no MeshCoverage record at all. Just 509 (0.745%)
have final-depth-agreeing coverage and a unique unchanged full compatibility
key/raw pose despite a changed recorded mesh origin, but an unresolved GPU
match. Another 506 covered body pixels fail final-depth agreement. A
conservative matching tie-break could recover those 509 pixels, but only
fragments of the facade: 261/9,689 on the main dark wall, 36/3,861 on the tank
and 23/4,391 on the central ramp; several other building regions have none. The
512-record cap and draw admission must be considered before adding per-object
work. Unchanged or shuffled records are also legitimate rotating-station parts,
so excluding every non-fit-member is not a safe replacement for visible-surface
ownership. Next work should establish that ownership for presently uncovered
draws, with a proven-static path and station/mover validation; no new flight is
needed to reconfirm this claim.

Artifact integrity has one reporting defect: the separate object ledger starts
at frame 12694, as expected because scene draws precede the temporal trigger,
but its old text labels C00..15 as 12694..12709. That label is one frame late;
use the manifest and CSV's 12693..12708. The ledger has 20 pool frames, 19
instance/palette/auxiliary streams, 349,350 draw rows and no skipped copies. It
cannot supply the trigger frame's already-finished original draws.

Evidence, checked images and read-only analysis outputs are archived under
`build/motion-trigger-analysis-174658`: integrity-report.json/.txt,
analyze_integrity.py, roi-report.json, roi-summary.json and roi-body-C00.png.
The same-pixel comparison is in roi-counterfactual.json; roi-findings.txt and
roi-body-building-band.png provide the compact report and visible proof.
ownership-report.json records the mesh intersection, input hashes and
invocation; ownership_intersection.py reproduces it and has a write-free dry
run. The installed graphics/runtime remain unchanged. Elite was still running
when cleanup was checked, so the temporary motion-trigger setting remains;
return Insert to manual at the next safe install, preserving other settings.

### 2026-09-18 -- static-surface test candidate and capture procedure

The 174658 capture establishes the unsafe ownership mechanism: a coarse body
path claims stationary building surfaces that lack mesh coverage. The candidate
adds a 96-bit geometry/rigid-pose fingerprint and raw depth to MRT7 during
eligible original draws. It does not reissue those draws or expand the
512-record matching table. A consecutive visible fingerprint match at the
world-reprojected pixel can replace a coarse ship/body proposal with world
motion. Moving, skinned, unsupported or unproven surfaces retain the existing
route. A one-bit raw pose change breaks the match; there is no movement
threshold. Fingerprints provide practical collision resistance, not a
mathematical identity guarantee.

The five original vertex shaders were checked against their captured DXBC: the
rigid position path uses packed vertex data, the six hashed pool pose words,
shared scene origin cb1[275] and scene projection cb1[270..273]. The
bone-palette path has nonzero boneBase and receives no owner. The detail
shader's extra switch culls vertices rather than applying another transform.
Prior confirmed Orbis captures eye_192513 and eye_195254 have changing raw
rotating-part poses, not unchanged station-local transforms. These are older
mesh captures; a rotating-station positive control with the new owner textures
is still required. The settlement captures 160859 and 163223 are not station
controls.

The new setting is advanced.temporal_aa_static_surfaces, default off. This
first candidate is restricted to full-frame NVIDIA temporal AA and the five
verified rigid VS families. Shader, runtime and consumer regression gates
passed before installation. Added ownership work still needs a flight timing
check; no performance improvement is claimed.

On-foot source-screen motion disables the consumer, but the producer can still
incur cost under full-frame DLSS. This candidate therefore remains default off
pending station correctness and on-foot cost validation. Its first flight is
the existing landed-cockpit view.

The producer retains bounded caches: 2,048 shaders per stage (64 MiB PS
bytecode), 1,024 patched PS/input pairs, 4,096 geometry resources and 8,192
geometry constant buffers. Exhaustion declines protection instead of changing
the fallback. The retained shader corpus has 469 unique PS blobs (6,560,040
bytes), but this does not bound all shader instances created during a flight.
The 1,800-frame log reports admission failures and sampled producer/restore CPU
time inside the ownership lock; it excludes lock acquisition and the
unsupported-family fast path. Use full-frame timing for the overall cost.
Modified draws are explicitly marked in the original-draw probe.

An eye run includes StaticOwner/StaticOwnerPrev binary textures and a JSON
availability marker. Motion decision flag 2048 records a confirmed static
match; path 2 is world motion. First verify these on the previously body-owned
walls, then compare clean frame-time windows. A remaining visible flash still
requires the Pause history: the earlier independent camera pulse has not been
explained by this correction.

For this test, focus Elite's desktop window, press Insert before watching, then
press Pause immediately after an observed flicker. Insert arms a one-shot for
the next eligible dominant-body off/on transition; it is not a continuous
recorder or a trigger for every kind of flash. Pause writes the camera and
temporal history immediately and again two seconds later. Keep the game running
in the same view for at least ten seconds before exiting. Report the mode
(cockpit/SRV/on foot) and whether all buildings or only specific objects
flashed. Both hotkeys require game focus. After this experiment, return Insert
to manual for general captures unless a specific event trigger is requested.

Validation and installation: the absolute build.bat completed with all 70 gates
passing (67 pooled and three quiet jobs), including the config contract. The
focused real shader corpus/WARP patcher passed 202 checks; production
controller passed 78; the unmodified production temporal consumer passed 181.
The consumer fixture changes body path 4 (0.04 pixels) to confirmed world path
2 (zero pixels); negative owner/depth/history/flag/jitter cases retain path 4.
The tests also cover original MRT colour/discard parity, one-bit pose changes,
bone exclusion, input-layout restrictions, two-eye history and state restore.

A final old/current temporal-shader reflection comparison uses the production
build macros and compiler flags. Temporary register counts remain 22 for fast
and 30 for normal/trace. Instruction counts increase 2234->2329 (fast),
2865->2960 (normal) and 2886->3002 (trace). This rules out increased compiler
temporary-register allocation, not added execution cost. The report and
reproduction harness are in build/temporal_reflection_audit.

Installed v0.17.0-42-gc8add89-dirty in Frontier after dry-run and live-config
hash comparison. Independent install_edvr.py --verify-only verified the pair,
loader and config. The only INI delta is the new switch set on. Archive
build/static-surfaces-frontier-c8add89 holds the exact DLLs/PDBs, original and
test INIs, manifest, build-validation log and VS transform audit. Graphics
SHA-256 starts c5732d9016e3df5c; runtime starts 7548265d90750b8a; full hashes
are in manifest.json. The installer backup receipt ends
pre-static-surfaces-c8add89-20260918-185005.bak. No flight result is claimed.

### 2026-09-18 -- 190059 static-owner regression; experiment disabled

User report: not everything blurred together, but performance was significantly
worse. The correct log is edvr_gfx_20260918_190059.log, version
v0.17.0-42-gc8add89-dirty, graphics stamp 6AADDB74. The paired eye capture is
190309, beginning at scene frame 12078. This report does not establish that the
less synchronized blur was caused by the new correction.

The defensible busy-scene comparison is prior flight 174448 window 5 versus new
flight 190059 window 7, with the same Quest 3 / VirtualDesktopXR / 90 Hz / DLSS
K and 2481x2121 input, 3072x3264 XR output. Depth-census draw counts are
similar: old 22,499/22,350 and 22,607/22,493; new 23,111/23,076 and
22,749/22,534. CPU median increases 10.606->18.394 ms and GPU elapsed median
14.342->22.090 ms. These separate elapsed spans are not additive or an isolated
measurement of GPU execution. The older scene reports about 59 FPS; the newer
scene falls through 42 to 37 FPS. This is a comparable scene, not a controlled
same-flight A/B.

New W7's complete 30-second sample ends 224 ms before the eye trigger; its
scope changes during drain. The capture therefore does not explain this
slowdown. New W6's 0.658/4.097 ms medians instead concern mostly menu/low-draw
frames (30-34 eye draws, about 90 FPS), with loading at the end; comparing it
with the old settlement window would be invalid. The corrected report is
build/static-surfaces-flight-190059/timing-corrected.md.

The 19:03:24.799 ownership summary reports 18,751,925 supported candidates in
1,800 frames, only 6,576 accepted (0.035%), 17,734,402 cache declines and
1,010,947 eye declines. The sampled mean of 0.390 us per supported check
implies about 4.063 ms/frame inside this added hook. It excludes locking and
unsupported-family overhead; it is not the complete cost. The aggregate cache
counter cannot distinguish resource/key/index-slice/state limits or allocation
failures, so naming a specific exhausted table would be speculation. Earlier
52,928 accepts show that this was not a completely inert path, but most later
work produced no ownership information.

The eye_190309 owner pair is available and valid: 225 current and 228 previous
pixels out of 5,262,201 have nonzero ownership, all exactly matching the
corresponding scene depth. All 225 current pixels finish on path 1 (head), with
no overlap with body pixels. Static-confirmed flag 2048 occurs zero times
across 16 frames and 31,360,000 decision pixels. Path 4 still claims 964,291
pixels across that run, ranging from 60,043 to 60,382 per frame. Thus the
correction did not engage the captured building surfaces; reduced synchronized
blurring cannot be attributed to it. Reproducible local evidence is in
build/static-surfaces-flight-190059/capture-engagement.md and its adjacent
JSON/analyzer.

Ruled out: this per-draw static-owner implementation as a performance solution,
because it adds millions of mostly unsuccessful admission checks while leaving
the existing capture, matching and motion replay intact. Raising its cache
limits and flying again would not address that architectural cost.

The static-building path should retain camera/world reprojection and avoid
per-object motion work. However, a static label must be checked cheaply enough
to detect new movement, bone animation, pool repacking and geometry changes.
The existing 512-record cap already returns before most expensive mesh
admission. Freeing static slots can admit more previously capped work, so count
both the saved work and newly admitted movers before claiming a speedup. The
existing pool probe deliberately uses delayed GPU readback: scanning full
WRITE_DISCARD mappings on the CPU is already documented as millisecond-scale
work in object_probe.h. No new draw suppression or CPU pool scan is justified.

There is currently no cheap, safe pre-cap static predicate. Exact
classification needs instance IDs and model-pool data, resolved after the cap;
moving those reads before the cap would charge millions of currently cheap
rejects. Delayed pool observations cannot detect the first moving frame, and
last-frame invisibility cannot justify skipping current-frame work. The
retained data contains nine visibility transitions despite unchanged bindings,
IDs and model-pool data.

Next offline discriminator: replay eye_190309's consecutive draws, instance ID
streams and model-pool captures in draw order, and simulate 512-record
admission with/without static exclusion. Count static records removed, later
movers admitted, residual cap saturation and lost first-moving-frame history.
Require unique geometry identity, exact pose comparison with rebasing handled,
write invalidation and no bone animation; reject the design if retained data
cannot establish consecutive identity. Both eyes must share the decision. This
tests the savings opportunity before a production prototype; it does not yet
prove a cheap runtime implementation.

Static exclusion alone also leaves the flicker unresolved: most building pixels
still lack exact ownership and can inherit coarse body motion. A world-motion
default with exact moving-object overrides is a possible separate design,
requiring evidence for rotating stations, bones, mixed draws and cap overflow.
Keep that correctness problem distinct from removing EDVR overhead.

Disabled and independently verified the experiment in Frontier. Same v42
binaries; only temporal_aa_static_surfaces=off and eye_run_trigger=manual
changed. Other settings, including ships_metres=0, are preserved. Archive:
build/static-surfaces-disabled-frontier-93af971, with exact payload hashes,
before/after INIs and manifest. Backup receipt ends
pre-static-surfaces-off-93af971-20260918-190805.bak. Insert again starts an
immediate eye capture; Pause records recent history and its delayed follow-up.
No rerun is needed to establish that this experiment regressed performance.

### 2026-09-18 -- offline static-exclusion feasibility

User authorized replaying the retained capture before another runtime change.
The analysis uses eye_190309 from the already authenticated v42 flight, with
the same Quest 3 / VirtualDesktopXR / DLSS K environment above. Frontier stays
on the verified static-surfaces-off/manual-capture configuration. No new game
build, installation or headset run is part of this analysis.

The authenticated Mesh/MeshPrev pair has 512 current records in 124 draws: 504
valid rigid records and eight invalid. Of the valid records, 501 have a unique
compatibility-key/raw-pose match in the previous capture; three are changed or
unmatched. Those 501 records fill 115 whole draws, with no mixed exact-pose
draws. All 501 have a different scene origin, so none passes the existing
same-origin raw-static candidate test. The 501 figure is an opportunity
requiring correct rebasing, not proof of static world identity or permission to
discard their history.

Within those 501 candidates, 446 records leave no marker pixels; 55 do, and 31
have any marker pixels agreeing exactly with final scene depth. Those 31 cover
2,182 final-depth pixels. At draw granularity, 80 of the 115 whole exact-pose
draws leave no marker pixels, eight leave markers with no exact final-depth
pixels, and 27 have some exact final-depth contribution. These measurements are
available after the coverage draw/final depth comparison; they cannot decide
whether to skip that work beforehand. The other 11 records cover 223,917 exact
final-depth pixels. Record counts therefore do not describe either pixel
workload or potential frame-time savings.

The scene-wide ledger is version 1: 351,208 rows over 20 frames. It records
shader, counts, start instance, draw kind and whether t33 held the pool. It
does not retain eye/depth eligibility, geometry bindings, base/start index,
write epochs or the exact runtime cap state. The detailed mesh snapshot is
bounded to 4,096 draws from frame 12080 with 28,020 dropped. Of its 4,002
supported-family draws, only 444 have vertex payloads, and no second rich frame
exists. Existing identity-fusion fixtures explicitly borrow later geometry and
binding descriptors by occurrence and reconstruct partial depth; their
synthetic compatibility groups cannot establish persistent game identity.

Ruled out: exact full-scene admission replay from eye_190309, because the
required per-draw eligibility and identity inputs are absent. Simulating the
first 512 ledger instances would not reproduce meshMotionDraw: admission is per
eye, checks all pipeline/resource guards, and rejects an entire draw when
count+instances exceeds 512. Smaller subsequent draws may still fit. Neither
slots freed nor newly admitted movers can be reported as measured results.

The independent pool/ledger census examines 188,886 supported-family, t33-held,
shape-compatible draw rows referencing 999,806 instances across the run.
Comparing stable non-pose bytes as a conditional content signature finds 4,294
all-rigid, unique-signature, equal-pose draw/reference observations; 184,098
draw rows have missing or ambiguous rigid identity and 494 are bone-only. These
are repeated observations, not unique scene objects. No static-to-moving
transition is established with a unique signature across three frames. This
absence does not validate wake-up behavior: 995,018 instance references have
ambiguous content identity.

The weaker same-slot comparison does find 23,120 unchanged-to-changed
draw-reference observations, representing 3,621 frame-local record events.
Slots are not persistent identities, so these cannot be called newly moving
objects; they demonstrate why a stale label indexed by pool slot is not a safe
alternative. Unknown identity, repacking and actual motion must stay distinct.

Current-flight timing gives a more useful CPU priority than raw static counts.
The last 1,800-frame window's exclusive 1/256 samples project to the following
stage envelopes (clock overhead included):

| Mesh hook stage | Estimated ms/frame |
|---|---:|
| Fast classification | 0.567 |
| Scene/depth/eye/cap | 0.403 |
| Pipeline guards | 0.034 |
| IA/resources | 0.027 |
| Preparation | 0.008 |
| Accepted work | 0.202 |
| Full hook | 1.241 |

The approximately 0.970 ms of early checks remains if an optimization only
removes accepted work. The 0.202 ms accepted envelope is neither a total EDVR
cost nor a guaranteed static-filter saving. Scaling it by 115/124 would assume
homogeneous draw costs and a representative single capture; it cannot establish
an upper bound. Coverage raster samples likewise cannot be scaled into a GPU
saving, and both eyes' cap probes find eligible rejected draws (64 draws total
in the bounded sample). Freed slots can refill rather than reduce work. This
supports auditing repeated eligibility bookkeeping before adding another broad
classifier, while preserving current scene/eye/cap selection and using offline
equivalence tests before any flight.

Decision: do not implement a pre-cap static skip from this evidence. No nonzero
safely avoidable pre-capture set has been established; that is an evidence
limit, not proof that static exclusion is impossible. A post-capture static tag
would still pay capture/coverage costs and would not protect unowned building
pixels from coarse body motion. The first-moving-frame correctness gate and net
CPU/GPU benefit remain open.

Artifacts: build/static-exclusion-190309/report.md and report.json contain the
ledger census, every input's hash, parser provenance and synthetic checks;
build/static-exclusion-coverage-190309/report.md, mesh-probe-report.json and
distribution.json contain the admitted-record analysis. Its
admission-extract.txt preserves the version-verified current-flight timing
lines read through tools/edvr_log.py. No production source changed.

### 2026-09-19 -- repeated eligibility checks: hypothesis and gates

User authorized the next CPU pass. The candidate is the live depth/eye lookup
inside meshMotionDraw, not a static-object cache. It first calls
depthProbeIsSceneDepth, then depthProbeSceneDepthFormat for eye zero and, when
needed, eye one. Each format call refreshes the size-specific scene pick,
orders the pair, and scans target records for the first matching texture's
format. The hypothesis is that combining these operations preserves the same
current result with less repeated work.

Confirm/refute before installing: compare the combined function against the
exact old call sequence across pair changes, first-bind order, failed refresh,
size switches, same-texture aliases and invalid formats. Compare scene-pick
state as well as returned eye. The old membership prefilter must precede any
refresh; replacing it with the view-based lookup would change behavior.
Preserve the mesh cap, its diagnostic probes, consumed-eye guard and all later
admission decisions. An outer-clock benchmark over repeated calls must show a
benefit; the existing sampled stage envelopes include measurement overhead and
are not a prediction of recoverable milliseconds. Do not introduce stale
scene-result caching or move timers to make the reported stage smaller.

The independent fast-path audit rejects broader micro-optimizations. Preserving
all admission counts while deriving redundant counters later saves only 0.225
ns/call in its randomized workload; caching the five-shader coverage tag saves
0.262 ns/call before accounting for tag updates. At this flight's 23,249 hook
entries/frame those are roughly 0.005-0.006 ms each. WARP GetType cost is
likewise about 0.012 ms/frame at the supported-call rate, insufficient reason
to remove the context guard. These are bounded synthetic results, not flight
savings.

Clock calibration measures 16.748 ns per QPC call, versus sampled Fast and
Scene spans of 24.41 and 38.61 ns. It cannot be subtracted directly from the
stage samples, but confirms that their projected 0.567/0.403 ms envelopes
include material measurement overhead. Preserve the full 41,848,960-entry
denominator, rejection priority, all counters and the destructor's final
timestamp. Benchmark sources, compiler settings and results are in
build/eligibility-fast-audit-fedd340/report.md. The combined live scene lookup
remains the sole production candidate.

Implemented depthProbeSceneTextureEye and changed only the mesh motion caller.
The old immutable depth-metadata cache remains; no new scene-result cache,
static predicate, visibility skip or change to the 512-record cap is added. The
other users of depthProbeSceneDepthFormat retain their existing API. Test-only
counters compile out of production. Independent review found no equivalence
issue after the benchmark checksum was corrected (XOR of equal A/B results had
cancelled).

The production-include depth rig passes 156 checks, covering the literal old
sequence against identical restored pair/cache/scan state. The mesh WARP rig
passes 2,506 checks, including live selection, consumed-eye/history behavior
and bounded cap probes. Observed target positions 2/3 reduce format-table
visits from ten to seven per alternating-eye pair, and refresh/order calls from
three to two. The depth table remains bounded to 32 targets.

The uninstrumented outer-clock benchmark warms up, alternates variant order
over nine trials, and uses volatile dispatch plus a non-cancelling checksum.
Initial medians at positions 2/3 are 4.69 ns/call old and 3.73 ns/call combined
(eight of nine paired wins). At synthetic positions 30/31 they are 16.10/10.43
ns (nine wins; visits 94 to 63). These are lookup-only synthetic measurements,
not game frame-time savings; the observed-position difference is less than one
nanosecond per query. It does not justify another dedicated headset comparison
or a claim of material progress toward 90 FPS.

The absolute-path full build exited zero; its 67 pooled rigs and three quiet
gates passed. Log: build/mesh-eligibility-build.log. A post-build benchmark
again measures 4.69/3.73 ns at positions 2/3, now nine of nine paired wins; the
synthetic late-table case measures 14.32/11.13 ns, also nine wins. Its full
ranges and checksums are in build/mesh-eligibility-benchmark.txt.

User steering: investigate whether planet-fixed scenery has an authoritative
classification, rather than discovering immobility repeatedly. That would be a
better early predicate if present. Planet/reference-frame membership alone does
not establish immobility: ships, drones and NPCs can share that frame. Start
with known instance fields and labeled static/movable examples; a correlated
bit is only a candidate. Confirm its meaning through shader use or the code
writing the instance buffer. If no such property reaches the GPU, the required
hook would move upstream into Elite's object/draw construction.

Field audit: the known t33 fields are bone base at byte 0 and raw rigid pose at
bytes 4..27. Both scenery and moving objects use that flattened record; the
parent relationship is not recoverable from its pose alone. The pose at
288..319 was previously measured as a duplicate of current pose, not previous
pose or a static tag. The second word of INSTANCEANDMODELDATAINDEX is retained
but its semantics are unknown; the transcribed FSS shader only consumes the
first word as its pool index. The compact discriminator words at t33 offsets 28
and 320 are used in mesh compatibility keys, but neither has established
static/parent semantics. A wider stable-byte signature proved non-unique.

Ruled out as static classifications: boneBase==0, the five rigid shader
families, immutable vertex/index buffers and stencil bit 0x10. Those also occur
on movable geometry. EDVR flag 2048 is our correction result, not a
classification supplied by Elite.

Next bounded discriminator: join retained building-owned coverage with its draw
snapshot and instance data, then compare the second instance word and the two
discriminator fields against independently moving records. Reject any candidate
value/mask also present on movable examples, including parked ships where
known. A surviving correlation requires tracing the CPU writer to its source
object/draw-list property; it does not authorize static exclusion by itself.
The retained 174658 data can start this search, but most building pixels lack
exact ownership, so label confidence and unknowns must be explicit.
Rotating-station controls, parent motion and animation remain required before
generalizing beyond settlements.

Installed and verified in Frontier through tools/install_edvr.py: dry-run,
apply, then verify-only. Version v0.17.0-45-gfedd340-dirty, graphics stamp
6AAE7FD3 (2026-09-19 12:28:03 UTC). The native pair and loader match the
archive build/mesh-eligibility-frontier-fedd340. Graphics SHA-256 begins
7eeafbec23282c01; runtime begins 6b1868b39ab8a1ea. Full hashes, symbols, build
log, source patch and receipt are retained there. Receipt backup ends
pre-mesh-eligibility-fedd340-20260919-063211.bak.

No INI was installed or edited. Before/after SHA-256 is identical:
6d6208a8af38e24fa9deb24ff3a066c69a516b4e48818ff097e4ac7329914b6f. Static
surfaces remain off, eye trigger manual and temporal AA DLSS. No game
frame-time gain is established, and no dedicated repeat flight is requested for
this small equivalent lookup change.

### 2026-09-19 — Object classification: metadata collisions and CPU upload provenance

This pass is offline evidence work. It changes no rendering, motion admission,
game configuration or installed payload. The combined scene/eye build above
remains installed in Frontier. The hypothesis was that a cheap instance/model
field distinguishes fixed settlement scenery from independently moving
geometry, before expensive per-object motion work. The discriminators are exact
metadata values on labeled building surfaces, the same values on rotating
controls, shader use, and the CPU code supplying the resource.

Reproducible local evidence is in
`build/object-classification-cd86f1a/classification.json`, generated by
`classify_metadata.py`. The report retains input paths, byte counts and SHA-256
hashes; its analysis self-test passes 14 checks. Controls reject duplicate
compatibility keys, missing GPU matches, invalid records and
translation-only/unchanged quaternion cases. Pool-slot equality is not required
or treated as persistent identity. These are investigation artifacts under
ignored build/, not a new supported tool or runtime classifier.

**Building labels and counterexamples.** Starting with the previously inspected
building ROIs in eye_174658, use frame 12693 path-4 pixels with nonzero valid
MeshCoverage and exact equality between coverage depth and SceneZ. This yields
17 building-owned records covering 333 pixels. Their compact Mesh keys directly
retain the source t33 DWORDs at byte offsets 28 and 320; every labeled record
has `(788063, 0)`. This direct extraction does not require a join to a later
rich snapshot. It samples a small recoverable subset, not every building or a
semantic label supplied by Elite.

The station eye_192513 and eye_195254 pairs supply 23 and 19 rotating controls
respectively: valid rigid records, a unique complete compatibility key in each
retained frame, the original GPU previous-match flag, and changed raw
quaternion bytes. Twelve of these 42 controls have the exact building pair
`(788063, 0)`; all 42 have word320=0. These are conservative retained/GPU
motion matches in known station scenes, not proof of persistent engine object
identity.

Ruled out: interpreting word28, word320, their pair, or a mask of that
identical pair as a standalone immobility predicate for these buildings,
because the same values occur on rotating controls. This does not rule out a
property meaning fixed relative to a parent; station walls may be fixed to a
rotating parent. Such a property still requires the parent's transform and
motion history. Nor does it rule out a classifier combining additional
independently verified fields. Merely belonging to a planet reference frame
still includes movable objects.

**Shader meaning.** All five retained rigid shader disassemblies declare only
`dcl_input v0.x` for INSTANCEANDMODELDATAINDEX; the second DWORD is unused by
these vertex shaders. This constrains GPU use, not possible CPU meaning. In
EB5234DB6ADB491D, the byte320 load feeds output o0.x, whose declared semantic
is `__USER_MATERIALMODULATION_DATAID`. That is evidence of material-related
use, not a static flag. Byte28 is not consumed by the explicit rigid pose
loads. Audit files are in `build/obj/static_surface_vs_audit/`; the report
includes their hashes and relevant instruction lines.

**Alignment limit.** The rich drawstate_174658 snapshot contains frame 12695,
two frames after the building labels. It retains 4,096 draws and reports 27,768
dropped. Its six source-buffer snapshots were copied at draw 0 or 36, so a
single retained source is not automatically valid after later writes. Both
whole stride-8 instance snapshots contain 192,512 pairs and 1,125 distinct
second-word values. Those are whole-buffer statistics, not active draw counts
or building labels. Do not attach these second words or the full model payload
to frame-12693 building records by equal pool slot or start instance. Elite can
repack them, and the required same-frame/write-generation association is
missing.

**CPU upload route.** The retained 5efb139 ETW sample examples expose Elite
callers of EDVR's Map and Unmap hooks. Read-only inspection of the installed
executable is anchored by the ETW image event: PE timestamp 1788384820 and
image size 104,894,464 both match. Current executable SHA-256 is
`e6be8bbe04e6a7ae226d4318945af7f367de13dc5a007a261964d9ba8144e988`. No
historical executable hash was recorded, so timestamp/size agreement is the
available version check. `trace-module.json`, `executable.json` and bounded
disassemblies are retained beside the comparison report.

One sampled route is map return RVA 0x50f0e5 through 0x51e1d7 and 0x51b675,
then unmap return 0x523207 through 0x51b69a. The helper at RVA 0x51b640
multiplies two size/count arguments, maps, copies from the caller's CPU source
pointer through 0x4899f50, then unmaps. The copy routine contains
size-dispatched byte/vector copying. The calls into the context use Map/Unmap
vtable offsets 0x70/0x78, consistent with the hook stacks. This establishes an
upload path for already prepared data. It does not establish that this path
uploads t33 or INSTANCEANDMODELDATAINDEX; the ETW stacks contain no D3D
resource association. Higher callers also remain unnamed. Grouped stack
examples establish existence only: their group counts must not be presented as
caller frequency, draw count or an exhaustive distribution.

The next bounded probe must close both missing associations together. During an
explicitly armed capture window, record the observed bound t33/instance
resource identity, descriptor, write generation, context and frame, plus
bounded game-module caller RVAs for its upload. Include
matched/not-observed/overflow counters so no trace cannot masquerade as a
successful match. Pair visible record labels and source payloads from the same
frame and write generation; never bridge pool slots across frames. Follow the
matched CPU source's construction only after the resource association is
proven. Capturing Unmap alone identifies upload completion, not necessarily the
instruction that created the object record. Avoid continuous stack capture,
full write-combined pool scans or an unverified executable detour.

A useful classifier must then separate buildings from NPCs, drones, ships
(including parked ones) and rotating station geometry, and distinguish
fixed-to-parent from immovable-in-world. Confirm its source meaning before
testing exclusion, retain camera/parent motion, and measure the cost of
reaching the property against the work saved. The current evidence justifies
targeted provenance work; it does not justify enabling a static filter or
promising a frame-time gain. No headset rerun is requested for this offline
pass.

### 2026-09-19 - Bounded classification capture

The offline audit above established two missing associations: which CPU upload
callers supply the actual t33/instance buffers used by a building draw, and
which source bytes belong to the same frame as the visible pixel label. The
diagnostic addresses both in one explicit Insert capture. It does not classify
static objects, suppress draws, change motion admission, or remove
camera/parent motion.

The accepted mesh-motion path nominates its actual t33 and stride-8 instance
buffers. Three discovery frames allow alternating upload buffers to be seen
before one later left-eye mesh frame is selected. Each admitted draw retains
its Mesh output-record range, original instance byte range, shader hash,
compatibility key and source resource/version references. Source buffers are
held for the capture, preventing address reuse from inventing identity. GPU
copies occur at the draw and are reused only for the same resource, observed
write generation and frame. Rewrites produce a new version; no full
write-combined CPU pool scan is performed.

Map/Unmap, UpdateSubresource, CopyResource, CopySubresourceRegion and
CopyStructureCount hooks record bounded provenance only while the capture is
armed. A failed or read-only map cannot become an upload. A writable map needs
its matching completion; partial writes preserve their range, and copies
preserve source association separately from CPU writes. Unknown command-list
writes invalidate prior attribution. Foreign-context writes conservatively make
CPU provenance unavailable. UAV/stream-output-capable resources, pre-nomination
versions, missing completions and saturated tables remain explicitly uncertain.
Generations continue to advance when the event table fills, so exhaustion
cannot reuse stale source bytes under an old generation.

The temporal consumer stages the selected frame's current Mesh records,
MeshCoverage, SceneZ and colour after mesh history matching. It records both
the internal mesh frame and the temporal scene frame. These are separate files
from the original trigger-frame eye dump, which keeps its existing timing. The
delayed object-ledger writer emits `classification_<stamp>.json` and `.bin`
beside the ledger in the pool capture directory, including a report for an
empty or incomplete run. Arming and completion have separate log lines, with
actual resource, draw, write, snapshot and stack counts.

`tools/object_classification.py` validates the schema, payload extents and
source references before joining a pixel to a Mesh record, its draw, the
instance pair and its 336-byte model record. An exact coverage/SceneZ depth
match establishes current-frame raster ownership; prior motion-history matching
is a separate observation. The source key, raw pose and metadata are
cross-checked against the retained Mesh record. The result reports the second
instance word, model metadata and available upload routes, without treating
absence of motion or a buffer upload as a semantic static flag.

An observed upload route is evidence about that buffer version. A partial
update may concern another record; Map does not expose a precise dirty-byte
range. A copy caller can be a transfer layer rather than the CPU record
constructor. This probe covers the hooked owner-context paths and explicitly
invalidates known unknown/foreign writes; it does not claim an exhaustive trace
of every possible D3D11 extension or engine-side memory write. The next offline
step is to follow a verified resource's caller chain into the code preparing
its payload, with the executable timestamp/image size from the report.

The capture applies to the current Quest 3 / VirtualDesktopXR / RTX 5090, DLSS
K settlement setup above, including the existing 512-record-per-eye motion cap.
Captured records are that admitted subset, not a scene census. Fixed probe
limits are 64 held resources, 512 upload events, 128 stack samples and 512 draw
rows, with bounded source and image readbacks; reported declines stay
unavailable. This is diagnostic capture overhead, not a performance
optimization or a frame-time benchmark. No configuration key is added or
changed.

Validation passed: 49 standalone WARP probe checks, reader self-tests and both
GPU-generated fixtures, 2,559 production mesh-motion checks, all 68 pooled plus
three quiet build jobs, and the 251-key config contract. The production fixture
joins 1,444 exact-depth pixels to its current source record and upload
generations. The standalone fixture also verifies current-frame ownership
without prior motion-history matching. A test-isolation failure was corrected
before the final green build; no rendering behavior was changed to satisfy it.

Final review covers predication save/restore, observed copy-source upload
chains, zero-length writes, failed snapshot retry after matching Unmap, and
unavailable provenance through an unobserved copy source. The reader refuses
ownership claims when binary publication failed. These cases have explicit
unavailable outcomes rather than silently reporting a valid static object.

Frontier dry-run, installation and verify-only passed. Final graphics build
v0.17.0-47-g60a5d38-dirty has stamp 6AAE9DB8 (2026-09-19 14:35:36 UTC), SHA-256
6bc13038fa85a279e4e7710ebedd877e1dde37452def579ec874ee95533130d6. Its payloads,
symbols, build output, source patch, receipts and manifest are in
build/object-classification-frontier-60a5d38. The live INI was preserved byte
for byte (SHA-256
1abd3fea466356c1348154193e6d088f1bc8bbda6b773d887a23d41a47ee91dd); DLSS, static
surfaces off and manual Insert capture remain set. Validate this flight with
`tools/edvr_log.py --target frontier --expect-build 60a5d38 --version` and
check the 6AAE9DB8 stamp; an earlier same-version trial linked at 14:31:44 UTC
was replaced before handoff.

The requested next flight is the same landed cockpit scene: let the scene
settle, press Insert once, remain in the game for at least 30 seconds, then
report completion. A flicker is not required; this capture investigates object
identity and upload provenance. Station/on-foot and movable/parked controls
remain required before any static-exclusion behavior can be justified.

### 2026-09-19 - Flight 085213: visible records reach real upload callers

The Frontier log edvr_gfx_20260919_084920.log verifies the intended
v0.17.0-47-g60a5d38-dirty build, graphics stamp 6AAE9DB8 and 14:35:36 UTC link
time. Insert armed capture 085213 at 08:52:13.213; the classification report
was written at 08:52:16.779. It sealed mesh and scene frame 16584 after
discovery began at 16581. This flight's actual input is 1996x2121 per eye, with
DLSS K output 3072x3264 on VirtualDesktopXR / Meta Quest 3 / 90 Hz. The
previous 2481x2121 input does not describe this capture, and its timings cannot
serve as an unchanged-resolution comparison.

Artifacts are retained under build/object-classification-flight-085213: the
original classification JSON/BIN, validated per-record report, spatial
statistics, same-frame colour and ownership images, and bounded caller
disassembly. The capture has 122 admitted draws / 512 records, six observed
upload events, 12 stack samples and three buffer snapshots. All payloads are
available. Resource/write/draw/stack/snapshot limits, readback failures,
unmatched writes, unobserved source versions and foreign writes are all zero.
Six rejected early stage attempts are expected during discovery; the later
frame sealed successfully.

The source pool is one 5,160,960-byte dynamic structured buffer with 336-byte
stride (15,360 capacity); the instance stream is one 1,540,096-byte dynamic
vertex buffer (192,512 eight-byte pairs). Both were observed in completed
WRITE_DISCARD Map/Unmap pairs on frames 16582, 16583 and 16584. The selected
draw snapshots refer to generation three. This proves the retained current
source bytes and their buffer upload routes; it does not identify the
particular CPU instruction that filled any one object record. WRITE_DISCARD
still does not expose a precise dirty-byte extent.

The reader finds 112,953 covered pixels, of which 49,704 exactly match SceneZ
and join to 27 Mesh records. There are 19 metadata groups. The entire
4,233,516-pixel eye and all source-buffer bytes were retained, but only the
existing 512-record admitted subset has these draw/coverage joins. Neither the
buffer capacity nor pixel counts are unique-object counts or a removable-draw
estimate.

The same-frame ownership overlay puts five records / 49,214 pixels on cockpit
and player foreground geometry, and 22 records / 490 pixels sparsely on distant
settlement facades and fixtures. The latter use 21 current-frame model slots;
those slots are not persistent identities. The foreground group has one
valid/history-matched rigid record and four invalid rigid records on the player
geometry. All 22 settlement-positioned records are valid rigid, but only two
are history matched. Across the complete staged 512-record table, 504 are valid
and only 24 are history matched. Current source ownership remains valid
independently of prior motion-history matching.

Those spatial groups carry t33 word28 values 1631 and 788063 respectively, with
word320 zero throughout the visible subset. Their location in this frame does
not establish semantic meanings for those values. The earlier rotating-station
counterexamples still rule out interpreting 788063/0 as immovable in world
space.

All 122 draws refer to the same pool resource0/generation3/snapshot0 and
instance resource1/generation3/snapshot1. The reader checks all 512 draw/source
joins, including key, original IA offset, pool slot, raw pose and metadata; the
report's missing list is empty. The visualization recipe
render_classification_085213.py was rerun successfully. The 512-record motion
admission cap is saturated even though the separate diagnostic tables did not
overflow.

The model-pool Map route is 0x50f0e5 -> 0x51e11a -> 0x4c821b3 -> 0x4c80c2d ->
0x6bf7a7. The instance-stream route instead reaches 0x4c80e56 from the same
0x51e11a helper, then 0x6bf7a7. Completion routes are 0x523207 -> 0x4c83ed8 for
the pool and 0x523207 -> 0x4c83ff9 for instances, both continuing through
0x6c0667 and 0x27bbe53. Each route repeats across the three observed upload
generations. These are stack return RVAs, not invented function names or entry
points.

Ruled out for this captured upload: assuming the earlier ETW sample's 0x51b640
map/copy/unmap helper is the source-preparation route for these two buffers. It
is absent from their directly associated stacks; the resource-linked paths
above now replace that guess. This does not rule out that helper elsewhere in
Elite.

The next offline step follows the actual buffer owners and mapped destination
pointers into record construction. PE exception-table entries can describe
chained unwind fragments rather than complete functions: the fragment
containing 0x4c821b3 begins at 0x4c81f0f, while its direct caller targets
0x4c81e80. Resolve those chains before interpreting entry arguments or naming a
containing function. No static filter, executable detour or rendering change is
justified by upload provenance alone.

Bounded disassembly resolves the paired preparation/completion virtual entries
to 0x4c80b20 and 0x4c83e70. Dispatchers at 0x6bf770 and 0x6c0630 call slots
+0x30 and +0x38 on the same kind of list entry; one read-only table at RVA
0x5a6fc10 contains this exact pair. No usable RTTI class name was recovered:
the preceding table word points into executable code, not a valid MSVC
complete-object locator. Adjacent function pointers alone must not be assumed
to belong to the same class.

The model-pool preparation helper at 0x4c81e80 walks an outer count/array at
owner +0x198/+0x1a0 and nested count/array pairs at +0x140/+0x148. Its loop
writes cumulative offsets to each leaf's +0xc0 and advances the running total
by that leaf's +0xc8. This gives a concrete CPU-side grouping/allocation chain
to follow from the verified pool owner. It does not yet establish entity
identity, planet attachment, static flags, or the function that fills each
336-byte record. That object/record-construction step remains the next offline
discriminant; this flight does not justify another render change or a claimed
performance gain.

The preparation loop calls 0x4c81be0 for each non-empty leaf payload. This is
the strongest next record-preparation candidate; a separate owner collection at
+0x60/+0x68 calls 0x4c822e0 and may prepare the instance stream. Their actual
payload writes and upstream object semantics remain unverified. The vtable has
direct RIP-relative references at 0x4c7e50b and 0x4c7e9e4, additional
owner-construction leads if needed. Bounded details and the exact field
assignments are in caller-findings.md beside the disassembly, not inferred from
a recovered class name.

The current executable matches the capture's PE timestamp 1788384820 and image
size 104,894,464; file size is 103,469,056 and SHA-256 is
e6be8bbe04e6a7ae226d4318945af7f367de13dc5a007a261964d9ba8144e988, identical to
the earlier offline binary. This turn changes documentation only. The installed
test build and settings remain unchanged; no additional flight is needed for
the next bounded offline step.

### 2026-09-19 - Offline payload preparation and CPU source blocks

The next pass uses the same verified executable and flight 085213. No game code
or installed payload is changed. New evidence is retained beside that capture
under mapping-*, payload-* and instance-* prefixes. The original upload
association remains the anchor; a matching 336-byte layout alone is not proof
that an arbitrary CPU producer made the captured settlement records.

Ruled out: 0x4c81be0 as the constructor of the model fields. It is a copier of
already-prepared CPU blocks. For a leaf batch and source descriptor, it
computes destination = mappedPool + stride * (leaf[+0xc0] + entry[+0x38]),
copies stride * entry[+0x3c] bytes from pointer entry[+8], and traverses one
0x48-byte descriptor list plus seven 0x50-byte descriptor lists. Aligned copies
use explicit 16/64-byte loads/stores; the fallback calls 0x4899f50. It does not
calculate the captured raw pose or synthesize t33 words28/320.

This also corrects the earlier archive note's units: leaf +0xc0/+0xc8
accumulate element offsets/counts, not bytes. The later multiplication by
resource stride establishes that distinction. The pool allocation aligns the
element count to 0x400; the captured 15,360-element, 336-byte-stride capacity
agrees with that representation.

Mapping helper 0x50f000 stores D3D Map's pData at result+0x28, RowPitch at +0
and DepthPitch at +4 on its success path at 0x50f213. Wrapper 0x51e0c0 retains
the resource wrapper in result+0x18; wrapper+0x140 holds the actual D3D
resource. Therefore pool-owner +0x178/+0x180 are mapped pData and its
duplicate, and +0x168 is a resource wrapper, not a D3D context. The
instance-owner equivalents are +0x128/+0x130 and +0x118, relative to the upload
method's owner base. Upload-state flags and these mapping fields are not
evidence of object mobility.

Ruled out: 0x4c822e0 as the instruction that constructs IA's two 32-bit words.
Its verified caller passes the mapped ID pointer, cumulative element offset and
count. The function stores them into entry +0xf0/+0xf8/+0xfc, clears the
per-entry counter at +0x100, and registers/enqueues the entry. It never writes
through the mapped ID pointer. The word emitter must be downstream of this
bookkeeping.

A common 0x150-byte initializer at 0x4c835d0 writes word28 = 0x65f (1631),
zeroes the position and optional tail fields, and initializes packed
orientation/scale. One directly linked CPU builder, 0x42b4130, invokes it,
writes a supplied pose, and replaces only mask 0xfc0000 of word28 with a
selected table index shifted by 18. Numerically, 0xc065f (788063) is the same
default plus index three in that field. That builder copies optional caller
data into the +0x140 tail, explaining how word320 can remain zero or receive
caller data on this path. These instruction-level facts explain the encoding on
that producer path; the table's semantic meaning and its connection to the
captured settlement source blocks still require proof. The earlier
rotating-station counterexamples continue to rule out either observed metadata
pair as an immovable-in-world test.

That producer retains up to eight records per CPU node, with count at
node+0x18, payloads at node+0x20+i*0x150 and separate 64-bit values at
node+0xaa0+i*8. These values are bitmasks on the verified caller path, not
established pointers (corrected in the later append-path investigation). Its
owner is builder[+0x20]; a compound-key lookup at owner+0x260 selects the list.
The immediate caller's primary function is 0x42b4420. These are concrete
storage and calling relationships, not entity or attachment semantics. In
particular, no pointer-identity chain yet connects node+0x20 to the upload
copier's entry[+8]. A same-sized record with the same default fields does not
close that gap.

The upload owner's constructor is primary 0x4c7e4d0. Its embedded labels
identify AtlasModel2, gfModelGPUModelData and
frAtlasModelRenderer_InstanceIndexRemapBuffer. A helper registers three named
streams: gfrAtlasModelBaseStreamData (0x150 bytes), gfrAnimationData (0x30),
and gfrPreviousAnimationData (0x30). These labels come from code references to
embedded strings, not recovered RTTI. They identify the rendering subsystem and
layouts; they do not identify buildings or planet-fixed objects.

There is an important eight-byte owner-base distinction: the constructor
returns complete object C, but the upload dispatch table at 0x5a6fc10 is
installed at C+8. The upload methods receive S=C+8 and do not adjust it. The
instance count/list at S+0x60/S+0x68 and mapping fields at S+0x100 onward are
therefore secondary-interface offsets. The constructor stores its supplied
owner at C+0x48; calling that an ownership parent does not establish a
transform/entity parent. The earlier reference at 0x4c7e9e4 belongs to
destructor primary 0x4c7e9c0, not a second constructor.

Ruled out: the constructor's registered callback as the missing IA pair
emitter. Thunk 0x4c7bd70 jumps to 0x4c82a40, which filters linked records and
submits integer ranges through 0x51baf0. It neither follows the prepared
entry's mapped-pointer/range fields nor writes through instance-buffer pData.
Callback registration alone does not join the CPU model producer to captured
source records.

The final bounded descriptor check also leaves the pointer join open. Primary
functions 0x4c7b010 and 0x4c7b3c0 contain the earlier scan's 0xc0/0xc8/0xd0
displacements as stack arguments, not object fields. Their grouped-submission
vectors mix 0x60-byte and 0x50-byte entries and do not expose the upload leaf's
+0xd0 link. Immediate helper 0x4c7a1d0 writes similarly placed +8/+0x38/+0x3c
fields in a 0x50-byte record, but its source is an input-array element pointer
and the latter fields have different established inputs. It supplies no
node+0x20-to-upload-source pointer chain. Matching offsets alone are therefore
ruled out as proof that these candidates construct the captured source
descriptors.

Flight 085213 contains stack return addresses and GPU buffer bytes, but no live
CPU-owner registers or source-block addresses. The bounded static pass has not
closed that missing pointer join. The next discriminant is a bounded diagnostic
at the already observed Map return 0x4c821b3. Recover the preserved pool-owner
register by unwinding, validate its wrapped native D3D resource against the
hook's mapped resource, and then retain bounded leaf descriptors and
source-record bytes for comparison with the same upload generation. The mapped
pData field may still refer to the previous mapping while the hook is running;
native resource identity, not that field, must validate the owner.
Build/callsite checks, guarded reads, bounds, pointer lifetime and explicit
failure reporting must be validated offline before requesting a flight. This is
an instrument design, not an implemented classifier or evidence of a
performance gain.

### 2026-09-19 - Bounded CPU source-owner capture

Sean approved implementing the source-owner instrument and installing test
builds to Frontier. The hypothesis is that the preserved RBX at the already
captured Map return 0x4c821b3 identifies the nested AtlasModel2 pool owner,
whose pre-existing upload descriptors identify the CPU records later copied
into the nominated GPU pool. The earlier disassembly confirms RBX is
initialized from the function's owner argument, is preserved across the Map
helper call, and that cumulative record ranges are assigned before that call.
This hypothesis concerns source provenance only, not planet attachment or
static classification.

The next capture must discriminate unsupported executable or callsite, failed
register recovery, wrong wrapped D3D resource, changed layout, unreadable
memory, exhausted bounds, overlapping destination ranges, changed source bytes,
and missing or changed upload generations. Exact byte equality with a unique
source descriptor and the same completed GPU upload generation is the
confirmation signal. It must not accept an identical record from another
generation, or interpret a partial traversal as proof that no competing
descriptor exists. CPU addresses and ownership fields remain capture-local
evidence, not persistent game-object IDs.

The instrument is restricted to the existing manual eye-run window and
nominated model-pool write Maps. The inactive hook retains its existing gate.
It validates executable identity and callsite bytes before chasing the owner,
validates the wrapped native resource against the actual Map resource, and
checks the known 336-byte stride. It does not read the owner's stale
mapped-pData field as validation, because the game assigns the new Map result
only after our hook returns. Source metadata and bytes are retained during the
hook; comparison, formatting and file output remain offline or on the existing
capture drain.

The known flight contains three uploads of a 5,160,960-byte pool before the
selected frame is sealed. The planned 32 MiB total CPU evidence budget and 8
MiB per attempt avoid spending a smaller budget entirely on the first two
generations. Traversals and unwind depth also have fixed caps and explicit
decline reporting. This is diagnostic work only: motion calculation, draw
admission, DLSS history and static-surfaces settings are unchanged.

Implementation extends the capture to edvr_object_classification_v2 while
preserving v1 reader support. ObjectSourceOwnerProbe unwinds at most 32 frames,
stops at the target return before unwinding its caller, and verifies the
instruction bytes around that callsite. It captures at most 16 attempts, with
64 groups, 512 leaves and 4,096 descriptors per attempt. The eight source lists
use the verified 0x48/0x50-byte layouts. Raw descriptors and bounded source
payloads are appended to the existing binary; known ownership pointers are
retained without guessing their semantic meaning. An empty leaf is skipped as
in Elite's copier. Rechecked container/leaf/list fields and descriptor bytes
detect observed metadata changes; this does not claim an atomic snapshot of
other threads' memory.

Review corrected three ways the probe could waste a flight: exhausting the byte
budget before the selected generation, reporting a complete scan when an exact
cap skipped a later list, and retaining failed-read allocations without
charging them to a budget. Failed source reads now consume the attempted-byte
budget and release their storage. A partial scan cannot prove unique source
ownership. The reader requires a direct completed Map on the selected pool
resource/generation, rejects foreign writes and ambiguous descriptors, and
compares all 336 bytes. Copy routes without validated slot translation remain
unmatched. Counters distinguish zero attempted Maps, unsupported
binary/callsite, unwind/resource mismatch, read faults, metadata changes, caps
and completed captures.

Focused validation passed before the full build: 12,454 C++ rig checks,
including a real x64 assembly caller with a known RBX value and normal unwind
metadata; guarded invalid-owner reads, wrong native resource, empty leaf,
invalid range, exact descriptor-cap boundary and attempt cap; reader self-tests
and both generated fixtures. The source fixture sends known CPU data through a
real D3D buffer upload, stages a selected visible record, writes v2 evidence
and proves event0/attempt0/descriptor0/leaf0/group0 by exact CPU/GPU byte
equality. Fixture-only identity and graph injection do not alter the production
guard. The retained v1 flight 085213 still yields 27 visible records / 19
groups and explicitly reports CPU source-owner data unavailable for all 27
records.

The full absolute-path build passed all 68 pooled jobs, three quiet timing jobs
and the 251-key config contract. Launch it with EDVR_JOBS=2 in the environment:
the initial --jobs argument invocation compiled both DLLs but failed when SHIFT
changed the batch file's %0 before the test runner consumed %~f0. The corrected
invocation passed every gate; no unrelated launcher change is included. Both
logs are retained in the archive.

Frontier dry-run, installation and verify-only passed. The game executable
still has the exact verified SHA-256
e6be8bbe04e6a7ae226d4318945af7f367de13dc5a007a261964d9ba8144e988. Installed
graphics v0.17.0-50-g104763d-dirty has stamp 6AAEB01E (2026-09-19 15:54:06
UTC), SHA-256 396a81a1e1539168d0ba115fd418deb5a6d1880d8a73418ef2f02031badd98a1.
Payloads, symbols, manifest, source patch, install receipt, build output and
before/after INIs are archived under build/source-owner-frontier-104763d. The
user's current INI was preserved byte for byte, SHA-256
aa27c06406576aadd3eda1a8dd0c0cb26a813ead380c9273a175eb459f5ccf24; it had
changed since the previous flight archive, so this installation preserves the
current file rather than restoring that older copy. DLSS, manual Insert and
static surfaces off were verified before installation.

Next flight: use the same landed cockpit settlement view, let it settle, press
Insert once, and remain in game for at least 30 seconds before exiting
normally. No Pause press or flicker reproduction is required for this ownership
capture. Check it with tools/edvr_log.py --target frontier --expect-build
104763d --version and the 6AAEB01E graphics stamp; the earlier failed-launch
build carried the same version but is not the installed payload. Require the
new CPU source-owner completion line and v2 report. An empty, failed or partial
capture is not evidence that the buildings have no CPU owner or are static.

### 2026-09-19 - Flight 100737: CPU sources joined to visible settlement records

The newest Frontier log is edvr_gfx_20260919_100542.log. The sanctioned reader
verifies v0.17.0-50-g104763d-dirty, graphics stamp 6AAEB01E, linked 2026-09-19
15:54:06 UTC, matching the installed archive rather than the earlier
failed-launch build. Insert at 10:07:37.469 armed capture 100737; publication
completed at 10:07:41.274. The v2 JSON/binary pair is copied and hash-verified
under build/source-owner-flight-100737, with the log evidence, manifest, reader
report and reproducible ownership visualization. The source files live under
edvr_logs/pool, not the eye-image directory.

All three nominated model-pool Maps yielded a complete owner capture:
maps/attempts 3/3, complete/partial 3/0, resource matches 3.
Identity/opcode/unwind rejects, read faults, observed metadata changes,
descriptor overflow and CPU-byte declines were all zero. The paired report
sealed mesh/scene frame 11738, with 133 draws, six resources, six write events,
three buffer snapshots and 12 stacks. GPU source provenance also has no
unobserved, unmatched or foreign writes. This confirms the instrument reached
the verified caller and recovered the associated upload-owner chain; it is not
merely an empty or dormant diagnostic.

The unchanged reader validates 512 staged mesh records and finds 27 records /
18 metadata groups with 64,583 exact-depth pixels out of 133,425
motion-coverage pixels in the 1996x2121 input eye. All 27 visible records have
a unique, complete, direct same-generation Map source and an exact 336-byte
CPU/GPU match. Scene colour, SceneZ and MeshCoverage belong to the same
selected frame. The overlay places five records (1, 3, 6, 8, 9) / 64,104 pixels
on foreground player/cockpit geometry, and 22 records / 479 pixels sparsely on
distant settlement facades and fixtures. These are spatial observations within
the saturated 512-record admission subset, not a count of the settlement's
meshes or a semantic classification.

Ruled out: using the recovered group/leaf owner alone as a planet-fixed or
static-object discriminator. All 22 settlement-facing records and four
player/body foreground records share CPU group3/leaf3 in this capture; the
remaining cockpit record6 uses group5/leaf5. The engine's upload grouping
therefore crosses the foreground/settlement distinction. Its child descriptors
and source blocks are the concrete evidence to follow next; the enclosing group
number is not an entity parent or an immobility flag.

An independent full-table analysis extends the visible reader result: all 512
staged records across all 133 draws have unique direct generation-3 descriptor
coverage and exact 336-byte CPU/GPU equality, using 96 descriptors. There are
no missing, overlapping, partial, failed or byte-mismatched joins. The 22
settlement-facing records use 21 pool slots and 17 descriptors/source blocks;
all 22 have valid rigid-motion inputs, while only three have matched motion
history. Valid rigid inputs and matched history do not establish immobility.

| Generation / frame | Groups / leaves | Descriptors | CPU source records | Source bytes |
|---|---|---|---|---|
| 1 / 11736 | 11 / 11 | 2262 | 12451 | 4183536 |
| 2 / 11737 | 11 / 11 | 2241 | 12322 | 4140192 |
| 3 / 11738 | 11 / 11 | 2250 | 12403 | 4167408 |

These CPU record totals describe the captured upload, not unique scene meshes
or visible objects. Declared/scanned descriptor counts agree in all three
generations. Within each generation both source and destination intervals are
disjoint. Owner, group and leaf addresses are reused across the three Maps, and
exact source intervals recur with both equal and changed bytes. For example,
generations 1 and 3 reuse 535 exact source intervals: 190 are byte-identical
and 345 have changed bytes. This is capture-local address/byte evidence, not
stable object identity. Only generation 3 has the staged draw and visibility
associations.

The reproducible archive analysis is analyze_source_owner.py, with
source-owner-analysis.json and source-owner-analysis.md. Input hashes match the
retained manifest, and the analysis JSON is byte-identical on rerun. No
production code, reader, configuration or installation changed during this
analysis.

The retained raw descriptors narrow the missing producer edge. All 6,753
descriptors across the three Maps contain source at +0x08, source+0x1c at
+0x10, the leaf owner link at +0x20, destination-relative index/count at
+0x38/+0x3c, and stride 0x150 at +0x40. For 18 of the 19 descriptors serving
visible records, the pointer at +0x18 is source+0xa80 (eight records); the
remaining player descriptor has a one-record span. Other captured descriptors
have different spans, so this observation does not establish universal capacity
semantics.

This corroborates the layout of the earlier 0x42b4130 producer: eight records
start at node+0x20 and its separate 64-bit array starts at node+0xaa0, the same
eight-record span later. It does not prove that producer supplied these
records. No retained descriptor contains source-0x20, and the capture excludes
both that candidate node header and memory reached through the +0x18 pointer.
The exact allocation-to-descriptor pointer relationship and its initializer
remain unproven. Metadata, record size and matching storage shape cannot
substitute for that edge. Reproducible raw-layout analysis is retained as
payload-source-shapes.py, payload-source-shapes.json and payload-findings.md.

A bounded offline check of the known AtlasModel constructors and descriptor
helpers still does not identify the initializer. The exact .text search for an
immediate 0x150 store to base+0x40 found zero hits in the verified executable
(optional REX/SIB, disp8/disp32 covered). This excludes only the searched
encoding: storing a register, wider initialization or copying a prepared entry
can still populate that field. The scan and result are retained as
payload-stride-store-scan.py/json. Both archive scripts verify that --dry-run
writes nothing.

Next is an offline trace of the registration/append path feeding the lists
consumed by 0x4c81be0, using the full captured descriptor layout rather than
the literal stride store alone. That path must connect a specific source
allocation to the descriptor and expose the upstream owner before it can
support object classification. Another run of the unchanged probe would not
supply the missing header or ownership fields. No new flight or performance
claim follows from this capture, and static filtering remains off.

### 2026-09-19 - Offline upload append path and source-block slicing

The exact descriptor appender is RVA 0x4c7ef70. It selects the first 0x48-byte
vector or one of seven 0x50-byte vectors on the same leaf read by the capture
and 0x4c81be0. Its stores reproduce the retained descriptor layout: source at
+8, record-word pointer at +0x10, mask-array pointer at +0x18, leaf owner link
at +0x20, another caller-supplied pointer at +0x28, combined mask at +0x30,
destination-relative index/count at +0x38/+0x3c, and a 16-bit stride at +0x40.
That last store explains why searching for an immediate DWORD stride store
failed. The appender advances leaf+0xc8 by the appended record count. This
establishes the descriptor constructor and consumption path without assigning
object semantics to its pointers.

Its caller 0x4c837b0 processes the source records' 64-bit masks, accumulates
per-bit counts and their union, trims leading/trailing records with zero masks,
and emits one descriptor for the resulting contiguous slice. Interior zero-mask
records can remain in that slice. At 0x4c83943 it calls the appender with
source advanced by start*sourceStride, mask pointer advanced by start*8, the
similarly advanced record-word pointer, and the trimmed count. This is not
evidence of final-eye visibility, occlusion or removable draws.

One concrete input bridge, primary 0x434d7a0, traverses a dictionary and each
entry's linked list of record nodes. Its call at 0x434d8d0 supplies base
source=node+0x20, count=[node+0x18], record-word pointer=node+0x3c, source/word
stride=0x150, and mask-array base=node+0xaa0. The descriptor's +0x18 value
therefore names a mask array on this path; it is not an allocation-end or
capacity field. With slice start i, descriptor.source=node+0x20+i*0x150 and
descriptor.mask=node+0xaa0+i*8, giving a pointer difference of 0xa80-i*0x148.
Source-0x20 is the node header only when i=0. The bridge proves these layout
relationships on its path, not that every captured descriptor came through that
path.

Ruled out: treating the candidate node's trailing 64-bit values as per-object
ownership pointers. Producer 0x42b4130 stores its fifth argument at
node+0xaa0+i*8; its verified caller 0x42b4420 loads this argument from values
combined with OR at 0x42b47a7/0x42b47d2. The upload grouping also applies bit
scans and mask operations to this array. Earlier journal references to separate
pointers have been corrected. Neither those values nor their combined
descriptor mask establish planet attachment.

The producer's immediate caller has two verified callers of its own, at
0x43204f3 and 0x4321adb. Both enumerate 0x2f0-byte records from an array/count
pair at collection+0x28/+0x30 and pass the pointer at record+0x290 as the
pose-bearing argument to 0x42b4420. They check bytes at record+0x234/+0x298 and
perform mask/nibble selection before making the call. This identifies a more
specific upstream render structure, but none of those checks proves immobility,
an entity parent or persistent identity. The trace is conditional until a
captured source is linked to that producer.

The verified executable SHA-256 remains
e6be8bbe04e6a7ae226d4318945af7f367de13dc5a007a261964d9ba8144e988. Bounded
append, grouping and bridge assembly plus xref results are retained under
build/source-owner-flight-100737/payload-trace-*.asm and
payload-append-xrefs.json. The independent caller trace is in
builder-owner/trace.json and builder-owner/findings.md, generated by
owner-code-trace.py with chained unwind resolution and a verified no-write dry
run. No game code or configuration has changed.

The dictionary connection is also proven. Writer 0x42b4130 calls 0x3696fa0 with
owner+0x260 and key=producer+0x40, then appends to an eight-slot record node
and increments owner+0x2a4. A second writer, 0x434d470, constructs another
0x150-byte record and calls 0x434d120; that helper uses the same dictionary
lookup, allocator, node layout and owner counter. The later 0x434d7a0 bridge
traverses the owner's dictionary array at +0x280/+0x288. Its entries hold a
resource reference at +8, the descriptor's +0x28 value at +0x10, a leaf at
+0x18 and the node-list anchor at +0x28. Thus the allocation-to-upload route
can accept records from either writer; this does not prove which writer or
runtime owner instance supplied a particular captured record. A descriptor does
not retain that association or establish a per-record game object.

Independent capture analysis confirms there is no hidden saved-pointer route to
that owner. Descriptor +0/+0x28/+0x18 targets have no same-generation
containment in retained CPU blobs or captured structural ranges. Each +0x28
value pairs with exactly one +0 value and group/leaf/list scope, while some +0
values span multiple +0x28 values/scopes: these are capture-local opaque batch
relationships. Seven source-0x20 windows per generation happen to be covered,
but all are the last 32 bytes of an adjacent preceding 336-byte source record,
not independently retained node headers. Ruled out: interpreting incidental
coverage of source-0x20 as proof of an allocation header. The reproducible
analysis, exact ranges and no-write/deterministic checks are in
analyze_descriptor_links.py and descriptor-link-analysis.json/md.

The next evidence seam is record creation before batching. At the 0x42b4130
insertion, the complete record is at rsp+0x30, the shared owner is in rsi and
the key is r14=producer+0x40. At 0x434d470's call to 0x434d120 at 0x434d73c,
the complete record is in r8, owner in rcx, key in rdx and sidecar mask in r9.
A diagnostic must capture both paths, retain their calling/pose-object context,
and require exact 336-byte correspondence with explicit duplicate/ambiguity
outcomes. Producer/builder addresses alone are insufficient: the first path's
more specific pose-bearing object belongs to caller 0x42b4420, whose original
RCX is retained in R12 across the producer call. The second path also needs its
caller/input-transform association. Neither object identity nor a
planet-attachment flag has yet been proven at those seams. This turn added
offline evidence and a concrete diagnostic target; no new hook, test build or
installation was made.

### 2026-09-19 -- capture record writers before upload batching

Hypothesis: exact completed 336-byte producer records can connect the existing
CPU/upload evidence to a narrower caller or pose-bearing object. A unique
payload match is candidate provenance only: reused bytes, repeated calls and
shared builder addresses still cannot prove persistent identity or planetary
attachment. The discriminating evidence is the writer return RVA, owner/key,
bounded caller-object snapshots, lookup completion and the exact payload join
before the upload cutoff. Missing, partial and multiple matches remain
explicit.

The verified executable has timestamp 1788384820 and image size 104894464; its
SHA-256 is unchanged from the preceding entry. A single diagnostic hook at RVA
0x3696fa0 covers five callers with complete records available before the
original lookup: returns 0x369ce91, 0x42b42ef, 0x42b4ed6, 0x43130aa and
0x434d149. The 0x434e316 caller merges existing node lists and is counted as a
management decline. Unknown callers are counted separately. The hook requires
the exact executable identity, entry prologue and seven verified callsites;
other builds fail closed. No new static or visibility interpretation is made.

The relay forwards the original lookup exactly once and preserves its result.
The executable entry is patched only when Insert arms a capture. Afterward, an
inactive relay bypasses the C++ observer; the trampoline remains allocated for
process lifetime so outstanding calls cannot execute freed code. Preparing the
trampoline and relay before publishing the entry patch closes the first-call
forwarding race. A new capture validates the exact owned patch before reusing
it, and epochs reject completions from an earlier capture. Classification
admission closes before the writer observer drains, preventing an upload from
being admitted after writer observation stops; draining holds no classification
lock.

Writer evidence is limited to 65,536 records and 64 MiB, alongside the existing
source-owner limits. Each retained event has a dense ID, begin and completion
event numbers, exact producer bytes, key bytes and applicable bounded context.
Map samples the event cutoff before traversing source-owner memory, preventing
a later writer from becoming eligible during the diagnostic scan. The reader
requires a complete lookup before that cutoff and compares all 336 bytes to the
CPU-source-proven upload record. It reports duplicate candidates and their
opaque-context agreement without promoting them to object identity. Legacy
captures without writer evidence remain readable. The completion log exposes
hook refusal, cap/budget declines, read/unwind/context/completion failures and
management/unknown callers so a dead probe cannot resemble an empty success.

This is a diagnostic for the documented Quest 3 / VirtualDesktopXR / DLSS K
setup, not a performance change. It neither skips Elite draws nor removes EDVR
motion work; the bounded capture itself can disturb frame timing. The next
flight needs ordinary settlement geometry and does not depend on reproducing a
flicker. The isolated real-hook WARP rig passes 12,465 checks, including relay
forwarding, real caller unwind, five recipes, stale completion rejection and
the upload-cutoff race. Both generated capture fixtures pass the Python reader.
The reader also tests event numbering with interleaved begin/completion events,
duplicate candidates and incomplete/capped evidence. Archived v1 capture 085213
and v2 capture 100737 remain readable. The full absolute build passed 68 pooled
and three quiet test jobs, including the common CodeHook publication tests, and
the 251-key config contract. The initial sandbox run could not pass its
compiler environment to child rigs; the successful build ran outside that
sandbox. An intermediate full run caught a fixture whitelist error, corrected
without relaxing the production executable or caller guards.

Installed to Frontier using the sanctioned dry-run/install/verify-only flow:
v0.17.0-53-gaadac6a-dirty, graphics stamp 6AAECC1E (17:53:34 UTC), runtime
6AAECC21 (17:53:37 UTC). Graphics SHA-256 is
263c47a4f5aabbee492829beb53d2039864b93bfa6b227be24953aa0d07ed0d1; runtime is
be136f62eb286470c92d13a09d7f4b3ebece08a8477641dd782ca0a35b4da13a. The user INI
is byte-identical before/after, SHA-256
aa27c06406576aadd3eda1a8dd0c0cb26a813ead380c9273a175eb459f5ccf24; native
routing config is also unchanged. The verified game executable hash still
matches the identity used for the hook. Payloads, symbols, source-base/patch,
fixtures, build log, INIs, manifest and installer receipts are retained in
build/record-writer-frontier-aadac6a. The next flight must match this literal
version and graphics link stamp; the later source commit changes the Git
description but does not rebuild the installed binary.

### 2026-09-19 -- writer flight 120047

The sanctioned log reader verified edvr_gfx_20260919_115849.log against
v0.17.0-53-gaadac6a-dirty and graphics stamp 6AAECC1E, linked at 17:53:34 UTC.
Installer verify-only also passed. Insert armed capture 120047 at 12:00:47.134;
the entry hook installed, classification sealed mesh/scene frame 15059, and the
complete report was written at 12:00:50.993. The game exited normally around
12:01:31. This is a valid diagnostic flight, with no early-exit loss.

The capture has 122 selected draws, six resources, six write events, three
snapshots and 12 stacks. Source-owner attempts were 3/3 complete with three
resource matches and no identity/opcode/unwind rejection, read fault, metadata
change, descriptor overflow or CPU-byte decline. Writer status was finished:
44,596 observed, stored and completed calls, 26,187,360 retained bytes, and
zero record overflow, byte-budget decline, read/context/unwind/completion
failure. The 15,248 list-management calls were declined as intended; no unknown
callers appeared. These counters establish that the instrument ran and retained
the intended data; they do not by themselves establish a mesh/object join.

Runtime remains VirtualDesktopXR / Meta Quest 3 / RTX 5090 / 90 Hz, DLSS K,
input 1996x2121 and output 3072x3264 per eye. W5 reports CPU/GPU elapsed-span
medians 12.285/15.065 ms with 1,638 valid samples over 30 seconds. W4 ended at
Insert after only 6.922 seconds, and W1-W3 have much lower costs from an
earlier part of the run. These windows are not a controlled performance
comparison; the diagnostic was the purpose of this flight. The verified log
extract is build/record-writer-120047-log-summary.txt.

The archived classification pair passes the reader. All 512 admitted records
have exact CPU-source correspondence. Writer matching yields 211 unique
candidates, 293 multiple-candidate records and eight with no candidate. The 29
exact-depth-visible records cover 125,462 pixels: nine unique, 16 multiple and
four without a candidate. These are exact byte/time-qualified candidates, not
proof of causal creation or persistent game-object identity.

The current color/coverage overlay confirms two spatial groups. Twenty-four
small settlement-facing records cover 482 pixels and all have writer matches:
nine unique and 15 multiple. The five foreground cockpit/player records (1, 3,
6, 8, 9) cover 124,980 pixels and have one multiple-candidate match and four
with no candidate. The foreground geometry issue remains separate from this
classification work. Coverage is a bounded admitted subset, not the visible
fraction of the entire settlement or the number of physical objects.

Nineteen settlement-facing records are inline-writer-only. Each has exactly one
nonzero captured object address across all eligible matches; collectively they
point to eight addresses. Nine have one writer event, one has two and nine have
four. Repeated events keep the same owner, key and pose-object address. The
four-event cases differ in entry identity/bookkeeping, without changing the
captured object bytes. The other five settlement-facing records are
direct-writer-only, each with four candidates but no captured object context.
No settlement candidate set mixes the two writer paths. This is a more specific
association than the old shared upload group, but it is still capture-local
candidate evidence.

Across all retained calls, 40,732 used inline return 0x42b4ed6, 3,600 used
direct 0x43130aa and 264 used helper 0x434d149. No primary 0x42b42ef or direct
0x369ce91 calls occurred. All 211 unique staged-record matches are inline; no
helper record matched this admitted subset. The per-record analysis and
scene/coverage views are in build/record-writer-flight-120047. Its JSON is
33,145,881 bytes, SHA-256
bf7030b6fc56a58f8f8bb4e10c23f2a38882d7a537edaacbe24d0dc49ed38964; BIN is
129,355,000 bytes, SHA-256
29662d98e767f0dbfc135630b587a73f756af3cbb15d6723559e619f315df0ef. Archive and
analysis helpers support verified no-write dry runs. No new DLL or settings
change follows from these counts.

The bounded context audit confirms the inline object's origin: 0x42b445c saves
the original RCX in [rbp-0x68], the slot the new probe reads at 0x42b4ed6. Both
verified collection enumerators obtain this pointer from +0x290 in a 0x2f0-byte
record. Thus the current inline capture is the same pose/render context studied
on the primary path, not a new inferred type. All 520 inline context addresses
share one heap pointer at +0. The routine passes that field to ordinary helpers
rather than dispatching virtually through it; it supplies no class name or
attachment semantics.

Two independent analyses find context+0x30 == writer_owner-0x78 in all 40,732
inline events. Ruled out: treating +0x30 as independent new evidence of a scene
parent, because it points into the already-known render-owner allocation. Ruled
out: using +0 as individual object identity, because all 520 contexts share it;
and following the optional +0xa0 builder for these 19 scene records, because it
is null in all of them. The +0x48/+0x50 pair is a count/array with stride 0x58,
not evidence of mobility. The retained +0xa8 field is nonzero and distinguishes
the eight scene contexts, but its producer and target semantics remain unknown.
Pointer-shaped variation alone does not justify a parent or static
classification.

The next precise offline edge is the initializer/store of collection-record
+0x290 before consumers 0x4320340/0x4321940, followed by the immediate writes
to context+0xa8 (and +8/+0x78 when present). The audited 0x4312040 routine only
reads +0x290; its complete chained fragments contain no store there, so it is
not that initializer. Bounded disassembly, verified executable identity and
findings are in build/record-writer-context-audit. This evidence narrows the
search without requiring another flight of the unchanged diagnostic.

The analysis and image-generation helpers were rerun twice with deterministic
outputs; their dry runs left the existing archive files byte-identical. The
current reader accepts the capture. Only this investigation document changed in
tracked source; installed binaries and settings remain as tested.

### 2026-09-19 -- offline context construction and KinematicRig ownership

This step uses the saved 120047 capture and the same verified executable,
SHA-256 e6be8bbe04e6a7ae226d4318945af7f367de13dc5a007a261964d9ba8144e988. It
requires no new flight. All addresses below are executable RVAs or explicit
structure offsets. Detailed bounded disassembly, pointer analysis and notes are
retained in build/record-writer-context-audit.

The exact collection-record initializer is now proved. 431B830 loads the
collection owner from outer+0x348 and calls 432A430. That function enumerates
the owner's +0x280 array / +0x298 count with stride 0x2F0, calls factory
42B5780 at 432A6A5, and stores its return directly to record+0x290 at 432A6AA.
The existing 431AFE0 -> 4321940 consumer uses the same outer+0x348 and the same
array/count fields. Factory 42B5780 allocates 0xC0 bytes and calls constructor
42B29C0. Its +0x30 registry comes from collection owner +0x2E0; the descriptor
and shared +0 field come from record+0x18 and record+0x2C8 respectively. This
closes a producer-to-consumer chain, not just an offset resemblance.

Ruled out: context+0x08 and +0x78 as object/parent links, because the
uninitialized allocation and complete constructor leave both holes untouched.
The 75 apparent +0x78-to-other-context matches are allocator residue, not an
ownership graph. Writes that superficially resembled initialization of +8 occur
only after the register is repurposed as a list-entry pointer.

Context+0xA8 is deliberately constructed, but its provenance is render
selection. A fallback lives at +0xB0; 16-bit handles/selectors live at
+0xB8/+0xBA. These final 16 bytes were outside the retained 0xB0 snapshot. The
constructor can replace the fallback through provider virtual slot +0x18,
passing the mutable selector, a float, and a boolean. The provider comes from
outer+0x180 through collection owner+0x10 and registry+0x20. All 520 captured
inline contexts have distinct, stable nonzero +0xA8 values, but no target bytes
or links into the other retained allocations. The eight settlement-associated
contexts likewise have eight different values. Ruled out: treating this
pointer's uniqueness as proof of a scene parent or static classification; it is
a selected render object with unresolved concrete type.

The provider trace narrows this to the LOD-selector interface family. 430B2A0
resolves outer+0x180 using the runtime token at 5F2D684. The same token is
returned by getter A8E530, slot zero of table 513F0F8, installed at object+0x58
by the constructor fragment A8A714. Table slot +0x18 is A8F1D0, whose complete
code looks up a 16-bit handle and binary-searches float thresholds, returning a
selected entry or fallback. Registration at A936D0 names a matching 0x130-byte
implementation LODManager. This strongly identifies LOD selection rather than
ownership; the flight did not capture the provider vtable, so the concrete
runtime implementation remains unverified. The process-local token number
itself is not a stable type identifier.

The direct path also has an exact structural origin. At 4313083, function
4312E00 builds the writer key as its input collection-record pointer+0x250. The
4312040 caller receives this record from the same enumerator schema.
Subtracting 0x250 therefore recovers 895 record addresses across the captured
direct keys, but the retained +0x250..+0x26F bytes omit the context pointer at
+0x290. They cannot retrospectively bridge those five direct-only visible
settlement records to an inline context. The direct-path resource byte test at
+0x4B selects a render path; no mobility meaning has been established.

The useful new ownership evidence is upstream of the collection. Its
initializer 431B2A0 contains exact references to KinematicRig.cpp and a
diagnostic naming Game Object, Item Symbol Name and Kinematic Rig Symbol Name
(431B472/431B4A5, string RVAs 5593820/5593890). The Game Object text is
obtained through outer+0x20. This ties the render collection to a named
kinematic component, rather than guessing a class from adjacent strings.

The same initializer calls 432A840, whose check at 432A912 uses the resolved
interface at outer+0x1C0 and names its dependency "parent root physics model"
(exact reference 432A939, string RVA55937F8). It calls virtual slot +0x90 and
reports when that readiness test fails. This establishes a parent-physics
dependency; it does not identify the planet or tell us whether the child can
move independently.

Constructor 430B2A0 installs final vtable RVA5592380; its +0x10 entry is
4327E40, which calls the proved outer initializer 431B830. At 430B68C it loads
a component token into outer+0x1B8, zeros +0x1C0/+0x1C8, and registers that
dependency through 529500. The helper either queues it or resolves the token,
stores the interface at +0x1C0 and marks +0x1B8 as -1. Ruled out: the -1
sentinel as a static/immovable flag, because the resolver writes it after
lookup regardless of the returned component's mobility. Parent presence alone
is also insufficient for static classification.

The next diagnostic needs a bounded join from the actual KinematicRig owner to
its collection, records and contexts in the captured generation, plus the
game-object, provider and parent-physics interface identities and context tail.
For a proposed attachment classifier, the discriminating evidence is a joined
visible record with a validated parent/interface type and a semantically proved
mobility field. A shared parent, stable payload or resolved token cannot pass
that gate. Capture must separately count unresolved/absent dependencies, read
failures and missing joins so an instrument that never reaches the owner cannot
look like success. Keep this work manual and bounded; do not add ownership
traversal to every normal draw.

The common path at 431B245 still retains RDI=outer and
RBX=[outer+0x348]=collection owner. It follows both the direct 4321940 dispatch
and virtual +0x50 dispatch, but also a no-render cleanup path; the diagnostic
must distinguish those branches. Retain the registry pointer at collection
owner+0x2E0 as an explicit join to context+0x30. The registry is separately
allocated: subtracting 0x2E0 from context+0x30 cannot recover the collection
owner. Record+0x290 and the direct key's record base provide the remaining
joins, with generation and lifetime checks still required.

The current capture cannot supply the missing owner/interface bytes, and an
unchanged diagnostic rerun would not add them. No static filtering, draw
suppression or performance claim follows from this trace. Saved-capture pointer
analysis was deterministic and its dry run wrote nothing; executable traces
were hash checked and instruction boundaries were verified after xref scans.
The shared parent-token reference scan exceeded its bounded cap and was not
broadened. This is documentation-only work; no C++ build or Frontier
installation was needed, and settings remain unchanged.

### 2026-09-19 -- bounded KinematicRig ownership capture

Hypothesis: the verified record-writer call stack can recover the KinematicRig
owner and connect its parent-physics interface to the already captured visible
record candidates. The discriminating evidence is an exact ancestor return
address, restored outer/collection registers, matching collection and registry
pointers, and a recipe-specific record/context join. The parent interface type
and retained bytes can then support offline mobility analysis; a resolved
dependency, null parent or repeated address is not a static classification.

The existing lookup hook already captures a Windows CONTEXT in a normally
unwindable observer. Continuing that unwind to 431B212 (the direct 4321940
return) or 431B21F (virtual +0x50 return) recovers RDI=outer and
RBX=[outer+0x348]=collection owner without a second game-code patch. These are
the dispatch returns before the common 431B245 path considered above. The
no-render cleanup branch does not pass either return; its absence from
writer-driven evidence must not be reported as observed scene coverage.

An additional direct-path distinction was verified before implementation:
431B18C/431B193 stores collection owner+0x300 into descriptor+0x10; 4312040
retains that descriptor in R15, and 43125D8 passes its +0x10 field to 4312E00.
The direct lookup's dictionary is that pointer+0x260. Thus direct_43130aa's
writer owner equals collection owner+0x300, whereas the proved inline/primary
writer owner equals registry+0x78. Applying the inline equation to the direct
path would reject valid direct records. Both paths still require the actual
ancestor tuple and matching pointer reads.

The latest saved capture has 44,596 writer calls across eight threads and four
mesh frames, so worker-thread stacks are explicitly in scope. A missing known
ancestor must retain a bounded sample of module-relative return addresses and
report why the unwind ended. This distinguishes an unsupported dispatch path
from an instrument that never ran without spending a separate flight merely to
discover the missing caller.

The extension uses version 2 of the optional record-writer section, retaining
legacy readers' version-1 captures through the updated analysis tool. It adds
at most 8,192 ownership snapshots and 32 distinct missing-ancestor traces of 32
frames each, under the existing shared 64-MiB writer byte budget. Ownership
records retain the 0x460-byte outer prefix, 0x2F0-byte collection prefix, full
0xC0-byte context, applicable direct-record tail, and guarded 0x80-byte
prefixes of associated interfaces/data. Those opaque prefixes do not establish
the allocations' complete layout. Candidate vtable addresses are related to the
verified executable where possible; no game virtual functions are invoked.

The ownership cache includes the context/direct-record identity as well as the
writer owner, frame, thread, dispatch branch and owner metadata. Distinct
meshes sharing a registry cannot collapse into one owner-context association.
Each event still validates its actual ancestor and tuple. A reused snapshot
records its first event; reuse does not establish persistent object identity or
unchanged physics state. Parent resolution and absent, unreadable or
unsupported evidence remain explicit in both JSON and the completion log.

Direct records may legitimately have no inline context: 4321A3D/4321A41 loads
and tests record+0x290, and 4321A44 skips the inline call when null, after the
direct 4312040 work has already run. A validated direct record can therefore
retain its outer association with an explicit null context; a non-null context
must still match the registry. Inline/primary records require their actual pose
context. The new ancestor opcode checks have a separate refusal counter and
leave the pre-existing writer evidence available.

Before the full build, the focused C++/ASM rig passed 12,528 checks. Actual
nested frames clobber RBX/RDI in the writer and verify Windows unwinding
restores the ancestor values on both direct and virtual dispatch routes.
Fixtures cover shared-owner/different-context separation, changed metadata,
parent states, direct null contexts, read/range refusals, unsupported writers
and the ownership cap. The reader's self-tests and the newly emitted C++ source
fixture pass, including writer -> virtual_50 owner -> available parent and
exact packed-byte accounting. Archived 085213, 100737 and 120047 captures
remain readable. These establish diagnostic behavior in fixtures; the next
flight must establish which owner paths occur in the real scene.

The full build passed all 68 pooled jobs, three quiet checks and the 251-key
config contract, including verification of the actual installer resources. The
first two-job run terminated installer and Python-gate subprocesses with
4294967295 and no compiler diagnostic; its cause is unproven. The one-job rerun
passed without omitting any gate. Both logs are retained under build/ as
kinematic-owner-build-r1-failed.log and kinematic-owner-build.log.

Frontier now has v0.17.0-56-g88c1ce2-dirty, graphics stamp 6AAEDE0A and runtime
stamp 6AAEDE0D. The sanctioned installer dry-run, install and verify-only all
passed. Both settings files are byte-identical before and after; static
surfaces remain off, with DLSS and manual Insert retained. The executable hash
is unchanged. Payloads, fixtures, source patch, receipts, build log and
manifest are archived in build/kinematic-owner-frontier-88c1ce2. The graphics
SHA256 is e96df10234c1b6c06ed2ab98ec62b4aaf8442a692b05d1bc563a570180c4f4ba;
runtime SHA256 is
d3615fc9dd44bf1979023a39590a8e98359b509f2a6d4427a0d985838b0bbd93. Use that
literal installed version when checking the next flight, since the subsequent
source commit will advance HEAD without changing these payloads.

Next flight: the same landed settlement view with DLSS on; press Insert once,
then leave the game running at least 30 seconds before exit. No flicker or
Pause is required. This is an ownership diagnostic, not an optimization or a
performance comparison during the capture window.

Sean is also having an independent agent disassemble settlement-related code.
The highest-value handoff is semantic evidence for mobility and visibility:

- KinematicRig constructor RVA 430B2A0 and render/update 431AFE0: trace
  collection owner +0x348, associated game-object interface +0x20 and
  descriptor +0x50.
- Resolve parent-physics +0x1C0 implementing classes. At 432A840 the diagnostic
  names the parent root physics model and calls virtual +0x90; 431B7D4 uses
  virtual +0x170. Find exact writes and consumers proving fixed, moving,
  kinematic or parent-relative semantics, including independent child motion.
  The +0x1B8 token becoming FFFFFFFF only proves dependency resolution.
- Trace the alternate 4320340 caller/job dispatch and compare it with the known
  431AFE0 -> 4321940 route. Records have stride 0x2F0 and context at +0x290.
- Trace visibility decisions and mask producers before dispatch around
  431B11D-431B221, including whether they encode frustum, occlusion, pass/layer
  eligibility or simply render readiness. Prove both producer and consumer.

These addresses are RVAs in the executable with SHA256
e6be8bbe04e6a7ae226d4318945af7f367de13dc5a007a261964d9ba8144e988 (preferred
image base 0x140000000). Names and repeated values alone do not prove safe
motion exclusion or safe suppression of Elite's draws.

### 2026-09-19 -- ownership flight 132513 and opcode-guard correction

Flight edvr_gfx_20260919_132329.log matches installed
v0.17.0-56-g88c1ce2-dirty, stamp 6AAEDE0A. Insert armed at 13:25:13.761 and the
sealed classification completed at 13:25:17.769, well before shutdown. The
capture is archived with source/destination SHA256 verification in
build/kinematic-owner-flight-132513; the archive dry-run created no directory.
The reader validates the capture. Mesh/scene frame 11058 has 119 draws and 36
exact-depth visible records covering 191,708 pixels. All 36 have CPU upload
matches; writer joins have 11 unique, 20 multiple and five absent candidates.
These candidate counts are not proven object identities or static counts.

All 44,319 writer calls were stored and completed, with no read, unwind,
completion or budget failures. Ownership attempted 44,055 eligible calls; 264
unsupported writers were explicit. Every attempted ownership call was rejected
by the opcode guard, leaving zero links, snapshots or ancestor traces. No
parent-physics or alternate-dispatch conclusion is possible from this run.

Ruled out: the installed executable matching the diagnostic's ownership opcode
expectation, because the exact hashed EXE has E8 2E 67 00 00 at RVA 431B20D,
where the guard expected E8 2E F7 00 00. The actual call targets 4321940 and
returns to 431B212, agreeing with the prior disassembly. The virtual call at
431B21C is FF 50 50 and returns to 431B21F, as expected. Validation requires
both sites, so the single incorrect displacement byte disables both paths. This
is an instrument defect, not evidence of a changed game or static objects.

The correction must preserve strict instruction validation, include a
regression derived from the real executable window with rejected wrong-target
variants, and exercise the production guard against the verified EXE before
another flight. Rendering behavior and static filtering remain unchanged.

The corrected guard decodes the signed E8 displacement and requires the proved
target RVA 4321940, together with the unchanged exact FF 50 50 virtual call.
Its regression uses the independently captured 40-byte executable window; wrong
target, neighboring call placement and wrong virtual slot are rejected. A
refusal-path fixture confirms that writer records still complete when ownership
validation refuses the image.

The isolated C++/ASM rig in build/kinematic-opcode-preflight passed 12,534
checks. Its optional executable argument maps the game with
SEC_IMAGE_NO_EXECUTE and invokes the same production validation as the
installed hook. Against the live EXE, rehashed to the exact SHA256 above, it
passed 12,541 checks. This covers the PE identity, lookup prologue, all seven
writer/management callsites and both ownership guards; it neither executes game
code nor proves a live object association.

The full absolute build passed all 68 pooled jobs, three quiet checks and the
251-key config contract, including actual installer-resource verification.
Frontier installation and verify-only passed with both settings files
unchanged. Installed version: v0.17.0-57-ge700d4f-dirty; graphics stamp
6AAEE376, runtime stamp 6AAEE379. The payloads, source patch, fixtures,
preflight evidence, build log, install receipts and hash manifest are archived
under build/kinematic-opcode-frontier-e700d4f. Graphics SHA256:
f3efee257b2ec50878802d206cf08af53cb3229c53ea44026328eb937bb9586f. The next
flight must match this literal version even after the fix is committed.

Next capture: same landed settlement view, DLSS on, Insert once, then wait 30
seconds before exit. No Pause or flicker reproduction is necessary. This
corrects the instrument only; ownership and mobility still require live
evidence.

### 2026-09-19 -- ownership flight 134252: asynchronous caller evidence

Flight edvr_gfx_20260919_134111.log matches v0.17.0-57-ge700d4f-dirty and
graphics stamp 6AAEE376. Insert armed at 13:42:52.664; the sealed
classification finished at 13:42:56.441. The JSON/binary pair is hash-verified
in build/kinematic-owner-flight-134252; the archive dry-run created no
directory. Mesh/scene frame 11084 contains 126 draws. The validated reader
finds 29 exact-depth visible records covering 153,957 pixels, with CPU source
matches for all 29. Writer candidates are unique for eight records, multiple
for 17 and absent for four. No object identity or mobility follows from these
counts.

The writer diagnostic completed all 44,303 calls: 40,452 inline, 3,587 direct
and 264 unsupported helper calls. No writer faults, caps or incomplete records
occurred. All 44,039 eligible ownership attempts passed opcode validation, but
none found either expected owner ancestor. They were labeled unwind_failed; 18
distinct traces were retained without trace overflow.

Ruled out: recovering the KinematicRig owner through the two expected ancestor
returns in the saved worker paths, because no retained trace contains 431B212
or 431B21F. Seventeen traces continue through scheduler callers and
outside-image returns to a zero RIP; this is not evidence that all those stacks
were corrupt. One short trace stops after 43125E4,4312655 and needs separate
explanation. The diagnostic currently labels stack termination as unwind
failure and has no per-trace frequency counts, so 17/18 is a count of distinct
saved paths, not a percentage of calls.

Inline paths reach 4321AE0 then 5D6A81. Direct paths reach 43125E4, optionally
4312655, then 43219C0 or 43203F6 and 5D6A81. The absent upstream owner cannot
be recreated by increasing stack depth or repeating the same flight. Task
payload and submission code must establish the missing owner association.

The existing v2 producer retains the final zero RIP as outside_image/null RVA;
the reader previously rejected that shape. The reader now narrowly accepts it
only as the final frame of an unwind_failed trace. Zero addresses in the
middle, zero module-relative frames and outside-image frames carrying an RVA
remain rejected. Its self-tests and the original unmodified flight capture
pass. This compatibility correction does not turn failed owner evidence into a
join.

Hash-verified bounded disassembly confirms 5D6A7E is call [rax+8], returning to
5D6A81 inside generic worker 5D6960. Five saved shapes are inline writers
inside 4321940; six are direct writers through 4321940 and six through 4320340.
The 18th stops at recursion. These are distinct trace shapes, not per-call
frequency counts. Writer records span eight threads and mesh frames
11081-11084.

Function 4312040 recursively calls itself at 4312650, returning to 4312655. The
current unwindOne requires both increasing RSP and a changed RIP, so a valid
repeated return address can truncate traversal. The short trace ends at that
recursive return. Increasing RSP is the relevant progress check; the bounded
depth remains necessary. Normal RIP-zero termination also needs an explicit
outcome. Correct both with regression fixtures alongside the next capture
instrument, without spending a flight solely on outcome labels.

The worker payload has an exact provenance chain. At 431B193, 431AFE0 builds
descriptor+0x10 as collection_owner+0x300; descriptor+0x28 is its record array,
+0x30 the count, and +0x40 the registry. Function 4321940 consumes the copied
0x60-byte descriptor; 4320340 enumerates the same descriptor layout. At the
observed writer-return ancestors, restored RDI in 4321940 or R14 in 4320340 can
identify the descriptor. These fields can validate a collection/registry
association but do not contain a direct outer pointer. The saved capture has
only ancestor addresses, not those register contexts or descriptor bytes.

The next instrument should therefore retain the owner-to-collection association
at the proved 431AFE0 submission path (entry RCX is outer; later RDI=outer,
RBX=collection_owner), and match it against the worker descriptor and existing
writer evidence. It must retain observation order/frame, validate registry and
record ranges, and expose absent, conflicting or reused associations rather
than promote pointer equality to persistent identity. Tests should model a
queued callback whose submitting caller has already returned, including
recursion and normal stack termination. A worker-only capture cannot establish
outer ownership. The mapping is still diagnostic evidence, not a static label.

Reader self-tests and the original 134252 capture pass after the narrow
terminal-frame compatibility fix. No C++ or installed game files changed in
this analysis; Frontier remains on v0.17.0-57-ge700d4f-dirty. An unchanged
headset rerun is unnecessary until the submission/worker instrument is ready.

### 2026-09-19 -- static reversing: KinematicRig pipeline decoded (Ghidra, no flight cost)

Setup now in-repo: Ghidra 12.1.3 headless project at analysis\ghidra\EDAnalysis
(exe SHA-256 e6be8bbe...e988, byte-identical to the capture target; .pdata gave
253,483 function entries, analysis\pdata_functions.csv), portable Temurin 21 at
analysis\jdk-21.0.12.1+1, Java GhidraScripts in analysis\ghidra_scripts, decomp
dumps in analysis\decomp. MSVC RTTI is stripped (20 stray names only); class
identity comes from Cobra reflection strings and vtable-adjacent job-name strings.
Gotcha recorded: GhidraScript toAddr(long) is a raw offset, not image-base+RVA;
use currentProgram.getImageBase().add(rva).

Decoded, all RVAs, all confirmed against the hash-verified exe:

- Lifecycle state dword at rig+0x380 (FUN_14432aa00 prints it): 0 Dead,
  1 WaitingToCreate (m_blockInitialisation=%d), 2 resolving dependencies
  (invokes the 0x432A840 labeler), 3 CreatingRT, 4 Ready, 5 DestroyingRT,
  6 DestroyingMT. Render/update 0x431AFE0 runs only in state 4; the
  initializer 0x431B2A0 leaves the rig in state 3. Source path in assert:
  Libraries\Systems\Kinematic\Component\KinematicRig.cpp line 0x6d8.
- Dependency triples: tokens at +0x188/+0x1A0/+0x1B8 (int, -1 = resolved)
  pair with interface pointers at +0x190/+0x1A8/+0x1C0. "token -1 AND pointer
  non-null" is the linked/ready invariant everywhere it is tested. The labeler
  passes &rig+0x1B8 to FUN_140e75ac0 (dependency registry) named "parent root
  physics model"; slot +0x90 of the +0x1C0 interface is a readiness predicate;
  slot +0x170 writes a parent-relative 3x4 pose to rig+0x58. The initializer
  calls +0x170 only when descriptor flags (rig+0x50 -> +0x28) bit 12 are clear.
- Culling in 0x431AFE0 (0x431B11D-0x431B221 region decoded): the 64-bit mask
  uVar9 is the OR of record+0x570 over eligible stride-0x6A0 records
  (eligibility: FUN_142840840 set-membership vs rig+0x350, record+0x68C bit 5,
  and FUN_14432C520 layer test ANDing record+0x68C against rig masks
  0x3D8/0x3DA/0x3DC selected by record class). uVar9 then has one of three
  masks at outer+0x1A950/+0x1A958/+0x1A960 ANDed out, selected by
  collection+0x70 bit 7. Empty mask or collection+0x70 bit 0 clear -> cheap
  path, no dispatch. The surviving mask is descriptor field +0x48.
- Dispatch: 0x431AFE0 builds the 0x60-byte worker descriptor and either calls
  0x4321940 directly or, if collection+8 non-null, virtual slot +0x50 (job
  queue). 0x4321940 consumes the single descriptor; 0x4320340 consumes a
  descriptor ARRAY at param+0x40 stride 0x60, count at param+0x300. Both run
  the same stride-0x2F0 record loop gating on record+0x290 (pose context),
  +0x234, +0x298, then per set bit of record+0x208 compare the 4-bit LOD
  nibble in record+0x210 (bit b -> qword b>>4, nibble b&0xF) against the
  ushort cap at *(record+0x18)+0x6A, and submit via FUN_1442B4420.
- The unresolved caller path into 0x4320340 is closed: 0x42DF47A/0x42DF523/
  0x42DFAF3 are not functions but thunks in a no-.pdata trampoline table.
  0x42DF460 is a this-adjust (-0xD0) adjuster into 0x431AFE0; 0x42DF520 and
  0x42DFAF0 are "MOV RCX,RDX; JMP" job adapters into 0x4321940/0x4320340.
  Their vtables sit at 0x145591C98 and 0x145591DC0, and the bytes immediately
  after the first vtable are the ASCII job name "Kinematic::UpdateRenderDataJob".
  Both dispatch functions are therefore reached through the Cobra job
  scheduler via named job descriptors, not direct calls.
- Children can move independently: FUN_144312040 (the recursive function in
  the 134252 unwind traces) walks pose contexts via children array at
  pose+0xAA/count+0xAC, recomposes parent x child 3x4 matrices per node, and
  consults a per-node predicate interface at pose+0xB0 (slots +0x70/+0x58)
  before recomputing. Planet attachment does not make children static.
- Factory: FUN_14431D2A0 allocates from pool DAT_145FC1B60 (gated on
  DAT_145FC1C20) and returns object+8, i.e. callers receive the secondary
  interface, not the object base.

Ruled out: 0x431B7D4 as a distinct state writer -- it is a 77-byte fragment
immediately after FUN_14431B2A0 sharing its tail (slot +0x170 call, +0x458=1,
+0x380=3); treat the initializer as one function.

Open, in priority order: (a) which class implements the +0x1C0 interface --
scan .rdata for vtables whose slots 18 (+0x90) and 46 (+0x170) point at
.pdata entries, decompile candidates; (b) which collection+0x70 flag bit, if
any, is the static/anchored classification EDVR wants -- bit 10 (0x400) is
cleared unless the +0x1A0 dependency is resolved, bit 0 gates dispatch, bit 7
selects the exclusion mask set, bit 8 is default-on unless rig+0x3F9;
(c) outer+0x1A950/58/60 mask provenance (which pass/eye each covers);
(d) thunk-table callers of the 0x145591DC0 batch-job vtable to find where
settlement batches are queued.

### 2026-09-19 -- wave 2: dependency bus, collection layout, static-bit verdict

Second Ghidra pass (scripts and dumps in analysis\, all RVAs, hash-verified exe):

- The dependency tokens are a typed component bus, not a name registry.
  FUN_14432F500 (the "labeler" helper) only builds log strings
  ("KinematicRig %s/%s <label>"); the "parent root physics model" string has
  exactly one xref (the labeler) and is diagnostic text. Resolution runs
  through per-interface template functions keyed by interface-name strings:
  "IKinematicRig" at RVA 0x5140308, "IPhysicsData" at 0x5284438,
  "IShipPhysics" at 0x5331AB0. The bus core FUN_1408C4D70 walks a provider
  linked list (node+0x3C8 = next); each node's provider at node+0x38 answers
  an interface-ID query via its vtable slot 0. ~100+ call sites; this is the
  Cobra component model.
- Consequence for the +0x1C0 question: the parent-physics interface is a
  provider-list node looked up by interface ID, so it has multiple
  implementors by design (planet body, ship, station physics). There is no
  single class to name; what EDVR can rely on is the contract: slot +0x90 =
  readiness predicate, slot +0x170 = write parent-relative 3x4 pose.
- Collection ctor FUN_14430A060 layout: +0x18 owning rig; +0x30..0x58 the
  48-byte default transform block (same 0x1450C8090 global as the rig ctor);
  +0x70 flags dword; four 17-bucket hash sets at +0xF8/+0x150/+0x180/+0x1B0
  (load factor 0.75 at +0x140) backed by rig arenas +0x110/+0x180/+0x1F0/
  +0x260; render record array fields +0x280..+0x2A0; +0x620 = 0xFFFF.
- Static-bit verdict: collection+0x70 is static configuration, not runtime
  mobility. Its low bits come from the asset descriptor flags dword
  (descriptor+0x28) via the initializer; bit 10 (0x400) is cleared unless the
  +0x1A0 dependency is resolved; bit 8 (0x100) defaults on unless rig+0x3F9;
  bit 0 gates dispatch in 0x431AFE0; bit 7 selects the exclusion mask set.
  No per-frame "is moving" bit exists in this structure, and the +0x1C0
  interface exposes no "static" query either (readiness + pose read only).
- The engine's own change tracking is the right hook target instead:
  FUN_144312040 consults a per-node predicate interface at pose+0xB0 before
  recomputing a node -- slots +0x70 (node, pose ctx, 0) and +0x58 -- and the
  pose context caches the composed matrices and LOD/mask state at +0x82..+0x8C
  with a resolved-handle cache at +0x00 (two -1 shorts = unresolved).
  Mirroring that predicate's verdict is a cheaper and more truthful mobility
  signal than pose-bytes equality. Open: which class sits at pose+0xB0 (set
  by the pose-context constructor FUN_144331300) and the exact polarity of
  its two slots.
- Cheap path decoded: FUN_14434DD50 (called with collection+0x300) just
  clears a dword at +0x2A4 of its argument -- a "no visible work this pass"
  reset, matching the zero-dispatch path in 0x431AFE0/0x4321940/0x4320340.

ruled out: identifying one +0x1C0 implementing class -- the bus is
interface-ID based with multiple implementors; the useful contract is the
slot pair, not the class.

### 2026-09-19 -- wave 3: LOD metric, record teardown, Kinematic job map

- FUN_144331300 (called at the top of 0x4321940/0x4320340) is the LOD metric
  computer: transforms a view position through two 3x4 matrices, walks the
  stride-0x6A0 records, and for each record whose record+0x570 mask hits the
  dispatch mask computes radius * (1/dist - k) * fov_scale + bias via
  rcpps/rsqrtps, then searches a threshold-band table for the 4-bit LOD
  nibble (slot index at record+0x5A4, table count at table+0x38). It emits
  the nibble array + remaining mask that the stride-0x2F0 loops consume.
  This is the per-frame CPU cost EDVR's motion work sits beside.
- FUN_14430EFE0 (inner per-record update in the FUN_144312040 traversal):
  gates on record+0x570 & mask, record+0x688 & 0x7FF0 flags, two record-class
  predicates, a portal/region test FUN_1404F4E10 (bounds pair in/out, -1 =
  reject), a distance-vs-band LOD pick, and a shadow-cascade branch
  (FUN_142842E90/FUN_14288AC40/FUN_14288A1E0) with a lazily-built cached
  plane set (flag byte at cache+0x60). Output: LOD index + visible byte.
- FUN_144332707 is the collection teardown: walks collection+0x280 records
  stride 0x2F0 releasing record+0x2B8 objects (vtable slot +8 arg 1) and
  zeroing record+0x290 (the pose context pointer), frees the +0x2E0 registry,
  clears collection+4 bit 1. Record population is a different function, still
  unidentified; the pose+0xB0 predicate writer is inside that path.
- Kinematic job family mapped from inline-name tables in .rdata (raw file
  pointers are 0x140000000-based VAs; preferred base 0x20000001000 is NOT
  what is stored). Descriptor layout: [phase fn slots..., 0x140578300,
  run thunk, GetCategoryStats thunk 0x1400039F0, 0x1401F2FE0, inline name].
  Jobs: JobBatcher, UpdateRenderDataJob (phase functions 0x1442DE6F0..
  0x1442DF4C0, run thunk 0x1442DF520 -> 0x4321940), PrePhysicsAdvanceJob
  (run 0x1442DF530), UpdatePhysicsObjectsJob (run 0x1442DF540),
  PrePhysicsAdvanceCurveJob (run 0x1442DF550), plus three unnamed tables
  with run thunks 0x1442DFAF0 (-> 0x4320340 batch), 0x1442DFB00,
  0x1442DFB50, 0x1442DFBA0. Map regenerated by analysis\kinematic_jobs.py.
- Correction to wave 1: the 14-run / 292-triple vtable scan
  (analysis\vtable_candidates.txt) is invalid -- it RVA-range-checked raw
  qwords and matched 32-bit offset tables. Disregard it; the thunk-table
  conclusions stand because they were verified through Ghidra memory.

Open: which function fills collection+0x280 records and writes pose+0xB0 --
  the UpdateRenderDataJob phase functions 0x1442DE6F0..0x1442DF4C0 are the
  search space, one decompile wave of ~25 functions, or a narrower store-scan
  for stride-0x2F0 writes of a fresh allocation.

### 2026-09-19 -- waves 4-12: record filler found, predicate field corrected,
### change-detection pipeline decoded (static, no flight spent)

- Record filler FOUND by exhaustive instruction store-scan
  (analysis\ghidra_scripts\FindStores.java, [reg+0x290] stores filtered to the
  0x14430-0x14434xxxx range). It is FUN_14432A4E8, a fragment of the 30 KB
  pdata function 0x4329984-0x4330160 (the parent resists decompilation:
  NO FUNCTION EVEN AFTER FORCED DISASSEMBLY). Per record (base
  collection+0x280, stride 0x2F0, count collection+0x298): record+0x2B8 =
  helper from factory FUN_14434C9A0, registered into the collection+0xB0/+0xB8
  list (count record+0x2D8); record+0x2C0 = interface from vtable slot +0xB8;
  record+0x2C8 = interface from slot +0x50 result slot +0xA0; record+0x290 =
  pose context via FUN_1442B5780 (allocates 0xC0 bytes via ctor FUN_1442B29C0,
  appends to registry collection+0x2E0 with cap/count/array at +0x58/+0x68/
  +0x70). Ends with collection+4 |= 2 (records-built flag). Per record the
  filler also runs FUN_14433C750: a 64-bit content hash of the +0x2B8 helper
  state -> record+0x268 (inputs cached record+0x250/+0x258/+0x260), and
  FUN_144312A60: initial render setup consuming record+0x290/+0x2D0.
- MAJOR CORRECTION to waves 1-3: the per-node predicate is at record+0x2C0,
  NOT pose+0xB0. FUN_144312040s param_5 is undefined4*, so param_5 + 0xb0
  is BYTE offset 0x2C0 (x4 pointer scaling misread). Verified against the
  consumer 0x4321940: pcVar8 = record+0x234; gates are record+0x290 non-null
  AND record+0x234 bool != 0 AND record+0x298 != 0; mask words read at
  record+0x208 and +0x210+i*8; LOD cap ushort at [record+0x18]+0x6A. All
  match the traversals writes once scaled. Corrected record (stride 0x2F0)
  layout: +0x0/+0x2 dependency tokens (-1 = resolved, via pred slot +0x80);
  +0x18 node; +0x208..+0x228 five cached mask qwords; +0x230 pass id; +0x234
  cached has-work bool (traversal writes it, consumer honors it); +0x2A8/
  +0x2B0 children array/count; +0x2B8 helper; +0x2C0 predicate interface;
  +0x2C8 secondary interface; +0x2D8 registration count; +0x2E0 registry.
- Predicate semantics (traversal, called from 0x4321940/0x4320340): the live
  byte = 1 ONLY IF record+0x2C0 != 0 AND slot+0x70(pred, record, 0) == 0 AND
  slot+0x58(pred) != 0. The byte lands in the worker descriptor at +0x49 and
  FUN_14430EFE0 honors it: byte set AND render-record (stride 0x6A0) +0x688
  bit 12 (0x1000) set -> skip the record (FUN_142854140 tests bit 12). A
  second gate: descriptor +0x58 byte AND record+0x688 bits 4-11 (0xFF0,
  FUN_142852F80) -> skip. So the engines skip signal is flag bit 0x1000 at
  render-record+0x688, honored only when the predicate allows. Polarity
  hypothesis: slot+0x70 nonzero = changed/cannot-skip; slot+0x58 zero = not
  tracking. Implementing class still unidentified: the filler obtains it via
  vtable slot +0xB8 on an object (unaff_R12) whose type lives in the
  undecompilable parent 0x4329984.
- The object at POSE-CTX+0xB0 is unrelated to the predicate: FUN_140A7ED60
  builds a 0x80-byte POD quantized-float array (scale DAT_14513FAA8, up to 7
  values, count at +0x70) -- no vtable. Pose-ctx ctor FUN_1442B29C0 zeroes
  pose+0xA8/+0xB0/+0xB8, writes a 16-byte GUID from DAT_1450C80C0 at +0x10,
  registry at +0x30, then installs the quantized array at +0xB0.
- Dirty-channel pipeline: FUN_14431E860 (called from unnamed job run
  0x1442DFB50 and three other sites) walks float channels collection+0xC0
  (stride 0x14, count +0xC8), |current-previous| >= epsilon DAT_144DEDD10 ->
  set bit in a local bitmap, copy current->previous. Any dirty (or force
  byte collection+0x98) -> per record FUN_14432CCC0(record, collection+0x30,
  dirty_bitmap, flags). FUN_14432CCC0 ANDs the dirty bitmap into sub-object
  mask sets (array record+0x1A8, stride 0xB0, count +0x1B0), transforms the
  source bounds record+0x1A0 by record+0x130..0x16C and writes world bounds
  record+0xB0..0xEC plus transform products record+0xF0..0x12C.
- Ruled out: FUN_14433AF10 = physics double-buffer swap/reparent (state
  machine returning 0/1/2, refcount churn, transform copy at +0x1E0), not the
  record filler. FUN_14431A0D0 = streaming-queue drain over the op stack at
  collection+0x5D8/+0x5E0; reads collection+0x280 but never populates it.

Open: (a) predicate implementing class and definitive slot polarity -- the
  +0x688/+0x2C0 writer scan (analysis\decomp\flag_writers.txt) found only
  the filler's own install at 0x14432A588 and no +0x688 writers in the
  Kinematic range; the 0x6A0-stride render record belongs to the renderer,
  so its flags are set elsewhere (96 MOV writers binary-wide, none OR/AND).
  Next evidence: a runtime vtable capture at record+0x2C0; (b) parent
  function 0x4329984 (30 KB) needs a manual slice or smaller sub-function
  forcing to identify unaff_R12; (c) exclusion-mask outer+0x1A950/58/60
  provenance; (d) callers of the batch-job table 0x145591DC0.

### 2026-09-19 -- next flight spec: predicate vtable + skip-flag capture

Goal: close the two remaining static unknowns with ONE settlement flight --
the predicate implementing class (record+0x2C0) and the runtime behaviour of
the skip flag (render-record+0x688 bit 0x1000). Read-only dereference only;
no writes to game state.

Hook: FUN_14430EFE0 (RVA 0x430EFE0, the per-render-record evaluator the
traversal calls; xrefs prove it runs inside both 0x4321940 and 0x4320340
worker paths). Entry hook, ABI args: RCX = worker descriptor, R8 = render
record (stride 0x6A0). From the descriptor: record2f0 = *(u64*)(RCX+0x10),
predicate byte = *(u8*)(RCX+0x49), second gate byte = *(u8*)(RCX+0x58).

Log per unique record2f0 (dedupe by pointer; reuse the existing safe-read
and admission machinery from the ownership capture):
- predPtr  = *(u64*)(record2f0+0x2C0); predVtable = predPtr ? *(u64*)predPtr
  - imageBase : 0   <-- THE key datum; the vtable RVA identifies the class
- pred2Vtable likewise from record2f0+0x2C8
- node     = *(u64*)(record2f0+0x18)
- poseCtx  = *(u64*)(record2f0+0x290)
- bool234  = *(u8*)(record2f0+0x234) (cached has-work bool)
- hash268  = *(u64*)(record2f0+0x268) (content hash from FUN_14433C750)
- flags688 = *(u32*)(R8+0x688) (skip bit 0x1000; gate bits 0xFF0)

Emit one line per unique predVtable/pred2Vtable (that is enough to close the
class question), plus a transition line whenever flags688 or hash268 CHANGES
for a known record2f0 (not every call -- the 44k-call flood of flight 134252
must not repeat). Keep the 512-record admission cap but make evictions
explicit in the log.

What each outcome proves:
- predVtable RVA -> I decompile its slots +0x58/+0x70/+0x80 statically and
  the polarity question (slot+0x70 == 0 = unchanged?) is settled without
  another flight. One vtable = one decompile wave.
- flags688 bit 0x1000 stable-set on known-static settlement records across
  frames -> the engine already marks them skippable; EDVR can key its
  static-skip on the predicate path instead of its own classifier.
- hash268 stable for records whose raw pose is unchanged (the 501/512
  cohort) -> validates record+0x268 as a cheap skip key.
- If predPtr is always 0 in settlements, the predicate path is inert there
  and the whole slot-polarity question is moot for this use case -- that
  would redirect the hook design to the +0x688 flag writers directly.

Environment note: landed cockpit, Quest 3 / VirtualDesktopXR / RTX 5090,
DLSS K -- same rig as flight 134252 so record/flags behaviour is comparable.

### 2026-09-19 -- next-flight spec addition: job-stats CPU attribution probe

Same settlement flight as the predicate capture above; read-only.

Goal: attribute the settlement CPU frame (landed ~10.0-10.8 ms p50 vs station
~4.75 ms) across Elite's own job families, so the optimization target is
chosen from measurement: render-data jobs vs physics vs animation vs
submission. Current evidence gives call counts (22.7-23.8k native calls
landed) but no in-engine CPU split.

Mechanism: every Kinematic job descriptor in .rdata carries a
GetCategoryStats thunk (0x1400039F0 pattern) beside its run thunk and inline
name (job map: analysis\kinematic_jobs.py; tables 0x145591C98, 0x145591DC0
and the unnamed runs 0x1442DFAF0/0x1442DFB00/0x1442DFB50/0x1442DFBA0). The
probe calls each known job table's GetCategoryStats once per frame for one
60-frame window landed at the settlement and one at a station, and logs the
per-family accumulators (layout unknown a priori: dump the returned struct
raw, 64 bytes, and diff across frames -- deltas are the per-frame family
times). Fallback if the thunk returns null or static zeroes: bracket the run
thunks themselves (0x1442DF520 -> 0x4321940, 0x1442DFAF0 -> 0x4320340,
0x1442DF530/40/50 physics) with QPC timestamps, count calls, report per-job
ms/frame.

What each outcome decides:
- UpdateRenderDataJob dominant -> the per-record pipeline mapped in waves
  4-12 (LOD metric + traversal + FUN_14430EFE0) is the settlement CPU tax;
  feed the kinematic-motion-injection phase 1 and the static-skip hook.
- Physics jobs dominant -> kinematic work is innocent; redirect to the
  physics object census before any render-side build.
- Neither dominant (submission thread) -> the 23k-call D3D11 volume is the
  tax; the lever becomes draw suppression of the 76% zero-sample draws, not
  the kinematic pipeline.

Cost guard: one indirect call per job table per frame (5-9 calls), one raw
64-byte copy each; no game-state writes; off by default behind a config key.

## 2026-09-19 (evening): capture probes built and installed to frontier

The two probes from the spec above are in, built green and installed to
the frontier copy (commit 116a2e0). kinematic_eval_probe captures, per
unique 0x2F0 record at FUN_14430EFE0 entry: record ptr, node, pose ctx,
content hash (+0x268), count298, predicate ptrs (+0x2C0/+0x2C8) with
vtable RVAs, bool234, render-record flags (+0x688) and both gate bytes,
with transitions on flags/hash change. kinematic_eval_hook brackets the
six job bodies (incl. 0x4320340 batch and both PrePhysicsAdvance jobs)
with QPC for the settlement CPU-frame split. One build wrinkle, fixed
before the build: this checkout had core.autocrlf=true from the bundle
git's system config, so the tree was CRLF on disk and the
original_draw_probe_test binary source-contract check failed at clean
HEAD; set core.eol=lf + core.autocrlf=false repo-local and re-checked
out the tree (744 LF / 20 CRLF, matching .gitattributes), gate passes,
621 checks. After the flight:
python tools\edvr_log.py --target frontier --expect-build HEAD

### 2026-09-19 20:31 flight: instrument failed, no eval evidence

The 20:31 flight ran the probe build (DLL link time 01:30:45 UTC matches
build output; --expect-build HEAD fails only because the build predates the
evening commits -- gf03cdb6d-dirty IS the probe build). No sentinel
refusals, no fault sites, record-writer probe healthy (44275 records).

ruled out: nothing about the engine -- the kinematic-eval hook never
installed. Log line 1192: target 00007FF76CAFEFE0 is more than two
gigabytes from the replacement; a five-byte E9 cannot reach our DLL from
the exe. The writer hook already solved this with a relay stub allocated
near the target; the eval hook had passed its replacement to CodeHook
directly. Fixed by porting the relay pattern (44-byte stub, gate on the
observer atomic, absolute-indirect tail jumps) to the eval hook and all
six job brackets; detach on finish/reset so relays fall through to the
trampoline between captures. Rebuilt green, installed to frontier
20:49, needs a refly. Prologue note: stolen bytes are 40 53 / 55 / 56 /
57 (exactly 5); the patchIsOurs tail check expects 41 54 41 56 41 57 48
83 after the patch.

### 2026-09-19 20:52 flight (relay-fix build): kinematic dominant, flags ruled out

Capture 20:52:51.9-20:52:55.6 (~3.75 s, ~63 fps at the settlement, eye run
205251). All hooks installed except kinematic-job-3: 0x42DF530 begins with
a jmp -- it is itself a thunk, not the PrePhysicsAdvanceJob body; decode
its target next Ghidra run. Job-4 hooked but saw zero calls.

CPU per frame: UpdateRenderDataJob 32604 calls, ~138/frame x 51 us =
~7.0 ms; RenderDataBatch (0x4320340) ~24/frame x 98 us = ~2.4 ms;
UpdatePhysicsObjectsJob ~5/frame x 7 us = 0.03 ms; BA0 negligible.
Kinematic render-data ~= 9.4 ms of the 15.9 ms frame; physics innocent.
CAVEAT: the eval observer adds per-call overhead that may nest inside the
job brackets (eval call path not yet mapped to jobs); the split is robust,
the absolute ms is an upper bound. Decision table: kinematic dominant ->
L3 eval narrowing + motion-injection phase 1.

Engine semantics:
- One predicate class everywhere: all 2633 records share vtable
  0x52e9288. Decompile its slots (+0x70/+0x58) next Ghidra run.
- ruled out: render-record+0x688 flags as a stasis/skip signal -- 1.44M
  transitions, 99.7% of eval calls, two interleaved count-up chains
  (0x3->0x100000->0x200000->0x400000->0x800000->0x3 and
  0x3->0x5000->0x4010->0x4020->0x4040). It is a per-call sequencer; the
  0x1000 bit appears transiently mid-sequence on every record.
- hash record+0x268: ZERO changes across 1.4M calls; 500 distinct values
  over 510 nodes (near 1:1 per node). Proven-static candidate, but a
  static scene cannot distinguish stable-hash from not-a-hash; needs a
  capture with known movers to confirm it tracks content.
- pred_byte (descriptor+0x49): 2132 records 0 / 501 records 1.
  bool234: 2624 / 9. Per-record gates to cross-reference with the
  predicate decompile.
- 2633 records over 510 nodes (~5 records/node); ~6120 eval calls/frame,
  ~2.3 per record per frame.

Probe defect found and fixed post-flight: transition JSON writer dropped
the closing quote on new_hash (analysis used regex; fixed in tree).

### 2026-09-19 21:05 Ghidra: eval gate mapped; two corrections

- CORRECTION: record+0x2C0 is not a per-record change predicate.
  *(pred) points at the string "p::DeferredBufferView" (RVA 0x52e9288 sits
  in a string table beside "NodeGroup::DeferredShading" and
  "NodeGroup::Schematics"). All 2633 captured records share the same
  +0x2C0 object, so it cannot carry per-record change state. The
  "predicate vtable 0x52e9288" line in the 20:52 entry above is that
  string's RVA, not a vtable. Where +0x2C0/+0x2C8 are actually consumed
  is unresolved; claim no meaning for them until a consumer is traced.
- Eval FUN_14430EFE0 gate chain, in order (decomp_430EFE0.txt):
  1. (record+0x570 mask AND *descriptor+0x18) == 0 -> skip.
  2. node = *(record2f0+0x18); skip when *(node+8)==0 or
     *(char*)(record2f0+0x298)==0 (a byte flag; the probe's u32
     "count298" had low byte 0x81 on every captured record).
  3. (record+0x688 AND 0x7ff0) != 0 and *(node+0x21)==0 -> skip.
  4. descriptor+0x58 byte set and FUN_142852f80(record) -> skip;
     FUN_142852f80 = (flags AND 0xff0) != 0 (bits 4..11).
  5. descriptor+0x49 byte set and FUN_142854140(record) -> skip;
     FUN_142854140 = (flags>>12) AND 0xffffff01 != 0 (bit 12, or >=20).
  6. LOD distance test on descriptor bounds, then FUN_1404f4e10 twice --
     a plane-loop frustum test (planes at *(record+0x30), count
     *(ushort*)(record+0x44); -1 = fully outside).
  The observed 0x688 churn IS this machinery: the count-up chains are
  evaluation-progress states and the gates skip records mid-sequence.
  Intra-frame dedup, not cross-frame stasis. Cross-frame stasis remains
  the +0x268 hash's job; content-tracking still unconfirmed (needs a
  capture with known movers).
- CORRECTION: kinematic-job-3 (0x42DF530) was not refused for being a
  thunk. Its first instruction is 80 FA 19 (cmp dl,0x19), which
  codeInstructionLength does not recognise. Decoder extension deferred:
  physics jobs measured <=0.05 ms/frame, so the missing bracket has no
  decision value now.

### 2026-09-19 21:23 review (Sean): four corrections and a shortcut

Checked against the cited evidence before recording; all four stand.

1. +0x268 is a RENDER-CONFIG hash, not a motion signal. FUN_14433C750
   (decomp_433C750.txt; called from the record filler FUN_14432A4E8)
   mixes *(helper+0x1A0), *(helper+0xF0) and a helper table (count at
   helper2+8 >>0x11 AND 0x7ff, entries at +0x10) -- no transform reads.
   A moving object with unchanged render config keeps the same hash.
   RETRACTED: +0x268 as justification for static classification or
   zero-motion injection, and the phase-0 "movers capture proves the
   hash" plan (moot: it cannot prove motion content).
2. "hash unchanged across 1.4M calls" was never established: stored
   transitions capped at 8192 with 1436052 overflowed, and the probe
   counts flags-or-hash transitions jointly (no hash-only counter).
3. "9.4 ms of the 15.9 ms frame" overstates: summed job durations can
   overlap across worker threads; the observer takes the probe mutex
   thousands of times per frame inside the measured jobs; job-3
   (PrePhysicsAdvanceJob) is unmeasured. Render-data jobs are a
   candidate, not proved dominant. Remeasure with the detailed observer
   disabled and job-3 hooked.
4. Skipping LOD/frustum for unchanged records would be incorrect: those
   decisions are view-dependent (camera/head move while buildings stand
   still). The valid lever is avoiding rebuild of unchanged object DATA
   while preserving view-dependent decisions. Zero OBJECT motion must
   retain camera/head motion in the final MVs.
5. Identity: 2633 records share 510 node pointers, so node alone cannot
   key per-record history; projected bounds + depth gating do not
   uniquely own pixels. Unresolved before any MV overwrite.
6. Shortcut to trace offline: writer -> collection -> collection+0x18 is
   a backing-owner pointer, possibly the KinematicRig itself; proving it
   could avoid the planned submission hook.
7. Architecture simplification (verified temporal_pass.cpp dlaaEvaluate
   hand-off): NGX already consumes EDVRs own e.dlMv -- no separate
   game-DLSS-path replacement; the diagnostic MV view and DLSS consume
   the same corrected composition.

### 2026-09-19 21:58 — offline: collection+0x18 resolved as backing-owner pointer; transform chain mapped

Ghidra waves 11-17, no flight spent; exe hash unchanged. Dumps cited are
in analysis\decomp (gitignored).

**Ownership chain (Sean's writer -> collection -> collection+0x18 shortcut):**

- rig+0x348 (collection) is only zeroed by the rig ctor (funasm_430B2A0.txt
  0x14430B8D7, R15 = 0, one of ~30 zero stores) and lazily allocated by the
  attach fragment FUN_14431B4D5 at 0x14431B650:
  FUN_14430A060(mem = factory DAT_146034500, param_2 = ([[rig+0x1D8]]+0x60)(),
  param_3 = R15 (live-in), param_4 = [rig+0x180], flags).
- Collection ctor FUN_14430A060 (decomp_430A060.txt line 47):
  `collection+0x18 = param_3`. This is the ONLY writer of +0x18 (attach's
  two rig+0x348 stores carry no +0x18 write; confirmed by full-asm listing).
  +0x18 is immutable for the collection's lifetime; the collection dtor
  FUN_14430DA30 never reads it.
- The ctor aliases seven owner arrays into the collection (+0xD8=owner+0x110,
  +0xF0=owner+0xA0, +0x148=owner+0x180, +0x178=owner+0x1F0,
  +0x1A8=owner+0x260, +0x2B0=owner+0x2D0, hash-key seed owner+0x340) and
  sizes its open-addressing tables from owner+0x388. The dtor actively frees
  list nodes back through collection+0xD8 = owner+0x110
  (decomp_430DA30.txt line 86), so the aliasing is live, not vestigial.
  Conclusion: collection+0x18 is a deliberate backing-owner pointer; the
  collection is a view over the owner's scene-graph state. Sean's structural
  claim CONFIRMED.
- The same owner value (R15) is handed to FUN_144319460(rig, owner)
  immediately before allocation (0x14431B587): it walks [owner+0x10]'s node
  tree (children +0x88/+0x90 stride 200, components +0xB8/+0xC0 stride 0x18)
  and registers components into the rig+0x310 map and rig+0x330/0x338
  vector. The owner is a scene-graph-bearing object.
- The reinit branch stores the same R15 to collection+0x20 (0x14431B670)
  while the ctor path sets collection+0x20 = param_2 = vtable slot +0x60 of
  [rig+0x1D8] — consistent with owner == that virtual result, i.e.
  collection+0x18 and +0x20 alias the same object.
- NOT proven: owner == KinematicRig. The attach is a dispatcher fragment
  entered through pair-slot 0x1462B4724 (dword pair 0x431B4D5/0x431B6C9; no
  static references — dispatch is computed), so R15's provenance is behind
  the dispatcher. Runtime discriminator for the next instrumented flight:
  owner = *(collection+0x18); check *(owner+0x348) == collection AND
  *(uint*)(owner+0x380) == 4 (rig render-ready state). Both true means
  owner IS the rig and the planned submission hook can be retired.
  Cost: two dereferences in the existing probe; saves the hook entirely.

**Transform/dirty chain (corrects the earlier "dirty bitmask" framing):**

- FUN_14431AFE0 accumulates uVar9 = OR of LOD-entry masks (entry+0x570;
  table at poseCtx+0x40 stride 0x6A0, count poseCtx+0x1A940) over entries
  passing render-state gate FUN_142840840 and the eval gates; exclusion
  masks poseCtx+0x1A950/58/60 selected by collection+0x70 bit-7 class.
  uVar9 lands at descriptor+0x48.
- FUN_144331300 (0x4331300) is a per-frame LOD evaluator, not a dirty-flag
  reader: for each entry in uVar9 it recomputes the record world center
  (local via [record+0x190] x parent world via matrix+0x50) and distance,
  picks a LOD level from the threshold array, writes a 4-bit nibble per
  entry and ORs the entry bit into the traversal mask (desc+0x50). Epoch
  check: desc+0x38 (= [collection+0x90]) vs record+0x1B8; mismatch calls
  FUN_14433C870(record).
- FUN_14433C870 (0x433C870) refreshes the record pivot/aggregate bounds:
  copies node position ([[record+0x18]+8]+0x20/+0x2C) to record+0x270..0x28C,
  accumulates into pose ctx +0x80, recurses children (record+0x2A8 array,
  +0x2B0 count) composing child-minus-parent centers.
- Traversal FUN_144312040 walks the mask bit-by-bit into
  FUN_14433DB20(record, filteredMask, 0) per active node.
- FUN_14433DB20 is the per-record world updater: writes world translation
  record+0x170..0x178, packed scale +0x17C, transformed center
  record+0x240..0x24C, quaternion to the pose ctx via FUN_1442B6410,
  current bounds record+0xF0..0x118, then copies current -> previous at
  record+0x1C0..0x1F8 (lines 506-513); cull path FUN_142846540 +
  record+0x2E8 = 1 when the record+0x2C8 object's mask misses.
- The "64-bit dirty bitmask" of earlier notes is recomputed every dispatch
  from distances and visibility masks; no stored per-record dirty flag
  participates in this path. The engine re-evaluates every in-mask record
  each frame — consistent with CPU frame time scaling with scene size
  rather than with change (engine-render-performance doc).
- FUN_144320340 (batch) iterates worker descriptors at collection+0x40
  stride 0x60, count collection+0x300; fields match the unwind findings
  (desc+0x00 pose ctx, +0x28 records, +0x30 count, +0x40 registry).

**Implications for kinematic-motion-injection phase 0:** record+0x170/0x240
world data and the +0xF0/+0x1C0 current/previous bounds pair are the
engine-side motion truth the design needs; the epoch pair (collection+0x90
vs record+0x1B8) is a genuine engine change signal worth capturing, unlike
the retracted +0x268 render-config hash. The owner==rig discriminator above
is the last open hop for retiring the submission hook.

### 2026-09-20 04:30 — instrument: owner discriminator + epoch pair (installed to frontier)

Build v0.17.0-82-gba373645-dirty (6AAFB3C8; tree == commit 8075da75)
installed to frontier and verified; INIs untouched. Implements the runtime
check the 21:58 offline trace named, so the NEXT flight can settle
owner == KinematicRig and validate the epoch change-signal in one session.

What was added (kinematic_eval_probe/hook):

- noteOwnership at job-0/1 body entry, OUTSIDE the QPC bracket (job-cost
  attribution stays clean). Job 0 (UpdateRenderDataJob body RVA 0x4321940)
  arg = FUN_14431AFE0 descriptor: collection = *(arg+0x10) - 0x300 (its
  +0x10 is the ADDRESS of collection+0x300). Job 1 (batch 0x4320340) arg =
  the collection itself. Captures collection+0x18 (owner), +0x20 (alias),
  +0x90 (epoch), *(owner+0x348) (backptr), *(uint*)(owner+0x380) (state),
  deduped per collection (cap 64).
- Discriminator verdict: owner == rig iff backptr == collection AND
  owner_state == 4 across the captured collections. Consistent
  backptr != collection refutes it; alias20 == owner would confirm the
  +0x18/+0x20 aliasing inferred from the ctor/reinit paths.
- Epoch pair in observe(): record+0x1B8 vs descriptor+0x38 with
  epoch_matches / epoch_mismatches / epoch_changes counters, validating
  the FUN_144331300 epoch semantics as a genuine per-record change signal.
- Failure modes are distinguishable from success in the JSON:
  ownership_checks == 0 with empty ownership[] = job hooks never fired;
  climbing read_faults with no ownership rows = the job-0 descriptor
  layout assumption failed; backptr == 0 = null owner. None reads as a
  pass.

Flight protocol: unchanged (eye dump, 30 s per the previous protocol).
After: python tools\edvr_log.py --target frontier --expect-build
v0.17.0-82-gba373645-dirty, then read kinematicEval.ownership and
kinematicEval.summary epoch_* / owner_* counters.
### 2026-09-20 05:05 — flight 043344: owner==rig REFUTED, epoch pair dead as captured

Flight edvr_gfx_20260920_042813.log, eye run 043344, mesh frames
26311-26314, build verified --expect-build v0.17.0-82-gba373645-dirty
(6AAFB3C8). kinematicEval: 1,567,565 observations, 2,882 records,
read_faults 0, ownership_checks 39,289. Data bug first, self-inflicted:
my record writer dropped the closing quote on desc_epoch38
(kinematic_eval_probe.cpp:283), corrupting every record line; repaired
offline (2,882 regex repairs), fix built green and installed below.
Probe-JSON validation has no build-gate self-test — same bug class as
b70e3d0; noted, not fixed here.

ruled out: collection+0x18 == KinematicRig, because all 64 captured
collections share ONE heap owner 0x144eae6bb60 with *(owner+0x348)==0
and *(owner+0x380)==3940990720 (!= 4). +0x18 points at a shared global
(scene-graph-world-like singleton), not a per-rig object, so the
submission capture is NOT retired. The job-0 reads themselves were
validated: collections come in 0x630-stride families (0x145139xxxxx,
0x14821d6xxxx), matching the collection size, so
collection = *(arg+0x10) - 0x300 is sound. Address note: the
0x144…/0x145… 11-digit values here are high heap (~1.4 TB), not
in-image ([0x140000000, 0x14640C000)); only vtables resolve to RVAs.

ruled out: +0x18/+0x20 aliasing, because alias20 != owner on all 64
rows (alias20 varies per collection group: 0x1450003df00,
0x14514b99c10, 0x148b0acdd30, 0x144e6f211e0, 0x144fb323290).

ruled out: the epoch pair as a change signal AS CAPTURED, because
record+0x1B8 is a small enum-like constant (values 0-8; nonzero on
778/2,882 records) that never changed in 1,567,565 observations
(epoch_changes 0, epoch_matches 0), and collection+0x90 == 0 on every
row. The eval-hook descriptor's +0x38 holds a heap POINTER
(0x14511b279e0), not [collection+0x90], so the eval hook's arg is not
FUN_14431AFE0's job descriptor and the offline epoch semantics do not
transfer to it. All ownership rows read job:0 because dedupe keeps
first-sighting jobId; job 1 did fire (RenderDataBatch 6,113 calls).

Job timings (observer ON — include observer mutex cost, not clean
engine cost): UpdateRenderDataJob 33,176 calls mean 44.6 us max 26.5
ms; RenderDataBatch 6,113 mean 108.2 us max 2.8 ms;
UpdatePhysicsObjectsJob 1,144 mean 6.8 us; Unnamed_BA0 2,402 mean 0.5
us. PrePhysicsAdvanceJob 0 calls EXPLAINED: CodeHook refused job-3 at
04:33:44.645 (target begins with a jmp/call — linker thunk or foreign
hook; "Not hooked, and nothing was changed"). PrePhysicsAdvanceCurveJob
was hooked but never invoked in the window; physics remains partly
unmeasured. transitions 8,192 kept / 1,556,491 overflowed — the cap
Sean flagged 21:23 still stands; "unchanged" claims remain unprovable
from this instrument.

Next: hook FUN_14431AFE0 (RVA 0x431AFE0) directly and capture
rig=param_1 with poseCtx=param_2, paired with *(rig+0x348). That is
the direct rig<->collection link the job descriptor cannot give, and
it does not depend on +0x18 being per-rig (it is not).

Build v0.17.0-85-ga02e1eee-dirty installed to frontier and verified
(adds only the desc_epoch38 JSON quote fix); INIs untouched. Flight
check: --expect-build v0.17.0-85-ga02e1eee-dirty.

### 2026-09-20 05:15 — flight 050820: JSON fix flight-proven, refutation replicates

Flight edvr_gfx_20260920_050630.log, eye run 050820, build verified
--expect-build v0.17.0-85-ga02e1eee-dirty (6AAFBC12).
classification_050820.json parsed CLEAN, no repair — the desc_epoch38
quote fix is flight-proven. kinematicEval: 1,729,224 observations,
3,066 records, read_faults 0, ownership_checks 39,936.

The 05:05 verdict replicates: 58/64 ownership rows share ONE heap
owner (0x174421585a0 this session — the address moves with the heap,
the pattern does not), backptr 0 on all 64 rows, owner_state never 4.
owner==rig stays refuted. epoch_changes 0 again; epoch1b8
nonzero-but-constant on 936/3,066 records.

New detail from the 6 job-1-first rows (collection family 0x529b…, a
low-heap family distinct from job-0's 0x17x…): 4 have NULL +0x18; the
other 2 have unique owners with alias20 == owner+0x270 exactly — the
fixed intra-owner array aliasing the ctor (FUN_14430A060) implied —
and collection+0x90 is small-nonzero (1-2) only on this family. No
rig link anywhere in either family.

Job timings replicate the 04:33 flight's shape (observer ON, same
caveat): UpdateRenderDataJob 33,696 calls mean 45.1 us max 25.0 ms;
RenderDataBatch 6,240 mean 107.5 us; UpdatePhysicsObjectsJob 1,248
mean 8.1 us; PrePhysics pair 0 calls (job-3 unhookable, CurveJob not
invoked); Unnamed_BA0 2,593 mean 0.6 us. transitions 8,192 kept /
1,699,570 overflowed — cap unchanged.

### 2026-09-20 05:30 — instrument: direct rig-link hook on FUN_14431AFE0 (installed to frontier)

Build v0.17.0-89-gbbfcded7-dirty installed to frontier and verified;
INIs untouched. Implements the reachability the 05:05 entry named: a
CodeHook "kinematic-rig-eval" on FUN_14431AFE0 (RVA 0x431AFE0) itself
— the per-rig render/update routine whose param_1 IS the rig
(decomp_431AFE0.txt: two-param signature, +0x380 state check first).

What was added (kinematic_eval_probe/hook):

- noteRigLink at FUN_14431AFE0 entry, before the original runs, on
  the same observer gate as the eval/job hooks. Captures per rig
  (deduped, cap 256): rig, poseCtx (param_2), collection =
  *(rig+0x348), gameObj = *(rig+0x20), descriptor = *(rig+0x50),
  rigState = *(uint32*)(rig+0x380), and from the collection: owner =
  *(collection+0x18), collEpoch90 = *(collection+0x90).
- The dump joins each row's collection against the ownership capture
  (collection_known): a hit closes rig -> collection -> shared-global
  owner in one flight, tying rigs to the job-0/job-1 collection
  families without any unwinding.
- Failure modes distinguishable from success: riglink_checks == 0 with
  status "installed" = the hook stood down (CodeHook logs the reason
  under kinematic-rig-eval at arm) or FUN_14431AFE0 genuinely never
  ran in the window; climbing read_faults = the +0x348/+0x380 layout
  assumption failed; riglink_null_collection climbing = rigs without a
  collection set (late init) — data, not a fault. None reads as a pass.
- Cost: one hash lookup per call after the first sighting; guarded
  reads only on new rigs plus two per known-rig call. No QPC bracket
  added — job-cost attribution untouched.

Flight protocol: unchanged (eye dump, 30 s). After: python
tools\edvr_log.py --target frontier --expect-build
v0.17.0-89-gbbfcded7-dirty, then read kinematicEval.riglinks and the
riglink_* counters.

### 2026-09-20 05:45 — flight 054002: rig -> collection CLOSED; job-1 arg is not a rig collection

Flight edvr_gfx_20260920_053816.log, eye run 054002, build verified
--expect-build v0.17.0-89-gbbfcded7-dirty (6AAFC286). Parse clean.
riglink_checks 81,744, read_faults 0, riglink_state4 == checks (every
call render-ready), null_collection 0. Window: 52 frames starting
frame 11327 — every stored row has first_frame 11327 and exactly 52
hits, so FUN_14431AFE0 runs once per rig per frame and the rig set is
STABLE: 256 stored + 68,432/52 overflow = ~1,572 distinct rigs, no
churn. (kRigLinkCap 256 censuses only ~16% of rigs — raise it if a
full census ever matters; noted, not changed.)

confirmed: the rig -> collection link, because all 58 job-0 ownership
collections appear as some rig's *(rig+0x348) (join saturated at the
64-row ownership cap), and all 256 sampled rig collections point at
the session's ONE shared global owner 0x1737817f7d0 — the same
shared-global pattern the job side sees. The chain is proven end to
end: rig -> collection (+0x348) -> shared global owner (+0x18), and
each rig has its OWN collection (0x177… family on both sides).

ruled out: the job-1 (batch) descriptor arg being a rig collection,
because its 6 collections are a separate low-heap family (0xa12…, the
SAME family as the shared pose/render context 0xa1197a6f0), with null
or unique owners (alias20 == owner+0x270) and zero rig joins.
FUN_144320340's descriptor arg is a batch-side structure, not
*(rig+0x348).

param_2 is ONE shared render context for all rigs (a single
0xa1197a6f0 across all 256 rows; matches the decomp's render-record
context: records at +0x40 stride 0x6A0, count at +0x1a940); it does
not match record+0x290. Rig descriptors are shared type objects (22
distinct across 256 rigs); game_obj (rig+0x20) is 1:1 with rig.

Consequence for kinematic-motion-injection phase 0: per-rig
reachability is SOLVED — enumerate rigs at FUN_14431AFE0, read
*(rig+0x348), walk collection records (+0x280, stride 0x2F0) to the
record+0x130..0x16C world transforms. Remaining phase-0 opens:
per-record identity (~5 records share one node pointer), pixel
ownership, and the observer-off job-cost remeasure.

### 2026-09-20 06:10 — instrument: per-record world-transform snapshots (installed to frontier)

Build v0.17.0-93-gd6ed264f-dirty installed to frontier and verified;
INIs untouched. Adds the engine-truth motion capture the 05:45 entry
made reachable — in observe(), NOT by walking collection arrays from
the rig side: the eval hook already sees the same stride-0x2F0 records
deduped, so no new layout assumption (array base/count) was needed,
and the riglink rows already attribute collections to rigs.

What was added (kinematic_eval_probe only; no new hook):

- Per eval call, one 64-byte guarded read of the world-transform
  block record+0x130..0x170 (the doc's 3x4 at +0x130..0x16C sits
  inside it; +0x170 is the next field). Bit-exact compare against the
  record's latest snapshot; on change: copy, ++xfChanges, stamp
  last_xf_change frame. New records store xfFirst + xfLatest.
- Summary: xf_movers (records with >=1 change), xf_changes (total
  change events). Per record in JSON: xf_changes, last_xf_change,
  xf_first[8], xf_latest[8] (raw uint64 bits as hex strings —
  float-exact; threshold offline).
- A record with 0 changes across the window is engine-proven STATIC
  in it; a record changing every frame is a mover. This is the
  classification the +0x268 render-config hash (retracted) and the
  epoch pair (constant enum) could never give.
- Failure modes: read_faults climbing = the +0x130 block assumption
  failed; xf_changes == 0 everywhere with read_faults 0 = a window
  with no motion (landed cockpit) — confirm against a station session
  before calling the instrument dead. Neither reads as a pass.
- Cost: one 64-byte guarded read + memcmp per observe() call, same
  mutex as before. No new hook, no QPC bracket.
- Process note: the writer edit needed a byte-level repair pass after
  an escaping mishap during editing (44-line collateral, fully
  reverted, verified at byte level); the build gates then passed.
  Probe-JSON validation still has no build-gate self-test.

Flight protocol: unchanged (eye dump, 30 s). After: python
tools\edvr_log.py --target frontier --expect-build
v0.17.0-93-gd6ed264f-dirty, then read kinematicEval.summary.xf_* and
records[].xf_*.

### 2026-09-20 06:30 — flight 061722: zero movers in 2,640 records, but a drone was moving; writer bug #3 fixed, coverage extended

Flight edvr_gfx_20260920_061543.log, eye run 061722, build verified
--expect-build v0.17.0-93-gd6ed264f-dirty (6AAFCC46). The report did
NOT parse: my xf writer edit had dropped the closing quotes after
epoch1b8 and desc_epoch38 — the line-leading \" idiom (each literal
following a hex value must open with \" to close the PREVIOUS field)
was stripped on exactly those two lines. Repaired offline (5,280
fixes = 2 fields x 2,640 records). Third incident of this bug class
(b70e3d0, desc_epoch38, now this) — a build-gate JSON self-test for
the probe writer is formally proposed to Sean.

kinematicEval: 2,640 records, read_faults 0, xf_movers 0,
xf_changes 0 — every record's +0x130..0x170 transform block
bit-identical across the 52-frame window; 0 hash changes across
8,192 kept transitions (820 records churn flags only — the
view-dependent flag class, not object motion). Sean confirms a drone
was VISIBLY MOVING in the scene, so "all static" cannot be taken at
face value. Three explanations, in order of likelihood:

1. The drone's per-frame motion lands in the updater's translation
   output at record+0x170 (FUN_14433DB20) — the field the 64-byte
   block stopped just short of. Coverage extended to +0x130..0x180
   (80 bytes) in build v0.17.0-95-g873c4024-dirty; the next flight
   discriminates.
2. The drone is not in the kinematic eval population at all
   (skinned/animated actors move through a different pipeline — the
   hidden-bone class that defeated pool estimation); its record
   would then never appear in kinematicEval.records.
3. The drone's record exists but its transform is composed
   parent-relative (the parent rig moves; child locals stay
   constant) — parent-relative composition is already established
   for the +0x1C0 interface (slot +0x170 writes a parent-relative
   3x4 pose to rig+0x58).

If the extended block still shows zero changes with a known mover in
view, explanation 1 is dead and the question becomes population
membership (2 vs 3): find the drone's record via its game object
(rig+0x20 is 1:1 with rig; the drone's rig is one of the ~1,572).

Also from 061722: record calls cluster at 552 (2,612 records), 340
(14), 117 (14) — ~10.6 eval passes per record per frame, plus 28
partial-window records (streamed in/out or intermittent evaluation).

Build v0.17.0-95-g873c4024-dirty installed to frontier and verified;
INIs untouched. Flight check: --expect-build
v0.17.0-95-g873c4024-dirty.

### 2026-09-20 06:45 — flight 064047: the drone FOUND at record+0x170; +0x130 matrix is static

Flight edvr_gfx_20260920_063858.log, eye run 064047 with the drone in
motion, build verified --expect-build v0.17.0-95-g873c4024-dirty
(6AAFD0FB). Parse clean — the writer quote fix is flight-proven.
xf_movers 338 of 2,909 records, xf_changes 16,316, read_faults 0.

Per-slot analysis across all movers: slots 0-15 (the +0x130 4x4)
NEVER changed for ANY record; slots 16-19 (+0x170..0x180) changed
for 322-329 records each. Bit churn includes the -0.0/+0.0 sign
class (the top-by-changes record's only delta was a zero sign bit),
so bit-compare alone overstates motion; ranking by NET displacement
separates real movers:

- Top 11 records all moved ~2.59 m as ONE rigid group (positions
  clustered at (-65..-68, 60..62, -188..-190)) — the drone, as ~11
  sub-mesh records including two pairs sharing exact start positions
  (the ~5-records-per-node sharing). 2.59 m over the 52-frame window
  (~4.5 m/s at 90 Hz): drone flight speed.
- One further record at 1.25 m elsewhere; 125 of 338 movers exceed
  1 cm net displacement; median mover displacement 5.8 mm
  (bobbing/rewrite class).

ruled out: record+0x130..0x16C as the per-frame world transform,
because with a known mover in view the 4x4 stayed bit-static for
every record while +0x170 moved. It is a static local/default
matrix; the motion-injection doc's "3x4 at record+0x130..0x16C =
current-frame transform" is corrected accordingly.

confirmed: record+0x170 (3 floats; +0x17C spare/flag) is the
per-frame world-position field — the engine-truth motion signal for
the kinematic MV source: object motion = per-frame delta of
record+0x170. The drone is IN the kinematic eval population (06:30
explanation 1 confirmed; 2 and 3 unnecessary for it). Remaining
phase-0 opens stand: per-record identity (sub-mesh records sharing
positions/nodes), pixel ownership, observer-off job-cost remeasure.

### 2026-09-20 06:55 — phase-1 spec written (no code)

The diagnostic kinematic motion source is specced in
kinematic-motion-injection-2026-09-19.md (2026-09-20 section):
tracker on the eval hook's record stream, record+0x170 deltas,
bounds+depth ownership, compose-into-camera MVs, config proposal
fix.engine_motion on|off|auto (default off), temporal_aa_debug
verification gate before DLSS sees anything, v1 = translation only
(rotation needs the node matrix, unverified). The doc's injection
table and phase-0 bullet were updated for the 064047 proofs. No code
yet — Sean picks when phase 1 gets built.


### 2026-09-20 07:45 — rotation decoded, spec revised per review, JSON gate catches a live writer bug

Sean's 07:03 review (four corrections) is worked into the phase-1 spec
(kinematic-motion-injection-2026-09-19.md): static claims are bounded
per-frame evidence, never permanent labels; the tracker keeps
frame-aligned pose history with identity re-established from the live
stream every frame; unknown identity / ambiguous coverage / overflow
PRESERVES the existing motion path instead of defaulting to static-zero;
pixel ownership is a ship prerequisite.

Rotation DECODED offline (review item 2): updater FUN_14433DB20 writes 8
bytes at record+0x17C = 4x uint16 lanes, component = (lane - 32768) /
32767; degenerate input maps to (0,0,0,65535) = identity. Runtime
scale/bias constants are RAM-initialized (garbage in the disk image);
the clamp constant 65535.0 x4 at RVA 0x5286470 and the (0,0,0,1.0f)
select table at 0x50C80C0 read clean from the exe. Verified on the
064047 capture: the drone's lanes tracked its yaw (0.2134 -> 0.1914)
while statics stayed bit-constant. Sign canonicalization (q == -q) is
still required before differencing two quats. Capture extended to
+0x130..0x188 (xf arrays 10 -> 11 qwords) so the full 8-byte quat word
rides the first/latest snapshots.

JSON writer gate, per Sean's "three serialization failures are enough"
directive: KinematicEvalProbe.selfTestPopulateForJson() fills a
deterministic fixture; tools\kinematic_json_test\kinematic_json_test.cpp
serializes it through the production writeJson (hook functions stubbed,
the mesh_motion_test pattern); tools\kinematic_json_selftest.py
strict-parses the output and asserts every fixture value round-trips;
wired as :rig_kinematic_json_test in build.bat. FIRST RUN caught a live
production bug: writeJson emitted epoch1b8 and desc_epoch38 with the
comma INSIDE the string value ("0x1122...,") — the line-leading literal
closed the quote AFTER the comma instead of before it. Valid JSON, wrong
values: a parse-only check would have passed. Two-line fix, verified by
the gate. Every capture written since the epoch fields landed carries
the trailing comma inside those two strings; the values themselves are
intact and remain readable.

Build v0.17.0-101-g39e6a422-dirty installed to frontier (BE7E7188); all
gates green, including the new JSON gate. INIs unchanged.

### 2026-09-20 08:22 — instrument: frame-aligned pose/identity capture (installed to frontier)

KinematicEvalProbe now takes one pose sample per record per frame,
riding the xf[11] block the re-seen path already reads (no extra guarded
reads for the transform; first-sight records sample from xfFirst).
Decoded per the flight-verified layout: tx/ty/tz = 3 floats at
record+0x170, q0..q3 = 4 raw uint16 lanes at +0x17C..0x184 (decode
offline). Per record: frames_sampled, dup_in_frame (second observation
of the same record in one frame), gaps/max_gap, node_changes,
quat_change_frames (8-byte quat bit-compare), max_jump/total_jump (3D
translation distance vs previous sample). Two identity events, bounded
at 256: kind 1 = gap-resume (record unseen for gapLen frames, then
re-seen), kind 2 = node-change (node pointer changed while the record
pointer stayed continuously seen -- the slot-reuse probe for the ~5
records sharing one node pointer). A bounded (8192) mover sample log
appends on first-ever sample, any change in the 20 pose bytes, or the
first sample after a gap. setFrame() flushes per-frame record counts
into frames_counted/zero_record_frames/min/max_frame_records (the first
flush after arm counts a partial frame -- acceptable). All of it rides
the existing eye-dump lifecycle (arm/finish/reset/setFrame/writeJson);
no new config keys, no rendering change.

JSON gate extended in the same change, per its own contract: the
fixture gained the per-record frame-sample stats, 2 PoseSamples and 2
IdentityEvents, and tools\kinematic_json_selftest.py asserts the new
summary counters, the pose_events array and the mover_samples array
(translation as raw 32-bit hex strings, quat lanes as ints) with exact
dict equality. Gate green on first run.

Dead-instrument check: framesCounted increments in setFrame and
poseSamples in observe, both already-live paths, so a zero reads as a
dead instrument, not as success.

Build v0.17.0-103-g267d0430-dirty installed to frontier (D3CA6EA1); all
gates green. INIs unchanged. Not yet flown.

### 2026-09-20 08:50 — pose/identity diagnostic flown (083323): frame clock refuted, drone series captured

Flight 083323 (eye dump with the drone in motion, 30 s wait), build
v0.17.0-103-g267d0430-dirty verified. 2,949 records, 1.6M evaluations,
zero read faults. Analysis scratch: build\pose-diag-analysis-083323.md.

- Frame clock REFUTED: the mesh-frame counter ticked 3 times in ~51
  rendered frames (every record: calls=545, frames_sampled=4,
  dup_in_frame=541, frame values 13081..13084 only). dup_in_frame
  (1,595,409) is counter staleness, not duplicate evaluation. Gap and
  node-change detection were unreachable by construction — zero
  pose_events is UNINFORMATIVE, not proven stability.
  zero_record_frames=1 shows the tick is decoupled from the evaluator
  sweep. Tracker clock moves to EDVR's own present/temporal-pass frame
  index: it ticks once per rendered frame, the pose data updates on
  that cadence (xf_changes median exactly 51 = one rewrite per frame),
  and it is the clock the MVs are consumed against.
- The capture is a 4-instant pose census: all 2,949 records at tick
  13081, movers only at ticks 13082-84 (276/324/341 re-logged). Up to
  ~35 rendered frames between ticks 2 and 3 are invisible.
- Drone re-found with the 064047 fingerprint: 11 records (2218, 2226,
  2234, 2250, 2252, 2254, 2472, 2475, 2480, 2484, 2492), 11 distinct
  sibling node pointers in two contiguous arrays (correcting the
  one-shared-node note), moving 5.60-5.65 m straight-line (~9.9 m/s)
  with translation AND quaternion changing at every sample. A separate
  67-record rigid mover at ~14 km moved 65.5 m (~115 m/s) — a ship.
- Quaternion decode validated: all 3,890 samples within 5e-5 of unit
  norm. Sign canonicalization IS required before differencing: 3/941
  consecutive pairs (records 2127, 2189, 2325) have negative dots;
  2127 keeps a ~79-degree jump after flipping — genuinely erratic
  rotation exists in the translation-static class.
- record+0x130 4x4 bit-constant for all 2,949 records (zero qword 0-7
  changes) — the static-matrix ruling re-confirmed on this build; all
  341 movers' changes are confined to qwords 8-10 (+0x170..0x188).
- The probed epoch fields are dead as a frame clock: epoch1b8 is a
  small per-record enum (9 distinct values), desc_epoch38 pointer-like
  (547 distinct); 100% mismatch with zero changes.
- Jobs this flight: UpdateRenderDataJob 33,228 calls / 1.462 s /
  44.0 us mean / 24.2 ms max; RenderDataBatch 6,240 / 0.689 s /
  110.4 us; UpdatePhysicsObjectsJob 1,196 / 8.8 ms; Unnamed_BA0 2,533 /
  1.25 ms; both PrePhysicsAdvance jobs 0 calls. ~2.16 s attributed over
  ~0.57 s wall — heavily parallel; still not clean frame-time shares
  (the 21:23 review point stands).

Next flight discriminator: log the mesh-frame counter once per present
frame to learn what it actually counts.

### 2026-09-20 09:35 — instrument: probe re-clocked on the present frame counter (installed to frontier)

Flight 083323 refuted the mesh-frame counter as the tracker clock: it
ticked 3 times in ~51 rendered frames, which stalled every frame-aligned
stat (a "frame" lasted ~17 presents; gap-resume and node-change
detectors were effectively disabled, so zero identity events there is
uninformative). The probe now runs on g_state->frameCounter in
device_hook.cpp — the session's canonical frame number, incremented
exactly once per owned Present before any feature gates — via a new
entry point KinematicEvalProbe.notePresentFrame(presentFrame,
meshClock), called immediately after ++g_state->frameCounter. The
alternative site beside vScreenFrameBoundary sits behind the
graphicsRuntimeDisabled early return and would skip those presents, so
it was rejected; exactly-once-per-owned-present is the hard requirement.
The object_classification_probe setFrame fan-out no longer forwards the
mesh clock to kinematicEvalProbe (objectRecordWriterProbe keeps it,
unchanged); arm() still takes the legacy mesh stamp, which only seeds
frame_ until the first notePresentFrame overwrites it.

Each call also appends {present, mesh} to a bounded (4096) clock_samples
ring — the mesh-staleness discriminator. Gated on active like the rest
of the probe: the log lives and dies with the capture lifecycle. The
mesh counter resets to 0 on every config re-poll (the once-per-second
re-configure runs meshMotionShutdown), so absolute mesh values are only
meaningful between configure events; the per-window ratio is the
signal. Next flight should show mesh:present ≈ 3:51 in a steady window
and the actual tick cadence; if mesh ticks align with configure or
streaming events the ring will show it directly. Failure modes read
clean: if notePresentFrame is never called, clock_samples is empty and
frames_counted is 0 (dead instrument, not success); if the mesh
accessor returns garbage the ratio is nonsense but the present clock
and all frame-aligned stats still work.

JSON gate extended in the same change: fixture gained 2 clock_samples
(same mesh value twice — the staleness signature) and the
clock_sample_overflow summary counter, asserted by exact dict equality.
Gate green on first run.

Build v0.17.0-107-g5fe9c004-dirty installed to frontier (02D09A2A); all
gates green. INIs unchanged. Not yet flown.

### 2026-09-20 10:05 — re-clocked capture flown (094158): clock healthy, eval fan-out ~11x, drone per-frame series

Flight 094158 (same protocol), build v0.17.0-107-g5fe9c004-dirty
verified. 3,086 records, 52 presents, zero read faults. Analysis
scratch: build\pose-diag-analysis-094158.md.

- Re-clock WORKS: clock_samples show mesh = present - 2 exactly, 1:1
  across all 52 presents — the 083323 stall was window-specific (the
  once-per-second config re-poll can reset the mesh clock; inferred,
  not proven). Present clock stays: no gates, no resets.
  frames_counted = clock_samples = 52, zero zero-record frames.
- Eval fan-out is ~11x per record per frame, uniform (3,073 records x
  569 calls; UpdateRenderDataJob 644 calls/present x ~52 records/call
  explains it). First-sample-wins dedup per frame is mandatory —
  confirmed by design, now measured.
- The 3,073 gap events are a STARTUP ARTIFACT: the arm path seeds
  frame_ with the mesh-domain stamp (12262) and the second stamp is
  present-domain (12265), firing gap_len=2 for every pre-existing
  record. True mid-window stream-out: ZERO. One real stream-in
  (records 3073-3085 at present 12302). Fix queued: suppress sampling/
  gap detection until the first present tick.
- node_change_events=0 is now INFORMATIVE: ~160k per-sample node
  compares with the identity path provably live — a continuously
  tracked record pointer never changes node. Pointer identity is
  stable while present; post-gap reuse remains untested (no real
  gaps occurred).
- Drone group (new ids, same fingerprint): 2266, 2276, 2279, 2304,
  2306, 2356, 2359, 2363, 2364, 2371, 2373 — all frames_sampled=52,
  path 4.179 m (members agree to 1.5 mm), net 3.445 m (curving),
  quaternion changes EVERY frame, no sign flips. Two ship groups:
  79 records at 320 m accelerating to ~10 m/present (a departure) and
  101 records at 205 m. Wall-clock speeds not derivable (no wall clock
  in the capture).
- Mover census: 519/531 translation movers, bimodal (12 twitch <=1 mm,
  then 0.1 m up; bands 44/196/99/180 for 0.1-0.5/0.5-2/2-10/>10 m, the
  >10 m band exactly the two ship groups); 315 rotated every frame,
  181 never. record+0x130 4x4 bit-static for all 3,086 records — third
  flight confirming.
- pose_samples saturated: demand 31,514 = 3,086 baselines + 3,073
  spurious startup re-logs + ~25,355 mover changes (~490/frame); the
  8,192 cap died ~6 frames in, 37.5% burned on the artifact. Fix
  queued: arm-seed fix + cap to 32,768 + gap re-log rate limit.
- Jobs: UpdateRenderDataJob 33,502 / 1.664 s / 49.7 us mean / 24.4 ms
  max; RenderDataBatch 6,292 / 0.784 s / 124.7 us; physics 1,248 /
  8.8 ms; PrePhysicsAdvance jobs 0 calls again.

Uninformative flags: identity events past frame one were lost to the
cap saturation; the re-poll -> mesh-stall link is inferred; post-gap
pointer reuse is untested.

### 2026-09-20 10:20 — instrument: arm-seed fix, sample-cap bump, gap re-log backstop (installed to frontier)

Flight 094158 (analysis: build\pose-diag-analysis-094158.md) proved the
re-clock works — mesh == present-2, 1:1 over 52 presents — but two
instrument defects burned the capture. Fix one, the startup clock-domain
seam: arm() seeded frame_ with the mesh-domain stamp, so records first
sampled under it were re-stamped at the first present-domain tick,
firing 3,073 spurious gap_len=2 events (saturating the 256-event
identity log at frame one; true mid-window stream-out was ZERO) and
3,073 spurious gap-resume sample re-logs. All pose sampling — first-
sight baselines, re-seen samples, gap/node-change detection — now waits
for the first notePresentFrame after arm (clockSeeded_); before the
clock is live the probe behaves exactly as the pre-pose-history build,
and first-sight baselines still log once it is (they carry identity
context). Fix two, the saturation: sample demand was 31,514 against the
8,192 cap, which died ~6 frames into the 52-frame window; kPoseSampleCap
is now 32,768 (24 B/sample, ~768 KB), sized to that measurement. Plus a
backstop: gap-resume re-log appends are capped at 256 per frame, with
overflows counted in the new gap_relog_skipped summary counter (the
IdentityEvent log keeps its own cap and overflow counter).

Next flight should read: zero gap events at startup unless real
stream-out occurs, pose_samples surviving the full window, the identity
log no longer saturated at frame one. Dead-instrument reads stay
distinguishable: if the clock never seeds, clock_samples is empty and
frames_counted is 0 (dead clock feed); clock_samples non-empty with
zero pose_samples would mean the sampling path broke while the clock
lives. JSON gate extended (gap_relog_skipped fixture + exact-equality
assertion); green on first run.

Build v0.17.0-111-g3b9fc127-dirty installed to frontier (76ED3EE3); all
gates green. INIs unchanged. Not yet flown.

### 2026-09-20 10:38 — capture v2 flown (103339): arm-seed fix confirmed, samples survive, drone series complete

Flight 103339 (same protocol: eye dump with the drone visibly moving,
then a 30 s wait), build v0.17.0-111-g3b9fc127-dirty verified
(--expect-build exit 0). Eye run 103339 armed 10:33:39.977, report
written 10:33:44.061. 3,008 records, 52 presents, zero read faults.
Analysis scratch: build\pose-diag-analysis-103339.md.

- Arm-seed fix CONFIRMED: gap_events=0 (094158 fired 3,073 spurious
  gap_len=2), identity event log empty and unsaturated
  (identity_event_overflow=0), gap_relog_skipped=0 -- the backstop
  never fired. Pose sampling now starts cleanly at the first present
  tick. Detector liveness is established by 094158 (the same path
  fired there) and by this flight's counters showing inputs flowing
  (24,520 samples written, frames advanced); zero events is correct
  silence, not a dead detector.
- pose_samples survived the full window: 24,520 of 32,768, zero
  overflow -- 3,008 first-sight baselines + ~21,512 pose-change
  samples. 094158's demand was 31,514 against the 8,192 cap and died
  ~6 frames in; ~37% of that demand was the spurious re-logs now fixed.
- Clock healthy, second flight running: mesh == present-2 for all 52
  clock_samples, contiguous presents 11136..11187,
  clock_sample_overflow=0, frames_counted=52.
- zero_record_frames=1 is the TERMINAL present 11187 -- capture
  teardown, not mid-window stand-down. Discriminator: a mid-window
  zero-record frame would have fired ~3,008 gap events on resume; none
  fired. All records span the window uniformly (first_frame 11133 = the
  arm-seed stamp, pose data begins at first present 11136 by design;
  last_frame 11186). No stream-in/out this flight.
- node_change_events=0 over 1,750,656 record observations -- pointer
  identity stable while continuously present, second informative
  flight. Post-gap pointer reuse remains UNTESTED (no real gaps).
- Drone group re-found (new ids, same 11-sub-mesh fingerprint): 2404,
  2410, 2413, 2416, 2419, 2420, 2430, 2459, 2475, 2482, 2490. Rigid:
  member spread 2.063 m at BOTH window ends. Centroid +12.27 m, median
  step 0.181 m/present; rotates every present (median 1.361 deg,
  canonicalized decode; lane deltas far above quantization). Per-frame
  translation+quat series COMPLETE: 51/51 frames for all 11 members.
- Mover census (path over 51 frames): 2,583 records <=1 mm; 296 micro
  (1 mm..0.1 m); 10 at 0.1-0.5 m; 4 at 0.5-2 m; ZERO at 2-10 m; 115 at
  >10 m -- all 115 genuine travelers (net/path ~0.99): two decelerating
  ship assemblies (tight 24+24 cores at ~142 m plus ~56 radius-spread
  companions at 171-203 m; per-present steps decay ~9 -> 2 m, arrivals
  slowing). xf_movers=437, xf_changes=21,945, quat_change_frames=21,033.
- NEW CLASS quantified -- rotating-in-place: 206+86 records translate
  <0.1 m over the whole window but rotate 0.8-1.6 deg/present EVERY
  present (canonicalized decode, max lane delta 1,551 -- orders above
  LSB noise). Phase-1 consequence, now flight-evidenced: a static-zero
  MV gated on translation alone would have mis-labeled ~292 records
  this window. Translation-static != rotation-static; the spec's
  both-required rule is not theoretical.
- Fan-out unchanged: uniform 582 calls/record (~11.2/present x 52),
  dup_in_frame=1,567,168 -- first-sample-wins dedup mandatory.
- Jobs: UpdateRenderDataJob 33,384 calls / 1.774 s / 53.1 us mean /
  25.9 ms max (094158: 49.7 us); RenderDataBatch 6,188 / 0.792 s /
  128.0 us; physics 1,196 / 8.3 ms; PrePhysicsAdvance jobs 0 calls
  again (never hooked). Still WITH the observer -- the observer-off
  job-cost remeasure remains owed (Sean's 21:23 review).
- Unchanged dead signals: epoch pair 0 matches / 1,750,656 mismatches /
  0 changes (ruled out, do not re-propose); transition cap 8,192 still
  saturated (overflow 1,703,360) -- flags/hash stream, not pose data.
- Eye-capture draw-side rig ownership join: 44,139 attempted / 0
  linked -- per the log's own wording, unavailable ownership evidence,
  not a successful empty scene. Eval-side riglinks healthy: 82,680
  checks, all state-4, zero null collections.

Uninformative this flight: post-gap pointer reuse (no real gaps); the
re-poll -> mesh-stall link (no stall occurred); live identity-event
firing (zero real events -- correct silence, see above).
### 2026-09-20 11:20 -- stage A landed: kinematic tracker live, diagnostics only

- Stage split, decided mid-implementation: stage A = tracker +
  diagnostics, ZERO rendering change. Stage B (GPU ownership coverage
  + compose-shader veto) is deferred -- the world-bounds layout is not
  flight-proven and the decomps conflict: FUN_14432CCC0 writes 16
  floats at record+0xB0..0xEC that read as four transformed float4
  rows (decomp_432CCC0.txt:410-425), while updater FUN_14433DB20
  copies a 64-byte current/previous pair at +0xF0->+0x1C0
  (decomp_433DB20.txt:506-513), reads a 4-float change-test block at
  +0x120 (line 398) and writes a world center at +0x240 (line 302).
  Guessing the layout ships an untested hypothesis; a wrong MV is
  worse than none.
- Stage A therefore also captures the raw +0xB0..+0x130 block (128 B)
  and the +0x240 center (16 B) per record, and dumps movers+statics on
  ONE flight so the layout decodes offline: position-like fields shift
  by exactly the +0x170 delta between the part-A and part-B dumps;
  view products move with the head instead.
- Landed: src\d3d11\kinematic_motion.h/.cpp -- cap 4,096, staticRun
  >= 3 of bit-exact stasis over all 20 pose bytes (quat included, so
  the 103339 rotating-in-place class can never label), 1 mm near-miss
  counter, gap/node/read-fault -> new identity with re-prove, no
  eviction, 20 s summaries, 5 s zero-observation stand-down note,
  10 s no-mover note, once-per-session bounds dump (part A on the
  first ended frame with >= 8 movers, part B the next ended frame,
  same table indices). The eval-hook relay is now a shared gate cell
  open while EITHER consumer wants callbacks; the tracker registers a
  fn-pointer observer, so the existing rigs needed no link changes.
  Present tick beside the probe's (device_hook.cpp), configure beside
  meshMotionConfigure (temporal_pass.cpp); fix.engine_motion = off/on
  (default off; auto logs "reserved" and behaves off).
- Gates: kinematic_motion_test.exe 35 checks, 0 failures (9 case
  groups incl. the 103339 rotating-in-place regression); full build
  green, config contract 252 keys. The gate caught one test bug: case
  6 checked eligibility after endFrame, but RecordEligible is a
  within-frame query (lastFrame == live frame) -- the check moved
  mid-frame, tracker semantics unchanged.
- End-to-end trace for the next flight: with fix.engine_motion = on
  the log shows "engine motion: tracker live"; 20 s summaries carry
  tracked/seen/eligible/movers; "STAND-DOWN -- five seconds live with
  zero eval" means the hook refused or is not feeding; "ten seconds,
  N tracked records, zero pose changes" means the feed is healthy and
  nothing is moving. Absence of the tracker-live line = the tracker
  never ran. install_edvr.py does not touch the live ini --
  engine_motion = on must be set in the game-dir edvr.ini by hand
  before flying.
### 2026-09-20 12:45 -- review fixes: nine findings, six under build-gate reproduction

Sean's review of 6899d0f (native harness reproductions for five) found
nine issues in the stage-A code; all fixed here, six now pinned by
build-gate cases:

Tracker (kinematic_motion):
- Same-frame dedup returned before the identity/pose reads, so an
  eligible record kept its static label through a node swap or pose
  change inside one Present. Dup observations now verify node + pose
  and invalidate exactly like a cross-frame change (new
  same-frame-invalidations count in the 20 s summary). Rig cases 9-11.
- The one-shot bounds dump fired on the first frame with >=8 movers --
  frame 2 of a session, before any static can be eligible -- and
  completed with zero static comparisons. Part A now waits for BOTH
  populations (>=8 movers AND >=8 eligible statics in one ended
  frame); a one-time 20 s note logs if it never fires, naming the
  last-frame counts. Rig case 12 via the new
  kinematicMotionBoundsDumpStage introspection.

Probe (kinematic_eval_probe):
- A faulted first transform read committed a zero pose baseline and
  recovery fabricated a 100 m mover; faulted hash reads emitted false
  transitions to zero and back. Record creation now aborts on ANY
  faulted field read (no state from partial reads; the record is
  created on a later fully-readable call), and a faulted hash keeps
  the last valid pair with the compare skipped.
- A detected node change kept the previous occupant's pose baseline:
  the boundary was charged as a 90 m jump plus an xf_mover. observe()
  now reads identity BEFORE the motion compares; a node change clears
  hasPrevSample and re-baselines xfLatest uncharged. The kind-2
  identity event (both nodes) is retained.
- The first present tick flushed the pre-seed mesh-domain stamp,
  fabricating an empty frame (zero_record_frames +1, min forced to 0).
  The clock now seeds WITHOUT flushing, as the tracker does.
  Consequence for flown captures: every zero_record_frames count since
  the re-clock (094158, 103339) carries this +1. 103339's sole empty
  frame is consistent with the artifact alone -- the terminal-teardown
  attribution is unproven, but the substantive reading stands (a
  mid-window gap would have fired ~3,008 gap events; none fired).
- Non-finite pose floats could reach max_jump/total_jump and print
  invalid JSON. Non-finite jumps are now rejected from the
  accumulators (raw bits kept; new non_finite_pose summary counter,
  round-tripped by the JSON gate).

Hook (kinematic_eval_hook -- fixed by construction; the races need a
104 MB fake module to exercise, no new rig):
- Both detaches now hold g_installMutex and recompute the relay gate
  from BOTH consumer cells; the probe-detach/tracker-attach
  interleaving that left a live tracker deaf is closed.
- Probe-side executable validation moved INSIDE the locked attach;
  ready is published as an atomic only after relay+trampoline are
  live. validateExecutableLocked is gone.
- Job brackets commit between seqlock toggles and only into the
  capture generation they started in (clearLocked bumps it); writeJson
  reads each job seqlocked. No drain -- the finishing thread may
  itself be inside an observed job.

Gates: kinematic_motion_test 52 checks (was 35), new
kinematic_probe_test 25 checks, kinematic_json gate round-trips the
new key, full build green (71 jobs, config contract 252 keys).
Build: v0.17.0-119-g6899d0ff-dirty installed to frontier (CFDEFA79);
supersedes v0.17.0-117-g0d042f5c-dirty (2AAA67BF). INIs unchanged.
### 2026-09-20 13:05 -- flight 125207: tracker clean, +0xB0..+0x130 decoded as the world transform

- Build check: v0.17.0-119-g6899d0ff-dirty (6AB0284E) -- the
  review-fix build, fix.engine_motion=on in the live ini. (The 12:46
  flight had the key under [advanced]; the unread-keys log line caught
  it and that flight is not evidence.)
- Tracker live from launch (12:52:07.703). Summaries every 20 s; final
  tally: observed 132.5M, dup 120.2M, faults 0, overflow 0, node
  changes 0, same-frame invalidations 0 -- the dup-invariance
  assumption the finding-1 fix defends held universally. 31 gap drops
  late (session end). The 10 s no-mover note fired correctly during
  the menu phase (8 records, still scene).
- Eligible set 2,571-2,636 per frame vs the 103339 census's <=1 mm
  band of 2,583 -- the bit-exact rule labels the population the
  offline census predicted. Near-miss: 374,482 of 1,690,073
  translation changes (22%) are sub-1 mm wobble on MOVERS; eligible
  stayed stable, so bit-exactness is NOT refuted -- the wobble rides
  on genuinely moving records.
- Zero-record frames 9,923/14,465 are the menu/loading phases around
  the settlement segment (in-scene feed healthy, faults 0). These are
  the tracker's own counts -- artifact-free by construction (the
  probe's seed-flush artifact, finding 7, never existed here).
- Bounds dump: part A frame 10292 (8 movers + 8 statics) and part B
  frame 10293. The finding-6 gate held: part A fired on the first
  settlement frame where BOTH populations qualified, not at session
  frame 2. Decode (raw lines in this log, 12:53:40):
  +0xB0..+0xEF constant identity 3x4 + zero row across all 16 records
  (likely parent-relative, unproven); +0xF0..+0x11F rotation, three
  padded float4 rows (unit rows, third ~= cross of the first two);
  +0x120..+0x12B translation numerically EQUAL to the +0x170 pose in
  all 16 records (e.g. id=1452: c38c6780 42772800 c3b0b980 =
  -280.809,61.789,-353.449); +0x12C zero. All 32 words bit-identical
  between the two frames for all 16 records: no view products in the
  block, and the top-by-path movers were stationary across the pair
  (run 2 -> 3, bit-exact).
- REFUTED: the decomp reading "world-bounds recompute at
  record+0xB0..0x12C" -- the block is the world transform (rotation +
  translation == +0x170); there are no min/max bounds in it. The
  updater's 64-byte current/previous pair (+0xF0 -> +0x1C0,
  decomp_433DB20:506-513) is this rotation+translation matrix; its
  +0x120 change-test (line 398) is the translation compare. CONFIRMED:
  +0x240 is the world centre (statics: a few metres off the origin,
  e.g. id=143 pos (-80.442,70.150,-237.202) vs centre
  (-79.85,73.20,-238.90); zero for the dumped movers).
- Stage B consequence: the ownership coverage needs a world AABB and
  the extents are not in the captured block -- centre at +0x240,
  extents somewhere else (local AABB in LOD data transformed by this
  matrix, or a field past +0x240). Next instrument: widen the raw
  capture toward +0x130..+0x250 for the same dump set.
- Eye dump taken (eye_125429_*, 12:54:29). The eval probe's own JSON
  capture was not armed this flight; its fixes (findings 2/3/7/9) are
  covered by the new kinematic_probe_test gate and await the next
  armed capture.
