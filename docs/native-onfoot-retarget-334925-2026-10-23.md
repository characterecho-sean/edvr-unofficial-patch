# Native on-foot VR: retarget from Elite 332841 to 334925 (2026-10-23)

## Status

- **State: all 99 build-bound addresses placed and verified offline; branch retargeted to 334925;
  full build green (`native_frame_test` 460 checks); not installed, not flown.** The path was
  332841 -> 334473 -> 334925: Elite updated twice in four days, and 334925's layout differs from
  334473's (text moves by -0x60, -0xA0, -0xC0 or -0x120; vtables by -0x30; the on-foot data globals
  sit at the same RVAs).
  Every table row is in `docs\native-onfoot-334925-address-table.txt`.
- **The identity pair was not touched.** `ecm::kExpectedTimestamp` and `kExpectedImageSize` still
  hold 334473's values here; main's own retarget moves them to 334925 and arrives by merge. Until
  then native on-foot refuses on every exe (identity mismatch on 334925; on 334473 the setter's
  slot compare fails). Merge main's retarget before any flight.
- **Pins:** 44 byte pins checked against the 334925 exe at their new RVAs; 40 were byte-equal and 4
  were rewritten (below). All 44 now match, taken from the edited source's own constants.
- **vtable slots:** 5 slots hold the expected functions in 334473 and 334925 (setter slot +0x28,
  `HumanoidModelManager` slot +0x30, load pipeline slot +0x18, frAnim slot +0x8, sink slot +0x1B0).
- **Struct offsets:** one real shift, the look processor's component (+0x30 for everything at or
  past +0x4A8); applied. HRC, pose-evaluator and AMC offsets read unchanged. See "Struct offsets".
- **Needs a flight, not provable offline:** the look processor gained a conditional extra term in
  334473 (a call, then a test on a sub-object at `comp+0x4B8`, then an added term; see below).
  The look-to-aim gain and sign lines (`F22`, `F24`) are the check.
- **Left as it was:** nothing unmapped, and no hook is left refusing for want of an address.
- **Not done here:** the identity pair, main's own addresses, install, flight.

## Executables

| Build | File | Stamp | Image size | SHA-256 |
|---|---|---|---|---|
| 332841 | Epic, Steam (before), `analysis\EliteDangerous64.exe` | `0x6A989634` | `0x6409000` | `e6be8bbe...4e988` |
| 334473 | `analysis\EliteDangerous64-334473.exe` (on no install now) | `0x6ACD513D` | `0x640C000` | `6891f70a...857e93` |
| 334925 | Steam and Frontier; `analysis\EliteDangerous64-334925.exe` | `0x6ADA8048` | `0x640C000` | `ED5A8F8C...264B63` |

