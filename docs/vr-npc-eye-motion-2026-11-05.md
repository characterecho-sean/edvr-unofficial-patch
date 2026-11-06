# NPC eyes under motion in VR (2026-11-05)

## Status

- **State: flown by eye, not measured, merged to main 2026-11-05.** Branch `claude/npc-eye-motion`,
  from `07738546` (v0.22.0-rc.7), flown on `ddb725c0` with the legacy glass fix (Frontier,
  VR on foot, log `edvr_gfx_20261105_165800`). Sean: "eye fix seems to work in VR". The
  log has the family live with its pixel shader patched, 9,070 draws in one 30 s window.
  No eye dump was taken, so the eyes' E, their path and the face's error after the fix
  are unmeasured. The corpus exactness test (`K9`) did not finish: 33 minutes, stopped,
  on a 64x64 target, cause not looked into. That the pair is the eyes' colour is
  supported by Sean's eye and is otherwise still inferred.
- **Report (Sean, 2026-11-05):** "the bar tender/other npcs on the concourse, their
  eyes see to be blurring with motion".
- **Environment:** native on-foot VR, native OpenXR runtime, Frontier, game build
  334925, DLSS, 2016x2017 in, 4032x4034 out. VR only: the second skin never runs in
  the flat profile, and this family is `vrOnly`.
- **Evidence:** eye run `143722980001` (log `edvr_gfx_20261105_143434`, `07738546`).
- **Not a regression:** this pair has never been keyed.
- **Temporary:** no keys.
- **Next flight:** stand in front of an NPC whose head moves, on foot, and take an
  eye dump. Then read it as in "What the flight must show".

## Measured

The depth writer map names each eye as a 624-index draw of the depth prepass
`F516BF02`/`B40B0462`, 96 and 99 input px on this bartender.

| On the map's frame | Eye | Skin round it |
|---|---|---|
| Slot | -1 | 4681 |
| SkinE.w | 0 | 1 |
| Path | world | engine |

Tracked by the face through the run, eye motion against the skin round it, native px
a frame:

| k | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|
| Apart | 7.65 | 5.75 | 5.76 | 6.09 | 3.67 | 2.76 | 2.84 |

It falls as the head slows, and is 0.48 to 1.24 on k 11 to 15. The capture's own hitch
fills k 2 and 3, which is why they are left out. `character_motion_check` on the same
run has the face at a worst of 0.35 native px, so the face is right and the eyes are
what differs.

## The hypothesis

The eyes' colour is the forward pass `33A5025C48FC8259`/`7AA0441EE0E88667`, unkeyed, so
the eyes have no slot and no E. The same class as the hair (`A6A3`/`1AE6`) and the
commander's suit (`01DA`/`2C3A`).

INFERRED, from the ledger of the run. The writer map cannot name it, because the
forward pass does not change the prepass's depth:

- Its 624-index draws repeat the prepass's pattern: each drawn twice, one instance,
  instance starts 262 apart (`F516` 1 and 263, `33A5` 2853 and 3115).
- The other pairs with 624-index draws do not fit: `EB52` and `5B4D` draw 2 or 3
  instances, and `B10B` once.
- Its vertex shader has the inputs and `t33`/`t38` of `D99A`'s.
- Its state is the hair pass's: blending off, depth write on, colour mask 15, target
  format 26.

Its pixel shader reads v0 to v5 and no system value after them, so E takes the vertex
shader's next free register and no `skinRegister` is set.

Its vertex shader declined the second skin at first (`K9.derive`, "anchor not found").
Its pose chain is `D99A`'s line for line and ends in r1, but after the displacement
block `add r3.xyz, r1.xyzx, r2.xzwx` moves the result into the register the matrix
multiplies read, and the search only looked in that register. `dxbc_skin_clone.h` now
follows that one add back to the anchor in r1. Only a shader the first search refused
reaches that step, so no pair already keyed changes.

The pair also draws 432- and 168-index meshes, not identified. They take the key too.

## What the flight must show

`python tools\edvr_log.py --target <install> --expect-build <commit>` first.

- Confirmed: the log has the family's derive line for `vs_33A5025C48FC8259`, and in a
  dump the eyes' pixels have SkinE.w 1 and take the engine path, with the holes in
  the face's engine path gone.
- Refuted: the pair draws and is patched, and the eyes still have SkinE.w 0. Then the
  colour is another draw: write `ruled out` here.
- Also read `character_motion_check` on the dump: the face must stay inside its limit.
- By eye: the eyes hold with the face while an NPC's head moves, and nothing changes
  on teeth, lashes or any part of the face that the unidentified meshes might be.

## Ruled out

- ruled out: a face or skin motion error, because the face measures at most 0.35
  native px on the same frames.
