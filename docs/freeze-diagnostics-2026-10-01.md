# Freeze diagnostics: where the render thread stopped (issue 63)

## Status

*Written 2026-10-01. Update whenever this doc changes.*

- **State:** BUILT in three commits on branch `claude/stall-diagnostics` (cut from
  main 2b734be2), full build green, NOT merged, NOT installed, NOT FLOWN. Diagnostics
  only: nothing here changes what the game draws or how the headset runs.
- **Why:** issue 63, 1-2 s freezes in VR on Index + SteamVR with EDHM chained. The rc.5
  flight showed EDVR's own work under 2 ms in every freeze, SteamVR answering, the GPU
  idle, and the render thread stopping at a different point of the game's frame each
  time (before the first submit, between the eyes, in the real Present, after it).
  Nothing recorded where. The worst freeze, 1858 ms (runtime sequence 44415, ending
  23:24:19.790), reached the runtime log only: 44 of the graphics log's 59 mid-session
  LONG FRAME lines were one-frame Present-gap blips (22-58 ms while the runtime's cycle
  stayed under 2x) that spent the 60-line cap, and the 5 s limiter kept the freeze out.
- **1. Freeze logging.** The graphics half judges a Present gap over 2x the period by the
  runtime's cycle (the wait-return stamps `native_timing.cpp` now keeps): a gap the cycle
  does not confirm is a blip, counted by size and never written. Any frame of 250 ms or
  more is a freeze: its LONG FRAME line is not capped or rate limited, and a new
  `monitor: FREEZE` line follows it. The runtime's `native_long_cycle` is likewise
  unlimited from 250 ms. Long frames not written are counted by size bucket, printed
  every 5 min and at session close, with the worst five and their times. GPU-clock gap
  pairs over 1 s are kept and named (sequence, ms) instead of discarded.
- **2. Stall sampler** (`stall_sampler.h`, `stall_watch.cpp`). A watchdog notices no
  Present for 150 ms; at 150, 500 and 1000 ms of a stall it stops the render thread for
  about 25 us (copy of registers and 32 KB of stack, nothing else while stopped),
  resumes it, then walks the copy and logs `stall:` lines naming module+RVA. Rate
  limited (6 back to back, 1 per 2 s, 200 a session). Key `advanced.freeze_location`
  (on/off, default on). Safety and why it should not look like an injector: below.
- **3. Heartbeat off the render thread** (`heartbeat_writer.h`, `proxy.cpp`). The 30 s
  breadcrumb write is made by a small thread; the render thread posts three stores and an
  event. The last line before a crash or kill keeps its meaning (below), pinned by a rig
  that crashes and kills real child processes.
- **Test trigger:** `advanced.freeze_test_ms` (0 = off): 60 s into a session the render
  thread sleeps that long once, so one flight shows the whole chain. Test only.
- **New log lines:** `monitor: FREEZE`, `monitor: long frame counts reason=`, `monitor:
  worst long frame N of M`, `stall sampler: armed|off`, `stall:`, `stall sampler counts`,
  runtime `native_long_cycle_counts`, `native_long_cycle_worst`, and `native_long_cycle_
  summary` extended; read with `python tools\edvr_log.py --target steam --freezes`.
- **Gates:** rigs `freeze_log_test` (41 mutants), `stall_sampler_test` (32),
  `heartbeat_writer_test` (24), `gpu_census_test` updated, each with `mutants.py`.
- **Ruled out:** a deferred (next-frame) judgement of the gap: a freeze must be written
  at the Present that ends it. Unwinding while the thread is stopped: `RtlLookupFunctionEntry`
  takes ntdll locks the stopped thread may hold. A heartbeat on a timer: it would say
  "alive" through a hang.
- **Not built (named in the investigation, not asked for):** render-thread CPU time on the
  line; logging affinity, priority and power scheme on change.
- **Watch:** `log.max_mb` defaults to 4. The reporter's graphics log was 3.9 MB for 70
  minutes; past the cap nothing is written, freeze lines included. Not changed here.
  The flat profile gets the stall sampler (the beat is in the Present hook) but not the
  FREEZE lines or counts: the monitor ticks in VR only.
- **Next flight:** the test plan below. Read with `--freezes --expect-build HEAD`.

## Evidence

(issue 63, rc.5, Windows 11, Index + SteamVR, EDHM chained; logs gone from the repo, the
numbers are from the investigation of 2026-10-01)

- 47 long runtime cycles: before Present 18 (5.8 s), after Present 23 (3.0 s), before the
  first submit 5 (0.6 s), inside the real Present 1 (1.86 s). EDVR-side phases totalled
  28.9 ms, max 2.1 ms.
- App-render GPU 8.4 / 5.9 / 6.7 ms in the freezes; GPU-clock gaps 573.7, 315.2, 291.5 ms
  matched other stalls (pairs over 1 s were discarded). SteamVR never held the app: worst
  WaitGetPoses 20.2 ms, frame-end body 13.9, submit 4.7 over 138 windows.
- `--tally periodic`: one slow operation, `journal_status` 2339.9 ms ending 23:24:19.797,
  7 ms after the 1858 ms stall; the worker ran its other 276 polls through the 1.4 s
  freezes (max 5.9 ms), so the cause was not machine-wide.
- The graphics log's LONG FRAME lines: cap 60, limiter 1 per 5 s, `f.presentMs < 5000`
  (a gap of 5 s or more was never logged at all).

## What the logs show, and what they show if the new code never ran

