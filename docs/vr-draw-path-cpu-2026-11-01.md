# VR draw-path CPU cost (2026-11-01)

## Status

- **State (2026-11-01): design agreed, not built.** This doc is the design
  for test build 1. A design review comes next, then the spec, then the
  build and one flight. Nothing here is implemented.
- **Question:** how much of a CPU-bound VR frame is EDVR's own draw path
  (GLOSSARY: Draw path), and how much of it can be removed without changing
  what reaches the screen?
- **Earlier investigation:** `docs\stereo-cpu-sharing-2026-10-16.md`. It
  holds the measurements this design rests on: the 2026-10-31 flight
  (H5-H7) and the 2026-11-01 CPU profile. Its numbers are not repeated
  here beyond what each item needs.
- **Test build 1 carries:** item 1 (world rows and the tee's fast path with
  AA off; changes behaviour, #155),
  item 2 (benchmark collector cleared only when used), item 3 (two
  early-exit tests moved inline), item 4 (depth surface slot cached), and a
  re-issue counter (GLOSSARY: Re-issue). Only item 1 changes behaviour.
- **Temporary keys (remove when this investigation closes; never shipped):**
  `experimental.vr_eye_rows_without_aa`, `experimental.vr_cpu_benchmark_idle`,
  `experimental.vr_cpu_depth_slot_cache`.
- **Left out, tracked:** #151 (`drawPathFast` in
  `hookedDrawIndexedInstanced`), #152 (per-scene subscriber mask so the draw
  gate can close in VR), #153 (cross-profile per-bind counter, was item 5),
  #154 (`/GL /LTCG`). These follow build 1.
- **Next flight:** "Flight plan" below.
- **Ruled out:** idling the whole marker tee while the UI layer is off,
  because its slow path is the only publisher of the per-eye world rows
  (`eyeRowsPublish`, native_on_foot.cpp 11870; called only from `worldDraw`
  and `rowSourceDraw`), which ADS aim, scope zoom, the head-containment grid
  and other on-foot systems read without the layer. Earlier ruled-out
  items are in the earlier doc.
- **Open hypothesis (item 1, #155):** with AA off the depth probe is off,
  no draw gets an eye, `worldOk` never sets, so the tee never takes its fast
  path and no world rows are published. Signature: the new tally names
  `world 0` / `world 1` in the keys-off AA-off legs.
- **Next step:** design review (Fable), then the spec.

## Where the time goes (summary of the evidence)

Rig: PSVR2 through SteamVR OpenXR, 2632x2684 per eye, 90 Hz, RTX 5070 Ti,
Ryzen 7 5700X3D, EDHM chained, Motion Smoothing off, game build 334925.
The test spot is a station concourse, chosen because it has the most draws
(13-14k eye draws a frame); nothing below is specific to it.

- The rig is CPU-bound: `held_by=game` 96%, vendor xrEndFrame about 0.36 ms.
- Concourse standing (log `edvr_gfx_20261031_225516`, build
  v0.22.0-rc.4-1-ge1606a23), twice over: DLAA 29.6 fps with `draw hook CPU`
  5.8-6.1 ms a frame; F7 (AA off) 34.7 fps with 2.7-3.5 ms.
- The draw gate (GLOSSARY: Draw gate) never closes in VR: default-on
  features hold it open, so even with AA off every draw takes the slow path.
- Profile (v0.22.0-rc.4-3-g6ac3c0e5, `EDVR_PROFILE_SYMBOLS=1`): EDVR's own
  code is about a quarter of the busy render thread in both AA modes. About
  3.6 ms of the 5.8 ms the hooks read with AA on is not EDVR's own code;
  EDVR's re-issues are the likely owner (inferred, unmeasured).
- The marker-probe tee loses its fast path with AA off (`native on foot
  marker probe: cost ... fast path 0 of N`, about 16.5k tee calls a frame):
  0.47-0.56 ms a frame with AA on, 1.1-1.5 with AA off.

## Rules for every item

- **No change to what reaches the screen.** Each item removes work whose
  result is discarded or recomputed identically.
- **Nothing tuned to one headset.** No constant depends on the headset,
  render size or runtime; anything that varies is read at runtime.
- **Each item is its own commit**, so one can be reverted after the flight.
- **Each item shows it ran.** A counter or log line distinguishes "ran and
  saved" from "never ran". A missing line must not read as success.
- **Pure logic gets a red/green rig test first** (item 1's eye fallback,
  items 2 and 4). Item 3 and item 1's draw-path wiring are shown by review,
  their counters and the flight's checks.

## Item 1: world rows and the tee's fast path with AA off

**Current behaviour.** Each eye draw on foot calls `nativeOnFootMarkerTee`
when the marker probe is live (`nativeOnFootMarkerProbeLive() && self ==
ownerCtx`, vscreen.cpp around 5339 and 5384, and the Draw and DrawIndexed
equivalents). Its fast path needs both eyes' world rows, which the UI layer
supplies; without the layer every call takes the out-of-line slow path
(`nativeOnFootMarkerProbeDraw`, native_on_foot.cpp around 35625).

**Why it is waste.** The tee exists to feed the marker fix, and the fix can
only apply inside the layer's bracket: `markerFixed` (vscreen.cpp:4456, its
only write) requires `layered`, and its only reader is the end call at
vscreen.cpp:4505. The condition is "the UI layer is live" (`uiLayerLive()`),
not "`fix.ui_quality` is live": the layer can also be live for weapon fire
or ripple alone.

