# Design: an EDVR-side LOD bias at the settlement part test

Offline measurement, 2026-09-22 (sections 1-7); the shadow governor built
on it 2026-09-23, not flown (section 8). Capture: eye run 165433
(Cranfield, parked on the pad, Pimax OpenXR, 3070x3032 an eye, game build
332841), frame 2, 18,267 pool eye draws (EB52 10,690).

## Status

- **State (2026-09-23):** the SHADOW GOVERNOR is built and gated, NOT
  FLOWN (section 8): `fix.settlement_detail = auto` derives k every frame
  from the engine's own signals, re-runs the part test and the record test
  with ctx+0x30 x k, and logs what would change -- it never acts. EDVRGATE
  v3 records each part's LOD table once per pointer, so the LOD-distance
  half below becomes exact from one capture. Offline so far: the
  screen-size half is priced exactly and is a weak lever; the slider's
  half is bounded, not priced.
- **Site and mechanism** (decomp_42B3FC0 + .rdata): FUN_1442B3FC0 passes a
  part in a view iff (1) screen size `0.5*(A*d + B) <= r` -- A = view
  +0x550 = 1/fy, the tangent of one pixel (0.000834297 here), B = +0x560
  = 0 for the eyes: the part's sphere spans at least one pixel; (2) the
  frustum (FUN_1404F4E10); (3) `f = A*(d - r)*s + B <= t0`, s = ctx+0x30,
  t0 the first float of the part's 0x80-byte LOD table (*(entry+8)). The
  LOD nibble is the first i with f <= t[i+1].
