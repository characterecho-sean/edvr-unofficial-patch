# Seat memory: the cockpit centre that nobody presses (2026-10-14)

The investigation is `reviews\boarding-recentre-2026-10-14.md` (gitignored; the evidence tables and the log reading are there). This doc is
the arc's status, the log lines the build writes, and the flight checklist.

## Status

- State: SHIPPED on `claude/flight-2026-10-14` (merged from `claude/seat-memory`), flown in part (the flight of 7116598f; see the dated
  notes). Instruments and behaviour ship together.
- **The flight key is RETIRED (2026-10-14, Sean: ship `keep`).** `advanced.cockpit_centre = keep | game` is deleted: no key, no ini entry, no
  read. The seat memory is always on in the VR profile. The old behaviour no longer exists as a setting; in the log `mode=` is `keep`, or
  `unpublished` while the graphics half has not written the word (the runtime then behaves as it always did). In the flat profile the word is
  cleared and the runtime is inactive. The cross-DLL word keeps its "active" bit (always set), so its layout and the pure logic's inactive path
  are unchanged; the `game_mode` decision survives only for that unpublished case and the rig.
- **On foot is a seat (Sean, 2026-10-14: "assume for now that on foot everyone is sitting").** Game resets on foot are decided exactly as
  aboard, and disembarking is a seat transition like boarding. The rule is ONE named constant, `kOnFootIsSeated` / `onFootIsSeated()` in
  `src\openxr\seat_memory.h`, read through `seatHolds(where)` by the decision table, every transition, and the host's meter schedule.
  Native on-foot VR (standing, room scale) flips it in that one place: false makes on foot a pass-through again and applies no seat. Not a
  config key, on purpose. The `where=` field still says `on_foot` in every log line, so the flight separates the two.
