# Flat on-foot CZ frame rate (report of 2026-10-11)

## Status

State (2026-10-18 certificate): geometry issue reuse is implemented on codex/flat-startup-discovery-cpu, full build PASS, Steam flat flown as v0.20.0-rc.4-26-g1012c7c2 in edvr_gfx_20261018_063400.log, carrier 97.7 fps at 06:41:16, live not frozen, no new config key; flight line is "flat geometry issue reuse:" and 052052 stays the baseline (handoff docs/flat-callback-cpu-handoff-2026-10-18.md). Prior snapshot `041f8e78` (`rc.4-23`) showed no useful speed win: last slow1 59.03 FPS, fast 221.29, full slow2 65.18. Earlier slow1 61.64 is a different overlapping slice. All 18,442 selected frames continue AA history/backend at native3840 HDR, zero new resets/refusals/jitter failures. Full slow2 callback gross5.976/net whole11.891 ms vs709's4.558/11.220; coverage is SAME, three new child timers add cost, attribution unresolved. Main unmerged, probe off, settings unchanged. User-requested handoff: `docs/flat-callback-cpu-handoff-2026-10-18.md`; start there for checkout/build/artifacts, external-review assessment and next certificate proof. Existing engine generation cache/1in64 shader checks do not certify all forward-Issue dependencies. No further identical flight requested. Sol RE/tests, Astra evidence only. Phase bootstrap remains unvalidated; Ctrl+NumLock ingress issue persists (earlier entries).
State (2026-10-16, flight build `claude/flat-settlement-cpu-7`, log `edvr_gfx_20261016_135436.log`): FLOWN OK, ALL THREE ON MAIN on Sean's approval. COPIER claim
check (`claude/copier-claim-check`, round 6): claims live from about 14:06:30, 6.0M copies lock-free, 0 declined. OFFSCREEN fast path (`claude/flat-offscreen-fastpath`):
about 3,720 draws a frame served (expected 3.5-3.8k); its saving is still read from the timing-on line, not measured. Merge/clear took 3.0M/8.6M lock-free with 2
declined. Present p50 11.5-12.0 ms before claims and 11.4-12.4 after: no jump (it was 12.3-12.6 -> 12.8-13.1 with the clear/merge check alone, and 3-5 ms without
any). DEBUG tab (`claude/flat-debug-menu`): the CPU census (`flat cpu 5s` timing) is OFF by default; the flat F8 panel has Performance and Debug tabs: **CPU timing in the
log** (`advanced.flat_cpu_timing`), **Motion source view** (`advanced.flat_refusal_view`); turn the timing on to see the families. Sections "round 6", "offscreen fast
path", "debug tab".
State (2026-10-16, round 5 results): the claim check for clears and merges is FLOWN OK and ON MAIN (`claude/claim-precheck`): 1.43M clears and 4.1M merges in the first
window, 0 declined, Present p50 12.3-12.6 -> 12.8-13.1 ms against a 3-5 ms jump without it (log `edvr_gfx_20261016_111653.log`); the remaining +0.5 ms is not attributed.
State (2026-10-16, results): the observer fix is FLOWN, CONFIRMED and ON MAIN (1ae94885, `claude/engine-motion-observers`): render-thread
engine motion 427 -> 77 ns a call (about 0.8 ms a frame), job merge and clear down 92%, 100% of calls lock-free, 0 contention. The
instruments (H1-H4, round 2) are on `claude/flat-settlement-cpu-2`, NOT on main. Round 2 flown (DLSS, 14.6k draws a frame): EDVR render
thread 5.17 ms floor-net (353 ns a draw), Present p50 14.91, GPU span p50 12.52, 63.6 fps. AA-off A/B: ~145-150 fps against ~150 with EDVR
suspended; the whole gap is the AA path. Section "2026-10-16, results" has the numbers. The arc is NOT closed: ~5 ms of per-draw CPU is
spread thin (about 20 pieces, none above 0.6 ms); the open question is structural (a per-pipeline-state "nothing to do" fast path).
Ruled out (2026-10-16, round 2): the unscoped binding thunks as the 2.4 ms unexplained (about 7.3k bind calls a frame at about 30 ns net
is 0.12-0.24 ms); a single hot block in the draw constructor (its 5 blocks hold about 0.85 ms net); g_mutex contention (0 contended).
Earlier state (2026-10-16): the observer reduction was ported onto main as `claude/flat-settlement-cpu` with H1-H4; the 5.8 ms a frame
(0.52 in the cockpit; draws 360 -> 10.5k) in `edvr_gfx_20261016_023749.log` was the diagnosis.
Earlier state: BUILT on `claude/fps-drops-onfoot-czs-577f5a` (observers, copier, lock lines, flat cpu by part, then the review's fixes), NOT
FLOWN. A bounded experiment on one of three suspects, plus instruments that say whether it was the right one. NOT the fix: the
expected gain is about 4-5 ms of a 13-25 ms gap at most, and the per-draw render-thread work (about 8-9 ms a frame) is untouched.
Report: flat v0.19.1 (Steam, build 6ACAF769), RTX 3050, "40 fps without the mod, 20-25 with DLSS" in on-foot CZs.

Evidence, from the reporter's log `edvr_gfx_20261011_184408.log`:
- In-CZ A/B: `fix.temporal_aa` toggled through F8, off at 18:58:43, 19:01:52, 19:13:28. vScreen 20 s windows: DLSS 21 fps
  (19:01:49), off 45 and 46 (19:02:29, 19:02:49), TAA 27 (19:03:29). Off is 1.5-2x faster each time: the temporal path costs
  10-25 ms a frame, and TAA is as slow as DLSS, so it is CPU, not the NGX resolve. 810p was no faster than 1080p (48 s of 810p).
- Heavy windows (over 1500 substituted draws a frame): Present minus GPU span 13.7 ms mean (median 13.9), 1.0 ms in light ones.
  The span runs from the first game draw to Present on the GPU timeline; it is not GPU busy time.
- EDVR's render-thread time, clock inflation removed (printed in the `flat cpu` line, about 1.6 ms): median 10.5 ms a frame,
  of which the per-draw work is about 8-9 ms.
- Job threads: about 2,500 merge (median) and 3,500 clear observer calls a frame. A clear costs 0.32 us low mode, 4.02 us high mode
  at the same call count: the high mode is waiting. Render-thread engine hooks: 0.42 us a call low, 2.14 us high, tracking the clear
  walks. The copier takes `g_mutex` about 2.7k times a frame; emissions run about 25 a second, copies about 70k.
Ranked causes (independent review): (1) EDVR's per-draw work on the render thread, about 2/3 of the delta, NOT touched here;
(2) `g_mutex` contention, about 2-3 ms on average, and only if the plan fix below holds; (3) observer work lengthening Elite's job
phase (H-obs primary): UNPROVEN, and these commits cannot prove it.

Built: `g_claimsLive` (emissions or plans) and `g_emissionsLive` (emissions) published by `ClaimLock`, the only way the header locks
`g_mutex`. A clear with no claim or plan skips the mutex. A merge with no emission gets a fixed token (no plan, walk or allocation),
under the lock too: a plan that holds nothing used to keep every fast path off. The copier skips the mutex for count 0, no lease, and
one fitting lease when no emission and no pool patch is published (seqlock view `g_poolView`). The clear walk uses small-buffer sets.
`ClaimLock` times 1 acquisition in 64 per caller (render thread from `emcpu::g_renderThreadId`, which both censuses publish every
frame; the gate cannot say, flat writes nobody into it on 15 frames in 16). The `flat cpu 5s` line ends with the render thread's
engine hooks by part. NOT built: that line while AA is off (its GPU span needs the draw scope, a device and an owner thread the
off state never creates: over 40 lines, and it touches the draw path).
The flight reads, per 30 s: the version line; `primary copy lock-free exits` (expect over 90% of calls); clear `nodes walked` (well under
100k, against 0.3-1.1M in this log); the lock lines (render copier, beginMap, endMap wait under 1 us average; a large render `wait`
beside a large `other` hold names the holder; all-zero n while `copier spans` grows means the instrument did not run); `flat cpu`
render hooks and the copier part under 0.3 ms in every window; then F8 off / TAA / DLSS, 30 s each, 3 cycles in one CZ.
Limits: the copier's patch flag is global. After a pool's first join in a frame (about one a frame here) every copy takes the
mutex until that pool's next Map, so expect perhaps half the copier gain. A rig cannot kill a weakened memory order (x86 hides it).
Open: was the reporter fine on 0.19.0? The observer and flat-substitution files are byte-identical between v0.19.0 and v0.19.1 and
the always-on 0.19.1 additions all run in the 45 fps off state. Ask which `temporal_aa` mode and scene they ran on 0.19.0.
NOT claimed: that the observers are on Elite's critical path; that this recovers the off/on delta; that any fps is gained.

## What changed

Working files (gitignored): `reviews\flat-cz-fps-findings-2026-10-11.md`, `reviews\flat-cz-fps-review-2026-10-11.md`.

`src\d3d11\engine_velocity_primary_copy.h`: `g_claimsLive`, `g_emissionsLive`, `ClaimLock`, `AddressSet`, `mergeNoClaimsToken`,
`copierLockFree`, `g_poolView`, `Caller`, `formatLockLine`, `formatExitLine`. `stats()` folds the lock-free counters into
`clearCalls`, `clearNoClaims`, `mergeCalls`, `mergeWithoutClaims`, `copies`, `copierNoLease` and also reports them apart
(`clearLockFree` and the rest); the new `lock-free exits` line prints them. So "no-claim skips" on the observer-work line mixes
lock-free exits with locked skips: read the exits line for the split. `merge plans` counts full plans only, in VR logs too.
Merge nodes are not counted for a no-claim merge. The token's unwind revokes every claim registered since (the old empty plan spared
unrelated later owners after a clear or a frame change): safe direction, native unwinds only. A malformed list with no emission no
longer bumps the epoch, so it no longer fails a claim-carrying plan in flight: that plan validated its own list.
Safety: every lock-free exit reads a flag that each critical section publishes exactly before it unlocks, so a stale read linearises
the call before an in-flight critical section, which the old mutex allowed too. The engine's own ordering (an emit and a Map before the
copy of that item) only decides how often a join happens, not whether the result is right. Equivalence tests run the pre-change
walk and copier beside the new ones. Plans do not read each other, and a plan's staged claims are not in `g_emissions`, which is why
a merge and a copy may ignore plans while a clear may not (it must mark an in-flight plan invalid). Tests:
`tools\engine_velocity_test\primary_copy_observer_tests.h`; `engine_velocity_test.exe --bench` is opt-in.

## Benchmark

ns per call on an in-process fake of Elite's lists, one machine, same run for both columns.

| case | before | after |
|---|---|---|
| merge begin+end, no claims, 1 node | 325 ns, 11 allocs | 5.9 ns, 0 |
| merge begin+end, no claims, 8 nodes | 966 ns, 24 allocs | 6.0 ns, 0 |
| clear, no claims, a no-claim merge in flight | 2495 ns, 80 allocs | 5.2 ns, 0 |
| clear walking 1 / 8 nodes, claims present | 421 / 2486 ns | 63 / 836 ns, 0 allocs |
| 4 threads, merge then clear, no claims | 2784 ns | 65 ns |
| 4 threads, merge then clear, claims present | 3766 ns | 1518-1882 ns |
| copier, active pool, 1 slot | 16.8 ns | 5.5 ns |
| copier against 3 threads calling `invalidateEmission` | 80 ns | 5.6 ns |
| 4 threads, copier (per thread) | 116 ns | 33 ns |
| merge beside an open plan that holds nothing | 268 ns, 8 allocs | 5.9 ns, 0 |
| copier beside an open plan that holds nothing | 20.8 ns | 5.5 ns |
| `invalidateEmission`, lock + failed erase | 14.1 ns | 16.6 ns (the instrument) |

The first rows are the observer commit against the baseline, the copier rows against the observer commit, the plan rows against
the lock-line commit, the last row the instrument's price (one thread-local increment per locked call, about 2 ns). The 4-thread
no-claim figures are bounded by shared counters; striping them was not worth the code.


## 2026-10-16: the settlement CPU arc (flat, on foot at a settlement)

Diagnosis, from the flat log `edvr_gfx_20261016_023749.log` (Steam, flat, DLSS): EDVR's render-thread CPU at a settlement is about 5.8 ms
a frame net against 0.52 in the cockpit; draws went about 29x (360 to 10.5k a frame). The `flat cpu 5s` buckets are exclusive-time
families. "other" was the draw hook's constructor shell, its destructor shell and the dispatch shell, about 117 ns net a call, two
calls a draw. The engine motion render-thread hooks were the one superlinear bucket (66 to 445 ns a call); the job-thread clear and
merge share one `std::mutex`, and 84-86% of their calls had no claims yet built a plan, a set and a vector anyway.

Branch `claude/flat-settlement-cpu` (from origin/main cbff0142). Two parts.

### Part 1: the fix, ported (no new code)

The observer reduction was reviewed and built on 2026-10-11 and never landed. It is ported by `git cherry-pick` of its seven commits
(457ce3a8 and its six parents on `claude/fps-drops-onfoot-czs-577f5a`; they apply cleanly, touch no ADS-naming-veto file):
c086b5d1 (merge and clear observers skip the mutex, the walk and the heap when no claim is live), f75840d9 (the pool copier's quiet
cases skip the mutex), 9d6a9b3e (lock timing per caller and thread class), d4739533 (`flat cpu 5s` splits the render thread's engine
hooks by part: this is H3's per-part print), 5c18371c (lock lines file the render thread under "render" in flat), 178d7c52 (plans that
hold nothing no longer keep the fast paths locked; the lock-free share is printed), 457ce3a8 (the doc). The reviewer's concerns, from
the Status block above, are what 5c18371c and 178d7c52 fixed: the lock-line render/other mislabel in flat, and empty merge plans
keeping the fast paths off. Both are in the port. Open as before: the observers are NOT proven to be on Elite's critical path.

Rig evidence (`tools\engine_velocity_test`, `primary_copy_observer_tests.h`; the rig exits at its first failing check, so each
mutant below stops at one): fail-before, by mutating the shipped header and compiling the rig against it (mutants run by hand,
sources restored, hashes compared):
- clear's lock-free exit off: `FAIL: plans: afterwards a merge, a clear and a copy all take the lock-free paths`
- merge's lock-free exit off, or the copier's: `FAIL: plans: while that plan is in flight a new merge and a copy both skip the mutex,
  and no second plan is registered` (those checks hold the mutex on another thread: a call that takes it cannot complete)
- the exits AND the locked stale-flag exits off (the old behaviour: a walk, a plan, allocations): `FAIL: primary observer: no-claim clear
  bypass avoids the native traversal; neither it nor the full one-node walk allocates`
Claimed paths are unchanged (the equivalence tests run the pre-change walk and copier beside the new ones), and a claim racing a
no-claim exit is never lost: every lock-free exit reads a flag each critical section publishes before it unlocks (see "What changed").

### Part 2: instruments (no behaviour change, no config key)

Hypotheses, each with the line that confirms or refutes it:
- H1 the constructor shell is the cost, in its lookup block (views, `flatContractKind` calls, pool-family pair, tone slots) or its
  predicate block (weapon, overlay, untrusted nomination). Confirm: `ctor lookup block` or `ctor predicate block` ms per frame holds
  most of the old "other". Refute: `draw ctor shell` (the remainder) or `draw dtor shell` holds it.
