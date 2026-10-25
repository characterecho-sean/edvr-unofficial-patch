# Game-update signatures (issue 124)

## Status

- **State:** step 3 and the review round (round 2, below) built on a
  branch, full build green, not merged; flown once (VR, Frontier). Step 1 was the offline
  generator (`tools\hook_signatures.py`), step 2 the runtime resolver and the
  store hook pilot, step 3 every other hook on the resolver and a build gate
  (`hook_signatures.py audit`). Round 2: foreign processes, the loader lock,
  field witnesses, the gate's blind spots, the hot path.
- **Rules (the maintainer's):** on the known build (334925) every hook keeps
  its behaviour and its log text byte for byte: `gameRva(k)` returns k and the
  pins compare exactly. On an unknown build a hook comes up, game-memory
  writers included, only when every address it needs is found by a
  whole-function masked match, and (round 2) every struct field it writes,
  or reads to decide a write, has a field witness found; anything missing
  means that hook stands down, and its log says which.
- **Exact-build only, by decision:** the Oculus route profile
  (`elite_oculus_profile.h`, `oculus_route.cpp`), Explorer Cam (another arc
  deletes it), `mem_tick.cpp` (a diagnostic, not in the table) and terrain cull
  cover (its own scan finds its site on an unknown build).
- **Counts, 318 addresses** (the 222 of `.agents\build-334925\map_all.json`,
  nine fragment starts and 87 field witnesses in `signature_extra.txt`): on
  334473 and on 332841 the Python matcher and the C++ resolver both find 318
  of 318, all EQUAL to the build map, 0 WRONG, with the same answer per
  target. RVAs inside a found unit: 267 checked per build against the ground
  truth, 0 WRONG; 606 unit probes, C++ and Python agree on every one;
  gameRva's sorted index answers as the scan does on 2670 RVAs per build.
- **Survey** (step 2, about 3500 sampled units per build): WRONG 0 on both.
- **Time:** the C++ resolve takes 54 to 57 ms in the rig with 318 targets
  (65 to 125 ms under load earlier); not yet measured in the game. On an
  unknown build it runs on `gameBuildStartup`'s thread, never in DllMain.
- **Temporary key:** `advanced.game_update_check`, `off` (default) or `on`,
  hidden. On, on the known build, a background thread runs the resolver once
  and logs one `game update check:` line; it changes nothing the hooks use.
  When the arc closes, propose removing it.
- **Flight 1** (2026-10-25, VR, Frontier, `edvr_gfx_20261025_114350.log`,
  v0.21.1-41-gc041596b): `315 of 318 found, all equal`, 0 differ, in 90.4 ms.
  The 3 not found (0x52EB4F8, 0x52EC240, 0x52EC248) are the pointer slots the
  VR SSAA hold's DLL-load install rewrites before the check's thread starts
  (entry below); every hook armed with its usual text. Flat
  (`edvr_gfx_20261025_134245.log`): the same 315 and the same three, 84.2 ms.
  An unknown build needs a game update; none is out yet.
- **Open:** (1) `tools\object_classification.py` checks a capture's
  return RVAs against build 334925's; a capture from an unknown build carries
  that build's offsets and would be rejected (step 3 left the reader alone);
  (2) no on-disk cache; (3) the reads round 2 left unwitnessed (its list);
  (4) the audit cannot follow a parameter whose caller is in another file;
  (5) terrain cull cover's own scan runs in any process (not changed).
- **Ruled out:** naming a clone by its neighbours' positions alone
  (layout-only), because it gave a WRONG answer in step 1's wider survey. It
  stays refused; a neighbour only counts when it agrees with a witness that is
  not layout.

## What step 1 measured

Each address is matched as part of its whole code unit: a `.pdata` function
range or a leaf stub. The bytes a rebuild moves are masked out: rel32 branch
fields, RIP-relative displacements, relocated pointers and jump-table RVAs.
The signature is the FNV-1a-64 of the masked unit plus its length. A target
is accepted only when exactly one unit of that length matches. Clones and
data need a second witness: a caller's reference from a unique unit; for
data, a reference, the one pointer cell holding a found function's address,
or the one string with that length and hash.

On 334473 and 332841 this found 215 of the 222 addresses, 0 WRONG. The seven
it refused were six clone functions, identical once masked (0x8BEA20,
0x19B3F40, 0x19B3FD5, 0x19B4059, 0x2864CE0, 0x42E2150), and the data cell
0x52EB4F8, which holds the address of one of them.

## What step 2 built

### Clone fingerprints

For a target whose masked unit has clones, the generator records what the
clone's masked fields point at in the known build:

- **code:** a rel32 or RIP-relative field that lands in another code unit.
  It records the field's offset, the length and masked hash of the unit it
  lands in, and the offset it lands on.
- **rodata:** a RIP-relative field into a read-only data section. It records
  the FNV-1a-64 of up to 16 bytes there. The window stops before the first
  relocated byte, and the import address table is never used, because the
  loader rewrites both.
- **fall:** the unit's last instruction does not end the flow (no ret, jmp,
  int3 or ud2), so execution runs on into the next unit. It records that
  unit's length and masked hash. This is a code relation, not a layout one:
  the next unit is where the function continues.

