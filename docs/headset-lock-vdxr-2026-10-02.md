# Headset locks on foot while the game drops to 10 fps (Virtual Desktop, v0.18.0)

## Status

*Written 2026-10-02; lock 2 added 2026-10-03. Update whenever this doc changes.*

- **State:** two locks read; lock 2 was caught by the new instruments (v0.18.1 ships
  them). Both are a hard 10 Hz limit inside the vendor runtime's `xrEndFrame`. No fix.
  Next: the user's on-foot flight with `fix.weapon_stability = 0`.
- **Report:** on foot near the ship, the headset image locks while the game carries on at
  a much lower frame rate on the mirror. Quest 3 through VDXR 1.0.10 at 72 Hz, RTX 4070 Ti
  12 GB, route and maps at their defaults, `fix.weapon_stability = 1`. Lock 1: v0.18.0,
  2026-10-02 11:14:38, HMD Quality 0.50, a heavy scene sliding from 72 to 26 fps. Lock 2:
  v0.18.1, 2026-10-03 08:59:22, HMD Quality 0.75, `openxr_resolution` 2100 wide, a light
  scene at a flat 72 fps, 21 s after disembarking.
- **What the logs show:** a step into exactly 10 fps until the user quits. Over 174
  consecutive frames of both locks the gap between `xrEndFrame` returns never fell under
  99.8 ms; the hold is 100 ms minus the game's own work (r = -0.92) and drops to 1.1 ms
  after a 141.6 ms game stall. That is a 100 ms timer or fallback in the vendor, not a
  throughput limit. Lock 2: our copy 0.43 ms, our work 0.11 ms, swapchain calls about 0,
  session FOCUSED, no vendor event, VRAM 74% of the budget. Both locks: on foot, deferred
  pacing with a synchronous end frame, the route owning.
