# Headset locks on foot while the game drops to 10 fps (Virtual Desktop, v0.18.0)

## Status

*Written 2026-10-02, instruments added the same day. Update whenever this doc changes.*

- **State:** one field log read; not reproduced; no fix. The stall is inside the vendor
  runtime's `xrEndFrame`; what it waits on is not in our logs. The four diagnostic
  instruments below are BUILT (branch `claude/slow-regime-tools`, full build green), not
  merged, not installed, not flown. Waiting on the user and on a flight with them.
- **Report:** on foot, walking to the boarding circle, the headset image locks while the
  game carries on at a much lower frame rate on the mirror. Twice since v0.18.0 (once at a
  settlement, once on a station). Quest 3 through Virtual Desktop's OpenXR runtime (VDXR
  1.0.10) at 72 Hz, RTX 4070 Ti 12 GB, HMD Quality 0.50 with DLSS Performance (1261x1196
  per eye to 2522x2392), `fix.vscreen_res_width = 2880`, the on-foot route and maps at
  their defaults, `fix.weapon_stability = 1` (deferred pacing on foot).
- **What the log shows:** from 11:14:38.1 the game ran at exactly 10.0 fps until the user
  quit about a minute later. Every frame's second Submit waited about 83 ms (6 display
  periods) inside VDXR's `xrEndFrame`, and EDVR's copy into VD's swapchain took up to 79 ms
  on the GPU clock (`native_producer_gpu` p95 75.6 ms; normally 0.04). Every call returned
  success, EDVR's own CPU cost stayed about 0.35 ms a frame, and the route treated every
  frame. The previous session's breadcrumbs end on the same plateau (300 frames in 30.08
  s), with no process exit.
- **Reading:** VD is holding the game back: it keeps its swapchain images, so our copy
  and `xrEndFrame` wait. The same fixed 100 ms cadence in both sessions looks more like a
  throttle than a slow GPU.
- **Ruled out:**
  - ruled out: the route or on-foot maps holding a stale panel, because the route treated
    every frame through the lock with no event at the onset, and a stale panel cannot
    lower the game's own Present rate;
  - ruled out: per-frame churn from the panel auto-fit, because the width is explicit
    (2880), every size stayed constant, and no DLSS feature was created after 10:58:35;
  - ruled out: EDVR blocking the render thread, because EDVR's CPU time stayed about
    0.35 ms a frame with no file I/O, lock or fence wait, and the sampler's samples name
    the game, the NVIDIA driver and NGX;
  - ruled out: device loss, because no DXGI, removal or XR-loss line exists and the exit
    was orderly;
  - ruled out: a performance-level hint to VDXR (`XR_EXT_performance_settings`), because
    EDVR already enables it wherever offered and VDXR reports `extension=absent`.
- **Open:** (A) VD's stream backed up (encoder, network or headset decoder) and VD
  throttles the app; (B) GPU memory pressure on a 12 GB card in a heavy on-foot scene
  (on foot, v0.18.0 holds the route's DLAA feature and buffers beside the idle eye
  features, and VD composites a quad layer); (C) deferred pacing (no overlapped end frame)
  interacting with VDXR. Nothing logged separates them.
- **Blind spot: CLOSED for the next flight (detection only; the cause stays open).** 100 ms
  frames sit under the 250 ms FREEZE line and the 150 ms stall sampler, so v0.18.0's
  `--freezes` printed PASS. The new build writes a SLOW line naming the owner; `--freezes`
  says SLOW (exit 4), never PASS, when the vendor runtime or EDVR holds the frames, and
  INFO (a load) or WARN (the game's scenes, or no named owner) when the game does. For a
  v0.18.0 log it reconstructs: the user's bundle reads 11:14:38.1 to 11:15:35.2, 9.9 fps
  of 72 Hz, held by the vendor runtime's `xrEndFrame` (82.7 ms of each 100.6 ms frame).
  Census: no regime in Sean's 12 newest Frontier logs (section below).
- **Instruments built (section below; diagnostics only):** `vram:` lines, the vendor's
  events, every `xrEndFrame` of 3 periods or more, sustained slow frames with the owner,
  the VRAM figures and a `context=`; each with an armed line; test key `advanced.slow_test_ms`.
- **Next:** from the user, the previous session's log pair (does its last `xr_end_frame`
  read about 83 ms too?); the VD version and settings (codec, bitrate, Synchronous
  Spacewarp), what the headset showed, and whether restarting VD's stream recovers it;
  then one flight with the new build (test plan: `docs\freeze-diagnostics-2026-10-01.md`),
  and `nvidia-smi` logging once a second. If it recurs on foot, an A/B with
  `experimental.temporal_aa_on_foot_world = off` and `experimental.on_foot_maps_sharp = off`.

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
