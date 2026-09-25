# Crash after closing the game: FreeLibrary race in OpenXR shutdown

## Status

Opened 2026-09-21, from a supporter's log bundle. Updated same day three
times more: a second bundle ruled out SteamVR-the-runtime (identical crash
on VirtualDesktopXR); the user then turned the Steam client's desktop
overlay off for Elite Dangerous with EDHM left in place, and **the crash
stopped** (reported directly, no log bundle for that run); then, starting
the fix design, a **code-citation correction**: the freeing call is
`NativeDevice::reset()` (`src\openxr\native_device.h:96-97`), not
`NativeGraphicsClient::reset()` as first written below -- the two classes
are easy to conflate by name and both have a `reset()` that frees a module,
but `native_runtime_host.h:1798`'s `graphics` field is declared
`NativeDevice graphics` (line 172), not a `NativeGraphicsClient`.
`NativeGraphicsClient` is unrelated to this arc; do not cite it again. See
`## Design` for the corrected mechanism, a real, not-yet-confirmed puzzle
in it (`systemD3D11CreateDevice()` appears to already leak a permanent
reference to the same module this code frees), and the proposed fix.
Built 2026-09-25 (PR #40), not flown: `NativeDevice::reset()` in
`src\openxr\native_device.h` now sets `systemModule_ = nullptr` without
calling `FreeLibrary()`, and logs `device_module_reset,pinned=%u`: whether
the static reference in `systemD3D11CreateDevice()` was taken in the same
DLL. `pinned=1` in a crash flight refutes the unmap hypothesis (the removed
`FreeLibrary` could not have been the last release); `pinned=0` leaves the
crash/no-crash result as the evidence. `native_device_test` checks the flag
and that `reset()` leaves it set; nothing tests the log line itself. The
investigation stays open until a repro flight shows the crash gone and the
new log line fired.

Hypothesis, as corrected in `## Design`: `host_graphics_reset`
(`src\openxr\native_runtime_host.h:1797-1798`) calls `NativeDevice::reset()`
(`src\openxr\native_device.h:96-97`), which does `FreeLibrary(systemModule_)`
on System32's `d3d11.dll` -- the genuine Windows module, obtained earlier by
`openSystemD3D11()` (`src\common\system_d3d11.h`), not EDVR's own proxy DLL
beside the game -- from the OpenXR shutdown thread, 31 ms after the whole
shutdown sequence reported clean, in both logged flights, to the
millisecond. At that same moment a thread was still active on the game's
real D3D11 immediate context (the crash's `rcx` register matches that
context's address, logged all session), reached by chaining through EDVR's
own proxy, EDHM and the Steam overlay
(`d3d11.dll -> d3d11_edhm.dll -> gameoverlayrenderer64.dll -> d3d11.dll ->
crash`), and the crash's own breadcrumb says the faulting address has "no
module" -- consistent with the called-through module having just been
unmapped out from under a thread still executing through it. The crash
handler's module-name printer strips every path down to its bare filename
(`crash_context.h:86-91`), so seeing "module=d3d11.dll" twice in one stack
does not prove it is the *same* d3d11.dll both times -- EDVR's own proxy
and System32's genuine module can both be loaded, both under that same
filename, at different base addresses. See `## Design` for how the two
frames were told apart.

`src\d3d11\device_hook.cpp:2729-2731` documents a related but distinct
assumption for the D3D11 side's own teardown ("Normal process exit skips
this entire path"): `shutdownDeviceHooks`, which would remove EDVR's own
hooks, was written on the belief that an explicit `FreeLibrary` of EDVR's
*own* proxy DLL during a normal close does not happen -- and, per the
correction, it still doesn't seem to: EDVR's own proxy is never observed
freed in these flights, only chained through, which is why its hooks (the
outermost stack frame, every flight) are still live at crash time.

Also worth a look if this reaches a fix: the gap from `host_gate_finish`
(shutdown reporting clean) to the crash was 31 ms in *both* flights,
exactly, despite otherwise-unrelated sessions. That is not what session
jitter in a race normally looks like; it reads more like a fixed cadence
(the overlay's or EDHM's own redraw or poll timer) than a coincidence, and
might narrow which side owns the next call into the freed module.

2026-09-25: external PR #40 builds the `## Design` fix; changes requested,
not merged. Two findings from its review, both under `## Not ruled out`:
the refcount question very likely answers itself against the hypothesis
(the fix would then be inert for this crash), and the Design's diagnostic
cannot discriminate as written. A freed overlay hook trampoline is recorded
there as an alternative cause.

Next flight, if this is picked up: raise `log.max_mb` first so the gfx-side
log survives to the close (the first flight's didn't; the second did).
Reproduce once with EDHM's chain target switched to the system `d3d11.dll`
(no EDHM), and once with the Steam overlay disabled for Elite Dangerous
specifically (Steam library > Elite Dangerous > Properties > General --
this is independent of the VR runtime and of "SteamVR", which the second
flight already ruled out), to see which component the race needs.

## First flight

`edvr-logs-20260921-142342.zip`, supplied by the user, read with
`tools\edvr_log.py`.

- Build confirmed: v0.17.0 (build 6AAC7CA4, linked 2026-09-17 23:49:56 UTC).
  `edvr_log.py --expect-build v0.17.0` matched on both the gfx and the
  openxr log -- exit 0, current public release, not a stale DLL.
- Chain, from `edvr_install_state.ini`: `chain_target = d3d11_edhm.dll`,
  `chain_mod = EDHM`. The user runs EDHM chained under EDVR.

### The gfx log's blind spot

`edvr_gfx_20260921_124940.log` stops at local 13:25:32.498 with:

    [edvr] log size cap reached; nothing further will be written. Raise log.max_mb to change this.

The crash was ~42 minutes later (both the openxr log and the breadcrumbs
file put it at local ~14:07 / UTC ~12:07). The D3D11-side detailed log is
silent for the entire close-out; only the lightweight, uncapped
`edvr_breadcrumbs.txt` was still recording when it happened. Any future
repro flight for this arc should raise `log.max_mb` first.

### The crash, from edvr_breadcrumbs.txt

    8472046 gfx: UNHANDLED exception 0xC0000005 at 0x7FFC59E4F780 in no module (address is not in a loaded image)
    8472046 crash: thread=0x75C0 code=0xC0000005 op=execute address=0x7FFC59E4F780
    8472046 crash: regs rcx=0x1CE67BEAF18 ...
    8472046 crash: unwind frame=0x0 rip=0x7FFC59E4F780 ... module=unknown
    8472046 crash: unwind frame=0x1 rip=0x7FFC5BC87009 ... module=d3d11.dll rva=0x177009
    8472046 crash: unwind frame=0x2 rip=0x7FFC5F9713D5 ... module=gameoverlayrenderer64.dll rva=0x713D5
    8472046 crash: unwind frame=0x3 rip=0x7FFC5F9BE368 ... module=gameoverlayrenderer64.dll rva=0xBE368
    8472046 crash: unwind frame=0x4 rip=0x7FFC5F9C0DFA ... module=gameoverlayrenderer64.dll rva=0xC0DFA
    8472046 crash: unwind frame=0x5 rip=0x7FFC5F993F05 ... module=gameoverlayrenderer64.dll rva=0x93F05
    8472046 crash: unwind frame=0x6 rip=0x7FFC5A983AD7 ... module=d3d11_edhm.dll rva=0xD3AD7
    8472046 crash: unwind frame=0x7 rip=0x7FFC5BB30D8E ... module=d3d11.dll rva=0x20D8E
    8472046 crash: unwind stop=frame-limit

`thread=0x75C0` is 30144 decimal -- the same thread ID the gfx log's
"Application-render GPU" lines named as the D3D11 immediate context owner
all session (`context 000001CE67BEAF18 thread 30144`), and the crash's own
`rcx=0x1CE67BEAF18` is that same context pointer. This is EDVR's own D3D11
render thread, not a thread the overlay or EDHM own.

Read outermost-to-innermost (frame 7 called frame 6, ... frame 1 called
frame 0, the fault): EDVR's own hook (`d3d11.dll+0x20D8E`) called into EDHM
(`d3d11_edhm.dll+0xD3AD7`), which reached the Steam overlay
(`gameoverlayrenderer64.dll`, four internal frames), which called back into
`d3d11.dll` at a *different* offset (`+0x177009`, not the entry point --
some other hooked function) -- and that call landed in memory with no
module at all.

### Correlation with EDVR's own shutdown

`edvr_openxr_20260921_124942_352_30076.log` (the OpenXR-side runtime, same
build) shows a clean, `ok=1` shutdown finishing on a different thread just
before the crash:

    12:07:41.285 shutdown_stage,begin=host_graphics_reset,thread=28252,tick=8471968
    12:07:41.285 shutdown_stage,end=host_graphics_reset,ok=1,thread=28252,tick=8471968
    ...
    12:07:41.339 shutdown_stage,end=host_gate_finish,ok=1,thread=28252,tick=8472015
    12:07:41.340 module_shutdown_return,exception=0

The `tick=` values are `GetTickCount64()` milliseconds (same clock as
`device_gpu_timing.cpp`'s `nowMs()`), so the crash breadcrumb's tick
(8472046) is 78 ms after `host_graphics_reset` began and 31 ms after the
whole sequence reported clean.

`host_graphics_reset` (`native_runtime_host.h:1797-1798`) is
`graphics.reset(); externalDevice = nullptr;`, where `graphics` is a
`NativeDevice` (`native_runtime_host.h:172`). `NativeDevice::reset()`
(`native_device.h:96-97`):

    void reset(){context_.Reset();device_.Reset();feature_=D3D_FEATURE_LEVEL_1_0_CORE;
      if(systemModule_){FreeLibrary(systemModule_);systemModule_=nullptr;}}

`systemModule_` is set only by `initializeSeparate()` (`native_device.h:77-91`),
which both flights' openxr logs confirm ran: `device_module,route=mapped,
path=C:\Windows\system32\d3d11.dll` and `graphics_ownership,mode=separate,
distinct_devices=1`, both flights. "Separate" mode is not an edge case --
`native_module.cpp:260-276`'s `packagedDefaultPaths()`, the path taken by
every normal install with no explicit bootstrap or local-config override,
sets `separateDevice=true` unconditionally. This is the default for
essentially every installed copy of EDVR, not something particular to this
reporter.

`initializeSeparate()` takes its module reference from `openSystemD3D11()`
(`src\common\system_d3d11.h:47-74`), which is explicit about ownership:
"One reference, the caller's to FreeLibrary" (line 34), obtained with
`GetModuleHandleExW(0, ...)` -- flag 0, a real reference, not
`UNCHANGED_REFCOUNT`. `NativeDevice::reset()`'s `FreeLibrary(systemModule_)`
is exactly that reference being correctly, deliberately released. The D3D11
side's own teardown comment (`src\d3d11\device_hook.cpp:2729-2731`,
"Normal process exit skips this entire path") is about a *different*
module -- EDVR's own proxy DLL, freed only via `shutdownDeviceHooks`, which
these flights never show running -- so it does not directly contradict this
release; it is cited here because the same file (~line 2764) already treats
"a thread can still be inside a stub" as a known hazard for a *different*
piece of freed memory (`VTableHook`'s trampoline pages, which it leaks on
purpose for exactly that reason) -- the same reasoning applies to
`systemModule_`, argued fully in `## Design`.

**A real complication, not yet resolved:** `system_d3d11.h:76-86`'s
`systemD3D11CreateDevice()` *also* calls `openSystemD3D11()`, inside a
function-local `static const` initializer -- a magic static, guaranteed to
run exactly once per process (per DLL; the OpenXR runtime module has its
own copy of this inline function, distinct from EDVR's D3D11 proxy's) --
and its comment says the module "stays mapped for the life of the process."
That local's `SystemD3D11` object goes out of scope without ever calling
`FreeLibrary` on the handle it holds -- a second, *permanent*, intentional
leak of the same System32 module. `native_runtime_host.h:1936` calls
`systemD3D11CreateDevice()` unconditionally, before the branch that chooses
`initializeSeparate()`, on every session. If that static genuinely runs
first and genuinely keeps the module's refcount above zero for the rest of
the process's life, `NativeDevice::reset()`'s later `FreeLibrary` should
never be able to bring System32's `d3d11.dll` down to zero references and
actually unmap it -- which would mean this specific call is *not* the
crash's cause after all, and the true freed resource is still unidentified.
This was not resolved by reading the code; `## Design` proposes removing
the release regardless, since it is safe either way, plus a cheap
diagnostic to settle which explanation is right.

## Second flight -- 2026-09-21, SteamVR removed

`edvr-logs-20260921-144712.zip`, supplied after asking the user to take
SteamVR out of the picture. Build still v0.17.0 (6AAC7CA4), chain still
`chain_target = d3d11_edhm.dll` per `edvr_install_state.ini` -- EDHM was not
touched, only the VR runtime. This flight's `native benchmark` lines read
`runtime="VirtualDesktopXR" headset="Meta Quest 3"`: SteamVR is confirmed
gone from the VR side.

The gfx log this time did not hit its size cap (716 KB of 2051 lines, well
under the limit) -- but it carries nothing extra at the close either; the
crash is D3D11-hook-chain territory that the gfx log's own instruments
don't cover, cap or no cap.

The shutdown sequence is the same shape, ending clean:

    12:44:59.847 shutdown_stage,begin=host_graphics_reset,thread=37592,tick=10710515
    12:44:59.847 shutdown_stage,end=host_graphics_reset,ok=1,thread=37592,tick=10710515
    12:44:59.909 shutdown_stage,end=host_gate_finish,ok=1,thread=37592,tick=10710578
    12:44:59.910 module_shutdown_return,exception=0

`edvr_breadcrumbs.txt` (append-only across every session ever run -- it
still carries the first flight's crash at tick 8472046, verbatim, further
up the file) adds a new one at tick 10710609:

    10710609 gfx: UNHANDLED exception 0xC0000005 at 0x7FFC594BF780 in no module (address is not in a loaded image)
    10710609 crash: thread=0x9364 ...
    10710609 crash: unwind frame=0x0 rip=0x7FFC594BF780 ... module=unknown
    10710609 crash: unwind frame=0x1 rip=0x7FFC598F7009 ... module=d3d11.dll rva=0x177009
    10710609 crash: unwind frame=0x2 rip=0x7FFC5F7813D5 ... module=gameoverlayrenderer64.dll rva=0x713D5
    10710609 crash: unwind frame=0x3 rip=0x7FFC5F7CE368 ... module=gameoverlayrenderer64.dll rva=0xBE368
    10710609 crash: unwind frame=0x4 rip=0x7FFC5F7D0DFA ... module=gameoverlayrenderer64.dll rva=0xC0DFA
    10710609 crash: unwind frame=0x5 rip=0x7FFC5F7A3F05 ... module=gameoverlayrenderer64.dll rva=0x93F05
    10710609 crash: unwind frame=0x6 rip=0x7FFC5C043AD7 ... module=d3d11_edhm.dll rva=0xD3AD7
    10710609 crash: unwind frame=0x7 rip=0x7FFC597A0D8E ... module=d3d11.dll rva=0x20D8E
    10710609 crash: unwind stop=frame-limit

Every RVA matches the first flight's crash exactly -- `d3d11.dll+0x20D8E`,
`d3d11_edhm.dll+0xD3AD7`, the same four `gameoverlayrenderer64.dll` offsets
in the same order, `d3d11.dll+0x177009`, unmapped. `10710609 - 10710578 =
31` ms after `host_gate_finish`, the same gap as the first flight to the
millisecond. The same file also holds a third, still-earlier crash (tick
27111687, from a session before either supplied bundle) with the same RVAs
again -- this is the user's third-for-third reproduction on every close
with this chain, not a rare race.

`gameoverlayrenderer64.dll` is Steam's per-game desktop overlay. It is
injected because the game is launched through the Steam client, and has
nothing to do with SteamVR as a VR runtime -- so removing SteamVR (the
runtime) left it untouched, which is exactly what this flight shows.

## Third result -- 2026-09-21, Steam overlay disabled, no crash

No log bundle for this one -- the user turned the Steam client's overlay
off for Elite Dangerous specifically (Steam library > Elite Dangerous >
Properties > General > "Enable Steam Overlay"), left EDHM chained, and
reported the crash stopped. Taken as reported, not independently verified
against a clean shutdown log; a future bundle from a since-quiet install
would be worth reading once for a `module_shutdown_return,exception=0`
with nothing after it in the breadcrumb file, to close this out with the
same rigor as the other two results.

This is the discriminating flight: it isolates the overlay as necessary
without a flight that removes EDHM instead, and its result matches the
mechanism -- with the overlay gone, nothing calls back into `d3d11.dll`
through the chain after `host_graphics_reset` frees it, so there is no
late call left to land in the unmapped module.

## Not ruled out

- Whether `NativeDevice::reset()`'s `FreeLibrary(systemModule_)` is the
  release that actually brings System32's `d3d11.dll` to zero references,
  given `systemD3D11CreateDevice()` appears to independently hold a
  permanent reference to the same module (see the complication above). Not
  resolved by reading the code; `## Design` proposes a fix that does not
  depend on the answer, plus a diagnostic that would settle it.
  2026-09-25, PR #40 review: the code points hard at "not the last
  release". `native_runtime_host.h:2405` calls `systemD3D11CreateDevice()`
  on every startup, in the same DLL as `NativeDevice`, and its static takes
  a reference through `openSystemD3D11()` that is never released. If that
  call succeeds, `FreeLibrary(systemModule_)` cannot drop the module to
  zero, and removing it cannot change the crash. The Design's diagnostic
  does not settle this: with the `FreeLibrary` gone, `systemModule_` still
  holds its own reference when the probe runs, so an
  `UNCHANGED_REFCOUNT` lookup always finds the module. The discriminating
  log line is whether that static reference was taken (`pinned=1` refutes
  the unmap hypothesis from the log alone).
- A freed hook trampoline instead of an unmapped module. The fault address
  resolves to "no module", which is also what an overlay's hook trampoline
  looks like: executable memory allocated outside any DLL. If the Steam
  overlay unhooks at shutdown and frees its trampolines while a render
  thread is still calling through one, the stack and the "no module" frame
  would look the same, and disabling the overlay would stop it, as the
  third result did. Untested. The separating fact is where the fault
  address falls: inside System32 `d3d11.dll`'s image range (base plus
  `SizeOfImage`, which would need logging at startup if it is not already)
  means the module was unmapped; outside it means something else was
  freed, such as a trampoline.
- Whether `stereo.drain()` (`host_owned_graphics_drain`, the stage
  immediately before `host_graphics_reset`) was meant to rule out exactly
  this and has a gap, or only ever covered EDVR's own in-flight XR frame,
  never a Present the game or the overlay issues on its own account during
  close.
- Whether EDHM is required alongside the overlay, or the overlay alone
  (EDHM out of the chain) would also crash. Not yet flown; lower priority
  now that a workaround is confirmed, but relevant to how a real fix should
  be scoped -- a fix aimed only at the EDHM link would miss an
  overlay-alone case. Given the corrected mechanism doesn't name EDHM at
  all (System32 `d3d11.dll` is shared process-wide, not EDHM-specific),
  overlay-alone reproducing it is now the more likely outcome, not the
  less likely one.
- Exactly which frames in the crash stack are System32's `d3d11.dll` versus
  EDVR's own proxy, both printed as bare filename "d3d11.dll". Frame 7
  (`+0x20D8E`, calls into EDHM) is architecturally EDVR's own proxy --
  nothing else in the process has a reason to call `d3d11_edhm.dll`. Frames
  1 and 0 (`+0x177009`, then unmapped) were not independently confirmed;
  the design below treats them as System32's on the strength of `rcx`
  matching the game's real immediate context (a COM vtable dispatch through
  a shared, process-wide object, not a call private to any one proxy), not
  on a direct address match.

## Ruled out

- EDVR's own OpenXR shutdown throwing: the log reports every stage `ok=1`
  and `module_shutdown_return,exception=0` on its own thread, in both
  logged flights. The exception is not there; a different thread lands in
  unmapped memory 31 ms later, both times.
- The Steam overlay as a bystander: turning it off (EDHM still chained)
  stopped the crash. It is a required part of the chain, not incidental.
- A stale build: `edvr_log.py --expect-build v0.17.0` matched on both logs,
  both flights.
- SteamVR as the VR runtime: the second flight ran on VirtualDesktopXR /
  Meta Quest 3, no SteamVR anywhere in the picture, and reproduced the
  identical crash. The runtime backend is not a factor; the D3D11-side
  chain (EDHM + Steam's desktop overlay) is.

## Design

Started 2026-09-22, prompted by the user asking to design a real fix. Built
2026-09-25: `NativeDevice::reset()` in `src\openxr\native_device.h` retains
`systemModule_` without `FreeLibrary()` and logs
`device_module_reset,pinned=...` from `isSystemD3D11Pinned()`. The
diagnostic below was replaced: its `UNCHANGED_REFCOUNT` re-probe always
succeeds once `systemModule_` keeps its own reference. Not yet flown.

### What changed from the original write-up

Re-reading the actual call graph (not just grepping for a `reset()` that
frees a module) found the citation above was wrong: `host_graphics_reset`
calls `NativeDevice::reset()` (`native_device.h`), not
`NativeGraphicsClient::reset()` (`native_graphics_client.h`) -- a different
class this arc never needed. `NativeDevice::reset()` frees `systemModule_`,
which both flights' logs confirm is System32's genuine `d3d11.dll`
(`device_module,route=mapped,path=C:\Windows\system32\d3d11.dll`), taken by
`initializeSeparate()` because EDVR runs in "separate device" mode -- the
default for every normal install, not an edge case. This does not change
what happened (the crash and its timing are unaffected by which citation
was wrong), but it changes where a fix belongs and who else it can affect:
this is process-wide shared-module lifetime, not EDVR's own proxy DLL, so
it's a `d3d11.dll`/`GetModuleHandleExW` refcounting question that has
nothing to do with EDHM's identity specifically.

### The fix

Stop calling `FreeLibrary(systemModule_)` in `NativeDevice::reset()`. Set
`systemModule_ = nullptr` without releasing the reference -- i.e. leak it,
the same way `systemD3D11CreateDevice()` (`system_d3d11.h:76-86`) already
and deliberately leaks its own reference to the identical module, with the
identical justification in that function's own comment: "which stays
mapped for the life of the process." `NativeDevice` gets torn down and
potentially reconstructed more than once in a process's life (its own
`reset()` runs at the top of both `initialize()` and `initializeExisting()`,
clearing state before a fresh acquire); leaking on the *shutdown* path
only, not on every `reset()`, would need either a separate
`releaseSystemModule()`-vs-`reset()` split or accepting that every
`initializeSeparate()` call after the first adds one more permanent
reference to a module that was always going to stay mapped anyway. Given
`systemD3D11CreateDevice()` already treats unbounded leakage of this exact
module as fine, the simplest version -- never call `FreeLibrary` on
`systemModule_` at all, anywhere, ever -- is in line with the codebase's
own stated model for this specific resource and needs no new bookkeeping.

This is safe regardless of which half of the "not resolved" complication
above turns out to be true:

- If `systemD3D11CreateDevice()`'s permanent reference is *not* already
  covering `systemModule_`'s slot (they're independent references, and this
  one really was the last), this removes the exact operation the evidence
  points at, and should end the crash outright.