- **Ruled out:**
  - ruled out: a stale route or maps panel, because the route treated every frame of lock
    1 and a stale panel cannot lower the game's own Present rate;
  - ruled out: auto-fit churn, because the width was explicit and every size constant;
  - ruled out: EDVR blocking the render thread, because its own time stayed 0.1-0.35 ms a
    frame with no file I/O, lock or fence wait;
  - ruled out: device loss, because no DXGI, removal or XR-loss line exists and both exits
    were orderly;
  - ruled out: VD holding our swapchain images, and EDVR's 100 ms handoff timeout, because
    lock 2's acquire, wait and release took about 0 ms, the copy 0.23 ms, and the hold sits
    inside `xrEndFrame` (lock 1's copy p95 of 75.6 ms was not the hold);
  - ruled out: graphics memory pressure in the game's process, because lock 2 peaked at
    74% of the budget with nothing demoted and no jump (VD's own process is not in it);
  - ruled out: a session state or focus change, because VD sent no event around lock 2;
  - ruled out: a heavy scene as the trigger, because lock 2 came at a flat 72 fps;
  - ruled out: `XR_EXT_performance_settings` as a lever, because VDXR does not offer it.
- **Open:** (A) VD's stream backed up (encoder, network or headset decoder) and VDXR paces
  the app at 10 Hz; (C) deferred pacing with VDXR: 2 of 2 locks under it over about 9.5
  min of deferred pacing, 0 in about 31 min of runtime pacing, confounded with the route;
  (D) the headset or link stopped consuming frames and VDXR keeps the app alive at 10 Hz.
- **Next:** the user, 20-30 min on foot with `fix.weapon_stability = 0` (live, F8), nothing
  else changed. A lock with `pacing=runtime path=overlapped` refutes (C), and the route
  goes off next; no lock supports (C) (flip it back to confirm), and EDVR would own a
  mitigation. If it locks: don't quit; note whether the picture is head- or world-locked,
  whether VD's menu answers, whether it recovers. Ask for VD's versions, codec, bitrate,
  Synchronous Spacewarp and the performance overlay.
- **Reader fixes, not built:** lock 2's STOP (runtime cycle 56969, 320 ms, no FREEZE line)
  is a false alarm: 95 ms of it was vendor-held before the Present, so the Present gap was
  about 231 ms; expect a FREEZE only when the cycle minus the second submit reaches 250 ms.
  The startup WARN (6 fps for 7 s) was a load the detector called `scene` by frame count
  (5.0 of its 7.0 s had no layers); weight the context by time.
- **Instruments (section below):** on main 521d625b, shipped in v0.18.1: `vram:` lines,
  vendor events, long `xrEndFrame` episodes, slow regimes with their owner, VRAM and
  context; test key `advanced.slow_test_ms`.

## The instruments (2026-10-02)

Nothing here changes what the game draws, how frames are paced or when the runtime calls
the vendor, except the test hold. Each line is written by the half named; the reader is
`python tools\edvr_log.py --target steam --freezes`.

1. **Graphics memory** (graphics log, `src\common\vram_watch.h`, `src\d3d11\vram_tick.cpp`,
   one call in the Present hook, VR and flat alike). `vram: reason=armed|periodic|pressure|
   over_budget|back_under_budget|cap_reached local_used_mb=7421 local_budget_mb=10863
   local_pct=68.3 nonlocal_used_mb=316 nonlocal_budget_mb=16311 nonlocal_pct=1.9`. `local`
   is the card's own memory, `nonlocal` the system memory the process uses, both against
   the budget the OS gives it. Sampled once a second (about 1 us); written every 30 s,
   every 5 s from 90% of the local budget, and at each crossing of the budget. A stack
   without `IDXGIAdapter3` (DXVK, Wine) writes `vram: unavailable -- <why>` once and never
   asks again.
2. **Vendor events** (runtime log, `vendor_events.h`, fed by `SessionState`'s observer).
   `native_xr_events,armed=1,...`; `native_xr_event,n=6,type=session_state_changed,
   from=FOCUSED,to=VISIBLE,event_time=...,lag_ms=1.250` (also events_lost,
   instance_loss_pending, reference_space_change_pending, interaction_profile_changed,
   visibility_mask_changed, display_refresh_rate_changed, perf_settings, and an undecoded
   type once by number); at most 200 lines, then a notice; `native_xr_events_summary` at
   close and every 5 min.
3. **End-frame episodes** (runtime log, `end_frame_episodes.h`, from the boundary's
   `FrameSink::endFrameReturned`, every xrEndFrame the runtime makes: synchronous,
   overlapped, loading, empty). An episode is calls of 3 display periods or more.
   `native_end_frame_episode,episode=1,sequence=96600,ms=83.1000,periods=5.98,...,path=
   synchronous,pacing=runtime,...,session_state=FOCUSED,state_age_ms=...` at the first
   slow call (one start line per 2 s, 100 a session), `native_end_frame_episode_end,...,
   calls,slow_calls,duration_ms,p50_ms,max_ms,...` when 8 normal calls follow or the
   session closes (always for an episode of 20 slow calls or more).
4. **Slow regime** (runtime log, `slow_regime.h`). One-second buckets of frames reaching
   xrEndFrame; a bucket under 40% of the display rate (the vendor's half-rate mode is 50%,
   not slow; 10 fps at 72 Hz is 14%) is slow; 5 in a row begin a regime. `native_slow_
   regime,event=SLOW,regime=1,duration_s=5.0,...,fps=9.97,display_hz=72.00,...,held_by=
   vendor_end_frame,held_share=0.831,vendor_end_frame_ms=83.10,...,vram_local_used_mb=...,
   summary=held by the vendor runtime's xrEndFrame: 83.1 ms of every 100.3 ms frame
   (83%)`; `still_slow` every 30 s; `end` 2 normal seconds after (`reason=recovered`, or
   `session_close`). Owners: vendor_end_frame, vendor_wait_frame (pose wait and the
   deferred pacer's block), vendor_swapchain, edvr_copy, edvr_work, game (the residual);
   named only at 35% of a frame or more. Each line also says what the frames were:
   `context=scene|loading|no_layers|none,loading_frames=N,empty_frames=N` (at least half of
   the window's frames decide; `loading` is a frame the runtime made itself while the game
   loaded, `no_layers` an end with nothing for the vendor to show).
5. **Test trigger** `advanced.slow_test_ms` (0 to 500, default 0, read once at the first
   frame). 90 s into the session the d3d11 half asks (frame_flag layout v36,
   `requestEndFrameHold`) for every xrEndFrame to be held that many ms longer, inside the
   timed region, for 40 s; the runtime logs `native_end_frame_hold,test=1,state=began|
   ended`, the graphics log `slow test:` lines. From outside it is a vendor that stalls in
   xrEndFrame, so one flight shows episodes, SLOW, still_slow and end (80 ms gives about
   10 fps at 72 Hz). It is a second key beside `freeze_test_ms` because that one is one
   render-thread sleep at 60 s, and one number cannot say a 40 s sustained hold.

**Costs.** The runtime now measures the submit phases on every frame. Before, the 30 s
window (256 samples) stopped the measuring once full: at 72 fps that is after 3.6 s, so
about 26 of every 30 s went unmeasured, and a regime that began in them would have reached
its first SLOW line with no per-owner figures. Measuring always is about 38 more
`QueryPerformanceCounter` reads a frame, about a microsecond, in those 26 s of every 30
(the window itself still takes only its 256). The VRAM query is about 1 us once a second
(1.1 us for both segment groups on the software adapter). `frame_flag` moves to layout v36
(one `endFrameHold` field), so a v35 graphics half and a v36 runtime refuse each other's
channel, as for every layout change.

**Reading the user's v0.18.0 bundle** (not committed): `--freezes` finds one regime,
`11:14:38.108 to 11:15:35.238 local (57.1 s)`, 9.94 fps from the runtime sequence (96599
to 97167) and from the graphics frame counter, held by the vendor runtime's xrEndFrame
(window 50, sequences 96730-96985, `xr_end_frame` p50 82.7 ms = 6.0 periods), EDVR's copy
p95 75.6 ms on the GPU against 0.04 before. The FREEZE at 11:15:41.4 is 6.2 s later and
its own. The method: marks (LONG FRAME and `native_long_cycle` lines of 3 periods or more,
FREEZE lines) within 5.5 s of each other and slow `vScreen totals` windows are one run; a
run of 5 s or more whose frame rate, the rise of the runtime sequence over its time, is
under 40% of the display's is a regime.

**Who may set SLOW (2026-10-02).** `--freezes` sets SLOW, and exit 4, only for a regime the
vendor runtime (vendor_end_frame, vendor_wait_frame, vendor_swapchain), EDVR's copy or
EDVR's own work holds. A regime the game's own frame holds is INFO when the runtime says its
frames were a load (`context=loading` or `no_layers`) and WARN when they were the game's
scenes (a GPU- or CPU-bound stretch) or the line carries no `context=`; one with no named
owner (none, no_frames, or unknown in a reconstruction) is a WARN. The verdict line counts
the others (`[+N slow stretch(es) ...: not a SLOW]`), so a game-owned regime is never hidden
behind a bare PASS. The reconstruction takes the rate from the runtime's sequence: the game
Presents up to 560 a second in menus while the runtime's frames stay at the display's rate.

**Census of normal sessions (2026-10-02).** `--freezes` over the 12 newest logs of Sean's
Frontier install (2026-10-01 09:20 to 16:27, builds v0.18.0-rc.5-37 to v0.18.0-4-gab90305c,
90 Hz): 0 regimes of any owner, so none from a session-start load, hyperspace or a station
load. The lowest average frame rate over any stretch of 5 s or more (the runtime sequence
between the log's lines) was 40.2 fps, 45% of the display's, for 5.1 s at 11:28:40 in the
11:24 session; the next were 51.7 and 53.3 fps. Those events show as isolated freezes (2 to 6
FREEZE lines a session) and as runs of long frames whose average rate stayed at 62 to 88 fps.
Five of the twelve (rc.5-37 to rc.5-51) predate the freeze logging and exit 3 with no regime.
The native detector counts 1 s buckets, so it can only be stricter than that average by a
stretch that is slow in every second, which none was.

**Not done.** Which of (A) a backed-up stream, (B) memory pressure or (C) deferred pacing
holds VDXR is still open: the new lines separate them in the next flight (the VRAM figures
for B, the episode's `pacing=` and the events for C, the xrEndFrame time against the
session state for A), they do not decide it. Nothing was changed to avoid the regime.

## Evidence

Bundle `edvr-logs-20261002-111610.zip`, v0.18.0 (build 6ABED11D). The graphics log runs
10:50:12 to 11:15:42 local; the runtime log stamps UTC, one hour behind.

- 10:59:03.6 to 11:04:03.1, on foot: the route owned the world at 72 fps, and the boarding
  at 11:04 worked (the route TAKES, a 468 ms game-side hitch).
- 11:13:19 disembark; FREEZE lines of 337, 275 and 263 ms, all game waits. 11:13:22.098
  the route OWNS; the runtime logs `native_pacing,mode=deferred`.
- 11:13:20 to 11:14:11: VDXR's predicted display period switched to 27.78 ms (half rate)
  in 11 stretches. The 20 s frame rates read 72, 54, 45, 55, 49, 54, 33, 41 and 26 in a
  heavy scene, with the game's GPU time at 13 to 22 ms against a 13.9 ms budget.
- 11:14:37.979, the last healthy route window: `frames=343 ... treated=343`.
- 11:14:38.108, the onset: `LONG FRAME -- 100.9 ms`, 0.08 ms of it in EDVR's Present hook.
  The runtime's `native_long_cycle` at 11:14:38.116 reads 101.9 ms with
  `second_submit_roundtrip=86.8`. EDVR logs no event at the onset.
- 11:14:38 to 11:15:35: route windows of exactly 50 frames (`door-layer-only=100`), and
  twice `vScreen totals ... 200 frames in 20062 ms is 10 fps`. `native_submit_phases`
  `xr_end_frame=82.7130/83.8729/84.4452/84.7388`; before the lock, every window's p50 is
  6.3 ms or less. `native_producer_gpu` copy `0.1741/75.6131/78.1940/79.4299` (p50/p95/
  p99/max; normally p95 0.04, so more than one sample in twenty held the GPU 76 ms). The
  GPU census at 11:15:12 reads the game's GPU time at 34.8 ms with a frame gap p50 of 66
  ms: the GPU idles two thirds of each frame and every span runs about 3x slower, which
  follows from the wait or from memory pressure. The runtime's period stays 13.889 ms with
  `shouldRender=1`.
- 11:15:35.238, a 254.6 ms FREEZE (a game wait); 11:15:35.5 the route releases
  (`on-foot-gate-lost`); 10:15:40.899Z, an orderly shutdown from the game's main thread.
  The user quit; the boarding never happened.
- Not attributed: in the slow state, every sampled frame creates a texture of about
  64.5 MiB (4096x4096 at 4 bytes). The same signature appears in the cockpit with the
  route off (11:08 to 11:11), so it is the game's.

## Evidence, lock 2 (2026-10-03, v0.18.1)

Bundle `edvr-logs-20261003-090033.zip`, v0.18.1 (build 6AC026D1), session 08:44:44 to
08:59:51 local; the runtime log stamps UTC, one hour behind. `--freezes` reads SLOW from
08:59:22.5 to 08:59:50.5, held by the vendor runtime's `xrEndFrame`.

- 08:58:39.5 to 08:58:40.7 and 08:59:00.5: VDXR's predicted period switched to 27.78 ms
  (half rate), first in a GPU-bound ship phase at about 41 fps (game GPU 16-17.6 ms), then
  in the disembark loads (game stalls of 188, 252 and 155 ms).
- 08:58:56.5 the journal's Disembark; 08:59:00.874 the route OWNS (frame 61271);
  08:59:01.040 Status.json on foot; 08:59:01.062 pacing turbo, the session's only pacing
  change. Then 72.0 fps flat, game GPU 8.6-10.4 ms, EDVR 3.56 ms a frame, the period
  steady at 13.889 ms.
- 08:59:22.007, the onset: a runtime cycle of 102.1 ms with the second submit at 91.3 ms,
  after a window reading `xr_end_frame` 1.61/1.78 ms (p50/max). The episode line: 88.3 ms,
  6.36 periods, path synchronous, pacing deferred, FOCUSED for 875.6 s, should_render 1,
  copy 0.23 ms. EDVR logged no event in the 10 s before.
- 08:59:22 to 08:59:50: 285 of 290 calls slow, p50 87.8 ms, max 89.4 ms; the gap between
  `xrEndFrame` returns 100.0 ms at p5. VRAM 7655 MB at 08:58:52, 8306 at 08:59:23.1, 8267
  at 08:59:27.5, against a budget of 11228 MB on every line; non-local at most 128 MB.
- 08:59:46.258 the route releases; 08:59:51.14 an orderly shutdown; Status.json clears at
  08:59:52.03. The user quit 29 s after the onset.
- Again not attributed: one texture creation of about 64.6 MB in every sampled frame of
  the lock (5 of 5 here, 11 of 11 in lock 1).
