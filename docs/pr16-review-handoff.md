# PR #16 review handoff — `transition-flash-run-radius`

Reviewed at head `d7734d1`. Everything below was reproduced locally against a
worktree of that commit, with `main` (`33a6f06`) built from the same fixtures as
the control. Where a claim is "measured", the probe and its result are given so
you can re-run it rather than take it on trust.

## What is already good, so nobody re-litigates it

- `build.bat`'s glitch suite is green at PR head; green on main too.
- Config contract 90/90, no new `/W4` warnings.
- **The field judder replay improves: 3 withheld frames on main → 1 on the PR
  head.** The PR body claims 2 → 1; the direction is right and the gain is
  bigger than claimed. The two-frame-glitch cells and the 551-frame flight
  replay are unchanged.
- The five new fixtures have teeth. Reverting `runStillSamePlace(s, pos)` to
  `true` in the `willMark` chain fails exactly three cells — the hyperspace
  shape, the 2,100 edge, and the anchored-not-chained walk — and pushes the
  field replay back to 2. No fixture passes vacuously.

## Blocking, in the order I would fix them

### 1. The separation verdict shadows `kVerdictMovedOn` — regression vs main

`glitch_frame.cpp:2060`. The verdict ladder asks
`residualIsKnownSeparation(resid)` before the moved-on arm at 2064, and that
predicate does **not** consult `separationMode`. So a run continuation that
`runStillSamePlace` refused is labelled `kVerdictSeparation`, and the boundary's
declare branch — which keys on `verdict == kVerdictMovedOn` — never fires.

The frame instead falls through to the legacy resolve at 2576, reads Stayed, and
arms `cooldownUntilMs = now + 1330 ms` with `rebaseProvisional` never set. There
is nothing to revoke it. It is also counted `++suppressedBySeparation` and fed to
`recordResidual`, which is exactly what the moved-on branch's own comment at
2273-2280 says must never happen to a rebase-band magnitude.

**Scope: `transition_flash_separation = log` or `off` only.** In the shipped
default this is safe, and it is worth being precise about why, because the State
field initializer misleads: `State::separationMode = 1` at line 719 looks like
the default, but install overrides it —
`cfg.getString("advanced.transition_flash_separation", "act")` at 1602 — and
`edvr.ini:360` documents `act`. In act mode the frame sets
`suppressedThisFrame`, the boundary takes the deliberately-empty deferral at
2429, and nothing is armed. Four separate review passes called this a
default-config bug on the strength of line 719; it is not.

Measured. A probe that certifies the 568k cascade separation, switches to log
mode (certifications survive re-install — `seps` is only cleared at
construction), withholds a bad frame, then feeds a certified flip as the run's
second frame:

```
[trace] f6551 resid=568000 sepKnown=1 mark=0 consec=1/2
[bnd]   f6552 verdict=4 sup=0 await=1 prov=0 -> legacy-resolve
[trace] f6565 resid=27500  mark=0 cd=1          <- genuine flash, LET THROUGH
[bnd]   f6566 cdLeft=1175
```

`verdict=4` is `kVerdictSeparation`. The same fixture built against main's
`glitch_frame.cpp` passes: main withheld the flip as the run's second frame,
armed nothing, and caught the flash. This is fixture 7m's regression arriving
through the separation door.

Fix direction: the moved-on test has to be asked before the ladder's recognition
arms, or recorded where `runStillSamePlace` actually refuses the mark rather than
reconstructed by elimination in the ternary. See item 9 — the two are the same
edit, and doing it that way fixes this class rather than this instance.

### 2. The confirmation frame is a blind spot at `max_consecutive >= 3` — regression vs main

`glitch_frame.cpp:2566` (declare) with `2440` (confirm). The declare arms the
stand-down and zeroes `camPrevValid` a frame earlier than the old machine did.
The next frame therefore cannot be judged at all — `glitchFrameObserve` bails at
its `camPrevValid < 2` gate — so a genuine flash landing there is shown, and its
position is what the confirm branch then measures `back` against.

Measured, at `max_consecutive = 3`:

```
[trace] f6927 resid=41000 mark=1 consec=0/3     <- bad frame, withheld
[trace] f6928 resid=96000 mark=0 consec=1/3
[bnd]   f6929 verdict=10 -> DECLARE  (cooldown armed, camPrevValid = 0)
[bnd]   f6930 verdict=0  prov=1 -> CONFIRM      <- the flash frame: Quiet,
                                                   never judged, shown
```

There is no `[trace]` line for the flash frame at all — it did not even register
as a jump. Same fixture on main: withheld. At the shipped `max_consecutive = 2`
there is no regression (main let that frame through as `kVerdictConsecutive`
anyway), so this is scoped to rigs that raised the knob — which fixture 7n
itself does.

The confirm branch's comment lists three limits and says "None is a regression."
This case is not among the three, and it is one.

### 3. `transition_flash_run_units = 0` does not do what it is documented to do

`glitch_frame.cpp:888`. `d <= s->runUnits` accepts `d == 0`, so with the knob at
0 a byte-identical repeat still continues the run. `edvr.ini:352` promises "0
ends every run at a single frame" and the install comment at 1502 repeats it.
Byte-identical is not an exotic case: it is 2 of the 7 runs in the PR's own
measurement table, the class the table leads with. Negative values clamp back to
2000, so no value of the knob delivers the documented behaviour.

Fix: `d < s->runUnits`. Verified — the contract cell passes and every existing
cell including both 1,900 / 2,100 edges stays green.

## Worth fixing before merge, cheap

### 4. Install clears `rebaseProvisional` but not `cooldownUntilMs`