**What else the tee feeds (checked 2026-11-01).** The tee cannot idle as a
whole. Its slow path also publishes the per-eye world rows that on-foot
systems read without the layer: `eyeRowsPublish` (native_on_foot.cpp
11870, called only from `worldDraw` and `rowSourceDraw`) feeds ADS aim and
sight (12405, 12698, 12865), scope zoom (36206), `f109TurnTick` (8383),
`f87EyeLines`, `pinCycloUpdate` and `geometryNow`; `g_f85SceneW/H` feeds
the head-containment depth grid. Its frame turn samples head motion.
Marker-only outputs: `markerDraw`, `hudDraw`, `labelIssue` (writes only the
eye-dump record) and `g_markerCropNdc` (eye-dump only). The draw serial has
no reader outside the marker and label code. `labelIssue` matches on the
serial, so it must share the tee's gate.

**Cause (code reading, supported by the log; #155).** The fast frame is
published only when `teeFastFrame` (native_on_foot_f100.h:39-42) sees
`!measure && !pending && !bound && !census && !check`, the frame polled,
and `worldOk[0]` and `worldOk[1]` both set this frame. With AA off the depth
probe is not wanted (`fix.temporal_aa != off || advanced.eye_depth_capture`,
depth_probe.cpp:524-525), so `eyeOfDraw` (native_on_foot.cpp:28582-28592)
returns -1 for every draw, `worldDraw` and `rowSourceDraw` never set
`worldOk`, and `f44::draw` never early-returns. The same gap means no world
rows are published with AA off. The 10-31 log agrees: its F113 `eye rows`
line shows 349-635 frames a window with row source `none` and 80k-156k draws
with no scene-depth eye with AA off, against about 0 and 5-8k with DLAA. The
other terms were checked and clear within one draw (`pending`, `bound`) or
do not depend on AA (`measure`, `census`, `check`, `polledFrame`).

**Change, in two parts.**
1. **Tally (always on, no key).** Where the slow path tries to publish the
   fast frame (native_on_foot.cpp around 35664), count the first failing
   term (measure, pending, bound, census, check, polled, world 0, world 1).
   Print it on the tee's cost line after the `over %u s` text, reset with
   the fast-call count, and extend the `edvr_log.py` parse at about 16778.
   This confirms or refutes the cause in the keys-off legs.
2. **Fix (keyed).** When the depth probe is not wanted, `eyeOfDraw` falls
   back to the eye from the depth view's size and order: the shape rule
   `targetEye` already applies (native_on_foot.cpp 29276-29286). CPU only,
   read from the game's own targets at runtime, nothing per headset. With
   the probe wanted, nothing changes.

**This item changes behaviour.** With AA off and the key on, the on-foot
systems that read the world rows (ADS aim and sight, scope zoom, the
head-containment grid, `f109TurnTick`, `f87EyeLines`, `pinCycloUpdate`,
`geometryNow`) start receiving rows they do not get today. It is the one
exception to "no change to what reaches the screen", and the reason for its
own key. A separate line of work on on-foot weapons reads the same rows; the
two will meet at merge.

**Why it is safe to fly.** Key off restores today's eye assignment exactly.
A wrong shape-rule eye would show as rows from the wrong eye; the tally,
the F113 `eye rows` sources and the flight's checks would show it.

**Key.** `experimental.vr_eye_rows_without_aa`.

**Shows it ran.** With the key on and AA off: the F113 `eye rows` source is
no longer `none`, and the cost line's `fast path X of N` is well above 0.
With the key off: the tally names `world 0` / `world 1` as the blocker. If
it names another term, the cause is refuted; write it down and stop.

**Test.** The fallback's eye choice is a pure function of the depth views'
sizes and order: a rig test in `tools\native_on_foot_hooks_test` (red
first) covers two eye-sized views in both orders, a non-eye-sized view, and
the probe-wanted case, which must not use the fallback.

**Expected.** 1.1-1.5 ms a frame with AA off in the concourse; none with AA
on, where the probe already supplies the eye.

## Item 2: the benchmark collector is cleared only when used

**Current behaviour.** `NativeBenchmarkCollector` (perf monitor state,
perf_monitor.cpp:174) holds 16384 entries plus orphans
(native_benchmark_collector.h:81, 136). `reset()` zeroes all of them
(`clearEntries()`, native_benchmark_collector.cpp:34) on every perf tick
where native perf history is not active (perf_monitor.cpp:1119, 1185), even
when nothing has been written since the last clear. The profile prices it at
0.10-0.14 ms a frame.

**Change.** The collector records whether anything has been written since
its last clear. Every writer (`observe`, `addCpu`, `addGpu`) marks it used;
`clearEntries()` marks it clean. `reset()` on a clean collector skips the
zeroing. `begin()` keeps clearing unconditionally.

**Why it is safe.** A clean collector is already all zero; skipping the
clear leaves the same state. A stale key read can only make it clear when it
need not.

**Key.** `experimental.vr_cpu_benchmark_idle` (`off` clears every tick, as
today).

**Shows it ran.** The perf line gains clears done and clears skipped per
window.

**Test (red first), `tools\native_perf_history_test`.** A clean collector's
`reset()` does no clear. After one `observe`, `reset()` clears and every
entry reads zero. After `addCpu` or `addGpu` alone, the same. With the key
off, every `reset()` clears.

**Expected.** 0.10-0.14 ms a frame, in every VR and flat scene where native
perf history is inactive.

## Item 3: two early-exit tests move inline

The build has no `/GL` or `/LTCG` (build.bat around 350), so a call into
another source file is never inlined, even when it returns at once. Of the
calls the profile named, most are already guarded inline or change state
before their first exit (`screenMotion*Draw`, `starGlowAfterDraw` and
`gpuCensusBegin`: see the earlier doc and #154). Two are worth moving:

- **`nativeOnFootLabelIssue`** (native_on_foot.cpp around 30494, called at
  vscreen.cpp:4457). Its exit is `!g_labPending.valid || g_labPending.serial
  != g_drawSerial || !g_labPending.d`, all file-local state. Change: expose
  the test as an inline function in the header and call the out-of-line body
  only when it passes.
- **`uiLayerCrispNoteEyeDraw`** (ui_layer.cpp:5373, called at
  vscreen.cpp:2537 and flat_ui_layer.cpp:371). It writes `g_crispPending`
  and `g_uiLayerCrispPending` first, then exits on `!g_uiLayerCrispOn ||
  !ctx`. Change: an inline wrapper does the same two writes, tests
  `detail::g_uiLayerCrispOn` (already extern, ui_layer.h:86) and `ctx`, and
  calls the body only when both pass. The writes stay in the same order.

**Why it is safe.** The same tests, in the same order, on the same state;
only where they are evaluated moves.

**Key.** None: a key would add a branch to the path this item slims, and
the change always helps.

**Shows it ran.** The profile's next capture, if one is taken; otherwise
the total `draw hook CPU`. Not separately measurable in one flight.

**Expected.** About 0.1 ms a frame with AA on.

## Item 4: the depth surface slot is cached

**Current behaviour.** `uiDepthSampledSurfaceSlot` (ui_depth.cpp:2419)
answers "which of pixel shader resources 0-3 is a surface EDVR knows?" by
calling `viewIsSurface` on each, on every qualifying eye draw (vscreen.cpp
4108 for scope marks and 4119 for learned surfaces). `viewIsSurface` alone
is 0.10 ms a frame with AA on.

**Change.** Remember the last answer with the inputs that produced it, and
return it while all of these are unchanged:
- the bind generations of PsSrv0..3 (`bindingGeneration`, binding_shadow.h
  138-153, bumped by `bindingSet` from `hookedPSSetShaderResources`);
- a new surface-set generation, bumped wherever a surface is learned or
  forgotten;
- `g_uiDepthOn` and `g_uiDepthStoodDown`;
- the key's value.

**Why it is safe.** The answer is a pure function of those inputs. The risk
is a missed input: any other place that changes which views count as
surfaces must bump the new generation. The spec must list every writer of
the surface set; the review should check that list.

**Key.** `experimental.vr_cpu_depth_slot_cache`. A change of the key clears
the cache.

**Shows it ran.** Cache hits and misses per perf window.

**Test (red first), `tools\ui_depth_test`.** A repeat call with nothing
changed is a hit and returns the same slot. Each input above, changed alone,
forces a miss and a fresh answer. With the key off, every call recomputes.

**Expected.** 0.1-0.2 ms a frame with AA on, in any scene with UI depth.

## The re-issue counter

Not a saving: it names the owner of the unexplained ~3.6 ms. Every re-issue
goes through `pureDrawReissue` (vscreen.cpp:3908) or the two paths built on
it, `crispHudTonemapReissue` (4512) and `worldScreenReissue` (4518). Count
calls and QPC time inside `pureDrawReissue`, split by caller: star glow,
blend probe, crisp HUD, world-screen re-issue, `uiLayerSecondIssues`,
tonemap. Logged as `draw reissue CPU:` beside `draw hook CPU:`
(perf_monitor.cpp:1216), with the same 1-in-64 sampling and 1800-frame
window. No key.

Trace it end to end before flying: with no re-issues the line must say 0,
not be absent; with the counter broken it must not say 0.

## The temporary keys

- In `[experimental]`, each shipped `on` in the test build; `off` restores
  today's behaviour exactly.
- Each has a `# TEMPORARY` comment block that says it is live and is removed
  when this investigation closes. No `# dev: hidden`, so each appears on the
  F8 menu's Experimental page under `menu.developer = on`.
- Read once per config sweep into a relaxed atomic the draw path reads (the
  `g_aaOffDraw` pattern, vscreen.cpp:6162). A stale read may only fall back
  to today's behaviour.
- Every change logs one line with the key and its new value, so
  `edvr_log.py` can split a leg at the flip.
- When this investigation closes the winning items become plain behaviour
  and the keys are deleted. They are never in a release.

## Flight plan

One session, no game restart; keys flipped from the F8 menu. Legs are 3
minutes (the perf lines print every 1800 frames, about a minute at 30 fps).
Standing at the same concourse spot as 2026-10-31:

1. DLAA, all keys on; then DLAA, all keys off.
2. F7 (AA off), all keys on; then F7, all keys off.
3. Repeat 1 and 2.
4. F7, only `vr_eye_rows_without_aa` off.
5. A short cockpit leg, all keys on: no-regression check only.

Weapons cannot be drawn in the concourse, so this flight cannot check ADS
aim or scope zoom with item 1 on. That check waits for a flight where
weapons can be drawn, or for the on-foot weapons work; until then item 1's
behaviour change is unverified on screen.

Before reading any number: `python tools\edvr_log.py --target steam
--expect-build HEAD --version` must pass, then `--health`.

Visual checks during the legs: on-foot markers and labels correct with DLAA,
and across an F7 toggle and back; HUD and UI depth correct.

## Pass criteria

- The log is from this build.
- `draw hook CPU` falls by at least 0.4 ms a frame with DLAA and at least
  1.0 ms with AA off, against the keys-off legs and against 2026-10-31.
- fps does not fall in any leg.
- Every item's counter shows it ran, and `draw reissue CPU:` is present.
- The keys-off AA-off legs' tally names `world 0` / `world 1` (else item
  1's cause is refuted), and the keys-on AA-off legs show F113 row sources
  other than `none`.
- No visual regression on the checks above.

If the result lands within noise, fly a fresh baseline of the previous
build before concluding.
