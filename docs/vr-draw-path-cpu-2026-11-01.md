# VR draw-path CPU cost (2026-11-01)

## Status

- **State (2026-11-02): build 1 flown (`53c965cd`); it failed the total
  criterion, and the maintainer approved the cleanup the same day** (branch
  `build1/cleanup`, based on `c089b802`). Shipped: item 1 as the standard
  behaviour (it is a behaviour fix with AA off: eye rows, marker fix and
  reticle depth now apply), item 3, the re-issue counter. Removed: items 2
  and 4. No temporary key remains. Draft PR #166 holds the build.
- **Review of the flight (2026-11-02, "Review of build 1's flight"):** the
  render thread is CPU-bound; build 1's DLAA items were worth 0.2-0.4 ms
  against a 0.8 ms leg-to-leg noise floor, and the profile prices were
  diluted about 1.7x by a non-concourse window. Build 2 (/GL flight) is
  replaced by the draw pair mask.
- **Build 2 (designed 2026-11-02, "Build 2: the draw pair mask"):** a
  per-shader-pair mask skips DLAA subscribers that cannot want a draw
  (#152), plus `drawPathFast` in the instanced hook (#151). No temporary
  key. Reviewed the same day ("Review of the mask design"): about
  0.4 ms, not 0.8-1.2; two bits are wrong; too small for an ABBA flight.
  Decided: "Build 2 as decided" (no key, instruments, one profiled flight).
- **Build 2 built (2026-11-02, branch `build2/may-want-byte`, #168):** the
  VS "may-want" byte, the pair test before `GetType`, the stash deleted,
  `drawPathFast` in the instanced hook, and the instruments. Log lines to
  read in the flight: `draw gate holders:` (on change), `draw mask:`,
  `gpu timing queries:` (each window) and the F87c lines' `Unmap copy`
  figures; `edvr_log.py --perf-windows` lays them beside R1 and the cycle.
- **Build 2 flown (2026-11-03, "Flight 2026-11-03"):** the byte's targets
  fell about 0.6 ms a frame in the profile (`uiLayerFamilyOf` 0.41 to 0.01,
  screen motion's draw calls to 0) and nothing rose more than 0.2 ms. The
  marker fix's Unmap copy is the next target: 0.80 ms a frame, reading
  about 230 KB of write-combined memory per eye per frame.
- **Post-build-2 review (2026-11-03, "Review of build 2's flight and next
  targets"):** two tracks: (A) next EDVR-internal CPU cuts (`streamUnmapCopy`
  0.80 ms via tight instance windows, `hookedPSSetShaderResources` 0.90 ms,
  `hookedMap`/`hookedUnmap` 1.08 ms, `flushIaObserved` 0.49 ms, and the F100
  tee's eye-0 slow-path trap); and (B) engine and hook-chain draw reductions
  (shadow-cascade small-prop filter and sub-pixel clutter floor in
  `FUN_1442B3FC0`, concourse keep-cell in `optimized_station`, layer-A
  weathering decal fade gate, and bypassing 3Dmigoto on `t33` scene draws).
  Checked the same day ("Check of that review" and "Rebuttal & code trace on
  A1, A2 and B1"). Kept: the marker copy (with two zero-coverage-loss cuts
  found in `streamUnmapCopy`: duplicate eye copies and overlapping windows),
  the bind and map hooks, the post-R1 split, and the layer-A fade gate.
  Ruled out or deferred: the tee trap under DLAA (0.08 ms; only matters with
  AA off), the sub-pixel floor, the EDHM bypass, and offscreen/concourse
  culling until a census/RTV check establishes what those draws are.
- **Build 3 as agreed (2026-11-03, "Reply to the rebuttal" and "Argon's
  reply"):** merge both eyes' marker-copy windows and copy each range once;
  same coverage by construction, so no key. Size the merged plan for up to
  `2 * kWinMax` (14) windows and `2 * kWinCapEntries` (65,536) entries so
  disjoint eye plans never truncate. The logs bound its saving only loosely, at
  0-86% of the copy (0-0.61 ms); the build 3 flight measures it. Ruled out:
  skipping the windows during an escalation (escalations only on arrival).
  Open: what the non-eye-target draws are (the `weapon_motion` skip count:
  4,070 a frame in one log, 514 in the other; not `8BC3..` draws). Agreed by
  both reviews.
- **Question:** how much of a CPU-bound VR frame is EDVR's draw path
  (GLOSSARY: Draw path), and how much can go without changing what reaches
  the screen?
- **Earlier investigation:** `docs\stereo-cpu-sharing-2026-10-16.md` (the
  2026-10-31 flight, H5-H7, and the 2026-11-01 CPU profile).
- **Test build 1:** item 1 (scene depth without AA: world rows and the
  marker tee's fast path; changes behaviour, #155), item 2 (benchmark
  collector), item 3 (two early exits inline), item 4 (depth slot cache),
  and a re-issue counter (GLOSSARY: Re-issue).
- **Temporary keys: all three are gone (2026-11-02).** Item 1's key is
  removed and its path is the behaviour; items 2 and 4 and their keys are
  removed with their code. See "The temporary keys".
- **Confirmed (item 1):** with AA off and the key off, every world draw is
  held as not scene depth (about 64M per leg), the tee never goes fast and
  F113 reports `none` 100%. Key on: fast path 36-46%, F113 scene meshes 94%.
- **Ruled out:** re-issues as a CPU cost: at most 0.004 ms per sampled frame
  in any window, and none at all with AA off. Item 4 as a saving: 0 hits in
  the whole flight, because the binding generation moves between every eye
  draw. Item 2 as a saving in flight: the collector cleared nothing, done or
  skipped, in any on-foot or cockpit window.
- **Ruled out:** idling the whole marker tee without the UI layer, because
  its slow path is the only publisher of the per-eye world rows
  (`eyeRowsPublish`), which on-foot systems read without the layer.
  Changing `eyeOfDraw` itself, because `rowSourceDraw` and five census
  callers would shift and a wrong eye would publish wrong rows silently.
- **Left out, tracked:** #151 (`drawPathFast` in
  `hookedDrawIndexedInstanced`), #152 (per-scene subscriber mask), #153
  (cross-profile per-bind counter), #154 (`/GL /LTCG`).
- **Reviewed:** design review 2026-11-01; its corrections are folded in.

## Evidence in brief

Rig: PSVR2, SteamVR OpenXR, 2632x2684 per eye, 90 Hz, RTX 5070 Ti, Ryzen 7
5700X3D, EDHM chained, Motion Smoothing off, game build 334925, live ini
`temporal_aa = dlss`, `eye_depth_capture` unset. Test spot: a station
concourse (13-14k eye draws a frame); nothing below is specific to it.

- CPU-bound: `held_by=game` 96%, vendor xrEndFrame about 0.36 ms.
- 10-31 log (`edvr_gfx_20261031_225516`): DLAA 29.3-29.6 fps, `draw hook
  CPU` 5.8-6.1 ms; F7 off 34.7-34.8 fps, 2.7-3.5 ms.
- The draw gate (GLOSSARY: Draw gate) never closes in VR.
- Profile: EDVR's own code is about a quarter of the busy render thread;
  about 3.6 ms of the 5.8 ms the hooks read with DLAA is not EDVR's code.
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
frame polled, and `worldOk` set for both eyes. F7 overrides
`fix.temporal_aa` (aa_compare.h:5-8); `depthProbeConfigure` re-reads it, so
with AA off the probe is not wanted (depth_probe.cpp:518-526) and
`depthProbeCurrentSceneEyeOf` returns false (860). `worldDraw` takes its eye
from the colour target (`e = t->eye`, native_on_foot.cpp:29298) but passes
`eyeOfDraw >= 0` to `sceneRefWhy` as "is scene depth" (29307), which
refuses with `kSceneNotSceneDepth`. So `worldOk` never sets, `f44::draw`
never early-returns, and no world rows publish. `pending` and `bound` clear
within one draw; `measure`, `census`, `check`, `polledFrame` do not depend
on AA.

**Change.**
1. **Tally (always on).** At the fast-frame publish (native_on_foot.cpp
   about 35664) count the first failing term (measure, pending, bound,
   census, check, polled, world 0, world 1), and add
   `sceneRefused[kSceneNotSceneDepth]` (29316; today printed only in
   measure mode). Print both on the tee's cost line after `over %u s`,
   reset with the fast-call count. Extend the `edvr_log.py` parse (about
   16778, not end-anchored) with a self-test fixture for the new text.
2. **Scene-depth test without the probe (keyed).** A separate predicate,
   used only for `worldDraw`'s `sceneRefWhy` argument when the probe is not
   wanted: the bound depth view's texture has the same size as the draw's
   colour target, whose eye `worldDraw` already knows. Read at runtime;
   nothing per headset. `eyeOfDraw`, `rowSourceDraw` and every other caller
   stay unchanged; mesh rows suffice.
3. **Agreement check (always on, measurement only).** While the probe is
   wanted (DLAA), compute the predicate too and count agree / disagree
   against `eyeOfDraw >= 0`. This validates it against the probe's pair
   pick, which handles twin eye-sized depth targets that the predicate
   cannot tell apart.

**Behaviour change.** With AA off and the key on, the systems that read the
world rows (ADS aim and sight, scope zoom, the head-containment grid,
`f109TurnTick`, `f87EyeLines`, `pinCycloUpdate`, `geometryNow`) receive rows
they do not get today. The separate on-foot weapons work reads the same
rows; they meet at merge.

**Key.** `experimental.vr_eye_rows_without_aa`. Off: today's refusal. A
mid-frame flip is harmless: `eyeOfDraw` and the fast frame are per frame.

**Shows it ran.** Key on, AA off: F113 row sources other than `none`, the
cost line's `fast path X of N` well above 0, its est. ms a frame down.
Key off, AA off: tally names `world`, `NotSceneDepth` large. If the tally
names another term, the cause is refuted: write it down and stop.

**Fails if** DLAA agreement is not near 100%. Then the fallback becomes a
pick-only mode of the depth probe, separate from `g_wanted` (which ui
depth's `dsvIsSceneDepth`, ui_depth.cpp:878, also reads).

**Test (red first), `tools\native_frame_test`** (its F100 checks).
Corrected from `native_on_foot_hooks_test`, which does not compile
native_on_foot.cpp. The predicate and the tally's first-failing-term
function are pure functions in native_on_foot_f100.h, beside `teeFastFrame`.
The predicate: matching depth and colour sizes pass; mismatched size, no
depth view, and the probe-wanted case (predicate not used) do not. The
tally: each term alone is reported, earlier terms win, all clear reports
none.

**Expected.** 0.6-1.0 ms a frame with AA off (the tee keeps its fast-path
cost); none with DLAA.

## Item 2: benchmark collector cleared only when used

**Current.** `NativeBenchmarkCollector` (perf_monitor.cpp:174) holds 16384
entries plus orphans (native_benchmark_collector.h:81, 136). `reset()`
calls `clearEntries()` (native_benchmark_collector.cpp:34) on every perf
tick while native perf history is inactive (perf_monitor.cpp:1119, 1185).

**Change.** A used flag, set where entries are created: `find(create)` and
`findOrphan(create)` (native_benchmark_collector.cpp:94, 117), not in
`observe`, which runs every tick. `clearEntries()` clears the flag. `reset()`
skips only `clearEntries()` when the flag is clear, and still resets
`scope_`, `phase_`, the drop counts and flags. `begin()` is unchanged.
`distribution()` only reads, so a clean collector is all zero.

**Key.** `experimental.vr_cpu_benchmark_idle`. Off: clears every tick.

**Shows it ran.** The perf line gains clears done and skipped. This runs in
the Present-time perf tick, outside the draw hooks, so it cannot move
`draw hook CPU`; it is judged by this count.

**Test (red first), `tools\native_perf_history_test`.** A clean collector's
`reset()` does not clear; after an entry is created it does and every entry
reads zero; `reset()` still resets phase and scope; key off always clears.

**Expected.** 0.10-0.14 ms a frame, wherever native perf history is idle.

## Item 3: two early exits inline

No `/GL` or `/LTCG` (#154), so calls across source files never inline.

- **`uiLayerCrispNoteEyeDraw`** (ui_layer.cpp:5373). In VR the caller
  (vscreen.cpp:2536) already checks `uiLayerCrispOn()` and `ctx` is never
  null, so every eye draw enters the body and leaves at the shape test
  (`kind`, `count == 3`, `instances`, `startInstance`, 5378). The inline
  wrapper calls the body when `detail::g_uiLayerCrispPending` is set or
  that shape test passes, and otherwise returns false. Corrected: the
  wrapper cannot do the body's two clears itself, because `g_crispPending`
  (ui_layer.cpp:4463) is in the anonymous namespace. Every path sets and
  clears it together with the header flag (5361-5369, 5375-5376, 5485,
  5673, 6825, 6937, 7064), except `uiLayerCrispToneBegin`'s null-context
  return (5670-5671), which cannot happen in VR. So a clear flag means
  nothing to clear. The flat caller (flat_ui_layer.cpp:371) is unaffected.
  Test (red first), `tools\ui_layer_device_test`, which compiles
  ui_layer.cpp whole: a non-matching draw with nothing pending does not
  enter the body, a matching one does, and a non-matching draw while a
  tonemap draw is pending clears it, as today.
- **`nativeOnFootLabelIssue`** (native_on_foot.cpp about 30494, called at
  vscreen.cpp:4457). `g_labPending` is in the anonymous `f44` namespace
  (30188), so a header cannot read it. Corrected: no mirror. `valid` leaves
  `LabPending` and becomes one inline `std::atomic<bool>` in native_on_foot.h, relaxed,
  which every site uses (30257, 30305, 30314, 30495-30496, 35879), the
  vscreen call site included. The body keeps its full test. The 35879 reset
  runs in `shutdownDeviceHooks` (device_hook.cpp:3313, by way of
  `shutdownVScreenFixes`), on the thread that unloads EDVR and possibly
  under the loader lock, not on the render thread. That is why the flag is
  atomic; a relaxed load costs the same as a plain one on x64. There is no
  rig, because no rig compiles native_on_foot.cpp; with one flag there is
  nothing to drift.

**Key.** None: always helps, and a key would add a branch to the path.
**Shows it ran.** Only in total `draw hook CPU`. **Expected.** About 0.1 ms
a frame with DLAA.

## Item 4: depth surface slot cached

**Current.** `uiDepthSampledSurfaceSlot` (ui_depth.cpp:2419) calls
`viewIsSurface` on PS SRV 0-3 for each qualifying eye draw (vscreen.cpp
4108, 4119); `viewIsSurface` alone is 0.10 ms a frame with DLAA.

**Change.** Remember the answer while all of these are unchanged: the
PsSrv0..3 bind generations (or `bindingEyeKeyGeneration()`, binding_shadow.h
138-153); `g_classifierEpoch` (ui_depth.cpp:335-342), which already bumps on
every surface-table write, on configure and every frame boundary;
`g_uiDepthOn` and `g_uiDepthStoodDown`; the key. `viewIsSurface`'s own
120-frame view memo is safe only because `bindingFrameBoundary` bumps every
slot's generation each frame; the cache inherits that. Known existing gaps
(not new): a null-array `PSSetShaderResources` (vscreen.cpp about 3110) and
`g_flatComputeInternal` binds skip `bindingSet`.

**Key.** `experimental.vr_cpu_depth_slot_cache`; a change clears the cache.
**Shows it ran.** Hits and misses per perf window.
**Test (red first), `tools\ui_depth_test`.** Repeat call is a hit; each
input changed alone forces a miss; key off always recomputes.
**Expected.** 0.1-0.2 ms a frame with DLAA.

## The re-issue counter

Names the owner of the unexplained ~3.6 ms. Many re-issues bypass
`pureDrawReissue` (vscreen.cpp:3908) and call the real draw directly. All
re-issues go through one counted helper with a caller name: motion vectors
(`screenMotionDraw`, `screenMotionUiDraw`, 5501-5502), weapon motion
(5440), panel curve (4047, 4405, 4485, 4601), loader panel (4369), quad
split (3744-3807), flat UI and sun (5135-5138, 5217-5220), ui depth (4553,
4570, 4572, 4581, 4587), star glow, blend probe, crisp HUD, world screen,
`uiLayerSecondIssues`, tonemap. Logged as `draw reissue CPU:` beside `draw
hook CPU:` (perf_monitor.cpp:1216), timed on the same sampled frames and
stride scaling (1199-1216) so the lines compare. No key. Traced end to end:
no re-issues prints 0, never an absent line.

Review fixes (build 1): the draw hook clock now stops at the game's own draw,
so the weapon-motion re-issue is inside `draw hook CPU:` as the line says (the
hook figure rises by that cost; it is EDVR's own work). The splash dim's
second issue and the FSS reveal replay go through the helper (`splash_dim`,
`fss_reveal`). `flat_ui` is outside the hook figure (the flat branch has no
hook clock): labelled, not clocked. `loader_panel` never prints (its
substitute ignores the draw it is handed); kept as a name, noted in the
tally's reading guide. The ms are the draw call only, not the Begin/End state
work. `--tally reissue` flags a GPU census re-issue section that ran against
windows counting none of its callers. The benchmark collector counts clears
at the idle tick and the native-menu tick; the line says so. `--perf-windows`
drops the window after a flip logged at a window's end frame as well. The
depth-size agreement check (item 1, DLAA) resolves through
`bindingResolveAgreement`, a fault budget of its own, so it cannot spend the
probe budget the world rows use. The slot cache keeps its `on` and
`stoodDown` inputs: both are constant at the lookup, but the invalidation
across a gap where UI depth was off rests on them as much as on the epoch,
and removing them is not worth a rig rewrite for one compare each.

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

- In `[experimental]`; off restores today's behaviour exactly. The code
  default is on; it reaches the game because install leaves the live
  `edvr.ini` alone and these keys are absent from it.
- Each has a `# TEMPORARY` comment block (live; removed when this closes).
  No `# dev: hidden`, so each shows on the F8 Experimental page under
  `menu.developer = on`.
- Read once per config sweep into a relaxed atomic (the `g_aaOffDraw`
  pattern, vscreen.cpp:6162). A stale read only falls back to today.
- Each change logs one line with the key, its value and the frame number.
  `edvr_log.py` drops any perf window that contains a flip.
- A menu flip writes the live `edvr.ini` and the `%LOCALAPPDATA%\EDVR`
  mirror; removing the keys means cleaning both.

## Flight plan

One session, no restart, keys flipped from F8, measurement families off
(`measure` blocks the fast path). Legs of 4 minutes at the 10-31 spot:

1. DLAA, all keys on; then all keys off.
2. F7 (AA off), all keys on; then all keys off.
3. Repeat 1 and 2.
4. F7, only `vr_eye_rows_without_aa` off.
5. A short cockpit leg, all keys on: no-regression only.

First `python tools\edvr_log.py --target steam --expect-build HEAD
--version`, then `--health --since/--until` per leg. Expected: in keys-off
AA-off legs `_hj_f100_tee` reports NEVER APPLIED (exit 3) by design.

Weapons cannot be drawn in the concourse, so ADS aim and scope zoom with
item 1 on are unchecked; that waits for a flight where weapons can be
drawn, or for the weapons work.

## Pass criteria

- The log is from this build.
- Each item's own reading shows it ran: item 1 the tee's est. ms a frame
  down and F113 sources not `none` (key on, AA off); item 2 clears skipped;
  item 4 hits; `draw reissue CPU:` present with its callers.
- Item 1's cause holds (tally and `NotSceneDepth`, keys off, AA off) and
  DLAA agreement is near 100%.
- Total: `draw hook CPU` keys on below keys off in the same session, by
  more than the spread of that mode's clean windows.
- fps keys on no more than 0.5 below keys off, in every mode.
- No visual regression: on-foot markers and labels with DLAA and across an
  F7 toggle; HUD and UI depth.

## Flight 2026-11-02

Build `53c965cd` (`--expect-build HEAD` matched), same rig and concourse
spot, one session, keys from F8. Legs of 4 minutes; windows that contain a
flip are dropped. `draw hook CPU` is the mean of each leg's kept 1800-frame
windows; the tee is `--grep 'tee held'` summed over the leg; fps is the
vScreen totals.

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

- Total: keys on is above keys off in all four pairs (+0.02 to +0.77 ms).
  The DLAA pairs differ by more than the spread inside a leg, but the scene
  moved too: 1d held about 40% as many world-0 draws as 1c. Fails.
- fps: 1c is 2.1 below 1d, past the 0.5 limit; the other pairs are within
  it or favour keys on. Fails the criterion as written. (This line first
  called the frame GPU-bound; the review below shows it is CPU-bound.)
- Item 1: the tee is about 0.27 ms a frame cheaper with the key on and AA
  off. `draw hook CPU` cannot show it: the tee runs before `DrawClock`
  starts (vscreen.cpp near 5417 and 5421). The rows also unlock: F113 scene meshes 94% (was none 100%), reticle depth F85c
  patched 100% (was NEVER APPLIED), marker fix F62c 6% (was NEVER APPLIED).
  Eye 1's world rows hold about 35M draws a leg (`world 1`), so the fast
  path reaches only 36-46% against DLAA's 70%. DLAA agreement: 0 disagree.
- Item 2: `entry clears 0 done / 0 skipped` in every window after the main
  menu (1800 skipped a window there). The collector does not run in flight.
- Item 4: 16 misses a frame with DLAA, none with AA off, 0 hits ever.
- Re-issues: about 28 a frame with DLAA (ui_depth, ui_layer_second,
  crisp_hud), 0 with AA off; their draw calls cost at most 0.004 ms per
  sampled frame.
- Visual regression: pending the pilot's report.

## Review of build 1's flight (2026-11-02)

An independent review re-read the flight and the 2026-11-01 profile
(notes outside the repo: `fable-build2-review.md`).

- **The bound is the render thread's CPU.** The runtime does not hold the
  game (`next_wait_roundtrip` 0.19 ms); the render thread runs for 98.5% of
  the time before the first submit (27377 of 27807 us), and that time is
  25.7 ms with DLAA against 21.7 ms with AA off. After both submits the
  frame spends another 7-8 ms before the next wait (game 5.1 ms, EDVR
  1.2 ms in the profile); not looked at before.
- ruled out: build 1 as a measurable change, because its DLAA items were
  worth about 0.2-0.4 ms while `draw hook CPU` moved up to 0.77 ms between
  legs with no code change, and every pair flew keys on first.
- ruled out: `draw hook CPU` as the judge for items 1 and 2, because the
  marker tee and the perf tick run outside its clock. The metric for a
  frame-level change is the render thread's time before the first submit.
- ruled out: item 2's premise, because the profile's clear is
  `clearEntries < NativeBenchmarkCollector::observe` (begin or
  startSampling), not `reset()`.
- ruled out: item 4's cache key, because the game binds pixel-shader SRVs on
  every draw, so a generation over the PS SRVs changes between every pair.
- ruled out: the build-1 function prices as concourse prices, because
  `self_split.json` averaged a non-concourse window (5) with window 12.
  Concourse windows alone put EDVR's own code at 6.2-7.8 ms with DLAA and
  4.0-5.3 ms with AA off; every build-1 price was about 1.7x low.
- ruled out: build 2 as designed (a /GL A/B flight of two separate
  launches), because it cannot resolve 0.3 ms against that noise, and the
  profile already bounds the named cross-TU callees at 0.25-0.35 ms of self
  time. /GL stays a build-only measurement (link time and a profiled A/B).

Where the review puts the gains, largest first:
1. The DLAA per-draw subscriber tail, about 3 ms: about 20 subscribers at
   0.05-0.3 ms each (`uiLayerFamilyOf` 0.27, `hookedEnd` 0.19, crisp 0.12,
   star glow 0.10, `viewIsSurface`, UI depth, engine velocity, screen
   motion, GPU census). One per-draw classification cached on the shader
   pair, skipping subscribers that cannot want the draw (#152), might save
   1.5-2.5 ms. Medium confidence.
2. The always-on per-draw ladder, about 1.9 ms in both modes (the AA-off
   floor): a VR fast path (#151, #152), 0.5-1.5 ms. Medium-low confidence.
3. `drawtag::streamLoad16`, 0.67-0.81 ms in both captures: the marker fix's
   instance-stream copy at Unmap, reading write-combined memory after the
   submits, outside every clock. Its cost line prints only with the detail
   log. Medium confidence.
- Outside EDVR: EDHM at 2.4-3.2 ms a frame, larger than builds 1 and 2
  together.

Next, cheapest first: re-export the existing ETLs for concourse windows
only and replace the function prices; a config-bisect flight with DLAA,
one subscriber family at a time from F8, judged on the time before the
first submit; then design the subscriber mask (#152) from the measured list.

## Concourse-only profile (2026-11-02)

The 2026-11-01 captures re-exported for concourse windows only (DLAA 7-11
and 15-18, 1003 frames; AA off 9-18, 1270 frames) with the build-identity
guarded analyzer. ms a frame, profiled (profiled R1 runs 24-33% over
unprofiled, so deltas read about 0.5 ms low; the DLAA capture lost 1.29M
events). "Innermost" includes the runtime, driver and EDHM below an EDVR
frame; the export cannot separate them.

| | DLAA | AA off | delta |
|---|---|---|---|
| R1 (before the first submit) | 32.26 | 28.79 | +3.48 |
| EDVR leaf | 7.43 | 4.46 | +2.96 |
| EDVR innermost | 12.90 | 8.92 | +3.98 |
| game exe | 13.0 | 13.3 | |
| EDHM | 2.8 | 3.1 | |

- DLAA delta by owner (innermost): UI layer +1.1 (`uiLayerFamilyOf` +0.41,
  crisp +0.17), UI depth +0.7, engine velocity +0.5, screen motion +0.4,
  star glow +0.2, GPU census +0.1. No function above 0.41.
- Always-on ladder (both modes): `beginPanelOverride`,
  `forwardWithVerdict` and `hookedDrawIndexedInstanced` 2.0 / 1.9, plus
  the draw lambda 0.83 / 0.81.
- Outside the draw clock: `drawtag::streamLoad16` 0.83 / 0.80;
  `clearEntries` through `NativeBenchmarkCollector::observe` 0.08 / 0.16.
- Unexplained: `f44::draw` 0.79 off against 0.13 DLAA;
  `hookedCreateTexture2D` 0.64 in DLAA R5 against 0.05 off.
- This replaces the per-function prices in the item sections above, which
  came from a mixed-window `self_split.json`.

## Build 2: the draw pair mask (#152, #151; designed 2026-11-02)

Decided 2026-11-02 with the pilot: the mask plus two free wins plus a
gate-holder log line; the UI layer family included; a live temporary key
flown ABBA. No change to when the draw gate closes (AA-off scene masks stay
out). Research notes outside the repo: `mask-research.md`.

**Mechanism priced.** The DLAA path makes about 14,000 eye draws a frame.
Most subscribers reject a draw with a hash compare, so the reject itself is
cheap; the cost is the number of cross-TU calls and live-flag loads paid
per draw before each one rejects. One mask test per draw that skips several
such calls is the saving. Estimate 0.8-1.2 ms a frame with DLAA, an upper
bound: the owner deltas in the concourse profile include the work each
feature does on the draws it wants, which the mask does not touch.

**The rule for what may be skipped.** A subscriber is skipped only on a draw
where its uncached code has no effect at all: no counter, no reset, no
recorded state. That excludes UI depth (it resets about 12 per-draw globals
that `forwardWithVerdict` reads, and is already memoized at a 91% hit
rate), `uiLayerNoteOther`, star glow's after-draw count, target sharp, and
the marker tee (its draw serial must advance). `temporalPassCountDraw`
stays unconditional.

### Changes

1. **The mask.** A pure header holds a table keyed on the game's (VS hash,
   PS hash), read from the binding shadow before `engineVelocityBeforeDraw`
   can substitute the bound shaders. Each entry is a set of "may want" bits,
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
   A clear bit skips the call. A set bit runs the subscriber exactly as
   today, including its per-frame checks (`sourceFrame == frame`, live and
   failed flags), which the mask never caches.
2. **Stale-true safety.** One config generation is bumped at the end of the
   config refresh (every F8 and ini change, and the profile switch) and by
   every runtime stand-down that can change a cached predicate. An entry with
   an old generation is recomputed. Any armed instrument (census, object,
   blend, quad or depth probe) bypasses the mask. A missing entry means
   "may want".
3. **Free win, #151.** `hookedDrawIndexedInstanced` takes `drawPathFast`
   as the other three draw hooks do.
4. **Free win.** `fssPanelRectDrawArgs`, a cross-TU call on every instanced
   draw, moves into the one branch of `beginPanelOverride` that reads it.
5. **Logs.**
   - A `draw mask:` line in each perf window: hits, misses and skips per
     bit, and the key state. Written every window, so zeros mean it ran.
   - A gate-holder bitmask beside the `drawGateSet` calls, logged on
     change, naming which term of `drawGateSubscribed` holds the gate.
   - `edvr_log.py --perf-windows` prints both.
6. **Temporary key** `vr_draw_mask = on | off`, live from F8, default on.
   Off runs every subscriber as today. Listed in Status; removed or kept as
   the behaviour once flown.

### Test seams

- The `aa_off_test` rig (it already drives `draw_fast_path.h` and
  `classifier_memo.h`) gains the mask: per bit, "mask clear implies the
  subscriber's predicate rejects" over the known pairs and random pairs; a
  generation bump recomputes; an armed instrument bypasses.
- The `sharp_weapons_test` rig: "the UI family bit clear implies
  `uiLayerFamilyFor` returns none" for every target kind.
- A source-scan test: every term of `drawGateSubscribed` has a holder bit,
  and every stand-down flag the mask reads bumps the generation.
- `edvr_log.py --self-test`: the `draw mask:` and holder lines parsed.

### Flight

One session, DLAA, all on-foot defaults, the concourse spot of 10-31,
standing still facing one spot. Note each leg's start time.
1. `vr_draw_mask` on, 3 min. 2. Off, 3 min. 3. Off, 3 min. 4. On, 3 min.

### Pass criteria

- The log is from this build (`--expect-build HEAD`); `--health` names no
  feature NEVER APPLIED with the key on that applied with it off.
- The `draw mask:` line shows skips in on legs and none in off legs.
- No visual difference between on and off legs.
- Worth keeping if R1 (before the first submit) on legs 1 and 4 is below
  legs 2 and 3 by more than 0.5 ms and by more than the spread within each
  pair. Otherwise `ruled out: per-draw subscriber calls as the DLAA cost`,
  and the mask and its key come out.

### Review of the mask design (2026-11-02)

Independent review against the code (notes outside the repo:
`fable-mask-review.md`). Verdict: change it, and do not fly it as a
keep-or-remove test.
- ruled out: the engine velocity bit, because a no-family draw after a
  substituted family draw is the one that restores the game's VS, PS, blend,
  t3 and MRT (`beforeDrawSlow` → `slowPath` → `restore`); skipping it leaves
  EDVR's shaders bound. It also loses the 1-in-64 shadow probe.
- ruled out: the temporal rigid-draw bit, because its gate is already five
  inline compares.
- ruled out: the 0.8-1.2 ms estimate, because it summed whole-owner deltas
  that include work on wanted draws. Priced per bit: UI family about 0.3,
  screen motion about 0.18, weapon motion under 0.05, the
  `fssPanelRectDrawArgs` move 0.03; about 0.4 ms net (0.3-0.5).
- ruled out: a four-leg ABBA judged at 0.5 ms, because R1 leg means within
  one build-1 session spanned 2.0 ms with DLAA and the scene drifts in
  steps; resolving 0.4 ms would take about 30 legs per arm, so a null would
  be a false "ruled out". The judge should be the whole frame cycle, not R1
  alone.
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
  `hookedEnd` +0.19 ms with DLAA (likely EDVR's own GPU timer queries), and
  a line-level split of `beginPanelOverride` (0.80 ms).

### Build 2 as decided (2026-11-02)

The pilot took the reviewed version: one build, one profiled flight that
prices several things.
- **Ships as behaviour, no key:** the VS "may want" byte (UI family on target
  kind 1, screen motion, weapon motion), the pair test before `GetType`, the
  `fssPanelRectDrawArgs` stash deleted, `drawPathFast` in the instanced hook
  (#151).
- **Instruments in the default log:** gate holders on change; mask skips per
  window; marker-fix Unmap copy bytes and time per eye per window; GPU
  timing queries per frame by owner.
- **Marker-fix copy, first read:** the build-1 flight log's F87c lines show
  whole-stream escalation in 12 of 568 ten-second lines, transients only.
  ruled out: escalated whole-stream copies as the steady 0.8 ms, because
  steady state is `escalated 0` with 6-7 windows per eye; the cost is the
  windowed copy itself.
- **Flight:** 5 minutes standing in the concourse, DLAA, with a CPU profile
  capture. Judged on the profile (per-function, about 0.05 ms) and the whole
  frame cycle, not on an A/B.
- Superseded: the `vr_draw_mask` key, the pair table and the ABBA flight
  above.

## Flight 2026-11-03

Build `277fae1f` with profile symbols, DLAA, Steam, PSVR2. The concourse at
Shamash 1 a, standing still. The log is from this build (`--expect-build
HEAD`). `--health` flags only full outfit (F142) as NEVER APPLIED, the same
as in every build-1 leg.

**Log, five 1800-frame windows (09:57-10:02, unprofiled):**
- R1 26.2 ms, cycle 36.0 ms. Build 1 leg 1a was 25.8 and 34.8, inside the
  noise as expected.
- `draw mask:` about 18,300 draws a frame on the full path. Skipped: screen
  motion's two calls on about 98% of them, the UI family on about 76%, and
  weapon motion on about 7M offscreen draws a window.
- Marker fix Unmap copy: one copy per eye per frame, about 230 KB each, 0.37
  ms (eye 0) plus 0.43 ms (eye 1) = 0.80 ms a frame, about 0.65 GB/s. The
  cost is reading write-combined memory, not the byte count.
- GPU timing query Ends: 75 a frame. 65 are untagged ("other": temporal
  pass, DLAA, engine velocity, the frame driver), the GPU census 8.5, UI
  depth 1.4, the particle copy 0.7-1.0.
- Gate holders: 13 of 28 (remlok, holo, ui_depth, sun_glare, object_probe,
  particle, backdrop, scrim, loader_panel, wake_pulse, night_vision,
  depth_probe, footprint).

**Profile.** The 300 s capture failed in the save: `wpr -stop` timed out at
180 s merging 7 GB. A 100 s capture (17:26:22Z, 2,773 frames, no events
lost) worked. Notes outside the repo: `profile-b2\table.md`. Baseline:
the 2026-11-01 DLAA concourse windows. Figures in ms a frame, old to new:

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

- The byte's own targets fell about 0.6 ms, at the top of the reviewed
  0.3-0.5 estimate. The rises (`shaderHashMemo`,
  `hookedPSSetConstantBuffers`, the hash map) total under 0.2 ms.
- The whole-EDVR drop (2.5 ms leaf) is an upper bound, not the build's
  saving. The game exe also fell 1.9 ms and EDHM 0.4, so the scene and
  session differ. The old capture also lost 1.29M events.
- The analyzer in the worktree's build lacked `msdia140.dll`, so the
  analysis ran from a copy with it added. That is the same symbol-path gap
  as the 2026-11-01 note.
- Next target, by size: the marker fix's Unmap copy (0.70-0.80 ms). Then
  `hookedPSSetShaderResources` (0.90 self), `hookedMap` and `hookedUnmap`
  (0.58, 0.50), and `flushIaObserved` (0.49).

## Review of build 2's flight and next targets (2026-11-03)

Post-build-2 review of `277fae1f` and the 2026-11-03 profile (`profile-b2\table.md`).
The VS "may-want" byte hit the top of its 0.3-0.5 ms estimate (~0.6 ms off its
direct targets at a 0.023 ms bind-time cost), and the F87c timer settled the
`streamLoad16` question: `streamUnmapCopy` alone accounts for the 0.70-0.80 ms
post-submit cost (one copy per eye per frame, ~230 KB per eye at 0.65 GB/s from
write-combined memory).

With EDVR leaf down to 4.97 ms in R1 while the full concourse cycle remains
36.0 ms (26.2 ms R1 + 9.8 ms between the first eye submit and the next frame's
start), further gains split into two tracks: (A) the remaining ~3.2 ms of
EDVR-internal leaf work exposed by build 2's profile, and (B) engine and
hook-chain draw reductions that cut time across the game exe (11.12 ms R1 +
~5 ms post-submit), EDHM/3Dmigoto (~2.4-2.8 ms), D3D11/driver (~5.9 ms) and
EDVR together.

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

2. **The F100 marker tee's Eye-0 slow-path trap (`src\d3d11\native_on_foot.cpp:35700`
   and `34075`):**
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
   - In the view loop `FUN_1442B4420` (`docs\stereo-cpu-sharing-2026-10-16.md`),
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
   - `FUN_1442B3FC0` has a separate pure screen-size test (`0.5*(A*d + B) <= r`)
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

The review's claims, checked against the code, the build-2 log and the
earlier engine docs.
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
- **A5, the timing queries:** unverified which of the 65 untagged queries
  feed live features (the frame driver, DLAA, temporal pass). Turning any of
  them off is a removal, so the maintainer is asked first.
- **A6, the post-R1 split:** a desk task on the existing 100 s capture.
- **B1, shadow-cascade clutter:**
  - The premise conflicts with `engine-render-pipeline.md`. The ledger found
    exactly 3 render targets (2 eyes and 1 offscreen) and no shadow or depth
    pass.
  - Offscreen draws are not established as shadow cascades. This needs
    evidence before any design.
- **B2, a sub-pixel floor:** already dead. `design-settlement-lod-bias`:
  "ruled out: the screen-size half of FUN_1442B3FC0 as the settlement lever,
  because at 8 pixels it removes 613 of 18,267 pool eye draws (0.21 ms)". A
  2-3 pixel floor removes fewer.
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
`src\d3d11\native_on_foot.cpp` and `src\d3d11\native_on_foot_f87.h` that
solve the coverage problem on **A1 (`streamUnmapCopy`)** without shrinking
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
   window widths:**
   The check above rightly notes that `kF85Entries = 8192` and `kWinEntries =
   4096` were sized so 6-14k layout shifts between passes do not miss and
   flicker (F87c). However, inspecting `streamUnmapCopy`
   (`src\d3d11\native_on_foot.cpp:29568-29624`), `streamEntryX`
   (`native_on_foot.cpp:29662-29690`), and `planWindows`
   (`src\d3d11\native_on_foot_f87.h:341-388`) shows why we are reading
   **458 KB/frame** (`229 KB x 2`) of write-combined memory when a single
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
     - Worse: when `total < kF85Entries` or when clusters sit within 2,048-6,000
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
this section. Code references are at `639904db` (build 2, `277fae1f`, plus
docs only). Two logs, both from build `277fae1f` (`version
v0.22.0-rc.4-50-g277fae1f` on line 2 of each), in the Steam game's
`edvr_logs\`:
- **L1** = `edvr_gfx_20261103_095211.log`, the unprofiled windows (`--nth 1`).
- **L2** = `edvr_gfx_20261103_112412.log`, the profiled flight (`--nth 0`).

To reproduce a log figure, run `Select-String` on the log for the quoted
prefix (`marker fix (F87c) eye`, `draw mask:`, `instance stream`), or use
`python tools\edvr_log.py --target steam --nth N`.

#### A1-a: two copies from one Unmap (agreed)

- **Claim:** each Unmap of the stream copies both eyes' plans from the same
  source pointer.
  **Evidence:** `native_on_foot.cpp:29569` takes `base` once; the loop at
  29591 runs `e = 0, 1`; 29601 copies `base + plan.lo[w]` into
  `g_streamWin[e]`. `++ss.copies` at 29614 is inside that loop, so each eye's
  `copies` counts the same Unmaps.
- **Claim:** there is one stream Unmap a frame, so each eye's `copies` per
  second is the frame rate.
  **Evidence:** L2's `draw mask:` windows ending 14400 (11:26:33.351) and
  16200 (11:27:36.972) are 1800 frames in 63.6 s, 28.3 frames a second. L2's
  F87c lines over the same minute show 268-299 copies per eye per 10 s.
- **Claim:** the lookup does not care which eye's buffer holds an entry.
  **Evidence:** `streamEntryX`, `native_on_foot.cpp:29665-29673`, loops over
  both plans and returns the first hit, whatever `eye` is.

#### A1-b: windows overlap within one eye (agreed as possible; size unknown)

- **Claim:** `planWindows` neither merges nor trims windows.
  **Evidence:** `native_on_foot_f87.h:364` skips a cluster only when `[a, b]`
  lies wholly in an earlier window. 372-373 place a full `kWinEntries`
  window about the cluster's middle, with no check against the others. The
  F85 window at 344-350 is placed the same way.
- **Claim:** two history windows can share up to about 2000 entries.
  **Evidence:** `ClusterHist::add` (`native_on_foot_f87.h:263`) joins an
  entry to an interval within `kWinGap` (2048). So two live intervals are
  more than 2048 apart, and two 4096 windows centred on single-entry
  clusters 2049 apart share 4096 - 2049 = 2047 entries.
- **Claim:** every planned window is copied at full width.
  **Evidence:** L2 11:26:34, eye 0: `windows 5 ... 277 copies, 54460416
  bytes`. That is 196,608 bytes a copy = 24,576 entries at 8 bytes = 8192 +
  4 x 4096. Eye 1: `windows 7 ... 72613888 bytes` = 262,144 a copy = 32,768
  entries = 8192 + 6 x 4096, the per-eye cap (`kWinCapEntries`, f87.h:249).
- **Not known:** how much of that is overlap. No log line records where the
  windows sit. L2 logs only eye 1's ring (first/last entry, 41,261-45,583
  over the steady minute); eye 0's entries are never logged
  (`native_on_foot.cpp:34882`).

#### The fix for build 3 (agreed in principle)

Plan each eye exactly as today, then sort and merge both plans' windows and
copy each merged range once into one buffer.
- **Coverage is unchanged by construction.** The merged ranges are the union
  of both plans' ranges, copied from the same Unmap, and the lookup already
  takes any plan's hit (A1-a). So `named`, `outside`, the escalation and the
  values read are the same.
- **No key and no grilling round:** it does not change what the marker fix
  draws. Narrower windows still need the full process.
- Keep `plan[e]` per eye for `windows`, `nearest` and `clusters`, so those
  counters keep their meaning.
- Log requested entries (both plans summed) beside copied entries (the
  merge), and eye 0's ring beside eye 1's. `edvr_log.py`'s F87c parser and
  its self-test follow the line.
- **Test seam:** a rig test over random histories and centres. It asserts
  that the merged ranges hold exactly the union of both plans' entries, and
  that each lookup returns the same value as today's two-buffer path.

#### The size of the saving: not agreed, measured by build 3's flight

- **Claim:** "50-75% (0.40-0.60 ms)" has no data behind it.
  **Evidence:** the overlap it depends on is not logged (A1-b, "Not known").
- **Retracted:** my earlier line that F87c's 6-14k shift means eye 1 sits
  away from eye 0. That shift is eye 1 moving between layouts
  (`native_on_foot_f87.h:242-243`), not a distance between the eyes.
- **Claim:** the cost is linear in bytes, so the saving is (1 - copied /
  requested) of the copy's time.
  **Evidence:** L2 11:26:34: eye 0 85.514 ms / 54,460,416 bytes = 1.57 ns a
  byte; eye 1 111.057 ms / 72,613,888 bytes = 1.53 ns a byte. Eye 1 copies a
  third more bytes and takes a third more time. L1 10:02:17 gives 1.45 and
  1.49 ns a byte.
- **Claim:** the logs bound the saving only loosely. In L2 the stream is
  65,536 entries (line 1930: `524288 bytes, 65536 entries`) and today's two
  copies are 57,344 entries (24,576 + 32,768), 7/8 of it. The union is at
  most 57,344 and at least 8,192 (one F85 window; each eye's own windows may
  overlap, so neither plan alone sets a higher floor). That is a cut of
  0-86%, 0-0.61 ms at L2's 0.71 ms a copy pair ((85.514 + 111.057) ms / 277
  Unmaps). Only the build 3 figures narrow it.
- **Test:** build 3's flight. Its requested/copied entries, and
  `streamUnmapCopy` in the profile, give the saving. `outside`, `nearest`
  and `fullNamed` must stay in build 2's range: steady-state L2 shows
  `outside 0` from 11:26:34 to 11:28:15. Whatever is left is the
  narrower-window design, priced from `outside`/`nearest` as planned.

#### The escalation's double copy: ruled out

- **Claim:** while an escalation is on, the windows are copied on top of the
  whole stream. **Evidence:** `native_on_foot.cpp:29582-29590`, then the
  loop at 29591 runs regardless.
- ruled out: skipping the windows during an escalation, because escalations
  happen only on arrival. **Evidence:** L2's F87c lines show `escalated 119`
  (11:26:13) and `68` (11:26:24), then `escalated 0` in every line from
  11:26:34 to 11:28:15. L1's last full lines (10:02:17, 10:02:27) are
  `escalated 0` too.

#### B1: what the "offscreen" draws are (corrected)

- **Claim:** the `weapon_motion` skip count is not `8BC3..` draws.
  **Evidence:** `vscreen.cpp:5520-5521` counts the skip when RTV0 is not
  eye-sized (`!g_state->rtv0Eye`) **and** the may-want byte says weapon
  motion does not want this vertex shader. `weaponMotionWants`
  (`weapon_motion.cpp:186-189`) is the family test, so a skip is a draw
  *outside* the weapon family.
- **Claim:** it includes depth-only draws. **Evidence:** `rtv0Eye` is
  `targetIsEyeSized(bindingGet(BindSlot::Rtv0))` (`vscreen.cpp:2099`,
  `2227`), which returns false for a null RTV (`vscreen.cpp:1272`).
- **Claim:** the count depends heavily on the scene. **Evidence:** L1's
  steady windows (25200-32400) skip about 7.33M a window, 4,070 a frame, of
  about 18,290 full-path draws a frame (`32916372` / 1800). L2's window
  ending 16200 skips `924299`, 514 a frame, of 14,076 full-path draws a
  frame (`25336198` / 1800). Both were called the concourse.
- **Why it matters:** a depth-only pass would land in this count, and
  `engine-render-pipeline.md` found no depth pass at a parked settlement.
- **Test before any B1 design:** split those draws by RTV0 and DSV size and
  format, and by vertex shader. The existing logs cannot do it:
  `edvr_log.py --target steam --nth 1 --tally vh` prints `no draw-census
  lines (DC/DCO/DC frame) in this log`. So build 3 adds a per-window count of
  non-eye draws by target size and format. It rides on build 3's flight; it
  needs no flight of its own.

#### Numbers (corrected)

- **Claim:** "4.06M tee calls / ~180 frames" is wrong; it is about 14,400 a
  frame. **Evidence:** the 4,063,615 tee calls are from 10 s lines at
  11:26-11:27 ("Check of that review"), and L2 runs at 28.3 frames a second
  over that minute (A1-a): 4,063,615 / 283 = 14,359.
- **Claim:** the full-path baseline differs between the two logs.
  **Evidence:** 18,290 a frame in L1 and 14,076 in L2 (B1 above). Judge
  build 3 against L2, the profiled flight, and fly the same spot.

#### Agreed for build 3, if Argon accepts the above

1. The merged marker copy (no key), with the requested/copied entries and
   eye 0's ring in the F87c line, and the rig test.
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
- **B1 correction and instrument accepted:** verified at `src\d3d11\vscreen.cpp:5520-5521`
  and `src\d3d11\binding_shadow.h:76-82`. `vsMaySkip(bindingVsMayWant(), kVsMayWeaponMotion)`
  is `(mayWant & bit) == 0`, so `maskNoteSkip(MaskSkip::WeaponMotion)` counts
  non-eye draws (`!g_state->rtv0Eye`, including `Rtv0 == nullptr` depth-only
  draws) whose vertex shader is *outside* `weaponMotionWants` (not `8BC3..`).
  Adding the per-window non-eye draw counter by RTV0/DSV size and format in
  build 3 is the exact right way to answer B1 on the same flight.
- **Numbers baseline accepted:** `4,063,615 / 283 = 14,359` tee calls a frame
  in L2, and L2 (`14,076` full-path draws/frame, `0.71 ms` `streamUnmapCopy` at
  `Shamash 1 a`) is the reference baseline for build 3.

#### One capacity guard for build 3's merged `WinPlan`

- **Claim:** the merged window plan cannot reuse the single-eye `kWinMax = 7` /
  `kWinCapEntries = 32,768` bounds without risking silent truncation when Eye 0
  and Eye 1 have disjoint windows; it needs `2 * kWinMax` (`14`) windows and
  `2 * kWinCapEntries` (`65,536`) entries.
- **Evidence:** in `src\d3d11\native_on_foot_f87.h:247-249`, a single eye's
  `WinPlan` holds up to `kWinMax = 1 + kHistCap = 7` windows and
  `kWinCapEntries = kActiveEntries + kHistCap * kHistEntries = 8,192 + 6 * 4,096 = 32,768`
  entries (`256 KB`). In `src\d3d11\native_on_foot.cpp:29601`,
  `g_streamWin[2][f87::kWinCapEntries * 2]` already allocates `65,536` entries
  (`512 KB`, 2 floats per entry) across the two eyes. If Eye 0 and Eye 1 have
  zero overlap (the `57,344`-entry worst case in L2, or `65,536` when both eyes
  carry 6 history windows), the union of `plan[0]` and `plan[1]` has up to
  `14` disjoint intervals and `65,536` entries. Sizing the merged plan for `14`
  windows over a single flat `g_streamWin[2 * f87::kWinCapEntries * 2]` buffer
  uses the exact same `512 KB` static footprint as today while guaranteeing
  zero coverage loss even when the two eyes' window sets are completely
  disjoint. Keep per-eye `plan[0]` and `plan[1]` (or their interval lists) for
  `nearest`'s per-eye `plan[e].distance(entry)` check (`native_on_foot.cpp:34092`),
  while `streamEntryX` resolves `where(entry)` in the merged plan.

#### Code evidence for the post-build-3 A3 pass (`hookedMap` / `hookedUnmap`)

While checking the map/unmap path (`0.58 + 0.50 = 1.08 ms` exclusive in L2),
two concrete findings in `src\d3d11\vscreen.cpp` are worth recording now for the
A3 pass after build 3:

1. **Three flat-profile `mapBounce` calls in `hookedMap` and `hookedUnmap` are
   left ungated in VR:**
   - **Evidence:** at `src\d3d11\vscreen.cpp:3511`, `flatRuntimeMapBounceNoteMap`
     is gated with `if (!runtimeVrProfile())`, with the comment on lines
     `3509-3510` noting that the VR profile should not pay the call across to
     `flat_runtime.cpp`. However, three sibling calls in the same hooks lack
     `!runtimeVrProfile()`:
     - `vscreen.cpp:3451`: `flatRuntimeMapBouncePreMap(res);` at the top of
       `hookedMap`.
     - `vscreen.cpp:3512`: `if (mapData0) mapped->pData = flatRuntimeMapBounceInstall(self, res, sub, type, mapped->pData);`
       immediately below the gated line `3511`.
     - `vscreen.cpp:3638`: `auto bounceLease = flatRuntimeMapBounceBeginUnmap(self, res);`
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