334925 is `VersionInfo.txt` 2026.10.23.334925. The copy in `analysis\` is read from the Steam
install; no game folder was written.

## Method

1. `tools\build_diff.py map` (no `-I`) on `docs\native-onfoot-332841-rvas.txt`, once per target
   build, plus 334473 -> 334925 on the 334473 values. The per-function CSV and cache come from
   Sean's copy in `analysis\`.
2. Tool result for 332841 -> 334473 and for 332841 -> 334925, which agree row for row: 23 IDENTICAL,
   42 RELOC-ONLY, 10 XREF-EXACT; 15 CHANGED, 6 AMBIGUOUS, 2 XREF-BRACKET and 1 NOT-FOUND were not
   placed. 334473 -> 334925 places all 99 (56 RELOC-ONLY, 25 IDENTICAL, 18 XREF-EXACT), and its
   result equals the direct 332841 -> 334925 table in every row.
3. **The 24 the tool left open** (RVAs in this step and in "Struct offsets" are 332841's):
   - Six functions are CHANGED only because a displacement lands on changed data. With rip and
     rel operands masked, their instruction streams are identical (`0x19973D0` 1128 instructions,
     `0x1A14D80` 612, `0x1A36400` 109, `0x1AB32F0` 53, `0x1D1CDF0` 248, `0x28873C0` 1426); every
     call and jump target matches the function map. Interior addresses keep their offset in the
     function (`0x1997C51`, `0x1A15640` to `0x1A15648`, `0x1A36453`, `0x1AB3351`, `0x1D1D1E2`,
     `0x2887A15`); `0x19B15FC` is `0x19B14E0` + 0x11C, confirmed by the `76 73` branch at the new
     patch site landing on it.
   - `0x1A9C370` (look processor) really changed; its entry and 5-byte pin match, so the hook
     address stands. See below.
   - Data came from following the code that references it, instruction for instruction in the
     mapped function: the requester `0x28873C0` holds `cmp byte [rip+X], r13b` at +0x739 (gate
     byte) and `mov eax, [rip+Y]` at +0x655 (HUD override), the layout routine and its callers
     hold the margin, scale and visor flags, 82 of 87 holster-id references agree, and the
     connection-retry functions hold the type key (8 of 8). Checked contents: margins 0.027 and
     0.06, layout scale 1.0f, gate 1, override -1, and the 21 stage names equal 332841's.
4. Pins: arrays and RVA constants read from the edited source and compared with the 334925 exe
   (`42` through their named constants, the other `2` through the table).

## The 4 rewritten pins (334925 bytes)

| Pin | Old | New | Why |
|---|---|---|---|
| `kWeaponUiBranchBefore` / `After` (bytes 1-2) | `EB 50` | `6B 55` | `E8` rel32 to the getter `0x3176ED0` |
| `kReticleExpect` (bytes 5-6) | `77 96` | `37 9D` | `E8` rel32 to `0x4351920` |
| `kHrcSubmitPrologue` (bytes 22-23) | `DE 1F` | `8E 2A` | stack-cookie load, rip disp32 |
| `kHumanoidArRenderPrologue` (bytes 22-23) | `2E 26` | `DE 30` | stack-cookie load, rip disp32 |

Both cookie loads resolve to one global (`0x5EA99B8`). The stolen bytes of every hook (the first 5
to 8) hold no rip operand, so each relay and trampoline is as before. The reticle rel32 stays in
one cache line (`0x1A17BE5` to `0x1A17BE8`).

## Struct offsets

- **Look component (`rdi` in `0x1A9C370`): +0x30 from +0x4A8 up.** The one qword store of the
  yaw/pitch pair is `movsd [rdi+0x5F4], xmm0` in 332841 and `movsd [rdi+0x624], xmm0` in both new
  builds; the aligned processor shows every field in between at +0x30. Applied:
  yaw delta `0x5F4` -> `0x624`, pitch delta `0x5F8` -> `0x628`, basis `0x5B0` -> `0x5E0`, gains
  `0x7D8` / `0x7DC` -> `0x808` / `0x80C`.
- **The new term.** In the processor after the pitch input is scaled, 334473 and 334925 add: a call
  to a tiny test (`[rcx]` object's `[+0xC0] >= 5`), a null test on `comp+0x4B8`'s sub-object, a
  virtual call (`+0x20`) on it, and when all pass an added term into the same value. Offsets are
  unaffected; the gain the hook measures may be.
- **Unchanged, by class scan** (every aligned instruction of every function touching the class):
  HRC `+0x418 +0x420 +0x428 +0x430 +0x470 +0x480 +0x4C0` (0 shifted); pose evaluator
  `+0x128 +0x12C +0x1F0 +0x210 +0x212 +0xA0 +0xE0`; AMC `+0x1B0 +0x208 +0x230 +0x238 +0x2E0 +0x321
  to +0x323 +0x378`.
- **Unchanged because the accessing function is stream-identical:** 76 of the 148 offset-like
  literals appear in the 63 proven functions (selector, requester, setter, HRC submit and layout,
  AMC and MGMC jobs, Animate, record writer, SetMode and the pov handler).
- **Not proven, but moved nowhere in the census of changed functions:** `+0x124`, `+0x212`,
  `+0x354`, `+0x1260`, `+0x17A0` (AMC record arrays, from the submit job's index arithmetic),
  `+0x3694` (settings mode, read only).

## Addresses by feature (334925 RVAs; the full table has 332841 and 334473)

| Group | RVAs |
|---|---|
| Setter | setter `0x2874920`; vtable `0x52EC798` slot +0x28; gate byte `0x5E85B8A`; HUD override `0x5E85B94`; comments `0x2889940`, `0x2889F95` |
| HUD patches | `0x19B0D77`, `0x1A38AD3`, `0x1A16F46` (patched byte `0x1A16F48`); comments `0x19B0CD0`, `0x19B0DEC`, `0x1A8F950` |
| HUD probes | `0x4717A80`, `0x1A16F10`, `0x434FF80`, `0x1A168C0`, `0x1A17320`, `0x1A10210` |
| Weapon UI | `0x1AB1960`, `0x1AB1969`, getter `0x3176ED0`; callers `0x1A16DFD`, `0x1A16E6D`; vtables `0x51FA970`, `0x51FB300`, `0x51FCD78`; sink slot `0x1A46170` |
| Reticle | `0x1A17BE0`, callee `0x4351920`; comments `0x1A17BE4`, `0x1A17BE5`, `0x1A17BE8` |
| Look to aim | `0x1999190`, `0x1999A11`, `0x1A9EB50` |
| Load timers | `0x260FC30`, `0x1CA8DD0`, table `0x5E7E480`; vtable `0x529B530` (comment) |
| Connection retry | `0x831B10`, `0x1D17B60`, `0x1D84130`, `0x1D4CF70`, `0x1D12400`, `0x1CFE440`, `0x1D7F140`, `0x653D50`, `0x1D1F650`, `0x1D1FA80`, `0x4C90AE0`, `0x4C93700`, `0x4C925B0`, `0x4C91960`, `0x4C90FC0`, `0x4C93260`, `0x4C92420`, `0x4C91520`, `0x4C93650`; key `0x5F80938`; comment `0x1D1FA42` |
| Body shift, records | `0x3DA4A00`, `0x3DA58A0`, `0x43F0500`, `0x4C86270`, `0x741620` (vtable `0x51113B0`); caller names `0x3DA4E62`, `0x43F0904`, `0x4350535`, `0x23C9649`/`0x23C9460`, `0x31EECE0`/`0x31EEB50`, `0x37320F1`/`0x3731BE0`, `0x3C9EB73`/`0x3C9E380`, `0x3E78B8D`/`0x3E789C0`; comments `0x43BA64A`, `0x43FE420`, `0x3DDCAC0`, `0x3DD8DCF` |
| Body mode | `0x1A13820`, vtable `0x5204168` slot +0x30, `0x19B3F40` |
| HUD layout data | margins `0x5E7D5D8`, `0x5E7D5DC`; scale `0x5E7D5E4` and mode `0x5F77202`; visor `0x5F772E2`, `0x5F772E3`; holster `0x5E7D7D4`; comments `0x1AB5B30`, `0x1AB5B91`, `0x1AB609B` |
| Pose freeze | `0xFDF170`; comments `0xFDEDB0`, `0x43FE420`, `0x3DDCAC0` |

## Text changed with the addresses

Runtime refusal and log text now says build 334925 (`native_on_foot.cpp`, `explorer_cam.cpp`,
`native_on_foot.h`, `native_on_foot_core.h` comments), as does the `edvr.ini` sentence "Only Elite
build 334925 is supported". The identity message names 334925 and becomes true when main's
retarget merges. The comment `0x02939677` (the reticle call's rel32) is now `0x02939D37`.

## Residual risk

The one thing offline reading cannot give is behaviour: the look processor's new term, and
anything a data-only global does differently in 334925. Short prologue pins can still match a
stale address by chance (0.0159 summed over 37 prologue pins on a random aligned address, led by
`kSetInitB`, `kDtorB`, `kSetInsertB`), but no address is stale now.
