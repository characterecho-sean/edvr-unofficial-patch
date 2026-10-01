# macOS under CrossOver: what DXMT answers about formats

On a Mac, Elite runs through CrossOver, where d3d11.dll is DXMT, a
D3D11-to-Metal layer. One rc.4 log from that setup showed the flat runtime
treating no frames at all; two later logs show the game exiting about 30 s in,
a run with the HDR route's key off shows its first treatment is the trigger,
and the crumbs then named the step: DXMT aborts in SwapDeviceContextState.
The explicit capture that replaces it flew clean: the HDR route runs on the Mac,
with DLSS and FSR. Read the Status block first.

## Status

- **State (2026-09-30, night): the exit is SOLVED by the explicit capture, and
  the HDR route runs on the Mac.** On v0.18.0-rc.4-114-g20031385 (bundle
  `edvr-logs-20260930-175527.zip`; CrossOver, DXMT, Apple M4 Max, key `auto`)
  the isolation line names all four DXMT markers, DLSS is created for the HDR
  route, and so is FSR after a menu switch. Every full 5 s window from 17:53:23
  to 17:53:58 reads `state=active`, frames=265-300, treated equal to frames,
  declined=0, late-hdr-writes=0. The cause was DXMT aborting in
  `SwapDeviceContextState` (the crumbs entry names the step).
- **By eye (Sean, Mac, this build):** "Black floor outlines went away and
  everything looks aliased appropriately, tested with bloom and DoF on as
  well." Read: the outlines are gone, the edges are anti-aliased as they should
  be, and it holds with Elite's bloom and depth of field on. The copy route
  refuses that chain on the Mac (`no-known-tone-pass`); the HDR route resolves
  before bloom and DoF. The visual check is closed.
- **On main:** on a DXMT device the resolver isolates by an explicit capture and
  restore of the whole context state (`flat_context_state.h`); every other
  device keeps the swap, unchanged. DXMT is recognised by its own markers, the
  adapter name only as the fallback (`flat_context_isolation.h`); key
  `advanced.flat_context_isolation = auto|swap|capture`, for testing. The
  `gfx: hdr-treat` crumbs stay, DXMT-only: the markers open their gate, the key
  cannot, and Windows gets none in `edvr_breadcrumbs.txt` (`flat_hdr_crumbs.h`,
  THE GATE). The 5 s line's step counts stay everywhere.
- **Still open:** CrossOver's DXMT is another revision than DXMT main (its "not
  implemented" wording differs). Every call the capture makes ran on it, three
  frames and both ways, with no DXMT error line; what else differs is unknown.
  The route also took no frames at the session's start (to frame 1650) and from
  17:53:59 on (`no-known-tone-pass`, a stand-down, probes to the end of the
  log); the log does not say what was on screen.
- **Hypotheses:** none open. (1) and (3) confirmed and fixed (the swap, a DXMT
  fatal error that ends the process without an exception); (2) ruled out below.
- **The key-off run** (`..._144722.log`, DLSS): the copy route refused every
  frame (`no-observed-hdr-writes`, then `no-known-tone-pass`), treated=0,
  stand-down at frame=3736, F8 warning shown. The HDR route, observing, would
  have taken every frame, so on the Mac it is the only path to AA.
- **The format finding:** in `edvr_gfx_20260930_123414.log` the format-23 world
  target, R10G10B10A2_TYPELESS, is the G-buffer; Windows draws one too. The flat
  runtime's HDR checks are written for format 26 (`flatHdrCandidateDraw`;
  `k.format != 26` in `flat_mono_frame.h`); why rc.4 saw no HDR writes is open.
- **DXMT's signature:** "47 of 49 d3d11 exports did not resolve" (it has 2 of
  the 49 the proxy forwards); `vendor=0x10DE` for "Apple M4 Max".
- **The damage (rc.4):** black outlines on floor markings, gone on the fix build
  by eye; engine motion's substitution and the overlay guard ran on refused
  frames (1,719 full-frame copies, 101 GB in 30 s), now paused by main's
  stand-down (`flat_standdown.h`, ce6d511a) after 5 s of `no-known-tone-pass`.