- **A runtime recentre is a manual recentre (Sean).** A LOCAL reference-space change (SteamVR's dashboard, Pimax Play) is always the player:
  once it has taken effect the head as it is in the new LOCAL is honoured as the seated origin and stored as the seat, aboard, on foot, in a
  window or out of one. Head untracked at that moment: LOCAL's own origin is the seat, applied when a frame allows. The compensate and
  forget path (`poseInPreviousSpace`) is gone.
- Open hypotheses (reviews\boarding-recentre-2026-10-14.md section 1): **H1** the game's reset lands while the head is standing or turned
  (seen 4.4 s before to 4.2 s after the journal Embark); **H2** most embarks (34 of 46) have no reset at all, so the origin left from the
  on-foot spawn reset is used in the cockpit; **H3** an internal game recentre with no runtime call. Discriminator: `seat_meter` at
  `transition_2s` after a boarding (offset_cm, height_cm, yaw_deg far from 0 with or without a `seat_reset` near the flip).
- Assumptions the first flight tests: (a) the first tracked pose at launch may be a headset on its stand, not a worn head (position repeated
  to 2 cm over 92 launches), so the seat is captured at the splash only after the head has been still for a window, and the capture's
  statistics are logged either way; (b) the 10 s transition window; (c) that seated zero is the cockpit's eye point, so a normally seated
  head reads about 0/0/0 after a good alignment; (d) Status.json flags arrive about a second late.
- **The window reaches backward (5 s; one constant, `SeatMemory::kRetroMs`, was 8 s until Sean shortened it on 2026-10-14).** A reset
  outside any window is honoured but not believed: the seat it would replace is kept as
  `previous`, and the disk is not written for 5 s (`seat_store,action=defer`). A transition inside those 5 s (boarding, disembarking, a
  journal boundary, Explorer Cam, a change of cockpit) takes the reset back: the old seat stands and is applied
  (`seat_store,action=discard_pending,why=transition_within_5s,age_ms=` and the apply's `seat_reset ... decision=replace_with_seat,retro=1,age_ms=`).
  Five quiet seconds commit it (`seat_store,action=save,why=manual_reset,deferred_ms=`). This is for the ramp's reset 2.6 to 4.4 s before the
  boarding flip. A runtime recentre is never held. With no seat stored there is nothing to go back to and a first recentre is written at once.
- Known limits of the hold: a recentre of yours followed within 5 s by a boarding, a disembark or any journal event (an undock) is taken
  back as if it were the ramp's; quitting the game within 5 s of one loses it (nothing flushes at shutdown); and a reset outside a window
  more than 5 s before a transition is still believed (the 2026-10-14 logs have one 25 s after an embark, docked). The flight should look for
  `action=defer` lines no one earned and for a seat that moved at boarding.
- 2026-10-14, flight 7116598f (8 s at the time): Sean's real recentre at 15:25:10 UTC was discarded by a `journal_event` transition 3.4 s
  later. That transition was his disembark: the journal event runs about 2.4 s ahead of the on-foot flag in Status.json, so the hold saw it
  first. 5 s would still discard that press (3.4 s is inside it). It is the documented limit above, not a bug; the shorter hold only
  narrows how often it can happen.
- Ruled out (review): STAGE as a seated centre (floor origin at the play-area centre; Sean's chair is 1.1 m and 28 degrees from LOCAL's
  origin on both runtimes); yaw-only or position-only handling as the primary fix; reading Elite's cockpit eye point as a prerequisite.
- Next flight: the checklist at the end of this doc.

## What the build does

State kept: the seat, a position and a heading in the runtime's natural LOCAL space, per headset key (`headset_key`, runtime plus system),
in `%LOCALAPPDATA%\EDVR\<leaf>-<store>\seat.txt` (`src\openxr\seat_store.h`; format `src\openxr\seat_memory.h`).

- Start (`centreAtStartup`): stored seat present: the seat is the origin (the same tracked-pose wait as before; no head sample
  chooses the origin). Otherwise today's centre on the head.
- Splash (`requestIntroRecentre`): seat stored and already the origin: no recentre. Seat stored but not applied: applied. None stored: sample
  the head until it has been still for 0.4 s (30+ samples, 2 cm and 4 degrees), at most 4 s; then the seat is that mean, or LOCAL's own
  origin when the head is within 25 cm and 20 degrees of it; saved. Not still in 4 s: today's recentre on the head as it is.
- Boarding, disembarking, going on foot from a menu, a change of cockpit (Status.json seat kind: ship, fighter, SRV, taxi, multicrew),
  Explorer Cam closing: the seat becomes the origin if it is not already (a 10 s window opens at each, and at a journal boundary).
  Disembarking usually finds the origin still on the seat, so it opens the window and puts nothing back. Each of these also takes back a
  recentre made in the 5 s before it (the Status block).
- A game ResetSeatedZeroPose: before the graphics half has spoken, or in a menu or a load: as before, nothing remembered. Aboard or on foot, inside
  a window: the stored seat replaces the head pose (nothing stored: as before). Outside a window: the player's own recentre, honoured and
  stored, but held 5 s first (it commits then, or a transition takes it back).
- The runtime recentres LOCAL (SteamVR dashboard, Pimax Play): see the Status block; the head in the new LOCAL becomes the seat.
- The word that crosses from d3d11.dll to openvr_api.dll is one 32-bit value (`src\common\seat_context.h`, `publishSeatContext` in
  frame_flag.h; the slot was padding, `reservedPoseReader0`, so the mapping version is unchanged). Status.json's seat kind is new in the
  journal watcher (`journalSeatKind`).

## Log lines (the runtime's `edvr_openxr_*.log`; none of them appear in a build without this code)

- `seat_memory,init=1,mode=keep|unpublished,key=,stored=,seat=x/y/z/yawdeg,file=,file_exists=,read_ok=,records=,skipped_lines=`
  once, at the first use. `mode=unpublished` means the graphics half has not written the word (the old behaviour).
- `seat_context,published=,mode=,gameplay=,seated=known/val,on_foot=known/val,camera=,seat_kind=0x,transitions=,where=aboard|on_foot|unknown`
  at every change of the word (at most 300).
- `seat_event,why=boarded|disembarked|seated|on_foot|seated_at_start|on_foot_at_start|cockpit_changed|camera_opened|camera_closed|journal_event|left_seat|unknown_state,window_opened=,apply_wanted=,no_seat=,where=,...`
- `seat_reset,n=,reason=seated_reset|launch_centre|intro_recentre|seat_transition|runtime_recentre,decision=,mode=,where=,window=,window_age_ms=,stored=,origin_was_seat=,caller_thread=,caller_rva=0x,head_local=x/y/z/yawdeg,from=,to=,moved_cm=,moved_deg=,after_offset_cm=,after_height_cm=,after_yaw_deg=,retro=,age_ms=,result=,ok=`
  for EVERY seated-origin change, game or ours. `decision`: `game_mode`, `pass_through`, `replace_with_seat`, `honour_no_seat`,
  `honour_store` (game calls, and the runtime's recentre); `start_seat`, `start_head`, `intro_head`, `intro_seat`, `capture_head`, `capture_snap_local`,
  `transition_seat` (ours). `caller_rva` is the game's return address for a game call (0 for ours).
- `seat_start,plan=todays_centre|apply_seat|capture_late,mode=,stored=` once, before the start's reset.
- `seat_intro,plan=skip_seat_in_place|apply_seat`; `seat_capture,state=begin|done|gave_up,...` (done: `n,span_ms,spread_cm,yaw_spread_deg,mean,dist_from_local_cm,heading_from_local_deg,snapped,seat`).
- `seat_meter,when=transition_2s|periodic,why=,where=,mode=,stored=,origin_was_seat=,offset_cm=,height_cm=,yaw_deg=,head_local=,origin=`
  (`head=untracked` when the headset is not tracked). 2 s after each window opens aboard and after each transition apply; every 30 s.
- `seat_store,action=load|save,why=...,deferred_ms=,ok=,key=,seat=,file=` (`deferred_ms` is 0 unless a held recentre committed);
  `seat_store,action=defer,why=manual_reset,prev=,new=,commit_after_ms=5000`;
  `seat_store,action=discard_pending,why=transition_within_5s,age_ms=,transition=,discarded=,restored=` (the number in `why` is `kRetroMs` in seconds).
- `seat_local_changed,events=,head=tracked,action=store_as_manual,where=,window=,seat=x/y/z/yawdeg` (the runtime recentred; the `seat_reset`
  line before it has `reason=runtime_recentre,decision=honour_store`); `seat_local_changed,events=,head=untracked,action=store_as_manual,fallback=local_identity,where=,seat=0.000/0.000/0.000/0.0`;
  `seat_local_changed,events=,action=ignored_game_mode`.
- `reference_spaces,count=,types=LOCAL|VIEW|STAGE...,stage=`, `stage_bounds,result=,width_m=,height_m=`,
  `stage_in_local,result=0,position=x/y/z,heading_deg=` once at the first poll: answers whether STAGE exists and where the play-space
  centre sits against LOCAL and the seat.

## Flight checklist

Read each log with `python tools\edvr_log.py --target <target> --expect-build HEAD --version` first (right build), then grep `seat_`.
Pimax OpenXR first; SteamVR/OpenXR as a second launch if time allows (the key differs, so each has its own seat).

0. Install, check the build line. The first `seat_` line must be `seat_memory,init=1`; if there is none the code did not run.
1. Launch 1, no seat file: `mode=keep`, `stored=0` in `seat_memory`, `seat_start,plan=capture_late` (the start's centre is the head, as it was); the baseline comparison with the old behaviour is the earlier flights' logs.
2. Same launch: `seat_intro`/`seat_capture,state=begin` then `done` at the splash. Pass: `done` with
   `spread_cm` under 2 and the splash in front at eye height; `seat_store,action=save,ok=1`. Record `dist_from_local_cm` and `snapped`.
   Fail: `gave_up` (the stats name why), or the splash is off.
3. Next launch, seat stored: `seat_memory,init=1,...,stored=1`, `seat_start,plan=apply_seat`, `seat_reset ... decision=start_seat`,
   no `intro_head` line, `seat_intro,plan=skip_seat_in_place`; first `seat_meter` within 5 cm / 5 degrees when seated.
4. Board from foot by the ramp, looking sideways; then by the interface: `seat_event,why=boarded,apply_wanted=1`, `seat_reset ... decision=transition_seat`,
   any game reset in the window `decision=replace_with_seat`; `seat_meter,when=transition_2s,where=aboard` within 5 cm and 5 degrees (offset_cm, yaw_deg),
   height_cm within 5. Read H1 or H2 from whether a game reset sits near the flip. Watch for the ramp's early reset:
   a `seat_reset ... where=on_foot,window=0,decision=honour_store` just before the flip is now held: `seat_store,action=defer`, then at the flip
   `seat_store,action=discard_pending,why=transition_within_5s,age_ms=` (2000 to 5000) and the apply `seat_reset ... decision=replace_with_seat,retro=1`.
   Pass: the seat in `seat_meter` after the boarding is the stored one, and no `action=save,why=manual_reset` for that reset.
5. Disembark (docked, then out of the ship): `seat_event,why=disembarked,window_opened=1,where=on_foot` (`apply_wanted=1` if the origin had left the
   seat), any game reset in the window `where=on_foot,window=1,decision=replace_with_seat`; `seat_meter,when=transition_2s,where=on_foot` within 5 cm and
   5 degrees seated. Walk, turn, lean on foot for a minute: no `honour_store` without a recentre of yours. Board again and repeat step 4.
6. Ship change in the hangar and back in: `seat_event,why=boarded` or `seated`; same meter.
7. SRV launch and return if a planet fits (`why=cockpit_changed`; the `seat_kind` in `seat_context` goes 0x1 to 0x4 and back); if not flown, say so.
8. Explorer Cam in and out, on foot and aboard: `camera_opened` then `camera_closed`, meter unchanged; no `seat_reset` that moves the origin off the seat.
9. Manual: press the game's recentre aboard well after a window: `decision=honour_store`, `seat_store,action=defer`, then 5 s later
   `seat_store,action=save,why=manual_reset,deferred_ms=5000` (a little over); the next boarding uses it. Press it again and board within 5 s:
   `discard_pending` and the old seat back (that is the known limit, not a bug). A runtime recentre is not held.
   Runtime recentre once (SteamVR dashboard or Pimax Play): `seat_reset ... reason=runtime_recentre,decision=honour_store,ok=1` then
   `seat_local_changed,head=tracked,action=store_as_manual`, `seat_store,action=save,why=runtime_recentre`; the head reads about 0 in the next `seat_meter`,
   and the next boarding uses that seat (`replace_with_seat` or `transition_seat` to the same numbers). A headset taken off at the moment gives `head=untracked,fallback=local_identity`.
10. Chair moved 30 cm, relaunch: the meter reads the move; one manual recentre fixes it and is remembered.
Pass overall: every boarding, ship change, SRV and Explorer Cam return inside 5 cm and 5 degrees at `transition_2s` without a press, and no
`seat_reset` with `ok=0`. Also read `reference_spaces` and `stage_in_local` once and write the answer into this Status block.
