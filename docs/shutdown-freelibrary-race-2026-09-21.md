# Crash after closing the game: FreeLibrary race in OpenXR shutdown

## Status

Opened 2026-09-21, from a supporter's log bundle. Hypothesis IDENTIFIED by
reading a crash breadcrumb and the code it points at; NOT YET CONFIRMED by a
targeted flight, and no code has been changed.

Hypothesis: `host_graphics_reset` (`src\openxr\native_runtime_host.h:1797`)
calls `NativeGraphicsClient::reset()` (`src\openxr\native_graphics_client.h:75`),
which does `FreeLibrary(module_)` on the D3D11 graphics proxy (`d3d11.dll`)
from the OpenXR shutdown thread, 78 ms after that stage began and 31 ms
after the whole shutdown sequence reported clean. At that same moment the
D3D11 render thread was still active *inside that module*, chained down
through EDHM and the Steam overlay
(`d3d11.dll -> d3d11_edhm.dll -> gameoverlayrenderer64.dll -> d3d11.dll ->
crash`), and the crash's own breadcrumb says the faulting address has "no
module" -- consistent with `d3d11.dll` having just been unmapped out from
under a thread still executing in it.

`src\d3d11\device_hook.cpp:2729-2731` documents the opposite assumption for
the D3D11 side's own teardown ("Normal process exit skips this entire
path"): `shutdownDeviceHooks` was written on the belief that `FreeLibrary`
on this module during a normal game close does not happen. The OpenXR side
now calls it there, on every close.

Not confirmed: whether this OpenXR-side `FreeLibrary` is the call that
actually drops `d3d11.dll`'s refcount to zero (the game, EDHM or Steam may
hold their own references, in which case the module would survive this call
and some other explanation is needed); whether `stereo.drain()`, the stage
immediately before this one, was meant to rule out exactly this and has a
gap, or never covered a Present arriving from the game's or the overlay's
own shutdown-time redraw; and whether this needs EDHM and the Steam overlay
both, or reproduces with either alone.

Next flight, if this is picked up: raise `log.max_mb` first so the gfx-side
log survives to the close (see below -- this flight's did not). Reproduce
once with EDHM's chain target switched to the system `d3d11.dll` (no EDHM),
and once with the Steam overlay disabled for Elite Dangerous, to see which
component the race needs.

## The flight

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
`graphics.reset(); externalDevice = nullptr;`. `NativeGraphicsClient::reset()`
(`native_graphics_client.h:75-80`):

    void reset() {
      context_.Reset();
      device_.Reset();
      if (module_) FreeLibrary(module_);
      module_ = nullptr;
    }

`module_` is a `GetModuleHandleExW`-retained handle to the D3D11 graphics
proxy itself, taken in `acquire()` (`native_graphics_client.h:21-72`) by its
trusted absolute path -- `d3d11.dll` beside the game, EDVR's own module.
This `FreeLibrary` is an explicit unload of the very module the render
thread was, per the breadcrumb above, still executing inside.

The D3D11 side's own teardown comment
(`src\d3d11\device_hook.cpp:2729-2731`) assumes the opposite:

    // FreeLibrary teardown can run under the loader lock on another thread.
    // Invalidate timing first, ...
    // Normal process exit skips this entire path.

and the same file already treats "a thread can still be inside a stub" as a
known hazard for the vtable hook's trampoline pages (~line 2764: the
live-mode stub page is leaked on purpose rather than freed, because a
thread can still be inside it) -- but that guard covers only the small stub
pages, not the whole-module `FreeLibrary` the OpenXR side now performs.

## Not ruled out

- Whether the OpenXR-side `FreeLibrary` is the release that actually brings
  `d3d11.dll`'s refcount to zero, or whether some other reference was
  already the last one before it. If the module survives this call, the
  mechanism above needs revising.
- Whether `stereo.drain()` (`host_owned_graphics_drain`, the stage
  immediately before `host_graphics_reset`) was meant to rule out exactly
  this and has a gap, or only ever covered EDVR's own in-flight XR frame,
  never a Present the game or the overlay issues on its own account during
  close.
- Whether this needs EDHM and the Steam overlay both in the chain, or
  reproduces with either alone. The one flight available has both.

## Ruled out

- EDVR's own OpenXR shutdown throwing: the log reports every stage `ok=1`
  and `module_shutdown_return,exception=0` on its own thread. The exception
  is not there; a different thread lands in unmapped memory 31 ms later.
- A stale build: `edvr_log.py --expect-build v0.17.0` matched on both logs.