Fingerprints are recorded only when, together, they single the target out
among its known clones. At resolve time a candidate is accepted only when it
is the one clone that shows every fingerprint ("fp"). Zero or several such
clones give no answer. As with every witness, an fp answer that disagrees
with another witness refuses the target. The six clones resolve as follows:

| Address | Told apart by |
|---|---|
| 0x8BEA20 | the 16-byte constant it loads |
| 0x19B3F40 | the fragment it falls into (a 9-byte prologue fragment) |
| 0x19B3FD5, 0x19B4059 | the read-only object whose address it passes |
| 0x2864CE0, 0x42E2150 | the function it calls or jumps to |

0x52EB4F8 then resolves through its pointee. In the survey, the fingerprint
answered two units where the build map guessed another place (the map called
them CHANGED). Both were read by hand and the fingerprint was right both
times. One is a getter for a `SetOrientation` binding entry, where the map
picked the getter for `SetOrientationExecuteManeuver`. The other registers a
function identical to the known one, where the map picked one that registers
a different function.

### The C++ table

`python tools\hook_signatures.py generate ... --emit-cpp
src\common\game_signatures.inc` writes the table as C++ arrays: per unit its
length, masked hash and mask ranges; per target the known RVA, kind, unit and
offset, its references, neighbours and fingerprints, and for data the
section, the guard and the pointee. It also writes the known build's PE stamp,
image size and number, and a hash of the JSON table. It holds hashes,
lengths and offsets only, never code bytes or strings, because the repository
is public and the game's code must not be redistributed. The self-test checks
for leaked bytes. The table is committed: 203 units, 1980 mask ranges, 57 KB
of source.

### The runtime resolver

`src\common\game_build_core.h` is the pure matcher: a C++ copy of the Python
`resolve`, rule for rule. It reads the image only through a copy callback.
`src\common\game_build.cpp` supplies that callback as a `memcpy` under SEH on
the running module, so an unreadable page refuses and never crashes. The API
(`src\common\game_build.h`):

- `uintptr_t gameRva(uint32_t knownRva)`: on the known build, knownRva with
  no other work. On an unknown build, the RVA found by signature, or 0 when it
  was not found or is not in the table. All addresses are resolved at once on
  the first call, under a lock, and one `game build:` line is logged: found N
  of M, by how, the time in ms, and up to 16 that were not found.
- `gameBuildKnown()`, `gameBuildIdentity(&stamp, &size)` and
  `gameBuildText()`: the last returns "build 334925" or "unknown build (PE
  stamp 0x..., image size 0x...)".
- `gamePinMatches(knownRva, got, pin, n)`: an exact compare on the known
  build. On an unknown build it skips the bytes the signature masks inside
  the matched unit and requires every other byte to be equal.
- `gamePinProvenByMatch(knownRva, n)`: true on an unknown build when the
  range lies wholly inside a matched unit. That unit match has already proved
  every byte a rebuild does not move, so a pinned range or body hash there is
  satisfied by it.

### The update check (temporary)

