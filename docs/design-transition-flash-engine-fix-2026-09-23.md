# Transition flash: preventing it inside the engine

*Design, 2026-09-23. Static analysis only: three Ghidra rounds against the
installed build and a re-read of existing flight logs. Nothing here is built or
flown.*

## Status

- **State:** Phases 1 and 2 BUILT TOGETHER, NOT FLOWN (Sean chose one flight
  for both). `advanced.transition_flash_prevent = off|watch|on|alternate`,
  default off: 0290e101, with five review fixes in 980a0c84 (per-call parent
  guard, H3 aggregated per frame, the cap counts frames, dumps widen instead
  of dropping, a 65536-entry ring dumped to `edvr_logs\flash\`). On main via
  31330089.
- **Installed (2026-09-23 17:58):** build 980a0c84 in the STEAM copy only;
  the Frontier copy keeps the engine-motion session's f05c84bf. The Steam
  `edvr.ini` has `transition_flash = 0` (the trap off, so the A/B is visible)
  and `[advanced] transition_flash_prevent = alternate` (events alternate
  watched, acted, starting watched). Both lines are marked in the ini: after
  the flight, put back `transition_flash = 1` and remove the prevent line.
  Backup:
  `scratchpad\steam-edvr.ini.pre-flash-flight.bak` (this session).
- **Goal (Sean, 2026-09-23):** stop trapping the bad frame (detecting it, then
  resubmitting the previous one) and stop Elite rendering it at all, by fixing
  the order inside the game.
- **What the bad frame is (measured):** the eye origin the vertex shaders
  subtract, `cb1[275]`, lands on the head pose alone: (-0.02, 0, -0.02) at
  f16450, and five more captures on native builds, all within 14 cm of the
  frame origin. The ship/seat transform under the head is missing for one
  frame. Withholding that frame cures the visible flash (17:58 flight,
  2026-09-12). The frame after, in two different high wakes, is the same
  canonical placement (-0.02, +3.51, +1613.23).
- **Leading hypothesis (H1):** the camera's parent node has a world block the
  engine's world pass has not written yet, and the compose reads it with no
  check: an identity parent, so the eye is the head alone.
- **Proposed fix:** at the camera compose, compare the parent's cached world
  block with the engine's own from-root recompute (`0x3CEE650`). When they
  disagree, use the recompute. The frame renders from the right place and
  nothing is withheld.
- **Next flight:** on the Steam copy, the Phase 1 list below. Note which
  jumps flashed; press Pause within a couple of seconds of any flash that
  shows (it dumps the whole ring). First read:
  `python tools\edvr_log.py --target steam --expect-build 980a0c84`, then
  the `transition flash prevent:` lines and `edvr_logs\flash\`.
- **Pass:** the watched events show the detector's CameraReset and a
  visible flash; the acted events show neither. The recompute agrees with
  the cached block on ordinary frames (`validated=yes`). H3 frames land in
  the `<1cm`/`<10cm` buckets.
- **Environment:** game build 332841 (PE TimeDateStamp 1788384820, image
  104,894,464), the same exe in both installs. Independent of VR runtime,
  headset, eye size and DLSS: the code is Elite's camera, below all of them. It
  rides `d3d11.dll`, which loads on every path.
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

## Ruled out

Each was ruled out on 2026-09-23 from existing flight data and static
analysis:

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
