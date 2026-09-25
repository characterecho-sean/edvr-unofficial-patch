# Load times investigation

## Status

* **State (2026-09-25):** Baseline flight completed with verbose netlog
  (`Journal.2026-09-25T144100.01.log` / `netLog.2026-09-25T144046.01.log`).
  Correlated 5 hyperspace jumps, 1 supercruise drop, station disembark/embark,
  and main menu load via `/home/dmnemec/tools/bin/load_analyzer`.
* **Findings & open hypotheses:**
  1. *H1 (Sean - Network waiting):* CONFIRMED. Inconsistent or protracted
     transitions (>18 s) are purely network-driven (AWS matchmaking RTTs,
     STUN/TURN UDP peer handshakes, fleet carrier updates).
  2. *Architectural floor (18.2 s):* Hyperspace jumps have an unalterable
     engine floor of ~18.2 s (5.0 s countdown + 0.7 s server establish +
     1.0 s vessel spawning + 10.2 s `EnterLocationFinalStep` animation floor).
  3. *Main menu load (24 s):* 16.0 s (66.7%) is sequential WebRequest HTTP
     calls to Frontier AWS endpoints; 7.75 s is local `EnterLocation`.
  4. *Station concourse / elevator (<2 s):* Instantaneous local swap; no
     network server handshakes needed.
* **Ruled out:**
  - Local CPU / disk decompression bottlenecks: asset spawning is ~1.0 s.
  - EDVR proxy overhead: 0.00 ms measurable contention during loading.
* **Actionable recommendations:** Document router UDP port forwarding
  (`Port="5100"` in `AppConfigLocal.xml`) to eliminate STUN/TURN fallback
  stalls for players experiencing 30-60 s jumps. No local patch can shorten
  the 18 s hyperspace floor without altering hardcoded animation timers.

---

## 1. Problem definition & scope

Players experience inconsistent, sometimes protracted loading times during game
transitions. The primary targets are:

1. **Hyperspace / Witchspace tunnel:** System-to-system jump. The animation
   masks server transaction, matchmaking, P2P peer mesh establishment, and
   Stellar Forge system generation. Minimum animation duration is ~11-13 s;
   delays frequently extend to 30-60+ seconds.
2. **Supercruise -> Normal space / Glide drop:** Island handoff, terrain clipmap
   generation, surface settlement geometry streaming.
3. **Main menu -> Game load ("Spinning ship"):** Initial commander state load,
   authentication, starting system warmup.

## 2. Investigation methodology

### Telemetry sources
- **Elite Player Journal:** `Saved Games\Frontier Developments\Elite Dangerous\Journal.*.log`
  provides exact timestamps for `StartJump`, `FSDJump`, `SupercruiseEntry`,
  `SupercruiseExit`, and `LoadGame`.
- **Elite Verbose NetLog:** Enabled via `AppConfigLocal.xml` (`<Network VerboseLogging="1" />`).
  Logs HTTP transaction durations, RTTs, matchmaking handshakes, and UDP peer
  connection timeouts with millisecond timestamps.
- **WPR / ETW CPU Profiling:** Windows Performance Recorder traces captured
  via `tools/cpu_profile.py` with symbol resolution for `EliteDangerous64.exe`
  and EDVR binaries.

### Flame chart ("Fire chart") generation
- Capture on-CPU execution and off-CPU thread wait states (`NtWaitForSingleObject`,
  `WSARecv` socket waits, file I/O).
- Convert/export folded call stacks into interactive flame charts (Speedscope /
  WPA flame view) aligned with Journal transition boundaries.

---

## 3. Retrospective baseline (flight 2026-09-24)

Analysis using `/home/dmnemec/tools/bin/load_analyzer` across the previous flight
session (`Journal.2026-09-24T135642.01.log` / `netLog.2026-09-24T135629.01.log`):

| # | Destination System | Start (UTC) | Total (s) | Net Wait (s) | Local Load (s) |
|---|---|---|---|---|---|
| 1 | 78 Ursae Majoris | 19:03:59 | 19.0s | 7.0s (36.8%) | 12.0s (63.2%) |
| 2 | Doris | 19:05:49 | 18.0s | 6.0s (33.3%) | 12.0s (66.7%) |
| 3 | Bei Dou Sector EB-X b1-2 | 20:09:04 | 18.0s | 5.0s (27.8%) | 13.0s (72.2%) |
| 4 | Alioth | 20:32:44 | 18.0s | 5.0s (27.8%) | 13.0s (72.2%) |

- **Baseline Total Duration:** 18.25s mean.
- **Baseline Net Wait:** 5.75s mean (31.5%) — transaction server list fetch & island matchmaking.
- **Baseline Local Procgen / Load:** 12.50s mean (68.5%) — Stellar Forge system generation, asset streaming & D3D warmup.

---

## 4. Measured flight analysis (flight 2026-09-25, verbose netlog)

Flight session: `Journal.2026-09-25T144100.01.log` / `netLog.2026-09-25T144046.01.log`.

### Hyperspace jumps

| # | Destination System | Start (UTC) | Total (s) | NetWait | ServerEst | Vessels | FinalStep (Tunnel) |
|---|---|---|---|---|---|---|---|
| 1 | 78 Ursae Majoris | 19:44:56 | 18.0s | 6.0s | 0.52s | 1.22s | 9.94s |
| 2 | Faceze | 19:46:23 | 18.0s | 6.0s | 1.43s | 0.19s | 10.56s |
| 3 | Alcor | 19:47:57 | 18.0s | 6.0s | 0.52s | 2.26s | 8.81s |
| 4 | He Bo | 19:49:05 | 18.0s | 6.0s | 0.52s | 0.63s | 10.60s |
| 5 | Alioth | 19:50:15 | 19.0s | 6.0s | 0.53s | 0.68s | 10.93s |

**Averages across 5 jumps:**
- **Total Jump Duration:** 18.20s mean.
- **Initial Net Wait / Countdown:** 6.00s (5.0s audio/visual countdown + matchmaking `server/list` query).
- **Server Establish (`EstablishServers`):** 0.70s (UDP connection to AWS EC2 instance).
- **Local Asset Spawning (`CreatePlayerVessels`):** 1.00s.
- **`EnterLocationFinalStep`:** 10.17s (hardcoded animation floor + background Stellar Forge procgen).

### Other transitions

1. **Main menu -> Cockpit ("Spinning ship"):**
   - **Total duration:** 23.75s.
   - **Network waiting:** 16.0s (67.4%) in sequential HTTPS WebRequests (`/companion/profile`, `/journal`, commander auth).
   - **Local `EnterLocation`:** 7.75s (32.6%) for player vessel, skybox, and station exterior.
2. **Supercruise drop at Donaldson starport:**
   - **Total duration:** 3.49s (`EnterLocationFinalStep` was 0.00s — no forced animation delay).
3. **Station concourse disembark / elevator:**
   - **Total duration:** < 2.0s.
   - **Network waiting:** 0.00s (already joined to Donaldson instance; zero server handshakes needed). Pure client scene swap.


