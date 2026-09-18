# Settlement flicker + frame time (2026-09-17)

## Status

- State: the verified 9446f4d Frontier flight supports the user's report of
  worse overall performance. Batching works, but its effect on total frame time
  is unresolved. Frontier has been restored to the verified 81eeedb package for
  a controlled comparison; main retains the batching change pending that
  evidence. No static or visibility skips are enabled.
- Latest capture: 204142, ship above the settlement, scene frame 20347. Quest 3
  / VirtualDesktopXR / 90 Hz / DLSS K, input 2481x2121 and active XR output
  3072x3264. The user's removed trims remain off. These sizes match the prior
  untrimmed settlement capture, but view and simulation state differ.
- Regression: prior native window 17 CPU/GPU p50 was 12.703/16.717 ms. New
  steady windows 8-10 report 13.381-14.930 ms CPU and 17.596-18.971 ms GPU. All
  finish before the dump; slower medians are not explained by capture stalls.
- Batching result: new steady windows have 14400 batches per 1800 frames
  (8/frame), versus 425080 (236/frame) in the prior mixed settlement/transition
  window. All size-only flushes are gone; bounded ranged-ID copies are active.
  Approximate mesh-draw CPU falls from 1.633 to 1.079-1.111 ms/frame. These
  measurements do not establish the cause of the overall regression.
- Capture result: 504 valid / eight invalid records, all valid raw poses
  unchanged with an origin shift; no malformed coverage IDs or obvious
  ranged-offset corruption. This is not proof against a GPU copy/queue
  scheduling regression. Broader game workload also remains a candidate.
- Static constraints remain: many static candidate pixels select the
  head-motion base; final UI/body/layer gates are unresolved. Station singleton
  pairs show coherent rotation. On-foot windows have no captured rigid meshes
  and need separate attribution.
- Ruled out: failure to activate batching, and dump stalls explaining the
  compared steady native windows. Earlier ruled-outs include changing bindings
  as the size-only flush cause, blanket static-pose skips, equal-origin
  requirements, and prior zero coverage as a safe current-visibility test. See
  the journal for evidence.
- Next flight: on restored v0.17.0-2-g81eeedb, hold the same settlement
  position/view with unchanged settings and trims off for roughly 90 seconds,
  then take one completed eye run. Verify logs against 81eeedb explicitly, not
  HEAD. Compare steady windows before another code change.
  Static/history/fallback and shared station-motion work remain separate.

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