- ruled out: "DXMT offers less for R11G11B10_FLOAT, so Elite renders its world
  in R10G10B10A2", because DXMT reports full R11G11B10_FLOAT support, the scene
  HDR target is R11G11B10_FLOAT on the Mac as on Windows, and the format-23
  target was the G-buffer. Evidence: the format-theory entry.
- ruled out: hypothesis 2, engine motion's substitution under DXMT, because it
  ran in the key-off run with no exit: it started at frames 1737 and 3473, each
  with the overlay guard active. Evidence: the key-off entry.
- ruled out: the route's creations (shaders, private textures, the target view)
  as the cause of the exit, because every one returned hr=0 and the trail runs
  past `create-rtv end` to `capture-state begin`. Evidence: the crumbs entry.
- **Decision (Sean, 2026-09-30):** no DXMT guard that turns the HDR route off on
  Apple adapters ("we'll fix crossover properly"); the route gets fixed on DXMT.

## 2026-09-30: the instrument

Built for one Mac launch, on both profiles, always on, no setting. It never
changes an answer the game gets, and prints at most 58 lines a session. It
flew twice on the Mac; results are in the next two entries.

**What it logs.** Grep the log for `D3D11 adapter:`, `format support` and
`device options`. Read them in this order; the first three say whether the
rest can be trusted.

