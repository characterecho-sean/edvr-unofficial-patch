# Game-update signatures (issue 124)

## Status

- **State:** step 2 built on a branch, full build green, not merged, not
  flown. Step 1 was `tools\hook_signatures.py` (the offline generator and a
  Python copy of the matcher). Step 2 adds clone fingerprints, the generated
  C++ table, the runtime resolver, its rig, a temporary check key and one
  pilot hook (the store hook).
- **Rules (the maintainer's):** on the known build (334925) nothing changes:
  every hook uses its constant and the resolver never runs. On an unknown
  build a hook may come up, game-memory writers included, only when each
  address it needs is found by a whole-function masked match; anything not
  found means that hook stands down. The Oculus route profile
  (`src\common\elite_oculus_profile.h`) stays exact-build only.
- **Counts, 222 addresses** (`.agents\build-334925\map_all.json`): on 334473
  and on 332841 the Python matcher and the C++ resolver both find 222 of 222,
  all EQUAL to the build map, 0 WRONG, with the same answer, the same unit
  count and the same candidate count per target. Step 1 found 215 of each.
  The known build resolves to itself, 222 of 222.
- **Survey** (about 3500 sampled units per build): WRONG 0 on both builds.
  Units the map places in the new build and the matcher finds: 1832 of 2258
  on 334473 (step 1: 1671), 1838 of 2252 on 332841 (step 1: 1682).
- **Time:** the C++ resolve takes 65 to 125 ms (machine load) over a 100 MB
  image mapped in memory by the rig (301,500 units, 157,000 candidates,
  9.4 MB hashed); not yet measured in the game. Python: 2.2 s.
- **Temporary key:** `advanced.game_update_check`, `off` (default) or `on`,
  hidden (commented out in `edvr.ini`, no menu row), read in VR and flat. On,
  on the known build, a background thread runs the resolver once at start-up
  and logs one `game update check:` line. It changes nothing the hooks use.
  When the arc closes, propose removing it.
- **Next flight:** the known build with the key on, VR or flat. Expect
  `game update check: 222 of 222 found, all equal (resolve N ms ...)`.
  Anything else lists the addresses that differ or were not found. A
  not-found address that "now starts with a jmp" was hooked before the check
  read it, which is a race in the check, not a resolver fault.
- **Open:** (1) migrate the other hooks (step 3), each replacing its
  stamp/size check and `base + kRva` with `gameRva` and its pin with
  `gamePinMatches`; (2) on an unknown build the first `gameRva` call
  resolves on its own thread: for the store hook that is the first Present,
  a one-time stall (65 to 125 ms in the rig; not measured in the game). A
  start-up warm-up thread would remove it (not built, a proposal); (3) no
  on-disk cache yet.
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

## After each retarget

Regenerate the table from the new known exe, then the expectation files
against each archived older build (the ground truth comes from `build_diff.py
map --json`, or from the previous expectation file):

    python tools\hook_signatures.py generate --exe analysis\EliteDangerous64-<new>.exe ^
        --rvas .agents\build-<new>\map_all.json --out build\hook_signatures.json ^
        --emit-cpp src\common\game_signatures.inc
    python tools\hook_signatures.py check --table build\hook_signatures.json ^
        --exe analysis\EliteDangerous64-<old>.exe --expect <build_diff map JSON> ^
        --emit-expect tools\game_build_test\expect_<old>.txt

Both take `--dry-run`. Then run the full build: the rig holds C++ to Python
on the new table.

## Environment

The resolver depends only on the game executable (x64 PE32+ with `.pdata`).
It is the same for every VR runtime, headset and render size, and for the
flat profile. It reads the running image's headers, `.pdata`, code and data
sections, and never writes to them.
