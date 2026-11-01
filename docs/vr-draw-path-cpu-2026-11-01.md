# VR draw-path CPU cost (2026-11-01)

## Status

- **State (2026-11-01): built (tickets #158-#163 merged), not flown.**
  Next: one flight with test build 1. Writing the spec corrected three points
  here: item 1's rig and item 3's two wrappers (see those sections). The
  build's code review (findings fixed on `build1/review-fixes`) moved the
  re-issue counter's edges: see that section.
- **Question:** how much of a CPU-bound VR frame is EDVR's draw path
  (GLOSSARY: Draw path), and how much can go without changing what reaches
  the screen?
- **Earlier investigation:** `docs\stereo-cpu-sharing-2026-10-16.md` (the
  2026-10-31 flight, H5-H7, and the 2026-11-01 CPU profile).
- **Test build 1:** item 1 (scene depth without AA: world rows and the
  marker tee's fast path; changes behaviour, #155), item 2 (benchmark
  collector), item 3 (two early exits inline), item 4 (depth slot cache),
  and a re-issue counter (GLOSSARY: Re-issue).
- **Temporary keys (never shipped; removed when this closes):**
  `experimental.vr_eye_rows_without_aa`, `experimental.vr_cpu_benchmark_idle`,
  `experimental.vr_cpu_depth_slot_cache`.
- **Open hypothesis (item 1):** with AA off the depth probe is off, no draw
  passes the scene-depth test, `worldOk` never sets, so the tee never goes
  fast and no world rows publish. Signature: in keys-off AA-off legs the
  tally names `world` and `sceneRefused[NotSceneDepth]` is large.
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
