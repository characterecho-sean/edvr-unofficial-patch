# Game-update signatures (issue 124)

## Status

- **State:** step 3 built on a branch, full build green, not merged, not
  flown. Step 1 was the offline generator (`tools\hook_signatures.py`), step 2
  the runtime resolver and the store hook pilot. Step 3 moves every other hook
  onto the resolver and adds a build gate (`hook_signatures.py audit`).
- **Rules (the maintainer's):** on the known build (334925) every hook keeps
  its behaviour and its log text byte for byte: `gameRva(k)` returns k and the
  pins compare exactly. On an unknown build a hook comes up, game-memory
  writers included, only when every address it needs is found by a
  whole-function masked match; anything not found means that hook stands down,
  and its log says which.
- **Exact-build only, by decision:** the Oculus route profile
  (`elite_oculus_profile.h`, `oculus_route.cpp`), Explorer Cam (another arc
  deletes it), `mem_tick.cpp` (a diagnostic, not in the table) and terrain cull
  cover (its own scan finds its site on an unknown build).
- **Counts, 231 addresses** (the 222 of `.agents\build-334925\map_all.json` plus
  nine fragment starts in `signature_extra.txt`): on 334473 and on 332841 the
  Python matcher and the C++ resolver both find 231 of 231, all EQUAL to the
  build map, 0 WRONG, with the same answer per target. RVAs inside a found unit
  (call sites, return addresses, site ends): 189 checked per build against the
  ground truth, 0 WRONG; 488 unit probes, C++ and Python agree on every one.
- **Survey** (step 2, about 3500 sampled units per build): WRONG 0 on both.
- **Time:** the C++ resolve takes 65 to 125 ms in the rig (machine load); not
  yet measured in the game. On an unknown build it now starts at load on a
  thread of its own (`gameBuildStartup`).
- **Temporary key:** `advanced.game_update_check`, `off` (default) or `on`,
  hidden. On, on the known build, a background thread runs the resolver once
  and logs one `game update check:` line; it changes nothing the hooks use.
  When the arc closes, propose removing it.
- **Next flight:** the known build with the key on, VR then flat: every hook's
  lines as before, and `game update check: 231 of 231 found, all equal`. A
  not-found address that "now starts with a jmp" was hooked before the check
  read it (a race in the check, not a resolver fault). An unknown build needs
  a game update to test against; none is out yet.
- **Open:** (1) `tools\object_classification.py` checks a capture's
  return RVAs against build 334925's; a capture from an unknown build carries
  that build's offsets and would be rejected (step 3 left the reader alone);
  (2) `vrSsaaHoldEarlyInstall` asks for addresses from DllMain, so on an
  unknown build the resolve runs there, under the loader lock (its log line is
  held until the log opens); (3) no on-disk cache.
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
  that finishes before the log opens (the VR Supersampling hold asks from
  DllMain) keeps its `game build:` line until `gameBuildStartup` writes it.

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

`signature_extra.txt` (the fragment starts step 3 added) is mapped with the
rest. `check` takes `--expect` more than once: a later file supplies the RVAs
the earlier ones lack, so an old expectation file plus a `build_diff.py map
--json` of only the new RVAs is enough. Both take `--dry-run`. Then run the full build: the rig holds C++ to Python
on the new table.
Then `hook_signatures.py audit --src src` must still pass.

## Environment

The resolver depends only on the game executable (x64 PE32+ with `.pdata`).
It is the same for every VR runtime, headset and render size, and for the
flat profile. It reads the running image's headers, `.pdata`, code and data
sections, and never writes to them.