The key is read at start-up, after the `game build` lines. On the known build
with the key on, a detached thread resolves the table against the running
executable into its own results and logs, for example:

    game update check: 222 of 222 found, all equal (resolve N ms over 301531 units, 157306 candidates, 9431913 bytes hashed)
    game update check: 221 of 222 found, 1 differ: 0x... found at 0x... (hash); not found: 0x... (ambiguous) (resolve N ms ...)

The resolve on an unknown build writes the same shape after `game build: `
(found N of M game addresses by signature, by how, the time, up to 16 not
found). The texts are built in `game_build_core.h` and the rig checks them.

If the key is on but the build is unknown, one line says the check runs only
on the known build. The key is allowed in every profile by
`runtimeProfileAllowsKey` (`src\common\runtime_profile.h`). It is not in
`flat_profile_keys.h`, so the installer offers no row for it.

### The pilot: the store hook

`src\d3d11\store_hook.cpp` now takes its identity from `gameBuildIdentity`,
the store's address from `gameRva(0x4314C60)` and its pin compare from
`gamePinMatches`. `storeInstallDecision` (`optimized_station_core.h`) takes
the PE header read, the store's address (0 when it was not found), the
prologue and the pin result. On 334925 the decision, the status strings and
every log line are what they were. On an unknown build the hook comes up
only if the store is found, and logs `store hook: the admission store
(0x4314C60 in build 334925) was found by signature on unknown build (...) at
0x...`. If it is not found it logs a refusal, and the station and the flat
world witness stand down as before. In both older builds the store moved
(0x4314D00, 0x4312040) and is found by hash. No other hook is migrated yet.

### The rig

`tools\game_build_test` (build.bat `:rig_game_build_test`):

- **(a) always, in CI too:** `hook_signatures.py fixture` writes the
  self-test's three synthetic exes, their table as C++ and Python's answers.
  Each exe is mapped as the loader maps an image: sections at their virtual
  addresses, relocated for the address it lands at, and the IAT bound to
  stand-in pointers. The rig checks the named cases: a move with changed
  rel32, RIP and reloc fields, a changed struct offset, clones with and
  without fingerprints, and data by reference, cell and string. It also
  checks the masked pin compare, a page made unreadable, and a non-image.
- **(c)** On every fixture exe, every target's address, via and reason
  equal Python's.
- **(b)** When the archived exes are at the main checkout's `analysis\` (found
  from the git common directory, or `EDVR_ANALYSIS_DIR`), the rig maps each
  one and resolves it with the committed table. It checks the result against
  `tools\game_build_test\expect_<build>.txt` (ground truth plus Python's
  answer): 0 WRONG, Python's EQUAL count and the same answer per target. It
  also checks that the known exe resolves to itself. If the exes are absent
  it prints `game_build_test: skipped: no archived exes` and passes. An
  expectation file made for another table fails the rig, with the command to
  regenerate it. One made for another known build is skipped.
- A `static_assert` ties the table's known identity to
  `ecm::kExpectedTimestamp`/`kExpectedImageSize`. A retarget that moves the
  constants without the table fails to compile.

## What step 3 built (2026-10-25)

### The resolver

