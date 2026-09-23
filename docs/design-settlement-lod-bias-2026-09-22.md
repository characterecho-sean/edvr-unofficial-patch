# Design: an EDVR-side LOD bias at the settlement part test

Offline measurement, 2026-09-22 (sections 1-7); the shadow governor built
on it 2026-09-23 (section 8: flown twice); the acting mode (section 9:
flown once, refined, shipped as the default). Capture: eye run 165433
(Cranfield, parked on the pad, Pimax OpenXR, 3070x3032 an eye, game build
332841), frame 2, 18,267 pool eye draws (EB52 10,690).

## Status

- **State (2026-09-23):** `fix.settlement_detail` SHIPS as a fix, default
  `auto` (live in edvr.ini, F8 Performance page), k_max 6.0 (1..8), cockpit
  only (k = 1 on foot). Refinement 4b and the review's fixes (section 9 part
  (6)) FLEW at 07:15 (6d34ffd5): a decision a second on cycles with FRESH
  timing; misses classed GPU-bound / unexplained / the CPU's; up while a
  tenth are the CPU's (0.25 at a quarter or > 1 ms over); a kick after 10 s,
  a trial (07:15: failed, 3.15 restored); down trials undone within 10 s;
  back aboard the k from before (07:15: 3.90). Since, NOT FLOWN: a step's
  benefit judged only on a stable scene, on the parts tested and passed and
  the caller work; the ceiling's outcome against the k = 1 baseline of the
  same view, both ends printed. Flights: 03:30 (b55e06b, s 1.5) k 1 -> 2.70
  in 80 s, ~50 -> 71-75 fps; 04:23 a third of the frames took two slots at a
  mean at the period, no step; 05:05 (reduced) two slots a cycle at k 6 (the
  half-rate trap); 05:53 (close range) ~4 of 4,800 parts dropped at k 6;
  06:53 re-boarding re-ramped from 1 and flickered. Review:
  reviews/lod-governor-review-2026-09-23.md (main checkout, gitignored).
- **Site and mechanism** (decomp_42B3FC0 + .rdata): FUN_1442B3FC0 passes a
  part in a view iff (1) screen size `0.5*(A*d + B) <= r` -- A = view
  +0x550 = 1/fy (0.000834297 here), B = +0x560 = 0 for the eyes; (2) the
  frustum (FUN_1404F4E10); (3) `f = A*(d - r)*s + B <= t0`, s = ctx+0x30,
  t0 the first float of the part's 0x80-byte LOD table. The LOD nibble is
  the first i with f <= t[i+1].
- **Reproduction:** (1)+(2) exact over all 109,291 rows; 8,355 engine
  rejects pass both: class (3), the unrecorded table. One monotone table
  per model fits (3,511 models, 0 conflicts); no global one does.
- **Slider:** LODDistanceScale acts only on s: ctx+0x30 = 2 -
  LODDistanceScale above its floor; the engine holds s = 1.5 at 0.001
  (reader run 012514), so the slider saturates at 1.5. +0x550/+0x560 are
  the camera projection's pixel size; the screen-size term has no s.
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

- **LOD distance, EXACT** (the v3 tables, reader run 012514, keyed by s x
  k; section 8): 1.875 -> 318 draws, 1.9%; 2.25 -> 1,123, 6.7%; 3.0 ->
  3,222, 19.2% (1.21 ms); 4.5 -> 5,634, 33.5% (2.11 ms); 6.0 -> 33.7%. It
  levels off near 34%: the building shells' tables (t0 21.38) never drop.
- **Visual cost** (section 5): the screen-size half removes only parts
  under k pixels; the t0 half has no such cap (at k = 1.25 already 0.74
  deg and a 4.2 m radius).
- **LOD shift is draw-neutral here** (99.0% of admitted eye parts at nibble
  3 or 4); Leg C's LODDistanceScale 0.001 left 18.9k eye draws (section 6).
