# Flat on-foot CZ frame rate (report of 2026-10-11)

## Status

State (2026-10-16, results): the observer fix is FLOWN, CONFIRMED and ON MAIN (1ae94885, `claude/engine-motion-observers`): render-thread
engine motion 427 -> 77 ns a call (about 0.8 ms a frame), job merge and clear down 92%, 100% of calls lock-free, 0 contention. The
instruments (H1-H4, round 2) are on `claude/flat-settlement-cpu-2`, NOT on main. Round 2 flown (DLSS, 14.6k draws a frame): EDVR render
thread 5.17 ms floor-net (353 ns a draw), Present p50 14.91, GPU span p50 12.52, 63.6 fps. AA-off A/B: ~145-150 fps against ~150 with EDVR
suspended; the whole gap is the AA path. Section "2026-10-16, results" has the numbers. The arc is NOT closed: ~5 ms of per-draw CPU is
spread thin (about 20 pieces, none above 0.6 ms); the open question is structural (a per-pipeline-state "nothing to do" fast path).
Ruled out (2026-10-16, round 2): the unscoped binding thunks as the 2.4 ms unexplained (about 7.3k bind calls a frame at about 30 ns net
is 0.12-0.24 ms); a single hot block in the draw constructor (its 5 blocks hold about 0.85 ms net); g_mutex contention (0 contended).
Next candidates, unbuilt: the overlay guard reading t3 from the shadow (0.2-0.4 ms); the ctor lookup memo (~0.2); the per-draw UI-layer
ask memo (0.1-0.2); the census "pool-family draws" GPU timing to price ~1.0 ms; the structural fast path. Designs (d) guarded runs and
(e) census cost: section "2026-10-16, round 2".
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
