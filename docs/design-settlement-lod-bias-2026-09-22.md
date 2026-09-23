# Design: an EDVR-side LOD bias at the settlement part test

Offline measurement, 2026-09-22 (sections 1-7); the shadow governor built
on it 2026-09-23 (section 8: flown twice). Capture: eye run 165433
(Cranfield, parked on the pad, Pimax OpenXR, 3070x3032 an eye, game build
332841), frame 2, 18,267 pool eye draws (EB52 10,690).

## Status

- **State (2026-09-23):** the SHADOW GOVERNOR (section 8) flew twice.
  First (07:25 UTC, parked, 45 fps): k never left 1, because its frame
  work was the pre-submit phase (8.4 ms) while the caller thread worked
  14.4 ms a cycle against 11.1. FIXED and reflown: frame work = the
  runtime's caller work per cycle (timing ABI v5; v3/v4 fall back to app
  work, named so); k_max default 4.0 (s saturates at 1.5); the summary
  prints s x k. Second flight (2026-09-23 02:04 local, fa6565b) confirms
  the fix: k 1 -> 4 in 142 s, mean caller work 11.4-12.8 ms against the
  11.11 ms period, 0 disagreements with the engine at k = 1, would-drop
  saturating at 48-50% of passed parts between s x k 4.5 and 6.0, as the
  exact draw table predicts (33.5-33.7% ceiling). ACTING MODE BUILT
  (section 9, merged 1404b1b), NOT FLOWN: a bracket on the slider's
  setter FUN_142819D90 scales the game's LOD scale by k right after the
  engine stores it each frame (the builder-site write was refuted: the
  setter runs every frame); auto = the governed k, reduced = k_max at
  once, advanced.settlement_detail_observe = 1 keeps the shadow-only
  behaviour. The v3 tables price the LOD-distance half exactly (section
  8); the screen-size half is a weak lever.
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

- **LOD distance, now EXACT** (the v3 tables from reader run 012514,
  keyed by effective s x k on top of the game's s; section 8): 1.875 ->
  318 draws, 1.9%; 2.25 -> 1,123, 6.7%; 3.0 -> 3,222, 19.2% (1.21 ms);
  4.5 -> 5,634, 33.5% (2.11 ms); 6.0 -> 33.7%. It levels off near 34%:
  the building shells' tables (t0 21.38) never drop.
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
- **Open:** the first acting flight (section 9's checklist): whether the
  frame work falls with k and caller_wait_fps rises toward 90, the
  disagreement gate at the held scale, and the visual cost in the
  headset (popping at steps, thinning beyond ~100 m, one-eye artefacts).
- **Ruled out:** see section 6 and section 9 (the builder-site write).
- **Next:** fly `fix.settlement_detail = auto` (observe 0, k_max 4)
  parked at Cranfield on the acting build; read section 9's "What the
  first acting flight must show".

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

## 9. Acting mode (2026-09-23): built at the setter, NOT FLOWN

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
