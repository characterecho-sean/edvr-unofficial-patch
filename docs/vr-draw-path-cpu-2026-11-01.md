# VR draw-path CPU cost (2026-11-01)

## Status

- **Question:** how much of a CPU-bound VR frame is EDVR's draw path (GLOSSARY:
  Draw path), and how much can go without changing what reaches the screen?
  Earlier investigation: `docs\stereo-cpu-sharing-2026-10-16.md`.
- **Build 1 (flown 2026-11-02, `53c965cd`, draft PR #166):** failed the total
  criterion. Shipped items 1 and 3 and the re-issue counter; removed the rest
  and all three temporary keys (0.2-0.4 ms against 0.8 ms of noise).
- **Build 2 (#168, flown 2026-11-03):** the VS "may-want" byte and pair test
  before `GetType`. Its targets fell about 0.6 ms a frame gross, with rises of
  under 0.2. Ruled out after it: the tee trap (0.08 ms), the EDHM bypass.
- **Build 3 (#180, `cfa5e806`; "Flight 2026-11-03 (build 3)"):** both eyes'
  marker windows copied once per Unmap. Passed: coverage held, R5
  `streamUnmapCopy` 0.697 to 0.506 ms. B1: 607 of 676 non-eye draws are
  depth-only into one 512x512 target (one VS); not yet identified.
- **Own time (2026-11-03, "EDVR's own time versus the calls it forwards"):**
  the arc's per-function "self" included the D3D calls each hook forwards.
  EDVR's own R1 time is 4.72 ms: the draw path 3.29, `Unmap` 0.39, `Map` 0.26,
  the bind hooks about 0.3. Ruled out: the bind hooks as a large target. A3
  (map/unmap) is worth at most 0.65 ms.
- **Build 4 (#185, `c86670bd`; design and reviews in "Build 4: the draw path's
  own cost"):** inlined first tests, a hot `State` block, the map bounce gated,
  the hologram pass by VS bit, own time in the analyzer. Item 3 dropped (#184).
- **Build 4 flown (2026-11-03, "Flight 2026-11-03 (build 4)"):** passed. Own
  time per full-path draw 349 to 304 ns; 0.45 ms a frame at build 3's draw
  count without Map/Unmap (0.60 with). Heavier scene (17,148 draws a frame
  against 13,480). Ruled out: item 4's map-bounce calls as a cost (under 0.02
  ms). Item 7: PS SRV re-sets 53-63% of sets, RTV0/DSV0 6-7%; `mapMemo` misses
  17%.
- **Build 5 (#187, `85006708`; "Build 5: the per-draw calls that decline at
  once"):** star glow gated inline, `uiDepthOnEyeDraw` inline, a
  `viewIsSurface` front cache, one engine-velocity generation, a plain probe
  count; temporary counts for #184 and S2. Argon and Opus (re-review) agreed.
- **Build 5 flown (2026-11-04, "Flight 2026-11-04 (build 5)"):** passed, 0.34
  ms a frame (304 to 283 ns a draw). Star glow, engine velocity and the plain
  count saved; ruled out: items 2 and 3 as cuts (each moved a cold load, which
  was the cost). S2 cleared: 40% of eye-memo misses, 0 disagreements in 8.1M.
  #184 undetermined (no blocked return), as expected.
- **Build 6 ("Build 6: one interest mask"):** S1 as a write-through mask; S2
  and #184 with the pending flags (both approved 2026-11-04); build 5's items
  2-3 removed. Argon agreed; Opus approved on re-review. Spec #191,
  `d5135e52`. Flown 2026-11-04: S2 cut eye-memo misses 45% (about 0.08 ms);
  S1 saved nothing (ruled out); total unchanged at 283 ns a draw.
  Closing build (#194, `a7941eb0`): S1 and the temporary lines out. Flown
  2026-11-04: no regression; F95c's NEVER APPLIED is the scene, not the build.
  Arc total about 1.4-1.75 ms a frame. Merged unflown; next flight at the end.
- **Superseded (item 1, #163):** removed at review. `main`'s F196 and F207 want
  the depth probe on foot with AA off, so its size test could never decide. Its
  figures came from a tree without them.
- **Ruled out:** re-issues as a CPU cost (at most 0.004 ms per sampled frame).
  Item 4 as a saving (0 hits: the binding generation moves between every eye
  draw). Item 2 as a saving (the collector cleared nothing). Idling the whole
  marker tee without the UI layer (its slow path is the only publisher of the
  per-eye world rows, `eyeRowsPublish`). Changing `eyeOfDraw` itself
  (`rowSourceDraw` and five census callers would shift).
- **Left out, tracked:** #151 (instanced `drawPathFast`, removed at review:
  unreachable until #152), #152 (per-scene subscriber mask), #153 (cross-profile
  per-bind counter), #154 (`/GL /LTCG`), #183 (cleanup when the arc closes:
  worktrees, branches, captures, notes, install backups).

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

1. **Filter tiny clutter out of offscreen shadow cascades in the game's part
   test (`~5,000-10,000` offscreen draws/frame):**
   - About half of all `DrawIndexedInstanced` calls (~15,000-17,700/frame;
     build 2's log shows 7M offscreen `8BC3..` draws per 1800-frame window =
     ~3,900/frame on `8BC3..` alone) are offscreen shadow cascade and auxiliary
     views, and 45% of `X` draws have `IndexCount <= 64`.
   - The game's view loop (`docs\stereo-cpu-sharing-2026-10-16.md`) tells the
     two eyes and the shadow cascades apart by view. Skipping tiny clutter parts
     (e.g. a world radius under 0.20 m) in its part test when the active view
     is a shadow cascade keeps the part out of submission, EDHM, EDVR, D3D11 and
     the GPU.

2. **Sub-pixel clutter floor in the part test (screen-size test only, capped
   by world radius):**
   - The retired LOD governor (`fix.settlement_detail`,
     `docs\design-settlement-lod-bias-2026-09-22.md`) scaled the factor the
     asset LOD table is tested with, and dropped 4.2 m parts (`0.74 deg`) whose
     table entries were small.
   - The part test also has a separate pure screen-size test before that table.
     Enforcing a 2-3 pixel minimum projected diameter strictly on small props
     (under 0.35 m, leaving the table alone so large structures never pop)
     leverages the engine's 8.08x submission
     multiplier (`docs\engine-render-pipeline.md`: 2 eyes x LOD ranges x
     material layers): each culled sub-pixel part saves ~8 D3D11 draw calls.

3. **Extend `fix.optimized_stations` to an on-foot concourse keep-cell
   (`src\d3d11\optimized_station.cpp`):**
   - `fix.optimized_stations` currently releases completely on foot
     (`docs\optimized-station-2026-10-12.md`) because the ship-hangar box
     clipped the lift and concourse. While standing in the enclosed concourse,
     the engine still evaluates and submits exterior station superstructure,
     docking bay rings, and hangar pads. Using the node-name cache and rig
     hooks in `optimized_station.cpp` (or the engine's portal callback in its
     view loop), we can veto exterior/hangar station
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
  out: the screen-size half of the part test as the settlement lever, because
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
   - **B2 (the part test's sub-pixel floor):** Agreed, ruled out. The 8-pixel
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
   test must be exactly the callee's first test; the callee keeps it. The Opus
   review checked every row: each first test is a pure read with nothing before
   it. Three need more than a copy of the test:
   - `optimizedStationSkipsDraw` (`:2087`) evaluates two pure hash loads as
     its arguments, so the inline `g_sunOff` test goes before them;
     `g_sunOff` needs a header accessor, and the inline test is the same
     acquire load.
   - `gpuFrameCommandIfLive` makes two independent tests: the cut is a
     forced inline, not its first test.
   - `uiLayerTargetKind`'s test reads `g_tc`, file-local to `ui_layer.cpp`;
     it needs an inline accessor in the header, exposing either its
     `TargetCache` type or its `gen` and `kind`.
   - Antigravity's review (checked): `gpuFrameCommandMightAct`
     (`gpu_frame_timing.h:44-46`) tests the thread-local
     `g_gpuFrameInternal` before the relaxed `g_gpuFrameCommandLive` load;
     swapping them is exact (both pure reads) and keeps the thread-local
     off the path while `app_gpu_timing` is off. `flatTemporalCapturing`
     (`flat_temporal.h:24-32`): force-inline its first load and move the
     rest to an out-of-line slow path.
2. **The per-draw `State` fields in one 64-byte-aligned block at the head of
   `State`:** `ownerCtx`, `rtv0Eye`, `rtv0EyeGen`, `rtv0Cand`, `curveThisDraw`,
   `introCurveThisDraw`, `eyeDrawsThisFrame`, `bindAuditSeq`, the `censusSkip*`
   counts, `censusAutoW`, `quadSkipArmed`, `distanceEnabled`, `fssBodyFrame`,
   `frameNo`. Not `glareClamp`: foreign threads write it (`vscreen.cpp:2073`),
   so in the block it would share the owner's busiest cache line. Their lines
   cost about 0.13 ms; expected 0.05-0.1. Layout only: `State` lives only in
   `vscreen.cpp`'s anonymous namespace, nothing takes `offsetof` or copies it,
   and `new` honours a 64-byte alignment under C++17 (Opus review). A
   `static_assert` pins the block's size.
3. *(Dropped after review.)* The three thread-local per-draw flags as
   owner-only `State` members (0.05-0.08 ms). Not exact: an owner draw blocked
   at `vscreen.cpp:4262` returns before `UiDepthScope` clears the flags, and
   `t_holoDepthThisDraw` is not cleared with the other two at `:2114-2115`
   (checked), so today a foreign-context draw on the render thread can consume
   a flag a blocked owner draw left, which gates the reissues at `:4629` and
   `:4646`. Owner-only members would change that. The lingering flag is its own
   issue (#184); item 3 can return once that is settled.
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
   profile and across a switch (both reviews). About 0.05 ms. Mechanism:
   `open_` is private and the bounce sits behind a function-local static, so
   `FlatMapBounce` gains an inline `anyOpen()` (the same acquire load as its
   first tests) and `flat_runtime.cpp` publishes a pointer to it, a
   `std::atomic` stored with release inside the static initialiser and loaded
   with acquire, since `hookedMap` and `hookedUnmap` reach it from any thread.
   A null pointer reads as nothing open, which is exact even though
   construction is lazy: `open_` rises only in `install`, which goes through
   `mapBounce()` and so constructs it first. The object is never freed, so the
   pointer never dangles (re-review, proven). Moving the `flatcpu::Scope`
   changes what the flat profile's own CPU report counts for `BeginUnmap`: it
   no longer times the early return.
5. **`uiDepthOnEyeDraw`'s per-draw outputs in one struct**, reset as a block.
   0.112 of its 0.132 ms sits right after its per-draw stores to globals on
   separate lines (`ui_depth.cpp:2896-2908`: twelve resets plus the
   `g_holoDraw` copy of the input, 14 stores). Same stores, same values: the
   struct's defaults reproduce the resets (-1 for both eye fields), and every
   access moves with it, including the inline accessors in `ui_depth.h`.
   Expected about 0.05. Skipping the resets is not in this build.
6. **Shape before flag, and hash before guard, in the eye branch.** Pure reads,
   reordered:
   - `holoWantsDraws() && holoPatternShape` (`vscreen.cpp:2678`) and
     `backdropWantsDraws() && backdropCompositeShape` (`:2708`): both shapes
     are (X, 6, 1) (`holo_fix.h:58-60`, `backdrop_fix.h:126-128`).
   - `targetSharpWantsDraws() && targetSharpOnEyeDraw` (`:2685`): the callee
     tests the same (X, 6, 1) shape right after its flag
     (`target_sharp.cpp:317-318`); an inline `targetSharpShape` goes first
     (Antigravity). Its constants move from `target_sharp.cpp:32-34` to the
     header with it, so the shape has one definition.
   - `vscreen.cpp:1994-2000`: the two-hash VS test moves before
     `guardedBudget` (Antigravity). `bindingShaderHash` is a shadow read, and
     a spent budget skips the lambda either way (`guard.h:133-138`).
   - Not scrim: Antigravity's review gives its shape as (X, 6, 1), but
     `scrimWashShape` is `X, instances 1, count >= 100`
     (`scrim_fix.h:123-125`), which most meshes pass.
   Expected 0.04-0.06.
7. **Two counters, for build 5's decisions, on one new line (TEMPORARY).** One
   compare and increment per call, outside the per-draw ladder. Every
   `bindingSet` caller is owner-only (Opus review), so plain counters do.
   - `mapMemo` lookups and misses (`vscreen.cpp:3582-3587`). A miss pays
     `mapBufferDesc` (0.028 own plus two COM calls).
   - `bindingSet` calls for RTV0, DSV0 and PS SRV 0-3, the calls whose
     pointer equals the one bound (compared before it is overwritten), and
     null-to-null re-sets counted apart. Every set bumps the generation
     today (`binding_shadow.cpp:107-112`, checked), which is why every
     per-binding cache in the ladder misses (this arc's item 4). Pointer
     equality is an upper bound for S2, not its saving.
   - The line goes beside `draw mask:` in `perf_monitor.cpp`, on its
     1800-frame window, formatted by a header function (about 300
     characters). `edvr_log.py` gets a parse fixture and a self-test, and
     `--perf-windows` prints it; `--health` works by feature tag, so no
     health rule.
8. **The analyzer reports own time.** In `tools\cpu_profile`'s EDVR export
   (`Report.cs:626-630`), a sample is own when `stack.TopModuleId ==
   edvrModule`, the test behind `callerSamplesTopFrameInEdvr` (`:724-736`).
   Outputs: own and below counts per innermost address and per chain, and an
   own total, which must equal `callerSamplesTopFrameInEdvr` (the self-test).
   This replaces the disassembly script used above and makes own time the
   figure every later table ranks by.
9. **The hologram pass skipped by the VS byte** (Antigravity, checked).
   `classifyHologramDraw`'s two first declines read only the vertex shader's
   hash, the exclusion list and the family lists (`ui_depth.cpp:3598-3607`).
   Revised after the re-review: the VS byte is cached per shader and never
   invalidated (`vs_may_want.h:6-7`, `vscreen.cpp:3314-3318`), so it may hold
   only fixed shader-hash terms. The runtime list `g_holoFamilies` is always
   `kHoloPanel` plus `kHoloFamiliesBuiltIn` (`holoBuildFamilyList`,
   `ui_depth.cpp:2227-2259`, checked), and empty before `holoDepthConfigure`
   runs, when the classifier declines every draw anyway.
   - A `kVsMayHoloDepth` bit (0x10) is set when the hash is in
     `kHoloWorldMarkers`, is `kHoloPanel`, or is in `kHoloFamiliesBuiltIn`:
     the constexpr lists only, a superset of the runtime list at every
     moment. No exclusion term (`kNullPsMesh` is in neither list), so the
     bit never clears for a listed hash.
   - `kVsMayWantAll` becomes 0x1F, so hash 0 keeps the call: `boundVsHash`
     can still find such a shader's real hash through `VSGetShader`
     (`ui_depth.cpp:815-829`).
   - `kHoloPanel` moves from `ui_depth.cpp:77` to `holo_families.h`, and the
     header comment in `vs_may_want.h` drops UI depth from its "not in it"
     list for the hologram pass.
   - The skip, at `vscreen.cpp:2513`, keeps the callee's one store on a
     decline, `g_holoEye = -1` (`ui_depth.cpp:3628`), which
     `uiDepthHologramContributionBegin` reads (`:3651`): `g_holoEye`
     becomes `detail::g_holoEye` with an inline setter (an out-of-line one
     would cost the call the item removes; the hologram rig's unqualified
     uses, `tools\hologram_depth_test\hologram_depth_test.cpp:311` and
     six more, take `using detail::g_holoEye;`). It sets `t_holoDepthThisDraw`
     false, as the call returns on a decline, so #184's behaviour is
     unchanged.
   - A skipped call stores nothing in `g_holoMemo`. Its key repeats only
     while the VS pointer, and so the byte, is unchanged, so a stored entry
     came from a draw whose bit was set and no skipped draw can hit a stale
     one (re-review, proven).
   - Counted: a `MaskSkip::HoloDepth` count on the `draw mask:` line, as
     build 2's byte skips are (`vscreen.cpp:5585-5588`), so the log shows
     item 9 ran. It goes last in the enum, before `Count`, with the token
     `hologram_depth` last in `names[]` (`draw_mask_line.h:11`, `:15`).
   - `bindingInvalidate` leaves `mayWant` unchanged
     (`binding_shadow.cpp:114-119`); it is called only for `VsSrv33` and
     `Blend` today (`vscreen.cpp:6430-6431`). It resets `mayWant` to 0xFF,
     so a future call for `Vs` cannot leave a stale byte.
   Own 0.06 ms (`uiDepthHologramOnEyeDraw`); expected 0.03-0.05.

Expected total: 0.34-0.51 ms a frame of EDVR's own time (items 1, 2, 4, 5, 6
and 9), nearly all in the draw path.

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
  7's miss count); skipping `uiDepthOnEyeDraw`'s resets. Antigravity's gate on
  `g_surfaceCount != 0` (`ui_depth.cpp:330`) is exact but saves nothing where
  surfaces are learned, as in the concourse; it goes with the front cache.
- B1 (Antigravity's note, for later): neither `census_skip_offscreen` nor
  `census_skip` can target the 512x512 pass today, the first because it
  resolves RTV0, null on depth-only draws (`vscreen.cpp:2397-2411`), the second
  because it sits inside `if (s->rtv0Eye)` (`:2488`).
- Not candidates: the marker tee and probes (already ruled out), star glow's
  per-draw classify (it keeps per-target run state that any other draw ends,
  `vr_star_glow.cpp:362-374`), `depthProbeNoteDraw`'s counters.

### Test seams

- Item 1: each predicate is a header read; a rig check per inlined test that it
  equals the callee's first test over its inputs where the callee's state is
  reachable (`temporalPassWantsRigidDraw`'s state moves to `temporal_pass.h`).
- Items 1, 5, 6 and 9 cover the pixel and blend probes, the temporal pass's
  rigid draws, the holo, backdrop and target-sharp shapes and the hologram pass
  by equivalence in the rig, not by the flight: the probes are off in the
  flight, and no periodic line carries the rigid-draw count.
- Item 4: `flat_map_bounce_test` gains "`anyOpen()` false: `preMap` and
  `beginUnmap` touch nothing" and a flat-to-VR switch with one bounce open: the
  next Unmap still flushes it.
- Item 7: `edvr_log.py` parses the new line (fixture, self-test).
- Item 8: the analyzer's tests gain an own/below case, and its own total equals
  `callerSamplesTopFrameInEdvr`.
- Item 9: a clear bit implies `classifyHologramDraw` declines, over every hash
  in the three lists, `kNullPsMesh`, hash 0 and random hashes, with the runtime
  list both empty and configured; `kVsMayWantAll` includes the bit; a skipped
  draw leaves `g_holoEye` at -1.

### What the log shows if the new code never ran

- Item 7: no new counter line.
- Item 9: `draw mask:` carries no hologram-depth skip count, or 0.
- Items 1, 2, 4, 5, 6: nothing in the log; the profile shows it. With item 8,
  the per-line own time of the changed lines falls, and the out-of-line callees
  of items 1 and 9 drop out of the own list. Today's lines are 1994, 2087,
  2109, 2513, 2591, 2678, 2685, 2708, 4342-4345, 4361, 5460, 5498, 5544-5552,
  5599, 5604 and `ui_depth.cpp:2896-2908`; the edits move them, so the
  implementation records the new numbers in the commit.

### Flight

The L2 spot, standing still, DLAA, `EDVR_PROFILE_SYMBOLS=1`, one 100 s capture.
No census.

### Pass criteria

- **The cut:** total EDVR own time in R1, per full-path draw, falls by at least
  0.2 ms a frame's worth at build 3's draw count. Build 3: 4.70 ms own (the
  analyzer's uncapped leaf, windows 3-20) over the full-path draws a frame of
  the `draw mask:` windows that overlap the capture. Total, not the draw
  subtree: the analyzer caps each chain at 6 frames (`Report.cs:627`), and 0.25
  ms a frame of build 3's chains hit the cap without reaching a hook, so a
  subtree figure loses them (Opus review). Per draw, because the scene and draw
  count move between flights; `draw mask:` counts all four draw thunks, which
  is fine for a ratio between builds. Build 3's per-window spread gives a
  standard error of about 0.07 ms a flight, so about 0.10 ms for the difference
  between two flights: 0.2 ms is about two of them (re-review). That is the
  within-flight spread only; the spread between flights of the same build is
  not known. Item 4's part scales with Map/Unmap calls, not draws, so own time
  a frame is reported too.
- **Nothing changed on screen:** against build 3, `ui depth totals:`
  (`ui_depth.cpp:4507`), `ui quality: composites left in the scene:`
  (`ui_scene_composites.h:137`) and `vscreen footprint:`; `--health` with no
  new NEVER APPLIED. Tolerance: each count per full-path draw within the
  min-max range of build 3's own windows of that line after arrival. The rest
  is the rig's (test seams).
- **Nothing else rose:** no EDVR function's own time up more than 0.1 ms,
  comparing each item-1 and item-9 caller together with its former callees,
  since their tests move into the caller by design.
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

### Review of the build 4 design (Opus, 2026-11-03), and dispositions

A fresh-context review on Opus, against the code at `f5b20467`. Verdict:
changes required, three blocking findings, all fixed in the design text above.
Notes outside the repo: `b4-review.md`. The Antigravity review above was
checked claim by claim against the code; its dispositions are in the second
table.

| # | Opus finding | Disposition |
|---|---|---|
| 1 | BLOCKING: item 3 is not exact; a blocked owner draw leaves a flag a foreign render-thread draw consumes | Checked (`vscreen.cpp:4262`, `:2114-2115`). Item 3 dropped; the lingering flag is #184 |
| 2 | BLOCKING: "nothing changed" lines misnamed; no rigid-draw line; probes off | Real line names used; the rest left to the rig, said so |
| 3 | Item 1 exact; three rows need more than a copy | Added under item 1 |
| 4 | Item 2 safe; leave `glareClamp` out (foreign writes) | Taken |
| 5 | Item 4 exact; needs a mechanism; the `Scope` move changes the flat CPU report | Taken (`anyOpen()`, global pointer) |
| 6 | Items 5 and 6 exact; 14 stores; accessors in `ui_depth.h`; defaults -1 | Taken |
| 7 | BLOCKING: the subtree criterion loses 0.25 ms of capped chains; numerator and window mismatch | Criterion is now total own per full-path draw, standard error 0.07 ms stated |
| 8 | Item 8 implementable at `Report.cs:626-630` | Outputs named, self-test against `callerSamplesTopFrameInEdvr` |
| 9 | Item 7: owner-only callers; compare before overwrite; null-to-null apart; where and how it prints | Taken |
| 10 | Estimates labelled; "no function up 0.1 ms" trips on the intended moves; line numbers shift | Callers compared with former callees; new line numbers recorded at commit |

| Antigravity | Disposition |
|---|---|
| `gpuFrameCommandMightAct` reads the thread-local first | Checked (`gpu_frame_timing.h:44-46`); swap taken, item 1 |
| `flatTemporalCapturing` too big to inline | Checked (`flat_temporal.h:24-32`); force-inlined first load, item 1 |
| Hash before `guardedBudget` at `vscreen.cpp:1994` | Checked (`guard.h:133-138`); taken, item 6 |
| Scrim shape first | Not taken: `scrimWashShape` is `count >= 100`, not 6 (`scrim_fix.h:123-125`) |
| Target-sharp shape first | Checked (`target_sharp.cpp:317-318`); taken, item 6 |
| Hologram pass by VS bit | Checked (`ui_depth.cpp:3592-3638`); taken as item 9, keeping the callee's `g_holoEye = -1` |
| `g_surfaceCount != 0` gate | Exact, no saving in the concourse; with build 5's front cache |
| Item 3 agreed | Superseded by Opus finding 1 |
| B1: `census_skip*` cannot target the 512x512 pass | Recorded for B1 |

**Re-review (Opus, fresh context, against `b2b0ee41`).** Changes required, one
blocking finding; the first review's blocking findings 1, 2 and 7 resolved.
Notes outside the repo: `b4-rereview.md`.

| # | Finding | Disposition |
|---|---|---|
| 1 | BLOCKING: item 9's "recompute the byte" has no mechanism; the byte is never invalidated; hash 0 must keep the call | Checked (`vs_may_want.h:6-7`, `:25`; `ui_depth.cpp:2227-2259`). Bit from the constexpr lists only, `kVsMayWantAll` 0x1F, test seam rewritten |
| 2 | Item 9 otherwise exact; `g_holoEye` is file-local | Inline setter on `detail::g_holoEye` |
| 3 | Item 4 exact; the pointer is crossed by any thread | `std::atomic`, release store, acquire load |
| 4 | Item 6 exact; target-sharp constants live in the .cpp | Moved to the header |
| 5 | Item 1 additions exact; `g_sunOff` is an acquire load; `g_tc` is a struct | Stated |
| 6 | Sum is 0.34-0.51; 0.2 ms is about two SEs of a difference; item 4 scales with maps; no tolerance | All fixed in the text |
| 7 | The item 7 line is missing from Status | Added |
| 8 | Optional: count item 9's skips on `draw mask:` | Taken; also its never-ran signature |

**Re-review 2 (Opus, fresh context, against `d398e131`): approved.** Item 9 is
exact against the code: the three lists are `constexpr`, the runtime list is
always a subset, widening `kVsMayWantAll` changes no subscriber (each tests
only its own bit), nothing reads `g_holoEye` between the skip and the next
call, and the one-slot memo's key covers what a plain decline reads
(`classifier_memo.h:40-42`). Its five non-blocking notes are in the text: the
hologram rig's `g_holoEye` uses, the enum position and token, `kNullPsMesh` in
the test, `bindingInvalidate` resetting `mayWant`, and the contradictory
"flight-to-flight spread" phrase removed. Notes outside the repo:
`b4-rereview2.md`.

## Flight 2026-11-03 (build 4)

Build `c86670bd` (log `edvr_gfx_20261103_225315.log`, `--expect-build HEAD`
matches), the L2 spot, standing still, DLAA, one 100 s capture (20 windows of 5
s, no events lost, coverage complete). `--health`: no NEVER APPLIED. Notes
outside the repo: `profile-b4\own_cmp.py`, `fn_cmp.py`, `subtree.py`,
`perdraw.py`.

**The scene was heavier than build 3's.** Full-path draws a frame, from `draw
mask:`: 17,148 in the log window 22:56:32-22:57:38, which spans profile windows
7-20 (build 3: 13,339 and 13,620). Of them, `ui_family` skipped 79% (build 3:
95%) and `weapon_motion` 3,450 a frame (build 3: 513), so the extra draws are
mostly ones that reach more of the path. Runtime cycle 36.5 ms (build 3: 33.8).
Windows 1-6 sit in a log window that began 25 s before the capture, so its
14,099 is not their count; their own time a frame (5.13 ms) equals windows
7-20's (5.22), so the draw count was likely the same throughout. Windows 7-20
only are used per draw.

**The cut: passed.** EDVR's total own time in R1 (the analyzer's own total,
which equals its uncapped leaf):

| | Build 3 (w3-20) | Build 4 (w7-20) |
|---|---|---|
| Own, ms a frame | 4.699 (SE 0.068) | 5.215 (SE 0.052) |
| Own per full-path draw | 348.6 ns | 304.1 ns |
| At build 3's draw count (13,480) | 4.699 | 4.100, cut **0.60 ms** |
| Without Map/Unmap own (per-frame, not per-draw) | 300.5 ns | 267.3 ns, cut **0.45 ms** |
| Draw-hook subtree own (capped chains) | 3.293 ms, 244 ns | 3.781 ms, 220 ns, cut 0.32 ms |

The criterion (0.2 ms) holds on every reading; 0.45 ms is the fair one, since
Map/Unmap cost does not scale with draws. It assumes the extra draws cost the
same per draw as build 3's; they reach more of the path, which would understate
the cut rather than inflate it.

**Item 4 saved nothing measurable.** Own a frame under `hookedMap` 0.255 to
0.242 ms and `hookedUnmap` 0.393 to 0.389 ms. Ruled out: the map bounce's calls
as a cost while nothing is open (under 0.02 ms a frame).

**Where the draw path's cut came from** (draw-hook subtree, build 4 scaled to
build 3's draw count): `beginPanelOverride` -0.08, `uiDepthOnEyeDraw` -0.05,
`viewIsSurface` -0.03, `objectProbeOnEyeDraw` -0.03; the item 1 and item 9
callees gone from the profile, 0.21 together (`uiDepthHologramOnEyeDraw` 0.06,
`pixelProbeAfterEye` 0.05, `pixelProbeBefore` 0.04, `gpuFrameCommandIfLive`,
`objectProbeCandidateDrawIssued`, `uiLayerTargetKind`, `blendProbeWantsDraws`,
0.02 each).

**Nothing else rose more than 0.1 ms**, grouping each caller with its former
callees. The forward lambdas plus those callees: 0.84 ms to 0.81. The
`observedDraw` lambda (`vscreen.cpp:4387`) rose from 0.087 with its callee to
0.139, mostly at `:4394`, after the inlined ledger test. `starGlowAfterDraw`
+0.04 and `hookedDrawIndexedInstanced` +0.04 (both scaled) are within the
limit, and both depend on the scene.

**Nothing changed on screen.** Composites left in the scene: 0 in every window
(build 3: 0-6). `vscreen footprint:` is in neither log. `ui depth totals:`
wrote depth for 6.6 and 5.9 interface draws a frame (build 3: 6.0 and 6.0);
left alone 18.6 and 28.4 (20.5-33.1). Per full-path draw, the letter of the
criterion, 6.6 and 5.9 fall just outside build 3's two windows (4.68e-4 and
3.44e-4 against 4.41e-4 to 4.50e-4). Interface draws do not scale with world
draws, so per frame is the right ruler here, and per frame they are within 10%.
The rigs check item 5's resets exactly.

**Item 7: the counts for build 5** (last window, a frame):
- `mapMemo`: 1,935 lookups, 324 misses (17%).
- `bindingSet` re-setting the pointer already bound: PS SRV 0 63%, SRV 1 57%,
  SRV 2 53%, SRV 3 55%, about 6,900 calls a frame; RTV0 6%, DSV0 7%.
  `bindingSet`'s own time is 0.06 ms a frame, so S2's worth is in the caches
  keyed on generations, not in the set itself. RTV0 and DSV0 change on nearly
  every set, so a cache keyed on them gains little from S2.
- Item 9 skipped the hologram call for 13,650 draws a frame (`hologram_depth`
  24,578,362 of 30,867,090).

## Build 5: the per-draw calls that decline at once, and the counts for build 6 (designed 2026-11-03)

**Evidence.** Build 4's own time (windows 7-20, 17,148 full-path draws a frame,
5.22 ms) has no line above 0.10 ms a frame: the cost is spread over hundreds of
lines. The AA-off fast path (`draw_fast_path.h`) needs `fix.temporal_aa` off,
so under DLAA it never runs and every draw takes the full path. Build 5
therefore takes the remaining exact cuts, each a call or lookup that a draw
pays and that declines at once, and counts what build 6's larger options (S1,
S2) would save. A code survey (Opus, read-only, notes outside the repo:
`b5-survey.md`) checked each candidate; anchors are at `c86670bd`.

### Changes (keyless; each changes nothing a feature decides)

1. **Star glow's call gated at the call site.** `starGlowAfterDraw`
   (`vscreen.cpp:4042-4048`, noinline, called from `:5301`, `:5383`, `:5473`,
   `:5650`) runs whenever `vrStarGlowWanted()` is true, and the HUD bloom alone
   keeps that true (`vr_star_glow.cpp:589`, `:641`). The flight had the HUD
   bloom on (log `vr hud glow: on`) and star glow off (no `vr star glow` line),
   so every draw paid two calls and an eager `uiLayerIssueBlocked()` to reach
   `if (g_mode != VrStarMode::kOn) return false;` (`vr_star_glow.cpp:656`). New
   inline `vrStarGlowAfterMayAct()` =
   `g_t0.active || g_mode == kOn` (both render-thread plain values, exported
   in `detail::`), tested before the call. Exact: when it is false the callee
   returns false before touching anything. Own time now: 0.108 + 0.041 ms a
   frame (`starGlowAfterDraw` `:4044`, `:4048`; `vrStarGlowAfterDraw`).
   Expected 0.10-0.15.
2. **`uiDepthOnEyeDraw`'s memo hit inline.** About nine eye draws in ten hit
   the eye memo (`ui_depth.cpp:2899`), yet each pays the out-of-line call, the
   struct reset (`:2893`), a 24-byte copy `g_holoDraw = draw` (`:2894`) and a
   re-test of the call site's own gate (`:2895`, duplicating
   `vscreen.cpp:2535`). Move the memo-hit test into `ui_depth.h`
   (`classifiermemo::hit` is already inline, `classifier_memo.h:40`) and the
   `g_holoDraw` copy into the miss path. Exact: on a hit every field of
   `g_uiDepthDraw` holds its default, because only `classifyEyeDraw` (the
   writers at `:2662-2875`) and `uiDepthReissueBegin`'s `coronaMotion = true`
   (`:3355`, reached only after a classify in the same draw) write a
   non-default value; and `g_holoDraw`'s readers (`:2953`, `:3260`, `:3354`)
   need a field only a classify sets. The reset therefore runs on the miss path
   and after any draw that classified: a `dirty` flag set before
   `classifyEyeDraw` (`:2901`) and at `:3355`. Belt and braces: the eye memo is
   stored valid only on a plain decline, which writes no non-default field, so
   the draw after any write always misses (Argon review); the flag keeps
   exactness from resting on the classifier's exits. The inline order stays the
   callee's: reset if dirty, then the `g_uiDepthOn`/stood-down test, then
   `bindingEyeKeyGeneration()`, which both rigs stub with `std::abort()`
   (`ui_depth_test.cpp:53`, `hologram_depth_test.cpp:78`). Build 4's rig check
   (`ui_depth_test.cpp:200-220`) dirties the struct by hand and asserts the
   `g_holoDraw` copy on an off call; it sets the dirty flag, and asserts the
   copy on a miss instead. Expected 0.04-0.06.
3. **A front cache for `viewIsSurface`** (`ui_depth.cpp:862-871`, 0.102 ms a
   frame at `:865`): `g_viewMemo.get` hashes and probes up to 4 slots of a 24
   KB table that is cold after the game's draw. One front entry per PS slot
   0-3, indexed by the slot both loops already walk (`:2419`,
   `uiDepthSampledSurfaceSlot`; `:2656`, `classifyEyeDraw`); the log caller at
   `ui_layer.cpp:3985` keeps the uncached call. Each entry: `{view, answer,
   frame, classifierEpoch, putSeq}`, hit when all five match. New
   `g_viewMemoPuts`, bumped beside each `g_viewMemo.put` (`:869`, `:2566`) and
   by `clear()` (`:4590`). Exact: with no put since the fill the memo slot is
   unchanged, the same frame cannot newly fail the life test (`:281`), and a
   table change moves the epoch (`:772`, `:798`, `:2567`, `:4591`). Without
   `putSeq` it is NOT exact: a put for another key can evict this entry's slot
   (`:289-306`), and the real path would then re-resolve and can drop a surface
   (`:767-772`). Expected 0.03-0.05.
4. **Engine velocity's five generation compares become one**
   (`engine_velocity.h:175-179`). A new aggregate `g_bindingEvKeyGen`, bumped
   by a second slot mask in `noteEyeKeySlot`'s style
   (`binding_shadow.cpp:27-35`) for Vs, Ps, Rtv0, Dsv0 and Blend, so every
   per-slot bump that calls `noteEyeKeySlot` (`bindingSet`,
   `bindingInvalidate`, `bindingSetShader`, the last including
   `psShadowProbe`'s heal) moves it; and bumped by `bindingForgetAll` (`:148`)
   and `bindingFrameBoundary` (`:157`), the only other per-slot bump sites.
   `beforeDrawSlow` stores it (`engine_velocity.cpp:3126-3130`). It starts at 1
   and `DrawCache::evGen` at 0, as per-slot `gen` starts at 1
   (`binding_shadow.h:125`) against `DrawCache`'s zeros
   (`engine_velocity.h:109`): `DrawCache` has no valid flag, so a fresh or
   reset cache (`cache = DrawCache{}`, seven sites) is stale only because 0
   never equals a generation (Argon review). The two pointer compares, `eye`
   and the guarded `srv3` compare stay. Exact: the aggregate moves if and only
   if a member's generation moves. Not folding `PsSrv3` in: it is compared only
   while `cache.guarded`, and folding it in would add slow paths, which are
   counted and call `restore()`. Expected 0.02-0.04.
5. *(Moved to build 6 after review.)* `t_uiDepthThisDraw` and
   `t_compositeThisDraw` passed per call. Not exact: they are cleared at
   `vscreen.cpp:2143-2144`, after the foreign-context return (`:2101-2111`) and
   the skip returns (`:2096`, `:2116`, `:2118`). A flag an owner draw set and
   left at the blocked return (`:4308`) is consumed by the next foreign-context
   draw on the render thread, whose `UiDepthScope` then calls `uiDepthEnd`
   (`:4332`). That is #184's shape for all three flags, not only the hologram
   one (Argon review). Item 6 counts it; the fix and the per-call move go
   together.
6. **TEMPORARY counts, one line beside `draw mask:`**, replacing build 4's
   `binding re-sets:` line (its counts are recorded above, and its
   `bindSetNote` call in `bindingSet` goes):
   - #184, for each of the three flags: blocked owner returns
     (`vscreen.cpp:4308`) with it set, and `UiDepthScope` constructions that
     consume it set on a draw whose `beginPanelOverride` did not set it (a
     per-draw sequence number stamped where the flag is set).
   - S2's size: eye-memo misses (`ui_depth.cpp:2899`) and hologram-memo
     misses (`:3619`), and of each, how many a pointer-only aggregate would
     have hit. The pointer-only aggregate is a second counter, bumped on a
     PS SRV 0-3 set only when the pointer changes and on every other member
     set as today; it decides nothing. Every same-pointer SRV re-set moves
     today's eye-key aggregate (`binding_shadow.cpp:28-34`, `:112-118`) and
     makes the next eye draw a miss, which runs the classifier with up to
     four `viewIsSurface` calls (`:2655-2660`). Shadow memos for the eye and
     hologram memos, keyed on the pointer-only aggregate in place of today's,
     mirror every store and invalidation of the real ones
     (`ui_depth.cpp:2902`, `:3622`, `:4592`). On each real miss the line
     counts `ptr_would_hit` (the shadow hits and the classifier then makes a
     plain decline: S2's saving) and `ptr_would_disagree` (the shadow hits
     and the classifier does not: S2 would have changed an answer). Zero
     disagreements over the flight answer S2's exactness question on the real
     frame stream (Argon review).
   - Item 3's front-cache lookups and hits.

   `edvr_log.py`: a regex, a fixture and a self-test; `--perf-windows` prints
   the line.

### Item 7: the cross-profile probe's count without a locked add (maintainer's choice, 2026-11-04)

In VR every PS constant-buffer bind runs
`cross_profile::enterSite(Site::FlatPsConstantBuffers, true).stop()`
(`vscreen.cpp:3223`, 0.096 ms a frame own): a `lock xadd` on one counter
(`cross_profile_cost.h:83`) and, one call in 64, two `QueryPerformanceCounter`
calls around an empty stretch. No feature reads it; it is the instrument of
`docs\design-unified-install-2026-10-12.md` section 3.3. Of the three options
(remove the site, a plain count, keep it), the maintainer took the
recommendation, the plain count: this one site gets a `Probe` begin that does a
relaxed load and store of `calls` in place of the `fetch_add`
(`cross_profile_cost.h:83`). The sampling test, the timing and the report are
unchanged, and every other site keeps the `fetch_add`. Exact for everything a
feature decides (nothing reads it). The count is exact on one thread; a PS
constant-buffer bind from a deferred context on another thread at the same
moment can lose one count, which the report tolerates. Expected 0.06-0.09. Test
seam: `tools\cross_profile_test`, the site's count over a run of calls on one
thread equals the calls.

### Left for build 6, each needing build 5's counts or a design

- **S2 on the eye-key aggregate only** (per-slot generations unchanged, so
  `srv0IsPanelSized`, `vscreen.cpp:1450`, and the scrim cache,
  `scrim_fix.h:105-109`, keep their protection against a recycled address,
  `binding_shadow.h:163-165`). Sized by item 6. The open exactness question: an
  SRV that D3D unbinds silently, is freed, and is replaced by a new view at the
  same address. The eye memo already depends on `viewIsSurface`'s pointer-keyed
  memo, so S2 adds only the case where that memo's entry was evicted in
  between; the review should settle whether that is acceptable or needs a
  guard.
- **S1 as a write-through interest mask** in `DrawHot`'s 11 spare bytes
  (`vscreen.cpp:415-432`), each flag's writer setting or clearing its bit.
  Ruled out: a per-frame snapshot, because stand-downs, the issue fence
  (`ui_layer.cpp:1867`) and probe arms flip mid-frame, so a snapshot can be
  stale-false. Expected 0.05-0.15; it touches about 15 writers.
- The three per-draw flags per call and the #184 fix, by item 6's counts (item
  5 above).
- Star glow's decline with star glow on (an inline Rtv0-generation target test,
  `vr_star_glow.cpp:317-357`): not needed while the pilot flies with star glow
  off.
- B1, the 512x512 depth pass: not identified yet.
- Ruled out: Antigravity's `g_surfaceCount != 0` gate (`ui_depth.cpp:327`),
  because the view memo keeps an answer of 1 for up to 120 frames after a
  surface is evicted (`:156`, `:767-772`), so it is not exact; and the
  `observedDraw` lambda (`vscreen.cpp:4387-4394`) as a target, because its
  samples are skid after the real draw call returns (`:5560`) and its ledger
  test is already inline (`object_probe.h:72`).

### Test seams

- Item 1: `tools\ui_quality_test` (the VR star and HUD glow headers): with the
  HUD bloom on and star glow off, `vrStarGlowAfterMayAct()` is false and
  `vrStarGlowAfterDraw` returns false without side effects; with either
  `g_t0.active` or star glow on, it is true.
- Item 2: `tools\ui_depth_test`: over a sequence of hit, miss, hit, reissue,
  hit, the `ui_depth.h` accessors return the same values as before.
- Item 3: `tools\ui_depth_test`: the front cache's answer equals `g_viewMemo`
  plus the resolve over fills, puts for other keys that evict, epoch moves and
  frame changes.
- Item 4: `tools\engine_velocity_test`: the aggregate moves exactly when one of
  the five generations moves, including forget-all and the frame boundary.
- Item 4 also: a reset `DrawCache{}` takes the slow path before any bind.
- Item 6: `edvr_log.py --self-test`; in `ui_depth_test`, a same-pointer re-set
  makes the real memo miss and the shadow memo hit.

### What the log and profile show if the code never ran

- No item 6 line, or front-cache hits of 0 with lookups above 0.
- `starGlowAfterDraw` and `vrStarGlowAfterDraw` still in the profile at about
  0.15 ms a frame.
- `uiDepthOnEyeDraw` own time unchanged (0.12 ms a frame at 17,148 draws).

### Flight

The L2 spot, standing still, DLAA, HUD bloom on and star glow off as now, one
100 s capture. No census.

### Pass criteria

- **Each item, in the profile, per full-path draw:** `starGlowAfterDraw` and
  `vrStarGlowAfterDraw` gone; `uiDepthOnEyeDraw` own down by at least half;
  `viewIsSurface` own down; the `engine_velocity.h:175` line down;
  `hookedPSSetConstantBuffers` own down (item 7); items 1-3 judged as caller
  plus callee and item 4 on the thunks (Opus review, finding 8). Judged per
  item, because the expected total (0.25-0.39 ms a frame at 17,148 draws, about
  0.20-0.31 at build 3's count) is within two standard errors of the difference
  between two flights (about 0.09 ms).
- **The total:** own time per full-path draw not up; reported against build 4.
- **Nothing changed on screen:** build 4's lines, judged per frame for `ui
  depth totals:` (interface draws do not scale with world draws) and per
  full-path draw otherwise; `--health` with no new NEVER APPLIED.
- **Nothing else rose:** no function's own time up more than 0.1 ms, grouping
  callers with their former callees.
- **Build 6 is decided:** item 6's counts are in the log.

### Review of the build 5 plan (2026-11-04, Antigravity)

All six items, the two Build 6 preview items (S1/S2), and the ruled-out notes
were verified against `c86670bd`. Items 1, 3, 5, and 6 are sound as designed;
items 2 and 4 have one blocking initialization or test-rig catch each that
would fail `build.bat` or break exactness on the first draw if not addressed
during implementation.

1. **Item 4 (`g_bindingEvKeyGen` initial value vs `DrawCache::evGen` — blocking
   exactness catch):**
   - In `src/d3d11/binding_shadow.h:182`, per-slot generations initialize to
     `uint32_t gen = 1`, whereas in `src/d3d11/engine_velocity.h:109`,
     `DrawCache`'s cached generations default to `0` (`uint32_t vs = 0, ps =
     0, rtv = 0, dsv = 0, blend = 0`).
   - Because `DrawCache` has no `bool valid` field, `engineVelocityBeforeDraw`
     (`engine_velocity.h:172-176`) relies on `0 != 1` to force a slow-path run
     on the first draw and after any `cache = DrawCache{}` reset
     (`engine_velocity.cpp:3048, 3091, 3364`).
   - By contrast, `detail::g_bindingEyeKeyGen` in
     `src/d3d11/binding_shadow.cpp:16` initializes to `0` only because
     `classifiermemo::Memo` carries `bool valid = false`
     (`src/d3d11/classifier_memo.h:18`).
   - If `g_bindingEvKeyGen` initialized to `0` while `DrawCache::evGen`
     initialized to `0`, a freshly zeroed `DrawCache{}` before any bind on the
     five EV slots (or in `tools\engine_velocity_test` after a cache reset)
     would falsely match `cache.evGen == bindingEvKeyGeneration()` (`0 == 0`)
     and skip `beforeDrawSlow`.
   - **Fix:** initialize `detail::g_bindingEvKeyGen = 1` in
     `binding_shadow.cpp` (and reset it to `1`, or bump `++g_bindingEvKeyGen`,
     in `bindingForgetAll`) while keeping `DrawCache::evGen = 0`. Also check
     `psShadowProbe` (`engine_velocity.cpp:3297`): when the 1-in-64 probe heals
     a desynced PS via `bindingSetShader(BindSlot::Ps, livePs, liveHash)`,
     `BindSlot::Ps` is in `kEvKeySlots`, so `g_bindingEvKeyGen` bumps before
     `beforeDrawSlow` stores `cache.evGen = bindingEvKeyGeneration()`.

2. **Item 2 (`uiDepthOnEyeDraw` inline hit in `ui_depth.h` — test-rig
   `std::abort()` and production invariant):**
   - **Test-rig ordering (`EDVR_BINDING_SHADOW_EXTERNAL`):** Both
     `tools/ui_depth_test/ui_depth_test.cpp:5, 53` and
     `tools/hologram_depth_test/hologram_depth_test.cpp:15, 78` define
     `EDVR_BINDING_SHADOW_EXTERNAL` and stub `uint32_t
     bindingEyeKeyGeneration() { std::abort(); }` with the comment `read only
     after the classifier's own on test`.
     - The inline `uiDepthOnEyeDraw` in `src/d3d11/ui_depth.h` must still test
       `if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown)` (and reset
       `detail::g_uiDepthDraw` when `dirty`) **before** calling
       `bindingEyeKeyGeneration()`, or any call with `g_uiDepthOn == false` in
       either rig will `std::abort()`.
     - In `tools/ui_depth_test/ui_depth_test.cpp:200-220` (Build 4's item 5
       test), the rig directly mutates `detail::g_uiDepthDraw` (`:201-206`),
       sets `detail::g_uiDepthOn = false` (`:207`), calls `uiDepthOnEyeDraw`
       (`:208`), and asserts both that `g_uiDepthDraw` was reset (`:210-218`)
       and that `g_holoDraw` copied the input `HoloDraw` (`:219`). Moving
       `g_holoDraw = draw` to the miss path and gating the reset of
       `g_uiDepthDraw` on `dirty` will fail `:210-219` unless that test sets
       `dirty = true` (or dirties through the miss path) and tests the
       `g_holoDraw` copy on a miss with a live `bindingEyeKeyGeneration()`
       stub in `ui_depth_test.cpp` (needed anyway for item 2's test seam at
       `:2308`).
   - **Production exactness invariant on `g_uiDepthDraw`:** Even stronger than
     the `dirty` flag argument at `:2203-2209`, in production `g_eyeMemo.valid`
     can *never* be `true` while `g_uiDepthDraw` holds a non-default field:
     - `g_eyeMemo.valid` is set to `true` only by
       `classifiermemo::store(g_eyeMemo, now, exit)` (`ui_depth.cpp:2902`) when
       `exit == classifiermemo::Exit::kPlainDecline` (`classifier_memo.h:47`).
     - In `classifyEyeDraw` (`ui_depth.cpp:2640-2709`), the only write to
       `g_uiDepthDraw` before `:2707` (`*exit = kPlainDecline`) is
       `detail::g_uiDepthDraw.composite = composite` (`:2662`), and `:2705`
       requires `!composite` to reach `:2707`. Every path that writes a
       non-default value to `g_uiDepthDraw` (`composite = true`, `planetPending
       = true` at `:2672`, `worldScreenPending = true` at `:2684`, `mode =
       kReissueScene` at `:2795`, `mode = kReissue` at `:2869`, or
       `uiDepthReissueBegin`'s `coronaMotion = true` at `:3355`, which runs
       only when `uiDepthOnEyeDraw` returned `true`) leaves `exit == kOther`,
       which sets `g_eyeMemo.valid = false`.
     - Thus the very next eye draw after any write to `g_uiDepthDraw` is
       guaranteed to miss `g_eyeMemo` and take the slow path. Keeping `dirty`
       (set before `classifyEyeDraw` at `:2901` and in `uiDepthReissueBegin` at
       `:3355`, or cleared on reset) is still good practice for test seams that
       poke `g_uiDepthDraw` directly.

3. **Item 1 (`vrStarGlowAfterMayAct()` inline gate — verified exact):**
   - Verified at `src/d3d11/vr_star_glow.cpp:653-656` and
     `src/d3d11/vscreen.cpp:4042-4048`. In all four draw thunks
     (`vscreen.cpp:5292, 5374, 5464, 5546`), `starGlow` already requires `self
     == g_state->ownerCtx` (`self != nullptr`), so `vrStarGlowAfterDraw`
     reaches `if (!g_t0.active && g_mode != VrStarMode::kOn) return false;` at
     `:655-656` before touching `plain`, `g_win.draws`, or `g_budget`. And in
     `starGlowAfterDraw` (`vscreen.cpp:4044`), `v == DrawVerdict::kNone &&
     !uiLayerIssueBlocked()` is pure, so gating the call on `if (starGlow &&
     vrStarGlowAfterMayAct())` at `vscreen.cpp:5332, 5419, 5509, 5650` is
     exact.

4. **Item 3 (`viewIsSurface` front cache with `putSeq` — verified exact):**
   - Verified at `src/d3d11/ui_depth.cpp:276-310, 762-799, 866-875, 2565-2568,
     2653-2660, 4590-4591`. Including `g_viewMemoPuts` (`putSeq`) in the 4-slot
     front cache entry is essential because `g_viewMemo.put` on another slot
     can evict a 4-way set entry (`:297-305`), and the subsequent `g_viewMemo`
     miss re-runs `surfaceMatches` (`:764`), whose recycled-texture check at
     `:767-772` has the side effect of evicting a stale surface entry and
     incrementing `g_classifierEpoch`. With `putSeq` in the key, a front-cache
     hit guarantees `g_viewMemo.get` would have hit without reaching
     `surfaceMatches`.
   - Note: `viewIsSurface` has a second caller in `classifyEyeDraw` at
     `ui_depth.cpp:2739-2745` (`int sampledSlot = -1; for (int i = 0; i < 4;
     ++i) if (viewIsSurface(bindingGet(kSlots[i]))) { sampledSlot = i; break;
     }`). Passing the slot index `i` (`0..3`) at both `:2656` and `:2741` (or
     sizing the front cache as `FrontEntry[4]` indexed by `slot`) covers both
     loops cleanly.
   - Agreed on ruling out `g_surfaceCount != 0` (`:2295-2297`): when the last
     tracked surface is evicted (`ui_depth.cpp:769`, `--g_surfaceCount`),
     positive entries in `g_viewMemo` remain valid for up to `kMemoLifeFrames
     = 120` frames (`:281`) unless invalidated by `g_classifierEpoch`, so a
     `g_surfaceCount == 0` short-circuit is not strictly identical to
     `g_viewMemo`.

5. **Item 5 (`t_uiDepthThisDraw` and `t_compositeThisDraw` passed per call —
   verified exact, plus an edge-case improvement on foreign draws):**
   - Verified across `beginPanelOverride` (`vscreen.cpp:2065-2189`),
     `forwardWithVerdict` (`:4295-4716`), and the four draw thunks (`:5295,
     5377, 5467, 5549`). When `drawPathFast(v)` is true (`:5439, 5527`),
     `aaOff` is true (`:4354`), so `uiDepthWantsDraws()` is false and both
     flags are unconditionally `false`.
   - Returning `{verdict, uiDepth, composite}` by value from
     `beginPanelOverride` actually fixes a latent `thread_local` hazard if an
     owner draw ever sets `t_uiDepthThisDraw = true` and then returns early at
     `if (owner && uiLayerIssueBlocked()) return;` (`vscreen.cpp:4308`, above
     `UiDepthScope` at `:4339`): a subsequent foreign-context draw returns
     early from `beginPanelOverride` at `:2083` before `:2143`
     (`t_uiDepthThisDraw = false`) and would otherwise inherit the stale TLS
     flag in `forwardWithVerdict`. With per-call return values, `:2083` returns
     `{DrawVerdict::kNone, false, false}`. (Even better: construct
     `UiDepthScope` before `:4308` or call `if (uiDepth) uiDepthEnd(self);` on
     the `:4308` return so `g_uiDepthDraw.mode` is also cleaned up.)

6. **Item 6 (temporary counters for #184 and S2 — one addition to prove S2's
   exactness question directly from the flight log):**
   - At `:2279-2284`, the open exactness question for Build 6's S2 (skipping
     the aggregate generation bump on a same-pointer PS SRV 0-3 re-set) is
     whether a silent D3D unbind + free + address-reused SRV + evicted
     `g_viewMemo` entry ever causes a pointer-only memo hit to disagree with
     `classifyEyeDraw` / `classifyHologramDraw`.
   - Because in Build 5 `classifyEyeDraw` and `classifyHologramDraw` **still
     run on every real `g_eyeMemo` and `g_holoMemo` miss**, item 6's shadow
     pointer-only memos (`g_eyeMemoPtrOnly`, `g_holoMemoPtrOnly`, keyed on
     `{bindingEyeKeyPtrOnlyGen(), g_classifierEpoch}` and updated via
     `classifiermemo::store` at `:2902` and `:3622` and invalidated at `:4592`)
     can count **both**:
     1. `ptr_would_hit`: `classifiermemo::hit(g_eyeMemoPtrOnly, nowPtr)` on a
        real memo miss where `classify*` then returns `exit == kPlainDecline`
        (the draws S2 will save in Build 6), and
     2. `ptr_would_disagree`: `classifiermemo::hit(g_eyeMemoPtrOnly, nowPtr)`
        on a real memo miss where `classify*` returns `exit != kPlainDecline`!
   - If `ptr_would_disagree == 0` across the flight, Build 5's log settles the
     S2 exactness question empirically on the real frame stream.

7. **On the maintainer question (`cross_profile::enterSite` at
   `vscreen.cpp:3223`):**
   - Confirmed at `src/d3d11/vscreen.cpp:3221-3225` and
     `src/d3d11/cross_profile_cost.h:82-98`: when `!runtimeFlatProfile()`,
     `enterSite(Site::FlatPsConstantBuffers, true).stop()` executes a locked
     `fetch_add(1, relaxed)` on every `PSSetConstantBuffers` call (~17k/frame)
     and two `QueryPerformanceCounter` calls every 64th call across an empty
     span before forwarding to `realPSSetConstantBuffers`. Removing the
     empty-span timing in VR (option a or b, with Sean's approval) recovers
     ~0.08 ms of pure instrumentation overhead with zero effect on flat-profile
     behaviour.

### Reply to the build 5 review (2026-11-04)

Each point checked at `c86670bd`; the design above now carries the outcome.

| # | Argon's point | Disposition |
|---|---|---|
| 1 | Item 4: the aggregate must start at 1 against `DrawCache::evGen = 0` | Agreed and taken. Checked: per-slot `gen` starts at 1 for the same reason (`binding_shadow.h:125`), `DrawCache`'s generations are 0 (`engine_velocity.h:109`), and it has seven `cache = DrawCache{}` resets. `psShadowProbe`'s heal goes through `bindingSetShader`, which calls `noteEyeKeySlot` (`binding_shadow.cpp:128-135`), so it moves the aggregate; the five per-slot bump sites are listed in item 4. |
| 2 | Item 2: the rigs abort on `bindingEyeKeyGeneration()` before the on test; build 4's rig check asserts the reset and `g_holoDraw` copy on an off call | Agreed and taken: the inline keeps the callee's order, and the rig check moves to a miss (`ui_depth_test.cpp:200-220`, checked). The plain-decline invariant is agreed too; the dirty flag stays as the guard that does not depend on it. |
| 3 | Item 1 exact | Agreed. |
| 4 | Item 3 exact; a second loop calls `viewIsSurface` | Agreed; the second loop is at `ui_depth.cpp:2419` (`uiDepthSampledSurfaceSlot`) at this commit, not `:2739-2745`, which is the HUD family test. Both loops index the front cache by slot. |
| 5 | Item 5 exact, and better than today on a foreign draw | Not exact, by this point's own finding: the foreign-context return (`vscreen.cpp:2101-2111`) and the skip returns come before the clears at `:2143-2144`, so today a foreign draw can consume a flag a blocked owner draw left (`:4308`), and a per-call value would change that. Better, perhaps, but a behaviour change; the rule for this arc is that nothing a feature decides changes. Item 5 moves to build 6 with #184, and item 6 now counts the case for all three flags. Its 0.02-0.04 ms comes out of the total. |
| 6 | Shadow pointer-only memos counting would-hit and would-disagree | Taken. |
| 7 | The cross-profile probe | Still the maintainer's question. Its call count is not measured; "about 17k a frame" is an estimate. |

### Agreement on the build 5 plan (2026-11-04, Antigravity)

Full consensus reached on Build 5 (`c86670bd`):

- **Dispositions 1–4 and 6:** Agreed across the board, including the
  line-number correction on item 3's second `viewIsSurface` loop
  (`src/d3d11/ui_depth.cpp:2419` in `uiDepthSampledSurfaceSlot`).
- **Disposition 5 (moving item 5 to Build 6 alongside #184 and counting all
  three TLS flags in item 6):** Agreed. Deferring the per-call move of
  `t_uiDepthThisDraw` and `t_compositeThisDraw` until item 6's per-draw
  sequence stamp measures whether any blocked owner draw at
  `src/d3d11/vscreen.cpp:4308` leaves a flag for a subsequent foreign/skip draw
  (`:2096-2118`) keeps Build 5 strictly behaviour-identical on every path.
- **One implementation detail to note for item 6's hologram shadow memo:** In
  `uiDepthHologramOnEyeDraw` (`src/d3d11/ui_depth.cpp:3614-3625`), the real hit
  at `:3620` returns early (`if (classifiermemo::hit(g_holoMemo, now)) return
  false;`) before `classifyHologramDraw` (`:3622`) and `classifiermemo::store`
  (`:3623`). That already matches item 6's specification (`:2270-2276`):
  because a pointer-only key changes strictly on a subset of the events that
  change `g_bindingEyeKeyGen`, every real `g_eyeMemo` / `g_holoMemo` hit is
  also a shadow hit, so evaluating `classifiermemo::hit` and
  `classifiermemo::store` on `g_eyeMemoPtrOnly` and `g_holoMemoPtrOnly` only
  inside the real miss path (plus the invalidations at `:4592` and
  `uiDepthConfigure` at `:3506-3507`) is exact and adds zero work on the 90%
  memo-hit fast path.

Build 5 is ready to implement and fly.

**Consensus noted (2026-11-04).** Agreed on every point. One anchor corrected
for the implementer: at `c86670bd` the eye and hologram memos are stored at
`ui_depth.cpp:2902` and `:3623` and invalidated only at `:4592-4593`;
`:3506-3507` is the HUD scene restore, not a memo site. The shadow memos mirror
those four lines, in the miss path only, which is exact for the reason Argon
gives: the pointer-only key moves on a subset of the events that move today's
key, so every real hit is also a shadow hit.

### Review of the build 5 design (Opus, 2026-11-04), and dispositions

**Changes required** (fresh context, against `c86670bd`; notes outside the
repo: `b5-opus-review.md`). The production changes in items 1-4, 6 and 7 hold
up as exact on every path traced, except one wrap-around gap in item 4. Four
test seams could not run as written, and the #184 counters could not tell
"never ran" from "never happened". Each finding was checked against the code;
all are taken. Where this table and the item text above differ, the table wins.

| # | Finding | Disposition |
|---|---|---|
| 1 | BLOCKING. Item 2's rig check moved "to a miss" still aborts: a miss runs `classifyEyeDraw`, whose first read is `bindingGet(Dsv0)` (`ui_depth.cpp:2647`), and `ui_depth_test.cpp:51-58` stubs it and its neighbours with `std::abort()` (checked). | Taken. Those stubs return rig values while a test flag is set and abort otherwise, as `hologram_depth_test.cpp:76` already does for `bindingGet`. The `g_holoDraw` copy is asserted on the miss (`ui_depth_test.cpp:219` moves). |
| 2 | BLOCKING. Item 3's fill and evict cases need a `g_viewMemo` miss, which calls the `bindingResolve` abort stub (`:55`); and `putSeq` must be read after the miss's own put (`ui_depth.cpp:869`), or a front entry filled on a miss never hits. | Taken, both: the same flagged fakes; the entry stores `g_viewMemoPuts` as it stands after the put. |
| 3 | BLOCKING. `engine_velocity_test` builds with `EDVR_BINDING_SHADOW_EXTERNAL` and its own fake slots (`lifecycle_tests.h:102-111`, checked), so it never runs the real bump; its tests bump `.gen` directly and expect the slow path (`skin_lifecycle_tests.h:673-689`). | Taken. In that rig the aggregate is the sum of the five fake generations, so every direct bump moves it. The real bump sites are tested in `scrim_metadata_test`, which links `binding_shadow.cpp`. |
| 4 | BLOCKING. The fence behind `vscreen.cpp:4308` rises only on a failed hologram restore (`ui_layer.cpp:1867`), so zeros prove nothing. The sequence stamp must be `thread_local` and bumped at `beginPanelOverride`'s entry, before the returns at `:2096`, `:2111`, `:2116`, `:2118`. `t_holoDepthThisDraw` is never cleared at `:2143`, so it can reach any later draw. | Taken. The line prints the totals beside the counts: blocked returns and `UiDepthScope` constructions. Zero blocked returns reads "undetermined", not "never". The stamp is `thread_local`, bumped first thing in `beginPanelOverride`. The consumed-stale count covers any later draw, foreign or owner. |
| 5 | Item 1's behavioural test cannot live in `ui_quality_test`, which does not link `vr_star_glow.cpp`; its text pins (`ui_vr_star_glow_test.h:526-542`, `584-615`, `711-724`; `ui_vr_hud_glow_test.h:308`) match today's call sites and will break. Export `g_t0.active` itself, not a mirror. | Taken: the pins are updated to the gated call, and the inline test reads `detail::g_t0` (its `active` field) and `detail::g_mode` directly; no new writer. |
| 6 | The S2 shadow's pointer-only bump lives in `bindingSet`, which `ui_depth_test` does not link; `bindingInvalidate` must always bump; both rigs that include `ui_depth.cpp` need the new reader defined. | Taken: tested in `scrim_metadata_test`; `bindingInvalidate`, `bindingSetShader`, forget-all and the frame boundary always bump it; both rigs define the reader. |
| 7 | Item 4 after a 32-bit wrap: an aggregate of 0 matches a reset cache. Keep `cache.vs/ps/rtv/dsv`, which are read elsewhere (`engine_velocity.cpp:1874`, `1961-1966`, `2114-2115`). Nine `DrawCache{}` resets, not seven (checked: nine). | Taken: the increment skips 0. The five per-slot fields stay in `DrawCache` and are still stored; only the compare changes. |
| 8 | Moving a cost inline makes "callee own down" pass by construction. | Taken: items 1-3 are judged as caller plus callee, and item 4 on the four thunks' own time, per full-path draw. |
| 9 | Item 7: the call count is already logged (`cross_profile_cost.h:51`, `psConstantBuffers=<calls>/<us>`); dropping the lock prefix keeps the store and the call, so "down by half" is optimistic. Needs a never-ran signature and a source pin. | Taken: "down"; never ran = the hook's own time unchanged; a source pin on the site's begin. |
| 10 | Never-ran signatures missing for item 4; item 3 and item 6 should print what they measure against. | Taken: the item 6 line also prints engine velocity's slow-half entries, `viewIsSurface` calls, and shadow lookups (which must equal the real misses). |
| 11 | Zero disagreements in one 100 s standing flight cannot settle the recycled-address case. | Taken: build 6 words it "none observed in N lookups", and S2's case rests on that plus the frame bound, stated as such. |
| 12 | The total's lower bound leans on items 3 and 7, the least supported. | Agreed: the 0.25-0.39 ms total is a guide; the per-item criteria decide. |

**Re-review (Opus, fresh context, against `c86670bd`): approved.** All twelve
findings resolved, each checked in the code. Its five non-blocking notes are
taken:
- A. Item 3's front entry reads `g_classifierEpoch` after `viewIsSurface`
  returns, as it reads `putSeq`: `surfaceMatches` can bump the epoch
  (`ui_depth.cpp:772`) before the put at `:869`.
- B. Item 1: `g_t0` and `g_mode` sit in an anonymous namespace
  (`vr_star_glow.cpp:45`, `:102-106`), and `FirstLevelBind` holds a `ComPtr`.
  The single storage of `active` moves out of the struct into
  `detail::g_vrStarT0Active`, and the mode becomes `detail::g_vrStarMode`
  (other files have their own `g_mode`); no new writer.
- C. Item 4's summed fake goes in `lifecycle_tests.h`, which `skin_engine_test`
  also includes (`build.bat:1521-1527`), and the aggregate's reader is declared
  in `binding_shadow.h`'s external-shadow branch (`:156-161`).
- D. For build 6: the F157 coat-pack skip (`vscreen.cpp:2135`) is another
  return before the flag clears; build 5's stamp, bumped at entry, covers it.
- E. Item 4's slow-half count is an equivalence check, not a never-ran
  signature: build 4 never logged it. The source pin and the two rigs show it
  ran.

Item 7 re-sized from build 4's own log: `psConstantBuffers=2598726/78.97` over
805 frames, about 3,230 PS constant-buffer binds a frame, so the 0.096 ms is
about 30 ns a call, of which the locked add is part. Expected 0.03-0.06 ms; the
build's total 0.22-0.36.

## Flight 2026-11-04 (build 5)

Build `85006708` (log `edvr_gfx_20261104_092014.log`, `--expect-build HEAD`
matches), the L2 spot, standing still, DLAA, one 100 s capture (20 windows of 5
s). Notes outside the repo: `profile-b5\cmp5.py`, `dig5.py`.

**Draw count.** One full on-foot log window, 09:23:05-09:24:06: 16,308
full-path draws a frame (build 4: 17,148). It spans profile windows 3-15, which
are the ones used per draw. Windows 16-20 have no closed log window; their own
time a frame (4.37-4.98 ms) is in the same range as 3-15's (4.37-5.10).

**The cut: passed, 0.34 ms a frame.** EDVR's own time in R1:

| | Build 4 (w7-20) | Build 5 (w3-15) |
|---|---|---|
| Own, ms a frame | 5.215 | 4.622 |
| Own per full-path draw | 304.1 ns | 283.4 ns |
| Without Map/Unmap own | 267.3 ns | 246.2 ns |
| At build 5's draw count (16,308), without Map/Unmap | | cut **0.34 ms** (0.36 at build 4's count) |

Expected 0.22-0.36 ms. Map and Unmap own fell 0.631 to 0.606 ms a frame.

**Per item**, ns per full-path draw (ms a frame at 16,308), each caller grouped
with its callees:
- **Item 1, star glow: passed.** `starGlowAfterDraw` 7.25 and
  `vrStarGlowAfterDraw` 2.38 to 0, 0.157 ms. The draw-thunk lambdas (renamed by
  the compiler, grouped by file) fell 59.1 to 51.4 ns, 0.125 ms, mostly the
  call they no longer make.
- **Item 2, `uiDepthOnEyeDraw` inline: no saving.** Build 4's cost in the
  callee sat on one line, `ui_depth.cpp:2895` (4.99 ns, the load of
  `g_uiDepthOn`); inlined, the same load lands in `beginPanelOverride`, which
  rose 35.9 to 42.0 ns. Callee, miss path and caller: 42.4 to 44.4 ns (+0.03
  ms).
- **Item 3, the `viewIsSurface` front cache: no saving.** It hit 64% of 10,312
  lookups a frame, yet `viewIsSurface` 6.43 became `viewIsSurfaceFront` 6.23,
  with its callers 9.56 to 9.81 ns. The cost is at the miss's memo lookup
  (`ui_depth.cpp:904`, 4.29 ns): 3,746 misses a frame at about 19 ns each,
  against about 10 ns a call before. The calls the cache took were the cheap,
  cache-hot repeats; the cost was the cold ones, which still miss.
- **Item 4, engine velocity: passed.** `engineVelocityBeforeDraw` 8.59 to 5.55
  ns (0.05 ms), its compare line 4.32 to 2.74.
- **Item 7, the plain count: passed, small.** `hookedPSSetConstantBuffers` 6.46
  to 5.65 ns (0.013 ms; expected 0.03-0.06).

**Nothing else rose more than 0.1 ms**, grouped as above. New in the profile: a
`std::unordered_map` lookup keyed on pixel shaders, 0.057 ms, mostly under
`hookedUnmap` through `uiDepthMotionResourceWrittenImpl`. It is an inlining
change, not new work: build 4's engine-velocity hash (0.058) left the profile,
and Map and Unmap's own time fell.

**Nothing changed on screen.**
- Composites left in the scene: 0 in every window.
- `ui depth totals:` wrote depth for 9.0 interface draws a frame (left alone
  36.9), outside build 4's 5.9-6.6. It is the scene, not the build: at this
  spot, standing still, every flight in the arc has ranged from 3.8 to 14.0 a
  frame (build 3: 6.0), and the share written (20%) is within build 4's 17-26%.
  Build 4's two windows set too narrow a range for the criterion; future
  flights judge against the arc's spread.
- `--health`: one NEVER APPLIED, prompt label draws (F87p). It is not new: all
  six earlier logs since `e1606a23` in which a prompt's focus key was set drew
  0 label groups; build 4's flight never set one.

**Item 6: the counts for build 6** (window ending 19800, a frame):
- **S2: cleared to build.** The eye memo missed 2,859 times a frame. The
  pointer-only key would have hit on 1,133 of those (40%), with 0
  disagreements; over the flight, 0 in 8,089,837 eye and 11,199 hologram shadow
  lookups. Shadow lookups equal real misses in every window. The hologram memo:
  3 misses a frame, 0 would-hits, so S2 does nothing there.
- **#184: undetermined**, as expected: 0 blocked owner returns in 29,354,697
  scope constructions.
- `viewIsSurface` calls 3,746, front-cache lookups 10,312 (hits 6,566), engine
  velocity's slow half 1,710.

**What items 2 and 3 say about build 6.** Both moved work without removing a
cold load, and the cold load was the cost. That is S1's premise: what an
unclaimed draw pays for is the cache lines it touches, not the instructions. S2
attacks the other half of item 3: it removes whole classifier runs, and with
them the cold `viewIsSurface` misses inside.

- ruled out: inlining `uiDepthOnEyeDraw`'s fast path as a cut, because its cost
  is the first load of the ui-depth state, which moved with it into
  `beginPanelOverride`.
- ruled out: a per-slot front cache for `viewIsSurface` as a cut, because it
  takes only the cache-hot repeat lookups; the cost is in the misses, which it
  cannot avoid.

## Build 6: one interest mask, S2 by its counts, #184, and the arc's temporary lines retired (designed 2026-11-04, draft)

The arc's last planned build. Its items wait on build 5's flight where marked;
the design is written now so the review can run in parallel. A code survey
(Opus, read-only, notes outside the repo: `b6-survey.md`) supplied the anchors,
at `c86670bd`; build 5 moves some of them, so the spec re-anchors.

**Evidence.** An unclaimed eye draw under DLAA reads 34 module-scope flag sites
across about 22 cache lines (one or two per module), plus about 7 fixed lines
(`g_state`, `DrawHot`, the binding shadow, the TLS block); build 4's own-time
lines in `beginPanelOverride` and `forwardWithVerdict` sit on those loads (the
0.02-0.06 ms lines in "Flight 2026-11-03 (build 4)").

### Changes

1. **S1: a write-through interest mask** (exact; keyless). A `uint64_t
   interest` at bytes 56-63 of `DrawHot` (`vscreen.cpp:415-432` uses 53 of 64).
   One bit per stored flag the ladder reads, about 31: the survey's sites 3-6,
   8-11, 12 (the flag, not its per-draw tail), 13-23, 25-27 and 29-33, each bit
   equal to the flag's whole expression (for example `g_uiDepthOn &&
   !g_uiDepthStoodDown`). Each ladder load becomes a bit test in place; no rung
   moves.
   - **Writers** (amended on review, R1): `drawInterestSet(bit, bool)` is
     inline in a new pure header, `draw_interest.h`, writing through `inline
     uint64_t* g_drawInterestPtr = nullptr`. `vscreen.cpp` points it at
     `g_state->interest` after `new State` (`:7331`) and clears it before
     the `State` is deleted, so it is null before install, in transport-only
     mode (`:6230`) and in every rig that compiles a module without
     `vscreen.cpp` (`ui_depth_test` includes `ui_depth.cpp` alone,
     `build.bat:2093`); there the call is a no-op, and a rig that tests a
     module's recompute points it at a local. Install recomputes every bit
     from the real accessors after `new State` and the configure sweep
     (`:7344-7367`).
   - **A bit mirrors stored state only** (R2). Where the ladder calls an
     accessor that does more than read a flag, the bit covers its first
     stored test and the rest stays behind the bit, unchanged. Site 6:
     `flatTemporalCapturing()` (`flat_temporal.h:34-37`) is a relaxed load of
     `g_flatTemporalCapturing`, then `flatTemporalCapturingSlow()` (the pause,
     `g_flatComputeInternal`, and the owner-thread test, which counts foreign
     calls). The bit is `g_flatTemporalCapturing`; `vscreen.cpp:2143` becomes
     `if ((interest & kBitFlatTemporal) && flatTemporalCapturingSlow())`, the
     same tests in the same order.
   - **The rule that makes it exact:** each module recomputes its bits from
     its real expressions at the end of every function that writes one of
     its flags. That covers the writes through aliases and reference
     parameters (`particle_fix.cpp:115`, `:883`; `depth_probe.cpp:100`;
     `native_on_foot.cpp:305`). The mid-draw writers recompute right after
     the write: `ui_depth.cpp:3488`, `panel_curve.cpp:277`, `:462`, `:492`,
     `quad_probe.cpp:469`, `ui_layer.cpp:1867`, `:2411`, `:5376`, `:5383`,
     `draw_census.cpp:775`.
   - **Why plain fields suffice:** every writer of a mirrored flag runs on
     the render thread (Present, the `tk*` ticks, the config refresh, menu
     actions, mid-draw; `device_hook.cpp:1571-1660`, `:1707`, `:2024`), and
     the ladder reads on the render thread, so program order gives `bit ==
     expression` at every read. The review should test this claim writer by
     writer; a writer found off the render thread stays unmirrored.
   - **Not mirrored:** `drawGateWanted` (its contract allows arming from any
     thread, `draw_gate.h:44-46`; one load, on the line it shares with
     `g_aaOffDraw`); `introPanelWants`, `introCurveWants` and
     `sunglareWantsDraws` (computed, not stored, and behind shape tests); the
     depth probe's per-draw tail; the per-draw outputs of `g_uiDepthDraw`.
     Also, named so no implementer adds them (R3): `fssResActive()`
     (`vscreen.cpp:2379`, `:4774`; `g_fssResCount` moves on the streaming
     threads, `fss_res.h:77-78`) and `nativeOnFootBodyCullWanted()`
     (`:5550`; `g_nativeOnFootCamExternal` is written on the game's update
     thread, `native_on_foot.h:123-148`). Neither is on an unclaimed eye
     draw's path: the first is behind `!s->rtv0Eye` or the FSS verdict, the
     second in the instanced thunk after the verdict. Only render-thread
     writers are mirrored, so the mask stays a plain field.
   - **Gated by an invariant:** `g_rollFreePending` is set only behind
     `particleSteady()` (`particle_fix.cpp:672`, `:681`, reached from
     `vscreen.cpp:2218`) and cleared on leaving steady (`:950`);
     `g_rollFreeEngaged` is set only when pending (`:697`, `:724`). So the
     clear at `vscreen.cpp:2004` and the tests at `:4346`, `:4348` are
     skipped when the steady bit is clear.
   - **Drift check, TEMPORARY:** in `bindingAudit`'s 1-in-1024 slot
     (`:2196`), recompute the mask and log once on a mismatch, naming the
     bit. Zero mismatches over a flight is part of the pass.
   - Expected: about 29 lines touched per unclaimed eye draw down to about
     7. Ms: 0.05-0.15 (S1's earlier bound was 0.5; the flight decides).
2. **S2, if build 5's counts allow it** (exact only on the flight's evidence).
   In `bindingSet` (`binding_shadow.cpp:112-118`), call `noteEyeKeySlot` for
   PsSrv0-3 only when `s.ptr != ptr`; per-slot `gen` still moves on every set,
   so `srv0IsPanelSized` (`vscreen.cpp:1450`), the scrim cache
   (`scrim_fix.h:108`, `scrim_fix.cpp:114`) and engine velocity's PsSrv3
   compare are unchanged. The key's readers are the eye memo
   (`ui_depth.cpp:2898`) and the hologram memo (`:3619`); the hologram
   classifier reads no PS SRV (`:3579-3612`), so S2 cannot change its answers.
   The eye classifier's plain decline loses one input on a same-pointer re-set:
   `viewIsSurface` of the re-set view. The exposure is a view freed after a
   silent D3D unbind and re-created at the same address over an already-learned
   surface, with its view-memo entry evicted earlier in the same frame; the
   per-frame bumps of the key (`binding_shadow.cpp:157`) and the classifier
   epoch (`ui_depth.cpp:4363`) end it at the frame. **Gate:**
   `ptr_would_disagree` is 0 in build 5's flight, and `ptr_would_hit` is large
   enough to matter (each hit saves one classifier run, up to four
   `viewIsSurface` calls). Otherwise S2 is ruled out. Rig:
   `scrim_metadata_test.cpp:121-123` asserts that a same-pointer re-set moves
   the key for all eight slots; it splits, PsSrv0-3 asserting the key stays and
   the per-slot `gen` moves.
3. **#184: the per-draw flags become the draw's own** (a BEHAVIOUR CHANGE; the
   maintainer said yes on 2026-11-04, the two pending flags below included).
   Today: an owner draw blocked at `vscreen.cpp:4308` leaves its flags in TLS.
   The fence that blocks it is raised only on a failed hologram-binding restore
   (`ui_layer.cpp:1865-1867`) and holds to the frame boundary (`:6792`), so
   every owner draw for the rest of that frame does the same. The next draws
   then consume them: (1) a foreign-context draw on the render thread passes
   `:4308` and runs the UI-depth and hologram re-issue Begins on the foreign
   context, reading the mode the blocked draw left (`:4675`, `:4692`; the
   hazard the comment at `:4662-4667` says the TLS exists to prevent); (2) the
   first non-eye owner draw after the fence clears re-issues into the hologram
   scratch targets, gated only on `g_holoEye >= 0` (`ui_depth.cpp:3638`,
   `:3755`); (3) a kSkip from `:2096-2135` (the F157 coat-pack skip at `:2135`
   among them) offers stale flags to the blend probe. The fix: `struct
   DrawCallFlags { bool uiDepth, holoDepth, composite; }` filled by
   `beginPanelOverride` (all false on every early return by construction;
   `:2536-2550` write it; `:2143-2144` go) and passed to `forwardWithVerdict`
   at the four thunks' call pairs (`:5277/5291`, `:5360/5373`, `:5440/5462`,
   `:5526/5554`, the only callers); the scope builds from it and the TLS goes.
   Plus `if (flags.uiDepth) uiDepthEnd(self);` on the `:4308` return, so the
   mode does not outlive it, and (R4) the same return clears
   `g_uiDepthDraw`'s `planetPending`, `planetSolarPending` and
   `worldScreenPending`, so the module's accessors agree with the flags after
   a blocked draw. Their only readers today are `:4720` and `:4726`, which
   move to the flags, so this changes nothing a draw does; it keeps a later
   reader of the accessors from seeing a blocked draw's state. This also removes build 5's deferred item 5 cost
   (about 11 TLS accesses a draw, 0.02-0.04 ms). Build 5's counter will most
   likely read 0 (the fence is a fault path); the case rests on the code, which
   is why it is put to the maintainer rather than gated on the count.
   - **Included (maintainer, 2026-11-04):** `planetPending` and
     `worldScreenPending` (`ui_depth.cpp:2675`, `:2688`) leak past the same
     returns (consumed only at `vscreen.cpp:4703`, `:4709`, reset by the next
     eye draw), so `DrawCallFlags` carries them too, false on every early
     return. Fixing the three and not these would leave the same hazard
     half open.
4. **The arc's temporary lines retired.**
   - Build 4's `binding re-sets:` line: already removed in build 5.
   - Build 5's item 6 line (the #184 and S2 counts) and item 1's drift check
     above, after build 6's flight.
   - **Kept (maintainer, 2026-11-04):** `draw mask:` (`draw_mask_line.h`)
     stays a standing line. It is the log's only full-path draw count, the
     denominator of every per-draw comparison, and its skip counts show the
     VS-bit gates working; in build 5's profile its call-site lines total
     under 0.02 ms a frame (the draw count 0.002), gate tests included.
5. **Build 5's items 2 and 3 removed** (maintainer, 2026-11-04; exact both
   ways). Each saved nothing ("Flight 2026-11-04 (build 5)"), so the code goes:
   - Item 2: `uiDepthOnEyeDraw` back out of line in `ui_depth.cpp`, in build
     4's form (`c86670bd`, `:2890-2904`): the reset, `g_holoDraw = draw`, the
     gate, then the memo. `g_uiDepthDrawDirty` and its sets go. S2's key
     change and build 6's other edits to the function stay.
   - Item 3: `ViewFront`, `g_viewFront`, `viewIsSurfaceFront` and
     `g_viewMemoPuts` (with its bumps) go; the two slot loops and the
     `ui_layer.cpp` log caller call `viewIsSurface` again.
   - Their rig cases go with them; build 4's `ui_depth_test` checks come
     back where they still apply.

### Left out

- Star glow's decline with star glow on; B1 (the 512x512 pass); the `mapMemo`
  size (17% misses, its cost not measured).
- The blend probe, `gpu_pass_profile` and the F44/F45/F47/F18 instruments:
  TEMPORARY, but other arcs'.

### Test seams

- Item 1: a pure header holding the bit layout and an `interestFrom(...)`
  function, tested in `tools\aa_off_test` against each accessor; a source-text
  check in the `gate_test.cpp:803` style that every mirrored flag's writer
  function calls its module's recompute; the drift check in flight.
- Item 2: the split `scrim_metadata_test` cases; `ui_depth_test`: a
  same-pointer re-set keeps the eye memo valid, a pointer change does not.
- Item 3: the struct and a pure "which re-issues for these flags and this
  verdict" function in a header (the `draw_fast_path.h` pattern), tested in
  `tools\aa_off_test`; a source-text check that no `thread_local bool
  t_*ThisDraw` remains and all four call pairs pass the flags;
  `hologram_depth_test`: Begin declines after `uiDepthHologramSkippedDraw()`;
  `ui_depth_test`: after `uiDepthEnd`, `uiDepthWantsReissue()` is false.
- Item 3 also: `DrawCallFlags` carries `planetPending` and
  `worldScreenPending`; the source-text check covers them.
- Item 4: `edvr_log.py --self-test` without the retired lines; the build's
  gates.
- Item 5: the removed symbols absent (a source-text check); `ui_depth_test`
  over hit, miss, hit, reissue gives build 4's accessor values.

### Flight and pass criteria

The L2 spot, standing still, DLAA, one 100 s capture.
- `beginPanelOverride` and `forwardWithVerdict` own time per full-path draw
  down; the drift check logs no mismatch.
- With S2: eye-memo misses per eye draw down by about build 5's `ptr_would_hit`
  share; `ui depth totals:` and the composites line within build 5's range.
- Nothing else up more than 0.1 ms; `--health` with no new NEVER APPLIED.
- If the code never ran: no drift line ever and no change in the two functions'
  own time (item 1); eye-memo misses unchanged (item 2); a `thread_local` flag
  still in the binary's symbols (item 3).

### Review of the build 6 plan (2026-11-04, Antigravity)

Reviewed against commit `85006708` (`PR #187`).

#### 1. Build 5 flight analysis and conclusions (`:2644-2729`)

Agree with the Build 5 attribution and conclusions:
- **What paid:** Item 1 (`starGlowOnDraw` gated inline on `starGlowActive()`,
  `vscreen.cpp:2591`, `-0.157 ms/frame`), Item 4 (one engine-velocity
  generation `bindingEngineVelGeneration()`, `binding_shadow.h:76-78`, `-0.05
  ms/frame`), and Item 7 (`psCbProbeCount` replacing the popcount loop,
  `ui_depth.cpp:3297`, `-0.013 ms/frame`) together lowered R1 own time per
  full-path draw from `304.1 ns` to `283.4 ns` (`-20.7 ns/draw`, `-0.34
  ms/frame` at 16,308 draws without `Map`/`Unmap`).
- **What did not pay (and why):** Item 2 (`uiDepthOnEyeDraw` inline in
  `ui_depth.h:161-171`) and Item 3 (`viewIsSurfaceFront` in
  `ui_depth.cpp:898-915`) confirmed the cache-line rule: moving instructions
  into `beginPanelOverride` without removing the first cold touch into a
  module's data line (`detail::g_uiDepthOn` for Item 2; `g_viewMemo` on probe
  misses for Item 3) shifts sample attribution without reducing total own time.
  Removing both in Build 6 Item 5 (`:2847-2858`) is the right call.
- **S2 cleared:** `1,133 / 2,859` eye-memo misses per frame (`39.6%`) were
  same-pointer SRV re-sets (`ptr_would_hit = 3,205,721` across 2,830 frames)
  with **0 disagreements** across `8,089,837` eye and `11,199` hologram shadow
  lookups (`:2696-2700`).

#### 2. Item 1 (`DrawHot::interest` write-through mask) — writer-thread audit and test-rig linkage

The design asks (`:2765-2770`) to verify writer by writer that every mirrored
flag is written only on the render thread, and to leave any off-render-thread
writer unmirrored. Auditing all candidate gates in `beginPanelOverride`,
`forwardWithVerdict`, and the 4 draw hooks surfaced **two off-render-thread
writers**, **one composite accessor (`flatTemporalCapturing`)**, and **one
test-rig linker seam**:

1. **Catch 1A (off-render-thread writer) — `fssResActive()` (`vscreen.cpp:2379,
   4774`):**
   - `fssResActive()` (`src\d3d11\fss_res.h:101-104`) reads
     `detail::g_fssResCount != 0`.
   - `detail::g_fssResCount` is incremented in `fssResNoteCreated`
     (`src\d3d11\fss_res.cpp:101-110`), called from `hookedCreateTexture2D`
     (`src\d3d11\device_hook.cpp`), and decremented in `fssResNoteDestroyed`
     (`fss_res.cpp:113-124`). As `fss_res.h:77-78` documents:
     *"CreateTexture2D runs on the game's streaming threads."*
   - If `fssResNoteCreated` on a streaming thread did a non-atomic
     read-modify-write on `g_state->interest` while the render thread was
     updating `g_state->interest` mid-draw (e.g. `uiDepthWorldScreenBegin` at
     `ui_depth.cpp:3488` or `uiLayerBeginCapture` at `ui_layer.cpp`), the two
     RMWs would race and could drop a bit (or trip the 1-in-1024 drift check
     at `:2782`).
   - **Fix:** Either:
     - *(a)* Leave `fssResActive()` unmirrored at `vscreen.cpp:2379` (it is
       already nested inside `if (!s->rtv0Eye)` at `vscreen.cpp:2373`, which
       runs only on the ~676 offscreen draws/frame, never on the ~16,000 eye
       draws) and `:4774` (reached only when `verdict == kFssTarget`), **or**
     - *(b)* Store `DrawHot::interest` as `std::atomic<uint64_t>` updated via
       `fetch_or` / `fetch_and` (`std::memory_order_relaxed`) in
       `drawInterestSet`, and read via
       `s->interest.load(std::memory_order_relaxed)` once at the top of the
       draw. On x86-64, a `memory_order_relaxed` load compiles to the exact
       same single `mov` instruction as a plain `uint64_t` read, while
       `lock or` / `lock and` on the cold writer path makes `drawInterestSet`
       race-free even if called from a streaming/update thread. (Even with
       *(b)*, leaving `fssResActive()` out of the drift check or out of the
       mask avoids a benign TOCTOU between `s->interest` loaded at draw entry
       and `fssResNoteCreated` firing on a streaming thread mid-draw.)
2. **Catch 1B (off-render-thread writer, if included) —
   `nativeOnFootBodyCullWanted()` (`vscreen.cpp:5550`):**
   - In `hookedDrawIndexedInstanced` (`vscreen.cpp:5550`),
     `nativeOnFootBodyCullWanted()` (`src\d3d11\native_on_foot.h:329-332`)
     evaluates:
     `g_nativeOnFootBodyCull.load(relaxed) != 0 && nativeOnFootActive() && !nativeOnFootCameraExternal()`.
   - While `g_nativeOnFootBodyCull` (`native_on_foot.cpp:35428`) and
     `g_nativeOnFootState` (`native_on_foot.cpp:35334, 36746, 36846`) are
     written on the render thread, `g_nativeOnFootCamExternal` is written in
     `nativeOnFootCameraPublish` (`native_on_foot.h:138-148`) from
     `controllerHooked -> nativeCamAfterUpdate`
     (`src\d3d11\native_on_foot_hooks.cpp:365-388`), which runs **on the
     game's update thread** (`native_on_foot.h:123-124, 136`).
   - **Fix:** Keep `nativeOnFootBodyCullWanted()` at `vscreen.cpp:5550` out of
     the mirrored bitmask (or mirror only the render-thread terms
     `g_nativeOnFootBodyCull != 0 && nativeOnFootActive()`, leaving
     `!nativeOnFootCameraExternal()` behind the bit).
3. **Catch 1C (composite accessor) — `flatTemporalCapturing()`
   (`vscreen.cpp:2143`):**
   - `flatTemporalCapturing()` (`src\d3d11\flat_temporal.h:26-37`) is not a
     pure flag read: it checks
     `detail::g_flatTemporalCapturing && !detail::g_flatTemporalPaused && !g_flatComputeInternal && flatObserveAsOwner(...)`,
     where `flatObserveAsOwner` calls `GetCurrentThreadId()` and increments
     `g_flatTemporalForeignCalls` when called off the owner thread.
   - **Fix:** Mirror only the stored state
     (`detail::g_flatTemporalCapturing && !detail::g_flatTemporalPaused`, which
     is always `false` in VR) into `kBitFlatTemporal`, gate `vscreen.cpp:2143`
     as `if ((interest & kBitFlatTemporal) && flatTemporalCapturing())`, and
     have `interestFrom(...)` in the test/drift header compute `kBitFlatTemporal`
     from `detail::g_flatTemporalCapturing && !detail::g_flatTemporalPaused`
     rather than calling `flatTemporalCapturing()`.
4. **Catch 1D (test-rig linker seam) — `drawInterestSet` must be header-inline,
   not an out-of-line symbol in `vscreen.cpp`:**
   - The draft states (`:2751-2753`): *"A new header declares the bit enum and
     `void drawInterestSet(uint64_t bit, bool on) noexcept`; `vscreen.cpp`
     implements it on `g_state->interest` (null-safe before install)."*
   - Over a dozen unit-test rigs in `build.bat` compile module `.cpp` files
     **without** `vscreen.cpp` — for example `tools\ui_depth_test` and
     `tools\hologram_depth_test` compile `src\d3d11\ui_depth.cpp` alone;
     `tools\screen_motion_test` compiles `src\d3d11\screen_motion.cpp` alone;
     `tools\particle_capture_test` compiles `src\d3d11\particle_fix.cpp` alone;
     `tools\ui_quality_test` compiles `src\d3d11\ui_layer.cpp` alone.
   - If `ui_depth.cpp`, `screen_motion.cpp`, `particle_fix.cpp`,
     `ui_layer.cpp`,
     etc. call an out-of-line `drawInterestSet` defined only in `vscreen.cpp`,
     every one of those test rigs will fail to link (`LNK2019: unresolved
     external symbol drawInterestSet`).
   - **Fix:** Define `drawInterestSet` as `inline` in the new pure header
     (`draw_interest.h`), writing through a C++17 `inline` pointer (or
     `inline uint64_t g_drawInterest = 0;` mirrored into `g_state->interest` /
     pointed to by `inline uint64_t* g_drawInterestPtr = nullptr;` that
     `vscreen.cpp` sets to `&g_state->interest` at `new State()` in
     `vscreen.cpp:7349` and clears before `delete s` in `vscreen.cpp:7406`).
     Then `drawInterestSet` is a zero-dependency inline write in every module
     and a no-op (`g_drawInterestPtr == nullptr`) in standalone test rigs
     unless a rig points `g_drawInterestPtr` at a local `uint64_t` to test it!
5. **Verified render-thread-only writers and the `particleSteady()` invariant
   (`:2776-2781`):**
   - Verified `particle_fix.cpp:672, 681, 690, 697, 724, 739-743, 950`:
     `g_rollFreePending` is only set inside `particleOnDraw` (`:672, :681`),
     which is only called when `particleSteady()` is true (`vscreen.cpp:2223`),
     and is cleared when leaving `kSteady` (`:950`). `g_rollFreeEngaged` is
     only set inside `particleRollFreeBegin` (`:697, :724`, gated on
     `detail::g_particleMode == Mode::kSteady` at `:690`) and cleared in
     `particleRollFreeEnd` (`:740`) at the end of the same draw
     (`~RollFreeScope` at `vscreen.cpp:4363`). Because `particleConfigure`
     only runs between draws, gating `:2009`, `:4363`, and `:4365` on the
     single `particleSteady` bit is exact.
   - Verified that all other candidate writers run on the render thread:
     `ui_depth` (`ui_depth.cpp:3245, 3260, 3488, 3500`), `ui_layer`
     (`ui_layer.cpp`), `depth_probe` (`depth_probe.cpp`), `panel_curve`
     (`panel_curve.cpp`), `quad_probe` (`quad_probe.cpp`), `draw_census`
     (`draw_census.cpp`), `object_probe` (`object_probe.cpp`), `remlok_fix`
     (`remlok_fix.cpp`), `holo_fix` (`holo_fix.cpp`), `target_sharp`
     (`target_sharp.cpp`), `scrim_fix` (`scrim_fix.cpp`), `backdrop_fix`
     (`backdrop_fix.cpp`), `fss_panel` (`fss_panel.cpp`), `wake_pulse`
     (`wake_pulse.cpp`), `loader_panel` (`loader_panel.cpp`),
     `celestial_motion` (`celestial_motion.cpp`), `blend_probe`
     (`blend_probe.cpp`), `screen_motion` (`screen_motion.cpp`),
     `vr_camera_census` (`vr_camera_census.cpp`), `vscreen_footprint`
     (`vscreen_footprint.cpp`), `optimized_station` (`optimized_station.cpp`),
     `desktop_window` (`desktop_window.cpp`), and `native_on_foot`
     (`native_on_foot.cpp:35334, 35412, 35417, 36746, 36846` — excluding
     `g_nativeOnFootCamExternal` in Catch 1B).

#### 3. Item 2 (S2 on `g_bindingEyeKeyGen`)

Verified against `src\d3d11\binding_shadow.cpp:48-55, 127-134` and
`tools\scrim_metadata_test\scrim_metadata_test.cpp:121-123, 154-200`:
- In `bindingSet` (`binding_shadow.cpp:127-134`), `const bool ptrMoved = (s.ptr
  != ptr);` is computed before `s.ptr = ptr; ++s.gen;`, so per-slot `s.gen`
  still advances on every bind call while `noteEyeKeySlot(slot, ptrMoved)`
  skips bumping `g_bindingEyeKeyGen` when `slot` is `PsSrv0..3` and
  `!ptrMoved`.
- **Note for `ui_depth_test.cpp` when executing Item 5 (`:2857-2858`):** When
  removing Build 5 Item 2/3's rig cases from
  `tools\ui_depth_test\ui_depth_test.cpp:212-405`, keep the `rigfake` state in
  `ui_depth_test.cpp:51-73` (`rigfake::on`, `rigfake::eyeKeyGen`, etc.) so Item
  2's new `ui_depth_test` seam (`:2874-2875`, testing same-pointer SRV re-set
  vs pointer change on `g_eyeMemo`) can run `uiDepthOnEyeDraw` misses without
  hitting the `std::abort()` stubs.

#### 4. Item 3 (`#184` `DrawCallFlags` with `planetPending` and `worldScreenPending`)

Verified against `vscreen.cpp:2540-2559, 4307-4745` and
`ui_depth.cpp:2715-2731, 3144-3152, 3420-3495`:
- Folding `planetPending` (`vscreen.cpp:4720`) and `worldScreenPending`
  (`vscreen.cpp:4726`) into `DrawCallFlags` alongside `uiDepth`, `holoDepth`,
  and `composite` closes the exact same stale-flag window on early returns,
  because `classifyEyeDraw` (`ui_depth.cpp:2716-2730`) returns `false` when it
  sets `planetPending` or `worldScreenPending`, meaning `flags.uiDepth` is
  `false` on those draws and `uiDepthEnd` (`ui_depth.cpp:3144-3152`) never
  cleared them anyway.
- **One detail on `uiDepthPlanetBegin` (`ui_depth.cpp:3421`) and the `:4313`
  blocked return (`:2826-2827`):**
  - Inside `uiDepthPlanetBegin` (`ui_depth.cpp:3421`), it reads
    `detail::g_uiDepthDraw.planetSolarPending`. Because `flags.planetPending`
    is local to the `DrawCallFlags` of the exact eye draw whose
    `uiDepthOnEyeDraw` just set `g_uiDepthDraw.planetPending` and
    `g_uiDepthDraw.planetSolarPending`, `planetSolarPending` is always fresh
    when `flags.planetPending` gates `uiDepthPlanetBegin(self)` at `:4720`.
  - On the `:4313` (`:4308` in Build 4) `uiLayerIssueBlocked()` return in
    `forwardWithVerdict`, in addition to `if (flags.uiDepth) uiDepthEnd(self);`,
    having `uiDepthEnd(self)` (or that return path) also clear
    `detail::g_uiDepthDraw.planetPending`, `planetSolarPending`, and
    `worldScreenPending` ensures the module accessors `uiDepthPlanetPending()`
    and `uiDepthWorldScreenPending()` also read `false` immediately after a
    blocked draw, matching `uiDepthWantsReissue()`.

#### 5. Items 4 and 5 (retiring temporary lines and removing Build 5's Items 2 and 3)

Agree in full. Keeping `draw mask:` (`src\d3d11\draw_mask_line.h`) as a
standing log line (`:2842-2846`) is well-justified at `< 0.02 ms/frame` total
and gives every future log the full-path draw denominator and VS-bit skip
counts.

### Reply to the build 6 review (2026-11-04)

Each point was checked against the code at HEAD (the source is `85006708`'s;
only docs moved since). Four are taken and folded into the changes above as
R1-R4. Where this reply and the changes differ, the changes win.

| # | Point | Disposition |
|---|---|---|
| R1 | 1D: an out-of-line `drawInterestSet` in `vscreen.cpp` breaks every rig that compiles a module alone. | Taken. Checked: `ui_depth_test` builds from `ui_depth_test.cpp`, which includes `ui_depth.cpp`, plus two timing files, no `vscreen.cpp` (`build.bat:2085-2095`). The writer is inline in `draw_interest.h`, through a pointer that is null until install and in rigs. |
| R2 | 1C: `flatTemporalCapturing()` is not a plain flag read. | Taken, narrower than proposed. The bit is `g_flatTemporalCapturing` alone; the pause stays in `flatTemporalCapturingSlow()` with the other tests, so its writer (`flat_temporal.cpp:932`) needs no audit and the order is unchanged. The general rule is written down: a bit mirrors stored state only. |
| R3 | 1A, 1B: `fssResActive()` and `nativeOnFootBodyCullWanted()` have off-thread writers. | Agreed they must not be mirrored; neither was on the list (the survey's sites 3-33 are an unclaimed eye draw's reads, and these sit behind `!s->rtv0Eye`, the FSS verdict, or in the instanced thunk). Both are now named under "Not mirrored". Option (b), an atomic mask, is not taken: with no off-thread writer mirrored, a plain field is exact, and the drift check would catch a drift. |
| R4 | 3: clear the pending flags on the blocked return too. | Taken. A fault-path store of three bools; it changes nothing a draw does, since the accessors' only readers move to `DrawCallFlags`. |
| - | 2.5: the render-thread audit of the other writers and the `particleSteady()` invariant. | Thanks; it is the check the draft asked for. |
| - | 3: keep `rigfake` when build 5's item 2 and 3 cases go. | Agreed: S2's `ui_depth_test` case runs eye-memo misses and needs it. Only the front-cache and inline-only cases go. |

**Corrections to section 1**, so the record is right:
- **Wrong names:** the functions it names do not exist; `starGlowOnDraw`, `starGlowActive()`, `bindingEngineVelGeneration()` and `psCbProbeCount` match nothing in `src`. Build 5 had:
  - `vrStarGlowAfterMayAct()` gating `starGlowAfterDraw` at the four thunks;
  - `bindingEvKeyGeneration()`;
  - a plain count in `cross_profile_cost.h` at the PS constant-buffer site, in place of a locked add (not a popcount loop).
- **The would-hit total:** `eye_ptr_would_hit` over the flight is 3,035,059 (79,803 + 58,883 + 856,856 + 2,039,517), not 3,205,721. It is a sum of four log windows, not of 2,830 frames.
- The 1,133 a frame (40%) is from the one full on-foot window and stands.

**Consensus:** on this reply, Argon is asked to confirm or object. With its agreement, build 6 goes to a fresh Opus review.

### Consensus on the build 6 plan (2026-11-04, Antigravity)

Confirmed. All four dispositions (`R1`-`R4`) and the section 1 symbol/window-sum
corrections are verified against `85006708` and folded cleanly into the spec:
- **R1 (`draw_interest.h` inline writer through `g_drawInterestPtr`):**
  Verified against `build.bat:2085-2095` and `vscreen.cpp:6230, 7331-7367,
  7406`. Keeping `drawInterestSet(bit, bool)` inline in `draw_interest.h`
  through `inline uint64_t* g_drawInterestPtr = nullptr` avoids `LNK2019`
  across every standalone module rig and stays a null-safe no-op before install
  and in transport-only mode.
- **R2 (`flatTemporalCapturing` bit covers `g_flatTemporalCapturing` alone):**
  Verified at `src\d3d11\flat_temporal.h:26-37` and `vscreen.cpp:2143`. Gating
  `flatTemporalCapturingSlow()` behind `interest & kBitFlatTemporal` preserves
  the exact evaluation order of `g_flatTemporalPaused`, `g_flatComputeInternal`,
  and `flatObserveAsOwner` without needing to mirror `g_flatTemporalPaused`.
- **R3 (`fssResActive()` and `nativeOnFootBodyCullWanted()` explicitly excluded;
  plain `uint64_t interest` retained):** Agreed. With both off-render-thread
  writers documented under "Not mirrored" (`:2789-2796`) and every mirrored bit
  written exclusively on the render thread, a plain `uint64_t` in `DrawHot` is
  exact and race-free.
- **R4 (`planetPending`, `planetSolarPending`, and `worldScreenPending` cleared
  on the `:4313` blocked return alongside `uiDepthEnd(self)`):** Agreed
  (`:2847-2853`).
- **Section 1 corrections acknowledged:** Verified at `vscreen.cpp:5296, 5379,
  5467, 5559` (`vrStarGlowAfterMayAct()` gating `starGlowAfterDraw`),
  `binding_shadow.h:76-78` (`bindingEvKeyGeneration()`),
  `cross_profile_cost.h:84-93` (plain `++g_crossProfileCost.psConstantBuffers`
  in place of `InterlockedIncrement64`), and the four `build 5 checks:` log
  windows (`79,803 + 58,883 + 856,856 + 2,039,517 = 3,035,059`).

No remaining objections — we have full consensus on the Build 6 plan.

### Review of the build 6 design (Opus, 2026-11-04), and dispositions

**Changes required** (fresh context, against `85006708`; notes outside the
repo: `b6-opus-review.md`). Four blocking points, each a spec edit; nine
others. All are taken. B2 and N7 were checked here: `State` is deleted at
`vscreen.cpp:7405` and `:7550`, and the world-screen re-issue at `:4726` is
skipped when `layered`, leaving the flag set. Where this table and the item
text above differ, the table wins.

| # | Finding | Disposition |
|---|---|---|
| B1 | BLOCKING. The fence `g_uiLayerIssueBlocked` rises mid-draw (`ui_layer.cpp:1867`) and `forwardWithVerdict` reads it again later in the same call (`:4405`, `:4667`). A mask copied into a local at the draw's top would issue after the fence. | Taken. Every site reads `g_state->interest` where it stands; no local copy, no load hoisted across a call. R2's `interest &` is shorthand for that read. |
| B2 | BLOCKING. `State` is deleted at `:7405` and `:7550`; R1 named one. | Taken. `g_drawInterestPtr` is cleared before both deletes. |
| B3 | BLOCKING. "No drift line" is what success and a dead check both look like; the two functions' own time falls from items 3 and 5 alone. | Taken. The drift check prints a positive line every window: checks run, mismatches, the mask's popcount. Item 1 is judged with callers grouped with callees, net of items 3 and 5: their expected deltas are build 5's inline cost (35.9 to 42.0 ns in `beginPanelOverride`) and about 11 TLS accesses. |
| B4 | BLOCKING. S2 is not exact by construction (the doc says so); only items 3 and 5 had a yes. | **The maintainer said yes on 2026-11-04.** S2 is an approved behaviour change resting on build 5's 0 disagreements in 8.1M lookups and the frame bound. The reviewer's note goes in the record: `Rtv0` and `Dsv0` are eye-key slots (`binding_shadow.cpp:34-36`, `:130`), so a view unbound by binding its resource as a target still moves the key, which narrows the window. |
| N1 | Item 3 deletes what build 5's counts line reads (`:4315-4318`, `:4339-4343`, `:1639-1640`, `:2002`, `:2544`, `:2558`). | Taken. Build 5's line goes in build 6; a TEMPORARY "build 6 counts:" line replaces it, carrying eye and hologram memo misses (counted in the reverted miss path) and B3's drift figures. The never-ran signature names the three flags; `t_drawClockOrdinal` (`:5166`) and `t_markerTeeClockOrdinal` (`:4855`) stay. |
| N2 | Rigs that pin the old forms: `sharp_weapons_test.cpp:571-572`, `ui_composite_census_test.cpp:630`, `:664`, `:729-756`, `ui_depth_test.cpp:222-279`, `:391`, `:429`, `hologram_depth_test.cpp:79`. | Taken: listed in the spec's test seams. |
| N3 | The ladder is not render-thread only: `particleRollFreeClear` (`:2009`) runs before the foreign return (`:2107`), and `~RollFreeScope` (`:4363`) runs for foreign contexts (`:413`). | Taken as wording. The `particleSteady` bit's read on a foreign thread races exactly as the flag's read does today (`particle_fix.h:135`); nothing changes. The doc says so. |
| N4 | The rule must be "recompute before the next D3D call or return": `depth_probe.cpp:1447-1482` writes `g_wanted` and makes hooked calls before restoring it. Missing mid-draw writers: `native_on_foot.cpp:30307`, `:30355`. The source check must know the alias writers. Stale anchor: the stood-down write is `ui_depth.cpp:3534`. | Taken, all. |
| N5 | Site 5 includes the per-draw `countsMatch` test (`:2129`). | Taken: like site 12, the bit is the stored part (`SpawnLive \|\| CoatPackLive`); the per-draw test stays behind it. |
| N6 | A bit saves a line only while its feature is off; `particle_billboard` defaults to steady (`particle_fix.cpp:923`), and ui depth and hologram depth are on at L2. | Taken. The spec counts the bits clear at L2 and re-derives the estimate; 0.05-0.15 ms is a guide, the per-function criteria decide. |
| N7 | R4 covers only the `:4313` return; the pending flags also survive `:4667`, the kSkip return at `:4374`, and `layered` at `:4726` (a normal path, not a fault). | Taken, within the approved change: every return of `forwardWithVerdict` after the flags are filled leaves the module's pending flags clear, and the `layered` case is listed as a real leak this closes. |
| N8 | Stale anchors: the thunk call pairs are `:5294/5308`, `:5377/5390`, `:5457/5479`, `:5543/5571`; item 5's "`ui_layer.cpp` log caller" does not exist, the callers are `ui_depth.cpp:2459`, `:2697`. | Taken. |
| N9 | "Within build 5's range" contradicts build 5's lesson; `interestFrom` cannot be tested against accessors in a pure-header rig; the 1-in-1024 check misses fault-path writers. | Taken. On-screen checks use the arc's spread (`ui depth totals:` 3.8-14.0 a frame), not one build's windows. Each module's rig asserts the mask equals its expressions after every writer, through `g_drawInterestPtr` pointed at a local. The drift check's limit is stated; the fault-path writers (`ui_layer.cpp:1867`) are covered by that rig. |

Re-review asked for on these dispositions.

**Re-review (Opus, 2026-11-04): one blocking point, taken.** Notes outside the
repo: `b6-rereview.md`. It supersedes N7's disposition and adds to N1 and N9:
- **N7, amended (BLOCKING).** Clearing the module's pending flags on every
  return of `forwardWithVerdict` would clear them for foreign-context draws
  too: all four thunks call it whoever owns the context (`:5308`, `:5390`,
  `:5479`, `:5571`), and `owner` is worked out inside (`:4312`). A worker's
  draw landing between the owner's `uiDepthOnEyeDraw` (`:2541`) and its Begins
  (`:4720`, `:4726`) would make `uiDepthPlanetBegin` and
  `uiDepthWorldScreenBegin` decline, since they decide on the module's own
  copy (`ui_depth.cpp:2985`, `:3097-3099`, checked). The rule is now: a return
  of `forwardWithVerdict` clears the module's pending flags only when this
  draw's own `flags.planetPending` or `flags.worldScreenPending` is set, which
  only the owner's eye draw that set them can be. Begin already clears them on
  entry, so this covers exactly the early returns and the `layered` case.
- **N1, added:** item 4's seam gets an `edvr_log.py --self-test` fixture for
  the `build 6 counts:` line, drift figures included.
- **N9, amended:** seven mirrored modules have no rig (`particle_fix.cpp` and
  `sunglare_fix.cpp`, `build.bat:727`; `quad_probe.cpp`, `:709`;
  `draw_census.cpp`, `:683`; `remlok_fix.cpp`, `:700`; `fss_panel.cpp`, `:694`;
  `desktop_window.cpp`, `:663`). No new rigs for them: they rely on the
  source-text writer check and the drift line. The rigs that exist
  (`ui_depth_test`, `hologram_depth_test`, `ui_quality_test`,
  `particle_capture_test`, `screen_motion_test`) assert the mask after their
  module's writers. The fence writer (`ui_layer.cpp:1867`) gets a named
  `ui_quality_test` case that forces the hologram restore to fail and asserts
  the bit, or, if that rig cannot reach it, the source-text check alone, and
  the spec says which.
- The spec is written from the table, as amended here, not from the item text
  above it.

**Second re-review (Opus, 2026-11-04): approved.** N7 as amended is exact;
N1 resolved. On N9 it corrects the rig list: `particle_capture_test` compiles
pure headers only (`build.bat:3795-3804`) and `ui_quality_test` scans
`ui_layer.cpp` as text (`:3645-3652`); the rigs that link `ui_layer.cpp` whole
are `ui_layer_world_test` (`:3706`) and `ui_layer_device_test` (`:3717`), so
the fence case goes there or falls back to the source-text check. The spec
carries the corrected list.

**Built (2026-11-04, `d5135e52`, spec #191).** Sonnet implemented it
(`3a0cb70e`); full gated build green, then the review below, then green again
on the committed tree.
- **R3 reversed on the code.** Eleven shutdown functions recompute their
  bits (`flatTemporalStop`, `uiDepthShutdown`, `uiLayerShutdown` and others),
  and `shutdownDeviceHooks` can run on another thread under the loader lock
  (`device_hook.cpp:3313-3318`). A plain read-modify-write of the whole mask
  could then drop another module's bit. The mask is now a
  `std::atomic<uint64_t>`, read with relaxed loads in place (a plain `mov`)
  and written by a compare-exchange loop (`80c10127`), so no reader sees a
  transient and no module's write overwrites another's. Left open: two
  writers of the same module on two threads, possible only on an abnormal
  unload.
- **Departures:** the fence raise (`ui_layer.cpp:1867`) could not be staged
  in `ui_layer_world_test` or `ui_layer_device_test`, so it rests on
  `gate_test`'s source-text check, as the spec allows; `ui_depth_test` asserts
  the mask over `uiDepthShutdown` and the recompute's eight combinations, not
  `uiDepthConfigure` (the rig aborts on Config). 30 bits are mirrored.
- **Environment:** a profile-symbols build needs `EDVR_TRACE_EVENT_DIR` set
  to the Build Tools' `Common7\IDE\PrivateAssemblies`; `cpu_profile.py
  --build-analyzer` looks only under `C:\Program Files`.

## Flight 2026-11-04 (build 6)

Build `d5135e52` (log `edvr_gfx_20261104_113631.log`, `--expect-build HEAD`
matches), the L2 spot, standing still, DLAA, one 100 s capture. `--health`
exits 0: no NEVER APPLIED. Notes outside the repo: `profile-b6\cmp6.py`.
One full on-foot log window, 11:39:12-11:40:15: 17,450 full-path draws a frame
(build 5: 16,308), spanning profile windows 3-14.

**The total did not move.** Own time per full-path draw 283.5 ns (SE 3.2, 13
windows) in build 5, 283.3 ns (SE 2.5, 12 windows) in build 6; without
Map/Unmap 246.2 and 246.0. The difference is under a tenth of its standard
error.

**Per item**, ns per full-path draw:
- **S2: passed.** Eye-memo misses per full-path draw 0.175 to 0.097 (-45%;
  1,683 a frame, against 2,859), expected about 40%. `viewIsSurface` with its
  callers 9.81 to 6.71, the binding shadow 5.62 to 3.89: about 0.08 ms a frame.
- **S1, the interest mask: failed, it saved nothing.** The drift check ran
  30,666 times in the window with 0 mismatches and 14 of the 30 bits set, so it
  ran and was exact. But each flag test it replaced cost the same as the bit
  test that replaced it, line for line in `forwardWithVerdict` (matched by
  source text): `uiLayerIssueBlocked()` 1.48 and 1.26 to 1.41 and 1.41,
  `celestialMotionLive()` 1.32 to 1.07, `nativeOnFootLabelPending()` 1.10 to
  1.61, `uiLayerLive()` 0.94 to 2.52. `beginPanelOverride` with
  `uiDepthOnEyeDraw` 44.4 to 43.7, which is item 5's undo and nothing more.
- **`forwardWithVerdict` and the thunk lambdas rose** 56.5 to 65.1 (+0.15 ms),
  most of it on lines with no new work: 6.7 on `drawReissuesFor` (four
  compares of a stack struct) and +4.1 on an unchanged `screenMotionLive()`
  test in the thunk. That is where samples land after the game's draw returns,
  not a cost of those lines; it offsets S2's saving and item 3's TLS removal.
- **Item 3 (#184):** fixed by construction; no counter, as planned.
- **Item 5:** as expected, neutral.

**Nothing changed on screen.** Composites left in the scene: 0 in every
window. `ui depth totals:` wrote 14.0 interface draws a frame (left alone
32.7), the top of the arc's spread, where builds 1 and 2 sat for minutes.
GPU timing queries 76.0 a frame (build 5: 75.2, build 4: 74.8).

**Why S1 failed.** Build 5's items 2 and 3 were read as cold loads; S1 was
built on that reading. It is wrong: at 17,000 draws a frame every flag's cache
line stays resident, so a flag test is one L1 load, and so is a bit test (a
relaxed atomic load of `interest` after a load of `g_state`, which the
compiler cannot merge across sites). The per-line costs in build 5 that
looked like stalls are where the sampler charges the instructions after a
call into the driver, and they move when the code around them moves.

- ruled out: a write-through interest mask as a cut (S1), because the flags it
  replaced were already L1-resident; each bit test costs what the flag test
  did.
- ruled out: reading a per-line own-time peak just after a forwarded D3D call
  as that line's cost; it moves with layout (build 5 `:4751`, build 6
  `:4694`).

## Closing build: S1 and the temporary lines removed (2026-11-04)

The arc branch is cumulative: builds 1-6 are all on it. This build makes it
fit to submit, removing what was there only to measure or what did not pay.
It adds nothing, so it skips the design review; a fresh review of the diff
replaces it.

- **Removed:**
  - S1 (ruled out in "Flight 2026-11-04 (build 6)"): `draw_interest.h`,
    `DrawHot::interest`, `g_drawInterestPtr`, every module's `...InterestBits()`
    and `...RecomputeInterest()`, the install store, and the drift check in
    `bindingAudit`. Every ladder site goes back to its build 5 flag test.
  - The TEMPORARY `build 6 counts:` line, its counters and its `edvr_log.py`
    parser.
- **Kept:** S2, the #184 fix (`DrawCallFlags` with the pending flags, N7's
  clear), item 5's revert, `draw mask:` (a standing line), and builds 1-5's
  cuts.
- **Mechanics:** sixteen modules changed in build 6 only for S1, so they return
  byte for byte to `85006708`: `blend_probe`, `celestial_motion`,
  `depth_probe`, `desktop_window`, `draw_census`, `fss_panel`,
  `native_on_foot`, `object_probe`, `optimized_station`, `panel_curve`,
  `particle_fix`, `quad_probe`, `remlok_fix`, `scrim_fix`, `sunglare_fix`,
  `flat_temporal` (each `.cpp` and `.h`; checked: their other build 6 lines
  exist only to call the recompute). `vscreen`, `ui_depth`, `ui_layer`,
  `perf_monitor` and the rigs are edited by hand.
- **Check flight:** the L2 spot, one 100 s capture. Pass: `--health` with no
  NEVER APPLIED, composites 0, `ui depth totals:` within the arc's spread,
  own time per full-path draw not above build 6's 283 ns beyond noise (SE about
  3 ns).

**Built and reviewed (2026-11-04, `a7941eb0`, spec #194).** Sonnet removed
it; full gated build green on the committed tree. The sixteen S1-only modules
are byte-identical to `85006708`. A fresh Opus review of the diff approved
it: every former mask site equals build 5's test, order and short-circuits
included; S2, the #184 fix and item 5 are unchanged from build 6. Two notes,
not fixed (tests only, each would cost another build):
- `ui_depth_test.cpp:272`, `:274`: with the miss counter gone, the S2 memo case
  passes whether the call hits or misses; S2's key stays tested in
  `scrim_metadata_test`.
- `gate_test.cpp:1698`: the scan for removed S1 names skips subfolders of
  `src` (`fsr`, `smaa`, `plugins`), which are clean today.

## Flight 2026-11-04 (closing build)

Build `a7941eb0` (log `edvr_gfx_20261104_130625.log`, `--expect-build
a7941eb0` matches; HEAD was a docs-only commit later), the L2 spot, standing
still, DLAA, one 100 s capture (`profile-closing\`, notes outside the repo:
`per_draw.py`). Position at the capture's start matches builds 3-6 to six
decimals of a degree; heading within 3 degrees.

- **Composites 0, `ui depth totals:` 9.3 a frame:** both pass.
- **`--health`: one NEVER APPLIED, head in the shadows (F95c); exits 3.** It
  fails the criterion as written, and the cause is the scene, not this build.
  The F95c code is byte-identical to build 5, which read 12 head and shadow-view
  pairs a call. Across the arc the same spot flies in two states: 12 pairs at
  about 16,000-18,000 draws a frame, and 0 pairs at about 11,000-14,000 (build 2's
  11:24 flight on 2026-11-03, build 3, this one). The 0-pair state also lacks
  the `0x4010` and `0x4020` shadow views and sits near 185 `weapon_motion`
  skips a frame, against about 3,870. What selects the state is not known.
- **Own time per full-path draw: 292.9 ns (SE 5.2), 250.5 without Map/Unmap;**
  build 6 283.3 (SE 2.5) and 246.1. The difference is 1.7 of its standard
  errors, and 4 ns without Map/Unmap, across different scenes: not a regression
  beyond noise. Own time 4.07 ms a frame. Draws a frame are estimated at 13,933
  from the tee counts, because the second `draw mask:` window runs past the
  capture into the exit from the game. Windows 5-6 are excluded: own samples fell to a third while
  tee calls held, and the cause is not known.

- ruled out: removing S1 as the cause of F95c's NEVER APPLIED, because the same
  0-pair state appears in builds 2 and 3, and on `e1606a23` before the arc.

**The arc's saving.** Sum of what each flight measured, ms a frame: build 2's
byte 0.6 gross and at least 0.4 net of its rises, build 3's copy 0.19, build 4
0.45-0.60, build 5 0.34; build 6 net 0. About 1.4-1.75 ms, on a cycle of about
32-36 ms. Build 2's figure is the softest: its baseline capture lost 1.29M
events, in another scene and session. Like for like since build 3,
in the 0-pair state at about the same draws (13,480 and 13,933): own time 4.72
to 4.07 ms, R1 23.8 to 22.4, cycle 33.8 to 31.7. Build 2's 0-pair flight (14,076 draws) read R1 26.1 and
cycle 35.8; most of that wider gap is not attributed to this arc, because the game's own time also moves between sessions.

## Merged with `main` (2026-11-04)

`origin/main` `859cde44` merged for PR #166: 248 commits since `e1606a23`,
conflicts in six files. Full build green on the merged tree; not flown. A fresh
Opus read of the merge for silent collisions found none beyond these three.
The depth writer's merged cleanly and no rig covers it; the SRV rig pins the
gate term and `sharp_weapons_test` scans the may-want list, so the other two
probably would have failed a rig:
- **Depth writer probe:** `pixelProbeLive` required `rtv0Eye`, but the probe's
  depth-only draws have no eye colour, so it also tests
  `depthWriterWantsDraws()`.
- **SRV turret HUD:** upstream's new gate term became the holder
  `SrvTurretHud` (29 terms).
- **Normal-space dust:** `kVsNormalSpaceDust` added to
  `uiLayerFamilyHdrVsMayWant`, or the may-want byte would skip the family.

Three rigs' text pins moved to the merged forms (`sharp_weapons_test`,
`depth_scene_pick_test`, `srv_turret_hud_test`). Noted, not changed: the cockpit
glass overlay's second draw calls the real draw directly, so `draw reissue CPU:`
does not count it; the comment above `ReissueClock` says so.

## Review of the merge (2026-11-04)

The maintainer's review of `40dfb5df` requested changes. Done here:

- **A fourth collision, of another class: item 1 (#163).** Nothing clashed in
  the text, but `main`'s F196 (`76b03845`) and F207 (`a4413c83`) changed a fact
  it rests on: the depth probe is wanted on foot with AA off, and the tee only
  runs on foot, so the size test decided at most one frame per on-foot entry.
  Removed: `sceneDepthSize`, `sceneDepthSizeMatch`, `worldSceneDepth`,
  `bindingResolveAgreement` and the agree/disagree counters; `sceneRefWhy` is
  `eyeOfDraw(frame) >= 0` again; `teeHeldBy` stays. Main already publishes rows
  with AA off.
- **#151's exit removed.** The instanced hook's `drawPathFast` needs the draw
  gate shut, and the shipped `edvr.ini` holds it open, so it never ran and cost
  four loads a draw under DLAA. #151 stays open until #152 makes it reachable.
- **Dead code:** `drawReissuesFor`'s `ended` parameter, `perfMonitorFrameNo`.
- **Corrections:** the total above is net of build 2's rises; decompiler detail
  taken out of this doc's later review sections (the maintainer holds the
  private analysis repository). The text stays in the branch's older commits,
  which merge unsquashed because this doc cites them; `main`'s own docs already
  carry the same function names.
- **S2 is a behaviour change, approved, and not exact by construction**
  (see "Build 6: one interest mask"): the flights' 0 disagreements are evidence
  for it, not proof.

- ruled out: item 1 on the merged tree, because F196 and F207 make the depth
  probe answer on every world draw but the first frame on foot.
- ruled out: the instanced `drawPathFast` as a cut today, because no shipped
  config shuts the gate.

A second read of the merge for changed premises (Opus, git objects only, not
run; `native_on_foot.cpp` and `ui_layer.cpp` by keyword) found no other.

S2's key against upstream's VR overlay guard holds, read from the code. The
guard's t3 sets run under `FlatComputeInternalScope`, so
`hookedPSSetShaderResources` returns before `bindingSet`: neither the shadow nor
the key hears them. The classifiers read the shadow, which keeps the game's t3
throughout. A game re-set of the same pointer still moves the slot's own
generation, which is what the guard and `cache.srv3` compare. The flights' 0
disagreements predate the guard, so this rests on the read, not on a count.

**This merge is not flown.** The maintainer chose to merge on a green build. The
next flight, with `--expect-build` and `--health`, should cover:
- DLAA, on foot, weapon drawn: ADS, scope lens, markers, weapon readout (the
  arc's concourse flights cannot draw a weapon).
- AA off, the same; no AA-off flight is recorded since build 1.
- SRV turret view, cockpit glass, normal-space dust, the depth writer probe.
