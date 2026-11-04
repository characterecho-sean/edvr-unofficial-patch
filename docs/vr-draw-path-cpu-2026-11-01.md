# VR draw-path CPU cost (2026-11-01)

## Status

- **Question:** how much of a CPU-bound VR frame is EDVR's draw path (GLOSSARY:
  Draw path), and how much can go without changing what reaches the screen?
  Earlier investigation: `docs\stereo-cpu-sharing-2026-10-16.md`.
- **Build 1 (flown 2026-11-02, `53c965cd`, draft PR #166):** failed the total
  criterion. Shipped items 1 and 3 and the re-issue counter; removed items 2
  and 4 and all three temporary keys. Its DLAA items were worth 0.2-0.4 ms
  against a 0.8 ms noise floor.
- **Build 2 (#168, branch `build2/may-want-byte`; flown 2026-11-03):** the VS
  "may-want" byte and pair test before `GetType`, the stash deleted,
  `drawPathFast` in the instanced hook, instruments (`draw gate holders:`,
  `draw mask:`, `gpu timing queries:`, the F87c `Unmap copy` figures; read with
  `edvr_log.py --perf-windows`). The byte's targets fell about 0.6 ms a frame,
  nothing rose more than 0.2 ms. The marker fix's Unmap copy is the next
  target: 0.80 ms a frame, about 230 KB of write-combined memory per eye.
- **Post-build-2 review (2026-11-03, "Review of build 2's flight and next
  targets", checked in "Check of that review" and "Rebuttal & code trace on A1,
  A2 and B1"):** track A, EDVR-internal cuts (`streamUnmapCopy`,
  `hookedPSSetShaderResources`, `hookedMap`/`hookedUnmap`, `flushIaObserved`,
  the F100 tee's eye-0 trap); track B, engine and hook-chain draw reductions.
  Kept: the marker copy (duplicate eye copies, overlapping windows), the bind
  and map hooks, the post-R1 split, the layer-A fade gate. Ruled out or
  deferred: the tee trap under DLAA (0.08 ms), the sub-pixel floor, the EDHM
  bypass, offscreen/concourse culling until a census/RTV check says what those
  draws are.
- **Build 3 (#180, `cfa5e806`; flown 2026-11-03, "Flight 2026-11-03 (build
  3)"):** both eyes' marker windows merged and copied once per Unmap, no key.
  Passed: coverage held (`outside 0`, `escalated 0`, `faults 0`, named 3.0),
  copied 69% of requested, `streamUnmapCopy` 0.697 to 0.506 ms (R5), nothing
  else up 0.1 ms. B1: 607 of 676 non-eye draws are depth-only into one 512x512
  target (one VS); the rest are three odd-sized colour+depth targets and a
  downsample chain. Ruled out: skipping windows during an escalation
  (escalations only on arrival).
- **Own time (2026-11-03, "EDVR's own time versus the calls it forwards"):**
  the arc's per-function "self" included the D3D calls each hook forwards.
  EDVR's own R1 time is 4.72 ms: the draw path 3.29, `Unmap` 0.39, `Map` 0.26,
  the bind hooks about 0.3. Ruled out: the bind hooks as a large target. A3
  (map/unmap) is worth at most 0.65 ms.
- **Build 4 (designed 2026-11-03, "Build 4: the draw path's own cost", reviewed
  in "Review of the build 4 plan"):** A3 and the draw path in one build, all
  keyless: inlined first tests, a hot `State` block, owner-only per-draw flags,
  the map bounce gated on its own `open_` test (not `!runtimeVrProfile()`),
  counters for build 5, own time in the analyzer. Expected 0.35-0.55 ms. Review
  agrees on all 8 items and adds four mechanical checks (why
  `gpuFrameCommandIfLive` and `flatTemporalCapturing` did not inline, two more
  shape-before-flag sites, `uiDepthHologramOnEyeDraw` / `viewIsSurface`
  empty-table fast-reject, and `census_skip_offscreen` DSV fallback for the
  512x512 depth pass).
- **Confirmed (item 1):** with AA off and the key off, every world draw is held
  as not scene depth, the tee never goes fast and F113 reports `none` 100%. Key
  on: fast path 36-46%, F113 scene meshes 94%.
- **Ruled out:** re-issues as a CPU cost (at most 0.004 ms per sampled frame).
  Item 4 as a saving (0 hits: the binding generation moves between every eye
  draw). Item 2 as a saving (the collector cleared nothing). Idling the whole
  marker tee without the UI layer (its slow path is the only publisher of the
  per-eye world rows, `eyeRowsPublish`). Changing `eyeOfDraw` itself
  (`rowSourceDraw` and five census callers would shift).
- **Left out, tracked:** #151 (`drawPathFast` in `hookedDrawIndexedInstanced`),
  #152 (per-scene subscriber mask), #153 (cross-profile per-bind counter), #154
  (`/GL /LTCG`), #183 (cleanup when the arc closes: worktrees, branches,
  captures, notes, install backups).

## Evidence in brief

Rig: PSVR2, SteamVR OpenXR, 2632x2684 per eye, 90 Hz, RTX 5070 Ti, Ryzen 7
5700X3D, EDHM chained, Motion Smoothing off, game build 334925, live ini
`temporal_aa = dlss`, `eye_depth_capture` unset. Test spot: a station concourse
(13-14k eye draws a frame); nothing below is specific to it.

- CPU-bound: `held_by=game` 96%, vendor xrEndFrame about 0.36 ms.
- 10-31 log (`edvr_gfx_20261031_225516`): DLAA 29.3-29.6 fps, `draw hook CPU`
  5.8-6.1 ms; F7 off 34.7-34.8 fps, 2.7-3.5 ms.
- The draw gate (GLOSSARY: Draw gate) never closes in VR.
- Profile: EDVR's own code is about a quarter of the busy render thread; about
  3.6 ms of the 5.8 ms the hooks read with DLAA is not EDVR's code.
- Tee: about 16.5k calls a frame; 0.47-0.56 ms a frame with its fast path
  (DLAA), 1.1-1.5 ms without (AA off, `fast path 0 of N`).
- F113 `eye rows`, AA off: row source `none` in 349-635 frames a window and
  80k-156k draws with no scene-depth eye; DLAA: about 0 and 5-8k.

## Rules for every item

- No change to what reaches the screen, except item 1 (stated there).
- Nothing tuned to one headset: anything that varies is read at runtime.
- One commit per item.
- Each item has its own direct reading that tells "ran" from "never ran".
- Pure logic gets a red/green rig test first (items 1, 2, 4, and item 3's
  crisp-HUD wrapper).

## Item 1: scene depth without AA

**Cause.** `teeFastFrame` (native_on_foot_f100.h:39-42) publishes the fast
frame only with `!measure && !pending && !bound && !census && !check`, the
frame polled, and `worldOk` set for both eyes. F7 overrides `fix.temporal_aa`
(aa_compare.h:5-8); `depthProbeConfigure` re-reads it, so with AA off the probe
is not wanted (depth_probe.cpp:518-526) and `depthProbeCurrentSceneEyeOf`
returns false (860). `worldDraw` takes its eye from the colour target (`e =
t->eye`, native_on_foot.cpp:29298) but passes `eyeOfDraw >= 0` to `sceneRefWhy`
as "is scene depth" (29307), which refuses with `kSceneNotSceneDepth`. So
`worldOk` never sets, `f44::draw` never early-returns, and no world rows
publish. `pending` and `bound` clear within one draw; `measure`, `census`,
`check`, `polledFrame` do not depend on AA.

**Change.**
1. **Tally (always on).** At the fast-frame publish (native_on_foot.cpp about
   35664) count the first failing term (measure, pending, bound, census, check,
   polled, world 0, world 1), and add `sceneRefused[kSceneNotSceneDepth]`
   (29316; today printed only in measure mode). Print both on the tee's cost
   line after `over %u s`, reset with the fast-call count. Extend the
   `edvr_log.py` parse (about 16778, not end-anchored) with a self-test fixture
   for the new text.
2. **Scene-depth test without the probe (keyed).** A separate predicate, used
   only for `worldDraw`'s `sceneRefWhy` argument when the probe is not wanted:
   the bound depth view's texture has the same size as the draw's colour
   target, whose eye `worldDraw` already knows. Read at runtime; nothing per
   headset. `eyeOfDraw`, `rowSourceDraw` and every other caller stay unchanged;
   mesh rows suffice.
3. **Agreement check (always on, measurement only).** While the probe is wanted
   (DLAA), compute the predicate too and count agree / disagree against
   `eyeOfDraw >= 0`. This validates it against the probe's pair pick, which
   handles twin eye-sized depth targets that the predicate cannot tell apart.

**Behaviour change.** With AA off and the key on, the systems that read the
world rows (ADS aim and sight, scope zoom, the head-containment grid,
`f109TurnTick`, `f87EyeLines`, `pinCycloUpdate`, `geometryNow`) receive rows
they do not get today. The separate on-foot weapons work reads the same rows;
they meet at merge.

**Key.** `experimental.vr_eye_rows_without_aa`. Off: today's refusal. A
mid-frame flip is harmless: `eyeOfDraw` and the fast frame are per frame.

**Shows it ran.** Key on, AA off: F113 row sources other than `none`, the cost
line's `fast path X of N` well above 0, its est. ms a frame down. Key off, AA
off: tally names `world`, `NotSceneDepth` large. If the tally names another
term, the cause is refuted: write it down and stop.

**Fails if** DLAA agreement is not near 100%. Then the fallback becomes a
pick-only mode of the depth probe, separate from `g_wanted` (which ui depth's
`dsvIsSceneDepth`, ui_depth.cpp:878, also reads).

**Test (red first), `tools\native_frame_test`** (its F100 checks). Corrected
from `native_on_foot_hooks_test`, which does not compile native_on_foot.cpp.
The predicate and the tally's first-failing-term function are pure functions in
native_on_foot_f100.h, beside `teeFastFrame`. The predicate: matching depth and
colour sizes pass; mismatched size, no depth view, and the probe-wanted case
(predicate not used) do not. The tally: each term alone is reported, earlier
terms win, all clear reports none.

**Expected.** 0.6-1.0 ms a frame with AA off (the tee keeps its fast-path
cost); none with DLAA.

## Item 2: benchmark collector cleared only when used

**Current.** `NativeBenchmarkCollector` (perf_monitor.cpp:174) holds 16384
entries plus orphans (native_benchmark_collector.h:81, 136). `reset()` calls
`clearEntries()` (native_benchmark_collector.cpp:34) on every perf tick while
native perf history is inactive (perf_monitor.cpp:1119, 1185).

**Change.** A used flag, set where entries are created: `find(create)` and
`findOrphan(create)` (native_benchmark_collector.cpp:94, 117), not in
`observe`, which runs every tick. `clearEntries()` clears the flag. `reset()`
skips only `clearEntries()` when the flag is clear, and still resets `scope_`,
`phase_`, the drop counts and flags. `begin()` is unchanged. `distribution()`
only reads, so a clean collector is all zero.

**Key.** `experimental.vr_cpu_benchmark_idle`. Off: clears every tick.

**Shows it ran.** The perf line gains clears done and skipped. This runs in the
Present-time perf tick, outside the draw hooks, so it cannot move `draw hook
CPU`; it is judged by this count.

**Test (red first), `tools\native_perf_history_test`.** A clean collector's
`reset()` does not clear; after an entry is created it does and every entry
reads zero; `reset()` still resets phase and scope; key off always clears.

**Expected.** 0.10-0.14 ms a frame, wherever native perf history is idle.

## Item 3: two early exits inline

No `/GL` or `/LTCG` (#154), so calls across source files never inline.

- **`uiLayerCrispNoteEyeDraw`** (ui_layer.cpp:5373). In VR the caller
  (vscreen.cpp:2536) already checks `uiLayerCrispOn()` and `ctx` is never null,
  so every eye draw enters the body and leaves at the shape test (`kind`,
  `count == 3`, `instances`, `startInstance`, 5378). The inline wrapper calls
  the body when `detail::g_uiLayerCrispPending` is set or that shape test
  passes, and otherwise returns false. Corrected: the wrapper cannot do the
  body's two clears itself, because `g_crispPending` (ui_layer.cpp:4463) is in
  the anonymous namespace. Every path sets and clears it together with the
  header flag (5361-5369, 5375-5376, 5485, 5673, 6825, 6937, 7064), except
  `uiLayerCrispToneBegin`'s null-context return (5670-5671), which cannot
  happen in VR. So a clear flag means nothing to clear. The flat caller
  (flat_ui_layer.cpp:371) is unaffected. Test (red first),
  `tools\ui_layer_device_test`, which compiles ui_layer.cpp whole: a
  non-matching draw with nothing pending does not enter the body, a matching
  one does, and a non-matching draw while a tonemap draw is pending clears it,
  as today.
- **`nativeOnFootLabelIssue`** (native_on_foot.cpp about 30494, called at
  vscreen.cpp:4457). `g_labPending` is in the anonymous `f44` namespace
  (30188), so a header cannot read it. Corrected: no mirror. `valid` leaves
  `LabPending` and becomes one inline `std::atomic<bool>` in native_on_foot.h,
  relaxed, which every site uses (30257, 30305, 30314, 30495-30496, 35879), the
  vscreen call site included. The body keeps its full test. The 35879 reset
  runs in `shutdownDeviceHooks` (device_hook.cpp:3313, by way of
  `shutdownVScreenFixes`), on the thread that unloads EDVR and possibly under
  the loader lock, not on the render thread. That is why the flag is atomic; a
  relaxed load costs the same as a plain one on x64. There is no rig, because
  no rig compiles native_on_foot.cpp; with one flag there is nothing to drift.

**Key.** None: always helps, and a key would add a branch to the path. **Shows
it ran.** Only in total `draw hook CPU`. **Expected.** About 0.1 ms a frame
with DLAA.

## Item 4: depth surface slot cached

**Current.** `uiDepthSampledSurfaceSlot` (ui_depth.cpp:2419) calls
`viewIsSurface` on PS SRV 0-3 for each qualifying eye draw (vscreen.cpp 4108,
4119); `viewIsSurface` alone is 0.10 ms a frame with DLAA.

**Change.** Remember the answer while all of these are unchanged: the PsSrv0..3
bind generations (or `bindingEyeKeyGeneration()`, binding_shadow.h 138-153);
`g_classifierEpoch` (ui_depth.cpp:335-342), which already bumps on every
surface-table write, on configure and every frame boundary; `g_uiDepthOn` and
`g_uiDepthStoodDown`; the key. `viewIsSurface`'s own 120-frame view memo is
safe only because `bindingFrameBoundary` bumps every slot's generation each
frame; the cache inherits that. Known existing gaps (not new): a null-array
`PSSetShaderResources` (vscreen.cpp about 3110) and `g_flatComputeInternal`
binds skip `bindingSet`.

**Key.** `experimental.vr_cpu_depth_slot_cache`; a change clears the cache.
**Shows it ran.** Hits and misses per perf window. **Test (red first),
`tools\ui_depth_test`.** Repeat call is a hit; each input changed alone forces
a miss; key off always recomputes. **Expected.** 0.1-0.2 ms a frame with DLAA.

## The re-issue counter

Names the owner of the unexplained ~3.6 ms. Many re-issues bypass
`pureDrawReissue` (vscreen.cpp:3908) and call the real draw directly. All
re-issues go through one counted helper with a caller name: motion vectors
(`screenMotionDraw`, `screenMotionUiDraw`, 5501-5502), weapon motion (5440),
panel curve (4047, 4405, 4485, 4601), loader panel (4369), quad split
(3744-3807), flat UI and sun (5135-5138, 5217-5220), ui depth (4553, 4570,
4572, 4581, 4587), star glow, blend probe, crisp HUD, world screen,
`uiLayerSecondIssues`, tonemap. Logged as `draw reissue CPU:` beside `draw hook
CPU:` (perf_monitor.cpp:1216), timed on the same sampled frames and stride
scaling (1199-1216) so the lines compare. No key. Traced end to end: no
re-issues prints 0, never an absent line.

Review fixes (build 1): the draw hook clock now stops at the game's own draw,
so the weapon-motion re-issue is inside `draw hook CPU:` as the line says (the
hook figure rises by that cost; it is EDVR's own work). The splash dim's second
issue and the FSS reveal replay go through the helper (`splash_dim`,
`fss_reveal`). `flat_ui` is outside the hook figure (the flat branch has no
hook clock): labelled, not clocked. `loader_panel` never prints (its substitute
ignores the draw it is handed); kept as a name, noted in the tally's reading
guide. The ms are the draw call only, not the Begin/End state work. `--tally
reissue` flags a GPU census re-issue section that ran against windows counting
none of its callers. The benchmark collector counts clears at the idle tick and
the native-menu tick; the line says so. `--perf-windows` drops the window after
a flip logged at a window's end frame as well. The depth-size agreement check
(item 1, DLAA) resolves through `bindingResolveAgreement`, a fault budget of
its own, so it cannot spend the probe budget the world rows use. The slot cache
keeps its `on` and `stoodDown` inputs: both are constant at the lookup, but the
invalidation across a gap where UI depth was off rests on them as much as on
the epoch, and removing them is not worth a rig rewrite for one compare each.

## The temporary keys

**Closed 2026-11-02 (cleanup, #157).** What shipped and what went:

- `experimental.vr_eye_rows_without_aa` (item 1): removed, the path as it ran
  with the key on is the behaviour. `worldSceneDepth` lost its key argument;
  the F100 rig dropped its key-off cases and kept the on cases and the DLAA
  agreement counting.
- `experimental.vr_cpu_benchmark_idle` (item 2): removed with the used flag,
  `reset(skipCleanClear)` (always clears, as before #161), the `benchmark
  collector: ... entry clears` line and its `edvr_log.py` parser and row.
- `experimental.vr_cpu_depth_slot_cache` (item 4): removed with
  `depth_slot_cache.h`, `uiDepthSetSlotCache`, the `depth slot cache:` line,
  its rig and its `edvr_log.py` parsing; the UI depth pass works the slot out
  every call, as before #162.
- The `vr draw path key:` line, `vrDrawPathKeySweep` and `parse_perf_keys` are
  gone (nothing uses them). `edvr_log.py --perf-windows` tables the `draw hook
  CPU:` windows with no dropped-window logic.
- Kept: item 3, the re-issue counter (#159) with `--tally reissue`, and the
  `tee held (#155)` tail on the tee cost line.

The rest of this section describes the keys as they were flown.

- In `[experimental]`; off restores today's behaviour exactly. The code default
  is on; it reaches the game because install leaves the live `edvr.ini` alone
  and these keys are absent from it.
- Each has a `# TEMPORARY` comment block (live; removed when this closes). No
  `# dev: hidden`, so each shows on the F8 Experimental page under
  `menu.developer = on`.
- Read once per config sweep into a relaxed atomic (the `g_aaOffDraw` pattern,
  vscreen.cpp:6162). A stale read only falls back to today.
- Each change logs one line with the key, its value and the frame number.
  `edvr_log.py` drops any perf window that contains a flip.
- A menu flip writes the live `edvr.ini` and the `%LOCALAPPDATA%\EDVR` mirror;
  removing the keys means cleaning both.

## Flight plan

One session, no restart, keys flipped from F8, measurement families off
(`measure` blocks the fast path). Legs of 4 minutes at the 10-31 spot:

1. DLAA, all keys on; then all keys off.
2. F7 (AA off), all keys on; then all keys off.
3. Repeat 1 and 2.
4. F7, only `vr_eye_rows_without_aa` off.
5. A short cockpit leg, all keys on: no-regression only.

First `python tools\edvr_log.py --target steam --expect-build HEAD --version`,
then `--health --since/--until` per leg. Expected: in keys-off AA-off legs
`_hj_f100_tee` reports NEVER APPLIED (exit 3) by design.

Weapons cannot be drawn in the concourse, so ADS aim and scope zoom with item 1
on are unchecked; that waits for a flight where weapons can be drawn, or for
the weapons work.

## Pass criteria

- The log is from this build.
- Each item's own reading shows it ran: item 1 the tee's est. ms a frame down
  and F113 sources not `none` (key on, AA off); item 2 clears skipped; item 4
  hits; `draw reissue CPU:` present with its callers.
- Item 1's cause holds (tally and `NotSceneDepth`, keys off, AA off) and DLAA
  agreement is near 100%.
- Total: `draw hook CPU` keys on below keys off in the same session, by more
  than the spread of that mode's clean windows.
- fps keys on no more than 0.5 below keys off, in every mode.
- No visual regression: on-foot markers and labels with DLAA and across an F7
  toggle; HUD and UI depth.

## Flight 2026-11-02

Build `53c965cd` (`--expect-build HEAD` matched), same rig and concourse spot,
one session, keys from F8. Legs of 4 minutes; windows that contain a flip are
dropped. `draw hook CPU` is the mean of each leg's kept 1800-frame windows; the
tee is `--grep 'tee held'` summed over the leg; fps is the vScreen totals.

| Leg | Mode, keys | draw hook CPU ms | tee est. ms/frame | fast path | fps |
|---|---|---|---|---|---|
| 1a | DLAA, on | 6.12 | 0.68 | 70% | 29.2 |
| 1b | DLAA, off | 5.93 | 0.63 | 70% | 29.7 |
| 2a | AA off, on | 2.89 | 1.03 | 36% | 33.8 |
| 2b | AA off, off | 2.87 | 1.31 | 0% | 32.7 |
| 1c | DLAA, on | 6.19 | 0.62 | 70% | 28.3 |
| 1d | DLAA, off | 5.42 | 0.50 | 81% | 30.4 |
| 2c | AA off, on | 2.61 | 0.87 | 46% | 34.4 |
| 2d | AA off, off | 2.51 | 1.14 | 0% | 34.9 |
| 3 | AA off, only item 1 off | 2.46 | 1.12 | 0% | 35.0 |
| 4 | cockpit, DLAA, on | 1.4-2.9 | none | | 42.0 |

- Total: keys on is above keys off in all four pairs (+0.02 to +0.77 ms). The
  DLAA pairs differ by more than the spread inside a leg, but the scene moved
  too: 1d held about 40% as many world-0 draws as 1c. Fails.
- fps: 1c is 2.1 below 1d, past the 0.5 limit; the other pairs are within it or
  favour keys on. Fails the criterion as written. (This line first called the
  frame GPU-bound; the review below shows it is CPU-bound.)
- Item 1: the tee is about 0.27 ms a frame cheaper with the key on and AA off.
  `draw hook CPU` cannot show it: the tee runs before `DrawClock` starts
  (vscreen.cpp near 5417 and 5421). The rows also unlock: F113 scene meshes 94%
  (was none 100%), reticle depth F85c patched 100% (was NEVER APPLIED), marker
  fix F62c 6% (was NEVER APPLIED). Eye 1's world rows hold about 35M draws a
  leg (`world 1`), so the fast path reaches only 36-46% against DLAA's 70%.
  DLAA agreement: 0 disagree.
- Item 2: `entry clears 0 done / 0 skipped` in every window after the main menu
  (1800 skipped a window there). The collector does not run in flight.
- Item 4: 16 misses a frame with DLAA, none with AA off, 0 hits ever.
- Re-issues: about 28 a frame with DLAA (ui_depth, ui_layer_second, crisp_hud),
  0 with AA off; their draw calls cost at most 0.004 ms per sampled frame.
- Visual regression: pending the pilot's report.

## Review of build 1's flight (2026-11-02)

An independent review re-read the flight and the 2026-11-01 profile (notes
outside the repo: `fable-build2-review.md`).

- **The bound is the render thread's CPU.** The runtime does not hold the game
  (`next_wait_roundtrip` 0.19 ms); the render thread runs for 98.5% of the time
  before the first submit (27377 of 27807 us), and that time is 25.7 ms with
  DLAA against 21.7 ms with AA off. After both submits the frame spends another
  7-8 ms before the next wait (game 5.1 ms, EDVR 1.2 ms in the profile); not
  looked at before.
- ruled out: build 1 as a measurable change, because its DLAA items were worth
  about 0.2-0.4 ms while `draw hook CPU` moved up to 0.77 ms between legs with
  no code change, and every pair flew keys on first.
- ruled out: `draw hook CPU` as the judge for items 1 and 2, because the marker
  tee and the perf tick run outside its clock. The metric for a frame-level
  change is the render thread's time before the first submit.
- ruled out: item 2's premise, because the profile's clear is `clearEntries <
  NativeBenchmarkCollector::observe` (begin or startSampling), not `reset()`.
- ruled out: item 4's cache key, because the game binds pixel-shader SRVs on
  every draw, so a generation over the PS SRVs changes between every pair.
- ruled out: the build-1 function prices as concourse prices, because
  `self_split.json` averaged a non-concourse window (5) with window 12.
  Concourse windows alone put EDVR's own code at 6.2-7.8 ms with DLAA and
  4.0-5.3 ms with AA off; every build-1 price was about 1.7x low.
- ruled out: build 2 as designed (a /GL A/B flight of two separate launches),
  because it cannot resolve 0.3 ms against that noise, and the profile already
  bounds the named cross-TU callees at 0.25-0.35 ms of self time. /GL stays a
  build-only measurement (link time and a profiled A/B).

Where the review puts the gains, largest first:
1. The DLAA per-draw subscriber tail, about 3 ms: about 20 subscribers at
   0.05-0.3 ms each (`uiLayerFamilyOf` 0.27, `hookedEnd` 0.19, crisp 0.12, star
   glow 0.10, `viewIsSurface`, UI depth, engine velocity, screen motion, GPU
   census). One per-draw classification cached on the shader pair, skipping
   subscribers that cannot want the draw (#152), might save 1.5-2.5 ms. Medium
   confidence.
2. The always-on per-draw ladder, about 1.9 ms in both modes (the AA-off
   floor): a VR fast path (#151, #152), 0.5-1.5 ms. Medium-low confidence.
3. `drawtag::streamLoad16`, 0.67-0.81 ms in both captures: the marker fix's
   instance-stream copy at Unmap, reading write-combined memory after the
   submits, outside every clock. Its cost line prints only with the detail log.
   Medium confidence.
- Outside EDVR: EDHM at 2.4-3.2 ms a frame, larger than builds 1 and 2
  together.

Next, cheapest first: re-export the existing ETLs for concourse windows only
and replace the function prices; a config-bisect flight with DLAA, one
subscriber family at a time from F8, judged on the time before the first
submit; then design the subscriber mask (#152) from the measured list.

## Concourse-only profile (2026-11-02)

The 2026-11-01 captures re-exported for concourse windows only (DLAA 7-11 and
15-18, 1003 frames; AA off 9-18, 1270 frames) with the build-identity guarded
analyzer. ms a frame, profiled (profiled R1 runs 24-33% over unprofiled, so
deltas read about 0.5 ms low; the DLAA capture lost 1.29M events). "Innermost"
includes the runtime, driver and EDHM below an EDVR frame; the export cannot
separate them.

| | DLAA | AA off | delta |
|---|---|---|---|
| R1 (before the first submit) | 32.26 | 28.79 | +3.48 |
| EDVR leaf | 7.43 | 4.46 | +2.96 |
| EDVR innermost | 12.90 | 8.92 | +3.98 |
| game exe | 13.0 | 13.3 | |
| EDHM | 2.8 | 3.1 | |

- DLAA delta by owner (innermost): UI layer +1.1 (`uiLayerFamilyOf` +0.41,
  crisp +0.17), UI depth +0.7, engine velocity +0.5, screen motion +0.4, star
  glow +0.2, GPU census +0.1. No function above 0.41.
- Always-on ladder (both modes): `beginPanelOverride`, `forwardWithVerdict` and
  `hookedDrawIndexedInstanced` 2.0 / 1.9, plus the draw lambda 0.83 / 0.81.
- Outside the draw clock: `drawtag::streamLoad16` 0.83 / 0.80; `clearEntries`
  through `NativeBenchmarkCollector::observe` 0.08 / 0.16.
- Unexplained: `f44::draw` 0.79 off against 0.13 DLAA; `hookedCreateTexture2D`
  0.64 in DLAA R5 against 0.05 off.
- This replaces the per-function prices in the item sections above, which came
  from a mixed-window `self_split.json`.

## Build 2: the draw pair mask (#152, #151; designed 2026-11-02)

Decided 2026-11-02 with the pilot: the mask plus two free wins plus a
gate-holder log line; the UI layer family included; a live temporary key flown
ABBA. No change to when the draw gate closes (AA-off scene masks stay out).
Research notes outside the repo: `mask-research.md`.

**Mechanism priced.** The DLAA path makes about 14,000 eye draws a frame. Most
subscribers reject a draw with a hash compare, so the reject itself is cheap;
the cost is the number of cross-TU calls and live-flag loads paid per draw
before each one rejects. One mask test per draw that skips several such calls
is the saving. Estimate 0.8-1.2 ms a frame with DLAA, an upper bound: the owner
deltas in the concourse profile include the work each feature does on the draws
it wants, which the mask does not touch.

**The rule for what may be skipped.** A subscriber is skipped only on a draw
where its uncached code has no effect at all: no counter, no reset, no recorded
state. That excludes UI depth (it resets about 12 per-draw globals that
`forwardWithVerdict` reads, and is already memoized at a 91% hit rate),
`uiLayerNoteOther`, star glow's after-draw count, target sharp, and the marker
tee (its draw serial must advance). `temporalPassCountDraw` stays
unconditional.

### Changes

1. **The mask.** A pure header holds a table keyed on the game's (VS hash, PS
   hash), read from the binding shadow before `engineVelocityBeforeDraw` can
   substitute the bound shaders. Each entry is a set of "may want" bits,
   computed on the pair's first draw from the same pure predicates the
   subscribers use, and stamped with a config generation:
   - screen motion: the UI-draw pairs and the recognize pair;
   - weapon motion (offscreen draws only);
   - the temporal pass rigid-draw VS gate;
   - engine velocity: pairs with no velocity family (the cached hit path's
     probe tick and `familyDraws` count only family pairs; to be confirmed
     against its code, else this bit is dropped);
   - the UI layer family on the HDR target, keyed additionally on the
     target kind (`ui_layer_math.h`'s pure rule); the post-tonemap target
     needs bound SRVs and stays uncached.
   A clear bit skips the call. A set bit runs the subscriber exactly as today,
   including its per-frame checks (`sourceFrame == frame`, live and failed
   flags), which the mask never caches.
2. **Stale-true safety.** One config generation is bumped at the end of the
   config refresh (every F8 and ini change, and the profile switch) and by
   every runtime stand-down that can change a cached predicate. An entry with
   an old generation is recomputed. Any armed instrument (census, object,
   blend, quad or depth probe) bypasses the mask. A missing entry means "may
   want".
3. **Free win, #151.** `hookedDrawIndexedInstanced` takes `drawPathFast` as the
   other three draw hooks do.
4. **Free win.** `fssPanelRectDrawArgs`, a cross-TU call on every instanced
   draw, moves into the one branch of `beginPanelOverride` that reads it.
5. **Logs.**
   - A `draw mask:` line in each perf window: hits, misses and skips per
     bit, and the key state. Written every window, so zeros mean it ran.
   - A gate-holder bitmask beside the `drawGateSet` calls, logged on
     change, naming which term of `drawGateSubscribed` holds the gate.
   - `edvr_log.py --perf-windows` prints both.
6. **Temporary key** `vr_draw_mask = on | off`, live from F8, default on.
   Off runs every subscriber as today. Listed in Status; removed or kept as the
   behaviour once flown.

### Test seams

- The `aa_off_test` rig (it already drives `draw_fast_path.h` and
  `classifier_memo.h`) gains the mask: per bit, "mask clear implies the
  subscriber's predicate rejects" over the known pairs and random pairs; a
  generation bump recomputes; an armed instrument bypasses.
- The `sharp_weapons_test` rig: "the UI family bit clear implies
  `uiLayerFamilyFor` returns none" for every target kind.
- A source-scan test: every term of `drawGateSubscribed` has a holder bit, and
  every stand-down flag the mask reads bumps the generation.
- `edvr_log.py --self-test`: the `draw mask:` and holder lines parsed.

### Flight

One session, DLAA, all on-foot defaults, the concourse spot of 10-31, standing
still facing one spot. Note each leg's start time.
1. `vr_draw_mask` on, 3 min. 2. Off, 3 min. 3. Off, 3 min. 4. On, 3 min.

### Pass criteria

- The log is from this build (`--expect-build HEAD`); `--health` names no
  feature NEVER APPLIED with the key on that applied with it off.
- The `draw mask:` line shows skips in on legs and none in off legs.
- No visual difference between on and off legs.
- Worth keeping if R1 (before the first submit) on legs 1 and 4 is below legs 2
  and 3 by more than 0.5 ms and by more than the spread within each pair.
  Otherwise `ruled out: per-draw subscriber calls as the DLAA cost`, and the
  mask and its key come out.

### Review of the mask design (2026-11-02)

Independent review against the code (notes outside the repo:
`fable-mask-review.md`). Verdict: change it, and do not fly it as a
keep-or-remove test.
- ruled out: the engine velocity bit, because a no-family draw after a
  substituted family draw is the one that restores the game's VS, PS, blend, t3
  and MRT (`beforeDrawSlow` → `slowPath` → `restore`); skipping it leaves
  EDVR's shaders bound. It also loses the 1-in-64 shadow probe.
- ruled out: the temporal rigid-draw bit, because its gate is already five
  inline compares.
- ruled out: the 0.8-1.2 ms estimate, because it summed whole-owner deltas that
  include work on wanted draws. Priced per bit: UI family about 0.3, screen
  motion about 0.18, weapon motion under 0.05, the `fssPanelRectDrawArgs` move
  0.03; about 0.4 ms net (0.3-0.5).
- ruled out: a four-leg ABBA judged at 0.5 ms, because R1 leg means within one
  build-1 session spanned 2.0 ms with DLAA and the scene drifts in steps;
  resolving 0.4 ms would take about 30 legs per arm, so a null would be a false
  "ruled out". The judge should be the whole frame cycle, not R1 alone.
- Corrections:
  - Bake only shader-hash terms, so the mask reduces to a "may want" byte
    stored beside the VS hash at bind (hash 0 means "may want"). That needs
    no pair table, generation or bypass.
  - The binding shadow keeps the game's hashes under substitution, so the
    key-order worry was moot.
  - UI family: the bit applies to target kind 1 only. The panel and loader
    VS, the scope mark and the on-foot HUD always stay "may want".
  - #151 is safe but saves nothing with DLAA.
  - `fssPanelRectDrawArgs`: pass `args` and delete the stash.
- Larger: `streamLoad16` 0.8 ms after the submits (in the frame, not in R1),
  `hookedEnd` +0.19 ms with DLAA (likely EDVR's own GPU timer queries), and a
  line-level split of `beginPanelOverride` (0.80 ms).

### Build 2 as decided (2026-11-02)

The pilot took the reviewed version: one build, one profiled flight that prices
several things.
- **Ships as behaviour, no key:** the VS "may want" byte (UI family on target
  kind 1, screen motion, weapon motion), the pair test before `GetType`, the
  `fssPanelRectDrawArgs` stash deleted, `drawPathFast` in the instanced hook
  (#151).
- **Instruments in the default log:** gate holders on change; mask skips per
  window; marker-fix Unmap copy bytes and time per eye per window; GPU timing
  queries per frame by owner.
- **Marker-fix copy, first read:** the build-1 flight log's F87c lines show
  whole-stream escalation in 12 of 568 ten-second lines, transients only. ruled
  out: escalated whole-stream copies as the steady 0.8 ms, because steady state
  is `escalated 0` with 6-7 windows per eye; the cost is the windowed copy
  itself.
- **Flight:** 5 minutes standing in the concourse, DLAA, with a CPU profile
  capture. Judged on the profile (per-function, about 0.05 ms) and the whole
  frame cycle, not on an A/B.
- Superseded: the `vr_draw_mask` key, the pair table and the ABBA flight above.

## Flight 2026-11-03

Build `277fae1f` with profile symbols, DLAA, Steam, PSVR2. The concourse at
Shamash 1 a, standing still. The log is from this build (`--expect-build
HEAD`). `--health` flags only full outfit (F142) as NEVER APPLIED, the same as
in every build-1 leg.

**Log, five 1800-frame windows (09:57-10:02, unprofiled):**
- R1 26.2 ms, cycle 36.0 ms. Build 1 leg 1a was 25.8 and 34.8, inside the noise
  as expected.
- `draw mask:` about 18,300 draws a frame on the full path. Skipped: screen
  motion's two calls on about 98% of them, the UI family on about 76%, and
  weapon motion on about 7M offscreen draws a window.
- Marker fix Unmap copy: one copy per eye per frame, about 230 KB each, 0.37 ms
  (eye 0) plus 0.43 ms (eye 1) = 0.80 ms a frame, about 0.65 GB/s. The cost is
  reading write-combined memory, not the byte count.
- GPU timing query Ends: 75 a frame. 65 are untagged ("other": temporal pass,
  DLAA, engine velocity, the frame driver), the GPU census 8.5, UI depth 1.4,
  the particle copy 0.7-1.0.
- Gate holders: 13 of 28 (remlok, holo, ui_depth, sun_glare, object_probe,
  particle, backdrop, scrim, loader_panel, wake_pulse, night_vision,
  depth_probe, footprint).

**Profile.** The 300 s capture failed in the save: `wpr -stop` timed out at 180
s merging 7 GB. A 100 s capture (17:26:22Z, 2,773 frames, no events lost)
worked. Notes outside the repo: `profile-b2\table.md`. Baseline: the 2026-11-01
DLAA concourse windows. Figures in ms a frame, old to new:

| | old | new |
|---|---|---|
| R1, profiled | 32.26 | 26.15 |
| EDVR leaf | 7.43 | 4.97 |
| EDVR innermost | 12.90 | 9.54 |
| game exe | 13.01 | 11.12 |
| `uiLayerFamilyOf` | 0.407 | 0.008 |
| `screenMotionUiDraw` + `screenMotionDraw` | 0.211 | 0 |
| `fssPanelRectDrawArgs` | 0.024 | gone |
| `shaderHashMemo` (the byte at bind) | 0 | 0.023 |
| `streamUnmapCopy` (post-submit) | 0.82 | 0.70 |

- The byte's own targets fell about 0.6 ms, at the top of the reviewed 0.3-0.5
  estimate. The rises (`shaderHashMemo`, `hookedPSSetConstantBuffers`, the hash
  map) total under 0.2 ms.
- The whole-EDVR drop (2.5 ms leaf) is an upper bound, not the build's saving.
  The game exe also fell 1.9 ms and EDHM 0.4, so the scene and session differ.
  The old capture also lost 1.29M events.
- The analyzer in the worktree's build lacked `msdia140.dll`, so the analysis
  ran from a copy with it added. That is the same symbol-path gap as the
  2026-11-01 note.
- Next target, by size: the marker fix's Unmap copy (0.70-0.80 ms). Then
  `hookedPSSetShaderResources` (0.90 self), `hookedMap` and `hookedUnmap`
  (0.58, 0.50), and `flushIaObserved` (0.49).

## Review of build 2's flight and next targets (2026-11-03)

Post-build-2 review of `277fae1f` and the 2026-11-03 profile
(`profile-b2\table.md`). The VS "may-want" byte hit the top of its 0.3-0.5 ms
estimate (~0.6 ms off its direct targets at a 0.023 ms bind-time cost), and the
F87c timer settled the `streamLoad16` question: `streamUnmapCopy` alone
accounts for the 0.70-0.80 ms post-submit cost (one copy per eye per frame,
~230 KB per eye at 0.65 GB/s from write-combined memory).

With EDVR leaf down to 4.97 ms in R1 while the full concourse cycle remains
36.0 ms (26.2 ms R1 + 9.8 ms between the first eye submit and the next frame's
start), further gains split into two tracks: (A) the remaining ~3.2 ms of
EDVR-internal leaf work exposed by build 2's profile, and (B) engine and
hook-chain draw reductions that cut time across the game exe (11.12 ms R1 + ~5
ms post-submit), EDHM/3Dmigoto (~2.4-2.8 ms), D3D11/driver (~5.9 ms) and EDVR
together.

### Track A: EDVR-internal CPU targets (for build 3)

1. **`streamUnmapCopy` (0.70-0.80 ms -> < 0.02 ms) — stop copying 460 KB of
   write-combined memory for ~12 marker draws (`src\d3d11\native_on_foot.cpp`,
   `src\d3d11\native_on_foot_f87.h`):**
   - **Why it is 230 KB per eye today:** in `native_on_foot_f87.h:247-249`,
     `kF85Entries = 8192` (64 KB) and `kWinEntries = 4096` (32 KB x 6 windows =
     192 KB). Even with `escalated == 0`, `streamUnmapCopy` unconditionally
     copies the 64 KB `f85` prefix (`0..8192`) plus every 32 KB window ever
     touched by `noteMarkerDraw(eye, startInstance)`. Worse, `noteMarkerDraw`
     buckets `startInstance` into 4,096-entry blocks and `winUsed` is sticky,
     so a handful of marker draws causes 5-6 windows (192-230 KB per eye,
     460 KB total) to be read out of GPU write-combined memory on every frame's
     per-eye stream Unmap.
   - **What actually reads the copy:** `markerDraw` (`VS == B10B032BDFD46700`,
     ~6-12 draws per eye) reads a single 8-byte entry (`streamEntryX(eye,
     startInstance)`) per marker draw — under 200 bytes per frame total, a
     ~2,000x over-copy.
   - **Fix options:**
     - *Tight micro-windows + expiry:* shrink `kWinEntries` from 4096 (32 KB)
       to 64 or 128 entries (512 B - 1 KB) around the actual `startInstance`
       values seen in recent frames, expire windows not hit in the last N
       frames, and gate the 64 KB `f85` prefix copy so it runs only when the
       F85 diagnostic / escalation path actually needs it. That drops the WC
       read from ~460 KB to ~3-6 KB (< 0.01 ms).
     - *Post-submit Unmap check:* verify why `streamUnmapCopy` appears
       post-submit in the profile — if an Unmap of `g_stream.res` occurs after
       an eye's marker pass has already finished (or in R5 after both submits),
       verify whether the next frame's `WRITE_DISCARD` overwrites it before
       `markerDraw` reads it, or if `streamUnmapCopy` is capturing the last
       Unmap of the eye pass.

2. **The F100 marker tee's Eye-0 slow-path trap
   (`src\d3d11\native_on_foot.cpp:35700` and `34075`):**
   - In `nativeOnFootMarkerProbeDraw` and `f44::draw`, `teeFastFrame` only arms
     when **both** `st.worldOk[0] == frame && st.worldOk[1] == frame`.
   - Because Elite renders in blocked eye passes (~17,000 offscreen draws, then
     ~7,000 Eye 0 draws, then ~7,000 Eye 1 draws), `worldOk[1]` is never equal
     to `frame` during the offscreen pass or during Eye 0's entire pass. Thus
     the first ~24,000 `DrawIndexedInstanced` calls of every frame fall through
     `nativeOnFootMarkerTee` into `nativeOnFootMarkerProbeDraw` -> `f44::draw`
     -> `worldDraw`, running `hudIssuerIndex` (16-entry scan) and `rowSourceVs`
     on every draw. (In AA-off, where `worldOk` waits for
     `captureEyeSceneDepthForMarkers` on non-instanced `EB529BFAD4918EC9`
     draws, this also explains why the tee fast-path rate was only 36-46% and
     `f44::draw` was 0.79 ms in the 2026-11-01 AA-off profile.)
   - **Fix:** add a `kVsMarkerTee` bit to build 2's VS "may-want" byte at
     `VSSetShader`, set only for the ~22 vertex shaders that `f44::draw` and
     `worldDraw` actually inspect (`kHudIssuers`, `kMarkerCompositeVs`,
     `engine_velocity_family`, `EB529BFAD4918EC9`, `8BC3FFBCA98BCD4C`,
     `2684F02B9B0BB0DE`). Also allow `worldDraw` to fast-skip once the
     *current* eye's `worldOk[eye]` is already set for `frame`.

3. **The newly exposed bind/map hooks (`2.47 ms` combined in `profile-b2`):**
   - `hookedPSSetShaderResources` (**0.90 ms self**), `hookedMap` (**0.58 ms**)
     + `hookedUnmap` (**0.50 ms**), and `flushIaObserved` (**0.49 ms**) now
     account for half of EDVR's remaining 4.97 ms leaf time.
   - `flushIaObserved` (`0.49 ms`): IA state is dirtied on every VB/IB/layout
     bind across ~33k draws, but only a small subset of draws (panel/HUD/probe
     shaders) ever reads `iaObserved`. Deferring `flushIaObserved` until after
     the VS "may-want" mask check (or only flushing for draws that inspect IA
     bindings) avoids flushing IA state on ordinary scene and shadow draws.
   - `hookedMap` / `hookedUnmap` (`1.08 ms` excluding `streamUnmapCopy`): the
     game calls `Map`/`Unmap` thousands of times per frame on dynamic CBs and
     streams. Adding a 1-check fast reject for untracked `ID3D11Resource*`
     pointers before entering the multi-feature map/unmap dispatch will trim
     this directly.
   - `hookedPSSetShaderResources` (`0.90 ms self`): inspect the per-slot loop
     in `vscreen.cpp` / `binding_shadow` to see how much of the 0.90 ms is
     `AddRef`/`Release` or hash/identity maintenance on slots that no active
     subscriber reads.

4. **Gate holders (`13 of 28`) and `#151` (`drawPathFast`) in VR:**
   - Build 2's log shows 13 holders active in the concourse (`remlok, holo,
     ui_depth, sun_glare, object_probe, particle, backdrop, scrim,
     loader_panel, wake_pulse, night_vision, depth_probe, footprint`). Because
     several of these are permanently subscribed in `drawGateSubscribed`
     (`vscreen.cpp:1784`), `#151` (`drawPathFast` in `hookedDrawIndexedInstanced`)
     cannot fire in VR even with F7 AA off unless ordinary scene vertex shaders
     bypass the gate via the VS mask byte when AA is off, or unless probe/state
     holders (`object_probe`, `depth_probe`, `footprint`, `loader_panel`,
     `wake_pulse`, `backdrop`) unsubscribe when idle.

5. **GPU timing queries (`75 Ends/frame`, 65 untagged "other"):**
   - 75 `ID3D11Query` `Begin`/`End` pairs per frame (~150 D3D11 calls plus
     `GetData` readbacks) explained the `+0.19 ms` `hookedEnd` rise under DLAA.
     Gate the 65 untagged timing queries off when GPU pass timing / diagnostics
     are not actively requested.

6. **Desk check on the R5 / post-R1 gap (9.8 ms between R1 26.2 ms and cycle
   36.0 ms):**
   - Extract the per-function table for the post-R1 / R5 region from the
     2026-11-03 100 s capture (`profile-b2`) to see how the ~9.8 ms between the
     first eye submit and the next frame start splits across Eye 1 tail, game
     post-submit work, and EDVR post-submit work.

### Track B: Engine & hook-chain draw call reductions (bigger FPS levers)

Because ~33,000 draws and >100,000 bind/map calls per frame drive the 11.12 ms
game R1 leaf, ~5 ms game post-submit, ~5.9 ms D3D11/driver/kernel, and ~2.4 ms
EDHM costs (~0.5-0.8 us/draw across all modules), cutting unneeded draw calls
at the engine or proxy boundary is the only way to save 5-10 ms and push VR
concourse/settlement frame rates from ~28 fps into the 40+ fps range:

1. **Filter tiny clutter out of offscreen shadow cascades in `FUN_1442B3FC0`
   (`~5,000-10,000` offscreen draws/frame):**
   - About half of all `DrawIndexedInstanced` calls (~15,000-17,700/frame;
     build 2's log shows 7M offscreen `8BC3..` draws per 1800-frame window =
     ~3,900/frame on `8BC3..` alone) are offscreen shadow cascade and auxiliary
     views, and 45% of `X` draws have `IndexCount <= 64`.
   - In the view loop `FUN_1442B4420`
     (`docs\stereo-cpu-sharing-2026-10-16.md`),
     Eye A is bit 1 (`0x2`) and Eye B is bit 22 (`0x400000`); shadow cascades
     are other bits in `param_3`. In the part test `FUN_1442B3FC0`
     (`kPartTestRva = 0x42B3FC0`), skipping tiny clutter parts (e.g. world
     radius `r < 0.20 m`) when the active view is a shadow cascade prevents the
     part from entering Stage 4-6 submission, EDHM, EDVR, D3D11, and the GPU.

2. **Sub-pixel clutter floor in `FUN_1442B3FC0` (screen-size test only, capped
   by world radius):**
   - The retired LOD governor (`fix.settlement_detail`,
     `docs\design-settlement-lod-bias-2026-09-22.md`) scaled `ctx+0x30` (`s`),
     which multiplied the asset LOD table test `A*(d - r)*s + B <= t0` and
     dropped 4.2 m parts (`0.74 deg`) that had small `t0` values.
   - `FUN_1442B3FC0` has a separate pure screen-size test (`0.5*(A*d + B) <=
     r`)
     before the `t0` table loop. Enforcing a 2-3 pixel minimum projected
     diameter strictly on small props (`r < 0.35 m`, leaving `t0` untouched so
     large structures never pop) leverages the engine's 8.08x submission
     multiplier (`docs\engine-render-pipeline.md`: 2 eyes x LOD ranges x
     material layers): each culled sub-pixel part saves ~8 D3D11 draw calls.

3. **Extend `fix.optimized_stations` to an on-foot concourse keep-cell
   (`src\d3d11\optimized_station.cpp`):**
   - `fix.optimized_stations` currently releases completely on foot
     (`docs\optimized-station-2026-10-12.md`) because the ship-hangar box
     clipped the lift and concourse. While standing in the enclosed concourse,
     the engine still evaluates and submits exterior station superstructure,
     docking bay rings, and hangar pads. Using the node-name cache and rig
     hooks in `optimized_station.cpp` (or the engine's portal callback
     `FUN_14288D1C0` in `FUN_1442B4420`), we can veto exterior/hangar station
     rigs when the player is inside the concourse shell.

4. **Gate `t33` layer-A weathering decals (`vh = 8056C9D5F22007F9`) at their
   pixel-shader alpha-zero fade distance (`~550-1,100` draws/frame, zero visual
   change):**
   - As documented in `docs\engine-render-pipeline.md` (Stage 4), layer A is a
     world-space-projected deferred normal/roughness detail pass on `t33`
     surfaces that lacks an engine LOD gate and submits ~550 draws/frame at all
     ranges even though its pixel shader (`8B5AA6A7E4042AF3` /
     `D617226453887353`) fades its alpha to `0.0` at medium/far distance.
     Dropping layer-A draws beyond their fade-out distance saves 550-1,100
     draws/frame with zero visual difference.

5. **Bypass 3Dmigoto (EDHM, `~2.4-2.8 ms/frame`) on `t33` world/shadow mesh
   draws:**
   - EDHM intercepts every D3D11 draw and bind call to check its
     `[ShaderOverride]` tables, even though EDHM only overrides ship cockpit
     HUD, radar, and lighting shaders. Forwarding the ~5 `t33` scene/shadow
     vertex shaders (`EB529BFAD4918EC9`, `8BC3FFBCA98BCD4C`, `2684F02B9B0BB0DE`,
     `8056C9D5F22007F9`, `C2208C16..`, ~85% of all concourse/settlement draws)
     directly to system `d3d11.dll`'s `DrawIndexedInstanced` would save
     ~1.5-2.0 ms/frame on rigs with EDHM installed.

### Check of that review (2026-11-03)

The review's claims, checked against the code, the build-2 log and the earlier
engine docs.
- **A1, the marker copy:**
  - Confirmed. `kF85Entries` 8192 plus 6 history windows of `kWinEntries`
    4096, at 8 bytes an entry, is 224 KB per eye. The log reads 229 KB a
    copy.
  - Not addressed in the review: why the windows are wide. They are the F85
    window plus the history clusters, because the marker's entries move
    6-14k between layouts (F87c). A window that misses is a marker flicker.
  - So the fix is a design question about coverage, not a constant change.
    It is the strongest item.
- **A2, the tee "eye-0 trap":**
  - The code condition is as stated (`teeFastFrame` needs both
    `worldOk`). The size is wrong.
  - Build 2's #155 counters (11:26-11:27, 10 s lines) show the fast path on
    86% of tee calls (3,495,602 of 4,063,615).
  - What holds the rest: census frames 272k, world 0 141k, world 1 65k,
    check 60k.
  - The slow path's scanned draws cost about 25 ms per 10 s, about
    0.08 ms a frame.
  - ruled out: the tee's eye-0 hold as a large cost, because the fast path
    already takes 86% of calls and the world terms hold about 5%.
- **A3, the bind and map hooks:** unverified; reasonable desk targets. Which
  draws read `iaObserved`, and which resources Map and Unmap track, are the
  first questions.
- **A5, the timing queries:** unverified which of the 65 untagged queries feed
  live features (the frame driver, DLAA, temporal pass). Turning any of them
  off is a removal, so the maintainer is asked first.
- **A6, the post-R1 split:** a desk task on the existing 100 s capture.
- **B1, shadow-cascade clutter:**
  - The premise conflicts with `engine-render-pipeline.md`. The ledger found
    exactly 3 render targets (2 eyes and 1 offscreen) and no shadow or depth
    pass.
  - Offscreen draws are not established as shadow cascades. This needs
    evidence before any design.
- **B2, a sub-pixel floor:** already dead. `design-settlement-lod-bias`: "ruled
  out: the screen-size half of FUN_1442B3FC0 as the settlement lever, because
  at 8 pixels it removes 613 of 18,267 pool eye draws (0.21 ms)". A 2-3 pixel
  floor removes fewer.
- **B3, a concourse keep-cell:**
  - A behaviour change to a feature released on foot because its box
    clipped the lift and the concourse.
  - It needs a census showing exterior station draws while in the
    concourse first.
- **B4, a layer-A fade gate:**
  - A recorded open design input (`engine-render-pipeline.md`, "gate A by
    its own fade distance", about 550 draws a frame, a product decision).
  - Valid. It needs the fade distance read from the PS, and the suppression
    discipline of that doc's 2026-09-21 entry.
- **B5, bypassing EDHM:**
  - Calling the system context under EDHM's 3Dmigoto for some draws skips
    its per-draw work on those shaders. That can desynchronise what it
    tracks, and silently drops any user override that targets them.
  - "EDHM only overrides cockpit HUD" is unverified.
  - High risk to a third-party mod; maintainer's call, not a performance
    item.
- **Numbers:** the review's "~33,000 draws a frame" does not match build 2's
  `draw mask:` count, about 18,300 draws a frame on the full path.

### Rebuttal & code trace on A1, A2 and B1 (2026-11-03)

Agreements with the check above, plus three concrete code findings in
`src\d3d11\native_on_foot.cpp` and `src\d3d11\native_on_foot_f87.h` that solve
the coverage problem on **A1 (`streamUnmapCopy`)** without shrinking
`kF85Entries` or `kWinEntries`:

1. **Agreements (accepted rule-outs):**
   - **B2 (`FUN_1442B3FC0` sub-pixel floor):** Agreed, ruled out. The 8-pixel
     test in `docs\design-settlement-lod-bias-2026-09-22.md` (613 draws =
     0.21 ms) kills this lever.
   - **B5 (EDHM / 3Dmigoto bypass):** Agreed. Skipping 3Dmigoto's draw hook on
     selected vertex shaders risks desynchronizing 3Dmigoto's internal state
     tracking and silently breaking user shader overrides. Dropped.
   - **A2 (`teeFastFrame` eye-0 hold):** Agreed for DLAA (`86%` fast path in
     build 2's `#155` counters, ~0.08 ms/frame). Note for completeness: the
     low 36-46% fast-path rate and 0.79 ms `f44::draw` cost occurred specifically
     with **F7 AA off** (build 1 legs 2a/2c, where `worldOk` waits for
     `captureEyeSceneDepthForMarkers` on non-instanced `EB52..` draws). Under
     DLAA it is minor and rightly deprioritized behind A1 and A3.
   - **B3 (concourse keep-cell) & A5 (timing queries):** Agreed — B3 needs a
     concourse rig/node census first, and A5 needs an owner check before touching
     any query that feeds a live feature.

2. **A1 (`streamUnmapCopy`, 0.70-0.80 ms/frame) — two bugs in the copy planner
   that waste ~60-75% of the write-combined reads with ZERO change to F87c
   window widths:** The check above rightly notes that `kF85Entries = 8192` and
   `kWinEntries = 4096` were sized so 6-14k layout shifts between passes do not
   miss and flicker (F87c). However, inspecting `streamUnmapCopy`
   (`src\d3d11\native_on_foot.cpp:29568-29624`), `streamEntryX`
   (`native_on_foot.cpp:29662-29690`), and `planWindows`
   (`src\d3d11\native_on_foot_f87.h:341-388`) shows why we are reading **458
   KB/frame** (`229 KB x 2`) of write-combined memory when a single
   deduplicated copy of the exact same F87c windows is much smaller:
   - **Finding A1-a (`streamUnmapCopy` copies the same buffer twice per Unmap,
     and `streamEntryX` searches both eyes' buffers regardless of the drawing
     eye):**
     - In `native_on_foot.cpp:29591`, a single `Unmap` of `g_stream.res` runs
       `for (int e = 0; e < 2; ++e)`, calling `planWindows` and
       `streamCopyGuarded` from the **same `base` pointer** into
       `g_streamWin[0]` (0.37 ms, ~229 KB) and `g_streamWin[1]` (0.43 ms,
       ~229 KB). (That is why the F87c log reports "one copy per eye per frame":
       `ss.copies` is incremented inside `for (int e = 0; e < 2; ++e)` during a
       single `streamUnmapCopy` call!)
     - Now look at `streamEntryX` (`native_on_foot.cpp:29665-29672`):
       ```cpp
       for (int e = 0; e < 2; ++e) {
           const nofoot::f87::WinPlan& p = g_stream.plan[e];
           const int w = p.where(entry);
           if (w >= 0) {
               *x = g_streamWin[e][2 * (static_cast<size_t>(p.off[w]) + (entry - p.lo[w]))];
               if (eye == 0 || eye == 1) ++g_streamSec[eye].named;
               return true;
           }
       }
       ```
       `streamEntryX` does not even partition lookups by `eye` — it loops
       `for (int e = 0; e < 2; ++e)` and returns the first window in `plan[0]`
       or `plan[1]` that covers `entry`, because both `g_streamWin[0]` and
       `g_streamWin[1]` are copies of ranges from the **exact same `base`
       buffer** at the **exact same `Unmap`**!
     - Because Eye 0 and Eye 1 draw the same markers at the same or adjacent
       stream clusters, `plan[0]` and `plan[1]` cover mostly identical or
       heavily overlapping `[lo, lo + n)` ranges of `base`, reading the same
       write-combined cachelines over PCIe twice in a row.
   - **Finding A1-b (`planWindows` does not merge or trim overlapping windows
     Even within a single eye):**
     - Look at `planWindows` in `src\d3d11\native_on_foot_f87.h:362-375`:
       ```cpp
       const uint32_t a = h.iv[best].lo, b = std::min(h.iv[best].hi, total - 1u);
       bool inside = false;
       for (uint32_t w = 0; w < out->windows && !inside; ++w)
           inside = out->n[w] && a >= out->lo[w] && b - out->lo[w] < out->n[w];
       if (inside) continue;
       ```
       It only skips a cluster `[a, b]` if `[a, b]` is *already strictly
       inside* an existing window. When it is not strictly inside, line 372
       places a new 4,096-entry window (`winPlace(mid, total, 4096, &lo)`)
       **without clamping or merging against existing windows in `out`**!
     - Worse: when `total < kF85Entries` or when clusters sit within
       2,048-6,000
       entries of `centre` (or near the start/end of the buffer where `winPlace`
       clamps `lo` to `0` or `total - want` at `native_on_foot_f87.h:327-328`),
        multiple 4,096-entry windows and the 8,192-entry `f85` window overlap
       the **exact same `[lo, hi)` entry range** and copy it multiple times
       within the same eye!
   - **Zero-risk fix for A1 (preserves 100% of F87c coverage):**
     - Build one combined list of requested `[lo, hi)` intervals from both eyes
       (`centre[0]`, `centre[1]` with `kF85Entries = 8192`, and all active
       intervals in `hist[0]` and `hist[1]` with `kWinEntries = 4096`), **sort
       and merge overlapping `[lo, hi)` intervals**, and run `streamCopyGuarded`
       **once** per merged interval into a single `g_streamWin` buffer.
     - Coverage of every entry is mathematically identical to today (every
       entry covered by `plan[0]` or `plan[1]` is still covered), while
       eliminating both the 2x eye duplication and all intra-eye window
       overlaps. That alone should cut the 0.80 ms WC read by **50-75%
       (~0.40-0.60 ms saved)** with zero risk of marker flicker.

3. **Clarification on B1 (draw counts and offscreen draws):**
   - On the draw count number: `draw mask: 18,300` in build 2's log counts only
     draws that reached the `draw mask` counter in `forwardWithVerdict` /
     `hookedDrawIndexedInstanced`; the earlier ~33,000 figure was total
     `DrawIndexedInstanced` calls across the full frame cycle (`4.06M tee calls
     / ~180 frames = ~22.5k` instanced draws/frame in R1, plus non-instanced `D`
     draws). Using build 2's `18,300` full-path draws/frame as the baseline is
     cleaner.
   - On what the offscreen draws are: build 2's log states `weapon motion`
     skipped **7M offscreen draws per 1800-frame window (~3,888 offscreen
     draws/frame)**, and `weaponMotionDraw` only runs on `8BC3FFBCA98BCD4C`
     (the primary skinned/rigged prop vertex shader). Meanwhile,
     `docs\engine-render-pipeline.md`'s 3-target ledger was taken at a **parked
     settlement** pose, whereas in the concourse ~3,900 `8BC3..` draws/frame are
     landing on a target where `eyeOfDraw` / `targetEye` is `-1` (offscreen).
   - **Agreement on next step for B1:** before proposing any engine cull on
     those ~3,900 offscreen `8BC3..` draws/frame, we should identify what
     render target / depth buffer (`tw x th`, format, RTV/DSV resource) those
     7M offscreen draws per window are actually rendering into (e.g. holo/avatara
     preview, planar reflection, environment map, or a local shadow map).

### Reply to the rebuttal, and what build 3 carries (2026-11-03)

Every claim below carries its evidence, so it can be checked without trusting
this section. Code references are at `639904db` (build 2, `277fae1f`, plus docs
only). Two logs, both from build `277fae1f` (`version
v0.22.0-rc.4-50-g277fae1f` on line 2 of each), in the Steam game's
`edvr_logs\`:
- **L1** = `edvr_gfx_20261103_095211.log`, the unprofiled windows (`--nth 1`).
- **L2** = `edvr_gfx_20261103_112412.log`, the profiled flight (`--nth 0`).

To reproduce a log figure, run `Select-String` on the log for the quoted prefix
(`marker fix (F87c) eye`, `draw mask:`, `instance stream`), or use `python
tools\edvr_log.py --target steam --nth N`.

#### A1-a: two copies from one Unmap (agreed)

- **Claim:** each Unmap of the stream copies both eyes' plans from the same
  source pointer. **Evidence:** `native_on_foot.cpp:29569` takes `base` once;
  the loop at 29591 runs `e = 0, 1`; 29601 copies `base + plan.lo[w]` into
  `g_streamWin[e]`. `++ss.copies` at 29614 is inside that loop, so each eye's
  `copies` counts the same Unmaps.
- **Claim:** there is one stream Unmap a frame, so each eye's `copies` per
  second is the frame rate. **Evidence:** L2's `draw mask:` windows ending
  14400 (11:26:33.351) and 16200 (11:27:36.972) are 1800 frames in 63.6 s, 28.3
  frames a second. L2's F87c lines over the same minute show 268-299 copies per
  eye per 10 s.
- **Claim:** the lookup does not care which eye's buffer holds an entry.
  **Evidence:** `streamEntryX`, `native_on_foot.cpp:29665-29673`, loops over
  both plans and returns the first hit, whatever `eye` is.

#### A1-b: windows overlap within one eye (agreed as possible; size unknown)

- **Claim:** `planWindows` neither merges nor trims windows. **Evidence:**
  `native_on_foot_f87.h:364` skips a cluster only when `[a, b]` lies wholly in
  an earlier window. 372-373 place a full `kWinEntries` window about the
  cluster's middle, with no check against the others. The F85 window at 344-350
  is placed the same way.
- **Claim:** two history windows can share up to about 2000 entries.
  **Evidence:** `ClusterHist::add` (`native_on_foot_f87.h:263`) joins an entry
  to an interval within `kWinGap` (2048). So two live intervals are more than
  2048 apart, and two 4096 windows centred on single-entry clusters 2049 apart
  share 4096 - 2049 = 2047 entries.
- **Claim:** every planned window is copied at full width. **Evidence:** L2
  11:26:34, eye 0: `windows 5 ... 277 copies, 54460416 bytes`. That is 196,608
  bytes a copy = 24,576 entries at 8 bytes = 8192 + 4 x 4096. Eye 1: `windows 7
  ... 72613888 bytes` = 262,144 a copy = 32,768 entries = 8192 + 6 x 4096, the
  per-eye cap (`kWinCapEntries`, f87.h:249).
- **Not known:** how much of that is overlap. No log line records where the
  windows sit. L2 logs only eye 1's ring (first/last entry, 41,261-45,583 over
  the steady minute); eye 0's entries are never logged
  (`native_on_foot.cpp:34882`).

#### The fix for build 3 (agreed in principle)

Plan each eye exactly as today, then sort and merge both plans' windows and
copy each merged range once into one buffer.
- **Coverage is unchanged by construction.** The merged ranges are the union of
  both plans' ranges, copied from the same Unmap, and the lookup already takes
  any plan's hit (A1-a). So `named`, `outside`, the escalation and the values
  read are the same.
- **No key and no grilling round:** it does not change what the marker fix
  draws. Narrower windows still need the full process.
- Keep `plan[e]` per eye for `windows`, `nearest` and `clusters`, so those
  counters keep their meaning.
- Log requested entries (both plans summed) beside copied entries (the merge),
  and eye 0's ring beside eye 1's. `edvr_log.py`'s F87c parser and its
  self-test follow the line.
- **Test seam:** a rig test over random histories and centres. It asserts that
  the merged ranges hold exactly the union of both plans' entries, and that
  each lookup returns the same value as today's two-buffer path.

#### The size of the saving: not agreed, measured by build 3's flight

- **Claim:** "50-75% (0.40-0.60 ms)" has no data behind it. **Evidence:** the
  overlap it depends on is not logged (A1-b, "Not known").
- **Retracted:** my earlier line that F87c's 6-14k shift means eye 1 sits away
  from eye 0. That shift is eye 1 moving between layouts
  (`native_on_foot_f87.h:242-243`), not a distance between the eyes.
- **Claim:** the cost is linear in bytes, so the saving is (1 - copied /
  requested) of the copy's time. **Evidence:** L2 11:26:34: eye 0 85.514 ms /
  54,460,416 bytes = 1.57 ns a byte; eye 1 111.057 ms / 72,613,888 bytes = 1.53
  ns a byte. Eye 1 copies a third more bytes and takes a third more time. L1
  10:02:17 gives 1.45 and 1.49 ns a byte.
- **Claim:** the logs bound the saving only loosely. In L2 the stream is 65,536
  entries (line 1930: `524288 bytes, 65536 entries`) and today's two copies are
  57,344 entries (24,576 + 32,768), 7/8 of it. The union is at most 57,344 and
  at least 8,192 (one F85 window; each eye's own windows may overlap, so
  neither plan alone sets a higher floor). That is a cut of 0-86%, 0-0.61 ms at
  L2's 0.71 ms a copy pair ((85.514 + 111.057) ms / 277 Unmaps). Only the build
  3 figures narrow it.
- **Test:** build 3's flight. Its requested/copied entries, and
  `streamUnmapCopy` in the profile, give the saving. `outside`, `nearest` and
  `fullNamed` must stay in build 2's range: steady-state L2 shows `outside 0`
  from 11:26:34 to 11:28:15. Whatever is left is the narrower-window design,
  priced from `outside`/`nearest` as planned.

#### The escalation's double copy: ruled out

- **Claim:** while an escalation is on, the windows are copied on top of the
  whole stream. **Evidence:** `native_on_foot.cpp:29582-29590`, then the loop
  at 29591 runs regardless.
- ruled out: skipping the windows during an escalation, because escalations
  happen only on arrival. **Evidence:** L2's F87c lines show `escalated 119`
  (11:26:13) and `68` (11:26:24), then `escalated 0` in every line from
  11:26:34 to 11:28:15. L1's last full lines (10:02:17, 10:02:27) are
  `escalated 0` too.

#### B1: what the "offscreen" draws are (corrected)

- **Claim:** the `weapon_motion` skip count is not `8BC3..` draws.
  **Evidence:** `vscreen.cpp:5520-5521` counts the skip when RTV0 is not
  eye-sized (`!g_state->rtv0Eye`) **and** the may-want byte says weapon motion
  does not want this vertex shader. `weaponMotionWants`
  (`weapon_motion.cpp:186-189`) is the family test, so a skip is a draw
  *outside* the weapon family.
- **Claim:** it includes depth-only draws. **Evidence:** `rtv0Eye` is
  `targetIsEyeSized(bindingGet(BindSlot::Rtv0))` (`vscreen.cpp:2099`, `2227`),
  which returns false for a null RTV (`vscreen.cpp:1272`).
- **Claim:** the count depends heavily on the scene. **Evidence:** L1's steady
  windows (25200-32400) skip about 7.33M a window, 4,070 a frame, of about
  18,290 full-path draws a frame (`32916372` / 1800). L2's window ending 16200
  skips `924299`, 514 a frame, of 14,076 full-path draws a frame (`25336198` /
  1800). Both were called the concourse.
- **Why it matters:** a depth-only pass would land in this count, and
  `engine-render-pipeline.md` found no depth pass at a parked settlement.
- **Test before any B1 design:** split those draws by RTV0 and DSV size and
  format, and by vertex shader. The existing logs cannot do it: `edvr_log.py
  --target steam --nth 1 --tally vh` prints `no draw-census lines (DC/DCO/DC
  frame) in this log`. So build 3 adds a per-window count of non-eye draws by
  target size and format. It rides on build 3's flight; it needs no flight of
  its own.

#### Numbers (corrected)

- **Claim:** "4.06M tee calls / ~180 frames" is wrong; it is about 14,400 a
  frame. **Evidence:** the 4,063,615 tee calls are from 10 s lines at
  11:26-11:27 ("Check of that review"), and L2 runs at 28.3 frames a second
  over that minute (A1-a): 4,063,615 / 283 = 14,359.
- **Claim:** the full-path baseline differs between the two logs. **Evidence:**
  18,290 a frame in L1 and 14,076 in L2 (B1 above). Judge build 3 against L2,
  the profiled flight, and fly the same spot.

#### Agreed for build 3, if Argon accepts the above

1. The merged marker copy (no key), with the requested/copied entries and eye
   0's ring in the F87c line, and the rig test.
2. The non-eye draw count by target, for B1.
3. One profiled standing-still flight at L2's spot.

The order after it is unchanged: the bind and map hooks, the post-R1 split
(desk), the layer-A fade gate (a product decision).

### Argon's reply: build 3 agreed, plus capacity guard and A3 map findings (2026-11-03)

Full agreement on build 3's scope, receipts and flight plan. Every correction
in Opus's reply above checks out against the code and logs:

- **A1 bounds and escalation rule-out accepted:** `streamUnmapCopy`'s cost is
  linear in bytes copied (`1.45-1.57 ns/byte`), so `0-86%` (`8,192..57,344`
  entries, `0-0.61 ms` at L2's `0.71 ms`) is the right pre-flight bound until
  build 3 logs `requested` vs `copied` entries and `eye-0` ring alongside
  `eye-1` ring. Agree that skipping windows during an escalation is ruled out
  (`escalated == 0` in all steady-state windows after the first 20 s on
  arrival).
- **B1 correction and instrument accepted:** verified at
  `src\d3d11\vscreen.cpp:5520-5521` and `src\d3d11\binding_shadow.h:76-82`.
  `vsMaySkip(bindingVsMayWant(), kVsMayWeaponMotion)` is `(mayWant & bit) ==
  0`, so `maskNoteSkip(MaskSkip::WeaponMotion)` counts non-eye draws
  (`!g_state->rtv0Eye`, including `Rtv0 == nullptr` depth-only draws) whose
  vertex shader is *outside* `weaponMotionWants` (not `8BC3..`). Adding the
  per-window non-eye draw counter by RTV0/DSV size and format in build 3 is the
  exact right way to answer B1 on the same flight.
- **Numbers baseline accepted:** `4,063,615 / 283 = 14,359` tee calls a frame
  in L2, and L2 (`14,076` full-path draws/frame, `0.71 ms` `streamUnmapCopy` at
  `Shamash 1 a`) is the reference baseline for build 3.

#### One capacity guard for build 3's merged `WinPlan`

- **Claim:** the merged window plan cannot reuse the single-eye `kWinMax = 7` /
  `kWinCapEntries = 32,768` bounds without risking silent truncation when Eye 0
  and Eye 1 have disjoint windows; it needs `2 * kWinMax` (`14`) windows and `2
  * kWinCapEntries` (`65,536`) entries.
- **Evidence:** in `src\d3d11\native_on_foot_f87.h:247-249`, a single eye's
  `WinPlan` holds up to `kWinMax = 1 + kHistCap = 7` windows and
  `kWinCapEntries = kActiveEntries + kHistCap * kHistEntries = 8,192 + 6 *
  4,096 = 32,768` entries (`256 KB`). In `src\d3d11\native_on_foot.cpp:29601`,
  `g_streamWin[2][f87::kWinCapEntries * 2]` already allocates `65,536` entries
  (`512 KB`, 2 floats per entry) across the two eyes. If Eye 0 and Eye 1 have
  zero overlap (the `57,344`-entry worst case in L2, or `65,536` when both eyes
  carry 6 history windows), the union of `plan[0]` and `plan[1]` has up to `14`
  disjoint intervals and `65,536` entries. Sizing the merged plan for `14`
  windows over a single flat `g_streamWin[2 * f87::kWinCapEntries * 2]` buffer
  uses the exact same `512 KB` static footprint as today while guaranteeing
  zero coverage loss even when the two eyes' window sets are completely
  disjoint. Keep per-eye `plan[0]` and `plan[1]` (or their interval lists) for
  `nearest`'s per-eye `plan[e].distance(entry)` check
  (`native_on_foot.cpp:34092`), while `streamEntryX` resolves `where(entry)` in
  the merged plan.

#### Code evidence for the post-build-3 A3 pass (`hookedMap` / `hookedUnmap`)

While checking the map/unmap path (`0.58 + 0.50 = 1.08 ms` exclusive in L2),
two concrete findings in `src\d3d11\vscreen.cpp` are worth recording now for
the A3 pass after build 3:

1. **Three flat-profile `mapBounce` calls in `hookedMap` and `hookedUnmap` are
   left ungated in VR:**
   - **Evidence:** at `src\d3d11\vscreen.cpp:3511`,
     `flatRuntimeMapBounceNoteMap`
     is gated with `if (!runtimeVrProfile())`, with the comment on lines
     `3509-3510` noting that the VR profile should not pay the call across to
     `flat_runtime.cpp`. However, three sibling calls in the same hooks lack
     `!runtimeVrProfile()`:
     - `vscreen.cpp:3451`: `flatRuntimeMapBouncePreMap(res);` at the top of
       `hookedMap`.
     - `vscreen.cpp:3512`: `if (mapData0) mapped->pData =
       flatRuntimeMapBounceInstall(self, res, sub, type, mapped->pData);`
       immediately below the gated line `3511`.
     - `vscreen.cpp:3638`: `auto bounceLease =
       flatRuntimeMapBounceBeginUnmap(self, res);`
       at the top of `hookedUnmap` (plus `bounceLease.finish()` on return).
   - **What they do in VR:** `mapBounce().setMode(flatmap::Mode::Auto)` is only
     ever called inside `flatRuntimePresent` (`src\d3d11\flat_runtime.cpp:4341`),
     which returns at line `4214` when `!runtimeFlatProfile()`. So in VR,
     `mapBounce()` stays in `State::Pending` with `open_ == 0` for the entire
     session, yet every `Map` and `Unmap` in VR still pays three cross-TU calls
     into `flat_runtime.cpp` (`lines 5417-5465`), atomic acquire loads on
     `open_`, `trip_`, and `state_` (`src\d3d11\flat_map_bounce.h:100, 128, 139, 164`),
     and a `flatcpu::Scope` construction in `flatRuntimeMapBounceBeginUnmap`
     (`flat_runtime.cpp:5462`). Gating all three with `if (!runtimeVrProfile())`
     is a zero-behaviour-change cut in VR.
2. **64-slot `mapMemo` collisions in `hookedMap` call `GetType` + `GetDesc`:**
   - **Evidence:** at `src\d3d11\vscreen.cpp:3566-3595`, every `Map` of
     subresource 0 (other than `compositeCb`) probes a 64-slot direct-mapped
     table `s->mapMemo[(reinterpret_cast<uintptr_t>(res) >> 6) & 63]` to check
     `glitchFrameWantsBuffer(mm.byteWidth)` (which only wants the 5,376-byte
     scene camera buffer, `src\d3d11\glitch_frame.h:30-31, 74`). Whenever more
     than 64 buffers/textures are mapped per frame or addresses alias mod 64 in
     bits `6..11`, `mm.res != res` triggers `mapBufferDesc(res, &mm.byteWidth)`
     (`vscreen.cpp:3586`), which makes two COM calls (`res->GetType(&dim)` and
     `GetDesc(&d)`). Checking whether `mapMemo` misses in VR (or widening
     `mapMemo` from 64 to 256/512 slots) is an easy desk check alongside the
     `!runtimeVrProfile()` gates above.

## Build 3: one marker copy for both eyes (designed 2026-11-03)

Both reviews agree on the scope ("Reply to the rebuttal", "Argon's reply").
Environment: VR profile, the native OpenXR runtime, PSVR2, DLAA, Steam, build-2
baseline L2 (`edvr_gfx_20261103_112412.log`, the profiled flight at Shamash 1
a's concourse).

### Changes

Revised 2026-11-03 after the design review ("Review of the build 3 design"):
item 3's new counter is replaced by the existing draw census, and items 1 and 2
take the review's corrections.

1. **The merged copy (#157, A1).** In `streamUnmapCopy`
   (`native_on_foot.cpp:29568`):
   - Plan each eye exactly as today: `hist[e].expire`, then `planWindows`
     into `plan[e]`. Nothing about which entries are wanted changes.
   - New in `native_on_foot_f87.h`, so the rig runs the shipped code: a
     `MergedPlan` (up to `2 * kWinMax` = 14 ranges of `lo`, `n`, `off`, and
     `2 * kWinCapEntries` = 65,536 entries, Argon's capacity guard), a pure
     `mergePlans(plan[0], plan[1], &merged)`, and the merged lookup
     (`where(entry)`, the entry's offset in the buffer).
   - `mergePlans` skips zero-length windows (`planWindows` can add one,
     `native_on_foot_f87.h:382`), sorts the rest by `lo`, and joins windows
     that overlap or touch. `off` runs end to end. The ranges stay even, as
     the windows are; that matters only for speed (`streamLoad16` falls back
     to a plain copy when unaligned).
   - `merged` lives in `StreamShadow`, beside `plan[2]`, so a new stream
     resets it with the rest (`g_stream = StreamShadow{}`,
     `native_on_foot.cpp:29631`).
   - Copy each merged range once into one flat buffer,
     `g_streamWin[2 * kWinCapEntries * 2]`, the same 512 KB as today's two.
   - `streamEntryX` looks the entry up in `merged` instead of `plan[0]` and
     `plan[1]`. The merged lookup skips zero-length ranges. A hit returns the
     same value as today: the same bytes of the same Unmap. A miss goes on
     to `outside`, `nearest` (still `plan[eye].distance`, per eye), the
     escalation and the whole copy, unchanged.
   - **A fault** in a merged range's copy zeroes that range's `n` and the
     `n` of every `plan[e]` window inside it, so `nearest` never measures to
     a window with no copy. The step is a header function,
     `MergedPlan::fault(range, &plan[0], &plan[1])`, so the rig tests the
     shipped code. Each window lies inside exactly one merged range. This is the one place the merge differs from
     today. Today a fault loses only the window it hit; a merged fault
     loses the whole range, which may hold windows whose own bytes would
     have copied. Faults are counted on the new line (item 2), so a flight
     shows whether this ever happens. The default log does not record them
     today (only the detail log's F63 line, `native_on_foot.cpp:34863`).
   - The escalation's whole-stream copy is unchanged.
   - No key: coverage is the same by construction outside a fault, and
     nothing reaches the screen differently.
2. **The F87c figures.** The two eyes' copy can no longer be split by eye.
   - The per-eye line's `Unmap copy %llu copies, %llu bytes, %.3f ms; eye-1
     first/last ring %s` becomes, exactly, `Unmap copy %llu copies,
     requested %llu bytes; eye-%d first/last ring %s`: each eye's line
     carries its own eye's ring (eye 0's needs its own `snprintf`; the
     fields exist). Requested is that eye's planned window entries times 8.
     Eye 0's figure no longer carries the whole-stream bytes; those move to
     the new line.
   - A new line per window, exactly: `marker fix (F87c) Unmap copy: %llu
     Unmaps, requested %llu bytes, copied %llu bytes in %llu ranges (%llu
     windows joined), whole stream %llu bytes, faults %llu, %.3f ms; over %u
     s`.
     - Unmaps: every `streamUnmapCopy` call that reached the copy (a new
       counter; `g_win.streamCopies` counts only calls where something
       copied).
     - Requested: both eyes' planned bytes. Copied: the merged bytes.
       Ranges: the merged ranges, summed over the Unmaps. Windows joined:
       planned windows that landed in a range with another, summed.
     - These new sums live in the window's `Window` (`g_win`), not in
       `StreamSecond`, which each eye's line clears
       (`native_on_foot.cpp:34887`); `g_win` resets once a window, after
       every line (`35161`).
     - Faults: `g_win.streamFaults`, which also counts a whole-stream
       fault; a merged fault counts once per range.
     - The ms is `g_win.streamCopyTicks`: the whole function, planning and
       merge included, so the cut it shows is a lower bound on the copy's
       own saving.
   - `edvr_log.py`:
     - Keep `PERF_COPY_RE` (line 17249) for build-2 logs such as L2, and add
       a regex for the new line. `--perf-windows` prints the merged copy
       when the new line is there, the old per-eye figures when not.
     - The marker health rule (lines 16361-16366) gains a pattern for the
       new line, so `--health` does not report it as unmatched.
     - The source pin at line 24111 is a literal match: it takes the new
       per-eye text and the new line's text.
     - The `mark_win` fixture (line 18775) passes the copy arguments to the
       per-eye format, so it changes with the text.
     - Self-tests for each of these.
3. **B1, what the non-eye draws are: the existing draw census, plus a tally of
   its non-eye lines.** The review's finding 9 ("reuse the owner"): a census
   line records each draw's RTV0 (`r=`) and DSV0 (`d=`) as `texWxHfFMT` or an
   interned `@id` whose size the census's table gives, and its vertex shader
   hash (`vh=`) (`draw_census.cpp:367-387`, `817-818`, `489`). With
   `census_offscreen` on, the non-eye draws are recorded too (`DCO`).
   - **Which draws:** Draw, DrawIndexed, DrawInstanced and
     DrawIndexedInstanced reach `DCO` (`vscreen.cpp:2289`). The two
     indirect draws are written as `DC` lines with `t=i`, and deferred-
     context draws as `DC` with `t=f`, whatever their target. DrawAuto is
     not recorded in VR, nor are the sun, desktop-window and coat-pack
     skips. The tally reports the `t=i` and `t=f` lines apart.
   - **The tally (new code in `edvr_log.py`):** `--tally vh` today reduces
     `DCO` lines to one count (lines 557-562), never resolves `@N` tokens
     and does not parse `d=` (337-339). Add `--tally offscreen`: group the
     `DCO` lines (and the `t=i`/`t=f` `DC` lines) by `r=` and `d=`, resolve
     `@N` through the census's `DC id @N tex WxH` table, and list each
     group's count, draw kinds and top vertex shaders, per census frame.
     With a self-test.
   - **Flight-only settings,** in the live `edvr.ini` (edited with the Edit
     tool and diffed, reverted after the flight): `dump_draws` set to a free
     key, `census_offscreen = 1`, `census_frames = 1`, `census_lines =
     16384`. One frame keeps the burst to about 3 MB under the 16 MB log cap
     (`log.max_mb` is unset in the live ini; `log.cpp:156`), since the size
     table is written last and a capped log would lose every `@N` size. The
     line cap applies to non-eye lines apart from eye lines, but `DCO`
     shares it with clears, queries, copies, dispatches and indirect draws.
     L1's 4,070 non-eye DrawIndexedInstanced draws a frame is a lower
     bound; L2 at the flight's spot counted 514.
   - One press, after the profile capture has stopped, so the census's
     logging is not in the profile.
   - **Truncation check:** `DCO` truncation is not in `truncated=`. Compare
     the `DC frame` line's `off=` with the number of `DCO` lines; the tally
     prints both.
   - This drops the seven-hook counter and with it the review's findings 5
     (placement after the world route's `g_vrWorldInternal` return), 6
     (line length), 7 (its cost), 8 (top-N and threads) and 10 (the
     cross-check).

Left out, in order for after build 3 (Argon's A3 findings):
- The three ungated map-bounce calls in `hookedMap` and `hookedUnmap`.
  **Price:** in the build-2 profile (20 R1 windows, 2,773 frames, inclusive per
  frame) `flatRuntimeMapBounceBeginUnmap` 0.045 ms, `Install` 0.010, `PreMap`
  0.008, `mapBounce` 0.014: about 0.06 ms. Zero behaviour change if the bounce
  never leaves `Pending` in VR, but too small to see in a flight.
- `mapMemo` misses: `mapBufferDesc` 0.041 ms inclusive in the same windows. The
  two COM calls under it are outside EDVR's frames and not in that figure.
- `hookedMap` (0.58 self) and `hookedUnmap` (0.50 self) themselves stay the A3
  pass's real target.

### Test seams

In `tools\native_frame_test\f85_checks.h`, beside the F87c checks, all on the
header's own `mergePlans` and merged lookup:
- Random histories and centres (a fixed seed), both eyes: every entry `plan[0]`
  or `plan[1]` holds is in `merged`, and nothing else; ranges are sorted,
  disjoint and not touching; `lo` and `n` even; `off` end to end; within 14
  ranges and 65,536 entries.
- Two disjoint full plans (7 windows each, 32,768 entries each) merge to 14
  ranges and 65,536 entries with nothing dropped: the capacity guard.
- Identical plans: copied equals the plan's own union (which is less than its
  requested entries when its windows overlap each other), and requested is
  twice the plan's requested.
- A plan with a zero-length window: the window is skipped and the rest merge as
  without it.
- A synthetic stream (entry i holds x = i): the merged lookup returns the same
  x as today's two-buffer lookup for every entry of the stream, hit or miss.
- A fault in one merged range (`MergedPlan::fault`): that range and every
  `plan[e]` window inside it read as empty, the lookup skips the range, and
  every other range still returns its entries.
- `edvr_log.py --self-test` for `--tally offscreen`: a fixture census with
  `DCO` lines, `@N` tokens, the `DC id` table and `t=i`/`t=f` lines groups by
  resolved target, and reports `off=` against the `DCO` line count.
- `edvr_log.py --self-test`: the new per-eye text and the new line parse, the
  health rule matches the new line, `--perf-windows` prints the merged copy for
  a new log and the per-eye copy for an L2-style log, and the source pin holds.

### What the log shows if the new code never ran

- The merge: no `marker fix (F87c) Unmap copy:` line (only the new code writes
  it), or `windows joined 0` across the window while the per-eye lines show
  both eyes' windows. Joined 0 with copied equal to requested is possible only
  when every window is disjoint; the eye-0 and eye-1 rings say whether the
  eyes' entries are far enough apart for that.
- The census: no `DC begin` line (the key was not pressed or not set), or `DC
  begin ... offscreen=no` (`census_offscreen` did not load;
  `draw_census.cpp:1730`), or `off=0` on the `DC frame` line
  (`draw_census.cpp:1175`).

### Flight

The L2 spot (Shamash 1 a, the concourse), standing still, DLAA, the build made
with `EDVR_PROFILE_SYMBOLS=1`.
1. A 100 s profile capture.
2. After it stops: one census press.

First command after: `edvr_log.py --target steam --expect-build HEAD
--version`, then `--health`.

### Pass criteria

- **Coverage held:** steady-state F87c lines show `outside 0` and `escalated 0`
  after arrival, as in L2 from 11:26:34 to 11:28:15, `faults 0` on the new
  line, and `named` per Unmap between 2.6 and 3.0 an eye, as in L2's steady
  windows (831 named in 277 copies at 11:26:34 is 3.0; 819 in 313 at 11:28:15,
  the last window, is 2.62). Named per second rises with the frame rate, so it
  is not the measure.
- **The cut:** `copied / requested` from the new line. `streamUnmapCopy`'s
  inclusive time in R5 should fall by that ratio from 0.697 ms (the build-2
  profile, windows 1-20, R5, 2,773 frames; the table's rounded 0.70), at the
  1.45-1.57 ns a byte seen in both logs. The 0.697 is from analysis notes
  outside the repo (`%TEMP%\edvr-build1-notes\profile-b2`, the
  `b2-r5-edvr-w*.json` exports summed per frame); the doc's 0.70 is its
  rounding.
- **Nothing else rose:** no EDVR function in the profile up more than 0.1 ms
  against the build-2 profile's windows.
- **B1 answered:** the census's `DCO` lines name the non-eye draws' targets and
  shaders. The next step on B1 (a ledger entry, or dropping it) is decided from
  them.

### Review of the build 3 design (2026-11-03)

A fresh-context review on `claude-opus-5-5` (its transcript records that model
only), against the code at `0e3cc1f2`. Verdict: changes required; two blocking
findings, both on the non-eye counter. Dispositions:

| # | Finding | Disposition |
|---|---|---|
| 1 | A merged fault loses the whole range; `nearest` could read faulted windows | Taken: zero the covered `plan[e]` windows too, count faults (item 1) |
| 2 | Readers complete; keep `merged` in `StreamShadow`; skip zero-length windows | Taken (item 1, a test) |
| 3 | Capacity bound right; evenness only for speed | Wording fixed (item 1) |
| 4 | "Identical plans: half" is wrong when a plan overlaps itself | Test fixed |
| 5 | BLOCKING: a note at the hook top counts EDVR's own world-route draws | Moot: the counter is dropped for the census |
| 6 | BLOCKING: the line exceeds `Log::note`'s 1200 bytes | Moot, as 5 |
| 7 | Counter cost unproven | Moot, as 5 |
| 8 | Top-N method and threads undefined | Moot, as 5 |
| 9 | The census already records this | Taken: B1 by census (item 3) |
| 10 | The cross-check wording; the hook range | Moot, as 5 |
| 11 | `edvr_log.py`: health pattern, keep the old regex, exact text, eye 0's whole-stream bytes | Taken (item 2) |
| 12 | Put the merged lookup in the header so the rig tests shipped code | Taken (item 1, test seams) |
| 13 | Copied = requested does not prove the merge never ran | Taken: `windows joined`, the new line's presence (never-ran section) |
| 14 | 0.697 uncited; use named per copy | Taken (pass criteria) |
| 15 | No key, right; say VR-only | The census is a developer key already; the merge applies wherever the marker fix does |

**Re-review (2026-11-03, Opus, fresh context, at `2eac9e74`): approve,** eight
non-blocking findings, all taken: `--tally offscreen` added (the existing tally
cannot read `DCO` lines); the census's never-ran check uses `DC begin ...
offscreen=` and `off=`; which draw kinds reach `DCO`; `census_frames = 1` for
the log cap; the new line's sums in `Window`, its `Unmaps` counter, and the ms
as a lower bound on the saving; named per Unmap 2.6-3.0; `MergedPlan::fault` in
the header; the `mark_win` fixture and each eye's own ring.

### Reviews in this arc (checked 2026-11-03)

The "Fable" reviews named in this arc did not run on Fable. Every subagent
transcript of this project's sessions records its model, and none records
`claude-fable`; the reviews dispatched as Fable ran on `claude-opus-5-5`. Fable
needs usage credits this account does not have. So "Review of the mask design
(2026-11-02)" is an Opus review, independent only in that it ran with a fresh
context.

## Flight 2026-11-03 (build 3)

Build `cfa5e806` (`EDVR_PROFILE_SYMBOLS=1`), the L2 spot (Shamash 1 a, the
concourse), standing still, DLAA. Log `edvr_gfx_20261103_184210.log`;
`edvr_log.py --expect-build HEAD` matches. A 100 s `cpu_profile.py` capture
from 18:44:35 (pid 26996, 2,926 frames in windows 1-20, 0 events lost, symbols
matched 2 of 2), then one census press (census frame 19271). The flight-only
ini settings are reverted; the live `edvr.ini` matches its pre-flight backup.

**Coverage held (pass).** From 18:44:49 to the end, every F87c window shows
`outside 0`, `escalated 0` for both eyes, and `faults 0` on the new line. Named
per copy is 3.0 an eye (for example 927 named in 309 copies at 18:45:39). The
two windows before 18:44:49 (arrival) escalated 66 and 72 times and missed 30
and 48 entries on eye 1, as L2's arrival did; `--health` sums them as `outside
78`.

**The cut (pass).** The new line, steady state (18:44:49 to 18:46:40, 12
windows): copied is 68-69% of requested (for example 97,945,584 of 141,754,368
bytes at 18:45:39), so 31% of the bytes are gone. In each 10 s log window about
3,400 plan windows join into about 620 ranges, two an Unmap. The function's own
time on the line is 0.48-0.54 ms an Unmap (147.652 ms over 309 Unmaps at
18:45:39), 1.5 ns a copied byte, the same rate as before.

The profile, windows 1-20 against build 2's windows 1-20 (R5, inclusive, ms a
frame; the same method and scripts as build 2, notes outside the repo in
`profile-b3\`):

| | build 2 | build 3 |
|---|---|---|
| `streamUnmapCopy` | 0.697 | 0.506 |
| `streamCopyGuarded` (the copy itself) | 0.696 | 0.504 |
| `hookedUnmap` | 0.734 | 0.543 |

- A 27% fall, against 31% predicted from the bytes (0.697 x 0.69 = 0.48).
  Windows 3-20 alone, after arrival, give 0.507, so the arrival's escalations
  do not explain the gap. The planning and merge cost about 0.003 ms
  (`streamUnmapCopy` less `streamCopyGuarded`); the rest is the copy running at
  about 1.59 ns a byte here against 1.45-1.57 before.
- **Nothing else rose by 0.1 ms (pass).** The largest rises: R5 `hookedPresent`
  +0.058, `hookedCreateTexture2D` +0.074, `presentFrameBoundary` +0.044. In R1
  no named function rose by 0.05 ms. The draw-hook lambdas were renamed by the
  build (new hashes), and each fell when paired by file:line: the draw lambda
  (`vscreen.cpp:5514`) 0.608 to 0.526, `forwardWithVerdict` (`:4688`) 0.449 to
  0.413, `guarded` 0.145 to 0.136.
- R1 fell from 26.15 to 24.08 ms and EDVR's leaf from 4.97 to 4.72, but the
  game exe also fell 1.24 ms, so the scene differs and the totals are not this
  build's saving. Build 3's saving is the copy's 0.19 ms, post-submit.

**B1, what the non-eye draws are.** `edvr_log.py --tally offscreen`: one census
frame, 676 `DCO` lines, `off=` 676, none cut. The eye is 2632x2684. Formats are
DXGI numbers.

| draws | RTV0 | DSV0 | top VS |
|---|---|---|---|
| 607 | none | 512x512, 53 (R16 typeless) | `C2208C162D010083` (585) |
| 20 | 1844x1639, 27 (RGBA8 typeless) | 1844x1639, 44 (R24G8) | `1E8D013722FE71B8`, `A3E5D3FCBC1165F8` |
| 12 | 2057x1235, 27 | 2057x1235, 44 | the same two |
| 7 | 2360x2032, 27 | 2360x2032, 44 | the same two |
| 28 | a chain at 1316x1342, 658x671, 329x335, 164x167, 82x83 (half, quarter ... of the eye), mostly 26 (R11G11B10 float) | none, or 658x671 39 | `129F602B2A9CA439`, `DFED8E1C9E191BEC`, `DEF19B035D5EDEDC` |
| 1 | 1024x1024, 33 | 1024x1024, 53 | `AB6D504EE6B5398D` |

- 90% of the non-eye draws are depth-only into one 512x512 16-bit target,
  nearly all with one vertex shader. That is the shape of a shadow or depth-map
  pass; which light or view it serves is not yet known.
- 39 draw colour and depth into three larger targets that match no eye size.
- The chain is a downsample of an eye-sized image, one or two draws a level,
  which is post-processing, not geometry.
- Next on B1: name the 512x512 pass (its 607 draws are the only bulk), then
  decide whether it is a candidate for a ledger entry or dropped.

## EDVR's own time versus the calls it forwards (2026-11-03)

The "innermost" figure this arc has ranked targets by (the per-function "self"
in build 2's and build 3's tables, and the A3 figures) is not EDVR's own time.
A sample's innermost EDVR frame is the sampled instruction when the CPU was in
EDVR code, but when the CPU was below EDVR (the D3D runtime, the driver, EDHM,
the kernel) it is the return address of EDVR's call into that code. So a hook
that forwards to the real D3D call is charged with that call. Build 2's table
said so in a footnote ("only the leaf row is pure EDVR"); the targets were
still ranked by innermost.

**Evidence.** Build 3's capture, windows 3-20, R1, 2,649 frames.
- `hookedMap`'s hottest address, 0x526409 (1,172 of its 1,526 samples), and
  `hookedUnmap`'s, 0x527f41 (918 of 1,141), are each the instruction right
  after `call rax`, the call to `s->realMap` (`vscreen.cpp:3492`) and
  `s->realUnmap` (`:3761`) (`dumpbin /disasm` of the flown `d3d11.dll`).
- A script splits every sampled address the same way: an address is a return
  address when an x64 call instruction, confirmed by disassembly, ends exactly
  at it. It gives 8.794 ms innermost = 4.719 own + 4.075 below EDVR. The own
  total matches the analyzer's independent "EDVR leaf" for the same windows
  (4.699 ms), which counts samples whose top frame is in EDVR's module.
  Addresses with fewer than 3 samples (1.05 ms) are counted as own, so own is
  an upper bound. Scripts and output, outside the repo:
  `profile-b3\leaf_rank.py`, `leaf_rank.txt`.

Own time, ms a frame (own | below EDVR):

| | own | below |
|---|---|---|
| `hookedDrawIndexedInstanced` and all it calls | 3.29 | 1.11 |
| of which `beginPanelOverride` | 0.56 | |
| `forwardWithVerdict` (the draw lambda's) | 0.39 | |
| `hookedDrawIndexedInstanced` itself | 0.31 | |
| the draw lambda (`vscreen.cpp:5514`) | 0.16 | 0.37 |
| `uiDepthOnEyeDraw`, `viewIsSurface`, `engineVelocityBeforeDraw`, `depthProbeNoteDraw` | 0.13, 0.11, 0.11, 0.11 | |
| `hookedUnmap` and all it calls | 0.39 | 0.40 |
| `hookedMap` and all it calls | 0.26 | 0.53 |
| `hookedPSSetShaderResources` and all it calls | 0.05 | 0.79 |
| `hookedVSSetConstantBuffers` | 0.09 | 0.25 |
| `hookedPSSetConstantBuffers` | 0.11 | 0.00 |
| `flushIaObserved` | 0.02 | 0.37 |
| `hookedEnd` | 0.00 | 0.13 |

- **What this changes.** A3 was priced at `hookedMap` 0.58 + `hookedUnmap` 0.50
  ms "self"; EDVR's own code under both hooks is 0.65 ms in all, of which the
  hooks' bodies are 0.17. Under `hookedUnmap`: the engine-velocity hash map
  0.046, `flatRuntimeMapBounceBeginUnmap` 0.034, the marker probe 0.031,
  `AnimatedVertexHistory::resourceWritten` 0.029. Under `hookedMap`: the marker
  probe 0.052, `mapBufferDesc` (the `mapMemo` misses) 0.028, `mapBounce` 0.012.
- **Ruled out:** the bind hooks as a large target. `hookedPSSetShaderResources`
  (ranked first at 0.82-0.90 ms) is 0.05 ms of EDVR code; `flushIaObserved` is
  0.02. The rest is the game's own D3D calls, which EDVR forwards and cannot
  remove.
- **The largest EDVR cost is the draw path:** 3.29 of the 4.72 ms own, 1.42 of
  it in the ladder (`beginPanelOverride`, `forwardWithVerdict`, the hook, the
  lambda) and the rest in per-draw subscribers.
- Builds 2 and 3 are unaffected: the byte's targets and the marker copy are
  EDVR's own code (the copy is `streamCopyGuarded`'s reads of write-combined
  memory, all own).

## Build 4: the draw path's own cost, and the map/unmap cuts (designed 2026-11-03)

Scope decided 2026-11-03: A3 together with the draw path, one build. The
candidates come from a code read against build 3's per-line own time (Opus,
fresh context; notes outside the repo, `b4-candidates.md`), checked here where
marked. Environment: VR profile, the native OpenXR runtime, PSVR2 at 2632x2684,
DLAA, EDHM chained, game build 334925, about 13,600 full-path draws a frame in
the concourse. No change reads a headset size or a runtime.

**Where the time goes (inferred, not measured with cache counters).** The
ladder's own samples land on the instruction after a load of a module flag
(`g_drawCensusPending`, `g_holoSteady`, `g_footprintWanted`,
`g_vrStarGlowWanted`), a `State` field 50 KB in (`censusSkipCount` at +0xC930,
`introCurveThisDraw` at +0xCB0D), or a thread-local slot, not on arithmetic.
Those lines sum to about 0.63 ms a frame, about 2 ns each a draw: the shape of
one cache miss per flag, because the game's draw between two hooks evicts them
and each lives on its own line. So the lever is the number of distinct cache
lines the ladder touches and the out-of-line calls it makes, not the tests
themselves.

### Changes (all keyless: each is the same test or the same data, moved)

1. **Inline each callee's first test at the call site.** Out-of-line calls made
   for every draw that return on their first line. The codebase does this
   already ("the callee's own first test, inline", `vscreen.cpp:2185-2188`).

   | call | site (`vscreen.cpp`) | first test | own ms |
   |---|---|---|---|
   | `pixelProbeBefore`, `pixelProbeAfterEye` | 5498, 5604 and the other thunks | `pixelProbeWantsDraws()` (`:1900`, `:1915`, checked) | 0.036 + 0.052 |
   | `temporalPassWantsRigidDraw` | 5544-5552 | `g_temporalPassWantedFssChrome`, eye, seen (`temporal_pass.cpp:6086-6089`) | 0.036 |
   | `gpuFrameCommandIfLive` | 5460 | inline in `gpu_frame_timing.h:62`, not inlined | 0.028 |
   | `flatTemporalCapturing` | 2109, 3524, 3668 | one relaxed load (`flat_temporal.h:24-25`) | 0.024 |
   | `uiLayerTargetKind` | 4361 | generation compare (`ui_layer.cpp:3808-3810`) | 0.025 |
   | `objectProbeCandidateDrawIssued` | 4342-4345 | `g_objectProbeLedgerOn && ctx` (`object_probe.cpp:1497`) | 0.023 |
   | `optimizedStationSkipsDraw` | 2087 | `g_sunOff` (`optimized_station.cpp:2422`) | 0.021 |
   | `blendProbeWantsDraws` | 4265 | one relaxed load (`blend_probe.cpp:724-726`) | 0.019 |

   Bound 0.29 ms; expected 0.12-0.2, since the flag load stays. Each inlined
   test must be exactly the callee's first test; the callee keeps it.
2. **The per-draw `State` fields in one 64-byte-aligned block at the head of
   `State`:** `ownerCtx`, `rtv0Eye`, `rtv0EyeGen`, `rtv0Cand`, `glareClamp`,
   `curveThisDraw`, `introCurveThisDraw`, `eyeDrawsThisFrame`, `bindAuditSeq`,
   the `censusSkip*` counts, `censusAutoW`, `quadSkipArmed`, `distanceEnabled`,
   `fssBodyFrame`, `frameNo`, and item 3's flags. Their lines cost about 0.13
   ms; expected 0.05-0.1. Layout only. A `static_assert` pins the block's size.
3. **The three thread-local per-draw flags become owner-only `State` members.**
   `t_uiDepthThisDraw`, `t_holoDepthThisDraw`, `t_compositeThisDraw`
   (`vscreen.cpp:1598-1610`) are thread-local so a deferred-context draw on
   another thread cannot take the render thread's flag. Every write is already
   after the foreign-context return (`:2074-2084` before `:2114-2115`,
   `:2507-2513`), and only `UiDepthScope` (`:4279-4284`) reads and clears them.
   As members, read and cleared only when the draw is the owner's (computed at
   `:4261`), a foreign draw reads false, as it does today. Lines 2114, 2115,
   4293: bound 0.09, expected 0.05-0.08.
4. **The flat map bounce, gated on its own "anything open" test.** In VR
   nothing is ever installed (`flat_map_bounce.h:139`, `setMode` only at
   `flat_runtime.cpp:4341` after the flat-only return at `:4214`), so `open_`
   stays 0 and `preMap`/`beginUnmap` return at their first test
   (`flat_map_bounce.h:128`, `:164`, checked). Argon's proposed gate,
   `!runtimeVrProfile()`, is not exact: the profile switches live at a frame
   boundary (`mode_watch.cpp:451`), and a bounce still open across a flat-to-VR
   switch would never be flushed (one Unmap's write lost, the slot and its
   reference leaked); `hookedUnmap`'s comment requires the flush "independently
   of the current mode" (`vscreen.cpp:3636`). Instead: expose `open_ != 0`
   inline, skip `PreMap` and `BeginUnmap` when it is 0, move `flatcpu::Scope`
   in `flatRuntimeMapBounceBeginUnmap` (`flat_runtime.cpp:5462`) below that
   test, and gate `Install` (`vscreen.cpp:3512`) on `flatRuntimeActive()`, an
   exact term of its own refusal (`flat_runtime.cpp:5441`). Exact in every
   profile and across a switch. About 0.05 ms.
5. **`uiDepthOnEyeDraw`'s per-draw outputs in one struct**, reset as a block.
   0.112 of its 0.132 ms sits right after twelve resets to globals on separate
   lines (`ui_depth.cpp:2896-2908`). Same resets, same values; expected about
   0.05. Skipping the resets is not in this build.
6. **Shape before flag in the eye branch:** `holoWantsDraws() &&
   holoPatternShape` (`vscreen.cpp:2678`) and `backdropWantsDraws() &&
   backdropCompositeShape` (`:2708`) become shape first; both shapes are (X, 6,
   1) (`holo_fix.h:58-60`, `backdrop_fix.h:126-128`) and count and instances
   are in registers. Pure reads. Expected 0.03-0.05.
7. **Two counters, for build 5's decisions, on one new 10 s line.** They cost
   one compare and increment per call, outside the per-draw ladder:
   - `mapMemo` lookups and misses (`vscreen.cpp:3582-3587`). A miss pays
     `mapBufferDesc` (0.028 own plus two COM calls).
   - Same-pointer re-sets in `bindingSet`, for RTV0, DSV0 and PS SRV 0-3:
     calls, and calls whose pointer equals the one bound. Every set bumps
     the generation today (`binding_shadow.cpp:107-112`, checked), which is
     why every per-binding cache in the ladder misses (this arc's item 4).
8. **The analyzer reports own time.** `tools\cpu_profile`'s EDVR export gains,
   per innermost address, whether the sample's top frame was in EDVR (own) or
   below it. This replaces the disassembly script used above and makes own time
   the figure every later table ranks by.

Expected total: 0.35-0.55 ms a frame of EDVR's own time, nearly all in the draw
path.

### Left for build 5, each needing evidence first

- **S1, a per-frame snapshot of the feature flags** (extending
  `drawGateWanted`, `vscreen.cpp:2144-2164`), packed in item 2's block so the
  ladder reads one cache line. Each bit a "may want" superset. Bound about 0.5,
  expected 0.15-0.3; needs a one-flight A/B that counts stale-false snapshots.
- **S2, a content generation** that moves only when the bound pointer changes,
  for the per-binding caches; engine velocity keeps the set generation
  (`vscreen.cpp:5489-5493`). Bound about 0.29; decided by item 7's re-set
  counts.
- `viewIsSurface`'s per-slot front cache (0.117); `mapMemo` resizing (by item
  7's miss count); skipping `uiDepthOnEyeDraw`'s resets.
- Not candidates: the marker tee and probes (already ruled out), star glow's
  per-draw classify (it keeps per-target run state that any other draw ends,
  `vr_star_glow.cpp:362-374`), `depthProbeNoteDraw`'s counters.

### Test seams

- Item 1: each predicate is a header read; a rig check per inlined test that it
  equals the callee's first test over its inputs where the callee's state is
  reachable (`temporalPassWantsRigidDraw`'s state moves to `temporal_pass.h`).
- Item 3: a rig case where a foreign-context draw runs between an owner draw's
  set and its `UiDepthScope`: the foreign draw reads false and the owner's flag
  survives.
- Item 4: `flat_map_bounce_test` gains "`open()==0`: `preMap` and `beginUnmap`
  touch nothing" and a flat-to-VR switch with one bounce open: the next Unmap
  still flushes it.
- Item 7: `edvr_log.py` parses the new line (fixture, self-test, health
  pattern).
- Item 8: the analyzer's parser tests gain an own/below case.

### What the log shows if the new code never ran

- Item 7: no new counter line.
- Items 1-6: nothing in the log; the profile shows it. With item 8, the
  per-line own time of the changed lines (2087, 2109, 2114, 2115, 2591, 2678,
  2708, 4293, 4342-4345, 4361, 5460, 5498, 5544-5552, 5599, 5604,
  `ui_depth.cpp:2896-2908`) falls, and the out-of-line callees of item 1 drop
  out of the own list.

### Flight

The L2 spot, standing still, DLAA, `EDVR_PROFILE_SYMBOLS=1`, one 100 s capture.
No census.

### Pass criteria

- **The cut:** the draw subtree's own time per full-path draw (own ms over the
  `draw mask:` line's full-path draws a frame) falls by at least 0.2 ms a
  frame's worth at build 3's draw count, against build 3's 3.29 ms at about
  13,600 draws. Per draw, because the scene and draw count move between
  flights.
- **Nothing changed on screen:** every feature these items touch logs the same
  as in build 3 within flight-to-flight spread: the `ui depth` 20 s line and
  `ui scene composites` (item 3), the temporal pass's rigid-draw count, the
  footprint line, the pixel and blend probes (off: zero both times), `--health`
  with no new NEVER APPLIED.
- **Nothing else rose:** no EDVR function's own time up more than 0.1 ms.
- **Build 5 is decided:** item 7's miss and re-set counts are in the log.

### Review of the build 4 plan (2026-11-03, Antigravity)

Checked against `src\d3d11\` and build 3's own-time split (`cfa5e806`).

#### 1. Agreement on scope and items 1-8

- **Pairing A3 with the draw path:** agreed. `hookedMap` (0.26 ms own) +
  `hookedUnmap` (0.39 ms own) is 0.65 ms total, most of which is active VR work
  (`engineVelocity`, `nativeOnFootMarkerProbe`, `AnimatedVertexHistory`). A3
  alone would sit well below the ~0.8 ms flight noise floor; pairing it with
  the draw path (3.29 ms own) is the right scope.
- **Item 4 (`open_ != 0` instead of `!runtimeVrProfile()`):** good catch.
  Checked `vscreen.cpp:3636` ("Flush independently of the current mode, owner
  thread, and projection lifetime") and `mode_watch.cpp:451`: gating `PreMap`
  and `BeginUnmap` on `open_.load(std::memory_order_acquire) != 0` (the exact
  first test of `FlatMapBounce::preMap` at `flat_map_bounce.h:128` and
  `beginUnmap` at `:164`) and `Install` on `flatRuntimeActive()`
  (`flat_runtime.cpp:5441`) preserves live flat-to-VR switch drain while saving
  the cross-TU call and `flatcpu::Scope` (`flat_runtime.cpp:5462`) on every VR
  `Map`/`Unmap`.
- **Items 2, 3, 5, 7, 8:** agreed as designed. Moving the hot per-draw `State`
  fields and the three owner-only per-draw flags (`t_uiDepthThisDraw`,
  `t_holoDepthThisDraw`, `t_compositeThisDraw`) into one cache-line-aligned
  header block, resetting `uiDepthOnEyeDraw`'s twelve outputs
  (`ui_depth.cpp:2896-2907`) as a single struct, logging `mapMemo` misses +
  `bindingSet` same-pointer re-sets, and teaching `tools\cpu_profile` to split
  own vs. below-EDVR time are all clean, zero-behaviour-change cuts.

#### 2. Four code checks and small additions for Build 4

1. **Item 1 implementation trap — why `gpuFrameCommandIfLive` (0.028 ms) and
   `flatTemporalCapturing` (0.024 ms) did not inline:**
   - Both are *already* marked `inline` in their headers
     (`gpu_frame_timing.h:62-65`, `flat_temporal.h:24-32`). Why did MSVC emit
     out-of-line calls at `vscreen.cpp:5460` and `:2109, 3524, 3668`?
   - In `gpu_frame_timing.h:44-46`, `gpuFrameCommandMightAct()` reads
     `detail::g_gpuFrameInternal`, which is `extern thread_local bool`
     (`gpu_frame_timing.h:34`), **and** does it *after* testing
     `!detail::g_gpuFrameInternal` first! Accessing an `extern thread_local`
     across TUs without `/GL` emits a `_tls_index` lookup + function call stub,
     and causes MSVC's inliner heuristics to refuse inlining
     `gpuFrameCommandIfLive`. Flipping the order in
     `gpuFrameCommandMightAct()` (`gpu_frame_timing.h:44-46`) so the atomic
     bool `detail::g_gpuFrameCommandLive.load(std::memory_order_relaxed)` is
     tested **before** `detail::g_gpuFrameInternal` (or marking
     `__forceinline`) avoids touching TLS at all when `app_gpu_timing` is off!
   - In `flat_temporal.h:24-32`, `flatTemporalCapturing()` has a 4-branch body
     ending in `flatObserveAsOwner(..., GetCurrentThreadId())`, which pushes
     MSVC past its inline threshold at multiple call sites. Splitting out the
     cold tail (`flatTemporalCapturingSlow()`) or `__forceinline` on the first
     test (`detail::g_flatTemporalCapturing.load(std::memory_order_relaxed)`)
     guarantees the single-load fast reject is actually inlined at `:2109`,
     `:3524`, and `:3668`.

2. **Item 6 — two more shape-before-flag / guard-before-hash sites in the same
   pass (`vscreen.cpp:1994, 2685, 2695`):**
   - At `vscreen.cpp:1994-2000`, every `kind == 'X' && count == 6` draw enters
     `guardedBudget(g_panelCbBudget, [&] { ... })` *before* checking whether
     `bindingShaderHash(BindSlot::Vs)` is one of the two FSS chrome VS hashes
     (`0xA8885C4DCAD7D1C6ULL`, `0xB01815A7DBDE04DCULL`). Since
     `bindingShaderHash(BindSlot::Vs)` is a pure array read of the shadow
     (`binding_shadow.h:236`, no D3D call), testing the two VS hashes *before*
     `guardedBudget` skips setting up the guard on every 6-index quad in the
     frame.
   - At `vscreen.cpp:2695`, `scrimWantsDraws() && scrimWashShape(kind, count,
     instances)` has the same flag-before-shape order as `holoWantsDraws`
     (`:2678`) and `backdropWantsDraws` (`:2708`): `scrimWashShape`
     (`scrim_fix.h:76-78`) is also `(kind == 'X' && count == 6 && instances ==
     1)` in registers. Put `scrimWashShape` first too.
   - At `vscreen.cpp:2685`, `targetSharpWantsDraws() && targetSharpOnEyeDraw`
     lacks an inline shape helper in `target_sharp.h` (unlike `holo_fix.h`,
     `scrim_fix.h`, `backdrop_fix.h`), even though `targetSharpOnEyeDraw`'s
     header comment (`target_sharp.h:70-71`) explicitly notes *"Shape first (6
     indices, one instance)"*. Adding `inline bool targetSharpShape(char kind,
     uint32_t count, uint32_t instances)` and checking it before
     `targetSharpWantsDraws()` keeps all four `(X, 6, 1)` eye-branch checks
     uniform and register-first.

3. **Why `uiDepthHologramOnEyeDraw` and `viewIsSurface` (0.11 ms own) run on
   scene draws, and two keyless guards:**
   - Look at `classifyHologramDraw` (`ui_depth.cpp:3592-3625`): it **never
     reads `Ps` or `PsSrv0..3`**, and its plain decline (`!h ||
     uiDepthIsExcluded(h)` or not in `kHoloWorldMarkers` / `g_holoFamilies`) is
     a pure function of the vertex shader hash `vs`. Yet `g_holoMemo` keys on
     `bindingEyeKeyGeneration()`, which invalidates on every `Ps` and
     `PsSrv0..3` bind! Adding a `kVsMayHoloDepth` bit to `vs_may_want.h` (set
     when `vs` is in `kHoloWorldMarkers` or `g_holoFamilies`) lets
     `vscreen.cpp:2513` skip `uiDepthHologramOnEyeDraw` inline before the
     cross-TU call.
   - In `classifyEyeDraw` (`ui_depth.cpp:2657-2665`), every `g_eyeMemo` miss
     loops 4 times over `viewIsSurface(bindingGet(kSlots[i]))`, probing
     `g_viewMemo` (`PtrMemo<1024>::get`, `ui_depth.cpp:866-875`, 0.11 ms own)
     4 times per draw—even when **`g_surfaceCount == 0`**
     (`ui_depth.cpp:330`, before any UI surface has been learned, or after a
     reset)! Gating that loop (or `viewIsSurface` itself when no chrome override
     is active) on `g_surfaceCount != 0` is a 1-line keyless guard while leaving
     the full per-slot front cache for Build 5 once item 7's `bindingSet`
     re-set counter is in hand.

4. **B1 note for when we probe the 512x512 depth-only pass (607 draws, VS
   `C2208C162D010083`):**
   - When we get to naming the 512x512 depth-only pass, note that neither
     `advanced.census_skip_offscreen = 512x512` nor `advanced.census_skip =
     vs:C2208C162D010083` can skip it today:
     - `census_skip_offscreen` (`vscreen.cpp:2397-2411`) resolves
       `BindSlot::Rtv0`, which is `nullptr` on all 607 depth-only draws.
     - `census_skip` (`vscreen.cpp:2591-2604`) sits inside `if (s->rtv0Eye)`
       (`:2488`), so it never sees non-eye draws.
   - Having `census_skip_offscreen` (`vscreen.cpp:2399`) fall back to
     `bindingGet(BindSlot::Dsv0)` when `bindingGet(BindSlot::Rtv0) == nullptr`
     is a 2-line fix to the instrument that makes `advanced.census_skip_offscreen
     = 512x512` work on depth-only passes whenever we want to visually name the
     512x512 target.