- **Open:** part (6)'s stable-scene judgement and baseline outcome, unflown
  (section 9's last list).
- **Ruled out:** section 6 (the mean as the miss signal and the step size,
  the sliding window, the dropped count as the inert test) and section 9.
- **Next:** fly the shipped default parked at Cranfield with the in-game
  detail slider at its default, then close to the buildings; disembark and
  board once.

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
ruled out: s = 2 - LODDistanceScale below the slider's floor, because
the engine holds s = 1.5 at 0.001 (reader run 012514's view dump).
ruled out: NativeTimingSnapshot::applicationMs as the governor's frame
work, because it is the pre-submit phase only: the first shadow flight
read 8.4 ms of it against 14.4 ms of caller work a cycle (period 11.1),
and k never left 1 (section 8).
ruled out: the perf monitor's CPU figure as evidence the frame fits,
because it is the pre-submit phase only (applicationMs): 7-8 ms on the
HUD while the caller thread worked 11.7-12.7 ms a cycle and fps sat at
50-55 (shadow flight 2).
ruled out: 30 consecutive over-budget samples as the governor's trigger,
because with a third of the frames missing their slot such a run never
comes (04:23: k held at 2.50 for 30 s while 534 of 1712 samples ran over).
ruled out: a threshold on the mean caller work as the miss signal, because
a mean at the period hides a third of the frames taking two slots (04:23),
and at half rate the game's own work grows ~1.2 ms so the mean cannot say
whether the frame would fit at full rate (05:05).
ruled out: a sliding 30-sample window read every frame as the trigger,
because scattered two-slot cycles creep k up (3% at random fired it 19
times a minute in the rig); a decision a second on that second's own
cycles fires 0 times in 300 s at 3%.
ruled out: the all-sample mean as the up step's size, because good frames
cancel bad ones: 31% misses at a mean near the period took eleven 0.05
steps (the review of 2026-09-23, finding 2).
ruled out: the dropped-part count as the test of an inert lever, because
whole records culled upstream never reach the part observer and a nibble
change cuts work without dropping a part (the review); the benefit test
reads the parts tested and passed at EDVR's scale and the caller work.
ruled out: judging a step's benefit on a changing scene, because at 07:15
the inert line came during the approach (the records rising, the per-eye
tested count 2,109 -> 4,205 -> 2,477 across windows); a step is judged
only when both its sides are stable.
ruled out: the last step's passed parts as the ceiling's outcome, because
at 07:15 it said residual benefit beside an acting dropped count of 4 a
frame; the outcome reads the whole effect against a k = 1 baseline of the
same view and prints both ends.

## 7. Sources

Decompiles: analysis\decomp\ 42B3FC0, 4F4E10, 42B4420, 2817260,
4308B30, 280F800, 2819F70, 2819D90, 01EA920; FUN_142855A50 by string
xref (session scratch, lodbias\ghidra). Scripts: session scratch lodbias\
(repro, elastic, visual, lodshift, headroom, famcheck), reusing phaseB3's
join and truth. Tool: `python tools\cull_gate_probe.py <pool> 165433
--lod-bias 1,1.25,1.5,2,3,4,6,8` reproduces section 3 and the bounds.

## 8. Shadow governor (2026-09-23): flown twice, signal confirmed by the second flight

`fix.settlement_detail` = `game` (default: off, nothing observed) |
`auto` (this build: shadow only) | `reduced` (reserved: behaves as auto,
logged as such); `advanced.settlement_detail_max` = k_max (default 4.0
since the first flight, was 2.0; held to 1..4). Both ship commented out.
Code: src\d3d11\lod_governor.*, the hook side in kinematic_eval_hook.cpp,
rig tools\lod_governor_test.

**Signals**, once a frame at vScreenFrameBoundary (vscreen.cpp:5339, the
caller thread):

| signal | source |
|---|---|
| builder records a frame | the draw-item builder bracket, one call per engine record (kinematic_eval_hook.cpp:516 -> lod_governor.cpp:773); owner-thread counter slots, differenced at the boundary |
| part tests a frame | FUN_1442B3FC0's relay after its forward (kinematic_eval_hook.cpp:577 -> lod_governor.cpp:804) |
| frame work | the caller work per cycle, EdvrNativeTimingFrame::callerWorkMs (timing ABI v5, src\common\native_timing.h): WALL ms on the game's caller thread from one WaitGetPoses return to the next one's entry = cycle - next_wait_roundtrip = before-first + both submit roundtrips (the waits inside them included) + between-eyes + post-second-submit, everything the period must hold. Computed where the cycle completes (frame_cycle_stats.h finishCurrent, at the NEXT wait's return; callerWorkForCurrent) and sent with the next frame's publishCpu (native_runtime_host.h), so it lags one frame and is valid only when that cycle completed whole. A v3/v4 runtime sends none: fallback NativeTimingSnapshot::applicationMs (pose wait end to submit plus the treatments, the monitor's app CPU, which keeps reading it), named `app work (pre-submit only; host older)`; afterSecond never crosses, so there is no better fallback. A v5 frame without caller work is an invalid sample, never the fallback. Read once per new sequence; an invalid newest frame breaks the runs (lod_governor.cpp:448 readWork) |
| budget | 1000 / EdvrNativeTimingFrame::baseDisplayHz (v4 and later), else the session's first predicted period (native_perf_history.cpp:122-125's rule): 11.111 ms at 90 Hz |
| s | ctx+0x30, read by the builder observer, for the log (s x k too) |

**First flight (2026-09-23 07:25 UTC, parked at the settlement, 45
fps).** The 30 s summaries read frame work 8.37-8.67 ms mean vs period
11.11 ms, so k never left 1; the runtime's cycle instrument for the same
windows read game_before_first_submit 8.24, post_second_submit_to_next_wait
5.31, next_wait_roundtrip 7.57, cycle 21.99 ms: the caller thread worked
21.99 - 7.57 = 14.4 ms a cycle against an 11.1 ms period. The signal was
the pre-submit phase only (section 6's ruled-out line); it is now the
caller work above, and every line names which figure it read.

**Second flight (2026-09-23 02:04 local, fa6565b), parked at Cranfield,
02:04-02:09, same spot and settings as the first flight.** Configure
line at 02:04:37, k in [1, 4.00], both hooks hooked; the one-off line at
02:04:41 reads `frame work = caller work per cycle (runtime timing v5)`
-- the fix is in force. Mean caller work climbed 8.82 (still loading) ->
11.38 -> 12.76 -> 12.47 -> 11.80 -> 12.34 ms across the settlement
windows, against the 11.11 ms period; caller work absent was 0
throughout; s held at 1.500, matching the floor. k rose 1.00 -> 4.00 in
142 s (one step per ~2.6 s: each step needs 30 consecutive over-budget
samples, slower than the one-step-a-second cap when the load settles
first, as it did here). At 02:09:12 a reset line took k back to 1.00
after 30 frames under 150 records (Sean left the view): the reset path
works. The runtime's own cycle instrument for the same windows (11 s
earlier) gives caller work (cycle - next_wait_roundtrip) of 11.80 /
11.70 / 12.71 / 11.79 / 12.11 ms against the governor's 11.38 / 12.76 /
12.47 / 11.80 / 12.34 for its offset windows -- the same figure by
construction, agreeing within the window offset. 0 disagreements with
the engine at k = 1, every window. Per eye, would-drop saturates with
s x k just as the draw table predicts: 48% of passed parts at 4.5
(window ending 02:08:08), 49-50% at 6.0 (window ending 02:09:08), against
the exact draw table's 33.5% -> 33.7% ceiling -- the dropped parts are
the low-draw ones, the building shells' tables (t0 21.38) never drop.
Also answers a question about the HUD: its CPU figure is applicationMs,
the pre-submit phase only (~7 ms), which is why it read under 10 ms
while the caller thread's real work was 11.7-12.7 ms a cycle at 50-55
fps; GPU under 10 ms is consistent with the earlier 9.2-10.2 ms
measurement and is not the limiter.

**k_max default 4.0 (reader run 012514).** The probe's view dump shows
the engine holding s = ctx+0x30 = 1.5 at LODDistanceScale 0.001 and 1.0 at
1.0: the slider saturates at 1.5, so section 4's `2 - slider` is wrong
below the floor. k multiplies whatever s the game holds, so what the
tests see is s x k, which the summary prints beside k. The recorded
tables give the exact removal at the parked pad by effective s x k:

| s x k | 1.875 | 2.25 | 3.0 | 4.5 | limit |
|---|---|---|---|---|---|
| pool eye draws removed | 1.9% | 6.7% | 19.2% (1.21 ms at the 6.3 ms post-cut share) | 33.5% (2.11 ms) | ~34%: the building shells' tables have t0 = 21.38 and never drop |

From the game's maximum setting (s = 1.0), s x k = 3 needs k = 3, past
the old 2.0 ceiling; hence the default 4.0, still held to 1..4.

**Policy** (lod_governor.cpp:37; constants in lod_governor.h, no keys):
k = 1 + 0.05 n up to k_max. Up one step while the frame has >= 200 builder
records AND the last 30 valid samples each ran more than 0.30 ms over the
period; down one after 30 samples each more than 1.00 ms under it; at most
one step per 1000 ms; k = 1 at once after 30 consecutive frames under 150
records (in at 200, out under 150). A frame with no new sample holds both
runs, an invalid one breaks them, a lowered k_max clamps at once. A full
ramp to 4.0 takes ~60 s (one step a second after the first 30 samples).

**The shadow** (worker threads, read-only):
- Per part (lod_governor.h:131-213), the engine's own inputs: c =
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

**Log lines** (the rig's `--self-test --print-log` shows them all; the
rig holds every line to the real log's 1166 characters):
configure `settlement detail: shadow governor on (auto: shadow, never
acts) -- k in [1, 4.00] ...; frame work = caller work per cycle (runtime
timing v5: ...). ... Hooks: draw-item builder FUN_1442B4420 hooked,
per-part test FUN_1442B3FC0 hooked` (before any runtime frame: `frame
work = caller work per cycle if the runtime sends it (timing v5), else app
work (pre-submit only; host older); the first runtime frame decides and a
line names it`); that line, once: `settlement detail: frame work = caller
work per cycle (runtime timing v5: ...)` or `... = app work (pre-submit
only; host older: runtime timing v4 sends no caller work, ...)`; `reduced`
reads `(reduced is reserved and behaves as auto in this build: shadow,
never acts)`; `game` reads `settlement detail: off (...)`; a refused
attach `could not attach its engine hooks (<status>)`. A step, at most one
line per 5 s (skipped steps counted): `settlement detail (shadow, never
acts): k 1.25 -> 1.30, up: ...; 679 builder records, frame work = caller
work per cycle: 14.41 ms vs period 11.11 ms`. Every 30 s a header (k now
and `effective s x k`, window min..max, steps; records and part tests a
frame, mean and max; `frame work = <signal>:` mean vs period, samples
over and under, invalid, caller work absent; s; the eye bits), one line
per eye (parts tested and passed; would
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

**What the second flight showed** (parked at Cranfield, the Leg C pose;
six of eight items MET, one not evidenced, one optional and not run):
1. MET: `edvr_log.py --expect-build HEAD` exit 0; the configure line
   with both hooks `hooked`; `frame work = caller work per cycle
   (runtime timing v5` at 02:04:41.
2. MET: ~680 records/frame from 02:07:08 on (>= 200 on nearly every
   frame; 474.8 while still loading in), 20.2k-33.3k part tests/frame, s
   1.500 every window (the slider's floor), `effective s x k` printed
   throughout.
3. MET, with a timing caveat: `frame work = caller work per cycle:` mean
   11.38-12.76 ms over the 11.11 ms period (first flight 14.4 ms, Leg C
   11.9 ms), `caller work absent` 0 throughout -> step lines to k 4.00 --
   but in 142 s, not ~60 s, because the 30-consecutive-sample rule
   outlasts the one-step-a-second cap when the load settles first (two
   down excursions at 02:06:34/02:07:02); its mean matches the runtime's
   cycle - next_wait_roundtrip for the same windows, as required.
4. MET: disagreements at k = 1 were 0 for parts and 0 for records, every
   window.
5. MET: per-eye tested/passed and the would-drop histogram were logged
   every window (e.g. window ending 02:08:08: tested 10,528 / 10,528,
   passed 9,749 / 9,768, would-drop 4,705 / 4,704, 48%; >= 1 deg 1.316M).
6. MET: records the builder would not be called for at all were 0.0 per
   frame, every window.
7. NOT EVIDENCED this flight: census eye draws as in a `game` run, and
   an explicit caller-thread comparison to Leg C's 11.9 ms.
8. NOT RUN, optional: no new `advanced.cull_gate_capture` this flight;
   the per-eye match above uses the existing 012514 tables, not a fresh
   v3 gate file.

## 9. Acting mode (2026-09-23): built at the setter, flown once, shipped as default auto

The brief: make the governor act by writing s_game x k into ctx+0x30 at
the draw-item builder, with a shadow copy of the game's value, if the
object is persistent and its setter runs only on settings changes; if the
setter runs every frame, write up the evidence and stop. The pointer is
stable and the setter runs every frame, so phase 1 built nothing; the
overseer then chose the site the evidence supports -- a bracket on the
setter itself -- and it is built (As built, below).

**(1) The object FUN_142819D90 writes** is a member at a fixed offset of
a long-lived engine object: no global, nothing transient. FUN_1401EA920
(no code caller; its xrefs are data, 0x144DDC7B8 an .rdata slot) fills
two: `FUN_142819d90(param_1 + 0xf4b,...` (01EA920:232) and, after
`pppplVar13 = (longlong ****)(param_1 + 0x4479);` (:233), the same at
:251 -- this+0x7A58 and this+0x223C8, 0x1A970 apart, the context's own
size (its last field is +0x1A96C, 2819D90:52). Both are constructed once
in place: FUN_1401E4FD0, the only other writer of the view count
+0x1A940 (stores_1a940.txt; decompiled for this entry with Ghidra
-readOnly, not saved under analysis/decomp) is the constructor --
`param_1[6] = _DAT_144e2f880;` (+0x30 = 1.0), 64 view slots,
`FUN_14489a600(param_1 + 0x3508,0,0x100);` (the bit table),
`param_1[0x3528] = 0;`, `return param_1;` -- called twice from
FUN_1401E42C0 (0x1401E4E79, 0x1401E4E85; <- FUN_1401F0480, FUN_1401F04B0).

**(2) The builder's ctx is that object.** The part test's param_1[5] is
the builder's param_2: `local_440 = param_2;` (42B4420:240), then
`pfStack_360 = (float *)local_440;` (:498), the sixth qword of the block
`&local_388` passed at :512; 3FC0:79 reads `*(float *)(param_1[5] +
0x30)`. The layout is the setter's: the builder walks `lVar39 = param_2
+ 0x40;` to `*(longlong *)(param_2 + 0x1a940) * 0x6a0 + 0x40 + param_2`
(:199-201) and maps view bits through `lVar39 + 0x1a840 + uVar34 * 4`
(:510): 2819D90's count `param_1[0x3528]` (:50), view blocks (param_1 +
8, stride 0xd4 qwords) and bit table (:90).

**(3) FUN_142819D90 runs every frame.** FUN_1401EA920 is its only code
caller (its other three xrefs are data). Every call builds up to 64
source views per context on its own stack (`appplStack_2b888 [11136]`,
`appplStack_15c78 [11136]`: 64 x 0x570 bytes, 01EA920:70-72, filled at
:164-176); the setter re-copies each into the context (2819D90:79-103;
FUN_142819F70 copies the camera, `uVar8 = *(undefined8 *)(param_2 +
0x30);` ... `param_1[0xa8] = uVar8;` = view +0x540, 2819F70:59/65, and
the planes), rewrites the count and the bit table, and last of all
`*(float *)(param_1 + 6) = DAT_144e2f880 + (DAT_144e2f880 - *param_5);`
(:108), unconditionally, from x = the float at +0x124 of the render
component's settings (01EA920:137-157, passed at :223/:230). The three
EDVRGATE captures (three consecutive frames each, parked at Cranfield;
the gate files' view dumps and builder rows):

| capture | ctx (every builder call, frames 2-4) | s | views | eye cameras moved | bit table |
|---|---|---|---|---|---|
| 152632 | 0x54DCACA2A0 | 1.0 | 10, 10, 10 | 1.21, 0.86 mm | same |
| 165433 | 0x1C44ECA450 | 1.0 | 10, 11, 11 | 0.79, 0.20 mm | rebuilt 2 -> 3 (eye bit 22: view 5 -> 6) |
| 012514 | 0x2E752CA810 | 1.5 | 10, 10, 11 | 0.46, 0.13 mm | rebuilt 3 -> 4 (5 -> 6) |

All six frame transitions rewrote the context's view records (both eye
cameras at view +0x540, the planes of 2-4 views); two rewrote the count,
whose only writers are the constructor and FUN_142819D90. The context is
persistent; its LOD scale is not: the engine sets it from the settings
before every frame's tests.

**(4) Other readers of +0x30** in the four decompiles: the record test,
`*(float *)(lVar4 + 0x30)` with `lVar4 = *(longlong *)*param_1;`, the
ctx (4308B30:47, :67-68). The builder reads no ctx+0x30 itself (its
`+ 0x30` at :315, :588 and :666-677 are the model's sub-table and a
bucket's list tail) but hands the ctx to FUN_142817260 (:308), not
decompiled here.

**Why the brief's design cannot act.** A write at the builder bracket is
overwritten by 2819D90:108 before the next frame's tests. Within a frame
the record test's results are read by the builder's own callers
(4320340:64-95, 4321940:59-86), so each record's test runs before its
builder call, at the game's s; the part tests run at the game's s until
the frame's first builder call writes; `game re-writes` would count once
a frame per context. The shadow copy's premise (the game writes only on
a slider change or a re-apply) is false. The disagreement check would
still read 0 (it recomputes at the value each test read), so the log
would not show the gap. Section 8's shadow is unaffected: it only reads
s, always the game's.

- ruled out: FUN_1401EA920 as "a content/load path, not per frame" (the
  perf doc's 2026-09-20 20:25 entry, the flicker doc's), because the
  context it builds is rewritten between consecutive frames while parked
  and the only other writer of its count is its constructor.
- ruled out: acting by writing ctx+0x30 at the draw-item builder,
  because FUN_142819D90 rewrites it from the settings every frame, and
  each record's test runs before its builder call.

**Where the evidence points (decided by the overseer, built below).**
FUN_1401EA920 builds both contexts (:232, :251) before it hands them on
(:253-271), so the one write every test of the frame sees is right after
FUN_142819D90 returns: a post-forward bracket there scales the value the
engine just wrote by k. s_game is then exactly that value, every frame;
when k returns to 1 or EDVR stands down, the next frame's setter writes
the game's value back itself; the record test is covered. Of the two
questions left open: only the builder's context is written (the other
context is rebuilt too but never reaches the builder), and FUN_142817260
reads only the bit table and view +0x580 from the ctx
(decomp_2817260.txt:47-48), not +0x30. Not chosen: writing x itself
(+0x124, where FUN_142855A50 stores the player's LODDistanceScale, section
4): it bypasses that clamp, and the settings path owns it.

### As built (2026-09-23, not flown)

**The bracket** (kinematic_eval_hook.cpp): `setterObserved` on
FUN_142819D90 through its own relay cell, patched once per process when
the governor attaches (`ensureSetter`), standing down alone. Its
build-keyed signature: build 332841's PE timestamp and size; the prologue
`48 89 5C 24 20 55 56 41 54 48 83 EC 20 49 8B 28` (read from both
installs' exe, sha256 e6be8bbe...), of which only `mov [rsp+20h],rbx`
moves to the trampoline -- five bytes, one instruction, not RIP-relative;
the function's eight in-body branches land at +0x0AE..+0x1B0, none inside
it, and its code callers CALL the entry; `mov rbx,rcx` at +0x24 (the
context held to the end); `movaps xmm1,[1.0]; mov rax,[rsp+60h]; movaps
xmm0,xmm1; subss xmm0,[rax]; addss xmm1,xmm0` at +0x1B0 (s = 1 + (1 -
*param_5)); `movups [rbx+30h],xmm1` at +0x1C7; and the epilogue after it
through the only `ret` at +0x1D8 (nothing stores after the scale). The
callback forwards all seven arguments (three on the stack) and hands
param_1 to `lodGovernorSetterObserver`. The builder bracket and the part
relay are unchanged.

**The write** (lod_governor.cpp, the engine's thread, two calls a
frame): read ctx+0x30 (SEH) = s_game. Only a context the builder observer
has registered (a table of 8; a ninth stands acting down): the setter
rebuilds two contexts a frame and the builder uses one. s_game is held to
0.25..8 (else counted `implausible`, left alone). While acting and k > 1:
VirtualQuery once per context (committed, read-write, no guard page),
then s_game x k is stored under SEH; a refused page or a fault stands
acting down for the process, logs once and writes the game's value back.
k is the policy's, computed at the frame boundary, which never writes
engine memory. Disable (game, observe switched on) and FreeLibrary
teardown (`lodGovernorShutdown` from `shutdownVScreenFixes`) write s_game
back once to every context still holding exactly EDVR's value, guarded;
otherwise the engine's own store each frame is the restore.

**Modes.** `game` off; `auto` the governed k, acting; `reduced`
(`Policy::setFixed`) k = k_max from the first frame with >= 200 builder
records, 1 after 30 frames under 150, no ramp and no frame-work steps;
`advanced.settlement_detail_observe = 1` computes and logs everything and
writes nothing. The contract is 260 keys.

**Counting.** A test ran at EDVR's scale iff the value it read equals, bit
for bit, what EDVR wrote after the setter's last call for that context.
The disagreement gate recomputes at the value read and must read 0. The
price, one quantity: observing, an engine pass that fails at s x k (`would
drop`); acting, an engine reject whose LOD term fails at the held scale
and passes at s_game, whose screen-size term passes (0.5*(1.0*(A*d + B))
<= r, the part test's +0x63..+0x7F bit for bit) and whose plane test
passes -- the engine's own FUN_1404F4E10, called after the 16-byte check
the cull gate probe makes (`dropped (the game's setting would have kept
it)`; `plane test not run` without it). Level changes are against s x k
observing and against s_game acting; records alike (FUN_144308B30).

**What acting cannot see.** A record that lost an eye at EDVR's scale has
that bit cleared in rec+0x208, and the traversal keeps no pre-test mask
(4312040:135-165): it cannot be told from a view never tested. A record
with no view left never reaches the builder. Their parts are never tested
for that eye. Acting's `dropped` is therefore a lower bound; the whole
effect is the fall of `engine passed` (parts and records, per eye) from a
k = 1 window, and the census eye draws.

**Other readers the scale reaches.** FUN_144312040:166-184 reads
ctx+0x30 (`pfVar5[0xc]`) for a main-view LOD pick on the context's header
(+0x00 camera, +0x10/+0x20 A/B, the setter's copy of the main view).
Every reader sees what a slider beyond its range would give: s up to 6.0
(k_max 4 on s 1.5), a value the game itself never produces.

**Log lines** (the rig's `--self-test --print-log`; every line within
1166 characters, the worst configure line 1113). Tags: `acting` |
`observe only, never writes` | `acting stood down, observing` | `cannot
act, observing`. Configure: `settlement detail: on (auto: acts by scaling
the game's LOD scale right after the engine sets it each frame
(FUN_142819D90): the game's value x k) -- k in [1, 4.00] ...; frame work =
...; Hooks: builder FUN_1442B4420 hooked, part test FUN_1442B3FC0 hooked,
LOD-scale setter FUN_142819D90 hooked, plane test FUN_1404F4E10
matched.`; observe: `on (auto, observe only: never writes
(advanced.settlement_detail_observe = 1))`; a refused setter: `on (auto,
but it cannot act: the LOD-scale setter hook stood down; ...)` and
`STOOD DOWN (<why>)` in the hooks. Once per context: `settlement detail:
LOD scale scaled: game s 1.500 -> 1.575 (k 1.05), ctx 0x...`. Steps:
`settlement detail (acting): k 1.00 -> 1.05, up: ...; 250 builder
records, frame work = caller work per cycle: 12.50 ms vs period 11.11 ms
-> LOD scale s x k 1.575.`; reduced: `k 1.00 -> 3.00, reduced: in a
settlement (>= 200 builder records), k = k_max at once; ... -> LOD scale
s x k 4.500.` The 30 s header adds `LOD scale: game s 1.500, held 3.000
(k 2.00); setter calls 5458 (scaled 2699) on 1 pointers (called with 2;
builder contexts 1); implausible 0; faults 0` and, with k above 1 and no
setter call or none scaled, `NOT ACTING: k rose above 1 but
FUN_142819D90's hook ran 0 times, so nothing was written`. Acting eye
lines: `parts tested 10.0/frame (9.9 at EDVR's LOD scale), engine passed
0.1; dropped (the game's setting would have kept it) 9.9/frame (max 10),
LOD level changed from the game's ...`. A stand-down: `settlement
detail: acting STOOD DOWN for this process: <why> (ctx 0x...)`. Off:
`... the game's LOD scale written back to N context(s))`.

**Rig** (tools\lod_governor_test, 153 checks): reduced's policy; the
screen-size term; the setter bracket on a fake context and a fake setter
storing 1 + (1 - x) (a context the builder never used is never written,
game 1.5 -> 3.0 at k 2, a k step lands at the next rebuild, k = 1 leaves
the engine's value, a slider change is taken as stored, 12 and NaN left
alone, observe never writes, restore never overwrites a value the engine
stored since, the ninth context and a read-only page stand acting down);
the acting counts (a kept reject with the plane test passed, failed,
missing; under a pixel; a level change; the gate at EDVR's scale; a
record that lost both eyes); and acting end to end with a fake engine
that tests at whatever the context holds (the lines above, observe on and
off, game writing back, reduced entering and leaving, a setter that never
fires). Cost, hot memory: setter observer ~15 ns a call, part observer
~17 ns observing and for an acting dropped reject with the (fake) plane
test.

### What the first acting flight must show

Parked at Cranfield (the Leg C pose), `fix.settlement_detail = auto`,
observe 0, k_max 4 (the game's s 1.5 at the slider's floor):
1. `edvr_log.py --expect-build HEAD` exits 0; the configure line says
   `acts by scaling the game's LOD scale right after the engine sets it
   each frame (FUN_142819D90)`, with the setter and part test `hooked`
   and the plane test `matched`. A setter `STOOD DOWN` means no acting:
   not evidence.
2. One `LOD scale scaled: game s 1.500 -> 1.575 (k 1.05)` line after the
   first step.
3. Step lines with `-> LOD scale s x k`, k ramping; each summary's `held`
   following `game s` x k (one frame's lag); `setter calls` about twice
   the frames, `scaled` about the frames at k > 1, `on 1 pointers (called
   with 2; builder contexts 1)`; implausible 0, faults 0; no `NOT
   ACTING`, no `STOOD DOWN`.
4. The frame work mean FALLING as k rises, toward under the period, and
   the runtime's caller_wait_fps rising toward 90 (caller work 14.4 ms on
   the first shadow flight, 11.9 at Leg C; the table prices ~1.2 ms at s x
   k 3 and ~2.1 ms at 4.5). auto should settle where the frame work sits
   between period - 1.0 and period + 0.3 ms, or at k_max.
5. Disagreements at the LOD scale the engine held: parts 0 and records 0
   per eye (or a handful at thresholds); dispatch disagreements 0.
6. Dropped parts per eye per frame against the table: ~2.5k at s x k 3,
   ~4.7k at 4.5 -- a lower bound while acting, so read them beside the fall
   of `engine passed` per eye from the first (k = 1) window and the census
   eye draws.
7. Builder records per frame staying >= 200 at k_max: acting removes
   records the cascades do not keep, and under 150 for 30 frames would put
   k back to 1 and oscillate.
8. In the headset: popping at each 0.05 step, thinning beyond ~100 m,
   anything present in one eye only (both eyes' tests read the one scale),
   and anything else that changes with detail (FUN_144312040's main-view
   pick reads the same scale).
9. Switched to `game` mid-flight: `the game's LOD scale written back to 1
   context(s)` and the detail back at once.

### Refinements after the first acting flight (2026-09-23)

**The flight** (as the overseer read the log: 03:30 local, build b55e06b,
parked at Cranfield, 90 Hz, the game's s = 1.5 at the slider's floor,
auto, observe 0, k_max 4): k climbed 1 -> 2.70 over 80 s at 0.05 a step
(a step after each 30 consecutive over-budget samples), the caller work
per cycle fell 12.9 -> 10.6 ms against the 11.11 ms period, fps rose from
~50 to 71-75 mean (median cycle one 90 Hz slot), then k HELD at 2.70-2.75
with zero steps in a whole window (the work inside the dead band). 0
faults, 0 disagreements. Sean saw nothing change visually; what he noticed
was that nothing happened for the first minute and a half. Four changes
follow; none is flown.

**(1) A faster start** (lodgov::Policy; built at 58e622f -- its trigger
was replaced by (5) after the 04:23 flight, its size rule stands). The
trigger was unchanged: 30 consecutive samples over period + 0.3 ms in
frames with >= 200 builder records, at most one step a second, the clamp,
k back to 1 after 30 frames under 150 records, down after 30 samples more
than 1.0 ms under the period. The SIZE of an up step: 0.25 (kCoarseQuanta)
when the mean excess (work - period) of the 30 samples behind the step is
more than 1.0 ms (kCoarseExcessMs), else 0.05.
Down stays 0.05; k stays quantised to 0.05 and held to k_max (a 0.25 step
that would pass it stops there: `held to k_max`). The mean is a ring of the
latest 30 valid samples, emptied with the runs (a bad sample, leaving the
settlement, on foot), so at a step it is exactly the triggering run's
latest 30. Far from the target big steps (the flight's ramp windows sat
1.0-1.8 ms over), near it fine ones and the dead band, so the operating
point is still found from below. Rig (tools\lod_governor_test, 181 checks):
from k 1 at 12.9 ms against 11.11 the ramp reaches 2.50 in 6 steps of 0.25,
5.32 s (90 Hz samples; at 0.05 a step that is 30 steps, 29 s or more); at
11.6 ms (0.49 over) every step is 0.05; the size reads the latest 30, not
the run's older samples; down is 0.05 even 6 ms under; k_max 2.1 stops a
0.25 step at 2.1; a lowered k_max still clamps at once; on a line through
the flight's two ends (12.9 ms at k 1, 10.6 at 2.70) it takes 3 steps of
0.25 then 8 of 0.05 and settles at k 2.15 (11.34 ms, in the dead band) with
no step down. Lines as first built: `k 1.00 -> 1.25, up 0.25: the frame
work ran more than 0.30 ms over the period for 30 samples, their mean 1.79
ms over (more than 1.00 ms: the coarse step); ...` (the current form is in
(5)); the summary's `N up (M by 0.25)`.

**(2) The perf monitor's CPU figure.** The HUD's `cpu`, the Monitor page's
CPU TIME tile and its CPU strip showed NativeTimingSnapshot::applicationMs,
the pre-submit phase: under 10 ms on the flights while the caller thread
worked 11.7-12.7 ms a cycle at 50-55 fps. Now NativePerfHistory::cpuFigure,
readWork's rule: EdvrNativeTimingFrame::callerWorkMs when the frame is
version 5 and callerWorkValid (a version 5 frame without it has no figure,
never the app time beside it), else applicationMs from an older runtime.
Labels: the overlay's `cpu` (the line's width unchanged) or `cpu
(pre-submit)`; the tile `CPU TIME`, "ms game thread per frame", or `CPU
PRE-SUBMIT`, "ms; the runtime DLL is older". A change of figure starts the
average over. The runtime is unchanged; so is the native benchmark
collector's CPU (perf_monitor.cpp's benchmark.cpuMs, still applicationMs).
Rig: tools\native_perf_history_test (91 checks) pins the rule, the
fallback, the invalid version 5 frame and the never-mixed average.

**(3) The cockpit gate.** The arc measured the cockpit only, and the
on-foot deferred frame pacing changes the cycle's shape. On foot --
`journalOnFootKnown() && journalOnFoot()` (journal_watch.h: Status.json
Flags2 bit 0, polled on the frame thread before the boundary), the signal
fix.weapon_stability's pacing keys on (native_frame.cpp:405) -- the policy
holds k = 1 exactly as outside a settlement: at once (Step::Foot), the
settlement, the samples and a pending clamp forgotten, reduced alike;
aboard it starts over (200 records, then 30 samples). One line per
transition: `settlement detail (acting): on foot (the game's Status.json,
the flag the on-foot frame pacing reads): k 2.00 -> 1.00, held at 1 while
on foot -- ...` and `no longer on foot (Status.json) after 11.0 s, 1000
frames held at k 1; the governor resumes ...`; the summary's `held on foot
N frames`; a window wholly on foot reads `k stayed 1: on foot the whole
window`. With the journal watcher off (d3d11.journal_watch = 0, or no
journal folder) on foot cannot be told: one line says so and the governor
runs as in the cockpit. Status.json is rewritten about once a second, so k
can stay raised for a second or so after disembarking; the setter call
before the boundary that sees the flag still writes (one rebuild's lag,
pinned by the rig). Not gated: the SRV (Status.json Flags bit 26),
unmeasured like on foot.

**(4) The shipped default.** fix.settlement_detail compiles to `auto`
(an empty value too) and ships live: `settlement_detail = auto` under [fix]
with `# ui: Settlement detail | choices game, auto=Auto, reduced | live |
menu performance` above it (the F8 menu's Performance page and the
installer's window are generated from it). k_max: kDefaultMax 6.0 (100
steps), kMaxCeiling 8.0. The flight settled at k 2.70-2.75 on s 1.5 (s x k
about 4.1); from the slider's default (s 1.0) the same needs k about 4.1,
past the old ceiling of 4, and the removal levels off between s x k 4.5
and 6 (section 8). Every reader of the scale now sees up to 9.0 (6 on
s 1.5; 12 at the ceiling), a value the game never produces (FUN_144312040's
main-view pick reads it too). advanced.settlement_detail_max and
advanced.settlement_detail_observe stay commented templates (the menu's
developer tier). The contract is still 260 keys; the worst configure line
is 1155 of 1166 characters (after (5)).

**(5) Misses, not runs** (after the 04:23 flight). That flight (fd25af9,
04:23 local, the slider at its default, s 1.0; the overseer's reading):
the coarse ramp worked, k 1.00 -> 2.50 in six 0.25 steps, but over 15 s
(04:24:51 to 04:25:06), one step per ~2.5 s, paced by the trigger -- 30
CONSECUTIVE over-budget samples -- not by the one-second cap. Then, in the
window ending 04:25:44, k held at 2.50 for 30 s with the caller work at
10.99 ms mean while 534 of 1712 samples ran more than 0.3 ms over the
period and the runtime's median cycle was 21.6 ms at 56 fps: a third of
the frames missed their slot and took two, and with a third missing a run
of 30 consecutive misses essentially never occurs (0.31^30), so no step
came. The GPU was 5.6-8.3 ms, not the wall. A miss costs a whole display
slot, so the policy now reacts to the FRACTION of misses. On the latest 30
valid samples (a ring that must be full before any step; a bad sample,
leaving the settlement and on foot empty it), a sample being over when it
ran more than 0.3 ms past the period:
- up when 3 or more of the 30 are over (in a frame with >= 200 builder
  records): 0.25 if their mean excess is more than 1.0 ms, else 0.05;
- down, 0.05, when none of the 30 is over AND their mean is under the
  period by more than 1.0 ms;
- between -- 1 or 2 of 30 over, or none without that millisecond -- k
  holds: the dead band, aimed at under a tenth of the frames missing.

Everything else stands: the one-step-a-second cap, k_max, the density
gate, the reset under 150 records, on foot, reduced, the write path.
Lines: `k 1.00 -> 1.05, up 0.05: 7 of the last 30 samples ran more than
0.30 ms over the period, their mean 0.42 ms over (1.00 ms over or less:
the fine step); ...`, `down 0.05: none of the last 30 samples ran more
than 0.30 ms over the period, their mean 2.11 ms under (more than 1.00 ms
to spare); ...`, and a window with misses that never stepped reads `k
stayed 1: never 3 of the last 30 samples over budget in a frame with 200
records (1-2 is the dead band)`. The summary's over/under counts are as
before. Rig (191 checks): every sample over at 12.9 ms, 2.50 in 6 steps of
0.25, 5.32 s, as in (1); every sample over with their mean 1.5 ms over,
0.25 at the 30th sample and not before; 31% over with the mean at the
period (the 04:23 shape, 9-10 of any 30), 11 steps of 0.05 in 11 s, one a
second; 2 of every 30 over holds; none over with the mean 1.2 ms under,
down 0.05 once a second; 0.5 ms under holds; a down step also waits for 30
samples; 2 of 30 hold and the 3rd steps; the cap and the clamp hold; the
flight's slope as in (1).

What the dead band is not: a hard 10% line. The up test reads a sliding
window on every frame once the cap has passed, so misses scattered at
random below a tenth still fire it. The rig's fixed-seed streams (60 s at
56 Hz from k 1, never a millisecond to spare) take 50 up steps at 10%
misses, 28 at 5%, 8 at 3%, 7 at 2% and 0 at 1%. In flight the misses
fall as k rises, so k should settle where they are rare -- a percent or
two, with more detail removed than a 10% target implies. A fresh window
per decision, or a higher count, would move that; not changed here.

**(6) Refinements 4 and 4b, and the review: the slot, once a second,
measured** (after the 05:05 and 05:53 flights, and the independent review
of 6964c31, `reviews/lod-governor-review-2026-09-23.md` in the main
checkout, gitignored). The evidence. 05:05 (build 8ce12926, the slider at
its default; the mode had persisted as `reduced` from the F8 menu, so k sat
at 6.00; the overseer's reading): in the cockpit the caller work averaged
11.0-12.1 ms with 31-62% of samples over the period, the GPU 5-8 ms, and
the runtime's median cycle was two slots (45-56 fps) for four windows; at
03:45 at the same spot k 2.65-3.0 gave 90 Hz at 10.9-11.6 ms. Two
mechanisms: (a) the trap -- at half rate the game's own per-frame work
grows ~1.2 ms, so a frame that fits at 90 does not fit at 45, and the
compositor needs a run of fitting frames before it returns; (b) the lever
saturates at s x k ~6. 05:53 (close to the buildings): k climbed to 6 in
about 83 s and dropped about 4 of 4,800 passed parts a frame -- the lever
inert at that view. The review found in 6964c31: decisions on old samples
(a frozen or lost timing feed climbed k 1.25 -> 2.75 in 6.6 s on the same
30 samples), the step's size from the all-sample mean (31% misses at a
mean near the period took eleven 0.05 steps), upward drift from a window
read every frame, recovery on a third of a second of good frames; and that
a dropped-part count cannot show the lever inert (whole records culled
upstream never reach the part observer; a nibble change cuts work without
dropping a part). As built (lodgov::Policy, the boundary, readWork):
1. Fresh evidence. A timing sample counts once, at the boundary that
   first sees its new sequence, and only if captured within 2 s. A lost
   lease, an invalid frame, a sample already older than 2 s, a sequence
   unchanged for more than 2 s, or a change of the timing generation,
   source or display period EXPIRES the evidence: the ring and the second
   in progress go, the runs restart, k holds. The summary counts `no fresh
   timing on N frames, E expiries`.
2. The slot. One QueryPerformanceCounter read a boundary; a cycle longer
   than 1.5 periods took two slots. It is GPU-bound when the application's
   GPU render time (the newest valid gpu_frame_timing sample, at most 2 s
   old) was at least the period - 0.5 ms; else unexplained when the caller
   work was under the period - 0.3 ms (the GPU under that or unknown); else
   the CPU's. Only the CPU's trigger; the others are counted per second,
   per summary and in the ceiling line, and neither class is called
   LOD-fixable or LOD-inelastic.
3. Once a second: the cycles completed in each second of wall time with a
   fresh sample; fewer than 20 decide nothing and break no run. A second
   triggers when a tenth of its cycles were the CPU's misses. Up (a frame
   with >= 200 records): 0.25 when a quarter or more were, or the second's
   mean caller work ran more than 1.0 ms over; else 0.05, and 0.05 always
   within 0.25 below a remembered working point.
4. The kick, a trial -- the one kick rule (refinement 4's first, 3eacf58,
   never flew). Ten triggering seconds in a row below k_max put k at k_max
   at once, at most one per 30 s, never in reduced. If the fifth second at
   k_max still triggers, the pre-kick k comes back at once (`restored k X
   after a failed kick`) and no kick follows for 60 s; if not, five clean
   seconds at a time bring k back 0.25 to the pre-kick k + 0.25 (unless a
   second triggers again), then 0.05.
5. Down, a trial, and the working point. After five clean seconds in a row
   (no two-slot cycle of any kind, the mean more than 1.0 ms under): 0.05
   (0.25 while relaxing after a kick). The k before it is the working
   point: a second that triggers within 10 s restores it at once
   (`restored k 3.25 after a failed recovery trial`) and doubles the clean
   seconds the next trial waits for -- 5, 10, 20, 40, 60. A step down that
   holds 60 s without a trigger puts the wait back at 5, counted from the
   step: counted from the last trigger, every 60-second wait would reset
   itself and the cap would never hold. Triggering at or above the working
   point forgets it.
6. Benefit, and the inert lever. Each up step is measured on its whole
   effect, both eyes: the parts tested (a record that loses the eye at
   EDVR's scale never reaches the builder, so its parts leave this count)
   and the parts passed at EDVR's scale (acting, the engine's own passes;
   observing, those less the shadow's would-drop), and the caller work,
   over the 30 frames and samples after it against the 30 before -- and
   only on a stable scene: every frame's builder records within 5% and
   parts tested within 10% of its side's mean, 10 frames or more a side,
   neither mean rising more than 2% across the step (the lever only
   removes), the eye camera (view A's +0x540) within 2 m. Else not judged,
   never a reason to hold. A benefit: the parts tested or passed falling
   1% or more, or the caller work 0.2 ms or more; none: neither, with the
   parts (200 tested a frame or more) and the caller work judged. Two
   steps of 0.25 without one in a row, or four of 0.05, and the lever is
   inert at this view: no up step and no kick (a held second restarts the
   kick's run), said once with the run's figures; one step is retried 30 s
   after the hold or the last retry, or at once when the parts tested a
   frame move more than 20%, and a retry with a benefit re-arms (`the LOD
   lever responds again`). Down still applies. The summary counts the up
   steps judged with a benefit, without one, and not judged.
7. The outcome at k_max, on the same whole effect. A k = 1 baseline of the
   view is taken each second at k 1 in the settlement on a stable scene
   (records, parts tested and passed, caller work, the eye camera); kept
   across on foot, it answers only while the eye camera is within 2 m of
   where it was taken, the scene is stable and neither the records nor the
   parts tested have risen more than 2%; without one, the last judged step
   answers. Five triggering seconds in a row at k_max: once a summary
   window, `at the ceiling (k, s x k) and still missing N of the last 30
   display slots (C the CPU's, G GPU-bound, U unexplained): outcome X
   (against k 1 at this view: tested A -> B, passed C -> D parts a frame,
   caller work E -> F ms); caller work M ms mean, GPU G ms` (`: the GPU is
   the wall` when it is the larger; `the last judged step:` without a
   baseline), X one of `target reached`, `residual benefit`, `no observed
   benefit` or `unknown (no fresh evidence)`. The summary gives the outcome
   while k is at k_max. Nothing acts on it.
8. Kept: without caller work (timing v3/v4) auto holds, said once and in
   each summary; in at 200 records, out after 30 frames under 150 (k 1 at
   once). The summary now counts a settlement's frames at 150-199 records,
   and the rig pins today's behaviour when the lever itself cuts the
   records (680 at k 1 to 140 at k 2: k resets and the ramp repeats, 6
   resets in 30 s); no flight has shown it.
9. Back aboard (after the 06:53 flight: re-boarding from on foot, every
   settlement structure flickered -- the gate's release re-ramped k 1 -> 5
   in ~20 s at the default slider, 0.25 a step, popping LOD levels across
   the whole settlement at each). On foot k is still 1 at once, but the k
   in force when the hold began is kept; aboard, a frame with 200 records
   within 5 s brings it back in one step, `back aboard: k restored to 3.45
   (held on foot 1843 frames)`, no ramp; no such frame within 5 s and it
   starts from 1 as before. A hold that begins at 1 keeps the pending k;
   reduced's own jump to k_max does the same work there.
10. The 07:15 flight (6d34ffd5, the pad, auto, the slider at its default;
    edvr_gfx_20260923_071521.log; the overseer's reading): every rule
    above fired -- 2030 frames without fresh timing and 4 expiries during
    the load, the per-second decisions with their classes, a kick at
    07:17:3x (3.15 -> 6.00 after ten triggering seconds) that failed its
    trial and restored 3.15, the on-foot hold (161 + 1167 frames), `back
    aboard: k restored to 3.90 (held on foot 1328 frames)`, then steps to
    the ceiling with `at k_max now: residual benefit` for three windows.
    Two readings were wrong, and 6 and 7 now carry the corrections: the
    inert line came during the approach (`passed parts 9,720 -> 9,618 ...`
    while the records rose and the per-eye tested count ran 2,109 -> 4,205
    -> 2,477 across windows), and `residual benefit` stood beside an acting
    dropped count of 4 a frame (the record-level removal is invisible to
    that counter; section 9's lower bound).

Lines: the policy is two lines after the configure line (`auto's policy,
decided once a second ...`, `auto's trials: ...`); `up 0.25: 91 of 91
cycles in the last second took two display slots as the CPU's (0
GPU-bound, 0 unexplained), their mean caller work 3.29 ms over (a quarter
or more the CPU's: the coarse step)`; `down 0.05: 5 clean seconds in a row
(...); a trial: k 2.00 comes back if a second triggers within 10 s`;
`kick: 10 seconds in a row ...: from the pre-kick k 1.45 to k_max 2.00
...; a trial, judged on the fifth second at k_max`; the two restores; `the
LOD lever is inert at this view: no observed benefit: tested 500 -> 500,
passed 500 -> 500 parts a frame, caller work 12.90 -> 12.90 ms across two
0.25 steps; holding k 1.50 (...)`; and a second summary line, `decisions:
slots missed N of S (the CPU's C, GPU-bound G, unexplained U); kicks K;
restores R (F after a failed kick); the next recovery trial after W clean
seconds; up steps' benefit B yes, N no, U not judged (...); inert holds H
(retries, re-armed); parts a frame: tested T, passed at EDVR's scale P,
dropped D; at the ceiling with misses T s (at k_max now: X)`. Longest line
950 of 1166.

Rig (238 checks): the trigger's line at 90 Hz, 8 of 82 hold and 9 of 81
step; at a mean at the period 17 of 73 step 0.05 and 21 of 69 (a quarter)
0.25; 30% misses at the period whatever k: 0.25 a second to 3.25, the kick
at the 10th second, restored at the 15th, k_max again at the 26th, spent at
the 31st (no part tests: outcome unknown); 15% misses with k_max 8: kicks
at seconds 10 and 75, the failed first blocking the next 60 s; every cycle
two slots at 12.9 ms until 2.50: six 0.25 steps, no kick; a kick from 2.90
whose trial passes: 12 relaxing steps to 3.15 in 60.0 s, then 0.05; failed
recovery trials restore 3.25 at once, the waits 5, 10, 20, 40, 60, 60; a
failed relaxing step restores 5.75 and ends the relaxation; the GPU at
11.5 ms never triggers, at 6 ms with the caller work over it does, as with
no GPU sample; the caller work 0.5 ms under is unexplained; v3/v4 hold; two
0.25 steps moving neither figure hold, a retry 30 s on without a benefit
keeps the hold, a 25% change in the parts tested retries at once, a retry
past a plateau (passed parts -17%) re-arms; four 0.05 steps without a
benefit hold; 3% fewer passed parts a step, or the caller work 0.25 ms
lower a step, keep stepping; no part tests: unknown, the steps go on;
random two-slot cycles at 3% for 300 s: no step; at 10% for 60 s: 23 steps
in 59 seconds (at exactly the trigger's rate a second's count straddles
the line); the flight's slope (12.9 ms at k 1, 10.6 at 2.70): five 0.25
steps to 2.25 (11.21 ms) by 5.0 s, no kick -- refinement 4b alone kicked
there at 10 s from 2.05 and relaxed only to 3.00, where 1 ms of headroom
ends; the review's four rows (its lines 237-240: a frozen sequence, an
inactive source, a sequence already 3 s old, an explicit invalid source)
all hold k at 1.25 through 6.6 s, each expiring the evidence once; back
aboard, 3.45 held and 2000 frames on foot, 680 records within 5 s: k 3.45
in one step and no step after; records under 200 for 6 s: k 1, then the
ordinary 0.25 ramp; a hold that begins at 1 keeps the pending k; steps in
a loading scene (records 400 -> 680 across each second) are not judged,
six steps and no hold; a stable scene where a step takes 10% of the parts
tested with the passed unchanged is a benefit; at k_max 1.50 against the
k 1 baseline, tested 5,500 -> 4,400: residual benefit; nothing moved: no
observed benefit; the eye camera 5 m away: the last judged step answers;
at the boundary the ceiling line at k 2.00 reads `outcome residual benefit
(against k 1 at this view: tested 500 -> 500, passed 500 -> 0 parts a
frame, caller work 12.90 -> 12.90 ms)`.

### What the next flight must show (the shipped build)

Parked at Cranfield, the ini as shipped (auto, k_max 6, observe 0), the
in-game detail slider at its default (s 1.0); then close to the buildings:
1. `edvr_log.py --expect-build HEAD` exits 0; the configure line reads
   `on (auto: acts by ...) -- k in [1, 6.00], auto's policy on the next
   line`, every hook `hooked`/`matched`, and the two policy lines follow.
2. Step lines naming the slots and whose: `up 0.25` while a quarter of a
   second's cycles are the CPU's misses or it runs over 1 ms, then `up
   0.05`, at most one a second; after ten seconds that do not clear, one
   `kick:` line and k 6.00, then `restored k X after a failed kick` five
   seconds later or relaxing `down 0.25` lines five seconds apart; `down
   0.05` trials, and `restored k X after a failed recovery trial` if one
   fails; in `decisions:`, the CPU's share falling as k rises, and `no fresh
   timing on 0 frames` while the runtime feeds it.
3. During the approach, the decisions line's `not judged` rising and no
   inert line; close to the buildings, once loaded, the inert line with
   the parts tested and passed and the caller work at both ends, no steps
   after it, one retried step every 30 s; any `at the ceiling` line gives
   its outcome `against k 1 at this view` with both ends, or says it had
   only the last judged step.
4. Disembark and board once: one `on foot` and one `no longer on foot`
   line, beside the pacing's own `native frame: begin ... pacing=turbo` and
   `pacing=runtime` lines (the same flag: turbo with no `on foot` line means
   the gate never ran); `held on foot N frames`, k 1 on foot, and aboard
   `back aboard: k restored to X (held on foot N frames)` with no ramp and
   no flicker in the headset.
5. The HUD's `cpu` near the summary's `frame work = caller work per cycle`
   mean, not the ~10 ms pre-submit figure.
6. As before: implausible 0, faults 0, disagreements 0, no `NOT ACTING`, no
   `STOOD DOWN`; in the headset, popping at the 0.25 steps and thinning at
   the larger k.