1. One adapter line per device, the first three, written right after the
   `D3D11 device ... created` line. The created line has no vendor; this one
   says DXMT or NVIDIA or AMD in any log bundle. A real line, from a Windows
   WARP device in the rig:

       D3D11 adapter: device=000002864EC18470 (#1) description="Microsoft Basic Render Driver" vendor=0x1414 deviceId=0x008C subsys=0x00000000 revision=0 dedicatedVideoMemory=0 MiB dedicatedSystemMemory=0 MiB sharedSystemMemory=15930 MiB luid=00000000:0001610D

2. The self-query, on the first device only: 11 formats (R11G11B10_FLOAT,
   R10G10B10A2_UNORM and _TYPELESS, R16G16B16A16_FLOAT, R8G8B8A8_UNORM, _SRGB
   and _TYPELESS, R32_FLOAT, D32_FLOAT_S8X24_UINT, R32G8X24_TYPELESS,
   R24G8_TYPELESS), each with `CheckFormatSupport` (`support`) and
   `CheckFeatureSupport(FORMAT_SUPPORT2)` (`support2`) in hex and as bit names,
   then the D3D11 options. A failed call prints its HRESULT and no mask. WARP:

       format support (self-query): R11G11B10_FLOAT[26] support=0x02E6F3F3 [BUFFER|IA_VERTEX|TEX1D|TEX2D|TEX3D|CUBE|SHADER_LOAD|SHADER_SAMPLE|MIP|MIP_AUTOGEN|RENDER_TARGET|BLENDABLE|CPU_LOCKABLE|MSAA_RESOLVE|MSAA_RENDER_TARGET|MSAA_LOAD|SHADER_GATHER|TYPED_UAV] support2=0x000006C0 [UAV_TYPED_LOAD|UAV_TYPED_STORE|TILED|SHAREABLE] via-CheckFeatureSupport=same
       device options (self-query): D3D11_OPTIONS hr=0x00000000 [OMLogicOp=1 ... ExtendedResourceSharing=1] D3D11_OPTIONS2 hr=0x00000000 [PSStencilRef=0 TypedUAVLoadAdditionalFormats=1 ROVs=0 ...]

   `via-CheckFeatureSupport` compares the `FORMAT_SUPPORT` query through
   `CheckFeatureSupport` with `CheckFormatSupport`; it says `DIFFERS` and gives
   the other answer if they disagree.

3. One line saying the hooks went on, right after the hook install (or, if
   the table is too short, that they did not). Absent, the hooks were never
   installed, and another line in the log says why (`d3d11 fixes are OFF`,
   `SENTINEL TRIPPED`, `device vtable unusable`). Fixture device pointer:

       format support: CheckFormatSupport (vtable slot 29) and CheckFeatureSupport (slot 33) are hooked on the game's device 000001F2A3B4C5D0; each forwards the real call first and returns its answer untouched. The game's queries follow as "format support (game #N)" lines.

4. The game's own queries, from those two hooks on the device's
   `CheckFormatSupport` and `CheckFeatureSupport` slots (29 and 33). Each
   distinct (query, format, answer) prints once, in the order the game first
   asked, the first 40;
   `#N` is the call's number among all the game's queries, so a gap is a
   repeat. `from=exe+0x...` is the call site in EliteDangerous64.exe, which a
   disassembler can follow to the code that picks the format. Other callers
   print their raw address. EDVR's own queries are not logged.

       format support (game #1): CheckFormatSupport(R11G11B10_FLOAT[26]) -> hr=0x00000000 support=0x... [...] tid=14248 from=exe+0x2D4A1C3
       format support (game #2): CheckFeatureSupport(FORMAT_SUPPORT2, R11G11B10_FLOAT[26]) -> hr=0x00000000 support2=0x... [...] tid=14248 from=exe+0x2D4A2F0

   Other features print their name, size and answer. The closing line comes
   once the hooks have been quiet for 5 s (twice at most):

       format support (game queries): 412 call(s), 77 distinct; 40 logged, 37 suppressed past the 40-line cap; 0 untracked (table full); 0 before the log opened; hooks ran 450 time(s), 38 not reported

   `hooks ran` counts every entry into either hook; `not reported` is what they
   ran for and did not log (EDVR's own queries, another device's). If the
   hooks are never entered, the line still comes, once, after a minute of
   frames, with `hooks ran 0 time(s)`. So an empty game section has three
   readings, and the lines above tell them apart: no install line (never
   hooked), `hooks ran 0` (hooked, never reached: the game's calls go through
   another table or device), or `hooks ran N, N not reported` (reached, but
   not as the game's own device).

**Pass-through.** The two hooks forward the real call first, unguarded, with
the caller's arguments, and return its HRESULT on every path. The log is read
from that result afterwards, inside a fault budget, so a report that faults
costs the line and not the answer. No lock, no allocation; safe on any thread
and before the log is open (those calls are counted, not lost).

**Tested.** `tools\format_support_test` (in build.bat's rig gate): the decode
against the SDK's enumerators, the vtable slots against `ID3D11DeviceVtbl`,
the ledger from eight threads, every line's exact text, and, in a child
process, the hook bodies on a WARP device: 119 formats three ways and 26
features at 7 sizes, compared with and without the hook, from four threads,
with the log checked line by line; and in another, a session whose hooks are
never entered, which must stay silent for 59 ticks and say `hooks ran 0` on
the 60th.

## 2026-09-30: the format theory is ruled out

ruled out: "DXMT offers less for R11G11B10_FLOAT, so Elite renders its world in
R10G10B10A2", because DXMT reports full R11G11B10_FLOAT support, the scene HDR
target is R11G11B10_FLOAT on the Mac as on Windows, and the format-23 target
was the G-buffer.

The instrument flew twice (`edvr_gfx_20260930_144001.log` and `..._144103.log`;
v0.18.0-rc.4-68-g84c8d555, CrossOver with DXMT, Apple M4 Max). Three reasons,
from the 144103 log:

(a) The self-query row: full support, typed UAV load and store included:

       format support (self-query): R11G11B10_FLOAT[26] support=0x02F6F3F3 [... RENDER_TARGET|BLENDABLE|... TYPED_UAV] support2=0x000003C0 [UAV_TYPED_LOAD|UAV_TYPED_STORE|OM_LOGIC_OP|TILED]

(b) The scene HDR target is R11G11B10_FLOAT at 5120x1440: `flat unknown
projection capture: ... fmt=26 size=5120x1440`, and `flat hdr route: first
trigger ... reads the scene HDR 5120x1440 at t0; ... selection: the route could
resolve this frame`.

(c) The format-23 target of the 123414 log (the `bank=world` contract entries,
R10G10B10A2 with depth) is the G-buffer. Section 82's census retake in
`design-flat-temporal-aa-2026-09-23.md` has Windows drawing the world into R2,
R10G10B10A2, too. The earlier comparison set a G-buffer against the HDR target.

The adapter line, where DXMT reports NVIDIA's vendor id:

       D3D11 adapter: ... description="Apple M4 Max" vendor=0x10DE deviceId=0x0000 ... dedicatedVideoMemory=55050 MiB

The game's queries: 352 calls, 140 distinct, the hooks ran 352 times, 0 not
reported.

## 2026-09-30: the game exits at the first treatment

On v0.18.0-rc.4-68-g84c8d555 the game exits about 30 s in, in both logged
sessions: 144001 with DLSS (NGX initialised on the M4 Max) and 144103 with FSR
(the fsr3 DX11 backend initialised).

The breadcrumbs end with `gfx: process exit` and there is no `UNHANDLED`
record, so no fault went through EDVR's filter; something ends the process.
DllMain writes that crumb only during process shutdown (`reserved != NULL`,
`d3d11_proxy.cpp`), which the source reads as the game exiting. The last log
lines, in both:

       engine motion: on-foot source slot target created 5120x1440 R32G32
       engine motion: substitution starts at present frame N ... the first this session
       flat hdr route: declined at frame=N seq=...: engine-source-not-ready

The frame before, `flat hdr route: first trigger ... selection: the route
could resolve this frame` was declined for the same reason. The exit falls at
the first frame EDVR would treat on DXMT, and rc.4 never treated a Mac frame,
so no treatment path had run on DXMT before. No `dlss: the feature ... created
for the flat HDR route` line and no FSR context line follow the trigger; the
gfx log may not flush its last lines on exit.

Hypotheses, none tested:

1. Leading: the first treatment's path under DXMT (the HDR route's resolve,
   the prep and finish shaders, or the backend with HDR flags), common to DLSS
   and FSR.
2. Engine motion's substitution under DXMT since rc.4. Two changes are new:
   13b1b8ee keeps the substitution bound across producer draws and puts the
   game's state back once (the lazy restore), and 515af954 answers the
   per-draw Get* questions from the runtime's own record (the query cut).
3. A DXMT fatal error that exits the process through Wine without an
   exception.

With `temporal_aa_before_post = off` the copy route stands alone and the HDR
route never treats. CrossOver's debug log is there for hypothesis 3.

## 2026-09-30: the key-off run, the HDR route's first treatment is the trigger

Sean ran the Mac with `temporal_aa_before_post = off` under `[experimental]`
(bundle `edvr-logs-20260930-144837.zip`, log `edvr_gfx_20260930_144722.log`,
v0.18.0-rc.4-68-g84c8d555, DLSS on). No exit: the session ran about 70 s and
ended with his own quit, `gfx: process exit` after two `alive` crumbs.

1. The copy route refuses every Mac frame: `no-observed-hdr-writes`, then
   `no-known-tone-pass`. treated=0 all session. The stand-down entered, and
   the F8 warning named Bloom:

       flat stand-down: entered at frame=3736: every frame for 5.0 s (265 frames) was refused for no-known-tone-pass, none treated
       flat settings warning: shown (mode=DLSS, ...): DLSS is not active: Elite's post-processing is not recognised. Turn off in Elite's graphics options: Bloom

2. The HDR route, observing with the key off, would take every frame. On the
   Mac it is the only path to AA:

       flat hdr route 5s: key=off state=observing frames=130 hdr-frames=130 trigger=130 ... last=selected last-trigger=VS=DEF19B035D5EDEDC PS=DED8796049C7BB4A target=5120x1440 ...

3. ruled out: hypothesis 2, engine motion's substitution under DXMT, because
   it ran in this session with no exit. `engine motion: substitution starts at
   present frame 1737` (14:47:49.967) and again at frame 3473 (14:48:19.779),
   each with `flat overlay guard active`. The first is about 28 s after the log
   opened, near where the key-on sessions ended.
4. Hypothesis 1 is confirmed as the trigger: the HDR route's first treatment
   ends the process, with DLSS and with FSR. The same build and backend ran
   70 s with the route off and exited at about 30 s with it on. Hypothesis 3,
   a DXMT fatal error through Wine, remains a possible mechanism of it.

Decision (Sean, 2026-09-30): no DXMT guard that turns the route off on Apple
adapters ("we'll fix crossover properly"). The route gets fixed on DXMT.

Next: a build with crash-safe breadcrumbs before and after each step of the HDR
route's first 3 treated frames (branch `claude/hdr-route-crumbs`, in progress).
Then one Mac run with the key on `auto`, DLSS on, and CrossOver's debug log if
possible. The last crumb names the failing step.

## 2026-09-30: the crumbs name the step, and it is DXMT's SwapDeviceContextState

Sean ran the crumbs build on the Mac: v0.18.0-rc.4-69-ge44db9c2 (build
6ABD82CA, linked 2026-09-30 21:44 UTC), CrossOver with DXMT, adapter "Apple M4
Max" (vendor=0x10DE), key `auto`, DLSS. The session ended at the route's first
treatment, as before, and this time the trail says where. Evidence: the bundle
`edvr-logs-20260930-163519.zip` (`edvr_gfx_20260930_163450.log`,
`edvr_breadcrumbs.txt`) and CrossOver's own log, `Steam 2.cxlog`. The two share
one clock: the breadcrumb stamps are the tick count in milliseconds, CrossOver's
are seconds of the same counter.

1. The trail. Frames 1662 and 1663 were admitted and declined
   (`engine-source-not-ready`). Frame 1664 reached the resolver, every creation
   returned hr=0, and the trail ends one step after them:

       3560926280 gfx: hdr-treat 1/3 reached frame=1664 backend=dlss step=resolve
       3560926282 gfx: hdr-treat 1/3 create-texture end hr=0x00000000 srv=0x00000000 uav=0x00000000 srgb=0x8000000A
       3560926282 gfx: hdr-treat 1/3 create-rtv begin over H fmt=R11G11B10_FLOAT(26) size=5120x1440
       3560926282 gfx: hdr-treat 1/3 create-rtv end hr=0x00000000
       3560926283 gfx: hdr-treat 1/3 capture-state begin
       3560926290 gfx: process exit

   There is no `capture-state end`. `capture-state` is the resolver's `Isolate`
   constructor: `SwapDeviceContextState`, then `ClearState`. The gfx log's own
   last line is frame 1664's route line, NGX having initialised 52 ms
   earlier (`dlaa: NGX initialised in 52 ms`).

2. CrossOver's log, on the render thread (07a8) that made the crumbs. The first
   line has no time stamp: DXMT writes it to stderr.

       452745  err:   ../../dxmt/src/d3d11/d3d11_context_impl.cpp:SwapDeviceContextState is not implemented.
       452746  3560926.283:0794:07a8:trace:seh:raise (22)
       452747  3560926.284:0794:07a8:trace:module:LdrShutdownProcess ()
       452748  3560926.284:0794:07a8:trace:module:MODULE_InitDLL (00006FFFE3AC0000 L"nvngx.dll",PROCESS_DETACH,0000000000000001) - CALL

   `raise (22)` is SIGABRT, msvcrt's `abort()`, raised before any SEH filter
   could see anything: that is why there was no `UNHANDLED` record. `gfx:
   process exit` is DllMain's shutdown crumb, 7 ms later.

3. Conclusion: DXMT aborts inside `SwapDeviceContextState`. DXMT's source
   (github.com/3Shain/dxmt main, read 2026-09-30, `src/d3d11/d3d11_context_impl.cpp`)
   has `SwapDeviceContextState(...) override { UNIMPLEMENTED("SwapDeviceContextState"); }`,
   and `UNIMPLEMENTED` (`d3d11_private.h`) logs the place and calls `abort()`.
   Main words the message differently from CrossOver's (`file:function: "text".`,
   not `file:function is not implemented.`), so CrossOver ships another revision
   of DXMT than main; the function and the abort are the same.
   `CreateDeviceContextState` hands back a stub (`MTLD3D11DeviceContextState`,
   "TODO: implement it properly"), which is why the creation at the first
   preflight had returned S_OK and the swap was the first sign of trouble. Only
   `flat_mono_resolve.cpp` calls either in `src\`; the VR world route (section
   82) reaches the swap through the same resolver.

4. The fix (branch `claude/hdr-route-crumbs`). On a DXMT device the resolver
   isolates the game's state by an explicit capture instead; on every other
   device the swap runs exactly as before (the two `SwapDeviceContextState`
   calls, in `Isolate`, are untouched, and source pins hold them so).
   - `flat_context_state.h`: Get calls out, `ClearState`, the work, `ClearState`,
     Set calls back, every reference released. The resolver starts from the
     defaults as it does after a swap, because the swap path also ClearStates
     after it. Covered, stage by stage:
       IA    layout, topology, vertex buffers 0..31 with stride and offset (0..15
             on DXMT, whose table holds 16), index buffer with format and offset
       VS HS DS GS PS CS, each: shader; shader resources 0..127; constant
             buffers 0..13 with their D3D11.1 first-constant and count; samplers 0..15
       CS    UAVs 0..63 (0..7 below feature level 11_1)
       SO    targets 0..3 (restored at offset 0; the offset is not readable)
       OM    render targets 0..7, depth view, OM UAVs (same range as CS), blend
             state with factor and mask, depth-stencil state with reference
       RS    state, viewports 0..15, scissors 0..15
       and   predication (predicate and value)
     The ranges are D3D11's, cut where DXMT's binding tables end
     (`dxmt_binding_set.hpp`, `d3d11_context_state.hpp`). Not captured: class
     instances (ClearState resets them; DXMT does not support them; Elite binds none).
   - Every call was read in main's `d3d11_context_impl.cpp`; none is
     UNIMPLEMENTED (CrossOver's revision is not main, see 3). Three DXMT habits
     shaped the restore: the array arguments of the Set calls are read before the
     count, so none is ever null; `Get...ConstantBuffers1` leaves the window of an
     unbound slot alone, so the capture pre-fills it; and after `ClearState`
     DXMT's blend factor is 0, not D3D11's 1, so blend, depth-stencil and
     rasterizer state are always set from the game's own values.
   - Detection (`flat_context_isolation.h`), four markers asked together, each named in
     the log: the device answers `IMTLD3D11DeviceExt` {efc77ae6-2179-4c0a-b844-7661ca0dcde7};
     the context answers `IMTLD3D11ContextExt` {43ace3ce-1956-448b-a4eb-aee68bdeb283}
     (both defined in DXMT's `d3d11_interfaces.hpp`, answered in `d3d11_device.cpp` and
     `d3d11_context_impl.cpp`, and the context's IID is the one DXMT's nvngx shim
     reaches TemporalUpscale through); the module holding the device's QueryInterface
     has a version resource with ProductName DXMT (`src\d3d11\version.rc`); the
     adapter description begins "Apple", which alone is only the fallback.
   - One log line per renderer initialisation: `flat resolver: context isolation by
     explicit state capture (DXMT: ...)` or `... by context state swap (no DXMT marker)`.
   - Crumbs: `capture-state` and `restore-state` now carry `by=swap|capture`; the capture
     writes `capture-ia`, `capture-vs` ... `capture-cs`, `capture-so`, `capture-om`,
     `capture-rs`, `capture-predication` and the restore the same, for the session's first
     capture and first restore only (44 lines). The capture's end lines say what the game
     had bound. The session budget went from 128 to 192 crumbs to hold them.
   - `advanced.flat_context_isolation = auto|swap|capture` (hidden, needs a restart),
     read once by the flat runtime. `swap` on DXMT ends the game; `capture` on
     Windows works.

5. The rig (`tools\flat_mono_resolve_test`, WARP), what it proves:
   - An oracle of nothing but the context's own Get calls reads 1,291 values
     (feature level 11_0, the rig's device) or 1,403 (a second device at 11_1)
     and every fixture object's reference count. The game's state fills every
     range; the block captures; the context is at its defaults after
     `ClearState`; a stand-in backend dirties every stage and slot with other
     objects and runs a dispatch; `ClearState`; the block restores. All values and
     every count are what they were, at both feature levels. The D3D debug layer
     (Graphics Tools) is not installed on the build machine, so the calls were held
     to the oracle and to WARP's results (a call the runtime refuses leaves its slot
     unbound and the snapshot no longer matches), not to the layer's validation.
   - The oracle is held to its word: 54 single-slot changes, one in each stage and
     kind of slot, are each seen at their own label; a leaked reference is seen; a
     context cleared and not restored differs; a capture with ranges cut short
     leaves exactly the far slots behind.
   - The resolver in both modes, on the copy route, the HDR route, a refused backend
     and both spatial recoveries, with the backend dirtying every stage: the game's
     whole state and counts come back. On the default route: mode `swap`, the swap
     counted and no explicit capture, `by=swap` and none of the eleven groups, the
     same state back. Forced `capture`: `by=capture`, the eleven groups once.
   - The decision: the key's text, each marker on a stand-in object, the version
     scanner on a real Windows blob patched to say DXMT, the choice and the log
     line for every case. The real WARP device answers none, so auto is swap.

6. Not known, because no rig can run DXMT: how DXMT reacts to this sequence beyond
   what main's source says, whether every call used here is implemented in
   CrossOver's revision (the eleven capture groups name the first that is not),
   and what comes after the swap (the prep dispatch, the DLSS evaluate, the
   finish draw). The CrossOver log's environment block has `DXMT_ENABLE_NVEXT=1`,
   `D3DM_ENABLE_METALFX=1` and `CX_GRAPHICS_BACKEND=dxmt`.

## 2026-09-30: the fix flew on the Mac, and the crumbs go DXMT-only

Sean ran the fix build on the Mac: v0.18.0-rc.4-114-g20031385 (build 6ABD9EAC,
linked 2026-09-30 23:43 UTC), CrossOver with DXMT, adapter "Apple M4 Max", key
`auto`. Evidence: the bundle `edvr-logs-20260930-175527.zip`
(`edvr_gfx_20260930_175233.log`, `edvr_breadcrumbs.txt`) and CrossOver's log,
`Steam 3.cxlog`. The log runs 17:52:33 to 17:55:18. The game was still running
when the bundle was taken: the last `alive` crumb is frame 8935 and there is no
`process exit`.

1. The isolation line, at the first resolver initialisation (17:52:59.971, and
   again at 17:53:13.385 when a mode switch rebuilt the renderer):

       flat resolver: context isolation by explicit state capture (DXMT: device interface IMTLD3D11DeviceExt, context interface IMTLD3D11ContextExt, module version resource ProductName=DXMT, adapter name "Apple M4 Max")

   All four markers answered on CrossOver's DXMT, so its private interfaces
   and version resource are the ones DXMT main has, and the adapter-name
   fallback was not needed.

2. The first treated frame, in the crumbs (frame 1653; frames 1651 and 1652 had
   been declined for `engine-source-not-ready`). Every step ran and balanced,
   and so did the next two frames (2/3 and 3/3, reset=0). What Elite had bound
   at that draw, from the capture's end lines:

       capture-ia end layout=1 vb=1 ib=0 topology=5
       capture-vs end shader=1 srv=0 cb=3 sampler=1
       capture-hs, capture-ds, capture-gs end shader=0 srv=0 cb=0 sampler=0
       capture-ps end shader=1 srv=1 cb=2 sampler=3
       capture-cs end shader=0 srv=0 cb=3 sampler=2 uav=0
       capture-so end targets=0
       capture-om end rtv=1 dsv=0 uav=0 blend=1 depth-stencil=1
       capture-rs end state=1 viewports=1 scissors=1
       capture-predication end predicate=0 value=0

   A triangle-strip draw into one target, with a small state. The flight read
   every group but set back only the ones with something in them: DXMT's Set
   paths for hull, domain and geometry shaders, stream output, UAVs, the index
   buffer, a depth view and predication have not run with objects in them on the
   Mac (the rig has run them on WARP). Then, in order: `backend-available`
   ok=1, `copy-h`, `prep groups=640x180`, `backend-query`, `backend-create ngx
   feature in=5120x1440 out=5120x1440 quality=5 flags=0x4B`, `backend-evaluate`
   (685 ms the first time, which holds the feature's first run; 0 to 1 ms on
   the next two), `finish-bind`, `finish-draw`, the eleven `restore-` groups,
   `before-present`, `present end hr=0x00000000 removed=0x00000000`, `frame-end`.
   The capture took 4 ms the first time and under 1 ms after.

3. DLSS and FSR, both:

       17:53:00.023 dlaa: NGX initialised in 50 ms on device 000000000080B8C8.
       17:53:00.053 dlss: the feature for eye 0 was created for the flat HDR route: HDR input and automatic exposure (IsHDR | AutoExposure with MVLowRes | DepthInverted); the history starts here.
       17:53:05.582 menu: fix.temporal_aa dlss -> fsr (written to edvr-flat.ini; live).
       17:53:05.616 fsr3: first asked for on Apple M4 Max, AMD's port version fsr 3.1.2.
       17:53:05.628 fsr3: the context is created for eye 0 at 5120x1440 -> 5120x1440, HDR input with automatic exposure (the flat HDR route); its three working surfaces take 84.4 MB ...; the history starts here (made in 11 ms).

   DLSS runs through CrossOver's nvngx redirect: `Steam 3.cxlog` has three
   `redirect_nvngx_to_d3dmetal HACK: redirecting nvngx.dll to D3DMetal original`
   lines, the first 71 ms before the capture of the first treated frame. Sean
   then switched modes in the menu, from dlss to fsr, back to dlss, on, off, on,
   dlss, fsr and dlss again (17:53:05 to 17:53:17), and the process went on.

4. The route's 5 s windows. 17:53:03 to 17:53:18 are the mode switches
   (frames=186 treated=182 declined=2; 280/282/0; 220/219/2; 288/291/0). Then
   eight windows, 17:53:23 to 17:53:58, each `state=active`, frames=265-300,
   treated equal to frames, declined=0, late-hdr-writes=0, and
   `steps: admitted=reached=captured=copied=prepped=backend=finished=restored`
   equal to treated in every one. `Steam 3.cxlog` has no DXMT "not
   implemented" line and no `trace:seh:raise` at all.

5. By eye, Sean on the Mac with this build: "Black floor outlines went away and
   everything looks aliased appropriately, tested with bloom and DoF on as
   well." Read: the black outlines are gone, the edges are anti-aliased as they
   should be, and it holds with Elite's bloom and depth of field on. On the Mac
   the copy route refuses that chain (`no-known-tone-pass`); the HDR route
   resolves H before bloom and DoF, so they run on the resolved image. This
   closes the open item "Sean confirms by eye that the outlines are gone".

6. After the good minute the route took no frames again. 17:53:59.737 `flat
   runtime early fallback: frame=5088 temporal=no-known-tone-pass`; 17:54:01.728
   engine motion released its on-foot source (`no on-foot source for 120
   frames`, frame 5208); 17:54:04.745 `flat stand-down: entered at frame=5388:
   every frame for 5.0 s (301 frames) was refused for no-known-tone-pass`; the
   windows read frames=3-4 and treated=0 to the end of the log, with 39 probes
   by 17:55:04 (the last one `no-known-tone-pass`). The session's start looked the same
   (stand-down at 17:52:40, `first trigger at frame=1650` at 17:52:59, "resumed
   ... after 19.8 s stood down (13 probes)"). The log does not say what was on
   screen either time. This is the refusal rc.4 already had where Elite's chain
   is not recognised; the fix did not touch it.

7. The crumbs go DXMT-only (this commit). The exit is solved, and the crumbs are
   still the way to see what DXMT does with the next call, so they stay, on DXMT
   and nowhere else. The flat runtime opens a gate once, at the first Present with
   a temporal mode on, from the markers alone (`flatCrumbsWantedFor`); every
   `gfx: hdr-treat` writer, the `armed` line, `admitted`, `reached`, `declined`,
   every span, the frame end, the Present hook's pair and the capture's eleven
   groups, returns before it touches any state while the gate is shut. The
   isolation key is not asked: `advanced.flat_context_isolation=capture` on a
   Windows device runs the capture and writes no crumb, and `swap` on a DXMT
   device keeps them. On Windows `edvr_breadcrumbs.txt` gets none of them, the
   "crash-safe trail on" log line is not written, and the budget never starts.
   The 5 s line's step counts are plain counters and stay everywhere. Pinned in
   `flat_temporal_test` (the gate in each writer, the runtime's one call, no other
   file writing a `gfx: hdr-treat` line, the gate never reading the key; five
   broken gates each fail it) and in `flat_mono_resolve_test` (a preflight, three
   resolves, a refused backend and a recovery write no crumb with the gate shut,
   swap and forced capture alike, while the step counts count them).
