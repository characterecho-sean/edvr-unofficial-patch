# Settlement flicker + frame time (2026-09-17)

## Status

- State: verified Frontier build 7cfb5a6 accounts for the landed cycle:
  15.11-15.12 ms, or 66 FPS, including 4.11-4.23 ms after the second Submit
  returns and before the next pose-wait entry. Captured Submit/Wait caller
  handoffs do not explain that gap. Its internal cause is still unmeasured. The
  prior verified dcd0a9f draw probe completed 29520 captures; three unchanged
  payloads went from zero passed samples to nonzero. Cached rejection alone is
  unsafe; no draw suppression is active. See the final journal entry.
- Environment: Quest 3 / VirtualDesktopXR / RTX 5090 / 90 Hz, AA off, input
  2481x2121, XR output 3072x3264 per eye, trims off. On-foot source is
  5120x2880 in the prior run; this flight has no on-foot leg or eye capture.
  Prior DLSS runs used K; the flight does not log the DLSS DLL version.
- Timing: two clean 30-second windows account for all 3970 admitted cycles with
  zero residual or missing counters. Before-first-Submit mean is 10.42-10.55
  ms, both Submit roundtrips total 0.39-0.40 ms, and next pose-wait roundtrip
  is 0.05-0.06 ms. Separate gameplay benchmark windows have CPU p50 9.93-10.74
  ms and GPU p50 10.61-11.18 ms; GPU p95 is 12.10-12.55 ms. These application
  segments are not full frame periods, and their window alignment differs from
  the cycle probe. Do not subtract their percentiles.
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
- CPU: the hook timer excludes the probe/owner lookup and, for indexed
  instanced draws, weapon-motion work. It cannot measure total EDVR overhead. A
  skip at the D3D draw hook also cannot recover Elite's earlier scene
  traversal, draw-list construction or state submission.
- Prior DLSS work: static identity across rebases and first-visible/moving
  history remain unresolved. Typed fusion was not useful; precomputed capture
  controls suggest only 0.21-0.35 ms per captured subset. Earlier evidence and
  ruled-out batching/cache/screen-motion hypotheses remain in the journal.
  Motion tables remain 512 records/eye and 64 source records; station rotation
  and independently moving ships still need separate validation.
- Next diagnostic: split the post-stereo interval on the same caller/sequence
  into game-side gaps, raw DXGI Present, all EDVR Present work including its
  trailing render callback, and any PostPresentHandoff roundtrip. Existing
  Present timers are not persisted in the native flight log and cannot answer
  this yet. No new build or flight requested. Conservative current-depth
  culling remains an offline candidate; preserve motion history and keep
  foveated-DLSS work paused.

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