| Line | Written when | If it is absent |
|---|---|---|
| `monitor: FREEZE -- ...` (graphics) | every Present gap of 250 ms or more, after its LONG FRAME line | no such frame, or an older DLL |
| `monitor: long frame counts reason=periodic` | every 5 min, even with zeros | the freeze logging never ran (older DLL) |
| `... reason=session_close` / `shutdown` | the runtime closes its timing context / the DLL is freed | the game exited without a VR close (process exit cannot write: loader lock, threads dead): the periodic line is the last word |
| `monitor: worst long frame N of M` | at the same moments, when the list changed (periodic) or whole (end) | no long frame |
| `stall sampler: armed` / `off` | first Present | an older DLL, or the key was read before the config loaded |
| `stall: the render thread stalled N ms in ...` | at 150, 500, 1000 ms of a stall | no stall, or the rate limit (`skipped_rate_limit` in the counts line) |
| `native_long_cycle_counts` / `_worst` / `_summary` | runtime, every 5 min / at close | an older runtime |

`over_250ms_unwritten=0` in every counts line says no freeze slipped past the writer. A
nonzero value is a bug, and `--freezes` reports it as STOP.

## Why a Present-gap blip is not a stall

A Present gap runs from one Present to the next, so it holds the tail of one runtime cycle
and the head of the next; a game that Presents early in one cycle and late in the next
stretches the gap by up to a cycle without any cycle being long. The judge reads the
cycle the gap touches: the previous cycle whole, and the current one up to this Present
(a floor of what the runtime will log). Over 2x the period it is long, otherwise a blip.
It cannot read it (flat profile, fewer than two waits) the gap is trusted. The Monitor
page's "last drop" still takes every gap over 2x: a blip is a missed frame to the person
in the headset.

## The sampler: safety, and why it should not look like an injector

1. While the thread is stopped, one function runs: `SuspendThread`, `GetThreadContext`, a
   bounded copy of the stack into a buffer that existed beforehand, `ResumeThread`. No
   allocation, no formatting, no logging, no lock, no exception handler (dispatch looks up
   unwind data under locks the stopped thread could hold), no early return.
2. The walk (`RtlLookupFunctionEntry`, `RtlVirtualUnwind`) and the naming happen after the
   resume, on the copy, with the registers that point into the stack rebased into the copy.
   A rig overwrites the live stack after the capture and checks the frames are still named.
3. Every path after a successful stop reaches the resume (failed context read, stack
   pointer outside the registered stack, exited thread): three hundred stops in a row leave
   the thread running; `stall_sampler_test` has mutants that skip the resume.
4. Same process, one thread, no writes. The handle is the thread's own pseudo-handle
   duplicated by itself with three rights (suspend/resume, get context, query limited
   information). No `OpenThread`, no enumeration, no `SetThreadContext`, no
   `WriteProcessMemory`, `VirtualAllocEx`, remote thread or APC; the rig scans the sampler's
   code (comments removed) for 21 such names. EDVR already uses these calls for its
   hardware-watch probes, and this is what Chromium's and Firefox's samplers do, so the
   import table gains no new kind of call. That reduces the signal Defender's heuristics
   look for; it cannot promise a new hash will not be flagged (see the Defender note).
5. Cost: one relaxed store per frame; a thread that wakes about 7 times a second to read a
   clock; at most three stops per stall.
6. Measured in the rig: stopped for 19-28 us per sample, 7 frames named, samples at 154,
   508 and 1015 ms of a 1.3 s stall.

## The heartbeat: what the last line still means

The writer thread writes only what the render thread posted (no clock of its own, so a hung
render thread stops the heartbeat); a line carries the frame and uptime of the moment it was
posted; posts coalesce, newest wins, so a slow disk never writes an old frame after a newer;
the crash filter closes the heartbeat (bounded wait of 20 ms for a write under way) before its
first line, and DllMain closes it on both detach paths before the closing crumb. A write
already stuck in a stalled disk for longer than 20 ms can still land behind the crash lines.

## Test plan for Sean

1. Install the build (`python tools\install_edvr.py --target steam --dry-run`, then for
   real). Set `advanced.freeze_test_ms = 1200` under `[advanced]` in the live edvr.ini
   (Edit tool, diff before and after).
2. One VR session of a few minutes. At about 60 s the game stops for 1.2 s. Expect in the
   graphics log: `freeze test:` lines; three `stall:` lines at about 154, 508 and 1015 ms
   (top frame in ntdll, owner `d3d11.dll+0x...`, `EDVR code on the stack: yes`, a few tens
   of us suspended); a `monitor: LONG FRAME` and a `monitor: FREEZE` line, the FREEZE line
   saying `stall sampler took 3 samples`; in the runtime log a `native_long_cycle` of about
   1200 ms.
3. Quit the game from the menu. Expect `reason=session_close` counts and `worst long frame`
   lines in the graphics log, `native_long_cycle_summary` with buckets and
   `native_long_cycle_worst` in the runtime log. Kill the game from Task Manager in another
   session: the counts of the last five minutes are the `periodic` ones.
4. `python tools\edvr_log.py --target steam --freezes --expect-build HEAD` reads all of it.
5. Set the key back to 0. A normal session: no `stall:` lines unless the game really stalls;
   `edvr_breadcrumbs.txt` still gets an `alive` line every 30 s (check it is written, then
   kill the game: the last line is an `alive` line).
6. `advanced.freeze_location = off`: `stall sampler: off`, no `stall:` lines, the FREEZE
   lines unchanged.
7. The reporter's setup (EDHM chained): ask for one session with the build and the freeze
   lines; the owner module of a freeze is the answer to issue 63.
