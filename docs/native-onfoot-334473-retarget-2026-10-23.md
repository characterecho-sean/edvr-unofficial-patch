# Native on-foot VR: retarget from Elite build 332841 to 334473 (2026-10-23)

## Status

- **State: BLOCKED. No address was changed.** The 334473 executable is not on this machine, so
  nothing the map does not already cover can be placed or verified. Every
  `EliteDangerous64.exe` here is build 332841 (stamp `0x6A989634`, image size `0x6409000`,
  SHA-256 `e6be8bbe...4e988`): Epic, Steam, Frontier and `analysis\` in the main checkout. Steam's
  `appmanifest_359320` still reads buildid 25433925 (2026-09-21). Main's retarget was made on the
  Steam Deck, where `analysis\EliteDangerous64-334473.exe` and the Ghidra project live; the repo
  carries only the RVA map for main's own addresses (`.agents\build-334473\`).
- **Target identity** (`ecm::kExpectedTimestamp` / `kExpectedImageSize`, as main set them): stamp
  `0x6ACD513D`, image size `0x640C000`, SHA-256 `6891f70a021fa9f3...47857e93`, `VersionInfo.txt`
  2026.10.13.334473. No file on this PC has it.
- **Unblock:** let the Epic launcher update Elite on this PC (a user action; the agent never
  writes a game folder), or copy the Deck's `EliteDangerous64-334473.exe` to
  `analysis\EliteDangerous64-334473.exe`. Check the three identity values above, then follow "To
  finish".
- **Inventory (done):** 99 build-bound RVA literals on the branch's on-foot paths (list:
  `docs\native-onfoot-334473-rvas.txt`, 81 code, 7 `.rdata`, 11 `.data`), 44 byte pins, 5 vtable
  slots, and the struct-offset families below. 4 RVAs are in main's map (`0xFDDD10`, `0xFDE0D0`,
  `0x19B1240`, and `0x3DD61AF` as a comment); 1 more sits in a unit main mapped (`0x3DA2242`).
  The other 94 need the new exe. `0x434FE49` (record-writer probe, `object_classification_test`)
  already holds main's 334473 value.
- **Verified on 332841 (the old side, offline):** all 44 pins reproduce byte for byte at their
  RVAs; 5 vtable slots hold the functions the code expects; the static data matches what the code
  checks (gate byte 1, HUD override -1, layout scale 1.0f, margins 0.027 and 0.06).
- **Pins that must be recomputed on retarget (4):** `0x1AAF120` (`E8` rel32 to `0x3174210`),
  `0x1A15640` (`E8` rel32 to `0x434ECC0`), `0x1A14970` and `0x1A14320` (stack-cookie load, rip
  disp32 at +0x13). The other 40 hold no rel32 or rip operand outside their own function and
  should KEEP.
- **The branch as merged is inert on every build.** On 332841 the shared identity pair refuses it;
  on 334473 the setter install refuses on its vtable-slot compare, and every game-data write sits
  behind that install (or native on-foot being active) and a check of the game's own current
  value. Residual risk: a short prologue pin can match a different function by chance (below).
- **Next flight:** none until the retarget lands. Not installed, not flown.

## Executables on this machine

| File | Stamp | Image size | SHA-256 | Build |
|---|---|---|---|---|
| `C:\Program Files\Epic Games\EliteDangerous\Products\elite-dangerous-odyssey-64\` | `0x6A989634` | `0x6409000` | `e6be8bbe...` | 332841 |
| `C:\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64\` | same | same | same | 332841 |
| `C:\Users\seanm\AppData\Local\Frontier_Developments\Products\elite-dangerous-odyssey-64\` | same | same | same | 332841 |
| `C:\Users\seanm\projects\edvr-unofficial-patch\analysis\EliteDangerous64.exe` | same | same | same | 332841 |

A search of C: and D: for any file over 50 MB named `EliteDangerous64*` found only these and one
copy of the Epic file in a codex worktree. No cached function map exists
(`analysis\build_map_332841_to_334473.cache.json` is on the Deck).

## What the branch binds

Method: the added lines of `git diff -U0 origin/main...HEAD` (90 files) scanned for hex literals
from `0x1000` to the image size and `FUN_`/`DAT_` names, then classified against main's map
(`map_all.json`, `pins_report.txt` and `comment_map_report.txt`: 291 old to new pairs), the old
exe's `.pdata`, and capstone. Per file, distinct values: `native_on_foot.cpp` 86,
`native_on_foot_core.h` 26, `explorer_cam.cpp` 5, `native_on_foot.h` 2, `explorer_cam_core.h` 1.
Tool rigs carry only synthetic values, except `native_frame_test.cpp:4707` (`0x1AAF120`, stub
arithmetic) and `object_classification_test.cpp` (`0x369FB91`, `0x434FE49`: main's new values).

| Group | Old RVAs | Guard when the RVA is stale | Main's map |
|---|---|---|---|
| Setter (F0) | code `0x28723C0`; vtable `0x52E9608` slot +0x28; data `0x5E82B3E` gate byte, `0x5E82B48` HUD override; comments `0x28873C0`, `0x2887A15` | slot compare plus 27-byte prologue; data writes need the install armed and the byte to read 1 / -1 | none |
| HUD patches | `0x19B1587` (2 B), `0x1A36453` (1 B), `0x1A149A6` (6 B, patched byte `0x1A149A8`); comments `0x19B14E0`, `0x19B15FC`, `0x1A8D190` | bytes checked before any write | none |
| HUD probes | `0x4714FE0`, `0x1A14970`, `0x434D320`, `0x1A14320`, `0x1A14D80`, `0x1A0DCD0` | prologue pins, 19 to 32 B | none |
| Weapon UI | `0x1AAF120` (25 B), `0x1AAF129`, `0x3174210`; callers `0x1A1485D`, `0x1A148CD`; vtables `0x51F77D0`, `0x51F8160`, `0x51F9BD8`; sink slot `0x1A439F0` | pin holds the `E8` rel32 to `0x3174210`; `0x3174210` has no pin of its own, so it must move with the site; vtables and callers are compared only | none |
| Reticle | `0x1A15640` (9 B), `0x434ECC0`; comments `0x1A15644`, `0x1A15645`, `0x1A15648` | pin holds the `E8` rel32 | none |
| Look to aim | `0x19973D0` (32 B), `0x1997C51` (31 B, interior), `0x1A9C370` (5 B) | pins; the 5 B pin is weak alone | none |
| Load timers | `0x260D6B0` (10 B), `0x1CA6510` (16 B), table `0x5E7B460` (21 x 0x30); vtable `0x5298390` (comment) | pins; the table read is SEH-guarded and range-checked | none |
| Connection retry (`fix.connection_retry`) | `0x831A10`, `0x1D15300`, `0x1D818D0`, `0x1D4A710`, `0x1D0FBA0`, `0x1CFBBE0`, `0x1D7C8E0`, `0x653C50`, `0x1D1CDF0`, `0x1D1D220`, `0x4C8E020`, `0x4C90C40`, `0x4C8FAF0`, `0x4C8EEA0`, `0x4C8E500`, `0x4C907A0`, `0x4C8F960`, `0x4C8EA60`, `0x4C90B90`; data `0x5F7D7F8` (the key SetInsert receives); comment `0x1D1D1E2` | pins of 6 to 20 B; `0x5F7D7F8` is unchecked and must move with the callees | none |
| Body shift, records | `0x3DA1DE0`, `0x3DA2C80`, `0x43ED820`, `0x4C837B0`, `0x741520` (vtable `0x510E370`, comment); caller names `0x3DA2242`, `0x43EDC24`, `0x434D8D5`, `0x23C72E9`/`0x23C7100`, `0x31EC020`/`0x31EBE90`, `0x372F491`/`0x372EF80`, `0x3C9BF53`/`0x3C9B760`, `0x3E75F6D`/`0x3E75DA0`; comments `0x43B796A`, `0x43FB740`, `0x3DD9EA0` | pins; the name tables only label callers | `0x3DD61AF` -> `0x3DD8E6F` (comment); `0x3DA2242` shares a RELOC-ONLY unit with main's `0x3DA20DE` -> `0x3DA4D9E` (hint `0x3DA4F02`, not main's) |
| Body mode | `0x1A112B0` (22 B), vtable `0x5200F90` slot +0x30, `0x19B1240` (32 B) | slot compare plus two pins | `0x19B1240` -> `0x19B4000` (IDENTICAL) |
| HUD layout data | `0x5E7A754`, `0x5E7A758` (margins), `0x5E7A760` + `0x5F741CA` (layout scale), `0x5F741B7`, `0x5F741B8` (visor), `0x5E7A7B4` (read only); comments `0x1AB32F0`, `0x1AB3351`, `0x1AB385B` | each write needs the game's own current value (0.027 / 0.06, 0 and 1.0f, 0 and 0) and native on-foot active | none |
| Pose freeze (`explorer_cam.cpp`) | `0xFDE0D0` (11 B, called directly), comments `0xFDDD10`, `0x43FB740`, `0x3DD9EA0` | pin | `0xFDE0D0` -> `0xFDF1D0`, `0xFDDD10` -> `0xFDEE10` (RELOC-ONLY) |

Not an RVA: `0x2939677` in `native_on_foot.cpp:3870` is the reticle call's rel32 displacement
(`0x434ECC0 - (0x1A15644 + 5)`); it changes with the call and callee. Same for the `E8` bytes
inside the two `E8` pins.

The delta between builds is not constant across the branch's code: main's points read +0x2DC0 at
`0x19B1240`, +0x2640 at `0x2814686`, +0x2630 at `0x28767D0`, +0x2D00 at `0x36819D0`, +0x2CC0 at
`0x3CB93F0`. No point of main's lies between `0x19B1359` and `0x2814686`, where most of the
on-foot hooks sit, so interpolation cannot place them (the bracket deltas differ).

## Struct offsets

Not enumerated one by one. Families: selector `+0x20 +0x58 +0x60 +0x2B4`; settings chain `+0x1180`
then `+0x18 +0x16C +0x3694 +0x2CB8 +0x90 +0x98 +0x2E0`; HRC `+0x1D0 +0x291 +0x418 +0x430 +0x470
+0x480 +0x4C0`; view `+0x20 +0x50 +0x8E8`; AMC `+0x11B0 +0x11C0 +0x1260 +0x17A0 +0x2E0 +0x323`
(stride `0x680`); MGMC `+0x1C8 +0x230 +0x240 +0x354 +0x380`; look component `+0x5F4 +0x5F8 +0x5B0
+0x7D8 +0x7DC`, action `+0x190 +0x198`. Main found no struct offset moved in the 87 functions that
hold its addresses (all RELOC-ONLY or IDENTICAL). None of the on-foot functions has been checked
against 334473, so each offset is presumed unchanged and unproven. A function the map reports
CHANGED, or one whose masked bytes differ outside a displacement, needs a disassembly read before
its offsets are trusted.

## Text-only mentions of 332841

`native_on_foot.cpp` (10 lines, mostly refusal and log text), `native_on_foot_core.h` (4),
`native_on_foot.h` (1), `explorer_cam.cpp` (1), and `edvr.ini:711` ("Only Elite build 332841 is
supported"). Update them with the addresses, not before.

## Why the tree is inert now, and what is left

- On 332841 `nofoot::identityMatches` uses `ecm`'s pair, which is now 334473's, so the install
  entry points (setter, probes, look, body shift, records, pose freeze, body mode) refuse.
- On 334473 the setter install reads `[base + 0x52E9608 + 0x28]` and compares it with
  `base + 0x28723C0`; a stale pair fails that and the install refuses. `gateByteTick`,
  `hudOverrideTick` and `visorTick` return early unless the install is armed; the margin and
  layout-scale writers need `nativeOnFootActive()` and the game's own current values.
- Independent hooks (look, load timers, connection retry, body shift, records, body mode, probes)
  each verify a prologue before patching. With a stale RVA a pin can match another function by
  chance. Counted against the 332841 text (the chance that one random 16-aligned address matches
  each pin): 0.0159 summed over the 37 prologue pins, led by `kSetInitB` (6 B, 0.35%), `kDtorB` (0.29%), `kSetInsertB` and
  `kInstantiateB` (8 B, 0.25% each), `kStopB` and `kDeactB` (9 B, 0.12% each). All of those belong
  to the connection-retry hooks. The risk is small and not zero. If the branch must run on 334473
  before the retarget, bind the on-foot identity to a pair of its own (332841's) so it refuses;
  that is a design change and was not made here.

## To finish (when the exe is on this PC)

1. Copy it to `analysis\EliteDangerous64-334473.exe` and confirm stamp `0x6ACD513D`, size
   `0x640C000`, SHA-256 `6891f70a...47857e93`.
2. `python tools\build_diff.py map --old analysis\EliteDangerous64.exe --new
   analysis\EliteDangerous64-334473.exe --cache build\onfoot_map.cache.json --rvas-file
   docs\native-onfoot-334473-rvas.txt --json build\onfoot_map.json` (run without `-I`: the tool
   imports `openxr_pe` from its own directory). The same call on the old exe against itself
   returned IDENTICAL for all 81 code RVAs and XREF-BRACKET (delta 0) for the 18 data RVAs on
   2026-10-23, so the file and the command work.
3. Accept IDENTICAL, RELOC-ONLY, XREF-EXACT. A vtable must show its slots confirmed at the new
   address. Treat CHANGED, XREF-BRACKET on a single-use data byte, and anything unplaced as
   unmapped: leave that hook refusing and list it.
4. Rewrite the literals with a mapping table in the commit body (`.agents\build-334473\retarget.py`
   is the pattern; it hard-codes the Deck's paths and a 211-entry assert). Recompute the 4 pins
   from the new bytes; confirm the other 40 are byte-equal at the new addresses. Keep
   `0x3174210`, `0x434ECC0` and `0x5F7D7F8` in step with their sites. Leave the identity pair.
5. `build.bat --wait-lock` by absolute path; `native_frame_test` should still report 460 checks.