- If `systemD3D11CreateDevice()`'s reference already keeps the module alive
  regardless, this change is a no-op for correctness -- the module was
  never actually at risk from this specific call -- and the crash's true
  cause is still open. In that case the change should still land (it is
  strictly more correct than freeing a shared system module from
  per-instance teardown code, matches the file's own precedent, and costs
  nothing), but the arc stays open rather than closing on an unconfirmed
  fix.

Either way this does not touch `context_.Reset()`/`device_.Reset()`
(releasing EDVR's own separate device/context, which nothing else in the
process shares -- `distinct_devices=1`, both flights) or any of the other
shutdown stages. Scope stays to the one line implicated by the evidence.

### The diagnostic, to fly alongside the fix

Add one breadcrumb line to `NativeDevice::reset()`, gated on
`systemModule_` being non-null, before clearing it: re-probe the module
with a fresh `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
...)` on the same absolute path immediately after the (now-removed)
`FreeLibrary` would have run, and log whether it still resolves. If it
does, `systemD3D11CreateDevice()`'s permanent reference (or another one) is
confirmed as what was really keeping the module alive, settling the open
question without costing a flight beyond the one already needed to confirm
the fix itself. If it does not resolve, that's surprising given the
analysis above and worth its own look before trusting the fix.

### Before this ships

- Build clean (`build.bat`, absolute path, read the tail) -- this is a
  one-line removal plus one diagnostic line, but it's shutdown-path code
  in a header included from the OpenXR runtime module, and a typo here
  costs a flight to notice.
- Rig coverage in `tools\openxr_native_test\native_device_test.cpp`, under
  both `--self-test` and `--hardware`: `systemD3D11CreateDevice()` returns
  a function, `isSystemD3D11Pinned()` is then true, and `reset()` leaves it
  true. The `pinned=` log line itself is not checked by any rig.
- A repro flight from this reporter (or anyone reproducing today) with the
  fix installed, confirming the crash is gone AND the new diagnostic line
  fired, read with `edvr_log.py --expect-build` against the exact commit
  first.
- Consider, separately and not blocking this fix, the still-open EDHM-alone
  question -- if the mechanism is confirmed process-wide rather than
  EDHM-specific, it may be worth a second discriminating flight (Steam
  overlay on, EDHM out of the chain) purely to complete the arc, not
  because the fix's scope depends on the answer.