- **Reproduction:** (1)+(2) exact over all 109,291 rows: 0 engine passes
  they reject. 8,355 engine rejects (2,352 in frame 2's eye rows) pass
  both: class (3), the unrecorded table. The rows fit one monotone table
  per model (3,511 models, 0 t0 conflicts, 0 nibble inversions); no global
  table fits. Nibbles and t0 are data, not reproducible.
- **Slider:** LODDistanceScale acts only on s: ctx+0x30 = 2 -
  LODDistanceScale (floor 0.1 -> s = 1.9). +0x550/+0x560 are the camera
  projection's pixel size; the screen-size term has no s.
- **Elasticity** (exact join; ms = share x 6.3 post-cut / x 8.5 pre-cut):

| k | screen size x k (exact): draws A+B, share, ms post/pre | LOD distance x k (the slider's form): bounds |
|---|---|---|
| 1.25 | 0, 0% | 16..4,075 (0.1..22.3%) |
| 1.5 | 4, 0.02% | 16..4,409 |
| 2 | 6, 0.03%, 0.00 | 16..5,234 (0.1..28.7%) |
| 3 | 40, 0.22%, 0.01/0.02 | 18..6,675 |
| 4 | 74, 0.41%, 0.03/0.03 | 18..7,743 |
| 6 | 243, 1.33%, 0.08/0.11 | 18..9,827 |
| 8 | 613, 3.36%, 0.21/0.29 | 18..11,493 (0.1..62.9%) |
| 10.46 (0.25 deg cap) | 1,089, 5.96%, 0.38/0.51 | 82..13,033 |

- **Visual cost (screen size):** only parts under k pixels go: at k = 8,
  radius <= 1.28 m at 77-396 m (p50 279 m), r/d <= 0.191 deg; 16 small
  records (1-6 parts) go whole, no building; 21 / 19 removed slots were
  visible (exact re-draw), 2.5% of those removed. Every k <= 10.46 keeps
  removed parts under 0.25 deg. The t0 half has no such cap: its certain
  removals at k = 1.25 already reach 0.74 deg and a 4.2 m radius.
- **LOD shift is draw-neutral here:** 99.0% of admitted eye parts sit at
  nibble 3 or 4, which draw identical meshes in 149 of 151 models.
- **Leg C (engine arc, 2026-09-23):** LODDistanceScale 0.001 left the
  parked view at 18.9k eye draws, as section 5 predicts: ruled out there
  as a draw lever (section 6).
- **Open:** the first shadow flight (section 8's checklist); the tables
  from one v3 capture (BUILT: *(entry+8) per verified row, 0x80 bytes once
  per pointer; `--lod-bias` exact where a row names one).
- **Ruled out:** see section 6.
- **Next:** fly `fix.settlement_detail = auto` parked at Cranfield, with
  `advanced.cull_gate_capture = 1` and one eye run for the v3 tables; read
  section 8's checklist. Whether any k is worth ACTING on is Sean's call
  after that flight.

## 1. The test and what the decompile leaves undefined

decomp_42B3FC0.txt:47-71: d = rsqrt(rcp(|c - cam|^2)), cam = view
+0x540; the screen-size term uses DAT_144E2F870 x DAT_144E2F880 = 0.5 x
1.0 (read from the exe). :75 FUN_1404F4E10(view, centre, radius): out
when n.c - w > r on any dumped plane. :78-109 f against the table
param_1[4]: f > t[0] rejects; else the nibble is the first i < count
(+0x70) with f <= t[i+1], else count. The builder copies the table from
*(entry+8) (42B4420:410-428) and clamps the nibble to the model's meshes
(:536-595, FUN_142817260). The table's values are asset data.

## 2. Reproduction (tools/cull_gate_probe.py --lod-bias prints it)

Frame 2 eye rows: 23,542: 20,728 pass, 412 screen-size, 50 frustum and
2,352 t0 rejects. Distance precision: 6 rows lie within 0.2% of the
screen-size threshold, all rejected either way. A = 1/fy of the depth
files' clip matrix to 6 significant digits (fy 1198.61 px, H 3032).

## 3. Elasticity, per eye (screen size x k, exact)

| k | parts A / B | draws A / B | EB52 | visible parts A / B | visible slots A / B | draws with a seen slot |
|---|---|---|---|---|---|---|
| 1.5 | 1 / 1 | 2 / 2 | 2 | 0 / 0 | 0 / 0 | 0 |
| 2 | 2 / 2 | 3 / 3 | 4 | 0 / 0 | 0 / 0 | 0 |
| 3 | 66 / 66 | 20 / 20 | 26 | 2 / 2 | 1 / 1 | 4 |
| 4 | 154 / 163 | 37 / 37 | 42 | 6 / 6 | 5 / 5 | 7 |
| 6 | 439 / 453 | 120 / 123 | 156 | 11 / 11 | 10 / 10 | 43 |
| 8 | 979 / 993 | 305 / 308 | 386 | 24 / 22 | 21 / 19 | 73 |
| 10.46 | 1,735 / 1,750 | 543 / 546 | 694 | 52 / 51 | 45 / 43 | 122 |

A draw goes only when every slot of its instance range goes in that eye
(the 14.8% holding an unclaimed slot never go); k = 1 removes 0. The
view's LOD scale x k (screen size and t0 together) has the bounds
22..5,234 at k = 2 and 613..11,493 at k = 8. Bounds: t0 >= the largest f
a model passed at, t0 < the smallest f it was t0-rejected at; the rows
certify k*f <= t0 for 61.7% of admitted eye rows at k = 1.25, 9.6% at 8.
Visible = the exact re-draw (phaseB3 truth).

## 4. The slider (item 3)

FUN_14280F800 sets scene view +0x530/+0x540 = 2*M23/(M11*H) and
2*M33/(M11*H) (camera projection at +0x1B0, H the target height);
FUN_142819F70 copies them to render view +0x550/+0x560. FUN_142819D90:108
sets ctx+0x30 = 1 + (1 - x); FUN_1401EA920:137-157 passes x = +0x124 of
the render component's settings (type id DAT_145F02BB4; default 1.0),
where FUN_142855A50 stores LODDistanceScale (clamped to +0x128/+0x12C).
Identified by offset, not traced end to end; 165433's s = 1.0 matches
LODDistanceScale 1.0 in Custom.4.4.fxcfg (written 12:47 that day).

At s x 1.9 against the 2026-09-21 pass C (Quest 3, another session):

| family | measured | bounds on 165433 |
|---|---|---|
| EB52 | -16.9% | 0.1..34.2% |
| 2684F02B9B0BB0DE | -14% | 3.1..42.0% |
| 8056C9D5F22007F9 | immune | 0.0..2.2% |
| all pool eye draws | -18.7% | 0.1..27.2% |

Agreement: every measured delta lies inside; 8056's immunity is the one
the rows pin (<= 2.2%). Gap: EB52's number is unpinned. The screen-size
half cannot be the slider: it has no s, and x 10 removes 5.9% of EB52.

## 5. Visual cost and LOD shift

Screen size, eye A (B within 2%):

| k | parts | distance p10/50/90 m | radius p50/90/max m | r/d p50/max deg | whole records |
|---|---|---|---|---|---|
| 3 | 66 | 84/321/379 | 0.35/0.44/0.44 | 0.063/0.072 | 2 |
| 4 | 154 | 174/300/368 | 0.39/0.55/0.58 | 0.077/0.095 | 3 |
| 6 | 439 | 164/298/371 | 0.48/0.79/0.88 | 0.116/0.143 | 4 |
| 8 | 979 | 153/279/368 | 0.59/1.00/1.28 | 0.148/0.191 | 16 |
| 10.46 | 1,735 | 133/245/364 | 0.73/1.06/1.69 | 0.181/0.250 | 51 |

Removed draws at k = 8: EB52 386, 5B4D8E894EEDA8B4 110, BBE58E40FE88EC80
38, 2684F02B9B0BB0DE 22. A player would see sub-k-pixel details thin
out beyond ~150 m; the 16 whole records at k = 8 have 1-6 parts, each
radius <= 1.01 m, at 77-391 m. No building goes.

LOD shift (forms that scale f): at s x 1.9 the nibble certainly rises
for 3,783 of 20,728 admitted eye rows, 58 of them to a different
observed mesh set; possibly for 12,374. Nibble pairs seen on one model:
152 identical mesh sets (149 of them 3 vs 4), 9 disjoint.

## 6. Ruled out

ruled out: the screen-size half of FUN_1442B3FC0 as the settlement lever,
because at 8 pixels it removes 613 of 18,267 pool eye draws (0.21 ms
post-cut) and at the 0.25-degree cap 1,089 (0.38 ms), against 1.5 ms.
ruled out: view +0x550/+0x560 as where LODDistanceScale acts, because
they are the projection's pixel size (+0x550 = 1/fy); it acts on
ctx+0x30 = 2 - LODDistanceScale.
ruled out: the LOD pick as a draw lever at the parked range, because
99.0% of admitted eye parts are at nibble 3 or 4 and those draw
identical meshes in 149 of 151 models.
ruled out: pricing the LOD-distance half from EDVRGATE v2, because its
t0 is per-model data the probe does not record: at s x 1.9 the rows bound
it only to 0.1..27.2% of the pool eye draws.
ruled out: LODDistanceScale as a draw-count lever at the parked cockpit
view, because 0.001 left the census at 18.8-18.9k eye draws a frame
(engine arc 2026-09-23, Leg C; the same count as at 1.0).

## 7. Sources

Decompiles: analysis\decomp\ 42B3FC0, 4F4E10, 42B4420, 2817260,
4308B30, 280F800, 2819F70, 2819D90, 01EA920; FUN_142855A50 by string
xref (session scratch, lodbias\ghidra). Scripts: session scratch lodbias\
(repro, elastic, visual, lodshift, headroom, famcheck), reusing phaseB3's
join and truth. Tool: `python tools\cull_gate_probe.py <pool> 165433
--lod-bias 1,1.25,1.5,2,3,4,6,8` reproduces section 3 and the bounds.

## 8. Shadow governor (2026-09-23): built and gated, NOT FLOWN

`fix.settlement_detail` = `game` (default: off, nothing observed) |
`auto` (this build: shadow only) | `reduced` (reserved: behaves as auto,
logged as such); `advanced.settlement_detail_max` = k_max (default 2.0,
held to 1..4). Both ship commented out. Code: src\d3d11\lod_governor.*,
the hook side in kinematic_eval_hook.cpp, rig tools\lod_governor_test.

**Signals**, once a frame at vScreenFrameBoundary (vscreen.cpp:5339, the
caller thread):

| signal | source |
|---|---|
| builder records a frame | the draw-item builder bracket, one call per engine record (kinematic_eval_hook.cpp:516 -> lod_governor.cpp:673); owner-thread counter slots, differenced at the boundary |
| part tests a frame | FUN_1442B3FC0's relay after its forward (kinematic_eval_hook.cpp:577 -> lod_governor.cpp:704) |
| frame work | NativeTimingSnapshot::applicationMs (native_timing.h:14, native_timing.cpp:341-347): WALL ms on the producer (the game's caller) thread from the pose wait's end to the submit plus the per-eye treatments, valid only with all four segments; the perf monitor's app CPU (perf_monitor.cpp:765). Read once per new sequence; an invalid newest frame breaks the runs (lod_governor.cpp:407) |
| budget | 1000 / EdvrNativeTimingFrame::baseDisplayHz (v4), else the session's first predicted period (native_perf_history.cpp:122-125's rule): 11.111 ms at 90 Hz |
| s | ctx+0x30, read by the builder observer, for the log |

**Policy** (lod_governor.cpp:37; constants in lod_governor.h, no keys):
k = 1 + 0.05 n up to k_max. Up one step while the frame has >= 200 builder
records AND the last 30 valid samples each ran more than 0.30 ms over the
period; down one after 30 samples each more than 1.00 ms under it; at most
one step per 1000 ms; k = 1 at once after 30 consecutive frames under 150
records (in at 200, out under 150). A frame with no new sample holds both
runs, an invalid one breaks them, a lowered k_max clamps at once. A full
ramp to 2.0 takes ~20 s.

**The shadow** (worker threads, read-only):
- Per part (lod_governor.h:113-195), the engine's own inputs: c =
  *param_1[0], r = *param_1[1], camera/A/B = view +0x540/+0x550/+0x560, s
  = *(param_1[5]+0x30), the table = param_1[4] (the builder's copy). d =
  rsqrtps(rcpps((dx*dx + dy*dy) + dz*dz)), the engine's SSE approximations;
  f = A*(d - r)*s + B, f_k = A*(d - r)*(s*k) + B. Only parts the engine
  passed, and only when the k = 1 recompute reproduces its pass and nibble
  (else a counted disagreement): would-drop when t0 < f_k, would-change
  when the nibble at f_k differs; the would-drop parts' r/d in four
  buckets (< 0.25, 0.25-0.5, 0.5-1, >= 1 deg).
- Per record, at the builder bracket before its forward
  (lod_governor.cpp:215): FUN_144308B30 IS reachable there, no new patch.
  Its inputs are the record's own fields (4312040:154-165: centre
  rec+0x240, radius rec+0x280, table *(rec+0x20); ctx+0x30; each mask
  bit's view through ctx+0x1A840) and its results are what the dispatch
  read (rec+0x208, rec+0x210 nibbles by view bit, node+0x6A;
  4320340:64-95, 4321940:59-86). Per eye: passed, would lose the eye,
  would change level, disagreements; per record: would not be dispatched
  at all. Not modelled: a parent's lost view is its children's too
  (4312040:353), and FUN_144331300's collection mask, so the record counts
  are a lower bound. Orthographic views (A = 0, f = B) do not move with s:
  a record a shadow cascade admits stays dispatched.
- The eyes: the two perspective views (B = 0) with the finest A, keyed by
  view BIT (165433: bits 1 and 22, A 0.000834297; the other perspective
  views are ~9x coarser, the cascades A = 0). Array indices are not
  stable: 165433's eye B is view 5 in frame 2 and view 6 in frames 3-4.
- Only two relays open: the builder bracket's (its own cell, evalGate OR
  the governor) and the part test's (the probe's build-keyed patch, now
  installed for the governor too). The evaluator, job brackets and direct
  producers stay dark; the bucket census runs only with the eval gate.

**Log lines** (the rig's `--self-test --print-log` shows them all):
configure `settlement detail: shadow governor on (auto: shadow, never
acts) -- k in [1, 2.00] ... Hooks: draw-item builder FUN_1442B4420
hooked, per-part test FUN_1442B3FC0 hooked`; `reduced` reads `(reduced is
reserved and behaves as auto in this build: shadow, never acts)`; `game`
reads `settlement detail: off (...)`; a refused attach `could not attach
its engine hooks (<status>)`. A step, at most one line per 5 s (skipped
steps counted): `settlement detail (shadow, never acts): k 1.25 -> 1.30,
up: ...; 679 builder records, frame work 12.41 ms vs period 11.11 ms`.
Every 30 s a header (k now, window min..max, steps; records and part
tests a frame, mean and max; frame work mean vs period, samples over and
under; s; the eye bits), one line per eye (parts tested and passed; would
drop per frame, mean and max; would change level; the histogram; records
passed, would lose the eye, would change; disagreements at k = 1) and one
for the other views (records not dispatched, dispatch disagreements,
faults, foreign callers, over-long tables). A window that never left k = 1
says why (no builder calls / never 200 records / no valid sample / never
0.30 ms over in a 200-record frame / runs never reached 30). No builder
call in a window: the header only. Off: nothing at all.

**Cost.** The rig, hot synthetic memory, this machine: part observer
14-17 ns/call, record observer 21-35 ns/call (3 views; the higher figures
under build.bat's parallel rigs), the boundary's slot sum 0.06 us. At
165433's density (34,250 part tests and 679 builder calls a frame):
~0.5-0.6 ms/frame of worker CPU for the parts plus the part relay's own
jump and call (estimated 0.1-0.2 ms), spread over the job workers;
~0.02 ms for the records; on the caller thread one boundary call (a few
us: the slot sum, ~11 view reads, one native-timing snapshot) and, only if
the builder runs there, one owner-thread increment per builder call.
`game`: nothing -- the part patch is not installed unless the probe arms,
and the bracket's relay reads one cell as before.

**EDVRGATE v3** (cull_gate_probe.*, tools\cull_gate_probe.py): each
verified part row carries *(entry+8), the table the builder copies into
param_1[4]; each distinct pointer is recorded once (the 0x80 bytes the
test read, the pointer, the entry's model, a flag when the asset no longer
matches the copy); counters kept/dropped. The reader lists them
(`--tables N`; `--tables-only <file>` needs no pool), re-runs the third
term and the pick with each row's own table, and makes `--lod-bias`'s
LOD-scale and LOD-distance forms exact for those rows. v1/v2 still read.

**What the first flight must show** (parked at Cranfield, the Leg C
pose):
1. `edvr_log.py --expect-build HEAD` exits 0; the configure line with
   both hooks `hooked`.
2. Headers with ~679 builder records a frame (>= 200 on nearly every
   frame), ~34k part tests, eye bits 1 / 22 named, s 1.000.
3. Frame work over the period (Leg C: caller 11.9 ms) -> step lines and k
   2.00 within ~25 s; if k stays 1, the header's reason is the finding.
4. Disagreements at k = 1: parts 0 and records 0 per eye, or a handful at
   thresholds. More, and nothing else on the eye lines is evidence.
5. Per eye ~11.8k parts tested, ~10.35k passed (165433 frame 2, eye A:
   11,771 / 10,354); the would-drop parts at k = 2 and their histogram
   (parts, not draws: the v3 capture's `--lod-bias` turns them into draws
   against section 3's bounds).
6. Records not dispatched at all: few expected here (a record any
   orthographic cascade admits stays dispatched); every one is a whole
   record's parts in every view, the biggest lever, and is read first.
7. Census eye draws as in a `game` run (it never acts) and the caller
   thread no slower than Leg C.
8. Same flight, optional: `advanced.cull_gate_capture = 1` and one eye
   run -> a v3 gate file; `--tables-only` must report 0 disagreements off
   the thresholds, then `--lod-bias 1,1.25,1.5,2` is exact.
