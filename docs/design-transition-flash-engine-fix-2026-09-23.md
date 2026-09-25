# Transition flash: preventing it inside the engine

*Design, 2026-09-23, from three Ghidra rounds and a re-read of existing flight
logs. Built and flown the same evening (flight 184826). The flight REFUTED the
static chain: see "Flight 184826".*

## Status

- **The view write was corrupting: column-major groups, and the finder was
  hitting prev-view (flight 160557).** The buffer's view groups store their
  translation in the w lanes (flat [3],[7],[11]); viewOriginMatch read
  [12..14] (zeros) and degenerated to |origin| on head-only frames, so the
  located "view" was the PREVIOUS frame's view (its implied eye decodes to
  the old base within 5 cm) and the row-major postmultiply wrote
  non-orthonormal garbage (norms 55/206) -- the persisting flash. Origin
  and the pilot block were exact throughout. Fix: w-lane translation read
  (prev-view rejected), the view premultiplied by B^-1 (the eye corrects
  as a row-vector POINT transform, patchEyeOrigin(B, P) = P.B, verified
  flight after flight -- so the eye pose POSTmultiplies, E' = E x B, and
  its inverse view PREmultiplies), a write-time orthonormality guard
  (failure skips the view write, never corrupts), and origin+pilot patched
  on every gated fill with the view group added only when cleanly located.

- **The pilot block was the residual flash (flight 151942).** The patched
  exit fixed the world (origin+view exact to the next frame) but left rows
  276-279 (the pilot's position+basis) head-only: basis near-identity vs
  the next frame's full rotation -- Sean's "different, faster but still
  noticeable". Fix: premultiply rows 275-279 by B too (276 = origin,
  277-279 = R(B) x basis) -- the eye's own premultiply; the dump's
  next-frame rows are the ground truth and fit. The latch held
  (poolEarly=yes, scene-new -> live); wouldDiffer's 76 was the metric's
  store-advance artifact (re-evaluation compared the upload to itself),
  now re-evaluated against the latch-time snapshot; `base=` reports the
  latch. Controls 11598/14537 flashed by design (alternate).

- **Review finding FIXED (2026-09-24): the fill-time selector read
  evidence computed after the fills.** The patch now decides ONCE at the
  frame's first head-only fill from the pool's own upload when present
  (compare on a copy, camera = row 275), latches base and B for every
  fill, and is all-or-nothing (no evidence -> no patch; poolLate counted
  per frame). Unclear/thin -> NoPatch; the detector's matched>=32/finite
  floor adopted. Also fixed: frameFarPos/H3 pre-tap copies, VP scan on
  original rows only, and the wouldDiffer metric now re-evaluates against
  the latch-time pool snapshot, never the advanced store.

- **Locator VALIDATED; the selector is the decision (flight 134813, build
  7eb4a536).** Per-fill structural view location works (the row moves
  64/85/233/282 by pass); corrO within 6 cm of the next frame's eye at every
  scene-new event. Skip 22726 (a scene-old hyperspace entry: the objects had
  not switched, the mailbox already held the tunnel base) proved the base
  must be selector-chosen: scene-new -> live, scene-old -> held; a
  selector-less live patch would have flashed 1614 m there. The pool and
  the writer's scene graph can switch a frame apart; the patch agrees with
  the POOL. live=RESET at one control tap (bad-frame taps 10/10 ok).

- **LIVE-READ VALIDATED (flight 125237, build e9fefca0).** At all four
  skips the live mailbox at the bad render's tap, premultiplied onto the
  head-only eye, equals the engine's own next-frame eye to the millimetre;
  zero live=RESET taps at bad frames; the record-indexed new candidate was
  stale at every tap (refilled age=3) and is ruled out as the patch's
  source.
- **Goal (Sean, 2026-09-23):** stop trapping the bad frame and stop Elite
  rendering it at all, by fixing the order inside the game. The trap stays
  on as referee until the acting build verifies.
- **What the bad frame is (measured):** `cb1[275]` of the 5376-byte scene
  CB lands on the head pose alone for one frame (within ~14 cm of the frame
  origin); the ship/seat transform under the head is missing because the
  eye-base mailbox (ship+0x3330, consumer `0x28431D0`, writer `0x2874B20`)
  is not refilled that frame. Every flash is a one-frame writer skip
  (100043, 125237).
- **Consume frame numbers lag render taps by ~1 wall-clock frame** (125237):
  the refill the bad render needs is already LIVE in the mailbox at the tap;
  the sim's window starts at N+1, not N, and the act fires at tap frame
  skip+1 (6/6 on 134813).
- **The render-time patch is on main** (simulation 310d9883, live read
  e9fefca0, locator 7eb4a536, the acting build 9a243675 plus the review
  fixes): `advanced.transition_flash_eye_base = off|watch|on|alternate`.
  The Steam copy runs the acting build, trap OFF, `alternate` (two marked
  ini lines). Read each flight with `--expect-build` set to the stamped
  commit its section names.
- **Hyperspace exits can be scene-new too** (125237): the second scene-new
  event on record after 100043's f23338. 22726 then proved the selector is
  NOT demotable: scene-old and scene-new both occur at hyperspace entries.