`glitch_frame.cpp:1483-1490`. They are armed as a pair; clearing only one
converts a revocable stand-down into a permanent 1330 ms one, because the revoke
path requires the flag. Fixture 7n exercises this today — its third walked frame
declares (`[bnd] f6150 -> DECLARE`) and the next statement re-installs; the leak
is absorbed only because the trailing `settle(b, x, 150)` outlasts the window.
In the field the trigger is a device recreation landing within a frame of a
declare. Add `s.cooldownUntilMs = 0;` to the reset block.

### 5. The suite cannot see `transition_flash_run_units` at all

`kIni` never states the key and no fixture sets it. Its default, 2000,
coincides with both `transition_flash_units`' default and `Config::getFloat`'s
missing-key fallback — so the 1,900 / 2,100 edge cells pin a number that arrives
by three different routes. Measured: renaming the key at the read site to
`transition_flash_run_unitsX` leaves the **entire suite green**. A typo there, or
a regression to sharing `jumpMin`, ships silently. Add the key to `kIni`, and
ideally one cell that sets it to a non-default value.

### 6. The new fixture comment asserts the design the PR abandoned

`glitch_test.cpp:993-999` says "AND THE RADIUS IS THE JUMP THRESHOLD ...
`transition_flash_units`, not a number of its own" — the one-commit design the
shipped code, `edvr.ini`, and `docs/transition-flash.md` all spend paragraphs
rejecting, on the grounds that the two knobs pull opposite ways. A tuner who
reads the fixture that claims to pin the edge retunes the wrong knob, which is
precisely the half-wrong advice the split exists to prevent. The PR's second
review pass reported fixing this contradiction in the doc; it survives in the
test file.

### 7. `runStillSamePlace`'s header still says six runs

`glitch_frame.cpp:870`: "with the six runs it was taken from." The block it
points at lists seven and explicitly records that an earlier draft saying six
"made the evidence look tidier than it is." The predicate's own header
reintroduces the retired count.

### 8. `jumpFactor` never got the `isfinite` its sibling did

`glitch_frame.cpp:1500`. The PR hardens `jumpMin` at 1694 with a comment saying
"Its four neighbours all check this" — `jumpFactor`, read one line below
`jumpMin`, has no validation at all. `transition_flash_speed_factor = 1e40`
parses to infinity through `strtof`, `trip` goes infinite, `resid > trip` is
false forever, and the detector arms, logs ACTIVE, and withholds nothing for the
session. The hole is pre-existing; the comment asserting it is closed is new.

## Structural, take or leave

### 9. The moved-on fact is reconstructed instead of recorded

`glitch_frame.cpp:2064`. `kVerdictMovedOn` is derived by mirroring `willMark`'s
conjuncts by elimination — and this verdict is not just a label any more, it
drives the declare. Two consequences: the `s->consecutive > 0 &&` conjunct is
provably dead (the arm is only reachable when the cap or the run rule refused,
both implying `consecutive >= 1`), and, more seriously, item 1 exists because
the reconstruction disagrees with the decision. Recording `movedOnThisFrame`
where `runStillSamePlace` refuses — the way `*SuppressedThisFrame` already works
— fixes item 1 and deletes the dead conjunct in one move. This file's own
comment at 2336 records that a predicate evaluated in two places has drifted
three times already.

### 10. The confirm branch is a verbatim third copy of the resolve

`glitch_frame.cpp:2503-2534` duplicates the `back` extrapolation
(vs 2578-2585), the came-home restore (vs 2589-2593), and the advance-track step
(vs 2652-2657). Three finder angles landed on this independently. The copies have
already drifted — one guards with `isfinite(back)` and the other does not, with a
comment explaining the asymmetry — and the branch's own comment queues two future
edits to this same computation (gate the revoke on `lastRunSpread`; the
three-frame `camPrev2` splice), each of which would have to be mirrored by hand.
`backFromPreJumpPath()` / `restorePreJumpTrack()` / `advanceTrack()` beside the
existing `burstDown` / `rebaseDown` predicates would keep one of each.

### 11. Two comment claims that are not true of the code

- `2529`: "Carry on rebuilding under the stand-down, exactly as the Stayed
  branch below leaves things." Stayed writes nothing into `camPrev` and leaves
  `camPrevValid = 0`; this branch splices `frameFarPos` into `camPrev` and
  leaves `camPrevValid = 1`. A maintainer reasoning about the rebuild from that
  sentence gets it wrong.
- `2736`: `letThroughMovedOn` is passed raw into the totals line while its
  neighbours pass since-last deltas, and the print gate at 2710 does not fire on
  it. Every totals line repeats the session total. Note `withheldNotRendering`
  beside it is also raw, so this matches existing practice rather than breaking
  it — worth a snapshot only if you are touching that line anyway.

### 12. Smaller cleanups

- `markedPos` may be removable: writing `runAnchorPos` directly at the mark site
  under the same `consecutive == 0` guard looks equivalent, since a withdrawn
  mark leaves an anchor that cannot be read while `consecutive == 0`. Not
  exhaustively verified — check the `kVerdictWithheldSepWould` path can never
  make a frame count as withheld without a `willMark` candidate before acting.
- The two threshold-edge cells are one fixture with two constants changed, and
  the vacuous-guard prefix is copy-pasted four times; the file's own `wakeDrop`
  lambda at 1677 is the precedent for collapsing them.
- Fixture 7n restores `max_consecutive` with a literal `"2"` duplicating `kIni`
  900 lines up — the second fixture to flip that key. A scoped-config RAII would
  make restoration structural. Inherited convention, not this PR's doing.

## What still is not answered by any of this

The PR says it plainly and it remains true: **none of this is field-verified.**
The suite passing is not evidence the reported hyperspace flash is fixed. It
wants a jump in a headset. Everything above is about not shipping a new
regression alongside the fix.
