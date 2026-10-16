# Flat on-foot CZ frame rate (report of 2026-10-11)

## Status

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
Working files (gitignored): `reviews\flat-cz-fps-findings-2026-10-11.md`, `reviews\flat-cz-fps-review-2026-10-11.md`.

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