- **Addresses inside a unit.** `gameRva(k)` for an RVA that is not in the
  table but lies inside the code unit of a found table address returns the
  same offset in the unit found. A whole-unit match keeps every instruction's
  offset, so a call site, a return address or a site end moves with its
  function (the list merge's `0x43513A5` re-check is one). If two found
  targets of one unit disagree on where it is, there is no answer.
  `hook_signatures.py` has the same rule (`unit_rva`); the expectation files
  list 488 unit probes and the rig holds C++ to Python on each, and checks the
  byte after every found code target against the ground truth.
- **Fragments.** `gamePinMatches` and `gamePinProvenByMatch` accept a range
  over several units when every unit was found and all moved by the same
  amount: a function that `.pdata` splits into chained fragments. Three
  kinematic body hashes span fragments, so their nine fragment starts joined
  the table (`.agents\build-334925\signature_extra.txt`, the second `--rvas`
  input). All nine resolve EQUAL on both older builds.
- **`gameRvasFound(list, n, &missing)`** checks a hook's whole list at once.
- **At load.** On an unknown build `gameBuildStartup` starts the resolve on a
  thread of its own; a hook that asks first waits on the same lock. A resolve
  that finishes before the log opens keeps its `game build:` line until
  `gameBuildStartup` writes it. (Until round 2 the VR Supersampling hold asked
  from DllMain; it no longer does on an unknown build.)

### The hooks

Each hook takes its identity from `gameBuildIdentity`/`gameBuildKnown`, its
addresses from `gameRva` and its pins from `gamePinMatches`. Body hashes run
exactly as before on the known build; on an unknown build they are satisfied
by `gamePinProvenByMatch` over the same range. Where a pin held a call's rel32,
which the masked compare skips, the hook now also checks that the call reaches
the function found (true by construction on 334925). Before patching a field
the signature masks, the hook checks that the original still points where it
should. Every new log line is written only on an unknown build.

| Hook | Addresses | Notes |
|---|---|---|
| Transition flash engine fix | consumer 0x2845730 | the armed line prints the identity `gameBuildIdentity` read |
| Kinematic family (16 detours, part test, primary emit, pool copier, list merge, typed clear) | all table addresses; 0x43513A5 in-unit | five body hashes; each sub-hook stands down alone |
| Engine motion lookup and append | 0x3699C00, 0x4315DA5 | |
| Scheduler stack probe | 0x42E2560, 0x36A3BB0 | the scan bound comes from the PE header on an unknown build |
| Object record writer probe | 18: the lookup, its call sites and returns | all or none (`gameRvasFound`); `*Rva` members became `*Offset` |
| Cull gate probe | 0x4F4F10, data 0x5EA63E9 | |
| Skin join, pad state, vertex resync | 0x4C56BA0; four readers; 0x522B50 | decision functions and rigs as the pilot's |
| Light grid cache | builder, refresh, copy, six data globals | all nine needed on an unknown build |
| UI quality panels | two sites, two functions, three constants, setter, getter, two slots | each disp32 must reach its constant before it is patched |
| VR Supersampling gate, hold, window trampolines | slot 0x52EB4F8, wrapper, loader; 0x7E9E50, 0x281E650 | the hold's rel32 check is kept, on resolved addresses |
| Flat camera inject, producer probe | 0x592300, 0x51B740; four known callers | these had only a pin before, no identity check |
| Flat stationary capture | renderer, MainView return, 27 vtables and functions | the pure core takes resolved addresses from the glue |

The store hook was step 2's pilot. Not migrated: the exact-build-only list in
the Status block.

### The gate

`python tools\hook_signatures.py audit --src src` is one of build.bat's Python
gates. Outside the resolver and the exact-only files (named in
`AUDIT_EXACT_ONLY`), it fails the build when:

- **identity:** a file uses the known build's PE stamp or image size, or a
  name defined from one;
- **known:** a hex literal in the image's range is neither a table address nor
  inside the unit of one (`AUDIT_NOT_ADDRESSES` lists the masks, shader tokens
  and the like that are not addresses, each with its reason);
- **routed:** a game address, a name defined from one, or a member named
  `*rva` is used other than as the first argument of a resolver call, a
  constant's or a table entry's initializer, inside `sizeof` or
  `static_assert`, as a log argument or as an assignment's target. `base +
  0x4314C60` and `base + entry.rva` fail; `base + gameRva(0x4314C60)` passes.

A future hook that skips the resolver fails the build rather than standing
down in silence. `hook_signatures.py where --rva 0x...[:length]` says how
`gameRva` places an RVA and whether a range lies in table units. The startup
line for an unfamiliar build (`d3d11_proxy.cpp`) said every fix found its
target by shape; it now says what the hooks do.

## Round 2: the review's findings (2026-10-25)

An independent review found no change on the known build and these gaps on
an unknown one. Every fix leaves 334925's behaviour and log text as they
were: each new check is true there by construction (`gameRva(k)` is k), and
each new line is written only off the known build.

### Processes that are not the game

`identity()` called any executable that was not 334925 an unknown build, so
a stranger's process that loaded the proxy (the installer, a tool) had its
image scanned. Now an identity that is not the known build's is the game
only when the main module's file name is `EliteDangerous64.exe` (any case,
`gamesig::isGameExecutable`, pinned by `game_build_test`). Anything else is
"not the game (the process is X.exe)": nothing is resolved, `gameRva` is 0,
every hook refuses, and `gameBuildStartup` logs one line if a log opens.
The known identity is not name-checked, so its path is unchanged. No rig
depended on the old behaviour: the rigs stub `game_build.h`, and the one that
loads the real DLL (`profile_switch_test`) reads no game-build line.

### The loader lock: the hold arms after the resolve (option a)

The hold installs from DllMain so that its slot is in before the fxcfg
loader first runs. Every flight log read (Frontier VR 2026-10-23, four
sessions; Steam flat 2026-10-25) has that first run 4 to 8 s after the first
device; the doc's own sessions agree (4.4 s after the first Present). So on
an unknown build `vrSsaaHoldEarlyInstall` does nothing, and
`gameBuildStartup`'s thread installs the hold
(`vrSsaaHoldInstallAfterResolve`) once the resolve is done, about 0.1 s after
the first device. `vrSsaaHoldReport` calls the same install first, under one
mutex, so the report never runs ahead of it. A loader run before the install
would not get the startup hold; the copy, setter and live paths still hold
the field. The resolve is no longer split: nothing needs a subset, and the
background thread now does the whole of it. Build 334925 still installs from
DllMain, with no resolve.

### Field witnesses

A whole-unit match proves a function's bytes, and so the displacements
inside it, but not a field EDVR itself writes. A witness is an instruction
in a matched unit whose unmasked bytes carry that field's displacement on
the same object. Each is a table target (`signature_extra.txt`, "Field
witnesses"), and the hook requires them (`gameRvasFound`) on an unknown
build before it writes. All 92 lines map IDENTICAL, RELOC-ONLY or XREF-EXACT
on both older builds, and resolve EQUAL.

| Hook | Field written (W) or read to decide (R) | Witness |
|---|---|---|
| VR SS hold | ctx = setter this+0x18 (R); ctx+0x3564 (W) | 0x2878D40, 0x2878D5D in the setter |
| VR SS hold, startup | loader object +0x13C (W) | 0x285813F: the store at +0x124+12*entry; entry 2 is named by 0x28580F0's lea, which must reach the string 0x52EEB60 (data, found by content; checked at install) |
| Copy W relay and copy hold | arg+0x1180 ctx, ctx+0x33C8, settings+0x30 (R) | 0x281E678, 0x281E67F, 0x281E68D |
| VR window rewrite | request +0x08, +0x10, +0x18, +0x20, +0x24, +0x25 (W) | 0x7E9EC4, 0x7E9EDF, 0x7E9F1F, 0x7E9E8D, 0x7E9F6B, 0x7E9F72 (rsi = the request) |
| Kinematic rig veto | rig+0x348, coll+0x2E0 (R); coll+0x629 (W); coll+0x300 (call argument) | 0x431DD7D, 0x431DE94, 0x431DE88, 0x431DE81; the callees carry +0x2A4 and +0x78 |
| Engine motion | item+0x124, +0x138 (W): the producer's local at rsp+0x60 copied 0x150 bytes into node+0x20 | 0x4315A29, 0x4315A31, 0x4315AAC, 0x4315AD4, 0x4315D14, 0x4315D22, 0x4315D27 |
| Engine motion | item+0x120 marker (W): a dword no game code writes | the constructor's stores of +0x118 and +0x124 (0x4C861D6, 0x4C861F4); the producer's call 0x4315A5C must reach 0x4C86090 (checked) |
| Engine motion | record +0x170, +0x17C, +0x250; owner +0x260, +0x2A4; entry +0x28, +0x30; node +0x18; item +0x08, +0x10 (R) | 0x4315A6D, 0x4315A7C, 0x4315CA3, 0x4315CBA, 0x4315DB5, 0x4315CCA, 0x4315CD3, 0x4315CD7, 0x4315AC8, 0x4315AA6 |
| Vertex resync | applied+0x168, +0x228 (W); pso+0x188, layout+0x60, wrapper+0x140, desired+0x80 (R) | 0x522C80, 0x522C88, 0x522BAB, 0x522C1E, 0x4E9EC0, 0x4E9EA2 |
| Light grid cache | the key +0x00..+0x77, vectors +0x80, +0xA0, +0x110, GPU pointers +0x130..+0x148 (W, swapped whole); vector size +0x10 and data +0x18; first-call byte +0xC0; view +0x1B0..+0x1EF, +0x230, +0x234, +0x238 (R) | 33: the builder's compares and stores of each key field, its leas of the vectors, its four GPU stores, both resize routines' header reads, its view reads (light_grid_cache_hook.cpp lists them) |
| Store hook (optimized station) | record = arg 5; record+0x18 (R); +0x234, +0x208 (W) | 0x4314C89, 0x4314CBD, 0x4315227, 0x431523D |
| Flat camera inject | camera+0x250 bits 8 and 4, +0x28C, +0x290 (W); +0x264, +0x260, +0x280, +0x254, +0x258 (R) | 0x592344, and 0x59234E (the finalizers take camera+0x20); the projection builder's 0x4F3845, 0x4F34A1, 0x4F3499, 0x4F3135, 0x4F343A, 0x4F3422, 0x4F312D, 0x4F3208 |

Skin join, pad state, flat stationary capture, transition flash, the
scheduler stack, object record writer and cull gate probes write no game
memory (an Explore pass over each file, 2026-10-25).

**Unwitnessed:** no write stands down. The mode request's +0x26 and +0x27
are the only written bytes no instruction reads; on an unknown build the
rewrite now leaves them as the request has them (334925 writes the block
whole, as before). Residual reads, which feed a write but are not witnessed:
optimized station's record walk (record +0x190, +0x240, +0x280, +0x290 and
the pose's +0x10, +0x20, +0x80, +0x90, +0x2A8, +0x2B0, the bucket's +0x28,
the render context's +0x1A940, the node's +0x08 and its block's +0x20,
+0x2C); engine motion's record+0x18; light grid's element sizes (32, 32, 48)
in its size bound. All are read under SEH.

### The gate

`audit` gained three rules, and the old ones lost their type lists:

- **base:** the module base (a name, member or parameter holding
  `GetModuleHandle` of the executable, or a function returning one, followed
  through assignments, conditionals and same-file calls) plus anything but a
  gameRva result fails, whatever the value. PE header fields, comparisons,
  range tests and `AUDIT_BASE_OPERANDS` (the scheduler's two .text bounds)
  pass. 126 sums in the tree.
- **zero:** a gameRva result (a call, or a name, member or array holding one)
  in address arithmetic needs a zero test or a `gameRvasFound` guard in the
  same function (the file, for a member or global). Seven tree sites lacked
  one and now test locally (kinematic x4, engine motion, the hold x2).
- **known:** a game address written in decimal fails.
- **routed:** a constant is a carrier whatever its type is spelled, a
  `std::array` or C array of addresses is one, and a struct member that a
  table entry fills with an address is tracked like `*rva` (by position,
  from the struct's declaration).

The self-test pins the review's shapes, each now failing: a member not named
`*rva`, `UINT64` and `std::array` constants, a low RVA on the module base and
on a parameter its caller fills from it, a decimal address and a decimal
offset, a gameRva result and call with no zero test, a conditional result,
a member holding the base, and the control. A file of passing shapes (PE
fields, tested conditionals, comparisons, bounds) must stay clean. Limit:
a parameter whose only caller is in another file is not followed; the
review's verbatim `b + 0x8A3C0` on a bare parameter still passes.

### Hot path, log order, comments

`rigOffScreenEmulate` reads its two callees from atomics set at attach. On
an unknown build `gameRva` is a binary search (`gamesig::RvaIndex`, built
after the resolve and checked against `unitRva` by the rig). The kinematic
"found by signature" line now follows a successful install, and engine
motion's found line is written once. The stale comments in
`scheduler_stack_hook.h` and `vr_window_trampolines.h` were rewritten.

## Flight 1: the check in the game (2026-10-25)

VR, Frontier, build 334925 (the resolver does not depend on the VR runtime), log
`edvr_gfx_20261025_114350.log`, version v0.21.1-41-gc041596b (the log's
version line matched `edvr_log.py --expect-build HEAD`), `advanced.game_update_check = on`.

- `game update check: 315 of 318 found, all equal; not found: 0x52EB4F8
  (data-ref-failed), 0x52EC240 (data-ref-failed), 0x52EC248 (data-ref-failed)
  (resolve 90.4 ms over 301531 units, 183686 candidates, 10396271 bytes
  hashed)`. The unit, candidate and byte counts are the rig's for 334925
  within a few candidates, so the in-process image reads as the file does.
- The three are pointer cells: the VR SSAA loader slot (`vr_ssaa_hold.cpp`
  `kLoaderSlotRva`) and the Supersampling setter and getter vtable slots
  (`ui_sizing_math.h`). A pointer cell is found as the one cell holding the
  found function's address. On the known build DllMain runs
  `vrSsaaHoldEarlyInstall`, which writes the loader slot and calls
  `uiPanelScaleEarlyHooks` for the other two, so all three hold EDVR's thunks
  before the check's thread starts at the proxy's init (11:43:50.430). The
  hold's "installed at DLL load" line and the panels' slot line are reports
  written later (11:43:50.950, .958). On an unknown build that DllMain path
  does nothing and the hooks install after the resolve (round 2), which is the
  order the rig tests (318 of 318 on both archived builds).
- ruled out: the three misses as a resolver fault on an unknown build,
  because each cell is one EDVR itself rewrote at DLL load on the known build
  only, and the rig, reading unhooked images, finds all three.
- Every hook armed with its usual text: transition flash, the kinematic set
  (job-3's "a jump or a call" decline is in every Frontier log since 10-23),
  skin join, vertex resync, light grid cache (97.8% hits), terrain cull
  cover, the store hook, optimized station and the pad state probe.
- Flat, the same install (`edvr_gfx_20261025_134245.log`, profile flat, 3D
  mode 0): `315 of 318 found, all equal`, the same three not found, in 84.2
  ms. The VR SSAA hold installs at DLL load in flat too. The first flat start
  (13:38) had no check line: the key was only in `edvr.ini`, and the flat
  profile reads `edvr-flat.ini` alone when it exists. Its hooks armed as
  usual: vertex resync, light grid cache, the store hook, optimized station,
  the pad state probe, the UI panel sites and slots, flat camera inject's
  refresh hook; transition flash "not the VR profile", as before.

## After each retarget

Regenerate the table from the new known exe, then the expectation files
against each archived older build (the ground truth comes from `build_diff.py
map --json`, or from the previous expectation file):

    python tools\hook_signatures.py generate --exe analysis\EliteDangerous64-<new>.exe ^
        --rvas .agents\build-<new>\map_all.json --rvas .agents\build-<new>\signature_extra.txt ^
        --out build\hook_signatures.json --emit-cpp src\common\game_signatures.inc
    python tools\hook_signatures.py check --table build\hook_signatures.json ^
        --exe analysis\EliteDangerous64-<old>.exe --expect <build_diff map JSON> ^
        --emit-expect tools\game_build_test\expect_<old>.txt

`signature_extra.txt` (the fragment starts step 3 added, and round 2's field
witnesses) is mapped with the rest; the hooks' witness constants move with
it, and each witness must still carry its displacement in the new build (read
it with capstone: the comment beside each line names the instruction). `check` takes `--expect` more than once: a later file supplies the RVAs
the earlier ones lack, so an old expectation file plus a `build_diff.py map
--json` of only the new RVAs is enough. Both take `--dry-run`. Then run the full build: the rig holds C++ to Python
on the new table.
Then `hook_signatures.py audit --src src` must still pass.

## Environment

The resolver depends only on the game executable (x64 PE32+ with `.pdata`).
It is the same for every VR runtime, headset and render size, and for the
flat profile. It reads the running image's headers, `.pdata`, code and data
sections, and never writes to them.
