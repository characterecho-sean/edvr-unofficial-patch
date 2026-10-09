# Epic quit-to-desktop crash at EliteDangerous64.exe+0x4D78C51

## Status

Opened 2026-10-09. State: **leading reading is an Elite/Epic bug; EDVR not
proven innocent. The no-EDVR baseline has not been flown.** It is the next step
and needs no build.

The crash: Windows Application event 1000, EliteDangerous64.exe (Epic,
332.841.0.0, time stamp 0x6a989634), 0xc0000005, offset 0x4d78c51. 170
occurrences since 2026-09-23 20:29, all Epic FLAT-profile sessions on EDVR
v0.17.0-v0.18.3, none on Steam or Frontier. 163 of 170 follow a journal
`Shutdown` by 1-3 s. About 94% of Epic sessions that load a commander end this
way.

What the exe shows (capstone, image base 0x140000000; 2026-10-09):

- 0x4d78c40 is an atexit destructor, registered once by a static-init thunk
  (0x89f10, via the .rdata initializer array at 0x4db9f50). It releases the
  global at 0x5f32520 with `lock xadd [rcx+0x10],-1`; that is the fault.
- The global is **Epic Online Services auth data**: a pointer to an intrusive
  refcounted string (+0 length, +8 capacity, +0x10 refcount, +0x14 chars) copied
  from the EOS auth token at 0x9a3810 (`EOS_Auth_CopyUserAuthToken`) and handed to
  `EOS_Auth_Login` at 0x9aaba0. Steam and Frontier builds do not run this login,
  which alone explains why only Epic crashes.
- Nothing else touches the global: no direct store, no second releaser, no
  copy of the pointer. The string lives in Elite's own size-class pool
  (free: 0x549c90, pool pointer 0x5f1b428). That pool has a shutdown routine
  (0x55afc0, refcount-gated, called from 0x522c60, 0x5d2290, 0x7eca20, 0x7fe493).
  If it runs before the CRT atexit pass, the string's page is gone and the
  destructor faults exactly here. Not confirmed: whether it unmaps, and its order.
- A second route: the EOS callback path (0x9a3810 -> 0x1e5f40) has no lock, so a
  late refresh on another thread leaves a stale pointer at exit. Not confirmed.

Two facts from outside the exe:

- The Epic exe was last written 2026-09-23 19:38 (Epic manifest 4.4.332841).
  The first EDVR log and the first crash followed within the hour. **Game
  update and first EDVR run coincide**, so the timing does not separate them.
- EDVR's own breadcrumb file (191 Epic sessions) holds `gfx: process exit`
  (`d3d11_proxy.cpp:679`, DLL_PROCESS_DETACH with `reserved != NULL`) **once**,
  and no `UNHANDLED exception` line ever names +0x4D78C51. EDVR's crash filter is
  last-installed-wins (`proxy.cpp:382`), so the game's handler replaces it. EDVR
  is blind to this crash today, and its detach code almost never runs.

Open hypotheses and what separates them:

1. **Elite/Epic bug, EDVR irrelevant** (leading). Baseline without EDVR crashes
   the same way: close the arc.
2. EDVR makes it more likely by timing (an exit slower or faster than stock moves
   the pool teardown or the EOS thread relative to the atexit pass). Baseline
   shows a lower rate than 94% but not zero.
3. EDVR causes it: a hook or thunk, a heap or memory release of game memory, the
   Supersampling vtable thunk (0cd8bb7b), or flat camera injection. Baseline
   shows 0 of N. Then the exit instrument below.

## Ruled out

- Resizing: it is a quit-to-desktop crash, 1-3 s after `Shutdown`.
- EDVR's DLL_PROCESS_DETACH teardown as the faulting code: the breadcrumb file
  shows it almost never runs in these sessions, and the fault is inside the exe.
- A double release from the exe's own code: the exe has one releaser and one
  writer for the global (static analysis; a dynamic double release is not excluded).

## No-EDVR baseline (Sean)

Close the game and the Epic launcher's game process first. In the repo (or a
worktree with the tool):

    python tools\install_edvr.py --target "C:\Program Files\Epic Games\EliteDangerous\Products\elite-dangerous-odyssey-64" --suspend

That renames `d3d11.dll` to `d3d11.dll.edvr-suspended` (one rename, hash checked,
refused while the game runs). Fly 3 sessions: load a commander, quit to desktop,
wait 5 s. Count the new events:

    Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000} -MaxEvents 10 | Where-Object { $_.Message -match 'EliteDangerous64' } | Select TimeCreated, @{n='off';e={($_.Message -split "`n" | sls 'Fault offset')}}

Then `--resume` and `--verify-only --target <same> --profile flat` (the latter
compares against whichever tree's build you point `--root` at). The settings in
`edvr-flat.ini` and the mirror are untouched throughout.

Result: _pending_. Record N sessions, N events, offsets.

## Next flight

1. The baseline above (no build).
2. Crashes without EDVR: record, close the arc, tell the EDVR side nothing to fix.
3. Does not crash without EDVR: build the exit instrument (design below), fly one
   session with EDVR.

## Exit instrument (designed, not built)

A first-chance vectored handler (EDVR already uses them: `pose_reader_watch.cpp`)
that fires only on an access violation inside EliteDangerous64.exe at RVA
0x4d78c51 and writes, before the game's filter runs: the global at 0x5f32520, the
pointee address, its `VirtualQuery` state (MEM_FREE, region base, protection),
whether the address lies in a range EDVR allocated or hooked, and the thread id
and tick since the last `Shutdown` journal line. Plus two breadcrumbs: at DLL
detach in both branches (already there), and a periodic read of the global's
pointee refcount while the process lives (to see it skew). Discriminates
hypothesis 3 (pointee in EDVR territory or refcount skewed) from 1 and 2 (page
freed by the game's pool teardown: MEM_FREE with no EDVR involvement).

## Journal

**2026-10-09.** Opened. Added `--suspend` / `--resume` to `tools\install_edvr.py`
with a self-test (passes; dry run against the Epic dir resolves). Static analysis
of the global done by a delegated agent (report above, mid-confidence on the
token field name, high on the single writer, single releaser and single
registration). Windows log: 181 Elite 1000-events since 09-19, 170 at this offset,
the rest are other offsets, including six Steam 0xc0000409 at +0x137881.
