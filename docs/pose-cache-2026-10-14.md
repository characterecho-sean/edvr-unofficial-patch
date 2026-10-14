# Pose cache and local event ring, ported to main (2026-10-14)

## Status

- **State.** Built on `claude/pose-cache-main` from main 1bf28f5f, NOT FLOWN on this base. The original (`claude/render-cpu-posecache`, 80e749aa)
  was flown OK on 2026-10-11 (flight f31fdc9a, Frontier): `locateHead` owner hops fell from about 6 a frame to 0.003-0.01, 8-9 cache
  hits a frame (about 4 in a wait), `target_delta` 0.000/0.025 ms, angle p95 0.08-0.29 deg, age 2-8 ms.
- **What it fixes.** Elite's job thread calls GetDeviceToAbsoluteTrackingPose and PollNextEvent. Both hopped to the owner thread and
  queued behind xrWaitFrame, about 2.5 + 0.95 ms a frame of unexplained post-Present game time on a SteamVR user with 2.2 ms of
  xrWaitFrame slack (v0.20.0-rc.1).
- **Temporary key (A/B).** `pose_cache=on|off` in `Openvr\win64\edvr_openxr.ini` (the native runtime's startup file, NOT `edvr.ini`; the
  runtime does not read `edvr.ini`). Default on, read once at startup. `off` is the owner-hop path exactly as before. Goal: if the flight
  is clean, ship the behaviour and remove the key (quote it first, per AGENTS.md).
- **New since the original (this port).** Every seated-origin change withdraws the publication before the next cached answer (see
  Invalidation); the withdrawals are counted by reason in `native_pose_cache_invalidations`; the cache's lock covers only copies, never a
  sort or an allocation; tests per route in `tools\openxr_native_test\pose_cache_invalidation_cases.h`.
- **Open.** (1) Flight on Pimax or SteamVR: hangar, `on` against `off`. (2) The original's in-wait `next` slot is early by k periods
  after a skipped frame (measured by `target_delta`, accepted in the 10-11 flight). (3) `edvr_log.py` does not parse the two cache
  lines; read them with `--grep native_pose_cache`.
- **Ruled out.** Nothing new. The original's own journal (not on main) holds H7 (the job thread stalls behind xrWaitFrame, confirmed by
  flight 9033ff1b, queue p95 8.1-9.4 ms).
- **Next flight.** Checklist at the end.

## What was ported

Two commits of the original lineage, nothing else (the CPU profilers, the wait-lead lines and the elite_hotspots tool stay on their
branches):

1. 56ed90fc `native_owner_invoke`, the per-kind owner-hop accounting. The pose cache's "hops a frame" figure is this line, and 80e749aa
   builds on it. The wait-lead pieces that sit beside it on that branch (`WaitLeadStats`, `native_wait_lead`) were NOT taken: the hunks
   were dropped in the merge and `emitOwnerInvoke` hangs off `reportFrameCycles` directly.
2. 80e749aa the cache itself: `pose_cache.h`, `poll_ring.h`, `reset_events.h` (shared `SeatedResetItem`, `takeFor`), the host's publish,
   answer and ring-fill, the `pose_cache` key, and a use-after-free fix in `owner_service.h`: the owner kept a raw pointer to the host's
   `ownerInvoke` stats and read it while a request ran; the stats object is now shared and the pointer is read under the service mutex
   (the rig crashed in `OwnerInvokeStats::note` four runs out of four before it). The fix is only needed because 56ed90fc is here.

## How the cache works

The owner publishes, at the end of each waitPoses, the head pose at the two instants an Elite display call can be answered at: `now`
(display + 1 period, for a call between waits) and `next` (display + 2 periods, for a call made while the next wait is in progress; the
owner marks `waiting` at the wait's entry). A display call on the job thread is answered from the publication when its keys match (geometry
generation and origin generation, both readable from any thread), its display time is fresh at the caller's clock, and its RVA has been
seen once. Anything else takes the hop unchanged and is counted by reason. PollNextEvent drains a local ring of the seated resets the owner
fills at each wait; a queued quit is delivered first, as before (`quitMutex_`).

Thread safety: the publication is one record under one `std::mutex`. The owner holds it for a struct copy or a counter bump, never across
an OpenXR call; the job thread holds it for the same. A mutex is chosen over a seqlock because the record is about 300 bytes with
several writers (publish, beginWait, invalidate, takeWindow) and a seqlock's retry buys nothing the lock does not already give at this
rate. There is no unlocked read of the record anywhere. `takeWindow` swaps the sample vectors out under the lock and sorts them outside.
`resetPolls` is atomic; the quit flag has its own mutex; the ring has its own mutex.

## Invalidation: a stale seated space is never served

The cached pose is in the seated space as it stood at publication. Each of these withdraws it, on the owner, before the next cached answer:

| Route | Where | Counted as |
|---|---|---|
| Seat memory applies its seat ("seat_transition") | `applySeatedResetCore`, before `seated.replace` | `seat_apply` |
| The game's ResetSeatedZeroPose, honoured or replaced by the seat, or a late press ("seated_reset") | same | `game_reset` |
| The runtime moves LOCAL: the pending-change event | `referenceEvent`, at `changes.note` | `runtime_recentre` |
| ...the frame it takes effect on, withheld (`advanceReferenceChanges`, "reference_change") | `invalidateOrigin` | `runtime_recentre` |
| ...the seat memory's answer ("runtime_recentre") | `applySeatedResetCore` | `runtime_recentre` |
| Launch centre, intro recentre (head or stored seat), session restart | same / `invalidateOrigin` | `startup` |
| Session, geometry, fatal failure, failed reset, a frame that failed to pose | `dropPoseFrame()` | `other` |

The withdrawal in `applySeatedResetCore` comes before the space is replaced, not only in `invalidateOrigin` after, so no cached answer in
the old space can be served from the instant the space changes. The pose's origin-generation key is a second guard: a caller that read the
old generation after the bump is refused by key. A withdrawal is counted only when a live publication was withdrawn.

## Log lines

The original's, unchanged: `native_pose_cache,window=N,hits=,hits_in_wait=,misses=,miss_reasons=...,age_p50/p95=,target_delta_p50/p95=,
angle_delta_p50/p95=,publishes=,events_local=,events_dropped=` and `native_pose_cache_config,enabled=` at start;
`native_owner_invoke,window=N,kind=locateHead|pollEvent|waitPoses|submit,calls=,per_frame=,queue_p50/p95=,run_p50/p95=,behind_wait=` (hops a
frame is `per_frame`, `behind_wait` is the stall count).
New: `native_pose_cache_invalidations,window=N,seat_apply=,game_reset=,runtime_recentre=,startup=,other=,total=`, once a window beside the
others, only while the key is on.

## Flight checklist

On a Pimax or SteamVR headset, hangar. Confirm the build first: `python tools\edvr_log.py --target steam --expect-build HEAD --version`.

1. `pose_cache=on`: `native_owner_invoke` for `locateHead` and `pollEvent` should read `per_frame` near 0; `native_pose_cache` shows hits
   at 8-9 a frame, angle p95 under 0.3 deg, age 2-8 ms.
2. No seat jumps: board, exit, recentre by F12, recentre in the SteamVR dashboard; each should appear in
   `native_pose_cache_invalidations` (game_reset, runtime_recentre, seat_apply) and the view should not pop.
3. Quit by the game's menu still delivers the quit (events_local counts resets only).
4. `pose_cache=off`: the same hangar, compare game post-submit time and fps. `native_pose_cache` lines must be absent.