- H2 EDVR's per-draw cost is spread over scopes the families do not name, or the game's own draw is slower for EDVR's state. Confirm:
  `draw hook remainder` (the thunk's unscoped time) is large, or `game draw after substituted` per call is well above `game draw
  other`. Refute: the families add up to `EDVR hook cost` with a remainder under 0.1 ms and the three game draws cost the same a call.
- H3 `g_mutex` contention between the render thread and the job threads. Confirm: `lock contention` shows render or other counts
  that are a double-digit share of the estimated acquisitions, or the render thread's hooks by part (`clear`, `merge`) track the
  contended counts. Refute: contended is a few per window while hooks by part still cost 66-445 ns a call (then it is the work, not
  the wait).
- H4 the overlay guard (shader pairs BBE/DB3E) forces eager runs that cost many D3D calls. Confirm: `eager guarded` runs a frame is
  non-trivial and its calls a run and microseconds a run are well above `lazy`. Refute: guarded runs are a few a frame.

Scopes (one per site, none per draw logged): `flat_cpu.h` splits kOther into `draw ctor shell` (kDrawCtor), `draw dtor shell`
(kDrawDtor) and `dispatch shell` (kDispatchShell), with `ctor lookup block` and `ctor predicate block` inside the constructor
(Scope::stop() ends them mid-function; locals outlive them). `draw hook remainder` is a scope around the whole flat branch of the five
draw thunks in vscreen.cpp (Draw, DrawAuto, DrawIndexed, DrawInstanced, DrawIndexedInstanced), so it is the thunk's own time less
everything nested, and the Cell now also keeps INCLUSIVE wall time for it. The game's draw call is timed in three families by what it
met: `game draw substituted` (the draw is a substituted producer), `game draw after substituted` (the previous watched draw was
one: the driver meets state engine motion put back or left bound), `game draw other`. All use the census gate, so they are clocked on
the same one frame in 16 as the rest; a scope's whole wall time is subtracted from its parent exactly once, and the game's draw
families are printed but kept OUT of "EDVR per frame total" and out of the other threads' "every other family".

COMPARABILITY: "EDVR per frame total" on this build includes the new `draw hook remainder` (EDVR time the old total did not see) and
nothing of the game's draw. "other" in older logs = `draw ctor shell` + `ctor lookup block` + `ctor predicate block` + `draw dtor shell`
+ `dispatch shell` here. The line is now up to 6 lines of 1000 characters at the worst plausible values (kMaxLines 7, kMaxTokens 96).

Line formats (examples from the rig's fake data; real lines are single lines):
- families, in `flat cpu 5s:` as before, the new names: `... + draw dtor shell 0.400 ms (calls 10500) + dispatch shell 0.020 ms
  (calls 90) + ctor lookup block 0.700 ms (calls 10500) + ctor predicate block 0.600 ms (calls 10500) + draw hook remainder 0.500 ms
  (calls 10500) + game draw substituted 1.100 ms (calls 2400) + game draw after substituted 0.500 ms (calls 700) + game draw other 2.000
  ms (calls 7400) + camera inject ...`
- H2: `; draw hook flat branch (render thread, per clocked frame): wall 8.300 ms over 10500 draws, game draw 3.600 ms, EDVR hook cost 4.700
  ms (unscoped remainder 0.500 ms); game draw per call: substituted 0.46 us a call (2400 a frame), after substituted 0.71 us a call
  (700 a frame), other 0.27 us a call (7400 a frame)`. "-" when a family has no call, never 0.00.
- H3 per part (from d4739533, last on the line): `; engine motion hooks by part (render thread, per clocked frame): emit 0.000 ms (calls
  0.0), ... merge 0.080 ms (calls 900), clear 0.120 ms (calls 2600), ...`; contention is a third lock line every 30 s: `engine motion:
  primary copy lock contention (30 s; acquisitions that found the mutex held, per thread class, of ~acquisitions): ... clear walk:
  render 0 of ~0, other 41 of ~7680 | copier: render 7 of ~19200, other 0 of ~0 | ...`. Every acquisition tries `try_lock` first
  and counts a failure before it blocks (exact); "of ~N" is the 1-in-64 timed sample times 64.
- H4: `; engine motion runs (the wrapper's brackets, a frame; the time is the clocked frames'): lazy 2380.0 runs/frame (4.0 D3D calls/run,
  0.46 us/run, 1.100 ms per clocked frame), eager guarded (overlay t3) 20.0 runs/frame (24.0 D3D calls/run, 15.00 us/run, 0.300 ms per
  clocked frame), eager capture armed 0.0 runs/frame (- D3D calls/run, - us/run, 0.000 ms per clocked frame), lazy flushes 500.0
  flushes/frame (11.0 D3D calls/flush, 4.40 us/flush, 2.200 ms per clocked frame)`. A run is one substituted draw; its D3D calls are
  BeginDraw's plus, for an eager run, its restore; a lazy run's restore is paid by whichever event flushes it (`lazy flushes`).

If the code never ran: every family and the run line print as zeros (never absent), the per-call times print "-", and the run counts
beside `engine motion wrapper D3D calls .../frame over N substituted draws/frame` disagree: runs summing to nothing while that N is
large means the counting is dead. Contention all zeros beside lock lines with a large n means nothing met on the lock, OR (check the
version line) the build is stale. Rigs: `flat_cpu_tests.h` (stop(), wall, the thunk line, game draw per call, empty-window dashes, the
runs line), `flat_temporal_test.cpp` wiring pins, `flat_lazy_tests.h` (runs by kind against the overlay sequence, the capture-off
case and the census line), `primary_copy_observer_tests.h` (contention counted while the caller is still blocked, per thread class,
exact text, worst-case length). Mutants caught: contention counting removed -> `FAIL: contention: a call that blocks on another thread
is counted once, under other, while it is still blocked`; run counting removed -> `FAIL: flat lazy: with the lazy form off every run of
the overlay sequence is filed as an eager capture run, with its D3D calls`.

Flight plan (flat, Steam or Epic install, one on-foot settlement, 2 min; `python tools\install_edvr.py --target steam --scope flat`, then
`--verify-only`, then `python tools\edvr_log.py --target steam --expect-build HEAD` first):
1. DLSS on: read `draw hook flat branch`, `draw hook remainder`, `ctor lookup block`, `ctor predicate block`, `draw dtor
   shell`, the by-part hooks, the contention line and the runs line. The per-draw cost is what H1/H2 rank; one flight eliminates several.
2. A/B: F8 `fix.temporal_aa` off and back (the census runs only while a temporal mode is selected; the thunk scopes then cost a gate
   load and a compare). For a true no-EDVR baseline see below.
3. Compare the lock-free share (`primary copy lock-free exits`, expect over 90%) against the 2026-10-16 log's 84-86% no-claim calls.

`install_edvr.py --suspend` exists (`--target <dir|steam|epic|frontier>`, `--dry-run` to preview). It renames the game directory's
d3d11.dll to d3d11.dll.edvr-suspended (one rename, hashed before and after; only an EDVR build is accepted) and `--resume` puts it back.
It refuses unless the game is PROVEN stopped, and the DLL is loaded at launch, so each leg of an A/B needs a game restart: suspend,
launch, fly, quit, resume. Settings, receipts and backups stay. It takes the graphics proxy only, so a d3d11 chain behind it (Sean's
Epic `real_dll = d3d11_edhm.dll`, 3Dmigoto) is out of the run too: Elite loads Windows' d3d11.dll.

## 2026-10-16, round 2: leg A flown, binding thunks and the constructor body timed, designs for the guarded runs and the census

### Leg A (flown, confirmed)
Branch `claude/flat-settlement-cpu` (the observer fix and H1-H4) flown flat at a settlement. Render-thread engine motion fell from 427
to 77 ns a call (about 0.8 ms a frame); job-thread merge and clear fell 92%; 100% of calls took the lock-free path and the contention
line read 0. The fix alone is `claude/engine-motion-observers` (the seven commits, no instruments), full build green, for Sean to
merge. Leg A also ranked what is left of the 5.8 ms a frame: about 2.4 ms unattributed (binding thunks the prime suspect), about 2.1 ms
in the constructor body after the predicate block, and the guarded eager runs (below).

### Round 2 instruments (zero behaviour change, no config key)
(a) Binding thunks, `vscreen.cpp`. Each of nine thunks takes one scope, in six families split by kind (the VS and PS setters of one
kind share a family): `bind thunk shaders` (VSSetShader, PSSetShader), `bind thunk resources` (PS and VS SetShaderResources), `bind
thunk constants` (VS and PS SetConstantBuffers), `bind thunk targets` (OMSetRenderTargets, OMSetRenderTargetsAndUnorderedAccessViews),
`bind thunk viewport` (RSSetViewports), `bind thunk blend` (OMSetBlendState). The game's forwarded call inside each is a seventh,
`game bind calls` (kGameBind, kept out of "EDVR per frame total"), so a family is EDVR's own time and the driver call is apart. The
scopes sit after the `g_flatComputeInternal` early return, so EDVR's own binds are not counted, only the game's: calls a frame are
the game's binds. They use the census gate (the same one frame in 16) and the same exclusive accounting. The `thunkHits` counters
are NOT printed: the reclaim pass in vscreen.cpp zeroes them every pass, so they are not a per-window count; the scopes' calls are.
(b) The constructor body after `predicateBlock.stop()`, in five blocks, chosen by what each does (the shell family keeps what none
holds, and the copy draw's own work after `if (!copy) return;`, once a frame): `ctor reducer block` (to the Probe return: the deferred
inverse, the contract observation, HDR detector, trace record, copy admission, bookkeeping; its reducer, trace and HDR scopes come out
of it), `ctor nominate block` (source shape, `namingVetoed`, the untrusted nomination), `ctor domain block` (source naming, foreground
candidates, `observeDomainCandidate` and the explicit-domain classification), `ctor coverage block` (the substitution decision,
foreground release, the projection and coverage block), `ctor tail block` (the producer bracket, draw capture, projection binding,
HDR treatment, the UI layer's decisions). Five scopes a draw, ended with `Scope::stop()`.
(c) Cost: about 33 ns a scope on a clocked frame. This round adds 5 scopes per draw and 2 per bind (a thunk and its forwarded call).
They only run on the one frame in 16 the render thread is clocked on, and the line's own "the clocks cost the render thread about X
ms a frame" token prices them at that window's rates: read it first, and read the families against it. The packer's worst window is
now 7 lines of the 9 it holds (kMaxLines 9).

Line formats (examples from the rig's fake data, wrapped here; the real lines are single lines). Families, in the chain as before:
`... + game draw other 2.000 ms (calls 7400) + bind thunk shaders 0.300 ms (calls 3100) + bind thunk resources 0.400 ms (calls 5200) +
bind thunk constants 0.250 ms (calls 2600) + bind thunk targets 0.150 ms (calls 900) + bind thunk viewport 0.010 ms (calls 60) + bind
thunk blend 0.080 ms (calls 800) + game bind calls 0.900 ms (calls 12660) + ctor reducer block 0.300 ms (calls 10500) + ctor nominate
block 0.200 ms (calls 10500) + ctor domain block 0.500 ms (calls 10500) + ctor coverage block 0.450 ms (calls 10500) + ctor tail block
0.400 ms (calls 10500) + camera inject ...`, and one summary token: `; bind thunks (render thread, per clocked frame): EDVR 1.190 ms over
12660 calls (94 ns a call), the game's forwarded calls 0.900 ms over 12660 (71 ns a call)` ("-" for a nanosecond figure with no call).
If it never ran: every new family reads 0.000 ms (calls 0.0) and the summary reads `EDVR 0.000 ms over 0.0 calls (- ns a call)`; a
window with draws and no binds is as suspicious as one with binds and no draws.
Rigs: flat_cpu_tests.h (a bind window with exact per-kind ticks, the forwarded call kept out of both the family and the total, the
five blocks' accounting with a nested family and a gap that stays in the shell, the dashes of an empty window), the wiring pins in
flat_temporal_test.cpp (nine thunk scopes, ten forwarded-call scopes, five block scopes and their stops).
Reading the next flight: (1) `bind thunks` EDVR ms against the 2.4 ms unexplained; if the ns a call is near the clock floor it is
the volume, not the work. (2) the five ctor blocks against the 2.1 ms; the hot block names the next bisect. (3) `game bind calls` is
the driver cost EDVR cannot remove but could reduce by not forwarding a redundant bind (not proposed here).

### (d) DESIGN ONLY: why the overlay-guard runs are eager, and whether they can be lazy
Leg A: 895-974 guarded runs a frame, 13 D3D calls each, about 0.75 ms, 77% of engine-motion run time, about 12.7k D3D calls a frame.
Why eager (read from the code; `engine_velocity.cpp`: `restore()`, `snapshotOverlayBase`, the guard in `BeforeDraw`,
`engineVelocityFlatEndDraw`; commit 13b1b8ee, the lazy bracket, and 22fe85d2, the guard): the exact pair BBE58E40/DB3E8D20 (pool
family draws with depth writes off, `overlayDepthState`) is drawn through a guarded pixel shader that reads a private snapshot of the
slot target at PS t3 (`e.overlayBase`, one `CopyResource` per group, reused by the group's following overlay draws, a new group after
an intervening underlay draw or a new frame), so MRT6 gets only the fragments whose owner matches. t3 is the game's own PS SRV slot,
so the guard binds the private SRV over it for the draw and EndDraw is eager so the game's t3 is back "for its own draw only":
13b1b8ee's lazy bracket restores before any hooked call that is not a substituted producer draw, but the NEXT substituted producer
draw does not flush, and its original pixel shader may read the game's t3; a lazily kept private t3 would be sampled by it. That
is the only recorded reason (commit message: "with ... the overlay guard's private t3 bound, the game's state goes back at once
after each draw, as before"); the other half of the cost is that `BeforeDraw` flushes the pending run (`flatRestoreLocked`, cause
kOverlay) at EVERY overlay draw (`if (overlayPair && g_flatPending)`), so each guarded run pays the full bind and the full restore.
Is a lazy or batched form safe? Yes, with five conditions, each testable in the existing flat_lazy_tests rig:
 1. The private t3 may stay bound across consecutive guarded draws only (same guarded PS, same group snapshot). The next draw that is
    not a guarded draw of the same group restores the game's t3 first: in `BeforeDraw`, when `g_bound.guardSrv3` is set and this
    draw is not `guardOverlay`, put t3 back (one Set; the Get verify can go because the shadow's generation `srv3Gen` already says
    whether the game rebound it, and EDVR's own set is under `FlatComputeInternalScope`, which the shadow does not hear).
 2. Every flush already restores t3 (`restore()` handles `guardSrv3` first), so every hooked non-producer call, dispatch, clear,
    copy, resolve, command list, targets-keeping set and Present gives the game its own t3 back, exactly as for MRT6.
 3. The kOverlay flush at each overlay draw becomes a flush only when a NEW snapshot is needed (`!e.overlayGroup`): the copy
    wants the game's bindings (no MRT6 over the slot target, no private t3, `snapshotOverlayBase` refuses a slot target still bound
    as an output). Within a group no copy is made, so nothing has to be put back.
 4. A game rebind of t3 (or of PS, blend, targets) between two guarded draws is seen by generation, as it is for the lazy bracket;
    the guarded draw then re-binds, and the restore writes back only what is still EDVR's (`srv3Gen` compare, as now).
 5. `engineVelocityFlatEndDraw` stops treating `guardSrv3` as eager: `eager = !g_flatLazy` only. The cost: a game Get of t3 (no Get
    is hooked) between two guarded draws reads the private SRV, the same exposure MRT6 and the derived blend already have in lazy
    mode and for the same reason (any hooked call restores first).
Expected: a guarded run costs what a lazy run costs (4 calls with state to bind, about 0 when kept), plus 2 once a group: roughly
13 -> 1-2 calls a run, about 10k fewer D3D calls a frame and most of the 0.75 ms. NOT claimed: that no pixel changes (the guarded
pixel shader and the snapshot are untouched, so MRT6 must be bit-identical; the rig's guarded-overlay WARP fixture should compare
the eager and lazy forms' MRT0-3 and slot target byte for byte, and `flat lazy` already asserts that the overlay counters agree). The
flight to run first costs nothing: leg B's `eager guarded` line gives us/run and calls/run to compare with `lazy`; implement only if
`eager guarded` is still tens of us per frame there. Mutants to write with it: keep t3 across a non-guard producer draw (must fail),
skip the new-snapshot flush, restore without the generation test.

### (e) DESIGN ONLY: the census's own cost, for every flat user
`the clocks cost the render thread about X ms a frame` is the instrument's own price (leg A: about 0.46 ms a frame, a clocked frame
about 7 ms at 10.5k draws, hitching one frame in 16, and more with this round's scopes). Where it is gated: the flat runtime's State owns
`flatcpu::Census census{flatcpu::kRenderPeriod, flatcpu::clocksWanted()}` (flat_runtime.cpp, the member in the census section);
it runs only while a temporal mode is selected (`s.census.idle()`, called when none is, closes both gates, and then a scope costs
a load and a compare), and `clocksWanted()` is on under Windows and off under Wine unless `EDVR_FLAT_CPU_CLOCKS` says otherwise
(flat_cpu.h, `clocksPolicy`). So every flat user with DLSS, FSR, XeSS or TAA pays it. Options, none built:
 A. An adaptive period: kRenderPeriod 16 while draws a frame are under about 3000, backing off to 64 above it (the price is per
    clocked frame and grows with draws; a settlement frame is where it hurts and where the figure is least needed once calibrated).
 B. Off by default outside a diagnostic: clocksWanted() default false, on for `EDVR_FLAT_CPU_CLOCKS=1` or an F8 diagnostics toggle;
    a user report then needs the toggle, and `edvr_log.py` and the reports' tooling would have to say "census off" (the line already
    prints `clocks off, no thread clocked` with present p50/p95 and the GPU spans, so the log stays parseable). This needs a quoted
    key or env decision from Sean (Scope control) and a release-note line.
 C. Keep the families but drop the per-bind and per-block scopes from the shipped build (compile flag), leaving them for flights.
Recommendation for later: C for this round's high-count scopes once a flight has named the hot block, then A. B only if A is not
enough. The always-on part that remains is the gate load and compare per scope (about 1 ns), which is not what the 0.46 ms is.


## 2026-10-16, results: round 2 flown, and the AA-off A/B

Recorded from the coordinator's analysis of the two flat flights below; the logs are the evidence (check each with
`python tools\edvr_log.py --target steam --expect-build HEAD` before reading counters off it).

### Round 2 (`edvr_gfx_20261016_060143.log`, DLSS, 14.6k draws a frame)
- EDVR render-thread CPU, floor-net: 5.17 ms a frame, 353 ns a draw. Present p50 14.91 ms, GPU span p50 12.52 ms, 63.6 fps. The census's
  own cost: 0.67 ms a frame on average (design (e) above).
- Ruled out: the unscoped binding thunks as the 2.4 ms unexplained, because about 7.3k bind calls a frame at about 30 ns net is only
  0.12-0.24 ms.
- Ruled out: a single hot block in the draw constructor, because its five blocks hold about 0.85 ms net. The roughly 2.2 ms of "other",
  net, is spread over about 20 pieces, none above 0.6 ms: the shell residual 0.55, the tail 0.2-0.37 (with the per-draw UI-layer ask),
  the lookup block 0.33, the destructor 0.35, the reducer and nominate blocks about 0.2 each.

### AA-off A/B (`edvr_gfx_20261016_061811.log`, AA set off at the menu before loading, the same spot)
- 135-144 fps in the log and about 145-150 on Sean's counter, against about 150 with EDVR suspended. GPU span p50 4.4 ms.
- The always-on EDVR work is negligible (about 0.0005 ms a frame). The WHOLE gap, about 7.35 ms against the 14.25 ms production DLSS
  frame, is the AA path:
  - EDVR render-thread CPU 5.17 ms (about 70%);
  - timed GPU 1.18 ms (DLSS evaluate 0.94, the census spans 0.24);
  - about 1.0 ms untimed: the substituted pool-family draws' GPU cost (the census prints "pool-family draws -") plus driver-side state
    calls.
- With DLSS the frame is CPU-bound but co-limited: the CPU chain explains about 10.3 ms of the 12.5 ms GPU span, so CPU cuts pay about
  2.2 ms before the GPU chain binds.

### Next candidates (none built)
- The overlay guard (design (d) above), reading t3 from the binding shadow and skipping the verification Gets: 0.2-0.4 ms.
- The constructor's lookup memo: about 0.2 ms.
- A memo for the per-draw UI-layer ask: 0.1-0.2 ms.
- Turn on the census's "pool-family draws" GPU timing to price the roughly 1.0 ms untimed.
- STRUCTURAL: how much of the per-draw flat work is needed for draws that need nothing? A memoised per-pipeline-state "nothing to do"
  fast path would remove cost across the 20 small pieces that no single block change reaches. The question to answer first is what share
  of the 14.6k draws a frame are such draws, and what decides it (shader pair, targets, depth, camera rows).


## 2026-10-16, round 5 results: the claim regime measured, and the claim check

Branch `claude/claim-precheck` (from main eba550ad). BUILT, NOT FLOWN. The claim meter that took these numbers lives on the diag branch
`claude/flat-settlement-cpu-5`/`-6`, not on main.

### What the meter said (flight f4e167db, Steam, flat, log `edvr_gfx_20261016_102211.log`)
From 10:29:32 the claims began and stayed: the claim set was non-empty 13-21% of each 30 s window, 1.00 emissions a frame, claim lifetime p50 and p95
both 6554 us (consumed by the native copy; none cleared or dropped). The claim-set size was 1 at EVERY claimed call (merges 5.3M / 0 / 0 / 0, clears
1.9M / 0 / 0 / 0 in the 1/2/3-4/5+ buckets); claimed merges that touched a claim 1621 of 5.3M and claimed clears 0 of 1.9M, so the zero-hit share of
claimed calls was 99.98%. The entity: one rigid record a frame, a new address each frame, a near-static pose. Present p50 rose from 13.2-14.2 to
16.4-18.8 ms once the claims began. The lock lines in the same windows: merge begin and end about 5.3M acquisitions each, clear walk 1.9M, copier 3.7M,
contention 12-14%. The decision rule (zero-hit about 100%, set 3 or fewer) cleared the lock-free claim-set check; Sean approved building it.
Premise check before building: the claim does get hit once (a merge carries it, 1621 hits for 1621 emissions), so the check cannot assume "never
hit"; it takes the mutex on a hit and the hit path is unchanged.

### The fix (engine_velocity_primary_copy.h)
- `ClaimView`: the set of live claim addresses (at most 4, sorted; `kClaimViewMax`+1 as the count when more are live) and whether a merge plan is in
  flight, as an immutable snapshot under a seqlock, the `g_poolView` pattern. `ClaimLock::publish` rewrites it from the containers at every unlock and
  only when it differs, so it can lag only a critical section that has not finished.
- `clearClaimCheck`: reached when `g_claimsLive` is true. It reads the snapshot; with a plan in flight, more than 4 claims, a torn read or an empty
  snapshot (a flag one release behind) it returns at once and the clear takes the mutex as before. Otherwise it walks the dictionary with
  `walkDictionary` (the locked clear's own walk, now shared, so the two refuse exactly the same shapes) and tests each node's 8 record slots against the
  snapshot. Nothing held and the walk well formed: the call is counted and returns, no mutex, no address set built, no allocation (up to 64 nodes and
  64 entries; past that the visited sets spill as they always did).
- `mergeClaimCheck`: the same with `walkMerge` (the locked merge's walk, shared). A claim in any record slot the merge clears or overwrites (a source
  node's 8 slots, and the destination tail's slots [tailCount, tailCount+fill)) is a hit; no hit: the fixed no-claims token (no plan, no mutex, no
  allocation). Option 2 (a plan only when a claim is hit) is this: the plan is built only on the locked path.
- A hit, a plan in flight, more than 4 claims, a refused list or dictionary (a refusal clears every claim and bumps the epoch, which only the locked
  path may do), or any exception take the mutex and do what they always did, from the start. The merge-end epoch check and the plan's invalidation by a
  clear are unchanged, since they only exist on the locked path. Counters: `Claim check (live claims, none touched): N clears and M merges ... K calls
  declined it` joins the `lock-free exits` line; the figures sit inside the lock-free and call totals but not inside the no-claim skips.

### Linearization
Every call the check hands back is ordered at the instant it read the snapshot. The snapshot is a state some completed critical section left (publish
runs before the unlock, and the seqlock read is kept only if no write began or ended during it), so the call is exactly a call that took the mutex when
the claim set was that snapshot, found no claim in its list and no plan, and changed nothing. A claim published after that instant is ordered after the
call; the mutex allowed that order for a producer that had not yet taken it, and nothing the producer's thread did before returning from
`recordEmission` is missed, because that unlock published the snapshot before the call could have begun. A claim published BEFORE the call began is in
the snapshot (the producer's release and the caller's acquire through Elite's own hand-off order the stores), so the walk finds it if it is in the
list. The native list is not guarded by the mutex either way: the locked walk read it at a slightly different instant than the lock-free one, and
nothing in the locked code made list and claims an atomic pair. A torn read (a write in flight) is retried by taking the mutex, not by trusting
either half. What differs from the locked merge on a miss: it registered an empty plan; the token has none, so a concurrent clear cannot mark it
invalid (nothing to lose) and a native unwind of the merge now always revokes the claims registered since (the safe direction, already the token's
rule). x86 hides a weakened memory order from the rig, as for `g_poolView`.

### Rig evidence (tools\engine_velocity_test\claim_check_tests.h, 18,506 checks in the whole rig)
- C1: 1000 clears and 1000 merges under 1, 2, 3 and 4 live claims they do not touch: 0 mutex acquisitions, 0 heap allocations, no plan, claims and
  snapshot exact. The same calls with the check off (`g_claimPrecheck=false`, the rig only): 2000 acquisitions and 10000 allocations, so the first
  two checks FAIL on the old code. Mutex held by another thread: a clear and a merge with a live claim they do not touch return; with the claim in
  their list they block until it is released.
- C2/C3: the clear and the merge are compared with the pre-change code (the old clear and a copy of the old merge are in the rig) on 16 dictionary
  shapes and 21 list shapes, each with the claim in the list (hit) and with a claim elsewhere only (miss), the merge also with a frame the claim is not
  in: same claims after begin and after end, same transfers, failure, epoch and counters. The merge cases cover every slot class: a source node's slots
  including past the count, the first, middle and last filled tail slots, tail slots kept (below the count, a full tail), a tail of 0, three source
  nodes into a tail of 4, and six refusals (previous link, count 0, a self link, the last link, a tail count of 9, a tail whose next is wrong).
- C4: four claims (the limit) check lock-free; five take the mutex and are counted as declined; back to four, lock-free again; a claim in the list among
  six is revoked. C5: with a plan in flight a miss takes the mutex, walks and registers its plan, and a clear of the plan's list marks it invalid.
  C6: the snapshot equals the containers (sorted, plan flag, the more-than-four marker) after every entry point of every existing test, and is
  rewritten once per change and never for a repeated claim or a call that changes none.
- C7: a claim published inside a call's snapshot read tears the read: the call takes the mutex and revokes (clear) or stages (merge) it. A claim
  published after the check is ordered after the call and left alone, and the next call sees it. Four threads, 3000 rounds (a producer, a caller
  behind a release/acquire hand-off, a churn thread keeping the claim count between two and six): 0 claims lost, 0 wrong moves, the sentinel claim
  never erased, the snapshot exact at the end.
- Mutants (`mutants.py --run`, 15 for this check, each killed by the case named): a plan in flight ignored (C5, clear and merge), the 4-claim limit
  ignored (C4, both), hits ignored (C2), a refused walk or list accepted (C2, C3), source slots ignored, tail fill ignored, tail fill off by one,
  the slot window missing the last slot (C2/C3), the snapshot never rewritten (C1), unsorted (C1), omitting the plan flag (C6), the read not re-checked (C7).
- Bench (single thread, one claim live and untouched; ns a call): clear 1 node 69 -> 23, merge begin+end 314 -> 15 (8000 allocations per 1000 calls ->
  0); four threads contending on merge+clear 1922 -> 91 ns a call pair a thread.

### Flown (cpu-6 diag build, Steam, log `edvr_gfx_20261016_111653.log`)
Claims went live at about 11:36. In the first window 1.43M clears and 4.1M merges took the lock-free check, 0 declined (no plan in flight, no fifth
claim, no torn read counted). Present p50 went from 12.3-12.6 ms (no claims) to 12.8-13.1 ms (claims live), against 13.2-14.2 -> 16.4-18.8 ms in the
unfixed flight of the previous session: the 3-5 ms jump is down to about 0.5 ms. What that 0.5 ms is was not measured (the copier's mutex, the
walks themselves, and the claim-live windows differing in scene are all candidates); the copier is the named next candidate below.

### What the flight reads, and what is not done
Per 30 s: `lock-free exits` clear and merge near 100% of calls (they were 80% and 60%); `Claim check ... N clears and M merges` in the millions while claims
are live; `declined` near 0 (a plan in flight, a fifth claim, a torn read); lock lines: merge begin, merge end and clear walk n near 0 in claim-live
windows (the hits, once a frame, remain); contention near 0; `flat cpu` engine hooks back to about 0.14 ms; Present p50 back to 13-14 ms. If
`declined` is large, read which reason: only plan in flight, more than four claims and a torn read are counted. NOT done (adjacent, not folded in):
the pool copier takes the mutex on every call while a claim is live (about 3.7M acquisitions a window on the render thread, hold 0.1 us): the
snapshot could clear it for ranges holding no claim, with its pool-patch conditions; it needs its own argument.

## 2026-10-16, round 6: the pool copier's claim check

FLOWN OK (2026-10-16, flight build `claude/flat-settlement-cpu-7`, `edvr_gfx_20261016_135436.log`): claims went live at about 14:06:30 and the copier check took 6.0M
copies lock-free with 0 declined; with the clear and merge check, 3.0M merges and 8.6M clears lock-free with 2 declined; Present p50 11.5-12.0 ms before claims and 11.4-12.4
after: no jump. ON MAIN.

Branch `claude/copier-claim-check` (from main 34c100f5). BUILT, NOT FLOWN. Same idea as the clear/merge check, with the copier's own argument.

### Why the old lock-free exit did not cover the claim regime
`copierLockFree` hands a copy back only when `g_emissionsLive` is false AND no pool holds a patch (a global flag). At a settlement a claim is live 13-21% of
a window, and the join that consumes it leaves a patch in the pool until that pool's next DISCARD map, so a copy took `g_mutex` both while a claim was
live and for most of the frame after it. A check that only looked at claims would have left the patch half of that locked ("Limits" in the Status
block said the same), so the new check also reads where a pool's patches are.

### What the locked copier does (step by step), and when it changes anything
For 0 < count <= kMaxPatches and a source that does not wrap: (1) ++copies; (2) find the pools with an ACTIVE lease at `base` (two or more: clear their
patches and mark them invalid; none: ++copierNoLease); (3) one lease: a range that does not fit, or a stride that is not 336, clears its patches and marks it
invalid, a range that fits ERASES the pool's patches in [firstSlot, firstSlot+count); (4) for each i < count, look up the claim `source + i*336`: found is
consumed (erased even when the copy is refused) and, when the lease is valid, current and the bytes match, becomes a patch (the join); not found is nothing.
So the call changes shared state only through (a) a lease count other than 1, (b) a range that does not fit or a stride that is not 336, (c) a patch
in the range, (d) a claim in the range. A call with none of the four is the counter bump of step 1, and of step 2 when there is no lease. A pool with no
active lease at `base` is not touched at all, so its patches do not matter (the old exit ignored that and refused on them).

### The check (engine_velocity_primary_copy.h, `copierClaimCheck`)
It is tried after `copierLockFree` declines, and takes the mutex (the locked path, from scratch, unchanged) unless all of these hold:
- (d) none of the at most 4 claim addresses in `g_claimView` equals `source + i*336` for an i < count (the locked loop's own test); more than 4 claims, or a
  merge plan in flight, decline. The copier never reads `g_mergePlans`, so the plan condition is not needed for correctness; it is kept because the plan
  fallback was asked for and costs nothing (a plan exists only while a locked merge is between begin and end).
- (a)(b) the lease table has exactly one lease at `base` with stride 336 and the range inside its size, or none at all.
- (c) the lease's patch bound `[patchLo, patchHi]` does not meet the range. The bound is kept on `Pool`: `notePatch` raises it before the one insert (the join),
  `clearPatches` resets it at every `patches.clear()`, an erase leaves it wide. It is a superset of the slots, so it can only cause a lock, never a skip; a
  held map with a bound nobody raised publishes as "whole range". It is published per lease in `g_poolView` and compared on every republish.
- the read is one state: see below.
Success bumps `g_copierPrechecked` (and `g_copierPrecheckedNoLease` when it found no lease); `stats()` folds them into `copies`, `copierNoLease` and the lock-free
figures exactly as the other exits do, so every counter keeps its meaning. The exit line gains `Copier claim check (claims live or patches held, none in the
range): N copies of those, M copies declined it.` Declined means a plan, more than 4 claims, a leases-unknown table or a moved window; a claim, a patch or a
lease shape that needs the lock is a hit, not a decline.

### Linearization
The two tables (claim snapshot, lease table) are separate seqlocks, each written at the end of a critical section, one after the other. A reader of both could
see one section's claims with the previous section's pools, a pair no instant of the mutex's order ever held (a copy that neither consumed a claim nor erased
the patch that either order of the two sections would have). So `ClaimLock::publish` now opens a `PublishWindow` (`g_publishSeq`, odd while open) around every
write to either table, lazily, only when a table changes; the check reads `g_publishSeq` (acquire), both tables, then re-reads it, and keeps the read only if it
was even and unchanged. A kept read is the state exactly one completed critical section left. The call is ordered at that read: it is a call that took the mutex
when the state was that snapshot, found no claim in its range, one fitting lease (or none), no patch in the range, and changed only counters. A claim published
after the read is ordered after the call (the mutex allowed that order for a producer that had not yet taken it). A claim published before the call began is in
the snapshot: the producer's unlock published it and happens-before the call through Elite's own hand-off (the source is handed to the copier after
`recordEmission` returned), so the range test finds it and the call takes the mutex. A change in flight (odd, or moved) is not read: mutex, from scratch. The check
dereferences nothing; every address is compared as an integer. x86 hides a weakened memory order from the rig, as for the other seqlocks.

### Rig (tools\engine_velocity_test\claim_check_tests.h, C8-C12; primary_copy_observer_tests.h updated)
- C8: 1000 copies of an untouched range plus 1000 with no lease, under 1, 2, 3 and 4 live claims and a patch held: 0 mutex acquisitions, 0 heap allocations,
  claim-check counters 2000 and 0 declined, the no-lease split exact. The same copies with the check off (`g_claimPrecheck=false`): 2000 acquisitions each
  time, so the first checks FAIL on the old code.
- C9: 23 shapes (a claim in the first or last record of the range, one before, one after, no lease, two leases, a range past the lease, stride 320, a patch at
  the first or last slot of the range, one below, one past, a span over two patches, 4 claims, 5 claims, 4 with one in range, a plan in flight) run against
  the frozen locked copier (`referenceCopier`): same counters, claims, patches, lease state and epoch, and the expected lock-free or locked outcome.
- C10: five claims and a plan in flight take the mutex and are counted as declined; four claims and the plan's end make the same copy lock-free again.
- C11: a claim for the copy's own range published inside the read tears it (declined, locked, joined); a lease that ends inside the read tears it (declined,
  locked, no lease found); a claim published after the check is ordered after the copy (lock-free, claim left alone) and the next copy takes it.
- C12: four threads, 3000 rounds (producer, a caller re-mapping its pool every round behind a release/acquire hand-off, a claim churn between 2 and 6 claims, a
  lease churn on a second pool): 0 claims lost, 0 wrong, 3000 joins, 3.4e5-6.6e5 copies handed back, 6e3-4e4 declined.
- Changes to the existing observer rig, all consequences of the new exit: cases that were "locked" because the old exit was global now expect lock-free
  ("a patch outside the range", "a patch in a lease that ended", "a claim elsewhere"); the frozen `referenceCopier` calls `notePatch` at its one insert
  (the bound is part of the state it leaves); the "view moved between the reads" case runs with the claim check off (it is the old exit's own re-check);
  `viewExact` also requires each lease's bound to cover its patches and to say "none" exactly when it holds none; the "who waits" cases use copies that do
  hold a claim or meet a patch; 17 new equivalence cases sit beside the old ones.
- Mutants (`mutants.py --run`, 18 new, all killed; the rig stops at the first failing check, so the label is the first case that fails): plans ignored,
  limit ignored (C10); hits ignored, range one short, bound never raised, bound lost (C8; each also killed by C9 alone); ambiguity ignored, fit ignored,
  stride ignored, patches ignored, bound off by one at either end, bound change not republished (C9); window not re-checked, window never opened (C11);
  nothing counted, no-lease not counted (C8); declines not counted (C10).

### What the flight reads
`Copier claim check ... N copies` in the millions in claim-live windows (the 3.7M a window) and `declined` near 0; the render copier lock line n near 0 in
those windows (the join once a frame, and copies over a patch, remain); Present p50 in claim-live windows against the ~0.5 ms left after the clear/merge check.
If copies still take the mutex, the bound is the first suspect: a pool whose patches span the whole lease (a span over two patches locks the range between).
Not attributed: whether this is the remaining 0.5 ms; a flight decides. Ruled out while designing: gating on `g_claimsLive` alone, because the patch half is
the common locked case and a claims-only check would have left it.


## 2026-10-16: the offscreen fast path (class 4 of the draw class census)

FLOWN OK (same flight): about 3,720 draws a frame served by the fast path, against 3,773 offscreen draws a frame in the census. The saving was not clocked (the census is off by default in that build). ON MAIN.

Branch `claude/flat-offscreen-fastpath` (from main 34c100f5). BUILT, NOT FLOWN. The census (round 4, the diag branches) put offscreen draws at
3,773 a frame, 27%, 98% of them the same as the previous draw on target, pair and all. The round 3 design said such a draw only extends its target's
record and advances q. Reading the constructor for it showed that is only true while a short list of conditions holds; the work is naming them.

### What a class-4 draw is, and what it still needs
Class 4 here is stricter than the census's: not a motion source or tone or copy or UI family (the pair table says), no colour target or a colour target
that is not the output and not format 9, 23, 26, 27 or 60 (the formats the skipped blocks act on), no depth of the scene's size (the foreground domain's
candidate block reads it), and not a fullscreen triangle (the UI layer's tone probes and the back-off trail read it). `flatOffscreenShapeVerdict` is
that, pure. The constructor's blocks for such a draw are no-ops while the LIVE conditions hold (`FlatOffscreenLive`, eleven fields, each filled from the
state it names): the frame is Full; the GPU span is open and the foreground domain's frame reset ran; no ingress audit; no capture armed
(`flatTemporalCapturing`, a draw capture, draw packets); no foreign work; no overlay suffix open or active; no F10 projection audit; no weapon footprint
probe; the foreground probe is not active on a draw with a depth; and the HDR detector could not consume the draw. What it still does: `s.drawVs/Ps`,
`engineVelocityFlatLazy(true)`, `flatRuntimeSubstitution` (the `!d.supported` call: engine motion's state goes back), the reducer for real, the HDR
detector for real, the trace record for real, the new-target bookkeeping, and the UI layer's scene-draw note. Nothing the reducer, detector or trace
read is cached except the draw record, which is a pure function of the key.

### The key, and why there is no epoch word
`FlatOffscreenKey`, fifteen inputs, each named in the header and each shown necessary by two draws that differ only in it (O1): the shader pair, the
colour, depth, RTV, DSV and b1, the camera rows' hash and write epoch and sequence (the camera-row epoch) and whether there are rows, the extents and
formats, the viewport, and the generations of the RTV and DSV bindings. The frame rides beside it. The design asked for an epoch word bumped at every
write to the frame-scoped states; that is a list of writers to find and a missed one is silent. The live conditions are READ at every draw instead
(about a dozen loads), so there is no writer to miss, and the rig pins each field to its source (O5). The memo holds the previous verified draw: one
whose shape verdict and live verdict were None and whose slow path planned and started nothing (no producer, projection binding, capture, packet,
overlay, foreground, untrusted or domain plan, no HUD take, tone, composite or replacement, no HDR trigger, no camera probe), registered no target and
did not change certainty.

### The decisive replay (O7)
Every frame of the committed corpus (tools\flat_temporal_test\traces, 17 files, 46 frames, 44,261 draws) is replayed twice through the reducer, once as
recorded and once with the rules: a candidate draw that repeats the verified key takes the memo's record in place of its own. 17,158 draws were
candidates and 16,314 (36.9%) were memo hits; every frame's contract hash and prefix digest (every target's record, every source's, the counters) is
identical, and every hit's cached record EQUALS the event's own field by field but the instance count. The corpus alone cannot tell any single key
input apart (no two candidates in it differ in exactly one input), which is why O1 proves each input on constructed pairs.

### The audit as a test (O5)
Each gate that makes a skipped block a no-op is a pinned string of the source (verifyHdrCopy and its siblings off format 26 or 9, the F10-only captures,
the weapon footprint, the cohort and overlay candidates, sceneExtent, the untrusted nominee, the foreground gap and domain block, the coverage
classification, the UI layer's family, copy and triangle branches, the reducer's colourless return and format guards, the detector's gate). A change to
any fails the rig until the fast path is reviewed against it. Against origin/main's flat_runtime.cpp the wiring check fails (the functions are not
there): the fail-before. 49 mutants of the headers (`offscreen_mutants.py --run`) are each killed by the case that owns the rule.

### Measurement
The `flat cpu 5s` line carries `offscreen fast path: N draws a frame skipped the draw scope's constructor and destructor (V a frame were verified on the
slow path first; D a frame matched but had to ask why not: reasons)`, a counter that prints with the timing off, and the timing-on line has a family
`offscreen fast path` (calls = the draws served). The saving is the served draws times the difference between a class-4 draw's slow cost and the fast
path's: not measured here; the flight reads it from the timing-on line (the ctor families against the previous build's) and from Present.
Expected: 3,773 draws times 100-250 ns is 0.4-1.0 ms a frame, if the census's share holds in the flight. If `matched but had to ask why not` is large,
the reason names the live condition to refine (foreground active is the likely one on foot).

### Not done
The slow path's first class-4 draw of each kind per frame (verified, then remembered) is the `verified` count in the token. Class 4 only; the other
classes are not touched. The memo is one draw deep: an alternation between two offscreen targets misses every time (the counters would show a large
verified count and a small served one).

## 2026-10-16: the CPU census off by default, and the flat Debug tab

Branch `claude/flat-debug-menu` (from main 34c100f5). BUILT, NOT FLOWN. Sean's decision after the settlement flights: the census costs every flat user with a
temporal mode on a hitch (a clocked frame runs 7-10 ms long at a settlement, once in sixteen frames: Present p95 about 22 ms against a p50 of 13) and
about 0.27 ms a frame on average; so it is off unless a user turns it on, and a sampling rework was set aside (nothing of it is built).

- **The switch.** `advanced.flat_cpu_timing` (on/off, default off, live; documented in edvr.ini; in the flat allow-list `flatProfileReadsKey`). The
  flat runtime reads it once a Present (flat_runtime.cpp, before `s.census.onFrame`) and calls `Census::setClocks(on, now)` (flat_cpu.h). A change
  starts a fresh window; off closes the gates at once, on opens them with the next frame and calibrates the clock floors at the close of its first
  window. One log line when the session starts and one per change: `flat runtime: CPU timing in the log on|off (advanced.flat_cpu_timing = on|off[;
  EDVR_FLAT_CPU_CLOCKS decides it])`. The environment variable stays: `=1` forces the timing on, `=0` off, whatever the key says (`clocksPolicy`).
  The Wine-only default is gone (off everywhere).
- **Off is the existing `clocksOff` mode.** No thread is clocked on any frame, so no scope reads the clock (engine motion's too: the census drives its
  gate) and no floor is calibrated; the rig counts the reads on the render thread across 320 frames of scopes and the window close: zero. The
  `flat cpu 5s` line is still written every window: `flat cpu 5s: frames=N (...) present p50 X ms (p95 Y); CPU timing is off; turn it on in F8 > Debug >
  CPU timing in the log = render thread: CPU timing is off, no thread clocked (EDVR_FLAT_CPU_CLOCKS=1 also turns it on); other threads: CPU timing is off;
  ...` followed by the parts that cost nothing: the camera witness count, the wrapper's D3D calls over substituted draws, the query shortcuts, the GPU
  spans and the instrument-cost token (zeros). The Present figures and GPU spans keep the log readable for frame time without the timing.
- **The tabs.** The flat panel (menu.cpp `buildPages`, rows in menu_flat_rows.h) had one page, "Flat graphics". It is now **Performance** (renamed, the
  same six rows in the same order: intro movie, anti-aliasing, DLSS preset, sharpening, interface quality, optimized stations) and **Debug**, last,
  with a heading ("For bug reports. Both are off unless you turn them on.") over the two toggles, in the generated order: CPU timing in the log, then
  Motion source view. Both pages are built from tables (`kFlatPageRows`, `kFlatDebugRows`); `flatPageFirstRefused` checks both against the flat gate.
  The keyboard footer gains "Tab page" (the pad's "LB/RB pages" was there already).
- **The generator.** `menu debug` is a new page token (`MENU_PAGES`); an [advanced] key may carry a menu token only if named in
  `MENU_ADVANCED_EXCEPTIONS`, which now names `advanced.sharp_weapons_blend`, `advanced.flat_cpu_timing` and `advanced.flat_refusal_view`. The rows
  are Fix tier on the page `debug`, one row each (the developer tier does not repeat them, so VR's developer Advanced page no longer lists these two
  flat-only switches); VR builds no `debug` page. Self-test cases: the rows and their order, and any other [advanced] key with `menu debug` refused.
- **The motion source view** is `advanced.flat_refusal_view`, which exists and was not touched by the 2026-10-09 cull (that took VR's eye-path
  `temporal_aa_debug`): see the flat temporal design doc, section "Flat Debug tab: the motion source view". Only its panel row is new.
- **Rig.** tools\flat_sharpen_test (the generated table: two tabs, Performance's rows unchanged and in order, Debug's rows labelled, switches, shipped off,
  live, Fix tier, page `debug`, once each; menu.cpp builds both tabs from the tables, with controls; the flat gate admits both keys and a gate
  without either is caught by name, another [advanced] key stays a developer row); tools\flat_temporal_test (timing off reads no clock, on clocks one
  frame in sixteen from the next frame and calibrates at its first window's close, off again closes the gates at once and starts a fresh window, the
  line's text on and off, `clocksPolicy`); gen_settings_schema --self-test; check_config_contract.
- **What a flight reads.** `flat runtime: CPU timing in the log off (advanced.flat_cpu_timing = off)` at the start; the `flat cpu 5s` lines say the timing
  is off; Present p95 at a settlement near p50 (no clocked-frame hitch). Turned on from F8 > Debug: the line logs the change and the next window's
  line carries the families again. NOT measured here: the off state's cost (the scopes' gate checks remain: a load and a compare each, about 130k a frame
  at 14k draws).

## 2026-10-17: carrier terminals, elevator and vault hallway frame drops

The reporter describes roughly 120 to 40 fps near a carrier terminal and
front elevator, with the worst view looking down the rear hallway from the
vault side of the glass. Bundle `edvr-logs-20261017-164941.zip` contains
`edvr_gfx_20261017_160006.log`, 48,377 lines over about 49 minutes. It does not
mark those exact views, so timestamps cannot be assigned to individual rooms.

Build verification: `c7c14898`, PE build `6AD3FA41`, flat `/novr`, scope both.
The explicit expected build is C7, not the current rc.4 tag on `735f495d`:
the intervening commits only change Python installation/docs, and
`git diff c7c14898 HEAD -- src/d3d11 src/common src/openxr` is empty. This is
the fixed AA rendering code. Environment: RX 9070 XT, FSR 3.1.2, native
3440x1440 HDR/output, VSync off, origin probe off, UI quality off. VR runtime
and headset are not involved. Early/live AA mode switches are separate from
the steady FSR windows below.

| Log window | Present p50 | Frames per 5 s | Estimated EDVR CPU after reported clock-floor deduction | Copy-check family, measured |
|---|---:|---:|---:|---:|
| 16:40:49 | 24.93 ms | 199 | 9.56 ms | 4.755 ms / 16,863 calls |
| 16:46:39 | 24.71 ms | 199 | 9.82 ms | 4.884 ms / 16,844 calls |
| 16:48:59 | 8.35 ms | 591 | 1.09 ms | 0.232 ms / 1,270 calls |

Twenty-five timing-on windows have Present p50 at least 20 ms; their median
is 22.67 ms. Slow-group median EDVR CPU after the reported floor deduction is
8.749 ms, versus 2.303 ms for the under-10-ms group. The individual examples
above are recovered/heavy views, not a controlled same-view AA-off baseline.
The measurements support a substantial draw-dependent EDVR CPU contribution;
they do not assign the entire frame-time difference to EDVR.

The GPU first-game-draw-to-Present span grows from 7.90 to about 24.9 ms. This
includes gaps while the CPU supplies commands, so it does not prove GPU
saturation. The measured resolve grows only from 1.34 to about 2.1 ms.

Ruled out: recurrence of the AA admission/reset loop as the cause of these
steady slow windows, because they retain nonzero phases, valid history and
zero row mismatches while refusal/failure totals remain unchanged. Cumulative
backend failures are 191, accrued earlier around rapid mode changes; they are
not zero for the whole flight and must not be attributed to these later
steady FSR windows.

Ruled out: debug timing as the onset, because a 25.50-ms p50 window already
occurs at 16:34:33 before CPU timing is enabled at 16:34:50.421. The slow-group
logged average clock cost is about 0.253 ms/frame, distinct from the roughly
2-ms floor contamination in a sampled frame's displayed family totals.

Ruled out: the motion-source view alone, because the 16:40:49 heavy window is
inside its off interval, 16:36:17.297 through 16:42:00.626. The view initially
enables at 16:34:41.517, also after the first cited slow window. Bundled final
INI values are not the settings for the whole flight.

Ruled out: a manual diagnostic capture causing this run, because no General,
Full, trace, draw-packet or light-audit arm appears. Only startup passive
discovery ran. Production `FlatWork::Full` is not a manually armed capture.

Source audit: `kCopyChecks` wraps forward camera/depth provenance, copy/menu/
image/deferred checks and bounded captures; a second scope covers camera
conflict capture. The 16,863 calls therefore correspond to roughly 8,432 slow
draw entries, not that many output scans. `forwardBefore` calls
`forwardDrawWrites` before shader filtering. Once watching resources, it reads
all eight RTVs, 8 or 64 UAV slots depending on feature level, blend state and
bound resources. That scan is included in the measured family, but its exact
share is not separately timed. The previous isolated RTX 5090 97–138-ns query
benchmark does not bound this AMD flight's complete function or driver cost.

Foreground capture is also production work: original-VS stream output,
state/projection checks and owner-marker preparation. Later motion-map replay
is under resolve. The motion view adds classification writes/reads in existing
GPU passes; it does not enable the broad per-draw output scan.

Ruled out: an overflow-only scan shortcut as a useful fix for this report,
because every one of the 588 forward-refusal summaries has overflow zero.
All 588 forward summaries also have qualified zero: optional forward admission
never succeeds, while the independent AA route continues. An all-failed-domain
shortcut still needs proof that a later healthy domain/authority cannot be
missed; do not bypass actual mutation checks from these aggregate counters.

Next investigation: split/measure the copy-check family and its actual query
counts, then prove a fast path or complete binding-cache invalidation before
changing rendering code. Preserve actual-state fallback for uncertain
bindings. Existing data is enough to choose this target; no further carrier
walk or generic graphics-setting advice is requested yet. No C++ or live
configuration change was made for this report.

## 2026-10-17: separate copy checks before choosing a performance fix

The user approved proceeding with cost isolation and a safe reduction audit.
Work is on `codex/flat-copy-check-costs`. The current evidence does not price
the forward output scan separately and does not justify disabling mutation
checks whenever forward admission happens to fail in an observed scene.

Discriminating hypotheses for one instrumented capture:

- Output binding/mutation queries dominate: the output-scan timer is large,
  with actual OM/blend/resource-query counts scaling with slow draw entries.
- Raw-depth provenance dominates: the domain timer is large while output
  queries account for little of the parent family.
- Camera or existing copy/deferred checks dominate: qualification or the
  remaining verification timer accounts for the difference. Cheap exact-pair
  filters remain outside expensive-path timers.
- Conflict/capture bookkeeping dominates despite no manual arm: its separate
  timer/calls expose that path rather than attributing it to output queries.
- A failed-ledger fast path is useful: sampled scans spend meaningful time
  with no healthy raw domain, no live publication and no current authority.
  If other healthy domains/publications keep watching necessary, the cohort
  counts will refute this shortcut without another hypothesis-only build.

Use the existing CPU-timing switch and one-in-16 schedule, with no new key or
manual capture requirement. The existing headline copy-check total and call
count must retain their meaning. Nested exclusive child timers are folded
back into that headline; total EDVR CPU must count each cell only once.
Separate bounded lines report the breakdown and exact queries within clocked
frames, with calibration including the new scopes. Timing off must read no
clock, update no query counter, and evaluate no shadow predicate.
The output-scan cells are exclusive: add `output scan` and
`output inactive-ledger` for the complete actual-scan cost. The former's call
count includes all scans; the latter counts only the advisory inactive subset.

The stateless candidate predicate conservatively watches current live
authority, every live publication and any nonfailed raw domain. Unknown
epochs/state and pending issues retain the watching fallback. It is only an
advisory shadow probe: the real full actual-state scan always runs. The
pre-current-issue owner seam has no global pending-issue publication; outer
issue/reentrancy safety still requires proof before this could become an
actual scan gate. No cache is inferred from the existing incomplete binding
cache, and no refusal, camera, phase, history or renderer policy is relaxed.
The source audit found converter callbacks only mark an already-live
publication and cannot revive an invalidated one; pending light issues require
a live publication, and geometry return cannot clear a domain failure. The
later production audit below resolves how this supports a reentrant skip.

Offline validation must exercise profiler partition/aggregate reporting,
timing-off no-work, query counts, logging bounds, failed/new-domain and
publication transitions, current/revoked authority and reset/unknown-state
fallbacks. Measure default-off gate overhead separately from GPU work and
from any claimed game FPS benefit. Build and test results follow.

The focused rig passed 50 watch checks and 28,561 actual-ledger state
sequences, plus the existing 166 forward-core, 114 integration and 9,570
phase-bound checks. The profiler fixture reconciles the legacy aggregate and
parent call count, sampled query counts, window reset, timing-off no-work and
bounded output. The full gate will also exercise maximum query-counter values.

An ignored CPU benchmark on Ryzen 7 9800X3D, MSVC 19.44.35228.0 and Windows
26200 measured median incremental layout costs of 1.380 ns/draw with timing
off and 4.775 ns/draw with one-in-16 sampling (five repeated medians, identical
dummy work). The local RTX 5090 full-query fixture cost 104.5-145.9 ns/check;
the pure predicate cost 2.1-2.3 ns/check. These are isolated layout/query
measurements, not the complete production function, the reporter's AMD driver,
the real inactive-cohort rate or an FPS prediction. Evidence and exact method:
`build/rc3-latest-review/forward_watch_probe_audit.txt` and adjacent raw results.

Full validation: absolute `build.bat --wait-lock` passed in 166.5 seconds,
including Python gates, both production DLLs, regression rigs, installer
resource checks and receipt. The temporal rig rebuilt after the final
maximum-counter fixture and passed; no shared focused-test artifact was reused.
Receipt `199a6f501ae4f4d1431fae3ec8a2a77dd1f6ad63b406040d4b59e71299344209`
matches the final source tree. Log: `build/flat-copy-costs-full.log`.

Next evidence, after installing this instrumented build: keep the same flat
FSR/resolution settings on the reporter's AMD setup. In F8 > Debug enable
`CPU timing in the log`, disable `Motion source view`, then close the menu.
Stand still facing a fast view for 30 seconds, a reported slow terminal or
hallway view for 30 seconds, and the first view again for 30 seconds. Note the
transition times and save a log bundle. No General/Full capture or origin probe
is needed. Verify the new build first, then pair `flat cpu copy checks 5s`
with the existing headline and AA/history summaries by window/frame. A zero
inactive count refutes this proposed shortcut for that capture; a large cohort
only motivates an optimization if its measured scan time is meaningful.

No game install was performed in this task. No scan skip, binding cache,
rendering change, configuration key or performance improvement is claimed.

## 2026-10-17: Steam carrier fast/slow/fast cost capture

The follow-up installed and separately verified the flat payload using
`tools/install_edvr.py --target steam --scope flat`; settings were preserved.
Sean completed the requested fast/slow/fast comparison and quit normally.
`tools/edvr_log.py --target steam --expect-build HEAD --version` verified
`edvr_gfx_20261017_185739.log`: `v0.20.0-rc.4-2-g137d4743`, PE `6AD4140F`,
flat `/novr`. Subsequent reads pinned `--expect-build 137d4743`. This is the
local RTX 5090/DLSS setup, 2496x1404 rendering into 3840x2160, not the AMD/FSR
reporter's system. CPU timing was on from startup, Motion source view off.
The build did not record the DLSS runtime or driver version.

Seven steady five-second windows in each group, log-local timestamps:

| Metric | Fast before, 19:01:02-32 | Slow, 19:01:42-19:02:12 | Fast after, 19:02:22-52 |
|---|---:|---:|---:|
| Frames / clocked frames | 3151 / 197 | 2546 / 160 | 3138 / 196 |
| Present p50 (ms) | 11.11 | 13.45 | 11.10 |
| GPU first-draw-to-Present span p50 (ms) | 6.36 | 13.06 | 6.74 |
| GPU resolve p50 (ms) | 0.95 | 0.95 | 0.95 |
| EDVR CPU gross / estimated floor-net (ms) | 2.543 / 1.976 | 7.943 / 6.204 | 2.566 / 1.996 |
| Copy checks aggregate (ms) | 0.754 | 3.483 | 0.759 |
| Raw-domain checks (ms) | 0.294 | 1.413 | 0.296 |
| All output scans (ms) | 0.275 | 1.291 | 0.277 |
| Inactive subset of output scans (ms) | 0.202 | 0.981 | 0.204 |
| Remaining copy-check residual (ms) | 0.181 | 0.775 | 0.182 |
| Actual output scans / clocked frame | 1154.84 | 5473.26 | 1145.70 |
| Inactive candidates / clocked frame | 979.15 | 4803.83 | 970.40 |
| Inactive fraction of actual scans | 84.79% | 87.77% | 84.70% |

CPU values are weighted by clocked frames; Present/GPU figures are medians of
the window p50s. The estimated total floor correction was 0.567/1.740/0.570 ms
per clocked frame; no floor was allocated to individual child families. All
output time adds exclusive normal and inactive cells once (slow:
0.310 + 0.981 = 1.291 ms); the inactive row is a subset, not another addition.
Query-count bookkeeping is not floor-corrected. No measured gross family cost
is a guaranteed net saving or an FPS prediction.

The slow group executed 875,722 actual OM Gets and 768,613 inactive candidate
scans: 7,005,776 RTV slots and 56,046,208 UAV slots requested/scanned, with
zero bound UAVs, and 3,835,394 GetResource calls. Shadow unknown was zero in
every comparison group, so the nonzero cohort is an observed known-state
result, not absence of sampling. Slow copy-check shares were 40.57% raw domain,
37.07% all output scans, and 28.18% inactive scans (the latter inside outputs).
Converter/forward costs rounded to 0.002/0.001 ms; verification/manual-conflict
had zero calls in these sampled windows. Earlier at 19:00:22 the inactive
fraction was only 20.1%, demonstrating the predicate did not label every scan
inactive. Per-reason live-authority/publication/healthy-domain counts were not
logged, so the remaining scans cannot be classified more precisely.

Across 19:01:00-19:02:55, all 24 history-injection summaries reported valid
history and clean closes, with zero stale/write/flush/off-thread/history-reset
or fallback counts. Frame refusals stayed at 684; backend failures stayed zero
and reset/camera-cut/origin-ambiguity counters did not grow. Forward qualified
564 and overflow 8 were historical unchanged totals. Phase failures were zero,
but the selected forward phase receipt was still awaiting bootstrap; this
capture is not proof of a rendered forward-phase certificate.

Ruled out: an inactive-ledger shortcut having no useful cohort in this scene,
because the running probe observed 87.77% of slow-view scans inactive.
Ruled out: output scans alone dominating the copy-check aggregate here,
because raw-domain checks were larger (1.413 versus 1.291 ms).
Ruled out: new backend failures or history resets explaining this comparison,
because their counters stayed unchanged while cost rose and recovered.

Next: complete the offline guarded-skip proof around owner/epoch uncertainty,
pending issues and external-call reentrancy, keeping the actual-state fallback
whenever demand is unknown. The existing conservative cohort keeps every live
publication, authority and healthy raw domain watching. Its measured cost
justifies that work; it does not authorize weakening provenance checks or
claim a fix is validated. Raw-domain work is the larger remaining audit target.
No further flight is needed before that code audit. The log does not label
specific rooms, prove a 90-fps cap or reproduce the AMD user's 120-to-40 drop;
the GPU span can include CPU starvation and is not GPU-busy time.

Independent reader spot check at 19:01:57.836, frame 23996: 22 clocked frames,
123,978 actual OM Gets, 109,139 inactive candidates, zero shadow unknown;
3.566 ms aggregate; the exclusive breakdown was 1.447 raw, 0.314 normal
outputs, 1.008 inactive outputs, 0.792 residual, 0.002 converter and 0.001
admission (rounding applies).
Parsed evidence from the sanctioned reader is in ignored
`build/latest_steam_perf_group_summary.json`, `build/latest_steam_perf_rows.json`
and `build/latest_steam_perf_stdout.txt`; the canonical evidence is the pinned
flight log and the numeric summary above.

## 2026-10-17: additional slower carrier view, about 66 fps

While the first analysis completed, Sean found a slower view and reported
66 fps after a stationary hold. The same verified 137d4743 log continued;
the exact room/view was not supplied. Six complete windows ending
19:06:43.153-19:07:08.198 cover 2,008 frames over 30.045 seconds: 66.83 fps.
The 126 clocked frames give this comparison against the first slow group:

| Metric (ms) | Original slow | New plateau |
|---|---:|---:|
| Present p50 | 13.45 | 14.545 |
| GPU first-draw-to-Present span p50 | 13.06 | 14.255 |
| GPU resolve p50 | 0.95 | 0.93 |
| EDVR CPU gross / estimated floor-net | 7.943 / 6.204 | 8.817 / 6.949 |
| Copy-check aggregate | 3.483 | 4.021 |
| Raw-domain checks | 1.413 | 1.634 |
| All output scans | 1.291 | 1.492 |
| Included inactive output subset | 0.981 | 1.114 |

CPU times are clocked-frame weighted; Present/GPU are medians of window p50s.
Actual output scans averaged 6,159 per clocked frame; 5,279 were inactive
candidates (85.71%), with zero shadow unknown. The inactive subset was 27.70%
of copy-check time; raw-domain checks remained largest at 40.63%. These are
instrumented costs, not proven net savings. GPU span is not GPU-busy time.

No new treatment refusal, history reset, camera cut, backend failure or
injector write failure accompanied the slowdown. Frame refusals stayed 684
and historical overflow eight; timing stayed on, motion view off. The forward
phase receipt continued awaiting bootstrap. This reinforces the same CPU-cost
finding rather than identifying a new AA failure. The next step remains the
offline guarded-skip safety proof; no further stationary capture is needed
before that audit. No code or game installation changed during analysis.

Ignored evidence: `build/carrier_66fps_group_summary.json`,
`build/carrier_66fps_rows.json`, `build/carrier_66fps_stability_stdout.txt`.

## 2026-10-17: guarded output-scan implementation audit

The user approved proceeding with the guarded-skip audit and implementation.
The measured hypothesis is that NoUsableState contains no admission or
revocation work for output mutation: every used raw domain has failed, every
publication is nonlive, and every authority is nonlive. Slow captures already
confirm this is a large real cohort, not an assumed performance opportunity.

The source audit finds mutation cannot heal failed raw domains or revive
publications/authority. A subsequently created domain/publication did not
exist for the baseline's earlier scan either; recomputing demand next draw
restores scanning. Converter callbacks only mark already-live publications,
light return requires a live publication, and geometry return cannot clear a
failed domain. Skipping can retain an earlier
nonzero failure reason instead of replacing it with another nonzero reason;
that distinction must be documented/tested without relaxing admission.

The diagnostic probe's known-state gate is insufficient as production policy:
add actual owner/draw-context identity, prefix certainty and established epoch.
The decision itself calls no D3D/external API and changes no state; nesting
before or after it recomputes demand. The known pending Issue kinds cannot
make an elided failed-domain mutation useful later, so no new scope counter
is required. Unknown ownership/context/epoch or invalid state retains scanning.
Current-frame live authority/publication and every healthy domain always scan.
Overflow cannot hide live authority/publication, even though the ledger then
rejects admission; failed overflow state is subject to the same inactive cut.

Acceptance evidence before installation: transition/revocation equivalence,
late/pending issue callbacks and nested state changes, changed frame/context,
invalid/overflow state, diagnostic counts distinguishing actual skips from
observed candidates, no clock/counter work when timing off, and measured guard
overhead with timing off and sampled timing on. Run the full absolute
`build.bat --wait-lock` after all sources freeze. No new config key, binding
cache, raw-depth optimization or camera/phase/history policy change is in scope.

Corrected assumption: converter IssueValid can succeed after its publication
was invalidated. Safety relies on Ledger::issued/returned never setting live
true, not on IssueValid requiring a live publication. Tests must exercise that
late valid-return case explicitly. Pin the current Issue kind count and test
actual Ledger completion/replacement/reset behavior. The runtime callback
semantics were manually audited; tests do not execute D3D callbacks or prove
future callback edits cannot add state-healing behavior. Such edits require
reassessment of the representation argument.

Implementation is limited to the output scan seam: the actual draw context
flows from the draw constructor into forwardBefore/forwardDrawWrites; owner,
context identity, upstream ownership, device/context presence, current ledger
epoch, prefix certainty and foreign-work state gate the decision. The existing
complete publication walk is reused. NoUsableState returns before any D3D
query; every other result retains the original full scan. Raw-domain/color,
dispatch/write and issue-validation/return/abandon observers are unchanged.

Demand is functional and evaluated every watched draw with CPU timing on or
off. Existing sampled diagnostic counters now describe decision samples;
additional actual guard checks/skips/full-scan-fallback counts expose the
production result. The output-scan family includes decision CPU time; the old
inactive-scan family stays zero because skipped query work is not executed.
Zero there is not a measurement of time saved. Timing off reads no profiler
clock and writes no query counter while the functional guard still runs.

Focused tests cover actual Ledger late/invalidated Converter returns, replaced
tokens, reset with delayed prior-token callbacks, alias invalidation, fresh
authority, nonhealing Geometry, malformed/unknown state and admission/revocation
equivalence across 28,561 short event sequences. The final watch suite passed
60 checks; the focused temporal rig also passed the existing core/integration
and 9,570 phase-bound checks. Source wiring checks verify the guard precedes
queries and receives the actual draw argument. The callback proof includes a
manual source audit, not execution of the D3D runtime callbacks in these tests.

An ignored no-dummy CPU benchmark measured median incremental guard costs of
4.004 ns/call for healthy raw state, 2.917 for all-failed state, 3.190 for a live
publication and 2.201 for unknown state. Five repetitions each used medians of
five 100,000-call batches after 1,000 warmup calls; an observable returned
reason was consumed equally by both sides. The fuller dummy-work layout was
about 31 ns on each side because independent guard work can overlap the dummy
chain; that is not evidence of a free guard. No D3D/game/GPU/FPS measurement.
Exact results: `build/rc3-latest-review/forward_output_guard_bench_results.txt`.

All tracked sources froze before the root's absolute full build. After its
gates, commit/push the branch and promote clean DLL versions, then install for
Steam flat using the sanctioned installer. Runtime validation must show
nonzero actual skips, fewer actual OM/output queries, healthy history and
continued admission when useful state appears. Compare the same slow/fast/slow
carrier views with timing on and Motion source view off; keep the origin probe
off and usual AA settings. Merge main after validation, with no FPS promise.

Full validation passed in 160.2 seconds: Python gates, both production DLLs,
all regression rigs, installer resource checks and receipt. The temporal rig
passed the final 60 guard checks/28,561 sequences, core 166, integration 114
and phase-bound 9,570 checks (2,016 independent engine cameras). Receipt
`9867ad15a04c5626e80cb2cedaaeeb8ea486c8662eb1c8614241f44e2ed66882`
matches the frozen source tree. Log: `build/flat-output-guard-full.log`.
Only documentation changed after the gate; no further C++ edits or builds by
the agents. The host process check found Elite closed before installation.

Stationary validation plan: launch Steam flat with usual AA selected, origin
probe off, CPU timing on and Motion source view off. Let the main-menu scene
run for 10 seconds, then load the carrier and hold the same slow view for
30 seconds, a fast view for 30 seconds and the slow view again for 30 seconds.
Record FPS and any visible AA/history regression; quit normally afterwards.
The log must show actual guard skips and fewer OM/output queries, continued
full scans where demand remains, and healthy treatment/history in stable
windows. The query count/time comparison, not the old inactive timer's zero,
will establish whether an actual saving was realized.

## 2026-10-17: combined Steam guard validation and evidence audit

Sanctioned reader verifies `edvr_gfx_20261017_195802.log` as installed
`v0.20.0-rc.4-8-ga4a4e751`, Steam flat, RTX 5090, DLSS model M,
2496x1404 into 3840x2160. CPU timing on, Motion source view off, origin probe
off; settings preserved. Sean reports 64 fps in the slow view and 90+ after
turning around. Primary groups stop at 20:01:34.626, the completed capture;
later live rows are excluded, as are loading and turn-transition windows.

| Group / window ends | Frames / clocked | Present / GPU span ms | EDVR gross / estimated floor-net ms | Copy / output / raw ms |
|---|---:|---:|---:|---:|
| Slow 20:00:14.519-29.548, 4 windows | 1246 / 78 | 15.81 / 14.81 | 8.519 / 6.550 | 3.211 / 0.306 / 1.909 |
| Fast 20:00:39.564-59.577, 5 windows | 2252 / 141 | 11.09 / 4.99 | 1.497 / 1.149 | 0.207 / 0.023 / 0.102 |
| Slow 20:01:14.603-34.626, 5 windows | 1639 / 102 | 14.86 / 14.78 | 8.442 / 6.496 | 3.202 / 0.303 / 1.906 |

CPU times are weighted by clocked frames; Present/GPU are medians of window
p50s. GPU span includes scheduling gaps and does not prove GPU-busy time.

Guard checks / skipped scans / actual scans per clocked frame are
7069/6150/919, 428/362/66 and 7039/6155/884: skip fractions 87.00%, 84.52%
and 87.44%. Unknown decisions are zero and actual OM Gets equal full-scan
fallbacks in each group. The old inactive timer is zero because that work
does not execute. The functional skip and conservative full-scan paths both
run; this is not a timing-disabled instrument or a dead cohort.

Against the earlier 137d4743 66.83-fps plateau, measured output work falls
1.492 to 0.303 ms, copy aggregate 4.021 to 3.202 and EDVR gross 8.817 to
8.442 (estimated floor-net 6.949 to 6.496). Raw-domain work rises 1.634 to
1.906 ms, but its calls rise 6150 to 7027 per clocked frame (+14.26%);
gross per-call time changes only 266 to 271 ns (+2.14%). Actual scans fall
from 6159 to 884 per clocked frame. The views are not guaranteed identical;
these establish query elimination and remaining cost, not a controlled FPS
improvement or regression. The approximately 14.3k copy-parent calls in the
slow view are two scopes per approximately 7.1k draws, not 14.3k actual scans.

DLSS/history remain healthy: refusals stay 686; reset, lost-history and backend
failure counters do not increase; all 17 relevant phase windows have zero
failures. GPU resolve remains about 0.96 ms. The current capture does not
reproduce the external reporter's deferred-matrix refusal.

Ruled out: the new output guard being inactive in the carrier, because it
actually skips about 87% of eligible scans and the OM query count follows
only the remaining fallback cohort. Ruled out: new stable AA history failures
causing this slow/fast contrast, because history/refusal counters remain
unchanged. A 64-versus-66 readout alone cannot identify a regression with
this changed per-draw workload.

Sol's source audit identifies the next measured target: raw-domain observation
runs before converter/light shader filters on every screen-sized DSV draw.
`forwardDsv` repeats QueryInterface/GetDesc on the same held resource; its
immutable descriptor can be read once. CB binding/range, copied camera bytes
and metadata are also checked, with geometry issue begin/return validation
outside the constructor's raw-domain timer. The residual approximately 0.99 ms
includes publication/authority walks and mutation/filter work; it is not all
attributable to one function. No new optimization was applied during this
analysis. A failed-domain shortcut needs a no-healing proof and cohort/query
measurements; skipping raw observation globally could miss a new valid domain.

Astra reviewed evidence/source history only, without tests or changes, and
agrees this is remaining broad admission cost rather than proof of a new FPS
regression. Its RC3/RC4 causal audit is appended to the shared AA investigation.
No additional flight is needed before the source/cost audit. Frozen evidence:
`build/carrier_guard_a4a4e751_summary.json` and companion sanctioned-reader
stdout files. Main remains unmerged.

## 2026-10-17: raw-domain CPU audit and descriptor query reduction

Sean authorizes optimization, Sol for RE/tests, Astra for evidence review only,
full build and Steam-flat install when ready, with main unmerged. Source branch
`codex/flat-raw-domain-cpu` starts at `b513b775` (documentation after the tested
and installed `a4a4e751` source). Baseline is the verified capture above:
approximately 7027 raw-domain calls per clocked slow frame, 1.906 ms raw-domain
time and 0.303 ms output scans, with healthy AA/history. The approximately
14.3k copy-parent scope entries are two scopes per approximately 7.1k draws.

Discriminating audit hypotheses before this build:

- Redundant immutable resource description: `forwardDsv` and `forwardSrv`
  perform two Texture2D QueryInterface/GetDesc pairs on the same held resource,
  separately obtaining surface structure and format. Source call ordering
  confirms the duplication; one description must produce the same surface and
  format. A counted production-helper read and adapter wiring must show one
  description per call across valid and refusing shapes/types/formats.
- Redundant failed-domain camera work: an already-failed domain cannot heal
  within its ledger epoch, but observation still increments versions and can
  set global overflow. Geometry begin/return/abandon can revoke a publication
  created after a constructor snapshot, including after reset/reused identity.
  Issue presence also affects needsActualDraw. These effects discriminate a
  valid shortcut from a local no-healing claim; no such shortcut is applied.
- Mutable-state caching or moving shader filtering ahead of raw observation:
  actual DSV/depth-state/CB identity, newly introduced raw domains and writer
  effects remain required. The output guard's NoUsableState result is not
  continuing authority to skip this observation. No binding cache is added.

Chosen change: one same-call immutable description supplies both Surface and
format, while fresh DSV/SRV bindings, view descriptors, depth state, CB reads,
ledger observation and Geometry issue lifecycle retain their existing paths.
Reuse applies to DSV and SRV consumers, including issue validation. Existing
per-frame raw-domain timing/call counts support a normalized comparison; no
new config key or diagnostic arm is needed. No FPS improvement is claimed.

Ruled out: failed-domain no-healing alone as a complete elision proof, because
global version overflow and later publication/epoch callback effects remain
observable. Astra independently identified these obligations and reviews the
implementation boundary without executing tests/builds. Sol owns production
and behavioral tests. Validation must cover null/not-Texture2D, structural
shape/mip/slice/sample/format cases, snapshot lifetime and fresh reacquisition;
the fake immutable-read count is not a D3D-driver performance measurement.

Implemented `TextureDescription`/`TextureObservation` and the reused
`describeTexture` helper in `flat_forward_light.h`. The actual adapter issues
one Texture2D QueryInterface and at most one GetDesc per non-null observation;
DSV/SRV validators obtain format from that same value. A null resource never
calls the reader; a failed query clears its output. No persistent descriptor
or binding cache, ledger change, issue elision, new key or new instrument.

Sol's focused temporal rig passes 17 new checks and 20,736 legacy-equivalence
cases, plus existing forward 166, output guard 60/28,561 sequences,
integration 119, phase-bound 9570, paired payload 35 and 46 corpus replays.
The fixture tests a counted immutable-read callback and separately checks the
actual adapter wiring; it does not claim driver/GPU timing. Astra reviewed
the production diff and fixture boundary read-only and found no blocking
concern; it ran no tests/builds/benchmarks. Focused log:
`build/rawdomain-focused.log`.

Full absolute `build.bat --wait-lock` passed all gates in 159.3 seconds,
including production DLLs, GPU/state-restoration regressions, Python tools and
installer payload checks. Log: `build/flat-raw-domain-cpu-full.log`; receipt
input identity:
`5acd33dbdfe3754a9175df68ebd290a4a1479ea9a7c3516a5f46796e414afaf3`.
Sources froze before these gates. Commit/push this exact tree, promote clean
DLLs, install and verify Steam flat using the sanctioned tool without --ini.
The existing menu fix, functional output guard and failure capture remain.

One stationary validation: usual DLSS, CPU timing on, Motion source view off,
origin probe off; return to the same slow carrier view for 30 seconds, turn
to the faster view for 30 seconds, return for 30 seconds. No capture/F7 arm
is needed. Note FPS and visible changes; keep running until the log read is
confirmed complete. Compare raw-domain ms/call as well as total CPU, actual
scan counts, phase refusals and history; the old slow baseline is 1.906 ms
at 7027 calls (271 ns/call). No measured saving is available before that run.
Clean promotion/install/ZIP verification is recorded in
`build/rawdomain-validation.json`; main remains unmerged.

## 2026-10-17: descriptor optimization capture and startup discovery cost

Sanctioned reader verifies `edvr_gfx_20261017_201643.log` as exact installed
`v0.20.0-rc.4-10-gc6082f4b` (PE `6AD42BBC`), Steam flat, RTX 5090,
DLSS model M, 2496x1404 into 3840x2160. CPU timing on, Motion source view
and origin probe off. Sean reports initial high-40s/mid-50s FPS, then upper
70s/about 80, plus a later brief mid-60s dip while holding the same view with
menus closed. The completed primary capture ends at 20:20:39.329. Separate
late windows and an uncapped observation are not folded into those groups.

| Group / window ends | Real fps | EDVR gross / estimated floor-net ms | Copy / output / raw ms | Raw calls / ns per call |
|---|---:|---:|---:|---:|
| Initial cold 20:18:34-44 | 55.99 | 12.264 / 10.188 | 2.686 / 0.345 / 1.214 | 4779 / 253.9 |
| Steady slow1 20:19:04-14 | 77.11 | 6.694 / 5.072 | 2.294 / 0.240 / 1.289 | 5442 / 236.8 |
| Fast 20:19:24-39 | 90.00 | 2.216 / 1.690 | 0.503 / 0.060 / 0.266 | 1145 / 231.9 |
| Steady slow2 20:19:49-20:20:39 | 80.39 | 6.845 / 5.208 | 2.380 / 0.244 / 1.345 | 5557 / 242.0 |

CPU values are clocked-frame weighted; real FPS uses completed frame counts
and actual window duration. The cold group is mixed startup work, excluded
from the steady optimization comparison. The fast group is frame-limited.
After this capture, Sean removed that limit and reported about 190 FPS in
the fast view versus about 80 in the slow view. This is separate user-reported
context, not a new measurement group or a controlled patch comparison.

Against `a4a4e751` slow2's 271.28 ns/raw call, current slow2 is 241.97 ns:
29.31 ns/call or 10.8% lower gross cost, consistent with eliminating duplicate
descriptor queries. At the old 7027-call volume this scales to about 0.206 ms
of raw-block cost. Current volume is 20.9% lower and shader/role mix is not
identical; neither the full 8.442-to-6.845 ms EDVR reduction nor the whole
FPS rise can be assigned to the patch. One-call normalization is not a
controlled driver benchmark. The output guard continues to skip 87.33%, with
zero unknown decisions and 705 actual scans versus 5567 checks per clocked
slow2 frame; actual OM Gets still equal the full-scan fallback cohort.

AA/history continuity PASS: at the primary endpoint there are 10,767 DLSS
treatments, 671 historical refusals, two accepted/requested startup/load
resets and zero backend failures; uninterrupted continuation reaches 10,303
frames. Refusals and reset/lost-history counters remain constant after
20:18:29. A separate later check through 20:21:54 reaches 16,749 resolves,
16,747 continuations and a 16,285-plus current continuation run, with the same
two resets, zero backend failures/camera cuts/origin-ambiguity resets and zero stable phase
failures. No external deferred-matrix failure was reproduced.

The initial slowdown has an additional proven source mechanism. Automatic
passive discovery completes at 20:18:44.018, about two minutes after the first
owned Present. The cold group includes 2.216 ms direct discovery and 2.856 ms
engine wrapper work, versus 0.259 ms wrapper work in steady slow2. Source
`flat_runtime.cpp:5568` includes flatTemporalCapturing in diagnostics and calls
engineVelocityFlatLazy(!diagnostics); diagnostics force other-draw flushing.
`engine_velocity.cpp:2965` performs an eager flush after each producer when
lazy is off. finishDiscovery in `flat_temporal.cpp` clears capturing before
the completion line, switching that policy back on the next draw. Wrapper
D3D calls collapse from about 31,954 to 283 per frame despite substituted
draws increasing. This demonstrates discovery-triggered eager restoration as
well as direct discovery cost; it does not assign every transient millisecond
to that mechanism. These paths are unchanged by the descriptor optimization.

Late same-view/menus-closed windows ending 20:20:44.351-20:21:49.449 are
separate: aggregate 79.86 FPS, five-second averages 75.87-82.72, Present p50
11.80-12.71 ms and p95 14.66-16.00 ms. Raw cost stays 1.312-1.380 ms at
5440-5665 calls; EDVR total 6.687-7.060 ms and GPU resolve about 0.95-0.96.
The tail is compatible with brief slower frames, but no exact timestamp/frame
ties the reported dip to a cause. No sustained 60-FPS median plateau, AA
reset, backend failure, mode change or recorded LONG FRAME/FREEZE accompanies
these windows; the freeze-location sampler was off. GPU span is not proof of
GPU-busy time or a GPU bottleneck.

Ruled out: recurring AA disengagement/history restart as the explanation for
the measured late dip, because all relevant continuity counters stay fixed.
Ruled out: assigning the whole initial recovery to descriptor query removal,
because automatic discovery changes a separate per-draw restoration policy
and the raw workload rises while that cold cost falls. The brief untimed dip
does not justify another optimization or flight by itself. Startup discovery
cost is a separate concrete audit target; no such change is folded in here.

Sol aggregated/tested evidence and traced the source; Astra independently
reviewed the claims read-only without tests, builds, benchmarks or edits.
Frozen evidence: `build/carrier_texture_c6082f4b_summary.json`, `_rows.json`,
`_stdout.txt` and `_late_stdout.txt`. Log capture is complete; the user may
quit normally. No source or install change; main remains unmerged.

## 2026-10-17: automatic discovery and restoration audit

User authorizes a startup CPU audit, optimization and Steam flat install on
`codex/flat-startup-discovery-cpu`, keeping main unmerged. Sol owns RE and
tests; Astra reviews evidence only. The installed baseline remains
`v0.20.0-rc.4-10-gc6082f4b`. Environment and cold-window evidence are in the
preceding entry. No new user setting is proposed.

Hypothesis: automatic discovery's logical binding/CPU-byte observations do
not require restoration of EDVR's private bindings after every producer.
Confirm with a complete consumer audit and a real D3D counted-call fixture
showing the same logical records, original state at game work and manual
capture boundaries, and fewer restoration calls. Refute if any automatic
consumer needs unrestored physical context state or loses an observation.
Manual General/Full capture and an independently active compute candidate
must retain actual-state fidelity. In particular, flatTemporalDraw precedes
the runtime draw scope, so restoring only in that constructor is too late
for the first draw after a manual arm.

Consumer audit supports that narrow policy. Automatic Draw uses binding
shadows, observed setters/viewport, immutable view descriptions and CPU-write
bytes; Map/Unmap/Update and clear/transfer records use passed operands and
retain their stamps/invalidation. Actual pipeline readers are compute
ProbeDraw/Draw/Dispatch and Full menu-copy. Existing clear/copy/update/resolve
hooks restore before their callbacks. Draw/Dispatch need a new preflight
because their runtime scopes follow the discovery observer. Candidate
activation occurs at the owned Present boundary, independently of the
manual tier. Unknown command-list invalidation and owner/internal guards
remain in place; the offscreen shortcut still follows discovery observation.

Implementation uses one original-state-demand predicate in the regular and
offscreen paths. Automatic discovery may remain lazy; manual General/Full
and active candidate observation demand restoration. Start/stop/completion
clear manual demand; pause gates it without forgetting a manual window.
Astra's source-only review finds no blocking concern. Offline validation is
recorded below; in-game startup validation remains open.

Direct discovery work remains unchanged: no record, diagnostic feature,
capture budget, CPU copy or hash is removed. Prior steady-state raw-domain
savings do not establish a startup improvement. Existing discovery
arm/sample/completion lines, flat-CPU discovery/wrapper scopes and engine
draw-bracket kept/runs/not-lazy/call counts will distinguish successful lazy
operation from a collector that never ran. No new setting or counter is
needed; an in-game startup improvement has not yet been measured.

Focused validation (Sol, sequential compiler window, final sources frozen):
engine WARP rig PASS, 18,877 checks. The same current draw bracket serving
128 producers records 896 actual D3D state calls eager, 5 automatic-lazy and
896 manual-eager; calls are recorded on the context vtable, not inferred
from EDVR's counters. Actual pending bindings are restored before first
manual/candidate physical observation and dispatch. The witness slice
compares binding identities, shader hashes, camera96 bytes, exemplar4096
bytes/width, epochs/sequence stamps and contract properties across policy
transitions. Pause/resume, internal nesting, foreign/no-owner and independent
candidate demand are checked through the exported gate. Temporal PASS:
32 live-policy combinations, observer/lifecycle wiring and 46/46 corpus
replays (44,261 draws, 16,314 memo hits). Log-line policy has zero failures.

Limits: the witness uses fixture-provided CB bytes and selected production
helpers; it does not link the complete collector or resolver. Source audit
supports unchanged Map/Unmap assembly and observation ordering, not complete
flight/history equivalence. Astra reviewed the production/test sources only,
ran nothing, and found no blocking issue within this scope. No FPS estimate
is derived from the 128-producer fixture. Focused logs:
`build/startup_discovery_engine_focused.log` and
`build/startup_discovery_temporal_focused.log`.

Root full `build.bat --wait-lock` PASS, all gates, 149.1 s: production DLLs,
Python self-tests, complete rendering/lifecycle rigs and self-contained
installer checks. Receipt input identity:
`e63e0bf8e4319bcd00cd3b8fdcfec3cfea40eb05c3764b74c15af5f84256bfb8`.
Full log: `build/flat-startup-discovery-cpu-full.log`. The clean-commit
`--dll-only` promotion and sanctioned Steam-flat install have a separate
machine-readable record at `build/startup-discovery-validation.json`; that
record identifies the installed commit/version and preserves config hashes.
Main is deliberately unmerged.

Next capture needs no flight or manual capture arm: launch Steam flat,
load straight into the carrier and face the same slow view as soon as
loaded. Keep that view and current graphics/FPS settings for three minutes,
with menus closed and no F7/F8 or Shift+NumLock. Note early and settled FPS,
then quit normally. Compare automatic discovery's active windows against
its completion boundary (120 seconds or 12,000 useful frames after first
owned Present, whichever comes first), reporting
scene/load transitions separately. Require discovery records to remain,
lazy kept-runs to operate during discovery, eager/not-lazy amplification to
fall absent manual captures, and AA refusal/reset/continuation counters to
remain clean after normal startup/load resets. If loading finishes after
discovery, do not claim that this capture measured the carrier's cold cost.

## 2026-10-18: startup policy capture, carrier continuity PASS

Sean reports stable low-to-mid 80s FPS after the requested Steam test.
Sanctioned reader verifies `edvr_gfx_20261018_030452.log` as installed
`v0.20.0-rc.4-12-g0bb83e63`, PE `6AD430D5`, Steam flat `/novr`, RTX 5090,
DLSS model M/preset 13, 2496x1404 into 3840x2160. Driver/DLSS DLL versions
are not logged by this build. Preserved profile: CPU timing on, origin
probe off. No manual General/Full arm or F7 action was recorded. The file
continued growing, so analysis uses a frozen sanctioned-reader snapshot
(13,090 source lines), cutoff strictly before 03:15:00; last included
five-second windows end at 03:14:58. Times below are as logged. The earlier
root claim that the game had closed normally was premature; process lookup
did not establish that fact. No live object read is needed for this capture.

Automatic discovery ran 03:04:53.341-03:05:28.528, 35.187 seconds. Its final
report reaches exactly 12,000 useful frames (12,705 presents), with 8,647,949
draws, 32,699 copies, 104,721 dispatches and retained world/handoff records
142/17. No frame-dropped observations are reported in sampled/final reports;
this is not an exhaustive all-startup observation-equivalence proof. Source
`flat_temporal.cpp:966,969-980` confirms that useful-frame exhaustion can end
collection before the 120-second upper limit.

| Window ends / scene | Real FPS | Substituted draws/frame | Wrapper D3D calls/frame | Wrapper / direct discovery / EDVR gross ms |
|---|---:|---:|---:|---:|
| 03:05:23-28, menu, discovery active | 400.96 | 248.50 | 103.00 | 0.041 / 0.247 / 0.886 |
| 03:05:33, following boundary window | 401.76 | 249.00 | 103.00 | 0.039 / 0.006 / 0.641 |
| 03:07:53-03:08:18, early stable carrier | 78.78 | 4933.83 | 300.50 | 0.256 / - / 6.961 |
| 03:14:03-28, late stable carrier | 81.17 | 4988.48 | 295.42 | 0.265 / - / 7.065 |
| Prior `c6082f4b` steady slow2 | 80.39 | 4749.31 | 283.28 | 0.259 / - / 6.845 |

CPU timings use sampled clocked-frame weighting; wrapper D3D calls and
substituted draws use real frames. The 03:05:33.380 boundary window starts at
03:05:28.377, about 151 ms before discovery completes; it is not a wholly
post-discovery control. Low wrapper call counts already coexist with the
active collector and remain similar across this boundary. The first
draw-bracket kept/runs/not-lazy report is 03:07:58.952, after collection ends,
so those counters cannot directly establish lazy kept-runs during discovery.
Later carrier brackets report not-lazy zero and extensive kept runs.

The heavy carrier view starts around 03:07:33, over two minutes after
discovery ends. My capture steps did not account for the uncapped menu
exhausting the useful-frame limit before loading the carrier. Ruled out:
this launch as a same-scene cold-carrier comparison, because collection and
the heavy view do not overlap. The carrier workload is somewhat higher than
the prior steady capture; neither these FPS values nor the gross CPU times
establish a performance gain. The direct collector work remains unchanged.

AA continuity through the cutoff: 43,920 treatments, 43,918 accepted
continuations, two requested/accepted resets and two lost-history events
during startup/loading (03:05:18 and 03:07:28). Current/longest continuation
reaches 37,100 frames. Refusals stay at 13,634 from carrier entry through the
cutoff. Backend/camera-cut/origin-ambiguity cumulative counters remain zero;
phase-failure receipts remain zero and stable carrier jitter failures are
zero. One `no-3d-scene` jitter failure occurs during the 03:05:38 transition.
Although cumulative gap/format counters are zero, expected startup/load
reset-event records contain gap/format flags; do not describe all event
flags as zero. No AA disengagement or repeated history restart is observed
in the stable carrier interval.

Scoped PASS: collector operation, low menu wrapper call counts while it is
active, and carrier AA continuity. Heavy cold-carrier savings remain
unmeasured. Sol extracted/weighted the sanctioned output; Astra reviewed
the frozen evidence only, ran no tests/builds/benchmarks and agrees with
these limits. No source/build/install change or additional flight request;
main remains unmerged. Frozen evidence:
`build/startup_discovery_0bb83e63_summary.json`, `_rows.json`, `_stdout.txt`.

## 2026-10-18: start passive discovery from the carrier

User requests exact steps that keep the measured discovery window out of
the main menu before merging. The installed build cannot guarantee this:
`flatTemporalStart` is called at device creation; its deadline expires even
while paused or without draws. Limiting FPS avoids premature useful-frame
exhaustion but cannot extend the 120-second wall-clock limit. Existing
NumLock restarts manual General capture (1000 ms / 3 useful frames), and
Shift+NumLock restarts Full; both demand eager restoration and add a two-frame
draw census. They cannot substitute for automatic passive discovery in this
measurement.

Prepare a narrow temporary Ctrl+NumLock action (Ctrl plus the configured keyboard
dump_draws key, with Shift absent) that starts the same automatic collector
from the settled carrier. It must retain the 120-second / 12,000-useful-frame
budget, clear manual demand through the existing start path, honor ownership
and independent compute-candidate safety, and avoid arming the draw census,
projection audit or bulk compute capture. Plain NumLock and Shift (including
Ctrl+Shift) retain their existing General/Full behavior; VR behavior stays
unchanged. No new INI key and no origin-probe enable. This shortcut is
temporary diagnostic scope; propose its removal explicitly after validation.

Instrument hypothesis: a carrier-triggered fresh passive window exercises
the automatic discovery/restoration policy without spending its budget in
the menu. Confirm through a distinct accepted/rejected request log, first
owned Present, active reports/records, low wrapper calls and not-lazy counts
during collection, completion and continuing AA history. Reject if the new
route enables manual demand or another expensive capture, or fails to start.
Sol owns implementation/tests; Astra reviews evidence only. No startup or
renderer admission policy change is intended.

Implemented source contract: hotkey routing preserves Shift priority, VR,
mouse/pad routes and configured Ctrl-required keyboard bindings (their normal
press remains General). The passive entry verifies availability and established
owner/current thread before reading collector device state. Stopped, wrong
device, paused/internal, manual, pending/active compute/readback and runtime
bulk/projection states reject with a reason before mutation. Busy queries do
not consume pending requests or allocate the compute bank. An active automatic
collector may be rearmed; AA runtime/history are separate and untouched.
Accepted requests log `passive rearm accepted` with `manual=0`, followed by
the existing first-owned-Present/sample/final chain; acceptance alone does
not prove useful observations occurred. No census or bulk arm is on this route.

Sol full absolute `build.bat --wait-lock` PASS: all 170 jobs, production DLLs,
Python gates, complete rendering/lifecycle rigs and installer validation,
145.4 seconds. Engine WARP PASS, 18,885 checks; existing 128-producer fixture
remains eager896/automatic5/manual896 actual context calls. Temporal policy
PASS includes route/peek tests and 32 live-policy combinations. Wiring checks
pin the rearm guards, dispatch and first-Present/report/final chain, including
foreign rejection before the device read. The complete collector restart TU
is not executed by the new fixture; actual acceptance/useful frames remain
part of the in-game check. Astra reviewed source/evidence only, ran no tests
or builds and found no blocker after the guard-order correction.

Full log: `build/flat-passive-rearm-full-escalated.log`; receipt input identity
`66b81cd808541be4e70e49abf434f0accf15f969a8b18c4aadd3c9a1cd8a8627`.
The first two sandbox runs stopped before compilation at tempfile/process
permissions. The successful run used an approved execution environment with
TEMP/TMP/TMPDIR under ignored `build/validation_temp`; no gate was skipped.
Clean promotion and sanctioned Steam-flat install are recorded separately in
`build/passive-rearm-validation.json`, including the exact installed version
and preserved config hashes. Use that promoted build, not `0bb83e63`.

Exact stationary sequence after installation:

1. Launch Steam flat, load the carrier and face the same slow view.
2. Send Ready before the timed capture, then focus the game and close menus.
3. Hold that view for 30 seconds as the baseline.
4. Hold Ctrl, tap NumLock once, then release Ctrl. Do not hold Shift.
5. Hold the same view for 180 seconds, keeping settings, FPS limit and AA mode
   unchanged. No F7/F8 or other capture key. This spans the collector's maximum
   120-second window and a post-completion control; at about 80 FPS the time
   limit comes first. Check the accepted/first-Present/useful-frame log chain
   while the user holds position, so a rejected trigger is caught promptly.
6. Note baseline/during/after FPS and any blur or scene jump; quit normally,
   then send Done.

This isolates collector/restoration overhead in a warm heavy scene; it does
not measure cold shader compilation or asset loading. Direct discovery still
costs CPU, so judge wrapper calls/timing and AA continuity alongside FPS.
The carrier capture remains outstanding and main stays unmerged.

## 2026-10-18: slow carrier view and AA-off comparison

User completed the requested sequence but omitted the Ready chat message,
then reported this different carrier area running in the mid-60s with GPU
utilization around 25%. They subsequently report 263 FPS with AA off. User
redirects the investigation to steady AA-dependent CPU overhead; do not
spend another flight on startup instrumentation at this point.

Sanctioned reader pins `edvr_gfx_20261018_033520.log` to installed
`v0.20.0-rc.4-14-g4c810f12`, PE `6AD49235`, Steam flat `/novr`. Same RTX 5090,
DLSS model M and 2496x1404 into 3840x2160; CPU timing on, origin probe off.
Startup discovery finishes at 03:35:54.534 after 12,000 useful frames. No
passive-rearm accepted/rejected, manual dump_draws or census press is logged
through the frozen carrier read at 03:43:11.157. Ready is only a chat
coordination signal; it has no DLL dependency and its omission cannot prevent
arming. This carrier read is post-collector, not the new passive validation.

The six stable five-second windows ending 03:42:46-03:43:11 contain 2054
frames over 30.062 seconds: 68.33 real FPS, mean frame period 14.636 ms.
Present p50 is 13.97-14.35 ms and must not be inverted as average FPS.

| CPU family / measure | Current weighted value |
|---|---:|
| EDVR total, gross per sampled clocked frame | 8.311 ms |
| Recorded sampled timing floor | 1.978 ms |
| Estimated EDVR floor-net | 6.333 ms |
| Copy/admission checks, gross | 3.019 ms |
| Raw-domain subset of copy checks | 1.722 ms / 7089 calls / 242.9 ns per call |
| Output-scan subset of copy checks | 0.305 ms |
| Reported residual copy bucket | 0.989 ms |
| Other draw/dispatch shell, gross | about 2.23 ms |
| Engine draw wrapper, gross | 0.324 ms / 307 D3D calls per real frame |
| Discovery | 0.000 ms |

The raw/output/residual buckets are nested inside copy checks, not additional
costs to sum on top. The residual is its reported bucket; displayed subsets
leave about 0.003 ms elsewhere. Timing runs one frame in sixteen. Estimated
average clock perturbation is 0.253 ms per real frame; do not subtract that
again after subtracting the sampled floor. Estimated net is not an exact
uninstrumented benchmark.

Current substituted draws average 6201/frame, versus prior `c6082f4b` steady
4749 (+30.6%); raw calls rise 27.6%, while gross raw cost is 242.9 versus
242.0 ns/call (+0.4%). Copy cost rises 26.9%, total gross 21.4% and estimated
net 21.6%. This supports larger work volume multiplying ordinary per-draw
cost. Views and role mixes differ; it does not establish a regression or
prove every operation is unchanged. Offscreen-fast work skips about 4025
draws/frame. Inferring about 7211 slow draws from two copy scopes per draw
is a workload estimate, not an exact all-draw census.

The log confirms F7 OFF at 03:44:38.933 and restored at 03:44:41.811, a
2.878-second interval. The user's 263 FPS corresponds to about 3.80 ms/frame,
versus the earlier sampled 14.64 ms, a directional difference near 10.8 ms.
The short off interval is not a logged stable off benchmark. F7 also disables
AA-dependent UI quality through the same configure sweep, so this isolates
the broader AA-dependent EDVR path, not NGX alone. Do not attribute the full
apparent difference to the 6.333-ms CPU estimate or add CPU/GPU spans as
independent wall times. Reported GPU utilization alone cannot establish the
bottleneck; combined with measured render-thread cost and the AA-off response,
CPU-side overhead is strongly supported. GPU span includes CPU-feed gaps.

Sol source trace: `flat_runtime.cpp:5704-5724` runs forward/copy verifications
for slow-path draws; `flat_forward_light_runtime.inl:198-207` observes depth
domain/writes before converter shader filtering. Domain checks query the
current DSV/resource/texture and depth-stencil state, then update camera and
mutation proof. The immutable descriptor duplication was already cut; mutable
actual-state queries and ledger work remain. The output guard costs about
0.3 ms, making it a smaller target than raw-domain work. The other bucket
spans constructor/destructor/dispatch bookkeeping and must not be assigned to
one function. Simply moving observations behind shader filtering would lose
unknown-writer correctness evidence. Failed-domain no-heal alone was already
ruled out as a safe skip proof; global overflow/publication/issue lifecycle
must also be respected (earlier descriptor audit entry).

At the pre-toggle cutoff, AA continues: treated 31,064, refused 13,621,
accepted reset cumulative 2, accepted continue 31,062, current treated streak
27,247; no backend failure, all adapter reset-window fields zero. Later F7
reset/warmup events are user actions, not spontaneous history failures.
Ruled out: active discovery as the cause of these steady carrier costs,
because its scope is zero and collection already ended. Ruled out: attributing
the whole view-to-view CPU rise to slower raw checks, because mean ns/call
is nearly unchanged while call volume rises. No broader regression claim.

Separate input finding: the configured key is NumLock/vk0x90/mods0 and the
passive entry was never reached. Existing `device_hook.cpp:2035-2046` warns
about Ctrl+ScrollLock becoming Break. Microsoft's
[keyboard account](https://devblogs.microsoft.com/oldnewthing/20080211-00/?p=23503)
describes the corresponding Ctrl+NumLock/Pause equivalence. Choosing that
default chord was a mistake; actual hardware VK state was not captured, so
the remap is the likely explanation rather than a measured key trace. F8
Hotkeys can rebind dump_draws live to an ordinary key when this test resumes;
no binding has been changed. Do not infer a collector ownership failure
from a missing ingress event.

Next read-only audit: raw-domain and other cost per call, repeated actual
state queries versus generation-certified reuse, and offscreen-fast decline
reasons. Existing evidence justifies those targets, not a new fix yet.
Sol extracted/normalized the data and traced source; Astra independently
reviewed arithmetic/attribution only, without tests/builds/benchmarks.
No source, build, install or config change; main remains unmerged. Frozen
evidence: `build/passive_rearm_4c810f12_cpu_summary.json` and
`build/passive_rearm_4c810f12_carrier_read.md`.

## 2026-10-18: repeated state-query and draw-shell audit

User authorizes optimization, Sol for RE/tests and Astra for evidence review
only, full build/install Steam flat, origin probe off and main unmerged.
Baseline is the preceding pinned `4c810f12` carrier read, not a controlled
comparison against a different view. No new flight is needed to establish
that the immutable descriptions are repeatedly queried: the production
`forwardDsv` path reads actual bound DSV and depth state on every call and
then repeats view/resource/texture/depth-state metadata queries.

H1: retained immutable description reuse can remove that metadata work
without weakening mutable state or writer proof. Confirmation requires a
production-used helper rig counting every actual OM getter, descriptions
once per retained identity run, fresh descriptions after identity change or
reset, and identical surface/write decisions including null defaults,
read-only views, unsupported descriptions and aliases. The flight signature
will be nonzero cache hits and fewer immutable queries per raw-domain call,
with uninterrupted AA history and no new safety refusals.

H2: the roughly 2.23-ms gross draw-shell bucket has redundant work. Sol's
first audit finds a distributed constructor/destructor residual, not a
single expensive function; removing unknown-writer observation or bypassing
the reducer/phase lifecycle would be unsafe. Do not justify a broad skip
from shader filtering or failed-domain status. Existing offscreen fast
path already skips about 4,025 draws/frame. Further shell audit is pending.

Proposed H1 boundary: actual OM DSV/depth-state queries and all ledger,
mutation, camera and Issue checks remain per call. Independently retained
DSV/resource and depth-state COM identities key owner-state, bounded
immutable metadata; reset releases references, changes query fresh, and
uncertainty fails closed. Binding shadow generations are not evidence of
actual mutable state. Astra's initial evidence review accepts that narrow
boundary subject to production wiring and the counted transition tests.

Implementation keeps one retained DSV/resource description and one retained
depth-state description, independently, and clears both in `forwardReset`.
Both adapters execute `ImmutableDescriptionCache::describe`; miss loaders
query fresh metadata, successful immutable observations are retained, and
uncertain observations are returned only as uncached scratch and retried.
The actual OM getters remain outside and before reuse. Existing copy-window
sampling adds DSV/depth OM Gets, description hits/misses and null-depth
defaults; these are staged events, not individual texture QI/GetDesc counts.
Non-null DSV hits plus misses equal depth Gets; depth hits plus misses plus
defaults equal depth Gets. Null DSV returns before either cache observation
or the depth getter. No new toggle, binding shadow dependency or GPU read.

The revised behavioral rig executes that production helper with counted
loaders, retained fake COM handles, replacement/reset, independent identities,
null and failed-description retries. Actual D3D OM calls and write/ledger
wiring are source assertions and runtime counters, not an executable D3D
equivalence claim. The first test-owned adapter loop was removed after root
and Astra review because it duplicated production control flow. The full
formatter test exercises every new label at maximum uint64 width.

Draw-shell audit completed without a production shell change. Most target
loops are already feature-gated. The `overlayOpen && hdrBad` scan cannot be
gated on `overlaySuffixActive` alone: `flatRuntimeObserve` can open and fail
an overlay on the protected draw before that flag is raised. Scope-object
default initialization precedes the constructor's shell clock and cannot
be assigned to its measured residual. Ruled out: redundant same-identity
COM reference churn in `domainHolds`, because the local SDK's WRL
`ComPtr` exact-type and raw-pointer assignments already compare pointers
before AddRef/Release (`winrt/wrl/client.h:301-307,317-323`). Adding an
external compare would duplicate existing work. A repeated scalar jitter
baseline assignment is too small to justify a separate optimization claim.

Expected H1 saving is bounded and unmeasured: a prior one-pair reduction
of about 29 ns/call would correspond to about 0.2 ms at the current 7,089
raw calls/frame if reuse nearly always hits; remaining immutable calls and
hit rate are not individually timed. This is not a claim to recover the
roughly 10.8-ms directional F7 gap. Installed capture remains pending.

Sol's exclusive full validation build PASS: absolute `build.bat --wait-lock`,
158.6 seconds reported (159.9 wall), 170-job pool, production d3d11 13 units
compiled / 128 reused. Shared cache helper 38 checks / 0 failures; texture
description 17 checks and 20,736 oracle cases / 0 failures; worst-width
formatter, collector policy and 18,885 engine checks PASS. Both production
DLLs, installer and resource checks pass; final all-gates PASS and receipt
inputs `0497a4a287da2e5e417e942619309bc79475a055496b569f44b22314b028c702`.
Log: `build/repeated-query-full-build.log`. Astra reviewed source/evidence
only, including revised shared-helper tests, and found no blocker; no tests,
builds or benchmarks executed by Astra. Clean-version promotion and install
follow this validated source commit; runtime saving remains unmeasured.

Clean source commit `ee68d075` promoted with `build.bat --dll-only --wait-lock`
PASS (34.9 seconds wall, receipt verified). Steam flat sanctioned install
dry-run, install and verify-only PASS, version
`v0.20.0-rc.4-16-gee68d075`; DLL SHA256
`915C909F26280288E1191F53CE0B34319A319F6B87BDDBA1813A7220B864B50C`.
Both live settings hashes exactly match the pre-install snapshot: DLSS,
CPU timing on, origin probe off. Logs/receipt:
`build/repeated-query-promotion.log`, `build/repeated-query-validation.json`.
Feature branch pushed; main remains `043d7ff6` and unmerged.

Stationary validation steps (no arm or Ready dependency): launch Steam flat
and load the same on-foot carrier slow view, keep DLSS selected, menus
closed and frame-rate limit unchanged/off as in the preceding comparison.
Wait two minutes there for startup work to finish; then hold slow 30 seconds,
turn at least 90 degrees to the fast view for 30 seconds, and return to the
same slow view for 30 seconds. Record FPS for each leg and whether AA stays
visibly engaged. Do not press F7, NumLock or its modifier chords during
these windows. Normal quit afterwards is fine; no live object read is needed.
Use `edvr_log.py --target steam --expect-build ee68d075` before reading.
Compare DSV/depth hits and misses, actual getter stage equalities, raw ns/call
and per-frame cost normalized by call/draw counts, discovery-zero windows,
and AA continuation/reset counters. Different carrier positions/views or
startup legs cannot establish causal FPS savings.

## 2026-10-18: immutable-description cache carrier validation

User completed slow/fast/slow and left Steam flat running. Sanctioned reader
verifies `edvr_gfx_20261018_040555.log`,
`v0.20.0-rc.4-16-gee68d075`, PE `6AD49934`, flat `/novr`. Source is the
installed `ee68d075`; later HEAD commits are records only. CPU/copy reports
are frozen through 04:11:51.242, AA through 04:11:50.960 (282 ms earlier).
The log grew during reading: timestamp cutoffs, not incidental file lengths,
define the evidence. No live object read is needed; normal quit is safe.

Sol selects stable workload clusters and excludes startup/loading and mixed
turn windows. Slow 1 has 18 full windows ending 04:08:45.992-04:10:11.114;
fast core has five ending 04:10:21.115-04:10:41.123; slow 2 has 13 ending
04:10:51.143-04:11:51.242. These are inferred view intervals, without arm
markers. For the compact comparison, use six-window slow cores around the
five-window fast core; excluded first fast/return windows contain the turns.

| Stable group (CPU window ends) | Real FPS | Substituted draws/frame | Raw gross ms / calls / ns per call | DSV / depth-state description hits | EDVR gross / estimated floor-net ms |
|---|---:|---:|---:|---:|---:|
| Slow 1 last six, 04:09:46.074-04:10:11.114 | 74.13 | 6352 | 1.497 / 7339.7 / 203.9 | 99.99012% / 99.5956% | 8.271 / 6.207 |
| Fast five, 04:10:21.115-04:10:41.123 | 290.74 | 121 | 0.049 / 310 / 156.8 | 99.6471% / 94.773% | 1.247 / 0.923 |
| Slow 2 first six, 04:10:51.143-04:11:16.180 | 73.05 | 6534 | 1.550 / 7467.3 / 207.6 | 99.99034% / 99.5916% | 8.436 / 6.350 |

FPS is total frames divided by summed window seconds; CPU and call means
are weighted by clocked frames, draws by real frames. Present p50 is not
used as an average FPS. Full slow groups yield 72.94/71.68 FPS and raw
204.0/207.5 ns per call, versus prior `4c810f12` 242.9: observed rates are
16.0%/14.6% lower. Raw gross frame cost is 1.490/1.553 ms versus prior
1.722 despite more raw calls (7302.5/7485.2 versus 7088.8). At current call
volumes the prior-rate difference is about 0.284/0.265 ms per clocked frame;
that is an indicative rate-normalized comparison, not isolated causal frame
time saved. Different views, role mixes, draw counts and capture conditions
prevent attributing the whole FPS difference or an exact saving to this
patch. Timing floor-net is an instrument estimate, not an uninstrumented
benchmark. The remaining roughly 6.2-6.35 ms of EDVR CPU in the slow cores
shows that the broad AA-dependent CPU overhead is not resolved.

All 71 paired CPU/copy reports satisfy the getter event partitions. In slow
1, DSV gets 8,285,644 = 8,284,822 hits + 822 misses; depth hits 8,252,379 +
33,265 misses, no defaults. Slow 2 gets 6,060,538 = 6,059,954 DSV hits +
584 misses; depth 6,035,622 hits + 24,916 misses, no defaults. Actual OM
getters remain live. Their count exceeds raw-domain timer calls because
`forwardDsv` also runs in admission/validation paths. A DSV miss performs
GetResource/view GetDesc and attempts texture description; it is not an
individual count of texture QI/GetDesc on failed resources. No timing gain
is inferred from hit counts alone. Discovery completes 04:06:26.379 and is
zero in comparison windows; it cannot explain these steady costs.

AA continuity PASS for the measured legs: slow 1 +6558 accepted continues,
fast core +7264, slow 2 +4661, all with zero new refusals or accepted resets.
Requested/lost history, invalid previous camera, camera cut, backend failure,
origin ambiguity and invalidations have zero leg deltas. No F7 `aa compare`
toggle appears through cutoff. Two accepted resets precede the comparison:
04:06:21.070 initial accepted scene and 04:08:28.342 after a 123,625-ms load
gap. Reset event gap/format flags can be nonzero while cumulative gap/format
counters stay zero under lost-history handling; that is not contradictory.
The wider interval's treated streak 1810 to 22646 includes excluded turn
windows; do not equate that 20,836 growth with the clean-leg sum 18,483.
Phase failure/reset counters stay unchanged, but status remains
`awaiting-issued-selected-bootstrap`; this is not new phase-bootstrap proof.

Cache reuse and uninterrupted AA are demonstrated in this capture; no
additional flight is needed for those observations. No source/build/install
change from this log review; main remains unmerged and origin probe off.
Frozen evidence: `build/repeated_query_ee68d075_capture_filtered.txt`,
`build/repeated_query_ee68d075_capture_summary.json` and `.md`, and
`build/repeated_query_ee68d075_aa_frozen.json`. Sol performed log extraction
and normalization; Astra reviewed the evidence only, without running tests,
builds or benchmarks.

## 2026-10-18: remaining per-draw CPU audit

User authorizes continued optimization, Sol for RE/tests, Astra for evidence
review only, full build/install Steam flat and main unmerged. Baseline is
the preceding `ee68d075` capture, not a new unpinned flight. Origin probe
stays off, settings preserved. No live object read is needed for this audit.

Candidate discriminants before editing: repeated pure target/source/pair
lookups need a source call/occupancy census and equivalence proof; repeated
actual-state/CB proof needs a bounded no-mutation certificate covering each
intervening operation; draw-scope initialization needs production codegen or
measured helper evidence; timing-gate overhead needs the real production
Scope path and must not be confused with the recorded clock floor. A large
aggregate bucket does not identify a removable operation. Existing camera,
pair-fact and offscreen memoization must be accounted for first.

Prior ruled-out shortcuts still apply: overlay-suffix flag alone can miss
the same draw's new failure, failed-domain status cannot bypass global
overflow/publications/authority or a pending Issue, and WRL already avoids
same-pointer reference churn. Any faster lookup must preserve array order,
first-match/null behavior, append/reset and full/unknown fallback; it may
accelerate finding live data, not cache mutable camera/mutation verdicts.

Confirmed source evidence before editing: Geometry and Light
`forwardIssueValid` each read actual DSV only to fill `key.dsv`, then
`forwardDsv` immediately reads it again and compares to that just-read
identity. It is not a prepared-Issue view identity. Owner/foreign/uncertain
and phase guards precede the queries; no game Draw or state setter intervenes.
The narrow change uses one fresh actual DSV snapshot per validation while
keeping ordinary draw-key exact-view matching and all surface/resource,
format/subresource/extent, read-only/write-state, phase, CB, publication and
RTV proof. Begin and Return still validate independently on opposite sides
of the actual draw; Converter's separate null-DSV/RTV path is untouched.
Single-owner operation is the supported assumption, not an atomic snapshot
claim for concurrent context mutation. Astra accepts this boundary subject
to production wiring and transition/alias/default/unknown tests.

Also confirmed a timing-coverage hole: flat draw thunks in `vscreen.cpp`
return from the constructor, call `beginActualDraw`, issue the game Draw,
call `endActualDraw`, then destruct the scope. The constructor/destructor
local `kOther` clocks do not cover these callbacks. In particular, the
per-draw forward Issue checks before/after Draw are outside the reported
family sum. Prior roughly 6.3-ms floor-net is therefore incomplete coverage,
not a bound on EDVR's total overhead. A new exclusive draw-issue-callback
family will time both callbacks, excluding the game Draw between them and
carving out existing nested feature scopes. Begin's timer follows !ctx
return; end's is conditional on ctx, preserving cleanup/reentry behavior.
EDVR private replays inside callbacks count as EDVR CPU; destructor cleanup
before its existing clock remains outside this family, so this still does
not claim exhaustive hook coverage. Adding measured callback CPU may raise
the report total without a runtime regression. Calibration and bounded
formatting must include the new family.

Existing raw DSV OM Get counters count only `forwardDsv`'s inner query, not
the redundant pre-query in Issue validation. They must not be claimed to
drop from about 20k to 7k per frame. A separate sampled reached-Issue DSV
snapshot count will show one former pre-query eliminated per reached stage;
it does not imply every Issue passes earlier guards or measure time saved.
Expected eligible count is roughly the excess of inner getter calls over
raw-domain calls (about 13k/frame, with other qualification paths mixed in),
not a measured ms saving. No target-index accelerator is justified yet
without call/occupancy/cost evidence.

User then reports Frontier's same carrier spot held 90 FPS in VR cinema and
flat, and requests the installed-commit diff before proceeding. Build was
held while this comparison was checked. Explicit Frontier path is
`C:\Users\seanm\AppData\Local\Frontier_Developments\Products\elite-dangerous-odyssey-64`.
Sanctioned log reader verifies `edvr_gfx_20261018_042154.log` as
`v0.20.0-rc.2-66-g208ea3e8`, PE `6AD27EAB`; initial profile VR/scope both,
subsequently switched flat. Steam installed source is `ee68d075`; the pending
unbuilt query/timer patch is not part of that historical diff. `208ea3e8` is
the merge base and an ancestor. Complete exact diff: 158 files, 23,048
insertions / 573 deletions, saved as
`build/frontier-208ea3e8-vs-steam-ee68d075.patch` (1,825,374 bytes). Source
and test subset is large; do not attribute all changed lines to CPU work.

Most relevant path changes, source-proven independently by Sol and Astra:

- `44a8ef1a` introduces the production forward-camera adapter and raw-depth
  writer ledger, mutation/revocation and Issue prepare/begin/return/abandon
  proof. These paths are absent in `208ea3e8`. Installed Steam calls
  `forwardBefore`/`forwardQualify` on slow-path draws; under upstream phase
  ownership, output/write tracking and screen-depth domain observation run
  before the exact converter/light shader filter. Geometry Issues then add
  shader/DSV/depth-state/CB proof at Begin and Return. This is substantial
  added per-draw work; current raw timer alone is about 1.55 ms/frame and
  callback work was unclocked. It does not establish the full FPS delta.
- `942cfeb3` supplies operation-derived frustum-rounding bounds for forward
  admission; `c7c14898` keeps rejected forward candidates local.
  `a4a4e751` integrates the menu/on-foot camera-proof fix with the output
  guard. These are correctness/admission changes, not justified removals.
- `219d2bdb`, `c6082f4b` and `ee68d075` trim output scans and repeated
  immutable description work in that new path. `45595786` adds the offscreen
  repeat fast path absent from the Frontier baseline; it currently avoids
  roughly 4.2k slow draw scopes/frame. `0bb83e63` reduces startup discovery
  restoration cost. Those offsets also belong in the comparison.
- Manual light-payload capture (`b025e8fd`) is Full-capture gated, and passive
  stationary capture (`387288ed`) is gated by the default-off origin probe.
  Their file size is not evidence of normal per-draw CPU cost.

Crucial control limitation: Frontier flat config selects XeSS, CPU timing
is off, and its runtime is unavailable. At 04:25:58.773 the flat summary is
treated=0, refused=6214, last=spatial-fallback, accepted-history-5s=0; HDR
prepped/backend=0 and no timed GPU resolve. At 04:26:01.461 preflight reports
`libxess.dll is missing beside EDVR`, backend-available=0 and 6427 spatial
fallbacks, then flat switches back to VR at 04:26:01.462. Older renderer
accepted totals include the separate earlier VR interval; they cannot be
presented as successful flat AA. Frontier flat's 90 FPS therefore is not a
working temporal-AA baseline, and VR uses a different draw path. No
store-specific regression or exact frame-time increase follows from this
pair of logs. No Frontier files/settings/runtime were changed.

The diff still identifies added proof work worth optimizing. Deferring all
writer observation until a light consumer appears is unsafe: later admission
needs earlier same-frame coherence and unknown-writer/mutation history;
qualified=0 at the end is not a safe skip certificate. A broad lazy-start
design needs independent prefix provenance or explicit fail-closed recovery.
The narrower duplicate-query removal and new timing remain justified, and
the original authorized build/install resumes after this comparison.

Sol tests execute production-used one-query observation and structural/write
proof helpers, changed bindings/defaults/null/read-only/resource/plane
cases, timing exclusivity with a nested child and game Draw outside the
callbacks, unclocked gating and maximum-width log fields. Production hook
ordering and guard seams have static assertions; this is not a real-driver
benchmark or complete Issue execution fixture. Astra found a stale `&dsv`
assertion after the helper switched to `&current`; Sol corrected it before
build. Final source/test review PASS, no additional blocker; Astra ran no
tests/builds/benchmarks. No wider draw-shell shortcut was justified.

Sol full validation PASS: absolute `build.bat --wait-lock`, 172.8 seconds
reported / 174.1 wall, production d3d11 13 compiled / 128 reused, 170-job
pool and installer/resource gates. Forward core 166 checks, cache/helper
52, integration 119, texture description 17 plus 20,736 oracle cases, and
engine velocity 18,885 checks all pass. Callback fake-clock tests include
nested exclusivity and game work outside scopes; maximum-width window
format fits five lines (longest 989 of 1000 chars), both new counters named.
Full receipt inputs:
`15b822edffa892f271953021ad41d0528f59edb5707e8a25bc9efee8524282a9`.
Log `build/per-draw-issue-full-build.log`; final all-gates PASS read. Source
commit and clean promotion follow, then sanctioned Steam flat install.
This validates code/rigs; removed-query counts, new callback CPU and FPS
benefit still require the installed capture. Future comparisons must keep
legacy families separate from the new callback family; subtracting gross
callback time from an expanded floor-net total would mix timing corrections.

Clean source `709ba499` promoted by absolute `build.bat --dll-only --wait-lock`
PASS, 41.2 seconds wall / 39.9 reported, receipt matched validated source.
Version `v0.20.0-rc.4-19-g709ba499`; Steam flat sanctioned dry-run, install
and verify-only PASS. DLL SHA256
`16C20864D0712FDE96CF92E799F107D46CCB94B02F268F4C30912568011C8927`.
Both Steam settings hashes match the pre-audit snapshot; DLSS selected, CPU
timing on, origin probe off. Frontier files remain untouched. Source pushed
to feature branch; main still `043d7ff6`. Logs/metadata:
`build/per-draw-issue-promotion.log`, `build/per-draw-issue-validation.json`.

User then reports Frontier flat with DLSS also exceeds 90 FPS. This is a new
run and must not be explained by the preceding XeSS failure. Sanctioned
reader pins `edvr_gfx_20261018_043544.log` to `208ea3e8`/PE `6AD27EAB`,
initial VR at 04:35:44.943, flat switch 04:36:21.301. Frozen evidence through
04:37:56.317 is in `build/frontier_dlss_208ea3e8_frozen.md`.

DLSS genuinely worked earlier in that flat session: treated reaches 601 by
04:36:36.316, including 160 treated/prepped/backend frames in that early
five-second window. Those are not VR carryover. The later carrier route
differs, and the three recent windows end 04:37:46.317, 04:37:51.318 and
04:37:56.317 with 424, 442 and 440 frames respectively. All 1306 report
foreground-contract-unqualified refusal; configured mode is DLSS, treated
stays 601, accepted history is zero, and HDR treated/prepped/backend are
zero. Spatial fallback grows by 882 over the last two windows, exactly
442+440. This is bounded evidence of fallback in the current carrier view,
not a claim that DLSS never worked in the session.

Relevant current cause: selected-H first failure is foreground-active-query,
H attempts 3471 / qualified 0, replay query rejects 1760/1760 with
UNCERTAIN_BRACKET#100. At old source `flat_runtime.cpp:6860-6871`,
`replayQueriesSafe` refusal blocks planning opaque foreground work; old
`flat_mono_resolve.cpp:830-833` requires the qualified same-frame motion map.
Earlier backend-failure cumulative 407 is not a new failure delta here.
GPU resolve is untimed in the complete adjacent CPU window and CPU timing
is off. The reported FPS is real user experience, but these recent frames
do not provide a successful-DLSS CPU control or establish a store-specific
cause. Sol extracted and traced this separate run; Astra reviewed the frozen
evidence and accepted this interval-limited conclusion without execution.

Next Steam validation: launch flat, load the same slow on-foot carrier view,
DLSS selected, menus closed and FPS uncapped. Wait two minutes, then hold
slow 30 seconds, turn at least 90 degrees to fast 30 seconds, return to the
same slow view 30 seconds; record FPS and visible AA continuity. No F7,
NumLock, Ready or diagnostic arm is required. Normal quit afterward is fine.
Read the resulting log with `edvr_log.py --expect-build 709ba499` first.
Check nonzero draw-issue-callback time/calls and reached Geometry/Light DSV
Get counters; absent counters with clocks active mean the new stage did
not run, not a proven zero cost. Keep raw OM Get counters' historical scope
separate from the eliminated pre-gets. Compare legacy families and new
coverage explicitly, plus reset/refusal/history/backend continuity. No
measured FPS claim for this patch until that capture.

User repeats Frontier with a 300-Hz frame limit: slow view holds about 90,
fast view exceeds it. This rules out a fixed 90-FPS cap as the whole
explanation. A reported 90 versus earlier Steam roughly 73 corresponds to
11.11 versus 13.70 ms/frame, a descriptive difference around 2.6 ms; it is
not an isolated measured code-regression size.

Crucial new run: sanctioned reader verifies `edvr_gfx_20261018_043900.log`
as the same old `208ea3e8`/PE `6AD27EAB`, launching directly flat at
04:39:00.016. DLSS now genuinely works in the sampled carrier frames.
Five consecutive runtime windows ending 04:41:05.490-04:41:25.491 have
762/668/653/578/548 treated/prepped/backend and accepted-history frames:
3209/3209, no new refusal/reset/jitter failure, backend failure cumulative
zero. Foreground captures 14916/14916, query rejects 0/14916, H qualified
3728/3728, treated streak 3728. Ruled out: extending the prior session's
fallback conclusion to old Frontier flat generally, because this clean
flat launch sustains DLSS and history. The earlier `043544` interval really
did refuse all 1306 frames; keep that finding scoped to that run. Their
different outcomes follow the foreground query-safety gate, without proof
that the VR transition alone caused its previous unsafe state.

This is a valid active-AA older-code reference, with remaining workload
differences: Frontier uses `treated-jittered-hdr` at native 3840x2160;
Steam `ee68d075` used the copy route at 2496x1404, HDR refusing because
render was below output. During these Frontier windows substituted draws
rise 1399/2637/3234/4322/4762, while approximate FPS inferred from adjacent
CPU report intervals falls 151.3/135.1/129.0/115.6/109.6. These are changing
views/load, not a held 90-FPS benchmark. Earlier 300-FPS blank/no-3D windows
and the partial exit window ending 04:41:30 are excluded. CPU timing is off,
so no EDVR CPU milliseconds are available. Steam slow2 had about 6545
substituted draws/frame. The comparison supports view-dependent load and
active AA on both builds, not a causal storefront/forward-adapter share of
the difference. Avoid claiming that GPU span is GPU busy time or inverting
Present p50 as average FPS.

CPU companion cutoff is 04:41:25.568; AA cutoff is 04:41:25.491. Frozen
evidence: `build/frontier_208ea3e8_043900_summary.json` and `.md`, plus
`build/frontier_dlss_active_208ea3e8_frozen.md`. Sol normalized the logs and
traced routes; Astra evidence-only review PASS, no arithmetic blocker.
AA-window frames total 3209 and adjacent CPU-window frames total 3207;
independent report boundaries explain the difference. Keep these populations
separate. No new source/build/install change from this read; Steam
`709ba499` remains installed and main unmerged. Use the user's 300-FPS limit
for the next Steam slow/fast/slow check, keeping the comparison conditions
steady; no new settings-file edit is needed.

## 2026-10-18: query-once and callback capture results

User completed slow/fast/slow with the 300-FPS limit. Sanctioned reader
verifies `edvr_gfx_20261018_044929.log` as installed `709ba499`,
`v0.20.0-rc.4-19-g709ba499`, PE `6AD4A15C`, flat `/novr` and DLSS.
Freeze CPU/copy at 04:53:45.379 and paired runtime at 04:53:45.120;
exclude subsequent growing-log rows. Automatic discovery finished at
04:50:45.243, 12,000 useful frames, before all three measured legs.

| Core (five-second report ends) | Real FPS | Substituted draws/frame | Callback gross ms / calls per clocked frame | Raw-domain ms / calls per clocked frame |
| --- | ---: | ---: | ---: | ---: |
| Slow1, 04:51:40-04:52:05 | 63.6 | 7046 | 4.426 / 14354 | 1.619 / 8075 |
| Fast, 04:52:20-04:52:40 | 204.9 | 139 | 0.160 / 369 | 0.052 / 337 |
| Slow2 first 30 seconds, 04:52:50-04:53:15 | 66.3 | 7236 | 4.499 / 14723 | 1.646 / 8301 |
| Slow2 full, 04:52:50-04:53:45 | 64.4 | 7261 | 4.558 / 14773 | 1.669 / 8322 |

FPS uses real CPU-report frames divided by paired logged window seconds;
CPU time/calls use clocked-frame weighting. CPU and AA reports have slightly
different boundaries; retain their separate frame populations. Callback
timing is exclusive of existing nested families and excludes the game's
Draw, but includes other begin/end work besides forward Issue validation.
The whole 4.6 ms cannot be assigned to DSV proof without finer evidence.
Slow2 whole CPU is 13.709 ms gross / 11.220 ms recorded-floor net estimate.
The directly recorded callback floor estimate is 0.236 ms, estimated
callback clock cost about 0.488 ms; neither is an uninstrumented benchmark.
Old gross 8.504 / net 6.414 ms omitted callbacks. Do not subtract gross
callback time from expanded floor-net totals or call that increase a
regression. Current slow2 also has about 11% more draws than old 6545/frame.

Query accounting is internally consistent: slow2 raw DSV Gets 5,578,313 =
5,577,829 description-cache hits + 484 misses, a 99.9913% hit rate; depth
state description hits are 99.5437%. Raw-domain work averages 200.5 ns/call
against old 207.5, with 8322 versus 7485 calls per clocked frame and changed
route/call mix. This does not isolate a time saving from this edit.
Issue Geometry reaches 3,564,170 snapshots / 242 clocked frames = 14,728
per frame; Light is zero. Each reached stage now performs one actual DSV
query instead of two, retaining separate before/after Draw validation.
The Geometry count is a subset of raw DSV Gets, not another getter count.
The removed outer pre-get was uncounted previously, so stable raw counts
cannot refute the removal or measure milliseconds saved. Zero Light sites
do not certify that earlier producer/provenance observations can be skipped.

AA passes all 23 core windows: slow1 1909/1909, fast 5119/5119, slow2
3865/3865 accepted history and completed HDR backend, 10,893/10,893 total.
No new reset, refusal, backend failure or jitter-failed frame in the legs;
treated streak reaches 15,109. Four cumulative startup/scene-entry resets
precede the legs; the last is the 04:51:10 scene entry with no-previous=1.
Ruled out: repeated AA disengagement/history loss as the cause of this
capture's held slow-view cost, because every measured leg frame continues
history and completes treatment. Current Steam uses native 3840x2160 HDR,
matching the valid Frontier route; previous Steam used 2496x1404 copy.
Frontier clocks were off and draw load differed, so no isolated historical
or storefront CPU/FPS regression size follows from these captures.

Frozen artifacts: `build/per_draw_issue_709ba499_summary.json` and `.md`,
`build/per_draw_issue_709ba499_aa_frozen.md`, reader output
`build/per_draw_issue_709ba499_filtered.txt`. Sol normalized CPU/counters
and independently traced AA/routes; Astra evidence-only review PASS, with
no arithmetic or attribution blocker. Astra ran no tests/builds/benchmarks.
No source/build/install change from this log review; Steam stays `709ba499`,
settings unchanged, probe off, main unmerged. Saved evidence is sufficient;
normal quit is safe. Remaining CPU target is the newly exposed callback
work; trace/subdivide it before selecting a guarded optimization. Phase
bootstrap remains outside this validation.

## 2026-10-18: callback-internals optimization preflight

Hypothesis: Geometry's camera proof repeats bounded tracking/shadow-table
searches inside one serialized owner-thread read, adding avoidable work at
the 14,728 reached sites per sampled slow frame. This does not attribute
all 4.558 ms of callback work to that proof. Discriminators: exclusive
shader, DSV/depth and constant-validation timing, plus counted production
snapshot lookups and fresh binding checks in the test fixture.

Source evidence before edits: `forwardIssueValid` calls `lightBoundCb`,
which calls `constantsMetadata`, `copyConstants`, `constantsMetadata`.
Each runtime operation searches 64 retained tracked entries and then the
shadow bank. The first metadata must say tracked before copy is attempted,
so the private-buffer fallback in `copyConstants` is unreachable here.
`FlatProjectionRuntime::owner()` requires context plus owner thread ID;
map/unmap/update/invalidation writers require that owner, and the shadow
bank documents single-owner serialization. There is no D3D call, external
callback or supported concurrent writer between the three old reads.

Candidate: original-only snapshot API gets the same metadata and 96 bytes
from one tracked entry and one shadow view, preserving mapped/pending,
shadow/generation, range, freshness and identity rules. Actual CB/range is
queried anew on each side of game Draw; shader and DSV/depth checks stay
fresh too. Diagnostic readers/private fallback stay unchanged. Retain the
actual immutable buffer width read initially to limit scope. Require
production-used fixture coverage of failure cases and mutations between
separate snapshots, not a mirror-only helper test. Astra reviews the owner
serialization/equivalence evidence before implementation. No broad producer
skip, cross-Draw mutable-state cache or new configuration key is proposed.

Implementation is deliberately limited to Geometry Issue validation. New
`snapshotTrackedConstants` reads one retained original tracked entry and
one shadow view; metadata and the byte copy use that same view, with no
external call, allocation or supported writer in between. `lightBoundCb`
selects it explicitly only for this production check. Fresh actual CB,
range and `GetDesc` remain; shader, DSV/depth, owner/foreign/phase, identity
and before/after game Draw validation remain too. Ordinary writer observation
and Full diagnostic readers retain their old path. Width caches, smaller
scratch records and hash changes were deferred to keep this proof narrow.

Three exclusive child timers separate Geometry shader/predication, DSV/depth
and constant/camera validation. The parent label is now `draw issue callbacks
residual`; compare its sum with `issue geometry shader`, `issue geometry
DSV` and `issue geometry constants` against the old callback family. A lower
residual alone is a category change, not a speedup. Three additional clocked
scopes per fully reached Geometry validation require refreshed floor
estimates; instrumentation is not free. Stage calls remain sampled counts,
not all real-frame operations.

Sol focused temporal and WARP `projectionRuntimeTests,projectionSnapshotTests`
PASS. Runtime tests exercise the production API and actual shadow writers,
exact bytes/metadata, chronology/freshness, range, wrong owner, invalidation,
reset, private/untracked, mapped/pending and tracker eviction. Eviction
checks old identity Untracked and replacement Full, not numeric generation
values directly. Source pins check one tracked lookup plus one shadow lookup
and fresh actual CB/width selection; this is structural work reduction,
not a measured millisecond saving. Fake clocks verify exclusive child sums,
game Draw exclusion and disabled-clock behavior; maximum-width report checks
cover the new names. Stale formatter expectations were repaired. The large
new runtime fixture moved to a separate heap-backed suite after the first
layout exceeded the existing test function's stack; final focused rerun
passes, and Astra verified the suite executes and propagates failures.
Astra final source/test review CLEAR, evidence only, no execution. Full
validation ran under the single build lock before any source commit.

Full validation PASS: absolute `build.bat --wait-lock`, 151.3 seconds
reported / 152.6 wall, 170-job pool, d3d11 8 compiled / 133 reused.
Production DLLs, all Python and rig gates, installer resource validation
and final receipt pass. Forward cache/helper checks 55/0; the WARP shard
runs both `projectionRuntimeTests` and `projectionSnapshotTests`. Worst-width
CPU output fits five lines (985/973/867/962/459 chars, all below 1000).
Full receipt inputs:
`96cc23878fd2aede59ecad4965f77ecd70174f5663ee5f7af11fd5cffe1e05de`.
Log: `build/callback-snapshot-full-build.log`; final all-gates PASS read.
Both Steam settings hashes still match the pre-build snapshot. Source
commit, clean DLL promotion and sanctioned Steam flat install follow.

Clean source `041f8e78` promoted by absolute
`build.bat --dll-only --wait-lock` PASS, matching full receipt, 34.3 seconds
reported / 35.5 wall. Version `v0.20.0-rc.4-23-g041f8e78`; d3d11 5 compiled /
136 reused. Game was closed. Sanctioned Steam flat dry-run, install and
verify-only PASS; built and installed graphics DLL SHA256 both
`026F38A011E3AF1B07CA78C06EEA7C89A1B567E484F143DD01452C183491E05B`.
Both Steam settings hashes are unchanged; CPU timing remains on, origin
probe off. No Frontier file changes or main merge. Evidence:
`build/callback-snapshot-promotion.log`,
`build/callback-snapshot-validation.json`. Source pushed on feature branch;
flight performance and AA continuity on this installed build remain pending.
Astra final metadata review CLEAR: full receipt matches and promotion targets
the clean source commit; no measured flight benefit is claimed.

Next capture: launch Steam flat with DLSS and the same resolution/settings
and 300-FPS limit as the `709ba499` capture. Load the same carrier position,
close menus and wait two minutes in the slow view. Hold slow for 30 seconds,
turn at least 90 degrees to the same fast view for 30 seconds, then return
to the same slow view for 30 seconds. No F7, NumLock or manual capture arm.
Normal quit afterward is fine. Read the log with
`edvr_log.py --target steam --expect-build 041f8e78` first. Compare callback
residual plus three children with old callback gross, update clock-floor
estimates and check raw-domain/cache counters, draw load, route and AA
history/refusal/reset/backend continuity. Shader/DSV/constant child calls
must be nonzero where their stage runs; absent labels with clocks active
do not count as a zero-cost success. Keep claims of saved milliseconds
conditional on that capture and its workload.

Completed Steam capture, user-confirmed: log `edvr_gfx_20261018_052052.log`,
build `041f8e78`/PE `6AD4AAE7`. Freeze CPU/copy 05:27:48.247 and runtime
05:27:47.834. Final last slow1 six windows end 05:24:38.027: 59.03 FPS,
7234 draws/frame, gross15.940/net estimate12.647 ms. Earlier 61.64-FPS
slice ends 05:24:17.991 and overlaps; do not label it last30 or sum both.
Fast 221.29 FPS; full stable slow2 65.18 FPS,7085 draws/frame,
gross15.070/net estimate11.891, callback residual+shader+DSV+constants
1.430+1.609+0.700+2.237=5.976 ms. Selected nonoverlap AA cores total
1771+8849+7822=18,442 clean HDR/backend/history frames, no new reset,
refusal or jitter failure. The 05:25:48 CPU/copy report exists 1ms apart;
the parser now pairs it and excludes it as the first stabilization window.

No demonstrated useful speed win. Slow1/early slow2 FPS fall; full slow2
is slightly faster with fewer draws. Whole callback coverage is unchanged
from709; extra children subdivide it and add clocks/gates. Full slow2
whole floor-net rises0.671 ms; combined callback recorded-floor rises0.682
ms (three new child floors total0.688). Neither source-specific regression
nor instrumentation-only cause is isolated. Ruled out: using the existing
engine cache unchanged as a complete forward-Issue certificate; its actual
1in64 probe covers shaders only, and raw/unhooked state paths remain.

User-requested handoff in active checkout:
`docs/flat-callback-cpu-handoff-2026-10-18.md`. It records environment,
installed/source/worktree identities, final artifacts, complete mutation
coverage needed for run reuse, and a narrower camera-proof option.
Final evidence: `build/callback_snapshot_041f8e78_final_summary.md/.json`,
JSON SHA256 `F6FDEBC27750060BF4B251364F145B8007817E1AEDDA83D84CE1EC9B4CE5AB52`.
Sol normalized/analyzed; Astra reviewed corrected selection/arithmetic.
No new source/build/install from this read; main remains unmerged.