- **Environment:** game build 332841 (PE TimeDateStamp 1788384820, image
  104,894,464), the same exe in both installs. Independent of VR runtime,
  headset, eye size and DLSS: the code is Elite's camera, below all of them.
- **Old chain (compose `0x23BC8A0` / recompute `0x3CEE650`):** REFUTED
  (flight 184826); its hooks stay inert until Sean says to remove them.
- **Instrument history:** the journal below; each flight section carries its
  own as-installed note.
- **Ruled out:** see the list at the end.

## What the trap costs

The per-frame cost is smaller than it looks. The eye `CopyResource`
(`eye_capture.cpp:284`) belongs to the OpenXR runtime and stays whatever
happens here; the trap only swaps pointers. Its own work per frame:

- the scene cross-check, 0.133 ms at its cap (4 reads of 33.2 us);
- the size compare on every Map, up to 0.4 ms a frame before its memo
  (`vscreen.cpp:3391-3401`), not measured since.

It runs even with `fix.transition_flash = 0` (`glitch_frame.cpp:1577-1618`,
"Off, but still watching").

The real price is correctness. Each catch shows a repeated frame. The
recognisers exist because false positives cost frames. A miss shows the flash
(issue #34 was closed on timing, never re-measured). An engine fix removes all
three and the per-frame work with them.

## The engine chain (static, build 332841)

| Role | RVA | First bytes / CodeHook |
|---|---|---|
| Camera manager, per-item loop; run from a 4-byte RVA table at .rdata `0x5CCCCE8` (scheduler-driven, no direct calls) | `0x237BAC0` | not a hook target |
| **Compose:** resolve the parent through `*(mgr+0x180)` vtbl `+0x60` with key `*(item+0x18)` (refcounted, released by `0x3CFA0C0`), decode, combine, push | `0x23BC8A0` (twin `0x23859A0`) | `48 89 5C 24 08 48 89 7C 24 10 55 48 8D 6C 24 D0`, clean |
| Item's local pose: 128-byte copy of its slot `+0x80` | `0x3CED470` | |
| **Parent world decode:** 128-byte copy of the parent's slot `+0x100`, no branch, flag or version check | `0x3CEE4C0` | `8B 81 84 03 00 00 ...` (`mov eax,[rcx+0x384]`); decoder support to verify |
| Combine: double-precision affine compose | `0x8C1640` | `48 8B C4 48 81 EC C8 00 00 00`, clean |
| Push into both views' `*(+0x168/+0x178)+0x70` at `+0x1130`, via the one-instruction wrapper `0x1290E90` | `0x3D0CF10` | `48 8B C4 48 89 58 10 48 89 70 18 57 48 81 EC 90`, clean |
| From-root recompute: walks the `+0x350` chain composing each ancestor, writes nothing back | `0x3CEE650` | signature to confirm |
| World-pass writer: a switch-case body in a dispatcher that sweeps indices `< *(container+0x380)` | `0x3CFE748` | not a function, not hookable |
| The only setter of the manager's re-target latch (`+0x3D8 = 1`) | `0x23AE880` | `48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20`, clean |

Pool slot address: `*(*(node+0x378)+0x388) + *(uint32*)(node+0x384) * 0x180`,
with three 0x80-byte double blocks at `+0x00` (unknown), `+0x80` (local) and
`+0x100` (world).

One link is unproven. The push stores doubles and `cb1` holds floats, so
`+0x1130` is at best the double-precision source that a later step narrows.
Confirming the link is the instrument's first job (H3).

Why the engine can do this: the world pass sweeps indices below the
container's count. A node allocated after the pass has run keeps its
allocation contents until the next pass. The compose resolves the parent by
key, so after a transition re-targets the key, the first frame can read that
unwritten block. Allocation itself was not found statically.

Dumps: `analysis\decomp\flash\` (round 1), `\r2\`, `\r3\` in the main checkout
(gitignored); scripts `analysis\ghidra_scripts\Flash*.java`.

## Hypotheses and what tells them apart

| | Hypothesis | Signature in the Phase 1 log |
|---|---|---|
| H1 | New parent node allocated after this frame's world pass | on N the parent pointer or slot index changed; slot index `>=` `*(container+0x380)`, or block differs from recompute; block first written by N+1's compose |
| H1' | Same, but the block is written later in frame N (a same-frame race) | block changes between N's compose and N's `cb1` upload |
| H4 | Same parent, block reset (a pool re-init at the transition) | pointer and index unchanged on N; block is identity |
| H2 | A different push lands on N | the caller of `0x3D0CF10` on N differs from N-1's |
| H3 | This chain is not `cb1[275]`'s source | pushed translation differs from `cb1[275]` on ordinary frames |

H1, H1' and H4 all take the fix below. H2 takes the same fix at that site.
Only H3 sends the work back to static analysis, anchored on the Unmap stack
the instrument captures.

## Phase 1: the anatomy instrument (one flight)

Read-only. Built with `CodeHook`, keyed to TimeDateStamp + image size + the
prologue bytes above, and each hook stands down on its own.

1. `0x23BC8A0` entry: stash (manager, key, target) in a thread-local. After
   the original returns, record the push.
2. `0x3CEE4C0`, but only for calls from the compose sites (return address
   inside `0x23BC8A0`/`0x23859A0`; other callers only counted). Record: the
   parent pointer, `*(parent+0x384)`, `*(*(parent+0x378)+0x380)`, the cached
   world block's translation and basis, and the dry run.
   - **The dry run** is `0x3CEE650`'s recompute of the same node, its
     difference from the cached block, and whether the fix's test would have
     acted. It is SEH-guarded; a fault stands the recompute down for the
     session and says so.
3. `0x3D0CF10`: the pose and both view pointers. A `captureGameCallStack()` on
   the dump frames shows which site pushed.
4. `0x23AE880`: every call (frame, manager, item, key), to learn whether the
   latch is part of a transition.
5. At the scene-CB Unmap (the existing `vscreen` path): `cb1[275]` beside the
   last pushed translation, for H3. A stack capture every 300th frame and on
   N-2..N+2, which gives the upload's owner.

A ring of 600 frames, dumped by the detector's existing CameraReset/withheld
verdict and by Pause. The trap stays on as today, so the flight is no worse to
fly.

An armed line names each hook's install result, and a count line prints every
~20 s when anything moved. A build where nothing ran reads differently from a
build where nothing happened. The key is an advanced diagnostic, default off;
its name is to be agreed under the rule that values name functionality.

**The flight:** two high wakes (the canonical repro), two low wakes, a
supercruise drop at a station, an orbital glide entry, dock and undock, and the
galaxy map opened and closed. Press Pause after any visible flash. First
command after landing:
`python tools\edvr_log.py --target <store> --expect-build HEAD`.

**Pass criteria, before any fix acts:**

- H3 refuted: the pushed translation matches `cb1[275]` to float precision on
  ordinary frames.
- On every frame the detector flags, the compose shows a block that differs
  from the recompute. The recompute's eye is continuous with N+1's to within
  one frame of motion (after N+1's coherent shift).
- On every other frame of the flight, cached and recompute agree within a
  tolerance. That includes dock, undock, station approach, map and glide,
  where the rule must not act.

## Phase 2: the fix

In the `0x3CEE4C0` hook, and only for the camera compose's calls: if the
cached world block and `0x3CEE650`'s recompute disagree by more than a
tolerance set from Phase 1's measured normal-frame agreement, return the
recompute. The engine then composes the eye from where the ship actually is
on that frame. Nothing is withheld and nothing is repeated.

This is the value the engine would have written on its next pass, supplied at
the read that raced it. It is not a compensation.

**The floor, if Phase 1 disqualifies the recompute** (it disagrees on ordinary
frames, or is stale at N too): an exact trigger. The compose hook marks frame
N and the existing resubmit shows N-1. That has no per-frame detection and no
false positives, but it is still a discard. It is the floor, not the goal.

**Residual risk:** other consumers of the same unwritten node, such as cockpit
props attached to the ship, could still show for a frame. The detector cannot
see them; a headset can. If one is seen, widen the filter to the decode's
other callers (at least five sites use the idiom, including the scene-CB-fill
function `0x27FBFC0`).

**Verification flight:** the detector stays armed as referee. Pass means zero
CameraReset/withheld verdicts at transitions, with the fix's act counter
showing one act per transition and none anywhere else.

## Phase 3: retire the trap

- `fix.transition_flash` stays the one key, `on`/`off`, naming the
  functionality. On a build the hooks verify, `on` means the engine fix, and
  the detector is not armed. Not armed must mean nothing runs per draw or per
  Map, unlike today's `off`.
- On an unverified build the hooks stand down and `on` falls back to the
  detector plus resubmit, so a game update cannot silently bring the flash
  back.
- **Dependents:** `native_temporal.cpp` waits for a verdict only after a
  withheld jump frame (`:255-264`, set at `:311-315`); with none withheld,
  nothing waits. The eye copy stays with the runtime.
- The branches `transition-flash-run-radius` (PR #16) and `flash-cap-one`
  (PR #18) were never merged, and this supersedes both.

## Flight 160557 (2026-09-24 16:05, Steam copy, build c321631e)

alternate, trap off; build matched. Supercruise entry/exit; "a similar
flash" persists.
- Skip 11028 = watched control (flashed by design). Skip 11731 (patched):
  82 fills, live base, pilot block premultiplied; the origin exact to the
  millimetre -- but the flash persisted.
- **The view write was corrupting, not correcting**: the located group
  stores its translation in the w lanes; the row-major postmultiply fed
  them B^-1's translation row (~15.9) and wrote rows of norm 55/206. The
  located group was the PREV-VIEW (implied eye (+6.119,-3.432,+11.511) =
  the old base to 5 cm); viewOriginMatch read [12..14] (zeros in this
  layout) and degenerated to |origin|, so every "located view" on every
  flight was a head-only-frame spurious match (ordinary frames all read
  view=-1).
- Ruled out (160557): the view group as a row-major affine, and the
  locator's origin test as a current-vs-prev discriminator in this layout.

## Flight 151942 (2026-09-24 15:19, Steam copy, build e5daea6b)

alternate, trap off; build matched. Supercruise entry, exit, entry; no
hyperspace.
- 11598 and 14537 (watched): the controls, flashed by design. 12744
  (patched): 82 fills, live base, origin exact to the next frame -- but
  the flash persisted, "different, faster".
- **The miss: rows 276-279 (pilot position+basis) are base-derived and
  were left head-only** (bad frame: basis near-identity; next frame: full
  rotation). The world corrected; the pilot block did not.
- wouldDiffer=76 and base=held were the metric's own artifact
  (re-evaluation against the advanced store); the patch used live.
- vp=not-found at every event.
- Ruled out (151942): "origin + view is the whole correction" -- the
  pilot block is base-derived too.

## Flight 134813 (2026-09-24 13:48, Steam copy, build 7eb4a536)

watch, trap on; build matched. One supercruise entry, exit, entry again,
then a hyperspace entry/exit. No flashes (trap).
- Six skips (6729, 17703, 18021, 19945, 22726, 23885). At every scene-new
  event live-> == the next frame's eye exactly; corrO within 6 cm of it.
- The located view row varies by fill/pass (64/85/233/282): per-fill
  structural location is required; no fixed row. match275 ran 75-96% of
  fills (the rest are other views).
- **Skip 22726 (hyperspace entry) was scene-old**: pool 2.951 vs cam 13.525
  at the bad render while the mailbox already held the tunnel base.
  scene-old->held was right; a selector-less live patch would have been
  1614 m wrong (crossO flagged it). The writer's scene graph and the
  rendered object pool can switch a frame apart; the selector reads the
  POOL, which is what the patch must agree with.
- **live=RESET at one N+3 control tap (23888)**: the reset window is real;
  bad-frame taps stand at 10/10 live=ok across 125237+134813.
- Ruled out (134813): the live mailbox as the patch base WITHOUT the
  selector; a fixed view-matrix row.

## Flight 125237 (2026-09-24 12:52, Steam copy, build e9fefca0)

watch, trap on; build matched. Supercruise entry/exit, hyperspace
entry/exit. No flashes (the trap hid them).
- Four skips (13701, 14052, 16711, 17862); every covered tap live=ok. live->
  at the bad render == the next frame's actual eye: (-11.382,-1.368,+7.145),
  (-13.582,-2.234,+8.528), (+0.006,+3.504,+1613.257) (the tunnel),
  (-10.231,-8.062,+3.605).
- All four scene-new (pool~=cam) -- the hyperspace exit is the second
  scene-new one on record (after 100043's f23338). The live-read design is
  indifferent to the timing case by construction.
- Ruled out (125237): the consume-indexed lastRefilledM as the new-base
  source -- one refill stale at the tap in 4 of 4 (refilled age=3 at R;
  age=2 only at R+1).
- The consume frame counter lags render taps by ~1 wall-clock frame; that
  skew is why the record is stale and why the sim's window starts at N+1,
  not N.

## Flight 184826 (2026-09-23 18:48, Steam copy, build 980a0c84)

Sean flew high and low wakes, a supercruise drop and the galaxy map, and
skipped the glide and dock/undock. He pressed Pause five times; each press
wrote two whole-ring dumps to `edvr_logs\flash\`. The evidence is in
`edvr_gfx_20260923_184826.log` up to its cap and in those dumps.

- **The hooks ran.** The armed line shows all four installed and the
  recompute verified. Compose, decode and push stayed at zero through the
  loader, then started at f10385, when flight began. The latch never fired.
- **Not the eye.** Every compose-site push is at system scale, about
  (-1.6e8..-8.3e8, 1.6e9, -9e9) m. There are five distinct targets a frame,
  up to about 7e8 m apart, moving ~76 m a frame together. By the log cap,
  H3 read 1,844 frames at `>=1m` and none closer.
- **Unusable recompute.** At f10385, parent `0x17E10E2FB00` had its cached
  world at about 9e9 m and the recompute at about 1e12 m, with dr 1.78. Every
  compose-site decode of the flight disagreed, so the fix never validated.
- **Head-offset eye origins occur on ordinary frames.** From f10386 some
  5376-byte buffer's float 1100 sat at (-0.105, +0.055, -0.047). That is the
  docked-style, seat-centred frame (`per-object-motion.md`: docked
  `cb1[275]` = (0.030, -0.002, 0.000)). So a head-offset eye origin alone is
  not the bad frame; the detector's CameraReset rule rightly also needs the
  objects to disagree.
- **One canonical placement.** At f11068 the same pick was exactly
  (0, 0, +2.000), between head-offset values and km-scale ones: another
  transition placement, beside the high wake's (-0.02, +3.51, +1613.23).

## Flight 195435 (2026-09-23 19:54, Steam copy, build e8b37bb7)

`advanced.eye_origin_trace = on`, trap on. The build matched
(`--expect-build e8b37bb7`). Sean flew the list in reverse.

- **Coverage:** 18,582 frames traced, about 60 writes of the 5376-byte
  buffer a frame, 36 distinct write stacks.
- **Cost:** stack capture cost about 180 us a frame (diagnostic only).
- **Dumps:** 12 automatic dumps (the cap). Ten came from the detector's usual
  render-pass jumps (4,991, 16,256 and 153,410 units), which also used the
  cap up by 19:58:33.
- **f12604, world to ship-centred.** The eye origin goes from
  (-1174, +3425, +1439) to (-0.02, -0.01, +0.02) on the bad frame, then
  holds at (-5.97, +11.98, +2.02).
- **f13072, ship-centred to world.** It goes from (-5.98, +11.97, +2.00) to
  (-0.03, -0.01, +0.02) on the bad frame, then (-229.8, +663.0, +274.5),
  moving hundreds of units a frame.
- **One path.** Both bad frames were written with the eye buffer
  `buf=written` and by stack #0, like every ordinary frame. So the capture's
  blind spots (UpdateSubresource, deferred contexts) did not matter.
- **What is missing is the eye point.** |(-5.97, +11.98, +2.02)| is 13.5 m,
  the same size as the 13.505 m reset of the 2026-09-12 small-origin high
  wake.
- **Stack #0**, return addresses innermost first:
  `0x523207 / 0x51B69A / 0x501A4F / 0x4F8C98 / 0x5D92FD` (the upload path,
  common to all 36 stacks), then
  `0x4C8158F / 0x4C82D15 / 0x594ED5 / 0x58F2F4 / 0x58F9CF / 0x58AF82 /
  0x6BF929 / 0x594C3E / 0x2869073`.

## Flight 100043 (2026-09-24 10:00, Steam copy, build d28f9ada): the low-wake measurement

Trap ON, `watch` (nothing acted); the build matched. Sean flew three low
wakes and saw no flashes, because the trap hid them.

- **Seven scene-judged eye resets.** Frames 13549, 13939, 15653, 15965,
  17586, 17905 and 23340 (median object error 6.2-8.6 m; 264.6 m for the
  last). Each sits exactly 2 frames after a single-frame ENTRY edge (an
  un-refilled consume at 13547, 13937, ...): the consume-to-render offset.
  The first six come in pairs 3.7-5 s apart, the three low wakes' entry and
  exit.
- **Two long identity stretches** (2,729 and 5,813 consumes, both with
  EXIT edges) produced no reset at either edge.
- **The low-wake ENTRY (skip 13547, world to ship-centred).** On the
  skipped frame's render (f13549) the camera fell to head-only (step
  2,925.8). The ship's own geometry (the pool's matched records are the
  ship's parts; ordinary frames show cam = pool, relMed 0) had ALREADY
  rebased into the ship-centred frame on that same frame (pool step
  2,927.2). The right base there was the NEW one,
  (-0.660, +11.066, -7.725), which the writer produced only on the next
  consume. The held (old) base would be ~2.9 km off. This is the opposite
  of the hyperspace exit, where the scene was still in the old frame on the
  skipped frame.
- **The second skip of the pair (13937, ship-centred static to moving).**
  The ship's parts moved 14.7 m and then 17.2 m around the skip (matched
  57 of 87). Neither the held base (0 m) nor the next write (2.6 m) matches
  that. Object matching at this edge is too thin to call.
- **Conclusion.** Every transition flash is a one-frame skip of the eye-base
  writer. What differs by transition is whether the scene switches frames
  on the skipped frame (low-wake entry: yes) or a frame later (hyperspace
  exit: no). The correct base is the new one in the first case and the old
  one in the second. At consume time neither the new base nor the scene's
  switch is visible; at render time (2 frames later) both are.

## Flight 091726 (2026-09-24 09:17, Steam copy, build aecf9800)

Offered-base fix, trap OFF, `alternate`; the build matched.

- **Sean's report:** "low wakes entry/exit still flash, but high wakes
  don't".
- **The controller does not run on the event frame.** Every event frame
  shows `controller_calls=0 writer_entered=0 writer_wrote=0`, so the name
  gate is not it. No offered matrix was ever fresh, and every acted event
  used the held base. The skip is the controller's job not running for this
  camera that frame (round 7's candidate 3 or 5).
- **A low-wake entry is a MODE switch, not a skip.** At event #2 (5597) the
  controller stopped running altogether. The mailbox stayed at identity for
  5,041 consecutive consumes (frames 5597-10637, about a minute: the time
  in supercruise). So in supercruise identity is the base the engine means
  to use: the eye is the head pose relative to a camera-centred frame. The
  low-wake flash is the camera and the objects switching modes on different
  frames, at entry and at exit.
  - Holding the world base for up to 2 frames at the entry (acted at 5597
    and 5598) still flashed.
  - The exit refills the mailbox (the controller resumes), so there is no
    un-refilled frame to act on at all.
- **High wakes:** no flash reported. The one-frame skip there is fixed by
  the held base, as in 073114.
- **`ship+0x130`'s translation is (0,0,0) in every frame**, world frames
  included. It is not the ship's placement in the render frame, and nothing
  can be rebuilt from it.
- **During the flight** (09:23) the Steam ini's `temporal_aa` went from
  `dlss` to `off`. That was not EDVR's flight edit; it was left as found.

## Static round 7: the writer and its gates (dumps `analysis\decomp\flash\r7\`)

- **The writer is `FUN_142874b20`** (true entry `0x2874B20`, 239 bytes,
  chained pdata). It copies 64 bytes from `param_3` into
  `*(param_1+0x38)+0x3330` (the mailbox), then makes a notify call. It
  does this ONLY if `_stricmp(name1, name2) == 0`:
  - name1 comes from the last node of `param_2`'s chain;
  - name2 from `(*(param_1-8))->vtable[0x50]()`;
  - both are runtime strings with no literal xrefs.
  The first bytes are `48 85 D2 0F 84 E5 00 00 00` (TEST RDX,RDX; JZ
  rel32), a relocatable 9-byte steal.
- **Its caller is the camera controller tick `FUN_1410730a0`** (`0x10730A0`,
  first bytes `48 89 5C 24 10`, clean). It skips everything when
  `(*(param_1+0x190))->vtable[0x80]()` is false. Otherwise it fetches the
  base by a named lookup (registries `DAT_145f2afa0`, `DAT_145f501e8`, then
  the target's `vtable+0x20`), falling back to its own previous `+0x70`
  block, and hands that verbatim to the writer.
- **The job chain.** Per-slot iterator `0x10672C0`, job body `0x106F210`
  (busy gate, inline or deferred), system dispatcher `0x106C840`, frame
  tick `0x7F32D0` ("Long frame time"), outer driver `0x7F8B60`.
- **Skip candidates, ranked:**
  - (1) the controller's `vtable[0x80]` check is false for one frame;
  - (2) the writer's name gate fails for one frame. The matrix is offered
    and refused, as it would be if the camera's target name changes at the
    transition;
  - (3) the slot loop's bound is 0 for one frame;
  - (4) the writer's `param_2` is null.
- **Running the writer ourselves: rejected.** The objects differ from the
  consumer's (ship is at `+0x38` here, `+0x50` there, with no pointer path
  found). The chain has side effects: a notify call, a refcount release,
  the job loop's lock-free state.
- **What a flight can settle.** Whether the writer is called on the skipped
  frame, which gate fails, and what matrix it was offered. If (2), that
  offered-and-refused matrix is exactly "the base the writer would have
  written".

## Flight 073114 (2026-09-24 07:31, Steam copy, build b0d3632a)

Held-base fix, trap OFF, `alternate`; the build matched.

- **Sean's report:** "Hyperspace worked, the low wake didn't." The low wake
  was the first transition (event #2, acted, held base (17.6, -38.8, -68.2)
  m, age 1).
- **The fix acted on #2, #4, #6 and #8.** #6 and #8 were the two hyperspace
  exits. There the eye origin held the tunnel base for the switch frame,
  (-3.43, -0.63, +3860.50) and (-0.95, +3.38, +3860.89), where the watched
  control #3 showed head-only (-0.01, 0, 0). Then it went to the
  ship-centred eye point (13.5 m).
- **THE WRITER FOUND.** There is one: RIP `0x2874BC9`, called from camera
  code `0x10734D8` in the `0x107xxxx` camera region. Its stack is
  `0x10734D8 / 0x1067316 / 0x106F2F1 / 0x7EB63D / 0x106C9B9 / 0x1E8202 /
  0x1ECA51 / 0x7EB63D / 0x1EB6AF / 0x7F3862 / ...`. This job differs from
  the VR consumer's (`0x1E7906 / 0x1EE0F1`).
- **The order of calls.** Per frame: the writer writes, then the consumer's
  mode-1 peek, then its mode-2 read-and-reset. Both run on one thread
  (t4532), and the mode histogram is exactly 50/50.
- **THE REAL MECHANISM.** On the event frame the writer SKIPS the frame
  entirely: no write between consumes (`wsince=no`, `writerHits=0`). Both
  calls see the reset value. The next frame's write already carries the NEW
  state: (-14.640, +6.774, +1.296) after 12591, the ship-centred
  (-6.890, -10.898, +4.043) after the tunnel exit 16619.
- **Why one substitute cannot serve.** The objects switch coordinate frames
  at different moments by transition type. At a hyperspace exit they are
  still in the old frame on the skipped frame, so the held base is right
  (confirmed by eye). At the low wake they have already switched, so the
  held base is wrong and only the value the writer would have written is
  right.
- **Next:** static round 7 on the writer (`0x2874BC9`'s function and
  `0x10734D8`'s): what it computes into `ship+0x3330`, and why it skips
  the transition frame. If its computation can run on demand at an
  un-refilled consume, the camera gets the base for whichever frame the
  engine is in.
- **The dump for #2** was overwritten before it was serviced (the call
  ring is 8192 entries, and the dump was written a minute later), so the low
  wake's per-call detail is lost.

## Flight 062910 (2026-09-24 06:29, Steam copy, build 04db82fa)

`advanced.transition_flash_eye_base = alternate`, trap OFF; the build
matched. Sean flew the low wakes, then the high wakes. EVERY transition
flashed, including the exits from high wakes.

- **The fix never acted.** Every acted slot says "not validated yet": the
  refilled mailbox and `ship+0x130` agreed 0 times in 11,084 comparisons.
  Both translations were (0,0,0) at every un-refilled call.
- **The mechanism holds.** In flight (after ~06:30:51) the un-refilled count
  rose by exactly one per event: 7 events (#3-#9, frames 11980, 12846,
  15342, 16455, 19183, 20164 and 20712) for Sean's 7-8 transitions, in
  entry/exit pairs 10-13 s apart. There were none otherwise.
  - Before flight (menu/loader) about half of all calls were un-refilled
    (1,411 of 2,821). That burst is unexplained.
- **Instrument defect 1: the writer watch never armed.** Its flight gate
  waits for the detector's camera validation ("transition flash fix
  ACTIVE"), and with the trap off the detector never validates.
- **Instrument defect 2: no dumps.** The dumps are serviced by the eye-trace
  code, which runs only when `eye_origin_trace` is also on.
- **A reading to test next, not yet proof.** On the 2026-09-12 flash frame
  21467 the objects had NOT moved from the frame before (65 pairs, pool step
  0.000). The detector's verdict on every reset is "without a matching
  object rebase". Both say the objects are still in the OLD frame on the
  switch frame. If so, the base the mailbox held one frame earlier is the
  right base for that frame, and only the camera switches early. The
  earlier ruling against holding the last good pose rested on pool
  statistics (f16451's 2 m residual, 16450's 1600 m object step) that are
  noisy across a transition, and it was made against the refuted
  compose-chain parent pose, not this mailbox.

## Flight 045636 (2026-09-24 04:56, Steam copy, build 014edc20)

`advanced.eye_origin_readers = on`, trap on; the build matched.

- **No watchpoint.** The watch waited for flight (04:58:06), then declined
  to arm: the buffer the game passes to `WaitGetPoses` (`0xD3084FD7EC`) is
  on the calling thread's own stack.
- **The pose-fetch stack.** It is in the runtime log
  (`edvr_openxr_20260924_045637_834_4220.log`), innermost first:
  `0x4E3718 / 0x4E2651 / 0x8D2588 / 0x283D548 / 0x2843324 / 0x2868C90 /
  0x1E7906 / 0x1EE0F1 / 0x7EB63D / ...`.
  - `0x4E3718` sits beside round 1's OpenVR setup (`0x4E4870`, `0x4E5900`):
    the VR device class.
  - The tail from `0x1E7906` is the same job framework as the eye-origin
    write stack, whose frame there is `0x2869073` (here `0x2868C90`).
- **The positioner never ran.** Tick `0x107C760` and swap-sync `0x1090420`
  logged 0 calls on every dumped frame.
- **Four eye resets.** f12358 (median object error 6.6 m), f13498
  (1,075 m), f16711 (13.515 m) and f17854 (820 m).
  - f16711 is a high wake. The eye goes from (+10.10, +8.97, -0.02)
    (13.5 m, the eye point on other axes) to head only, then to
    (-0.01, +3.51, +1613.24), the same tunnel placement as on 2026-09-12.
  - The objects did not follow the camera's 13.5 m jump.
- **The Steam ini.** The flight line is removed. EDVR's own writes during
  the flight stay: `ui_quality = 125` (the new values) and
  `openxr_resolution`.

## Static round 6: the eye's base is a consume-and-reset mailbox

This comes from the `WaitGetPoses` anchor of flight 045636. Dumps are in
`analysis\decomp\flash\r6\`; the key file is `follow_28431D0.txt`.

- **One job does both.** The per-frame VR job `FUN_142868c30`
  (`0x2868C30..0x286938A`) calls the camera driver `FUN_1428431d0`, then
  `FUN_140594b60`, which uploads `cb1[275]`. The pose-fetch and eye-write
  stacks meet in this one function, confirmed by the exact call
  instructions.
- **The chain.** The camera driver `0x28431D0` calls the eye/base composer
  `0x283D4C0` (base matrix x eye pose). That calls a per-eye wrapper
  `0x8D2510`, then `VRDevice::GetEyePose` `0x4E25A0` (eye-to-head x head
  pose), then `0x4E3690`, which calls `WaitGetPoses` into its own stack
  buffer.
- **The mailbox.** Let `ship = *(driver+0x50)`. The driver copies
  `ship+0x3330..+0x336F` (a 4x4) into a local. If mode != 1 (the job passes
  2 every frame) it resets that field to the constant at `0x1450C8090`,
  checked from the exe's bytes to be an exact identity, and zeroes its
  translation (`+0x3360/+0x3368`). It then composes the eye views from the
  LOCAL copy (line 170). Those outputs are copied into both eye view
  objects (`FUN_14059f1a0(eyeView+0x20, ...)`).
- **The second composition.** A second call composes from `ship+0x130`
  (never reset, mode 3) into separate buffers.
- **Reading.** The mailbox must be refilled every frame by a writer
  (not yet found). On the switch frame it is not refilled before the
  driver reads it, so the eye composes against identity: head only. That
  matches every measured bad frame, including the 13.5 m eye point
  missing.
- **Not yet fixable blind.** On a frame switch the last good base is in
  the OLD render frame, so holding it would repeat the ruled-out
  last-good-pose error. The fix needs either the writer's ordering, or a
  fallback base (for example `ship+0x130`) proven equal to the mailbox on
  ordinary frames.

## Static rounds 4-5 (after 195435; leads, not proof)

Dumps are in `analysis\decomp\flash\r4\` and `\r5\`. Ghidra holds phantom
functions at earlier rounds' return addresses, so resolve true entries
through `pdata_functions.csv`.

- **The upload path.** The stack #0 frames `0x5015C4..` are the generic
  `f3dxEffect::ConstantBuffer` commit. `0x4C82BE0` is the per-item submission
  loop and `0x4C813A0` a dedup cache. It runs as a scheduled job. A stack
  taken at Unmap names the commit, not the code that set the value.
- **`gCameraPos`.** It and `gPreviousCameraPos` are resolved BY NAME, once
  per effect instance, at `0x57B99E` (`FUN_1404fc6a0(effect,0,name,1)`,
  handles cached in the effect object). The per-frame setter was NOT found.
- **The positioner swap.** A per-frame Tick at `0x107C760` checks the
  `+0x450` positioner against the `+0x2A0` cache. On a change it calls
  `0x1090420`, which zeroes `+0x2A8/+0x2B0` and recomputes an offset block
  only if `info[+0x61]`. The Tick re-seeds (`0x1093650`) only if
  `info[+0x62]`. INFERENCE: a skipped re-seed on the swap frame. There is no
  static tie to `gCameraPos`, and nothing yet shows this positioner is the
  cockpit VR camera's.

## Ruled out

Each was ruled out on 2026-09-23 from existing flight data and static
analysis, or from flight 184826 or 195435 where marked:

- **Ruled out (091726): the writer's name gate as the skip, and the
  offered-and-refused matrix as the low-wake answer.** On every event frame
  the controller made no call at all (`controller_calls=0`), so no matrix
  was ever offered.
- **WITHDRAWN (100043 contradicts it): "ruled out (091726): a low-wake
  flash as a one-frame skip".** The low-wake entry and exit flashes ARE
  one-frame skips. Six scene-judged resets in 100043 each sit on a
  single-frame un-refilled consume (render offset +2). The long 30-70 s
  identity stretches are something else: none of their edges produced a
  reset. The 091726 reading mistook one of those stretches for
  supercruise.
- **Ruled out (073114): one fixed substitute for every transition.** The
  held base fixed both hyperspace exits (Sean: "hyperspace worked") but not
  the low wake. The objects change frame on different frames by
  transition type.
- **Ruled out (073114): waiting for the writer inside the frame.** The
  writer skips the transition frame entirely (no write between consumes),
  so there is no late write to wait for.
- **Ruled out (062910): `ship+0x130` as a stand-in for the eye-base
  mailbox.** (Dump 4 of 073114 shows why: it is the ship's placement
  without the eye point, 13.5 m and a rotation away from the mailbox.) On refilled frames it agreed 0 times in 11,084 comparisons, and
  its translation is (0,0,0) on the flash frames too.
- **Ruled out (045636): round 5's camera positioner (Tick `0x107C760`,
  swap-sync `0x1090420`) as the cockpit camera's.** Neither ran on any
  dumped frame in flight, across four eye resets.
- **Ruled out (045636): a hardware watch on the returned head pose.** The
  game's `WaitGetPoses` buffer is on the caller's own stack.
- **Ruled out (195435): a different code path writing the eye origin on the
  bad frame.** Both reset frames (f12604, f13072) were written by stack #0,
  like every ordinary frame.

- **Ruled out (184826): the compose `0x23BC8A0` as the eye camera's
  compose.** It pushes about 5 system-scale targets a frame, ~1e8 m apart,
  and the eye origin came within 1 m of none of the last 4 pushes in 1,844
  of 1,844 frames. Round 1's evidence was an offset coincidence (`+0x1130`)
  and a compose-and-push shape.
- **Ruled out (184826): `0x3CEE650` as a stand-in for the cached parent world
  block.** It disagreed on 12,906 of 12,906 compose-site decodes: at f10385
  the cached world was at ~9e9 m and the recompute at ~1e12 m, with dr 1.78.

- **Ruled out: objects lag the camera by a frame (stale objects).** On the
  frame after, objects and camera move together to 2.0 m median in both high
  wakes. A lag would show 13.5 m to 4.2 km. Source:
  `edvr_gfx_20260912_173400.log`, f16451 and f21468.
- **Ruled out: the camera returns to its pre-transition path.** It never does.
  f16451 and f21468 land on the same new placement.
- **Ruled out: holding the last good parent pose as a fix.** Frame N's layout
  agrees with N+1's to about 2 m, while N-1's is 13.5 m to 4.2 km away. The old
  pose belongs to the old frame, so holding it would be worse than the bug.
- **Ruled out: the parent is a KinematicRig gated on `rig+0x380 == 4`.** The
  parent's container reads `+0x380` as a 64-bit dirty-bucket count, and the rig
  has no `+0x378`/`+0x384` pair.
- **Ruled out: the `+0x1130` push is `cb1` row 275 itself.** It stores doubles
  and `cb1` holds floats; at most it is the source (H3).
- **Ruled out: the classification captures as the `cb1` writer anchor.** That
  probe records only pool and index buffers
  (`object_classification_probe.h:156`, `:380-386`).
